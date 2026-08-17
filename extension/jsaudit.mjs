/* jsaudit.mjs — the JS-SURFACE GATE. `node extension/jsaudit.mjs`
 *
 * CLAUDE.md §Architecture: the C engine owns ALL semantics; the ONLY irreducible JS is a BRIDGE, and the
 * reasons it may exist are the four SECURITY.md names — (1) relay safeFetch, (2) relay IndexedDB, (3) relay
 * chrome.*, (4) render the popup — plus loading the WASM. Every other line of JS in extension/ is semantics
 * that must become a C component, and the list of which-is-which has to live in ONE place that every file
 * goes through, or a new file joins the surface unclassified and nobody notices.
 *
 * This is that place, and it is a GATE rather than a note:
 *   • a .js on disk with no LEDGER row FAILS   — a new file must declare its zone
 *   • a LEDGER row with no file on disk FAILS  — a stale row is the DFAIL-that-lies failure mode
 *   • any surviving LOGIC row FAILS            — the queue is the work, printed in order with live counts
 * Its only correct trajectory is to zero: when the last LOGIC row is gone the gate goes green, and the file
 * that follows it out is this one. Do NOT soften a row to make it pass — that is choosing the report over
 * the forcing function.
 *
 * ZONES
 *   BRIDGE:<n>  irreducible, where n names the SECURITY.md reason (1 fetch, 2 IDB, 3 chrome.*, 4 popup, 5 WASM load)
 *   LOGIC       semantics that belongs in a C component — `dest` names the component it becomes
 *   MIXED       both, in one file — `split` states the boundary; it is queued as LOGIC until split
 */

import { readdirSync, readFileSync } from "node:fs";
import { join, dirname, relative } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = dirname(fileURLToPath(import.meta.url));

/* THE MOVE ORDER IS DERIVED FROM THE TREE, NEVER DECLARED — that is the change this file exists in its
   current shape for, and it is a correction rather than a refinement.
 *
 * WHAT WAS HERE. Every row carried a hand-written `step` number and the paragraph above them stated the
 * ordering principle in two sentences that CONTRADICT EACH OTHER: "a producer whose consumers still live in
 * JS is not first, because moving it makes every call it serves cross the JS↔WASM boundary the architecture
 * rule exists to delete", and then "callees before callers". Those are opposite instructions. A callee IS a
 * producer, and its callers ARE the consumers that still live in JS, so a queue built leaf-first is precisely
 * the queue the first sentence forbids. The second one won, twice, and both times the head of the queue named
 * a file nobody could move: `lib/discovery.js` sat at step 1 until 7adea69d re-stepped it to 7, and the
 * codecs sat at step 2 with every one of their consumers — learn.js, send.js, encode.js, offscreen-brain.js,
 * and five popup files — still in JS. The `step` field is DELETED. It is the field that has been wrong twice,
 * and a hand-written number cannot be checked against anything.
 *
 * WHAT REPLACES IT is the same question asked of the tree. These are CLASSIC SCRIPTS sharing one global
 * scope, so a call is a top-level declaration in one file named in another, and both halves of that are
 * readable from disk: the declarations by their position (column zero), the uses by a word-boundary match
 * over the source with COMMENTS REMOVED. The comment strip is not tidiness — it decided real edges. Twelve of
 * the graph's edges before it were PROSE: `lib/learn.js` names `handleResponseBody` twice in comments and
 * calls it never, `lib/grouping.js` names half of `lib/stats.js` in a paragraph about what it does not do.
 * A ledger built on those edges would order the queue by what the files SAY about each other.
 *
 * A CYCLE IS ONE UNIT, NOT A CONTRADICTION. `offscreen-brain.js` calls `handleResponseBody`, which calls
 * `_pushGlobalLog`, `notifyPopup` and `_docForLearning` straight back; `learn.js` and `merge.js` reach into
 * each other the same way. Eleven of the queued files are one mutually-referencing component of the graph,
 * and the honest thing to print is that they MOVE AS ONE — no ordering among them exists to discover, and an
 * order invented for them would be the hand-written number again.
 *
 * THE ORDER IS THEN FORCED: a unit moves only after every unit that CALLS it, because until then the call it
 * serves would cross the boundary this architecture deletes. That puts the DRIVERS first and the leaf codecs
 * LAST — the exact inverse of what the deleted field claimed — and it is the same answer the three landed
 * moves already gave when they were made one capability at a time rather than one file at a time:
 * `multipart_batch.c` (an address, and `endpoint.c` already consumed addresses), `reply_decode.c`'s Flight
 * reader (`engine_provide` already held the reply), `browser_process/network/resource_kind.c` (the trusted
 * zone already called that program). Each moved because ITS CONSUMER WAS ALREADY IN C. That is what "takeable"
 * means here, and the derived order says which unit to look inside for the next one.
 *
 * AND THE PRINTED NUMBER IS NOT A NAME — NOTHING OUTSIDE THIS FILE MAY CITE IT. It is a position in a
 * derivation, so it moves whenever an edge does, and the deleted field proved what that costs: six C headers
 * cited "jsaudit step 2" / "step 1" / "step 3", every one of them written true and every one made false by a
 * renumber they could not see — the stale-DFAIL failure mode with a queue behind it instead of a crash. All
 * six now name the FILE and its destination, which is the fact that does not move, and a comment that wants
 * to point at this queue points at a row. */
