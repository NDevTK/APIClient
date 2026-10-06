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
const { artifactStamp, artifactRowsPresent } = require("./artifact_stamp.js");
const { absentPair } = require("./absent_census.js");
/* WHAT KIND EACH CENSUS ROW IS, ASKED OF THE PRODUCER THAT EMITS IT. The six lists below used to answer
   TWO questions with one array — WHICH ROWS THIS DRIVER CARRIES and WHAT KIND EACH IS — and only the
   first of those is this file's to answer. The curation is a driver's own and is deliberately a SUBSET
   (the `census()` banner refuses to take everything, in as many words); the KIND is a fact only the
   composer can state, and until this it was stated in no artifact at all. */
const { kindsOf, requireWhole, requireFrom } = require("./census_rows.js");

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

/* THE OFFSCREEN DOCUMENT, WAITED FOR AND THEN REFUSED WITH THE STATE IT REFUSED IN — because this threw
   `no offscreen document — is the extension loaded?` and the extension WAS loaded, which is two states
   behind one answer on the one question that decides whether a run happens at all.
   MEASURED: of six runs of a drive over two documents, the FIRST on a brand-new profile died here and the
   five after it did not. A fresh profile has no V8 code cache (`harness.js restart` wipes it deliberately,
   and says why), so the extension's service worker and its offscreen document come up cold exactly once —
   and 60 polls of 200ms is twelve seconds, which was not enough for that one. The run was lost and the
   message blamed the extension. That is §A-MEASUREMENT-THAT-A-LOADED-MACHINE-CAN-FALSIFY: an artifact of
   HOW the run was made, reported as a fact about WHAT ran, and it cost one sixth of a drive's capacity.
   THE WAIT IS A BACKSTOP AND IS GENEROUS, which is what CLAUDE.md §Testing allows a budget to be when the
   real measure cannot see the case — a service worker that never wakes produces no progress of any kind, so
   there is nothing to key the wait on but time. What it may NOT do is report that absence in the same words
   as a missing extension.
   THE TWO STATES ARE SEPARATED BY ASKING WHETHER ANY TARGET OF THIS EXTENSION EXISTS, which is the shipped
   fact rather than a guess: no target under this origin at all is an extension that did not load, and
   targets under it WITHOUT `ast-worker.html` is an offscreen document that has not come up. The second
   message names the targets it DID see, because a reader meeting it needs to know which half of the
   extension is alive. */
async function offscreenPage(browser, extId) {
  const origin = `chrome-extension://${extId}/`;
  const url = `${origin}ast-worker.html`;
  const WAIT_MS = 45000, STEP = 200;
  let seen = [];
  for (let i = 0; i < WAIT_MS / STEP; i++) {
    const ts = browser.targets().filter((t) => t.url().startsWith(origin));
    if (ts.length) seen = ts.map((t) => t.type() + " " + t.url().slice(origin.length));
    const t = ts.find((t) => t.url().startsWith(url));
    if (t) { const pg = await t.page().catch(() => null); if (pg) return pg; }
    await sleep(STEP);
  }
  if (!seen.length)
    throw new Error(`the extension did not load: NO target under ${origin} in ` +
                    `${WAIT_MS / 1000}s. That is this harness or this profile, not the page — ` +
                    `check that --load-extension pointed at extension/ and that the profile is one ` +
                    `the browser's own account can reach.`);
  throw new Error(`the extension loaded and its offscreen document did not come up in ` +
                  `${WAIT_MS / 1000}s. Its other target(s) are alive: ${seen.join("; ")}. This is a ` +
                  `DIFFERENT fact from an extension that never loaded, and on a fresh profile it is ` +
                  `usually the cold V8 cache — re-run, and if it repeats the offscreen document itself ` +
                  `is failing to register.`);
}

/* THE ENGINE'S OWN RECORD OF WHAT THE PAGE THREW, WHICH IS A THIRD CHANNEL AND NOT A SECOND COPY OF EITHER
   OF THE TWO THIS DRIVER ALREADY HAS. `pageConsole` in oneRun is the TAB's own errors, and bridge.js's comment
   at the seam says conflating those with the engine's "would report a site's own console noise as engine
   output"; `counters[].err` is the CRASH ARM's cause and exists only when the renderer died. This is the
   engine's `pageErrors` — offscreen-brain.js's `_recordEnginePageErrors` writes every row of it to the
   offscreen console as `[AST:page-error] <context>: <message>`, and until this line no driver collected it,
   so the surface CLAUDE.md §NO-STUBS calls the forcing function was emitted on every run and read on none.

   IT IS READ OFF THE CONSOLE RATHER THAN OFF THE RESULT DOCUMENT BECAUSE OF WHICH RUNS HAVE ONE. A crashed run
   carries no result document at all, so its `resolverErrors` array does not exist — and that is exactly the
   run whose errors a reader most needs. `_recordEnginePageErrors` runs on EVERY relay including the crash
   arm's, so the console is the one surface that speaks for both outcomes. This is the offscreen document's
   own console and not the renderer's stdout, which §Testing correctly says is not teed and is the wrong
   surface by construction.

   ABSENT, ZERO AND TRUNCATED ARE THREE FACTS. A listener that could not attach yields `null` and prints `-`
   (this driver could not ask); an attached listener that saw nothing yields `[]` (the engine recorded no page
   error, which is a finding about the page); and a buffer at its cap says so in its own token rather than
   reporting a floor as a total. The cap exists because this handler runs on EVERY offscreen console message
   and §Testing's instrument-cost rule applies to a driver as much as to an engine — the substring test is the
   first thing it does, so a message this driver does not want costs one `indexOf`. */
const ENGINE_ERR_TAG = "[AST:page-error]";
const ENGINE_ERR_CAP = 500;
function attachEnginePageErrors(pg) {
  const buf = [];
  try {
    pg.on("console", (m) => {
      let t;
      try { t = m.text(); } catch (e) { return; }
      if (!t || t.indexOf(ENGINE_ERR_TAG) < 0) return;
      if (buf.length >= ENGINE_ERR_CAP) { buf.truncated = true; return; }
      /* THE PRODUCER RENDERS THE ROW, SO ONLY THE TAG AND ITS SEPARATING SPACE COME OFF HERE. THIS USED TO
         STRIP AN UNINTERPOLATED `%s: %s` AS WELL, and that clause is written out rather than merely removed
         because its reasoning was sound and a reader who re-derives it will put it back: a
         `console.debug("%s: %s", a, b)` reaches THIS transport as the format and the args side by side, so the
         specifier pair really did have to go. What it could not do is RECOVER the `: ` -- the two fields
         arrived joined by a space, and `distinctEngineErrors` collapses on exactly that string, so a context
         and a message both containing spaces could not be told apart again by anything downstream. The fix is
         at the producer and the workaround is gone with it; if a `%s` ever appears in a collected row again,
         that is a producer still passing a format and it is now VISIBLE rather than silently flattened. */
      buf.push(t.slice(t.indexOf(ENGINE_ERR_TAG) + ENGINE_ERR_TAG.length).replace(/^\s+/, ""));
    });
  } catch (e) { return null; }      // absent: this driver could not ask, which is not "no errors"
  return buf;
}

/* COLLAPSED TO DISTINCT MESSAGES WITH A COUNT, because ONE event is relayed more than once: a renderer death
   arrives as the root `@WHY`'s own reason AND as the `engine-crash` envelope that quotes it, so a reader
   counting EMISSIONS double-counts a single abort. The count is kept beside each message rather than dropped,
   since "this page threw the same thing forty times" and "this page threw it once" are different facts. */
