/* The FORK's capability fixtures — what test262 does not contain because it is not in the spec.
 *
 * WHY THIS EXISTS: test262 is the baseline and it is self-validating, but every assertion in it is about what
 * ECMAScript says. Nothing in it asks whether a JSON parse can be INTERRUPTED, whether a regexp match resumes
 * at the exact opcode, or whether a pattern with four hundred capture groups compiles — those are properties
 * of this fork, and the spec is silent on them because a conforming engine may cap them all. So the corpus
 * going green proves the browser half is right and proves nothing about the half this project is for.
 *
 * These files were living in a scratch directory until a container restart deleted them, which is the whole
 * argument: verification that evaporates is not verification. They are test262-format (each carries its own
 * `assert.*` oracle) and run through the same run-test262 under the same forced time-travel, so they cost one
 * binary and no new harness.
 *
 * A file here asserts a CAPABILITY, never an implementation. "JSON.parse yields once per completed value" is a
 * capability; "the JP_PARSE stage is numbered 2" is a design pin and belongs nowhere. That distinction is why
 * these are committed while a regression test is deleted after use — a capability assertion does not prevent a
 * better design, it constrains what a better design still has to do.
 *
 * Usage:  node engine/features.mjs
 */
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync } from "node:fs";
import { join, relative } from "node:path";
import { tmpdir } from "node:os";
import { gateRevision, revisionLines, revisionMoved } from "./gate_revision.mjs";
import { childCpuSeconds, childCpuDelta, cpuText } from "./gate_cpu.mjs";
import { collectFixtures } from "./gate_collect.mjs";

const ROOT = join(import.meta.dirname, "..");
const QJS = join(import.meta.dirname, "qjs");
const TESTS = join(import.meta.dirname, "tests");
const SRCS = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c", "quickjs-libc.c", "run-test262.c"];

/* ---------------------------------------------------------------------------------------------------------
   THE COLLECTION IS ACCOUNTED FOR, NOT ASSUMED — and the assertion below is the whole reason this section
   exists rather than a `-d` and a hope.

   §Testing: "A TEST FILE THE GATE DOES NOT COLLECT IS AN EXCLUDED TEST, AND AN EXCLUDED TEST IS A FAILURE …
   worse, because the total LOOKS complete." That is not hypothetical here — `-d <dir>` hands run-test262 a
   tree and run-test262 then answers `Result: F/N errors` where N is the number of RUNS it performed, which is
   the number of files only under a one-run-per-file mode (asserted below, against test262.conf). Two things
   silently shrink N with nothing in the summary that a reader could notice:

     (1) A `features:` tag naming anything absent from test262.conf's `[features]` list makes the file SKIP —
         run_test_buf is never entered, so `test_count` is never incremented. `0/17` and `0/18` are the same
         shade of green. This driver never printed the excluded/skipped counts at all, so the shrink was not
         merely easy to miss, it was not in the output.
     (2) Anything that is not a `.js` file is never a candidate: run-test262's own add_test_file takes
         `.js` and rejects `_FIXTURE.js`, and everything else in the tree is passed over in silence.

   So the count is DERIVED FROM DISK and compared. engine/gate_collect.mjs derives it, restating run-test262's
   own rule (recursive, `.js`, not `_FIXTURE.js`) as a MIRROR and saying so, because if the two rules drift the
   gate reddens on a difference that is about this project rather than about the corpus — the honest failure of
   the pair and the one that gets fixed. Its own header carries the structural argument and the `GATE` format.
   --------------------------------------------------------------------------------------------------------- */

/* THE WALK ITSELF IS engine/gate_collect.mjs, and it lives there rather than here for the reason this whole
   section exists one level up: it is a pure function of a path, and inline it was reachable only AFTER a clang
   invocation — so the lanes that do not build could not exercise the accounting at all, and an accounting
   nobody can run is the unexecuted design §Security warns about wearing a gate's clothes. Extracted, it takes
   one fixture tree and no compiler. */
const { files: expected, failures } = collectFixtures(TESTS, ROOT);
const fail = (msg) => failures.push(msg);

/* WHAT `test_count` COUNTS IS EXECUTIONS, NOT FILES, and the equality below is only a file count because the
   configured mode runs each file once. run_test dispatches on test_mode: `default`/`default-nostrict`/
   `default-strict` pick exactly one of use_strict/use_nostrict, `strict`/`nostrict` pick one or NEITHER (a
   file flagged the other way is counted as SKIPPED, which the shortfall message below already explains) —
   but `all`/`both` run a non-module file TWICE, and run_test_buf increments test_count per RUN.
   So under `mode=all` this gate would report "ran 36 of the 18 fixtures on disk" and send the reader hunting
   for eighteen missing files. That is the assert-with-a-remedy-and-no-site shape: a true-sounding sentence
   pointing at nothing. The mode is therefore READ and stated, so a change to the conf reddens with the cause
   in its own words instead of through a bogus shortfall. */
