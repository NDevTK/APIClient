/* bridge.js — the v2 HOST BRIDGE, the ONLY irreducible trusted-zone JS (SECURITY.md).
 *
 * Extracted OUT of offscreen-brain.js so the bridge and the (to-be-deleted) analysis logic are no
 * longer in one file. This is the platform edge the lexbor+quickjs engine CANNOT be, because the engine
 * is the UNTRUSTED WASM: it drives the qjs_step protocol, safe-fetches replies/chunks (the safeFetch
 * chokepoint the untrusted bundle must never bypass), persists the cross-session frontier to IndexedDB,
 * and JSON.parses the engine's ONE @RESULT. It installs self.astDispatch (+ self.kickHostPool) for the
 * offscreen to call. NO analysis LOGIC here — the C engine owns identity/dedup/detection; this is only
 * the network/IDB/renderer edges.
 *
 * NO ENGINE LIVES IN THIS REALM. This file used to `import("./lib/qjs/qjs.mjs")` and build every instance
 * with `createQJS()` right here, which made SECURITY.md's "confined to the WASM sandbox + a fixed set of host
 * edges" a convention rather than a boundary: the offscreen held the `Module` handle, `M.HEAPU8` was an
 * exported view of an untrusted instance's whole linear memory, and two instances in one realm were a
 * NAMESPACE. Every instance is now a RENDERER — renderer-host.js provisions it in an
 * `<iframe sandbox="allow-scripts">` whose unique opaque origin is cross-origin to the extension origin, which
 * is what lets Site Isolation put it in its own renderer process — and every qjs_* call is an `await`ed method
 * of `content.mojom.Renderer`, a mojom interface whose every parameter is declared and validated at both ends
 * of a MessagePort. The in-realm path is DELETED rather than kept beside it: while both
 * existed the sandboxed one served nothing and the boundary was a claim about a path production did not take.
 * The consequence to hold on to is that EVERY ABI call is now a suspension point at which another engine's
 * round can interleave, so anything that was atomic because it was two ccalls in a row is atomic here only if
 * it is written to be. */
// The ROOT DOCUMENT NAME for each engine this offscreen owns — see the qjs_init call below. Only the roots are
// named here: a document an engine CREATES (an iframe's child navigable, a popup) names itself "<parent>.<n>",
// which is what lets HTML 4.8.5 create a child navigable inside the insertion steps without asking this zone.
let nextDocumentId = 0;
/* THE RUN LOG AND THE CRASH COUNT, DECLARED HERE SO THEIR ABSENCE IS THIS FILE NOT BEING LOADED and their
   emptiness is no engine having finalized / no engine having aborted. linesToAnalysis appends one record per
   finalized session, crashBanner counts every abort path, and rendererPoolProbe reads both (the log also
   answers the popup's GET_ENGINE_RUNS). The log holds page ADDRESSES, so hostClear empties it and the crash
   count stays — see the Clear path for why those two differ. */
self._engineLog = [];
self._engineCrashOccurred = 0;

/* A URL with an opaque HOLE ("{}"/"{tag}") is not concretely fetchable (used to gate reply/chunk fetch).
   Endpoint IDENTITY (hole-normalization, shape/concrete collapse) is the ENGINE's now — the host's
   normHoles/SEG_HOLE/pathSegs/mergeCallsites/dedupShapeConcrete were DELETED. */
const HOLE = /\{[a-z]*\}/;
const hasHole = (s) => HOLE.test(s || "");

/* Map the engine's ONE structured `@RESULT <json>` line -> the analysis object the brain consumes.
   The ENGINE builds + DEDUPS the whole result (endpoints/params/headers/body, @S sinks, page errors,
   park recipes) and JSON.stringifies it; the host does ONE JSON.parse and relays — NO per-line
   @H/@Q/@HDR/@BODY parsing, NO host identity/dedup (all DELETED, the engine owns it like a browser).
   @E lines (host-side protocol errors) are still surfaced so a zero-result never fails silently.

   `expectResult` IS THE CONTRACT, STATED BY THE CALLER. A finalize and a partial snapshot both READ the one
   result document the engine builds, so its absence there is a broken engine↔JS contract; a CRASH record has
   no document by construction (the instance aborted before it could answer), and that is the only caller that
   passes false. Without the parameter the two cases were indistinguishable and the missing document took the
   `|| {}` path below — which is how an engine that answered nothing at all became a successful analysis
   reporting no endpoints and no sinks. */
/* WHAT THE ENGINE GUARANTEES IN THAT DOCUMENT — solver/result.c's composition, field for field. Asserted at
   the seam rather than trusted, because every one of these is read below and a missing one becomes an EMPTY
   FINDING SET reported to the user as a clean page. The sibling fields the brain reads that the engine does
   NOT emit are host-side empties (see the map) and are not part of this. */
function assertResultDocument(r) {
  DCHECK(r && typeof r === "object" && !Array.isArray(r),
         "the engine's @RESULT line is not a JSON object — result_json emits one document per session");
  DCHECK(Array.isArray(r.fetchCallSites),
         "the engine's result document carries no fetchCallSites array — endpoint.c serializes its deduped " +
         "endpoints into that field, so its absence is the whole learned API surface arriving as nothing");
  DCHECK(Array.isArray(r.securitySinks),
         "the engine's result document carries no securitySinks array — solve.c serializes its fire-verified " +
         "PoCs into that field, and a missing one reports an exploitable page as clean");
  DCHECK(Array.isArray(r.pageErrors),
         "the engine's result document carries no pageErrors array — it is the engine's own record of what " +
         "went wrong while running the page, and the analysis reports it as the run's resolverErrors");
  /* THE EIGHT COST COUNTERS ARE ONE FIELD, because result.c writes them in ONE snprintf — there is no arm in
     which five arrive and three do not. So the contract is all-of-them, and asserting a subset of a set that
     is emitted atomically is not a weaker check, it is a check on the wrong thing: it passes for exactly the
     document shapes it was meant to reject. `_switches` alone was asserted here while the other six were read
     below with a `|| 0` beside each, which is the file's own recorded defect (see `_orphans` at the return)
     re-spelt: a name the engine stops writing becomes a zero the diagnostic reports forever. The seam's live
     table and its cumulative history are two of the eight and NOT one field named twice — result.c says why. */
  for (const k of ["_switches", "_flows", "_candidates", "_jobsQueued", "_jobsRun",
                   "_worldSegmentsHeld", "_worldSegmentsMade", "_worldSegmentsForked"]) {
    DCHECK(typeof r[k] === "number",
           "the engine's result document carries no " + k + " count — solver/result.c emits all eight cost " +
           "counters in one snprintf, so a missing one is that composition having changed under this seam, " +
           "not a run that happened not to do the thing. They are the only OBSERVABLE that the single BFS " +
           "context-switches, forks and pumps jobs rather than running its flows FIFO");
  }
  DCHECK(Array.isArray(r._park),
         "the engine's result document carries no _park array — that is the PARKED RESIDUE (solver/cold.h), " +
         "the recipes this zone writes to IndexedDB and hands back to qjs_begin next session. An absent one " +
         "reads exactly like a fully-explored document, so every flow the engine paged out would be dropped " +
         "here and the cross-session frontier would silently restart from the boot flow on every visit");
}
function linesToAnalysis(lines, msg, expectResult) {
  let result = null;
  const extraErrors = [];
  let resumed = 0;
  for (const raw of lines) {
    const ln = String(raw);
    if (ln.startsWith("@RESUMED ")) { resumed = parseInt(ln.slice(9), 10) || 0; continue; }
    if (ln.startsWith("@RESULT ")) {
      try { result = JSON.parse(ln.slice(8)); }
      catch (e) {
        /* result.c ASSERTS its own document against buffer truncation before handing it over, so text that
           will not parse here is the engine's output being wrong — not a page that did something unusual.
           Dev aborts; release keeps the parse error in the run's errors, because in release there is no
           document to read and a zero-result must still say why. */
        DFAIL("the engine emitted an @RESULT line that is not JSON: " + String(e && e.message || e));
        extraErrors.push({ context: "result-parse", message: String(e && e.message || e) });
      }
    } else if (ln.startsWith("@E ")) {
      extraErrors.push({ context: "engine", message: ln.slice(3) });
    } else if (ln.startsWith("@WHY ")) {
      // engine diagnostic for a zero-result/resource path (e.g. reg_oom) — surface it so an OOM or
      // aborted flow never fails SILENTLY (CLAUDE.md: every zero-result path emits @WHY).
      try { const o = JSON.parse(ln.slice(5)); extraErrors.push({ context: o.phase || "why", message: o.reason || ln.slice(5) }); }
      catch (_) { extraErrors.push({ context: "why", message: ln.slice(5) }); }
    }
  }
  /* THE DOCUMENT IS EITHER THERE OR THIS CALLER SAID IT WOULD NOT BE. `result || {}` on its own is the exact
     shape the rule forbids: a malformed (here, ABSENT) engine answer defaulted into a plausible one, and the
     plausible one is "this page has no API surface and no XSS" — a wrong FINDING shown to the user rather than
     a crash. The default survives only as the release path under the assert. */
  DCHECK(!expectResult || result !== null,
         "an engine session produced no @RESULT document at all — the one result document is what every " +
         "finding for this page travels in, and reporting its absence as an empty page is a false clean bill");
  if (expectResult && result) assertResultDocument(result);
  /* THE TWO CASES ARE WRITTEN AS TWO CASES. `result = result || {}` merged them into one, and everything after
     it then had to read a document that might not be there — which is where each `|| 0` and `|| []` below came
     from, one per field, each individually reasonable and collectively the defaulting the rule forbids. With
     the absent case handled ONCE, on its own arm, every read on the present arm is a read off a document
     `assertResultDocument` has already checked field for field, so a default beside it can only ever hide that
     assert being wrong. There are none left. */
  const m = result
    /* THE SCHEDULER'S OWN COUNTERS, so fairness/deep-preemption is OBSERVABLE (a real signal that the single
       BFS context-switches rather than running FIFO) — and they are the fields solver/result.c ACTUALLY emits.
       NOT wrapped in a swallowing try/catch: every value here is a number off a document asserted above. */
    ? { switches: result._switches, flows: result._flows, candidates: result._candidates,
        jobsQueued: result._jobsQueued, jobsRun: result._jobsRun,
        worldSegmentsHeld: result._worldSegmentsHeld, worldSegmentsMade: result._worldSegmentsMade,
        worldSegmentsForked: result._worldSegmentsForked,
        /* WHAT THE RUN ACTUALLY LEARNED, beside what it cost. The counters above say the BFS switched, forked
           and pumped jobs; these two say it produced something, which is the only question a probe watching an
           engine that now lives behind a frame boundary can ask without reaching into the moat. Both arrays are
           asserted field-for-field above, so there is nothing to default; the crash arm carries neither,
           because a run with no result document has no surface to have found and a zero there would read as a
           page that was analysed and was clean. */
        endpoints: result.fetchCallSites.length, sinks: result.securitySinks.length,
        park: result._park.length, resumed: resumed, url: (msg && msg.sourceUrl) || "" }
    /* A CRASH RECORD HAS NO DOCUMENT BY CONSTRUCTION, and the honest report of that is the ABSENCE, not seven
       zeroes. Zeroes here read as "the engine ran and did nothing" — indistinguishable in the log from a real
       run that explored nothing, which is a finding. `crashed` is the field that keeps the two apart, and the
       counters are simply not present: nothing may compute a rate, a delta or a total out of a run that never
       reported one. This arm is reachable only where the caller passed expectResult=false; the DCHECK above is
       what makes any other path here a crash rather than a silent second producer of empty runs. */
    : { crashed: true, resumed: resumed, url: (msg && msg.sourceUrl) || "" };
  // A per-run LOG (not a single overwritten global): concurrent cold-kick engines each report here, so the
  // full park->persist->rehydrate->resume SEQUENCE across all engines is observable, not just the last one.
  /* AND THERE IS NO `self._engineMeta` BESIDE IT. It held the LAST record — a page URL and its counters — in a
     global nothing has ever read: not this file, not the popup's GET_ENGINE_RUNS, not rendererPoolProbe, not
     testing/. A single overwritten global is the shape the line above says the log replaced, and it survived
     underneath it, holding one page's address for the life of the offscreen with no surface to show it on. */
  /* THE ARRAY IS DECLARED AT LOAD (top of this file), not created on first use: `self._engineLog = self._engineLog || []`
     is the defaulting shape, and here it defaulted the one thing a reader wants to distinguish — an empty log
     because nothing has run from an absent log because this file never loaded. */
  self._engineLog.push(m);
  if (self._engineLog.length > 200) self._engineLog.shift();
  /* THE HOST-SIDE EMPTIES A CRASH RECORD IS MADE OF, named once: there is no engine answer to default, so
     every field is the host's own empty and the engine's `_park` is absent rather than zero. The engine's own
     protocol errors still travel, because @E and @WHY are emitted on their own lines and do not ride the
     result document; that is the whole reason a crash record is worth returning at all. */
  const engineDoc = result || { fetchCallSites: [], securitySinks: [], pageErrors: [], _park: [] };
  /* THIS DOCUMENT IS EXACTLY WHAT THE BRAIN READS, AND IT USED TO CARRY FIFTEEN MORE FIELDS THAT NOTHING DID.
     A "sibling fields the brain reads unconditionally, present + empty so it never throws" block held
     protoEnums/protoFieldMaps/dangerousPatterns/esmImportUrls/inRunModuleUrls/domEndpoints/sourceMapTypes/
     sourceMapsByUrl/traceMapsByUrl/valueConstraints/sourceMapUrl/sourceMap, beside `chunkUrls`, `_replyWant`
     and `_switches` — every one of them a constant the ENGINE has never written. The merge passes that read
     them were deleted (lib/merge.js records four of them by name), and the writers outlived the readers, which
     is the §FIELD-A-CONSUMER-DEFAULTS defect with the arrow reversed: `dangerousPatterns` crossed two
     boundaries into `tab._securityFindings`, and a `.length` of 0 there is indistinguishable from "no
     dangerous patterns found". A constant `[]` is not a measurement of a page, so it is not shipped as one.
     (testing/extractors.js and testing/classify.js read dangerousPatterns/valueConstraints/protoEnums/
     protoFieldMaps/sourceMapUrl off the analysis with `|| []`; they were already reading the constant, so they
     read the same nothing, and testing/test-spec.js's two `domEndpoints` assertions fail exactly as they did —
     they are the record that the DOM-attribute scan is an ENGINE capability nobody has built.) */
  return {
    fetchCallSites: engineDoc.fetchCallSites,
    securitySinks: engineDoc.securitySinks,
    /* THE ENGINE'S OWN PAGE ERRORS, WHICH IT CALLS `pageErrors`. This line read `resolverErrors` — a name
       nothing on the engine side has ever written — so every error the engine recorded while running the page
       was dropped here, and the `|| []` beside it is precisely what made the drop invisible: the field the
       brain reads existed, held the host's own errors, and looked complete.
       A ROW IS {context, message}, TWO NON-EMPTY STRINGS, AND NOTHING ELSE. It carried `snippet` and
       `replyExample` as constant `null`s: offscreen-brain dropped `replyExample` on the way through, no
       renderer ever read either, and the engine has no source-text or reply to state for a page error in the
       first place (result.c's pageErrors are strings). Three comments — here, in lib/serialize.js and in
       popup.js — named `snippet` as part of the contract the popup reads; it was read nowhere. */
    resolverErrors: engineDoc.pageErrors.map((e) => ({ context: "page", message: String(e) })).concat(extraErrors),
    /* NO probeResults ON THIS SEAM. The engine issues no request, so it receives no rejection and has no
       error-derived schema to relay; the record the Send panel reads is written by the two systems that DO
       probe — lib/req2proto.js (driven by lib/discovery-probe.js and lib/response-decode.js) — straight into
       `globalStore.probeResults`, which never crossed this boundary. */
    sourceUrl: (msg && msg.sourceUrl) || "",
    _park: engineDoc._park,
  };
}

/* Run one page through a fresh v2 engine instance, capturing @H/@CHUNK stdout. The ENGINE parses the
   page HTML with its in-wasm Lexbor DOM and runs the document's scripts in order (against the real
   DOM) — the bridge no longer scrapes scripts, and `html` is the whole of what qjs_init takes for a
   document. `code` is carried here for the AST_ANALYZE consumer below and is NOT handed to the engine:
   it used to be, and the engine cast it away, so a caller that put extra scripts in it was silently
   running none of them. */
function originOf(u) { try { return new URL(u).origin; } catch (_) { return ""; } }
/* THE ORIGIN A DELIVERED MESSAGE IS STAMPED WITH, serialized the way HTML serializes one. SECURITY.md:
   "identity may be minted by the untrusted side because it is only a name, but ROUTING and the ORIGIN
   STAMPED ON A DELIVERED MESSAGE are the trusted zone's alone. A forgeable `event.origin` would defeat
   every origin check in every bundle the engine analyses — it would report exploits that are not real and
   miss ones that are."
   The value this zone HOLDS for an opaque document is `_senderOrigin`'s per-document uniqueness token
   ("null:<uuid>"), which exists so two opaque documents never compare same-origin. That token is an
   internal identity and must never reach a page: HTML §7.5 serializes an opaque origin as the literal
   "null", which is what a real browser puts in `event.origin`, and a bundle testing `e.origin === "null"`
   (the ordinary "accept my sandboxed widget" check) would otherwise never match. So the comparison value
   stays internal and the DELIVERED value is the serialization. */
function stampOrigin(o) { return _isRealOrigin(o) ? o : "null"; }
/* THE AGENT CLUSTER AN INSTANCE IS — SECURITY.md's `(browsing-context group, origin)` — computed HERE because
   both halves are BROWSER-STATED and the untrusted engine may state neither.

   IT REPLACES A PER-DOCUMENT KEY, AND THAT IS NOT A LOOSENING. `admit` used to ask "does an instance already
   hold this exact documentId?", so a page and its SAME-ORIGIN iframe — one similar-origin window agent, which
   HTML puts in ONE heap and then RELIES on it — were handed two WASM instances and two heaps. In that split
   `iframe.contentDocument.body.appendChild(x)` cannot be what §4.5 says it is: adopt-then-insert leaves
   `x.parentNode` a node belonging to the OTHER document while the wrapper stays the same object, and
   `frame.contentWindow.onunload = fn` hands that document this agent's live CLOSURE to call. Neither is a
   property read that could suspend across an instance boundary — they are one object graph, and passing them
   by name is an unbounded bidirectional export table whose `===` answers wrong. The engine has always modelled
   this correctly (navigable.c's `child_in_this_agent` builds a same-origin child as a second REALM in the same
   JSRuntime and only a CROSS-origin one as a peer instance); it was this zone that split what the engine joins.
   The security invariant is untouched, because it was never keyed on the document: one instance still holds
   exactly one ORIGIN, so a fetch out of it still has exactly one principal.

   THE GROUP HALF IS `sender.tab.id`. No extension API exposes a browsing-context group; a TAB is exactly one
   top-level traversable and every nested navigable under it is in that traversable's group, so the tab id is
   the browser-stated fact closest to the group, and — the direction that matters — it never JOINS two documents
   that are in different groups, which is what would put two principals behind one instance. RESIDUAL, named
   because it is a NARROWING and narrowing is the wrong direction: an auxiliary browsing context opened by
   `window.open()` without `noopener` is in the SAME group as its opener and lands in ANOTHER tab, so this key
   splits that pair. Closing it needs `openerTabId`, which `MessageSender` does not carry. (An auxiliary the
   ENGINE opens never travels this path — it is provisioned from the create notice below, which takes its
   CREATOR's group precisely so that this residual does not reach the case the engine can see.)

   THE ORIGIN HALF IS `_senderOrigin`'S, AND IT IS ALREADY OPAQUE-UNIQUE. SECURITY.md: "An opaque origin is
   unique per spec → never same-origin with anything (the `"null"==="null"` collision is closed)." The offscreen
   hands over the browser's `MessageSender.origin` for a tuple origin and a STABLE per-document `"null:<uuid>"`
   token for an opaque one, so two sandboxed iframes of one page land in two clusters and two instances — the
   case a naive `origin === origin` gets wrong, and it costs nothing here because the value that arrives is
   already the right one. It is also why this key is never `originOf(sourceUrl)`: a sandboxed document's address
   parses to a tuple origin the browser refused to give it.

   AN EMPTY ORIGIN HALF BELONGS TO A REHYDRATED COLD RECIPE, which has no live browser document and therefore no
   browser-stated principal to resume with. Its GROUP is its frontier key (`cold:<origin>|<bundle>`), unique per
   recipe and never equal to a tab id, so an empty origin there cannot collide with anything — it is a cluster
   of one, which is the truth about a resumed frontier rather than a default.

   A NUL SEPARATOR, because neither half can contain one. */
function clusterKeyOf(msg) {
  DCHECK(msg && msg.groupId != null && msg.groupId !== "",
         "a document reached the pool naming no browsing-context group — an instance IS its (group, origin) " +
         "agent cluster, so a document with no group would share one cluster (and one heap, and one principal) " +
         "with every document of its origin in every tab the user has open");
  return String(msg && msg.groupId) + "\u0000" + String((msg && msg.origin) || "");
}
/* Stable BUNDLE IDENTITY for the frontier key: the EXTERNAL <script src> set (content-hash filenames
   like main.abc123.js ARE the app version), NOT the volatile HTML wrapper (per-request nonces/CSRF
   tokens would change the key every visit -> the frontier would never resume). A redeploy changes a src
   -> new key -> stale frontier invalidated. Inline-only pages fall back to the HTML hash (rare; they're
   small, finish in one visit, never park). */
/* The document's bundle IDENTITY is computed by the ENGINE (qjs_bundle_id, a real Lexbor <script> scan) —
   NOT a host-side regex. The frontier key = origin | that id, is only a RELEVANCE grouping for which parked
   recipes to pull on resume (recipes self-identify by function SOURCE hash, so a coarse key is sound). */
/* Cross-session flow FRONTIER (IndexedDB): the learned surface (globalStore) already persists; this
   persists the UNFINISHED frontier as compact replay recipes, keyed by origin+bundle-hash (a changed
   bundle invalidates stale orphan indices). A parked frontier resumes next visit/session -> ONE
   continuous attention across sessions, extracting more breadth each time until fully explored. */
/* THE STORE VERSION IS THE ENTRY GRAMMAR'S VERSION, and the upgrade transaction is where a grammar change is
   executed (IndexedDB §5.7 "Upgrading a database", §2.7.3 "Upgrade transactions"). v2 added `responseHeaders`
   (see frontierDoc); a v1 entry cannot state the response its document was created from, so it cannot be
   resumed into the environment it parked in and is DELETED here rather than resumed under a policy container
   nobody delivered. A one-time grammar change, not a frontier reset — and not a default either, which is the
   alternative it replaces: `|| {}` at the read would resume every v1 entry as a page whose server sent no CSP. */