function distinctEngineErrors(rows) {
  if (rows === null) return null;
  const m = new Map();
  for (const r of rows) m.set(r, (m.get(r) || 0) + 1);
  return [...m.entries()].map(([msg, n]) => ({ n, msg }));
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
 * belong together: `testing/corpus/mirror/gitlab` (a 4.5 MB real bundle, 18 files tracked AT THE TIME) replayed
 * by `testing/corpus/serve-faithful.mjs` at its original host and paths, driven by `engine/pagecensus.mjs`
 * through the production ABI, artifact stamped `f84f671f`, quiet box (load 0.32 rising to 1.03), 51 samples.
 *
 * THAT SUBJECT AND THAT TRANSPORT ARE DELETED AND THE READING IS KEPT WITH ITS INSTRUMENT MARKED GONE, which
 * is the only honest form for it: the capture and `serve-faithful.mjs` were removed because this repository
 * carries no copy of anybody else's site, established by CONTENT and not by ancestry since the clone is
 * SHALLOW — `git cat-file -e origin/main:testing/corpus/serve-faithful.mjs` answers that the path does not
 * exist, and `testing/corpus/run.sh` refuses `AT=frozen` in as many words. So "18 tracked files" was true when
 * written and names nothing today. The figures below are SOUND where they are about the ENGINE and they CANNOT
 * BE RE-TAKEN: a later disagreement with them is not a regression, cannot be bisected, and is not evidence
 * about anything. What makes them worth keeping anyway is that they are a fact about the PRODUCT rather than
 * about one capture, and the companion live reading beside them is re-takeable at any time.
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
/* AND WHY THESE THREE LISTS ARE HAND-KEPT WHEN engine/build.mjs DERIVES ITS @COLD ROW SET, asked and answered
   here because it is the first question a reader of them has and the answer is not the one the shape suggests.
   `coldFields()` solves a DIFFERENT problem: it derives PRESENCE — which rows the composer publishes — from
   `result_cold_json`'s own format string. These lists are this driver's CURATION — which rows it prints — and
   nothing else; `census_rows.js` reads the KIND off the producer that emits each row and this file's own header
   line is composed from that.
   THE CLAIM THAT STOOD HERE IS RETIRED AND IS KEPT IN ITS OWN WORDS BECAUSE IT IS THE ONE A READER RE-DERIVES
   FROM THESE LISTS' SHAPE. It said `These lists declare KIND — which rows may be DIFFERENCED — and no artifact
   in this tree states a @COLD row's kind at all`, with a retirement naming the diff that would change it:
   result.c stating each row's kind beside it, after which a consumer derives instead of listing. BOTH HALVES OF
   THAT ARE MET — result.c carries an `@kinds-of cold` block, rung_entry.c and endpoint.c carry their own, and
   `kindsOf` refuses an undeclared row outright — so a reader who finds the old sentence and concludes a kind
   list is still owed here would be re-adding the second copy that mechanism ended.
   WHAT IS STILL HAND-KEPT IS THE CURATION, WHICH IS THIS DRIVER'S OWN FACT AND MAY NOT BE DERIVED: `census()`
   refuses to take everything in as many words, and a driver that took every row would be a second copy of the
   popup. WHAT WAS SILENT UNTIL `requireWhole` IS THE ONE DIRECTION CURATION CANNOT EXCUSE — a composer whose
   rows the producer says are ONE READING, carried in part. See COLD_WHOLE below for the measured drift that
   put it there.
   AND THE DERIVATION COULD NOT REACH THE `epFetch*` ROWS EVEN FOR PRESENCE, WHICH IS WORTH KNOWING BEFORE
   ANYBODY REACHES FOR IT. They are composed by solver/endpoint.c and spliced into result_cold_json through a
   BARE `%s` that names no row, so `censusComposerFields`' `\"name\":%` match cannot see them — and neither can
   `censusRowSet`'s object check, since an unnamed splice lands in neither the numeric list nor the object one
   and the extra/gone mismatch never fires. MEASURED at 468e06ee over that composer's own region: 123 named
   rows as the armed control, ZERO of them named `epFetch`, one bare `%s`. So the five are in build.mjs's
   unchecked half BY DEFAULT — which is the hole `censusRowSet`'s banner exists to end, arriving one splice
   over — and this driver asking for them by name is the only thing that reads them anywhere.
   WHAT THE PRODUCER DID INSTEAD IS SPELL THE KIND INTO THE NAME (`Life`), which is checkable by eye and covers
   five rows of the fifty-odd lifetime rows on this line; a suffix rule standing beside these lists would be
   two mechanisms answering one question with the partial one drifting, which is the second copy this file's
   own `absentPair` reader exists to avoid.
   RETIREMENT: this note goes when a row's CURATION can be stated at the producer too — a composer declaring
   which of its rows are a reading a consumer may not take in part — because the only hand-kept fact left here
   is then derivable and the question this note answers can be asked of the producer entire. */
/* THE DENOMINATOR OF THE LADDER'S THREE INVOKER ARMS, FIRST ON THE LIST BECAUSE IT IS WHAT THEIR ZERO IS A
   FRACTION OF. `stepUnitRuns`' rendering, timer and idle arms are `else if`s inside flow_step, so REACHING ONE
   IS RUNNING and a zero there stood for two states that take opposite work: the document hangs nothing off that
   rung, or the ladder never gave the rung a turn. solver/rung_entry.h states why no counter INSIDE the ladder
   can ever separate them — the ask has to be recorded upstream of every arm that may decline, and reach is such
   an arm — so these are raised where the COMPILER resolves a free identifier against the global object.
   READ AS A BIT AND NEVER AS A MAGNITUDE. A program is recompiled by every flow that replays it, so these count
   COMPILER RESOLUTIONS and not source sites; the producer names them `Named` rather than `Sites` for that
   reason. NO INEQUALITY AGAINST THE RUNS ARM HOLDS IN EITHER DIRECTION and the producer asserts none: one
   `setInterval` feeds the timer arm for ever (runs exceed names) and a rung the ladder never reached runs zero
   times against any number of names (names exceed runs, which is the finding).
   THE `…Typeof…` HALF IS THE DISCRIMINATOR AND NOT A SECOND OBSERVATION. The unary parser patches an ordinary
   read into the non-throwing form only for `typeof` (ECMAScript §13.5.3 "The typeof Operator" step 2.a), so a
   bundle that merely PROBES for a name raises that row and not its partner — which is what keeps a nonzero
   denominator from being read as work the document actually asked for. Read the pair, never either alone.
   AND THE THIRD ROW PER RUNG IS THE PROPERTY SPELLING, WHICH IS NOT A SECOND READING OF THE PAIR AND MAY NOT
   BE SUMMED INTO EITHER HALF OF IT. `window.requestIdleCallback` is a FIELD GET of the global, and the guard the
   pair above is split on does not exist for one: ECMAScript §13.5.3 "The typeof Operator" step 2.a needs a
   non-throwing read only for an unresolvable REFERENCE, so `typeof window.x` emits the same field get as
   `window.x` and the producer raises a THIRD count rather than choosing an arm. Reporting it into the `…Life`
   row would merge a population that is largely FEATURE DETECTION into the row read as work the document asked
   for, on the rung where that matters most, since a name a bundle probes is by construction a name that is not
   universally present. A reader who wants the union of the spellings adds two of these; nobody here is handed a
   sum somebody else took, which is why the column is carried and not folded.
   THE PROPERTY ROW IS ALSO WHERE A POLYFILL LIVES, which is what makes a rung's three-row ladder readable: a
   bundle that assigns the global it probes writes the property spelling, so a rung whose `…Life` and
   `…TypeofLife` both read 0 while `…PropLife` does not is a document that reached the rung's work through a
   shim rather than one that named nothing.
   AN ARTIFACT OLDER THAN THESE ROWS PRINTS `-` FOR EVERY ONE OF THEM, this driver's absent-versus-zero rule:
   the producer's absent form is the rows being ABSENT from `_cold` — a host whose realms install none of the
   three components has no population — so `k in c` is false and this list yields `null`, with no arm that could
   turn that into a 0. A ROW READING 0 AND A ROW READING `-` ARE DIFFERENT FACTS and only the first is about the
   run. THAT IS ALSO WHY NO PRESENCE ASSERT STANDS AT THE READ and its absence is not an omission: requiring
   these rows of a record would convert an artifact older than them into a crash, which is the one distinction
   this list is most careful about. What IS asserted is the ROW SET, at `requireWhole` below, which is a fact
   this tree computes from its own source rather than one an engine stated.
   THEY ARE NUMBERS ON A LIST WHOSE OTHER TWO MEMBERS ARE HISTOGRAMS, which is safe because nothing spreads it:
   `census()` copies every member of COLD_ROWS with one uniform `k in c` read. Their KIND is not this list's
   statement and is no longer guessed from the `Life` suffix — `census_rows.js` reads it off each producer's own
   `@kinds-of` declaration, and this driver's header line prints what the producer says. */
const COLD_STEP_UNITS = ["stepNamedRenderingLife", "stepNamedRenderingTypeofLife", "stepNamedRenderingPropLife",
                         "stepNamedTimerLife", "stepNamedTimerTypeofLife", "stepNamedTimerPropLife",
                         "stepNamedIdleLife", "stepNamedIdleTypeofLife", "stepNamedIdlePropLife",
                         "stepUnitRuns", "stepUnitOverruns"];
/* AND WHICH COMPOSER THE NINE ROWS ABOVE ARE THE WHOLE OF, WHICH IS THE ONLY THING THAT MAKES THEIR NUMBER
   CHECKABLE BY ANYTHING. `rung_entry_rows` publishes exactly the three-row ladder per rung and nothing else, and
   its own emitter says the three are read together — "a document whose three rows for one rung all read zero
   while that rung's runs are nonzero" is the finding, which is a statement about a TRIPLE and not about a row.
   So this driver takes that composer WHOLE, declares that it does, and `census_rows.js` refuses the day the
   composer grows a row this list does not carry.
   MEASURED, WHICH IS WHY THE DECLARATION EXISTS AND IS NOT A TIDY-UP: at 56206ade this list named SIX of that
   composer's NINE rows, omitting `stepNamedRenderingPropLife`, `stepNamedTimerPropLife` and
   `stepNamedIdlePropLife` — one whole VARIANT across all three families, which is an emitter that grew and a
   hand list that did not, never a curation anybody made. Nothing could have said so: the rows are spliced into
   `_cold` under no name of their own, so `build.mjs`'s presence loop cannot require them, `census_rows.js`
   required their KIND and not their carriage, and the banner above this list asserted `ALL SIX` in prose while
   the producer emitted nine. The omission is stated here rather than only fixed, because the next reader of a
   six-of-nine list has no way to tell it from a choice.
   IT IS THE COMPOSER KEY AND NEVER THE REGION, so where `rung_entry_rows` lives stays stated in exactly one
   place. A path and two literals repeated here would be the second copy `census_rows.js` exists to end — and
   the names above stay SPELLED for a measured reason, recorded at `requireWhole`: a list replaced by a call
   that returns the composer's rows is a list no static reader can see, and the record-field audit's
   WRITE-with-no-reader accusation over this composer then rises from four rows to nine. */
/* AND THE TWO EDGE COMPOSERS ARE TAKEN WHOLE TOO AND WERE NOT DECLARED, WHICH IS THE STATE `rungEntry` WAS IN
   AT 56206ade AND WHICH NOTHING COULD HAVE TOLD FROM A CURATION. A subset is not a defect and `requireWhole`
   says so in as many words — this driver carries 58 of `cold`'s 142 rows BY DESIGN. What is a defect is a
   subset nobody declared, and a COMPLETE list nobody declared is that defect waiting for the emitter to grow:
   the day either edge publishes a row this list does not carry, a driver that never declared the composer
   whole prints a smaller population and nothing anywhere says so, which is the silent direction of
   CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS and reads as progress.
   THE WARRANT IS THE PRODUCER'S AND IS NOT THIS DRIVER'S OPINION OF ITS OWN COMPLETENESS, which is the only
   ground on which a whole declaration may be made: solver/endpoint.h states, for each edge, identities over
   its own rows that solver/endpoint.c asserts at the accessor where every term is in one hand. For the fetch
   edge, "THE THREE IDENTITIES" — a PARTITION (the stage arms plus the freed-and-offered row equal the freed
   row), freed-and-offered <= offered, and offered <= the ask total. For the XHR edge, the same partition over
   its placed row plus the one containment, with endpoint.h stating why the second containment is NOT
   assertable there — a step state is byte-copied at a deep fork and `XHR_SEND_DECL` declares no guard, so two
   copies would file against one placement. A row inside an identity is a row whose omission makes its
   siblings unreadable rather than merely absent, and the stage histograms are the arms of one outcome that
   SUM to the freed row: carrying a partition in part publishes arms that no longer add up.
   AND THE `…NamedTypeofLife` HALF CARRIES NO IDENTITY AND IS STILL NOT A ROW THIS LIST MAY DROP, which is
   stated because it is the one pair a reader would check and find unasserted. endpoint.h says those are the
   only ask rows in that file with no relation over them in either direction — a program is recompiled by every
   flow that replays it, so the row counts COMPILER RESOLUTIONS — and the split exists so that a bundle which
   merely PROBES for a name is not merged into the population read as uses. The pair IS the discriminator, so
   either half alone re-merges exactly what the split was built to separate.
   MEASURED, AND THE DECLARATION IS GREEN THE DAY IT LANDS, which is what makes it a forcing function rather
   than a red gate: the derivation is `testing/census_rows.js`'s own `composerRowsFromText` over each
   composer's emission, and reconciled against this driver's `COLD_ROWS` it answers 8 of 8 for `fetchEdge` and
   9 of 9 for `xhrEdge`, carried, with nothing published-and-missing in either direction. The controls were
   run and SPOKE, because a check that has never refused anything is not a check: `cold` and `wfq` declared
   whole each throw with the rows they publish and this driver curates away, a composer name that does not
   exist throws, and dropping ONE published row of either edge throws NAMING THAT ROW.
   WHAT THIS IS NOT, AND THE ALTERNATIVE IS REFUTED BY MEASUREMENT RATHER THAN BY PREFERENCE: it is not a
   DERIVATION of the row lists above. Replacing a spelled list with a call returning the composer's rows reads
   as the stronger fix and `requireWhole`'s own record measures it in both arms at 56206ade — the names then
   appear in no construct anywhere, `engine/fieldgate.mjs`'s holder band has nothing left to see, and that
   file's WRITE-with-no-reader accusation over one composer rises from four rows to nine. The names stay
   SPELLED and the completeness becomes an ASSERT, which is CLAUDE.md
   §THE-COROLLARY-IS-THE-MORE-USEFUL-HALF: one added total covers every future spelling of the question
   where a rewritten reader covers only the one somebody happened to find.
   RETIREMENT: this note goes when a composer declares its own rows ONE READING at the emitter — a machine-read
   line beside `@kinds-of` saying which of its rows no consumer may take in part — because the three keys below
   are then derived from the producers that know, and which composer is whole stops being a fact a consumer
   states about itself. */
const COLD_WHOLE = ["rungEntry", "fetchEdge", "xhrEdge"];
/* …AND THE REPLY DOOR'S ONE LEVEL, FILED WITH THE GAUGES AND NOT WITH ITS OWN THREE SIBLINGS, which is the
   whole reason this driver splits the two lists: `replyOutstanding` is the count of records the host may still
   be shown AT THE INSTANT the census was composed, so it may FALL and differencing it reads a level as a rate.
   Its three siblings are lifetime counts and sit in COLD_COUNTERS below. They only mean anything read
   together — the identity is `replyAsked == replyAnswered + replyDeclined + replyDropped + replyOutstanding`,
   which solver/result.c asserts at the instant all five are in one hand and which holds on NO pair of lines. */
/* AND THE LIVE HALF OF THE BUNDLE'S OWN DEBT, WHICH IS THE ROW THAT SEPARATES THE TWO READINGS OF A RUN THAT
   LEARNED NOTHING. solver/result.c states the pair in its own words: `17` beside `0` is a bundle that arrived
   WHOLE, so a run that never reached its later programs is the ORDER failing; `17` beside `17` is a bundle
   whose bytes never came, which is the fetch path and the reply door. Those take opposite work and NOTHING
   ELSE ON THIS ROW SEPARATED THEM — this driver carried neither half, so the question had to be asked of
   `self._engineLog` by hand.
   IT IS A GAUGE AND ITS PARTNER BELOW IS NOT, which is the whole reason they are filed apart: this is summed
   per MEMBER at the instant the census was composed, so a fork copies its parent's rows into the count and a
   sold member takes its rows out of it — it may FALL, and no inequality against the seed's arm holds in
   either direction. */
/* AND THE POPULATION `flow_step`'s LADDER CANNOT REACH AT ALL, WHICH IS THE ROW THAT MAKES `orphansAsked`'s
   ZERO READABLE AND WHICH THIS DRIVER CARRIED NEITHER HALF OF. solver/engine.h states the split in its own
   words: that zero with `unframedStepsLifetime` 0 says the ladder was never descended, so the cause is
   UPSTREAM of every arm in it, and the same zero with it LARGE says the ladder WAS descended and an arm ABOVE
   the orphan rung took every descent. Those are different files to open, and solver/result.c names the row
   that tells them apart — zero orphan asks otherwise "reads identically for `nobody has run out of programs`
   and for `members have and something else is due`".
   THIS LIST CARRIED THE ROW BEFORE IT AND THE ROW AFTER IT IN THE COMPOSER'S OWN FORMAT STRING AND NOT THE
   ONE BETWEEN THEM, WHICH IS MEASURED AND NOT ASSERTED: an archived 19-site corpus drive taken with this
   driver publishes `unframedStepsLifetime` and `programCursors` on its own key line and no `outOfPrograms`
   anywhere, so FOUR of its six attributed engine rows read `orphansAsked` 0 with nothing on the line that
   could say which of the two readings it was.
   AND THE DERIVATION A READER REACHES FOR INSTEAD IS FORBIDDEN BY THE PRODUCER, which is why carrying the row
   is not a convenience. `programCursors`' top bucket against `rootPrograms` is NOT this number: one cursor
   value covers a member INSIDE the program at that index and a member PAST THE LAST ROW of its own sequence,
   and `dyn_n` is PER-FLOW and crosses no boundary — solver/result.c says a member at that bucket "may have
   sixteen chunk rows still in front of it; read the old way it looks like a document that finished".
   `live` TRAVELS WITH IT AND `wfqMembers` IS NOT ITS DENOMINATOR. The quantity is `live - outOfPrograms` and
   both halves are ONE walk at ONE instant (solver/cold.c raises `out->flows++` at the top of the loop and
   `out->out_of_programs++` inside the same body); `wfqMembers` is a DIFFERENT walk's count taken at whichever
   entry `wfqFrom` names, so subtracting from THAT is the two-moments defect and no arm here does it.
   BOTH ARE GAUGES and are filed here for `programCursors`' reason exactly: they are summed per MEMBER at the
   instant the census was composed, so a fork copies its parent's state into the count and a sold member takes
   it out. READ THEM WITH `stepUnitRuns.finished`, NEVER ALONE — a census taken when `live` is 0 reports 0
   whatever every member did before it left, which is the same 0 a frontier that never forked reports.
   THE THREE-ARM PARTITION UNDER THE TOTAL IS DERIVED IN `census` AND IS NOT ON THIS LIST, which is where a
   reader will look for it. It says WHICH of three things holds an out-of-programs member — never dispatched,
   framed, or standing at the ladder — and separates "the pick has not returned to it" from "a rung above the
   orphan seed takes it every round"; it is taken by PREFIX off the composer's own object so that a fourth arm
   added to cold.c's if/else chain is carried the day it lands, which a hand-typed list here would not be.
   `outOfProgramsAtTheLadderUnits` IS A NAMED ROW AND SITS ON THE LIST, because it is an OBJECT: the numeric
   prefix derivation excludes it by construction, exactly as `testing/corpus/site.mjs` excludes it, and a row
   excluded from a derivation is a row that has to be named somewhere or it is carried by nobody.
   AND THE DISTANCE `finished` IS A DISTANCE TO, WHICH NOTHING PUBLISHED IS DERIVABLE INTO AND WHICH NEITHER
   REAL-SITE DRIVER HAS EVER CARRIED. solver/cold.h states it in those words: a member retires through the
   terminal arm of flow_step's ladder, the whole ladder sits below the block that starts the next row, so the
   precondition for ANY of it is `script_i == dyn_n` — which is this histogram's BUCKET 0 and is
   `outOfPrograms` above. Every other row on this line answers about `script_i` ALONE: `programCursors` is its
   distribution and `deepest`/`deepestLeft`/`completed` are global maxima over it, while `dyn_n` is PER-FLOW
   and appears in none of them, so two frontiers standing at ONE cursor with one row left and with forty rows
   left render as the same bytes everywhere else here. That is the pair CLAUDE.md
   §A-FIXTURE-BUILT-TO-EXERCISE-EVERY-MECHANISM is about: `engine/build.mjs` reads this row and asserts its
   bucket 0 against the total, so the discriminator existed and was read only on a document whose program
   depth its own author chose.
   MEASURED, WHICH IS WHY IT IS HERE AND NOT ARGUED. One 540-second fresh-browser drive of gitlab.com/explore
   read `finished` 0 LIFETIME with `outOfPrograms` 0 and `programCursors` massing 18348 of 18495 members at
   cursor 7 of `rootPrograms` 38 — so no member was at the ladder, every member had rows left, and whether the
   frontier was ONE row from its first retirement or THIRTY-ONE was unstatable from the whole census line.
   A PARTITION AND A GAUGE, like the two rows it is filed with: every live member stands at exactly one
   distance, so the buckets sum to `live`, and the set is DENSE over [0, the greatest distance any standing
   member is at] because the ZEROES are the signal. It is never `{}` — cold.c gives an empty frontier distance
   0 — and an artifact older than the row prints `null`, which is this driver's absent-versus-zero rule. */
/* AND THE ROW THE ENGINE ITSELF SAYS TO READ BEFORE SPENDING A LANE ON ANY ARM OF THAT LADDER, which no
   real-site driver carried. solver/engine.c, at the arm a reader of a frozen `deepest` reaches for first:
   "Read `stepUnitRuns` beside @COLD's `live`/`framed` before spending a lane here: those two converge to
   within ten members at every census of every run measured, and the whole ladder below `if (!f->frame)` is
   unreachable for the population they name, so NO ORDERING OF THE ARMS INSIDE IT CAN BE ITS FIRST CAUSE."
   This driver carried `live` and not `framed`, so the one comparison that sentence prescribes was unmakeable
   from a real page — and every arm-by-arm reading taken with it was a reading of a ladder whose reachability
   was unstated.
   WHY IT DECIDES THE QUESTION `programsAhead` OPENS: the ladder is where a member's NEXT program row is
   started, and it sits inside `if (!f->frame)`. A fork is BORN FRAMED — engine.h: "an arm is born holding the
   frame taken AT its branch, so it is inside a program by construction" — so `framed` near `live` is not a
   population to shed but the design running, and a member cannot reach its next row until the program at its
   current one ENDS. That is why a frontier can hold tens of thousands of members 31 rows from retirement with
   the start arm's own precondition perfectly satisfiable: what is unsatisfied is the precondition for ASKING
   it.
   THE CONVERGENCE CLAIM IS THE ENGINE'S AND IS A CLAIM TO CHECK, NOT AN AXIOM. "every census of every run
   measured" is a statement about the runs its author had, and CLAUDE.md
   §A-FIXTURE-BUILT-TO-EXERCISE-EVERY-MECHANISM is exactly about a proportion whose denominator a fixture's own
   design set. A real document is the check, and carrying the row is what makes it one.
   A GAUGE, like `live` and `outOfPrograms` beside it, and COMPOUND BY DESIGN — result.c refuses to split it,
   because "the first is the park's re-execution COST … a pager pays for a member whichever population it
   belongs to, so a split would be a row no consumer could state anything new from." An artifact older than it
   prints `null`, this driver's absent-versus-zero rule. */
/* AND THE THREE ROWS OF THIS SAME @kind DECLARATION THAT NO LIVE DRIVER ASKED FOR, which is why the one
   question a real-page drive keeps arriving at could not be answered from a drive. `programsAhead` below is
   carried; `dynBodies`, `dynKiB` and `sharedKiB` are declared on the SAME line of solver/result.c and were
   carried by nothing in testing/ — read only by engine/build.mjs, which reports on a FIXTURE. So the rows were
   in the shipped artifact the whole time and absent from every real-site log, and those are different facts:
   CLAUDE.md §AND-THE-IDENTITY-IS-CHECKED-PER-COUNTER's install against finding, settled here by asking the
   artifact rather than by inferring from the logs — all three read PRESENT in the installed wasm with an
   invented name as the armed control, so nothing had to be built to begin asking for them.
   WHAT THEY SEPARATE, AND IT IS NOT A MAGNITUDE. A drive that reports `completed` and `deepest` standing at a
   document program well short of `rootPrograms` has TWO readings a reader cannot tell apart from those rows:
   the engine reached that cursor and stalled, or the cursor over-reads and the stall is EARLIER because fewer
   program BODIES were ever compiled than the cursor implies. `dynBodies` is a count of the bodies the engine
   HOLDS, so it is upstream of any cursor and answers which of the two it is. The same disagreement arrives one
   row over whenever a free-identifier hook reads low against a bundle's own spelling count — a hook that
   under-counts and a document whose programs never compiled are the same two readings again.
   ALL THREE AND NOT `dynBodies` ALONE, because solver/result.c prices `dynKiB` WITH the shared half at its own
   site, so a `dynKiB` carried without `sharedKiB` is a numerator whose denominator this driver would not
   print — CLAUDE.md §THE-TELL-IS-THAT-YOUR-METRIC-IS-A-FRACTION. They are GAUGES, which this driver takes from
   the producer's declaration rather than from their position here, so none of them may be differenced.
   RETIREMENT: this record goes when a row a composer declares and no driver in testing/ carries is a build
   failure, because the gap is then closed by construction and no reader has to notice a fourth name on a
   three-name line. */
const COLD_FRONTIER = ["stepUnits", "programCursors", "replyOutstanding", "rowsAwaitingBytes",
                       "live", "framed", "outOfPrograms", "outOfProgramsAtTheLadderUnits", "programsAhead",
                       "dynBodies", "dynKiB", "sharedKiB"];
/* …AND THE CONSTANT IT IS READ AGAINST, WHICH IS NEITHER OF THE TWO KINDS EVERY OTHER LIST HERE STATES.
   solver/engine.c writes both arms at the ONE line `rootPrograms` is written and never again, because the pair
   is a DENOMINATOR — a fact about what the DOCUMENT owed the reply door when its rows were laid down — so it
   is not a GAUGE, which states what is true now, and not a LIFETIME COUNT, which may be differenced across
   samples. A constant that decreases is a broken seed rather than progress.
   IT HAS ITS OWN LIST BECAUSE FILING IT UNDER EITHER EXISTING KIND WOULD PRINT A FALSE KIND STATEMENT, and
   the header line is where a reader learns what they may do with a number. Putting these in COLD_COUNTERS
   would say LIFETIME, may be differenced — of a constant — which is the §a-quantity-whose-kind-you-cannot-name
   defect committed by the very line that exists to prevent it.
   MEASURED, WHICH IS WHY THEY ARE HERE AND NOT ARGUED. The misreading engine.c names — `…Awaited 17` read as
   "seventeen are STILL owed" — was relayed as a live finding about a real page (`25 of 33 program rows were
   <script src> whose bytes never arrived`) and sent a lane to the reply seam. One fresh-browser run of
   gitlab.com/explore at artifact d17472ff read `rootProgramsAwaitedAtSeed 28` beside `rowsAwaitingBytes 0`,
   with `replyAsked 45 == replyAnswered 45` and `replyOutstanding 0`: the bundle arrived whole and the
   frontier stood at cursor 7 of `rootPrograms 35`. Both halves were on the engine's census the whole time and
   neither was on this driver's row.
   RETIREMENT: this list goes when result.c states each @COLD row's KIND beside it and the three lists here are
   derived from that, which is the same condition the note above COLD_STEP_UNITS already carries — a third
   hand-kept list is a third copy of a fact only the producer's header states, and adding one is what makes
   that condition worth more rather than less. */
const COLD_SEED = ["rootProgramsHeldAtSeed", "rootProgramsAwaitedAtSeed"];
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
const WFQ_PICKS = ["unframedPicksLifetime"];
/* …AND WHAT THE LADDER DID WITH THOSE DISPATCHES, WHICH IS THE ANSWER THE ROW ABOVE HANDS OFF AND THIS DRIVER
   HAS NEVER CARRIED. `unframedPicksLifetime` establishes that the order DOES hand the thread to a member with
   an empty execution context stack; `jobsReadyTask` beside it establishes that such members are holding tasks.
   Between those two and a `run-a-task` that is a few per cent of a document's steps there was exactly one
   unmeasured step, and it is the one that decides the diff: WHICH ARM took the step instead. These four are
   that, raised at the two arms of flow_step that stand above the task arm and can be reached with a task
   runnable, plus the task arm's own two reasons — solver/engine.h states which repair each of the four sizes.
   A SEPARATE SET FROM `WFQ_PICKS` FOR THE REASON THAT ONE IS SEPARATE FROM `WFQ_JOB_SPLIT`: one group per
   question, and the question here is not who waits or who was dispatched but what the ladder declined. They
   are LIFETIME counts like their neighbour and are filed with it rather than with the gauges above, so a
   series of one that decreases is the free tell that the filing is wrong.
   THE READING NEEDS `run-a-task` FROM THE @COLD LINE, which this driver already carries in `stepUnitRuns`, and
   the last two of these PARTITION it — solver/engine.c asserts that sum where both halves and the histogram
   are in one hand, so a reader who finds them not adding up on this document has found a second writer rather
   than a scheduler fact. MEASURED before they existed, which is why they are here: one real application page
   read `jobsReadyTask` 36169 with `jobsReadyMicro` 0 and `run-a-task` 163 of 6673 steps, and nothing in the
   emitted record could say whether the queue was behind the program sequence or behind the reply-delivery arm
   — two mechanisms, opposite diffs, one number. */
const WFQ_LADDER = ["taskHeldDelivLifetime", "taskHeldSeqLifetime",
                    "taskArmOlderLifetime", "taskArmNoRowLifetime"];
/* THE OWED-GLOBALS PAIR, NAMED HERE SO THE KIND LINE CANNOT DRIFT FROM THE ROWS. Both are LIFETIME
   counts over the agent's life — solver/absent.c raises them per read and zeroes them only when the
   agent goes — so they are differenceable, unlike the gauges beside them. The names are this list and
   the reading is testing/absent_census.js's; a row added to one and not the other would print a kind
   statement that does not cover it, which is the §a-quantity-whose-kind-you-cannot-name defect wearing
   a header. */
const ABSENT_ROWS = ["absentAsked", "absentOwed"];
/* THE TWO CURATED SETS, AND THE KIND OF EVERY MEMBER TAKEN FROM THE COMPOSER RATHER THAN FROM THEIR NAMES.
   The lists above are this driver's CURATION — which rows it carries and why, one group per question they
   answer — and that is the half no derivation may take over: `result_cold_json` publishes 124 rows against
   the 56 here and `result_wfq_json` 107 against 9, and a driver that took everything would be a second
   copy of the popup. What is derived is the KIND, which is the half that was a second copy: four of these
   arrays were NAMED for a kind, so each was a statement about the producer kept by hand at a consumer, and
   the row that made the case landed with a kind stated in no artifact anywhere. They are named for their
   SUBJECT now, and `census_rows.js` reads the kind off the composer that emits the row.
   A CARRIED ROW WITH NO DECLARED KIND THROWS AT STARTUP AND NAMES ITSELF, which is the whole gain: adding
   a row here without stating its kind at the producer is a loud failure where it used to be a silent one.
   `members` IS THE COMPOSER'S SPELLING AND `wfqMembers` IS THIS DRIVER'S, which is why the alias is stated
   rather than guessed either way round — the kind is asked under the name the producer emits and the
   header prints the name a reader will meet in the output. */
/* FORKS TAKEN OVER A SUBJECT THE FLOW HAD ALREADY PROVED, READ BESIDE THE FORK TOTAL IT IS A PART OF. §Solver-
   half's CONCRETIZE-ON-PIN says a branch over a source this flow's own equality determined "is decided by
   RUNNING the real predicate on a real string and does not fork at all" — true of the two pin MINT arms, whose
   bare primitive never reaches a branch hook, and NOT of a source the page materialised before its gate, which
   keeps its record and arrives at the hook with a singleton domain and one arm the run itself contradicted.
   IT IS A POPULATION SIZE AND NOT A DEFECT COUNT, carried here because it is the reachability witness for the
   refusal it precedes: a guard whose population is empty is the stub §NO-STUBS forbids, and this driver is the
   one reader that drives REAL documents rather than a fixture whose fork shape its own author chose.
   READ AS A FRACTION OF `forks` OR NOT AT ALL, which is why the pair is named in one place: the engine asserts
   `forkOverPinned <= forks` where both are in one hand, and a bare numerator says nothing about whether any
   spellable branch was reached at all. */
const COLD_FORK_PINNED = ["forkOverPinned"];
/* THE ORPHAN WALK'S ORDER WITNESS. Its denominator is `_orphansDriven`, which is NOT a `_cold` row — it sits on
   the document beside `_wfq` — so this driver reads the pair across two keys and the ONE place they are asserted
   against each other is the composer that emits them. Zero here beside a nonzero `epFetchAskNamedLife` is not a
   broken order: it says the bundle's `fetch` occurrences are in PROGRAM bodies, which that walk skips. */
const COLD_ORPHAN_ORDER = ["orphanPreferred", "orphanScripts"];
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
     non-overrunning turn's time is `stepUnitOverruns` in COLD_STEP_UNITS above — the per-arm histogram
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
  /* AND THE ONE THING `sliceOverruns` AND `stepUnitOverruns` TOGETHER CANNOT SAY, WHICH IS WHAT THOSE TURNS
     WERE INSIDE. An arm is where a step ENDED, and `resume-program` and `start-a-classic-program` both end
     inside one call whether the time went into the page's bytecode between two of its own raise points or
     into a single native call that never returned — and those take opposite work, one being a page's own
     choice that §NO BOUNDS forbids capping and the other a step-machine conversion (§C-stack) in whichever
     component owns the call. `sliceOverrunSeamless` is how many of the overrunning turns offered NOT ONE
     suspend point and `sliceOverrunAsks` is how many the rest offered between them; read as a pair, because a
     sum can be carried by one chatty turn. An artifact older than them prints `-`, which is this driver's
     absent-versus-zero rule and is the honest answer — the run did not state them. */
  "sliceOverrunAsks", "sliceOverrunSeamless",
  /* AND THE OTHER TWO THIRDS OF THE TURN, WITHOUT WHICH `sliceUs` IS A NUMERATOR WHOSE TOTAL THIS DRIVER
     NEVER PRINTED. solver/engine.h splits a turn into the STEP (`sliceUs`) and EVERYTHING ELSE (`schedUs` —
     the pick, the context switch, the delta swap and the previous iteration's tail), and states that they are
     TWO ROWS AND NOT A SUBTRACTION precisely so a reader ADDS rather than infers: `sliceUs + schedUs ==
     stepUs` is asserted at engine_step_unit_runs where all three are in one hand, so carrying all three makes
     that identity checkable FROM THIS DRIVER'S OWN OUTPUT rather than on trust.
     THE PAIR IS WHAT SEPARATES THE TWO DIAGNOSES A LARGE `sliceUs` INVITES, and they are in different
     components: a turn whose STEP dominates is the cooperative quantum with nothing to expire it mid-call,
     which is solver/quantum.h's transport; a turn whose PICK and SWAP dominate is the ORDERING and the COW
     delta costing more than the work they order, which is the frontier's own shape and an O(members) walk at
     every ask. This driver carried `sliceUs` alone, so every reading it has ever produced was consistent with
     both and could refute neither — the engine has computed `schedUs` on every census of every run and no
     tracked driver in this tree has ever printed it. */
  "stepUs", "schedUs",
  /* …AND THE SPAN ALL THREE ARE SHARES OF, PLUS THE DENOMINATOR `sliceUs` ACTUALLY HAS — the rows that turn
     the three accumulators above from a RATIO BETWEEN THEMSELVES into a share of the thread. The banner above
     reads `schedUs` against `stepUs` and separates the ORDERING from the QUANTUM, which is a real split and is
     silent about how much of the instance either of them is: `stepUs` is "the share of the engine's thread
     that reached a dispatch turn" ONLY against `instanceUs`, which solver/engine.c says in those words at the
     assert, and `instanceUs` was on no real-site driver's line.
     `slices` IS THE DENOMINATOR `sliceUs` HAS AND `steps` IS NOT, which result.c states at the group and which
     a reader without it gets WRONG IN A DIRECTION THAT INVENTS A FINDING. Measured, by the author of this
     hunk, one command before this landed: `sliceUs / steps` on a 180 s drive of gitlab.com/explore reads
     21.6 ms against solver/quantum.h's 12 ms, which looks like a quantum overshooting by 1.8x on every slice
     while `sliceOverruns` reports 179 of 3769 asks — two rows contradicting each other because one of them was
     divided by the wrong row. A slice spans as many steps as the flow takes before it yields, so `steps` and
     `slices` are two units and `sliceUs/slices` is the mean slice.
     `instanceUs` IS A SPAN AND THE OTHER TWO ARE ACCUMULATORS, which is why this file says so beside the kind
     the producer declares: it is ONE subtraction of two clock readings rather than a sum of anything, so it is
     the only row here a reader may put UNDER an accumulator. `loopUs + betweenSlicesUs == instanceUs` is the
     one pair on the whole census that may be read against EACH OTHER at a single census, and
     `betweenSlicesUs` is the half nothing else here can state: the thread the HOST held while the engine was
     not in its slice bracket. A budget that is mostly `betweenSlicesUs` and one that is mostly `loopUs` are
     opposite diagnoses — the first is this extension's own relay and message pump, the second is the engine —
     and every real-site reading ever taken with this driver was consistent with both.
     BOTH IDENTITIES ARE CHECKED IN `census` BECAUSE BOTH ARE `DCHECKF`s, at solver/engine.c's
     engine_step_unit_runs, which a RELEASE artifact compiles out — and a live drive measures release. That is
     the same argument the `outOfPrograms` partition and `programsAhead` bucket 0 carry one list up, and it is
     the whole reason carrying a row and checking its contract are one diff rather than two.
     LIFETIME, PER INSTANCE, as the producer declares all four — so they may be differenced and accumulated,
     and a sample below its predecessor is the engine and not the run. An artifact older than them prints `-`,
     which is this driver's absent-versus-zero rule and is the honest answer: the run did not state them. */
  "slices", "instanceUs", "loopUs", "betweenSlicesUs",
  "forks",
  "unitMidProgram", "unitParked", "unitCheckpointOwed",
  /* AND THE ROW THAT DECIDES WHICH READING OF `orphansAsked: 0` IS EVEN AVAILABLE, which this driver carries
     the numerator of in COUNTERS and has never carried the denominator of. flow_step's whole work ladder —
     the routed deliveries, the checkpoint, the reply, the program sequence, the task, the lifecycle, the
     ORPHAN rungs, the clock-driven sources and every resting arm — is inside one `if (!f->frame)`, so a step
     on a framed member asks none of those conditions and shows up in none of them. `unframedStepsLifetime`
     is how many steps entered that block. Read beside `orphansAsked`: 0 here says the ladder was never
     descended and the cause is upstream of every arm in it; a large value says it WAS descended and an arm
     ABOVE the orphan rung took every descent, which `stepUnitRuns` then names by arm.
     IT IS NOT `unitMidProgram` ABOVE IT AND IS NOT DERIVABLE FROM IT. That row is the member's frame AFTER
     the step, taken at the convergence point; this is the branch the step TOOK. They differ by exactly the
     arms that change framedness — a start compiles and leaves framed, a resume ends its frame and leaves
     unframed — and neither bounds the other. MEASURED on gitlab.com/explore, one terminal census: 209 and 210
     respectively, from 285 steps of which 75 descended the ladder.
     IT IS NOT `unframedPicksLifetime` IN WFQ_PICKS EITHER, WHICH IS THE TRAP THE NAMES SET. That one is
     raised in flow_credit_pick, whose only caller is the scheduler's `best != cur` block, so it counts
     SWITCH-INS that found an empty JavaScript execution context stack — a member switched in framed that
     unframes later is a descent this row sees and that one does not. Same page, same census: 3 against 75.
     AND IT IS NOT COMPARABLE WITH `steps`, which is why it is named for what it counts rather than for a
     share: `steps` is raised once per entry into flow_step and this per PASS through the block, because the
     loop body iterates at the reply delivery's turn continuation. The pair it belongs to is
     (`unframedStepsLifetime`, `orphansAsked`), counted on one basis, with the containment asserted in the
     engine. An artifact older than this row prints `-`, which is this driver's absent-versus-zero rule and is
     the honest answer: the run did not state it. */
  "unframedStepsLifetime",
  /* …AND HOW FAR DOWN THAT LADDER THE DESCENTS GOT, WHICH IS THE ROW THAT MAKES `finished` 0 READABLE AND
     WHICH THIS DRIVER CARRIED NEITHER OF THE THREE OF. solver/result.c states the reading in its own words:
     `finished` at 0 with `stepReachedRenderingLife` at 0 says the retirement arm was never ASKED and the cause
     is UPSTREAM of this boundary, and the same 0 with it LARGE says the boundary WAS reached and one of the ten
     arms ABOVE `finished` took every descent — it is the LAST of the eleven — which `stepUnitRuns` then names.
     Those are different files to open, and they were ONE string in the hung-cause verdict engine/build.mjs
     composes until these rows existed.
     THEY ARE SUFFIX SUMS AND `stepUnitRuns` IS A PARTITION, WHICH IS THE WHOLE REASON A READER NEEDS THEM.
     The three clock rungs are consecutive arms of one `else if` chain, so the first counts every descent that
     reached the chain, the next those the rendering rung did not take, the next those the timer rung did not
     take either — so "the lowest 0 is the localisation" is FALSE of that histogram read row by row and TRUE of
     these. An arm of `stepUnitRuns` reading 0 says THAT ARM NEVER TOOK A DISPATCH and never says it was not
     reached, which is the §AN-INVARIANT-OVER-A-GATED-OPERATION shape: those three arms are raised only where
     the hook TAKES the step, and these are the ASK.
     NO SUM IS CARRIED BESIDE THEM AND THAT IS THE PRODUCER'S DECISION, NOT AN OMISSION HERE — result.c states
     that every operand of all four identities is already on its line and that a second spelling of one number
     in one document is the drift the record-field gate exists to catch. They are contained in
     `unframedStepsLifetime` above, which is why they are filed with it.
     LIFETIME COUNTS, PER INSTANCE, RELEASED BY NOTHING — the `Life` in each key is the kind, so they may be
     differenced and accumulated and a sample below its predecessor is the engine and not the run. An artifact
     older than them prints `-`, which is this driver's absent-versus-zero rule and is the honest answer: the
     run did not state them. */
  "stepReachedRenderingLife", "stepReachedTimerLife", "stepReachedIdleLife",
  /* AND WHETHER THE COMPILE IS REPEATED, WHICH `classicCompiles` ALONE CANNOT SAY AND WAS BEING READ AS
     SAYING. solver/engine.h used to have a reader difference it against the programs a document reached and
     call a figure far above that count a compile repeated per flow; the numerator counts every FLOW, every
     TIMELINE and every APPENDED row — a lazy chunk, an injected `<script>`, a `javascript:` URL, a peer's
     operation — so on an app page whose bundle loads dozens of chunks of DISTINCT BYTES a figure many times
     `rootPrograms` is what a healthy run MUST read, and that reading cannot tell such a page from one that is
     re-parsing. `classicCompileAgain` is the parses whose BYTES some flow of this process had already parsed
     to completion, raised on the same line as the total it partitions.
     THE THREE ARE A FLOOR, A PRICE AND A BOUND, AND NONE OF THEM IS READABLE ALONE. `…Again` is the floor;
     `…AgainBytes` is what those re-parses covered, because twenty repeats of a 200-byte inline script and
     twenty of a 1.4 MB chunk are the same count and two different answers about whether a sharing diff is
     worth making; `…OwnDecode` is the bound, since a reply is decoded PER DELIVERY and two arms parked on one
     external row hold two buffers over one chunk, so a repeat inside that population is UNOBSERVABLE rather
     than absent. The true figure is in [`…Again`, `…Again + …OwnDecode`] and the two subsets are TWO
     PARTITIONS of one total that may not be added to each other.
     An artifact older than them prints `-`, which is this driver's absent-versus-zero rule and is the honest
     answer: the run did not state them. */
  "classicCompiles", "classicCompileOverruns",
  "classicCompileAgain", "classicCompileAgainBytes", "classicCompileOwnDecode",
  /* AND WHETHER A PARSE THAT HANDED THE THREAD BACK WAS EVER PICKED UP AGAIN, which none of the five rows
     above can say and which decides what a `classicCompiles` BELOW a document's row count means. It is read
     as a SUBTRACTION against an arm this driver already carries, and the pair is the whole of its value:
         stepUnitRuns["compile-handed-the-thread-back"] - classicCompileResumed
     is the parses BEGUN AND NOT ENDED, because an ended parse of k stints parks k-1 times and resumes k-1
     times while one still in flight parks k times and resumes k-1 — so every ended parse contributes ZERO and
     every parse still held contributes exactly ONE. Near zero, the seam is carrying every parse forward and a
     program count under `rootPrograms` is a BUDGET: the parse was advancing and the run ended inside it.
     Large, parses are being handed back and not picked up, which is a work item the frontier is holding and
     not advancing. Those take opposite work and every other row on this line reads the same for both.
     IT IS NOT A SUBSET OF `classicCompiles` AND THEIR QUOTIENT IS NOT A RATE: the two are raised at different
     events, one per stint that continued a parse and one per parse that ended, which is the same relationship
     `classicCompileOverruns` has to it. The containment that does hold is against the yielded arm.
     BOTH ZERO IS A THIRD READING AND NOT THE FIRST: a process whose yielded arm is also zero never fired the
     parse seam at all — every program parsed inside one stint — and says nothing about resumption. So the arm
     is the witness that makes this row's own zero mean something, and neither is read without the other.
     An artifact older than it prints `-`, which is this driver's absent-versus-zero rule. */
  "classicCompileResumed",
  "rootPrograms", "deepest", "completed", "deepestLeft", "finished",
  /* AND WHETHER A REPLY EVER BECAME A PROGRAM, WHICH IS CLAUDE.md §Learning-from-replies' HEADLINE MOAT
     SURFACE AND WHICH NO ROW ABOVE CAN STATE. "A fetch whose body is JAVASCRIPT is ALWAYS fetched + EXECUTED
     (a lazy chunk reveals real endpoints)" is built at TWO doors — solver/engine.c's FLOW_PENDING_RESOLVE
     delivery and core/xhr/xml_http_request.c's `xhr_take_reply` — and both end in the one compile entry,
     which queues a row of the SAME KIND the document's own seeded `<script>` rows carry. So `progStarts`
     and its two arms sum a chunk that arrived over the network with the page's own bundle, and this driver
     could print `rootPrograms 33 / deepest 4` all day without ever saying whether a single chunk had been
     queued at all.
     EACH DOOR IS A PAIR AND NEITHER HALF IS READABLE ALONE. `…AsksLife` is raised where the door HOLDS A
     REPLY RECORD, upstream of the type gate, because both doors DECLINE correctly for a reply whose computed
     type is not JavaScript — most replies are not programs — so an outcome census reports every correct
     refusal as the door failing. `0/0` is a door this run never reached; `0/N` is a door reached N times
     that queued nothing. Those take opposite work: the first is a page that issued no `fetch()` or sent no
     XMLHttpRequest, and the second is a question about what those replies WERE.
     THE FIVE ARE NOT FIVE INDEPENDENT READINGS AND THE PRODUCER SAYS SO. Each door's two rows share a
     precondition — nothing is queued at a door nobody asked — so a pair at `0/0` is ONE fact about that door;
     and `netProgQueuedLife` is the one compile entry's own total with `fetch + xhr <= total` asserted in the
     engine, so a zero there entails both arms at zero. What IS independent is the two DOORS, which is why
     both are carried: a bundle may send no `fetch()` and many XMLHttpRequests, since axios's browser adapter
     IS one — the same reading the six `epXhr*` rows below exist for, asked of the program door instead of
     the request door.
     `netProgQueuedLife` IS NOT THE SUM OF THE TWO ARMS AND MUST NOT BE READ AS ONE. It is raised inside
     `engine_queue_fetched_script`, whose third caller is `test_forced.c`'s `loadScript` host edge — a
     `<script src>`-shaped door that cannot reach the delivery arm at all — so the residue is ZERO in the
     shipped artifact this driver points at and is the fixture's own edge in that binary. Reading the residue
     is what makes a FOURTH door visible in this output; the engine asserts only the direction that cannot be
     innocent, an arm ABOVE the total.
     ALL FIVE ARE LIFETIME COUNTS AND THE PRODUCER STATES IT — `Life` is spelled into the name for a reader
     and the kind is declared at result.c's `@kinds-of cold` block for this driver, which composes its header
     from that declaration rather than from a list here. An artifact older than these rows prints `-` for all
     five, which is this driver's absent-versus-zero rule and is the honest answer: the run did not state
     them. */
  "netProgQueuedLife",
  "netProgFetchAsksLife", "netProgFetchQueuedLife",
  "netProgXhrAsksLife", "netProgXhrQueuedLife",
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
     absent-versus-zero rule and is the honest answer: the run did not state them.
     SEVEN OF THE NINE ARE LIFETIME COUNTS AND TWO ARE NOT, WHICH THIS SENTENCE USED TO GET WRONG — it read
     "All nine are lifetime counts and may be differenced across two samples of ONE instance", and a declared
     kind LICENSES arithmetic, so the wrong one is worse than a missing one. `epEmitted` and `epPreProgram`
     are GAUGES: `endpoint_mark_asset` MARKS RATHER THAN DELETES and its verdict arrives with the REPLY while
     the record was minted at the REQUEST, so a census taken between those two instants counts the record and
     the next one does not — `epEmitted` FALLS by one with nothing wrong, and `epPreProgram` is raised inside
     that same emitted arm. solver/result.c states the split and the reason at its own `@kinds-of` block, and
     the header line above is composed from that declaration, so this driver prints the correction without
     being told it. `epMinted` and `epAssets` are the monotone halves and stay lifetime.
     THE TELL GENERALISES AND IS FREE: `epEmitted = epMinted - epAssets` with BOTH terms rising, and a
     difference of two monotone counts is not monotone. CLAUDE.md §A-GAUGE-AND-A-LIFETIME-COUNTER's check —
     the samples decreased — needs a SERIES, and a reader holding one census has none, so a kind of this shape
     has to be read off the mechanism rather than waited for.
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
  "epAsks", "epAskPreProgram", "epAskSuppressed", "epAskMerged", "epAskMinted",
  /* AND THE CUT INSIDE THE MERGED ARM, WHICH IS THE RAZOR'S SECOND READING STATED RATHER THAN BOUNDED. The
     five rows above make `asks - preProgram` readable and that subtraction is a CEILING on "running code
     reached a network call site and every address it built was already known" — it is nonzero for a
     post-program MINT and for a merge into a post-program record too. This row is that population and only
     it. A NONZERO here beside `epBeyondMarkup: 0` is the whole finding in two numbers: the page's own code
     ran, composed addresses, and composed the `<head>`'s. It is a LIFETIME count like its neighbours and it
     is CONTAINED in `epAskMerged`, so it is never added to the three arms beside it. */
  "epAskMergedPreProgram",
  /* AND THE HOST EDGE'S OWN ENTRY, WHICH EVERY ROW ABOVE STRUCTURALLY CANNOT SEE AND WHICH THIS DRIVER — THE
     ONE THAT READS THE RAZOR — WAS THE CONSUMER THAT NEVER ASKED. The eleven `ep*` rows above are counted at
     endpoint_record's door, so a `fetch()` the page CALLED and the engine threw out of, or parked inside and
     never resumed, is a network call site REACHED and is in none of them. solver/endpoint.h states what these
     five separate and it is the product's own question: `epFetchAskBeganLife == 0` beside a nonzero `epAsks`
     says the page never called this door and the defect is upstream of every host edge; a nonzero one with
     mass in the stage histogram says the page called and the CONSTRUCTION died, at a named stage. Before
     these rows those two produced byte-identical documents, and `epAsks == epAskPreProgram` — which is what a
     real page reads — is exactly the reading that cannot tell them apart.
     KIND IS SPELLED INTO EVERY NAME BY THE PRODUCER AND IS NOT THIS FILE'S CLAIM: `Life` is a LIFETIME COUNT
     and never a gauge, `Ask` or `Out` is which side of the gate it counts. So they are filed here with the
     lifetime rows, they may be differenced across two samples of ONE instance, and a sample below its
     predecessor is this instrument rather than the run.
     THE UNIT IS STATES OF core/fetch's REQUEST-CONSTRUCTION MACHINE AND IS NOT CALLS — which stages of Fetch
     those are is solver/endpoint.h's to state and is deliberately NOT restated here, because a bare section
     number in this file names no standard and would be resolved by its FILE VOTE, and this file's numeric
     citations are HTML's. A state is BYTE-COPIED at a deep
     fork and the copy inherits the capture flag, so one `fetch()` whose `input` ToString forks composes TWO
     requests against ONE capture — which is not exotic, it is this tool's own subject. That is why
     `epFetchAskBeganLife` is READ AS A FACT AND NEVER SUBTRACTED FROM: `began - freed` and `began - offered`
     are both quantities the engine refuses to assert and this driver refuses to print.
     `epFetchOutDiedAtLife` IS A PARTITION AND NOT A LADDER, which is the one thing a reader of a stage table
     gets wrong. Its arms are the stage a torn-down construction that offered NOTHING was standing at, keyed
     by the machine's own `js_fetch_steps[]` labels, and NO ARM IMPLIES ANOTHER — so `the lowest 0 is the
     localisation` is not a reading it supports. A zero in one stage is the positive statement that no
     construction died there, because the engine emits every stage including the zeroes.
     THE THREE IDENTITIES ARE CHECKABLE FROM THIS DRIVER'S OWN OUTPUT, which is the whole reason all five are
     carried rather than the two a headline would want: the stage arms plus `epFetchOutFreedOfferedLife` equal
     `epFetchOutFreedLife` (a PARTITION over one teardown); `epFetchOutFreedOfferedLife <= epFetchAskOfferedLife`,
     whose slack is the constructions still parked on their replies and on a page mid-run is most of them; and
     `epFetchAskOfferedLife <= epAsks` three rows up, whose slack is every OTHER door into the surface — which
     is what makes the fetch edge's SHARE of the ask population readable and is the only relation that ties
     this census to the razor it was built to explain.
     AN ARTIFACT OLDER THAN THESE ROWS PRINTS `-` FOR ALL FIVE, which is this driver's absent-versus-zero rule
     and is the honest answer: the run did not state them. The engine's absent form is the rows being ABSENT
     from `_cold` rather than five zeroes — a host that installs no fetch runs no fetch machine and has no
     population — so `k in c` is false and this list yields `null`, with no arm anywhere that could turn that
     into a 0. THAT MEASUREMENT IS REWRITTEN RATHER THAN DELETED, because the reasoning above it is what a reader
     re-derives and the COORDINATE is the only part that rotted. It read: with `epAsks` and `epEmitted` as the
     armed controls answering 1 each and an invented name answering 0, all five read 0 occurrences in the
     installed `extension/lib/qjs/qjs.wasm`, whose stamp was 54 commits behind and was NOT a descendant of the
     commit that landed them — so the first reader of this list saw five `-` and that was the rows working,
     not the run. EVERY ONE OF THEM READS PRESENT IN THE INSTALLED ARTIFACT NOW, measured twice by two readers
     with an invented name as the armed control, so the sentence had become AN ABSENCE ASSERTED AFTER IT WAS
     FILLED — the direction CLAUDE.md rates worst, since the only reader of a named absence is somebody about
     to go and build it, and the thing they would build is already here.
     IT IS NOT REPLACED BY A FRESH NUMBER, WHICH WOULD RESTART THE SAME CLOCK. The `# artifactRows` line this
     driver prints before its first row names the rows the installed wasm does not carry, so the absence is
     DERIVED at the moment a reader meets a `-` instead of being recalled here — and it covers every row this
     driver asks for rather than the five somebody once measured by hand.
     RETIREMENT: that line goes when the build stamp itself records the row names its composers spell, because
     the question is then answered FROM the stamp and no reader probes the bytes at all. */
  /* AND THE ROW WITHOUT WHICH `epFetchAskBeganLife`'s ZERO IS THREE READINGS, which is the reading this driver
     is pointed at every day and the one it could not answer. The begun row is raised at Fetch §5.4's first
     stage, and core/idl_args.h numbers a declared member's stages from IDL_STEP_FIRST because stages 0 and 1
     belong to the HOSTING machine — the argument-count check and the ES-to-IDL conversions — and BOTH are rest
     points. So a zero there was consistent with the page calling no `fetch()`, with the page calling one whose
     conversion THREW, and with one whose conversion PARKED and was never resumed, and those take opposite
     work: leave it, fix core/idl_args.c, or fix the ORDER. `epFetchAskCalledLife` is raised at that prologue's
     entry, so `called > 0` with `began` at 0 is the middle state and `called == 0` is the other two.
     WHAT IT STILL DOES NOT SEPARATE, because no row at this edge can: a page that calls no `fetch()` from a
     flow that never reached a call the page does make — the prologue is entered in neither. That is the
     ORDER's question and solver/flow.h's `readyPicksLifetime` legend is its instrument, which is why these two
     rows are read BESIDE the job rows on this list and not instead of them.
     AND THAT SENTENCE IS STILL TRUE OF THIS EDGE AND IS NO LONGER TRUE OF THIS LIST, which is why it is
     REWRITTEN rather than left standing: `epFetchAskNamedLife` below separates exactly those two states, and it
     does so WITHOUT being a row at this edge — it is raised where the COMPILER resolves the free identifier
     `fetch` against the global object, which is upstream of reach because reach is what running is. A reader who
     took the paragraph above as it stood would go on reading `called == 0` beside the job rows and inferring,
     where one row on this same line now answers it. The job rows remain the instrument for WHY a call was not
     reached; the pair below is the instrument for WHETHER the program contains one at all.
     A ZERO HERE IS NOT EVIDENCE THE HOOK IS BROKEN, and the producer states the narrowing: `idl_concolic_rule`
     answers CROSSES for IDL_USVSTRING, so a concolic URL — the computed address this tool exists to report —
     passes the conversion without parking, and the conversions park on a page GETTER rather than on an unknown
     string. `called == began` is therefore the expected healthy reading, and the containment `began <= called`
     is asserted by the producer where both terms are in one hand. */
  /* THE DENOMINATOR, BEFORE THE ROW IT IS A FRACTION OF. `named > 0` with `called == 0` is a program that spells
     `fetch` and a flow that never reached the call — the state the paragraph above says no row at this edge can
     reach, answered from outside the edge. `named == 0` beside `called == 0` is the CORRECT SILENCE, and it is
     the reading this census could not express before: a document whose bundle names no `fetch` at all.
     IT IS A FLOOR OVER ONE SPELLING AND THAT IS WHY IT WITHHOLDS RATHER THAN ACCUSES. `window.fetch(u)` is a
     property read and a bundle that shadows the name with a parameter uses a local slot, so neither reaches a
     global resolution — `called > named` is ORDINARY and so is `named > called`, and the producer asserts no
     containment in either direction. Read as a BIT; the magnitude counts compiler resolutions and rises with
     every flow that replays the document. */
  "epFetchAskNamedLife", "epFetchAskNamedTypeofLife",
  /* AND THE PROPERTY SPELLING, WHICH IS WHY THE FLOOR SENTENCE ABOVE NO LONGER NAMES `window.fetch(u)` AS A
     MISS IT CANNOT SEE. It is a row of its own and is NEVER SUMMED with the two above, because the `typeof`
     split does not exist for a property — §13.5.3 step 2.a has nothing to patch when a missing key is already
     `undefined` — so this population mixes uses with feature detection and the two above do not. Read it
     BESIDE them: a document whose named row is zero and whose prop row is not is a bundle that reaches the
     door only through a global receiver, which is what connect-es's `(e.fetch ?? globalThis.fetch)(url, …)` is
     and therefore what every protobuf-over-HTTP app in this corpus looks like. */
  "epFetchAskNamedPropLife",
  "epFetchAskCalledLife",
  "epFetchAskBeganLife", "epFetchAskOfferedLife",
  "epFetchOutFreedLife", "epFetchOutFreedOfferedLife", "epFetchOutDiedAtLife",
  /* AND THE OTHER DOOR, WITHOUT WHICH THE FIVE ROWS ABOVE ARE READ AS THE WHOLE OF WHAT A PAGE CALLED. The
     reading solver/endpoint.h names is the one this driver is pointed at every day: a document whose
     `epFetchAskBeganLife` is ZERO and whose `epAsks` is not, taken for a page that reached no network call
     site, when what it reached was XMLHttpRequest — which is what a large share of real bundles ship, since
     axios's browser adapter IS one. These six say so, and they are NEVER SUMMED with the five above: they
     count states of a DIFFERENT machine whose stages are its own.
     THE UNIT IS STATES OF `send()`'s MACHINE AND NOT OF THE ONE THAT RECORDS, which is the correction
     solver/endpoint.h carries in full at the residual that had it the other way round. `send()` is its own
     declared member with seven stages and four page-code park points, and the lifecycle machine it mints
     records at the FIRST stage that machine has — so where an XHR request DIES is a fact about `send()` and
     the lifecycle machine's stages are all downstream of the door.
     WHY THERE ARE SIX AND NOT FIVE. The fetch edge's second row is an OFFER, because one state constructs and
     offers; here the send state is torn down before the asynchronous arm's task runs, so it can say only that
     it PLACED the fetch (`epXhrAskPlacedLife`) and the OFFER is a row of its own raised at the door
     (`epXhrAskOfferedLife`). They are two different populations and the producer asserts NO relation between
     them — a placed send whose task never runs offers nothing, and abort() and the request error steps mint
     lifecycle machines that record nothing.
     THE IDENTITIES CHECKABLE FROM THIS DRIVER'S OWN OUTPUT ARE TWO AND NOT THREE: the stage arms plus
     `epXhrOutFreedPlacedLife` equal `epXhrOutFreedLife` (a PARTITION over one teardown), and
     `epXhrAskOfferedLife <= epAsks`, whose slack is every OTHER door — which is what makes this edge's SHARE
     of the ask population readable beside the fetch edge's. THE FETCH EDGE'S MIDDLE RELATION HAS NO ANALOGUE
     HERE and the producer says why: `send()`'s machine declares no fork refusal, so a deep fork of a state
     that has already placed its fetch files twice against one placement — which core/fetch survives only
     because it REFUSES that fork. So `epXhrOutFreedPlacedLife` and `epXhrAskPlacedLife` are two facts and a
     reader may not subtract them, exactly as `epXhrAskBeganLife` may not be subtracted from.
     KIND IS SPELLED INTO EVERY NAME BY THE PRODUCER AND IS NOT THIS FILE'S CLAIM — `Life` is a LIFETIME
     COUNT, `Ask` or `Out` is which side of the gate it counts — so they are filed here with the lifetime
     rows. `epXhrOutDiedAtLife` IS A PARTITION AND NOT A LADDER, exactly as its fetch sibling is.
     AN ARTIFACT OLDER THAN THESE ROWS PRINTS `-` FOR ALL SIX, which is this driver's absent-versus-zero rule
     and is the honest answer. The engine's absent form is the rows being ABSENT from `_cold` rather than six
     zeroes — a host that installs no XMLHttpRequest runs no send machine and has no population — so `k in c`
     is false and this list yields `null`, with no arm anywhere that could turn that into a 0. */
  /* AND THE SAME SPLIT FOR THIS DOOR, for the same reason and with the same two states it cannot separate —
     `send()` is its own declared member, so SEND_STAGES is based at IDL_STEP_FIRST and the conversion of its
     argument runs in front of SEND_CHECKS. Read the pair, never either alone. */
  /* …AND THIS DOOR'S DENOMINATOR, whose entry identifier is the INTERFACE's and not `send`'s: a program reaches
     this construction only through `new XMLHttpRequest`, so that is the free identifier the compiler resolves
     and `send` — reached through a receiver — is never a global. Same floor, same two ordinary inequalities,
     same reason the `…Typeof…` half travels with it. */
  "epXhrAskNamedLife", "epXhrAskNamedTypeofLife",
  "epXhrAskNamedPropLife",
  "epXhrAskCalledLife",
  "epXhrAskBeganLife", "epXhrAskPlacedLife", "epXhrAskOfferedLife",
  "epXhrOutFreedLife", "epXhrOutFreedPlacedLife", "epXhrOutDiedAtLife",
  /* AND CLAUDE.md §What-the-tool-produces' RAZOR, WHICH IS THE ONE ROW ON THIS WHOLE LIST THAT ANSWERS WHAT
     THE RUN LEARNED RATHER THAN HOW FAR IT GOT. Every `ep*` row above is a TOTAL over the learned surface or
     over the gate in front of it; this partitions that surface by WHAT A PARSE OF THE SERVED DOCUMENT WOULD
     HAVE REACHED — `beyond` the addresses a `<script src>` scan does not already state, `markup` the `<head>`
     counted back, `either` the doors that are reached by a parser-inserted element and a script-created one
     alike and do not record which. solver/endpoint.h holds the map and states why there are three classes and
     not two; the count is a FLOOR (`beyond`) with its undecidable population beside it rather than a single
     number that would have to guess.
     IT IS THE ONE OBJECT ON THIS LIST AND ITS KIND IS THE PRODUCER'S LIKE EVERY OTHER MEMBER'S. `census()`
     copies it whole and the per-run line carries it as JSON; nothing here spreads it, because a range over
     bucket names is not a quantity — which is `endpointDoors`' rule one array over.
     A DIAGNOSTIC AND NEVER A TARGET, on §netdiff's own terms: `beyond` 0 against a nonzero `epEmitted` is a
     REFUSAL TO CLAIM the capability on this document, not a smaller version of it, and it is an IDENTITY read
     WITHIN one run rather than a total to compare across two. An artifact older than the row prints `null`,
     which is this driver's absent-versus-zero rule and is a different fact from a surface with nothing
     beyond the markup — the first is the run not stating it, the second is the razor answering. */
  "epReach",
  /* …AND THE SAME RAZOR READ AGAINST THE OWNER'S HARD BAR RATHER THAN AGAINST A MARKUP PARSE, WHICH IS THE
     ROW THAT MAKES THAT BAR SCORABLE OVER A CORPUS AT ALL. The bar is "an address, a key or a value that NO
     PARSE of the served bytes can state, because it exists only at run time", and `epReach` cannot be
     coarsened into it in either direction: a literal chunk URL delivered through `module-import` is `beyond`
     a markup parse and clears NOTHING at this bar, while `/api/{location.hash}` through `fetch` clears it
     outright. Two addresses through ONE door differ on exactly this — which is why a 19-site census of this
     driver could close with a full `epDoors` table, `epReach` in hand, and no way to say whether any of its
     119 code-door addresses cleared the bar.
     IT IS A SECOND OBSERVATION AND NOT A THIRD GRAIN, which is where it differs from the row above it.
     `epReach` is `epDoors` summed by a map and says so; this is keyed on a property of the ADDRESS VALUE —
     whether the run had DETERMINED it — which solver/endpoint.c reads off that value's own concolic
     provenance at the recording door. No door implies it either way, so a reader holding all three of these
     rows holds TWO observations (CLAUDE.md §EVIDENCE-INFLATION).
     IT IS A FLOOR AND THE TWO FLOORS OVERLAP AND MAY NOT BE SUMMED. `unknown` rows DEFINITELY clear the bar;
     `concrete` claims nothing whatever about a parse, because whether a static reader could have stated an
     address is not decidable by anybody — a bundler's chunk manifest needs a scope pass to resolve. And an
     address a REPLY named is `concrete` here and past every parse of the document at once, so the bar's floor
     over a surface is the UNION of this row's `unknown` and `epReach`'s `beyond`; a union is not a sum,
     because a row can be in both, and nothing in this driver composes one.
     ITS KIND AND ITS ABSENCE FOLLOW `epReach`'s EXACTLY: an object copied whole and never spread, and an
     artifact older than the row prints `null`, which is a different fact from a surface every address of
     which the run had determined — the first is the run not stating the bar, the second is the bar answering
     and refusing to claim it. */
  "epAddressClass",
  /* …AND THE UNION OF THOSE TWO, COMPOSED BY THE PRODUCER, WHICH IS THE ONE ROW ON THIS LIST A PERSON MAY
     READ AS THE HARD BAR ITSELF. The row above ends "the bar's floor over a surface is the UNION of this
     row's `unknown` and `epReach`'s `beyond`; a union is not a sum, because a row can be in both, and
     NOTHING IN THIS DRIVER COMPOSES ONE" — and that last clause was a statement about THIS FILE which is
     why it is kept there: a reader holding the two marginals re-derives the union-is-not-a-sum argument and
     then has to assemble the union by hand, which is the one assembly CLAUDE.md
     §What-the-tool-produces names as the defect. solver/endpoint.c's `endpoint_razor_class_of` composes it
     PER ROW at the emitter, out of the address class and the door list's FOURTH column — whether the door
     handed this surface bytes that were in the served document at all — and `endpoint_razor_hist_json`
     partitions the emitted surface by it, so the composed class is a @COLD row and no consumer assembles
     anything.
     IT IS A THIRD GRAIN OF TWO OBSERVATIONS AND NOT A THIRD OBSERVATION. It is DERIVED from `epReach`'s
     operand and `epAddressClass`, so a reader holding all three of these rows still holds TWO facts
     (CLAUDE.md §EVIDENCE-INFLATION) — and the derivation is named here rather than left to be noticed,
     which is that record's own prescribed cure.
     IT IS A FLOOR AND NEVER A TARGET, in `epReach`'s words: `runtime-only` 0 against a nonzero `epEmitted`
     is this document REFUSING TO CLAIM the bar and not a smaller version of it, and `unproven` is not the
     claim that a parse COULD have stated the address — solver/endpoint.h enumerates the three populations
     it holds, one of which (a source this flow PINNED and re-read) really is past every parse.
     ITS KIND AND ITS ABSENCE FOLLOW THE TWO ROWS ABOVE IT EXACTLY: an object copied whole and never spread,
     and an artifact older than the row prints `null`, which is the run not stating the bar and is a
     different fact from the bar answering and refusing to claim it. */
  "epRazorClass",
  /* …AND THE BOUND ON THE ONE POPULATION THAT ROW'S OWN BANNER NAMES AND COULD NOT SIZE. It says
     "solver/endpoint.h enumerates the three populations it holds, one of which (a source this flow PINNED and
     re-read) really is past every parse" — a sentence that was true, was load-bearing, and pointed at a
     quantity NO census published: `path_pinned` is written at five request sites and read by one accessor,
     and no row anywhere carried it, so a reader meeting `runtime-only: 0` could not tell a run that genuinely
     determined every address from one whose pin erased a taint the bar should have seen.
     IT IS NOT A THIRD OPERAND OF THE BAR AND MUST NEVER BE READ AS ONE. The bar is a FLOOR made of two
     POSITIVE statements and this is a MAY — the pin is a fact about the PATH, and this row does not say the
     address read that source at all — so unioning it in would turn a floor into an OVER-claim, which is the
     defect solver/endpoint.h states at the list itself and the reason it is a separate word.
     WHAT THE PAIR SAYS, AND IT IS A DECISION BETWEEN TWO OPPOSITE DIFFS rather than a column: `unproven`
     beside `may-rest-on` 0 is a run on which no address was composed by a path that had pinned anything, so
     the bar's zero is the run having genuinely proved nothing and the next diff belongs to the SOLVER keeping
     more values unknown; a nonzero `may-rest-on` is the population the bar's zero does not account for, and
     the next diff belongs to the PIN carrying a provenance through a determined read.
     `unasked` IS NOT A SMALL `no-witness`, which is the one misreading this row most invites: it is the
     question not having been asked, because no flow stood when the record was minted, and a nonzero row there
     is a door composing a request outside the scheduler — a finding about that door and not about an address.
     ITS KIND AND ITS ABSENCE FOLLOW THE THREE ROWS ABOVE IT EXACTLY: an object copied whole and never spread,
     and an artifact older than the row prints `null`, which is a different fact from a surface no address of
     which rested on a pin. */
  "epWitnessClass"];