const LEDGER = [
  // ── BRIDGE — irreducible platform edge ───────────────────────────────────────────────────────────────
  { f: "bridge.js", zone: "BRIDGE:1,2",
    why: "drives qjs_step, relays every byte through safeFetch, persists the frontier to IDB, routes " +
         "cross-instance notices. The Level-1 WFQ lives here because one WASM instance is one agent cluster " +
         "and no instance can rank the others. THE ROW NO LONGER CLAIMS REASON 5, and that is the whole point " +
         "of the change that took it: this file used to `import(\"./lib/qjs/qjs.mjs\")` and build every " +
         "instance with `createQJS()` in the offscreen's own realm, so it held a Module handle and `HEAPU8` " +
         "over an UNTRUSTED engine. Loading is renderer-host.js's now, every qjs_* call is an awaited " +
         "postMessage to a sandboxed opaque-origin frame, and nothing here can address an instance's memory — " +
         "grep this file for HEAPU8 or createQJS and the only hits are the comments recording that." },
  { f: "mojo.js", zone: "BRIDGE:1,5",
    why: "THE IPC PRIMITIVE, and it is a bridge for the reason a bridge exists at all: the WASM cannot reach " +
         "`Worker`, `MessagePort` or `postMessage`, which are the only things a process boundary is made of " +
         "here. It carries reason 1 (safeFetch's CORB and classification decisions, taken in the browser " +
         "process) and reason 5 (a renderer is obtained by ordering one, which is how the engine's WASM comes " +
         "to be loaded at all), so the row names both. IT HOLDS NO SEMANTIC AND CANNOT: a Remote, a Receiver, " +
         "an interface broker and a message validator are Chromium's Mojo mapped onto MessagePort — the layer " +
         "knows the TYPES a method declares and nothing about what any of them mean. What it DELETED is two " +
         "hand-written transports: a request-id table, a demultiplexer, an `op` capability list and an error " +
         "convention, written once per boundary and free to drift, plus two copies of the same three " +
         "header-shape rules. Those rules now live once, in mojom.js, as the sentence the validator prints." },
  { f: "mojom.js", zone: "BRIDGE:1,5",
    why: "THE INTERFACE DEFINITIONS — the `.mojom` file. It is DATA rather than logic: four interfaces, their " +
         "versions, their methods' ordinals, and each parameter's type beside the sentence that explains that " +
         "type. It is separate from mojo.js for the reason Chromium separates a generated `*.mojom.js` from " +
         "`bindings.js`, and it is loaded by BOTH realms because an interface exists only if both ends agree " +
         "on it — one description, so a skew is a build that packaged two generations rather than a runtime " +
         "negotiation. The zone follows the transport's: it declares what crosses for reasons 1 and 5." },
  { f: "renderer-host.js", zone: "BRIDGE:5",
    why: "LOADING THE WASM IS REASON 5, and this is where that reason stops being a figure of speech. While " +
         "`createQJS()` runs in the offscreen's OWN realm the untrusted engine is confined by convention — the " +
         "host holds the Module handle and HEAPU8 is an exported view of the whole linear memory, so two " +
         "instances in one realm are a namespace and not a sandbox. This provisions each instance in an " +
         "`<iframe sandbox=\"allow-scripts\">` with no `allow-same-origin`, whose UNIQUE OPAQUE ORIGIN is " +
         "cross-origin to the extension origin and is what lets Site Isolation give it its own renderer " +
         "process. Everything it holds is the load and the relay: it hands the frame check.js, the engine glue " +
         "and the wasm BYTES (an opaque-origin frame can load none of them itself — the manifest's " +
         "require-corp plus Chromium's CORP/ACAO-for-web-accessible-only rule sees to that), then carries the " +
         "qjs_* ABI across a MessagePort in `M.ccall`'s own shape. No analysis logic, no policy, no key: the " +
         "pool that owns the agent-cluster question passes the name in. IT SERVES THE PRODUCT AND NOT A PROBE: " +
         "bridge.js's engineCreate provisions here, and the sentence above about `createQJS()` running in the " +
         "offscreen's realm is now history rather than a description of the tree — that path is deleted, so " +
         "there is one way to build an instance and it is this one. A reply carries three things beside its " +
         "call id: what the ABI answered, the engine output drained since the last one with the STREAM each " +
         "line came from, and the instance's HEAPU8 length, which is the Level-1 RAM floor's one input and " +
         "the one fact the pool cannot read for itself any more. AND IT IS NOW THE ZYGOTE, which is what stops " +
         "reason 5 from carrying a decision with it: `rendererCreate(name)` is deleted and the only path that " +
         "materializes a frame is this file's `content.mojom.Zygote.ForkRenderer` implementation, called BY the " +
         "browser process. That process holds the registry, mints the routing id and refuses a second renderer " +
         "for one agent cluster; this file cannot decide, and that process cannot create — a dedicated Worker's " +
         "global has no `document`. Which is exactly why Chromium's browser process asks a zygote to fork." },
  { f: "browser-process-host.js", zone: "BRIDGE:1,5",
    why: "THE TRUSTED SIDE OF THE BROWSER PROCESS — renderer-host.js's counterpart, facing the other way. It " +
         "provisions extension/browser-process.js as a dedicated module Worker (its own realm, its own module " +
         "instance, its own thread, and no HEAPU8 held over it by anybody), accepts its Mojo invitation, and " +
         "holds the Remotes this document reaches it through: the CORB and classification decisions " +
         "safe-fetch.js and response-decode.js take, and the RendererHost the pool asks for an instance. That " +
         "is reason 1 (the fetch chokepoint's own check) reached through reason 5 (loading the WASM), which is " +
         "why the row names both. WHAT IT DOES NOT HOLD is the decision: §7 and Chromium's CORB analyzer are C " +
         "in another program, and what stays on this side is the ORIGIN COMPARISON alone, because SECURITY.md " +
         "makes the same-origin principal the browser's `MessageSender.origin` and forbids re-deriving it from " +
         "a URL. A browser-stated boolean crosses; no URL and no principal ever do. WHAT LEFT THIS FILE is the " +
         "transport: `bpCall`'s request-id table, `onReply`'s demultiplexer, the `op` list and this side's copy " +
         "of three header-shape rules are deleted, and `mojo.Connection` plus three `bindInterface` calls " +
         "stand where they were." },
  { f: "browser-process.js", zone: "BRIDGE:5",
    why: "THE BROWSER PROCESS's own bootstrap, and the whole of the JS inside that boundary: instantiate the " +
         "module, place a resource header in ITS linear memory, call the named entry, post the record back. " +
         "It is the same role renderer.html's bootstrap plays for a renderer and it is measured the same way " +
         "— a line here that decided anything about a response would be the logic this move exists to delete, " +
         "since the point of a network service is that the algorithm is the standard's and not a zone's. THE " +
         "MEASUREMENT MOVED ONCE, in the right direction: `noSniff` used to arrive as a boolean, which meant " +
         "Fetch's determine-nosniff was being computed in lib/safe-fetch.js as a substring test. Both entries " +
         "now take the HEADER VALUE and network/nosniff.c decides what it means, so this file relays two " +
         "header values, two browser-stated facts and a byte sequence, and interprets none of them. IT ALSO " +
         "HOLDS THE RENDERER REGISTRY, which is the one piece of STATE in this program and is not a semantic " +
         "the engine could own: which agent clusters have an instance, what routing id each was given, and the " +
         "refusal of a second one — SECURITY.md's own one-instance-per-`(browsing-context group, origin)` rule, " +
         "held by the process a browser holds it in. It is here rather than in the offscreen because a registry " +
         "split across two zones is two authorities, and the first time they disagree the result is two heaps " +
         "behind one principal. The transport under all of it is mojo.js; the `{v,id,op}` records this file " +
         "used to build are deleted with it." },
  { f: "lib/safe-fetch.js", zone: "BRIDGE:1",
    why: "THE network chokepoint. SOP/CORS/PNA/CORB/GET-only cannot live inside the untrusted WASM — " +
         "SECURITY.md §Network. THIS ROW WAS FALSE UNTIL THE BODY BECAME BYTES: the file ended in " +
         "`await resp.text()`, which is Fetch §5.2's `text()` — \"run consume body with this and UTF-8 " +
         "decode\" — so a BRIDGE:1 row was carrying a SEMANTIC, and the one semantic this architecture can " +
         "least afford here, because a decode run in transit destroys the evidence every downstream assert " +
         "would have needed to catch it. HTML §8.1.4.2's classic-script decode (core/loader/script_fetch.c) " +
         "honours the response's charset LABEL, and it had never once been handed the bytes that label " +
         "describes. The chokepoint now answers a Uint8Array on every path and each consumer runs the decode " +
         "ITS standard names. AND THE LAST CLAUSE OF THIS ROW WAS THE NEXT THING TO GO. It used to defend " +
         "CORB's byte sniff as \"a CHECK decoding the bytes it judges\", which excused four functions — " +
         "`_jsMime`, `_corbProtectedMime`, `_sniffsProtected`, `_corbAllowsScript` — that between them were a " +
         "hand-rolled HTML JavaScript-MIME-type list, a hand-rolled CORB-protected set, and a body sniff " +
         "taking `charAt(0) === \"<\"` for markup and a `JSON.parse` of the WHOLE body for JSON. A check is " +
         "still an algorithm over content when it is one, and this one had a standard: WHATWG MIME Sniffing " +
         "§7, which in a real browser runs in the NETWORK SERVICE. All four are deleted and the decision is " +
         "asked of engine/host/browser_process/network/{mime_sniff,corb}.c across browser-process-host.js. " +
         "What keeps this row BRIDGE is now literal: every remaining rule is decided from the URL, the " +
         "principal and the headers, and the one fact about the body that this file still states is which " +
         "BYTES to hand over." },
  { f: "lib/persistence.js", zone: "BRIDGE:2",
    why: "IndexedDB open/get/put/clear. The WASM cannot reach IDB." },
  { f: "background.js", zone: "BRIDGE:3",
    why: "stateless SW: owns the offscreen document's lifecycle and performs the chrome.tabs.*/webNavigation.* calls the offscreen cannot." },
  { f: "content.js", zone: "BRIDGE:3",
    why: "the renderer edge: ships the document's own HTML + relays intercept.js bodies + performs page-context fetches. Only a content script can be in the page." },
  { f: "intercept.js", zone: "BRIDGE:3",
    why: "main-world fetch/XHR/WS/postMessage capture + the apiclientsink PoC-fire hook. Only main-world JS can wrap the page's own globals." },
  { f: "check.js", zone: "BRIDGE:3",
    why: "the JS mirror of engine/host/check.h. It exists exactly as long as bridge JS exists, and goes out with the last of it." },
  { f: "popup.js", zone: "BRIDGE:4", why: "the popup's DOM controller." },
  { f: "lib/popup-form.js", zone: "BRIDGE:4", why: "Send-panel field UI." },
  { f: "lib/popup-response.js", zone: "BRIDGE:4", why: "response rendering." },
  { f: "lib/popup-send.js", zone: "BRIDGE:4", why: "Send-panel selectors." },
  { f: "lib/popup-gql.js", zone: "BRIDGE:4", why: "GraphQL operation editor." },
  { f: "lib/popup-mp.js", zone: "BRIDGE:4", why: "multipart sub-part editor." },
  { f: "lib/popup-protocols.js", zone: "BRIDGE:4", why: "per-protocol response renderers." },
  { f: "lib/popup-reqlog.js", zone: "BRIDGE:4", why: "virtual-scrolled request log." },
  { f: "lib/popup-security.js", zone: "BRIDGE:4", why: "@S finding cards + the sandboxed live-verify driver." },
  { f: "lib/popup-console.js", zone: "BRIDGE:4", why: "WS/postMessage console." },

  // ── LOGIC / MIXED — the queue. No row carries a step; the order under it is derived. ──────────────────
  { f: "offscreen-brain.js", zone: "MIXED", dest: "engine/host/solver/moat.c + bridge.js",
    split: "globalStore, the request log, _handleFormSubmit and buildExportRequest are LOGIC. buildLiveDelivery is BRIDGE: the delivery VOCABULARY moved into C (each source declares its mechanism beside its encode set in concolic_declare_source; the @S record carries `delivery`+`deliveryPrefix`), so what is left here is the MECHANISM — window.open, the sandboxed attacker page, the real user gesture — which is exactly what a bridge edge is. It replaced buildPocFromShape, which matched a host-side {hash}|{search}|{pm}|{reply} taxonomy against a `shape` field the engine has never emitted. The chrome.runtime.onMessage router + the sender.tab.url trust gate are BRIDGE:3 and fold into bridge.js. _dispatchDocument (what step 0 left of lib/analyze.js) is BRIDGE:3 with it: it reads the browser-stated facts off the document's own buffer, builds the AST_ANALYZE message and hands it to the ONE pool — every field it carries is asserted on arrival in bridge.js." },
  { f: "lib/response-decode.js", zone: "MIXED", dest: "engine/host/solver/moat.c + bridge.js",
    split: "THE ROW THAT WAS HERE WAS FALSE ABOUT THIS TREE IN BOTH FIELDS, and it is the reason a row is now " +
         "checked against the graph. It read \"LOGIC → reply_decode.c: the protocol-chain unwrap that drives " +
         "the two above\", and this file contains no protocol-chain unwrap: the isGrpcWeb/isSSE/isNDJSON/" +
         "isMultipartBatch/isGraphQLUrl dispatch is `learnFromResponse` in lib/learn.js. What it actually " +
         "holds is the LIVE-CAPTURE INTAKE — one `handleResponseBody` that the offscreen's chrome.runtime " +
         "router hands every body intercept.js caught. reply_decode.c is the wrong destination for the same " +
         "reason: that component reads a reply THE ENGINE FETCHED, at the one point every one of them crosses, " +
         "and nothing here has ever been handed one. The LOGIC half is moat aggregation over a captured " +
         "exchange — the request-body decode (protobuf / JSPB / f.req / structural JSON), the key extraction, " +
         "the endpoint lastSeen and 403 `WWW-Authenticate` scope reads, the entry the learners are given. The " +
         "BRIDGE half is the intake itself: `_pushGlobalLog`, `notifyPopup`, the WS/postMessage/MessageChannel " +
         "log entries, and the one `await self.browserProcessClassify` that asks the browser process what a " +
         "body is." },
  { f: "lib/learn.js", zone: "LOGIC", dest: "engine/host/solver/moat.c",
    why: "THE moat aggregation — CLAUDE.md §Architecture names moat aggregation as the engine's. It holds the " +
         "protocol-chain unwrap the response-decode row used to claim: `learnFromResponse` is what dispatches " +
         "a body to gRPC-Web / SSE / NDJSON / batchexecute / multipart / GraphQL and turns each into a SCHEMA." },
  { f: "lib/merge.js", zone: "LOGIC", dest: "engine/host/solver/moat.c",
    why: "@RESULT → doc-model merge. The engine already dedups its own @RESULT; this is the last host-side re-merge." },
  { f: "lib/schema.js", zone: "LOGIC", dest: "engine/host/solver/moat_schema.c",
    why: "schema inference over decoded JSON/JSPB — what every branch of learn.js's unwrap ends in." },
  { f: "lib/grouping.js", zone: "LOGIC", dest: "engine/host/solver/endpoint.c",
    why: "endpoint identity/service grouping — the component already exists in C and already owns dedup." },
  { f: "lib/keys.js", zone: "LOGIC", dest: "engine/host/solver/moat.c", why: "credential/token extraction from decoded bodies." },
  { f: "lib/serialize.js", zone: "LOGIC", dest: "engine/host/solver/result.c",
    why: "the popup snapshot IS the engine's result view once the moat is in C." },
  { f: "lib/send.js", zone: "LOGIC", dest: "engine/host/solver/moat.c",
    why: "schema resolution + type coercion for the manual replay, driven by a popup RPC the engine cannot answer until the moat is in C." },
  { f: "lib/encode.js", zone: "LOGIC", dest: "engine/host/solver/reply_decode.c", why: "the encode direction of the codec, driven by the same popup RPC." },
  { f: "lib/popup-handlers.js", zone: "MIXED", dest: "bridge.js",
    split: "runs in the OFFSCREEN, not the popup, so it cannot claim reason 4: it is the popup RPC surface. The dispatch is BRIDGE:3; every per-command backend it calls is LOGIC that moves with the unit it belongs to." },
  { f: "lib/protobuf.js", zone: "LOGIC", dest: "engine/host/solver/reply_decode.c",
    why: "a wire CODEC. §A JS-engine encoding builtin is modeled FAITHFULLY — the engine runs the real codec; " +
         "a second one in JS is the redundant layer. IT IS A LEAF AND THEREFORE LAST, which is the correction " +
         "the derived order makes: it calls nothing else in this queue and TWELVE files call it, so moving it " +
         "first would put every one of those calls across the boundary. Two of the twelve are not even in the " +
         "queue — content.js and intercept.js use `uint8ToBase64` to carry a captured binary body over " +
         "chrome.runtime, which is a BRIDGE:3 transport concern and not this codec; that pair is what has to " +
         "stop being answered from here before the rest can follow." },
  { f: "lib/protocol-parsers.js", zone: "LOGIC", dest: "engine/host/solver/reply_decode.c",
    why: "batchExecute/gRPC-Web/SSE/NDJSON/GraphQL framing, and the RESPONSE half of multipart. The " +
         "multipart REQUEST half left: `parseMultipartBatchRequest` read `METHOD /path HTTP/1.1` out of the " +
         "parts of a batch body, and that is the one capability in this file whose consumer was already in C " +
         "— every other parser here turns a body into a SCHEMA (moat_schema.c), this one turned it " +
         "into an ADDRESS, and solver/endpoint.c has taken addresses since before this queue existed. It is " +
         "now engine/host/solver/multipart_batch.c, read at the `fetch()` call site that already records the " +
         "outer request's endpoint, so a batch the FORCED EXECUTION composes surfaces all N of its " +
         "sub-endpoints instead of one. The JS body stayed because it is not the same input: this copy reads " +
         "bodies intercept.js CAPTURED off live traffic (lib/learn.js) and bodies the user EDITS in the " +
         "popup's multipart panel (lib/popup-mp.js), and deleting it would remove the editor with nothing " +
         "replacing it. THAT SENTENCE IS ALSO WHY THIS FILE IS LAST rather than second: it is a leaf, every " +
         "one of its eight consumers is still JS, and the four popup ones must first read a decoded body off " +
         "the engine's result instead of decoding one themselves." },
  { f: "lib/stats.js", zone: "LOGIC", dest: "engine/host/solver/moat_stats.c", why: "per-parameter observation model. A leaf: lib/learn.js is its only caller." },
  { f: "lib/chains.js", zone: "LOGIC", dest: "engine/host/solver/moat.c", why: "response-value → request-param chaining. A leaf of the moat unit." },
  { f: "lib/discovery.js", zone: "LOGIC", dest: "engine/host/solver/moat.c",
    why: "ONE COMPONENT LEFT: the Send panel's schema resolution (findDiscoveryMethod/findMethodById/" +
         "resolveDiscoverySchema), whose consumers are lib/send.js, lib/popup-handlers.js and two popup " +
         "renderers. It is LOGIC and not " +
         "a bridge because it is an ALGORITHM over a discovery document's schema graph — walk `$ref`, flatten " +
         "a method's parameter list, resolve a request body's type — over state the moat owns, and the engine " +
         "already reads that same graph in solver/discovery.c. It cannot go earlier for this ledger's own " +
         "rule: a producer whose consumers still live in JS is not first, and there is no host→engine COMPUTE " +
         "edge for a JS caller to reach a moved callee through, because the engine is the DRIVER and adding " +
         "one would be the orchestration layer inverted. THE OTHER TWO COMPONENTS ARE GONE, and the second " +
         "one is the reason that sentence needs its qualifier. The RSC parser went to the engine at " +
         "engine_provide (`looksLikeRSC` was a body-shape guess §RUN, DON'T MATCH forbids and did not move). " +
         "classifyResponseAsset — a hand-rolled magic-byte table beside WHATWG MIME Sniffing §6, a two-row " +
         "markup test beside §7.1's nineteen, and SVG/CSS/WebVTT/HLS/DASH sniffs no standard has — went to " +
         "the BROWSER PROCESS (browser_process/network/resource_kind.c), and the version of this row that " +
         "kept it said it could not, for the compute-edge reason above. That reason is about the ENGINE and " +
         "does not reach this destination: the browser process is a program the TRUSTED ZONE CALLS over a " +
         "postMessage the offscreen already owns, so lib/response-decode.js awaits it and no edge is " +
         "inverted. The ruling it was standing against is one sentence — type checking is safeFetch's job and " +
         "safeFetch is the only source of sniffing — and a second implementation in this file was what that " +
         "forbids." },
  { f: "lib/openapi-import.js", zone: "LOGIC", dest: "engine/host/solver/discovery.c", why: "OpenAPI → discovery conversion; the import half of the discovery component." },
  { f: "lib/openapi-export.js", zone: "LOGIC", dest: "engine/host/solver/result.c", why: "discovery → OpenAPI, a projection of the engine's result." },
];

