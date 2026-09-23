// Drive a set of live URLs through the already-built extension and report, PER RUN,
// what the SHIPPED PATH WROTE — never what a harness printed.
//
// CLAUDE.md §Testing: "ONE RUN OF A LIVE SITE IS NOT A MEASUREMENT, AND A BEFORE/AFTER
// BUILT FROM TWO OF THEM IS AN ARTIFACT OF THE SITE." So this driver takes a RUN COUNT
// and reports every run's row plus the spread across them; it never averages, and it
// never collapses an ABSENT count into a zero — those are different facts and the two
// are printed with different tokens (`-` vs `0`).
//
// WHAT IT READS is `self._engineLog` in the offscreen document, which is the record
// `bridge.js` writes for every engine run (see `engineLogWrite`). That is the shipped
// path: the popup's GET_ENGINE_RUNS reads the same array. It is NOT a console scrape —
// the renderer deliberately does not tee its stdout, so a console scrape measures the
// instrument rather than the run.
//
// A row whose `run` is "crashed" carries NO counters at all, by bridge.js's own rule,
// and this driver preserves that: it prints `-` for each, because seven zeroes read as
// a run that explored nothing, which is a finding, and a crash is not one.
//
// Completion is polled off that row's own `run` word rather than off a wall clock,
// because §Testing's loaded-machine rule says a wall clock measures how busy the box
// was. The elapsed budget that remains is a BACKSTOP for a run that never reports at
// all, and it reports through a DIFFERENT token (`timeout`) so the two never collapse
// into one verdict.
//
//   node testing/live-run.js <runs> <url> [url…]
//
"use strict";

const path = require("path");
const fs = require("fs");
const puppeteer = require("puppeteer");
const { artifactStamp } = require("./artifact_stamp.js");

const LOCK_FILE = process.env.HARNESS_LOCK
  ? path.resolve(process.env.HARNESS_LOCK) : path.join(__dirname, "harness.lock");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* THE ARTIFACT IS NAMED IN THE OUTPUT — see testing/artifact_stamp.js, which is where the refusal that
   makes that name trustworthy lives, and which testing/live-why.js reads through as well. It was a copy
   in each driver and only this one carried the refusal. */

async function connect() {
  const lock = JSON.parse(fs.readFileSync(LOCK_FILE, "utf8"));
  return {
    extId: lock.extId,
    browser: await puppeteer.connect({
      browserURL: `http://127.0.0.1:${lock.port}`,
      defaultViewport: null,
      targetFilter: (t) => t.type() !== "browser",
    }),
  };
}

async function offscreenPage(browser, extId) {
  const url = `chrome-extension://${extId}/ast-worker.html`;
  for (let i = 0; i < 60; i++) {
    const t = browser.targets().find((t) => t.url().startsWith(url));
    if (t) { const pg = await t.page().catch(() => null); if (pg) return pg; }
    await sleep(200);
  }
  throw new Error("no offscreen document — is the extension loaded?");
}

// The offscreen's own view of this run. `rows` is bridge.js's per-run log; the store
// sizes are the cumulative moat, which is why the driver diffs them across the run
// rather than reporting the total (a total is every site this browser has ever seen).
function snapshot(pg) {
  return pg.evaluate(() => ({
    rows: (self._engineLog || []).map((r) => Object.assign({}, r)),
    endpoints: typeof globalStore !== "undefined" ? globalStore.endpoints.size : null,
    findings: typeof globalStore !== "undefined" ? globalStore.securityFindings.size : null,
  }));
}

/* IS THE ONE LEVEL-1 LOOP STILL ALIVE — a fact no counter in the row above can answer, and the one that
   decides whether a row is a measurement of the site named on it.
   bridge.js sets `_hostDead` ONCE and never clears it, and every document arriving afterwards is answered by
   `_hostKicksRefused++` and nothing else. So the FIRST crash that takes the scheduler down makes every later
   run in that browser produce no engine row at all — and this driver printed those under
   `budget-elapsed(no-row)`, whose own comment says it means "a document that was admitted and never
   provisioned an engine, which is NOT the same as an engine that ran and found nothing". That token was
   therefore carrying a THIRD nothing it does not name: a browser that died in an earlier run and was never
   asked about this site. §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES: an absent count and a zero count are
   different facts, and so are these.
   MEASURED, which is why this column exists: five runs across three app pages, artifact head 82c1a924. Run 0
   crashed all three sites, the third crash killed the scheduler, and the remaining TWELVE rows read
   `budget-elapsed(no-row)` — twelve rows about a dead loop, printed in the same column and the same shape as
   a finding about the engine. The driver takes a RUN COUNT precisely because one run is not a measurement,
   and it was silently returning one sample per site out of five.
   READ THROUGH `rendererPoolProbe` BECAUSE THAT IS THE ONLY SURFACE — `_hostDead`, `_hostDriving` and
   `_hostKicksRefused` are module-scoped bindings in bridge.js, not properties of `self`. The probe runs its
   own DCHECKs, so a throw is reported under its OWN token rather than folded into `alive`: a probe that could
   not answer and a scheduler that is answering are different facts, and defaulting one into the other is the
   exact shape this column was added to stop. */
function scheduler(pg) {
  return pg.evaluate(() => {
    try {
      const p = self.rendererPoolProbe();
      return { alive: p.scheduler.alive, driving: p.scheduler.driving,
               kicksRefused: p.scheduler.kicksRefused, diedOf: p.scheduler.diedOf };
    } catch (e) { return { PROBE_THREW: String((e && e.message) || e) }; }
  });
}

/* WHAT THE RUN COST, AND — the last four — WHAT ITS WORK MET. bridge.js forwards the @S arrival census onto
   every run record for the reason solver/result.c emits it: an empty `securitySinks` array has four readings
   that take opposite actions, and a probe that reports only `sinks: 0` cannot tell "no attacker source was
   ever read" from "taint arrived at a sink and the search was declined as unforgeable". This driver was
   reporting the cost columns and dropping the four that say whether there was anything to find. */
const COUNTERS = ["switches", "flows", "candidates", "jobsQueued", "jobsRun", "unitsDone",
                  "worldSegmentsHeld", "worldSegmentsMade", "worldSegmentsForked",
                  "sourceReads", "sinkReached", "sinkTainted", "sinkSuppressed",
                  /* AND THE ORPHAN PAIR, for the same sentence this list's own comment makes about the four
                     before it: they are the columns that say whether there was anything to find. §What-the-
                     tool-produces makes the drive of never-called code the headline surface, and `driven`
                     alone cannot tell a bundle with no uncalled code from a frontier that never reached the
                     question — `asked` is the one that separates them. */
                  "orphansDriven", "orphansAsked",
                  "endpoints", "sinks", "park", "resumed"];