const COLD_ROWS = COLD_STEP_UNITS.concat(COLD_FRONTIER, COLD_SEED, COLD_COUNTERS, COLD_FORK_PINNED,
                                        COLD_ORPHAN_ORDER);
/* WHAT ASKING THE ORDER COST — READ OFF THE `wfq` OBJECT, WHICH IS THE COMPOSER THAT PUBLISHES IT.
   These twelve were first added to COLD_COUNTERS, which reads `r.cold`, and a 180 s drive of a real page
   read `null` for every one of them while `kindsOf` and `requireWhole` both PASSED — the first asks
   whether a carried row is published by ANY composer and the second whether one composer's rows are all
   carried, and neither can see a row looked up in the wrong object. `requireFrom` in
   testing/census_rows.js is that question, added with this repair and armed on this exact defect, so the
   mistake is now a startup throw naming the composer that really owns the row.
   WHAT THEY ARE FOR, UNCHANGED BY THE MOVE: they are the one quantity the three microsecond
   accumulators in COLD_COUNTERS cannot bound, and a reader of `schedUs` will bound it anyway. solver/engine.h says it in as many words at the row that
     exists for it: "`sched_us` bounds what the PICK cost — flow_next_to_run runs before the step bracket opens
     — and the preempt hook's OWN rescan of the frontier does not land there: it is called from the
     interpreter, so an O(members) walk through flow_rival_of is charged to `slice_us`, inside the very turns
     this row counts. A reader who takes a small `sched_us` for `the ordering is not the cost` has bounded the
     pick and said nothing about the hook."
     MEASURED, AND THE READER WHO DID THAT WAS THIS FILE'S OWN AUTHOR ONE COMMIT EARLIER. The hunk that landed
     `instanceUs` above carries, in its own commit message, `schedUs/stepUs = 10.3%` read as "the ordering is
     NOT the constraint and a weight change is still the wrong diff" — on a 180 s drive of gitlab.com/explore
     at 13600 members. That is the sentence engine.h forbids, and the row that would have contradicted it is
     `scanRivalWeights`, which no real-site driver has ever carried. History is not rewritten to repair a
     published message, so the correction lives here, where the row does.
     WHAT THE SIX ARE FOR, in the producer's words rather than mine: "the tail is not being reached" has TWO
     causes — not enough thread time for the members standing, or the thread spent ASKING the order rather than
     running it — and no row separated them. It names the readings too: `scanNextWeights / steps` against
     `members` for the first, and `scanRivalRuns / forks` for the second, the dispatch loop asking once per STEP
     and the hook once per frontier GENERATION, which a forking page moves per fork. `forks` is on this list
     for that second reading and for nothing else; it was not carried either.
     `scanCensus*` IS THE INSTRUMENT'S OWN COST AND IS THE REASON IT IS NOT OPTIONAL. `scanCensusWeights`
     against `scanNextWeights` is "the share of all frontier-weighing that went to REPORTING rather than to
     running — the only way to settle whether an instrument is heavy enough to change the run it samples", and
     the census weighs the frontier TWICE per sample, so the share is `scanCensusWeights` DOUBLED. A driver
     that publishes scheduler cost and not its own observer's is one whose numbers nobody can clear.
     `preemptAsksLifetime` IS `scanRivalRuns`' DENOMINATOR AND ITS CONTRACT. `scanRivalRuns / scanNextRuns` is
     a COST and was being read as the hook's CADENCE, which it is not; the ask count is what answers the
     cache's own question, and `scanRivalRuns <= preemptAsksLifetime` is a `DCHECK` at result.c — compiled out
     of the release artifact a live drive measures — so the ratio is published here as a rate that may not
     exceed 1 and refused by name when it does.
     EVERY ROW HERE IS A LIFETIME COUNT, as the producer declares, and the scan rows are TWO QUANTITIES —
     a RUN count and a WEIGHT count — over the entries of `FLOW_SCANS`, rather than a partition of anything on
     this line: there is no total here for a sum check to be made against, which result.c states at the group
     and which is why no identity between them is asserted. NO COUNT OF EITHER IS WRITTEN IN THIS PARAGRAPH,
     and that is a repair rather than a style: it read `ALL TEN ARE LIFETIME COUNTS … the six scan rows are TWO
     QUANTITIES OVER THREE ENTRIES`, and the list it was written over is eleven lines below it holding TWELVE
     rows over FOUR entries. Both figures were wrong when written, in the UNDER-counting direction, and the
     enumeration they count was in the same comment block — CLAUDE.md's cheapest check, needing no tree, no
     command and no revision, because the evidence is already in the sentence.
     THE ARITY ERROR GENERATED A PUBLISHED SCOPE OVER-CLAIM AND THAT IS WHY IT IS RECORDED RATHER THAN QUIETLY
     FIXED. `FLOW_SCANS` has FOUR entries — NEXT, RIVAL, OTHER, CENSUS — and a reader who believes THREE and
     sees CENSUS named separately concludes that NEXT plus RIVAL is everything that is not reporting. A
     coordinator holding this paragraph published `the preempt hook does 61.3% of ALL frontier-weighing` off
     `hookWeighShare`, whose denominator is `scanNextWeights + scanRivalWeights` and whose own banner correctly
     says ORDERING. The row was right, the share was right, and the sentence relaying it was wrong because this
     comment had mis-stated the enumeration the share is a subset of. A stale count in a banner is not inert: it
     is the premise a reader composes their claim from.
     A DERIVED REFUSAL STANDS WHERE THE COUNT WAS, because a number in prose cannot be checked and a shape can:
     `scanRowShape` below asserts that these rows are exactly `{Runs,Weights} × entries` with no entry missing
     a twin, so a fifth `FLOW_SCANS` entry whose Weights row is carried and whose Runs row is not is refused by
     name rather than silently halving a denominator.
     An artifact older than them prints `-`, this driver's absent-versus-zero rule. */
