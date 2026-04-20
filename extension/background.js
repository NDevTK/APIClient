// Service worker: intercepts network requests, extracts API keys,
// endpoints, auth headers, coordinates discovery document fetching,
// req2proto fallback probing, and stores AST security findings.

importScripts("lib/protobuf.js", "lib/discovery.js", "lib/req2proto.js", "lib/stats.js", "lib/chains.js");

// ─── AST Cache Version ──────────────────────────────────────────────────────
// Bump this when AST analysis logic changes to auto-invalidate cached results.
const AST_ANALYSIS_VERSION = 1;

// ─── Offscreen AST Worker ────────────────────────────────────────────────────
// Heavy libs (babel-bundle.js, ast.js, sourcemap.js) run in an offscreen
// document so the service worker stays responsive during analysis.

var _offscreenReady = null;

async function ensureOffscreen() {
  if (_offscreenReady) return _offscreenReady;
  _offscreenReady = (async () => {
    var contexts = await chrome.runtime.getContexts({
      contextTypes: ["OFFSCREEN_DOCUMENT"]
    });
    if (contexts.length > 0) return;
    await chrome.offscreen.createDocument({
      url: "ast-worker.html",
      reasons: ["WORKERS"],
      justification: "AST analysis of JavaScript bundles"
    });
  })();
  return _offscreenReady;
}

async function sendToOffscreen(msg) {
  await ensureOffscreen();
  return chrome.runtime.sendMessage(msg);
}

// Inlined from ast.js — extracts sourceMappingURL from the last 500 chars.
// Runs synchronously in the service worker (no Babel needed).
function extractSourceMapUrl(code) {
  var tail = code.length > 500 ? code.slice(-500) : code;
  var marker = "sourceMappingURL=";
  var idx = tail.indexOf(marker);
  if (idx === -1) return null;
  var start = idx + marker.length;
  while (start < tail.length && (tail.charCodeAt(start) === 32 || tail.charCodeAt(start) === 9)) start++;
  var end = start;
  while (end < tail.length && tail.charCodeAt(end) > 32) end++;
  return end > start ? tail.substring(start, end) : null;
}

// ─── State ───────────────────────────────────────────────────────────────────

const state = {
  // Map<tabId, TabData>
  tabs: new Map(),
};

// Maximum entries retained per tab in the live request log. When exceeded,
// the oldest entries are evicted. Complex sites (LinkedIn, Booking, Figma,
// Vercel, Discord, Spotify) saturated the previous 50-entry cap in under a
// minute of browsing, silently dropping real traffic. 500 keeps per-tab
// memory bounded while leaving enough headroom to capture a real session.
// The per-service schemas still persist to globalStore independent of this
// cap, so evicted log entries don't undo learning — only the request-log
// replay view loses them.
const MAX_REQUEST_LOG_ENTRIES = 500;
function _trimRequestLog(tab) {
  while (tab.requestLog.length > MAX_REQUEST_LOG_ENTRIES) tab.requestLog.pop();
}

// Session storage for request logs — survives SW restarts, clears on browser close
const _sessionSaveTimers = new Map(); // tabId → timeoutId
const _tabMeta = new Map(); // tabId → { title, url, closed? }
const _tabFrames = new Map(); // tabId → Map<frameId, { url, origin, isTop, lastSeen }>
const _wsConnState = new Map(); // tabId → Map<wsId, { url, readyState }>

// ─── Frame tracking via webNavigation ────────────────────────────────────────
// Replaces content-script FRAME_REGISTER — authoritative frame data from the
// browser process, not from potentially cross-origin iframe JS contexts.
chrome.webNavigation.onCommitted.addListener(function(details) {
  var tabId = details.tabId;
  var frameId = details.frameId;
  if (!_tabFrames.has(tabId)) _tabFrames.set(tabId, new Map());
  var frMap = _tabFrames.get(tabId);
  var url = details.url || "";
  var origin = "";
  try { origin = new URL(url).origin; } catch (_) {}
  var isTop = details.parentFrameId === -1;
  frMap.set(frameId, { url: url, origin: origin, isTop: isTop, lastSeen: Date.now() });
  // Update _tabMeta for top frames
  if (isTop && url) {
    var tm = _tabMeta.get(tabId);
    if (!tm) {
      _tabMeta.set(tabId, { title: "Tab " + tabId, url: url });
    } else {
      tm.url = url;
    }
  }
  notifyPopup(tabId);
});

// Cross-script AST analysis: buffer scripts per tab, debounce, concatenate + analyze
const _scriptBuffers = new Map(); // tabId → { scripts: [{url, code}], timer: null }

// Global persistent store — survives tab closes and SW restarts
const globalStore = {
  apiKeys: new Map(), // key → { origin, referer, firstSeen, ... }
  endpoints: new Map(), // endpointKey → endpoint data
  discoveryDocs: new Map(), // service → { status, url, method, apiKey, fetchedAt, doc }
  probeResults: new Map(), // endpointKey → probe result
  scopes: new Map(), // service → string[]
  securityFindings: new Map(), // sourceUrl → { sourceUrl, securitySinks[], dangerousPatterns[] }
  scriptCache: new Map(), // SHA-256 hash → { version, result, timestamp }
  discoveryChanges: new Map(), // service → [{ timestamp, fetchUrl, changes }]
};

// In-flight exploit-probe sessions, keyed by marker. The EXPLOIT_PROBE
// handler registers a session before opening the target tab, intercept.js
// (running in that tab) forwards matching sink hits via content.js →
// PROBE_HIT, and the handler reads the accumulated hits after its
// observation window. Non-persistent: sessions outlive only their own
// observation window (seconds), so loss-on-SW-restart is acceptable.
const _probeSessions = new Map();

// ─── Key Extraction ──────────────────────────────────────────────────────────

const KEY_PATTERNS = [
  { name: "Google API Key", re: /AIzaSy[\w-]{33}/g },
  { name: "Firebase Key", re: /AIza[0-9A-Za-z-_]{35}/g },
  { name: "Bearer Token", re: /bearer\s+([a-zA-Z0-9-._~+/]+=*)/gi },
  {
    name: "Generic API Key",
    re: /(?:api[-_]?key|access[-_]?token|auth[-_]?token)['"]?\s*[:=]\s*['"]?([a-zA-Z0-9\-_]{16,})['"]?/gi,
  },
  { name: "JWT", re: /ey[a-zA-Z0-9-_]+\.ey[a-zA-Z0-9-_]+\.[a-zA-Z0-9-_]+/g },
  { name: "Mapbox Token", re: /pk\.[a-zA-Z0-9.]+/g },
  { name: "GitHub Token", re: /ghp_[a-zA-Z0-9]{36}/g },
  { name: "Stripe Key", re: /[sk|pk]_(?:test|live)_[0-9a-zA-Z]{24}/g },
];

// ─── batchexecute Decoding ──────────────────────────────────────────────────

function extractKeysFromText(tabId, text, sourceUrl, sourceContext) {
  if (!text) return;
  const tab = getTab(tabId);
  const url = sourceUrl ? new URL(sourceUrl) : null;
  const service = url ? extractInterfaceName(url) : "unknown";

  // Iterative BFS over nested base64 payloads. `visited` dedupes both the
  // input text and every decoded printable string, so a cycle (same bytes
  // reappearing at a deeper level) terminates without dropping new data.
  const visited = new Set();
  const queue = [{ text, context: sourceContext || "network" }];
  const B64_RE = /[a-zA-Z0-9+/]{20,2000}=*/g;

  while (queue.length > 0) {
    const { text: currentText, context } = queue.shift();
    if (visited.has(currentText)) continue;
    visited.add(currentText);

    // 1. Scan for direct key matches
    for (const pattern of KEY_PATTERNS) {
      pattern.re.lastIndex = 0;
      let m;
      while ((m = pattern.re.exec(currentText)) !== null) {
        const key = m[1] || m[0];
        if (key.length < 10) continue;

        if (!tab.apiKeys.has(key)) {
          tab.apiKeys.set(key, {
            name: pattern.name,
            origin: url ? url.origin : null,
            referer: url ? url.href : null,
            source: context,
            firstSeen: Date.now(),
            lastSeen: Date.now(),
            services: new Set(),
            hosts: new Set(),
            endpoints: new Set(),
            pageUrls: new Set(),
            requestCount: 0,
          });
        }

        const keyData = tab.apiKeys.get(key);
        keyData.lastSeen = Date.now();
        if (url) {
          keyData.services.add(service);
          keyData.hosts.add(url.hostname);
          keyData.endpoints.add(`${url.hostname}${url.pathname}`);
        }
        const _keyMeta = _tabMeta.get(tabId);
        if (_keyMeta && _keyMeta.url) keyData.pageUrls.add(_keyMeta.url);
        if (!keyData.pageUrls) keyData.pageUrls = new Set();
      }
    }

    // 2. Scan for base64 blobs that might contain hidden keys.
    // Cap at 2000 chars to avoid decoding huge binary blobs (images, protobuf
    // payloads). Limit to first 50 matches per text to bound CPU time.
    B64_RE.lastIndex = 0;
    let b64m;
    let b64Count = 0;
    while ((b64m = B64_RE.exec(currentText)) !== null && b64Count < 50) {
      b64Count++;
      const candidate = b64m[0];
      try {
        if (tab.apiKeys.has(candidate)) continue;

        const padded =
          candidate.length % 4 === 0
            ? candidate
            : candidate + "=".repeat(4 - (candidate.length % 4));
        const decoded = atob(padded);

        // Filter out non-printable garbage to avoid regex hangs
        const printable = decoded.replace(/[^\x20-\x7E\t\n\r]/g, "");
        if (printable.length > 10 && !visited.has(printable)) {
          queue.push({ text: printable, context: context + " > b64" });
        }
      } catch (e) {
        // Not valid base64, ignore
      }
    }
  }
}

function getTab(tabId) {
  if (!state.tabs.has(tabId)) {
    state.tabs.set(tabId, {
      apiKeys: new Map(), // key → { origin, referer, firstSeen }
      endpoints: new Map(), // endpointKey → { method, service, key, headers, firstSeen }
      authContext: null, // { sapisid, sapisidhash, cookies }
      discoveryDocs: new Map(), // service → discovery JSON or status
      probeResults: new Map(), // endpointKey → probe result
      scopes: new Map(), // service → string[] of required scopes
      requestLog: [], // Array of { id, url, method, service, timestamp, status, headers, responseHeaders, ... }
      _valueIndex: createValueIndex(), // Chain engine: response value → source tracking
    });
    captureTabMeta(tabId);
  }
  return state.tabs.get(tabId);
}

async function captureTabMeta(tabId) {
  if (_tabMeta.has(tabId)) return;
  try {
    const tab = await chrome.tabs.get(tabId);
    if (tab) {
      _tabMeta.set(tabId, { title: tab.title || `Tab ${tabId}`, url: tab.url || "" });
    }
  } catch (_) {
    _tabMeta.set(tabId, { title: `Tab ${tabId}`, url: "" });
  }
}

// ─── Persistent Storage (IndexedDB) ─────────────────────────────────────────

const _IDB_NAME = "uasr_store";
const _IDB_VERSION = 1;
const _IDB_STORE = "global";

function _openIDB() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(_IDB_NAME, _IDB_VERSION);
    req.onupgradeneeded = (e) => {
      const db = e.target.result;
      if (!db.objectStoreNames.contains(_IDB_STORE)) {
        db.createObjectStore(_IDB_STORE);
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function _idbGet(key) {
  return _openIDB().then(
    (db) =>
      new Promise((resolve, reject) => {
        const tx = db.transaction(_IDB_STORE, "readonly");
        const store = tx.objectStore(_IDB_STORE);
        const req = store.get(key);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
        tx.oncomplete = () => db.close();
      }),
  );
}

function _idbSet(key, value) {
  return _openIDB().then(
    (db) =>
      new Promise((resolve, reject) => {
        const tx = db.transaction(_IDB_STORE, "readwrite");
        const store = tx.objectStore(_IDB_STORE);
        store.put(value, key);
        tx.oncomplete = () => {
          db.close();
          resolve();
        };
        tx.onerror = () => {
          db.close();
          reject(tx.error);
        };
      }),
  );
}

function _idbClear() {
  return _openIDB().then(
    (db) =>
      new Promise((resolve, reject) => {
        const tx = db.transaction(_IDB_STORE, "readwrite");
        const store = tx.objectStore(_IDB_STORE);
        store.clear();
        tx.oncomplete = () => {
          db.close();
          resolve();
        };
        tx.onerror = () => {
          db.close();
          reject(tx.error);
        };
      }),
  );
}

let _saveTimer = null;

function scheduleSave() {
  if (_saveTimer) clearTimeout(_saveTimer);
  _saveTimer = setTimeout(saveGlobalStore, 2000);
}

function _serializeGlobalStore() {
  return {
    apiKeys: Object.fromEntries(
      [...globalStore.apiKeys].map(([k, v]) => [
        k,
        {
          origin: v.origin,
          referer: v.referer,
          source: v.source,
          firstSeen: v.firstSeen,
          lastSeen: v.lastSeen,
          requestCount: v.requestCount || 0,
          services: [
            ...(v.services instanceof Set ? v.services : v.services || []),
          ],
          hosts: [...(v.hosts instanceof Set ? v.hosts : v.hosts || [])],
          endpoints: [
            ...(v.endpoints instanceof Set ? v.endpoints : v.endpoints || []),
          ],
          pageUrls: [
            ...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || []),
          ],
        },
      ]),
    ),
    endpoints: Object.fromEntries(globalStore.endpoints),
    discoveryDocs: Object.fromEntries(
      [...globalStore.discoveryDocs].map(([k, v]) => [
        k,
        {
          status: v.status,
          url: v.url || null,
          method: v.method || null,
          apiKey: v.apiKey || null,
          fetchedAt: v.fetchedAt || null,
          doc: v.doc || null,
          grouping: v.grouping || null,
          isVirtual: !!v.isVirtual,
          pageUrls: [...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || [])],
          frameOrigins: [...(v.frameOrigins instanceof Set ? v.frameOrigins : v.frameOrigins || [])],
        },
      ]),
    ),
    probeResults: Object.fromEntries(globalStore.probeResults),
    scopes: Object.fromEntries(globalStore.scopes),
    securityFindings: Object.fromEntries(globalStore.securityFindings),
    scriptCache: Object.fromEntries(globalStore.scriptCache),
    discoveryChanges: Object.fromEntries(globalStore.discoveryChanges),
    savedAt: Date.now(),
  };
}

function _deserializeIntoGlobalStore(s) {
  if (s.apiKeys) {
    for (const [k, v] of Object.entries(s.apiKeys)) {
      globalStore.apiKeys.set(k, {
        ...v,
        services: new Set(v.services || []),
        hosts: new Set(v.hosts || []),
        endpoints: new Set(v.endpoints || []),
        pageUrls: new Set(v.pageUrls || []),
      });
    }
  }
  if (s.endpoints) {
    for (const [k, v] of Object.entries(s.endpoints))
      globalStore.endpoints.set(k, v);
  }
  if (s.discoveryDocs) {
    for (const [k, v] of Object.entries(s.discoveryDocs)) {
      globalStore.discoveryDocs.set(k, {
        ...v,
        pageUrls: new Set(v.pageUrls || []),
        frameOrigins: new Set(v.frameOrigins || []),
      });
    }
  }
  if (s.probeResults) {
    for (const [k, v] of Object.entries(s.probeResults))
      globalStore.probeResults.set(k, v);
  }
  if (s.scopes) {
    for (const [k, v] of Object.entries(s.scopes))
      globalStore.scopes.set(k, v);
  }
  if (s.securityFindings) {
    for (const [k, v] of Object.entries(s.securityFindings))
      globalStore.securityFindings.set(k, v);
  }
  if (s.scriptCache) {
    var TTL = 30 * 24 * 60 * 60 * 1000; // 30 days
    var now = Date.now();
    var entries = Object.entries(s.scriptCache);
    // Evict expired entries
    entries = entries.filter(function([_, v]) { return now - v.timestamp < TTL; });
    // Cap at 500 entries (LRU by timestamp)
    if (entries.length > 500) {
      entries.sort(function(a, b) { return b[1].timestamp - a[1].timestamp; });
      entries = entries.slice(0, 500);
    }
    for (const [k, v] of entries)
      globalStore.scriptCache.set(k, v);
  }
  if (s.discoveryChanges) {
    for (const [k, v] of Object.entries(s.discoveryChanges))
      globalStore.discoveryChanges.set(k, v);
  }
}

async function saveGlobalStore() {
  _saveTimer = null;
  try {
    await _idbSet("gapiStore", _serializeGlobalStore());
  } catch (_) {
    console.error("[Storage] Save failed:", _);
  }
}

async function loadGlobalStore() {
  try {
    // Migrate from chrome.storage.local if data exists there (one-time)
    const legacy = await chrome.storage.local.get("gapiStore");
    if (legacy.gapiStore) {
      _deserializeIntoGlobalStore(legacy.gapiStore);
      await _idbSet("gapiStore", _serializeGlobalStore());
      await chrome.storage.local.remove("gapiStore");
      return;
    }
    // Normal load from IndexedDB
    const s = await _idbGet("gapiStore");
    if (s) _deserializeIntoGlobalStore(s);
  } catch (_) {
    console.error("[Storage] Load failed:", _);
  }
}

function mergeToGlobal(tab) {
  for (const [k, v] of tab.apiKeys) {
    const existing = globalStore.apiKeys.get(k);
    if (existing) {
      existing.lastSeen = v.lastSeen;
      // Take the higher count — tab count is a running total, not a delta
      existing.requestCount = Math.max(
        existing.requestCount || 0,
        v.requestCount || 0,
      );
      const mergeSet = (target, source) => {
        if (source instanceof Set)
          source.forEach((s) => (target instanceof Set ? target.add(s) : null));
        else if (Array.isArray(source))
          source.forEach((s) => (target instanceof Set ? target.add(s) : null));
      };
      if (existing.services instanceof Set)
        mergeSet(existing.services, v.services);
      if (existing.hosts instanceof Set) mergeSet(existing.hosts, v.hosts);
      if (existing.endpoints instanceof Set)
        mergeSet(existing.endpoints, v.endpoints);
      if (!existing.pageUrls) existing.pageUrls = new Set();
      if (existing.pageUrls instanceof Set)
        mergeSet(existing.pageUrls, v.pageUrls);
    } else {
      globalStore.apiKeys.set(k, {
        origin: v.origin,
        referer: v.referer,
        source: v.source,
        firstSeen: v.firstSeen,
        lastSeen: v.lastSeen,
        requestCount: v.requestCount || 0,
        services: new Set(v.services || []),
        hosts: new Set(v.hosts || []),
        endpoints: new Set(v.endpoints || []),
        pageUrls: new Set(v.pageUrls || []),
      });
    }
  }
  for (const [k, v] of tab.endpoints) {
    if (!globalStore.endpoints.has(k)) {
      v._isNew = true;
      v._firstSeenGlobal = Date.now();
    } else {
      var ge = globalStore.endpoints.get(k);
      if (ge.lastSeen) v.lastSeen = Date.now();
      if (ge._firstSeenGlobal) v._firstSeenGlobal = ge._firstSeenGlobal;
      v._isNew = false;
    }
    globalStore.endpoints.set(k, v);
  }
  for (const [k, v] of tab.discoveryDocs) {
    if (v.status === "found") {
      // Merge pageUrls and frameOrigins Sets with existing global entry
      var _existingGDoc = globalStore.discoveryDocs.get(k);
      var _mergedPageUrls = new Set(_existingGDoc?.pageUrls || []);
      if (v.pageUrls) for (var _pu of v.pageUrls) _mergedPageUrls.add(_pu);
      var _mergedFrameOrigins = new Set(_existingGDoc?.frameOrigins || []);
      if (v.frameOrigins) for (var _fo of v.frameOrigins) _mergedFrameOrigins.add(_fo);
      globalStore.discoveryDocs.set(k, {
        status: v.status,
        url: v.url,
        method: v.method,
        apiKey: v.apiKey,
        fetchedAt: v.fetchedAt,
        doc: v.doc || null,
        grouping: v.grouping || null,
        pageUrls: _mergedPageUrls,
        frameOrigins: _mergedFrameOrigins,
        isVirtual: !!v.isVirtual,
      });
    } else if (!globalStore.discoveryDocs.has(k)) {
      globalStore.discoveryDocs.set(k, { status: v.status });
    }
  }
  for (const [k, v] of tab.probeResults) {
    globalStore.probeResults.set(k, v);
  }
  for (const [k, v] of tab.scopes) {
    globalStore.scopes.set(k, v);
  }
  if (tab._securityFindings) {
    for (var sf = 0; sf < tab._securityFindings.length; sf++) {
      var finding = tab._securityFindings[sf];
      globalStore.securityFindings.set(finding.sourceUrl || ("unknown_" + sf), finding);
    }
  }
  scheduleSave();
}

async function clearGlobalStore() {
  globalStore.apiKeys.clear();
  globalStore.endpoints.clear();
  globalStore.discoveryDocs.clear();
  globalStore.probeResults.clear();
  globalStore.scopes.clear();
  globalStore.securityFindings.clear();
  try {
    await _idbClear();
  } catch (_) {
    console.error("[Storage] Clear failed:", _);
  }
}

// Load persisted data on startup — handlers must await this before reading globalStore
const _globalStoreReady = loadGlobalStore();

// ─── Session Storage (Request Logs) ─────────────────────────────────────────

function serializeLogEntry(entry) {
  return { ...entry };
}

function scheduleSessionSave(tabId) {
  if (_sessionSaveTimers.has(tabId)) {
    clearTimeout(_sessionSaveTimers.get(tabId));
  }
  _sessionSaveTimers.set(
    tabId,
    setTimeout(() => {
      _sessionSaveTimers.delete(tabId);
      saveTabSessionLog(tabId);
    }, 1000),
  );
}

async function saveTabSessionLog(tabId) {
  const tab = state.tabs.get(tabId);
  if (!tab) return;
  try {
    const serialized = tab.requestLog.map(serializeLogEntry);
    await chrome.storage.session.set({ [`reqLog_${tabId}`]: serialized });
    await saveSessionIndex();
  } catch (e) {
    console.error("[Session] Save failed for tab", tabId, e);
  }
}

async function saveSessionIndex() {
  const index = {};
  for (const [tabId, meta] of _tabMeta) {
    const tab = state.tabs.get(tabId);
    const count = tab ? tab.requestLog.length : 0;
    if (count > 0 || meta.closed) {
      index[tabId] = { ...meta, count };
    }
  }
  try {
    await chrome.storage.session.set({ reqLog_index: index });
  } catch (e) {
    console.error("[Session] Index save failed:", e);
  }
}

async function loadSessionLogs() {
  try {
    const data = await chrome.storage.session.get(null);
    for (const [key, value] of Object.entries(data)) {
      if (key === "reqLog_index") {
        for (const [tidStr, meta] of Object.entries(value)) {
          const tid = parseInt(tidStr, 10);
          if (!isNaN(tid)) _tabMeta.set(tid, meta);
        }
        continue;
      }
      if (key.startsWith("reqLog_")) {
        const tabId = parseInt(key.slice(7), 10);
        if (isNaN(tabId) || !Array.isArray(value)) continue;
        const tab = getTab(tabId);
        if (tab.requestLog.length === 0) {
          tab.requestLog = value;
          // Restore _wsConnState from persisted WEBSOCKET entries
          for (const entry of value) {
            if (entry.method === "WEBSOCKET" && entry.channelId) {
              if (!_wsConnState.has(tabId)) _wsConnState.set(tabId, new Map());
              // After SW restart, mark all as closed — intercept.js will re-emit WS_OPEN for live ones
              _wsConnState.get(tabId).set(entry.channelId, {
                url: entry.url,
                readyState: entry.wsOpen ? 1 : 3,
                entryId: entry.id,
              });
            }
          }
        }
      }
    }
  } catch (e) {
    console.error("[Session] Load failed:", e);
  }
}

loadSessionLogs();

// ─── Patterns ────────────────────────────────────────────────────────────────

const API_KEY_RE = /AIzaSy[\w-]{33}/g;


