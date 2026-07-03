// offscreen-brain.js — the API-learning brain. Runs in the OFFSCREEN document
// (loaded by ast-worker.html, which provides the lib/* globals via <script> tags
// and the analysis Web Worker via ast-worker.js). It owns globalStore + discovery
// + AST merge + the request log + ALL the popup message handlers, and persists to
// IndexedDB — a stable home, unlike the evicted service worker. The thin service
// worker (background.js) forwards browser events here and performs privileged
// chrome.tabs.* calls on this brain's behalf (see _swRpc / __rpc).
//
// Was background.js (the service worker) until the brain moved to the offscreen.
// Differences from a SW: no importScripts (libs are <script>s in ast-worker.html);
// no chrome.tabs/scripting/webNavigation (those are RPC'd to the SW or arrive as
// forwarded __evt messages); the analysis worker is reached via self.astDispatch.

// ─── AST Cache Fingerprint ──────────────────────────────────────────────────
// The fingerprint hashes the analyzer worker files at first use — any
// change to ast-thread.js, hostedge.gen.js, or qjs_worker.js auto-
// invalidates cached results because the cache key shifts. Replaces
// the prior manual AST_ANALYSIS_VERSION constant that had to be bumped
// by hand whenever the analyzer output shape changed (smell:
// non-additive shape changes leaked across versions; never bumping
// meant stale cached results deserialized into new consumer code wrong).
var _analyzerFingerprint = null;
var _analyzerFingerprintP = null;
async function getAnalyzerFingerprint() {
  if (_analyzerFingerprint) return _analyzerFingerprint;
  if (_analyzerFingerprintP) return _analyzerFingerprintP;
  _analyzerFingerprintP = (async () => {
    var files = ["ast-worker.js", "lib/qjs/qjs.mjs"];   // v2 host bridge + engine module (wasm bytes bump via qjs.mjs's embedded fingerprint is out of scope; a wasm rebuild that changes behavior should bump ast-worker.js)
    var hashes = [];
    for (var i = 0; i < files.length; i++) {
      try {
        var resp = await fetch(chrome.runtime.getURL(files[i]));
        var txt = await resp.text();
        hashes.push(await _hashScriptSHA256(txt));
      } catch (e) {
        // A file missing during a partial build/stage cycle would
        // produce a wrong fingerprint; treat as not-yet-cacheable.
        hashes.push(null);
      }
    }
    if (hashes.indexOf(null) >= 0) {
      _analyzerFingerprintP = null;       // retry on next call
      return null;
    }
    _analyzerFingerprint = hashes.join(".").slice(0, 32);
    return _analyzerFingerprint;
  })();
  return _analyzerFingerprintP;
}

// ─── Analysis Web Worker dispatch ─────────────────────────────────────────────
// The brain runs IN the offscreen document; the analysis Web Worker (ast-thread.js)
// is owned by ast-worker.js (loaded in the SAME document), which exposes
// self.astDispatch(msg) → Promise<response>. AST_* messages go straight to it —
// chrome.runtime.sendMessage would NOT reach a same-document listener (the
// sender's own context is excluded from the broadcast).
function sendToOffscreen(msg) {
  if (typeof self.astDispatch === "function") return self.astDispatch(msg);
  return Promise.resolve({ success: false, error: "analysis worker dispatch unavailable" });
}
// The offscreen document's lifecycle + the cross-session resume kick are owned by
// the thin service worker (background.js). Nothing to create from inside it.
function ensureOffscreen() { return Promise.resolve(); }

// Privileged chrome.tabs.* / webNavigation.* calls the offscreen can't make — the
// thin service worker performs them and returns the result. Resolves to the
// result, rejects on error. (chrome.scripting.executeScript injects a function
// that can't be serialized across this boundary; the exploit-probe that needs it
// runs in the SW, not here.)
function swRpc(api) {
  var args = Array.prototype.slice.call(arguments, 1);
  return chrome.runtime.sendMessage({ __rpc: true, api: api, args: args }).then(function (r) {
    if (!r || !r.ok) throw new Error((r && r.error) || ("swRpc failed: " + api));
    return r.result;
  });
}

// External fetches go through the single safeFetch (lib/safe-fetch.js, loaded
// before this file): direct, GET only, cookies omitted, http(s) only. There is no
// swFetch / SW relay any more — the offscreen document fetches cross-origin itself.
// Credentialed same-origin requests (schema.verify) still use pageContextFetch.

// Inlined from ast.js — extracts sourceMappingURL from the last 500 chars.
// Runs synchronously in the service worker (no Babel needed).
function extractSourceMapUrl(code) {
  var tail = code.length > 500 ? code.slice(-500) : code;
  var marker = "sourceMappingURL=";
  var idx = tail.lastIndexOf(marker);   // last occurrence = the real trailing annotation
  if (idx === -1) return null;
  var start = idx + marker.length;
  while (start < tail.length && (tail.charCodeAt(start) === 32 || tail.charCodeAt(start) === 9)) start++;
  var end = start;
  while (end < tail.length && tail.charCodeAt(end) > 32) end++;
  var url = tail.substring(start, end);
  // Strip the trailing `*/` from the block-comment form `/*# … */` (github
  // ships both `//#` line and `/*# … */` block styles) so the fetch URL
  // doesn't end in `*/` and 404.
  var star = url.indexOf("*/");
  if (star >= 0) url = url.slice(0, star);
  return url.length ? url : null;
}

// ─── State ───────────────────────────────────────────────────────────────────

const state = {
  // Map<documentId, DocData> — per-document analysis state. tabId/frameId are
  // FIELDS on each entry (UI filter for the network tab + Chrome routing), NEVER
  // storage keys. The cross-site cumulative moat lives in `globalStore` (global).
  docs: new Map(),
};

// Host CPU slice (resource-pressure proxy): a page with more than this many forced STARTERS parks its
// deep residue to the GLOBAL frontier (resumed later by value order) instead of running to completion.
// Large enough that ordinary pages finish in one visit; only genuinely huge bundles park -> BFS across
// sites without regressing normal per-visit completeness.
const FRONTIER_QUANTUM = 256;

// GLOBAL network/PostMessage/MessageChannel traffic stream. The request log is
// NOT per-document: a document is evicted from `state.docs` once its grind
// completes (_maybeEvictReviewedDoc), but its captured traffic must survive that
// eviction. So the log is ONE global array; each entry carries {tabId,
// documentId, frameId} metadata for the popup's tab/frame filtering +
// interaction (BUILD/SEND off a log entry). It is a session view
// (offscreen-lifetime, NOT persisted to IDB — the per-service schemas in
// globalStore are the durable moat). Complex sites (LinkedIn, Booking, Figma,
// Vercel, Discord, Spotify) saturate a small cap in under a minute; 2000 keeps
// memory bounded across ALL tabs while leaving headroom for a real multi-tab
// session. When exceeded, the oldest entries are evicted; the per-service
// schemas still persist to globalStore independent of this cap, so dropped log
// entries don't undo learning — only the request-log replay view loses them.
const MAX_REQUEST_LOG_ENTRIES = 2000;
let globalRequestLog = [];
// Push a captured traffic entry onto the global log, stamping the {tabId,
// documentId, frameId} filter metadata from the SENDER (not state.docs — the
// doc may already be evicted, and a log push must never re-create it). Newest
// first; oldest trimmed at the cap.
function _pushGlobalLog(entry, tabId, documentId, frameId) {
  entry.tabId = tabId != null ? tabId : null;
  entry.documentId = documentId || null;
  if (entry.frameId == null) entry.frameId = frameId != null ? frameId : 0;
  globalRequestLog.unshift(entry);
  while (globalRequestLog.length > MAX_REQUEST_LOG_ENTRIES) globalRequestLog.pop();
}

const _wsConnState = new Map(); // documentId → Map<wsId, { url, readyState }>

// -- Review-completion eviction ---------------------------------------------
// A document whose analysis has COMPLETED (its ONE forced-exec run merged to
// globalStore, its frontier residue parked to IDB via frontierPut) is dropped
// from in-memory state.docs to keep RAM bounded across a long many-site session.
// Its learnings live in globalStore (the moat) + globalRequestLog (traffic); its
// unfinished exploration lives in the GLOBAL frontier (replay recipes), resumed by
// the host WFQ — not by holding the doc's transient buffer in RAM. Navigate-away
// never forces eviction; a doc is evictable once analysed and not re-analysing.
// The sweep is debounced so it runs after the merge has settled.
var _evictSweepTimer = null;
function _scheduleEvictSweep() {
  if (_evictSweepTimer) return;
  _evictSweepTimer = setTimeout(function () { _evictSweepTimer = null; _evictReviewedDocs(); }, 1000);
}
function _evictReviewedDocs() {
  var gone = [];
  state.docs.forEach(function (doc, documentId) {
    // Reviewed = its forced-exec run produced results (merged to globalStore) AND it is not
    // currently being (re-)analysed. The parked frontier (IDB recipes) carries any residue.
    var reviewed = doc && (doc._astResults || doc._astError) && !_analysisInflight.has(documentId);
    if (reviewed) gone.push(documentId);
  });
  for (var i = 0; i < gone.length; i++) _evictReviewedDoc(gone[i]);
  if (gone.length) console.debug("[evict] dropped %d reviewed document(s) from state.docs", gone.length);
}
function _evictReviewedDoc(documentId) {
  // Drop the per-document analysis state + its BIG transient buffer (the combined
  // source) + transient side-maps. NEVER touch globalRequestLog (the log is
  // global) or _docOrigins (the credentialed-read principal stays; late same-doc
  // traffic is routed to globalStore-only by _docForLearning, never resurrecting).
  state.docs.delete(documentId);
  _scriptBuffers.delete(documentId);
  _wsConnState.delete(documentId);
  _contentPings.delete(documentId);
}

// A document's SECURITY ORIGIN for same-origin checks — the BROWSER-provided
// MessageSender.origin of the REQUESTING FRAME (authoritative), NEVER parsed from
// sender.url/tab.url and NEVER the top frame's: a page can sandbox its own iframes,
// giving a sub-frame an OPAQUE origin even though its URL/embedder looks normal, so
// parsing the URL (or using the main frame) would wrongly grant it same-origin
// access to the embedder's credentialed data. A VALID tuple origin is used as-is
// (the better check). An opaque origin ("null") is UNIQUE per the spec, so it gets
// a STABLE random token (crypto.randomUUID) keyed by sender.documentId — the only
// reliable per-document identity (a (tab,frame) id pair is reused across navigations
// with a DIFFERENT origin, and several documents can share one origin). Two opaque
// documents must NEVER compare same-origin. No documentId (pre-106 Chrome) -> a
// fresh token per call -> never same-origin -> credentialed read fails closed.
// Consumed by safeFetch's credentialed SOP as opts.pageOrigin.
// The single durable documentId -> origin map. The ONLY identity that survives
// a document's analysis (the script buffer is reclaimed after review, so it can't
// back this map) and the ONLY one that distinguishes two about:blank / sandboxed
// frames (same url, DIFFERENT origin). A valid tuple origin is stored as-is; an
// opaque origin gets a STABLE random token, both keyed by sender.documentId. Look
// up later with _originForDoc(documentId) — never by tabId (assumes the main
// frame) and never by URL-parsing (about:blank parses to a bogus "null").
const _docOrigins = new Map(); // documentId -> origin ("https://x" | "null:<uuid>")
function _senderOrigin(sender) {
  var docId = sender && sender.documentId;
  var o = sender && sender.origin;
  if (typeof o === "string" && o.indexOf("://") > 0) {         // valid tuple origin — used as-is
    if (docId) _docOrigins.set(docId, o);                      // remember durably for later lookups
    return o;
  }
  if (!docId) return "null:" + crypto.randomUUID();            // no stable key -> fail closed, unstorable
  var tok = _docOrigins.get(docId);
  if (!tok) { tok = "null:" + crypto.randomUUID(); _docOrigins.set(docId, tok); }
  return tok;
}
// Authoritative origin for a documentId, recoverable after the buffer is gone.
// "" if the document never reported (caller must fail closed — never assume same-origin).
function _originForDoc(documentId) {
  return (documentId && _docOrigins.get(documentId)) || "";
}

// Prioritization (recency + grind focus) is NOT driven by browser navigation or
// tab events anymore — those operate at the TAB level and can only guess which
// frame is the "main" document. It is driven by each document's own CONTENT_HTML
// message (see handleContentMessage): the message arrival IS the "analyze me now"
// signal, per document, with the document's authoritative own url/origin.

// Cross-script AST analysis: buffer scripts per document, debounce, concatenate + analyze
// Per-DOCUMENT analysis state, keyed by documentId (sender.documentId ONLY — no
// tabId:frameId fallback; a doc-less content message fails closed). A tab holds many documents/frames (iframes,
// navigations), each its OWN origin; keying by tab merged them into one buffer
// and collapsed the credentialed-read principal. Each entry: { tabId, docKey,
// origin, url, pageHtml, scripts:[], chunk-state }. One CONTENT_HTML per document
// creates/refreshes its entry; the engine sources the scripts itself (Lexbor
// parses the HTML, qjs_run_doc_scripts runs each <script> in document order —
// inline directly, external <script src> via __feLoadScript).
const _scriptBuffers = new Map(); // documentId → per-document analysis state

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
  // pageUrl(no query) → { scriptOffsets, sourceMapScripts, savedAt }. The deep
  // (chunk-fold) round spawns a resumable grind that can outlive an SW
  // eviction; when it finishes via AST_RESUMED the SW has lost the per-script
  // line map + the chunk source-map URL list, so path-param names (owner/repo)
  // can't be resolved. Persisting just those two small artefacts lets the
  // resume merge re-run the eager path's source-map name resolution.
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

function extractKeysFromText(documentId, text, sourceUrl, sourceContext) {
  if (!text) return;
  const tab = _docForLearning(documentId);
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
        if (tab && tab.url) keyData.pageUrls.add(tab.url);
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

// Per-DOCUMENT analysis state, keyed by documentId — the stable per-document
// identity from MessageSender. tabId/frameId are FIELDS (UI filter + Chrome
// routing), NEVER the key. `origin` is the MessageSender principal (stamped at
// CONTENT_HTML via _originForDoc) — NEVER derived from `url`; it stays "" (fail
// closed) until the document reports. `url` is the document's OWN url (display +
// relative TARGET resolution only).
function getDoc(documentId) {
  if (!state.docs.has(documentId)) {
    state.docs.set(documentId, {
      // ── identity / routing ──
      documentId: documentId,   // storage key (also a field)
      tabId: null,              // UI filter (network tab) + Chrome routing ONLY — never a key
      frameId: 0,               // Chrome routing ONLY (tabs.sendMessage / pageContextFetch)
      origin: "",               // MessageSender principal (set at CONTENT_HTML) — NEVER url-derived
      url: "",                  // the document's OWN url (display + relative TARGET resolution)
      title: "",                // multi-tab log label
      closed: false,            // true once the owning tab closes (logs stay visible)
      // ── learned facts (per-document view; the cumulative moat is globalStore) ──
      apiKeys: new Map(), // key → { origin, referer, firstSeen }
      endpoints: new Map(), // endpointKey → { method, service, key, headers, firstSeen }
      authContext: null, // { sapisid, sapisidhash, cookies }
      discoveryDocs: new Map(), // service → discovery JSON or status
      probeResults: new Map(), // endpointKey → probe result
      scopes: new Map(), // service → string[] of required scopes
      _valueIndex: createValueIndex(), // Chain engine: response value → source tracking
    });
  }
  return state.docs.get(documentId);
}

// All live documents belonging to a browser tab — for AGGREGATE/UI views only
// (the network/log tab filters by tab purely at the UI level). tabId is a field.
function docsForTab(tabId) {
  var out = [];
  state.docs.forEach(function (d) { if (d.tabId === tabId) out.push(d); });
  return out;
}

// Resolve the DocData a popup RPC targets. Per-document RPCs send documentId;
// for robustness (and the migration window) fall back to a tab's main-frame doc
// when only a tabId is sent. Returns null if nothing matches (handlers fail
// closed). Never creates an entry for a bare tabId.
function _docFromMsg(msg) {
  // documentId is the ONLY document key. A tabId does NOT identify a document:
  // a tab holds many documents, "main frame" is a wrong guess, and a (tab,frame)
  // pair is reused across navigations with a DIFFERENT origin. So a doc-less
  // query returns null and the caller falls back to the global-overlay view;
  // tabId stays a UI-filter / Chrome-routing FIELD, never a document resolver.
  // state.docs.get (NOT getDoc) — a query must NEVER create a phantom entry.
  return (msg && msg.documentId && state.docs.get(msg.documentId)) || null;
}

// A transient (unstored) empty DocData. Views that should still surface the
// GLOBAL cumulative moat when no specific document matches — the popup opened
// over a tab with no analyzed page — pass this to serializeTabData, which
// overlays globalStore onto it (preserving the pre-refactor "always show the
// cumulative learnings" behavior). NEVER stored in state.docs.
function _emptyDocView() {
  return {
    documentId: null, tabId: null, frameId: 0, origin: "", url: "", title: "", closed: false,
    apiKeys: new Map(), endpoints: new Map(), authContext: null, discoveryDocs: new Map(),
    probeResults: new Map(), scopes: new Map(), _valueIndex: createValueIndex(),
  };
}

/* Merge ONE global-frontier advance (a background exploration of some ORIGIN's parked residue) into the
   cumulative moat. Keyed by its OWN sourceUrl via a transient view -> globalStore only (the origin's
   page may not be open), the same gone-doc path _mergeDeepResult uses. This is how the host-level WFQ
   arbiter's cross-site/cross-session learning reaches the real netdiff --unused surface. */
function _mergeFrontierResult(sourceUrl, result) {
  try {
    if (!result || !result.fetchCallSites || !result.fetchCallSites.length) return;
    var view = _emptyDocView();
    view.url = sourceUrl || ""; view.tabUrl = sourceUrl || "";
    result.sourceUrl = sourceUrl || result.sourceUrl || "";
    mergeASTResultsIntoVDD(view, [result], null, true);
    mergeToGlobal(view);
  } catch (e) { console.debug("[frontier] merge error: %s", e && e.message); }
}

/* Opportunistic idle burst of the ONE host-level attention: advance the globally-highest-value parked
   frontier a few rounds and merge each per-origin. Serialized (a single in-flight burst) + yields, so
   it NEVER blocks a fresh tab's analysis -- BFS ACROSS sites: a deep site's residue resumes by value in
   the background while new tabs are learned promptly via the review queue. */
var _frontierBurstInFlight = false;
function _driveGlobalFrontierBurst(rounds) {
  if (_frontierBurstInFlight || typeof self.driveFrontier !== "function") return;
  _frontierBurstInFlight = true;
  Promise.resolve().then(async function () {
    try {
      var advances = await self.driveFrontier(rounds || 4);
      for (var i = 0; i < (advances || []).length; i++) _mergeFrontierResult(advances[i].sourceUrl, advances[i].result);
      if (advances && advances.length) { try { notifyPopup(null); } catch (_) {} }
    } catch (e) { console.debug("[frontier] burst error: %s", e && e.message); }
    finally { _frontierBurstInFlight = false; }
  });
}

// The DocData a network-capture learner writes into. A live document -> its
// stored DocData (per-document view + global merge). An evicted (grind-
// complete) or not-yet-created document -> a TRANSIENT view that merges to
// globalStore ONLY, so post-eviction traffic still enriches the cumulative moat
// WITHOUT resurrecting a null-identity doc in state.docs (mirrors the gone-doc
// deep merge). NEVER use getDoc on the passive-traffic path: it would re-create
// the very doc eviction just removed.
function _docForLearning(documentId) {
  return (documentId && state.docs.get(documentId)) || _emptyDocView();
}

// Find a captured channel entry (WS/PM/MC) in the GLOBAL log. The popup
// message console routes by tabId, so scope the match to that tab; channelId
// is per-document (page-generated), hence not globally unique on its own.
function _findLogEntry(tabId, channelId, method) {
  return globalRequestLog.find(function (r) {
    return r.channelId === channelId && r.method === method && r.tabId === tabId;
  }) || null;
}

// The live WS connection state for a channel, across a tab's documents
// (_wsConnState is keyed by documentId).
function _findWsConn(tabId, channelId) {
  var docs = docsForTab(tabId);
  for (var i = 0; i < docs.length; i++) {
    var c = _wsConnState.get(docs[i].documentId);
    if (c) { var conn = c.get(channelId); if (conn) return conn; }
  }
  return null;
}
// (No tab probing: the tab's title/url are populated from the browser-verified
// sender.tab on every content message in handleContentMessage — never a tabs.get
// round-trip and never a webNavigation event.)

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
  // Learned data persists in IndexedDB (the offscreen has it + a stable
  // lifetime). chrome.storage is NOT used: chrome.storage.local is banned, and
  // chrome.storage is not even exposed to the offscreen document — a previous
  // chrome.storage.local read here THREW before the IndexedDB restore ran, so a
  // recreated offscreen came up with an empty store and the popup saw nothing.
  try {
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
    // Evict prior findings whose URL has the same origin+pathname as a
    // script in this round but a different query/hash. Versioned asset
    // URLs (`index.js?v=14` vs `?v=15`) otherwise accumulate forever
    // because each version is keyed separately even though only the
    // latest bundle is live.
    var newBasePaths = new Set();
    for (var _spi = 0; _spi < tab._securityFindings.length; _spi++) {
      try {
        var _u = new URL(tab._securityFindings[_spi].sourceUrl);
        newBasePaths.add(_u.origin + _u.pathname);
      } catch (_) { /* non-URL source (e.g. "unknown_N") — no base path to match */ }
    }
    var staleKeys = [];
    for (var _key of globalStore.securityFindings.keys()) {
      try {
        var _ku = new URL(_key);
        var _kbase = _ku.origin + _ku.pathname;
        if (newBasePaths.has(_kbase)) {
          // Same base path AND full URL changed → new version replaces old.
          var sameUrl = tab._securityFindings.some(function(_f) { return _f.sourceUrl === _key; });
          if (!sameUrl) staleKeys.push(_key);
        }
      } catch (_) { /* skip non-URL keys */ }
    }
    for (var _ski = 0; _ski < staleKeys.length; _ski++) {
      globalStore.securityFindings.delete(staleKeys[_ski]);
    }

    for (var sf = 0; sf < tab._securityFindings.length; sf++) {
      var finding = tab._securityFindings[sf];
      globalStore.securityFindings.set(finding.sourceUrl || ("unknown_" + sf), finding);
    }
  }
  scheduleSave();
}

// Incremented every time the store is wiped (the bin/Clear reset). An analysis
// or its async continuation (the worker round-trip, a source-map re-merge, a
// resume merge) captures the epoch when it starts and bails before writing to
// the store if the epoch has moved — so work already in flight when Clear ran
// can't repopulate the just-wiped store. Eviction-agnostic (unlike a buffer
// check), so it never suppresses a legitimate post-eviction resume merge.
var _dataEpoch = 0;

