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
import { lexborSourceId } from "./lexbor_source.mjs";
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
/* THE LIST IS THE CONTRACT AND THE READING IS A SUBSET OF IT, which is exactly what WFQ_FIELDS is and why it
   is written the same way. Its job is not that every name below is compared — `hungCause` uses five of them —
   it is that a row solver/result.c STOPS EMITTING, or renames, fails the build at this line instead of being
   compared as `undefined` (which is FALSE for every input, so the arm reading it can never fire again). That
   is not hypothetical here: this file records THREE separate WFQ rows that were added to the census and not to
   its list, each one a number computed on every sample and read by nothing until somebody noticed months
   later. A census whose consumer names a subset is a census whose next row joins the unchecked half by
   default. So the list is solver/result.c's `result_cold_json` format string, in full, and adding a row there
   is a change in two places on purpose. */
/* AND `finished`/`sold` EACH ARRIVE WITH THE TWO POPULATIONS THEY ARE THE SUM OF, which is the row every
   reading in this file that turns on retirement was missing. The frontier holds exploration flows and @S
   candidate sessions, and on a real document the second are the great majority of it — so `retiring` was true,
   and "flows retired steadily" meant "the search discarded candidates" with nothing in the census able to say
   which. The two take opposite work: coverage gained versus a derived payload that ran and did not fire.
   BOTH ARMS ARE NAMED HERE AND THE SUM IS CHECKED, never one arm and a subtraction. `finished - finishedCands`
   is a number for every pair of inputs including the pair where one of them stopped being written, so a
   derived half is a half that cannot fail; `coldPartition` below is what makes it fail. */