// Strip JSONP wrapper: callbackName({"key":"value"}) → '{"key":"value"}'
// Returns the inner JSON string or null if not JSONP.
function stripJsonp(text) {
  var m = /^[a-zA-Z_$][\w$.]*\s*\(\s*/.exec(text);
  if (!m) return null;
  var inner = text.slice(m[0].length);
  // Remove trailing );\s* or )\s*
  var end = inner.lastIndexOf(")");
  if (end === -1) return null;
  inner = inner.slice(0, end).trim();
  // Sanity check: must look like JSON (object or array)
  if (inner.charAt(0) !== "{" && inner.charAt(0) !== "[") return null;
  return inner;
}

// Extract interface name from URL with better granularity
// Service grouping is inherently heuristic — there's no server-side
// fact that tells us "this URL path is service X". The function below
// applies a set of URL-structure rules, each with a named reason and
// the exact fragment it matched, so a reviewer can see WHY a request
// was grouped into a given bucket and judge whether the classification
// is right. `extractInterfaceName` remains a string-returning wrapper
// for back-compat with existing callers.
function classifyInterface(urlObj) {
  const hostname = urlObj.hostname;
  const segments = urlObj.pathname.split("/").filter(Boolean);

  // batchexecute handling: /_/PlayStoreUi/data/batchexecute -> PlayStoreUi
  if (urlObj.pathname.includes("batchexecute")) {
    const dataIdx = segments.indexOf("data");
    if (dataIdx > 0) {
      return { name: hostname + "/" + segments[dataIdx - 1], rule: "google-batchexecute-data", matched: segments[dataIdx - 1] };
    }
    const underscoreIdx = segments.indexOf("_");
    if (underscoreIdx !== -1 && segments.length > underscoreIdx + 1) {
      return { name: hostname + "/" + segments[underscoreIdx + 1], rule: "google-batchexecute-underscore", matched: segments[underscoreIdx + 1] };
    }
  }

  // Google Boq pattern: /_/<ServiceName>/<method>... where <ServiceName> is
  // an UpperCamelCase identifier. Covers ConsentUi, OneGoogleBar, etc., even
  // when the URL does not mention batchexecute. Restricted to Google-ish
  // hosts to avoid matching unrelated `_` path segments elsewhere.
  if (
    segments.length >= 2 &&
    segments[0] === "_" &&
    /^[A-Z][A-Za-z0-9]{2,}$/.test(segments[1]) &&
    (hostname.endsWith(".google.com") || hostname.endsWith(".googleapis.com"))
  ) {
    return { name: hostname + "/" + segments[1], rule: "google-boq", matched: "/_/" + segments[1] };
  }

  // gRPC-over-HTTP $rpc/ paths: the package+service identifies the gRPC
  // service and must not collapse to bare hostname, which would fold every
  // method across every gRPC service on that host into one bucket.
  // Matched before the googleapis short-name so each gRPC service on a
  // `*-pa.clients6.google.com` host gets its own bucket.
  const rpcInfo = parseRpcPath(urlObj.pathname);
  if (rpcInfo) {
    return { name: hostname + "/$rpc/" + rpcInfo.grpcFullService, rule: "grpc-over-http", matched: "/$rpc/" + rpcInfo.grpcFullService };
  }

  // Special handling for Google API hosts
  if (
    hostname.endsWith(".googleapis.com") ||
    hostname.endsWith(".clients6.google.com")
  ) {
    const m = hostname.match(/^(?:staging-)?([^.]+)\./);
    return { name: m ? m[1] : hostname, rule: m ? "googleapis-host-prefix" : "googleapis-host-fallback", matched: m ? m[1] : hostname };
  }

  // Google-specific: /async/ is an API root on Google properties only
  const isGoogleHost =
    hostname.endsWith(".google.com") || hostname.includes("google");

  // API root keywords — segments that mark where the API namespace begins
  const apiRootKeywords = [
    "api",
    "_api",
    "__api",
    "rest",
    "graphql",
    "gql",
    "grpc",
    "rpc",
    "wp-json",
    "services",
    "gateway",
  ];
  if (isGoogleHost) apiRootKeywords.push("async");

  const isVersionSeg = (s) => /^v\d+\w*$/i.test(s);

  // Find where the API "root" starts — match keyword roots first
  let rootIdx = -1;
  let keywordMatched = null;
  let versionAppended = null;
  for (let i = 0; i < segments.length; i++) {
    if (apiRootKeywords.includes(segments[i].toLowerCase())) {
      rootIdx = i;
      keywordMatched = segments[i];
      // Also include a following version segment (e.g. api/v2 → rootIdx covers both)
      if (i + 1 < segments.length && isVersionSeg(segments[i + 1])) {
        rootIdx = i + 1;
        versionAppended = segments[i + 1];
      }
      break;
    }
  }

  // If no keyword root, find the first version segment anywhere in the path
  let versionOnly = null;
  if (rootIdx === -1) {
    for (let i = 0; i < segments.length; i++) {
      if (isVersionSeg(segments[i])) {
        rootIdx = i;
        versionOnly = segments[i];
        break;
      }
    }
  }

  if (rootIdx !== -1) {
    const name = hostname + "/" + segments.slice(0, rootIdx + 1).join("/");
    if (keywordMatched) {
      return {
        name,
        rule: versionAppended ? "path-keyword+version" : "path-keyword",
        matched: versionAppended ? keywordMatched + "/" + versionAppended : keywordMatched,
        keyword: keywordMatched,
        version: versionAppended || null,
      };
    }
    return { name, rule: "path-version-only", matched: versionOnly, version: versionOnly };
  }

  // Fallback: group under hostname alone — most sites have one API,
  // and the first path segment is typically a resource, not a service boundary
  return { name: hostname, rule: "hostname-fallback", matched: hostname };
}

function extractInterfaceName(urlObj) {
  return classifyInterface(urlObj).name;
}

// Refine a hostname-fallback classification by detecting shared path
// prefixes with URLs already registered on the same host. This is
// OBSERVATION-DRIVEN — when the tool sees multiple URLs under
// /svc/shreddit/… (or any other common path root) on a host with no
// /api/ or /v1/ keyword, it infers that prefix as a service boundary
// rather than dumping everything under one hostname bucket.
//
// Returns `null` if no shared prefix of >=2 segments is found with any
// sibling method. Otherwise returns a refined classification that
// replaces the hostname-fallback rule with `path-common-prefix`.
function refineByObservedPrefix(tab, urlObj, initialName) {
  if (!tab || !tab.discoveryDocs) return null;
  const hostname = urlObj.hostname;
  const newSegs = urlObj.pathname.split("/").filter(Boolean);
  if (newSegs.length < 2) return null;          // need at least 2 segs to form a prefix

  // Collect already-known method paths under the same hostname, by
  // checking each method's origin. Registered services don't always key
  // on hostname (a probe-discovered doc may have a distinct name), so
  // the origin check is the correct fact-based sibling test.
  const siblingPaths = [];
  for (const [, docEntry] of tab.discoveryDocs) {
    if (!docEntry || !docEntry.doc) continue;
    for (const bucket of Object.values(docEntry.doc.resources || {})) {
      for (const m of Object.values(bucket.methods || {})) {
        if (!m || typeof m.path !== "string" || !m.origin) continue;
        let origHost = null;
        try { origHost = new URL(m.origin).hostname; } catch (_) {}
        if (origHost !== hostname) continue;
        siblingPaths.push(m.path);
      }
    }
  }
  if (!siblingPaths.length) return null;

  let bestPrefixLen = 0;
  for (const sp of siblingPaths) {
    const segs = sp.split("/").filter(Boolean);
    let i = 0;
    while (i < newSegs.length && i < segs.length && newSegs[i] === segs[i]) i++;
    // Require the match to be a STRICT prefix of both — otherwise
    // the two URLs are identical (same method, not a common service).
    if (i > bestPrefixLen && i < newSegs.length && i < segs.length + 1) {
      bestPrefixLen = i;
    }
  }
  if (bestPrefixLen < 2) return null;

  const prefixSegs = newSegs.slice(0, bestPrefixLen);
  // Don't promote prefixes whose last segment is a dynamic-looking ID
  // (numeric, UUID, token) — those aren't service boundaries. Walk
  // backward until we find a non-dynamic segment.
  while (prefixSegs.length >= 2 && looksLikeDynamicSegment(prefixSegs[prefixSegs.length - 1])) {
    prefixSegs.pop();
  }
  if (prefixSegs.length < 2) return null;

  const prefix = "/" + prefixSegs.join("/");
  return {
    name: hostname + prefix,
    rule: "path-common-prefix",
    matched: prefix,
  };
}

// Migrate method entries whose path starts with the given prefix out of
// a hostname-fallback bucket and into a new common-prefix bucket. Used
// when refineByObservedPrefix detects a shared prefix — without this
// the original URL stays orphaned in the hostname bucket while the new
// URL lands in the refined bucket, producing two buckets for the same
// service. Schemas referenced via $ref by migrating methods are copied
// along to preserve lookups.
function migrateToCommonPrefixBucket(tab, oldName, refinement, urlObj) {
  const oldDoc = tab.discoveryDocs.get(oldName);
  if (!oldDoc || !oldDoc.doc) return;
  const newName = refinement.name;
  if (newName === oldName) return;
  const prefixSegs = refinement.matched.replace(/^\//, "").split("/").filter(Boolean);

  // Create or fetch the new docEntry.
  let newDoc = tab.discoveryDocs.get(newName);
  if (!newDoc) {
    newDoc = {
      status: "found",
      isVirtual: true,
      grouping: {
        rule: refinement.rule,
        matched: refinement.matched,
        firstUrl: urlObj ? urlObj.href : null,
      },
      doc: {
        kind: "discovery#restDescription",
        name: newName,
        title: `${newName} (Learned)`,
        rootUrl: oldDoc.doc.rootUrl,
        baseUrl: oldDoc.doc.baseUrl,
        resources: {},
        schemas: {},
      },
    };
    tab.discoveryDocs.set(newName, newDoc);
  }

  const oldSchemas = oldDoc.doc.schemas || {};
  const newSchemas = newDoc.doc.schemas;

  for (const [bucketKey, bucket] of Object.entries(oldDoc.doc.resources || {})) {
    const methods = bucket.methods || {};
    for (const methodKey of Object.keys(methods)) {
      const m = methods[methodKey];
      const mSegs = (m.path || "").split("/").filter(Boolean);
      let matches = mSegs.length >= prefixSegs.length;
      for (let i = 0; matches && i < prefixSegs.length; i++) {
        if (mSegs[i] !== prefixSegs[i]) matches = false;
      }
      if (!matches) continue;
      // Re-id to match new interface.
      m.id = `${newName.replace(/\//g, ".")}.${methodKey}`;
      if (!newDoc.doc.resources[bucketKey]) newDoc.doc.resources[bucketKey] = { methods: {} };
      newDoc.doc.resources[bucketKey].methods[methodKey] = m;
      delete methods[methodKey];
      // Copy the schema this method references so $ref lookups still work.
      if (m.request && m.request.$ref && oldSchemas[m.request.$ref] && !newSchemas[m.request.$ref]) {
        newSchemas[m.request.$ref] = oldSchemas[m.request.$ref];
      }
      if (m.response && m.response.$ref && oldSchemas[m.response.$ref] && !newSchemas[m.response.$ref]) {
        newSchemas[m.response.$ref] = oldSchemas[m.response.$ref];
      }
    }
  }

  // If the hostname bucket is now empty, remove it so it doesn't clutter.
  let remainingMethods = 0;
  for (const b of Object.values(oldDoc.doc.resources || {})) {
    remainingMethods += Object.keys(b.methods || {}).length;
  }
  if (remainingMethods === 0) tab.discoveryDocs.delete(oldName);
}

// Parse $rpc/ paths: "/$rpc/google.internal.people.v2.InternalPeopleService/GetPeople"
// → { grpcPackage, grpcService, grpcMethod }
const RPC_PATH_RE = /^\/\$rpc\/(.+)\/([^/]+)$/;

function parseRpcPath(path) {
  const m = RPC_PATH_RE.exec(path);
  if (!m) return null;
  const fullService = m[1]; // "google.internal.people.v2.InternalPeopleService"
  const method = m[2]; // "GetPeople"
  // Split into package + service name
  const lastDot = fullService.lastIndexOf(".");
  return {
    grpcFullService: fullService,
    grpcPackage: lastDot > -1 ? fullService.slice(0, lastDot) : "",
    grpcService: lastDot > -1 ? fullService.slice(lastDot + 1) : fullService,
    grpcMethod: method,
  };
}




/** Detect path segments that look like dynamic IDs rather than resource names. */
function looksLikeDynamicSegment(s) {
  if (/^\d+$/.test(s)) return true; // Pure numeric
  if (/^[0-9a-f]{8}-[0-9a-f]{4}-/i.test(s)) return true; // UUID prefix
  if (/^[0-9a-f]{24}$/i.test(s)) return true; // MongoDB ObjectId
  // Base64-like tokens: 16+ chars, must contain a digit (avoids camelCase names)
  if (
    s.length >= 16 &&
    /^[A-Za-z0-9_-]+$/.test(s) &&
    /\d/.test(s) &&
    !/^[a-z]+$/.test(s)
  )
    return true;
  return false;
}

function calculateMethodMetadata(urlObj, interfaceName, hint) {
  // Explicit hint (e.g. GraphQL operationName) takes precedence over URL.
  // A GraphQL endpoint at /svc/shreddit/graphql serves dozens of distinct
  // operations (GetUser, CreatePost, …). Without the hint every op would
  // collapse to one URL-derived name; with the hint each gets its own
  // method entry keyed by operationName.
  if (hint && typeof hint === "string" && hint.length > 0) {
    return {
      methodName: hint,
      methodId: `${interfaceName.replace(/\//g, ".")}.${hint}`,
    };
  }
  // batchexecute: use first rpcid from URL param (individual calls registered by learnFromRequest)
  if (urlObj.pathname.includes("batchexecute")) {
    const rpcids = urlObj.searchParams.get("rpcids") || "batch";
    const primaryRpcId = rpcids.split(",")[0].trim();
    return {
      methodName: primaryRpcId,
      methodId: `${interfaceName.replace(/\//g, ".")}.${primaryRpcId}`,
    };
  }

  const segments = urlObj.pathname.split("/").filter(Boolean);
  const interfaceParts = interfaceName.split("/");

  // Method segments are everything after the interface prefix
  // If interface is "example.com/api/v1" and path is "/api/v1/users/get"
  // startIdx should skip "api" and "v1".

  const hostname = urlObj.hostname;
  let startIdx = 0;
  if (interfaceName.startsWith(hostname)) {
    startIdx = interfaceParts.length - 1;
  }

  let methodSegments = segments.slice(startIdx);

  // Strip segments that look like hashes, long ID lists, or path-style params
  methodSegments = methodSegments.filter((s) => {
    if (s.length > 32) return false;
    if (s.includes("=")) return false; // path-style parameter (e.g. name=foo)
    return true;
  });

  // Normalize dynamic segments (IDs, UUIDs, tokens) to prevent method proliferation
  methodSegments = methodSegments.map((s) =>
    looksLikeDynamicSegment(s) ? "_id" : s,
  );

  let methodName = methodSegments.join("_") || "root";

  // If it's a gRPC-style path, use the actual method name
  if (urlObj.pathname.includes("$rpc")) {
    methodName = segments[segments.length - 1];
  }

  const methodId = `${interfaceName.replace(/\//g, ".")}.${methodName}`;

  return { methodName, methodId };
}

// ─── Smart Learning ──────────────────────────────────────────────────────────

// Register an AST-observed fetch call site as a method on its service
// WITHOUT fabricating data. Unlike the previous design (which laundered
// AST values through a fake URL + fake JSON body into learnFromRequest
// so generateSchemaFromJson could extract field names), this function
// attaches AST facts DIRECTLY:
//   - method.parameters[name]._astValidValues    query params
//   - doc.schemas[…].properties[name]._astValidValues  body fields
//   - method._astSourceScript                     where in the JS bundle
// All AST-origin data is tagged `_astInferred: true` so pickExampleValue
// reports it under `ast-constraint` provenance — never as "observed-top"
// (which implies the SERVER received this value; the server never did).
// Stats observation counters are never bumped.
function learnFromAstCallSite(tabId, interfaceName, callSite, scriptUrl) {
  const tab = getTab(tabId);

  // Resolve URL. Dynamic / unresolvable → register service-level only,
  // no synthetic method entry (would confuse the reviewer with made-up
  // paths like `dynamic_0`).
  //
  // Relative URLs resolve against the PAGE's origin at runtime, NOT the
  // script's host. Cross-origin-hosted scripts (e.g. Reddit serves its
  // shreddit bundle from www.redditstatic.com but it fetches against
  // www.reddit.com when the bundle executes on a reddit.com page) would
  // otherwise be misattributed to the script's host. Fall back to
  // scriptUrl only when the tab's page URL isn't available.
  const isDynamic = /^\$\{|^\(dynamic\)|^\{[a-zA-Z]/.test(callSite.url);
  let csUrl = null;
  if (!isDynamic) {
    try {
      const _pageMeta = _tabMeta.get(tabId);
      const _baseForRel = (_pageMeta && _pageMeta.url) ? _pageMeta.url : scriptUrl;
      csUrl = /^https?:\/\//i.test(callSite.url)
        ? new URL(callSite.url)
        : new URL(callSite.url, _baseForRel);
    } catch (_) { return null; }
  }

  // Classification at AST-time is an OPEN question — we don't have a
  // response body to magic-byte-sniff, and request shape alone (GET
  // with no query / body) can mean either "static asset fetch" or
  // "plain API endpoint." We register the method regardless and defer
  // real API-vs-asset classification to the moment real traffic flows
  // (handleResponseBody stamps _responseKind="asset" via magic bytes).


  // Get-or-create docEntry — same prologue as learnFromRequest.
  // When the classifier falls back to hostname, also check for observed-
  // prefix clustering against siblings on the same host. If a shared
  // path prefix of >=2 segments exists, promote to a prefix-based bucket
  // AND migrate matching siblings so the service groups as one.
  let grouping = csUrl ? classifyInterface(csUrl) : { rule: "ast-dynamic", matched: "dynamic URL" };
  if (csUrl && grouping.rule === "hostname-fallback") {
    const refined = refineByObservedPrefix(tab, csUrl, grouping.name);
    if (refined) {
      migrateToCommonPrefixBucket(tab, grouping.name || interfaceName, refined, csUrl);
      grouping = refined;
      interfaceName = refined.name;
    }
  }
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry || !docEntry.doc) {
    docEntry = {
      status: "found",
      isVirtual: true,
      grouping: { rule: grouping.rule, matched: grouping.matched, firstUrl: csUrl ? csUrl.href : callSite.url },
      doc: {
        kind: "discovery#restDescription",
        name: interfaceName,
        title: `${interfaceName} (Learned)`,
        rootUrl: csUrl ? csUrl.origin + "/" : "https://" + interfaceName + "/",
        baseUrl: csUrl ? csUrl.origin + "/" : "https://" + interfaceName + "/",
        resources: { learned: { methods: {} } },
        schemas: {},
      },
    };
    tab.discoveryDocs.set(interfaceName, docEntry);
  }
  const doc = docEntry.doc;
  if (!doc.resources.learned) doc.resources.learned = { methods: {} };

  if (!csUrl) return docEntry;   // dynamic-URL case: docEntry exists, no method

  // Method name + collision handling — mirrors learnFromRequest.
  const { methodName: baseMethodName } = calculateMethodMetadata(csUrl, interfaceName);
  const qualifiedName = callSite.method.toLowerCase() + "_" + baseMethodName;
  const probedMethod = doc.resources.probed?.methods?.[baseMethodName];

  let methodName;
  const existingBase = doc.resources.learned.methods[baseMethodName];
  const existingQualified = doc.resources.learned.methods[qualifiedName];
  if (existingQualified) {
    methodName = qualifiedName;
  } else if (existingBase && existingBase.httpMethod !== callSite.method && !probedMethod) {
    const existQualName = existingBase.httpMethod.toLowerCase() + "_" + baseMethodName;
    if (!doc.resources.learned.methods[existQualName]) {
      existingBase.id = `${interfaceName.replace(/\//g, ".")}.${existQualName}`;
      doc.resources.learned.methods[existQualName] = existingBase;
    }
    delete doc.resources.learned.methods[baseMethodName];
    methodName = qualifiedName;
  } else {
    methodName = baseMethodName;
  }

  const methodId = `${interfaceName.replace(/\//g, ".")}.${methodName}`;
  if (!doc.resources.learned.methods[methodName] && !probedMethod) {
    doc.resources.learned.methods[methodName] = {
      id: methodId,
      path: csUrl.pathname.substring(1),
      httpMethod: callSite.method,
      parameters: {},
      request: null,
      origin: csUrl.origin,
      _astSourceScript: scriptUrl || null,
      _astInferred: true,
      _astCallSites: [],
    };
  }
  const m = probedMethod || doc.resources.learned.methods[methodName];
  if (m && !m.origin) m.origin = csUrl.origin;
  // Record every AST call site that registered this method. Dedup by
  // script + line so the same call doesn't accumulate on repeat scans.
  // Reviewer uses these to click through to the JS location each
  // endpoint was discovered in — facts about where the code lives.
  if (m) {
    if (!Array.isArray(m._astCallSites)) m._astCallSites = [];
    const cs = {
      script: scriptUrl || null,
      line: callSite.loc ? callSite.loc.line : null,
      column: callSite.loc ? callSite.loc.column : null,
      enclosingFunction: callSite.enclosingFunction || null,
    };
    const key = `${cs.script}:${cs.line}:${cs.column}`;
    const alreadySeen = m._astCallSites.some(x => `${x.script}:${x.line}:${x.column}` === key);
    if (!alreadySeen) m._astCallSites.push(cs);
  }

  // Type inference helper: pick from the first valid value's runtime type.
  // Not a guess — this is the literal AST observed. Default "string" when
  // no observations (neutral; no false precision).
  const _inferType = (validValues, defaultValue) => {
    const sample = Array.isArray(validValues) && validValues.length ? validValues[0]
      : (defaultValue !== undefined ? defaultValue : null);
    if (sample == null) return "string";
    if (typeof sample === "number") return "number";
    if (typeof sample === "boolean") return "boolean";
    return "string";
  };

  // Merge AST-observed valid values onto a target (param or schema prop).
  // Promotes to `enum` when distinct count >= 2 (matches prior behavior).
  const _mergeAstValues = (target, validValues, defaultValue) => {
    if (Array.isArray(validValues) && validValues.length) {
      const prev = Array.isArray(target._astValidValues) ? target._astValidValues.slice() : [];
      for (const vv of validValues) {
        const s = String(vv);
        if (prev.indexOf(s) < 0) prev.push(s);
      }
      target._astValidValues = prev;
      if (prev.length >= 2 && !target.customEnum && !target.enum) {
        target.enum = prev.slice();
        target._detectedEnum = true;
      }
    }
    if (defaultValue !== undefined) target._astDefault = defaultValue;
  };

  // Query params — direct registration.
  if (callSite.params) {
    for (const p of callSite.params) {
      if ((p.location || "query") !== "query") continue;
      if (!m.parameters[p.name]) {
        m.parameters[p.name] = {
          type: _inferType(p.validValues, p.defaultValue),
          location: "query",
          description: "Learned from AST fetch call site",
          _astInferred: true,
        };
      }
      _mergeAstValues(m.parameters[p.name], p.validValues, p.defaultValue);
    }
  }

  // Body params — build a direct AST schema (no synthetic JSON round-trip).
  const bodyParams = (callSite.params || []).filter(p => (p.location || "query") === "body");
  if (bodyParams.length) {
    const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
    if (!doc.schemas[schemaName]) {
      doc.schemas[schemaName] = { id: schemaName, type: "object", properties: {}, _astInferred: true };
    }
    const schema = doc.schemas[schemaName];
    if (!schema.properties) schema.properties = {};
    for (const bp of bodyParams) {
      if (!schema.properties[bp.name]) {
        schema.properties[bp.name] = {
          type: _inferType(bp.validValues, bp.defaultValue),
          _astInferred: true,
        };
      }
      _mergeAstValues(schema.properties[bp.name], bp.validValues, bp.defaultValue);
    }
    if (!m.request) m.request = { $ref: schemaName };
  }

  // Record content-type when AST captured it and the method hasn't seen
  // a real request-time content type yet. Real traffic overrides.
  if (callSite.headers) {
    const ct = callSite.headers["content-type"] || callSite.headers["Content-Type"];
    if (ct && (!m.contentTypes || m.contentTypes.length === 0)) {
      m.contentTypes = [ct];
    }
  }

  // Apply example-value picker so the Send form has prefills even
  // before any real traffic hits — pickExampleValue's `ast-constraint`
  // tier uses the _astValidValues we just attached. applyStatsToMethod
  // also walks any body-schema props we created with _astInferred:true.
  applyStatsToMethod(m, doc);

  return docEntry;
}

function learnFromRequest(tabId, interfaceName, entry, headers) {
  const tab = getTab(tabId);
  const url = new URL(entry.url);
  const method = entry.method;

  // Record WHICH grouping rule fired when this service was first
  // created. Grouping decisions must be traceable to the rule that
  // produced them so reviewers can judge. When classifyInterface falls
  // back to hostname-only, also check for shared path prefixes with
  // siblings on the same host — observed-prefix clustering catches
  // cases like `/svc/shreddit/*` that no keyword rule covers.
  let grouping = classifyInterface(url);
  if (grouping.rule === "hostname-fallback") {
    const refined = refineByObservedPrefix(tab, url, grouping.name);
    if (refined) {
      migrateToCommonPrefixBucket(tab, grouping.name, refined, url);
      grouping = refined;
      interfaceName = refined.name;
    }
  }
  // Stamp the resolved name onto the entry so callers (handleResponseBody)
  // can use it for downstream lookups instead of their pre-migration
  // `service` variable — the bucket they came in with may have been
  // emptied and deleted during migration.
  entry.interfaceName = interfaceName;
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry || !docEntry.doc) {
    docEntry = {
      status: "found",
      isVirtual: true,
      grouping: { rule: grouping.rule, matched: grouping.matched, firstUrl: entry.url },
      doc: {
        kind: "discovery#restDescription",
        name: interfaceName,
        title: `${interfaceName} (Learned)`,
        rootUrl: url.origin + "/",
        baseUrl: url.origin + "/",
        resources: {
          learned: { methods: {} },
        },
        schemas: {},
      },
    };
    tab.discoveryDocs.set(interfaceName, docEntry);
  }

  const doc = docEntry.doc;
  if (!doc.resources.learned) doc.resources.learned = { methods: {} };

  // GraphQL: method name = operationName (or first root field). Every op on
  // a /graphql endpoint is its own method entry. Detect by parsing the
  // request body — we don't rely on URL containing "graphql" alone.
  let _nameHint = null;
  if (entry.rawBodyB64) {
    try {
      const _bodyText = new TextDecoder().decode(base64ToUint8(entry.rawBodyB64));
      const _gql = parseGraphQLRequest(_bodyText);
      if (_gql && _gql.operations && _gql.operations.length > 0) {
        // Batched: name from first op; other ops registered separately would
        // need splitting at the call site — left as one method for now.
        _nameHint = deriveGraphQLMethodName(_gql.operations[0]);
      }
    } catch (_) {}
  }

  const { methodName: baseMethodName } = calculateMethodMetadata(url, interfaceName, _nameHint);
  const qualifiedName = method.toLowerCase() + "_" + baseMethodName;

  // If this method was already probed with richer schema, update it there instead
  const probedMethod = doc.resources.probed?.methods?.[baseMethodName];

  // Resolve method name — disambiguate when different HTTP methods hit the same path
  let methodName;
  const existingBase = doc.resources.learned.methods[baseMethodName];
  const existingQualified = doc.resources.learned.methods[qualifiedName];

  if (existingQualified) {
    // Already disambiguated from a prior collision — use qualified name
    methodName = qualifiedName;
  } else if (existingBase && existingBase.httpMethod !== method && !probedMethod) {
    // Collision: different HTTP method to same path — rename existing, qualify new
    const existQualName = existingBase.httpMethod.toLowerCase() + "_" + baseMethodName;
    if (!doc.resources.learned.methods[existQualName]) {
      existingBase.id = `${interfaceName.replace(/\//g, ".")}.${existQualName}`;
      doc.resources.learned.methods[existQualName] = existingBase;
    }
    delete doc.resources.learned.methods[baseMethodName];
    methodName = qualifiedName;
  } else {
    // No collision — use base name
    methodName = baseMethodName;
  }

  const methodId = `${interfaceName.replace(/\//g, ".")}.${methodName}`;
  entry.methodId = methodId;

  if (!doc.resources.learned.methods[methodName] && !probedMethod) {
    doc.resources.learned.methods[methodName] = {
      id: methodId,
      path: url.pathname.substring(1),
      httpMethod: method,
      parameters: {},
      request: null,
      origin: url.origin,
    };
  }

  const m = probedMethod || doc.resources.learned.methods[methodName];
  if (m && !m.origin) m.origin = url.origin;

  // Learn query parameters from URL
  if (!url.pathname.includes("batchexecute")) {
    url.searchParams.forEach((value, name) => {
      if (name === "key" || name === "api_key") return;
      // $httpHeaders is a gRPC-Web transport mechanism (CRLF-separated headers in URL),
      // not an API parameter. Putting it in a form input strips \r\n and corrupts the URL.
      if (name === "$httpHeaders") return;
      // $ct is a multipart batch Content-Type transport param, not an API parameter.
      if (name === "$ct") return;
      if (!m.parameters[name]) {
        m.parameters[name] = {
          type: isNaN(value) ? "string" : "number",
          location: "query",
          description: "Learned from request",
        };
      }
    });

    // Learn path parameters by comparing URL to stored template AND
    // by detecting ID-like segments on first observation.
    const segments = url.pathname.split("/").filter(Boolean);
    const templateParts = (m.path || "").split("/").filter(Boolean);
    if (templateParts.length === segments.length) {
      let changed = false;
      for (let i = 0; i < segments.length; i++) {
        if (templateParts[i].startsWith("{")) continue; // Already templated
        if (templateParts[i] !== segments[i]) {
          // Segment differs from template — definitely a parameter
          const paramName = `path_${templateParts[i] || "param" + i}`;
          templateParts[i] = `{${paramName}}`;
          if (!m.parameters[paramName]) {
            m.parameters[paramName] = {
              type: "string",
              location: "path",
              description: "Inferred path parameter",
            };
          }
          changed = true;
        } else if (looksLikeDynamicSegment(segments[i])) {
          // First observation but segment looks like an ID/UUID/token
          const paramName = `path_param${i}`;
          templateParts[i] = `{${paramName}}`;
          if (!m.parameters[paramName]) {
            m.parameters[paramName] = {
              type: "string",
              location: "path",
              description: "Inferred path parameter (pattern-detected)",
            };
          }
          changed = true;
        }
      }
      if (changed) m.path = templateParts.join("/");
    }
  }

  // Record the observed Content-Type on the method for replay fidelity
  if (headers["content-type"]) {
    const ct = headers["content-type"].split(";")[0].trim();
    if (!m.contentTypes) m.contentTypes = [];
    if (!m.contentTypes.includes(ct)) m.contentTypes.unshift(ct);
  }

  // Learn request body if present
  if (entry.rawBodyB64) {
    const bytes = base64ToUint8(entry.rawBodyB64);
    const text = new TextDecoder().decode(bytes);
    const isBatch = url.pathname.includes("batchexecute");

    if (isBatch) {
      const calls = parseBatchExecuteRequest(text);
      if (calls) {
        for (const call of calls) {
          const callMethodId = `${interfaceName.replace(/\//g, ".")}.${call.rpcId}`;
          if (!doc.resources.learned.methods[call.rpcId]) {
            doc.resources.learned.methods[call.rpcId] = {
              id: callMethodId,
              path: url.pathname.substring(1),
              httpMethod: "POST",
              parameters: {},
              request: null,
            };
          }
          const callM = doc.resources.learned.methods[call.rpcId];
          const schemaName = `${call.rpcId}Request`;
          callM.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(
            call.data,
            schemaName,
            doc.schemas,
            true,
          );
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      }
    } else if (isMultipartBatch(headers["content-type"])) {
      // Multipart batch: each part is an individual HTTP sub-request with its own body
      const parts = parseMultipartBatchRequest(text, headers["content-type"]);
      if (parts) {
        for (const part of parts) {
          // Derive method name from part path
          const pathSegs = part.path.split("?")[0].split("/").filter(Boolean)
            .filter((s) => s.length <= 32)
            .map((s) => looksLikeDynamicSegment(s) ? "_id" : s);
          const partMethodName = part.method.toLowerCase() + "_" +
            (pathSegs.join("_") || "batch_part");
          const partMethodId = `${interfaceName.replace(/\//g, ".")}.${partMethodName}`;
          if (!doc.resources.learned.methods[partMethodName]) {
            doc.resources.learned.methods[partMethodName] = {
              id: partMethodId,
              path: part.path,
              httpMethod: part.method,
              parameters: {},
              request: null,
              _batchPart: true,
            };
          }
          const partM = doc.resources.learned.methods[partMethodName];
          if (part.body) {
            try {
              const json = JSON.parse(part.body);
              const schemaName = `${partMethodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
              partM.request = { $ref: schemaName };
              const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
              mergeSchemaInto(doc, schemaName, newSchema);
            } catch (_) {}
          }
        }
      }
    } else if (
      headers["content-type"]?.includes("grpc-web") ||
      headers["content-type"]?.includes("grpc+proto")
    ) {
      // gRPC-Web request body: 5-byte frame header + protobuf payload
      try {
        const parsed = parseGrpcWebFrames(bytes);
        if (parsed) {
          for (const frame of parsed.frames) {
            if (frame.type !== "data") continue;
            const tree = pbDecodeTree(frame.data, 8);
            if (tree && tree.length > 0) {
              const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
              m.request = { $ref: schemaName };
              const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
              mergeSchemaInto(doc, schemaName, newSchema);
            }
          }
        }
      } catch (e) {}
    } else if (headers["content-type"]?.includes("json+protobuf")) {
      // JSPB body — positional array encoding
      try {
        const json = JSON.parse(text);
        if (Array.isArray(json)) {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas, true);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {}
    } else if (
      headers["content-type"]?.includes("x-protobuf") ||
      headers["content-type"]?.includes("application/protobuf")
    ) {
      // Binary protobuf body
      try {
        const tree = pbDecodeTree(bytes, 8);
        if (tree && tree.length > 0) {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {}
    } else if (headers["content-type"]?.includes("json")) {
      try {
        const json = JSON.parse(text);
        const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
        m.request = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
        mergeSchemaInto(doc, schemaName, newSchema);
      } catch (e) {}
    } else if (headers["content-type"]?.includes("x-www-form-urlencoded")) {
      // Form-urlencoded with f.req JSPB (non-batchexecute, e.g. browserinfo)
      try {
        const params = new URLSearchParams(text);
        const fReq = params.get("f.req");
        if (fReq) {
          const json = JSON.parse(fReq);
          if (Array.isArray(json)) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
            m.request = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas, true);
            mergeSchemaInto(doc, schemaName, newSchema);
          }
        }
      } catch (e) {}
    }
  }

  // ─── Statistics collection ───────────────────────────────────────────────
  // Only real network traffic reaches here — AST fetch call sites go
  // through learnFromAstCallSite directly (no synthetic entries).
  if (!m._stats) m._stats = { requestCount: 0, params: {}, bodyFields: {} };
  m._stats.requestCount++;

  // Track query param values
  if (!url.pathname.includes("batchexecute")) {
    url.searchParams.forEach((value, name) => {
      if (name === "key" || name === "api_key") return;
      if (name === "$httpHeaders" || name === "$ct") return;
      if (!m._stats.params[name]) m._stats.params[name] = createParamStats();
      updateParamStats(m._stats.params[name], value);
    });
  }

  // Track body field values (JSON bodies only)
  if (entry.isJson && entry.decodedBody && typeof entry.decodedBody === "object") {
    const flat = flattenObjectValues(entry.decodedBody);
    for (const [fieldPath, value] of Object.entries(flat)) {
      if (typeof value === "string" || typeof value === "number") {
        if (!m._stats.bodyFields[fieldPath]) m._stats.bodyFields[fieldPath] = createParamStats();
        updateParamStats(m._stats.bodyFields[fieldPath], String(value));
      }
    }
  }

  // Apply stats-derived metadata back to parameters AND body-field schemas
  applyStatsToMethod(m, doc);

  // ─── Chain detection ────────────────────────────────────────────────────
  if (tab._valueIndex) {
    const requestParams = {};
    url.searchParams.forEach((v, k) => { requestParams[k] = v; });
    // Extract body values for chain matching: use JSON body if available
    let chainBody = {};
    if (entry.isJson && entry.decodedBody) {
      chainBody = entry.decodedBody;
    } else if (entry.rawBodyB64) {
      try {
        const _cbText = new TextDecoder().decode(base64ToUint8(entry.rawBodyB64));
        chainBody = JSON.parse(_cbText);
      } catch (_) {}
    }
    const bodyValues = flattenObjectValues(chainBody);
    const links = findChainLinks(tab._valueIndex, requestParams, bodyValues, methodId);
    if (links.length) {
      m._chains = mergeChainLinks(m._chains, links);
      // Update outgoing chains on source methods
      for (var li = 0; li < links.length; li++) {
        var srcMethod = findMethodInDoc(doc, links[li].sourceMethodId);
        if (srcMethod) {
          if (!srcMethod._chains) srcMethod._chains = { incoming: [], outgoing: [] };
          var outLink = {
            targetMethodId: methodId,
            paramName: links[li].paramName,
            sourceFieldPath: links[li].sourceFieldPath,
            lastSeen: links[li].lastSeen,
          };
          var outDupe = false;
          for (var oi = 0; oi < srcMethod._chains.outgoing.length; oi++) {
            var o = srcMethod._chains.outgoing[oi];
            if (o.targetMethodId === methodId && o.paramName === links[li].paramName && o.sourceFieldPath === links[li].sourceFieldPath) {
              o.observedCount = (o.observedCount || 1) + 1;
              o.lastSeen = links[li].lastSeen;
              outDupe = true;
              break;
            }
          }
          if (!outDupe) {
            outLink.observedCount = 1;
            srcMethod._chains.outgoing.push(outLink);
          }
        }
      }
    }
  }
}

function _applyStatsToField(field, fieldStats, requestCount) {
  if (!field || !fieldStats) return;

  // Required detection
  const reqAnalysis = analyzeRequired(fieldStats, requestCount);
  if (!field.customRequired) {
    field.required = reqAnalysis.required;
    field._requiredConfidence = reqAnalysis.confidence;
  }

  // Enum detection
  const enumAnalysis = analyzeEnum(fieldStats);
  if (enumAnalysis.isEnum && !field.customEnum) {
    field.enum = enumAnalysis.values;
    field._detectedEnum = true;
  }

  // Default detection
  const defaultAnalysis = analyzeDefault(fieldStats);
  if (defaultAnalysis.hasDefault) {
    field._defaultValue = defaultAnalysis.value;
    field._defaultConfidence = defaultAnalysis.confidence;
  }

  // Type narrowing from format hints
  const narrowedFormat = analyzeFormat(fieldStats);
  if (narrowedFormat && field.type === "string") {
    field.format = narrowedFormat;
  }

  // Numeric range
  const range = analyzeRange(fieldStats);
  if (range) field._range = range;
}

// Walk a request schema's property tree, resolving $refs, and attach
// body-field stats at each dot-path that matches a stats.bodyFields
// entry. Stops when the schema ref has already been visited to avoid
// cycles on self-referential message types.
function _applyBodyFieldStats(m, doc, bodyFieldStats, requestCount) {
  if (!m.request || !m.request.$ref || !doc || !doc.schemas) return;
  function walk(sch, prefix, visited) {
    if (!sch || !sch.properties) return;
    for (const [k, def] of Object.entries(sch.properties)) {
      const dotPath = prefix ? prefix + "." + k : k;
      const fs = bodyFieldStats[dotPath];
      if (fs) {
        _applyStatsToField(def, fs, requestCount);
        // Example value + provenance on the field def so the form
        // renderer can prefill without a second pass.
        const ex = pickExampleValue(def, fs);
        def._exampleValue = ex.value;
        def._exampleValueSource = ex.source;
        if (ex.confidence != null) def._exampleConfidence = ex.confidence;
      } else {
        // No observation for this specific field — still pick a synthetic
        // example from the schema shape so the UI never has nothing.
        const ex = pickExampleValue(def, null);
        def._exampleValue = ex.value;
        def._exampleValueSource = ex.source;
      }
      // Recurse into sub-schemas. Track visited refs per-walk to prevent
      // infinite loops on recursive schemas (e.g. tree-shaped messages).
      if (def.$ref && !visited.has(def.$ref)) {
        visited.add(def.$ref);
        walk(doc.schemas[def.$ref], dotPath, visited);
        visited.delete(def.$ref);
      } else if (def.type === "array" && def.items && def.items.$ref && !visited.has(def.items.$ref)) {
        visited.add(def.items.$ref);
        walk(doc.schemas[def.items.$ref], dotPath + "[]", visited);
        visited.delete(def.items.$ref);
      } else if (def.properties) {
        walk(def, dotPath, visited);
      }
    }
  }
  const rootSchema = doc.schemas[m.request.$ref];
  if (!rootSchema) return;
  walk(rootSchema, "", new Set([m.request.$ref]));
}

function applyStatsToMethod(m, doc) {
  // Runs for:
  //   - Real-traffic methods (m._stats populated by learnFromRequest).
  //   - AST-only methods (m._stats may be empty; m.parameters populated
  //     by learnFromAstCallSite with _astValidValues / _astInferred).
  // pickExampleValue naturally handles both: if paramStats is null/empty
  // and the field has _astValidValues, the `ast-constraint` tier wins;
  // otherwise it falls through to enum/format/type-default.
  const stats = m._stats || { requestCount: 0, params: {}, bodyFields: {} };

  // Observed-stats pass: fires analyzer metadata (required/enum/default/
  // format/range) for params we have counts on.
  for (const [name, paramStats] of Object.entries(stats.params || {})) {
    if (!m.parameters[name]) continue;
    _applyStatsToField(m.parameters[name], paramStats, stats.requestCount);
  }

  // Example value pass: cover EVERY declared parameter, real-observed
  // or AST-only. Previously AST-only params got no _exampleValue
  // because we iterated stats.params (which didn't include them) and
  // the Send form showed empty inputs forever until real traffic hit.
  for (const [name, param] of Object.entries(m.parameters || {})) {
    const paramStats = (stats.params || {})[name] || null;
    const ex = pickExampleValue(param, paramStats);
    param._exampleValue = ex.value;
    param._exampleValueSource = ex.source;
    if (ex.confidence != null) param._exampleConfidence = ex.confidence;
    else delete param._exampleConfidence;
  }

  // Body fields: the schema tree lives in doc.schemas — walk it and
  // attach stats + example values by dot-path. Without this, popup
  // rendering knew which FIELDS existed but not what values to prefill.
  if (doc && stats.bodyFields) {
    _applyBodyFieldStats(m, doc, stats.bodyFields, stats.requestCount);
  }

  // Correlations
  stats.correlations = detectCorrelations(stats);
}

function findMethodInDoc(doc, methodId) {
  if (!doc || !doc.resources) return null;
  for (const rKey of Object.keys(doc.resources)) {
    var methods = doc.resources[rKey]?.methods;
    if (methods) {
      for (var mKey in methods) {
        if (methods[mKey].id === methodId) return methods[mKey];
      }
    }
  }
  return null;
}

function learnFromResponse(tabId, interfaceName, entry) {
  if (!entry.responseBody) return;

  const tab = getTab(tabId);
  const url = new URL(entry.url);
  const { methodName } = calculateMethodMetadata(url, interfaceName);
  // Check tab-level first, then fall back to globalStore (survives SW restarts)
  let docEntry = tab.discoveryDocs.get(interfaceName);
  if (!docEntry?.doc) {
    const globalEntry = globalStore.discoveryDocs.get(interfaceName);
    if (globalEntry?.doc) {
      docEntry = globalEntry;
      // Also set on tab so subsequent lookups are fast
      tab.discoveryDocs.set(interfaceName, docEntry);
    }
  }
  if (!docEntry || !docEntry.doc) return;
  const doc = docEntry.doc;
  // Find method — try base name first, then HTTP-qualified name (from disambiguation)
  const qualifiedName = entry.method ? entry.method.toLowerCase() + "_" + methodName : null;
  const learned = doc.resources.learned?.methods;
  const m = learned
    ? (learned[methodName] || (qualifiedName ? learned[qualifiedName] : null))
    : null;
  // Also check probed methods
  const proM = doc.resources.probed
    ? doc.resources.probed.methods[methodName]
    : null;
  const targetM = m || proM;
  if (!targetM) return;

  // Decode base64 to text for JSON/Batch parsing
  let textBody = entry.responseBody;
  if (entry.responseBase64) {
    try {
      const bytes = base64ToUint8(entry.responseBody);
      textBody = new TextDecoder().decode(bytes);
    } catch (e) {
      textBody = null;
    }
  }
  if (!textBody) return;

  const mimeType = entry.mimeType || "";
  if (isAsyncChunkedResponse(textBody)) {
    const chunks = parseAsyncChunkedResponse(textBody);
    if (chunks) {
      if (!doc.resources.learned) doc.resources.learned = { methods: {} };
      // Use endpoint path as the method key (e.g. "hpba" from /async/hpba)
      const asyncPath = url.pathname.split("/").filter(Boolean).pop() || methodName;
      for (let i = 0; i < chunks.length; i++) {
        const chunk = chunks[i];
        if (chunk.type !== "jspb" || !Array.isArray(chunk.data)) continue;

        const chunkKey = `${asyncPath}_chunk${i}`;
        let callM =
          doc.resources.learned.methods[chunkKey] ||
          doc.resources.probed?.methods[chunkKey];
        if (!callM) {
          doc.resources.learned.methods[chunkKey] = {
            id: `${interfaceName.replace(/\//g, ".")}.${chunkKey}`,
            path: url.pathname.substring(1),
            httpMethod: entry.method || "GET",
            parameters: {},
            request: null,
            response: null,
          };
          callM = doc.resources.learned.methods[chunkKey];
        }

        const schemaName = `${chunkKey}Response`;
        callM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          chunk.data,
          schemaName,
          doc.schemas,
          true,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    }
  } else if (isBatchExecuteResponse(textBody)) {
    const results = parseBatchExecuteResponse(textBody);
    if (results) {
      if (!doc.resources.learned) doc.resources.learned = { methods: {} };
      for (const res of results) {
        let callM =
          doc.resources.learned.methods[res.rpcId] ||
          doc.resources.probed?.methods[res.rpcId];
        // Create method entry if response arrived before request was learned
        if (!callM) {
          doc.resources.learned.methods[res.rpcId] = {
            id: `${interfaceName.replace(/\//g, ".")}.${res.rpcId}`,
            path: url.pathname.substring(1),
            httpMethod: "POST",
            parameters: {},
            request: null,
            response: null,
          };
          callM = doc.resources.learned.methods[res.rpcId];
        }

        const schemaName = `${res.rpcId}Response`;
        callM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          res.data,
          schemaName,
          doc.schemas,
          true,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    }
  } else if (isGrpcWeb(mimeType)) {
    // gRPC-Web: unwrap frames, decode protobuf payload
    try {
      let bytes;
      if (isGrpcWebText(mimeType)) {
        // grpc-web-text uses base64 encoding
        bytes = base64ToUint8(
          entry.responseBase64 ? entry.responseBody : btoa(entry.responseBody),
        );
      } else {
        bytes = entry.responseBase64
          ? base64ToUint8(entry.responseBody)
          : new TextEncoder().encode(entry.responseBody);
      }
      const parsed = parseGrpcWebFrames(bytes);
      if (parsed) {
        for (const frame of parsed.frames) {
          if (frame.type !== "data") continue;
          const tree = pbDecodeTree(frame.data, 8, (val) => {
            if (typeof val === "string") {
              extractKeysFromText(tabId, val, entry.url, "response_grpc");
            }
          });
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
          targetM.response = { $ref: schemaName };
          const newSchema = generateSchemaFromPbTree(
            tree,
            schemaName,
            doc.schemas,
          );
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      }
    } catch (e) {}
  } else if (isSSE(mimeType)) {
    // Server-Sent Events: learn schema from JSON data payloads
    try {
      const events = parseSSE(textBody);
      if (events) {
        for (const evt of events) {
          if (typeof evt.data === "object" && evt.data !== null) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Event`;
            targetM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              evt.data,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
            break; // Schema from first JSON event is representative
          }
        }
      }
    } catch (e) {}
  } else if (isNDJSON(mimeType)) {
    // NDJSON: learn schema from first object
    try {
      const objects = parseNDJSON(textBody);
      if (objects && objects.length > 0) {
        const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
        targetM.response = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(
          objects[0],
          schemaName,
          doc.schemas,
        );
        mergeSchemaInto(doc, schemaName, newSchema);
      }
    } catch (e) {}
  } else if (isMultipartBatch(mimeType)) {
    // Multipart batch: learn schema from each part's body
    try {
      const parts = parseMultipartBatch(textBody, mimeType);
      if (parts) {
        for (let i = 0; i < parts.length; i++) {
          const part = parts[i];
          if (!part.body) continue;
          try {
            const json = JSON.parse(part.body);
            const partKey = `${methodName}_part${i}`;
            if (!doc.resources.learned) doc.resources.learned = { methods: {} };
            let partM = doc.resources.learned.methods[partKey];
            if (!partM) {
              doc.resources.learned.methods[partKey] = {
                id: `${interfaceName.replace(/\//g, ".")}.${partKey}`,
                path: url.pathname.substring(1),
                httpMethod: entry.method || "POST",
                parameters: {},
                request: null,
                response: null,
              };
              partM = doc.resources.learned.methods[partKey];
            }
            const schemaName = `${partKey}Response`;
            partM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              json,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
          } catch (_) {}
        }
      }
    } catch (e) {}
  } else if (isRSC(mimeType) || (mimeType === "" && looksLikeRSC(textBody))) {
    // React Server Components stream: line-framed `<id>:<payload>`. Each
    // json-typed row carries structured data; module-typed rows catalog
    // imported JS bundles (real endpoints the page will fetch). We learn
    // the shape of json rows (merged into one response schema) and record
    // module URLs so they show up in the endpoints list.
    try {
      const rsc = parseRSC(textBody);
      if (rsc && rsc.rows.length) {
        const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
        targetM.response = { $ref: schemaName };
        for (const row of rsc.rows) {
          if (row.type === "json" && row.value && typeof row.value === "object") {
            const newSchema = generateSchemaFromJson(row.value, schemaName, doc.schemas);
            mergeSchemaInto(doc, schemaName, newSchema);
          }
          if (row.type === "error" && Array.isArray(row.value) && typeof row.value[1] === "string") {
            extractKeysFromText(tabId, row.value[1], entry.url, "rsc_error");
          }
        }
        // Register module chunks as endpoints so the user sees the JS
        // bundles the page will fetch for this route. Same pattern as
        // AST-discovered call sites.
        try {
          for (const mod of rsc.modules) {
            if (!Array.isArray(mod.chunks)) continue;
            for (const chunk of mod.chunks) {
              if (typeof chunk !== "string" || !chunk.startsWith("/")) continue;
              const chunkHost = url.host;
              const epKey = `RSC GET ${chunkHost}${chunk}`;
              if (!tab.endpoints.has(epKey)) {
                tab.endpoints.set(epKey, {
                  url: url.origin + chunk,
                  method: "GET",
                  host: chunkHost,
                  path: chunk,
                  service: interfaceName,
                  source: "rsc_module",
                  pageUrl: entry.url,
                  firstSeen: Date.now(),
                });
              }
            }
          }
        } catch (_) {}
      }
    } catch (_) {}
  } else if (isGraphQLUrl(url.href) && mimeType.includes("json")) {
    // GraphQL response: extract data/errors structure
    try {
      const gqlResp = parseGraphQLResponse(textBody);
      if (gqlResp) {
        for (const r of gqlResp.results) {
          if (r.data) {
            const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
            targetM.response = { $ref: schemaName };
            const newSchema = generateSchemaFromJson(
              r.data,
              schemaName,
              doc.schemas,
            );
            mergeSchemaInto(doc, schemaName, newSchema);
          }
        }
      }
    } catch (e) {}
  } else if (mimeType.includes("json") || mimeType.includes("javascript")) {
    // JSON or JSONP (callback-wrapped JSON returned as text/javascript)
    try {
      var _lrText = textBody;
      if (!mimeType.includes("json")) {
        var _lrJsonp = stripJsonp(textBody);
        if (!_lrJsonp) throw new Error("not JSONP");
        _lrText = _lrJsonp;
      }
      // Strip Google XSSI prefix if present. Many Google (and now GitLab
      // snowplow, others) endpoints prepend `)]}'\n` to prevent <script>
      // JSON hijacking. JSON.parse would fail without this.
      if (_lrText.startsWith(")]}'")) _lrText = _lrText.replace(/^\)\]\}'[\r\n]*/, "");
      // Plain "ok" / "OK" confirmations aren't JSON — avoid learning a
      // schema from them.
      if (/^(ok|OK|true|false|null)\s*$/.test(_lrText)) throw new Error("non-object response");
      const json = JSON.parse(_lrText);
      const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
      targetM.response = { $ref: schemaName };
      const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
      mergeSchemaInto(doc, schemaName, newSchema);
    } catch (e) {}
  } else if (
    mimeType.includes("protobuf") ||
    entry.contentType?.includes("protobuf") ||
    mimeType.includes("octet-stream") ||
    entry.contentType?.includes("octet-stream")
  ) {
    // Decode response protobuf heuristically
    try {
      const bytes = entry.responseBase64
        ? base64ToUint8(entry.responseBody)
        : new TextEncoder().encode(entry.responseBody);
      const tree = pbDecodeTree(bytes, 8, (val) => {
        if (typeof val === "string") {
          extractKeysFromText(tabId, val, entry.url, "response_protobuf");
        }
      });
      const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
      targetM.response = { $ref: schemaName };
      const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
      mergeSchemaInto(doc, schemaName, newSchema);
    } catch (e) {}
  }

  // ─── Chain value indexing ─────────────────────────────────────────────────
  // Index response values so subsequent requests can detect chains
  if (tab._valueIndex && textBody) {
    const methodId = targetM.id || `${interfaceName.replace(/\//g, ".")}.${methodName}`;
    try {
      var _ciText = stripJsonp(textBody) || textBody;
      const parsed = JSON.parse(_ciText);
      indexResponseValues(tab._valueIndex, parsed, methodId);
    } catch (_) {
      // Not JSON/JSONP — index the raw text body if it looks like a useful value
      if (textBody.length >= 4 && textBody.length <= 500) {
        indexResponseValues(tab._valueIndex, textBody, methodId);
      }
    }
  }
}

function generateSchemaFromPbTree(tree, name, schemas) {
  // First pass: count field occurrences to detect repeated fields
  const fieldCounts = {};
  for (const node of tree) {
    fieldCounts[node.field] = (fieldCounts[node.field] || 0) + 1;
  }

  const properties = {};
  const seen = new Set();
  for (const node of tree) {
    const fieldKey = `field${node.field}`;
    if (seen.has(node.field)) {
      // Merge nested schemas from additional occurrences of repeated message fields
      if (node.message) {
        const nestedName = `${name}Field${node.field}`;
        if (schemas[nestedName]) {
          const additionalSchema = generateSchemaFromPbTree(node.message, nestedName, schemas);
          const existing = schemas[nestedName];
          if (!existing.properties) existing.properties = {};
          for (const [k, v] of Object.entries(additionalSchema.properties || {})) {
            if (!existing.properties[k]) {
              existing.properties[k] = v;
            }
          }
        }
      }
      continue;
    }
    seen.add(node.field);

    const isRepeated = fieldCounts[node.field] > 1 || !!node.isRepeatedScalar || !!node.packed;
    let wireType;

    // For JSPB-sourced nodes, infer type from the actual JS value
    // since wire codes are synthetic and less reliable
    if (node.isJspb) {
      const val = node.value;
      if (typeof val === "boolean") wireType = "bool";
      else if (typeof val === "number") wireType = Number.isInteger(val) ? "int64" : "double";
      else if (typeof val === "string") wireType = "string";
      else if (node.isRepeatedScalar && Array.isArray(val) && val.length > 0) {
        // Infer from first non-null element of repeated scalar
        const sample = val.find((v) => v != null);
        if (typeof sample === "boolean") wireType = "bool";
        else if (typeof sample === "number") wireType = Number.isInteger(sample) ? "int64" : "double";
        else wireType = "string";
      } else wireType = "string";
    } else if (node.packed) {
      // Packed repeated: values are varint-decoded numbers
      wireType = "int64";
    } else {
      // Binary protobuf wire type inference
      if (node.wire === 0) wireType = "int64";
      else if (node.wire === 5) wireType = "float";
      else if (node.wire === 1) wireType = "double";
      else if (node.string !== undefined) wireType = "string";
      else if (node.hex) wireType = "bytes";
      else wireType = "string";
    }

    const prop = {
      id: node.field,
      number: node.field,
      type: wireType,
      description: "Discovered via response capture",
    };

    if (isRepeated) {
      prop.type = "array";
      prop.items = { type: wireType };
    }

    if (node.message) {
      const nestedName = `${name}Field${node.field}`;
      if (isRepeated) {
        prop.items = { $ref: nestedName };
      } else {
        prop.type = "message";
        prop.$ref = nestedName;
      }
      schemas[nestedName] = generateSchemaFromPbTree(
        node.message,
        nestedName,
        schemas,
      );
    } else if (node.string !== undefined) {
      if (!isRepeated) prop.type = "string";
    }

    properties[fieldKey] = prop;
  }
  return { id: name, type: "object", properties };
}

function generateSchemaFromJson(json, name, schemas, isIndexed = false) {
  if (Array.isArray(json)) {
    if (isIndexed) {
      const properties = {};
      json.forEach((val, idx) => {
        const fieldNum = idx + 1;
        const fieldKey = `field${fieldNum}`;
        const nestedName = `${name}_f${fieldNum}`;

        if (val === null || val === undefined) {
          properties[fieldKey] = {
            id: fieldNum,
            number: fieldNum,
            type: "string",
            description: "Learned (null)",
          };
        } else if (Array.isArray(val)) {
          // Distinguish repeated scalars from nested JSPB messages:
          // - All primitives (string/number/bool/null) → repeated scalar
          // - Contains sub-arrays or objects → nested message
          const allPrim =
            val.length > 0 &&
            val.every(
              (v) =>
                v === null ||
                v === undefined ||
                typeof v === "string" ||
                typeof v === "number" ||
                typeof v === "boolean",
            );
          if (allPrim) {
            const itemType = inferRepeatedItemType(val);
            properties[fieldKey] = {
              id: fieldNum,
              number: fieldNum,
              type: itemType,
              label: "repeated",
            };
          } else {
            properties[fieldKey] = {
              id: fieldNum,
              number: fieldNum,
              $ref: nestedName,
            };
            schemas[nestedName] = generateSchemaFromJson(
              val,
              nestedName,
              schemas,
              true,
            );
          }
        } else if (typeof val === "object") {
          // Object within indexed array → nested named-key message
          properties[fieldKey] = {
            id: fieldNum,
            number: fieldNum,
            $ref: nestedName,
          };
          schemas[nestedName] = generateSchemaFromJson(
            val,
            nestedName,
            schemas,
            false,
          );
        } else {
          properties[fieldKey] = {
            id: fieldNum,
            number: fieldNum,
            type: inferJsonType(val),
          };
        }
      });
      return { id: name, type: "object", properties };
    } else {
      const items =
        json.length > 0
          ? generateSchemaFromJson(json[0], name + "Item", schemas, false)
          : { type: "string" };
      return { type: "array", items };
    }
  } else if (typeof json === "object" && json !== null) {
    const properties = {};
    for (const key in json) {
      const val = json[key];
      const safeKey = key.replace(/[^a-zA-Z0-9]/g, "");
      if (Array.isArray(val)) {
        properties[key] = {
          type: "array",
          items:
            val.length > 0
              ? generateSchemaFromJson(val[0], name + safeKey + "Item", schemas)
              : { type: "string" },
        };
      } else if (typeof val === "object" && val !== null) {
        const nestedName =
          name + safeKey.charAt(0).toUpperCase() + safeKey.slice(1);
        properties[key] = { $ref: nestedName };
        schemas[nestedName] = generateSchemaFromJson(val, nestedName, schemas);
      } else {
        properties[key] = { type: inferJsonType(val) };
      }
    }
    return { id: name, type: "object", properties };
  } else {
    return { type: inferJsonType(json) };
  }
}

/**
 * Infer a protobuf-style type from a JS value.
 * More precise than raw `typeof` — distinguishes int vs float, bool, etc.
 */
function inferJsonType(val) {
  if (val === null || val === undefined) return "string";
  if (typeof val === "boolean") return "bool";
  if (typeof val === "number") {
    return Number.isInteger(val) ? "int64" : "double";
  }
  if (typeof val === "string") return "string";
  return "string";
}

/** Infer the best scalar type for a repeated field from sample values. */
function inferRepeatedItemType(arr) {
  for (const v of arr) {
    if (v === null || v === undefined) continue;
    return inferJsonType(v);
  }
  return "string";
}

/**
 * Merge new schema properties into an existing schema, preserving custom renames
 * and enriching with new fields. Existing fields keep customName/name if set;
 * new fields or missing type info gets filled in from the new observation.
 */
function mergeSchemaInto(doc, schemaName, newSchema) {
  if (!doc.schemas[schemaName]) {
    doc.schemas[schemaName] = newSchema;
    return;
  }
  const existing = doc.schemas[schemaName];
  if (!existing.properties) existing.properties = {};
  if (!existing._drift) existing._drift = [];
  const newProps = newSchema.properties || {};

  // Build field-number → key index for deduplication
  const numToKey = {};
  for (const [k, p] of Object.entries(existing.properties)) {
    const n = p.number ?? p.id;
    if (n != null) numToKey[n] = k;
  }

  for (const [key, newProp] of Object.entries(newProps)) {
    // Match by key first, then fall back to field number
    const fieldNum = newProp.number ?? newProp.id;
    const matchKey = existing.properties[key] ? key
      : (fieldNum != null && numToKey[fieldNum]) ? numToKey[fieldNum]
      : null;
    const old = matchKey ? existing.properties[matchKey] : null;

    if (!old) {
      // Brand new field — add it
      existing.properties[key] = newProp;
      if (fieldNum != null) numToKey[fieldNum] = key;
      existing._drift.push({ type: "field_added", field: key, fieldType: newProp.type, timestamp: Date.now() });
    } else {
      // Re-key if matched by field number and the new key has a real name
      if (matchKey !== key && !old.customName && !/^field\d+$/.test(key)) {
        existing.properties[key] = old;
        delete existing.properties[matchKey];
        numToKey[fieldNum] = key;
      }
      // Merge: preserve custom names, upgrade types
      if (old.customName) {
        // Keep the user's rename
      } else if (newProp.name && !old.name) {
        old.name = newProp.name;
      }
      // Upgrade generic types with more specific ones
      if (newProp.type && newProp.type !== old.type) {
        if (old.type === "string" && newProp.type !== "string") {
          existing._drift.push({ type: "type_changed", field: key || matchKey, from: old.type, to: newProp.type, timestamp: Date.now() });
          old.type = newProp.type;
        }
        // int → double/float (observed fractional value refines integer assumption)
        else if (
          (old.type === "int64" || old.type === "int32") &&
          (newProp.type === "double" || newProp.type === "float")
        ) {
          existing._drift.push({ type: "type_changed", field: key || matchKey, from: old.type, to: newProp.type, timestamp: Date.now() });
          old.type = newProp.type;
        }
      }
      // Upgrade array item types
      if (old.type === "array" && newProp.items) {
        if (!old.items) {
          old.items = newProp.items;
        } else {
          if (old.items.type === "string" && newProp.items.type && newProp.items.type !== "string") {
            old.items.type = newProp.items.type;
          }
          if (newProp.items.$ref && !old.items.$ref) {
            old.items.$ref = newProp.items.$ref;
          }
        }
      }
      if (newProp.id != null && old.id == null) old.id = newProp.id;
      if (newProp.number != null && old.number == null)
        old.number = newProp.number;
      if (newProp.$ref && !old.$ref) {
        old.$ref = newProp.$ref;
        old.type = "message";
      }
      if (newProp.children && !old.children) old.children = newProp.children;
      if (newProp.description && !old.description) old.description = newProp.description;
      // Merge nested schema recursively
      if (newProp.$ref && doc.schemas[newProp.$ref]) {
        mergeSchemaInto(doc, newProp.$ref, doc.schemas[newProp.$ref]);
      }
    }
  }
  // Cap drift log at 50 entries per schema
  if (existing._drift.length > 50) existing._drift = existing._drift.slice(-50);
}

// ─── Page-Context Fetch Bridge ───────────────────────────────────────────────
// Routes fetch requests through the content script so they execute with the
// page's cookie jar and Origin. The content script shares the page's cookies,
// so the browser attaches them automatically. Targets a specific frameId when
// the request originated from an iframe (e.g. proxy.html).
//
// If the original tab/frame is unreachable, a minimized background window is
// opened to the initiator origin so the content script loads and carries the
// right cookies + Origin.

/**
 * Send a PAGE_FETCH message to a tab's content script.
 */
async function sendPageFetch(tabId, url, opts, frameId = 0) {
  return chrome.tabs.sendMessage(
    tabId,
    {
      type: "PAGE_FETCH",
      url,
      method: opts.method || "GET",
      headers: opts.headers || {},
      body: opts.body ?? null,
      bodyEncoding: opts.bodyEncoding || null,
    },
    { frameId: frameId ?? 0 },
  );
}

/**
 * Fetch through a content script on the original tab.
 * @param {number} frameId — target frame (0 = main frame, >0 = iframe)
 */
async function pageContextFetch(tabId, url, opts, frameId) {
  // Validate URL
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
      return { error: "blocked: invalid protocol" };
    }
  } catch (_) {
    return { error: "blocked: invalid URL" };
  }

  // Try the original tab's target frame
  if (tabId != null) {
    try {
      return await sendPageFetch(tabId, url, opts, frameId ?? 0);
    } catch (_) {}
  }

  return {
    error: "relay_failed: content script unreachable on tab " + tabId,
  };
}

/**
 * Create a fetchFn bound to a specific tab.
 */
function makePageFetchFn(tabId) {
  return (url, opts) => pageContextFetch(tabId, url, opts);
}

// ─── Discovery Document Fetching ─────────────────────────────────────────────

/**
 * Collect all API keys that have been seen with a specific service.
 */
function collectKeysForService(tab, service, hostname) {
  const keys = [];
  for (const [key, data] of tab.apiKeys) {
    if (data.services?.has(service) || data.hosts?.has(hostname)) {
      keys.push(key);
    }
  }
  return keys;
}

// ─── Discovery Document Diffing ──────────────────────────────────────────────

function _collectAllMethods(doc) {
  var result = {};
  if (!doc || !doc.resources) return result;
  function walk(resources, prefix) {
    for (var rName in resources) {
      var res = resources[rName];
      if (res.methods) {
        for (var mName in res.methods) {
          var id = (prefix ? prefix + "." : "") + rName + "." + mName;
          result[id] = res.methods[mName];
        }
      }
      // Recurse into sub-resources
      if (res.resources) walk(res.resources, (prefix ? prefix + "." : "") + rName);
    }
  }
  walk(doc.resources, "");
  return result;
}

function _diffDiscoveryDocs(oldDoc, newDoc) {
  if (!oldDoc || !newDoc) return null;
  var changes = [];

  var oldMethods = _collectAllMethods(oldDoc);
  var newMethods = _collectAllMethods(newDoc);

  // New methods
  for (var id in newMethods) {
    if (!oldMethods[id]) {
      changes.push({ type: "method_added", methodId: id, path: newMethods[id].path, httpMethod: newMethods[id].httpMethod });
    }
  }
  // Removed methods
  for (var id in oldMethods) {
    if (!newMethods[id]) {
      changes.push({ type: "method_removed", methodId: id, path: oldMethods[id].path, httpMethod: oldMethods[id].httpMethod });
    }
  }
  // Changed methods — compare parameters
  for (var id in newMethods) {
    if (!oldMethods[id]) continue;
    var oldM = oldMethods[id], newM = newMethods[id];
    var oldParams = Object.keys(oldM.parameters || {});
    var newParams = Object.keys(newM.parameters || {});

    for (var pi = 0; pi < newParams.length; pi++) {
      var p = newParams[pi];
      if (!oldM.parameters || !oldM.parameters[p]) {
        changes.push({ type: "param_added", methodId: id, param: p, details: newM.parameters[p] });
      }
    }
    for (var pi = 0; pi < oldParams.length; pi++) {
      var p = oldParams[pi];
      if (!newM.parameters || !newM.parameters[p]) {
        changes.push({ type: "param_removed", methodId: id, param: p });
      }
    }
    for (var pi = 0; pi < newParams.length; pi++) {
      var p = newParams[pi];
      if (!oldM.parameters || !oldM.parameters[p]) continue;
      var op = oldM.parameters[p], np = newM.parameters[p];
      if (op.type !== np.type) {
        changes.push({ type: "param_type_changed", methodId: id, param: p, from: op.type, to: np.type });
      }
      if (!op.required && np.required) {
        changes.push({ type: "param_required", methodId: id, param: p });
      }
    }
  }

  // Compare schemas — new/removed schema names
  var oldSchemas = Object.keys(oldDoc.schemas || {});
  var newSchemas = Object.keys(newDoc.schemas || {});
  for (var si = 0; si < newSchemas.length; si++) {
    if (oldSchemas.indexOf(newSchemas[si]) === -1) {
      changes.push({ type: "schema_added", schema: newSchemas[si] });
    }
  }
  for (var si = 0; si < oldSchemas.length; si++) {
    if (newSchemas.indexOf(oldSchemas[si]) === -1) {
      changes.push({ type: "schema_removed", schema: oldSchemas[si] });
    }
  }

  return changes.length > 0 ? changes : null;
}

/**
 * Fetch discovery document for a service, trying multiple API keys.
 * Some discovery documents only load with the correct API key.
 *
 * @param {number} tabId
 * @param {string} service
 * @param {string} hostname
 * @param {string[]} apiKeys - All API keys to try for this service
 */
async function fetchDiscoveryForService(
  tabId,
  service,
  hostname,
  apiKeys,
  seedUrl,
) {
  const tab = getTab(tabId);

  const fetchFn = makePageFetchFn(tabId);
  const triedKeys = new Set();

  // Build a deduplicated candidate list across all keys
  // Try each key separately to track which one works
  const keysToTry = [...new Set(apiKeys || [])];

  // Always try without key as a fallback (some public APIs don't need one)
  if (!keysToTry.includes(null)) keysToTry.push(null);

  for (const apiKey of keysToTry) {
    if (apiKey) triedKeys.add(apiKey);
    const candidates = buildDiscoveryUrls(hostname, apiKey);

    for (const { url, headers, method } of candidates) {
      try {
        const resp = await fetchFn(url, { method: method || "GET", headers });

        if (resp.error || !resp.ok) continue;

        let doc;
        try {
          doc = JSON.parse(resp.body);
        } catch (_) {
          continue;
        }

        if (
          doc &&
          (doc.discoveryVersion ||
            doc.kind === "discovery#restDescription" ||
            doc.openapi ||
            doc.swagger)
        ) {
          let unifiedDoc = doc;
          if (doc.openapi || doc.swagger) {
            unifiedDoc = convertOpenApiToDiscovery(doc, url);
          }

          const existingEntry = tab.discoveryDocs.get(service);

          // Diff before merge overwrites — track API surface changes
          if (existingEntry?.doc) {
            var diff = _diffDiscoveryDocs(existingEntry.doc, unifiedDoc);
            if (diff) {
              var dcList = globalStore.discoveryChanges.get(service) || [];
              dcList.push({ timestamp: Date.now(), fetchUrl: url, changes: diff });
              if (dcList.length > 20) dcList = dcList.slice(-20);
              globalStore.discoveryChanges.set(service, dcList);
              console.debug("[Discovery:diff] %d changes detected for %s", diff.length, service);
            }
          }

          const mergedDoc = mergeVirtualParts(unifiedDoc, existingEntry?.doc);

          var _prevDiscovery = tab.discoveryDocs.get(service);
          tab.discoveryDocs.set(service, {
            status: "found",
            doc: mergedDoc,
            url,
            method: method || "GET",
            apiKey: apiKey || null,
            fetchedAt: Date.now(),
            pageUrls: _prevDiscovery?.pageUrls || new Set(),
            frameOrigins: _prevDiscovery?.frameOrigins || new Set(),
          });
          mergeToGlobal(tab);

          // Check if the seedUrl method is actually in the doc.
          // If not, trigger immediate hybrid probe to patch it.
          if (seedUrl) {
            const seedUrlObj = new URL(seedUrl);
            const match = findDiscoveryMethod(doc, seedUrlObj.pathname, "POST");
            if (!match) {
              notifyPopup(tabId);
              await performProbeAndPatch(tabId, service, seedUrl, apiKey);
              return;
            }
          }

          notifyPopup(tabId);
          return;
        }
      } catch (err) {
        // continue to next candidate
      }
    }
  }

  // All keys (including null) failed.
  // FALLBACK: Try req2proto probing if we have a seed URL.
  const currentStatus = tab.discoveryDocs.get(service);
  const finalSeedUrl = seedUrl || currentStatus?.seedUrl;

  if (finalSeedUrl) {
    // Pick a key to try probing with (use the first available one if any)
    const probeKey = keysToTry[0] || null;
    await performProbeAndPatch(tabId, service, finalSeedUrl, probeKey);
  } else {
    // If we get here, truly not found — record timestamp for cooldown
    var _prevDiscoveryNF = tab.discoveryDocs.get(service);
    tab.discoveryDocs.set(service, {
      status: "not_found",
      _triedKeys: triedKeys,
      _failedAt: Date.now(),
      pageUrls: _prevDiscoveryNF?.pageUrls || new Set(),
      frameOrigins: _prevDiscoveryNF?.frameOrigins || new Set(),
    });
    mergeToGlobal(tab);
    notifyPopup(tabId);
  }
}

// Track in-flight probes to prevent concurrent duplicates
const _inflight = new Set();

/**
 * Perform req2proto probing and patch the discovery document.
 */
async function performProbeAndPatch(tabId, service, targetUrl, apiKey) {
  // Deduplicate: skip if already probing this service+url combo
  const probeKey = `${service}::${targetUrl}`;
  if (_inflight.has(probeKey)) return;
  _inflight.add(probeKey);

  const tab = getTab(tabId);

  if (typeof probeApiEndpoint === "undefined") {
    console.error("[Debug] CRITICAL: probeApiEndpoint is not defined!");
    _inflight.delete(probeKey);
    return;
  }

  const fetchFn = makePageFetchFn(tabId);

  const probeHeader = apiKey ? { "x-goog-api-key": apiKey } : {};

  // Add #_internal_probe fragment to avoid interception loop
  const safeTargetUrl = targetUrl + "#_internal_probe";

  try {
    const probeResult = await probeApiEndpoint(safeTargetUrl, probeHeader, {
      fetchFn,
    });

    if (probeResult && probeResult.fields) {
      // Save raw probe result for observability. Previously only the
      // UI-triggered on-demand probe stored into tab.probeResults — the
      // auto-probe kept its output only in the synthesized virtual doc,
      // so consumers auditing "what did probing learn" saw an empty map.
      // Keyed by service::url so repeat probes overwrite cleanly.
      tab.probeResults.set(`auto:${service}::${targetUrl}`, probeResult);

      // Convert probe result to a "Virtual" Discovery Doc
      // Merge with existing if available
      const currentStatus = tab.discoveryDocs.get(service);
      const existingDoc = currentStatus?.doc ? currentStatus.doc : null;

      const virtualDoc = updateOrCreateVirtualDoc(
        service,
        targetUrl,
        probeResult,
        existingDoc,
      );

      var _prevProbed = tab.discoveryDocs.get(service);
      tab.discoveryDocs.set(service, {
        status: "found", // Treat as found so it shows up in UI
        doc: virtualDoc,
        apiKey: apiKey,
        fetchedAt: Date.now(),
        method: existingDoc ? currentStatus.method || "HYBRID" : "PROBE",
        isVirtual: existingDoc ? currentStatus.isVirtual || false : true,
        pageUrls: _prevProbed?.pageUrls || new Set(),
        frameOrigins: _prevProbed?.frameOrigins || new Set(),
      });
      mergeToGlobal(tab);
      notifyPopup(tabId);
    }
  } catch (probeErr) {
    console.error("Probe fallback failed:", probeErr);
  } finally {
    _inflight.delete(probeKey);
  }
}

function updateOrCreateVirtualDoc(service, seedUrl, probeResult, existingDoc) {
  const u = new URL(seedUrl);
  const origin = `${u.protocol}//${u.host}`;
  const fullPath = u.pathname.substring(1); // remove leading /

  const { methodName, methodId } = calculateMethodMetadata(u, service);
  const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
  const responseSchemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;

  let doc = existingDoc
    ? JSON.parse(JSON.stringify(existingDoc))
    : {
        kind: "discovery#restDescription",
        name: service,
        version: "v1",
        title: `${service} (Probed)`,
        description: "Auto-generated from req2proto probe",
        rootUrl: origin + "/",
        servicePath: "",
        baseUrl: origin + "/", // We use root as base, and full paths for methods
        resources: {},
        schemas: {},
      };

  // Ensure resources structure
  if (!doc.resources) doc.resources = {};
  if (!doc.resources.probed) {
    doc.resources.probed = { methods: {} };
  }

  // Ensure schemas structure
  if (!doc.schemas) doc.schemas = {};

  // Heuristic for response schema: try GetAsyncDataResponse, AsyncDataResponse, etc.
  const responseCandidates = [
    responseSchemaName,
    schemaName.replace("Request", "Response"),
    methodName.charAt(0).toUpperCase() + methodName.slice(1) + "Response",
    methodName.replace(/^Get/, "") + "Response",
    methodName.replace(/^BatchGet/, "") + "Response",
  ];

  let actualResponseRef = responseSchemaName;
  if (doc.schemas) {
    for (const cand of responseCandidates) {
      if (doc.schemas[cand]) {
        actualResponseRef = cand;

        break;
      }
    }
  }

  // Add/Update Method
  doc.resources.probed.methods[methodName] = {
    id: methodId,
    path: fullPath,
    httpMethod: "POST", // Assumed
    description: `Probed endpoint: ${fullPath}`,
    parameters: {},
    request: { $ref: schemaName },
    response: { $ref: actualResponseRef },
  };

  // Create/Merge Schema for Request recursively
  const newProperties = convertProbeFieldsToSchema(
    probeResult.fields,
    doc.schemas,
    schemaName,
  );

  // Merge probe properties into request and response schemas.
  // Probe data has verified field numbers/types — prefer it over learned data,
  // but always preserve user's customName renames.
  function mergeProbeInto(target, probeProps) {
    if (!target.properties) target.properties = {};
    // Build field-number → key index for deduplication
    const numToKey = {};
    for (const [k, p] of Object.entries(target.properties)) {
      const n = p.number ?? p.id;
      if (n != null) numToKey[n] = k;
    }
    for (const [key, probeProp] of Object.entries(probeProps)) {
      const fieldNum = probeProp.number ?? probeProp.id;
      const matchKey = target.properties[key] ? key
        : (fieldNum != null && numToKey[fieldNum]) ? numToKey[fieldNum]
        : null;
      const existing = matchKey ? target.properties[matchKey] : null;
      if (!existing) {
        target.properties[key] = probeProp;
        if (fieldNum != null) numToKey[fieldNum] = key;
      } else {
        // Re-key: probe has the real name, replace generic fieldN key
        if (matchKey !== key && !existing.customName && !/^field\d+$/.test(key)) {
          target.properties[key] = existing;
          delete target.properties[matchKey];
          numToKey[fieldNum] = key;
        }
        // Probe has authoritative field numbers and types
        if (probeProp.id != null) existing.id = probeProp.id;
        if (probeProp.number != null) existing.number = probeProp.number;
        if (probeProp.type && existing.type === "string" && probeProp.type !== "string") {
          existing.type = probeProp.type;
        }
        if (probeProp.$ref && !existing.$ref) existing.$ref = probeProp.$ref;
        if (probeProp.children && !existing.children) existing.children = probeProp.children;
        if (probeProp.description && !existing.description) existing.description = probeProp.description;
        // Preserve user renames
        if (existing.customName) {
          // keep existing.name
        } else if (probeProp.name) {
          existing.name = probeProp.name;
        }
      }
    }
  }

  if (!doc.schemas[schemaName]) {
    doc.schemas[schemaName] = {
      id: schemaName,
      type: "object",
      properties: newProperties,
    };
  } else {
    mergeProbeInto(doc.schemas[schemaName], newProperties);
  }

  if (!doc.schemas[actualResponseRef]) {
    doc.schemas[actualResponseRef] = {
      id: actualResponseRef,
      type: "object",
      properties: newProperties,
    };
  } else {
    mergeProbeInto(doc.schemas[actualResponseRef], newProperties);
  }

  return doc;
}

function convertProbeFieldsToSchema(fieldsObj, schemas, prefix = "") {
  const properties = {};
  const fields = Array.isArray(fieldsObj)
    ? fieldsObj
    : fieldsObj instanceof Map
      ? [...fieldsObj.values()]
      : Object.values(fieldsObj || {});

  for (const field of fields) {
    // Discovery format property
    const prop = {
      id: field.number,
      number: field.number,
      name: field.name,
      type: field.type || "string",
      description: `Field ${field.number} (${field.type || "unknown"})`,
    };

    if (field.label === "repeated") {
      prop.type = "array";
      prop.items = { type: field.type || "string" };

      if (field.type === "message" && field.children) {
        // Use messageType as key if available, otherwise generate
        const nestedName =
          field.messageType ||
          `${prefix}${field.name.charAt(0).toUpperCase() + field.name.slice(1)}Entry`;

        if (!schemas[nestedName]) {
          const nestedProperties = convertProbeFieldsToSchema(
            field.children,
            schemas,
            nestedName,
          );
          schemas[nestedName] = {
            id: nestedName,
            type: "object",
            properties: nestedProperties,
          };
        }
        prop.items.$ref = nestedName;
        prop.items.children = schemas[nestedName].properties;
        delete prop.items.type;
      }
    } else if (field.type === "message" && field.children) {
      const nestedName =
        field.messageType ||
        `${prefix}${field.name.charAt(0).toUpperCase() + field.name.slice(1)}`;

      if (!schemas[nestedName]) {
        const nestedProperties = convertProbeFieldsToSchema(
          field.children,
          schemas,
          nestedName,
        );
        schemas[nestedName] = {
          id: nestedName,
          type: "object",
          properties: nestedProperties,
        };
      }
      prop.$ref = nestedName;
      prop.children = schemas[nestedName].properties;
      delete prop.type;
    }

    const fieldKey = field.name || `field_${field.number}`;
    properties[fieldKey] = prop;
  }
  return properties;
}

// ─── req2proto Fallback Probing ──────────────────────────────────────────────

async function probeEndpoint(tabId, endpointKey) {
  const tab = getTab(tabId);
  const ep = tab.endpoints.get(endpointKey);
  if (!ep || ep.method !== "POST") return null;

  // Pass the API key the same way it was originally sent (URL param vs header).
  // Cookie, Origin, Referer are handled by the browser via the content script relay.
  const headers = {};
  const probeUrl = new URL(ep.url);
  probeUrl.searchParams.delete("key");

  if (ep.apiKey) {
    if (ep.apiKeySource === "url") {
      probeUrl.searchParams.set("key", ep.apiKey);
    } else {
      headers["X-Goog-Api-Key"] = ep.apiKey;
    }
  }

  const fetchFn = makePageFetchFn(tabId);
  const result = await probeApiEndpoint(probeUrl.toString(), headers, {
    fetchFn,
  });
  tab.probeResults.set(endpointKey, result);

  // Store scopes if the probe discovered them
  if (result.scopes?.length) {
    const svc = ep.service || extractInterfaceName(new URL(ep.url));
    tab.scopes.set(svc, result.scopes);
  }

  mergeToGlobal(tab);
  notifyPopup(tabId);
  return result;
}

// ─── Request/Response Handling (from intercept.js via content.js relay) ──────

async function handleResponseBody(tabId, msg, frameId) {
  if (!msg.url) return;
  await _globalStoreReady;

  // Normalize channel ID from relay messages
  const channelId = msg.channelId || msg.wsId;

  // WebSocket lifecycle: one log entry per connection, messages[] array
  const isWs = msg.method === "WS_OPEN" || msg.method === "WS_CLOSE" ||
    msg.method === "WS_SEND" || msg.method === "WS_RECV";
  if (isWs) {
    const tab = getTab(tabId);
    if (!_wsConnState.has(tabId)) _wsConnState.set(tabId, new Map());
    const conns = _wsConnState.get(tabId);

    if (msg.method === "WS_OPEN") {
      // Create one combined log entry for this connection
      const entry = {
        id: "ws_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "WEBSOCKET",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        wsOpen: true,
        messages: [],
      };
      tab.requestLog.unshift(entry);
      _trimRequestLog(tab);
      conns.set(channelId, { url: msg.url, readyState: 1, entryId: entry.id });
      scheduleSessionSave(tabId);
      notifyPopup(tabId);
      return;
    }

    if (msg.method === "WS_CLOSE") {
      const conn = conns.get(channelId);
      if (conn) conn.readyState = 3;
      // Mark the log entry as closed
      const entry = tab.requestLog.find((r) => r.channelId === channelId && r.method === "WEBSOCKET");
      if (entry) {
        entry.wsOpen = false;
        entry.messages.push({
          dir: "close",
          time: Date.now(),
          body: msg.body || "",
          base64: false,
          status: msg.status || 1000,
        });
        // Cap messages to prevent storage bloat
        if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);
      }
      scheduleSessionSave(tabId);
      notifyPopup(tabId);
      return;
    }

    // WS_SEND or WS_RECV — append message to existing connection entry
    let entry = tab.requestLog.find((r) => r.channelId === channelId && r.method === "WEBSOCKET");
    if (!entry) {
      // WS was opened before extension injected, or after SW restart — create entry now
      entry = {
        id: "ws_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "WEBSOCKET",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        wsOpen: true,
        messages: [],
      };
      tab.requestLog.unshift(entry);
      _trimRequestLog(tab);
      conns.set(channelId, { url: msg.url, readyState: 1, entryId: entry.id });
    }

    entry.messages.push({
      dir: msg.method === "WS_SEND" ? "sent" : "recv",
      time: Date.now(),
      body: msg.body || "",
      base64: msg.base64Encoded || false,
    });
    entry.timestamp = Date.now(); // Bump to keep it near top in sorted views
    // Cap messages to prevent storage bloat
    if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);

    // Key scanning on message body
    if (msg.body) {
      let textBody = msg.body;
      if (msg.base64Encoded) {
        try { textBody = new TextDecoder().decode(base64ToUint8(msg.body)); }
        catch (_) { textBody = null; }
      }
      if (textBody) extractKeysFromText(tabId, textBody, msg.url, "response_body");
    }

    scheduleSessionSave(tabId);
    notifyPopup(tabId);
    return;
  }

  // postMessage: one log entry per source origin, messages[] array
  // Only PM_RECV — can't wrap window.postMessage (see intercept.js comments)
  if (msg.method === "PM_RECV") {
    const tab = getTab(tabId);
    let entry = tab.requestLog.find((r) => r.channelId === channelId && r.method === "POSTMESSAGE");
    if (!entry) {
      entry = {
        id: "pm_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "POSTMESSAGE",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        sourceOrigin: msg.sourceOrigin || "",
        targetOrigin: msg.targetOrigin || "",
        messages: [],
      };
      tab.requestLog.unshift(entry);
      _trimRequestLog(tab);
    }

    entry.messages.push({
      dir: "recv",
      time: Date.now(),
      body: msg.body || "",
      base64: false,
    });
    entry.timestamp = Date.now();
    if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);

    // Key scanning on message body
    if (msg.body) {
      extractKeysFromText(tabId, msg.body, msg.url, "response_body");
    }

    scheduleSessionSave(tabId);
    notifyPopup(tabId);
    return;
  }

  // MessageChannel: MC_OPEN creates entry, MC_RECV appends messages
  if (msg.method === "MC_OPEN") {
    const tab = getTab(tabId);
    let entry = tab.requestLog.find((r) => r.channelId === channelId && r.method === "MSGCHANNEL");
    if (!entry) {
      entry = {
        id: "mc_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "MSGCHANNEL",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        sourceOrigin: msg.sourceOrigin || "",
        targetOrigin: msg.targetOrigin || "",
        messages: [],
      };
      tab.requestLog.unshift(entry);
      _trimRequestLog(tab);
    }
    scheduleSessionSave(tabId);
    notifyPopup(tabId);
    return;
  }

  if (msg.method === "MC_RECV") {
    const tab = getTab(tabId);
    let entry = tab.requestLog.find((r) => r.channelId === channelId && r.method === "MSGCHANNEL");
    if (!entry) {
      // Port message arrived before MC_OPEN (race) — create entry
      entry = {
        id: "mc_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
        url: msg.url,
        method: "MSGCHANNEL",
        service: extractInterfaceName(new URL(msg.url)),
        timestamp: Date.now(),
        status: 0,
        channelId: channelId,
        sourceOrigin: "",
        targetOrigin: "",
        messages: [],
      };
      tab.requestLog.unshift(entry);
      _trimRequestLog(tab);
    }
    entry.messages.push({
      dir: "recv",
      time: Date.now(),
      body: msg.body || "",
      base64: false,
    });
    entry.timestamp = Date.now();
    if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);

    if (msg.body) {
      extractKeysFromText(tabId, msg.body, msg.url, "response_body");
    }

    scheduleSessionSave(tabId);
    notifyPopup(tabId);
    return;
  }

  // ─── SSE: streaming events, no request data ─────────────────────────────
  if (msg.method === "SSE") {
    if (!msg.body) return;
    const tab = getTab(tabId);
    const entry = {
      id: "alt_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
      url: msg.url,
      method: "SSE",
      service: extractInterfaceName(new URL(msg.url)),
      timestamp: Date.now(),
      status: msg.status || 200,
      responseBody: msg.body,
      responseBase64: msg.base64Encoded || false,
      mimeType: msg.contentType || "",
      responseHeaders: msg.responseHeaders || {},
    };
    tab.requestLog.unshift(entry);
    _trimRequestLog(tab);
    if (msg.body) {
      extractKeysFromText(tabId, msg.body, msg.url, "response_body");
    }
    learnFromResponse(tabId, entry.service, entry);
    scheduleSessionSave(tabId);
    notifyPopup(tabId);
    return;
  }

  // ─── HTTP (fetch / XHR): unified request + response ─────────────────────

  // Non-network URL schemes. Page-local blobs/data URIs go through fetch()
  // but produce an empty hostname, which breaks service grouping ("" bucket)
  // and isn't reverse-engineerable API traffic.
  if (/^(blob|data|file|chrome(-extension)?|about):/i.test(msg.url)) return;

  const url = new URL(msg.url);

  // Filter internal extension requests
  if (url.hash.includes("_uasr_send")) return;
  if (url.hash.includes("_internal_probe")) return;

  // Static-asset filtering is now CONTENT-based (see classifyResponseAsset
  // below). URL extensions do not decide API-vs-asset here — an API endpoint
  // at /users/42/avatar.png that returns JSON metadata is still an API, and
  // a dynamic endpoint like /models/duck.glb returning a binary 3D model is
  // still an asset. The body's magic bytes are the source of truth.

  // Filter telemetry/tracking noise
  const noisePaths = ["/gen_204", "/client_204", "/jserror", "/ulog", "/log", "/error", "/collect"];
  if (noisePaths.some((p) => url.pathname.includes(p))) return;

  // Skip internal probe requests
  if (url.searchParams.has("_probe")) return;

  const tab = getTab(tabId);
  let service = extractInterfaceName(url);

  // Build request header map from intercept.js capture
  const headerMap = msg.requestHeaders || {};

  // Key scanning: URL + request headers. Record the SPECIFIC header name
  // (lowercased) so on replay we can re-emit the key in the same location
  // instead of defaulting to X-Goog-Api-Key on every non-Google host.
  extractKeysFromText(tabId, msg.url, msg.url, "url");
  for (const [k, v] of Object.entries(headerMap)) {
    extractKeysFromText(tabId, `${k}: ${v}`, msg.url, "header:" + k.toLowerCase());
  }

  // Extract key header values
  let authorization = null, cookie = null, contentType = null;
  let origin = null, referer = null, apiKey = null;
  for (const [name, value] of Object.entries(headerMap)) {
    const lname = name.toLowerCase();
    if (lname === "cookie") { cookie = "[PRESENT]"; headerMap[lname] = "[REDACTED]"; }
    if (lname === "authorization") authorization = value;
    if (lname === "origin") origin = value;
    if (lname === "referer") referer = value;
    if (lname === "content-type") contentType = value;
    if (lname === "x-goog-api-key" || lname === "x-api-key" || lname === "apikey") apiKey = value;
  }

  // Compute rawBodyB64 from request body
  let rawBodyB64 = null;
  if (msg.requestBody) {
    if (msg.requestBodyBase64) {
      rawBodyB64 = msg.requestBody;
    } else {
      rawBodyB64 = uint8ToBase64(new TextEncoder().encode(msg.requestBody));
    }
  }

  // Create entry atomically — request + response together
  const entry = {
    id: "http_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
    url: msg.url,
    method: msg.method,
    service: service,
    timestamp: Date.now(),
    status: msg.status,
    completedAt: Date.now(),
    requestHeaders: headerMap,
    contentType: contentType || "",
    rawBodyB64: rawBodyB64,
    responseBody: msg.body || null,
    responseBase64: msg.base64Encoded || false,
    mimeType: msg.contentType || "",
    responseHeaders: msg.responseHeaders || {},
    frameId: frameId ?? 0,
  };

  // Update lastSeen on matching endpoint
  var _epKey = entry.method + " " + url.hostname + url.pathname;
  var _ep = tab.endpoints.get(_epKey);
  if (_ep) _ep.lastSeen = Date.now();

  // Update auth context
  if (authorization || cookie) {
    tab.authContext = tab.authContext || {};
    if (authorization) tab.authContext.hasAuthorization = true;
    if (cookie) tab.authContext.hasCookies = true;
    if (origin) tab.authContext.origin = origin;
  }

  // Key scanning on response body
  if (msg.body) {
    let textBody = msg.body;
    if (msg.base64Encoded) {
      try { textBody = new TextDecoder().decode(base64ToUint8(msg.body)); }
      catch (_) { textBody = null; }
    }
    if (textBody) extractKeysFromText(tabId, textBody, msg.url, "response_body");
  }

  // Classify the captured response purely by magic bytes (no URL extension,
  // no content-type). Three buckets result:
  //   structured API → full learning (schema, methods, discovery probe)
  //   image API     → learn URL + request body + auth; skip response schema;
  //                    create method entry (user can inspect/replay)
  //   boring asset  → log + key-extract + endpoint tracking ONLY; no method
  //                    entry, no discovery probe, no response schema
  //
  // "Boring" = static asset with zero dynamic signals. Any one of (query
  // string, auth header, request body, non-GET) promotes it to image API.
  // Keeps a signed-URL photo or avatar-generation endpoint visible as an
  // API while preventing 124 synthetic "methods" for HLS video segments
  // or a hash-busted CDN .glb file polluting the discovery doc.
  //
  // Nothing is ever hidden from the log — every request, including boring
  // CDN fetches, is captured and surfaced to the user. The bucket only
  // decides how much schema to synthesize around it.
  const _assetClass = classifyResponseAsset(msg.body, msg.base64Encoded, {
    responseType: msg.responseType || null,
    responseContentType: (msg.responseHeaders && (msg.responseHeaders["content-type"] || msg.responseHeaders["Content-Type"])) || null,
  });
  entry._assetKind = _assetClass.kind;     // "asset" | "empty" | "api"
  entry._assetLabel = _assetClass.label;   // e.g. "image/png" when asset
  const _isAsset = _assetClass.kind === "asset";
  const _isBoringFetch = _isAsset &&
    msg.method === "GET" &&
    !url.search &&
    !rawBodyB64 &&
    !authorization &&
    !cookie &&
    !apiKey;
  entry._boring = _isBoringFetch;

  // Snapshot discovery status before learnFromRequest (which creates a virtual doc)
  const preLearnDiscovery = tab.discoveryDocs.get(service);

  // Decode request body (protobuf/JSPB/JSON) — must happen BEFORE
  // learnFromRequest so entry.isJson / entry.decodedBody are set when
  // schema learning records body-field stats. Downstream code further
  // in this function (probing trigger, chain analysis) also reads
  // these fields, so the single decode here covers both.
  const logContentType = contentType || "";
  const isProtobuf = logContentType.includes("protobuf") || url.pathname.includes("$rpc");
  if (rawBodyB64) {
    try {
      const bytes = base64ToUint8(rawBodyB64);
      if (isProtobuf) {
        if (logContentType.includes("json") || logContentType.includes("text")) {
          try {
            const text = new TextDecoder().decode(bytes);
            if (text.trim().startsWith("[")) {
              const json = JSON.parse(text);
              if (Array.isArray(json)) {
                entry.decodedBody = jspbToTree(json);
                entry.isJspb = true;
              }
            }
          } catch (_) {}
        } else {
          entry.decodedBody = pbDecodeTree(bytes, 8, (val) => {
            if (typeof val === "string") {
              extractKeysFromText(tabId, val, msg.url, "protobuf_body");
            }
          });
        }
      } else if (logContentType.includes("x-www-form-urlencoded")) {
        try {
          const text = new TextDecoder().decode(bytes);
          const params = new URLSearchParams(text);
          const fReq = params.get("f.req");
          if (fReq) {
            const json = JSON.parse(fReq);
            if (Array.isArray(json)) {
              entry.decodedBody = jspbToTree(json);
              entry.isJspb = true;
            }
          }
        } catch (_) {}
      } else if (logContentType.includes("json")) {
        try {
          const text = new TextDecoder().decode(bytes);
          const json = JSON.parse(text);
          if (json && typeof json === "object") {
            entry.decodedBody = json;
            entry.isJson = true;
          }
        } catch (_) {}
      }
    } catch (_) {}
  }

  // Learn from request — skipped only for "boring" fetches. Image APIs
  // (any dynamic signal present) still learn URL + request body + auth so
  // they can be replayed and inspected.
  if (!_isBoringFetch) {
    learnFromRequest(tabId, service, entry, headerMap);

    // learnFromRequest may migrate the service (e.g. hostname-fallback →
    // path-common-prefix) when observed-prefix clustering promotes the
    // bucket. Use the post-migration name for all downstream lookups —
    // our pre-migration `service` may now point at a bucket that was
    // emptied and deleted during the migration.
    if (entry.interfaceName && entry.interfaceName !== service) {
      service = entry.interfaceName;
    }

    // If the response was binary media (and it's a real endpoint), annotate
    // the method entry with the detected media type so the consumer knows
    // not to expect a JSON/protobuf response schema.
    if (_isAsset && entry.methodId) {
      const _mid = entry.methodId;
      const _methodName = _mid.slice(_mid.lastIndexOf(".") + 1);
      const _methods = tab.discoveryDocs.get(service)?.doc?.resources?.learned?.methods;
      if (_methods && _methods[_methodName]) {
        _methods[_methodName]._responseKind = "asset";
        _methods[_methodName]._responseLabel = _assetClass.label;
      }
    }
  }
  mergeToGlobal(tab);

  // Track which pages/origins this service has been used from.
  var _svcDocEntry = tab.discoveryDocs.get(service);
  if (_svcDocEntry) {
    if (!_svcDocEntry.pageUrls) _svcDocEntry.pageUrls = new Set();
    var _svcFrameInfo = frameId != null ? _tabFrames.get(tabId)?.get(frameId) : null;
    // pageUrls always tracks the top-level page URL
    var _svcMeta = _tabMeta.get(tabId);
    if (_svcMeta?.url) _svcDocEntry.pageUrls.add(_svcMeta.url);
    if (_svcFrameInfo && !_svcFrameInfo.isTop) {
      // Request came from an iframe — record iframe origin separately
      if (_svcFrameInfo.origin) {
        if (!_svcDocEntry.frameOrigins) _svcDocEntry.frameOrigins = new Set();
        _svcDocEntry.frameOrigins.add(_svcFrameInfo.origin);
      }
    }
  }

  // Protobuf probing trigger — skip for boring asset fetches.
  if (!_isBoringFetch && isProtobuf && msg.method === "POST") {
    const discoveryStatus = tab.discoveryDocs.get(service);
    const doc = discoveryStatus?.doc;
    const match = doc ? findDiscoveryMethod(doc, url.pathname, msg.method) : null;
    const isLearnedOnly = match &&
      discoveryStatus.doc.resources?.learned?.methods[match.method.id.split(".").pop()];
    if (!match || isLearnedOnly) {
      const keysForService = collectKeysForService(tab, service, url.hostname);
      if (apiKey && !keysForService.includes(apiKey)) keysForService.push(apiKey);
      performProbeAndPatch(tabId, service, msg.url, apiKey || keysForService[0] || null);
    }
  }

  // Automatic background discovery — skip for boring fetches. Probing
  // /.well-known/openapi.json on a CDN is wasted traffic.
  const notFoundCooldown = preLearnDiscovery?.status === "not_found" &&
    preLearnDiscovery._failedAt && (Date.now() - preLearnDiscovery._failedAt < 300000);
  if (!_isBoringFetch && !notFoundCooldown && (!preLearnDiscovery || preLearnDiscovery.status === "not_found")) {
    const discoveryStatus = tab.discoveryDocs.get(service);
    if (discoveryStatus) {
      discoveryStatus.status = "pending";
    } else {
      tab.discoveryDocs.set(service, { status: "pending", seedUrl: msg.url });
    }
    const keysForService = collectKeysForService(tab, service, url.hostname);
    if (apiKey && !keysForService.includes(apiKey)) keysForService.push(apiKey);
    fetchDiscoveryForService(tabId, service, url.hostname, keysForService, msg.url);
  }

  // Extract OAuth scopes from 403 www-authenticate response header
  if (msg.status === 403 && msg.responseHeaders) {
    const wwwAuth = msg.responseHeaders["www-authenticate"];
    if (wwwAuth) {
      const scopeMatch = wwwAuth.match(/scope="([^"]*)"/);
      if (scopeMatch) {
        const scopeList = scopeMatch[1].split(/\s+/).filter(Boolean);
        if (scopeList.length > 0) {
          tab.scopes.set(service, scopeList);
          const endpointKey = `${msg.method} ${url.hostname}${url.pathname}`;
          const ep = tab.endpoints.get(endpointKey);
          if (ep) ep.requiredScopes = scopeList;
        }
      }
    }
  }

  // Add to request log
  tab.requestLog.unshift(entry);
  _trimRequestLog(tab);

  // Learn from response — skip for static assets.
  if (entry.responseBody && !_isAsset) {
    learnFromResponse(tabId, service, entry);
  }

  mergeToGlobal(tab);
  scheduleSessionSave(tabId);
  notifyPopup(tabId);
}

