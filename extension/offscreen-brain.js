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
     last script ast-worker.html loads, so by the time any document delivers its CONTENT_SEED the dispatch is
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
// NOT per-document: it spans tabs and outlives any one document's analysis, and
// the popup's network view filters it rather than owning it.
// So the log is ONE global array; each entry carries {tabId,
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

/* THE LOG ENTRY IS A DISCRIMINATED UNION, AND ITS DISCRIMINATOR IS THE PRODUCER'S, NOT THE PAGE'S.
 *
 * Six creators push onto this one array — a WebSocket connection, a postMessage stream, a MessageChannel
 * stream, an EventSource stream, an HTTP round trip, and a form submission — and they do NOT write the same
 * fields, because they are not the same thing. A channel record has a `messages` array and no response body;
 * an HTTP record has a response body and no messages. That is a real union, and it was being read as a flat
 * record through `||`, which is the ONE form that makes a field's absence indistinguishable from its value.
 *
 * `kind` IS WHY THE UNION CAN BE ASSERTED AT ALL, and `method` could never have been. Every consumer used to
 * discriminate on `req.method === "WEBSOCKET"`, and on the HTTP arm `method` is `msg.method` — the verb the
 * PAGE handed its own `fetch()`. `fetch(u, { method: "WEBSOCKET" })` is a legal request, so the page could
 * spell the discriminator of a record it does not own: the popup would render its round trip as a socket card
 * and read a `messages` array that is not there. A discriminator a consumer branches on must be a fact the
 * PRODUCER states, so `_pushGlobalLog` takes it as an ARGUMENT — the creator that ran knows which it is, and
 * no message content can name it. `method` stays what it always was: the verb, reported, never routed on.
 *
 * ABSENCE IS A STATEMENT, WHICH IS WHY THE ARMS DIFFER RATHER THAN THE FIELDS BEING OPTIONAL. A WebSocket
 * record carries no `statusText` because there is no status line to have read one from — not because one went
 * missing — and the assertion says so per arm, so a creator that STOPS writing a field it owns crashes here
 * instead of rendering as a socket that never opened or a response with no reason phrase. */
const _LOG_KINDS = ["websocket", "postmessage", "msgchannel", "http"];
function _checkLogEntry(entry, kind) {
  DCHECK(_LOG_KINDS.indexOf(kind) >= 0,
         "a captured traffic record is being filed under a log kind nothing renders — the four arms are the " +
         "four record shapes the popup, the HAR export and lib/learn.js each branch on, and a fifth spelling " +
         "would reach every one of them as whichever arm their last `else` happens to be, got: " + kind);
  /* The shared spine: what every one of the six creators states about the traffic it captured, whatever
     transport carried it. `url` and `id` are non-empty because the popup keys its click-to-replay on the id
     and renders the url as the card's whole identity. */
  DCHECK(typeof entry.id === "string" && entry.id !== "" &&
         typeof entry.url === "string" && entry.url !== "" &&
         typeof entry.method === "string" && entry.method !== "" &&
         typeof entry.service === "string" &&
         typeof entry.timestamp === "number" && typeof entry.status === "number",
         "a captured traffic record reached the global log without the spine every creator writes " +
         "(id/url/method/service/timestamp/status) — the popup renders all six off every entry it lists, " +
         "and a hole in one is a card identifying a request nobody can find again, for kind " + kind);
  if (kind === "http") {
    /* THE RESPONSE HALF. `responseBody` is `string | null` and the null is load-bearing: a captured request
       whose response had no body says so, and every consumer that renders a body branches on it. */
    DCHECK(typeof entry.statusText === "string" &&
           (entry.responseBody === null || typeof entry.responseBody === "string") &&
           typeof entry.responseBase64 === "boolean" &&
           typeof entry.mimeType === "string" &&
           !!entry.responseHeaders && typeof entry.responseHeaders === "object",
           "a captured HTTP record reached the global log with an incomplete response half — the HAR export " +
           "writes status/statusText/mimeType/headers/body straight into a document a reviewer reads as the " +
           "server's own answer, so an absent one is exported as a server statement nothing observed");
    /* THE REQUEST HALF. An EventSource stream and a GET form both truthfully have `{}` headers, `""`
       content type and a `null` body — those are the request they made, not fillers for one nobody saw. */
    DCHECK(typeof entry.completedAt === "number" &&
           !!entry.requestHeaders && typeof entry.requestHeaders === "object" &&
           typeof entry.contentType === "string" &&
           (entry.rawBodyB64 === null || typeof entry.rawBodyB64 === "string"),
           "a captured HTTP record reached the global log with an incomplete request half — the Send panel " +
           "replays a request out of exactly these four fields, so a hole here is a replay that differs from " +
           "the captured request in a way the operator cannot see");
    /* THE BUNDLE FRAME THAT FIRED IT, and "" is "no call site was captured" — which is the honest answer for
       a form submission and for an EventSource message, neither of which a bundle function called. It is the
       oracle signal `testing/harness.js`'s netdiff reads to name the function forced execution failed to
       drive, so an ABSENT one there reads as a live request with no bundle behind it: exactly the state that
       diagnostic exists to distinguish, reported by the field being missing instead of by the run. */
    DCHECK(typeof entry.callStack === "string",
           "a captured HTTP record reached the global log with no call-site field — netdiff names the bundle " +
           "function that fired a [live]-only endpoint out of it, and an absent one is that endpoint " +
           "reported as having no call site rather than one nobody captured");
  } else {
    /* A CHANNEL RECORD IS ONE CONNECTION AND ITS MESSAGES, appended to in place for the connection's whole
       life — so `messages` must be a real array from the moment the record is filed, never a hole a later
       push would have to create. */
    DCHECK(typeof entry.channelId === "string" && entry.channelId !== "" && Array.isArray(entry.messages),
           "a captured channel record reached the global log without the channel identity its own later " +
           "frames are matched by — every WS_SEND / PM_RECV / MC_RECV finds its record by channelId, so an " +
           "absent one opens a second record per frame and the console shows a channel with one message");
    if (kind === "websocket") {
      DCHECK(typeof entry.wsOpen === "boolean",
             "a captured WebSocket record reached the global log without its open state — the request-log " +
             "card renders OPEN/CLOSED off it, and absence would render every socket as closed");
    } else {
      /* Both halves are the RENDERER'S claim (lib/popup-reqlog.js says so where it prints them) and the
         empty string is that renderer having stated no origin — a fact, and the one the reply path refuses
         to address a postMessage to. It is not a missing value, so it is asserted as a string, not filled. */
      DCHECK(typeof entry.sourceOrigin === "string" && typeof entry.targetOrigin === "string",
             "a captured " + kind + " record reached the global log without both origin halves — the popup " +
             "prints the pair as the page's own claim and the console refuses to reply to a channel whose " +
             "source origin is not a real one, so an absent half is a refusal that never happens");
    }
  }
}

// Push a captured traffic entry onto the global log, stamping the {tabId,
// documentId, frameId} filter metadata from the SENDER (the browser's answer for
// THIS message, never a lookup that could name another document). Newest first;
// oldest trimmed at the cap.
function _pushGlobalLog(entry, kind, tabId, documentId, frameId) {
  _checkLogEntry(entry, kind);
  entry.kind = kind;
  entry.tabId = tabId != null ? tabId : null;
  /* THE ENTRY'S DOCUMENT IDENTITY IS ASSERTED, NOT DEFAULTED. `documentId || null` stood here, and every one
     of this function's eight call sites is downstream of handleContentMessage's fail-closed check, which
     already refuses a content message carrying no sender.documentId. So the default could only ever have
     fired for a caller that bypassed that check — and the value it would have written is the one thing this
     entry must not carry: lib/serialize.js slices the per-document requestLog with
     `r.documentId === tab.documentId`, so a null here silently files a real captured request under no
     document, where the page that made it can never show it again. */
  DCHECK(typeof documentId === "string" && documentId.length > 0,
         "a captured request is being filed on the global log with no documentId — it is the ONLY key the " +
         "per-document request log is sliced by, and handleContentMessage refuses a content message without " +
         "one, so a caller reaching here without it has bypassed that gate and this entry would belong to " +
         "no page");
  entry.documentId = documentId;
  if (entry.frameId == null) entry.frameId = frameId != null ? frameId : 0;
  globalRequestLog.unshift(entry);
  while (globalRequestLog.length > MAX_REQUEST_LOG_ENTRIES) globalRequestLog.pop();
}

const _wsConnState = new Map(); // documentId → Map<wsId, { url, readyState }>

/* THE REVIEW-COMPLETION RECLAIM IS DELETED, AND WITH IT THE LAST BYTES THIS ZONE HELD OF A DOCUMENT.
   It existed for ONE thing - "the eviction was aimed at the BIG state (the combined page source), and that is
   the only thing reclaimed here" - and this zone no longer holds a page source at all: a seed is an ADDRESS,
   the custom browser loads the document itself through the one chokepoint, and the bytes live in that
   instance (`eng.html`, which the park writes to the cold tier) and are freed with it. A sweep that frees
   nothing is not harmless upkeep, it is a mechanism whose reason has gone, and one earlier version of it
   swept `state.docs.delete(documentId)` for a document that was still OPEN and still firing requests - so
   every key, endpoint, schema and probe result learned from live traffic was written into a throwaway object
   for the entire lifetime of the page, with nothing to say so. What is left is small and correct: a DocData
   is identity plus this document's own view (already merged into globalStore) and lives exactly as long as
   `_docOrigins`' entry for the same document does - the offscreen's session - and a `_scriptBuffers` entry
   is now the same size and the same lifetime. */

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