const CONF = join(QJS, "test262.conf");
const modeLine = readFileSync(CONF, "utf8").match(/^[ \t]*mode[ \t]*=[ \t]*(\S+)/m);
const MODE = modeLine ? modeLine[1] : "default";        /* run-test262's own default is TEST_DEFAULT_NOSTRICT */
if (MODE === "all" || MODE === "both")
  fail(`${relative(ROOT, CONF)} sets mode=${MODE}, under which run-test262 runs a non-module file in BOTH ` +
       "strict and sloppy mode and counts each run in `test_count`. The disk-count equality this gate asserts " +
       "compares FILES to that counter, so it can no longer hold and its failure message would name a " +
       "shortfall that does not exist. Either restore a one-run-per-file mode, or teach the equality to " +
       "derive the expected EXECUTION count per file (strict + nostrict, minus onlyStrict/noStrict flags).");

if (failures.length) {
  console.error("\n[features] COLLECTION IS BROKEN — the corpus this gate would report on is not the corpus on disk:");
  for (const f of failures) console.error("  " + f);
  process.exit(1);
}

/* WHICH TREE THIS NUMBER IS ABOUT, asked before the build and printed with the result — §Testing, and
   engine/gate_revision.mjs for the incident that made it a mechanism. The cone is the submodule plus this
   driver plus the fixtures themselves: those files ARE the assertions, so an edit to one changes what this
   number means exactly as an edit to quickjs.c does. */
const REV_AT_START = gateRevision(["engine/qjs", "engine/features.mjs", "engine/tests", "engine/gate_revision.mjs"]);
for (const l of revisionLines(REV_AT_START)) console.log(l);

console.log(`[features] building native run-test262 (clang, NDEBUG) for ${expected.length} fixtures…`);
const bin = join(mkdtempSync(join(tmpdir(), "feat-")), "run262.exe");
/* THE COMPILER AND THE QUIET LIST ARE THE SHIPPED BUILD'S, for the two reasons engine/build.mjs gives at its
   own CFLAGS. (1) Everything that ships is built by emcc, i.e. clang, so a gate on gcc certifies a translation
   of these sources that nothing runs — quickjs-step.h's three struct tags are at file scope because each was
   otherwise a fresh type per prototype, and "gcc -w hid all three; the project's clang build does not".
   (2) `-w` was here, and `-w` is exactly what that sentence blames: it suppresses the diagnostic outright, so a
   missing #include makes C assume `int (...)` and a returned 64-bit POINTER comes back TRUNCATED. That is not
   hypothetical — window.c called window_proxy_name without its header and the whole corpus segfaulted inside
   strcmp, with no diagnostic. A gate that silences the class of error it exists to catch is not a gate.
   (3) AND `-DENABLE_DUMPS`, which was MISSING here while engine/build.mjs sets it on every wasm build and both
   other native gates set it too. It is not optional decoration: it changes the interpreter's own per-opcode
   dispatch macros, so without it this driver certified a DIFFERENT interpreter from the one that ships — the
   same defect test262.mjs records at its own CFLAGS, where a dangling `if` in the per-opcode dump made every
   `if (cond) BREAK;` dispatch unconditionally and 43239 passing tests said nothing about it. A gate on a
   translation nothing runs is the loaded-machine defect with the compiler holding the wrong knob. */
const cc = spawnSync("clang", ["-O1", "-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare",
  "-Wno-parentheses", "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds",
  "-Wno-stringop-overflow", "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer",
  "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration",
  "-DNDEBUG", "-D_GNU_SOURCE", "-DCONFIG_VERSION=\"t262\"",
  "-DAPICLIENT_DEV=1", "-DENABLE_DUMPS", "-I.", ...SRCS, "-o", bin, "-lm", "-lpthread"],
  { cwd: QJS, encoding: "utf8" });
if (cc.status !== 0) { console.error("[features] build FAILED\n" + (cc.stderr || "")); process.exit(1); }

/* THE BUDGET IS CPU, NOT WALL CLOCK — §Testing, and engine/test262.mjs carries the full argument at its own
   budget. This was `timeout: 1_800_000`: half an hour of ELAPSED time on a box that runs fifteen agents, which
   is a measurement of the machine's load wearing a verdict about the engine. RLIMIT_CPU counts what the child
   actually consumed, so a run starved of the thread is never killed for waiting; the wall backstop stays,
   generously, for a child that consumes no CPU because it is DEADLOCKED, and the two report through DIFFERENT
   signals (SIGXCPU from the kernel, SIGTERM from node) so the summary can name which one fired. The hard limit
   sits above the soft one because dash makes them equal when given a single value. */