// ─── Cross-Script AST Buffering ──────────────────────────────────────────────

function _bufferScript(tabId, scriptUrl, code, pageUrl) {
  var buf = _scriptBuffers.get(tabId);
  if (!buf) {
    buf = { scripts: [], timer: null, pageUrl: pageUrl };
    _scriptBuffers.set(tabId, buf);
  }

  // Detect navigation: if page URL changed, clear old buffer
  if (pageUrl && buf.pageUrl && pageUrl !== buf.pageUrl) {
    if (buf.timer) clearTimeout(buf.timer);
    buf.scripts = [];
    buf.pageUrl = pageUrl;
    console.debug("[AST:buffer] Navigation detected, cleared buffer for tab=%d", tabId);
  }
  if (pageUrl) buf.pageUrl = pageUrl;

  // Deduplicate by URL or content hash
  var key = scriptUrl || _hashScriptCode(code);
  for (var i = 0; i < buf.scripts.length; i++) {
    if (buf.scripts[i].key === key) return; // already buffered
  }

  buf.scripts.push({ url: scriptUrl, code: code, key: key });
  console.debug("[AST:buffer] Buffered script %s (%d chars) tab=%d — %d scripts pending",
    scriptUrl || "(inline)", code.length, tabId, buf.scripts.length);

  // Reset debounce timer — wait for more scripts before combined analysis
  if (buf.timer) clearTimeout(buf.timer);
  buf.timer = setTimeout(function() {
    buf.timer = null;
    _analyzeCombinedScripts(tabId);
  }, 1500);
}