/* …AND THE CENSUS ROWS THE ENGINE-LOG ROW ALREADY CARRIES AND THIS DRIVER DROPPED, which is a different
   question from every column above and is the one a reader asks the moment those columns disagree. The list
   above is what the run COST and what its work MET; none of its members can say WHERE the frontier is
   standing, WHICH arm
   its steps took, or WHICH predicate grew it — so a run reporting `flows: 18670, endpoints: 0` was a
   contradiction this driver could state and not resolve.
   IT IS THE SHIPPED PATH AND IT WAS ALREADY HERE. extension/bridge.js puts `forkAt` and `cold` on every
   engine-log row and says in its own words why: "Until they rode this document they were printed only by the
   smoke driver's loop, so every one of these numbers had been quoted about one fixture and never once about a
   real page." They still had not — `snapshot` copies the whole row, so these fields have been sitting one
   property access away from this output the entire time, and answering the question they answer took an
   ad-hoc script written against the offscreen document by hand. That is an instrument gap of exactly the kind
   §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES is about, pointed the other way: not a harness print that production
   never emits, but a production field no harness reads.
   WHAT IS TAKEN AND WHAT IS NOT. `forkAt` WHOLE, and from `cold` its three step/cursor histograms plus the
   four host/reply counters — not `heap`, not `swap`, and not the rest of `cold`, which answer what the
   allocator and a context switch cost rather than where the frontier is. A driver that took everything would
   be a second copy of the popup, and the columns that decided the question below are these.
   MEASURED, WHICH IS WHY IT IS THESE AND NOT THE WHOLE CENSUS. On play.grafana.org at head 64b09d1e, three
   runs each in a FRESH browser (§ONE-RUN-IS-NOT-A-MEASUREMENT, and the spread is quoted rather than a mean),
   the cost columns read `flows 18283-19583, endpoints 0` — and `jobsQueued` read 0, 832, 779, which is by
   itself the reason a single run of this driver could not settle anything. Those columns cannot distinguish
   "the engine never reached a request" from "it reached one and the emission did not fire". The census rows
   settle it and are STABLE across all three where the cost columns are not: `replyAsked/replyAnswered` read
   `6/6` every time, and that document ships EXACTLY SIX `<script src>` elements — so the request door opened
   for the bundle and for nothing else, which is what says no page `fetch()`, XHR or dynamic `import()` was
   ever reached. `programCursors` puts 88-97% of every run's members inside the document's FIRST program, with
   the deepest cursor reached being 1 or 2 of its ten script rows, and `forkAt`'s heaviest NAMED row is a
   branch on the same absent polyfill global in all three.
   THAT DENOMINATOR WAS COUNTED BY HAND AND IS A ROW NOW. "Ten script rows" is not in the census this driver
   reads: it was counted off the document, so it could not be re-derived from any log and could not be
   compared against another page at all. The engine publishes `rootPrograms` — the root document's own
   executable `<script>` count — beside `deepest` and `completed`, which is what makes "1 or 2 of ten" a
   reading a later run states rather than one a reader supplies. Taking it here is the next diff for this
   driver: without it `deepest` names a distance with no length beside it, and the same absence produced a
   landed analysis that read `progStarts` as a script count and reported scripts that never start.
   `hostAsked`/`hostAnswered` IS THE OTHER DOOR AND IS NOT THAT PAIR — solver/result.c says so where it emits
   them, and records that `hostAsked: 0` "has already been relayed as 'nothing is ever asked of the host' for
   a document holding hundreds of thousands of records". They are minted at engine.c's `mint_req`, which is
   reached from `engine_host_request` — and THAT has FIVE production callers, not four: a navigable's
   cross-instance read, a remote object's, a WindowProxy's, an iframe's, AND `XMLHttpRequest`, which places
   its request through the same rendezvous because §3.5.6's synchronous arm must BLOCK the flow while the
   asynchronous arm places the identical request from a task. So `hostAsked > 0` on a single-instance document
   MEANS AN XHR WAS ISSUED, and that is the one reading a driver of real pages most needs.
   THIS PARAGRAPH SAID "CROSS-INSTANCE RENDEZVOUS only" AND THAT WAS FALSE BY EXACTLY THE CALLER A LIVE DRIVER
   CARES ABOUT — CLAUDE.md's under-claim, which costs evidence rather than fabricating it, so nothing catches
   it: a reader told the row cannot speak about network requests DISCARDS a true reading and never finds out.
   It is not found by acting on it; it is found by COUNTING, which is one command
   (`grep -rn "engine_host_request *(" engine/host/`) and is how this one was found.
   AND IT WAS ALREADY CORRECTED IN solver/result.c AND NOT HERE, WHICH IS THE POINT: a fix that retires an
   argument falsifies every site where that argument was written down, and its code delta is not its size.
   THE REPAIR HAS LANDED AND THE TEST THIS SENTENCE GAVE FOR IT CANNOT SAY SO, WHICH IS THE PART WORTH
   KEEPING. It said `grep -rn "cross-instance rendezvous and NOTHING ELSE" engine/host/` still answers and that
   the site was owed a repair — and that grep STILL answers today, at the repaired site, because this tree's
   own rule is that a retired argument is REWRITTEN RATHER THAN DELETED and solver/result.c's correction opens
   by quoting the sentence it retires. So a retirement condition spelled as "grep for the wrong claim" is
   unsatisfiable HERE BY CONSTRUCTION: the fix makes the string more common, not less. Naming it by string
   rather than by line was still right — the coordinate would have rotted too — and what the string must name
   is the CORRECTION: that site now reads "THIS PARAGRAPH SAID … AND THAT IS FALSE BY EXACTLY THE CALLER",
   which is the positive form and which a later deletion of the repair would remove.
   Both pairs are carried here because each names its own door: the pair that answers "did this run ever ask
   for a RESOURCE" is the REPLY pair, and the pair that answers "did this run ever issue an XHR or read across
   an instance boundary" is this one. A document that does neither reads 0/0 for ever and is right to.
   READ `programCursors` WITH `stepUnitRuns.finished`, NEVER ALONE: the cursor row is a GAUGE over the members
   standing now, so it is a statement about the whole population only while nothing has retired, and `finished`
   is the row that says whether anything has.
   AND `start-a-classic-program` IS NOT THE STATEMENT IT READS AS, which cost one wrong sentence here before
   it was caught: solver/engine.c assigns it before `JS_FlowResume` and the completion arms OVERWRITE it, so a
   program that starts and ends in one step is filed under `resume-ended-its-frame`. The row counts starts that
   were PREEMPTED mid-program. Read the assignment, not the name.
   THE KINDS ARE NOT ALIKE AND THE HEADER SAYS SO. `stepUnitRuns` (solver/step_unit.h) and `forkAt`
   (solver/decide.h) are LIFETIME COUNTS and may be differenced; `stepUnits` and `programCursors` are GAUGES
   over the members standing NOW and CAN FALL. `hostAsked`/`hostAnswered`/`replyAsked`/`replyAnswered` are
   counters and are named separately from the histograms because neither door's rate is derivable from any of
   them — a frontier that never reached a request and one whose requests were all answered stand in the same
   arms and read identically in the three objects above.
   ABSENT AND ZERO STAY DIFFERENT FACTS: a crashed row carries no census at all, and each field is `null` there
   rather than `{}` — an empty object is a census that was taken and found nothing, which is a finding. */