function idbOpen() {
  return new Promise((res, rej) => {
    const r = indexedDB.open("apiclient-frontier", 2);
    r.onupgradeneeded = () => {
      const db = r.result;
      if (db.objectStoreNames.contains("frontier")) db.deleteObjectStore("frontier");
      db.createObjectStore("frontier");
    };
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}
/* A frontier entry (the GLOBAL union spans all origins): { key: origin|hash, sourceUrl, topLevelUrl, origin,
   responseHeaders, html, code, recipes: "idx,dec;...", emit, visits, ts, credentialed }. Rehydration re-runs
   (html,code) + resumes recipes -- so a parked flow on ANY site can be advanced later, even when that page
   isn't open. */
/* THE DOCUMENT HALF OF A COLD-TIER ENTRY, WITH ONE SPELLER FOR BOTH DIRECTIONS. solver/cold.h's recipe is the
   FLOWS — the arms each parked flow took — and those arms replay INSIDE a document, so the entry must also
   carry the document they replay in. That is exactly what `qjs_init` takes: its bytes, its URL (DOM §4.5
   "Interface Document"), the top-level creation URL of HTML §8.1.3.1 "Environments", its browser-stated
   principal, and the RESPONSE HEADER LIST that HTML §7.1.7 "Policy containers" makes this Document's policy
   container (the CSP list, plus §7.1.3 "Cross-origin
   opener policies" and §7.1.4 "Cross-origin embedder policies"; §7.1.2 "Origin-keyed agent clusters" reads
   `Origin-Agent-Cluster` out of the same list). None of it is derivable from any of the rest.
   THE MISMATCH THIS CLOSES IS STRUCTURAL, not one field's. `responseHeaders` was asserted at the engine
   boundary and never parked, so every rehydration reached qjs_init with none and aborted before its document
   had a policy container — the ONE continuous cross-session frontier dead in a dev build, 104 admissions and
   202 crash banners, and the park still reporting that it had stored a resumable residue. A field the engine
   asserts could be added at one end alone because the two ends were two object literals; now the store's
   writer and its reader go through this, so a field written by neither crashes at the park rather than at the
   resume a session later. NOTHING IS DEFAULTED: `{}` is the honest header list of a response that carried no
   headers, so substituting it for a producer that stated none turns a CSP-protected page into an unprotected
   one, and "CSP does not block this sink" is then a reported exploit that is not real. */
function frontierDoc(e, when) {
  DCHECK(e && typeof e === "object",
         "a cold-tier entry " + when + " as no record at all — the entry IS the parked document, so there is " +
         "nothing to resume the recipe's flows inside of");
  DCHECK(typeof e.sourceUrl === "string" && e.sourceUrl !== "",
         "a cold-tier entry " + when + " with no document URL — DOM §4.5 \"Interface Document\" gives every " +
         "Document one, the engine derives this document's principal from it, and every relative URL the " +
         "bundle builds resolves against it");
  DCHECK(typeof e.topLevelUrl === "string" && e.topLevelUrl !== "",
         "a cold-tier entry " + when + " with no top-level creation URL — HTML §8.1.3.1 \"Environments\" " +
         "defines it and §8.1.3.5 \"Secure contexts\" reads it, so the flows would resume against a different " +
         "set of Web IDL §3.3.13 \"[SecureContext]\" members than they parked against; the engine refuses an " +
         "empty one one call later, in the process handed the hole rather than in the zone that made it");
  DCHECK(typeof e.origin === "string",
         "a cold-tier entry " + when + " with no principal — it is browser-stated, this zone cannot re-derive " +
         "it from the address (a sandboxed document's address lies about it), and a resumed instance that " +
         "posts a cross-document message must stamp the origin the parked one had");
  DCHECK(e.responseHeaders && typeof e.responseHeaders === "object",
         "a cold-tier entry " + when + " with no response header list — HTML §7.1.7 \"Policy containers\" " +
         "makes those headers this Document's policy container, so the parked flows would resume under a " +
         "policy nobody delivered and every @S verdict on them would be decided against it");
  DCHECK(e.html instanceof Uint8Array || typeof e.html === "string",
         "a cold-tier entry " + when + " as neither a byte sequence nor characters — the parked document " +
         "would rebuild as a page that parses to nothing, which reads as an origin whose flows found nothing");
  DCHECK(typeof e.code === "string",
         "a cold-tier entry " + when + " with no script inventory — it is the empty string when the document " +
         "carries its own scripts, which is a different thing from absent");
  return e;
}
/* THE COLD TIER'S THREE EDGES, AND WHAT THEIR ERRORS USED TO MEAN. Each was `catch (_) { return <empty> }`
   plus an `onerror` that RESOLVED with an empty answer, so an IndexedDB that refused a read reported the same
   thing an unvisited origin reports — no parked frontier — and an IndexedDB that refused a WRITE reported
   nothing at all. That is the ONE continuous cross-session frontier silently becoming a per-session one: the
   defect has no symptom, because "this page had no parked residue" is exactly what the first visit looks like.
   The failure is surfaced instead. It is a DCHECK rather than a CHECK: in release the user is not crashed
   because their profile's storage is unavailable — they lose cross-session resume, which is degraded, not
   wrong — but in dev this is our own edge failing and it aborts at the line that failed. */
function frontierFail(op, err) {
  DFAIL("the cross-session frontier's IndexedDB " + op + " failed (" + String((err && err.message) || err) +
        ") — the ONE continuous frontier is persisted there, and an empty answer from this edge is " +
        "indistinguishable from a page that has never been visited");
}
async function frontierGet(key) {
  try {
    const db = await idbOpen();
    return await new Promise((res, rej) => { const t = db.transaction("frontier").objectStore("frontier").get(key); t.onsuccess = () => res(t.result || null); t.onerror = () => rej(t.error); });
  } catch (e) { RETHROW_FATAL(e); frontierFail("read", e); return null; }
}
async function frontierPut(key, entry) {
  if (entry && entry.recipes) frontierDoc(entry, "was written");   // before the edge: the grammar, not the storage
  try {
    const db = await idbOpen();
    await new Promise((res, rej) => { const s = db.transaction("frontier", "readwrite").objectStore("frontier"); const t = (entry && entry.recipes) ? s.put(entry, key) : s.delete(key); t.onsuccess = () => res(); t.onerror = () => rej(t.error); });
  } catch (e) { RETHROW_FATAL(e); frontierFail("write", e); }
}
async function frontierAll() {
  try {
    const db = await idbOpen();
    return await new Promise((res, rej) => { const t = db.transaction("frontier").objectStore("frontier").getAll(); t.onsuccess = () => res(t.result || []); t.onerror = () => rej(t.error); });
  } catch (e) { RETHROW_FATAL(e); frontierFail("scan", e); return []; }
}
/* HOST-level value-of-information for a PARKED frontier's rehydration order. It shares the engine's WFQ
   POLICY (rank by value + an exploration bonus, never drop a work item) but NOT the engine's exact formula:
   flow_weight is additive `val + optimism − per-opcode cpu-aging`; a parked frontier has no live per-opcode
   CPU to age by, so its expected FUTURE productivity is best estimated by emit-per-VISIT (efficiency),
   guarded against 0/0 on an unvisited entry (so the spec's anti-ratio point — the 0/0 degeneracy on unrun
   LIVE flows — doesn't apply here). Same attention, two levels; the estimator adapts to each level's
   granularity. The duplicate JS scheduler lib/priority.js was DELETED. */
function frontierWeight(e) {
  const emit = (e && e.emit) || 0, visits = (e && e.visits) || 0;
  const rate = visits ? emit / visits : emit;   // expected emit per rehydration — future productivity, not raw total
  const exploreBonus = 1 / (visits + 1);         // optimism: never starve an unvisited / under-visited frontier
  return 1 + rate + exploreBonus;
}
/* No separate cold-tier scheduler: parked frontiers are rehydrated into the SAME pool by _hostOps.admit
   (ranked by frontierWeight, gated by the RAM budget) when live work drains — ONE WFQ, not two loops. */
/* ────────────────────────────────────────────────────────────────────────────────────────────────────
   HOST-LEVEL WFQ (Level-1 of the ONE attention): interleave the LIVE document engines by value-of-
   information, in SLICES, so no single document (or one deep path within it) monopolizes CPU. Each
   AGENT CLUSTER is one wasm instance (SECURITY.md: one instance per (browsing-context group, origin) — never
   per page, which is what this line used to say and what admit used to key on); the engine exposes its best flow's
   weight (qjs_top_weight) and yields HOT after a slice (qjs_step -> 2). The host ranks all live engines by
   that weight and advances the winner one slice, then re-ranks — the same WFQ policy the C engine runs
   over flows WITHIN a document, now over engines ACROSS documents. THE RANKING DOES NOT ASK: both of its
   inputs — that weight and the instance's working set — are RECORDED by engineRecordFacts at the end of every
   round this zone has with an instance, because an instance behind renderer-host.js's frame boundary answers
   by postMessage and there is no synchronous value to read on the line that ranks. RAM is the bound: at most POOL_CAP hot
   engines resident; the lowest-weight one is EVICTED (qjs_park -> replay recipe in IDB) under pressure, and
   the cold tail is rehydrated INTO this same pool by admit (one WFQ, no second loop). Fetches don't block the pool: an engine awaiting a
   reply is 'fetching' and skipped until its body lands, so a slow fetch on doc A never stalls doc B.
   ──────────────────────────────────────────────────────────────────────────────────────────────────── */
// The hot working set is bounded by ACTUAL RAM, not a fixed instance count: admit a new document engine
// while resident WASM memory is under the budget (a light page's instance is a few MB, a heavy bundle's is
// tens — a count would ignore that). Over the budget, new docs wait as cold recipes -> IDB, pulled back into this pool by admit.
// This is the RAM floor (like the disk floor), not a truncating bound. Always admit >=1 so a lone doc runs.
const HOT_RAM_BUDGET = 512 * 1024 * 1024;   // bytes of summed live WASM memory before new engines wait
/* THE HOT WORKING SET, SUMMED OVER WHAT EACH INSTANCE LAST REPORTED — never reached for across the boundary
   an instance lives behind. This read `e.M.HEAPU8.length` live, through `(e.M && e.M.HEAPU8) ? … : 0` inside a
   `try {} catch (_) {}`: three defaults over one number, and every one of them turns "this instance did not
   report its working set" into "this instance occupies no memory", which admits another engine against RAM
   that is already spoken for. engineRecordFacts writes the number at the end of every round this zone has with
   an instance — the first of them before the record ever reaches the pool — so an absent one is a producer
   that stopped writing rather than an engine of size zero, and it CRASHES.
   A RESERVATION IS NOT A TERM IN THIS SUM, AND ITS ABSENCE FROM IT IS ANSWERED BY A STATE RATHER THAN BY A
   NUMBER. An engine that has not booted has reported nothing, and there is no honest figure to stand in: 0 is
   the exact default this comment already refuses (it admits another engine against RAM that is about to be
   spent), and an invented floor would be a number the producer never stated. So the sum is over the instances
   that HAVE reported, and `_admissionHasHeadroom` — not this function — is where a reservation says "not
   yet", by COUNTING ITSELF rather than by contributing a byte count nobody said. The states are enumerated
   rather than defaulted past: a fourth one has to be classified here before it can be silently summed or
   silently skipped. */
function _residentBytes() {
  let b = 0;
  for (const e of _pool) {
    if (e.state === "booting") continue;   // has reported nothing; _admissionHasHeadroom reads the state instead
    DCHECK(e.state === "hot" || e.state === "fetching",
           "an engine is in the pool in state `" + e.state + "`, which the RAM floor has no classification " +
           "for — it would be summed as if it had reported or skipped as if it never will, and neither is a " +
           "decision this function may take about a state it has never heard of");
    DCHECK(typeof e.residentBytes === "number",
           "a live engine is in the pool with no reported working set — the RAM floor is the sum of what each " +
           "instance last reported, and an engine missing from that sum is admission running against memory " +
           "that is already spent");
    b += e.residentBytes;
  }
  return b;
}
/* THE RESERVATIONS IN FLIGHT — the POSITIVE form of "that sum is not complete yet". */
function _bootingCount() {
  let n = 0;
  for (const e of _pool) if (e.state === "booting") n++;
  return n;
}
/* WHETHER ANOTHER INSTANCE MAY BE BUILT, ASKED IN THE ONE PLACE THAT DECIDES IT. This condition was written out
   three times (the waiting-document loop, the cold-rehydration gate, and that gate's inner break) — three
   copies of a rule that has just gained a term, and a term is exactly what a copy forgets.
   A RESERVATION BLOCKS ADMISSION, and that is the honest reading of an incomplete sum rather than a cautious
   one: while an instance is being provisioned the working set is a LOWER BOUND, so admitting against it is
   admitting against memory that is already spoken for and merely not yet counted. It is not a stall —
   provisioning always settles (a reservation that fails leaves the pool), hostSchedule's wait arm waits on the
   very promise that clears this, and the next iteration re-asks. */
function _admissionHasHeadroom() {
  if (_pool.length === 0) return true;     // always admit >= 1 so a lone document runs
  if (_bootingCount() > 0) return false;   // an instance that has not reported is a term the sum is missing
  return _residentBytes() < HOT_RAM_BUDGET;
}

/* THE NAVIGATION RESPONSE'S HEADER LIST, IN THE ONE FORM THAT CROSSES AN ABI — the HTTP field lines the
   response delivered, `name: value`, one per line. This is a RELAY and not logic: it restates what the browser
   already gave this zone, and every decision made from it (which policy container, which sandboxing flags,
   which agent cluster key) is the engine's, in the browser components that own those standards.
   NOTHING IS DEFAULTED HERE. `h` is written by both producers that reach engineCreate — a content script's
   captured response headers, and the `{}` a child-document notice starts from — so an absent one is a contract
   that changed rather than "a response with no headers", and the empty object already says the second thing.
   A value carrying CR or LF would split into a line the engine cannot read; the browser's `Headers` forbids
   both, so one here means the value did not come off a response and the engine's own CHECK would abort on the
   fragment. It is asserted at the producer's edge instead, where the name of the offending header survives. */
function responseFieldLines(h) {
  DCHECK(h && typeof h === "object",
         "an engine was started with no response header list — HTML §7.5.1 creates a Document from its " +
         "response's headers, and a missing list is a producer that stopped writing one rather than a " +
         "response that carried none, which is the empty object");
  const out = [];
  for (const name of Object.keys(h)) {
    const value = h[name];
    DCHECK(typeof value === "string",
           "a response header value that is not a string reached the engine boundary (" + name + ") — a " +
           "header list holds byte strings, and anything else is a producer writing a shape this relay " +
           "would stringify into a header the server never sent");
    DCHECK(value.indexOf("\n") < 0 && value.indexOf("\r") < 0,
           "a response header value carries CR or LF (" + name + ") — the browser's own Headers forbids both, " +
           "so this value did not come off a response, and splitting it into field lines would present the " +
           "engine with headers nobody delivered");
    out.push(name + ": " + value);
  }
  return out.join("\n");
}

// ---- Engine lifecycle over ONE wasm instance (one document) ----
/* `docName` is set ONLY for a document another engine CREATED — its name arrived in that engine's
   navigable.create notice, minted there because HTML §4.8.5 creates a child navigable inside the insertion
   steps and cannot ask this zone for a name. A root document is named here, by the counter above. */
/* `topLevelUrl` is HTML §8.1.3.1's TOP-LEVEL CREATION URL for this document's environment. A ROOT document is
   its own top-level traversable, so its address is it; a document a peer engine CREATED carries its creator's
   decision on the navigable.create notice. It is a separate argument from the address because one WASM
   instance is one DOCUMENT regardless of origin — this document may be NESTED in a document of another
   instance, and §8.1.3.5 decides secure-context from the TOP of that chain. */
/* PROVISION THE INSTANCE, THEN ROOT IT. Two operations with two failure modes, and separating them is what
   makes the cleanup a line instead of a rule someone follows: the renderer is a FRAME in this document, so
   anything that throws after it exists — the init return code, the bundle id, the frontier key's origin, an
   IndexedDB read, the engine's own abort — leaves an untearable WASM instance parked under a document that
   never reloads unless the throw takes the frame with it. The old path leaked a Module that the collector
   eventually took; a frame is not collected.
   THE RENDERER'S NAME IS THIS INSTANCE'S AGENT CLUSTER. renderer-host.js takes a name and nothing else — it
   does not compute the key, does not admit and does not rank — and this is the zone that owns that question,
   so the name it passes in IS the answer (SECURITY.md's `(browsing-context group, origin)`), which is also
   what makes a frame in the offscreen's DOM identifiable as the instance the pool is talking about. */
function engineCreate(code, html, msg, persist, docName, topLevelUrl, cold) {
  /* THE ONE WAY TO OBTAIN AN INSTANCE, ASSERTED RATHER THAN DISCOVERED AS A TypeError. Without this the failure
     of a load-order change (renderer-host.js is a <script> before this one in ast-worker.html) arrives as
     "self.rendererLaunch is not a function" inside admit's catch, which reports it as a BOOT ABORT of the
     engine — a crash record blaming an instance that was never built, for a document that would then be
     reported as analysed and empty.
     AND `rendererLaunch` IS NOT `rendererCreate`. The name changed because the direction did: this pool no
     longer tells renderer-host.js to make a renderer, it asks for one that THE REGISTRY has decided on.
     `render-process-host.js` holds which agent clusters have an instance, mints the routing id, and refuses a
     second for a cluster that already has one — fatally, in every build — and renderer-host.js materializes
     the frame for the id it is given and can mint none of its own. */
  DCHECK(typeof self.rendererLaunch === "function",
         "renderer-host.js is not loaded in this zone — it is what obtains an instance once the registry has " +
         "admitted its agent cluster, so without it every document would be reported as a crashed instance " +
         "rather than as a bridge that is missing half of itself");
  DCHECK(typeof cold === "boolean",
         "an instance was started without saying whether it has a live caller — `_cold` decides at finalize " +
         "whether this document's findings are RETURNED to a requester or MERGED to the moat, and it is a " +
         "fact about the call site (a child navigable and a resumed recipe have no requester; an admitted " +
         "document does), so it belongs on the record from the instant the pool can see it");
  const cluster = clusterKeyOf(msg);
  /* THE POOL IS THE REGISTER OF WHO HOLDS WHAT, AND ASKING IT IS THE CALLER'S JOB — this asserts they did.
     Every site that builds an instance has already answered "does this agent cluster have one?" (admit's
     `hostClusterOf`, the create notice's, and the cold tier's key, which is unique per recipe by construction),
     so a hit here is a caller that skipped the question or a reservation that did not take. */
  DCHECK(hostClusterOf(cluster) === null,
         "a second instance was started for an agent cluster that already has one (" + cluster + ") — two " +
         "heaps for one similar-origin window agent is the split SECURITY.md's one-instance-per-cluster rule " +
         "exists to forbid, and the pool already held the answer on the line that asked");
  /* THE DOCUMENT ID IS MINTED HERE, BEFORE THE FIRST AWAIT, because the reservation is answerable by NAME from
     the instant it exists: `hostHolderOf` is what routes a cross-document message, and a reservation with no
     name is a document that this zone reports as held by nothing for as long as its instance takes to boot.
     A LIVE ROOT DOCUMENT IS NAMED BY THE NAME THE BROWSER ALREADY GAVE IT. The counter answered "which
     document is this?" with a fresh number every time it was asked, so the pool could not tell one document
     delivered twice (content.js re-ships its CONTENT_HTML when the offscreen brain broadcasts RESHIP) from
     two documents, and built a second WASM instance for it — two instances behind one principal, which is
     the one thing SECURITY.md's "one instance per ORIGIN-KEYED AGENT CLUSTER" forbids. A document a peer
     engine CREATED arrives already named (`docName`, minted in that engine's navigable.create notice); a
     rehydrated cold recipe has no browser document to name it and takes the counter. */
  const docId = docName || (msg && msg.documentId ? String(msg.documentId) : String(++nextDocumentId));
  const eng = engineReserve(cluster, docId, msg, cold);
  /* THE PROVISIONING ITSELF, WHICH IS THE ONLY PART THAT SUSPENDS, AND THE PROMISE IS WHAT SAYS *WHEN*. It
     never carries the instance — the caller already holds the record, which is the point — and it settles on
     BOTH outcomes, because the thing waiting on it is hostSchedule's wait arm and a document that merely
     failed to boot may not take the Level-1 loop down with it. So an ENGINE abort resolves it: that is a
     RECORDED outcome, answered on this record's own callers by engineBootFailed, which is also what takes the
     reservation back out of the pool. The one thing it rejects for is an INVARIANT abort, which is this zone's
     contract with the engine breaking and must not be reported as a page that failed to analyse.
     IT IS ASSIGNED BEFORE ANYTHING CAN OBSERVE THE RECORD: the async body runs synchronously to its first
     await, and this whole function is synchronous, so there is no turn in which the pool holds a reservation
     nothing can wait on. */
  eng._readyP = (async () => {
    try {
      eng.r = await self.rendererLaunch(cluster);
      await engineRoot(eng, code, html, msg, persist, docName, topLevelUrl);
      /* A CLEAR THAT LANDED MID-BOOT TAKES THE FRAME HERE, for the reason serviceFetch takes it there:
         hostClear cannot destroy a renderer this path has a call outstanding on, so it marks the record and
         the operation that owns the outstanding call removes the frame when it lands. */
      if (eng._dropped) eng.r.destroy();
      _reserveStats.rooted++;
    } catch (e) {
      engineBootFailed(eng, e);
      RETHROW_FATAL(e);
    }
  })();
  return eng;
}
/* THE POOL SLOT, TAKEN SYNCHRONOUSLY — the whole point of this record existing before the instance does.
   `hostNotice`'s create arm checked `hostClusterOf` and then AWAITED a document fetch, a frame boot and three
   ABI round trips before pushing, and detached service rounds run concurrently with admit, so two arrivals for
   one agent cluster could both pass that check and both build. Two heaps claiming to be one similar-origin
   window agent is not a duplicated cost: `iframe.contentDocument.body.appendChild(sub)` inserts a node one of
   them created into a tree the other owns, and a live `frame.contentWindow.onunload` closure is a function
   object neither can call, so the pair is unrepresentable rather than slow. The window is closed by taking the
   slot BEFORE the first suspension, so the second arrival finds a record and joins it.
   IT CARRIES ONLY WHAT IS KNOWN WITHOUT ASKING THE INSTANCE, and nothing is stubbed in beside it. There is no
   `residentBytes: 0` and no `topWeight: -Infinity` here: the first is the default that makes admission run
   against memory already spoken for, and the second is the engine's own word for a frontier with no runnable
   flow (355a03d2) — a drained instance, which is a different thing from one that has not started. Both facts
   are ABSENT until engineRecordFacts states them, and every reader of either reads the STATE first. */
function engineReserve(cluster, docId, msg, cold) {
  /* `_readyP` IS DECLARED NULL AND FILLED BY engineCreate ON THE VERY NEXT LINE, which is a stated hole rather
     than a placeholder promise: a stand-in built here would be a second promise that resolves at a different
     moment than the provisioning does, and the wait arm would then be released by whichever of the two the
     next reader happened to reach for. The arm asserts it is there. */
  /* `joinedDocIds` IS THE HOST'S HALF OF main.c's `g_joined_ctx`/`g_joined_dom`, AND IT IS DECLARED EMPTY HERE
     RATHER THAN CREATED ON THE FIRST JOIN. An instance is an agent CLUSTER and holds one document per realm;
     `docId` names only the one it was ROOTED at, so every document joined to it afterwards must answer for it
     in `hostHolderOf` — that is the routing table a `windowproxy.post` to a joined child resolves through, and
     a list that exists only once something has been joined is a list every reader has to ask about first. */
  const eng = { state: "booting", cluster, docId, joinedDocIds: [], msg, groupId: msg && msg.groupId,
                origin: (msg && msg.origin) || "", _cold: cold, _resolvers: [], _remoteAsked: new Set(),
                r: null, _readyP: null };
  _pool.push(eng);
  _reserveStats.made++;
  const n = _bootingCount();
  if (n > _reserveStats.peakBooting) _reserveStats.peakBooting = n;
  return eng;
}
/* A RESERVATION THAT DID NOT BECOME AN INSTANCE LEAVES THE POOL, and it leaves it HERE rather than at each of
   the three call sites, because a reservation left behind is worse than the race it replaced: every later
   arrival for that cluster finds it, joins it, and waits forever on a boot that already failed — a phantom
   holding an agent cluster that will never have an instance, with nothing anywhere to say so. */
function engineBootFailed(eng, e) {
  const i = _pool.indexOf(eng);
  DCHECK(i >= 0 || eng._dropped,
         "a reservation failed to boot and was already out of the pool with no Clear having taken it — the " +
         "slot it holds is this function's to release, so something else removing it means two owners for one " +
         "record and the surviving one will free a frame twice");
  if (i >= 0) _pool.splice(i, 1);
  eng.state = "failed";
  /* THE FRAME GOES WITH IT. renderer-host.js reclaims its own frame when the boot handshake fails — and frees
     the agent cluster in the registry on its way out of the launch — so `r` is null exactly when there is
     nothing to remove; anything that threw after it is this path's to reclaim, and an iframe nobody reaches
     is a whole WASM instance resident under a document that does not reload. */
  if (eng.r) eng.r.destroy();
  /* ONE INSTANCE FAILING IS ONE CRASH, however many documents were waiting on it — which is why the banner
     fires once here and the RECORD is built per caller. crashResult bundled the two, so answering N joined
     callers would have counted N crashes for one aborted boot and the probe's `crashes` would read the number
     of documents that happened to share a cluster. */
  const m = String((e && e.message) || e);
  /* AND IT POINTS AT ITS CAUSE, exactly as engineCrash's does. A boot that aborts inside the engine rejects on
     an ABI call, and renderer-host.js attaches the frame's last lines to that rejection (`e.rendererLines`) —
     which is where the C-side CHECK/DCHECK printed its @WHY ROOT line immediately before abort(). This read
     `e.message` alone, so the banner and every caller's crash record carried emscripten's "native code called
     abort()" and the engine's own statement of what was wrong was thrown away at the one line that had it.
     TWO PRODUCERS, ENUMERATED RATHER THAN DEFAULTED PAST: a rejected ABI call carries the lines, and a failure
     that never reached one (rendererLaunch itself, or an assert in this zone) has none to carry and its own
     message IS the whole cause. Anything else on that field is renderer-host writing a shape this cannot read. */
  const _rl = e ? e.rendererLines : undefined;
  DCHECK(_rl === undefined || Array.isArray(_rl),
         "a boot failure carried a `rendererLines` that is not a list of the frame's output — it is the only " +
         "place the engine's own @WHY survives a rejected ABI call, and a shape this cannot scan is that root " +
         "line being discarded at the one site that holds it");
  const root = _rl === undefined ? "" : rootWhyLine(_rl);
  const err = root ? (m + " | ROOT: " + root) : m;
  crashBanner("create", err);
  for (const w of eng._resolvers) w.resolve(crashRecord("create", err, w.msg));
  eng._resolvers.length = 0;
  _reserveStats.failed++;
}
/* THE LOCAL IS `rend` AND THE FIELD IS `eng.r`, which is not an inconsistency: the three fetch closures
   below each bind `const r = await self.safeFetch(...)` — Fetch's reply record — and a renderer named `r` in
   this scope would be SHADOWED by it inside exactly the functions that deliver bytes into that renderer. The
   shadow would be silent, because none of them needs the renderer today; the next one that does would reach
   for `r` and get a Response.
   IT FILLS THE RESERVATION AND RETURNS NOTHING. The record in the pool is the one every question about this
   agent cluster has been answered by since the instant provisioning began, so building a second object here
   would leave the pool holding the reservation while every caller held the instance — one document with two
   records, which is the defect the reservation exists to close wearing a different hat. */
async function engineRoot(eng, code, html, msg, persist, docName, topLevelUrl) {
  const rend = eng.r;
  DCHECK(rend && rend.name === eng.cluster,
         "a renderer was provisioned under a name that is not its instance's agent cluster — the frame's " +
         "title is how this zone identifies which instance the pool is talking about, and the pool keys the " +
         "same instance by `cluster`, so two spellings of one identity is a routing table that disagrees with " +
         "the DOM");
  /* THE ENGINE'S OUTPUT, WHICH IS THE RENDERER'S LINE BUFFER AND NOT A SECOND COPY OF IT. renderer-host.js
     appends every drained line to this array as each reply lands, so `eng.lines` and `r.lines` are one array:
     engineCrash's backwards scan for the ROOT @WHY, streamPartial's scan for the last @RESULT and
     linesToAnalysis all read exactly what the frame printed, in order, with nothing in this realm deciding
     what to keep. */
  const lines = rend.lines;
  // PHASE 1 — parse + boot; the engine computes the stable bundle IDENTITY from its Lexbor <script> scan.
  /* THE WHOLE RESPONSE HEADER LIST CROSSES, not one header out of it. This used to pull
     `content-security-policy` out of the map and hand the engine that single string, and three things HTML
     decides about a Document from its response were unreachable behind that shape: §7.1.7's policy container
     has an EMBEDDER POLICY item (`Cross-Origin-Embedder-Policy`), §7.5.1's creation table gives a Document an
     OPENER POLICY row (`Cross-Origin-Opener-Policy`), and §7.5.1 reads `Origin-Agent-Cluster` to decide
     §8.1.2.2's agent cluster key — so `window.originAgentCluster` answered `false` for every document this
     extension has ever analysed without the question ever being asked. Widening it one header at a time is the
     wrong shape twice over: several algorithms read different names out of the SAME list, and Fetch's own
     `get` is what decides what a REPEATED header means (HTML §7.1.4.1 prints a table whose point is that
     `require-corp, require-corp` FAILS to parse and leaves the policy at `unsafe-none`).
     SO IT CROSSES AS HTTP FIELD LINES, `name: value` per line, which is a LIST and can say that. The map this
     zone holds cannot — a repeat has already been combined by the browser's own `Headers` iteration, which is
     exactly Fetch's `get`, so what the engine receives is one line per name carrying the combined value and
     its ITEM parse reaches the same verdict a browser's does. The engine parses it back into Fetch's header
     list (core/fetch/headers.c) and reads it ONCE, into §7.4.6's navigation params. */
  const _headers = responseFieldLines(msg && msg.responseHeaders);
  // THE DOCUMENT ID — the ROOT one, because an instance is an agent CLUSTER and holds one realm per same-origin
  // document. A flow that scripts a CROSS-ORIGIN iframe or popup writes state in a PEER instance (a same-origin
  // one is a realm in this same heap and needs no peer at all), and that peer keys its segment of the flow's
  // world by an id minted here. The offscreen mints it because SECURITY.md makes the offscreen the only zone that knows which
  // instance holds which document — the same reason it owns the routing. The engine rejects 0.
  // SCOPE, stated rather than assumed: this counter is unique across the instances ALIVE in this offscreen,
  // which is exactly the set that can message each other today. It is not yet persisted, because a parked
  // foreign segment does not yet outlive a session; when segments park, this becomes a persisted allocator.
  /* AND IT IS MINTED ON THE RESERVATION RATHER THAN HERE, which is why there is no local for it below:
     `hostHolderOf` routes a cross-document message by this name, and a document whose instance is still
     booting would otherwise be one this zone reports as held by nothing for the whole of that boot. A second
     name for it here would be a second answer to "which document is this?" waiting to disagree with the pool's.
     engineCreate states the provenance rules. */
  // HTML §8.1.3.1's TOP-LEVEL CREATION URL. THREE PROVENANCES AND ALL THREE ARE BROWSER-STATED, never
  // derived here: a document a PEER engine created carries its creator's §7.4 decision on the navigable.create
  // notice (hostNotice below, which passes it as the argument); a document a content script reported carries
  // the TAB's url, captured from sender.tab.url where the trusted zone receives it; and a REHYDRATED cold
  // recipe carries what its own session recorded. §8.1.3.5 reads it to decide whether this realm is a SECURE
  // CONTEXT, which decides which of Web IDL §3.3.13's members exist in it, so the engine refuses an empty one.
  const _tlu = topLevelUrl || (msg && msg.topLevelUrl) || "";
  // NO `code` ARGUMENT: identity and the script inventory are the engine's own Lexbor <script> scan of `html`,
  // because a concatenation of a page's scripts cannot represent per-script scope and shifts with an inline
  // script the page did not ship. It used to be passed and cast away on the other side.
  // THE DOCUMENT'S ADDRESS, NOT ITS ORIGIN. This used to hand over originOf(sourceUrl), so the engine's §4.4
  // API base URL was the bare origin and every relative URL a bundle built resolved against the site root:
  // a page at /app/dashboard calling fetch("api/users") was reported as /api/users. The engine derives the
  // origin from the address itself (§4.7's serialization, which its own url.c implements), so the principal
  // and the address are one fact from one place instead of two that can disagree.
  /* qjs_init ANSWERS, and this discarded the answer. Its C body is a wall of CHECKs whose failures abort the
     instance, so the only value it can return is 0 — which is exactly why reading it costs nothing and why a
     non-zero would be an entry that started reporting a failure this zone was not listening for. */
  /* THE DOCUMENT CROSSES AS BYTES, ONE SHAPE, because `qjs_init` takes one thing: a NUL-terminated byte
     sequence it `strlen`s. It used to cross as EITHER — content.js ships a SERIALIZED DOM (the renderer parsed
     the response and this is its characters) while a child navigable's document is the response's own BYTES off
     safeFetch — and the UNTRUSTED frame was the zone that ran the UTF-8 encode on the first of those.
     `content.mojom.Renderer.Init` declares `array<uint8>`, so the encode happens HERE, in the zone that already
     holds the characters. It is an encode and never a decode, which is the direction that matters: decoding the
     BYTES into a string on the way would be this zone running HTML §13.2.3.2's encoding sniffing, badly, as
     UTF-8.
     THE TWO SHAPES ARE ASSERTED RATHER THAN DEFAULTED PAST. `html || ""` stood here, and it turned "the
     producer handed over no document at all" into a page that parses to nothing — a successful analysis of an
     empty document, which reads exactly like a page with no endpoints and no sinks.
     NOT TRANSFERRED, and the ownership argument is the reason rather than caution: these bytes are ALSO
     retained by this zone as `eng.html`, which finish() writes to IndexedDB as the cold recipe's copy of the
     document, so moving the buffer here would leave the cross-session frontier holding a page that parses to
     nothing. mojom.js states why that is a property of the declared TYPE rather than a flag on this call. */
  DCHECK(html instanceof Uint8Array || typeof html === "string",
         "a document reached qjs_init as neither a byte sequence nor characters — content.js ships the " +
         "renderer's serialized DOM and safeFetch ships a response body, so anything else is a producer that " +
         "stopped producing a document, with nothing downstream to notice but an empty finding set");
  const _doc = html instanceof Uint8Array ? html : new TextEncoder().encode(html);
  /* THE SAME NUL, NOW ASSERTED OVER BOTH SHAPES — which the byte-only form of this check could not do: a
     STRING document containing U+0000 encodes to a 0x00 and truncated the parse there just as silently, and
     that half was unasserted. HTML §13.2.5's tokenizer has a rule for that byte (it emits U+FFFD), which is
     the proof it is a byte a document may legitimately contain. What to build is the LENGTH beside the
     pointer, the way qjs_provide already carries one, and with it §13.2.3.2's encoding sniffing algorithm so
     that the document's own encoding is decided by the engine rather than assumed to be UTF-8. */
  DCHECK(_doc.indexOf(0) < 0,
         "a document's bytes contain a 0x00 and qjs_init takes a NUL-terminated C string — the parse would " +
         "stop there and the rest of the document would be absent with nothing to say so. Give qjs_init a " +
         "LENGTH beside the pointer (qjs_provide now carries one) and run HTML §13.2.3.2's encoding sniffing " +
         "over those bytes in the engine");
  /* AND THE ADDRESS IS ASSERTED RATHER THAN DEFAULTED TOO, for the reason the line below it already proves:
     `(msg && msg.sourceUrl) || ""` stood here, and `originOf("")` is `""` — a frontier key every unidentifiable
     document would share, so one page's parked residue would resume inside another's engine. The engine's own
     entry CHECKs that this address parses, so an empty one has always aborted; it aborted one call later, in
     the process that was handed the hole rather than in the zone that made it. */
  DCHECK(!!msg && typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
         "a document reached qjs_init with no address — §4.4's document address is what the engine derives " +
         "this document's ORIGIN from (§4.7's serialization, its own url.c) and what every relative URL the " +
         "bundle builds resolves against, so a document without one is analysed behind no principal at all");
  const _init = await rend.renderer.init(_doc, msg.sourceUrl, eng.docId, _headers, _tlu);
  DCHECK(_init.rc === 0, "qjs_init reported a failure this zone has no handling for — the engine's own entry " +
                         "CHECKs every precondition and aborts, so a non-zero return is a contract that changed");
  /* NEVER 0 — document_bundle_id folds an empty scan to 1 precisely so that a 0 cannot mean two things. A 0
     here would key every unidentifiable document to the SAME frontier entry, so one page's parked residue
     would resume in another's engine. */
  /* `>>> 0` BECAUSE THE WIRE CARRIES AN i32. `qjs_bundle_id` returns a C `unsigned` and the wasm hands it back
     signed, which is what the mojom declares (`int32 bundleId`) rather than hiding behind a type the wire does
     not have — so the reinterpretation is the READER'S, on the line that needs the unsigned value. */
  const _bidRaw = (await rend.renderer.getBundleId()).bundleId >>> 0;
  DCHECK(_bidRaw !== 0, "the engine answered a bundle id of 0 — document_bundle_id never returns one, and a 0 " +
                        "collides every document's frontier key with every other's");
  const _bid = _bidRaw.toString(36);
  /* THE FRONTIER KEY'S ORIGIN HALF. originOf answers "" for an address that does not parse, and "" is a key
     every such document would share — but qjs_init has already CHECKed that this address parses, so an empty
     one here means the two parsers disagree rather than that the page had no origin. */
  const _fkeyOrigin = originOf(msg && msg.sourceUrl);
  DCHECK(_fkeyOrigin !== "", "this zone could not serialize an origin from a document address the engine's own " +
                             "url.c accepted — the frontier key's origin half would be empty for every such page");
  const fkey = _fkeyOrigin + "|" + _bid;
  const prior = persist ? await frontierGet(fkey) : null;
  // PHASE 2 — seed the frontier (fresh, or resume parked recipes). The host sets a VALUE yield-floor per
  // step (the runner-up engine's weight), so this engine yields when it's outranked — no fixed slice.
  await rend.renderer.begin((prior && prior.recipes) ? prior.recipes : "");
  // DEV-ONLY verification hook (a real page never carries this query param): force the RAM-pressure park so
  // the cross-session round trip (park recipes -> IDB -> restart-keep -> resume) is VERIFIABLE without a 512MB
  // working set. Keyed off the URL (flows reliably through msg.sourceUrl to here, unlike a cross-context
  // global). The query param does NOT change the frontier key (origin+bundle), so a later plain visit resumes.
  // The park is DEFERRED N steps (not requested here, before any step) so it fires MID-EXPLORATION — after
  // boot + the first flow bursts have RUN, FORKED, and SUSPENDED — exactly like a production RAM-pressure park
  // (hostSchedule requests park only after engines have stepped, line ~285). Requesting it pre-step parked at
  // work=1 with EMPTY decvecs (nothing had run), so decvec REPLAY (the whole point of a recipe) went untested;
  // deferring parks real residue whose recipes carry non-empty decision vectors AND handler-driven async flows.
  // THE DEFERRAL IS LOAD-BEARING AND NOT AN ARBITRARY DELAY, and the engine side now says exactly why in the
  // one shape it produces: a park taken before any step writes ONE record, `f-,0` — a flow standing on no
  // decision segment with no reward — which solver/cold.c's resume rebuilds into a flow indistinguishable from
  // the boot flow a fresh visit seeds anyway. The round trip would then complete, persist, rehydrate and
  // report success while having carried NOTHING across it. Two dispatches is the smallest count at which the
  // boot flow has forked, so the document holds `s…` segment records and the `f…` records name them; that is
  // the thing under test. If this number is ever changed, the property to preserve is "the park document
  // contains at least one `s` record", never the count itself.
  // Only on the INITIAL park (no recipes to resume yet); firing while resuming would re-park the rehydrated
  // recipes forever (the stored sourceUrl keeps the query param), so a cold/re-visit resume never runs.
  let _forceparkSteps = 0;
  if (msg && typeof msg.sourceUrl === "string" && /[?&]__forcepark=1\b/.test(msg.sourceUrl) && !(prior && prior.recipes)) {
    _forceparkSteps = 2;   // park after boot + the first flow burst: flows have RUN + SUSPENDED (real decvecs) but not yet drained
  }
  const canFetch = typeof self.safeFetch === "function" && msg && msg.sourceUrl;
  /* THE REPLY RECORD THE ENGINE PARSES, which is the ONE shape every host of this engine delivers —
     `{status, statusText, headers: [[name, value], …], urlList, computedType}` PLUS the body's BYTES beside
     it, the same
     record the C hosts build with fetch_reply_new. It used to hand back the BODY'S BYTES alone, so everything
     this zone had actually seen was dropped at this line and re-invented on the other side: the engine reported
     status 200, status message "OK", no headers, and — because Fetch §2.2.6's URL LIST is what `response.url`
     and `response.redirected` are — no redirect, ever, for any reply. `null` is a NETWORK ERROR (the engine
     rejects with §5.6's TypeError), which is what a URL this zone must not or cannot fetch honestly is; it is
     NOT an empty 200.
     THE BODY IS NOT IN THE RECORD, AND THAT IS THE POINT. §2.2.5 makes a response's body a BYTE SEQUENCE, and
     JSON cannot carry one: `JSON.stringify` on a Uint8Array answers `{"0":72,"1":101,…}` — a plausible record
     whose body is not the body. The only ways to put bytes in JSON are to encode them (base64: a CODEC on both
     sides to get wrong, 4/3 on the wire for bodies that are whole JS bundles, and a second copy of the
     expanded text held by the parse) or to DECODE them, which is what `resp.text()` was doing and is the whole
     defect — a UTF-8 decode run in the zone that owns SOP/CORS and owns no semantics, before HTML §8.1.4.2's
     own decode could look at the charset the response declared. So the record travels as text and the bytes
     travel as bytes, copied straight into the engine's linear memory. */
  const fetched = async (u, asScript) => {
    if (!canFetch || hasHole(u)) return null;
    try {
      const abs = new URL(u, msg.sourceUrl).href;
      // chunks: as-script (CORB), never credentialed. replies: opt-in credentialed -> the AUTHENTICATED
      // logged-in reply (the moat headline), gated by safeFetch's own SOP/CORS + GET-only. Default off.
      /* @security-finding  NO `pageOrigin` REACHES THE CHOKEPOINT FROM HERE, AND ONE MUST BEFORE
         `credentialed` IS EVER TURNED ON. safeFetch's credentialed SOP/CORS is written against
         `opts.pageOrigin` — SECURITY.md: "the requesting frame's MessageSender.origin, opaque-unique —
         an opaque/"null" principal is same-origin with nothing" — and this zone passes none, so every
         credentialed read currently fails CLOSED (`_isRealOrigin("")` is false, so no ACAO can match).
         Nothing sets `msg.credentialed` today, so no credentialed read happens at all; the danger is the
         obvious "fix" when one is enabled, which is to pass `originOf(msg.sourceUrl)`. That is the exact
         URL-derivation the credentialed principal exists to forbid, and it would hand a page's own
         sandboxed iframe (opaque origin, ordinary-looking address) same-origin access to the EMBEDDER's
         authenticated bytes. THE VALUE TO PASS IS `msg.origin` (the browser's MessageSender.origin,
         plumbed by _dispatchDocument) — never the address. Wiring it also loosens CORB for a genuinely
         same-origin chunk, which is spec-correct and is a deliberate decision to take at that time, not a
         side effect of this line. */
      const opts = asScript ? { pageUrl: msg.sourceUrl, as: "script" }
                            : { pageUrl: msg.sourceUrl, credentialed: !!(msg && msg.credentialed) };
      const r = await self.safeFetch(abs, opts);
      /* THE CHOKEPOINT'S RECORD IS FIXED — safe-fetch.js returns {ok,status,statusText,headers,body,urlList}
         on every path it has, including every blocked one. `if (!r || typeof r.body !== "string") return null`
         was a malformed answer being turned into a NETWORK ERROR, which is a real Fetch outcome the engine
         acts on: the page's request would report as having failed on the wire when what actually happened is
         that this zone's own chokepoint answered something it never answers.
         AND THE BODY IS BYTES, asserted rather than assumed: a string here is safeFetch back to running Fetch
         §5.2's `text()`, which no assert further down could ever have caught — the bytes it disagrees with
         would already be gone. */
      DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && typeof r.status === "number",
             "safeFetch answered with something other than its reply record — the engine builds a Response " +
             "out of this and a page reads status/headers/body off it, and §2.2.5's body is a BYTE SEQUENCE");
      /* §2.2.6's URL list, straight from the chokepoint that performed the fetch. safeFetch always reports at
         least the URL it requested — §4.1's "If internalResponse's URL list is empty, then set it to a clone of
         request's URL list" — and the engine DCHECKs that at both ends, so an empty one is a bug here rather
         than a response that silently claims never to have redirected. */
      DCHECK(Array.isArray(r.urlList) && r.urlList.length >= 1,
             "safeFetch answered a reply with no URL list — response.url and response.redirected are read off " +
             "nothing else, and the engine would report every redirect as none");
      DCHECK(r.headers && typeof r.headers === "object",
             "safeFetch answered a reply with no header map — the engine's Headers record is built from it");
      /* AND THE TYPE THE CHOKEPOINT COMPUTED, which is the one fact on this record the engine may not derive
         for itself. safeFetch read the bytes and ran the sniff; the renderer is TOLD the answer, exactly as it
         is told a browser-stated origin rather than handed a URL to parse. It is a STRING on every path,
         and the empty one is §5.1's "the supplied MIME type is undefined" surviving the sniff — a positive
         answer — so `undefined` here is a chokepoint that stopped stamping and would reach the engine's own
         DCHECK one call later, naming the wrong side. */
      DCHECK(typeof r.computedType === "string",
             "safeFetch answered a reply with no computed content type — solver/reply_decode.c reads this " +
             "field instead of re-deriving a type from the raw header, so an absent stamp is a producer that " +
             "failed and never a resource whose type is unknown");
      /* STATUS 0 IS NOT A REPLY. It is the one status no HTTP response has, and it is exactly what the
         chokepoint answers when the request never went on the wire at all (bad URL, blocked scheme, blocked
         private target, CORB). This comment's own rule — «`null` is a NETWORK ERROR, which is what a URL this
         zone must not or cannot fetch honestly is» — names that case, and the record was crossing anyway: the
         page saw a real reply whose status was a number no server returns, instead of §5.6's TypeError. */
      if (r.status === 0) return null;
      /* TWO PIECES OF ONE REPLY, because they cross on two channels — `meta` is the JSON qjs_provide parses
         and `bytes` is what it copies into the engine's heap beside it. They are handed over together so a
         caller cannot deliver one without the other. */
      return { meta: { status: r.status, statusText: r.statusText || "", headers: Object.entries(r.headers),
                       urlList: r.urlList, computedType: r.computedType },
               bytes: r.body };
    } catch (e) {
      /* A THROWN fetch IS §5.6's network error and `null` is how it crosses. AN INVARIANT ABORT IS NOT: the
         asserts above throw through this same catch, and returning null for one would deliver a broken host
         contract to the engine disguised as a page whose request failed on the wire. */
      RETHROW_FATAL(e);
      return null;
    }
  };
  // §7.4 STEP 14's RESPONSE, which is the SAME safeFetch and a DIFFERENT answer: a Document is judged against
  // the policy its response carried, so the header travels with the bytes. `fetched` above cannot serve this —
  // it returns the body alone, and answering a child document with no policy is how a page whose CSP kills a
  // sink gets reported as exploitable. A load that does not load answers `bytes: null`, which is a navigable
  // that still exists showing an error page, exactly as the engine's own child_document reads it.
  const fetchedDocument = async (u) => {
    if (!canFetch) return { csp: null, bytes: null };
    try {
      const abs = new URL(u, msg.sourceUrl).href;
      // Never `as:"script"` — these bytes are PARSED as a document, not run as code — and never credentialed:
      // §7.4's fetch is a navigation this engine initiated, not a learned GET being replayed for its reply.
      const r = await self.safeFetch(abs, { pageUrl: msg.sourceUrl });
      DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && r.headers && typeof r.headers === "object",
             "safeFetch answered a document load with something other than its reply record — §7.4 step 14 " +
             "reads the BODY and the POLICY off it, and a Document judged under no policy is how a page whose " +
             "CSP kills a sink gets reported as exploitable");
      /* NOT OK IS A LOAD THAT DID NOT LOAD — the navigable still exists and shows an error page, which is what
         a null body means to the engine's child_document. That is a real §7.4 outcome, not a softening. */
      if (!r.ok) return { csp: null, bytes: null };
      /* THE BYTES, because a Document is PARSED from a byte sequence. This answered `r.body` while that was
         `resp.text()`'s UTF-8 decode, so a document served in any other encoding reached lexbor already
         replaced with U+FFFD and HTML §13.2.3.2's encoding sniffing — the BOM, then the transport charset,
         then §13.2.3.3's prescan — had nothing left to decide. The engine owes that algorithm; this zone owes
         it the bytes. */
      return { csp: r.headers["content-security-policy"] || null, bytes: r.body };
    } catch (e) { RETHROW_FATAL(e); return { csp: null, bytes: null }; }
  };
  /* THE ROUTING TABLE IS THE POOL. `docId` is which document this instance holds and `origin` is the value
     this zone stamps on everything it sends — both are read only by the notice router below, which is the
     only thing that may know either: SECURITY.md makes this zone the one that knows which instance holds
     which document, and the one that may state a sender's origin. */
  // XHR §3.5.6's fetch, through the SAME chokepoint every other byte goes through. `rec` is the engine's own
  // JSON record — method, url, headers, body — and the answer is the reply shape the engine's fetch_reply_new
  // builds, so a null body is the network error §3.5.6's "handle errors" turns into an `error` event.
  const fetchedXhr = async (rec) => {
    let q = null;
    /* THE REQUEST RECORD IS THE ENGINE'S, and it crosses as JSON so it carries its types — a header list is a
       list and a body is a string or null. Text that will not parse is xml_http_request.c writing something
       other than the record it declares, so it aborts here rather than becoming an `error` event on the page's
       XHR: the page would see a server that refused it, which is a lie about whose code was wrong. */
    try { q = JSON.parse(rec); }
    catch (e) { DFAIL("the engine's xhr.send record is not JSON: " + String(e && e.message || e)); }
    DCHECK(q && typeof q === "object" && typeof q.url === "string" && q.url,
           "the engine's xhr.send record names no URL — the chokepoint decides SOP, CORS, method and " +
           "credentials, and cannot decide about a request it was never told");
    if (!q || typeof q.url !== "string") return { meta: { body: null, status: 0, statusText: "", headers: [] }, bytes: null };
    if (!canFetch) return { meta: { body: null, status: 0, statusText: "", headers: [] }, bytes: null };
    try {
      const abs = new URL(q.url, msg.sourceUrl).href;
      /* @security-finding  THREE OF THESE FOUR ARGUMENTS ARE NOT READ BY THE CHOKEPOINT, AND ITS REFUSAL IS
         SILENT. safeFetch forces `method:"GET"`, takes credentials only from `opts.credentialed`, and never
         looks at `body` — which is SECURITY.md's rule holding ("GET only … POST/PUT/DELETE endpoints are
         RECORDED by forced exec, never issued"), but holding invisibly: a page's `xhr.open("POST", u)` is
         answered with the reply to a GET of `u`, and the engine models that reply as the POST's. The engine
         then reasons about a response the server never gave for that request. The chokepoint should REFUSE a
         non-GET the way it refuses a scheme (a `blocked-method` status the engine turns into §3.5.6's error
         event), rather than downgrade it — that is a behaviour change to the analysis, so it is named here
         and not taken silently. `headers` IS read, and comes from the untrusted bundle: within the model
         (uncredentialed GET to a public host, forbidden header names stripped by the browser), but it is why
         safeFetch's "analyzer probe headers only" comment is no longer the whole truth. */
      const r = await self.safeFetch(abs, { pageUrl: msg.sourceUrl, method: q.method, headers: q.headers,
                                            body: q.body, credentials: q.credentials });
      DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && typeof r.status === "number" &&
             r.headers && typeof r.headers === "object",
             "safeFetch answered an XHR with something other than its reply record — §3.5.6's response is " +
             "built from it and the page reads status, statusText and every header off that, and §2.2.5's " +
             "body is a BYTE SEQUENCE");
      /* THE BYTES BESIDE THE RECORD, for the reason `fetched` states: §3.6.6's "get a text response" DECODES
         the received bytes with the FINAL encoding, and until those bytes crossed as bytes that algorithm was
         decoding a re-encoding of this zone's own UTF-8 guess. `responseType = "arraybuffer"` handed the page
         the same round trip in place of the server's bytes. */
      return { meta: { status: r.status, statusText: r.statusText || "", headers: Object.entries(r.headers) },
               bytes: r.body };
    } catch (e) {
      /* §3.5.6's "handle errors": a THROWN fetch is the network error that becomes the page's `error` event.
         An invariant abort is not one and travels on. */
      RETHROW_FATAL(e);
      return { meta: { body: null, status: 0, statusText: "", headers: [] }, bytes: null };
    }
  };
  /* THE PRINCIPAL IS BROWSER-STATED, NEVER PARSED OFF THE ADDRESS. This read `originOf(msg.sourceUrl)`, and a
     document's address does not determine its origin: a page that sandboxes its own iframe
     (`<iframe sandbox="allow-scripts" src="/widget.html">`) gives that document an OPAQUE origin while its
     address still reads `https://site/widget.html`. The offscreen already has the authoritative answer —
     `_senderOrigin(sender)` from the browser's MessageSender, which SECURITY.md requires precisely because
     "a page can sandbox its own iframe, giving it an opaque ("null") origin whose URL still looks normal" —
     and _dispatchDocument hands it over as `msg.origin`. Parsing the address instead re-fabricated the tuple origin
     the browser had already refused to give that document, and then STAMPED it on every message the document
     posts (hostNotice below), which is the one field a bundle's cross-origin check is written against.
     A document this zone provisioned itself carries the origin of the URL THIS zone fetched (hostNotice sets
     it); "" belongs to a rehydrated recipe that predates the field, and the stamp site refuses it rather than
     inventing one. */
  /* `_resolvers` IS A LIST BECAUSE ONE DOCUMENT MAY BE ASKED FOR MORE THAN ONCE while a single instance holds
     it (the RESHIP re-delivery). Every caller waiting on this document is answered by the ONE engine's
     finalize; a second engine is never built to give a second caller its own copy. */
  /* `cluster` IS THIS INSTANCE'S IDENTITY and `docId` is only which document it is ROOTED at. They used to be
     the same field, and that is the whole defect: an instance is an ORIGIN-KEYED AGENT CLUSTER holding one
     realm per same-origin document (main.c's `engine_child_realm`), so the document it was started from names
     it no more than a tree is named by its root. `groupId` is kept beside the key because a document this
     instance CREATES inherits its creator's browsing-context group and the key alone cannot be taken apart. */
  /* `_remoteAsked` IS THIS INSTANCE'S OWN — the request ids of cross-agent operations already carried to the
     peer that holds the object. It belongs to the engine and not to the zone's token map because the question
     it answers is "has THIS instance's request already been asked", and an unanswered request is re-reported by
     qjs_host_requests on every single step; asking again would perform the peer's operation — a program, with
     the page's own side effects — once per step. */
  /* `r` IS THE INSTANCE. It replaces the `M` handle and the `ptrs` free list together: a pointer an ABI call
     RETAINED (qjs_init's five arguments) lives as long as the module does, the module dies with the frame, and
     the frame is removed at finalize — so there is nothing left in this realm to free, and nothing in this
     realm that can read an untrusted instance's memory. */
  eng.lines = lines; eng.fkey = fkey; eng.prior = prior; eng.persist = persist;
  eng.fetched = fetched; eng.fetchedDocument = fetchedDocument; eng.fetchedXhr = fetchedXhr;
  eng.code = code; eng.html = html; eng._forceparkSteps = _forceparkSteps;
  DCHECK(eng.msg === msg,
         "the reservation this instance is being rooted into carries a different message record than the one " +
         "this document was parsed, fetched and keyed from — the reservation's is what hostNotice stamps a " +
         "cross-document delivery's origin from and what finish() writes the cold recipe out of, so two " +
         "records for one document is a message stamped with one document's principal and persisted under " +
         "another's address");
  /* THE FIRST ROUND'S RECORD, TAKEN BEFORE THIS INSTANCE IS RANKABLE. `qjs_begin` above is what seeded the
     frontier, so this line is the earliest moment a top-flow weight exists at all — and it is earlier than the
     moment this record leaves the `booting` state, which is what makes "an engine the ranking has never heard
     from" a state that cannot occur rather than a case to default. THE ORDER IS THE INVARIANT: the two facts
     are written first and the state that makes them readable is written second, so there is no instant at
     which the ranking or the RAM floor can see a hot engine that has not reported. */
  await engineRecordFacts(eng);
  eng.state = "hot";
}
/* A SECOND DOCUMENT OF AN AGENT CLUSTER JOINS THE INSTANCE THAT IS ALREADY RUNNING THAT CLUSTER. It is the same
   five arguments `qjs_init` takes, stated by this zone for the same reasons, and it is a DIFFERENT operation
   from rooting: the runtime, the class registrations, the world registry and the frontier already exist, so
   what this adds is a Lexbor tree in the agent's own arenas, a realm through main.c's engine_child_realm, and
   that document's scripts seeded on the ONE frontier. Provisioning a second instance instead is the split
   SECURITY.md's `(browsing-context group, origin)` key exists to forbid — two heaps for one similar-origin
   window agent, in which `iframe.contentDocument.body.appendChild(x)` is unrepresentable.
   THE INSTANCE MUST ALREADY BE ROOTED AND SEEDED, which is what `qjs_join`'s own two asserts say (`g_dom`,
   `g_begun`): a document joined between those two calls would be parsed, given a realm, and never execute a
   line — indistinguishable from a document that had no scripts. So a caller holding a RESERVATION waits for it
   the way the delivery branch does, and this asserts what it waited for.
   IT IS NOT A PLACE THE SAME DOCUMENT MAY ARRIVE TWICE. The engine interns documents by NAME, and a name it
   already holds fires main.c's own `world_doc_hosted` assert — but only if the two arrivals SPELL the document
   the same way, which is exactly what the caller's `hostHolderOf` question decides here. Two spellings of one
   document would pass both checks and build a second tree, a second realm and a second run of that document's
   scripts inside one agent, so the name is asserted to be unheld across the WHOLE pool before it crosses. */