function _fetchAndBufferScript(tabId, scriptUrl, pageUrl) {
  // Check if already buffered
  var buf = _scriptBuffers.get(tabId);
  if (buf) {
    for (var i = 0; i < buf.scripts.length; i++) {
      if (buf.scripts[i].key === scriptUrl) return;
    }
  }

  fetch(scriptUrl).then(function(resp) {
    if (!resp.ok) {
      console.debug("[AST:buffer] Fetch failed for %s: %d %s", scriptUrl, resp.status, resp.statusText);
      return;
    }
    var ct = resp.headers.get("content-type") || "";
    // Skip non-JS responses (images, CSS, etc. that might share .open() URLs)
    if (ct && !ct.includes("javascript") && !ct.includes("ecmascript") && !ct.includes("text/plain") && !ct.includes("application/x-javascript")) {
      console.debug("[AST:buffer] Skipping non-JS content-type for %s: %s", scriptUrl, ct);
      return;
    }
    return resp.text();
  }).then(function(code) {
    if (code && code.length >= 50) {
      _bufferScript(tabId, scriptUrl, code, pageUrl);
    }
  }).catch(function(err) {
    console.debug("[AST:buffer] Fetch error for %s: %s", scriptUrl, err.message || err);
  });
}

function _hashScriptCode(code) {
  var h = 0;
  for (var i = 0; i < Math.min(code.length, 500); i++) {
    h = ((h << 5) - h + code.charCodeAt(i)) | 0;
  }
  return "inline:" + h;
}

// Full-content SHA-256 hash for AST cache keys (async, SubtleCrypto)
async function _hashScriptSHA256(code) {
  var buf = new TextEncoder().encode(code);
  var hash = await crypto.subtle.digest("SHA-256", buf);
  return Array.from(new Uint8Array(hash)).map(function(b) {
    return b.toString(16).padStart(2, "0");
  }).join("");
}

function _findScriptForLine(line, scriptOffsets) {
  for (var i = scriptOffsets.length - 1; i >= 0; i--) {
    if (line >= scriptOffsets[i].lineStart) return scriptOffsets[i];
  }
  return scriptOffsets[0];
}

// Build cross-file definition index from combined AST propDefs+defMap.
// Maps property/function names to {sourceUrl, line} using scriptOffsets
// to convert combined-code lines back to per-script lines.
function _buildCrossDefs(buf, analysis, scriptOffsets) {
  if (!buf || !scriptOffsets || !scriptOffsets.length) return;
  var crossDefs = {}; // { name: { sourceUrl, line } }
  // propDefs: { "defLine:objName": { propName: propDefLine } }
  if (analysis.propDefs) {
    for (var pk in analysis.propDefs) {
      var props = analysis.propDefs[pk];
      for (var pn in props) {
        if (!crossDefs[pn]) {
          var info = _findScriptForLine(props[pn], scriptOffsets);
          crossDefs[pn] = { sourceUrl: info.url, line: props[pn] - info.lineStart + 1 };
        }
      }
    }
  }
  // defMap: { name: line } — top-level function/var/class
  if (analysis.defMap) {
    for (var dn in analysis.defMap) {
      if (!crossDefs[dn]) {
        var dInfo = _findScriptForLine(analysis.defMap[dn], scriptOffsets);
        crossDefs[dn] = { sourceUrl: dInfo.url, line: analysis.defMap[dn] - dInfo.lineStart + 1 };
      }
    }
  }
  buf.crossDefs = crossDefs;
}

// Compare new security findings against globalStore to mark as new/existing/fixed
function _markSecurityFindingChanges(scriptUrl, findings) {
  var prev = globalStore.securityFindings.get(scriptUrl);
  if (prev) {
    var prevSigs = new Set();
    var ps = prev.securitySinks || [];
    for (var i = 0; i < ps.length; i++) {
      prevSigs.add(ps[i].sink + ":" + (ps[i].sourceType || "") + ":" + (ps[i].location ? ps[i].location.line : ""));
    }
    var pp = prev.dangerousPatterns || [];
    for (var i = 0; i < pp.length; i++) {
      prevSigs.add(pp[i].pattern + ":" + (pp[i].location ? pp[i].location.line : ""));
    }
    for (var i = 0; i < findings.sinks.length; i++) {
      var s = findings.sinks[i];
      var sig = s.sink + ":" + (s.sourceType || "") + ":" + (s.location ? s.location.line : "");
      findings.sinks[i]._changeType = prevSigs.has(sig) ? "existing" : "new";
      prevSigs.delete(sig);
    }
    for (var i = 0; i < findings.patterns.length; i++) {
      var p = findings.patterns[i];
      var sig = p.pattern + ":" + (p.location ? p.location.line : "");
      findings.patterns[i]._changeType = prevSigs.has(sig) ? "existing" : "new";
      prevSigs.delete(sig);
    }
    findings._fixedCount = prevSigs.size;
  } else {
    for (var i = 0; i < findings.sinks.length; i++) findings.sinks[i]._changeType = "new";
    for (var i = 0; i < findings.patterns.length; i++) findings.patterns[i]._changeType = "new";
  }
}

// Replay a cached AST analysis result — mirrors the post-analysis flow in
// _analyzeCombinedScripts() but skips the offscreen worker entirely.
function _replayCachedAST(tabId, tab, cached, sourceMapScripts, buf) {
  // Clear previous AST-derived endpoints only. _astResults and
  // _securityFindings are swapped in atomically below (see the same
  // rationale in _analyzeCombinedScripts above): consumers should never
  // see an empty-but-transient state.
  var keysToDelete = [];
  tab.endpoints.forEach(function(val, key) {
    if (key.startsWith("AST ") || key.startsWith("AST DYN ")) {
      keysToDelete.push(key);
    }
  });
  for (var di = 0; di < keysToDelete.length; di++) {
    tab.endpoints.delete(keysToDelete[di]);
  }

  var analysis = JSON.parse(JSON.stringify(cached.result)); // deep copy
  var scriptOffsets = cached.scriptOffsets || [];
  var tabUrl = cached.tabUrl || "";

  // Override tabUrl with current tab URL if available
  var meta = _tabMeta.get(tabId);
  if (meta && meta.url) tabUrl = meta.url;
  else if (buf && buf.pageUrl) tabUrl = buf.pageUrl;

  // Cross-file definition index is populated by analyzeJSBundle's pre-pass
  // into analysis.defMap/propDefs — GET_CROSS_DEFS projects those
  // combined-bundle lines back into per-script coords on first request.

  var hasFindings = analysis.protoEnums.length || analysis.protoFieldMaps.length ||
    analysis.fetchCallSites.length || analysis.sourceMapUrl ||
    (analysis.securitySinks && analysis.securitySinks.length) ||
    (analysis.dangerousPatterns && analysis.dangerousPatterns.length);

  if (!hasFindings && sourceMapScripts.length === 0) return;

  if (hasFindings) {
    analysis._securityMerged = true;

    // Build the new security-findings list in a LOCAL array first, then
    // swap it into tab._securityFindings atomically once fully populated.
    // Same rationale as tab._astResults: no transient empty window.
    var newSecurityFindings = [];
    var secSinks = analysis.securitySinks || [];
    var dangerousPats = analysis.dangerousPatterns || [];
    if (secSinks.length || dangerousPats.length) {
      var byScript = {};
      for (var _fsi = 0; _fsi < secSinks.length; _fsi++) {
        var sink = secSinks[_fsi];
        var sLine = sink.location ? sink.location.line : 0;
        var sInfo = _findScriptForLine(sLine, scriptOffsets);
        var sKey = sInfo.url || tabUrl;
        if (!byScript[sKey]) byScript[sKey] = { sinks: [], patterns: [] };
        var adjustedSink = Object.assign({}, sink);
        if (sInfo.url && sink.location) {
          adjustedSink.location = Object.assign({}, sink.location, {
            line: sink.location.line - sInfo.lineStart + 1
          });
        }
        byScript[sKey].sinks.push(adjustedSink);
      }
      for (var _fpi = 0; _fpi < dangerousPats.length; _fpi++) {
        var pat = dangerousPats[_fpi];
        var pLine = pat.location ? pat.location.line : 0;
        var pInfo = _findScriptForLine(pLine, scriptOffsets);
        var pKey = pInfo.url || tabUrl;
        if (!byScript[pKey]) byScript[pKey] = { sinks: [], patterns: [] };
        var adjustedPat = Object.assign({}, pat);
        if (pInfo.url && pat.location) {
          adjustedPat.location = Object.assign({}, pat.location, {
            line: pat.location.line - pInfo.lineStart + 1
          });
        }
        byScript[pKey].patterns.push(adjustedPat);
      }
      for (var sUrl in byScript) {
        _markSecurityFindingChanges(sUrl, byScript[sUrl]);
        newSecurityFindings.push({
          sourceUrl: sUrl,
          pageUrl: tabUrl,
          securitySinks: byScript[sUrl].sinks,
          dangerousPatterns: byScript[sUrl].patterns,
          _fixedCount: byScript[sUrl]._fixedCount || 0,
        });
      }
    }
    // Atomic swap for both state slots — a concurrent reader sees either
    // the previous (valid) analysis or this one, never an empty interim.
    tab._astResults = [analysis];
    tab._securityFindings = newSecurityFindings;
    mergeASTResultsIntoVDD(tab, [analysis], tabId);

    mergeToGlobal(tab);
    notifyPopup(tabId);
  }

  // Fetch source maps (not cached — they're fetched separately and may change)
  for (var smi = 0; smi < sourceMapScripts.length; smi++) {
    _fetchSourceMapForScript(tabId, tab, analysis, sourceMapScripts[smi].scriptUrl, sourceMapScripts[smi].smUrl);
  }
}

