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

// (Cross-tab/global merge -- _mergeDocInto/mergeToGlobal -- moved to lib/merge.js with the rest of merge.)

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

// (Response-body protocol decoding -- handleResponseBody -- extracted to lib/response-decode.js, first.)

// (Analysis orchestration -- script hashing/caching, _replayCachedAST, _analyzeCombinedScripts(Inner),
// _drainReviewQueue, source-map recovery -- extracted to lib/analyze.js, loaded first.)

// (Engine-result -> doc merge -- mergeASTResultsIntoVDD -- extracted to lib/merge.js, loaded first.)

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
// (Send-panel replay -- resolveEndpointSchema/coerceValue/executeSendRequest -- extracted to lib/send.js.)

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
