// offscreen-brain.js — the API-learning brain (analysis LOGIC). Runs in the OFFSCREEN
// document (loaded by ast-worker.html). It owns globalStore + discovery + AST merge +
// the request log + ALL the popup message handlers, and persists to IndexedDB. The thin
// service worker (background.js) forwards browser events here and performs privileged
// chrome.tabs.* calls on this brain's behalf (see _swRpc / __rpc).
//
// DIRECTION: this file is LOGIC that is being DELETED into the C engine (lexbor+quickjs
// owns identity/dedup/detection/aggregation, used like a browser). The irreducible
// SECURITY.md relay (safeFetch/IDB/WASM-load) was already extracted OUT to bridge.js
// (reached via self.astDispatch). As the engine absorbs the aggregation, this file
// shrinks toward nothing and is deleted; what remains is only the untrusted WASM cannot
// do (chrome messaging + popup render), which stays trusted per SECURITY.md.

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
    var files = ["offscreen-brain.js", "bridge.js", "lib/qjs/qjs.mjs"];   // brain logic + host bridge + engine module (wasm rebuild that changes behavior bumps qjs.mjs's embedded fingerprint)
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

// ─── Engine dispatch ──────────────────────────────────────────────────────────
// self.astDispatch(msg) → Promise<response> is installed by the merged v2 host
// bridge at the bottom of THIS file (it drives the engine WASM). AST_* messages go
// straight to it — a direct call, not chrome.runtime.sendMessage (which would NOT
// reach a same-document listener; the sender's own context is excluded).
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

// Shape/template holes ({}, {id}) survive new URL().href/.pathname as %7B..%7D (the WHATWG path
// percent-encode set includes {}). Decode so a learned endpoint URL/path keeps the canonical hole the
// dedup + UI recognize. Idempotent — normal URLs never contain %7B — so applying it broadly is safe.
// The single home for this decode (previously duplicated inline at several endpoint builders).
function _decHoles(s) {
  return typeof s === "string" ? s.replace(/%7[Bb]/g, "{").replace(/%7[Dd]/g, "}") : s;
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

// (Key extraction -- KEY_PATTERNS + extractKeysFromText -- extracted to lib/keys.js, loaded first.)

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
  // The engine↔JS contract guarantees a result OBJECT (never null) — a null here is a should-never-happen
  // in the engine's own output, not a case to `if(!result)return`-past.
  DCHECK(result && typeof result === "object", "_mergeFrontierResult: engine produced a non-object frontier result");
  if (!result) return;   // release: the DCHECK is stripped, tolerate the impossible-in-dev case (don't crash the user)
  try {
    // Merge on EITHER surface: an XSS-only page carries verified @S PoCs with no endpoints. Gating on
    // fetchCallSites alone dropped every incremental sink (they only surface here now, not just at teardown).
    var hasEps = result.fetchCallSites && result.fetchCallSites.length;
    var hasSinks = result.securitySinks && result.securitySinks.length;
    if (!hasEps && !hasSinks) return;   // legitimately empty (a page with nothing learned yet) — not an invariant break
    var view = _emptyDocView();
    view.url = sourceUrl || ""; view.tabUrl = sourceUrl || "";
    result.sourceUrl = sourceUrl || result.sourceUrl || "";
    mergeASTResultsIntoVDD(view, [result], null, true);
    mergeToGlobal(view);
  } catch (e) {
    // A merge THROW is a real bug (malformed engine output / a broken merge invariant). DEV: surface it LOUD
    // at the origin, never log-and-continue — that swallow is the exact defensive robustness this forbids.
    if (self.APICLIENT_DEV) throw e;
    console.debug("[frontier] merge error: %s", e && e.message);   // release: don't block the user on an unbuildable case
  }
}

/* Opportunistic idle burst of the ONE host-level attention: advance the globally-highest-value parked
   frontier a few rounds and merge each per-origin. Serialized (a single in-flight burst) + yields, so
   it NEVER blocks a fresh tab's analysis -- BFS ACROSS sites: a deep site's residue resumes by value in
   the background while new tabs are learned promptly via the review queue. */