async function engineJoin(eng, msg, docName, topLevelUrl) {
  DCHECK(eng.state === "hot" || eng.state === "fetching",
         "a document was joined to an instance in state `" + eng.state + "` — a join ADDS a document to a LIVE " +
         "agent, and qjs_join's own asserts name the two calls that must already have happened (qjs_init roots " +
         "the agent, qjs_begin seeds the frontier its scripts become members of)");
  DCHECK(typeof docName === "string" && docName !== "",
         "a document was joined with no NAME — a peer routes a delivery and a cross-agent operation on that " +
         "name, so an unnamed one is a document no message can ever reach");
  DCHECK(hostHolderOf(docName) === null,
         "a document was joined under a name some instance in this pool already holds — the engine interns " +
         "documents by name, so this would either be one document arriving twice (a second tree, a second " +
         "realm and a second run of its scripts in one agent) or two documents wearing one name, and both are " +
         "a routing table that answers a peer's delivery with whichever it wrote last");
  DCHECK(msg.sourceUrl !== "" && originOf(msg.sourceUrl) === eng.origin,
         "a document whose principal is not this instance's was joined to it — an instance is an ORIGIN-KEYED " +
         "agent cluster and SECURITY.md makes the instance the principal, so this would put two origins behind " +
         "one credentialed-read principal; the engine's own CHECK aborts on it, and this zone computed the " +
         "cluster key that sent it here");
  const html = msg.pageHtml;
  /* THE SAME ONE SHAPE THE ROOT'S DOCUMENT TAKES, and for the same reason: `content.mojom.Renderer.Join`
     declares `array<uint8>` because `qjs_join` has main.c's byte-identical signature, so the two operations
     take ONE contract and a change to what a document arrives with reaches both. */
  DCHECK(html instanceof Uint8Array || typeof html === "string",
         "a joined document arrived as neither a byte sequence nor characters — a child navigable's document " +
         "is safeFetch's response body and a reported one is the renderer's serialized DOM, so anything else " +
         "is a document this agent would hold as nothing at all");
  const _doc = html instanceof Uint8Array ? html : new TextEncoder().encode(html);
  /* THE SAME NUL THE ROOT'S DOCUMENT IS ASSERTED FOR, and for the same reason: qjs_join strlen()s the bytes,
     so a document carrying a 0x00 is parsed truncated at it with the rest simply absent. HTML §13.2.5's
     tokenizer has a rule for that byte, which is the proof a document may legitimately contain one. */
  DCHECK(_doc.indexOf(0) < 0,
         "a joined document's bytes contain a 0x00 and qjs_join takes a NUL-terminated C string — the parse " +
         "would stop there and the rest of the document would be absent with nothing to say so. Give qjs_join " +
         "a LENGTH beside the pointer (qjs_provide now carries one) and run HTML §13.2.3.2's encoding " +
         "sniffing over those bytes in the engine");
  const _join = await eng.r.renderer.join(_doc, msg.sourceUrl, docName,
                                          responseFieldLines(msg.responseHeaders), topLevelUrl);
  DCHECK(_join.rc === 0,
         "qjs_join reported a failure this zone has no handling for — the engine's own entry CHECKs " +
         "every precondition and aborts, so a non-zero return is a contract that changed");
  /* THE NAME ENTERS THE ROUTING TABLE ONLY ONCE THE ENGINE HOLDS IT. Between the call and this line the
     document is one this zone would answer for and the instance would not, which is the direction that is
     safe: a delivery in that window is refused by the post branch's assert rather than routed into an agent
     that has never heard the name. */
  eng.joinedDocIds.push(docName);
  /* THE ROUND ENDS HERE, SO IT RECORDS. A join is the second-biggest thing this zone ever copies into an
     instance's linear memory (a whole document) and it seeds that document's scripts as flows on the frontier
     — so both Level-1 facts have moved, and the ranking that picks next must see them. This is the same
     statement engineRoot makes on its last line and the service round makes on its way out. */
  await engineRecordFacts(eng);
}
/* THE LEVEL-1 RANKING'S TWO FACTS, RECORDED WHERE THE INSTANCE ANSWERS THEM RATHER THAN READ WHERE THE RANKING
   NEEDS THEM. Both were SYNCHRONOUS reaches into the module on every ranking pass — a `qjs_top_weight` ccall
   per comparison, and an `M.HEAPU8.length` per engine per admission check — and neither survives the boundary
   renderer-host.js puts between this zone and an instance: a sandboxed opaque-origin frame answers by
   postMessage, so on the line that ranks there is no value to read and no way to ask for one. So the host
   RECORDS. Every round this zone has with an instance ends here, and the ranking reads a number it already
   holds. That prediction has now been paid: the pool's engines ARE renderers, this function's two reads are
   an awaited ABI call and a field off the reply record it answered on, and not one of its callers moved.
   THE NUMBERS ARE AS OF THE LAST ROUND, AND hostSchedule IS BUILT SO THAT IS THE RIGHT TIME. It ranks, advances
   the winner, and re-ranks — so the engine that just did work is the one whose numbers are freshest, and an
   engine that did none has numbers that cannot have moved, because nothing but a round with this zone changes
   either fact. That is also why the recording sites are the three ROUNDS (seed, step, service) and not the
   ccalls inside them: a round is what the boundary makes atomic.
   THERE IS NO UNVISITED ARM HERE, AND THAT IS NOT AN OMISSION. §scheduler's "a UCB optimism bonus ∝ 1/(visits+1)
   so a never-run flow is never starved" is implemented once, in solver/flow.c's flow_weight, where a never-run
   flow stands at its reward plus the full 1.0 bonus; a freshly seeded frontier's boot flow carries it and
   qjs_top_weight reports it. Level-1 therefore INHERITS level-2's optimism instead of restating it, which is
   what "ONE WFQ policy at both levels" means — and restating it here would additionally be a branch on a state
   the producer cannot be in, since qjs_top_weight DCHECKs `g_begun` (main.c) and qjs_begin is what sets it. */
