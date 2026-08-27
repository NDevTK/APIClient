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
   emptiness is no engine having finalized / no engine having aborted. crashBanner counts every abort path, and
   rendererPoolProbe reads both (the log also answers the popup's GET_ENGINE_RUNS). The log holds page
   ADDRESSES, so hostClear empties it and the crash count stays — see the Clear path for why those two differ.
   ONE ROW PER RUN, AND A RUN IS AN INSTANCE, NOT A REPORT ABOUT ONE. Every record `linesToAnalysis` builds was
   pushed here, and streamPartial builds one every 750 ms — so ONE engine on ONE page wrote a new row every
   cadence, each carrying that instant's accumulated totals and each indistinguishable from a run that had
   ENDED. Measured on a local fixture: 77 rows for one page, of which the popup rendered the last 8 as eight
   separate runs of one URL that had each learned ~198 endpoints. The counts were real and the thing they were
   counts OF did not exist. A run therefore owns exactly one row (`eng._log`), a partial WRITES that row rather
   than appending beside it, and `run` on the record says which of the three states the row is reporting — so
   a snapshot of a run in progress can never be read as a run that finished. */
self._engineLog = [];
self._engineCrashOccurred = 0;

/* A URL with an opaque HOLE is not concretely fetchable (used to gate reply/chunk fetch). Asked in
   lib/callsite-url.js, in endpoint.c's own hole grammar: the `/\{[a-z]*\}/` that stood here answered NO for
   every shape this engine emits (a digit, a dot or a paren in the name defeated it), so the gate matched `{}`
   and `{id}` and let `{orphan3.arg0.replace()}` through to safeFetch as a percent-encoded literal path against
   the page's own origin — a request for an address the page's code never determined.
   Endpoint IDENTITY (hole-normalization, shape/concrete collapse) is the ENGINE's now — the host's
   normHoles/SEG_HOLE/pathSegs/mergeCallsites/dedupShapeConcrete were DELETED. */
const hasHole = (s) => astAddressHasHole(s || "");

/* Map the engine's ONE structured `@RESULT <json>` line -> the analysis object the brain consumes.
   The ENGINE builds + DEDUPS the whole result (endpoints/params/headers/body, @S sinks, page errors,
   park recipes) and JSON.stringifies it; the host does ONE JSON.parse and relays — NO per-line
   @H/@Q/@HDR/@BODY parsing, NO host identity/dedup (all DELETED, the engine owns it like a browser).
   @E lines (host-side protocol errors) are still surfaced so a zero-result never fails silently.

   `outcome` IS THE CONTRACT, STATED BY THE CALLER, AND IT TRAVELS ON THE RESULT AS `_run`. It used to be the
   boolean `expectResult` — "must there be a result document?" — and one bit was answering two different
   questions, which is how both of them came out wrong. A finalize and a partial snapshot both READ the one
   result document the engine builds, so its absence there is a broken engine↔JS contract; a CRASH record and
   a document with nothing to run have none by construction, and those two are NOT the same thing. Inferring
   the label from "is there a document" made a page with no scripts log as an ENGINE CRASH, and made a crash
   that happened to leave an unconsumed partial behind log as a COMPLETE run with counters. So the caller says
   which of the four it is, once, and every consumer downstream reads the same word:

     "partial"        — a mid-run snapshot (qjs_emit_partial). A document is required. The findings are REAL
                        observations of the running page; the run has not ended.
     "complete"       — the run ended and the engine answered with its result document. A document is required.
     "crashed"        — the instance aborted. There is usually no document; there IS one when the abort landed
                        after a partial was printed and before this zone consumed it, and that document is
                        still result.c's composition and is asserted like any other.
     "nothing-to-run" — no HTML and no code, so no engine ran. No document, and NOT a crash.

   WHAT A CRASH INVALIDATES IS THE RUN, NOT THE OBSERVATIONS. The findings this function carries were composed
   by the engine BEFORE the abort, out of an already-deduped record, and they are already in the cumulative
   moat (streamPartial merges every snapshot as it takes it — see `_engineCrashed`'s deletion at engineFinalize
   for what the old "we discard them all" comment was claiming while that ran). Deleting them here does not
   un-observe them; it only hides them from the DOCUMENT that learned them, which is a false clean bill on the
   one surface a user reads. What the crash does invalidate is every claim of COMPLETENESS: the cost counters
   (which is why the log record still carries none), the park residue, and this run's right to overwrite the
   cross-session frontier entry. Those are gated on `_run`, at their own sites. */
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
  /* THE COST COUNTERS ARE ONE FIELD, because result.c writes them in ONE snprintf — there is no arm in
     which five arrive and three do not. So the contract is all-of-them, and asserting a subset of a set that
     is emitted atomically is not a weaker check, it is a check on the wrong thing: it passes for exactly the
     document shapes it was meant to reject. `_switches` alone was asserted here while the other six were read
     below with a `|| 0` beside each, which is the file's own recorded defect (see `_orphans` at the return)
     re-spelt: a name the engine stops writing becomes a zero the diagnostic reports forever. The seam's live
     table and its cumulative history are two of them and NOT one field named twice — result.c says why.
     AND THE LAST FOUR ARE NOT COSTS, WHICH IS WHY THEY ARE HERE RATHER THAN MERELY WELCOME. They are what
     makes an EMPTY securitySinks array readable: whether this run ever acquired attacker input at all, whether
     a code-execution sink ever ran, whether anything tainted arrived at one, and whether an arrival was
     DECLINED because the check standing on it was unforgeable. Those are four different pages and one empty
     array was the evidence for each. A missing one here is the whole split gone, so it is asserted with the
     rest and never defaulted — the same rule that put the other eight in this loop.
     AND THE SIX CROSS-INSTANCE ONES ARE IN IT BECAUSE THE RULE ABOVE IS NOT A RULE ABOUT WHAT THIS ZONE
     HAPPENS TO READ. This loop asserted thirteen of the nineteen counters result.c writes in that one
     snprintf, and the six it left out — the routed-delivery pair and §9.3.3 step 8's four task ends — are
     precisely the newest, i.e. the ones the composition most recently changed to add. That is the "check on
     the wrong thing" this comment already names, arriving by omission instead of by design: a subset that
     tracks the fields somebody thought of asserts the document as it was, and the next field added to
     result.c joins the unchecked half by default. `engine/route.mjs` reads all six and asserts them, so
     nothing here is asserting a name with no writer; what this adds is that the EXTENSION — the consumer
     that runs in production, where route.mjs never runs — notices the same break. */
  for (const k of ["_switches", "_flows", "_candidates", "_jobsQueued", "_jobsRun", "_unitsDone",
                   "_worldSegmentsHeld", "_worldSegmentsMade", "_worldSegmentsForked",
                   "_routedDelivered", "_routedRefused", "_routedTasksFired",
                   "_routedTasksTargetOrigin", "_routedTasksTargetGone", "_routedTasksThrew",
                   "_sourceReads", "_sinkReached", "_sinkTainted", "_sinkSuppressed"]) {
    DCHECK(typeof r[k] === "number",
           "the engine's result document carries no " + k + " count — solver/result.c emits every cost " +
           "counter and the whole @S arrival census in one snprintf, so a missing one is that composition " +
           "having changed under this seam, not a run that happened not to do the thing. They are the only " +
           "OBSERVABLE that the single BFS context-switches, forks and pumps jobs rather than running its " +
           "flows FIFO, and the only thing that tells an empty finding set from a run that never looked");
  }
  DCHECK(Array.isArray(r._park),
         "the engine's result document carries no _park array — that is the PARKED RESIDUE (solver/cold.h), " +
         "the recipes this zone writes to IndexedDB and hands back to qjs_begin next session. An absent one " +
         "reads exactly like a fully-explored document, so every flow the engine paged out would be dropped " +
         "here and the cross-session frontier would silently restart from the boot flow on every visit");
}
const RUN_OUTCOMES = ["partial", "complete", "crashed", "nothing-to-run"];
/* THE ROW THIS RUN OWNS. `eng` is the run's identity — one instance, one row — and it is absent for the two
   records that belong to no instance: a document with nothing to run (which logs nothing at all) and a boot
   that aborted before the reservation had an instance, whose record is built PER WAITING CALLER and so cannot
   share a row with the others. A record that has a run writes that run's row IN PLACE, keeping the array
   position the run STARTED at (which is the order a list of runs is read in) and keeping the row correct under
   the log's own truncation: a row that has already been shifted out is no longer displayed and writing to it
   is a no-op, where an index would have addressed somebody else's run.
   THE FIELDS ARE REPLACED, NOT MERGED. A crashed run carries no counters at all — bridge's own rule, because
   seven zeroes read as a run that explored nothing — so a terminal crash record landing on a row a partial had
   filled must LEAVE it with no counters, and `Object.assign` alone would have left the last snapshot's numbers
   underneath a row labelled crashed. */
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
function linesToAnalysis(lines, msg, outcome, eng) {
  DCHECK(RUN_OUTCOMES.indexOf(outcome) >= 0,
         "a run outcome this seam does not speak: `" + outcome + "` — every consumer of an analysis branches " +
         "on `_run`, so a word none of them knows is a run whose completeness nothing can judge");
  let result = null;
  const extraErrors = [];
  let resumed = 0;
  /* THE CAUSE OF A CRASH, READ OFF THE SAME LINES EVERY OTHER FACT ABOUT THE RUN IS READ FROM. Both producers
     of a crashed record put an `@E {"phase":"engine-crash",…,"err":…}` line in `lines` before calling here —
     engineCrash for a live instance (with the ROOT @WHY appended) and crashRecord for a boot that never got
     one — so this is not a new channel, it is the existing one being read where the RUN LOG is composed. */
  let crashErr = "";
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
      /* A `@E` LINE IS TEXT UNTIL SOMEBODY PARSES IT, AND EXACTLY ONE SHAPE OF IT IS THE RUN'S OWN DEATH.
         Parsed here rather than string-matched: the phase is the field the two producers write, so a shape
         that stops carrying it stops being read as a crash instead of matching a substring of a page's own
         error text. A non-JSON `@E` is the engine's other diagnostics, which have their own entry above. */
      try {
        const o = JSON.parse(ln.slice(3));
        if (o && o.phase === "engine-crash" && typeof o.err === "string") crashErr = o.err;
      } catch (_) { /* not the crash line; it is already recorded as an engine error above */ }
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
  const mustHaveDocument = (outcome === "partial" || outcome === "complete");
  DCHECK(!mustHaveDocument || result !== null,
         "an engine session produced no @RESULT document at all — the one result document is what every " +
         "finding for this page travels in, and reporting its absence as an empty page is a false clean bill");
  /* AND A DOCUMENT THAT IS THERE IS CHECKED WHETHER OR NOT ONE WAS REQUIRED. This asked `expectResult &&
     result`, so the one document that arrives on a path that did not demand it — a crashed instance's
     unconsumed partial — was the only one relayed unasserted, and its fields were then read below. The
     question "must there be one" is the caller's; the question "is this one result.c's composition" is the
     document's own and has one answer. */
  if (result) assertResultDocument(result);
  /* THE TWO CASES ARE WRITTEN AS TWO CASES. `result = result || {}` merged them into one, and everything after
     it then had to read a document that might not be there — which is where each `|| 0` and `|| []` below came
     from, one per field, each individually reasonable and collectively the defaulting the rule forbids. With
     the absent case handled ONCE, on its own arm, every read on the present arm is a read off a document
     `assertResultDocument` has already checked field for field, so a default beside it can only ever hide that
     assert being wrong. There are none left. */
  const m = (outcome !== "crashed" && result)
    /* THE SCHEDULER'S OWN COUNTERS, so fairness/deep-preemption is OBSERVABLE (a real signal that the single
       BFS context-switches rather than running FIFO) — and they are the fields solver/result.c ACTUALLY emits.
       NOT wrapped in a swallowing try/catch: every value here is a number off a document asserted above. */
    ? { run: outcome,
        switches: result._switches, flows: result._flows, candidates: result._candidates,
        jobsQueued: result._jobsQueued, jobsRun: result._jobsRun,
        /* THE PRECONDITION FOR RUNNING A JOB, beside the count of jobs run, because one of those numbers alone
           has two opposite readings — see solver/engine.c's g_units_done. A run whose reactions never fire and
           a run whose flows never reach the between-units boundary at all need opposite fixes. */
        unitsDone: result._unitsDone,
        worldSegmentsHeld: result._worldSegmentsHeld, worldSegmentsMade: result._worldSegmentsMade,
        worldSegmentsForked: result._worldSegmentsForked,
        /* …AND WHY THE SECURITY SURFACE IS THE SIZE IT IS. Every counter above is about how much work the run
           did; these four are about what the work MET, and they are the only thing that distinguishes an
           analysed page with nothing to find from a page nobody got to. Forwarded rather than left in the
           result document because the probe watching this seam cannot reach into the moat, and "no findings"
           is the one answer it must never take at face value. */
        sourceReads: result._sourceReads, sinkReached: result._sinkReached,
        sinkTainted: result._sinkTainted, sinkSuppressed: result._sinkSuppressed,
        /* WHAT THE RUN ACTUALLY LEARNED, beside what it cost. The counters above say the BFS switched, forked
           and pumped jobs; these two say it produced something, which is the only question a probe watching an
           engine that now lives behind a frame boundary can ask without reaching into the moat. Both arrays are
           asserted field-for-field above, so there is nothing to default; the crash arm carries neither,
           because a run with no result document has no surface to have found and a zero there would read as a
           page that was analysed and was clean. */
        endpoints: result.fetchCallSites.length, sinks: result.securitySinks.length,
        park: result._park.length, resumed: resumed, url: (msg && msg.sourceUrl) || "" }
    /* A CRASHED RUN REPORTS NO COUNTERS, and the honest report of that is the ABSENCE, not seven zeroes.
       Zeroes here read as "the engine ran and did nothing" — indistinguishable in the log from a real run that
       explored nothing, which is a finding. `run` is the field that keeps the two apart, and the counters
       are simply not present: nothing may compute a rate, a delta or a total out of a run that never reported
       one. IT IS `run` AND NOT `crashed:true`, AND THE BOOLEAN IS DELETED RATHER THAN KEPT BESIDE IT: the
       record now has to speak THREE states (a snapshot of a run still going, a run that ended, a run that
       died) and a flag that can only say one of them would have made the other two the same value — a partial
       reading in the log exactly as a completed run does, which is the defect this record is being fixed for.
       THE ARM IS CHOSEN BY THE OUTCOME AND NOT BY WHETHER A DOCUMENT ARRIVED, which is the same
       distinction one level down: a crash that left an unconsumed partial behind used to take the arm ABOVE
       and log as a complete run with a full set of counters, because the only question asked was "is there a
       document". Its findings still travel (they are on the returned analysis, labelled `_run:"crashed"`);
       what does not travel is a COST record for a run that did not finish. */
    /* …AND IT REPORTS ITS CAUSE, WHICH IS NOT A COUNTER AND IS THE ONE THING THE ROW OWED A READER. The rule
       one paragraph up is about COST counters: seven zeroes claim a run explored nothing, so a crashed run
       carries none. `err` claims nothing about how much was explored — it says WHY the run ended — and its
       absence here was the crash announcing itself in a place nobody reads while the record every consumer
       DOES read (the popup's GET_ENGINE_RUNS, testing/live-run.js) could say only the word "crashed". The
       banner has carried the ROOT @WHY since engineCrash was written; this is the same string, on the row.
       A crash whose cause lives only in a console the renderer does not tee is a crash that names no
       capability, which is the whole value a live site has. */
    : { run: outcome, resumed: resumed, url: (msg && msg.sourceUrl) || "", err: crashErr };
  /* AND THE CAUSE IS ASSERTED, NOT HOPED FOR. There are exactly two producers of a crashed record and each
     one writes the `engine-crash` line into `lines` before it calls here, so an empty `err` on this arm is
     that composition having changed under this seam — a third crash path, or a producer that stopped writing
     the line — and the symptom would be a row that says "crashed" and nothing else, which is the state this
     field exists to end. It is not defaulted for the same reason no other field on this record is. */
  DCHECK(outcome !== "crashed" || (typeof crashErr === "string" && crashErr !== ""),
         "a crashed run reached the run log with no `engine-crash` line among its output — every crash path " +
         "writes one (engineCrash appends the ROOT @WHY to it, crashRecord constructs it), so a crash with " +
         "no cause here is a producer that stopped announcing itself and a row a reader cannot act on");
  // A per-run LOG (not a single overwritten global): concurrent cold-kick engines each report here, so the
  // full park->persist->rehydrate->resume SEQUENCE across all engines is observable, not just the last one.
  /* AND THERE IS NO `self._engineMeta` BESIDE IT. It held the LAST record — a page URL and its counters — in a
     global nothing has ever read: not this file, not the popup's GET_ENGINE_RUNS, not rendererPoolProbe, not
     testing/. A single overwritten global is the shape the line above says the log replaced, and it survived
     underneath it, holding one page's address for the life of the offscreen with no surface to show it on. */
  /* THE ARRAY IS DECLARED AT LOAD (top of this file), not created on first use: `self._engineLog = self._engineLog || []`
     is the defaulting shape, and here it defaulted the one thing a reader wants to distinguish — an empty log
     because nothing has run from an absent log because this file never loaded. */
  /* AND A DOCUMENT WITH NOTHING TO RUN PRODUCES NO RECORD, because no engine ran. It used to produce a
     `crashed:true` row — the popup rendered "the engine crashed" for a page whose only fault was carrying no
     script — which is the mirror of the defect this seam is being fixed for: a false statement about a run
     in the one place a reader looks for what the runs did. An absent row and a row of zeroes are different
     facts, and so are an absent row and a crash row. */
  if (outcome !== "nothing-to-run") engineLogWrite(eng, m);
  /* A RUN WITH NO ENGINE DOCUMENT SAYS SO BY NOT CARRYING ONE, AND THAT IS THE WHOLE OF WHAT IT MAY SAY.
     `result || { fetchCallSites: [], securitySinks: [], pageErrors: [], _park: [] }` stood here — the defect
     §Architecture opens its list with, grown into its most developed form. The substitute literal had reached
     the document's FULL shape, so it no longer read as a default at all: it fabricated a well-formed result
     in which the engine looked at the page and learned nothing, and "no document arrived" and "the page was
     analysed and is clean" became the same four empty arrays travelling under the same field names. Calling
     them "the host's own empties" (which the comment that stood here did) does not make them the host's to
     state — `fetchCallSites` means THE ENDPOINTS THE ENGINE LEARNED, and this zone does not know that number
     for a page whose engine never answered. It is not a host-side empty, it is a measurement nobody made.
     SO THE THREE ENGINE-OWNED FIELDS ARE PRESENT-OR-ABSENT, NEVER EMPTY-AS-A-SUBSTITUTE. Their PRESENCE is
     the positive statement "an @RESULT document arrived and assertResultDocument checked it field for
     field", and every consumer reads that presence rather than a length: lib/merge.js's two passes,
     offscreen-brain's incremental merge and its terminal replace-or-keep, and the frontier write below.
     `resolverErrors` is NOT one of them and stays on both arms — it is the HOST's record of what went wrong,
     of which the engine's `pageErrors` are one contributor, and on this arm it carries the `@E engine-crash`
     row that says why there is no document. `_run` is the other half of the same statement: WHICH absence
     this is (an instance that died, or a document with nothing to run). */
  /* THIS DOCUMENT IS EXACTLY WHAT THE BRAIN READS, AND IT USED TO CARRY FIFTEEN MORE FIELDS THAT NOTHING DID.
     A "sibling fields the brain reads unconditionally, present + empty so it never throws" block held
     protoEnums/protoFieldMaps/dangerousPatterns/esmImportUrls/inRunModuleUrls/domEndpoints/sourceMapTypes/
     sourceMapsByUrl/traceMapsByUrl/valueConstraints/sourceMapUrl/sourceMap, beside `chunkUrls`, `_replyWant`
     and `_switches` — every one of them a constant the ENGINE has never written. The merge passes that read
     them were deleted (lib/merge.js records four of them by name), and the writers outlived the readers, which
     is the §FIELD-A-CONSUMER-DEFAULTS defect with the arrow reversed: `dangerousPatterns` crossed two
     boundaries into `tab._securityFindings`, and a `.length` of 0 there is indistinguishable from "no
     dangerous patterns found". A constant `[]` is not a measurement of a page, so it is not shipped as one.
     (testing/test-spec.js's two `domEndpoints` assertions fail exactly as they did — they are the record
     that the DOM-attribute scan is an ENGINE capability nobody has built. The two testing drivers that read
     dangerousPatterns/valueConstraints/protoEnums/protoFieldMaps/sourceMapUrl off the analysis with `|| []`
     are gone with the batch pipeline they belonged to: their input was a report field no writer in this tree
     produced, so the whole analyzer was composed of absences, and this paragraph's own naming of them was the
     last thing pointing a reader at it.) */
  const analysis = {
    /* THE ENGINE'S OWN PAGE ERRORS, WHICH IT CALLS `pageErrors`. This line read `resolverErrors` — a name
       nothing on the engine side has ever written — so every error the engine recorded while running the page
       was dropped here, and the `|| []` beside it is precisely what made the drop invisible: the field the
       brain reads existed, held the host's own errors, and looked complete.
       A ROW IS {context, message}, TWO NON-EMPTY STRINGS, AND NOTHING ELSE. It carried `snippet` and
       `replyExample` as constant `null`s: offscreen-brain dropped `replyExample` on the way through, no
       renderer ever read either, and the engine has no source-text or reply to state for a page error in the
       first place (result.c's pageErrors are strings). Three comments — here, in lib/serialize.js and in
       popup.js — named `snippet` as part of the contract the popup reads; it was read nowhere. */
    resolverErrors: (result ? result.pageErrors.map((e) => ({ context: "page", message: String(e) })) : []).concat(extraErrors),
    /* NO probeResults ON THIS SEAM. The engine issues no request, so it receives no rejection and has no
       error-derived schema to relay; the record the Send panel reads is written by the two systems that DO
       probe — lib/req2proto.js (driven by lib/discovery-probe.js and lib/response-decode.js) — straight into
       `globalStore.probeResults`, which never crossed this boundary. */
    sourceUrl: (msg && msg.sourceUrl) || "",
    /* WHAT THIS ANALYSIS IS A RECORD OF, CARRIED WITH IT. Every consumer of an analysis has to be able to tell
       a snapshot of a running page from a finished run, and both of those from a run that died — and until
       this field there was nothing on the object that said so. `_engineCrashed` was set on the crash arm and
       READ NOWHERE, which is the §FIELD-A-CONSUMER-DEFAULTS defect pointed the other way: the writer looked
       live, so the discard it announced looked enforced. Written on EVERY arm, never absent, so a consumer
       asserts it rather than defaulting a missing one to "fine". */
    _run: outcome,
  };
  /* THE THREE FIELDS THAT ARE THE ENGINE'S TO STATE, ADDED ONLY WHERE THE ENGINE STATED THEM. Read straight
     off `result` rather than through a second name, so there is no object in this function that a document
     could be defaulted INTO — the shape that let the substitute literal survive four hardenings of the
     surrounding code. */
  if (result) {
    analysis.fetchCallSites = result.fetchCallSites;
    analysis.securitySinks = result.securitySinks;
    analysis._park = result._park;
  }
  return analysis;
}
/* THE ONE QUESTION EVERY CONSUMER OF AN ANALYSIS ASKS BEFORE IT READS A FINDING ARRAY, asked in one place so
   the three fields cannot drift apart at four call sites. It is a POSITIVE read of an absence, not a guard
   against one: a true answer means an @RESULT document arrived and `assertResultDocument` checked it, and a
   false answer means this run produced no document at all and `_run` says which absence it is. The DCHECK is
   what keeps the two states from silently becoming three — a record carrying one of the trio and not the
   others is `linesToAnalysis` broken, and would read here as a document. */
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
   NOT a host-side regex.
   THE KEY IS THE DOCUMENT'S ADDRESS AND ITS PROGRAM, AND THE SENTENCE THAT STOOD HERE WAS FALSE ABOUT THIS
   TREE. It said the key "is only a RELEVANCE grouping for which parked recipes to pull on resume (recipes
   self-identify by function SOURCE hash, so a coarse key is sound)". No recipe carries a source hash: the
   grammar solver/cold.c writes is `f<chain-id>,<reward>` — a POSITIONAL id into the decision chain that
   document's own scripts built, in the order they asked their questions — and solver/cold.c's own DFAIL asks
   for `JS_OrphanHash` to be REBUILT (deleted at 1dd6bbe4) for a record kind that does not exist yet. A source
   hash could not make a coarse key sound even if it existed, because it locates a FUNCTION and a recipe is a
   PATH: there is no hash under which "arm 3 of question 7" means the same thing in a different program.
   WHAT THE COARSE KEY COST, MEASURED ON REAL ROUTE SETS: `document_bundle_id` hashes the external <script src>
   set, and an SPA ships one bundle to every route — app.netlify.com `/`, `/login`, `/signup` and `/teams` all
   hash to `1qi62vn`; app.asana.com `/`, `/-/login` and `/0/home` all to `1mksgsl`. Under `origin|bundle` those
   are ONE entry, and frontierPut REPLACES, so route B's residue overwrote route A's — and worse than losing
   it: solver/engine.c seeds a session from the recipes OR from a boot flow, never both ("they are ALTERNATIVES"),
   so route B opened holding route A's decision vectors and NO boot flow. Route B's own document was never
   explored from script 0, and route A's arms were replayed positionally against route B's program.
   SO THE KEY NAMES THE DOCUMENT THE RECIPES REPLAY IN: its ADDRESS (DOM §4.5 "Interface Document" — the
   document's URL is the browser's own name for it, it is what the engine derives the principal from and what
   every relative URL resolves against, and it is what tells one SPA route from another) plus the PROGRAM the
   engine identified. The bundle half is retained for exactly what it always did: a redeploy changes a
   content-hashed src, so the key changes and the stale residue is invalidated rather than replayed against
   new code. The origin half is subsumed — an address carries its origin — and originOf is still asserted
   below, as the statement that this zone can serialize a principal from the address the engine accepted.
   WHAT THIS COSTS, STATED RATHER THAN HIDDEN: a document whose ADDRESS is volatile (a per-request id in the
   query) now keys a new entry per visit and never resumes. That is the honest reading — a residue for a
   document at an address nobody will visit again is not resumable — and it is strictly better than the
   alternative it replaces, which was to resume it inside a DIFFERENT document. */