async function clearGlobalStore() {
  _dataEpoch++;
  globalStore.apiKeys.clear();
  globalStore.endpoints.clear();
  globalStore.discoveryDocs.clear();
  globalStore.probeResults.clear();
  globalStore.scopes.clear();
  globalStore.securityFindings.clear();
  // Drop any SW-side analyses still queued so a pending review can't repopulate
  // the store right after we wipe it (the offscreen worker's running/queued
  // grind is stopped separately via AST_CLEAR before this runs).
  _reviewQueue.length = 0;
  // The AST replay cache is keyed by analyzer-fingerprint + script hashes,
  // so it normally self-invalidates across builds — but an explicit Clear
  // is the tester's "re-analyze from scratch" action, and leaving the cache
  // means the next navigation replays a stale derived result instead of
  // re-running the worker. Clearing it here makes Clear actually clear.
  globalStore.scriptCache.clear();
  try {
    await _idbClear();
  } catch (_) {
    console.error("[Storage] Clear failed:", _);
  }
}

// Load persisted data on startup — handlers must await this before reading globalStore
const _globalStoreReady = loadGlobalStore();

// Cold-start delivery race: if the offscreen brain wasn't alive when content.js
// shipped its initial HTML/SCRIPT_SOURCE at document_idle (Chrome restart, the
// first nav landing inside the ~ensureOffscreen createDocument window), those
// broadcasts went nowhere. Once we're up, ask each live content script in an
// http(s) tab to re-ship — buffer dedup makes it idempotent. tabs.sendMessage
// to a tab without an active content script (e.g. invalidated post extension
// reload) just errors; the catch makes it a no-op. We don't re-broadcast on
// later inits beyond the brain's birth because there's no state for it to
// catch up to past this moment.
// Apply persisted analysis opts (cooling + worker pool size) on brain boot
// so the pool spawns at the user's chosen size BEFORE the first analysis
// arrives. Missing record (first run) is fine — astDispatch keeps its
// default pool of 1.
_globalStoreReady.then(async function () {
  try {
    const opts = await _idbGet("analysisOpts");
    if (opts && typeof opts === "object" && typeof self.astDispatch === "function") {
      self.astDispatch({ type: "SET_ANALYSIS_OPTS", opts: opts });
    }
  } catch (e) {
    console.warn("[brain] applying persisted analysisOpts at boot failed:", e && e.message || e);
  }
});

_globalStoreReady.then(async function () {
  try {
    const tabs = await swRpc("tabs.query", { url: ["http://*/*", "https://*/*"] });
    if (!Array.isArray(tabs)) return;
    for (const t of tabs) {
      if (!t || t.id == null) continue;
      swRpc("tabs.sendMessage", t.id, { type: "RESHIP" }).catch(function (e) {
        // Per-tab reship failures are normal (tab without active content script
        // after extension reload). Don't surface — that's expected; surface
        // ONLY the outer tabs.query failure, since that means we couldn't even
        // enumerate the tabs to reship to and the brain starts up partially blind.
        console.debug("[brain:reship] tab=%d sendMessage failed: %s", t.id, e && e.message || e);
      });
    }
  } catch (e) {
    console.warn("[brain:reship] tabs.query failed at startup — content scripts won't reship buffered scripts: %s", e && e.message || e);
  }
});

/* Session-storage persistence layer removed. Previously the brain mirrored
   request logs to chrome.storage.session so they survived MV3 SW eviction
   (when the brain lived in the SW). The brain now runs in the offscreen
   document — stable lifetime, no eviction — so the global `requestLog`
   is the single authoritative store. scheduleSessionSave /
   saveTabSessionLog / saveSessionIndex / loadSessionLogs / serializeLogEntry
   and all call sites have been deleted. */

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
        // canParse guard — same root-cause fix as the webnav origin handler.
        // A malformed stored m.origin yields null hostname which can't equal
        // hostname; the `continue` below is the correct semantic.
        const origHost = URL.canParse(m.origin) ? new URL(m.origin).hostname : null;
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

  // __feUrlShape path-template params ({id}) survive new URL() as
  // %7B..%7D (the WHATWG path percent-encode set includes {}); decode so
  // a templated method reads `{id}`, not `%7Bid%7D`. Harmless for normal
  // paths — they never contain %7B.
  const _decTpl = (s) => s.replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}");
  const segments = urlObj.pathname.split("/").filter(Boolean).map(_decTpl);
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
/* Hex string → Uint8Array. Pair-by-pair scan; odd-length input is treated
   as ending one nibble early (a malformed body emission). No allocation
   bloat — single typed-array allocated at exact size. */
function _hexToBytes(hex) {
  if (typeof hex !== "string") return new Uint8Array(0);
  const n = hex.length >> 1;
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = parseInt(hex.substr(i * 2, 2), 16) | 0;
  }
  return out;
}