async function engineRecordFacts(eng) {
  /* -INFINITY IS AN ANSWER, NOT A BROKEN ONE. `engine_top_weight` is `flow_best() ? flow_weight(b) : -1.0/0.0`
     (solver/engine.c), so negative infinity is the engine's POSITIVE statement that its frontier holds no
     runnable flow — and it is the RIGHT statement, because that is precisely the value that ranks below every
     real weight in the comparisons below. Demanding a finite number here rejected the producer's own vocabulary:
     every drained engine aborted its round, was destroyed and re-provisioned, and aborted again — 43279 crashes
     against 0 completed runs on a single fixture page, with the pool still reporting a live framed renderer, so
     nothing about the shape of the pool said anything was wrong.
     NaN AND +INFINITY ARE STILL BROKEN and are still asserted, which is what makes this a narrowing of the
     contract rather than its removal: NaN is the case §qjs_set_yield_floor's own comment calls out — every
     comparison against it is false, so the top flow never yields and the Level-1 interleave silently stops —
     and +Infinity is a weight no flow_weight can produce. The check that matters is the one that can fire.
     THE RANKING NEEDS NO ARM FOR IT. `-Infinity < x` is true for every real weight, so a drained engine sorts
     last by arithmetic rather than by a branch, and the step that follows retires it through the DONE path it
     was always going to take. */
  const _rank = await eng.r.renderer.getTopWeight();
  const w = _rank.weight;
  DCHECK(Number.isFinite(w) || w === -Infinity,
         "the engine answered a top-flow weight of " + w + " — a weight is either a finite number or the " +
         "-Infinity that says its frontier holds no runnable flow, and NaN or +Infinity is neither: every " +
         "Level-1 ranking comparison against a NaN is false, so the pool would pick by array order and call " +
         "it value-of-information");
  eng.topWeight = w;
  /* THE WORKING SET AS THE FRAME STATED IT ON THE VERY CALL ABOVE, read off that call's own reply. There is no
     HEAPU8 in this realm to read — that is the whole point of the boundary — so `workingSetBytes` is a DECLARED
     REPLY FIELD of every `content.mojom.Renderer` method, validated by the transport at both ends. Taking it
     from the reply just awaited is the freshest statement that exists and no extra round trip could improve on
     it: a call is the only thing that can grow that memory, so the moment a call answers is the moment the
     number changed. WASM memory is a whole number of 64 KiB pages by definition, so anything else is not the
     view over this instance's memory. */
  const b = _rank.workingSetBytes;
  DCHECK(Number.isInteger(b) && b > 0 && b % 65536 === 0,
         "an instance reported a working set that is not a whole number of WASM pages (" + b + ") — HEAPU8 is " +
         "the view over the entire linear memory, which is allocated and grown in 64 KiB pages, so a length " +
         "outside that is a view onto something other than this engine's memory");
  eng.residentBytes = b;
}
/* THE LEVEL-1 WFQ'S ONE INPUT, read off the record. A NaN would order this engine against every other by a
   comparison that is false in both directions, so the pool's pick would depend on array order — a fairness
   invariant silently replaced by whichever engine happened to be first; that is asserted above, where the
   engine answers it. What is asserted HERE is that there is an answer at all, because an absent number and a
   stale one are different things and only one of them may be ranked. */
/* AND A NON-HOT ENGINE IS NOT RANKED LOW, IT IS NOT RANKED. `if (eng.state !== "hot") return -Infinity` stood
   here and was dead in both directions: hostSchedule ranks the HOT set it filtered a line earlier, so nothing
   ever reached it — and if anything had, -Infinity is the wrong answer to give. 355a03d2 established that
   -Infinity is the ENGINE's own word for a frontier holding no runnable flow, so answering it for a
   RESERVATION would report an instance that has not started as one that has finished, and the step that
   follows a bottom-ranked engine is the DONE path: a booting engine would be finalized before it existed. The
   two states are distinguished by not answering for either. */