/* WHAT THE PRODUCT ACTUALLY FINDS ON A REAL BUNDLE, MEASURED — AND IT IS FOUR STATIC FONTS.
 *
 * This driver watches a live page through a browser. The companion measurement is the frozen one, and it is
 * recorded here rather than beside the instrument because it is a fact about the PRODUCT and the two readings
 * belong together: `testing/corpus/mirror/gitlab` (a 4.5 MB real bundle, 18 tracked files) replayed by
 * `testing/corpus/serve-faithful.mjs` at its original host and paths, driven by `engine/pagecensus.mjs`
 * through the production ABI, artifact stamped `f84f671f`, quiet box (load 0.32 rising to 1.03), 51 samples.
 *
 *   fetchCallSites: FOUR, every one a `.woff2` font under /assets/, `provenance=derived`, `params` EMPTY.
 *   securitySinks: 0.   pageErrors: 0.   Terminal event: a real `@WHY`, not the budget.
 *
 * §Attacker-sources says a static asset is never an endpoint, so BY THIS PROJECT'S OWN RULE THE LEARNED API
 * SURFACE IS EMPTY — no address, no parameter, no example value, no encoding. Nothing built for bodies,
 * grades, headers or provenance was exercised, because nothing reached it.
 *
 * AND THE SURFACE WAS COMPLETE AT SAMPLE 0 AND NEVER GREW, which is the half that says where to look. All four
 * were known inside the first 750 ms; 118 seconds and 17 165 flows added nothing. So the terminal abort is NOT
 * the cause — it fired 117 seconds after the last thing was learned, and reading it as the cause is the
 * terminal-event-as-explanation mistake this file warns about one paragraph up.
 *
 * WHAT SURVIVES A REPEAT AND WHAT DOES NOT, because two runs of this document have now been taken and they
 * ended DIFFERENTLY (one at the CPU rlimit, one at an abort), so their reach columns are not comparable:
 *   IDENTITY, stable across both: `rootPrograms 24 / Held 7 / Awaited 17`; `replyAsked 21 == replyAnswered 21`
 *   at the FIRST sample before any fork — the whole bundle fetched up front, so the fetch path is not the
 *   problem; `_jobsRun` 0 in BOTH, so nothing behind an `await`, a `.then`, a timer or a delivery ever ran.
 *   REACH, this run only: `deepest 8`, `completed 7`, cursors `{7: 17158, 8: 7}` with ZERO below 7,
 *   `forks 17164`, `_unitsDone` 55.
 *
 * FIFTY-FIVE UNITS OF WORK AGAINST SEVENTEEN THOUSAND FLOWS IS THE FINDING. Every member finished program 6
 * and is standing at program 7's door — a chunk whose bytes were delivered before the first fork — and the run
 * spends itself forking rather than executing.
 *
 * AND WHAT IT FORKS ON IS ONE NAME, MEASURED TWICE. The fork census keyed by source identity
 * (solver/decide.c's Space-Saving table, read through the transcript) answers this outright on two independent
 * runs of this same mirror: SIXTY-FOUR OF SIXTY-FOUR named predicate rows key on `webpackJsonp`, carrying
 * 78.4% and 78.0% of every fork the run took, from SIX symbolic reads of the global object. The two runs
 * differed 4.25x in reach and both ended at SIGXCPU, so their fork TOTALS are reach totals and do not survive
 * a repeat — the IDENTITY does, and it is the identity that matters. The instrument licenses its own argmax:
 * its published Space-Saving bound was 213 against a largest floor of 2975, which is decide.h's own condition
 * for quoting an argmax whatever the spill is, and the site table's bound was 0, so nothing was evicted and
 * the unnamed-site rows are complete rather than truncated.
 * THE MULTIPLIER IS IN THE MIRROR'S OWN RUNTIME CHUNK and it is per-element x per-key, not per-chunk:
 *
 *   var h=this.webpackJsonp=this.webpackJsonp||[],t=h.push.bind(h);
 *   h.push=a; h=h.slice(); for(var s=0;s<h.length;s++)a(h[s]);
 *
 * `h.slice()` on a symbolic global yields an unknown array; `s<h.length` is an unknown-LENGTH loop, so it
 * forks per POSITION; each `h[s]` is unknown and the callback for-ins over it, so it forks per KEY. Every
 * operation token the census carries maps onto one step of that line, ending in `[[OwnPropertyKeys]]`,
 * `[[GetPrototypeOf]]` and `%ForInIteratorPrototype%.next`.
 * A COORDINATOR'S ARITHMETIC WAS HALF RIGHT AND THE HALF IT GOT WRONG IS THE INSTRUCTIVE ONE: he predicted the
 * NAME correctly and predicted the multiplier as per-chunk, which gives 2^6=64 against a frontier of tens of
 * thousands, and he said the arithmetic did not close rather than fitting it. It did not close because the
 * multiplication was over the wrong operation. A prediction that names its own gap is what let the census
 * settle it in one reading instead of confirming a plausible wrong mechanism.
 * WHAT THIS DOES NOT LICENSE, and it is the whole reason the measurement is recorded rather than acted on:
 * the forking is CORRECT. §Solver-half REQUIRES an iteration over opaque input to fork each iteration as its
 * own parkable flow, and no run has falsified any of those arms' premises. A hot path is exactly where
 * §a-wrong-narrowing is most tempting and most expensive. What is refuted is the MITIGATION §Solver-half
 * pairs with the forking — that the identical-input tail is outranked and paged — and that refutation is
 * recorded in CLAUDE.md at the sentence that states it.
 * STILL OPEN, stated so nobody reads it as settled: whether a global an EARLIER PROGRAM OF THE SAME FLOW
 * assigned is still read as unknown. Six `_absent` reads against roughly eighteen sites of that name is
 * CONSISTENT with sequential assignment already working, so the census cannot separate that from six genuine
 * first-reads and nothing here should be built on it.
 *
 * That is a THROUGHPUT statement and not an ordering one:
 * `neverPickedGap` was 0, and engine/build.mjs's own reading of that row says in its own words that nothing is
 * ranked ahead of those members, so the ordering is not what is keeping them out and the instant is not
 * evidence either way. A coordinator quoted `neverPicked 64605 of 71452` into a brief WITHOUT
 * `picksLifetime` — a fraction with no denominator, and the reading that says so was in a file he had been
 * editing the same day. The denominator is the whole of it: no order can dispatch what the thread never
 * reached.
 *
 * AND THE PROGRAM THE MASS IS STANDING IN IS THE PRODUCT'S OWN HEADLINE SURFACE, WHICH IS WHY THE EMPTY
 * RESULT IS WORSE THAN IT READS. The cursors above are an index into the root document's seeded programs, and
 * that table is derivable from the document alone rather than from any run — which is what makes this
 * checkable instead of a coordinate that rots:
 *
 *   grep -o '<script[^>]*>' <the mirror>/index.html        — 25 elements, of which ONE is
 *                                                            `type="application/ld+json"` and not executable
 *
 * Twenty-four executable, SEVEN inline and SEVENTEEN `src=`, which is `rootPrograms 24 /
 * rootProgramsHeldAtSeed 7 / rootProgramsAwaitedAtSeed 17` exactly — three rows confirmed against the
 * DOCUMENT rather than against the engine, which is the only independent check those rows have ever had.
 * Index 7 of that table is a chunk of TWO KILOBYTES whose entire top level is one statement:
 *
 *   (this.webpackJsonp=this.webpackJsonp||[]).push([["…"],{<module id>:function(n,r,a){…}}]);
 *
 * THE MODULE IS DEFINED AND NOT RUN — webpack executes a factory only when something requires it — and that
 * factory's body is a ROUTE TABLE: nine admin addresses with their parameter shapes, among them
 * `/admin/session/destroy`, `/admin/impersonation`, `/admin/deploy_keys/:id/edit` and
 * `/o/:organization_path/admin`, each with `id` or `organization_path` REQUIRED and `format` optional.
 * Admin endpoints shipped to a logged-out visitor, which is §What-the-tool-produces' entire thesis in one
 * file. The run reached the program that defines them and emitted nothing.
 *
 * SO THE PATH TO THEM IS ORPHAN DRIVING AND IT HAS AN ORDERING CONSTRAINT NOBODY HAS STATED. §Attacker-sources
 * already says the frontier gets "one drive per function the bundle shipped and never called", and a webpack
 * module factory is exactly that. What is NOT free is WHEN: the factory's first statements are `a("<id>")`
 * calls against the webpack require it is handed, and the route strings are built by a helper living in ANOTHER
 * module. Drive it with an unknown `a` and every route is unknown — the drive completes, emits nothing, and
 * looks like exploration. Drive it after the runtime and the entry chunk have registered their modules, with
 * the REAL require, and the same body computes nine concrete addresses. §Do-subproblems-IN-ORDER, arriving as
 * a constraint on a drive rather than on a diff.
 * WHAT WOULD SHOW THE DIFFERENCE — and the first answer written here was WRONG, which is recorded rather
 * than corrected away because the wrong pair is the one a reader reaches for. It named
 * `handAParkedDriveItsFunction` and `resumeAParkedOrphanDrive` against `seedOneOrphanFlow`, and those two
 * arms serve CROSS-SESSION RESIDUE ROUTING: solver/engine.c raises ORPHAN_ROUTE only behind
 * `!g_orphan_claims_closed && engine_orphan_route(...)`, which is the path that hands an INHERITED recipe the
 * body it was recorded for, and its own comment says a session with no residue pays one comparison. A fresh
 * session therefore reads 0 at both, CORRECTLY, and a reader following the retired sentence would have
 * concluded the drive mechanism was dead. The build that produced these numbers said so in its own output —
 * `no residue was handed to this session, so the drive verdict is not a reading` — one line above the
 * histogram the sentence was derived from.
 * THE PAIR THAT ACTUALLY ANSWERS IT is the orphan census's own: `orphan drive: N ask(s), M body(ies) driven`.
 * M of 0 is a frontier that queued drives and ran none. M above 0 beside an endpoint surface that is still
 * empty is the ordering constraint above — the bodies ran and computed unknowns, which is what driving a
 * webpack factory with an unknown require looks like from outside. On the fixture that pair reads 923 asked
 * and 315 driven and BOTH RISE across samples, so the mechanism is working end to end there; the mirror has
 * not been read for it.
 * THE METHOD IS THE FINDING AND NOT THE SENTENCE. The retired pair was chosen by NAME — three arms whose
 * names contain the word `orphan`, one of which counts the thing I meant — without reading what raises them.
 * §READ-THE-ACCESSOR is the same rule for a counter, and a step-unit arm is a counter with a sentence for a
 * name.
 * THE COORDINATES HERE ROT AND THE SHAPE DOES NOT: a minified bundle's chunk names, hashes and index numbers
 * change on every deploy, so re-derive the table with the grep above rather than trusting the index, and read
 * the rest as what a webpack bundle IS. */