/* THE BROWSER-STATED FACTS OF ONE DOCUMENT, MINTED IN ONE PLACE OUT OF THE ONE OBJECT THAT CARRIES THEM.
   Every field here is an answer `chrome.runtime` gave and that NO WASM instance can ask for: SECURITY.md keys
   authorization on `sender.tab.url` and fixes the credentialed-read principal at `MessageSender.origin`
   precisely because the browser stamps them and a renderer cannot forge one. When safeFetch's SOP/CORS moves
   into a trusted instance, this record is what crosses to it; the policy is an ALGORITHM and may move, these
   are FACTS and may not. (A C counterpart to this record was written for an `engine/host/browser_process/`
   linked as its own wasm artifact, and that is deleted: a separate link is a separate FILE, not a separate
   process — the objects are shared, both Modules instantiate in this realm, and the host holds an exported
   HEAPU8 over each. The record below is the whole of the mechanism today and is not waiting on a C twin.)

   IT TAKES A `MessageSender` AND NOTHING ELSE, which is the whole mechanism. A caller cannot state one of
   these values without holding the object the browser filled in, so the failure this exists to prevent —
   deriving a principal from an address — has no argument to travel in. That failure is not hypothetical:
   SECURITY.md records a `scripts[0].url` fallback that WAS the analysis principal for a while, and bridge.js
   carries a standing @security-finding against the obvious "fix" of passing `originOf(msg.sourceUrl)` to the
   chokepoint. A sandboxed frame reports an ordinary-looking address and an OPAQUE origin, so the address
   fabricates a tuple origin the browser refused to give that document — and grants it same-origin access to
   the embedder's authenticated bytes.

   `stated` IS THE BRAND, and it is read by `_statedFacts` below at every consumer. JS has no types to make
   `{origin: originOf(url)}` unrepresentable, so the consumer asserts that what it was handed came from HERE.
   It catches the accident, which is the failure mode inside a trusted zone; it is not an anti-forgery device,
   and this comment does not claim to be one. */
const _BROWSER_STATED = "MessageSender";
function _browserFacts(sender) {
  DCHECK(!!sender && typeof sender === "object" && !!sender.tab,
         "browser facts were asked for out of something that is not a content-script MessageSender — every " +
         "field below is a browser answer, and a record built from anything else is this zone inventing them");
  DCHECK(!!sender.documentId,
         "a content message reached the fact mint with no documentId — it is the only stable per-document " +
         "identity (a (tab, frame) pair is reused across navigations at a DIFFERENT origin), so it is the key " +
         "the principal is remembered by and the name the engine pool answers \"who holds this document?\" by");
  DCHECK(typeof sender.url === "string" && sender.url !== "",
         "a content message reached the fact mint with no sender.url — the document's OWN address is the " +
         "private-network principal and the engine's window.location, and no other document's address may " +
         "stand in for it: a frame in a tab whose top is http://localhost/ would inherit that PRIVATE " +
         "classification and be allowed to reach the user's intranet on its behalf");
  DCHECK(typeof sender.tab.url === "string" && sender.tab.url !== "",
         "a content message reached the fact mint with no sender.tab.url — HTML §8.1.3.1's TOP-LEVEL " +
         "CREATION URL decides §8.1.3.5's secure context, and the extension holds <all_urls>, so the browser " +
         "always states it and its absence is a broken invariant rather than a value to substitute for");
  const facts = {
    stated: _BROWSER_STATED,
    documentId: sender.documentId,            // the identity every other fact is keyed by
    origin: _senderOrigin(sender),            // MessageSender.origin, opaque-unique — the credentialed-read principal
    url: sender.url,                          // this document's OWN address — the private-network principal
    topLevelUrl: sender.tab.url,              // HTML §8.1.3.1's top-level creation URL — SECURITY.md's authorization key
    tabId: sender.tab.id,                     // the browsing-context group half of the agent-cluster key
    frameId: sender.frameId || 0,             // browser-set; 0 IS the top-level traversable, never a frame's own claim
  };
  DCHECK(typeof facts.origin === "string" && facts.origin !== "",
         "the principal mint answered an empty origin — _senderOrigin returns the browser's tuple origin or a " +
         "per-document opaque token, and an empty one is neither: it would compare same-origin with every " +
         "other document that also failed to state one");
  DCHECK(facts.tabId != null,
         "a content message named no tab — the tab id is the browsing-context group half of the (group, " +
         "origin) agent cluster one WASM instance IS, and a document with no group shares one heap and one " +
         "principal with every document of its origin in every tab the user has open");
  return Object.freeze(facts);
}
/* THE READER, so a consumer never defaults one of these into existence. CLAUDE.md: a value the producer can
   legitimately omit is a POSITIVE statement the consumer reads as one — and none of these can be omitted, so
   the honest read is an assertion rather than an `||`. CHECK and not DCHECK: this is the authorization
   boundary itself, and continuing in release with a principal this zone did not mint is worse than aborting. */
function _statedFacts(facts) {
  CHECK(!!facts && facts.stated === _BROWSER_STATED,
        "a record that did not come from _browserFacts was read as this document's browser-stated facts — " +
        "the principal, the address and the group are chrome.* answers, and a value assembled anywhere else " +
        "is this zone deciding an origin instead of being told one");
  return facts;
}

/* THE ORIGIN OF AN ADDRESS, AND IT IS NOT A PRINCIPAL. `_browserFacts.origin` is the credentialed-read
   principal and comes from `MessageSender.origin`, which is where every authorization decision in this zone
   reads it from — a sandboxed document has a perfectly ordinary URL and an OPAQUE origin, so parsing its
   address would fabricate a tuple origin the browser refused to give it, and SECURITY.md records that exact
   substitution as a hole that was removed. This function answers a different and much smaller question: do
   two ADDRESSES name the same origin. Its one caller compares a suggested address against the address the
   browser reported for the same document, to establish that the suggestion is a route of THAT document and
   not a new destination — a comparison between two URLs, decided by URL rules, authorizing nothing.
   `""` for an address that will not parse, which its caller reads as a refusal rather than as a match. */
function _originOfUrl(u) { try { return new URL(u).origin; } catch (_) { return ""; } }

// Prioritization (recency + grind focus) is NOT driven by browser navigation or
// tab events anymore — those operate at the TAB level and can only guess which
// frame is the "main" document. It is driven by each document's own CONTENT_SEED
// message (see handleContentMessage): the message arrival IS the "analyze me now"
// signal, per document, with the document's authoritative own url/origin.

// Per-DOCUMENT dispatch record, keyed by documentId (sender.documentId ONLY — no
// tabId:frameId fallback; a doc-less content message fails closed). A tab holds many documents/frames (iframes,
// navigations), each its OWN origin; keying by tab merged them into one buffer
// and collapsed the credentialed-read principal. Each entry is exactly TWO fields:
// `{ facts, seedUrl }` — the browser-minted fact record this document is analysed under, and the ADDRESS the
// ambient observer suggested. It holds NO BYTES: one CONTENT_SEED per document creates/refreshes it, the
// custom browser LOADS that address through the one chokepoint, and the engine sources every script from what
// it loaded (Lexbor parses the response, qjs_run_doc_scripts runs each <script> in document order — inline
// directly, external <script src> via __feLoadScript).
const _scriptBuffers = new Map(); // documentId → { facts, seedUrl }