const files = [];
(function walk(d) {
  for (const e of readdirSync(d, { withFileTypes: true })) {
    /* `lib/qjs` and `lib/bproc` hold LINKED PROGRAMS, not surface JS — engine/build.mjs emits the renderer's
       into one and the browser process's into the other, both gitignored — and `icons` holds no JS at all.
       THE SKIP IS STRUCTURAL AND NOT A FILTER THAT HAPPENS TO WORK: today the collector takes `.js` and
       emscripten's glue is `.mjs`, so nothing in either directory is collected in any case, and a comment
       claiming otherwise would be describing a rule this file does not have. What names them here is that a
       program's output extension is the LINKER'S decision (`-o …mjs` on one line of build.mjs), and a
       classification gate must not depend on it: change that flag and a 100 KB glue file would arrive as a new
       unclassified surface file with a row nobody could honestly write for it. */
    if (e.name === "qjs" || e.name === "bproc" || e.name === "icons") continue;
    const p = join(d, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.name.endsWith(".js")) files.push(relative(ROOT, p).split("\\").join("/"));
  }
})(ROOT);

const text = new Map(files.map((f) => [f, readFileSync(join(ROOT, f), "utf8")]));
const lines = (f) => text.get(f).split("\n").length;
const byFile = new Map(LEDGER.map((r) => [r.f, r]));
const fail = [];