/* …AND THE HALF OF THAT PAIR THIS DRIVER CARRIED WITHOUT. solver/engine.h's `over_arms` states the contract
 * in its own words — "the PAIR is the reading: an arm with many runs and no overruns is cheap however often
 * it is taken, and an arm whose two counts are EQUAL is a step that cannot rest" — and this list held the
 * RUNS half alone, which is the half that banner says means nothing by itself. Both are LIFETIME histograms
 * over solver/step_unit.h's one arm list, `sum(runs) == steps` and `sum(over) == sliceOverruns` asserted
 * where each pair is in one hand, so they are differenceable across two samples of ONE instance.
 * MEASURED, WHICH IS WHY IT IS HERE AND NOT ARGUED: on gitlab.com/explore, two fresh browsers, the terminal
 * census reads `start-a-classic-program` overrunning 9 of 12 and 6 of 10 while `start-ended-its-frame` — a
 * start that COMPILED AND FINISHED inside the step — overran 0 of 10 and 0 of 7, and `resume-program`
 * overran 227 of 5797 and 20 of 25102. Those three rows name three different diffs and the runs half alone
 * names none of them. The reading took an ad-hoc script against scratchpad probe JSON because no tracked
 * driver in this tree read either row; `testing/step_unit_read.py` is that derivation, tracked. */
const CENSUS_LIFETIME = ["stepUnitRuns", "stepUnitOverruns"];
/* …AND THE REPLY DOOR'S ONE LEVEL, FILED WITH THE GAUGES AND NOT WITH ITS OWN THREE SIBLINGS, which is the
   whole reason this driver splits the two lists: `replyOutstanding` is the count of records the host may still
   be shown AT THE INSTANT the census was composed, so it may FALL and differencing it reads a level as a rate.
   Its three siblings are lifetime counts and sit in COLD_COUNTERS below. They only mean anything read
   together — the identity is `replyAsked == replyAnswered + replyDeclined + replyDropped + replyOutstanding`,
   which solver/result.c asserts at the instant all five are in one hand and which holds on NO pair of lines. */
const CENSUS_GAUGE = ["stepUnits", "programCursors", "replyOutstanding"];
/* AND WHAT THE JOB BACKLOG IS WAITING ON — solver/flow.h's split, off `wfq` rather than `cold`. This
   driver already carries `jobsQueued`/`jobsRun`/`unitsDone` in COUNTERS and those cannot name a component:
   a queued job waits on the HOST (`jobsOwed`), on its member finishing its own program (`jobsFramed`, HTML
   §8.1.4.4 "Calling scripts", clean up after running script step 3), or on RANK (`jobsReady`), and ONLY THE
   LAST IS THE ORDERING'S TO MOVE. So a run reporting `jobsRun: 0` is charged to the scheduler or to flow_step
   by THESE rows and by nothing else in this file's output.
   THEY ARE GAUGES, filed here rather than with the counters for the reason the header line below states: a
   walk of the frontier at one instant may FALL and may never be differenced, and `jobsRun` beside it is a
   monotone total. `jobWGap` is read with `jobsReady` and never alone: 0 means EITHER that no holder is
   ready OR that the queue's top holds a runnable job. THOSE TWO READINGS ARE THIS ENGINE'S AND NOT
   HTML'S, AND THEY ARE DELIBERATELY NOT IN QUOTATION MARKS. They used to be, three lines under a
   §8.1.4.4 citation, and the citation auditor read a nine-word gloss of a `jobWGap` reading as a
   quotation of a standard that has no such counter — it diverged after four words and was reported
   as a spec defect. The words were ours the whole time. A run in quotation marks near a citation IS
   a quotation claim, whatever it was meant as, so a gloss of one of this engine's own rows is
   written as plain prose and a spelling being shown is written in backticks; neither is quoted.
   Derive rather than trust this: `grep -rci "top of the queue" engine/specindex/html*.json` answers
   0, with `clean up after running script` answering 1 as the armed control. `memUnframed` separates
   `jobsReady: 0`'s two silences; and
   `wfqMembers` is the population all of them are taken over. */
const WFQ_JOB_SPLIT = ["jobsReady", "jobsFramed", "jobsOwed", "jobWGap", "jobsReadyTask", "jobsReadyMicro",
                       "memUnframed", "visZero"];
/* `jobsReadyTask`/`jobsReadyMicro` ARE IN THAT LIST AND NOT AN AFTERTHOUGHT, because the reading they make is
   one only a REAL DOCUMENT poses and this driver is what carries a census off one. `jobsReady` says a backlog
   waits on rank; these say which ARM of flow_step can dispatch it — the checkpoint, which stands above the
   program sequence, or the task arm below it — so a run reporting `jobsRun: 0` has the SEQUENCE ARM'S
   exclusion confirmed by an all-TASK reading and refuted by any MICROTASK, which is the narrow claim the pair
   makes and the only one in this file's output. They are GAUGES like their neighbours and are
   filed with them for that reason, and the identity `jobsReadyTask + jobsReadyMicro == jobsReady` holds within
   ONE line and on no pair of them. */