const WFQ_SCAN_ROWS = ["preemptAsksLifetime",
  /* …AND WHICH HALF OF THE HOOK'S KEY HAD MOVED WHEN IT MISSED, WHICH IS THE ROW THAT TURNS A LARGE
     `hookWeighShare` FROM A FINDING INTO A DIFF. `scanRivalRuns` and `preemptAsksLifetime` together cannot ask
     it: the cache is keyed on a DISJUNCTION — the frontier GENERATION or the INCUMBENT — and both of those rows
     publish only the miss, so every reading of that rate has had to ASSUME which disjunct supplied it.
     result.c's own words for what each one decides, because they are the repair and its price:
       `rivalMissGen`  — the order GENUINELY changed. "the walk is what a forking page owes", so this half is
                         not a defect and no diff removes it.
       `rivalMissCur`  — "a rescan for a frontier in which nothing moved but the EXCLUDED member, WHICH A WALK
                         THAT FOLDED ITS TOP TWO WOULD ANSWER WITHOUT ONE." This is the avoidable half and the
                         producer names the repair.
       `rivalMissBoth` — "the row that prices either repair: where both moved in one interval, removing one
                         invalidator buys NOTHING because the other would have forced the same walk, so a large
                         `cur` beside a large `both` and a large `cur` beside a zero `both` recommend the SAME
                         DIFF AT COMPLETELY DIFFERENT PRICES."
     READ AS A PARTITION AND NEVER AS THREE RATES — their sum is asserted equal to `scanRivalRuns` at a `DCHECK`,
     which a release artifact compiles out, so `census` checks it here for the reason every other identity on
     this list is checked here.
     THE COMMIT THAT ADDED THESE SAID THEY WERE "READ BY NOBODY AT ALL", AND THAT ABSOLUTE WAS FALSE AT BIRTH.
     `testing/live-wfq.js` reads all three — four occurrences — at EVERY commit of that series and before it,
     measured with `git show <sha>:testing/live-wfq.js | grep -c rivalMissGen` against an invented name
     answering 0; and it is not a passive carry, it ASSERTS the conservation identity below. History is not
     rewritten to repair a message, so the correction is here, at the row.
     THE TRUE CLAIM IS THE NARROW ONE AND IT IS WHY THE ROWS ARE STILL CARRIED: THIS driver did not read them,
     so no reading taken with it could compose the share below. The absolute was an over-claim of exactly the
     shape CLAUDE.md §AN-OVER-CLAIM-IS-REFUTABLE names — one counterexample ends the sentence and takes the
     true part with it.
     THE CAUSE WAS A HAND-CHOSEN DENOMINATOR AND IT IS THE PART WORTH KEEPING. The sweep behind that commit
     tested THREE consumers named by hand (this file, engine/build.mjs, testing/corpus/site.mjs); census rows
     are spelled by TWELVE files, and `live-wfq.js` is the second-largest consumer of them in the tree. A
     population derived from the files somebody happened to open is §AND-THE-COMMONEST-WAY-TO-GET-THAT-LIST-WRONG
     exactly, and the prefix-blindness that sweep self-reported was the SMALLER half of its error.
     AND THE READERLESS AXIS IS ALREADY ASKED, SO DO NOT BUILD AN INSTRUMENT FOR IT. `engine/fieldgate.mjs`
     carries a `WRITE-NO-READER` band — "a producer emits a field nothing reads: a measurement that has never
     once been looked at, or the surviving half of a rename" — with a DERIVED-READER channel that credits rows
     no construct spells, and `engine/build.mjs`'s `censusRowSet` THROWS on a spliced object row with no
     reader, so the axis is closed by construction and not merely audited. A stale "nothing asks this" is the
     one direction that argues for the redundant second auditor §AN-AUDITOR-DERIVES-THE-RULE forbids.
     AND THE MISREADING THEY END IS RECORDED AT THE PRODUCER: `scanRivalRuns / forks` near 2.0 was taken as
     evidence that something raises the generation twice per fork, and "that inference does not follow from
     these rows — a raise is not a miss, and raises made inside one C call with no interpreter opcode between
     them collapse into ONE miss at the next poll." So `rivalPerFork` above is a COST and never a cadence, and
     this trio is what a reader needs beside it. LIFETIME, raised in every build. */
  "rivalMissGen", "rivalMissCur", "rivalMissBoth",
  "scanNextRuns", "scanNextWeights", "scanRivalRuns", "scanRivalWeights",
  "scanOtherRuns", "scanOtherWeights", "scanCensusRuns", "scanCensusWeights"];