for (const f of files.sort())
  if (!byFile.has(f))
    fail.push(`UNCLASSIFIED  ${f} — no LEDGER row. Every .js on this surface declares whether it is an\n` +
              `              irreducible BRIDGE (and which SECURITY.md reason) or LOGIC (and which C component it becomes).`);
for (const r of LEDGER)
  if (!files.includes(r.f))
    fail.push(`STALE ROW     ${r.f} — the ledger names a file that is not on disk. A row that outlives its file\n` +
              `              reads as authoritative and sends the next reader to move something already gone.`);

/* ── THE CALL GRAPH, READ OFF THE TREE ──────────────────────────────────────────────────────────────────
 *
 * COMMENTS ARE NOT CALLS, so they come out first. A file that says `handleResponseBody` in a paragraph is
 * describing the surface, not using it, and twelve of the graph's edges were exactly that before this ran.
 * Strings STAY: `popup-reqlog.js` calls `isGrpcWeb` from inside a template literal, and a strip that took
 * template substitutions with it would delete a real edge to remove a hypothetical one. An escape is copied
 * whole in both modes — a regex literal spelling `\/\/` would otherwise open a line comment that swallows the
 * rest of the line. */
function codeOnly(src) {
  let out = "";
  for (let i = 0; i < src.length; ) {
    const c = src[i];
    if (c === "\\") { out += src.slice(i, i + 2); i += 2; continue; }
    if (c === "/" && src[i + 1] === "/") { while (i < src.length && src[i] !== "\n") i++; continue; }
    if (c === "/" && src[i + 1] === "*") {
      i += 2;
      while (i < src.length && !(src[i] === "*" && src[i + 1] === "/")) i++;
      i += 2;
      continue;
    }
    if (c === '"' || c === "'" || c === "`") {
      out += c; i++;
      while (i < src.length) {
        if (src[i] === "\\") { out += src.slice(i, i + 2); i += 2; continue; }
        out += src[i];
        if (src[i] === c) { i++; break; }
        i++;
      }
      continue;
    }
    out += c; i++;
  }
  return out;
}