const CPU_BUDGET_S = 900;
const WALL_BACKSTOP_MS = 1_800_000;
const cpu0 = childCpuSeconds();
const r = spawnSync("/bin/sh",
  ["-c", `ulimit -H -t ${CPU_BUDGET_S + 60} 2>/dev/null; ulimit -S -t ${CPU_BUDGET_S}; cd "$1" && shift && exec "$@"`,
   "sh", QJS, bin, "-c", "test262.conf", "-d", TESTS],
  { cwd: QJS, encoding: "utf8", maxBuffer: 1 << 28,
    env: { ...process.env, FORK_PREEMPT: "1" }, timeout: WALL_BACKSTOP_MS });
const cpuUsed = childCpuDelta(cpu0, childCpuSeconds());
const out = (r.stdout || "") + (r.stderr || "");

const why = out.match(/@WHY .*/);
/* ALL THREE READ OFF THE ONE `Result:` LINE, anchored to it. run-test262 builds it as
   `Result: F/N error(s)[, X excluded][, S skipped]`, both tails optional, so the skipped count is reached
   THROUGH the optional excluded one. A bare /(\d+) skipped/ would also match those words appearing in a
   fixture's own stdout, and the number it then fed the reconciliation below would be a fact about a test's
   output wearing a verdict about the corpus — the same collapsed-signal defect this driver corrects one level
   up, at the one place a stray match is cheapest to make impossible. */
const m = out.match(/Result: (\d+)\/(\d+) errors?/);
const excl = out.match(/Result: \d+\/\d+ errors?, (\d+) excluded/);
const skip = out.match(/Result: \d+\/\d+ errors?(?:, \d+ excluded)?, (\d+) skipped/);
const feat = out.match(/Feature: (\d+) preempt-requested, (\d+) fired/);
/* `Feature: NOT ENGAGED` is a DIFFERENT line, and matching only the numeric one made a run that reached zero
   back-edges print no engagement line at all — an absent statement read as a satisfied one. It is a failure:
   a corpus written to exercise suspension that suspended nothing proves nothing about it. */
const notEngaged = /Feature: NOT ENGAGED/.test(out);
/* BOTH detectors, because they count different bypasses and this driver read one. `DriveToCompletion` counts a
   coroutine body run to completion off-tramp; `SyncDriveToCompletion` counts a bytecode body entered by C
   recursion below a live activation. The regexp for the first must not be satisfied by the second's name. */
const d2c = out.match(/(?:^|[^c])DriveToCompletion: (\d+)/m);
const sync = out.match(/SyncDriveToCompletion: (\d+)/);

for (const line of out.split("\n")) if (/unexpected error/.test(line)) console.log("  " + line.trim());
if (why) console.log("  " + why[0]);

const budgetCause =
    r.signal === "SIGXCPU" || (r.signal === "SIGKILL" && cpuUsed !== null && cpuUsed >= CPU_BUDGET_S)
    ? `the ${CPU_BUDGET_S}s CPU budget — ${cpuText(cpuUsed)} of real CPU consumed, not elapsed`
  : r.signal === "SIGKILL"
    ? `SIGKILL after ${cpuText(cpuUsed, 2)} of CPU, far below the ${CPU_BUDGET_S}s budget, so it is NOT a limit ` +
      "this driver installed. The OOM killer is the usual one (dmesg names it)."
  : r.signal === "SIGTERM"
    ? `the ${WALL_BACKSTOP_MS / 1000}s WALL BACKSTOP having consumed ${cpuText(cpuUsed, 2)} of CPU — ` +
      (cpuUsed === null ? "unmeasured, so this driver cannot say whether that is a DEADLOCK or a saturated box."
       : cpuUsed < 1 ? "that is a DEADLOCK, not load: it was asleep waiting for something that never came."
       : "that is a SATURATED BOX, not a deadlock — it was starved of the thread, NOT a crash.")
    : null;

console.log("\n==================== fork capabilities ====================");
if (!m) console.log(budgetCause ? `  DID-NOT-COMPLETE — killed by ${budgetCause}`
                                : "  NO RESULT — a HARD crash before the summary (segfault or abort). FAIL LOUD:\n" +
                                  out.split(/\r?\n/).slice(-40).join("\n"));
else console.log(`  ${m[1]}/${m[2]} errors over ${expected.length} fixtures on disk   (CPU ${cpuText(cpuUsed)})`);

