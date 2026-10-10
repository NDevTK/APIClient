/* bridge.js — the host bridge, the trusted-zone JS edge of the engine (SECURITY.md).
 *
 * The engine is the untrusted WASM, so this file is what it cannot be: it drives the qjs_step protocol,
 * fetches replies and chunks through the safeFetch chokepoint, persists the cross-session frontier to
 * IndexedDB, and JSON.parses the engine's one @RESULT document. It installs self.astDispatch and
 * self.kickHostPool for the offscreen document. It holds no analysis logic: identity, dedup and detection
 * belong to the C engine.
 *
 * No engine lives in this realm. Each instance is a renderer that renderer-host.js provisions in an
 * `<iframe sandbox="allow-scripts">` with a unique opaque origin, and every qjs_* call is an awaited method of
 * `content.mojom.Renderer`, validated at both ends of a MessagePort. Every ABI call is therefore a suspension
 * point at which another engine's round can interleave: two calls in a row are atomic only if written to be. */
// The root document name for each engine this offscreen owns (see qjs_init below). A document an engine
// creates (a child navigable, a popup) names itself "<parent>.<n>", so HTML §4.8.5 The iframe element can
// create a child navigable inside the insertion steps without asking this zone.
let nextDocumentId = 0;
/* The run log and the crash count, declared at load so that their absence means this file did not load, an
   empty log means no engine finalized, and a zero means no engine aborted. crashBanner counts every abort
   path; rendererPoolProbe reads both, and the log also answers the popup's GET_ENGINE_RUNS. The log holds page
   addresses, so hostClear empties it; the crash count holds none and survives a clear.
   One row per run: a run owns exactly one row (`eng._log`), a partial snapshot overwrites that row rather than
   appending, and the record's `run` field says which state the row reports, so a snapshot of a run in
   progress is never read as a finished run. */
self._engineLog = [];
self._engineCrashOccurred = 0;
/* The Level-1 order: the reading `_level1Record` writes at the end of every scheduler round. Level 1 (the host
   ordering live per-page engines by best-flow weight) is composed entirely in this zone, since no engine can
   see another, so no result document can carry it. The record carries extrema and populations across the
   ranked set rather than only the pick, because a rank frozen at a constant is visible only in the spread.
   Declared at load for three states: `undefined` is this file not loaded, `null` is no round completed this
   session, an object is a reading. A clear does not touch it: it holds no address or identity, and the
   round after a clear records the emptied pool. */
self._level1 = null;

/* Whether a URL has an opaque hole, so it is not concretely fetchable (gates reply and chunk fetch). Asked
   through lib/callsite-url.js in endpoint.c's own hole grammar, so every hole shape the engine emits is
   recognised. Endpoint identity (hole normalization, shape/concrete collapse) belongs to the engine. */
const hasHole = (s) => astAddressHasHole(s || "");

/* Map the engine's one `@RESULT <json>` line to the analysis object the offscreen consumes. The engine builds
   and dedups the whole result (endpoints, params, headers, bodies, @S sinks, page errors, park recipes); this
   zone does one JSON.parse and relays. @E lines (host-side protocol errors) are surfaced so a zero result
   never fails silently.
   `outcome` is the caller's statement of what this record is, and travels on the result as `_run`:
     "partial"        — a mid-run snapshot (qjs_emit_partial); a document is required.
     "complete"       — the run ended and the engine answered with its result document; a document is required.
     "crashed"        — the instance aborted; usually no document, but one is present when the abort landed
                        after a partial was printed and before this zone consumed it, and it is asserted as usual.
     "nothing-to-run" — no HTML and no code, so no engine ran; no document, and not a crash.
   A crash invalidates the run's claims of completeness, not its observations: findings composed before the
   abort are already merged into the cumulative record and are still relayed. The cost counters, the park
   residue and the right to overwrite the cross-session frontier entry are gated on `_run` at their sites. */
/* The contract solver/result.c composes, asserted at the seam because every field is read below and a missing
   one would become an empty finding set reported as a clean page. */
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
  /* `pageErrorsRetracted` is disjoint from `pageErrors` (one snprintf emits both): a message the engine
     reported and then took back — HTML §8.1.6.4 HostPromiseRejectionTracker(promise, operation), a handler
     attached in a later task — is here, and a message in neither was never recorded. It is what tells "the page
     raised nothing" from "the page handled every error it raised", so it is asserted, never defaulted. */
  DCHECK(Array.isArray(r.pageErrorsRetracted),
         "the engine's result document carries no pageErrorsRetracted array — result.c composes it beside " +
         "pageErrors in the same snprintf, so its absence is that composition changed under this reader and " +
         "a page whose rejections were all handled would be indistinguishable from one that raised nothing");
  /* `pageErrorsExplored` is orthogonal to the pair above: each message in it is also in exactly one of them.
     It marks throws the engine itself chose to explore — a forked completion over unknown input whose spec
     step answers with a throw (Web IDL §3.2.15 Interface types: "Throw a TypeError.") — so they are not shown
     to a person as their page's own errors. Asserted, never defaulted. */
  DCHECK(Array.isArray(r.pageErrorsExplored),
         "the engine's result document carries no pageErrorsExplored array — result.c composes it beside " +
         "pageErrors and pageErrorsRetracted in the same composition, so its absence is that composition " +
         "changed under this reader and every engine-minted exploration throw would be reported to a person " +
         "as an error their own page raised");
  /* The cost counters and the @S arrival census are emitted by result.c in one snprintf, so the contract is all
     of them and a subset check would pass the very shapes it should reject. The @S group (`_sourceReads`,
     `_sinkReached`, `_sinkTainted`, `_sinkSuppressed`) is what makes an empty securitySinks readable: whether
     attacker input was acquired, whether a sink ran, whether tainted data arrived, and whether an arrival was
     declined by an unforgeable check.
     This JS ships when pushed while its C writer ships only when built, so a counter added to both in one
     commit fails here against every older artifact until the next build. The message names that cause first;
     the assert stays, since a default would turn a name the engine stops writing into a permanent zero. */
  for (const k of ["_switches", "_flows", "_candidates", "_jobsQueued", "_jobsRun", "_unitsDone",
                   "_worldSegmentsHeld", "_worldSegmentsMade", "_worldSegmentsForked",
                   "_routedDelivered", "_routedRefused", "_routedTasksFired",
                   "_routedTasksTargetOrigin", "_routedTasksTargetGone", "_routedTasksThrew",
                   "_sourceReads", "_sinkReached", "_sinkTainted", "_sinkSuppressed",
                   "_orphansDriven", "_orphansAsked",
                   "_orphanAskMemo", "_orphanAskEmpty", "_orphanAskTook",
                   "_orphanWalks", "_orphanWalkEntries", "_orphanWalksFull",
                   "_orphanWalkFullCandidates"]) {
    DCHECK(typeof r[k] === "number",
           "the engine's result document carries no " + k + " count. TWO CAUSES, AND THE SECOND IS THE " +
           "ORDINARY ONE — check it first. (1) THE LOADED WASM IS OLDER THAN THIS FILE: this half of the " +
           "contract ships the instant it is pushed and the other half ships only when someone BUILDS, so a " +
           "field added to result.c and to this list in one commit is red for every artifact until the next " +
           "build lands. That is not a stale contract, it is a SCHEDULED one, and it is what a freshly added " +
           "name almost always means. Read extension/lib/qjs/qjs.mjs.build.json's `head` and ask whether the " +
           "commit that added " + k + " is an ancestor of it; if it is not, the answer is to build, and " +
           "nothing here is wrong. (2) Otherwise the composition changed under this seam: result.c emits " +
           "every cost counter and the whole @S arrival census in ONE snprintf, so a field missing from a " +
           "wasm that should have it is that snprintf having been edited without this list. NEVER SOFTEN " +
           "THIS INTO A DEFAULT for either cause — a name the engine stops writing becomes a zero the " +
           "diagnostic reports forever, which is the defect this loop exists to end. They are the only " +
           "OBSERVABLE that the single BFS context-switches, forks and pumps jobs rather than running its " +
           "flows FIFO, and the only thing that tells an empty finding set from a run that never looked");
  }
  /* `_wfq` (solver/result.h) is the order the frontier was in. Its shape is asserted and its names are not:
     the object is relayed whole and popup.js renders whatever rows it carries, so a name list here would be a
     third copy of solver/flow.h's fields. `members` is always present: `{members: 0}` with no term rows is an
     empty frontier (a session ends by draining or parking, leaving no members), a full object is a reading,
     and no `_wfq` is a broken contract. The rows-iff-members biconditional is asserted so the absence of rows
     stays a positive statement. */
  DCHECK(r._wfq && typeof r._wfq === "object" && !Array.isArray(r._wfq),
         "the engine's result document carries no _wfq census — solver/result.c composes the WFQ's own " +
         "ordering (solver/flow.h's WfqCensus) into that field on every document it builds, partials " +
         "included, and it is the ONLY observable of what the one BFS is ordering its flows BY. Its absence " +
         "has the same two causes as a missing counter above and in the same order: check first whether the " +
         "loaded wasm predates this reader (extension/lib/qjs/qjs.mjs.build.json's `head`), then whether " +
         "result_json's composition changed under this seam");
  DCHECK(Number.isInteger(r._wfq.members) && r._wfq.members >= 0,
         "the engine's WFQ census carries no `members` count — it is the row that says how many flows the " +
         "order was taken over, so without it an empty frontier and a frontier this census could not read " +
         "are the same document, and every term below is a reading of an unknown population");
  DCHECK((Object.keys(r._wfq).length > 1) === (r._wfq.members > 0),
         "the engine's WFQ census reports " + r._wfq.members + " members with " +
         (Object.keys(r._wfq).length - 1) + " term rows — the two must agree, because the ABSENCE of the " +
         "terms is how an empty frontier says there was no order to report. Rows beside `members: 0` are " +
         "readings of an order that did not exist; `members > 0` with no rows is a census that ran and said " +
         "nothing, and both would be read as the verdict `no term orders this frontier`");
  for (const k of Object.keys(r._wfq))
    DCHECK(typeof r._wfq[k] === "number" && Number.isFinite(r._wfq[k]),
           "the engine's WFQ census carries a non-finite `" + k + "` — every row of it is a count, a service " +
           "notch or a weight, and a NaN reaching a reader makes every comparison against it false, which is " +
           "the same silent failure §engineRecordFacts asserts one level up for the Level-1 weight");
  /* `_quantum` (solver/quantum.h) says what the `_wfq` order was billed in, so it is asserted here between the
     order and the censuses it qualifies. It is not a census: `measure` is a string and `isCpu` a boolean, so
     the generic finite-number pass below would reject it or force a 0/1 encoding; hence three named asserts.
     On a host with no CPU clock (the engine's realm) the cooperative slice and solver/engine.c's
     `flow_age_running` charge are billed in wall time, so an OS descheduling moves one flow's rank and two runs
     of one build over one page can take different frontier orders; bounding the slice in steps would be a cap,
     so the variance stays and the document names it. The cause is the build, not isolation:
     engine/build.mjs links no `-pthread`, so linear memory is an ordinary ArrayBuffer with no second thread,
     even though the engine's frame is cross-origin isolated (renderer.html verifies that by attempting it).
     `isCpu` says whether the caveat applies, `measure` what was billed instead, `sliceMs` how coarse the
     slicing was. `quantum_json` reads only compile-time constants, so none is ever legitimately absent. */
  DCHECK(r._quantum && typeof r._quantum === "object" && !Array.isArray(r._quantum),
         "the engine's result document carries no _quantum — solver/quantum.c's quantum_json composes it into " +
         "every document result.c builds, partials included, and it is what says whether the `_wfq` ordering " +
         "above may be compared with another run's at all. Its absence has the same two causes as a missing " +
         "counter and in the same order: check first whether the loaded wasm predates this reader " +
         "(extension/lib/qjs/qjs.mjs.build.json's `head`), then whether result_json's composition changed " +
         "under this seam. NEVER SOFTEN IT INTO A DEFAULT — the default a reader would reach for is `isCpu` " +
         "true, which is the one answer that is false on the host this extension actually runs");
  DCHECK(typeof r._quantum.measure === "string" && r._quantum.measure !== "",
         "the engine's _quantum carries no `measure` string — it is what the scheduler's slice and the WFQ's " +
         "aging charge are billed in, stated by the component that owns the fact so that no message anywhere " +
         "restates it and goes stale. An empty one is that composer emitting a field it cannot answer");
  DCHECK(typeof r._quantum.isCpu === "boolean",
         "the engine's _quantum carries no boolean `isCpu` — it is a yes/no about the HOST and the producer " +
         "always knows it, so an absent one is a broken contract and never a false. It is deliberately not a " +
         "0/1: a number here would pass the generic census check below, and the whole point of this field is " +
         "that it is not a census reading and must be read by name");
  DCHECK(typeof r._quantum.sliceMs === "number" && Number.isFinite(r._quantum.sliceMs) && r._quantum.sliceMs > 0,
         "the engine's _quantum carries no positive `sliceMs` — it is solver/engine.h's ENGINE_QUANTUM_MS, a " +
         "compile-time constant of the artifact, so a zero or a NaN is the composer having lost it rather " +
         "than a host that shares its thread infinitely finely");
  /* The subsystem censuses: solver/result.h's `_cold`, `_heap`, `_swap`, solver/decide.h's `_forkAt` and
     solver/absent.h's `_absent`. `_absent` counts standard-owned globals the page feature-detected and this
     engine answered with `undefined` (solver/absent.c); it separates a page with nothing to learn from one whose
     guards took their false arm because an API is unbuilt.
     One loop and no name list, as for `_wfq`: these are relayed whole and rendered generically by popup.js, so
     what this consumer checks is that each is an object whose values are finite numbers or histograms.
     The empty shape differs: `_cold`, `_heap`, `_swap` and `_absent` read state that exists at every instant, so
     `{}` is a broken contract (asserted after this loop); `_forkAt` lists predicates that forked, so `{}` is
     the positive statement that this document never forked. */
  for (const k of ["_cold", "_heap", "_swap", "_forkAt", "_absent"]) {
    DCHECK(r[k] && typeof r[k] === "object" && !Array.isArray(r[k]),
           "the engine's result document carries no " + k + " census — solver/result.c composes it into " +
           "every document it builds, partials included. Its absence has the same two causes as a missing " +
           "counter above and in the same order: check first whether the loaded wasm predates this reader " +
           "(extension/lib/qjs/qjs.mjs.build.json's `head`), then whether result_json's composition changed " +
           "under this seam. NEVER SOFTEN IT INTO A DEFAULT — these are the only readings this zone has ever " +
           "been handed of what the pager, the heap and the delta chains are doing on a real page");
    /* A row is a finite number or a per-arm histogram (e.g. `_cold`'s `stepUnits` and `stepUnitRuns` over
       solver/step_unit.h's ladder); a histogram's values are asserted one level down, so a NaN inside one is
       caught like a NaN beside one. An empty histogram is not refused: some tables are keyed on derived keys
       (`_heap`'s `childRealmRefSites`, `_swap`'s `cowHostRecAsksBySite`) whose `{}` is a positive statement, and
       engine/build.mjs's census readers (`cowHostRecSiteReading`, `cowStateReading`, `coldFields`) check key
       sets from the composer's own format string, which a hand list here cannot do.
       Named residual: `_absent`'s per-member bucket tables have no non-empty check on the document in any
       build. Next diff builds an `@ABSENT` reader in engine/build.mjs deriving its row set from `absent_json`'s
       format string through `censusRowSet`. Absence shows as an @ABSENT member with `{}` buckets while the
       member is on the census because a read was recorded.
       Named residual: the shape check refuses `null`, which navigable.h documents as `childRealmRefSites`'
       answer in a build without attribution, so a dev extension loading a release wasm aborts here. Next diff
       admits `null` for that row, taken from the producer. Absence shows as a dev zone driving a release
       artifact reporting no runs, with the @WHY naming that row. */
    for (const f of Object.keys(r[k])) {
      const v = r[k][f];
      const hist = v !== null && typeof v === "object" && !Array.isArray(v);
      DCHECK(hist || (typeof v === "number" && Number.isFinite(v)),
             "the engine's " + k + " census carries a `" + f + "` that is neither a finite number nor a " +
             "per-arm histogram — every row of it is a count, a byte figure, a per-switch mean or a bucket " +
             "table keyed on solver/step_unit.h's arms, and a NaN reaching a reader makes every comparison " +
             "against it false, which is the same silent failure asserted for the WFQ census one block up");
      if (!hist) continue;
      /* A histogram arm is a finite number or a positional tuple of them: solver/result.c composes each
         `childRealmRefSites` arm as `[min, max, total]`. The rule is a shape, not a name list, and a NaN
         anywhere inside either is still caught. It would become a name check if the census named the triple's
         members, which navigable.h's `NavigableRealmRefSite` declares. */
      for (const a of Object.keys(v)) {
        const arm = v[a];
        const tuple = Array.isArray(arm);
        DCHECK(tuple || (typeof arm === "number" && Number.isFinite(arm)),
               "the engine's " + k + " census carries a `" + f + "." + a + "` that is neither a finite " +
               "number nor a positional tuple of them — a histogram arm is a count of members or of steps, " +
               "or the fixed triple a per-site table composes, and a NaN in one makes every sum taken over " +
               "the table false while each other row still looks like a measurement");
        if (!tuple) continue;
        for (let i = 0; i < arm.length; i++)
          DCHECK(typeof arm[i] === "number" && Number.isFinite(arm[i]),
                 "the engine's " + k + " census carries a non-finite element " + i + " of the tuple at `" +
                 f + "." + a + "` — the triple is positional and every member of it is a count, so a NaN " +
                 "inside one is the same silent failure as a NaN beside it");
      }
    }
  }
  for (const k of ["_cold", "_heap", "_swap", "_absent"])
    DCHECK(Object.keys(r[k]).length > 0,
           "the engine's " + k + " census is EMPTY — unlike `_wfq`, whose absent rows are how a drained " +
           "frontier says there was no order to report, this one reads the frontier, the runtime, the C " +
           "allocator or the population of global reads, and all four exist at every instant a document can " +
           "be composed at. There is no reading in which it has no rows, so an empty object is the composer " +
           "having stopped composing");
  /* `_absent` is in that list and `_forkAt` is not: solver/absent.c emits its members on every census, zeroes
     included, so `{}` is the composer having stopped, while `_forkAt`'s `{}` means the document never forked. */
  DCHECK(Array.isArray(r._park),
         "the engine's result document carries no _park array — that is the PARKED RESIDUE (solver/cold.h), " +
         "the recipes this zone writes to IndexedDB and hands back to qjs_begin next session. An absent one " +
         "reads exactly like a fully-explored document, so every flow the engine paged out would be dropped " +
         "here and the cross-session frontier would silently restart from the boot flow on every visit");
}
const RUN_OUTCOMES = ["partial", "complete", "crashed", "nothing-to-run"];
/* Writes the run's own log row. `eng` is the run's identity and is absent for records that belong to no
   instance: nothing to run (which logs nothing) and a boot that aborted before the reservation had an
   instance, whose record is built per waiting caller. A row is written in place, keeping the position the run
   started at; a row already shifted out of the log is no longer displayed, so writing it is a harmless no-op
   where an index would have addressed another run.
   Fields are replaced, not merged: a crash record carries no counters, and `Object.assign` alone would leave
   a partial's numbers under a row labelled crashed. */
function engineLogWrite(eng, m) {
  const row = eng ? eng._log : null;
  if (!row) {
    if (eng) eng._log = m;
    self._engineLog.push(m);
    if (self._engineLog.length > 200) self._engineLog.shift();
    return;
  }
  for (const k of Object.keys(row)) delete row[k];
  Object.assign(row, m);
}
/* The one reader of `_resumed`, keeping its three states apart: `null` engine (nothing-to-run) reports null; an
   engine reports the count `begin` wrote; an engine whose boot died before `begin` carries its reservation's
   `null`. The DCHECK keeps `undefined` from becoming a fourth state rendered as absence. */
function engineResumed(eng) {
  if (eng === null) return null;
  DCHECK(eng && typeof eng === "object",
         "an analysis was composed against an engine record that is neither an instance nor the stated " +
         "absence — `null` is how this seam's callers say 'no instance ran', and any other falsy value is a " +
         "caller that stopped passing one rather than one saying there was none");
  DCHECK(eng._resumed === null ||
         (typeof eng._resumed === "number" && Number.isInteger(eng._resumed) && eng._resumed >= 0),
         "an engine record carries a resume count that is neither a count nor the stated 'not known' (`" +
         String(eng._resumed) + "`) — engineReserve declares it null and engineRoot writes it once at `begin`, " +
         "so anything else is a third writer, and the value it wrote would be rendered to the user as a " +
         "number of parked flows that came back");
  return eng._resumed;
}
/* The one reader of the cold-tier lookup, on `engineResumed`'s shape: no engine reports the stated absence, an
   engine that reached its frontier read reports what it met, and one whose boot died first carries its
   reservation's nulls unchanged. The three fields are returned together because each is unreadable without
   the others: the bundle id is the half of the frontier key not already on the row as `url`, which is what
   makes two drives of one address comparable. */
function engineColdLookup(eng) {
  if (eng === null) return { lookup: null, other: null, bundle: null };
  DCHECK(eng && typeof eng === "object",
         "an analysis was composed against an engine record that is neither an instance nor the stated " +
         "absence while its cold-tier lookup was being read — `null` is how this seam's callers say 'no " +
         "instance ran', and any other falsy value is a caller that stopped passing one");
  DCHECK(eng._coldLookup === null || COLD_LOOKUP.indexOf(eng._coldLookup) >= 0,
         "an engine record carries a cold-tier lookup this seam does not speak (`" + String(eng._coldLookup) +
         "`) — engineReserve declares it null and engineRoot writes one of a closed set of words once, so " +
         "anything else is a second writer, and the reader is shown a reason for a miss that nothing decided");
  /* The two nulls are written on one line by engineRoot and must stay one fact. */
  DCHECK((eng._bundleId === null) === (eng._coldLookup === null),
         "an engine record states a bundle id (`" + String(eng._bundleId) + "`) and a cold-tier lookup (`" +
         String(eng._coldLookup) + "`) that disagree about whether the frontier was ever read — engineRoot " +
         "writes both on one line at the one moment either exists");
  DCHECK(eng._bundleId === null || (typeof eng._bundleId === "string" && eng._bundleId !== ""),
         "an engine record carries a bundle id that is neither a base-36 identifier nor the stated absence " +
         "(`" + String(eng._bundleId) + "`) — it is half the frontier key, and the half a reader needs to " +
         "tell a redeployed bundle from a cold tier that lost an entry");
  /* The count is present exactly where keys were read: `not-asked` and `unreadable` read none, so a zero from
     them would falsely claim this address holds no other entries (which is what `unvisited` says). */
  const counted = eng._coldLookup !== null && eng._coldLookup !== "not-asked" && eng._coldLookup !== "unreadable";
  DCHECK(counted === (eng._coldOther !== null),
         "an engine record reports a cold-tier lookup of `" + String(eng._coldLookup) + "` with a sibling " +
         "count of `" + String(eng._coldOther) + "` — the two arms that read no keys state the absence of a " +
         "count and every other arm states one, and a zero from an arm that never looked is the claim this " +
         "address has nothing else parked at it");
  DCHECK(eng._coldOther === null ||
         (typeof eng._coldOther === "number" && Number.isInteger(eng._coldOther) && eng._coldOther >= 0),
         "an engine record carries a sibling-entry count that is neither a count nor the stated absence of " +
         "one (`" + String(eng._coldOther) + "`) — it is how many entries this document's address holds under " +
         "OTHER bundle ids, which is the evidence under the word beside it");
  return { lookup: eng._coldLookup, other: eng._coldOther, bundle: eng._bundleId };
}
/* The emitted surface partitioned by a fact each @H row states about itself, over the same `fetchCallSites`
   array whose length is `endpoints` on the run record, so partition and total are one population at one
   moment. solver/endpoint.c writes `door`, `mintedAt` and the other class keys per row; the histogram is taken
   here, not in lib/merge.js, because that loop skips structural and non-network rows and folds rows by
   `method+host+path`, while a door is a per-sighting fact the producer refuses to fold.
   An absent key is its own bucket, `(unstated)`: an artifact predating the key is ordinary because this JS
   ships before the engine is rebuilt. The parentheses keep it from colliding with producer tokens.
   Values are not checked against a copied vocabulary: the door list grows with endpoint_record's call sites,
   so a copy here would abort the trusted zone on a correct new door. Only a non-empty string is asserted.
   A null-prototype map is used so an engine key spelled `constructor` or `__proto__` cannot read
   `Object.prototype`. */
const ENDPOINT_FACT_UNSTATED = "(unstated)";
function endpointFactHistogram(rows, key) {
  const h = Object.create(null);
  for (let i = 0; i < rows.length; i++) {
    /* `in`, never `||`: an absent key is a fact about the build, a present one a fact about the address. */
    let v;
    if (!(key in rows[i])) v = ENDPOINT_FACT_UNSTATED;
    else {
      DCHECK(typeof rows[i][key] === "string" && rows[i][key] !== "",
             "an @H row carries `" + key + ": " + JSON.stringify(rows[i][key]) + "` — solver/endpoint.c " +
             "writes both of these keys through a function returning one of a fixed table of C string " +
             "literals, so anything that is not a non-empty string is that emission and this reader having " +
             "parted, and the row would partition under a key nothing can be read as");
      v = rows[i][key];
    }
    h[v] = (h[v] === undefined ? 0 : h[v]) + 1;
  }
  return Object.assign({}, h);
}
/* The same partition over `addressRoot`, as a separate reader for two reasons. Its domain is three JSON types,
   not a token: solver/endpoint.c emits `null` where no sighting held a concolic address, `false` where one did
   and its bytes entered through no engine-minted source, and a non-empty string naming the sources otherwise.
   And the string is page-influenced (solver/absent.c mints source names from page property names), so it may
   not be a histogram key: `Object.assign({}, h)` copies with [[Set]] and a `__proto__` key would set a
   prototype. The names stay on the row; this counts the four-way state under fixed parenthesised keys.
   It is a diagnostic, not an operand of the bar: whose unknown a root names is not decidable from the string. */
const ENDPOINT_ROOT_NAMED = "(named)";
const ENDPOINT_ROOT_UNATTRIBUTED = "(unattributed)";
const ENDPOINT_ROOT_NO_CONCOLIC = "(no-concolic)";
function endpointAddressRootHistogram(rows) {
  const h = Object.create(null);
  for (let i = 0; i < rows.length; i++) {
    let v;
    if (!("addressRoot" in rows[i])) v = ENDPOINT_FACT_UNSTATED;
    else if (rows[i].addressRoot === null) v = ENDPOINT_ROOT_NO_CONCOLIC;
    else if (rows[i].addressRoot === false) v = ENDPOINT_ROOT_UNATTRIBUTED;
    else {
      DCHECK(typeof rows[i].addressRoot === "string" && rows[i].addressRoot !== "",
             "an @H row carries `addressRoot: " + JSON.stringify(rows[i].addressRoot) + "` — solver/endpoint.c "
             + "writes this key in one of exactly three JSON types (`null`, `false`, or the delivery root as a "
             + "non-empty string), so a fourth is that emission and this reader having parted, and the row "
             + "would partition under a state nothing can be read as");
      v = ENDPOINT_ROOT_NAMED;
    }
    h[v] = (h[v] === undefined ? 0 : h[v]) + 1;
  }
  return Object.assign({}, h);
}
function linesToAnalysis(lines, msg, outcome, eng) {
  DCHECK(RUN_OUTCOMES.indexOf(outcome) >= 0,
         "a run outcome this seam does not speak: `" + outcome + "` — every consumer of an analysis branches " +
         "on `_run`, so a word none of them knows is a run whose completeness nothing can judge");
  /* The document this analysis is about. lib/merge.js resolves relative call-site addresses against
     `sourceUrl` and keys each security finding on it, so it is asserted, never defaulted. engineRoot asserts the
     same field non-empty and `eng.msg === msg` where the instance is rooted; both callers pass `eng.msg`. */
  DCHECK(!!msg && typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
         "an analysis is being built for a document with no address (`" + (msg && msg.sourceUrl) + "`) — " +
         "engineRoot asserts this same field non-empty before the instance runs, so its absence here is that " +
         "message record having been replaced, and every finding on this analysis would be keyed on the " +
         "empty string and land in the moat under a fabricated name");
  let result = null;
  const extraErrors = [];
  /* The resume count is a fact about the session, read off the record (`eng._resumed`, written once by
     engineRoot at `begin`), never off `lines`: callers pass different slices of output (`finish` the whole run,
     `streamPartial` one @RESULT, `crashRecord` a synthetic line), and only the first holds `@RESUMED`. A
     session that never reached `begin` carries `null`, which this function cannot distinguish itself. */
  const resumed = engineResumed(eng);
  /* The cold-tier lookup is also a fact about how the session began, so it is reported on both record shapes;
     a crashed run's reader needs it most. */
  const cold = engineColdLookup(eng);
  /* The cause of a crash. Both producers of a crashed record (engineCrash, with the root @WHY appended, and
     crashRecord for a boot that never got an instance) put an `@E {"phase":"engine-crash",…,"err":…}` line in
     `lines` before calling here. */
  let crashErr = "";
  for (const raw of lines) {
    const ln = String(raw);
    if (ln.startsWith("@RESULT ")) {
      try { result = JSON.parse(ln.slice(8)); }
      catch (e) {
        /* result.c asserts its own document against truncation, so unparseable text is an engine defect. Dev
           aborts; release records the parse error so a zero result still says why. */
        DFAIL("the engine emitted an @RESULT line that is not JSON: " + String(e && e.message || e));
        extraErrors.push({ context: "result-parse", message: String(e && e.message || e) });
      }
    } else if (ln.startsWith("@E ")) {
      extraErrors.push({ context: "engine", message: ln.slice(3) });
      /* Parsed, not substring-matched: the `phase` field identifies the crash line, so a page's own error
         text cannot be mistaken for it. Non-JSON `@E` lines are other diagnostics, recorded above. */
      try {
        const o = JSON.parse(ln.slice(3));
        if (o && o.phase === "engine-crash" && typeof o.err === "string") crashErr = o.err;
      } catch (_) { /* not the crash line; it is already recorded as an engine error above */ }
    } else if (ln.startsWith("@WHY ")) {
      /* An engine diagnostic for a zero-result or resource path, surfaced so no flow fails silently. Two
         producers: engine/host/check.h emits a JSON record (phase/cond/at/reason, escaped at the emitter), and
         engine/qjs/quickjs-check.h, which cannot include the host header, emits a plain line whose text is the
         message. Non-JSON is therefore the second producer, not a parse failure. On the JSON arm `phase` and
         `reason` are asserted, never defaulted: `reason` is the field naming what to build. */
      let o = null;
      try { o = JSON.parse(ln.slice(5)); } catch (_) { o = null; }
      if (o === null) { extraErrors.push({ context: "why", message: ln.slice(5) }); continue; }
      DCHECK(typeof o.phase === "string" && typeof o.reason === "string",
        "a check.h @WHY record parsed as JSON but is missing `phase` or `reason`. APICLIENT_ASSERT_EMIT " +
        "writes all four fields on every line, so this is that emitter and this reader having come apart — " +
        "and `reason` is the only field of the four that names the capability to build: " + ln.slice(5, 400));
      extraErrors.push({ context: o.phase, message: o.reason });
    }
  }
  /* A partial or complete run must carry its document; reporting its absence as an empty page would be a false
     clean bill. */
  const mustHaveDocument = (outcome === "partial" || outcome === "complete");
  DCHECK(!mustHaveDocument || result !== null,
         "an engine session produced no @RESULT document at all — the one result document is what every " +
         "finding for this page travels in, and reporting its absence as an empty page is a false clean bill");
  /* A document that is present is asserted whether or not one was required, including a crashed instance's
     unconsumed partial, whose fields are read below. */
  if (result) assertResultDocument(result);
  /* A complete run with an empty `_park` must have no live members. `finish` joins `_park` into the recipe
     string and frontierWrite deletes the origin's cross-session entry when it is empty, which is right for a
     drained frontier (solver/cold.h) and an erasure of earlier parked flows otherwise. solver/engine.c asserts
     `flow_count() == 0 || engine_frontier_paged()` at session close, but only in dev, while this delete runs
     in every build.
     One-directional: a non-empty `_park` with `live` 0 is legitimate (a peer reference can keep a session
     live, a RAM-floor engine may have sold every flow, and a park does not free the flows it writes).
     Scoped to `complete`, the only outcome meaning the session closed (`finish` runs on ENGINE_STEP_DONE); a
     partial snapshot is mid-run, where a live frontier with no park is normal. `live` is asserted present on
     its own line because assertResultDocument does not name that row. */
  if (result && outcome === "complete") {
    DCHECK(typeof result._cold.live === "number" && Number.isFinite(result._cold.live),
           "a complete run's `_cold` census carries no `live` member count (`" + String(result._cold.live) +
           "`) — it is the gauge of flows still standing when the session closed (solver/cold.c counts one per " +
           "flow_at), and it is the only thing in this document that can say whether an empty `_park` means " +
           "the frontier DRAINED or that its members were dropped, which is what the frontier delete below " +
           "turns into an erasure of a previous session's residue");
    DCHECK(result._park.length > 0 || result._cold.live === 0,
           "a COMPLETE run closed its session over " + result._cold.live + " live frontier member(s) and " +
           "wrote NO park recipes — every one of them holds a snapshot, a COW delta and an unexplored " +
           "timeline, and `finish` is about to join that empty array into the empty string, which " +
           "frontierWrite reads as DELETE THIS ORIGIN'S ENTRY. So this is both a dropped work item (CLAUDE.md " +
           "\u00a7NO BOUNDS: a resting member is deprioritized and PAGED, never terminated) and the erasure " +
           "of whatever a PREVIOUS session parked at this address. solver/engine.c asserts the engine half of " +
           "this at its session close; that assert is dev-only and this delete is not");
  }
  /* The absent-document case is handled once on its own arm, so every read on the present arm is off a
     document assertResultDocument has checked, and none is defaulted. */
  /* The counters arm reads `eng._egress`. The two callers that may pass a null `eng` (crashRecord and the
     nothing-to-run arm) never take this branch, so a null here means that split moved. */
  DCHECK(!(outcome !== "crashed" && result) || (eng && eng._egress),
         "a run reached the counters arm with no egress census on its instance — `_egress` is declared in the " +
         "`eng` literal beside `_cold`, so an instance without one was built somewhere else, and the row would " +
         "report this zone's own refusals as absent for a run that had them");
  /* The counters arm also reads `eng.residentBytes`, which engineRecordFacts writes before the reservation
     leaves `booting`; only an instance that has stepped reaches here. An absent value would be dropped by the
     structured clone and read as a producer that never existed. */
  DCHECK(!(outcome !== "crashed" && result) || (eng && typeof eng.residentBytes === "number"),
         "a run reached the counters arm with no reported working set on its instance — engineRecordFacts " +
         "states it at the end of every round and before the reservation becomes hot, so its absence is that " +
         "ordering having moved, and the one byte figure on this row that can answer what share of the wasm32 " +
         "address space this instance has taken would be missing from a run that had it");
  const m = (outcome !== "crashed" && result)
    /* The scheduler's own counters, so context switching, forking and preemption are observable. Every value
       is a number off the document asserted above. */
    ? { run: outcome,
        switches: result._switches, flows: result._flows, candidates: result._candidates,
        jobsQueued: result._jobsQueued, jobsRun: result._jobsRun,
        /* The precondition for running a job, beside the job count: reactions that never fire and flows that
           never reach the between-units boundary need opposite fixes (see solver/engine.c's g_units_done). */
        unitsDone: result._unitsDone,
        worldSegmentsHeld: result._worldSegmentsHeld, worldSegmentsMade: result._worldSegmentsMade,
        worldSegmentsForked: result._worldSegmentsForked,
        /* What the work met, which is what tells a page with nothing to find from a page nobody reached. Carried
           on the record because the probe watching this seam cannot read the cumulative store. */
        sourceReads: result._sourceReads, sinkReached: result._sinkReached,
        sinkTainted: result._sinkTainted, sinkSuppressed: result._sinkSuppressed,
        /* Uncalled-code driving: `driven` alone cannot tell a page that ships no uncalled code from a scheduler
           that never reached the question; `asked` can. */
        orphansDriven: result._orphansDriven, orphansAsked: result._orphansAsked,
        /* Which exit each ask took. `memo` is an ask the generation cache answered with no walk; `empty` a walk
           that found nothing (a fact about the heap: the walk sees only bodies with a live function object);
           `took` a productive walk. A low `memo` points at walk cost, which `orphanWalks` and its siblings
           price. The three sum to `asked`, which solver/result.c asserts. */
        orphanAskMemo: result._orphanAskMemo, orphanAskEmpty: result._orphanAskEmpty,
        orphanAskTook: result._orphanAskTook,
        /* Walk cost with both denominators. `entries / walks` is the mean object-list length one take reads;
           `fullCandidates / walksFull` is the mean candidate population over walks that reached the end of the
           list, since an early exit on a preferred candidate sees only a floor of the set. `walksFull` 0 with
           `walks` nonzero means every take was decided early. solver/result.c asserts
           `walks == empty + took`. */
        orphanWalks: result._orphanWalks, orphanWalkEntries: result._orphanWalkEntries,
        orphanWalksFull: result._orphanWalksFull,
        orphanWalkFullCandidates: result._orphanWalkFullCandidates,
        /* What the run learned (`endpoints`, `sinks` below), beside what it cost. The crash arm carries
           neither: a zero there would read as an analysed, clean page. */
        /* The WFQ census at the instant the document was composed. It is a reading of an instant, not a
           total, so it stays one nested object. It crosses whole: this zone reads no row of it, so a row added
           to the census reaches the popup unedited. A partial's census is the informative one (qjs_emit_partial
           composes one every PARTIAL_MS over a live frontier); a finalize's is composed after drain or park and
           reads `{members: 0}`. */
        wfq: result._wfq,
        /* The subsystem censuses, crossing whole for the same reason. They stay separate objects because a
           reader compares within a census, never across. */
        cold: result._cold, heap: result._heap, swap: result._swap, forkAt: result._forkAt,
        /* Standard-owned names the document read and this realm did not answer, with the population of global
           reads they are drawn from. */
        absent: result._absent,
        /* What the order above was denominated in, relayed whole. It is a property of the host and build,
           constant for the session. With no `-pthread` in the link, linear memory is an ordinary ArrayBuffer
           and no second thread can raise the yield request, so the aging charge is billed in wall time and the
           OS's descheduling decides part of the order; two runs of one build can differ, and the popup renders
           this beside the order so that is not read as an engine change. solver/quantum.c's #error keeps the
           link and this denomination in step. */
        quantum: result._quantum,
        /* What this zone's egress policy did, which the engine's counters cannot say: a page with no API
           surface is either one the engine never derived requests for or one whose requests this tool refused,
           and the two need opposite work (improve driving, or widen the origin).
           `egressAsked` is the denominator; `egressDeclined` is keyed on the rule, not the signal name, since
           rules differ in whether a person can reopen them. It is a different denominator from the engine's
           `cold.replyDeclined` (one decline of a (method, url) pair marks every parked record on it); this row
           nonzero with `replyDeclined` 0 is a refusal that never reached the engine.
           The map is copied so the row is one moment: a live reference would keep changing after the partial
           was composed, out of step with `egressAsked`. Null-prototype in (exact `in`), plain object out, like
           every other census on the row. */
        egressAsked: eng._egress.asked, egressDeclined: Object.assign({}, eng._egress.declined),
        /* The instance's whole linear memory, `M.HEAPU8.length` as renderer.html stated it on the reply
           engineRecordFacts last awaited: arena plus stack plus static data, so it is the only figure here that
           answers what share of the wasm32 address space the instance has taken (`heap.arenaKiB` is a subset).
           It is separate from `heap`, whose rows build.mjs derives from result_heap_json's format string, and
           it is a different observer at a different moment, so no quotient against `heap` is composed. A wasm
           Memory never shrinks, so the latest value is the high-water mark and a share of the per-module ceiling
           is sound; `_residentBytes` sums it across engines against a device RAM floor, a different question.
           engineRecordFacts asserts the whole-wasm-page shape where the value enters this zone. */
        workingSetBytes: eng.residentBytes,
        endpoints: result.fetchCallSites.length, sinks: result.securitySinks.length,
        /* What composed each address (`door`) and whether the page's own code had run when it did
           (`mintedAt`), per row, so the share of the surface owed to forced execution is read off the rows
           rather than by subtracting two totals. Two histograms, not one keyed on the pair: solver/endpoint.c
           states they are two facts, and a reader reads the margins. Each sums to `endpoints`. */
        endpointDoors: endpointFactHistogram(result.fetchCallSites, "door"),
        endpointMintedAt: endpointFactHistogram(result.fetchCallSites, "mintedAt"),
        /* The address class, the one of these that answers the product's bar (an address no parse of the served
           bytes can state). Doors cannot be coarsened into it: a literal chunk URL via `module-import` is beyond a
           markup parse yet clears nothing, while `/api/{location.hash}` via `fetch` clears it.
           It is a floor, not a verdict: `unknown` means the run reached the address holding a value it had not
           determined and resting on an unknown supplied from outside the engine, so those rows clear the bar;
           `concrete` claims nothing about a parse (solver/endpoint.h enumerates what it hides). This zone holds
           no copy of that classification, so a class added to `ENDPOINT_ADDRESS_CLASSES` reaches it unedited, and
           a row from an older wasm lands in `(unstated)`. */
        endpointAddressClass: endpointFactHistogram(result.fetchCallSites, "addressClass"),
        /* The razor class, the figure a person may read as the bar: solver/endpoint.c unions the address
           class's `unknown` rows with the rows whose door handed it bytes never in the served document, per row,
           where both facts are in hand. The union is composed there, not here, so this zone holds no door map
           that would abort it when the engine adds a door (and `EPR_BEYOND`, the markup-parse class, is not the
           operand). It derives from `door` and `addressClass` and adds no observation: `unproven` is not a claim
           that a parse could state the address. Rendered and asserted generically, like the rows above. */
        endpointRazorClass: endpointFactHistogram(result.fetchCallSites, "razorClass"),
        /* The witness class, sizing the population `unproven` hides (a source a flow pinned and re-read). It is
           never unioned into the bar: `may-rest-on` is a fact about the path, not this address's bytes, so
           folding it in would turn a floor into an over-claim. The columns are read side by side. Rendered
           and asserted generically. */
        endpointWitnessClass: endpointFactHistogram(result.fetchCallSites, "witnessClass"),
        /* The delivery-root state the address class derives from, sizing the population whose `unknown` is a
           hole `engine_orphan_call` minted for its own drive (solver/endpoint.c's `address_class_of` residual).
           It is a third observation about the same array and is not unioned into or subtracted from the others:
           `(named)` does not claim the bytes are the page's, nor `(unattributed)` that they are the engine's.
           The names stay on the emitted row, since a page-influenced string may not be a key here. */
        endpointAddressRoot: endpointAddressRootHistogram(result.fetchCallSites),
        park: result._park.length, resumed: resumed,
        coldLookup: cold.lookup, coldOther: cold.other, bundleId: cold.bundle,
        url: (msg && msg.sourceUrl) || "" }
    /* A crashed run reports no cost counters: zeroes would read as a run that explored nothing. `run` keeps the
       three states (partial, ended, died) apart. The arm is chosen by the outcome, not by whether a document
       arrived, so a crash that left an unconsumed partial logs as crashed; its findings still travel on the
       returned analysis labelled `_run:"crashed"`. */
    /* It does carry its cause, `err`, the same root @WHY string engineCrash puts on the banner, so the row
       the popup's GET_ENGINE_RUNS and testing/live-run.js read names the capability that failed. */
    : { run: outcome, resumed: resumed,
        coldLookup: cold.lookup, coldOther: cold.other, bundleId: cold.bundle,
        url: (msg && msg.sourceUrl) || "", err: crashErr };
  /* Both crash producers write the `engine-crash` line before calling here, so an empty `err` (asserted below)
     is a third crash path or a producer that stopped writing it. */
  /* The egress containment: `asked` and each decline are raised in engineServiceFetch's one loop (one ask per
     iteration, at most one decline in it), so a histogram summing past its denominator is a second raiser. */
  if (outcome !== "crashed" && result) {
    /* Read off the row's copy, not `eng`, so it is an assertion about what the row states. */
    let _sum = 0;
    for (const k of Object.keys(m.egressDeclined)) _sum += m.egressDeclined[k];
    DCHECK(_sum <= m.egressAsked,
           "this zone's egress census counted " + _sum + " refusal(s) against " + m.egressAsked + " " +
           "request(s) asked of it — the two are raised in one loop, one `asked` per delivered pending line " +
           "and at most one refusal inside that same iteration, so a sum above the denominator is a second " +
           "site raising one of them and every share read off this pair is over a population that never ran");
    /* The endpoint partitions are partitions: (a) each histogram sums to `endpoints`, which fails if one is
       built over a filtered or deduped walk; (b) the `(unstated)` bucket is all or nothing, since one instance
       emits one document shape and a key stated for some rows only is a conditional emit. */
    /* This list is the only statement of which partitions are checked, so a histogram added above needs a line
       here. `endpointAddressRoot` is built by a different reader; both claims hold regardless. */
    for (const _p of [["endpointDoors", m.endpointDoors], ["endpointMintedAt", m.endpointMintedAt],
                      ["endpointAddressClass", m.endpointAddressClass],
                      ["endpointRazorClass", m.endpointRazorClass],
                      ["endpointWitnessClass", m.endpointWitnessClass],
                      ["endpointAddressRoot", m.endpointAddressRoot]]) {
      let _n = 0;
      for (const k of Object.keys(_p[1])) _n += _p[1][k];
      DCHECK(_n === m.endpoints,
             "this run's `" + _p[0] + "` partition sums to " + _n + " over an emitted surface of " +
             m.endpoints + " row(s) — both are read off the ONE `fetchCallSites` array at this composition, " +
             "so a difference is a walk that filtered or deduped rows the figure beside it still counts, and " +
             "the partition would be read as a statement about the surface a person is shown");
      const _u = _p[1][ENDPOINT_FACT_UNSTATED] === undefined ? 0 : _p[1][ENDPOINT_FACT_UNSTATED];
      DCHECK(_u === 0 || _u === m.endpoints,
             "this run's `" + _p[0] + "` partition states the fact for some rows and not others (" + _u +
             " unstated of " + m.endpoints + ") — solver/endpoint.c writes every one of these keys " +
             "unconditionally in " +
             "one loop over one array, so a mixed run is that emit having become conditional, and the " +
             "bucket that means 'this artifact predates the key' would be read as a property of some " +
             "addresses");
    }
    /* The two readers agree on the row both can see: solver/endpoint.c emits `addressRoot: null` on exactly the
       rows whose `addressClass` is `concrete`, from two different emit lines, so a difference is those emits
       having parted.
       The check is gated on both keys being stated. This JS is live when written while the engine is live only
       after a build, so it is routinely newer than the wasm, and an artifact that emits `addressClass` but not
       `addressRoot` passes the per-key all-or-nothing claim above (fully unstated) yet would compare 0 against
       a real `concrete` count. popup.js follows the same rule for its partitions. */
    const _rootUnstated = m.endpointAddressRoot[ENDPOINT_FACT_UNSTATED] === undefined
                          ? 0 : m.endpointAddressRoot[ENDPOINT_FACT_UNSTATED];
    const _clsUnstated = m.endpointAddressClass[ENDPOINT_FACT_UNSTATED] === undefined
                         ? 0 : m.endpointAddressClass[ENDPOINT_FACT_UNSTATED];
    /* Both operands are gated, since the precondition of a two-operand comparison is two-sided; gating one
       would claim knowledge of which key is newer. */
    const _bothStated = _rootUnstated === 0 && _clsUnstated === 0;
    const _nc = m.endpointAddressRoot[ENDPOINT_ROOT_NO_CONCOLIC] === undefined
                ? 0 : m.endpointAddressRoot[ENDPOINT_ROOT_NO_CONCOLIC];
    const _cc = m.endpointAddressClass["concrete"] === undefined ? 0 : m.endpointAddressClass["concrete"];
    DCHECK(!_bothStated || _nc === _cc,
           "this run counts " + _nc + " address(es) with no delivery root to ask against " + _cc + " the " +
           "engine classed `concrete`, with BOTH keys stated by this artifact for every one of its " +
           m.endpoints + " row(s) — solver/endpoint.c writes `addressRoot: null` on exactly the rows " +
           "whose `addressClass` is `concrete`, from the one stored class, so a difference is those two emits " +
           "having parted and `(no-concolic)` would be read as a statement about addresses it is not about");
  }
  DCHECK(outcome !== "crashed" || (typeof crashErr === "string" && crashErr !== ""),
         "a crashed run reached the run log with no `engine-crash` line among its output — every crash path " +
         "writes one (engineCrash appends the ROOT @WHY to it, crashRecord constructs it), so a crash with " +
         "no cause here is a producer that stopped announcing itself and a row a reader cannot act on");
  // A per-run log, not a single overwritten global: concurrent engines each report here, so the whole
  // park, persist, rehydrate, resume sequence across engines is observable.
  /* A document with nothing to run produces no record, because no engine ran: an absent row, a row of zeroes
     and a crash row are three different facts. */
  if (outcome !== "nothing-to-run") engineLogWrite(eng, m);
  /* The engine-owned fields `fetchCallSites`, `securitySinks` and `_park` are present or absent, never empty
     substitutes: their presence states that an @RESULT document arrived and was asserted, and every consumer
     reads that presence (lib/merge.js, offscreen-brain's merges, the frontier write). An empty substitute would
     make "no document arrived" and "analysed and clean" identical. `resolverErrors` is the host's record and is
     on both arms; on the no-document arm it carries the `@E engine-crash` row. `_run` says which absence.
     The object carries only what the offscreen reads; a constant `[]` is not a measurement and is not shipped. */
  const analysis = {
    /* The engine's page errors as `{context, message}` rows of two strings. `context` is a closed 2x2: the
       base is `page` (standing) or `page-retracted` (taken back, HTML §8.1.6.4
       HostPromiseRejectionTracker(promise, operation), still worth showing because it names a capability the
       page reached for), and `-explored` is appended where the throw was engine-minted exploration, since
       `pageErrorsExplored` overlaps the other two and concatenating it would show one message twice. popup.js
       renders `context + ": " + message` verbatim, so nothing downstream learns a new shape. A Set keeps the
       lookup linear. */
    resolverErrors: (function () {
      if (!result) return [].concat(extraErrors);
      const explored = new Set(result.pageErrorsExplored.map((e) => String(e)));
      const row = (base) => (e) => {
        const message = String(e);
        return { context: explored.has(message) ? base + "-explored" : base, message };
      };
      return result.pageErrors.map(row("page"))
          .concat(result.pageErrorsRetracted.map(row("page-retracted")))
          .concat(extraErrors);
    })(),
    /* No probeResults on this seam: the engine issues no requests. lib/req2proto.js (driven by
       lib/discovery-probe.js and lib/response-decode.js) writes `globalStore.probeResults` directly. */
    // The document this analysis is about, asserted at the top of this function rather than defaulted here.
    sourceUrl: msg.sourceUrl,
    /* What this analysis is a record of (see `outcome` above); written on every arm, so consumers assert it. */
    _run: outcome,
  };
  /* The engine-owned fields, added only where the engine stated them, read straight off `result` so no object
     here exists for a document to be defaulted into. */
  if (result) {
    analysis.fetchCallSites = result.fetchCallSites;
    analysis.securitySinks = result.securitySinks;
    analysis._park = result._park;
  }
  return analysis;
}
/* Whether an analysis carries an engine document, asked in one place so the three fields cannot drift apart
   across consumers. True means an @RESULT arrived and was asserted; false means none did and `_run` says which
   absence. A record with some but not all of the trio is linesToAnalysis broken. */
function analysisHasDocument(a) {
  DCHECK(a && typeof a === "object", "an analysis record is not an object — every producer of one is in this file");
  const n = (a.fetchCallSites !== undefined) + (a.securitySinks !== undefined) + (a._park !== undefined);
  DCHECK(n === 0 || n === 3,
         "an analysis carries " + n + " of the three engine-document fields (fetchCallSites, securitySinks, " +
         "_park) — linesToAnalysis writes all three or none, because their presence IS the statement that an " +
         "@RESULT document arrived, and a partial set would be read by every consumer as a document that is " +
         "there while one of the surfaces it travels in is silently gone");
  DCHECK(n === 3 || a._run === "crashed" || a._run === "nothing-to-run",
         "an analysis marked `" + a._run + "` carries no engine document — only a crashed instance and a page " +
         "with nothing to run may lack one, so a partial/complete run without one is the @RESULT relay broken " +
         "and reporting it as a page with no API surface is a false clean bill");
  return n === 3;
}
self.analysisHasDocument = analysisHasDocument;   // read by offscreen-brain.js and lib/merge.js across the zone's one realm

/* The origin of a URL, or "" when it does not parse. */
function originOf(u) { try { return new URL(u).origin; } catch (_) { return ""; } }
/* The origin stamped on a delivered message, serialized as HTML does. SECURITY.md: routing and the origin
   stamped on a delivered message belong to the trusted zone alone, since a forgeable `event.origin` defeats
   every origin check in the analysed bundle. This zone holds `_senderOrigin`'s per-document "null:<uuid>"
   token for an opaque document, so two opaque documents never compare same-origin; that token is internal,
   and HTML §7.1.1 Origins serializes an opaque origin as "null", which is what pages compare against. */
function stampOrigin(o) { return _isRealOrigin(o) ? o : "null"; }
/* The agent cluster an instance is, SECURITY.md's (browsing-context group, origin), computed here because
   both halves are browser-stated and the engine may state neither. Same-origin documents of one group share
   one similar-origin window agent and therefore one heap (navigable.c's `child_in_this_agent` builds a
   same-origin child as a second realm in one JSRuntime); one instance still holds exactly one origin, so a
   fetch out of it has one principal.
   The group half is `sender.tab.id`: no API exposes a browsing-context group, and a tab never joins documents
   from different groups. Named residual: an auxiliary context opened without `noopener` shares its opener's
   group but lands in another tab; next diff joins it to its opener's group from a browser-stated opener fact;
   absence shows as an opener and its popup in two instances. Engine-opened auxiliaries take their creator's group.
   The origin half is `_senderOrigin`'s, opaque-unique, never `originOf(sourceUrl)` (a sandboxed document's
   address parses to a tuple origin the browser refused it). A rehydrated cold recipe's group is `cold:` plus
   its frontier key (`<address>|<bundle>`), unique per recipe, so its stored principal, even an empty one,
   collides with nothing. A NUL separator, because neither half can contain one. */
function clusterKeyOf(msg) {
  DCHECK(msg && msg.groupId != null && msg.groupId !== "",
         "a document reached the pool naming no browsing-context group — an instance IS its (group, origin) " +
         "agent cluster, so a document with no group would share one cluster (and one heap, and one principal) " +
         "with every document of its origin in every tab the user has open");
  return String(msg && msg.groupId) + "\u0000" + String((msg && msg.origin) || "");
}
/* The frontier key is the document's address plus its program: `address + "|" + bundle`, with the bundle id
   computed by the engine (qjs_bundle_id, a Lexbor <script> scan of the external src set, whose content-hashed
   names are the app version). A recipe is a path, not a function: solver/cold.c writes `f<chain-id>,<reward>`,
   a positional id into the decision chain that document's own scripts built, so it replays only in the same
   document running the same program. The address (DOM §4.5 Interface Document) separates one SPA route from
   another, since an SPA ships one bundle to every route; the bundle half invalidates a residue when a redeploy
   changes a src. A document at a volatile address keys a new entry per visit and never resumes, which is
   correct: no one will visit that address again. */
/* The cross-session flow frontier (IndexedDB). The learned surface persists in globalStore; this persists the
   unfinished frontier as compact replay recipes, so a parked frontier resumes on a later visit or session as
   one continuous attention across sessions. */
/* The store version is the entry grammar's version, executed in the upgrade transaction (IndexedDB §5.7
   Upgrading a database). Each arm states the version that introduced it and runs only for older profiles,
   so a future version bump never wipes the frontier as a side effect.
   v2 added `responseHeaders` (see frontierDoc): a v1 entry cannot state the response its document came from,
   so it cannot resume under the right policy container and is deleted rather than defaulted to no CSP.
   v3 adds `prefs` (the configured device share, see residency, and the egress table): this zone's state lives
   in IndexedDB, never `chrome.storage.local`, beside the store it is about. */
function idbOpen() {
  return new Promise((res, rej) => {
    const r = indexedDB.open("apiclient-frontier", 3);
    r.onupgradeneeded = (ev) => {
      const db = r.result;
      if (ev.oldVersion < 2) {   // v1's entry grammar cannot state its document's response; it is not resumable
        if (db.objectStoreNames.contains("frontier")) db.deleteObjectStore("frontier");
        db.createObjectStore("frontier");
      }
      if (ev.oldVersion < 3 && !db.objectStoreNames.contains("prefs")) db.createObjectStore("prefs");
    };
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}
/* The preference edge. Failures are surfaced like the frontier's own (frontierFail), so an unreadable
   preference is not mistaken for one the user never set. */
function frontierPref(name) {
  return new Promise((res, rej) => {
    idbOpen().then((db) => {
      const t = db.transaction("prefs").objectStore("prefs").get(name);
      t.onsuccess = () => res(t.result === undefined ? null : t.result);
      t.onerror = () => rej(t.error);
    }, rej);
  }).catch((e) => { RETHROW_FATAL(e); frontierFail("preference read", e); return null; });
}
function frontierPrefPut(name, value) {
  return new Promise((res, rej) => {
    idbOpen().then((db) => {
      const tx = db.transaction("prefs", "readwrite");
      const t = tx.objectStore("prefs").put(value, name);
      t.onsuccess = () => res();
      t.onerror = () => rej(t.error || tx.error);
      tx.onabort = () => rej(tx.error || t.error);
    }, rej);
  }).catch((e) => { RETHROW_FATAL(e); frontierFail("preference write", e); });
}
/* ─── The per-origin exploration widening, persisted ──────────────────────────────────────────────────
   The decision lives in lib/safe-fetch.js (the table, what a widening means, the refusal). This file holds
   only what is the host's: where a person's standing permission is kept between sessions, and when it is
   stated; engine/trusted.mjs answers both differently for its command line. It sits in the `prefs` store as
   a record of what this person permitted, outside the cumulative store, so a store-shape migration can
   never drop or invent a permission.
   The store follows the table: every grant and revocation goes into the chokepoint first and is written back
   from `safeFetchEgressTable()`, so the persisted copy is a projection, never a second registry.
   `null` means the restore has not been started; the chokepoint itself keeps "stated empty" and "not yet
   stated" apart. */
let _egressPolicyStated = null;
/* The origins whose standing permissions this restore could not carry forward, kept as a list so the surface
   can name them; a permission that silently vanishes reads as a broken control or as still granted. */
let _egressLegacyDropped = [];
function egressPolicyReady() {
  if (_egressPolicyStated === null) _egressPolicyStated = (async () => {
    const stored = await frontierPref("exploreOrigins");
    /* The stored value is this zone's own write, so an unreadable shape is corruption, except an array: the
       previous single-switch control's legacy shape. It is dropped rather than carried forward, because a
       bare origin meant "fire everything here" and everything now includes signals that did not exist then;
       the person re-permits per signal and is told. `safeFetchEgressStated` re-asks each member with the
       chokepoint's own predicate and returns the entries it could not use. */
    DCHECK(stored === null || typeof stored === "object",
           "the stored per-origin egress table is neither null nor an object (" + String(stored) + ") — this " +
           "zone is its only writer, so this is that record corrupted, and reading past it would state an " +
           "EMPTY table while every origin this person has permitted silently stopped being permitted");
    if (Array.isArray(stored)) {
      /* The legacy shape is translated here: the chokepoint refuses an array outright (a CHECK). */
      _egressLegacyDropped = stored.filter((o) => typeof o === "string");
      self.safeFetchEgressStated({});
    } else {
      _egressLegacyDropped = self.safeFetchEgressStated(stored === null ? {} : stored);
    }
  })();
  return _egressPolicyStated;
}
/* The restore is started at this zone's load and its promise is handed to the chokepoint, which awaits it in
   `safeFetch`, so every asker waits for the person's table. Two entries reach the network: the engine behind
   astDispatch, and passive learning (handleResponseBody's discovery sweep), which runs from intercept.js at
   `document_start`, earlier than content.js's `document_idle` message; registering at the chokepoint covers
   both by construction. */
DCHECK(typeof self.safeFetchEgressStating === "function",
       "this zone has no `safeFetchEgressStating` to hand its egress-policy read to — ast-worker.html loads " +
       "lib/safe-fetch.js ahead of this file precisely so that the chokepoint exists before the host that " +
       "feeds it, and without the registration every request would race the restore and be refused with a " +
       "row of the person's own control they never ticked");
self.safeFetchEgressStating(egressPolicyReady());
/* The write-back, taken off the table rather than off the message that changed it, so a grant the chokepoint
   refused is never persisted. */
function egressPolicyPersist() {
  /* The whole table, not an origin list: a permission is per signal and per value. `safeFetchEgressTable`
     answers in exactly the shape `safeFetchEgressStated` takes. */
  return frontierPrefPut("exploreOrigins", self.safeFetchEgressTable());
}
/* A frontier entry: { key: address|bundle, sourceUrl, topLevelUrl, origin, responseHeaders, html, code,
   recipes, emit, visits, credentialed, provenance }, plus `shed`/`stranded` for a shed entry. Rehydration
   rebuilds the document from (html, code) and resumes the recipes, so a parked flow on any site can advance
   later even when that page is not open. */
/* The document half of a cold-tier entry, one speller for the store's writer and its reader. Recipes replay
   inside a document, so the entry carries exactly what `qjs_init` takes: its bytes, its URL (DOM §4.5
   Interface Document), the top-level creation URL (HTML §8.1.3.1 Environments), its browser-stated principal,
   and the response header list that HTML §7.1.7 Policy containers makes its policy container (CSP, COOP,
   COEP, and `Origin-Agent-Cluster`). None is derivable from the rest. Nothing is defaulted: `{}` is the header
   list of a response with no headers, and substituting it would turn a CSP-protected page into an unprotected
   one and report exploits that are not real. */
/* The half of an entry that holds whether or not it still has its document: the address the recipes replay
   at, the environment, the principal and the policy. A shed entry is asserted by the same sentences. */
function frontierPlace(e, when) {
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
  /* How the load that stored it got its address: absent (an entry written before the field existed, see
     frontierProvenance) or one of the three words; a fourth would reach `safeFetch` as an invented grade. */
  DCHECK(e.provenance === undefined || e.provenance === PROVENANCE_OBSERVED ||
         e.provenance === PROVENANCE_DERIVED || e.provenance === PROVENANCE_FORCED,
         "a cold-tier entry " + when + " stating the provenance `" + String(e.provenance) + "`, which is none " +
         "of the three — the re-derivation of a shed entry is decided from this word, and one the chokepoint " +
         "does not know would abort the fetch rather than be refused by the policy it names");
  return e;
}
/* The provenance of the load that stored this entry, for the re-fetch that brings its document back.
   An absent field is a positive statement: every entry a live build parks writes it (asserted at frontierPut),
   so a missing one was written by a build with no door onto `safeFetchWiden`, where the chokepoint refused
   every forced load and every entry came from an ambient navigation (`observed`) or a declared route
   (`derived`). The weaker, `derived`, is answered: under-claiming is the allowed direction (solver/engine.h's
   `engine_provenance_of_running_path`), and its only cost is the destructive-path deny list staying armed.
   The grade is never read off the address, since that would let a widening for one origin apply to another. */
function frontierProvenance(e) {
  return e.provenance === undefined ? PROVENANCE_DERIVED : e.provenance;
}
function frontierDoc(e, when) {
  frontierPlace(e, when);
  /* And it still holds its document: a shed entry's bytes are on the network, and the two states are told
     apart by the `shed` field, not by whether a read found bytes. */
  DCHECK(e.shed !== true,
         "a cold-tier entry " + when + " as a document while it is SHED — its bytes were discarded against a " +
         "proved re-fetch and live on the network, so this record must be re-derived before it is read as a " +
         "document rather than parsed as one that is missing");
  DCHECK(e.html instanceof Uint8Array || typeof e.html === "string",
         "a cold-tier entry " + when + " as neither a byte sequence nor characters — the parked document " +
         "would rebuild as a page that parses to nothing, which reads as an origin whose flows found nothing");
  DCHECK(typeof e.code === "string",
         "a cold-tier entry " + when + " with no script inventory — it is the empty string when the document " +
         "carries its own scripts, which is a different thing from absent");
  return e;
}
/* ── Residency: a configured share of this person's device, and the one order ─────────────────────────
   This is not a quota handler: the extension declares `unlimitedStorage` (manifest.json), which exempts the
   extension origin (where SECURITY.md puts every persisted byte) from the origin quota. What remains is a
   preference: how much of this person's device the tool may use, defaulted from the device and adjustable.
   A share is not a cap: it decides how much finished work is stored rather than re-derived, never whether
   work happens. No flow, recipe or exploration is dropped; frontier membership is untouched at every share.
   There is no eviction policy beside the WFQ: the store keeps the document half of the highest-weight entries
   (by `frontierWeight`, the same function `_hostOps.evictee` and admission ask) until the share is spent. The
   store holds no clock, so the order cannot become a recency cap. Nothing polls or sweeps.
   A shed entry keeps recipes, counters, address, principal and policy: it resumes on the next visit as before
   (engineRoot seeds from `prior.recipes`) and cold-rehydrates by fetching its document back, so the
   re-derivability test is a tiebreak on what leaves first, not a gate. */
/* The share: bytes of this profile's disk the frontier's document halves may occupy, defaulted from
   `navigator.deviceMemory` (Device Memory §2 "The deviceMemory attribute", coarse and capped at 8), so a phone and a workstation each get a
   share they can afford. `chrome.system.memory` is not used because it would cost a new permission for a
   default the user can set. An absent `deviceMemory` is answered by the conservative constant. */
const FRONTIER_SHARE_UNKNOWN_DEVICE = 256 * 1024 * 1024;
const FRONTIER_SHARE_PER_DEVICE_GB = 64 * 1024 * 1024;
function frontierDefaultShare() {
  const gb = (typeof navigator !== "undefined") ? navigator.deviceMemory : undefined;
  if (typeof gb !== "number" || !(gb > 0)) return FRONTIER_SHARE_UNKNOWN_DEVICE;
  return Math.round(gb * FRONTIER_SHARE_PER_DEVICE_GB);
}
/* The configured value, held in the `prefs` store (this zone's state is IndexedDB, never
   `chrome.storage.local`), read once into `_frontierShare` and written by the popup's setting. `null` means
   not read yet; zero is a legitimate setting (store no documents, re-derive everything). */
let _frontierShare = null;
async function frontierShare() {
  if (_frontierShare !== null) return _frontierShare;
  const stored = await frontierPref("share");
  /* Validated, not trusted: the popup writes it, and a non-number would make every comparison false, which
     reads as an unlimited share. */
  DCHECK(stored === null || (typeof stored === "number" && Number.isFinite(stored) && stored >= 0),
         "the frontier's stored share is not a byte count (" + String(stored) + ") — every residency " +
         "comparison against it would be false, which is an UNLIMITED store wearing the appearance of a " +
         "configured one, and the user's setting silently doing nothing");
  _frontierShare = (typeof stored === "number" && Number.isFinite(stored) && stored >= 0)
                   ? stored : frontierDefaultShare();
  return _frontierShare;
}
/* The size of an entry's document half, the only part residency can give back. Recipes, counters and address
   stay at every share. Characters count two bytes (the store holds UTF-16); the number exists for the order it
   induces, not as an accounting identity with the disk. */
function frontierDocBytes(e) {
  if (e.shed === true) return 0;
  const h = (e.html instanceof Uint8Array) ? e.html.length : e.html.length * 2;
  return h + e.code.length * 2;
}
/* The tiebreak for which tail entry gives up its document first: a cheap test on the record, not a proof,
   because a wrong answer costs only a re-fetch (a residue whose document does not come back keeps its recipes
   and visit-resume and is counted in `_frontierStats.stranded`). Not re-derivable: a non-http(s) address
   (nothing re-requests it), a principal the address does not state (an opaque or sandboxed document), or a
   caller-assembled script inventory. Those are the only copies and go last, not never. */
function frontierRederivable(e) {
  if (typeof self.safeFetch !== "function") return false;
  let u = null;
  try { u = new URL(e.sourceUrl); } catch (_) { return false; }
  if (u.protocol !== "http:" && u.protocol !== "https:") return false;
  if (e.origin !== u.origin) return false;
  /* A shed entry's inventory went with its document, and it was shed because this test passed. */
  if (e.shed === true) return true;
  return e.code === "";
}
/* The store's decisions, counted where they are made, since a share cannot be observed by its outcome:
   documents shed, re-derived, and stranded (shed and then not fetchable back, the costly direction), plus
   `overShare`, the bytes residency refused to give back because they were the only copy of a residue. */
const _frontierStats = { shed: 0, rederived: 0, stranded: 0, docBytes: 0, overShare: 0 };
/* Residency, restored at the one door that can break it. "The stored document halves fit the share" is an
   invariant of the store and only `frontierPut` can falsify it, so it is re-established there; there is no
   pressure loop or trigger.
   Sort by `frontierWeight`, keep documents from the top while the share lasts, shed the rest; within the tail
   the re-derivable leave first. If what remains over the line is all unrecoverable, residency stops and the
   overage is reported: a disk preference is not worth the only copy of work nobody can recompute. */
async function frontierResidency() {
  const share = await frontierShare();
  const rows = [];
  let total = 0;
  for (const row of (await frontierIndex()).values()) { total += row.bytes; if (row.bytes > 0) rows.push(row); }
  _frontierStats.docBytes = total;
  _frontierStats.overShare = 0;
  if (total <= share) return;
  /* Highest weight first, so the share is spent on what the one order values most. */
  rows.sort((a, b) => frontierWeight(b) - frontierWeight(a));
  /* The resident set is a strict prefix of the order: the first entry that does not fit ends it. A best fit
     would keep a small low-weight document over a large high-weight one, a second policy nobody chose. */
  let kept = 0;
  let cut = rows.length;
  for (let i = 0; i < rows.length; i++) {
    if (kept + rows[i].bytes > share) { cut = i; break; }
    kept += rows[i].bytes;
  }
  const tail = rows.slice(cut);
  /* The tiebreak orders the tail and changes nothing about who is in it. */
  tail.sort((a, b) => (a.rederivable === b.rederivable) ? 0 : (a.rederivable ? -1 : 1));
  for (const row of tail) {
    if (total <= share) break;
    if (!row.rederivable) {
      /* The sort put every re-derivable row first, so the rest of the tail is unrecoverable; stop here. */
      _frontierStats.docBytes = total;
      _frontierStats.overShare = total - share;
      return;
    }
    const e = await frontierGet(row.key);
    DCHECK(e, "the cold tier's ranking view named an entry the store does not hold while residency was being " +
              "restored — this zone is the store's only writer, so a row with no entry is the projection and " +
              "the store having drifted apart, and shedding would be deciding about a residue nobody can read");
    /* `provenance` goes through frontierProvenance, so an entry predating the field leaves stating the word
       its population supports. */
    const shed = { key: e.key, sourceUrl: e.sourceUrl, topLevelUrl: e.topLevelUrl, origin: e.origin,
                   responseHeaders: e.responseHeaders, recipes: e.recipes, emit: e.emit, visits: e.visits,
                   credentialed: e.credentialed, provenance: frontierProvenance(e), shed: true };
    frontierRecord(shed, "was shed to the configured share");
    await frontierWrite(e.key, shed);
    if (_frontierIndexBuilt) _frontierIndex.set(e.key, frontierRow(shed));
    total -= row.bytes;
    _frontierStats.shed++;
  }
  _frontierStats.docBytes = total;
}
/* The one record door, routing on the `shed` field rather than on whether a read found bytes. The document
   half and `shed` are complements, asserted together, so a self-contradicting record crashes here instead of
   parsing to nothing a session later. An absent `shed` means "not shed": entries predating the field hold
   their document. */
function frontierRecord(e, when) {
  frontierPlace(e, when);
  /* The two numbers the Level-1 order is made of, asserted at the door they are written through, not only at
     the read (`frontierWeight`), where any non-negative number passes. A stored `visits` is at least one: an
     entry exists because a run finished. `visits: 0` is legitimate only for a waiting work item
     (`FRONTIER_UNSERVED`), which is never stored. */
  DCHECK(typeof e.emit === "number" && e.emit >= 0 && Number.isFinite(e.emit),
         "a cold-tier entry " + when + " with no emitted-value count (`" + String(e.emit) + "`) — it is the " +
         "reward half of the ONE WFQ order at Level-1, so an entry without it is ranked by its optimism bonus " +
         "alone and a productive parked frontier is admitted as one that has never found anything");
  DCHECK(typeof e.visits === "number" && Number.isInteger(e.visits) && e.visits >= 1,
         "a cold-tier entry " + when + " with no admission count (`" + String(e.visits) + "`) — a stored " +
         "entry exists because a run finished, so its visits are at least one, and this is the divisor that " +
         "amortises the demonstrated surface over the fetches spent on it; without it a document that has " +
         "been re-fetched fifty times ranks exactly like one nobody has ever opened");
  DCHECK((e.html !== undefined) !== (e.shed === true),
         "a cold-tier entry " + when + " claiming `shed=" + String(e.shed) + "` while its document half is " +
         (e.html === undefined ? "absent" : "present") + " — the two are complements, and a record that " +
         "disagrees with itself is read as a document by one consumer and as a re-fetchable residue by another");
  DCHECK(e.stranded !== true || e.shed === true,
         "a cold-tier entry " + when + " marked STRANDED while it still holds its document — stranded means a " +
         "shed entry's proved re-derivation stopped working, and an entry with its bytes has nothing to " +
         "re-derive");
  if (e.shed === true) {
    DCHECK(e.code === undefined,
           "a SHED cold-tier entry " + when + " still carrying a script inventory — the shed discards the " +
           "document HALF, and half a discard leaves the quota it was taken to relieve exactly where it was");
    DCHECK(typeof e.recipes === "string" && e.recipes !== "",
           "a SHED cold-tier entry " + when + " with no recipes — the recipes are the whole of what a shed " +
           "keeps, so shedding an entry down to nothing is the reset this category exists instead of");
    return e;
  }
  return frontierDoc(e, when);
}
/* The cold tier's edges surface failure instead of answering empty, since an empty answer is exactly what a
   first visit looks like and the continuous frontier would silently become per-session. A DCHECK, not a CHECK:
   in release a user whose profile storage is unavailable loses cross-session resume (degraded, not wrong);
   in dev it is this codebase's edge failing. */
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
/* The cold tier's lookup, which answers why a key missed and not only whether: nothing was ever parked for
   this document, something is parked at this address under another bundle id (the key working), or this key
   is stored and the read did not return it (the only defect).
   The key is composed `address + "|" + bundle`, so all entries for one address are a contiguous run of the
   store's key order and a bounded `getAllKeys` finds them without deserializing any record or adding an index.
   Both reads ride one transaction, a consistent snapshot against this zone's writes, so "get absent, scan
   present" is the store contradicting itself, never an interleaving, which is what entitles the DCHECK.
   A `keys` of null is the edge failing, not an empty store. */
function frontierAddressRange(sourceUrl) {
  return IDBKeyRange.bound(sourceUrl + "|", sourceUrl + "|￿");
}
/* The address half of a stored key. The bundle id is base-36, so it cannot contain `|` and the last `|` is the
   joint; a stored address that itself ends in `|<x>` sorts inside the range and is rejected here. */
function frontierKeyAddress(key) {
  const k = String(key);
  const i = k.lastIndexOf("|");
  return i < 0 ? null : k.slice(0, i);
}
async function frontierLookup(key, sourceUrl) {
  try {
    const db = await idbOpen();
    return await new Promise((res, rej) => {
      const tx = db.transaction("frontier");
      const s = tx.objectStore("frontier");
      const g = s.get(key);
      const a = s.getAllKeys(frontierAddressRange(sourceUrl));
      let entry = null; let keys = [];
      g.onsuccess = () => { entry = g.result || null; };
      a.onsuccess = () => { keys = a.result || []; };
      /* Settled on the transaction, so both halves are delivered from one snapshot. */
      tx.oncomplete = () => res({ entry: entry, keys: keys.filter((k) => frontierKeyAddress(k) === sourceUrl) });
      tx.onerror = () => rej(tx.error || g.error || a.error);
      tx.onabort = () => rej(tx.error || g.error || a.error);
    });
  } catch (e) { RETHROW_FATAL(e); frontierFail("read", e); return { entry: null, keys: null }; }
}
/* What the lookup met, as one word from a closed set; each is a positive statement:
     `not-asked`     — this document does not persist a residue, so the store was not asked.
     `unreadable`    — the store's read edge failed (frontierFail aborts in dev); not "never visited".
     `unvisited`     — no entry for this address under any bundle id: a first visit.
     `other-bundle`  — entries for this address, none under this bundle id: the key doing its job. It names the
                       observation; a redeploy is the usual cause but is an inference.
     `unread`        — this key is stored and the get did not answer it; the only defect of the six.
     `hit`           — the key answered and the session resumes from it.
   `others` is the evidence beside the word, not entailed by it (a hit can have older deploys' residues
   parked beside it). It is `null` on the two arms that counted nothing. */
const COLD_LOOKUP = ["hit", "unvisited", "other-bundle", "unread", "not-asked", "unreadable"];
function coldLookupOf(fkey, look) {
  if (look === null) return { state: "not-asked", others: null };
  if (look.keys === null) return { state: "unreadable", others: null };
  const held = look.keys.indexOf(fkey) >= 0;
  const others = look.keys.length - (held ? 1 : 0);
  /* A record came back for this key, so the scan of its address must name it; otherwise the range or separator
     split the key differently from engineRoot and `others` counts the wrong address. */
  DCHECK(!look.entry || held,
         "the cold tier answered the frontier key `" + fkey + "` with a record while a key scan of that " +
         "document's own address did not name it — the two halves are one read transaction, so this is the " +
         "address range or the separator split disagreeing with the key engineRoot composed, and the " +
         "sibling-entry count every other arm reports would be a count over a different address");
  if (look.entry) return { state: "hit", others: others };
  if (held) {
    /* Reading (c): both halves are this zone's records under its keys in one snapshot, so this is the store
       contradicting itself. Release returns the word, leaving the state a miss leaves (`prior` null, the engine
       seeds a boot flow), so the run still says which of the six it met. */
    DFAIL("the cross-session frontier enumerates the key `" + fkey + "` for this document's address and " +
          "answered a read of that same key, in the same transaction, with nothing — this zone is the store's " +
          "only writer and both halves are one snapshot, so a parked residue is being kept out of the session " +
          "it belongs to and every visit to this document will re-explore from a boot flow for ever");
    return { state: "unread", others: others };
  }
  return { state: others > 0 ? "other-bundle" : "unvisited", others: others };
}
/* The one write. It rejects on failure like every edge here; there is no quota arm (`unlimitedStorage`), and
   residency is asked by frontierPut after the write. The transaction's abort is listened for as well as the
   request's error, since a write can fail on either and a promise settling only on `t.onerror` would hang. */
function frontierWrite(key, entry) {
  return new Promise((res, rej) => {
    idbOpen().then((db) => {
      const tx = db.transaction("frontier", "readwrite");
      const s = tx.objectStore("frontier");
      const t = (entry && entry.recipes) ? s.put(entry, key) : s.delete(key);
      t.onsuccess = () => res();
      t.onerror = () => rej(t.error || tx.error);
      tx.onabort = () => rej(tx.error || t.error);
    }, rej);
  });
}
async function frontierPut(key, entry) {
  if (entry && entry.recipes) frontierRecord(entry, "was written");   // before the edge: the grammar, not the storage
  try {
    await frontierWrite(key, entry);
    /* The ranking view moves with the store, on the one door that writes it, once the write landed; an unbuilt
       index is skipped because it will be built from the store, which then contains this. */
    if (_frontierIndexBuilt) { if (entry && entry.recipes) _frontierIndex.set(key, frontierRow(entry)); else _frontierIndex.delete(key); }
    /* Residency is re-established after the ranking view moved, so it asks the order the store is in. A store
       inside its share does one comparison. */
    await frontierResidency();
  } catch (e) { RETHROW_FATAL(e); frontierFail("write", e); }
}
async function frontierAll() {
  try {
    const db = await idbOpen();
    return await new Promise((res, rej) => { const t = db.transaction("frontier").objectStore("frontier").getAll(); t.onsuccess = () => res(t.result || []); t.onerror = () => rej(t.error); });
  } catch (e) { RETHROW_FATAL(e); frontierFail("scan", e); return []; }
}
/* The Level-1 weight of a work item that is not resident (a parked frontier, or a document waiting for an
   instance). It shares the engine's WFQ policy (value plus an exploration bonus, never drop work) in the
   engine's own currency: solver/flow.c gives a never-run flow its reward plus 1.0, and an unserved item here
   is 0 + 1/(0+1) = 1.0, so the two levels form one order. There is no CPU aging, since nothing is running.
   `emit` is the last run's demonstrated surface (one emission one point, as flow_credit_emit counts), so
   `emit / visits` is the mean of new findings per admission and an unproductive document sinks. A zero-visit
   row states "never served at this level", which is what a waiting document is. */
const FRONTIER_UNSERVED = { emit: 0, visits: 0 };
function frontierWeight(row) {
  DCHECK(row && typeof row.emit === "number" && row.emit >= 0 && row.emit === row.emit,
         "a Level-1 work item was ranked with no emission count — `emit` is written by every frontierPut and " +
         "is the reward half of the one WFQ order, so an absent one would rank this item by its optimism " +
         "bonus alone and report a productive parked frontier as one that has never found anything");
  DCHECK(typeof row.visits === "number" && row.visits >= 0 && Number.isInteger(row.visits),
         "a Level-1 work item was ranked with no service count — `visits` is both the divisor of the reward " +
         "and the decay of the optimism bonus, so an absent one makes an item that has been rehydrated a " +
         "hundred times indistinguishable from one that has never been tried");
  DCHECK(row.visits > 0 || row.emit === 0,
         "a work item that has never been served carries emissions — nothing can have emitted before it ran, " +
         "so the rate below would be a total masquerading as a per-visit expectation");
  return frontierReward(row) + 1 / (row.visits + 1);     // reward + optimism; no aging (nothing is burning CPU)
}
/* The reward term alone, named so the Level-1 census can report which half of the weight ordered the pick. A
   spread over the sum cannot tell a rank frozen at a constant from the legitimate tie of an empty store, where
   every candidate is unserved at 1.0. One definition, read by the weight and by the census. */
function frontierReward(row) {
  return row.visits ? row.emit / row.visits : 0;         // expected emit per admission — future productivity
}
/* The cold tier's ranking view, a projection of the store. The Level-1 order asks two numbers of a parked
   frontier on every scheduler round, and `getAll` would deserialize every parked document to answer them.
   This zone is the store's only writer and frontierPut updates the view on its way through, so a row and an
   entry cannot disagree; it is built once per zone lifetime from the store. */
const _frontierIndex = new Map();   // key -> { key, sourceUrl, emit, visits }
let _frontierIndexBuilt = false;
function frontierRow(e) {
  DCHECK(typeof e.key === "string" && e.key !== "",
         "a cold-tier entry carries no key — it is the name the pool holds this item under while it ranks it, " +
         "and an entry the ranking cannot name is one it can neither admit nor exclude from being admitted twice");
  DCHECK(typeof e.sourceUrl === "string" && e.sourceUrl !== "",
         "a cold-tier entry carries no document address — the pool excludes a parked item whose document a " +
         "LIVE tab already holds, and an item with no address is one it would rehydrate into a second " +
         "instance beside the tab that is already running it");
  /* The state bits the order reads, decided here as strict booleans (frontierRecord asserted what their absence
     means): `shed` has given its document up, `stranded` failed to re-derive it and is no longer a cold
     candidate (only a visit resumes it). The two residency numbers are computed here too, so residency never
     opens an entry to learn its size. */
  return { key: e.key, sourceUrl: e.sourceUrl, emit: e.emit, visits: e.visits,
           shed: e.shed === true, stranded: e.stranded === true,
           bytes: frontierDocBytes(e), rederivable: frontierRederivable(e) };
}
async function frontierIndex() {
  if (_frontierIndexBuilt) return _frontierIndex;
  for (const e of await frontierAll()) {
    /* The store's invariant: frontierPut deletes an entry with no recipes, so every stored row has them. */
    DCHECK(e && typeof e.recipes === "string" && e.recipes !== "",
           "the cross-session frontier holds an entry with no parked recipes — frontierPut deletes rather than " +
           "storing one, so this is a residue whose flows were dropped between the park and the store");
    /* The record grammar on the way out, where an entry from an older build or a half-landed shed is met. */
    frontierRecord(e, "came back from the store");
    _frontierIndex.set(e.key, frontierRow(e));
  }
  _frontierIndexBuilt = true;
  return _frontierIndex;
}
/* The re-derivation of a shed entry's document: the same chokepoint, principal, address and session as the
   request the shed was judged against. It answers the document half, or null.
   It asks `navigationCarriesSession` like every document load in this file, so a document loads with the
   same credentials live and from the cold tier; the principal is the one parked with the recipe, so an empty
   one re-derives uncredentialed. `landed === e.sourceUrl` proves the bytes are this document.
   The headers that come back are used, not the parked ones: HTML §7.1.7 Policy containers builds the policy
   container from the response the document came from. A changed document is safe because engineRoot
   re-derives the key from what the engine parsed, so a redeployed bundle boots fresh.
   A failure is not a @WHY (the world may retire a route): it is recorded on the entry, which stops being a
   cold candidate, and in `_frontierStats.stranded`. The recipes are untouched, so the next visit resumes. */
async function frontierRederive(e) {
  DCHECK(e.shed === true,
         "a cold-tier entry that still holds its document was sent to be re-derived — the fetch would replace " +
         "bytes this store already has, and the round would pay a network round trip to learn nothing");
  let r = null;
  if (frontierRederivable(e)) {
    /* A document: Fetch §2.2.5 Requests' `document` destination ("HTML's navigate algorithm (top-level
       only)"), parsed by the engine's own parser. */
    /* `provenance` and `docReach` are the entry's own parked word, since a re-derivation is the same load that
       stored it; a different word would re-fetch under a permission nobody granted. `pinned` is `unstated`:
       the store parks no witness mark, and `unpinned` would be a false claim for a forced address whose flow
       pinned a witness. The mark only refines a refusal's sentence (`_firingRefusal` chooses between `forced`
       and `forced-witness`), never the decision. */
    /* `credentials: "include"` is the navigation's own credentials mode (HTML §7.4.5 Populating a session
       history entry: "destination `document` … credentials mode `include`"). `credentialed` is this zone's
       willingness to spend the session; safe-fetch.js's `_credentialedOf` composes the two and requires a mode
       wherever the session pays. */
    /* `actor: "tool"`: no page asked for this load; the frontier did. It changes no outcome here, since the
       witness mark is `unstated`. */
    try { r = await self.safeFetch(e.sourceUrl, { pageUrl: e.sourceUrl, pageOrigin: e.origin,
                                                  destination: "document", provenance: frontierProvenance(e),
                                                  docReach: frontierProvenance(e),
                                                  actor: "tool",
                                                  pinned: "unstated",
                                                  credentials: "include",
                                                  credentialed: navigationCarriesSession(e.sourceUrl, e.origin) }); }
    catch (err) { RETHROW_FATAL(err); r = null; }
  }
  const landed = r && Array.isArray(r.urlList) && r.urlList.length ? r.urlList[r.urlList.length - 1] : null;
  if (r && r.ok && landed === e.sourceUrl && r.body instanceof Uint8Array && r.headers) {
    _frontierStats.rederived++;
    return { bytes: r.body, headers: r.headers };
  }
  const strandedEntry = { key: e.key, sourceUrl: e.sourceUrl, topLevelUrl: e.topLevelUrl, origin: e.origin,
                          responseHeaders: e.responseHeaders, recipes: e.recipes, emit: e.emit,
                          visits: e.visits, credentialed: e.credentialed, provenance: frontierProvenance(e),
                          shed: true, stranded: true };
  frontierRecord(strandedEntry, "was stranded");
  await frontierWrite(e.key, strandedEntry);
  if (_frontierIndexBuilt) _frontierIndex.set(e.key, frontierRow(strandedEntry));
  _frontierStats.stranded++;
  return null;
}
/* ────────────────────────────────────────────────────────────────────────────────────────────────────
   Host-level WFQ (Level 1 of the one attention): interleave the live document engines by value of
   information, in slices, so no document or deep path monopolizes CPU. Each agent cluster is one wasm
   instance (SECURITY.md: one per (browsing-context group, origin)). The engine exposes its best flow's weight
   (qjs_top_weight) and yields hot after a slice (qjs_step -> 2); the host ranks all live engines by that
   weight, advances the winner one slice and re-ranks, the same WFQ the engine runs over flows within a
   document. The ranking does not ask: the weight and the working set are recorded by engineRecordFacts at the
   end of every round, since an instance behind the frame boundary answers only by message.
   RAM is the floor: under HOT_RAM_BUDGET of summed working set new engines are admitted; at the floor
   `_hostOps.evictee` picks a resident engine to park (qjs_request_park, recipes to IndexedDB), and the cold
   tail is rehydrated into this same pool by admission. An engine awaiting a reply is `fetching` and skipped
   until its body lands, so a slow fetch on one document never stalls another.
   ──────────────────────────────────────────────────────────────────────────────────────────────────── */
// The hot working set is bounded by actual RAM, not an instance count (a light page's instance is a few MB, a
// heavy bundle's tens). Over the budget new documents wait as cold recipes and are pulled back by admission.
// This is a RAM floor, not a truncating bound; at least one document is always admitted.
const HOT_RAM_BUDGET = 512 * 1024 * 1024;   // bytes of summed live WASM memory before new engines wait
/* The hot working set, summed over what each instance last reported (engineRecordFacts writes it every round,
   the first time before the record reaches the pool), so an absent figure is a producer that stopped writing
   and crashes rather than counting as zero.
   `booting` reservations and `loading` seats (an admission whose document is still on the network: a declared
   route's navigation, HTML §7.4 Navigation and session history, or a shed residue's re-derivation) have
   reported nothing and are skipped; there
   is no honest figure for them, so the sum is a lower bound while one stands, and `_admissionHasHeadroom`
   counts them by state instead. Every other state must be classified here before it can be summed. */
function _residentBytes() {
  let b = 0;
  for (const e of _pool) {
    if (e.state === "booting" || e.state === "loading") continue;   // has reported nothing; _admissionHasHeadroom reads the state instead
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
/* The reservations in flight: the positive form of "that sum is not complete yet". */
function _bootingCount() {
  let n = 0;
  for (const e of _pool) if (e.state === "booting") n++;
  return n;
}
/* The admissions whose document is still arriving, a separate fact from reservations: `_bootingCount`'s other
   readers (engineReserve's `peakBooting`, the Level-1 census's `booting` row and its `<= pool` assert) mean
   reservations by name. The two are disjoint subsets of the pool, which the census asserts. */
function _loadingCount() {
  let n = 0;
  for (const e of _pool) if (e.state === "loading") n++;
  return n;
}
/* Whether another instance may be built, asked in one place. A reservation or a loading seat blocks admission
   because the working set is then a lower bound, and admitting against it spends memory already spoken for.
   It is not a bound: provisioning and loads settle, hostSchedule's wait arm waits on the promise that clears
   it, the next round re-asks, and hot engines keep being stepped meanwhile.
   Named residual: a `loading` seat holds admission for as long as its load takes, so a body a remote party
   never ends closes admission for the session. Next diff builds a seat that does not hold admission: a cold
   seat carries its frontier key so `_bestCandidate` excludes its row, a seed seat carries its address into the
   walk's `live` set, and hostSchedule's wait arm becomes releasable by an arrival. Absence shows as
   rendererPoolProbe reporting `loadingSeats` of 1 or more with `waiting` climbing and `reservations.seeded`
   and `rehydrated` frozen, while the Level-1 census is still written every round with `loading` of 1 or
   more, no `cands` and `atFloor: 0`. */
function _admissionHasHeadroom() {
  if (_pool.length === 0) return true;     // always admit >= 1 so a lone document runs
  if (_bootingCount() > 0) return false;   // an instance that has not reported is a term the sum is missing
  if (_loadingCount() > 0) return false;   // a document already fetched FOR is memory this sum has not counted
  return !_atRamFloor();
}
/* The RAM floor, a different fact from whether another instance may be built: a reservation blocks admission
   but is no reason to evict. A wasm Memory never shrinks, so the floor once reached is permanent for the
   instances that reached it; it is answered by `_hostOps.evictee`, which parks one resident engine to the
   cold tier, not by refusing admission. */
function _atRamFloor() {
  return _residentBytes() >= HOT_RAM_BUDGET;
}
/* The one Level-1 candidate order: the highest-value work item with no instance, whatever its kind. A waiting
   document and a parked frontier compete in the same order (cold-tail resume is the same admission step), so
   a residue parked long ago competes on value while browsing continues.
   A waiting document ranks as unserved (weight 1.0): its key's bundle half comes from the engine's scan, so
   the host cannot yet know which residue it will resume and attributes none. engineRoot's frontierGet picks
   the residue up once the engine answers. An address a live document holds is not also a cold candidate,
   since the tab's engine resumes that residue itself; the exclusion is by address because the live half's key
   is not yet known, and exact because the address is in the key. */
/* The kinds of work item with no instance, declared once so that the walk below, `_candRanked`'s counting,
   `_level1Record`'s accounting assert and admission's dispatch on `kind` cannot disagree about the set. `kind`
   is the pick's tag, `pop` the population row, `wMax` its extremum (names already on the wire to the popup).
   `from` names the population member the kind's walk reads, so a caller that omits it fails rather than
   reporting a zero population; `shape` is that member's container, checked by shape and never by
   `instanceof`, because a driver may hand populations minted in its own realm. */
const CAND_KINDS = [
  // a document waiting for an instance
  { kind: "doc",  pop: "candDocs",  wMax: "candDocWMax",  from: "waiting", shape: "list" },
  // an address an application declared is a page of itself
  { kind: "seed", pop: "candSeeds", wMax: "candSeedWMax", from: "seeds",   shape: "map" },
  // a parked frontier in the cold tier
  { kind: "cold", pop: "candCold",  wMax: "candColdWMax", from: "idx",     shape: "map" },
];
/* The two container shapes a population member can be, written as what the walk calls on it. `map` requires
   `get`/`keys`/`values`/`size` together, since an array answers `keys()` with indices. */
const CAND_SHAPES = {
  list: (v) => Array.isArray(v),
  map:  (v) => v !== null && typeof v === "object" && typeof v.get === "function" &&
               typeof v.keys === "function" && typeof v.values === "function" && typeof v.size === "number",
};
/* The spread rows, readings over the ranked set as a whole, declared once because they are attached at the
   member that earns them and asserted at the record. A spread over the weight can show that every candidate
   ties but not why: an empty store ties every unserved candidate at 1.0 correctly, while an address admitted n
   times still at 1.0 is a frozen rank. `candVisMax` and `candUnserved` separate the two, and
   `candRewardMax`/`candRewardMin` show whether the reward term orders anything. Each row exists only over a
   non-empty ranked population; `candUnserved` is a count and lives with the populations.
   Each entry writes its row with a literal key, so engine/fieldgate.mjs and grep can see the writer; the
   DCHECK after the fold stops an entry naming one row and writing another. */
const CAND_SPREAD = [
  { row: "candWMax",
    fold: (c, t) => { if (!("candWMax" in c) || t.w > c.candWMax) c.candWMax = t.w; } },
  { row: "candWMin",
    fold: (c, t) => { if (!("candWMin" in c) || t.w < c.candWMin) c.candWMin = t.w; } },
  { row: "candRewardMax",
    fold: (c, t) => { if (!("candRewardMax" in c) || t.reward > c.candRewardMax) c.candRewardMax = t.reward; } },
  { row: "candRewardMin",
    fold: (c, t) => { if (!("candRewardMin" in c) || t.reward < c.candRewardMin) c.candRewardMin = t.reward; } },
  { row: "candVisMax",
    fold: (c, t) => { if (!("candVisMax" in c) || t.visits > c.candVisMax) c.candVisMax = t.visits; } },
];
/* The zeroed reading: the one statement of which rows a candidate walk produces. Counts start at 0 because a
   zero count is a reading; spread rows are absent because an extremum over nothing is not a number, and
   `_candRanked` attaches each on the first member. */
function _candCensus() {
  const cen = { cands: 0, candUnserved: 0, exclSub: 0, exclSeedLive: 0, exclSeedParked: 0,
                exclLive: 0, exclHeld: 0, exclStranded: 0 };
  for (const k of CAND_KINDS) cen[k.pop] = 0;
  return cen;
}
/* The one site that ranks a candidate: it takes the row and returns the weight, so a kind's total, population,
   extrema and the weight the pick compares cannot part, and the census reports terms of the number the pick
   used. Spread rows appear on the first member, enforcing the record's presence rule where numbers are made. */
function _candRanked(cen, kind, row) {
  const d = CAND_KINDS.find((k) => k.kind === kind);
  DCHECK(d !== undefined,
         "the Level-1 candidate walk ranked a work item of a kind that is not declared (`" + kind + "`) — " +
         "CAND_KINDS is what the record's accounting, the popup's columns and the admission's dispatch are " +
         "all read from, so a kind ranked outside it is a work item that would be counted into the total, " +
         "accounted for by no population, and built by whichever admission arm happened to be the fallback");
  const t = { w: frontierWeight(row), reward: frontierReward(row), visits: row.visits };
  DCHECK(Number.isFinite(t.w) && Number.isFinite(t.reward),
         "the Level-1 candidate walk ranked a work item at a non-finite weight or reward — both are " +
         "`frontierWeight`'s own terms over an emission count and a visit count, so a non-finite one is that " +
         "weight's invariants having been bypassed, and it would win this order against every real number");
  cen[d.pop]++;
  cen.cands++;
  if (t.visits === 0) cen.candUnserved++;
  if (!(d.wMax in cen) || t.w > cen[d.wMax]) cen[d.wMax] = t.w;
  for (const s of CAND_SPREAD) {
    s.fold(cen, t);
    DCHECK(s.row in cen,
           "a Level-1 spread row (`" + s.row + "`) was declared and its own fold did not write it — the " +
           "declaration is what `_level1Record` asserts the record's presence rule against and what says a " +
           "reading was taken over the ranked set, so an entry naming one row and writing another leaves the " +
           "named row absent over a non-empty population, which this census reads as `nothing was ranked`");
  }
  return t.w;
}
/* The population the order is taken over, composed in one place and handed in, so the Level-1 order (which
   exists only in this zone) can be asked over a population a driver supplies. It is a composer, not accessors:
   what is under test is the walk, so the privates stay private, and both production call sites take the same
   population. */
function _candPopulation(idx) {
  return { resident: _pool, waiting: _waiting, seeds: _seeds, idx: idx };
}
/* The population's contract, checked against the kind table before the walk: a member the walk is not handed
   is indistinguishable downstream from an empty one (see CAND_KINDS' `from`). */
function _candPopulationCheck(pop) {
  DCHECK(pop !== null && typeof pop === "object",
         "the Level-1 candidate order was asked over no population at all — it ranks the work items that have " +
         "no instance, and the set of them is an input rather than something this walk knows");
  /* The resident set is not a kind: it is what every kind is excluded by, so a missing one would widen the
     order (every excluded item becomes a candidate) rather than empty it. */
  DCHECK(CAND_SHAPES.list(pop.resident),
         "the Level-1 candidate order was asked with no resident set — it is what a candidate is excluded BY, " +
         "so without it a parked frontier whose page a tab already holds is rehydrated beside that tab and " +
         "`exclLive` reports that nothing left the order");
  for (const k of CAND_KINDS)
    DCHECK(CAND_SHAPES[k.shape](pop[k.from]),
           "the Level-1 candidate order was asked without the `" + k.from + "` half of its population, or " +
           "with one that is not a " + k.shape + " — the `" + k.kind + "` arm walks it, so what reaches the " +
           "census is `" + k.pop + ": 0`, which is a READING (nothing of this kind was waiting) and not the " +
           "absence it actually is: a whole kind of work item leaves the order and the record agrees with " +
           "itself about a walk that never looked");
}
/* The pick is synchronous, which the index makes possible: a cluster can be rooted by a concurrent service
   round (hostNotice's create arm), so a pick acted on after a suspension could reach engineCreate's
   one-instance-per-cluster assert with a cluster that just acquired one. The caller awaits frontierIndex() once
   and hands the map in. */
/* It answers the pick and the reading together, because a `max` cannot say what it was taken over: a frozen
   rank or a row dropped from the order is visible only in the spread (`candWMax` against `candWMin`) and the
   exclusions (`exclLive`). Counts are present whenever the walk ran; weights only over a non-empty population,
   the same rule solver/result.c's `_wfq` states for `members`. */
function _bestCandidate(pop) {
  _candPopulationCheck(pop);
  const { resident, waiting, seeds, idx } = pop;
  const live = new Set();
  for (const e of resident) if (e.msg && e.msg.sourceUrl) live.add(e.msg.sourceUrl);
  /* What the store already knows about the waiting addresses, which is the rank a waiting document is
     entitled to. The key's bundle half is the engine's, so the host cannot know which residue a visit resumes,
     but every row carries its `sourceUrl`: "this address was admitted n times and demonstrates F findings" is a
     fact about the address being admitted. This is also where a document request's cost rides the order:
     `visits` counts the fetches spent at the address and divides the reward, so a document cycle (A names B,
     B names A) sinks instead of re-entering at 1.0 for ever. Nothing refuses a second fetch; the order only
     prefers other work first. One pass over the index, only for the addresses in question. */
  const waitingAddrs = new Set();
  for (const job of waiting)
    if (!(job.msg.frameId && _isRealOrigin(job.msg.origin))) waitingAddrs.add(job.msg.sourceUrl);
  /* Declared routes ask the same index whether a parked frontier already exists at their address; one that
     does is already a work item (its admission fetches the document back), so the seed walk leaves it there. */
  for (const addr of seeds.keys()) waitingAddrs.add(addr);
  const byAddress = new Map();
  if (waitingAddrs.size) for (const row of idx.values()) {
    if (!waitingAddrs.has(row.sourceUrl)) continue;
    const a = byAddress.get(row.sourceUrl);
    /* Summed across the bundles served at one address: a pooled mean, "what one admission of this address has
       been worth", weighted by the admissions each bundle received. */
    if (a) { a.emit += row.emit; a.visits += row.visits; }
    else byAddress.set(row.sourceUrl, { emit: row.emit, visits: row.visits });
  }
  let best = null;
  /* The reading is accumulated by the walk that picks: each `w` is counted once where it is computed. */
  const cen = _candCensus();
  for (const job of waiting) {
    /* A sub-frame never roots a cluster (its embedder names it, see admit), so it is not a candidate; it keeps
       its place in `_waiting`. It is counted, so "every waiting document is a sub-frame" is told from "the
       order found nothing". */
    if (job.msg.frameId && _isRealOrigin(job.msg.origin)) { live.add(job.msg.sourceUrl); cen.exclSub++; continue; }
    live.add(job.msg.sourceUrl);
    /* An address with no rows has never been served, which is what `FRONTIER_UNSERVED` states. */
    const known = byAddress.get(job.msg.sourceUrl);
    const w = _candRanked(cen, "doc", known !== undefined ? known : FRONTIER_UNSERVED);
    if (!best || w > best.w) best = { kind: "doc", job, w };
  }
  /* Declared routes: the third kind, ranked by the same weight over the same index; its cost is one fetch,
     priced by the weight's `visits` divisor. An address a live or waiting document holds leaves the order
     (that document is its exploration) and is counted, so "no route declared" and "every route already being
     explored" differ. Nothing is dropped: the entry stays in `_seeds`. */
  for (const addr of seeds.keys()) {
    if (live.has(addr)) { cen.exclSeedLive++; continue; }
    /* An address with a parked frontier leaves too: that residue's admission is a visit to it, and a seed
       beside it would fetch one address twice for one exploration. Nothing is refused; when the residue
       drains (frontierPut deletes an entry with empty recipes) the address is admitted again. */
    if (byAddress.get(addr) !== undefined) { cen.exclSeedParked++; continue; }
    /* What is left has never been served: no live holder, no waiting document, no stored row. */
    const w = _candRanked(cen, "seed", FRONTIER_UNSERVED);
    if (!best || w > best.w) best = { kind: "seed", addr, w };
  }
  for (const row of idx.values()) {
    if (live.has(row.sourceUrl)) {
      /* The exclusion moves the row's weight, it does not delete it: a parked entry and the tab holding its
         address are one work item (the tab's engine resumes the residue), so a waiting document at that address
         must have been ranked by the row's history. A row excluded because a live instance holds its address is
         carried by that engine's `engineWeight` instead. */
      DCHECK(!waitingAddrs.has(row.sourceUrl) || byAddress.has(row.sourceUrl),
             "a parked residue was taken out of the Level-1 order because a waiting document holds its " +
             "address, and that document was not ranked by it — the row's weight has left the order with " +
             "nothing carrying it, so a productive frontier is admitted as if it had never found anything");
      cen.exclLive++;
      continue;
    }
    if (resident.some((p) => p.fkey === row.key)) { cen.exclHeld++; continue; }
    /* A stranded residue has no bytes to build an instance over, so it is not a candidate; its recipes remain
       and the next visit resumes them through engineRoot's frontierGet. */
    if (row.stranded) { cen.exclStranded++; continue; }
    const w = _candRanked(cen, "cold", row);
    if (!best || w > best.w) best = { kind: "cold", row, w };
  }
  /* `candDocWMax`, `candSeedWMax` and `candColdWMax` stay apart because the Level-1 question is which kind is
     worth the next instance, and one merged extremum erases that comparison. */
  /* A walk that ranked members and picked nothing would be the order silently declining an admissible item. */
  DCHECK((cen.cands > 0) === (best !== null),
         "the Level-1 candidate order ranked " + cen.cands + " item(s) and picked " +
         (best ? "one" : "none") + " — the census and the pick are produced by one walk over one set, so a " +
         "disagreement is an arm that counted a candidate without offering it (or offered one without " +
         "counting it), and either way the order this zone reports is not the order it took");
  return { best: best, census: cen };
}

/* The navigation response's header list in the form that crosses the ABI: HTTP field lines `name: value`, one
   per line. A relay, not logic: every decision made from it (policy container, sandboxing flags, agent
   cluster) is the engine's. Nothing is defaulted; every producer reaching engineCreate writes `h`
   (navigationLoad's reply, a child notice's `{}`, a cold entry's stored list), and `{}` already states "no
   headers". A value with CR or LF did not come off a response (`Headers` forbids both) and is asserted here,
   where the header's name survives. */
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

/* ─── Does this document load carry the person's session? ────────────────────────────────────────────
   One answer, read by every load in this file. A real navigable sends cookies: a same-origin navigation
   carries them exactly as a browser's does, so the engine is served what the person is served.
   The condition is Fetch's: a response is readable as "basic" when the request's current URL's origin is same
   origin with the request's origin (Fetch §4.1 Main fetch); otherwise a credentialed read needs §4.10 CORS
   check, which no document server grants. Asking cross-origin with cookies would spend the session for bytes
   the chokepoint must refuse, so this declines to make that request; policy remains safe-fetch.js's.
   The principal is the browser's, never re-derived from the address: `_isRealOrigin` (safe-fetch.js's own
   predicate, shared) makes an opaque principal's load uncredentialed, and an empty principal is a document
   with no session to carry.
   Named residual: a cross-origin `<iframe src>` navigation carries no cookies here, though a browser sends
   them. Next diff builds a document load type in safe-fetch.js whose read principal is the response's own
   origin (the bytes go to a different instance, not the initiator). Absence shows as cross-origin child
   documents loading in their logged-out form. */
function navigationCarriesSession(absUrl, principalOrigin) {
  DCHECK(typeof principalOrigin === "string",
         "a document load was asked whether it carries the session with no principal stated at all — every " +
         "producer writes a string (the browser's MessageSender.origin for a live document, the origin of " +
         "the URL this zone fetched for a peer's child, the parked one for a cold recipe), so `undefined` " +
         "here is a producer that stopped writing the field rather than a document with no session");
  return _isRealOrigin(principalOrigin) && originOf(absUrl) === principalOrigin;
}

/* ─── The one document-load path ────────────────────────────────────────────────────────────────────
   HTML §7.4 Navigation and session history's load, as this host performs it: an address goes in and a §7.4.5
   Populating a session history entry response comes out (the bytes a Document is parsed from, the header list
   its policy container is built from, the URL its origin is determined over). Every document the engine holds
   arrives here, through lib/safe-fetch.js, so scheme allowlist, SSRF/PNA guard on the initial and
   post-redirect URL, CORB and the destructive-path deny list apply to every navigation. It carries the
   session where a browser's navigation would (see navigationCarriesSession).
   Two paired outcomes, never defaulted: `bytes` is a byte sequence and `unavailable` null, or `bytes` is null
   and `unavailable` names why in the closed vocabulary the popup renders (status/empty/network). `bytes: null`
   is a load that did not load: the navigable exists and shows an error page. A third outcome, `declined`, is
   below. The chokepoint's refusals arrive as `kind: "network"` with the rule in `statusText`
   (`blocked-signal:`, `blocked-scheme:`, `blocked-private-from-public`).
   Emptiness is not judged here: an OK zero-length body is an ordinary empty Document; refusing one is a seed's
   rule, stated at the seed. */
/* `fromReach` is how the document issuing this navigation was itself reached, distinct from `provenance` (the
   engine's word about this navigation act); safe-fetch.js reads both. It is trailing, and an omitted one is
   `undefined`, which `_docReachOf` refuses with a fatal CHECK. The produced document's reach is the join of
   the two (`safeFetchReachJoin`), composed by whoever states that document's analyze record. */
/* What a caller of a §7.4 load does with a refusal, as a declared word rather than a boolean, because "holds a
   rendezvous" and "can carry a refusal" differ at the route seed:
     `rendezvous` — the engine is parked on a request id; the refusal is routed to it (engineDeliverDocument →
                    HostDecline → engine_host_decline) and the navigable keeps the pre-operation document HTML
                    §7.3.1.3 Child navigables' create-a-new-child-navigable created it holding.
     `report`     — nothing is provisioned or parked; the caller drops its work item and reports the
                    chokepoint's reason verbatim (a route seed, via admitSeatLand's seat-drop arm).
     `none`       — the caller has no arm that can state a load that was never attempted, so a decline aborts
                    and the message names what each such caller would fabricate.
   The test is written in the positive (`!== "rendezvous" && !== "report"`), so an unstated or misspelled word
   refuses by construction. */
const DOC_REFUSAL_ARMS = Object.freeze(["rendezvous", "report", "none"]);
async function navigationLoad(u, base, principalUrl, principalOrigin, provenance, fromReach, refusalArm,
                              actor) {
  /* `refusalArm` is required; an unstated word takes the refusing arm, exactly as `none` does. */
  DCHECK(DOC_REFUSAL_ARMS.indexOf(refusalArm) >= 0,
         "a §7.4 navigation was loaded stating the refusal arm " + JSON.stringify(refusalArm) + ", which " +
         "is none of the three this zone declares (" + DOC_REFUSAL_ARMS.join("/") + ") — the word says what " +
         "this caller DOES with a refusal, and a caller that does not state it would have this function pick " +
         "for it: a rendezvous it does not hold, a report it cannot write, or an empty document for an " +
         "address nothing ever fetched");
  /* `actor` is whose act this navigation is, stated by each caller: a child navigable and a declared route are
     the page's, an ambient seed this tool composed is the tool's (safe-fetch.js's `_actorOf`: a request this
     tool composed at a person's direction is still `tool`). It is the signal the egress registry grades
     `certain`, shown on the person's own control, so it must be true. The value space is asked of the
     chokepoint and never copied here. */
  DCHECK(typeof self.safeFetchSignalUsable === "function" &&
         self.safeFetchSignalUsable("actor", actor) === null,
         "a \u00a77.4 navigation was loaded with an actor word the chokepoint's own registry does not " +
         "gate on: " + JSON.stringify(actor) + " — the word says whether the ANALYSED PAGE composed " +
         "this navigation or THIS TOOL did, which no other signal can answer, and safe-fetch.js aborts on an " +
         "absent one. It is stated per caller rather than once here because this loader serves a child " +
         "navigable (the page), a route an application declared of itself (the page), and an ambient seed " +
         "this tool composed from an address the person's browser navigated to (the tool)");
  /* The requested address, resolved once because every arm owes a URL: a navigable whose load did not load
     still gets a Document, at the URL that was requested. An unparseable address is a caller's serializer
     disagreeing with the URL parser, a broken contract, so it throws (a DFAIL would leave arms reading
     `undefined` in release). */
  const abs = new URL(u, base).href;
  /* The chokepoint is not optional: ast-worker.html loads safe-fetch.js before this file, so its absence is
     that load order broken. */
  DCHECK(typeof self.safeFetch === "function",
         "a §7.4 navigation was asked for with no network chokepoint installed — lib/safe-fetch.js is loaded " +
         "before this file in ast-worker.html, so its absence is that load order broken and every document " +
         "of this session would report as a load that did not load");
  /* The private-network principal is the caller's, never re-derived from `u`: safeFetch classifies the SSRF
     host relative to `opts.pageUrl`, so an address naming itself as principal would self-authorize. */
  DCHECK(typeof principalUrl === "string" && principalUrl !== "",
         "a §7.4 navigation was asked for with no private-network principal — safeFetch classifies the SSRF " +
         "host relative to it, and a load that supplied its own address as its own principal would let any " +
         "requested URL authorize itself into the user's intranet");
  /* The credentialed-read principal is a second fact: `pageUrl` is an address for the SSRF check, `pageOrigin`
     is a browser-stated origin deciding whose authenticated bytes may be read. A sandboxed frame reports an
     ordinary address and an opaque origin, so deriving one from the other would give it its embedder's
     session. */
  DCHECK(typeof principalOrigin === "string",
         "a §7.4 navigation was asked for with no credentialed-read principal — it is the browser's " +
         "MessageSender.origin for the document this load belongs to, and it is what decides whether the " +
         "person's session travels; `undefined` is a caller that stopped stating it, which would silently " +
         "return every tab to loading the LOGGED-OUT document");
  /* ── Who named this address ─────────────────────────────────────────────────────────────────────────
     A top-level navigation is a GET, in RFC 9110 §9.2.1 Safe Methods' safe set, so what remains is provenance.
     The firing decision is not made here: safe-fetch.js reads the word this call passes (`_provenanceOf` is a
     CHECK, fatal in release) and `_firingRefusal` is the per-origin widening, so the one decision serves both
     hosts at the line that opens the socket. At an unconfigured origin a derived navigation is refused like a
     forced one (the default fires program loads only), and the person widens the origin to permit it. A
     refusal comes back as the chokepoint's `blocked-signal:` reply record. */
  try {
    // Never `as: "script"`: these bytes are parsed as a document, not run as code.
    /* The session is carried where a browser's navigation would carry it; the chokepoint re-decides on the
       bytes that come back (credentialed SOP over the post-redirect origin), so a load that leaves this origin
       is refused there. Asking for cookies arms the destructive-path deny list, which safe-fetch.js scopes to
       `credentialed && provenance !== "observed"`: a document whose address carries a listed token is loaded
       when the person's browser navigated to it and refused when this tool derived or forced the address. */
    /* Fetch §2.2.5 Requests' `document` destination, the navigate algorithm's own fetch (HTML §7.4.5). Not
       script-like, so no CORB: the HTML parser reads these bytes. */
    /* `provenance` is the caller's statement of who named this address, relayed verbatim; the chokepoint owns
       the firing decision. */
    /* `credentials: "include"` is the navigation's own credentials mode (HTML §7.4.5 Populating a session
       history entry: "destination `document` … credentials mode `include`"); `credentialed` is this zone's
       willingness to spend the session, and safe-fetch.js's `_credentialedOf` composes the two. */
    /* `docReach` is `fromReach`, the grade of the document this request was made from (the initiator), not
       the join, which names the document these bytes will become. */
    /* `actor` and `provenance` are independent: a page navigates itself on a forced arm as readily as on an
       observed one. */
    const r = await self.safeFetch(abs, { pageUrl: principalUrl, pageOrigin: principalOrigin,
                                          destination: "document", provenance: provenance,
                                          docReach: fromReach, actor: actor,
                                          /* `unstated`: a navigation's provenance arrives by notice, not by
                                             park, so no witness mark was composed (see safe-fetch.js's
                                             `_pinnedOf`). Navigations fire through their own arm
                                             (`destination=navigation`, `actor=page`, `provenance=derived`),
                                             so the witness mark carries no decision here. */
                                          pinned: "unstated",
                                          credentials: "include",
                                          credentialed: navigationCarriesSession(abs, principalOrigin) });
    DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && r.headers && typeof r.headers === "object",
           "safeFetch answered a document load with something other than its reply record — HTML §7.4.5 " +
           "\"Populating a session history entry\"'s attempt-to-populate reads the BODY and the POLICY off " +
           "it, and a Document judged under no policy is how a page whose CSP kills a sink gets reported as " +
           "exploitable");
    DCHECK(typeof r.statusText === "string",
           "safeFetch answered a document load with no statusText — every refusal it makes carries its REASON " +
           "there (`blocked-scheme:`, `blocked-private-from-public`, `blocked-corb:`), which is the whole of " +
           "what a seeded page that could not be loaded has to tell its reader");
    /* Fetch §2.2.6 Responses' URL list, whose last item is the response's URL. Only this zone saw the redirect
       chain, and the engine determines the Document's origin over this string: a same-origin request that
       redirects off-origin yields a Document of another agent cluster. Asserted, since safeFetch was handed an
       absolute href and an empty list is its contract changing. */
    DCHECK(Array.isArray(r.urlList) && r.urlList.length >= 1 &&
           typeof r.urlList[r.urlList.length - 1] === "string" && r.urlList[r.urlList.length - 1] !== "",
           "safeFetch answered a document load with no Fetch §2.2.6 URL LIST — its last item is the " +
           "RESPONSE's URL, which is what HTML §7.4.5 determines the loaded Document's origin, CSP §2.2.2's " +
           "self-origin and §7.5.1's creationURL over. Without it a redirect off this origin becomes a " +
           "Document created under the principal of the address that was merely requested");
    const finalUrl = r.urlList[r.urlList.length - 1];
    /* Status 0 is not a reply: the chokepoint answers it when the request never went on the wire (blocked
       scheme, private target, CORB, unparseable URL), with the reason in `statusText`, which travels. */
    /* A decline is not an error page. `bytes: null` reads in the engine's `child_document` as a navigable
       showing an error page (opaque origin), which is right where the network refused. Where this zone
       declined, no load was attempted, and HTML §7.3.1.3 Child navigables leaves the navigable at the
       `about:blank` it was created holding, which inherits the creator's origin; flows forked in that frame
       differ between the two. */
    if (r.refusal) {
      DCHECK(r.refusal.kind === "network" || r.refusal.kind === "decline",
             "safeFetch graded a §7.4 navigation's refusal `" + r.refusal.kind + "`, which is neither word it " +
             "states — and the arm an unknown grade falls to here reports the load as a page whose server " +
             "could not be reached");
      /* The decline goes to the two arms that can state it (`rendezvous`, `report`) and aborts at `none`, whose
         three callers would fabricate different things: the `navigable.create` and `navigable.swap` notices
         (no request id; they would turn a null body into an empty document) and the ambient seed (it can
         report an unavailability through `onNavigationOutcome` but has no page-source kind for a decline in
         offscreen-brain.js's `_PAGE_SOURCE_KINDS`). The DFAIL names what to build for each. */
      if (r.refusal.kind === "decline") {
        if (refusalArm !== "rendezvous" && refusalArm !== "report")
          DFAIL("a §7.4 navigation was DECLINED by the chokepoint (" + r.refusal.reason + ") for a caller " +
                "whose refusal arm is `" + String(refusalArm) + "` — it has NO arm that can state a load " +
                "that was NEVER ATTEMPTED, so anything it is handed becomes a claim that is false. Its " +
                "`unavailable` vocabulary is status/empty/network and every one of the three says a load was " +
                "ATTEMPTED and failed, so answering with any of them hands the engine a navigable showing an " +
                "error page for an address nothing ever fetched, where §7.3.1.3 \"Child navigables\" leaves " +
                "the creator-inherited `about:blank` the navigable was created holding. THREE CALLERS STATE " +
                "`none` AND THEY FABRICATE DIFFERENT THINGS, so the thing to build differs and this names " +
                "which you are standing in. (1) A `navigable.create` NOTICE and (2) a `navigable.swap` " +
                "notice: peer-provisioning records with no request id, whose own body lines turn a null " +
                "body into an EMPTY Uint8Array — so what has to be built is a refusal the NOTICE channel " +
                "can carry, or the creating engine's own park reached some other way. (3) The AMBIENT SEED: " +
                "it CAN report an unavailability (`onNavigationOutcome`) and it CANNOT report a decline, " +
                "because offscreen-brain.js's `_PAGE_SOURCE_KINDS` is `status`/`empty`/`network` and every " +
                "one of those says a load was attempted — so what has to be built is a FOURTH page-source " +
                "kind, through that list, its sibling asserts and the popup that renders it. A ROUTE SEED " +
                "USED TO BE ON THIS LIST AND IS NOT: it states `report`, and `admitSeatLand`'s seat-drop arm " +
                "is what answers it. a3a93fd widened the old boolean to all three forward consumers with a " +
                "literal, which turned a refused child navigable into a silently empty one; fetchedDocument " +
                "takes the word from its consumer now");
        /* The third outcome: `bytes` and `unavailable` are both null, and `declined` is the reason, the only
           field telling "no load attempted" (creator-origin `about:blank`) from "a load failed" (opaque error
           page). */
        return { url: finalUrl, headers: {}, bytes: null, unavailable: null, declined: r.refusal.reason };
      }
    }
    if (r.status === 0)
      return { url: finalUrl, headers: {}, bytes: null, declined: null,
               unavailable: { kind: "network", detail: r.statusText } };
    /* Not OK is a load that did not load: the navigable shows an error page at the URL the response came from.
       An error response's body is never used as the document. */
    if (!r.ok)
      return { url: finalUrl, headers: {}, bytes: null, declined: null,
               unavailable: { kind: "status", status: r.status } };
    /* The bytes, because a Document is parsed from a byte sequence: HTML §13.2.3.2 Determining the character
       encoding is the engine's algorithm. And the whole header list: which names matter (opener policy,
       embedder policy, `Origin-Agent-Cluster`) is the engine's question. */
    return { url: finalUrl, headers: r.headers, bytes: r.body, unavailable: null, declined: null };
  } catch (e) {
    /* A thrown fetch is a network error (Fetch §5.6 Fetch methods) and a real outcome; an invariant abort
       thrown through this catch is not, so it is rethrown. */
    RETHROW_FATAL(e);
    return { url: abs, headers: {}, bytes: null, declined: null,
             unavailable: { kind: "network", detail: String((e && e.message) || e) } };
  }
}

/* The embedder policy item (HTML §7.1.4 Cross-origin embedder policies) of §7.1.7's policy container, as this
   zone relays it beside the CSP list. This zone does not interpret it: only the engine maps the tokens back
   to values, and it crashes on an unknown one. It owes only that the four items arrive unchanged and whole. A
   caller with no creator states NEW_EMBEDDER_POLICY, the section's initial value, since every container has
   one and there is no empty spelling. */
const NEW_EMBEDDER_POLICY = Object.freeze({ value: "unsafe-none", endpoint: "",
                                            reportOnlyValue: "unsafe-none", reportOnlyEndpoint: "" });

/* Whole or not at all, like the CSP list's two halves: §7.1.7's clone moves every item, so half an item
   arrives having replaced the creator's answer with the default. */
function embedderPolicyWhole(e) {
  return !!e && typeof e.value === "string" && e.value !== "" && typeof e.endpoint === "string" &&
         typeof e.reportOnlyValue === "string" && e.reportOnlyValue !== "" &&
         typeof e.reportOnlyEndpoint === "string";
}

// ---- Engine lifecycle over one wasm instance (one agent cluster) ----
/* `docName` is set only for a document another engine created: its name arrived in that engine's
   navigable.create notice (HTML §4.8.5 The iframe element creates a child navigable inside the insertion
   steps and cannot ask this zone). A root document is named here, by the counter above. */
/* `topLevelUrl` is the top-level creation URL of HTML §8.1.3.1 Environments. A root document is its own
   top-level traversable; a document a peer engine created carries its creator's decision on the notice. It is
   separate from the address because this document may be nested in another instance's document, and HTML
   §8.1.3.5 Secure contexts decides from the top of that chain. */
/* Provision the instance, then root it. The renderer is a frame in this document and is never collected, so
   anything that throws after it exists (init's return code, the bundle id, the key's origin, an IndexedDB
   read, the engine's abort) must take the frame with it; separating the two operations makes that cleanup
   one line. The renderer's name is this instance's agent cluster key: renderer-host.js takes a name and
   computes nothing, so the name this zone passes is the answer, and it identifies the frame. */
function engineCreate(code, html, msg, persist, docName, topLevelUrl, cold, inherited, parentNavigable,
                      containerPolicy, ancestorOrigins, creationSandboxFlags, referenced) {
  /* The one way to obtain an instance, asserted rather than met as a TypeError inside admit's catch (which
     would report a boot abort of an instance never built). render-process-host.js holds which agent clusters
     have an instance, mints the routing id and refuses a second for one cluster in every build;
     renderer-host.js materializes the frame for the id it is given. */
  DCHECK(typeof self.rendererLaunch === "function",
         "renderer-host.js is not loaded in this zone — it is what obtains an instance once the registry has " +
         "admitted its agent cluster, so without it every document would be reported as a crashed instance " +
         "rather than as a bridge that is missing half of itself");
  DCHECK(typeof cold === "boolean",
         "an instance was started without saying whether it has a live caller — `_cold` decides at finalize " +
         "whether this document's findings are RETURNED to a requester or MERGED to the moat, and it is a " +
         "fact about the call site (a child navigable and a resumed recipe have no requester; an admitted " +
         "document does), so it belongs on the record from the instant the pool can see it");
  /* The clone of the creator's policy container (HTML §7.1.7 Policy containers), or `null` for a document with
     no creator (a reported root, a rehydrated recipe, a group swap). Stated by every call site and never
     defaulted: a create notice always carries one, so `null` and "the caller forgot" must stay distinct. */
  DCHECK(inherited === null ||
         (!!inherited && typeof inherited.csp === "string" && typeof inherited.selfOrigin === "string" &&
          inherited.selfOrigin !== "" && embedderPolicyWhole(inherited.embedder)),
         "an instance was started with an inherited policy container that is neither `null` nor a WHOLE one — " +
         "CSP §2.2 makes a list a struct of policies AND a self-origin, and HTML §7.1.7 makes the container a " +
         "struct of that list AND §7.1.4's embedder policy, so a half of either is a clone that arrives unable " +
         "to resolve `'self'` against anything but this document's own address or claiming `unsafe-none` for a " +
         "creator that opted into cross-origin isolation");
  /* The parent of the navigable this instance is rooted in (HTML §7.3.1.3 Child navigables), a separate kind
     of fact from the container policy. `u` is the engine's encoding for no parent (a top-level traversable);
     anything else is the identity record the emitting engine wrote, relayed verbatim. SECURITY.md makes a
     cross-origin `<iframe>` the root of its own instance, so this is how such a root knows it is a child. No
     default: "the caller forgot" and "no parent" would become one value, reporting a frame as a page. */
  DCHECK(typeof parentNavigable === "string" && parentNavigable !== "",
         "an instance was started with no HTML §7.3.1.3 PARENT NAVIGABLE — every call site knows which of the " +
         "two answers applies (a create notice carries the identity its engine wrote; a reported root, a " +
         "rehydrated recipe and §7.3.2.3's swap have no embedder and state `u`), so an absent one is a caller " +
         "that skipped the question and a document that would present as a top-level page in the only " +
         "instance that holds it");
  /* The navigable's container's answer. The container is an element in the creating instance's tree and cannot
     cross, so the creator runs Permissions Policy §9.5 Create a Permissions Policy for a navigable and this
     carries the result; `null` is that grammar's "no container". No default: §9.7 Define an inherited policy
     for feature in container at origin turns a null container into `Enabled` for every feature, which would
     hand a cross-origin frame its embedder's capabilities. */
  DCHECK(typeof containerPolicy === "string" && containerPolicy !== "",
         "an instance was started with no HTML §7.3.1.3 CONTAINER statement — Permissions Policy §9.5's " +
         "answer for its navigable and `null` are the two things a caller can say, and every call site knows " +
         "which applies (a create notice carries the answer the creating engine computed; a document nothing " +
         "embeds says `null`). An absent one is a caller that skipped the question and a frame that would be " +
         "granted every feature its embedder holds");
  /* The internal ancestor origin objects list (HTML §3.1.3 Ancestor origins) for the Document this instance
     builds, computed in the creating instance because it reads the parent document's list, its origin record
     and the container element, none of which survive serialization (an opaque origin is decided by identity).
     `none` means no ancestors. No default: an empty list claims this Document is at the top of its tree, so a
     cross-origin frame would answer `location.ancestorOrigins` with `[]`. */
  DCHECK(typeof ancestorOrigins === "string" && ancestorOrigins !== "",
         "an instance was started with no HTML §3.1.3 ANCESTOR ORIGINS statement — the composed list and " +
         "`none` are the two things a caller can say, and every call site knows which applies (a create " +
         "notice carries the list the creating engine composed; a document nothing embeds says `none`). An " +
         "absent one is a caller that skipped the question and a frame that would report itself as the top of " +
         "its own tree");
  /* The creation sandboxing flag set (HTML §7.1.5 Sandboxing) for that navigable, not an item of the policy
     container (§7.3.2.1 Creating browsing contexts sets them in different steps). It reads the embedder
     element's flags, so the creating engine computes it and this carries the answer; `none` is the empty set.
     No default: an empty set claims nothing is sandboxed, so a cross-origin `<iframe sandbox>` would run
     scripts and submit forms its embedder forbids. */
  DCHECK(typeof creationSandboxFlags === "string" && creationSandboxFlags !== "",
         "an instance was started with no HTML §7.1.5 CREATION SANDBOXING FLAG SET — the composed set and " +
         "`none` are the two things a caller can say, and every call site knows which applies (a create " +
         "notice carries the set the creating engine computed; a document nothing embeds says `none`). An " +
         "absent one is a caller that skipped the question and a sandboxed frame with its sandbox deleted");
  /* Whether another instance holds a WindowProxy for this document. A referenced document's last flow reports
     itself host-owed instead of finishing (solver/engine.h's `engine_set_referenced`, via `SetReferenced`), so
     a later `windowproxy.get`, delivery or `w.length` still has a timeline to be answered in; only the zone
     holding the routing table knows. It is stated per call site because the answer differs: a group switch
     (HTML §7.1.3.2 Browsing context group switches due to opener policy) mints a name nothing holds a proxy
     for. Understating it drains a peer about to be read (the crash lands in the asking instance);
     overstating holds a frontier open until the pool releases it, costing a slot and truncating nothing. */
  DCHECK(referenced === 0 || referenced === 1,
         "an instance was started without stating whether a peer holds a reference into its document — the " +
         "flag decides whether this instance's timelines may finish, so an unstated one is either a peer that " +
         "drains before it is asked anything or a document held open for a question nobody can ask, and " +
         "neither is visible from here");
  const cluster = clusterKeyOf(msg);
  /* The pool is the register of who holds what; every caller has already asked whether this cluster has an
     instance (admit's `hostClusterOf`, the create notice's, and the cold tier's per-recipe key). */
  DCHECK(hostClusterOf(cluster) === null,
         "a second instance was started for an agent cluster that already has one (" + cluster + ") — two " +
         "heaps for one similar-origin window agent is the split SECURITY.md's one-instance-per-cluster rule " +
         "exists to forbid, and the pool already held the answer on the line that asked");
  /* The document id is minted before the first await, because the reservation must be answerable by name
     (`hostHolderOf` routes cross-document messages) from the instant it exists. A live root document keeps the
     name the browser gave it, so a document delivered twice (content.js re-ships CONTENT_SEED on RESHIP) is
     recognised rather than given a second instance; a peer-created document arrives named (`docName`); a
     rehydrated recipe takes the counter. */
  const docId = docName || (msg && msg.documentId ? String(msg.documentId) : String(++nextDocumentId));
  const eng = engineReserve(cluster, docId, msg, cold, referenced);
  /* The provisioning, the only part that suspends. The promise says when, never carries the instance (the
     caller already holds the record), and settles on both outcomes because hostSchedule's wait arm waits on
     it: an engine abort resolves it, recorded through engineBootFailed, which also removes the reservation;
     only an invariant abort rejects. It is assigned before anything can observe the record, since this function
     is synchronous up to the async body's first await. */
  eng._readyP = (async () => {
    try {
      eng.r = await self.rendererLaunch(cluster);
      await engineRoot(eng, code, html, msg, persist, docName, topLevelUrl, inherited, parentNavigable,
                       containerPolicy, ancestorOrigins, creationSandboxFlags);
      /* A Clear that landed mid-boot is honoured here: hostClear cannot destroy a renderer with a call
         outstanding, so it marks the record and the owner of that call removes the frame. */
      if (eng._dropped) eng.r.destroy();
      _reserveStats.rooted++;
    } catch (e) {
      engineBootFailed(eng, e);
      RETHROW_FATAL(e);
    }
  })();
  return eng;
}
/* The pool slot, taken synchronously before any suspension, so two arrivals for one agent cluster cannot both
   pass the `hostClusterOf` check and both build: two heaps for one similar-origin window agent are
   unrepresentable (a node inserted across them, a closure neither can call). The second arrival finds the
   record and joins it. The record carries only what is known without asking the instance: `residentBytes`
   and `topWeight` are absent until engineRecordFacts states them, and readers check `state` first. */
function engineReserve(cluster, docId, msg, cold, referenced) {
  /* `_readyP` is null here and filled by engineCreate on the next line; a placeholder promise would settle at
     a different moment from the provisioning. The wait arm asserts it is present. */
  /* `joinedDocIds` is the host's half of main.c's `g_joined_ctx`/`g_joined_dom`, declared empty: an instance
     holds one document per realm and `docId` names only the root, so every joined document must answer for it
     in `hostHolderOf`, the routing table a `windowproxy.post` resolves through. */
  /* The wipe generation this instance observes under, taken once before the first await: what a merge asks is
     whether this instance was running before a Clear, which is the same for every advance it produces.
     `onFrontierAdvance` (the partial-snapshot merge and the finalize merge of an instance with no live caller)
     compares it, as `_dispatchDocument` does for the terminal path. */
  DCHECK(typeof self.frontierEpoch === "function",
         "the trusted zone states no frontier epoch — it is what tells an advance from this instance apart " +
         "from one observed before the user emptied the store, and without it every finding this engine " +
         "produces would merge into whatever the store holds when it lands");
  /* `topDocId` is which document is currently the active document of this group's top-level traversable;
     `docId` is the root the instance was reserved for and never changes (the crash record and cold recipe are
     written under it). A same-origin navigation changes the first (HTML §7.4.6.1 Updating the traversable
     replaces the Document and keeps the navigable). */
  /* `_resumed` is null until `begin` decides how many parked flows seeded the frontier; zero is a different
     fact (no residue handed over, so a boot flow was seeded). */
  /* `referenced` is on the reservation, not an engineRoot parameter, because it outlives rooting: the
     create-notice join arm reads it to refuse adding a peer-referenced document to an instance entitled to
     drain. engineRoot turns it into the ABI call. */
  /* The cold-tier lookup fields, null until engineRoot's frontier read decides them; a boot that dies first
     carries the nulls unchanged, which says when it died. */
  const eng = { state: "booting", cluster, docId, topDocId: docId, joinedDocIds: [], msg,
                groupId: msg && msg.groupId, _resumed: null, referenced,
                _coldLookup: null, _coldOther: null, _bundleId: null,
                origin: (msg && msg.origin) || "", _cold: cold, _resolvers: [], _remoteAsked: new Set(),
                /* The requests this zone has asked for and not yet answered to the engine, keyed as the
                   answering seam is keyed. The engine re-lists a request with neither value nor refusal every
                   round (solver/engine.c's `skip` is `!u || PEND_HAVE_VALUE || declined`), so a door that issues
                   without waiting must dedup, or one park becomes one request per round. engine/host/wpt_runner.c
                   drives the same ABI with the same table and keys (see engineIssue). */
                _inflight: new Map(),
                _epoch: self.frontierEpoch(), r: null, _readyP: null,
                /* Egress asked and refused, per rule, declared here so a run that refused nothing still carries
                   the map. `asked` is raised at the call and `declined` at the refusal in engineServiceFetch's
                   one loop, so the sum is bounded by `asked`. Both are lifetime counts of ask events, not of
                   distinct addresses (an @S candidate re-fire asks again). */
                _egress: { asked: 0, declined: Object.create(null) } };
  _pool.push(eng);
  _reserveStats.made++;
  const n = _bootingCount();
  if (n > _reserveStats.peakBooting) _reserveStats.peakBooting = n;
  return eng;
}
/* A reservation that did not become an instance leaves the pool here, for every call site: one left behind
   would be joined by every later arrival for that cluster, which would wait for ever on a failed boot. */
function engineBootFailed(eng, e) {
  const i = _pool.indexOf(eng);
  DCHECK(i >= 0 || eng._dropped,
         "a reservation failed to boot and was already out of the pool with no Clear having taken it — the " +
         "slot it holds is this function's to release, so something else removing it means two owners for one " +
         "record and the surviving one will free a frame twice");
  if (i >= 0) _pool.splice(i, 1);
  eng.state = "failed";
  /* The frame goes with it. renderer-host.js reclaims its own frame (and frees the cluster in the registry)
     when the boot handshake fails, so `r` is null exactly when there is nothing to remove. */
  if (eng.r) eng.r.destroy();
  /* One instance failing is one crash however many documents were waiting on it: the banner fires once and a
     record is built per caller. */
  const m = String((e && e.message) || e);
  /* The cause: a boot that aborts inside the engine rejects an ABI call, and renderer-host.js attaches the
     frame's last lines (`e.rendererLines`), where the C CHECK/DCHECK printed its @WHY root line. A failure that
     never reached an ABI call (rendererLaunch, or an assert here) has no lines and its message is the cause. */
  const _rl = e ? e.rendererLines : undefined;
  DCHECK(_rl === undefined || Array.isArray(_rl),
         "a boot failure carried a `rendererLines` that is not a list of the frame's output — it is the only " +
         "place the engine's own @WHY survives a rejected ABI call, and a shape this cannot scan is that root " +
         "line being discarded at the one site that holds it");
  const root = _rl === undefined ? "" : rootWhyLine(_rl);
  const err = root ? (m + " | ROOT: " + root) : m;
  crashBanner("create", err);
  for (const w of eng._resolvers) w.resolve(crashRecord("create", err, w.msg, eng));
  eng._resolvers.length = 0;
  _reserveStats.failed++;
}
/* How many parked flows seeded this session's frontier, read once where the seeding happened. solver/cold.c
   prints `@RESUMED <n>` from cold_resume, called only for a non-empty residue, after DCHECKing `flows > 0`, so
   an absent line and a zero are distinct. `asked` (this zone composed the `recipes` argument) and the line are
   asserted against each other: a residue with no line is a silent rebuild, a line with no residue a resume of
   a frontier never given. Release answers `null`, not `0`, for asked-but-silent, since the count is unknown. */
function engineResumeCount(lines, asked) {
  let n = -1;
  for (const raw of lines) {
    const ln = String(raw);
    if (!ln.startsWith("@RESUMED ")) continue;
    const tail = ln.slice(9);
    DCHECK(/^[0-9]+$/.test(tail),
           "the engine printed an @RESUMED line whose count is not a decimal number (`" + tail + "`) — " +
           "solver/cold.c writes it with one `%ld`, so anything else is that line having been composed by " +
           "something other than the rebuild it reports on");
    DCHECK(n < 0,
           "a session printed @RESUMED twice (" + n + " then " + tail + ") — solver/engine.c calls cold_resume " +
           "once per session and the frontier is seeded once, so a second line is a second rebuild landing on " +
           "top of a frontier that already stands on the first one's segments");
    n = Number(tail);
    DCHECK(n >= 1,
           "the engine reported a rebuild of " + n + " flows — cold_resume DCHECKs `flows > 0` before it " +
           "prints, so a zero here is a residue whose whole point was unreachable arriving as a number this " +
           "zone would render as a successful resume of nothing");
  }
  DCHECK((n >= 0) === asked,
         asked ? "this zone handed the engine a parked residue and the engine reported no rebuild — the " +
                 "recipes are the only thing that seeds a resumed session, so a silent begin is every parked " +
                 "flow of that document dropped between the store and the frontier, with the next park " +
                 "writing the survivors back as if they were all there had ever been"
               : "the engine reported resuming " + n + " parked flows into a session this zone seeded with no " +
                 "recipes at all — solver/engine.c seeds from a residue or from a boot flow and never both, " +
                 "so those flows stand on decision vectors this document was never handed");
  return n >= 0 ? n : (asked ? null : 0);
}
/* The local is `rend` and the field `eng.r` because the fetch closures below bind `const r` to Fetch's reply
   record; a renderer named `r` would be shadowed there. engineRoot fills the reservation and returns nothing:
   the pool's record is the one every question about this cluster has been answered by since provisioning
   began, and a second object would give one document two records. */
async function engineRoot(eng, code, html, msg, persist, docName, topLevelUrl, inherited, parentNavigable,
                          containerPolicy, ancestorOrigins, creationSandboxFlags) {
  const rend = eng.r;
  DCHECK(rend && rend.name === eng.cluster,
         "a renderer was provisioned under a name that is not its instance's agent cluster — the frame's " +
         "title is how this zone identifies which instance the pool is talking about, and the pool keys the " +
         "same instance by `cluster`, so two spellings of one identity is a routing table that disagrees with " +
         "the DOM");
  /* The renderer's line buffer itself: renderer-host.js appends every drained line as each reply lands, so
     engineCrash's root-@WHY scan, streamPartial's @RESULT scan and linesToAnalysis read what the frame printed,
     in order. */
  const lines = rend.lines;
  // Phase 1: parse and boot; the engine computes the bundle identity from its Lexbor <script> scan.
  /* The whole response header list crosses as HTTP field lines, not single headers: several algorithms read
     different names from the same list (HTML §7.1.7's embedder policy, §7.5.1 Shared document creation
     infrastructure's opener policy and `Origin-Agent-Cluster` for §8.1.2.2's agent cluster key), and Fetch's
     `get` decides what a repeated header means. The browser's `Headers` iteration has already combined repeats
     as `get` does, so one line per name gives the engine's item parse the browser's verdict. The engine parses
     it back into a header list (core/fetch/headers.c) and reads it once, into HTML §7.4.2.1 Supporting
     concepts' navigation params, which §7.4.5 Populating a session history entry creates from the response. */
  const _headers = responseFieldLines(msg && msg.responseHeaders);
  // The document id is minted on the reservation (engineCreate), so `hostHolderOf` routes to a booting
  // document. It is unique across the instances alive in this offscreen, the set that can message each other;
  // it is not persisted because a parked foreign segment does not yet outlive a session.
  // The top-level creation URL (HTML §8.1.3.1 Environments), always browser-stated: a peer-created document
  // carries its creator's decision on the navigable.create notice; a content-script report carries the tab's
  // url from sender.tab.url; a rehydrated recipe carries what its session recorded. HTML §8.1.3.5 Secure
  // contexts reads it to decide which Web IDL §3.3.13 [SecureContext] members exist, so the engine refuses "".
  const _tlu = topLevelUrl || (msg && msg.topLevelUrl) || "";
  // No `code` reaches qjs_init: identity and the script inventory are the engine's Lexbor <script> scan of
  // `html`, since a concatenation of scripts cannot represent per-script scope.
  // The address, not the origin, crosses: it is the API base URL (HTML §8.1.3.2 Environment settings objects)
  // every relative URL resolves against, and the engine derives the origin from it (its own url.c), so the
  // principal and the address are one fact.
  /* qjs_init's return value is read: its C body is CHECKs that abort, so it returns 0, and anything else is an
     entry reporting a failure this zone must hear. */
  /* The document crosses as bytes, since `qjs_init` takes a byte sequence and its length, and every live
     document arrives as the response's own bytes from navigationLoad. The string arm serves a cold-tier entry
     an earlier session parked as characters; it encodes, never decodes, because decoding would be this zone
     running HTML §13.2.3.2 Determining the character encoding badly. Both shapes are asserted, never defaulted
     to "". The buffer is not transferred: `eng.html` keeps it for finish() to write to IndexedDB as the cold
     recipe's document (mojom.js states this as a property of the declared type). */
  DCHECK(html instanceof Uint8Array || typeof html === "string",
         "a document reached qjs_init as neither a byte sequence nor characters — navigationLoad ships a " +
         "response body and a cold-tier entry may hold characters, so anything else is a producer that " +
         "stopped producing a document, with nothing downstream to notice but an empty finding set");
  const _doc = html instanceof Uint8Array ? html : new TextEncoder().encode(html);
  /* A U+0000 in these bytes is input, not an invariant: HTML §13.2.3.5 Preprocessing the input stream defines
     how each tokenizer state handles it. `qjs_init`/`qjs_join` take `(bytes, len)` and the C entry DCHECKs the
     guard byte at `bytes[len]`, so the length and the C read cannot disagree.
     Named residual: the engine decodes these bytes as UTF-8 rather than running HTML §13.2.3.2 Determining
     the character encoding. Next diff builds the sniff over this byte sequence. Absence shows as non-UTF-8
     pages whose text and attribute values arrive mojibaked. */
  /* The address is asserted, never defaulted: `originOf("")` is "", a frontier key every unidentifiable
     document would share. The engine's entry CHECKs that it parses; this fails in the zone that made the hole. */
  DCHECK(!!msg && typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
         "a document reached qjs_init with no address — §4.4's document address is what the engine derives " +
         "this document's ORIGIN from (§4.7's serialization, its own url.c) and what every relative URL the " +
         "bundle builds resolves against, so a document without one is analysed behind no principal at all");
  /* The inherited container as wire fields beside `_headers`, because it is not part of the response: it is
     HTML §7.1.7's clone for a Document with a creator, and the engine decides which list the Document gets. The
     empty pair states that there is no creator. */
  /* The embedder policy item rides with the list. A document with no creator (a reported root, a rehydrated
     recipe, a group swap) gets "a new embedder policy" (HTML §7.1.7), which NEW_EMBEDDER_POLICY spells. */
  const _initEp = inherited ? inherited.embedder : NEW_EMBEDDER_POLICY;
  /* The parent navigable and the container answer are relayed beside the container: they are what only this
     zone knows about a document it did not root (whether it is nested in another instance's navigable), and
     engineCreate asserts them. */
  const _init = await rend.renderer.init({
    document: _doc, url: msg.sourceUrl, docId: eng.docId, headers: _headers, topLevelUrl: _tlu,
    inheritedCsp: inherited ? inherited.csp : "",
    inheritedCspSelfOrigin: inherited ? inherited.selfOrigin : "",
    inheritedCoep: _initEp.value, inheritedCoepEndpoint: _initEp.endpoint,
    inheritedCoepReportOnly: _initEp.reportOnlyValue,
    inheritedCoepReportOnlyEndpoint: _initEp.reportOnlyEndpoint,
    parentNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags });
  DCHECK(_init.rc === 0, "qjs_init reported a failure this zone has no handling for — the engine's own entry " +
                         "CHECKs every precondition and aborts, so a non-zero return is a contract that changed");
  /* Whether a peer holds a WindowProxy for this document, stated after `Init` and before `Begin`, the C entry's
     own bounds: there must be a document (`g_ctx`) and the frontier must not be seeded yet (`!g_begun`), since
     the flag decides whether the last timeline may finish. It is stated unconditionally, including 0, so the
     C flag is never left at its static initialiser. `qjs_set_referenced` survives a `Teardown`, so it is not
     restated after one. */
  await rend.renderer.setReferenced({ referenced: eng.referenced });
  /* Never 0: document_bundle_id folds an empty scan to 1, since a 0 would key every unidentifiable document to
     one frontier entry. */
  /* `>>> 0` because the wire carries an i32 (mojom `int32 bundleId`) for a C `unsigned`; the reader
     reinterprets it. */
  const _bidRaw = (await rend.renderer.getBundleId()).bundleId >>> 0;
  DCHECK(_bidRaw !== 0, "the engine answered a bundle id of 0 — document_bundle_id never returns one, and a 0 " +
                        "collides every document's frontier key with every other's");
  const _bid = _bidRaw.toString(36);
  /* The address parses in this zone too: qjs_init has CHECKed that the engine's url.c parsed it, so an empty
     origin here means the two parsers disagree about which document the key names. */
  DCHECK(originOf(msg && msg.sourceUrl) !== "",
         "this zone could not serialize an origin from a document address the engine's own url.c accepted — " +
         "the frontier key would name a document by a string the two parsers do not agree is one");
  const fkey = msg.sourceUrl + "|" + _bid;
  /* One read that answers why a key missed beside what it found (see COLD_LOOKUP): frontierLookup asks the key
     and, in the same transaction, the keys stored for this address. The three fields are written here, at the
     one moment the answer exists. */
  const _look = persist ? await frontierLookup(fkey, msg.sourceUrl) : null;
  const _cl = coldLookupOf(fkey, _look);
  eng._coldLookup = _cl.state; eng._coldOther = _cl.others; eng._bundleId = _bid;
  /* The word and the residue come from one read and are not asserted against each other: `hit` is defined by
     `_look.entry` and `prior` is `_look.entry`, so that comparison cannot fail. The assert that can fail is in
     `coldLookupOf`. */
  const prior = _look ? _look.entry : null;
  /* The residue belongs to this document: the engine seeds from these recipes instead of a boot flow, so a
     residue from another document would replay a path this program never took. Under the current key this is
     an identity; it fires if a term ever leaves the key. */
  DCHECK(!prior || prior.sourceUrl === msg.sourceUrl,
         "the cold tier answered this document's frontier key with a residue parked by a document at a " +
         "DIFFERENT address (" + (prior && prior.sourceUrl) + " vs " + msg.sourceUrl + ") — the engine seeds " +
         "from recipes instead of a boot flow, so this document would be replayed along another one's " +
         "decision vectors and never explored from its own first script");
  // Phase 2: seed the frontier, fresh or from parked recipes. The host sets a value yield floor per step (the
  // runner-up engine's weight), so this engine yields when outranked rather than after a fixed slice.
  /* The residue handed over, named before it is sent: solver/engine.c seeds from recipes or a boot flow and
     never both, so "" is this zone's own statement that nothing was resumed. */
  const _residue = (prior && prior.recipes) ? prior.recipes : "";
  await rend.renderer.begin({ recipes: _residue });
  /* The engine's half: `cold_resume` prints `@RESUMED <n>`, and every reply drains the output with it (mojo.js
     `_envelope`), so the line is in `lines` when this await resolves and never again. It is read once here and
     linesToAnalysis reads the field. */
  eng._resumed = engineResumeCount(lines, _residue !== "");
  // Dev-only verification hook (a real page never carries this query param): `?__forcepark=1` forces the
  // RAM-pressure park, so the cross-session round trip (park recipes, IndexedDB, restart, resume) is verifiable
  // without a 512MB working set. The param is part of the address and so of the frontier key: that address
  // names its own entry, and a rehydration re-derives the same key.
  // The park is deferred two dispatches so it fires mid-exploration, like a production park: a park before any
  // step writes one record, `f-,0`, which resumes as a flow indistinguishable from a fresh boot flow, so the
  // round trip would carry nothing. Two is the smallest count at which the boot flow has forked and the park
  // holds `s…` segment records; the property to preserve is "the park contains at least one `s` record".
  // Only on the initial park: firing while resuming would re-park the rehydrated recipes for ever.
  let _forceparkSteps = 0;
  if (msg && typeof msg.sourceUrl === "string" && /[?&]__forcepark=1\b/.test(msg.sourceUrl) && !(prior && prior.recipes)) {
    _forceparkSteps = 2;   // park after boot + the first flow burst: flows have RUN + SUSPENDED (real decvecs) but not yet drained
  }
  /* The chokepoint and the document's address are a load order, asserted once here as a crash. Answering
     either with a network error would tell the page's code its server was unreachable and explore a failure
     path under a wiring defect. */
  DCHECK(typeof self.safeFetch === "function",
         "an engine is being driven in a realm where `safeFetch` is not a function — SECURITY.md puts every " +
         "byte of network through that chokepoint, so this zone cannot answer a single park, and the two " +
         "seams that used to report it as a network error told every page's code that its server was " +
         "unreachable. It is a load order: `extension/lib/safe-fetch.js` has not been loaded into this realm");
  DCHECK(msg && typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
         "an engine is being driven for a document with no source URL — every park's address is resolved " +
         "against it (`new URL(u, msg.sourceUrl)`), so without one nothing this frontier asks for can be " +
         "named, and answering that with a network error reports an unreachable server for a request this " +
         "zone could not even address");
  /* The reply record the engine parses, the shape every host of this engine delivers:
     `{status, statusText, headers: [[name, value], …], urlList, computedType}` plus the body's bytes beside it,
     as the C hosts build with fetch_reply_new. The URL list (Fetch §2.2.6 Responses) is what `response.url`
     and `response.redirected` read. `null` is a network error (the engine rejects with Fetch §5.6 Fetch
     methods' TypeError), never an empty 200.
     The body is not in the record: Fetch §2.2.4 Bodies makes a body's source a byte sequence, which JSON cannot
     carry without a codec or a decode, and a decode here would run in the zone that owns SOP/CORS and no
     semantics, before the engine's own decode sees the declared charset. So the record travels as text and the
     bytes as bytes, copied straight into the engine's linear memory. */
  /* The request is the (method, url) pair the flow parked on; `method` arrives normalized (Fetch §2.2.1
     Methods). A method this zone cannot issue is refused before the call and never downgraded: safeFetch is
     GET only (SECURITY.md), so answering a POST's key with a GET's reply would be a response the server never
     gave. On this seam the refusal is spelled `null`, because core/fetch builds any object into a Response,
     so a status-0 record would resolve `fetch()` with a status no server returns. fetchedXhr spells its
     refusal as a status-0 record, which xml_http_request.c's xhr_take_reply treats as a network error. */
  /* The request's destination travels with it (Fetch §2.2.5 Requests): safe-fetch.js decides the CORB class
     from §2.2.5's script-like predicate over it, so it is passed through, not reduced to a boolean. */
  const fetched = async (method, u, destination, provenance, pinned, credentials) => {
    DCHECK(pinned === "pinned" || pinned === "unpinned",
           "a pending request reached the fetch relay carrying no witness mark — solver/engine.h states " +
           "`pinned` or `unpinned` on every line of the pending join, and the chokepoint reads it WITH the " +
           "destination to decide whether an act may be spent on an address this engine may have composed. " +
           "A relay that dropped it would have safe-fetch.js answer from `unstated`, which is the word for " +
           "an act that never had one");
    DCHECK(typeof destination === "string",
           "a pending request reached the chokepoint with no DESTINATION — GetPending answers " +
           "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` and Fetch §2.2.5 " +
           "makes the destination part of the request, so a caller that omits it is one whose code load " +
           "would be fetched as data and compiled. The empty string is a real destination and means DATA");
    DCHECK(typeof method === "string" && method !== "",
           "a pending request reached the chokepoint with no method — GetPending answers " +
           "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` and the " +
           "(method, url) pair is what the flow parked on, so a request whose method is unknown can be " +
           "neither refused nor issued");
    /* The credentials mode is passed through for the same reason; reducing it with this zone's willingness is
       the chokepoint's job (`_credentialedOf`). */
    DCHECK(typeof credentials === "string" && credentials !== "",
           "a pending request reached the chokepoint with no CREDENTIALS MODE — GetPending answers " +
           "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL`, Fetch §2.2.5 " +
           "\"Requests\" gives every request one, and only the algorithm that created it knows which. A " +
           "caller that drops it hands `safe-fetch.js` a request it can decide nothing about");
    /* The method half of the firing question is asked of the chokepoint (`safeFetchMethodRefusal`), so this
       host and engine/trusted.mjs answer it once, in the chokepoint's refusal vocabulary, and the flow stays
       parked rather than being told the server was unreachable. */
    const _mRefusal = self.safeFetchMethodRefusal(method);
    if (_mRefusal) return { refusal: _mRefusal };
    /* A holed address is a park carrying an address the engine never determined, so there is no request to
       make and nothing for the chokepoint to grade; it is answered as a network error.
       Named residual: that answer sends the page's code down a failure path for a request nobody could make.
       Next diff stops the engine listing such a park to a host at all, treating the address as a shape (the
       @H shape rules). Absence shows as holed addresses that never reach the wire yet whose callers' catch
       arms are explored. */
    if (hasHole(u)) return null;
    try {
      const abs = new URL(u, msg.sourceUrl).href;
      // Every park takes one shape; the request's own destination decides the chokepoint's rule (CORB for a
      // code load), and credentials are stated below.
      /* @security-finding: no `pageOrigin` reaches the chokepoint from this path (replies to requests the
         analysed bundle made, and chunk loads), so it is stated uncredentialed. The document-load path passes
         one and carries the session. `msg.credentialed` states whether the document load carried the session
         (what the frontier record remembers) and is not this question; reading it here made the credentialed SOP
         refuse every reply unread after spending the person's cookies. Stated `false`, nothing gains credentials,
         and the deny list's scope to credentialed requests is unchanged (an uncredentialed GET to a logout path
         destroys nothing).
         When credentials are built for this path, pass `msg.origin` (MessageSender.origin, plumbed by
         _dispatchDocument), never `originOf(msg.sourceUrl)`, which would give a sandboxed iframe its embedder's
         authenticated bytes. That also loosens CORB for a same-origin chunk, a decision to take then. */
      /* One shape for every park: the destination decides which rule the chokepoint applies and the policy on
         whether a code load may carry the session is the chokepoint's (HTML §8.1.4.2 Fetching scripts gives a
         classic script `same-origin` credentials). */
      /* `provenance` is what the request is evidence of, composed by the engine at the park from HTML §4.12.1.1
         Processing model's parser-inserted (a script whose parser document is non-null) and the parking flow's
         `path_forced` (solver/engine.h). Relayed, never re-derived or defaulted: the chokepoint holds the firing
         decision and an unknown value is fatal there. */
      /* `credentialed: false` is stated, not read (see the finding above). */
      /* `credentials` is the request's own mode (Fetch §2.2.5 Requests), relayed off the pending line; its
         conjunction with `credentialed` is safe-fetch.js's and can only narrow, so it makes `omit` parks
         refusable and `include` ones honest once the principal above is wired. */
      /* `docReach` is how the document this park belongs to was reached (`msg.provenance`, this zone's word
         about the load that produced it), distinct from `provenance` (the engine's word about the flow's path
         inside the document): a page this tool opened makes requests the engine grades `observed`.
         Named residual: a same-origin child that joins this instance (`engineJoin`) is judged under the root's
         `msg.provenance`, since this closure holds the root's message. Next diff carries the park's document
         name on the pending line so the lookup is per document (the engine already routes by that name). Absence
         shows, at an origin widened for one grade and not another, as identical requests firing from a joined
         document and refused from a top-level document reached the same way. */
      /* `actor: "page"`: this frame answers a park the engine put on its pending line, and the engine parks on
         what the analysed document's code does. Stated so safe-fetch.js can assert it; with `witness` unpinned it
         is the row the owner's value arm turns on. */
      const opts = { pageUrl: msg.sourceUrl, destination, provenance, pinned, credentials,
                     actor: "page",
                     docReach: msg.provenance, credentialed: false };
      const r = await self.safeFetch(abs, opts);
      /* The chokepoint's record is fixed: safe-fetch.js returns {ok,status,statusText,headers,body,urlList} on
         every path, blocked ones included, so a malformed answer is asserted rather than turned into a network
         error. The body is bytes; a string would mean safeFetch ran `text()` (Fetch §5.3 Body mixin) and the
         original bytes are gone. */
      DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && typeof r.status === "number",
             "safeFetch answered with something other than its reply record — the engine builds a Response " +
             "out of this and a page reads status/headers/body off it, and §2.2.4's body source is a BYTE " +
             "SEQUENCE");
      /* The URL list straight from the chokepoint: it always holds at least the requested URL (Fetch §4.1 Main
         fetch: an empty internal URL list is set to a clone of the request's), and the engine DCHECKs that too. */
      DCHECK(Array.isArray(r.urlList) && r.urlList.length >= 1,
             "safeFetch answered a reply with no URL list — response.url and response.redirected are read off " +
             "nothing else, and the engine would report every redirect as none");
      DCHECK(r.headers && typeof r.headers === "object",
             "safeFetch answered a reply with no header map — the engine's Headers record is built from it");
      /* The type the chokepoint computed, the one fact on this record the engine may not derive itself:
         safeFetch ran the sniff and the renderer is told the answer. The empty string is MIME Sniffing §5.1
         Interpreting the resource metadata's "the supplied MIME type is undefined", a positive answer. */
      DCHECK(typeof r.computedType === "string",
             "safeFetch answered a reply with no computed content type — solver/reply_decode.c reads this " +
             "field instead of re-deriving a type from the raw header, so an absent stamp is a producer that " +
             "failed and never a resource whose type is unknown");
      /* The status message's empty value is a real answer (HTTP/2 and HTTP/3 carry no reason phrase), and the
         blocked arms put their refusal reason there, so it is asserted, never defaulted. */
      DCHECK(typeof r.statusText === "string",
             "safeFetch answered a reply with no statusText — it is written on every path (the blocked arms " +
             "carry their REFUSAL REASON in it), and an empty string is a legitimate answer from any HTTP/2 " +
             "response, so an absent one cannot be defaulted without erasing the difference");
      /* Status 0 is not a reply: the chokepoint answers it when the request never went on the wire (bad URL,
         blocked scheme, private target, CORB), and it crosses as `null`, the network error. */
      /* Which kind of non-reply it is, stated by the chokepoint (`refusal.kind`) and never re-derived from
         `statusText`. A refusal a browser also makes (a `file:` scheme, a CORS failure, CORB) is a network
         error, which is fidelity. A refusal only this tool makes (the firing policy, the destructive deny list)
         has no browser behind it, so it is a `decline`: the flow stays parked rather than being told the origin
         failed. */
      DCHECK((r.status === 0) === (r.refusal !== null),
             "safeFetch answered with a status and a refusal that disagree — `status: 0` is the one status no " +
             "HTTP response has and is what every refusal answers, and `refusal` is the same fact graded, so " +
             "a record carrying one without the other is a decline arriving as a reply or a reply arriving as " +
             "a request that never happened");
      if (r.refusal) {
        DCHECK(r.refusal.kind === "network" || r.refusal.kind === "decline",
               "safeFetch graded a refusal `" + r.refusal.kind + "`, which is neither word it states — a " +
               "third grade would take whichever arm this branch happens to be written with as its else, and " +
               "that arm fabricates a network error for a request nobody sent");
        DCHECK(typeof r.refusal.reason === "string" && r.refusal.reason === r.statusText,
               "safeFetch graded a refusal whose reason is not the one on the record — the reason is the only " +
               "account a person ever gets of a request this zone did not make, and two spellings of it are " +
               "two answers to which rule refused");
        if (r.refusal.kind === "decline") return { refusal: r.refusal };
      }
      if (r.status === 0) return null;
      /* Two pieces of one reply crossing on two channels: `meta` is the JSON qjs_provide parses and `bytes` is
         copied into the engine's heap beside it. Returned together so neither is delivered without the other. */
      return { meta: { status: r.status, statusText: r.statusText, headers: Object.entries(r.headers),
                       urlList: r.urlList, computedType: r.computedType },
               bytes: r.body };
    } catch (e) {
      /* A thrown fetch is a network error and crosses as `null`; an invariant abort thrown through this catch
         is not, so it is rethrown. */
      RETHROW_FATAL(e);
      return null;
    }
  };
  // The response (HTML §7.4.5 Populating a session history entry) for a child navigable this engine created,
  // through the same navigationLoad as the seed that rooted this instance. `fetched` cannot serve it: a child
  // document needs its header list, since a document with no policy would report CSP-blocked sinks as
  // exploitable. Both principals are the initiating document's: `msg.sourceUrl` for the SSRF classification
  // and `msg.origin` for the credentialed read, so a same-origin child carries the session as this document's
  // own load did. `unavailable` has no reader here (a child's page is not in the popup's page-source row).
  /* `provenance` is the engine's statement of who named the address, carried by every record reaching this
     function (the `document.fetch` request, the `navigable.create` and `navigable.swap` notices), relayed
     and never re-derived: an address cannot tell a frame the person's session would load from one that exists
     because a gate was forced. */
  /* `refusalArm` is the consumer's, not this function's: `document.fetch` is answered through
     engineDeliverDocument, which routes a refusal to the rendezvous the engine is parked on, while
     `navigable.create` and `navigable.swap` are notices with no id to refuse and state `none`. A shared
     literal would let a notice turn a declined load into an empty document, a plausible datum for a frame
     the chokepoint refused. */
  /* This forward serves only `rendezvous` and `none`: no consumer of it drops a work item, so a `report` would
     be honoured by the loader and then fabricated into a document by the notice that asked. A consumer that
     gains such an arm widens this list deliberately. */
  const fetchedDocument = async (u, provenance, refusalArm) => {
    DCHECK(refusalArm === "rendezvous" || refusalArm === "none",
           "a §7.4 document load was asked for stating the refusal arm " + JSON.stringify(refusalArm) +
           ", and this forward serves only `rendezvous` and `none` — a request that parks on a rendezvous " +
           "and two notices that can state nothing. `report` is a word for a caller that drops a WORK ITEM, " +
           "which none of these three does, so honouring one here would hand a declined record to a consumer " +
           "whose only arm for it fabricates an empty document — which is the literal this parameter " +
           "replaced");
    const r = await navigationLoad(u, msg.sourceUrl, msg.sourceUrl, msg.origin, provenance, msg.provenance,
                                   refusalArm, /*actor*/"page");
    /* `declined` crosses (engineDeliverDocument routes it to the rendezvous, so the navigable keeps the
       `about:blank` it was created holding); `unavailable` does not, since nothing here reads it. */
    DCHECK(refusalArm === "rendezvous" || r.declined === null,
           "a §7.4 document load came back DECLINED for a consumer whose refusal arm is `" +
           String(refusalArm) + "`" +
           " —" + " `navigationLoad` is supposed to have aborted instead, so either its " +
           "refusal-arm test or this forward has come apart. The arms are not interchangeable: a notice " +
           "has no rendezvous, and the only thing such a consumer could do with a refusal is turn it into bytes");
    DCHECK(r.declined === null || typeof r.declined === "string",
           "a §7.4 navigation's result does not STATE whether it was declined — `r.declined` is `" +
           String(r.declined) + "`. The field used to be read as `r.declined || null`, and that default is the " +
           "whole of what this check replaces: a load arm that forgot to state it read as a load this zone " +
           "fetched, so a frame the chokepoint had refused would be answered with its bytes-less reply through " +
           "the ANSWER path and the engine would park on a rendezvous nothing would ever refuse");
    return { url: r.url, headers: r.headers, bytes: r.bytes, declined: r.declined };
  };
  /* The routing table is the pool. `docId` (which document this instance holds) and `origin` (the value
     stamped on everything it sends) are read only by the notice router below: SECURITY.md makes this zone
     the one that knows which instance holds which document and the one that may state a sender's origin. */
  // The XMLHttpRequest fetch (XHR §3.5.6 The send() method) through the same chokepoint. `rec` is the
  // engine's JSON request record (method, url, headers, body); the answer is the reply shape fetch_reply_new
  // builds, and a null body is the network error §3.5.6's handle errors turns into an `error` event.
  const fetchedXhr = async (rec) => {
    let q = null;
    /* The request record is the engine's, crossing as JSON so it carries its types; unparseable text is
       xml_http_request.c writing something other than its record, so it aborts rather than becoming an
       `error` event on the page's XHR. */
    try { q = JSON.parse(rec); }
    catch (e) { DFAIL("the engine's xhr.send record is not JSON: " + String(e && e.message || e)); }
    DCHECK(q && typeof q === "object" && typeof q.url === "string" && q.url,
           "the engine's xhr.send record names no URL — the chokepoint decides SOP, CORS, method and " +
           "credentials, and cannot decide about a request it was never told");
    // Release: the DCHECK above is stripped, so a missing record answers a network error here.
    if (!q || typeof q.url !== "string") return { meta: { status: 0, statusText: "", headers: [] }, bytes: null };
    /* The method is part of the request's identity: a method this zone cannot issue is refused, never
       downgraded. safeFetch is GET only (SECURITY.md), so a POST answered with a GET's reply would be a
       response the server never gave, from which every @H example and @S verdict would derive. The method
       arrives normalized (Fetch §2.2.1 Methods; XHR §3.5.1 The open() method runs "normalize a method"), so
       GET, HEAD included, is exactly what this zone can perform. */
    DCHECK(typeof q.method === "string" && q.method !== "",
           "the engine's xhr.send record names no method — xhr_request_op writes one on every record, and a " +
           "request whose method is unknown cannot be refused OR issued");
    /* The refusal is the chokepoint's grade (`safeFetchMethodRefusal`), shared with `fetched` and
       engine/trusted.mjs. This seam can answer it only with the page's `error` event, which no browser would
       produce for a POST, so it aborts in dev; the release arm below answers the network error. */
    const _xhrMethodRefusal = self.safeFetchMethodRefusal(q.method);
    if (_xhrMethodRefusal) {
      DFAIL("an XMLHttpRequest was DECLINED by the chokepoint (" + _xhrMethodRefusal.reason + ") and this " +
            "seam can only answer it with §3.5.6 \"The send() method\"'s network error — the page's `error` " +
            "event, for a request this zone chose never to send. The address is DERIVED IN FULL and REPORTED " +
            "(§Attacker-sources: that is not a gap in the report, it IS the report), and what is missing is " +
            "the way to say so to the FLOW: an `xhr.send` is a HOST REQUEST, so a decline means HOLDING it " +
            "open as a relayed `perform` is held, with the flow SUSPENDED at the `send()` line rather than " +
            "resumed down its failure path");
      return { meta: { status: 0, statusText: _xhrMethodRefusal.reason, headers: [] }, bytes: null };
    }
    try {
      const abs = new URL(q.url, msg.sourceUrl).href;
      /* The credentials mode is part of the request's identity too, refused rather than downgraded. XHR
         §3.5.6 The send() method sets it from §3.5.4 The withCredentials getter and setter (`include` if
         cross-origin credentials is true, otherwise `same-origin`), which xml_http_request.c writes on the
         record. safeFetch's fetch here is uncredentialed, and for a cross-origin `include` request that is a
         different fetch: Fetch's CORS check under `include` refuses `Access-Control-Allow-Origin: *` and needs
         `Access-Control-Allow-Credentials: true`, so it is refused with `blocked-credentialed-cross-origin`,
         which the page sees as the `error` event a browser would also give. Same-origin `include` and
         `same-origin` attach the same cookies and are not refused.
         The page's `withCredentials` never turns credentials on: `q.credentials` is relayed as
         `opts.credentials` (Fetch §2.2.5 Requests' credentials mode, a statement about the request) and
         safe-fetch.js composes it with `credentialed`, so it can refuse and never grant. */
      DCHECK(q.credentials === "include" || q.credentials === "same-origin",
             "the engine's xhr.send record names no credentials mode this zone speaks (`" + q.credentials +
             "`) — XHR §3.5.6 The send() method computes exactly two, and xhr_request_op writes one on every " +
             "record. A request whose credentials mode is unknown cannot be refused OR issued, and answering " +
             "it uncredentialed would hand the page a response for a request it did not make");
      if (q.credentials === "include" && new URL(abs).origin !== new URL(msg.sourceUrl).origin)
        return { meta: { status: 0, statusText: "blocked-credentialed-cross-origin", headers: [] },
                 bytes: null };
      /* @security-finding: `headers` come from the untrusted bundle and are read by the chokepoint. That is
         within the model for an uncredentialed GET to a public host (the browser strips forbidden header names),
         and must be re-decided the day this path is credentialed. Only what the chokepoint reads is passed. */
      /* An XMLHttpRequest's destination is the empty string (Fetch §2.2.5 Requests), stated so its CORB class
         is not decided by silence. */
      /* What the request is evidence of: the engine composes it at the record (`xhr_request_op` →
         `engine_provenance_of_running_path`), `derived` or `forced` and never `observed`, since no parser
         inserted an XHR (HTML §4.12.1 The script element). Asserted, never defaulted: `derived` for a record that
         stopped stating one could fire a request whose values exist only past a gate. The chokepoint CHECKs the
         vocabulary again in release; this names the producer. */
      DCHECK(q.provenance === PROVENANCE_OBSERVED || q.provenance === PROVENANCE_DERIVED ||
             q.provenance === PROVENANCE_FORCED,
             "the engine's xhr.send record states the provenance `" + q.provenance + "`, which is none of " +
             "the three solver/engine.h declares — xhr_request_op writes one on every record from " +
             "engine_provenance_of_running_path, and the firing decision is made from it, so an absent or " +
             "unknown one is a producer that stopped stating what its request is evidence of");
      /* `credentials` is relayed (see above) and `credentialed: false` is stated as a literal, since the egress
         surface grades the `cookies` row `certain` and the door asserts every caller states one. */
      /* The witness mark is relayed off the record: `xhr_request_op` writes `pinned` from
         `engine_pinned_of_running_path` beside the grade. `in`, never `||`: an absent key is an older build (this
         JS ships before the engine is rebuilt), and the default a `||` would supply, `unpinned`, is the word
         safe-fetch.js's value arm fires on. An absent key is answered `unstated`, a word the engine never spells.
         The vocabulary assert sits inside the present arm, because `unstated` is not one of the engine's two
         tokens and the assert must not judge the absence the `in` test already handled. */
      var _xhrPinned;
      if (!("pinned" in q)) {
        _xhrPinned = "unstated";
      } else {
        DCHECK(q.pinned === PINNED_YES || q.pinned === PINNED_NO,
               "the engine's xhr.send record states the witness mark `" + q.pinned + "`, which is neither " +
               "token solver/engine.h declares — `xhr_request_op` writes one through `engine_pinned_token`, " +
               "whose fall-through is fatal in release, so anything else here is that mapping and this " +
               "reader having parted. The firing decision is made from this field and `unpinned` is the word " +
               "it FIRES on, so a mark nothing can be read as may not be relayed as one");
        _xhrPinned = q.pinned;
      }
      /* `docReach` is the document's own reach grade, for the reason `fetched` gives. */
      /* `actor: "page"`: every XHR is made by running the page's code, the same population as the `fetch()`
         relay. With a real witness mark, an XHR whose path pinned nothing fires at an origin the person
         permitted for that row, and one whose path pinned a witness is refused, as such a `fetch()` is. */
      const r = await self.safeFetch(abs, { pageUrl: msg.sourceUrl, destination: "",
                                            provenance: q.provenance, pinned: _xhrPinned,
                                            docReach: msg.provenance, actor: "page",
                                            credentialed: false,
                                            credentials: q.credentials, headers: q.headers });
      DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && typeof r.status === "number" &&
             r.headers && typeof r.headers === "object",
             "safeFetch answered an XHR with something other than its reply record — §3.5.6's response is " +
             "built from it and the page reads status, statusText and every header off that, and §2.2.4's " +
             "body source is a BYTE SEQUENCE");
      /* The status message is asserted for `fetched`'s reason: "" is a legitimate HTTP/2 answer and the
         refusal arms carry their reason there. */
      DCHECK(typeof r.statusText === "string",
             "safeFetch answered an XHR with no statusText — XMLHttpRequest §3.6.3 `The statusText getter`'s `statusText` getter is " +
             "read straight off it, and the chokepoint writes one on every path including the refusals whose " +
             "reason it is");
      /* The bytes cross beside the record, for `fetched`'s reason: XHR §3.6.6 Response body decodes the received
         bytes with the final encoding, and `responseType = "arraybuffer"` needs the server's own bytes. */
      /* A decline is graded but cannot yet be acted on: XHR §3.5.6 The send() method's handle errors turns a
         status-0 record into the page's `error` event, right where a browser also fails and false where this
         zone declined. There is no decline channel: an `xhr.send` is a host request answered by
         `engineAnswer(eng, id, …)`, and an unanswered one is re-reported by `qjs_host_requests` every step, so
         declining by silence would spin. The DFAIL names what to build. */
      if (r.refusal) {
        DCHECK(r.refusal.kind === "network" || r.refusal.kind === "decline",
               "safeFetch graded an XHR's refusal `" + r.refusal.kind + "`, which is neither word it states");
        if (r.refusal.kind === "decline")
          DFAIL("an XMLHttpRequest was DECLINED by the chokepoint (" + r.refusal.reason + ") and this seam " +
                "can only answer it with §3.5.6 \"The send() method\"'s network error — the page's `error` " +
                "event, " +
                "which tells the flow the server was unreachable for a request this zone chose never to send, " +
                "and hands every branch under `onerror` a fact about the origin nothing observed. BUILD the " +
                "decline: an `xhr.send` is a HOST REQUEST, so declining it means HOLDING it open the way a " +
                "relayed `perform` is held, with the flow SUSPENDED at the line the page wrote `send()` on — " +
                "not answered, and not re-reported into a spin by qjs_host_requests");
      }
      /* The type the chokepoint computed, relayed as `fetched` does: this zone is the only one that may answer
         it (safeFetch saw the body and ran the sniff), and solver/reply_decode.c reads `computedType` rather than
         re-deriving a type. "" is MIME Sniffing §5.1's undefined supplied type, a positive answer. */
      DCHECK(typeof r.computedType === "string",
             "safeFetch answered an XHR with no computed content type — solver/reply_decode.c reads this " +
             "field instead of re-deriving a type from the raw header, so an absent stamp is a producer " +
             "that failed and never a resource whose type is unknown");
      return { meta: { status: r.status, statusText: r.statusText, headers: Object.entries(r.headers),
                       computedType: r.computedType },
               bytes: r.body };
    } catch (e) {
      /* A thrown fetch is the network error that becomes the page's `error` event (XHR §3.5.6 handle errors);
         an invariant abort is rethrown. No `body` key in the record: `bytes: null` states there are none, and a
         second spelling is what `fetch_reply_set_body` DCHECKs against. */
      RETHROW_FATAL(e);
      return { meta: { status: 0, statusText: "", headers: [] }, bytes: null };
    }
  };
  /* The principal is browser-stated, never parsed off the address: a page that sandboxes its own iframe gives
     that document an opaque origin while its address looks ordinary. _dispatchDocument hands over
     `_senderOrigin(sender)` (from MessageSender) as `msg.origin`, and hostNotice stamps it on every message the
     document posts. A document this zone provisioned carries the origin of the URL this zone fetched; "" is a
     rehydrated recipe predating the field, and the stamp site refuses it. */
  /* `_resolvers` is a list because one document may be asked for more than once while one instance holds it
     (the RESHIP re-delivery); the one engine's finalize answers every waiting caller. */
  /* `cluster` is this instance's identity and `docId` only the document it was rooted at: an instance is an
     origin-keyed agent cluster holding one realm per same-origin document (main.c's `engine_child_realm`).
     `groupId` is kept beside the key because a document this instance creates inherits its group. */
  /* `_remoteAsked` holds the request ids of cross-agent operations already carried to the peer that holds the
     object. It is per instance because an unanswered request is re-reported by qjs_host_requests every step,
     and asking again would run the peer's operation, with the page's side effects, once per step. */
  /* `r` is the instance. A pointer an ABI call retained lives as long as the module, which dies with the frame
     removed at finalize, so nothing in this realm is freed by hand or can read the instance's memory. */
  eng.lines = lines; eng.fkey = fkey; eng.prior = prior; eng.persist = persist;
  eng.fetched = fetched; eng.fetchedDocument = fetchedDocument; eng.fetchedXhr = fetchedXhr;
  eng.code = code; eng.html = html; eng._forceparkSteps = _forceparkSteps;
  /* The document's reach grade, asserted at the one door every instance passes, because the three closures
     above state it on every request; failing here names the composer (ambient seed, declared route, child
     navigable, swapped-to document, cold rehydration) rather than the chokepoint at whichever park runs
     first. It applies to runs that persist nothing too, since their requests still fire by it. */
  DCHECK(msg.provenance === PROVENANCE_OBSERVED || msg.provenance === PROVENANCE_DERIVED ||
         msg.provenance === PROVENANCE_FORCED,
         "a document is being rooted with the reach grade `" + String(msg.provenance) + "`, which is none of " +
         "CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's three — every composer of an AST_ANALYZE states one " +
         "(the ambient seed `observed`, a declared route the join of the declaring document's grade and its " +
         "own word, a child navigable and a swapped-to document the same join, a rehydration the word it was " +
         "parked under), so an absent one is a SIXTH composer nobody told, and every request this document " +
         "makes would be refused at the chokepoint under a word it never stated");
  DCHECK(eng.msg === msg,
         "the reservation this instance is being rooted into carries a different message record than the one " +
         "this document was parsed, fetched and keyed from — the reservation's is what hostNotice stamps a " +
         "cross-document delivery's origin from and what finish() writes the cold recipe out of, so two " +
         "records for one document is a message stamped with one document's principal and persisted under " +
         "another's address");
  /* The first round's record, taken before this instance is rankable: `qjs_begin` above seeded the frontier, so
     a top-flow weight now exists, and the facts are written before the state that makes them readable, so the
     ranking and RAM floor never see a hot engine that has not reported. */
  await engineRecordFacts(eng);
  eng.state = "hot";
}
/* A second document of an agent cluster joins the instance already running it, with the same arguments
   `qjs_init` takes. It adds a Lexbor tree in the agent's arenas, a realm through main.c's engine_child_realm,
   and that document's scripts seeded on the one frontier; a second instance would be the two-heap split
   SECURITY.md's (browsing-context group, origin) key forbids.
   The instance must already be rooted and seeded (`qjs_join` asserts `g_dom` and `g_begun`), or the document
   would never run a line; a caller holding a reservation waits for it, and this asserts the state.
   The same document may not arrive twice: the engine interns documents by name (main.c's `world_doc_hosted`
   assert), and two spellings of one document would pass that, so the name is asserted unheld across the
   whole pool. */
/* The document's bytes are a parameter, not a field of `msg`: a peer-announced child arrives on a message this
   zone built with `pageHtml`, while a tab navigation arrives on the ambient AST_ANALYZE message, which asserts
   `pageHtml === undefined` (a seed is an address), with its bytes on the waiting job. As a parameter the
   operation takes its input with it, the assert stands on what the caller said, and the shape matches
   engineCreate's. */
async function engineJoin(eng, html, msg, docName, topLevelUrl, inherited, parentNavigable, containerPolicy,
                          ancestorOrigins, creationSandboxFlags) {
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
  /* The parent navigable, on engineCreate's rule. A same-origin document nested through a cross-origin one
     belongs to this cluster and to another instance's frame tree at once, and only this zone can name its
     parent. */
  DCHECK(typeof parentNavigable === "string" && parentNavigable !== "",
         "a document was joined with no HTML §7.3.1.3 PARENT NAVIGABLE — a navigable either has a parent or " +
         "is a top-level traversable and both are facts a host states, so an absent one is a caller that " +
         "skipped the question and a nested document that would answer `parent === self` inside the only " +
         "instance that holds it");
  /* The container answer, on engineCreate's rule: for such a document both HTML §7.3.1.3 links are in another
     instance, so only that instance could compute Permissions Policy §9.5's answer. */
  DCHECK(typeof containerPolicy === "string" && containerPolicy !== "",
         "a document was joined with no HTML §7.3.1.3 CONTAINER statement — §9.5's answer for its navigable " +
         "and `null` are the two facts a host states, so an absent one is a caller that skipped the question " +
         "and a nested document granted every feature its embedder holds, through §9.7 step 1's null-container "+
         "arm");
  /* The ancestor origins, on engineCreate's rule: every ancestor of such a document is in another instance,
     which alone can read HTML §3.1.3's inputs. */
  DCHECK(typeof ancestorOrigins === "string" && ancestorOrigins !== "",
         "a document was joined with no HTML §3.1.3 ANCESTOR ORIGINS statement — the composed list and " +
         "`none` are the two facts a host states, so an absent one is a caller that skipped the question and " +
         "a nested document that would answer `location.ancestorOrigins` with `[]` while sitting inside " +
         "somebody else's frame tree");
  /* The creation sandboxing flags, on the same rule. A join is reachable with a sandbox: `<iframe
     sandbox="allow-same-origin allow-scripts" src="https://cdn/…">` keeps the child's tuple origin (so it
     clusters with that host's documents) while most HTML §7.1.5 Sandboxing flags stay set. The sandboxed
     origin flag cannot arrive here (it forces an opaque origin that clusters with nothing); main.c asserts
     that. */
  DCHECK(typeof creationSandboxFlags === "string" && creationSandboxFlags !== "",
         "a document was joined with no HTML §7.1.5 CREATION SANDBOXING FLAG SET — the composed set and " +
         "`none` are the two facts a host states, so an absent one is a caller that skipped the question and " +
         "a sandboxed frame joined with its scripts, its forms and its `document.domain` unsandboxed");
  /* The same shape the root's document takes: `content.mojom.Renderer.Join` declares `array<uint8>` because
     `qjs_join` has main.c's byte-identical signature, so both operations take one contract. */
  DCHECK(html instanceof Uint8Array || typeof html === "string",
         "a document was joined with no BYTES — every live document is navigationLoad's response body and a " +
         "cold-tier one may hold characters, so anything else is a document this agent would hold as nothing " +
         "at all. It is the CALLER'S to state: a peer engine's create notice carries it on the message this " +
         "zone built, and a tab navigation carries it on the waiting job (`_waiting`'s `html`), never on the " +
         "AST_ANALYZE message — whose own entry asserts `pageHtml === undefined` because a seed is an address");
  const _doc = html instanceof Uint8Array ? html : new TextEncoder().encode(html);
  /* A NUL in a joined document is input, as for the root: `qjs_join` takes `(bytes, len)` and the tokenizer
     applies its per-state rule (HTML §13.2.3.5 Preprocessing the input stream). */
  /* The clone of the creator's container, off the create notice that sent this document. The pair is empty
     where there is no creator: a cross-document navigation of a top-level traversable (HTML §7.4.6.1
     Updating the traversable) joins a Document whose container comes from its own response. There is no
     `null` arm: engineRoot takes one for a rehydrated recipe with no message, and every caller here has one. */
  DCHECK(!!inherited && typeof inherited.csp === "string" && typeof inherited.selfOrigin === "string" &&
         (inherited.selfOrigin !== "" || inherited.csp === "") && embedderPolicyWhole(inherited.embedder),
         "a document was joined with a CSP list and no SELF-ORIGIN for it — CSP §2.2 \"Policies\" makes a list " +
         "a struct of policies AND the origin `'self'` resolves against, so a policy with no self-origin is " +
         "half a container and §6.7.2.8 would answer one directive backwards. BOTH EMPTY IS A REAL STATE AND " +
         "THIS ASSERT USED TO REFUSE IT: it demanded a creator on the ground that `every join in this zone " +
         "comes off a navigable.create`, and that stopped being true the moment a CROSS-DOCUMENT NAVIGATION " +
         "of a top-level traversable joined its incoming Document here (HTML §7.4.6.1 \"Updating the " +
         "traversable\"). That Document has NO creator — HTML §7.1.7 \"Policy containers\" clones a creator's " +
         "container only where there is one, and this one's is created from its OWN response — so the empty " +
         "pair is the positive statement the engine already reads it as, and demanding a creator would have " +
         "made this zone invent one");
  const _join = await eng.r.renderer.join({
    document: _doc, url: msg.sourceUrl, docId: docName,
    headers: responseFieldLines(msg.responseHeaders), topLevelUrl,
    inheritedCsp: inherited.csp, inheritedCspSelfOrigin: inherited.selfOrigin,
    inheritedCoep: inherited.embedder.value, inheritedCoepEndpoint: inherited.embedder.endpoint,
    inheritedCoepReportOnly: inherited.embedder.reportOnlyValue,
    inheritedCoepReportOnlyEndpoint: inherited.embedder.reportOnlyEndpoint,
    parentNavigable, containerPolicy, ancestorOrigins, creationSandboxFlags });
  DCHECK(_join.rc === 0,
         "qjs_join reported a failure this zone has no handling for — the engine's own entry CHECKs " +
         "every precondition and aborts, so a non-zero return is a contract that changed");
  /* The name enters the routing table only once the engine holds it; a delivery in between is refused by the
     post branch's assert rather than routed to an agent that has never heard the name. */
  eng.joinedDocIds.push(docName);
  /* The round ends here, so it records: a join copies a whole document in and seeds its scripts as flows, so
     both Level-1 facts moved. */
  await engineRecordFacts(eng);
}
/* The document a navigation replaced: HTML §7.4.6.1 Updating the traversable deactivates the old document for
   a cross-document navigation, the other half of engineJoin. Both halves are told to the instance in the
   standard's order (the incoming Document exists first). It is not §7.3.1.6 Navigable destruction: the
   navigable survives and only the active Document changes; the two meet at HTML §7.5.9 Unloading documents
   ("if oldDocument's salvageable state is false, then destroy oldDocument") and §7.5.10 Destroying documents,
   which is why the engine serves both entries from one machine.
   Only this zone knows the navigation happened, and only the browser's document id crosses. The call seeds and
   does not wait: the engine attaches the unload to every live timeline, each fires that document's `pagehide`
   and `unload` listeners under its own delta when next scheduled (an unload `sendBeacon` is an endpoint), and
   nothing is dropped. */
async function engineUnload(eng, docName, incomingDocName) {
  DCHECK(eng.state === "hot" || eng.state === "fetching",
         "a document was reported replaced to an instance in state `" + eng.state + "` — §7.5.9's unload is a " +
         "task of every timeline of that document, so an instance whose frontier is not yet seeded has none " +
         "to attach it to and the destruction would be queued onto nothing");
  DCHECK(typeof docName === "string" && docName !== "",
         "a document was reported replaced with no NAME — an instance is an ORIGIN-KEYED AGENT CLUSTER and " +
         "holds one realm per same-origin document, so the name is the whole of what says which of them the " +
         "browser navigated away from");
  DCHECK(eng.docId === docName || eng.joinedDocIds.indexOf(docName) >= 0,
         "a document this instance never held was reported replaced (" + docName + ") — this zone is what " +
         "routes a document to an instance and what records which documents each one holds, so a name that " +
         "is in neither `docId` nor `joinedDocIds` is this table and the agent disagreeing about what is in " +
         "the agent, and the engine's own `world_doc_hosted` assert is what it would reach");
  /* The order of the two halves is checked here, by the party that performed the navigation. The engine does
     not check it and must not: a cross-origin incoming Document loads into a peer instance, so the agent asked
     to unload never holds it. The engine takes only the outgoing name, the realm HTML §7.5.9 Unloading
     documents queues the operation in. */
  DCHECK(typeof incomingDocName === "string" && incomingDocName !== "" && incomingDocName !== docName,
         "a navigation was reported with no INCOMING document, or with one document named as both halves — " +
         "HTML §7.4.6.1 replaces a navigable's active document WITH ANOTHER, so a report with only an " +
         "outgoing half is a destruction wearing a navigation's name, and one naming a single document as " +
         "both halves is a navigation that did not happen");
  DCHECK(eng.joinedDocIds.indexOf(incomingDocName) >= 0,
         "a navigation named an INCOMING document this instance has not joined (" + incomingDocName + ") — " +
         "engineJoin is what puts it in the agent and is what pushes the name here, so this is the two halves " +
         "of one navigation called in the wrong order");
  /* The name stays in `joinedDocIds`: HTML §7.5.10 destroys the document, not the realm, so the engine still
     interns the name and a peer's route must resolve here to an instance that reports a destroyed navigable
     (what HTML §7.2.2.1's `closed` reads). Removing it would let the next arrival re-join that name and hit
     the engine's one-realm-per-document CHECK. */
  await eng.r.renderer.unload({ docId: docName });
  /* The round ends here, so it records: an unload attaches a job to every live timeline. */
  await engineRecordFacts(eng);
}
/* The Level-1 ranking's two facts (top-flow weight and working set), recorded at the end of every round this
   zone has with an instance (seed, step, service), because an instance behind the frame boundary answers only
   by message and there is no value to read on the line that ranks. hostSchedule ranks, advances the winner and
   re-ranks, so the engine that just worked has the freshest numbers and one that did none has numbers that
   cannot have moved.
   There is no unvisited arm: solver/flow.c's flow_weight gives a never-run flow its reward plus the full 1.0
   bonus and qjs_top_weight reports it, so Level 1 inherits Level 2's optimism (one WFQ at both levels);
   qjs_top_weight DCHECKs `g_begun`, which qjs_begin sets. */
async function engineRecordFacts(eng) {
  /* -Infinity is an answer: `engine_top_weight` is `flow_next_to_run(NULL) ? flow_weight(b) : -1.0/0.0`
     (solver/engine.c), the engine's statement that no flow is runnable, and it sorts below every real weight
     by arithmetic, so a drained engine ranks last and its next step retires it through DONE. NaN and +Infinity
     are broken and asserted: every comparison against NaN is false (the top flow would never yield), and no
     flow_weight produces +Infinity. */
  const _rank = await eng.r.renderer.getTopWeight();
  const w = _rank.weight;
  DCHECK(Number.isFinite(w) || w === -Infinity,
         "the engine answered a top-flow weight of " + w + " — a weight is either a finite number or the " +
         "-Infinity that says its frontier holds no runnable flow, and NaN or +Infinity is neither: every " +
         "Level-1 ranking comparison against a NaN is false, so the pool would pick by array order and call " +
         "it value-of-information");
  eng.topWeight = w;
  /* The working set as the frame stated it on the call above: `workingSetBytes` is a declared reply field of
     every `content.mojom.Renderer` method, and only a call can grow the memory, so the reply just awaited is
     the freshest value. WASM memory is a whole number of 64 KiB pages. */
  const b = _rank.workingSetBytes;
  DCHECK(Number.isInteger(b) && b > 0 && b % 65536 === 0,
         "an instance reported a working set that is not a whole number of WASM pages (" + b + ") — HEAPU8 is " +
         "the view over the entire linear memory, which is allocated and grown in 64 KiB pages, so a length " +
         "outside that is a view onto something other than this engine's memory");
  eng.residentBytes = b;
}
/* The Level-1 WFQ's one input, read off the record. The finite-or-`-Infinity` shape is asserted where the
   engine answers it; here only presence and state are. A non-hot engine is not ranked at all: hostSchedule
   ranks the hot set it filtered, and answering `-Infinity` (the engine's word for a drained frontier) for a
   reservation would send a booting engine down the DONE path. */
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
/* One delivery, both channels, so no call site carries the record and forgets the bytes. `rep` is what
   `fetched` answered: `null` for a network error (the JSON `null` the engine rejects with a TypeError), or
   `{meta, bytes}`. The renderer places the bytes in linear memory (one extra byte so an empty body still has
   an address; `null` means no body) and frees them after the call, as the mojom declares the body not
   retained.
   The bytes are copied, not transferred: mojo.js builds a transfer list from declared types, Mojo's
   `array<uint8>` is a copied byte sequence, and a moved buffer would be `mojo_base.mojom.BigBuffer`. A move is
   therefore a second declared type, never a flag on this call, because `Init`'s document has the same
   spelling and its bytes this zone retains. */
/* Delivered against the request, which is the pair: `qjs_provide` takes the method first, as a request line
   states it (RFC 9112 §3 Request Line), because the engine's pending register is keyed on both, so a GET and a
   POST to one address cannot collect each other's bodies. */
async function engineProvide(eng, method, url, rep) {
  /* A decline may not arrive here: `provide` settles the park, and `reply: "null"` settles it with a network
     error. A decline is delivered through engineDecline, which keeps the flow parked. */
  DCHECK(!(rep && rep.refusal),
         "a DECLINED request reached the delivery seam — `provide` settles the park, and settling it with a " +
         "network error tells the flow the server was unreachable for a request this zone chose not to send. " +
         "A decline is delivered by NOT calling this: the flow stays parked, which is what §@S requires of a " +
         "search not yet solved, and it fires the day the origin is widened. Refusal: " +
         String(rep && rep.refusal && rep.refusal.reason));
  DCHECK(rep === null || (rep && typeof rep === "object" && rep.meta && rep.bytes instanceof Uint8Array),
         "`fetched` answered neither §5.6's network error (null) nor a reply — a reply is its JSON metadata " +
         "and its BYTES together, and a record arriving without one of the two is half a response");
  await eng.r.renderer.provide({ method, url, reply: JSON.stringify(rep === null ? null : rep.meta),
                                 body: rep === null ? null : rep.bytes });
}
/* The other answer on the same pair: this zone stating it will not make the request. It is not a delivery
   with a different payload, which is why engineProvide refuses a refusal. The engine keeps the flow's park and
   forks an arm that runs the page's `catch`, so the wait is preserved and the failure path explored. No bytes
   accompany it, since nothing was fetched. */
async function engineDecline(eng, method, url, reason) {
  DCHECK(typeof method === "string" && method !== "" && typeof url === "string" && url !== "",
         "a refusal was relayed naming no request — it is keyed on the (method, url) pair exactly as a reply " +
         "is, because it answers the same question, and a half-named one refuses a request no flow parked on");
  DCHECK(typeof reason === "string" && reason !== "",
         "a refusal was relayed with no reason — the flow it refuses will not drain this session, so this is " +
         "the only account of it anybody gets and it is what says whether a widening would change the answer");
  await eng.r.renderer.decline({ method, url, reason });
}
/* One pending line, split in one place, mirroring the engine's `engine_pending_split` so the grammar cannot
   drift across hosts. The TAB delimiter is unambiguous: URL Standard §4.4 URL parsing strips every ASCII tab
   or newline from its input first, and a method is a token (Fetch §2.2.1 Methods) whose tchar (RFC 9110 §5.6.2
   Tokens) excludes HTAB. It is a CHECK, as the engine's splitter is, because the release path has no defined
   answer: a mis-split line would be delivered against a method nobody asked for, and never against an assumed
   `GET`. */
/* It splits on the TAB rather than counting indexes because the destination field is legitimately empty
   (Fetch §2.2.5 Requests: "" for a `fetch()`), so the shape assert is over the field count and the two ends. */
function pendingRequest(line) {
  const f = line.split("\t");
  CHECK(f.length === 7 && f[0] !== "" && f[6] !== "",
        "content.mojom.Renderer.GetPending answered a line that is not " +
        "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL`: `" + line + "` — the " +
        "engine joins the seven and this zone must deliver against the (method, url) pair, so a line missing a " +
        "field puts a token where the address belongs and keys a reply on a request nothing parked on");
  const destination = f[1];
  const initiator = f[2];
  const provenance = f[3];
  /* Whether the address may rest on a witness the engine chose: solver/engine.h's `pinned`/`unpinned`,
     composed at the park from solver/flow.h's `path_pinned`. Relayed; safe-fetch.js decides from it with the
     destination. */
  const pinned = f[4];
  const credentials = f[5];
  /* The destination (Fetch §2.2.5 Requests) is what this zone acts on: safe-fetch.js decides the CORB class
     with §2.2.5's script-like predicate over it. The initiator is HTML §4.12.1.1 Processing model's
     parser-inserted (solver/engine.h), whether a real load of the document makes this request; nothing here
     branches on it. The provenance (observed, derived, forced) is composed at the park from that flag and the
     parking flow's `path_forced`; the per-origin firing decision is safe-fetch.js's, never this zone's or the
     engine's.
     Membership checks here cover the small fixed vocabularies. The destination's enumeration is refused where
     the decision is made: safe-fetch.js's `_destinationOf` CHECKs it before the request goes out (its
     `_isScriptLike` answers false for an unknown value, so an invented token would take the permissive arm),
     and engine/trusted.mjs loads that file verbatim, so one check covers both hosts. The engine's own asserts
     on this field are DCHECKs on the untrusted side and cannot be relied on. */
  CHECK(initiator === "parser" || initiator === "script",
        "GetPending stated an initiator this zone does not know: `" + initiator + "` — solver/engine.h " +
        "declares exactly `parser` and `script`, and a third value would be answered by whichever arm a " +
        "consumer happened to write first");
  CHECK(provenance === "observed" || provenance === "derived" || provenance === "forced",
        "GetPending stated a provenance this zone does not know: `" + provenance + "` — solver/engine.h " +
        "declares exactly `observed`, `derived` and `forced`, and a fourth value would be answered by " +
        "whichever arm the firing policy happens to be written with as its else");
  CHECK(pinned === "pinned" || pinned === "unpinned",
        "GetPending stated a witness mark this zone does not know: `" + pinned + "` — solver/engine.h " +
        "declares exactly `pinned` and `unpinned` on this line, and the firing policy reads it to decide " +
        "whether an ACT MAY BE SPENT on an address that may be one this engine composed, so an unknown word " +
        "would take that policy's permissive arm. `unstated` is safe-fetch.js's word for an act that carries " +
        "no mark at all and is never one a pending line may state");
  /* The credentials mode (Fetch §2.2.5 Requests) says whose session pays. Only the algorithm that created the
     request knows it (an `<img>`'s potential-CORS request, a `<script src>`'s CORS settings attribute, a
     `fetch()`'s `RequestInit`), so it is relayed, never re-derived; its membership is checked at the deciding
     door (`_credentialedOf`), one check for both hosts. Here only the line's shape is asserted. */
  return { method: f[0], destination, initiator, provenance, pinned, credentials, url: f[6] };
}
/* The door that does not await a body with no end. A round used to wait on each pending fetch in turn, and a
   response the server never ends (one `fetch()` or XHR to an endpoint that holds its body open) kept the
   document out of the rankable set for the session. No deadline is added: a timeout would truncate a
   legitimate response into a wrong answer, so the door changes instead.
   The shape is engine/host/wpt_runner.c's `g_inflight` table (`wpt_issue_pending` issues without waiting;
   `wpt_request_asked` refuses to re-issue a key in flight), and engine/trusted.mjs's `track`/`answered` pair
   is the same table. A record stays until its reply is delivered, not until bytes land: a landed but
   undelivered request is still on the engine's pending list and would otherwise be re-issued next round.
   Two key namespaces, as in wpt_runner.c: a network park is `(method, url)` and a host request is its id,
   kept apart by prefixes, as `cold:` and `seed:` prefix group ids, so an id spelled like a method cannot dedup
   against the wrong request. */
function engineIssue(eng, key, start, deliver) {
  DCHECK(typeof key === "string" && key !== "",
         "a request was issued with no in-flight key — the key is what refuses to re-issue a park the engine " +
         "re-lists every round, so an unkeyed issue is one request per round at somebody's server");
  DCHECK(typeof start === "function" && typeof deliver === "function",
         "a request was issued with no starter or no deliverer — the pair is the whole contract: `start` is " +
         "called exactly once and only when this key is not already in flight, and `deliver` is what a later " +
         "round hands the answer to, so an issue missing either is a request nobody will ever answer");
  if (eng._inflight.has(key)) return 0;
  /* The record enters the map before the call suspends, so the next round, re-listing the same request,
     sees it in flight. */
  const rec = { landed: false, value: undefined, error: null, settled: null, deliver };
  eng._inflight.set(key, rec);
  /* `settled` never rejects; the error is carried and rethrown at delivery inside the round, so an invariant
     abort out of `eng.fetched` still reaches hostSchedule's failure arm and there is no unhandled rejection. */
  rec.settled = start().then(
    (v) => { rec.landed = true; rec.value = v; },
    (e) => { rec.landed = true; rec.error = e; });
  /* The round can wait on it (hostSchedule's wait arm relies on that). It fails only if `start` threw
     synchronously, which none of the callers can, since each returns an async function's promise. */
  DCHECK(rec.settled && typeof rec.settled.then === "function",
         "a request was issued whose starter did not answer a promise — the round waits on this when the " +
         "frontier has nothing runnable, and a non-promise there resolves the wait immediately and turns the " +
         "stall arm into the full-speed spin it exists to prevent");
  return 1;
}
/* The delivering half, which the engine's park is waiting for. It returns how many registers it filled,
   progress like `wpt_net_pump`'s return. The record leaves the in-flight set here, not when bytes land, so a
   landed reply not yet handed over still refuses a re-issue. */
async function engineDeliverLanded(eng) {
  let n = 0;
  for (const key of [...eng._inflight.keys()]) {
    const rec = eng._inflight.get(key);
    if (!rec.landed) continue;
    eng._inflight.delete(key);
    if (rec.error) throw rec.error;
    await rec.deliver(rec.value);
    n++;
  }
  return n;
}
/* One round: issue what the engine newly parked on, act on what it owes, deliver what has landed; the engine
   is rankable again as soon as these finish. The final wait fires only when `qjs_step` answered
   ENGINE_STEP_STALLED (nothing runnable at all) and the round made no progress, so it starves no flow and
   replaces a full-speed spin through `macroYield`. It is unbounded, never a deadline (as `wpt_net_pump(ctx,
   1)`): the waiting document is out of the hot set, so every other engine keeps stepping. */
async function engineServiceFetch(eng, stalled) {   // one round: issue, act, deliver — never await a body
  DCHECK(typeof stalled === "boolean",
         "a service round was driven without the engine's own stall statement — `qjs_step` answers " +
         "ENGINE_STEP_STALLED for a frontier holding nothing runnable, and that is the ONLY state in which " +
         "this round may wait on bytes; inferring it here would be a second answer to a question the engine " +
         "already answered, and defaulting it either spins or freezes");
  let did = await engineIssuePending(eng);
  did += await engineServiceHostRequests(eng);
  did += await engineDeliverLanded(eng);
  if (stalled && did === 0 && eng._inflight.size > 0) {
    await Promise.race([...eng._inflight.values()].map((r) => r.settled));
    await engineDeliverLanded(eng);
  }
}
async function engineIssuePending(eng) {   // issue every parked request; a later round delivers the answer
  /* The reply's metadata crosses as JSON text (as qjs_host_answer's does), so it can say `null` and carry the
     URL list, status and headers; its body crosses as bytes beside it, since decoding it (Fetch §5.3 Body
     mixin's `text()`) would destroy what the engine's own decode reads. */
  /* There is one owed list and each line carries its own class: the destination (Fetch §2.2.5 Requests) on the
     pending line decides whether a reply is code, via safe-fetch.js's script-like predicate, so a document's
     own `<script src>` and an injected one are classified like a dynamic `import()`. A fact about a request
     belongs on the request. */
  const requests = owedList("GetPending", (await eng.r.renderer.getPending()).requests);
  /* The provenance travels with its request to safe-fetch.js, the sole owner of the firing decision. */
  /* How many requests this round started, as `wpt_issue_pending` returns: progress, so the round has something
     outstanding to wait on. */
  let issued = 0;
  for (const line of requests) {
    const { method, destination, provenance, pinned, credentials, url } = pendingRequest(line);
    /* The ask is counted at the call, before the gate, so the census separates a request nobody made from one
       the gate refused: `declined: {}` beside a nonzero `asked` says the policy refused nothing (an empty
       surface is then about the driving), while `asked: 0` says this loop never ran. */
    /* The dedup key is the pair the engine parked on, the key the answer is delivered under (`engine_provide`
       and `engine_decline` key on it, and `engine_pending_fetches` dedups over it). It uses the raw address off
       the line, never the absolute URL `fetched` resolves. */
    const key = "fetch\n" + method + "\n" + url;
    /* An entry stays listed until answered and this visits the list every slice, so without the dedup one
       page `fetch` would become one request per round. */
    if (eng._inflight.has(key)) continue;
    /* The ask is raised inside the starter, which `engineIssue` invokes only for a key not in flight, so a
       re-listed request is counted once and the containment with the declined histogram holds. */
    issued += engineIssue(eng, key,
      () => { eng._egress.asked++;
              return eng.fetched(method, url, destination, provenance, pinned, credentials); },
      (answer) => engineDeliverReply(eng, method, url, answer));
  }
  return issued;
}
/* What a landed answer means on the pending seam: the `deliver` half of one issue, closing over the request's
   own pair, so the reply cannot be handed over against a pair it was not fetched under. */
async function engineDeliverReply(eng, method, url, answer) {
  /* A decline is its own delivery, on its own method. `Decline` carries the same pair `Provide` does with the
     chokepoint's reason: the engine records the refusal, stops listing the request (silence would be
     re-offered and re-refused every round), keeps one arm waiting with no reply invented, and forks the other
     to run the page's failure path marked forced, since a declined request's outcome is unobserved and both
     arms are feasible. The reason names either a row of the person's own control that would make it fire
     (`blocked-signal:cookies=yes`) or a refusal nothing reopens (`blocked-destructive:logout`); the address
     is derived in full and reported. */
  if (answer !== null && answer.refusal) {
    DCHECK(typeof answer.refusal.reason === "string" && answer.refusal.reason !== "",
           "a declined request carries no reason — the reason is the only account a person gets of a park " +
           "this zone will not pay, and an unnamed one leaves a frontier stalled with nothing to read");
    DCHECK(answer.refusal.kind === "decline",
           "a refusal graded `" + answer.refusal.kind + "` reached the decline seam — `network` is a " +
           "refusal a real browser performing this same request also makes, so it is Fetch §5.6 \"Fetch " +
           "methods\"' network error and belongs on `Provide` as the JSON `null`; relaying it here would " +
           "leave the flow parked on a failure that IS a fact about the origin");
    /* It is also told to the person: nothing in the extension renders the engine's record, so this line is
       where a person reading a frontier that will not drain learns which rule holds it. */
    console.warn("[bridge] " + method + " " + url + " — " + answer.refusal.reason +
                 ". This zone DECLINED to make the request: the flow stays PARKED rather than being told " +
                 "the server was unreachable, and one arm is forked to explore the page's failure path");
    /* Counted per whole token, never parsed: safe-fetch.js composes `blocked-signal:<name>=<value>` from the
       signal it walked, so the histogram is per signal by construction, and matching `statusText` would be a
       second copy of the policy. The decline family's tokens come from closed sets (`_SIGNALS` and their
       values, `_DESTRUCTIVE`'s words, Fetch §2.2.5's destinations), so no address or origin reaches this map.
       The `network` family (refusals a browser also makes, some carrying an origin) is not counted here. A
       first occurrence is inserted, never read through a default. */
    const _tok = answer.refusal.reason;
    if (!(_tok in eng._egress.declined)) eng._egress.declined[_tok] = 0;
    eng._egress.declined[_tok]++;
    await engineDecline(eng, method, url, answer.refusal.reason);
    /* `return`: the decline is the whole delivery for this request. */
    return;
  }
  await engineProvide(eng, method, url, answer);
}
/* Every owed list crosses the same way, newline-joined records or "" for none, and is split here. The caller
   names the mojom method and mojom declares the reply's type, so a NULL cannot become the text "null" and one
   bogus record. */
function owedList(method, s) {
  DCHECK(typeof s === "string",
         "content.mojom.Renderer." + method + " answered with something that is not text — every owed list " +
         "crosses as newline-joined records and an empty list is the empty string");
  return s.split("\n").filter(Boolean);
}

/* The instance holding a document, by exact name. A child's name is prefixed by its creator's
   ("<creator>.<n>") and the creator is exactly the instance that does not hold it, so a prefix match would
   route a message back to its sender. */
/* It answers for every document the instance holds, not only its root: `qjs_join` adds documents to the live
   agent (main.c's `g_joined_ctx`/`g_joined_dom`) and the engine interns every document by name (`qjs_route`'s
   arrival assert is `world_doc_hosted(world_doc_intern(doc))`, solver/engine.c), so the two lists are one
   table on the other side of the boundary. */
function hostHolderOf(docName) {
  for (const e of _pool) {
    if (e.docId === docName) return e;
    if (e.joinedDocIds.indexOf(docName) >= 0) return e;
  }
  return null;
}

/* The instance of an agent cluster, a different question from the one above: `hostHolderOf` is routing (where
   a named document is), this is admission (which instance a new document belongs in), answered by the
   (group, origin) cluster, so a document the engine created inside that heap answers here before anything has
   named it to this zone. Asking the routing question for admission would put a same-origin pair in two
   heaps. */
function hostClusterOf(key) {
  for (const e of _pool) if (e.cluster === key) return e;
  return null;
}

/* The rendezvous for a cross-agent operation, owned by this zone because neither engine can hold it: the
   asking instance's request id is unique only inside that instance, so the token, opaque to both engines
   (main.c states that at both entries), names which instance and call site a completion belongs to.
   It is not deleted at the first completion: the holder performs the operation on every live timeline and
   each completes with its own answer, so one token names N true completions, and relaying each makes
   engine_host_answer's assert on the fork the asking flow still owes reachable. One entry per operation that
   crossed, dropped with the asking instance (engineFinalize). */
const _remoteOps = new Map();   // token -> { asker, req }
let _nextRemoteToken = 1;
/* A new browsing context group (HTML §7.1.3.2's switch creates one), as the only thing this zone needs of it:
   an id no other group has. Minted here, never derived from an engine's word, since a group id decides which
   documents share a heap. */
let _nextSwapGroup = 1;
/* The same for a declared route, a top-level traversable in a group of its own (not nested in the declaring
   document, so it may not share its heap). A separate counter from the swap's so log lines stay readable;
   `seed:` prefixes it as `cold:` prefixes the cold tier's, because group ids are compared against tab ids and
   colliding namespaces would be one namespace. */
let _nextSeedGroup = 1;

/* The outbound request vocabulary, named once. The verb is solver/route_seed.h's `ROUTE_SEED_NOTICE`; the
   provenance words are solver/engine.h's `PENDING_PROVENANCE_*`, one vocabulary for every outbound request,
   fetch or navigation, so the firing policy reads one set of words. `observed` is stated by the ambient seed
   (the person's browser performed that navigation); it is unreachable on a `document.seed` record, which that
   arm's own check states. */
const ROUTE_SEED_NOTICE = "document.seed";
const PROVENANCE_OBSERVED = "observed";
const PROVENANCE_DERIVED = "derived";
const PROVENANCE_FORCED = "forced";
/* The witness mark's two words, solver/engine.h's `PENDING_PINNED_*`: a separate field from provenance
   (provenance says what a reply is worth; this says whether the act may be spent). `unstated` is not here: it
   is safe-fetch.js's word for a record with no mark (its `_PINNED_MARKS` lists all three), and the engine
   never spells it. */
const PINNED_YES = "pinned";
const PINNED_NO = "unpinned";

/* What this zone owes a one-way notice. Each op is an action only this zone can take (SECURITY.md: the
   offscreen alone knows which instance holds which document); ops not listed here are described at their arms.
   `navigable.create <child> <creator> <url> <origin> <topLevelUrl> <cspSelfOrigin> <coep> <coepEndpoint>
   <coepReportOnly> <coepReportOnlyEndpoint> <parentNavigable> <containerPolicy> <ancestorOrigins>
   <creationSandboxFlags> <provenance> <csp>` — the engine has named the document and handed the page a
   WindowProxy for it; this provisions (or joins) an instance under that name, loading the child's document
   through the safeFetch chokepoint.
   `navigable.swap <new document> <url> <origin> <provenance>` — see that arm.
   `windowproxy.post <target> <world> <targetOrigin> <base64>` — routed verbatim to the instance holding
   <target>, with this engine's origin stamped on it: the untrusted engine may not state its own origin, since
   a forgeable event.origin defeats every origin check in every bundle. */
async function hostNotice(eng, line) {
  const f = line.split("\t");
  if (f[0] === "navigable.create") {
    /* The count covers every field the reader below indexes and moves when the record grows, because the
       field added last is the one a stale count would let arrive as `undefined`. */
    DCHECK(f.length >= 17, "a navigable.create notice was short of its fields — the engine writes child, creator, url, origin, top-level creation URL, CSP self-origin, the four items of §7.1.4's embedder policy, HTML §7.3.1.3's parent navigable and its container's Permissions Policy §9.5 answer, HTML §3.1.3's internal ancestor origin objects list, HTML §7.1.5's creation sandboxing flag set, CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's word for the navigation, and the policy");
    if (hostHolderOf(f[1])) return;   // already provisioned: the engine announces a document once
    /* Field 15 is the engine's word for what this navigation is evidence of; navigationLoad and the chokepoint
       decide on it, and this arm passes it through without testing it. */
    /* `none`: a notice has no request id for a refusal, and its only arm for a bytes-less load would fabricate
       an empty document, so navigationLoad aborts on a decline instead, naming what to build. */
    const loaded = await eng.fetchedDocument(f[3], f[15], /*refusalArm*/"none");
    /* The child's principal is the origin of the response's URL (`loaded.url`, after redirects, which only
       this zone followed), not the notice's f[4]: SECURITY.md makes routing and the origin stamped on a
       delivered message the trusted zone's alone. The same URL gives the cluster and the address the instance
       is rooted at (HTML §7.5.1 Shared document creation infrastructure's `creationURL`).
       Named residual: a child whose creator applied `sandbox` without `allow-same-origin` has an opaque origin
       (HTML §7.3.2.1 Creating browsing contexts: the sandboxed origin browsing context flag yields "a new
       opaque origin"), and this still stamps its address's tuple origin. Next diff reads that flag out of
       `creationSandboxFlags` and mints a per-document opaque token here, as `_senderOrigin` does. Absence
       shows as main.c's root entry aborting on the pairing of that flag with a tuple principal. */
    /* The child's browsing-context group is its creator's: a nested navigable is in its parent's group, and an
       auxiliary one too unless `noopener` severed it. So two cross-origin children of one page at one origin
       are one cluster, as HTML has them. */
    /* The child's document as bytes, passed unchanged (see engineCreate); a load that did not load carries
       none, and the empty byte sequence is the `about:blank`-shaped document the engine's child_document builds
       for one. */
    DCHECK(loaded && (loaded.bytes === null || loaded.bytes instanceof Uint8Array),
           "the document load answered neither bytes nor the null that means it did not load");
    DCHECK(loaded && typeof loaded.url === "string" && loaded.url !== "",
           "the document load answered no RESPONSE URL for a cross-origin child — the address the notice " +
           "carries is the one the creating engine ASKED for, and the principal this zone stamps on a peer " +
           "instance has to be the origin of the URL its bytes CAME from");
    const _childBytes = loaded.bytes === null ? new Uint8Array(0) : loaded.bytes;
    /* How this document was reached is the join (`safeFetchReachJoin`) of the creating document's grade and the
       engine's word for the navigation, the weaker of the two, so a child of a document this tool opened is
       never graded as one the person navigated to. It is on every analyze record, since `fetched` and the XHR
       relay read it for every request the document makes. */
    const msg = { type: "AST_ANALYZE", pageHtml: _childBytes, sourceUrl: loaded.url,
                  origin: originOf(loaded.url), groupId: eng.groupId,
                  provenance: self.safeFetchReachJoin(eng.msg.provenance, f[15]),
                  responseHeaders: {}, credentialed: !!(eng.msg && eng.msg.credentialed) };
    /* The response's own header list, whole; the creator's container is relayed separately below. */
    DCHECK(loaded && loaded.headers && typeof loaded.headers === "object",
           "the document load answered no header list — §7.5.1 creates the child's Document from its " +
           "response's headers, and a missing list is a producer that stopped writing one rather than a " +
           "response that carried none, which is the empty object");
    for (const _n of Object.keys(loaded.headers)) msg.responseHeaders[_n] = loaded.headers[_n];
    /* The creator's clone is relayed as a container, never as a response header: a response header's policy
       gets the response URL's origin as its self-origin (CSP §2.2.2 Parse response's Content Security
       Policies), while CSP §2.2 Policies gives an inherited list the creator's self-origin, which §6.7.2.8 Does
       url match expression in origin with redirect count? reads. Mixing them answers `'self'` backwards. Both
       halves cross separately and the engine decides which container the Document gets (main.c's
       document_csp_list); this zone relays and computes only the child's principal.
       The policy is the record's remainder: a raw CSP header may contain HTAB, so it is last and every other
       field precedes it. The C router splits the same way, keeping the remainder verbatim. */
    /* The embedder policy's four items sit between the self-origin and the policy and are safe in split
       fields: their values are fixed tokens, and `report-to` is a structured-field string (RFC 8941 §3.3.3
       Strings: printable ASCII %x20-%x7E, no tabs). Relayed verbatim; only the receiving engine maps the tokens,
       and it crashes on an unknown one (see NEW_EMBEDDER_POLICY). */
    /* The policy is the remainder from field 16; every field added to the record is added in front of it, so
       this index and the count above move together. A wrong index would join a neighbouring field onto the
       creator's CSP, which CSP's parser drops as an unreadable directive without a crash. */
    const inherited = { csp: f.slice(16).join("\t"), selfOrigin: f[6],
                        embedder: { value: f[7], endpoint: f[8],
                                    reportOnlyValue: f[9], reportOnlyEndpoint: f[10] } };
    /* HTML §7.3.1.3 Child navigables' parent navigable at field 11, separate from the container policy: an
       auxiliary navigable (`window.open`) has a full container and no parent. It is the emitting engine's own
       navigable identity (core/frame/remote_object.h), relayed verbatim; only the receiving engine decodes it,
       and it crashes on an unparseable record. A one-letter tag and '.'-terminated base64 contain no tab. */
    const parentNavigable = f[11];
    /* The container answer at field 12, the other §7.3.1.3 link: a child `<iframe>` has both links and an
       auxiliary navigable has neither. It carries Permissions Policy §9.5's result, computable only in the
       creating engine, which holds the element and the child's origin. Relayed verbatim; feature tokens and
       `Enabled`/`Disabled` contain no tab. */
    const containerPolicy = f[12];
    /* The internal ancestor origin objects list (HTML §3.1.3 Ancestor origins) at field 13, a third algorithm
       over inputs neither field above carries. The answer travels rather than the inputs because §3.1.3 asks
       whether an ancestor is same origin with the parent document's origin, and opaque origins are compared by
       identity while all serialize to `null` (a `data:` document is in its own instance because it is
       opaque). Entries are origin serializations joined by SPACE, which URL §3.2 Host miscellaneous forbids in
       a host, as it does TAB. */
    const ancestorOrigins = f[13];
    /* The creation sandboxing flag set (HTML §7.1.5 Sandboxing) at field 14, not an item of the policy
       container; it reads the embedder element and its node document's active flags, so the creating engine
       computes it. Members are joined by COMMA because §7.1.5's flag names contain spaces. Relayed verbatim;
       the receiving engine crashes on a word outside that section. */
    const creationSandboxFlags = f[14];
    DCHECK(typeof creationSandboxFlags === "string" && creationSandboxFlags !== "",
           "a navigable.create notice carried no HTML §7.1.5 CREATION SANDBOXING FLAG SET — navigable.c " +
           "writes that section's own flag names on every record, and its word for the empty set where there " +
           "are none, because both are facts and neither is an empty field. Without it this child would be " +
           "provisioned with the EMPTY set, which is the positive claim that nothing about it is sandboxed: a " +
           "cross-origin `<iframe sandbox>` would run its scripts, submit its forms and relax its " +
           "`document.domain`, none of which its embedder's markup permits");
    DCHECK(typeof ancestorOrigins === "string" && ancestorOrigins !== "",
           "a navigable.create notice carried no HTML §3.1.3 ANCESTOR ORIGINS statement — navigable.c writes " +
           "the composed list on every record, and `none` where the Document has no container document at " +
           "all, because both are facts and neither is an empty field. Without it this child would be " +
           "provisioned with §3.1.3's EMPTY list, which is the positive claim that it is at the TOP of its " +
           "own tree: `location.ancestorOrigins` would answer `[]` for a cross-origin frame, and nothing " +
           "anywhere would disagree — the member exists precisely to report a tree the page cannot otherwise " +
           "see");
    DCHECK(typeof containerPolicy === "string" && containerPolicy !== "",
           "a navigable.create notice carried no HTML §7.3.1.3 CONTAINER statement — navigable.c writes " +
           "Permissions Policy §9.5's answer on every record, and `null` where the navigable has no container " +
           "at all, because both are facts and neither is an empty field. Without it this child would be " +
           "provisioned to take §9.7 step 1 (\"if container is null, return `Enabled`\") for a frame that HAS " +
           "an embedder, and would hold every supported feature that embedder was never asked about");
    DCHECK(typeof parentNavigable === "string" && parentNavigable !== "",
           "a navigable.create notice carried no HTML §7.3.1.3 PARENT NAVIGABLE — navigable.c writes the " +
           "identity on every record because a navigable either has a parent or is a top-level traversable, " +
           "and it spells the second `u`; an empty field is an engine that stopped writing it, and this child " +
           "would be provisioned as a top-level page inside the only instance that holds it, with `parent`, " +
           "`top` and §7.1.4.2's embedder policy check all answering about a document that does not exist");
    DCHECK(embedderPolicyWhole(inherited.embedder),
           "a navigable.create notice carried no §7.1.4 EMBEDDER POLICY — navigable.c writes all four of its " +
           "items on every record because HTML §7.1.7's clone moves a container WHOLE, so a missing one is an " +
           "engine that stopped writing the field and a child created claiming `unsafe-none` for a creator " +
           "that opted into cross-origin isolation, with no header on the child's own response to say so");
    DCHECK(typeof inherited.selfOrigin === "string" && inherited.selfOrigin !== "",
           "a navigable.create notice carried no CSP self-origin — navigable.c writes the creator's on every " +
           "record because HTML §7.1.7 clones the container whole and CSP §2.2 makes its list a struct of " +
           "policies AND an origin, so an empty one is an engine that stopped writing the field and a child " +
           "whose inherited `'self'` would resolve against its own address");
    /* The top-level creation URL (HTML §8.1.3.1 Environments) is the creator's decision (its own for a nested
       navigable, the navigable's address for an auxiliary one), carried because the new instance cannot see
       what embeds it; HTML §8.1.3.5 Secure contexts reads it. */
    /* The cluster decides which of two operations this is. A same-origin child never reaches here
       (navigable.c's `child_in_this_agent` builds it as a realm in the creator's heap and emits no notice), so a
       cluster hit is a second cross-origin document of one cluster (two `<iframe src="https://cdn/…">` of one
       page), which HTML puts in one heap: it is joined to the running instance. A cluster with no instance is
       provisioned one. */
    const _ckey = clusterKeyOf(msg);
    DCHECK(_ckey !== eng.cluster,
           "the engine sent a create notice for a child in its OWN agent cluster — a same-origin child is a " +
           "second REALM in that heap and never leaves it, so this zone and navigable.c's child_in_this_agent " +
           "have answered one origin question two ways");
    /* Reservations answer too: engineCreate takes the pool slot before its first await, so a cluster being
       provisioned answers here as a provisioned one does. */
    const holder = hostClusterOf(_ckey);
    if (holder) {
      /* A reservation is still the holder, so the join waits for it: `qjs_join` needs `qjs_init` and
         `qjs_begin` to have run, which `_readyP` settling means. */
      if (holder.state === "booting") await holder._readyP;
      DCHECK(holder.state === "hot" || holder.state === "fetching",
             "a document was announced for a cluster whose instance never became one — the reservation holding " +
             "it failed to boot, so this child has an agent that does not exist rather than one it can join, " +
             "and every read through its proxy would park forever");
      /* The joined document is referenced (the creating engine minted this name because its page holds a
         WindowProxy under it), so its instance may not be one whose timelines may run out; the flag is stated
         only before `qjs_begin`, so an unreferenced holder is refused rather than told late. It is reachable
         through nesting that returns to an origin a top-level document already runs in this group (A embeds
         cross-origin B, B embeds A). What to build: an entry that states the flag for a live frontier, a
         different ABI entry from the one whose `!g_begun` assert exists. */
      DCHECK(holder.referenced === 1,
             "a cross-origin child was joined to an instance that never declared a peer reference — the create " +
             "notice is itself the proof that one exists (the creating engine minted this name because its " +
             "page holds a WindowProxy under it), so this agent may not let its timelines run out, and the " +
             "flag that says so can only be stated before the frontier is seeded; this instance's was seeded " +
             "for a document nothing held a proxy for, and the first read through the new child's proxy will " +
             "arrive at a heap entitled to have finished");
      /* The bytes are on the message here, the one caller for which that holds: `msg` is the object this zone
         composed for the announced child. */
      await engineJoin(holder, msg.pageHtml, msg, f[1], f[5], inherited, parentNavigable, containerPolicy,
                       ancestorOrigins, creationSandboxFlags);
      return;
    }
    /* A child document joins the one pool and is ranked, sliced, parked and finalized like every other. It has
       no caller, so its findings merge like a rehydrated engine's (`cold`, stated at the reservation). Awaited,
       because one round's notices are acted on in order: a page opens a window and posts to it in one turn, so
       the instance must exist before the post is routed. Referenced, since the notice itself proves a
       WindowProxy is held for it. */
    await engineCreate("", msg.pageHtml, msg, false, f[1], f[5], true, inherited, parentNavigable,
                       containerPolicy, ancestorOrigins, creationSandboxFlags, 1)._readyP;
    return;
  }
  /* `navigable.swap <new document> <url> <origin> <provenance>` — HTML §7.1.3.2 Browsing context group
     switches due to opener policy: a navigation whose response's opener policy does not match builds its
     Document in a new top-level browsing context in a new browsing context group, and the old one is left
     behind (the emitting instance has recorded that, so the opener's handle answers `closed === true`).
     It must not join: an instance is (group, origin) and a swap changes the group, so even at an origin this
     pool runs it is a separate heap. The group id is minted here by a counter, a routing fact that is this
     zone's alone. The new context has a null creator, so no container or opener is inherited, and its
     top-level creation URL is its own address. This zone loads the bytes: an untrusted engine that supplied
     bytes and a principal could name any origin's document into existence.
     Named residual: the address is fetched again instead of committing the response the navigation already
     had. Next diff remembers this zone's own reply to that `document.fetch`, keyed by (instance, address).
     Absence shows as a swapped-to document whose content or headers differ from the response that decided
     the swap when the server answers the two requests differently. */
  if (f[0] === "navigable.swap") {
    DCHECK(f.length >= 5 && !!f[1] && !!f[2] && !!f[4],
           "a navigable.swap notice was short of its fields — the engine writes the new document's name, the " +
           "address it is loading, the origin it computed and what the navigation that caused the swap is " +
           "EVIDENCE OF; a record missing either of the first two names a Document this zone cannot provision " +
           "while the navigable it replaces is already closed, and one missing the last leaves the load below " +
           "with nothing to be decided from");
    DCHECK(!hostHolderOf(f[1]),
           "§7.1.3.2's swap named a document this pool already holds — the name is minted fresh for each swap " +
           "(solver/world.h), so a collision is one instance provisioned for two Documents, which is one heap " +
           "answering for two");
    /* The re-fetch is made under the same decision as the navigation's own load, since the provenance on this
       record is the load job's (core/frame/browsing_context_group.c takes it from there); a swap cannot fetch
       an address this zone declined at `document.fetch`. */
    /* `none`, for the create arm's reason: a swap is a notice, and its `pageHtml` line turns a null body into
       an empty Uint8Array. */
    const swapped = await eng.fetchedDocument(f[2], f[4], /*refusalArm*/"none");
    DCHECK(swapped && (swapped.bytes === null || swapped.bytes instanceof Uint8Array),
           "the swapped-to document load answered neither bytes nor the null that means it did not load");
    DCHECK(swapped.headers && typeof swapped.headers === "object",
           "the swapped-to document load answered no header list — §7.5.1 creates its Document from the " +
           "response's headers, and §7.1.3 obtains the opener policy that decides its new group's cross-origin " +
           "isolation mode out of the same list");
    /* The response's URL, for the create arm's reason: the instance's principal and `creationURL` are the
       address the bytes came from. */
    DCHECK(typeof swapped.url === "string" && swapped.url !== "",
           "the swapped-to document load answered no RESPONSE URL — §7.1.3.2 provisions a new instance from " +
           "the address the Document is at, which after a redirect is not the address the swap requested");
    /* The reach grade is the join of the creating document's grade and the engine's word (`f[4]`), as for a
       created child, and is stated on every analyze record. */
    const swapMsg = { type: "AST_ANALYZE", pageHtml: swapped.bytes === null ? new Uint8Array(0) : swapped.bytes,
                      sourceUrl: swapped.url, origin: originOf(swapped.url),
                      groupId: "swap:" + (_nextSwapGroup++),
                      provenance: self.safeFetchReachJoin(eng.msg.provenance, f[4]),
                      responseHeaders: {}, credentialed: !!(eng.msg && eng.msg.credentialed) };
    for (const _n of Object.keys(swapped.headers)) swapMsg.responseHeaders[_n] = swapped.headers[_n];
    /* No inherited policy: a swapped-to Document has no creator to clone a container from (HTML §7.3.2.1 with
       a null creator), so a response without CSP is a Document under no policy. */
    DCHECK(hostClusterOf(clusterKeyOf(swapMsg)) === null,
           "§7.1.3.2's swap minted a browsing-context group this pool already runs an instance for — the group " +
           "id is a fresh counter, so a hit means two swaps were given one id and the second would JOIN the " +
           "heap the first built, which is exactly the boundary a group switch exists to draw");
    /* `null` container: the swapped-to context is created with a null creator, so HTML §7.1.7 has nothing to
       clone and the Document is judged against its own response. */
    /* The parent is `u`: HTML §7.1.3.2's swap is reached only for a top-level browsing context (the engine
       asserts it where the record is written), so the navigable has no parent. The ancestor list (HTML §3.1.3)
       is `none` for the same reason: a top-level context has no container document. */
    /* The top-level creation URL is `swapped.url`: the swapped-to navigable is a top-level traversable, so it is
       its own Document's address after redirects. */
    /* Not referenced: the old browsing context "will not be used by the new Document" (HTML §7.1.3.2's note)
       and the emitting engine discards it after writing this notice, so the opener's handle answers about the
       document it had (its `closed` is true) and nothing holds a proxy for this one. */
    await engineCreate("", swapMsg.pageHtml, swapMsg, false, f[1], swapped.url, true, null, "u", "null",
                       "none", "none", 0)._readyP;
    return;
  }
  /* `document.seed <address> <provenance>` — an address the application declared is a page of itself, reached
     when forced execution ran the bundle's own `history.pushState`/`replaceState` (solver/route_seed.h says what
     a declaration is). This branch spends no network: a declared route is a work item whose fetch is a cost the
     Level-1 order carries, so it records and `admit` loads (see `_seeds`). */
  if (f[0] === ROUTE_SEED_NOTICE) {
    DCHECK(f.length >= 3 && !!f[1] && !!f[2],
           "a route declaration was short of its fields — the engine writes the ADDRESS and the PROVENANCE, " +
           "and a record missing either is a writer this zone no longer shares a grammar with: an absent " +
           "address is a page nothing can load, and an absent provenance is the one field the decision to " +
           "load it at all is made from");
    /* Enumerated, not defaulted. `observed` is unreachable for a declaration (HTML §7.4.4 is reached only by
       running the page's code), so a record carrying it is a field read at the wrong offset. */
    DCHECK(f[2] === PROVENANCE_DERIVED || f[2] === PROVENANCE_FORCED,
           "a route declaration carries the provenance `" + f[2] + "`, which is neither `derived` nor " +
           "`forced` — those are the only two a declaration can have, so this is either the field split at " +
           "the wrong tab or an engine speaking a vocabulary this zone does not");
    /* A route of this document, established from the two addresses: HTML §7.2.5 The History interface's
       can-have-its-URL-rewritten already refuses a cross-origin rewrite in the engine, and this zone checks the
       same thing from its own facts. The comparison decides no principal (the load reads under `eng.origin`).
       It is a refusal, not an assert, because the engine is untrusted. */
    if (originOf(f[1]) !== originOf(eng.msg.sourceUrl)) {
      console.warn("[bridge] a route declaration named an address outside the declaring document's origin (" +
                   f[1] + " vs " + eng.msg.sourceUrl + ") — HTML §7.2.5's can-have-its-URL-rewritten permits " +
                   "no such rewrite, so this is not a page of that application and it is not seeded");
      return;
    }
    /* An address the chokepoint would not fire is not enqueued. `safeFetchFiringRefusal` is the same function
       the chokepoint refuses with (`_firingRefusal`), asked here because enqueuing a load it will refuse would
       spend an admission slot every round. It answers the refusing grade, which the warning names. For a
       navigation the address is the whole of the safety, and a navigation carries the person's session. A
       derived but unfired address is still reported. */
    /* `document` and `unstated`, as navigationLoad states them: a declaration is a notice, so no witness mark was
       composed, and a document load is never script-like. */
    /* The facts arrive as one object so the growing signal set cannot shift positional operands. `credentialed`
       is the answer this seed's load would get, asked with the same predicate; `headers` is absent on a
       navigation and stated as such. The principal is `eng.origin` (MessageSender.origin, the same value the
       seed carries as `principalOrigin`), never a URL-derived origin, which would answer `true` for a sandboxed
       document. */
    /* `docReach` is the declaring document's own grade, not the join, since this asks about a request issued by
       that document. */
    /* `actor: "page"`: the route was declared by this document's own code, as the load will state. */
    const _seedRefusal = self.safeFetchFiringRefusal({
      url: f[1], destination: "document", provenance: f[2], pinned: "unstated",
      docReach: eng.msg.provenance, actor: "page",
      credentialed: navigationCarriesSession(f[1], eng.origin), headers: null });
    if (_seedRefusal) {
      console.warn("[bridge] a route declaration for `" + f[1] + "` is refused by this origin's egress " +
                   "policy on `" + _seedRefusal + "` — the address is derived and reported, and it is not " +
                   "seeded; the signal and value named there is the row of the person's own control that " +
                   "holds it");
      return;
    }
    /* The work item takes its inputs with it: both principals belong to the declaring document, not the declared
       address (`safeFetch` classifies the SSRF host relative to `principalUrl`, so an address supplying itself
       would self-authorize), and the pool may no longer hold that engine at admission. The provenance is stored
       from the record (always `derived` here, since a refused one returned above); navigationLoad asserts the
       vocabulary on the way out. */
    if (!_seeds.has(f[1]))
      /* The declaring document's reach grade travels with the item for the same reason. */
      _seeds.set(f[1], { url: f[1], principalUrl: eng.msg.sourceUrl, principalOrigin: eng.origin,
                         provenance: f[2], reach: eng.msg.provenance });
    /* Nothing is kicked: this router runs inside serviceFetch within the one scheduling loop, so `_hostDriving`
       is true and a `_hostKick()` would return on its first line. `admit` ranks this entry next round. */
    return;
  }
  if (f[0] === "windowproxy.post") {
    DCHECK(f.length >= 5, "a windowproxy.post notice was short of its fields");
    const target = hostHolderOf(f[1]);
    DCHECK(target !== null, "a message was posted to a document no instance in this pool holds — the create " +
                            "notice naming it was dropped, or that instance was finalized while a peer still " +
                            "held a WindowProxy for it");
    /* A target mid-boot is still the target, and the delivery waits for it: the reservation carries the name
       from the start, so routing now would post into `null` and refusing would drop a delivery the page already
       holds a WindowProxy for. */
    if (target.state === "booting") await target._readyP;
    DCHECK(target.state === "hot" || target.state === "fetching",
           "a message was posted to a document whose instance never became one — the reservation holding that " +
           "name failed to boot, so the send is a delivery with nowhere to go rather than a message in flight");
    /* The stamp is the sender's browser-stated origin, serialized. An empty one is an instance rehydrated from a
       recipe written before the principal was recorded; this zone does not know whose message it is and must
       not invent one. A CHECK, because in release the delivery would carry a fabricated identity into a
       security decision. */
    CHECK(!!eng.origin, "a cross-document message was posted by an instance with no recorded principal — only " +
                        "the trusted zone may state a sender's origin, and this one has none to state; carry " +
                        "the document's browser-stated origin into the cold recipe so a resumed instance keeps it");
    await target.r.renderer.route({ record: line, senderOrigin: stampOrigin(eng.origin) });
    return;
  }
  /* `remoteop.answer <token> <world> <completion>` — a completion this instance produced for an operation it
     was asked to perform, under the token this zone minted. It is a notice, not a return value, because a peer
     answers by running a program (an IDL getter, a page's setter or function) as a flow on its own frontier.
     Relayed verbatim and unread: the completion is in remote_object.c's grammar, where an object crosses as a
     name in the answering agent's namespace, which only an engine can interpret. */
  if (f[0] === "remoteop.answer") {
    DCHECK(f.length >= 4, "a remoteop.answer notice was short of its fields — the engine writes the rendezvous " +
                          "token, the TIMELINE that computed the answer and the completion record, and a " +
                          "notice missing any of them names no call site or no timeline");
    const to = _remoteOps.get(f[1]);
    /* A token this zone did not mint names no asker, leaving the asking flow parked on an answer nothing can
       deliver. */
    DCHECK(to !== undefined, "a peer answered a cross-agent operation under a rendezvous token this zone never " +
                             "minted — the token is echoed verbatim by the instance that performed it, so the " +
                             "flow that asked is parked on an answer nothing can deliver");
    /* The world is the third field (world_serialize's grammar, separators ':' and ','), the completion the
       remainder (it may contain a tab). This zone reads neither but must know where one ends. */
    const _t2 = line.indexOf("\t", line.indexOf("\t") + 1);
    const _t3 = _t2 < 0 ? -1 : line.indexOf("\t", _t2 + 1);
    DCHECK(_t3 > _t2 + 1, "a remoteop.answer notice carried no completion record after its timeline — an empty " +
                          "answer is not `undefined`, it is a relay that lost the peer's completion, and the " +
                          "engine's own decoder says so at the other end");
    /* Every completion is relayed: a peer's document state is its flows, so one question has one true answer per
       timeline, and the asking flow forks an arm over each. Keeping one per token would answer a page from
       whichever timeline the interleaving left. The engine refuses a repeat of one timeline at its delivery
       entry. */
    if (!to || _t3 < 0) return;
    await to.asker.r.renderer.hostAnswerRemote({ request: to.req, world: line.slice(_t2 + 1, _t3),
                                                 completion: line.slice(_t3 + 1) });
    return;
  }
  /* `world.gone <world>` — a world of this instance is finished (its flow left the frontier, or the session
     parked into a generation that will never mint again). Every peer that received a delivery or operation from
     it holds a COW segment keyed on it (solver/world.h) until told, and cannot park while it does (cold_park
     refuses a foreign segment). Broadcast by design: the sender does not track which peers a flow reached,
     since releasing an unheld world is a no-op, and only this zone knows the other instances. A booting
     instance is skipped because both delivery branches wait for `_readyP`, so it holds no foreign world
     (qjs_world_gone asserts the frontier was seeded). */
  if (f[0] === "world.gone") {
    DCHECK(f.length >= 2 && !!f[1],
           "a world.gone notice carried no world name — world_parse answers out of whatever the receiving " +
           "instance last parsed, so an empty one releases a segment belonging to a live peer's timeline");
    for (const peer of _pool) {
      if (peer === eng) continue;
      if (peer.state !== "hot" && peer.state !== "fetching") continue;
      await peer.r.renderer.worldGone({ world: f[1] });
    }
    return;
  }
  /* `remoteop.retracted <token>` — an instance is parking with a question it was asked and hands it back,
     started or not: a half-run answering program's work is the parking flow's own delta, and what was asked
     for was a call (HTML §7.2.1.3.5 CrossOriginGet ( O, P, Receiver ) ends "Return ? Call(getter, Receiver)"),
     so an abandoned call was made zero times. One notice per question, sent by the last holding timeline.
     This zone forgets: `_remoteAsked` suppresses re-asks because `engine_host_requests` does not dedupe, and
     the asking flow is still suspended and re-reported every step, so lifting the suppression is the re-ask.
     Nothing is stored: a token is this session's in-memory name and never enters a residue; across a restart
     the asker's own recipe re-issues the request under a new id. */
  if (f[0] === "remoteop.retracted") {
    DCHECK(f.length >= 2 && !!f[1],
           "a remoteop.retracted notice carried no rendezvous token — the token is the only thing that names " +
           "which asking flow's question is being handed back, so an empty one leaves that flow suspended " +
           "forever with this zone still suppressing the re-ask");
    const back = _remoteOps.get(f[1]);
    /* An unknown token means the engine minted a name of its own, and the flow this was meant to release is not
       the one released. */
    DCHECK(back !== undefined,
           "an engine handed back a cross-agent operation under a rendezvous token this zone never minted — " +
           "the token is echoed verbatim by the instance that was asked, so the asking flow this was meant to " +
           "un-suppress is still suppressed and stays parked");
    if (!back) return;
    back.asker._remoteAsked.delete(back.req);
    _remoteOps.delete(f[1]);
    return;
  }
  /* `world.parked <world vector>` — a park wrote the foreign segments this instance holds into its residue
     (solver/cold.h's 'w' record), so a peer's timeline outlives the session that held it. `world.gone` is
     broadcast only to live instances, so a death announced while this document is parked is missed and the
     resuming instance would hold a segment for an ended world for the rest of its process.
     What to build: a persisted index from world name to the parked document carrying a segment for it (the
     frontier entry is keyed by address|bundle, so this is a reverse map or a store of its own), surviving a
     browser restart as the residue does, drained into the resuming instance's `worldGone` after `begin`. */
  if (f[0] === "world.parked") {
    DCHECK(f.length >= 2 && !!f[1],
           "a world.parked notice carried no world vector — the vector is what the resuming instance hands " +
           "back to world_segment, so an empty one names a peer timeline nothing can rebuild or release");
    DFAIL("an engine parked while carrying a PEER's world segment across the cold tier (`" + f[1] + "`), and " +
          "this zone holds no death queue for a cold document. Every `world.gone` announced while this " +
          "document is parked is broadcast to the LIVE pool alone and lost, so the instance that resumes it " +
          "rebuilds a segment for a world that has ended and never releases it. Build the index this notice " +
          "exists to make exact — world name -> the parked document carrying a segment for it, persisted as " +
          "far as the residue is — and drain it into the resuming instance's `worldGone` after `begin`");
  }
  DFAIL("an engine emitted a notice with an op this zone does not act on — the engine's half is built, so the " +
        "host's half is the unbuilt one: `" + f[0] + "`");
}

// What only this zone can answer. A cross-document operation is answered by the instance holding that document,
// and only the offscreen knows which instance that is (SECURITY.md). The asking flow is suspended mid-frame
// until the answer lands, so this is pumped every round beside the fetch replies; an unanswered one parks that
// flow while its siblings keep running.
/* It returns how many registers it filled, for engineDeliverLanded's reason; a branch that answers nothing (an
   op this zone may not guess at) counts none, leaving the asking flow parked and visible. */
async function engineServiceHostRequests(eng) {
  let did = 0;
  // One-way notices: this zone owes each an action, handled in order and one at a time, because a page opens a
  // window and posts to it in the same turn and the create must finish provisioning before the post is routed.
  for (const line of owedList("GetHostNotices", (await eng.r.renderer.getHostNotices()).notices)) {
    await hostNotice(eng, line);
    did++;
  }
  // Only ops this zone can answer are answered: `document.fetch` is a network fetch through the safeFetch
  // chokepoint, and a cross-agent operation is asked of the peer. Every other op stays unanswered: the asking
  // flow stays parked with its snapshot intact and qjs_host_requests keeps reporting it, which is visible where
  // a guessed answer is not. Answering is what lets a navigation finish.
  const reqs = owedList("GetHostRequests", (await eng.r.renderer.getHostRequests()).requests);
  for (const line of reqs) {
    /* `id<TAB>op`, written by engine_host_requests from a counter it CHECKs against wrapping; a record without
       that shape is asserted, since dropping it would leave its flow parked with no symptom. */
    const tab = line.indexOf("\t");
    DCHECK(tab > 0, "an owed host request is not `id<TAB>op` — the id is what an answer is routed by, so a " +
                    "record without one names a call site this zone can never reach");
    if (tab < 0) continue;   // release: an unanswerable record leaves its flow parked, which is visible
    const id = +line.slice(0, tab), op = line.slice(tab + 1);
    /* The id is the whole routing table for an answer: engine_host_answer matches every flow's register on it
       and the engine's counter starts at 1, so 0 or NaN would answer a call site that does not exist. */
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
      await holder.r.renderer.perform({ token, record: op });
      did++;
      continue;
    }
    // XHR §3.5.6's fetch is the SECOND thing this zone can genuinely answer, and for the identical reason: it
    // is a network fetch, and safeFetch is the one chokepoint SECURITY.md allows it through. The record carries
    // the whole request — method, headers and body — because the chokepoint decides SOP, CORS, method and
    // credentials and cannot decide about a method it was never told. A flow parked on one is SUSPENDED at the
    // exact line the page wrote `send()` on, which is what a synchronous XMLHttpRequest is.
    if (op.startsWith("xhr.send\t")) {
      /* ISSUED AND NOT AWAITED, FOR THE PENDING SEAM'S REASON AND THROUGH THE SAME PRIMITIVE. `fetchedXhr`
         reaches the same `safeFetch` and therefore the same `_readBody`, so a response the server never ends
         held this round exactly as one on the other seam did — and `XMLHttpRequest.send()` is half of the
         two-line reproduction of that freeze. The KEY IS THE REQUEST ID and not a `(method, url)` pair, which
         is `wpt_runner.c`'s own split (`wpt_request_asked_id` beside `wpt_request_asked`): this answer is
         delivered against `engine_host_answer`'s request id, and `engine_host_requests` deliberately does not
         dedupe, so two identical questions from two flows are two questions with two ids.
         THE FLOW STAYS SUSPENDED AT THE LINE THE PAGE WROTE `send()` ON EITHER WAY, which is what a
         synchronous XMLHttpRequest is — what changes is that its SIBLINGS now get the thread while it waits. */
      did += engineIssue(eng, "xhr\n" + id,
        () => eng.fetchedXhr(op.slice("xhr.send\t".length)),
        // 0 IS THE NORMAL COMPLETION. An answer is a completion record and not a value (ECMA-262 6.2.4): this
        // zone fetched bytes rather than running another instance's program, so it has nothing to have thrown
        // in. A relayed cross-agent operation answers with 1 and the thrown value, which is what lets the
        // asking page's `try`/`catch` around it run.
        (r) => engineAnswer(eng, id, r.meta, r.bytes));
      continue;
    }
    if (!op.startsWith("document.fetch\t")) continue;
    /* `document.fetch<TAB><provenance><TAB><url>` — SPLIT ONCE, AT THE FIRST TAB AFTER THE VERB, so the
       ADDRESS IS THE REMAINDER. A URL cannot contain a tab (URL Standard §4.4 "URL parsing" removes every
       ASCII tab from its input before anything else and the C0 control percent-encode set escapes one
       everywhere it could reappear), and the vocabulary in front of it is three words of ASCII lowercase
       letters, so this grammar has exactly one place it can be taken apart and this is it.
       IT IS INDEXED RATHER THAN `split("\t")`-ed for that same reason in reverse: splitting on every tab and
       taking the last field would answer correctly for an address that has none and silently pick the tail of
       one that does, which is the shape a reader that can never be told it is wrong has. */
    const fetchArgs = op.slice("document.fetch\t".length);
    const fetchTab = fetchArgs.indexOf("\t");
    DCHECK(fetchTab > 0 && fetchTab < fetchArgs.length - 1,
           "a document.fetch request is not `document.fetch<TAB><provenance><TAB><url>`: `" + op + "` — " +
           "core/frame/navigable.c writes both fields non-empty on every path (the provenance from " +
           "solver/engine.h's three tokens, the address as an absolute serialization), so a record with one " +
           "tab is this zone and that job no longer sharing a grammar, and the address read out of it would " +
           "be a provenance token");
    /* ISSUED AND NOT AWAITED, FOR THE XHR SEAM'S REASON EXACTLY: a document load reaches the same `safeFetch`
       and the same `_readBody`, so a server that holds a document's body open held this round — and a page
       states one with `location.href` or an `iframe src`. Keyed on the REQUEST ID for the same reason that seam
       is. The navigable that asked stays parked on its load, which is what a navigation in flight is; every
       other flow in the document keeps running. */
    did += engineIssue(eng, "doc\n" + id,
      /* `rendezvous` \u2014 THE ONE CONSUMER OF THIS FORWARD THAT HOLDS ONE. `engineDeliverDocument` reads
         `r.declined` and routes it to `hostDecline` against the id the engine is parked on, so a refusal has
         somewhere to go and the navigable keeps the `about:blank` \u00a77.3.1.3 created it holding. */
      () => eng.fetchedDocument(fetchArgs.slice(fetchTab + 1), fetchArgs.slice(0, fetchTab),
                                /*refusalArm*/"rendezvous"),
      (r) => engineDeliverDocument(eng, id, r));
    continue;
  }
  return did;
}
/* THE DOCUMENT LOAD'S DELIVERY, which is the body of the branch above unchanged — a named function for the
   reason `engineDeliverReply` is one: the answer is handed over against the id it was fetched under. */
async function engineDeliverDocument(eng, id, r) {
  // JSON, because the answer carries its TYPE across this seam: a null body is a load that did not load, and
  // the string "null" is a one-word document. The BODY is not in that JSON — a Document is parsed from a
  // BYTE SEQUENCE, and this seam carries one (HostAnswer's `array<uint8>?` body).
  /* HTML §7.4.5 "Populating a session history entry"'s answer: the RESPONSE'S URL, its HEADER LIST as the HTTP field lines it delivered, and the
     document as BYTES. It carried one extracted policy (`{csp}`) — see fetchedDocument. The field-line form
     is the one a header list crosses this ABI in and is exactly what qjs_init takes, so a navigated Document
     and a rooted one are built from the identical shape by the identical parse.
     THE URL IS FETCH §2.2.6's RESPONSE URL AND NOT THE ADDRESS THE ENGINE ASKED FOR. Everything HTML §7.4.5
     decides about the incoming Document — its origin, and therefore which agent cluster it belongs to at
     all — is written over the response's URL, and a redirect is what makes the two different. Only this zone
     saw the chain, so only this zone can state it. */
  DCHECK(typeof r.url === "string" && r.url !== "",
         "the document load answered no RESPONSE URL — fetchedDocument states one on every arm, including " +
         "the ones where the load did not load, because §7.4.5 determines a Document's origin over it and a " +
         "navigable whose load failed still gets a Document");
  /* A REFUSAL IS NOT AN ANSWER, SO IT GOES TO THE OTHER ENTRY — and that is the whole of the §7.4 decline on
     this side. `HostAnswer` settles the rendezvous: the parked machine takes the value and NAV_LOAD_CREATE runs
     over it, which is right for a load that failed (a navigable showing an error page is a real §7.4 outcome)
     and a fabrication for a load nobody made. `HostDecline` records the refusal on the same rendezvous instead,
     and the engine's `flow_decline_fork` builds the pair CLAUDE.md §Solver-half requires: one arm goes on
     waiting — holding no value, so the document load yields for ever and the navigable keeps the initial
     `about:blank` §7.3.1.3 "Child navigables" created it holding, which is what fires the day the origin is
     widened — and the other takes §5.6's network error and becomes the error-page document, with its path
     marked FORCED so every value it learns carries the weakest grade this vocabulary has.
     BOTH OUTCOMES ARE EXPLORED, which is why this is not a narrowing of the arm it replaces: the error page was
     the only answer before and it is still one of the two. What is new is the world in which the frame was never
     navigated at all, and that is the world a browser is in when this tool declines to spend the network. */
  if (r.declined) {
    const matched = await eng.r.renderer.hostDecline({ request: id, reason: r.declined });
    /* AND A REFUSAL NOBODY WAS PARKED ON IS WORTH REPORTING RATHER THAN SWALLOWING, for the reason the
       address-keyed `Decline` gives: the asking flow CAN legitimately be gone by the time this zone decides, so
       a zero is not an error — but this zone issued the load against an id the engine had just published, so a
       zero here means the pairing between `GetHostRequests` and this answer has drifted, and every later refusal
       would be recorded for nobody with the machine still parked. */
    DCHECK(matched && matched.matched === 1,
           "the engine was parked on no flow for a document load this zone had just been asked for — the id " +
           "came off `GetHostRequests` and is answered here against the same id, so a miss is this zone and the " +
           "engine no longer agreeing about which rendezvous is outstanding");
    return;
  }
  await engineAnswer(eng, id, { url: r.url, headers: responseFieldLines(r.headers) }, r.bytes);
}
/* THE SAME TWO CHANNELS FOR A SYNCHRONOUS ANSWER. Two of the requests this zone can genuinely answer carry a
   fetched BODY — XHR §3.5.6's fetch and HTML §7.4.5 "Populating a session history entry"'s document load —
   and a body is a byte sequence for the
   same reason a reply's is. `bytes === null` says this answer has none, which is what every other request kind
   is: an answer that is a number or a document NAME has no bytes beside it. The trailing 0 is ECMA-262 6.2.4's
   NORMAL completion — this zone fetched bytes rather than running another instance's program, so it has
   nothing to have thrown in. */
async function engineAnswer(eng, id, meta, bytes) {
  await eng.r.renderer.hostAnswer({ request: id, answer: JSON.stringify(meta), completion: 0, body: bytes });
}
async function engineFinalize(eng) {
  /* ASK THE ENGINE FOR ITS RESULT — the ABI entry that exists for exactly this and had NO CALLER anywhere in
     the extension. The only place the production engine ever printed an @RESULT was qjs_emit_partial, and
     streamPartial CONSUMES that line as it merges it, so a session's findings reached this function only when
     a partial happened to be left over: a page that finished before the first 750 ms cadence produced a
     result with no document in it at all, which `result || {}` then turned into a successful analysis
     reporting no endpoints and no sinks. It is asked BEFORE teardown, because the document is built out of
     the context teardown frees, and it is the same call qjs_emit_partial makes.
     A CRASHED instance is not asked: its memory is what aborted, so re-entering it would only produce a
     second abort. What it had already PRINTED is a different thing from what it still holds — the lines are
     in this zone's buffer, and the last unconsumed snapshot among them is read below like any other. */
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
  /* AND THE OUTSTANDING REQUESTS GO THE SAME WAY, FOR THE RENDEZVOUS MAP'S REASON: a reply delivered into a
     torn-down instance is a call into a renderer this function has already removed. Dropping them is not
     dropping an ANSWER — the flows that were parked on them went with the instance, and their addresses are in
     the residue this finalize has just written, so a resumed recipe re-issues the request against CURRENT
     sources, which is what §Time-travel-resume requires of one anyway. The promises still settle into records
     nothing reads; they hold no frame and no port. */
  eng._inflight.clear();
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
  /* THE DISCARD THAT STOOD HERE WAS NEVER A DISCARD, AND ITS COMMENT SAID OTHERWISE IN SO MANY WORDS: "nothing
     downstream (cache, popup, moat) ever consumes a crashed engine's output". It does. `streamPartial` merges
     every 750 ms snapshot into the cumulative moat as it takes it, so by the time an instance aborts, every
     endpoint and every @S sink it had emitted is ALREADY in globalStore — the four lines below could not
     un-observe them, and un-merging is not a thing this zone can do anyway (a merged endpoint has no owner to
     give it back to). What they actually did was hide those findings from the DOCUMENT that learned them: the
     brain writes `tab._astResults = [analysis]` off this record, so a page that learned a real endpoint and
     then crashed reported `fetchCallSites: []` — a false clean bill on the one surface a user reads, produced
     by the code that was trying to be careful. MEASURED on a mirrored vuejs.org: globalStore held
     `GET media.bitterbrains.com/banners` while its own document reported no endpoints and no sinks.
     THE FINDINGS STAY AND THE RUN IS LABELLED. `_run` is on the record from linesToAnalysis, on every arm, so
     the completeness claims a crash DOES invalidate are refused where each of them is made: no cost counters
     in the run log (above), no frontier write (finish), and a per-document crash marker the popup renders. */
  const result = linesToAnalysis(eng.lines, eng.msg, eng._crashed ? "crashed" : "complete", eng);
  result._fkey = eng.fkey; result._prior = eng.prior;   // engine-computed key + parked entry -> persisted below
  return result;
}
/* A WASM Aborted() is the engine CRASHING — a should-never-happen. It stays scoped to this engine (one page's
   crash must not throw and kill the whole multi-engine scheduler serving the user's other tabs), but it must
   be IMPOSSIBLE to overlook: a LOUD console.error banner, a persistent batch flag, a `_run:"crashed"` record
   on the analysis every consumer reads, and no completeness claim anywhere (no counters, no frontier write).
   NOT a quiet @E buried in resolverErrors — that is how the g_optaint teardown leak hid for so long.
   WHAT IS NOT DISCARDED IS WHAT THE ENGINE ALREADY OBSERVED — see engineFinalize for why the discard this
   comment used to claim was never one. A crash halts trust in the RUN, not in the endpoints it recorded
   before it died and already merged. */
function crashBanner(stage, m) {   // LOUD + a persistent batch flag; EVERY abort path (create/step/teardown) routes here — no crash is ever quiet
  /* TWO SWALLOWING CATCHES ARE GONE FROM THESE TWO LINES, and with them the `|| 0` that made the counter
     create itself. Neither operation can throw — an increment of a declared number and a console write — so
     each `catch (_) {}` could only ever have hidden the ONE thing that matters here: that this is the crash
     path, and a crash that fails to announce itself is exactly the outcome this function exists to prevent.
     The counter is declared at load beside _engineLog, so a reader can tell "no engine has crashed" from
     "bridge.js is not in this zone". */
  self._engineCrashOccurred++;
  console.error("\n==== ENGINE CRASH (" + stage + ") — WASM ABORTED, run marked crashed, NOT swallowed ====\n" + m + "\n");
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
// A crash BEFORE the engine object exists (creation/boot abort). LOUD, and `_run:"crashed"` — never a quiet
// "degenerate result" the reviewer reads as a boring empty page. There are no findings to discard here: the
// instance aborted before it ran a line of the page, which is the one crash that genuinely has nothing to say.
/* THE BANNER IS NO LONGER PART OF THIS FUNCTION, and the split is what one aborted boot costs: `crashBanner`
   increments the crash COUNT, and a reservation may have several documents attached to it (a RESHIP
   re-delivery, a sub-frame, a second arrival that joined it while it booted), each of which needs its OWN
   record because each is answering a different caller. Bundled, answering N callers counted N crashes for one
   instance that failed to boot, and the probe's `crashes` would have read the number of documents that
   happened to share a cluster. engineBootFailed banners once and calls this per caller. */
/* `eng` IS THE RESERVATION THAT FAILED, AND IT IS PASSED BECAUSE A BOOT DOES NOT ALWAYS DIE BEFORE `begin`.
   `engineBootFailed` covers everything `engineRoot` throws, and `begin` is in the middle of it — so a
   reservation reaching here may already have been handed a residue and already have been told how many flows
   came back. Passing `null` would have thrown that away and reported the ONE run where a resumed frontier
   aborted as a run whose resume state was never known, which is the reading that hides exactly the failure a
   cold-tier rebuild is most likely to cause. A reservation that died EARLIER still carries the `null` its own
   literal declared, so this hands the honest answer on both halves without asking which one it is. */
function crashRecord(stage, m, msg, eng) {
  /* NO RESULT DOCUMENT IS EXPECTED HERE, and this is the only caller that may say so: the instance aborted
     before it could answer, so its absence is the crash rather than a broken contract. */
  /* The empties are `linesToAnalysis`'s own, on the arm that has no document to read — not four assignments
     over a record it just built, which is how the two producers of a crash record drifted apart in the first
     place. `_run:"crashed"` is what every consumer reads; there is no second marker beside it. */
  /* AND IT OWNS NO RUN ROW, which is the same split as the banner one line up. This is called PER WAITING
     CALLER for one instance that never booted, and each caller is a different DOCUMENT with a different
     address — so the records cannot share a row, and each states the run that document did not get. The
     instance-level fact (one abort) is the crash COUNT, which is incremented once. */
  return linesToAnalysis(['@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(m) + "}"], msg, "crashed", eng);
}

/* MACROTASK yield (worker/offscreen). Between engine quanta the host MUST return to the event loop with a
   MACROtask (not just an awaited microtask, which the message queue never interleaves with) so the ONE worker
   thread services its message port — triage/GET_STATE evals, postMessage from the offscreen, other timers —
   while a lone engine keeps exploring its byte-identical frontier across qjs_step re-entries. MessageChannel is
   sub-ms, and HTML §8.7 "Timers"' own timer initialisation steps say
   "If nestingLevel is greater than 5, and timeout is less than 4, then set timeout to 4" — a third of a 12ms
   quantum, which would dominate it. §NO BOUNDS: a thread-yield, not a cap.

   AND THE `setTimeout` ARM THAT USED TO STAND UNDER THIS WAS A FALLBACK AND IS DELETED RATHER THAN KEPT, WHICH IS WORTH
   RECORDING BECAUSE IT READ AS A CAPABILITY CHECK AND BECAUSE THE PARAGRAPH ABOVE IT ALREADY SAID WHY IT WAS WRONG. It was
   `const _macroChan = (typeof MessageChannel !== "undefined") ? new MessageChannel() : null;` with `macroYield` taking
   `setTimeout(res, 0)` on the null arm. §C-stack's test settles which it was in one question: delete the thing a predicate
   selects AGAINST and ask whether the predicate is still needed — delete the `setTimeout` arm and the `typeof` test has no
   consumer at all, so it was never routing. **AND THE TWO ARMS WERE NOT TWO SPELLINGS OF ONE ANSWER, WHICH THE COMMENT
   DIRECTLY ABOVE THEM STATED AND NOTHING ACTED ON**: the clamp is a THIRD of this quantum, so a run that took the second arm
   returned the thread to the event loop three times later than the slice asked, on every yield, for the whole run — and
   nothing in any output said which arm it had taken. That is §A-FIELD-A-CONSUMER-DEFAULTS exactly: not a wrong number but a
   PLAUSIBLE one, a run that completes and reports and whose slice was never the slice. The forcing function is the crash.

   IT IS A `CHECK` AND NOT A `DCHECK` BECAUSE THERE IS NO ROUTE BEHIND IT. check.js's law puts an always-fatal assertion where
   the engine must not PROCEED even in production, and with the fallback gone there is nothing to proceed with: the host would
   have no macrotask to return to the event loop on, so a lone engine would never service its port, which is the freeze
   §scheduler names the cooperative quantum to prevent. A DCHECK here would be compiled out in exactly the build that ships. */
const PARTIAL_MS = 750;   // incremental-merge cadence: a hot engine surfaces its current findings this often
CHECK(typeof MessageChannel !== "undefined",
      "this realm has no MessageChannel, so the host has no sub-millisecond macrotask to return to its event loop on " +
      "between two engine quanta — a lone engine would hold the one thread across qjs_step re-entries and never service " +
      "its message port. There is deliberately no setTimeout fallback: HTML §8.7 'Timers' clamps a nested zero-delay " +
      "timer to at least 4ms, a third of the 12ms quantum, and a slice silently three times late is the plausible datum " +
      "this CHECK exists instead of. Build the macrotask source this realm does have, and name it here.");
const _macroChan = new MessageChannel();
function macroYield() {
  return new Promise((res) => { _macroChan.port1.onmessage = () => res(); _macroChan.port2.postMessage(0); });
}
/* ─── THE LEVEL-1 CENSUS ─────────────────────────────────────────────────────────────────────────────────
   THE ORDER THE HOST TOOK, WRITTEN WHERE IT WAS TAKEN. Level-2's census rides the result document because the
   engine composes it; Level-1's cannot, and that is structural rather than an omission — this order is
   composed out of `engineWeight` per HOT INSTANCE and `frontierWeight` per waiting address and cold row, and
   no engine can see another engine. So it is written here, in the trusted zone, at the pick.

   THE UNIT IS A ROUND, NOT A PICK, AND A PICK WOULD BE THE WRONG UNIT FOR THREE REASONS. (1) A pick is a
   `max`, and the defects this instrument exists to expose are not properties of the winner: a rank frozen at a
   constant is a property of the SPREAD (every loser tied with the winner), and a weight deleted from the order
   is a property of what is ABSENT — a per-pick record states neither. (2) One round asks the order in two
   places (`hostSchedule`'s scan over the resident set, and `_bestCandidate` over everything that is not
   resident), and the Level-1 question §scheduler actually poses — is a non-resident item worth more than the
   RAM a resident one holds — is a comparison BETWEEN them, so a per-pick record would split one order into two
   records whose relationship is expressed nowhere. (3) `hostSchedule` is rank-advance-re-rank: the round IS
   the unit in which the order is a consistent set of numbers (every weight in it was recorded by the last
   round with that instance and nothing suspends inside the scan), and any finer unit reports readings taken
   across a suspension as one ordering.

   THREE FACTS, KEPT APART BY PRESENCE AND NEVER BY A ZERO — the same discipline solver/result.h states for
   `_wfq` and for the same reason: a full row of zeroes is a reading of an order that did not exist, and a
   reader taking it would conclude "nothing orders this" about a scheduler that simply had nothing to order.
     · `self._level1 === undefined` — this file did not load; the relay is broken.
     · `self._level1 === null`      — no round has completed in this session.
     · `cands` ABSENT               — the round never ASKED the non-resident order (it spent its advance on a
                                      navigation, or on SEATING a document a previous round's fetch had already
                                      landed, or a reservation or a document-load in flight held admission and
                                      the resident set was under the floor, so neither arm asked). `booting` and
                                      `loading` are what say WHICH of those held it, so a `cands`-absent round
                                      is never three reasons behind one silence.
     · `cands: 0`                   — the order WAS asked and ranked nothing; `exclSub`/`exclLive`/`exclHeld`/
                                      `exclStranded` stand beside it and say what was taken out of it.
     · `cands: n` with rows         — a reading.
   ONE RULE COVERS EVERY CONDITIONAL ROW: a COUNT is present whenever the walk that produces it ran, because a
   count of zero is a reading; a WEIGHT is present only over a NON-EMPTY population, because an extremum over
   nothing is not a number. `wRunner` obeys it too — the runner-up's population is the rankable set minus one.

   A ROUND HAS NO SINGLE INSTANT, SO EACH ROW NAMES ITS OWN AND THE DIFFERENCES BETWEEN THEM ARE FACTS. The
   candidate half is read when the order is ASKED, at the top of the round; the resident half is read at the
   RANK, after the admission; and `pool`/`booting`/`waiting`/`atFloor` are read HERE, when the round ends. So
   `candDocs: 1` beside `waiting: 0` is not two rows disagreeing — it is the round having SEATED that document,
   which is the one thing an admission does, and the difference is the only place it is visible. Collapsing
   them to one instant is not available (a round is a sequence of suspensions by construction) and pretending
   to it would be worse: it would report the pre-admission pool as the pool that was ranked.

   `-Infinity` NEVER CROSSES INTO THIS RECORD, AND THAT IS NOT A ROUNDING. `engine_top_weight` answers
   -Infinity as the engine's POSITIVE statement that its frontier holds no runnable flow (see
   engineRecordFacts), so it is a SENTENCE and not a magnitude: folding it into `wMin` would report a resident
   order whose bottom is 0.3 as one whose bottom is unbounded. It is reported as the population it names,
   `drained`, and the weight extrema are readings of the RANKABLE engines. This also keeps every value here a
   finite number, which matters because the reader is reached through `chrome.runtime.sendMessage`, whose
   message is documented as JSON-ifiable — a -Infinity would arrive as `null` and the consumer's own shape
   assert would fire on a value this producer never wrote. */
let _level1Round = 0;
/* THE DISTRIBUTION OVER ROUNDS, WHICH IS WHAT MAKES A SINGLE OVERWRITTEN RECORD READABLE AT ALL. `_level1`
   holds the LAST round, so every per-round row on it is a GAUGE at an instant THIS LOOP CHOSE — and the
   instant it chooses is the worst one for the question the record is most often asked. A round that steps an
   engine and takes the yield arm sets `state = "fetching"` and then records from its own `finally`, so a
   terminal round of a drive that was still exploring reports `hot: 0` BY CONSTRUCTION, every single time,
   whatever the drive did. MEASURED: one gitpod pass, 60s dwell, `pool: 1, booting: 0, loading: 0, hot: 0` at
   round 34 — a reading consistent with an engine that was unrankable all drive AND with one that was hot in
   every round but the last, and the row cannot separate them.
   SO THE SHAPES ARE COUNTED OVER THE LIFETIME AND RIDE EVERY RECORD, which is the §A-GAUGE-AND-A-LIFETIME-
   COUNTER pair stated as two rows rather than inferred from one: the gauge says where the loop IS and these
   say what it has BEEN DOING, and only the second can answer "was the engine rankable" over a drive.
   THEY ARE AN EXACT PARTITION AND THE ASSERT IS WHAT KEEPS THEM ONE: a round assigns `rd.shape` at the single
   point it leaves the body and `_level1Record` is the ONLY incrementer, so the six cannot drift from each
   other or from `round` — and a round that leaves by a THROW arrives here with no shape and is counted as
   `rThrew`, which is a reading rather than a hole in the sum. Counting at the exits instead would have made
   every future exit a place to forget, which is the shape of defect this project keeps paying for. */
const _level1Shapes = { rNoPool: 0, rIdle: 0, rWaited: 0, rReleased: 0, rFinished: 0, rServiced: 0,
                        /* A ROUND THAT LEFT BY A THROW, AND A ROUND WHOSE EXIT NAMED A SHAPE THIS PARTITION HAS
                           NO ARM FOR, ARE TWO ARMS AND NOT ONE. The second is unreachable until somebody adds
                           an exit, and it exists because the DCHECK that names that edit is COMPILED OUT in
                           release: without it the release build would increment a key that is not here, read
                           `undefined`, store NaN, and hand the popup a non-finite row — so the arm is what
                           keeps the sum exact in the build that cannot assert. Folding it into `rThrew` would
                           have been the two-facts-one-number shape in the one place this record exists to
                           refuse it. */
                        rUnknown: 0, rThrew: 0 };
const _LEVEL1_SHAPE_KEY = { nopool: "rNoPool", idle: "rIdle", waited: "rWaited",
                            released: "rReleased", finished: "rFinished", serviced: "rServiced" };
/* AND THE ONE ARM THAT IS TWO POPULATIONS, SPLIT BY THE ENGINE'S OWN WORD FOR WHICH IT IS. `rServiced` counts
   every round that ended in a service round, and the yield arm is reached for TWO step codes whose benches mean
   OPPOSITE things about this loop. ENGINE_STEP_STALLED is the engine stating that its frontier holds nothing
   runnable, so taking it out of the hot set is what §scheduler prescribes in as many words — "a fetching engine
   is skipped so one doc's wait never stalls another's" — and the round is then entitled to wait on bytes.
   ENGINE_STEP_YIELD is the engine stating that it has a runnable frontier and hit a slice boundary, and the
   bench it gets is IDENTICAL, which is the thing nothing in this tree could measure: §ONE-WFQ-policy says
   level 1 "runs the top until its best flow no longer outranks the runner-up", and an engine that leaves the
   rankable set after one step cannot be run on however far above the runner-up it is.
   SO THE TWO READINGS OF ONE NUMBER ARE A CORRECT MECHANISM AND A CANDIDATE DEFECT, AND THEY TAKE OPPOSITE
   WORK. A high `rServiced/round` is the statement that nearly every round benched an engine, and it is
   consistent with a pool whose every member is legitimately waiting on a body AND with a pool being
   round-robined past an order that had already picked a winner. That is the three-states-behind-one-answer
   shape CLAUDE.md §a-bare-count-over-a-population-you-have-not-partitioned refuses, and the discriminator was
   in a local variable at the moment the shape was assigned and thrown away. The derivation, rather than a
   figure that rots on the next drive: `SITES=apps.tsv node report.mjs` from `testing/corpus`, which DERIVES
   its own pass set (a named set is `--named`, and a bare filename is refused; the `<census files>` placeholder
   this line used to carry was a hand-chosen scope wearing a derivation's clothes, and CLAUDE.md records it
   answering `0` for a field 110 of 119 passes carry)
   prints `<rounds ending in a fetch service>serv/pool<n>` per pass in its ENGINE SPAN block.
   AND THE BENCH IS NOT WHAT COSTS THE SECONDS, WHICH IS RECORDED HERE SO THE NEXT READER OF THESE TWO ROWS
   DOES NOT SPEND A DIFF ON IT. The obvious next move — keep a YIELDing engine rankable while its payment is in
   flight — was refuted by arithmetic over the same corpus before these rows were landed, and the refutation
   needs no new instrument. At pool 1 this loop has nothing else to step, so it WAITS on exactly this bench's
   own latency and `betweenSlicesUs / slices` measures it directly: 8ms, 12ms, 14ms and 63ms over the four
   pool-1 gitpod rows. The SAME CODE runs at pool >= 2, where that figure is 2.1 to 8.5 SECONDS, and it does
   not scale with pool (pool 2 reads ~5s and pool 9 reads ~5s). So removing the yield bench can return at most
   the bench's own latency, which is under one percent of the gap it would be offered to explain, and the
   remainder is the OTHER pool members' own slices — which is a question about why there are seven instances of
   one document, not about this arm. It also carries a correctness objection in its own right: a YIELDing engine
   left in the hot set is re-picked next round (it is the top), so `ops.step` would be issued CONCURRENTLY with
   that engine's own in-flight `serviceFetch` on one renderer port, with `engineRecordFacts` running from two
   async chains and one of them mutating `eng._inflight` while the other reads it.
   RETIREMENT -- THAT CONDITION WAS BORN MET AND IS RE-KEYED, AND THE RETIRED WORDING IS KEPT BECAUSE A READER
   WHO RE-DERIVES IT FROM `_level1Record`'S OWN ROWS WILL WRITE IT AGAIN. It read: "this record goes when the
   pool publishes the SEAT KIND of each member, because the question these rows exist to refine stops being
   'which bench' and becomes 'why is this pool seven engines of one document', which no row in this file can
   answer today". The last clause is FALSE and was false when it was written: `rendererPoolProbe` has published
   `reservations` all along -- `rooted` (an ambient dispatch rooting a cluster), `seeded` (an address an
   application declared), `navigated` (a top replaced in a cluster that had one), `rehydrated` (a cold residue),
   `joinedBooting`/`joinedRooted` (a document attaching to an agent that already has an instance) -- which IS
   the seat-kind partition, as lifetime counters, one command from a live drive. What is true is narrower and is
   about a READER rather than about this file: `testing/corpus/site.mjs` composes the census out of
   `self._level1` and asks `rendererPoolProbe` for none of it, so the archived corpus cannot answer it and a
   lane reading censuses concludes the row does not exist. That is CLAUDE.md §A-RETIREMENT-CONDITION-IS-
   GRAMMATICALLY-FUTURE-TENSE caught by its own prescription -- grep the mechanism a condition names AT THE
   MOMENT YOU WRITE IT -- and the grep that found it was `grep -n _reserveStats extension/bridge.js` against an
   invented sibling reading zero.
   RETIREMENT: this record goes when the corpus census carries that `reservations` block beside `hostRound`, so
   "seven engines of one document" is a row somebody reads rather than a question a lane has to drive for.
   IT IS ITS OWN OBJECT AND NOT TWO MORE ARMS OF `_level1Shapes`, because those sum to `round` by construction
   and are asserted to; these sum to ONE of them. A partition of an arm is not an arm.
   NOTHING HERE DECIDES ANYTHING, AND THE ARM ABOVE STILL DOES NOT READ A STEP CODE TO CHOOSE A BEHAVIOUR —
   see the paragraph at `rd.shape = "serviced"`, which argues that the two codes differ in what they say about
   RANK and that rank is answered by `engine_top_weight` rather than by a second question asked here. That
   argument is untouched: the code is COUNTED and never branched on, so there is still exactly one answer to
   the question of what a service round does. */
const _level1Serviced = { rServicedYield: 0, rServicedStalled: 0,
                          /* AND A CODE THIS PARTITION HAS NO ARM FOR, WHICH IS AN ARM AND NOT A DEFAULT, for
                             exactly `rUnknown`'s reason one object up: the DCHECK that names the edit is
                             COMPILED OUT in release, so without this a widened step enumeration would be
                             counted as a YIELD — a fabricated reading of the one row that exists to separate
                             a correct bench from a defective one — or would key a name that is not here and
                             store NaN. The sum assert covers all three, so the arm is what keeps the
                             partition exact in the build that cannot assert. */
                          rServicedUnknown: 0 };
const _LEVEL1_STEP_KEY = { 2: "rServicedYield", 3: "rServicedStalled" };
function _level1Record(pool, rd) {
  /* THE SHAPE IS COUNTED BEFORE THE ROW IS COMPOSED, so `round` and the arms are read at one instant and the
     identity below is over one set of numbers rather than over two. An empty shape is a round that left the
     body by a THROW: every normal exit assigns one, so the fallback is a POSITIVE statement and not a default. */
  DCHECK(rd.shape === "" || _LEVEL1_SHAPE_KEY[rd.shape] !== undefined,
         "the Level-1 round recorded shape `" + rd.shape + "`, which this partition has no arm for — the arms " +
         "sum to `round` by construction, so an unknown shape is a new exit added to the loop without a row, " +
         "and the dev build refuses it here rather than letting release carry it as `rUnknown` unread");
  _level1Shapes[rd.shape === "" ? "rThrew" : (_LEVEL1_SHAPE_KEY[rd.shape] || "rUnknown")]++;
  /* COUNTED AT THE SAME INSTANT AS THE ARM IT REFINES, for the reason stated over the line above: `round` and
     every distribution riding this record are read once, so the partition below cannot be taken across a
     mutation of the arm it is asserted against. */
  DCHECK((rd.shape === "serviced") === (rd.stepCode !== null),
         "the Level-1 round recorded a service round with no step code, or a step code with no service round — " +
         "the engine's own {YIELD, STALLED} answer is assigned on the same two lines as the shape, so a " +
         "disagreement is a third exit having learned to service without saying which bench it took, and the " +
         "partition below would silently stop summing to the arm it refines");
  if (rd.stepCode !== null) {
    DCHECK(_LEVEL1_STEP_KEY[rd.stepCode] !== undefined,
           "the Level-1 round serviced an engine that answered step code `" + rd.stepCode + "` — the yield arm " +
           "is reached for ENGINE_STEP_YIELD (2) and ENGINE_STEP_STALLED (3) and for nothing else (the round " +
           "enumerates the codes and DONE takes the terminal arm), so a third value here is that enumeration " +
           "having been widened without this partition gaining the arm, which release would carry as a NaN");
    _level1Serviced[_LEVEL1_STEP_KEY[rd.stepCode] || "rServicedUnknown"]++;
  }
  const r = { round: ++_level1Round, pool: pool.length, booting: _bootingCount(),
              /* AND THE SEATS WHOSE DOCUMENT IS STILL ON THE NETWORK, WHICH IS A SECOND REASON ADMISSION WAS
                 NOT ASKED AND THEREFORE A SECOND ROW. Folded into `booting` it would say a reservation was
                 provisioning when none was; left out it would leave `cands` absent beside `atFloor: 0` and
                 `booting: 0`, which reads as a round that asked nothing for no reason at all. */
              loading: _loadingCount(),
              waiting: _waiting.length, hot: rd.hot === null ? 0 : rd.hot.n,
              /* WHICH OF THE TWO ARMS COULD HAVE ASKED THE ORDER, AND THE ONLY THING THAT MAKES
                 `candWMax` AGAINST `wMin` READ AS A DECISION RATHER THAN AS A COINCIDENCE OF TWO NUMBERS:
                 under the floor the comparison is admission's, at it the comparison is eviction's. */
              atFloor: _atRamFloor() ? 1 : 0 };
  if (rd.hot !== null && rd.hot.n > 0) {
    r.drained = rd.hot.drained;
    const rank = rd.hot.n - rd.hot.drained;
    if (rank > 0) { r.wTop = rd.hot.wTop; r.wMin = rd.hot.wMin; }
    if (rank > 1) r.wRunner = rd.hot.wRunner;
  }
  /* THE NON-RESIDENT HALF ARRIVES WHOLE OR NOT AT ALL — see `_bestCandidate`, which composes it in one walk.
     `candAsk` rides with it because a round can legitimately ask the order twice (an admission that reaches
     the RAM floor is followed by an eviction that asks again over the pool the admission changed), and a
     record that reported the later reading without saying so would present two instants as one. */
  if (rd.cand !== null) {
    for (const k of Object.keys(rd.cand)) r[k] = rd.cand[k];
    r.candAsk = rd.candAsk;
  }
  /* THE PRODUCER ASSERTS ITS OWN GRAMMAR HERE, WHICH IS THE OPPOSITE SPLIT FROM `_wfq` AND FOR THE SAME REASON.
     There this zone RELAYS a document another program composed, so it asserts the SHAPE and never the names —
     a name list would be a third copy of solver/flow.h's. Here this zone IS the composer, so the names are
     already stated once, above, and what a consumer must not do is re-state them: popup.js renders whatever
     rows arrive and asserts only the shape, so a row added here reaches a human unedited. */
  for (const k of Object.keys(r))
    DCHECK(typeof r[k] === "number" && Number.isFinite(r[k]),
           "the Level-1 census composed a non-finite `" + k + "` — every row of it is a population count, a " +
           "0/1 state or a weight over a non-empty population, and the one value that is legitimately not a " +
           "number (an engine's -Infinity for a drained frontier) is reported as the `drained` COUNT rather " +
           "than folded into an extremum, so a non-finite here is a term that lost its presence rule");
  /* THE ONE-INSTANT HALF OF THIS, AND THE HALF THAT WAS NEVER A RELATION AT ALL. `booting` and `pool` are
     read on the same line above, so a booting count that outruns the pool IS a reading taken across a
     mutation, which is what this says. `hot` is not: it is the round's own scan, taken before the step, and
     this record's whole doctrine is stated three paragraphs up — "a round has no single instant, so each row
     names its own AND THE DIFFERENCES BETWEEN THEM ARE FACTS". `hot: 1` beside `pool: 0` is the round having
     FINALIZED that engine, which is the one thing a terminal round does; the paragraph's own example is the
     mirror of it (`candDocs: 1` beside `waiting: 0` is the round having SEATED a document).
     SO THIS CLAUSE ASSERTED THAT THE ROUND HAD MUTATED NOTHING, WHICH EVERY TERMINAL ROUND MUTATES BY DESIGN.
     `finish` splices the engine out of the pool and `_level1Record` runs from the round's own `finally`
     afterwards, so a pool whose members were all hot — one tab, the ordinary case — reached this with hot 1
     and pool 0 and aborted, out of an exit that is EVERY exit, taking the Level-1 scheduler down on the round
     that COMPLETED a document. It is the same defect shape as the accounting sum that named two of three
     kinds: an instrument built so a census could not go quiet, killing the loop it measures, on correct code.
     The relation it wanted is structural and holds where it is computed — `hot` is a FILTER of `pool`, a
     subset at the line that makes it, with nothing there left to assert. */
  /* AND THE TWO NOT-YET-AN-INSTANCE POPULATIONS ARE ASSERTED AS ONE STATEMENT RATHER THAN AS TWO `<=`s,
     because what makes them readable together is that they are DISJOINT: a pool member holds exactly one
     state, so their sum is a subset of the pool and a sum that outruns it is a seat counted in both — which is
     the one failure a per-row bound cannot see and which would make `loading` look like a reason admission was
     held when the reservation beside it was the reason. All three are read on the same line above. */
  /* AND THE LIFETIME DISTRIBUTION RIDES EVERY RECORD, UNCONDITIONALLY, because it is not a reading of
     anything this round walked — it is what every round before it did, and its whole value is that it survives
     being read at an instant the loop chose. */
  for (const k of Object.keys(_level1Shapes)) r[k] = _level1Shapes[k];
  for (const k of Object.keys(_level1Serviced)) r[k] = _level1Serviced[k];
  /* THE ARMS SUM TO `round` AND THAT IS ASSERTED RATHER THAN DOCUMENTED, which is the one property that makes
     them readable as a partition instead of as seven numbers that happen to sit together. A reader who sees
     `rServiced` at 3 against `round` at 34 is entitled to conclude the other 31 rounds were NOT service rounds
     only if nothing can be in two arms or in none; this is what says so, and it fires on the one way that
     breaks — a new exit from the loop body with no `rd.shape` on it, which arrives here as a silent `rThrew`. */
  DCHECK(Object.keys(_level1Shapes).reduce((n, k) => n + _level1Shapes[k], 0) === r.round,
         "the Level-1 round shapes sum to " +
         Object.keys(_level1Shapes).reduce((n, k) => n + _level1Shapes[k], 0) + " against " + r.round +
         " round(s) — this function is the ONLY incrementer and every exit of the round body assigns a shape, " +
         "so a sum that disagrees is an exit that assigns none being counted as a throw, or a second caller");
  /* AND THE ONE ARM'S PARTITION SUMS TO THAT ARM, which is the whole of what makes the two rows readable as a
     split of `rServiced` rather than as two numbers that happen to sit beside it. One assignment site, one
     incrementer, one instant — so a disagreement is a service round counted into the arm by something other
     than the line that states its code, and a reader differencing the pair against `rServiced` would be
     attributing the remainder to a bench nobody took. */
  DCHECK(r.rServicedYield + r.rServicedStalled + r.rServicedUnknown === r.rServiced,
         "the Level-1 census splits " + r.rServiced + " service round(s) into " + r.rServicedYield +
         " yield, " + r.rServicedStalled + " stalled and " + r.rServicedUnknown + " unknown — these are one " +
         "arm's own partition, counted from the " +
         "same `finally` as the arm, so a sum that disagrees is a round that serviced an engine without " +
         "relaying the engine's statement of which bench it was given");
  DCHECK(r.booting + r.loading <= r.pool,
         "the Level-1 census reports " + r.booting + " booting record(s) and " + r.loading + " loading seat(s) " +
         "in a pool of " + r.pool + " — all three are read at the same instant, at the end of the round, and " +
         "both populations are DISJOINT members of the pool they are counted against, so a sum that outruns it " +
         "is either a reading taken across a mutation or one seat counted as two reasons admission was held");
  DCHECK(("wTop" in r) === ("wMin" in r) && ("wTop" in r) === (r.drained !== undefined && r.hot - r.drained > 0),
         "the Level-1 census reports a resident order whose extrema and whose rankable population disagree — " +
         "the weights exist exactly when there is a rankable engine to have them, and a `wTop` beside a " +
         "wholly drained hot set is a rank invented for an order every member of which said it had none");
  DCHECK(!("wRunner" in r) || (r.hot - r.drained > 1 && r.wRunner <= r.wTop && r.wRunner >= r.wMin),
         "the Level-1 census reports a runner-up that is not one — it is the SECOND-highest rankable weight, " +
         "which is the value yield floor the winner is handed, so a runner-up outside [wMin, wTop] or beside " +
         "a rankable set of one is the scan that found it counting the winner as its own runner-up");
  DCHECK(!("wTop" in r) || r.wMin <= r.wTop,
         "the Level-1 census reports a resident order whose bottom outranks its top — these are the extrema " +
         "of one scan over one array of weights, so an inversion is two scans over two sets");
  /* THE NON-RESIDENT HALF ARRIVES WHOLE, ASSERTED AGAINST THE WALK'S OWN ROW SET RATHER THAN AGAINST A LIST
     KEPT HERE. This named two of the eight rows `_candCensus` declares — the two somebody thought of — so it
     was a check that passed for exactly the shapes it was meant to reject: an exclusion row added to the walk
     and dropped on the way here would have been an order reporting that it excluded nothing. `_candCensus()`
     is the one statement of which rows a walk that RAN produces, so this cannot go stale. */
  for (const k of Object.keys(_candCensus()))
    DCHECK(("cands" in r) === (k in r),
           "the Level-1 census carries part of the non-resident order — `" + k + "` and the total are one " +
           "walk's output and arrive together, so a census holding one half would report an order with no " +
           "exclusions as one that excluded nothing, or a population as an order that was never asked");
  DCHECK(("cands" in r) === ("candAsk" in r),
         "the Level-1 census holds a non-resident order without saying which of the round's two arms asked " +
         "it (or says so with no order to qualify) — under the floor the comparison against `wMin` is " +
         "admission's and at it it is eviction's, so a reading with no ask is two instants presented as one");
  DCHECK(!("candAsk" in r) || (r.candAsk === 1 || r.candAsk === 2),
         "the Level-1 census says the non-resident order was asked " + r.candAsk + " time(s) in one round — " +
         "there are exactly two arms that ask it (admission under the floor, eviction at it) and each asks " +
         "at most once, so anything else is an ask this record did not see and whose reading it is not holding");
  /* THE POPULATIONS ACCOUNT FOR THE TOTAL, SUMMED OVER THE DECLARED KINDS AND NEVER OVER A SUM WRITTEN HERE.
     `cands === candDocs + candCold` stood here after a THIRD kind — an address an application declared is a
     page of itself — was already being ranked into the total, so this assert fired on healthy code at the
     first bundle that named a route it does not link, from the round's own `finally`, which is every exit:
     the instrument built so a frozen rank could not hide killed the scheduler it was measuring. A sum over
     CAND_KINDS is the same statement that cannot be left behind, and it is still a real check — a kind
     counted into the total by something other than `_candRanked` has no population and fails here. */
  DCHECK(!("cands" in r) || r.cands === CAND_KINDS.reduce((n, k) => n + r[k.pop], 0),
         "the Level-1 census ranked " + r.cands + " candidate(s) and its populations account for " +
         CAND_KINDS.map((k) => k.pop).join(" + ") + " of them — every work item with no instance is one of " +
         "the declared kinds, so a total that outruns them is an item counted into the order and accounted " +
         "for by no population, which the admission would then build through whichever arm is the fallback");
  DCHECK(CAND_SPREAD.every((s) => (s.row in r) === ("cands" in r && r.cands > 0)) &&
         CAND_KINDS.every((k) => (k.wMax in r) === ("cands" in r && r[k.pop] > 0)),
         "the Level-1 census reports a spread over an empty candidate population, or omits one over a " +
         "population that has members — the ABSENCE of a reading is how this record says there was nothing to " +
         "rank, so a `candWMax: 0` beside `cands: 0` is a rank fabricated for an order with no members and a " +
         "missing one beside `cands: 3` is three items ranked by nothing");
  DCHECK(!("candWMax" in r) || (r.candWMin <= r.candWMax && r.candRewardMin <= r.candRewardMax),
         "the Level-1 census reports a candidate order whose lowest-ranked item outranks its highest — one " +
         "walk produces both, so an inversion is the spread being taken over a different set from the pick, " +
         "and the spread is what can show a rank frozen at a constant");
  /* THE SPREAD ROWS ARE READINGS OF THE SAME SET, AND THE THING THAT MAKES THEM ONE READING IS ASSERTED. A
     candidate that has never been served contributes reward 0 and the full optimism bonus, so `candUnserved`
     is a subset of the ranked population and it is exactly the population `candVisMax === 0` describes when it
     is ALL of it. Without this the two rows could be taken over different walks and the popup's discriminator
     — a tie at the entry value is the order's floor, a tie above an item with history is a frozen rank —
     would be composed of two facts about two sets. */
  DCHECK(!("cands" in r) || (r.candUnserved <= r.cands &&
                             (r.cands === 0 || (r.candVisMax === 0) === (r.candUnserved === r.cands))),
         "the Level-1 census reports " + r.candUnserved + " never-served candidate(s) of " + r.cands +
         " with a maximum visit count of " + r.candVisMax + " — those two rows are one walk's reading of one " +
         "set, and they are what separates an order sitting at its entry value (every item unserved, so a tie " +
         "at 1.0 is correct) from a rank frozen at a constant (an address with history ranking at that same " +
         "1.0), so a disagreement makes the census unable to tell the healthy case from the defect");
  self._level1 = r;
}

/* THE PURE SCHEDULER POLICY (no wasm knowledge — engine ops are injected, so this is unit-testable with
   mock engines). Each iteration: ADMIT waiting documents up to the RAM cap (ops.admit gates creation — no
   instance is built until a slot is free), then advance the highest-weight HOT engine and re-rank. Before
   stepping it the host sets its VALUE yield-floor to the RUNNER-UP engine's weight (ops.setFloor), so the
   engine runs until it's outranked then yields HOT — no fixed slice count (a banned step-cap). Slots turn
   over because each engine self-parks to the cold tier (IDB recipe) under RAM pressure (ops.requestPark). */
async function hostSchedule(pool, ops) {
  for (;;) {
    /* THE ROUND'S READING, COLLECTED BY THE ROUND AND WRITTEN ONCE. Declared before the body and published in
       `finally` so that EVERY exit records — the two `break`s, the `continue` that waits on a reservation, and
       a round that dies inside an op (which records the half it had reached, and whose other half is then
       ABSENT rather than zero, which is what `_hostDead` beside it is read against). One write site is the
       whole of "one census, one place": there is no arrangement of this loop in which the order is taken and
       nothing records it, which is precisely how a Level-1 rank frozen at a constant survived. */
    /* `stepCode` IS NULL FOR EVERY ROUND THAT DID NOT SERVICE, AND THAT IS A POSITIVE STATEMENT RATHER THAN A
       HOLE: the only arm that assigns it is the one that assigns `shape = "serviced"`, on the same two lines, so
       the two are a biconditional and `_level1Record` asserts it. A round that leaves by a THROW anywhere ahead
       of that arm arrives with both absent, which is `rThrew` and is already a reading. */
    const rd = { hot: null, cand: null, candAsk: 0, shape: "", stepCode: null };
    try {
    if (ops.admit) rd.cand = await ops.admit();   // gate creation to cap: seat waiting docs into freed slots
    /* ADMISSION ANSWERS WITH THE ORDER IT TOOK, OR WITH THE POSITIVE `null` THAT SAYS IT TOOK NONE. `undefined`
       is neither — it is an admission arm that stopped answering, and it would reach the record as a
       half-filled candidate half rather than as an absent one, which is the one distinction this census is
       built to keep. Asserted at the seam and not at the composer, where the caller's identity is gone. */
    DCHECK(rd.cand === null || (rd.cand && typeof rd.cand === "object" && Number.isInteger(rd.cand.cands)),
           "the admission op answered the round with something that is not a candidate-order reading — it " +
           "returns the census `_bestCandidate` composed, or null where it never asked the order, and an " +
           "undefined answer is an arm that returns nothing being read as an order that was never taken");
    if (rd.cand) rd.candAsk++;
    if (!pool.length) { rd.shape = "nopool"; break; }
    const hot = pool.filter((e) => e.state === "hot");
    if (!hot.length) {   // every live engine is mid-something: wait for the earliest to become hot, then re-rank
      rd.hot = { n: 0, drained: 0 };   // a rankable set of none is a READING; the census omits the weights, not the row
      /* THREE STATES REACH THIS ARM AND THEY ARE ONE KIND OF THING: not rankable YET, on a promise that says
         when. An engine awaiting a reply body is one; a RESERVATION whose instance is still being provisioned
         is the second, and it was missing. Without it an empty hot set with a booting engine in the pool fell
         through to the `break` below — and the pool is NOT empty (the reservation is in it), so _hostKick's
         `finally` re-entered immediately: a full-speed spin through admit on a condition that only the boot
         it refused to wait for could change.
         THE THIRD IS AN ADMISSION WHOSE DOCUMENT IS STILL ON THE NETWORK (`loading`), AND ADDING IT IS WHAT
         MOVED A REMOTE BODY OFF THIS ROUND. `ops.admit()` used to AWAIT a declared route's §7.4 navigation and a
         shed residue's re-derivation, one level above any service round, so a seeded address whose server holds
         its body open froze THIS LOOP rather than one instance: no engine ranked, stepped or serviced again for
         the rest of the session, including every instance that was perfectly healthy, and the census stopped
         being written at all because no round began. The fetch is a pool member now, so the round returns, the
         hot set is ranked and stepped, and only when NOTHING is rankable does the loop wait here — on a promise
         that resolves whether the bytes arrive or the load refuses. Nothing is bounded: the wait is the same
         wait a reservation gets, and the next iteration re-asks. */
      const pending = pool.filter((e) => e.state === "fetching" || e.state === "booting" ||
                                         e.state === "loading");
      if (!pending.length) { rd.shape = "idle"; break; }
      for (const e of pending)
        DCHECK(e._readyP && typeof e._readyP.then === "function",
               "an engine in state `" + e.state + "` carries no readiness promise — this arm is the only " +
               "thing that resumes the pool when nothing is hot, so an engine it cannot wait on is one the " +
               "loop spins on or abandons, and both are silent");
      rd.shape = "waited";
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
    /* MATERIALIZED, WHICH IS WHAT MAKES "ONE READING PER ENGINE" STRUCTURAL RATHER THAN A PROPERTY OF HOW THIS
       LOOP HAPPENS TO BE WRITTEN. The census below is a second question of the same set, and asking
       `ops.weight` again for it would reintroduce exactly the defect the paragraph above records: two passes
       ranking against two answers. Every number this round reports about the resident order comes out of this
       one array. */
    const ws = hot.map((e) => ops.weight(e));
    let best = hot[0], bestW = ws[0], runner = -Infinity;
    for (let i = 1; i < hot.length; i++) {
      const w = ws[i];
      if (w > bestW) { runner = bestW; best = hot[i]; bestW = w; }
      else if (w > runner) runner = w;
    }
    /* THE READING OF THE RESIDENT ORDER, TAKEN OVER THE RANKABLE ENGINES. `-Infinity` is an engine's own word
       for a frontier holding no runnable flow, so it is counted (`drained`) and never folded into an extremum;
       `wRunner` is the SECOND-highest rankable weight, which is the value yield floor the winner is handed
       whenever nothing is drained. `_level1Record` attaches each of these only over a population that has
       members — see the presence rule stated there. */
    let _rk = 0, _top = 0, _min = 0, _second = 0;
    for (const w of ws) {
      if (!Number.isFinite(w)) continue;            // drained: a sentence, counted as one, never an extremum
      if (_rk === 0) { _top = _min = _second = w; }
      else {
        if (w > _top) { _second = _top; _top = w; }
        else if (_rk === 1 || w > _second) _second = w;
        if (w < _min) _min = w;
      }
      _rk++;
    }
    rd.hot = { n: hot.length, drained: hot.length - _rk, wTop: _top, wMin: _min, wRunner: _second };
    /* THE RANKING ITSELF IS STILL SYNCHRONOUS, WHICH IS WHY IT IS STILL A RANKING. Every `ops.weight` above is
       a number the last round with that instance RECORDED (8196a0e7), so the whole scan runs on one consistent
       set of values with no suspension in it. The three calls below DO suspend — each is a message to a frame —
       and the pick they act on is therefore as of the top of this iteration, which is exactly what the policy
       says it is: rank, advance the winner, re-rank. Nothing between here and the step can change the set, because
       the only thing that moves an engine between hot and fetching is this loop, and a detached service round
       touches only the engine it belongs to (which is not in `hot`). */
    if (ops.setFloor) await ops.setFloor(best, hot.length > 1 ? runner : -1e300);   // outranked-by-runner-up => yield; lone engine => run on
    /* Normally step `best` (the highest-value engine). At the RAM floor, step the engine the ONE order says
       must give up its memory — after flagging it to PARK, which evicts it to the IDB cold tier (residue ->
       replay recipes) so the work item that outranks it can have the RAM. Parking needs a step (the flag is
       read inside qjs_step) and `best` never steps that engine, so it is targeted directly.
       THE CONDITION THAT USED TO SELECT IT IS DELETED, AND IT WAS THE DEADLOCK. `hot.length > 1` said a LONE
       over-budget engine is never parked because "no slot contention, it runs to completion" — and on a real
       site an engine does not run to completion. A wasm Memory never shrinks, so the first instance to touch
       the floor closed admission for the whole extension for the rest of the session: measured at pool = 1,
       residentBytes 539,820,032 against a 512 MiB floor, 142 documents waiting, and topWeight and
       residentBytes byte-identical over six minutes. §scheduler: "STARVE means deprioritize-and-page
       (resumable, cross-session), NEVER terminate" — and the cold tier exists precisely so that the ONLY
       engine can still yield its residue. There is no count in the question any more: `ops.evictee` asks the
       one Level-1 order whether anything that is NOT resident is worth more than the worst thing that IS. */
    let target = best;
    DCHECK(typeof ops.evictee === "function" && typeof ops.requestPark === "function",
           "the pool was driven with no eviction op — the RAM floor is answered by the WFQ giving up the " +
           "lowest-value resident engine, so without it the floor is answered by refusing every admission " +
           "forever and one document holds the whole extension");
    DCHECK(typeof ops.release === "function",
           "the pool was driven with no release op — a Clear cannot take the frame of an engine this round " +
           "has a call outstanding on, so this round is what gives it back, and without it every Clear during " +
           "an analysis leaves a whole WASM instance resident under a document that does not reload");
    /* AND THE ORDER IT ASKED, IF IT ASKED ONE. Admission asks the non-resident order where there is headroom
       and eviction asks it at the floor, so a round usually asks it ONCE — but not always, and the exclusivity
       that looks obvious here is FALSE: `admit` can seat a document and the instance it boots can be what puts
       the working set at the floor, so the very next line asks the same order again over a pool that has
       changed underneath it. The record holds the LATER reading, because that is the one the eviction
       comparison (`cand.w > engineWeight(worst)`) was actually made against — and `candAsk` states that the
       round asked twice, so a superseded reading is never a silent one. An assert that the two are exclusive
       would have fired on healthy code at exactly the RAM pressure this instrument exists to watch. */
    const ev = await ops.evictee(hot);
    DCHECK(ev && typeof ev === "object" && "evict" in ev && "cand" in ev,
           "the eviction op answered the round with something other than the pair it decides — the engine " +
           "that must give up its RAM (or null) AND the reading of the non-resident order it decided that " +
           "against (or null where it never asked), so a bare answer is the round's one look at that order " +
           "at the floor going unrecorded, which is the instant it matters most");
    if (ev.cand) { rd.candAsk++; rd.cand = ev.cand; }
    if (ev.evict) { target = ev.evict; await ops.requestPark(ev.evict); }
    /* THE PARK FLAG AND THE STEP THAT READS IT STAY ORDERED ACROSS THE BOUNDARY, and not by luck: both are
       calls on ONE renderer's port, which is point-to-point and delivers in order, and this loop makes no other
       call into that instance in between. The pair was atomic when it was two ccalls; it is sequenced now. */
    const st = await ops.step(target);
    /* THE STEP CODE IS THREE VALUES AND EVERY ONE OF THEM IS THE ENGINE'S OWN STATEMENT. qjs_step answers
       ENGINE_STEP_DONE (0), ENGINE_STEP_YIELD (2) or ENGINE_STEP_STALLED (3). It used to answer two — it FOLDED
       the stall into the yield, "the bridge speaks two values" — and the fold is deleted because a host cannot
       undo it: a yield asks to be OUTRANKED and a stall asks to be PAID, so one value for both leaves every
       driver guessing. The one that guessed wrong is engine/route.mjs, whose pump had exactly two terminators
       and therefore stepped a stalled peer 10.8 million times with no switches, no jobs and no emission.
       THE ONE BEFORE IT WAS THE SAME DEFECT INVERTED, and it is why this branch is spelled out rather than left
       as `if/else`: the third arm here once tested for 1, a NEED_FETCH code no version of engine_sched_step has
       ever returned, and it was the ONLY caller of ops.serviceFetch — so the whole reply path
       (qjs_pending -> safeFetch -> qjs_provide) was unreachable in the shipped extension, every flow a page's
       `fetch()` parked stayed parked, and the analysis promise for that document never resolved. A value the
       engine cannot produce wearing a branch someone had thought about, and a value the engine CAN produce
       with no branch at all, are the two halves of one rule: the codes are enumerated, never defaulted. */
    DCHECK(st === 0 || st === 2 || st === 3,
           "qjs_step answered with a code outside {DONE, YIELD, STALLED} — a fourth value is an ABI that " +
           "changed under a host still speaking the old one");
    // DEV __forcepark: request the park only after N dispatches, so it captures MID-EXPLORATION residue
    // (recipes with real decvecs + handler-driven async flows), mirroring a production RAM-pressure park.
    /* AND ONLY WHILE THERE IS A SESSION TO PARK, which is the SAME statement `st !== 0` makes on the line
       below and for the same reason: DONE means the step DRAINED the frontier and closed the session inside
       itself, so a park asked after it is asked of an engine that has nothing left to write — and the engine
       aborts saying exactly that, because storing an empty residue over a real one is the cold-tier corruption
       the park exists to prevent. It is not a rare race: the counter is 2, and a small dev fixture reaches its
       last program well inside two dispatches, so the step that decrements to zero is the very step that
       drained. A DONE engine wants no park; this round's release ends it. */
    if (st !== 0 && target._forceparkSteps > 0 && --target._forceparkSteps === 0) await ops.requestPark(target);
    // INCREMENTAL MERGE: a lone UNBOUNDED engine never reaches st===0, so without this its already-emitted
    // breadth surfaces only at finalize. Snapshot + merge on a coarse cadence on every non-final step.
    /* AWAITED, AND THAT IS A CORRECTNESS REQUIREMENT RATHER THAN TIDINESS. streamPartial asks the engine to
       PRINT a result document and then finds it by INDEX in the line buffer and splices it out. The print
       arrives on that call's own reply, so the scan must not run until the call has answered — and the service
       round started below appends to the same buffer, so an unawaited partial would splice by an index the
       round had already moved. Fire-and-forget was sound only while the ccall was synchronous. */
    if (st !== 0 && ops.streamPartial) await ops.streamPartial(target);
    /* AN ENGINE THAT LEFT THE POOL MID-ROUND IS NOT CARRIED ANY FURTHER, AND THIS ROUND IS THE ONE THAT OWNS
       ITS FRAME. Every op above suspends, and the only thing that takes an engine out of the pool while this
       loop is holding it is a Clear — which cannot take the frame with it (a renderer with a call outstanding
       may not be destroyed) and so hands that to whoever owns the outstanding call, exactly as the service
       round is already handed it. Without this the round carried straight on into `finish` on an engine the
       user had just wiped: the finalize would ask a dead instance for a result and write this origin's residue
       back into a cross-session frontier that had just been emptied, and the waiters it answers were rejected
       by the Clear before it got there.
       IT IS ONE CHECK AND IT IS PLACED LAST, after every op that can suspend, because a drop that lands in
       `setFloor`, in `step` or inside `streamPartial`'s own await is the same drop and must not need its own
       site to notice it — the ops above are each safe on a dropped engine (the renderer is still there,
       because the Clear could not take it) and streamPartial refuses to MERGE one, which is the only thing
       any of them does that outlives the round.
       The membership test is the POLICY'S own — `pool` is this function's array — so the pure scheduler stays
       free of any knowledge of frames; `release` is what turns that into a teardown. */
    if (pool.indexOf(target) < 0) { rd.shape = "released"; await ops.release(target); continue; }
    if (st === 0) {   // fully explored, or self-parked under RAM pressure: finalize (residue -> IDB cold tier)
      rd.shape = "finished";
      await ops.finish(target);
    } else {   // ENGINE_STEP_YIELD (a cooperative quantum) or ENGINE_STEP_STALLED (a bill) — see below.
      /* PAY EVERYTHING THE ENGINE SAYS IT IS OWED, in ONE round: the replies parked flows wait on, the lazy
         chunks, the notices this zone must act on and the synchronous requests only it can answer. Servicing
         only the REQUESTS (which is what this did) left the fetch half of the same owed list unpaid forever.
         THE TWO CODES TAKE THIS ARM TOGETHER, AND THAT IS A DECIDED ANSWER RATHER THAN A DEFAULT. engine.c
         states the schedule: this host pays at EVERY slice boundary and not only at a stall, because paying
         only at a stall makes one flow's reply conditional on every other flow in the document also becoming
         blocked — a cross-flow coupling that gets worse as exploration succeeds, since every fork adds a
         member that must also block and re-issues its parent's unanswered request. So the PAYMENT does not
         differ, and the two codes differ in what they say about RANK, which this loop does not read off a step
         code at all: engine_top_weight already publishes -Infinity for a frontier whose every member is
         host-owed, so a stalled engine sorts last through the ordering that exists rather than through a
         second question asked here. Two answers to one question is the defect, not the shared arm.
         WHAT WOULD BE A DEFAULT is treating an unknown code this way, which the enumeration above refuses.
         NON-BLOCKING, the way the unreachable branch was: the engine drops out of the hot set while its round
         runs, so a slow reply on this document never stalls another's. */
      rd.shape = "serviced";
      /* THE ENGINE'S OWN WORD FOR WHICH OF THE TWO THIS ROUND WAS, RELAYED AND NEVER RE-DERIVED. The bench on
         the next line is the same for both codes and means opposite things for each, so this is the ONE fact
         that makes `rServiced` readable — see `_level1Serviced`. It is recorded and not branched on. */
      rd.stepCode = st;
      target.state = "fetching";
      /* ONE FIELD FOR ONE QUESTION, WHICH IS "WHEN DOES THIS ENGINE BECOME RANKABLE AGAIN". It was `_fetchP`,
         and a boot is not a fetch — naming the reservation's provisioning promise after the reply round would
         be one name answering two different facts, which is how the wait arm above would come to be read as
         "wait for a body" by the next person to add a state to it. */
      /* AND THE ENGINE'S OWN STALL STATEMENT TRAVELS WITH THE ROUND, because the round is the only thing that
         may wait on bytes and it may do so ONLY when this engine has nothing runnable. ENGINE_STEP_STALLED is
         that statement — the step above answered it — and it is RELAYED rather than re-derived inside the
         round, which could only ask the engine a second time and get a second answer. The round with a
         runnable frontier returns at once, which is the whole of what stops one never-ending reply body from
         taking a document out of this hot set for the rest of the session. */
      target._readyP = ops.serviceFetch(target, st === 3).then(
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
    } finally { _level1Record(pool, rd); }
  }
}

// ---- Engine-bound ops + the live pool ----
const _pool = [];        // BOOTING/hot/fetching engines — one record per agent cluster, bounded by the RAM floor
/* documents awaiting a slot: { html, msg, persist, resolve, reject } — NO instance built yet. `code` stood in
   this list and no writer has ever put one here (the entry that fills it passes the empty string explicitly at
   the one call site that needs one), and `html` is the field BOTH consumers must read: the admission arm hands
   it to engineCreate and the navigation arm hands it to engineJoin. The message beside it carries no document
   — that entry asserts so — so a consumer reaching for `msg.pageHtml` on one of these jobs is reaching for a
   field this table's own producer is forbidden to write. */
const _waiting = [];
/* ADDRESSES AN APPLICATION HAS DECLARED ARE PAGES OF ITSELF, waiting to be loaded — the third kind of Level-1
   work item, beside a document that has bytes and a parked frontier that has recipes.
   WHAT PUTS ONE HERE. An engine reached HTML §7.4.4 "Non-fragment synchronous \"navigations\""'s URL and
   history update steps — `history.pushState`/`replaceState`, which is how every client-side router says "this
   address is a page of my app" — and announced the address (solver/route_seed.h, the `document.seed` notice).
   That is the surface §What-the-tool-produces exists for and the one forced execution could not reach: a route
   the bundle NAMES and no link exposes is not a navigation, so nothing created a navigable for it and nothing
   ever loaded it.
   IT HOLDS NO BYTES, AND THAT IS THE WHOLE REASON IT IS ITS OWN REGISTER RATHER THAN A `_waiting` ENTRY. A
   waiting document was already fetched by whoever produced it; a declared address has not been fetched by
   anyone, and CLAUDE.md §A-SELF-SEEDED-DOCUMENT puts the fetch under the ORDER: "an outbound request is an
   EXTERNAL EFFECT and therefore a cost the WFQ carries like any other, so an unproductive document sinks
   beneath productive work instead of being re-fetched at the same rank for ever". Loading at the notice would
   spend one request per declaration, unranked, at the instant a router happened to run — which is precisely
   the shape that sentence refuses. So the load happens at the ADMISSION, exactly where a shed cold entry's
   re-derivation happens and for the same stated reason ("the order reads two numbers; only the item it PICKS
   is deserialized").
   KEYED BY ADDRESS, AND THAT IS NOT A SEEN-SET. §NO BOUNDS names a visited-set, a crawl depth, a page budget
   and a same-URL check as caps wearing a crawler's vocabulary, and none of them is this: an address declared
   twice while it is still waiting is ONE work item declared twice, so the second declaration adds nothing to
   do; an address ADMITTED leaves this map and a later declaration puts it back, so re-fetching an address is
   permitted exactly as §Time-travel-resume requires. MEMBERSHIP is never refused — what decides whether the
   fetch is spent now is the weight, which is the address's own demonstrated surface per admission.
   IT IS IN MEMORY AND THAT IS NOT A LOSS. CLAUDE.md's re-derivable tier is the argument: a declaration's
   RECIPE outlives its bytes, and here the recipe is the DECLARING DOCUMENT'S OWN RESIDUE — a resumed session
   replays that document, its router runs again, and the address is declared again. Persisting the set would be
   storing what a replay reproduces, which is the tier's own definition of what to shed first. */
const _seeds = new Map();   // absolute address -> { url, principalUrl, principalOrigin, provenance }
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
   run ever had two provisionings in flight at once.
   `evicted` AND `rehydrated` ARE THE TWO ENDS OF THE RAM FLOOR, counted apart because their DIFFERENCE is the
   only thing that says whether the floor is doing its job or churning. One eviction that is never rehydrated
   is a document that gave its memory to a better one; an eviction and a rehydration of the SAME item, round
   after round, is the Level-1 ratchet solver/flow.h names (a resident engine's weight ages by CPU without
   bound while a parked item's estimate does not, so a mature document sinks below every page that arrives
   afterwards and its own cold row then wins it straight back). Nothing here truncates that — it is measured
   so the primitive flow.h asks for, start-time fair queueing's virtual time applied at LEVEL-1, is built
   against a number rather than against a suspicion. */
/* `navigated` IS ITS OWN NUMBER AND NOT A THIRD KIND OF JOIN. A join ADDS a document to an agent; a navigation
   adds one AND deactivates the one it replaced (HTML §7.4.6.1 "Updating the traversable"), so folding it into
   `joinedRooted` would make the count that is supposed to say "how many same-origin sub-frames did this
   session model" answer with the number of link clicks as well. It is also the one number that says continuous
   browsing is working at all: this used to be the abort that killed the scheduler, so a session with several
   tab navigations and a zero here is one where every one of them went somewhere else. */
const _reserveStats = { made: 0, rooted: 0, failed: 0, joinedBooting: 0, joinedRooted: 0, peakBooting: 0,
                        evicted: 0, rehydrated: 0, navigated: 0, seeded: 0 };
/* WHICH LIVE DOCUMENTS THESE FINDINGS BELONG TO. The merge in the trusted zone used to be handed a sourceUrl
   and nothing else, so it had no document to merge INTO and built a throwaway view instead — a producer whose
   consumer was an object discarded on return, which is exactly the shape `_emptyDocView`'s own comment
   forbids. A sourceUrl cannot stand in for the identity: `documentId` is the ONLY document key in this system
   (a tab holds many documents and a (tab,frame) pair is reused across navigations at a DIFFERENT origin), so
   the name has to be carried, not re-derived.
   THE LIST IS THE WAITERS, WHICH IS THE SAME SET THE TERMINAL RESULT IS RESOLVED TO — one instance holds one
   agent cluster and several browser documents can join it (a same-origin sub-frame, a re-delivery), and every
   one of them is answered with the same finalized analysis, so a snapshot of the run belongs to all of them
   too. EMPTY IS A POSITIVE STATEMENT AND NOT AN ABSENCE: a child navigable the engine announced and a
   rehydrated cold recipe have no live caller by construction (`_cold`), and their findings are the moat's
   alone. */
function engineLiveDocumentIds(eng) {
  DCHECK(eng._cold === (eng._resolvers.length === 0),
         "an instance disagrees with itself about whether it has a live caller — `_cold` is declared at " +
         "engineCreate and the waiter list is what finalize resolves, so a cold engine holding a waiter " +
         "would answer a document nobody is running, and a live one holding none would merge this page's " +
         "findings to the moat alone and report the document that asked for them as clean. The one way a " +
         "LIVE engine loses its waiters is hostClear, which also drops it from the pool and marks it — so " +
         "this firing says an engine that was dropped is still being driven, and the fix is at the round " +
         "that kept driving it, not here. That is exactly what it caught: a Clear landing inside " +
         "streamPartial's own await left a live instance with no waiters, and the merge on the next line " +
         "aborted the trusted zone rather than reaching the store. streamPartial refuses a dropped engine " +
         "before it builds this list; a firing here now means some OTHER round is doing what that one was");
  return eng._resolvers.map((w) => {
    DCHECK(w.msg && w.msg.documentId,
           "a live caller waiting on this instance carries no documentId — astDispatch asserts the browser's " +
           "name for every document it seats, so a waiter without one is a page whose findings have nowhere " +
           "to be merged and would be reported as analysed and empty");
    return String(w.msg.documentId);
  });
}
/* THE SEAT FOR AN ADMISSION WHOSE DOCUMENT IS STILL ON THE NETWORK — the fourth pool state, and the one thing
   that keeps a remote body out of the Level-1 round. `ops.admit()` is called at the TOP of every round, so any
   suspension inside it is a suspension of the whole across-documents loop: while it stands nothing is ranked,
   nothing is stepped, nothing is serviced and `_level1Record` is not reached, because the round that would have
   written it has not begun. Two of admission's arms fetched a DOCUMENT there — a declared route's §7.4
   navigation and a shed residue's re-derivation — and a body a remote party never ends is therefore the one
   stall in this zone that takes every healthy instance down with it. §NO BOUNDS forbids the deadline that would
   have hidden it: a deadline could only ever truncate a reply that was merely slow.
   A SEAT IS NOT A RESERVATION AND IT DELIBERATELY ANSWERS FOR NO DOCUMENT. It carries no cluster and no
   document id, because neither is known until the response has landed (`clusterKeyOf` reads the message and the
   message's address is the RESPONSE's after a redirect) — so `hostClusterOf` and `hostHolderOf` cannot match it,
   which is correct rather than a gap: both kinds mint a group id nothing else can key to, and `admitSeatLand`
   asserts that where the message finally exists. What it DOES carry is the three fields every pool walk reads
   without asking the state first (`_resolvers` for a Clear, `joinedDocIds` for the routing walk, `_readyP` for
   the wait arm), so no walk has to learn about it in order not to crash on it.
   IT NEVER REJECTS, AND THE ERROR IS RELAYED RATHER THAN SWALLOWED. A throw out of the load is an invariant
   abort — `navigationLoad` DCHECKs its principal and `safeFetch` asserts its own contract — and today it
   travels out of `ops.admit()` into `hostSchedule`'s failure arm, which latches `_hostDead` and banners once.
   Rejecting HERE would reach nobody: the wait arm races this promise only when nothing is rankable, so on every
   other round it would be an unhandled rejection with the loop still re-kicking, which is the 23,163-abort
   shape `_hostKick`'s split exists to prevent. So it is stored and re-thrown BY THE ROUND that lands the seat,
   at which point the failure path is byte-identical to the one the await had. */
function admitLoadSeat(what, load) {
  DCHECK(what !== null && typeof what === "object" && (what.kind === "seed" || what.kind === "cold"),
         "an admission seat was taken for a work item this zone has no landing arm for — `admitSeatLand` " +
         "dispatches on that word, so a seat outside the two kinds is a slot that blocks admission and can " +
         "never be finished by anybody");
  DCHECK(typeof load === "function",
         "an admission seat was taken with no load to perform — the seat exists so the round can RETURN while " +
         "a document arrives, so one with nothing arriving is a slot nothing will ever clear");
  const seat = { state: "loading", what: what, landed: null, failed: null, settled: false,
                 /* ABSENT BY VALUE RATHER THAN OMITTED, so a walk that reads one gets `null` and not a
                    TypeError: `hostHolderOf` compares `docId` and indexes `joinedDocIds`, `hostClusterOf`
                    compares `cluster`, `_bestCandidate` reads `msg && msg.sourceUrl`, and hostClear iterates
                    `_resolvers`. None of them asks the state first, and none of them may match a seat. */
                 cluster: null, docId: null, topDocId: null, joinedDocIds: [], msg: null, r: null,
                 _resolvers: [], _cold: true, _readyP: null };
  seat._readyP = (async () => {
    try { seat.landed = await load(); }
    catch (e) { seat.failed = e; }
    finally { seat.settled = true; }
  })();
  _pool.push(seat);
  return seat;
}
/* AND THE LANDING, WHICH IS EVERY DECISION THE AWAIT USED TO MAKE, MADE ON A ROUND. It runs at the top of
   `admit` ahead of the candidate order, because the seat's fetch is ALREADY SPENT: the order chose this work
   item, the request went out, and refusing to seat the document now would waste it and re-rank an address whose
   cost has been paid. That is the same sentence the seed arm's `_seeds.delete` is unconditional for.
   IT RETURNS `null` FOR THE CENSUS, WHICH IS A STATEMENT AND NOT A MISSING READING. This round spent its
   advance on the admission and never ASKED the non-resident order, exactly as the navigation-swap arm above it
   does — so a census of that order would be a reading of a walk that did not run, and `_level1Record` keeps
   that apart from a walk that ran and found nothing. */
async function admitSeatLand(seat) {
  const i = _pool.indexOf(seat);
  DCHECK(i >= 0,
         "an admission seat was landed while no longer in the pool — the pool is the register of who holds " +
         "what and this is the only thing that takes a seat out of it, so a seat that is not there is one a " +
         "Clear removed and whose document is about to be seated into a session the person asked to forget");
  _pool.splice(i, 1);
  /* THE SEAT SETTLED EXACTLY ONE WAY, ASSERTED BECAUSE THE TWO OUTCOMES TAKE OPPOSITE ARMS AND `null` IS A
     LEGITIMATE VALUE OF NEITHER. A seat with both is a load that answered and threw; a seat with neither is
     one this round is landing before its promise settled, which would read `landed === null` as a document of
     no bytes and seat a page this zone never fetched. */
  DCHECK(seat.settled && ((seat.landed === null) !== (seat.failed === null)),
         "an admission seat was landed in a state it cannot be in (settled=" + seat.settled + ", landed=" +
         (seat.landed === null ? "absent" : "present") + ", failed=" +
         (seat.failed === null ? "absent" : "present") + ") — the load stores exactly one of the two and the " +
         "round lands the seat only after `settled`, so anything else is a document about to be built out of " +
         "an answer nobody gave");
  if (seat.failed !== null) throw seat.failed;   // relayed, not swallowed: the round owns this failure (see above)
  if (seat.what.kind === "seed") {
    const seed = seat.what.seed;
    /* THE SEED'S OWN §7.4 NAVIGATION, THROUGH THE ONE CHOKEPOINT — the same call a live document's seed and
       a peer's child navigable both make. The PRIVATE-NETWORK principal and the CREDENTIALED-READ principal
       are the DECLARING document's, taken when the route was declared and carried on the work item, because
       §scheduler's "an operation that becomes a work item takes its inputs with it" is exactly the rule a
       read of the pool here would break: the engine that declared this route may be gone by now. */
    /* AND SO IS THE PROVENANCE, for the identical sentence: the engine that declared this route stated
       what its path made the address, and the load is decided from that word and not from the address. */
    /* THE BYTES, WHICH ARRIVED ON AN EARLIER ROUND AND ARE READ HERE RATHER THAN AWAITED. The residual
       that stood at the await is RETIRED, and it is rewritten rather than deleted because its reasoning is
       what a reader re-derives from `engineIssue`'s table: that table answers the three doors INSIDE a
       service round — the pending-fetch seam, `xhr.send` and `document.fetch` — so an engine no longer
       leaves the rankable set because a server holds a reply open, and this door was the one left, held one
       level ABOVE any round by `hostSchedule`'s own `ops.admit()`. Its cost clause was that a declared
       route whose body never ends froze the LEVEL-1 LOOP rather than one instance.
       ITS NEXT-DIFF CLAUSE WAS RIGHT ABOUT THE SEAT AND WRONG ABOUT THE FOURTH STATE'S READERS, WHICH IS
       WORTH KEEPING BECAUSE IT IS THE CLAUSE A LANE WOULD HAVE BUILT. It said the seat had to be read by
       `_waiting` and by `hostClusterOf`'s "a cluster being provisioned answers exactly as a provisioned one
       does". Neither: a seat holds no `_waiting` entry (a seed has no job and a cold row is not one either),
       and it may not answer `hostClusterOf` at all, because the cluster key is not known until the response
       has landed — `clusterKeyOf` reads the message, and the message's `sourceUrl` is `loaded.url` after a
       redirect. What the state IS read by is the RAM accounting, the wait arm and the probe, each of which
       enumerates its states and would have crashed rather than defaulted; and the reason nothing needs a
       cluster answer is that both kinds mint a group id no other arrival can key to (`seed:` from a
       counter, `cold:` from the frontier key), which the DCHECK below states as an invariant. */
    const loaded = seat.landed;
    /* AND THE SAME THREE REFUSALS A SEEDED DOCUMENT ALWAYS OWES ITS READER, PLUS THE DECLINE, which is a
       FOURTH member of exactly this population and never a new decision — this arm is why `navigationLoad` is
       told `report` for this caller. The three, in the same order and for the same reasons stated at the live
       seed: the chokepoint's own `unavailable`; a response that landed on ANOTHER ORIGIN (which is a Document
       of origin B about to be seated in a cluster keyed on origin A, and is also evidence that this server
       answers this request differently from the one the person's own navigation made); and an EMPTY body,
       which is a perfectly ordinary Document under §7.4.5 and cannot be the bundle this run exists to
       explore. Each costs the one fetch and nothing more. A DECLINE costs LESS — no request went out at all —
       and the pool's answer is the same one, because it is the same state: the seat is already out of `_pool`,
       `_seeds` was spliced unconditionally when the order picked this address, nothing is provisioned, and
       the entry is never LOST by leaving, since the declaring document's residue replays its router and
       declares the route again.
       IT IS TESTED FIRST AND THE ORDER IS LOAD-BEARING RATHER THAN TIDY. A decline carries `unavailable: null`
       AND `bytes: null`, so for a same-origin declared route the first three disjuncts are all FALSE — the
       notice refuses a cross-origin declaration and a refused reply's `urlList` holds the requested href —
       and `loaded.bytes.length` would then read `length` off `null`. An `unavailable` load short-circuits on
       its own first disjunct and never reaches that term, which is the whole reason this was not already a
       live TypeError: the term was unreachable until the decline could arrive.
       AND THE REASON IS PRINTED VERBATIM RATHER THAN CLASSIFIED, because the two shapes that reach here do
       not have one remedy. `blocked-signal:<signal>=<value>` names a row of the person's own egress control —
       it can arrive at all because the notice's `_seedRefusal` pre-screen asks the IDENTICAL vector ROUNDS
       EARLIER and an origin may be narrowed in between — while `blocked-destructive:<token>` is PERMANENT, no
       widening reopening that list by safe-fetch.js's own statement. A message promising a control row for
       both would be wrong about one of them, and `blocked-<rule>:<ground>` is attributable by construction. */
    DCHECK(loaded.declined === null || typeof loaded.declined === "string",
           "a declared route's load does not STATE whether it was declined — `loaded.declined` is `" +
           String(loaded.declined) + "`. Every arm of `navigationLoad` states it, and a `||`-shaped read here " +
           "would make a route the chokepoint REFUSED indistinguishable from one it fetched: the three " +
           "disjuncts beside it are all false for a declined same-origin route, so the seat would fall " +
           "through to `loaded.bytes.length` and die reading `length` off null");
    const _landed = originOf(loaded.url);
    if (loaded.declined !== null || loaded.unavailable !== null || _landed === "" ||
        _landed !== originOf(seed.principalUrl) || loaded.bytes.length === 0) {
      console.warn("[bridge] a declared route could not be seeded: " + seed.url + " — " +
                   (loaded.declined !== null ? "the chokepoint DECLINED it on `" + loaded.declined + "`, so no " +
                      "request was made; the rule that refused it is named in that token"
                    : loaded.unavailable !== null ? JSON.stringify(loaded.unavailable)
                    : _landed !== originOf(seed.principalUrl) ? "landed cross-origin at " + (_landed || "an unparseable URL")
                    : "the response carried no bytes"));
      return null;
    }
    /* A CLUSTER OF ONE, WHICH IS THE TRUTH ABOUT IT RATHER THAN A CONVENIENCE. The declared page is not
       nested in the document that declared it and shares no heap with it: nothing holds a WindowProxy for
       it, no element presents it, and it is reached the way a person reaches a route — by navigating a tab
       of the custom browser's own. So it is a TOP-LEVEL TRAVERSABLE in a browsing-context group this zone
       mints, which is §7.3.2.3's own sentence read for a navigation nobody's opener survives.
       THE PRINCIPAL IS THE DECLARING DOCUMENT'S AND IS NEVER RE-DERIVED FROM THIS ADDRESS. It is the
       browser's `MessageSender.origin` for the document whose router declared the route, carried on the
       work item, and SECURITY.md's credentialed-read principal is exactly that and never `originOf(url)` —
       a sandboxed document has an ordinary address and an OPAQUE origin, so parsing the address would hand
       its declared route same-origin access to authenticated bytes the browser refused it. The two
       ADDRESSES were compared above (which is what HTML §7.2.5's can-have-its-URL-rewritten guarantees and
       what a redirect could still break); this is the other question and it takes the other fact.
       `credentialed` IS THEREFORE COMPUTED FROM THAT SAME PRINCIPAL — the identical predicate
       `navigationLoad` used to decide whether to ask for cookies, so the field STATES the load that
       happened rather than re-deciding it from the address that came back.
       `topLevelUrl` IS ITS OWN ADDRESS because a top-level traversable's environment is its own top
       (§8.1.3.1), and it is `loaded.url` rather than the requested address for §7.5.1's `creationURL`
       reason: after a redirect the Document is AT where the response came from. */
    /* AND THE WORD THE DECLARING ENGINE STATED, CARRIED ON INTO THE RESIDUE THIS RUN WILL PARK. It is
       `seed.provenance` and not a literal for the reason the work item carries it at all: the load above
       was decided from that word, and an entry whose stored grade disagreed with the load that fetched it
       would have the cold tier re-fetch under a permission nobody granted. It is `derived` BY
       CONSTRUCTION only while no origin is widened: at a widened one `_seedRefusal` passes a FORCED
       declaration, and that entry is exactly what this field exists to keep honest. */
    const msg = { type: "AST_ANALYZE", sourceUrl: loaded.url, origin: seed.principalOrigin,
                  groupId: "seed:" + (_nextSeedGroup++), responseHeaders: loaded.headers,
                  topLevelUrl: loaded.url,
                  credentialed: navigationCarriesSession(loaded.url, seed.principalOrigin),
                  /* AND IT IS THE JOIN, FOR THE REASON THE CHILD-NAVIGABLE RECORD STATES: a route only
                     the bundle names, declared from a document this zone chose to open, is reached under
                     both. At every setting reachable today the declaring document is `observed` and the
                     join is the identity, so the stored grade is unchanged — the pairs it separates are
                     the ones a widening creates. */
                  provenance: self.safeFetchReachJoin(seed.reach, seed.provenance),
                  persist: true };
    DCHECK(hostClusterOf(clusterKeyOf(msg)) === null,
           "a declared route minted a browsing-context group this pool already runs an instance for — the " +
           "group id is a fresh counter, so a hit means two seeds were given one id and the second would " +
           "JOIN the heap the first built, which is two documents behind one agent");
    _reserveStats.seeded++;
    /* NULL: a declared route has NO CREATOR — nothing embedded it and nothing opened it, so HTML §7.1.7 has
       no container to clone and this Document is judged against its own response alone. `u`/`null`/`none`:
       §7.3.1.3 gives it no parent and no container element, and §3.1.3's steps 2-3 return the empty list
       for a Document with no container document — three statements of one fact about a top-level page.
       `cold: true` — it has no caller. Nothing awaited this document, so its findings MERGE to the moat
       rather than being returned to a requester, which is the same arm a rehydrated recipe takes.
       AND NOT REFERENCED, WHICH IS THE SAME SENTENCE THE CLUSTER-OF-ONE PARAGRAPH ABOVE ALREADY MAKES:
       nothing holds a WindowProxy for a route the bundle merely DECLARED — no element presents it and no
       page opened it, which is precisely why this zone had to mint its group and its name. Its frontier
       is entitled to drain, and draining is what produces the findings this arm exists to collect. */
    await engineCreate("", loaded.bytes, msg, true, null, loaded.url, true, null, "u", "null",
                       "none", "none", 0)._readyP;
    return null;
  }
  DCHECK(seat.what.kind === "cold",
         "an admission seat reached the landing with a kind neither arm above builds — `admitLoadSeat` " +
         "refuses any word but the two, so a third here is a kind added at the seat and not at the landing, " +
         "and the document would be dropped with its fetch spent and nothing said");
  {
    /* THE WHOLE PARKED DOCUMENT COMES BACK THROUGH ONE READER, and the recipe's flows resume inside it.
       `frontierDoc` is the same shape the park wrote, asserted the same way in the other direction, so
       the field the tier was missing (`responseHeaders` — the policy container) cannot go missing again
       from one end only.
       THE CONTENT IS READ FOR THE PICKED ITEM AND NOT AT THE RANKING, which is the other half of why the cold
       tier has a ranking VIEW. The order reads two numbers; only the item it PICKS is deserialized — and since
       the read is a suspension it happens on the SEAT, one round earlier, so this line takes what landed. */
    const stored = seat.landed.stored;
    DCHECK(stored,
           "the cold tier's ranking view named an entry the store does not hold — this zone is the store's " +
           "only writer and frontierPut updates the view on its way through, so a row with no entry is the " +
           "projection and the store having drifted apart");
    /* A SHED RESIDUE'S DOCUMENT IS ON THE NETWORK RATHER THAN IN THE STORE, so it was fetched back — the
       same request the shed decision was PROVED against, performed for real. THE ARGUMENT FOR AWAITING IT
       INSIDE THE ROUND IS RETIRED AND IS KEPT BECAUSE IT IS THE ONE A READER RE-DERIVES: it said this arm
       "already suspends on the store and then on a whole wasm instantiation, so a fetch ahead of them is the
       same shape and not a new one". Every clause of that is true about the SHAPE and false about the COST.
       A store read and a wasm instantiation are LOCAL and settle; a response body's arrival is a remote
       party's to decide, and this arm sits in `ops.admit()`, one level above every service round — so a shed
       entry whose server holds the body open froze the LEVEL-1 LOOP and not this admission. "The same shape"
       is exactly the reasoning that makes an unbounded wait look like a bounded one.
       A re-derivation that fails strands the entry and this round seats nothing — it is not offered again
       (see `_bestCandidate`), so the failure costs one fetch in total rather than one per round. */
    let doc;
    if (stored.shed) {
      const back = seat.landed.back;
      if (!back) return null;
      doc = frontierDoc({ key: stored.key, sourceUrl: stored.sourceUrl, topLevelUrl: stored.topLevelUrl,
                          origin: stored.origin, responseHeaders: back.headers, html: back.bytes, code: "",
                          recipes: stored.recipes, emit: stored.emit, visits: stored.visits,
                          credentialed: stored.credentialed, provenance: frontierProvenance(stored) },
                        "was re-derived for the cold tier");
    } else {
      doc = frontierDoc(stored, "came back from the cold tier");
    }
    /* THE PRINCIPAL RESUMES WITH THE RECIPE. A parked flow's world is only the same world if the
       document it resumes into is the same PRINCIPAL — and this zone cannot re-derive one from
       c.sourceUrl without re-fabricating the tuple origin a sandboxed document does not have. The stamp
       site refuses to deliver a message for an empty one rather than inventing one. */
    /* A RESUMED RECIPE IS A CLUSTER OF ONE, and its GROUP says so rather than borrowing a tab's. The
       browsing-context group it parked in is gone — the tab was closed, or the session was — so there is
       no live document it may share a heap with, and the frontier key (address|bundle, already unique per
       recipe and never equal to a tab id) is the honest name for the group it resumes into. That is also
       what lets the origin half stay "" for a recipe whose principal is empty: an empty origin on a key
       whose group is unique collides with nothing, so nothing is invented. */
    const msg = { type: "AST_ANALYZE", pageHtml: doc.html, code: doc.code, sourceUrl: doc.sourceUrl,
                  origin: doc.origin, groupId: "cold:" + seat.what.key,
                  responseHeaders: doc.responseHeaders,
                  topLevelUrl: doc.topLevelUrl, credentialed: stored.credentialed,
                  /* THE WORD THIS RESIDUE WAS PARKED UNDER, RE-STATED SO THE ENTRY IT PARKS AGAIN KEEPS IT.
                     A rehydration does not re-decide how the address was first reached; it replays it. */
                  provenance: frontierProvenance(stored), persist: true };
    /* THE `try {} catch` AROUND THIS IS GONE WITH THE REPORTING IT DID. A rehydration whose engine ABORTS
       is now bannered by engineBootFailed, at the reservation, together with the pool slot it releases —
       one place on every creation path rather than one arm per call site. What was left in the catch was
       an invariant abort, which `RETHROW_FATAL` was already rethrowing, so the arm could only ever have
       caught something it immediately gave back. A rehydrated cold recipe always participates in the
       frontier and never has a caller, which is the `cold` argument. */
    /* THE STORED DOCUMENT IS PASSED AS IT WAS PARKED. `c.html || ""` stood here and it defeated
       engineRoot's own assert: an entry carrying no document became a page that parses to nothing, which
       reads as an origin whose parked flows found nothing rather than one that was never rebuilt. */
    _reserveStats.rehydrated++;
    /* NULL: a rehydrated cold recipe replays a document that had no creator in this session either. */
    /* `u`: a rehydrated recipe carries the DOCUMENT its session recorded and no embedder — the frontier
       key is a document's, and a parked child navigable resumes through the create notice its creator's
       replay re-emits rather than through this path.
       `none`: and §3.1.3's list arrives on that same re-emitted notice for the same reason, so what is
       replayed HERE is a document with no embedder and therefore no ancestors. */
    /* AND NOT REFERENCED, ON THAT SAME SENTENCE. A resumed recipe is a CLUSTER OF ONE in a group this zone
       mints from the frontier key: the browsing-context group it parked in is gone, so there is no live
       instance anywhere holding a WindowProxy for the document being rebuilt. If this document creates a
       child navigable again, that child arrives as a create notice and is provisioned referenced by the arm
       above — which is the same route the parked child took the first time.
       THIS IS THE ONE ARM WITH NO NATIVE COUNTERPART, and it is worth saying why it does not contradict
       `qjs_set_referenced` surviving a teardown: what survives a teardown is one INSTANCE's statement about
       itself across a park, and what happens here is a NEW instance for a document whose peers no longer
       exist. Carrying the old session's `1` forward would hold a frontier open for a proxy nothing holds. */
    await engineCreate(doc.code, doc.html, msg, true, null, null, true, null, "u", "null",
                       "none", "none", 0)._readyP;
    return null;
  }
}
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
    await eng.r.renderer.setYieldFloor({ floor });
  },
  /* WHICH RESIDENT ENGINE MUST GIVE UP ITS RAM, ASKED OF THE ONE ORDER — the third moment of the SAME question
     `admit` asks and `frontierWeight` answers, and the reason those are no longer three policies. Admission
     asks it when there is room, this asks it when there is not, and rehydration is not a separate question at
     all (a parked frontier is simply a candidate that is not resident).
     THE FLOOR IS THE ONLY GATE, AND IT IS THE FLOOR RATHER THAN `_admissionHasHeadroom`. Those two are
     different facts and were one before: a RESERVATION in flight also blocks admission, and a boot that has
     not reported yet is not a reason to evict a running document — it is a reason to wait for the number.
     STRICTLY GREATER, so a tie leaves the incumbent holding the RAM. That is the identical comparison
     solver/flow.c's flow_pick makes one level down ("the thread moves only for a flow that is strictly
     better"), and it is what stops two items of equal weight from paying a park and a rehydration to swap
     places. It is not a bound: the loser keeps its place, its weight and every flow it holds.
     WHAT IT CAN STILL DO IS CHURN, NAMED HERE BECAUSE IT IS MEASURED RATHER THAN SUSPECTED (`evicted` and
     `rehydrated` beside each other in _reserveStats). The two sides of this comparison are not denominated
     the same way through time: a resident engine's weight AGES by CPU without bound (solver/flow.h: "a
     document's best flow falls by one point per second of unproductive CPU while a document that boots today
     enters at 1.0… past it a mature document is outranked by every page that arrives afterwards"), while a
     parked item's `emit/visits` is a RATE that does not fall — so a productive heavy document can sink below
     the floor's rivals, park, and win its own slot straight back on the next round. Every such cycle does
     real work (the replay re-explores) and admission still interleaves the waiting tabs around it, so it is a
     COST and not a starvation — and the primitive that removes it is the one flow.h already names: start-time
     fair queueing's virtual time, applied at LEVEL-1 so an item RE-ENTERING the resident set enters at the
     resident set's virtual time instead of resetting its own clock by leaving. Not invented here, because
     every substitute for it is a decay constant somebody picked. */
  /* IT ANSWERS TWO THINGS AND THE SECOND IS NOT A DECISION. `evict` is the engine that must give its memory up,
     or null; `cand` is the READING of the non-resident order this arm took to decide that, or null where it
     did not take one. They travel together because this arm is where the Level-1 comparison the whole design
     is about actually happens — `pick.best.w > engineWeight(worst)`, a non-resident item against the RAM a
     resident one holds — so a census that only saw admission's ask would be blind exactly at the floor. It is
     NOT the round's only ask: `admit` asks under the floor and this asks at it, and an admission that seats an
     instance can be what puts the pool at the floor in the same round, so hostSchedule counts the asks rather
     than assuming there was one. */
  evictee: async (hot) => {
    if (!_atRamFloor()) return { evict: null, cand: null };   // there is room: nothing has to give anything up
    const pick = _bestCandidate(_candPopulation(await frontierIndex()));
    if (!pick.best) return { evict: null, cand: pick.census };   // nothing is waiting for the memory
    let worst = null;
    for (const e of hot) if (!worst || engineWeight(e) < engineWeight(worst)) worst = e;
    // every live engine is mid-round; the floor is re-asked next round
    if (!worst) return { evict: null, cand: pick.census };
    if (!(pick.best.w > engineWeight(worst))) return { evict: null, cand: pick.census };
    _reserveStats.evicted++;
    return { evict: worst, cand: pick.census };
  },
  /* THE COLD-TIER PARK. The catch that stood here swallowed the engine's own abort, so the host went on
     believing it had evicted an engine that never parked. It is the forcing function; it is not caught.
     THE CAPABILITY IT USED TO NAME AS UNBUILT IS BUILT: this comment said "qjs_request_park is a bare DFAIL
     naming the serializer to write", and main.c's qjs_request_park DCHECKs `g_begun` and calls
     engine_request_park, which raises `g_park_req`; engine_sched_slice takes it at the next step boundary,
     switches the running flow out, writes every member through cold_park and answers ENGINE_STEP_DONE. A
     comment that names another file's absence is a claim about that file, and this one outlived it. */
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
    /* A CLEAR LANDED WHILE THIS SNAPSHOT WAS IN THE AIR. This is the ONE await in this function, so a dropped
       engine here is exactly that, and the snapshot below is an observation of a page the user has just asked
       to have deleted — merging it repopulates the store the Clear emptied, into the cumulative moat AND (since
       the merge learned to name its documents) into the document itself. It also cannot be merged even if one
       wanted to: hostClear rejects a live instance's waiters, and `engineLiveDocumentIds` — evaluated as the
       argument on the very next line — asserts that a live instance still has some, so the merge path for a
       cleared engine ABORTS the trusted zone rather than reaching the store at all.
       THE FRAME IS NOT TAKEN HERE. This round has one owner and it is the release step at the bottom of
       hostSchedule; destroying here would be a second, and the second teardown removes an element that is
       already gone — which renderer-host reports as a renderer having left the document twice. */
    if (eng._dropped) return;
    let idx = -1;
    for (let i = eng.lines.length - 1; i >= 0; i--) if (String(eng.lines[i]).startsWith("@RESULT ")) { idx = i; break; }
    /* qjs_emit_partial PRINTS ONE, unconditionally. No line here means the print sink between the engine and
       this zone dropped it, and returning quietly would report the engine as having found nothing. */
    DCHECK(idx >= 0, "qjs_emit_partial produced no @RESULT line — it prints the one result document every time " +
                     "it is called, so its absence is this zone's own capture of the engine's output failing");
    if (idx < 0) return;   // release
    const partial = linesToAnalysis([eng.lines[idx]], eng.msg, "partial", eng);   // parse only the snapshot line
    eng.lines.splice(idx, 1);                                           // consume it
    // Merge on EITHER surface: an XSS-only page (verified @S PoCs, no endpoints) must surface incrementally
    // too — gating the merge on fetchCallSites alone dropped every sink from a live/looping engine.
    /* `securitySinks` IS ASSERTED ON EVERY DOCUMENT THIS SEAM PRODUCES (assertResultDocument), so the
       `partial.securitySinks &&` that stood beside this one was a defaulted read of a guaranteed field — the
       shape that turns a producer that stopped writing it into a page with no sinks. A snapshot is BUILT from
       the one @RESULT line above, so it always has a document; asserted rather than assumed, because "always"
       here is a claim about the arm `linesToAnalysis` took and the two lengths below are read regardless. */
    DCHECK(analysisHasDocument(partial),
           "an incremental snapshot carried no engine document — it is parsed from the @RESULT line this " +
           "function just found, so its absence is that parse having produced something else entirely and " +
           "every finding in the snapshot is about to be read off a record that has none");
    /* AND THE THIRD SURFACE, WHICH IS THE ONLY ONE A PAGE THAT LEARNED NOTHING HAS. The two lengths above are
       what the run FOUND; the engine's page errors are what it could NOT do — HTML §8.1.4.4 "Calling scripts"
       step 8 REPORTS an uncaught exception rather than propagating it, so each one ENDS a program and names a
       capability the page reached for. A snapshot gated on the finding arrays alone is therefore discarded
       for exactly the document whose only output is that diagnosis, and the terminal record which would
       otherwise carry it is the one a run that never drains its frontier never reaches. It is the same
       repair the comment above records for `securitySinks`, owed at the sibling and not made.
       ASSERTED, NOT DEFAULTED: `linesToAnalysis` builds this array on BOTH its arms, so an absent one is that
       producer changed under this reader and a `||`-past would read it as a page with nothing to say. */
    DCHECK(Array.isArray(partial.resolverErrors),
           "an incremental snapshot carried no resolverErrors array — linesToAnalysis builds one on every " +
           "arm it has, so its absence is that composition changed under this reader and every uncaught " +
           "throw the engine recorded for this page would be discarded with the snapshot that carries it");
    const hasWork = partial.fetchCallSites.length || partial.securitySinks.length ||
                    partial.resolverErrors.length;
    if (!hasWork) return;
    /* THE MERGE CALLBACK IS THE OTHER HALF OF THIS EDGE. `typeof … === "function"` guarding the call meant a
       zone that had not installed it dropped every incremental finding silently; offscreen-brain.js installs
       it before this file is even loaded, so its absence is a broken load order, not an optional feature. */
    DCHECK(typeof self.onFrontierAdvance === "function",
           "the trusted zone has no onFrontierAdvance to merge an engine's findings into — every incremental " +
           "finding this engine emits has nowhere to go");
    self.onFrontierAdvance(eng.msg.sourceUrl, engineLiveDocumentIds(eng), partial, eng._epoch);
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
  serviceFetch: async (eng, stalled) => {
    try { await engineServiceFetch(eng, stalled); await engineRecordFacts(eng); }
    finally { if (eng._dropped) eng.r.destroy(); }
  },
  /* THE ROUND'S HALF OF THE CLEAR, and the only thing it does is give back the frame the Clear could not take.
     There is NOTHING else to do here on purpose: no result is asked for (the instance is going away and its
     document would be a page nobody requested), no residue is persisted (the cross-session frontier was
     emptied by the same Clear), no waiter is answered (hostClear already rejected them all with "cleared",
     which is the error _dispatchDocument reads to abandon its tail). A `finish` here would have been all four. */
  release: async (eng) => {
    DCHECK(eng._dropped,
           "the scheduler was handed an engine that left the pool without a Clear having dropped it — the pool " +
           "is spliced by this loop's own `finish` and by hostClear and by nothing else, so a third remover is " +
           "an instance being reclaimed by two owners and its frame freed twice");
    DCHECK(eng.r.outstandingCalls() === 0,
           "a cleared engine's round landed with " + eng.r.outstandingCalls() + " call(s) still outstanding — " +
           "this is the round that owns the frame, so a call still in flight here is a second op driving an " +
           "instance the user wiped, and the teardown below would abort inside renderer-host instead");
    eng.r.destroy();
  },
  /* ADMISSION IS THE ONE ORDER ASKED WHERE THERE IS ROOM — the same question `evictee` asks where there is
     not. It runs in two parts and they are DIFFERENT KINDS OF QUESTION, which is why the ranked part no
     longer contains the routing part:
       (1) ROUTING, WHICH COSTS NO RAM AND IS THEREFORE NOT RANKED AND NOT GATED. A waiting document whose
           agent cluster already has an instance JOINS it, and a sub-frame waits for its embedder to name it.
           Both used to sit INSIDE the RAM-gated walk, so a same-origin iframe of a page that was already
           running could not attach to its own agent while the pool was at the floor — a join blocked by a
           budget it does not spend.
       (2) ADMISSION, over the ONE candidate order (_bestCandidate): a waiting document and a parked frontier
           compete by value, and the only thing that holds an item back is the RAM floor — never a liveness
           gate saying a cold item may not compete. That gate (`!_waiting.length && !_pool.some(e => !e._cold)`)
           is deleted: in continuous browsing there is always a live document, so a flow parked last week
           could never be reached regardless of its value. */
  admit: async () => {
    /* THE ONE SUSPENSION IN THIS FUNCTION, TAKEN FIRST AND DELIBERATELY. Everything below it — the routing
       walk, the pick, and the synchronous half of engineCreate that takes the pool slot — then runs in one
       uninterrupted turn, which is what makes "the pool is the register of who holds what" true at the moment
       the register is consulted. */
    const _coldRanking = await frontierIndex();
    /* AND A DOCUMENT THAT HAS ALREADY ARRIVED IS SEATED BEFORE ANYTHING ELSE IS CONSIDERED, because its fetch
       is spent. A `loading` seat whose load has settled is a work item the order already chose and already paid
       for, so ranking anything against it would be ranking against a cost that cannot be unspent; and taking it
       FIRST is also what makes the wait arm unable to spin — a settled seat's `_readyP` resolves immediately, so
       a round that left it in the pool would re-enter that arm at full speed on a condition only this line
       changes. ONE PER ROUND like every other advance: `admitSeatLand` returns, so the pick below is not reached
       and the round goes on to rank and step the hot set it already has. */
    const _landing = _pool.find((e) => e.state === "loading" && e.settled);
    if (_landing) return admitSeatLand(_landing);
    /* AN INDEX WALK RATHER THAN A SHIFT, because one waiting document can be legitimately UNSEATABLE YET (the
       defer below) and a shift would either drop it or spin this loop forever re-reading it. */
    let i = 0, swap = null;
    while (i < _waiting.length) {
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
           steps (navigable.c mints its name, adopts it into this agent, and HTML §7.4.5 "Populating a
           session history entry" loads its address
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
        /* THE GROUP'S TOP WAS REPLACED — WHICH IS WHAT BROWSING AN SPA IS, AND USED TO BE THE ABORT THAT KILLED
           THE SCHEDULER FOR THE REST OF THE SESSION. A same-origin link click in one tab reaches a cluster that
           already has an instance rooted at the document being replaced, and there was nothing to do about it
           but crash: joining alone would have left the old document's realm, tree and parked flows running and
           REPORTING inside the same instance — two tops for one traversable, a worse answer than the abort.
           BOTH HALVES EXIST NOW AND THEY ARE ONE NAVIGATION IN THE STANDARD'S OWN ORDER. HTML §7.4.6.1
           "Updating the traversable" unloads `displayedDocument` given `targetEntry's document`, so the
           incoming Document is created FIRST and the outgoing one is deactivated after it — engineJoin then
           engineUnload, and never the other way round, which would join a top into an agent whose top had
           already been destroyed. The unload is §7.4.6.1's deactivate-a-document-for-a-cross-document-
           navigation and NOT §7.3.1.6 "Navigable destruction": a navigation destroys no navigable, it replaces
           the Document active in one. (§7.3.1.6 is what this abort's own text used to name, after naming §7.4
           before that, and the section it should have named defines an operation over a different object.)
           A COOP NAVIGATION THAT SEVERED THE GROUP IS NOT THIS CASE and does not reach here: §7.3.2.3's swap
           puts the new Document in a NEW browsing-context group, and the group is half of `clusterKeyOf`, so
           such a document arrives with a cluster key no instance holds and roots one of its own. */
        if (isTop && cluster.topDocId !== docId) {
          /* ONE PER ROUND, WHICH IS THE ADMISSION'S SHAPE AND FOR ITS REASON. A join copies a whole document
             into an instance's linear memory and an unload seeds a task on every one of its timelines, so this
             is an ADVANCE and hostSchedule is rank-advance-re-rank. The rest stay in `_waiting` and are looked
             at again next round, which is also what keeps two navigations of one tab in ORDER — the second
             cannot be taken until the first has moved `topDocId`. */
          if (swap) { i++; continue; }
          /* RECORDED AND SPLICED SYNCHRONOUSLY, BEFORE ANYTHING SUSPENDS — the whole reason this function takes
             its one suspension first. `topDocId` is what the next arrival for this cluster consults, so a
             second navigation landing between this decision and the join below would otherwise read the OLD
             top and record a second swap out of the same, already-replaced document. */
          swap = { cluster, job, outgoing: cluster.topDocId, docId };
          cluster.topDocId = docId;
          cluster._resolvers.push(job);
          _waiting.splice(i, 1);
          _reserveStats.navigated++;
          continue;
        }
        /* AND THE ROUTING INVARIANT THE ABORT WAS PROTECTING IS STILL ASSERTED, over what is left of it: every
           other arrival for a cluster that has an instance is a document that instance is ALREADY running — a
           same-origin sub-frame the engine created as a realm of its own, or one document delivered twice. A
           top that is not the cluster's current top has been taken by the branch above, so reaching here with
           one is the swap having failed to record it, which would attach this caller to an instance reporting
           the page the browser replaced. */
        DCHECK(!isTop || cluster.topDocId === docId,
               "the TOP document of a browsing-context group arrived for a cluster whose instance holds a " +
               "different top (" + cluster.topDocId + " vs " + docId + ") and the navigation branch above did " +
               "not take it — that branch is the ONLY thing that may change `topDocId`, so this is either a " +
               "second writer for that field or an `isTop` computed two different ways in one function, and " +
               "attaching this caller here reports the page that was replaced");
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
      i++;
    }
    /* (1b) THE NAVIGATION, PERFORMED HERE AND NOT IN THE WALK, because it SUSPENDS and the walk may not. The
       walk's whole property is that it runs in one uninterrupted turn after this function's single await, so
       that "the pool is the register of who holds what" is true at the moment the register is consulted; an
       `await` inside it would let a second arrival for the same cluster run against a half-updated register.
       So the walk DECIDES (synchronously, moving `topDocId` and attaching the caller) and this performs, and
       it RETURNS afterwards for the same reason the admission arm does: this round's advance is spent.
       IT IS NOT RAM-GATED, like every other routing decision in this function. A navigation does not create an
       instance — it puts a second document in one that already exists, and then destroys the one it replaced,
       which is the direction that RELEASES memory. Gating it would be a cap that made continuous browsing work
       only while the pool was under the floor, and would leave the replaced document running in the meantime.
       NULL: the incoming Document of a cross-document navigation has NO CREATOR — HTML §7.1.7 "Policy
       containers" clones a creator's container only where there is one, and a top-level traversable's new
       Document is created from its OWN response — so the empty pair is the positive statement, exactly as it
       is for a root document a content script reported. */
    if (swap) {
      /* A CLUSTER MAY STILL BE BOOTING WHEN THE TAB NAVIGATES — a fast second click is not exotic — AND THE
         NAVIGATION WAITS FOR IT, which is the same suspension the create-notice join takes and for the same
         reason: `qjs_join` needs both `qjs_init` and `qjs_begin` to have run, and a document joined between
         them is parsed, given a realm and never executes a line. `_readyP` settling is what says both have. */
      if (swap.cluster.state === "booting") await swap.cluster._readyP;
      DCHECK(swap.cluster.state === "hot" || swap.cluster.state === "fetching",
             "a tab navigated in a cluster whose instance never became one — the reservation holding it failed " +
             "to boot, so there is no agent for the incoming document to join and nothing to unload the " +
             "outgoing one out of; the caller was attached to that reservation and has already been answered " +
             "with the crash record by engineBootFailed, which is why this is a state to assert and not one " +
             "to report");
      /* RELEASE PATH UNDER THE ASSERT: the record is out of the pool and its callers are answered, so there is
         no instance to speak to and nothing here can honestly reach one. */
      if (swap.cluster.state !== "hot" && swap.cluster.state !== "fetching") return;
      const _tlu = swap.job.msg.topLevelUrl || swap.job.msg.sourceUrl;
      DCHECK(typeof _tlu === "string" && _tlu !== "",
             "a navigated top-level document arrived with no address to be its own TOP-LEVEL CREATION URL — " +
             "HTML §8.1.3.5 reads it to decide whether this realm is a secure context, so Web IDL §3.3.13's " +
             "members would exist or not by a guess; a top-level traversable's is its own document address, " +
             "which is the one thing every arrival at this zone carries");
      /* NO CREATOR, SO NO CONTAINER TO CLONE — §7.3.2.3 creates the swapped-to browsing context "with null,
         null, and group", and this Document's container comes from its OWN response. The empty CSP pair says
         that; §7.1.4's item has no empty spelling and so states the section's own new embedder policy. */
      /* AND §7.3.1.3's PARENT IS `u` FOR THE SAME SENTENCE: what joins here is a TOP-LEVEL TRAVERSABLE's
         incoming Document (§7.4.6.1 "Updating the traversable"), which is nested in nothing — and §3.1.3's
         list is `none` because a Document nested in nothing has no container document for its step 2 to find. */
      /* THE BYTES ARE ON THE JOB, NOT ON ITS MESSAGE — `_waiting.push({ html: loaded.bytes, msg, … })` is the
         only writer of this table, and the AST_ANALYZE entry that fills it asserts the message carries no
         document at all. This is the same `job.html` the admission arm below hands engineCreate; reading
         `swap.job.msg.pageHtml` instead is what made this branch abort on every navigation it was built for. */
      await engineJoin(swap.cluster, swap.job.html, swap.job.msg, swap.docId, _tlu,
                       { csp: "", selfOrigin: "", embedder: NEW_EMBEDDER_POLICY }, "u", "null", "none",
                       "none");
      await engineUnload(swap.cluster, swap.outgoing, swap.docId);
      /* NULL, AND IT IS A STATEMENT RATHER THAN A MISSING NUMBER: this round spent its one advance on the
         navigation and NEVER ASKED the non-resident order, so a census of it would be a reading of a walk that
         did not run. `_level1Record` keeps that apart from a walk that ran and found nothing. */
      return null;
    }
    /* (2) THE ONE ADMISSION, over the ONE order. Nothing distinguishes a waiting tab from a parked frontier
       here except its weight, which is the whole of what "Cold-tail resume is the SAME admission step" means.
       ONE PER ROUND, WHICH IS THE LEVEL-1 LOOP'S OWN SHAPE AND NOT A BUDGET. hostSchedule is "rank, advance the
       winner, re-rank" and an admission is an advance: seating the whole backlog inside one call would rank 142
       documents against a working set measured before any of them existed, and would hold the pool through 142
       sequential boots while the engines already resident ran not one step. Re-asking per round also re-reads
       the floor after a step has moved it. AND IT IS WHAT KEEPS A FAILING REHYDRATION FROM SPINNING: a cold
       entry whose document aborts on boot leaves the pool through engineBootFailed and is a candidate again, so
       a loop that kept picking inside one call would never return; picked once per round it banners once per
       round, loudly, with every other engine still being stepped in between. */
    /* THE PICK AND THE READING OF THE ORDER IT WAS TAKEN OVER. `pick` is null where the order was NOT ASKED
       (no headroom — a reservation in flight, or the floor, which `evictee` then asks it at instead), and
       `pick.best` is null where it was asked and had no members. Those are two different facts and the census
       carries them as two, which is why this is not one `cand` variable any more. */
    const pick = _admissionHasHeadroom() ? _bestCandidate(_candPopulation(_coldRanking)) : null;
    const cand = pick ? pick.best : null;
    if (cand) {
      DCHECK(CAND_KINDS.some((k) => k.kind === cand.kind),
             "the admission order produced a candidate of a kind this zone has no way to build (`" +
             cand.kind + "`) — every work item that is not resident is one of the kinds CAND_KINDS declares " +
             "(a document waiting for an instance, an address an application declared is a page of itself, a " +
             "parked frontier), and one outside that table would be silently skipped by whichever of the " +
             "arms below happened to be the fallback");
      /* AN ADDRESS THE APPLICATION DECLARED IS A PAGE OF ITSELF — HTML §7.4.4's URL and history update steps
         ran in some engine and it announced the route (solver/route_seed.h). This is where its ONE fetch is
         spent, and spending it HERE rather than at the notice is the whole reason the register exists: the
         order decided this address was worth a request ahead of every other work item, which is what
         §A-SELF-SEEDED-DOCUMENT means by an outbound request being a cost the WFQ carries.
         IT LEAVES THE REGISTER FIRST AND UNCONDITIONALLY. A load that fails has still spent the fetch, so
         re-offering the entry would spend one per round on an address that has already said no; and the entry
         is never LOST by leaving, because the declaring document's residue replays its router and declares the
         route again — which is the re-derivable tier's own exchange, storage traded for recomputation. */
      if (cand.kind === "seed") {
        const seed = _seeds.get(cand.addr);
        DCHECK(seed !== undefined,
               "the admission order picked a declared route that is no longer declared — `_seeds` is written " +
               "by the notice router and spliced only here, so a pick with no entry is the order and the " +
               "register having parted, and the address about to be loaded would be one nothing named");
        _seeds.delete(cand.addr);
        if (!seed) return pick.census;
        /* AND THE FETCH IS ISSUED RATHER THAN AWAITED, WHICH IS THE WHOLE OF WHY THIS ARM IS THREE LINES. The
           load used to be awaited HERE, inside `ops.admit()`, one level above any service round — so a declared
           route whose server holds its body open did not freeze one instance, it froze the LEVEL-1 LOOP: no
           engine ranked, no engine stepped, no engine serviced again for the rest of the session, including
           every instance that was perfectly healthy, and no round began so nothing recorded that either. The
           seat is a member of the pool, `_admissionHasHeadroom` counts it, and `hostSchedule`'s wait arm waits
           on its promise only when NOTHING is rankable — so the round returns and the hot set keeps running.
           THE ADMISSION FINISHES ON A LATER ROUND, AND EVERY DECISION STAYS ON A ROUND. What the seat carries
           off-round is exactly the network body; the refusals, the message, the cluster check and the boot are
           all performed by `admitSeatLand` at the top of a round, so an invariant abort out of any of them
           reaches `hostSchedule`'s own failure arm exactly as it did when the load was awaited here. */
        admitLoadSeat({ kind: "seed", seed: seed },
                      () => navigationLoad(seed.url, seed.principalUrl, seed.principalUrl,
                                           seed.principalOrigin, seed.provenance, seed.reach,
                                           /* `report` — A ROUTE SEED HOLDS NO RENDEZVOUS AND HAS AN ARM,
                                              which is the pair that made the boolean this replaces answer
                                              two questions. `admitSeatLand` below drops this seat and prints
                                              the chokepoint's own reason, exactly as it already did for a
                                              load that was unavailable, landed cross-origin or carried no
                                              bytes — nothing is provisioned, so there is nothing to
                                              fabricate. The decline this admits is REACHED: the notice's
                                              `_seedRefusal` pre-screen asks `safeFetchFiringRefusal` and the
                                              DESTRUCTIVE DENY LIST is not that function, so an application
                                              that routes to `/logout`, `/settings/reset` or
                                              `/account/delete` declares a page of itself that the
                                              pre-screen passes and the chokepoint then refuses
                                              `blocked-destructive:<token>`. */
                                           /*refusalArm*/"report",
                                           /* `page` — HTML \u00a77.4.4's URL and history update
                                              steps RAN IN THE ANALYSED DOCUMENT and it announced the route
                                              (solver/route_seed.h), so the address is one the page's own
                                              code composed. This is the one of the two seed kinds the word
                                              is `page` for, which is why the two are no longer answered by
                                              one literal. */
                                           /*actor*/"page"));
        return pick.census;
      }
      if (cand.kind === "doc") {
        const job = cand.job;
        const key = clusterKeyOf(job.msg);
        const j = _waiting.indexOf(job);
        DCHECK(j >= 0,
               "the admission order picked a waiting document that is no longer waiting — `_waiting` is only " +
               "spliced by this function and by hostClear, and a job admitted twice is one document answered " +
               "by two instances and one caller resolved by whichever finishes first");
        _waiting.splice(j, 1);
        /* THE JOB IS ATTACHED TO THE RESERVATION BEFORE ANYTHING SUSPENDS, which is what makes the boot's failure
           path the ONE owner of every caller on this document. It used to be pushed after the await, so a
           creation that aborted answered this job here (`crashResult`) while any caller that had joined in the
           meantime was answered by nobody — two settlement owners for one record, split by timing.
           boot/creation abort: LOUD failure, not a quiet degenerate result, and that is engineBootFailed's now:
           it banners once, answers every attached caller with the crash record, destroys the frame and takes the
           reservation back out of the pool. An invariant abort from the creation path (the init return code, the
           bundle id, the frontier key's address) is NOT a boot abort — it is this zone's contract with the engine
           breaking — so it travels on through `_readyP` to hostSchedule's own failure arm. */
        /* NULL: a document a content script reported is a ROOT one — no creator, so no §7.1.7 clone. */
        /* `u`: a document a content script reported is the TAB's, which is a top-level traversable — the one
           navigable no create notice named, and the reason this zone can state its §7.3.1.3 parent at all.
           `none`: a top-level traversable's Document has no container document, so §3.1.3's steps 2-3 return
           the empty list — the same fact its `u` parent states one link along. */
        /* NOT REFERENCED. What is admitted here is a TOP-LEVEL document of a browsing-context group — the
           walk above leaves a cross-origin sub-frame WAITING for its embedder to name it, precisely so that a
           document a peer created is provisioned through the create notice and never through this arm — so no
           instance in this pool holds a WindowProxy for it. It is a person's own tab, and its frontier is
           entitled to drain and answer its caller.
           A CROSS-ORIGIN CHILD MAY STILL BE CREATED *INTO* THIS INSTANCE LATER, and that is not this line
           being wrong — it is the join arm's question, asked and refused there, because the flag can only be
           stated before the frontier is seeded and this one is about to be. */
        const eng = engineCreate("", job.html, job.msg, job.persist, null, null, false, null, "u",
                                 "null", "none", "none", 0);
        DCHECK(hostClusterOf(key) === eng,
               "engineCreate did not leave its reservation in the pool before returning — the whole point of " +
               "the slot being taken synchronously is that the next arrival for this cluster finds it, so a " +
               "pool that does not hold it is the race this reservation exists to close, still open");
        eng._resolvers.push(job);
        await eng._readyP;
        return pick.census;
      }
      /* AND THE STORE READ AND THE RE-DERIVATION ARE ISSUED RATHER THAN AWAITED, FOR THE REASON THE SEED ARM
         STATES AND FOR ONE MORE OF ITS OWN. A shed residue's document is on the NETWORK — the same request the
         shed decision was proved against — so awaiting it here put a remote body one level above every service
         round exactly as the seed's load did; and the `frontierGet` beside it rides the same seat rather than
         being split from it, because the two are one question (`shed` is a field of the entry, so which of the
         two reads is needed is not known until the first has answered) and splitting them would be two seats
         for one admission. Both are relayed to `admitSeatLand`, which performs every DECISION on a round. */
      admitLoadSeat({ kind: "cold", key: cand.row.key }, async () => {
        const stored = await frontierGet(cand.row.key);
        return { stored: stored, back: stored && stored.shed ? await frontierRederive(stored) : null };
      });
    }
    return pick ? pick.census : null;
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
    /* A CRASHED RUN DOES NOT WRITE THE CROSS-SESSION FRONTIER, AND THIS IS THE READER `_engineCrashed` NEVER
       HAD. The park recipes are a claim that these flows can be resumed — out of the heap that just aborted —
       and the counters beside them (`emit`, `visits`) are a claim about a run that finished. Worse in the
       direction that has no symptom: a crashed run has NO `_park` at all (and had an empty one before the
       document became present-or-absent), `frontierPut` DELETES an entry whose recipe string is empty, so
       persisting a crash would erase the residue a PREVIOUS session parked for this origin — findings and
       flows nobody in this session ever measured, dropped by a run that died. Skipping the write leaves that
       entry exactly as it was, which is the honest state: this run has nothing to say about what is
       resumable. */
    if (eng.persist && result._fkey && result._run === "complete") {   // persist into the GLOBAL frontier (cross-session cold tier)
      /* AND THE GATE'S OWN PREMISE IS ASSERTED, because it is a premise about ANOTHER function's arms. `_run`
         is `complete` exactly where `linesToAnalysis` was given a document, so the two reads below are safe —
         and if that ever stops being true the failure is silent and maximally expensive: `recipes` would be
         the empty string, which frontierPut reads as "delete this origin's residue". */
      DCHECK(analysisHasDocument(result),
             "a run marked `complete` reached the frontier write with no engine document — its `_park` " +
             "recipes and its endpoint count are what this origin's cross-session entry is MADE of, and a " +
             "missing one would erase a previous session's parked residue rather than extend it");
      const prior = result._prior;
      /* AND THE PRIOR ENTRY'S OWN RANKING PAIR, ASSERTED WHERE IT IS TAKEN OFF THE ANALYSIS. This copy came
         from `frontierGet` at the root, which is the ONE read of this store that does not go through
         `frontierIndex` and therefore the one entry the record grammar never saw. Its `visits` is the number
         the next admission of this address is amortised against, so an entry written by an older build — or
         by a write that landed half a shed — would restart the count here with nothing to say so, and the
         only symptom would be a document that keeps winning the Level-1 pick. */
      DCHECK(prior === null || prior === undefined ||
             (typeof prior.visits === "number" && Number.isInteger(prior.visits) && prior.visits >= 1),
             "the parked entry this run resumed carries no admission count (`" + String(prior && prior.visits) +
             "`) — it is the divisor the next Level-1 rank of this address is computed from, and a count that " +
             "restarts here is a document that has been re-fetched many times ranking as one nobody has opened");
      /* THE PARKED GRADE, ASSERTED BEFORE THE RECORD IS COMPOSED — AND THIS ASSERT IS WHAT MAKES AN ABSENT
         FIELD MEAN SOMETHING. `frontierProvenance` answers `derived` for an entry carrying none, on the ground
         that no build able to write one could have parked a forced address; that argument holds only while
         EVERY entry a live build writes states a word, which is this line. A composer that stopped stating it
         would not be caught downstream — its entry would quietly rejoin the population whose absence reads as
         "written by an older store" and be re-fetched under a grade nobody granted it.
         IT IS NOT REDUNDANT WITH `engineRoot`'s, AND THE DIFFERENCE IS WHICH CONTRACT EACH IS ABOUT. That one
         is owed to the three fetch closures, which read this word on every request; this one is owed to the
         STORE, whose entries outlive the build that wrote them. A diff that made a document's grade
         per-Document rather than per-instance would move the first and leave this one exactly where it is. */
      DCHECK(eng.msg.provenance === PROVENANCE_OBSERVED || eng.msg.provenance === PROVENANCE_DERIVED ||
             eng.msg.provenance === PROVENANCE_FORCED,
             "a run about to park its residue carries the provenance `" + String(eng.msg.provenance) +
             "` — every composer of an AST_ANALYZE states one (the ambient seed `observed`, a declared route " +
             "and a child navigable the JOIN of the reaching document's grade and their own word, a " +
             "rehydration the word it was parked under), so an absent one is a composer nobody told, and the " +
             "entry it writes would read back as an older store's and be re-fetched as `derived`");
      const _parkProvenance = eng.msg.provenance;
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
        /* `_park` AND `fetchCallSites` ARE GUARANTEED ON THE ARM THIS GATE SELECTS — `linesToAnalysis` writes
           both exactly where an @RESULT document arrived and `assertResultDocument` checked it, and the
           DCHECK above is that premise stated where it is relied on rather than assumed across two functions.
           A `|| []` here cannot fire, and what it CAN do is exactly what this file has been burnt by twice:
           outlive the guarantee. This entry is the cross-session frontier's residue, and the shape of the
           failure it would hide is "the recipes joined to the empty string", which reads to the next session
           as an origin that finished rather than one whose parked flows were dropped. */
        /* AND HOW THE LOAD THAT PRODUCED THIS DOCUMENT GOT ITS ADDRESS — the field `frontierRederive` reads
           when it fetches these bytes back, carried from the analyze record exactly as `credentialed` is, and
           bound above rather than computed here for the reason the frontier-share reply names its awaits
           before it builds its answer: an entry whose receiver is an EXPRESSION is a field no auditor of this
           record can anchor to it. */
        provenance: _parkProvenance,
        credentialed: !!eng.msg.credentialed, recipes: result._park.join(";"),
        /* `emit` IS THE SURFACE THIS RUN DEMONSTRATED, AND IT USED TO ACCUMULATE ONE TERM OF IT PER VISIT.
           Its consumer divides: `frontierWeight` computes `emit / visits` and names it "expected emit per
           admission — future productivity". Added up over admissions, the quotient is the arithmetic MEAN OF
           A SEQUENCE OF TOTALS, and for a document whose surface does not change that mean IS the total — a
           number that does not fall however many fetches are spent re-learning the same endpoints. §scheduler
           requires the opposite of a document ("an unproductive document sinks rather than being re-fetched
           at rank for ever"), and the only term left that could deliver it is the optimism bonus, whose
           ENTIRE RANGE IS 1.0 — which is also exactly what a never-seen address enters at. So a document that
           demonstrated ten endpoints once outranked every address this profile has never opened, permanently,
           and was re-fetched at rank for ever. That is the loop, and it needs no cycle in the seeding to
           arrive: it is what the Level-1 order does to any endpoint-rich page on its own.
           WHAT THE QUOTIENT MEANS NOW IS DERIVED, NOT PICKED. This states the surface the LAST completed run
           demonstrated, so `emit / visits` amortises that surface over the admissions spent reaching it — and
           whenever the demonstrated surface is monotone that is EXACTLY the running mean of the NEW findings
           per visit: a second admission that demonstrates the same ten demonstrated zero new, and 10/2 = 5 is
           the mean of {10, 0}. A document that keeps growing keeps its rate; one that re-learns what it
           already knew decays toward zero and passes below the never-seen addresses at 1.0. No constant, no
           decay rate and no threshold enters — the arithmetic is the definition of the quantity, and the
           crossing it produces is an identity rather than a tuning: a document demonstrating F findings and
           learning nothing new is worth exactly F+1 admissions before it ranks below an address nobody has
           opened, because `F/n + 1/(n+1)` first falls under 1 at `n = F+1` (at `n = F` it is `1 + 1/(F+1)`,
           and at `n = F+1` it is `1 − 1/(F+1) + 1/(F+2)`). A document is worth as many fetches as it has
           shown findings, which is the exchange §scheduler asks for and nobody had to pick.
           IT IS NOT A BOUND AND NOTHING STOPS A SECOND FETCH. The entry keeps its recipes, its address, its
           principal and its MEMBERSHIP of the one frontier at every value of this number; what changes is
           only where it stands, and when nothing outranks it, it is admitted and re-fetched exactly as
           §OOM/paging's re-derivable tier requires. Sinking is starvation, which is permitted; the cap this
           design forbids would be refusing to admit it at all.
           AND IT COUNTS BOTH HALVES OF WHAT A RUN EMITS. §scheduler's value is "new @H+@S"; this counted
           `fetchCallSites` alone, so a document whose entire output is fire-verified @S PoCs and no endpoints
           wrote `emit: 0` and was ranked, for the rest of the profile's life, as one that had found nothing.
           The incremental merge one screen up already refuses that same reading of these same two arrays
           ("Merge on EITHER surface: an XSS-only page carries verified @S PoCs with no endpoints"); the
           persisted reward was the copy that had not been corrected. Both arrays are guaranteed on the arm
           this gate selects, by the same `analysisHasDocument` premise the recipes rest on. */
        emit: result.fetchCallSites.length + result.securitySinks.length,
        /* `visits` COUNTS ADMISSIONS AND THE TWO STATES ARE STATED RATHER THAN DEFAULTED. An absent `prior` is
           the positive fact "this address's residue has never been persisted under this key", which is one
           admission; a `|| 0` reads a prior whose count stopped being written as that same first admission,
           and with `emit` no longer accumulating that restarts the amortisation — handing a document that has
           been re-fetched fifty times the rank of one nobody has opened, which is precisely the ratchet this
           write exists to close. `prior` came off the store through the ONE read that does not pass the record
           grammar, which is why its count is asserted above, before this line reads it. */
        visits: prior ? prior.visits + 1 : 1,
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
      self.onFrontierAdvance(eng.msg.sourceUrl, engineLiveDocumentIds(eng), result, eng._epoch);
    }
    /* EVERY CALLER WAITING ON THIS DOCUMENT IS ANSWERED, not just the first. A cold/child engine has no
       caller at all, which is why the list is allowed to be empty and the merge above is what its findings
       travel on instead. */
    for (const w of eng._resolvers) w.resolve(result);
    eng._resolvers.length = 0;
  },
};
/* THE SCHEDULER DIED, AND A DEAD SCHEDULER IS NOT RE-ENTERED. Set by the rejection arm below and never
   cleared: every rejection out of this loop is an INVARIANT abort (see the arm for why none of them is
   transient), so the state that killed the loop is still in `_pool`/`_waiting` when the next kick arrives and
   the next round dies at the identical line. `_hostKicksRefused` counts the kicks that arrive after, because a
   refusal nothing records is the silence this whole file is written against — the pair says "the scheduler is
   down, and N callers have asked for it since", which is a diagnosis, where a bare no-op is a mystery. */
let _hostDead = null;
let _hostKicksRefused = 0;
function _hostKick() {
  /* DEAD IS ASKED BEFORE DRIVING, AND THE ORDER IS THE WHOLE VALUE OF THE COUNTER. Behind the `_hostDriving`
     guard this line was unreachable and `kicksRefused` could only ever read 0 — a field READ by the probe and
     WRITTEN by nothing, which is the contract break §Architecture names and the reason a default is never
     allowed to stand in for a producer. The two are not the same question either: `driving` says a round is in
     flight, `dead` says there will never be another, and only one of them outlives the round.
     REFUSED, NOT RETRIED, AND NOT ASSERTED EITHER. The abort has already been reported ONCE, by the arm that
     latched it; a DCHECK here would blame whichever content script happened to arrive next for a violation
     that is neither theirs nor new, and would throw that abort into a fresh caller on every subsequent
     navigation — the same unbounded repetition this latch exists to end, relocated. */
  if (_hostDead) { _hostKicksRefused++; return; }
  if (_hostDriving) return;
  _hostDriving = true;
  /* THE ONE LOOP'S FAILURE IS NOT A DEBUG LINE. Every assertion in the bridge — the notice router's field
     counts and its DFAIL on an op this zone does not act on, the owed-request checks, the result document's
     shape, the merge's own contract check — is thrown from inside this loop, and `.catch(console.debug)` took
     the whole class and printed one line of it at a level nothing reads. Worse, it did so while the analysis
     promise for the document being stepped was still unresolved, so a violated invariant surfaced as a page
     that simply never finished. A LOUD banner, then rethrow in dev: an unhandled rejection is the honest
     shape of "the scheduler died", and it is impossible to overlook.
     THAT REPORT WAS BEING DEFEATED ON THE VERY NEXT LINE, AND IT COST 16 MINUTES OF A WEDGED BROWSER. The
     two outcomes shared one `.finally`, so a loop that DIED was carried into the same re-kick as a loop that
     finished a round — and since the only thing that shrinks `_pool` is `finish`, which a dying round never
     reaches, `_pool.length` stayed true for ever. One invariant abort therefore became an unbounded retry at
     the rate of the one IDB read `admit` opens with: MEASURED on the real extension, two same-origin
     documents in one tab (which is what a link click IS, and what makes this the continuous-browsing case
     rather than an edge case), 23,163 identical aborts and 17.9 s of renderer CPU inside 20 s of wall clock,
     49 MB of console, the offscreen document unreachable over CDP, and no end state — the analysis promises
     for BOTH documents unresolved throughout. The "impossible to overlook" unhandled rejection was overlooked
     precisely because there were thousands of it a second and the one that mattered was the first.
     So the two paths are SPLIT. Re-kicking is what a COMPLETED round does; the failure arm latches and stops.
     A dead scheduler is a worse outcome for the user than a live one — that is the point of it. §Offensive
     programming: the crash is the system doing its job, and the job is to force the root fix, which a retry
     loop actively prevents by burying the one line that names it. */
  hostSchedule(_pool, _hostOps).then(
    /* THE COMPLETED ROUND, WHICH IS THE ONLY THING THE RE-KICK WAS EVER REASONED ABOUT.
       RE-KICK ON THE POOL ALONE. `|| _waiting.length` stood here so a document held back by the RAM floor was
       picked up when a slot freed — but a freed slot is a pool that is not empty, and with an EMPTY pool RAM is
       free by definition, so the only document that can still be waiting is one admit DEFERRED (a sub-frame
       whose same-origin top has not reported yet). Re-kicking for that one spins the loop at full speed on a
       condition nothing in this file can change; the thing that changes it is the top's CONTENT_SEED, which
       kicks the pool itself when it arrives. */
    () => { _hostDriving = false; if (_pool.length) _hostKick(); },
    /* THE LOOP DIED. NOTHING RETRIED, BECAUSE NOTHING HERE IS RETRYABLE: every rejection that reaches this arm
       is a should-never-happen — a bridge assertion, or an invariant abort travelling out of `_readyP` (an
       ENGINE abort is not one of them; engineBootFailed answers those on the record and RESOLVES, precisely so
       that one page failing to boot does not take the pool down). A should-never-happen does not become false
       by being asked again, and the second ask is over the same `_waiting` entry and the same `_pool`.
       `_hostDriving` IS CLEARED HERE LIKE ANYWHERE ELSE, because it answers "is a round in flight" and no round
       is. Holding it true as a second bolt against re-entry was one fact spread over two fields, and it made
       the refusal counter unreachable — `_hostKick` returned on `driving` before it could record that a
       document had arrived to a dead scheduler, so the probe's one number for "how much has been silently
       dropped since" was structurally always 0. The latch is what refuses; this field just tells the truth. */
    (e) => {
      _hostDead = e;
      _hostDriving = false;
      crashBanner("host-wfq", String((e && e.stack) || e));
      if (self.APICLIENT_DEV) throw e;
    });
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
    /* A SEAT WHOSE DOCUMENT IS STILL ON THE NETWORK IS REPORTED AS ITSELF, AND IT IS NOT COUNTED AS A
       RESERVATION. It has no renderer, no agent cluster and no document — the load has not landed, so there is
       nothing yet to key or to name — and every Level-1 fact it could carry is ABSENT for the reason the
       reservation arm states one line down: a `heapBytes: 0` is the default that admits another engine against
       RAM about to be spent. `booting` is deliberately NOT incremented: the arithmetic below is a claim about
       `engineCreate`'s own made/rooted/failed ledger, and a seat that never called it would make a correct
       pool read as one holding a phantom reservation — an assert firing on the repair that put the seat there. */
    if (eng.state === "loading") {
      DCHECK(eng.what !== null && typeof eng.what === "object" && typeof eng.what.kind === "string",
             "a loading seat is in the pool naming no work item — the seat exists so a LATER round can finish " +
             "the admission its fetch was spent on, and one that cannot say which kind of work it is holding " +
             "is a slot that blocks admission and can never be landed by anybody");
      return { name: null, docId: null, topDocId: null, state: "loading",
               framed: false, routingId: null, heapBytes: null, topWeight: null,
               cold: !!eng._cold, joined: eng._resolvers.length, joinedDocIds: eng.joinedDocIds.slice(),
               loadKind: eng.what.kind, settled: !!eng.settled };
    }
    if (eng.state === "booting") {
      booting++;
      DCHECK(typeof eng.cluster === "string" && eng.cluster !== "" && typeof eng.docId === "string" && eng.docId !== "",
             "a reservation is in the pool naming no agent cluster or no document — it is answered for by " +
             "both (hostClusterOf for admission, hostHolderOf for routing), so a nameless one is a slot that " +
             "blocks admission while answering nobody");
      return { name: eng.cluster, docId: eng.docId, topDocId: eng.topDocId, state: "booting",
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
    return { name: eng.r.name, docId: eng.docId, topDocId: eng.topDocId, state: eng.state,
             framed: !!eng.r.frame.parentNode,
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
  /* IS THE ONE LOOP STILL ALIVE, which is a fact no other field here can be read for. Every number below
     describes what the pool HOLDS, and a dead scheduler holds exactly what it held when it died — so a probe
     reading a plausible pool, a plausible waiting count and a plausible renderer set was the picture of a
     healthy extension and of a wedged one alike. `kicksRefused` is what tells them apart from the outside: it
     is the number of documents that have arrived since and been answered by nothing. */
  return { renderers: renderers, registry: registry, mojo: self.mojo.stats(),
           scheduler: { alive: !_hostDead, driving: _hostDriving, kicksRefused: _hostKicksRefused,
                        diedOf: _hostDead ? String((_hostDead && _hostDead.message) || _hostDead) : null },
           pool: pool, waiting: _waiting.length, residentBytes: _residentBytes(),
           /* AND THE ADMISSIONS WHOSE DOCUMENT IS STILL ON THE NETWORK, WHICH IS THE ONE THING ABOUT THIS
              STATE THAT CAN BE OBSERVED FROM OUTSIDE. It is NOT folded into `reservations.booting`: that
              block is an accounting of `engineCreate`'s own ledger and a seat has never called it. */
           loadingSeats: _loadingCount(),
           reservations: { made: _reserveStats.made, rooted: _reserveStats.rooted, failed: _reserveStats.failed,
                           booting: booting, inFlight: inFlight, peakBooting: _reserveStats.peakBooting,
                           joinedBooting: _reserveStats.joinedBooting, joinedRooted: _reserveStats.joinedRooted,
                           navigated: _reserveStats.navigated, seeded: _reserveStats.seeded,
                           evicted: _reserveStats.evicted, rehydrated: _reserveStats.rehydrated },
           coldRows: _frontierIndexBuilt ? _frontierIndex.size : null,   // null = the ranking view has not been asked for yet
           /* THE STORE'S DECISIONS, WHICH ARE THE ONLY THING ABOUT QUOTA THAT CAN BE OBSERVED FROM OUTSIDE. A
              store that did not crash is not evidence the relief works, so what is reported is what the one
              order DECIDED: documents shed to the share, documents fetched back, residues that were shed and
              then could not be fetched back (`stranded` — this design being wrong in the direction that costs
              something, stated as a number), and `overShare`, the bytes the share asked for that residency
              refused to take because taking them meant discarding the only copy of a residue. `share` is null
              until the preference has been read, which is a different fact from a share of zero. */
           frontier: { shed: _frontierStats.shed, rederived: _frontierStats.rederived,
                       stranded: _frontierStats.stranded, docBytes: _frontierStats.docBytes,
                       overShare: _frontierStats.overShare, share: _frontierShare,
                       defaultShare: frontierDefaultShare() },
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
    _frontierIndex.clear();   // the ranking view is the store's projection: a cleared store ranks nothing
  } catch (e) { RETHROW_FATAL(e); frontierFail("clear", e); }
}
async function hostClear() {
  const waiting = _waiting.splice(0);
  /* AND THE ROUTES DECLARED IN THIS SESSION GO WITH THE FRONTIER THEY WERE WORK ITEMS ON. A Clear empties the
     cold tier and drops every live engine; a declared address that survived it would be the one member of the
     one frontier that a wipe did not reach, and the next idle kick would fetch a page of an origin the person
     has just asked this tool to forget. There is nobody to reject: a declaration has no caller by construction
     (see the notice router — it is a work item, not a request), which is the same reason its instance is
     created `cold`. */
  _seeds.clear();
  const dropped = _pool.splice(0);
  for (const job of waiting) job.reject(new Error("cleared"));
  for (const eng of dropped) { for (const w of eng._resolvers) w.reject(new Error("cleared")); eng._resolvers.length = 0; }
  /* AND THE FRAME GOES WITH THE ENGINE, which a dropped Module never needed: an engine that leaves the pool is
     never stepped and never finalized, so nothing else will ever reach its renderer — and an iframe nobody
     reaches is a whole WASM instance held resident under a document that does not reload until the browser
     restarts. Two Clears with a heavy page open would have left two of them.
     A ROUND IN FLIGHT KEEPS ITS FRAME UNTIL IT LANDS, and the question that decides it is ASKED rather than
     inferred from the state. This read `state === "fetching"` and its comment said that state "is exactly
     'this engine has a call outstanding'" — which is FALSE, and measurably so: a `hot` engine has a call
     outstanding for the whole of every awaited scheduler op (setFloor, requestPark, step, streamPartial), which
     is where the loop spends essentially all of its time. Clearing with one page open therefore destroyed a
     renderer mid-`step` and hit renderer-host's own assert — "a renderer was destroyed with 1 call(s) still
     outstanding" — which travels back out of astDispatch as an invariant abort, so AST_CLEAR rejected,
     `clearGlobalStore` never ran, and the Clear button did not clear. The transport knows the answer, so it is
     the transport that is asked; `_dropped` is what the owning round reads on the way out (see serviceFetch and
     hostSchedule's release step). */
  /* AND A RESERVATION KEEPS ITS FRAME FOR THE SAME REASON, WHICH IS WHY THIS IS WRITTEN AS THE POSITIVE SET
     RATHER THAN AS `!== "fetching"`. A booting engine has no `r` at all until rendererLaunch answers, and from
     that instant until engineRoot finishes it has an ABI call outstanding on every await in between — so it is
     the same case the negative form would have missed twice over (a TypeError, then a broken renderer-host
     rule). engineCreate reads `_dropped` when its provisioning lands and removes the frame there, on both
     arms, exactly as the service round does. */
  for (const eng of dropped) { eng._dropped = true; if (eng.state === "hot" && eng.r.outstandingCalls() === 0) eng.r.destroy(); }
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
    /* THE PERSON'S STANDING EGRESS SENTENCE IS AWAITED HERE BECAUSE THE ARMS BELOW READ THE TABLE
       SYNCHRONOUSLY, WHICH IS A DIFFERENT REASON FROM THE ONE THAT USED TO STAND HERE. `AST_EGRESS_POLICY`
       answers a person at a surface through `safeFetchWiden` / `safeFetchPermit` / `safeFetchPermitted` /
       `safeFetchEgressTable` / `safeFetchWidenedOrigins`, and every one of those is a `CHECK` on the table
       having been stated — fatal in release too. None of them is a fetch, so none of them is behind the
       chokepoint's own wait, and this is the only place they can be put behind it.
       WHAT IT IS NO LONGER IS THE THING THAT KEEPS `_firingRefusal`'s ASSERT QUIET. That is `safeFetch`'s
       wait on the promise this file registers at load, and the argument that used to stand here is recorded
       at the assert rather than repeated: it held that the table is read only for a `forced` request, that
       only an engine composes one, and that every engine is created behind this function — and the first
       clause was false, so a `derived` data GET from this zone's passive-learning arm reached the assert
       without passing here at all.
       IT IS AWAITED FOR EVERY TYPE AND NOT ONLY THE ONE THAT NEEDS IT, because which types reach a
       synchronous table read is a question about the rest of this function and a premise stated here would
       go stale inside it. It is the same memoized read the registration at load already started, so by the
       time any message arrives this is a settled promise and costs a microtask. */
    await egressPolicyReady();
    /* TWO TYPES, AND A THIRD IS AN UNBUILT CAPABILITY THAT SAYS SO. This entry used to answer every type but
       one with `{success:false, error:"unknown type"}`, and the senders wrapped their calls in catches — so
       AST_CLEAR (the Clear button's "stop all work") and SET_ANALYSIS_OPTS (the popup's Settings panel) were
       both refused in silence for as long as they existed. AST_CLEAR is now BUILT below; SET_ANALYSIS_OPTS,
       its two controls and its persisted record are DELETED, because neither knob may exist: the working set
       is bounded by resident WASM memory and not by a user-set instance count, and a wall-clock throttle over
       the cooperative quantum is a step cap. What is left is a real refusal, so it aborts rather than
       reporting a false success to a caller that will not look. */
    DCHECK(!!msg && (msg.type === "AST_ANALYZE" || msg.type === "AST_CLEAR" ||
                     msg.type === "AST_FRONTIER_SHARE" || msg.type === "AST_EGRESS_POLICY"),
           "the trusted zone dispatched a type this bridge does not answer: `" + (msg && msg.type) + "` — " +
           "every edge into the engine is built here, so an unanswered type is a capability that was asked " +
           "for and never made, not an option the caller may proceed without");
    if (!msg) return { success: false, error: "dispatch with no message" };   // release path under the assert
    if (msg.type === "AST_CLEAR") return { success: true, result: { cleared: await hostClear() } };
    /* THE ONE SETTING THIS SURFACE MAY CARRY, AND WHY IT IS NOT THE TWO THAT WERE DELETED. "Yield throttle
       (ms)" was a step cap under a label and "Analyzer workers" was a fixed instance count the RAM floor
       exists to refuse — both were knobs on WORK, and CLAUDE.md §NO BOUNDS says work is not the user's to
       bound. This one is a knob on STORAGE: how much of the user's own disk the cross-session frontier's
       stored DOCUMENTS may occupy. It decides nothing about what runs, what is explored, or what is
       remembered — every recipe, every counter and every entry survives at every setting, including zero —
       only whether a document is held or FETCHED BACK when it is next wanted. That is a preference about the
       person's device, which nothing in the engine can answer for them.
       IT IS NOT SILENTLY REFUSED, WHICH IS WHAT THE DELETED PAIR WAS. The reply carries the value that is now
       in force, so a caller that set one and reads back another knows it. */
    if (msg.type === "AST_FRONTIER_SHARE") {
      DCHECK(msg.bytes === undefined ||
             (typeof msg.bytes === "number" && Number.isFinite(msg.bytes) && msg.bytes >= 0),
             "a frontier-share setting arrived that is not a byte count (`" + String(msg.bytes) + "`) — it is " +
             "compared against the stored document halves, and a value no comparison is true of is an " +
             "UNLIMITED store wearing the appearance of a configured one");
      if (typeof msg.bytes === "number" && Number.isFinite(msg.bytes) && msg.bytes >= 0) {
        await frontierPrefPut("share", msg.bytes);
        _frontierShare = msg.bytes;
        /* THE SETTING TAKES EFFECT ON THE SETTING, not at the next park. Residency is an invariant of the
           store and this call is one of the two things that can break it (the other is a write), so it is
           restored here for the same reason and in the same place. */
        await frontierResidency();
      }
      /* THE TWO AWAITS ARE NAMED BEFORE THE REPLY IS BUILT, in the order they already ran. A reply record is
         this seam's own subject — the popup reads every one of these names — and an `await` INSIDE the literal
         gives one of its entries a receiver that is an expression rather than a binding, which is a value no
         auditor of the seam can anchor to a record. `frontierIndex` writes none of `_frontierStats` (it reads
         the store, asserts each row's grammar and builds a ranking view), so hoisting it above those five reads
         moves no number. */
      const _share = await frontierShare();
      const _index = await frontierIndex();
      return { success: true, result: { share: _share, defaultShare: frontierDefaultShare(),
                                        docBytes: _frontierStats.docBytes, overShare: _frontierStats.overShare,
                                        shed: _frontierStats.shed, stranded: _frontierStats.stranded,
                                        rederived: _frontierStats.rederived,
                                        entries: _index.size } };
    }
    /* ─── THE PERSON'S EGRESS SENTENCE, AND THE ONLY DOOR THE EXTENSION HAS ONTO IT ──────────────────────
       CLAUDE.md §Attacker-sources makes firing what a bundle reaches only past a forced gate "CONFIGURABLE
       AND PER-ORIGIN, BECAUSE EXPERIMENTATION IS NOT ALWAYS WRONG AND A SINGLE SWITCH CANNOT SAY SO …
       Default conservative, widened deliberately per origin, never inferred from a site looking like a test."
       The chokepoint has held that table for a while and `--explore <origin>` has written to it from a
       command line; the offscreen has no command line, so in the SHIPPED extension there was no way for the
       person the setting is about to say anything at all. This is that way.
       IT DECIDES NOTHING AND IT MUST NOT. Every question — what a widening means, what may be widened, and
       what any given request is answered with — is `lib/safe-fetch.js`'s, which is where SECURITY.md puts
       the network policy and where both hosts read it from. What arrives here is a PERSON'S ANSWER to that
       policy, carried to it. A second rule on this path would be the second policy point the whole parameter
       exists to end.
       THE AUTHORIZATION IS THE ROUTER'S AND IS ASSERTED AT THE DOOR THIS MESSAGE ARRIVES AT, not here: a
       message reaching `handlePopupMessage` has already been gated on `sender.origin === EXTENSION_ORIGIN`
       (SECURITY.md's document→document rule — browser-set, and the opaque `"null"` a sandboxed extension
       page carries is not it), and that case re-asserts it where it is relied on.
       WHAT THIS ENTRY ASSERTS IS THE OTHER HALF, WHICH NO PRINCIPAL CAN ANSWER: that a HUMAN initiated the
       act. A trusted extension document is where a person's click arrives AND where an automatic caller
       would sit, and nothing about the message tells them apart — which is the mistake this project has
       already made once on the page-context relay, where three automatic senders were covered by an
       exemption scoped by a sentence about who the callers were. So the grade TRAVELS, stated by the surface
       that knows, and an absent one takes the refusing arm. The constant is `lib/schema.js`'s because the
       question is the same question and this project states it once; the identifier is that relay's only
       because that is where a human-initiated act first had to be told from a tool-initiated one.
       `subject` IS ECHOED AND ITS REFUSAL IS ANSWERED, which is what makes this a surface a person can read
       rather than a switch they must trust. `safeFetchFiringRefusal` is the SAME function the chokepoint
       refuses requests with, asked about a forced DATA request at that origin — so the control shows what
       this origin's requests are actually answered with now, in the policy's own vocabulary, before and
       after the person changes it. */
    if (msg.type === "AST_EGRESS_POLICY") {
      CHECK(msg.initiator === PAGE_CONTEXT_USER_INITIATED,
            "the per-origin egress widening was reached with the initiator grade " +
            JSON.stringify(msg.initiator) + " — a widening is a PERSON'S SENTENCE and CLAUDE.md says it is " +
            "\"never inferred from a site looking like a test\", so the one thing this command may not do is " +
            "be issued by something that decided on its own. A caller that cannot state the grade never " +
            "answered the question, which is why an absent value takes this arm and not the permissive one");
      /* AT MOST ONE COMMAND PER MESSAGE, AND THE COUNT IS TAKEN RATHER THAN THE PAIRS ENUMERATED — a rule
         re-written per pair has a hole for every command nobody has added yet, which is the general form
         `_refuseUnreadOptions` already carries one file over. Three commands make three pairs; the fourth
         makes six, and the one that gets forgotten is the one whose order is then decided by the shape of
         an `if` rather than by anybody. */
      DCHECK([msg.grant, msg.revoke, msg.permit].filter((c) => c !== undefined).length <= 1,
             "the egress command carried more than one of grant/revoke/permit in one message — they are " +
             "competing sentences about one table and the order they would be applied in is whichever this " +
             "entry happens to test first, which is a permission decided by the shape of an `if`");
      DCHECK(typeof msg.subject === "string",
             "the egress command carried no subject — it is the origin the surface is SHOWING the person, " +
             "and the refusal answered below is about it, so without one the control would report a policy " +
             "answer for nothing while the person read it as being about the page in front of them");
      /* THE GRANT IS REFUSED WITH ITS REASON AND NEVER ASSERTED. `safeFetchWiden` aborts on an origin it
         cannot use, which is right for a caller inside this project and wrong for a person looking at a
         sandboxed document or a `file://` page: an opaque origin is an ordinary state of the web, and a
         fatal here would hand any page that sandboxes an iframe an abort switch on the trusted zone. So the
         chokepoint's OWN predicate is asked first and its answer is carried to the surface. */
      let refused = null;
      let changed = false;
      if (msg.grant !== undefined) {
        DCHECK(typeof msg.grant === "string",
               "the egress command was asked to grant " + JSON.stringify(msg.grant) + " — a widening names " +
               "an ORIGIN, and a non-string would reach the chokepoint's parse as a permission about nothing");
        refused = self.safeFetchWidenable(msg.grant);
        if (refused === null) { self.safeFetchWiden(msg.grant); changed = true; }
      } else if (msg.revoke !== undefined) {
        DCHECK(typeof msg.revoke === "string",
               "the egress command was asked to revoke " + JSON.stringify(msg.revoke) + " — a revocation " +
               "names an ORIGIN, and a non-string would silently remove nothing while reading as a " +
               "permission the person had just taken back");
        changed = self.safeFetchUnwiden(msg.revoke);
      } else if (msg.permit !== undefined) {
        /* ONE SIGNAL, ONE VALUE, ONE ORIGIN — THE PER-SIGNAL DOOR, AND IT DECIDES NOTHING. What arrives is a
           person's answer to a row `lib/safe-fetch.js` declared and this surface rendered; whether that row
           exists, whether the value is one of its own, and what permitting it MEANS are all the chokepoint's,
           asked here through its own predicate so the two cannot drift.
           IT IS ONE ROW PER MESSAGE AND DELIBERATELY NOT A BATCH. A control that set several rows at once
           would make the table a person reads back a summary of the act rather than the policy now in force,
           which is the rule this whole row already keeps: every answer below is read off the chokepoint. */
        DCHECK(msg.permit !== null && typeof msg.permit === "object" &&
               typeof msg.permit.origin === "string" && typeof msg.permit.signal === "string" &&
               typeof msg.permit.value === "string" && typeof msg.permit.allow === "boolean",
               "the egress command carried a permission that is not an {origin, signal, value, allow} — a " +
               "field missing here would reach the chokepoint as `undefined`, which its own predicate " +
               "refuses, so the person would be told their row could not be permitted for a reason about " +
               "this message rather than about their app");
        /* REFUSED WITH ITS REASON AND NEVER ASSERTED, for the reason the grant above gives: a person looking
           at a sandboxed document or a row this build no longer declares is an ordinary state of the world,
           and a fatal here would hand any page that sandboxes an iframe an abort switch on the trusted zone.
           The ORIGIN and the ROW are two questions and both are asked, because a message naming a good
           origin and a retired signal would otherwise abort inside `safeFetchPermit`'s own CHECK. */
        refused = self.safeFetchWidenable(msg.permit.origin);
        if (refused === null) refused = self.safeFetchSignalUsable(msg.permit.signal, msg.permit.value);
        if (refused === null) {
          self.safeFetchPermit(msg.permit.origin, msg.permit.signal, msg.permit.value, msg.permit.allow);
          changed = true;
        }
      }
      /* THE STORE MOVES ONLY WHERE THE TABLE DID, and it is written FROM the table. A refused grant persists
         nothing, and a revocation of an origin that was not there persists nothing — so the record cannot
         come to hold a permission the chokepoint refused. */
      if (changed) await egressPolicyPersist();
      /* AND WHAT A FORCED DATA REQUEST AT THE SUBJECT IS ANSWERED WITH RIGHT NOW. `""` is the destination a
         data fetch carries (never §2.2.5 script-like, which is the arm that fires by default whatever this
         table says), `unstated` is the honest witness mark for a question that is not an act, and `forced`
         is the one grade the widening is about. A subject the chokepoint could not widen is not asked —
         `safeFetchFiringRefusal` takes an absolute URL and an unparseable one is a caller's serializer
         disagreeing with a URL parser, which it THROWS on rather than answering a permission question about
         nothing. `null` there is the positive statement "this origin cannot be the subject", which the
         surface renders as the reason rather than as a policy answer. */
      const _subjectUsable = self.safeFetchWidenable(msg.subject);
      /* THE PROBE IS A FORCED, UNCREDENTIALED DATA REQUEST AT THE SUBJECT'S OWN ROOT, and every one of those
         words is a choice this line has to make rather than a default it falls into. `""` is the destination
         a data fetch carries (never §2.2.5 script-like, which is the arm that fires by default whatever this
         table says); `forced` is the grade a permission is most about; `unstated` is the honest witness mark
         for a question that is not an act; `false` is the credential state the learned-GET replay states
         today. The ADDRESS is the origin's own root, which is the only address this surface has — so the
         `url-authority` row it shows reads `unknown`, and that is exactly what that row means and not a
         statement that this origin's addresses carry none. */
      /* AND THE REACH GRADE IS `forced` FOR THE PROVENANCE'S OWN REASON — it is the grade a permission is
         most about, so the row this surface shows as `this request` is the one a person is deciding. It is
         NOT a claim that any document at this origin was reached that way; it is the hardest case, which is
         what a control surface owes somebody about to tick a box. */
      /* AND `tool` FOR THE ACTOR, FOR THE SAME REASON EVERY OTHER WORD ON THIS PROBE IS THE HARDEST CASE:
         a request the ANALYSED PAGE made now fires by default wherever its address rests on nothing this
         engine pinned, so the act a person is actually deciding about at this origin is the one THIS TOOL
         composed. Showing them the row that is already answered would be a control about a question they do
         not have. It is not a claim that anything at this origin was composed that way. */
      const _probe = { url: msg.subject + "/", destination: "", provenance: PROVENANCE_FORCED,
                       pinned: "unstated", docReach: PROVENANCE_FORCED, actor: "tool",
                       credentialed: false, headers: null };
      return { success: true, result: {
        origins: self.safeFetchWidenedOrigins(),
        subject: msg.subject, subjectUsable: _subjectUsable,
        subjectRefusal: _subjectUsable === null ? self.safeFetchFiringRefusal(_probe) : null,
        /* WHAT THE PERSON IS DECIDING ABOUT, ROW BY ROW, AND IT IS THE POLICY'S OWN DERIVATION RATHER THAN
           THIS FILE'S GUESS AT IT. `signals` is the registry with each value's permitted state at this
           origin; `vector` is what THIS probe's facts compute, so a person can see which row their current
           refusal is about; `defaults` is what fires without anybody saying so, which is the answer to "why
           do my app's scripts still load". Every one of them is read from `lib/safe-fetch.js` — a surface
           that computed any of them would be the second copy of the policy this whole parameter exists to
           end. */
        signals: _subjectUsable === null ? self.safeFetchPermitted(msg.subject) : [],
        vector: _subjectUsable === null ? self.safeFetchSignalVector(_probe) : null,
        defaults: self.safeFetchDefaultArms(),
        legacyDropped: _egressLegacyDropped,
        refused, changed } };
    }
    if (msg.type !== "AST_ANALYZE") return { success: false, error: "unknown type " + msg.type };
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
    /* ─── THE SEED'S OWN §7.4 NAVIGATION, PERFORMED HERE AND NOWHERE ELSE ────────────────────────────────
       The trusted zone hands this entry an ADDRESS the ambient observer suggested and the browser's own
       answer agreed with; the document behind it is loaded HERE, through `navigationLoad` — the same
       function a child navigable's load goes through, which is what makes "one document-load path" a shape
       rather than a rule. Nothing arrives carrying bytes any more, so there is nothing to compare a second
       transport's bytes against and nothing for a carve-out to hide in.
       THE PRINCIPAL IS THE BROWSER'S, NOT THE SEED'S. `msg.sourceUrl` is the address the BROWSER reported
       for this document (MessageSender-derived, in the trusted zone), and safeFetch classifies the SSRF host
       relative to it — so a suggested address can never authorize itself into the person's intranet. The
       zone that admits the seed has already established the two are same-origin, which is the whole of what
       HTML §7.2.5 "The History interface" lets an SPA's pushState change. */
    DCHECK(typeof msg.seedUrl === "string" && msg.seedUrl !== "",
           "an AST_ANALYZE for a live document named no seed — a seed is an ADDRESS and it is the whole of " +
           "what an ambient observer contributes, so a dispatch without one is a document nothing can load");
    DCHECK(typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
           "an AST_ANALYZE for a live document carried no browser-stated address — it is the private-network " +
           "principal this navigation is classified against, and without it a suggested address would be " +
           "classified against itself");
    DCHECK(msg.pageHtml === undefined && msg.responseHeaders === undefined,
           "an AST_ANALYZE for a live document arrived carrying a document body or a header list — a seed is " +
           "an address and never bytes, so a producer that ships either has rebuilt the second, unpoliced " +
           "document-load transport this entry exists to be the only alternative to");
    /* AND IT CARRIES THE PERSON'S SESSION, because this is one of the custom browser's own tabs and the
       person navigated here. The credentialed-read principal is `msg.origin` — the browser's
       MessageSender.origin, minted by `_browserFacts` from the object the browser filled in — and NEVER
       `originOf(msg.sourceUrl)`, which is the URL-derivation this principal exists to forbid.
       IT IS SAME-ORIGIN BY CONSTRUCTION, so the condition `navigationCarriesSession` tests is one the zone
       that admitted the seed has already established: it refuses a seed whose origin is not the origin of
       the browser-stated address, and HTML §7.2.5 "The History interface" lets `pushState` move the PATH and
       never the origin ("if targetURL and documentURL differ in their scheme, username, password, host, or
       port components" the rewrite is refused). Two zones reaching the same answer from two facts is not a
       redundancy to collapse — the brain compares two ADDRESSES to decide this is a route of this document,
       and this compares an address against a BROWSER-STATED ORIGIN to decide whose bytes may be read, which
       is what makes an opaque-origin (sandboxed) document's load uncredentialed while its seed is admitted.
       AND ITS PROVENANCE IS `observed`, WHICH IS THE ONE PLACE IN THIS FILE THAT WORD IS TRUE OF A NAVIGATION
       AND IS A FACT RATHER THAN AN ASSUMPTION. solver/engine.h defines `observed` as "a real load of this
       document makes exactly this request", and an ambient seed is that in the strongest possible form: the
       address is the one the browser ACTUALLY NAVIGATED TO (`PerformanceNavigationTiming.name`, which a
       `pushState` cannot forge — CLAUDE.md §PASSIVE-AND-FORCED-DISCOVERY names exactly that as the one fact
       an ambient observer contributes), so the person's own browser performed this exact credentialed GET
       seconds ago in this same profile. It is stated HERE, by the zone that knows where the seed came from,
       and never derived inside the loader from the shape of the address. */
    /* AND `observed` FOR THE ISSUING CONTEXT TOO, WHICH IS A SECOND STATEMENT AND NOT A RESTATEMENT: the
       browser really navigated to the page this seed came from, which is the same sentence the word beside
       it makes about the navigation itself. */
    /* AND `tool` FOR WHOSE ACT IT IS, WHICH IS NOT IN TENSION WITH THE `observed` ABOVE AND IS THE ONE WORD
       THIS CALL USED TO GET WRONG. The two answer different questions: `observed` says the person's own
       browser really made this exact request seconds ago, and `actor` says who composed the request THIS
       ZONE is about to make — which is this tool, from an address an ambient observer reported, with
       no analysed page's code anywhere in it. safe-fetch.js's `_actorOf` decides it: "A REQUEST THIS TOOL
       COMPOSED AT A PERSON'S DIRECTION IS STILL `tool`", and it names this file's residue re-fetch and
       trusted.mjs's command-line seed as the two it had in mind. The owner's value arm's enumeration says
       "the seed and the residue re-fetch state `tool`", so this is the word that arm was already written
       against. It fires either way today — measured, `node testing/egress_arm_probe.mjs` —
       because the arm that admits it is keyed on the PATH and names no actor.
       AND THE SIBLING HOST ALREADY ANSWERED THIS QUESTION THE OTHER WAY, WHICH IS WHAT MAKES THIS A
       REPAIR RATHER THAN A PREFERENCE: `engine/trusted.mjs`'s own command-line seed states
       `actor: 'tool'` and gives the identical reasoning in its own words — "no analysed document
       exists yet when a seed is fetched, so `page` would be false. It changes no outcome: the
       observed/observed arm already admits this load". So ONE question had TWO answers in TWO hosts,
       each with confident prose under it, which is the shape CLAUDE.md records for the firing policy
       itself before it moved to the chokepoint. The native zone was right and this line was wrong, and
       the two zones now state one word for one act. */
    const loaded = await navigationLoad(msg.seedUrl, msg.sourceUrl, msg.sourceUrl, msg.origin,
                                        /* `none` — AND IT IS THE ONE SEED THAT CANNOT REPORT A DECLINE,
                                           which is why the two seeds no longer answer with one literal. This
                                           one CAN report an unavailability: `onNavigationOutcome` carries a
                                           page-source record a person reads. It cannot report a DECLINE,
                                           because `_PAGE_SOURCE_KINDS` is `status`/`empty`/`network` and
                                           every one of those says a load was ATTEMPTED and failed — the
                                           same sentence the loader's abort makes about the engine, one zone
                                           further out. WHAT IS NOT COVERED: a declined ambient seed. WHAT
                                           THE NEXT DIFF BUILDS: a fourth page-source kind through
                                           offscreen-brain.js's `_PAGE_SOURCE_KINDS`, its sibling
                                           pairing asserts and the popup that renders it, after which this
                                           word becomes `report`. HOW ITS ABSENCE SHOWS: a run whose log
                                           carries this loader's `none` abort with an ambient seed's own
                                           address in it. MEASURED UNREACHABLE TODAY rather than argued:
                                           both destructive gates are scoped `credentialed && provenance !==
                                           "observed"` and this seed states `observed`, the firing policy
                                           fires for the observed/observed arm at every address, and no
                                           other `decline` producer applies to a GET navigation with no
                                           stream body. */
                                        PROVENANCE_OBSERVED, PROVENANCE_OBSERVED, /*refusalArm*/"none",
                                        /*actor*/"tool");
    /* THE SEED'S OWN RULE, ON TOP OF THE LOADER'S, AND IT IS THE SEED'S BECAUSE IT IS ABOUT A BUNDLE. §7.4.5
       gives an OK response with a zero-length body a perfectly ordinary empty Document, and a child navigable
       gets exactly that — but a SEEDED document with no bytes cannot be the program this run exists to
       explore, and analysing it would emit nothing and read as a page that was analysed and found clean. */
    /* AND A SEEDED LOAD THAT LANDED ON ANOTHER ORIGIN IS NOT THIS DOCUMENT, WHICH IS THE PRICE OF TAKING THE
       RESPONSE'S URL. §7.4.5 determines the loaded Document's origin over the RESPONSE's URL and this entry
       adopts that answer as `sourceUrl` — but the PRINCIPAL this instance runs under is the browser's
       `MessageSender.origin` and cannot be re-derived from an address (a sandboxed document has an ordinary
       URL and an opaque origin, which is why nothing here parses one into a principal). So a seed whose
       response URL is cross-origin to the address the browser reported would seat a document of origin B
       inside a cluster keyed on origin A: two origins behind one credentialed-read principal, which is
       exactly what engineJoin's own DCHECK refuses one algorithm along, and what SECURITY.md's
       one-instance-per-origin-keyed-agent-cluster forbids.
       AND THE CHOKEPOINT NOW REFUSES THE CREDENTIALED CASE ONE LAYER EARLIER, WHICH DOES NOT MAKE THIS DEAD.
       A load that carried the session and landed on another origin comes back as `blocked-cors-credentialed:
       <landed origin>` — safeFetch's credentialed SOP reads the POST-REDIRECT origin, so the bytes never
       leave that function, which is strictly better than reading them here and discarding them. This branch
       is what still answers for a load that carried NO session and so met no such gate: a document whose
       browser-stated origin is OPAQUE (a sandboxed frame) is seeded and loaded uncredentialed, and a
       cross-origin 302 under it is caught by exactly this comparison and nothing else. Two refusals, two
       populations, and each names the origin it landed on.
       IT IS A REPORT AND NOT AN ASSERT, because a server choosing a cross-origin 302 is the SERVER's doing and
       not this zone's invariant broken. It is also EVIDENCE: the person's own navigation landed at the address
       the browser reported, so a second GET of that same address arriving somewhere else is a server treating
       this request differently — the single-use-token / bot-challenge class — and the honest answer is to say
       so rather than to analyse whatever came back. The comparison is between two ADDRESSES and decides no
       principal, the same way the seed's own admission check does. */
    const _landed = originOf(loaded.url);
    const _asked = originOf(msg.sourceUrl);
    const unavailable = loaded.unavailable !== null ? loaded.unavailable
                      : (_landed === "" || _landed !== _asked)
                        ? { kind: "network", detail: "cross-origin-redirect:" + (_landed || "unparseable") }
                      : loaded.bytes.length === 0 ? { kind: "empty" } : null;
    /* THE REPORT GOES BACK THE INSTANT THE LOAD SETTLES, not when the run ends. What a reader asks on opening
       the popup over an empty panel is "did this fail, or is there nothing here yet", and the analysis has
       not started — so a record written only at finalize would leave every RUNNING page indistinguishable
       from one nothing was ever said about. This is the edge that used to be the content script's two message
       types, moved to the zone that now knows the answer; `onFrontierAdvance` beside it is the same shape. */
    DCHECK(typeof self.onNavigationOutcome === "function",
           "the trusted zone has no onNavigationOutcome to record this navigation's outcome against — it is " +
           "the ONLY writer of a document's page-source record now that the load happens here, so without it " +
           "a page whose seed could not be loaded is a silence in the only zone that renders one");
    if (unavailable !== null) {
      self.onNavigationOutcome(msg.documentId, Object.assign({ state: "unavailable" }, unavailable));
      /* NO ENGINE RUNS, so there is no result document to expect — the one analysis that legitimately has
         none. The REASON is not lost by sharing this outcome with an empty document: it is on the page-source
         record above, which is the field that exists to carry it, and a seeded document that DID load is
         never empty (that is the rule immediately above). */
      return { success: true, result: linesToAnalysis([], msg, "nothing-to-run", null) };
    }
    self.onNavigationOutcome(msg.documentId, { state: "delivered" });
    /* AND THE DOCUMENT IS AT THE ADDRESS THE RESPONSE CAME FROM. HTML §7.4.5 determines the loaded Document's
       URL — and therefore its origin, its §8.1.3.2 "Environment settings objects" API base URL (§4.4 stood
       here and is "Grouping content") and the frontier key its residue parks under —
       over the RESPONSE's URL, not over the address that was merely requested. This is the same rule
       `fetchedDocument` has always applied to a child navigable; the root document being the one exception
       was the second-path defect one level down. It also deletes a hybrid no browser produces: the old
       transport handed the engine the bytes served at the navigation URL under a `location` that pushState
       had already moved to a client route. */
    msg.sourceUrl = loaded.url;
    msg.responseHeaders = loaded.headers;
    /* AND THE WORD THE LOAD ABOVE WAS PERFORMED UNDER, WRITTEN ONTO THE RECORD RATHER THAN RE-DECIDED. The
       paragraph over `navigationLoad` argues `observed` in full — the address is the one the browser ACTUALLY
       NAVIGATED TO — and this run persists (`msg.persist`), so the residue it parks is re-fetched later from
       whatever this states. Assigning the same constant the load was given is what keeps the stored grade and
       the performed load one fact: a second literal here could disagree with the one above it. */
    msg.provenance = PROVENANCE_OBSERVED;
    /* THE WAITER CARRIES BOTH SETTLERS. A Clear must be able to tell a document that was never seated that its
       analysis is not coming, and "cleared" is the exact error _dispatchDocument reads to abandon its tail without
       recording a page-level failure — resolving it with a plausible empty result instead would report the
       wiped page as analysed and clean.
       `code` IS GONE FROM THIS PATH RATHER THAN DEFAULTED THROUGH IT: `msg.code || ""` stood here and no
       producer that reaches this entry has ever written one (the two that do — a peer engine's child document
       and a rehydrated cold recipe — build their engine directly), so it was a read of a field nobody writes
       with a `||` as the reason it never crashed. The empty string is passed explicitly at the one call site. */
    const result = await new Promise((resolve, reject) => { _waiting.push({ html: loaded.bytes, msg, persist, resolve, reject }); _hostKick(); });
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

/* ─── THE DOOR `engine/level1.mjs` DRIVES THE LEVEL-1 ORDER THROUGH ──────────────────────────────────────
   Node-only, and never present in a browser realm: `module` is undefined in `ast-worker.html`, so what this
   guard opens exists for exactly one caller and adds nothing to what ships.
   IT IS NOT A CONVENIENCE, AND THE LINE THAT USED TO STAND HERE SAID IT WAS — "for deterministic unit tests
   of ordering/eviction/non-blocking-fetch", an affordance no file in this tree had ever taken up. §Testing's
   subject is the measurement: Level-2's census RIDES the result document because the engine composes it, and
   Level-1's cannot — this order is `engineWeight` per hot instance against `frontierWeight` per waiting
   address and cold row, and no engine can see another engine. So the Level-1 order is the one part of this
   scheduler whose only possible reader is a driver in this zone, and both defects found in it this session
   (a rank frozen at the constant 1.0, and a weight silently deleted from the order) were invisible to every
   gate there is. This is what makes them askable.
   WHAT IS EXPORTED IS THE ORDER, NEVER THE STATE. `_bestCandidate` takes its whole population as an argument
   and `_candPopulation` is the one composer of it out of this module's privates, so a driver hands its own
   population and the privates stay private — an export of `_pool`/`_waiting`/`_seeds` would hand a driver the
   fields and leave the WALK exactly as unaskable as it was.
   THE DECLARATIONS TRAVEL WITH IT because the census's contract is stated in them: `CAND_KINDS` is what the
   accounting sums over, `CAND_SPREAD` is which readings a non-empty ranked set must carry, and `_candCensus`
   is the one statement of which rows a walk that RAN produces. A driver that kept its own copy of those lists
   would be the second hand-maintained copy §Testing names as the banned shape, and it would go stale in the
   direction that matters — silently agreeing with a walk that had dropped a row. */
if (typeof module !== "undefined" && module.exports)
  module.exports = { hostSchedule, engineWeight, frontierWeight, frontierReward,
                     _bestCandidate, _candPopulation, _level1Record, _candCensus, CAND_KINDS, CAND_SPREAD };