// The bridge's ONE host pool advances cold parked recipes itself (admit rehydrates the highest-value ones
// when live work drains). A cold engine finalizing calls back here so its facts merge to the GLOBAL moat.
self.onFrontierAdvance = function (sourceUrl, result) {
  // Do NOT wrap in a swallowing catch — that would silence the _mergeFrontierResult DCHECK (a broken engine↔JS
  // contract must crash LOUD in dev). notifyPopup is a UI side-effect whose own failure is non-fatal, isolated.
  _mergeFrontierResult(sourceUrl, result);
  try { notifyPopup(null); } catch (e) { if (self.APICLIENT_DEV) throw e; }
};
function _driveGlobalFrontierBurst() {   // idle nudge: kick the ONE pool to re-check the cold frontier
  if (typeof self.kickHostPool !== "function") return;   // optional edge (bridge may not be up yet) — an ABSENCE, not an error
  try { self.kickHostPool(); } catch (e) { if (self.APICLIENT_DEV) throw e; }   // a THROW from the pool is a real bug -> dev-loud
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

// (Persistence -- IndexedDB open/get/set/clear + (de)serialize + save/loadGlobalStore -- extracted to
// lib/persistence.js, loaded first.)

function _mergeDocInto(existingDoc, newDoc) {
  if (!existingDoc || !existingDoc.resources) return newDoc || existingDoc || null;
  if (!newDoc || !newDoc.resources) return existingDoc;
  for (const bk in newDoc.resources) {
    const nb = newDoc.resources[bk];
    if (!nb || !nb.methods) continue;
    let eb = existingDoc.resources[bk];
    if (!eb) { existingDoc.resources[bk] = nb; continue; }
    if (!eb.methods) eb.methods = {};
    for (const mk in nb.methods) {
      const nm = nb.methods[mk], em = eb.methods[mk];
      if (!em) { eb.methods[mk] = nm; continue; }   // distinct endpoint from another page -> keep both
      if (nm.parameters) {                          // same method key: union each param's example values
        em.parameters = em.parameters || {};
        for (const pn in nm.parameters) {
          const np = nm.parameters[pn], ep = em.parameters[pn];
          if (!ep) { em.parameters[pn] = np; continue; }
          const ev = Array.isArray(ep._astValidValues) ? ep._astValidValues : [];
          const nv = Array.isArray(np._astValidValues) ? np._astValidValues : [];
          for (const x of nv) if (ev.indexOf(x) < 0) ev.push(x);
          if (ev.length) ep._astValidValues = ev;
        }
      }
    }
  }
  if (newDoc.schemas) { existingDoc.schemas = existingDoc.schemas || {}; for (const sk in newDoc.schemas) if (!existingDoc.schemas[sk]) existingDoc.schemas[sk] = newDoc.schemas[sk]; }
  return existingDoc;
}
function mergeToGlobal(tab) {
  // Central merge — EVERY analysis result (hot tab + cold frontier) funnels here, so guard the DocView contract
  // once: a malformed tab is a should-never-happen the callers must not construct, not a shape to defend against.
  DCHECK(tab && typeof tab === "object", "mergeToGlobal: tab (DocView) must be an object");
  DCHECK(tab.apiKeys && typeof tab.apiKeys[Symbol.iterator] === "function", "mergeToGlobal: tab.apiKeys must be an iterable Map");
  DCHECK(globalStore && globalStore.apiKeys, "mergeToGlobal: globalStore must be initialized before a merge");
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
      // UNION path-param examples so a later paramless re-emit (e.g. a re-visit before the concolic reply
      // re-run landed) never DROPS values a prior emit learned — the moat is monotonic.
      if (ge.pathParams || v.pathParams) {
        var _mp = new Map();
        for (var _s of [ge.pathParams || [], v.pathParams || []]) for (var _pp of _s) {
          var _cur = _mp.get(_pp.name) || new Set();
          for (var _val of (_pp.values || [])) _cur.add(_val);
          _mp.set(_pp.name, _cur);
        }
        v.pathParams = Array.from(_mp, function (e) { return { name: e[0], values: Array.from(e[1]).slice(0, 20) }; });
      }
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
        doc: _mergeDocInto(_existingGDoc && _existingGDoc.doc, v.doc) || null,   // UNION, not replace: keep every page's methods
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

// (Endpoint identity -- classifyInterface/extractInterfaceName/refineByObservedPrefix/
// migrateToCommonPrefixBucket/calculateMethodMetadata -- extracted to lib/grouping.js, loaded first.)

function _hexToBytes(hex) {
  if (typeof hex !== "string") return new Uint8Array(0);
  const n = hex.length >> 1;
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = parseInt(hex.substr(i * 2, 2), 16) | 0;
  }
  return out;
}

// (VDD passive learning -- learnFromAstCallSite/learnFromRequest/learnFromResponse + stats + templated-
// method matching -- extracted to lib/learn.js, loaded first. One problem per file.)

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

// (Discovery-doc probing -- diff/fetch/probe/virtual-doc build -- extracted to lib/discovery-probe.js.)

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
      /* Recency-priority pick instead of FIFO shift — the tab the user is LOOKING
         AT (most recently activated: lastActivatedTs, stamped at CONTENT_HTML) gets
         analyzed next; docs without a timestamp default to 0 and trail. ORDER only,
         never COVERAGE (every queued doc is still analyzed eventually). Inlined here —
         the shared lib/priority.js WFQ mirror was DELETED (the C engine owns the real
         WFQ: flow_weight/wfq_yield). Splicing IS the pick; the comparison is pure. */
      var docKey = null;
      if (_reviewQueue.length) {
        var _bestI = 0, _bestTs = ((_scriptBuffers.get(_reviewQueue[0]) || {}).lastActivatedTs) | 0;
        for (var _k = 1; _k < _reviewQueue.length; _k++) {
          var _ts = ((_scriptBuffers.get(_reviewQueue[_k]) || {}).lastActivatedTs) | 0;
          if (_ts > _bestTs) { _bestI = _k; _bestTs = _ts; }
        }
        docKey = _reviewQueue[_bestI];
        _reviewQueue.splice(_bestI, 1);
      }
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
         Different tabId CAN still overlap (see below). The re-queue isn't
         dropped; the late scripts are folded into the current buf and the
         next drain iteration after the in-flight one finishes picks them
         up via the cache-miss path. */
      if (_analysisInflight.has(docKey)) continue;   // per-DOCUMENT in-flight guard (distinct documents of a tab may overlap)
      _analysisInflight.add(docKey);
      /* Fire-and-forget — do NOT await. astDispatch ENQUEUES this document
         into the host WFQ pool (bridge.js): a bounded set of live wasm
         instances the pool interleaves by value-of-information (each engine
         exposes qjs_top_weight; it advances the highest one a slice, then
         re-ranks) — so this is TWO levels of ONE WFQ: flows within a doc
         (C engine, g_reg) and engines across docs (the host pool). This
         recency pick only orders the doc's ENTRY into the pool; the pool
         then arbitrates by weight and admission-gates creation to the RAM
         cap. The cold tail (parked recipes) is advanced separately by
         _driveGlobalFrontierBurst -> driveFrontier. The per-doc cache +
         _dataEpoch guard keep results attribution-correct; errors surface
         via analysis.resolverErrors / _astError, never a bare catch. */
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
      responseHeaders: getDoc(buf.docKey)._responseHeaders || {},   // real CSP/Content-Type -> engine (header-CSP is the PRIMARY policy; meta-CSP is secondary)
      // Participate in the GLOBAL cross-session frontier: this engine's residue parks to IDB under RAM
      // pressure (resource-driven, host-side) and rehydrates by value order later. With headroom the page
      // runs to completion in one visit — nothing is lost to a clock; there is NO dispatch/step quantum.
      persist: true,
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

  // NOTE: lazy-chunk consumption is no longer a separate host-side re-review
  // step. The C engine now fetches every discovered chunk IN-RUN: it surfaces
  // chunk URLs (analysis.chunkUrls) and bridge.runEngine's step loop fetches
  // each and feeds it back via qjs_provide, so the chunk's code (classic, or a
  // linked ESM module graph) is analyzed inside the same instance as it arrives.
  // There is no combined-whole re-analysis and no per-schedule re-instantiation
  // to gate on.

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
  // aborted mid-bundle, or a host-edge stub is missing).
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
    // The fromReply reply-example (a fully-opaque URL that IS a reply field ->
    // fetch the source, extract the field) is ENGINE-SIDE now: g_reply_table +
    // @REPLYWANT/qjs_provide inject the concrete reply so r.json() returns the
    // real value in-flow. That logic must NOT live in this bridge (engine is the
    // browser; the offscreen only relays safeFetch/IDB/chrome). So resolverErrors
    // here is purely a DIAGNOSTIC surface (@E crashes) for the popup.
    for (var _rei = 0; _rei < analysis.resolverErrors.length; _rei++) {
      var _re = analysis.resolverErrors[_rei];
      console.debug("[AST:resolver] %s: %s", _re.context, _re.message);
      if (_re.stack) console.debug(_re.stack);
      if (!_seenRe.has(_re.message)) {
        _seenRe.add(_re.message);
        tab._resolverErrors.push({ context: _re.context, message: _re.message, snippet: _re.snippet || null });
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
        var _csPath = _decHoles(csUrl.pathname);
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
            // new URL().href percent-encodes shape holes ({} -> %7B%7D); decode so the endpoint URL keeps
            // the canonical `{}` param placeholder the dedup/UI recognize (path is already decoded via _csPath).
            url: isDynamic ? callSite.url : _decHoles(csUrl.href),
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
            // Concrete PATH-PARAM examples the engine computed (e.g. a reply field `orgId=acme-42` collapsed
            // into /api/org/{}/members). The rich per-doc method schema carries these, but it is EVICTED after
            // review, so without persisting them onto the flat endpoint the cumulative moat loses the real
            // learned values — the whole point of the tool. Carried here so they survive eviction.
            pathParams: (function () {
              var pp = (callSite.params || []).filter(function (p) { return (p.location === "path") && p.validValues && p.validValues.length; });
              return pp.length ? pp.map(function (p) { return { name: p.name, values: p.validValues.slice(0, 20) }; }) : null;
            })(),
            // (Request body: the @BODY params[location:body] feed the discovery method schema, which is the
            //  SINGLE source the Send panel (schema.requestBody) and OpenAPI export (convertDiscoveryToOpenApi)
            //  read. An endpoint-entry requestBody copy was DEAD — resolveEndpointSchema never surfaced it — so
            //  it is deleted, not duplicated here.)
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
          url: _decHoles(deResolved.href),
          method: "?",
          host: deResolved.hostname,
          path: _decHoles(deResolved.pathname),
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
    doc._responseHeaders = msg.responseHeaders || {};   // real navigation response headers (CSP, Content-Type) — same-origin fetch(location.href), so all readable
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

    // EXPLOIT_PROBE_STATUS: report whether the engine's poc, run against the real page, fired the sink.
    // Correlation is the relayed apiclientsink(<marker>) hit (intercept.js → content.js → PROBE_HIT). A hit
    // = REAL EXPLOIT (Chrome agrees with the engine); no hit = divergence / CSP-blocked.
    case "EXPLOIT_PROBE_STATUS": {
      const ses = msg.sessionId ? _probeSessions.get(msg.sessionId) : null;
      if (!ses) { sendResponse({ error: "session not found or expired" }); return; }
      sendResponse({
        success: true, status: ses.status, marker: ses.marker, pageUrl: ses.pageUrl,
        hits: ses.hits.slice(), executed: ses.executed || null,
        pocJs: ses.pocJs || null, error: ses.error || null,
        startedAt: ses.createdAt, finishedAt: ses.finishedAt || null,
      });
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
// ENGINE AGREEMENT: the live PoC is the ENGINE's exact solved breakout input (its `poc` field) — the input
// a candidate-replay flow drove through the REAL page code+branches+filters to the sink where it broke out.
// We do NOT re-derive a payload from the shape (that second solver would diverge: it wouldn't carry the
// gate-guided prefix like `cmd:`, a filter bypass, or the exact transform-surviving form the engine found).
// We only (a) map the engine's fire-marker X9 -> the verifier's proof hook apiclientsink(marker), and
// (b) DELIVER it to the REAL page via the source (hash/search/pm), so real Chrome — not the model — is the
// ground truth. Disagreement (engine says breakout, Chrome doesn't fire) is a precise engine-fidelity bug.
function buildPocFromShape(sinkName, poc, shape, pageUrl, marker, srcpath, gatefields) {
  if (!poc || !pageUrl) return { pocJs: null, why: "missing engine poc / pageUrl" };
  if (!shape) return { pocJs: null, why: "missing source shape" };
  var m = /\{(hash|search|pm|reply)\}/.exec(shape);   // which attacker-controlled source the engine's poc rides
  if (!m) return { pocJs: null, why: "hole source unknown (generic {}) — no delivery vector; complete source-tagging first" };
  var source = m[1];
  // STRUCTURED attacker input: srcpath ("{pm}.html", "{pm}.a.b") means the sink read a FIELD of the source
  // object, so the payload must ride in that field (post {html:payload}), not the bare value. The field path
  // is everything after the source token; empty = whole-value use (unchanged).
  var fieldPath = null;
  if (srcpath) { var fm = /^\{(?:hash|search|pm|reply)\}\.(.+)$/.exec(srcpath); if (fm) fieldPath = fm[1].split("."); }
  var call = "apiclientsink('" + marker + "')";           // the verifier's proof hook (marker = crypto.randomUUID)
  // The engine's X9 is a bare fire-marker: `onerror=X9`, `javascript:X9`, `1;X9();//`. Map every X9 (call or
  // ref form) to the apiclientsink CALL so the real sink, on firing, relays the proof. This is the ONLY edit
  // to the engine's payload — its structure (breakout + gate prefix + surviving transforms) is untouched.
  var payload = String(poc).split("X9()").join(call).split("X9").join(call);
  var context = "engine:" + (sinkName || "?");
  // Delivery: how the attacker gets `payload` into the source hole. pocJs is eval'd in the sandbox (real user
  // gesture), so window.open is allowed; it navigates to the REAL page with the payload in the real source.
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
    // The message value: the bare payload for whole-value use, else an OBJECT nesting the payload at the read
    // field path (`{html}=e.data; sink(html)` -> post {html:payload}; `e.data.a.b` -> {a:{b:payload}}).
    var msgVal;
    if (!fieldPath) { msgVal = payload; }
    else {
      // Structured object: the sink field carries the payload, PLUS every sibling GATE field the real handler
      // requires (`if(e.data.type==='render')` -> set type:'render'), or the gate blocks the sink live.
      msgVal = {};
      var setPath = function (obj, path, v) { var o = obj; for (var i = 0; i < path.length - 1; i++) { if (typeof o[path[i]] !== "object" || o[path[i]] === null) o[path[i]] = {}; o = o[path[i]]; } o[path[path.length - 1]] = v; };
      setPath(msgVal, fieldPath, payload);
      if (gatefields && typeof gatefields === "object") { for (var gk in gatefields) if (Object.prototype.hasOwnProperty.call(gatefields, gk)) setPath(msgVal, gk.split("."), gatefields[gk]); }
    }
    delivery = "open the victim, then postMessage the " + (fieldPath ? "object " + JSON.stringify(msgVal).replace(/"/g, "") : "payload") + " from the attacker window";
    pocJs = "var w = window.open(" + JSON.stringify(pageUrl.split("#")[0]) + ', "_blank");' +
            " setTimeout(function(){ try { w && w.postMessage(" + JSON.stringify(msgVal) + ', "*"); } catch (e) {} }, 800);';
  } else {   // reply: server-reflected — needs the server to echo attacker input; can't be delivered client-side alone
    return { pocJs: null, why: "source is a server reply (reflected) — PoC needs server-side reflection, not client delivery", source: source, payload: payload, context: context };
  }
  return { pocJs: pocJs, payload: payload, context: context, source: source, delivery: delivery };
}
function startExploitProbe(msg) {
  _pruneProbeSessions();
  const { waitMs, findingId, sinkName, shape, poc, srcpath, gatefields } = msg || {};
  if (!poc || !shape) throw new Error("need the engine's poc + source shape (from the @S finding)");

  // pageUrl: the page the finding was observed on (recorded in securityFindings). The caller may pass it;
  // else resolve from the finding. Never guessed.
  let pageUrl = msg && msg.pageUrl ? msg.pageUrl : null;
  if (!pageUrl && msg.sourceUrl) {
    const entry = globalStore.securityFindings.get(msg.sourceUrl);
    if (entry && entry.pageUrl) pageUrl = entry.pageUrl;
  }

  const wait = Math.max(1000, Math.min(30000, Number(waitMs) || 6000));
  // The correlation id is a crypto.randomUUID — it rides INSIDE the payload as apiclientsink("<id>"),
  // relayed back by intercept.js → content.js, and keys this session. Never placed in the URL.
  const marker = (self.crypto && crypto.randomUUID)
    ? crypto.randomUUID()
    : ("probe-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 10));
  const session = {
    marker, status: "running", pageUrl: pageUrl || null,
    findingId: findingId || null, sourceUrl: (msg && msg.sourceUrl) || null,
    sinkName: sinkName || null, waitMs: wait,
    hits: [], createdAt: Date.now(), finishedAt: null, error: null,
  };
  // ENGINE AGREEMENT: build the delivery from the engine's EXACT poc (X9 -> apiclientsink) via its source.
  // The user runs it by clicking Run in the sandboxed attacker page (poc-sandbox.html) — that real click is
  // the user activation window.open needs. When the real page's sink fires, intercept.js → content.js →
  // PROBE_HIT lands on this marker and EXPLOIT_PROBE_STATUS reports REAL EXPLOIT (Chrome-confirmed).
  var _poc = buildPocFromShape(sinkName, poc, shape, session.pageUrl, marker, srcpath, gatefields);
  session.pocJs = (_poc && _poc.pocJs) || null;
  session.pocWhy = _poc && !_poc.pocJs ? _poc.why : null;   // @WHY when the source isn't client-deliverable
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

  // AST-learned PATH-PARAM examples persisted on the endpoint (they survive doc eviction, unlike the rich
  // per-document method schema resolveEndpointSchema otherwise reads). Surface them so the reviewer sees the
  // REAL learned values (e.g. orgId=acme-42, the logged-in surface the tool exists to learn) even after the
  // per-doc schema was evicted — a learned-but-invisible value defeats the point.
  if (ep && Array.isArray(ep.pathParams) && ep.pathParams.length) {
    parameters = parameters || {};
    for (const pp of ep.pathParams) {
      const cur = parameters[pp.name] || { name: pp.name, type: "string", location: "path", required: true, description: "AST-learned path segment" };
      const vals = (cur._astValidValues || []).slice();
      for (const val of (pp.values || [])) if (vals.indexOf(val) < 0) vals.push(val);
      cur._astValidValues = vals;
      cur._astValueSource = cur._astValueSource || "ast_forced_exec";
      if ((cur._exampleValue === undefined || cur._exampleValue === null) && vals.length) { cur._exampleValue = vals[0]; cur._exampleValueSource = "ast"; }
      parameters[pp.name] = cur;
    }
    if (source === "none") source = "ast";
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