/* THE SHAPE THOSE SCAN ROWS MUST HAVE, ASSERTED RATHER THAN COUNTED IN PROSE. Two quantities over the entries
   of `FLOW_SCANS`, so every entry this driver carries owes BOTH a `Runs` row and a `Weights` row. The one that
   matters is the WEIGHTS half: `hookWeighShare` is a quotient over a chosen SUBSET of the weight rows, and a
   subset is only readable as one while a reader can see what it is a subset OF — so an entry carried on one
   axis and not the other does not make a figure merely incomplete, it makes the denominator of every share here
   a number nobody can name. It refuses BY ENTRY rather than by total, because a count that is short and a count
   that is lopsided are different defects and only the second is repairable from this line.
   WHAT IT CANNOT SEE, stated so the gap is not inferred: whether this list covers every entry the PRODUCER
   emits. That is a fact about `FLOW_SCANS` in solver/flow.h, which no JavaScript here reads, and the honest
   consequence is that a fifth entry added there and carried nowhere is invisible to this check — `census`
   prints `-` for an absent row and a row that was never listed has no cell to print. This asserts CONSISTENCY
   of what is carried and never COMPLETENESS of it. */
const scanRowShape = () => {
  const by = new Map();
  for (const k of WFQ_SCAN_ROWS) {
    const m = /^scan([A-Z][A-Za-z]*?)(Runs|Weights)$/.exec(k);
    if (!m) continue;
    const e = by.get(m[1]) || {};
    e[m[2]] = k;
    by.set(m[1], e);
  }
  const lop = [];
  for (const [entry, axes] of by)
    if (!axes.Runs || !axes.Weights)
      lop.push(entry + " carries only " + (axes.Runs || axes.Weights));
  if (lop.length)
    throw new Error("live-run.js: a FLOW_SCANS entry is carried on one axis only — " + lop.join("; ") +
      ". Two quantities over the entries, both or neither: `hookWeighShare` and `censusWeighShare` are " +
      "quotients over a SUBSET of the weight rows, and a half-carried entry makes their denominator a " +
      "number no reader can name. Add the missing row to WFQ_SCAN_ROWS, or remove its twin.");
  return by;
};
const SCAN_ENTRIES = scanRowShape();
/* …AND THE TWO ROWS THAT PRICE THE REPAIR THE SCAN ROWS RECOMMEND, WHICH NO REAL-SITE DRIVER HAS CARRIED. The
   lane sent to FOLD the rival scan's top two refused the brief and asked for these by name, in its own words:
   `the hook rows alone cannot price their own recommended repair`. solver/engine.h now holds the design that
   replaced the fold — a MARGIN rather than a quantum, the scan's third-best weight bounding every member
   outside a retained pair for as long as the frontier generation stands — and the one quantity that says
   whether any margin exists is the carry, because solver/flow.c calls its bit `THE ONE PART OF A NON-RUNNING
   MEMBER'S WEIGHT THAT MOVES WITH NO GENERATION BUMP BEHIND IT`.
   WHAT EACH ONE IS, in the producer's words. `silPhases` is how many DISTINCT sub-quantum residues the frontier
   is standing on — the count of distinct `flow_silence_phase` values over the members — and `silCarry` is how
   many members are on the far side of the boundary right now. Members sharing a residue flip the bit TOGETHER,
   so `silPhases` is exactly the number of groups that can reorder between two generations: at 1 the bit is a
   COMMON OFFSET, nothing reorders, and a single cached maximum is exact. BOTH ARE GAUGES and `silCarry` may
   FALL between samples — the threshold sweeps downward as the family burns and resets every member at once —
   so neither may be differenced and `census` nulls both on an empty frontier through the producer's own
   `@kind` line rather than through any list here.
   `picksLifetime` IS CARRIED FOR THESE TWO AND FOR NOTHING ELSE. solver/flow.h names the reading: a member that
   has never held the thread carries the phase it was FORKED with, so `silPhases` far below the dispatch count
   says the residues are INHERITED rather than EARNED — which is a different finding from a frontier that has
   genuinely spread, and the two recommend different structures. It is the one row of this group that may be
   differenced, and `picksLifetime == _switches` is an identity result.c checks at the one moment both terms are
   in one hand.
   NO FIGURE FROM EITHER HOST IS COPIED HERE. solver/flow.h carries both measurements with the corpus each was
   taken over and the derivation as a command, and they disagree by two orders of magnitude in the ratio AND in
   its direction — about every other member its own group on the native host and rising, against a flat 120 on
   the vehicle while the frontier grew 4.9x. A count copied away from its derivation is a claim competing with a
   command, and this driver's whole purpose is to take the reading rather than to quote one. */
const WFQ_CARRY_ROWS = ["silPhases", "silCarry", "picksLifetime"];
const WFQ_ROWS = ["members"].concat(WFQ_JOB_SPLIT, WFQ_PICKS, WFQ_LADDER, WFQ_SCAN_ROWS, WFQ_CARRY_ROWS);
const OUT_NAME = { members: "wfqMembers" };
/* WHICH `wfq` ROWS ARE LIFETIME COUNTS, TAKEN FROM THE PRODUCER AND NEVER LISTED HERE — the set `census` gates
   on. A hand-kept list of which rows survive an empty frontier would be a second copy of a fact only the
   composer's `@kind` line states, which is the drift `kindsOf` exists to end. */
const WFQ_LIFETIME = new Set(kindsOf(WFQ_ROWS).byKind.lifetime);

/* WHERE THE FRONTIER STOOD, WHAT ITS STEPS DID, AND WHAT GREW IT — read off the row bridge.js wrote, never
   recomputed. `forkAt` is taken WHOLE and is not truncated to its heaviest rows: it is already a Space-Saving
   table with a bounded row count and it publishes its own understatement bound as a member, so a driver
   re-truncating it would hide rows AND drop the bound that says how far the survivors understate. */