/* Cross-session flow FRONTIER (IndexedDB): the learned surface (globalStore) already persists; this
   persists the UNFINISHED frontier as compact replay recipes, keyed by the document's ADDRESS plus the
   bundle hash (a changed bundle invalidates a residue written against other code). A parked frontier
   resumes next visit/session -> ONE continuous attention across sessions, extracting more breadth each
   time until fully explored. */
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
    /* THE RANKING VIEW MOVES WITH THE STORE, ON THE ONE DOOR THAT WRITES IT AND ONLY ONCE THE WRITE LANDED —
       so a refused write leaves a row claiming a residue the store does not hold, which is the one way this
       projection could start lying. It is skipped entirely when the index has not been built yet: an unbuilt
       index is rebuilt from the store, which by then contains this. */
    if (_frontierIndexBuilt) { if (entry && entry.recipes) _frontierIndex.set(key, frontierRow(entry)); else _frontierIndex.delete(key); }
  } catch (e) { RETHROW_FATAL(e); frontierFail("write", e); }
}
async function frontierAll() {
  try {
    const db = await idbOpen();
    return await new Promise((res, rej) => { const t = db.transaction("frontier").objectStore("frontier").getAll(); t.onsuccess = () => res(t.result || []); t.onerror = () => rej(t.error); });
  } catch (e) { RETHROW_FATAL(e); frontierFail("scan", e); return []; }
}
/* THE LEVEL-1 WEIGHT OF A WORK ITEM THAT IS NOT RESIDENT — a parked frontier, or a document waiting for an
   instance. It shares the engine's WFQ POLICY (rank by value + an exploration bonus, never drop a work item)
   but NOT the engine's exact formula: flow_weight is `val + optimism − cpu-aging`; a work item with no
   instance has no live CPU to age by, so its expected FUTURE productivity is estimated by emit-per-VISIT.
   IT IS IN THE ENGINE'S OWN CURRENCY, AND THE CONSTANT THAT MADE IT A SECOND SCALE IS DELETED. `1 + rate +
   bonus` stood here, and the `1 +` is exactly what stops two levels of one WFQ from being one order: an
   UNVISITED entry read 2.0 while an UNRUN FLOW — the same statement one level down, reward 0 plus the full
   optimism bonus — reads 1.0 (solver/flow.c: "a never-run flow carries the FULL optimism bonus, so its weight
   is its reward + 1.0"). Every cold entry therefore outranked every live engine whose top flow had aged below
   2.0, unconditionally and for no reason anybody chose. flow_credit_emit counts "ONE EMISSION IS ONE POINT"
   and `emit` counts findings the same way, so with the constant gone the two terms are the same quantity and
   the comparison the pool now makes — is a non-resident item worth more than the RAM a resident one holds —
   is a comparison rather than a coincidence of scales.
   A ZERO-VISIT ROW IS A POSITIVE STATEMENT, NOT A DEFAULT: it says this item has never been SERVED at this
   level, which is what a waiting document is, and its weight is then the full optimism bonus and nothing
   else. The duplicate JS scheduler lib/priority.js was DELETED. */
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
  const rate = row.visits ? row.emit / row.visits : 0;   // expected emit per admission — future productivity
  return rate + 1 / (row.visits + 1);                    // reward + optimism; no aging (nothing is burning CPU)
}
/* THE COLD TIER'S RANKING VIEW, WHICH IS NOT THE COLD TIER. The Level-1 order asks two numbers of a parked
   frontier and the store answers with a whole parked DOCUMENT — its bytes, its script inventory, its header
   list — so ranking by `getAll` deserialized every page this profile has ever parked in order to sort by
   `emit` and `visits`. That was affordable only while rehydration was gated on "nothing live is running";
   the gate is gone (it was a second admission policy beside the one WFQ), so the order is asked on every
   scheduler round and the content may not ride with it.
   IT IS A PROJECTION OF ONE STORE, NOT A SECOND REGISTRY, and the thing that makes that true is that this zone
   is the store's ONLY writer: frontierPut is the one door and it updates this on its way through, so a row
   here and an entry there cannot disagree. It is built once per zone lifetime from the store itself, which is
   the only moment the two could differ. */
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
  return { key: e.key, sourceUrl: e.sourceUrl, emit: e.emit, visits: e.visits };
}
async function frontierIndex() {
  if (_frontierIndexBuilt) return _frontierIndex;
  for (const e of await frontierAll()) {
    /* THE STORE'S OWN INVARIANT, ASSERTED WHERE IT IS READ BACK. frontierPut DELETES an entry with no recipes
       rather than storing one, so every row in this store has them; the `e && e.recipes` filter that stood at
       the rehydration site read that invariant as an option and would have skipped a residue silently. */
    DCHECK(e && typeof e.recipes === "string" && e.recipes !== "",
           "the cross-session frontier holds an entry with no parked recipes — frontierPut deletes rather than " +
           "storing one, so this is a residue whose flows were dropped between the park and the store");
    _frontierIndex.set(e.key, frontierRow(e));
  }
  _frontierIndexBuilt = true;
  return _frontierIndex;
}
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
  return !_atRamFloor();
}
/* THE FLOOR ITSELF, WHICH IS A DIFFERENT FACT FROM "MAY ANOTHER INSTANCE BE BUILT" AND IS NOW ASKED SEPARATELY.
   A reservation in flight blocks admission and is NOT a reason to evict a running document — it is a reason to
   wait for the number it has not reported yet. Written once because both askers must mean the same floor: the
   sum of what the live instances last reported, against the working-set budget.
   AND IT IS NO LONGER A REFUSAL. A wasm Memory never shrinks, so this predicate going true once is permanent
   for the instances that made it true — which is exactly why answering it by declining every admission
   deadlocked the whole extension. What answers it now is `_hostOps.evictee`: the one order decides which
   resident engine gives its memory up, and the residue goes to the cold tier where it keeps its place. */