function learnFromAstCallSite(docData, interfaceName, callSite, scriptUrl) {
  // Takes the DocData object directly (not documentId) so a TRANSIENT view
  // (_emptyDocView, documentId=null) can carry a globalStore-only merge for a
  // GONE document without getDoc(null) creating a phantom state.docs entry.
  const tab = docData;

  // Structural @T candidates carry url:null (a host-edge site in
  // unreached code whose value never resolved). They are surfaced as
  // structural candidates / focusedView review items, never as a
  // learnable endpoint — resolving null through new URL() fabricates a
  // bogus "/null" path (origin + String(null)). Skip cleanly.
  if (callSite.url == null || callSite.url === "") return null;

  // Resolve URL. Dynamic / unresolvable → register service-level only,
  // no synthetic method entry (would confuse the reviewer with made-up
  // paths like `dynamic_0`).
  //
  // Inline-content schemes (data:/blob:/about:/javascript:) aren't API
  // endpoints — they're content embedded in the bundle. Skip them so
  // the service list doesn't accumulate empty-host records with
  // garbled paths.
  if (/^(data|blob|about|javascript):/i.test(callSite.url)) return null;
  // Relative URLs resolve against the DOCUMENT's own url at runtime, NOT the
  // script's host. Cross-origin-hosted scripts (e.g. Reddit serves its
  // shreddit bundle from www.redditstatic.com but it fetches against
  // www.reddit.com when the bundle executes on a reddit.com page) would
  // otherwise be misattributed to the script's host. Using THIS document's url
  // (not the tab's top-page url) keeps an iframe's relative fetches on its own
  // origin. Fall back to scriptUrl only when the document url isn't available.
  const isDynamic = /^\$\{|^\(dynamic\)|^\{[a-zA-Z]/.test(callSite.url);
  let csUrl = null;
  if (!isDynamic) {
    try {
      const _baseForRel = (tab && tab.url) ? tab.url : scriptUrl;
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
  // Verb-matched probed lookup (see learnFromRequest for the same rule):
  // a probed POST entry must not absorb GET traffic.
  const probedBase = doc.resources.probed?.methods?.[baseMethodName];
  const probedMethod = (probedBase && probedBase.httpMethod === callSite.method) ? probedBase : null;

  let methodName;
  const existingBase = doc.resources.learned.methods[baseMethodName];
  const existingQualified = doc.resources.learned.methods[qualifiedName];
  if (existingQualified) {
    methodName = qualifiedName;
  } else if (existingBase && existingBase.httpMethod !== callSite.method) {
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
      path: csUrl.pathname.substring(1).replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}"),
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
  // Re-picks the example value when new AST values land — without this,
  // a form-scan-created param whose initial pickExampleValue ran BEFORE
  // any AST values were added would stay frozen at "type-default" even
  // though tier-3 (ast-constraint) is now satisfied.
  const _mergeAstValues = (target, validValues, defaultValue) => {
    let merged = false;
    if (Array.isArray(validValues) && validValues.length) {
      const prev = Array.isArray(target._astValidValues) ? target._astValidValues.slice() : [];
      const before = prev.length;
      for (const vv of validValues) {
        const s = String(vv);
        if (prev.indexOf(s) < 0) prev.push(s);
      }
      if (prev.length > before) merged = true;
      target._astValidValues = prev;
      if (prev.length >= 2 && !target.customEnum && !target.enum) {
        target.enum = prev.slice();
        target._detectedEnum = true;
      }
    }
    if (defaultValue !== undefined) {
      if (target._astDefault !== defaultValue) merged = true;
      target._astDefault = defaultValue;
    }
    if (merged) {
      const ex = pickExampleValue(target, null);
      if (ex) {
        target._exampleValue = ex.value;
        target._exampleValueSource = ex.source;
        if (ex.confidence != null) target._exampleConfidence = ex.confidence;
      } else {
        // No real value was traceable — leave the field without an
        // example so callers can surface the gap rather than acting on
        // a synthesised type-default that nothing in the bundle
        // produced.
        delete target._exampleValue;
        delete target._exampleValueSource;
        delete target._exampleConfidence;
      }
    }
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

  // Path params — the {name} segments __feUrlShape recovered from an
  // opaque path interpolation (e.g. /settings/avatars/{id}, where id is
  // real attacker/server input the bundle interpolated). Registered as
  // location:"path" (required — it IS the path) so the reviewer sees the
  // templated segment as a real parameter; value stays opaque (no
  // example) until real traffic or replay fills it.
  if (callSite.params) {
    for (const p of callSite.params) {
      if ((p.location || "query") !== "path") continue;
      if (!m.parameters[p.name]) {
        m.parameters[p.name] = {
          type: _inferType(p.validValues, p.defaultValue),
          location: "path",
          required: true,
          description: "Learned from AST fetch call site (path template)",
          _astInferred: true,
        };
      }
      // Real declared name from the page's source map (e.g. minified `e` →
      // `owner`), for display; the param key stays the minified name so URL
      // substitution still matches the `{e}` hole.
      if (p._sourceMapName && !m.parameters[p.name]._sourceMapName) {
        m.parameters[p.name]._sourceMapName = p._sourceMapName;
      }
      _mergeAstValues(m.parameters[p.name], p.validValues, p.defaultValue);
    }
  }

  // Reverse cross-doc reconcile: if THIS method is templated ({hole} path
  // segments), fold any existing CONCRETE same-host live records that match
  // the template into it, then drop the duplicate. Closes the dominant
  // first-load split where live requests (en-us/...) created concrete records
  // BEFORE the deep grind learned the template ({userLocale}/...) — the
  // forward `_matchTemplatedMethodAcrossHost` only catches the other order.
  // Each concrete segment aligned with a {hole} becomes that path-param's
  // example value (goal #2); the concrete method's query params / response /
  // request / stats fold in where the template lacks them. Precise match:
  // same segment count, literal segments equal, same HTTP method, matched
  // method's origin host == this host, and the dup must be concrete at >=1
  // hole (else it IS this template, not a distinct concrete record) — so
  // distinct endpoints are never merged.
  if (m && typeof m.path === "string" && m.path.indexOf("{") >= 0) {
    const _tSegs = m.path.split("/").filter(Boolean);
    const _hostname = csUrl.hostname;
    for (const [, _de] of tab.discoveryDocs) {
      if (!_de || !_de.doc || !_de.doc.resources) continue;
      for (const _bucket of Object.values(_de.doc.resources)) {
        if (!_bucket || !_bucket.methods) continue;
        for (const _key of Object.keys(_bucket.methods)) {
          const _cm = _bucket.methods[_key];
          if (!_cm || _cm === m || _cm.httpMethod !== m.httpMethod || typeof _cm.path !== "string") continue;
          const _cSegs = _cm.path.split("/").filter(Boolean);
          if (_cSegs.length !== _tSegs.length) continue;
          const _oh = (_cm.origin && URL.canParse(_cm.origin)) ? new URL(_cm.origin).hostname : null;
          if (_oh !== _hostname) continue;
          let _ok = true, _concreteAtHole = false;
          for (let _i = 0; _i < _tSegs.length; _i++) {
            const _isHole = _tSegs[_i].charAt(0) === "{" && _tSegs[_i].slice(-1) === "}";
            const _cHole = _cSegs[_i].charAt(0) === "{" && _cSegs[_i].slice(-1) === "}";
            if (_isHole) { if (!_cHole) _concreteAtHole = true; continue; }
            if (_tSegs[_i] !== _cSegs[_i]) { _ok = false; break; }
          }
          if (!_ok || !_concreteAtHole) continue;
          for (let _i = 0; _i < _tSegs.length; _i++) {
            if (!(_tSegs[_i].charAt(0) === "{" && _tSegs[_i].slice(-1) === "}")) continue;
            const _val = _cSegs[_i];
            if (_val.charAt(0) === "{") continue;
            const _hole = _tSegs[_i].slice(1, -1);
            if (!m.parameters[_hole]) m.parameters[_hole] = { type: "string", location: "path", required: true, description: "Learned (concrete value from live traffic)" };
            _mergeAstValues(m.parameters[_hole], [_val], _val);
          }
          if (_cm.parameters) for (const _pn in _cm.parameters) { if (!m.parameters[_pn]) m.parameters[_pn] = _cm.parameters[_pn]; }
          if (_cm.response && !m.response) m.response = _cm.response;
          if (_cm.request && !m.request) m.request = _cm.request;
          if (_cm._stats && !m._stats) m._stats = _cm._stats;
          delete _bucket.methods[_key];
        }
      }
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
  if (callSite.headers && typeof callSite.headers === "object") {
    // AST-captured required headers: the SET the bundle actually attached at
    // the host edge (fetch init.headers / XHR setRequestHeader), each entry
    // {kind:"literal",value}|{kind:"opaque"} (older format: bare string).
    const ctEntry = callSite.headers["content-type"] || callSite.headers["Content-Type"];
    const ct = ctEntry && (typeof ctEntry === "string" ? ctEntry : ctEntry.value);
    if (ct && (!m.contentTypes || m.contentTypes.length === 0)) {
      m.contentTypes = [ct];
    }
    // Store the full set per-endpoint as transport metadata (NOT body params),
    // so the Send panel can show "this endpoint needs header X". A literal
    // supersedes an earlier opaque for the same header; real traffic refines.
    if (!m.requiredHeaders) m.requiredHeaders = {};
    for (const hk in callSite.headers) {
      const hv = callSite.headers[hk];
      const norm = (typeof hv === "string") ? { kind: "literal", value: hv } : hv;
      if (!norm || !norm.kind) continue;
      const prev = m.requiredHeaders[hk];
      if (!prev || (prev.kind === "opaque" && norm.kind === "literal")) m.requiredHeaders[hk] = norm;
    }
  }

  // JSON/text request body (fetch init.body from @BODY): the request SCHEMA. Parse the JSON shape into
  // body params (field name + literal example OR opaque = external input), analogous to query params —
  // an opaque field serialized to `{}` (empty object) by JSON.stringify. Non-JSON body kept as raw shape.
  if (typeof callSite.body === "string" && callSite.body) {
    m.requestBodyRaw = callSite.body.slice(0, 600);
    try {
      const parsed = JSON.parse(callSite.body);
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        if (!m.bodyParams) m.bodyParams = {};
        for (const bk in parsed) {
          const bv = parsed[bk];
          const isOpaque = bv === null || bv === "{}" ||
            (typeof bv === "object" && bv && !Array.isArray(bv) && Object.keys(bv).length === 0);
          const norm = isOpaque ? { kind: "opaque" } : { kind: "literal", value: bv };
          const prev = m.bodyParams[bk];
          if (!prev || (prev.kind === "opaque" && norm.kind === "literal")) m.bodyParams[bk] = norm;
        }
      }
    } catch (_) { /* non-JSON body: raw shape retained in requestBodyRaw */ }
  }

  // Binary body: hostedge.bodyShape captured the full byte sequence + the
  // worker's magic-byte sniffer classified the wire format (protobuf,
  // grpc-web, gzip, zlib, json, bytes). For protobuf/grpc-web, decode via
  // lib/protobuf.js (pbDecodeRaw) so each field becomes a body param named
  // f<num>:<wire> with its concrete value as the example. Real wire-format
  // bytes, real values — not a name guess. (#7 protocol classification)
  if (callSite.bodyBinary && typeof callSite.bodyBinary.hex === "string" && typeof self.pbDecodeRaw === "function") {
    const bb = callSite.bodyBinary;
    m.bodyBinary = { byteLength: bb.byteLength | 0, protocol: bb.protocol || "bytes" };
    let pbBytes = null;
    if (bb.protocol === "protobuf") {
      pbBytes = _hexToBytes(bb.hex);
    } else if (bb.protocol === "grpc-web" && bb.hex.length >= 10) {
      // gRPC-Web frame = 1-byte flag + 4-byte BE length + payload. The
      // payload is the protobuf message; strip the 5-byte header.
      pbBytes = _hexToBytes(bb.hex.slice(10));
    }
    if (pbBytes && pbBytes.length > 0) {
      try {
        const fields = self.pbDecodeRaw(pbBytes);
        for (const f of fields) {
          // Field name as `f<num>` (wire format has no names; the .proto
          // descriptor would map it but we don't have one at AST time).
          // Wire-type tag suffixes the param name so the reviewer sees
          // what kind of value lives there (`varint` vs `len` vs `i32`).
          const wireTag = f.wire === 0 ? "varint" :
                          f.wire === 1 ? "i64" :
                          f.wire === 2 ? "len" :
                          f.wire === 5 ? "i32" : ("w" + f.wire);
          const pname = "f" + f.field + ":" + wireTag;
          let example;
          if (f.wire === 0) {
            example = String(f.data);
          } else if (f.wire === 2 && f.data instanceof Uint8Array) {
            // LEN: could be a string or a nested message. Try UTF-8 decode;
            // if the bytes look like a printable string, that's the value.
            // Otherwise emit hex so the bytes are still visible.
            let asString = "";
            try { asString = new TextDecoder("utf-8", { fatal: true }).decode(f.data); }
            catch (_) { asString = ""; }
            example = asString || ("0x" + Array.from(f.data).map(b => (b < 16 ? "0" : "") + b.toString(16)).join(""));
          } else if (f.data instanceof Uint8Array) {
            example = "0x" + Array.from(f.data).map(b => (b < 16 ? "0" : "") + b.toString(16)).join("");
          } else {
            example = String(f.data);
          }
          // Reuse the same param map the JSON body path uses so the popup
          // renders binary fields alongside textual ones uniformly.
          const existing = m.parameters && m.parameters[pname];
          if (!m.parameters) m.parameters = {};
          if (!existing) {
            m.parameters[pname] = { location: "body", _astValidValues: new Set([example]), _astInferred: true };
          } else if (existing._astValidValues) {
            existing._astValidValues.add(example);
          }
        }
      } catch (e) {
        /* pbDecodeRaw rejected — surface so a malformed protobuf body is
           visible (not silently dropped). The bytes stay on m.bodyBinary
           for the popup to inspect raw. */
        console.warn("[brain] protobuf decode failed:", e && e.message || e, "url=" + callSite.url);
      }
    }
  }

  // Apply example-value picker so the Send form has prefills even
  // before any real traffic hits — pickExampleValue's `ast-constraint`
  // tier uses the _astValidValues we just attached. applyStatsToMethod
  // also walks any body-schema props we created with _astInferred:true.
  applyStatsToMethod(m, doc);

  return docEntry;
}

// Find an existing method whose TEMPLATED path matches this concrete request
// path, so network traffic merges into the QuickJS-learned (or earlier
// network-templated) endpoint instead of forking a new per-value method. This
// is what lets QuickJS supply the editable URL structure (/{e}/{a}/…) while the
// network supplies the real example values for those path params: a match routes
// the request to that method, and the path-param value capture below records the
// concrete segment (NDevTK, APIClient) into its stats. Match = same verb, same
// segment count, every non-{…} segment equal, at least one {…} segment. No
// scoring — a literal segment-by-segment structural match.
function _matchTemplatedMethod(learnedMethods, httpMethod, pathname) {
  const segs = pathname.split("/").filter(Boolean);
  if (!segs.length) return null;
  for (const key in learnedMethods) {
    const mm = learnedMethods[key];
    if (!mm || mm.httpMethod !== httpMethod || !mm.path) continue;
    const tps = mm.path.split("/").filter(Boolean);
    if (tps.length !== segs.length) continue;
    let hasTemplate = false, ok = true;
    for (let i = 0; i < tps.length; i++) {
      if (tps[i].startsWith("{") && tps[i].endsWith("}")) { hasTemplate = true; continue; }
      if (tps[i] !== segs[i]) { ok = false; break; }
    }
    if (ok && hasTemplate) return key;
  }
  return null;
}

// Cross-doc template reconcile. Forced-exec may learn an endpoint as a
// TEMPLATED method (e.g. {userLocale}/content-nav/site-header.json) under
// one service grouping, while a concrete live request (en-us/content-nav/
// site-header.json) refines (refineByObservedPrefix) to a DIFFERENT
// same-host service — leaving the same logical endpoint split into a
// templated [ast] record and a concrete [live] one (observed live on
// learn.microsoft.com: /{userLocale}/content-nav vs /en-us/content-nav).
// `_matchTemplatedMethod` only searches one doc, so it can't bridge the
// split. This searches EVERY same-host doc and returns the doc name whose
// templated method matches this concrete path+method, so learnFromRequest
// can redirect the request into that doc — the concrete segment then lands
// as the param's example value (goal #2) instead of duplicating the
// endpoint. Safe: precise segment match (same count; each segment equal or
// a {hole}), same HTTP method only, and the matched method's origin host
// must equal this host — so distinct services aren't mis-merged.
function _matchTemplatedMethodAcrossHost(tab, hostname, httpMethod, pathname) {
  if (!tab || !tab.discoveryDocs) return null;
  for (const [docName, docEntry] of tab.discoveryDocs) {
    if (!docEntry || !docEntry.doc || !docEntry.doc.resources) continue;
    for (const bucket of Object.values(docEntry.doc.resources)) {
      if (!bucket || !bucket.methods) continue;
      const mk = _matchTemplatedMethod(bucket.methods, httpMethod, pathname);
      if (!mk) continue;
      const mm = bucket.methods[mk];
      const oh = (mm && mm.origin && URL.canParse(mm.origin)) ? new URL(mm.origin).hostname : null;
      if (oh === hostname) return docName;
    }
  }
  return null;
}

function learnFromRequest(documentId, interfaceName, entry, headers) {
  const tab = _docForLearning(documentId);
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
  // Cross-doc template reconcile: if forced-exec already learned this endpoint
  // as a templated method under a DIFFERENT same-host service grouping, route
  // this concrete request into THAT doc so the same logical endpoint isn't
  // split into a templated [ast] record + a concrete [live] one. The
  // subsequent _matchTemplatedMethod (below) then finds the template within
  // the redirected doc and merges, recording the concrete segment as the
  // param example. Only redirects on a precise same-host templated match.
  const _crossHostDoc = _matchTemplatedMethodAcrossHost(tab, url.hostname, method, url.pathname);
  if (_crossHostDoc && _crossHostDoc !== interfaceName) {
    interfaceName = _crossHostDoc;
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
    } catch (e) {
      /* GraphQL operation-name detection failed (body wasn't base64 /
         wasn't text / wasn't valid GraphQL syntax). The endpoint still
         registers under its fallback methodName; only the named
         disambiguation between `gql_GetUser` and `gql_GetRepo` on the
         same /graphql URL is missed. Surface so a GraphQL parser
         regression on a real bundle is visible. */
      console.debug("[brain] GraphQL name-hint derive failed:", e && e.message || e, "url=" + entry.url);
    }
  }

  const { methodName: baseMethodName } = calculateMethodMetadata(url, interfaceName, _nameHint);
  const qualifiedName = method.toLowerCase() + "_" + baseMethodName;

  // If this method was already probed with richer schema, update it there
  // instead — but ONLY when the probed entry's HTTP verb matches. A POST
  // probe entry must not absorb GET traffic (or vice versa); they're
  // distinct methods that happen to share a path. Without the verb match,
  // real GET stats land on the POST schema, corrupting the probed entry.
  const probedBase = doc.resources.probed?.methods?.[baseMethodName];
  const probedMethod = (probedBase && probedBase.httpMethod === method) ? probedBase : null;

  // Resolve method name — disambiguate when different HTTP methods hit the same path
  let methodName;
  // A concrete request that matches an existing TEMPLATED method (QuickJS gave
  // /{e}/{a}/… or earlier traffic templatized it) merges into that method, so
  // its real path-segment values become examples for the editable params.
  const _tplMatch = _matchTemplatedMethod(doc.resources.learned.methods, method, url.pathname);
  const existingBase = doc.resources.learned.methods[baseMethodName];
  const existingQualified = doc.resources.learned.methods[qualifiedName];

  if (_tplMatch) {
    methodName = _tplMatch;
  } else if (existingQualified) {
    // Already disambiguated from a prior collision — use qualified name
    methodName = qualifiedName;
  } else if (existingBase && existingBase.httpMethod !== method) {
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
      path: url.pathname.substring(1).replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}"),
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
              path: url.pathname.substring(1).replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}"),
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
            } catch (e) {
              /* Multipart-batch sub-request JSON parse failed for one
                 part — other parts still process. Surface so a malformed
                 sub-request body on an otherwise-valid batch is visible. */
              console.debug("[brain] multipart-batch request-part JSON parse failed:", e && e.message || e, "part=" + partMethodName, "url=" + entry.url);
            }
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
      } catch (e) {
        console.debug("[brain] grpc-web request-body decode failed:", e && e.message || e, "url=" + entry.url);
      }
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
      } catch (e) {
        console.debug("[brain] JSPB request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
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
      } catch (e) {
        console.debug("[brain] x-protobuf request-body decode failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (headers["content-type"]?.includes("json")) {
      try {
        const json = JSON.parse(text);
        const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
        m.request = { $ref: schemaName };
        const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
        mergeSchemaInto(doc, schemaName, newSchema);
      } catch (e) {
        console.debug("[brain] JSON request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
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
      } catch (e) {
        console.debug("[brain] form-urlencoded f.req parse failed:", e && e.message || e, "url=" + entry.url);
      }
    } else if (text && /^[\s﻿\x00-\x1f]*[{\[]/.test(text)) {
      // Structural JSON detection for request bodies whose content-type
      // is text/plain, missing, or anything other than the explicit
      // tagged shapes above. Many analytics + telemetry endpoints
      // (reddit /svc/shreddit/events, GitHub error reporters, snowplow
      // collectors) POST JSON with `Content-Type: text/plain` to dodge
      // CORS preflight. Body STRUCTURE is authoritative — same rule
      // already applied to responses at line ~2183. Without this,
      // observed traffic produces 1000+ orphan body-field stats with
      // 0 declared schema fields (verified on reddit.com events: 35
      // requests captured, 1141 orphan fields, 0 attached).
      try {
        const json = JSON.parse(text);
        if (json !== null && typeof json === "object") {
          const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Request`;
          m.request = { $ref: schemaName };
          const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
          mergeSchemaInto(doc, schemaName, newSchema);
        }
      } catch (e) {
        /* Structural JSON sniff parse failure — text LOOKED like JSON
           (starts with `{`/`[` after whitespace) but JSON.parse rejected
           it. Most often: a partial body / malformed JSON / a JSON
           prefix followed by binary. Surface so the schema-not-learned
           symptom is traceable to the parse failure. */
        console.debug("[brain] structural-JSON request-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
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

  // Track path param values. The path-param detection block above
  // updates m.path to a templated form like `/posts/{path_param1}` and
  // adds the parameter to m.parameters, but the runtime value of the
  // segment was not being recorded as an observation — leaving the
  // example resolver with only a type-default empty string. Walk the
  // current request's segments alongside the (now-templated) m.path and
  // record each {name} segment's value into the same stats bucket as
  // query params, so pickExampleValue picks the observed-top value.
  {
    const reqSegs = url.pathname.split("/").filter(Boolean);
    const tmplSegs = (m.path || "").split("/").filter(Boolean);
    if (tmplSegs.length === reqSegs.length) {
      for (let i = 0; i < tmplSegs.length; i++) {
        const t = tmplSegs[i];
        if (t.startsWith("{") && t.endsWith("}")) {
          const paramName = t.slice(1, -1);
          if (!paramName) continue;
          if (!m._stats.params[paramName]) m._stats.params[paramName] = createParamStats();
          updateParamStats(m._stats.params[paramName], reqSegs[i]);
        }
      }
    }
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
      } catch (e) {
        /* Chain-tracking body parse failed — request body isn't JSON
           (binary protobuf / form-encoded / etc.). chainBody stays
           empty so this request doesn't contribute body-value chain
           links; URL params still flow through findChainLinks above.
           Surface so a chain-detection gap on binary-body endpoints
           is visible. */
        console.debug("[brain] chain-body parse failed:", e && e.message || e, "url=" + entry.url);
      }
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

// Aggregate stats across all array-index variants matching the schema's
// path pattern: schema uses `info[].source`, stats are keyed
// `info[0].source`, `info[1].source`, etc (per flattenObjectValues).
// Walking the schema with `[]` placeholders, we collect the per-index
// stats keys that match the pattern and merge their counters into a
// single virtual stats bucket so pickExampleValue + dominance reporting
// see the full population, not a single index slice.
function _aggregateStatsForSchemaPath(bodyFieldStats, schemaPath) {
  // Build a regex from the schema path: "info[].source" → /^info\[\d+\]\.source$/
  // First swap the `[]` placeholders for a sentinel that won't be touched
  // by regex escaping; escape the rest; then swap the sentinel back to
  // the index pattern. No regex from user input — schemaPath is built
  // by the walker from controlled property names + literal `[]` markers.
  const SENTINEL = "\0ARR\0";
  const escaped = schemaPath
    .split("[]").join(SENTINEL)
    .replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
    .split(SENTINEL).join("\\[\\d+\\]");
  const re = new RegExp("^" + escaped + "$");
  let merged = null;
  for (const key of Object.keys(bodyFieldStats)) {
    if (!re.test(key)) continue;
    const fs = bodyFieldStats[key];
    if (!fs) continue;
    if (!merged) {
      merged = createParamStats();
    }
    // Sum scalar counters
    merged.observed = (merged.observed || 0) + (fs.observed || 0);
    merged.empty = (merged.empty || 0) + (fs.empty || 0);
    merged.absent = (merged.absent || 0) + (fs.absent || 0);
    // Merge value frequency map
    if (fs.values) {
      if (!merged.values) merged.values = {};
      for (const v in fs.values) {
        merged.values[v] = (merged.values[v] || 0) + fs.values[v];
      }
    }
    // Track type observations
    if (fs.types) {
      if (!merged.types) merged.types = {};
      for (const t in fs.types) {
        merged.types[t] = (merged.types[t] || 0) + fs.types[t];
      }
    }
  }
  return merged;
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
      // Direct match: stats keyed exactly by dotPath (no array indices).
      // Aggregated match: stats keyed by index variants of an array-path
      // (info[0].x, info[1].x, ...) — merge into one virtual bucket.
      const fs = bodyFieldStats[dotPath] ||
        (dotPath.indexOf("[]") >= 0 ? _aggregateStatsForSchemaPath(bodyFieldStats, dotPath) : null);
      if (fs) {
        _applyStatsToField(def, fs, requestCount);
        // Example value + provenance on the field def so the form
        // renderer can prefill without a second pass.
        const ex = pickExampleValue(def, fs);
        if (ex) {
          def._exampleValue = ex.value;
          def._exampleValueSource = ex.source;
          if (ex.confidence != null) def._exampleConfidence = ex.confidence;
        } else {
          delete def._exampleValue;
          delete def._exampleValueSource;
          delete def._exampleConfidence;
        }
      } else {
        const ex = pickExampleValue(def, null);
        if (ex) {
          def._exampleValue = ex.value;
          def._exampleValueSource = ex.source;
        } else {
          delete def._exampleValue;
          delete def._exampleValueSource;
        }
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
      } else if (def.type === "array" && def.items && def.items.properties) {
        // Inline array items (no $ref) — recurse into items.properties
        // directly. Without this, observed paths like `info[0].source`
        // never reach the schema's array-item field declarations and
        // every nested field stays orphaned. Verified on reddit
        // /svc/shreddit/events: 1141 orphan paths under info[0].* .
        walk(def.items, dotPath + "[]", visited);
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
    if (ex) {
      param._exampleValue = ex.value;
      param._exampleValueSource = ex.source;
      if (ex.confidence != null) param._exampleConfidence = ex.confidence;
      else delete param._exampleConfidence;
    } else {
      delete param._exampleValue;
      delete param._exampleValueSource;
      delete param._exampleConfidence;
    }
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

function learnFromResponse(documentId, interfaceName, entry) {
  if (!entry.responseBody) return;

  const tab = _docForLearning(documentId);
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

  /* Magic-byte mimeType fallback. Servers commonly omit/genericize the
     Content-Type on binary responses (application/octet-stream, or no
     header at all); the existing isGrpcWeb/isSSE/isNDJSON dispatch
     below is string-on-mimeType, so an unmarked gRPC-Web frame or
     protobuf body falls through to the generic JSON path and the
     schema/example-value extraction never runs. Per CLAUDE.md #7
     (magic-byte sniff + content-type, never URL-suffix), peek at the
     leading bytes and synthesize a mimeType when the server didn't
     supply one. Real wire-format signatures (gRPC frame, protobuf
     varint tag, gzip/zlib magic) come straight from the spec.  */
  let mimeType = entry.mimeType || "";
  if (!mimeType || /^application\/octet-stream(?:$|;)/i.test(mimeType)) {
    let _sniffBytes = null;
    if (entry.responseBase64) {
      try { _sniffBytes = base64ToUint8(entry.responseBody); }
      catch (e) { console.warn("[brain] mime-sniff base64 decode failed:", e && e.message || e, entry.url); }
    } else if (typeof entry.responseBody === "string") {
      try { _sniffBytes = new TextEncoder().encode(entry.responseBody); }
      catch (e) { console.warn("[brain] mime-sniff encode failed:", e && e.message || e, entry.url); }
    }
    if (_sniffBytes && _sniffBytes.length >= 1) {
      const b0 = _sniffBytes[0];
      const n = _sniffBytes.length;
      // gRPC-Web frame: flag byte (0x00 uncompressed / 0x01 compressed)
      // + 4-byte BE payload length matching the remaining bytes
      if (n >= 5 && (b0 === 0x00 || b0 === 0x01)) {
        const declared = (_sniffBytes[1] << 24) | (_sniffBytes[2] << 16) | (_sniffBytes[3] << 8) | _sniffBytes[4];
        if (declared === n - 5) mimeType = "application/grpc-web+proto";
      }
      // gzip magic 1f 8b — the brain doesn't decompress here, but tagging
      // the mimeType means downstream sees a real content-encoding rather
      // than treating gzip bytes as text and corrupting the schema.
      if (!mimeType && n >= 2 && b0 === 0x1f && _sniffBytes[1] === 0x8b) mimeType = "application/gzip";
      // zlib magic 78 da | 78 9c | 78 01
      if (!mimeType && n >= 2 && b0 === 0x78 && (_sniffBytes[1] === 0xda || _sniffBytes[1] === 0x9c || _sniffBytes[1] === 0x01)) mimeType = "application/zlib";
      // Protobuf varint tag — first byte's low 3 bits ∈ {0,1,2,5}
      // (valid wire types) and field number > 0. Apply only when the
      // body decidedly isn't JSON (first byte not whitespace / { / [ /
      // " / digit / true|false|null start), since JSON is the
      // overwhelming default.
      if (!mimeType && (b0 !== 0x7b && b0 !== 0x5b && b0 !== 0x22 &&
                        !(b0 >= 0x30 && b0 <= 0x39) &&
                        b0 !== 0x74 && b0 !== 0x66 && b0 !== 0x6e &&
                        b0 !== 0x20 && b0 !== 0x09 && b0 !== 0x0a && b0 !== 0x0d)) {
        const wireType = b0 & 0x07;
        const fieldNum = (b0 & 0x78) >> 3;
        if ((wireType === 0 || wireType === 1 || wireType === 2 || wireType === 5) && fieldNum > 0) {
          mimeType = "application/x-protobuf";
        }
      }
    }
  }
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
            path: url.pathname.substring(1).replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}"),
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
            path: url.pathname.substring(1).replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}"),
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
              extractKeysFromText(documentId, val, entry.url, "response_grpc");
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
    } catch (e) {
      /* gRPC-Web frame decode failed — the schema for this endpoint
         won't be learned from THIS response, the next captured response
         from the same URL may decode correctly. Surface so a real
         malformed-frame symptom (server-side bug or wrong protocol
         classification) is visible instead of disappearing into an
         empty schema. */
      console.debug("[brain] grpc-web frame decode failed:", e && e.message || e, "url=" + entry.url);
    }
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
    } catch (e) {
      /* SSE parse failed — malformed event stream (server sent
         data without `data: ` prefix, missing terminator, etc.).
         Surface so the schema-learning skip is observable. */
      console.debug("[brain] SSE parse failed:", e && e.message || e, "url=" + entry.url);
    }
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
    } catch (e) {
      console.debug("[brain] NDJSON parse failed:", e && e.message || e, "url=" + entry.url);
    }
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
                path: url.pathname.substring(1).replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}"),
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
          } catch (e) {
            /* One part's JSON parse failed — rest of the batch still
               processes. Surface so a malformed part on an otherwise-
               valid batch response is visible. */
            console.debug("[brain] multipart part JSON parse failed:", e && e.message || e, "partIdx=" + i, "url=" + entry.url);
          }
        }
      }
    } catch (e) {
      console.debug("[brain] multipart batch parse failed:", e && e.message || e, "url=" + entry.url);
    }
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
            extractKeysFromText(documentId, row.value[1], entry.url, "rsc_error");
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
        } catch (e) {
          /* RSC module-chunk registration failed — likely a malformed
             rsc.modules entry. The response schema still got learned
             above. Surface so a chunk-discovery gap on RSC bundles
             (Next.js app router, etc.) is visible. */
          console.debug("[brain] RSC module-chunk registration failed:", e && e.message || e, "url=" + entry.url);
        }
      }
    } catch (e) {
      /* RSC parse failed — malformed stream or non-RSC content that
         looksLikeRSC false-positived. Surface so the schema-learning
         skip is observable. */
      console.debug("[brain] RSC parse failed:", e && e.message || e, "url=" + entry.url);
    }
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
    } catch (e) {
      console.debug("[brain] GraphQL response parse failed:", e && e.message || e, "url=" + entry.url);
    }
  } else if (mimeType.includes("json") || mimeType.includes("javascript") ||
             /^[\s﻿\x00-\x1f]*[{\[]/.test(textBody)) {
    // JSON or JSONP (callback-wrapped JSON returned as text/javascript) OR
    // body whose first non-whitespace byte is `{` or `[`. Many APIs return
    // JSON under text/plain or no content-type (analytics endpoints,
    // Cloudflare-fronted services, etc.); gating on mimetype alone misses
    // them. Body STRUCTURE is authoritative — if it parses as a JSON
    // object/array, learn the schema; otherwise the catch silently drops.
    try {
      var _lrText = textBody;
      if (!mimeType.includes("json") && (mimeType.includes("javascript") || mimeType === "")) {
        var _lrJsonp = stripJsonp(textBody);
        if (_lrJsonp) _lrText = _lrJsonp;
      }
      // Strip Google XSSI prefix if present. Many Google (and now GitLab
      // snowplow, others) endpoints prepend `)]}'\n` to prevent <script>
      // JSON hijacking. JSON.parse would fail without this.
      if (_lrText.startsWith(")]}'")) _lrText = _lrText.replace(/^\)\]\}'[\r\n]*/, "");
      // Plain "ok" / "OK" confirmations aren't JSON — avoid learning a
      // schema from them.
      if (/^(ok|OK|true|false|null)\s*$/.test(_lrText)) throw new Error("non-object response");
      const json = JSON.parse(_lrText);
      // Only learn from object/array roots — bare strings/numbers/bool
      // don't carry a useful schema and would clutter the doc.
      if (json === null || (typeof json !== "object")) throw new Error("non-object root");
      const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
      targetM.response = { $ref: schemaName };
      const newSchema = generateSchemaFromJson(json, schemaName, doc.schemas);
      mergeSchemaInto(doc, schemaName, newSchema);
    } catch (e) {
      /* JSON/JSONP response parse failed — common when the body is
         truncated, has a JSONP callback we couldn't strip, isn't valid
         JSON, or is just `ok`/`true`/etc. (the explicit throw above).
         Surface so a schema-not-learned symptom traces to the parse
         step rather than disappearing. */
      console.debug("[brain] JSON/JSONP response parse failed:", e && e.message || e, "url=" + entry.url);
    }
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
          extractKeysFromText(documentId, val, entry.url, "response_protobuf");
        }
      });
      const schemaName = `${methodName.replace(/[^a-zA-Z0-9]/g, "")}Response`;
      targetM.response = { $ref: schemaName };
      const newSchema = generateSchemaFromPbTree(tree, schemaName, doc.schemas);
      mergeSchemaInto(doc, schemaName, newSchema);
    } catch (e) {
      /* Protobuf response decode failed — body bytes didn't decode as
         valid wire format (might be a mis-classified text body, a
         compressed payload the brain didn't decompress, or a truncated
         response). Surface so the schema-not-learned symptom is
         traceable. */
      console.debug("[brain] protobuf response decode failed:", e && e.message || e, "url=" + entry.url);
    }
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

function generateSchemaFromPbTree(rootTree, rootName, schemas) {
  // Iterative worklist replaces self-recursion. Each entry is either a
  // "build" (create a fresh schema for tree, attach via slot) or "merge"
  // (build a fresh schema for tree, then fold its properties into an
  // existing schemas[key]). Handles arbitrarily nested protobuf trees
  // without growing the JS call stack.
  let result = null;
  const queue = [{ kind: "build", tree: rootTree, name: rootName, slot: { kind: "result" } }];
  function setSlot(slot, value) {
    if (slot.kind === "result") result = value;
    else if (slot.kind === "schemas") schemas[slot.key] = value;
  }
  function buildShell(tree, name, queueOut) {
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
        // Repeated message field — additional occurrence merges into the
        // existing nested schema. Queue a "merge" job; don't recurse.
        if (node.message) {
          const nestedName = `${name}Field${node.field}`;
          if (schemas[nestedName]) {
            queueOut.push({ kind: "merge", tree: node.message,
              name: nestedName, mergeKey: nestedName });
          }
        }
        continue;
      }
      seen.add(node.field);

      const isRepeated = fieldCounts[node.field] > 1 || !!node.isRepeatedScalar || !!node.packed;
      let wireType;
      if (node.isJspb) {
        const val = node.value;
        if (typeof val === "boolean") wireType = "bool";
        else if (typeof val === "number") wireType = Number.isInteger(val) ? "int64" : "double";
        else if (typeof val === "string") wireType = "string";
        else if (node.isRepeatedScalar && Array.isArray(val) && val.length > 0) {
          const sample = val.find((v) => v != null);
          if (typeof sample === "boolean") wireType = "bool";
          else if (typeof sample === "number") wireType = Number.isInteger(sample) ? "int64" : "double";
          else wireType = "string";
        } else wireType = "string";
      } else if (node.packed) {
        wireType = "int64";
      } else {
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
        // Queue nested build, attaching to schemas[nestedName].
        queueOut.push({ kind: "build", tree: node.message, name: nestedName,
          slot: { kind: "schemas", key: nestedName } });
      } else if (node.string !== undefined) {
        if (!isRepeated) prop.type = "string";
      }
      properties[fieldKey] = prop;
    }
    return { id: name, type: "object", properties };
  }
  while (queue.length > 0) {
    const job = queue.shift();
    const built = buildShell(job.tree, job.name, queue);
    if (job.kind === "build") {
      setSlot(job.slot, built);
    } else if (job.kind === "merge") {
      const existing = schemas[job.mergeKey];
      if (existing) {
        if (!existing.properties) existing.properties = {};
        for (const [k, v] of Object.entries(built.properties || {})) {
          if (!existing.properties[k]) existing.properties[k] = v;
        }
      }
    }
  }
  return result;
}

function generateSchemaFromJson(rootJson, rootName, schemas, rootIsIndexed = false) {
  // Iterative worklist replaces self-recursion. Each entry pairs an input
  // (json, name, isIndexed) with a destination "slot" — where the
  // generated schema gets attached. The slot can be:
  //   - { type: "schemas", key: NAME }   → schemas[NAME] = schemaObj
  //   - { type: "items", parent: SCHEMA } → SCHEMA.items = schemaObj
  //   - { type: "result" }                → set the function's return value
  // This keeps the JS stack at depth 1 even for deeply-nested JSON.
  let result = null;
  const queue = [{ json: rootJson, name: rootName, isIndexed: rootIsIndexed, slot: { kind: "result" } }];
  function setSlot(slot, value) {
    if (slot.kind === "result") result = value;
    else if (slot.kind === "schemas") schemas[slot.key] = value;
    else if (slot.kind === "items") slot.parent.items = value;
    else if (slot.kind === "prop") slot.parent[slot.key] = value;
  }
  while (queue.length > 0) {
    const { json, name, isIndexed, slot } = queue.shift();

    if (Array.isArray(json)) {
      if (isIndexed) {
        const properties = {};
        const obj = { id: name, type: "object", properties };
        setSlot(slot, obj);
        for (let idx = 0; idx < json.length; idx++) {
          const val = json[idx];
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
            const allPrim =
              val.length > 0 &&
              val.every(
                (v) => v === null || v === undefined ||
                  typeof v === "string" || typeof v === "number" || typeof v === "boolean",
              );
            if (allPrim) {
              const itemType = inferRepeatedItemType(val);
              properties[fieldKey] = {
                id: fieldNum, number: fieldNum, type: itemType, label: "repeated",
              };
            } else {
              properties[fieldKey] = { id: fieldNum, number: fieldNum, $ref: nestedName };
              queue.push({ json: val, name: nestedName, isIndexed: true,
                slot: { kind: "schemas", key: nestedName } });
            }
          } else if (typeof val === "object") {
            properties[fieldKey] = { id: fieldNum, number: fieldNum, $ref: nestedName };
            queue.push({ json: val, name: nestedName, isIndexed: false,
              slot: { kind: "schemas", key: nestedName } });
          } else {
            properties[fieldKey] = {
              id: fieldNum, number: fieldNum, type: inferJsonType(val),
            };
          }
        }
        continue;
      }
      // Non-indexed array → { type: "array", items: <schemaForFirstElement> }
      const arr = { type: "array", items: { type: "string" } };
      setSlot(slot, arr);
      if (json.length > 0) {
        queue.push({ json: json[0], name: name + "Item", isIndexed: false,
          slot: { kind: "items", parent: arr } });
      }
      continue;
    }

    if (typeof json === "object" && json !== null) {
      const properties = {};
      const obj = { id: name, type: "object", properties };
      setSlot(slot, obj);
      for (const key in json) {
        const val = json[key];
        const safeKey = key.replace(/[^a-zA-Z0-9]/g, "");
        if (Array.isArray(val)) {
          const arr = { type: "array", items: { type: "string" } };
          properties[key] = arr;
          if (val.length > 0) {
            queue.push({ json: val[0], name: name + safeKey + "Item", isIndexed: false,
              slot: { kind: "items", parent: arr } });
          }
        } else if (typeof val === "object" && val !== null) {
          const nestedName = name + safeKey.charAt(0).toUpperCase() + safeKey.slice(1);
          properties[key] = { $ref: nestedName };
          queue.push({ json: val, name: nestedName, isIndexed: false,
            slot: { kind: "schemas", key: nestedName } });
        } else {
          properties[key] = { type: inferJsonType(val) };
        }
      }
      continue;
    }

    // Primitive
    setSlot(slot, { type: inferJsonType(json) });
  }
  return result;
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
function mergeSchemaInto(doc, rootSchemaName, rootNewSchema) {
  // Iterative: merge doc.schemas[schemaName] ← newSchema, queueing
  // nested ($ref) merges instead of recursing. visited-set on the
  // merge target prevents cycles when a schema references itself or
  // forms a $ref loop. Replaces the previous self-recursive form whose
  // depth was bounded by JS-stack — adversarially-deep nested $refs
  // in a learned schema would crash the merge before this conversion.
  const visited = new Set();
  const queue = [{ schemaName: rootSchemaName, newSchema: rootNewSchema }];
  while (queue.length > 0) {
    const { schemaName, newSchema } = queue.shift();
    if (visited.has(schemaName)) continue;
    visited.add(schemaName);
    if (!doc.schemas[schemaName]) {
      doc.schemas[schemaName] = newSchema;
      continue;
    }
    const existing = doc.schemas[schemaName];
    if (!existing.properties) existing.properties = {};
    if (!existing._drift) existing._drift = [];
    const newProps = newSchema.properties || {};

    const numToKey = {};
    for (const [k, p] of Object.entries(existing.properties)) {
      const n = p.number ?? p.id;
      if (n != null) numToKey[n] = k;
    }

    for (const [key, newProp] of Object.entries(newProps)) {
      const fieldNum = newProp.number ?? newProp.id;
      const matchKey = existing.properties[key] ? key
        : (fieldNum != null && numToKey[fieldNum]) ? numToKey[fieldNum]
        : null;
      const old = matchKey ? existing.properties[matchKey] : null;

      if (!old) {
        existing.properties[key] = newProp;
        if (fieldNum != null) numToKey[fieldNum] = key;
        existing._drift.push({ type: "field_added", field: key, fieldType: newProp.type, timestamp: Date.now() });
      } else {
        if (matchKey !== key && !old.customName && !/^field\d+$/.test(key)) {
          existing.properties[key] = old;
          delete existing.properties[matchKey];
          numToKey[fieldNum] = key;
        }
        if (old.customName) {
          // Keep the user's rename
        } else if (newProp.name && !old.name) {
          old.name = newProp.name;
        }
        if (newProp.type && newProp.type !== old.type) {
          if (old.type === "string" && newProp.type !== "string") {
            existing._drift.push({ type: "type_changed", field: key || matchKey, from: old.type, to: newProp.type, timestamp: Date.now() });
            old.type = newProp.type;
          } else if (
            (old.type === "int64" || old.type === "int32") &&
            (newProp.type === "double" || newProp.type === "float")
          ) {
            existing._drift.push({ type: "type_changed", field: key || matchKey, from: old.type, to: newProp.type, timestamp: Date.now() });
            old.type = newProp.type;
          }
        }
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
        // Queue nested $ref merge instead of recursing.
        if (newProp.$ref && doc.schemas[newProp.$ref]) {
          queue.push({ schemaName: newProp.$ref, newSchema: doc.schemas[newProp.$ref] });
        }
      }
    }
    if (existing._drift.length > 50) existing._drift = existing._drift.slice(-50);
  }
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
async function sendPageFetch(tabId, url, opts, documentId) {
  // documentId-ONLY routing. A credentialed page-context read must hit the EXACT
  // document (its own origin/credentials). NO frameId fallback — a frameId is
  // reused across navigations and could resolve to a DIFFERENT origin; and with
  // no target option tabs.sendMessage would broadcast to every frame in the tab.
  // No documentId → refuse rather than risk a wrong-origin / broadcast read.
  if (!documentId) return { error: "blocked: no documentId for page-context fetch" };
  return swRpc(
    "tabs.sendMessage",
    tabId,
    {
      type: "PAGE_FETCH",
      url,
      method: opts.method || "GET",
      headers: opts.headers || {},
      body: opts.body ?? null,
      bodyEncoding: opts.bodyEncoding || null,
    },
    { documentId },
  );
}

/**
 * Fetch through a content script in the EXACT document (credentialed,
 * session-aware). Routed by documentId ONLY — no frameId fallback, because a
 * frame is reused across navigations and could be a different origin.
 * @param {string} documentId — the target document (stable across the page's life)
 */
async function pageContextFetch(tabId, url, opts, documentId) {
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
  let _pageFetchErr = null;
  if (tabId != null) {
    try {
      return await sendPageFetch(tabId, url, opts, documentId);
    } catch (e) {
      /* Page-context fetch failed (tab closed, content script not injected,
         frame removed, etc.). Capture the underlying reason so the caller
         sees more than "content script unreachable". */
      _pageFetchErr = e && e.message || String(e);
      console.debug("[brain] pageContextFetch sendPageFetch failed:", _pageFetchErr, "tabId=" + tabId + " url=" + url);
    }
  }

  return {
    error: "relay_failed: content script unreachable on tab " + tabId + (_pageFetchErr ? " (" + _pageFetchErr + ")" : ""),
  };
}

/**
 * Create a fetchFn bound to a specific tab.
 */
function makePageFetchFn(tabId, documentId = null) {
  // Bind the DOCUMENT (stable) so page-context discovery/probe reads hit the
  // exact document's own origin/credentials. Routed by documentId only.
  return (url, opts) => pageContextFetch(tabId, url, opts, documentId);
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
  documentId,
  service,
  hostname,
  apiKeys,
  seedUrl,
) {
  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : null; // Chrome routing (makePageFetchFn/notifyPopup) — derived from the doc

  const fetchFn = makePageFetchFn(tabId, documentId);
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
              await performProbeAndPatch(documentId, service, seedUrl, apiKey);
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
    await performProbeAndPatch(documentId, service, finalSeedUrl, probeKey);
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
async function performProbeAndPatch(documentId, service, targetUrl, apiKey) {
  // Deduplicate: skip if already probing this service+url combo
  const probeKey = `${service}::${targetUrl}`;
  if (_inflight.has(probeKey)) return;
  _inflight.add(probeKey);

  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : null; // Chrome routing — derived from the doc

  if (typeof probeApiEndpoint === "undefined") {
    console.error("[Debug] CRITICAL: probeApiEndpoint is not defined!");
    _inflight.delete(probeKey);
    return;
  }

  const fetchFn = makePageFetchFn(tabId, documentId);

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

function convertProbeFieldsToSchema(rootFieldsObj, schemas, rootPrefix = "") {
  // Iterative: build a properties{} map for each fields list. Nested
  // fields (message with children) get a placeholder schemas[name] entry
  // up-front so later refs can attach .children pointers without waiting
  // for recursion to finish; the queue then populates each placeholder's
  // properties in BFS order. Visited-set on nestedName prevents infinite
  // expansion for cyclic schemas.
  const rootProperties = {};
  const visited = new Set();
  const queue = [{ fieldsObj: rootFieldsObj, prefix: rootPrefix, dst: rootProperties }];
  // Track deferred .children attachments — populated once nested
  // properties land in schemas[nestedName].
  const pendingAttach = [];
  while (queue.length > 0) {
    const job = queue.shift();
    const { fieldsObj, prefix, dst } = job;
    const fields = Array.isArray(fieldsObj)
      ? fieldsObj
      : fieldsObj instanceof Map
        ? [...fieldsObj.values()]
        : Object.values(fieldsObj || {});
    for (const field of fields) {
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
          const nestedName = field.messageType ||
            `${prefix}${field.name.charAt(0).toUpperCase() + field.name.slice(1)}Entry`;
          if (!schemas[nestedName] && !visited.has(nestedName)) {
            visited.add(nestedName);
            const nestedProperties = {};
            schemas[nestedName] = { id: nestedName, type: "object", properties: nestedProperties };
            queue.push({ fieldsObj: field.children, prefix: nestedName, dst: nestedProperties });
          }
          prop.items.$ref = nestedName;
          delete prop.items.type;
          pendingAttach.push({ target: prop.items, key: "children", schemaName: nestedName });
        }
      } else if (field.type === "message" && field.children) {
        const nestedName = field.messageType ||
          `${prefix}${field.name.charAt(0).toUpperCase() + field.name.slice(1)}`;
        if (!schemas[nestedName] && !visited.has(nestedName)) {
          visited.add(nestedName);
          const nestedProperties = {};
          schemas[nestedName] = { id: nestedName, type: "object", properties: nestedProperties };
          queue.push({ fieldsObj: field.children, prefix: nestedName, dst: nestedProperties });
        }
        prop.$ref = nestedName;
        delete prop.type;
        pendingAttach.push({ target: prop, key: "children", schemaName: nestedName });
      }
      const fieldKey = field.name || `field_${field.number}`;
      dst[fieldKey] = prop;
    }
  }
  // All nested schemas are now populated; resolve deferred .children refs.
  for (const a of pendingAttach) {
    if (schemas[a.schemaName] && schemas[a.schemaName].properties) {
      a.target[a.key] = schemas[a.schemaName].properties;
    }
  }
  return rootProperties;
}

// ─── req2proto Fallback Probing ──────────────────────────────────────────────

async function probeEndpoint(documentId, endpointKey) {
  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : null; // Chrome routing — derived from the doc
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

  const fetchFn = makePageFetchFn(tabId, documentId);
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

// Host+path of POST URLs already error-probed by discoverServiceInfo (the
// automatic service-info probe below). Module-global so it dedupes across
// documents/tabs; resets on offscreen reload (a re-probe is harmless).
const _svcInfoProbedUrls = new Set();

async function handleResponseBody(tabId, msg, frameId, documentId) {
  if (!msg.url) return;
  await _globalStoreReady;

  // Normalize channel ID from relay messages
  const channelId = msg.channelId || msg.wsId;

  // WebSocket lifecycle: one log entry per connection, messages[] array
  const isWs = msg.method === "WS_OPEN" || msg.method === "WS_CLOSE" ||
    msg.method === "WS_SEND" || msg.method === "WS_RECV";
  if (isWs) {
    if (!_wsConnState.has(documentId)) _wsConnState.set(documentId, new Map());
    const conns = _wsConnState.get(documentId);

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
      _pushGlobalLog(entry, tabId, documentId, frameId);
      conns.set(channelId, { url: msg.url, readyState: 1, entryId: entry.id });
      notifyPopup(tabId);
      return;
    }

    if (msg.method === "WS_CLOSE") {
      const conn = conns.get(channelId);
      if (conn) conn.readyState = 3;
      // Mark the log entry as closed
      const entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "WEBSOCKET" && r.documentId === documentId);
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
      notifyPopup(tabId);
      return;
    }

    // WS_SEND or WS_RECV — append message to existing connection entry
    let entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "WEBSOCKET" && r.documentId === documentId);
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
      _pushGlobalLog(entry, tabId, documentId, frameId);
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
        catch (e) {
          /* Base64-decode or UTF-8 decode of a captured WS message failed
             — likely a binary frame (Protobuf/MessagePack/Flatbuffers)
             rather than text. Surface so key-extraction skip is observable
             and a future diff can route binary WS frames through the same
             protocol classifier the brain runs on HTTP response bodies. */
          console.debug("[brain] WS body decode failed:", e && e.message || e, "url=" + msg.url);
          textBody = null;
        }
      }
      if (textBody) extractKeysFromText(documentId, textBody, msg.url, "response_body");
    }

    notifyPopup(tabId);
    return;
  }

  // postMessage: one log entry per source origin, messages[] array
  // Only PM_RECV — can't wrap window.postMessage (see intercept.js comments)
  if (msg.method === "PM_RECV") {
    let entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "POSTMESSAGE" && r.documentId === documentId);
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
      _pushGlobalLog(entry, tabId, documentId, frameId);
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
      extractKeysFromText(documentId, msg.body, msg.url, "response_body");
    }

    notifyPopup(tabId);
    return;
  }

  // MessageChannel: MC_OPEN creates entry, MC_RECV appends messages
  if (msg.method === "MC_OPEN") {
    let entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "MSGCHANNEL" && r.documentId === documentId);
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
      _pushGlobalLog(entry, tabId, documentId, frameId);
    }
    notifyPopup(tabId);
    return;
  }

  if (msg.method === "MC_RECV") {
    let entry = globalRequestLog.find((r) => r.channelId === channelId && r.method === "MSGCHANNEL" && r.documentId === documentId);
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
      _pushGlobalLog(entry, tabId, documentId, frameId);
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
      extractKeysFromText(documentId, msg.body, msg.url, "response_body");
    }

    notifyPopup(tabId);
    return;
  }

  // ─── SSE: streaming events, no request data ─────────────────────────────
  if (msg.method === "SSE") {
    if (!msg.body) return;
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
    _pushGlobalLog(entry, tabId, documentId, frameId);
    if (msg.body) {
      extractKeysFromText(documentId, msg.body, msg.url, "response_body");
    }
    learnFromResponse(documentId, entry.service, entry);
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

  const tab = _docForLearning(documentId);
  let service = extractInterfaceName(url);

  // Build request header map from intercept.js capture
  const headerMap = msg.requestHeaders || {};

  // Key scanning: URL + request headers. Record the SPECIFIC header name
  // (lowercased) so on replay we can re-emit the key in the same location
  // instead of defaulting to X-Goog-Api-Key on every non-Google host.
  extractKeysFromText(documentId, msg.url, msg.url, "url");
  for (const [k, v] of Object.entries(headerMap)) {
    extractKeysFromText(documentId, `${k}: ${v}`, msg.url, "header:" + k.toLowerCase());
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
    // Bundle call-site stack captured at the network-API hook in intercept.js
    // (`new Error().stack`, wrapper frames stripped). For a [live]-only
    // endpoint (one the forced-execution engine didn't reach), this names the
    // exact bundle function that fired the request — diagnostic provenance
    // for the network-vs-AST diff.
    callStack: msg.callStack || null,
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
      catch (e) {
        /* Base64 / UTF-8 decode failure on a captured response body — most
           often a binary frame (Protobuf / gRPC-Web / image / gzipped) that
           the text-decoder rejects. Surface so the skipped key-extraction
           is observable instead of silent (was `catch (_) { textBody = null }`,
           which dropped the diagnostic). The classifier below still runs on
           the raw bytes via magic-byte sniff. */
        console.debug("[brain] response body text-decode failed:", e && e.message || e, "url=" + msg.url);
        textBody = null;
      }
    }
    if (textBody) extractKeysFromText(documentId, textBody, msg.url, "response_body");
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
          } catch (e) {
            console.debug("[brain] JSPB-in-text body parse failed:", e && e.message || e, "url=" + msg.url);
          }
        } else {
          entry.decodedBody = pbDecodeTree(bytes, 8, (val) => {
            if (typeof val === "string") {
              extractKeysFromText(documentId, val, msg.url, "protobuf_body");
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
        } catch (e) {
          console.debug("[brain] form-urlencoded f.req decode failed:", e && e.message || e, "url=" + msg.url);
        }
      } else {
        // Try JSON parsing for any non-protobuf, non-form-encoded body.
        // Many analytics SDKs (reddit's /svc/shreddit/events, GA, sentry,
        // segment, ...) send JSON bodies with `Content-Type: text/plain`
        // to bypass CORS preflight. Gating on the content-type alone
        // misses every one. Body STRUCTURE is authoritative: if it parses
        // as a JSON object/array, treat as JSON. A failed parse is the
        // expected outcome for text/binary bodies that aren't JSON; we
        // surface the diagnostic so a NEW class of mis-detected body is
        // visible rather than silently absent from field-extraction.
        try {
          const text = new TextDecoder().decode(bytes);
          const trimmed = text.trimStart();
          if (trimmed.startsWith("{") || trimmed.startsWith("[")) {
            const json = JSON.parse(text);
            if (json && typeof json === "object") {
              entry.decodedBody = json;
              entry.isJson = true;
            }
          }
        } catch (e) {
          console.debug("[brain] structural-JSON request-body parse failed:", e && e.message || e, "url=" + msg.url);
        }
      }
    } catch (e) {
      console.debug("[brain] request-body decode outer failed:", e && e.message || e, "url=" + msg.url);
    }
  }

  // Learn from request — skipped only for "boring" fetches. Image APIs
  // (any dynamic signal present) still learn URL + request body + auth so
  // they can be replayed and inspected.
  if (!_isBoringFetch) {
    learnFromRequest(documentId, service, entry, headerMap);

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
    // The requesting frame's AUTHORITATIVE origin, by the response's documentId,
    // from the DURABLE _docOrigins map — never the transient script buffer (gone
    // after review) and never URL-parsed (about:blank parses to a bogus "null").
    var _svcFrameOrigin = _originForDoc(documentId);
    // pageUrls: the tab's last-seen top url (UI display of "used from"). This is a
    // URL for the human, NOT an origin decision.
    if (tab.url) _svcDocEntry.pageUrls.add(tab.url);
    // frameOrigins: the SET of authoritative origins that used this service. We do
    // NOT derive a "top page origin" from the tab url to flag iframe-ness — mapping
    // a tab to an origin assumes the main frame and races navigation (banned). A
    // cross-origin caller stands out in the set on its own.
    if (_svcFrameOrigin) {
      if (!_svcDocEntry.frameOrigins) _svcDocEntry.frameOrigins = new Set();
      _svcDocEntry.frameOrigins.add(_svcFrameOrigin);
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
      performProbeAndPatch(documentId, service, msg.url, apiKey || keysForService[0] || null);
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
    fetchDiscoveryForService(documentId, service, url.hostname, keysForService, msg.url);
  }

  // Error-based service-info probe (req2proto; README "Error-Based Schema
  // Probing"). An intentionally-malformed POST to the endpoint provokes a gapi
  // error envelope, learning its CANONICAL service/method + OAuth scopes, merged
  // into the discovery doc — EVEN when a real doc exists, since the probe can
  // surface a HIDDEN service/method/scope the doc omits. This is the automatic
  // form of the old DISCOVER_SERVICE message (the bug was that it was a message,
  // never sent, instead of a background step). Bounded: once per endpoint, POST
  // endpoints only (the probe's own method, so it introduces no new method).
  if (!_isBoringFetch && msg.method === "POST") {
    // Probe the POST request URL DIRECTLY. handleResponseBody sees EVERY captured
    // POST; the request need NOT be a "learned" endpoint in globalStore.endpoints
    // (Google batchexecute / $rpc calls are logged + classified into a service but
    // are not endpoint-keyed). Cache per host+path (module-global, across docs).
    const _siUrl = new URL(url.href);
    _siUrl.searchParams.delete("key");
    const _siKey = _siUrl.hostname + _siUrl.pathname;
    if (!_svcInfoProbedUrls.has(_siKey)) {
      _svcInfoProbedUrls.add(_siKey);
      const _siHeaders = {};
      if (apiKey) _siHeaders["X-Goog-Api-Key"] = apiKey;
      const _siPath = _siUrl.pathname;
      discoverServiceInfo(_siUrl.toString(), _siHeaders, { fetchFn: makePageFetchFn(tab.tabId, documentId) }).then((result) => {
        if (!result) return;
        let _merged = false;
        const _scopes = Array.isArray(result.scopes) ? result.scopes.filter(Boolean) : [];
        if (_scopes.length) { tab.scopes.set(service, _scopes); _merged = true; }
        const _doc = globalStore.discoveryDocs.get(service)?.doc;
        if (_doc) {
          const _m = findDiscoveryMethod(_doc, _siPath, "POST")?.method;
          if (_m) {
            if (_scopes.length && (!Array.isArray(_m.scopes) || !_m.scopes.length)) { _m.scopes = _scopes; _merged = true; }
            if (result.service && _m._probedService !== result.service) { _m._probedService = result.service; _merged = true; }
            if (result.method && _m._probedMethod !== result.method) { _m._probedMethod = result.method; _merged = true; }
          }
        }
        if (_merged) { tab.probeResults.set(`svcinfo:POST ${_siPath}`, result); mergeToGlobal(tab); notifyPopup(tab.tabId); }
      }).catch((e) => { if (typeof console !== "undefined") console.debug("[brain] @WHY {phase:'svcinfo-probe',reason:'" + (e && e.message || e) + "'}"); });
    }
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
  _pushGlobalLog(entry, tabId, documentId, frameId);

  // Learn from response — skip for static assets.
  if (entry.responseBody && !_isAsset) {
    learnFromResponse(documentId, service, entry);
  }

  mergeToGlobal(tab);
  notifyPopup(tabId);
}

// ─── Cross-Script AST Buffering ──────────────────────────────────────────────

// _bufferScript / _scriptBufferDecrementPending / _fetchAndBufferScript removed:
// the per-script SCRIPT_SOURCE buffering machinery is gone. content.js ships one
// CONTENT_HTML per document; the engine (Lexbor + qjs_run_doc_scripts) sources the
// document's scripts. The per-document buffer now holds only {tabId, origin, url,
// pageHtml, chunk-state}; the offscreen no longer fetches/combines scripts.

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
  // EMPTY scriptOffsets (the engine sources inline scripts now, so a page with
  // only inline <script> has no per-script offset map) must NOT return undefined
  // — callers do sInfo.url and would throw, taking down the WHOLE findings/endpoint
  // result (every DOM-XSS + inline-script endpoint silently lost). A null url is
  // the correct inline attribution: page URL, original line numbers.
  return scriptOffsets[0] || { url: null, lineStart: 1 };
}