const code = new Map(files.map((f) => [f, codeOnly(text.get(f))]));
/* A TOP-LEVEL declaration is one at COLUMN ZERO. These files are classic scripts in one shared global scope,
   so that position is exactly what makes a name reachable from another file; an indented one is a local and
   carries no edge. */
const DECL = /^(?:async\s+)?(?:function\s+|const\s+|let\s+|var\s+|class\s+)([A-Za-z_$][\w$]*)/gm;
const declares = new Map();
for (const f of files) {
  const s = new Set();
  let m;
  DECL.lastIndex = 0;
  while ((m = DECL.exec(code.get(f)))) s.add(m[1]);
  declares.set(f, s);
}
const owners = new Map();
for (const f of files) for (const n of declares.get(f)) {
  if (!owners.has(n)) owners.set(n, []);
  owners.get(n).push(f);
}

/* A NAME TWO FILES DECLARE CARRIES NO EDGE — there is no way to tell from a use which one it reached, and
   guessing would invent an ordering constraint. It is not necessarily a defect (`ensureOffscreen` is declared
   once in the service worker and once in the offscreen, two realms that never see each other), so it is
   REPORTED rather than failed: a hole in the graph the reader must know about is not the same as a hole the
   reader cannot see. */
const ambiguous = [...owners].filter(([, fs]) => fs.length > 1).map(([n]) => n).sort();