function engineWeight(eng) {
  DCHECK(eng.state === "hot",
         "the Level-1 ranking was asked for the weight of an engine in state `" + eng.state + "` — it ranks " +
         "the hot set it filtered a line earlier, so a fetching or BOOTING engine arriving here is that filter " +
         "and this function disagreeing about which engines are rankable");
  DCHECK(typeof eng.topWeight === "number",
         "a hot engine carries no recorded top-flow weight — engineRecordFacts writes one at the end of every " +
         "round this zone has with an instance, the first of them before the record ever reaches the pool, so " +
         "an absent one is a round that stopped recording rather than an engine with nothing to do");
  return eng.topWeight;
}
/* ONE DELIVERY, both channels, so no call site can carry the record and forget the bytes. `rep` is what
   `fetched` answered: `null` for §5.6's network error (the JSON `null` the engine rejects with a TypeError),
   or `{meta, bytes}`.
   THE PLACEMENT MOVED INTO THE FRAME AND `engineBodyBytes` IS DELETED WITH IT. It malloc'd into `M.HEAPU8`,
   handed qjs_provide a [ptr, len] pair and freed the block afterwards — the exact algorithm renderer.html's
   `bytes-pair` placement now performs, on the only side of the boundary that can address that memory, with the
   same "one extra byte so a zero-length body still has an address of its own" and the same `null` meaning THIS
   ANSWER CARRIES NO BODY AT ALL. Keeping a copy here would have been a second placement over a heap this realm
   no longer has a view of.
   THESE BYTES ARE THE ONES WHOSE OWNERSHIP WOULD ALLOW A MOVE, AND THE REASON THEY ARE STILL COPIED CHANGED.
   safeFetch mints them per call and this delivery is their only consumer, so nothing in this realm holds a
   second reference the way `eng.html` does for a document load — the ownership argument is sound. What used to
   stop it was the transport: `bodies` was one list shared by every argument shape, so the relay would have had
   to carry a transfer list naming WHICH entries were owned. That reason is gone with the envelope, and the one
   that stands is stronger. mojo.js builds a message's transfer list from the DECLARED TYPES, which is what
   makes `handle<message_pipe>` a real pipe pass, and Mojo's `array<uint8>` is a copied byte sequence; the type
   for bytes that MOVE is `mojo_base.mojom.BigBuffer`. So a move is a second declared byte type, not a flag on
   this call — and it must be, because `Init`'s document is a parameter of the same spelling whose bytes this
   zone RETAINS. One type meaning two ownerships is exactly what a validator cannot check. */
async function engineProvide(eng, url, rep) {
  DCHECK(rep === null || (rep && typeof rep === "object" && rep.meta && rep.bytes instanceof Uint8Array),
         "`fetched` answered neither §5.6's network error (null) nor a reply — a reply is its JSON metadata " +
         "and its BYTES together, and a record arriving without one of the two is half a response");
  await eng.r.renderer.provide(url, JSON.stringify(rep === null ? null : rep.meta),
                               rep === null ? null : rep.bytes);
}
async function engineServiceFetch(eng) {   // one round: resolve every pending reply/chunk, then the engine is hot again
  /* THE REPLY'S METADATA CROSSES AS TEXT AND CARRYING ITS TYPE — JSON, exactly as qjs_host_answer's answer
     does. A bare string could not say `null` for a network error without it being the four characters "null",
     and could not carry the URL list, the status or the headers at all. Its BODY crosses as BYTES beside it,
     because JSON can say none of the 256 values a byte has without first running an algorithm over them, and
     the algorithm this zone used to run (Fetch §5.2's `text()`, a UTF-8 decode) destroyed exactly the evidence
     HTML §8.1.4.2's classic-script decode exists to read. */
  const replies = owedList("GetPending", (await eng.r.renderer.getPending()).urls);
  for (const u of replies)
    await engineProvide(eng, u, await eng.fetched(u, false));
  const chunks = owedList("GetChunks", (await eng.r.renderer.getChunks()).urls);
  for (const u of chunks)
    await engineProvide(eng, u, await eng.fetched(u, true));
  await engineServiceHostRequests(eng);
}
/* EVERY OWED LIST CROSSES THE SAME WAY — newline-joined records, or "" for none — so it is SPLIT in one place
   that says so. IT NO LONGER MAKES THE CALL, and that is the typing: it used to take an ABI entry NAME and
   relay it through a generic `fn` string, which is precisely the shape that let a caller ask for anything and
   let nothing check the answer. The caller now names the method and mojom declares the reply's type, so the
   `String(x == null ? "" : x)` this function opened with is gone with it — a NULL pointer becoming the four
   characters "null" and then one bogus record (a URL nothing is parked on, which the engine's own qjs_provide
   DFAILs on one call later, naming the wrong side) is a shape the validator refuses before it arrives. */
function owedList(method, s) {
  DCHECK(typeof s === "string",
         "content.mojom.Renderer." + method + " answered with something that is not text — every owed list " +
         "crosses as newline-joined records and an empty list is the empty string");
  return s.split("\n").filter(Boolean);
}

/* THE INSTANCE HOLDING A DOCUMENT, by exact name. A child's name is PREFIXED by its creator's ("<creator>.<n>")
   and the creator is precisely the instance that does NOT hold it — that is why the notice exists at all — so a
   prefix match routes a message straight back to its sender. The engine catches that, twice; it should not have
   to be the thing that catches it. */
/* AND IT ANSWERS FOR EVERY DOCUMENT THE INSTANCE HOLDS, NOT ONLY THE ONE IT WAS ROOTED AT. `qjs_join` adds a
   second document of this cluster to the live agent (main.c records it in `g_joined_ctx`/`g_joined_dom`), and
   the engine interns every document by NAME — `qjs_route`'s own arrival assert is
   `world_doc_hosted(world_doc_intern(doc))` (solver/engine.c) — so a name this table cannot resolve is a
   delivery routed to the wrong instance or dropped, which the post branch's assert reports as a create notice
   that was lost. The two lists are read as one because they ARE one on the other side of the boundary. */
function hostHolderOf(docName) {
  for (const e of _pool) {
    if (e.docId === docName) return e;
    if (e.joinedDocIds.indexOf(docName) >= 0) return e;
  }
  return null;
}

/* THE INSTANCE OF AN AGENT CLUSTER — and it is a DIFFERENT QUESTION from the one above, which is why they are
   two functions. `hostHolderOf` asks WHERE A NAMED DOCUMENT IS, and is ROUTING: it answers for the one document
   an instance is ROOTED at, and a message posted to a document nothing holds is a dropped delivery. This asks
   WHICH INSTANCE A NEW DOCUMENT BELONGS IN, and is ADMISSION: it answers by the (group, origin) cluster, so a
   document the engine has already created inside that heap answers here even though nothing has ever named it
   to this zone. `admit` used to ask the ROUTING question and take a "no" as licence to build a second instance
   — one question standing in for the other, which is exactly how a same-origin pair ended up in two heaps. */
function hostClusterOf(key) {
  for (const e of _pool) if (e.cluster === key) return e;
  return null;
}

/* THE RENDEZVOUS FOR A CROSS-AGENT OPERATION, and it is THIS ZONE'S because neither engine can hold it. The
   asking instance knows only its own request id, which is unique inside that instance alone — two peers may
   ask one holder the same number — and the holder must echo something back that says which instance and which
   call site the completion belongs to. So the token is minted here, is opaque to both engines (main.c states
   that at both entries), and carries nothing an untrusted engine could state for itself.
   IT IS NOT DELETED WHEN THE FIRST COMPLETION ARRIVES, and that is the design rather than a leak. A peer
   document's state IS its flows: the operation is performed by EVERY live timeline the holder has and each one
   completes with its own answer, so one token names N completions and all of them are true. Relaying every one
   is what makes the asking engine's own assert — engine_host_answer's, which names the fork the asking flow
   still owes — reachable at all; dropping the second here would hide the missing half in this zone instead.
   One entry per operation that actually crossed, dropped with the instance that asked (engineFinalize). */
const _remoteOps = new Map();   // token -> { asker, req }
let _nextRemoteToken = 1;

/* WHAT THIS ZONE OWES A ONE-WAY NOTICE. Two ops today, and each is an ACTION only this zone can take —
   SECURITY.md makes the offscreen the only zone that knows which instance holds which document.
   `navigable.create <child> <creator> <url> <origin> <topLevelUrl> <csp>` — the engine has already named the document and
   already handed the page a WindowProxy for it; what is missing is an INSTANCE. This provisions one under that
   name, loading the child's own document through the one safeFetch chokepoint.
   `windowproxy.post <target> <world> <targetOrigin> <base64>` — routed VERBATIM to the instance holding
   <target>, with THIS engine's origin stamped on it. The engine may not state that origin for itself: it is
   untrusted, and a forgeable event.origin defeats every origin check in every bundle. */
async function hostNotice(eng, line) {
  const f = line.split("\t");
  if (f[0] === "navigable.create") {
    /* SEVEN FIELDS, because the POLICY and HTML §8.1.3.1's TOP-LEVEL CREATION URL are both read below. The
       count said five once, so a record that stopped at the origin passed the assert and then took `undefined`
       for the creator's policy clone — a child document judged under NO policy, which is §7.4's inheritance
       silently deleted, and the one field a CSP-blocked sink verdict is decided against. An assert that
       permits the record it is about to misread is the shape of check that reports green while the value is
       missing, so it counts every field the reader below indexes. */
    DCHECK(f.length >= 7, "a navigable.create notice was short of its fields — the engine writes child, creator, url, origin, top-level creation URL and policy");
    if (hostHolderOf(f[1])) return;   // already provisioned: the engine announces a document once
    const loaded = await eng.fetchedDocument(f[3]);
    /* THE CHILD'S PRINCIPAL IS THE ORIGIN OF THE URL THIS ZONE FETCHED — derived HERE and not read off the
       notice, even though the notice carries one at f[4]. SECURITY.md draws that line at this exact record:
       "identity may be minted by the untrusted side because it is only a name, but ROUTING and the ORIGIN
       STAMPED ON A DELIVERED MESSAGE are the trusted zone's alone." The name is a name; the origin is the
       thing every bundle's cross-origin check is written against, so it comes from the load this zone
       performed. RESIDUAL, named because it is not yet built: a child whose creator applied `sandbox`
       (attribute or CSP) has an OPAQUE origin that this derivation cannot see — the sandbox flag set is not
       yet carried on the notice, so such a child is currently stamped with its address's tuple origin. Carry
       the flags on the notice and mint a per-document opaque token here, the way _senderOrigin does. */
    /* THE CHILD'S BROWSING-CONTEXT GROUP IS ITS CREATOR'S, and that is the spec rather than a convenience: a
       nested navigable is in its parent's group by construction, and §7.4's auxiliary one is too unless
       `noopener` severed it. So the child's cluster key is (creator's group, the origin of the URL this zone
       fetched) — which is what makes two cross-origin children of ONE page at the SAME origin one cluster, the
       way HTML says they are, instead of two heaps for a pair that can script each other. */
    /* THE CHILD'S DOCUMENT, AS BYTES. `(loaded && loaded.body) || ""` was a defaulted read of a producer's
       field — and the field it defaulted was a STRING this zone had decoded, so a cross-origin child served in
       any encoding but UTF-8 was provisioned from a document already replaced with U+FFFD. The bytes go
       through `arg` unchanged (see engineCreate); a load that did not load carries none, and the empty byte
       sequence is the `about:blank`-shaped document the engine's own child_document builds for one. */
    DCHECK(loaded && (loaded.bytes === null || loaded.bytes instanceof Uint8Array),
           "the document load answered neither bytes nor the null that means it did not load");
    const _childBytes = loaded.bytes === null ? new Uint8Array(0) : loaded.bytes;
    const msg = { type: "AST_ANALYZE", pageHtml: _childBytes, sourceUrl: f[3],
                  origin: originOf(f[3]), groupId: eng.groupId,
                  responseHeaders: {}, credentialed: !!(eng.msg && eng.msg.credentialed) };
    /* THE POLICY IS THE RESPONSE'S, AND THE CREATOR'S CLONE IS THE FALLBACK — §7.2.6/§7.4 in the order the
       spec states them: a Document is judged against the policy container its own response carried, and a
       response that carried none inherits the clone of its creator's, which is the field the notice carries.
       THE CLONE IS THE REST OF THE RECORD, not one field: it is a raw CSP header value and HTTP allows
       HTAB inside one (this engine's own CSP parser treats tab as source-list whitespace), so a policy carrying
       one splits into more fields than the record has. That is also why it is LAST and why the top-level
       creation URL sits before it. The C router already reads it this way — its splitter stops at the policy
       and keeps the remainder verbatim — and two readers of one format that disagree about where a field ends
       are two formats. */
    const policy = (loaded && loaded.csp) || f.slice(6).join("\t") || "";
    if (policy) msg.responseHeaders["content-security-policy"] = policy;
    /* HTML §8.1.3.1's TOP-LEVEL CREATION URL, which the CREATOR decided (§7.4: the creator's own for a nested
       navigable, the navigable's own address for an auxiliary one) and this zone carries, because the new
       instance cannot see what embeds it. §8.1.3.5 reads it to decide whether the child is a SECURE CONTEXT,
       and Web IDL §3.3.13's members exist in that child or do not by that answer. */
    /* AND ITS CLUSTER DECIDES WHICH OF TWO OPERATIONS THIS IS. A SAME-ORIGIN child never reaches this line at
       all — navigable.c's `child_in_this_agent` builds it as a realm in the creator's own heap and emits no
       notice — so a cluster hit here is a SECOND CROSS-ORIGIN document of one cluster (two `<iframe
       src="https://cdn/…">` of one page), which HTML puts in ONE heap for exactly the reasons the same-origin
       pair is in one. That document is JOINED to the instance already running the cluster; only a cluster with
       no instance is PROVISIONED one. Both arms exist now, so there is no third state in which a child is
       announced and nothing happens — the "host gap, visible as a parked flow" navigable.h names is what this
       zone used to be left with, and a document that is never hosted is one every read through its proxy parks
       on forever. */
    const _ckey = clusterKeyOf(msg);
    DCHECK(_ckey !== eng.cluster,
           "the engine sent a create notice for a child in its OWN agent cluster — a same-origin child is a " +
           "second REALM in that heap and never leaves it, so this zone and navigable.c's child_in_this_agent " +
           "have answered one origin question two ways");
    /* AND THE QUESTION IS ASKED OF THE RESERVATIONS TOO, which is what makes asking it worth anything here.
       This check stood a document fetch, a frame boot and three ABI round trips away from the push that made
       its answer true, and detached service rounds run concurrently with admit — so two arrivals for one
       cluster could both be told "no instance" and both build one. engineCreate takes the pool slot before its
       first await now, so a cluster that is merely BEING provisioned answers here exactly as a provisioned one
       does, and the case below is reached only when it is genuinely a second document. */
    const holder = hostClusterOf(_ckey);
    if (holder) {
      /* A RESERVATION IS STILL THE HOLDER, AND THE JOIN WAITS FOR IT — the same suspension the delivery branch
         below takes, for the same reason: the pool slot is taken before the first await precisely so that a
         cluster being provisioned answers here exactly as a provisioned one does, and the honest thing to do
         with a document whose agent is being built is to hold it until it is. `qjs_join` needs both `qjs_init`
         and `qjs_begin` to have run, which is what `_readyP` settling means. */
      if (holder.state === "booting") await holder._readyP;
      DCHECK(holder.state === "hot" || holder.state === "fetching",
             "a document was announced for a cluster whose instance never became one — the reservation holding " +
             "it failed to boot, so this child has an agent that does not exist rather than one it can join, " +
             "and every read through its proxy would park forever");
      await engineJoin(holder, msg, f[1], f[5]);
      return;
    }
    /* A CHILD DOCUMENT IS A DOCUMENT: it joins the ONE pool and is ranked, sliced, parked and finalized by the
       one host WFQ like every other. It has no caller to resolve to, so its findings merge the way a
       rehydrated cold engine's do rather than being returned to a requester that never asked for it — which
       is `cold`, stated at the reservation because it is a fact about this call site and not about how the
       boot turns out. AWAITED, because the notices of one round are acted on IN ORDER: a page opens a window
       and posts to it in the same turn, so the instance must exist before the post that names it is routed. */
    await engineCreate("", msg.pageHtml, msg, false, f[1], f[5], true)._readyP;
    return;
  }
  if (f[0] === "windowproxy.post") {
    DCHECK(f.length >= 5, "a windowproxy.post notice was short of its fields");
    const target = hostHolderOf(f[1]);
    DCHECK(target !== null, "a message was posted to a document no instance in this pool holds — the create " +
                            "notice naming it was dropped, or that instance was finalized while a peer still " +
                            "held a WindowProxy for it");
    /* A TARGET MID-BOOT IS STILL THE TARGET, AND THE DELIVERY WAITS FOR IT. The reservation carries the
       document's name from the instant its provisioning began, so hostHolderOf answers with a record whose
       instance does not exist yet — and the honest thing to do with a message for a document that is being
       built is to hold it until it is, which is the same suspension every other cross-instance operation in
       this zone takes. Routing it now would post into `null`; refusing it would drop a delivery the engine has
       already handed the page a WindowProxy for. */
    if (target.state === "booting") await target._readyP;
    DCHECK(target.state === "hot" || target.state === "fetching",
           "a message was posted to a document whose instance never became one — the reservation holding that " +
           "name failed to boot, so the send is a delivery with nowhere to go rather than a message in flight");
    /* THE STAMP IS THE SENDER'S BROWSER-STATED ORIGIN, SERIALIZED. An empty one means this instance was
       rehydrated from a cold recipe written before the principal was part of it — this zone then does not know
       whose message this is, and the one thing it may not do is invent it (a wrong `event.origin` makes the
       engine report exploits that are not real and miss ones that are). CHECK, not DCHECK: in release the
       delivery would otherwise carry a fabricated identity into a security decision. */
    CHECK(!!eng.origin, "a cross-document message was posted by an instance with no recorded principal — only " +
                        "the trusted zone may state a sender's origin, and this one has none to state; carry " +
                        "the document's browser-stated origin into the cold recipe so a resumed instance keeps it");
    await target.r.renderer.route(line, stampOrigin(eng.origin));
    return;
  }
  /* `remoteop.answer <token> <completion>` — a COMPLETION this instance produced for an operation it was asked
     to perform, naming the token this zone minted. It leaves as a NOTICE and not as a return value because a
     peer answers BY RUNNING A PROGRAM (an IDL getter, a page's setter, a page's function) as a flow on its own
     frontier, so the answer does not exist when qjs_perform returns and may be parked behind anything else that
     frontier is doing.
     RELAYED VERBATIM AND UNREAD. The completion is in remote_object.c's grammar, not JSON, because a member
     whose value is an OBJECT crosses as a NAME in the answering agent's namespace — which means nothing outside
     an engine, and which JSON could only either serialize (returning something that is not the thing) or drop.
     This zone routes text; only an engine knows what a name means, so the remainder is kept whole rather than
     re-joined field by field on a grammar this file does not own. */
  if (f[0] === "remoteop.answer") {
    DCHECK(f.length >= 3, "a remoteop.answer notice was short of its fields — the engine writes the rendezvous " +
                          "token and the completion record, and a notice missing either names no call site");
    const to = _remoteOps.get(f[1]);
    /* A TOKEN THIS ZONE DID NOT MINT names no asker, and the answer is then unroutable: the flow that asked is
       parked on a completion that will never arrive, which is silent everywhere. It is this zone's own key, so
       an unknown one means the holder echoed something other than what it was handed. */
    DCHECK(to !== undefined, "a peer answered a cross-agent operation under a rendezvous token this zone never " +
                             "minted — the token is echoed verbatim by the instance that performed it, so the " +
                             "flow that asked is parked on an answer nothing can deliver");
    const _t2 = line.indexOf("\t", line.indexOf("\t") + 1);
    DCHECK(_t2 > 0, "a remoteop.answer notice carried no completion record — an empty answer is not " +
                    "`undefined`, it is a relay that lost the peer's completion, and the engine's own decoder " +
                    "says so at the other end");
    if (!to || _t2 < 0) return;
    await to.asker.r.renderer.hostAnswerRemote(to.req, line.slice(_t2 + 1));
    return;
  }
  DFAIL("an engine emitted a notice with an op this zone does not act on — the engine's half is built, so the " +
        "host's half is the unbuilt one: `" + f[0] + "`");
}