/* THE ACCOUNTING, PRINTED — a count the reader cannot check against the directory is the same trust this
   section removes. THE SHORTFALL IS RECONCILED RATHER THAN LISTED, and the difference is the whole of the
   correction: run-test262 prints a filename only when something goes WRONG with it (an error, or
   `unknown feature:` for a skip), so a PASSING file is named NOWHERE. Filtering the disk set by "was this path
   mentioned in the output" therefore returns the files that RAN CLEANLY and omits the one that did not — a
   confident list pointing at exactly the wrong files, which is worse than no list, and is the plausible-datum
   defect performed inside the instrument built to end it.
   So the three states are separated by ARITHMETIC, which the summary states exactly: ran, excluded, skipped.
   (`excluded` should be structurally impossible here and is still reconciled rather than assumed away: the
   child gets an ABSOLUTE `-d`, and test262.conf's [exclude] entries are matched as strings against the paths
   run-test262 enumerated, so a corpus-relative entry cannot match one of these. A non-zero count would mean
   that stopped being true, which is a thing to be told rather than to have silently folded into the residual.)
   What is left over is the residual — files the enumeration never reached — and this driver says so and says
   it CANNOT name them, because it cannot. Naming them needs `-vv`, which prints every path run_test was
   handed; it is not on by default here because `verbose > 1` also installs a Test262Error subclass into the
   harness and dumps exceptions, i.e. it changes the run this gate is measuring. */
if (m && +m[2] !== expected.length) {
  const ran = +m[2], nExcl = excl ? +excl[1] : 0, nSkip = skip ? +skip[1] : 0;
  const residual = expected.length - ran - nExcl - nSkip;
  console.log(`  COLLECTION SHORTFALL: run-test262 ran ${ran} of the ${expected.length} fixtures on disk ` +
              `(${nExcl} excluded by test262.conf, ${nSkip} skipped, ${residual} unaccounted)`);
  const skipped = [...out.matchAll(/^(.*):\d+: unknown feature: (\S+)/gm)];
  if (skipped.length)
    console.log("  SKIPPED FOR AN UNLISTED `features:` TAG — add it to test262.conf's [features], or drop the " +
                "tag from the file:\n" +
                skipped.map(([, f, tag]) => `    ${relative(ROOT, f)}  (${tag})`).join("\n"));
  if (residual !== 0)
    console.log(`  ${Math.abs(residual)} FIXTURE(S) THE ENUMERATION NEVER REACHED, and this driver cannot name ` +
                "them: run-test262 prints a path only for a file that FAILED or was skipped, so the ones that " +
                "ran cleanly are indistinguishable from the ones it never opened. Either an [exclude] entry " +
                "matched without being counted, or gate_collect.mjs's mirror of add_test_file has DRIFTED from " +
                "run-test262's own rule. Re-run with `-vv` appended to the child's argv to get the per-path " +
                "listing, and fix whichever of the two rules is wrong.");
}
/* Engagement is reported for the same reason it is on the corpus: a green result proves nothing the run did
   not exercise, and these files exist to exercise the suspending paths. It is ENFORCED for the same reason —
   §Testing requires the harness to fail below 100%, because a fake green is worse than a red. */
if (notEngaged)
  console.log("  feature: NOT ENGAGED (0 back-edges)  <-- proves NOTHING: no loop ever suspended in this run");
else if (feat) {
  const [, req, fired] = feat;
  const pct = (100 * Number(fired)) / Number(req);
  console.log(`  feature: ${pct.toFixed(1)}% engaged (${fired}/${req} back-edges)` +
    (fired !== req ? "  <-- FAKE-GREEN: the feature was SKIPPED here (a body with no adopting driver)" : ""));
} else if (m) console.log("  feature: NO ENGAGEMENT LINE — FORK_PREEMPT off, or an engine that no longer reports it");
if (d2c) console.log(`  drive-to-completion: ${d2c[1]}  (>0 = a coroutine ran to completion off-tramp)`);
if (sync) console.log(`  sync drive-to-completion: ${sync[1]}  (>0 = a body ran by C recursion, unable to suspend)`);
for (const l of revisionLines(REV_AT_START)) console.log(l);
{
  const moved = revisionMoved(REV_AT_START);
  console.log(moved ? `[rev] AND IT MOVED WHILE THIS RAN — ${moved}` : "[rev] the engine did not move during this run");
}
console.log("===========================================================");

const shortfall = !m || +m[2] !== expected.length;
const fakeGreen = notEngaged || !feat || feat[1] !== feat[2];
const drove = (d2c && +d2c[1] > 0) || (sync && +sync[1] > 0);
process.exit(why || !m || m[1] !== "0" || shortfall || fakeGreen || drove ? 1 : 0);