/* …AND THE ONE @WFQ ROW THAT IS A LIFETIME COUNT, FILED APART FROM THEM BECAUSE THE KINDS ARE OPPOSITE AND
   THE NAMES DO NOT SAY. Every row above is a walk of the frontier at one instant and may FALL;
   `unframedPicksLifetime` is raised once per dispatch in flow_credit_pick and lowered by nothing, so it is
   the one of the set a reader may difference — and a series of it that decreases is the free tell that the
   filing is wrong. Putting it in the list above would be a counter read as a gauge, which is the defect
   CLAUDE.md records as a per-member gauge read as a lifetime histogram INVERTING a conclusion.
   IT IS THE ROW THIS DRIVER EXISTS TO CARRY TO A REAL PAGE. `memUnframed` beside it says who stands with an
   empty JavaScript execution context stack; this says how many dispatches that state has EVER received, and
   the pair is what separates the two readings of a `jobWGap: 0` that solver/flow.c's job-split residual
   states and refuses to choose between — zero with dispatches made says the order never handed the thread to
   one of them and the DISPATCH PATH is the subject, above zero refutes that for the unframed population as a
   whole. engine/build.mjs's `unframedPickSentence` renders the verdict; this carries the number so a run over
   a real document can be read without it.
   ITS DENOMINATOR IS ALREADY IN `COUNTERS` UNDER ANOTHER NAME and that is not a coincidence to be relied on
   silently: solver/result.c DCHECKs `picks_lifetime == engine_switch_count()`, so `switches` on this line IS
   the denominator, and a share taken against anything else is a fraction over the wrong population. Read the
   two together — this row at 0 with `switches` at 0 is the ABSENT reading of a zero and is about neither.
   GATED ON `live` WITH ITS NEIGHBOURS EVEN THOUGH IT IS NOT A READING OF THE WALK, because solver/result.c
   composes `{members: 0}` with NO term rows at all: on an empty frontier the row is absent from the document
   and `null` is the honest answer, never 0. */