const queued = LEDGER.filter((r) => !r.zone.startsWith("BRIDGE") && files.includes(r.f)).map((r) => r.f);
const consumers = new Map();   // queued file -> every surface file that names one of its top-level declarations
for (const callee of queued) {
  const names = [...declares.get(callee)].filter((n) => owners.get(n).length === 1)
                                         .map((n) => n.replace(/[$]/g, "\\$"));
  const re = names.length ? new RegExp("\\b(?:" + names.join("|") + ")\\b") : null;
  consumers.set(callee, files.filter((g) => g !== callee && re && re.test(code.get(g))).sort());
}

/* ── UNITS, AND THE ORDER THEY MOVE IN ──────────────────────────────────────────────────────────────────
   Tarjan over the queued-only subgraph: a strongly-connected component is a set of files that reach each
   other, which for classic scripts means they are ONE component of the program and there is no order among
   them to find. Then a unit's position is one past the deepest unit that CALLS it, so every caller moves
   first and no call is left crossing the boundary. */
const calls = new Map(queued.map((f) => [f, new Set()]));
for (const callee of queued)
  for (const c of consumers.get(callee)) if (calls.has(c)) calls.get(c).add(callee);

const index = new Map(), low = new Map(), onStack = new Set(), stack = [], units = [];
let counter = 0;
(function tarjan() {
  const visit = (v) => {
    index.set(v, counter); low.set(v, counter); counter++;
    stack.push(v); onStack.add(v);
    for (const w of calls.get(v)) {
      if (!index.has(w)) { visit(w); low.set(v, Math.min(low.get(v), low.get(w))); }
      else if (onStack.has(w)) low.set(v, Math.min(low.get(v), index.get(w)));
    }
    if (low.get(v) === index.get(v)) {
      const u = [];
      let w;
      do { w = stack.pop(); onStack.delete(w); u.push(w); } while (w !== v);
      units.push(u.sort());
    }
  };
  for (const v of queued) if (!index.has(v)) visit(v);
})();