const COLD_FIELDS = ["live", "framed", "blocked", "owed",
                     "finished", "finishedFlows", "finishedCands",
                     "deepest", "completed", "sold", "soldFlows", "soldCands", "forks",
                     /* `resumed` IS THE POSITIVE STATEMENT the three orphanClaims rows below need, and it is
                        listed for the reason this list exists rather than as decoration: those three read 0
                        both when no residue was handed to the session and when a rebuild carried no drives,
                        and solver/result.c calls the last of them THE VERDICT. Without `resumed` beside them a
                        reader takes a pass out of a session that never resumed. The four decomposition rows
                        are cold.h's "which ARMS of the grammar ran" — a residue of only `'f'` records
                        exercised neither the hex walk nor a candidate resume nor the foreign-world rebuild. */
                     "resumed", "resumedSegs", "resumedFlows", "resumedCands", "resumedWorlds",
                     "orphanClaims", "orphanClaimsMet", "orphanClaimsUnmet",
                     "hostAsked", "hostAnswered", "hostAnswersExtra", "hostAnswersLate", "hostTerminated",
                     "pagedReqs",
                     "decEntries", "decKiB", "headEntries", "headKiB",
                     "domHeadEntries", "domHeadKiB", "jobs", "pend", "pendKiB",
                     "miscKiB", "perFlowKiB",
                     "segKiB", "domSegKiB", "pinSegs", "pinSegEntries", "pinSegKiB",
                     "decSegs", "decSegEntries", "decSegKiB", "dynBodies", "dynKiB", "sharedKiB"];
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
           : ``);
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
  const mech = [], pred = [];
  let spill = 0, bound = null;
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
                      `in and absence is its only way of saying "not seen"), and its one bound member is a ` +
                      `non-negative integer. Anything else is that composer having lost the row rather than a ` +
                      `table with a strange shape, and every percentage taken off it would be a reading of ` +
                      `that loss.`);
    if (k === over) { spill = v; continue; }
    if (k.startsWith("_")) {
      if (k !== lightest)
        throw new Error(`[build] the @FORKAT census carries an unknown member ${JSON.stringify(k)} — the ` +
                        `leading \`_\` is how solver/decide.c marks what is NOT a row, and this reader knows ` +
                        `exactly one of those. A member it cannot name is either summed into the forks as ` +
                        `mass no fork produced or silently dropped out of a partition it is supposed to ` +
                        `close, so it stops here instead.`);
      bound = v; continue;
    }
    if (k.startsWith("(")) { mech.push(k); continue; }
    if (k[0] >= "0" && k[0] <= "9") { pred.push(k); continue; }
    throw new Error(`[build] the @FORKAT census carries a row ${JSON.stringify(k)} opening on none of its ` +
                    `namespaces — solver/decide.c asserts that a constraint key opens on ` +
                    `concolic_ident_compose's decimal length prefix, a mechanism row on \`(\` and the ` +
                    `census's own bound on \`_\`, and this object is partitioned by that byte alone. A third ` +
                    `spelling is read as whichever of the two populations it resembles, which files a ` +
                    `mechanism's exact count among the predicate floors or a page's predicate among rows a ` +
                    `document cannot grow.`);
  }
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
  const sum = (a) => a.reduce((n, k) => n + t[k], 0);
  const mechSum = sum(mech), predSum = sum(pred), total = mechSum + predSum + spill;
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
  const pct = (v) => Math.round(100 * v / total);
  const top = (a) => (a.length ? a.reduce((x, k) => (t[k] > t[x] ? k : x), a[0]) : null);
  const tp = top(pred), tm = top(mech);
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
    /* AND SENTENCE THREE IS WHAT THE CENSUS COULD NOT HOLD, WITH THE ONE NUMBER THAT MAKES THE ARGMAX ABOVE
       SAFE OR UNSAFE TO QUOTE. `tp` is non-null on every arm that reads it: a spill proves a full table, and
       the check above proves a full table has its rows. */
    `. ` + (spill === 0
      ? `decide.c's table did not overflow, so no site was excluded and a key absent from these rows was ` +
        `never reached by this document`
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
  for (const m of out.matchAll(/^@H (.*)$/gm)) {
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
function probeStanding(out) {
  const rows = probeFlips(out);
  if (!rows.length) return null;
  const last = rows[rows.length - 1];
  const names = Object.keys(last);
  const ever = new Set();
  for (const r of rows) for (const k of Object.keys(r)) if (r[k]) ever.add(k);
  return { answered: names.filter((k) => last[k]).length, asked: names.length,
           unanswered: names.filter((k) => !last[k]), ever: ever.size, samples: rows.length };
}
const standingText = (s) =>
  s === null ? "no @H probe stream in this run — this stage makes no statement of that kind"
             : `${s.answered}/${s.asked} of the fixture's statements answered` +
               (s.ever > s.answered ? ` (${s.ever} ever — ${s.ever - s.answered} went back to 0)` : "");

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
  if (m) return m[0];
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
const causeName = (cause) => {
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
const WFQ_FIELDS = ["members", "valMin", "valMax", "valTop", "valZero", "selfEmit", "unrun",
                    "cands", "candUnrun", "candDecMax", "decMax", "wTop", "wMin", "candWMax",
                    /* THE TERMS THAT ARE NOT THE REWARD, WHICH THIS LIST OMITTED AND THE VERDICT BELOW NEEDED.
                       `ordered` is FALSE whenever the reward spread is within one optimism bonus, and the text
                       that case printed said only that the reward is not what is holding the run and that "that
                       census is the measurement to start from" — while naming none of the terms that ARE
                       ordering it. Measured on the smoke fixture: reward spread 0.0, aging 856 points, whole
                       weight spread 0.020, and every number in that sentence absent from this list. A
                       discriminator that can rule a cause out and cannot name the alternative sends the reader
                       back to a census it did not read. */
                    "svcMax", "svcMin", "svcFamMax", "svcFamMin", "visMin", "visMax", "distMax",
                    /* AND THE MOST SERVICE ANY ONE @S CANDIDATE HAS CONSUMED, which the census has emitted
                       since the candidate rows were added and which no reader took. Without it this
                       discriminator names TWO of the three states solver/flow.h says the candidate rows exist
                       to separate — served-and-progressing (a distance problem: build the fitness) and
                       never-served (a starvation problem: the ordering) — and is silent about the third,
                       SERVED WHILE PINNED AT ZERO, which is a candidate being RESTARTED rather than resumed
                       and which no amount of thread time fixes. Three states behind one silence is the defect
                       §@S names about a search's own instrument. */
                    "candSvcMax",
                    /* AND HOW MANY FORK FAMILIES THE PAIR ABOVE ARE THE ENDS OF, which is the row that turns
                       `svcFamMin === svcFamMax` from an ambiguity into a reading. The engine has emitted it
                       since the census learned to count it and this list did not read it, so the verdict below
                       was inferring which of two opposite states held from a pair of extrema that cannot
                       distinguish them: on a ONE-family frontier the equality is an identity of the structure
                       (every member reads one node's service through one pointer) and the family half can
                       never order that document at all; on a several-family frontier it is one instant's
                       coincidence that the next charge moves. "Structurally an offset" is the finding that
                       says stop looking. The reader states it now instead of assuming it. */
                    "families",
                    /* AND WHAT THE ORDER IS COSTING THE JOB QUEUE, which the cold line's `jobs` could never
                       say. One number read the same whether the scheduler was broken or merely mis-scaled, and
                       solver/result.c splits it by WHAT EACH JOB WAITS ON: the host (`jobsOwed` — the pick
                       refuses it), the member finishing its own program (`jobsFramed` — HTML §8.1.4.4 "Calling
                       scripts"' clean up after running script step 3 forbids running it while the JavaScript
                       execution context stack is non-empty, which is a SPEC PRECONDITION and not an ordering
                       problem), or RANK ALONE (`jobsReady`). Only the last is the WFQ's to move, so only the
                       last is a finding about the order — and `jobWGap`, the distance from the front of the
                       queue to the best ready holder in the order's own points, is what says whether it is
                       outranked at all. Those two take opposite work and read identically without each other.
                       `visZero` is the count `visMin` cannot give: how many members have completed NO unit of
                       work, which is the population whose optimism bonus is at its undecayed maximum. It is
                       NOT `unrun` — that row is ZERO OWN SILENCE, which an emission by any arm of a member's
                       fork family RESETS for the whole family at once, so a member that
                       has run and whose account emitted is in `unrun` with visits to its name. */
                    "visZero", "jobsReady", "jobsFramed", "jobsOwed", "jobWGap",
                    /* AND THE ONE WORD OF §scheduler'S RAZOR THIS READER COULD NOT SPEAK. It forbids a resume
                       that "drops, starves, skips, reorders, or forgets ANY flow", and STARVES had no row: the
                       three fields that look like the starved population are all TERMS OF THE WEIGHT and
                       flow_credit_emit resets every one of them, which is why this file already has to warn,
                       twice, that `unrun` counts a member that has run and emitted. `visZero` carries the same
                       flaw one field over. So a reader asking "is some member never being chosen" had only
                       rows that answer "…or chose to emit recently", and the two states take opposite work.
                       `neverPicked` is the dispatch count's zero — nothing resets it, because a member that
                       was never handed the thread did nothing that could — and `neverPickedGap` is how far the
                       best of that population stands behind the weight the pick returned, which is the same
                       count/distance pair as `jobsReady`/`jobWGap` and is read the same way. */
                    "neverPicked", "neverPickedGap"];
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
  for (const f of WFQ_FIELDS)
    if (typeof w[f] !== "number")
      throw new Error(`[build] the @WFQ census has no numeric \`${f}\` — this discriminator reads ` +
                      `${WFQ_FIELDS.join(", ")} and solver/result.c's result_wfq_json is what decides they ` +
                      `exist; a renamed field must be renamed here rather than silently compared as ` +
                      `undefined. A census with \`members: ${w.members}\` states that there WAS an order, so ` +
                      `this is not the empty-frontier shape handled above.`);
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
  /* THE SAME TWO CHECKS FOR THE STARVATION PAIR, which is the same shape one row over and is worth stating
     separately for the reason the job pair is: the C DCHECK on the sign is compiled out of a release build and
     this reader still runs, and the population check spans two rows that no assert anywhere covers. The
     implication is again ONE-directional — with nobody starved there is no member to measure a distance to, so
     the gap is 0 by construction; a gap of 0 WITH a non-zero count is the state the row exists to report and
     must not be refused, because it is precisely the razor's STARVES. */
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
      : w.candDecMax === 0 && w.candSvcMax > 0
        ? `candidates have been served (${w.candSvcMax} notches at the most) and the deepest still stands on ` +
          `ZERO gates — they are being RESTARTED from the baseline rather than resumed, which no amount of ` +
          `thread time fixes`
        : `candidates are being served (${w.candSvcMax} notches at the most) and are progressing — the ` +
          `deepest stands on ${w.candDecMax} gates, so what limits this search is DISTANCE rather than turns`;
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
  const starved = w.neverPicked === 0
    ? `every member has been handed the thread at least once`
    : `${w.neverPicked} of ${w.members} members have NEVER been handed the thread, the best of them standing ` +
      `${w.neverPickedGap.toFixed(3)} points behind the front` +
      (w.neverPickedGap <= 1
        ? ` — within one emission's worth of it, so they are not being outranked and this is the razor's ` +
          `STARVES rather than an ordering that has not reached them yet`
        : ` — more than one emission's worth, so they are genuinely outranked and the aging term is what ` +
          `reaches them`);
  const terms = `terms over the frontier: reward ${rangeVal.toFixed(3)}, fitness ${w.distMax.toFixed(3)}, ` +
                `optimism ${rangeUcb.toFixed(3)}, aging ${(rangeOwn + rangeFam).toFixed(3)} ` +
                `(own ${rangeOwn.toFixed(3)}, family ${rangeFam.toFixed(3)}) — against a total order spread ` +
                `of ${spread.toFixed(3)} and an aging term ${agingPts.toFixed(1)} points deep; ${fam}; ` +
                `${ucb}; ${starved}`;
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
          `${cand}; ${jobs}; ` + terms,
  };
}

/* WHY THE MEMBERS ARE NOT FINISHING — the `stepUnits` histogram, which is the row `finished` and `live` could
   never carry. Those two say work is being ADMITTED and not RETIRED; this says what the members that are not
   retiring are DOING, and the arms take opposite work: `compile-program`/`resume-program` is a frontier that
   legitimately holds more script than it used to, `queue-rendering-opportunity`/`fire-due-timer`/
   `document-lifecycle-stage` is unbounded periodic work, the orphan arms are seeding drives, and
   `host-blocked`/`await-owed-reply`/`await-fetch-record`/`await-peer-operation` are four distinct kinds of
   waiting. One verdict covered all of them.

   IT READS NO LIST OF ITS OWN, and that is deliberate. solver/step_unit.h is the only place an arm is named —
   the enum, the diagnostic string and this row are three expansions of one macro — so a copy of the names here
   would be the second list that eventually disagrees. The rows are taken from the census as it emits them and
   the CONTRACT is checked instead of the spelling.

   THE CONTRACT IS THE PARTITION, AND IT IS WHAT MAKES AN ABSENT ROW DIFFERENT FROM A ZERO ONE. Every live
   member carries exactly one arm, so the values SUM to `live`. A missing row therefore cannot hide behind a
   plausible rendering: the sum falls short and this throws, naming the composer. A row that reads 0 is a
   MEASUREMENT — that frontier had nobody in that arm — and is reported as one. Neither is ever defaulted into
   the other, which is the defect this whole instrument exists to end. */
function stepUnitReading(b) {
  const u = b.stepUnits;
  if (u === null || typeof u !== "object" || Array.isArray(u))
    throw new Error("[build] the @COLD census carries no `stepUnits` object — solver/result.c composes it " +
                    "from solver/step_unit.h's list on every census, so its absence is that composer having " +
                    "changed rather than a frontier with nothing in any arm. An absent histogram and an " +
                    "all-zero one are different facts and this reader will not average them.");
  const rows = Object.entries(u);
  if (!rows.length)
    throw new Error("[build] the @COLD census's `stepUnits` is empty — the histogram is emitted with EVERY " +
                    "row including the zeroes, so an empty object is a composer that stopped listing them.");
  for (const [k, v] of rows)
    if (typeof v !== "number")
      throw new Error(`[build] the @COLD census's \`stepUnits.${k}\` is not a number — the histogram is a ` +
                      `count per arm and a non-numeric row cannot be summed against \`live\`.`);
  const total = rows.reduce((t, r) => t + r[1], 0);
  if (total !== b.live)
    throw new Error(`[build] the @COLD census's \`stepUnits\` sums to ${total} over ${rows.length} arms ` +
                    `against \`live\` ${b.live} — every member carries exactly one arm, so the two sides ` +
                    `disagree about who is standing and no reading composed from this row is about the ` +
                    `frontier that was there. The engine asserts the same identity at the walk ` +
                    `(solver/cold.c); a difference visible HERE and not there is a row lost between the ` +
                    `census and this document.`);
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
   file's own record-field gate refuses one rather than guessing past it. */
const COLDPARK_FIELDS = ["records", "segs", "flows", "cands", "orphans", "worlds", "bytes", "store"];
/* THE @S ARRIVAL CENSUS, SPELLED AS THE RESULT DOCUMENT SPELLS IT. test_forced.c prints the same four numbers
   the document carries as `_sourceReads`/`_sinkReached`/`_sinkTainted`/`_sinkSuppressed`, from the same
   producers, and it prints them under the document's own names — one namespace, so a reader who learns these
   off `@RESULT` can read them off the line and a rename breaks in one place rather than drifting in two. */
const SCENSUS_FIELDS = ["_sourceReads", "_sinkReached", "_sinkTainted", "_sinkSuppressed"];
const COLDRESUME_FIELDS = ["segs", "flows", "cands", "orphans", "worlds", "orphansMet", "orphansUnmet"];
function oneCensus(out, marker, fields) {
  const m = [...out.matchAll(new RegExp(`^${marker} (\\{.*\\})$`, "gm"))];
  if (!m.length) return null;
  let v;
  try { v = JSON.parse(m[m.length - 1][1]); }
  catch { throw new Error(`[build] the last ${marker} line is not JSON — test_forced.c composes it in one ` +
                          `printf, so a line that will not parse is that printf truncated or interleaved.`); }
  for (const f of fields)
    if (typeof v[f] !== "number")
      throw new Error(`[build] the ${marker} census has no numeric \`${f}\` — this reader compares ` +
                      `${fields.join(", ")} and test_forced.c's printf is what decides they exist; a renamed ` +
                      `field must be renamed here rather than silently compared as undefined.`);
  return v;
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
     `childRealms` — §A-CAPABILITY-MATERIALIZED-PER-FLOW's ceiling, one realm per flow that creates a navigable
        with an address, none reclaimed. navigable.c's OOM CHECK sends its reader to this number BY NAME, and
        until now the extension printed no line carrying it.
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
/* BOTH LISTS ARE THE CONTRACT IN FULL, for COLD_FIELDS' reason exactly — solver/result.c's `result_heap_json`
   and `result_swap_json` format strings, every row, whether or not the sentence below reads it. */
const HEAP_FIELDS = ["allocations", "atoms", "strings", "objects", "shapes", "props", "funcs", "funcCode",
                     "arrays", "miscBytes", "miscParts", "childRealms",
                     "objBytes", "propBytes", "shapeBytes", "strBytes", "atomBytes", "funcBytes",
                     "arrayElemBytes", "unattributed", "stepMachines", "trampFrames",
                     "cLiveKiB", "arenaKiB"];
const SWAP_FIELDS = ["installs", "entries", "worst", "mean",
                     "heapSegs", "heapSegEntries", "domSegs", "domSegEntries"];
function lastTwo(out, marker, fields, composer) {
  const s = [];
  for (const m of out.matchAll(new RegExp(`^${marker} (\\{.*\\})$`, "gm")))
    { try { s.push(JSON.parse(m[1])); } catch { /* truncated tail */ } }
  if (s.length === 0) return null;
  const b = s[s.length - 1], a = s[Math.floor((s.length - 1) / 2)];
  for (const f of fields) for (const c of [a, b])
    if (typeof c[f] !== "number")
      throw new Error(`[build] the ${marker} census has no numeric \`${f}\` — this discriminator reads ` +
                      `${fields.join(", ")} and ${composer} is what decides they exist; a renamed field must ` +
                      `be renamed here rather than silently compared as undefined.`);
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
  const h = lastTwo(out, "@HEAP", HEAP_FIELDS, "solver/result.c's result_heap_json");
  const w = lastTwo(out, "@SWAP", SWAP_FIELDS, "solver/result.c's result_swap_json");
  const c = lastTwo(out, "@COLD", COLD_FIELDS, "solver/result.c's result_cold_json");
  const parts = [];
  if (h) {
    const grew = (k) => h.b[k] - h.a[k];
    /* FRAGMENTING AND LEAKING ARE NAMED APART rather than both reported as "memory grew", because engine.h
       states the difference and it is the whole of what a reader does next. */
    parts.push(`heap: live ${h.b.cLiveKiB} KiB / arena ${h.b.arenaKiB} KiB` +
               (grew("arenaKiB") > 0 && grew("cLiveKiB") <= 0
                 ? ` — arena grew ${grew("arenaKiB")} KiB while live did NOT, which is FRAGMENTATION and not a leak`
                 : grew("cLiveKiB") > 0 ? ` — live grew ${grew("cLiveKiB")} KiB` : ` — flat`) +
               `; ${h.b.childRealms} child realm(s), ${h.b.trampFrames} heap frame(s), ` +
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
               `entries, ${c.b.jobs} queued job(s), ${c.b.pend} owed repl(ies), ` +
               `${c.b.dynBodies} shared program(s); frozen: ${c.b.pinSegs}/${c.b.pinSegEntries} pin, ` +
               `${c.b.decSegs}/${c.b.decSegEntries} decision`);
    /* AND WHETHER THE HOST PAID, WHETHER THE DOCUMENT GOT ANYWHERE, AND WHAT THE INHERITED DRIVES DID — three
       questions the frontier's size cannot answer and which each have a row that nothing read. `hostAsked`
       against `hostAnswered` says whether a waiting frontier is waiting because of the RANKING or because
       nobody paid it; `deepest` against `completed` says whether this document reaches its later programs at
       all; `orphanClaimsUnmet` is the cold round trip's loss, exactly. */
    parts.push(`payment: ${c.b.hostAnswered}/${c.b.hostAsked} asks paid` +
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
         rebuild this same document says did not land anything. It throws for the reason a renamed COLD_FIELDS
         row throws: the alternative is a reading composed out of a census that is not internally true. */
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
  return parts.length ? parts.join("; ")
                      : "no @HEAP/@SWAP/@COLD/@FORKAT/@SCENSUS census in this run — a stage that drives no " +
                        "scheduler prints none of them, so this is the absence of the signal and not a " +
                        "reading of the run";
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
   the same way and for the same reason a renamed `COLD_FIELDS` row does. The converse is not a contract: a
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
   — a caveat printed only on the failures is a caveat absent from precisely the comparison it is about. */
const quantumText = (q) =>
  q === null
    ? `[build]   no @QUANTUM line — this stage opened no engine slice, so it has no scheduler denomination ` +
      `rather than an unstated one`
    : `[build]   the engine's slice (${q.sliceMs} ms) and the WFQ's aging charge are denominated in ` +
      `${q.measure}` + (q.instances > 1 ? ` — ${q.instances} instances, all agreeing` : ``) +
      (q.cpu
        ? `\n[build]   that is real thread CPU, so this run's census series is invariant to what else this ` +
          `box was doing`
        : `\n[build]   THAT IS NOT CPU: this run's census series is a reading of ONE INTERLEAVING. The aging ` +
          `charge bills wall time to whichever flow the OS happened to leave running, so the frontier ORDER ` +
          `— and therefore every census below it — varies run to run on one artifact. Compare two runs of ` +
          `one revision before reading a difference between two revisions.`);

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
  for (const f of COLD_FIELDS) for (const c of [a, b])
    if (typeof c[f] !== "number")
      throw new Error(`[build] the @COLD census has no numeric \`${f}\` — this discriminator reads ` +
                      `${COLD_FIELDS.join(", ")} and engine.c's printf is what decides they exist; a renamed ` +
                      `field must be renamed here rather than silently compared as undefined.`);
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
                  would make the answer depend on the verdict it is supposed to inform. */
               stepUnitReading(b);
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
  if (b.live >= a.live)
    return `a STALL${landmarks} (${span}; ${hspan}; ${wfq.text}; ${cs}) — no flow finished across the window, ` +
           `nothing was paged out and nothing was waiting on the host, so work is being admitted and not ` +
           `retired. ` +
           (lastRetire < 0
             ? `AND NOTHING HAS RETIRED IN THIS RUN AT ALL, which is the stronger statement: this is not a ` +
               `frontier that stopped retiring, it is one that never did, so the first question is whether any ` +
               `flow of this document has a terminating shape rather than why these ones do not.`
             : `finished last rose at census ${lastRetire} of ${n}, so the silence is the ` +
               `${n - 1 - lastRetire} census(es) — ${(n - 1 - lastRetire) * PROGRESS_EVERY} units of engine ` +
               `work — since then, and THAT is the number two runs of one revision must agree on.`) +
           /* AND WHOSE REWARD THE ADMITTED WORK IS RANKED ON, which is the one thing this arm can say about the
              ORDER and used to leave inside `wfq.text` for the reader to derive. A frontier that admits and
              does not retire is ranked almost entirely on inheritance — every arm carries its parent's `val`
              (flow_fork_inherit) and every from-baseline newcomer carries the incumbent's
              (flow_arrive_at_virtual_time) — so this number is what separates "the members ahead are the ones
              that produced the findings" from "the members ahead produced nothing and are standing on an
              ancestor that did", and the two take different work. */
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
   ONLY WHERE IT IS FALSE, because a mark on every verdict is a mark that stops being read. `cpu=1` is a
   positive statement that this caveat does not apply, and it is stated in the detail rather than in silence. */
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
const runNumbers = (t) =>
  `[build]   CPU consumed: ${cpuText(t.cpuSeconds)} of the ${RUN_CPU_BUDGET_S / 60} min budget — THIS IS THE ` +
  `MEASURE THE VERDICT IS IN\n` +
  `[build]   elapsed ${t.wallSeconds.toFixed(1)} s against a ${RUN_DEADLOCK_MS / 60000} min deadlock ` +
  `backstop, at load ${loadNow()} on ${cpus().length} cores — CONTEXT, never the verdict\n` +
  quantumText(quantumDenomination(t.captured));

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
function pageErrorText(out) {
  const staged = new Set([...out.matchAll(/^@PAGEERR-STAGED (\S+)$/gm)].map((m) => m[1]));
  const errs = [...out.matchAll(/^@PAGEERR at=(\S+) (.*)$/gm)]
    .map((m) => ({ at: m[1], msg: m[2].trim() }))
    .filter((e) => e.msg);
  if (!errs.length) return "";
  const known = errs.filter((e) => staged.has(e.at));
  const rogue = errs.filter((e) => !staged.has(e.at));
  /* THE STAGED COUNT IS CARRIED EVEN WHEN NOTHING IS WRONG, because its DISAPPEARANCE is the other direction
     this line can report: a run in which the document staged two errors and produced one is a run whose
     fixture stopped exercising something, and a reader shown only the rogue population would see silence. */
  const stagedText = `${known.length} from the ${staged.size} address(es) it declares`;
  if (!rogue.length) return ` — ${errs.length} page error(s), all staged: ${stagedText}`;
  const q = (e) => `${JSON.stringify(e.msg.slice(0, 160))} at ${e.at === "-" ? "no throw site (§8.1.4.6's own answer for a value with no backtrace)" : e.at}`;
  return ` — ${rogue.length} UNSTAGED UNCAUGHT PAGE ERROR(S) (plus ${stagedText}), first: ${q(rogue[0])}` +
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

function runOutcome(label, t, hint) {
  /* APPENDED TO EVERY VERDICT THIS FUNCTION PRODUCES, which is why it is computed once here and folded into
     `bad` rather than added at each arm — an arm added later would otherwise be the one that drops it, and
     the arm most likely to be added later is another failure arm. */
  const pe = pageErrorText(t.captured) + consoleSeverityText(t.captured);
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
  const bad = (verdict, code, why) => {
    console.error(`[build] ${label} ${why}`);
    console.error(runNumbers(t));
    if (hint) console.error(`[build]   ${hint}`);
    if (stand && stand.unanswered.length)
      console.error(`[build]   the rows still 0 (${stand.unanswered.length} of ${stand.asked}): ` +
                    stand.unanswered.join(" ") + `\n` +
                    `[build]   each of those is EITHER a statement this run answered wrongly OR one it never ` +
                    `reached — read them against the standing in the verdict above, and never read a single ` +
                    `0 as a verdict on the mechanism its row names while the others beside it are 0 too.`);
    return { label, verdict: verdict + pe, code };
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
  const aborted = abortRecord(t.captured);
  if (t.signal) {
    return bad("CRASHED on " + t.signal + (aborted ? " — " + causeName(aborted) : ""), 3,
      `DIED ON ${t.signal} — an abort is a DCHECK naming either an invariant to fix at its root or a ` +
      `capability to build, and it is the RESULT of this run rather than an interruption of it.\n` +
      `[build]   ` + (aborted ? aborted
                              : `no @WHY/@E line in this run's output — this signal did not come from ` +
                                `check.h, so the cause is above and is not an assertion`));
  }
  /* THE ABORT THAT ARRIVES AS AN ORDINARY EXIT STATUS — the wasm smoke's only shape. Same class and same code
     as the signal above, because it IS that event; only the transport differs. */
  if (t.status !== 0 && aborted)
    return bad("ABORTED — " + causeName(aborted), 3,
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
  console.log(runNumbers(t));
  /* `stand` IS THE ONE HOISTED TO THE TOP OF THIS FUNCTION — the second `probeStanding(t.captured)` that stood
     here was a fourth reading of one input, taken in the arm least likely to disagree with the other three. */
  /* THE PASS ARM CARRIES IT TOO, AND IT IS THE ARM THAT NEEDS IT MOST — see pageErrorText. A run that answers
     every statement it makes while one of the page's scripts died is still a PASS of the probe table and is
     not a clean run of the document, and those two are the same green line without this. */
  return { label, verdict: (stand ? `PASS — ${standingText(stand)}` : "PASS") + pe, code: 0 };
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
buildLexbor(process.argv[2] === "lexbor");
if (process.argv[2] === "lexbor") { console.log("[build] lexbor archive rebuilt; re-run without arg to build the engine."); process.exit(0); }

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
  
/* LEXBOR, NATIVELY, exactly as wpt.mjs provisions it — the same vendored source and the same cached archive,
     because a second copy of that provisioning is a second thing to keep in step with the pinned tag. */
  const LEXBOR_NATIVE = join(WORK, "lexbor-native", "liblexbor_static.a");
  if (!existsSync(LEXBOR_NATIVE)) {
    console.error("[build] the native lexbor archive is not built: " + LEXBOR_NATIVE + "\n" +
                  "[build] `node engine/wpt.mjs` builds it once (cmake + make) — run that first.");
    process.exit(1);
  }
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
       twenty-two entries the extension calls.
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
       — and it is stated as a skip with that reason so the report never has a silent hole in it. */
    const v2 = v1.code
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
    report([v1, v2]);
  }
  /* AND IT IS RUN, because a target that is only built is the excluded test one layer down: the whole point is
     the stream it prints and the report it ends with, and nothing else in the tree produces either. */
  report([runChild("the native run (" + kind + (MIN ? ", minimal document" : "") + ")", bin, MIN ? ["--min"] : [],
                   "a LeakSanitizer summary above is a real leak, and an AddressSanitizer report a real fault")]);
}

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
                 "qjs_pending", "qjs_provide", "qjs_top_weight", "qjs_set_yield_floor",
                 "qjs_request_park", "qjs_emit_partial",
                 "qjs_host_requests", "qjs_host_answer", "qjs_host_notices", "qjs_route",
                 "qjs_perform", "qjs_host_answer_remote", "qjs_world_gone"];

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