// Resolve an AST endpoint's path-param names (minified, in URL order) to the
// page's REAL declared names via its chunk's source map. No minified pattern-
// matching and no value guessing: it maps the fetch call-site position through
// the map to the original position, reads that line from the map's own
// `sourcesContent`, and takes the URL template's path interpolation identifiers
// in order — e.g. `await fetch(\`/${owner}/${repo}/…?source=${source}\`)` →
// ["owner","repo"]. Returns the names array, or null if anything's missing.
function _resolvePathParamNames(callSite, scriptOffsets, traceMapsByUrl) {
  // Resolve a SHOWN finding's minified path params (e/a) to their declared names
  // (owner/repo) by running the source-map LIBRARY (@jridgewell/trace-mapping)
  // on the finding's own call-site location — the position the engine already
  // emits via its normal stack trace (NO engine instrumentation, NO bundle
  // transform). originalPositionFor() + sourceContentFor() hand back the
  // ORIGINAL fetch line; we read its template literal's path interpolations.
  try {
    if (!callSite || !callSite.loc || !scriptOffsets || !scriptOffsets.length || !traceMapsByUrl) return null;
    var sc = _findScriptForLine(callSite.loc.line, scriptOffsets);
    if (!sc || !sc.url) return null;
    var tm = traceMapsByUrl[sc.url];
    if (!tm) return null;
    var genLine = callSite.loc.line - sc.lineStart + 1;            // 1-based line within the chunk
    var col0 = (callSite.loc.column != null ? callSite.loc.column : (callSite.loc.col || 1)) - 1;
    if (col0 < 0) col0 = 0;
    var op = traceMapping.originalPositionFor(tm, { line: genLine, column: col0 });
    if (!op || op.source == null || op.line == null) return null;
    var content = traceMapping.sourceContentFor(tm, op.source);
    if (!content) return null;
    var lines = content.split("\n");
    var BT = String.fromCharCode(96);   // backtick
    // The fetch's original line(s) hold the URL template literal; scan a small
    // window (beautified calls can wrap) for the first backtick template, then
    // read its PATH interpolations (before '?'), last identifier of each ${...}.
    var win = (lines[op.line - 1] || "") + " " + (lines[op.line] || "") + " " + (lines[op.line - 2] || "");
    var bt = win.indexOf(BT);
    if (bt < 0) return null;
    var bt2 = win.indexOf(BT, bt + 1);
    var tmpl = bt2 > bt ? win.slice(bt + 1, bt2) : win.slice(bt + 1);
    var qm = tmpl.indexOf("?");
    var pathPart = qm >= 0 ? tmpl.slice(0, qm) : tmpl;
    var names = [], re = /\$\{\s*(?:[A-Za-z_$][\w$]*\s*\.\s*)*([A-Za-z_$][\w$]*)\s*\}/g, mm;
    while ((mm = re.exec(pathPart))) names.push(mm[1]);
    return names.length ? names : null;
  } catch (e) { return null; }
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
  var meta = tab;   // tab-level url folded onto the per-tab state
  if (meta && meta.url) tabUrl = meta.url;
  else if (buf && buf.pageUrl) tabUrl = buf.pageUrl;

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
  // Lazy chunks are loaded by the ONE scheduler IN PLACE: the engine emits @CHUNK during forced
  // execution, runEngine fetches it (self.safeFetch) and qjs_provide evals it in the live instance,
  // surfacing its endpoints in the SAME run. No host-side re-fetch/re-analyze round (that was a second
  // scheduler — deleted). A chunk form the engine doesn't yet discover in-place is an engine gap to close.
}