async function _analyzeCombinedScripts(tabId) {
  var buf = _scriptBuffers.get(tabId);
  if (!buf || buf.scripts.length === 0) return;

  var tab = getTab(tabId);
  var scripts = buf.scripts;
  var totalChars = 0;
  for (var i = 0; i < scripts.length; i++) totalChars += scripts[i].code.length;

  console.debug("[AST:combined] Analyzing %d scripts (%d total chars) for tab=%d",
    scripts.length, totalChars, tabId);

  // Extract source map URLs from individual scripts before concatenation
  var sourceMapScripts = []; // [{url, smUrl}]
  for (var si = 0; si < scripts.length; si++) {
    var smUrl = extractSourceMapUrl(scripts[si].code);
    if (smUrl) {
      sourceMapScripts.push({ scriptUrl: scripts[si].url, smUrl: smUrl });
    }
  }

  // ─── AST Cache Check ───────────────────────────────────────────────
  // Hash each script individually, then combine hashes into a cache key.
  // If the exact same set of scripts was analyzed before with the same
  // AST_ANALYSIS_VERSION, replay the cached result without touching the
  // offscreen worker.
  var scriptHashes = [];
  try {
    for (var hi = 0; hi < scripts.length; hi++) {
      scriptHashes.push(await _hashScriptSHA256(scripts[hi].code));
    }
  } catch (_) {
    // SubtleCrypto unavailable — proceed without cache
    scriptHashes = [];
  }

  var cacheKey = null;
  if (scriptHashes.length === scripts.length) {
    cacheKey = scriptHashes.join("+");
    var cached = globalStore.scriptCache.get(cacheKey);
    if (cached && cached.version === AST_ANALYSIS_VERSION) {
      console.debug("[AST:cache] Cache HIT for tab=%d (%d scripts, key=%s…)",
        tabId, scripts.length, cacheKey.slice(0, 16));
      // Update timestamp for LRU eviction
      cached.timestamp = Date.now();
      // Replay cached results
      _replayCachedAST(tabId, tab, cached, sourceMapScripts, buf);
      return;
    }
    console.debug("[AST:cache] Cache MISS for tab=%d (%d scripts, key=%s…)",
      tabId, scripts.length, cacheKey.slice(0, 16));
  }

  // DO NOT reset tab._astResults / tab._securityFindings here. Clearing
  // them at the start of analysis creates a visible "empty" window for
  // consumers (popup, harness, test suites) that poll during the async
  // sendToOffscreen() await below. Instead, we build the new results into
  // local variables and swap them into the tab atomically AFTER the
  // offscreen worker returns successfully. Endpoints are AST-derived too
  // but the popup tolerates staleness there — safe to clear them up-front
  // to avoid double-registration when a late script triggers re-analysis.
  var keysToDelete = [];
  tab.endpoints.forEach(function(val, key) {
    if (key.startsWith("AST ") || key.startsWith("AST DYN ")) {
      keysToDelete.push(key);
    }
  });
  for (var di = 0; di < keysToDelete.length; di++) {
    tab.endpoints.delete(keysToDelete[di]);
  }

  // Concatenate all scripts with semicolons (safe delimiter for script mode)
  // Track line offsets for per-script finding attribution
  var combined = "";
  var scriptOffsets = []; // [{url, lineStart}]
  var nlCount = 0;
  for (var ci = 0; ci < scripts.length; ci++) {
    if (ci > 0) { combined += ";\n"; nlCount++; }
    scriptOffsets.push({ url: scripts[ci].url, lineStart: nlCount + 1 });
    var code = scripts[ci].code;
    for (var ch = 0; ch < code.length; ch++) {
      if (code.charCodeAt(ch) === 10) nlCount++;
    }
    combined += code;
  }

  // Determine source URL for the combined analysis (use tab URL or first script URL)
  var tabUrl = "";
  var meta = _tabMeta.get(tabId);
  if (meta && meta.url) tabUrl = meta.url;
  else if (buf.pageUrl) tabUrl = buf.pageUrl;
  else if (scripts[0].url) tabUrl = scripts[0].url;

  // Analyze combined in offscreen document (non-blocking)
  var analysis;
  var response;
  try {
    response = await sendToOffscreen({
      type: "AST_ANALYZE", code: combined, sourceUrl: tabUrl, forceScript: true
    });
  } catch (e) {
    console.debug("[AST:combined] sendToOffscreen failed for tab=%d: %s", tabId, e.message || e);
    return;
  }
  if (!response || !response.success) {
    console.debug("[AST:combined] analyzeJSBundle failed for tab=%d: %s", tabId,
      response ? response.error : "no response");
    if (response && response.stack) console.debug(response.stack);
    // Fallback: analyze scripts individually
    for (var fi = 0; fi < scripts.length; fi++) {
      analyzeScript(tabId, scripts[fi].url, scripts[fi].code);
    }
    return;
  }
  analysis = response.result;

  if (analysis._timings) {
    // Surface per-script AST latency on the analysis result so the
    // harness (`scripts` command) can display it — useful when a
    // user-facing stall needs root-causing without leaning on the
    // background console. _analysisTimings is a stable name distinct
    // from the internal _timings the worker fills in and strips.
    tab._lastAstTimings = analysis._timings;
    analysis._analysisTimings = analysis._timings;
    delete analysis._timings;
  }

  // ─── Cache the analysis result ──────────────────────────────────────
  if (cacheKey) {
    globalStore.scriptCache.set(cacheKey, {
      version: AST_ANALYSIS_VERSION,
      result: JSON.parse(JSON.stringify(analysis)), // deep copy to avoid aliasing
      scriptOffsets: scriptOffsets,
      tabUrl: tabUrl,
      timestamp: Date.now(),
    });
    scheduleSave();
  }

  // Cross-file definition index is populated by analyzeJSBundle's pre-pass
  // into analysis.defMap/propDefs — GET_CROSS_DEFS projects those
  // combined-bundle lines back into per-script coords on first request.

  if (analysis.resolverErrors && analysis.resolverErrors.length > 0) {
    for (var _rei = 0; _rei < analysis.resolverErrors.length; _rei++) {
      var _re = analysis.resolverErrors[_rei];
      console.debug("[AST:resolver] %s: %s", _re.context, _re.message);
      if (_re.stack) console.debug(_re.stack);
    }
  }

  var hasFindings = analysis.protoEnums.length || analysis.protoFieldMaps.length ||
    analysis.fetchCallSites.length || analysis.sourceMapUrl ||
    (analysis.securitySinks && analysis.securitySinks.length) ||
    (analysis.dangerousPatterns && analysis.dangerousPatterns.length);
  if (!hasFindings && sourceMapScripts.length === 0) {
    console.debug("[AST:combined] No findings for tab=%d", tabId);
    return;
  }

  if (hasFindings) {
    console.debug("[AST:combined] Findings for tab=%d: %d protoEnums, %d fieldMaps, %d fetchSites, %d secSinks, %d dangerousPatterns",
      tabId, analysis.protoEnums.length, analysis.protoFieldMaps.length, analysis.fetchCallSites.length,
      (analysis.securitySinks ? analysis.securitySinks.length : 0),
      (analysis.dangerousPatterns ? analysis.dangerousPatterns.length : 0));

    // Pre-empt mergeASTResultsIntoVDD's security merge — we split findings per-script below
    analysis._securityMerged = true;

    // Build security findings locally, then swap into tab._* slots atomically.
    // Matches the visibility-preserving pattern in _replayCachedAST above:
    // consumers never see an empty-but-populating state.
    var newSecurityFindings = [];
    var secSinks = analysis.securitySinks || [];
    var dangerousPats = analysis.dangerousPatterns || [];
    if (secSinks.length || dangerousPats.length) {
      // Shift every nested-location field by -(lineStart-1) so the hop/
      // candidate coords end up in SCRIPT-LOCAL space, matching the
      // primary sink location. Without this, taintPath.at.line and
      // sanitizerReport.candidates[i].loc.line stay in combined-bundle
      // space and sourcemap lookups silently return null.
      function _shiftFindingLines(finding, lineDelta) {
        if (!lineDelta) return finding;
        if (Array.isArray(finding.taintPath)) {
          finding.taintPath = finding.taintPath.map(function(h) {
            if (!h || !h.at || typeof h.at.line !== "number") return h;
            return Object.assign({}, h, { at: Object.assign({}, h.at, { line: h.at.line + lineDelta }) });
          });
        }
        if (finding.sanitizerReport && Array.isArray(finding.sanitizerReport.candidates)) {
          finding.sanitizerReport = Object.assign({}, finding.sanitizerReport, {
            candidates: finding.sanitizerReport.candidates.map(function(c) {
              if (!c || !c.loc || typeof c.loc.line !== "number") return c;
              return Object.assign({}, c, { loc: Object.assign({}, c.loc, { line: c.loc.line + lineDelta }) });
            }),
          });
        }
        return finding;
      }

      var byScript = {}; // scriptUrl → {sinks: [], patterns: []}
      for (var _fsi = 0; _fsi < secSinks.length; _fsi++) {
        var sink = secSinks[_fsi];
        var sLine = sink.location ? sink.location.line : 0;
        var sInfo = _findScriptForLine(sLine, scriptOffsets);
        // External scripts: attribute to script URL with adjusted line numbers
        // Inline scripts (url empty): attribute to page URL with original line numbers
        var sKey = sInfo.url || tabUrl;
        if (!byScript[sKey]) byScript[sKey] = { sinks: [], patterns: [] };
        var adjustedSink = Object.assign({}, sink);
        if (sInfo.url && sink.location) {
          var sDelta = -(sInfo.lineStart - 1);
          adjustedSink.location = Object.assign({}, sink.location, {
            line: sink.location.line + sDelta
          });
          _shiftFindingLines(adjustedSink, sDelta);
        }
        byScript[sKey].sinks.push(adjustedSink);
      }
      for (var _fpi = 0; _fpi < dangerousPats.length; _fpi++) {
        var pat = dangerousPats[_fpi];
        var pLine = pat.location ? pat.location.line : 0;
        var pInfo = _findScriptForLine(pLine, scriptOffsets);
        var pKey = pInfo.url || tabUrl;
        if (!byScript[pKey]) byScript[pKey] = { sinks: [], patterns: [] };
        var adjustedPat = Object.assign({}, pat);
        if (pInfo.url && pat.location) {
          var pDelta = -(pInfo.lineStart - 1);
          adjustedPat.location = Object.assign({}, pat.location, {
            line: pat.location.line + pDelta
          });
          _shiftFindingLines(adjustedPat, pDelta);
        }
        byScript[pKey].patterns.push(adjustedPat);
      }
      for (var sUrl in byScript) {
        // Mark findings as new/existing by comparing against globalStore
        _markSecurityFindingChanges(sUrl, byScript[sUrl]);
        newSecurityFindings.push({
          sourceUrl: sUrl,
          pageUrl: tabUrl,
          securitySinks: byScript[sUrl].sinks,
          dangerousPatterns: byScript[sUrl].patterns,
          _fixedCount: byScript[sUrl]._fixedCount || 0,
        });
      }
      console.debug("[AST:combined] Split security findings across %d scripts for tab=%d",
        Object.keys(byScript).length, tabId);
    }
    // Atomic swap — never show consumers an empty interim.
    tab._astResults = [analysis];
    tab._securityFindings = newSecurityFindings;
    mergeASTResultsIntoVDD(tab, [analysis], tabId);

    mergeToGlobal(tab);
    notifyPopup(tabId);
  }

  // Fetch source maps for individual scripts (each has its own source map)
  for (var smi = 0; smi < sourceMapScripts.length; smi++) {
    _fetchSourceMapForScript(tabId, tab, analysis, sourceMapScripts[smi].scriptUrl, sourceMapScripts[smi].smUrl);
  }
}

function _fetchSourceMapForScript(tabId, tab, analysis, scriptUrl, smUrl) {
  try {
    if (!/^https?:\/\//i.test(smUrl)) {
      smUrl = new URL(smUrl, new URL(scriptUrl)).href;
    }
  } catch (_) {
    console.debug("[AST:sourcemap] Failed to resolve URL: %s (base: %s)", smUrl, scriptUrl);
    return;
  }
  console.debug("[AST:sourcemap] Fetching: %s (from %s)", smUrl, scriptUrl);
  pageContextFetch(tabId, smUrl, { method: "GET" })
    .then(async function(smResp) {
      if (!smResp.body || smResp.error) {
        console.debug("[AST:sourcemap] Fetch failed for %s: %s", smUrl, smResp.error || "empty body");
        return;
      }
      try {
        var smJson = JSON.parse(smResp.body);
        var smResp2 = await sendToOffscreen({ type: "AST_PARSE_SOURCEMAP", sourceMapJson: smJson });
        if (!smResp2 || !smResp2.success) {
          console.debug("[AST:sourcemap] parseSourceMap failed for %s: %s", smUrl, smResp2 ? smResp2.error : "no response");
          return;
        }
        var smData = smResp2.result;
        analysis.sourceMap = smData;
        console.debug("[AST:sourcemap] Parsed: %d sources, %d names, %d proto files, %d API client files",
          smData.sources.length, smData.names.length, smData.protoFileNames.length, smData.apiClientFiles.length);
        if (smData.sourcesContent && smData.sourcesContent.length) {
          var typesResp = await sendToOffscreen({
            type: "AST_EXTRACT_TYPES",
            sourcesContent: smData.sourcesContent,
            sources: smData.sources
          });
          if (typesResp && typesResp.success) {
            analysis.sourceMapTypes = typesResp.result;
            if (analysis.sourceMapTypes.length) {
              console.debug("[AST:sourcemap] Extracted %d types", analysis.sourceMapTypes.length);
            }
          }
          // Per-file security analysis on sourcemap sourcesContent is
          // intentionally disabled. It was meant to catch sinks that the
          // main bundled analysis missed, but in practice every sink
          // surfaces in both coord spaces. The dedup key was
          // `(type, sink, line, column)` — bundled findings sit at
          // minified (line=2, col=big-number), source-mapped findings
          // sit at beautified (line=107, col=8), so the key never
          // matches and EVERY source-mapped sink duplicates one that
          // already exists. Duplicates then can't be opened in the
          // viewer (the source-mapped URL isn't HTTP-fetchable), so
          // they're pure reviewer noise. Re-enable only after a proper
          // cross-coord dedup (reverse-map the source-mapped location
          // through smData back to bundled coords, compare those).
        }
        mergeASTResultsIntoVDD(tab, [analysis], tabId);
        mergeToGlobal(tab);
        notifyPopup(tabId);
      } catch (e) {
        console.debug("[AST:sourcemap] Parse error for %s: %s", smUrl, e.message);
      }
    }).catch(function(e) {
      console.debug("[AST:sourcemap] Network error for %s: %s", smUrl, e.message || e);
    });
}

// ─── AST Bundle Analysis ─────────────────────────────────────────────────────

async function analyzeScript(tabId, scriptUrl, code) {
  var tab = getTab(tabId);
  console.debug("[AST] Received script: %s (%d chars) tab=%d", scriptUrl || "(inline)", code.length, tabId);
  var analysis;
  var response;
  try {
    response = await sendToOffscreen({
      type: "AST_ANALYZE", code: code, sourceUrl: scriptUrl
    });
  } catch (e) {
    console.debug("[AST] sendToOffscreen failed for %s: %s", scriptUrl, e.message || e);
    return;
  }
  if (!response || !response.success) {
    console.debug("[AST] analyzeJSBundle failed for %s: %s", scriptUrl,
      response ? response.error : "no response");
    if (response && response.stack) console.debug(response.stack);
    return;
  }
  analysis = response.result;

  if (analysis.resolverErrors && analysis.resolverErrors.length > 0) {
    for (var _rei = 0; _rei < analysis.resolverErrors.length; _rei++) {
      var _re = analysis.resolverErrors[_rei];
      console.debug("[AST:resolver] %s: %s", _re.context, _re.message);
      if (_re.stack) console.debug(_re.stack);
    }
  }

  var hasFindings = analysis.protoEnums.length || analysis.protoFieldMaps.length ||
    analysis.fetchCallSites.length || analysis.sourceMapUrl ||
    (analysis.securitySinks && analysis.securitySinks.length) ||
    (analysis.dangerousPatterns && analysis.dangerousPatterns.length);
  if (!hasFindings) {
    console.debug("[AST] No findings for %s", scriptUrl || "(inline)");
    return;
  }

  console.debug("[AST] Findings for %s: %d protoEnums, %d fieldMaps, %d fetchSites, %d secSinks, %d dangerousPatterns, sourceMap=%s",
    scriptUrl || "(inline)", analysis.protoEnums.length, analysis.protoFieldMaps.length,
    analysis.fetchCallSites.length,
    (analysis.securitySinks ? analysis.securitySinks.length : 0),
    (analysis.dangerousPatterns ? analysis.dangerousPatterns.length : 0),
    analysis.sourceMapUrl || "none");

  if (!tab._astResults) tab._astResults = [];
  tab._astResults.push(analysis);
  mergeASTResultsIntoVDD(tab, [analysis], tabId);
  mergeToGlobal(tab);
  notifyPopup(tabId);

  // Source map recovery (async, fires after initial merge)
  if (analysis.sourceMapUrl) {
    var smUrl = analysis.sourceMapUrl;
    try {
      if (!/^https?:\/\//i.test(smUrl)) {
        smUrl = new URL(smUrl, new URL(scriptUrl)).href;
      }
    } catch (_) {
      console.debug("[AST:sourcemap] Failed to resolve URL: %s (base: %s)", analysis.sourceMapUrl, scriptUrl);
      return;
    }
    console.debug("[AST:sourcemap] Fetching: %s", smUrl);
    pageContextFetch(tabId, smUrl, { method: "GET" })
      .then(async function(smResp) {
        if (!smResp.body || smResp.error) {
          console.debug("[AST:sourcemap] Fetch failed for %s: %s", smUrl, smResp.error || "empty body");
          return;
        }
        try {
          var smJson = JSON.parse(smResp.body);
          var smResp2 = await sendToOffscreen({ type: "AST_PARSE_SOURCEMAP", sourceMapJson: smJson });
          if (!smResp2 || !smResp2.success) {
            console.debug("[AST:sourcemap] parseSourceMap failed for %s: %s", smUrl, smResp2 ? smResp2.error : "no response");
            return;
          }
          var smData = smResp2.result;
          analysis.sourceMap = smData;
          console.debug("[AST:sourcemap] Parsed: %d sources, %d names, %d proto files, %d API client files, %d sourcesContent",
            smData.sources.length, smData.names.length, smData.protoFileNames.length,
            smData.apiClientFiles.length, (smData.sourcesContent || []).length);
          if (smData.protoFileNames.length) {
            console.debug("[AST:sourcemap] Proto files: %s", smData.protoFileNames.join(", "));
          }
          if (smData.apiClientFiles.length) {
            console.debug("[AST:sourcemap] API client files: %s", smData.apiClientFiles.join(", "));
          }
          if (smData.sourcesContent && smData.sourcesContent.length) {
            var typesResp = await sendToOffscreen({
              type: "AST_EXTRACT_TYPES",
              sourcesContent: smData.sourcesContent,
              sources: smData.sources
            });
            if (typesResp && typesResp.success) {
              analysis.sourceMapTypes = typesResp.result;
              if (analysis.sourceMapTypes.length) {
                console.debug("[AST:sourcemap] Extracted %d types: %s", analysis.sourceMapTypes.length,
                  analysis.sourceMapTypes.map(function(t) { return t.kind + " " + t.name; }).slice(0, 10).join(", "));
              }
            }
          }
          mergeASTResultsIntoVDD(tab, [analysis], tabId);
          mergeToGlobal(tab);
          notifyPopup(tabId);
        } catch (e) {
          console.debug("[AST:sourcemap] Parse error for %s: %s", smUrl, e.message);
        }
      }).catch(function(e) {
        console.debug("[AST:sourcemap] Network error for %s: %s", smUrl, e.message || e);
      });
  }
}