const WFQ_LIFETIME = ["unframedPicksLifetime"];
const COLD_COUNTERS = ["hostAsked", "hostAnswered", "replyAsked", "replyAnswered",
  /* AND THE OTHER THREE ENDS OF THE REPLY DOOR, WITHOUT WHICH `replyAsked - replyAnswered` IS A NUMBER WITH
     THREE READINGS THAT TAKE OPPOSITE WORK. A record ends answered, REFUSED by this tool's own egress policy,
     DROPPED with a flow that departed owing it, or still OUTSTANDING; only the last is the host being behind.
     Measured on the very pages this driver is pointed at, before these rows existed: four fresh-browser drives
     of play.grafana.org read `9/7` every time and the two were refusals of the page's own boot `fetch()` and
     of its `.catch` arm's error report, while excalidraw.com read `31/31` in three — the same pair saying
     opposite things, and the gap was relayed on as two replies the host had failed to pay.
     `replyOutstanding` IS A GAUGE and the other two are LIFETIME counts (solver/pending_index.h), which is why
     it is named here rather than filed with them in the header line below — a reader who differences it is
     reading a level as a rate. An artifact older than these rows prints `-` for all three, which is this
     driver's own absent-versus-zero rule and is the honest answer: the run did not state them. */
  "replyDeclined", "replyDropped",
  /* AND THE REPLAY TRIPLE, WHICH IS THE COUNTER THE `forkAt` ROWS ABOVE HAVE TO BE READ AGAINST AND THE ONE
     THING THIS DRIVER DID NOT CARRY. solver/decide.c's `fork_site_name` states a NAMED RESIDUAL — a fork over
     an operand with no spellable identity "records no constraint, claims no replay slot, and re-forks every
     time the flow reaches it" — and it names exactly ONE way its absence would show: "a `~` row climbing
     across a session while the flow that owns it consumes no recorded arms — g_replay_hits flat against a
     growing site row". This driver already carried the `~` rows, because `forkAt` crosses whole; it did not
     carry the counter they have to be divided by, so the residual's own falsifier was the single reading a
     live run could not make. That is §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES pointed the other way — not a
     harness print production never emits, but a production field already sitting on this row that no harness
     read — and it is the same instrument gap this file's `census()` comment records for `forkAt` and `cold`.
     MEASURED, WHICH IS WHY THEY ARE HERE: five fresh-browser runs of play.grafana.org at artifact 00754e96,
     one row each. Four stood with 89-91% of the whole frontier inside the document's FIRST program and their
     `~` site rows summed 2122, 2964, 3546, 3868 — one key,
     `~{}[{__core-js_shared__}.wks.iterator]`, the largest row in every one of them. Reading that against the
     replay counter is the whole diagnosis and it took a side probe against the offscreen document to get a
     number this row was already carrying.
     THE UNITS ARE NOT ALIKE, AND THE BANNER NAMES THEM BECAUSE result.c CALLS THAT "the thing a reader must
     carry": `replayHits` and `replayLeftArms` are ARMS (decision-vector slots) and `replayLeft` is EVENTS
     (one per divergence, whatever it abandoned). Three lifetime counts in two units under one banner is the
     kind-stated/unit-unstated half of §A-GAUGE-AND-A-LIFETIME-COUNTER, and a reader who divides one by the
     other gets a ratio of two things. */
  "replayHits", "replayLeft", "replayLeftArms",
  /* AND THE DENOMINATOR EVERY ROW ABOVE IS A SHARE OF, PLUS THE FOUR-WAY ANSWER TO WHY A TURN DID NOT END A
     UNIT OF WORK. `steps` is what solver/result.c names as `_unitsDone`'s denominator — it says so at the
     `_unitsDone` line and deliberately does not re-emit itself there — and this driver carried `unitsDone` in
     COUNTERS with no denominator anywhere in its output, so every reading of it was a numerator alone.
     THE THREE `unit*` ROWS ARE THE REFUSAL ARMS OF THAT SAME GATE and the fourth arm is `unitsDone` itself,
     in the OTHER object of the result document; solver/engine.c asserts all four sum to `steps` at the line
     the credited arm is written on, which is the only thing that makes composing the split across this
     driver's two lists legitimate. Without them `unitsDone` reading low is three states behind one answer.
     MEASURED on gitlab.com/explore, terminal censuses of two fresh browsers: `unitParked` and
     `unitCheckpointOwed` read ZERO at every one of 28 censuses while `unitMidProgram` carried 96.6% and 98.6%
     of all steps — so the turns are not parked on the host and not owed a checkpoint, which is a pair of
     NEGATIVES no other row in this driver's output can state. The same counters read 12-19 and 34-38 on the
     native smoke at the same revisions, so that zero is an armed measurement and not a dead probe.
     `sliceOverruns` IS THE TURNS THAT MET THE COOPERATIVE SLICE, AND `sliceUs` IS NOT WHAT THOSE TURNS
     SPENT — this sentence said it was, and it is corrected rather than deleted because the division it
     prescribed is the one a reader re-derives. solver/engine.c accumulates `g_slice_us` UNCONDITIONALLY, on
     the line directly above the overrun test, so it is the STEP half of EVERY turn; solver/engine.h declares
     it as one of the two phases of `step_us`, and `slice + sched == step` is DCHECK'd where all three are in
     one hand. So `sliceUs / sliceOverruns` charges the turns that did NOT overrun to the ones that did: an
     UPPER BOUND on a mean overrun rather than one, exact only where every turn overran — which is the shape
     of this document's real-page runs and is why the error has never shown. The count alone still cannot
     tell a turn 1.2x past the slice from one 7400x past it, and what prices an overrun without borrowing a
     non-overrunning turn's time is `stepUnitOverruns` in CENSUS_LIFETIME above — the per-arm histogram
     solver/engine.c raises on the overrun line itself and asserts against `sliceOverruns` there. Read that
     against `stepUnitRuns` arm by arm; read `sliceUs` against `steps`, which is the population it is over.
     RETIREMENT: this correction goes when no reading in this file divides a whole-population accumulator by
     a subset count.
     `classicCompiles`/`classicCompileOverruns` SPLIT A START'S COMPILE FROM ITS EXECUTION and landed later
     than the rest; an artifact older than them prints `-`, which is this driver's absent-versus-zero rule and
     is the honest answer — the run did not state them. As of this commit NO measurement artifact in this tree
     carries either one, so the phase question they exist to settle has never been measured on either host.
     `rootPrograms`/`deepest`/`completed`/`deepestLeft` ARE THIS FILE'S OWN NAMED NEXT DIFF, taken now: the
     banner above says in as many words that without `rootPrograms` "`deepest` names a distance with no length
     beside it, and the same absence produced a landed analysis that read `progStarts` as a script count and
     reported scripts that never start". They are MAXIMA and not counts — solver/engine.h calls them so — and a
     maximum saturates and then plateaus, so a plateau in one is NOT a ceiling and the series length is part of
     quoting it. `finished` is the departure fact and is a genuine lifetime count. */
  "steps", "sliceUs", "sliceOverruns",
  "unitMidProgram", "unitParked", "unitCheckpointOwed",
  "classicCompiles", "classicCompileOverruns",
  "rootPrograms", "deepest", "completed", "deepestLeft", "finished",
  /* AND THE @H SURFACE'S OWN DENOMINATOR, WHICH IS THE PAIR THIS DRIVER'S HEADLINE COLUMN CANNOT BE READ
     WITHOUT. `endpoints` in COUNTERS is the store's SIZE — a reach figure — and solver/result.c records what
     such a number has already been quoted as: "every row of a 43-row surface was one of that document's own
     `<script src>`, `<link rel=stylesheet>` or `<link rel=preload>` elements, so the number a person reads as
     a learned API surface was the `<head>` counted back". `epEmitted - epPreProgram` is the most addresses
     FORCED EXECUTION can have contributed to that surface, so a run reading the two EQUAL learned nothing the
     markup did not already state, whatever `endpoints` says. That subtraction is §What-the-tool-produces'
     thesis stated as a number, and NO TRACKED DRIVER IN THIS TREE READ EITHER ROW — the same instrument gap
     the `census()` banner records for `forkAt` and `cold`, pointed at the one surface the product exists for.
     `epMinted`/`epAssets` ARE A DIFFERENT PARTITION OF THE SAME SURFACE AND ARE CARRIED SO THE SUBTRACTION IS
     NEVER READ ALONE: those two split it by what the REPLY was, and this pair splits it by who composed the
     ADDRESS. result.c gives an `epAssets: 0` three readings and names the one a reader reaches for first as
     the rarest, so the row is here to be read with its siblings rather than to be read.
     THE FIVE `epAsk*` ROWS ARE THE ASK SIDE OF THE SAME GATE — how many addresses were OFFERED, and what
     became of the ones that did not mint — without which every row above is an OUTCOME census over a gate
     with a legitimate declining arm. An artifact older than them prints `-` for each, which is this driver's
     absent-versus-zero rule and is the honest answer: the run did not state them. All nine are lifetime
     counts and may be differenced across two samples of ONE instance, like their neighbours on this list.
     MEASURED WITH THE ROWS, WHICH IS WHY THEY ARE HERE AND NOT ARGUED. `gitlab.com/explore`, TWO FRESH
     BROWSERS (one per case — the frontier is cross-session by design, so consecutive cases in one browser are
     not independent experiments), artifact stamped d18fa92658db25b9f64000ae7a16e10c9103f9da, one run each:
     `epEmitted` 43 and `epPreProgram` 43 — EQUAL IN BOTH — against `endpoints` 43, `replyAsked`/`replyAnswered`
     43/43 and `replyDeclined` 0. So the reply door opened for every address on the surface and answered every
     one, and the surface is the markup door counted back: forced execution contributed ZERO addresses to a
     document that ships `rootPrograms` 33. The same two rows read `deepest` 4 and 7 AGAINST that 33, so
     programs 9 through 33 were never started at all.
     THAT SENTENCE USED TO REACH ITS CONCLUSION THROUGH `start-a-classic-program` — "ran 6 and 8 times, so
     programs 9 through 33 were never started" — AND IS REWRITTEN RATHER THAN DELETED BECAUSE THE CONCLUSION
     IS RIGHT AND THE ROW CANNOT CARRY IT, which CLAUDE.md rates worse than an open question: a reader checks
     the conclusion, finds it holds, and inherits the METHOD. The banner at the head of this file already
     says that row counts starts which were PREEMPTED mid-program and tells its reader to read the
     assignment rather than the name — so the file disagreed with itself twelve paragraphs apart, and the
     paragraph carrying a NUMBER won, because a digit beside a conclusion reads as its evidence.
     WHICH ROW A START LANDS IN IS SELECTED BY `started_here` IN solver/engine.c, so a start is counted in
     whichever of SIX its outcome takes: `start-a-classic-program` (preempted, frame still live),
     `start-ended-its-frame`, `start-reported-an-exception`, `start-detached-its-base`,
     `start-blocked-on-a-host-answer` and `evaluate-a-module-program`. On these two runs the quoted
     `stepUnitRuns` rows sum to `steps` 79 and 285 EXACTLY, so every other row of that histogram is zero and
     the start totals are TOTALS rather than floors: 6+6 = 12 and 8+7 = 15, not 6 and 8.
     READ `deepest` AGAINST `rootPrograms` FOR COVERAGE, and the six rows only when the question is what a
     start DID. RETIREMENT: this correction goes when no reading in this file derives a count of the
     document's own programs from a step-unit row.
     AND `finished` 0 IS THE SAME MISREADING ONE ROW OVER, MADE SINCE FROM THIS VERY LINE — a brief took it
     for "no member ever finishes a program" and built a subject on it. solver/engine.h declares it as flows
     that RAN TO THEIR END (`finishedFlows + finishedCands`), and solver/engine.c decides it only with every
     rung of the ladder empty, which needs the member OUT OF PROGRAMS. A cursor advances only where a row is
     LEFT, and `script-load-failed` is zero on both runs by that same exhaustive sum, so no member ever stood
     past cursor 8 of a sequence at least 33 long. `finished: 0` is therefore ENTAILED by the rows already
     quoted and is not a fourth independent zero — CLAUDE.md's evidence inflation, in an output whose rows a
     reader counts.
     THE ROWS THAT SAY A PROGRAM ENDED ARE `completed` — 3 and 7 here, the highest program run to its END —
     and the frame-end step units `resume-ended-its-frame` / `start-ended-its-frame` /
     `report-an-exception` / `start-reported-an-exception`, summing 5+6 = 11 and 9+7 = 16. Programs END on
     this page in BOTH runs; what no member does is RETIRE, and those are different facts about different
     populations — one about the document's sequence and one about the frontier.
     RETIREMENT: this note goes when this driver prints a program-END count beside `finished`, because the
     entailment is then visible in the output rather than argued here.
     THE TWO RUNS DISAGREE BY 1550x ON `flows` (4 against 6199) AND AGREE EXACTLY ON THE SUBTRACTION, which is
     the only reason one page's reading is worth stating: CLAUDE.md §Testing says a reach total is not
     comparable across two runs of a wall-denominated quantum, and an IDENTITY is. `epEmitted == epPreProgram`
     is an identity; `endpoints: 43` beside it is not, and was the number being quoted before these rows had a
     reader. */
  "epMinted", "epAssets", "epEmitted", "epPreProgram",
  "epAsks", "epAskPreProgram", "epAskSuppressed", "epAskMerged", "epAskMinted"];