function _atRamFloor() {
  return _residentBytes() >= HOT_RAM_BUDGET;
}
/* THE ONE LEVEL-1 CANDIDATE ORDER — the highest-value work item that is NOT resident, whichever kind it is.
   §scheduler: "Cold-tail resume is the SAME admission step (not a separate loop)". A waiting document and a
   parked frontier are the same kind of thing to this order — work with no instance — and the ONE thing that
   used to separate them was a liveness gate (`!_waiting.length && !_pool.some(e => !e._cold)`), i.e. "a cold
   item competes only when nothing live exists". In continuous browsing that is never true, so a flow parked
   last week could never compete at Level-1 regardless of its value, which is a SECOND admission policy beside
   the one WFQ and §THERE IS NO GRIND calls that the cardinal violation. It is deleted; value decides.
   A WAITING DOCUMENT RANKS AS UNSERVED, AND THAT IS COMPUTED RATHER THAN INVENTED. Its frontier key is
   `address|bundle` and the bundle half is the ENGINE's Lexbor <script> scan — this zone may not guess it (a
   host-side regex over the markup is exactly what §Architecture forbids), so the host does not yet know WHICH
   residue this visit will resume and may not attribute another key's history to it. What it does know is that
   this item has never been served at this level, which is the same statement solver/flow.c makes about a flow
   it has never run, and it carries the same weight: the full optimism bonus, 1.0. The residue is then picked
   up where it IS known — engineRoot's frontierGet, once the engine has answered.
   AN ADDRESS A LIVE DOCUMENT HOLDS IS NOT ALSO A COLD CANDIDATE. The parked entry and the open tab are ONE
   work item: the tab's engine resumes that residue itself, and rehydrating it beside the tab would replay one
   document's flows in two instances. The exclusion is by ADDRESS rather than by key precisely because the key
   is not yet known for the live half — and it is exact only because the address is now IN the key. */
/* THE PICK IS SYNCHRONOUS, AND THE INDEX IS WHY IT CAN BE. Its caller must not suspend between choosing a
   candidate and building its instance: a cluster can be rooted by a concurrent service round (hostNotice's
   create arm), so a pick made before a suspension and acted on after it would reach engineCreate's
   one-instance-per-cluster assert with a cluster that has just acquired one. The caller awaits frontierIndex()
   once and hands the map in; from there this reads only in-memory state. */
