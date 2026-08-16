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

// ─── Engine dispatch ──────────────────────────────────────────────────────────
// self.astDispatch(msg) → Promise<response> is installed by the merged v2 host
// bridge at the bottom of THIS file (it drives the engine WASM). AST_* messages go
// straight to it — a direct call, not chrome.runtime.sendMessage (which would NOT
// reach a same-document listener; the sender's own context is excluded).
function sendToOffscreen(msg) {
  /* THE OTHER HALF OF THE EDGE bridge.js ALREADY ASSERTS FROM ITS SIDE (onFrontierAdvance). bridge.js is the
     last script ast-worker.html loads, so by the time any document delivers its CONTENT_HTML the dispatch is
     installed — its absence is a broken load order, not an optional feature. Returning a failure response
     instead reported "analysis worker dispatch unavailable" as this document's `_astError` and moved on,
     which is a page that silently never reached the frontier at all. */
  DCHECK(typeof self.astDispatch === "function",
         "the trusted zone has no astDispatch to hand this document to — it is the ONE entry to the host " +
         "WFQ pool, so without it the document never becomes work on the frontier and reports as unanalysed");
  return self.astDispatch(msg);
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
// Credentialed same-origin requests (schema.verify) still use the page-context relay (lib/schema.js).

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
    /* Reviewed = its forced-exec run produced results (merged to globalStore). The parked frontier (IDB
       recipes) carries any residue. There is no second condition: `_astResults`/`_astError` are written by
       the ONE analysis when its engine finalizes, so a document holding either is a document whose dispatch
       has already returned. The `!_analysisInflight.has(documentId)` that stood beside this asked a
       per-document in-flight REGISTER the same question its own result slots already answer, and that
       register was half of the second scheduler this file no longer has. */
    var reviewed = doc && (doc._astResults || doc._astError);
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
  /* NO scriptCache. It was a (analyzer fingerprint + origin + SHA-256 of the page HTML) → stored result map
     that the handoff below consulted BEFORE dispatching, and a hit replayed the stored result and returned without
     ever creating an engine — so a revisited page never resumed the flows it had parked. §NO BOUNDS bans a
     seen-set outright, and this one was keyed on document IDENTITY, which is exactly what may never stand in
     for emitted output as proof that a flow is finished. */
  /* NO discoveryChanges. It was service → [{timestamp, fetchUrl, changes}], written in exactly one place —
     the host-side discovery fetch, which diffed the document it had just pulled against the one it held. That
     fetch is the engine's now (engine/host/solver/discovery.c), so the map has no writer, and a store field
     nothing writes is a plausible datum: every reader of it would have reported "this API's surface has never
     changed" forever. The diff belongs where the documents now arrive, and it is not carried here in the
     meantime. */
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
      frameId: 0,               // Chrome routing ONLY (tabs.sendMessage / the page-context relay)
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
  /* THE WORK ITSELF IS STOPPED BY AST_CLEAR, WHICH RUNS FIRST (popup-handlers.js CLEAR_TAB): it tears down
     every live engine in the host pool, drops the documents waiting for a slot, and empties the cross-session
     frontier's IndexedDB. There is nothing left on this side to empty — the queue that used to be drained
     here was the second scheduler, and the replay cache that used to be cleared here was the seen-set. What
     survives is `_dataEpoch`, which is not a register of work: it invalidates the RESULT of any analysis that
     was already in the engine when the wipe happened, so it cannot repopulate what we just cleared. */
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
/* NO analysisOpts RE-APPLY AT BOOT. The record held two knobs — a yield throttle in milliseconds and an
   "analyzer workers" count — that this dispatch answered "unknown type" to and both senders swallowed, so
   neither had ever taken effect. Neither may be BUILT, either: the hot working set is bounded by ACTUAL
   RESIDENT WASM MEMORY (bridge.js's HOT_RAM_BUDGET), and a user-set instance count is precisely the fixed
   count that comment refuses ("a count would ignore that"); a wall-clock throttle on top of the cooperative
   quantum is a step cap wearing a settings label. The controls, the message types and the record are gone. */
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

/* THE LEARNING FETCH, BOUND TO ONE DOCUMENT. It binds the DOCUMENT (stable) so a page-context read hits the
   exact document's own origin/credentials; routed by documentId only.
   IT IS A GET FUNCTION AND CANNOT BE ANYTHING ELSE. It returned `(url, opts) => pageContextFetch(…opts…)`, so
   `opts.method` decided the verb and discovery passed `POST` with `X-Http-Method-Override: GET` — a request
   fired automatically by passive learning, which SECURITY.md §Network ("GET only") and CLAUDE.md §Attacker
   sources ("a state-mutating request is NEVER fired to learn") both forbid. There is now no parameter in which
   a caller could express a method: the second argument is HEADERS. */
function makePageGetFn(tabId, documentId = null) {
  return (url, headers) => pageContextGet(tabId, url, headers, documentId);
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

// (Discovery-doc probing is the ENGINE's: engine/host/solver/discovery.c seeds a probe FLOW per candidate
//  address on the one frontier. lib/discovery-probe.js, which held the host-side fetch loop and the
//  document diff, is deleted.)

// (Response-body protocol decoding -- handleResponseBody -- extracted to lib/response-decode.js, first.)

/* THE ONE HANDOFF from a delivered document to the ONE frontier — and it is now beside the CONTENT_HTML
   handler that writes the buffer and hands it over on the next line, instead of in a file of its own.
   `lib/analyze.js` held it and is DELETED. What went with it was not a step in the handoff; it was four
   mechanisms reading fields NO PRODUCER WRITES, each held together by a `||`:

     * PER-SCRIPT FINDING ATTRIBUTION (`_findScriptForLine`, the combined→per-script line shift). It read
       `sink.location`, `finding.taintPath` and `finding.sanitizerReport`. `solve.c`'s sink record carries
       sink/source/poc/firesOn/cspBlocks/trustedTypes/delivery and NO location, and `taintPath` /
       `sanitizerReport` appear NOWHERE in the engine — they were written only by the host that shifted them.
       So every sink took the `scriptOffsets[0] || {url:null}` fallback and was filed under the page URL: the
       machinery's entire observable effect was ONE page-keyed entry, which is exactly what merge.js's own
       security merge produces, and the per-script attribution it appeared to perform was fabricated. That is
       why the split is DELETED rather than moved to result.c: a location is a fact only the running engine
       holds, and when solve.c records one the attribution is the ENGINE's, never a host re-derivation from
       line offsets. `analysis._securityMerged` went too — it existed to pre-empt merge.js's merge in favour
       of this one, so removing it leaves ONE security merge instead of two.
     * `_markSecurityFindingChanges` — wrote `_changeType` and `_fixedCount`. Neither name has a reader.
     * THE SCRIPT CONCATENATION (`combined`/`scriptOffsets`/`scriptUrls`/`totalChars`/`_analyzeDiag`). The
       CONTENT_HTML handler sets `_buf.scripts = []` and nothing ever pushes to it — the engine sources every
       script itself (Lexbor parses the HTML; `qjs_run_doc_scripts` runs inline + external in document order).
       The concatenation therefore built `""`, `analysis.scriptOffsets` had no reader, and `_analyzeDiag`
       reported `n:0, hasCDN:false` for every document there has ever been — a diagnostic answering from a
       collection its producer stopped filling.
     * the `_timings` block — the bridge's result view emits no `_timings`, and neither `_lastAstTimings` nor
       `_analysisTimings` is read anywhere.

   §"A FIELD A CONSUMER DEFAULTS IS A FIELD NOBODY WILL NOTICE IS NEVER WRITTEN": every one of those was one
   `||` away from being a crash. Nothing here defaults an engine field any more — the two this function reads
   are DCHECKed for their shape, and the rest are asserted by `assertResultDocument` in bridge.js.

   NOT awaited by its caller and NOT wrapped in a catch: an assertion in here (or anywhere down the dispatch)
   is an invariant abort, and an unhandled rejection is the honest shape of it. A `.catch` that printed one
   debug line is what let the bridge's own contract checks land in a log nothing reads. */
async function _dispatchDocument(docKey) {
  var buf = _scriptBuffers.get(docKey);
  /* THE BUFFER IS THIS DOCUMENT'S RECORD AND THE HANDLER JUST WROTE IT. `if (!buf || !buf.pageHtml) return`
     stood here and made three different broken states — no buffer, a buffer for another document, and a
     document whose HTML never arrived — indistinguishable from a page legitimately having nothing to
     analyse, which is the one outcome that produces no symptom at all. content.js THROWS on an empty body
     rather than shipping one, so an empty pageHtml at this point is our own delivery path having lost it. */
  DCHECK(!!buf, "a document was handed to the analysis with no script buffer — the CONTENT_HTML handler " +
                "creates the buffer immediately before this call, so its absence is that record being lost " +
                "between the two lines");
  DCHECK(buf.docKey === docKey, "a script buffer is filed under a documentId that is not its own — every " +
                                "principal this analysis runs under (the SSRF origin, window.location, the " +
                                "credentialed-read origin) is read off this buffer, so a mis-filed one " +
                                "analyses a document under another document's identity");
  DCHECK(!!buf.pageHtml, "a document reached the analysis with no page HTML — content.js refuses to ship an " +
                         "empty body (it throws), so this is the bundle that was fetched being lost on the " +
                         "way here, and analysing nothing would report the page as clean");
  var tabId = buf.tabId;
  var tab = getDoc(docKey);
  var _ep = _dataEpoch;   // a Clear during the engine round-trip invalidates this run

  // This run's merge re-registers every AST-derived endpoint, so drop the previous round's first — a
  // re-delivered document would otherwise double-register them. (The `AST DYN ` prefix this also tested for
  // is a prefix OF `AST `, so it was one condition written twice.)
  var keysToDelete = [];
  tab.endpoints.forEach(function (val, key) { if (key.startsWith("AST ")) keysToDelete.push(key); });
  for (var di = 0; di < keysToDelete.length; di++) tab.endpoints.delete(keysToDelete[di]);

  // Source URL for the analysis. SECURITY: this becomes the analysis PRINCIPAL — safeFetch's
  // origin-relative SSRF origin (self.__sfPageOrigin in the worker) AND window.location. It MUST derive
  // only from the browser-provided sender.url (captured into buf.url on CONTENT_HTML), NEVER a
  // content-script-supplied value (msg.url -> scripts[0].url) — else a hostile page could claim a localhost
  // origin to defeat the SSRF guard. No untrusted fallback: unknown origin leaves tabUrl "" -> safeFetch's
  // safe default (block private) + a placeholder window.location. Per-DOCUMENT principal: buf.url is THIS
  // document's own browser-provided url, not the tab's — a sub-frame analyses as its own origin, never the
  // embedder's.
  var tabUrl = buf.url || buf.pageUrl || "";
  var response;
  try {
    response = await sendToOffscreen({
      type: "AST_ANALYZE", sourceUrl: tabUrl, documentId: docKey, origin: buf.origin,
      // THE AGENT CLUSTER THIS DOCUMENT BELONGS TO — SECURITY.md keys one WASM instance on
      // `(browsing-context group, origin)`, and BOTH halves have to be BROWSER-STATED because the untrusted
      // engine may state neither. `origin` above is the MessageSender principal (opaque-unique per document);
      // `groupId` is the tab, which is the browser-set fact closest to a browsing-context group — a tab is
      // exactly one top-level traversable and every navigable nested under it is in that traversable's group.
      // NOTE THE DISTINCTION FROM THE RULE THIS FILE OTHERWISE KEEPS: a tabId may never key a DOCUMENT (a tab
      // holds many, and a (tab,frame) pair is reused across navigations with a different origin). A GROUP is
      // not a document, and it is the only thing the tab id is used as here — the origin half is what keeps
      // two documents of one tab in two clusters when they are cross-origin. `frameId` distinguishes the
      // group's TOP document from a sub-frame, which the pool needs because a same-origin sub-frame is created
      // and run INSIDE its cluster's instance (§4.8.5's insertion steps) while a top document is not.
      groupId: buf.tabId, frameId: buf.frameId,
      // HTML §8.1.3.1's TOP-LEVEL CREATION URL — the browser-provided address of the top of this document's
      // navigable chain, captured on CONTENT_HTML from sender.tab.url. It is NOT sourceUrl: this document may
      // be a sub-frame, and §8.1.3.5 decides secure-context (and therefore which [SecureContext] members the
      // engine installs) from the TOP of the chain rather than from the frame's own address.
      topLevelUrl: buf.topLevelUrl || tabUrl,
      pageHtml: tab._pageHtml || null,
      responseHeaders: tab._responseHeaders || {},   // real CSP/Content-Type -> engine (header-CSP is the PRIMARY policy; meta-CSP is secondary)
      // Participate in the GLOBAL cross-session frontier: this engine's residue parks to IDB under RAM
      // pressure (resource-driven, host-side) and rehydrates by value order later. With headroom the page
      // runs to completion in one visit — nothing is lost to a clock; there is NO dispatch/step quantum.
      persist: true,
    });
  } catch (e) {
    /* AN INVARIANT ABORT IS NOT A DISPATCH FAILURE. Everything down this call — the bridge's result-document
       contract, the notice router's field counts, the merge's own checks — throws through here, and recording
       it as this document's `_astError` would let the zone carry on with a contract it has already proved
       broken. RETHROW_FATAL is what keeps the ONE assertion mechanism from being locally disabled. */
    RETHROW_FATAL(e);
    console.debug("[AST] dispatch failed for tab=%d: %s", tabId, e.message || e);
    tab._astError = "sendToOffscreen threw: " + (e.message || String(e));
    return;
  }
  if (!response || !response.success) {
    // The Clear button terminated the engine mid-analysis. Abort cleanly — do NOT fall back to a second,
    // degraded analysis, which would re-flood the freshly respawned engine right after a Clear and
    // repopulate the just-wiped store.
    if (response && response.error === "cleared") {
      console.debug("[AST] tab=%d aborted — engine cleared", tabId);
      return;
    }
    console.debug("[AST] analysis failed for tab=%d: %s", tabId, response ? response.error : "no response");
    if (response && response.stack) console.debug(response.stack);
    // SURFACE the failure — there is no fallback path. A page is reviewed as the COMBINATION of all its
    // scripts in one realm; emitting a degraded result would MASK the real failure. `_astError` + the
    // engine's @WHY/@E are the signal to root-cause; the resumable frontier retries via replay recipes.
    tab._astError = "offscreen unsuccessful: " + (response ? (response.error + " | " + (response.stack || "")) : "no response");
    return;
  }
  tab._astError = null;
  // The bin/Clear reset fired while this analysis was in the engine. Its result predates the wipe, so
  // merging it would repopulate the just-cleared store. Abandon the whole tail.
  if (_ep !== _dataEpoch) {
    console.debug("[AST] tab=%d result discarded — store reset mid-analysis", tabId);
    return;
  }
  var analysis = response.result;

  /* THE PAGE'S OWN UNCAUGHT ERRORS. A script that throws names an unbuilt capability, so this is a P1 the
     reviewer must SEE — it goes to the popup's diagnostic view (serialize.js reads `tab._resolverErrors`),
     never console-only. NOT `analysis.resolverErrors || []`: the bridge builds this array unconditionally
     from the engine's `pageErrors`, so its absence is that edge broken, not a page with nothing to say.
     There is no dedup set here either — `result_page_error` in result.c already refuses a message it is
     holding, so a second one on this side would be a host-side identity set standing over the producer's
     own answer. */
  DCHECK(Array.isArray(analysis.resolverErrors),
         "the engine result carried no resolverErrors array — bridge.js builds it from the engine's own " +
         "pageErrors on every result, so its absence is that relay broken and every error the engine " +
         "recorded while running this page is being dropped silently");
  if (analysis.resolverErrors.length) {
    if (!Array.isArray(tab._resolverErrors)) tab._resolverErrors = [];
    for (var _rei = 0; _rei < analysis.resolverErrors.length; _rei++) {
      var _re = analysis.resolverErrors[_rei];
      console.debug("[AST:page-error] %s: %s", _re.context, _re.message);
      tab._resolverErrors.push({ context: _re.context, message: _re.message, snippet: _re.snippet || null });
    }
  }

  DCHECK(Array.isArray(analysis.securitySinks) && Array.isArray(analysis.fetchCallSites),
         "the engine result reached the merge without its two finding arrays — every endpoint and every @S " +
         "record for this page travels in them, and merging a document that has neither reports the page as " +
         "analysed and clean");
  console.debug("[AST] tab=%d: %d fetchSites, %d secSinks, %d pageErrors", tabId,
    analysis.fetchCallSites.length, analysis.securitySinks.length, analysis.resolverErrors.length);

  /* MERGED UNCONDITIONALLY. A `hasFindings` gate stood here and RETURNED before all four lines below when the
     engine emitted no endpoint and no sink — so `tab._astResults` was never written, and `_evictReviewedDocs`
     reads exactly that slot to decide a document's run has returned. A page with nothing to find was therefore
     pinned in `state.docs` forever, holding its buffer, and the one case that produces no symptom is again the
     one that was broken. "The engine found nothing" is a RESULT and is recorded as one. */
  tab._astResults = [analysis];
  mergeASTResultsIntoVDD(tab, [analysis], tabId);
  mergeToGlobal(tab);
  notifyPopup(tabId);

  // Idle burst of the ONE host-level attention: advance other origins' parked frontiers by value
  // (non-blocking, serialized). This page's own residue (if it parked) is now in the global frontier.
  _driveGlobalFrontierBurst();
}

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
    /* THE FRAME'S OWN ID, BROWSER-SET, and it is on the buffer because the ANALYSIS needs it: bridge.js keys a
       WASM instance on the (browsing-context group, origin) agent cluster, and telling a SUB-FRAME (which its
       cluster's instance already runs as a realm of its own — §4.8.5 creates it inside that heap) from the
       group's TOP document (which it does not) is what frame 0 answers. This is not the "isTop" the comment
       above refuses to record: that would be a frame's claim ABOUT ITSELF, and `sender.frameId` comes from the
       browser process, the same provenance as `sender.tab.url`. */
    _buf.frameId = doc.frameId;
    _buf.docKey = _dk;
    _buf.origin = doc.origin;                                         // credentialed-read principal (per-document; = _senderOrigin(sender))
    // THIS DOCUMENT'S OWN ADDRESS, WHICH IS THE SSRF PRINCIPAL — and a sub-frame must never analyse under the
    // EMBEDDER's. This fell back to `sender.tab.url` when the frame's own address was missing, which is the
    // one substitution that inverts safeFetch's origin-relative rule: SECURITY.md allows a private target
    // "unless the page principal is itself private — a public page cannot use the extension's host
    // permissions to reach the user's localhost/intranet". A frame in a tab whose top document is
    // http://localhost/ would have inherited that PRIVATE classification and been allowed to fetch the user's
    // intranet on its behalf. sender.url is browser-set and present on every content-script message, so its
    // absence is a broken invariant, not a case to substitute a different document's address for — and ""
    // is what the chokepoint already treats as unknown (public, private targets blocked).
    DCHECK(!!doc.url, "a CONTENT_HTML arrived with no sender.url — the document's own address is the SSRF " +
                      "principal and the engine's window.location, and no other document's address may stand in for it");
    _buf.url = doc.url;                                               // SSRF origin + window.location (document's own url)
    // HTML §8.1.3.1's TOP-LEVEL CREATION URL for this document's environment — the address of the document at
    // the TOP of its navigable chain, which is the TAB's url and is browser-provided exactly like sender.url
    // (SECURITY.md already keys authorization on sender.tab.url for the same reason: a frame cannot state it
    // about itself). §8.1.3.5 reads it to decide whether the engine's realm for this document is a SECURE
    // CONTEXT, and Web IDL §3.3.13's members exist in that realm or do not by that answer — so a sub-frame that
    // answered from its OWN address would report an https frame inside an http page as secure, which is the
    // ancestral hole Secure Contexts §4.2 exists to close. A sender with NO TAB is not in one, so it is its own
    // top: that is the real answer for it, not a fallback for a value that went missing.
    _buf.topLevelUrl = (sender.tab && sender.tab.url) || _buf.url;
    _buf.pageHtml = doc._pageHtml;
    _buf.scripts = [];   // engine-sourced: Lexbor parses the HTML, qjs_run_doc_scripts runs inline + external scripts — one system
    _buf.pending = 0;
    _buf.loadFired = true;
    /* THIS DOCUMENT NOW BECOMES WORK ON THE ONE FRONTIER — it APPENDS its flows (boot fork + orphans) and
       does not start a run, a scheduler or an attention of its own. `lastActivatedTs` went with the queue
       that read it: it ordered documents by RECENCY, and recency is not value. The only order is the level-1
       WFQ in bridge.js, which ranks live engines by their best flow's weight (qjs_top_weight) and admits
       against the RAM working-set floor — a policy this zone cannot hold, because no instance can rank the
       others. */
    _dispatchDocument(_dk);   // straight to astDispatch -> the ONE host WFQ pool. No queue, no cache, no in-flight register.
    notifyPopup(tabId);
    return;
  }

  /* CONTENT_DOM handler removed: Lexbor in the engine worker parses
     the same CONTENT_HTML and exposes the spec DOM the bundle reads
     via document.querySelector / dataset / getAttribute. No
     parallel content-script DOM snapshot. */
}

