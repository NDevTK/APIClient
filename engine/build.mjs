/* APIClient v2 build — minimal emscripten WASM build of CLEAN quickjs-ng + the
 * host scheduler entry (engine/host/main.c). Deliberately small: the old build
 * (COW barrier post-processing, wasm64, JSPI, Lexbor/Z3 link) is gone with the
 * fresh fork. Re-add each capability ONLY when the scheduler needs it, verified.
 *
 *   node engine/build.mjs                  -> engine/host/out/qjs.mjs + qjs.wasm (node smoke test)
 *   node engine/build.mjs native [min]     -> the smoke fixture built and run NATIVELY (the memory series)
 *   node engine/build.mjs native leak      -> the same under LeakSanitizer
 *   node engine/build.mjs native address   -> the same under AddressSanitizer
 *   node engine/build.mjs native cold      -> the CROSS-SESSION round trip: session one parks its frontier to a
 *                                            file, session two (a second process) resumes from it
 *
 * Build success/failure is the milestone-0 signal (does clean quickjs-ng compile
 * + link + boot). Design-correctness verification stays on the live Chrome
 * harness once the browser target is wired.
 */
import { spawnSync, spawn } from "node:child_process";
import { mkdirSync, existsSync, copyFileSync, readdirSync, writeFileSync, readFileSync, renameSync, rmSync, openSync, closeSync, symlinkSync } from "node:fs";
import { dirname, join, resolve, relative, isAbsolute, sep } from "node:path";
import { cpus } from "node:os";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { stampArtifact, gateRevision, revisionLines, revisionMoved } from "./gate_revision.mjs";
import { lexborSourceId, lexborNativeArchive } from "./lexbor_source.mjs";
import { childCpuSeconds, cpuText } from "./gate_cpu.mjs";

/* A RUN THAT NEVER RETURNS IS NOT A VERDICT, AND A WALL CLOCK CANNOT SAY WHY. Every program this file
   launches gets ONE budget and ONE backstop, and they measure DIFFERENT THINGS through DIFFERENT SIGNALS so
   that they can never collapse into one verdict. Declared here because the native targets run ~300 lines
   before the wasm ones and a `const` below them would be a TDZ throw.

   THE BUDGET IS CPU, WHICH IS THE THING THE QUESTION IS ABOUT. What this file wants to know is how much
   EXPLORATION the fixture was granted, and until this change it granted a stretch of WALL CLOCK — so the
   amount of work the smoke was allowed was a function of what the other nine agents on the box were doing.
   Measured on six consecutive builds of this project, each killed at the same fifteen wall-clock minutes:
   the run retired between 45158 and 68947 flows. That is a 53% spread in the budget, invisible in the report,
   and a reader comparing two revisions' smoke results was comparing two different machines. CLAUDE.md
   §Testing names the fix in the units to use — CPU actually consumed, RLIMIT_CPU — and the kernel enforces it
   for free: the child is exec'd under `ulimit -t`, exceeds it, and dies on SIGXCPU. THAT IS NOT A CAP ON THE
   FRONTIER (§NO BOUNDS is about the engine capping its own exploration and has never had anything to say
   about a harness refusing to wait forever); it is the same budget the wall clock was pretending to be,
   stated in units the machine cannot move.

   THE BACKSTOP IS THE WALL CLOCK, AND IT IS NOW ONLY FOR WHAT CPU CANNOT SEE. A deadlocked child consumes no
   CPU, so RLIMIT_CPU never fires on one and something else must. It is GENEROUS — four times the CPU budget,
   so it fires only on a child that averaged under a quarter of one core — it arrives as SIGTERM from THIS
   HARNESS where the budget arrives as SIGXCPU from the KERNEL, and it carries its own verdict and its own
   exit code. Both numbers are printed at every outcome and the verdict names which of them decided.

   AND RAISING THAT WALL NUMBER IS NOT RAISING A CAP, BECAUSE THE CAP MOVED TO THE KERNEL. The incident this
   backstop was written for — four abandoned `out/qjs.js` processes found at 84-97% CPU having run 1.9 to 4.1
   hours, which WERE the machine load six lanes then declined to measure against — is now impossible by
   construction rather than by the clock: a runaway can burn at most RUN_CPU_BUDGET_S of CPU before the kernel
   kills it, whatever the wall clock says. A process the wall backstop reaches is by definition one that was
   not consuming the machine. */
const RUN_CPU_BUDGET_S = 15 * 60;
/* THE HARD LIMIT IS THE FLOOR UNDER THE SOFT ONE. The kernel sends SIGXCPU at the soft limit, whose default
   disposition terminates — but a child that installed a handler and ignored it would run on, so the hard
   limit is what makes the budget unconditional (SIGKILL, unblockable). One minute apart, which is enough for
   an orderly SIGXCPU death and short enough that the runaway case is still bounded. */
const RUN_CPU_HARD_S = RUN_CPU_BUDGET_S + 60;
const RUN_DEADLOCK_MS = 4 * RUN_CPU_BUDGET_S * 1000;
/* THE BUDGET IS INSTALLED BY THE ONE PROGRAM THAT CAN INSTALL IT — node has no setrlimit — and the child is
   `exec`'d so the shell is REPLACED rather than left as a layer between this reporter and the signal it
   reads. A budget that silently failed to install would be the defaulted-field defect wearing a rlimit: the
   run would look measured and be unmeasured, so the failure to set it is a LOUD marker and a distinct exit
   code, never a silent continuation. `$0` is the wrapper's name, `$1`/`$2` the two limits, `$3` the program. */
const CPU_BUDGET_SH =
  'ulimit -H -t "$1" 2>/dev/null; ulimit -S -t "$2" || { echo "@BUDGET-NOT-INSTALLED"; exit 125; }; ' +
  'p="$3"; shift 3; exec "$p" "$@"';
const BUDGET_NOT_INSTALLED = "@BUDGET-NOT-INSTALLED";

/* THE QUIET LIST IS DECLARED AT MODULE SCOPE, ABOVE EVERY READER, and both halves of that matter.
   It must be ABOVE the `native` target, which reads it during module evaluation — a `const` below that
   point is a temporal dead zone and the target dies before compiling anything. It must be at DEPTH ZERO,
   because a declaration moved inside the function that reads it is invisible to the module-level reader
   below and the DEFAULT target dies instead. Both failures were made here, in that order, and neither is
   a syntax error: `node --check` passes on both, so only running each target catches them. */
const QUIET_WARNINGS = ["-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses",
  "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow", "-Wno-maybe-uninitialized",
  "-Wno-misleading-indentation", "-Wno-dangling-pointer", "-Wno-char-subscripts", "-Wno-implicit-fallthrough",
  "-Werror=implicit-function-declaration"];

/* THE CHILD-CPU METER MOVED TO engine/gate_cpu.mjs, WHOLE, because this file's copy was the CORRECT one of
   THREE and the other two were not — engine/wpt.mjs and engine/solvergate.mjs each carried a hand-copy that
   indexed /proc/self/stat two fields too low and so reported the PARENT's own `utime`/`stime`, ~0 by
   construction, as the killed child's consumption. One fact answered from three places is the defect; that two
   of the three were wrong is only how it surfaced. Nothing about the reading changed — fields 16 and 17,
   `getconf` for the units, `null` rather than a plausible 0.0 where a host cannot measure — and the shared
   reader additionally ASSERTS that its parse is anchored at field 3, which is the one thing that can go wrong
   with the arithmetic and the one thing nothing here could see. */
const loadNow = () => {
  try { return readFileSync("/proc/loadavg", "utf8").trim().split(/\s+/).slice(0, 3).join(" "); }
  catch { return "unknown"; }
};
/* `.signal` IS NOT THE HANG TEST, AND USING IT AS ONE MADE THIS REPORTER LIE. `spawnSync` sets `signal` for a
   child killed by ANY signal, so an ABORT — a DCHECK doing its job, arriving as SIGABRT — was reported as
   "DID NOT FINISH within 15 min", with a load average beside it inviting the reader to blame the machine. That
   is exactly the conflation this backstop exists to prevent, committed inside the backstop: measured, a run
   that reached its completion moment and then aborted at `idl_args.c:2148` was filed as a hang.
   The discriminator is node's own: a timeout kill sets `error.code === "ETIMEDOUT"`, and nothing else does. A
   signal without it is a CRASH, and a crash's `@WHY` is the result — it must not be dressed as a timing
   artifact.
   AND SIGXCPU IS THE THIRD THING THAT SIGNAL CAN BE, which is the whole reason the budget was moved to the
   kernel: it is neither a crash nor a hang, it is the run reaching the exact amount of CPU this file agreed to
   spend on it, and it is the only one of the three whose arrival is a fact about the TREE rather than about
   the machine.
   AND THE PARAGRAPH ABOVE WAS TRUE OF A HOST THAT IS NOT THE ONE THIS BUILD SHIPS. `signal` distinguishes an
   abort ONLY where the host can deliver one, and the WASM smoke's cannot — emscripten turns `abort()` into a
   thrown `RuntimeError` and node exits 1, which is also what an incomplete probe table exits with. So the
   discriminator that keeps a DCHECK from being misfiled is not `signal` at all; it is what the aborting
   macros WRITE. See `abortRecord`. Six outcomes, six reports, six exit codes.

   AND THE HUNG REPORT ASSERTED A CAUSE IT NEVER MEASURED. It said the kill meant "a defect in the fixture,
   not the engine" — a claim about which of two named causes held, made by a function whose children run
   `stdio: "inherit"`, so it has never seen a line of their output. Measured, it was backwards: the three runs
   it called HUNG reached `finished` 545-705 while every run it called FAILED died on an abort at 16-261, so
   it reported 20-40x MORE work completed as the worse verdict, and a reader who trusted it went looking for a
   stall in a frontier whose `blocked` and `owed` were 0 in every sample of every run. The kill cannot decide
   this — deciding it needs the child's census, which needs capturing stdout rather than inheriting it, and
   that is a real change and not this one. So it now names the discriminator and stops claiming the answer.

   AND THE VERDICT IS RETURNED RATHER THAN EXITED ON, BECAUSE A STAGE THAT EXITS IS A DOOR IN FRONT OF EVERY
   STAGE BEHIND IT — see the stage list at the bottom for the one that stood behind this exit.
   The `hint` is printed HERE, at the non-pass, which is also the only way it prints at all: the three call
   sites below each carried an `if (t.status !== 0)` block with a diagnostic in it, and every one of those
   blocks was DEAD CODE — this function had already exited on that exact condition. The lines naming the
   @COLDPARK census and the LeakSanitizer summary have never once reached a terminal. */
/* THE KILL NOW READS THE CENSUS, because the previous sentence here — "deciding it needs the child's census,
   which needs capturing stdout rather than inheriting it, and that is a real change and not this one" — was a
   gap DOCUMENTED where the fix belonged. It named the mechanism, named the obstacle, and left the reporter
   printing an instruction to a human about numbers the reporter itself could have read. Every child now runs
   through `runChild`, which gives the run a FILE instead of the terminal and hands the bytes here, so the two
   causes the paragraph above distinguishes are distinguished BY THIS FUNCTION.
   The samples compared are the last one and one an ABSOLUTE span of engine work behind it, never two adjacent
   ones, because the paragraph's own caveat — "a plateau that resolves is WFQ re-ranking, not a stall" — is
   exactly what two adjacent samples cannot see: one re-ranking pause between them reads as a stall, and a
   stall that happens to emit once reads as health. That window used to be HALF THE RUN, which made its left
   edge a function of where the budget ran out; the paragraph at the window's own definition says what that
   cost and why the width is fixed now.
   THE VERDICT IS STILL A NON-PASS AND STILL CODE 2. A healthy frontier that wanted more budget is a REAL
   change in this fixture — this same smoke terminated inside the backstop until it did not — so naming the
   cause is diagnosis, never permission to call the run green. */
/* THE FIELDS ARE NAMED AND THEIR ABSENCE THROWS, because the paragraph this function replaced read a field the
   census does not have. It told the reader "`finished` flat with `live` rising is the stall" — and @COLD had no
   `live`: it printed `flows` for the live count, and `live` was a @PROGRESS name. Written as a comparison that
   was the worst possible form of the mistake, `undefined > undefined`, which is FALSE for every input, so the
   stall arm could never once have fired and the discriminator would have answered "healthy" to a stall
   forever. A JS property read that answers `undefined` is this file's silent-fallback, and the fix is the same
   one C gets: name the contract and abort on the origin that breaks it.
   AND THE READER WAS RIGHT ABOUT THE NAME, WHICH IS WHY THE PRODUCER MOVED RATHER THAN THIS LIST. `flows` was
   the frontier's LIVE size and `flows` was ALSO the number of flows ever created, one line apart in two
   markers — and those are opposite verdicts on the same shape, since a frontier that stops growing has either
   RETIRED its members or PAGED them. solver/result.c names it `live` now, for what it counts, and the created
   total keeps `_flows` on the document where it is a TOTAL among totals. A counter named after something it
   does not count is worse than a missing one: it is a wrong answer that looks like a measurement. */
/* AND `finished` RISING IS NOT SUFFICIENT FOR "HEALTHY", WHICH THIS FUNCTION LEARNED FROM ITS OWN FIRST REAL
   READING. Measured on the full-document smoke: 150 @H samples, the last row to flip did so at sample 82, and
   the 68 samples after it moved NOTHING — sixteen rows still 0, every `-atsink` row among them — while @COLD's
   `finished` climbed the whole time and `blocked`/`owed` stayed 0. The first arm called that a healthy frontier
   that wanted more budget. It is not: the frontier was retiring flows steadily and NONE of that work advanced a
   single statement the fixture makes, which is a claim about the WFQ's value ordering and not about the clock.
   More budget helps the first state and does nothing for this one, so telling them apart is the whole point.
   The probe table is the second stream and the fixture's OWN measure of progress — @COLD says work is moving,
   @H says whether any of it arrived — so the discriminator reads both or it is guessing from half the evidence.
   The window is the same absolute window, for the same reason: one WFQ re-ranking pause is not a freeze. */
/* THE ROW SET IS THE CONTRACT AND THE READING IS A SUBSET OF IT, AND THE SET IS DERIVED — this reader's last
   hand-kept copy closed rather than extended by three more names. Its job is not that every row is compared
   (`hungCause` reads five of them): it is that a row solver/result.c STOPS EMITTING, or renames, fails the
   build at this line instead of being compared as `undefined` — which is FALSE for every input, so the arm
   reading it can never fire again — and that a row ADDED over there does not join the unchecked half by
   default, which a required-PRESENCE loop over a typed-out list structurally cannot deliver.
   THE MEASUREMENT THAT ENDED THE LIST IS THE SAME ONE `wfqFields()` WAS BUILT ON, TAKEN A SECOND TIME. That
   half of this file abandoned "adding a row there is a change in two places on purpose" on evidence: EIGHT
   rows had been added to the @WFQ census and to no reader, three of them in the hour the derivation was being
   written. The sentence that stood here said these three lists AGREED with their composers at that revision
   and were therefore a residual and not a defect — and it was TRUE WHEN WRITTEN AND WRONG WITHIN THE DAY.
   `result_cold_json` gained `replayHits`, `replayLeft` and `replayLeftArms`; none of the three reached this
   list; nothing broke, because `censusFields` requires the names it is GIVEN and is silent about a row it was
   never told about. Eleven rows over two censuses, by two lanes' counts, is not a list anybody is going to
   remember to update. It is a derivation somebody typed out.
   THE ARTIFACT IT DERIVES FROM IS THE COMPOSER'S OWN FORMAT STRING — `idlgen`'s argument one layer down: an
   auditor reads the declaration the producer already obeys and never a table of names beside it.
   THE PER-ROW READING NOTES THAT STOOD HERE ARE GONE WITH THE LIST, deliberately and not as collateral, and
   for a reason sharper than the @WFQ half's. Every one of them answered the question WHY IS THIS ROW LISTED —
   why `resumed` rides beside the three orphanClaims rows, why `pendReady` partitions `pend`, why `steps` is
   here rather than left to the histogram that is checked against it — and a DERIVED set does not list
   anything, so that question has no referent left. What each of them said about the row's MEANING is
   solver/result.c's own paragraph beside the row, one `git show` away and maintained where the row is
   written; what they said about this file's READING of it lives at the reading (`stepCostReading` states the
   ratio and its denomination, `programCursorReading` states what `deepest` and `completed` are read against,
   `coldPartition` states the two population splits). A second copy of either, kept in the file whose whole
   subject is what a second copy costs, is the thing being deleted here.
   THE OBJECT ROWS ARE NOT NUMBERS AND ARE NOT IN THIS SET — `stepUnits`, `stepUnitRuns` and `programCursors`
   are spliced with `%s` and are validated as PARTITIONS by `censusHistRows`, a contract a numeric-presence
   loop cannot state and would refuse outright. The split is taken from the CONVERSION in the format string
   and never from an exclusion list beside it, and `censusRowSet` is what refuses the day a fourth object
   arrives with no reader.
   MEMOIZED BECAUSE IT IS A FILE READ AND THE ANSWER CANNOT CHANGE UNDER ONE BUILD — and never evaluated at
   module load, for `wfqFields`' reason: `HOST` is a `const` far below this line, so a top-level derivation
   here would reach it in its temporal dead zone and `node --check` would pass on it. */
let g_coldFields = null;
const coldFields = () => (g_coldFields ??= censusRowSet(
  "solver/result.c", "char *result_cold_json(void)", "\n}\n",
  ["stepUnits", "stepUnitRuns", "programCursors"],
  "the @COLD reader states which rows it requires of the frontier census, and it takes that set from the " +
  "composer rather than from a list beside it"));
/* THE POPULATION SPLITS ARE PARTITIONS AND THE PARTITION IS THE CONTRACT, checked here for the reason
   `stepUnitReading` checks its histogram against `live`: two rows that are supposed to be the whole of a third
   are two numbers that can drift, and drifted they are WORSE than the one number they replaced, because each
   half still looks like a measurement. The engine asserts the same identity at engine_frontier_census, where
   all three are in one hand; a difference visible HERE and not there is a row lost between the census and this
   document, which is exactly what a renamed field does. Every reading below that quotes an arm goes through
   this first, so no verdict is composed from a census that is not internally true. */
function coldPartition(c, total, parts, where) {
  const sum = parts.reduce((t, p) => t + c[p], 0);
  if (sum !== c[total])
    throw new Error(`[build] the @COLD census's \`${total}\` is ${c[total]} while ${parts.join(" + ")} sums ` +
                    `to ${sum} — those rows are a PARTITION of it (every member is on exactly one side of ` +
                    `\`Flow.cand_src\` for the whole of its life), so a difference means the two sides have ` +
                    `stopped describing one population and no reading composed from either is about the run ` +
                    `that happened. solver/engine.c asserts the same identity at ${where}; a disagreement ` +
                    `visible here and not there is a row lost between the census and this document.`);
}
/* WHAT RETIRED, AND WHICH POPULATION IT WAS — the reading `finished` alone could never be, and a NAMED
   function for `stepUnitReading`'s reason: a reading that is a sentence spliced into the middle of a composer
   can only be exercised by running the whole build, so it is written where one census object is the whole of
   its input and it can be handed a line off a log.
   A frontier holds exploration flows and @S candidate sessions, and on a document that finds anything the
   second are the great majority of the members — so a retirement total is dominated by the search's own
   discards. An exploration flow that ran to its end is coverage this document GAINED; a candidate session that
   ran to its end is one derived payload that ran and did NOT fire, which is the search spending itself and
   buys coverage nothing. Stated at EVERY outcome, pass included, because the two are one number and take
   opposite work.
   THE PARTITION IS CHECKED HERE AND NOT ONLY AT THE VERDICT, because this is the reading that quotes the arms:
   a sentence composed from rows that do not sum to their total is a sentence about a frontier that was not
   there, and it would be printed at every outcome including the passes.
   THE ZERO ARM IS THE LOUD ONE and is named rather than left to be noticed in a pair of digits: a run whose
   entire retirement is candidates has retired no exploration at all, and that is the sentence a reader acts
   on. It is a reading of the counters and not a threshold — nothing here is compared against a proportion. */
function retiredReading(c) {
  coldPartition(c, "finished", ["finishedFlows", "finishedCands"], "engine_frontier_census");
  coldPartition(c, "sold", ["soldFlows", "soldCands"], "engine_frontier_census");
  /* AND THE PAGER'S OWN PARTITION, which is what makes the `sold` arm below a reading rather than a zero.
     engine_reclaim_tail leaves by exactly three doors and each counts itself on the line it leaves by. */
  coldPartition(c, "pagedAsks", ["pagedUnarmed", "pagedFloor", "sold"], "engine_frontier_census");
  return `retired: ${c.finished} (${c.finishedFlows} exploration flow(s), ` +
         `${c.finishedCands} @S candidate session(s))` +
         (c.finished === 0
           ? ` — nothing has retired in this run at all`
           : c.finishedFlows === 0
             ? ` — EVERY retirement in this run was a candidate session, so what this document has done is ` +
               `discard derived payloads that did not fire; not one exploration flow has reached its end and ` +
               `no coverage was gained by any of it`
             : c.finishedCands === 0
               ? ` — all of it exploration, so no @S candidate has finished a re-fire in this run`
               : ``) +
         (c.sold > 0
           ? `; sold ${c.sold} (${c.soldFlows} exploration, ${c.soldCands} candidate)` +
             (c.soldCands > 0
               ? ` — a paged candidate comes back WITHOUT its ladder (solver/flow.h: \`cand_surv\` and ` +
                 `\`cand_rung\` are readings of a re-execution and do not cross the cold tier), so those ` +
                 `${c.soldCands} cost the search a measured distance the exploration sales did not`
               : ``)
           /* THE ZERO ARM IS A SENTENCE AND NOT A BLANK, because `sold 0` was the one row here a reader could
              take three incompatible ways. `pagedAsks` is the positive statement: 0 asks means the allocator
              never refused this instance anything, so nothing was paged because nothing NEEDED to be — the RAM
              floor was not met and the pager is unexercised rather than broken. Asks with no sale is the loud
              case and splits again: refusals declined OUTSIDE the reclaim safepoint are pressure the frontier
              was never consulted about (arming, not paging, is what is short), while refusals answered at the
              FLOOR are the pager consulted and holding nothing but the running flow, which is the physical end
              of paging and the point at which the allocator's NULL is the honest answer. */
           : `; sold 0 of ${c.pagedAsks} allocator refusal(s)` +
             (c.pagedAsks === 0
               ? ` — the allocator never refused this instance anything, so the RAM floor was NOT met in this ` +
                 `run and the pager is unexercised rather than failing; the reclaim edge's write half ` +
                 `(engine_reclaim_tail, solver/reclaim.c) has had nothing to answer`
               : ` — ${c.pagedUnarmed} declined outside the reclaim safepoint and ${c.pagedFloor} answered at ` +
                 `the frontier's floor` +
                 (c.pagedFloor > 0
                   ? `; a floor answer is the pager holding nothing but the running flow, which is where ` +
                     `paging genuinely runs out`
                   : `; not one refusal reached the frontier at all, so every one of them arrived where the ` +
                     `safepoint is down — what is short here is ARMING and not members to sell`)));
}
/* THE FORK TABLE'S LARGEST ROW, AND ONLY THE LARGEST. decide.c's own note says the table is a CASCADE — a
   chain of gates produces rows in a geometric series and the last site in program order is always the biggest
   — so a list of every row is a list, and the one row plus how much of all forking it is, is the reading.
   `{}` is the positive statement that nothing forked, which on a bundle that should have branched on opaque
   input is the loudest thing in this sentence and must not render as a blank.
   AND THE OVERFLOW ROW IS NOT A PREDICATE, WHICH THIS READER STATED THAT IT WAS. decide.c's predicate table is
   FIXED-SIZE, and the mass no resident row can prove is its own is published as one prose-named row the
   composer renders in the same object under the same shape as a real key — deliberately, because "an
   undercount that says so is a measurement". Taking the argmax over the object therefore hands back a NON-SITE
   as the answer to "which predicate is growing the frontier", and counts the prose row among the
   `predicate(s)`. It had no reader that honoured it, and on a real document that row has LED the table by a
   wide margin — so the one line anybody reads named a bucket as the hot predicate, with a percentage beside it.
   AND A MECHANISM IS NOT A PREDICATE EITHER, WHICH IS THE SAME DEFECT ONE LAYER IN AND IS THE ONE THE DATA
   FORCED. This reader called every non-overflow row a `named predicate`, and the largest row of a real
   document is `(a function the page never called was driven — no predicate was asked)` — an ORPHAN DRIVE,
   which is a mechanism this tree performs and definitionally not a question the program asked. So the one line
   anybody reads answered "which predicate is growing the frontier" with a row that is not one, exactly as it
   did with the bucket, and the fix is the same: say WHAT KIND OF ROW leads. It is not to special-case that row
   into second place — it legitimately leads, and decide.h says why (a mechanism that fires on every orphan
   drive outweighs every individual branch beneath it).
   THE DISCRIMINATOR IS THE PRODUCER'S OWN, NOT A SNIFF. decide.c asserts at `fork_key_count` and again at the
   row loop of `decide_fork_json` that a constraint key opens on concolic_ident_compose's decimal length prefix
   and a mechanism row on `(`, and that its own bound member opens on `_`. That is the rule used here, byte for
   byte, so the two halves are one rule and not two spellings of one; a first byte outside all three THROWS,
   because a row read as whichever namespace it resembles is the merge those asserts exist to prevent.
   THE OVERFLOW ROW IS MATCHED BY NAME BEFORE THAT TEST. It is prose, so it opens on `(` exactly as a mechanism
   does, and it is the opposite kind of thing — a mechanism row names a site this tree wrote, the overflow row
   is the mass of the sites it CANNOT name. Split on the byte first and the largest thing in the object is
   filed under the population that is exact by construction.
   AND THE `spill > top` HEURISTIC IS GONE, REPLACED RATHER THAN KEPT BESIDE ITS REPLACEMENT. It was a proxy
   for the only question that matters about an exclusion — could a site the table dropped outrank the one it
   named — and it was a proxy in the wrong units twice over: it compared the spill against the largest row of
   EITHER population, so a mechanism row (exact, never at risk of eviction, and typically the largest thing
   here) decided a question about the predicates; and even against the right population it answered with a
   TOTAL where the question is about a SINGLE key. decide.c now emits Space-Saving's own bound — the most any
   absent site can have taken — so the question is answered exactly, and a proxy kept beside an exact answer is
   a second system whose only contribution is to disagree.
   NEITHER HALF DEFAULTS. Every value is checked as an integer in the range its class allows, every key is
   classified or the read THROWS — which is what makes the classes a partition rather than an arithmetic
   identity restating its own definition — and the bound's absence beside rows is refused rather than filled in — that absence is structurally "this census predates this reader", which is the one thing a stale
   @FORKAT line can be. `{}` is untouched and remains the positive statement that nothing forked, which on a
   bundle that should have branched on opaque input is the loudest thing in this sentence.
   IT TAKES THE TABLE AND NOT THE LOG for `retiredReading`'s reason: one object is the whole of its input, so
   it can be handed a line off a log and exercised without a build. */
function forkReading(t) {
  const keys = Object.keys(t);
  if (!keys.length) return `forks: NONE — not one predicate of this document ever split a flow`;
  const over = forkOverflowKey(), lightest = forkLightestKey();
  const siteOver = forkSiteOverflowKey(), siteLightest = forkSiteLightestKey();
  const mech = [], pred = [], site = [];
  let spill = 0, bound = null, siteSpill = 0, siteBound = null;
  for (const k of keys) {
    const v = t[k];
    /* A ROW IS AT LEAST ONE AND THE BOUND MAY BE ZERO, which is the one place their shapes differ and is
       exactly solver/decide.c's `fork_rows_sane`: a row is claimed BY an arrival and only grows, so a count of
       zero is a state that table cannot be in and a site published as having taken no forks is a site the
       reader will go and look for. The bound is a different kind of number and zero is its ordinary reading. */
    if (!Number.isInteger(v) || v < (k.startsWith("_") ? 0 : 1))
      throw new Error(`[build] the @FORKAT census carries ${JSON.stringify(v)} under ${JSON.stringify(k)} — ` +
                      `a row of it is a count of forks and is at least 1 (solver/decide.c's fork_rows_sane: a ` +
                      `row is claimed by an arrival and only grows, so ZERO is not a state that table can be ` +
                      `in and absence is its only way of saying "not seen"), and each of its two bound ` +
                      `members is a non-negative integer. Anything else is that composer having lost the row rather than a ` +
                      `table with a strange shape, and every percentage taken off it would be a reading of ` +
                      `that loss.`);
    /* THE TWO OVERFLOW ROWS ARE MATCHED BY NAME AND BEFORE ANY BYTE TEST, which is solver/decide.h's own
       rule and is why it must be stated twice now rather than once: both are prose opening on `(`, exactly
       like a mechanism row, and both are the OPPOSITE kind of thing — a mechanism row names a site this tree
       wrote, an overflow row is the mass of the sites its table CANNOT name. Splitting on the byte first
       files the largest thing in the object under the population that is exact by construction. */
    if (k === over) { spill = v; continue; }
    if (k === siteOver) { siteSpill = v; continue; }
    if (k.startsWith("_")) {
      if (k !== lightest && k !== siteLightest)
        throw new Error(`[build] the @FORKAT census carries an unknown member ${JSON.stringify(k)} — the ` +
                        `leading \`_\` is how solver/decide.c marks what is NOT a row, and this reader knows ` +
                        `exactly two of those (one bound per admitted table). A member it cannot name is ` +
                        `either summed into the forks as mass no fork produced or silently dropped out of a ` +
                        `partition it is supposed to close, so it stops here instead.`);
      if (k === lightest) bound = v; else siteBound = v;
      continue;
    }
    if (k.startsWith("(")) { mech.push(k); continue; }
    if (k[0] >= "0" && k[0] <= "9") { pred.push(k); continue; }
    /* AND THE THIRD POPULATION: A FORK THAT ASKED NOTHING THIS ENGINE COULD SPELL, NAMED BY THE VALUE'S OWN
       DISPLAY SHAPE. Its prefix is a byte solver/decide.c WRITES in front of that shape and never a property
       of the shape itself — a page can spell `(` or a decimal digit as a first byte as easily as anything
       else, so without the prefix this population would arrive scattered across the other two and the census
       would report a page's shapes as this tree's own call sites. */
    if (k[0] === "~") { site.push(k); continue; }
    throw new Error(`[build] the @FORKAT census carries a row ${JSON.stringify(k)} opening on none of its ` +
                    `namespaces — solver/decide.c asserts that a constraint key opens on ` +
                    `concolic_ident_compose's decimal length prefix, a mechanism row on \`(\`, an unnamed ` +
                    `fork site on \`~\` and the census's own bounds on \`_\`, and this object is ` +
                    `partitioned by that byte alone. A fourth spelling is read as whichever of the three ` +
                    `populations it resembles, which files a mechanism's exact count among the predicate ` +
                    `floors or a page's shape among rows a document cannot grow.`);
  }
  if (siteBound === null && bound !== null)
    throw new Error(`[build] the @FORKAT census carries \`${lightest}\` and not \`${siteLightest}\` — ` +
                    `solver/decide.c emits ONE bound per admitted table and emits both with every object ` +
                    `that has rows at all. A census carrying one of the pair was written by an engine that ` +
                    `knew only one admitted table, so its unnamed-fork sites — if it had any — are in no ` +
                    `namespace this reader can partition and would be summed into whichever population ` +
                    `their first byte resembles.`);
  if (bound === null)
    throw new Error(`[build] the @FORKAT census has ${keys.length} row(s) and no \`${lightest}\` member — ` +
                    `solver/decide.c emits that bound with every object that has rows at all, precisely so ` +
                    `its absence cannot be confused with a producer that never wrote it. So this line was ` +
                    `written by an engine older than this reader, and partitioning it would file that ` +
                    `engine's differently-named overflow bucket — prose, opening on \`(\` — among the ` +
                    `mechanism rows and report it as the largest site this tree names.`);
  if ((bound > 0) !== (spill > 0))
    throw new Error(`[build] the @FORKAT census bounds an absent site at ${bound} while its overflow row ` +
                    `holds ${spill} — those are one fact stated twice (a spill is an eviction's residue and ` +
                    `an eviction is the only way a key leaves that table), so this census contradicts itself ` +
                    `and neither number separates an EVICTED site from one this document never reached.`);
  if ((siteBound > 0) !== (siteSpill > 0))
    throw new Error(`[build] the @FORKAT census bounds an absent unnamed fork SITE at ${siteBound} while ` +
                    `that table's overflow row holds ${siteSpill} — one fact stated twice (a spill is an ` +
                    `eviction's residue and an eviction is the only way a key leaves that table), so this ` +
                    `census contradicts itself and neither number separates an EVICTED shape from one this ` +
                    `document never reached.`);
  const sum = (a) => a.reduce((n, k) => n + t[k], 0);
  const mechSum = sum(mech), predSum = sum(pred), siteSum = sum(site);
  const total = mechSum + predSum + spill + siteSum + siteSpill;
  /* AND A BOUND WITH NOTHING TO BOUND IS NOT A DOCUMENT THAT NEVER FORKED — that is `{}`, handled above.
     solver/decide.c emits this member only where it counted a fork, and a fork it counted is a row it filed,
     so the two arrive together. Refused rather than divided by: every percentage below is over this total. */
  if (total === 0)
    throw new Error(`[build] the @FORKAT census carries its bound member and not one row — solver/decide.c ` +
                    `emits that member only where it has counted a fork, and every fork it counts is filed ` +
                    `into a row, so this object is a composer that counted forks it did not publish. A ` +
                    `document that never forked is \`{}\` and is a different sentence entirely.`);
  /* THE PARTITION IS THE CONTRACT AND IT IS ENFORCED BY EXHAUSTIVE CLASSIFICATION, NOT BY AN IDENTITY. The
     mechanisms' exact counts, the predicates' floors and the errors summed once ARE Space-Saving's counters,
     so they close over the forks — and solver/decide.c asserts that at the composer, where `g_fork_total` is
     in hand. Here it is not: this object carries no total of its own, so `mech + pred + spill === total` would
     be its own definition restated and could not fail. What CAN fail is the classification, and every way it
     can is refused above — an unknown namespace, an unknown `_` member, a row below 1 — so the denominator
     below is every fork this census filed and the bound is outside it, which is the half a consumer owns.
     RESIDUAL: the rows are NOT checked against the engine's own fork total, which solver/result.c publishes as
     the @COLD census's `forks` from the same emission block (solver/engine.c prints @COLD then @FORKAT with
     @HEAP and @WFQ between). The next diff pairs the two BY INDEX within that block rather than by last-line —
     they are printed in order, so a truncated tail leaves one more @COLD than @FORKAT and a last-to-last
     comparison would false-throw on the artifact rather than on the census. Its absence shows as a dropped or
     double-counted row surviving into this reading in a RELEASE build, where decide_fork_json's own
     `sum == g_fork_total` DCHECK is compiled out and nothing else is looking. */
  const slots = hostDefine("solver/decide.c", "DECIDE_FORK_KEYS",
                           "the @FORKAT reader checks that a census reporting an eviction is one whose " +
                           "predicate table is actually full, which is the condition Space-Saving's bound " +
                           "holds under");
  if (spill > 0 && pred.length !== slots)
    throw new Error(`[build] the @FORKAT census reports ${spill} unattributable fork(s) with ${pred.length} ` +
                    `predicate row(s) in a table of ${slots} — a row is only ever displaced out of a FULL ` +
                    `table and rows are never released, so this pair cannot both be true. Space-Saving's ` +
                    `bound on an excluded site holds over a full table and nothing else, and the reading ` +
                    `below quotes it.`);
  const siteSlots = hostDefine("solver/decide.c", "DECIDE_SITE_KEYS",
                               "the @FORKAT reader checks that an unnamed-fork-site table reporting an " +
                               "eviction is actually full, which is the condition Space-Saving's bound holds " +
                               "under — the same check the predicate table gets, over its own size");
  if (siteSpill > 0 && site.length !== siteSlots)
    throw new Error(`[build] the @FORKAT census reports ${siteSpill} unattributable fork(s) at unnamed ` +
                    `sites with ${site.length} site row(s) in a table of ${siteSlots} — a row is only ever ` +
                    `displaced out of a FULL table and rows are never released, so this pair cannot both be ` +
                    `true.`);
  const pct = (v) => Math.round(100 * v / total);
  const top = (a) => (a.length ? a.reduce((x, k) => (t[k] > t[x] ? k : x), a[0]) : null);
  const tp = top(pred), tm = top(mech), ts = top(site);
  /* SENTENCE ONE IS THE QUESTION THIS CENSUS EXISTS FOR — which PREDICATE is growing the frontier — and it is
     answered out of the predicate population alone. */
  return `forks: ${total} — ` +
    (tp === null
      ? `NOT ONE of them asked a predicate this table could name`
      : `${predSum} (${pct(predSum)}%) at ${pred.length} named predicate(s), the largest ${pct(t[tp])}% at ` +
        `${JSON.stringify(tp)}, and every named count is a FLOOR: the hits that row can prove are its own`) +
    /* AND SENTENCE TWO IS THE OTHER POPULATION, NAMED AS WHAT IT IS. These rows are prose this tree writes at
       its own call sites, so a document cannot add one and none of them is a branch a program took — which is
       why they are exact, and why the largest of them leading says nothing whatever about a predicate. */
    `. ` + (tm === null
      ? `Every fork asked a predicate — no mechanism forked without one`
      : `${mechSum} (${pct(mechSum)}%) asked NO predicate at all: ${mech.length} mechanism(s), the largest ` +
        `${pct(t[tm])}% at ${JSON.stringify(tm)}, counted exactly — those rows are this tree's own call sites ` +
        `and not questions the program asked, so a document cannot add one and the largest of them leading ` +
        `is not an answer about a predicate`) +
    /* AND SENTENCE THREE IS THE POPULATION THAT ASKED NOTHING THIS ENGINE COULD SPELL, WHICH IS NEITHER OF
       THE OTHER TWO AND USED TO BE COUNTED AS ONE OF THEM. A site row names WHERE a fork happened — the
       value's own display shape — and never WHAT it asked: nothing was asked, both arms were kept and no
       constraint was recorded, so these are not predicates; and the names are the page's, so they are not
       this tree's call sites either. They are floors for the predicates' reason exactly, over their own
       admitted table. */
    `. ` + (ts === null
      ? (siteSum + siteSpill === 0
          ? `Every fork this document took asked a question this engine could spell`
          : `${siteSpill} (${pct(siteSpill)}%) forked over a value this engine could not spell and NOT ONE ` +
            `of those sites is named here`)
      : `${siteSum + siteSpill} (${pct(siteSum + siteSpill)}%) forked over a value this engine could not ` +
        `spell — no predicate, no constraint, no replay slot — at ${site.length} named site(s), the largest ` +
        `${pct(t[ts])}% at ${JSON.stringify(ts)}` +
        (siteSpill === 0
          ? `, and that table did not overflow, so every such site this document reached is named`
          : `, with ${siteSpill} (${pct(siteSpill)}%) the named shapes cannot prove is theirs and no site ` +
            `it dropped taking more than ${siteBound} fork(s)`)) +
    /* AND SENTENCE FOUR IS WHAT THE PREDICATE TABLE COULD NOT HOLD, WITH THE ONE NUMBER THAT MAKES THE ARGMAX
       ABOVE SAFE OR UNSAFE TO QUOTE. `tp` is non-null on every arm that reads it: a spill proves a full table, and
       the check above proves a full table has its rows. */
    `. ` + (spill === 0
      ? `decide.c's PREDICATE table did not overflow, so no predicate was excluded and a constraint key ` +
        `absent from these rows was never reached by this document`
      : `${spill} (${pct(spill)}%) is mass the named rows cannot prove is theirs` + (bound < t[tp]
        ? `, but no site the table dropped took more than ${bound} fork(s) — below the largest named ` +
          `predicate's floor (${t[tp]}), so nothing excluded can outrank it and the argmax above is safe to ` +
          `quote: that mass is a long tail, not one hot predicate that fell out of the table`
        : `, and a site it dropped may have taken up to ${bound} fork(s) — at least the largest named ` +
          `predicate's floor (${t[tp]}), so an excluded site may outrank every predicate named here and the ` +
          `argmax above is NOT worth quoting. solver/decide.h names what decides which keys a fixed census ` +
          `retains; a bigger table is not it`));
}
function probeFlips(out) {
  const rows = [];
  /* THE TABLE, AND NEVER PROSE ABOUT THE TABLE. test_forced.c's `probes_report` prints TWO shapes under this
     one marker: `@H   <row>: <why>` for each folded 0, and then `@H ` + the whole table + `=> OK|FAIL|
     INCOMPLETE`. A `why` is a SENTENCE, so it carries whatever `k=0` pairs its author needed to make the
     sentence say something — `(engine_orphan_census: asked=0, driven=0)`, `forked=0` — and a bare
     `(\S+)=([01])` reads those as tables. Measured on one smoke log: NINE rows where the run printed ONE,
     eight of them prose.
     A WRONG COUNT IS THE SMALLEST OF THE THREE THINGS THAT COSTS. `ever` is a union over every row, so the
     first `why` that ever spells `k=1` invents a statement the fixture does not make; and `answered`,
     `asked` and `unanswered` are read off the LAST row, so a run killed between a why line and the table it
     precedes reports `0/2 … asked driven` — a fabricated fraction, in the arms whose whole subject is
     killed runs.
     THE DISCRIMINATOR IS THE PRODUCER'S OWN TERMINATOR: the table is the line that ENDS in the verdict
     `probes_report` writes after it, and prose never carries one. */
  for (const m of out.matchAll(/^@H ((?:\S+=[01] )+)=> (?:OK|FAIL|INCOMPLETE)$/gm)) {
    const r = {};
    for (const [, k, v] of m[1].matchAll(/(\S+)=([01])\b/g)) r[k] = v === "1";
    if (Object.keys(r).length) rows.push(r);
  }
  return rows;
}
/* WHERE THE RUN GOT TO, IN THE FIXTURE'S OWN UNITS — AND THE ONE NUMBER THE VERDICT USED TO THROW AWAY.
   `hungCause` has always computed this (its `zero` list) and the verdict line has always cut it off:
   the verdict kept the cause's NAME and dropped everything in parentheses, which is exactly where
   every number lives. So the stage table read `HUNG — a HEALTHY FRONTIER THAT WANTED MORE BUDGET` on six
   consecutive builds of six different revisions, while underneath it the fixture answered 93/135, 96/138,
   96/138, 97/141, 98/141 and 128/141 — a run that reached 128 and a run that reached 93 produced the SAME
   line, and the 128 was the one nobody saw. That is a computed observation with no reader — the mirror of the
   defect §Architecture names, and harder to see, because the value was real, printed in the hint, and
   discarded by the one line anybody reads.
   IT IS A STATEMENT ABOUT THE DOCUMENT AND NOT ABOUT THE MACHINE, which is what makes it the right thing to
   put in a verdict: the fixture declares its own probe table (engine/host/test_forced.c), so this is not a
   stored expectation this file has to keep in step — nothing here knows what 141 is until the run says so,
   and a fixture that gains a statement moves both halves of the fraction by itself.
   ABSENCE IS REPORTED AS ABSENCE. A stage that drives no scheduler prints no @H at all (the two-instance ABI
   drive, the browser-process layer), and `0/0` for one of those would be the same lie as counting a marker no
   shipped path writes. */
/* AND IT IS A READING OF THE LAST INSTANT, WHICH IS ONLY HALF OF WHAT THE STREAM SAYS. `answered` is the last
   @H row, and a probe row can go 1→0 — this file says so itself, in the arm that calls that "the finding rather
   than the frontier's pace" — so a run that established a statement and then stopped making it reports the same
   fraction as one that never established it. Those are opposite findings and the count that separates them is
   the MONOTONE one: how many distinct rows were 1 at ANY sample. It is carried beside the instant reading and
   printed only where the two DIFFER, because a difference is the whole of the information: equal, it is noise
   in the one line everybody reads; unequal, it is a document that stopped saying something it had said. */
/* HOW FAR THE RUN HAD GOT WHEN EACH TABLE ABOVE WAS COMPOSED — test_forced.c's `@HWORK`, and it is the
   number the sentence under `standingText` used to INFER one bit of. `fixture_have_answers` gates the table on
   `engine_work_done()`, so the quantity that decides WHEN every measurement of a run is taken was itself on no
   line of the stream; the producer put it on one, and until this reader existed the only consumer of that line
   was a person.
   PAIRED BY INDEX AND NEVER BY LAST-LINE, which is the same rule the @FORKAT reader states for the @COLD block
   it is printed beside. `probes_report` writes this line IMMEDIATELY BEFORE the table it belongs to, in one
   function with no other emitter between them, so the i-th record belongs to the i-th table — and a run killed
   between the two leaves exactly ONE more record than tables, which is a real state and the only imbalance a
   truncation can produce. Any other disagreement is the producer having changed and throws, for the reason the
   @COLD and @WFQ field loops throw: a stream this reader can no longer pair is one whose numbers it would be
   attributing to the wrong table.
   THE FIELDS ARE THE PRODUCER'S OWN AND ARE NOT LISTED HERE. A hand list is what let SEVEN rows reach a
   census and no reader — see `censusComposerFields`.
   EVERY ROW IS A LIFETIME COUNTER and the producer says so where it emits them, so two samples MAY be
   differenced and the series is its own check: a lifetime counter cannot fall. test_forced.c DCHECKs exactly
   that at the emission and the DCHECK is compiled out of a release build, where this reader still runs — the
   same division of labour the `jobWGap` check below is written for. */
function probeWork(out) {
  const w = [];
  for (const m of out.matchAll(/^@HWORK (\{.*\})$/gm)) { try { w.push(JSON.parse(m[1])); } catch { /* truncated tail */ } }
  if (!w.length) return w;
  const fields = censusComposerFields("test_forced.c", 'printf("@HWORK {', '}\\n"',
    "`standingText` states how far a run had got when its probe table was composed, and that is the one " +
    "number deciding which of a 0 row's two readings the WHOLE table has").numeric;
  for (const r of w) for (const f of fields)
    if (typeof r[f] !== "number")
      throw new Error(`[build] an @HWORK record has no numeric \`${f}\` — this reader takes its field list ` +
                      `from test_forced.c's own printf (${fields.join(", ")}), so a row that is absent from ` +
                      `the line is one the producer stopped writing rather than one this file forgot.`);
  /* THE ONE IDENTITY THAT IS INTERNAL TO THIS LINE, and the fork total that falls out of it. `engine_work_done`
     is forks + flows + jobs + switches (solver/engine.c), and the producer deliberately does NOT restate the
     forks term because @COLD already spells it — so the identity a reader checks spans two lines and two
     CADENCES, which are taken at different instants and cannot be paired. What CAN be checked is the half that
     is one printf at one instant: the three named counters are non-negative, so the sum of them can never
     exceed the total. Its RESIDUE is then this instant's fork count, which is the @COLD row contemporaneous
     with the probe table and which no other line of this stream carries. */
  for (const r of w) {
    const named = r._switches + r._flows + r._jobsRun;
    if (!(r.workDone >= named))
      throw new Error(`[build] an @HWORK record states workDone ${r.workDone} against ${named} in its three ` +
                      `named counters — solver/engine.c's engine_work_done is those three plus the fork ` +
                      `total, every term of which only climbs, so a total below its own parts is one of the ` +
                      `four having acquired a second writer and every standing derived from this line is ` +
                      `about work nobody did.`);
    r.forks = r.workDone - named;
  }
  for (let i = 1; i < w.length; i++)
    if (w[i].workDone < w[i - 1].workDone)
      throw new Error(`[build] the @HWORK stream fell from ${w[i - 1].workDone} to ${w[i].workDone} — every ` +
                      `row of that line is a LIFETIME counter (engine.c refuses to reset the four at a ` +
                      `session boundary), so a fall means one of them is a gauge and every difference taken ` +
                      `across two samples of this stream is arithmetic about nothing. test_forced.c asserts ` +
                      `this at the emission; that DCHECK is compiled out of a release build and this is not.`);
  return w;
}
function probeStanding(out) {
  const rows = probeFlips(out);
  if (!rows.length) return null;
  const last = rows[rows.length - 1];
  const names = Object.keys(last);
  const ever = new Set();
  for (const r of rows) for (const k of Object.keys(r)) if (r[k]) ever.add(k);
  const work = probeWork(out);
  if (work.length && work.length !== rows.length && work.length !== rows.length + 1)
    throw new Error(`[build] this run printed ${work.length} @HWORK record(s) against ${rows.length} @H ` +
                    `table(s) — test_forced.c's probes_report writes the record on the line immediately ` +
                    `before the table, so the two streams are paired by index and a truncated tail can leave ` +
                    `at most ONE more record than tables. Any other count is an emitter between them that ` +
                    `this reader has not been told about, and pairing them anyway would attribute one ` +
                    `table's standing to another.`);
  return { answered: names.filter((k) => last[k]).length, asked: names.length,
           unanswered: names.filter((k) => !last[k]), ever: ever.size, samples: rows.length,
           /* THE RECORD CONTEMPORANEOUS WITH THE TABLE THE FRACTION ABOVE IS READ OFF, which is `rows.length - 1`
              and NOT the last record: a run killed while composing its final table leaves a record whose table
              does not exist, and quoting it would state a standing for a table nobody has. `null` where the
              artifact predates the producer, which is a positive statement — this stream said nothing about
              its own progress — and never a 0 for one that said 0. */
           work: work.length >= rows.length ? work[rows.length - 1] : null,
           first: work.length ? work[0] : null };
}
/* AND HOW MANY TIMES THE QUESTION WAS ASKED, WHICH `probeStanding` HAS ALWAYS COMPUTED AND THIS LINE HAS
   ALWAYS DROPPED. That is the defect the paragraph above this function describes — a computed observation
   with no reader — one field over, in the function that describes it.
   IT IS WHAT SEPARATES A SOLVER THAT RAN AND FOUND NOTHING FROM A RUN THAT NEVER STARTED, and those are the
   two readings of an all-zero table. solver/engine.c's `run_scheduler` calls the park hook at the TOP of its
   loop — before `engine_sched_step` has run once — and test_forced.c's `fixture_have_answers` prints on its
   first call and thereafter only every PROBE_SAMPLE_EVERY units of `engine_work_done`. So ONE table in the
   whole output is the table composed BEFORE THE FIRST STEP, and every 0 in it is a statement the run never
   reached rather than one it answered wrongly — which is the opposite reading from the one an all-zero table
   invites, and the one nothing in this file said. Measured: a smoke that aborted on its second scheduler step
   printed a single table reading 193 of 196 still 0, and the frontier under it (`@WFQ members: 1`,
   `@FORKAT {}`, `_sourceReads: 0`) was read as a scheduler that starves.
   AND IT IS MEASURED NOW RATHER THAN INFERRED — BUT NOT BY THE NUMBER THE PRODUCER OFFERED, and the
   difference is the whole of what this paragraph is worth. The sentence above is a claim about
   `run_scheduler`'s LOOP ORDER, held in a file that cannot check it and decided off the SAMPLE COUNT, which is
   a proxy for it. test_forced.c now prints `engine_work_done` beside every table (`probeWork`) and its own
   note names `workDone` 0 as the pre-first-step marker — SO THAT MARKER WAS DERIVED HERE BEFORE IT WAS BUILT
   ON, and it does not hold. `engine_work_done` is forks + FLOWS CREATED + jobs + switches, and
   `engine_sched_begin` seeds the frontier (flow_add, or cold_resume) BEFORE the loop the hook sits at the top
   of — so the pre-first-step table is composed at a work total of at least one flow and `workDone === 0` is
   reachable at no table of any run. An arm keyed on it would have been a reading with no state behind it,
   which is the mirror of the defect this whole comment is about.
   THE ROW THAT DOES ANSWER IT IS `_switches`, AND IT ANSWERS WITHOUT A CLAIM ABOUT THE LOOP AT ALL.
   `engine_sched_step` raises that counter beside its `flow_credit_pick`, which is the only site, so
   `_switches: 0` IS "no flow has ever been handed the thread" — a fact about DISPATCH, checkable at the
   counter, true whatever order the hook and the step stand in. That is what the loop-order sentence was
   reaching for, and it is the one form of it this file is entitled to state.
   THE SPAN IS THE OTHER HALF AND IT IS THE ONE A BISECT NEEDS. A row at 0 because a mechanism failed and a row
   at 0 because the run stopped earlier are the two readings this whole paragraph is about, and the fraction
   alone separates neither — what separates them is how far the two runs got, which is `first→last` in the
   engine's own units. It is a difference of LIFETIME counters and is therefore arithmetic a reader is entitled
   to; `probeWork` is where that kind is established. */
const standingText = (s) =>
  s === null ? "no @H probe stream in this run — this stage makes no statement of that kind"
             : `${s.answered}/${s.asked} of the fixture's statements answered` +
               (s.ever > s.answered ? ` (${s.ever} ever — ${s.ever - s.answered} went back to 0)` : "") +
               (s.work === null
                  /* NOT A ZERO, AND NOT THE OLD INFERENCE EITHER. An artifact older than the producer of that
                     line says nothing about its own progress, and this reports the absence as one rather than
                     re-deriving the sample-count proxy the paragraph above retires — a fallback to a reading
                     this file has just called a claim it cannot check would be that claim surviving its own
                     correction. */
                  ? `, over ${s.samples} @H table(s), NO @HWORK LINE — this artifact predates the progress ` +
                    `line, so how far the run had got when that table was composed is not in its output and ` +
                    `neither reading of a 0 row is established here`
                  : s.work._switches === 0
                    ? `, AT A TABLE COMPOSED BEFORE ANY FLOW WAS EVER DISPATCHED — MEASURED, not inferred ` +
                      `from the sample count: ${s.work.workDone} unit(s) of engine work stand at ${s.work._flows} ` +
                      `flow(s) created, ${s.work.forks} fork(s), ${s.work._jobsRun} job(s) run and ZERO ` +
                      `context switches, and engine_sched_step raises that last counter beside its only ` +
                      `flow_credit_pick. So every 0 above is a statement this run never REACHED and not one ` +
                      `it answered wrongly, and nothing here is a verdict on the mechanism any row names`
                    : `, over ${s.samples} @H table(s), the last composed at ${s.work.workDone} units of ` +
                      `engine work (${s.work.forks} forks, ${s.work._flows} flows, ${s.work._jobsRun} jobs ` +
                      `run, ${s.work._switches} switches)` +
                      (s.first.workDone === s.work.workDone
                         ? ` — the whole stream stands at that one point, so this run answered what it ` +
                           `answered without the work clock moving between its tables`
                         : ` from ${s.first.workDone} at the first, a span of ` +
                           `${s.work.workDone - s.first.workDone} units: a row still 0 across it is a ` +
                           `statement this run had the work to reach and did not, which is the OTHER reading ` +
                           `of a 0 and takes the mechanism rather than the budget`));

/* AN ABORT IS AN ABORT EVEN WHERE NO SIGNAL CAN CARRY IT, AND UNDER EMSCRIPTEN NONE CAN.
   `runOutcome`'s `.signal` arm exists precisely so that a DCHECK doing its job is never misfiled as a timing
   artifact — and for the WASM smoke, which is the stage this build is about, that arm is UNREACHABLE. The
   emscripten runtime does not deliver SIGABRT: `abort()` reaches `__abort_js`, which throws a JS
   `RuntimeError: Aborted(native code called abort())`, and node exits with STATUS 1. `test_forced.c`'s own
   completion path also exits 1 (`return h_ok ? 0 : 1`) when the probe table is merely incomplete. So the two
   most different outcomes this gate can produce — "a DCHECK named an unbuilt capability at a file:line" and
   "the frontier answered most of the fixture and not all of it" — arrived as the SAME `FAILED rc=1`, and the
   arm written to keep them apart only ever fires for the `native` targets.
   MEASURED, AND IT COST A READING: a smoke run whose last words were
   `@WHY §7.1.1 ToPrimitive ( input [ , preferredType ] ) reached with a REAL object … ROUTE THE CALL SITE`
   followed by `Aborted(native code called abort())` was read off the stage table as an incomplete probe
   table. The @WHY names the exact thing to build; the verdict discarded it and offered a row count instead.
   THE WITNESS IS WHAT THE ABORTING MACROS ACTUALLY WRITE, not a signal the host cannot send: `check.h`'s
   `@WHY`/`@E ` JSON line, and `engine/qjs/quickjs-check.h`'s prose `@WHY <msg> (<file>:<line>)`. Both abort
   on the next statement, so either one in a run's output means that run died at an assert. Matched at the
   START of a line and only in those two exact shapes, so a stage that quotes the token out of a source
   comment cannot manufacture one. */
const ABORT_WITNESS = /^@(?:WHY|E) (?:\{"phase":"assert"|.*\([^()]*:\d+\)$)/m;
const EMSCRIPTEN_ABORT = "Aborted(native code called abort())";

/* AND THE STAGE THAT DECLINED TO ASK ITS QUESTION AT ALL, which `skipped` below already reports for the cases
   THIS file can see — a program that did not link — and which was invisible for the cases only the CHILD can
   see. A gate whose first act is to establish that the artifact in front of it belongs to a nameable revision
   (§Testing) exits non-zero when it does not, and that arrived here as `FAILED rc=1`: indistinguishable from
   the layer under test being broken, and read as exactly that. The witness is a line the child WROTE, not its
   exit status, on the same argument BUDGET_NOT_INSTALLED is read that way — a status is a value a program may
   produce for some other reason entirely. The tag is left open (`[<gate>] REFUSED TO MEASURE: …`) so any gate
   can state it in its own name rather than this file keeping a list of which ones can. */
const REFUSED_WITNESS = /^\[[^\]\n]+\] REFUSED TO MEASURE: (.+)$/m;
function abortRecord(out) {
  const m = out.match(ABORT_WITNESS);
  /* THE WHOLE LINE, NOT THE WITNESS'S OWN MATCH — which is what `return m[0]` here was, and it discarded the
     record for exactly one of the two shapes. The prose alternative ends at `$`, so its match IS its line and
     nothing was ever wrong with it; the JSON alternative is a seven-word PREFIX, so every `check.h` abort
     arrived here as the literal `@WHY {"phase":"assert"` and nothing else — a cause that names no cause, in
     the one line anybody reads AND in the detail line under it, with the `cond`, the `at` and the `reason`
     the macro composed thrown away at this statement. It was relayed to another lane as the cause and cost
     that lane its own reading to find out the cause had been dropped, which is §A-FINDING-RELAYED with the
     finding removed before the relay. One regex serving two shapes can only ever be right about the one whose
     match happens to reach the end of its line, so the line is taken from the match's POSITION (`^` anchors
     it to a line start) and never from its extent. */
  if (m) return out.slice(m.index).split("\n", 1)[0];
  /* An abort with no assertion line above it is a RAW `abort()`/`assert` — still an abort, and the absence of
     the message is itself the finding (a site that should be using check.h). Said, rather than defaulted. */
  if (out.includes(EMSCRIPTEN_ABORT))
    return `${EMSCRIPTEN_ABORT} — with NO @WHY/@E line above it, so this abort came from a raw abort()/assert ` +
           `rather than from check.h, and it carries no message naming what to fix`;
  return null;
}

/* THE CAUSE'S NAME, FOR THE ONE LINE ANYBODY READS. A `split(" (")[0]` was the whole of this and it only
   worked for the arms that happen to open with a parenthesis: the two that do not — NOT ESTABLISHED, and the
   too-few-censuses arm — put a whole paragraph into the stage table, where the report pads every label to a
   column width. A name ends at the first " (" or the first " — ", whichever comes first, which is true of
   every arm because that is how each one is written. */
/* AND `check.h`'S LINE IS A RECORD, SO ITS NAME IS COMPOSED FROM ITS FIELDS RATHER THAN SLICED OFF ITS FRONT.
   The rule below ends a name at the first " — " and pads it into a table column, and the macro writes
   `@WHY {"phase":…,"cond":…,"at":…,"reason":…}` — so the ninety columns a name gets are spent on `phase` and
   on the C text of the condition, and the `reason` (the only field written for a reader) never starts before
   the cut. The two fields anybody acts on are WHERE it fired and WHAT it says, in that order, which is the
   order §AN-ASSERT-THAT-NAMES-A-REMEDY asks for: a remedy with an object.
   A LINE THIS CANNOT PARSE IS RETURNED WHOLE. The record is evidence; a name derived from a shape it does not
   have would be a guess replacing it, and the shape moving is itself the finding. The captured bytes are used
   as they were written rather than JSON-unescaped, so there is no arm here that can throw. */
const assertRecordName = (cause) => {
  const at = cause.match(/^@(?:WHY|E) \{.*"at":"((?:[^"\\]|\\.)*)"/);
  const why = cause.match(/^@(?:WHY|E) \{.*"reason":"((?:[^"\\]|\\.)*)"/);
  return at && why ? `${at[1]} ${why[1]}` : cause;
};

const causeName = (raw) => {
  const cause = assertRecordName(raw);
  const cut = [" (", " — "].map((d) => cause.indexOf(d)).filter((i) => i >= 0);
  const name = (cut.length ? cause.slice(0, Math.min(...cut)) : cause).trim();
  /* AND IT STAYS A TABLE CELL. `report` pads every label to one column width, so a verdict that runs to a
     paragraph reflows the whole stage list and the one line anybody reads becomes unreadable. The full record
     is printed in the detail line above it — this is the name, not the evidence. */
  return name.length > 90 ? name.slice(0, 89) + "…" : name;
};

/* THE ORDERING, MEASURED — the @WFQ line, whose bytes are solver/result.c's `result_wfq_json` (solver/flow.h's
   WfqCensus) and are the SAME BYTES the result document carries as `_wfq`. It is one composer with two
   emission sites and not two renderings: this stage's host publishes lines, the extension publishes documents,
   and until the composer moved into result.c the census existed only on the line — so it was written by a
   driver the production ABI never enters, and every ordering number this project has quoted is a reading of
   this one fixture.
   THE VERDICT BELOW USED TO ASSERT THIS CAUSE, which is the defect this function's own history already names
   one paragraph up: a reporter claiming which of two causes held, from evidence it did not have. "This is the
   WFQ's value ordering rather than the budget" is a statement about the two terms flow_weight is made of, and
   nothing in @COLD or @H carries either of them. The engine now emits them, so the claim is either supported
   by a number or it is not made.
   THE READING IS THE REWARD SPREAD AGAINST 1.0, because 1.0 is the optimism term's ENTIRE range: a frontier
   whose rewards differ by more than that is one whose ends the bonus can no longer reorder, so its bottom
   waits on the aging term alone — FLOW_AGE_RATE, about one point per second of unproductive thread time, per
   member ahead of it. `valZero` says whether anything is actually down there; a from-baseline flow (a
   candidate session, a joined document's boot flow) enters at reward 0 and is exactly that population.
   Same field contract as @COLD: the names are engine.c's printf, and an absent one throws rather than being
   silently compared as undefined. */
/* THE LIST IS DERIVED AND IS NO LONGER A LIST, which is this reader's own defect closed rather than another
   row added to it. What stood here was `result_wfq_json`'s format string retyped by hand, on the argument
   that "adding a row there is a change in two places on purpose" — and the measurement is that it never was.
   The second place does not get changed. This file already recorded THREE rows found months after they were
   added; at the revision this was written the count was EIGHT — `picksLive`, `picksMax`, `picksLifetime`,
   `topForgiven`, `scanCensusRuns`, `scanCensusWeights`, `workDone`, and the four of test_forced.c's progress
   line — and THREE of those landed in the hour this function was being written. A required-PRESENCE loop
   cannot notice a row that was never in its list, so every row added over there joins the unchecked half by
   default, is computed on every sample of every run, and is read by nothing.
   SO THE ROW SET COMES FROM THE COMPOSER THAT DECIDES IT (`censusComposerFields`), which is `hostDefine`'s
   rule for a constant and `forkCensusName`'s for a spelling, applied to the one fact this file kept a copy of.
   The contract is unchanged and is now unforgettable: a row solver/result.c stops emitting, or renames, fails
   at this line instead of being compared as `undefined` — which is FALSE for every input, so the arm reading
   it can never fire again.
   THE PER-ROW READING NOTES THAT STOOD HERE ARE GONE WITH THE LIST, deliberately and not as collateral. Every
   one of them restated a paragraph solver/result.c or solver/flow.h already carries beside the row itself —
   what `topSvc` says that `topSvcFam` cannot, why `visZero` is not `unrun`, what the three candidate rows
   separate — so they were second copies of the producer's own reasoning, kept in the file whose whole subject
   is what a second copy costs. The producer is one `git show` away and is where a row's meaning is maintained.
   WHAT SURVIVES IS THE READING THAT BELONGS TO THIS FILE AND TO NO OTHER, because it spans two lines and the
   producer of neither can state it: `delivReady` is NOT the @COLD census's `canDeliver`. That row is
   `flow_stack_empty && pending_ready`; this one subtracts the members flow_pick REFUSES for carrying the
   host-owed mark, so `canDeliver - delivReady` is exactly the population the ARM could serve and the PICK will
   not offer the thread to. And `nonrewardMax` is carried out of the engine rather than grepped for the reason
   `ageQuantum` is read and not restated — it is the bound `flow_nonreward` asserts, folding the optimism
   ceiling, the fitness ladder over FLOW_RUNGS_N and the aging's zero, and `hostDefine` reads plain numeric
   defines while that one is an expression.
   MEMOIZED BECAUSE IT IS A FILE READ AND THE ANSWER CANNOT CHANGE UNDER ONE BUILD — and never evaluated at
   module load, which is the shape that has already left an instrument throwing for every lane in this shared
   tree: `HOST` is a `const` far below this line, so a top-level derivation here would reach it in its temporal
   dead zone and `node --check` would pass on it. */
let g_wfqFields = null;
const wfqFields = () => (g_wfqFields ??= censusComposerFields(
  "solver/result.c", "char *result_wfq_json(void)", "\n}\n",
  "the @WFQ reader states which rows it requires of a census that claims an order, and it takes that set " +
  "from the composer rather than from a list beside it").numeric);
/* A CONSTANT OF THE ENGINE IS READ FROM THE ENGINE, NEVER RESTATED HERE — the rule `ageQuantum` was written
   for and which now has a second reader (`hungCause`'s census cadence), so it is one function rather than two
   copies of one regex. Throws on an absent or unparseable define, because the alternative is this reader
   quietly pricing something at a rate the engine stopped charging — the same class of defect as comparing an
   absent census field as undefined, and caught the same way. `HOST` is initialised long before any stage calls
   this. */
function hostDefine(file, name, why) {
  const m = readFileSync(join(HOST, file), "utf8")
    .match(new RegExp(`^#define\\s+${name}\\s+\\(?\\(?(?:\\(int64_t\\))?\\s*(\\d+)`, "m"));
  if (!m) throw new Error(`[build] cannot read \`${name}\` from engine/host/${file} — ${why}, and it will not ` +
                          `substitute a remembered value for one it cannot find.`);
  return Number(m[1]);
}
/* AND THE SAME RULE FOR A CENSUS'S FIELD LIST, WHICH IS THE ONE FACT THIS FILE KEPT A SECOND COPY OF.
   `hostDefine` and `forkCensusName` exist because a constant and a spelling belong to the engine; a census's
   ROW SET belongs to the engine in exactly the same way and was being retyped here, and the cost of that is
   not hypothetical and is not small. A required-PRESENCE loop cannot notice a row that was never in its list,
   so every row added over there and not here joins the unchecked half BY DEFAULT and is computed on every
   sample of every run and read by nothing. This file's own note records THREE such rows found months later;
   at the revision this function was written the count was SEVEN — `picksLive`, `picksMax`, `picksLifetime`,
   `topForgiven`, `scanCensusRuns`, `scanCensusWeights` and the four of test_forced.c's progress line — and two
   of those landed WHILE it was being written. A list that has been wrong seven times is not a list anybody is
   going to remember to update; it is a derivation somebody typed out.
   THE ARTIFACT IT DERIVES FROM IS THE COMPOSER'S OWN FORMAT STRING, which is the thing that decides what the
   line carries, so the rule and its check cannot drift. This is `idlgen`'s argument applied one layer down: an
   auditor reads the declaration the producer already obeys and never a table of names beside it.
   IT FAILS LOUD IN THE DIRECTION THAT MATTERS, which is the half a derivation has to answer for. §Testing's
   record is a scan that read a peer's half-written file and INVERTED its own signal, and a truncated read here
   would yield a SHORT list — a check that quietly got weaker, which is worse than a stale one. So the region
   is bounded by the composer's own closing bytes and a region that does not close throws, and a region that
   closes and names nothing throws: a composer emits rows or it is not this composer. What it cannot catch is a
   file mid-edit that happens to close, and that is why the region is the smallest one that contains the format
   string rather than the whole file.
   NO C STRING DECODING, on `forkCensusName`'s rule and for its reason: a name is matched as the bytes between
   `\"` and `\":%`, so a key that needed an escape would simply not be found, and not-found is this function's
   loud arm rather than its quiet one. */
function censusComposerFields(file, from, to, why) {
  const src = readFileSync(join(HOST, file), "utf8");
  const open = src.indexOf(from);
  if (open < 0)
    throw new Error(`[build] cannot find ${JSON.stringify(from)} in engine/host/${file} — ${why}, and this ` +
                    `reader takes the census's row set from that composer rather than from a list here. A ` +
                    `renamed or moved composer is a change this file must be told about, not one it guesses ` +
                    `a remembered list past.`);
  const close = src.indexOf(to, open);
  if (close < 0)
    throw new Error(`[build] the composer at ${JSON.stringify(from)} in engine/host/${file} does not reach ` +
                    `${JSON.stringify(to)} — ${why}. An unterminated region yields a SHORT row set, which is ` +
                    `a contract that silently got weaker rather than one that broke, so it stops here.`);
  const body = src.slice(open, close).replace(/\/\*[\s\S]*?\*\//g, " ").replace(/^[ \t]*\/\/.*$/gm, " ");
  /* SPLIT BY THE CONVERSION, BECAUSE THE TWO KINDS TAKE DIFFERENT CHECKS AND A CALLER THAT CONFLATES THEM
     THROWS ON A HEALTHY CENSUS. A row spliced in with `%s` is a nested composer's object — @COLD's `stepUnits`,
     `stepUnitRuns` and `programCursors` — and its rows are validated as a PARTITION by `censusHistRows`, which
     is a contract a numeric-presence loop cannot state and would refuse outright. Everything else is a number.
     The distinction is NAMED rather than filtered silently: a caller asks for the kind it can check, so the day
     a composer splices a fourth object the caller that wanted numbers is not handed one. */
  const numeric = [], object = [];
  for (const m of body.matchAll(/\\"([A-Za-z_][A-Za-z0-9_]*)\\":%([-0-9.]*)([a-zA-Z]+)/g)) {
    const to = m[3] === "s" ? object : numeric;
    if (!numeric.includes(m[1]) && !object.includes(m[1])) to.push(m[1]);
  }
  if (!numeric.length && !object.length)
    throw new Error(`[build] the composer at ${JSON.stringify(from)} in engine/host/${file} names no field ` +
                    `at all — ${why}. Every row it publishes is written \`\\"name\\":%…\` in its own format ` +
                    `string, so a composer with none is one whose shape this reader no longer recognises and ` +
                    `an empty required-field list would pass every census ever printed.`);
  return { numeric, object };
}
/* AND THE HALF A NUMERIC ROW SET CANNOT SPEAK FOR: WHICH OBJECTS THE COMPOSER SPLICES, AND WHETHER THIS FILE
   CHECKS THEM. `censusComposerFields` names the two kinds apart so that a caller asks for the one it can
   check; that is the whole of what it can do, because it does not know who its caller is. What it cannot
   notice is a composer that grows a FOURTH `%s` — the row is silently absent from `.numeric`, absent from
   every presence loop, computed on every census of every run, and read by nothing. That is precisely the
   defect the numeric derivation was built to end, arriving one KIND over and invisible to it.
   SO THE CALLER STATES WHICH OBJECTS IT VALIDATES AND THE MISMATCH THROWS, IN BOTH DIRECTIONS. The @COLD
   reader validates three as partitions (`censusHistRows`, called by name at each of its three readings,
   because a row reached through a computed key is a row no reader of this file can see); @HEAP and @SWAP
   validate none, and pass an EMPTY set to say so rather than by omission — "this census publishes no object"
   is a claim, and the day one does the reader that cannot check it is told instead of quietly dropping it.
   THE DIRECTION THAT IS ALREADY COVERED IS STILL CHECKED HERE, AND CHEAPLY: a renamed histogram would throw at
   `censusHistRows` when the reading runs, but only if that reading is reached, whereas this fires while the
   row set is being taken and names the composer. One throw, at the seam, for a set that is one line long. */
function censusRowSet(file, from, to, objects, why) {
  const d = censusComposerFields(file, from, to, why);
  const extra = d.object.filter((k) => !objects.includes(k));
  const gone = objects.filter((k) => !d.object.includes(k));
  if (extra.length || gone.length)
    throw new Error(`[build] the composer at ${JSON.stringify(from)} in engine/host/${file} splices ` +
                    `[${d.object.join(", ")}] as objects and this reader validates [${objects.join(", ")}] — ` +
                    `${why}. ` +
                    (extra.length
                      ? `${extra.join(", ")} is spliced and checked by NOTHING here: it is not a number, so no ` +
                        `presence loop can hold it, and it would be computed on every census of every run and ` +
                        `read by nobody — the same hole a hand-kept numeric list left, one kind over. Give it ` +
                        `a reader (\`censusHistRows\` is the shape, if it is a partition) and name it here. `
                      : ``) +
                    (gone.length
                      ? `${gone.join(", ")} is validated here and this composer no longer splices it, so the ` +
                        `reading that takes it apart is reading a row that has been renamed or dropped.`
                      : ``));
  return d.numeric;
}
/* THE SAME RULE FOR A STRING — the two members of decide.c's fork census that this file has to recognise BY
   NAME rather than by shape, because neither is a row and both are spelled in prose that only decide.c owns.
   A copy of either here would be a second spelling that goes stale in silence: a rename over there would leave
   this reader quietly reporting the overflow BUCKET as the hot predicate again, or summing the census's own
   BOUND into the forks as though it were mass — both invisible from either side, and the first is precisely
   the defect this reader exists to end. So they are read, and an unreadable one throws rather than being
   guessed past.
   IT REFUSES AN ESCAPED LITERAL RATHER THAN HALF-DECODING ONE. There is no C string unescaper here, and a key
   containing `\n` or `\x41` would be matched against the JSON's decoded bytes as the two characters it is
   written with — a comparison that silently never matches, which is exactly the same silence. decide.c's own
   note says the overflow row is named "under a name no constraint key can collide with because no key is
   prose", so a backslash appearing in either is a change of kind and this is where it stops.
   ONE READER FOR BOTH, which is the rule `hostDefine` above is written for applied to a second kind of fact: a
   second copy of this regex and this refusal would be two spellings of one contract, and the day one of them
   learned something the other would keep not knowing it. */
function forkCensusName(sym, why) {
  const file = "solver/decide.c";
  const m = readFileSync(join(HOST, file), "utf8")
    .match(new RegExp(`static\\s+const\\s+char\\s+${sym}\\s*\\[\\s*\\]\\s*=\\s*"([^"\\\\]*)"\\s*;`));
  if (!m)
    throw new Error(`[build] cannot read \`${sym}\` from engine/host/${file} as an unescaped literal — ` +
                    `${why}. It will not substitute a remembered spelling for one it cannot find, and it will ` +
                    `not half-decode an escaped one.`);
  return m[1];
}
const forkOverflowKey = () =>
  forkCensusName("OVERFLOW_KEY",
                 "the @FORKAT reader tells decide.c's overflow BUCKET from a real predicate by that exact " +
                 "name, and without it the largest row of that table is reported as the hot predicate even " +
                 "when it is the row the table could not hold");
const forkLightestKey = () =>
  forkCensusName("LIGHTEST_KEY",
                 "the @FORKAT reader tells decide.c's own BOUND — the most any site the table is not holding " +
                 "can have taken — from the rows by that exact name, and without it that bound is summed into " +
                 "the forks as mass no fork produced and every percentage is taken against the wrong " +
                 "denominator");
/* AND THE SAME PAIR FOR THE UNNAMED-FORK-SITE TABLE, READ RATHER THAN COPIED FOR THE SAME REASON. A bound is
   a statement about ONE table (Space-Saving's guarantee is about the table that did the evicting), so the two
   tables have two spills and two bounds and neither pair can answer the other's question. */
const forkSiteOverflowKey = () =>
  forkCensusName("SITE_OVERFLOW_KEY",
                 "the @FORKAT reader tells the unnamed-fork-site table's overflow BUCKET from the shapes it " +
                 "could name by that exact name — it is prose opening on `(` exactly as a mechanism row is, " +
                 "so without it the mass that table could not attribute is reported as one of this tree's " +
                 "own call sites");
const forkSiteLightestKey = () =>
  forkCensusName("SITE_LIGHTEST_KEY",
                 "the @FORKAT reader tells the unnamed-fork-site table's BOUND from the rows by that exact " +
                 "name, and without it that bound is summed into the forks as mass no fork produced");
/* flow.c's FLOW_AGE_QUANTUM, read from the two files that define its factors rather than copied. */
const ageQuantum = () =>
  hostDefine("solver/engine.h", "ENGINE_QUANTUM_MS",
             "the @WFQ reader prices the aging term from flow.c's FLOW_AGE_QUANTUM factors") * 1000 /
  hostDefine("solver/flow.c", "FLOW_SILENCE_US",
             "the @WFQ reader prices the aging term from flow.c's FLOW_AGE_QUANTUM factors");

function wfqReading(out) {
  const s = [];
  for (const m of out.matchAll(/^@WFQ (\{.*\})$/gm)) { try { s.push(JSON.parse(m[1])); } catch { /* truncated tail */ } }
  /* AND AN ABSENT CENSUS CARRIES THE SAME FIELDS AS A PRESENT ONE, because the caller renders `orderedBy` on
     every arm where `ordered` is false and `whose` on the arm where it is true — this arm used to carry only
     `text`, so a run that printed no census at all rendered "What IS ordering it is the undefined term", a
     sentence with the shape of a verdict and a producer's hole where the answer goes. */
  if (!s.length)
    return { ordered: false, orderedBy: "nothing this run said — it printed no census at all",
             whose: "nothing — there is no census to say whose reward the order is",
             text: "no @WFQ census in this run's output" };
  const w = s[s.length - 1];
  /* AN EMPTY FRONTIER IS A DIFFERENT FACT FROM AN UNORDERED ONE, AND THE CENSUS SAYS WHICH BY OMISSION.
     solver/result.c emits `{"members":0}` and NO term rows when the census is taken with nothing standing —
     rendering `valMin: 0, wTop: 0` there would be readings of an order that does not exist. The field loop
     below would then throw about a producer that is behaving correctly, and worse, the verdict computed from
     those zeroes would read `nothing — every term reads the same at both ends of this frontier`, which is a
     confident false statement about the ordering of a run that had simply finished. `members` is asserted
     unconditionally because it is the row that separates the two; the terms are asked for only where the
     producer states there was an order to report. */
  if (typeof w.members !== "number")
    throw new Error("[build] the @WFQ census has no numeric `members` — it is the row that says how many " +
                    "flows the order was taken over, and solver/result.c emits it on BOTH shapes of the " +
                    "census, so its absence is the composer having changed rather than an empty frontier.");
  if (w.members === 0)
    return { ordered: false, orderedBy: "nothing was standing — this census was taken over an empty frontier",
             whose: "nobody's — no flow was standing when this census was taken",
             text: `@WFQ: 0 members. The last census of this run was taken with no flow standing, so it ` +
                   `reports no order — which is a fact about WHEN it was taken and says nothing about how ` +
                   `the run was ordered. Read a census from while the frontier was live.` };
  const fields = wfqFields();
  for (const f of fields)
    if (typeof w[f] !== "number")
      throw new Error(`[build] the @WFQ census has no numeric \`${f}\` — this reader requires every row ` +
                      `solver/result.c's result_wfq_json publishes (${fields.join(", ")}), taken from that ` +
                      `composer's own format string rather than from a list here, so a renamed or dropped ` +
                      `row fails at this line instead of being silently compared as \`undefined\` — which is ` +
                      `FALSE for every input, so the arm reading it could never fire again. A census with ` +
                      `\`members: ${w.members}\` states that there WAS an order, so this is not the ` +
                      `empty-frontier shape handled above.`);
  /* THE CENSUS AGAINST ITSELF, on the two pairs whose rows are one fact twice over. The engine DCHECKs the
     first at flow_wfq_census and that check is compiled out of a release build, where this reader still runs;
     the second is not asserted anywhere, because it spans two rows that are computed in one walk and nothing
     downstream had ever read both. A gap the pick and the census measure with different comparators, or a
     visit count that disagrees with the population it is the extremum of, makes every job sentence below a
     reading of the disagreement rather than of the run. */
  if (!(w.jobWGap >= 0))
    throw new Error(`[build] the @WFQ census reports jobWGap ${w.jobWGap} — it is \`wTop\` minus the best ` +
                    `READY job holder's weight, and the front of the order cannot be behind a member of it, ` +
                    `so a negative gap is the pick and the census reading two different comparators.`);
  /* AND THE DISPATCH ROWS AGAINST THEMSELVES AND AGAINST THE ROW THEY WERE ADDED BESIDE. Three of these hold
     in one walk of flow_wfq_census, so none of them can be a reading of two instants: `picks_live` sums
     `f->picks` over the standing members while `picks_max` is their maximum and `never_picked` counts the ones
     at zero, and `Flow.picks` is written to 0 exactly once at flow_add and raised only by flow_credit_pick —
     which is why the last of these is a BICONDITIONAL and is exact rather than a guard that might fire on a
     healthy frontier. The lifetime bound is the one the engine states itself, and it is checked here for the
     reason `jobWGap` is: flow_wfq_census DCHECKs it and that DCHECK is compiled out of a release build, where
     this reader still runs.
     THE BICONDITIONAL IS WRITTEN OUT BECAUSE THIS FILE HAS ALREADY GOT ONE WRONG. The `jobsReady`/`jobWGap`
     pair was written as an equivalence, fired on the healthiest state its rows can produce, and had to be
     retired to an implication — so this one carries its derivation rather than its plausibility: over
     non-negative counts a SUM is zero exactly when every term is, and `neverPicked` is the count of the terms
     that are. Nothing about the ordering is inferred from either side. */
  if (!(w.picksMax <= w.picksLive))
    throw new Error(`[build] the @WFQ census reports picksMax ${w.picksMax} above picksLive ${w.picksLive} — ` +
                    `one walk of flow_wfq_census accumulates the sum and takes the maximum over the SAME ` +
                    `members and \`Flow.picks\` never falls, so a maximum above the sum is that walk reading ` +
                    `two populations and every sweep reading below is about a frontier that did not exist.`);
  if (!(w.picksLive <= w.picksLifetime))
    throw new Error(`[build] the @WFQ census reports picksLive ${w.picksLive} above picksLifetime ` +
                    `${w.picksLifetime} — flow_credit_pick raises the member's count and the runtime total in ` +
                    `one statement and is the only writer of either, so the live members cannot hold more ` +
                    `dispatches between them than the scheduler has ever made. solver/flow.c asserts this at ` +
                    `the census; that DCHECK is compiled out of a release build and this is not.`);
  if ((w.picksLive === 0) !== (w.neverPicked === w.members))
    throw new Error(`[build] the @WFQ census reports picksLive ${w.picksLive} with ${w.neverPicked} of ` +
                    `${w.members} members never picked — those are two readings of one walk over ` +
                    `non-negative counts, so the sum is zero exactly when every member is at zero, and a ` +
                    `disagreement means one of the two rows is not counting the members the other is.`);
  /* THE IMPLICATION AND NOT THE BICONDITIONAL, WHICH IS A CORRECTION THE EXERCISE MADE RATHER THAN A CAUTION.
     Written as `(ready === 0) !== (gap === 0)` this refused the state solver/result.c names by name — "0 with
     `jobsReady > 0` is the top of the queue holding a runnable job and no ordering problem at all" — so the
     assert would have fired on the healthiest reading the pair can produce, on real bytes, and been reported
     as an engine defect. Only one direction is the producer's claim: with no ready holder there is nothing to
     measure a distance to. */
  if (w.jobsReady === 0 && w.jobWGap !== 0)
    throw new Error(`[build] the @WFQ census reports no job waiting on rank and a gap of ${w.jobWGap} — the ` +
                    `gap is measured to the best READY holder, so with none there is nothing to measure to ` +
                    `and solver/result.c states the row is 0 by construction. A distance to a holder the ` +
                    `census did not count is the pick and the census disagreeing about who is waiting.`);
  /* THE SAME TWO CHECKS FOR THE DELIVERY BACKLOG, which is the identical shape over the identical rows and is
     stated separately for the identical reason: the C DCHECK on the sign is compiled out of a release build
     that this reader still runs on, and the implication is ONE-directional because with no ready holder there
     is nothing to measure a distance TO. A gap of 0 WITH a non-zero count is the state the row exists to
     report — solver/flow.h: "0 means the front of the queue ITSELF is holding an undelivered reply and the
     backlog is not an ordering problem at all" — and refusing it would fire on the healthiest reading the pair
     can produce, which is the mistake the job pair above already made once and had corrected.
     THE THIRD CHECK IS THE PARTITION, WHICH THE JOB ROWS CANNOT HAVE AND THESE CAN. solver/flow.h states the
     three delivery rows are "disjoint and exhaustive over the members that hold one", so their sum is a count
     of MEMBERS and is bounded by `members`; the job three are over JOBS and have no such total here. A sum
     above the frontier's own size is three counts taken over different populations, and every sentence below
     would then be a reading of the disagreement. */
  if (!(w.delivWGap >= 0))
    throw new Error(`[build] the @WFQ census reports delivWGap ${w.delivWGap} — it is \`wTop\` minus the best ` +
                    `READY reply holder's weight, and the front of the order cannot be behind a member of it, ` +
                    `so a negative gap is the pick and the census reading two different comparators.`);
  if (w.delivReady === 0 && w.delivWGap !== 0)
    throw new Error(`[build] the @WFQ census reports no reply holder waiting on rank and a gap of ` +
                    `${w.delivWGap} — the gap is measured to the best READY holder, so with none there is ` +
                    `nothing to measure to and solver/flow.h states the row is 0 for that too. A distance to ` +
                    `a holder the census did not count is the pick and the census disagreeing about who is ` +
                    `waiting.`);
  if (w.delivReady + w.delivFramed + w.delivOwed > w.members)
    throw new Error(`[build] the @WFQ census reports ${w.delivReady} ready + ${w.delivFramed} framed + ` +
                    `${w.delivOwed} host-owed reply holders against ${w.members} members — solver/flow.h ` +
                    `states the three are disjoint and exhaustive over the members that hold an undelivered ` +
                    `reply, so a sum above the frontier is three counts over different populations and not a ` +
                    `backlog.`);
  /* THE SAME TWO CHECKS FOR THE STARVATION PAIR, which is the same shape one row over and is worth stating
     separately for the reason the job pair is: the C DCHECK on the sign is compiled out of a release build and
     this reader still runs, and the population check spans two rows that no assert anywhere covers. The
     implication is again ONE-directional — with nobody starved there is no member to measure a distance to, so
     the gap is 0 by construction; a gap of 0 WITH a non-zero count is the state the row exists to report and
     must not be refused, because it is what a TIE reads as. That last clause used to end "because it is
     precisely the razor's STARVES", and it was the same retired sentence solver/flow.h's `never_picked` block
     carried: flow_pick seeds the incumbent and compares STRICTLY, so it returns ONE of N equal maxima and the
     other N-1 stand exactly at the front with `picks == 0` — the ordinary state of a one-family frontier, not
     a verdict about one. The check is unchanged and was always right; only its reason was wrong. */
  if (!(w.neverPickedGap >= 0))
    throw new Error(`[build] the @WFQ census reports neverPickedGap ${w.neverPickedGap} — it is \`wTop\` ` +
                    `minus the best NEVER-DISPATCHED member's weight, and the front of the order cannot be ` +
                    `behind a member of it, so a negative gap is the pick and the census reading two ` +
                    `different comparators.`);
  if (w.neverPicked < 0 || w.neverPicked > w.members || (w.neverPicked === 0 && w.neverPickedGap !== 0))
    throw new Error(`[build] the @WFQ census reports neverPicked ${w.neverPicked} of ${w.members} members ` +
                    `with a gap of ${w.neverPickedGap} — the population is counted over the members this ` +
                    `walk visits and the gap is measured to the best of it, so with none there is nothing to ` +
                    `measure to and solver/result.c states the row is 0 by construction.`);
  /* THE LEADER AGAINST THE FRONTIER IT LEADS — the same shape as the two pairs above, and the same reason for
     stating it here as well as in C: the engine's DCHECK is compiled out of a release build and this reader
     still runs on that build's bytes. The extrema come from the census walk and the leader's own rows come from
     flow_best's return, which are two traversals of what must be ONE population; a leader outside its own
     frontier's range is the walk and the pick disagreeing about who is standing, and every attribution made
     below from `topSvc` would then be a statement about a member that is not in the picture. */
  if (w.topSvc < w.svcMin || w.topSvc > w.svcMax || w.topSvcFam < w.svcFamMin || w.topSvcFam > w.svcFamMax)
    throw new Error(`[build] the @WFQ census reports the front of the order at own silence ${w.topSvc} ` +
                    `(frontier ${w.svcMin}..${w.svcMax}) and family silence ${w.topSvcFam} (frontier ` +
                    `${w.svcFamMin}..${w.svcFamMax}) — the leader is one of the members the same walk ` +
                    `enumerated, so a reading outside those extrema is the census and flow_best no longer ` +
                    `walking one frontier.`);
  /* AND THE BOUND ITSELF, WHICH IS THE ONE ROW THIS READER JUDGES BY AND THEREFORE THE ONE IT MAY NOT ACCEPT
     UNREAD. It is a property of flow_weight's formula and not of the run, so it is the same number on every
     census of every build; a zero or a negative would mean the optimism ceiling and the fitness ladder had both
     gone, which is not a frontier state but an edit — and a reader that divided by it or compared against it
     regardless would report every member as behind by AGING, which is the verdict that says stop looking. */
  if (!(w.nonrewardMax > 0) || !Number.isFinite(w.nonrewardMax))
    throw new Error(`[build] the @WFQ census reports nonrewardMax ${w.nonrewardMax} — it is flow.c's ` +
                    `FLOW_NONREWARD_MAX, the bound flow_nonreward asserts on every weight, and it is a fact ` +
                    `about the formula rather than about this run. A non-positive or non-finite value is an ` +
                    `edit that removed the optimism ceiling and the fitness ladder, not a state of the ` +
                    `frontier, and this reader will not judge a gap against it.`);
  if (w.visZero < 0 || w.visZero > w.members || (w.visZero === w.members) !== (w.visMax === 0))
    throw new Error(`[build] the @WFQ census reports visZero ${w.visZero} of ${w.members} members with ` +
                    `visMax ${w.visMax} — those are one walk's two answers about the same visit counts, so ` +
                    `"every member has completed no unit of work" and "the largest visit count is zero" are ` +
                    `the same statement and disagree only if the composer lost a member.`);
  /* EACH TERM OF flow_weight AGAINST THE SPREAD IT COULD ORDER — the reading that says which term is deciding
     this run, rather than which one is largest. A term's magnitude and a term's RANGE take opposite actions:
     an aging term of 856 points whose two ends are identical orders nothing at all and is a common offset.
     WHY THAT SENTENCE USED TO END `and that is what a single-family frontier is (flow.h's svc_fam_min)`, AND
     WHY IT NO LONGER DOES. It named a cause from a reading that cannot carry one: `svcFamMax === svcFamMin` is
     produced BOTH by a one-family frontier, where the equality is an identity of the structure and the family
     half can never order anything, AND by a several-family frontier whose services happen to agree at this
     instant, where the term IS ordering and is momentarily level. Those take opposite actions and the first is
     the one that says stop looking, so inferring it was the stale-verdict shape this function's own history
     already names one paragraph up. `families` is the row that answers it and it is read now, so the reading
     below STATES which of the two this frontier is instead of assuming.
     THE PRICE OF A NOTCH IS READ FROM THE SOURCE THAT DEFINES IT, NEVER RESTATED HERE. flow.c's
     FLOW_AGE_QUANTUM is `ENGINE_QUANTUM_MS * 1000 / FLOW_SILENCE_US`, and a reader that hardcodes today's
     0.012 keeps reporting 0.012 after the engine is retuned — a number that is true when written and wrong
     soon after, which is the one failure mode a build-time reader has no excuse for when both constants are
     one grep away. An absent define THROWS rather than defaulting: a consumer never fills a producer's hole
     with a plausible value (§Architecture). */
  const AGE_QUANTUM = ageQuantum();
  const rangeVal  = w.valMax - w.valMin;
  const rangeUcb  = 1 / (1 + w.visMin) - 1 / (1 + w.visMax);
  const rangeOwn  = (w.svcMax - w.svcMin) * AGE_QUANTUM;
  const rangeFam  = (w.svcFamMax - w.svcFamMin) * AGE_QUANTUM;
  const agingPts  = (w.svcMax + w.svcFamMax) * AGE_QUANTUM;
  const spread    = w.wTop - w.wMin;
  /* WHICH OF THE TWO STATES THE FAMILY HALF IS IN, STATED FROM `families` RATHER THAN INFERRED FROM ITS
     RANGE. One family is the structural case: every arm reads one node's service through one pointer, so the
     term is a common offset that can never order that document however deep it is. More than one with a zero
     range is the same arithmetic and the opposite finding — a term that orders, level at this instant. */
  const fam = w.families === 1
    ? "one fork family, so the family half is structurally a common offset and can never order this frontier"
    : rangeFam > 0
      ? `${w.families} fork families, spread ${rangeFam.toFixed(3)} — the family half is ordering`
      : `${w.families} fork families whose services are level at this instant — the family half orders but is ` +
        `not ordering here, which is not the same as the one-family case above`;
  /* WHICH OF THE THREE STATES THE @S SEARCH IS IN — solver/flow.h says the candidate rows exist to separate
     them and that they take different work, and this reader could name only two. `candSvcMax` is the row that
     splits the pair `candUnrun`/`candDecMax` cannot: a candidate that HAS been charged for the thread and
     still stands on no gates is being restarted from the baseline rather than resumed, which more scheduling
     does not fix, while one that has never been charged is starving in the order and more scheduling is
     exactly the fix. Zero candidates is a fourth statement and it is about the RUN's @S population rather
     than about the search — flow.h: `cands: 0` puts `distMax` at 0 by construction, so the fitness term is
     ordering nothing and that is not a finding about the term. */
  const cand = w.cands === 0
    ? "no @S candidate is live, so the fitness term is ordering nothing — a fact about this run's @S " +
      "population and not about the term"
    : w.candUnrun === w.cands
      ? `every one of the ${w.cands} candidates is STARVED — not one has ever been charged for the thread, ` +
        `so the ordering is what is costing this search`
      : `candidates have been served (${w.candSvcMax} notches at the most); how far along its runway any of ` +
        `them got is NOT REPORTED — see below, and do not read \`candDecMax\` (${w.candDecMax}) as an answer`;
  /* THIS READER USED TO STATE A CAUSE HERE FROM `candDecMax` AND THE NUMBER CANNOT CARRY ONE. It read
     "the deepest stands on N gates, so what limits this search is DISTANCE rather than turns", and one arm up
     it read a zero as "they are being RESTARTED from the baseline rather than resumed". Both are the
     instrument backwards. solver/decide.c's `decide_blob_stats` sets `*entries = b->seg->below + b->seg->n` —
     the LENGTH OF THE CHAIN — and never reads `b->c`, which is the cursor saying how much of it has been
     replayed. An @S candidate is seeded with the DETECTING flow's entire chain under a cursor of 0, so this
     row reports the full path the candidate was HANDED, from the instant of seeding, constant across the
     replay's whole life. It is the denominator, printed where a reader wants the numerator, and the two are
     furthest apart exactly where the question is about progress.
     SO THE ARM IS DELETED RATHER THAN REWORDED, and the sentence says the measurement is ABSENT. A diagnosis
     nobody can act on is better than a confident wrong one: §A-FIELD-A-CONSUMER-DEFAULTS is about a hole
     rendered as a plausible datum, and "what limits this search is DISTANCE" is that defect performed in
     prose — it names a cause, in the popup, out of a number that says only how long the recorded path was.
     WHAT WOULD SUPPLY IT, so this is a named absence and not a shrug: `decide.c` exposing the cursor
     (`b->c`) as its own accessor, and solver/flow.c feeding `cand_dec_max` from THAT while `dec_max` keeps
     the chain length. Then a candidate stuck at rung 0 with a GROWING cursor is one whose replay is diverging
     before its own source read, and one with a flat cursor is one nobody is serving — which is the
     three-state separation solver/flow.h already claims this row makes and which it cannot make today.
     ITS ABSENCE SHOWS as this line: the day the row means what flow.h says, this paragraph and the sentence
     above it are what must be rewritten, and the arm that states a cause can come back. */
  /* WHAT THE ORDER IS COSTING THE JOB QUEUE, WHICH IS A QUESTION ABOUT ONE THIRD OF IT. Two of the three
     classes are not the WFQ's to move — an OWED job waits on a reply the host has not sent, and a FRAMED one
     is forbidden to run by HTML §8.1.4.4 "Calling scripts"' clean up after running script step 3 while the
     JavaScript execution context stack is non-empty, which is a spec precondition and not a backlog. Reporting
     a total is what made those indistinguishable from the one class an ordering change can reach.
     AND THE READY CLASS HAS TWO OPPOSITE READINGS THAT THE COUNT ALONE CANNOT SEPARATE, which is why the gap
     is read beside it and never after it: a large ready count at the FRONT of the order is a queue being
     served and no finding at all, and the same count buried behind the reward spread is the order costing the
     run its job backlog. THE YARDSTICK IS THE CENSUS'S OWN AND NOT A CONSTANT — solver/result.c states it
     ("a gap on the scale of `valMax - valMin` is the reward spread burying the backlog"), so the comparison is
     against this frontier's reward spread and against the aging term's own notch, both of which this reader
     already prices. A fixed threshold here would be a number true of one fixture. */
  const jobsTotal = w.jobsReady + w.jobsFramed + w.jobsOwed;
  const jobNotches = AGE_QUANTUM > 0 ? Math.ceil(w.jobWGap / AGE_QUANTUM) : null;
  const jobs = jobsTotal === 0
    ? `no job is queued at all, so this order is costing the job queue nothing`
    : w.jobsReady === 0
      ? `${jobsTotal} queued job(s) and NOT ONE waits on rank — ${w.jobsFramed} on its member finishing its ` +
        `own program (HTML §8.1.4.4 "Calling scripts", clean up after running script step 3: a spec ` +
        `precondition, not an ordering problem) and ${w.jobsOwed} on the host. None of it is the WFQ's to move`
      : `${w.jobsReady} of ${jobsTotal} queued job(s) wait on RANK ALONE (${w.jobsFramed} framed, ` +
        `${w.jobsOwed} owed by the host)` +
        (w.jobWGap === 0
          ? `, and the front of the order is holding one — the backlog is NOT outranked, so nothing the ` +
            `ordering can do would run these sooner`
          : rangeVal > 0 && w.jobWGap >= rangeVal
            ? `, standing ${w.jobWGap.toFixed(3)} points behind the front — at or beyond this frontier's ` +
              `whole reward spread (${rangeVal.toFixed(3)}), so the backlog IS outranked and it is the ` +
              `REWARD spread burying it` +
              (jobNotches === null ? `` : `; the aging term would need ${jobNotches} quanta of silence to ` +
                                          `close that, which is what "inside a session" has to mean`)
            : `, standing ${w.jobWGap.toFixed(3)} points behind the front — inside this frontier's reward ` +
              `spread (${rangeVal.toFixed(3)}), so the backlog is behind but not buried` +
              (jobNotches === null ? `` : ` (${jobNotches} quanta of silence for the aging term to close)`));
  /* AND WHAT THE ORDER IS COSTING THE REPLY BACKLOG, WHICH IS THE SAME QUESTION OVER THE LARGER DEBT AND WHICH
     THIS READER COULD NOT ASK AT ALL. §Learning-from-replies calls learning from replies THE POINT, and the
     @COLD line can say a document was paid in full and delivered nothing (`replyAnswered == replyAsked` with
     the `deliver-one-reply` arm at 0) without being able to say WHY — three states behind one zero: the
     holders are FRAMED (a spec precondition, HTML §8.1.4.4 "Calling scripts"' clean up after running script
     step 3, and no ordering change reaches them), the holders are HOST-OWED (flow_pick refuses them, so no
     ranking can move them either), or they wait on RANK ALONE, which is the only one the WFQ owns. Exactly one
     of the three is a finding about the order and the other two are findings about other subsystems.
     THE GAP IS READ BESIDE THE COUNT AND NEVER AFTER IT, for the job backlog's reason exactly: a large ready
     count AT THE FRONT of the order is a backlog being served and no finding at all, and the same count buried
     behind the reward spread is the order costing this run the replies it was paid. The yardsticks are this
     frontier's own — its reward spread and the aging term's notch — never a constant, which would be a number
     true of one fixture. */
  const delivTotal = w.delivReady + w.delivFramed + w.delivOwed;
  const delivNotches = AGE_QUANTUM > 0 ? Math.ceil(w.delivWGap / AGE_QUANTUM) : null;
  const deliv = delivTotal === 0
    ? `no member holds an undelivered reply, so this order is costing the reply backlog nothing`
    : w.delivReady === 0
      ? `${delivTotal} member(s) hold an undelivered reply and NOT ONE waits on rank — ${w.delivFramed} are ` +
        `inside a program (HTML §8.1.4.4 "Calling scripts", clean up after running script step 3: a spec ` +
        `precondition, not an ordering problem) and ${w.delivOwed} carry the host-owed mark, which flow_pick ` +
        `refuses. None of it is the WFQ's to move, and re-pricing a weight term would reach none of it`
      : `${w.delivReady} of ${delivTotal} member(s) holding an undelivered reply wait on RANK ALONE ` +
        `(${w.delivFramed} framed, ${w.delivOwed} owed by the host)` +
        (w.delivWGap === 0
          ? `, and the front of the order is itself one of them — the backlog is NOT outranked, so nothing ` +
            `the ordering can do would deliver these sooner and the finding is upstream of the WFQ`
          : rangeVal > 0 && w.delivWGap >= rangeVal
            ? `, standing ${w.delivWGap.toFixed(3)} points behind the front — at or beyond this frontier's ` +
              `whole reward spread (${rangeVal.toFixed(3)}), so the backlog IS outranked and it is the ` +
              `REWARD spread burying it` +
              (delivNotches === null ? `` : `; the aging term would need ${delivNotches} quanta of silence ` +
                                            `to close that`)
            : `, standing ${w.delivWGap.toFixed(3)} points behind the front — inside this frontier's reward ` +
              `spread (${rangeVal.toFixed(3)}), so the backlog is behind but not buried` +
              (delivNotches === null ? `` : ` (${delivNotches} quanta of silence for the aging term to ` +
                                            `close)`));
  /* AND WHAT ASKING THIS ORDER COST, WHICH IS THE ONE QUESTION EVERY ROW ABOVE IS STRUCTURALLY SILENT ABOUT —
     they are all about what the order DECIDED. The census can already say the tail is not being reached and
     name that a THROUGHPUT statement rather than an ordering one; it could not say where the throughput went,
     and the two causes take opposite work: too few steps for the members standing, or steps that are expensive
     because each one asks a question whose cost is the frontier's SIZE.
     EVERY READING HERE IS INSIDE ONE CENSUS, deliberately. `steps` and `forks` are @COLD rows and this is the
     @WFQ reader, so quoting them would be comparing two lines taken at two instants — and it is not needed:
     the DISPATCH entry is asked once per iteration of the loop that steps a flow, and it is the only caller
     routed to it (the host's Level-1 read asks the same function and is routed to `other`, solver/flow.h), so
     `scanNextRuns` is this instance's step count up to the iterations that ended a slice without stepping.
     Every quotient below is two rows of this same census.
     THE ONE CHECK IS THE ONLY ONE THAT IS THE PRODUCER'S CLAIM, and the check that is NOT here is worth stating
     because it was written first and would have fired on a healthy run. "A scan of a non-empty frontier prices
     at least one member" is FALSE: the runnable-only scans skip a host-owed member before pricing it, so a
     frontier every member of which is waiting on the host evaluates ZERO weights — which is the STALL
     solver/engine.c names at its own `if (!best) break`, a real state and not a broken counter. That is the
     same shape as the `jobsReady`/`jobWGap` biconditional this file already had to correct: a guard that fires
     on the healthiest reading its rows can produce. What is left is the one claim with no state behind it — a
     lifetime count that only climbs cannot be negative. */
  /* THE SCAN NAMES, TAKEN FROM THE CENSUS AND NOT SPELLED AGAIN. They are the guard's subject and the series
     filter's, and this was the second hand-kept spelling of them in one function — one that agrees with the
     first only until a row is renamed on one side, which is exactly the contract `wfqFields()` was derived to
     stop being. It was ALSO already wrong: solver/flow.h's FLOW_SCANS gained a CENSUS entry, so
     `scanCensusRuns`/`scanCensusWeights` were emitted on every sample, excluded from the only guard that
     checks a scan row is a lifetime count, and excluded from the filter that decides which censuses carry an
     interval — a row this reader was blind to in TWO places at once.
     DERIVED BY THE COMPOSER'S OWN NAMING AND NOT BY A CONVENTION THIS FILE INVENTS: an entry of that enum is
     published as `scan<Entry>Runs` and `scan<Entry>Weights` in result_wfq_json's format string, so the set is
     whatever `wfqFields()` returns matching that shape, and a new entry joins both the guard and the filter
     without anybody remembering. `rankChanges` is named beside them because it is the denominator those
     readings are taken against and is a lifetime count under the same argument — it is not a scan row and is
     therefore not derivable from their shape.
     IT REFUSES AN EMPTY MATCH, for `censusComposerFields`' reason: a guard over no rows passes every census
     ever printed, and a naming change over there would turn this check off rather than break it. */
  const scanRows = wfqFields().filter((k) => /^scan[A-Z][A-Za-z0-9]*(?:Runs|Weights)$/.test(k));
  if (!scanRows.length)
    throw new Error(`[build] result_wfq_json publishes no \`scan<Entry>Runs\`/\`scan<Entry>Weights\` row — ` +
                    `solver/flow.h's FLOW_SCANS is what this reader prices the order's own cost from, and a ` +
                    `guard with no rows under it passes every census ever printed rather than reporting that ` +
                    `it has nothing to check.`);
  scanRows.push("rankChanges");
  for (const k of scanRows)
    if (w[k] < 0)
      throw new Error(`[build] the @WFQ census reports ${k} ${w[k]} — the order-scan counters are lifetime ` +
                      `counts that only climb (solver/flow.c increments them at the scan and nothing resets ` +
                      `them), so a negative is a counter that wrapped or a row that was never written, and ` +
                      `the cost reading below would be about no run at all.`);
  /* AND WHO CARRIES THE OPTIMISM TERM AT ITS MAXIMUM. `visMin`/`visMax` give the term's RANGE and cannot say
     how much of the frontier sits at the top of it; `visZero` is that population, and where it is the WHOLE
     frontier the bonus is one flat maximum and orders nothing — which is the same arithmetic as `rangeUcb: 0`
     and the reading a range alone leaves the reader to guess at. It is not `unrun`: flow.h says that row is
     ZERO OWN SILENCE and an emission by any arm of a member's fork family RESETS it for the whole family, so a
     member that has run and whose account emitted is counted there with
     visits to its name, and taking the two for one population reads a busy frontier as an idle one. */
  const ucb = w.visZero === w.members
    ? `every member has completed no unit of work, so all carry the same undecayed maximum bonus and the ` +
      `optimism term orders nothing here`
    : `${w.visZero} of ${w.members} members have completed no unit of work and carry the bonus undecayed`;
  /* AND WHETHER ANY MEMBER IS BEING STARVED — §scheduler's razor names five ways a schedule may be a cap and
     this reader could speak four of them. The distinction the pair makes is the one that decides what to fix:
     OUTRANKED is the ordering working (the aging term is what reaches those members, and the gap says how much
     silence it has to buy), while AT THE FRONT AND STILL NOT CHOSEN is the dispatch failing, which no amount
     of re-pricing a weight term will touch. The gap is quoted against the optimism term's whole range, because
     1.0 is the most any single emission can be worth and therefore the natural unit for "can this be closed by
     something happening" — a starved member within one bonus of the front is one turn away, and one many
     bonuses behind is genuinely last in a queue that is ordering it. */
  /* THE BOUND IS DERIVED FROM THE ENGINE'S OWN AND IS NOT A `1` SOMEBODY REMEMBERED. This read `<= 1` beside
     the reasoning that "1.0 is the most any single emission can be worth and therefore the natural unit for
     'can this be closed by something happening'". The unit is right and the number was a restatement, which is
     the second copy §Architecture's auditor sentence forbids: what a gap has to be measured against is not one
     emission, it is the most EVERY non-reward term TOGETHER can lift one member, and that is the quantity
     `flow_nonreward` already asserts as FLOW_NONREWARD_MAX and the census now carries out. The two differ by
     the whole fitness ladder — a candidate at the top rung carries a full extra point that no reader here can
     see — so the remembered 1 called a candidate one bonus behind the front "genuinely outranked" when a single
     rung would have put it at the head, and the day a rung is added beneath the ladder the restatement would
     have gone on reporting the old range with nothing to say so.
     AND THE REWARD SPREAD IS THE OTHER HALF OF IT, because a weight is an ACCOUNT's reward plus that sum: two
     members can differ by their accounts as well as by their terms, and `valTop - valMin` bounds that over the
     members this walk saw. It is identically zero on a one-family frontier, where every member reads one
     account, so this degenerates to the engine's bound exactly where a real page's frontier lives. */
  const liftBound = (w.valTop - w.valMin) + w.nonrewardMax;
  /* THE SERIES THIS FUNCTION HAS BEEN HOLDING AND READING ONE ROW OF. `s` is every census the run printed and
     `w` is its last; two sentences below used to end by telling their reader to go and read the stream — the
     `topSvc` discriminator on the leader arm, and the tail-not-reached statement solver/result.c names as "the
     reading that needs neither guessed". An instruction to consult a series, issued by the one reader that had
     already parsed it, is CLAUDE.md's assert-that-names-a-remedy-but-not-a-site performed in a report: a
     correct instruction with no object, which is why nobody carried it out. Both are answered here instead,
     from rows already in hand.
     ONLY CENSUSES THAT STATE AN ORDER ARE IN IT. solver/result.c emits `{"members":0}` with NO term rows when
     the walk finds nothing standing, and that omission is a POSITIVE statement — there was no order — never a
     hole for a `|| 0` to fill. Filtering on it is what stops a run that finished between two censuses from
     reading as a frontier that collapsed; a row that claims members and then omits a term throws, exactly as
     the last-census field loop above does, because that is the composer having changed.
     IT DECIDES NOTHING AND GATES NOTHING. No arm below turns on these numbers and none of them throws on a
     value: a reader that refused a run for not draining its tail would be the no-progress count §NO BOUNDS
     forbids, wearing a diagnostic's clothes. It is a reading, printed beside the instant. */
  const ordSeries = s.filter((r) => typeof r.members === "number" && r.members > 0);
  for (const r of ordSeries)
    if (typeof r.neverPicked !== "number" || typeof r.topSvc !== "number")
      throw new Error(`[build] a @WFQ census states ${r.members} members and omits neverPicked or topSvc — a ` +
                      `census reporting an order states every term row (solver/result.c's result_wfq_json), so ` +
                      `this is the composer having changed and not the empty-frontier shape.`);
  /* WHAT ASKING THE ORDER COST — AND IT IS READ OFF THE SERIES RATHER THAN OFF THE LAST CENSUS, which is a
     correction to this reader's first version and not a refinement of it.
     THE DEFECT IT HAD: the scan counters are LIFETIME and `members` is an INSTANT, so dividing one by the other
     compares a total accumulated over a frontier that GREW against the size that frontier finished at. Every
     early scan walked a smaller frontier, so the lifetime average is dragged below the final member count even
     where every scan was fully linear — the verdict "the filters are keeping the pick below the frontier's
     size" would then be printed for an engine whose pick is linear in it, which is the opposite reading. Two
     quantities over two populations, exactly the collapse the rows on this line exist to stop.
     WHAT REPLACES IT IS A WITHIN-RUN DELTA, and that choice is what makes it survive a measured fact about this
     harness: TWO passes of ONE revision, same frozen artifact, minutes apart, reported `scanNextRuns` 808 and
     362 — a 2x spread with no code between them, on a wall-denominated quantum. So no COUNT here may be quoted
     against another run, and this reader must not compose a verdict out of one. A DELTA between two adjacent
     censuses of ONE run is a trajectory rather than a magnitude, and a RATIO of two counters that move together
     divides the spread out of itself — which is why the verdicts below are ratios and the counts beside them are
     labelled as within-run only. That is CLAUDE.md's caveat rule built into the instrument instead of left to
     whoever reads its output. */
  const scanSeries = ordSeries.filter((r) => scanRows.every((k) => typeof r[k] === "number"));
  /* THE LAST INTERVAL, AND THE MEMBER COUNT THAT BELONGS TO IT. The delta is the work done BETWEEN two
     censuses; the frontier it was done against is the one standing at the later of them, which is the closest
     contemporaneous denominator this line has. It is still an approximation over an interval in which the
     frontier moved — stated, not hidden — and it is the only one available without a per-scan row. */
  const ivl = scanSeries.length >= 2
    ? (() => {
        const a = scanSeries[scanSeries.length - 2], b = scanSeries[scanSeries.length - 1];
        const dRuns = b.scanNextRuns - a.scanNextRuns;
        const dGen = b.rankChanges - a.rankChanges;
        return dRuns > 0
          ? { dRuns, walked: (b.scanNextWeights - a.scanNextWeights) / dRuns,
              rival: (b.scanRivalRuns - a.scanRivalRuns) / dRuns, members: b.members,
              /* THE CACHE'S OWN QUESTION, WHICH IS NOT THE COST QUESTION BESIDE IT. `rival` is scan
                 work per STEP; this is rescans per RANK CHANGE, and the hook rescans on a rank change
                 OR an incumbent switch, so above 1 is switching and at or below 1 is the cache doing
                 its job. Null when the order did not change across the interval — there is nothing to
                 be a rate of, and a rate over zero events is the empty denominator §Testing names. */
              gen: dGen, perGen: dGen > 0 ? (b.scanRivalRuns - a.scanRivalRuns) / dGen : null }
          : null;
      })()
    : null;
  /* THE ONE VERDICT THAT SURVIVES THE SPREAD, and it is the falsifier stated when these rows were built: the
     dispatch loop asks once per step, the preempt hook asks once per frontier GENERATION, and solver/flow.c's
     frontier_rank_changed raises that on every fork, arrival, departure and emission. At or below one rescan
     per dispatch scan the hook is not a multiplier and the linear-scan hypothesis is refuted; well above one,
     a forking page is paying for the ORDER once per branch, which is a rate nothing about the dispatch loop
     predicts and which is fixed at the hook rather than at the pick. Both terms move together with the run's
     length, so their quotient is what a 2x spread cannot reach. */
  const cost = !w.scanNextRuns
    ? `the order was never asked by the dispatch loop in this run, so there is no cost to read — a census ` +
      `taken before the scheduler's first pick`
    : `asking the order: ` +
      (ivl === null
        ? `only one census carries the scan rows, so there is no interval to read and the lifetime totals ` +
          `below are an instant's accumulation — no verdict is available from one sample`
        : `over the last interval the dispatch scan walked ${ivl.walked.toFixed(1)} member(s) of the ` +
          `${ivl.members} standing (${(100 * ivl.walked / ivl.members).toFixed(0)}%)` +
          (ivl.walked >= ivl.members * 0.5
            ? `, so the pick is LINEAR in the frontier and a step's cost grows with it — the run's total scan ` +
              `work is then quadratic in its own step count, which is a throughput finding and not an ` +
              `ordering one`
            : `, so the filters keep the pick well below the frontier and its cost is not tracking it`) +
          `; the preempt hook rescanned ${ivl.rival.toFixed(2)} time(s) per dispatch scan over ` +
          `${ivl.gen} rank change(s)` +
          /* THE COST AND THE CADENCE ARE TWO QUESTIONS AND THE SAME COUNT ANSWERS BOTH ONLY WITH TWO
             DENOMINATORS. Read per STEP it says what a step pays; read per RANK CHANGE it says whether
             the cache is working. A run whose forking has stopped shows a low per-step rate with the
             mechanism entirely intact, which is exactly how one run's lifetime ratio (3.22) and its last
             interval (0.86) came to say opposite things about one engine. Both are printed; neither is
             called the verdict on its own. */
          (ivl.gen === 0
            ? ` — the order did not change across this interval, so the hook had nothing to rescan for ` +
              `and this interval says nothing about its cadence`
            : ` (${ivl.perGen.toFixed(2)} per rank change` +
              (ivl.perGen > 1.2
                ? `, ABOVE one, so the rescans are being driven by incumbent switches as well as by rank ` +
                  `changes — the cache is being invalidated by the scheduler switching, not only by the ` +
                  `page branching`
                : `, at or below one, so the cache is absorbing what it can and the rescan count is the ` +
                  `rank-change count`) + `)`) +
          (ivl.rival > 1.5
            ? ` — MORE than one rescan per step: this page is paying for the ORDER per branch, and with a ` +
              `linear pick that is the frontier's size again on every one of them`
            : ` — at or below one per step, which REFUTES the per-branch multiplier for this interval ` +
              `and does NOT refute it for the run: read it beside the rank-change count, because an ` +
              `interval in which nothing forked cannot show a per-fork cost`)) +
      /* THE LIFETIME TOTALS ARE PRINTED AND LABELLED, never used to decide. They are what a reader needs to
         see the interval in proportion, and they are exactly what the 2x spread makes unquotable between two
         runs — so the sentence that carries them says so, rather than leaving a bare count for somebody to
         compare against another revision's. */
      ` (within this run only, not comparable across runs: ${w.scanNextWeights} weight(s) over ` +
      `${w.scanNextRuns} dispatch scan(s), ${w.scanRivalWeights} over ${w.scanRivalRuns} hook rescan(s), ` +
      `${w.scanOtherWeights} over ${w.scanOtherRuns} host/pager scan(s))`;

  const signedDelta = (n) => (n >= 0 ? `+${n}` : `${n}`);
  const svcUp = ordSeries.filter((r, i) => i > 0 && r.topSvc > ordSeries[i - 1].topSvc).length;
  const svcDown = ordSeries.filter((r, i) => i > 0 && r.topSvc < ordSeries[i - 1].topSvc).length;
  const series = ordSeries.length < 2
    ? `one census with an order in it, so every reading here is an instant and none of them is a run`
    : (() => {
        const a = ordSeries[0], b = ordSeries[ordSeries.length - 1];
        return `across the ${ordSeries.length} censuses with an order in them the frontier moved ` +
          `${signedDelta(b.members - a.members)} members while the never-dispatched population moved ` +
          `${signedDelta(b.neverPicked - a.neverPicked)}` +
          (b.neverPicked > a.neverPicked
            ? `, so the tail is NOT being reached — a THROUGHPUT statement and not an ordering one, which is ` +
              `the only reading of this pair that does not need an instant guessed`
            : `, so the sweep reached at least as much of the tail as forking added to it`) +
          `; the front's OWN silence rose at ${svcUp} of those ${ordSeries.length - 1} steps and fell at ` +
          `${svcDown}, so ` +
          (svcUp > svcDown
            ? `the leader is holding the thread and being charged for it, and a gap behind it is closing`
            : `the front's own charge is not accumulating: the flow STANDING at the front is not the flow ` +
              `being charged. Two things read that way and \`topSvcFam\` beside it separates them — a front ` +
              `REFILLED by freshly-minted arms, or a leader whose silence an emission keeps forgiving — and ` +
              `in neither does waiting close a gap behind it`);
      })();
  /* WHAT THE FRONT ITSELF IS DOING, which is the half `neverPickedGap` cannot supply and without which a
     standing gap has two causes and one reading. See solver/flow.h: `topSvcFam` is the leading ACCOUNT's
     silence and is what an emission resets, so it says whether that account's aging is being forgiven; `topSvc`
     is the leader's OWN, and on a one-family frontier it is the only half that can move a gap at all. */
  const leadAging = ((w.topSvc + w.topSvcFam) * AGE_QUANTUM);
  const leader =
    `the front carries ${leadAging.toFixed(3)} points of aging (${w.topSvc} notches of its own silence, ` +
    `${w.topSvcFam} of its family's), so the leading account ` +
    (w.topSvcFam === 0
      ? `emitted within the last quantum and its aging is being forgiven as fast as it is charged`
      : `has been silent for ${w.topSvcFam} quanta`) +
    (w.families === 1
      ? `; this frontier is ONE family, so that family charge lands on every member in the same instant and ` +
        `cancels out of every gap above — only the ${(w.topSvc * AGE_QUANTUM).toFixed(3)} points of the ` +
        `leader's OWN silence can close one, and this digit is one instant of it: the series sentence beside ` +
        `this reads that half across the whole stream, which is the only place the discriminator lives`
      : `; across ${w.families} families both halves order, so either can move a gap between accounts`);
  /* FOUR ARMS AND NOT THREE, AND THE ONE ADDED IS THE ONLY ONE THAT FIRED ON A HEALTHY RUN. A gap of exactly
     0.000 fell into the first arm below and was reported, in words, as "this is the razor's STARVES" — the
     sentence solver/result.c retired with measurement (six runs, 212 censuses, SIXTY-THREE samples at exactly
     0.000, spread across every run including ones whose ladder drained to the orphan seed) and that
     solver/flow.h went on carrying. A verdict that fires on 30% of the samples of a frontier that is working is
     not a verdict, and this one was quoted out of the tree and acted on as a dispatch defect.
     ZERO IS A TIE AND A TIE IS NOT A DEFICIT. flow_pick seeds the incumbent and compares STRICTLY, so it
     returns ONE of N equal maxima and the other N-1 stand at that instant exactly at the front — the ordinary
     state of a one-family frontier, where every member reads one reward through one pointer and an emission
     zeroes that family's silence at every arm at once. Nothing is ranked ahead of those members, so the
     ORDERING is not what is keeping them out and no reading of the ordering can say anything about them. */
  const starved = w.neverPicked === 0
    ? `every member has been handed the thread at least once`
    : w.neverPickedGap === 0
    ? `${w.neverPicked} of ${w.members} members have NEVER been handed the thread and the best of them is not ` +
      `behind at all — it TIES the front. flow_pick seeds the incumbent and compares STRICTLY, so it returns ` +
      `one of N equal maxima and the rest stand exactly here; nothing is ranked ahead of them, so the ordering ` +
      `is not what is keeping them out and this instant is not evidence either way. The incumbent's hold ends ` +
      `at its next completed unit of work (flow_credit_visit drops the optimism term from 1/(1+v) to 1/(2+v)) ` +
      `or at its next notch of OWN silence (flow_age_running), whichever comes first — the series beside this ` +
      `is the reading, and it is a throughput one`
    : `${w.neverPicked} of ${w.members} members have NEVER been handed the thread, the best of them standing ` +
      `${w.neverPickedGap.toFixed(3)} points behind the front` +
      /* THREE ARMS AND NOT TWO, WHICH IS THE DISTINCTION THE BOUND'S TWO SUMMANDS ALREADY CARRY AND WHICH A
         SINGLE COMPARISON THREW AWAY. A weight is an ACCOUNT's reward plus a non-reward sum, and a member can
         be behind on either — but those are not the same finding and they do not take the same work. Within
         `nonrewardMax` the member is behind by LIFT: every term it is behind on is bounded and one of them
         reading differently puts it at the front, so the ordering is a hair from returning it. Between that
         and the reward spread it is behind by its ACCOUNT, which no term reads and no waiting moves — an
         account closes that gap by EMITTING, and a member that is never dispatched cannot emit, which is the
         loop worth naming rather than filing under the same verdict. Beyond the whole bound its own terms are
         already net negative and it is behind by AGING, which nothing bounded reaches at all. On a one-family
         frontier the middle arm is unreachable by construction (one account, so the spread is identically
         zero) and this is the two-way reading it was — which is why collapsing them looked right. */
      (w.neverPickedGap <= w.nonrewardMax
        ? ` — within the ${w.nonrewardMax.toFixed(3)} that every non-reward term of flow_weight together can ` +
          `lift one member, so the best of them is behind by LIFT alone: it IS strictly outranked, and one ` +
          `bounded term reading differently puts it at the front — the leader's own silence being the term ` +
          `that will. This is an ordering that has not reached them YET and not the razor's STARVES, which is ` +
          `a claim about a member the order never returns and is therefore a claim about the series`
        : w.neverPickedGap <= liftBound
        ? ` — beyond the ${w.nonrewardMax.toFixed(3)} every non-reward term can lift it but inside the ` +
          `${(w.valTop - w.valMin).toFixed(3)} of reward spread between the ${w.families} accounts standing, ` +
          `so the best of them is behind by its ACCOUNT and not by any term: no term reads that away and no ` +
          `silence moves it, an account closes it only by EMITTING, and a member that is never handed the ` +
          `thread cannot emit — which is the loop, and it is neither a lift problem nor an aging one`
        : ` — beyond the ${liftBound.toFixed(3)} that its account's reward and every non-reward term together ` +
          `could give it, so the best of them already carries a NET NEGATIVE non-reward sum and is behind by ` +
          `AGING, which no bounded term reaches. Nothing that member can do closes this; only the leader ` +
          `sinking does, which is what the front's own silence beside this says`);
  /* AND WHERE THE DISPATCHES THAT DID HAPPEN WENT, WHICH IS THE ROW ABOVE'S THREE STATES BEHIND ONE ANSWER.
     `starved` says the tail is not being reached; solver/flow.h names three frontiers that produce that same
     sentence and says two of them take DIFFERENT weight changes while the third takes none — a frontier simply
     outgrowing one thread, an order re-serving a reachable cohort ahead of members it has never served, and
     one member holding the thread outright. The rows that separate them have been emitted since they were
     added and read by NOTHING: `picksLive` is T restricted to the members still standing, `members -
     neverPicked` is P, and `picksMax` tells the second state from the third.
     NO THRESHOLD IS INVENTED HERE, and that is a decision rather than an omission. flow.h's discriminators are
     "T/P near 1", "T/P large" and "picksMax near T", and a number this file chose for `near` would be the
     remembered `1` §the-lift-bound already had to retire — a restatement with no artifact behind it, going
     stale the day the engine's shape changes and reading as authority in the meantime. What CAN be stated
     exactly is each range's ENDPOINT, because each is an equality over integers from one walk: `picksLive ===
     P` is T/P at exactly 1, and `picksMax === picksLive` is exactly one member holding every dispatch the
     standing members hold. Those two are named; between them the two ratios are printed and the reader has
     what flow.h asks for.
     THE KINDS DECIDE THE ARITHMETIC AND ARE WHY THIS IS ONE SAMPLE'S READING. `picksLive` and `picksMax` are
     GAUGES over the members standing NOW and can FALL between two censuses as members depart, so neither may
     be differenced; `picksLifetime` is the only counter of the three and is the only one that may. It is
     rendered as the DIFFERENCE it defines — what the departed members took with them — which is the one thing
     the gauges cannot say and is a statement about retirement rather than about order. */
  const P = w.members - w.neverPicked;
  const dispatch = (P === 0
    ? `no member standing has ever been handed the thread (picksLive 0 over ${w.members} members), so this ` +
      `census states no distribution at all — the scheduler has made ${w.picksLifetime} dispatch(es) in this ` +
      `instance's lifetime and every member that received one has since departed or none was ever made`
    : `${w.picksLive} dispatch(es) are held by the ${P} member(s) ever chosen` +
      (w.picksLive === P
         ? ` — EXACTLY one each, so the thread reached a FRESH member every time it was handed out and this ` +
           `frontier is outgrowing one thread rather than being mis-ordered: no term of the weight reaches ` +
           `that, and the answer is throughput`
         : `, ${(w.picksLive / P).toFixed(2)} apiece`) +
      (w.picksMax === w.picksLive && w.picksLive > 0
         ? `, and ONE member holds every one of them — a monopolizer the aging term is failing to sink, which ` +
           `is a different repair from an order re-serving a reachable cohort`
         /* THE MAXIMUM ALONE, WITH NO RATIO BESIDE IT. `picksLive / picksMax` has exactly ONE reading and
            solver/result.c states it — the mean sweep DEPTH between two emissions, and only under the tier
            hypothesis `topForgiven` is what tests. Printed here it would be a second, unlabelled copy of a
            number whose meaning is stated below, and the label this line first carried for it ("of those held
            by others") named a quantity that exists nowhere in the program: a sum over the other members
            divided by nothing. A row is labelled from its accessor or it is not labelled. */
         : `, the most any one holds being ${w.picksMax}`) +
      `; ${w.picksLifetime} made in this instance's lifetime, so departed members took ` +
      `${w.picksLifetime - w.picksLive} away`) +
      /* RENDERED ON BOTH ARMS ABOVE AND NOT ONLY ON THE ONE THAT HAS A DISTRIBUTION TO REPORT — which is the
         decision the `leader` row two paragraphs down is written for and the reason this clause is outside the
         conditional rather than inside its second half. A frontier before its first dispatch still HAS a front
         and a forgiveness count, and dropping the reading there would make it an observation computed on one
         arm and absent on the other: the pre-dispatch census is precisely where "the leading account has never
         emitted" is the whole of what there is to say.
         AND THE EVENT THE FRONT'S TWO SILENCE NOTCHES ARE A READING BETWEEN, which is what turns "the leader's
         aging is being reset" from an inference into a count. It is NOT a lifetime counter even though it only
         climbs for one account: the count belongs to whichever account is at the FRONT, and that changes — so
         a FALL between two samples is a change of leader, which `valTop` falling beside it confirms, and this
         row is read per sample and never differenced on its own. The engine asserts the equivalence this arm
         splits on (flow_credit_emit raises the ledger and bumps the generation in one statement), so a
         forgiveness count at zero IS a leading account that has never emitted, not a hole. */
      (w.topForgiven === 0
         ? `. The leading account has never been forgiven its silence window, which is the same fact as its ` +
           `ledger standing at ${w.valTop} — flow_credit_emit raises both in one statement — so its aging is ` +
           `accumulating and has never been reset`
         : `. The leading account's silence window has been forgiven ${w.topForgiven} time(s) for a ledger of ` +
           `${w.valTop}, ${(w.valTop / w.topForgiven).toFixed(2)} point(s) per finding; an emission zeroes ` +
           `both aging halves for EVERY arm of it at once, so read this against the ${w.picksMax} above — if ` +
           `the frontier is collapsing into tied visit tiers and flow_pick is sweeping one from its oldest ` +
           `member forward, that maximum tracks this count and ${w.picksMax > 0 ? (w.picksLive / w.picksMax).toFixed(2) : "the sweep depth"} ` +
           `is the mean depth between two emissions`);
  const terms = `terms over the frontier: reward ${rangeVal.toFixed(3)}, fitness ${w.distMax.toFixed(3)}, ` +
                `optimism ${rangeUcb.toFixed(3)}, aging ${(rangeOwn + rangeFam).toFixed(3)} ` +
                `(own ${rangeOwn.toFixed(3)}, family ${rangeFam.toFixed(3)}) — against a total order spread ` +
                `of ${spread.toFixed(3)} and an aging term ${agingPts.toFixed(1)} points deep; ${fam}; ` +
                /* THE FRONT IS RENDERED ON EVERY CENSUS AND NOT ONLY WHERE A GAP IS WIDE, which is a decision
                   about who reads it rather than about when it matters. Computed on one arm of a verdict and
                   dropped on the others it would be an observation with a writer and no reader on most runs —
                   the mirror of the defect this reader's own field list exists to stop — and it is the row a
                   reader needs BEFORE a gap opens, because the discriminator it carries is a shape across the
                   stream and a stream is only assembled from censuses that all state it. */
                `${ucb}; ${starved}; ${dispatch}; ${leader}; ${series}`;
  /* WHOSE REWARD THE ORDER IS, which is a different question from whether the reward is ordering it and is the
     one the verdict's own sentence makes a claim about. `selfEmit` counts members that have emitted something
     THEMSELVES, so the difference is how many stand on an account some other arm of their fork family filled.
     THE SENTENCE IT USED TO END ON IS RETIRED AND IS NOT A SMALLER VERSION OF ITSELF. It said the spread
     between the ends "is an ANCESTRY's", which was true while the reward was copied onto every arm at its fork
     instant — a per-CHAIN prefix, so two arms of one family stood at two rewards for something neither of them
     did. The reward is the fork FAMILY's now (solver/flow.h's flow_reward), read through the account the aging
     is charged to, so every arm of one family reads one number and a spread is a gap between ACCOUNTS. That
     makes this row and `fam` a PAIR and neither is worth rendering alone: a large `inherited` with ONE family
     is a producing account spread over many live arms, which is what a branching document looks like and is
     not a defect; the same figure with SEVERAL families and a spread is one account outranking another none of
     whose members can act, which is. Stated on every arm of this function for the reason `orderedBy` is: the
     caller renders it, so an arm without it renders a hole. */
  const inherited = w.members - w.selfEmit;
  const whose = `${inherited} of the ${w.members} members have emitted none of the reward their account holds ` +
                `(${w.selfEmit} have emitted something themselves), so any spread between the ends is between ` +
                `ACCOUNTS and is read against the family count beside it`;
  return {
    /* THE DISCRIMINATOR IS THE REWARD SPREAD AGAINST THE OPTIMISM TERM'S WHOLE RANGE, AND IT USED TO CARRY A
       SECOND CONJUNCT THAT THE ENGINE HAS SINCE MADE PERMANENTLY FALSE. It read `&& w.valZero > 0`, on the
       reasoning solver/flow.h stated beside the row: a from-baseline flow "enters at reward 0", so `valZero`
       was the population sitting under the spread and its being non-empty was what made the spread matter.
       flow.c's flow_arrive_at_virtual_time assigns the REWARD among the four tags a newcomer arrives at, so a
       from-baseline flow now enters at the incumbent's reward and NOTHING is at zero inside a busy period —
       `valZero` can be non-zero only before the first pick. The conjunct therefore stopped selecting a state
       and started selecting an impossibility, and the arm it gated is the one that names the reward term: a
       smoke run standing at reward 14..182 with `valZero: 0` took the `else` and had its verdict say "its ends
       are within one optimism bonus of each other" about a frontier whose ends are 168 points apart. A
       reader's discriminator outliving the population it names is the stale-DFAIL shape wearing a boolean —
       true when written, and afterwards a confident statement of the opposite of the measurement. What the
       conjunct was reaching for is reported instead of gating: `whose` above says how much of that spread any
       member actually earned, which is the fact that decides what to do about it. */
    ordered: w.valMax - w.valMin > 1,
    whose,
    /* WHICH TERM THE ORDER ACTUALLY IS, named from the ranges rather than inferred. A term that cannot move
       two members apart is not ordering them however large it is, so the biggest RANGE is the answer and a
       frontier whose ranges are all zero is one the pick cannot distinguish at all — which is a true and
       reportable state (one fork family, equal reward, no candidates) and not a defect to be inferred into. */
    orderedBy: (() => {
      const t = [["reward", rangeVal], ["fitness", w.distMax], ["optimism", rangeUcb],
                 ["aging", rangeOwn + rangeFam]].sort((a, b) => b[1] - a[1]);
      return t[0][1] <= 0 ? "nothing — every term reads the same at both ends of this frontier" : t[0][0];
    })(),
    text: `@WFQ: ${w.members} members, account reward ${w.valMin}..${w.valMax} (top ${w.valTop}), ` +
          `${w.valZero} on accounts at 0, ${w.selfEmit} emitted something themselves, ` +
          `${w.unrun} at zero own silence (never charged since their family last emitted); ` +
          `${w.cands} @S candidates of which ${w.candUnrun} never ran, deepest one ${w.candDecMax} of ` +
          `${w.decMax} gates in; weight ${w.wTop} at the front against ${w.candWMax} for the best candidate; ` +
          `${cand}; ${jobs}; ${deliv}; ${cost}; ` + terms,
  };
}

/* WHY THE MEMBERS ARE NOT FINISHING — the `stepUnits` histogram, which is the row `finished` and `live` could
   never carry. Those two say work is being ADMITTED and not RETIRED; this says what the members that are not
   retiring are DOING, and the arms take opposite work: `start-a-classic-program` is a frontier moving THROUGH
   its documents while `resume-program` beside it is one grinding inside programs it has already started (one
   row carried both until solver/step_unit.h split them, and a frontier that admits members and retires none is
   exactly the state in which that difference is the diagnosis), `queue-rendering-opportunity`/`fire-due-timer`
   is unbounded periodic work, the orphan arms are seeding drives, and
   `host-blocked`/`await-owed-reply`/`await-fetch-record`/`await-peer-operation` are four distinct kinds of
   waiting. One verdict covered all of them.

   AND `document-lifecycle-stage` IS NOT THE THIRD MEMBER OF THAT PERIODIC PAIR, WHICH IS WHAT THIS PARAGRAPH
   USED TO SAY. solver/step_unit.h's own sentence — the authority, since it is the one place an arm is named —
   groups exactly `queue-rendering-opportunity` OR `fire-due-timer` as "unbounded periodic work and is a
   fidelity gap or a regression". This reader added a third arm to that group, and the two are opposite in the
   property the group is ABOUT: core/dom/document.c's document_lifecycle_step is MONOTONE PER DOCUMENT, from
   readiness 0 to 1 to 2, and it says so in two DCHECKs — "a document's DOMContentLoaded stage ran and left its
   readiness where it was … which is a live-lock the scheduler cannot tell from progress", and the same again
   at `load`. An arm that crashes rather than repeat itself is the opposite of unbounded periodic work.
   SO THE MIS-GROUPING WAS NOT A WORDING SLIP — IT CHANGED THE DIFF A READER WOULD GO AND WRITE. A frontier
   sitting in this arm is a document held at HTML §13.2.7 "The end" step 8, whose whole text is "Spin the event
   loop until there is nothing that delays the load event in the Document" — so a member resting there is
   WAITING BY THE SPEC'S OWN INSTRUCTION, which is the precise opposite of a loop that should not be running.
   (§13.2.7 is 11 top-level steps with list depth tracked; 7 and 8 are both spins and only 8 is this one.) What
   this engine models as the delayer is a CHILD document that is not ready yet, which is why document.c's
   second pass walks innermost-first. The honest next question is which child and what it is owed — the
   delaying-the-load-event sources document.c names at that site as belonging to their own components — and
   NOT "find the fidelity gap making this loop". Reported under the periodic heading, a member parked on a
   child reads as an engine spinning, which is a diagnosis of the wrong file.

   IT READS NO LIST OF ITS OWN, and that is deliberate. solver/step_unit.h is the only place an arm is named —
   the enum, the diagnostic string and this row are three expansions of one macro — so a copy of the names here
   would be the second list that eventually disagrees. The rows are taken from the census as it emits them and
   the CONTRACT is checked instead of the spelling.

   THE CONTRACT IS THE PARTITION, AND IT IS WHAT MAKES AN ABSENT ROW DIFFERENT FROM A ZERO ONE. Every live
   member carries exactly one arm, so the values SUM to `live`. A missing row therefore cannot hide behind a
   plausible rendering: the sum falls short and this throws, naming the composer. A row that reads 0 is a
   MEASUREMENT — that frontier had nobody in that arm — and is reported as one. Neither is ever defaulted into
   the other, which is the defect this whole instrument exists to end. */
/* THE VALIDATION ALL THREE HISTOGRAMS NEED, WRITTEN ONCE — the same split `censusFields` makes one screen up,
   and for the same reason: the rows differ ONLY in the total they must sum to and in where their row SET comes
   from, and a second copy of the shape check is the fourth place a renamed field has to be renamed. `name`,
   `totalName` and `extent` are parameters because they are the whole of the difference AND because a reader
   who hits one of these throws has to know WHICH histogram broke and what its rows are derived FROM — a shared
   checker whose message names neither is a throw nobody can act on.
   `extent` IS NOT DECORATION: two of these histograms are expansions of solver/step_unit.h's list, so an
   absent or empty one can only be a composer that stopped emitting; the third's row set is the FRONTIER's own
   (solver/cold.h), so its extent legitimately changes census to census and the sentence a reader is handed on
   a break has to say which kind it was looking at. It is emptiness that both kinds agree on and that is not an
   accident either — solver/cold.c gives an empty frontier program 0 precisely so that no reader has to hold a
   second rule about a shape it cannot tell apart from the outside. */
function censusHistRows(b, name, totalName, extent) {
  const u = b[name];
  if (u === null || typeof u !== "object" || Array.isArray(u))
    throw new Error(`[build] the @COLD census carries no \`${name}\` object — solver/result.c composes it ` +
                    `from ${extent} on every census, so its absence is that composer having ` +
                    "changed rather than a frontier with nothing in any row. An absent histogram and an " +
                    "all-zero one are different facts and this reader will not average them.");
  const rows = Object.entries(u);
  if (!rows.length)
    throw new Error(`[build] the @COLD census's \`${name}\` is empty — the histogram is emitted with EVERY ` +
                    `row including the zeroes (its rows are ${extent}), so an empty object is a composer ` +
                    "that stopped listing them and never a population with nobody in it.");
  for (const [k, v] of rows)
    if (typeof v !== "number")
      throw new Error(`[build] the @COLD census's \`${name}.${k}\` is not a number — the histogram is a ` +
                      `count per row and a non-numeric row cannot be summed against \`${totalName}\`.`);
  if (typeof b[totalName] !== "number")
    throw new Error(`[build] the @COLD census carries no numeric \`${totalName}\` for \`${name}\` to be ` +
                    `checked against — the sum is the only thing that makes a MISSING arm different from an ` +
                    `arm that read 0, so without it this reader would be rendering a histogram it cannot ` +
                    `establish is whole.`);
  const total = rows.reduce((t, r) => t + r[1], 0);
  if (total !== b[totalName])
    throw new Error(`[build] the @COLD census's \`${name}\` sums to ${total} over ${rows.length} rows ` +
                    `against \`${totalName}\` ${b[totalName]} — the rows are a PARTITION of that total, so ` +
                    `the two sides disagree and no reading composed from this row is about the run that ` +
                    `happened. The engine asserts the same identity where both halves are in one hand ` +
                    `(solver/cold.c's walk for \`stepUnits\` and \`programCursors\`, solver/engine.c's ` +
                    `convergence point for \`stepUnitRuns\`, all three re-checked at solver/result.c's ` +
                    `composer); a difference visible HERE and not there is a row lost between the ` +
                    `census and this document.`);
  return rows;
}

const STEP_UNIT_EXTENT = "solver/step_unit.h's list";

function stepUnitReading(b) {
  const rows = censusHistRows(b, "stepUnits", "live", STEP_UNIT_EXTENT);
  const live = rows.filter((r) => r[1] > 0).sort((x, y) => y[1] - x[1]);
  const zero = rows.length - live.length;
  /* AN EMPTY FRONTIER IS A SENTENCE AND NOT AN EMPTY LIST, for the same reason wfqReading has an arm for
     `members: 0`: rendering nothing there reads as a histogram that failed rather than as a census taken with
     nobody standing, and the two take opposite work. */
  return live.length === 0
    ? `step units at the last census: no member was standing, so all ${rows.length} arms read 0`
    : `step units at the last census: ` + live.map((r) => `${r[1]} ${r[0]}`).join(", ") +
      ` (${zero} of the ${rows.length} arms read 0, which is a measurement and not an absence)`;
}

/* AND THE OTHER HALF OF THE PAIR — WHICH RUNGS THE LADDER HAS EVER RUN, over the whole life of the instance.
   THIS IS THE ONE READING THE CENSUS ABOVE STRUCTURALLY CANNOT GIVE, and it is why both are rendered. The row
   above is a GAUGE: it says where the members are standing at the instant the last census was taken, so an arm
   reading 0 there is one nobody is resting on — which is equally true of an arm the ladder NEVER REACHES and
   of an arm every step passes straight through. Those are opposite diagnoses. The first is a rung whose
   predecessors never run out, and the fix is upstream of it; the second is a rung that runs constantly and
   retires nothing, and the fix is at it. `stepUnitRuns` is a lifetime count, so a 0 in it is the first of
   those and a large number in it beside a 0 in the gauge is the second — and nothing in this stream could tell
   them apart before it existed.
   IT IS REPORTED AND NEVER USED TO DECIDE (§NO BOUNDS). No arm of the verdict below reads it, no discriminator
   branches on it, and it is not a no-progress detector: it is rendered beside the gauge because a reader
   needs both to name a rung, and that is the whole of its job here. */
function stepUnitRunReading(b) {
  const rows = censusHistRows(b, "stepUnitRuns", "steps", STEP_UNIT_EXTENT);
  const ran = rows.filter((r) => r[1] > 0).sort((x, y) => y[1] - x[1]);
  const never = rows.filter((r) => r[1] === 0).map((r) => r[0]);
  /* A RUN THAT TOOK NO STEP AT ALL IS A SENTENCE, for stepUnitReading's reason exactly: an all-zero histogram
     over zero steps is a census taken before the scheduler ran, and rendering it as a list of zeroes reads as
     a ladder that was climbed and reached nothing. */
  if (b.steps === 0)
    return `step units over the whole run: no step has been taken yet, so all ${rows.length} arms read 0`;
  return `step units over the whole run (${b.steps} steps): ` +
         ran.map((r) => `${r[1]} ${r[0]}`).join(", ") +
         (never.length
           ? ` — and ${never.length} of the ${rows.length} arms have NEVER run (${never.join(" ")}), which is ` +
             `a rung the ladder has not reached rather than a rung nobody is resting on. ONE OF THEM IS ` +
             `ALWAYS IN THAT LIST and is not a finding: the value flow_step resets to at its entry is in the ` +
             `same list as the arms, and the scheduler asserts a RECORDED step is never it ` +
             `(solver/step_unit.h), so its zero here is that assert restated`
           : ` — every arm has run at least once`);
}

/* WHAT A TURN OF THE DISPATCH LOOP COST — the row every reading above is structurally silent about, and the
   one the question "why did this run make so few choices" actually needs.
   THE TWO READINGS ABOVE ARE ABOUT WHERE THE STEPS WENT AND THIS IS ABOUT HOW MANY THERE COULD BE. A run's
   step count has two candidate explanations that take OPPOSITE work and that no arm histogram separates: a
   turn that consumes a whole cooperative slice makes about one choice per slice BY CONSTRUCTION — a
   granularity floor, and no finding about the order at all — while turns that are cheap mean the loop was
   given little thread time and the question moves off the scheduler entirely. The @WFQ census answers the
   neighbouring half (what ASKING the order costs, in members walked per scan), which is one TERM of a turn
   and not the turn.
   IT IS A RATIO AND NEVER A COUNT, which is the whole of why it can be printed at all. §Testing: two passes
   of one revision on one frozen artifact came back a 2x spread apart on this harness, so `stepUs` and `steps`
   are each unquotable against another run — while their QUOTIENT is two lifetime totals of ONE run over ONE
   population (solver/engine.h's `step_us`: one charge per loop iteration that stepped a flow) that move
   together, so the spread divides out of it. The INTERVAL form beside it is the same discipline the @WFQ
   reader's scan rows use: a delta between two adjacent censuses is a trajectory rather than a magnitude, and
   it is what says whether a turn has been getting more expensive as the frontier grew.
   THE YARDSTICK IS THE RUN'S OWN SLICE AND NOT THIS TREE'S HEADER. `sliceMs` comes off the `@QUANTUM` line
   the measured run printed, so the comparison is against the slice that run was actually scheduled on; reading
   ENGINE_QUANTUM_MS out of solver/engine.h here would be today's working tree answering for an artifact built
   from some other revision, which is §Testing's frozen-snapshot defect arriving through the reader.
   AND THE MEASURE CONSTRAINS THE CONCLUSION RATHER THAN DECORATING IT. Where the host has a thread CPU clock,
   a mean at the slice says the turn CONSUMED a slice. Where it does not (solver/quantum.h answers per host;
   emscripten's every WASI clock is wall), the same mean is equally consistent with a turn that was
   DESCHEDULED for a slice's worth of wall — and since the slice is armed on that same clock, the engine
   cannot tell those apart either. That does not make the number useless and it does eliminate one reading of
   it, so the sentence says which, rather than printing a caveat and then reasoning past it.
   IT DECIDES NOTHING (§NO BOUNDS). Nothing here throws on a value, no verdict branches on it, and no run is
   refused for a slow turn — a per-step cost is exactly the shape a watchdog would be built from, which is why
   that is said at the engine's counter, at the census composer and here. */
function stepCostReading(a, b, q) {
  /* THE DENOMINATOR IS ASKED FOR BEFORE IT IS USED, because a mean over zero turns is the empty denominator
     §Testing names and a `0` printed for one is a magnitude somebody will compare. A census taken before the
     scheduler's first pick is a real state and is reported as one. */
  if (b.steps === 0)
    return `dispatch turns cost: no step has been taken, so there is no turn to be a cost of — a census taken ` +
           `before the scheduler's first pick and not a loop that ran for free`;
  const per = b.stepUs / b.steps;
  const dSteps = b.steps - a.steps, dUs = b.stepUs - a.stepUs;
  const ivl = dSteps > 0
    ? `, and ${(dUs / dSteps).toFixed(0)} over the last window's ${dSteps} turn(s)`
    : `, and the window between the two censuses this reading spans contains NO turn, so it has no interval ` +
      `rate — a trajectory needs two points and this is one`;
  /* THE SLICE IS THE RUN'S, SO ITS ABSENCE IS AN ABSENCE AND NEVER A DEFAULT. A stage that opened no engine
     slice prints no @QUANTUM line, and substituting a number here would be this reader asserting a
     denomination the run never claimed. */
  if (q === null)
    return `dispatch turns cost ${per.toFixed(0)} unit(s) of the scheduler's own measure each over the whole ` +
           `run (${b.stepUs} over ${b.steps} turns)${ivl} — this stage printed no @QUANTUM line, so there is ` +
           `no slice to read that against and the number is a rate with no yardstick rather than a verdict`;
  const sliceUs = q.sliceMs * 1000;
  const frac = per / sliceUs;
  return `dispatch turns cost ${per.toFixed(0)} ${q.measure} microsecond(s) each over the whole run ` +
         `(${b.stepUs} over ${b.steps} turns)${ivl} — that is ${frac.toFixed(2)} of the ${q.sliceMs} ms ` +
         `cooperative slice, both sides in the same measure, so this quotient is what the run-to-run spread ` +
         `cannot reach. ` +
         (frac >= 0.5
           ? `A turn costing most of a slice means the loop makes about one choice per slice BY ` +
             `CONSTRUCTION: the number of picks a run can make is its thread time divided by the slice, and ` +
             `a frontier larger than that is not being under-served by the ORDER — it is a granularity ` +
             `floor, and no re-pricing of a weight term reaches it` +
             (q.cpu
               ? `. That is real thread CPU, so the turn CONSUMED it`
               : `. THAT IS NOT CPU: an equally good reading is a turn DESCHEDULED for a slice's worth of ` +
                 `wall, which the engine cannot tell apart either because the slice is armed on this same ` +
                 `clock. The consumed reading is not established here — only the arithmetic that follows ` +
                 `from a turn taking a slice's worth of the clock the scheduler runs on`)
           : `A turn well inside the slice means the loop is NOT slice-bound: the picks a run made are not ` +
             `capped by what a turn costs, so a small step count is a statement about how much thread time ` +
             `the loop was given rather than about the granularity` +
             (q.cpu ? `` : `. Not CPU, so a descheduled turn would have read HIGH — this reading is the ` +
                           `direction that survives that`));
}

/* AND WHERE IN ITS DOCUMENT THE FRONTIER'S MASS IS STANDING — the row `deepest` and `completed` structurally
   cannot give, and the reason one stall in this scheduler has now been diagnosed three ways, each reading
   refuting the last.
   THOSE TWO ARE GLOBAL MAXIMA (solver/engine.h). Each is set by whichever ONE member got furthest through the
   document, so `deepest 11` is exactly as true of a frontier holding one member at 11 and two thousand at 3
   as it is of one whose every member is at 11 — and the sentence a reader wants ("the frontier is not
   advancing") is true of the first and FALSE of the second. They take opposite work: the first is a claim
   about what the pick prefers, and the second is BFS behaving precisely as designed on a page that forks,
   where the work is unbounded and there is nothing to fix. No maximum separates them at any value.
   WHAT EACH LOOKS LIKE HERE, SO THE NEXT READER CAN TELL WHICH THEY ARE HOLDING WITHOUT ASKING ANYBODY.
   Measured on the histogram's own code over both populations: a frontier of 2155 members with `deepest 11`
   renders as `2154 at 3, 1 at 11` under the first and as `2155 at 11` under the second — same `live`, same
   `deepest`, same extent, and the buckets are the only thing that differs. So the reading is the comparison
   between where the MASS is and what `deepest` says, and both numbers are on this one line.
   THE DENOMINATOR IS ON THE LINE WITH THE NUMBERS, which is why `live` and the extent are printed beside
   them rather than left in the census: a bucket count with nothing to be a fraction OF is not a coverage
   figure, and this project has produced several that were true of half of what they appeared to describe.
   IT DECIDES NOTHING (§NO BOUNDS). No arm of any verdict in this file reads it, nothing branches on it, and it
   never refuses a run: a frontier whose mass sits low is a READING, and a reader that failed a build for one
   would be the no-progress count this project forbids. What it throws on is the census being internally
   untrue — a row set that does not partition `live` — which is a statement about the document and not about
   the run. */
function programCursorReading(b) {
  const rows = censusHistRows(b, "programCursors", "live",
                              "the live members' own cursors (solver/cold.h), whose extent is therefore the " +
                              "deepest program any standing member is at rather than a fixed list — and which " +
                              "is still never empty, because solver/cold.c gives an empty frontier program 0");
  /* `deepest` AND `completed` ARE THE THING THIS ROW IS READ AGAINST, so their absence is not a missing
     decoration — it leaves the histogram with nothing to be low or high RELATIVE TO, which is the whole
     reading. They are in the derived @COLD row set and `censusFields` has already refused a non-numeric
     one by the time this runs; this says so rather than re-checking, because a second check here would
     be the second place a renamed row has to be renamed. */
  const at = rows.filter((r) => r[1] > 0);
  /* AN EMPTY FRONTIER IS A SENTENCE AND NOT AN EMPTY LIST, for `stepUnitReading`'s reason exactly: rendering
     nothing there reads as a histogram that failed rather than as a census taken with nobody standing, and
     those take opposite work. The row set is `{"0":0}` in that state by construction, so this arm is reached
     through a real measurement and not through a hole. */
  if (at.length === 0)
    return `program cursors at the last census: no member was standing (live ${b.live}), so all ` +
           `${rows.length} of the histogram's rows read 0 — a census taken on an empty frontier, which is a ` +
           `measurement and not an absent row`;
  const top = at.reduce((x, r) => (r[1] > x[1] ? r : x), at[0]);
  const standingDeepest = at.reduce((x, r) => (Number(r[0]) > x ? Number(r[0]) : x), Number(at[0][0]));
  /* THE BUCKETS ARE CURSORS AND `deepest` IS A PROGRAM INDEX, AND THIS SENTENCE USED TO CALL THEM BOTH
     PROGRAMS. solver/flow.h's `script_i` runs over [0, dyn_n] — closed at the top, because `dyn_n` is what a
     member holds between two programs at the tail — so the histogram is ONE BUCKET WIDER than the document has
     programs and its top bucket is the members with no row left. Rendered as "N program slots" and "at program
     11" beside `document deepest 10`, that reads as a gauge naming a program the document does not have, and
     it was read that way: a lane took the pair for two instruments contradicting each other and stopped, which
     is the right instinct applied to a disagreement that does not exist. The unit is named on the line now,
     the cursor that MEANS "finished the deepest program" is spelled out, and solver/result.c asserts the
     identity (top cursor <= deepest + 1) at the composer where both numbers are in one hand. */
  const finishedAll = Number(b.deepest) + 1;
  return `program cursors at the last census (${b.live} live member${b.live === 1 ? "" : "s"} over ` +
         `${rows.length} cursor slot${rows.length === 1 ? "" : "s"} — a CURSOR is one-past-the-program-it-left, ` +
         `so the slots run one wider than the document's programs; ` +
         `document deepest ${b.deepest} / completed ${b.completed}, and ${b.outOfPrograms} member` +
         `${b.outOfPrograms === 1 ? " has" : "s have"} no row left to run): ` +
         at.map((r) => `${r[1]} at ${r[0]}`).join(", ") +
         ` — largest bucket ${top[1]} of ${b.live} at cursor ${top[0]}, deepest member standing at cursor ` +
         `${standingDeepest}. A mass LOW against \`deepest\` and a mass AT it are opposite diagnoses ` +
         `(solver/cold.h): the first is a frontier whose mass never advances while a few members run deep, ` +
         `the second is BFS on a forking page with unbounded work — and "AT it" is cursor ${finishedAll}, ` +
         `not ${b.deepest}, because a member that has finished the deepest program stands one past it. ` +
         `This row states which and decides nothing`;
}

/* THE COLD ROUND TRIP, PER RECORD KIND — @COLDPARK from session ONE against @COLDRESUME from session TWO, and
   `orphansMet`/`orphansUnmet` beside them, which is the round trip's own VERDICT and which nothing has ever
   read. §Time-travel-resume rests entirely on this pair of sessions, and until now the only thing said about
   it was `[build] cold round trip (native) — residue at <path>`: a statement that two processes exited 0.
   WHAT A KIND-BY-KIND COMPARISON CATCHES THAT AN EXIT CODE CANNOT. A residue that carried a kind and a resume
   that rebuilt none of it are two numbers, ABSENT, and with nothing reading them a round trip that silently
   stopped rebuilding foreign world segments or orphan drives looks exactly like one that worked. That is why
   test_forced.c added `worlds` to BOTH lines at once: "a kind that is written and never reported is a kind
   whose absence and whose zero read alike."
   `orphansUnmet` IS THE VERDICT AND `orphansMet` IS CONTEXT, in test_forced.c's own words. Met can legitimately
   EXCEED the records — a waiting drive forks arms while it replays and every arm is the same drive of the same
   body — so met-minus-claims is NOT a loss and must never be reported as one. Unmet is the loss, exactly: on a
   document whose bytes did not change between two sessions it is ZERO, and a resumed frontier whose most
   expensive members drive nothing is otherwise indistinguishable from one that worked.
   SAME FIELD CONTRACT AS EVERY OTHER CENSUS READER HERE: an absent name throws rather than being compared as
   undefined. THE ABSENCE OF A LINE IS REPORTED AS AN ABSENCE — a park that was never taken prints no
   @COLDPARK, and reporting `0 records` for that would be the same lie as counting a marker nothing writes. */
/* THE TWO CONTRACTS IN FULL — spelled out rather than spread into each other, because a field list is only a
   contract if a reader can see the names in it: a computed key is a name that is not a static fact, and this
   file's own record-field gate refuses one rather than guessing past it.
   AND `store` IS NOT IN IT, BECAUSE THIS CONTRACT CERTIFIES NUMBERS AND `store` IS A PATH. `censusFields` is
   one implementation of one contract — "every name in it is present and is a NUMBER" — and `store` was listed
   under it while test_forced.c prints it as `\"store\":\"%s\"`, so `oneCensus` threw `has no numeric
   \`store\`` on the exact bytes its own composer emits. That is not a strict reader refusing a broken census:
   it is the reader unable to accept a correct one, so the round-trip report — which is guarded on both
   sessions exiting 0 and reached only when a park HAPPENED — could not print for the runs it exists to
   describe. A row's KIND is not in its key, and a list that says NUMBER about a string is the same defect as
   summing a gauge: the check passes for the wrong reason or fails for no reason, and here it was the second.
   The shelf identity is asserted at `coldRoundTrip`, where the value is READ and where a reader who hits it
   is already holding both paths. */
const COLDPARK_FIELDS = ["records", "segs", "flows", "cands", "orphans", "worlds", "bytes"];
/* THE @S ARRIVAL CENSUS, SPELLED AS THE RESULT DOCUMENT SPELLS IT. test_forced.c prints the same four numbers
   the document carries as `_sourceReads`/`_sinkReached`/`_sinkTainted`/`_sinkSuppressed`, from the same
   producers, and it prints them under the document's own names — one namespace, so a reader who learns these
   off `@RESULT` can read them off the line and a rename breaks in one place rather than drifting in two. */
const SCENSUS_FIELDS = ["_sourceReads", "_sinkReached", "_sinkTainted", "_sinkSuppressed"];
const COLDRESUME_FIELDS = ["segs", "flows", "cands", "orphans", "worlds", "orphansMet", "orphansUnmet"];
/* THE ORPHAN-DRIVE CENSUS, SPELLED AS THE RESULT DOCUMENT SPELLS IT, for SCENSUS_FIELDS' reason exactly — the
   same two producers reach a reader twice (this line, and `result_json`'s `_orphansDriven`/`_orphansAsked`
   which bridge.js asserts and the popup renders), so a reader who learns the names off `@RESULT` reads them
   off the stream and a rename breaks in one place instead of drifting in two. */
const OCENSUS_FIELDS = ["_orphansDriven", "_orphansAsked"];
/* ONE FIELD CONTRACT, ONE IMPLEMENTATION OF IT. Three readers here assert the same thing about a census line —
   every name in the contract is present and is a NUMBER — and the assertion had been written out twice
   already; a third copy is the drift this file's own record-field gate exists to catch, one level up in the
   instrument rather than in the tree it audits. The COMPOSER is a parameter because it is the only part that
   differs: it is what a reader who hits this throw has to go and open. */
function censusFields(v, marker, fields, composer) {
  for (const f of fields)
    if (typeof v[f] !== "number")
      throw new Error(`[build] the ${marker} census has no numeric \`${f}\` — this reader compares ` +
                      `${fields.join(", ")} and ${composer} is what decides they exist; a renamed ` +
                      `field must be renamed here rather than silently compared as undefined.`);
  return v;
}
/* A MARKER'S WHOLE STREAM, SPLIT AT THE POINTS ITS PRODUCER RESTARTS THE COUNT — the shape a reader needs when
 * the fact is a TIME SERIES and not a final number, which is a different question from the one `oneCensus` and
 * `lastTwo` answer and is why this is a third helper rather than an argument to either.
 *
 * WHY IT SPLITS AT ALL, AND WHY A DROP IS NOT CORRUPTION. solver/engine.c releases the orphan counters with
 * the agent (`g_orphans_driven = 0; g_orphan_asks = 0;`) and its own comment says why they are per-SESSION and
 * not per-process: "the whole of what `asked` is for is that `asked == 0` means NO FLOW IN THIS SESSION ever
 * reached the end of its own work. A carried-over count makes that read `asked > 0` for a session that never
 * asked at all". A host that takes one runtime down and brings another up therefore emits two rising ramps on
 * ONE stdout, and last-minus-first across the pair is a difference between two different sessions' counters —
 * a number about nothing, which is the defect §Testing names when it says an artifact of HOW you asked must
 * never be reported as a fact about WHAT you asked. So a decrease is READ as the boundary it is, the sessions
 * are returned as sessions, and the caller states which one it is reading rather than averaging them.
 *
 * MONOTONE WITHIN A SESSION IS THEN AN INVARIANT AND NOT AN ASSUMPTION: the only writes to either counter are
 * `++` and that release, so inside one segment the sequence can only rise. The split is what makes that true,
 * which is why the caller may compare two samples of one segment at all.
 *
 * THE BOUNDARY IS *ANY* COUNTER FALLING AND NOT ALL OF THEM, AND THE STRICTER RULE IS THE WRONG ONE — written
 * down because it is what the next reader will reach for, and it would fire on the round trip WORKING. The
 * release zeroes the pair together, but the first sample AFTER it is not taken at that instant: a session that
 * inherits drives from a residue raises `driven` on the cold-tier path before any flow has asked, so a stream
 * running (driven 0, asked 400) -> release -> (driven 5, asked 0) shows `asked` falling while `driven` RISES.
 * Requiring every counter to fall would read that legal boundary as one continuous session and splice two
 * sessions' counters into one ramp. Within a session neither counter can fall, so one falling is already
 * proof of a boundary and is the whole test. */
function censusSessions(out, marker, fields, composer, counters) {
  const s = [];
  for (const m of out.matchAll(new RegExp(`^${marker} (\\{.*\\})$`, "gm")))
    { try { s.push(censusFields(JSON.parse(m[1]), marker, fields, composer)); } catch (e) {
        if (e instanceof SyntaxError) continue;   /* a truncated tail line, as `lastTwo` reads one */
        throw e; } }
  if (s.length === 0) return null;
  const runs = [[s[0]]];
  for (let i = 1; i < s.length; i++) {
    if (counters.some((k) => s[i][k] < s[i - 1][k])) runs.push([s[i]]);
    else runs[runs.length - 1].push(s[i]);
  }
  return runs;
}
function oneCensus(out, marker, fields) {
  const m = [...out.matchAll(new RegExp(`^${marker} (\\{.*\\})$`, "gm"))];
  if (!m.length) return null;
  let v;
  try { v = JSON.parse(m[m.length - 1][1]); }
  catch { throw new Error(`[build] the last ${marker} line is not JSON — test_forced.c composes it in one ` +
                          `printf, so a line that will not parse is that printf truncated or interleaved.`); }
  return censusFields(v, marker, fields, "test_forced.c's printf");
}
function coldRoundTrip(v1, v2, store) {
  const park = oneCensus(v1.captured, "@COLDPARK", COLDPARK_FIELDS);
  const res = oneCensus(v2.captured, "@COLDRESUME", COLDRESUME_FIELDS);
  if (!park) return "session ONE printed no @COLDPARK line, so it took no park and there is no round trip to " +
                    "report — this is the absence of the measurement, not a residue of nothing";
  if (!res) return `session ONE parked ${park.records} record(s) and session TWO printed no @COLDRESUME line, ` +
                   `so nothing says what it rebuilt`;
  /* THE SHELF IS THE SAME SHELF, ASSERTED RATHER THAN ASSUMED. This stage hands session ONE a path and hands
     session TWO the same path, and if the fixture wrote its residue somewhere else BOTH sessions still exit 0
     — session two resumes from whatever is at the path it was given (an earlier build's residue, or nothing)
     and every number below is then a comparison between two unrelated runs. It is one string and it is the
     one thing that makes the rest of this function a round trip rather than two censuses side by side. */
  /* AND ITS KIND IS ASSERTED HERE, WHERE IT IS READ — the one row of this census that is not a number, so it
     is outside `censusFields`' contract and inside this one. An ABSENT row and a MISMATCHED shelf are
     different facts and the sentence below would report the first as the second, naming `undefined` as the
     path session ONE parked to; that is a composer change wearing a round-trip failure's message. */
  if (typeof park.store !== "string" || !park.store.length)
    throw new Error(`[build] the @COLDPARK census carries no \`store\` string — test_forced.c prints it as ` +
                    `\`"store":"%s"\` on every park it takes, so its absence is that printf having changed ` +
                    `rather than a park that went nowhere, and the shelf identity below cannot be asked of a ` +
                    `path this census did not state.`);
  if (park.store !== store)
    throw new Error(`[build] session ONE parked its residue at ${JSON.stringify(park.store)} and this stage ` +
                    `handed it ${JSON.stringify(store)} — session TWO resumes from the path THIS stage names, ` +
                    `so the two sessions are not two ends of one round trip and every kind compared below ` +
                    `would be a comparison between unrelated runs.`);
  /* A KIND WRITTEN AND NOT REBUILT IS THE FINDING, and each is named at its own read rather than looped over a
     list of strings: the whole value of splitting the residue by kind is that the arm which stopped working is
     the arm to look at, and a kind reached through a computed key is a kind no reader of this file can see. */
  const kinds = [["segs", park.segs, res.segs], ["flows", park.flows, res.flows],
                 ["cands", park.cands, res.cands], ["orphans", park.orphans, res.orphans],
                 ["worlds", park.worlds, res.worlds]];
  const lost = kinds.filter(([, p, r]) => p > 0 && r < p).map(([k]) => k);
  const never = kinds.filter(([, p]) => p === 0).map(([k]) => k);
  return `${park.records} record(s) / ${park.bytes} B parked into ` +
         kinds.map(([k, p, r]) => `${k} ${p}→${r}`).join(", ") +
         (lost.length ? `; REBUILT SHORT on ${lost.join(", ")} — a kind session ONE wrote and session TWO did ` +
                        `not fully re-materialize is the arm to look at`
                      : `; every kind session ONE wrote came back`) +
         (never.length ? `; ${never.join(", ")} carried NOTHING to exercise (a zero the residue wrote, not a ` +
                         `rebuild that failed)` : ``) +
         `; inherited drives: ${res.orphansMet} met, ${res.orphansUnmet} UNMET` +
         (res.orphansUnmet > 0
           ? ` — unmet is the round trip's loss, exactly: on a document whose bytes did not change between the ` +
             `two sessions it is ZERO, so every one of these is an inherited drive that finished having been ` +
             `handed no body. (met ABOVE the record count is not a loss: a waiting drive forks arms while it ` +
             `replays and every arm is the same drive of the same body.)`
           : ` — zero unmet is the pass: every inherited drive that waited for a body was handed one`);
}

/* WHAT THE HEAP, THE DELTA CHAINS AND THE FORK TABLE WERE DOING — the @HEAP, @SWAP and @FORKAT censuses, which
   are emitted at the SAME cadence as @COLD and, until they rode solver/result.c's document, were printed only
   by `run_scheduler`, a loop `qjs_step` never enters. So `hungCause` below has always ended with "the frontier
   is doing something this discriminator does not model" while three streams that model exactly those things
   existed and no reader took them.
   EACH ROW HERE ANSWERS A CAUSE THE FRONTIER COUNTS STRUCTURALLY CANNOT.
     `arenaKiB` vs `cLiveKiB` — the allocator's high-water mark against what it currently holds. In wasm linear
        memory ONLY GROWS, so a run whose `cLive` is flat while `arena` climbs is FRAGMENTING and not leaking,
        and the two have entirely different fixes. `finished`/`live`/`blocked` cannot see either.
     `childRealms`/`childRealmsMade`/`childRealmsPeak` — §A-CAPABILITY-MATERIALIZED-PER-FLOW's ceiling: one
        realm per flow that creates a navigable with an address. THE THREE ARE READ TOGETHER AND THE LIVE ONE
        ALONE DECIDES NOTHING — a small `childRealms` is the answer both for a run that built none and for a
        run that built a great many and reclaimed every one, which are opposite facts. `made` is monotone and
        `peak` is the high-water live, so `made == peak` says not one realm was ever given back (the ceiling)
        and `made > peak` says HTML §7.5.10 "Destroying documents" step 9's reference drop ran. This line used
        to assert "none reclaimed" as a standing fact, which stopped being true when that step landed —
        core/frame/window_proxy.c's window_proxy_set_destroyed releases the Window that is the one counted
        reference to a child realm. navigable.c's OOM CHECK sends its reader to these numbers BY NAME.
     `unattributed`, `trampFrames`, `stepMachines` — what the residual is MADE of. "The heap grew" names nothing
        to fix; a residual that is suspended heap frames and a residual that is atoms have different owners.
     `mean` and `heapSegs` — the COST of a context switch and the RETENTION under it, which are independent and
        the second is invisible in the first: a frontier of four flows whose chains hold tens of thousands of
        frozen segments is a lifetime bug that reads exactly like a healthy run in `installs`/`entries`/`worst`.
     the @FORKAT table's largest row — WHICH PREDICATE is growing the frontier, which is the first question a
        run whose frontier explodes asks and the one nothing in @COLD or @H can answer.
   SAME FIELD CONTRACT AS @COLD AND @WFQ: the names are solver/result.c's composers, and an absent one THROWS
   rather than being silently compared as undefined. ABSENCE OF THE STREAM IS REPORTED AS ABSENCE, because a
   stage that drives no scheduler prints none of these and for that stage the silence is expected. */
/* BOTH ROW SETS ARE THE CONTRACT IN FULL AND BOTH ARE DERIVED, for `coldFields`' reason exactly — every row
   solver/result.c's `result_heap_json` and `result_swap_json` publish, whether or not the sentences below read
   it, taken from those composers' own format strings rather than retyped here. These two AGREED with their
   composers name for name at the revision the third stopped agreeing with its own, which is not evidence that
   a hand-kept copy works: it is what the @COLD list looked like the day before three rows landed in the
   composer and in no reader. The empty object set is the positive statement that neither census splices an
   object — see `censusRowSet`, which is what refuses the day one does. */
let g_heapFields = null;
const heapFields = () => (g_heapFields ??= censusRowSet(
  "solver/result.c", "char *result_heap_json(JSContext *ctx)", "\n}\n", [],
  "the @HEAP reader states which rows it requires of the runtime's memory census, and it takes that set " +
  "from the composer rather than from a list beside it"));
let g_swapFields = null;
const swapFields = () => (g_swapFields ??= censusRowSet(
  "solver/result.c", "char *result_swap_json(void)", "\n}\n", [],
  "the @SWAP reader states which rows it requires of the delta-swap census, and it takes that set from the " +
  "composer rather than from a list beside it"));
function lastTwo(out, marker, fields, composer) {
  const s = [];
  for (const m of out.matchAll(new RegExp(`^${marker} (\\{.*\\})$`, "gm")))
    { try { s.push(JSON.parse(m[1])); } catch { /* truncated tail */ } }
  if (s.length === 0) return null;
  const b = s[s.length - 1], a = s[Math.floor((s.length - 1) / 2)];
  for (const c of [a, b]) censusFields(c, marker, fields, composer);
  return { a, b, n: s.length };
}
/* THE LARGEST OF A NAMED SET, AND WHICH OF THEM MOVED — the shape every reading below is built out of. The
   pairs are written with each field NAMED at its read rather than looped over a list of strings, and that is
   deliberate: a field name reached through a computed key is a name no reader of this file and no auditor of
   this seam can see, so a row solver/result.c renamed would go on being summed under a key that no longer
   exists. Naming them is what makes the two halves of the contract legible from one side. */
const biggest = (pairs) => pairs.reduce((x, p) => (p[1] > x[1] ? p : x), pairs[0]);
const shareText = (pairs, total) => {
  const t = biggest(pairs);
  return total > 0 ? `${t[0]} ${t[1]} (${Math.round(100 * t[1] / total)}%)` : `nothing — all ${pairs.length} are 0`;
};
function censusReading(out) {
  const h = lastTwo(out, "@HEAP", heapFields(), "solver/result.c's result_heap_json");
  const w = lastTwo(out, "@SWAP", swapFields(), "solver/result.c's result_swap_json");
  const c = lastTwo(out, "@COLD", coldFields(), "solver/result.c's result_cold_json");
  /* AND AN ABSENT @COLD STREAM IS AN ABSENCE, WHICH THIS GUARD IS AND THE DEREFERENCE ABOVE IT WAS NOT. The
     banner over this reader states it — "a stage that drives no scheduler prints none of these and for that
     stage the silence is expected" — and `h`, `w` and the cold arms below are all reached through `if (…)`,
     while the internal-truth checks were reading `c.a` off a `lastTwo` that returns NULL for exactly that
     stage. A TypeError there is not this file refusing a broken census; it is the reader falling over on the
     one input its own contract says to expect, and it names neither the marker nor the stage. */
  if (c) {
    /* THE MEMBER-SIDE SUBSET CHAIN, CHECKED HERE FOR THE REASON THE @WFQ PAIRS ARE CHECKED IN THEIR OWN
       READER: solver/result.c asserts `can_deliver <= stack_empty <= flows` in a DCHECK that is compiled out
       of a release build, and this reader still runs on that build's bytes. The three rows are one walk of one
       frontier — `canDeliver` is the reply-delivery arm's whole guard and `stackEmpty` its left conjunct — so
       an excess is not a large number, it is two sums over different populations, and every sentence composed
       out of the pair below would then be a reading of the disagreement rather than of the run. */
    for (const x of [c.a, c.b])
      if (x.canDeliver > x.stackEmpty || x.stackEmpty > x.live)
        throw new Error(`[build] the @COLD census reports ${x.canDeliver} member(s) that can DELIVER a reply, ` +
                        `${x.stackEmpty} whose execution context stack is empty and ${x.live} live — the ` +
                        `first is the reply-delivery arm's whole guard and the second its left conjunct, ` +
                        `counted in one pass of cold_census over one frontier, so this is two sums over ` +
                        `different populations and not a frontier state.`);
    /* AND THE REPLAY LEDGER'S IDENTITY, FOR THE SAME REASON AND WITHIN ONE SAMPLE ONLY. solver/result.c
       asserts it where all three rows are in one hand and that DCHECK is compiled out of the build this
       reader's bytes come from, so the three rows reach a human and — until this line — no automated reader.
       KIND AND UNIT, READ OFF THE ACCESSOR AND NOT THE KEYS (solver/decide.h states both beside
       `decide_replay_stats`, whose body is three plain static reads with no division behind it): all three
       are LIFETIME counts over the SESSION, so they are differenceable between two samples of one session —
       and `replayHits` and `replayLeftArms` are ARMS (decision-vector slots) while `replayLeft` is EVENTS,
       one per divergence whatever it abandoned. The middle name reads as arms and is not, which is the
       `svcMax` shape and the reason this comment states the unit rather than the row's spelling.
       IT IS CHECKED PER SAMPLE AND NEVER ACROSS THE PAIR. §Testing: an identity holds WITHIN one sample and
       nowhere else, and this session has already paid for the other reading — two rows taken at two ends of a
       run were differenced into a contradiction that held of no quantity, and three mechanisms were written
       down for it before the file's own owner read the constant that explained it. `lastTwo` hands back the
       last census and the middle one, which may be two different SESSIONS of one stdout (`censusSessions`
       exists because the host takes one runtime down and brings another up), so a difference of these rows
       across the pair is a number about nothing. Each sample is asked alone.
       BOTH CLAUSES, BECAUSE `replayLeftArms >= replayLeft` ALONE PERMITS `replayLeft == 0` BESIDE A NON-ZERO
       SUM — which is what a counter incremented on the wrong side of dec_leave_path's early return produces,
       and it is the one state a single-clause check would pass. Both come from that function's own
       precondition (`g_c < dec_total()`, so every divergence abandons at least one arm). A break publishes
       `replayLeftArms` as a loss no `replayLeft` accounts for, or a divergence that abandoned nothing. */
    for (const x of [c.a, c.b])
      if (x.replayLeftArms < x.replayLeft || (x.replayLeft === 0) !== (x.replayLeftArms === 0))
        throw new Error(`[build] the @COLD census reports replayLeft ${x.replayLeft} divergence event(s) ` +
                        `abandoning replayLeftArms ${x.replayLeftArms} arm(s) — every call of ` +
                        `dec_leave_path abandons AT LEAST ONE arm (its precondition is that the cursor is ` +
                        `short of the end), so the arm total can be neither smaller than the event count nor ` +
                        `zero beside a non-zero one. The two are counted at one site two lines apart and are ` +
                        `read here out of ONE census, so this is the ledger and not the sampling: one of the ` +
                        `two increments is on the wrong side of that function's early return. They are about ` +
                        `to be published as the statement of what this session's resume did with its ` +
                        `recorded path (\`replayHits\` ${x.replayHits} arm(s) honoured).`);
  }
  const parts = [];
  if (h) {
    const grew = (k) => h.b[k] - h.a[k];
    /* FRAGMENTING AND LEAKING ARE NAMED APART rather than both reported as "memory grew", because engine.h
       states the difference and it is the whole of what a reader does next. */
    parts.push(`heap: live ${h.b.cLiveKiB} KiB / arena ${h.b.arenaKiB} KiB` +
               (grew("arenaKiB") > 0 && grew("cLiveKiB") <= 0
                 ? ` — arena grew ${grew("arenaKiB")} KiB while live did NOT, which is FRAGMENTATION and not a leak`
                 : grew("cLiveKiB") > 0 ? ` — live grew ${grew("cLiveKiB")} KiB` : ` — flat`) +
               /* THE THREE REALM NUMBERS ARE PRINTED AS ONE SENTENCE, because reading the live one alone is
                  the mistake this row exists to stop. The verdict is the comparison and never the count. */
               `; child realms ${h.b.childRealms} live / ${h.b.childRealmsPeak} peak / ` +
               `${h.b.childRealmsMade} made` +
               (h.b.childRealmsMade === 0
                 ? ` — none built, so this run says nothing about reclamation`
                 : h.b.childRealmsMade > h.b.childRealmsPeak
                   ? ` — RECLAIMED: at least one realm died while others were being made`
                   : ` — NOT ONE RECLAIMED: every realm this run built was live at once, which is the ceiling`) +
               `; ${h.b.trampFrames} heap frame(s), ` +
               `${h.b.stepMachines} suspended builtin(s), ${h.b.unattributed} B the runtime cannot name`);
    /* WHICH KIND GREW, WHICH IS THE COMPARISON result_heap_json'S OWN COMMENT DESCRIBES AND NOTHING PERFORMED.
       "A climbing `allocations` with a flat `objects` is memory no GC object owns — an atom, a string, a
       property table, a bytecode function — and each of those has a different owner and a different place
       where the owner forgot to let go." Every one of those kinds was emitted on every census of every run
       and no reader had ever compared two of them. */
    const top = biggest([["allocations", h.b.allocations - h.a.allocations],
                         ["atoms", h.b.atoms - h.a.atoms], ["strings", h.b.strings - h.a.strings],
                         ["objects", h.b.objects - h.a.objects], ["shapes", h.b.shapes - h.a.shapes],
                         ["props", h.b.props - h.a.props], ["funcs", h.b.funcs - h.a.funcs],
                         ["funcCode", h.b.funcCode - h.a.funcCode],
                         ["arrays", h.b.arrays - h.a.arrays],
                         ["miscParts", h.b.miscParts - h.a.miscParts]]);
    parts.push(`heap kinds: ${top[1] > 0 ? `${top[0]} grew most (+${top[1]})` : `nothing grew`}` +
               (h.b.allocations > h.a.allocations && h.b.objects <= h.a.objects
                 ? `, and allocations climbed with objects FLAT — memory no GC object owns`
                 : ``));
    /* AND WHERE THE ATTRIBUTED BYTES ARE, which is the other half of the same sentence: the counts say how
       many of each kind, and only the SIZE rows say which kind is the memory. `miscBytes` is quickjs's total
       and is the denominator rather than a row beside them — result_heap_json says why summing it with the
       others counts the named heap twice. */
    parts.push(`heap bytes: ${h.b.miscBytes} B attributed, largest ` +
               shareText([["objBytes", h.b.objBytes], ["propBytes", h.b.propBytes],
                          ["shapeBytes", h.b.shapeBytes], ["strBytes", h.b.strBytes],
                          ["atomBytes", h.b.atomBytes], ["funcBytes", h.b.funcBytes],
                          ["arrayElemBytes", h.b.arrayElemBytes]], h.b.miscBytes));
  }
  if (w)
    parts.push(`swap: ${w.b.installs} switches over ${w.b.entries} delta entries, ${w.b.mean} each and ` +
               `${w.b.worst} at the worst; chains holding ${w.b.heapSegs} heap segment(s) ` +
               `(${w.b.heapSegEntries} entries) + ${w.b.domSegs} DOM (${w.b.domSegEntries})` +
               (w.b.heapSegs > w.a.heapSegs ? ` and still growing` : ``));
  if (c) {
    parts.push(retiredReading(c.b));
    /* WHAT THE PARKED FRONTIER WEIGHS AND WHICH HALF OF IT — the pager's own trade, and the reason
       result_cold_json splits per-flow from shared: the first multiplies by the frontier's size and the second
       does not, so a frontier that is expensive because it is BIG and one that is expensive because its shared
       prefixes are huge take opposite work. Neither total had a reader, nor did any of the rows they are made
       of. */
    parts.push(`frontier weight: ${c.b.perFlowKiB} KiB per-flow (largest ` +
               shareText([["decKiB", c.b.decKiB], ["headKiB", c.b.headKiB],
                          ["domHeadKiB", c.b.domHeadKiB], ["pendKiB", c.b.pendKiB],
                          ["miscKiB", c.b.miscKiB]], c.b.perFlowKiB) +
               `) + ${c.b.sharedKiB} KiB shared (largest ` +
               shareText([["segKiB", c.b.segKiB], ["domSegKiB", c.b.domSegKiB],
                          ["pinSegKiB", c.b.pinSegKiB], ["decSegKiB", c.b.decSegKiB],
                          ["dynKiB", c.b.dynKiB]], c.b.sharedKiB) + `)`);
    /* AND WHAT IT IS MADE OF IN ENTRIES RATHER THAN BYTES, because those answer different questions: a chain
       that is long and a chain that is heavy have different causes, and `decEntries` in particular is the
       number that was QUADRATIC before the decision vector was shared — so a run where it climbs with the
       frontier's size rather than with its depth is that sharing having stopped working. */
    parts.push(`frontier shape: ${c.b.framed} framed of ${c.b.live} live, ` +
               `${c.b.decEntries} decision + ${c.b.headEntries} heap + ${c.b.domHeadEntries} DOM head ` +
               /* `pend` IS A REGISTER LENGTH AND NOT A DEBT, and rendering it as one cost a reading. It sums
                  every entry of every live flow's register whatever state it is in — outstanding, ALREADY
                  ANSWERED and awaiting that flow's own delivery, declined, synchronous — so it stood at
                  299306 beside `owed 0` and `blocked 0` in the same census, which read as a contradiction and
                  was not one. The debt is `pending_owed_replies` and the RATE is the `reply` pair below.
                  AND ONE OF THOSE FOUR STATES IS NOW SPLIT OUT BESIDE IT, because naming the collapse did not
                  end it: a reader still had to choose which of the four a rising `pend` was, and the two
                  answers that mattered — a host that never paid, and a frontier drowning in replies nobody
                  took — are the two that take opposite work. `pendReady` is the second, in the unit
                  `deliver-one-reply` consumes, so the arm and its debt are finally on one line. The rest of
                  `pend` is what the host is still owed plus the synchronous and declined entries, which is why
                  this prints both rather than the difference. */
               `entries, ${c.b.jobs} queued job(s), ${c.b.pend} pending register entr(ies) ` +
               `(${c.b.pendReady} awaiting their own flow's delivery), ` +
               `${c.b.dynBodies} shared program(s); frozen: ${c.b.pinSegs}/${c.b.pinSegEntries} pin, ` +
               `${c.b.decSegs}/${c.b.decSegEntries} decision`);
    /* AND WHO COULD TAKE ANY OF THAT, WHICH IS THE HALF THE LINE ABOVE COUNTS IN THE WRONG UNIT. `pendReady`
       is over ENTRIES and so is silent about eligibility; solver/engine.c guards the reply-delivery arm on
       `flow_stack_empty(f) && flow_pending_ready(f)`, and those two rows are that guard and its left conjunct
       counted over MEMBERS. Without them a debt of hundreds of thousands of answered entries beside
       `deliver-one-reply: 0` has two opposite readings and the census could state neither: the members that
       may take a reply are not being CHOSEN (an ordering finding, and the @WFQ census's `delivReady`/
       `delivWGap` are where it is read), or there are almost none because the frontier is INSIDE its programs
       (a resume-seam finding, which no re-pricing of a weight term touches).
       IT IS RENDERED ON EVERY CENSUS AND NOT ONLY WHERE THE DEBT IS LARGE, for the reason the @WFQ leader row
       is: computed on one arm and dropped on the others it would be a reading with a writer and no reader on
       most runs, which is the defect this whole instrument exists to end.
       `stackEmpty` IS NOT `live - framed` AND THE DIFFERENCE IS THE POINT. flow_stack_empty has a second half
       — HTML §4.12.1.1 "Processing model"'s prepare the script element ends "Otherwise, immediately execute
       the script element el, even if other scripts are already executing", so that program runs INSIDE the one
       that inserted it and the stack has NOT emptied across it — and a member holding such a row at its cursor
       has no live frame and a non-empty stack. So `framed` bounds this row and never counts it, which is why
       the engine asks the predicate rather than the field, and why the gap between the two is reported here
       instead of being subtracted into silence. */
    const immediate = c.b.live - c.b.framed - c.b.stackEmpty;
    parts.push(`reply eligibility: ${c.b.stackEmpty} of ${c.b.live} member(s) stand with an EMPTY JavaScript ` +
               `execution context stack (${c.b.framed} are inside a program` +
               (immediate > 0
                 ? `, and ${immediate} more hold an immediately-executed row at the cursor, which is a ` +
                   `non-empty stack with NO live frame — HTML §4.12.1.1 "Processing model"`
                 : ``) +
               `), of which ${c.b.canDeliver} hold a reply they could take` +
               (c.b.pendReady === 0
                 ? ` — there is no undelivered reply on this frontier, so the arm having nothing to do is not ` +
                   `a finding`
                 : c.b.canDeliver === 0
                   ? ` — so the ${c.b.pendReady} answered entr(ies) awaiting delivery are held by NOBODY who ` +
                     `may take one, and no ordering change reaches them: the frontier is inside its programs ` +
                     `and this is a resume-seam reading, not a WFQ one`
                   : ` — so the delivery arm IS reachable and whether it runs is a question about the ORDER, ` +
                     `which the @WFQ census's delivReady/delivWGap answer and this pair cannot`));
    /* AND WHETHER THE HOST PAID, WHETHER THE DOCUMENT GOT ANYWHERE, AND WHAT THE INHERITED DRIVES DID — three
       questions the frontier's size cannot answer and which each have a row that nothing read. `deepest`
       against `completed` says whether this document reaches its later programs at all; `orphanClaimsUnmet` is
       the cold round trip's loss, exactly.
       AND THERE ARE TWO DOORS, WHICH IS WHAT THIS LINE USED TO SAY WITH ONE NUMBER. It read `N/M asks paid`
       under a caption asking whether "a waiting frontier is waiting because of the RANKING or because nobody
       paid it" — the GENERAL question — while the pair it printed counts SYNCHRONOUS cross-instance
       rendezvous alone (engine.c's `mint_req`: two callers, both FLOW_PENDING_HOSTREQ). A document with no
       cross-document read therefore prints `0/0` for ever, correctly, and a reader took the general answer
       from it and reported that nothing is ever asked of the host. CLAUDE.md: a coverage figure states WHAT
       IT IS A FRACTION OF, in the same line, or it is not a coverage figure — so each ratio now names its
       door, and the reply door has a ratio to name. */
    parts.push(`payment: sync ${c.b.hostAnswered}/${c.b.hostAsked} rendezvous paid` +
               /* AND HOW MANY PEER TIMELINES ANSWERED BEYOND THE FIRST, which is a different population and
                  used to be added into the numerator above — a peer holding four timelines then read as four
                  payments for one ask, and the ratio this line exists to show was three times the truth. */
               (c.b.hostAnswersExtra ? `, +${c.b.hostAnswersExtra} extra peer-timeline answer(s)` : ``) +
               (c.b.hostAnswersLate ? `, ${c.b.hostAnswersLate} refused after close` : ``) +
               /* AND HOW MANY ASKS THE ENGINE TOOK BACK. A withdrawn rendezvous (Fetch §2 Infrastructure's
                  terminate a fetch controller) is an ask that will never be paid and is not a failure to pay,
                  so without this row the ratio above reads as a host falling behind by exactly the number of
                  fetches the page itself cancelled. */
               (c.b.hostTerminated ? `, ${c.b.hostTerminated} withdrawn` : ``) +
               (c.b.pagedReqs ? `, ${c.b.pagedReqs} taken by a sale` : ``) +
               /* AND THE REPLY DOOR, WHICH IS THE ONE §Learning-from-replies CALLS THE POINT AND WHICH THIS
                  LINE HAD NO NUMBER FOR AT ALL. A record enters the set the host is shown when its (method,
                  url) pair completes and leaves it answered, so this ratio is over RECORDS — the same unit
                  `engine_provide` returns and the same unit `pend` is a length of.
                  AND IT IS READ AGAINST `deliver-one-reply`, WHICH IS THE HALF IT CANNOT SEE. A payment is the
                  value reaching the REGISTER; the flow still has to take it, and that take is a step-unit arm
                  the histogram beside this already counts. `asked == answered` with that arm at 0 is a
                  document being paid in full and consuming nothing — measured exactly so on the wasm smoke at
                  74eb1d62, where the reply-dependent probe rows (fetch, then-chain, clone-body, body-bytes,
                  body-iso) were all 0 and 55% of the frontier's per-flow memory was undelivered replies.
                  THAT IS A PREDICATE ON THE ARM BEING ZERO AND IT IS NOT A RATIO, WHICH IS A DISTINCTION THIS
                  SENTENCE DID NOT MAKE AND SOMEBODY THEN NEEDED. This pair is per RECORD; the arm is per
                  NAMING — pending_fork gives the sibling a new array naming the SAME record and each arm owes
                  its own delivery — so on a forking document the arm may legitimately EXCEED `answered`, and
                  the quotient of the two is a percentage of nothing. It was taken as one: 998 arm runs against
                  24636 answered records was relayed as "about 4% consumed". The denominator in the arm's unit
                  is `pendReady` on the frontier-shape line above. */
               `; reply ${c.b.replyAnswered}/${c.b.replyAsked} record(s) answered` +
               `; programs: deepest ${c.b.deepest}, completed ${c.b.completed}` +
               `; forks ${c.b.forks}` +
               /* THREE STATES, THREE SENTENCES — and the middle one is why this is not a ternary. `no inherited
                  drives in this session` was printed for BOTH "a rebuild ran and carried none" and "no residue
                  was handed to this session at all", which is a claim about drives made out of a question that
                  was never asked. solver/result.c calls UNMET the verdict, so a reader taking that sentence at
                  face value reads a clean bill out of a session with no cold tier in it. `resumed` is the
                  positive statement that separates them, which is the whole reason it is a row of its own. */
               (c.b.resumed === 0
                 ? `; no residue was handed to this session, so the drive verdict is not a reading`
                 : c.b.orphanClaims
                   ? `; inherited drives ${c.b.orphanClaims} rebuilt, ${c.b.orphanClaimsMet} met, ` +
                     `${c.b.orphanClaimsUnmet} UNMET`
                   : `; a rebuild ran and carried no inherited drives`));
    /* WHICH ARMS OF THE COLD GRAMMAR THE REBUILD ACTUALLY RAN — a question the drive verdict above cannot
       answer and which had no reader at all. `resumed` says a rebuild happened; the sentence above says what
       it did for INHERITED DRIVES; neither says how much of the READ HALF of the cold tier the residue
       exercised. cold.c's own note: "a residue of nothing but 'f' records exercises neither park_unhex nor
       solve_resume_candidate nor the probe" — so a rebuild that landed four thousand flows and touched no
       segment, no candidate and no foreign world PROVED A QUARTER OF THE ROUND TRIP and read here as a whole
       one. cold.h calls these four "the observable that says which ARMS of the grammar ran"; this is the
       reader that makes them one.
       THE UNEXERCISED ARMS ARE NAMED, AND THAT IS THE WHOLE POINT. `resumedSegs 0` beside three other numbers
       is what the popup already renders generically, and a bare number under a bare field name is a number
       nobody can act on: the actionable fact is WHICH half of cold.c this session did not show to work. So
       every arm is stated as the WORD for what it rebuilt, and the ones that did not run are named again with
       the record letter and the mechanism, which is where a reader has somewhere to go.
       ONLY UNDER `resumed`. Under `resumed: 0` the four zeroes are the true decomposition of a rebuild that
       did not happen, so reporting them as arms that failed to run would be the two-states-one-number defect
       solver/result.c added the `resumed` row to END, rebuilt one layer up inside its reader.
       AND THE COUNTS ARE STATED ONCE. The `carried no inherited drives` arm above used to spell `resumedFlows`
       and `resumedCands` itself, which put two of these four numbers in this report under two names and only
       in one of the three drive states — the drift result.c refuses `resumedOrphans` for, and a decomposition
       that vanishes in exactly the state where `orphanClaimsUnmet` is the verdict.
       SEGMENTS AND WORLDS ARE NOT FLOWS, so the four are listed as four arms and never summed. cold.c asserts
       `flows + cands` IS the landed-flow count, and says of a 'w' record "IT IS NOT A FLOW and is deliberately
       outside `flows`". */
    if (c.b.resumed !== 0) {
      const arms = [["segments", c.b.resumedSegs, "'s' — the shared decision prefixes every flow stands on"],
                    ["flows", c.b.resumedFlows, "'f' — exploration flows"],
                    ["candidates", c.b.resumedCands, "'c' — @S candidate sessions, solve_resume_candidate"],
                    ["foreign worlds", c.b.resumedWorlds, "'w' — peer timelines rebuilt through world_segment"]];
      /* THE PRODUCER'S OWN DERIVATION, CHECKED RATHER THAN ASSUMED. result_cold_json emits `resumed` as
         literally `flows + cands > 0`, so a census claiming a rebuild whose two flow-producing arms are both
         zero is that composer contradicting itself, and every arm sentence below would be describing a
         rebuild this same document says did not land anything. It throws for the reason a @COLD row that
         solver/result.c renamed throws: the alternative is a reading composed out of a census that is
         not internally true. */
      if (c.b.resumedFlows + c.b.resumedCands === 0)
        throw new Error(`[build] the @COLD census says resumed=${c.b.resumed} with resumedFlows and ` +
                        `resumedCands both 0 — solver/result.c derives that row as \`flows + cands > 0\`, so ` +
                        `this census contradicts its own composer and the cold round trip cannot be read ` +
                        `off it.`);
      const dark = arms.filter((a) => a[1] === 0);
      parts.push(`cold arms: ` + arms.map((a) => `${a[1]} ${a[0]}`).join(", ") +
                 (dark.length === 0
                   ? ` — every arm of the read half ran`
                   : ` — NOT exercised, so unproven this session: ` +
                     dark.map((a) => `${a[0]} (${a[2]})`).join("; ")));
    }
  }
  const f = [];
  for (const m of out.matchAll(/^@FORKAT (\{.*\})$/gm)) { try { f.push(JSON.parse(m[1])); } catch { /* tail */ } }
  if (f.length) parts.push(forkReading(f[f.length - 1]));
  /* AND WHAT THE @S HALF MET, WHICH IS THE ONE THING AN EMPTY SECURITY SURFACE CANNOT SAY ABOUT ITSELF. The
     probe rows above say whether a sink FIRED; `@S []` has four readings that take opposite work — the page
     never read an attacker source, a source was read but no sink RAN, a sink ran and only the page's own
     strings arrived, or something tainted arrived and the search was SUPPRESSED because the check on it was
     unforgeable — and the last of those is the engine's STRONGEST negative result rendered as the same
     nothing as never having looked. test_forced.c's own note records the measurement that forced the line
     into existence: a full-budget run emitted 349 samples of `@S []` and 349 @WFQ lines reading `cands: 0`,
     and neither said whether a sink had run at all. It had not — the script holding all four of them ended on
     an uncaught throw more than a thousand statements earlier — and `_sinkReached: 0` is the one number that
     says so. It had no reader; this is it, and it sits beside the @PAGEERR count that names the throw. */
  const sc = oneCensus(out, "@SCENSUS", SCENSUS_FIELDS);
  if (sc)
    parts.push(`@S arrivals: ${sc._sourceReads} attacker-source read(s), ${sc._sinkReached} sink(s) reached, ` +
               `${sc._sinkTainted} with tainted input, ${sc._sinkSuppressed} search(es) declined as ` +
               `unforgeable` +
               (sc._sourceReads === 0
                 ? ` — ZERO source reads, so an empty @S surface here says nothing about the page: no attacker `
                   + `input was ever acquired, which is a driving gap and not a finding`
                 : sc._sinkReached === 0
                   ? ` — sources were read and NO sink ever ran, so an empty @S surface is a reach problem `
                     + `rather than a page with nothing to find`
                   : ``));
  /* AND THE SAME FOR THE HEADLINE SURFACE, WHICH IS THE @S CENSUS'S ORPHAN-SIDE TWIN AND WAS THE HALF WITH NO
     READER AT ALL. §What-the-tool-produces makes orphan-invoke the proposition ("a sniffer shows what FIRED;
     this shows what the bundle CAN do but didn't"), and solver/engine.h's census is the pair that says whether
     this engine ever got to it: `asked` is how many times a flow ran out of work and put the question,
     `driven` is how many bodies it took. test_forced.c printed them at every sample and NOTHING in this tree
     matched the marker — a computed writer consumed by nobody, which §A-FIELD-A-CONSUMER-DEFAULTS calls the
     mirror of the read-with-no-writer defect and harder to see, because the value is real and asserted.

     WHAT THE SERIES ANSWERS THAT A FINAL PAIR CANNOT, WHICH IS THE WHOLE REASON IT IS READ AS A SERIES:
       1. ON THE RUNS THAT MATTER THERE IS NO FINAL PAIR. The document that carries these two numbers is
          rendered by `result_json` and printed after run_scheduler RETURNS, so a run whose frontier does not
          drain is killed by this file's own backstop before it gets there. On exactly the run where the orphan
          half is stuck, the pair is computed at every sample and printed zero times. The stream survives the
          kill; the document does not. §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES: an ABSENT result and a zero
          result are different facts and must never be averaged.
       2. A TAKE THAT STOPPED READS IDENTICALLY TO ONE THAT WORKED. `asked 400, driven 12` at the end is either
          a mechanism that ran throughout or one that drove twelve bodies early and has driven NOTHING since
          while the asks kept climbing. Those take opposite work and one pair cannot separate them. Only two
          samples separated in time say which.

     A CORRECTION TO THE WRITER'S OWN COMMENT, RECORDED HERE BECAUSE THAT FILE WAS BEING EDITED BY ANOTHER LANE
     WHEN THIS LANDED. test_forced.c says of the pair "their ONLY reader was the result document", and that is
     FALSE: `probes_report` in the same file reads `engine_orphan_census` too and renders the three-state
     `orphan_why` sentence out of it, so the classification NOT-ASKED / ASKED-AND-DROVE-NOTHING / DROVE-N was
     already reachable in-process. What was true is the part that matters and is why this reader exists anyway
     — `orphan_why` classifies ONE INSTANT, so it cannot see state 2 above, and it is composed only where the
     fixture's own orphan probe rows exist, while the marker is printed on every sample of every document. The
     over-claim is worth correcting rather than repeating: a reader who checked it would find the second
     consumer, disbelieve the whole note, and miss the real gap it names.

     `driven > asked` IS LEGAL AND IS NOT FLAGGED — the obvious invariant here is FALSE, and it is written down
     because the next reader will reach for it. solver/engine.c raises `driven` at TWO sites: a fresh take
     (after the ask), and a cold-tier RESUMED drive building its call frame, which returns before the ask is
     ever counted. A session that inherits drives from a residue therefore raises `driven` with no `asked`
     beside it, and an assert ordering the two would fire on the round trip working. */
  const ocs = censusSessions(out, "@OCENSUS", OCENSUS_FIELDS,
                             "test_forced.c's fixture_have_answers printf", OCENSUS_FIELDS);
  if (ocs) {
    const run = ocs[ocs.length - 1], first = run[0], last = run[run.length - 1];
    const dAsk = last._orphansAsked - first._orphansAsked, dDrv = last._orphansDriven - first._orphansDriven;
    /* THE FRONTIER'S OWN MOVEMENT, WHICH IS WHAT MAKES `asked == 0` A VERDICT RATHER THAN A SHRUG. A session
       that never reached the question and a session that barely ran are the same zero, and @COLD's `live` is
       the number that splits them. It is read as a POSITIVE statement and never defaulted: with no @COLD line
       there is no frontier reading to cite, and the sentence says that instead of implying a still frontier. */
    const liveMoved = c ? c.b.live - c.a.live : null;
    parts.push(`orphan drive: ${last._orphansAsked} ask(s), ${last._orphansDriven} body(ies) driven` +
      (ocs.length > 1 ? ` — read off the LAST of ${ocs.length} sessions on this stdout, since solver/engine.c `
                      + `releases both counters with the agent and last-minus-first across a restart is a `
                      + `difference between two different sessions` : ``) +
      (run.length < 2
        ? `; ONE sample, so this run states a pair and not a series — whether the take is moving is the `
          + `question this reader exists to answer and a single sample cannot answer it`
        : `; over ${run.length} samples asked +${dAsk}, driven +${dDrv}` +
          (last._orphansAsked === 0
            ? ` — NEVER ASKED. engine_orphan_seed is reached only where a flow has no program, job, lifecycle `
              + `event, timer, rendering opportunity, outstanding reply or unmodelled close request left, so `
              + `no flow of this session has `
              + `run out of its own work yet. That is the SCHEDULE and says nothing whatever about the take, `
              + `the drive, or whether this bundle ships uncalled code`
              + (liveMoved === null
                  ? ` (no @COLD line in this run, so there is no frontier reading to say whether it was moving)`
                  : liveMoved > 0
                    ? ` — and the frontier GREW by ${liveMoved} live member(s) across the same span, so the `
                      + `session was running and still never reached the question`
                    : ` — and @COLD's live count did not grow either, so this may be a session that barely ran `
                      + `rather than one that ran and never asked`)
            : dAsk === 0 && dDrv === 0
              ? ` — both counters FLAT across the whole span while the session went on sampling: the frontier `
                + `has stopped reaching the question, which is neither the take nor the page`
              : dDrv === 0
                ? ` — asked ${dAsk} more time(s) and drove NOTHING across the span. That is the TAKE` +
                  (last._orphansDriven === 0
                    ? `: JS_OrphanTakeOne's entered/is_program/bytecode filter, or the orphan-generation memo `
                      + `answering for a heap that has moved. It is not the schedule — the frontier is asking`
                    : `, and it is the state a final pair cannot report: ${last._orphansDriven} bodies WERE `
                      + `driven earlier in this session and none in the ${run.length} samples since, so the `
                      + `take has stopped rather than never started`)
                : ` — both rising, so the mechanism is working end to end this session`)));
  }
  return parts.length ? parts.join("; ")
                      : "no @HEAP/@SWAP/@COLD/@FORKAT/@SCENSUS/@OCENSUS census in this run — a stage that " +
                        "drives no scheduler prints none of them, so this is the absence of the signal and " +
                        "not a reading of the run";
}

/* WHAT THIS RUN'S NUMBERS ARE DENOMINATED IN, READ FROM THE ENGINE'S OWN STATEMENT OF IT — solver/quantum.c's
   `@QUANTUM` line, written once per instance at its first slice by the component that owns the fact
   (`quantum_measure`/`quantum_measure_is_cpu`). It is not a reading of the run and does not vary within one:
   it is a property of the HOST, and it is here because every OTHER number this file prints is a reading of a
   frontier whose ORDER that property decides.
   WHY IT MATTERS TO A READER OF THIS FILE'S VERDICTS. On a host with no CPU clock (the wasm one — emscripten
   answers CLOCK_MONOTONIC and both CPUTIME clocks from one `emscripten_get_now()`) the slice AND
   solver/engine.c's `flow_age_running` charge are denominated in WALL TIME. The charge is a comparison BETWEEN
   flows, so a descheduling the OS chose lands on whichever flow was running, moves ITS rank alone, and changes
   which flow is picked next — two runs of ONE artifact over ONE document then take different frontier orders
   and their census series differ with nothing about the tree differing. Every discriminator below compares
   census samples, so without this line a reader cannot tell a frontier difference from an artefact of slicing,
   and this file's own history says what that costs: it records two consecutive revisions with the SAME probe
   standing answering `a HEALTHY FRONTIER` and `a STALL`.
   IT IS LEGIBILITY AND NOT A FIX. §NO BOUNDS and §scheduler's razor forbid both cures — removing the quantum
   is a drive-to-completion, bounding the slice in steps is a cap — so the variance stays and is NAMED.
   THE CONTRACT IS ONE IMPLICATION AND IT IS CHECKED RATHER THAN DEFAULTED. A run that printed `@COLD` drove
   `run_scheduler`, whose census is printed only AFTER an `engine_sched_step`, and every step is bracketed by
   `quantum_begin`/`quantum_end` — so `@COLD` present with no `@QUANTUM` is a BROKEN CONTRACT and throws, in
   the same way and for the same reason a renamed @COLD row does. The converse is not a contract: a
   stage that opens slices and drives no scheduler (the two-instance ABI drive) prints this line and no census,
   and a stage that declines the edge entirely prints neither. ABSENCE IS REPORTED AS ABSENCE — never as a
   default, and never as the positive claim that a run was CPU-denominated.
   AND THE PAYLOAD IS THE SAME BYTES THE SHIPPED PATH READS, WHICH IS WHY THE GRAMMAR IS JSON AND NOT A BESPOKE
   `cpu=… slice=…ms measure=…`. Those three facts now ride solver/result.c's result document as `_quantum`,
   where extension/bridge.js asserts them and the popup renders them beside the frontier order — the surface a
   person actually compares two runs on. One composer (solver/quantum.c's `quantum_json`) emits to both, so
   this reader and that one parse ONE format string; the previous spelling was a second hand-serialization of
   the same three facts, which is exactly how `svc_min` came to be computed on every census and printed by
   nobody. `^@QUANTUM \{…\}` is also the grammar `lastTwo` already reads for @COLD/@HEAP/@SWAP, so there is one
   line shape on this seam rather than a marker only this function speaks.
   THE FIELDS ARE ASKED FOR BY NAME AND TYPE RATHER THAN DESTRUCTURED, for the reason every list in this file
   is checked against the thing it describes: a renamed or retyped field must fail HERE, not become an
   `undefined` that reads as a run with no denomination — which is the one reading this whole function exists
   to make impossible. */
function quantumDenomination(out) {
  const rows = [...out.matchAll(/^@QUANTUM (\{.*\})$/gm)].map((m) => {
    let j;
    try { j = JSON.parse(m[1]); } catch (e) {
      throw new Error(`[build] an @QUANTUM line is not JSON (${e.message}): ${m[1]}. solver/quantum.c's ` +
                      `quantum_json composes it and asserts its own buffer, so a malformed one is that ` +
                      `composer truncated or its measure string carrying a character it does not escape.`);
    }
    if (typeof j.isCpu !== "boolean" || typeof j.sliceMs !== "number" || typeof j.measure !== "string")
      throw new Error(`[build] an @QUANTUM line is missing a field of {measure:string, isCpu:boolean, ` +
                      `sliceMs:number}: ${m[1]}. That object is solver/quantum.c's quantum_json in full and ` +
                      `is the SAME shape extension/bridge.js asserts on the result document's \`_quantum\`, ` +
                      `so a rename here is a rename there — fix the composer or both readers, never one.`);
    return { cpu: j.isCpu, sliceMs: j.sliceMs, measure: j.measure.trim() };
  });
  if (!rows.length) {
    if (/^@COLD \{/m.test(out))
      throw new Error("[build] this run printed the @COLD frontier census and no @QUANTUM line. Every " +
                      "`engine_sched_step` is bracketed by solver/quantum.c's slice and the first " +
                      "`quantum_begin` announces what that slice is measured in, so a run that reached a " +
                      "census reached the announce — a missing line is that writer having been removed, " +
                      "renamed or compiled out from under this reader, and every verdict below would then " +
                      "read as CPU-denominated by default.");
    return null;
  }
  /* TWO INSTANCES OF ONE BINARY MUST AGREE, and the ABI drive provisions exactly that pair, so this is a
     reading rather than a hypothetical. A disagreement is not a number to average: it is one program whose two
     engines are being scheduled in two different currencies, which no verdict here could be about. */
  for (const r of rows)
    if (r.cpu !== rows[0].cpu || r.measure !== rows[0].measure || r.sliceMs !== rows[0].sliceMs)
      throw new Error(`[build] two @QUANTUM lines in one run disagree — ${JSON.stringify(rows[0])} against ` +
                      `${JSON.stringify(r)}. They are the same binary on the same host, so this is not a ` +
                      `measurement that varies; it is the announce reading something that is not a property ` +
                      `of the host.`);
  return { ...rows[0], instances: rows.length };
}
/* THE SAME FACT AT EVERY OUTCOME, PASS INCLUDED, because the runs a reader compares are the ones that finished
   — a caveat printed only on the failures is a caveat absent from precisely the comparison it is about.
   AND THE SAME INSTRUCTION AT EVERY HOST, WHICH IS THAT SENTENCE APPLIED ONE LEVEL IN AND IS THE CORRECTION
   THIS BLOCK CARRIES. `Compare two runs of one revision` used to be printed ONLY on the non-CPU arm, and the
   CPU arm said the opposite in as many words: "that is real thread CPU, so this run's census series is
   invariant to what else this box was doing". THAT CLAIM IS FALSE, and it is false in the direction that
   costs — it licenses reading a SINGLE native run as a measurement, which is the exact reading §Testing calls
   an artifact of HOW a run ran reported as a fact about WHAT ran.
   WHAT A CPU CLOCK ACTUALLY BUYS, stated at the strength the mechanism supports. `flow_age_running` bills
   `quantum_thread_us()` (solver/engine.c), which natively is CLOCK_THREAD_CPUTIME_ID — so every microsecond
   charged is one the flow HELD THE THREAD FOR, and no flow is demoted for time the OS spent elsewhere. That is
   a statement about WHOSE BILL a charge lands on, and it is the whole of the difference between the two arms.
   WHAT IT DOES NOT BUY IS A REPRODUCIBLE ORDER, because the SLICE is denominated in that same CPU and the work
   a flow completes inside one CPU-microsecond is not a constant of the program: a contended box costs the same
   opcode sequence more thread CPU (stall cycles are on-CPU time), so the quantum's timer fires at a DIFFERENT
   opcode and the suspend point moves. It reaches the order a second way with no timer in it at all:
   solver/flow.h's `flow_silence_notch` is a FLOOR over whole quanta of accumulated thread time and is a term
   of `flow_weight`, so the step at which a flow crosses a notch — and therefore the pick that follows —
   depends on what a microsecond bought. Both are load-sensitive on a host with a perfect CPU clock.
   SO THE TWO ARMS DIFFER IN THE KIND OF ERROR, NOT IN WHETHER THERE IS ONE, and the instruction is
   unconditional. §NO BOUNDS forbids the cures that would make it conditional (drop the quantum and it is a
   drive-to-completion; denominate the slice in steps or work and it is a cap), so what is owed a reader is the
   truth about the instrument, not a number that stops moving. */
const quantumText = (q) =>
  q === null
    ? `[build]   no @QUANTUM line — this stage opened no engine slice, so it has no scheduler denomination ` +
      `rather than an unstated one`
    : `[build]   the engine's slice (${q.sliceMs} ms) and the WFQ's aging charge are denominated in ` +
      `${q.measure}` + (q.instances > 1 ? ` — ${q.instances} instances, all agreeing` : ``) +
      (q.cpu
        ? `\n[build]   that is real thread CPU, so every microsecond the aging charge bills is one the flow ` +
          `HELD THE THREAD for — no flow is demoted for time the OS spent elsewhere. It does NOT make the ` +
          `order reproducible: the slice is that same CPU, and a loaded box costs one opcode sequence more ` +
          `thread CPU, so the quantum fires at a different opcode and flow_silence_notch's floor is crossed ` +
          `at a different step.`
        : `\n[build]   THAT IS NOT CPU: this run's census series is a reading of ONE INTERLEAVING. The aging ` +
          `charge bills wall time to whichever flow the OS happened to leave running, so a descheduling ` +
          `nothing in the program chose lands on one flow's rank alone — a second and larger source of ` +
          `variance on top of the one the CPU arm has.`) +
      `\n[build]   EITHER WAY: compare two runs of ONE revision before reading a difference between two ` +
      `revisions. The frontier ORDER — and therefore every census below it — moves run to run on one artifact ` +
      `on BOTH hosts; only the reason differs.`;

/* A DIAGNOSIS DERIVED FROM A SIGNAL THE SUBJECT DOES NOT EMIT IS A CLAIM ABOUT THE INSTRUMENT, NOT THE RUN.
   This discriminator reads @COLD, which only a stage that drives a scheduler prints. Applied to a stage that
   never prints one -- the two-instance ABI drive -- it concluded "has not reached engine_sched_begin's first
   census at all" about a run that had demonstrably done cross-agent reads and delivered five messages to its
   peer. That is the same shape as counting a marker no shipped path writes: the zero was a property of the
   question, and it read as a confident verdict about the subject. So the absence of the signal is now reported
   AS an absence of the signal, and the tail -- which is what a reader actually has -- is named as the evidence
   instead. `nothing has been established` is a true statement; `it never started` was not. */
function hungCauseCensus(out) {
  const s = [];
  for (const m of out.matchAll(/^@COLD (\{.*\})$/gm)) { try { s.push(JSON.parse(m[1])); } catch { /* truncated tail */ } }
  if (s.length === 0) {
    const lines = out.split("\n").filter(l => l.trim().length);
    return `NOT ESTABLISHED — this stage printed no @COLD census, so this discriminator has nothing to read ` +
           `and says nothing about why it ended without answering. A stage that drives no scheduler never ` +
           `prints one, and for ` +
           `that stage the absence is expected rather than a finding. The evidence is the tail: ` +
           `${lines.length} line(s), last was ${JSON.stringify((lines[lines.length - 1] || "").slice(0, 120))}.`;
  }
  if (s.length < 2) return `only ${s.length} @COLD census line(s) — too few to say why. The run reached ` +
                           `engine_sched_begin's first census and then stopped producing them.`;
  /* THE WINDOW IS AN ABSOLUTE SPAN OF THE ENGINE'S OWN WORK, NEVER A FRACTION OF THE RUN — and the fraction is
     the defect this whole function was quoted for. It read `a = s[floor((n-1)/2)]`, so the LEFT EDGE of the
     comparison was set by where the CPU budget happened to run out: a run that reached 60 censuses asked about
     work-points 30..60 and a run of the SAME trajectory that reached 200 asked about 100..200. Those are
     DISJOINT stretches of one run, so a frontier that retires its boot flows early and then climbs into a long
     search answers HEALTHY through the first window and STALL through the second — with nothing whatever
     different about the engine. MEASURED, and it is what sent this file's own verdict to be rewritten: two
     consecutive revisions, the SAME 95/159 probe standing, and the verdicts `a HEALTHY FRONTIER THAT WANTED
     MORE BUDGET` and `a STALL` — the STALL on the revision that had just made the reply path 3.2x FASTER, i.e.
     on the run that got FURTHER. A verdict a faster build flips is a verdict about the budget, not the tree.
     SO THE WIDTH IS FIXED AND THE RIGHT EDGE FLOATS. Two runs that both end in the same phase then read the
     same width of that phase and answer the same way, whatever extent each reached; only a run that stops
     exactly on a phase boundary can differ, and the landmarks below say when that happened.
     IT IS A GRANULARITY AND NOT A BOUND (§NO BOUNDS): it truncates nothing, drops no flow, and decides only how
     much of the tail this reader looks at. So it is a POLICY INPUT stated here with its reasoning and PRINTED
     with the verdict, rather than a constant hidden in an expression. The width is in CENSUSES because that is
     what the stream is made of, and one census is ENGINE_PROGRESS_EVERY units of engine_work_done — read from
     engine.c so a retuned cadence cannot leave this sentence quoting a span the engine stopped taking.
     WHY TWENTY: the window has to be longer than one WFQ RE-RANKING PAUSE, which is this function's own
     standing reason for not comparing adjacent samples — a flow outranked and another picked costs a handful of
     context switches, and twenty censuses is twenty thousand units of forks+flows+jobs+switches, four orders of
     magnitude above that. It is clamped to half the run so a SHORT run never reads a window longer than it has
     evidence for, which is exactly the old behaviour and is therefore the floor rather than a new risk. */
  const PROGRESS_EVERY = hostDefine("solver/engine.c", "ENGINE_PROGRESS_EVERY",
    "hungCause states its window as an absolute span of engine_work_done and the census cadence is what " +
    "converts censuses to work units");
  const HUNG_WINDOW_CENSUSES = 20;
  const n = s.length;
  const width = Math.min(HUNG_WINDOW_CENSUSES, Math.max(1, Math.floor(n / 2)));
  const b = s[n - 1], a = s[n - 1 - width];
  /* THE FIELD CONTRACT THROUGH ITS ONE IMPLEMENTATION, which is what `censusFields`' own banner asks for and
     what this loop was the fourth hand-written copy of. It also carried the wrong composer: it named
     "engine.c's printf" for a document solver/result.c's `result_cold_json` composes, so a reader who hit it
     was sent to a file that has never emitted these rows. */
  for (const c of [a, b]) censusFields(c, "@COLD", coldFields(), "solver/result.c's result_cold_json");
  /* AND THE PARTITIONS HOLD AT BOTH ENDS OF THE WINDOW, checked before anything is composed out of either.
     Every arm below reads a DIFFERENCE across the window, so a census whose parts do not sum is one whose
     difference is a difference of nothing. */
  for (const c of [a, b]) {
    coldPartition(c, "finished", ["finishedFlows", "finishedCands"], "engine_frontier_census");
    coldPartition(c, "sold", ["soldFlows", "soldCands"], "engine_frontier_census");
  }
  /* WHERE THE MONOTONE COUNTERS LAST MOVED, OVER THE WHOLE RUN — the numbers that make two runs comparable and
     which nothing here has ever computed. `finished` and `sold` only ever climb, so "the last census at which
     this one climbed" is a fact about the run's TRAJECTORY rather than about the instant it stopped: two runs of
     one revision that both get past a plateau report the SAME landmark and differ only in how far past it they
     went. NEVER-MOVED is a stronger statement than silent-across-the-window and is reported as its own word,
     because "this run retired nothing at all" and "this run stopped retiring" take different work. They ride the
     verdict's NAME rather than its parenthetical, which is where this function's own history says every number
     it computed went to die: `causeName` cuts at the first " (" or " — ", so a landmark behind either is a
     number the one line anybody reads does not carry. */
  const lastRise = (key) => { let i = -1; for (let k = 1; k < n; k++) if (s[k][key] > s[k - 1][key]) i = k; return i; };
  const lastRetire = lastRise("finished"), lastSale = lastRise("sold");
  /* AND THE SAME LANDMARK PER POPULATION, which is the one the total structurally cannot carry. A frontier
     whose members are mostly @S candidate sessions has `finished` climbing for the whole run while not one
     exploration flow has ever reached its end — so `retire never` is unreachable on such a document and the
     total's landmark says nothing about coverage. These two say which half moved and when. */
  const lastRetireFlow = lastRise("finishedFlows"), lastRetireCand = lastRise("finishedCands");
  const mark = (i, tot) => (i < 0 ? "never" : `${i}/${tot}`);

  /* THE FIXTURE'S OWN PROGRESS, over a window of the SAME absolute shape — its own stream and its own cadence
     (test_forced.c's PROBE_SAMPLE_EVERY), so it is counted in ITS samples and clamped the same way. */
  const h = probeFlips(out);
  const hwidth = Math.min(HUNG_WINDOW_CENSUSES, Math.max(1, Math.floor(h.length / 2)));
  const hb = h[h.length - 1], ha = h[h.length - 1 - hwidth];
  const flipped = h.length >= 2 ? Object.keys(hb).filter((k) => hb[k] && !ha[k]) : [];
  const zero = h.length ? Object.keys(hb).filter((k) => !hb[k]) : [];
  let lastFlip = -1;
  for (let k = 1; k < h.length; k++)
    if (Object.keys(h[k]).some((x) => h[k][x] && !h[k - 1][x])) lastFlip = k;
  /* THE COMPACT FORM THAT SURVIVES INTO THE STAGE TABLE. Semicolon-and-comma only: `causeName` ends a name at
     the first " (" or " — ", so these separators are chosen to be neither. */
  /* THE RETIRE LANDMARK IS THE SPLIT ONE AND NOT THE TOTAL'S, which is the whole of what this row is for: on a
     frontier that is mostly candidate sessions the total's landmark moves every census whatever the
     exploration is doing, so it reads healthy for a run that has retired no exploration flow at all. The
     total's own landmark stays in `span` beside the counts it belongs to. */
  const landmarks = `; retire ${mark(lastRetireFlow, n)} flow, ${mark(lastRetireCand, n)} cand, ` +
                    `flip ${mark(lastFlip, h.length)}`;
  const span = `over the last ${width} of ${n} censuses — an ABSOLUTE window of ${width * PROGRESS_EVERY} ` +
               `units of engine_work_done, not a fraction of the run: ` +
               `finished ${a.finished}→${b.finished} (exploration ${a.finishedFlows}→${b.finishedFlows}, ` +
               `@S candidate sessions ${a.finishedCands}→${b.finishedCands}), live ${a.live}→${b.live}, ` +
               `sold ${a.sold}→${b.sold} (exploration ${a.soldFlows}→${b.soldFlows}, candidate ` +
               `${a.soldCands}→${b.soldCands}), blocked ${b.blocked}, owed ${b.owed}. ` +
               `Over the WHOLE run, finished last rose at census ${mark(lastRetire, n)} — its exploration half ` +
               `at ${mark(lastRetireFlow, n)} and its candidate half at ${mark(lastRetireCand, n)} — and sold at ` +
               `${mark(lastSale, n)}` +
               (h.length ? `, and the probe table last flipped a row at sample ${mark(lastFlip, h.length)}` : ``) +
               ` — those are the landmarks two runs of one revision are compared on, and `+
               `blocked/owed/live above are readings of the LAST census alone; ` +
               /* AND WHAT THE MEMBERS THAT DID NOT RETIRE WERE DOING, on EVERY arm rather than on the one that
                  happens to name a cause. `span` is where it goes for that reason: the arms below disagree
                  about why the run ended and none of them disagrees about this, so putting it in one of them
                  would make the answer depend on the verdict it is supposed to inform.
                  BOTH HISTOGRAMS, because one of them is a gauge and one is a lifetime count and a rung is
                  named by the PAIR. `stepUnitReading` says where the frontier is standing at the last census;
                  `stepUnitRunReading` says which rungs the ladder has ever run at all. An arm reading 0 in the
                  first is either of two opposite things and the second is what says which.
                  AND THE THIRD IS A DIFFERENT AXIS ENTIRELY, which is why it is spliced here and not folded
                  into either. Those two say WHICH ARM the members that are not retiring are in; this one says
                  WHERE IN THE DOCUMENT they are standing, and the pair `deepest`/`completed` above — being
                  maxima — cannot answer it at any value. A frontier every member of which is in
                  `resume-program` reads identically whether its mass is at program 3 with one member at 11 or
                  every member is at 11, and only this row separates them.
                  AND THE FOURTH IS A DIFFERENT AXIS AGAIN AND IS THE ONE THE OTHER THREE PRESUPPOSE. All
                  three above are about WHERE the steps a run took went; `stepCostReading` is about how many
                  there could have been, which is the question a frontier that grows and does not retire
                  actually raises — "the tail is not being reached" is a THROUGHPUT statement, and until this
                  row nothing said where the throughput went. It is handed the `@QUANTUM` denomination
                  because its yardstick is the slice THAT RUN was scheduled on and because the measure decides
                  which readings of the number are available; both are facts of the run and neither is this
                  tree's to supply. */
               stepUnitReading(b) + "; " + stepUnitRunReading(b) + "; " +
               stepCostReading(a, b, quantumDenomination(out)) + "; " + programCursorReading(b);
  /* AND WHICH OF THE STILL-0 ROWS WERE EVER ANYTHING ELSE, which is the distinction `flipped.length === 0`
     cannot draw and which decides what "still advancing" is worth. Measured across six builds: the rows that
     reached 1 in the last window were, every time, the ten members of ONE family (the @S search rows), while
     forty-two others had been 0 in EVERY sample since the first — so "the probe table was still advancing"
     was true of one corner and false of the document, and it was the sentence the verdict carried. A row that
     is late and a row nothing has ever approached take opposite work, and the count that separates them is
     one pass over the samples this function already holds. */
  const everOne = new Set();
  for (const r of h) for (const k of Object.keys(r)) if (r[k]) everOne.add(k);
  const neverOne = zero.filter((k) => !everOne.has(k));
  const hspan = h.length < 2
    ? `no @H stream to read`
    : `@H: ${flipped.length} row(s) reached 1 across the last ${hwidth} of ` +
      `${h.length} samples` + (flipped.length ? ` (${flipped.join(" ")})` : "") +
      `, ${zero.length} still 0` + (zero.length ? ` (${zero.join(" ")})` : "") +
      `, ${neverOne.length} of them 0 in every one of the ${h.length} samples`;

  const wfq = wfqReading(out);
  /* AND THE THREE STREAMS THE VERDICT USED TO END WITHOUT. Every arm below carries it, including the two that
     name a cause: a healthy-but-short run whose arena is climbing and a healthy-but-short run whose arena is
     flat want different next steps, and "wanted more budget" is the same sentence for both. */
  const cs = censusReading(out);
  /* WHAT THE ARMS BELOW ARE ALLOWED TO DECIDE ON, AND WHY IT IS NOT WHAT THEY USED TO DECIDE ON.
     `finished` and `sold` are CUMULATIVE COUNTERS, so "did this one climb across the window" is a statement
     about work the run PERFORMED. `live`, `blocked` and `owed` are GAUGES, and a gauge read at the last census
     is a reading of the microsecond the kernel's SIGXCPU happened to land in. Three of this function's five
     arms turned on a gauge, and each one was a way for two runs of one revision to disagree:
       - HEALTHY and WORK-THAT-ADVANCES-NO-STATEMENT both required `blocked === 0 && owed === 0` AT THE LAST
         CENSUS. A run retiring flows steadily across twenty thousand units of work was thrown out of both arms
         — and, because arms 3 and 4 require `finished` to be FLAT, straight past them into `NEITHER named
         cause` — because one flow happened to be mid-fetch when the last census was taken. That is a run whose
         cause this function HAD and reported as unnameable.
       - STALL and PAGED-OUT both required `b.live > a.live`. A frontier that retired nothing and paged nothing
         is a stall whether or not its live count happened to be higher at one instant than at another.
     So the gauges are REPORTED at every arm and DECIDE only the one thing they are the sole evidence for: the
     unmodelled arm, where the live count fell while nothing retired and nothing was sold, which is the
     counters disagreeing and is worth saying rather than inferring past. And `blocked`/`owed` get an arm of
     their OWN — a frontier parked on the host is a cause this function could see and had no word for. */
  const retiring = b.finished > a.finished;
  /* AND WHICH POPULATION RETIRED, WHICH IS WHAT BOTH ARMS BELOW USED TO ASSERT WITHOUT HAVING. They said
     "flows retired steadily" and "flows were still finishing" off a counter that sums exploration flows and @S
     candidate sessions — and on a frontier that is mostly candidates, both sentences were true of a run in
     which not one exploration flow had ever reached its end. Those are opposite findings: a retiring
     exploration frontier is gaining coverage and legitimately wants more budget, while a frontier retiring
     only candidates is the SEARCH discarding derived payloads that did not fire, which buys coverage nothing
     and which more budget buys more of.
     IT RIDES THE VERDICT'S NAME IN THE CANDIDATE-ONLY CASE, with a comma rather than " — " or " (" because
     `causeName` ends a name at either of those and the one line anybody reads is the name. Calling that run
     "a HEALTHY FRONTIER" without the qualifier is the headline the split exists to stop. The ROUTING is
     unchanged: retiring candidates IS retiring work, so the arms are the same arms and it is the claim they
     make that got narrower. */
  const retFlows = b.finishedFlows - a.finishedFlows, retCands = b.finishedCands - a.finishedCands;
  const whoRetired =
    retFlows === 0
      ? `every one of the ${retCands} retirement(s) across this window was an @S CANDIDATE SESSION and not ` +
        `one exploration flow reached its end, so what moved is the search discarding derived payloads that ` +
        `did not fire — that gains this document no coverage, and more budget buys more discards`
      : retCands === 0
        ? `all ${retFlows} of them were exploration flows, so no candidate re-fire finished in this window`
        : `${retFlows} exploration flow(s) and ${retCands} @S candidate session(s) — coverage gained and ` +
          `search spent, which are different things and are worth different budget`;
  const candOnly = retiring && retFlows === 0 ? `, @S CANDIDATE DISCARD ONLY` : ``;
  if (retiring && h.length >= 2 && flipped.length === 0)
    return `WORK THAT ADVANCES NO STATEMENT${candOnly}${landmarks} (${span}; ${hspan}; ${wfq.text}; ${cs}) — members retired steadily ` +
           `(${whoRetired}) and not ` +
           `one probe row reached 1 across the window, so more time buys more of the same. ` +
           (wfq.ordered
             ? `The reward spread is wider than the optimism term's whole range, so the ORDER is the reward's ` +
               `and no other term can reorder its ends — and ${wfq.whose}, so the bottom of it is reached only ` +
               `as the aging term gives back about one point per second of unproductive thread time, per ` +
               `member ahead of them.`
             : `The @WFQ census does NOT show a reward-ordered frontier — its ends are within one optimism ` +
               `bonus of each other — so the reward term is not what is holding this run. What IS ordering it ` +
               `is the ${wfq.orderedBy} term, read from the ranges above rather than from the magnitudes: a ` +
               `term whose two ends read the same cannot separate two members however many points deep it is. ` +
               `The census above says whether that is the ONE-FAMILY case, where the aging term's family half ` +
               `is structurally an offset, or a level moment in a frontier it does order — this line used to ` +
               `name the first of those without reading the row that tells them apart.`) +
           ` The rows still 0 name what nothing scheduled was working toward.`;
  if (retiring)
    return `a HEALTHY FRONTIER THAT WANTED MORE BUDGET${candOnly}${landmarks} (${span}; ${hspan}; ${wfq.text}; ${cs}) — members were still ` +
           `retiring when the budget ran out (${whoRetired}), ${b.blocked} flow(s) were blocked and ` +
           `${b.owed} reply(s) owed at ` +
           `the last census, and ${flipped.length} row(s) ` +
           `reached 1 across the window. ` +
           (neverOne.length
             ? `THAT ADVANCE IS NOT THE WHOLE DOCUMENT: ${neverOne.length} row(s) have been 0 in every sample ` +
               `of this run, so more budget extends a search that is moving and does nothing for those — read ` +
               `them beside the @WFQ census before spending more CPU on this fixture.`
             : `AND EVERY ROW STILL 0 HAS BEEN 1 AT SOME SAMPLE, which is not a budget question at all — a ` +
               `probe row is a statement the result document makes, so one that goes 1→0 means the document ` +
               `stopped making it, and that is the finding rather than the frontier's pace.`);
  /* A STALL AND A PAGE-OUT ARE DIFFERENT VERDICTS ON THE SAME TWO NUMBERS, and `sold` is the whole of the
     difference. `finished` flat with `live` rising says work is being ADMITTED and not RETIRED — but a
     frontier under RAM pressure retires nothing and pages instead, which §Time-travel-resume calls the
     correct behaviour and not a defect, and it reads identically in `finished` and `live`. The row that says
     which is `sold`, and it could not be read here at all until the cold census carried it: it was a
     @PROGRESS name, and @PROGRESS was printed by this same unreachable loop. */
  /* THE FRONTIER PARKED ON THE HOST, which this function could always see and had no word for. `blocked` and
     `owed` are the two registers engine.h calls "what the host is owed", and a frontier that retires nothing
     while one of them is non-empty is not stalled on its own ordering at all — it is waiting for something
     only the host can supply, and the work to do is in the HOST rather than in the WFQ. It is asked only where
     nothing retired, because a run that IS retiring is not characterised by one parked flow. */
  if (b.blocked > 0 || b.owed > 0)
    return `WAITING ON THE HOST${landmarks} (${span}; ${hspan}; ${wfq.text}; ${cs}) — no flow finished across the ` +
           `window while ${b.blocked} flow(s) sat blocked on a host request and ${b.owed} reply(s) were owed. ` +
           `Those two are readings of the LAST census, so what they establish is that the frontier ended parked ` +
           `on the host — the question is which record was never answered, not why the ordering retires nothing.`;
  /* A STALL AND A PAGE-OUT ARE DIFFERENT VERDICTS ON THE SAME COUNTER, and `sold` is the whole of the
     difference. `finished` flat says work is not being RETIRED — but a frontier under RAM pressure retires
     nothing and pages instead, which §Time-travel-resume calls the correct behaviour and not a defect, and it
     reads identically in `finished`. The row that says which is `sold`, and it could not be read here at all
     until the cold census carried it: it was a @PROGRESS name, and @PROGRESS was printed by an unreachable
     loop. Both arms used to ALSO require `live` to have risen, which is a gauge comparison — see above. */
  if (b.sold > a.sold)
    return `a FRONTIER BEING PAGED OUT${landmarks} (${span}; ${hspan}; ${cs}) — no member finished across the ` +
           `window, but ${b.sold - a.sold} member(s) were SOLD to ` +
           `the cold tier in it — ${b.soldFlows - a.soldFlows} exploration flow(s) and ` +
           `${b.soldCands - a.soldCands} @S candidate session(s). That is the RAM floor doing its job, so the ` +
           `question is what the working set is made of rather than why nothing retires` +
           /* AND THE ONE ASYMMETRY BETWEEN THE TWO HALVES OF THAT SALE, which is why the split is worth having
              here rather than only in the total: an exploration flow's snapshot is a recipe and comes back as
              itself, while a candidate comes back WITHOUT the ladder it stood on — solver/flow.h keeps
              `cand_surv` and `cand_rung` off the cold tier deliberately, because a distance is an observation
              of a re-execution and a resumed session has not made it. So the same sale costs the two
              populations different things, and a pager reading one number cannot see which it just spent. */
           (b.soldCands > a.soldCands
             ? `. The candidate half of that sale costs the SEARCH a measured distance: a parked candidate is ` +
               `written out with its substitution and not its ladder (solver/flow.h), so those ` +
               `${b.soldCands - a.soldCands} re-enter the frontier having to re-earn the rungs they had.`
             : `.`);
  /* THE EVIDENCE HAS TWO AXES AND THIS HALF OF THE TABLE ASKED ONLY ONE OF THEM. Above, a RETIRING frontier is
     crossed against the probe stream — `WORK THAT ADVANCES NO STATEMENT` where nothing reached 1, a healthy
     frontier where something did. Below, a frontier that retires NOTHING went straight to `a STALL` however
     many statements the document had just made, because `flipped` was computed once and read on the retiring
     side only. One quadrant of the table was therefore missing, and it is the quadrant a FAIRLY ORDERED
     frontier lives in.
     RETIREMENT IS A FACT ABOUT IDENTITY AND EMISSION IS THE OUTPUT, which is why they may not be collapsed:
     §NO BOUNDS says "Only EMITTED OUTPUT — never identity — proves a flow is done", and `finished` is the
     other one. A member retires when its OWN path runs out of continuations, and under a fair order that
     happens only once every member ahead of it has advanced as far — so a breadth-first frontier retires in
     WAVES and reads 0 between them, while the document goes on answering statements throughout.
     MEASURED, in a smoke log that was already on disk when this was written and needed no run to produce:
     `finishedFlows` froze at 49 at census 11 of 182 and never moved again, while the @H stream first reached 1
     on `s-evalc`, `s-evalc-atsink`, `s-html-atsink`, `s-html`, `s-url-atsink`, `s-url`, `s-eval` and
     `s-eval-atsink` at samples 53, 73, 79, 84, 90 and 94 of 171 — eight statements, every one of them after
     the freeze, and every one of them an @S breakout, which is the highest-value thing this document
     produces. The two streams have different cadences and both span the whole run, so which came after which
     is not in question.
     WHAT THAT LOG DOES AND DOES NOT ESTABLISH, because the difference is the whole discipline here. It
     establishes that the two axes are INDEPENDENT — a frontier retired nothing for 170 censuses and answered
     eight statements inside them — and that is what makes reading one of them a verdict about half the table.
     It does NOT establish that this arm would have relabelled that run: the comparison is over the LAST
     `hwidth` samples, its last new statement landed at sample 94 of 171, and at HUNG_WINDOW_CENSUSES = 20 its
     final window flips nothing, so it is `a STALL` at its end under this reader and under the one before it,
     correctly. Whether a given run lands in this quadrant is a fact about its final window and is settled per
     run; what is settled here is that the quadrant exists and had nowhere to go. */
  if (b.live >= a.live && h.length >= 2 && flipped.length > 0)
    return `a FRONTIER ADVANCING WITHOUT RETIRING${landmarks} (${span}; ${hspan}; ${wfq.text}; ${cs}) — no ` +
           `member retired across the window, nothing was paged out and nothing was waiting on the host, so ` +
           `work is being ADMITTED and not retired — and ${flipped.length} probe row(s) reached 1 in that same ` +
           `window (${flipped.join(" ")}), so this document ADVANCED STATEMENTS while retiring nothing. Those ` +
           `two are independent and only the second is output: a member retires when its own path runs out of ` +
           `continuations, which under a fair order waits on every member ahead of it, so a breadth-first ` +
           `frontier retires in waves and reads 0 between them. ` +
           (lastRetire < 0
             ? `Nothing has retired in this run at all, and that is a reading of WHERE THE WAVE IS rather than ` +
               `evidence that a flow of this document cannot terminate — the terminating shape is the ` +
               `fall-through of solver/engine.c's flow_step, which no term of the order gates.`
             : `finished last rose at census ${lastRetire} of ${n}, so a wave has completed in this run and ` +
               `the ${n - 1 - lastRetire} census(es) since — ${(n - 1 - lastRetire) * PROGRESS_EVERY} units of ` +
               `engine work — are the run inside the next one.`) +
           ` The question this run poses is what the WORKING SET is made of, not why nothing retires. ` +
           `${wfq.whose}.`;
  if (b.live >= a.live)
    return `a STALL${landmarks} (${span}; ${hspan}; ${wfq.text}; ${cs}) — no flow finished across the window, ` +
           `nothing was paged out, nothing was waiting on the host and ` +
           (h.length < 2
             ? `this run printed no @H stream to read, so retirement is the ONLY axis there was evidence on ` +
               `and this verdict is made of one of the two: `
             : `not one probe row reached 1 in that window either, so BOTH axes are silent — this is the ` +
               `quadrant that is a stall rather than a wave: `) +
           `work is being admitted and not retired. ` +
           (lastRetire < 0
             ? `AND NOTHING HAS RETIRED IN THIS RUN AT ALL, which is the stronger statement: this is not a ` +
               `frontier that stopped retiring, it is one that never did.`
             : `finished last rose at census ${lastRetire} of ${n}, so the silence is the ` +
               `${n - 1 - lastRetire} census(es) — ${(n - 1 - lastRetire) * PROGRESS_EVERY} units of engine ` +
               `work — since then, and THAT is the number two runs of one revision must agree on.`) +
           /* AND WHOSE REWARD THE ADMITTED WORK IS RANKED ON, which is the one thing this arm can say about the
              ORDER and used to leave inside `wfq.text` for the reader to derive. A frontier that admits and
              does not retire is ranked on an account almost none of whose members filled — a fork JOINS its
              parent's family rather than copying its reward (flow.c's flow_fork_inherit), and a from-baseline
              newcomer arrives at the incumbent's (flow_arrive_at_virtual_time) — so this number is what
              separates "the members ahead are the ones that produced the findings" from "the members ahead
              produced nothing and are standing on an account some other arm filled", and the two take
              different work. It said every arm CARRIES its parent's `val`, which was the per-chain prefix
              flow.c retired; the reward is the fork family's and there is nothing for an arm to carry. */
           ` ${wfq.whose}.`;
  /* THE UNMODELLED ARM CARRIES THE MOST EVIDENCE, NOT THE LEAST, which is the inversion it used to be. It
     said "the two censuses above are the measurement to start from" and printed neither the heap, the delta
     chains nor the fork table — the three streams that model precisely the causes the frontier counts do not.
     An arm that admits it cannot name the cause is the one arm that owes the reader everything it has. */
  return `NEITHER named cause${landmarks} (${span}; ${hspan}; ${wfq.text}; ${cs}) — nothing retired across the ` +
         `window, nothing was sold to the cold tier, nothing was owed by the host, and the live count FELL ` +
         `(${a.live}→${b.live}) anyway. A member leaves the frontier by finishing or by being sold, so the ` +
         `three counters disagree about where ${a.live - b.live} of them went — that contradiction is the ` +
         `measurement to start from, and the streams above are the rest of what this run said.`;
}

/* …AND THE CAUSE CARRIES THE CURRENCY ITS EVIDENCE WAS TAKEN IN. Every arm above is a comparison of census
   SAMPLES, and on a host with no CPU clock which samples exist — and in which ORDER the frontier reached them
   — is decided by a wall-time aging charge, so the discriminator's own inputs move between two runs of one
   artifact. That does not make an arm wrong: a stall is a stall. It makes the DIFFERENCE between two runs
   unreadable, which is the comparison this file's verdicts are used for.
   THE MARK RIDES THE NAME, not the parenthetical, and that is deliberate: `causeName` ends a name at the first
   " (" or " — ", so this file's own record says a fact behind either is a fact the one line anybody reads does
   not carry. The separator is "; " for exactly the reason the landmarks use it — it is neither cut. The full
   sentence goes to the detail line, which `runNumbers` prints at EVERY outcome including the pass.
   ONLY WHERE IT IS FALSE, AND NARROWER THAN IT WAS. The argument that stood here — "a mark on every verdict
   is a mark that stops being read" — was written when this was the ONLY mark a verdict carried, and it has
   stopped being the standing rule: `runDependenceText` marks EVERY arm now, because the three readings that
   cost this project a session were all taken off the stage table while the unconditional instruction sat in a
   detail line beneath it. What that mark cannot say is what THIS one says, which is why this one stays and is
   not a second copy of it: every arm of `hungCauseCensus` is a comparison of census SAMPLES, so this names the
   currency the DIAGNOSIS's own evidence was taken in, where the verdict mark names what the line's TOTALS are
   a reading of. `cpu=1` is a positive statement that this narrower caveat does not apply — the wall-time
   charge landing on whichever flow the OS left running — and it is stated in the detail rather than in
   silence.
   AND IT IS NOT A STATEMENT THAT THE DIFFERENCE BETWEEN TWO RUNS IS READABLE, which is the reading this mark
   would otherwise invite by its own absence. A CPU-clocked host still slices in CPU, and the work a
   microsecond of it buys is a property of the machine — see quantumText and quantum_measure_is_cpu()'s
   declaration — so the samples this discriminator compares move between two runs of one artifact on BOTH
   hosts. What the mark separates is the SOURCE of that movement, which is what decides where to look; the
   instruction to take two runs of one revision is unconditional and is printed by quantumText at every
   outcome. */
function hungCause(out) {
  const cause = hungCauseCensus(out);
  const q = quantumDenomination(out);
  return q && !q.cpu ? `WALL-SLICED; ${cause}` : cause;
}

/* ONE WAY TO RUN A CHILD, and it hands the run's own bytes to the reporter that judges it. The five call sites
   this replaced each open-coded `spawnSync(..., { stdio: "inherit", timeout })` and then asked `runOutcome`
   about a run it had never seen — five copies of the same three options, and every one of them a place for the
   backstop to go missing (it already had: the smoke's spawn carried no timeout at all until the paragraph
   below was written, and that is the shape a per-site option takes when it is forgotten).
   THE TERMINAL LOSES THE LIVE STREAM AND KEEPS EVERY BYTE. `stdio: "inherit"` cannot also capture, and a
   captured pipe (`spawnSync`'s own `stdout`) is a fixed buffer that TRUNCATES the tail — which is exactly the
   half a hang is diagnosed from. So the child writes to a FILE, its path is announced BEFORE the run so it is
   tailable while it runs, and the whole file is written to this process's stdout afterwards, in order, so a
   transcript of this build still contains the run in full. */
/* THE PATH CARRIES THE RUN'S IDENTITY, because the checkout is shared and two builds are ordinary. The path
   used to be fixed per LABEL, so two concurrent builds opened the same file "w" and both children wrote into
   it -- and then each parent read the mixture back as its own `captured` and derived its own verdict from
   bytes the other run produced. That is not a garbled log, it is a measurement about no single run: an agent
   found `switches` going 8252 -> 8513 -> 11693 -> 9426 in one file, two independent series for one search
   interleaved, and the numbers were unanswerable. A stable name is kept as a SYMLINK so `— live at` stays
   tailable and so the last run of a label is still findable by hand; the bytes this parent reads are only
   ever the ones its own child wrote. */
function runChild(label, prog, args, hint) {
  const slug = label.replace(/[^A-Za-z0-9]+/g, "-").replace(/^-|-$/g, "");
  const log = join(OUT, `run-${slug}.${process.pid}.log`);
  const stable = join(OUT, `run-${slug}.log`);
  mkdirSync(OUT, { recursive: true });
  console.log(`[build] ${label} — live at ${log}`);
  const fd = openSync(log, "w");
  let t;
  /* THE TWO MEASURES ARE READ AROUND THE SPAWN AND BOTH ARE CARRIED OUT, because a verdict that names one of
     them must be able to print the other beside it. The CPU meter is the kernel's; the wall clock is this
     process's and is CONTEXT — it is the number that six builds' worth of readers mistook for a budget. */
  const cpuBefore = childCpuSeconds();
  const wallBefore = Date.now();
  try {
    t = spawnSync("/bin/sh",
                  ["-c", CPU_BUDGET_SH, `apiclient-build:${slug}`,
                   String(RUN_CPU_HARD_S), String(RUN_CPU_BUDGET_S), prog, ...args],
                  { stdio: ["inherit", fd, fd], shell: false, timeout: RUN_DEADLOCK_MS });
  }
  finally { closeSync(fd); }
  const cpuAfter = childCpuSeconds();
  t.cpuSeconds = (cpuBefore === null || cpuAfter === null) ? null : cpuAfter - cpuBefore;
  t.wallSeconds = (Date.now() - wallBefore) / 1000;
  /* Best-effort convenience only: a failure here must never change the verdict below. */
  try { rmSync(stable, { force: true }); symlinkSync(log, stable); } catch { /* not fatal */ }
  t.captured = readFileSync(log, "utf8");
  process.stdout.write(t.captured);
  /* THE BYTES TRAVEL WITH THE OUTCOME, because a stage whose verdict spans TWO runs cannot be reached from
     inside either one. The cold round trip is exactly that shape — session ONE writes a residue and session
     TWO rebuilds it, and "a kind session one wrote and this one did not rebuild" is a comparison of two
     children's output that nothing but the caller holds both halves of. */
  return Object.assign(runOutcome(label, t, hint), { captured: t.captured });
}

/* BOTH NUMBERS, AT EVERY OUTCOME, WITH THE VERDICT NAMING WHICH ONE DECIDED — CLAUDE.md §Testing, and the
   half of it this file had never done. The elapsed figure is labelled CONTEXT in the line itself rather than
   in a comment nobody reads at 2am, because it is the number that six consecutive builds' worth of readers
   took for a budget; the load average is printed beside it for the same reason it always was — it is what
   explains an elapsed figure, and it explains nothing about the CPU one. */
/* AND THE THIRD NUMBER IS NOT THIS FILE'S AT ALL — it is the ENGINE'S own clock, which is a different question
   from either of the two above and was stated nowhere. The two lines above are what THIS FILE spent and what
   the box was doing while it spent it; the line below is what the SCHEDULER INSIDE the child denominated its
   slice and its aging charge in, which is what decides the order of the very census samples every verdict here
   is derived from. A run that says how much CPU it consumed and not which currency its frontier was ordered in
   has answered the cheaper of the two questions. */
/* THE DENOMINATION IS PARSED BY THE CALLER AND HANDED IN, because the verdict line now states it too and one
   input read twice is two readings that can disagree about one run. It is a REQUIRED argument and its absence
   throws rather than reaching `quantumText` as `undefined` — which is not `null`, so the arm written for "this
   stage opened no engine slice" would be skipped and the next line would read `.sliceMs` off nothing. A
   consumer that defaults a producer's field is the defect this file is largely about; this is its own. */
const runNumbers = (t, q) => {
  if (q !== null && (typeof q !== "object" || typeof q.sliceMs !== "number" || typeof q.measure !== "string"))
    throw new Error(`[build] runNumbers was handed ${JSON.stringify(q)} where quantumDenomination's reading ` +
                    `belongs — that function returns either its {measure, sliceMs, isCpu, instances} record ` +
                    `or \`null\` for a stage that opened no engine slice, and anything else is a caller that ` +
                    `has stopped passing it rather than a run with nothing to say.`);
  return `[build]   CPU consumed: ${cpuText(t.cpuSeconds)} of the ${RUN_CPU_BUDGET_S / 60} min budget — THIS IS THE ` +
  `MEASURE THE VERDICT IS IN\n` +
  `[build]   elapsed ${t.wallSeconds.toFixed(1)} s against a ${RUN_DEADLOCK_MS / 60000} min deadlock ` +
  `backstop, at load ${loadNow()} on ${cpus().length} cores — CONTEXT, never the verdict\n` +
  quantumText(q);
};

/* WHAT THIS LINE IS A MEASUREMENT OF, IN THE LINE ITSELF — because a caveat and the number it is about must
   travel together or the caveat is not applied. The fact is already computed here: `quantumText` prints the
   engine's own denomination at EVERY outcome, pass included, and ends with an instruction it calls
   unconditional — "compare two runs of ONE revision before reading a difference between two revisions". That
   sentence has been right and in the wrong place. It is a DETAIL line, and what gets pasted, quoted and
   dispatched on is the STAGE TABLE row, which carries `standingText`'s fraction, its table count and its four
   work totals and said nothing whatever about what any of them is a reading OF.
   MEASURED, AND IT IS THE READER THAT FAILED RATHER THAN THE INSTRUMENT: three readings in one session, all
   three taken off the stage table with the detail line available. Probe standings of 140, 141, 173 and 204
   were quoted across revisions as if comparable; a counter reading 0 was raised as an alarm; and a lane was
   dispatched at "the top abort". Then two runs of ONE frozen snapshot — same binary, same bytes, same
   machine — answered 35/208 and 75/208, over 4 @H tables and 35, at 3,003 and 34,947 units of engine work,
   with 0 and 22,603 jobs run, and TERMINATED DIFFERENTLY: one at a layout assert, one having spent its whole
   CPU budget without ever reaching it. Every quantity in that sentence is a stage-table quantity and not one
   of them is a property of the revision — the terminal event included, which is the one nobody suspects.
   CLAUDE.md §A-CAVEAT-STATED-AND-THEN-NOT-APPLIED names this failure and names its tell as SYNTACTIC: "a
   paragraph that names a source of variance, followed by a paragraph that reasons from a quantity that
   variance governs, with nothing in between that BOUNDS it". A detail line above a stage table IS that shape.
   Inside the row there is nothing in between, and the fraction cannot be quoted without it.
   SO IT IS ON EVERY ARM, INCLUDING THE PASS, AND THE PASS IS THE ARM IT IS MOST NEEDED ON — the runs a reader
   compares are the ones that finished, so a caveat carried only by the failures is a caveat absent from
   precisely the comparison it is about. That is `quantumText`'s own argument, applied one line up rather than
   restated.
   AND IT IS NOT A CONSTANT STAMP, which is the one real argument against marking every verdict (`hungCause`
   states it, and its paragraph is rewritten to the rule that now holds rather than left asserting the old
   one). Two of the mark's three parts are READ OFF THE ARTIFACT and not written here — the engine's own word
   for its denomination and its own slice, both off the `@QUANTUM` line solver/quantum.c composes, so a fork
   that changes either moves this text by itself. The third VARIES BY ARM, which is the half a reader can act
   on: what survives a repeat is the fixture's own denominator, plus an assertion's IDENTITY on the arms that
   have one and nothing at all on the arms that do not. CLAUDE.md §Testing is where that list comes from — "a
   crash's IDENTITY and its frame list, a conservation identity read within one sample, a count that cannot be
   true, and a value that is wrong rather than small" survive; "any comparison of totals across two runs" does
   not — so a verdict naming an abort has something quotable in it, a PASS has nothing, and the mark says
   which rather than leaving a reader to know.
   IT IS NOT A VERDICT AND IT DECIDES NOTHING (§NO BOUNDS). No arm branches on it, no code reads it back, no
   stage's exit code moves by one. It is the sentence that stops a total being read as a property of the tree.
   ABSENCE IS REPORTED AS ABSENCE, and the INVERSE READING IS REFUSED IN THE SAME BREATH. A stage that opens
   no engine slice prints no `@QUANTUM` line, and this says so rather than defaulting to a denomination it did
   not observe — and "no denomination stated" is not "these numbers are comparable", which is exactly the
   reading an unmarked row invites. Measured on the two-instance ABI drive, which prints no `@QUANTUM` line at
   all: one unchanged driver against one binary returned 5, 5, 31, 31 routed posts over four runs, and a 6x
   "collapse" was localised to a commit by a bisect before anybody re-ran the old revision. */
const survivesRepeat = (stand, aborted) => {
  const s = [];
  if (stand) s.push(`the ${stand.asked} statements the fixture itself asks`);
  if (aborted) s.push(`the IDENTITY of the assertion named above — its file:line and what it says`);
  return s.length ? s.join(", and ") : `nothing this line states`;
};
const runDependenceText = (q, stand, aborted) =>
  q === null
    ? `  [NO DENOMINATION STATED — this stage printed no @QUANTUM line, so nothing in its own output says ` +
      `what its numbers are a reading OF. That is an absence, and an absence is not a licence to read them ` +
      `across two runs.]`
    : `  [ONE INTERLEAVING (${q.measure}-denominated ${q.sliceMs} ms slice) — WHICH terminal event this line ` +
      `names, and any total it carries, is a reading of THIS RUN and not of this revision: a repeat of this ` +
      `same artifact moves them and can end somewhere else entirely. Surviving a repeat: ` +
      `${survivesRepeat(stand, aborted)}.]`;

/* THE PAGE'S OWN UNCAUGHT ERRORS, WHICH ARE THE ONE THING A GREEN RUN CAN BE HIDING. A `<script>` that throws
   ends at the throw — spec-correct, and CLAUDE.md deliberately makes this a PRINT rather than an assert,
   because a forced-exec flow throwing on opaque or attacker input is the one class of error that is NOT a
   should-never-happen. What was wrong was never that the error happened; it was that nothing SAID so. Every
   endpoint the rest of that script would have emitted is simply absent, every probe row over them reads 0,
   and a 0 row is indistinguishable from the capability behind it being unbuilt — three states behind one
   answer, which is the defect the fixture's whole design exists to prevent.
   ITS READER IS THIS FUNCTION AND EVERY ARM OF IT, INCLUDING THE PASS. That is the point of putting it here
   rather than in a hint: a run can answer its whole probe table while a script died a thousand statements
   before the rows that script would have moved, and the arm that reports such a run is the PASS arm. It paid
   for itself on its first run by naming a document-ending regression that had been invisible across every
   prior build, and the only thing it lacked was a reporter — a marker a person has to scroll a log for is a
   measurement nobody takes. It is REPORTED and never a verdict: a page error is evidence, not a failure. */
/* …AND THE PART OF IT THAT WAS ITSELF THREE STATES BEHIND ONE ANSWER, WHICH IS THE DEFECT ONE LEVEL UP. The
   fixture STAGES uncaught errors on purpose (a chunk whose top level throws §4.13.3's SyntaxError, and a
   rejection nobody handles), so this line was ON for every run there has ever been and carried no information
   in the state it spends its life in. An unexpected SECOND error read as `2 UNCAUGHT PAGE ERROR(S), first:
   <the expected one>` — the count moved and the one that mattered was not even quoted — and `staged only`,
   `staged plus a real one` and `a real one only` were one number. A reader that cannot separate a designed
   observation from a real one is not a reader.
   THE PARTITION IS BY SCRIPT AND NEVER BY MESSAGE TEXT. A message is engine prose: `x-panel` breaking raises
   the identical "not a valid custom element name" string from the DOCUMENT's script, so a text-keyed
   suppression list swallows exactly the regression it exists to surface. `at=` is HTML §8.1.4.6 "Runtime
   script errors"'s `filename`, which the fixture's own chunk table mints and no other program of that document
   can produce — and it is right only because a queued script now carries the address its bytes came from.
   THE STAGED SET IS THE FIXTURE'S OWN STATEMENT, NOT THIS FILE'S. `@PAGEERR-STAGED` is printed from the same
   table the chunks are served from, so nothing here has to be kept in step with the document, and a fixture
   that stages one more error moves both sides by itself. It is also NOT a suppression: each staged error is
   asserted to have HAPPENED by a NODE_ALGOS row over the result document, so one that stops occurring still
   fails — this only says which occurrences are expected.
   `-` IS §8.1.4.6'S OWN ANSWER for a thrown value with no backtrace and is reported as its own population,
   because "raised from nowhere nameable" is a fact about the throw and not a hole in this reader.
   REPORTED AND NEVER A VERDICT: a page error is evidence, not a failure. */
/* AND THE CORRECTIONS, SUBTRACTED — because a stream cannot withdraw a line it has printed. `@PAGEERR` says a
   (message, throw site) pair now stands; `@PAGEERR-RETRACTED` says its last standing occurrence was taken back,
   which HTML §8.1.6.4 "HostPromiseRejectionTracker(promise, operation)" step 7.4 raises when the page attaches
   a handler in a later task. A reader that counted only the first would report an UNSTAGED UNCAUGHT PAGE ERROR
   for a page that did nothing wrong — the fabricated finding the retraction exists to remove, surviving in the
   one place a person actually reads the run.
   PAIRED IN ORDER AND NOT SET-SUBTRACTED, because the producer's latch can re-announce a pair after correcting
   it (result.h): a message reported, retracted, and reported again stands ONCE at the end, and set arithmetic
   would say it stands zero times. A correction that matches no standing report is a BROKEN PRODUCER CONTRACT
   and throws rather than being ignored — result.c fires RETRACTED only on the falling edge of a pair it has
   already announced, so an unmatched one means that latch has stopped holding and the count on either side of
   it has stopped meaning anything. */
function pageErrorText(out) {
  const staged = new Set([...out.matchAll(/^@PAGEERR-STAGED (\S+)$/gm)].map((m) => m[1]));
  /* AND THE ENGINE'S OWN CLAIM ABOUT A ROW IT PRODUCED — solver/result.h. Keyed on the (message, throw site)
     PAIR rather than on an address, and the asymmetry with `staged` is the design: an address is the only unit
     a FIXTURE can speak in, because a program is what a fixture owns, while the ENGINE knows the exact throw
     it minted. A forked completion over unknown external input reaching a spec step whose answer is a throw is
     the exploration surface — CLAUDE.md names it among the things that are deliberately NOT a `@WHY` — and
     before this line it arrived here as `UNSTAGED UNCAUGHT PAGE ERROR`, which is this reader's name for a
     fixture statement that broke. Two facts, one number, in the very function written to end three states
     behind one answer: it misread a designed world as a regression twice, once for an expert reader and once
     for a whole session's brief.
     IT IS NOT A SUPPRESSION AND CANNOT BECOME ONE. The error is still counted, still quoted, still its own
     population — what changes is that the population a reader is sent to investigate contains only throws
     nobody declared. And it is not a message-text list either, which is what the address partition next door
     refuses for good reason: the pair is COMPUTED by the producer at the instant it raises, so a real page
     raising the identical string from the identical site was never declared and lands in `rogue`.
     THE SEPARATION IS ALSO WHY THE TWO POPULATIONS MUST NOT BE ADDED. A staged error's ABSENCE is a finding
     (the fixture stopped exercising something); an explored one's absence is the SCHEDULER's answer and never
     the fixture's — how many flows reach a forked completion is not a fact this document states — so it is
     reported as a count and is never asserted against a number. */
  /* ONE SPELLING OF THE PAIR, MINTED AND PROBED THROUGH THE SAME FUNCTION. The set and the membership test are
     two sites naming ONE key, and a key each composes for itself is the read-with-no-writer defect with no name
     missing for a grep to find: both spellings look right alone, `has` answers false for every pair a producer
     ever declared, and the engine's own throw lands in `rogue` wearing the one label this partition exists to
     deny it. It is not hypothetical — the joiner disagreed here ("\0") and at the probe (" ") from this line's
     first day, so `mine` was empty in every run there has ever been and `mineText` said nothing, which reads
     exactly like a run that explored no throwing world. A SEPARATOR and not a bare concatenation because an
     address and a message are both free text: at="a b"+msg="c" and at="a"+msg="b c" are different pairs that
     concatenate alike. NUL is the byte neither can contain — written as the ESCAPE and never as the raw byte,
     because a single raw NUL makes this whole file BINARY to grep, and a reader grepping `PAGEERR` across the
     .mjs tree getting no hit at all is how a two-site key gets to disagree unobserved in the first place. */
  const pairKey = (at, msg) => at + "\0" + msg;
  const explored = new Set();
  for (const m of out.matchAll(/^@PAGEERR-EXPLORED at=(\S+) (.*)$/gm)) {
    const msg = m[2].trim();
    /* THE SAME ACCEPTANCE RULE AS THE ANNOUNCEMENT LOOP BELOW, which drops an empty message. A side that keeps
       what the other drops mints a key nothing can ever match, and would fire the contract check below on a
       disagreement between two readers rather than on anything the producer did. */
    if (!msg) continue;
    explored.add(pairKey(m[1], msg));
  }
  const errs = [];
  /* EVERY PAIR EVER ANNOUNCED, WHICH IS NOT `errs` — a retraction splices the pair out of what STANDS, and an
     explored pair whose report was later taken back is a correct run rather than a broken producer. */
  const announced = new Set();
  let retracted = 0;
  for (const m of out.matchAll(/^@PAGEERR(-RETRACTED)? at=(\S+) (.*)$/gm)) {
    const e = { at: m[2], msg: m[3].trim() };
    if (!e.msg) continue;
    if (!m[1]) { announced.add(pairKey(e.at, e.msg)); errs.push(e); continue; }
    const k = errs.findIndex((s) => s.at === e.at && s.msg === e.msg);
    if (k < 0)
      throw new Error("[build] an @PAGEERR-RETRACTED line names a (message, throw site) pair no @PAGEERR line " +
                      "announced — solver/result.c prints the retraction only when the last STANDING " +
                      "occurrence of a pair it already announced is taken back, so an unmatched one means " +
                      "that latch is broken and every page-error count in this report is wrong: " +
                      JSON.stringify(e.msg.slice(0, 160)) + " at " + e.at);
    errs.splice(k, 1);
    retracted++;
  }
  /* AND THE CHECK THAT KEEPS `mineText` FROM READING ZERO WHEN IT IS BROKEN RATHER THAN QUIET — the sibling of
     the @PAGEERR-RETRACTED contract above, and it exists because the failure it catches ALREADY HAPPENED here
     and was unobservable for precisely the reason `mineText` names: a silent zero and a mechanism that stopped
     being reached read alike. `mine` stays REPORTED and never asserted against a number — how many flows reach
     a forked completion is the SCHEDULER's answer, so an empty `explored` set is a correct run and nothing
     here objects to it. What is NOT the scheduler's answer is a declaration that classifies NOTHING:
     test_forced.c prints @PAGEERR-EXPLORED from the same call as the @PAGEERR it qualifies, with the same two
     arguments, on the standing edge only — so a declaration matching no announcement means the reader's key
     has stopped meeting the producer's and every population below it is mis-assigned.
     ASKED AGAINST WHAT WAS ANNOUNCED AND NOT AGAINST WHAT STANDS, so a retraction can never fire it. */
  for (const k of explored)
    if (!announced.has(k))
      throw new Error("[build] an @PAGEERR-EXPLORED line names a (message, throw site) pair no @PAGEERR line " +
                      "announced — test_forced.c prints the declaration from the same call as the report it " +
                      "qualifies, so an unmatched one means this reader's key no longer meets the producer's " +
                      "and every page-error population in this report is mis-assigned: " +
                      JSON.stringify(k.slice(k.indexOf("\0") + 1, k.indexOf("\0") + 161)) +
                      " at " + k.slice(0, k.indexOf("\0")));
  /* SAID EVEN WHEN NOTHING STANDS, because it is the difference between a run whose page raised nothing and a
     run whose page raised errors and handled every one of them — §Testing's absent-count-versus-zero-count on
     the stream route, exactly as `pageErrorsRetracted` is on the document route. */
  const retractedText = retracted ? ` (plus ${retracted} reported and retracted, handled in a later task)` : "";
  if (!errs.length) return retracted ? ` — 0 standing page error(s)${retractedText}` : "";
  /* THREE POPULATIONS AND NOT TWO, IN THAT ORDER, BECAUSE THE CLAIMS ARE ASKED OF DIFFERENT UNITS. The
     fixture's is an ADDRESS and the engine's is a PAIR, so an error can satisfy both — a chunk that stages a
     throw the engine also declares its own — and a reader needs each error in exactly one column. The staged
     claim is asked FIRST because it is the DOCUMENT's statement about a program it owns, and a fixture that
     stopped exercising its own staged throw must not have that absence hidden by the engine's classification
     of some other throw at the same address. */
  const isExplored = (e) => explored.has(pairKey(e.at, e.msg));
  const known = errs.filter((e) => staged.has(e.at));
  const mine = errs.filter((e) => !staged.has(e.at) && isExplored(e));
  const rogue = errs.filter((e) => !staged.has(e.at) && !isExplored(e));
  /* THE STAGED COUNT IS CARRIED EVEN WHEN NOTHING IS WRONG, because its DISAPPEARANCE is the other direction
     this line can report: a run in which the document staged two errors and produced one is a run whose
     fixture stopped exercising something, and a reader shown only the rogue population would see silence. */
  const stagedText = `${known.length} from the ${staged.size} address(es) it declares`;
  /* AND THE ENGINE'S OWN, CARRIED ON THE SAME RULE BUT NEVER AS AN ASSERTION. Its absence is not a finding
     about this document: how many flows reach a forked completion is the SCHEDULER's answer, so a run that
     explored one throwing world and a run that explored none are both correct runs of one fixture. It is here
     because the count is what tells a reader the classification is WIRED — a silent zero and a mechanism that
     stopped being reached read alike, and only one of them is worth a look. */
  const mineText = mine.length ? `, plus ${mine.length} this engine raised itself exploring a forked completion`
                               : ``;
  if (!rogue.length)
    return ` — ${errs.length} page error(s), none unaccounted for: ${stagedText}${mineText}${retractedText}`;
  const q = (e) => `${JSON.stringify(e.msg.slice(0, 160))} at ${e.at === "-" ? "no throw site (§8.1.4.6's own answer for a value with no backtrace)" : e.at}`;
  return ` — ${rogue.length} UNSTAGED UNCAUGHT PAGE ERROR(S) (plus ${stagedText}${mineText}${retractedText}), first: ${q(rogue[0])}` +
         (rogue.length > 1 ? ` (+${rogue.length - 1} more, deduped by solver/result.c on (message, throw site))` : "");
}

/* WHAT THE PAGE ITSELF SAID WENT WRONG — the @LOG stream's `level`, which is Console Standard §2.3 Printer's
   own severity and is the ONLY thing that separates a bundle's ordinary chatter from the page reporting a
   failure. It is evidence of a different STRENGTH from an uncaught throw and the two belong beside each
   other: a throw ENDS the script (every endpoint after it silently absent), while `console.error` means the
   page noticed something and CARRIED ON. Reported, never a verdict — a page's own error log is a fact about
   the page, not a defect in this engine.
   IT COUNTS BY LEVEL AND REPORTS ONLY THE SEVERE ONES, because a real bundle logs constantly (the corpus logs
   carry hundreds of `warn` lines from one analytics library) and a count of everything is a count of nothing.
   `level` was written on every console line this engine has ever printed and read by nothing, which made the
   severity of a page's own output a distinction the stream carried and no report used.
   IT IS NOT ON THE RESULT DOCUMENT AND SHOULD NOT BE. `pageErrors` rides the document because an uncaught
   throw is ONE deduped fact per script; a page's console output is unbounded and would put a bundle's whole
   log inside a record composed every PARTIAL_MS. So this is the line-oriented host's half of the same split
   result.h states for a page error: a host whose output IS lines reports it from the lines. */
function consoleSeverityText(out) {
  const bad = { error: 0, warn: 0, assert: 0 };
  let first = "";
  for (const m of out.matchAll(/^@LOG (\{.*\})$/gm)) {
    let v;
    try { v = JSON.parse(m[1]); } catch { continue; }   /* a truncated tail is not a finding about the page */
    if (typeof v.level !== "string")
      throw new Error("[build] an @LOG line carries no string `level` — browser/core/console/console.c writes " +
                      "one on every line it prints (Console Standard §2.3 Printer's severity), so a line " +
                      "without it is that printer having changed under this reader, and the severity of a " +
                      "page's own output would silently become uncountable.");
    if (!(v.level in bad)) continue;
    bad[v.level]++;
    if (!first) first = m[1].slice(0, 160);
  }
  const n = bad.error + bad.assert;
  if (!n && !bad.warn) return "";
  return ` — the page's own console: ${bad.error} error, ${bad.assert} assert, ${bad.warn} warn` +
         (n ? `, first severe: ${JSON.stringify(first)}` : ``);
}

/* WHAT THE PAGE ASKED A PERSON — the `@DIALOG` stream, which HTML §8.9.1 Simple dialogs' show step writes for
   every `alert()`, `confirm()` and `prompt()` a run reaches (browser/core/html/simple_dialogs.c). It is
   reported beside the page's own console for the reason that one is: it is a fact about the PAGE and never a
   verdict on this engine.
   IT IS WORTH A LINE BECAUSE TWO OF THE THREE RETURN A VALUE THIS ENGINE DOES NOT KNOW. A `confirm` answers a
   concolic boolean and a `prompt` a concolic string, so each one a run reaches is a place the frontier FORKED
   on what a person would have said — which is exactly the reach a headless browser does not have, since a
   headless browser takes §8.9.1's optional "we cannot show simple dialogs" arm and answers false/null. A run
   with dialogs in it is a run whose later findings sit behind a question somebody was asked, and the count is
   how a reader sees that without reading the log.
   NEITHER FIELD IS DEFAULTED, for the reason the @LOG reader above states about `level`: simple_dialogs.c
   writes `dialog` and `message` on every line it prints, so a line missing either is that emitter and this
   reader having come apart — and a dropped line is indistinguishable from a dialog that never happened once
   the answer is a number. An UNKNOWN `dialog` throws for the same reason the @FORKAT census throws on an
   unknown member: §8.9.1 has exactly three, they are the three strings the algorithm hands to WebDriver BiDi
   user prompt opened, and a fourth is a producer this reader has not been told about. */
function dialogText(out) {
  const n = { alert: 0, confirm: 0, prompt: 0 };
  let first = "";
  for (const m of out.matchAll(/^@DIALOG (\{.*\})$/gm)) {
    let v;
    try { v = JSON.parse(m[1]); } catch { continue; }   /* a truncated tail is not a finding about the page */
    if (typeof v.dialog !== "string" || typeof v.message !== "string")
      throw new Error("[build] an @DIALOG line carries no string `dialog` or no string `message` — " +
                      "browser/core/html/simple_dialogs.c writes both on every line it prints (HTML §8.9.1's " +
                      "own name for the dialog, and the message after normalize newlines), so a line without " +
                      "one is that emitter having changed under this reader.");
    if (!(v.dialog in n))
      throw new Error(`[build] an @DIALOG line carries the dialog name ${JSON.stringify(v.dialog)} — HTML §8.9.1 ` +
                      `Simple dialogs defines three (alert, confirm, prompt) and simple_dialogs.c writes ` +
                      `those three literals, so a fourth is a producer this reader has not been told about.`);
    n[v.dialog]++;
    if (!first) first = m[1].slice(0, 160);
  }
  const total = n.alert + n.confirm + n.prompt;
  if (!total) return "";
  return ` — the page opened ${total} modal dialog(s): ${n.alert} alert, ${n.confirm} confirm, ` +
         `${n.prompt} prompt, first: ${JSON.stringify(first)}`;
}

function runOutcome(label, t, hint) {
  /* APPENDED TO EVERY VERDICT THIS FUNCTION PRODUCES, which is why it is computed once here and folded into
     `bad` rather than added at each arm — an arm added later would otherwise be the one that drops it, and
     the arm most likely to be added later is another failure arm. */
  const pe = pageErrorText(t.captured) + consoleSeverityText(t.captured) + dialogText(t.captured);
  /* AND WHERE THE RUN GOT TO, ON THE SAME RULE AND FOR THE SAME REASON — computed once and folded into `bad`,
     because three arms below already asked probeStanding for the FRACTION and exactly ONE of them printed the
     ROWS behind it, so the two arms that drop it are the two that KILL the child.
     THAT IS THE WORST PLACE TO DROP IT, because a row's 0 is TWO different findings and a kill decides which:
     a statement this run answered WRONGLY, or one it never REACHED. They send a reader to opposite places —
     the mechanism the row names, or the budget — and a killed run is where the second is overwhelmingly the
     answer. The fraction alone does not separate them: it says how many rows are 0 and nothing about WHICH,
     so a reader with a row list in front of them and 83/177 above it can still take one 0 as a verdict on the
     builtin it names. THE NAMES ARE WHAT SEPARATE THEM — `sortbranch` standing beside `fefork owfork genfork
     setaddfork redfork rerepfork toprimfork gcallfork hostreq-fork` is visibly a run that stopped part-way,
     and `sortbranch` alone in a list of one would be a builtin that broke. Measured, and it cost a lane a
     session: a budget-killed smoke that answered 83/177 was read row by row, one 0 among ninety-odd was
     singled out, and a lane was dispatched to fix a fork-clone gap in Array.prototype.sort's step machine
     that no evidence supported — the eight rows quoted at it split one-world-passes / two-world-fails, which
     reads as a defect in forking and IS the shape "the second world never got there" makes.
     `unanswered` was already computed for all of them; only its reader was missing from the arms that needed
     it most, which is the mirror of the defect the comment over probeStanding describes. */
  const stand = probeStanding(t.captured);
  /* HOISTED ABOVE `bad`, WHICH IS WHAT MAKES IT READABLE FROM THE KILLING ARMS AT ALL. It used to be declared
     below the SIGXCPU and ETIMEDOUT arms, and those arms CALL `bad` — so a closure reading it from there
     would reach its temporal dead zone and throw a ReferenceError, turning a reportable stage into an
     unhandled one. `abortRecord` is a pure match over the captured bytes, so computing it for every arm costs
     one regex and changes no arm's answer. */
  const aborted = abortRecord(t.captured);
  /* THE SAME RULE AS `pe` DIRECTLY ABOVE, AND FOR THE SAME REASON: computed once here and folded into `bad`
     rather than added at each arm, because the arm most likely to be added later is another failure arm and
     it would be the one that drops it. The denomination is parsed ONCE and serves both readers — the mark on
     the verdict and `quantumText` in the detail block — so the two can never state different slices for one
     run. It is parsed BEFORE any arm returns, so a malformed `@QUANTUM` line throws for every outcome rather
     than only for the outcomes whose detail block happened to be reached. */
  const q = quantumDenomination(t.captured);
  const rd = runDependenceText(q, stand, aborted);
  const bad = (verdict, code, why) => {
    console.error(`[build] ${label} ${why}`);
    console.error(runNumbers(t, q));
    if (hint) console.error(`[build]   ${hint}`);
    if (stand && stand.unanswered.length)
      console.error(`[build]   the rows still 0 (${stand.unanswered.length} of ${stand.asked}): ` +
                    stand.unanswered.join(" ") + `\n` +
                    `[build]   each of those is EITHER a statement this run answered wrongly OR one it never ` +
                    `reached — read them against the standing in the verdict above, and never read a single ` +
                    `0 as a verdict on the mechanism its row names while the others beside it are 0 too.`);
    return { label, verdict: verdict + pe + rd, code };
  };
  /* THE BUDGET INSTALL, WHICH MUST NEVER FAIL QUIETLY. A run without its rlimit is an UNMEASURED run wearing a
     measured one's report, so the shell says so in the log and this reads the marker rather than trusting a
     bare 125 — which is also a code a program may legitimately exit with. */
  if (t.status === 125 && t.captured.includes(BUDGET_NOT_INSTALLED))
    return bad("CPU BUDGET NOT INSTALLED", 5,
      `could not install its ${RUN_CPU_BUDGET_S} s RLIMIT_CPU — \`ulimit -S -t\` was refused, which happens ` +
      `when this process already sits under a LOWER hard limit. Nothing was run: a run under an unknown ` +
      `budget would report a number about no budget at all.`);
  /* THE BUDGET REACHED — SIGXCPU, FROM THE KERNEL. Not a hang and not a crash: the run consumed exactly the
     CPU this file agreed to spend, which is a fact about the tree and is why the census's cause and the
     fixture's own standing both belong in the verdict rather than in a hint under it. */
  if (t.signal === "SIGXCPU") {
    const cause = hungCause(t.captured);
    return bad(`CPU BUDGET SPENT — ${standingText(stand)} — ${causeName(cause)}`, 2,
      `SPENT ITS WHOLE ${RUN_CPU_BUDGET_S / 60} min CPU BUDGET — killed by the KERNEL at the rlimit, which is ` +
      `the verdict measure and is invariant to what else this box was doing.\n` +
      `[build]   the census says it was ${cause}`);
  }
  /* AND THE CASE CPU CANNOT SEE — SIGTERM, FROM THIS HARNESS. A deadlocked child consumes nothing, so the
     rlimit never fires on one and this is the only thing that will. The CPU figure printed beside it is what
     tells the two apart in the one case they blur: a child that consumed most of its budget in this many wall
     minutes was STARVED of the thread rather than deadlocked on it, and the reader can see which. */
  if (t.error && t.error.code === "ETIMEDOUT") {
    const cause = hungCause(t.captured);
    return bad(`DEADLOCK BACKSTOP — ${standingText(stand)} — ${causeName(cause)}`, 4,
      `RAN ${RUN_DEADLOCK_MS / 60000} min OF WALL CLOCK WITHOUT REACHING ITS ${RUN_CPU_BUDGET_S / 60} min CPU ` +
      `BUDGET — killed by the harness, NOT by the kernel. Read the CPU figure below: near the budget means ` +
      `this child was starved of the thread, near zero means it was waiting on something that never came.\n` +
      `[build]   the census says it was ${cause}`);
  }
  /* AND THE TWO ARMS THAT NEVER CARRIED THE STANDING AT ALL. The paragraph above put `unanswered` in front of
     every verdict and the FRACTION was still dropped by exactly these two, which are the arms where an abort
     is the result — so a smoke that died at an assertion printed its cause and said nothing about whether the
     probe table under it had been composed at 0 units of work or at tens of thousands. Those are the two
     readings of every 0 in that table and an abort is precisely where the question is live: the run stopped,
     and how far it had got is what decides whether a row names a broken mechanism or a stretch of the document
     nothing reached. `standingText` carries the measured number now (`probeWork`), so this is one call and not
     a sentence anybody has to compose. */
  const standWith = (v) => v + (stand ? " — " + standingText(stand) : "");
  if (t.signal) {
    return bad(standWith("CRASHED on " + t.signal + (aborted ? " — " + causeName(aborted) : "")), 3,
      `DIED ON ${t.signal} — an abort is a DCHECK naming either an invariant to fix at its root or a ` +
      `capability to build, and it is the RESULT of this run rather than an interruption of it.\n` +
      `[build]   ` + (aborted ? aborted
                              : `no @WHY/@E line in this run's output — this signal did not come from ` +
                                `check.h, so the cause is above and is not an assertion`));
  }
  /* THE ABORT THAT ARRIVES AS AN ORDINARY EXIT STATUS — the wasm smoke's only shape. Same class and same code
     as the signal above, because it IS that event; only the transport differs. */
  if (t.status !== 0 && aborted)
    return bad(standWith("ABORTED — " + causeName(aborted)), 3,
      `ABORTED at an assertion and exited rc=${t.status} — under emscripten an abort() is a thrown ` +
      `RuntimeError rather than a signal, so this is the same event the native targets report as SIGABRT. ` +
      `The line below names what to fix or build; it is the RESULT of this run.\n` +
      `[build]   ${aborted}`);
  /* THE STAGE THAT DECLINED TO MEASURE — `skipped` for a condition only the child could see. It keeps a
     non-zero code by construction like every other non-pass, and it says NOT MEASURED rather than FAILED
     because the two send a reader to opposite places: one to the layer under test, the other to the tree the
     artifact was linked from. */
  const refused = t.captured.match(REFUSED_WITNESS);
  if (t.status !== 0 && refused)
    return bad("NOT MEASURED — " + causeName(refused[1]), 7,
      `REFUSED TO MEASURE and exited rc=${t.status} — it declined to ask its question, so this is NOT a ` +
      `verdict on what it measures and nothing below its first check ran.\n[build]   ${refused[1]}`);
  /* AND THE PROGRAM'S OWN VERDICT CARRIES WHERE IT GOT TO. This is the arm the smoke reaches when its
     frontier DRAINS and its probe table is merely incomplete — the one outcome for which "how much of the
     fixture was answered" is the entire content of the result, and it read `FAILED rc=1` with the number
     computed and dropped exactly as the budget arms did. */
  if (t.status !== 0) {
    /* THE ROW LIST THAT USED TO BE SPELLED OUT HERE IS GONE FROM THIS ARM and is not lost with it: `bad`
       prints it for EVERY verdict now, which is what put it in front of the two killing arms that never had
       it. Keeping a second copy here would print it twice and would be the per-arm plumbing folding it into
       `bad` exists to end. */
    return bad("FAILED rc=" + t.status + (stand ? " — " + standingText(stand) : ""), t.status || 1,
      `FAILED rc=${t.status} with NO assertion line in its output — this is the program's own verdict on ` +
      `itself (for the smoke, test_forced.c's probe table reporting INCOMPLETE), not a crash`);
  }
  /* A ZERO EXIT OVER AN ABORT WITNESS IS AN IMPOSSIBLE STATE. Every emitter of those two shapes aborts on its
     next statement, so a run that printed one and exited 0 either swallowed the abort or something else is
     writing the tag — and both are worth a non-pass rather than a green stage. Reported rather than thrown,
     because a stage that throws is a door in front of every stage behind it. */
  if (aborted)
    return bad("ABORT WITNESS OVER A ZERO EXIT", 6,
      `EXITED 0 having printed an assertion line — check.h and quickjs-check.h both abort() on the next ` +
      `statement, so either an abort was swallowed on the way out or something other than those two macros ` +
      `is writing the tag. Both are defects and neither is a pass.\n[build]   ${aborted}`);
  /* A PASS PRINTS ITS COST TOO, and the verdict carries where the run got to. A stage that answers every
     statement it makes and a stage that makes none both used to read `PASS`, and the CPU a passing smoke
     spends is the one number that says a revision made the fixture cheaper or dearer to answer — which is
     invisible if it is only ever printed on the runs that failed. */
  console.log(runNumbers(t, q));
  /* `stand` IS THE ONE HOISTED TO THE TOP OF THIS FUNCTION — the second `probeStanding(t.captured)` that stood
     here was a fourth reading of one input, taken in the arm least likely to disagree with the other three. */
  /* THE PASS ARM CARRIES IT TOO, AND IT IS THE ARM THAT NEEDS IT MOST — see pageErrorText. A run that answers
     every statement it makes while one of the page's scripts died is still a PASS of the probe table and is
     not a clean run of the document, and those two are the same green line without this. */
  return { label, verdict: (stand ? `PASS — ${standingText(stand)}` : "PASS") + pe + rd, code: 0 };
}

/* A STAGE THAT CANNOT RUN IS REPORTED AS SKIPPED WITH ITS REASON, AND IT CARRIES A NON-ZERO CODE. Absorbing it
   into a pass is the excluded test again — a run that did not ask the question must never read like one that
   asked and liked the answer. The code is non-zero BY CONSTRUCTION rather than by the argument that whatever
   caused the skip already failed: that argument is true today and is exactly the kind of thing a later edit
   makes quietly false. */
function skipped(label, why) {
  console.error(`[build] ${label} SKIPPED — ${why}`);
  return { label, verdict: "SKIPPED (" + why + ")", code: 1 };
}

/* PER STAGE, IN ONE REPORT — §Testing: a gate reports PER AREA, never one number in which one area drowns the
   rest. Every stage this run reached is named with its own verdict, so "the smoke failed" and "the two-instance
   drive was never asked" can never again be the same line. The exit code is the FIRST non-zero in stage order,
   which is exactly what each single-stage target exited with before. */
/* THE REVISION THIS BUILD IS OF, TAKEN BEFORE THE FIRST COMPILER RUNS. This file STAMPED the artifact and
   never SAID anything, so the one program every other gate then measures arrived with no revision on it and
   each reader restated the pair from memory — which is the recovery-by-forensics failure gate_revision.mjs
   was written for, one layer earlier: a stamp nobody reads aloud is a fact that has to be dug up.
   AND THIS IS THE STAGE THE QUESTION IS SHARPEST FOR. §Testing's worked example of a program no revision
   contains IS A BUILD: `idl_args.h` gained a field 33 seconds before a link finished, two translation units
   disagreed on a struct's size, and the segfault was in `strcmp` inside a DFAIL's own order check. A build
   reads its inputs over minutes from a checkout several agents are editing, so "which revision is this" is not
   a formality here — it is the difference between a verdict and an artifact of when the reads happened. */
let REV_AT_START = null;
const revAtStart = () => (REV_AT_START ||= gateRevision(
  ["engine/host", "engine/qjs", "engine/build.mjs", "engine/gate_revision.mjs"]));

function report(stages) {
  const w = Math.max(...stages.map((s) => s.label.length));
  /* BEFORE THE STAGE TABLE, because the tail is what gets pasted and the revision is what the table is about.
     Printed on the failing path as well as the passing one: a stage that ABORTED is the result most likely to
     be quoted at another agent, and "which tree aborted" is the whole of what they need. */
  for (const l of revisionLines(revAtStart())) console.log(l);
  const moved = revisionMoved(revAtStart());
  /* A POSITIVE STATEMENT EITHER WAY. The sources that were COMPILED are not necessarily the sources on disk
     now, and a reader who runs `git show` after this build would be reading a different program. Saying "did
     not move" is what makes the silence readable as an answer rather than as a question nobody asked. */
  if (moved) console.error("[rev] THE TREE MOVED UNDER THIS BUILD — " + moved + ". The stages below measured " +
                           "the sources as they were read, which no revision now describes.");
  else console.log("[rev] the tree did not move under this build");
  console.log("[build] ── stages ──");
  for (const s of stages) console.log("[build]   " + s.label.padEnd(w) + "  " + s.verdict);
  const bad = stages.find((s) => s.code !== 0);
  if (bad) { console.error("[build] BUILD FAILED — " + bad.label + ": " + bad.verdict); process.exit(bad.code); }
  process.exit(0);
}

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const HOST = join(ENGINE, "host");
const OUT = join(HOST, "out");
const EXT_QJS = join(ENGINE, "..", "extension", "lib", "qjs");   // where bridge.js imports the engine from
/* THE CHECKOUT ROOT, and the only thing anything here is allowed to state an in-tree path relative to. §Testing
   runs every gate from a frozen snapshot, so the absolute path of this tree is a fact about one directory that
   existed for one afternoon — anything derived from it (a cache key, a `__FILE__`, an emitted manifest) is a
   claim about a place rather than about the program. */
const ROOT = resolve(ENGINE, "..");

/* `--list-sources` answers WHAT THE PROGRAM IS and exits — check_recursion.sh shells back into this file to ask,
   and a list with build output in front of it is not a list.
   `--list-include-roots` answers WHERE ITS HEADERS COME FROM, for the same reason and to the same rule: the
   compiler is handed these roots and nobody else may restate them. gate_revision.mjs's dangling-include check
   had its own copy of the list — four roots, hand-written — and a second program's extra `-I` made that copy
   wrong the day it landed, so the check declared a revision that BUILDS to be one that "cannot be built by
   anyone who checks it out". A confident false red is worse than a silent miss: it is the phantom §Testing
   describes, and the next real dangling include arrives in a report nobody believes. The answer stays a LIST
   OF SOURCE SETS rather than a flat union even while there is one set, because that is what stops the check
   answering "fine" to a unit including a header its own compiler is never given — which is precisely the
   include it exists to catch — and a shape that degenerates the moment the program count drops to one is a
   shape that has to be rebuilt the moment it rises again. */
const LIST_SOURCES = process.argv.includes("--list-sources");
const LIST_INCLUDE_ROOTS = process.argv.includes("--list-include-roots");

const WORK = join(ENGINE, ".work");
const EMSDK = join(WORK, "emsdk");
const EMCC = join(EMSDK, "upstream", "emscripten", process.platform === "win32" ? "emcc.bat" : "emcc");

/* ASKED WHERE EMCC IS SPAWNED, NOT AT THE TOP OF THE FILE — the same fact the `--list-sources` header two
   screens down states about buildLexbor, one step further up. That header says "a mode whose whole contract is
   'answer and exit' had a compiler under it" and moved the COMPILE below both questions; the compiler's
   PRESENCE REQUIREMENT stayed above them, so the questions still could not be asked without the toolchain, and
   a requirement is as load-bearing as a call when it exits 1.
   Three things need no emscripten and all three were refused by it:
     * `--list-include-roots` and `--list-sources`, whose ENTIRE consumer is a lane running `clang
       -fsyntax-only` — the check CLAUDE.md §Testing requires of every change. A lane on a box with clang and no
       emsdk could not compute its own include roots, so the one verification a non-building lane owes was
       gated behind the toolchain for a host it was not touching.
     * the NATIVE target, which links `lexbor-native` with clang and never spawns EMCC at all. CLAUDE.md calls
       that host the engine's home — "real threads, a real CPU clock, a real `fork()` and a real sanitizer" —
       and the wasm host "ONE host among several"; requiring the one to build the other inverts that exactly,
       and it is also the host that carries ASan, which §Architecture prescribes BY NAME for memory crashes and
       which wasm32 cannot run.
   So the check is a function called at each spawn site. The message is unchanged and still says exactly what to
   run, which is the half worth keeping: "emcc not found" alone sends whoever hits it hunting for the version. */
function requireEmcc() {
  if (existsSync(EMCC)) return EMCC;
  console.error("[build] emcc not found at " + EMCC + "\n" +
                "[build] provision it with:\n" +
                "  git clone --depth 1 https://github.com/emscripten-core/emsdk.git " + EMSDK + "\n" +
                "  " + join(EMSDK, "emsdk") + " install latest && " + join(EMSDK, "emsdk") + " activate latest");
  process.exit(1);
}
mkdirSync(OUT, { recursive: true });
mkdirSync(EXT_QJS, { recursive: true });

/* ── Lexbor DOM (HTML5 parser + DOM + CSS selectors) ─────────────────────────────
   The moat runs the page's real bundle against a real DOM. Lexbor is pure C, compiles
   to wasm, and links in the same module as quickjs. It's slow to compile (213 files),
   so build it ONCE into a cached static archive (liblexbor.a) and link that; rebuilds
   of the engine (quickjs + main.c) then stay fast. Rebuild the archive with
   `node engine/build.mjs lexbor`. */
/* THE SOURCE IS TRACKED, so there is nothing to provision and nothing to pin. It lives at engine/lexbor,
   vendored at v3.0.0 in one commit with the fork's delta in the next, which is what makes `git diff` against
   that baseline the answer to "what did we change" — the property the engine/qjs submodule has and the reason
   this is worth a directory in the tree.
   WHAT THIS REPLACED, so the next reader does not rebuild it: a clone into .work/lexbor-src guarded by a tag
   comparison. The clone ran only when the directory was ABSENT, so on any machine that already had a checkout
   the tag was decoration — editing it changed the build's REPORT and not one byte of what compiled. The later
   pin check fixed that for a MOVED checkout and still could not see an EDITED one, because a working-tree edit
   leaves HEAD on the tag; the moment tree construction needed an embedder seam in html/tree.c, the fork existed
   and the pin said the source was pristine. A pin that cannot see the edit is not a weaker pin, it is a false
   statement, and the fix is not a better comparison but a source the repository holds.
   An older comment here warned that engine/lexbor/source was "a second, git-ignored location that is empty in
   a fresh container". That was true of an ignored path and is the opposite of true for a tracked one: a fresh
   container now has the source before it has a network. */
const LEXBOR_DIR = join(ENGINE, "lexbor");
const LEXBOR_SRC = join(LEXBOR_DIR, "source");
if (!existsSync(join(LEXBOR_SRC, "lexbor", "html", "html.h"))) {
  console.error("[build] lexbor source missing at " + LEXBOR_SRC);
  console.error("[build]   it is TRACKED, not fetched — a checkout without it is incomplete, so this is a");
  console.error("[build]   broken working tree rather than a step somebody forgot to run.");
  process.exit(1);
}
const LEXBOR_LIB = join(WORK, "liblexbor.o");   // relocatable partial-link object (emcc -o .a doesn't archive from .c)
const LEXBOR_INC = LEXBOR_SRC;
function findC(dir, out) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) { if (p.includes("windows_nt")) continue; findC(p, out); }  // posix port on emscripten
    else if (e.name.endsWith(".c")) out.push(p);
  }
  return out;
}
/* WHAT THE CACHED ARCHIVE WAS COMPILED FROM, asked of the SOURCE rather than of a version string. While lexbor
   was fetched by tag, "same tag" implied "same bytes" and presence was a sound cache key. A tracked source is
   EDITABLE — that is the whole point of vendoring it — so presence now means "some archive exists", which is
   the defect this build already carries a paragraph about one level up: a value read from one place and written
   in another with nothing asserting they agree. An edit to html/tree.c would relink the PREVIOUS objects and
   every gate would attribute the result to the edited revision.
   THE COMPUTATION IS engine/lexbor_source.mjs's, NOT THIS FILE'S, because it was this file's and the OTHER
   compiler in this tree never got it: engine/wpt.mjs cached its native archive on presence alone and stopped
   linking the day the fork grew a function. One archive-identity answer, imported by everything that caches an
   archive — a second copy of it is the same shape as the defect it removes. */
const LEXBOR_ID_FILE = join(WORK, "liblexbor.srcid");
function buildLexbor(force) {
  const id = lexborSourceId(LEXBOR_SRC);
  const cachedId = existsSync(LEXBOR_ID_FILE) ? readFileSync(LEXBOR_ID_FILE, "utf8").trim() : null;
  if (!force && existsSync(LEXBOR_LIB) && cachedId === id) return;
  if (existsSync(LEXBOR_LIB) && cachedId !== id)
    console.log("[build] lexbor source changed (" + (cachedId || "no id") + " -> " + id + ") — recompiling");
  const srcs = findC(join(LEXBOR_SRC, "lexbor"), []);
  console.log("[build] lexbor: compiling " + srcs.length + " sources -> liblexbor.a (once, ~minutes)");
  const rsp = join(WORK, "lexbor.rsp");
  const fwd = (s) => s.replace(/\\/g, "/");   // response-file backslashes are clang escapes -> forward-slash paths
  writeFileSync(rsp, [...srcs.map(fwd), "-I", fwd(LEXBOR_INC), "-O2", "-w", "-D_GNU_SOURCE", "-DENABLE_DUMPS", "-r", "-o", fwd(LEXBOR_LIB)].join("\n"));
  const r = spawnSync(requireEmcc(), ["@" + rsp], { stdio: "inherit", shell: true, cwd: ENGINE });
  if (r.status !== 0) { console.error("[build] lexbor FAILED rc=" + r.status); process.exit(r.status || 1); }
  /* STAMPED ONLY ON SUCCESS, and only after the archive exists: a failed compile that recorded the id would
     make the next build skip it and link whatever object was there before, which is the stale-link this
     mechanism exists to prevent, reached through its own cache. */
  writeFileSync(LEXBOR_ID_FILE, id + "\n");
  console.log("[build] lexbor OK -> " + LEXBOR_LIB + " (source " + id + ")");
}

// THE IDL GAP AUDIT IS A STAGE OF THIS BUILD — see the stage list at the bottom. It was in nobody's build,
// which is §Testing's excluded gate: its whole subject (does each component install the members its IDL
// declares) was unmeasured while the report ended in a complete-looking total. It is a stage on the SAME terms
// as every other one — it fails the run, it is not best-effort, and it is not skipped when @webref/idl is
// absent, because that package is a declared devDependency of this repo and a gate that skips itself when its
// input is missing is a gate that silently is not one.

// THE SOLVER CORE AND THE BROWSER HALF ARE BOTH IN THE PROGRAM, and both entries are BUILT. The note that
// stood here — "there is no qjs_* extension ABI entry yet" — was false in the way CLAUDE.md §DFAIL describes:
// main.c IS that entry, QJS_ABI below is checked against its own QJS_EXPORT bodies in both directions, and
// link() emits both artifacts (test_forced.c -> out/qjs.js, main.c -> extension/lib/qjs/qjs.mjs). Neither may
// be dropped from the compile: §Testing's "a translation unit no gate compiles is outside the gate" is exactly
// how the shipped ABI entry rotted the last time only one of the two was linked.
/* Every .c under a directory, sorted — the same rule engine/wpt.mjs uses to decide what the gate links, and
   for the same reason its comment gives: a list picked per component only ever describes what was needed the
   last time someone remembered to edit it. */
function walkC(dir, out = []) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) walkC(p, out);
    else if (e.name.endsWith(".c")) out.push(p);
  }
  return out.sort();
}

/* WHICH DOCUMENT THE SMOKE DRIVES. `min` selects test_forced.c's minimal clone/COW/verify fixture and its probe
   subset — the per-change MEMORY gate, seconds instead of minutes, and the one to pair with `sanitize` (that
   fixture exists precisely to avoid the full document's fork-tree blowup under a sanitizer).
   IT IS PASSED AS AN ARGUMENT, and that is the whole fix: the selection used to be getenv("APICLIENT_ASAN_MIN"),
   and emscripten's ENV is a fixed default set that never merges process.env — so the minimal document, its probe
   subset and every statement only it carried were unreachable in EVERY mode of this script. A fixture no mode of
   the build can run is an excluded test, and an excluded test is a failure (CLAUDE.md, Testing). */
const MIN = process.argv.includes("min");
const SOLVER = (f) => join(HOST, "solver", f);     // the Time-Travel Solver (the novel half)
/* THE TWO ENTRIES, NAMED — they are alternatives at the LINK and identical everywhere else, which is the whole
   reason both can be verified for the price of one. test_forced.c owns main() and runs on load; main.c owns the
   qjs_* ABI the bridge drives through ccall and must not run on load.
   THERE HAVE TWICE BEEN THREE, AND BOTH THIRDS ARE GONE FOR TWO DIFFERENT REASONS WORTH KEEPING APART. The
   first was a "trusted" program linked out of a subtree of THIS list so that MIME Sniffing §7 would not link
   into the renderer, and A SEPARATE LINK IS NOT A PROCESS BOUNDARY: every object here was offered to every
   link, both Modules instantiated in the offscreen's own realm with the host holding an exported HEAPU8 over
   each, and the "trusted" artifact was in fact the LARGER of the two. The second was a real second program —
   its own source list, its own objects, its own dedicated Worker — and it went because of WHAT WAS IN IT
   rather than how it was linked. It held WHATWG MIME Sniffing §7 and Chromium's CORB analyzer, which
   CLAUDE.md §Architecture rules belong in `extension/lib/safe-fetch.js` where SECURITY.md's threat model
   puts the CORB gate; and it held the RENDERER REGISTRY, which is a `Map` from an agent cluster key to an
   integer. Neither is what a FLOW needs mid-execution, which is this project's whole test for what the engine
   owns — and the registry is the component that arbitrates between renderers of DIFFERENT ORIGINS, so a
   memory-corruption bug in it is a cross-origin boundary failure and it is the last thing that should be C.
   It is `extension/render-process-host.js`. There are two programs and both are the renderer's. */
const ENTRY_SMOKE = join(HOST, "test_forced.c");
const ENTRY_ABI   = join(HOST, "main.c");

/* THE HEADER ROOTS THE COMPILER IS GIVEN, DECLARED ONCE. CFLAGS below is BUILT from these rather than spelling
   them again, and `--list-include-roots` reports them, so the compiler, the build and any checker are reading
   one statement.
   Include by FULL path from the host root — a browser component is "core/dom/dom_element.h", a solver component
   "solver/concolic.h" — so a cross-layer include always names its layer and no bare-name shortcut hides one. */
const ENGINE_INCLUDE_ROOTS = [QJS, HOST, join(HOST, "browser"), LEXBOR_INC];
const dashI = (roots) => roots.flatMap((r) => ["-I", r]);
const SHARED_SOURCES = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c"]
  .map((f) => join(QJS, f))
  .concat([
    /* EVERY BROWSER AND SOLVER SOURCE, WALKED — not a hand-picked list. The list that stood here named each
       component with a comment, and it was wrong in the way a hand list always becomes wrong: a component
       added and not added to it is simply NOT IN THE ENGINE. remote_object.c is what proved it — the file
       existed, compiled, was tested through the WPT runner (which walks the tree, and has for exactly this
       reason) and did not link into the shipped wasm, because one place knew about it and the other did not.
       The failure was a link error here, which is the lucky version; the unlucky version is a component whose
       symbols happen to be unreferenced and which silently ships absent.
       ORDER IS STABLE (sorted), so a build is reproducible and a diff of this list is a diff of the tree. */
    ...walkC(join(HOST, "solver")),
    ...walkC(join(HOST, "browser")),
  ]);
/* WHAT THE PROGRAM IS — BOTH entries, because both are compiled and both are linked. This used to be whichever
   single entry the `abi` argument selected, and that argument is gone: it chose which of the two programs a run
   produced, and a run now produces both, so nothing is left for it to select. check_recursion.sh shells in here
   for this list, and an answer naming one entry excluded the other from the recursion check too. */
/* DEDUPED, because check_recursion.sh reads this list to decide what to analyse and a file named twice is a
   unit analysed twice. The `new Set` earned itself when core/mime/mime_type.c was in two programs and one
   file; it stays because the property it guarantees is about this list and not about how many programs
   happen to exist. */
const sources = [...new Set(SHARED_SOURCES.concat([ENTRY_SMOKE, ENTRY_ABI]))];

/* WHAT THE PROGRAM IS, asked rather than copied. check_recursion.sh needs exactly this list — its header says
   "the unit list mirrors engine/build.mjs" — and it was a second copy that had drifted to a THIRD of it: every
   browser component (the DOM tree walks, the HTML serialiser, custom elements, fetch, Headers) was outside the
   check entirely, so its zero was a zero about quickjs and the solver and nothing else. A checker that covers a
   fraction is worse than none, which is what that script's own header warns; the fix is that there is one list
   and the checker reads it. */
if (LIST_SOURCES) {
  console.log(sources.join("\n"));
  process.exit(0);
}

/* THE HEADER ROOTS, PER SOURCE SET, EMITTED FROM THE ONE PLACE THAT HANDS THEM TO THE COMPILER. There is one
   set today and the shape is still a LIST OF SETS, which is not hedging: a set is a compiler invocation with
   its own `-I` list, so the day a second program exists again the answer is one more entry rather than a
   reader that has to learn a new shape — and a flat union, which is what a single set collapses to if anyone
   "simplifies" it, would answer "fine" to a unit including a header its own compiler is never given, which is
   exactly the include the consumer of this manifest exists to catch.
   The roots are repo-relative because the consumer resolves them against a git revision rather than a path on
   this disk, and they are derived from the same CFLAGS array the link uses rather than restated — this file
   may not hold a second copy either, or it becomes the thing it is fixing. */
if (LIST_INCLUDE_ROOTS) {
  const rel = (p) => relative(ROOT, p).split(sep).join("/");
  console.log(JSON.stringify([
    { name: "engine", roots: ENGINE_INCLUDE_ROOTS.map(rel), sources: sources.map(rel) },
  ], null, 1));
  process.exit(0);
}

/* THE FIRST COMPILE OF THIS RUN, AND IT SITS BELOW BOTH QUESTIONS BECAUSE A QUESTION MUST NOT BUILD ANYTHING.
   It sat ABOVE them, and the header two screens up already said why that is wrong — "a list with build output
   in front of it is not a list" — while this call put exactly that in front of both lists. It was not a style
   defect and it did not stay quiet:
     * `--list-include-roots` ANSWERED NON-JSON. buildLexbor prints its progress on stdout, so gate_revision's
       dangling-include check got `[build] lexbor: compiling …` where it expected a manifest and reported
       "THIS REVISION DOES NOT COMPILE — 1 include(s) below name a file no commit provides" on a clean tree, on
       every gate run. That is the confident false red the paragraph above `--list-sources` was written to
       prevent, produced by this line instead of by the copied list it replaced.
     * AND ASKING THE QUESTION RAN EMCC. A gate that only wanted to print its revision banner spent a full
       lexbor compile doing it, on the shared four-core box CLAUDE.md §Testing prices every build against.
   Both are one fact: a mode whose whole contract is "answer and exit" had a compiler under it. */

/* TAKEN AND SAID HERE: after BOTH question-answering modes have exited — a list with a revision block in front
   of it is not a list, and `--list-include-roots` emits JSON a consumer parses — and before the first compiler
   runs. Both halves matter. Taken early, so the end-of-build re-ask can tell whether the tree moved WHILE the
   reads were happening. Said early, so a build that dies before `report()` — an unhandled throw, a kill, a
   full disk — still names the tree it was reading. The block is printed again in the summary because that is
   the end a reader pastes. */
for (const l of revisionLines(revAtStart())) console.log(l);

/* THE NATIVE SMOKE TARGET, and the sanitized builds that are only possible on it.
 *
 *   node engine/build.mjs native            -> the smoke fixture as a native binary (the MEMORY series: its
 *                                              @HEAP/@PROGRESS stream is the only place this engine's live
 *                                              allocation is reported, and a wasm link per iteration is a tool
 *                                              nobody reaches for)
 *   node engine/build.mjs native leak       -> LeakSanitizer (which allocation is still live at exit)
 *   node engine/build.mjs native address    -> AddressSanitizer (UAF / double-free / overflow, leaks included)
 *   … plus `min` to drive the minimal fixture, as the wasm smoke takes it.
 *
 * It used to be a `-fsanitize=address` FLAG ON THE EMCC LINK, and that target could not run: wasm32 addresses
 * 4 GiB total, and this fixture's forced multi-path frontier measures 5.2 GiB of live allocation NATIVE and
 * unsanitized. `node engine/build.mjs asan` therefore aborted `Aborted(OOM)` inside the FIRST context switch,
 * with 46 KiB of JS heap on it — refusing to grow past 1.29 GB — and raising INITIAL_MEMORY only moved the wall
 * (MAXIMUM_MEMORY clamps it; lifting both reached 3.1 GiB still inside switch 1). A sanitizer target that cannot
 * reach switch two measures nothing, so the flag is DELETED rather than kept as a mode nobody can use.
 *
 * Native is not a workaround for that: it is where every other gate in this project already runs its C —
 * engine/wpt.mjs builds this same source list natively for the same reason (an eight-minute wasm link per
 * iteration is a gate nobody runs), and a native ASan run over that runner is what named the attribute-lifetime
 * SEGV in one go. The flags below are wpt.mjs's, with the SMOKE entry instead of the WPT one, and DEV on so a
 * DCHECK stays live — a sanitized build with the asserts compiled out reports faults the engine's own
 * invariants would have caught first, at the wrong site. */
const NATIVE = process.argv.includes("native");
if (NATIVE) {
  /* WHICH SANITIZER, IF ANY — named, because the plain native build is the one the memory series comes from and
     a sanitizer changes both the numbers and the wall-clock by an order of magnitude. */
  const kind = process.argv.includes("address") ? "address"
             : process.argv.includes("leak")    ? "leak" : "none";
  const bin = join(OUT, "qjs-native-" + kind);
  mkdirSync(OUT, { recursive: true });
  
/* LEXBOR, NATIVELY — the SAME CALL wpt.mjs makes, which is the whole of the fix for what stood here.
     THIS LINE USED TO BE `if (!existsSync(LEXBOR_NATIVE))` OVER A HARDCODED PATH, and it linked a lie. The
     paragraph two screens up says why presence is not a cache key for a TRACKED source, engine/wpt.mjs's own
     comment says the same thing about the same archive, and this arm — added after both — still took whatever
     `.a` happened to be in `.work/lexbor-native`, beside a `liblexbor_static.srcid` that recorded, in the same
     directory, that it had been compiled from something else.
     WHAT THAT LINKED IS THE CASE A LINK ERROR CANNOT CATCH. An ADDED function breaks the link and names itself;
     an EDITED STRUCT does not. This fork gave `struct lxb_selectors` a host-callback table (a `:defined` that
     the selector engine has to ask the host language about), so the header the host compiles against said 56
     bytes while the archive's `lxb_selectors_create` still callocated 40 — and `lxb_selectors_host_cb_set` is
     `lxb_inline`, so `dom_collect_scripts` wrote `host_ctx` one pointer PAST the allocation, into the following
     chunk's header. `document_bundle_id` runs on EVERY document, so the native ABI host aborted inside `free()`
     before writing a single line, three frames from the write, saying `free(): invalid pointer` and naming no
     cache at all. Every measurement taken with that binary was a measurement of a lexbor no revision contains.
     SO THE PATH IS NOT A CONSTANT HERE AND THERE IS NOTHING FOR THIS ARM TO CHECK: `lexborNativeArchive`
     returns an archive that is this source's or it does not return. It also BUILDS one rather than refusing and
     naming `node engine/wpt.mjs` — a division that put the recipe in one file and this file's correctness in a
     sentence a person had to read, which is exactly how an archive nobody owned sat stale for two days. */
  const LEXBOR_NATIVE = lexborNativeArchive(ENGINE, "build");
  /* The quiet list and -Werror=implicit-function-declaration are the SHIPPED build's, taken from the
     same place rather than restated, so the sanitized program is the program. */
  const quiet = QUIET_WARNINGS;
  /* THE DIALECT THIS TARGET COMPILES IN, NAMED ONCE. Both clang invocations below are the SAME target and must
     stay the same target, so the defines and include paths are one list rather than two that can drift — the
     hand-copied-list defect this file warns about elsewhere, in miniature. */
  const NATIVE_DIALECT = [
    ...quiet,
    "-D_GNU_SOURCE", "-DENABLE_DUMPS", '-DCONFIG_VERSION="native"', "-DAPICLIENT_DEV=1",
    "-Werror=implicit-function-declaration",
    "-I" + QJS, "-I" + HOST, "-I" + join(HOST, "browser"), "-I" + LEXBOR_INC,
  ];
  const cc = spawnSync("clang", [
    "-O1", "-g", "-fno-omit-frame-pointer",
    ...(kind === "none" ? [] : ["-fsanitize=" + kind]),
    ...NATIVE_DIALECT,
    /* BOTH ENTRIES. `main.c` owns the `qjs_*` ABI and has no `main()`, so it contributes no entry point and
       cannot collide with the fixture's — every other symbol in either file is `static`. What it contributes
       is the ABI ITSELF, which `test_forced.c`'s `--abi` arm is now a host of, so the two are one program:
       run with no arguments it is the fixture, run with `--abi` it drives a document through the same
       `qjs_*` entries the extension calls. (A COUNT stood here and was wrong by one before this line was even
       read — a number in prose that `QJS_ABI` answers is status, and the grep is the authority.)
       THIS SUPERSEDES A COMPILE-ONLY GATE THAT STOOD HERE and is deleted with it rather than kept beside it.
       That gate was `clang -fsyntax-only` over ENTRY_ABI, and its own comment gave the reason it existed:
       §Testing's "A TRANSLATION UNIT NO GATE COMPILES IS OUTSIDE THE GATE, AND THE SHIPPED ENTRY POINT IS THE
       ONE THAT ROTS" — this exact file having stopped compiling across many commits in which every gate was
       green. A LINK is strictly stronger than a syntax check and answers the same question plus the one the
       syntax check explicitly could not: syntax-only "catches a missing #include and a bad declaration and
       catches NOTHING about an undefined symbol", which is the emcc path's own sentence about why it links
       both. And the run below carries it further still — the shipped entry is now EXECUTED natively, under
       the sanitizers this target exists for, which is what CLAUDE.md means by the engine's home being the
       host with a real sanitizer. Keeping the weaker check beside the stronger one would be a second gate
       whose only possible contribution is to disagree. */
    ...SHARED_SOURCES, ENTRY_SMOKE, ENTRY_ABI, LEXBOR_NATIVE, "-o", bin, "-lm", "-lpthread",
  ], { stdio: "inherit" });
  if (cc.status !== 0) { console.error("[build] native build FAILED rc=" + cc.status); process.exit(cc.status || 1); }
  console.log("[build] OK -> " + bin + " (both entries: the fixture, and `--abi` over the shipped qjs_* ABI)");
  /* EVERY STAGE THIS TARGET RUNS, IN ONE LIST, because `report()` exits and a stage that reports alone is a
     stage that ends the run. This is the whole reason `cold` could not fall through to the native run. */
  const stages = [];
  /* THE CROSS-SESSION ROUND TRIP: TWO INVOCATIONS OVER ONE SHELF.
   *
   * §Time-travel-resume's whole claim is that the frontier persists as suspended snapshots ACROSS SESSIONS, and
   * until this target existed nothing in this tree could run both halves of it: the residue the engine writes
   * and the residue cold_resume reads were produced and consumed in different processes, and the only host that
   * held both ends was a browser with an IndexedDB. So the read half — the segment rebuild, park_unhex,
   * solve_resume_candidate, the probe address — had never executed in ANY process.
   * A SESSION BOUNDARY IS A PROCESS BOUNDARY, which is why this is two spawns and not one binary doing both. A
   * single process would leave the first session's endpoint surface, sink searches and world namespace standing
   * behind the second, so "the resumed session found it" and "the previous session had already found it" would
   * be the same observation and the round trip would prove nothing.
   * IT IS NOT A DRIVER. The shelf is a file, the resume is engine_sched_begin's own choice between a residue
   * and a boot flow, and everything between the two spawns is the store — which is exactly what the trusted
   * zone is to the shipped engine. */
  if (process.argv.includes("cold")) {
    const store = join(OUT, "park.recipes");
    /* THE SHELF IS EMPTY BEFORE SESSION ONE. A residue left by an earlier run of a DIFFERENT tree would resume
       flows standing on segments this build never wrote — and it would look like a pass. */
    rmSync(store, { force: true });
    const v1 = runChild("session ONE (--cold-park)", bin, ["--cold-park", store],
      "read its `@H park-*` rows beside the round-trip line below: a 0 kind names which record the park did " +
      "not write, and the moment it was taken at is `fixture_want_park` in engine/host/test_forced.c.");
    /* SESSION TWO IS SKIPPED AND NOT MERELY UNREPORTED. This is a real data dependency and not a door — the
       resume reads the residue session ONE writes, so with no residue there is nothing for it to be a test OF
       — and it is stated as a skip with that reason so the report never has a silent hole in it.
       THE DEPENDENCY IS THE RESIDUE, SO THE RESIDUE IS WHAT IS ASKED. This read `v1.code`, and an exit code is
       not that fact: it folds session ONE's six park rows — and every other probe in the fixture — into one
       door, so a run that wrote a full residue and failed a row named an arm it did not exercise was reported
       under a sentence claiming it had written nothing. That is the shape this file warns about elsewhere,
       three states behind one answer, with the printed REASON being the part that was false. Session ONE still
       FAILS on its own rows and the report below still fails with it; what changes is that the read half is no
       longer gated on the write half being perfect, which is the whole reason the round trip is two spawns. */
    /* READ RATHER THAN STAT'D, and not for want of `statSync`: cold.c's rule is that "an engine with no members
       writes no bytes at all", so ABSENT and EMPTY are the same non-residue and a size is one of the two ways to
       ask. Reading it asks both at once and costs nothing at this size — the document is a few hundred bytes. */
    const residue = existsSync(store) && readFileSync(store, "utf8").trim().length > 0;
    const v2 = !residue
      ? skipped("session TWO (--cold-resume)", "session ONE wrote no residue for it to resume from")
      : runChild("session TWO (--cold-resume)", bin, ["--cold-resume", store],
                 "the round-trip line below says what it rebuilt out of the residue; a kind session one " +
                 "wrote and this one did not rebuild is the arm to look at.");
    /* AND THE ROUND TRIP IS REPORTED RATHER THAN HINTED AT, which is this file's own recorded lesson applied
       to the last two places that had not had it. The hints these replace "named the mechanism, named the
       obstacle, and left the reporter printing an instruction to a human about numbers the reporter itself
       could have read" — and worse, they printed only on a NON-PASS, so the round trip that WORKED said
       nothing at all and `orphansUnmet`, the round trip's own verdict, has never been read by anything. */
    if (!v1.code && !v2.code) console.log("[build] cold round trip (" + kind + ") — " +
                                          coldRoundTrip(v1, v2, store) + " — residue at " + store);
    /* COLLECTED, NOT REPORTED, and the difference is a whole stage. `report()` ALWAYS exits — both arms end in
       `process.exit` — so reporting here ended the run, and the native run below was UNREACHABLE FROM `cold`.
       Not merely on a red cold round trip: on every invocation of it, green included. So `node engine/build.mjs
       native cold` has never once run the fixture, and the paragraph immediately below is the one that says why
       that is the defect — "a target that is only built is the excluded test one layer down". The two stages
       were written months apart and the second never noticed the first could not fall through to it.
       IT COST A REAL MEASUREMENT: a lane converted 45 probe rows to record-scoped clauses and asked for a run
       of the fixture to confirm none of them had become a term that can only ever read 0 — the defect that
       fixture records having had three times — and `native cold` answered with a cold verdict and no fixture at
       all. The two are INDEPENDENT questions about one binary, so they are two stages of one report, and a red
       cold round trip must not decide whether the fixture is exercised. */
    stages.push(v1, v2);
  }
  /* AND IT IS RUN, because a target that is only built is the excluded test one layer down: the whole point is
     the stream it prints and the report it ends with, and nothing else in the tree produces either. */
  stages.push(runChild("the native run (" + kind + (MIN ? ", minimal document" : "") + ")", bin, MIN ? ["--min"] : [],
                       "a LeakSanitizer summary above is a real leak, and an AddressSanitizer report a real fault"));
  report(stages);
}

/* THE WASM LEXBOR, AND IT SITS BELOW `native` FOR THE REASON THE HEADER TWO SCREENS UP ALREADY GIVES.
   That header records this same call being moved below `--list-sources` and `--list-include-roots` because "a
   mode whose whole contract is 'answer and exit' had a compiler under it". `native` is the third such mode and
   was missed: it links LEXBOR_NATIVE out of WORK/lexbor-native, names no emscripten anything, and ends in a
   `report()` that always exits — so it never reached the emcc link below and never wanted this archive. It
   still could not START without emsdk, because this line built lexbor TO WASM before the branch was tested.
   The symptom was the one that rule predicts: `node engine/build.mjs native cold`, on a machine with a native
   archive sitting ready, died on `emcc not found` after a full 213-source lexbor compile — an emscripten
   dependency reported as the native target's, at a site the native target does not use.
   It is the same defect as the lazy `requireEmcc()` below and not a repeat of that fix: that one made the
   toolchain CHECK lazy, and this line ran the toolchain regardless of whether the check was ever consulted.
   The invariant that now holds is one sentence — NOTHING COMPILES ABOVE THE BRANCH THAT DECIDES WHAT TO
   COMPILE — and every target that exits on its own is above it. */
buildLexbor(process.argv[2] === "lexbor");
if (process.argv[2] === "lexbor") { console.log("[build] lexbor archive rebuilt; re-run without arg to build the engine."); process.exit(0); }

/* THE EXPORTS THE BRIDGE ccalls — and the previous sentence here ("emscripten drops anything not named") is
   DELETED, because it is not the mechanism and stating a wrong one is how a check gets skipped as redundant.
   `QJS_EXPORT` is `EMSCRIPTEN_KEEPALIVE`, which is `__attribute__((used))`, and EXPORT_KEEPALIVE defaults to 1
   outside MINIMAL_RUNTIME — so an entry left off this list may well reach `Module` anyway, by the accident of a
   default that another setting flips. AN ABI REACHED BY ACCIDENT IS NOT AN ABI: what this list decides is
   `--export=` on wasm-ld and `Module._x` on purpose, and the entry that is only there when the toolchain feels
   like it is the entry that vanishes in the build nobody re-checked. So the list is ENFORCED rather than
   asserted about — `qjs_perform` and `qjs_host_answer_remote` (the peer's half of the cross-instance seam, the
   only entries by which one instance is ASKED to perform another's operation) were written, linked and left
   off it, and no gate said a word. */
const QJS_ABI = ["qjs_init", "qjs_join", "qjs_unload", "qjs_bundle_id", "qjs_begin", "qjs_step",
                 "qjs_result", "qjs_teardown",
                 "qjs_pending", "qjs_provide", "qjs_decline", "qjs_top_weight", "qjs_set_yield_floor",
                 "qjs_request_park", "qjs_emit_partial",
                 "qjs_host_requests", "qjs_host_answer", "qjs_host_notices", "qjs_route",
                 "qjs_set_referenced", "qjs_perform", "qjs_host_answer_remote", "qjs_world_gone"];

/* THE LIST IS THE ABI, SO THE ENTRY POINT AND THE LIST ARE ONE FACT AND ARE CHECKED AGAINST EACH OTHER. Both
   directions are a real defect and neither has a symptom at build time: an entry main.c defines and this omits
   is a capability the extension cannot call (or can, until a setting changes); a name here that main.c does not
   define is `--export=` of a symbol that does not exist, which wasm-ld reports as an undefined export only
   because ERROR_ON_UNDEFINED_SYMBOLS happens to be on. Read from the source rather than restated: `QJS_EXPORT`
   is exactly the marker main.c puts on every ABI body.
   IT IS A HELPER WITH ONE CALLER, WHICH IT HAS BEEN TWICE BEFORE AND IN OPPOSITE DIRECTIONS. It was inlined
   once on the argument that "a helper kept for a caller that no longer exists is scaffolding", then made a
   helper again when a second program with its own entry arrived, and that program is deleted. It stays a
   helper because the argument for inlining it was wrong even when it was true: a second copy of these lines is
   the hand-maintained list this file spends its length warning about, and the shape it fails in is silence —
   an entry added to one program's list with the copy for the other left unedited exports nothing and says
   nothing. A function whose parameters are exactly the four facts that differ per program costs nothing to
   keep and is what makes the next program's check one line rather than a transcription. */
function abiCheck(program, entrySrc, marker, prefix, list) {
  const src = readFileSync(entrySrc, "utf8");
  const re = new RegExp(marker + "\\s+[\\w \\t*]+?\\b(" + prefix + "\\w+)\\s*\\(", "g");
  const defined = [...new Set([...src.matchAll(re)].map((m) => m[1]))];
  const missing = defined.filter((f) => !list.includes(f));
  const phantom = list.filter((f) => !defined.includes(f));
  if (missing.length || phantom.length) {
    console.error("[build] the " + program + " ABI list and its entry's " + marker + " bodies disagree — the\n" +
                  "[build] list IS the ABI, so a disagreement is either an entry nothing can call or an export\n" +
                  "[build] of nothing (" + entrySrc + "):\n" +
                  (missing.length ? "[build]   defined in the entry, missing from the list: " + missing.join(", ") + "\n" : "") +
                  (phantom.length ? "[build]   named in the list, defined nowhere:          " + phantom.join(", ") + "\n" : ""));
    return { label: program + " ABI list", verdict: "FAILED (list vs " + marker + " bodies)", code: 1 };
  }
  return { label: program + " ABI list", verdict: "PASS", code: 0 };
}
/* IT FAILS THE RUN AND IT IS NOT A DOOR EITHER. This check is about ONE program's export list, and it used to
   exit before anything was compiled — so a name added to main.c and not to QJS_ABI took the SMOKE gate, which
   has no export list and does not know main.c exists, out of the run with it. It reports its own verdict; what
   it withholds is the one thing it is actually about, the ABI link (an `--export=` of a name defined nowhere is
   what wasm-ld would report, less clearly and only by the accident of ERROR_ON_UNDEFINED_SYMBOLS). */
const ABI_LIST = abiCheck("renderer", join(HOST, "main.c"), "QJS_EXPORT", "qjs_", QJS_ABI);
/* A SECOND ABI LIST STOOD HERE, five entries of a second program, and it is deleted with that program. What
   it exported was the RENDERER REGISTRY: which agent clusters have an instance, what routing id each was
   given, and the refusal of a second instance for one cluster. That is `extension/render-process-host.js`
   again, in the trusted zone, in JavaScript, because the component that arbitrates between renderers of
   DIFFERENT ORIGINS is the one where a memory bug is a cross-origin boundary failure — and because a `Map`
   from a string to an integer is not what a FLOW needs mid-execution, which is this build's test for what the
   engine owns. There is one ABI list again and it is the renderer's. */

/* COMPILE FLAGS AND LINK FLAGS ARE SEPARATED, and that separation is what lets both entries be verified.
   They used to be one list handed to one emcc invocation that compiled and linked together, which forced two
   things: every build recompiled all 130-odd sources from scratch, and only ONE entry could be produced per
   run. The consequence was that the shipped ABI entry — the one the extension actually loads — was never
   LINKED by any default gate. It was compiled to a throwaway object, which catches a missing #include and a
   bad declaration and catches NOTHING about an undefined symbol; a qjs_* body calling a function deleted from
   the tree passed that check and would have failed only in the extension. §Testing's rule is that the shipped
   entry is the one that rots, and half a check is how it rots quietly.
   The two programs differ ONLY in which ENTRY OBJECTS enter the link, so compiling once into shared objects
   and linking twice costs one extra link and closes that hole completely.
   ONE OF THEM TAKES BOTH, AND THAT IS NOT A BLURRING OF THE TWO. `main.c` has no `main()`, so it is an entry
   only in the sense of owning the `qjs_*` bodies — and `test_forced.c` is now a HOST of those bodies (its
   `--abi` arm drives a document through them), so the smoke program needs them the way it needs any other
   component. The ABI program does NOT take the smoke object in return: `test_forced.c` owns `main()` and the
   whole fixture graph behind it, and neither belongs in the artifact the extension loads. */
/* THE WARNING LIST, ONE COPY. Two existed, and the comment above the second said it was "taken from the same
   place rather than restated" while restating it — a second copy of a build's configuration, which is the same
   defect as a second copy of its source list. Turning one diagnostic back on had to be done twice or the
   sanitized program stops being the program.
   `-Wformat-truncation` IS NOT IN IT, DELIBERATELY. It was, and it hid a real defect: a lane's new DFAIL wrote
   526 bytes into 320, which is silent truncation of the one mechanism this project uses to say what to build —
   a crash that names less than it knows. The diagnostic caught it and this build had switched it off. Measured
   before removing it rather than argued: at the level `-Wall` gives, ONE warning across 40 host translation
   units, plus three in url.c that are all `u->port` (max 65535) into a 7-byte buffer where the compiler cannot
   see the range. That is the whole cost. The rest of the list stays quiet for the reason below. */
const CFLAGS = [
  ...dashI(ENGINE_INCLUDE_ROOTS),   // declared once beside the source sets; see ENGINE_INCLUDE_ROOTS
  /* -Werror ON IMPLICIT DECLARATIONS, and the reason `-w` is NOT here beside it. A missing #include makes C
     assume `int (...)`, so a returned 64-bit POINTER comes back TRUNCATED — a segfault with no diagnostic, and
     it happened: window.c called window_proxy_name without its header and the whole corpus segfaulted inside
     strcmp. This line used to read `-w -Werror=implicit-function-declaration`, which does NOT work: `-w`
     suppresses the diagnostic outright, so the -Werror= promotion has nothing left to promote. The quiet list
     is explicit for that reason, and the moment it went in the gate found three more missing includes. */
  "-O1", ...QUIET_WARNINGS,
  "-D_GNU_SOURCE", "-DENABLE_DUMPS",
  /* `__FILE__` IS REPO-RELATIVE, AND THAT IS A CORRECTNESS FLAG AND NOT A TIDINESS ONE. Every source reaches
     the compiler by absolute path, so without this the string that bakes into an object — the one a DCHECK
     prints as `@WHY <cond> at <file>:<line>` — is the absolute path of the directory the build happened to run
     in. §Testing runs gates from a frozen snapshot that is deleted afterwards, so the file every abort named
     was a path that no longer exists, in a tree no revision contains; `engine/host/browser/core/dom/x.c:123` is
     an answer the reader can open. It is also what makes an object PATH-INDEPENDENT — measured: two checkouts
     of one revision emit BYTE-IDENTICAL objects with this flag and differ without it — which is the property
     the content-addressed object names below rely on to be shareable rather than merely equal-looking.
     It rewrites `__FILE__` and debug paths only; the -MMD dependency list keeps real absolute paths, which is
     what the identity code below re-roots for itself. */
  "-ffile-prefix-map=" + ROOT + "/=",
  // Offensive-programming build mode (check.h): DEV (default) keeps every DCHECK live so a should-never-happen
  // aborts LOUD at its origin; a `release` arg compiles them out (the release exemption — the user is not
  // crashed on an unsupportable state). CHECK (OOM/security) stays fatal in both.
  "-DAPICLIENT_DEV=" + (process.argv.includes("release") ? "0" : "1"),
];

const LDFLAGS_COMMON = [
  LEXBOR_LIB,                 // link the cached Lexbor DOM archive
  // Opt-in `assert` build: emscripten ASSERTIONS=2 turns a bare terse `Aborted()` into an INFORMATIVE crash
  // (the failing C assert + file:line — e.g. a refcount/gc_obj_list leak), the offensive-programming ideal of a
  // LOUD *and* diagnosable dev failure. Off by default so normal dev builds stay fast; enable when debugging.
  ...(process.argv.includes("assert") ? ["-sASSERTIONS=2"] : []),
  /* THE ARCHITECTURE'S CEILING, not a budget. wasm32 addresses 4 GiB and emscripten stops the heap at 2 GiB
     unless told otherwise, so the growth flag alone was a 2 GiB cap wearing the word "growth". The smoke
     fixture's forced multi-path run measures gigabytes of live allocation — the frontier holds every flow's COW
     delta in RAM because the smoke has no IDB cold tier to page the low-value tail into — so the 2 GiB stop was
     already the thing about to fail. Raising it to what the address space actually holds is the platform floor;
     the real answer for a frontier this size is the cold tier, which is the scheduler's work and not a flag.
     THE SANITIZED TARGET IS NOT HERE, and that is measured rather than assumed — see `sanitize` above: a wasm
     ASan link cannot reach the second context switch inside 4 GiB, so the flag that used to sit on this line
     was a mode that had never run. */
  "-sALLOW_MEMORY_GROWTH=1", "-sMAXIMUM_MEMORY=4294967296",
  "-sSTACK_SIZE=8388608",
  /* THE MODULE NAMES ITS OWN FUNCTIONS, so a stack is readable without a second instrument. `wasm-ld` strips
     the name section by default — emscripten's own `building.py` says so in its own words, "wasm-ld can strip
     debug info for us. this strips both the Names section and DWARF, so we can only use it when we don't need
     any of those things" — and this build set nothing that turns that off, so every shipped `qjs.wasm` carried
     exactly ONE custom section (`target_features`, 148 bytes) and every abort printed `wasm-function[7391]`.
     THAT IS WHAT MADE FIVE CONSECUTIVE LAYOUT ABORTS COST A LANE EACH: with no subject in the frame list, the
     asking function was all anyone had, and four of the five next-abort predictions made from it named the
     wrong box. An index is an ordinal into a link, so it is also the §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-
     SET-IS-FIXED defect wearing a stack trace: the same number means a different function in the next build,
     and it means a different function in the ABI binary than in the smoke binary of the SAME build (measured:
     all 17 frames of one stack land inside their bodies in one and outside them in the other).
     `--profiling-funcs` sets EMIT_NAME_SECTION and NOTHING else — it does not raise the debug level, so no
     codegen changes and the emitted code is the same program. It costs SIZE only, and the size is MEASURED
     rather than estimated, because a cost carried as a range is a cost nobody can weigh: two links of ONE
     revision, every other input shared, gave 14991347 bytes without and 15252790 with — +261443, or +1.74%,
     of which the `name` section is 254334. Re-measure it the same way if it ever needs re-arguing; do not
     quote this number against a later tree, which is what §Testing means about a result belonging to the
     revision it was taken at. This is the one flag that RETIRES an instrument rather than adding one, which
     is why it belongs on the default link and not behind an opt-in arg: a symbolizer reached by REMEMBERING
     to pass a flag is a symbolizer nobody has at the moment the crash lands. */
  "--profiling-funcs",
];
// The smoke entry RUNS on load and exits with the @H/@S pass code; the ABI entry is driven by the bridge
// through ccall, so its runtime must stay alive across qjs_step re-entries and be importable as an ES module.
const LDFLAGS_SMOKE = ["-sEXIT_RUNTIME=1"];
const LDFLAGS_ABI = [
  "-sEXPORTED_FUNCTIONS=" + JSON.stringify(QJS_ABI.map((f) => "_" + f).concat(["_malloc", "_free"])),
  /* HEAPU8 IS AN EXPORT, and it is the one every byte that crosses this ABI travels through. Fetch §2.2.5's
     body is a BYTE SEQUENCE, so the zone places it with `M.HEAPU8.set(bytes, p)` and hands `qjs_provide`
     a (ptr, len) — text cannot carry one and every way of making it able to is an encode by a zone with
     no business performing one. Emscripten does not export the heap views by default and answers a read
     of an unexported one by ABORTING the module, so omitting it here is not a missing convenience, it is
     the whole byte path failing at its first use. */
  "-sEXPORTED_RUNTIME_METHODS=" + JSON.stringify(["ccall", "lengthBytesUTF8", "stringToUTF8", "HEAPU8"]),
  "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createQJS", "-sINVOKE_RUN=0",
];

/* ── OBJECTS, NAMED BY WHAT PRODUCED THEM ─────────────────────────────────────────────────────────────────────
   AN OBJECT'S FILENAME IS THE HASH OF EVERYTHING THAT DECIDES ITS BYTES: the flags, the toolchain, the source,
   and every header the compiler recorded itself as having read. Nothing below compares a timestamp, so nothing
   below can be wrong about one, and "is this object stale?" stops being a question anyone has to answer
   correctly — a stale object simply has a different NAME from the one being asked for.
   WHERE THE HEADER LIST COMES FROM IS UNCHANGED AND IS STILL THE POINT: a cache that misses a header edit is
   worse than no cache, because it reports a stale binary as a fresh one, so the list is not hand-rolled —
   clang emits the real one with -MMD and this reads it back. What changed is that the list is HASHED rather
   than stat'd, which is the same guarantee with no clock, no ordering assumption, and no `touch` that costs
   twenty minutes of compiling.
   THE PATH IS NOT THE IDENTITY, AND THAT IS WHY THIS WAS THE PROJECT'S RATE LIMITER. The key used to be the
   source's ABSOLUTE path. §Testing requires a gate to run from a FROZEN SNAPSHOT, so every measured build
   happens in a checkout that has never existed before — and the same file at a new absolute path was a new
   cache entry, so every one of those builds compiled all ~295 translation units from cold. A content name is
   the same name in any checkout at any path, so an obj/ directory copied or shared between snapshots HITS
   exactly when the content genuinely matches, and MISSES exactly when it does not.
   AND MERELY MAKING THE KEY RELATIVE IS THE TRAP, NOT THE FIX: the .d files clang writes record ABSOLUTE header
   paths, so a relative key plus a copied obj/ would resolve every dependency into the DONOR tree — where those
   headers still exist, with older mtimes — and report FRESH for an object compiled against a different tree's
   headers. That is the stale-binary-reported-fresh failure with an extra tree in it. Every dependency is
   therefore RE-ROOTED before it is recorded (`repo:<path-from-the-checkout-root>`); an out-of-tree toolchain
   header keeps its absolute path, because that IS one file for every checkout on this machine; and either way
   it is hashed by CONTENT, so which tree it was read from cannot matter. */
const OBJDIR = join(WORK, "obj");
mkdirSync(OBJDIR, { recursive: true });
/* THE FLAGS ARE PART OF THE OBJECT'S NAME, NOT A THING THE CACHE COMPARES. The comment above says a cache
   that misses a header edit reports a stale binary as a fresh one; this cache once missed a FLAG edit, which is
   the same defect with a wider blast radius. `CFLAGS` carries `-DAPICLIENT_DEV=0` under `release` and `=1`
   otherwise — the switch that compiles out every DCHECK — and the cache compared only mtimes, so
   `node engine/build.mjs release` after a dev build found every object fresh, relinked the DEV objects, and
   reported a green release build of a program that was never built. That is §Testing's "a number about
   NOTHING" in the build system itself, and it is why release mode had to be verified with `-fsyntax-only`
   rather than by running the target.
   Hashing the flags into the NAME rather than storing them for comparison is what makes the failure
   impossible instead of detected: two flag sets are two files, so neither can masquerade as the other and
   both stay cached across switches. */
/* WHERE THIS TREE HAPPENS TO SIT IS NOT PART OF ANY IDENTITY — the one rule, applied to every string that goes
   into a name. `CFLAGS` carries four absolute `-I` roots and an absolute -ffile-prefix-map, and `emcc -v` names
   an absolute InstalledDir; each of those changes in every frozen snapshot while naming the same headers and
   the same compiler, so leaving any of them in re-creates the absolute-path cache key one level up and every
   snapshot goes cold for a reason that is not about the program.
   Measured rather than assumed: the `-I` roots alone made two checkouts of ONE revision share nothing, and the
   run that seemed to prove the cache worked had in fact only reused its own earlier run in the same tree. */
/* THE TOOLCHAIN IS ASKED FOR ITS VERSION, NOT POINTED AT. The line this replaced hashed the STRING `EMCC` under
   a comment claiming "the compiler binary is in the hash for the same reason a header is" — and it was not:
   emsdk upgrades IN PLACE, so one path names two compilers that emit different objects, and the claim was a
   false statement of exactly the kind a cache comment must not make. `emcc -v` costs ~0.2 s once per build,
   names both the emscripten and the clang commit, and goes through `unroot` like every other string here. */
const unroot = (s) => s.split(EMSDK).join("<emsdk>").split(ROOT).join("<root>");
const TOOLCHAIN_ID = (() => {
  const v = spawnSync(requireEmcc(), ["-v"], { encoding: "utf8" });
  const text = unroot(((v.stdout || "") + (v.stderr || "")).split("\n")
                        .filter((l) => !l.startsWith("InstalledDir")).join("\n"));
  if (!/^emcc \(/m.test(text)) {
    console.error("[build] `emcc -v` did not report a version (rc=" + v.status + ")\n" +
                  "[build]   an object may not be named for a toolchain this cannot identify — a name that\n" +
                  "[build]   does not change when the compiler does is how a stale object is reported fresh.");
    process.exit(1);
  }
  return text;
})();
const FLAG_ID = createHash("sha256")
  .update(TOOLCHAIN_ID + "\0" + unroot(CFLAGS.join("\0"))).digest("hex").slice(0, 12);

/* ONE READ PER FILE PER BUILD, NOT PER TRANSLATION UNIT — check.h is in nearly every dependency list and is
   hashed once. That is the whole of the cost control: what gets read is the unique file SET, not the ~2900
   (source, header) pairs that name it. */
const fileHashes = new Map();
function fileHash(abs) {
  if (!fileHashes.has(abs))
    /* ABSENT IS A POSITIVE ANSWER, not a hole to fill: a recorded dependency that is gone means this source has
       no identity in this tree, which the one caller turns into a compile. */
    fileHashes.set(abs, existsSync(abs) ? createHash("sha256").update(readFileSync(abs)).digest("hex") : null);
  return fileHashes.get(abs);
}
const depName = (p) => {
  const abs = resolve(p), r = relative(ROOT, abs);
  return (r && !r.startsWith("..") && !isAbsolute(r)) ? "repo:" + r.split(sep).join("/") : "abs:" + abs;
};
const depFile = (d) => (d.startsWith("repo:") ? join(ROOT, d.slice(5)) : d.slice(4));

/* WHAT CLANG RECORDED IT READ. The first token is the `<object>:` target, which is why it is dropped; every
   remaining token is a real input and the source itself is among them. Sorted, so the name does not depend on
   the order a compiler happened to emit — a reordering would otherwise cost a recompile and nothing else. */
function parseDepFile(dFile) {
  if (!existsSync(dFile)) return null;   /* no recorded deps is not "no deps" — it is no information */
  const toks = readFileSync(dFile, "utf8")
                 .replace(/\\\r?\n/g, " ").trim().split(/\s+/).slice(1).filter(Boolean);
  return toks.length ? [...new Set(toks.map(depName))].sort() : null;
}

/* THE NAME. Null when the list names a file this tree does not have — that is "no identity", and it is never a
   name computed from the shorter list that remains: a name is only ever the hash of a COMPLETE set. */
function contentId(deps) {
  const h = createHash("sha256").update("apiclient-obj-v1\0" + FLAG_ID);
  for (const d of deps) {
    const fh = fileHash(depFile(d));
    if (fh === null) return null;
    h.update("\0" + d + "\0" + fh);
  }
  return h.digest("hex");
}
const objFor = (id) => join(OBJDIR, id + ".o");

/* THE DEPENDENCY RECORD IS A HINT AND THE OBJECT NAME IS A FACT — the invariant the whole scheme rests on, and
   the answer to "can a stale record produce a false HIT?".
   The record says which files to hash. It is keyed by (source path, flags) because that is what is known BEFORE
   a compile; it can be some other tree's answer; and being wrong makes the computed name MISS.
   The name is written only by `adopt` below, only after a compile that had just observed its OWN COMPLETE
   dependency list. So `<id>.o` existing means: some compile, with these flags and this toolchain, over a source
   and headers whose bytes hash to `id`, produced it — and a compiler's output is a function of exactly those
   inputs, so that object IS the object being asked for. Worked through: a hint that omits a header h2 makes the
   computed name H(src, h1) for a source that really reads {h1, h2}; no compile of that source ever recorded a
   list without h2, so nothing was ever published under that name, and the lookup misses. A wrong hint therefore
   costs a needless recompile and can cost nothing else, which is the only failure mode a memo in a build system
   may have.
   The record's first line is its SUBJECT, checked on read, so a record can never be spent on another source. */
const depRecord = (src) => join(OBJDIR, createHash("sha256")
  .update("apiclient-deps-v1\0" + FLAG_ID + "\0" + depName(src)).digest("hex").slice(0, 32) + ".deps");
function recordedDeps(src) {
  const f = depRecord(src);
  if (!existsSync(f)) return null;
  const lines = readFileSync(f, "utf8").split("\n").filter(Boolean);
  return lines.length > 1 && lines[0] === depName(src) ? lines.slice(1) : null;
}

/* Both entries are compiled every time, because both are LINKED every time. */
const TO_COMPILE = SHARED_SOURCES.concat([ENTRY_SMOKE, ENTRY_ABI]);
/* src -> the object that IS this source at this content. Absent means "compile it", and that is also the only
   thing a NEVER-COMPILED source can mean: its header list does not exist until a compile writes one, so there
   is nothing to hash and NO name to invent. Naming it from the source bytes alone is the false hit this whole
   arrangement exists to make impossible — such a name does not change when a header does, so the SECOND build
   would compute the same name and HIT an object compiled against headers that have since been edited. A source
   with no identity is therefore given none: it compiles to a private temporary and is named afterwards. */
const OBJ_OF = new Map();
const IDENTITY_T0 = Date.now();
for (const src of TO_COMPILE) {
  const deps = recordedDeps(src);
  const id = deps && contentId(deps);
  if (id && existsSync(objFor(id))) OBJ_OF.set(src, objFor(id));
}
const IDENTITY_MS = Date.now() - IDENTITY_T0;
const stale = TO_COMPILE.filter((s) => !OBJ_OF.has(s));
console.log("[build] " + TO_COMPILE.length + " sources, " + stale.length + " to compile" +
            (stale.length < TO_COMPILE.length ? " (rest cached)" : "") +
            " [identity " + IDENTITY_MS + " ms over " + fileHashes.size + " files]");

if (stale.length) {
  /* IN PARALLEL, bounded by the cores actually present. A cold build is every translation unit in the program
     and the machine is otherwise idle while each one runs. */
  const JOBS = Math.max(1, cpus().length - 1);
  let next = 0, failed = 0, running = 0, tmpSeq = 0;
  await new Promise((done) => {
    const pump = () => {
      while (running < JOBS && next < stale.length) {
        const src = stale[next++];
        /* A PRIVATE TEMPORARY, because the name this object will carry is not known until it has been compiled
           and has said what it read. The pid keeps two concurrent builds out of each other's way; the final
           name they both arrive at is the same one, and arriving there is a rename, which is atomic. */
        const tmp = join(OBJDIR, ".tmp-" + process.pid + "-" + tmpSeq++);
        running++;
        const p = spawn(requireEmcc(), [...CFLAGS, "-MMD", "-MF", tmp + ".d", "-c", src, "-o", tmp + ".o"],
                        { stdio: "inherit", shell: true, cwd: QJS });
        /* THE NAMING, AND IT IS THE ONLY PLACE AN OBJECT EVER GETS A NAME. It runs on a compile that exited 0,
           over the dependency list THAT compile just wrote, so the invariant the lookup rests on holds by
           construction: `<id>.o` exists only where `id` is the hash of a complete, observed input set. Every
           way of not knowing that set is a failure here rather than a shorter hash — a name computed from part
           of the inputs is precisely the stale-object-reported-fresh defect, arrived at from the other end. */
        const adopt = () => {
          const deps = parseDepFile(tmp + ".d");
          if (!deps)
            return "the compiler exited 0 and recorded no dependency list, so this object cannot be named — "
                 + "naming it from its source alone would give it a name that does not change when a header does";
          if (!deps.includes(depName(src))) return "the recorded dependency list does not name the source itself";
          const id = contentId(deps);
          if (!id) return "a file this compile had just read is already gone";
          renameSync(tmp + ".o", objFor(id));            /* the FACT, published atomically */
          writeFileSync(depRecord(src), [depName(src), ...deps].join("\n") + "\n");   /* the HINT, after it */
          rmSync(tmp + ".d", { force: true });
          OBJ_OF.set(src, objFor(id));
          return null;
        };
        let settled = false;
        const settle = (why) => {          /* exit and error are not mutually exclusive; the pump must run once */
          if (settled) return;
          settled = true;
          if (why) {
            failed++;
            /* NOTHING HALF-NAMED SURVIVES A FAILURE. A temporary left behind is an object with no identity, and
               an obj/ directory that accumulates those is a directory whose contents stop meaning anything. */
            rmSync(tmp + ".o", { force: true });
            rmSync(tmp + ".d", { force: true });
            console.error("[build] FAILED " + src + " — " + why);
          }
          running--;
          if (next >= stale.length && running === 0) done();
          else pump();
        };
        p.on("exit", (code) => settle(code === 0 ? adopt() : "compiler exited " + code));
        /* A SPAWN THAT NEVER STARTS MUST BE A FAILED TU, NOT AN UNHANDLED THROW. With no `error` listener node
           raises the event as an exception, so the build died mid-run with no line naming a source — and
           `running--` never ran either, so the promise it was inside could not have settled had the throw been
           caught. Observed twice under fork pressure as `spawn /bin/sh ENOENT`: the machine was saturated, not
           the code wrong, and the build reported neither. That is the loaded-machine defect §Testing names —
           an artifact of HOW it ran presented as a fact about WHAT ran — so it is reported as what it is, with
           the source named. */
        p.on("error", (e) => settle("could not start the compiler: " + e.message));
      }
      if (next >= stale.length && running === 0) done();
    };
    pump();
  });
  if (failed) { console.error("[build] FAILED — " + failed + " source(s) did not compile"); process.exit(1); }
}

/* THE LINK ASKS FOR AN OBJECT BY SOURCE AND IS ANSWERED OR THE BUILD STOPS. Every source either had a name at
   the top of this section or was compiled and named by `adopt`, so an absent entry here is not a link that will
   be short one object — it is a source that reached the link with no compiled identity at all, which can only
   mean the compile loop lost it. Say so at the source rather than hand wasm-ld a shorter list. */
function mustObj(src) {
  const o = OBJ_OF.get(src);
  if (!o || !existsSync(o)) {
    console.error("[build] no object for " + src + " — it reached the link with no compiled identity");
    process.exit(1);
  }
  return o;
}
const OBJS_SHARED = SHARED_SOURCES.map(mustObj);

/* ── LINK BOTH PROGRAMS ───────────────────────────────────────────────────────────────────────────────────
   THE ABI ARTIFACT STAGES WHERE THE EXTENSION LOADS IT: bridge.js does import("./lib/qjs/qjs.mjs"), so that is
   the output path, not engine/host/out. Two artifacts, two homes — emcc derives the .wasm name from the -o
   basename, so both emitting into out/qjs.* would share one qjs.wasm and overwrite each other.
   AND THAT IS ALL A SEPARATE LINK IS — a separate FILE. There is no `extra` object list here selecting which
   components a program may reach, because a link boundary is not a trust boundary: the objects are the same
   objects, what wasm-ld leaves out is only what the exports do not reach, and both artifacts are instantiated
   in the offscreen's own realm with the host holding an exported HEAPU8 over each.
   AND NEITHER LINK IS A DOOR IN FRONT OF THE OTHER: a failing smoke link used to take the production ABI link
   — and with it the only two-instance drive in the tree — out of the run entirely. Both are attempted, both
   are reported, and a program that did not link makes its OWN drive a SKIP with that reason. */
/* `entryObjs` is a LIST because one of the two programs takes two of them — see the note above LDFLAGS: the
   smoke program is a host of the `qjs_*` bodies now, and a parameter that could only ever name one would have
   to be worked around at the one call site that needs both. */
function link(what, entryObjs, ldflags, out) {
  const l = spawnSync(requireEmcc(), [...OBJS_SHARED, ...entryObjs, ...LDFLAGS_COMMON, ...ldflags, "-o", out],
                      { stdio: "inherit", shell: true, cwd: QJS });
  if (l.status !== 0) {
    console.error("[build] " + what + " LINK FAILED rc=" + l.status);
    return { label: what + " link", verdict: "FAILED rc=" + l.status, code: l.status || 1 };
  }
  console.log("[build] OK -> " + out);
  return { label: what + " link", verdict: "PASS", code: 0 };
}
const SMOKE_LINK = link("smoke", [mustObj(ENTRY_SMOKE), mustObj(ENTRY_ABI)], LDFLAGS_SMOKE, join(OUT, "qjs.js"));
const ABI_LINK = ABI_LIST.code
  ? skipped("production ABI link", "the renderer ABI list and main.c's QJS_EXPORT bodies disagree, so this "
                                 + "link's --export= list is known wrong")
  : link("production ABI", [mustObj(ENTRY_ABI)], LDFLAGS_ABI, join(EXT_QJS, "qjs.mjs"));

/* THE ARTIFACT RECORDS THE REVISION IT WAS BUILT FROM, because engine/solvergate.mjs runs this file and
   never compiles anything, so without a stamp the only question it could ask about the program was how old
   the FILE was. That answer is wrong in exactly the mode CLAUDE.md §Testing mandates: `git worktree add`
   writes every tracked file at the checkout instant, so in a frozen snapshot every source is newer than
   any artifact and the check reported a build OF that revision as stale against 600 sources, three minutes
   after the checkout. The stamp is computed by gate_revision.mjs itself rather than re-derived here, so
   what is written and what is checked are the same answer by construction. The cone is what this link
   actually compiled — the host and the submodule — and not the whole tree, for the reason that file gives:
   another agent's popup edit is not a reason to distrust a JS-engine number.
   ONLY WHEN THAT LINK PRODUCED THE ARTIFACT: stamping after a failed link would mark whatever qjs.mjs a
   PREVIOUS build left on disk as belonging to this revision, which is §Testing's number about nothing with the
   stamp itself doing the lying. */
if (ABI_LINK.code === 0) stampArtifact(join(EXT_QJS, "qjs.mjs"), ["engine/host", "engine/qjs"]);

// Milestone smoke test: run test_forced.c's main (the @H merge + @S sink fire-verification on a fixture doc) —
// the design-correctness signal until the live-Chrome harness is re-wired to a rebuilt production ABI entry.
// (The old ES6-module + qjs.wasm staging into extension/lib/qjs served the deleted qjs_* entry; it returns when
// that entry is rebuilt.)
/* BOTH PROGRAMS ARE RUN, in the build that produced them. A target that is only built is the excluded test one
   layer down: the whole point of each is the stream it prints and the report it ends with.
   The compile-only check that used to stand here — the unlinked entry compiled to a throwaway object — is
   DELETED rather than kept beside this. It existed only because one invocation could produce one program, and
   it answered a strictly weaker question than the link above now answers: it caught a missing #include and a
   bad declaration, and nothing at all about an undefined symbol. Leaving it would be a second, worse check of
   the same thing (CLAUDE.md: a superseded system is deleted in the same diff, never kept as a fallback). */

/* A RUN THAT NEVER RETURNS IS NOT A VERDICT, AND UNTIL THIS BACKSTOP IT WAS NOT EVEN AN EVENT. `spawnSync`
   here carried no timeout, so `node engine/build.mjs` — the command CLAUDE.md names as THE build — did not
   terminate whenever the fixture's frontier did not drain, and it consumed a core while not terminating. That
   is not hypothetical and it was not rare: four abandoned `out/qjs.js` processes were found at 84-97% CPU
   having run 1.9-4.1 hours, in the scratch trees of agents that had long since finished. They WERE the machine
   load, and the load was then read by six separate lanes as "the machine is saturated, a measurement now would
   be a loaded-machine artifact" — so they each declined to run their gates. One missing timeout suppressed
   every gate in the project, and nothing anywhere said the word "hang".
   THE BUDGET IS NOT A CAP, AND THE DISTINCTION IS §Testing's: the real measure is the fixture reporting its
   own stream, and the budget is what this file agrees to spend before reading that stream and deciding. It is
   in CPU — see RUN_CPU_BUDGET_S — because a stretch of WALL CLOCK is a budget the box sets rather than this
   file, and every comparison of two revisions' smoke runs made under one was a comparison of two machines.
   The wall clock survives ONLY as the backstop for what CPU cannot see, generous, through its own signal
   (SIGTERM from this harness against SIGXCPU from the kernel), with its own verdict and its own exit code, so
   the two never collapse into one. §NO BOUNDS is about the FRONTIER: it forbids the engine capping its own
   exploration, and it has never had anything to say about a harness declining to spend an unbounded amount of
   a shared machine on one process it launched. */
function runProgram(label, argv, hint) {
  return runChild(label, process.execPath, argv, hint);
}

/* THE TWO PROGRAMS ARE TWO AREAS AND BOTH ARE ASKED, EVERY RUN.
 *
 * The smoke drives test_forced.c's fixture document (its @H probe stream: any row 0 and the process exits
 * non-zero — the trailing arguments reach main()'s argv, the channel getenv could not be, since emscripten's
 * ENV never merges the launching process's environment). The second drives the SHIPPED entry through the
 * surface the bridge actually calls: engine/route.mjs boots the module just linked, provisions a SECOND
 * instance from the create notice, routes posts and a synchronous cross-origin `length` read between them,
 * and parks one of them on an outstanding read.
 *
 * IT IS THE ONLY THING IN THE TREE THAT PROVISIONS TWO INSTANCES, which §SECURITY makes the precondition for
 * believing any cross-instance mechanism has ever run — the world registry, the nearest-first ancestry, the
 * lazy segment materialization, the peer that answers by running a program. It stood BEHIND the smoke's exit,
 * so the run that would first show a cross-instance regression is precisely the run that never asked: any
 * probe row 0, in any unrelated area of the fixture, and the seam went unexercised while the report named only
 * the smoke. That is §Testing's excluded test wearing a complete-looking total, and the fix is that neither
 * stage gates the other and BOTH numbers are in one report. */
const STAGES = [ABI_LIST, SMOKE_LINK, ABI_LINK];
STAGES.push(SMOKE_LINK.code
  ? skipped("smoke test", "the smoke program did not link")
  : runProgram("smoke test" + (MIN ? " (minimal document)" : ""),
               [join(OUT, "qjs.js"), ...(MIN ? ["--min"] : [])],
               "the @H row printed 0 above names the statement the fixture document makes and this run did " +
               "not answer — engine/host/test_forced.c's probe table is where that row is declared."));
/* A STALE ARTIFACT IS NOT A SUBJECT. route.mjs imports extension/lib/qjs/qjs.mjs off disk, so running it after
   a failed ABI link would measure whatever a PREVIOUS build left there and report the number under this
   revision — which is worse than not running it, and is why this is a SKIP rather than an attempt. */
STAGES.push(ABI_LINK.code
  ? skipped("two-instance ABI drive", "the production ABI program did not link")
  : runProgram("two-instance ABI drive", [join(ENGINE, "route.mjs")],
               "this is the cross-instance seam: the world registry, the nearest-first ancestry fork, the " +
               "synchronous cross-origin read, and the park on an outstanding one. Nothing else in this tree " +
               "provisions a second instance, so a failure here is unobserved by every other gate."));
/* AND THE SAME SEAM ONE LAYER UP — the BROWSER-PROCESS half, which route.mjs plays the part of rather than
   runs. route.mjs calls `makeEngine` itself, so what it proves about the transport says nothing about the two
   components that decide an instance exists and materialize it in the shipped extension:
   `extension/render-process-host.js` (the registry: which agent clusters have a renderer, the routing-id mint,
   and the refusal of a second for one cluster) and `extension/renderer-host.js` (the RenderFrameHost). This
   stage loads both by their own bytes and drives them, so the admission decision and the CHECK-class refusal
   SECURITY.md's one-instance-per-agent-cluster rule is made of are taken by the shipped code.
   IT SKIPS ON THE SAME CONDITION AND FOR THE SAME REASON as the drive above: it boots the renderer program the
   ABI link just produced, so running it after a failed link would measure whatever a previous build left in
   `extension/lib/qjs/` and report that number under this revision. */
STAGES.push(ABI_LINK.code
  ? skipped("browser-process layer", "the production ABI program did not link")
  : runProgram("browser-process layer", [join(ENGINE, "renderer_host_gate.mjs")],
               "this is the renderer registry and the RenderFrameHost, driven by their own bytes: two " +
               "cross-origin renderers provisioned into one browsing-context group, the duplicate-cluster " +
               "refusal that is SECURITY.md's one-instance-per-cluster rule, and the routing-id accounting. " +
               "Until this stage existed neither file was compiled, imported or run by anything."));
/* THE RECORD-FIELD CONTRACT, which is the defect class §Architecture names and which nothing else here asks
   about: a name a consumer READS off a producer's record and no producer WRITES, a field a producer emits that
   nothing reads, a `||`/`??`/`?.`/swallowed-catch that turns either of those into a plausible datum instead of
   a crash, and — the one where the plausible datum is a VALUE rather than a name — a host branching on a code
   the ABI cannot answer. Every instance of it this project has recorded was a live product defect that was
   invisible AS A CRASH and was found later by somebody chasing a wrong number.
   IT IS A STAGE FOR THE TWO REASONS ITS AUTHOR NAMED AND NEITHER IS OPTIONAL. (1) The SNAPSHOT: it is a
   READER, and a reader of a shared working tree measures a program no revision contains just as a compiler of
   one does — the difference is only that the reader's wrong answer looks like a finding rather than like a
   segfault. Run here it reads the same checkout this build compiled, and it prints its OWN `[rev]` block over
   its OWN cone (engine/host, engine/*.mjs, extension, testing/*.js), which is deliberately NOT this build's
   cone: a popup edit must not be a reason to distrust a link, and a link's cone would say nothing about the
   extension files that are half of this contract. (2) The EXIT CODE JOINS: it is pushed onto STAGES like every
   other stage, so report() carries it into the build's verdict rather than letting it print beside one. A gate
   that only prints is a gate somebody has to remember to read.
   IT SITS WITH THE AUDITS AND NOT IN FRONT OF THE PROGRAMS, on the same argument as the one below: it compiles
   no C and reads no artifact, so it asks its question of the SOURCES whatever the programs did, and a link
   failure can never take it out of the run — while report() exiting on the FIRST non-zero code keeps a program
   that did not build or did not run ahead of a source-scan finding. */
STAGES.push(runProgram("record-field contract audit", [join(ENGINE, "fieldgate.mjs")],
                       "each category above is one side of a contract with nothing on the other: a field read " +
                       "off a record no producer emits, a field emitted and never read, a default that stops " +
                       "either from crashing, or a branch on a value outside a producer's return domain. Fix " +
                       "at the ROOT — make the consumer DCHECK the field (extension/check.js mirrors check.h) " +
                       "or delete the half of the contract that has gone stale. There is no baseline to " +
                       "update: the count IS the disagreement."));
/* THE THIRD AREA: does each component install the surface its Web IDL declares. It gates nothing and nothing
   gates it — it compiles no C and reads no artifact, so it asks its question of the SOURCES whatever the two
   programs above did, and a link failure can never take the member census out of the run with it.
   IT IS LAST IN STAGE ORDER, AND THAT IS THE ONE THING THIS PLACEMENT DECIDES: report() exits with the first
   non-zero code, so a program that does not build or does not run keeps its own exit code and the member gap
   does not stand in front of it. Every stage still reports, which is the whole point of the list. */
STAGES.push(runProgram("Web IDL gap audit", [join(ENGINE, "idlgen.mjs")],
                       "each category above is a spec member no component installs, a stub where a real value " +
                       "belongs, or an install construct the audit cannot resolve — implement it at the root " +
                       "in its real component. There is no baseline to update: the count IS the gap."));
report(STAGES);

/* A THIRD DRIVE STOOD HERE — the driver for the deleted second program, which put the RENDERER REGISTRY's
   transitions through it — and it is deleted with the program it drove. THE COVERAGE IT HELD IS NAMED RATHER
   THAN QUIETLY DROPPED, because a gate that vanishes with its subject is only honest if what it was measuring
   is stated: it exercised the duplicate-cluster refusal, the two keys that are equal up to `clusterKeyOf`'s
   NUL separator, and the reported-dead-twice and never-minted-id refusals.
   THAT GAP IS THE `browser-process layer` STAGE ABOVE AND THIS PARAGRAPH NO LONGER DESCRIBES THIS TREE. It
   stood here saying "this build compiles no JavaScript" and "the REFUSALS have no caller that fires them",
   which was true and then was read as an instruction — the exact way a `DFAIL` outlives the absence it names
   and starts to lie. `engine/renderer_host_gate.mjs` loads render-process-host.js and renderer-host.js by
   their own bytes into a realm carrying only the browser surface they read, provisions two cross-origin
   renderers through the registry, asks it for a second renderer for a cluster that already has one, and asks
   it to bury a routing id it never minted and a renderer it has already buried — so all THREE of the
   registry's CHECK-class refusals now have a caller that fires them on every build, which is the whole of the
   coverage this paragraph was written to record the loss of.
   WHAT NO GATE IN THIS FILE ASKS AT ALL, stated here rather than left for a reader to discover by grep:
   whether `clusterKeyOf` decides that two LIVE documents are two clusters. Both gates above write their own
   keys, and SECURITY.md requires both halves of a real one to be BROWSER-STATED — so that claim belongs to the
   live harness (`harness offscreen "return await self.rendererPoolProbe()"`) and is made nowhere here.
   AND A MESSAGE ROUTED BETWEEN TWO RENDERERS, WHICH THIS PARAGRAPH DESCRIBED WRONGLY ON THE LINE IT NOW
   REPLACES. It said renderer-to-renderer routing "is bridge.js's and is exercised by route.mjs one layer
   down", which reads as covered and is not: the two halves belong to different components and only one of
   them has a caller. route.mjs exercises the ENGINE's receiving half — `qjs_route` delivering into the right
   timeline, `qjs_perform`, `qjs_host_answer_remote` — but the routing DECISION it makes is its OWN model,
   three lines of `engines.find((e) => e.docId === doc)`, and the shipped decision is `bridge.js`'s
   (which instance holds the named document, and the sender's origin STAMPED BY THE TRUSTED ZONE — SECURITY.md
   makes that stamp this zone's precisely because a forgeable one defeats every origin check in every bundle).
   No gate loads bridge.js. So the claim splits three ways and each belongs somewhere different:
     - the engine's receiving half — route.mjs, where it already is, and it must STAY there: giving that stage
       frames, mojo and a registry would put an engine-seam regression and a browser-process regression in one
       verdict, which is the collapse §Testing forbids;
     - the record crossing the TYPED, VALIDATED wire — `content.mojom.Renderer` declares `route`, `perform`,
       `hostAnswerRemote` and `worldGone`, and route.mjs reaches those entries by raw `ccall`, so the validator
       that assumes the renderer is hostile has never seen a routed record. That belongs in the stage ABOVE,
       which already holds two mojo-bound instances. ITS PRECONDITION IS NOW MET AND WAS NOT WHEN THIS LIST WAS
       WRITTEN: that stage called `init`/`getBundleId`/`teardown` and never `begin`/`step`, so no flow had ever
       run behind the frame boundary and a routing phase would have asserted against a peer with no frontier to
       route into. Phase 4 drives one renderer's frontier over the wire — a cross-origin `window.open`, which
       core/frame/navigable.c ANNOUNCES rather than creates because the child is in another agent cluster — and
       reads the `navigable.create` notice back through the typed boundary, asserting the record against its
       own grammar field by field — including HTML §7.3.1.3's PARENT NAVIGABLE, whose `u` is what distinguishes
       the AUXILIARY navigable a `window.open` makes from the child navigable an `<iframe>` does. What is still unwritten is the record travelling the other way: `Route`,
       `Perform` and `HostAnswerRemote` are declared and the validator has still never seen one;
     - the routing DECISION and the origin stamp — bridge.js's, exercised by nothing, and NOT closed by adding
       a phase above: a gate that models `holderOf` the way route.mjs does is honest only if it says so. What
       would close it is bridge.js's router becoming loadable without the chrome extension APIs and IndexedDB
       it currently drags in, which is a refactor
       and not a gate, and is written here so the next reader starts from it rather than from a green stage. */