// WHAT ONLY THIS ZONE CAN ANSWER. A cross-document operation is answered by the instance holding that document,
// and SECURITY.md makes the offscreen the only zone that knows which instance that is — the same reason it owns
// the routing and stamps the sender's origin. The asking flow is SUSPENDED mid-frame until the answer lands, so
// this is pumped every round alongside the fetch replies; leaving one unanswered parks that flow indefinitely
// (its siblings keep running, which is the point of suspending rather than blocking).
async function engineServiceHostRequests(eng) {
  // ONE-WAY NOTICES, and this zone OWES each of them an action — a notice it reads and discards is a document
  // nothing runs and a message nothing delivers, with every later read through them parked forever. They were
  // being read and discarded. Handled IN ORDER and one at a time: a page opens a window and posts to it in the
  // same turn, so the create must have finished provisioning before the post that names it is routed.
  for (const line of owedList("GetHostNotices", (await eng.r.renderer.getHostNotices()).notices))
    await hostNotice(eng, line);
  // ONE BRANCH, AND ONLY BECAUSE THIS ZONE CAN GENUINELY ANSWER IT. The rule that deleted every other branch
  // stands: a loop that walks the owed requests is a place to be tempted into GUESSING an answer, which is what
  // answering `navigable.create` with "not created" was. `document.fetch` is not a guess — it is a network
  // fetch, and this zone already relays those through the one safeFetch chokepoint SECURITY.md requires. Every
  // other op is still left UNANSWERED: the asking flow stays parked with its snapshot intact, its siblings keep
  // running, and qjs_host_requests keeps reporting it, which is visible where a wrong answer is not.
  //
  // AND IT HAS TO BE ANSWERED, not merely answerable. A flow parked on a request this host never satisfies
  // leaves the engine stalled forever, and the step loop above has no other reason to stop — the same shape the
  // WPT pump was spinning on. Answering is what lets a navigation finish.
  const reqs = owedList("GetHostRequests", (await eng.r.renderer.getHostRequests()).requests);
  for (const line of reqs) {
    /* `id<TAB>op`, which engine_host_requests writes with a snprintf of a counter it CHECKs against wrapping.
       `if (tab < 0) continue` dropped a record that did not have that shape — and dropping it is the one
       outcome with no symptom anywhere: the asking flow stays parked on an answer nobody will ever send, its
       siblings keep running, and the engine reports the same unanswerable request every step forever. */
    const tab = line.indexOf("\t");
    DCHECK(tab > 0, "an owed host request is not `id<TAB>op` — the id is what an answer is routed by, so a " +
                    "record without one names a call site this zone can never reach");
    if (tab < 0) continue;   // release: an unanswerable record leaves its flow parked, which is visible
    const id = +line.slice(0, tab), op = line.slice(tab + 1);
    /* THE ID IS THE WHOLE ROUTING TABLE for an answer — engine_host_answer walks every flow's register for
       exactly this number and the engine's own counter starts at 1, so a 0 or a NaN answers a call site that
       does not exist and the answer is silently dropped on the other side (its `return 0` means "the asking
       flow is gone", which would be a lie). */
    DCHECK(Number.isInteger(id) && id > 0,
           "an owed host request carries no usable id — an answer is routed by that number alone");
    /* A CROSS-AGENT OPERATION IS NOT ANSWERED BY THIS ZONE AT ALL — it is ASKED OF A PEER, which is the one
       shape neither of the two branches below has. §7.2.5.1's `otherW.length` is the child-navigable count of
       the PEER's active document and the four internal methods a lent object performs ([[Get]], [[Set]],
       [[Delete]], [[Call]]) run the peer's own code, so an answer computed here would be this document's frames
       reported as the other's, or a write that never happened. This zone does the one thing only it can:
       SECURITY.md makes the offscreen the only zone that knows which instance holds which document, so it
       carries the record there and carries the completion back.
       A PREFIX AND NOT A LIST, so an operation added to remote_object.c reaches its instance with nothing here
       to remember: every one of them names its target document in the same field and differs only in what the
       peer resolves, never in who resolves it.
       NOTHING IS ANSWERED INSIDE THE ASK. The peer answers BY RUNNING A PROGRAM as a flow on its own frontier,
       so the completion arrives later through that instance's notices (hostNotice above) — which is also what
       lets the answer suspend, park and resume like every other flow instead of blocking this zone. */
    if (op.startsWith("windowproxy.get\t") || op.startsWith("object.")) {
      /* AN UNANSWERED REQUEST IS RE-REPORTED EVERY STEP (engine_host_requests deliberately does not dedupe:
         two identical questions from two flows are two questions), so asking on every sighting would perform
         the peer's operation once per step forever — each one a program with the page's own side effects. */
      if (eng._remoteAsked.has(id)) continue;
      const holder = hostHolderOf(op.split("\t")[1]);
      /* NOT A SLOW ANSWER, A MISSING INSTANCE: the navigable.create notice naming that document was dropped,
         or the instance holding it was finalized while a peer still held a reference into it. Left alone the
         asking flow parks forever with its snapshot intact, which is correct and invisible — so it is said. */
      DCHECK(holder !== null, "a cross-agent operation named a document no instance in this pool holds — the " +
                              "create notice for it was dropped, or that instance was finalized while a peer " +
                              "still held a WindowProxy or a lent object of it");
      if (!holder) continue;
      DCHECK(holder !== eng, "a cross-agent operation was routed back to the instance whose flow asked it — an " +
                             "operation on this agent's own object is performed in this heap and never leaves, " +
                             "so this zone and the engine have answered one identity question two ways");
      const token = "op" + (_nextRemoteToken++);
      _remoteOps.set(token, { asker: eng, req: id });
      eng._remoteAsked.add(id);
      /* THE ASK IS RECORDED BEFORE IT IS MADE, and with an await under it that is no longer a matter of style.
         `_remoteAsked` and `_remoteOps` are both written above this line because the call below suspends: an
         unanswered request is re-reported by qjs_host_requests on every step, so a round that started the ask
         and recorded it afterwards would let the next round see the same id unrecorded and perform the peer's
         operation — a program, with the page's own side effects — a second time. */
      await holder.r.renderer.perform(token, op);
      continue;
    }
    // XHR §3.5.6's fetch is the SECOND thing this zone can genuinely answer, and for the identical reason: it
    // is a network fetch, and safeFetch is the one chokepoint SECURITY.md allows it through. The record carries
    // the whole request — method, headers and body — because the chokepoint decides SOP, CORS, method and
    // credentials and cannot decide about a method it was never told. A flow parked on one is SUSPENDED at the
    // exact line the page wrote `send()` on, which is what a synchronous XMLHttpRequest is.
    if (op.startsWith("xhr.send\t")) {
      const r = await eng.fetchedXhr(op.slice("xhr.send\t".length));
      // 0 IS THE NORMAL COMPLETION. An answer is a completion record and not a value (ECMA-262 6.2.4): this
      // zone fetched bytes rather than running another instance's program, so it has nothing to have thrown
      // in. A relayed cross-agent operation answers with 1 and the thrown value, which is what lets the
      // asking page's `try`/`catch` around it run.
      await engineAnswer(eng, id, r.meta, r.bytes);
      continue;
    }
    if (!op.startsWith("document.fetch\t")) continue;
    const r = await eng.fetchedDocument(op.slice("document.fetch\t".length));
    // JSON, because the answer carries its TYPE across this seam: a null body is a load that did not load, and
    // the string "null" is a one-word document. The BODY is not in that JSON — a Document is parsed from a
    // BYTE SEQUENCE, and this seam carries one (HostAnswer's `array<uint8>?` body).
    await engineAnswer(eng, id, { csp: r.csp }, r.bytes);
  }
}
/* THE SAME TWO CHANNELS FOR A SYNCHRONOUS ANSWER. Two of the requests this zone can genuinely answer carry a
   fetched BODY — XHR §3.5.6's fetch and §7.4 step 14's document load — and a body is a byte sequence for the
   same reason a reply's is. `bytes === null` says this answer has none, which is what every other request kind
   is: an answer that is a number or a document NAME has no bytes beside it. The trailing 0 is ECMA-262 6.2.4's
   NORMAL completion — this zone fetched bytes rather than running another instance's program, so it has
   nothing to have thrown in. */
async function engineAnswer(eng, id, meta, bytes) {
  await eng.r.renderer.hostAnswer(id, JSON.stringify(meta), 0, bytes);
}
async function engineFinalize(eng) {
  /* ASK THE ENGINE FOR ITS RESULT — the ABI entry that exists for exactly this and had NO CALLER anywhere in
     the extension. The only place the production engine ever printed an @RESULT was qjs_emit_partial, and
     streamPartial CONSUMES that line as it merges it, so a session's findings reached this function only when
     a partial happened to be left over: a page that finished before the first 750 ms cadence produced a
     result with no document in it at all, which `result || {}` then turned into a successful analysis
     reporting no endpoints and no sinks. It is asked BEFORE teardown, because the document is built out of
     the context teardown frees, and it is the same call qjs_emit_partial makes.
     A CRASHED instance is not asked: its memory is what aborted, every finding in it is discarded below, and
     re-entering it would only produce a second abort. */
  if (!eng._crashed) {
    let json = null;
    /* AN ENGINE ABORT IS A RECORDED OUTCOME AND A HOST INVARIANT FAILURE IS NOT, which every catch around an
       ABI call now has to say out loud: an ABI call rejects for BOTH — the frame's own abort travels as the
       rejection engineCrash is written against, and this zone's own contract failures (a call made into a
       renderer whose connection is dead, a record the mojo validator refused) travel as apiclientFatal.
       Reporting the second as the engine's crash would blame the instance for this zone's broken contract, and
       would discard a page's findings for it. */
    try { json = (await eng.r.renderer.getResult()).result; }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "result", e); }
    if (!eng._crashed) {
      DCHECK(typeof json === "string" && json.length > 0,
             "qjs_result answered with no document — result_json returns nothing only when the composition " +
             "itself could not be allocated, which is this page's entire finding set being dropped");
      if (json) eng.lines.push("@RESULT " + json);
    }
  }
  /* THE OUTSTANDING RENDEZVOUS GO WITH THE INSTANCE THAT ASKED, and this is the line that makes the map's
     never-delete-on-answer rule safe: a completion relayed into a torn-down instance is a call into a renderer
     that no longer exists — a frame this function has already removed, which renderer-host.js refuses at the
     seam rather than posting into a closed port. Dropping them here is not dropping the ANSWER — the engine's own engine_host_answer already treats
     a request whose flow is gone as an answer nobody is waiting on. */
  for (const [token, to] of _remoteOps) if (to.asker === eng) _remoteOps.delete(token);
  /* A CRASHED INSTANCE IS NOT TORN DOWN, and this used to try. `qjs_teardown` is the engine walking its own
     gc_obj_list to report leaks — a FINDING about a runtime that ran — and an instance whose linear memory is
     the thing that aborted has no such finding to give. Across this boundary it is worse than pointless: the
     renderer is dead after one failed call (renderer.html cannot serve another), so the attempt would abort in
     THIS zone on renderer-host's own assert and be reported as a second engine crash on top of the first.
     THE FRAME GOES REGARDLESS, and that is the whole cleanup: the retained qjs_init arguments, the module and
     its linear memory die with the document, so there is no free list on either side of this seam. It is also
     the only thing that reclaims an instance — a Module was collected once nothing referenced it, an iframe
     left in this document is not. */
  if (!eng._crashed) {
    try { await eng.r.renderer.teardown(); }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "teardown", e); }
  }
  eng.r.destroy();
  const result = linesToAnalysis(eng.lines, eng.msg, !eng._crashed);
  result._fkey = eng.fkey; result._prior = eng.prior;   // engine-computed key + parked entry -> persisted below
  if (eng._crashed) {
    // NEVER BUILD ON AN UNCERTAIN ARCHITECTURE. A WASM abort means this engine crashed — every finding it
    // produced is from a crashed (untrustworthy) instance, so DISCARD them all. The result is a pure FAILURE:
    // only the crash marker + the error, so the crash is impossible to overlook and nothing downstream (cache,
    // popup, moat) ever consumes a crashed engine's output. Experimental stage: fail hard, then fix the ROOT.
    result._engineCrashed = true;
    result.fetchCallSites = []; result.securitySinks = []; result._park = []; result._prior = null;
  }
  return result;
}
/* A WASM Aborted() is the engine CRASHING — a should-never-happen. It stays scoped to this engine (one page's
   crash must not throw and kill the whole multi-engine scheduler serving the user's other tabs), but it must
   be IMPOSSIBLE to overlook: a LOUD console.error banner + a persistent batch flag + total discard of the
   engine's findings (engineFinalize). NOT a quiet @E buried in resolverErrors — that is how the g_optaint
   teardown leak hid for so long. Experimental stage: a crash halts trust in that engine, never softened. */
function crashBanner(stage, m) {   // LOUD + a persistent batch flag; EVERY abort path (create/step/teardown) routes here — no crash is ever quiet
  /* TWO SWALLOWING CATCHES ARE GONE FROM THESE TWO LINES, and with them the `|| 0` that made the counter
     create itself. Neither operation can throw — an increment of a declared number and a console write — so
     each `catch (_) {}` could only ever have hidden the ONE thing that matters here: that this is the crash
     path, and a crash that fails to announce itself is exactly the outcome this function exists to prevent.
     The counter is declared at load beside _engineLog, so a reader can tell "no engine has crashed" from
     "bridge.js is not in this zone". */
  self._engineCrashOccurred++;
  console.error("\n==== ENGINE CRASH (" + stage + ") — WASM ABORTED, findings DISCARDED, NOT swallowed ====\n" + m + "\n");
}
// The C-side CHECK/DCHECK emits its @WHY/@E ROOT line (phase/cond/at/reason) to stderr -> sink -> the frame's
// line buffer IMMEDIATELY before abort(). A bare emscripten Aborted() message ("native code called abort()") is
// terse and useless on its own, so every crash path surfaces that root line IN the loud banner — a crash must
// POINT AT ITS CAUSE, not just announce itself (this is what forced grepping the reason out of the result
// during debugging).
/* ONE SCAN, BECAUSE THERE IS ONE QUESTION. It stood inside engineCrash alone, so the CREATE path — the only
   path whose whole failure is inside the engine's own boot — announced itself with the abort message and
   nothing else, and a real @WHY that the frame had already printed cost a full day's wrong diagnosis. The two
   callers differ only in WHERE the lines come from (a live engine's buffer, or the ones renderer-host attaches
   to the rejection of the call that aborted), which is an argument and not a second copy of this loop. */