// Global persistent store — survives tab closes and SW restarts
const globalStore = {
  apiKeys: new Map(), // key → { origin, referer, firstSeen, ... }
  endpoints: new Map(), // endpointKey → endpoint data
  discoveryDocs: new Map(), // service → { status, url, apiKey, fetchedAt, doc }  (see lib/merge.js: no `method`)
  probeResults: new Map(), // endpointKey → probe result
  scopes: new Map(), // service → string[]
  securityFindings: new Map(), // sourceUrl → { sourceUrl, pageUrl, securitySinks[] }
  /* NO scriptCache. It was a (analyzer fingerprint + origin + SHA-256 of the page HTML) → stored result map
     that the handoff below consulted BEFORE dispatching, and a hit replayed the stored result and returned without
     ever creating an engine — so a revisited page never resumed the flows it had parked. §NO BOUNDS bans a
     seen-set outright, and this one was keyed on document IDENTITY, which is exactly what may never stand in
     for emitted output as proof that a flow is finished. */
  /* service → [{timestamp, fetchUrl, changes}]. Its ONE writer is the discovery fetch
     (lib/discovery-probe.js `fetchDiscoveryForService`), which diffs the document it has just pulled against
     the one it held, so every entry records a real change in a published API's surface. It was deleted here
     when that fetch left for the engine, on the correct ground that a store field nothing writes reads as
     "this API's surface has never changed" forever — the writer is back, so the field is. */
  discoveryChanges: new Map(),
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
      origin: "",               // MessageSender principal (set at CONTENT_SEED) — NEVER url-derived
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

/* THE BIN, ON THE PER-DOCUMENT VIEWS: empty every LEARNED map and drop every analysis slot, and KEEP the
   browser-stated identity. `state.docs.clear()` stood in the CLEAR_TAB handler, and with registration now
   load-bearing that would delete the identity of documents that are still open and still sending traffic — the
   next request from one of them would then reach `_docForLearning` unregistered, which is a DCHECK, reporting
   the user's own Clear as a broken invariant. Identity is not learned DATA: it is what the browser said about a
   document that exists, and Clear does not un-say it. What Clear must remove is everything we inferred, and
   that is exactly the list below (the `delete`s rather than `= null` matter: lib/serialize.js distinguishes an
   ABSENT `_resolverErrors` — "the engine recorded nothing" — from a present one of the wrong shape, which it
   asserts on). */
function _clearDocLearning() {
  state.docs.forEach(function (doc) {
    doc.apiKeys = new Map();
    doc.endpoints = new Map();
    doc.authContext = null;
    doc.discoveryDocs = new Map();
    doc.probeResults = new Map();
    doc.scopes = new Map();
    doc._valueIndex = createValueIndex();
    /* The page-source record goes with what it describes. Clear bins what was inferred AND what was loaded;
       leaving "delivered" behind would have the popup state that this document's bundle was loaded while the
       run it names is gone, which is the one thing this field exists to stop being said wrongly. There is no
       `_pageHtml`/`_responseHeaders` beside it any more: this zone holds no document bytes and no response of
       its own — the custom browser loads the document through the one chokepoint and holds both. */
    delete doc._pageSource;
    delete doc._astResults;
    delete doc._astRun;
    delete doc._astError;
    delete doc._securityFindings;
    delete doc._resolverErrors;
    delete doc._epNorm;
  });
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

/* A transient (unstored) empty DocData — A READ-ONLY OVERLAY BASE, NEVER A PLACE TO LEARN INTO. Two callers:
   GET_STATE, so the popup opened over a tab with no analysed document still shows the GLOBAL cumulative moat
   (serializeTabData overlays globalStore onto it), and the frontier merge, which fills one and hands it
   straight to mergeToGlobal in the same statement. Anything that WRITES to one and does not itself merge it
   is writing into an object that is discarded on return — which is what `_docForLearning` used to hand every
   learner in the extension. NEVER stored in state.docs. */
function _emptyDocView() {
  return {
    documentId: null, tabId: null, frameId: 0, origin: "", url: "", title: "", closed: false,
    apiKeys: new Map(), endpoints: new Map(), authContext: null, discoveryDocs: new Map(),
    probeResults: new Map(), scopes: new Map(), _valueIndex: createValueIndex(),
  };
}

/* Merge ONE engine advance — a mid-run snapshot of a live document, or the whole output of an engine with no
   live caller (a child navigable the engine announced, a rehydrated cold recipe) — into the moat AND into the
   documents it belongs to.

   THE DOCUMENTS ARE NAMED BY THE CALLER AND ARE NOT DERIVED FROM THE URL. This took a sourceUrl alone, so it
   had nothing to merge into and built an `_emptyDocView()` — whose own comment says "anything that WRITES to
   one and does not itself merge it is writing into an object that is discarded on return". It DID merge it,
   to globalStore, and that is exactly half the job: the cumulative moat learned every incremental endpoint and
   the DOCUMENT that learned it never did. Measured on a mirrored vuejs.org: globalStore held
   `GET media.bitterbrains.com/banners` while the document reporting on that same page carried an empty
   endpoints map and an empty `_astResults`, so the per-page view a user reads said the page was clean.
   `documentId` is the ONLY document key in this zone (a tab holds many documents; a (tab,frame) pair is
   reused across navigations at a DIFFERENT origin), so the name is carried across the seam rather than
   re-derived from an address that names no document.
   AN EMPTY LIST IS A POSITIVE STATEMENT: this engine has no live caller, its findings are the moat's alone,
   and the transient view is then the right target because there is no document to be wrong about. */
function _mergeFrontierResult(sourceUrl, documentIds, result, epoch) {
  // The engine↔JS contract guarantees a result OBJECT (never null) — a null here is a should-never-happen
  // in the engine's own output, not a case to `if(!result)return`-past.
  DCHECK(result && typeof result === "object", "_mergeFrontierResult: engine produced a non-object frontier result");
  if (!result) return;   // release: the DCHECK is stripped, tolerate the impossible-in-dev case (don't crash the user)
  DCHECK(Array.isArray(documentIds),
         "_mergeFrontierResult was not told which documents this advance belongs to — the empty list is how " +
         "an engine with no live caller says so, and an absent one is the bridge's half of this edge broken: " +
         "every finding would merge to the cumulative moat and no document would report having learned it");
  /* WHAT THIS RECORD IS. `_run` rides every analysis bridge.js builds, and a merge that could not tell a
     mid-run snapshot from a finished run from a crashed one could not label what it stores either. */
  DCHECK(result._run === "partial" || result._run === "complete" || result._run === "crashed",
         "an engine advance reached the merge without a run outcome (`" + result._run + "`) — bridge.js writes " +
         "`_run` on every analysis it builds, so its absence is that seam broken and a crashed run's findings " +
         "would be stored as a completed page's");
  /* WHICH STORE THIS ADVANCE WAS OBSERVED AGAINST. bridge.js stamps the wipe generation on the instance at
     `engineReserve`, before its first await, so this is the generation the engine was RUNNING under — not the
     one in force when the merge happens to land, which is always the current one and therefore always agrees.
     THE TERMINAL PATH HAS ASKED THIS SINCE IT EXISTED AND THIS ONE HAD NOTHING. `_dispatchDocument` captures
     `_dataEpoch` before it dispatches and abandons its whole tail if it moved; the incremental merge — every
     750 ms snapshot, and the finalize of an instance with no live caller — asked nothing, so an advance that
     crossed a Clear repopulated the store the user had just emptied. It reached globalStore alone while this
     function had no documents to name; it reaches a DOCUMENT too now, which is why the guard is here rather
     than pending.
     ASSERTED, NEVER DEFAULTED: an absent generation cannot be read as "current" — that is the exact shape that
     turns a producer which stopped stamping it into a merge that always passes. */
  DCHECK(typeof epoch === "number",
         "an engine advance reached the merge without the wipe generation it was observed under (`" + epoch +
         "`) — bridge.js stamps `_epoch` on every instance it reserves, so its absence is that seam broken and " +
         "findings observed before a Clear would repopulate the store the user just emptied");
  if (epoch !== _dataEpoch) {
    // Not a failure and not a swallow: the observation PREDATES the wipe, and the Clear button's whole
    // contract is that nothing observed before it survives it. Said out loud, like the terminal path's.
    console.debug("[frontier] advance discarded — store reset mid-run (epoch %s, now %s)", epoch, _dataEpoch);
    return;
  }
  try {
    // Merge on EITHER surface: an XSS-only page carries verified @S PoCs with no endpoints. Gating on
    // fetchCallSites alone dropped every incremental sink (they only surface here now, not just at teardown).
    /* NOT `result.fetchCallSites && …`: the two finding arrays are PRESENT-OR-ABSENT on a bridge.js analysis
       and their presence IS the statement that an @RESULT document arrived, so the `&&` was reading a
       three-state fact as a two-state one — and what it produced when the producer stopped writing an array
       was a quiet return, i.e. a page reported as having learned nothing.
       AN ADVANCE WITH NO DOCUMENT AND AN ADVANCE THAT LEARNED NOTHING ARE DIFFERENT, and only the second of
       them is an observation of a page. A cold/child instance that ABORTED reaches here with no document at
       all (bridge.js's crash arm carries none), and merging that as an empty advance is precisely how a run
       that died came to be recorded as a page with no API surface. Said out loud, like the epoch path above,
       rather than returned through a `.length` that cannot tell the two apart. */
    if (!analysisHasDocument(result)) {
      console.debug("[frontier] advance discarded — the run produced no engine document (_run=%s)", result._run);
      return;
    }
    if (!result.fetchCallSites.length && !result.securitySinks.length) return;   // legitimately empty (nothing learned yet) — not an invariant break
    result.sourceUrl = sourceUrl || result.sourceUrl || "";
    if (!documentIds.length) {
      var view = _emptyDocView();
      view.url = sourceUrl || ""; view.tabUrl = sourceUrl || "";
      mergeASTResultsIntoVDD(view, [result]);
      mergeToGlobal(view);
      return;
    }
    for (var _di = 0; _di < documentIds.length; _di++) {
      /* REGISTERED BEFORE THE ENGINE WAS SEATED. `_dispatchDocument` calls getDoc off the browser's own facts
         and only then dispatches, and a reclaimed document keeps its entry (only its page source is freed),
         so a miss here is that registration broken — not a document that legitimately went away — and
         merging into a view nobody holds is what this function was doing wrong in the first place. */
      var doc = state.docs.get(documentIds[_di]);
      DCHECK(!!doc,
             "an engine advance names a document this zone has no record of (" + documentIds[_di] + ") — the " +
             "analysis is dispatched only after the document is registered, so its findings would have " +
             "nowhere to go and the page would report as clean");
      if (!doc) continue;   // release
      /* THE SNAPSHOT THE DOCUMENT REPORTS. Each partial carries the run's GROWING accumulated findings, so
         the newest one supersedes the previous rather than adding to it — `_astResults` holds the most
         complete snapshot this run has produced, which is what makes it a real answer for a run that later
         dies. The terminal record only replaces it when it carries a document of its own. */
      doc._astResults = [result];
      mergeASTResultsIntoVDD(doc, [result]);
      mergeToGlobal(doc);
    }
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
/* THE WIPE GENERATION, ASKED BY THE ZONE THAT PRODUCES THE ADVANCES. bridge.js reads it ONCE per instance (at
   engineReserve, before the first await) and hands it back on every advance that instance produces, which is
   what lets this side tell an observation made before a Clear from one made after. It is a FUNCTION and not a
   copied number for the same reason `onFrontierAdvance` is: a number would be read at load and be wrong from
   the first Clear onwards, with nothing to say so. */
self.frontierEpoch = function () { return _dataEpoch; };
self.onFrontierAdvance = function (sourceUrl, documentIds, result, epoch) {
  // Do NOT wrap in a swallowing catch — that would silence the _mergeFrontierResult DCHECK (a broken engine↔JS
  // contract must crash LOUD in dev). notifyPopup is a UI side-effect whose own failure is non-fatal, isolated.
  _mergeFrontierResult(sourceUrl, documentIds, result, epoch);
  /* THE POPUP IS TOLD WHICH DOCUMENT MOVED. `notifyPopup(null)` broadcast a tab-less update for advances that
     belong to real documents, and the popup's own listener re-loads on any update, so this is not a fix for a
     missed render — it is the same rule as above one layer out: a message about a document that does not name
     it is a message nothing can attribute. A cold recipe genuinely has none, and null says so. */
  var _tabId = null;
  for (var _i = 0; _i < documentIds.length && _tabId === null; _i++) {
    var _d = state.docs.get(documentIds[_i]);
    if (_d) _tabId = _d.tabId;
  }
  try { notifyPopup(_tabId); } catch (e) { if (self.APICLIENT_DEV) throw e; }
};
function _driveGlobalFrontierBurst() {   // idle nudge: kick the ONE pool to re-check the cold frontier
  if (typeof self.kickHostPool !== "function") return;   // optional edge (bridge may not be up yet) — an ABSENCE, not an error
  try { self.kickHostPool(); } catch (e) { if (self.APICLIENT_DEV) throw e; }   // a THROW from the pool is a real bug -> dev-loud
}

/* THE DocData A LEARNER WRITES INTO, AND IT IS ALWAYS A REGISTERED ONE. Every learning path in the extension
   (lib/learn.js, lib/keys.js, lib/response-decode.js, lib/discovery-probe.js, lib/send.js) takes a documentId
   and resolves it HERE, several of them more than once per message — so an answer that is not the one stored
   object is not merely a lost write, it is several DIFFERENT objects for one message, one of which the caller
   happens to merge and the rest of which vanish.
   `|| _emptyDocView()` stood at the end of this line and made that the normal case for every document whose
   analysis had returned. It is a DCHECK now: a learner is reached from a message a LIVE document sent, and
   `handleContentMessage` registers the document off the browser's own facts before any of them runs, so an
   unregistered one here is that registration broken and not a state to carry on from. */
function _docForLearning(documentId) {
  var doc = documentId ? state.docs.get(documentId) : null;
  DCHECK(!!documentId,
         "a learner was handed no documentId — it is the only stable per-document identity, and without it " +
         "there is no view to write into and no principal to fetch as");
  DCHECK(!!doc,
         "a learner ran against a document that is not registered (" + documentId + ") — handleContentMessage " +
         "registers every document that sends a message, so everything this call is about to learn (keys, " +
         "endpoints, schemas, probe results) has nowhere to land and the page-context relay has no tab to " +
         "route to");
  DCHECK(!doc || doc.tabId != null,
         "a registered document carries no tabId — the browser states it on every content message and the " +
         "page-context relay (req2proto probes, discovery fetches) routes by it, so a null one is every " +
         "active-discovery request failing with relay_failed");
  return doc;
}

// Find a captured channel entry (WS/PM/MC) in the GLOBAL log. The popup
// message console routes by tabId, so scope the match to that tab; channelId
// is per-document (page-generated), hence not globally unique on its own.
/* KEYED ON `kind`, NOT ON `method`, for the reason `kind` exists. A channel record's `method` is the literal
   its creator wrote, but an HTTP record's is the verb the PAGE handed `fetch()` — so matching on "WEBSOCKET"
   could return a plain round trip, and the console would then push a frame onto a `messages` array that
   record does not have. */
function _findLogEntry(tabId, channelId, kind) {
  return globalRequestLog.find(function (r) {
    return r.channelId === channelId && r.kind === kind && r.tabId === tabId;
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
  /* `discoveryChanges` IS CLEARED HERE TOO, AND IT NEVER WAS BEFORE. It is serialized into the persisted
     store beside the six above, so a Clear that left it behind would restore a service's recorded API-surface
     history into a store the user had just emptied — the Clear button's whole contract is that no data
     survives it. */
  globalStore.discoveryChanges.clear();
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

// (VDD passive learning -- learnFromAstCallSite/learnFromRequest/learnFromResponse + stats + templated-
// method matching -- extracted to lib/learn.js, loaded first. One problem per file.)

/* TWO DOCUMENT-BOUND RELAYS, ONE PER OPERATION. Both bind the DOCUMENT (stable) so a page-context read hits
   the exact document's own origin/credentials, routed by documentId only — a page-context request is the
   ANALYZER acting AS the page, so the page's own jar and origin are the whole point of the edge.

   THE DOCUMENT FETCH names no method because the candidate it walks has no field for one: an API's published
   description is read with a GET of a published address (lib/discovery-probe.js `fetchDiscoveryForService`
   over lib/discovery.js `buildDiscoveryUrls`).

   THE ERROR PROBE names POST because the probe IS a POST. lib/req2proto.js sends a deliberately malformed
   body to a Google API and reads the `google.rpc.Status` rejection describing the request the service wanted;
   `sendProbe` writes `method: "POST"` at the call site. A rejected malformed body mutates nothing, and there
   was for a while a rule here saying no caller could express that verb at all — it is deleted, because it did
   not stop a state change, it stopped a measurement. */
function makePageGetFn(tabId, documentId = null) {
  return (url, headers) => pageContextGet(tabId, url, headers, documentId);
}

function makePageFetchFn(tabId, documentId = null) {
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

/* THE ONE HANDOFF from a seeded document to the ONE frontier — and it is now beside the CONTENT_SEED
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
       CONTENT_HTML handler set `_buf.scripts = []` and nothing ever pushed to it (that handler is now
       CONTENT_SEED and holds no bytes at all) — the engine sources every
       script itself (Lexbor parses the HTML; `qjs_run_doc_scripts` runs inline + external in document order).
       That empty array is gone too, with `_buf.pending` and `_buf.loadFired` beside it: a writer whose reader
       was deleted is the same broken contract as a reader whose writer was, and this sentence would otherwise
       be describing a line that no longer exists.
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
  /* THE BUFFER IS THIS DOCUMENT'S RECORD AND THE HANDLER JUST WROTE IT. A guarded early return stood here and
     made three different broken states — no buffer, a buffer for another document, and a record whose seed
     never arrived — indistinguishable from a page legitimately having nothing to analyse, which is the one
     outcome that produces no symptom at all. */
  DCHECK(!!buf, "a document was handed to the analysis with no dispatch record — the CONTENT_SEED handler " +
                "creates it immediately before this call, so its absence is that record being lost " +
                "between the two lines");
  /* THE FACTS THIS DOCUMENT IS ANALYSED UNDER, AND THE ASSERTION THAT THEY ARE THE BROWSER'S. Everything the
     analysis is authorized by — the private-network principal, window.location, the credentialed-read origin,
     the agent-cluster key — is read out of this one record, so a record this zone did not mint from a
     MessageSender is the whole analysis running under an identity nobody was told. */
  var facts = _statedFacts(buf.facts);
  DCHECK(facts.documentId === docKey,
         "a script buffer is filed under a documentId that is not its own — every principal this analysis " +
         "runs under is read off this record, so a mis-filed one analyses a document under another " +
         "document's identity");
  /* THE SEED, WHICH IS THE WHOLE OF WHAT AN AMBIENT OBSERVER CONTRIBUTES. The handler admits a seed only
     after checking it against the browser's own address for this document, so an absent one at this point is
     our own delivery path having lost it — and dispatching without one would hand the custom browser no
     document to navigate to at all. */
  DCHECK(typeof buf.seedUrl === "string" && buf.seedUrl !== "",
         "a document reached the analysis with no seed address — the CONTENT_SEED arm validates one and " +
         "writes it immediately before handing over, so this is that address being lost between the two " +
         "lines, and the custom browser would be asked to navigate to nothing");
  var tabId = facts.tabId;
  var tab = getDoc(docKey);
  /* THE PAGE-SOURCE RECORD IS NOT ASSERTED HERE ANY MORE, AND THAT IS THE MOVE RATHER THAN A LOOSENING. It
     used to be written by the arm above this line, because the arm above this line held the document's bytes;
     the document is now LOADED by the custom browser, one zone along, so the fact of whether it loaded is not
     yet known at this point and asserting it here would be asserting a thing this zone had just made up. The
     PAIRING it enforced is intact and lives where the answer is: bridge.js writes {state:"delivered"} on the
     one path that seats an engine and an {state:"unavailable", kind, …} record — carrying the chokepoint's
     own refusal reason — on every path that does not, both through `onNavigationOutcome` below, before either
     returns. No document is analysed without the positive record, and no document is refused without a
     REASON. */
  var _ep = _dataEpoch;   // a Clear during the engine round-trip invalidates this run

  // This run's merge re-registers every AST-derived endpoint, so drop the previous round's first — a
  // re-delivered document would otherwise double-register them. (The `AST DYN ` prefix this also tested for
  // is a prefix OF `AST `, so it was one condition written twice.)
  var keysToDelete = [];
  tab.endpoints.forEach(function (val, key) { if (key.startsWith("AST ")) keysToDelete.push(key); });
  for (var di = 0; di < keysToDelete.length; di++) tab.endpoints.delete(keysToDelete[di]);

  /* THE ANALYSIS PRINCIPAL. This becomes safeFetch's `opts.pageUrl` — the origin-relative private-network
     principal — and the engine's `window.location`. It is the DOCUMENT'S OWN browser-provided address, never
     the tab's (a sub-frame analyses as itself, never as its embedder) and never a content-script-supplied
     value: SECURITY.md records a `msg.url -> scripts[0].url` fallback that was an actual hole here.
     `buf.url || buf.pageUrl || ""` STOOD HERE AND `buf.pageUrl` HAD NO WRITER. Nothing in the extension has
     ever set it, so the limb was a defaulted read of a field no producer produces — CLAUDE.md's "a name that
     is READ somewhere and WRITTEN nowhere is a broken contract, and a default is what stops it being a crash".
     Worse than dead: the only value in this zone that has ever been spelled `pageUrl` is the one content.js
     ever put in its own message to this zone is a `pageUrl`, so the one plausible way to make that limb fire
     would have re-introduced by name the exact content-script-stated principal SECURITY.md says was removed.
     THE SEED IS NOT AN EXCEPTION TO THAT AND MUST NOT BECOME ONE: `buf.seedUrl` is what the custom browser
     NAVIGATES TO, and this is what the navigation is AUTHORIZED as — safeFetch classifies the private-network
     target against `sourceUrl`, so a seed that reached this line would be an address authorizing itself. The
     two are same-origin by the CONTENT_SEED arm's own check, which is exactly why nothing is lost by keeping
     them apart. */
  var tabUrl = facts.url;
  var response;
  try {
    response = await sendToOffscreen({
      type: "AST_ANALYZE", sourceUrl: tabUrl, documentId: facts.documentId, origin: facts.origin,
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
      groupId: facts.tabId, frameId: facts.frameId,
      // HTML §8.1.3.1's TOP-LEVEL CREATION URL — the browser-provided address of the top of this document's
      // navigable chain, captured on CONTENT_SEED from sender.tab.url. It is NOT sourceUrl: this document may
      // be a sub-frame, and §8.1.3.5 decides secure-context (and therefore which [SecureContext] members the
      // engine installs) from the TOP of the chain rather than from the frame's own address.
      // `|| tabUrl` went with it: the mint asserts sender.tab.url, so a top-level address that is missing here
      // is a broken invariant and not a document that legitimately has none — substituting the frame's OWN
      // address for its embedder's is precisely the ancestral answer Secure Contexts §4.2 exists to refuse.
      topLevelUrl: facts.topLevelUrl,
      /* THE SEED — AN ADDRESS, AND NEVER BYTES. `pageHtml` and `responseHeaders` stood here and this zone had
         them because a content script had FETCHED the document in the page's own realm, with the person's
         cookies, reaching the network chokepoint never — a second document-load transport, unpoliced by
         construction. Both were also DEFAULTED reads (`|| null`, `|| {}`), so a delivery path that lost the
         bundle or the policy answered the engine with a page that parses to nothing and a response that sent
         no CSP. What crosses now is the one fact an ambient observer holds and no solver can derive: the
         address the browser actually navigated to. bridge.js LOADS it — through the same `navigationLoad` a
         child navigable's document comes through — and derives from the reply everything this pair used to
         carry, plus the two things a page realm could never state: Fetch §2.2.6 "Responses"' URL LIST, and a
         refusal that names the rule that refused. */
      seedUrl: buf.seedUrl,
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
     own answer.

     A ROW IS {context, message} AND IS COPIED WHOLE. This loop rebuilt each row field by field and the third
     field was `_re.snippet || null` — bridge.js wrote that as a constant `null` on every row it ever built, no
     renderer read it, and the `replyExample` beside it was dropped here without anybody noticing, which is what
     a hand-copied projection of a record does. Both fields are deleted at the producer; a row that reached here
     with more than the pair would now be carried instead of silently trimmed. */
  DCHECK(Array.isArray(analysis.resolverErrors),
         "the engine result carried no resolverErrors array — bridge.js builds it from the engine's own " +
         "pageErrors on every result, so its absence is that relay broken and every error the engine " +
         "recorded while running this page is being dropped silently");
  if (analysis.resolverErrors.length) {
    if (!Array.isArray(tab._resolverErrors)) tab._resolverErrors = [];
    for (var _rei = 0; _rei < analysis.resolverErrors.length; _rei++) {
      var _re = analysis.resolverErrors[_rei];
      DCHECK(_re && typeof _re.context === "string" && typeof _re.message === "string",
             "an engine page-error row carried no context/message pair — bridge.js writes both as strings on " +
             "every row it builds, so a row missing one is that relay broken and the popup's diagnostic view " +
             "would print an undefined attribution beside a real error");
      console.debug("[AST:page-error] %s: %s", _re.context, _re.message);
      tab._resolverErrors.push(_re);
    }
  }

  /* WHETHER THIS RUN PRODUCED A DOCUMENT AT ALL, ASKED ONCE AND USED THREE TIMES BELOW. It is a fact about
     the RECORD, not a length: bridge.js writes the two finding arrays exactly where the engine handed it an
     @RESULT, so their presence is the run's own statement that there is something here to merge. Read into a
     local because the console line, the replace gate and the merge all need the same answer and asking three
     times is three chances to ask it differently. */
  const hasDoc = analysisHasDocument(analysis);
  /* AND THE COUNTS ARE ONLY PRINTED WHERE THERE ARE COUNTS. `0 fetchSites, 0 secSinks` for a run that never
     answered is the same false line as the empty arrays it was read off — §Testing: an absent count and a
     zero count are different facts and must never be averaged. */
  if (hasDoc)
    console.debug("[AST] tab=%d: %d fetchSites, %d secSinks, %d pageErrors", tabId,
      analysis.fetchCallSites.length, analysis.securitySinks.length, analysis.resolverErrors.length);
  else
    console.debug("[AST] tab=%d: no engine document (_run=%s), %d pageErrors", tabId,
      analysis._run, analysis.resolverErrors.length);

  /* WHAT THIS RUN WAS, ON THE DOCUMENT ITSELF. `_engineCrashed` was written twice in bridge.js and read
     nowhere, so a crashed run reached this document as a plausible empty analysis: `_astError` was null (the
     dispatch succeeded), the finding arrays were the host's own fabricated empties, and every surface
     downstream then reported a page that had been analysed and found clean. Both halves of that are gone —
     the arrays are absent on a run with no document, and the outcome is stated: it is `_run`, it is written
     on every arm of every producer, and this is where it becomes a fact about the DOCUMENT — read by the
     reclaim sweep below, by lib/serialize.js, and by the popup, which says so in words.
     THE RECLAIM GATE MOVED HERE WITH IT. It asked `_astResults || _astError` — "has this run returned" — and
     `_astResults` is now also written by the incremental merge while the run is still going, which would have
     freed a running document's page source. `_astRun` is the terminal fact and is written exactly once. */
  DCHECK(analysis._run === "complete" || analysis._run === "crashed" || analysis._run === "nothing-to-run",
         "the analysis for this document carried no run outcome (`" + analysis._run + "`) — bridge.js writes " +
         "`_run` on every record it builds, so its absence is that seam broken and a crashed run would be " +
         "recorded here as a completed analysis of a page that has nothing to find");
  tab._astRun = analysis._run;
  /* MERGED UNCONDITIONALLY. A `hasFindings` gate stood here and RETURNED before all four lines below when the
     engine emitted no endpoint and no sink — so `tab._astResults` was never written, and the reclaim sweep
     reads exactly that slot to decide a document's run has returned. A page with nothing to find therefore
     held its page source forever, and the one case that produces no symptom is again the one that was broken.
     "The engine found nothing" is a RESULT and is recorded as one.
     A RECORD THAT CARRIES NO DOCUMENT DOES NOT REPLACE ONE THAT DOES. A crash record and a page with nothing
     to run carry no finding arrays at all — they are not an observation of a page — and overwriting the last
     real snapshot with them is precisely how a run that learned an endpoint came to report none.
     "The engine found nothing" and "the engine died" are both recorded, in `_astRun`; only the first of them
     is a finding set.
     A CRASH RECORD THAT DID CARRY A DOCUMENT STILL REPLACES, and it is not the same test written twice: an
     instance that aborts between printing a snapshot and this zone consuming it hands over that snapshot,
     which is strictly newer than the last one merged (each is the run's growing accumulation and the merged
     ones are consumed as they go), so it is the most complete answer this run has.
     THE TEST IS PRESENCE AND NOT LENGTH, AND THAT IS THE FIX RATHER THAN A REWORDING. `_run === "complete" ||
     fetchCallSites.length || securitySinks.length` was asking "did it observe anything" of a record that
     could not say — an ABSENT document and a document holding zero endpoints both answered 0, so the length
     was standing in for presence and got it right only because bridge.js was fabricating the empty arrays
     that made the two indistinguishable in the first place. With the document present-or-absent the question
     has a direct answer, and the case the lengths got wrong is now the one that is right: a completed engine
     document holding no endpoint and no sink is a real observation of a page with nothing to find, and it
     REPLACES, because the engine's endpoint set is cumulative and a later document is never smaller. */
  if (hasDoc) tab._astResults = [analysis];
  mergeASTResultsIntoVDD(tab, [analysis]);
  mergeToGlobal(tab);
  notifyPopup(tabId);

  // Idle burst of the ONE host-level attention: advance other origins' parked frontiers by value
  // (non-blocking, serialized). This page's own residue (if it parked) is now in the global frontier.
  _driveGlobalFrontierBurst();
}

// (Engine-result -> doc merge -- mergeASTResultsIntoVDD -- extracted to lib/merge.js, loaded first.)

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
    /* THE RESPONSE HALF IS EMPTY BECAUSE THE SUBMISSION WAS OBSERVED AND ITS ANSWER WAS NOT. This record is
       built at the moment the page submits; the navigation that answers it lands in a document this log never
       sees. `statusText: ""` is that statement in the field the HAR export reads — never the "OK" the export
       used to synthesize from `status === 200`, which would have named a reason phrase for a round trip
       nothing here observed the end of. */
    statusText: "",
    responseBody: null,
    responseBase64: false,
    mimeType: "",
    responseHeaders: {},
    // A form submission is the BROWSER navigating, not a bundle function calling fetch — there is no call
    // site to have captured, and "" says that rather than leaving the field for a reader to default.
    callStack: "",
    /* `_source: "form_submit"` is GONE: nothing in this extension has ever read it. It was the other half of
       the same contract defect — a field written so a consumer could tell this record apart, while every
       consumer went on telling records apart by `method`, which is the string this whole change exists to
       stop routing on. `kind` is the discriminator now and it has readers; a second, unread one beside it is
       a name a future reader would trust and a producer would eventually stop writing. */
  };

  _pushGlobalLog(entry, "http", tabId, documentId, frameId);

  // Learn from the form submission
  learnFromRequest(documentId, service, entry, entry.requestHeaders);
  mergeToGlobal(tab);
  notifyPopup(tabId);
}

/* WHAT A DOCUMENT'S PAGE SOURCE IS, AS A THREE-VALUED FACT — the analysability of this document, stated so
   that a page nobody could analyse is never rendered as a page that was analysed and found clean.
     • absent            — nothing has been said about this document's source YET. Not "clean", not "failed".
                           A document registered by its own live traffic (RESPONSE_BODY) and nothing else, and
                           a document whose navigation is still in flight, sit here.
     • {state:"delivered"}   — the custom browser LOADED this document; the analysis owns the outcome from
                           there (`_astRun`).
     • {state:"unavailable", kind, …} — the seeded navigation did not produce a document, WITH the reason.
   MEASURED, and the reason this exists: reddit.com answers 403 to a second GET of a document whose URL
   carries its bot challenge's single-use token, so the load fails and this zone would otherwise hold ONE
   registered document with no document, no error and no engine — the same picture as a page still loading and
   as a page nobody visited. §@S: "a candidate killed by a gate MUST be distinguishable from one a filter ate
   and from one that was never scheduled".
   ITS WRITER IS THE ZONE THAT LOADS, WHICH IS NO LONGER THIS ONE. The two content-script message types that
   used to carry this fact are gone with the fetch that produced them; bridge.js performs the §7.4 navigation
   and calls `onNavigationOutcome` below the instant it settles. That is the same edge shape as
   `onFrontierAdvance`, and it is why the record still appears while a run is going rather than only at its
   end: what a reader asks over an empty panel is "did this fail, or is there nothing here yet". */
function _setPageSource(doc, record) {
  DCHECK(record.state === "delivered" || record.state === "unavailable",
         "a document's page-source record was written with state `" + record.state + "`, which is neither of " +
         "the two this zone speaks — serialize.js renders this field verbatim, so a third state reaches the " +
         "reviewer as a page whose analysability is simply not stated");
  /* NO TIMESTAMP. One stood here and serialize.js dropped it, so it was a writer with no reader — the exact
     half-contract §Architecture names, and the one that is hardest to notice because the value is real. What
     a reader wants to know about this record is WHICH state it is, and the record is overwritten rather than
     accumulated, so there is no ordering question for a clock to answer. */
  doc._pageSource = record;
}

/* THE KINDS A REFUSAL MAY WEAR, AND THEY ARE A CLOSED SET BECAUSE THAT PAIRING IS THE WHOLE CONTENT OF THE
   REPORT — "unavailable" with no distinguishing reason is the silence this record exists to replace, spelled
   as a field. `status` rides exactly one of them; an absent `status` on `network`/`empty` is the POSITIVE
   statement "this kind has no status" and is stored absent, never as a plausible 0.
   THEY ARE ASSERTED NOW, NOT VALIDATED-AND-DROPPED, AND THAT IS THE CONSEQUENCE OF THE PRODUCER MOVING. The
   old producer was a CONTENT SCRIPT — a hostile input under SECURITY.md's trust table, so a malformed report
   was a compromised renderer and had to be dropped closed rather than crash the only trusted zone. The
   producer is now bridge.js, in this same document, composing from `lib/safe-fetch.js`'s own reply record: a
   malformed one there is OUR logic being wrong, which is a DCHECK by §Offensive-programming and would be
   silently swallowed by a drop. */
var _PAGE_SOURCE_KINDS = ["status", "empty", "network"];
/* THE EDGE FROM THE ZONE THAT LOADS TO THE ZONE THAT RENDERS. bridge.js calls this exactly once per seeded
   document, the instant its §7.4 navigation settles and BEFORE it seats an engine or returns — so the
   pairing that used to be asserted at the dispatch is intact one zone along: no document is analysed without
   {state:"delivered"}, and no document is refused analysis without a reason. */
self.onNavigationOutcome = function onNavigationOutcome(documentId, record) {
  DCHECK(typeof documentId === "string" && documentId !== "",
         "a navigation outcome arrived naming no document — the record is filed against the browser-provided " +
         "documentId the dispatch was made under, and one without it would be a report about a page the " +
         "reviewer cannot be shown");
  DCHECK(!!record && (record.state === "delivered" || record.state === "unavailable"),
         "a navigation outcome arrived in a state this zone does not speak (`" + (record && record.state) +
         "`) — bridge.js writes exactly the two, and a third would reach the popup as a page whose " +
         "analysability is simply not stated");
  if (record.state === "unavailable") {
    DCHECK(_PAGE_SOURCE_KINDS.indexOf(record.kind) >= 0,
           "a navigation outcome was refused with kind `" + record.kind + "`, which is not one of the three " +
           "this seam and the popup both spell out — a report that cannot say WHICH failure it was is the " +
           "silence it exists to replace wearing a field name");
    DCHECK((record.kind === "status") === (record.status !== undefined),
           "a navigation outcome carries kind `" + record.kind + "` with status `" + record.status + "` — a " +
           "status rides the `status` kind and no other, so this pair is the two halves of one report " +
           "disagreeing about which failure it describes");
    DCHECK(record.kind !== "status" ||
           (Number.isInteger(record.status) && record.status >= 100 && record.status <= 599),
           "a navigation outcome reported status `" + record.status + "`, which no HTTP response has — " +
           "safeFetch answers 0 for a request that never went on the wire and that is the `network` kind, " +
           "carrying the rule that refused it");
    DCHECK((record.kind === "network") === (typeof record.detail === "string"),
           "a navigation outcome of kind `" + record.kind + "` carries detail `" + record.detail + "` — the " +
           "detail is the chokepoint's own refusal reason and rides the `network` kind alone, so this pair " +
           "is one report's two halves disagreeing");
  }
  /* THE DOCUMENT IS LOOKED UP, NEVER MINTED. `getDoc` creates an empty DocData for an unknown id, and a
     navigation outcome is only ever produced for a document this zone SEEDED — so a mint here would file a
     load report against a document nobody announced, under an identity (tabId, origin, url) that is empty,
     and the popup would render a page that does not exist. */
  var doc = state.docs.get(documentId);
  DCHECK(!!doc,
         "a navigation outcome arrived for a document this zone never registered — the CONTENT_SEED arm " +
         "registers the document and mints its facts before it dispatches, so an unknown id here is a " +
         "report about a load nobody asked for");
  if (!doc) return;   // release path under the assert
  _setPageSource(doc, record);
  notifyPopup(doc.tabId);
};

/* THE FOUR TYPES `CONTENT_TYPES` ADMITS, AND NOTHING ELSE — the dispatch below is exhaustive over that set
   and DFAILs past it, so the router's admission and this function's arms cannot drift apart in silence.
   CONTENT_KEYS / CONTENT_ENDPOINTS were removed (heuristic regex scans over HTML text, which is Lexbor's
   parsing job); CONTENT_DOM / CONTENT_FORMS / SCRIPT_SOURCE went with the per-script shipping the engine
   replaced; CONTENT_HTML and CONTENT_HTML_UNAVAILABLE went with the content-script fetch that produced them —
   a seed is an ADDRESS and the custom browser loads the document itself, so what a page could not load is the
   LOADER's report and not a message type. Real endpoints come from forced execution observing actual
   fetch/XHR at the host edge. */
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
  /* A MESSAGE IS A DOCUMENT ANNOUNCING ITSELF, SO THIS IS WHERE IT IS REGISTERED — every type, not just
     CONTENT_SEED. `state.docs.get` for everything else stood here, guarded by `if (doc)`, on the reasoning
     that passive traffic must not "resurrect" a document a review sweep had dropped. That sweep is gone (this
     zone holds no document bytes to reclaim), and what the guard actually did was route every learner below
     to a throwaway view. There is nothing to resurrect:
     a DocData is identity plus this document's own view of what has been learned, the learned half is already
     in globalStore, and getDoc mints an empty one. CONTENT_SEED remains the ANALYSIS trigger — a distinct
     thing from being registered.
     ONE MINT PER MESSAGE, and the DocData is stamped from it rather than from `sender` a field at a time.
     `if (sender.url) doc.url = sender.url` stood here too, which is a guarded assign past a broken invariant:
     a content message with no address left the DocData holding the PREVIOUS document's url at this key, and
     that url is the private-network principal. The mint asserts instead. `origin` is the MessageSender
     principal (via _senderOrigin, which also populates _docOrigins) — NEVER url-derived; `url` is the
     document's OWN address, NOT the tab's top-page url; tabId/frameId are routing/UI fields only. */
  const _facts = _browserFacts(sender);
  const doc = getDoc(documentId);
  doc.tabId = _facts.tabId;
  doc.frameId = _facts.frameId;
  doc.url = _facts.url;
  doc.origin = _facts.origin;
  if (sender.tab.title) doc.title = sender.tab.title;   // UI label; the browser legitimately has none yet

  // RESPONSE_BODY comes from intercept.js via content.js relay
  if (msg.type === "RESPONSE_BODY") {
    handleResponseBody(tabId, msg, sender.frameId, documentId);
    return;
  }

  // PROBE_HIT: a document reports that apiclientsink was called with an active probe marker. Correlating it to
  // a session is only half the question — WHICH DOCUMENT reported it is the other half, and the record is only
  // evidence about the sink if that document is the one the payload was delivered to. `_facts` is the ONE
  // mint's output; the hit's own fields are the untrusted renderer's claims. (`sender.frameId || 0` and
  // `tabId` were re-derived here beside the mint that already answers both — one record, one derivation.)
  if (msg.type === "PROBE_HIT") {
    _recordProbeHit(msg, _facts);
    return;
  }

  // SCRIPT_SOURCE / SCRIPTS_LOADED removed: content.js no longer ships per-script
  // bodies or a load signal. One CONTENT_SEED per document drives the analysis; the custom browser loads the
  // suggested address and the engine sources every script from what it loaded (inline via the SSR phase,
  // external via __feLoadScript, dynamic via the createElement host edge).

  if (msg.type === "CONTENT_FORM_SUBMIT") {
    _handleFormSubmit(documentId, msg, tabId, sender.frameId);
    return;
  }

  /* A DOCUMENT SUGGESTING WHICH DOCUMENT TO EXPLORE — an ADDRESS, and the whole of what an ambient observer
     contributes. It used to be the page's BYTES, fetched by the content script in the page's own realm with
     the person's cookies: a second document-load transport that reached `lib/safe-fetch.js` never, so the
     scheme allowlist, the origin-relative SSRF/PNA guard on the initial AND post-redirect URL, CORB and the
     credentialed destructive-path deny list applied to the engine's own navigations and to nothing arriving
     that way. The custom browser now LOADS the suggestion itself; there is one document-load path and this is
     not it. `CONTENT_HTML_UNAVAILABLE` went with the fetch that produced it — a report about a load belongs
     to the LOADER, and bridge.js makes it through `onNavigationOutcome`, carrying the chokepoint's own reason.

     THE ONE CHECK THIS ZONE OWES, AND WHY IT IS EXACTLY THIS ONE. A content script is a hostile input
     (SECURITY.md's trust table) and an untrusted zone does not get to state an address — so a suggestion that
     named ANY address would hand a compromised renderer a fetch of the user's intranet through this
     extension's `<all_urls>` host permissions. What makes the seed admissible is that it is not a free
     address: HTML §7.2.5 "The History interface"'s shared history push/replace state steps refuse a rewrite
     unless the target passes "a Document document can have its URL rewritten to a URL targetURL", which
     returns false "if targetURL and documentURL differ in their scheme, username, password, host, or port
     components". So the ONLY thing pushState can move is the PATH, and the only thing an SPA needs the seed
     for is exactly that. This zone therefore admits a seed whose ORIGIN is the one the BROWSER reported for
     this document and refuses every other, which admits every SPA route and no new address at all.
     AND THE PRINCIPAL IS STILL NEVER THE SEED'S. `facts.url` — the browser-stated address — is what travels
     as `sourceUrl` and is what safeFetch classifies the private-network target against; the seed is only ever
     the thing to fetch. Same-origin is what makes those two classify identically, which is why refusing the
     mismatch costs nothing real.
     DROPPED CLOSED, NOT ASSERTED: a mismatch is a compromised renderer, not this zone's invariant broken, and
     a DCHECK here would hand any web page an abort of the only trusted zone in the extension. */
  if (msg.type === "CONTENT_SEED") {
    if (typeof msg.seedUrl !== "string" || msg.seedUrl === "") {
      console.debug("[brain:content] CONTENT_SEED dropped — no address (a content script is an untrusted " +
                    "input; fail closed)");
      return;
    }
    var _seedOrigin = _originOfUrl(msg.seedUrl);
    var _docOrigin = _originOfUrl(_facts.url);
    if (_seedOrigin === "" || _seedOrigin !== _docOrigin) {
      console.debug("[brain:content] CONTENT_SEED dropped — suggested `%s` (origin %s) for a document the " +
                    "browser says is at origin %s; HTML §7.2.5 lets pushState move the path and never the " +
                    "origin, so this is not a route of this document", msg.seedUrl, _seedOrigin, _docOrigin);
      return;
    }
    /* NO PAGE-SOURCE RECORD IS WRITTEN HERE. Whether this document loads is not known yet — the navigation
       has not happened — and writing "delivered" at the arrival of a SUGGESTION would state a fact this zone
       invented. bridge.js writes it, both ways, the instant the load settles. */
    var _dk = documentId;
    var _buf = _scriptBuffers.get(_dk);
    if (!_buf) { _buf = {}; _scriptBuffers.set(_dk, _buf); }
    /* THE RECORD CARRIES THE FACT RECORD, NOT SIX COPIES OF ITS FIELDS. Each of tabId / frameId / docKey /
       origin / url / topLevelUrl used to be re-stated here off `doc` or off `sender`, and every restatement is
       a place a value can be substituted for a neighbouring one — which is exactly what happened at the
       address: `_buf.url` once fell back to `sender.tab.url`, inverting the origin-relative rule (a frame in a
       tab whose top is http://localhost/ inherited that PRIVATE classification and could reach the user's
       intranet on its behalf). One record, minted from the MessageSender, asserted at the mint.
       WE STILL DO NOT RECORD "isTop": a frame cannot prove it is the main frame from its own report — a fenced
       frame is its own tree's root and would impersonate it. `frameId` is the browser's answer to that
       question, the same provenance as `sender.tab.url`, and bridge.js reads it to tell a SUB-frame (which its
       cluster's instance already runs as a realm of its own, §4.8.5) from the group's TOP document. */
    _buf.facts = _facts;
    /* AND THE SEED BESIDE IT — the one thing on this record that is not the browser's, kept as its own field
       rather than folded into the fact record precisely so that nothing downstream can mistake it for one. */
    _buf.seedUrl = msg.seedUrl;
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

  /* AN UNMATCHED TYPE IS THE ROUTER AND THIS DISPATCH DISAGREEING, never a message to drop. `CONTENT_TYPES`
     is the admission set and the four arms above are its arms, so a type reaching here is one added to that
     set with no handler — which as a silent `return` is a document announcing itself into nothing. */
  DFAIL("handleContentMessage was handed `" + msg.type + "`, which its dispatch has no arm for — the router " +
        "admits exactly CONTENT_TYPES, so this type was added to that set without one");
}

// Popup messages — sender.tab is absent for popup contexts.
// (Popup command dispatch -- handlePopupMessage -- extracted to lib/popup-handlers.js; the brain wires
// the onMessage listener to it.)

function _pruneProbeSessions() {
  const now = Date.now();
  // `createdAt` is the age, and it is the only clock a session has — see startExploitProbe on why there is
  // no `finishedAt` for this to have preferred.
  for (const [k, s] of _probeSessions) {
    if (now - s.createdAt > PROBE_SESSION_TTL_MS) _probeSessions.delete(k);
  }
  if (_probeSessions.size > PROBE_SESSION_MAX) {
    const entries = [...(_probeSessions.entries())]
      .sort((a, b) => a[1].createdAt - b[1].createdAt);
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
  var call = "apiclientsink('" + marker + "')";           // the verifier's proof hook (marker = crypto.randomUUID)
  // The engine's X9 is a bare fire-marker: `onerror=X9`, `javascript:X9`, `';X9()//`. Map every X9 (call or
  // ref form) to the apiclientsink CALL so the real sink, on firing, relays the proof. Its structure (breakout
  // + gate prefix + surviving transforms) is untouched.
  var payload = String(poc).split("X9()").join(call).split("X9").join(call);
  var context = "engine:" + (sinkName || "?");
  var out = { pocJs: null, payload: payload, context: context, source: source || null };
  /* EVERY ARM ANSWERS THE FULL RECORD, INCLUDING THIS ONE — it used to return `{pocJs, why}` alone, above the
     line that builds `payload`, so the one caller's own assert ("it returns {pocJs, payload, context, source}
     on every arm including the ones it cannot perform") aborted on a finding whose page address was never
     recorded. The finding's breakout is still real; what is missing is a document to navigate. */
  if (!pageUrl) {
    out.why = "no page url recorded for this finding — a live delivery navigates the page the sink was observed on";
    return out;
  }
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
    /* THE ADDRESS IS BUILT THROUGH THE URL PARSER, AND IT IS RETURNED, because it is not only the thing to
       navigate to — it is the thing a HIT IS ATTRIBUTED AGAINST. `pageUrl.split("#")[0]` produced a string
       nobody had parsed, so an unparseable pageUrl became an unparseable window.open argument and there was
       no origin to compare a reporting document's principal to. A page whose address does not parse has no
       document to navigate and no origin to attribute to, which is an answer, not a build failure. */
    var base;
    try { base = new URL(pageUrl); }
    catch (_) {
      out.why = "the page this sink was observed on has no parseable address (" + JSON.stringify(pageUrl)
              + "), so there is no document to navigate to and no origin a hit could be attributed to.";
      return out;
    }
    base.hash = "";                       // empty string ⇒ fragment null ⇒ no trailing `#` in href
    if (!frag) base.search = "";
    out.targetUrl = base.href + deliveryPrefix + payload;
    out.targetOrigin = base.origin;       // OUR expectation, compared against the browser's MessageSender.origin — never the reverse
    out.pocJs = "window.open(" + JSON.stringify(out.targetUrl) + ', "_blank");';
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
  if (delivery === "cross-document-message") {
    // HTML §9.3.3 "Posting messages". The POST itself is the one delivery this layer is best placed to make —
    // open the victim, hold the handle, post the payload — and it is deliberately NOT made, because the thing
    // the sandbox cannot choose is the one thing the victim's handler reads first.
    //
    // §9.3.2.2 "User agents" states the whole basis of the API: "the integrity of this API is based on the
    // inability for scripts of one origin to post arbitrary events … to objects in other origins". The
    // attacker page here is `<iframe sandbox="allow-scripts">`, whose origin is OPAQUE, so every message it
    // sends arrives with `event.origin === "null"` — an origin no real handler's check accepts. A handler that
    // checks nothing would fire, and a handler with any origin gate would not; both would come back as NO HIT
    // through the same channel, and §LIVE-VERIFY reads no-hit as an ENGINE-FIDELITY DIVERGENCE. Emitting a
    // probe that can answer "no" for a reason that is not a divergence poisons the one signal this whole path
    // exists to produce, so the mechanism is NAMED instead of half-performed.
    //
    // WHAT IT NEEDS is one thing and it is not this file's: an attacker document at an origin the harness
    // CHOOSES — a registrable one, served rather than sandboxed — so the post carries an origin a gate can be
    // tested against. The engine's own half of that pair is the required-origin the gate demands, which it
    // does not yet surface for a forgeable check (a startsWith/endsWith/includes token is not recorded by any
    // derivation today), so neither side of the delivery can be built without the other.
    out.delivery = "the attacker keeps the victim open in a document of their own and posts the payload to it";
    out.why = "this source arrives by CROSS-DOCUMENT MESSAGE (HTML §9.3.3): the attacker holds the victim open "
            + "in a document of their own and postMessage()s the payload to it. The post itself is performable "
            + "here — what is not is the ORIGIN it comes from: the attacker page is a sandboxed frame with an "
            + "OPAQUE origin, so the victim would see `event.origin === \"null\"`, and a handler with any "
            + "origin check would report NO HIT for a reason that is not an engine divergence. Reproducing it "
            + "needs an attacker document at a chosen, registrable origin. The sink and its breakout are still "
            + "fire-verified.";
    return out;
  }
  DFAIL("the engine declared a delivery mechanism this layer has no arm for: " + JSON.stringify(delivery)
        + " — the token vocabulary is solve.h's `delivery` field (address / plant / referring-address / "
        + "user-file / cross-document-message), so "
        + "either a mechanism was added in C without its delivery arm here, or this record did not come from "
        + "solve_json_array");
  // Release only (the DFAIL above is the dev answer): `why` is stated here too, so the record's shape holds
  // on every arm and the panel prints the reason rather than an unexplained absent PoC.
  out.why = "the engine declared the delivery mechanism `" + delivery + "`, which this layer has no arm for.";
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
    // `prepared` is the only state a STORED session has ever had — it was written "running" here and
    // overwritten below before the map ever saw it, so the transition was to nobody. It is stated once, where
    // it is true, and the state a session would need for a lifecycle it does not have is not invented.
    marker, status: "prepared", pageUrl: pageUrl || null,
    findingId: findingId || null, sourceUrl: (msg && msg.sourceUrl) || null,
    sinkName: sinkName || null, waitMs: wait,
    /* NO `finishedAt` AND NO `error`. Both were written here, to null, and by nothing else ever — this
       session has no terminal transition: the delivery is performed by the sandboxed page and evidence
       arrives as PROBE_HIT appends to `hits`, so there is no moment at which a session finishes or fails.
       Two names that could only ever be null were two `|| null` defaults on the STATUS reply pretending to
       be a lifecycle, and `error` collided with that reply's own "session not found" error. `createdAt` is
       the LRU key, which is what `finishedAt || createdAt` was already resolving to on every session. */
    hits: [], createdAt: Date.now(),
    /* THE DELIVERED DOCUMENT — the whole basis on which a hit can be ATTRIBUTED rather than merely counted.
       Filled from the delivery this zone actually built (never re-derived at hit time: §scheduler's rule that
       an operation becoming a work item takes its inputs WITH it — a PROBE_HIT arrives long after, and reading
       the address back off the finding then would read whatever the store holds by then). `expect` null means
       this session performed no delivery at all, so NO document was ever handed the marker and every hit on it
       is somebody's claim about a payload that was never sent. `deliveredDocumentId` is latched by the first
       attributed hit: documentId is the only stable per-document identity (a (tab, frame) pair is reused across
       navigations at a DIFFERENT origin), so it is what makes "the same document fired twice" distinguishable
       from "two documents both know the marker". */
    expect: null,
    deliveredDocumentId: null,
  };
  // ENGINE AGREEMENT: perform the delivery the ENGINE declared, carrying its EXACT poc (X9 -> apiclientsink).
  // The user runs it by clicking Run in the sandboxed attacker page (poc-sandbox.html) — that real click is
  // the user activation window.open needs. When the real page's sink fires, intercept.js → content.js →
  // PROBE_HIT lands on this marker and _recordProbeHit decides whether the reporting document is the one the
  // payload was delivered to.
  var _poc = buildLiveDelivery(sinkName, poc, source, delivery, deliveryPrefix, session.pageUrl, marker);
  DCHECK(!!_poc && typeof _poc === "object" && typeof _poc.payload === "string",
         "buildLiveDelivery answered with no record — it returns {pocJs, payload, context, source} on every "
         + "arm including the ones it cannot perform, so an absent one is that function having grown a path "
         + "that returns nothing and this session would carry neither a PoC nor a reason");
  session.pocJs = _poc.pocJs;
  session.pocWhy = _poc.pocJs ? null : _poc.why;   // which declared mechanism this layer cannot perform
  if (_poc.pocJs) {
    DCHECK(typeof _poc.targetUrl === "string" && typeof _poc.targetOrigin === "string",
           "a delivery arm produced a pocJs without naming the address it navigates to — the target URL and "
           + "its origin are what a PROBE_HIT is attributed against, so a PoC without them can only ever be "
           + "counted, and every hit on this session would report as unattributable");
    // frameId 0 is the BROWSER'S name for the top-level traversable, and window.open creates exactly one.
    session.expect = { url: _poc.targetUrl, origin: _poc.targetOrigin, frameId: 0 };
  }
  /* THE REASON IS AS REQUIRED AS THE PoC, because one of the two is always the answer: a delivery this layer
     can perform has a pocJs, and one it cannot has the sentence saying which mechanism and why. An absent
     both is a session the panel can only render as a broken build. */
  DCHECK(typeof session.pocJs === "string" ? session.pocWhy === null : typeof session.pocWhy === "string",
         "a probe session carries neither a PoC nor a reason — buildLiveDelivery states `why` on every arm " +
         "that answers no pocJs, so this is that record having grown a path that answers neither");
  _probeSessions.set(marker, session);
  return session;
}

/* ATTRIBUTE A RELAYED apiclientsink CALL TO A DOCUMENT — the check SECURITY.md names as missing and buildable.
   Before this, a PROBE_HIT was accepted from ANY document in ANY tab that knew the marker: tabId/frameId were
   stamped onto the record and never compared, and the reporting document's principal was never looked at. The
   marker rides INSIDE the payload (it has to — it is what a fired sink relays), and the payload is delivered TO
   a page, so ANY document that can read that address holds it. A hit from somewhere else is therefore not a
   weaker hit; it is a different document's claim about someone else's exploit.

   WHAT THIS CLOSES AND WHAT IT DOES NOT. It closes CROSS-DOCUMENT fabrication: a document at another origin,
   in another tab, in a subframe, or in a second document of the target origin cannot make this zone print a
   hit as evidence for the sink. It does NOT close the SAME-DOCUMENT case and cannot: `window.apiclientsink` is
   installed in the page's own main world, so any script in the delivered document can call it, and every fact
   the hook could report about its own caller is reported BY that world. That residual is irreducible by this
   mechanism and is what the verdict wording must state (lib/popup-security.js).

   THE DIRECTION OF THE ORIGIN COMPARISON IS THE SECURITY RULE. `facts.origin` is the browser's
   MessageSender.origin (opaque-unique via _senderOrigin) — the principal, never derived here. `expect.origin`
   is OUR OWN value: the origin of the address this zone built and handed to window.open. A fact is compared
   against an expectation; an expectation is never promoted to a fact. (An opaque target — a file:/data: page —
   parses to "null" while every opaque DOCUMENT gets a per-document `null:<uuid>` token, so such a session can
   never attribute a hit. That is the spec's rule, not a gap: an opaque origin is same-origin with nothing.)

   A MISMATCH IS RECORDED, NEVER DROPPED. §@S: absence of a PoC is never a "safe" verdict, and a marker
   surfacing in a document that was never handed it is itself a fact about the page. It lands on the same one
   `hits` array carrying its own verdict, because two arrays would be two vocabularies for one event. */
function _sameAddress(a, b) {
  if (typeof a !== "string" || typeof b !== "string") return false;
  try { return new URL(a).href === new URL(b).href; } catch (_) { return false; }   // b is attacker text; a non-URL is an answer
}
function _recordProbeHit(msg, facts) {
  // `hit` crosses from the UNTRUSTED renderer, so its fields are validated as attacker input rather than
  // asserted — a DCHECK here would be this zone asserting a hostile producer's correctness, and a page could
  // abort the offscreen by sending `{type:"PROBE_HIT", hit:{}}`. The legacy `marker` spelling is DELETED:
  // intercept.js relays `{id, at, url, …}` and always has, so it was a reader with no writer.
  const hit = msg && msg.hit;
  const hid = hit && hit.id;
  if (typeof hid !== "string" || hid === "") return;
  const ses = _probeSessions.get(hid);
  if (!ses) return;                 // no session by that marker — nothing this hit could be evidence about

  const exp = ses.expect;
  let mismatch = null;
  if (!exp) {
    mismatch = "this session delivered nothing — its engine-declared mechanism is one this zone cannot perform, "
             + "so no document was ever handed this marker";
  } else if (facts.origin !== exp.origin) {
    mismatch = "reported by " + facts.origin + ", but the payload was delivered to " + exp.origin;
  } else if (facts.frameId !== exp.frameId) {
    mismatch = "reported by frame " + facts.frameId + ", but the payload was delivered to the top-level document";
  } else if (ses.deliveredDocumentId !== null && ses.deliveredDocumentId !== facts.documentId) {
    mismatch = "reported by a second document of " + exp.origin + " — the delivered one already fired this marker";
  }
  if (!mismatch && ses.deliveredDocumentId === null) ses.deliveredDocumentId = facts.documentId;   // latch it

  /* TWO HALVES, NAMED FOR THEIR PROVENANCE, because the whole defect this fixes was a record that mixed them.
     `browserStated` is the ONE mint's output — chrome.* answers a renderer cannot forge. `pageClaimed` is
     everything intercept.js read inside the page: it is CORROBORATION and never attribution, and the popup
     labels it as such. `Object.assign({}, msg.hit, …)` used to splice the untrusted half straight onto the
     record under unmarked names, which is SECURITY.md's "a UI that labels an untrusted claim like a fact". */
  ses.hits.push({
    id: hid,
    attributed: !mismatch,
    mismatch: mismatch,
    browserStated: {
      documentId: facts.documentId, origin: facts.origin, url: facts.url,
      tabId: facts.tabId, frameId: facts.frameId,
    },
    pageClaimed: {
      at: typeof hit.at === "number" ? hit.at : null,
      url: typeof hit.url === "string" ? hit.url : null,
      // Was the reporting document still AT the delivered address when the hook ran? BOTH sides go through
      // the URL parser rather than being compared as strings: the fragment percent-encode set (WHATWG, and
      // the engine owns the table — a second copy here is the drift CLAUDE.md forbids) means `location.href`
      // shows an HTML breakout re-encoded, so a raw `indexOf(payload)` is false for every real payload. The
      // platform normalises both ends identically, which is the only comparison that is spec-faithful.
      addressMatch: _sameAddress(exp && exp.url, hit.url),
      /* The caller's own frame, for a HUMAN to read. It is deliberately NOT scored, and the reason is worth
         stating so nobody later promotes it: there is no shape here that only a fired sink produces. A real
         `eval(location.hash)` sink fires with the page's own script on the stack and `document.currentScript`
         set to it — byte-identical to a page script calling the hook directly — so any rule over these fields
         would call a genuine sink a fabrication. It is context; the verdict never derives strength from it. */
      stack: typeof hit.stack === "string" ? hit.stack.slice(0, 1024) : null,
      currentScript: typeof hit.currentScript === "string" ? hit.currentScript : null,
      eventType: hit.event && typeof hit.event.type === "string" ? hit.event.type : null,
      eventTarget: hit.event && typeof hit.event.target === "string" ? hit.event.target : null,
      // The page's world refused to answer at all — itself a signal, so it is recorded rather than defaulted
      // into "no evidence".
      unavailable: hit.evidenceUnavailable === true,
    },
  });
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

  /* API key: the user's explicit choice from the Send panel's key selector, which the popup posts with every
     export. THERE IS NO ENDPOINT ARM. `else if (ep?.apiKey) { ep.apiKeySource === "url" ? … }` stood here and
     neither field exists on an endpoint record — lib/merge.js, the extension's only `endpoints.set`, writes
     {url, method, host, path, service, source, pageUrl, requiredHeaders, pathParams, firstSeen} — so the arm
     could not fire and the whole `ep` lookup existed to feed it. */
  if (msg.apiKeyOverride && !msg.apiKeyOverride.disabled && msg.apiKeyOverride.key) {
    if (msg.apiKeyOverride.source === "url") {
      parsedUrl.searchParams.set("key", msg.apiKeyOverride.key);
    } else {
      headers["X-Goog-Api-Key"] = msg.apiKeyOverride.key;
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
/* NO `CONTENT_PING`. The type was admitted here and handled above by appending {at, pageUrl} to a per-document
   array that NOTHING has ever read — and no content script has ever sent one, so it was a liveness channel
   with neither end. A document's liveness is answered by webNavigation.getAllFrames (GET_FRAMES), which is the
   browser's answer rather than a page's claim about itself. */
const CONTENT_TYPES = new Set([
  "CONTENT_SEED",
  "CONTENT_FORM_SUBMIT",
  "RESPONSE_BODY",
  "PROBE_HIT",
]);

// The brain runs in the OFFSCREEN document and receives messages DIRECTLY:
// chrome.runtime.sendMessage broadcasts to every extension context, so both our
// content scripts (in web renderers) and the popup reach this document without
// the SW relaying anything. The thin SW forwards only ONE browser event the
// offscreen can't observe and that no document message carries: __evt TAB_REMOVED
// (so per-tab transient state is freed when a tab closes). Navigation/activation
// are NOT forwarded — prioritization is driven by each document's CONTENT_SEED.
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
  // A tab close frees NOTHING else. The document's own analysis buffer is freed by the review reclaim (when
  // its run returns), not by the tab going away, and the cross-session continuation of a closed tab's learning
  // is the GLOBAL frontier's parked recipes rather than a page source held in RAM. Keyed by documentId.
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