// Popup messages — sender.tab is absent for popup contexts.
// (Popup command dispatch -- handlePopupMessage -- extracted to lib/popup-handlers.js; the brain wires
// the onMessage listener to it.)

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

// PERFORM the delivery the ENGINE declared. This layer owns MECHANISM — window.open, a real user gesture, the
// sandboxed attacker page — and owns NO vocabulary: which source is reached how is declared in C beside that
// source's other browser constraints (`concolic_declare_source` in the component that owns it), and arrives on
// the @S record as `delivery` + `deliveryPrefix`.
//
// WHAT THIS REPLACES, and why it could never have worked. The old builder matched
// `/\{(hash|search|pm|reply)\}/` against a `shape` field — a second, host-side statement of the attacker-source
// taxonomy, in a spelling the engine has never emitted (it says `location.hash`, `location.search`,
// `document.cookie`, `document.referrer`), against a field that is not on the record at all. So every finding
// this engine has ever produced failed at the first regex, and the taxonomy had a `pm` arm for a source no
// component declares. CLAUDE.md puts each source's intrinsic browser constraints in the engine — the WHATWG
// per-component encode sets, the fragment/query difference, the delivery prefix — so a second table of them
// here is a copy that drifts, and this one had already drifted into being non-functional.
//
// ENGINE AGREEMENT: the live PoC is the ENGINE's exact solved breakout input (its `poc` field) — the input a
// candidate-replay flow drove through the REAL page code+branches+filters to the sink where it broke out. We do
// NOT re-derive a payload (a second solver would diverge: no gate-guided prefix, no filter bypass, not the
// transform-surviving form the engine found). The ONLY edit is (a) mapping the engine's fire-marker X9 to the
// verifier's proof hook apiclientsink(marker) — intercept.js defines it and relays it (content.js → PROBE_HIT),
// the same correlation path that verifies any probe — and (b) placing it through the declared mechanism, so
// real Chrome, not the model, is ground truth. Disagreement is a precise engine-fidelity bug.
//
// Returns {pocJs, payload, context, source, delivery} — or pocJs:null with `why` when the declared mechanism is
// one this layer cannot PERFORM. That is an answer, not a failure: the finding stands (the engine fire-verified
// the breakout), and `why` names the mechanism rather than a missing field.
function buildLiveDelivery(sinkName, poc, source, delivery, deliveryPrefix, pageUrl, marker) {
  DCHECK(typeof poc === "string" && poc.length > 0,
         "an @S live-verify was started with no engine poc — solve_json_array emits `poc` on every fire-verified "
         + "entry and only on those, so a probe without one is being built for a PARKED search (sink=" + sinkName + ")");
  DCHECK(typeof marker === "string" && marker.length > 0,
         "an @S live-verify was started with no correlation marker — the marker rides INSIDE the payload as "
         + "apiclientsink('<id>') and is the only thing that ties a real Chrome hit back to this session");
  if (!pageUrl) return { pocJs: null, why: "no page url recorded for this finding — a live delivery navigates the page the sink was observed on" };
  var call = "apiclientsink('" + marker + "')";           // the verifier's proof hook (marker = crypto.randomUUID)
  // The engine's X9 is a bare fire-marker: `onerror=X9`, `javascript:X9`, `';X9();//`. Map every X9 (call or
  // ref form) to the apiclientsink CALL so the real sink, on firing, relays the proof. Its structure (breakout
  // + gate prefix + surviving transforms) is untouched.
  var payload = String(poc).split("X9()").join(call).split("X9").join(call);
  var context = "engine:" + (sinkName || "?");
  var out = { pocJs: null, payload: payload, context: context, source: source || null };
  if (!delivery) {
    // The engine declared no delivery for this source — which is itself the statement, not a missing field:
    // server-injected page state is written by the attacker directly and no component carries or transforms
    // it, so there is nothing for this layer to perform. Never guess a vector here.
    out.why = "the engine declares no browser delivery for source `" + (source || "?") + "` — nothing carries or "
            + "transforms these bytes on the way into the page, so there is no navigation this layer can perform. "
            + "The sink and its breakout are still fire-verified.";
    return out;
  }
  if (delivery === "address") {
    // The payload rides the VICTIM'S OWN address, at the component the engine's prefix names. A third component
    // would mean the engine grew an address source this layer has no placement for — a contract drift, not a
    // page state, so it asserts rather than degrading to one of the two it knows.
    DCHECK(deliveryPrefix === "#" || deliveryPrefix === "?",
           "the engine declared an `address` delivery whose component is neither `#` nor `?` (got "
           + JSON.stringify(deliveryPrefix) + ") — a URL has no third place an attacker-controlled component "
           + "lives, so this is either a declaration with no prefix or a component this layer cannot place");
    var frag = deliveryPrefix === "#";
    out.pocJs = "window.open(" + JSON.stringify(frag ? pageUrl.split("#")[0] + "#" + payload
                                                     : pageUrl.split("#")[0].split("?")[0] + "?" + payload) + ', "_blank");';
    out.delivery = "navigate the victim to a URL whose " + (frag ? "fragment" : "query string") + " is the payload";
    return out;
  }
  if (delivery === "plant") {
    // §S(b)'s two-stage PoC. Stage one writes the value on the VICTIM'S origin (a cookie), which an attacker
    // page cannot do from outside it — so the sandbox can perform stage two and never stage one.
    out.delivery = "two-stage: plant the value on the victim's origin, then load the page";
    out.why = "this source is a PLANTED one (§S(b) two-stage): the attacker must place the value on the victim's "
            + "own origin before the load that fires it, and a sandboxed attacker page has no way to perform that "
            + "first stage. The second stage is just loading the page.";
    return out;
  }
  if (delivery === "referring-address") {
    // The payload rides the ATTACKER's address, not the victim's, and Chrome's default referrer policy sends
    // only the origin cross-origin — so reproducing it needs an attacker-hosted page serving
    // `Referrer-Policy: unsafe-url`, which is not a page this extension can host.
    out.delivery = "the payload rides the address the victim arrives from";
    out.why = "this source is carried by the NAVIGATION, not by the victim's URL: the payload has to be part of "
            + "an address the attacker serves, and Chrome's default referrer policy strips everything but the "
            + "origin cross-origin — so reproducing it needs an attacker-hosted page sending "
            + "`Referrer-Policy: unsafe-url`, which the sandbox cannot be.";
    return out;
  }
  if (delivery === "user-file") {
    out.delivery = "the user hands the document an attacker-supplied file";
    out.why = "this source is a FILE the user gives the page. No navigation delivers it, so reproducing it means "
            + "opening the page and selecting the attacker's file by hand.";
    return out;
  }
  DFAIL("the engine declared a delivery mechanism this layer has no arm for: " + JSON.stringify(delivery)
        + " — the token vocabulary is solve.h's `delivery` field (address / plant / referring-address / "
        + "user-file), so "
        + "either a mechanism was added in C without its delivery arm here, or this record did not come from "
        + "solve_json_array");
  return out;
}
function startExploitProbe(msg) {
  _pruneProbeSessions();
  const { waitMs, findingId, sinkName, poc, source, delivery, deliveryPrefix } = msg || {};
  if (!poc) throw new Error("need the engine's poc (the fire-verified breakout input from the @S finding)");

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
  // ENGINE AGREEMENT: perform the delivery the ENGINE declared, carrying its EXACT poc (X9 -> apiclientsink).
  // The user runs it by clicking Run in the sandboxed attacker page (poc-sandbox.html) — that real click is
  // the user activation window.open needs. When the real page's sink fires, intercept.js → content.js →
  // PROBE_HIT lands on this marker and EXPLOIT_PROBE_STATUS reports REAL EXPLOIT (Chrome-confirmed).
  var _poc = buildLiveDelivery(sinkName, poc, source, delivery, deliveryPrefix, session.pageUrl, marker);
  session.pocJs = (_poc && _poc.pocJs) || null;
  session.pocWhy = _poc && !_poc.pocJs ? _poc.why : null;   // which declared mechanism this layer cannot perform
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
  // TWO PREDICATES, BECAUSE SECURITY.md STATES TWO RULES AND THEY ARE NOT THE SAME CHECK:
  // "SW→offscreen (`__evt`) is authenticated by `sender.url`: a service worker has no frame, so Chrome
  //  leaves its `MessageSender.origin` undefined (a browser quirk) and only `sender.url` carries the
  //  extension origin. Document→document hops (offscreen→SW in background.js, offscreen→popup) use
  //  `sender.origin === chrome-extension://<id>` (browser-set, and `"null"` for a sandboxed extension
  //  page → rejected)."
  // ONE url-prefix predicate answered BOTH questions here, which is the document→document rule DELETED:
  // a sandboxed extension page's sender.url is still `chrome-extension://<id>/…` while its ORIGIN is the
  // opaque `"null"`, so a url prefix hands an opaque-origin document the trusted popup command surface
  // (GET_STATE, SEND_REQUEST, CLEAR_TAB) — the exact impersonation SECURITY.md's attack table says is
  // mitigated. The popup itself already gates the reverse direction on sender.origin (popup.js); a
  // boundary checked on one side only is not a boundary.
  const fromExtUrl = !!(sender.url && sender.url.startsWith(EXTENSION_ORIGIN + "/"));   // SW OR extension document
  const fromExtDocument = sender.origin === EXTENSION_ORIGIN;                            // a real (non-opaque) extension document

  if (msg.__evt) {
    // The SW's own hop: it has no frame, so sender.origin is undefined and sender.url is the only
    // browser-set carrier of the extension origin.
    if (!fromExtUrl) return;
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
    // Defense in depth: no trusted extension context ever sends a CONTENT_TYPE. The URL predicate is the
    // BROADER of the two (it also covers a sandboxed extension page, whose origin is "null"), so it is the
    // right one to DROP on — dropping more here can only ever refuse.
    if (fromExtUrl) return;
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

  // Discriminate by sender.ORIGIN, NOT sender.tab: an action popup's sender.tab is
  // the ACTIVE tab (defined!), so a sender.tab check would wrongly drop popup
  // messages. This is the document→document hop, so it is the origin rule — an
  // opaque ("null") extension document is NOT this extension's document and gets
  // nothing. sender.url is deliberately NOT accepted here: the SW never sends a
  // popup command, so widening this to it would only admit the sandboxed page.
  if (!fromExtDocument) return;
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

// (State serialization for the popup -- mergeVirtualParts/serializeApiKeyEntry/mergedSecurityFindings/
// serializeTabData -- extracted to lib/serialize.js, loaded first.)