function _bestCandidate(idx) {
  DCHECK(idx instanceof Map,
         "the Level-1 pick was asked without the cold tier's ranking view — its caller awaits frontierIndex() " +
         "so that this pick and the instance it leads to are one uninterrupted turn, and a pick that fetched " +
         "its own would suspend in the middle of that");
  const live = new Set();
  for (const e of _pool) if (e.msg && e.msg.sourceUrl) live.add(e.msg.sourceUrl);
  let best = null;
  for (const job of _waiting) {
    /* A SUB-FRAME NEVER ROOTS A CLUSTER — its embedder names it (see admit), so it is not admissible and is
       therefore not a candidate. It keeps its place in `_waiting` and is answered by the instance that
       creates it; nothing here drops it. */
    if (job.msg.frameId && _isRealOrigin(job.msg.origin)) { live.add(job.msg.sourceUrl); continue; }
    live.add(job.msg.sourceUrl);
    const w = frontierWeight(FRONTIER_UNSERVED);
    if (!best || w > best.w) best = { kind: "doc", job, w };
  }
  for (const row of idx.values()) {
    if (live.has(row.sourceUrl)) continue;
    if (_pool.some((p) => p.fkey === row.key)) continue;
    const w = frontierWeight(row);
    if (!best || w > best.w) best = { kind: "cold", row, w };
  }
  return best;
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

/* HTML §7.1.4 "Cross-origin embedder policies"' EMBEDDER POLICY, AS THIS ZONE RELAYS IT — the ITEM of §7.1.7's
   policy container that travels beside the CSP list on every path a container takes through here.
   THIS ZONE DOES NOT INTERPRET IT AND MUST NOT. §7.1.4 names three token strings and the ENGINE is the only
   party that turns one back into a value; a relay that mapped them here would be a second reading of §7.1.4.1
   in a language with no enum, and the engine crashes on a token it does not know rather than defaulting. What
   this zone owes is that the four items reach the other side unchanged and that a caller cannot half-state one.
   AND ITS ABSENCE IS NOT AN EMPTY VALUE. The CSP list says "there is no container" with an empty SELF-ORIGIN;
   an embedder policy has no such spelling, because §7.1.7 gives every container one — so a caller with no
   creator states NEW_EMBEDDER_POLICY, the section's own initial value, in as many words. */
const NEW_EMBEDDER_POLICY = Object.freeze({ value: "unsafe-none", endpoint: "",
                                            reportOnlyValue: "unsafe-none", reportOnlyEndpoint: "" });

/* WHOLE OR NOT AT ALL, which is the same rule the CSP list's two halves are held to and for the same reason:
   §7.1.7's clone moves every item of a container, so half an item is a clone that arrives having silently
   replaced the creator's answer with the default. */
function embedderPolicyWhole(e) {
  return !!e && typeof e.value === "string" && e.value !== "" && typeof e.endpoint === "string" &&
         typeof e.reportOnlyValue === "string" && e.reportOnlyValue !== "" &&
         typeof e.reportOnlyEndpoint === "string";
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
function engineCreate(code, html, msg, persist, docName, topLevelUrl, cold, inherited, parentNavigable) {
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
  /* HTML §7.1.7 "Policy containers"' CLONE OF THE CREATOR'S, or `null` for a document with no creator. IT IS
     STATED BY EVERY CALL SITE AND NEVER DEFAULTED HERE, because `null` and "the caller forgot" are the same
     shape and only one of them is a fact: a create notice always carries a container (the creator has one even
     when it holds no policies), while a reported root document, a rehydrated recipe and §7.3.2.3's swap have
     no creator at all. A `?.` or a `|| {}` here would turn the second into the first silently, which is
     exactly how the CHILD's own address came to answer `'self'`. */
  DCHECK(inherited === null ||
         (!!inherited && typeof inherited.csp === "string" && typeof inherited.selfOrigin === "string" &&
          inherited.selfOrigin !== "" && embedderPolicyWhole(inherited.embedder)),
         "an instance was started with an inherited policy container that is neither `null` nor a WHOLE one — " +
         "CSP §2.2 makes a list a struct of policies AND a self-origin, and HTML §7.1.7 makes the container a " +
         "struct of that list AND §7.1.4's embedder policy, so a half of either is a clone that arrives unable " +
         "to resolve `'self'` against anything but this document's own address or claiming `unsafe-none` for a " +
         "creator that opted into cross-origin isolation");
  /* HTML §7.3.1.3 "Child navigables"' PARENT of the navigable this instance is rooted in, and it is a SEPARATE
     argument from the container above because it is a separate KIND of fact: §7.1.7's container is five
     policies and says nothing about a frame tree. §7.3.1.3 defines the term over the link — a navigable "is a
     child navigable", "which means that its parent is non-null" — so a caller that says nothing is not leaving
     a field blank, it is declaring a TOP-LEVEL TRAVERSABLE, and SECURITY.md makes the commonest child navigable
     there is (a cross-origin `<iframe>`) the ROOT of its own instance. `u` is the engine's own encoding for the
     absence and is what a document with no embedder states; anything else is the identity record the emitting
     engine wrote, relayed verbatim. THERE IS NO DEFAULT HERE for the reason there is none for the embedder
     policy: "the caller forgot" and "there is no parent" would be one value, and one of them is a frame
     reported as a page. */
  DCHECK(typeof parentNavigable === "string" && parentNavigable !== "",
         "an instance was started with no HTML §7.3.1.3 PARENT NAVIGABLE — every call site knows which of the " +
         "two answers applies (a create notice carries the identity its engine wrote; a reported root, a " +
         "rehydrated recipe and §7.3.2.3's swap have no embedder and state `u`), so an absent one is a caller " +
         "that skipped the question and a document that would present as a top-level page in the only " +
         "instance that holds it");
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
      await engineRoot(eng, code, html, msg, persist, docName, topLevelUrl, inherited, parentNavigable);
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
  /* THE WIPE GENERATION THIS INSTANCE OBSERVES UNDER, TAKEN BEFORE THE FIRST AWAIT AND CARRIED FOR THE WHOLE
     RUN. Everything an engine learns, it learns from the page it was rooted at — so the question a merge has to
     answer is not "was this snapshot taken before the Clear" but "was this INSTANCE running before the Clear",
     and the answer is the same for every advance it will ever produce. Taken once, here, it also cannot be
     taken at the wrong TIME: a value read at the merge would be the generation AFTER the wipe, which is the
     one reading that always agrees.
     THE TERMINAL PATH ALREADY HAD THIS AND THE INCREMENTAL ONE HAD NOTHING. `_dispatchDocument` captures the
     epoch before it dispatches and abandons its tail if it moved, so a Clear could not repopulate the store
     through an analysis that was in the engine when it ran; `onFrontierAdvance` — the 750 ms snapshot merge and
     the finalize merge of an instance with no live caller — wrote straight into globalStore and (since the
     merge learned to name its documents) into a DOCUMENT, with no such question asked anywhere. */
  DCHECK(typeof self.frontierEpoch === "function",
         "the trusted zone states no frontier epoch — it is what tells an advance from this instance apart " +
         "from one observed before the user emptied the store, and without it every finding this engine " +
         "produces would merge into whatever the store holds when it lands");
  /* `topDocId` IS A DIFFERENT FACT FROM `docId` AND THE TWO MAY NOT BE ONE FIELD. `docId` is the document this
     instance was ROOTED at and never changes: it is the reservation's identity, what the crash record and the
     cold recipe are written under, and what `hostHolderOf` answered from the instant the record existed.
     `topDocId` is which document is the ACTIVE document of this browsing-context group's top-level traversable
     RIGHT NOW, and a same-origin navigation in the tab changes it — HTML §7.4.6.1 "Updating the traversable"
     replaces the Document, keeps the navigable. Collapsing them would make a navigated tab report its findings
     under a document the browser replaced, and would make the reservation's identity move under `finish`. */
  const eng = { state: "booting", cluster, docId, topDocId: docId, joinedDocIds: [], msg,
                groupId: msg && msg.groupId,
                origin: (msg && msg.origin) || "", _cold: cold, _resolvers: [], _remoteAsked: new Set(),
                _epoch: self.frontierEpoch(), r: null, _readyP: null };
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
async function engineRoot(eng, code, html, msg, persist, docName, topLevelUrl, inherited, parentNavigable) {
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
  /* THE DOCUMENT CROSSES AS BYTES, ONE SHAPE, because `qjs_init` takes one thing: a byte sequence and its
     LENGTH. It used to cross as EITHER — content.js ships a SERIALIZED DOM (the renderer parsed
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
  /* A 0x00 IN THESE BYTES IS NOT ASSERTED AGAINST ANY MORE, AND THE ASSERT THAT STOOD HERE IS DELETED RATHER
     THAN WEAKENED. It read `_doc.indexOf(0) < 0` and named the fix — "Give qjs_init a LENGTH beside the
     pointer (qjs_provide now carries one)" — and that length exists: `qjs_init`/`qjs_join` take `(bytes,
     len)`, renderer.html's `bytes-pair-retained` placement puts both operands in linear memory, and the C
     entry DCHECKs the guard byte at `bytes[len]` so a length and a C read cannot disagree without crashing.
     The state it forbade is a state the standard defines: HTML §13.2.3.5 "Preprocessing the input stream" says
     "The handling of U+0000 NULL characters varies based on where the characters are found … They are either
     ignored or, for security reasons, replaced with a U+FFFD REPLACEMENT CHARACTER", and the tokenizer has a
     rule per state (§13.2.5.4 "Script data state" emits a U+FFFD; §13.2.5.1 "Data state" emits the character
     and §13.2.6.4.7 The "in body" insertion mode ignores it). Measured: three sites of a thirty-site mirror
     aborted on this defect, one of them on five NULs in its own markup.
     WHAT IS STILL OWED IS THE OTHER HALF OF THE OLD NOTE — HTML §13.2.3.2 "Determining the character encoding"
     — and it is a DIFFERENT capability rather than the rest of this one: the engine still decodes these bytes
     as UTF-8 instead of sniffing them. The length is that algorithm's precondition (sniffing is defined over a
     byte sequence, and until now there was none), not a piece of it. */
  /* AND THE ADDRESS IS ASSERTED RATHER THAN DEFAULTED TOO, for the reason the line below it already proves:
     `(msg && msg.sourceUrl) || ""` stood here, and `originOf("")` is `""` — a frontier key every unidentifiable
     document would share, so one page's parked residue would resume inside another's engine. The engine's own
     entry CHECKs that this address parses, so an empty one has always aborted; it aborted one call later, in
     the process that was handed the hole rather than in the zone that made it. */
  DCHECK(!!msg && typeof msg.sourceUrl === "string" && msg.sourceUrl !== "",
         "a document reached qjs_init with no address — §4.4's document address is what the engine derives " +
         "this document's ORIGIN from (§4.7's serialization, its own url.c) and what every relative URL the " +
         "bundle builds resolves against, so a document without one is analysed behind no principal at all");
  /* THE INHERITED CONTAINER, AS TWO WIRE FIELDS AND NOT AS A HEADER. It stands beside `_headers` because it is
     not part of the response: HTML §7.1.7's clone is what a Document created by a CREATOR gets, and the engine
     is what decides which of the two lists this Document is created with. The empty pair is the positive
     statement that there is no creator, which is what `null` means at this function's own boundary. */
  /* §7.1.4's ITEM RIDES WITH THE LIST. `null` here is a document with NO CREATOR — a root a content script
     reported, a rehydrated recipe, §7.3.2.3's swapped-to context — and §7.1.7 gives the container such a
     Document is created with "a new embedder policy", which NEW_EMBEDDER_POLICY spells rather than a `||`
     filling in a producer's silence. */
  const _initEp = inherited ? inherited.embedder : NEW_EMBEDDER_POLICY;
  /* AND HTML §7.3.1.3's PARENT NAVIGABLE, WHICH IS NOT PART OF THAT CONTAINER — it rides beside it because it
     is the other thing only this zone knows about a document it did not root: whether the navigable it is
     rooting is nested in one another instance holds. engineCreate asserts it; this line is a relay. */
  const _init = await rend.renderer.init(_doc, msg.sourceUrl, eng.docId, _headers, _tlu,
                                         inherited ? inherited.csp : "",
                                         inherited ? inherited.selfOrigin : "",
                                         _initEp.value, _initEp.endpoint,
                                         _initEp.reportOnlyValue, _initEp.reportOnlyEndpoint,
                                         parentNavigable);
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
  /* THE ADDRESS PARSES IN THIS ZONE TOO — asserted here even though the origin is no longer concatenated into
     the key, because it is the statement the key's ADDRESS half rests on. originOf answers "" for an address
     this zone cannot parse, and qjs_init has already CHECKed that the engine's own url.c parsed it, so an
     empty answer means the two parsers disagree about what document this is — under which the pool would key
     a residue by a string one of them does not consider an address at all. */
  DCHECK(originOf(msg && msg.sourceUrl) !== "",
         "this zone could not serialize an origin from a document address the engine's own url.c accepted — " +
         "the frontier key would name a document by a string the two parsers do not agree is one");
  const fkey = msg.sourceUrl + "|" + _bid;
  const prior = persist ? await frontierGet(fkey) : null;
  /* THE RESIDUE THIS SESSION IS ABOUT TO REPLAY BELONGS TO THIS DOCUMENT. The engine seeds its frontier from
     these recipes INSTEAD of a boot flow, so a residue from another document is not a degraded resume — it is
     a frontier of flows standing on a path this program never took, with this document's own first flow
     absent. Under the key as it stands this is an identity; it is written down because it fires the moment a
     term leaves the key that this comparison still carries, which is exactly how the coarse key came to look
     sound for as long as it did. */
  DCHECK(!prior || prior.sourceUrl === msg.sourceUrl,
         "the cold tier answered this document's frontier key with a residue parked by a document at a " +
         "DIFFERENT address (" + (prior && prior.sourceUrl) + " vs " + msg.sourceUrl + ") — the engine seeds " +
         "from recipes instead of a boot flow, so this document would be replayed along another one's " +
         "decision vectors and never explored from its own first script");
  // PHASE 2 — seed the frontier (fresh, or resume parked recipes). The host sets a VALUE yield-floor per
  // step (the runner-up engine's weight), so this engine yields when it's outranked — no fixed slice.
  await rend.renderer.begin((prior && prior.recipes) ? prior.recipes : "");
  // DEV-ONLY verification hook (a real page never carries this query param): force the RAM-pressure park so
  // the cross-session round trip (park recipes -> IDB -> restart-keep -> resume) is VERIFIABLE without a 512MB
  // working set. Keyed off the URL (flows reliably through msg.sourceUrl to here, unlike a cross-context
  // global). THE QUERY PARAM IS PART OF THE FRONTIER KEY, which it was not while the key was origin+bundle:
  // the key is the document's ADDRESS plus the bundle id, so `?__forcepark=1` names its own entry and a later
  // PLAIN visit to the same route resumes nothing. That is the correct reading rather than a cost — the
  // residue was parked by a document at that address, and a document at a different address is a different
  // document — and it is what the round trip verifies against: the stored sourceUrl carries the param, so a
  // rehydration re-derives the same key and the `!(prior && prior.recipes)` guard below still keeps a resumed
  // session from re-parking forever.
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
     THE BODY IS NOT IN THE RECORD, AND THAT IS THE POINT. §2.2.4 Bodies makes a body's source a BYTE SEQUENCE
     and JSON cannot carry one: `JSON.stringify` on a Uint8Array answers `{"0":72,"1":101,…}` — a plausible record
     whose body is not the body. The only ways to put bytes in JSON are to encode them (base64: a CODEC on both
     sides to get wrong, 4/3 on the wire for bodies that are whole JS bundles, and a second copy of the
     expanded text held by the parse) or to DECODE them, which is what `resp.text()` was doing and is the whole
     defect — a UTF-8 decode run in the zone that owns SOP/CORS and owns no semantics, before HTML §8.1.4.2's
     own decode could look at the charset the response declared. So the record travels as text and the bytes
     travel as bytes, copied straight into the engine's linear memory. */
  /* THE REQUEST IS THE PAIR — `method` is half of what the flow parked on, not a hint about the address. It
     arrives already normalized (Fetch §2.2.1 Methods, "normalize a method"), so `GET` is exactly the set this
     zone can perform, HEAD included: a HEAD answered with a GET's body is the same substitution.
     A REQUEST THIS ZONE CANNOT ISSUE IS REFUSED BEFORE THE CALL AND NEVER DOWNGRADED. safeFetch hardcodes
     `method:"GET"` and reads neither `opts.method` nor `opts.body` (SECURITY.md §Network, "GET only, enforced by
     ABSENCE"), so fetching a POST's address as a GET and providing those bytes under the POST's key would be
     the defect this seam's method was added to close, with the pairing now CORRECT: the reply would match the
     request it names and still be a response the server never gave for it. That is the wrong answer moved one
     zone up. Nothing state-changing is issued, which is the rule this preserves rather than relaxes.
     THE REFUSAL IS §5.6 Fetch methods' NETWORK ERROR, WHICH ON THIS SEAM IS SPELLED `null` — the same refusal
     fetchedXhr makes, in this seam's own grammar rather than in the XHR seam's. There the record always crosses
     and `status: 0` with no bytes leaves the response the network error §3 starts it as, so `blocked-method:<M>`
     rides along as a diagnostic that xml_http_request.c's xhr_take_reply returns before ever reading. HERE the
     record IS the page's Response: core/fetch's delivery rejects with the TypeError only for the JSON `null`,
     and ANY object is built into a Response, so a status-0 record would RESOLVE `fetch()` with a status no
     server returns — a reply this zone fabricated. `if (r.status === 0) return null` below already says exactly
     that about every other refusal safeFetch makes (blocked-scheme, blocked-cors-credentialed, CORB). */
  const fetched = async (method, u, asScript) => {
    DCHECK(typeof method === "string" && method !== "",
           "a pending request reached the chokepoint with no method — GetPending answers `METHOD<TAB>URL` and " +
           "the pair is what the flow parked on, so a request whose method is unknown can be neither refused " +
           "nor issued");
    if (method !== "GET") return null;
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
             "out of this and a page reads status/headers/body off it, and §2.2.4's body source is a BYTE " +
             "SEQUENCE");
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
      /* AND §2.2.6's STATUS MESSAGE, WHOSE EMPTY VALUE IS A REAL ANSWER — which is exactly why `|| ""` could
         not stand beside it. HTTP/2 and HTTP/3 carry no reason phrase at all, so `""` is what a perfectly
         ordinary response says, and safeFetch writes the field on every one of its return paths (its blocked
         arms put the REASON there, which is the only account a page ever gets of a request the chokepoint
         refused). Defaulting it therefore mapped "this protocol has no reason phrase" and "the chokepoint
         stopped writing the field" onto the same two bytes, and the second of those is a blocked-by-CORB or
         blocked-scheme answer arriving at the engine with its explanation deleted. */
      DCHECK(typeof r.statusText === "string",
             "safeFetch answered a reply with no statusText — it is written on every path (the blocked arms " +
             "carry their REFUSAL REASON in it), and an empty string is a legitimate answer from any HTTP/2 " +
             "response, so an absent one cannot be defaulted without erasing the difference");
      /* STATUS 0 IS NOT A REPLY. It is the one status no HTTP response has, and it is exactly what the
         chokepoint answers when the request never went on the wire at all (bad URL, blocked scheme, blocked
         private target, CORB). This comment's own rule — «`null` is a NETWORK ERROR, which is what a URL this
         zone must not or cannot fetch honestly is» — names that case, and the record was crossing anyway: the
         page saw a real reply whose status was a number no server returns, instead of §5.6's TypeError. */
      if (r.status === 0) return null;
      /* TWO PIECES OF ONE REPLY, because they cross on two channels — `meta` is the JSON qjs_provide parses
         and `bytes` is what it copies into the engine's heap beside it. They are handed over together so a
         caller cannot deliver one without the other. */
      return { meta: { status: r.status, statusText: r.statusText, headers: Object.entries(r.headers),
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
    if (!canFetch) return { headers: {}, bytes: null };
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
      if (!r.ok) return { headers: {}, bytes: null };
      /* THE BYTES, because a Document is PARSED from a byte sequence. This answered `r.body` while that was
         `resp.text()`'s UTF-8 decode, so a document served in any other encoding reached lexbor already
         replaced with U+FFFD and HTML §13.2.3.2's encoding sniffing — the BOM, then the transport charset,
         then §13.2.3.3's prescan — had nothing left to decide. The engine owes that algorithm; this zone owes
         it the bytes. */
      /* THE WHOLE HEADER LIST, NOT ONE POLICY PULLED OUT OF IT. This answered
         `r.headers["content-security-policy"] || null` — one name, chosen here — so HTML §7.1.3's opener
         policy, §7.1.4's embedder policy and §7.5.1's `Origin-Agent-Cluster` could not reach a navigated
         Document at ALL, and every document a navigation created got the agent-cluster keying and the
         cross-origin isolation of a response that sent nothing. Which names matter is the ENGINE's question,
         in the browser components that own those standards; this zone relays what the response carried, the
         same way it already does for the document that roots an instance (responseFieldLines). */
      return { headers: r.headers, bytes: r.body };
    } catch (e) { RETHROW_FATAL(e); return { headers: {}, bytes: null }; }
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
    // Release: the DCHECK above is stripped, and every read below is of a record this proves exists.
    if (!q || typeof q.url !== "string") return { meta: { status: 0, statusText: "", headers: [] }, bytes: null };
    /* THE METHOD IS PART OF THE REQUEST'S IDENTITY, so a request whose method this zone cannot issue is
       REFUSED and never DOWNGRADED. safeFetch hardcodes `method:"GET"` and reads neither `opts.method` nor
       `opts.body` (SECURITY.md's "GET only, enforced by ABSENCE"), so handing it a POST used to answer the
       page with the reply to a GET of the same address — a response the server never gave for that request,
       which every @H example value and every @S verdict downstream is then derived from. A wrong answer is
       worse than an absent one: `blocked-method:<M>` is the same refusal shape as `blocked-scheme:` and
       `blocked-cors-credentialed`, and with no bytes beside it the engine's xhr_take_reply leaves the
       response the network error §3 starts it as, which §3.5.6's "handle errors" turns into the `error`
       event. Nothing state-changing is issued, which is the rule this preserves rather than relaxes.
       GET IS THE WHOLE SET, HEAD INCLUDED: a HEAD answered with a GET's body is the same substitution.
       The method arrives already normalized (Fetch §2.2.1 Methods, "normalize a method", which XHR §3.5.1
       The open() method runs), so `GET` is exactly the requests this zone can perform. */
    DCHECK(typeof q.method === "string" && q.method !== "",
           "the engine's xhr.send record names no method — xhr_request_op writes one on every record, and a " +
           "request whose method is unknown cannot be refused OR issued");
    if (q.method !== "GET")
      return { meta: { status: 0, statusText: "blocked-method:" + q.method, headers: [] }, bytes: null };
    if (!canFetch) return { meta: { status: 0, statusText: "", headers: [] }, bytes: null };
    try {
      const abs = new URL(q.url, msg.sourceUrl).href;
      /* @security-finding  `headers` COMES FROM THE UNTRUSTED BUNDLE and IS read by the chokepoint: within
         the model (uncredentialed GET to a public host, forbidden header names stripped by the browser), but
         it is why safeFetch's "analyzer probe headers only" comment is no longer the whole truth, and it must
         be re-decided the day credentialed mode is turned on. `q.credentials` is deliberately NOT mapped onto
         `opts.credentialed`: that flag is the trusted zone's decision to replay a learned GET with the user's
         cookies, and taking it from the page's `withCredentials` would let the analysed bundle turn credential
         attachment on for itself. Only what the chokepoint reads is passed, so there is nothing left here for
         it to drop in silence. */
      const r = await self.safeFetch(abs, { pageUrl: msg.sourceUrl, headers: q.headers });
      DCHECK(r && typeof r === "object" && r.body instanceof Uint8Array && typeof r.status === "number" &&
             r.headers && typeof r.headers === "object",
             "safeFetch answered an XHR with something other than its reply record — §3.5.6's response is " +
             "built from it and the page reads status, statusText and every header off that, and §2.2.4's " +
             "body source is a BYTE SEQUENCE");
      /* AND §3.5.6's STATUS MESSAGE, ASSERTED FOR THE REASON `fetched` GIVES ONE LINE-FOR-LINE: `""` is what
         an HTTP/2 response legitimately reports, so a `|| ""` beside it says the same two bytes for a real
         answer and for a chokepoint that stopped writing the field — and on the arms where safeFetch REFUSED
         the request, this field carries the whole reason it refused. */
      DCHECK(typeof r.statusText === "string",
             "safeFetch answered an XHR with no statusText — XMLHttpRequest §3.7.5's `statusText` getter is " +
             "read straight off it, and the chokepoint writes one on every path including the refusals whose " +
             "reason it is");
      /* THE BYTES BESIDE THE RECORD, for the reason `fetched` states: §3.6.6's "get a text response" DECODES
         the received bytes with the FINAL encoding, and until those bytes crossed as bytes that algorithm was
         decoding a re-encoding of this zone's own UTF-8 guess. `responseType = "arraybuffer"` handed the page
         the same round trip in place of the server's bytes. */
      return { meta: { status: r.status, statusText: r.statusText, headers: Object.entries(r.headers) },
               bytes: r.body };
    } catch (e) {
      /* §3.5.6's "handle errors": a THROWN fetch is the network error that becomes the page's `error` event.
         An invariant abort is not one and travels on.
         NO `body` IN THE RECORD. The bytes cross BESIDE the JSON and `bytes: null` is the whole statement that
         this answer has none; a `body` key inside the record is the second spelling `fetch_reply_set_body`
         DCHECKs against, and it would abort the day one of these paths carried bytes. */
      RETHROW_FATAL(e);
      return { meta: { status: 0, statusText: "", headers: [] }, bytes: null };
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
     RETAINED (five of qjs_init's arguments) lives as long as the module does, the module dies with the frame, and
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
   arguments `qjs_init` takes, stated by this zone for the same reasons, and it is a DIFFERENT operation
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
async function engineJoin(eng, msg, docName, topLevelUrl, inherited, parentNavigable) {
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
  /* HTML §7.3.1.3's PARENT, on the same rule engineCreate states and with the same two answers. A JOINED
     document is not exempt from the question and is one of the shapes that makes it interesting: a same-origin
     document nested through a CROSS-ORIGIN one belongs to this cluster and to another instance's frame tree at
     once, so its navigable is a child navigable whose parent this zone is the only party that can name. */
  DCHECK(typeof parentNavigable === "string" && parentNavigable !== "",
         "a document was joined with no HTML §7.3.1.3 PARENT NAVIGABLE — a navigable either has a parent or " +
         "is a top-level traversable and both are facts a host states, so an absent one is a caller that " +
         "skipped the question and a nested document that would answer `parent === self` inside the only " +
         "instance that holds it");
  const html = msg.pageHtml;
  /* THE SAME ONE SHAPE THE ROOT'S DOCUMENT TAKES, and for the same reason: `content.mojom.Renderer.Join`
     declares `array<uint8>` because `qjs_join` has main.c's byte-identical signature, so the two operations
     take ONE contract and a change to what a document arrives with reaches both. */
  DCHECK(html instanceof Uint8Array || typeof html === "string",
         "a joined document arrived as neither a byte sequence nor characters — a child navigable's document " +
         "is safeFetch's response body and a reported one is the renderer's serialized DOM, so anything else " +
         "is a document this agent would hold as nothing at all");
  const _doc = html instanceof Uint8Array ? html : new TextEncoder().encode(html);
  /* THE SAME 0x00 THE ROOT'S DOCUMENT NO LONGER ASSERTS AGAINST, deleted here with it and for the one reason:
     `qjs_join` takes `(bytes, len)` now, so a joined document carrying a NUL parses whole and the tokenizer
     applies its own per-state rule to that byte (§13.2.5.4 "Script data state" emits a U+FFFD; §13.2.5.1
     "Data state" emits the character and §13.2.6.4.7 The "in body" insertion mode ignores it). The two entries
     take ONE contract — main.c's signatures are byte-identical — so a change to what a document arrives with
     reaches both, which is exactly what this pair of asserts existed to keep true. */
  /* HTML §7.1.7's CLONE OF THE CREATOR'S CONTAINER, relayed off the same create notice that sent this
     document here — the two entries take ONE contract, so what a document arrives with reaches both. A join
     ALWAYS carries one where there IS a creator, and the pair is EMPTY where there is not — which is the case
     a cross-document navigation of a top-level traversable brings here (§7.4.6.1), and the reason this reads
     the pair rather than demanding a self-origin. There is still no `null` arm: engineRoot takes one because
     its callers include a rehydrated recipe that has no message at all, and every caller here has a message. */
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
  const _join = await eng.r.renderer.join(_doc, msg.sourceUrl, docName,
                                          responseFieldLines(msg.responseHeaders), topLevelUrl,
                                          inherited.csp, inherited.selfOrigin,
                                          inherited.embedder.value, inherited.embedder.endpoint,
                                          inherited.embedder.reportOnlyValue,
                                          inherited.embedder.reportOnlyEndpoint,
                                          parentNavigable);
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
/* THE DOCUMENT A NAVIGATION REPLACED — HTML §7.4.6.1 "Updating the traversable"'s DEACTIVATE A DOCUMENT FOR A
   CROSS-DOCUMENT NAVIGATION, and the other half of engineJoin above. The two are ONE navigation: the browser
   loaded a new Document into a navigable of this agent and the old one stopped being active, so the instance
   is told both halves, in the standard's own order (§7.4.6.1 unloads `displayedDocument` given `targetEntry's
   document`, so the incoming Document exists first).

   IT IS NOT §7.3.1.6 "Navigable destruction", WHICH IS THE NUMBER THIS ZONE'S OWN ABORT MESSAGE USED TO NAME.
   §7.3.1.6 destroys a NAVIGABLE — a container removed, a traversable closed — and a navigation destroys none:
   the navigable is exactly what survives, and what changes is which Document is ACTIVE in it. The two meet one
   step down, at §7.5.9 "Unloading documents"' "if oldDocument's salvageable state is false, then destroy
   oldDocument" and the §7.5.10 "Destroying documents" it reaches, which is why the engine serves both entries
   from one machine. A wrong section number is worse than none because it reads as authoritative, and that one
   was the standing instruction for what to build next.

   ONLY THIS ZONE KNOWS IT. A navigation the real browser performed leaves no trace inside the instance's heap:
   the outgoing document's scripts are still queued, its flows are still parked, its Window still answers. What
   crosses is the browser's own document id and nothing else — every other fact about that document is already
   in the agent, because the agent is where it was rooted or joined.

   IT SEEDS AND DOES NOT WAIT. The engine attaches the unload to every one of its live timelines and returns;
   each of them fires that document's `pagehide` and `unload` listeners under its own delta when the scheduler
   next runs it, which is real surface (a `sendBeacon` in an unload handler is an endpoint this tool exists to
   find) and is preemptible and parkable like every other job. No flow is dropped, starved or paged for it. */
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
  DCHECK(typeof incomingDocName === "string" && incomingDocName !== "" && incomingDocName !== docName,
         "a navigation was reported with no INCOMING document, or with one document named as both halves — " +
         "HTML §7.4.6.1 replaces a navigable's active document WITH ANOTHER, and the incoming name is what " +
         "makes the ORDER checkable at the engine (it must already be joined) and what gives the engine a " +
         "realm to queue the operation in that is not the one being destroyed; §7.5.10 step 7 removes every " +
         "queued task of the destroyed document, so the outgoing realm would make the first timeline's " +
         "destruction drop every other timeline's");
  DCHECK(eng.joinedDocIds.indexOf(incomingDocName) >= 0,
         "a navigation named an INCOMING document this instance has not joined (" + incomingDocName + ") — " +
         "engineJoin is what puts it in the agent and is what pushes the name here, so this is the two halves " +
         "of one navigation called in the wrong order, and the engine's own `world_doc_hosted` assert on the " +
         "incoming half is what it would reach");
  /* THE NAME IS NOT REMOVED FROM `joinedDocIds`, AND THAT IS NOT AN OVERSIGHT. §7.5.10 destroys the DOCUMENT,
     not the realm: the engine still interns that name, still answers for it, and a peer's route to it must
     still resolve here — to an instance that reports it as a destroyed navigable, which is the true answer and
     the one §7.2.2.1's `closed` is read for. Splicing it out would make `hostHolderOf` say no instance holds
     the name, and the very next arrival spelling that document would try to JOIN it and hit the engine's
     one-realm-per-document CHECK. */
  await eng.r.renderer.unload(docName, incomingDocName);
  /* THE ROUND ENDS HERE, SO IT RECORDS — engineJoin's reason exactly: an unload attaches a job to every live
     timeline, so the frontier this instance's Level-1 weight is computed from has moved. */
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
  /* -INFINITY IS AN ANSWER, NOT A BROKEN ONE. `engine_top_weight` is
     `flow_next_to_run(NULL) ? flow_weight(b) : -1.0/0.0` (solver/engine.c), so negative infinity is the
     engine's POSITIVE statement that its frontier holds no runnable flow — and it is the RIGHT statement,
     because that is precisely the value that ranks below every real weight in the comparisons below. THAT
     SENTENCE PREDATES THE C THAT MAKES IT TRUE: the engine asked flow_best, which ranks EVERY member including
     the ones parked on this very host, so an engine whose whole frontier was waiting published the weight of a
     flow it could not run — it burned no CPU, so that weight never aged, so the evictee pick below could never
     reach it. Demanding a finite number here rejected the producer's own vocabulary:
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
/* AND IT IS DELIVERED AGAINST THE REQUEST, WHICH IS THE PAIR. `qjs_provide` takes the method first, in the
   order a request line states them (RFC 9112 §3 Request Line), because the engine's pending register is keyed
   on both halves: a delivery named by the address alone settled whichever request was parked on that address
   first, so a page issuing a GET and a POST to one address had them collect each other's bodies. */
async function engineProvide(eng, method, url, rep) {
  DCHECK(rep === null || (rep && typeof rep === "object" && rep.meta && rep.bytes instanceof Uint8Array),
         "`fetched` answered neither §5.6's network error (null) nor a reply — a reply is its JSON metadata " +
         "and its BYTES together, and a record arriving without one of the two is half a response");
  await eng.r.renderer.provide(method, url, JSON.stringify(rep === null ? null : rep.meta),
                               rep === null ? null : rep.bytes);
}
/* ONE LINE, SPLIT IN ONE PLACE — the mirror of the engine's own `engine_pending_split`, which exists because
   three C hosts each finding the TAB for themselves is three places for the grammar to drift; a fourth host in
   another language is not an exception to that. The delimiter is unambiguous by two specs and neither half can
   contain one: URL Standard §4.4 URL parsing has the basic URL parser remove every ASCII tab or newline from
   its input before anything else, and Fetch §2.2.1 Methods makes a method a byte sequence matching the method
   token production, whose tchar (RFC 9110 §5.6.2 Tokens) is VCHAR-only and excludes HTAB.
   IT IS A `CHECK` AND THE ENGINE'S SPLITTER IS ONE FOR THE SAME REASON: the release path has no defined answer.
   A line this splitter cannot take apart is delivered against a method nobody asked for — the wrong answer this
   whole seam exists to make impossible — and NEVER against an assumed `GET`, which is the one guess that looks
   right for as long as the page issues nothing else. */
function pendingRequest(line) {
  const tab = line.indexOf("\t");
  CHECK(tab > 0 && tab < line.length - 1,
        "content.mojom.Renderer.GetPending answered a line that is not `METHOD<TAB>URL`: `" + line + "` — the " +
        "engine joins the pair and this zone must deliver against both halves, so a line missing one is a " +
        "reply keyed on a request nothing parked on");
  return { method: line.slice(0, tab), url: line.slice(tab + 1) };
}
async function engineServiceFetch(eng) {   // one round: answer every parked REQUEST, then the engine is hot again
  /* THE REPLY'S METADATA CROSSES AS TEXT AND CARRYING ITS TYPE — JSON, exactly as qjs_host_answer's answer
     does. A bare string could not say `null` for a network error without it being the four characters "null",
     and could not carry the URL list, the status or the headers at all. Its BODY crosses as BYTES beside it,
     because JSON can say none of the 256 values a byte has without first running an algorithm over them, and
     the algorithm this zone used to run (Fetch §5.2's `text()`, a UTF-8 decode) destroyed exactly the evidence
     HTML §8.1.4.2's classic-script decode exists to read. */
  /* THE CHUNK LIST CLASSIFIES THE PENDING LIST, IT DOES NOT ADD TO IT — so it is read FIRST and answered never.
     Every address the module loader reports was recorded by `module_load` at the moment it PARKED the load
     (engine_pending_module_url, which states its own `GET`), so each is already a GetPending line; fetching it
     again and providing it a second time answers a request that already carries a reply, which is
     engine_provide's answered-twice DFAIL — a live abort on every dynamic `import()` for as long as this loop
     provided both lists. What the list decides is the CORB class: a body that becomes executable code is
     fetched `as:"script"` so a cross-origin HTML/JSON body is never read as code (SECURITY.md §Network), and
     that is a property of the REQUEST — Fetch §2.2.5 Requests gives a request a destination — which belongs on
     the pending line beside the method, after which this list has nothing left to say. Until it is there, the
     register keeps a specifier after its load settled, so a later PLAIN fetch of that same address is classified
     as code: strictly a CORB narrowing (a cross-origin non-JS body is refused where a browser would have read
     it as data), and the residual that the destination is being read out of a second list instead of off the
     request. */
  const asScript = new Set(owedList("GetChunks", (await eng.r.renderer.getChunks()).urls));
  const requests = owedList("GetPending", (await eng.r.renderer.getPending()).requests);
  for (const line of requests) {
    const { method, url } = pendingRequest(line);
    await engineProvide(eng, method, url, await eng.fetched(method, url, asScript.has(url)));
  }
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
/* §7.3.2.3's "let group be a NEW browsing context group", as the only thing this zone needs of one: an id no
   other group has. It is minted here and never derived from anything an engine said, for the same reason the
   rendezvous token above is — a group id decides which documents share a heap, and an untrusted instance that
   could name an existing group could put its own document behind another origin's principal. */
let _nextSwapGroup = 1;

/* WHAT THIS ZONE OWES A ONE-WAY NOTICE. Two ops today, and each is an ACTION only this zone can take —
   SECURITY.md makes the offscreen the only zone that knows which instance holds which document.
   `navigable.create <child> <creator> <url> <origin> <topLevelUrl> <cspSelfOrigin> <coep> <coepEndpoint>
   <coepReportOnly> <coepReportOnlyEndpoint> <parentNavigable> <csp>` — the engine has
   already named the document and
   already handed the page a WindowProxy for it; what is missing is an INSTANCE. This provisions one under that
   name, loading the child's own document through the one safeFetch chokepoint.
   `windowproxy.post <target> <world> <targetOrigin> <base64>` — routed VERBATIM to the instance holding
   <target>, with THIS engine's origin stamped on it. The engine may not state that origin for itself: it is
   untrusted, and a forgeable event.origin defeats every origin check in every bundle. */
async function hostNotice(eng, line) {
  const f = line.split("\t");
  if (f[0] === "navigable.create") {
    /* TWELVE FIELDS, because CSP §2.2's SELF-ORIGIN, §7.1.4's four EMBEDDER POLICY items, the POLICY and HTML
       §8.1.3.1's TOP-LEVEL CREATION URL are all read below. The count said five once, so a record that stopped
       at the origin passed the assert and then took `undefined` for the creator's policy clone — a child
       document judged under NO policy, which is §7.1.7's inheritance silently deleted, and the one field a
       CSP-blocked sink verdict is decided against.
       An assert that permits the record it is about to misread is the shape of check that reports green while
       the value is missing, so it counts every field the reader below indexes. */
    DCHECK(f.length >= 13, "a navigable.create notice was short of its fields — the engine writes child, creator, url, origin, top-level creation URL, CSP self-origin, the four items of §7.1.4's embedder policy, HTML §7.3.1.3's parent navigable, and the policy");
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
    /* THE RESPONSE'S OWN HEADER LIST, WHOLE. The creator's inherited container does NOT go in here — it is
       relayed as a container of its own below, for the reason stated there. */
    DCHECK(loaded && loaded.headers && typeof loaded.headers === "object",
           "the document load answered no header list — §7.5.1 creates the child's Document from its " +
           "response's headers, and a missing list is a producer that stopped writing one rather than a " +
           "response that carried none, which is the empty object");
    for (const _n of Object.keys(loaded.headers)) msg.responseHeaders[_n] = loaded.headers[_n];
    /* THE CREATOR'S CLONE IS RELAYED AS A CONTAINER AND NEVER AS A RESPONSE HEADER, and that distinction is
       the whole of what this line used to get wrong. It read
           `if (!responseHeaders["content-security-policy"]) responseHeaders["content-security-policy"] = …`
       — writing the CREATOR's policy into the CHILD's response header list — and the engine then did the only
       thing it could with a response header: CSP §2.2.2 "Parse response's Content Security Policies" says
       "Return a CSP list whose policies is policies and self-origin is response's URL's origin", so the peer
       resolved `'self'` against the CHILD's address. CSP §2.2 "Policies" makes a CSP list "a struct consisting
       of policies (a list of policies) and a self-origin", and its note names this exact case: the self-origin
       exists "to facilitate the 'self' checks of local scheme documents/workers that have INHERITED their
       policy". §6.7.2.8 "Does url match expression in origin with redirect count?" is what reads it, so the
       consequence was one directive answered backwards in both directions — a creator's `script-src 'self'`
       permitting the child's origin and refusing the creator's, which is a real @S sink reported as
       CSP-blocked or a blocked one reported as live.
       SO BOTH HALVES CROSS, SEPARATELY FROM THE HEADERS, and the engine decides which container the Document
       is created with (main.c's document_csp_list). This zone RELAYS, it does not decide: the self-origin is
       the CREATOR's, stated by the engine that performed HTML §7.1.7's clone, and there is nothing here for
       this zone to compute — the one field it does own, the child's PRINCIPAL, is still derived from the URL
       it fetched.
       THE POLICY IS THE REST OF THE RECORD, not one field: it is a raw CSP header value and HTTP allows HTAB
       inside one (this engine's own CSP parser treats tab as source-list whitespace), so a policy carrying one
       splits into more fields than the record has. That is why it is LAST, and why both the top-level creation
       URL and the self-origin sit before it — an origin's serialization cannot contain a tab. The C router
       already reads it this way — its splitter stops at the policy and keeps the remainder verbatim — and two
       readers of one format that disagree about where a field ends are two formats. */
    /* §7.1.4'S FOUR ITEMS SIT BETWEEN THE SELF-ORIGIN AND THE POLICY, for the reason the self-origin sits
       before the policy: everything that is not the policy comes first, because the policy is the remainder.
       They are SAFE in split fields and that is the standard's own guarantee — §7.1.4's values are three fixed
       tokens, and §7.1.4.1 makes `report-to` a structured-field STRING, which RFC 8941 §3.3.3 "Strings" defines
       as "zero or more printable ASCII characters (i.e., the range %x20 to %x7E)" and notes "excludes tabs,
       newlines, carriage returns". A raw CSP header may hold a tab; none of these may.
       RELAYED VERBATIM. This zone does not read the tokens — the engine that receives them is the only party
       that turns one into a value, and it crashes on one it does not know rather than reading it as the
       default. See NEW_EMBEDDER_POLICY. */
    const inherited = { csp: f.slice(12).join("\t"), selfOrigin: f[6],
                        embedder: { value: f[7], endpoint: f[8],
                                    reportOnlyValue: f[9], reportOnlyEndpoint: f[10] } };
    /* HTML §7.3.1.3's PARENT NAVIGABLE, AT FIELD 11 AND NOT INSIDE THE CONTAINER BESIDE IT. A §7.1.7 container
       is five policies; a parent is a frame-tree link, and the two are relayed separately because they are
       separately true — an AUXILIARY navigable created by `window.open` has a full container and NO parent, and
       a record that folded the second into the first could not say so. It is the emitting engine's own
       navigable identity (core/frame/remote_object.h) and this zone relays it verbatim: the receiving engine is
       the only party that decodes one, and it crashes on a record it cannot parse rather than reading it as an
       absence. It sits BEFORE the policy for the reason everything else does — the policy is the record's
       remainder and a raw CSP header may hold a tab, while this field is a one-letter tag and base64 fields
       terminated by '.', none of which can. */
    const parentNavigable = f[11];
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
      await engineJoin(holder, msg, f[1], f[5], inherited, parentNavigable);
      return;
    }
    /* A CHILD DOCUMENT IS A DOCUMENT: it joins the ONE pool and is ranked, sliced, parked and finalized by the
       one host WFQ like every other. It has no caller to resolve to, so its findings merge the way a
       rehydrated cold engine's do rather than being returned to a requester that never asked for it — which
       is `cold`, stated at the reservation because it is a fact about this call site and not about how the
       boot turns out. AWAITED, because the notices of one round are acted on IN ORDER: a page opens a window
       and posts to it in the same turn, so the instance must exist before the post that names it is routed. */
    await engineCreate("", msg.pageHtml, msg, false, f[1], f[5], true, inherited, parentNavigable)._readyP;
    return;
  }
  /* `navigable.swap <new document> <url> <origin>` — HTML §7.1.3.2 "Browsing context group switches due to
     opener policy", step 10: a navigation whose response's opener policy does not match the navigable's active
     document's builds its Document in a NEW top-level browsing context in a NEW browsing context group, and the
     old browsing context is left behind (the emitting instance has already recorded that, which is what makes
     the opener's own handle answer `closed === true`).
     SO THIS IS THE ONE PROVISIONING THAT MUST NOT JOIN. An instance is `(browsing context group, origin)` and a
     swap changes the FIRST half, so a swapped-to document at an origin this pool already runs is still a
     SEPARATE heap — joining it to the existing instance would put the two principals COOP exists to separate
     behind one heap and one object graph. The group id is therefore minted HERE and by a counter: it is a
     routing fact, this zone's alone (SECURITY.md), and deriving it from anything the engine said would let an
     untrusted instance name a group that already exists.
     THREE FIELDS AND NO MORE, because §7.3.2.3 creates the new browsing context "with null, null, and group" —
     a NULL CREATOR, so there is no policy container to clone, no opener policy to inherit and no opener link to
     make. The swapped-to navigable is a top-level traversable, so HTML §8.1.3.1's top-level creation URL for
     its environments is its own address, derived here rather than sent twice and able to disagree.
     AND THE BYTES ARE LOADED BY THIS ZONE, not carried on the record: the emitting instance HAS the response
     (it needed the headers to obtain the opener policy that decided the swap) and may not hand it over — an
     untrusted engine that could supply bytes AND a principal could name any origin's document into existence.
     RESIDUAL, and it is one request too many rather than a wrong answer: a real browser commits the response it
     already has, and this fetches the address again, so a server that answers differently gives the new
     instance a document the first response's policy did not describe. Closing it is this zone remembering its
     OWN reply to the `document.fetch` that navigation already made, keyed by (instance, address). */
  if (f[0] === "navigable.swap") {
    DCHECK(f.length >= 4 && !!f[1] && !!f[2],
           "a navigable.swap notice was short of its fields — the engine writes the new document's name, the " +
           "address it is loading and the origin it computed, and a record missing either of the first two " +
           "names a Document this zone cannot provision while the navigable it replaces is already closed");
    DCHECK(!hostHolderOf(f[1]),
           "§7.1.3.2's swap named a document this pool already holds — the name is minted fresh for each swap " +
           "(solver/world.h), so a collision is one instance provisioned for two Documents, which is one heap " +
           "answering for two");
    const swapped = await eng.fetchedDocument(f[2]);
    DCHECK(swapped && (swapped.bytes === null || swapped.bytes instanceof Uint8Array),
           "the swapped-to document load answered neither bytes nor the null that means it did not load");
    DCHECK(swapped.headers && typeof swapped.headers === "object",
           "the swapped-to document load answered no header list — §7.5.1 creates its Document from the " +
           "response's headers, and §7.1.3 obtains the opener policy that decides its new group's cross-origin " +
           "isolation mode out of the same list");
    const swapMsg = { type: "AST_ANALYZE", pageHtml: swapped.bytes === null ? new Uint8Array(0) : swapped.bytes,
                      sourceUrl: f[2], origin: originOf(f[2]), groupId: "swap:" + (_nextSwapGroup++),
                      responseHeaders: {}, credentialed: !!(eng.msg && eng.msg.credentialed) };
    for (const _n of Object.keys(swapped.headers)) swapMsg.responseHeaders[_n] = swapped.headers[_n];
    /* NO INHERITED POLICY LINE HERE, and its absence is the spec rather than a field this record forgot: the
       create arm above falls back to the creator's cloned container for a response that carried none, and a
       swapped-to Document HAS no creator to clone one from. A response with no `Content-Security-Policy` is a
       Document under no policy, which is what §7.3.2.1 with a null creator produces. */
    DCHECK(hostClusterOf(clusterKeyOf(swapMsg)) === null,
           "§7.1.3.2's swap minted a browsing-context group this pool already runs an instance for — the group " +
           "id is a fresh counter, so a hit means two swaps were given one id and the second would JOIN the " +
           "heap the first built, which is exactly the boundary a group switch exists to draw");
    /* NULL: §7.3.2.3 creates the swapped-to browsing context "with null, null, and group" — a NULL CREATOR,
       so HTML §7.1.7 has no container to clone and this Document is judged against its own response alone. */
    /* AND HTML §7.3.1.3's PARENT IS `u`, WHICH IS §7.1.3.2's OWN STEP 2 AND NOT A BLANK: "if browsingContext
       is not a top-level browsing context, then return browsingContext" — the swap is only ever reached for a
       top-level one (the engine asserts it where the record is written), so the navigable this provisions has
       no parent by the standard rather than by this zone having nothing to say. */
    await engineCreate("", swapMsg.pageHtml, swapMsg, false, f[1], f[2], true, null, "u")._readyP;
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
  /* `remoteop.answer <token> <world> <completion>` — a COMPLETION this instance produced for an operation it was asked
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
    DCHECK(f.length >= 4, "a remoteop.answer notice was short of its fields — the engine writes the rendezvous " +
                          "token, the TIMELINE that computed the answer and the completion record, and a " +
                          "notice missing any of them names no call site or no timeline");
    const to = _remoteOps.get(f[1]);
    /* A TOKEN THIS ZONE DID NOT MINT names no asker, and the answer is then unroutable: the flow that asked is
       parked on a completion that will never arrive, which is silent everywhere. It is this zone's own key, so
       an unknown one means the holder echoed something other than what it was handed. */
    DCHECK(to !== undefined, "a peer answered a cross-agent operation under a rendezvous token this zone never " +
                             "minted — the token is echoed verbatim by the instance that performed it, so the " +
                             "flow that asked is parked on an answer nothing can deliver");
    /* THE COMPLETION IS THE REMAINDER AFTER THE THIRD FIELD, and the WORLD is that third field. It is split
       here and not left whole because the two are different kinds of thing: the world is a name in a grammar
       BOTH engines share (world_serialize's, whose own separators are ':' and ','), so it is a field with a
       boundary; the completion is remote_object.c's and may contain a tab, so it can only be the remainder.
       This zone reads NEITHER — it relays both — but it has to know where one ends. */
    const _t2 = line.indexOf("\t", line.indexOf("\t") + 1);
    const _t3 = _t2 < 0 ? -1 : line.indexOf("\t", _t2 + 1);
    DCHECK(_t3 > _t2 + 1, "a remoteop.answer notice carried no completion record after its timeline — an empty " +
                          "answer is not `undefined`, it is a relay that lost the peer's completion, and the " +
                          "engine's own decoder says so at the other end");
    /* AND EVERY ONE OF THEM IS RELAYED, never the last one to arrive. A peer's document state IS its flows, so
       one question has one true answer per timeline and the asking flow forks an arm over each; a zone that
       kept one per token — which the two-instance harness driver did, in a one-slot map — hands the asking
       page whichever answer the step interleaving happened to leave there, and a page reading one member twice
       in one expression is then answered out of two CONTRADICTORY timelines of one document. The engine
       refuses a repeat of the same timeline at its own delivery entry, which is what makes relaying all of
       them safe rather than merely permitted. */
    if (!to || _t3 < 0) return;
    await to.asker.r.renderer.hostAnswerRemote(to.req, line.slice(_t2 + 1, _t3), line.slice(_t3 + 1));
    return;
  }
  /* `world.gone <world>` — a world of THIS instance is finished with: the flow holding it left the frontier, or
     the whole session parked into a generation that will never mint again. Every peer that ever received a
     delivery or an operation from that world materialized a COW segment keyed on it (solver/world.h) and holds
     it until told — so this record is the only thing that ever removes one, and without it an instance that
     answered anything carries a foreign flow's state for the rest of its process and can never park at all
     (its own cold_park refuses, correctly: a foreign segment has no recipe of ours).
     BROADCAST, WHICH IS THE DESIGN AND NOT THIS ZONE BEING COARSE. The sending engine deliberately does not
     record which peers a flow reached: releasing a world with no segment here is a NO-OP, so tracking it would
     be state kept only to avoid one. The record therefore names no target document, and only this zone knows
     what the other instances are — which is this notice's whole reason to be a notice.
     A BOOTING INSTANCE IS SKIPPED AS A POSITIVE STATEMENT, not as a default: a segment is materialized by a
     routed delivery or a performed operation, and both branches above WAIT for `_readyP` before they hand an
     instance either, so an instance that has not finished booting has never been given a foreign world to hold.
     The engine says the same thing from its side — qjs_world_gone asserts the frontier was seeded. */
  if (f[0] === "world.gone") {
    DCHECK(f.length >= 2 && !!f[1],
           "a world.gone notice carried no world name — world_parse answers out of whatever the receiving " +
           "instance last parsed, so an empty one releases a segment belonging to a live peer's timeline");
    for (const peer of _pool) {
      if (peer === eng) continue;
      if (peer.state !== "hot" && peer.state !== "fetching") continue;
      await peer.r.renderer.worldGone(f[1]);
    }
    return;
  }
  /* `remoteop.retracted <token>` — an instance is parking with a question it was asked, and it is handing that
     question back rather than carrying it. STARTED OR NOT: a half-run answering program's partial work is the
     parking flow's own COW delta and leaves with it, and what this zone asked for was a CALL rather than a
     value (HTML §7.2.1.3.5 "CrossOriginGet ( O, P, Receiver )" ends "Return ? Call(getter, Receiver)"), so a
     call abandoned before it completed has been made zero times and the re-ask below makes it once.
     ONE NOTICE PER QUESTION, NOT PER HOLDER — the operation was attached to every live timeline of that
     instance, and the notice is sent by the LAST holder leaving, so a token arriving here is one no timeline
     over there will answer under again. THIS ZONE'S ACTION IS TO FORGET, and that is the
     whole of it: the suppression below (`_remoteAsked`) exists because `engine_host_requests` deliberately does
     NOT dedupe — two identical questions from two flows are two questions — so without it one operation would
     be performed once per step, each a program with the page's own side effects. The ASKING flow is still
     suspended and its request is still being reported every step, so lifting the suppression is the whole
     re-ask: the next sighting carries it to whichever instance holds that document by then.
     NOTHING IS STORED, AND THAT IS THE POINT. A token is this zone's name for an entry in this zone's
     in-memory table; it has no generation and it does not outlive this session, so it can never be written into
     a residue (solver/engine.h says why, and it is the defect the world-name generation closes one namespace
     over). Across a browser restart the ASKER is gone too and its own recipe RE-ISSUES the request under a new
     id — which is what the engine's late-answer refusal has always been built on. So neither side carries
     anything, and the only thing that has to happen is this forget. */
  if (f[0] === "remoteop.retracted") {
    DCHECK(f.length >= 2 && !!f[1],
           "a remoteop.retracted notice carried no rendezvous token — the token is the only thing that names " +
           "which asking flow's question is being handed back, so an empty one leaves that flow suspended " +
           "forever with this zone still suppressing the re-ask");
    const back = _remoteOps.get(f[1]);
    /* AN UNKNOWN TOKEN IS THIS ZONE ANSWERING FOR A QUESTION IT DID NOT ASK. The token is echoed verbatim by
       the instance that was handed it, exactly as `remoteop.answer` echoes it, so anything else means the
       engine minted a name of its own — and the flow this was meant to release is not the one released. */
    DCHECK(back !== undefined,
           "an engine handed back a cross-agent operation under a rendezvous token this zone never minted — " +
           "the token is echoed verbatim by the instance that was asked, so the asking flow this was meant to " +
           "un-suppress is still suppressed and stays parked");
    if (!back) return;
    back.asker._remoteAsked.delete(back.req);
    _remoteOps.delete(f[1]);
    return;
  }
  /* `world.parked <world vector>` — the OPPOSITE statement to the one above, and the one this zone cannot yet
     act on. A park writes the foreign segments this instance holds into its residue as replay recipes
     (solver/cold.h's 'w' record), so a PEER's timeline now outlives the session that held it: whichever
     instance resumes this document rebuilds a segment for that world from the vector this notice carries.
     WHAT IS OWED HERE IS A MAPPING ACROSS A PARK, and it is a decision about THIS zone rather than a missing
     line: `world.gone` is BROADCAST to the instances that are LIVE (the sending engine deliberately does not
     record which peers a flow reached, because releasing a world with no segment is a no-op), and a cold
     document is not one of them. So every death announced while this document is parked misses it, the
     instance that resumes rebuilds a segment for a world that has ended, and it holds it for the rest of its
     process — the exact leak `world.gone` exists to close, re-opened one tier down. Only this zone knows an
     instance is parked and which document it was, which is why the engine states the set exactly (the worlds
     the residue really carries, not every death for every cold document forever) and stops there.
     THE DECISION, NAMED SO IT IS BUILT AND NOT GUESSED: where does that index live, and how far does it
     survive? The frontier entry is keyed by (origin, bundle) and a death arrives naming a WORLD, so an index
     on the entry needs a persisted reverse world -> entry map; a store of its own needs its own eviction. And
     it must survive a BROWSER RESTART, because the residue does. Decide that, then deliver the held deaths to
     the resuming instance through the `worldGone` call that already exists (ordinal 20) — after `begin`, which
     is when its segments exist to be released. */
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
    /* §7.4 step 14's answer: the RESPONSE'S HEADER LIST as the HTTP field lines it delivered, and the
       document as BYTES. It carried one extracted policy (`{csp}`) — see fetchedDocument. The field-line form
       is the one a header list crosses this ABI in and is exactly what qjs_init takes, so a navigated Document
       and a rooted one are built from the identical shape by the identical parse. */
    await engineAnswer(eng, id, { headers: responseFieldLines(r.headers) }, r.bytes);
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
function crashRecord(stage, m, msg) {
  /* NO RESULT DOCUMENT IS EXPECTED HERE, and this is the only caller that may say so: the instance aborted
     before it could answer, so its absence is the crash rather than a broken contract. */
  /* The empties are `linesToAnalysis`'s own, on the arm that has no document to read — not four assignments
     over a record it just built, which is how the two producers of a crash record drifted apart in the first
     place. `_run:"crashed"` is what every consumer reads; there is no second marker beside it. */
  /* AND IT OWNS NO RUN ROW, which is the same split as the banner one line up. This is called PER WAITING
     CALLER for one instance that never booted, and each caller is a different DOCUMENT with a different
     address — so the records cannot share a row, and each states the run that document did not get. The
     instance-level fact (one abort) is the crash COUNT, which is incremented once. */
  return linesToAnalysis(['@E {"phase":"engine-crash","stage":"' + stage + '","err":' + JSON.stringify(m) + "}"], msg, "crashed", null);
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
    const evict = await ops.evictee(hot);
    if (evict) { target = evict; await ops.requestPark(evict); }
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
    if (pool.indexOf(target) < 0) { await ops.release(target); continue; }
    if (st === 0) {   // fully explored, or self-parked under RAM pressure: finalize (residue -> IDB cold tier)
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
                        evicted: 0, rehydrated: 0, navigated: 0 };
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
  evictee: async (hot) => {
    if (!_atRamFloor()) return null;   // there is room: nothing has to give anything up
    const cand = _bestCandidate(await frontierIndex());
    if (!cand) return null;                        // nothing is waiting for the memory
    let worst = null;
    for (const e of hot) if (!worst || engineWeight(e) < engineWeight(worst)) worst = e;
    if (!worst) return null;                       // every live engine is mid-round; the floor is re-asked next round
    if (!(cand.w > engineWeight(worst))) return null;
    _reserveStats.evicted++;
    return worst;
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
    const hasWork = partial.fetchCallSites.length || partial.securitySinks.length;
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
  serviceFetch: async (eng) => {
    try { await engineServiceFetch(eng); await engineRecordFacts(eng); }
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
         incoming Document (§7.4.6.1 "Updating the traversable"), which is nested in nothing. */
      await engineJoin(swap.cluster, swap.job.msg, swap.docId, _tlu,
                       { csp: "", selfOrigin: "", embedder: NEW_EMBEDDER_POLICY }, "u");
      await engineUnload(swap.cluster, swap.outgoing, swap.docId);
      return;
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
    const cand = _admissionHasHeadroom() ? _bestCandidate(_coldRanking) : null;
    if (cand) {
      DCHECK(cand.kind === "doc" || cand.kind === "cold",
             "the admission order produced a candidate of a kind this zone has no way to build (`" +
             cand.kind + "`) — every work item that is not resident is either a document waiting for an " +
             "instance or a parked frontier, and a third would be silently skipped by whichever of the two " +
             "arms below happened to be the fallback");
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
           navigable no create notice named, and the reason this zone can state its §7.3.1.3 parent at all. */
        const eng = engineCreate(job.code, job.html, job.msg, job.persist, null, null, false, null, "u");
        DCHECK(hostClusterOf(key) === eng,
               "engineCreate did not leave its reservation in the pool before returning — the whole point of " +
               "the slot being taken synchronously is that the next arrival for this cluster finds it, so a " +
               "pool that does not hold it is the race this reservation exists to close, still open");
        eng._resolvers.push(job);
        await eng._readyP;
        return;
      }
      /* THE WHOLE PARKED DOCUMENT COMES BACK THROUGH ONE READER, and the recipe's flows resume inside it.
         `frontierDoc` is the same shape the park wrote, asserted the same way in the other direction, so
         the field the tier was missing (`responseHeaders` — the policy container) cannot go missing again
         from one end only.
         THE CONTENT IS FETCHED HERE AND NOT AT THE RANKING, which is the other half of why the cold tier has a
         ranking VIEW. The order reads two numbers; only the item it PICKS is deserialized. */
      const stored = await frontierGet(cand.row.key);
      DCHECK(stored,
             "the cold tier's ranking view named an entry the store does not hold — this zone is the store's " +
             "only writer and frontierPut updates the view on its way through, so a row with no entry is the " +
             "projection and the store having drifted apart");
      const doc = frontierDoc(stored, "came back from the cold tier");
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
                    origin: doc.origin, groupId: "cold:" + cand.row.key,
                    responseHeaders: doc.responseHeaders,
                    topLevelUrl: doc.topLevelUrl, credentialed: stored.credentialed, persist: true };
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
         replay re-emits rather than through this path. */
      await engineCreate(doc.code, doc.html, msg, true, null, null, true, null, "u")._readyP;
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
       condition nothing in this file can change; the thing that changes it is the top's CONTENT_HTML, which
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
           reservations: { made: _reserveStats.made, rooted: _reserveStats.rooted, failed: _reserveStats.failed,
                           booting: booting, inFlight: inFlight, peakBooting: _reserveStats.peakBooting,
                           joinedBooting: _reserveStats.joinedBooting, joinedRooted: _reserveStats.joinedRooted,
                           navigated: _reserveStats.navigated,
                           evicted: _reserveStats.evicted, rehydrated: _reserveStats.rehydrated },
           coldRows: _frontierIndexBuilt ? _frontierIndex.size : null,   // null = the ranking view has not been asked for yet
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
    if (!html && !code) return { success: true, result: linesToAnalysis([], msg, "nothing-to-run", null) };

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