function mergeASTResultsIntoVDD(tab, results, tabId) {
  for (var r = 0; r < results.length; r++) {
    var analysis = results[r];
    var sourceHost = "";
    try { sourceHost = new URL(analysis.sourceUrl).hostname; } catch (_) {}

    // Find matching discovery doc for this host (optional — endpoint registration works without it)
    var doc = null;
    var matchedSvc = null;
    tab.discoveryDocs.forEach(function(entry, svc) {
      if (entry.doc && svc.includes(sourceHost)) { doc = entry.doc; matchedSvc = svc; }
    });
    if (!doc) {
      globalStore.discoveryDocs.forEach(function(entry, svc) {
        if (entry.doc && svc.includes(sourceHost)) { doc = entry.doc; matchedSvc = svc; }
      });
    }

    // Proto field/enum merge — requires a matching doc
    if (doc) {
      console.debug("[AST:merge] Matched doc %s for host=%s", matchedSvc, sourceHost);

      // Merge proto field maps: match by field number to existing schema properties
      if (analysis.protoFieldMaps.length && doc.schemas) {
        var fieldMapMatches = 0;
        var fieldMapUnmatched = [];
        var matchedFieldNums = new Set();
        for (var schemaName in doc.schemas) {
          var schema = doc.schemas[schemaName];
          if (!schema.properties) continue;
          for (var propName in schema.properties) {
            var prop = schema.properties[propName];
            if (!prop["x-field-number"]) continue;
            for (var fm = 0; fm < analysis.protoFieldMaps.length; fm++) {
              var fieldMap = analysis.protoFieldMaps[fm];
              if (fieldMap.fieldNumber === prop["x-field-number"] && !prop.customName) {
                prop._astName = fieldMap.fieldName;
                prop._astAccessor = fieldMap.accessorName;
                fieldMapMatches++;
                matchedFieldNums.add(fieldMap.fieldNumber);
                console.debug("[AST:merge] Field #%d → %s.%s renamed to '%s'", fieldMap.fieldNumber, schemaName, propName, fieldMap.fieldName);
              }
            }
          }
        }
        for (var fmu = 0; fmu < analysis.protoFieldMaps.length; fmu++) {
          if (!matchedFieldNums.has(analysis.protoFieldMaps[fmu].fieldNumber)) {
            fieldMapUnmatched.push("#" + analysis.protoFieldMaps[fmu].fieldNumber + "=" + analysis.protoFieldMaps[fmu].fieldName);
          }
        }
        console.debug("[AST:merge] Field maps: %d matched, %d unmatched [%s]", fieldMapMatches, fieldMapUnmatched.length,
          fieldMapUnmatched.slice(0, 10).join(", ") + (fieldMapUnmatched.length > 10 ? ", ..." : ""));
      }

      // Merge proto enums: enrich existing enum-type fields
      if (analysis.protoEnums.length && doc.schemas) {
        var enumMatches = 0;
        for (var eName in doc.schemas) {
          var eSchema = doc.schemas[eName];
          if (!eSchema.properties) continue;
          for (var ePropName in eSchema.properties) {
            var eProp = eSchema.properties[ePropName];
            if (eProp.enum && !eProp.customEnum) {
              for (var pe = 0; pe < analysis.protoEnums.length; pe++) {
                var protoEnum = analysis.protoEnums[pe];
                if (!protoEnum.isReverseMap) {
                  var enumKeys = Object.keys(protoEnum.values);
                  if (enumKeys.length === eProp.enum.length) {
                    eProp._astEnum = protoEnum.values;
                    enumMatches++;
                    console.debug("[AST:merge] Enum matched: %s.%s ← {%s} (%d values)", eName, ePropName,
                      enumKeys.slice(0, 5).join(", ") + (enumKeys.length > 5 ? ", ..." : ""), enumKeys.length);
                    break;
                  }
                }
              }
            }
          }
        }
        if (analysis.protoEnums.length > enumMatches) {
          console.debug("[AST:merge] %d/%d proto enums unmatched (no schema field with same value count)", analysis.protoEnums.length - enumMatches, analysis.protoEnums.length);
        }
      }
      // Merge value constraints: enrich VDD method parameters with AST-discovered valid values
      if (analysis.valueConstraints && analysis.valueConstraints.length && doc.resources && doc.resources.learned) {
        var vcMatches = 0;
        var methods = doc.resources.learned.methods || {};
        for (var mName in methods) {
          var method = methods[mName];
          if (!method.parameters) continue;
          for (var pName in method.parameters) {
            var param = method.parameters[pName];
            if (param.customEnum) continue; // don't override manual enums
            for (var vci = 0; vci < analysis.valueConstraints.length; vci++) {
              var vc = analysis.valueConstraints[vci];
              // Match by parameter name or constraint variable name
              if (vc.variable === pName && vc.values.length >= 2 && vc.values.length <= 50) {
                param._astValidValues = vc.values;
                param._astValueSource = vc.sources.join(",");
                if (!param.enum || !param.customEnum) {
                  param.enum = vc.values.map(String);
                  param._detectedEnum = true;
                }
                vcMatches++;
                console.debug("[AST:merge] Value constraint: %s.%s ← [%s] (%d values, source: %s)",
                  mName, pName, vc.values.slice(0, 5).join(", ") + (vc.values.length > 5 ? ", ..." : ""),
                  vc.values.length, vc.sources.join(","));
                break;
              }
            }
          }
        }
        if (vcMatches > 0) {
          console.debug("[AST:merge] Value constraints: %d matched to VDD parameters", vcMatches);
        }
      }
      // Merge sourceMap TypeScript types: enrich VDD parameters with type info from original sources
      if (analysis.sourceMapTypes && analysis.sourceMapTypes.length) {
        var typeMatches = 0;
        var tsMethods = doc.resources && doc.resources.learned ? doc.resources.learned.methods || {} : {};
        for (var _tmName in tsMethods) {
          var _tmMethod = tsMethods[_tmName];
          if (!_tmMethod.parameters) continue;
          for (var _tpName in _tmMethod.parameters) {
            var _tpParam = _tmMethod.parameters[_tpName];
            if (_tpParam._tsType) continue; // already enriched
            for (var _sti = 0; _sti < analysis.sourceMapTypes.length; _sti++) {
              var _smType = analysis.sourceMapTypes[_sti];
              for (var _stf = 0; _stf < _smType.fields.length; _stf++) {
                if (_smType.fields[_stf].name === _tpName) {
                  _tpParam._tsType = _smType.fields[_stf].type;
                  _tpParam._tsInterface = _smType.name;
                  _tpParam._tsOptional = _smType.fields[_stf].optional || false;
                  if (!_tpParam.type) _tpParam.type = _smType.fields[_stf].type;
                  typeMatches++;
                  break;
                }
              }
              if (_tpParam._tsType) break;
            }
          }
        }
        // Enrich proto field maps with human-readable names from TypeScript .pb.ts interfaces
        if (analysis.protoFieldMaps && analysis.protoFieldMaps.length) {
          for (var _fmi = 0; _fmi < analysis.protoFieldMaps.length; _fmi++) {
            var _fm = analysis.protoFieldMaps[_fmi];
            for (var _sti2 = 0; _sti2 < analysis.sourceMapTypes.length; _sti2++) {
              var _pbType = analysis.sourceMapTypes[_sti2];
              if (_pbType.kind !== "interface" && _pbType.kind !== "type") continue;
              // Match by field count similarity — proto field maps and TypeScript interfaces
              // from the same proto definition should have similar field counts
              if (_pbType.fields.length === _fm.fields.length ||
                  Math.abs(_pbType.fields.length - _fm.fields.length) <= 2) {
                // Check if it looks proto-related (from .pb.ts file or has matching structure)
                var _isPbType = /\.pb\.|_pb\.|proto/i.test(_pbType.source || "");
                if (_isPbType) {
                  if (!_fm._tsNames) _fm._tsNames = {};
                  for (var _pfi = 0; _pfi < _pbType.fields.length; _pfi++) {
                    var _pbField = _pbType.fields[_pfi];
                    // Map by position: TypeScript interface field order matches proto field order
                    _fm._tsNames[_pfi + 1] = _pbField.name;
                  }
                  _fm._tsInterface = _pbType.name;
                  typeMatches++;
                  break;
                }
              }
            }
          }
        }
        if (typeMatches > 0) {
          console.debug("[AST:merge] TypeScript type enrichment: %d matches", typeMatches);
        }
      }
    } else {
      console.debug("[AST:merge] No doc for host=%s — registering endpoints only (script: %s)", sourceHost, analysis.sourceUrl);
    }

    // Register AST-observed fetch call sites as methods on their services.
    // Uses learnFromAstCallSite (direct fact-based registration), NOT the
    // old "synthesize fake URL/body, launder through learnFromRequest"
    // path — that conflated AST observations with real server traffic.
    var newEndpoints = 0;
    for (var fc = 0; fc < analysis.fetchCallSites.length; fc++) {
      var callSite = analysis.fetchCallSites[fc];
      try {
        var isDynamic = /^\$\{|^\(dynamic\)|^\{[a-zA-Z]/.test(callSite.url);
        var csUrl = null;
        var interfaceName = null;

        if (isDynamic) {
          if (!sourceHost) continue;
          interfaceName = sourceHost;
        } else if (/^https?:\/\//i.test(callSite.url)) {
          csUrl = new URL(callSite.url);
          interfaceName = extractInterfaceName(csUrl);
        } else {
          // A relative fetch URL resolves against the PAGE's origin at
          // runtime, NOT the script's host. Using analysis.sourceUrl as
          // the base misattributes cross-origin-hosted scripts: e.g.
          // `fetch('/svc/shreddit/graphql')` in a script served from
          // www.redditstatic.com actually hits www.reddit.com (the
          // page origin). Prefer the tab's page URL when available.
          var _csMeta = _tabMeta.get(tabId);
          var _csBaseForRel = (_csMeta && _csMeta.url) ? _csMeta.url : analysis.sourceUrl;
          csUrl = new URL(callSite.url, _csBaseForRel);
          interfaceName = extractInterfaceName(csUrl);
        }

        var _astDocEntry = learnFromAstCallSite(tabId, interfaceName, callSite, analysis.sourceUrl);
        // Refine interfaceName for endpoint registration if the call site
        // got promoted to a prefix bucket via observed-prefix clustering.
        if (_astDocEntry && _astDocEntry.doc && _astDocEntry.doc.name) {
          interfaceName = _astDocEntry.doc.name;
        }

        // Register endpoint for popup display — separate concern from
        // method registration (endpoint list shows "what fetches exist
        // on this page," method list shows "what API endpoints we know").
        var bundleId = analysis.sourceUrl ? analysis.sourceUrl.replace(/^https?:\/\//, "").slice(-60) : "";
        var epKey = isDynamic
          ? "AST DYN " + bundleId + " " + (callSite.enclosingFunction || "anon") + " " + callSite.method + " " + fc
          : "AST " + callSite.method + " " + csUrl.pathname;
        if (!tab.endpoints.has(epKey)) {
          var _epMeta = _tabMeta.get(tabId);
          tab.endpoints.set(epKey, {
            url: isDynamic ? callSite.url : csUrl.href,
            method: callSite.method,
            host: isDynamic ? sourceHost : csUrl.hostname,
            path: isDynamic ? callSite.url : csUrl.pathname,
            service: interfaceName,
            source: isDynamic ? "ast_dynamic" : "ast_analysis",
            pageUrl: _epMeta ? _epMeta.url : null,
            firstSeen: Date.now(),
          });
          newEndpoints++;
        }
      } catch (mergeErr) {
        console.debug("[AST:merge] Error processing fetch site %d (%s %s): %s", fc, callSite.method, callSite.url, mergeErr.message || mergeErr);
      }
    }
    if (analysis.fetchCallSites.length) {
      console.debug("[AST:merge] Fetch sites: %d call sites processed, %d endpoints registered",
        analysis.fetchCallSites.length, newEndpoints);
    }

    // Store security findings on tab state (only once per analysis — skip if already merged)
    var secSinks = analysis.securitySinks || [];
    var dangerousPats = analysis.dangerousPatterns || [];
    if ((secSinks.length || dangerousPats.length) && !analysis._securityMerged) {
      analysis._securityMerged = true;
      if (!tab._securityFindings) tab._securityFindings = [];
      var _mfMeta = tabId != null ? _tabMeta.get(tabId) : null;
      tab._securityFindings.push({
        sourceUrl: analysis.sourceUrl,
        pageUrl: _mfMeta ? _mfMeta.url : null,
        securitySinks: secSinks,
        dangerousPatterns: dangerousPats,
      });
      console.debug("[AST:merge] Security findings for %s: %d sinks, %d dangerous patterns",
        analysis.sourceUrl, secSinks.length, dangerousPats.length);
    }
  }
}

// ─── Message Handling ────────────────────────────────────────────────────────

// ─── Form Metadata Processing ─────────────────────────────────────────────

function _formFieldToParamType(field) {
  switch (field.type) {
    case "number": case "range": return "number";
    case "checkbox": return "boolean";
    case "file": return "file";
    default: return "string";
  }
}

function _handleFormMetadata(tabId, forms, sender) {
  const tab = getTab(tabId);
  const pageUrl = sender.tab ? sender.tab.url : null;

  for (var fi = 0; fi < forms.length; fi++) {
    var form = forms[fi];
    if (!form.action || !form.fields || !form.fields.length) continue;

    var url;
    try { url = new URL(form.action); } catch (_) { continue; }

    var service = extractInterfaceName(url);
    var { methodName: baseMethodName } = calculateMethodMetadata(url, service);
    var qualifiedName = form.method.toLowerCase() + "_" + baseMethodName;
    // Create or get VDD entry
    var docEntry = tab.discoveryDocs.get(service);
    if (!docEntry || !docEntry.doc) {
      docEntry = {
        status: "found",
        isVirtual: true,
        doc: {
          kind: "discovery#restDescription",
          name: service,
          title: `${service} (Learned)`,
          rootUrl: url.origin + "/",
          baseUrl: url.origin + "/",
          resources: { learned: { methods: {} } },
          schemas: {},
        },
      };
      tab.discoveryDocs.set(service, docEntry);
    }

    var doc = docEntry.doc;
    if (!doc.resources.learned) doc.resources.learned = { methods: {} };

    var methodId = `${service.replace(/\//g, ".")}.${qualifiedName}`;

    if (!doc.resources.learned.methods[qualifiedName]) {
      doc.resources.learned.methods[qualifiedName] = {
        id: methodId,
        path: url.pathname.substring(1),
        httpMethod: form.method,
        parameters: {},
        request: null,
        origin: url.origin,
        _source: "form_scan",
      };
    }

    var m = doc.resources.learned.methods[qualifiedName];

    // Convert form fields to parameters
    for (var fj = 0; fj < form.fields.length; fj++) {
      var field = form.fields[fj];
      if (!field.name || typeof field.name !== "string") continue;

      var location = form.method === "GET" ? "query" : "body";
      var paramType = _formFieldToParamType(field);

      if (!m.parameters[field.name]) {
        m.parameters[field.name] = {
          type: paramType,
          location: location,
          description: "Learned from form" + (field.placeholder ? ` (${field.placeholder})` : ""),
        };
      }

      var param = m.parameters[field.name];

      // HTML validation constraints
      if (field.required) param.required = true;
      if (field.pattern) param.pattern = field.pattern;
      if (field.minLength) param.minLength = field.minLength;
      if (field.maxLength) param.maxLength = field.maxLength;
      if (field.min != null) param.minimum = field.min;
      if (field.max != null) param.maximum = field.max;

      // Select/radio options → enum
      if (field.options && field.options.length > 0) {
        param.enum = field.options.map(function (o) { return o.value; });
      }

      if (field.defaultValue != null) param.default = field.defaultValue;
      if (field.autocomplete) param._autocomplete = field.autocomplete;
    }

    // Record content type
    if (!m.contentTypes) m.contentTypes = [];
    if (!m.contentTypes.includes(form.enctype)) m.contentTypes.push(form.enctype);

    // Apply example-value picker so form-scan-derived methods show
    // prefill values via pickExampleValue (enum/format/default tiers).
    // Without this, the Send form would render empty inputs on AST-
    // or form-only methods.
    applyStatsToMethod(m, doc);

    // Register as endpoint
    var epKey = "FORM " + form.method + " " + url.hostname + url.pathname;
    if (!tab.endpoints.has(epKey)) {
      tab.endpoints.set(epKey, {
        url: form.action,
        method: form.method,
        host: url.hostname,
        path: url.pathname,
        service: service,
        origin: url.origin,
        source: "form_scan",
        pageUrl: pageUrl,
        firstSeen: Date.now(),
      });
    }
  }

  mergeToGlobal(tab);
  notifyPopup(tabId);
}

function _handleFormSubmit(tabId, msg) {
  if (!msg.url || !msg.fields) return;
  var tab = getTab(tabId);
  var method = msg.method || "GET";

  var url;
  try { url = new URL(msg.url); } catch (_) { return; }

  var service = extractInterfaceName(url);

  // Build body for POST forms
  var reqBody = null;
  if (method !== "GET" && msg.fields.length > 0) {
    reqBody = msg.fields.map(function (f) {
      return encodeURIComponent(f.name) + "=" + encodeURIComponent(f.value);
    }).join("&");
  }

  // Create log entry
  var entry = {
    id: "form_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8),
    url: msg.url,
    method: method,
    service: service,
    timestamp: Date.now(),
    status: 0,
    completedAt: Date.now(),
    requestHeaders: method !== "GET" ? { "content-type": msg.enctype || "application/x-www-form-urlencoded" } : {},
    contentType: msg.enctype || "",
    rawBodyB64: reqBody ? btoa(reqBody) : null,
    responseBody: null,
    responseBase64: false,
    mimeType: "",
    responseHeaders: {},
    _source: "form_submit",
  };

  tab.requestLog.push(entry);
  scheduleSessionSave(tabId);

  // Learn from the form submission
  learnFromRequest(tabId, service, entry, entry.requestHeaders);
  mergeToGlobal(tab);
  notifyPopup(tabId);
}

// Content scripts handle CONTENT_KEYS, CONTENT_ENDPOINTS, CONTENT_FORMS, CONTENT_FORM_SUBMIT, RESPONSE_BODY, and SCRIPT_SOURCE.
// Manifest "matches" already restricts which pages they run on.
function handleContentMessage(msg, sender) {
  if (!sender.tab) return;
  const tabId = sender.tab.id;

  // Keep _tabMeta up to date — captureTabMeta only runs once and may get an
  // empty URL if the tab hadn't committed its navigation yet.
  if (sender.tab.url) {
    var _tm = _tabMeta.get(tabId);
    if (!_tm) {
      _tabMeta.set(tabId, { title: sender.tab.title || ("Tab " + tabId), url: sender.tab.url });
    } else if (!_tm.url) {
      _tm.url = sender.tab.url;
      if (sender.tab.title) _tm.title = sender.tab.title;
    }
  }

  // RESPONSE_BODY comes from intercept.js via content.js relay
  if (msg.type === "RESPONSE_BODY") {
    handleResponseBody(tabId, msg, sender.frameId);
    return;
  }

  // PROBE_HIT: intercept.js recorded a sink that saw the active
  // probe marker. Correlate to an open exploit-probe session via the
  // hit's own marker (carried in the URL at probe time) and append.
  if (msg.type === "PROBE_HIT") {
    if (msg.hit && typeof msg.hit.marker === "string") {
      const ses = _probeSessions.get(msg.hit.marker);
      if (ses) {
        // Record tabId + frameId so the reviewer can see which frame fired
        ses.hits.push(Object.assign({}, msg.hit, { tabId: tabId, frameId: sender.frameId || 0 }));
      }
    }
    return;
  }

  // SCRIPT_SOURCE comes from content.js script extraction — buffer for cross-script analysis
  if (msg.type === "SCRIPT_SOURCE") {
    var pageUrl = (sender.tab && sender.tab.url) || "";
    if (msg.code && typeof msg.code === "string") {
      // Inline script — code sent directly
      _bufferScript(tabId, msg.url || "", msg.code, pageUrl);
    } else if (msg.url && !msg.code) {
      // External script — content script sent URL only (avoids CORS issues)
      // Background has host_permissions: <all_urls>, so fetch is unrestricted
      _fetchAndBufferScript(tabId, msg.url, pageUrl);
    }
    return;
  }

  if (msg.type === "CONTENT_FORMS") {
    if (Array.isArray(msg.forms)) {
      _handleFormMetadata(tabId, msg.forms, sender);
    }
    return;
  }

  if (msg.type === "CONTENT_FORM_SUBMIT") {
    _handleFormSubmit(tabId, msg);
    return;
  }

  if (!Array.isArray(msg.keys || msg.endpoints)) return;
  const tab = getTab(tabId);

  if (msg.type === "CONTENT_KEYS") {
    for (const key of msg.keys) {
      API_KEY_RE.lastIndex = 0;
      if (!API_KEY_RE.test(key)) continue;
      if (!tab.apiKeys.has(key)) {
        var _ckPageUrl = sender.tab ? sender.tab.url : null;
        tab.apiKeys.set(key, {
          origin: sender.origin,
          referer: sender.origin,
          source: "page_source",
          firstSeen: Date.now(),
          lastSeen: Date.now(),
          services: new Set(),
          hosts: new Set(),
          endpoints: new Set(),
          pageUrls: new Set(_ckPageUrl ? [_ckPageUrl] : []),
          requestCount: 0,
        });
      } else if (sender.tab && sender.tab.url) {
        var _ckExisting = tab.apiKeys.get(key);
        if (!_ckExisting.pageUrls) _ckExisting.pageUrls = new Set();
        _ckExisting.pageUrls.add(sender.tab.url);
      }
    }
    mergeToGlobal(tab);
    notifyPopup(tabId);
  }

  if (msg.type === "CONTENT_ENDPOINTS") {
    for (const ep of msg.endpoints) {
      const key = `SOURCE ${ep}`;
      if (!tab.endpoints.has(key)) {
        try {
          const url = new URL(ep);
          const rpcInfo = parseRpcPath(url.pathname);
          tab.endpoints.set(key, {
            url: ep,
            method: "?",
            host: url.hostname,
            path: url.pathname,
            service: extractInterfaceName(url),
            origin: sender.origin,
            rpc: rpcInfo,
            source: "page_source",
            pageUrl: sender.tab ? sender.tab.url : null,
            firstSeen: Date.now(),
          });
        } catch (_) {}
      }
    }
    mergeToGlobal(tab);
    notifyPopup(tabId);
  }

}

// Popup messages — sender.tab is absent for popup contexts.
async function handlePopupMessage(msg, _sender, sendResponse) {
  await _globalStoreReady;
  const tabId = msg.tabId;

  switch (msg.type) {
    case "GET_STATE": {
      const tab = tabId != null ? getTab(tabId) : null;
      sendResponse(tab ? serializeTabData(tab) : null);
      return;
    }

    case "GET_FRAMES": {
      // Verify each registered frame is still alive before returning
      const frames = _tabFrames.get(tabId);
      if (!frames || frames.size === 0) {
        sendResponse([{ frameId: 0, url: _tabMeta.get(tabId)?.url || "", origin: "" }]);
        return;
      }
      const checks = [];
      for (const [fid, info] of frames) {
        checks.push(
          chrome.tabs.sendMessage(tabId, { type: "PING" }, { frameId: fid })
            .then(() => ({ frameId: fid, url: info.url, origin: info.origin, isTop: info.isTop, alive: true }))
            .catch(() => ({ frameId: fid, alive: false }))
        );
      }
      Promise.all(checks).then((results) => {
        const alive = [];
        for (var i = 0; i < results.length; i++) {
          if (results[i].alive) {
            alive.push({ frameId: results[i].frameId, url: results[i].url, origin: results[i].origin, isTop: results[i].isTop });
          } else {
            frames.delete(results[i].frameId);
          }
        }
        alive.sort((a, b) => a.frameId - b.frameId);
        if (alive.length === 0) {
          alive.push({ frameId: 0, url: _tabMeta.get(tabId)?.url || "", origin: "" });
        }
        sendResponse(alive);
      });
      return true; // async sendResponse
    }

    case "PROBE_ENDPOINT": {
      if (tabId == null) return;
      probeEndpoint(tabId, msg.endpointKey).then((result) => {
        sendResponse(result);
      });
      return true;
    }

    case "DISCOVER_SERVICE": {
      if (tabId == null) return;
      const tab = getTab(tabId);
      const ep = tab.endpoints.get(msg.endpointKey);
      if (!ep) {
        sendResponse(null);
        return;
      }

      const headers = {};
      const discoverUrl = new URL(ep.url);
      discoverUrl.searchParams.delete("key");
      if (ep.apiKey) {
        if (ep.apiKeySource === "url") {
          discoverUrl.searchParams.set("key", ep.apiKey);
        } else {
          headers["X-Goog-Api-Key"] = ep.apiKey;
        }
      }
      const fetchFn = makePageFetchFn(tabId);
      discoverServiceInfo(discoverUrl.toString(), headers, { fetchFn }).then(
        (result) => {
          tab.probeResults.set(`svc:${msg.endpointKey}`, result);
          if (result.scopes?.length) {
            const svc = ep.service || extractInterfaceName(new URL(ep.url));
            tab.scopes.set(svc, result.scopes);
          }
          mergeToGlobal(tab);
          notifyPopup(tabId);
          sendResponse(result);
        },
      );
      return true;
    }

    case "FETCH_DISCOVERY": {
      if (tabId == null) return;
      const tab = getTab(tabId);
      const ep = tab.endpoints.values().next().value;
      const hostname =
        msg.hostname || (ep?.host ?? `${msg.service}.googleapis.com`);
      const apiKeys = collectKeysForService(tab, msg.service, hostname);
      if (msg.apiKey && !apiKeys.includes(msg.apiKey)) apiKeys.push(msg.apiKey);
      if (ep?.apiKey && !apiKeys.includes(ep.apiKey)) apiKeys.push(ep.apiKey);
      fetchDiscoveryForService(tabId, msg.service, hostname, apiKeys).then(
        () => {
          sendResponse(serializeTabData(getTab(tabId)));
        },
      );
      return true;
    }

    case "CLEAR_TAB": {
      if (tabId != null) state.tabs.delete(tabId);
      clearGlobalStore();
      sendResponse({ ok: true });
      return;
    }

    case "CLEAR_LOG": {
      if (msg.clearAll) {
        for (const [tid, t] of state.tabs) {
          t.requestLog = [];
          chrome.storage.session.remove(`reqLog_${tid}`).catch(() => {});
        }
        saveSessionIndex();
      } else {
        if (tabId == null) return;
        const tab = getTab(tabId);
        tab.requestLog = [];
        chrome.storage.session.remove(`reqLog_${tabId}`).catch(() => {});
        saveSessionIndex();
      }
      sendResponse({ ok: true });
      return;
    }

    case "GET_TAB_LIST": {
      const tabs = [];
      for (const [tid, t] of state.tabs) {
        if (t.requestLog.length === 0) continue;
        const meta = _tabMeta.get(tid) || { title: `Tab ${tid}`, url: "" };
        tabs.push({ tabId: tid, title: meta.title, url: meta.url, count: t.requestLog.length, closed: !!meta.closed });
      }
      // Also include closed tabs from metadata that still have session storage
      for (const [tid, meta] of _tabMeta) {
        if (meta.closed && !state.tabs.has(tid)) {
          tabs.push({ tabId: tid, title: meta.title, url: meta.url, count: meta.count || 0, closed: true });
        }
      }
      sendResponse(tabs);
      return;
    }

    case "GET_ALL_LOGS": {
      const result = {};
      const filter = msg.filter; // "all" | tabId (number)
      for (const [tid, t] of state.tabs) {
        if (t.requestLog.length === 0) continue;
        if (filter !== "all" && filter !== tid) continue;
        const meta = _tabMeta.get(tid) || { title: `Tab ${tid}`, url: "" };
        result[tid] = { meta, requestLog: t.requestLog };
      }
      sendResponse(result);
      return;
    }

    case "GET_DISCOVERY_CHANGES": {
      sendResponse(Object.fromEntries(globalStore.discoveryChanges));
      return;
    }

    case "GET_SCRIPT_SOURCE": {
      var scriptUrl = msg.scriptUrl;
      if (!scriptUrl) { sendResponse({ error: "no URL" }); return; }
      var tab = tabId != null ? getTab(tabId) : null;
      var _slFindings = tab ? mergedSecurityFindings(tab).filter(function(f) { return f.sourceUrl === scriptUrl; }) : [];
      // Determine pageUrl from buffer or findings
      var _slBuf = _scriptBuffers.get(tabId);
      var _slPageUrl = _slBuf ? _slBuf.pageUrl : null;
      if (!_slPageUrl && _slFindings.length > 0) _slPageUrl = _slFindings[0].pageUrl || null;

      // Try script buffers first (already fetched for AST analysis)
      if (_slBuf) {
        for (var _sbi = 0; _sbi < _slBuf.scripts.length; _sbi++) {
          if (_slBuf.scripts[_sbi].url === scriptUrl && _slBuf.scripts[_sbi].code) {
            sendResponse({ code: _slBuf.scripts[_sbi].code, findings: _slFindings, pageUrl: _slPageUrl });
            return;
          }
        }
        // If requested URL is the page URL, return the same combined source the AST analyzed
        // (all scripts in buffer order, joined with ;\n) so finding line numbers match
        if (_slBuf.pageUrl && scriptUrl === _slBuf.pageUrl && _slBuf.scripts.length > 0) {
          var _combined = "";
          for (var _cli = 0; _cli < _slBuf.scripts.length; _cli++) {
            if (_cli > 0) _combined += ";\n";
            _combined += _slBuf.scripts[_cli].code;
          }
          sendResponse({ code: _combined, findings: _slFindings, pageUrl: _slPageUrl });
          return;
        }
      }

      // Try request log (captured response body from fetch/XHR)
      if (tab && tab.requestLog) {
        for (var _sli = tab.requestLog.length - 1; _sli >= 0; _sli--) {
          var _slEntry = tab.requestLog[_sli];
          if (_slEntry.url === scriptUrl && _slEntry.responseBody) {
            var _slCode = _slEntry.responseBase64 ? atob(_slEntry.responseBody) : _slEntry.responseBody;
            sendResponse({ code: _slCode, findings: _slFindings, pageUrl: _slPageUrl });
            return;
          }
        }
      }

      // Re-fetch the script (extension has <all_urls>)
      fetch(scriptUrl).then(function(r) {
        if (!r.ok) throw new Error(r.status + " " + r.statusText);
        return r.text();
      }).then(function(code) {
        sendResponse({ code: code, findings: _slFindings, pageUrl: _slPageUrl });
      }).catch(function(e) {
        sendResponse({ error: e.message });
      });
      return true; // async sendResponse
    }

    case "GET_TAB_SCRIPTS": {
      var scripts = new Set();
      // From script buffers (all scripts seen on the page)
      var _tsBuf = _scriptBuffers.get(tabId);
      if (_tsBuf) {
        for (var _tsi = 0; _tsi < _tsBuf.scripts.length; _tsi++) {
          if (_tsBuf.scripts[_tsi].url) scripts.add(_tsBuf.scripts[_tsi].url);
        }
      }
      // From tab security findings (survives SW restart via _securityFindings)
      var _tsTab = tabId != null ? getTab(tabId) : null;
      if (_tsTab) {
        var _tsFindings = mergedSecurityFindings(_tsTab);
        for (var _tsfi = 0; _tsfi < _tsFindings.length; _tsfi++) {
          if (_tsFindings[_tsfi].sourceUrl) scripts.add(_tsFindings[_tsfi].sourceUrl);
        }
      }
      // From globalStore.securityFindings (persisted in IndexedDB, survives SW restart)
      for (var [_tsKey] of globalStore.securityFindings) {
        if (_tsKey && !_tsKey.startsWith("unknown_")) scripts.add(_tsKey);
      }
      sendResponse([...scripts]);
      return;
    }

    case "GET_CROSS_DEFS": {
      // Click-to-definition data (propDefs + defMap) is now populated by
      // analyzeJSBundle's pre-pass — the structural-def collection rides
      // on the same traversal the security analysis already needs, so
      // there's no separate buildDefinitionMap call to make here. Pull
      // the cached analysis result off the tab and re-project it into
      // per-script coords. The AST_BUILD_DEFINITION_MAP fallback only
      // runs if the analysis is missing (shouldn't happen in practice
      // once the capture pipeline has completed).
      const _cdBuf = _scriptBuffers.get(tabId);
      if (!_cdBuf || !_cdBuf.scripts || !_cdBuf.scripts.length) {
        sendResponse(null);
        return;
      }
      if (_cdBuf.crossDefs) { sendResponse(_cdBuf.crossDefs); return; }

      const _cdTab = state.tabs.get(tabId);
      const _cdAnalysis = _cdTab && _cdTab._astResults && _cdTab._astResults[0];
      const scriptOffsets = [];
      let nlCount = 0;
      for (let ci = 0; ci < _cdBuf.scripts.length; ci++) {
        if (ci > 0) { nlCount++; }
        scriptOffsets.push({ url: _cdBuf.scripts[ci].url, lineStart: nlCount + 1 });
        const code = _cdBuf.scripts[ci].code;
        for (let ch = 0; ch < code.length; ch++) {
          if (code.charCodeAt(ch) === 10) nlCount++;
        }
      }
      if (_cdAnalysis && (_cdAnalysis.defMap || _cdAnalysis.propDefs)) {
        _buildCrossDefs(_cdBuf, _cdAnalysis, scriptOffsets);
        sendResponse(_cdBuf.crossDefs || null);
        return;
      }
      // Fallback: no analysis available yet, rebuild from scratch
      (async () => {
        let combined = "";
        let _fbNlCount = 0;
        for (let ci = 0; ci < _cdBuf.scripts.length; ci++) {
          if (ci > 0) { combined += ";\n"; _fbNlCount++; }
          const code = _cdBuf.scripts[ci].code;
          for (let ch = 0; ch < code.length; ch++) {
            if (code.charCodeAt(ch) === 10) _fbNlCount++;
          }
          combined += code;
        }
        let resp;
        try {
          resp = await sendToOffscreen({ type: "AST_BUILD_DEFINITION_MAP", code: combined });
        } catch (e) {
          console.debug("[GET_CROSS_DEFS] offscreen failed:", e && e.message || e);
          sendResponse(null); return;
        }
        if (!resp || !resp.success) { sendResponse(null); return; }
        _buildCrossDefs(_cdBuf, resp.result, scriptOffsets);
        sendResponse(_cdBuf.crossDefs || null);
      })();
      return true;
    }

    case "GET_ENDPOINT_SCHEMA": {
      if (tabId == null) return;
      // Pass service/methodId if available (for virtual endpoints)
      const result = resolveEndpointSchema(
        tabId,
        msg.endpointKey,
        msg.service,
        msg.methodId,
      );
      sendResponse(result);
      return;
    }

    case "SEND_REQUEST": {
      if (tabId == null) return;
      executeSendRequest(tabId, msg).then((result) => {
        sendResponse(result);
      });
      return true;
    }

    case "WS_SEND_MSG": {
      if (tabId == null) return;
      var _wsOpts = msg.frameId != null ? { frameId: msg.frameId } : undefined;
      chrome.tabs.sendMessage(tabId, {
        type: "WS_SEND_MSG",
        wsId: msg.channelId,
        data: msg.data,
        binary: msg.binary || false,
      }, _wsOpts).then(() => sendResponse({ ok: true }))
        .catch((err) => sendResponse({ error: err.message }));
      return true;
    }

    case "WS_GET_STATUS": {
      if (tabId == null) return;
      const conns = _wsConnState.get(tabId);
      const conn = conns?.get(msg.channelId);
      // Also return the messages array for the WS console
      let messages = [];
      if (conn) {
        const tab = getTab(tabId);
        const entry = tab.requestLog.find((r) => r.channelId === msg.channelId && r.method === "WEBSOCKET");
        if (entry) messages = entry.messages || [];
      }
      sendResponse({
        readyState: conn ? conn.readyState : 3,
        url: conn?.url || null,
        messages: messages,
      });
      return;
    }

    case "PM_SEND_MSG": {
      if (tabId == null) return;
      var _pmOpts = msg.frameId != null ? { frameId: msg.frameId } : undefined;
      chrome.tabs.sendMessage(tabId, {
        type: "PM_SEND_MSG",
        data: msg.data,
        targetOrigin: msg.targetOrigin,
      }, _pmOpts).then(() => {
        // Record sent message in the log entry (intercept.js can't capture outgoing postMessage)
        const tab = getTab(tabId);
        const entry = tab.requestLog.find((r) => r.channelId === msg.channelId && r.method === "POSTMESSAGE");
        if (entry) {
          entry.messages.push({ dir: "sent", time: Date.now(), body: msg.data || "", base64: false });
          if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);
          scheduleSessionSave(tabId);
          notifyPopup(tabId);
        }
        sendResponse({ ok: true });
      }).catch((err) => sendResponse({ error: err.message }));
      return true;
    }

    case "PM_GET_STATUS": {
      if (tabId == null) return;
      const tab = getTab(tabId);
      const entry = tab.requestLog.find((r) => r.channelId === msg.channelId && r.method === "POSTMESSAGE");
      sendResponse({
        readyState: 1, // postMessage is always "active"
        messages: entry ? (entry.messages || []) : [],
      });
      return;
    }

    case "MC_SEND_MSG": {
      if (tabId == null) return;
      var _mcOpts = msg.frameId != null ? { frameId: msg.frameId } : undefined;
      chrome.tabs.sendMessage(tabId, {
        type: "MC_SEND_MSG",
        channelId: msg.channelId,
        data: msg.data,
      }, _mcOpts).then(() => {
        const tab = getTab(tabId);
        const entry = tab.requestLog.find((r) => r.channelId === msg.channelId && r.method === "MSGCHANNEL");
        if (entry) {
          entry.messages.push({ dir: "sent", time: Date.now(), body: msg.data || "", base64: false });
          if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);
          scheduleSessionSave(tabId);
          notifyPopup(tabId);
        }
        sendResponse({ ok: true });
      }).catch((err) => sendResponse({ error: err.message }));
      return true;
    }

    case "MC_GET_STATUS": {
      if (tabId == null) return;
      const tab = getTab(tabId);
      const entry = tab.requestLog.find((r) => r.channelId === msg.channelId && r.method === "MSGCHANNEL");
      sendResponse({
        readyState: 1, // port is active once transferred
        messages: entry ? (entry.messages || []) : [],
      });
      return;
    }

    case "BUILD_REQUEST": {
      if (tabId == null) return;
      buildExportRequest(tabId, msg).then((result) => {
        sendResponse(result);
      });
      return true;
    }

    // EXPLOIT_PROBE_START: kick off an exploitability probe and return
    // the session id immediately. Caller polls EXPLOIT_PROBE_STATUS to
    // observe progress + results. Split from the "run to completion"
    // shape so a popup button can start a probe, let the popup close,
    // and retrieve results later without losing them.
    case "EXPLOIT_PROBE_START": {
      try {
        const session = startExploitProbe(msg);
        sendResponse({ success: true, sessionId: session.marker });
      } catch (e) {
        sendResponse({ error: (e && e.message) || String(e) });
      }
      return true;
    }

    // EXPLOIT_PROBE_STATUS: return everything known about a running or
    // completed probe. Sessions persist past completion (TTL 10 min)
    // so a closed-and-reopened popup can still render the result.
    case "EXPLOIT_PROBE_STATUS": {
      const ses = msg.sessionId ? _probeSessions.get(msg.sessionId) : null;
      if (!ses) { sendResponse({ error: "session not found or expired" }); return; }
      sendResponse({
        success: true,
        status: ses.status,
        marker: ses.marker,
        strategy: ses.strategy,
        pageUrl: ses.pageUrl,
        hits: ses.hits.slice(),
        executed: ses.executed || null,
        startedAt: ses.createdAt,
        finishedAt: ses.finishedAt || null,
        error: ses.error || null,
        // Expose the recipe the probe actually used so the reviewer can
        // audit "what did we send, why didn't it fire, is the AST's
        // precondition/decoder chain accurate?" without digging into
        // background logs.
        recipe: {
          sinkType: ses.sinkType || null,
          sinkName: ses.sinkName || null,
          paramName: ses.paramName || null,
          fieldPath: Array.isArray(ses.fieldPath) ? ses.fieldPath.slice() : [],
          decoders: Array.isArray(ses.decoders) ? ses.decoders.slice() : [],
          preconditions: Array.isArray(ses.preconditions) ? ses.preconditions.slice() : [],
        },
      });
      return;
    }

    // EXPLOIT_PROBE_LIST: enumerate recent sessions so the popup can
    // show a "probe history" view alongside findings.
    case "EXPLOIT_PROBE_LIST": {
      const out = [];
      for (const [marker, ses] of _probeSessions) {
        out.push({
          sessionId: marker,
          status: ses.status,
          strategy: ses.strategy,
          pageUrl: ses.pageUrl,
          hitCount: ses.hits.length,
          startedAt: ses.createdAt,
          finishedAt: ses.finishedAt || null,
        });
      }
      out.sort((a, b) => b.startedAt - a.startedAt);
      sendResponse({ success: true, sessions: out });
      return;
    }

    case "RENAME_FIELD": {
      if (tabId == null) return;
      const tab = getTab(tabId);
      const { service, schemaName, fieldKey, newName } = msg;
      const docEntry =
        tab.discoveryDocs.get(service) ||
        globalStore.discoveryDocs.get(service);

      if (!docEntry || !docEntry.doc) return;
      const doc = docEntry.doc;

      if (schemaName === "params") {
        // Find method and rename its parameter
        let m = null;
        if (msg.methodId) {
          const match = findMethodById(doc, msg.methodId);
          if (match) m = match.method;
        }

        if (!m) {
          // Fallback: Calculate from URL (less reliable)
          const { methodName } = calculateMethodMetadata(
            new URL(msg.url || ""),
            service,
          );
          m =
            doc.resources.learned?.methods[methodName] ||
            doc.resources.probed?.methods[methodName];
        }

        if (m && m.parameters?.[fieldKey]) {
          m.parameters[fieldKey].name = newName;
          m.parameters[fieldKey].customName = true;
          mergeToGlobal(tab);
          sendResponse({ ok: true });
        }
      } else {
        // Handle schema properties or create virtual schema for raw fields
        if (!doc.schemas) doc.schemas = {};
        if (!doc.schemas[schemaName]) {
          doc.schemas[schemaName] = { id: schemaName, type: "object", properties: {} };
        }

        const schema = doc.schemas[schemaName];
        if (!schema.properties) schema.properties = {};

        if (schema.properties[fieldKey]) {
          const prop = schema.properties[fieldKey];
          prop.name = newName;
          prop.customName = true;
        } else {
          // Create a virtual property for a raw field number
          schema.properties[fieldKey] = {
            id: fieldKey,
            number: parseInt(fieldKey) || null,
            name: newName,
            customName: true,
            type: "any"
          };
        }
        mergeToGlobal(tab);
        sendResponse({ ok: true });
      }
      return;
    }

    case "EXPORT_OPENAPI": {
      if (tabId == null) return;
      const tab = getTab(tabId);
      const svc = msg.service;
      const docEntry =
        tab.discoveryDocs.get(svc) || globalStore.discoveryDocs.get(svc);
      if (!docEntry?.doc) {
        sendResponse({ error: "No discovery document found for " + svc });
        return;
      }
      const openapi = convertDiscoveryToOpenApi(docEntry.doc, svc);
      sendResponse({ ok: true, spec: openapi });
      return;
    }

    case "IMPORT_OPENAPI": {
      if (tabId == null) return;
      const tab = getTab(tabId);
      try {
        const spec = msg.spec;
        if (!spec || typeof spec !== "object") {
          sendResponse({ error: "Invalid OpenAPI spec: not an object" });
          return;
        }
        if (!spec.paths || typeof spec.paths !== "object") {
          sendResponse({ error: "Invalid OpenAPI spec: missing or invalid paths" });
          return;
        }
        // Validate OpenAPI version — only 3.0.x and 3.1.x supported
        if (spec.openapi) {
          if (!/^3\.\d+\.\d+/.test(spec.openapi)) {
            sendResponse({ error: "Unsupported OpenAPI version: " + spec.openapi + ". Only 3.x is supported." });
            return;
          }
        } else if (spec.swagger) {
          // Swagger 2.0 — not supported by convertOpenApiToDiscovery
          sendResponse({ error: "Swagger 2.0 is not supported. Please convert to OpenAPI 3.x first." });
          return;
        }
        // Determine service name. Prefer the original internal key when
        // it was preserved via the `x-service-key` vendor extension on
        // export — otherwise a UASR-exported spec for a path-prefixed
        // service like "www.google.com/MapsWizUi" would import back
        // under the hostname-only "www.google.com", silently merging
        // unrelated services. Fall back to hostname (for specs from
        // other tools) and finally to info.title.
        let svcName;
        if (spec.info && typeof spec.info["x-service-key"] === "string" &&
            spec.info["x-service-key"].length > 0) {
          svcName = spec.info["x-service-key"];
        }
        if (!svcName && spec.servers?.[0]?.url) {
          try {
            svcName = new URL(spec.servers[0].url).hostname;
          } catch (_) {}
        }
        if (!svcName) {
          svcName = (spec.info?.title || "imported")
            .toLowerCase().replace(/[^a-z0-9.]/g, "_");
        }

        // Convert to internal Discovery format
        const sourceUrl = spec.servers?.[0]?.url || "https://" + svcName;
        const doc = convertOpenApiToDiscovery(spec, sourceUrl);

        // Merge with existing doc if present
        const existing = tab.discoveryDocs.get(svcName) ||
          globalStore.discoveryDocs.get(svcName);
        if (existing?.doc) {
          // Merge imported methods into existing doc
          for (const [rName, resource] of Object.entries(doc.resources)) {
            if (!existing.doc.resources[rName]) {
              existing.doc.resources[rName] = resource;
            } else {
              for (const [mName, method] of Object.entries(resource.methods || {})) {
                if (!existing.doc.resources[rName].methods[mName]) {
                  existing.doc.resources[rName].methods[mName] = method;
                }
              }
            }
          }
          // Merge schemas (imported fills gaps, doesn't overwrite)
          for (const [sName, schema] of Object.entries(doc.schemas)) {
            if (!existing.doc.schemas[sName]) {
              existing.doc.schemas[sName] = schema;
            }
          }
        } else {
          // Store as new discovery doc
          var _prevTabEntry = tab.discoveryDocs.get(svcName);
          var _prevGlobalEntry = globalStore.discoveryDocs.get(svcName);
          const entry = {
            status: "found",
            url: sourceUrl,
            method: "IMPORT",
            apiKey: null,
            fetchedAt: Date.now(),
            doc,
            isVirtual: false,
            pageUrls: _prevTabEntry?.pageUrls || _prevGlobalEntry?.pageUrls || new Set(),
            frameOrigins: _prevTabEntry?.frameOrigins || _prevGlobalEntry?.frameOrigins || new Set(),
          };
          tab.discoveryDocs.set(svcName, entry);
          globalStore.discoveryDocs.set(svcName, entry);
        }
        mergeToGlobal(tab);
        scheduleSave();
        sendResponse({ ok: true, service: svcName });
      } catch (err) {
        sendResponse({ error: "Import failed: " + err.message });
      }
      return;
    }
  }
}

// ─── Export Request Builder ──────────────────────────────────────────────────

/**
 * Build a fully-encoded request (URL, headers, body) for export.
 * Reuses the same encoding logic as executeSendRequest but returns the
 * request instead of sending it.
 */
// ─── Exploit probe (interactive per-finding verification) ─────────────────

// Construct the active payload that proves EXECUTION (not just taint
// reach). Three payload strings, all keyed by the same marker so the
// post-probe read can correlate:
//   - html: an <img src=x onerror="..."> fragment. If the page
//     innerHTMLs the probe input, the img fails to load, onerror runs,
//     and window.__apisec_fired_<marker>.html gets a timestamp. This
//     is the real "alert(origin)"-class proof for DOM XSS.
//   - js: a statement that sets the same flag. If the page passes the
//     probe input to eval() / new Function(), the flag is set.
//   - href: a javascript: URL that, if assigned to location.href, sets
//     the flag via its expression body.
// Markers are suffixed with a random probe-id so multiple probes in
// the same tab don't collide.
function _probePayloads(marker) {
  // Identifier-safe — marker is already [A-Z0-9_] so we can use it as
  // a property suffix without escaping.
  const flag = "__apisec_fired_" + marker;
  // Sequence-expression setter: returns Date.now() after initialising
  // `self.<flag>` as an object. Callers append `.key=Date.now()` and a
  // CLOSING `)` — the opener is included here, closer is caller-side
  // because html/svg/href embed it in attribute-quoted context where
  // the ')' must be balanced at the embedding layer.
  const setter =
    "(self." + flag + "=(self." + flag + "||{}),self." + flag;
  // CSP-tolerant DOM-presence id: creates a <div> in the DOM if the
  // payload is HTML-parsed, even when the page blocks inline event
  // handlers. Post-observation, getElementById reveals parsing.
  const domId = "__apisec_dom_" + marker;
  return {
    flag: flag,
    domId: domId,
    // html: <img> that fires onerror even when src fails.
    // The onerror value is itself a sequence-expression, balanced here.
    html: '<img src=x onerror=\'' + setter + '.html=Date.now())\'>',
    // js: complete expression-statement for eval() / Function(). Closing
    // ')' balances the setter's opener so eval(js) doesn't SyntaxError.
    js: setter + '.js=Date.now())',
    // href: javascript: URL — balance the setter's paren; trailing `//`
    // swallows anything appended by the sink.
    href: 'javascript:' + setter + '.href=Date.now())//',
    // svg: alternate HTML vector — some sanitizers strip <img> but miss <svg>
    svg: '<svg onload=\'' + setter + '.svg=Date.now())\'></svg>',
    // dom: CSP-tolerant signal. No script execution required — the
    // browser only needs to PARSE the HTML for a div with this id to
    // end up in the document. getElementById proves parsing happened.
    // Works on sites with strict `script-src 'none'` where onerror
    // would be silently dropped.
    dom: '<div id="' + domId + '" data-apisec="' + marker + '"></div>',
  };
}

function _buildProbeUrl(pageUrl, strategy, marker, opts) {
  const u = new URL(pageUrl);
  const pl = _probePayloads(marker);
  // Payload shape depends on sink semantics. The same string cannot
  // simultaneously be valid HTML-for-innerHTML AND valid JS-for-eval —
  // so we pick per sink type:
  //   eval / Function sinks → pl.js (valid JS statement sets flag.js)
  //   redirect sinks (location.*, window.open) → pl.href (javascript: URL)
  //   xss / DOM sinks / default → pl.html + pl.svg + pl.dom (HTML vectors)
  // If sinkType is unknown, default to HTML (covers innerHTML/writeLn/
  // setAttribute — the most common sink family).
  const sinkType = opts && opts.sinkType;
  const sinkName = opts && opts.sinkName;
  // Pick payload shape from how the sink executes attacker content:
  //   eval()/Function()           → pl.js (JS statement)
  //   location.href /.assign, etc.→ pl.href (javascript: URL navigation)
  //   href/src/action attributes  → pl.href (javascript: URL; the HTML
  //                                 payload wouldn't change scheme)
  //   innerHTML/write/append etc. → pl.html+svg+dom (HTML parsing)
  //   unknown xss                 → default to HTML (common case)
  const isUrlAttrSink = sinkType === "xss" && typeof sinkName === "string" && (
    sinkName === "href" || sinkName === "src" || sinkName === "action" ||
    sinkName === "formaction" ||
    sinkName === "setAttribute:href" || sinkName === "setAttribute:src" ||
    sinkName === "setAttribute:action" || sinkName === "setAttribute:formaction"
  );
  let activePayload;
  if (sinkType === "eval") {
    activePayload = pl.js;
  } else if (sinkType === "redirect" || isUrlAttrSink) {
    activePayload = pl.href;
  } else {
    // HTML payload: prefix with a parse-state-reset sequence so the
    // active <img>/<svg> elements escape the most common embedding
    // contexts and render as siblings. The three chars + `</script>`:
    //   "   — closes a double-quoted attribute value
    //   '   — closes a single-quoted attribute value
    //   >   — ends the enclosing start-tag
    //   </script>  — exits raw-text state inside <script src="…">
    //              (needed for document.write('<script src="'+x+'">'))
    // In pure-body contexts these leading chars render as text and do
    // no harm; <img>/<svg> still parse. This is a PARSING FACT about
    // HTML, not a heuristic about sinks.
    activePayload = '"\'></script>' + pl.html + pl.svg + pl.dom;
  }
  if (strategy === "hash" || strategy === "postmessage") {
    // Hash payload combines the sentinel (__apisec=MARKER) with the
    // active payload. Pages that decode location.hash and innerHTML
    // it will both (a) trip intercept.js sink wrappers on the marker
    // AND (b) expose an execution proof via one of the vectors.
    const active = "__apisec=" + marker + "&p=" + encodeURIComponent(activePayload);
    u.hash = "#" + active;
  } else if (strategy === "search") {
    // The caller MUST supply the param name the finding observed the
    // page reading. We don't guess — a hardcoded list of common names
    // is a heuristic that gives false positives (wrong key picked,
    // page echoes it somewhere unrelated) AND false negatives (app
    // uses a project-specific name not in our list). The AST finding
    // already knows which name its sink traced back to; the caller
    // derives it from the finding and passes it through.
    if (!opts || !opts.paramName) {
      throw new Error("search strategy requires opts.paramName — the query key the finding observed the page reading");
    }
    u.searchParams.set("__apisec", marker);
    u.searchParams.set(opts.paramName, activePayload);
  } else if (strategy === "pathname") {
    u.pathname = u.pathname.replace(/\/?$/, "/") + marker;
  }
  return u.href;
}

function _waitForTabLoaded(tabId, timeoutMs) {
  return new Promise((resolve) => {
    let done = false;
    const to = setTimeout(() => {
      if (done) return; done = true;
      chrome.tabs.onUpdated.removeListener(listener);
      resolve();
    }, timeoutMs || 15000);
    const listener = (id, info) => {
      if (id === tabId && info.status === "complete") {
        if (done) return; done = true;
        clearTimeout(to);
        chrome.tabs.onUpdated.removeListener(listener);
        resolve();
      }
    };
    chrome.tabs.onUpdated.addListener(listener);
  });
}

// Sessions persist past completion so the popup can reopen after the
// probe finishes and still render the result. Capped via TTL + LRU.
const PROBE_SESSION_TTL_MS = 10 * 60 * 1000;
const PROBE_SESSION_MAX = 50;

function _pruneProbeSessions() {
  const now = Date.now();
  for (const [k, s] of _probeSessions) {
    const age = now - (s.finishedAt || s.createdAt);
    if (s.status !== "running" && age > PROBE_SESSION_TTL_MS) _probeSessions.delete(k);
  }
  if (_probeSessions.size > PROBE_SESSION_MAX) {
    // Drop oldest finished sessions first
    const entries = [...(_probeSessions.entries())]
      .filter(([, s]) => s.status !== "running")
      .sort((a, b) => (a[1].finishedAt || a[1].createdAt) - (b[1].finishedAt || b[1].createdAt));
    for (let i = 0; i < entries.length && _probeSessions.size > PROBE_SESSION_MAX; i++) {
      _probeSessions.delete(entries[i][0]);
    }
  }
}

// Start an exploit probe, return the session handle immediately. The
// actual tab-opening + observation happens async in _runExploitProbe;
// its outcome lands back on the session object where pollers can see
// it. No await for the run — popup callers are non-blocking.
function startExploitProbe(msg) {
  _pruneProbeSessions();
  const { strategy, waitMs, findingId, paramName, fieldPath, sinkType, sinkName, decoders, preconditions } = msg || {};
  if (!strategy) throw new Error("strategy required (hash | search | pathname | postmessage)");
  if (strategy === "search" && !paramName) {
    throw new Error("search strategy requires paramName — derive it from the finding's observed source (e.g. the argument to URLSearchParams.get)");
  }
  if (fieldPath && !Array.isArray(fieldPath)) throw new Error("fieldPath must be an array of string field names");

  // Resolve pageUrl from the finding the probe is verifying.
  // globalStore.securityFindings records pageUrl when the finding is
  // observed; we read it directly rather than asking the caller to
  // supply it (the caller may not know, and we must never guess which
  // tab a finding came from). Caller can pass msg.pageUrl to override
  // — e.g. harness's --page-url for the reviewer who audited the site.
  let pageUrl = msg && msg.pageUrl ? msg.pageUrl : null;
  if (!pageUrl && findingId && msg.sourceUrl) {
    const entry = globalStore.securityFindings.get(msg.sourceUrl);
    if (entry && entry.pageUrl) pageUrl = entry.pageUrl;
  }

  const wait = Math.max(1000, Math.min(30000, Number(waitMs) || 5000));
  const marker = "PROBE_" + Math.random().toString(36).slice(2, 10).toUpperCase();
  const session = {
    marker, status: "running", strategy, pageUrl: pageUrl || null,
    findingId: findingId || null, sourceUrl: (msg && msg.sourceUrl) || null,
    paramName: paramName || null,
    fieldPath: Array.isArray(fieldPath) ? fieldPath.slice() : [],
    sinkType: sinkType || null, sinkName: sinkName || null,
    decoders: Array.isArray(decoders) ? decoders.slice() : [],
    preconditions: Array.isArray(preconditions) ? preconditions.slice() : [],
    waitMs: wait,
    hits: [], openedTabs: [], createdAt: Date.now(), finishedAt: null, error: null,
  };
  _probeSessions.set(marker, session);
  _runExploitProbe(session).catch((e) => {
    session.status = "error";
    session.error = (e && e.message) || String(e);
    session.finishedAt = Date.now();
  });
  return session;
}

// Read the per-marker execution flag from a tab. Returns an object
// like { html: timestamp, svg: timestamp, js: timestamp, href: timestamp }
// where each key corresponds to a payload variant that actually ran —
// that's the PROOF OF EXECUTION, distinguishing "taint reached a
// sink" (intercept.js wrappers saw the marker) from "the sink
// actually parsed it as HTML / evaluated it as JS". Returns null if
// no payload fired.
async function _readProbeExecFlag(tabId, marker) {
  try {
    const flagName = "__apisec_fired_" + marker;
    const domId = "__apisec_dom_" + marker;
    const results = await chrome.scripting.executeScript({
      target: { tabId, allFrames: true },
      world: "MAIN",
      func: (flag, id) => {
        // Two execution signals per frame:
        //   1. self[flag] — set by <img onerror>, <svg onload>,
        //      javascript: href navigation, or eval of js payload.
        //      Requires inline-handler-permissive CSP.
        //   2. document.getElementById(id) — our payload's <div>
        //      landed in the DOM. Proves HTML PARSING happened, no
        //      script-src required. Works under strict CSP.
        try {
          const out = Object.assign({}, self[flag] || {});
          if (document.getElementById(id)) out.dom = out.dom || Date.now();
          return Object.keys(out).length ? out : null;
        } catch (_) { return null; }
      },
      args: [flagName, domId],
    });
    const merged = {};
    for (const r of results || []) {
      if (!r || !r.result) continue;
      for (const k of Object.keys(r.result)) {
        if (!merged[k] || r.result[k] > merged[k]) merged[k] = r.result[k];
      }
    }
    return Object.keys(merged).length ? merged : null;
  } catch (_) {
    return null;
  }
}

// For postmessage: the target window is opened by the attacker via
// window.open — we didn't create that tab, so we have to find it by
// URL. The attacker uses _buildProbeUrl(pageUrl, "hash", marker) so
// we match on that canonical form.
async function _findProbeTargetTab(pageUrl, marker) {
  try {
    const tabs = await chrome.tabs.query({});
    const needleMarker = "__apisec=" + marker;
    for (const t of tabs) {
      if (t.url && t.url.indexOf(needleMarker) !== -1) return t.id;
    }
  } catch (_) {}
  return null;
}

async function _runExploitProbe(session) {
  // The caller MUST supply pageUrl — it comes from the finding's own
  // observed pageUrl (globalStore.securityFindings entry's pageUrl is
  // set when the finding was recorded on a tab). We never guess which
  // tab to probe against: if the caller doesn't know where the finding
  // was observed, we refuse to probe rather than hit a random tab.
  if (!session.pageUrl || !/^https?:/i.test(session.pageUrl)) {
    throw new Error("pageUrl required — caller should read it from the finding's stored pageUrl (globalStore.securityFindings[sourceUrl].pageUrl)");
  }

  let execReadTabId = null;
  let attackerPostedTargetUrl = null;
  try {
    if (session.strategy === "postmessage") {
      // Cross-origin attacker page: example.com is a real third-party
      // origin the extension has <all_urls> permission for. Open it,
      // inject an attacker script that window.opens the target with
      // the marker in the hash (so intercept.js arms wrappers at
      // document_start), then postMessage a payload that carries both
      // the sentinel marker AND active HTML/JS payloads. The handler
      // that routes message.data to a sink will trip our execution
      // flag if the sink actually parses the payload.
      const atkTab = await chrome.tabs.create({ url: "https://example.com/", active: false });
      session.openedTabs.push(atkTab.id);
      await _waitForTabLoaded(atkTab.id, 10000);
      const targetWithMarker = _buildProbeUrl(session.pageUrl, "hash", session.marker, { sinkType: session.sinkType, sinkName: session.sinkName });
      attackerPostedTargetUrl = targetWithMarker;
      const payloads = _probePayloads(session.marker);
      // Build the payload SHAPE from the finding's observed field
      // path (taint path member hops). No hardcoded field-name list
      // — if the handler reads event.data.payload.html, the probe
      // delivers {payload:{html:<active>}}. Empty fieldPath = the
      // handler treats event.data as a plain string, so we send a
      // raw string containing the active vectors.
      //    activeStr — the HTML/SVG/DOM concatenation, carrying the
      //      marker in multiple vectors (onerror flag, svg onload,
      //      DOM-presence div).
      // Postmessage payload shape depends on sink type — eval needs a
      // JS statement, redirect needs a javascript: URL, xss/default need
      // HTML vectors (with attribute-breakout prefix). Marker prefix is
      // included in every case so the intercept.js sink wrappers see
      // the sentinel whatever shape the handler unpacks it into.
      let activeStr;
      if (session.sinkType === "eval") activeStr = payloads.js + "/*" + session.marker + "*/";
      else if (session.sinkType === "redirect") activeStr = payloads.href;
      else activeStr = session.marker + ' "\'></script>' + payloads.html + payloads.svg + payloads.dom;
      let shaped = activeStr;
      for (let i = (session.fieldPath || []).length - 1; i >= 0; i--) {
        shaped = { [session.fieldPath[i]]: shaped };
      }
      // Merge preconditions the AST extracted from reachability guards
      // (if/switch/early-return). Each precondition pins a property
      // path to a literal — we set that value on the shaped payload so
      // the handler's guard passes and execution reaches the sink.
      // The path is rooted at the same object as fieldPath (MessageEvent
      // data after dropping the implicit .data hop), so we walk into
      // `shaped` with the same property chain.
      const _precs = Array.isArray(session.preconditions) ? session.preconditions : [];
      for (const p of _precs) {
        if (!p || !Array.isArray(p.path) || p.op !== "===") continue;
        // If the shaped payload is a primitive (no fieldPath), wrap it
        // so we can add sibling properties for the precondition.
        if (typeof shaped !== "object" || shaped === null) shaped = { __apisec_root: shaped };
        let cur = shaped;
        for (let i = 0; i < p.path.length - 1; i++) {
          const key = p.path[i];
          if (typeof cur[key] !== "object" || cur[key] === null) cur[key] = {};
          cur = cur[key];
        }
        if (p.path.length === 0) continue;
        cur[p.path[p.path.length - 1]] = p.value;
      }
      // If we injected the __apisec_root wrapper but no precondition
      // actually required wrapping, unwrap — the handler expects the
      // bare value as event.data.
      if (shaped && typeof shaped === "object" && Object.keys(shaped).length === 1 &&
          Object.prototype.hasOwnProperty.call(shaped, "__apisec_root")) {
        shaped = shaped.__apisec_root;
      }
      // Apply the inverse of each decoder on the taint path in REVERSE
      // order (the handler applies decoders source→sink; we pre-encode
      // sink→source so the decoder chain unpacks back to `shaped`).
      // Each transformer here must mirror a decoder the AST observed —
      // no speculative encoding.
      const _decs = Array.isArray(session.decoders) ? session.decoders : [];
      for (let i = _decs.length - 1; i >= 0; i--) {
        const d = _decs[i];
        try {
          if (d === "json") shaped = JSON.stringify(shaped);
          else if (d === "escape") shaped = escape(String(shaped));
          else if (d === "uri-component") shaped = encodeURIComponent(String(shaped));
          else if (d === "uri") shaped = encodeURI(String(shaped));
          else if (d === "base64") shaped = btoa(unescape(encodeURIComponent(String(shaped))));
        } catch (_) {}
      }
      await chrome.scripting.executeScript({
        target: { tabId: atkTab.id },
        world: "MAIN",
        injectImmediately: true,
        func: (tUrl, mk, shapedPayload) => {
          const win = window.open(tUrl, "_blank");
          // Retry so handler-registration timing races don't lose the
          // payload. The TARGET window sees the exact shape the AST
          // observed it reading — no guesses.
          const deliveries = [2500, 5000, 8000];
          for (const delay of deliveries) {
            setTimeout(() => { try { win && win.postMessage(shapedPayload, "*"); } catch (_) {} }, delay);
          }
        },
        args: [targetWithMarker, session.marker, shaped],
      });
      await new Promise((r) => setTimeout(r, 4000 + session.waitMs));
    } else {
      // URL-reachable strategies: new tab navigated to targetUrl with
      // marker + active payload embedded. intercept.js arms wrappers
      // at document_start; if the page consumes location.hash|search|
      // pathname and routes it to a sink, the active payload fires.
      const probeUrl = _buildProbeUrl(session.pageUrl, session.strategy, session.marker, { paramName: session.paramName, sinkType: session.sinkType, sinkName: session.sinkName });
      const tab = await chrome.tabs.create({ url: probeUrl, active: false });
      session.openedTabs.push(tab.id);
      execReadTabId = tab.id;
      await _waitForTabLoaded(tab.id, 10000);
      await new Promise((r) => setTimeout(r, session.waitMs));
    }

    // READ EXECUTION FLAG before cleanup — this is what distinguishes
    // a real PoC ("the page actually rendered our payload") from a
    // taint-only hit ("the marker string reached a sink but was
    // displayed as text, not interpreted as HTML/JS").
    if (!execReadTabId && attackerPostedTargetUrl) {
      execReadTabId = await _findProbeTargetTab(session.pageUrl, session.marker);
    }
    if (execReadTabId) {
      session.executed = await _readProbeExecFlag(execReadTabId, session.marker);
    }
    session.status = "done";
  } catch (e) {
    session.status = "error";
    session.error = (e && e.message) || String(e);
  } finally {
    session.finishedAt = Date.now();
    // Close tabs the extension opened. Tabs popped by window.open
    // inside pages are left so the reviewer can inspect state after
    // the probe finishes.
    for (const tid of session.openedTabs) {
      try { await chrome.tabs.remove(tid); } catch (_) {}
    }
    session.openedTabs = [];
  }
}

async function buildExportRequest(tabId, msg) {
  let parsedUrl;
  try {
    parsedUrl = new URL(msg.url);
  } catch (_) {
    return { error: "invalid URL" };
  }

  const headers = { ...(msg.headers || {}) };
  if (
    msg.contentType &&
    msg.httpMethod !== "GET" &&
    msg.httpMethod !== "DELETE"
  ) {
    headers["Content-Type"] = msg.contentType;
  }

  // API key: user override → endpoint → auto
  const tab = getTab(tabId);
  const ep = msg.endpointKey ? tab.endpoints.get(msg.endpointKey) : null;
  if (msg.apiKeyOverride) {
    if (!msg.apiKeyOverride.disabled && msg.apiKeyOverride.key) {
      if (msg.apiKeyOverride.source === "url") {
        parsedUrl.searchParams.set("key", msg.apiKeyOverride.key);
      } else {
        headers["X-Goog-Api-Key"] = msg.apiKeyOverride.key;
      }
    }
  } else if (ep?.apiKey) {
    if (ep.apiKeySource === "url") {
      parsedUrl.searchParams.set("key", ep.apiKey);
    } else {
      headers["X-Goog-Api-Key"] = ep.apiKey;
    }
  }

  const url = parsedUrl.toString();

  let body = null;
  if (msg.httpMethod !== "GET" && msg.httpMethod !== "DELETE" && msg.body) {
    // Check for multipart batch sub-request
    const _exportBatchMethod = (() => {
      if (!msg.service || !msg.methodId) return null;
      const docEntry = tab.discoveryDocs.get(msg.service) || globalStore.discoveryDocs.get(msg.service);
      if (!docEntry?.doc) return null;
      const mName = msg.methodId.split(".").pop();
      return docEntry.doc.resources?.learned?.methods?.[mName];
    })();

    if (msg.body.mode === "multipart" && Array.isArray(msg.body.parts)) {
      // Generic multipart reassembly: N editable parts → multipart envelope
      // with a fresh boundary. Each part carries its own Content-Type chosen
      // by the user in the per-part editor. No data is dropped even when
      // sub-parts use different formats (JSON, GraphQL, form-urlencoded,
      // raw) — the contextual editor produced the body string already.
      const boundary = "uasr_" + Date.now().toString(36) + "_" + Math.random().toString(36).slice(2, 8);
      const sections = msg.body.parts.map((p) => {
        const h = ["Content-Type: " + (p.contentType || "application/octet-stream")];
        if (p.contentId) h.push("Content-ID: <" + p.contentId + ">");
        if (p.extraHeaders) {
          for (const [hk, hv] of Object.entries(p.extraHeaders)) {
            const lk = hk.toLowerCase();
            if (lk === "content-type" || lk === "content-id") continue;
            h.push(hk + ": " + hv);
          }
        }
        // If the part represents an embedded HTTP sub-request (Google batch
        // pattern: `application/http` parts), keep the embedded request line.
        // Otherwise the part body is raw and we just attach it.
        let sectionBody = p.body || "";
        if ((p.contentType || "").toLowerCase().startsWith("application/http") && p.method && p.path) {
          sectionBody = p.method + " " + p.path + " HTTP/1.1\r\n" +
            "Content-Type: application/json\r\n\r\n" + sectionBody;
        }
        return h.join("\r\n") + "\r\n\r\n" + sectionBody;
      });
      body = "--" + boundary + "\r\n" + sections.join("\r\n--" + boundary + "\r\n") + "\r\n--" + boundary + "--";
      headers["Content-Type"] = "multipart/mixed; boundary=" + boundary;
    } else if (_exportBatchMethod?._batchPart && msg.body.mode === "form") {
      const fields = msg.body.formData?.fields || [];
      const jsonBody = JSON.stringify(encodeFormToJson(fields));
      const partPath = _exportBatchMethod.path;
      const partMethod = _exportBatchMethod.httpMethod || "GET";
      const boundary = "batch_" + Date.now();
      body = `--${boundary}\r\nContent-Type: application/http\r\n\r\n` +
        `${partMethod} ${partPath} HTTP/1.1\r\n` +
        `Content-Type: application/json\r\nAccept: application/json\r\n\r\n` +
        jsonBody + `\r\n--${boundary}--`;
      headers["Content-Type"] = `multipart/mixed; boundary=${boundary}`;
    } else if (url.includes("batchexecute") && msg.body.mode === "form") {
      const fields = msg.body.formData?.fields || [];
      const argsArray = encodeFormToJspb(fields);
      const innerJson = JSON.stringify(argsArray);
      const rpcId = msg.methodId ? msg.methodId.split(".").pop() : "unknown";
      const envelope = [[[rpcId, innerJson, null, "generic"]]];
      const params = new URLSearchParams();
      params.set("f.req", JSON.stringify(envelope));
      body = params.toString();
      headers["Content-Type"] =
        "application/x-www-form-urlencoded;charset=UTF-8";
    } else if (msg.body.mode === "raw" && msg.body.rawBody) {
      body = msg.body.rawBody;
    } else if (msg.body.mode === "form" && msg.body.formData?.fields?.length) {
      const fields = msg.body.formData.fields;
      if (
        msg.contentType === "application/grpc-web+proto" ||
        msg.contentType === "application/grpc-web-text+proto"
      ) {
        // gRPC-Web: encode protobuf, wrap in frame
        const pbBytes = encodeFormToProtobuf(fields);
        const framed = encodeGrpcWebFrame(pbBytes);
        body = uint8ToBase64(framed);
      } else if (
        msg.contentType === "application/x-protobuf" ||
        msg.contentType === "application/x-protobuffer" ||
        msg.contentType === "application/protobuf" ||
        msg.contentType === "application/vnd.google.protobuf"
      ) {
        // `application/x-protobuffer` is Google reCAPTCHA's non-standard
        // spelling — without handling it explicitly the body falls through
        // to JSON, which turns the protobuf field tree into
        // `{"field1":[byte,byte,...]}` gibberish.
        const encoded = encodeFormToProtobuf(fields);
        body = uint8ToBase64(encoded);
      } else if (msg.contentType === "application/json+protobuf") {
        body = JSON.stringify(encodeFormToJspb(fields));
      } else if (msg.contentType?.startsWith("application/x-www-form-urlencoded")) {
        // Standard form-urlencoded: each field is its own key=value pair.
        // The batchexecute `f.req` envelope is only correct for
        // `/batchexecute` URLs (handled above at the URL-match branch); it
        // must not be applied to plain form POSTs like recaptcha/userverify
        // or analytics beacons, which would collapse every field into a
        // single `f.req=[…]` key and lose all the original values.
        const params = new URLSearchParams();
        for (const f of fields) {
          if (f.value == null) continue;
          const v = f.value;
          if (f.label === "repeated" && Array.isArray(v)) {
            for (const item of v) params.append(f.name, String(item));
          } else if (typeof v === "object") {
            params.append(f.name, JSON.stringify(v));
          } else {
            params.append(f.name, String(v));
          }
        }
        body = params.toString();
      } else {
        body = JSON.stringify(encodeFormToJson(fields));
      }
    }
  }

  // GraphQL: wrap query/variables in standard envelope
  if (isGraphQLUrl(url) && msg.body?.mode === "graphql") {
    body = encodeGraphQLBody(msg.body);
    headers["Content-Type"] = "application/json";
  }

  return { url, method: msg.httpMethod || "POST", headers, body };
}

const EXTENSION_ORIGIN = `chrome-extension://${chrome.runtime.id}`;
const CONTENT_TYPES = new Set([
  "CONTENT_KEYS",
  "CONTENT_ENDPOINTS",
  "CONTENT_FORMS",
  "CONTENT_FORM_SUBMIT",
  "RESPONSE_BODY",
  "SCRIPT_SOURCE",
  "PROBE_HIT",
]);

// Threat model: Content scripts run in web page renderer processes. A compromised
// renderer has our extension's sender.id (since we inject into every page), so
// sender.id only rejects other extensions. The real security gate is sender.url —
// set by the browser process, unforgeable by the renderer. This router enforces:
//   1. sender.id must match our extension (rejects other extensions)
//   2. sender.url origin check (extension page vs content script — unforgeable)
//   3. Extension pages → handlePopupMessage (rejects CONTENT_TYPES)
//   4. Content scripts → handleContentMessage (rejects everything except CONTENT_TYPES)
// Data-returning types (GET_STATE, GET_ALL_LOGS, GET_TAB_LIST) are only reachable
// from extension pages, never from content scripts. See SECURITY.md.
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (sender.id !== chrome.runtime.id) return;

  const isExtensionPage =
    sender.url && sender.url.startsWith(EXTENSION_ORIGIN + "/");

  if (isExtensionPage) {
    if (CONTENT_TYPES.has(msg.type)) return;
    handlePopupMessage(msg, sender, sendResponse);
    return true; // keep sendResponse alive for async handlePopupMessage
  }

  if (!CONTENT_TYPES.has(msg.type)) return;
  handleContentMessage(msg, sender);
});

