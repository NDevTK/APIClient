/* bridge.js — the v2 HOST BRIDGE, the ONLY irreducible trusted-zone JS (SECURITY.md).
 *
 * Extracted OUT of offscreen-brain.js so the bridge and the (to-be-deleted) analysis logic are no
 * longer in one file. This is the platform edge the lexbor+quickjs engine CANNOT be, because the engine
 * is the UNTRUSTED WASM: it loads the engine WASM (dynamic import), drives the qjs_step protocol,
 * safe-fetches replies/chunks (the safeFetch chokepoint the untrusted bundle must never bypass),
 * persists the cross-session frontier to IndexedDB, and JSON.parses the engine's ONE @RESULT. It
 * installs self.astDispatch (+ self.kickHostPool) for the offscreen to call. NO analysis LOGIC here —
 * the C engine owns identity/dedup/detection; this is only the network/IDB/WASM edges.
 */
/* The engine WASM factory is an ES module; this bridge lives in the CLASSIC offscreen-brain.js, so load
   it via dynamic import() (cached), NOT a static top-level import. Same-origin under the offscreen CSP. */
let _createQJSp = null;
// The ROOT DOCUMENT NAME for each engine this offscreen owns — see the qjs_init call below. Only the roots are
// named here: a document an engine CREATES (an iframe's child navigable, a popup) names itself "<parent>.<n>",
// which is what lets HTML 4.8.5 create a child navigable inside the insertion steps without asking this zone.
let nextDocumentId = 0;
function getCreateQJS() { return (_createQJSp || (_createQJSp = import("./lib/qjs/qjs.mjs").then((m) => m.default))); }

/* A URL with an opaque HOLE ("{}"/"{tag}") is not concretely fetchable (used to gate reply/chunk fetch).
   Endpoint IDENTITY (hole-normalization, shape/concrete collapse) is the ENGINE's now — the host's
   normHoles/SEG_HOLE/pathSegs/mergeCallsites/dedupShapeConcrete were DELETED. */
const HOLE = /\{[a-z]*\}/;
const hasHole = (s) => HOLE.test(s || "");