function census(r) {
  const o = {};
  o.forkAt = ("forkAt" in r) ? r.forkAt : null;
  const c = ("cold" in r) ? r.cold : null;
  for (const k of COLD_ROWS) o[k] = c && (k in c) ? c[k] : null;
  /* AND THE OUT-OF-PROGRAMS PARTITION, TAKEN BY PREFIX OFF THE COMPOSER'S OWN OBJECT RATHER THAN FROM A LIST
     HERE — the rows beneath `outOfPrograms` are whatever NUMERIC keys result.c spells with that prefix, so a
     fourth arm added to cold.c's if/else chain is carried by this driver the day it lands. The OBJECT row
     beside them (`outOfProgramsAtTheLadderUnits`) is on COLD_FRONTIER and the numeric test excludes it, which
     is the same split `testing/corpus/site.mjs` makes and the same one engine/build.mjs makes by reading the
     CONVERSION in the format string rather than an exclusion list.
     AND THE SUM IS THE CONTRACT, WHICH IS WHY IT SUPPRESSES THE ROWS RATHER THAN BEING PRINTED BESIDE THEM.
     cold.c DCHECKs `unrun + framed + atTheLadder == outOfPrograms` where the whole population is in one hand,
     and a DCHECK is compiled out of the RELEASE artifact this driver drives — so this is the only place that
     identity is checked on the program a live drive actually measures. A disagreement means an arm was added
     without a row and the breakdown is "a SELECTION being read as a partition" in cold.c's own words, which
     makes every part under the total a guess; a guess printed as a partition is worse than a refusal, so the
     parts are dropped and the refusal is named.
     AND THE SECOND IDENTITY CROSSES THE BOUNDARY THE FIRST ONE DOES NOT. `programsAhead` bucket 0 and
     `outOfPrograms` are the same predicate written two ways over the same two fields (`dyn_n - script_i == 0`
     and `script_i == dyn_n`), asserted in result.c where both halves are in one hand and re-asked here at the
     point the numbers CROSS into this document — so a difference visible here and not there is a row lost
     between the census and the record rather than a walk that disagreed with itself.
     THE REFUSAL IS ITS OWN STRING FIELD AND IS NOT A ZERO, which is this driver's rule for `absentRefused`
     exactly: `null` means the run did not state the rows, and a message means it DID and they do not hold
     together. `spread` takes numbers only, so a refusal folded into the pair would print as `-` — the silence
     the field exists to end. */
  /* AND THE COMPARISON engine.c PRESCRIBES, COMPOSED HERE because a reader holding two bare gauges does not
     make it. `framedShare` near 1 says the whole work ladder — every arm of it, including the one that starts a
     member's next program row — is unreachable for essentially the whole frontier, so no ordering of those arms
     can be a first cause and a lane spent on one is spent on a population that never reaches it. Near 0 says
     the ladder IS being descended and an arm inside it is a fair subject.
     IT IS A GAUGE OVER A GAUGE, so it is read at ONE census and never differenced, and it is `null` rather
     than 0 when either half is absent or `live` is 0 — a share of an empty frontier is not a small share. */
  o.framedShare = (typeof o.framed === "number" && typeof o.live === "number" && o.live > 0)
    ? o.framed / o.live : null;
  o.framedShareOf = (o.framedShare === null) ? null : "framed / live";
  o.outOfProgramsRefused = null;
  if (c && typeof o.outOfPrograms === "number") {
    const parts = Object.keys(c).filter((k) => k !== "outOfPrograms" && k.startsWith("outOfPrograms")
                                               && typeof c[k] === "number");
    /* THE PARTS ARE ONLY CHECKED WHERE THERE ARE PARTS. A build publishing the total and no breakdown is an
       OLDER ARTIFACT and not a broken partition, and summing nothing against a nonzero total would refuse a
       run that measured an engine which never claimed a partition at all. */
    if (parts.length) {
      const sum = parts.reduce((a, k) => a + c[k], 0);
      if (sum !== o.outOfPrograms)
        o.outOfProgramsRefused = "the frontier census breaks `outOfPrograms` " + o.outOfPrograms +
          " into rows summing to " + sum + " (" + parts.join(", ") + ") — solver/cold.c raises them on one " +
          "if/else chain over one walk and DCHECKs the identity, so a disagreement on this RELEASE artifact " +
          "is an arm added without a row and the breakdown is a SELECTION published as a partition";
      else for (const k of parts) o[k] = c[k];
    }
    const pa = o.programsAhead;
    if (o.outOfProgramsRefused === null && pa && typeof pa === "object" && !Array.isArray(pa)) {
      const zero = (typeof pa["0"] === "number") ? pa["0"] : null;
      if (zero !== o.outOfPrograms)
        o.outOfProgramsRefused = "`programsAhead` bucket 0 is " + (zero === null ? "absent" : zero) +
          " against `outOfPrograms` " + o.outOfPrograms + " — they are the same predicate written two ways " +
          "over the same two fields, so a difference at this boundary is a row lost between the census and " +
          "this record and no reading composed from either is about the run that happened";
    }
  }
  /* AND THE SUBTRACTION IS COMPUTED HERE RATHER THAN LEFT TO THE READER, BECAUSE ONE A READER MUST PERFORM IS
     ONE NOBODY PERFORMS. Both halves are already rows above; this is the difference §What-the-tool-produces
     names as the form the razor USED to take — the addresses this run emitted MINUS the ones minted before it
     started a program, which is a PROXY for the markup door counted back. The sentence that stood here said a
     parser reaches the second set and only execution reaches the first, and that is the over-credit the
     paragraph below this one is about: it is true of the DOOR and only approximately true of the TIMING.
     IT IS ENTAILED BY THE TWO ROWS IT SITS BESIDE AND THEREFORE CARRIES ITS DERIVATION, which is the cure
     §EVIDENCE-INFLATION prescribes for a derived row: three rows here are TWO facts, and a reader counting
     zeroes must be able to see that from the output rather than by reading this file. The `Of` field is the
     derivation, spelled, so a row quoted out of this document into a brief carries what it is made of — which
     is the one copy a relay preserves.
     AND IT IS null RATHER THAN 0 WHEN EITHER HALF IS ABSENT. A build with no such counter emits nothing for
     it, and `0 - 0` would render a run that could not be asked identically to a run that was asked and
     contributed nothing — the absent-versus-zero pair landing on the one column the product is judged by.
     A DIAGNOSTIC AND NEVER A TARGET, on §netdiff's own terms: optimising toward a subtraction optimises the
     instrument. A zero here is a REFUSAL TO CLAIM the capability on this document, not a smaller version of
     it, and it is not comparable across two runs — it is an identity read WITHIN one. */
  /* AND IT IS NO LONGER THE RAZOR, WHICH IS THE FIRST THING A READER OF THIS NUMBER SHOULD BE TOLD AND WHICH
     THIS PARAGRAPH USED TO GET HALF RIGHT. It said the subtraction was no longer the ONLY statement available
     and named `endpointDoors`/`endpointMintedAt` as the rows beside it; that is true and it is weaker than
     what holds now. `epReach` on THIS census is CLAUDE.md §What-the-tool-produces' razor computed by the
     producer from a map no consumer can re-derive — which door a parse of the served bytes reaches — so the
     razor is a ROW and this line is a TIMING statement that was standing in for one.
     THE TWO ARE NOT TWO ANSWERS TO ONE QUESTION AND THE DIFFERENCE IS THE READING. `epEmitted - epPreProgram`
     counts rows minted AFTER the first program started, and solver/endpoint.h states why that is a proxy for
     the markup door and not the door: a `<head>` whose first `<script src>` runs before the parser reaches
     the `<link>` below it mints that link POST-program, so the subtraction credits a pure markup subresource
     to forced execution — in the flattering direction, on the commonest document shape there is. `epReach`
     asks WHICH DOOR and cannot make that mistake. So a nonzero `epBeyondMarkup` beside `epReach.beyond: 0` is
     that over-credit VISIBLE, on one line, from ONE document at ONE instant, which is what makes the pair
     worth carrying rather than one of them worth deleting.
     `endpointDoors`/`endpointMintedAt` ON THE SPREAD LINE ARE STILL A DIFFERENT DOCUMENT — the @H array
     bridge.js holds at composition, against this from the engine's `_cold` census — so no identity between
     THOSE and this is asserted anywhere and none may be read. `epReach` is the one that shares a document
     with the subtraction beside it. */
  o.epBeyondMarkup = (typeof o.epEmitted === "number" && typeof o.epPreProgram === "number")
    ? o.epEmitted - o.epPreProgram : null;
  o.epBeyondMarkupOf = (o.epBeyondMarkup === null) ? null : "epEmitted - epPreProgram";
  /* THE ORDER'S OWN CENSUS, AND `members: 0` IS NOT A READING. extension/bridge.js states the contract it
     asserts: no `wfq` is a BROKEN CONTRACT, `{members: 0}` is an EMPTY FRONTIER carrying NO term rows at
     all, and a full object is a READING. A finalize document is composed after the frontier drained or parked,
     so `members: 0` is the true reading of that instant and not of the run — its rows are absent, and they
     stay `null` here rather than becoming zeroes, because a frontier that was never observed standing and one
     observed with no backlog are different findings. */
  const w = ("wfq" in r) ? r.wfq : null;
  const live = w && typeof w === "object" && w.members > 0;
  o.wfqMembers = w && typeof w.members === "number" ? w.members : null;
  /* AND THE GATE IS BY THE PRODUCER'S DECLARED KIND AND NOT BY `live`, WHICH THIS LOOP DID FOR EVERY ROW.
     `members: 0` is a finalize census composed after the frontier drained or parked, so a GAUGE there is a
     reading of an instant this driver would rather report as unobserved than as 0 — that argument is the
     banner's above and is unchanged. It is WRONG for a LIFETIME count: those accumulated over the whole run
     and are not a statement about the instant at all, so nulling them because the frontier is momentarily
     empty discards the run's own totals and reads as an artifact too old to state them. The kinds are the
     PRODUCER's, read through census_rows.js, so this is not a second hand-kept list of which rows are which. */
  for (const k of WFQ_ROWS)
    if (k !== "members") o[k] = (live || WFQ_LIFETIME.has(k)) && typeof w[k] === "number" ? w[k] : null;
  /* THE THREE SHARES OF THE INSTANCE'S SPAN, COMPUTED HERE FOR `epBeyondMarkup`'s REASON — a derivation a
     reader must perform is one nobody performs, and these are the three a reader of the four time rows above
     will otherwise do by hand against the WRONG denominator. Each carries its derivation in an `…Of` field so
     a figure quoted out of this document into a brief carries what it is made of, which is the one copy a
     relay preserves, and each is `null` rather than 0 when either half is absent.
     WHAT EACH ONE SEPARATES, NAMED HERE BECAUSE A SHARE WITH NO READING IS A NUMBER NOBODY ACTS ON:
       `dispatchShare`  = stepUs / instanceUs — solver/engine.c's own words, "the share of the engine's thread
                          that reached a dispatch turn". ABOVE 1 is a broken reading and not a busy scheduler,
                          which is the direction its assert exists to catch and which this driver can see on a
                          RELEASE artifact where that assert is compiled out.
       `loopShare`      = loopUs / instanceUs — the share the engine held at all. A budget that is mostly its
                          COMPLEMENT is the host's relay and message pump rather than the engine, and the two
                          take opposite work.
       `meanSliceUs`    = sliceUs / slices — the mean slice against solver/quantum.h's quantum. FAR ABOVE it
                          with `sliceOverruns` small is the quantum not bounding slices at all, which is that
                          header's named transport gap (a C activation declaring no step boundary answers no
                          poll however the request was raised); NEAR it is a quantum doing its job and the cost
                          being the NUMBER of slices.
     A DIAGNOSTIC AND NEVER A TARGET: these are identities read WITHIN one run, not totals to compare across
     two, and §Testing's spread rule governs any comparison of them. */
  const spanOf = (num, den) => (typeof o[num] === "number" && typeof o[den] === "number" && o[den] > 0)
    ? o[num] / o[den] : null;
  o.dispatchShare = spanOf("stepUs", "instanceUs");
  o.dispatchShareOf = (o.dispatchShare === null) ? null : "stepUs / instanceUs";
  o.loopShare = spanOf("loopUs", "instanceUs");
  o.loopShareOf = (o.loopShare === null) ? null : "loopUs / instanceUs";
  o.meanSliceUs = spanOf("sliceUs", "slices");
  o.meanSliceUsOf = (o.meanSliceUs === null) ? null : "sliceUs / slices";
  /* AND THE TWO CONTRACTS THOSE SHARES REST ON, REFUSED BY NAME RATHER THAN LEFT TO A READER. Both are
     `DCHECKF`s at solver/engine.c's engine_step_unit_runs where every operand is in one hand, so a release
     artifact — the one a live drive measures — checks neither, and a share composed over a span that is not
     the sum of its halves is a share of nothing. The refusal is a STRING field for `absentRefused`'s reason:
     `null` means the run did not state the rows, and a message means it DID and they do not hold together. */
  o.spanRefused = null;
  if (typeof o.instanceUs === "number") {
    if (typeof o.stepUs === "number" && o.stepUs > o.instanceUs)
      o.spanRefused = "the dispatch loop's total " + o.stepUs + " exceeds the " + o.instanceUs +
        " the instance has measured since its first slice — every turn's charge is a sub-interval of that " +
        "span, so this is a baseline taken after a turn was charged or a measure that is no longer monotone, " +
        "and `dispatchShare` would read above 1, which is the one direction a reader takes for a busy " +
        "scheduler rather than for a broken reading";
    else if (typeof o.loopUs === "number" && typeof o.betweenSlicesUs === "number"
             && o.loopUs + o.betweenSlicesUs !== o.instanceUs)
      o.spanRefused = "the instance's span is not partitioned by its two halves (" + o.loopUs +
        " inside the slice bracket + " + o.betweenSlicesUs + " between slices against " + o.instanceUs +
        " measured since the first slice) — the readings TELESCOPE from engine_sched_step's own pair, so a " +
        "difference is a slice whose span was never charged, a second writer of one accumulator, or a clock " +
        "that is no longer monotone";
  }
  /* THE FOUR READINGS THE SCAN ROWS EXIST FOR, COMPUTED HERE FOR `epBeyondMarkup`'s REASON and each carrying
     its derivation, because a reader holding ten bare counts composes none of them:
       `scanPerStep`     = scanNextWeights / steps — the frontier the DISPATCH loop actually walked per step.
                           Read against `wfqMembers`: near it is an O(members) walk at every ask, and far below
                           it is a walk that stops early.
       `rivalPerFork`    = scanRivalRuns / forks — the producer's own named reading for the second cause, the
                           hook asking once per frontier GENERATION against a page that moves one per fork.
       `hookWeighShare`  = scanRivalWeights / (scanNextWeights + scanRivalWeights) — the share of all ORDERING
                           work done by the preempt hook, which is the share charged to `sliceUs` and therefore
                           the share `schedUs` cannot see. This is the number that settles whether a small
                           `dispatchShare`-era reading of `schedUs` was a bound on the ordering or only on the
                           pick.
       `censusWeighShare` = 2 * scanCensusWeights / (scanNextWeights + scanRivalWeights + 2*scanCensusWeights)
                           — the share of frontier-weighing spent REPORTING, doubled because the census weighs
                           twice per sample and the second walk lands in `scanOther*` where a shared entry
                           makes it unattributable. Large means this driver's own observation is changing the
                           run it measures, which no other row here can say.
     DIAGNOSTICS AND NEVER TARGETS, and identities read WITHIN one run rather than totals to compare across
     two. `null` rather than 0 when any operand is absent or the denominator is 0. */
  const num = (k) => (typeof o[k] === "number") ? o[k] : null;
  const ratio = (a, b) => (a === null || b === null || b <= 0) ? null : a / b;
  o.scanPerStep = ratio(num("scanNextWeights"), num("steps"));
  o.scanPerStepOf = (o.scanPerStep === null) ? null : "scanNextWeights / steps";
  o.rivalPerFork = ratio(num("scanRivalRuns"), num("forks"));
  o.rivalPerForkOf = (o.rivalPerFork === null) ? null : "scanRivalRuns / forks";
  const nw = num("scanNextWeights"), rw = num("scanRivalWeights"), cw = num("scanCensusWeights");
  o.hookWeighShare = (nw === null || rw === null) ? null : ratio(rw, nw + rw);
  o.hookWeighShareOf = (o.hookWeighShare === null) ? null
    : "scanRivalWeights / (scanNextWeights + scanRivalWeights)";
  /* AND WHAT THAT DENOMINATOR IS NOT OVER, DERIVED FROM THE CARRIED ENTRIES RATHER THAN NAMED HERE — the
     exclusion a reader cannot see from the quotient and the one a coordinator has already got wrong in print.
     `hookWeighShare` is a share of ORDERING weighing; `FLOW_SCANS` has entries this share's denominator omits,
     and omitting them is deliberate (OTHER is the best-and-eviction tail and CENSUS is this instrument's own
     cost, neither of which is the dispatch-versus-hook question). Printing the omitted set beside the figure is
     what stops `the hook does N% of ORDERING work` being relayed as `N% of all frontier-weighing`, which is the
     exact over-claim recorded at WFQ_SCAN_ROWS. It is composed from SCAN_ENTRIES, so an entry added to this
     driver appears in the exclusion the day it is carried and never on the day somebody remembers to say so. */
  o.hookWeighShareNotOf = (o.hookWeighShare === null) ? null
    : Array.from(SCAN_ENTRIES.keys()).filter((e) => e !== "Next" && e !== "Rival")
        .map((e) => "scan" + e + "Weights").join(" + ") || "nothing — this driver carries no other entry";
  o.censusWeighShare = (nw === null || rw === null || cw === null) ? null
    : ratio(2 * cw, nw + rw + 2 * cw);
  o.censusWeighShareOf = (o.censusWeighShare === null) ? null
    : "2*scanCensusWeights / (scanNextWeights + scanRivalWeights + 2*scanCensusWeights)";
  /* AND THE ONE CONTRACT THOSE RATES REST ON, REFUSED BY NAME. `scanRivalRuns <= preemptAsksLifetime` is a
     `DCHECK` at result.c, where both are in one hand and where the rescan branch sits INSIDE the policy that
     raises the ask — so the two are one event counted twice and a miss rate above 1 is a second caller of
     flow_rival_of rather than a busy hook. Release compiles that assert out, so this is the only place it is
     checked on the artifact a live drive measures. A STRING field, for `absentRefused`'s reason. */
  o.rivalMissRate = ratio(num("scanRivalRuns"), num("preemptAsksLifetime"));
  o.rivalMissRateOf = (o.rivalMissRate === null) ? null : "scanRivalRuns / preemptAsksLifetime";
  /* AND THE PARTITION'S OWN IDENTITY, which is the one thing that makes the three miss rows readable as a
     partition rather than as three rates. `rivalMissGen + rivalMissCur + rivalMissBoth == scanRivalRuns` is
     asserted at a `DCHECK` where all four are in one hand; release compiles it out, so a live drive measures
     an artifact on which nothing has checked it. A disagreement means a fourth invalidator was added without a
     row, at which point every share composed from the three is a share of the wrong total — so the parts are
     dropped and the refusal is named, exactly as the `outOfPrograms` partition does one list up. */
  o.rivalMissRefused = null;
  {
    const mg = num("rivalMissGen"), mc = num("rivalMissCur"), mb = num("rivalMissBoth"),
          rr = num("scanRivalRuns");
    if (mg !== null && mc !== null && mb !== null && rr !== null && mg + mc + mb !== rr) {
      o.rivalMissRefused = "the hook's miss rows " + mg + " + " + mc + " + " + mb + " sum to " +
        (mg + mc + mb) + " against `scanRivalRuns` " + rr + " — they are one if/else over one key at one " +
        "site and the engine DCHECKs the identity, so a difference on this RELEASE artifact is a fourth " +
        "invalidator added without a row and every share composed from the three is over the wrong total";
      o.rivalMissGen = o.rivalMissCur = o.rivalMissBoth = null;
    } else if (mc !== null && rr !== null && rr > 0) {
      /* THE ONE SHARE WORTH COMPOSING, AND ITS PRICE BESIDE IT RATHER THAN UNDER IT. `avoidableRivalShare` is
         the fraction of the hook's rescans whose ONLY invalidator was the excluded incumbent, and
         `rivalMissBoth` is carried unmixed because a `both` near `cur` means the repair buys nothing. Two
         numbers, one diff, two prices.
         THE CLAUSE NAMING THAT REPAIR IS RETIRED AND IS KEPT IN ITS OWN WORDS: it read `the half a top-two fold
         would answer without a walk`. The lane sent to build that fold refused it and solver/engine.h now
         carries the refutation — flow_silence_carry's bit moves with no generation bump behind it, so members
         within one FLOW_AGE_QUANTUM reorder between two rescans, and flow_pick_skipped drops the EXCLUDED member
         before flow_weight is ever called, so a rival scan holds no reading whatever of the one member a `cur`
         change re-admits. It is rewritten rather than deleted because a reader who re-derives it from the cache
         key will write it again: the key is a disjunction over the generation, so a generation that stood still
         reads as a frontier that stood still, and it is not.
         THIS IS THE FIFTH SITE OF THAT ARGUMENT AND THE LANE REPAIRED FOUR, which is the reason it is recorded
         here rather than silently corrected. The lane's scope was the solver and this clause is in the driver,
         so its grep for the retired argument could not reach it — CLAUDE.md's rule that a fix's CODE DELTA is
         not its SIZE, with the missing site one component over rather than one file over. What the share still
         measures is unchanged and is still worth taking: `cur` is the arm whose repair is cheap IF a margin
         exists, and `silPhases` below is the row that says whether one does. */
      o.avoidableRivalShare = mc / rr;
      o.avoidableRivalShareOf = "rivalMissCur / scanRivalRuns";
    }
    if (o.avoidableRivalShare === undefined) {
      o.avoidableRivalShare = null;
      o.avoidableRivalShareOf = null;
    }
  }
  /* AND THE TWO READINGS THE CARRY ROWS EXIST FOR, WHICH ARE THE PRICE OF THE MARGIN solver/engine.h NOW
     PRESCRIBES. Both are composed from GAUGES and are therefore statements about ONE census — never differenced,
     never accumulated — and both are read at the TERMINAL census or not at all.
       `phaseShare`      = silPhases / wfqMembers — how finely the frontier is split across sub-quantum residues.
                           solver/flow.h's own instruction is to read `silPhases` against `members` and NEVER
                           alone, because 1 at one member is the frontier being empty of the question and 1 at
                           tens of thousands is the finding. Near 0 is a frontier standing on few residues, where
                           the carry bit is close to a COMMON OFFSET and a margin is wide; near 1 is every member
                           its own group, where nearly any two members may reorder between generations and no
                           cached pair is sound at any margin.
       `phasesPerPick`   = silPhases / picksLifetime — flow.h's INHERITED-versus-EARNED reading. A member that has
                           never held the thread carries the phase it was FORKED with, so far below 1 says the
                           residues came from forking rather than from being charged. The two states recommend
                           different structures and no other row separates them.
     AND ONE PREDICATE RATHER THAN A MAGNITUDE, because flow.h states it as a RETIREMENT CONDITION on its own
     correction and a condition is answered yes or no: `carryCommonObserved` is `silPhases === 1` with more than
     one member, which flow.h says has NEVER been observed in this repository except degenerately — 48
     occurrences of `silPhases == 1` across its whole archived corpus and `members == 1` in all forty-eight. A
     true here on a real page is the observation that would make the strongest index design available, and a
     false is this driver declining to claim it. It is a PREDICATE and not a share for the reason CLAUDE.md gives
     where run lengths are uncontrolled: `did this ever happen` does not depend on how far a run got, and `how
     far did it get` is nothing but that.
     NEITHER FIGURE IS COMPARED AGAINST A STORED ONE. flow.h's two host readings disagree by two orders of
     magnitude AND in direction, so what is quoted from a drive is the pair plus its host's slice measure, and
     the derivation lives there rather than here.
     AND THE READING HAS NOW BEEN TAKEN, WHICH SETTLES WHICH OF THE TWO QUOTIENTS IS THE STABLE ONE AND PUTS
     flow.h'S PRESCRIBED ORDER THE WRONG WAY ROUND. flow.h says to read `sil_phases` against `members` and
     NEVER alone, and to read it against `picksLifetime` TOO — primary and secondary in that order. Over FOUR
     runs of ONE document in FOUR fresh browsers, `phasesPerPick` held inside a narrow band while `phaseShare`
     swung by more than a factor of two. **THE REASON IS IN THE DENOMINATORS AND IS NOT A PROPERTY OF THAT
     DOCUMENT**: only a CHARGED member mints a new residue, so `picksLifetime` is the population that PRODUCES
     phases, while `members` is decided by how far a wall-denominated run happened to get before its budget
     elapsed. One denominator is the cause of the numerator and the other is a lottery, which is why the
     secondary reading is the one to quote and the primary is the one to quote WITH ITS RUN COUNT.
     A FALLING `phasesPerPick` IS A DIFFERENT FINDING FROM A SWINGING `phaseShare`, AND THE SECOND DOCUMENT IS
     WHAT SEPARATES THEM. Where `phasesPerPick` falls MONOTONICALLY as picks rise, the residue count is
     SATURATING rather than the quotient being unstable — and saturation here is a CLOCK PIN and not a
     frontier fact, because `flow_silence_phase` is `flow_own_silence(f) % FLOW_SERVICE_US` (flow.c) and
     `FLOW_SERVICE_US` is `ENGINE_QUANTUM_MS * 1000` (flow.c, solver/engine.h), so the count of distinct
     residues cannot exceed that span divided by the clock's own granularity however large the frontier grows.
     flow.h records a vehicle reading pinned at `FLOW_SERVICE_US / 100`; a drive whose six microsecond
     accumulators share a gcd of 5 is pinned at `FLOW_SERVICE_US / 5` instead, which is why the pin is read off
     the RUN rather than copied from that record. THE DISCRIMINATOR IS ONE DIVISION AND NEEDS NO SECOND RUN:
     take the gcd of `instanceUs`, `loopUs`, `betweenSlicesUs`, `sliceUs`, `stepUs` and `schedUs` on the row in
     front of you, divide `sliceMs * 1000` by it, and compare `silPhases` against that ceiling. Near it, the
     figure is about the CLOCK and may not price anything; far below it, the figure is about the FRONTIER.
     NO MAGNITUDE FROM THAT DRIVE IS WRITTEN HERE AND THE DERIVATION IS WHAT IS HANDED OVER, because a live
     figure is a fact about a document at an hour and this file is the thing that takes it: run
     `node testing/harness.js restart` and `node testing/live-run.js 1 <url>` per run, ONE FRESH BROWSER EACH
     — the frontier is cross-session BY DESIGN and lives in RAM, so clearing storage does not reach it — and
     read the pair at the terminal census with `sliceMeasure` beside it. */
  const sp = num("silPhases"), sc = num("silCarry"), pk = num("picksLifetime"), mem = num("wfqMembers");
  o.phaseShare = ratio(sp, mem);
  o.phaseShareOf = (o.phaseShare === null) ? null : "silPhases / wfqMembers";
  o.phasesPerPick = ratio(sp, pk);
  o.phasesPerPickOf = (o.phasesPerPick === null) ? null : "silPhases / picksLifetime";
  o.carryCommonObserved = (sp === null || mem === null) ? null : (sp === 1 && mem > 1);
  /* AND WHAT `silCarry` ADDS THAT `silPhases` CANNOT, read at the same instant and refused when the two
     disagree about the population. The carry count is how many members stand on the far side of the boundary
     NOW, so 0 or `members` is the bit contributing nothing AT THAT SAMPLE — an observation about one census and
     never the structural claim `silPhases: 1` makes, which is the same two-states-one-number split result.c
     draws one scope down. A count ABOVE the member total is not a reading at all: the two are taken from one
     walk of one frontier, so it names a second writer or a sample composed across two walks. */
  o.carryInert = (sc === null || mem === null) ? null : (sc === 0 || sc === mem);
  o.carryRefused = (sc !== null && mem !== null && sc > mem)
    ? "`silCarry` " + sc + " exceeds `wfqMembers` " + mem + " — both are gauges taken from ONE walk of ONE " +
      "frontier at this census, so the carry cannot name more members than are standing. This is a second " +
      "writer of one accumulator or a sample composed across two walks, and every reading above is void"
    : null;
  o.scanRefused = (o.rivalMissRate !== null && o.rivalMissRate > 1)
    ? "the hook's rescan count " + o.scanRivalRuns + " exceeds the " + o.preemptAsksLifetime +
      " asks its own policy raised — the rescan branch is INSIDE that policy and runs after it raises its " +
      "count, and flow_rival_of has no other caller, so a rate above 1 is a second caller rather than a busy " +
      "hook and `rivalPerFork` beside it is a rate over a denominator that is not the population"
    : null;

  /* AND WHAT EVERY ORDERING FIGURE ABOVE WAS DENOMINATED IN, WHICH IS THE ONE FIELD ON THIS RECORD THAT
     QUALIFIES ALL OF THEM AND WHICH THIS DRIVER HAS NEVER ASKED FOR. solver/flow.h states it as the
     RETIREMENT CONDITION on its own correction — "it goes when a census row in this tree carries the host's
     slice measure beside it, so a live-page figure cannot be quoted without saying which clock it was
     denominated in" — and the WFQ_CARRY_ROWS banner above repeats the demand in this file's own words: what
     is quoted from a drive is the pair PLUS its host's slice measure. The engine composes it
     (solver/quantum.c's `quantum_json`), bridge.js relays it onto every run record this driver reads
     (`quantum: result._quantum`) and the popup renders it; this driver never asked, which is the same broken
     contract as a field written with no reader and is the defect `absent` below was taken for one rung out.
     WHY IT IS NOT A READING OF AN INSTANT AND NOT A TOTAL: it is a property of the HOST and the BUILD,
     constant for the session, and the reason flow.h's two prior live readings are not comparable with each
     other at all — one is thread-CPU and one is wall, and neither says so at the figure. A `phaseShare`
     quoted without it is a number whose denominator a later reader cannot reconstruct.
     THREE FIELDS AND NOT ONE, because each answers what the others cannot: `sliceIsCpu` is whether the
     loaded-machine caveat applies at all, `sliceMeasure` is what the slice was billed in instead, and
     `sliceMs` is how coarse the slicing was — and that last one is what stops `meanSliceUs` being priced
     against a constant this file names in PROSE. bridge.js DCHECKs all three, which a RELEASE artifact
     compiles out, so this is the only place the contract is checked on the artifact a live drive measures —
     exactly the argument `rivalMissRefused` and `scanRefused` above are made on.
     ABSENT AND MALFORMED ARE DIFFERENT FACTS AND NEITHER IS A DEFAULT. `null` is this driver's
     absent-versus-zero rule and means the run did not state the quantum; `sliceMeasureRefused` means it DID
     and a field is not the kind the producer composes, which is a fact about the seam and not about the host.
     The default a reader would reach for is `isCpu: false`, and that is the WRONG one to guess in both
     directions: guessed false it attaches a caveat to a host that has a real CPU clock, and guessed true it
     removes one from the host that ships. */
  {
    const q = ("quantum" in r && r.quantum && typeof r.quantum === "object" && !Array.isArray(r.quantum))
      ? r.quantum : null;
    const bad = [];
    if (q) {
      if (typeof q.measure !== "string" || q.measure === "") bad.push("`measure` is not a non-empty string");
      if (typeof q.isCpu !== "boolean") bad.push("`isCpu` is not a boolean");
      if (typeof q.sliceMs !== "number" || !Number.isFinite(q.sliceMs) || q.sliceMs <= 0)
        bad.push("`sliceMs` is not a positive finite number");
    }
    const ok = q !== null && bad.length === 0;
    o.sliceMeasure = ok ? q.measure : null;
    o.sliceIsCpu = ok ? q.isCpu : null;
    o.sliceMs = ok ? q.sliceMs : null;
    o.sliceMeasureRefused = (q !== null && bad.length)
      ? "the run stated a `_quantum` this driver cannot resolve — " + bad.join(", ") + ". solver/quantum.c " +
        "composes all three from compile-time constants of its own file and bridge.js DCHECKs each by name, " +
        "so on a release artifact a field of the wrong kind is a second composer of this seam rather than a " +
        "host without a clock, and every ordering figure on this row is a figure whose denomination is unknown"
      : null;
    /* AND THE ONE READING THE CARRIED `sliceMs` MAKES DERIVABLE RATHER THAN PROSE. `meanSliceUs`'s own banner
       above reads the mean slice "against solver/quantum.h's quantum" and names that quantum as a NUMBER in a
       comment, so the comparison a reader actually wants has been theirs to do by hand against a constant that
       moves in another file. ENGINE_QUANTUM_MS rides the record; the quotient is the overshoot, and FAR above 1
       with `sliceOverruns` small is that banner's own reading — a quantum not bounding slices at all, which on
       a host with no asynchronous edge is solver/quantum.h's named transport gap and not a busy box. It is
       `null` whenever either operand is, so a run that stated no quantum composes no overshoot. */
    o.sliceOvershoot = ratio(num("meanSliceUs"), o.sliceMs === null ? null : o.sliceMs * 1000);
    o.sliceOvershootOf = (o.sliceOvershoot === null) ? null : "meanSliceUs / (sliceMs * 1000)";
  }

  /* AND WHAT THE PAGE ASKED FOR AND DID NOT GET, WHICH IS THE ONE ABSENCE NOTHING ELSE ON THIS ROW CAN
     STATE. Every other column here is the engine reporting what it DID; this is solver/absent.c reporting
     what a document READ that a STANDARD owns and this realm does not answer. CLAUDE.md §NO-STUBS: a page
     writes `if (window.X)`, this engine does not have `X`, the read is CORRECTLY decided false — a real
     browser without it answers false too — the fallback branch runs, and every endpoint and sink behind the
     true branch goes unreachable with NOTHING THROWING. That is the one absence this project's forcing
     function cannot surface, so it is discoverable only by comparison and needs an instrument rather than a
     crash. The engine has been counting it and bridge.js has been relaying it onto every run record this
     driver reads; this driver never asked, which is the same broken contract as a field written with no
     reader and is the defect the census itself exists to make visible one rung out.
     BOTH NUMBERS OR NEITHER, because the FRACTION is the whole point: `absentOwed: 0` against
     `absentAsked > 0` is the positive statement that this engine answered every name the standards were
     asked for, and `absentOwed: 0` against `absentAsked: 0` is a census that was never reached. Opposite
     findings, one digit — so the pair comes back together from one reader or not at all.
     THE REFUSAL IS ITS OWN FIELD AND IS NOT A ZERO. `null` here is this driver's absent-versus-zero rule and
     means the run did not state the census; `absentRefused` means it DID and this driver cannot resolve it,
     which is a fact about the pair (the artifact's composer is not the tree's) and not a reading of the
     page. Collapsing the two would report a clean engine for as long as the drift stood. The message is a
     STRING deliberately: `spread` takes numbers only, so a refusal folded into the pair would be filtered
     out and print as `-`, which is the silence this row exists to end.
     NOT IN THE SPREAD SUMMARY, LIKE EVERY OTHER CENSUS ROW HERE — the summary reads `counters`, which holds
     run TOTALS read flat off the record, and this rides `frontier` with the other four censuses. It is
     printed per run in that row's own JSON, which is where a reader of repeated drives compares them. */
  const ab = absentPair(("absent" in r) ? r.absent : null);
  o.absentAsked = ab.err ? null : ab.asked;
  o.absentOwed = ab.err ? null : ab.owed;
  /* AND WHICH NAMES, WHICH IS THE ONLY HALF OF THIS PAIR A READER CAN ACT ON. `absentOwed: 3` says a
     document asked for three components this build does not have; the LIST says which three, and that is
     the work queue rather than a measurement. It is a field on this row and not a spread line for the same
     reason the numbers are: the summary compares WITHIN a kind and a list of names has no range.
     `[]` HERE IS A READING AND `null` IS NOT ONE — the empty list is the positive statement that this
     document read no name a standard owns and this realm lacks, which beside a nonzero `absentAsked` is the
     clean bill; `null` is the census not stated at all. absent_census.js has already asserted that these
     rows' buckets sum to `absentOwed`, so the two cannot disagree by the time they arrive here. */
  o.absentOwedNames = ab.err ? null : ab.names;
  if (ab.err) o.absentRefused = ab.err;
  return o;
}