chrome.tabs.onRemoved.addListener((tabId) => {
  // Keep session storage logs so closed tab requests remain viewable
  const meta = _tabMeta.get(tabId);
  if (meta) {
    const tab = state.tabs.get(tabId);
    meta.closed = true;
    meta.closedAt = Date.now();
    meta.count = tab ? tab.requestLog.length : meta.count || 0;
  }
  state.tabs.delete(tabId);
  _wsConnState.delete(tabId);
  _tabFrames.delete(tabId);
  // Clean up script buffer and cancel pending analysis
  var buf = _scriptBuffers.get(tabId);
  if (buf && buf.timer) clearTimeout(buf.timer);
  _scriptBuffers.delete(tabId);
  saveSessionIndex();
});

// ─── Send Request: Schema Resolution ─────────────────────────────────────────

/**
 * Resolve the full schema for an endpoint by merging discovery doc + probe data.
 * Returns a unified schema the popup can use to build a form.
 */
function resolveEndpointSchema(tabId, endpointKey, service, methodId) {
  const tab = getTab(tabId);
  const ep = endpointKey
    ? tab.endpoints.get(endpointKey) || globalStore.endpoints.get(endpointKey)
    : null;

  // If no endpoint but we have service+methodId (virtual), create a dummy ep object for context
  if (!ep && (!service || !methodId)) return { source: "none", endpoint: null };

  const targetService = ep?.service || service;

  let source = "none";
  let discoveryMethod = null;
  let parameters = null;
  let bodyFields = null;
  let bodySchemaName = null;
  let contentTypes = [];

  // 1. Try discovery doc (tab-specific first, then global store fallback)
  let discoveryEntry = tab.discoveryDocs.get(targetService);
  if (discoveryEntry?.status === "found" && !discoveryEntry.doc) {
    // Tab has a found entry but no full doc — check global store for in-memory doc
    const globalEntry = globalStore.discoveryDocs.get(targetService);
    if (globalEntry?.doc) discoveryEntry = globalEntry;
  }
  if (!discoveryEntry?.doc) {
    // Also try global store directly if tab has no entry
    const globalEntry = globalStore.discoveryDocs.get(targetService);
    if (globalEntry?.status === "found" && globalEntry.doc)
      discoveryEntry = globalEntry;
  }
  if (discoveryEntry?.status === "found" && discoveryEntry.doc) {
    const doc = discoveryEntry.doc;
    let match = null;

    if (methodId) {
      // Direct lookup by ID (virtual endpoint)
      match = findMethodById(doc, methodId);
    } else if (ep) {
      // Path matching (captured endpoint)
      match = findDiscoveryMethod(doc, ep.path, ep.method || "POST");
    }

    if (match) {
      source = "discovery";
      discoveryMethod = {
        id: match.method.id,
        httpMethod: match.method.httpMethod,
        path: match.method.path || match.method.flatPath,
        description: match.method.description,
        scopes: match.method.scopes || [],
        resourceName: match.resourceName,
        contentTypes: match.method.contentTypes || [],
      };

      // Resolve parameters
      if (match.method.parameters) {
        parameters = {};
        for (const [pName, pDef] of Object.entries(match.method.parameters)) {
          parameters[pName] = {
            name: pDef.name || pName,
            customName: !!pDef.customName,
            type: pDef.type || "string",
            location: pDef.location || "query",
            required: !!pDef.required,
            description: pDef.description || "",
            format: pDef.format || null,
            enum: pDef.enum || null,
            // Stats-derived metadata
            _requiredConfidence: pDef._requiredConfidence ?? null,
            _detectedEnum: !!pDef._detectedEnum,
            _defaultValue: pDef._defaultValue ?? null,
            _defaultConfidence: pDef._defaultConfidence ?? null,
            _range: pDef._range || null,
            // Unified example value (pickExampleValue result) — popup
            // uses this to prefill the Send form so reviewers can
            // send a plausible request without first replaying a
            // captured one. The source tag lets the UI label the
            // prefill (observed / ast / synthesized / type-default).
            _exampleValue: pDef._exampleValue === undefined ? null : pDef._exampleValue,
            _exampleValueSource: pDef._exampleValueSource || null,
            // AST-discovered valid values
            _astValidValues: pDef._astValidValues || null,
            _astValueSource: pDef._astValueSource || null,
          };
        }
      }

      // Resolve request body schema
      if (match.method.request?.$ref) {
        bodySchemaName = match.method.request.$ref;
        bodyFields = resolveDiscoverySchema(doc, bodySchemaName);
      }
    }
  }

  // 2. Try probe results (only if we have a real endpoint key)
  const probeResult = endpointKey
    ? tab.probeResults.get(endpointKey) ||
      globalStore.probeResults.get(endpointKey)
    : null;
  if (probeResult?.fields) {
    const probeFields = Object.entries(probeResult.fields).map(([name, f]) => ({
      name,
      type: f.type || "string",
      number: f.number || null,
      required: !!f.required,
      label: f.label || "optional",
      messageType: f.messageType || null,
      description: null,
      children: f.children || null,
    }));

    if (!bodyFields || bodyFields.length === 0) {
      // No discovery body fields — use probe fields directly
      source = source === "discovery" ? "merged" : "probe";
      bodyFields = probeFields;
    } else {
      // Merge: overlay probe field numbers onto discovery fields
      source = "merged";
      for (const pf of probeFields) {
        const match = bodyFields.find(
          (df) => df.name.toLowerCase() === pf.name.toLowerCase(),
        );
        if (match) {
          if (pf.number) match.number = pf.number;
          if (pf.type !== "unknown" && match.type === "string")
            match.type = pf.type;
          if (pf.label === "repeated") match.label = "repeated";
          if (pf.children && !match.children) match.children = pf.children;
        } else {
          bodyFields.push(pf);
        }
      }
    }
  }

  // 3. Content type suggestions — prefer method-level observed CTs
  if (discoveryMethod?.contentTypes?.length) {
    for (const ct of discoveryMethod.contentTypes) {
      if (!contentTypes.includes(ct)) contentTypes.push(ct);
    }
  }
  if (ep?.contentType && !contentTypes.includes(ep.contentType)) {
    contentTypes.push(ep.contentType);
  }
  if (probeResult?.probeDetails) {
    for (const pd of probeResult.probeDetails) {
      if (
        pd.fieldCount > 0 &&
        pd.contentType &&
        !contentTypes.includes(pd.contentType)
      ) {
        contentTypes.push(pd.contentType);
      }
    }
  }
  if (!contentTypes.length) {
    contentTypes = [
      "application/json",
      "application/json+protobuf",
      "application/x-protobuf",
    ];
  }

  // 4. Collect chain data from the raw method object
  let chains = null;
  if (discoveryEntry?.doc && methodId) {
    const rawMatch = findMethodById(discoveryEntry.doc, methodId);
    if (rawMatch?.method?._chains) {
      chains = rawMatch.method._chains;
    }
  }

  return {
    source,
    method: discoveryMethod,
    parameters,
    requestBody: bodyFields?.length
      ? { schemaName: bodySchemaName, fields: bodyFields }
      : null,
    contentTypes,
    chains,
    endpoint: ep
      ? {
          url: ep.url,
          method: ep.method,
          host: ep.host,
          path: ep.path,
          service: ep.service,
          apiKey: ep.apiKey,
          apiKeySource: ep.apiKeySource,
          origin: ep.origin,
          referer: ep.referer,
          contentType: ep.contentType,
        }
      : null,
  };
}

