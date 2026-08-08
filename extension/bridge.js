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
   @E lines (host-side protocol errors) are still surfaced so a zero-result never fails silently. Every
   sibling field the brain reads unconditionally is present (empty) so it never throws. */
function linesToAnalysis(lines, msg) {
  let result = null;
  const extraErrors = [];
  let resumed = 0;
  for (const raw of lines) {
    const ln = String(raw);
    if (ln.startsWith("@RESUMED ")) { resumed = parseInt(ln.slice(9), 10) || 0; continue; }
    if (ln.startsWith("@RESULT ")) {
      try { result = JSON.parse(ln.slice(8)); }
      catch (e) { extraErrors.push({ context: "result-parse", message: String(e && e.message || e), snippet: null, replyExample: null }); }
    } else if (ln.startsWith("@E ")) {
      extraErrors.push({ context: "engine", message: ln.slice(3), snippet: null, replyExample: null });
    } else if (ln.startsWith("@WHY ")) {
      // engine diagnostic for a zero-result/resource path (e.g. reg_oom) — surface it so an OOM or
      // aborted flow never fails SILENTLY (CLAUDE.md: every zero-result path emits @WHY).
      try { const o = JSON.parse(ln.slice(5)); extraErrors.push({ context: o.phase || "why", message: o.reason || ln.slice(5), snippet: null, replyExample: null }); }
      catch (_) { extraErrors.push({ context: "why", message: ln.slice(5), snippet: null, replyExample: null }); }
    }
  }
  result = result || {};
  // Surface the scheduler's interleave counter so fairness/deep-preemption is OBSERVABLE (verification +
  // a real signal that the single BFS actually context-switches, not just runs FIFO). A cumulative global
  // across steps; the mapped field carries the per-result value.
  try {
    const m = { switches: result._switches || 0, orphans: result._orphans || 0, park: (result._park || []).length, resumed: resumed, work: result._work || 0, parked: result._parked || 0, url: (msg && msg.sourceUrl) || "" };
    self._engineMeta = m;
    // A per-run LOG (not a single overwritten global): concurrent cold-kick engines each report here, so the
    // full park->persist->rehydrate->resume SEQUENCE across all engines is observable, not just the last one.
    (self._engineLog = self._engineLog || []).push(m);
    if (self._engineLog.length > 200) self._engineLog.shift();
  } catch (_) {}
  return {
    _switches: result._switches || 0,
    fetchCallSites: result.fetchCallSites || [],
    resolverErrors: (result.resolverErrors || []).concat(extraErrors),
    chunkUrls: result.chunkUrls || [],
    securitySinks: result.securitySinks || [],
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
   DOM) — the bridge no longer scrapes scripts. code (argv[1]) is any brain-assembled extra scripts
   (usually empty); html (argv[2]) is the page. */
function originOf(u) { try { return new URL(u).origin; } catch (_) { return ""; } }
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
/* A frontier entry (the GLOBAL union spans all origins): { key: origin|hash, sourceUrl, html, code,
   recipes: "idx,dec;...", emit, visits, ts, credentialed }. Rehydration re-runs (html,code) + resumes
   recipes -- so a parked flow on ANY site can be advanced later, even when that page isn't open. */
async function frontierGet(key) {
  try { const db = await idbOpen(); return await new Promise((res) => { const t = db.transaction("frontier").objectStore("frontier").get(key); t.onsuccess = () => res(t.result || null); t.onerror = () => res(null); }); }
  catch (_) { return null; }
}
async function frontierPut(key, entry) {
  try { const db = await idbOpen(); await new Promise((res) => { const s = db.transaction("frontier", "readwrite").objectStore("frontier"); const t = (entry && entry.recipes) ? s.put(entry, key) : s.delete(key); t.onsuccess = () => res(); t.onerror = () => res(); }); }
  catch (_) {}
}
async function frontierAll() {
  try { const db = await idbOpen(); return await new Promise((res) => { const t = db.transaction("frontier").objectStore("frontier").getAll(); t.onsuccess = () => res(t.result || []); t.onerror = () => res([]); }); }
  catch (_) { return []; }
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
   document is one wasm instance (SECURITY.md: one instance per page); the engine exposes its best flow's
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
async function engineCreate(code, html, msg, persist) {
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
  // THE DOCUMENT ID. One WASM instance is one DOCUMENT regardless of origin, so a flow that scripts an iframe
  // or a popup writes state in a PEER instance, and that peer keys its segment of the flow's world by an id
  // minted here. The offscreen mints it because SECURITY.md makes the offscreen the only zone that knows which
  // instance holds which document — the same reason it owns the routing. The engine rejects 0.
  // SCOPE, stated rather than assumed: this counter is unique across the instances ALIVE in this offscreen,
  // which is exactly the set that can message each other today. It is not yet persisted, because a parked
  // foreign segment does not yet outlive a session; when segments park, this becomes a persisted allocator.
  const _docId = String(++nextDocumentId);
  M.ccall("qjs_init", "number", ["number", "number", "number", "number", "number"],
    [arg(code || ""), arg(html || ""), arg(originOf(msg && msg.sourceUrl)), arg(_docId), arg(_csp)]);
  const _bid = (M.ccall("qjs_bundle_id", "number", [], []) >>> 0).toString(36);
  const fkey = originOf(msg && msg.sourceUrl) + "|" + _bid;
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
  // Only on the INITIAL park (no recipes to resume yet); firing while resuming would re-park the rehydrated
  // recipes forever (the stored sourceUrl keeps the query param), so a cold/re-visit resume never runs.
  let _forceparkSteps = 0;
  if (msg && typeof msg.sourceUrl === "string" && /[?&]__forcepark=1\b/.test(msg.sourceUrl) && !(prior && prior.recipes)) {
    _forceparkSteps = 2;   // park after boot + the first flow burst: flows have RUN + SUSPENDED (real decvecs) but not yet drained
  }
  const canFetch = typeof self.safeFetch === "function" && msg && msg.sourceUrl;
  const fetched = async (u, asScript) => {   // safe-fetch a pending reply/chunk url -> body ("" if unavailable)
    if (!canFetch || hasHole(u)) return "";
    try {
      const abs = new URL(u, msg.sourceUrl).href;
      // chunks: as-script (CORB), never credentialed. replies: opt-in credentialed -> the AUTHENTICATED
      // logged-in reply (the moat headline), gated by safeFetch's own SOP/CORS + GET-only. Default off.
      const opts = asScript ? { pageUrl: msg.sourceUrl, as: "script" }
                            : { pageUrl: msg.sourceUrl, credentialed: !!(msg && msg.credentialed) };
      const r = await self.safeFetch(abs, opts);
      return (r && r.ok && typeof r.body === "string") ? r.body : "";
    } catch (_) { return ""; }
  };
  return { M, ptrs, lines, fkey, prior, msg, persist, fetched, code, html, state: "hot", _forceparkSteps };
}
function engineWeight(eng) { return eng.state === "hot" ? +eng.M.ccall("qjs_top_weight", "number", [], []) : -Infinity; }
async function engineServiceFetch(eng) {   // one round: resolve every pending reply/chunk, then the engine is hot again
  const M = eng.M;
  const replies = String(M.ccall("qjs_pending", "string", [], [])).split("\n").filter(Boolean);
  for (const u of replies) M.ccall("qjs_provide", "void", ["string", "string"], [u, await eng.fetched(u, false)]);
  const chunks = String(M.ccall("qjs_chunks", "string", [], [])).split("\n").filter(Boolean);
  for (const u of chunks) M.ccall("qjs_provide", "void", ["string", "string"], [u, await eng.fetched(u, true)]);
  engineServiceHostRequests(eng);
}

// WHAT ONLY THIS ZONE CAN ANSWER. A cross-document operation is answered by the instance holding that document,
// and SECURITY.md makes the offscreen the only zone that knows which instance that is — the same reason it owns
// the routing and stamps the sender's origin. The asking flow is SUSPENDED mid-frame until the answer lands, so
// this is pumped every round alongside the fetch replies; leaving one unanswered parks that flow indefinitely
// (its siblings keep running, which is the point of suspending rather than blocking).
function engineServiceHostRequests(eng) {
  const M = eng.M;
  // ONE-WAY NOTICES. `navigable.create <child> <creator> <url> <origin> <csp>` announces a document the engine
  // has already named and already handed the page a WindowProxy for; there is nothing to answer, because the
  // engine mints its own child document names. What this zone still owes it is an INSTANCE: spin one up under
  // that name with that url/origin/policy, and route requests naming it to that instance.
  M.ccall("qjs_host_notices", "string", [], []);
  // NO REQUEST LOOP, deliberately. Every branch that was here has been deleted rather than emptied: a loop that
  // walks the owed requests and answers none of them is not a place to add the routing, it is a place to be
  // tempted into guessing an answer — which is what answering `navigable.create` with "not created" was. An
  // unanswered request stays on the asking flow's register, that flow stays parked (its siblings keep running,
  // which is the point of suspending rather than blocking), and qjs_host_requests keeps reporting it.
}
function engineFinalize(eng) {
  try { eng.M.ccall("qjs_teardown", "void", [], []); }
  catch (e) { engineCrash(eng, "teardown", e); }
  for (const p of eng.ptrs) { try { eng.M._free(p); } catch (_) {} }
  const result = linesToAnalysis(eng.lines, eng.msg);
  result._fkey = eng.fkey; result._prior = eng.prior;   // engine-computed key + parked entry -> persisted below
  if (eng._crashed) {
    // NEVER BUILD ON AN UNCERTAIN ARCHITECTURE. A WASM abort means this engine crashed — every finding it
    // produced is from a crashed (untrustworthy) instance, so DISCARD them all. The result is a pure FAILURE:
    // only the crash marker + the error, so the crash is impossible to overlook and nothing downstream (cache,
    // popup, moat) ever consumes a crashed engine's output. Experimental stage: fail hard, then fix the ROOT.
    result._engineCrashed = true;
    result.fetchCallSites = []; result.securitySinks = []; result.chunkUrls = []; result.domEndpoints = [];
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
  const r = linesToAnalysis(['@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(m) + "}"], msg);
  r._engineCrashed = true;
  r.fetchCallSites = []; r.securitySinks = []; r.chunkUrls = []; r.domEndpoints = [];
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
    // DEV __forcepark: request the park only after N dispatches, so it captures MID-EXPLORATION residue
    // (recipes with real decvecs + handler-driven async flows), mirroring a production RAM-pressure park.
    if (target._forceparkSteps > 0 && --target._forceparkSteps === 0) ops.requestPark(target);
    // INCREMENTAL MERGE (both hot AND fetch-parked): a lone UNBOUNDED engine never reaches st===0, so without
    // this its already-emitted breadth never surfaces (@RESULT is teardown-only). A FETCH-HEAVY engine (lazy
    // chunks / a reply loop) is perpetually st===1, never st===2 — so surfacing ONLY on st===2 leaves it at
    // zero (the realish symptom). Snapshot + merge on a coarse cadence on every non-final step.
    if (st !== 0 && ops.streamPartial) ops.streamPartial(target);
    if (st === 1) {   // NEED_FETCH: service asynchronously (non-blocking) so other engines keep advancing
      target.state = "fetching";
      target._fetchP = ops.serviceFetch(target).then(() => { target.state = "hot"; }, () => { target.state = "hot"; });
    } else if (st === 0) {   // fully explored, or self-parked under RAM pressure: finalize (residue -> IDB cold tier)
      await ops.finish(target);
    } else {   // st === 2: engine yielded HOT on its cooperative quantum (frontier intact).
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
  setFloor: (eng, floor) => { try { eng.M.ccall("qjs_set_yield_floor", "void", ["number"], [floor]); } catch (_) {} },   // value yield: run until outranked by the runner-up
  underPressure: () => _residentBytes() >= HOT_RAM_BUDGET,   // summed live wasm memory over the working-set floor
  requestPark: (eng) => { try { eng.M.ccall("qjs_request_park", "void", [], []); } catch (_) {} },   // cold-tier park under RAM pressure (resource-driven)
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
    try {
      eng.M.ccall("qjs_emit_partial", "void", [], []);
      let idx = -1;
      for (let i = eng.lines.length - 1; i >= 0; i--) if (String(eng.lines[i]).startsWith("@RESULT ")) { idx = i; break; }
      if (idx < 0) return;
      const partial = linesToAnalysis([eng.lines[idx]], eng.msg);   // parse only the snapshot line
      eng.lines.splice(idx, 1);                                     // consume it
      // Merge on EITHER surface: an XSS-only page (verified @S PoCs, no endpoints) must surface incrementally
      // too — gating the merge on fetchCallSites alone dropped every sink from a live/looping engine.
      const hasWork = partial.fetchCallSites.length || (partial.securitySinks && partial.securitySinks.length);
      if (hasWork && typeof self.onFrontierAdvance === "function") self.onFrontierAdvance(eng.msg.sourceUrl, partial);
    } catch (_) {}
  },
  step: (eng) => { try { return eng.M.ccall("qjs_step", "number", [], []); } catch (e) { engineCrash(eng, "step", e); return 0; } },   // crashed instance -> finalize (loud), don't keep stepping a dead engine
  serviceFetch: engineServiceFetch,
  admit: async () => {   // gate CREATION to the RAM budget: build an instance only under memory pressure headroom
    // 1) seat waiting LIVE documents (the user's open tabs) first
    while (_waiting.length && (_pool.length === 0 || _residentBytes() < HOT_RAM_BUDGET)) {
      const job = _waiting.shift();
      try { const eng = await _engineFactory(job.code, job.html, job.msg, job.persist); eng._resolve = job.resolve; _pool.push(eng); }
      catch (e) { job.resolve(crashResult("create", e, job.msg)); }   // boot/creation abort: LOUD failure, not a quiet degenerate result
    }
    // 2) ONE frontier: when no LIVE work is pending/running and RAM has headroom, rehydrate the highest-value
    //    COLD recipes into the SAME pool so they interleave with (and by) the one WFQ — not a second scheduler.
    if (!_waiting.length && !_pool.some((e) => !e._cold) && (_pool.length === 0 || _residentBytes() < HOT_RAM_BUDGET)) {
      const cold = (await frontierAll()).filter((e) => e && e.recipes && !_pool.some((p) => p.fkey === e.key));
      cold.sort((a, b) => frontierWeight(b) - frontierWeight(a));   // one global value order
      for (const c of cold) {
        if (_pool.length > 0 && _residentBytes() >= HOT_RAM_BUDGET) break;
        try {
          const msg = { type: "AST_ANALYZE", pageHtml: c.html, code: c.code, sourceUrl: c.sourceUrl, credentialed: c.credentialed, persist: true };
          const eng = await engineCreate(c.code || "", c.html || "", msg, true);   // a rehydrated cold recipe always participates in the frontier
          eng._cold = true; _pool.push(eng);
        } catch (e) { crashBanner("create-cold", String((e && e.message) || e)); }   // was silently swallowed — a cold-rehydration abort must be LOUD too
      }
    }
  },
  finish: async (eng) => {   // fully explored, or self-parked under RAM pressure -> persist residue to the cold tier + resolve/merge
    const i = _pool.indexOf(eng); if (i >= 0) _pool.splice(i, 1);
    const result = engineFinalize(eng);
    if (eng.persist && result._fkey) {   // persist into the GLOBAL frontier (cross-session cold tier)
      const prior = result._prior;
      await frontierPut(result._fkey, {
        key: result._fkey, sourceUrl: eng.msg.sourceUrl, html: eng.html, code: eng.code,
        credentialed: !!eng.msg.credentialed, recipes: (result._park || []).join(";"),
        emit: ((prior && prior.emit) || 0) + (result.fetchCallSites || []).length, visits: ((prior && prior.visits) || 0) + 1, ts: Date.now(),
      });
    }
    if (eng._cold) { try { if (typeof self.onFrontierAdvance === "function") self.onFrontierAdvance(eng.msg.sourceUrl, result); } catch (_) {} }   // no live caller -> merge to the moat here
    if (eng._resolve) eng._resolve(result);
  },
};
function _hostKick() {
  if (_hostDriving) return;
  _hostDriving = true;
  hostSchedule(_pool, _hostOps).catch((e) => console.debug("[host-wfq] %s", e && e.message)).finally(() => { _hostDriving = false; if (_pool.length || _waiting.length) _hostKick(); });
}
/* The offscreen kicks the pool on idle so parked COLD recipes get pulled in (admit re-checks the frontier)
   even when no new document arrives — the single ATTENTION keeps advancing across sessions. */
self.kickHostPool = _hostKick;

/* Endpoint IDENTITY (exact dedup by method+hole-normalized-url + shape/concrete collapse with path-param
   examples) is the ENGINE's job now — it emits an already-deduped fetchCallSites in @RESULT. The host
   mergeCallsites/dedupShapeConcrete/pathSegs were DELETED. */

self.astDispatch = async function astDispatch(msg) {
  try {
    if (!msg || msg.type !== "AST_ANALYZE") return { success: false, error: "unknown type " + (msg && msg.type) };
    const html = msg.pageHtml || "";
    const code = msg.code || "";           // any brain-assembled scripts (usually empty); the DOM carries the page
    if (!html && !code) return { success: true, result: linesToAnalysis([], msg) };

    /* ENQUEUE this document into the LIVE host WFQ pool. Its wasm instance interleaves in SLICES with every
       other open document by value-of-information — no run-to-completion, no recency monopoly. The per-doc
       promise resolves when THIS engine finalizes (fully explored or host-evicted); the pool persists its
       residue to the GLOBAL frontier (cross-session cold tier). msg.persist enables that IDB persistence. */
    const persist = !!msg.persist;
    const result = await new Promise((resolve) => { _waiting.push({ code, html, msg, persist, resolve }); _hostKick(); });
    return { success: true, result };   // result.fetchCallSites is already deduped by the engine
  } catch (e) {
    return { success: false, error: String(e && e.message || e), stack: e && e.stack };
  }
};

console.debug("[ast-worker v2] bridge ready (self.astDispatch installed)");

/* Node-only: expose the PURE host-WFQ policy (no wasm) for deterministic unit tests of ordering/
   eviction/non-blocking-fetch. Never runs in the worker (no `module`). */
if (typeof module !== "undefined" && module.exports) module.exports = { hostSchedule, engineWeight };