async function oneRun(browser, pg, url, budgetMs, engineErrs) {
  const before = await snapshot(pg);
  const baseRows = before.rows.length;
  /* WHERE THIS RUN'S SLICE OF THE ENGINE'S PAGE ERRORS BEGINS. The buffer is the whole session's, for the
     same reason `baseRows` exists: a driver that cleared it per run could not tell a run that recorded
     nothing from a run whose errors arrived after its poll loop gave up. */
  const baseErrs = engineErrs === null ? 0 : engineErrs.length;
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
           /* AN HTTP REFUSAL IS A NUMBER, SO IT FELL PAST EVERY RUNG AND LANDED ON A VERDICT THAT MEANS THE
              OPPOSITE. `no-navigation` is reached only when `nav` is NOT a number, which catches a navigation
              that threw and one that created no Document; a 403 IS a number, so it used to arrive at
              `budget-elapsed(no-row)`, whose own comment two hundred lines up says it means "a document that
              was admitted and never provisioned an engine". THE SITE WOULD NOT SERVE US and THE SITE SERVED US
              AND THE ENGINE NEVER RAN are opposite facts, and only the second is a finding about this engine.
              IT IS NOT A LABELLING NICETY, BECAUSE THE COUNT BELOW IS A NEGATIVE FILTER. `runsThatWereSamples`
              excludes a verdict carrying "NOT a sample of this site" and counts everything else AS a sample, so
              a refusal was being tallied as a measurement of the site -- the several-nothings-behind-one-answer
              shape this ladder exists to prevent, arriving through the one nothing it did not separate. The
              suffix is what both consumers key on, which is why the token carries it rather than being a word
              of its own.
              MEASURED: a corpus drive reported `replit` as `budget-elapsed(no-row)` with the scheduler alive
              and not driving at both ends and `kicksRefusedDelta` 0 -- a row that reads as an engine defect and
              was an HTTP 403. The same drive had also been told by a `curl` carrying a browser user-agent that
              the site answered 200, which is a different client and is why the browser is the one that decides.
              THE RANGE IS 2xx AND 3xx ONLY. A final `nav` outside that is not a document this engine was given,
              and `testing/live-requests.js` carries the same rung for the same reason -- its ladder fell through
              a refusal to the literal "sample", which its own positive filter then counted. */
           : (nav < 200 || nav >= 400)
             ? "http-refused(" + nav + ")(NOT a sample of this site)"
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
    /* WHICH DOCUMENT EACH ROW IS ABOUT, WHICH THE PRODUCER WRITES AND NO CONSUMER READ. A run carries ONE row
       per engine `bridge.js` logged, and on a browser whose profile survived a restart it carries SEVERAL --
       because `harness.js restart` clears IndexedDB and the code cache and does NOT clear Chrome's
       session-restore state, so the previous tab re-opens and its engine reports beside this one's. Every
       other field below is then a per-row array with nothing anywhere naming the row's subject, so a run's
       output is UNATTRIBUTABLE FROM ITSELF: two documents' door histograms sit side by side and read as two
       samples of one site.
       IT IS THE READ-WITH-NO-WRITER DEFECT INVERTED, on the axis that decides what a razor figure is about.
       `url` is on the engine row already -- a crash row prints it -- so this was a written field with no
       reader, and CLAUDE.md's rule about that pair is exact: a name READ somewhere and WRITTEN nowhere is a
       broken contract, and so is its mirror. The cost was measured rather than imagined: a per-site pass
       whose profile was reused reported a door histogram containing `document-script:1` for a document that
       ships ZERO `<script src>` elements, and a `beyond` of 3 for a site that on an isolated browser CRASHES
       -- a crashed run's razor is UNSCORED, not small, and the reused profile hid that behind a plausible
       number. Nothing in the emitted run could have said so.
       null RATHER THAN A GUESS where the row does not carry one, per this file's absent-versus-zero rule: a
       row from an artifact that predates the field is a row this driver cannot attribute, which is a
       different fact from a row about a document with no address. */
    rowUrls: mine.map((r) => ("url" in r) ? r.url : null),
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
    /* WHAT THE COLD TIER ANSWERED, AND IT IS ITS OWN ARRAY FOR THE REASON THE CENSUS BELOW IT IS: `counters`
       holds totals over a run, and this holds a statement about ONE read that happened before the run began.
       `park` is already in COUNTERS and is a numerator with no denominator without this — a drive that reads
       `park: 2` on two visits and `resumed: 0` on both has observed the cross-session round trip failing, and
       until this row existed it could not say WHICH of three things it had observed: a store that was never
       written, a store whose entry is under the bundle id the previous visit saw (the page redeployed, and the
       miss is the key working), or a store holding this very key and refusing to answer it. bridge.js decides
       that in ONE transaction at the lookup; this is the field a driver reads it out of.
       `bundleId` IS WHAT MAKES TWO RUNS COMPARABLE. It is the half of the frontier key that is not already on
       the row as `url`, so two drives of one address reporting two different ids ARE the redeploy — which is
       the reading no column here could previously state and the one a reader has to rule out first. */
    cold: mine.map((r) => ({ lookup: ("coldLookup" in r) ? r.coldLookup : undefined,
                             other: ("coldOther" in r) ? r.coldOther : undefined,
                             bundleId: ("bundleId" in r) ? r.bundleId : undefined })),
    /* A SEPARATE ARRAY AND NOT MORE KEYS ON THE ROW ABOVE, aligned with it index for index. Every member of
       `counters` is a TOTAL over the run; every member of this is a census, and two of its four are gauges. A
       reader compares WITHIN a kind and never across, and one object holding both invites exactly the
       comparison neither supports — the same reason bridge.js keeps the four censuses as four objects. */
    /* WHICH MECHANISM COMPOSED EACH ADDRESS THE RUN EMITTED, AND WHETHER THE PAGE'S CODE HAD RUN WHEN IT
       DID — the pair that makes `endpoints` in COUNTERS readable and the pair `epBeyondMarkup` below is a
       SUBTRACTION over. That subtraction says how many addresses forced execution CAN HAVE contributed and
       names none of them, so a run reading `epBeyondMarkup: 0` and one reading 30 are two numbers with no row
       under either; these two are the rows. CLAUDE.md §What-the-tool-produces asks for exactly this as that
       record's retirement, extension/bridge.js composes both off the ONE `fetchCallSites` array the
       `endpoints` figure is the length of, and each is asserted there to SUM to it.
       IT IS ITS OWN ARRAY FOR `cold`'s REASON: every member of `counters` is a number and every member of
       this is a histogram, and one object holding both invites a comparison neither supports.
       `undefined` IS A RUN WHOSE RECORD PREDATES THE FIELD and is a different fact from a run whose surface
       was empty — the first is this driver reading an older relay, the second is a finding about the page —
       so the formatter below spells them `-` and `{}` and never folds either into the other. */
    doors: mine.map((r) => ({ doors: ("endpointDoors" in r) ? r.endpointDoors : undefined,
                              mintedAt: ("endpointMintedAt" in r) ? r.endpointMintedAt : undefined,
                              /* …AND THE THIRD: whether the run had DETERMINED each address, per address,
                                 off the emitted array. The two beside it are WHICH MECHANISM and WHEN, and
                                 neither reaches the bar — a literal chunk URL through `module-import` and
                                 one built out of the fragment come through the same door as each other.
                                 THIS CLAUSE USED TO CALL IT "THE ONLY ONE OF THE THREE THAT ANSWERS THE
                                 OWNER'S HARD BAR" AND IS REWRITTEN RATHER THAN DELETED, because a reader
                                 who re-derives it from the two rows above will write it again: it is an
                                 OPERAND of that bar and not the bar. It cannot see a row whose address was
                                 an ordinary determined string by the time it arrived AND whose bytes were
                                 never in the served document at all — a chunk list a reply carried — which
                                 clears the bar outright and lands in `concrete` here. `undefined` is a relay
                                 predating the field and is a different fact from a surface whose every
                                 address the run had determined, for the two rows above's reason. */
                              addressClass: ("endpointAddressClass" in r)
                                ? r.endpointAddressClass : undefined,
                              /* …AND THE BAR ITSELF, COMPOSED BY THE PRODUCER OUT OF THAT OPERAND AND THE
                                 DOOR'S OWN BYTES COLUMN. extension/bridge.js writes it on every engine-run
                                 record from solver/endpoint.c's per-row `razorClass`, and it is read here
                                 because a field the shipped path writes and the one instrument that
                                 measures a REAL SITE does not read is a producer with no reader on exactly
                                 the column this product is judged by.
                                 IT IS NOT UNIONED WITH ANYTHING HERE AND MAY NOT BE. The zone that relays
                                 it holds no door map, by its own argument, and neither does this file: a
                                 union assembled by a consumer out of `doors` and `addressClass` is the
                                 figure CLAUDE.md §What-the-tool-produces DEMOTED, because `beyond` is what
                                 a MARKUP parse cannot reach and `fetch`, `xhr` and `module-import` are all
                                 in it. `undefined` is a relay predating the field, for the rows above's
                                 reason. */
                              razorClass: ("endpointRazorClass" in r)
                                ? r.endpointRazorClass : undefined,
                              /* …AND WHOSE HOLE EACH NON-DETERMINED ADDRESS CARRIED, which is the one axis
                                 none of the three above can express and the one the bar's own open question
                                 is about. The rows beside it say WHICH DOOR, WHEN, and WHETHER THE RUN HAD
                                 DETERMINED the address; not one of them says, of an address the run did NOT
                                 determine, WHERE THE UNKNOWN CAME FROM. IT IS NOT THE WHOSE AXIS AND THE
                                 CLAUSE THAT STOOD HERE SAID IT WAS, FALSELY, AT THIS COMMIT'S OWN PARENT —
                                 it read "a hole this engine minted for its own orphan drive clears the hard
                                 bar in exactly the same bucket as one a page's own `location.hash` put
                                 there, which are opposite findings about the product", and the retired
                                 wording is kept because a reader who re-derives it from "an unknown is an
                                 unknown" will write it again. solver/engine.c mints a driven orphan's
                                 argument AND its receiver `CONCOLIC_WHOSE_INSTRUMENT`, saying at the site
                                 that the bar's own claim "is false of it: nothing was LEARNED, because the
                                 hole is ours"; `address_class_of` asks `concolic_root_whose_any(url,
                                 CONCOLIC_WHOSE_WORLD)` — the ONE live call to that predicate in
                                 solver/endpoint.c, five matched as a CONSTRUCT against ten matches of the
                                 bare name, the other four being prose — so an INSTRUMENT-only mask answers
                                 NO, the class is `unknown-unproven`, and `endpoint_razor_class_of` clears
                                 only on `EPA_UNKNOWN`. THE BAR ALREADY REFUSES THE ENGINE'S OWN HOLES, and
                                 the clause describing the world before it did was published in the series
                                 that ended it — which is the cost clause written out of what its author
                                 FOUND rather than what they LEFT.
                                 WHAT THE ROW ADDS IS THE NAMES, AND THAT IS WHAT MAKES THAT REFUSAL
                                 FALSIFIABLE RATHER THAN MERELY ASSERTED: a `runtime-only` row whose root is
                                 WHOLLY `{orphan<hex>…}` is that refusal regressed, which is the observation
                                 solver/endpoint.c's own HOW-ITS-ABSENCE-WOULD-SHOW clause names this
                                 emitted row for.
                                 THE NAMES MAY NOT BE PARSED FOR WHOSE — extension/bridge.js says so at this
                                 very histogram and solver/endpoint.c says why, the root being a SET whose
                                 walk is static to solver/concolic.c — so `(named)` holds THREE states and
                                 not one: a real source root; a JOINED set mixing a page source with an
                                 engine hole, which is the one case that DOES clear the bar and the one case
                                 this row cannot resolve; and a derivation that became its OWN root, whose
                                 mask is `UNSTATED` (solver/concolic.c's `root ? root : shape` at four code
                                 sites, each with `root_whose_or_unstated` beside it).
                                 `(unattributed)` IS A TRIPWIRE AND NOT HALF OF A SPLIT. It needs a concolic
                                 carrying a `src` and no `root`, which `concolic_alloc`'s own pair asserts
                                 forbid in dev (`!!src == !!root` and `!!root == !!root_whose`), so a nonzero
                                 bucket here is a release-build mask drop or a mint that has parted from
                                 those asserts — worth a column, and not an observation about the page.
                                 IT IS NOT A FOURTH OPERAND OF THE BAR AND MAY NOT BE UNIONED INTO IT.
                                 solver/endpoint.h states that case at `ENDPOINT_WITNESS_CLASSES` and it
                                 holds here for the same reason: `endpoint_razor_class_of` takes TWO ints,
                                 the address class and the door's bytes column, so the bar cannot ask a
                                 property of a VALUE at all, and a consumer that folded this in would turn a
                                 floor into a claim about provenance the producer never composed.
                                 THREE OF ITS FOUR BUCKETS ARE NOT THREE FACTS. solver/endpoint.c writes
                                 `null` on EXACTLY the rows whose `addr_class` is `EPA_CONCRETE` — read at
                                 the emitter, not inferred — so `(no-concolic)` is ENTAILED by this line's
                                 `addressClass: concrete` and corroborates nothing (CLAUDE.md
                                 §EVIDENCE-INFLATION); `(unstated)` is a relay predating the field. The
                                 observation is the `(named)`-against-`(unattributed)` split WITHIN the rows
                                 that carried an unknown, and nothing else on this line can make it.
                                 `undefined` IS A RELAY PREDATING THE FIELD, for the three rows above's
                                 reason exactly, and extension/bridge.js records that NO ARCHIVED ROW
                                 carried this key at all — so the first drive that reads it is the first
                                 reading of this axis on a real site rather than a re-reading of one. */
                              addressRoot: ("endpointAddressRoot" in r)
                                ? r.endpointAddressRoot : undefined })),
                              /* NAMED RESIDUAL — THE ROOT NAMES REACH NO READER, AND A READER FOR THEM
                                 MAY NOT GO HERE. A reader was landed here and is WITHDRAWN rather than
                                 kept, because it was a READ WITH NO WRITER: it took `r.fetchCallSites`
                                 off the engine-log row, and extension/bridge.js composes the four
                                 histograms into THAT record while attaching `fetchCallSites` to the
                                 ANALYSIS, 278 lines apart — two objects. Measured by driving it: the row
                                 printed `-` on both completed runs while `endpointAddressClass` beside it
                                 printed `concrete:198`, so the document was there and the field was on
                                 something else, and the `-` token asserted "a relay with no engine
                                 document", which was false.
                                 WHAT IS NOT COVERED: solver/endpoint.c emits each row's `addressRoot`
                                 STRING and no consumer anywhere reads it. The histogram above buckets
                                 every non-empty one to `(named)`, so `{orphan<hex>.argN}` — a spelling
                                 solver/engine.c alone produces and no document can write — and an
                                 injected member a page's own inline script wrote are indistinguishable in
                                 every artifact a reader has.
                                 WHAT THE NEXT DIFF BUILDS: a reader on the surface that HOLDS the rows,
                                 which is `globalStore.endpoints` as lib/endpoint-record.js shapes it, not
                                 a new field on the log record — extension/bridge.js states at the
                                 histogram that the names are DELIBERATELY not there and gives its reason,
                                 so adding them would silence a decision rather than fill a gap. What that
                                 diff owes first is whether `addressRoot` survives lib/merge.js into the
                                 record at all; it is read off `result.fetchCallSites` by a DCHECK in
                                 bridge.js before the merge, and whether it is carried past it is not
                                 established here.
                                 HOW ITS ABSENCE WOULD SHOW: a run whose razor reads `runtime-only` and
                                 whose roots a reader cannot attribute — the engine's own instrument hole
                                 and a page source printing one identical bucket, so the one observation
                                 solver/endpoint.c's own HOW-ITS-ABSENCE-WOULD-SHOW clause names this
                                 emitted row for cannot be made from any artifact. Observed wherever that
                                 bucket is read; not keyed to whichever row currently exhibits it. */
    frontier: mine.map(census),
    storeEndpointsDelta: (last.endpoints === null || before.endpoints === null)
      ? null : last.endpoints - before.endpoints,
    storeFindingsDelta: (last.findings === null || before.findings === null)
      ? null : last.findings - before.findings,
    pageConsole: pageConsole.slice(0, 8),
    /* THE ENGINE'S OWN PAGE ERRORS FOR THIS RUN, beside the tab's and never merged with them. */
    enginePageErrors: engineErrs === null ? null : distinctEngineErrors(engineErrs.slice(baseErrs)),
    enginePageErrorsTruncated: engineErrs === null ? null : (engineErrs.truncated === true),
  };
}