// ─── Send Request: Body Encoding ─────────────────────────────────────────────

/**
 * Encode form fields as a JSON object (field names as keys).
 */
/**
 * Encode GraphQL body from popup operations array.
 * Supports single and batched (array) format, preserves extensions.
 */
function encodeGraphQLBody(bodyMsg) {
  const ops = bodyMsg.operations || [];
  const encode = (op) => {
    // Reddit-style persisted-operation envelope: the server maps `operation`
    // to a stored query doc. No query text goes over the wire. We emit the
    // exact shape the server expects instead of forcing a spec-compliant
    // `{query}` envelope that reddit's backend would reject.
    let obj;
    if (op.operation && !op.query) {
      obj = { operation: op.operation };
      if (op.variables) {
        try { obj.variables = typeof op.variables === "string" ? JSON.parse(op.variables) : op.variables; }
        catch (_) { obj.variables = op.variables; }
      }
      if (op.extensions) {
        try { obj.extensions = typeof op.extensions === "string" ? JSON.parse(op.extensions) : op.extensions; }
        catch (_) { obj.extensions = op.extensions; }
      }
    } else {
      obj = { query: op.query || "" };
      if (op.variables) {
        try { obj.variables = typeof op.variables === "string" ? JSON.parse(op.variables) : op.variables; }
        catch (_) { obj.variables = op.variables; }
      }
      if (op.operationName) obj.operationName = op.operationName;
      if (op.extensions) {
        try { obj.extensions = typeof op.extensions === "string" ? JSON.parse(op.extensions) : op.extensions; }
        catch (_) { obj.extensions = op.extensions; }
      }
    }
    // Attach any extra top-level fields preserved from the captured
    // envelope (csrf_token, clientId, rid, ...). Existing standard keys
    // win if a collision happens.
    if (op.extra && typeof op.extra === "object") {
      for (const k in op.extra) {
        if (!(k in obj)) obj[k] = op.extra[k];
      }
    }
    return obj;
  };
  if (bodyMsg.batched) return JSON.stringify(ops.map(encode));
  return JSON.stringify(ops.length > 0 ? encode(ops[0]) : { query: "" });
}

function encodeFormToJson(fields) {
  const obj = {};
  for (const f of fields) {
    const isObj = f.type === "message" || f.type === "object";

    // Repeated fields — both scalar arrays and arrays of objects must
    // roundtrip. Items may carry their own `children` tree (built by the
    // repeated-message renderer) OR be raw objects/scalars from replay
    // initialData. Handle both.
    if (f.label === "repeated") {
      let list;
      if (Array.isArray(f.value)) {
        list = f.value.map((v) => {
          if (v && typeof v === "object" && !Array.isArray(v) && Array.isArray(v.children)) {
            return encodeFormToJson(v.children);
          }
          return isObj ? v : coerceValue(v, f.type);
        });
      } else if (Array.isArray(f.children)) {
        // Children as a list of message instances — each item is a
        // sub-field whose own children describe one array element.
        list = f.children.map((item) =>
          Array.isArray(item.children) ? encodeFormToJson(item.children) : coerceValue(item.value, f.type),
        );
      } else {
        list = [];
      }
      obj[f.name] = list;
      continue;
    }

    if (isObj) {
      // Message/object: prefer children tree; fall back to raw value when
      // the caller has a parsed object but no tree (e.g. replay auto-fill
      // from captured JSON). Always surface the field even when empty so
      // servers see `{variables: {}}` rather than dropping it.
      if (Array.isArray(f.children) && f.children.length) {
        obj[f.name] = encodeFormToJson(f.children);
      } else if (f.value && typeof f.value === "object" && !Array.isArray(f.value)) {
        obj[f.name] = f.value;
      } else {
        obj[f.name] = {};
      }
      continue;
    }

    if (f.value == null && !f.children?.length) continue;
    obj[f.name] = coerceValue(f.value, f.type);
  }
  return obj;
}

/**
 * Encode form fields as a JSPB array (indexed by field number).
 */
function encodeFormToJspb(fields) {
  let maxNum = 0;
  for (const f of fields) {
    if (f.number > maxNum) maxNum = f.number;
  }
  if (maxNum === 0) {
    // If we have no numbered fields, but it's supposed to be an object/message,
    // return an empty array if we are in a JSPB context.
    return [];
  }

  // JSPB uses 0-based indexing for field 1 (i.e. index 0 is field 1)
  const arr = new Array(maxNum).fill(null);
  for (const f of fields) {
    if (!f.number) continue;
    
    const targetIdx = f.number - 1;
    if (f.type === "message" && f.label !== "repeated") {
      arr[targetIdx] = encodeFormToJspb(f.children || []);
    } else if (f.label === "repeated" && f.type === "message" && Array.isArray(f.value)) {
      // Repeated message: each item's children must be recursively encoded
      arr[targetIdx] = f.value.map((item) => {
        if (item && item.children) return encodeFormToJspb(item.children);
        if (Array.isArray(item)) return item;
        return item;
      });
    } else if (f.label === "repeated" && Array.isArray(f.value)) {
      arr[targetIdx] = f.value.map((v) => coerceValue(v, f.type));
    } else {
      arr[targetIdx] = coerceValue(f.value, f.type);
    }
  }
  return arr;
}

/**
 * Encode form fields as binary protobuf.
 */
function encodeFormToProtobuf(fields) {
  const parts = [];
  for (const f of fields) {
    if (!f.number) continue;
    if (f.value == null && !f.children?.length) continue;
    if (f.label === "repeated" && Array.isArray(f.value)) {
      // Packed encoding for repeated scalar numeric types (proto3 default)
      const packableTypes = [
        "int32", "int64", "uint32", "uint64", "sint32", "sint64",
        "bool", "enum", "fixed32", "fixed64", "sfixed32", "sfixed64",
        "float", "double",
      ];
      if (packableTypes.includes(f.type)) {
        const innerParts = [];
        for (const v of f.value) {
          innerParts.push(encodeSinglePbFieldRaw(f.type, v));
        }
        const packed = concatBytes.apply(null, innerParts.length ? innerParts : [new Uint8Array(0)]);
        parts.push(pbEncodeLenField(f.number, packed));
      } else {
        // Non-packable types (string, bytes, message): individual tag+value pairs
        for (const v of f.value) {
          parts.push(encodeSinglePbField(f.number, f.type, v, null));
        }
      }
    } else {
      parts.push(encodeSinglePbField(f.number, f.type, f.value, f.children));
    }
  }
  return concatBytes.apply(null, parts.length ? parts : [new Uint8Array(0)]);
}

function encodeSinglePbField(num, type, value, children) {
  if (type === "message" && children?.length) {
    const inner = encodeFormToProtobuf(children);
    return pbEncodeLenField(num, inner);
  }
  switch (type) {
    case "string":
      return pbEncodeLenField(num, String(value));
    case "bytes":
      return pbEncodeLenField(num, base64ToUint8(String(value)));
    case "bool":
      return pbEncodeVarintField(num, value ? 1 : 0);
    case "enum":
    case "int32":
    case "int64":
    case "uint32":
    case "uint64":
      return pbEncodeVarintField(num, Number(value) || 0);
    case "sint32":
    case "sint64": {
      // Arithmetic ZigZag to avoid 32-bit truncation from bitwise ops
      const n = Number(value) || 0;
      const zigzag = n >= 0 ? n * 2 : (-n) * 2 - 1;
      return pbEncodeVarintField(num, zigzag);
    }
    case "float":
    case "fixed32":
    case "sfixed32": {
      const buf = new Uint8Array(4);
      if (type === "float")
        new DataView(buf.buffer).setFloat32(0, Number(value) || 0, true);
      else new DataView(buf.buffer).setUint32(0, Number(value) || 0, true);
      return concatBytes(pbTag(num, PB_32BIT), buf);
    }
    case "double": {
      const buf = new Uint8Array(8);
      new DataView(buf.buffer).setFloat64(0, Number(value) || 0, true);
      return concatBytes(pbTag(num, PB_64BIT), buf);
    }
    case "fixed64":
    case "sfixed64": {
      // 64-bit integer encoding (not float64)
      const buf = new Uint8Array(8);
      const n = Number(value) || 0;
      const dv = new DataView(buf.buffer);
      dv.setUint32(0, n >>> 0, true);
      dv.setUint32(4, Math.floor(n / 0x100000000) >>> 0, true);
      return concatBytes(pbTag(num, PB_64BIT), buf);
    }
    default:
      return pbEncodeLenField(num, String(value));
  }
}

/**
 * Encode a single protobuf scalar value WITHOUT the field tag.
 * Used for packed repeated encoding where values are concatenated inside
 * a single length-delimited field.
 */
function encodeSinglePbFieldRaw(type, value) {
  switch (type) {
    case "bool":
      return pbWriteVarint(value ? 1 : 0);
    case "enum":
    case "int32":
    case "int64":
    case "uint32":
    case "uint64":
      return pbWriteVarint(Number(value) || 0);
    case "sint32":
    case "sint64": {
      const n = Number(value) || 0;
      return pbWriteVarint(n >= 0 ? n * 2 : (-n) * 2 - 1);
    }
    case "float":
    case "fixed32":
    case "sfixed32": {
      const buf = new Uint8Array(4);
      if (type === "float")
        new DataView(buf.buffer).setFloat32(0, Number(value) || 0, true);
      else new DataView(buf.buffer).setUint32(0, Number(value) || 0, true);
      return buf;
    }
    case "double": {
      const buf = new Uint8Array(8);
      new DataView(buf.buffer).setFloat64(0, Number(value) || 0, true);
      return buf;
    }
    case "fixed64":
    case "sfixed64": {
      const buf = new Uint8Array(8);
      const n = Number(value) || 0;
      const dv = new DataView(buf.buffer);
      dv.setUint32(0, n >>> 0, true);
      dv.setUint32(4, Math.floor(n / 0x100000000) >>> 0, true);
      return buf;
    }
    default:
      return pbWriteVarint(Number(value) || 0);
  }
}

function coerceValue(value, type) {
  if (value == null) return null;
  if (type === "bool" || type === "boolean") return value === true || value === "true";
  if (type === "enum") {
    var n = Number(value);
    return isNaN(n) ? String(value) : n;
  }
  if (
    type === "number" ||
    [
      "int32",
      "int64",
      "uint32",
      "uint64",
      "double",
      "float",
      "sint32",
      "sint64",
      "fixed32",
      "fixed64",
      "sfixed32",
      "sfixed64",
    ].includes(type)
  ) {
    // Already a number? Preserve exactly — `Number(42)` → 42, but
    // `Number("42")` also → 42 and crucially `String(42)` would emit `"42"`
    // which breaks JSON byte-equivalence.
    return typeof value === "number" ? value : Number(value);
  }
  // Numeric-typed JSON values without an explicit scalar-typed field still
  // need to stay numbers. Same for booleans and null-ish passthroughs.
  if (typeof value === "number" || typeof value === "boolean") return value;
  return String(value);
}

// ─── Send Request: Execute ───────────────────────────────────────────────────

/**
 * Execute a request from the Send panel.
 * Encodes form data, sends via pageContextFetch, decodes response.
 */
async function executeSendRequest(tabId, msg) {
  const startTime = Date.now();
  const service = msg.service;
  const methodId = msg.methodId;

  // Validate URL
  let parsedUrl;
  try {
    parsedUrl = new URL(msg.url);
    if (parsedUrl.protocol !== "http:" && parsedUrl.protocol !== "https:") {
      return { error: "blocked: invalid protocol" };
    }
  } catch (_) {
    return { error: "invalid URL" };
  }

  // Build headers
  const headers = { ...(msg.headers || {}) };
  if (
    msg.contentType &&
    msg.httpMethod !== "GET" &&
    msg.httpMethod !== "DELETE"
  ) {
    headers["Content-Type"] = msg.contentType;
  }

  // API key: user override → endpoint → service keys → discovery doc key
  const tab = getTab(tabId);
  const epKey = msg.endpointKey;
  const ep = epKey ? tab.endpoints.get(epKey) : null;
  let apiKey = null;
  let apiKeySource = "header";

  if (msg.apiKeyOverride) {
    // User explicitly selected a key (or disabled injection) from the Send panel
    if (msg.apiKeyOverride.disabled) {
      apiKey = null; // Skip all auto-selection
    } else {
      apiKey = msg.apiKeyOverride.key || null;
      apiKeySource = msg.apiKeyOverride.source || "header";
    }
  } else {
    apiKey = ep?.apiKey || null;
    apiKeySource = ep?.apiKeySource || "header";
  }

  if (!msg.apiKeyOverride && !apiKey && service) {
    const hostname = parsedUrl.hostname;
    const svcKeys = collectKeysForService(tab, service, hostname);
    // Also check globalStore for keys from previous sessions
    if (svcKeys.length === 0) {
      for (const [key, data] of globalStore.apiKeys) {
        if (data.services?.has(service) || data.hosts?.has(hostname)) {
          svcKeys.push(key);
        }
      }
    }
    if (svcKeys.length > 0) {
      // Prefer key from same pageUrl origin as current tab
      var _skTabMeta = _tabMeta.get(tabId);
      var _skOrigin = null;
      if (_skTabMeta && _skTabMeta.url) {
        try { _skOrigin = new URL(_skTabMeta.url).origin; } catch (_) {}
      }
      if (_skOrigin && svcKeys.length > 1) {
        var _skBest = null;
        for (var _ski = 0; _ski < svcKeys.length; _ski++) {
          var _skData = tab.apiKeys.get(svcKeys[_ski]) || globalStore.apiKeys.get(svcKeys[_ski]);
          if (_skData && _skData.pageUrls) {
            var _skPages = _skData.pageUrls instanceof Set ? _skData.pageUrls : new Set(_skData.pageUrls);
            for (var _skPurl of _skPages) {
              try { if (new URL(_skPurl).origin === _skOrigin) { _skBest = svcKeys[_ski]; break; } } catch (_) {}
            }
            if (_skBest) break;
          }
        }
        apiKey = _skBest || svcKeys[0];
      } else {
        apiKey = svcKeys[0];
      }
      // Look up the actual location (url vs specific header name) the key
      // was originally observed in — keys captured from
      // `X-Goog-Api-Key` shouldn't be re-emitted as Google-branded
      // headers against non-Google targets like statsigapi.
      if (apiKey) {
        var _skStoredData = tab.apiKeys.get(apiKey) || globalStore.apiKeys.get(apiKey);
        if (_skStoredData && _skStoredData.source) {
          apiKeySource = _skStoredData.source;
        } else {
          apiKeySource = null; // unknown origin — don't guess a header name
        }
      }
    }
    // Fall back to discovery doc's key
    if (!apiKey) {
      const docEntry = tab.discoveryDocs.get(service) || globalStore.discoveryDocs.get(service);
      if (docEntry?.apiKey) apiKey = docEntry.apiKey;
    }
  }

  // Only add key if not already present in headers or URL
  const hasKeyHeader = headers["X-Goog-Api-Key"] || headers["x-goog-api-key"];
  const hasKeyParam = parsedUrl.searchParams.has("key");
  if (apiKey && !hasKeyHeader && !hasKeyParam) {
    // apiKeySource carries either "url", "header:<name>", or a legacy
    // "header" (no name). Only inject when we know the exact location —
    // silently defaulting to X-Goog-Api-Key for arbitrary third-party
    // hosts pollutes their requests with a Google-branded header that
    // the server doesn't recognize. Fall back to X-Goog-Api-Key only for
    // Google-ish hostnames where it is the genuine convention.
    if (apiKeySource === "url") {
      parsedUrl.searchParams.set("key", apiKey);
    } else if (typeof apiKeySource === "string" && apiKeySource.startsWith("header:")) {
      var _hdrName = apiKeySource.slice("header:".length);
      headers[_hdrName] = apiKey;
    } else if (/\.google(?:apis)?\.com$/i.test(parsedUrl.hostname) || /\.clients6\.google\.com$/i.test(parsedUrl.hostname)) {
      headers["X-Goog-Api-Key"] = apiKey;
    }
    // Otherwise skip auto-attach — let the user pick explicitly via the
    // Send panel's key selector if they want to try a specific key.
  }

  const url = parsedUrl.toString();

  // Encode body
  let body = null;
  let bodyEncoding = null;

  if (msg.httpMethod !== "GET" && msg.httpMethod !== "DELETE" && msg.body) {
    // Check if this is a multipart batch sub-request (_batchPart methods)
    const _batchPartMethod = (() => {
      if (!service || !methodId) return null;
      const docEntry = tab.discoveryDocs.get(service) || globalStore.discoveryDocs.get(service);
      if (!docEntry?.doc) return null;
      const mName = methodId.split(".").pop();
      return docEntry.doc.resources?.learned?.methods?.[mName];
    })();

    if (_batchPartMethod?._batchPart && msg.body.mode === "form") {
      // Multipart batch: wrap form fields in a single-part multipart body
      const fields = msg.body.formData?.fields || [];
      const jsonBody = JSON.stringify(encodeFormToJson(fields));
      const partPath = _batchPartMethod.path;
      const partMethod = _batchPartMethod.httpMethod || "GET";
      const boundary = "batch_" + Date.now();
      body = `--${boundary}\r\nContent-Type: application/http\r\n\r\n` +
        `${partMethod} ${partPath} HTTP/1.1\r\n` +
        `Content-Type: application/json\r\nAccept: application/json\r\n\r\n` +
        jsonBody + `\r\n--${boundary}--`;
      headers["Content-Type"] = `multipart/mixed; boundary=${boundary}`;
    } else if (url.includes("batchexecute") && msg.body.mode === "form") {
      // Special handling for batchexecute: wrap in f.req envelope
      const fields = msg.body.formData?.fields || [];
      const argsArray = encodeFormToJspb(fields);
      const innerJson = JSON.stringify(argsArray);

      // Extract RPC ID from methodId (e.g. "Google.Photos.p1Takd" -> "p1Takd")
      const rpcId = methodId ? methodId.split(".").pop() : "unknown";

      const envelope = [[[rpcId, innerJson, null, "generic"]]];
      const params = new URLSearchParams();
      params.set("f.req", JSON.stringify(envelope));

      body = params.toString();
      headers["Content-Type"] =
        "application/x-www-form-urlencoded;charset=UTF-8";
    } else if (msg.body.mode === "raw" && msg.body.rawBody) {
      if (
        msg.contentType === "application/x-protobuf" ||
        msg.contentType === "application/grpc-web+proto" ||
        msg.contentType === "application/grpc-web-text+proto"
      ) {
        body = msg.body.rawBody;
        bodyEncoding = "base64";
      } else {
        body = msg.body.rawBody;
      }
    } else if (msg.body.mode === "form" && msg.body.formData?.fields?.length) {
      const fields = msg.body.formData.fields;
      if (
        msg.contentType === "application/grpc-web+proto" ||
        msg.contentType === "application/grpc-web-text+proto"
      ) {
        // gRPC-Web: encode protobuf, wrap in frame
        const pbBytes = encodeFormToProtobuf(fields);
        const framed = encodeGrpcWebFrame(pbBytes);
        body = uint8ToBase64(framed);
        bodyEncoding = "base64";
      } else if (msg.contentType === "application/x-protobuf") {
        const encoded = encodeFormToProtobuf(fields);
        body = uint8ToBase64(encoded);
        bodyEncoding = "base64";
      } else if (msg.contentType === "application/json+protobuf") {
        body = JSON.stringify(encodeFormToJspb(fields));
      } else if (msg.contentType?.startsWith("application/x-www-form-urlencoded")) {
        // Form-urlencoded with f.req JSPB (non-batchexecute)
        const argsArray = encodeFormToJspb(fields);
        const params = new URLSearchParams();
        params.set("f.req", JSON.stringify(argsArray));
        body = params.toString();
      } else {
        body = JSON.stringify(encodeFormToJson(fields));
      }
    }
  }

  // GraphQL: wrap query/variables in standard envelope
  if (isGraphQLUrl(url) && msg.body?.mode === "graphql") {
    body = encodeGraphQLBody(msg.body);
    headers["Content-Type"] = "application/json";
  }

  // Send request via page context (session-aware)
  let resp;
  try {
    resp = await pageContextFetch(
      tabId,
      url,
      {
        method: msg.httpMethod || "POST",
        headers,
        body,
        bodyEncoding,
      },
      msg.frameId,
    );
  } catch (err) {
    return { error: `fetch_exception: ${err.message}`, timing: Date.now() - startTime };
  }

  const timing = Date.now() - startTime;

  if (!resp || resp.error) {
    return { error: resp?.error || "fetch_failed: no response", timing };
  }

  // Decode response
  const respCt = resp.headers?.["content-type"] || "";
  let bodyResult;

  if (isGrpcWeb(respCt)) {
    // gRPC-Web: pass raw bytes for frame-level rendering in popup
    try {
      let bytes;
      if (isGrpcWebText(respCt)) {
        bytes = base64ToUint8(
          resp.bodyEncoding === "base64" ? resp.body : btoa(resp.body),
        );
      } else {
        bytes = resp.bodyEncoding === "base64"
          ? base64ToUint8(resp.body)
          : new TextEncoder().encode(resp.body);
      }
      // Scan protobuf frames for keys
      const parsed = parseGrpcWebFrames(bytes);
      if (parsed) {
        for (const frame of parsed.frames) {
          if (frame.type !== "data") continue;
          try {
            pbDecodeTree(frame.data, 8, (val) => {
              if (typeof val === "string") {
                extractKeysFromText(tabId, val, url, "send_response_grpc");
              }
            });
          } catch (_) {}
        }
      }
      // Serialize bytes as base64 array for message passing
      bodyResult = {
        format: "grpc_web",
        bytesB64: uint8ToBase64(bytes),
        raw: resp.body,
        size: bytes.length,
      };
    } catch (_) {
      bodyResult = {
        format: "binary",
        parsed: null,
        raw: resp.body,
        size: (resp.body || "").length,
      };
    }
  } else if (
    (resp.bodyEncoding === "base64" || isBinaryContentType(respCt)) &&
    (/^(image|video|audio)\//i.test(respCt) || /application\/(pdf|zip)/i.test(respCt))
  ) {
    // Non-API binary (media/document) — pass through for download
    const size = resp.bodyEncoding === "base64"
      ? Math.floor(resp.body.length * 3 / 4)
      : resp.body.length;
    bodyResult = {
      format: "binary_download",
      raw: resp.body,
      bodyEncoding: resp.bodyEncoding || "text",
      contentType: respCt,
      size,
    };
  } else if (resp.bodyEncoding === "base64" || isBinaryContentType(respCt)) {
    // Binary protobuf response
    try {
      const bytes =
        resp.bodyEncoding === "base64"
          ? base64ToUint8(resp.body)
          : new TextEncoder().encode(resp.body);
      const tree = pbDecodeTree(bytes, 8, (val) => {
        if (typeof val === "string") {
          extractKeysFromText(tabId, val, url, "send_response_protobuf");
        }
      });
      bodyResult = {
        format: "protobuf_tree",
        parsed: tree,
        raw: resp.body,
        size: bytes.length,
      };
    } catch (_) {
      bodyResult = {
        format: "binary",
        parsed: null,
        raw: resp.body,
        size: (resp.body || "").length,
      };
    }
  } else {
    // Try JSON parse (strip Google XSSI prefix if present)
    let jsonText = resp.body || "";
    if (jsonText.trimStart().startsWith(")]}'")) {
      jsonText = jsonText.trimStart().substring(4).trimStart();
    }
    try {
      const parsed = JSON.parse(jsonText);
      if (
        Array.isArray(parsed) &&
        (respCt.includes("json+protobuf") ||
          (respCt.includes("text/plain") &&
            parsed.length > 0 &&
            parsed.some((item) => item === null || Array.isArray(item) || typeof item !== "object")) ||
          (respCt.includes("json") &&
            parsed.length > 0 &&
            parsed.some((item) => item === null || Array.isArray(item) || typeof item !== "object")))
      ) {
        // JSPB format: json+protobuf content-type, or text/plain/json with array structure
        bodyResult = {
          format: "protobuf_tree",
          parsed: jspbToTree(parsed),
          raw: resp.body,
          size: (resp.body || "").length,
          isJspb: true,
        };
      } else {
        bodyResult = {
          format: "json",
          parsed,
          raw: resp.body,
          size: (resp.body || "").length,
        };
      }
    } catch (_) {
      bodyResult = {
        format: "text",
        parsed: null,
        raw: resp.body || "",
        size: (resp.body || "").length,
      };
    }
  }

  // Include latest discovery info in result
  const discovery = tab.discoveryDocs.get(msg.service);

  return {
    ok: resp.ok,
    status: resp.status,
    statusText: resp.statusText || "",
    headers: resp.headers || {},
    body: bodyResult,
    timing,
    discovery, // Pass back latest doc (+ summary/apiKey)
    service, // Echo back metadata
    methodId,
    error: null,
  };
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

function notifyPopup(tabId) {
  chrome.runtime.sendMessage({ type: "STATE_UPDATED", tabId }).catch(() => {});
}

function mergeVirtualParts(newDoc, oldDoc) {
  if (!oldDoc || !newDoc) return newDoc;

  // Preserve "learned" methods (deep copy to avoid aliasing)
  if (oldDoc.resources?.learned) {
    if (!newDoc.resources) newDoc.resources = {};
    newDoc.resources.learned = JSON.parse(JSON.stringify(oldDoc.resources.learned));
  }

  // Preserve "probed" methods (deep copy to avoid aliasing)
  if (oldDoc.resources?.probed) {
    if (!newDoc.resources) newDoc.resources = {};
    newDoc.resources.probed = JSON.parse(JSON.stringify(oldDoc.resources.probed));
  }

  // Preserve learned schemas + carry over custom renames into new schemas
  if (oldDoc.schemas) {
    for (const [name, schema] of Object.entries(oldDoc.schemas)) {
      if (!newDoc.schemas[name]) {
        newDoc.schemas[name] = schema;
      } else {
        // Schema exists in both — preserve customName fields from old
        const oldProps = schema.properties || {};
        const newProps = newDoc.schemas[name].properties || {};
        for (const [pKey, pVal] of Object.entries(oldProps)) {
          if (pVal.customName && newProps[pKey]) {
            newProps[pKey].name = pVal.name;
            newProps[pKey].customName = true;
          }
        }
      }
    }
  }

  // Carry over custom parameter renames from old methods
  if (oldDoc.resources) {
    function carryRenames(oldRes, newRes) {
      if (!oldRes || !newRes) return;
      for (const [rName, r] of Object.entries(oldRes)) {
        if (!newRes[rName]) continue;
        for (const [mName, oldM] of Object.entries(r.methods || {})) {
          const newM = newRes[rName]?.methods?.[mName];
          if (!newM) continue;
          // Carry parameter renames
          if (oldM.parameters) {
            for (const [pName, pVal] of Object.entries(oldM.parameters)) {
              if (pVal.customName && newM.parameters?.[pName]) {
                newM.parameters[pName].name = pVal.name;
                newM.parameters[pName].customName = true;
              }
            }
          }
          // Carry stats and chains
          if (oldM._stats && !newM._stats) newM._stats = oldM._stats;
          if (oldM._chains && !newM._chains) newM._chains = oldM._chains;
        }
      }
    }
    carryRenames(oldDoc.resources, newDoc.resources);
  }

  return newDoc;
}

function serializeApiKeyEntry(v) {
  return {
    name: v.name,
    origin: v.origin,
    referer: v.referer,
    source: v.source,
    firstSeen: v.firstSeen,
    lastSeen: v.lastSeen,
    requestCount: v.requestCount || 0,
    services: [...(v.services instanceof Set ? v.services : v.services || [])],
    hosts: [...(v.hosts instanceof Set ? v.hosts : v.hosts || [])],
    endpoints: [
      ...(v.endpoints instanceof Set ? v.endpoints : v.endpoints || []),
    ],
    pageUrls: [
      ...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || []),
    ],
  };
}

function mergedSecurityFindings(tab) {
  // Global base (keyed by sourceUrl), tab overwrites
  var merged = new Map();
  for (const [k, v] of globalStore.securityFindings) {
    merged.set(k, v);
  }
  if (tab._securityFindings) {
    for (var i = 0; i < tab._securityFindings.length; i++) {
      var f = tab._securityFindings[i];
      merged.set(f.sourceUrl || ("unknown_" + i), f);
    }
  }
  return [...merged.values()];
}

function serializeTabData(tab) {
  // Merge global store (base) with tab data (tab wins on conflict)

  // API keys: global base, tab overwrites
  const mergedKeys = {};
  for (const [k, v] of globalStore.apiKeys) {
    mergedKeys[k] = serializeApiKeyEntry(v);
  }
  for (const [k, v] of tab.apiKeys) {
    mergedKeys[k] = serializeApiKeyEntry(v);
  }

  // Endpoints: global base, tab overwrites
  const mergedEndpoints = {};
  for (const [k, v] of globalStore.endpoints) {
    mergedEndpoints[k] = v;
  }
  for (const [k, v] of tab.endpoints) {
    mergedEndpoints[k] = v;
  }

  // Scopes: global base, tab overwrites
  const mergedScopes = {};
  for (const [k, v] of globalStore.scopes) {
    mergedScopes[k] = v;
  }
  for (const [k, v] of tab.scopes) {
    mergedScopes[k] = v;
  }

  // Discovery docs: global base, tab overwrites with full doc
  const mergedDiscovery = {};
  for (const [k, v] of globalStore.discoveryDocs) {
    if (v.status === "found") {
      mergedDiscovery[k] = {
        status: v.status,
        url: v.url,
        method: v.method,
        apiKey: v.apiKey || null,
        fetchedAt: v.fetchedAt,
        doc: v.doc || null,
        grouping: v.grouping || null,
        isVirtual: !!v.isVirtual,
        pageUrls: [...(v.pageUrls instanceof Set ? v.pageUrls : v.pageUrls || [])],
        frameOrigins: [...(v.frameOrigins instanceof Set ? v.frameOrigins : v.frameOrigins || [])],
      };
    } else {
      mergedDiscovery[k] = { status: v.status };
    }
  }
  for (const [k, v] of tab.discoveryDocs) {
    if (v.status === "found") {
      // Merge pageUrls/frameOrigins from global base if present
      var _existingMerged = mergedDiscovery[k];
      var _allPageUrls = new Set(_existingMerged?.pageUrls || []);
      if (v.pageUrls) for (var _pu of v.pageUrls) _allPageUrls.add(_pu);
      var _allFrameOrigins = new Set(_existingMerged?.frameOrigins || []);
      if (v.frameOrigins) for (var _fo of v.frameOrigins) _allFrameOrigins.add(_fo);
      mergedDiscovery[k] = {
        status: v.status,
        url: v.url,
        method: v.method,
        apiKey: v.apiKey || null,
        fetchedAt: v.fetchedAt,
        doc: v.doc || null,
        grouping: v.grouping || (_existingMerged && _existingMerged.grouping) || null,
        isVirtual: v.isVirtual || (_existingMerged && _existingMerged.isVirtual) || false,
        pageUrls: [..._allPageUrls],
        frameOrigins: [..._allFrameOrigins],
      };
    } else {
      mergedDiscovery[k] = { status: v.status };
    }
  }
  // Probe results: global base, tab overwrites
  const mergedProbe = {};
  for (const [k, v] of globalStore.probeResults) {
    mergedProbe[k] = v;
  }
  for (const [k, v] of tab.probeResults) {
    mergedProbe[k] = v;
  }

  return {
    apiKeys: mergedKeys,
    endpoints: mergedEndpoints,
    authContext: tab.authContext,
    scopes: mergedScopes,
    discoveryDocs: mergedDiscovery,
    probeResults: mergedProbe,
    requestLog: tab.requestLog || [],
    securityFindings: mergedSecurityFindings(tab),
  };
}