// Deterministic in-flight signal for the diagnostic / e2e harness:
// _analyzeCombinedScripts sets this for the tab on entry, clears on
// exit (success OR error). Lets a test poll "wait until !running"
// instead of guessing a wall-clock budget — the wait scales with
// real worker execution.
const _analysisInflight = new Set();

// Review queue. New pages (and their JS) are QUEUED, then a single drainer
// reviews ONE page at a time. Combined with the worker throttling itself
// (it yields the core between every schedule/deep batch), this means many
// open tabs never stack analyses onto the CPU — the reviewer runs cool in
// the background and never pins a core. Time is free; a maxed core is not.
var _reviewQueue = [];
var _reviewDraining = false;
function _analyzeCombinedScripts(docKey) {
  var buf = _scriptBuffers.get(docKey);
  if (!buf || !buf.pageHtml) return;                              // engine-sourced: gate on the document HTML, not pre-buffered scripts
  if (_reviewQueue.indexOf(docKey) < 0) _reviewQueue.push(docKey);   // dedupe within queue; a re-queue after run re-reviews (combined-cache makes an unchanged doc a fast hit)
  _drainReviewQueue();
}
async function _drainReviewQueue() {
  if (_reviewDraining) return;
  _reviewDraining = true;
  try {
    while (_reviewQueue.length) {
      /* Recency-priority pick instead of FIFO shift — when the user tabs to
         a different page while an older queued tab is still waiting, the
         tab they're LOOKING AT gets analyzed next. The comparator lives in
         lib/priority.js (ORDER only, never COVERAGE — every queued tab still
         gets analyzed eventually). buf.lastActivatedTs is stamped when a document
         delivers its CONTENT_HTML (the load IS the user-attention signal);
         documents without a recorded timestamp default to 0 and trail the
         recently-loaded cohort. */
      var docKey = self._priorityCmp.pickFromReviewQueue(_reviewQueue, function (k) {
        var b = _scriptBuffers.get(k);
        return (b && b.lastActivatedTs) || 0;
      });
      if (docKey == null) break;
      var buf = _scriptBuffers.get(docKey);
      if (!buf || !buf.pageHtml) continue;
      var tabId = buf.tabId;
      /* Same-tab guard: a re-queue from a late-arriving script for a tab
         whose analysis is STILL IN FLIGHT (round-1 BFS or chunk-merged
         round-2 still grinding) must not spawn a CONCURRENT second call —
         two wasm instances on the same 4.4MB+ bundle would compete for
         memory (wasm `memory.grow` is monotonic per-instance) and one
         would trap "memory access out of bounds" mid-eval. The previous
         absence of this guard produced 7 concurrent round-2 retries on
         github, each duplicating the 17MB compiled-bytecode footprint.
         Different tabId CAN still run concurrently — JSPI scheduler
         interleaves them at yield points. The re-queue isn't dropped;
         the late scripts are folded into the current buf and the next
         drain iteration after the in-flight one finishes will pick them
         up via the cache-miss path. */
      if (_analysisInflight.has(docKey)) continue;   // per-DOCUMENT in-flight guard (distinct documents of a tab may analyse concurrently, bounded by the grind cap)
      _analysisInflight.add(docKey);
      /* Fire-and-forget — do NOT await. With the worker's JSPI scheduler
         (ast-thread.js _yieldDrain + _flowCmp), each page's analysis runs
         in its own wasm instance and interleaves with others at JSPI yield
         points by lexicographic priority (active-page focus, reaches-host-
         edge, recent emissions, visit recency, anti-starvation). Awaiting
         here would serialize and reduce the scheduler to a trivial pick-
         the-only-fiber loop — the same behavior we'd get without JSPI.
         The brain's per-tab cache + _dataEpoch guard already keep results
         attribution-correct under concurrency; errors surface via
         analysis.resolverErrors (worker-side) or _astError (this side),
         not lost via a bare catch. */
      _analyzeCombinedScriptsInner(tabId, buf)
        .catch(function (e) { console.debug("[AST:queue] doc review error: %s", e && e.message); })
        .finally(function () { _analysisInflight.delete(docKey); });
    }
  } finally { _reviewDraining = false; }
}
async function _analyzeCombinedScriptsInner(tabId, buf) {
  var tab = getDoc(buf.docKey);
  var _ep = _dataEpoch;   // a Clear during the worker round-trip invalidates this run
  // Concatenate in DOM/execution order, not fetch-arrival order — a
  // later chunk that reads state an earlier chunk set up (GitHub's app
  // chunk reading client-env loaded by environment-*.js) throws
  // "requested before it was loaded" if combined out of order. Stable
  // in-place sort so every downstream reader (scriptOffsets, the
  // fallback combine paths, _findScriptForLine) sees the same order.
  buf.scripts.sort(function (a, b) { return (a.order == null ? 1e9 : a.order) - (b.order == null ? 1e9 : b.order); });
  // Split executable scripts from server-rendered data islands. Islands
  // are NOT concatenated into the executable bundle (they're JSON, not
  // code) — they're rebuilt into the worker's virtual DOM so the bundle
  // bootstraps from them (GitHub #client-env) and runs correctly.
  // Lexbor inside the engine worker parses CONTENT_HTML and produces
  // the real spec DOM the bundle reads — including the page's data
  // islands (inline <script type="application/json">). No need to
  // ship them as a separate domIslands array.
  var scripts = buf.scripts;
  // DIAGNOSTIC: record each analysis round's script set so a dropped external
  // script (the cross-origin CDN case) is visible — which round ran, how many
  // scripts, and whether the CDN bundle was present.
  if (!self._analyzeDiag) self._analyzeDiag = [];
  self._analyzeDiag.push({ n: scripts.length, pending: buf.pending, loadFired: !!buf.loadFired,
    hasCDN: scripts.some(function (s) { return /sentry-cdn|browser\.sentry/.test(s.url || ""); }),
    urls: scripts.map(function (s) { return (s.url || "inline").split("/").pop().slice(0, 24); }).slice(0, 8) });
  if (self._analyzeDiag.length > 12) self._analyzeDiag.shift();
  // Real <script src> URLs (in execution order) — built into the
  // virtual DOM so webpack's auto-publicPath (document.currentScript
  // .src / getElementsByTagName("script")) finds a real script URL
  // instead of throwing "Automatic publicPath is not supported". Just
  // URLs (tiny); bodies are already in the combined code.
  var scriptUrls = [];
  for (var _sui = 0; _sui < scripts.length; _sui++) if (scripts[_sui].url) scriptUrls.push(scripts[_sui].url);
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
  // If the exact same set of scripts was analyzed before AND the
  // analyzer fingerprint (the SHA of the analyzer worker source) is
  // unchanged, replay the cached result without touching the offscreen
  // worker. Analyzer fingerprint is baked into cacheKey, so a stale
  // entry simply does not match — no manual version bumps needed.
  var scriptHashes = [];
  try {
    // The combination submitted IS the cache identity: hash the pageHtml (round 1's
    // scripts are engine-sourced, so buf.scripts is empty) + each downloaded chunk
    // (round 2). Content/combination keying — NOT url: urls change and pages reuse
    // scripts (jquery), so the same combination at a new url must HIT (dedup) and
    // distinct content at the same url (about:blank) must MISS (no leak). The
    // principal ORIGIN (buf.origin, MessageSender-derived, NEVER url) disambiguates
    // identical content under different principals (their credentialed reads +
    // relative resolution differ); opaque principals are unique so they never share.
    scriptHashes.push("doc:" + (buf.origin || "") + ":" + (await _hashScriptSHA256(buf.pageHtml || "")));
    for (var hi = 0; hi < scripts.length; hi++) {
      scriptHashes.push(await _hashScriptSHA256(scripts[hi].code));
    }
  } catch (_) {
    // SubtleCrypto unavailable — proceed without cache
    scriptHashes = [];
  }

  var cacheKey = null;
  var analyzerFp = await getAnalyzerFingerprint();
  // scriptHashes = [the leading "doc:" pageHtml-identity entry] + one per chunk,
  // so a successful hash run is exactly scripts.length + 1 (SubtleCrypto failure
  // resets it to [], which won't match — no cache, fail safe). The "+ 1" is the
  // post-migration fix: round-1 scripts are engine-sourced (buf.scripts empty), so
  // the document identity lives in that leading pageHtml entry, not a per-script one.
  if (analyzerFp && scriptHashes.length === scripts.length + 1) {
    // Cache key = (analyzer fingerprint) + (script content hashes). Any change to the analyzer
    // worker files OR the analyzed scripts flips the key, so stale entries simply don't match.
    // There is ONE analysis pass now (the engine drives BFS + orphan-residue + in-place chunks in
    // the ONE scheduler); no round-mode suffix.
    cacheKey = analyzerFp + "|" + scriptHashes.join("+");
    var cached = globalStore.scriptCache.get(cacheKey);
    if (cached) {
      console.debug("[AST:cache] Cache HIT for tab=%d (%d scripts, key=%s…)",
        tabId, scripts.length, cacheKey.slice(0, 16));
      cached.timestamp = Date.now();      // LRU touch
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

  // Source URL for the combined analysis. SECURITY: this becomes the analysis
  // PRINCIPAL — safeFetch's origin-relative SSRF origin (self.__sfPageOrigin in
  // the worker) AND window.location. It MUST derive only from the browser-
  // provided sender.url (captured into buf.url on CONTENT_HTML), NEVER a
  // content-script-supplied value (msg.url ->
  // scripts[0].url) — else a hostile page could claim a localhost origin to
  // defeat the SSRF guard. No untrusted fallback: unknown origin leaves tabUrl ""
  // -> safeFetch's safe default (block private) + a placeholder window.location.
  // Per-DOCUMENT principal: buf.url is THIS document's own browser-provided url
  // (set from sender on CONTENT_HTML), not the tab's — a sub-frame analyses as its
  // own origin, never the embedder's.
  var tabUrl = buf.url || buf.pageUrl || "";

  // Analyze combined in offscreen document (non-blocking)
  var analysis;
  var response;
  try {
    response = await sendToOffscreen({
      type: "AST_ANALYZE", code: combined, sourceUrl: tabUrl, documentId: buf.docKey, origin: buf.origin, forceScript: true,
      scriptUrls: scriptUrls,
      // Per-chunk line offsets + each chunk's sourceMappingURL so the OFFSCREEN
      // worker (long-lived, owns IndexedDB) can fetch maps and resolve minified
      // path-param names (e→owner) itself — the SW is evicted mid-grind and must
      // not fetch maps. sourceMapScripts = [{scriptUrl, smUrl}] (smUrl is the
      // bundle's real pragma: relative filename OR full address).
      scriptOffsets: scriptOffsets,
      sourceMapScripts: sourceMapScripts,
      pageHtml: getDoc(buf.docKey)._pageHtml || null,
      // Host CPU slice (resource-pressure proxy): a page with more than this many forced STARTERS parks
      // its residue to the GLOBAL frontier (resumed later by value) instead of running to completion and
      // blocking the offscreen -> BFS ACROSS sites. Small pages (< quantum) finish in one visit unchanged.
      quantum: FRONTIER_QUANTUM,
    });
  } catch (e) {
    console.debug("[AST:combined] sendToOffscreen failed for tab=%d: %s", tabId, e.message || e);
    getDoc(buf.docKey)._astError = "sendToOffscreen threw: " + (e.message || String(e));
    return;
  }
  if (!response || !response.success) {
    // The Clear button terminated the worker mid-analysis. Abort cleanly — do
    // NOT fall back to per-script re-analysis, which would re-flood the freshly
    // respawned worker right after a Clear and repopulate the just-wiped store.
    if (response && response.error === "cleared") {
      console.debug("[AST:combined] tab=%d aborted — worker cleared", tabId);
      return;
    }
    console.debug("[AST:combined] analyzeJSBundle failed for tab=%d: %s", tabId,
      response ? response.error : "no response");
    if (response && response.stack) console.debug(response.stack);
    getDoc(buf.docKey)._astError = "offscreen unsuccessful: " + (response ? (response.error + " | " + (response.stack || "")) : "no response");
    // SURFACE the combined-analysis failure — do NOT fall back to per-script
    // analysis. A page is reviewed as the COMBINATION of all its scripts; analysing
    // them in isolation loses the cross-script interprocedural visibility (webpack
    // chunk exports, shared globals) the whole design depends on, and emitting that
    // degraded result would MASK the real failure. _astError (above) + the worker's
    // @WHY/@E are the signal to root-cause; the resumable frontier retries via replay recipes.
    return;
  }
  getDoc(buf.docKey)._astError = null;
  // The bin/Clear reset fired while this analysis was in the worker. Its result
  // predates the wipe, so merging it (or downloading its chunks / spawning the
  // deep round) would repopulate the just-cleared store. Abandon the whole tail.
  if (_ep !== _dataEpoch) {
    console.debug("[AST:combined] tab=%d result discarded — store reset mid-analysis", tabId);
    return;
  }
  analysis = response.result;
  // Carry the combined→per-script line map onto the analysis so the VDD merge
  // can resolve a path param's minified name to its real source-map name.
  analysis.scriptOffsets = scriptOffsets;

  // NOTE: lazy-chunk consumption (download the chunk URLs the forced run
  // recorded — analysis.chunkUrls — and re-review the combined whole) is
  // intentionally NOT run inline here: it must not BLOCK the initial
  // endpoint population, and re-analyzing the 8 MB+ combined set is only
  // affordable once the worker reuses its instance across schedules. Today
  // each github schedule aborts at JS_FreeRuntime (the teardown GC leak) so
  // ast-thread recycles (re-instantiates the 36 MB instance) every
  // schedule, which makes even one combined re-analysis prohibitively slow
  // live. The discovery half stands (hostedge records chunk URLs →
  // analysis.chunkUrls); wiring the download+combined-review back in is
  // gated on the persistent-runtime / fresh-context-per-schedule fix that
  // removes the per-schedule re-instantiation.

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
  // Cache key already encodes the analyzer fingerprint + script hashes;
  // no separate version field — a stale fingerprint just won't match.
  //
  // BUT: don't cache a DEGENERATE result — a run that produced zero learned
  // facts AND surfaced a resolverError is a host-model gap (e.g. the wasm
  // aborted mid-bundle, JSPI suspend failed, GC residue assert tripped).
  // Caching it under the script-hash key would block re-analysis even after
  // the engine bug is fixed: the next navigation hashes the same scripts,
  // hits the cache, replays the empty result. The analyzer-fingerprint
  // covers the JS worker source but NOT the embedded wasm — a wasm rebuild
  // does not bump it, so the cache stays wedged until the user explicitly
  // hits the bin/Clear button. Skipping the cache write on a degenerate
  // result means a fresh navigation actually re-runs against the fixed
  // engine. A run with at least one fact or no resolverError is preserved
  // (the structural-learning rule — a real "no endpoints on this page" is
  // legitimate; a resolverError-bearing zero is not).
  var _hasFacts = ((analysis.fetchCallSites && analysis.fetchCallSites.length) ||
                   (analysis.securitySinks && analysis.securitySinks.length) ||
                   (analysis.protoEnums && analysis.protoEnums.length) ||
                   (analysis.protoFieldMaps && analysis.protoFieldMaps.length) ||
                   (analysis.domEndpoints && analysis.domEndpoints.length) ||
                   (analysis.chunkUrls && analysis.chunkUrls.length));
  var _hasResolverErr = analysis.resolverErrors && analysis.resolverErrors.length > 0;
  if (cacheKey && !(_hasResolverErr && !_hasFacts)) {
    globalStore.scriptCache.set(cacheKey, {
      result: JSON.parse(JSON.stringify(analysis)), // deep copy to avoid aliasing
      scriptOffsets: scriptOffsets,
      tabUrl: tabUrl,
      timestamp: Date.now(),
    });
    scheduleSave();
  } else if (cacheKey) {
    console.debug("[AST:cache] SKIPPING write for tab=%d (degenerate result: %d resolverErrors, no learned facts) — next navigation will retry", tabId, analysis.resolverErrors.length);
  }

  if (analysis.resolverErrors && analysis.resolverErrors.length > 0) {
    // Surface to the popup diagnostic view, not console-only. A reached-but-
    // opaque host call (fully-opaque URL/method) or a host-model gap (@E
    // bundle throw) is a P1 the reviewer must SEE and act on — per CLAUDE.md
    // "@WHY/diagnostics SHOULD be exposed in the popup's diagnostic view".
    // Deduped by message (the distinct-message set is the natural bound — no
    // cap); diagnostic buffer, not analysis state, so it drops nothing learned.
    if (!Array.isArray(tab._resolverErrors)) tab._resolverErrors = [];
    var _seenRe = new Set(tab._resolverErrors.map(function (r) { return r.message; }));
    // Reply-example seed (CLAUDE.md opaque-with-example policy): a fromReply
    // chain — fetch(reply.field) — names its SOURCE endpoint + field. Fire ONE
    // bounded safe GET per source (cached → no spam + deterministic; GET-only →
    // no account change; origin-scoped under safeFetch's OWN SOP/CORS with the
    // page principal) and extract the field — the real value the bundle CANNOT
    // compute (server input). The endpoint + field stay forced-exec-learned;
    // this only fills the EXAMPLE, a marked sample, never a branch decider.
    var _rcPrincipal = buf.url || buf.pageUrl || "";
    if (!tab._replyCache) tab._replyCache = {};
    for (var _rei = 0; _rei < analysis.resolverErrors.length; _rei++) {
      var _re = analysis.resolverErrors[_rei];
      console.debug("[AST:resolver] %s: %s", _re.context, _re.message);
      if (_re.stack) console.debug(_re.stack);
      if (_rcPrincipal && _re.fromReply && _re.opaqueSources && _re.opaqueFields && _re.opaqueFields.length) {
        var _src = null;
        for (var _si = 0; _si < _re.opaqueSources.length; _si++) {
          var _os = _re.opaqueSources[_si];
          if (typeof _os === "string" && _os.indexOf("fetch.") === 0 && _os.indexOf(" ") > 0) { _src = _os; break; }
        }
        var _srcEp = _src ? _src.slice(_src.indexOf(" ") + 1) : null;
        var _absSrc = null;
        // Resolve against the page principal — safeFetch wants an absolute URL +
        // checks its origin vs the principal (origin-relative SSRF: same-origin
        // ok, public->private blocked). A bundle's source URL is relative.
        if (_srcEp && _srcEp.indexOf("{") < 0) { try { _absSrc = new URL(_srcEp, _rcPrincipal).href; } catch (e) {} }
        if (_absSrc) {
          var _reply = tab._replyCache[_absSrc];
          if (_reply === undefined) {
            try {
              // Same-origin → credentialed (the user's session = the authed API
              // surface). Cross-origin → PUBLIC non-credentialed: a public
              // discovery/config doc (oidc .well-known) serves ACAO:* which
              // safeFetch's credentialed CORS would block, and creds must NEVER
              // leak cross-origin. Per the CLAUDE.md origin-scoped policy.
              // Principal ORIGIN = the requesting document's browser origin (per-frame,
              // documentId-keyed, opaque-unique) tracked on the buffer — NOT parsed from a
              // URL (a sandboxed frame's URL parses to a real origin it does NOT have).
              // Opaque / mixed / untracked ("null:<uuid>" or "null") matches no real
              // target, so the credentialed read never fires; pageOrigin also flows to
              // safeFetch's own SOP (defense in depth).
              var _rcOrigin = buf.origin || "";   // THIS document's authoritative origin (set from sender on CONTENT_HTML); "" -> fail closed
              var _sameOrig = false; try { var _to = new URL(_absSrc).origin; _sameOrig = _rcOrigin.indexOf("://") > 0 && _to === _rcOrigin; } catch (e) {}
              var _resp = await safeFetch(_absSrc, { pageUrl: _rcPrincipal, pageOrigin: _rcOrigin, method: "GET", credentialed: _sameOrig });
              _reply = (_resp && _resp.ok && typeof _resp.body === "string") ? _resp.body : null;
            } catch (e) { _reply = null; }
            tab._replyCache[_absSrc] = _reply;
          }
          if (_reply) {
            var _json = null; try { _json = JSON.parse(_reply); } catch (e) {}
            if (_json && typeof _json === "object") {
              var _ex = {};
              for (var _fi = 0; _fi < _re.opaqueFields.length; _fi++) {
                var _fld = _re.opaqueFields[_fi];
                if (typeof _json[_fld] === "string") _ex[_fld] = _json[_fld];
              }
              if (Object.keys(_ex).length) {
                _re.replyExample = { source: _srcEp, fields: _ex };
                // Surface the chained endpoint in the moat: a FULLY-opaque URL
                // that IS a reply field resolves to the field's real value (a
                // server-provided URL — marked reply-example, never computed).
                // forced exec already learned the chained fetch fired; the GET
                // only supplies its URL. (Templated chains stay {hole} endpoints,
                // not resolverErrors, so this only fires for whole-URL chains.)
                var _cm = /"method":"(\w+)"/.exec(_re.message || "");
                var _chainMethod = _cm ? _cm[1] : "GET";
                // Reconstruct the chained URL: substitute the recovered field
                // values INTO the URL template (rawUrl, e.g. "{apiHost}/api/widgets")
                // so the literal PATH survives the opaque host; a fully-opaque URL
                // (the whole URL IS one field) uses each field value directly.
                var _tmpl = (typeof _re.rawUrl === "string" && _re.rawUrl.indexOf("{") >= 0) ? _re.rawUrl : null;
                var _chainUrls = [];
                if (_tmpl) {
                  var _su = _tmpl;
                  for (var _f in _ex) _su = _su.split("{" + _f + "}").join(_ex[_f]);
                  if (_su.indexOf("{") < 0) _chainUrls.push(_su);
                } else {
                  for (var _f2 in _ex) _chainUrls.push(_ex[_f2]);
                }
                for (var _ci = 0; _ci < _chainUrls.length; _ci++) {
                  var _eu = null; try { _eu = new URL(_chainUrls[_ci], _rcPrincipal); } catch (e) {}
                  if (!_eu || (_eu.protocol !== "http:" && _eu.protocol !== "https:")) continue;
                  var _epk = "replyex " + _chainMethod + " " + _eu.host + _eu.pathname;
                  if (!tab.endpoints.has(_epk)) {
                    tab.endpoints.set(_epk, {
                      url: _eu.href, method: _chainMethod, host: _eu.host, path: _eu.pathname,
                      service: extractInterfaceName(_eu), source: "reply_example",
                      valueSource: "reply-example", replyFrom: { endpoint: _srcEp, field: Object.keys(_ex).join(",") },
                      pageUrl: _rcPrincipal, firstSeen: Date.now(),
                    });
                  }
                }
              }
            }
          }
        }
      }
      if (!_seenRe.has(_re.message)) {
        _seenRe.add(_re.message);
        tab._resolverErrors.push({ context: _re.context, message: _re.message, snippet: _re.snippet || null, replyExample: _re.replyExample || null });
      }
    }
  }

  var hasFindings = analysis.protoEnums.length || analysis.protoFieldMaps.length ||
    analysis.fetchCallSites.length || analysis.sourceMapUrl ||
    (analysis.securitySinks && analysis.securitySinks.length) ||
    (analysis.dangerousPatterns && analysis.dangerousPatterns.length);
  if (!hasFindings && sourceMapScripts.length === 0) {
    console.debug("[AST:combined] No findings for tab=%d", tabId);
    // The deep orphan-residue drive already ran IN the ONE scheduler (seed_orphans is continuous,
    // chunks eval'd in place) — nothing more to dispatch here.
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

  // Idle burst of the ONE host-level attention: advance other origins' parked frontiers by value
  // (non-blocking, serialized). This page's own residue (if it parked) is now in the global frontier.
  _driveGlobalFrontierBurst(4);

  // Fetch source maps — SCOPED to the chunks that actually hold a learned
  // fetch call site (so path-param names like e/a → owner/repo resolve),
  // not all ~661 shipped maps. The deep grind's endpoints (e.g. preheat) live
  // in lazy chunks, so this must consider the combined-bundle loc of every
  // fetchCallSite, mapped back to its chunk via scriptOffsets.
  var _needSM = new Set();
  for (var _fi = 0; _fi < analysis.fetchCallSites.length; _fi++) {
    var _fcl = analysis.fetchCallSites[_fi];
    if (_fcl && _fcl.loc && typeof _fcl.loc.line === "number") {
      var _fsc = _findScriptForLine(_fcl.loc.line, scriptOffsets);
      if (_fsc && _fsc.url) _needSM.add(_fsc.url);
    }
  }
  for (var smi = 0; smi < sourceMapScripts.length; smi++) {
    if (_needSM.size && !_needSM.has(sourceMapScripts[smi].scriptUrl)) continue;
    _fetchSourceMapForScript(tabId, tab, analysis, sourceMapScripts[smi].scriptUrl, sourceMapScripts[smi].smUrl);
  }

  // Lazy chunks were already fetched + eval'd IN PLACE by the ONE scheduler during this run
  // (@CHUNK → runEngine safeFetch → qjs_provide), so their endpoints are in `analysis` already.
  // No host-side re-fetch/re-analyze round.
}

function _fetchSourceMapForScript(tabId, tab, analysis, scriptUrl, smUrl) {
  var _ep = _dataEpoch;   // a Clear during the (async) source-map fetch invalidates this re-merge
  try {
    if (!/^https?:\/\//i.test(smUrl)) {
      smUrl = new URL(smUrl, new URL(scriptUrl)).href;
    }
  } catch (_) {
    console.debug("[AST:sourcemap] Failed to resolve URL: %s (base: %s)", smUrl, scriptUrl);
    return;
  }
  console.debug("[AST:sourcemap] Fetching: %s (from %s)", smUrl, scriptUrl);
  pageContextFetch(tabId, smUrl, { method: "GET" }, tab && tab.documentId)
    .then(async function(smResp) {
      if (!smResp.body || smResp.error) {
        console.debug("[AST:sourcemap] Fetch failed for %s: %s", smUrl, smResp.error || "empty body");
        return;
      }
      try {
        var smJson = JSON.parse(smResp.body);
        // Name resolution (e→owner) uses the standard library on the
        // engine-stamped hole position; stored per chunk URL (each lazy chunk
        // has its own map). The AST_PARSE_SOURCEMAP call below stays only for
        // proto-file/type extraction from the map's sources/sourcesContent.
        try { analysis.traceMapsByUrl = analysis.traceMapsByUrl || {}; analysis.traceMapsByUrl[scriptUrl] = new traceMapping.TraceMap(smJson); }
        catch (e) { console.debug("[AST:sourcemap] TraceMap failed for %s: %s", scriptUrl, e && e.message); }
        var smResp2 = await sendToOffscreen({ type: "AST_PARSE_SOURCEMAP", sourceMapJson: smJson });
        if (!smResp2 || !smResp2.success) {
          console.debug("[AST:sourcemap] parseSourceMap failed for %s: %s", smUrl, smResp2 ? smResp2.error : "no response");
          return;
        }
        var smData = smResp2.result;
        analysis.sourceMap = smData;
        // Per-script map store: a page loads many chunks, each with its OWN
        // map; `analysis.sourceMap` keeps only the last fetched, so param-name
        // resolution must look up the map for the SPECIFIC chunk an endpoint's
        // call site lives in (via scriptOffsets), not the last one.
        analysis.sourceMapsByUrl = analysis.sourceMapsByUrl || {};
        analysis.sourceMapsByUrl[scriptUrl] = smData;
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
        if (_ep !== _dataEpoch) return;   // store was reset while this map was fetching — don't repopulate
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

function mergeASTResultsIntoVDD(tab, results, tabId, isPartial) {
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
      // Note: bundle-wide value constraints (analysis.valueConstraints) are
      // NOT merged into params by name. That was a heuristic — any switch/
      // case on a variable named `q` anywhere in the bundle would attach
      // its values to every method's `q` param, including unrelated
      // form-scan-derived ones. Real-world FP: stackoverflow's `/search` q
      // received `["&", "read", "write", 0]` from an unrelated module.
      // Per-call-site values flow through learnFromAstCallSite →
      // _mergeAstValues from `callSite.params[i].validValues`, which is
      // structurally tied to the specific fetch site.
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
        // Note: proto-field-map enrichment from TypeScript .pb.ts interfaces
        // was removed. The previous heuristics — fuzzy field-count tolerance
        // (Math.abs(diff) <= 2) and source-filename pattern matching
        // (/\.pb\.|_pb\.|proto/i) — both violated CLAUDE.md (magic-number
        // cap + framework-specific naming). Proto field maps work by field
        // ID without TS-name enrichment; if field-name learning is needed
        // it must come from a structural signal (e.g. the .proto definition
        // file via sourcemap, or AST extraction of the message class).
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
        // Structural @T candidate (url:null — unreached site, value
        // unresolved): surfaced via focusedView/structuralCandidates, not
        // a learnable endpoint. Skip before new URL(null) fabricates a
        // "/null" path.
        if (callSite.url == null || callSite.url === "") continue;
        // Source-map: resolve this finding's minified path params (e/a) → the
        // page's declared names (owner/repo) by running the library on the
        // finding's own call-site location (originalPositionFor → original fetch
        // line). Tag each PATH param in URL order. Library-only, on shown
        // findings, no engine instrumentation, no bundle transform.
        var _smNames = _resolvePathParamNames(callSite, analysis.scriptOffsets, analysis.traceMapsByUrl);
        if (_smNames && callSite.params) {
          var _spi = 0;
          for (var _sp = 0; _sp < callSite.params.length; _sp++) {
            if ((callSite.params[_sp].location || "query") === "path") {
              if (_spi < _smNames.length) callSite.params[_sp]._sourceMapName = _smNames[_spi];
              _spi++;
            }
          }
        }
        // Skip data:/blob:/about: URLs — those are inline content, not
        // API endpoints. Registering them as services produces empty-
        // host records with garbled paths (the URL parser reads the
        // scheme as origin="null" and path starts mid-string).
        if (/^(data|blob|about|javascript):/i.test(callSite.url)) continue;

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
          var _csMeta = tab;
          var _csBaseForRel = (_csMeta && _csMeta.url) ? _csMeta.url : analysis.sourceUrl;
          csUrl = new URL(callSite.url, _csBaseForRel);
          interfaceName = extractInterfaceName(csUrl);
        }

        var _astDocEntry = learnFromAstCallSite(tab, interfaceName, callSite, analysis.sourceUrl);
        // Refine interfaceName for endpoint registration if the call site
        // got promoted to a prefix bucket via observed-prefix clustering.
        if (_astDocEntry && _astDocEntry.doc && _astDocEntry.doc.name) {
          interfaceName = _astDocEntry.doc.name;
        }

        // Register endpoint for popup display — separate concern from
        // method registration (endpoint list shows "what fetches exist
        // on this page," method list shows "what API endpoints we know").
        var bundleId = analysis.sourceUrl ? analysis.sourceUrl.replace(/^https?:\/\//, "").slice(-60) : "";
        // __feUrlShape renders an opaque path segment as {id}; the worker's
        // ep() emits that, but new URL() (csUrl) re-encodes the braces to
        // %7B/%7D. Decode so the learned endpoint path keeps the OpenAPI
        // template ({id}) instead of %7Bid%7D — used for BOTH the dedup key
        // and the stored path so they stay consistent.
        var _csPath = csUrl.pathname.replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}");
        var epKey = isDynamic
          ? "AST DYN " + bundleId + " " + (callSite.enclosingFunction || "anon") + " " + callSite.method + " " + fc
          : "AST " + callSite.method + " " + csUrl.hostname + _csPath;   // include HOST: an endpoint is method+host+path. Path-only collapsed same-path endpoints across DIFFERENT hosts (and across sites in the cumulative store) → lost the moat's "many sites per session" surface. Mirrors the network key (method + hostname + pathname).
        // DEDUP by STRUCTURAL identity: the SAME endpoint driven with opaque-POSITIONAL args ({arg0}, from
        // __hostDrive's JS-side drive) and with NAMED args ({id}, from the grind's declared-name drive) yields
        // TWO keys for ONE endpoint (verified: spa_gated 5 raw / 4 distinct). Collapse {..} path-param segments
        // to a structural key; on collision keep the DECLARED-name record over the positional one. Only
        // {placeholder} segments normalize, so genuinely-distinct endpoints (differing elsewhere) never merge.
        // The stored KEY stays the raw path, so probe/replay of the surviving record are unaffected.
        if (!isDynamic) {
          if (!tab._epNorm) tab._epNorm = new Map();
          var _structKey = "AST " + callSite.method + " " + csUrl.hostname + _csPath.replace(/\{[^}]*\}/g, "{}");
          var _posRe = /\{arg\d+\}/;
          var _priorKey = tab._epNorm.get(_structKey);
          if (_priorKey && _priorKey !== epKey && tab.endpoints.has(_priorKey)) {
            if (_posRe.test(_priorKey) && !_posRe.test(epKey)) {
              tab.endpoints.delete(_priorKey); tab._epNorm.set(_structKey, epKey);   // prior positional, new declared -> upgrade (add epKey below)
            } else {
              epKey = _priorKey;   // keep prior (declared/equal); has() below is true -> skip add, no dup
            }
          } else {
            tab._epNorm.set(_structKey, epKey);
          }
        }
        if (!tab.endpoints.has(epKey)) {
          var _epMeta = tab;
          tab.endpoints.set(epKey, {
            url: isDynamic ? callSite.url : csUrl.href,
            method: callSite.method,
            host: isDynamic ? sourceHost : csUrl.hostname,
            path: isDynamic ? callSite.url : _csPath,
            service: interfaceName,
            source: isDynamic ? "ast_dynamic" : "ast_analysis",
            pageUrl: _epMeta ? _epMeta.url : null,
            // AST-captured required headers (the SET the bundle attached at the
            // host edge, per-header literal/opaque) — transport metadata shown
            // in the Send panel so the endpoint is actually usable.
            requiredHeaders: (callSite.headers && Object.keys(callSite.headers).length) ? callSite.headers : null,
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

    // DOM-derived endpoints (href/src/action/data-* values from page
    // markup). Per user directive: "what DOM gets sent in the first
    // place [is] useful for learning". Surfaced into tab.endpoints
    // alongside AST-derived ones, with source="dom_html_<kind>" for
    // origin tracking.
    var domEps = analysis.domEndpoints || [];
    for (var dei = 0; dei < domEps.length; dei++) {
      var domEp = domEps[dei];
      try {
        var deBase = (tab && tab.url) || analysis.sourceUrl;
        if (!deBase) continue;
        var deResolved = new URL(domEp.url, deBase);
        if (/^(data|blob|about|javascript):/i.test(deResolved.protocol)) continue;
        var deKey = "DOM " + (domEp.source || "html") + " " + deResolved.href;
        if (tab.endpoints.has(deKey)) continue;
        tab.endpoints.set(deKey, {
          url: deResolved.href,
          method: "?",
          host: deResolved.hostname,
          path: deResolved.pathname,
          service: extractInterfaceName(deResolved),
          source: "dom_" + (domEp.source || "html").replace(/-/g, "_"),
          pageUrl: deBase,
          firstSeen: Date.now(),
        });
      } catch (e) {
        /* DOM-endpoint registration failed for one entry — almost always
           a malformed `url` attribute (relative path the bundle didn't
           normalize, javascript: handler we didn't filter early enough,
           etc.). Other entries in the batch still register. Surface
           so a real DOM-extraction regression on a vendor page is
           visible instead of disappearing into an empty endpoint list. */
        console.debug("[brain] DOM endpoint registration failed:", e && e.message || e, "url=" + (domEp && domEp.url), "src=" + (domEp && domEp.source));
      }
    }

    // Store security findings on tab state (only once per analysis — skip if already merged)
    var secSinks = analysis.securitySinks || [];
    var dangerousPats = analysis.dangerousPatterns || [];
    if ((secSinks.length || dangerousPats.length) && !analysis._securityMerged) {
      analysis._securityMerged = true;
      if (!tab._securityFindings) tab._securityFindings = [];
      var _mfMeta = tab || null;
      // REPLACE any prior entry for this source, don't append — the deep grind
      // streams partials each carrying the GROWING accumulated securitySinks for
      // the same sourceUrl, so appending would pile up snapshots (mergeToGlobal
      // set()s by sourceUrl so globalStore stays correct, but the tab array would
      // leak). Keep the latest (most complete) per source.
      for (var _sfx = tab._securityFindings.length - 1; _sfx >= 0; _sfx--)
        if (tab._securityFindings[_sfx].sourceUrl === analysis.sourceUrl) tab._securityFindings.splice(_sfx, 1);
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
  // Schedule the eviction sweep after this merge: the doc's forced-exec run has produced
  // results (globalStore updated, residue parked to IDB), so once it is no longer in-flight
  // the sweep drops its transient RAM view. Debounced to one pending timer.
  _scheduleEvictSweep();
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

/* _handleFormMetadata removed: forms are now learned through the
   engine path. The Lexbor-parsed document inside the QuickJS worker
   exposes every form; __hostDrive walks them and calls form.submit(),
   which routes through the same G.fetch hook the bundle's own JS
   reaches. The resulting @H records flow through learnFromAstCallSite
   (engine output) and learnFromRequest (live traffic) into the same
   discoveryDoc, with per-field literal/opaque provenance preserved.
   No separate content-script DOM walk. */

function _handleFormSubmit(documentId, msg, tabId, frameId) {
  if (!msg.url || !msg.fields) return;
  var tab = _docForLearning(documentId);
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

  _pushGlobalLog(entry, tabId, documentId, frameId);

  // Learn from the form submission
  learnFromRequest(documentId, service, entry, entry.requestHeaders);
  mergeToGlobal(tab);
  notifyPopup(tabId);
}

// Content scripts handle CONTENT_HTML, CONTENT_DOM, CONTENT_FORMS,
// CONTENT_FORM_SUBMIT, RESPONSE_BODY, and SCRIPT_SOURCE. CONTENT_KEYS
// / CONTENT_ENDPOINTS were removed — those were heuristic regex
// scans over HTML text, which is Lexbor's parsing job, and the
// endpoint heuristic ("url contains /api/" etc.) produced source:
// page_source entries with no method, no params, no taint info.
// Now: raw HTML lands in CONTENT_HTML, gets parsed by Lexbor in the
// worker; real endpoints come from forced execution observing actual
// fetch/XHR at the host edge.
function handleContentMessage(msg, sender) {
  if (!sender.tab) return;
  const tabId = sender.tab.id;
  // The browser-provided documentId is the ONLY stable per-document identity (a
  // tabId:frameId pair is reused across navigations with a DIFFERENT origin). No
  // documentId (pre-106 Chrome / opaque) → we cannot document-scope it → fail
  // closed (no tabId:frameId fallback — that would conflate distinct origins).
  const documentId = sender.documentId;
  if (!documentId) {
    console.debug("[brain:content] %s dropped — no sender.documentId (cannot document-scope; fail closed)", msg && msg.type);
    return;
  }
  // Stamp THIS document's identity on its DocData. `origin` is the MessageSender
  // principal (via _senderOrigin, which also populates _docOrigins) — NEVER
  // url-derived. `url` is the document's OWN url (display + relative TARGET base),
  // NOT the tab's top-page url. tabId/frameId are routing/UI fields only.
  // Always track the requesting frame security principal (populates _docOrigins
  // via _senderOrigin; a mixed-origin buffer fails closed). CONTENT_HTML is the
  // analysis trigger and (re)creates this document DocData; every other message
  // is passive traffic that must NOT create or resurrect a doc -- a doc evicted
  // on grind-completion stays evicted (its log is global + sender-tagged, and the
  // learners route to globalStore-only via _docForLearning). getDoc (create) ONLY
  // for CONTENT_HTML; otherwise refresh identity if the doc still lives.
  const _origin = _senderOrigin(sender);
  const doc = (msg.type === "CONTENT_HTML") ? getDoc(documentId) : state.docs.get(documentId);
  if (doc) {
    doc.tabId = tabId;
    doc.frameId = sender.frameId || 0;
    if (sender.url) doc.url = sender.url;
    doc.origin = _origin;
    if (sender.tab.title) doc.title = sender.tab.title;
  }

  // RESPONSE_BODY comes from intercept.js via content.js relay
  if (msg.type === "RESPONSE_BODY") {
    handleResponseBody(tabId, msg, sender.frameId, documentId);
    return;
  }

  // (AST_RESUMED / AST_PARTIAL are handled in the onMessage router's
  // isExtensionPage branch — they originate from the offscreen doc, which has
  // no sender.tab, so handleContentMessage would have dropped them at the top.)

  // PROBE_HIT: intercept.js recorded a sink that saw the active
  // probe marker. Correlate to an open exploit-probe session via the
  // hit's own marker (carried in the URL at probe time) and append.
  if (msg.type === "PROBE_HIT") {
    // The hit carries the finding's crypto.randomUUID (`id`) the PoC payload
    // passed to apiclientsink — correlate to the session by that id. (Legacy
    // `marker` field tolerated for safety.)
    const hid = msg.hit && (msg.hit.id || msg.hit.marker);
    if (typeof hid === "string") {
      const ses = _probeSessions.get(hid);
      if (ses) {
        ses.hits.push(Object.assign({}, msg.hit, { id: hid, tabId: tabId, frameId: sender.frameId || 0 }));
      }
    }
    return;
  }

  // SCRIPT_SOURCE / SCRIPTS_LOADED removed: content.js no longer ships per-script
  // bodies or a load signal. One CONTENT_HTML per document drives the analysis; the
  // engine sources every script from that HTML (inline via the SSR phase, external
  // via __feLoadScript, dynamic via the createElement host edge).

  if (msg.type === "CONTENT_FORM_SUBMIT") {
    _handleFormSubmit(documentId, msg, tabId, sender.frameId);
    return;
  }

  if (msg.type === "CONTENT_PING") {
    var arr = _contentPings.get(documentId);
    if (!arr) { arr = []; _contentPings.set(documentId, arr); }
    arr.push({ at: msg.at || Date.now(), pageUrl: msg.pageUrl || null });
    return;
  }

  if (msg.type === "CONTENT_HTML") {
    // ONE MESSAGE PER DOCUMENT. The engine (Lexbor) parses this HTML and runs EVERY
    // script in document order in one realm — inline directly, external <script src>
    // via __feLoadScript (the safeFetch chokepoint), all by qjs_run_doc_scripts.
    // The analysis target is the realm the grind drives; content.js ships nothing
    // but this HTML. Keyed by documentId, NOT tabId: a tab holds many documents/
    // frames (iframes, navigations), each its OWN origin — keying by tab merges them
    // and collapses the credentialed-read principal. The principal for this
    // document's analysis (safeFetch SSRF origin + window.location + the credentialed
    // reply-seed) is THIS document's own origin/url, from the browser-provided sender.
    doc._pageHtml = String(msg.html || "");
    var _dk = documentId;
    var _buf = _scriptBuffers.get(_dk);
    if (!_buf) { _buf = {}; _scriptBuffers.set(_dk, _buf); }
    // Each buffer IS this document's frame record, identified by its documentId
    // (docKey) — never a tab-relative frameId. There is no separate frame table: a
    // tab's frames ARE its buffers (GET_FRAMES lists them; routing + the response
    // origin-lookup key on documentId, which Chrome's messaging accepts directly).
    // The origin is AUTHORITATIVE (sender.origin via _senderOrigin), never URL-parsed
    // (a sandboxed frame's URL parses to a real origin it does NOT have — its true
    // origin is opaque, which _senderOrigin reflects), with no nav-event-vs-message
    // race. We do NOT record "isTop": a frame can't prove it is the main frame from
    // its own report — a fenced frame is its tree's root and would impersonate it.
    _buf.tabId = tabId;
    _buf.docKey = _dk;
    _buf.origin = doc.origin;                                         // credentialed-read principal (per-document; = _senderOrigin(sender))
    _buf.url = doc.url || (sender.tab && sender.tab.url) || "";       // SSRF origin + window.location (document's own url)
    _buf.pageHtml = doc._pageHtml;
    _buf.scripts = [];   // engine-sourced: Lexbor parses the HTML, qjs_run_doc_scripts runs inline + external scripts — one system
    _buf.pending = 0;
    _buf.loadFired = true;
    // Prioritization is driven by THIS message: a document just delivered its one
    // CONTENT_HTML, so it is the live "analyze me now" signal — no webNavigation or
    // tab-activation guess about which frame is the main one. Stamp THIS document's
    // load recency on its own buffer (the review-queue picker orders by it),
    // keyed by documentId (NEVER url — same url != same content).
    _buf.lastActivatedTs = Date.now();
    _analyzeCombinedScripts(_dk);   // through the review queue: per-document in-flight guard + recency priority (focused doc first), bounded CPU
    notifyPopup(tabId);
    return;
  }

  /* CONTENT_DOM handler removed: Lexbor in the engine worker parses
     the same CONTENT_HTML and exposes the spec DOM the bundle reads
     via document.querySelector / dataset / getAttribute. No
     parallel content-script DOM snapshot. */
}

// Popup messages — sender.tab is absent for popup contexts.
async function handlePopupMessage(msg, _sender, sendResponse) {
  await _globalStoreReady;
  const tabId = msg.tabId;            // aggregate/UI filter + Chrome routing (NEVER a storage key)
  const documentId = msg.documentId;  // per-document RPC target (resolved via _docFromMsg)

  switch (msg.type) {
    case "GET_STATE": {
      // A matched document, else a transient empty view so the popup still shows
      // the GLOBAL cumulative moat (serializeTabData overlays globalStore).
      const tab = _docFromMsg(msg) || _emptyDocView();
      const data = serializeTabData(tab);
      sendResponse(data);
      return;
    }

    case "GET_FRAMES": {
      // Authoritative frame tree from the BROWSER: chrome.webNavigation.getAllFrames
      // returns each live frame's documentId + url + frameType. frameType is the
      // only trustworthy "is this the main frame" signal — a fenced frame is reported
      // as "fenced_frame", so it CANNOT impersonate the outermost frame (which a
      // self-reported isTop could). Each frame's AUTHORITATIVE origin comes from the
      // DURABLE _docOrigins map by documentId (survives buffer reclamation after
      // review); frameId is Chrome's routing id for tabs.sendMessage, not an identity.
      let _wnFrames = null;
      try { _wnFrames = await swRpc("webNavigation.getAllFrames", { tabId }); }
      catch (e) { console.debug("[GET_FRAMES] getAllFrames failed:", e && e.message || e); }
      const out = (_wnFrames || []).map((f) => ({
        frameId: f.frameId,
        documentId: f.documentId || null,
        url: f.url,
        origin: _originForDoc(f.documentId),
        isMain: f.frameType === "outermost_frame",
      }));
      sendResponse(out.length ? out : [{ frameId: 0, documentId: null, url: "", origin: "", isMain: true }]);
      return;
    }

    case "PROBE_ENDPOINT": {
      const _pdoc = _docFromMsg(msg);
      if (!_pdoc) { sendResponse(null); return; }
      probeEndpoint(_pdoc.documentId, msg.endpointKey).then((result) => {
        sendResponse(result);
      });
      return true;
    }

    case "DISCOVER_SERVICE": {
      const tab = _docFromMsg(msg);
      if (!tab) { sendResponse(null); return; }
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
      const fetchFn = makePageFetchFn(tab.tabId, tab.documentId);
      discoverServiceInfo(discoverUrl.toString(), headers, { fetchFn }).then(
        (result) => {
          tab.probeResults.set(`svc:${msg.endpointKey}`, result);
          if (result.scopes?.length) {
            const svc = ep.service || extractInterfaceName(new URL(ep.url));
            tab.scopes.set(svc, result.scopes);
          }
          mergeToGlobal(tab);
          notifyPopup(tab.tabId);
          sendResponse(result);
        },
      );
      return true;
    }

    case "FETCH_DISCOVERY": {
      const tab = _docFromMsg(msg);
      if (!tab) { sendResponse(null); return; }
      const ep = tab.endpoints.values().next().value;
      const hostname =
        msg.hostname || (ep?.host ?? `${msg.service}.googleapis.com`);
      const apiKeys = collectKeysForService(tab, msg.service, hostname);
      if (msg.apiKey && !apiKeys.includes(msg.apiKey)) apiKeys.push(msg.apiKey);
      if (ep?.apiKey && !apiKeys.includes(ep.apiKey)) apiKeys.push(ep.apiKey);
      fetchDiscoveryForService(tab.documentId, msg.service, hostname, apiKeys).then(
        () => {
          sendResponse(serializeTabData(tab));
        },
      );
      return true;
    }

    case "CLEAR_TAB": {
      // The main Clear button: delete ALL extension data and stop ALL work.
      (async function () {
        // 1. Stop the offscreen worker FIRST — terminate it (kills the running
        //    wasm grind outright) and delete its resumable-grind DB — so no
        //    in-flight analysis or resume can repopulate what we wipe next.
        try { await sendToOffscreen({ type: "AST_CLEAR" }); } catch (e) {}
        // 2. Global findings + the persisted gapiStore (cleared inside clearGlobalStore).
        await clearGlobalStore();
        // 3. All in-memory request logs + per-tab working state, so the next
        //    navigation starts from a genuinely empty slate.
        state.docs.clear();
        _scriptBuffers.clear();
        _wsConnState.clear();
        sendResponse({ ok: true });
      })();
      return true;   // async sendResponse
    }

    case "GET_ANALYSIS_OPTS": {
      // IDB-backed analysis options (cooling + workers UI knobs). On first
      // read, the record may not exist yet — return an empty object so
      // the popup falls back to its HTML defaults. The brain's single
      // "global" object store uses key "analysisOpts" for this record.
      let opts = null;
      try { opts = await _idbGet("analysisOpts"); }
      catch (e) {
        /* Reading the IDB opts record failed — surface so a corrupt/locked
           IDB is diagnosable. Return an empty object so the popup still
           renders with HTML defaults. */
        console.warn("[brain] GET_ANALYSIS_OPTS idb read failed:", e && e.message || e);
      }
      sendResponse(opts || {});
      return;
    }

    case "SET_ANALYSIS_OPTS": {
      // Merge the incoming partial opts into the persisted record, then
      // broadcast via astDispatch so the worker pool (dispatcher) updates
      // its size + forwards yieldThrottleMs to each pool worker.
      let cur = null;
      try { cur = await _idbGet("analysisOpts"); } catch (e) {
        console.warn("[brain] SET_ANALYSIS_OPTS idb read failed:", e && e.message || e);
      }
      const next = Object.assign({}, cur || {}, msg.opts || {});
      try { await _idbSet("analysisOpts", next); }
      catch (e) {
        /* Persist failed — the in-memory propagation below still happens
           so the user's setting takes effect this session, but it won't
           survive a restart. Surface so quota/lock is visible. */
        console.warn("[brain] SET_ANALYSIS_OPTS idb put failed:", e && e.message || e);
      }
      try { self.astDispatch({ type: "SET_ANALYSIS_OPTS", opts: next }); }
      catch (e) {
        console.warn("[brain] SET_ANALYSIS_OPTS dispatch failed:", e && e.message || e);
      }
      sendResponse({ ok: true });
      return;
    }

    case "CLEAR_LOG": {
      // Request logs live in-memory only now (session storage layer removed
      // — the offscreen document's stable lifetime makes the mirror moot).
      // Clearing the in-memory array IS the operation.
      if (msg.clearAll) {
        globalRequestLog = [];
      } else {
        if (tabId == null) return;
        globalRequestLog = globalRequestLog.filter(function (r) { return r.tabId !== tabId; });
      }
      sendResponse({ ok: true });
      return;
    }

    case "GET_TAB_LIST": {
      // Closed tabs' documents stay in state.docs (d.closed=true) so one pass
      // covers live AND closed entries.
      // Roll up the documentId-keyed docs into one row per tab (the network tab
      // filters by tab purely in the UI). A tab is "closed" only once ALL its
      // documents are; title/url come from the main-frame document.
      // Count from the GLOBAL log (grouped by tabId); enrich title/url/closed
      // from the tab's live documents (an evicted doc leaves its log entries
      // but no identity, so that tab degrades to "Tab N").
      const _byTab = new Map();
      for (const r of globalRequestLog) {
        if (r.tabId == null) continue;
        let row = _byTab.get(r.tabId);
        if (!row) { row = { tabId: r.tabId, title: "", url: "", count: 0, closed: false }; _byTab.set(r.tabId, row); }
        row.count++;
      }
      _byTab.forEach((row, tid) => {
        const docs = docsForTab(tid);
        if (docs.length) {
          const main = docs.find((dd) => dd.frameId === 0) || docs[0];
          row.url = main.url || row.url; row.title = main.title || row.title;
          row.closed = docs.every((dd) => !!dd.closed);
        }
      });
      const tabs = [];
      _byTab.forEach((r) => tabs.push({ tabId: r.tabId, title: r.title || ("Tab " + r.tabId), url: r.url, count: r.count, closed: r.closed }));
      sendResponse(tabs);
      return;
    }

    case "GET_ALL_LOGS": {
      const result = {};
      const filter = msg.filter; // "all" | tabId (number)
      // Aggregate per tab across its documents; each entry keeps its own
      // documentId/frameId for the popup's per-frame sub-views.
      for (const ent of globalRequestLog) {
        if (ent.tabId == null) continue;
        if (filter !== "all" && filter !== ent.tabId) continue;
        let r = result[ent.tabId];
        if (!r) { r = { meta: { title: "", url: "", closed: false }, requestLog: [] }; result[ent.tabId] = r; }
        r.requestLog.push(ent);
      }
      for (const tid in result) {
        const docs = docsForTab(Number(tid));
        if (docs.length) {
          const main = docs.find((dd) => dd.frameId === 0) || docs[0];
          result[tid].meta.url = main.url || result[tid].meta.url;
          result[tid].meta.title = main.title || result[tid].meta.title;
          result[tid].meta.closed = docs.every((dd) => !!dd.closed);
        }
        if (!result[tid].meta.title) result[tid].meta.title = "Tab " + tid;
      }
      sendResponse(result);
      return;
    }

    case "GET_DISCOVERY_CHANGES": {
      sendResponse(Object.fromEntries(globalStore.discoveryChanges));
      return;
    }

    case "GET_ENDPOINT_SCHEMA": {
      // GLOBAL — keyed by endpointKey/service against the cumulative store,
      // never per-tab/document (only the network-stream log is per-tab).
      const result = resolveEndpointSchema(
        msg.endpointKey,
        msg.service,
        msg.methodId,
      );
      sendResponse(result);
      return;
    }

    case "SEND_REQUEST": {
      const _srdoc = _docFromMsg(msg);
      if (!_srdoc) { sendResponse({ error: "no document for request" }); return; }
      executeSendRequest(_srdoc.documentId, msg).then((result) => {
        sendResponse(result);
      });
      return true;
    }

    case "WS_SEND_MSG": {
      if (tabId == null) return;
      // documentId-ONLY routing: a frameId is reused across navigations and
      // could resolve to a DIFFERENT origin; with no target option,
      // tabs.sendMessage would broadcast to every frame. No documentId → refuse.
      if (!msg.documentId) { sendResponse({ error: "blocked: no documentId" }); return true; }
      var _wsOpts = { documentId: msg.documentId };
      swRpc("tabs.sendMessage", tabId, {
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
      const conn = _findWsConn(tabId, msg.channelId);
      // Also return the messages array for the WS console
      let messages = [];
      if (conn) {
        const entry = _findLogEntry(tabId, msg.channelId, "WEBSOCKET");
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
      // documentId-ONLY routing (no frameId fallback — reused across navs / origins).
      if (!msg.documentId) { sendResponse({ error: "blocked: no documentId" }); return true; }
      var _pmOpts = { documentId: msg.documentId };
      swRpc("tabs.sendMessage", tabId, {
        type: "PM_SEND_MSG",
        data: msg.data,
        targetOrigin: msg.targetOrigin,
      }, _pmOpts).then(() => {
        // Record sent message in the log entry (intercept.js can't capture outgoing postMessage)
        const entry = _findLogEntry(tabId, msg.channelId, "POSTMESSAGE");
        if (entry) {
          entry.messages.push({ dir: "sent", time: Date.now(), body: msg.data || "", base64: false });
          if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);
              notifyPopup(tabId);
        }
        sendResponse({ ok: true });
      }).catch((err) => sendResponse({ error: err.message }));
      return true;
    }

    case "PM_GET_STATUS": {
      if (tabId == null) return;
      const entry = _findLogEntry(tabId, msg.channelId, "POSTMESSAGE");
      sendResponse({
        readyState: 1, // postMessage is always "active"
        messages: entry ? (entry.messages || []) : [],
      });
      return;
    }

    case "MC_SEND_MSG": {
      if (tabId == null) return;
      // documentId-ONLY routing (no frameId fallback — reused across navs / origins).
      if (!msg.documentId) { sendResponse({ error: "blocked: no documentId" }); return true; }
      var _mcOpts = { documentId: msg.documentId };
      swRpc("tabs.sendMessage", tabId, {
        type: "MC_SEND_MSG",
        channelId: msg.channelId,
        data: msg.data,
      }, _mcOpts).then(() => {
        const entry = _findLogEntry(tabId, msg.channelId, "MSGCHANNEL");
        if (entry) {
          entry.messages.push({ dir: "sent", time: Date.now(), body: msg.data || "", base64: false });
          if (entry.messages.length > 200) entry.messages.splice(0, entry.messages.length - 200);
              notifyPopup(tabId);
        }
        sendResponse({ ok: true });
      }).catch((err) => sendResponse({ error: err.message }));
      return true;
    }

    case "MC_GET_STATUS": {
      if (tabId == null) return;
      const entry = _findLogEntry(tabId, msg.channelId, "MSGCHANNEL");
      sendResponse({
        readyState: 1, // port is active once transferred
        messages: entry ? (entry.messages || []) : [],
      });
      return;
    }

    case "BUILD_REQUEST": {
      // GLOBAL — build/export reads the cumulative store by endpointKey/service,
      // never per-tab/document.
      buildExportRequest(msg).then((result) => {
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
        // Return the EXACT PoC JS so the popup displays AND the sandbox runs the
        // one artifact. error surfaces a build failure (e.g. opaque page URL).
        sendResponse({ success: true, sessionId: session.marker, pocJs: session.pocJs || null, error: session.error || null });
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
      // Correlation is via the relayed apiclientsink(<id>) hits (content.js →
      // PROBE_HIT), which content.js drains durably from the documentElement
      // mirror even if it loaded after the sink fired. No tab-finding / flag-read
      // needed: hits arrive by id.
      {
        sendResponse({
        success: true,
        status: ses.status,
        marker: ses.marker,
        strategy: ses.strategy,
        pageUrl: ses.pageUrl,
        hits: ses.hits.slice(),
        executed: ses.executed || null,
        // The EXACT PoC JavaScript that was run on the attacker origin — the
        // single artifact the UI both displays and executes (no separate
        // payload system). A researcher copies this and runs it verbatim.
        pocJs: ses.pocJs || null,
        attackerOrigin: PROBE_ATTACKER_ORIGIN,
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
          // Orchestrator-populated fields (_runStructuredPlan): targetUrl,
          // events. Lets a reviewer see EXACTLY what URL + payloads the
          // SW dispatched, so a NOT REPRODUCED can be traced back to a
          // concrete gap (URL gate, payload shape, CSP).
          targetUrl: ses.recipe && ses.recipe.targetUrl || null,
          events: ses.recipe && Array.isArray(ses.recipe.events) ? ses.recipe.events : null,
        },
        });
      }
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
      // GLOBAL per-service edit — never takes a documentId/tabId. The discovery
      // store is global; a rename sets customName=true, which every merge path
      // preserves, so editing the global doc directly persists across later page
      // merges (no mergeToGlobal needed).
      const { service, schemaName, fieldKey, newName } = msg;
      const docEntry = globalStore.discoveryDocs.get(service);
      if (!docEntry || !docEntry.doc) {
        sendResponse({ error: "No discovery document for " + service });
        return;
      }
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
          sendResponse({ ok: true });
        } else {
          sendResponse({ error: "Parameter not found for rename" });
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
        sendResponse({ ok: true });
      }
      return;
    }

    case "EXPORT_OPENAPI": {
      // Per-SERVICE and fully GLOBAL — never takes a documentId/tabId. The
      // discovery store is global; nothing in the popup is per-tab except the
      // network-stream log filter (where tabId/documentId is just log metadata).
      const svc = msg.service;
      const docEntry = globalStore.discoveryDocs.get(svc);
      if (!docEntry?.doc) {
        sendResponse({ error: "No discovery document found for " + svc });
        return;
      }
      const openapi = convertDiscoveryToOpenApi(docEntry.doc, svc);
      sendResponse({ ok: true, spec: openapi });
      return;
    }

    case "IMPORT_OPENAPI": {
      // GLOBAL — an imported spec is a user-provided service definition, not a
      // page fetch; it goes straight into the global discovery store (no doc).
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
          } catch (e) {
            /* OpenAPI spec's `servers[0].url` isn't a valid absolute URL
               (relative or templated like `{protocol}://api/v1`). Fall
               back to spec.info.title below. Surface so a malformed
               spec doesn't silently lose its hostname-based service
               key. */
            console.debug("[brain] OpenAPI servers[0].url parse failed:", e && e.message || e, "url=" + spec.servers[0].url);
          }
        }
        if (!svcName) {
          svcName = (spec.info?.title || "imported")
            .toLowerCase().replace(/[^a-z0-9.]/g, "_");
        }

        // Convert to internal Discovery format
        const sourceUrl = spec.servers?.[0]?.url || "https://" + svcName;
        const doc = convertOpenApiToDiscovery(spec, sourceUrl);

        // Merge with existing doc if present
        const existing = globalStore.discoveryDocs.get(svcName);
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
          var _prevGlobalEntry = globalStore.discoveryDocs.get(svcName);
          const entry = {
            status: "found",
            url: sourceUrl,
            method: "IMPORT",
            apiKey: null,
            fetchedAt: Date.now(),
            doc,
            isVirtual: false,
            pageUrls: _prevGlobalEntry?.pageUrls || new Set(),
            frameOrigins: _prevGlobalEntry?.frameOrigins || new Set(),
          };
          globalStore.discoveryDocs.set(svcName, entry);
        }
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


// The real cross-origin attacker origin the PoC runs on. A minimal, stable,
// CSP-free page (IANA's reserved example domain) so the injected PoC can frame
// the target and run without the attacker page's own policy interfering. This
// is what a researcher would paste the PoC onto.
const PROBE_ATTACKER_ORIGIN = "https://example.com/";


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

// Register an exploit-probe session keyed by the finding's crypto.randomUUID for hit-correlation. The
// PoC ARTIFACT itself is no longer compiled here — the Z3 pocPlan compiler was deleted (best-design:
// forced execution is the whole engine). session.pocJs stays null until the forced-exec PoC (provenance
// inversion + forced-exec search + concrete-replay verify) lands and populates it.
// Reverse a captured @S taint SHAPE into a WORKING PoC for the EXISTING verifier. The payload calls
// apiclientsink('<marker>') — intercept.js defines that hook and relays it (content.js → PROBE_HIT),
// so the SAME correlation path that verifies any probe confirms REAL EXPLOIT here; no parallel checker.
// Template-FREE: the payload is COMPUTED from the sink's PARSE CONTEXT (scanned from the concrete text
// around the hole), and the DELIVERY vector comes from the hole's SOURCE tag ({hash}/{search}/{pm}).
// Returns {pocJs, payload, context, source, delivery} or null (with why) when the shape lacks a source.
function _htmlHoleContext(prefix) {
  // Walk the concrete HTML before the hole, tracking tag/attribute state, to know how the attacker
  // input must break OUT to reach an executable position. This is observation of the real interleaving,
  // not a signature match.
  var inTag = false, quote = null;
  for (var i = 0; i < prefix.length; i++) {
    var c = prefix[i];
    if (quote) { if (c === quote) quote = null; continue; }
    if (inTag) { if (c === '"' || c === "'") quote = c; else if (c === '>') inTag = false; }
    else if (c === '<') inTag = true;
  }
  if (quote) return quote === '"' ? "attr-dq" : "attr-sq";
  return inTag ? "tag" : "text";
}
function buildPocFromShape(sinkName, shape, pageUrl, marker) {
  if (!shape || !pageUrl) return { pocJs: null, why: "missing shape/pageUrl" };
  var m = /\{(hash|search|pm|reply)\}/.exec(shape);   // the FIRST attacker-controlled (source-tagged) hole
  if (!m) return { pocJs: null, why: "hole source unknown (generic {}) — no delivery vector; complete source-tagging first" };
  var source = m[1];
  var prefix = shape.slice(0, m.index);
  var call = "apiclientsink('" + marker + "')";           // the verifier's proof hook (marker = crypto.randomUUID)
  // Slash-separated attributes (no spaces) so the payload survives URL-fragment/query delivery intact.
  var htmlCore = "<img/src=x/onerror=" + call + ">";
  var context, payload;
  if (sinkName === "eval") {
    context = "js";
    payload = call + ";//";                                // raw JS statement; trailing // neutralises the suffix
  } else if (sinkName === "href" || sinkName === "setAttribute") {
    context = "url";
    payload = "javascript:" + call;                        // href/action/formaction navigation sink
  } else {                                                 // innerHTML/outerHTML/insertAdjacentHTML/document.write
    var hc = _htmlHoleContext(prefix);
    context = "html-" + hc;
    if (hc === "attr-dq") payload = '">' + htmlCore;       // break out of ="…"
    else if (hc === "attr-sq") payload = "'>" + htmlCore;  // break out of '…'
    else if (hc === "tag") payload = ">" + htmlCore;       // close the open tag, then inject
    else payload = htmlCore;                               // text content: inject directly
  }
  // Delivery: how the attacker gets `payload` into the hole. The SAME payload string fills the hole; the
  // vector is the source. pocJs is eval'd in the sandbox (real user gesture), so window.open is allowed.
  var base, delivery, pocJs;
  if (source === "hash") {
    base = pageUrl.split("#")[0] + "#" + payload;
    delivery = "navigate victim to a URL whose fragment is the payload";
    pocJs = "window.open(" + JSON.stringify(base) + ', "_blank");';
  } else if (source === "search") {
    // location.search consumed whole → the query string IS the payload (no known param name).
    base = pageUrl.split("#")[0].split("?")[0] + "?" + payload;
    delivery = "navigate victim to a URL whose query string is the payload";
    pocJs = "window.open(" + JSON.stringify(base) + ', "_blank");';
  } else if (source === "pm") {
    delivery = "open the victim, then postMessage the payload from the attacker window";
    pocJs = "var w = window.open(" + JSON.stringify(pageUrl.split("#")[0]) + ', "_blank");' +
            " setTimeout(function(){ try { w && w.postMessage(" + JSON.stringify(payload) + ', "*"); } catch (e) {} }, 800);';
  } else {   // reply: server-reflected — needs the server to echo attacker input; can't be delivered client-side alone
    return { pocJs: null, why: "source is a server reply (reflected) — PoC needs server-side reflection, not client delivery", source: source, payload: payload, context: context };
  }
  return { pocJs: pocJs, payload: payload, context: context, source: source, delivery: delivery };
}
function startExploitProbe(msg) {
  _pruneProbeSessions();
  const { strategy, waitMs, findingId, paramName, fieldPath, sinkType, sinkName, decoders, preconditions, pocPlan, shape } = msg || {};
  if (!strategy && !pocPlan && !shape) throw new Error("need a taint shape (from the @S finding), or a strategy / pocPlan");
  if (strategy === "search" && !paramName) {
    throw new Error("search strategy requires paramName — derive it from the finding's observed source (e.g. the argument to URLSearchParams.get)");
  }
  if (fieldPath && !Array.isArray(fieldPath)) throw new Error("fieldPath must be an array of string field names");
  if (pocPlan && (typeof pocPlan !== "object" || !Array.isArray(pocPlan.events))) {
    throw new Error("pocPlan must be an object with events[] array — pass the finding's `poc` field verbatim");
  }

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
  // The correlation id is a crypto.randomUUID — it rides INSIDE the PoC payload
  // (apiclientsink("<id>")), is relayed back by intercept.js → content.js, and
  // keys this session. Never placed in the URL.
  const marker = (self.crypto && crypto.randomUUID)
    ? crypto.randomUUID()
    : ("probe-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 10));
  const session = {
    marker, status: "running", strategy: strategy || (pocPlan ? "pocPlan" : null),
    pageUrl: pageUrl || null,
    findingId: findingId || null, sourceUrl: (msg && msg.sourceUrl) || null,
    paramName: paramName || null,
    fieldPath: Array.isArray(fieldPath) ? fieldPath.slice() : [],
    sinkType: sinkType || null, sinkName: sinkName || null,
    decoders: Array.isArray(decoders) ? decoders.slice() : [],
    preconditions: Array.isArray(preconditions) ? preconditions.slice() : [],
    pocPlan: pocPlan || null,
    waitMs: wait,
    hits: [], openedTabs: [], createdAt: Date.now(), finishedAt: null, error: null,
  };
  // Build THE PoC artifact and register the session for hit-correlation. We do
  // NOT auto-open any tab: the PoC is executed by the USER clicking Run inside
  // the sandboxed attacker page (poc-sandbox.html) — that real click is the user
  // activation window.open needs, and the sandbox's opaque origin is a genuine
  // cross-origin attacker context. The SAME pocJs is what the popup displays.
  // When the PoC fires the sink, intercept.js → content.js → PROBE_HIT lands on
  // this session's marker, and EXPLOIT_PROBE_STATUS reports REAL EXPLOIT.
  // Compile THE PoC by REVERSING the captured @S taint shape (opaque provenance): parse context around
  // the source-tagged hole → context-appropriate payload calling the verifier's apiclientsink(marker) →
  // delivery from the source tag. No solver, no template. Feeds the EXISTING PROBE_HIT correlation.
  var _poc = shape ? buildPocFromShape(sinkName || sinkType, shape, session.pageUrl, marker) : null;
  session.pocJs = (_poc && _poc.pocJs) || null;
  session.pocMeta = _poc ? { context: _poc.context, source: _poc.source, payload: _poc.payload, delivery: _poc.delivery } : null;
  session.pocWhy = _poc && !_poc.pocJs ? _poc.why : null;   // @WHY when a shape can't yield a client-deliverable PoC
  session.status = "prepared";
  _probeSessions.set(marker, session);
  return session;
}


async function buildExportRequest(msg) {
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

  // API key: user override → endpoint → auto. GLOBAL — the endpoint is keyed in
  // the cumulative store, not per-tab/document.
  const ep = msg.endpointKey ? globalStore.endpoints.get(msg.endpointKey) : null;
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
      const docEntry = globalStore.discoveryDocs.get(msg.service);
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
  "CONTENT_HTML",
  "CONTENT_PING",
  "CONTENT_FORM_SUBMIT",
  "RESPONSE_BODY",
  "PROBE_HIT",
]);
const _contentPings = new Map();  // documentId -> [{ at, pageUrl }, ...]

// The brain runs in the OFFSCREEN document and receives messages DIRECTLY:
// chrome.runtime.sendMessage broadcasts to every extension context, so both our
// content scripts (in web renderers) and the popup reach this document without
// the SW relaying anything. The thin SW forwards only ONE browser event the
// offscreen can't observe and that no document message carries: __evt TAB_REMOVED
// (so per-tab transient state is freed when a tab closes). Navigation/activation
// are NOT forwarded — prioritization is driven by each document's CONTENT_HTML.
//   • __evt TAB_REMOVED (from the SW, extension origin) → _onTabRemoved
//   • CONTENT_TYPES from a web-page origin   → handleContentMessage (UNTRUSTED;
//                    real browser-verified sender = tab/frame/url)
//   • extension-page origin (popup)          → handlePopupMessage
// sender.id is NOT a trust signal (every onMessage sender carries our id). The
// boundary is sender.url: extension-origin = trusted (popup/SW); a web URL = an
// untrusted content script. __evt is honored ONLY from the extension origin, so a
// compromised renderer can't forge a browser event (e.g. TAB_REMOVED for a
// victim tab) by broadcasting one straight to this document.
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (sender.id !== chrome.runtime.id) return;
  if (!msg) return;
  // Extension-origin sender = trusted (popup + SW). Authenticated by sender.URL
  // here: the SERVICE WORKER (which forwards __evt) has no frame, so Chrome leaves
  // its MessageSender.origin undefined (a browser quirk), and only sender.url carries
  // the extension origin for a SW. Document->document hops use sender.ORIGIN instead
  // (offscreen->SW in background.js, offscreen->popup in popup.js). A web content
  // script's sender.url is a web URL -> untrusted -> handleContentMessage.
  const fromExtOrigin = !!(sender.url && sender.url.startsWith(EXTENSION_ORIGIN + "/"));

  if (msg.__evt) {
    if (!fromExtOrigin) return;
    if (msg.__evt === "TAB_REMOVED") _onTabRemoved(msg.tabId);
    return;
  }

  if (typeof msg.type !== "string") return;

  // Content-script message — arrives straight from the page renderer. sender is
  // the browser-verified content-script sender (sender.tab / sender.frameId /
  // sender.url = the frame), so no synthetic tab context and no spoof surface. A
  // trusted extension page never sends a CONTENT_TYPE, so an extension-origin one
  // is dropped (defense in depth).
  if (CONTENT_TYPES.has(msg.type)) {
    if (fromExtOrigin) return;
    // Chunked-message reassembly: content.js's _sendChunked splits payloads over
    // ~16 MiB; rebuild before dispatch (no truncation, no caps).
    if (msg.__chunk) {
      const merged = _absorbChunk(msg);
      if (merged) handleContentMessage(merged, sender);
      return;
    }
    handleContentMessage(msg, sender);
    return;
  }

  // Discriminate by sender.url, NOT sender.tab: an action popup's sender.tab is
  // the ACTIVE tab (defined!), so a sender.tab check would wrongly drop popup
  // messages. Extension-page origin (popup) → handlePopupMessage.
  if (!fromExtOrigin) return;
  handlePopupMessage(msg, sender, sendResponse);
  return true; // keep sendResponse alive for async handlePopupMessage
});

const _chunkStreams = new Map();  // streamId -> { parts:[], received, total, payloadKey, envelope }
function _absorbChunk(msg) {
  const c = msg.__chunk;
  let s = _chunkStreams.get(c.streamId);
  if (!s) {
    s = { parts: new Array(c.total), received: 0, total: c.total, payloadKey: c.payloadKey, envelope: null };
    _chunkStreams.set(c.streamId, s);
  }
  if (typeof s.parts[c.index] === "undefined") {
    s.parts[c.index] = msg[c.payloadKey];
    s.received++;
  }
  if (!s.envelope) {
    // First part seen — snapshot the envelope minus the payload field
    // and the __chunk metadata. All parts carry the same envelope
    // contents EXCEPT the payload-key field, which gets reassembled.
    s.envelope = Object.assign({}, msg);
    delete s.envelope.__chunk;
    delete s.envelope[c.payloadKey];
  }
  if (s.received < s.total) return null;
  _chunkStreams.delete(c.streamId);
  const out = Object.assign({}, s.envelope);
  out[c.payloadKey] = s.parts.join("");
  return out;
}

// Forwarded by the SW as __evt TAB_REMOVED (the offscreen can't observe tabs).
function _onTabRemoved(tabId) {
  // Closed-tab request logs remain VIEWABLE for the offscreen-document's
  // lifetime. The old session-storage mirror existed because the brain used
  // to live in the SW (evicted) — the offscreen brain doesn't need that
  // mirror, but we still preserve the in-memory state.docs entries so the
  // popup's "All Tabs" / per-tab-history filter can show closed-tab logs
  // until the user clicks the bin button.
  // Mark every document of this tab closed (its logs stay viewable). The state
  // is documentId-keyed; a tab close marks all its documents. WebSocket
  // connections die with the page — free each document's (_wsConnState is
  // documentId-keyed too).
  for (const d of docsForTab(tabId)) {
    d.closed = true;
    d.closedAt = Date.now();
    _wsConnState.delete(d.documentId);
  }
  // The per-document script buffers are NOT freed on tab close: each document's
  // combined source backs a resumable/background deep grind that keeps learning
  // the closed tab's API surface (and resumes across sessions) without
  // revisiting the page. They are reclaimed only by the 🗑️ bin hard-reset
  // (_scriptBuffers.clear in CLEAR_TAB). Keyed by documentId, never tabId.
}

// ─── Send Request: Schema Resolution ─────────────────────────────────────────

/**
 * Resolve the full schema for an endpoint by merging discovery doc + probe data.
 * Returns a unified schema the popup can use to build a form.
 */
function resolveEndpointSchema(endpointKey, service, methodId) {
  // GLOBAL — endpoints/discovery/probes live in the cumulative store keyed by
  // endpointKey/service. Nothing here is per-tab/document (only the network log
  // is); the popup reviews the cumulative cross-site moat.
  const ep = endpointKey ? globalStore.endpoints.get(endpointKey) : null;

  // If no endpoint but we have service+methodId (virtual), create a dummy ep object for context
  if (!ep && (!service || !methodId)) return { source: "none", endpoint: null };

  const targetService = ep?.service || service;

  let source = "none";
  let discoveryMethod = null;
  let parameters = null;
  let bodyFields = null;
  let bodySchemaName = null;
  let contentTypes = [];

  // 1. Discovery doc — global, per-service (the discovery store is not per-tab).
  const discoveryEntry = globalStore.discoveryDocs.get(targetService);
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
        // AST-learned required headers (the header SET the bundle attached at
        // the host edge, per-header literal/opaque) — transport metadata the
        // Send panel surfaces. Whitelisted out before, so the popup never saw
        // it for AST endpoints (which load via service+methodId, ep=null).
        requiredHeaders: match.method.requiredHeaders || null,
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
            // Real declared name from the page's source map (e.g. `e`→`owner`)
            // for display; `name` stays the minified key for URL substitution.
            _sourceMapName: pDef._sourceMapName || null,
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
    ? globalStore.probeResults.get(endpointKey)
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
          requiredHeaders: ep.requiredHeaders || null,
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

function encodeFormToJson(rootFields) {
  // Iterative tree builder. Each work item populates a target object
  // (or array element) from a fields list. Nested message/repeated
  // fields enqueue empty sub-objects whose children arrays drive a
  // later iteration. Replaces self-recursion so deeply-nested form
  // structures encode without growing the JS call stack.
  const root = {};
  const queue = [{ fields: rootFields, target: root }];
  while (queue.length > 0) {
    const { fields, target } = queue.shift();
    for (const f of fields) {
      const isObj = f.type === "message" || f.type === "object";

      if (f.label === "repeated") {
        const list = [];
        target[f.name] = list;
        if (Array.isArray(f.value)) {
          for (const v of f.value) {
            if (v && typeof v === "object" && !Array.isArray(v) && Array.isArray(v.children)) {
              const sub = {};
              list.push(sub);
              queue.push({ fields: v.children, target: sub });
            } else {
              list.push(isObj ? v : coerceValue(v, f.type));
            }
          }
        } else if (Array.isArray(f.children)) {
          for (const item of f.children) {
            if (Array.isArray(item.children)) {
              const sub = {};
              list.push(sub);
              queue.push({ fields: item.children, target: sub });
            } else {
              list.push(coerceValue(item.value, f.type));
            }
          }
        }
        continue;
      }

      if (isObj) {
        // Message/object: prefer children tree; fall back to raw value
        // when the caller has a parsed object but no tree (e.g. replay
        // auto-fill from captured JSON). Always surface the field even
        // when empty so servers see `{variables: {}}` rather than
        // dropping it.
        if (Array.isArray(f.children) && f.children.length) {
          const sub = {};
          target[f.name] = sub;
          queue.push({ fields: f.children, target: sub });
        } else if (f.value && typeof f.value === "object" && !Array.isArray(f.value)) {
          target[f.name] = f.value;
        } else {
          target[f.name] = {};
        }
        continue;
      }

      if (f.value == null && !f.children?.length) continue;
      target[f.name] = coerceValue(f.value, f.type);
    }
  }
  return root;
}

/**
 * Encode form fields as a JSPB array (indexed by field number).
 */
function encodeFormToJspb(rootFields) {
  // Iterative: each work item builds one JSPB array from a fields list.
  // Nested messages enqueue a fresh array that subsequent iterations
  // populate. Replaces self-recursion so deeply-nested message trees
  // (or pathological repeated-message arrays) encode without growing
  // the JS call stack.
  function buildOne(fields) {
    let mx = 0;
    for (const f of fields) {
      if (f.number > mx) mx = f.number;
    }
    return mx === 0 ? [] : new Array(mx).fill(null);
  }
  const root = buildOne(rootFields);
  const queue = [{ fields: rootFields, target: root }];
  while (queue.length > 0) {
    const { fields, target } = queue.shift();
    for (const f of fields) {
      if (!f.number) continue;
      const targetIdx = f.number - 1;
      if (f.type === "message" && f.label !== "repeated") {
        const sub = buildOne(f.children || []);
        target[targetIdx] = sub;
        queue.push({ fields: f.children || [], target: sub });
      } else if (f.label === "repeated" && f.type === "message" && Array.isArray(f.value)) {
        const repeated = [];
        target[targetIdx] = repeated;
        for (const item of f.value) {
          if (item && item.children) {
            const sub = buildOne(item.children);
            repeated.push(sub);
            queue.push({ fields: item.children, target: sub });
          } else if (Array.isArray(item)) {
            repeated.push(item);
          } else {
            repeated.push(item);
          }
        }
      } else if (f.label === "repeated" && Array.isArray(f.value)) {
        target[targetIdx] = f.value.map((v) => coerceValue(v, f.type));
      } else {
        target[targetIdx] = coerceValue(f.value, f.type);
      }
    }
  }
  return root;
}

/**
 * Encode form fields as binary protobuf.
 *
 * Iterative driver — replaces the prior encodeFormToProtobuf ↔
 * encodeSinglePbField mutual recursion that descended through nested
 * message types. Each stack frame encodes one fields array. When a
 * field is a nested message with children, the driver pushes a sub-
 * frame for the children and stashes the parent's pending field
 * number; the sub-frame's encoded bytes are wrapped via
 * pbEncodeLenField and appended to the parent's parts when the
 * sub-frame pops. encodeSinglePbField stays a pure scalar leaf
 * encoder — its message branch is gone.
 */
function encodeFormToProtobuf(fields) {
  const PACKABLE = new Set([
    "int32", "int64", "uint32", "uint64", "sint32", "sint64",
    "bool", "enum", "fixed32", "fixed64", "sfixed32", "sfixed64",
    "float", "double",
  ]);
  const stack = [{ fields: fields, parts: [], i: 0, pendingNum: null }];
  let lastBytes = null;
  while (stack.length > 0) {
    const top = stack[stack.length - 1];
    if (lastBytes !== null) {
      // A child frame just finished; wrap its bytes as a length-
      // delimited field on this frame's pending field number.
      top.parts.push(pbEncodeLenField(top.pendingNum, lastBytes));
      top.pendingNum = null;
      lastBytes = null;
    }
    let pushedSubFrame = false;
    while (top.i < top.fields.length) {
      const f = top.fields[top.i];
      if (!f.number) { top.i++; continue; }
      if (f.value == null && !(f.children && f.children.length)) { top.i++; continue; }
      if (f.label === "repeated" && Array.isArray(f.value)) {
        if (PACKABLE.has(f.type)) {
          const innerParts = [];
          for (const v of f.value) {
            innerParts.push(encodeSinglePbFieldRaw(f.type, v));
          }
          const packed = concatBytes.apply(null, innerParts.length ? innerParts : [new Uint8Array(0)]);
          top.parts.push(pbEncodeLenField(f.number, packed));
        } else {
          // Non-packable types (string, bytes, message): individual
          // tag+value pairs. The original passed children=null here,
          // so message-typed repeated fields fell through to the
          // string-coerce default; preserving that behavior.
          for (const v of f.value) {
            top.parts.push(encodeSinglePbField(f.number, f.type, v));
          }
        }
        top.i++;
        continue;
      }
      if (f.type === "message" && f.children && f.children.length) {
        // Push sub-frame for nested message; parent waits at its
        // pending field number until the child returns its bytes.
        top.pendingNum = f.number;
        top.i++;
        stack.push({ fields: f.children, parts: [], i: 0, pendingNum: null });
        pushedSubFrame = true;
        break;
      }
      top.parts.push(encodeSinglePbField(f.number, f.type, f.value));
      top.i++;
    }
    if (pushedSubFrame) continue;
    const bytes = concatBytes.apply(null, top.parts.length ? top.parts : [new Uint8Array(0)]);
    stack.pop();
    lastBytes = bytes;
  }
  return lastBytes;
}

// Encode a single scalar protobuf field (tag + value). Pure leaf —
// message-typed fields are now handled by encodeFormToProtobuf's
// driver, so the message branch is no longer here. The 4-arg signature
// is kept so existing call sites compile, but the children param is
// unused.
function encodeSinglePbField(num, type, value /*, children */) {
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
async function executeSendRequest(documentId, msg) {
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
  const tab = _docForLearning(documentId);
  const tabId = (tab && tab.tabId != null) ? tab.tabId : msg.tabId; // Chrome routing for pageContextFetch — the doc's tab; fall back to msg.tabId for cross-tab replay
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
      // Use the first matching key. We do NOT tiebreak by "same origin as the
      // current tab": mapping a tabId to an origin assumes the main frame and races
      // navigation (banned). The keys are already filtered to this service/host.
      apiKey = svcKeys[0];
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
      documentId,
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
                extractKeysFromText(documentId, val, url, "send_response_grpc");
              }
            });
          } catch (e) {
            /* One frame's protobuf decode failed — other frames in the
               same response still process. Surface so a malformed frame
               on an otherwise-valid response is visible. */
            console.debug("[brain] send-response grpc-web frame decode failed:", e && e.message || e, "url=" + url);
          }
        }
      }
      // Serialize bytes as base64 array for message passing
      bodyResult = {
        format: "grpc_web",
        bytesB64: uint8ToBase64(bytes),
        raw: resp.body,
        size: bytes.length,
      };
    } catch (e) {
      /* Outer gRPC-Web frame parse failed — bytes weren't valid frame
         format. Fall back to binary blob so the reviewer still sees
         the raw response, but surface the parse failure so the format
         mismatch (likely a server bug or wrong content-type) is
         diagnosable. */
      console.warn("[brain] send-response grpc-web parse failed:", e && e.message || e, "url=" + url);
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
          extractKeysFromText(documentId, val, url, "send_response_protobuf");
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
    requestLog: tab.documentId ? globalRequestLog.filter(function (r) { return r.documentId === tab.documentId; }) : [],
    securityFindings: mergedSecurityFindings(tab),
    resolverErrors: tab._resolverErrors || [],
  };
}