/* WHERE THE FRONTIER STOOD, WHAT ITS STEPS DID, AND WHAT GREW IT — read off the row bridge.js wrote, never
   recomputed. `forkAt` is taken WHOLE and is not truncated to its heaviest rows: it is already a Space-Saving
   table with a bounded row count and it publishes its own understatement bound as a member, so a driver
   re-truncating it would hide rows AND drop the bound that says how far the survivors understate. */
function census(r) {
  const o = {};
  o.forkAt = ("forkAt" in r) ? r.forkAt : null;
  const c = ("cold" in r) ? r.cold : null;
  for (const k of CENSUS_LIFETIME) o[k] = c && (k in c) ? c[k] : null;
  for (const k of CENSUS_GAUGE) o[k] = c && (k in c) ? c[k] : null;
  for (const k of COLD_COUNTERS) o[k] = c && (k in c) ? c[k] : null;
  /* THE ORDER'S OWN CENSUS, AND `members: 0` IS NOT A READING. extension/bridge.js states the contract it
     asserts: no `wfq` is a BROKEN CONTRACT, `{members: 0}` is an EMPTY FRONTIER carrying NO term rows at
     all, and a full object is a READING. A finalize document is composed after the frontier drained or parked,
     so `members: 0` is the true reading of that instant and not of the run — its rows are absent, and they
     stay `null` here rather than becoming zeroes, because a frontier that was never observed standing and one
     observed with no backlog are different findings. */
  const w = ("wfq" in r) ? r.wfq : null;
  const live = w && typeof w === "object" && w.members > 0;
  o.wfqMembers = w && typeof w.members === "number" ? w.members : null;
  for (const k of WFQ_JOB_SPLIT) o[k] = live && typeof w[k] === "number" ? w[k] : null;
  for (const k of WFQ_LIFETIME) o[k] = live && typeof w[k] === "number" ? w[k] : null;
  return o;
}

async function oneRun(browser, pg, url, budgetMs) {
  const before = await snapshot(pg);
  const baseRows = before.rows.length;
  /* READ BEFORE THE NAVIGATION, because "the scheduler was already dead when this URL arrived" is the only
     reading under which this row is not about this URL at all. */
  const schedBefore = await scheduler(pg);

  let page;
  const pages = await browser.pages();
  const nonExt = pages.filter((p) => !p.url().startsWith("chrome-extension://") &&
                                     !p.url().startsWith("devtools://"));
  page = nonExt.length ? nonExt[nonExt.length - 1] : await browser.newPage();

  // A page error the ENGINE never sees is still part of what this run met, so it is
  // collected — but separately from the engine's own pageErrors, which ride the result
  // document. Conflating them would report a site's own console noise as engine output.
  const pageConsole = [];
  const onErr = (e) => pageConsole.push("pageerror: " + String(e && e.message || e));
  page.on("pageerror", onErr);

  /* EVERY RUN GETS ITS OWN DOCUMENT, AND THE RUN BEFORE IT IS WHY THAT NEEDS SAYING. A `goto` to the
     URL the tab is ALREADY at, when that URL carries a fragment, is a same-document navigation: no
     request, no response, no new Document, so nothing admits an engine — and the run then reports no
     engine row, which is indistinguishable in the table from a document that was admitted and never
     provisioned. That is the silent-zero-run shape, manufactured by the instrument: three runs of one
     fixture read as crash / nothing / nothing, and the two nothings were the browser declining to
     navigate. Going through about:blank first makes the next goto a real cross-document navigation
     whatever the previous run left in the tab. */
  try { await page.goto("about:blank", { waitUntil: "domcontentloaded", timeout: 15000 }); } catch (e) {}
  let nav = null;
  const t0 = Date.now();
  try {
    const resp = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
    /* A NULL RESPONSE IS NOT A STATUS, AND IT IS NOT A RUN. puppeteer returns null when no navigation
       actually happened, so this is reported under its own token rather than folded into the status
       column — an absent document and a document that produced nothing are different facts. */
    nav = resp ? resp.status() : "nav-noop(no document created)";
  } catch (e) {
    nav = "navfail:" + String(e && e.message || e).split("\n")[0];
  }

  // Poll the row this run owns. `partial` is a snapshot of a run still going; only
  // `complete`/`crashed` are terminal, and a run that never reports at all falls out
  // through the backstop below with its own distinct token.
  let terminal = null, last = before;
  while (Date.now() - t0 < budgetMs) {
    await sleep(2000);
    last = await snapshot(pg);
    const mine = last.rows.slice(baseRows);
    if (mine.length && mine.every((r) => r.run === "complete" || r.run === "crashed")) {
      terminal = mine; break;
    }
  }
  page.off("pageerror", onErr);
  /* READ AFTER THE POLL LOOP for the reason testing/live-why.js states at its own probe read: a scheduler
     that died mid-run must be seen dead, not seen alive one poll before it died. */
  const schedAfter = await scheduler(pg);
  const elapsed = Date.now() - t0;
  const mine = (terminal || last.rows.slice(baseRows));

  return {
    url, nav, elapsedMs: elapsed,
    // ABSENT and ZERO are different facts. No row at all is `rows: 0` with a null
    // outcome — a document that was admitted and never provisioned an engine, which is
    // NOT the same as an engine that ran and found nothing.
    /* THE VERDICT NAMES WHICH OF THE THREE NOTHINGS THIS IS. A run with no engine row is only a finding
       about the ENGINE when a document actually arrived for it to run on; when the navigation itself
       produced nothing, the run measured the browser, not the engine, and says so. */
    /* THE FOURTH NOTHING IS NAMED FIRST, because it is the one that makes the row not a row about this site:
       a scheduler already dead when this URL arrived was never asked about it, so `budget-elapsed(no-row)`
       would be reporting an engine that was never offered the document as an engine that produced nothing. */
    /* AND THE FIFTH, WHICH IS THE FOURTH'S LIVING TWIN AND READ AS A FINDING ABOUT THE ENGINE FOR AS LONG AS
       IT HAD NO NAME. `_hostKick` returns early while `_hostDriving` is true and re-kicks only when the round
       in flight COMPLETES, so a document that arrives mid-round is not refused — it is QUEUED, and a round
       that never completes inside the budget is a round in which this URL is never admitted. The scheduler is
       alive, `kicksRefused` stays 0, and the row that comes back is `budget-elapsed(no-row)`, whose own
       comment says it means "a document that was admitted and never provisioned an engine". That is a
       DIFFERENT nothing, and the difference is the whole reading: one is the engine finding nothing, the other
       is the engine never being asked.
       MEASURED, and it is why this arm exists: six interleaved runs of testing/fixtures/wjp_absent.html at
       artifact 00754e96. Run 0 left the loop driving and never finished inside 90 s; runs 1-5 each reported
       `budget-elapsed(no-row)` with `alive:true, driving:true, kicksRefused:0` before and after — five rows
       about a queue, printed in the same column and the same shape as a finding about the site. The spread
       line called them `runsThatWereSamples: 6/6`, because that denominator asked only whether the scheduler
       was ALIVE, so a driver that takes a run count precisely because one run is not a measurement was
       reporting one sample out of six as six.
       `=== true` AND NOT A TRUTHINESS TEST: `scheduler()` returns `{PROBE_THREW}` when the offscreen is still
       loading, and an ABSENT `driving` is a third fact — this reader could not ask — which must not be
       published as either answer. */
    verdict: schedBefore.alive === false && !mine.length
             ? "scheduler-dead-before-this-run(NOT a sample of this site)"
           : schedBefore.driving === true && !mine.length
             ? "scheduler-busy-before-this-run(queued behind a round still in flight; NOT a sample of this site)"
           : typeof nav !== "number" ? "no-navigation"
           : terminal ? "terminal"
           : (mine.length ? "budget-elapsed(partial)" : "budget-elapsed(no-row)"),
    /* CARRIED ON EVERY ROW, not only the refused ones: a run whose scheduler was alive before and dead after
       is where the death happened, and that is a fact about THIS site. `kicksRefusedDelta` is how many
       documents were answered by nothing while this row was being taken. */
    scheduler: { before: schedBefore, after: schedAfter,
                 kicksRefusedDelta:
                   (typeof schedBefore.kicksRefused === "number" && typeof schedAfter.kicksRefused === "number")
                     ? schedAfter.kicksRefused - schedBefore.kicksRefused : null },
    rows: mine.length,
    outcomes: mine.map((r) => r.run),
    counters: mine.map((r) => {
      const o = { run: r.run };
      for (const k of COUNTERS) o[k] = (k in r) ? r[k] : null;   // null = the crash arm carries none
      /* THE CRASH ARM CARRIES NO COUNTERS AND IT DOES CARRY ITS CAUSE, which is the whole reason a live site
         is worth running: the ROOT @WHY on it names the capability that is missing. Read off the run record
         rather than scraped from a console — the renderer does not tee its stdout, so a console scrape is
         the wrong surface by construction. */
      if (r.run === "crashed") o.err = r.err;
      return o;
    }),
    /* A SEPARATE ARRAY AND NOT MORE KEYS ON THE ROW ABOVE, aligned with it index for index. Every member of
       `counters` is a TOTAL over the run; every member of this is a census, and two of its four are gauges. A
       reader compares WITHIN a kind and never across, and one object holding both invites exactly the
       comparison neither supports — the same reason bridge.js keeps the four censuses as four objects. */
    frontier: mine.map(census),
    storeEndpointsDelta: (last.endpoints === null || before.endpoints === null)
      ? null : last.endpoints - before.endpoints,
    storeFindingsDelta: (last.findings === null || before.findings === null)
      ? null : last.findings - before.findings,
    pageConsole: pageConsole.slice(0, 8),
  };
}