const unitOf = new Map();
units.forEach((u, i) => u.forEach((f) => unitOf.set(f, i)));
const callersOf = new Map(units.map((_, i) => [i, new Set()]));
for (const [a, bs] of calls)
  for (const b of bs) if (unitOf.get(a) !== unitOf.get(b)) callersOf.get(unitOf.get(b)).add(unitOf.get(a));
const depth = new Map();
const depthOf = (i) => {
  if (depth.has(i)) return depth.get(i);
  depth.set(i, 0);
  let d = 0;
  for (const c of callersOf.get(i)) d = Math.max(d, depthOf(c) + 1);
  depth.set(i, d);
  return d;
};
units.forEach((_, i) => depthOf(i));
const order = [...units.keys()].sort((a, b) => depthOf(a) - depthOf(b) || units[a][0].localeCompare(units[b][0]));

const bridge = LEDGER.filter((r) => r.zone.startsWith("BRIDGE") && files.includes(r.f));
const tot = (fs) => fs.reduce((n, f) => n + lines(f), 0);

console.log(`\nextension/ JS surface: ${files.length} files, ${files.reduce((n, f) => n + lines(f), 0)} lines`);
console.log(`  BRIDGE (irreducible): ${bridge.length} files, ${tot(bridge.map((r) => r.f))} lines`);
console.log(`  LOGIC/MIXED (queued): ${queued.length} files, ${tot(queued)} lines`);
if (ambiguous.length)
  console.log(`  no edge (declared in more than one file): ${ambiguous.join(", ")}`);