/* Map the engine's ONE structured `@RESULT <json>` line -> the analysis object the brain consumes.
   The ENGINE builds + DEDUPS the whole result (endpoints/params/headers/body, @S sinks, chunkUrls,
   errors, park recipes) and JSON.stringifies it; the host does ONE JSON.parse and relays — NO per-line
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
  DCHECK(r.probeResults && typeof r.probeResults === "object" && !Array.isArray(r.probeResults),
         "the engine's result document carries no probeResults map — req2proto.c serializes the schemas the " +
         "APIs' own rejections described into that field, keyed by the `<METHOD> <host><path>` identity the " +
         "Send panel resolves a request body with. An absent one is every learned field, service, method and " +
         "OAuth scope arriving as nothing, and the Send panel offering an empty body for an endpoint the " +
         "server has already described");
  DCHECK(typeof r._switches === "number",
         "the engine's result document carries no _switches count — it is the one OBSERVABLE that the single " +
         "BFS actually context-switches rather than running its flows FIFO");
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
        extraErrors.push({ context: "result-parse", message: String(e && e.message || e), snippet: null, replyExample: null });
      }
    } else if (ln.startsWith("@E ")) {
      extraErrors.push({ context: "engine", message: ln.slice(3), snippet: null, replyExample: null });
    } else if (ln.startsWith("@WHY ")) {
      // engine diagnostic for a zero-result/resource path (e.g. reg_oom) — surface it so an OOM or
      // aborted flow never fails SILENTLY (CLAUDE.md: every zero-result path emits @WHY).
      try { const o = JSON.parse(ln.slice(5)); extraErrors.push({ context: o.phase || "why", message: o.reason || ln.slice(5), snippet: null, replyExample: null }); }
      catch (_) { extraErrors.push({ context: "why", message: ln.slice(5), snippet: null, replyExample: null }); }
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
  result = result || {};
  /* THE SCHEDULER'S OWN COUNTERS, so fairness/deep-preemption is OBSERVABLE (a real signal that the single BFS
     context-switches rather than running FIFO) — and they are the fields solver/result.c ACTUALLY emits. This
     read `_orphans`, `_work` and `_parked`, which nothing on the engine side has ever written, so the whole
     diagnostic reported three zeroes forever and the `|| 0` beside each is what made that invisible. NOT
     wrapped in a swallowing try/catch: every value here is a number off a document already asserted above. */
  const m = { switches: result._switches || 0, flows: result._flows || 0, candidates: result._candidates || 0,
              jobsQueued: result._jobsQueued || 0, jobsRun: result._jobsRun || 0,
              worldSegments: result._worldSegments || 0, park: (result._park || []).length,
              resumed: resumed, url: (msg && msg.sourceUrl) || "" };
  self._engineMeta = m;
  // A per-run LOG (not a single overwritten global): concurrent cold-kick engines each report here, so the
  // full park->persist->rehydrate->resume SEQUENCE across all engines is observable, not just the last one.
  (self._engineLog = self._engineLog || []).push(m);
  if (self._engineLog.length > 200) self._engineLog.shift();
  return {
    _switches: result._switches || 0,
    fetchCallSites: result.fetchCallSites || [],
    /* THE ENGINE'S OWN PAGE ERRORS, WHICH IT CALLS `pageErrors`. This line read `resolverErrors` — a name
       nothing on the engine side has ever written — so every error the engine recorded while running the page
       was dropped here, and the `|| []` beside it is precisely what made the drop invisible: the field the
       brain reads existed, held the host's own errors, and looked complete. The consumer (popup.js,
       _dispatchDocument) reads {context, message, snippet}; the engine's are strings. */
    resolverErrors: (result.pageErrors || []).map((e) => ({ context: "page", message: String(e), snippet: null, replyExample: null })).concat(extraErrors),
    /* The engine does NOT carry chunk URLs in the result document — they cross on their own ABI edge
       (qjs_chunks, drained and fetched every service round), so this is a host-side empty like the block
       below and never a defaulted engine field. */
    chunkUrls: [],
    securitySinks: result.securitySinks || [],
    /* THE SCHEMAS THE ENGINE LEARNED FROM THE APIs' OWN REJECTIONS — engine/host/solver/req2proto.c. Relayed
       VERBATIM, keyed exactly as the engine keyed it, because the key IS the endpoint identity `lib/send.js`
       looks a body schema up by: a host that re-keyed here would be the host owning structure again. NOT
       defaulted — the field is asserted above, and `|| {}` here would turn "the engine emitted nothing" into
       "this API describes no fields", which is a finding rather than a hole. */
    probeResults: result.probeResults,
    // sibling fields the brain reads unconditionally, present + empty so it never throws:
    protoEnums: [], protoFieldMaps: [], dangerousPatterns: [],
    esmImportUrls: [], inRunModuleUrls: [], domEndpoints: [],
    sourceMapTypes: [], sourceMapsByUrl: {}, traceMapsByUrl: {}, valueConstraints: [],
    sourceMapUrl: null, sourceMap: null, sourceUrl: (msg && msg.sourceUrl) || "",
    _orphans: result._orphans || 0, _emitDone: result._emit != null ? ("emit=" + result._emit) : "",
    _replyWant: [], _park: result._park || [],
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
function idbOpen() {
  return new Promise((res, rej) => {
    const r = indexedDB.open("apiclient-frontier", 1);
    r.onupgradeneeded = () => { r.result.createObjectStore("frontier"); };
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}
/* A frontier entry (the GLOBAL union spans all origins): { key: origin|hash, sourceUrl, topLevelUrl, html, code,
   recipes: "idx,dec;...", emit, visits, ts, credentialed }. Rehydration re-runs (html,code) + resumes
   recipes -- so a parked flow on ANY site can be advanced later, even when that page isn't open. */
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
   over flows WITHIN a document, now over engines ACROSS documents. RAM is the bound: at most POOL_CAP hot
   engines resident; the lowest-weight one is EVICTED (qjs_park -> replay recipe in IDB) under pressure, and
   the cold tail is rehydrated INTO this same pool by admit (one WFQ, no second loop). Fetches don't block the pool: an engine awaiting a
   reply is 'fetching' and skipped until its body lands, so a slow fetch on doc A never stalls doc B.
   ──────────────────────────────────────────────────────────────────────────────────────────────────── */
// The hot working set is bounded by ACTUAL RAM, not a fixed instance count: admit a new document engine
// while resident WASM memory is under the budget (a light page's instance is a few MB, a heavy bundle's is
// tens — a count would ignore that). Over the budget, new docs wait as cold recipes -> IDB, pulled back into this pool by admit.
// This is the RAM floor (like the disk floor), not a truncating bound. Always admit >=1 so a lone doc runs.
const HOT_RAM_BUDGET = 512 * 1024 * 1024;   // bytes of summed live WASM memory before new engines wait
function _residentBytes() { let b = 0; for (const e of _pool) { try { b += (e.M && e.M.HEAPU8) ? e.M.HEAPU8.length : 0; } catch (_) {} } return b; }

// ---- Engine lifecycle over ONE wasm instance (one document) ----
/* `docName` is set ONLY for a document another engine CREATED — its name arrived in that engine's
   navigable.create notice, minted there because HTML §4.8.5 creates a child navigable inside the insertion
   steps and cannot ask this zone for a name. A root document is named here, by the counter above. */
/* `topLevelUrl` is HTML §8.1.3.1's TOP-LEVEL CREATION URL for this document's environment. A ROOT document is
   its own top-level traversable, so its address is it; a document a peer engine CREATED carries its creator's
   decision on the navigable.create notice. It is a separate argument from the address because one WASM
   instance is one DOCUMENT regardless of origin — this document may be NESTED in a document of another
   instance, and §8.1.3.5 decides secure-context from the TOP of that chain. */
async function engineCreate(code, html, msg, persist, docName, topLevelUrl) {
  const lines = [];
  const createQJS = await getCreateQJS();
  // @DBG is the ONLY dev-trace channel: routed to console.debug, NEVER into `lines` — so it is never parsed
  // as @E/@RESULT and never pollutes resolverErrors. @E/@WHY stays STRICTLY for fatal should-never-happen
  // states (they abort); a diagnostic must never masquerade as one. (CLAUDE.md: @WHY is fatal, not a log.)
  const sink = (s) => { try { if(typeof s==="string" && s.indexOf("securitySinks")>=0){var m=s.match(/"poc":"([^"]{0,40})"/);console.debug("XR poc="+(m?m[1]:"NONE"));} } catch(_){} lines.push(s); };
  // Engine STDERR (printErr) — @E/@WHY aborts, AddressSanitizer reports, native diagnostics — is TEE'd to
  // console.debug so it is CAPTURABLE live (harness `diag`) and never silently lost when the crashed engine's
  // `lines` are discarded; it still lands in `lines` for the crash record. This is what makes the `asan` build
  // usable: ASan prints its UAF/double-free report (alloc + free stacks) to stderr, which surfaces here.
  const errsink = (s) => { try { console.debug(s); } catch (_) {} lines.push(s); };
  const M = await createQJS({ print: sink, printErr: errsink, noInitialRun: true });
  const ptrs = [];
  const cstr = (s) => { const n = M.lengthBytesUTF8(s || "") + 1; const p = M._malloc(n); M.stringToUTF8(s || "", p, n); return p; };
  const arg = (s) => { const p = cstr(s); ptrs.push(p); return p; };
  // PHASE 1 — parse + boot; the engine computes the stable bundle IDENTITY from its Lexbor <script> scan.
  // The real HTTP Content-Security-Policy RESPONSE HEADER (captured same-origin by content.js fetch(location.href),
  // lowercased) is the PRIMARY policy the engine uses for policy-relative XSS verdicts — header-CSP overrides
  // the <meta> scan, which alone missed the header entirely (a header-CSP-blocked sink was reported exploitable).
  const _csp = (msg && msg.responseHeaders && msg.responseHeaders["content-security-policy"]) || "";
  // THE DOCUMENT ID — the ROOT one, because an instance is an agent CLUSTER and holds one realm per same-origin
  // document. A flow that scripts a CROSS-ORIGIN iframe or popup writes state in a PEER instance (a same-origin
  // one is a realm in this same heap and needs no peer at all), and that peer keys its segment of the flow's
  // world by an id minted here. The offscreen mints it because SECURITY.md makes the offscreen the only zone that knows which
  // instance holds which document — the same reason it owns the routing. The engine rejects 0.
  // SCOPE, stated rather than assumed: this counter is unique across the instances ALIVE in this offscreen,
  // which is exactly the set that can message each other today. It is not yet persisted, because a parked
  // foreign segment does not yet outlive a session; when segments park, this becomes a persisted allocator.
  /* A LIVE ROOT DOCUMENT IS NAMED BY THE NAME THE BROWSER ALREADY GAVE IT. The counter answered "which
     document is this?" with a fresh number every time it was asked, so the pool could not tell one document
     delivered twice (content.js re-ships its CONTENT_HTML when the offscreen brain broadcasts RESHIP) from
     two documents, and built a second WASM instance for it — two instances behind one principal, which is
     the one thing SECURITY.md's "one instance per ORIGIN-KEYED AGENT CLUSTER" forbids. `hostHolderOf` was
     already the answer to that question for a document a peer engine CREATED ("already provisioned: the
     engine announces a document once"); naming the root by its own documentId is what lets it be asked for
     a root too. A rehydrated cold recipe has no browser document to name it, and takes the counter. */
  const _docId = docName || (msg && msg.documentId ? String(msg.documentId) : String(++nextDocumentId));
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
  const _initrc = M.ccall("qjs_init", "number", ["number", "number", "number", "number", "number"],
    [arg(html || ""), arg((msg && msg.sourceUrl) || ""), arg(_docId), arg(_csp), arg(_tlu)]);
  DCHECK(_initrc === 0, "qjs_init reported a failure this zone has no handling for — the engine's own entry " +
                        "CHECKs every precondition and aborts, so a non-zero return is a contract that changed");
  /* NEVER 0 — document_bundle_id folds an empty scan to 1 precisely so that a 0 cannot mean two things. A 0
     here would key every unidentifiable document to the SAME frontier entry, so one page's parked residue
     would resume in another's engine. */
  const _bidRaw = M.ccall("qjs_bundle_id", "number", [], []) >>> 0;
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
  M.ccall("qjs_begin", "void", ["string"], [(prior && prior.recipes) ? prior.recipes : ""]);
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
     `{status, statusText, headers: [[name, value], …], body, urlList}`, the same record the C hosts build with
     fetch_reply_new. It used to hand back the BODY'S BYTES alone, so everything this zone had actually seen was
     dropped at this line and re-invented on the other side: the engine reported status 200, status message
     "OK", no headers, and — because Fetch §2.2.6's URL LIST is what `response.url` and `response.redirected`
     are — no redirect, ever, for any reply. `null` is a NETWORK ERROR (the engine rejects with §5.6's
     TypeError), which is what a URL this zone must not or cannot fetch honestly is; it is NOT an empty 200. */
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
         that this zone's own chokepoint answered something it never answers. */
      DCHECK(r && typeof r === "object" && typeof r.body === "string" && typeof r.status === "number",
             "safeFetch answered with something other than its reply record — the engine builds a Response " +
             "out of this and a page reads status/headers/body off it");
      /* §2.2.6's URL list, straight from the chokepoint that performed the fetch. safeFetch always reports at
         least the URL it requested — §4.1's "If internalResponse's URL list is empty, then set it to a clone of
         request's URL list" — and the engine DCHECKs that at both ends, so an empty one is a bug here rather
         than a response that silently claims never to have redirected. */
      DCHECK(Array.isArray(r.urlList) && r.urlList.length >= 1,
             "safeFetch answered a reply with no URL list — response.url and response.redirected are read off " +
             "nothing else, and the engine would report every redirect as none");
      DCHECK(r.headers && typeof r.headers === "object",
             "safeFetch answered a reply with no header map — the engine's Headers record is built from it");
      /* STATUS 0 IS NOT A REPLY. It is the one status no HTTP response has, and it is exactly what the
         chokepoint answers when the request never went on the wire at all (bad URL, blocked scheme, blocked
         private target, CORB). This comment's own rule — «`null` is a NETWORK ERROR, which is what a URL this
         zone must not or cannot fetch honestly is» — names that case, and the record was crossing anyway: the
         page saw a real reply whose status was a number no server returns, instead of §5.6's TypeError. */
      if (r.status === 0) return null;
      return { status: r.status, statusText: r.statusText || "", headers: Object.entries(r.headers),
               body: r.body, urlList: r.urlList };
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
  // sink gets reported as exploitable. A load that does not load is `{body:null}`, which is a navigable that
  // still exists showing an error page, exactly as the engine's own child_document reads it.
  const fetchedDocument = async (u) => {
    if (!canFetch) return { body: null, csp: null };
    try {
      const abs = new URL(u, msg.sourceUrl).href;
      // Never `as:"script"` — these bytes are PARSED as a document, not run as code — and never credentialed:
      // §7.4's fetch is a navigation this engine initiated, not a learned GET being replayed for its reply.
      const r = await self.safeFetch(abs, { pageUrl: msg.sourceUrl });
      DCHECK(r && typeof r === "object" && typeof r.body === "string" && r.headers && typeof r.headers === "object",
             "safeFetch answered a document load with something other than its reply record — §7.4 step 14 " +
             "reads the BODY and the POLICY off it, and a Document judged under no policy is how a page whose " +
             "CSP kills a sink gets reported as exploitable");
      /* NOT OK IS A LOAD THAT DID NOT LOAD — the navigable still exists and shows an error page, which is what
         `{body:null}` means to the engine's child_document. That is a real §7.4 outcome, not a softening. */
      if (!r.ok) return { body: null, csp: null };
      return { body: r.body, csp: r.headers["content-security-policy"] || null };
    } catch (e) { RETHROW_FATAL(e); return { body: null, csp: null }; }
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
    if (!q || typeof q.url !== "string") return { body: null, status: 0, statusText: "", headers: [] };
    if (!canFetch) return { body: null, status: 0, statusText: "", headers: [] };
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
      DCHECK(r && typeof r === "object" && typeof r.body === "string" && typeof r.status === "number" &&
             r.headers && typeof r.headers === "object",
             "safeFetch answered an XHR with something other than its reply record — §3.5.6's response is " +
             "built from it and the page reads status, statusText and every header off that");
      return { body: r.body, status: r.status, statusText: r.statusText || "",
               headers: Object.entries(r.headers) };
    } catch (e) {
      /* §3.5.6's "handle errors": a THROWN fetch is the network error that becomes the page's `error` event.
         An invariant abort is not one and travels on. */
      RETHROW_FATAL(e);
      return { body: null, status: 0, statusText: "", headers: [] };
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
  return { M, ptrs, lines, fkey, prior, msg, persist, fetched, fetchedDocument, fetchedXhr, code, html, state: "hot",
           docId: _docId, cluster: clusterKeyOf(msg), groupId: msg && msg.groupId,
           origin: (msg && msg.origin) || "", _resolvers: [], _forceparkSteps, _remoteAsked: new Set() };
}
/* THE LEVEL-1 WFQ'S ONE INPUT. A NaN would order this engine against every other by a comparison that is
   false in both directions, so the pool's pick would depend on array order — a fairness invariant silently
   replaced by whichever engine happened to be first. */
function engineWeight(eng) {
  if (eng.state !== "hot") return -Infinity;
  const w = +eng.M.ccall("qjs_top_weight", "number", [], []);
  DCHECK(Number.isFinite(w), "the engine answered a top-flow weight that is not a finite number — every " +
                             "Level-1 ranking comparison against it is false, so the pool would pick by " +
                             "array order and call it value-of-information");
  return w;
}
async function engineServiceFetch(eng) {   // one round: resolve every pending reply/chunk, then the engine is hot again
  const M = eng.M;
  /* THE REPLY CROSSES AS TEXT AND CARRYING ITS TYPE — JSON, exactly as qjs_host_answer's answer does. A bare
     string could not say `null` for a network error without it being the four characters "null", and could not
     carry the URL list, the status or the headers at all. */
  const replies = engineOwedList(M, "qjs_pending");
  for (const u of replies)
    M.ccall("qjs_provide", "void", ["string", "string"], [u, JSON.stringify(await eng.fetched(u, false))]);
  const chunks = engineOwedList(M, "qjs_chunks");
  for (const u of chunks)
    M.ccall("qjs_provide", "void", ["string", "string"], [u, JSON.stringify(await eng.fetched(u, true))]);
  await engineServiceHostRequests(eng);
}
/* EVERY OWED LIST CROSSES THE SAME WAY — newline-joined records, or "" for none — so it is read in one place
   that says so. `String(x)` on its own would turn a NULL pointer (an entry answering before its list exists)
   into the four characters "null" and then into one bogus record: a URL nothing is parked on, which the
   engine's own qjs_provide DFAILs on one call later, naming the wrong side. */
function engineOwedList(M, entry) {
  const s = M.ccall(entry, "string", [], []);
  DCHECK(typeof s === "string", entry + " answered with something that is not text — every owed list crosses " +
                                "as newline-joined records and an empty list is the empty string");
  return String(s == null ? "" : s).split("\n").filter(Boolean);
}

/* THE INSTANCE HOLDING A DOCUMENT, by exact name. A child's name is PREFIXED by its creator's ("<creator>.<n>")
   and the creator is precisely the instance that does NOT hold it — that is why the notice exists at all — so a
   prefix match routes a message straight back to its sender. The engine catches that, twice; it should not have
   to be the thing that catches it. */
function hostHolderOf(docName) {
  for (const e of _pool) if (e.docId === docName) return e;
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
    const msg = { type: "AST_ANALYZE", pageHtml: (loaded && loaded.body) || "", sourceUrl: f[3],
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
    /* AND IT MAY ONLY BE PROVISIONED IF ITS CLUSTER HAS NO INSTANCE YET. A SAME-ORIGIN child never reaches this
       line at all — navigable.c's `child_in_this_agent` builds it as a realm in the creator's own heap and
       emits no notice — so a cluster hit here is a SECOND CROSS-ORIGIN document of one cluster (two `<iframe
       src="https://cdn/…">` of one page), which HTML puts in ONE heap for exactly the reasons the same-origin
       pair is in one. Building a second instance for it is the defect this file was keyed against; the entry
       that would put it in the right heap is the join below, and until that exists NOT provisioning is the
       honest state and the one navigable.h already names: "A HOST THAT WILL NOT HOST THE CHILD … simply never
       provisions the instance, and every read through the proxy parks. That is a host gap, visible as a parked
       flow." A dropped delivery to it is caught by the post branch's own assert. */
    const _ckey = clusterKeyOf(msg);
    DCHECK(_ckey !== eng.cluster,
           "the engine sent a create notice for a child in its OWN agent cluster — a same-origin child is a " +
           "second REALM in that heap and never leaves it, so this zone and navigable.c's child_in_this_agent " +
           "have answered one origin question two ways");
    if (hostClusterOf(_ckey)) {
      DFAIL("a second cross-origin document of ONE agent cluster was announced — its cluster already has an " +
            "instance, and the two documents are one similar-origin window agent that HTML puts in one heap. " +
            "This needs an ABI entry beside qjs_init that JOINS a document to a LIVE instance (a second Lexbor " +
            "parse, a realm through main.c's engine_child_realm, and its scripts seeded on the same frontier), " +
            "which does not exist yet; provisioning a second instance instead is the split this key deleted");
      return;
    }
    const child = await engineCreate("", msg.pageHtml, msg, false, f[1], f[5]);
    /* A CHILD DOCUMENT IS A DOCUMENT: it joins the ONE pool and is ranked, sliced, parked and finalized by the
       one host WFQ like every other. It has no caller to resolve to, so its findings merge the way a
       rehydrated cold engine's do rather than being returned to a requester that never asked for it. */
    child._cold = true;
    _pool.push(child);
    return;
  }
  if (f[0] === "windowproxy.post") {
    DCHECK(f.length >= 5, "a windowproxy.post notice was short of its fields");
    const target = hostHolderOf(f[1]);
    DCHECK(target !== null, "a message was posted to a document no instance in this pool holds — the create " +
                            "notice naming it was dropped, or that instance was finalized while a peer still " +
                            "held a WindowProxy for it");
    /* THE STAMP IS THE SENDER'S BROWSER-STATED ORIGIN, SERIALIZED. An empty one means this instance was
       rehydrated from a cold recipe written before the principal was part of it — this zone then does not know
       whose message this is, and the one thing it may not do is invent it (a wrong `event.origin` makes the
       engine report exploits that are not real and miss ones that are). CHECK, not DCHECK: in release the
       delivery would otherwise carry a fabricated identity into a security decision. */
    CHECK(!!eng.origin, "a cross-document message was posted by an instance with no recorded principal — only " +
                        "the trusted zone may state a sender's origin, and this one has none to state; carry " +
                        "the document's browser-stated origin into the cold recipe so a resumed instance keeps it");
    target.M.ccall("qjs_route", "void", ["string", "string"], [line, stampOrigin(eng.origin)]);
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
    to.asker.M.ccall("qjs_host_answer_remote", "void", ["number", "string"], [to.req, line.slice(_t2 + 1)]);
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
  const M = eng.M;
  // ONE-WAY NOTICES, and this zone OWES each of them an action — a notice it reads and discards is a document
  // nothing runs and a message nothing delivers, with every later read through them parked forever. They were
  // being read and discarded. Handled IN ORDER and one at a time: a page opens a window and posts to it in the
  // same turn, so the create must have finished provisioning before the post that names it is routed.
  for (const line of engineOwedList(M, "qjs_host_notices"))
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
  const reqs = engineOwedList(M, "qjs_host_requests");
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
      holder.M.ccall("qjs_perform", "void", ["string", "string"], [token, op]);
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
      M.ccall("qjs_host_answer", "void", ["number", "string", "number"], [id, JSON.stringify(r), 0]);
      continue;
    }
    if (!op.startsWith("document.fetch\t")) continue;
    const r = await eng.fetchedDocument(op.slice("document.fetch\t".length));
    // JSON, because the answer carries its TYPE across this seam: `{body:null}` is a load that did not load,
    // and the string "null" is a one-word document.
    M.ccall("qjs_host_answer", "void", ["number", "string", "number"], [id, JSON.stringify(r), 0]);
  }
}
function engineFinalize(eng) {
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
    try { json = eng.M.ccall("qjs_result", "string", [], []); }
    catch (e) { engineCrash(eng, "result", e); }
    if (!eng._crashed) {
      DCHECK(typeof json === "string" && json.length > 0,
             "qjs_result answered with no document — result_json returns nothing only when the composition " +
             "itself could not be allocated, which is this page's entire finding set being dropped");
      if (json) eng.lines.push("@RESULT " + json);
    }
  }
  /* THE OUTSTANDING RENDEZVOUS GO WITH THE INSTANCE THAT ASKED, and this is the line that makes the map's
     never-delete-on-answer rule safe: a completion relayed into a torn-down instance is a ccall into freed
     memory. Dropping them here is not dropping the ANSWER — the engine's own engine_host_answer already treats
     a request whose flow is gone as an answer nobody is waiting on. */
  for (const [token, to] of _remoteOps) if (to.asker === eng) _remoteOps.delete(token);
  try { eng.M.ccall("qjs_teardown", "void", [], []); }
  catch (e) { engineCrash(eng, "teardown", e); }
  for (const p of eng.ptrs) { try { eng.M._free(p); } catch (_) {} }
  const result = linesToAnalysis(eng.lines, eng.msg, !eng._crashed);
  result._fkey = eng.fkey; result._prior = eng.prior;   // engine-computed key + parked entry -> persisted below
  if (eng._crashed) {
    // NEVER BUILD ON AN UNCERTAIN ARCHITECTURE. A WASM abort means this engine crashed — every finding it
    // produced is from a crashed (untrustworthy) instance, so DISCARD them all. The result is a pure FAILURE:
    // only the crash marker + the error, so the crash is impossible to overlook and nothing downstream (cache,
    // popup, moat) ever consumes a crashed engine's output. Experimental stage: fail hard, then fix the ROOT.
    result._engineCrashed = true;
    result.fetchCallSites = []; result.securitySinks = []; result.chunkUrls = []; result.domEndpoints = [];
    result.probeResults = {};
    result.esmImportUrls = []; result.inRunModuleUrls = []; result.protoEnums = []; result.protoFieldMaps = [];
    result.dangerousPatterns = []; result._park = []; result._prior = null;
  }
  return result;
}
/* A WASM Aborted() is the engine CRASHING — a should-never-happen. It stays scoped to this engine (one page's
   crash must not throw and kill the whole multi-engine scheduler serving the user's other tabs), but it must
   be IMPOSSIBLE to overlook: a LOUD console.error banner + a persistent batch flag + total discard of the
   engine's findings (engineFinalize). NOT a quiet @E buried in resolverErrors — that is how the g_optaint
   teardown leak hid for so long. Experimental stage: a crash halts trust in that engine, never softened. */
function crashBanner(stage, m) {   // LOUD + a persistent batch flag; EVERY abort path (create/step/teardown) routes here — no crash is ever quiet
  try { self._engineCrashOccurred = (self._engineCrashOccurred || 0) + 1; } catch (_) {}
  try { console.error("\n==== ENGINE CRASH (" + stage + ") — WASM ABORTED, findings DISCARDED, NOT swallowed ====\n" + m + "\n"); } catch (_) {}
}
function engineCrash(eng, stage, e) {
  const m = String((e && e.message) || e);
  eng._crashed = true;
  // The C-side CHECK/DCHECK emits its @WHY/@E ROOT line (phase/cond/at/reason) to stderr -> sink -> eng.lines
  // IMMEDIATELY before abort(). A bare emscripten Aborted() message ("native code called abort()") is terse and
  // useless on its own, so surface that root line IN the loud banner — a crash must POINT AT ITS CAUSE, not just
  // announce itself (this is what forced grepping the reason out of the result during debugging).
  let root = "";
  for (let i = eng.lines.length - 1; i >= 0; i--) {
    const ln = String(eng.lines[i]);
    if (ln.startsWith("@WHY ") || (ln.startsWith("@E ") && /"(reason|cond|phase)"/.test(ln))) { root = ln; break; }
  }
  const err = root ? (m + " | ROOT: " + root) : m;   // the crash RECORD carries its cause (netdiff/result-visible), not only the console banner
  eng.lines.push('@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(err) + "}");
  crashBanner(stage, err);
}
// A crash BEFORE the engine object exists (creation/boot abort). Same rule: LOUD, findings discarded,
// marked _engineCrashed — never a quiet "degenerate result" the reviewer reads as a boring empty page.
function crashResult(stage, e, msg) {
  const m = String((e && e.message) || e);
  crashBanner(stage, m);
  /* NO RESULT DOCUMENT IS EXPECTED HERE, and this is the only caller that may say so: the instance aborted
     before it could answer, so its absence is the crash rather than a broken contract. */
  const r = linesToAnalysis(['@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(m) + "}"], msg, false);
  r._engineCrashed = true;
  r.fetchCallSites = []; r.securitySinks = []; r.chunkUrls = []; r.domEndpoints = [];
  r.probeResults = {};
  r.esmImportUrls = []; r.inRunModuleUrls = []; r.protoEnums = []; r.protoFieldMaps = []; r.dangerousPatterns = []; r._park = []; r._prior = null;
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
    if (!hot.length) {   // every live engine is mid-fetch: wait for the earliest body, then re-rank
      const fetching = pool.filter((e) => e.state === "fetching");
      if (!fetching.length) break;
      await Promise.race(fetching.map((e) => e._fetchP));
      continue;
    }
    let best = hot[0], runner = -Infinity;   // Level-1 WFQ pick + the runner-up weight (the value yield floor)
    for (const e of hot) { const w = ops.weight(e); if (w > ops.weight(best)) { runner = ops.weight(best); best = e; } else if (w > runner) runner = w; }
    if (ops.setFloor) ops.setFloor(best, hot.length > 1 ? runner : -1e300);   // outranked-by-runner-up => yield; lone engine => run on
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
      ops.requestPark(target);
    }
    const st = ops.step(target);
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
    if (target._forceparkSteps > 0 && --target._forceparkSteps === 0) ops.requestPark(target);
    // INCREMENTAL MERGE: a lone UNBOUNDED engine never reaches st===0, so without this its already-emitted
    // breadth surfaces only at finalize. Snapshot + merge on a coarse cadence on every non-final step.
    if (st !== 0 && ops.streamPartial) ops.streamPartial(target);
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
      target._fetchP = ops.serviceFetch(target).then(
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
const _pool = [];        // HOT/fetching engines (<= POOL_CAP resident wasm instances)
const _waiting = [];      // documents awaiting a slot: { code, html, msg, persist, resolve } — NO instance built yet
let _hostDriving = false;
let _engineFactory = engineCreate;   // injectable for tests
const _hostOps = {
  weight: engineWeight,
  /* THE VALUE YIELD FLOOR — run until outranked by the runner-up. The `try {} catch (_) {}` around it is gone
     with the two below it: a ccall into the engine either returns or ABORTS, and an abort is the WASM
     instance crashing. Swallowing that leaves a dead engine in the pool being stepped, ranked and finalized
     as if it were alive, which is the one outcome engineCrash exists to make impossible. */
  setFloor: (eng, floor) => {
    DCHECK(Number.isFinite(floor) || floor === -Infinity,
           "the pool set a yield floor that is not a number — the engine compares its top flow's weight " +
           "against it, and a NaN floor makes every comparison false so the flow never yields");
    eng.M.ccall("qjs_set_yield_floor", "void", ["number"], [floor]);
  },
  underPressure: () => _residentBytes() >= HOT_RAM_BUDGET,   // summed live wasm memory over the working-set floor
  /* THE COLD-TIER PARK, whose engine half is an unbuilt capability that says so: qjs_request_park is a bare
     DFAIL naming the serializer to write. The catch here swallowed exactly that — the abort a DFAIL exists to
     produce was turned into a no-op, so the host went on believing it had evicted an engine that never parked
     and the missing capability had no symptom at all. It is the forcing function; it is not caught. */
  requestPark: (eng) => { eng.M.ccall("qjs_request_park", "void", [], []); },
  /* INCREMENTAL MERGE (coarse cadence): snapshot a HOT engine's current findings + merge to the cumulative
     moat WITHOUT waiting for a finalize that an unbounded engine never reaches. First sight starts the clock,
     so short analyses (finalize before PARTIAL_MS) never pay for it. qjs_emit_partial appends a fresh @RESULT
     to eng.lines (teardown-free); we parse just that snapshot, CONSUME the line (bound eng.lines growth — the
     final teardown re-emits its own @RESULT), and merge via onFrontierAdvance (globalStore, dedup-idempotent). */
  streamPartial: (eng) => {
    const now = Date.now();
    if (!eng._lastPartial) { eng._lastPartial = now; return; }
    if (now - eng._lastPartial < PARTIAL_MS) return;
    eng._lastPartial = now;
    /* A WASM ABORT IS THE ENGINE CRASHING — the only failure this ccall has. It was inside the same swallowing
       catch as everything below it, so a crash mid-analysis left the engine in the pool to be stepped again. */
    try { eng.M.ccall("qjs_emit_partial", "void", [], []); }
    catch (e) { engineCrash(eng, "partial", e); return; }
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
  step: (eng) => { try { return eng.M.ccall("qjs_step", "number", [], []); } catch (e) { engineCrash(eng, "step", e); return 0; } },   // crashed instance -> finalize (loud), don't keep stepping a dead engine
  /* ONE SERVICE ROUND, and there is no second op beside it. `serviceHostRequests` was a separate injected op
     because the yield branch paid half the owed list; it is a strict subset of this one (engineServiceFetch
     ends by calling it), so keeping both would be two names for one round with a caller free to pick the
     half that skips the replies. */
  serviceFetch: engineServiceFetch,
  admit: async () => {   // gate CREATION to the RAM budget: build an instance only under memory pressure headroom
    // 1) seat waiting LIVE documents (the user's open tabs) first
    /* AN INDEX WALK RATHER THAN A SHIFT, because one waiting document can be legitimately UNSEATABLE YET (the
       defer below) and a shift would either drop it or spin this loop forever re-reading it. */
    let i = 0;
    while (i < _waiting.length && (_pool.length === 0 || _residentBytes() < HOT_RAM_BUDGET)) {
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
           branch beside it that built a second WASM instance, i.e. a second heap for one agent. */
        DCHECK(!isTop || cluster.docId === docId,
               "the TOP document of a browsing-context group arrived for a cluster whose instance is rooted at " +
               "a different document — the group's top was REPLACED (a same-origin navigation, or a COOP " +
               "navigation that severed the group), so the new document is NOT one this instance is running " +
               "and attaching it reports the page that was replaced. This needs §7.4's destroy-a-navigable for " +
               "the old document plus an ABI entry beside qjs_init that JOINS a document to a live instance " +
               "(`qjs_join`: a second Lexbor parse, a realm through main.c's engine_child_realm, its scripts " +
               "seeded on the same frontier) — neither exists yet, and neither is a second instance");
        cluster._resolvers.push(job);
        _waiting.splice(i, 1);
        continue;
      }
      /* A SUB-FRAME MAY NOT ROOT ITS OWN CLUSTER WHILE THE GROUP'S TOP IS SAME-ORIGIN WITH IT. Its cluster's
         instance is the TOP document's — the top creates this frame and runs it in its heap — and CONTENT_HTML
         arrival order across frames is not the document order: a sub-frame that reported first would otherwise
         root the cluster, and the top would then arrive at an instance rooted elsewhere and fire the assert
         above for a reason that is an artifact of message timing rather than a replaced document. So it WAITS,
         in place, and is seated by the next admit once the top has landed. `_isRealOrigin` is what keeps this
         off the opaque case: a sandboxed top's frames are opaque too, their per-document tokens never equal
         `originOf(topLevelUrl)`, and each is correctly its own cluster of one. */
      if (!isTop && _isRealOrigin(job.msg.origin) && originOf(job.msg.topLevelUrl) === job.msg.origin) { i++; continue; }
      _waiting.splice(i, 1);
      try { const eng = await _engineFactory(job.code, job.html, job.msg, job.persist); eng._resolvers.push(job); _pool.push(eng); }
      // boot/creation abort: LOUD failure, not a quiet degenerate result. An invariant abort from the creation
      // path (the init return code, the bundle id, the frontier key's origin) is NOT a boot abort and must not
      // be reported as one — it is this zone's own contract with the engine breaking.
      catch (e) { RETHROW_FATAL(e); job.resolve(crashResult("create", e, job.msg)); }
    }
    // 2) ONE frontier: when no LIVE work is pending/running and RAM has headroom, rehydrate the highest-value
    //    COLD recipes into the SAME pool so they interleave with (and by) the one WFQ — not a second scheduler.
    if (!_waiting.length && !_pool.some((e) => !e._cold) && (_pool.length === 0 || _residentBytes() < HOT_RAM_BUDGET)) {
      const cold = (await frontierAll()).filter((e) => e && e.recipes && !_pool.some((p) => p.fkey === e.key));
      cold.sort((a, b) => frontierWeight(b) - frontierWeight(a));   // one global value order
      for (const c of cold) {
        if (_pool.length > 0 && _residentBytes() >= HOT_RAM_BUDGET) break;
        try {
          /* THE PRINCIPAL RESUMES WITH THE RECIPE. A parked flow's world is only the same world if the
             document it resumes into is the same PRINCIPAL — and this zone cannot re-derive one from
             c.sourceUrl without re-fabricating the tuple origin a sandboxed document does not have. A recipe
             written before this field carries "", and the stamp site refuses to deliver a message for it
             rather than inventing one. */
          /* A RESUMED RECIPE IS A CLUSTER OF ONE, and its GROUP says so rather than borrowing a tab's. The
             browsing-context group it parked in is gone — the tab was closed, or the session was — so there is
             no live document it may share a heap with, and the frontier key (origin|bundle, already unique per
             recipe and never equal to a tab id) is the honest name for the group it resumes into. That is also
             what lets the origin half stay "" for a recipe written before the principal was part of one: an
             empty half of a key whose other half is unique collides with nothing, so nothing is invented. */
          const msg = { type: "AST_ANALYZE", pageHtml: c.html, code: c.code, sourceUrl: c.sourceUrl,
                        origin: c.origin || "", groupId: "cold:" + c.key,
                        topLevelUrl: c.topLevelUrl, credentialed: c.credentialed, persist: true };
          const eng = await engineCreate(c.code || "", c.html || "", msg, true);   // a rehydrated cold recipe always participates in the frontier
          eng._cold = true; _pool.push(eng);
        } catch (e) { RETHROW_FATAL(e); crashBanner("create-cold", String((e && e.message) || e)); }   // was silently swallowed — a cold-rehydration abort must be LOUD too
      }
    }
  },
  finish: async (eng) => {   // fully explored, or self-parked under RAM pressure -> persist residue to the cold tier + resolve/merge
    const i = _pool.indexOf(eng); if (i >= 0) _pool.splice(i, 1);
    const result = engineFinalize(eng);
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
        key: result._fkey, sourceUrl: eng.msg.sourceUrl, topLevelUrl: eng.msg.topLevelUrl, origin: eng.origin,
        html: eng.html, code: eng.code,
        credentialed: !!eng.msg.credentialed, recipes: (result._park || []).join(";"),
        emit: ((prior && prior.emit) || 0) + (result.fetchCallSites || []).length, visits: ((prior && prior.visits) || 0) + 1, ts: Date.now(),
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
  /* EVERY OUTSTANDING RENDEZVOUS BELONGED TO AN INSTANCE THAT IS NOW DROPPED — the whole pool went. Left
     behind, each entry holds its asking engine's wasm Module alive for the life of the offscreen, which is the
     one shape a map keyed on a live instance fails in. */
  _remoteOps.clear();
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

console.debug("[ast-worker v2] bridge ready (self.astDispatch installed)");

/* Node-only: expose the PURE host-WFQ policy (no wasm) for deterministic unit tests of ordering/
   eviction/non-blocking-fetch. Never runs in the worker (no `module`). */
if (typeof module !== "undefined" && module.exports) module.exports = { hostSchedule, engineWeight };