/* A HISTOGRAM RENDERED AS ITS OWN ROWS AND NEVER AS A SPREAD, because a range over buckets says nothing and
   because the three states a bucket map can be in are not one kind of number. `-` is ABSENT — the run record
   predates the field, which is this driver's absent-versus-zero rule and is the honest answer that the run did
   not state it; `{}` is a run that stated the partition of an EMPTY surface, which is a finding about the page;
   anything else is the partition. Sorted by count and then by name so two runs' strings are comparable by eye,
   which is the whole reason a driver prints a histogram at all. */
function hist(h) {
  if (h === undefined) return "-";
  const ks = Object.keys(h);
  if (!ks.length) return "{}";
  ks.sort((a, b) => (h[b] - h[a]) || (a < b ? -1 : a > b ? 1 : 0));
  return ks.map((k) => k + ":" + h[k]).join(",");
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
  /* THE KIND OF EVERY CENSUS ROW, TAKEN FROM THE COMPOSER THAT EMITS IT — §Testing: a quantity whose kind
     you cannot name FROM ITS OUTPUT is one you are not entitled to do arithmetic on, and the names do not
     say. This line used to be composed from four arrays here that were NAMED for a kind, which is a statement
     about the producer kept by hand at a consumer; it is now composed from the producer's own declaration, so
     a row whose kind this driver would have had to guess makes it THROW at startup naming that row.
     A FOURTH KIND APPEARS HERE THAT THE HAND LISTS COULD NOT STATE, AND IT IS THE ONE §Testing RECORDS BEING
     MISREAD. `deepest`, `completed` and `deepestLeft` were filed with the lifetime counts — true, because a
     maximum is monotone and may be differenced, and INSUFFICIENT, because a high-water mark SATURATES and
     then plateaus, so a plateau in one is not a ceiling and it is not comparable across two runs of different
     length. This file's own banner says that in prose twelve paragraphs up while the line beneath it said
     LIFETIME; the producer now says MAXIMUM and this prints what the producer says.
     AND HOW MUCH OF THE CENSUS HAS NO KIND IS NO LONGER PRINTED, BECAUSE IT CAN NO LONGER BE ANYTHING BUT
     ZERO. This line used to carry a count of the rows whose producers stated no kind, on the ground that a
     kind nobody has determined must not be invented and that a figure shrinking as the work is done beats a
     sentence that rots. Both halves of that were right and the count is still gone: `census_rows.js` REFUSES
     an undeclared row now, so the clause could only ever print a zero, and a clause that can only print one
     value is dead reporting that reads as live — CLAUDE.md §A-superseded-system-is-DELETED, where a
     superseded system kept beside its replacement hides the replacement's gaps. What this driver prints
     instead is nothing, which is the honest output: the producers are complete, and the fact that they are is
     asserted at the reader rather than reported here. */
  /* AND THAT EVERY ROW OF A COMPOSER THIS DRIVER TAKES WHOLE IS ON THAT LIST, asked BEFORE the kinds because
     it is the cheaper refusal and because a missing row would otherwise be reported as a kind partition with a
     hole in it rather than as a row nobody carries. `kindsOf` asks the other direction — a carried row no
     composer publishes — and the two together are the set equality; on its own it is satisfied by any subset,
     which is correct for a curated list and silent for a ladder. See COLD_WHOLE. */
  requireWhole(COLD_ROWS, COLD_WHOLE);
  /* AND THAT EVERY CARRIED ROW IS READ OUT OF THE OBJECT THAT PUBLISHES IT, which neither check above can ask
     and which this driver got WRONG: twelve `wfq` rows were listed under COLD_COUNTERS, read out of `r.cold`,
     and printed `null` on a 180 s real-page drive while both of those passed. Asked third because it is the
     cheapest of the three and because its failure makes the kind partition below a partition of rows the
     driver will never populate. */
  requireFrom({ cold: { rows: COLD_ROWS, composers: ["cold"].concat(COLD_WHOLE) },
                wfq:  { rows: WFQ_ROWS,  composers: ["wfq"] } });
  const K = kindsOf(COLD_ROWS.concat(WFQ_ROWS));
  /* AND WHETHER THE INSTALLED ARTIFACT CARRIES THE ROWS THIS DRIVER IS ABOUT TO ASK FOR, which none of the
     three refusals above can ask: every one of them checks this driver against the PRODUCERS IN THE TREE, and
     the program that answers a drive is the one that was BUILT. A row the build predates prints `-`
     identically to a row this run did not state, and those take opposite work — an install against a finding.
     PRINTED BEFORE THE FIRST ROW so that a `-` below is already explained, and REPORTED rather than REFUSED,
     because an artifact older than a row is the ordinary state of a tree that lands rows faster than it
     installs — refusing there would stop every drive whenever anyone adds a counter. The probe arms its own
     controls and says so in its reading; a VOID reading is a statement about the probe and never about the
     build, so it is printed whole rather than folded into an absence. */
  const askedRows = artifactRowsPresent(COLD_ROWS.concat(WFQ_ROWS));
  console.log("# artifactRows " + askedRows.reading +
              (askedRows.absent.length
                 ? " | ABSENT FROM THE INSTALLED wasm, so a `-` on these is THIS BUILD and not this run: " +
                   askedRows.absent.join(",")
                 : ""));
  const show = (names) => names.map((n) => OUT_NAME[n] || n).join(",");
  console.log("# frontier.* — LIFETIME (may be differenced): " + show(K.byKind.lifetime) + "," +
              ABSENT_ROWS.join(",") +
              ", and every `forkAt` row" +
              " | MAXIMA (monotone, so differenceable — but a HIGH-WATER MARK saturates and PLATEAUS, so a" +
              " plateau is NOT a ceiling and two runs of different length are not comparable on one): " +
              show(K.byKind.maximum) +
              " | UNITS: replayHits+replayLeftArms are ARMS (decision-vector slots), replayLeft is EVENTS" +
              /* SAID WHERE THE NUMBER IS READ AND NOT ONLY WHERE THE ROW IS FILED, on the clause above's own
                 precedent: a unit and a shape are facts a reader HOLDING the figure needs, and this table is
                 the one whose arms a reader will otherwise walk looking for a lowest zero. */
              " | epFetch*/epXhr* count STATES of a REQUEST-CONSTRUCTION machine, never calls — a deep fork" +
              " byte-copies one, so epFetchAskBeganLife and epXhrAskBeganLife are read as facts and NEVER" +
              " subtracted from; epFetchOutDiedAtLife and epXhrOutDiedAtLife are PARTITIONS over their own" +
              " machine's stage labels and NOT ladders (a 0 in one stage says nothing about its neighbours," +
              " and every stage is emitted including the zeroes). The two edges are NEVER summed: they count" +
              " two machines' states, and epXhr*'s are `send()`'s while its OFFER is raised one machine on" +
              " (epXhrAskPlacedLife and epXhrAskOfferedLife are two populations with no relation asserted)" +
              " | GAUGES (may FALL; never difference): " + show(K.byKind.gauge) +
              /* A KIND THAT IS NEITHER OF THE TWO ABOVE, STATED BECAUSE A ROW THAT IS NEITHER WOULD OTHERWISE
                 BE READ AS WHICHEVER LIST A READER'S EYE LANDED ON. These are written once at the seed and
                 never again, so they may be neither differenced nor read as a level — and `rowsAwaitingBytes`
                 in the gauge list is the live half they are read against. */
              " | CONSTANTS (written at seed, never again; neither differenced nor read as a level): " +
              show(K.byKind.constant));

  const { browser, extId } = await connect();
  try {
    const pg = await offscreenPage(browser, extId);
    const engineErrs = attachEnginePageErrors(pg);
    if (engineErrs === null)
      console.log("# enginePageErrors: ABSENT on every row — this driver could not attach to the offscreen " +
                  "console, so a `-` below is that failure and NOT a page that recorded no error.");
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
        const r = await oneRun(browser, pg, url, budgetMs, engineErrs);
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
        /* NOT A SPREAD, BECAUSE IT IS NOT A NUMBER AND BECAUSE THE SEQUENCE IS THE ANSWER. A range over words
           says nothing; what a reader of repeated drives needs is run 1's word beside run 2's, since the
           cross-session round trip is a claim about the SECOND visit and the pair is the whole observation:
           `unvisited` then `hit` is the frontier working, `unvisited` then `other-bundle` with two different
           bundle ids is a redeploy between the visits, and anything then `unread` is this zone's own store
           refusing a key it enumerates. `undefined` is a run whose record predates this field, which is a
           different fact from a run that never reached its lookup (`null`) and is not collapsed into it. */
        cold: rs.map((r) => (r.cold || []).map((c) => String(c.lookup) +
                            (c.bundleId === undefined || c.bundleId === null ? "" : "@" + c.bundleId) +
                            (c.other === undefined || c.other === null ? "" : "+" + c.other)).join("|") ||
                            "no-row"),
        endpoints: spread(rs, first("endpoints")),
        /* AND WHAT THOSE ENDPOINTS WERE, WHICH IS THE ONE COLUMN ON THIS LINE THAT CAN TELL A RUN THAT
           LEARNED A GATED API SURFACE FROM ONE THAT COUNTED A `<head>` BACK. It is NOT a spread, for `cold`'s
           reason one field up: the sequence is the answer, and a range over bucket names is not a quantity.
           READ WITH `epBeyondMarkup` IN `frontier` AND NOT INSTEAD OF IT. That subtraction is composed from
           the ENGINE's own census and this from the emitted array, so they are two documents at two instants
           and this driver asserts no identity between them — they are a CROSS-CHECK, and a door partition
           that is all markup beside a nonzero `epBeyondMarkup` is a disagreement worth opening, not a sum
           to reconcile. */
        endpointDoors: rs.map((r) => r.doors.map((d) => hist(d.doors)).join("|") || "no-row"),
        endpointMintedAt: rs.map((r) => r.doors.map((d) => hist(d.mintedAt)).join("|") || "no-row"),
        /* AND THE HARD BAR PER ADDRESS, BESIDE THE TWO ROWS THAT CANNOT STATE IT. Read WITH `epAddressClass`
           in `frontier` and not instead of it: that row is composed from the ENGINE's own census and this
           from the emitted array, so they are two documents at two instants and this driver asserts no
           identity between them — they are a CROSS-CHECK, and an `unknown: 0` here beside a nonzero one
           there is a disagreement worth opening rather than a pair to reconcile. It is the same relationship
           `endpointDoors` has with `epReach` one row up. */
        endpointAddressClass: rs.map((r) => r.doors.map((d) => hist(d.addressClass)).join("|") || "no-row"),
        /* …AND THE BAR, PER RUN, BESIDE THE OPERAND THAT CANNOT STATE IT. Read WITH `epRazorClass` in
           `frontier` and not instead of it, for `endpointAddressClass`'s reason exactly: that row is
           composed from the ENGINE's own census and this from the emitted array, so they are two documents
           at two instants and this driver asserts no identity between them — a `runtime-only: 0` here
           beside a nonzero one there is a disagreement worth opening rather than a pair to reconcile.
           IT IS THE COLUMN THIS PRODUCT IS JUDGED BY AND IT IS STILL A DIAGNOSTIC: a run whose whole
           surface reads `unproven` has REFUSED TO CLAIM the bar on that document, which is a finding about
           the run and never a target to optimise toward. */
        endpointRazorClass: rs.map((r) => r.doors.map((d) => hist(d.razorClass)).join("|") || "no-row"),
        /* …AND WHOSE HOLE PAID FOR IT, which is the column that separates this product working from this
           product measuring itself. A surface whose razor reads `runtime-only` on rows whose root names
           `{orphan<hex>.argN}` is the ENGINE'S OWN MINT clearing the engine's own bar; the same figure on
           rows naming a declared attacker source is the thing the tool exists to find. Both print an
           identical `endpointRazorClass`, so this is the only row on this line that can tell them apart.
           READ WITH `endpointAddressClass` ONE ROW UP AND NEVER AS A FOURTH SIGNAL: `(no-concolic)` is that
           row's `concrete` restated, by the emitter's own `addr_class == EPA_CONCRETE` arm, so the two rows
           carry THREE facts between them and not five. It composes no union and asserts no identity against
           `frontier`'s engine-side census, for `endpointAddressClass`'s reason exactly. */
        endpointAddressRoot: rs.map((r) => r.doors.map((d) => hist(d.addressRoot)).join("|") || "no-row"),
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