function spread(runs, pick) {
  const vals = runs.map(pick).filter((v) => typeof v === "number");
  if (!vals.length) return "-";                       // absent, not zero
  const lo = Math.min(...vals), hi = Math.max(...vals);
  return lo === hi ? String(lo) : lo + "–" + hi;  // an en dash: a RANGE, never a mean
}

async function main() {
  const runs = parseInt(process.argv[2], 10);
  const urls = process.argv.slice(3);
  if (!runs || !urls.length) {
    console.error("usage: node testing/live-run.js <runs> <url> [url…]");
    process.exit(2);
  }
  const budgetMs = Number(process.env.LIVE_RUN_BUDGET_MS || 90000);
  const stamp = artifactStamp();
  console.log("# artifact " + JSON.stringify(stamp));
  console.log("# runs=" + runs + " budgetMs=" + budgetMs + " (budget is a BACKSTOP, not the verdict)");
  /* THE KIND OF EVERY CENSUS ROW, STATED WHERE THE ROWS ARE FILED UNDER IT — §Testing: a quantity whose kind
     you cannot name FROM ITS OUTPUT is one you are not entitled to do arithmetic on, and the names do not say. */
  console.log("# frontier.* — LIFETIME (may be differenced): " +
              CENSUS_LIFETIME.concat(COLD_COUNTERS).concat(WFQ_LIFETIME).join(",") + ", and every `forkAt` row" +
              " | UNITS: replayHits+replayLeftArms are ARMS (decision-vector slots), replayLeft is EVENTS" +
              " | GAUGES (may FALL; never difference): " +
              CENSUS_GAUGE.concat(WFQ_JOB_SPLIT).concat(["wfqMembers"]).join(","));

  const { browser, extId } = await connect();
  try {
    const pg = await offscreenPage(browser, extId);
    const bySite = new Map();
    for (const url of urls) bySite.set(url, []);
    // Interleave the runs rather than repeating one site N times back to back: a site's
    // own run-to-run drift and a monotone drift in the browser (a growing moat, a warm
    // code cache) are otherwise indistinguishable in the spread.
    /* SAID ONCE, LOUDLY, AT THE INSTANT THE RUN COUNT STOPS MEANING ANYTHING. A driver that takes a RUN COUNT
       does so because §Testing says one run is not a measurement — so the moment the scheduler dies, every
       remaining run is a refused document and the count in the header is a promise this driver can no longer
       keep. Per-row tokens say it too, but they say it once per row in a stream the reader scrolls; this says
       it where the reader is still deciding what the run means. The run is NOT aborted: the remaining rows are
       what a wedged browser does, which is itself worth seeing, and stopping would hide the shape. */
    let announcedDead = false;
    for (let i = 0; i < runs; i++) {
      for (const url of urls) {
        const r = await oneRun(browser, pg, url, budgetMs);
        bySite.get(url).push(r);
        console.log(JSON.stringify(Object.assign({ runIndex: i }, r)));
        if (!announcedDead && r.scheduler.after.alive === false) {
          announcedDead = true;
          console.log("# ── SCHEDULER DEAD from run " + i + " of " + url + ". bridge.js sets `_hostDead` once " +
                      "and never clears it, so every run after this one is a REFUSED document, not a sample of " +
                      "the site on its row. Restart the browser between runs to get independent samples.");
        }
      }
    }
    console.log("\n# ── spread across " + runs + " run(s); a range, never a mean ──");
    for (const [url, rs] of bySite) {
      const first = (pickKey) => (r) => {
        const c = r.counters[0];
        return c && typeof c[pickKey] === "number" ? c[pickKey] : undefined;
      };
      console.log(JSON.stringify({
        url,
        /* THE DENOMINATOR OF EVERY SPREAD ON THIS LINE. §a-coverage-figure-states-what-it-is-a-fraction-of:
           a range across five runs of which four were refused is a range across ONE, and the reader cannot
           see that from the range. */
        /* ASKED OFF THE VERDICT, NOT OFF `alive`, SO THE TWO CANNOT DISAGREE. This read `alive !== false`,
           which is the fourth nothing only — it counted a run queued behind a round still in flight as a
           sample of its site, and that was the commoner of the two on a document whose first run does not
           finish. Deriving it from the verdict a row already carries means a nothing named up there is a
           nothing subtracted down here, with nothing to keep in step by hand. */
        runsThatWereSamples: rs.filter((r) => !/NOT a sample of this site/.test(r.verdict)).length +
                             "/" + rs.length,
        verdicts: rs.map((r) => r.verdict + ":" + (r.outcomes.join("+") || "no-row")),
        endpoints: spread(rs, first("endpoints")),
        sinks: spread(rs, first("sinks")),
        candidates: spread(rs, first("candidates")),
        flows: spread(rs, first("flows")),
        switches: spread(rs, first("switches")),
        jobsQueued: spread(rs, first("jobsQueued")),
        jobsRun: spread(rs, first("jobsRun")),
        unitsDone: spread(rs, first("unitsDone")),
        sourceReads: spread(rs, first("sourceReads")),
        sinkReached: spread(rs, first("sinkReached")),
        sinkTainted: spread(rs, first("sinkTainted")),
        sinkSuppressed: spread(rs, first("sinkSuppressed")),
        park: spread(rs, first("park")),
        storeEndpointsDelta: spread(rs, (r) => r.storeEndpointsDelta),
      }));
    }
  } finally { browser.disconnect(); }
}

main().catch((e) => { console.error(e.stack || e.message || e); process.exit(1); });