console.log("");

let shown = 0;
for (const i of order) {
  shown++;
  const u = units[i];
  console.log(`--- step ${shown} --- ${u.length > 1
      ? `${u.length} files, ONE unit: they name each other's globals, so no order among them exists`
      : "one file"}`);
  for (const f of u) {
    const r = byFile.get(f);
    console.log(`  ${String(lines(f)).padStart(5)}  ${f.padEnd(28)} → ${r.dest}`);
    console.log(`         ${r.why || r.split}`);
    const outside = consumers.get(f).filter((c) => unitOf.get(c) !== i);
    console.log(`         called from: ${outside.length ? outside.join(" ") : "nothing outside this unit"}`);
  }
}

if (queued.length) {
  const head = units[order[0]];
  fail.push(`QUEUE NON-EMPTY  ${queued.length} file(s), ${tot(queued)} lines of semantics still in JS. CLAUDE.md\n` +
            `                 §Architecture: "A JS orchestration layer is unwanted context-switching across the\n` +
            `                 JS↔WASM boundary; DELETE it."\n` +
            `                 Take step 1 — ${head.length > 1 ? `${head.length} files, one unit` : head[0]}.` +
            ` Nothing may precede it: every file below it is called\n` +
            `                 by something inside it, so moving one of those first only lengthens the boundary.\n` +
            `                 Inside the unit, what moves is a CAPABILITY whose consumer is ALREADY IN C — the\n` +
            `                 test the three landed moves passed. When the queue empties, this gate goes green\n` +
            `                 and is deleted with the last row.`);
}

if (fail.length) { console.error("\n" + fail.join("\n") + "\n"); process.exit(1); }
console.log("\nJS surface is bridge-only. Delete extension/jsaudit.mjs.\n");