function rootWhyLine(lines) {
  for (let i = lines.length - 1; i >= 0; i--) {
    const ln = String(lines[i]);
    if (ln.startsWith("@WHY ") || (ln.startsWith("@E ") && /"(reason|cond|phase)"/.test(ln))) return ln;
  }
  return "";
}
function engineCrash(eng, stage, e) {
  const m = String((e && e.message) || e);
  eng._crashed = true;
  const root = rootWhyLine(eng.lines);
  const err = root ? (m + " | ROOT: " + root) : m;   // the crash RECORD carries its cause (netdiff/result-visible), not only the console banner
  eng.lines.push('@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(err) + "}");
  crashBanner(stage, err);
}
// A crash BEFORE the engine object exists (creation/boot abort). Same rule: LOUD, findings discarded,
// marked _engineCrashed — never a quiet "degenerate result" the reviewer reads as a boring empty page.
/* THE BANNER IS NO LONGER PART OF THIS FUNCTION, and the split is what one aborted boot costs: `crashBanner`
   increments the crash COUNT, and a reservation may have several documents attached to it (a RESHIP
   re-delivery, a sub-frame, a second arrival that joined it while it booted), each of which needs its OWN
   record because each is answering a different caller. Bundled, answering N callers counted N crashes for one
   instance that failed to boot, and the probe's `crashes` would have read the number of documents that
   happened to share a cluster. engineBootFailed banners once and calls this per caller. */
function crashRecord(stage, m, msg) {
  /* NO RESULT DOCUMENT IS EXPECTED HERE, and this is the only caller that may say so: the instance aborted
     before it could answer, so its absence is the crash rather than a broken contract. */
  const r = linesToAnalysis(['@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(m) + "}"], msg, false);
  r._engineCrashed = true;
  r.fetchCallSites = []; r.securitySinks = []; r._park = []; r._prior = null;
  return r;
}

/* MACROTASK yield (worker/offscreen). Between engine quanta the host MUST return to the event loop with a
   MACROtask (not just an awaited microtask, which the message queue never interleaves with) so the ONE worker
   thread services its message port — triage/GET_STATE evals, postMessage from the offscreen, other timers —
   while a lone engine keeps exploring its byte-identical frontier across qjs_step re-entries. MessageChannel is
   sub-ms (setTimeout(0) is clamped ~4ms and would dominate a 12ms quantum). §NO BOUNDS: a thread-yield, not a cap. */
const PARTIAL_MS = 750;   // incremental-merge cadence: a hot engine surfaces its current findings this often
const _macroChan = (typeof MessageChannel !== "undefined") ? new MessageChannel() : null;
function macroYield() {
  if (_macroChan) return new Promise((res) => { _macroChan.port1.onmessage = () => res(); _macroChan.port2.postMessage(0); });
  return new Promise((res) => setTimeout(res, 0));
}
/* THE PURE SCHEDULER POLICY (no wasm knowledge — engine ops are injected, so this is unit-testable with
   mock engines). Each iteration: ADMIT waiting documents up to the RAM cap (ops.admit gates creation — no
   instance is built until a slot is free), then advance the highest-weight HOT engine and re-rank. Before
   stepping it the host sets its VALUE yield-floor to the RUNNER-UP engine's weight (ops.setFloor), so the
   engine runs until it's outranked then yields HOT — no fixed slice count (a banned step-cap). Slots turn
   over because each engine self-parks to the cold tier (IDB recipe) under RAM pressure (ops.requestPark). */
async function hostSchedule(pool, ops) {
  for (;;) {
    if (ops.admit) await ops.admit();   // gate creation to cap: seat waiting docs into freed slots
    if (!pool.length) break;
    const hot = pool.filter((e) => e.state === "hot");
    if (!hot.length) {   // every live engine is mid-something: wait for the earliest to become hot, then re-rank
      /* TWO STATES REACH THIS ARM AND THEY ARE ONE KIND OF THING: not rankable YET, on a promise that says
         when. An engine awaiting a reply body is one; a RESERVATION whose instance is still being provisioned
         is the other, and it was missing. Without it an empty hot set with a booting engine in the pool fell
         through to the `break` below — and the pool is NOT empty (the reservation is in it), so _hostKick's
         `finally` re-entered immediately: a full-speed spin through admit on a condition that only the boot
         it refused to wait for could change. */
      const pending = pool.filter((e) => e.state === "fetching" || e.state === "booting");
      if (!pending.length) break;
      for (const e of pending)
        DCHECK(e._readyP && typeof e._readyP.then === "function",
               "an engine in state `" + e.state + "` carries no readiness promise — this arm is the only " +
               "thing that resumes the pool when nothing is hot, so an engine it cannot wait on is one the " +
               "loop spins on or abandons, and both are silent");
      await Promise.race(pending.map((e) => e._readyP));
      continue;
    }
    /* Level-1 WFQ pick + the RUNNER-UP's weight (the value yield floor). ONE reading per engine: every weight
       here is a number the last round with that instance recorded, so asking three times per comparison — which
       this did, `ops.weight(best)` twice inside a loop over `ops.weight(e)` — bought nothing but the chance for
       one pass to rank against two different answers.
       AND THE SCAN NO LONGER COUNTS THE WINNER AS ITS OWN RUNNER-UP. Seeded with `best = hot[0]` and then
       visiting hot[0] again, the first iteration fell through to `else if (w > runner)` and set runner to BEST'S
       OWN weight; whenever the highest-value engine happened to be first in the pool, the floor handed to
       ops.setFloor was that engine's own top weight rather than the next engine's. The engine compares each
       running flow against that floor (engine.c: `flow_weight(cur) < g_yield_floor`), so the winner yielded the
       thread at its first back-edge below its own best flow — to a document worth strictly less — which is the
       Level-1 interleave inverted, silently, on exactly the arrangement where the pick was already right. */
    let best = hot[0], bestW = ops.weight(best), runner = -Infinity;
    for (let i = 1; i < hot.length; i++) {
      const e = hot[i], w = ops.weight(e);
      if (w > bestW) { runner = bestW; best = e; bestW = w; }
      else if (w > runner) runner = w;
    }
    /* THE RANKING ITSELF IS STILL SYNCHRONOUS, WHICH IS WHY IT IS STILL A RANKING. Every `ops.weight` above is
       a number the last round with that instance RECORDED (8196a0e7), so the whole scan runs on one consistent
       set of values with no suspension in it. The three calls below DO suspend — each is a message to a frame —
       and the pick they act on is therefore as of the top of this iteration, which is exactly what the policy
       says it is: rank, advance the winner, re-rank. Nothing between here and the step can change the set, because
       the only thing that moves an engine between hot and fetching is this loop, and a detached service round
       touches only the engine it belongs to (which is not in `hot`). */
    if (ops.setFloor) await ops.setFloor(best, hot.length > 1 ? runner : -1e300);   // outranked-by-runner-up => yield; lone engine => run on
    // Normally step `best` (the highest-value engine). But under RAM pressure with >1 engine competing for the
    // working-set floor, step the LOWEST-value engine after flagging it to PARK — evicting it to the IDB cold
    // tier (residue -> replay recipes) frees RAM so the top engines keep running (Level-1: "the lowest-weight
    // one is EVICTED under pressure"). Parking needs a step (the flag is read inside qjs_step), and `best` never
    // steps the low engine, so we must target it directly. A LONE over-budget engine is never parked: no slot
    // contention, it runs to completion (admission gates new docs until it frees its slot; hard OOM crashes loud).
    let target = best;
    if (ops.requestPark && ops.underPressure && hot.length > 1 && ops.underPressure()) {
      target = hot[0];
      for (const e of hot) if (ops.weight(e) < ops.weight(target)) target = e;
      await ops.requestPark(target);
    }
    /* THE PARK FLAG AND THE STEP THAT READS IT STAY ORDERED ACROSS THE BOUNDARY, and not by luck: both are
       calls on ONE renderer's port, which is point-to-point and delivers in order, and this loop makes no other
       call into that instance in between. The pair was atomic when it was two ccalls; it is sequenced now. */
    const st = await ops.step(target);
    /* THE STEP CODE IS TWO VALUES, AND THIS SPOKE THREE. qjs_step answers ENGINE_STEP_DONE (0) or
       ENGINE_STEP_YIELD (2) and nothing else — it folds the scheduler's STALLED into a yield deliberately,
       "the bridge speaks two values". The third branch here tested for 1, a NEED_FETCH code no version of
       engine_sched_step has ever returned, and it was the ONLY caller of ops.serviceFetch: the entire reply
       path (qjs_pending -> safeFetch -> qjs_provide) was unreachable in the shipped extension, so every flow
       a page's `fetch()` parked stayed parked, the engine answered STALLED forever, and the analysis promise
       for that document never resolved. The `else` that caught everything not 0 or 1 is what made a value the
       engine cannot produce look like a branch someone had thought about. */
    DCHECK(st === 0 || st === 2,
           "qjs_step answered with a code outside {DONE, YIELD} — the engine folds STALLED into YIELD itself, " +
           "so a third value is an ABI that changed under a host still speaking the old one");
    // DEV __forcepark: request the park only after N dispatches, so it captures MID-EXPLORATION residue
    // (recipes with real decvecs + handler-driven async flows), mirroring a production RAM-pressure park.
    if (target._forceparkSteps > 0 && --target._forceparkSteps === 0) await ops.requestPark(target);
    // INCREMENTAL MERGE: a lone UNBOUNDED engine never reaches st===0, so without this its already-emitted
    // breadth surfaces only at finalize. Snapshot + merge on a coarse cadence on every non-final step.
    /* AWAITED, AND THAT IS A CORRECTNESS REQUIREMENT RATHER THAN TIDINESS. streamPartial asks the engine to
       PRINT a result document and then finds it by INDEX in the line buffer and splices it out. The print
       arrives on that call's own reply, so the scan must not run until the call has answered — and the service
       round started below appends to the same buffer, so an unawaited partial would splice by an index the
       round had already moved. Fire-and-forget was sound only while the ccall was synchronous. */
    if (st !== 0 && ops.streamPartial) await ops.streamPartial(target);
    if (st === 0) {   // fully explored, or self-parked under RAM pressure: finalize (residue -> IDB cold tier)
      await ops.finish(target);
    } else {   // ENGINE_STEP_YIELD — a cooperative quantum, or a STALL the engine reports as one.
      /* PAY EVERYTHING THE ENGINE SAYS IT IS OWED, in ONE round: the replies parked flows wait on, the lazy
         chunks, the notices this zone must act on and the synchronous requests only it can answer. A stall is
         indistinguishable from an ordinary quantum here on purpose, so asking is the only way to tell — and
         asking costs an empty ccall on a quantum that owes nothing. Servicing only the REQUESTS (which is
         what this did) left the fetch half of the same owed list unpaid forever.
         NON-BLOCKING, the way the unreachable branch was: the engine drops out of the hot set while its round
         runs, so a slow reply on this document never stalls another's. */
      target.state = "fetching";
      /* ONE FIELD FOR ONE QUESTION, WHICH IS "WHEN DOES THIS ENGINE BECOME RANKABLE AGAIN". It was `_fetchP`,
         and a boot is not a fetch — naming the reservation's provisioning promise after the reply round would
         be one name answering two different facts, which is how the wait arm above would come to be read as
         "wait for a body" by the next person to add a state to it. */
      target._readyP = ops.serviceFetch(target).then(
        () => { target.state = "hot"; },
        (e) => {
          /* A THROW OUT OF A SERVICE ROUND IS AN INVARIANT FAILURE — every assert in the reply builders, the
             notice router and the request loop lands here. It was being discarded into a state reset. */
          target.state = "hot";
          crashBanner("service", String((e && e.stack) || e));
          if (self.APICLIENT_DEV) throw e;
        });
      // Then RETURN TO THE EVENT LOOP via a MACROtask so the ONE thread services its message port (evals,
      // postMessage, timers) before we re-enter and RESUME the byte-identical frontier — the anti-freeze yield.
      await macroYield();
    }
  }
}

// ---- Engine-bound ops + the live pool ----
const _pool = [];        // BOOTING/hot/fetching engines — one record per agent cluster, bounded by the RAM floor
const _waiting = [];      // documents awaiting a slot: { code, html, msg, persist, resolve } — NO instance built yet
let _hostDriving = false;
/* THE RESERVATION LEDGER, WHICH IS CUMULATIVE BECAUSE THE STATE IT DESCRIBES IS TRANSIENT. A probe that reads
   the pool after an analysis settles sees an empty array whether the reservation mechanism ran or was never
   reached — the exact shape renderer-host's provisioned/destroyed counters exist for, one level up. So every
   reservation is counted where it is made and every exit is counted where it is taken, and the probe asserts
   the arithmetic: the pool never holds more booting records than there are provisionings still running, which
   is the statement that no reservation was ever left behind as a phantom holding an agent cluster nothing
   will ever provision.
   `joinedBooting` IS THE ONE THAT PROVES THE RACE IS CLOSED. It counts second arrivals for a cluster whose
   instance was still being provisioned — the window in which the pool used to answer "no instance" and build
   a second heap for one similar-origin window agent. `joinedRooted` is the pre-existing case beside it (a
   RESHIP re-delivery, a sub-frame of an instance already running), kept apart so one cannot be read as the
   other. `peakBooting` is the high-water mark of simultaneous reservations, which is what says whether this
   run ever had two provisionings in flight at once. */
const _reserveStats = { made: 0, rooted: 0, failed: 0, joinedBooting: 0, joinedRooted: 0, peakBooting: 0 };
const _hostOps = {
  weight: engineWeight,
  /* THE VALUE YIELD FLOOR — run until outranked by the runner-up. The `try {} catch (_) {}` around it is gone
     with the two below it: a call into the engine either answers or REJECTS, and a rejection is either the WASM
     instance crashing or this zone's own transport contract breaking. Swallowing either leaves a dead engine in
     the pool being stepped, ranked and finalized as if it were alive, which is the one outcome engineCrash
     exists to make impossible. This one is not caught at all — an unranked, unsteppable engine is not a state
     the pool has a handling for, so it travels to hostSchedule's own failure arm. */
  setFloor: async (eng, floor) => {
    DCHECK(Number.isFinite(floor) || floor === -Infinity,
           "the pool set a yield floor that is not a number — the engine compares its top flow's weight " +
           "against it, and a NaN floor makes every comparison false so the flow never yields");
    await eng.r.renderer.setYieldFloor(floor);
  },
  underPressure: () => _residentBytes() >= HOT_RAM_BUDGET,   // summed live wasm memory over the working-set floor
  /* THE COLD-TIER PARK, whose engine half is an unbuilt capability that says so: qjs_request_park is a bare
     DFAIL naming the serializer to write. The catch here swallowed exactly that — the abort a DFAIL exists to
     produce was turned into a no-op, so the host went on believing it had evicted an engine that never parked
     and the missing capability had no symptom at all. It is the forcing function; it is not caught. */
  requestPark: async (eng) => { await eng.r.renderer.requestPark(); },
  /* INCREMENTAL MERGE (coarse cadence): snapshot a HOT engine's current findings + merge to the cumulative
     moat WITHOUT waiting for a finalize that an unbounded engine never reaches. First sight starts the clock,
     so short analyses (finalize before PARTIAL_MS) never pay for it. qjs_emit_partial appends a fresh @RESULT
     to eng.lines (teardown-free); we parse just that snapshot, CONSUME the line (bound eng.lines growth — the
     final teardown re-emits its own @RESULT), and merge via onFrontierAdvance (globalStore, dedup-idempotent). */
  streamPartial: async (eng) => {
    const now = Date.now();
    if (!eng._lastPartial) { eng._lastPartial = now; return; }
    if (now - eng._lastPartial < PARTIAL_MS) return;
    eng._lastPartial = now;
    /* A WASM ABORT IS THE ENGINE CRASHING — it was the only failure this call had while it was a ccall, and it
       is now one of two: renderer-host's own asserts reject the same way and are this zone's contract, not the
       instance's. The line scan below only runs once this has answered, because the @RESULT it prints rides
       that reply. */
    try { await eng.r.renderer.emitPartial(); }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "partial", e); return; }
    let idx = -1;
    for (let i = eng.lines.length - 1; i >= 0; i--) if (String(eng.lines[i]).startsWith("@RESULT ")) { idx = i; break; }
    /* qjs_emit_partial PRINTS ONE, unconditionally. No line here means the print sink between the engine and
       this zone dropped it, and returning quietly would report the engine as having found nothing. */
    DCHECK(idx >= 0, "qjs_emit_partial produced no @RESULT line — it prints the one result document every time " +
                     "it is called, so its absence is this zone's own capture of the engine's output failing");
    if (idx < 0) return;   // release
    const partial = linesToAnalysis([eng.lines[idx]], eng.msg, true);   // parse only the snapshot line
    eng.lines.splice(idx, 1);                                           // consume it
    // Merge on EITHER surface: an XSS-only page (verified @S PoCs, no endpoints) must surface incrementally
    // too — gating the merge on fetchCallSites alone dropped every sink from a live/looping engine.
    const hasWork = partial.fetchCallSites.length || (partial.securitySinks && partial.securitySinks.length);
    if (!hasWork) return;
    /* THE MERGE CALLBACK IS THE OTHER HALF OF THIS EDGE. `typeof … === "function"` guarding the call meant a
       zone that had not installed it dropped every incremental finding silently; offscreen-brain.js installs
       it before this file is even loaded, so its absence is a broken load order, not an optional feature. */
    DCHECK(typeof self.onFrontierAdvance === "function",
           "the trusted zone has no onFrontierAdvance to merge an engine's findings into — every incremental " +
           "finding this engine emits has nowhere to go");
    self.onFrontierAdvance(eng.msg.sourceUrl, partial);
  },
  step: async (eng) => {
    let st;
    try { st = (await eng.r.renderer.step()).code; }
    catch (e) { RETHROW_FATAL(e); engineCrash(eng, "step", e); return 0; }   // crashed instance -> finalize (loud), don't keep stepping a dead engine
    /* THE ROUND ENDS HERE, so the re-rank that follows reads what THIS step left behind: the frontier it
       advanced and the linear memory it grew. A crashed instance is never asked — its memory is the thing that
       aborted, and the branch above hands it to finish() rather than back to the ranking. */
    await engineRecordFacts(eng);
    return st;
  },
  /* ONE SERVICE ROUND, and there is no second op beside it. `serviceHostRequests` was a separate injected op
     because the yield branch paid half the owed list; it is a strict subset of this one (engineServiceFetch
     ends by calling it), so keeping both would be two names for one round with a caller free to pick the
     half that skips the replies. */
  /* A SERVICE ROUND IS A ROUND, so it records too — and it is the one that would be missed by recording after
     steps alone. A delivered reply body is the biggest single thing this zone ever copies into an instance's
     linear memory (a whole JS bundle), and it is copied with no step in between, so the RAM floor would not see
     that growth until this engine's NEXT slice — admission running against memory already spent. The weight
     moves for the same round's other half: a delivered reply resumes the flows parked on it, which is precisely
     the reply-gated work §Attacker-sources calls the point, and the ranking must see it before it picks again.
     A round that THREW recorded nothing, which is correct: hostSchedule's rejection arm owns that failure and
     the last round's numbers are still the last thing this instance actually reported. */
  /* AND IT IS THE ROUND THAT OWNS A CLEARED ENGINE'S FRAME. hostClear drops the whole pool, and an engine that
     was mid-round when it did is the one case where the frame cannot go with it: renderer-host refuses to
     destroy a renderer with a call outstanding, because the caller parked on that answer would never hear
     anything again. A Clear therefore does not interrupt a round — it marks the engine and the round removes
     the frame when it lands, on both arms, because a round that THREW has left the same iframe behind. */
  serviceFetch: async (eng) => {
    try { await engineServiceFetch(eng); await engineRecordFacts(eng); }
    finally { if (eng._dropped) eng.r.destroy(); }
  },
  admit: async () => {   // gate CREATION to the RAM budget: build an instance only under memory pressure headroom
    // 1) seat waiting LIVE documents (the user's open tabs) first
    /* AN INDEX WALK RATHER THAN A SHIFT, because one waiting document can be legitimately UNSEATABLE YET (the
       defer below) and a shift would either drop it or spin this loop forever re-reading it. */
    let i = 0;
    while (i < _waiting.length && _admissionHasHeadroom()) {
      const job = _waiting[i];
      /* ONE INSTANCE PER AGENT CLUSTER, ASKED OF THE POOL — which IS the register of who holds what, so nothing
         here remembers a document it no longer holds. This asked ONE INSTANCE PER DOCUMENT, by documentId, and
         that is the split CLAUDE.md calls wrong rather than strict: a page and its same-origin iframe are one
         similar-origin window agent, HTML puts them in one heap and then relies on it, and two heaps make
         `iframe.contentDocument.body.appendChild(x)` unrepresentable. */
      const key = clusterKeyOf(job.msg);
      const cluster = hostClusterOf(key);
      const docId = String(job.msg.documentId);
      /* BROWSER-SET, LIKE EVERY OTHER HALF OF THIS KEY: frame 0 is the group's top-level traversable. It is not
         the frame's claim about itself (SECURITY.md's reason for not recording an "isTop" a frame reports —
         a fenced frame is its own tree's root and would impersonate it); `sender.frameId` comes from the
         browser process, which is the same provenance as `sender.tab.url`. */
      const isTop = !job.msg.frameId;
      if (cluster) {
        /* THE CLUSTER'S INSTANCE ALREADY HOLDS THIS DOCUMENT, and that is a statement about the ENGINE rather
           than about this table. A same-origin sub-frame is created INSIDE that heap by §4.8.5's insertion
           steps (navigable.c mints its name, adopts it into this agent, and §7.4 step 14 loads its address
           through this same safeFetch chokepoint), so the instance is already running it as a realm of its own.
           The caller is therefore answered by that instance's finalize — its findings ARE this document's —
           which is the same path the RESHIP re-delivery of one document already took. What is deleted is the
           branch beside it that built a second WASM instance, i.e. a second heap for one agent.
           AND A RESERVATION ANSWERS THIS QUESTION EXACTLY AS AN INSTANCE DOES. A cluster whose instance is
           still being PROVISIONED is a cluster that has one — the pool slot was taken before the first await
           precisely so that this line cannot be reached during the boot and told "no" — so the join below
           attaches this caller to the record that is going to hold its document, and the boot answering later
           answers it too. That is the case the counter separates: `joinedBooting` is the window in which this
           branch used to be skipped and a second heap built for one similar-origin window agent. */
        DCHECK(!isTop || cluster.docId === docId,
               "the TOP document of a browsing-context group arrived for a cluster whose instance is rooted at " +
               "a different document — the group's top was REPLACED (a same-origin navigation, or a COOP " +
               "navigation that severed the group), so the new document is NOT one this instance is running " +
               "and attaching it reports the page that was replaced. HALF OF WHAT THIS NEEDS NOW EXISTS: " +
               "`qjs_join` puts the new document in this agent (engineJoin above). The half that does not is " +
               "§7.4's DESTROY A NAVIGABLE for the document being replaced, and joining without it leaves the " +
               "old document's realm, tree and parked flows running and reporting inside the same instance — " +
               "two tops for one traversable, which is a worse answer than this abort and is why the join is " +
               "not simply called here");
        cluster._resolvers.push(job);
        _waiting.splice(i, 1);
        if (cluster.state === "booting") _reserveStats.joinedBooting++; else _reserveStats.joinedRooted++;
        continue;
      }
      /* A SUB-FRAME NEVER ROOTS A CLUSTER — ITS EMBEDDER NAMES IT, AND WAITING IS HOW THIS ZONE LETS IT. A
         nested navigable is CREATED by the document that embeds it: §4.8.5's insertion steps mint its name in
         the embedder's engine, hand the page a WindowProxy under that name, and load its address through this
         same safeFetch chokepoint. The engine then either runs it as a REALM in its own heap (same-origin,
         navigable.c's `child_in_this_agent`, no notice) or announces it, and the announcement is what roots or
         joins its cluster HERE, under the engine's name.
         A CONTENT SCRIPT'S ARRIVAL FOR THAT SAME DOCUMENT IS A SECOND ROUTE TO IT, NEVER A SECOND DOCUMENT, so
         the one thing it may not do is NAME it. It used to, for the cross-origin case: the condition here was
         additionally `originOf(topLevelUrl) === origin`, so only a same-origin sub-frame waited and a
         cross-origin one raced its embedder — whichever arrived first named the document, and when the content
         script won, the embedder's later `navigable.create` for the very same frame found a cluster it could
         not recognise as holding it (`hostHolderOf` compares engine names) and the create arm aborted. Two
         names for one document is also unroutable in the direction that has no symptom here at all: the engine
         interns documents by NAME, so a `windowproxy.post` to `<embedder>.1` would reach an instance that has
         never heard of it. Naming is therefore ONE zone's job, and the browser's `documentId` is what JOINS the
         cluster (the branch above) rather than what names its document.
         `_isRealOrigin` IS WHAT KEEPS THIS OFF THE OPAQUE CASE, and it is the whole of what is left of the old
         condition: a sandboxed sub-frame's principal is a per-document token no embedder's notice can state
         (the create arm names that residual — the sandbox flag set is not yet carried on the notice), so such a
         document is correctly its own cluster of one and roots it here. */
      if (!isTop && _isRealOrigin(job.msg.origin)) { i++; continue; }
      _waiting.splice(i, 1);
      /* THE JOB IS ATTACHED TO THE RESERVATION BEFORE ANYTHING SUSPENDS, which is what makes the boot's failure
         path the ONE owner of every caller on this document. It used to be pushed after the await, so a
         creation that aborted answered this job here (`crashResult`) while any caller that had joined in the
         meantime was answered by nobody — two settlement owners for one record, split by timing.
         boot/creation abort: LOUD failure, not a quiet degenerate result, and that is engineBootFailed's now:
         it banners once, answers every attached caller with the crash record, destroys the frame and takes the
         reservation back out of the pool. An invariant abort from the creation path (the init return code, the
         bundle id, the frontier key's origin) is NOT a boot abort — it is this zone's contract with the engine
         breaking — so it travels on through `_readyP` to hostSchedule's own failure arm. */
      const eng = engineCreate(job.code, job.html, job.msg, job.persist, null, null, false);
      DCHECK(hostClusterOf(key) === eng,
             "engineCreate did not leave its reservation in the pool before returning — the whole point of " +
             "the slot being taken synchronously is that the next arrival for this cluster finds it, so a " +
             "pool that does not hold it is the race this reservation exists to close, still open");
      eng._resolvers.push(job);
      await eng._readyP;
    }
    // 2) ONE frontier: when no LIVE work is pending/running and RAM has headroom, rehydrate the highest-value
    //    COLD recipes into the SAME pool so they interleave with (and by) the one WFQ — not a second scheduler.
    if (!_waiting.length && !_pool.some((e) => !e._cold) && _admissionHasHeadroom()) {
      const cold = (await frontierAll()).filter((e) => e && e.recipes && !_pool.some((p) => p.fkey === e.key));
      cold.sort((a, b) => frontierWeight(b) - frontierWeight(a));   // one global value order
      for (const c of cold) {
        if (!_admissionHasHeadroom()) break;
        /* THE WHOLE PARKED DOCUMENT COMES BACK THROUGH ONE READER, and the recipe's flows resume inside it.
           `frontierDoc` is the same shape the park wrote, asserted the same way in the other direction, so
           the field the tier was missing (`responseHeaders` — the policy container) cannot go missing again
           from one end only. */
        const doc = frontierDoc(c, "came back from the cold tier");
        /* THE PRINCIPAL RESUMES WITH THE RECIPE. A parked flow's world is only the same world if the
           document it resumes into is the same PRINCIPAL — and this zone cannot re-derive one from
           c.sourceUrl without re-fabricating the tuple origin a sandboxed document does not have. The stamp
           site refuses to deliver a message for an empty one rather than inventing one. */
        /* A RESUMED RECIPE IS A CLUSTER OF ONE, and its GROUP says so rather than borrowing a tab's. The
           browsing-context group it parked in is gone — the tab was closed, or the session was — so there is
           no live document it may share a heap with, and the frontier key (origin|bundle, already unique per
           recipe and never equal to a tab id) is the honest name for the group it resumes into. That is also
           what lets the origin half stay "" for a recipe whose principal is empty: an empty half of a key
           whose other half is unique collides with nothing, so nothing is invented. */
        const msg = { type: "AST_ANALYZE", pageHtml: doc.html, code: doc.code, sourceUrl: doc.sourceUrl,
                      origin: doc.origin, groupId: "cold:" + c.key,
                      responseHeaders: doc.responseHeaders,
                      topLevelUrl: doc.topLevelUrl, credentialed: c.credentialed, persist: true };
        /* THE `try {} catch` AROUND THIS IS GONE WITH THE REPORTING IT DID. A rehydration whose engine ABORTS
           is now bannered by engineBootFailed, at the reservation, together with the pool slot it releases —
           one place on every creation path rather than one arm per call site. What was left in the catch was
           an invariant abort, which `RETHROW_FATAL` was already rethrowing, so the arm could only ever have
           caught something it immediately gave back. A rehydrated cold recipe always participates in the
           frontier and never has a caller, which is the `cold` argument. */
        /* THE STORED DOCUMENT IS PASSED AS IT WAS PARKED. `c.html || ""` stood here and it defeated
           engineRoot's own assert: an entry carrying no document became a page that parses to nothing, which
           reads as an origin whose parked flows found nothing rather than one that was never rebuilt. */
        await engineCreate(doc.code, doc.html, msg, true, null, null, true)._readyP;
      }
    }
  },
  finish: async (eng) => {   // fully explored, or self-parked under RAM pressure -> persist residue to the cold tier + resolve/merge
    /* A RESERVATION IS NEVER FINALIZED, AND THIS IS WHERE THAT IS SAID. Only the stepped engine reaches here
       and the step is over the hot set, so a booting record cannot arrive — but everything below it reads an
       instance that has answered (`eng.r` for the result document and the teardown, `eng.lines` for the
       findings, `eng.fkey` for the cold recipe), and a record that had none would report a document this zone
       never ran as a page that was analysed and found nothing. */
    DCHECK(eng.state === "hot",
           "an engine in state `" + eng.state + "` was finalized — finalize asks the instance for its one " +
           "result document and then tears it down, and a reservation has no instance to ask, so its " +
           "document would be reported as analysed and empty");
    const i = _pool.indexOf(eng); if (i >= 0) _pool.splice(i, 1);
    const result = await engineFinalize(eng);
    if (eng.persist && result._fkey) {   // persist into the GLOBAL frontier (cross-session cold tier)
      const prior = result._prior;
      await frontierPut(result._fkey, {
        /* THE TOP-LEVEL CREATION URL IS PART OF THE RECIPE, because a resumed flow must resume into the same
           ENVIRONMENT it parked in: §8.1.3.5 decides secure-context from it, so a rehydration that lost it
           would rebuild the realm with a different set of Web IDL §3.3.13 members and resume flows into a
           platform surface they never ran against. */
        /* AND SO IS THE PRINCIPAL, for the same reason one step down: the origin is browser-stated, this zone
           cannot re-derive it from the address (a sandboxed document's address lies about it), and a resumed
           instance that posts a cross-document message must stamp the origin the parked one had. */
        /* AND SO IS THE RESPONSE HEADER LIST — the same argument one step further out, and the one field a
           CSP-blocked sink verdict is decided against. It is the list engineRoot already relayed to qjs_init,
           carried verbatim; this zone never re-derives it, because no header is re-derivable from an address. */
        key: result._fkey, sourceUrl: eng.msg.sourceUrl, topLevelUrl: eng.msg.topLevelUrl, origin: eng.origin,
        responseHeaders: eng.msg.responseHeaders, html: eng.html, code: eng.code,
        /* `_park` AND `fetchCallSites` ARE GUARANTEED BY `linesToAnalysis` ON BOTH ARMS — asserted off the
           engine document when there is one, the host's own empty when the caller said there would not be —
           so a `|| []` here cannot fire. What it CAN do is exactly what this file has already been burnt by
           twice: outlive the guarantee. This entry is the cross-session frontier's residue, and the shape of
           the failure it would hide is "the recipes joined to the empty string", which reads to the next
           session as an origin that finished rather than one whose parked flows were dropped. */
        credentialed: !!eng.msg.credentialed, recipes: result._park.join(";"),
        emit: ((prior && prior.emit) || 0) + result.fetchCallSites.length, visits: ((prior && prior.visits) || 0) + 1, ts: Date.now(),
      });
    }
    if (eng._cold) {
      /* NO LIVE CALLER — a child document or a rehydrated cold recipe merges its findings to the moat here.
         The `try {} catch (_) {}` around this swallowed the merge's OWN assertions: _mergeFrontierResult
         DCHECKs the engine↔JS contract on the result it is handed, and that abort was landing in this catch
         and being discarded, which is the one place it could not be seen. */
      DCHECK(typeof self.onFrontierAdvance === "function",
             "the trusted zone has no onFrontierAdvance to merge a finalized engine's findings into — a child " +
             "document's and a resumed frontier's entire output has nowhere to go");
      self.onFrontierAdvance(eng.msg.sourceUrl, result);
    }
    /* EVERY CALLER WAITING ON THIS DOCUMENT IS ANSWERED, not just the first. A cold/child engine has no
       caller at all, which is why the list is allowed to be empty and the merge above is what its findings
       travel on instead. */
    for (const w of eng._resolvers) w.resolve(result);
    eng._resolvers.length = 0;
  },
};
function _hostKick() {
  if (_hostDriving) return;
  _hostDriving = true;
  /* THE ONE LOOP'S FAILURE IS NOT A DEBUG LINE. Every assertion in the bridge — the notice router's field
     counts and its DFAIL on an op this zone does not act on, the owed-request checks, the result document's
     shape, the merge's own contract check — is thrown from inside this loop, and `.catch(console.debug)` took
     the whole class and printed one line of it at a level nothing reads. Worse, it did so while the analysis
     promise for the document being stepped was still unresolved, so a violated invariant surfaced as a page
     that simply never finished. A LOUD banner, then rethrow in dev: an unhandled rejection is the honest
     shape of "the scheduler died", and it is impossible to overlook. */
  hostSchedule(_pool, _hostOps).catch((e) => {
    crashBanner("host-wfq", String((e && e.stack) || e));
    if (self.APICLIENT_DEV) throw e;
    /* RE-KICK ON THE POOL ALONE. `|| _waiting.length` stood here so a document held back by the RAM floor was
       picked up when a slot freed — but a freed slot is a pool that is not empty, and with an EMPTY pool RAM is
       free by definition, so the only document that can still be waiting is one admit DEFERRED (a sub-frame
       whose same-origin top has not reported yet). Re-kicking for that one spins the loop at full speed on a
       condition nothing in this file can change; the thing that changes it is the top's CONTENT_HTML, which
       kicks the pool itself when it arrives. */
  }).finally(() => { _hostDriving = false; if (_pool.length) _hostKick(); });
}
/* The offscreen kicks the pool on idle so parked COLD recipes get pulled in (admit re-checks the frontier)
   even when no new document arrives — the single ATTENTION keeps advancing across sessions. */
self.kickHostPool = _hostKick;

/* THE POOL, OBSERVED THROUGH THE BOUNDARY IT NOW RUNS BEHIND. renderer-host.js's `rendererProbe` proves the
   TRANSPORT — one frame, one document handed across as bytes, one number computed inside it and returned. This
   proves the PRODUCT: that the pool the extension actually analyses pages with builds its instances as
   renderers, that a real page's findings came out of one, and that the frames leave when the engines do.
   IT IS A READ, AND THAT IS DELIBERATE. A probe that started its own analysis would have to mint a
   documentId, a groupId, an origin and a frameId — the four facts SECURITY.md insists are BROWSER-STATED — so
   it would be exercising a message the browser never sent. The analysis it observes is the real one, driven
   from outside by `harness goto`, which is also why this is not a self-test in the sense §SECURITY.md warns
   about ("a host that cannot provision a second instance has not tested the transport").
   IT REPORTS THE FINISHED AS WELL AS THE LIVE, because an empty pool with no frames is what a clean teardown
   and an extension that never ran look like ALIKE — the counters are what tell them apart.
   AND IT REPORTS WHO ADMITTED, WHICH IS A DIFFERENT CLAIM FROM WHO HOLDS. Every renderer carries a ROUTING ID
   that came out of the registry, so that table is read and cross-checked against the frames this document has.
   A probe that only counted frames would read identically whether the instance had been admitted or merely
   built — the shape of number CLAUDE.md warns about — and the two asserts below the pool are what tell those
   apart; what they can and cannot prove now that the registry is a Map in this realm is stated there.
   IT IS SYNCHRONOUS AGAIN, AND THE PARAGRAPH THAT SAID OTHERWISE IS WHY THAT IS WORTH A LINE. It read "IT IS
   ASYNC FOR THAT REASON ALONE … the registry half is another process answering, which is a suspension by
   construction", and that reason went with the Worker. Every field below is a read of this realm, so nothing
   can interleave between the pool walk and the registry read, and the comparison is a fact rather than two
   snapshots taken at two moments. */
self.rendererPoolProbe = function rendererPoolProbe() {
  DCHECK(typeof self.rendererStats === "function",
         "renderer-host.js is not loaded in this zone — it is what obtains every engine's frame, so without " +
         "it the pool has no way to obtain an instance at all and this probe " +
         "would be reporting on an empty document");
  let booting = 0;
  const pool = _pool.map((eng) => {
    /* THE ASSERT THAT MAKES THIS PROBE WORTH RUNNING: not that a renderer exists, but that NOTHING ELSE does.
       An engine carrying a Module handle is a WASM instance living in the trusted zone's own realm with
       `HEAPU8` exported into it — the exact confinement-by-convention this conversion deleted — and it would
       be invisible from outside, because such an engine analyses pages perfectly well. */
    DCHECK(!("M" in eng),
           "a pooled engine still carries a Module handle — the in-realm path is deleted, so an engine holding " +
           "one is an untrusted instance built in this realm rather than in a sandboxed opaque-origin frame");
    /* A RESERVATION IS REPORTED AS ONE, and it is reported as a member of the pool because that is exactly
       what it is: the record answering "which instance holds this agent cluster" for the whole of a document
       fetch, a frame boot and three ABI round trips. Its two Level-1 facts read `null` rather than a number
       because it has stated neither — `heapBytes: 0` would be the default that admits another engine against
       RAM about to be spent, and `topWeight: -Infinity` would be the engine's own word for a DRAINED frontier
       on a record whose engine has not started. `joined` is how many callers are already waiting on it.
       `joinedDocIds` IS THE OTHER KIND OF JOINING AND THE TWO ARE DELIBERATELY NOT ONE FIELD: that one counts
       CALLERS waiting on this document's findings, this one names the DOCUMENTS this instance holds beyond the
       one it was rooted at (main.c's `g_joined_ctx`). An instance is an agent CLUSTER, so a pool of two entries
       can be a page of four documents — and without this the ones a `qjs_join` added are invisible from
       outside, which is exactly the shape of number that reads identically whether the join happened or the
       child was silently never hosted. */
    if (eng.state === "booting") {
      booting++;
      DCHECK(typeof eng.cluster === "string" && eng.cluster !== "" && typeof eng.docId === "string" && eng.docId !== "",
             "a reservation is in the pool naming no agent cluster or no document — it is answered for by " +
             "both (hostClusterOf for admission, hostHolderOf for routing), so a nameless one is a slot that " +
             "blocks admission while answering nobody");
      return { name: eng.cluster, docId: eng.docId, state: "booting",
               framed: !!(eng.r && eng.r.frame.parentNode), routingId: eng.r ? eng.r.routingId : null,
               heapBytes: null, topWeight: null, cold: !!eng._cold, joined: eng._resolvers.length,
               joinedDocIds: eng.joinedDocIds.slice() };
    }
    /* THE WORKING SET IS READ WHERE THE POOL RECORDED IT AND NOT OFF THE RENDERER, which is what the typing
       moved: `workingSetBytes` is a declared reply field of every ABI method, so engineRecordFacts takes it
       from the reply it already awaited and there is no second copy on the transport for this to disagree
       with. The invariant is unchanged and is the same one `_residentBytes` sums against — a live engine has
       reported, because engineRecordFacts runs before the record leaves the `booting` state. */
    DCHECK(eng.r && typeof eng.residentBytes === "number" && typeof eng.r.name === "string",
           "a pooled engine is not backed by a renderer that has reported itself — every instance is obtained " +
           "by rendererLaunch, which does not return until the registry has admitted its agent cluster and the " +
           "frame's invitation acceptance has landed, and engineRecordFacts states its working set before the " +
           "reservation becomes hot");
    /* THE ROUTING ID IS REPORTED PER ENGINE BECAUSE IT IS THE ONE FIELD NEITHER THIS FILE NOR THE FRAME
       PRODUCED. The cluster name is computed here, the doc id is minted here and the heap figure is stated by
       the frame — but the id came out of the renderer registry, so it is the evidence that this instance was
       admitted rather than merely built. */
    DCHECK(typeof eng.r.routingId === "number",
           "a pooled engine's renderer carries no routing id — an id is minted by the renderer registry when " +
           "it decides an agent cluster gets an instance, so an instance without one is a frame this document " +
           "created for itself");
    return { name: eng.r.name, docId: eng.docId, state: eng.state, framed: !!eng.r.frame.parentNode,
             routingId: eng.r.routingId, heapBytes: eng.residentBytes, topWeight: eng.topWeight,
             cold: !!eng._cold, joined: eng._resolvers.length, joinedDocIds: eng.joinedDocIds.slice() };
  });
  /* NO RESERVATION OUTLIVES ITS PROVISIONING, ASSERTED AS ARITHMETIC RATHER THAN LOOKED FOR. A reservation is
     made once and takes exactly one exit — it roots into an instance or it fails — so `made - rooted - failed`
     is the number of provisionings actually RUNNING, and the pool may never hold more booting records than
     that. The failure it catches is the one that would otherwise be silent forever: a provisioning that ended
     without transitioning or releasing its record leaves a phantom holding an agent cluster, and every later
     arrival for that cluster joins it and waits on a boot that finished long ago.
     IT IS `<=` AND NOT `===` BECAUSE hostClear IS ALLOWED TO TAKE A RESERVATION OUT FROM UNDER ITS OWN BOOT:
     the Clear splices the record and the provisioning removes the frame when it lands, so between those two
     moments a running provisioning legitimately has no pool slot. Equality would report the Clear path as the
     phantom, which is the direction that cannot happen. */
  const inFlight = _reserveStats.made - _reserveStats.rooted - _reserveStats.failed;
  DCHECK(booting <= inFlight,
         "the pool holds " + booting + " reservation(s) while only " + inFlight + " provisioning(s) are still " +
         "running (" + _reserveStats.made + " made, " + _reserveStats.rooted + " rooted, " +
         _reserveStats.failed + " failed) — the difference is a slot held by a record whose boot is over, " +
         "which blocks admission forever and answers every later arrival for that agent cluster with a wait " +
         "that never ends");
  /* ── WHO DECIDED, AND WHAT THAT STILL PROVES NOW THAT IT IS ONE REALM. `rendererStats()` is what
     renderer-host.js HOLDS — its `_live` set, itself checked against the DOM on the line above. `getRegistry()`
     is what render-process-host.js DECIDED — a table keyed by agent cluster, mutated by four transitions, each
     of which asserts the table's whole arithmetic before it returns.
     IT NO LONGER SPANS TWO PROGRAMS, AND THAT IS SAID HERE RATHER THAN LEFT TO BE INFERRED. While the registry
     was a WASM module behind a Worker, an id that table had never minted was evidence in the strongest sense:
     it came out of another address space. It is a `Map` in this realm now, so this comparison cannot
     distinguish "the registry decided" from "renderer-host decided and told the registry". What it still
     proves is everything else, and it is not small: two components keep two independent records of one set,
     over two different keys (cluster, and object identity beside the DOM), so a frame created without an
     admission, an admission whose renderer is gone, a transition counted without its slot and a slot freed
     without its counter are each a disagreement here. What makes the INVERSION hold instead is structural and
     one line long — `registerRenderer` has exactly one caller, and that caller is the only path in this
     extension to a renderer frame.
     THERE IS NO `provisioned` ARM ANY MORE, and its absence is the point: the registry is a module of this
     document, so it exists from the moment this script has run. The arm it replaces guarded a Worker that
     might not have been started yet, and an empty registry read through it was indistinguishable from a
     document that had never asked for a renderer.
     IT IS SYNCHRONOUS, WHICH IS WHY THE COMPARISON IS A FACT RATHER THAN A RACE. It used to be a mojo call,
     deliberately on the SAME pipe as `RendererTerminated` so that a termination already posted was processed
     before this read. In one run-to-completion realm nothing can interleave between the two reads at all. */
  const renderers = self.rendererStats();
  DCHECK(!!self.renderProcessHost,
         "this document holds " + renderers.forked + " forked renderer(s) with no renderer registry in the " +
         "realm — every routing id comes out of that table, so frames existing without it is a document that " +
         "materialized renderers on its own authority");
  const registry = self.renderProcessHost.getRegistry();
  const mine = renderers.routingIds.join(",");
  DCHECK(registry.routingIds === mine,
         "the renderer registry holds renderers [" + registry.routingIds + "] while this document holds " +
         "frames for [" + mine + "] — the two are one set: an id here that is not there is a frame nothing " +
         "admitted, and an id there that is not here is an agent cluster refused an instance forever because " +
         "the renderer holding it is already gone");
  DCHECK(registry.launched + registry.failed === renderers.forked,
         "the renderer registry admitted " + (registry.launched + registry.failed) + " renderer(s) (" +
         registry.launched + " launched, " + registry.failed + " failed) while this document forked " +
         renderers.forked + " — a frame is materialized by one admission and by nothing else, so a difference " +
         "is a frame created outside that path or an admission whose fork never began");
  return { renderers: renderers, registry: registry, mojo: self.mojo.stats(),
           pool: pool, waiting: _waiting.length, residentBytes: _residentBytes(),
           reservations: { made: _reserveStats.made, rooted: _reserveStats.rooted, failed: _reserveStats.failed,
                           booting: booting, inFlight: inFlight, peakBooting: _reserveStats.peakBooting,
                           joinedBooting: _reserveStats.joinedBooting, joinedRooted: _reserveStats.joinedRooted },
           runs: self._engineLog.length, recent: self._engineLog.slice(-4),
           crashes: self._engineCrashOccurred };
};

/* Endpoint IDENTITY (exact dedup by method+hole-normalized-url + shape/concrete collapse with path-param
   examples) is the ENGINE's job now — it emits an already-deduped fetchCallSites in @RESULT. The host
   mergeCallsites/dedupShapeConcrete/pathSegs were DELETED. */

/* WIPE EVERYTHING THIS ZONE IS HOLDING OR WILL RESUME. The Clear button's contract is "stop ALL work and
   delete ALL data", and it ran through an AST_CLEAR that this entry answered "unknown type" to while the
   sender swallowed the refusal — so every live engine kept running straight through the wipe and merged its
   findings back into the store the user had just emptied, and the parked frontier in `apiclient-frontier` was
   never touched at all, so the next idle kick rehydrated the cleared origins from IDB. Both halves are the
   operation: the HOT engines leave the pool (hostSchedule breaks the moment it is empty, and an engine no
   longer in the pool is never stepped, never finalized and so never persists its residue), and the COLD tier
   is emptied. A document still waiting for a slot is REJECTED with "cleared", which is the exact error
   _dispatchDocument reads to abandon its tail without recording a page-level failure. */
async function frontierClear() {
  try {
    const db = await idbOpen();
    await new Promise((res, rej) => { const t = db.transaction("frontier", "readwrite").objectStore("frontier").clear(); t.onsuccess = () => res(); t.onerror = () => rej(t.error); });
  } catch (e) { RETHROW_FATAL(e); frontierFail("clear", e); }
}
async function hostClear() {
  const waiting = _waiting.splice(0);
  const dropped = _pool.splice(0);
  for (const job of waiting) job.reject(new Error("cleared"));
  for (const eng of dropped) { for (const w of eng._resolvers) w.reject(new Error("cleared")); eng._resolvers.length = 0; }
  /* AND THE FRAME GOES WITH THE ENGINE, which a dropped Module never needed: an engine that leaves the pool is
     never stepped and never finalized, so nothing else will ever reach its renderer — and an iframe nobody
     reaches is a whole WASM instance held resident under a document that does not reload until the browser
     restarts. Two Clears with a heavy page open would have left two of them.
     A ROUND IN FLIGHT KEEPS ITS FRAME UNTIL IT LANDS. `state === "fetching"` is exactly "this engine has a call
     outstanding", and destroying then would break renderer-host's rule that a destroyed renderer has no caller
     parked on an answer. `_dropped` is what the round reads on the way out (see serviceFetch). */
  /* AND A RESERVATION KEEPS ITS FRAME FOR THE SAME REASON, WHICH IS WHY THIS IS WRITTEN AS THE POSITIVE SET
     RATHER THAN AS `!== "fetching"`. A booting engine has no `r` at all until rendererLaunch answers, and from
     that instant until engineRoot finishes it has an ABI call outstanding on every await in between — so it is
     the same case the negative form would have missed twice over (a TypeError, then a broken renderer-host
     rule). engineCreate reads `_dropped` when its provisioning lands and removes the frame there, on both
     arms, exactly as the service round does. */
  for (const eng of dropped) { eng._dropped = true; if (eng.state === "hot") eng.r.destroy(); }
  /* EVERY OUTSTANDING RENDEZVOUS BELONGED TO AN INSTANCE THAT IS NOW DROPPED — the whole pool went. Left
     behind, each entry holds its asking engine's wasm Module alive for the life of the offscreen, which is the
     one shape a map keyed on a live instance fails in. */
  _remoteOps.clear();
  /* AND THE RUN LOG GOES, BECAUSE IT IS A LIST OF PAGE ADDRESSES. Each record carries the `sourceUrl` of a
     document this zone analysed, so `_engineLog` is a browsing history held in the offscreen realm — the one
     thing Clear's stated contract ("delete ALL extension data") is most clearly about — and it survived every
     Clear there has ever been: declared at load, emptied by nothing, so the popup dropped its copy and the very
     next loadState fetched the same URLs straight back out of GET_ENGINE_RUNS. Truncated in place rather than
     reassigned: the array is the identity popup-handlers.js asserts is present (a fresh one would be a second
     array, and an absent one is this file never having loaded, which is what that assert distinguishes).
     `_engineCrashOccurred` DELIBERATELY DOES NOT GO. It is a count of aborts with no page identity in it — this
     document's own record that a WASM instance died — and a wipe that could zero it is a wipe that can silence
     a crash the reviewer has not seen yet. A crash must be impossible to overlook, including across a Clear. */
  self._engineLog.length = 0;
  await frontierClear();
  return dropped.length;
}

self.astDispatch = async function astDispatch(msg) {
  try {
    /* TWO TYPES, AND A THIRD IS AN UNBUILT CAPABILITY THAT SAYS SO. This entry used to answer every type but
       one with `{success:false, error:"unknown type"}`, and the senders wrapped their calls in catches — so
       AST_CLEAR (the Clear button's "stop all work") and SET_ANALYSIS_OPTS (the popup's Settings panel) were
       both refused in silence for as long as they existed. AST_CLEAR is now BUILT below; SET_ANALYSIS_OPTS,
       its two controls and its persisted record are DELETED, because neither knob may exist: the working set
       is bounded by resident WASM memory and not by a user-set instance count, and a wall-clock throttle over
       the cooperative quantum is a step cap. What is left is a real refusal, so it aborts rather than
       reporting a false success to a caller that will not look. */
    DCHECK(!!msg && (msg.type === "AST_ANALYZE" || msg.type === "AST_CLEAR"),
           "the trusted zone dispatched a type this bridge does not answer: `" + (msg && msg.type) + "` — " +
           "every edge into the engine is built here, so an unanswered type is a capability that was asked " +
           "for and never made, not an option the caller may proceed without");
    if (!msg) return { success: false, error: "dispatch with no message" };   // release path under the assert
    if (msg.type === "AST_CLEAR") return { success: true, result: { cleared: await hostClear() } };
    if (msg.type !== "AST_ANALYZE") return { success: false, error: "unknown type " + msg.type };
    const html = msg.pageHtml || "";
    const code = msg.code || "";           // any brain-assembled scripts (usually empty); the DOM carries the page
    // NOTHING TO RUN, so no engine runs and there is no result document to expect — this is the one analysis
    // that legitimately has none, and it says so rather than sharing the absent-document path with a real run.
    if (!html && !code) return { success: true, result: linesToAnalysis([], msg, false) };

    /* ENQUEUE this document into the LIVE host WFQ pool. Its wasm instance interleaves in SLICES with every
       other open document by value-of-information — no run-to-completion, no recency monopoly. The per-doc
       promise resolves when THIS engine finalizes (fully explored or host-evicted); the pool persists its
       residue to the GLOBAL frontier (cross-session cold tier). msg.persist enables that IDB persistence. */
    /* AND IT ARRIVES CARRYING THE BROWSER'S NAME FOR IT. `admit` asks the pool whether an instance already
       holds this document before it builds one, which is what keeps the RESHIP re-delivery from putting two
       instances behind one principal — and a job with no documentId skips that question SILENTLY, seating a
       second engine with nothing to say so. Every live document comes from _dispatchDocument, which reads the id off
       the browser-provided sender; the two callers that legitimately have no browser document (a child
       navigable a peer engine announced, a rehydrated cold recipe) build their engine directly and never
       reach this line. */
    DCHECK(!!msg.documentId,
           "an AST_ANALYZE for a live document carried no documentId — it is the name the pool answers " +
           "\"which instance holds this document?\" by, so without it a re-delivered document is seated twice");
    /* AND IT ARRIVES CARRYING ITS AGENT CLUSTER, WHOSE BOTH HALVES ARE BROWSER-STATED. These are asserted here,
       where the facts are BORN, rather than at the pool: a missing group would silently put every document of
       one origin in every tab into ONE heap behind ONE principal, and a missing origin would do the same to
       every document of one tab — both of which look exactly like the design working. `frameId` is asserted for
       its PRESENCE and not its value, because 0 is the top frame and `undefined` would read as one. */
    DCHECK(msg.groupId != null && msg.groupId !== "",
           "an AST_ANALYZE for a live document named no browsing-context group — an instance is its " +
           "(group, origin) agent cluster, and _dispatchDocument carries the browser's sender.tab.id as the group");
    DCHECK(typeof msg.origin === "string" && msg.origin !== "",
           "an AST_ANALYZE for a live document named no principal — the cluster's origin half is the browser's " +
           "MessageSender.origin (opaque-unique via _senderOrigin), and an empty one collapses every document " +
           "of this tab into one heap; it is never derived from the address, which a sandboxed document's lies about");
    DCHECK(typeof msg.frameId === "number",
           "an AST_ANALYZE for a live document carried no frameId — the pool reads it to tell a sub-frame " +
           "(which its cluster's instance already runs) from the group's TOP document (which it does not), and " +
           "an absent one is indistinguishable from frame 0");
    const persist = !!msg.persist;
    /* THE WAITER CARRIES BOTH SETTLERS. A Clear must be able to tell a document that was never seated that its
       analysis is not coming, and "cleared" is the exact error _dispatchDocument reads to abandon its tail without
       recording a page-level failure — resolving it with a plausible empty result instead would report the
       wiped page as analysed and clean. */
    const result = await new Promise((resolve, reject) => { _waiting.push({ code, html, msg, persist, resolve, reject }); _hostKick(); });
    return { success: true, result };   // result.fetchCallSites is already deduped by the engine
  } catch (e) {
    /* AN INVARIANT ABORT IS NOT AN ANALYSIS FAILURE. This catch reports a failed dispatch to the brain, which
       records it as `_astError` and moves on — the right handling for a page that could not be analysed, and
       the wrong handling for a DCHECK, which would arrive as a string in a debug line and let the zone carry
       on with a contract it has already proved is broken. */
    RETHROW_FATAL(e);
    return { success: false, error: String(e && e.message || e), stack: e && e.stack };
  }
};

console.debug("[ast-worker v2] bridge ready (self.astDispatch + self.rendererPoolProbe installed)");

/* Node-only: expose the PURE host-WFQ policy (no wasm) for deterministic unit tests of ordering/
   eviction/non-blocking-fetch. Never runs in the worker (no `module`). */
if (typeof module !== "undefined" && module.exports) module.exports = { hostSchedule, engineWeight };
