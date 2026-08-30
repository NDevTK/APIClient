/* FEATURE test262 harness — the project's baseline testing system.
 *
 * WHY THIS SHAPE: the project is NEVER run without its features (time-travel forced execution), so a plain
 * conformance pass tests something that never runs — there is NO non-feature mode to compare against, and a
 * fallback to plain JS_Eval is BANNED (it hides feature gaps). test262 is SELF-VALIDATING: every file carries
 * its own oracle (its assert.* calls + `negative:` metadata), so a `run-test262` "error" already means the
 * FEATURE engine got the spec-WRONG answer. So the harness is ONE run: every stock file driven through the
 * flow machinery under FORCED time-travel (preempt every loop back-edge => thousands of heap-frame
 * suspend/rebuild cycles; async through the async path; NO module/async carve-out — an unsupported construct
 * FAILS LOUD, an honest gap to build at the root, never papered over).
 *
 * An error is a BUG — browser-half or solver, doesn't matter, it runs the one way the project runs. Localise
 * the root with `git bisect run` between HEAD and the upstream fork-base `d0c2272` (remote `upstream`): a
 * browser-half regression bisects to a trampoline/Context-model commit, a time-travel bug to a scheduler
 * commit. Leaks are surfaced by the gc_obj_list walk in JS_FreeRuntime (built NDEBUG so a leak LOGS+COUNTS as
 * a failure line, never a silent abort). Default runs the WHOLE corpus — cherry-picking a clean subdir and
 * calling a category "supported" is the banned self-deception.
 *
 * Usage:  node engine/test262.mjs [subdir]     e.g. node engine/test262.mjs built-ins/Array
 */
import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { gateRevision, revisionLines, revisionMoved } from "./gate_revision.mjs";
import { childCpuSeconds, childCpuDelta, cpuText } from "./gate_cpu.mjs";

const QJS = join(import.meta.dirname, "qjs");
const CORPUS = join(QJS, "test262", "test");
/* A SUBDIRECTORY *OR* A SINGLE FILE, plus pass-through of anything after it. Confirming which test failed has
   repeatedly gone wrong by reading a -vv listing, whose last line is the last SUCCESS and not the failure —
   three separate mis-identifications are in this file's history. run-test262 takes `-f <file>` for exactly
   this; it just had no way through from here, so the only tool available was the one that misleads.
   A path ending in .js becomes -f; anything else is -d; extra argv goes to the binary verbatim. */
const sub = process.argv[2] || "";
const passthru = process.argv.slice(3);
/* THE PATH MUST BE SPELLED THE WAY test262.conf SPELLS IT. Excludes are matched by string — a directory by
   prefix, a file by equality — against the paths run-test262 enumerated, so passing an ABSOLUTE -d made every
   exclusion in the conf silently inapplicable. The conf's own comment claimed a by-hand staging run honoured
   "exclusions and all"; it did not, and an exclusion that quietly does nothing is worse than none, because the
   run reports a number for a set nobody chose. cwd is the qjs directory, so the corpus-relative spelling
   resolves and matches. An absolute path is still taken as-is, for a one-off probe that lives outside the
   corpus rather than being written into it. */
const target = sub ? (sub.startsWith("/") ? sub : join("test262", "test", sub)) : null;
const isFile = sub.endsWith(".js");
const dir = isFile ? null : target;
const SRCS = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c", "quickjs-libc.c", "run-test262.c"];

/* WHAT THE CONF DECIDED, READ HERE SO THE SUMMARY CAN NAME IT. Two of this gate's numbers are functions of
   test262.conf and the summary quoted neither: the `skipped` total is decided by the [features] section, and the
   DENOMINATOR of the result itself is decided by one `mode=` line. A summary that prints a number whose meaning
   lives in another file, without saying which meaning, hands the reader a quantity they cannot check. */
const CONF = join(QJS, "test262.conf");
const confSkipFeatures = [];
let confMode = null;
{
  let section = "";
  for (let line of readFileSync(CONF, "utf8").split(/\r?\n/)) {
    line = line.replace(/#.*$/, "").trim();
    if (!line) continue;
    if (line.startsWith("[")) { section = line.slice(1, -1); continue; }
    const eq = line.indexOf("=");
    const k = (eq < 0 ? line : line.slice(0, eq)).trim();
    const v = eq < 0 ? "" : line.slice(eq + 1).trim();
    if (section === "config" && k === "mode") confMode = v;
    /* `no` and `skip` both disable a feature in run-test262's load_config; `Atomics=!tcc` is a compiler
       condition and not a skip, so matching the two words exactly is what keeps it out of this list. */
    else if (section === "features" && (v === "skip" || v === "no")) confSkipFeatures.push(k);
  }
}
/* THE ONLY MODES THAT PERFORM THE EXECUTIONS THE ORACLE DEMANDS. test262 INTERPRETING.md "Strict Mode": a file
   flagged none of noStrict/onlyStrict/module/raw MUST be executed twice, sloppy and strict. Every other mode
   runs such a file ONCE — and because the FILE was collected, that missing execution appears in no exclusion
   list, no skip count and no NOT-RUN line. It is the excluded-test failure with nothing anywhere to name it. */
const modeRunsBoth = confMode === "all" || confMode === "both";
/* A LIST IS PRINTED WHOLE OR IT IS NOT EVIDENCE. Truncating the skip entries to the interesting few would put
   this summary back in the business of choosing what the reader gets to check, which is the habit the line
   below exists to end; so it wraps instead of eliding. */
const wrapList = (items, perLine, indent) => {
  const rows = [];
  for (let i = 0; i < items.length; i += perLine) rows.push(indent + items.slice(i, i + perLine).join(", "));
  return rows.join("\n");
};

if (!existsSync(CORPUS)) {
  console.error("[test262] corpus missing — check it out:\n" +
    "  cd engine/qjs && git -c submodule.test262.update=checkout submodule update --init --depth 1 test262");
  process.exit(1);
}

/* WHICH TREE THIS NUMBER IS ABOUT, asked before the build and printed with the result — CLAUDE.md §Testing,
   and engine/gate_revision.mjs for the incident that made it a mechanism. THE CONE IS THE SUBMODULE AND NOT
   THE HOST: this gate links five quickjs sources and not one file under engine/host, so an edit to solve.c or
   cow.c does not touch the program measured here and reporting it as if it did would be the same collapsed
   verdict §Testing forbids one level down. */
const REV_AT_START = gateRevision(["engine/qjs", "engine/test262.mjs", "engine/gate_revision.mjs"]);
for (const l of revisionLines(REV_AT_START)) console.log(l);

console.log("[test262] building native run-test262 (clang, NDEBUG)…");
const bin = join(mkdtempSync(join(tmpdir(), "t262-")), "run262.exe");
/* THE SAME FLAGS THE ENGINE SHIPS WITH. ENABLE_DUMPS is not optional decoration — engine/build.mjs sets it on
   every wasm build, and it changes the interpreter's own dispatch macros. Building the oracle without it tested
   a DIFFERENT interpreter: a dangling-if in the per-opcode dump made every `if (cond) BREAK;` dispatch
   unconditionally, so `typeof x === "function"` answered with its operand, and 43239 passing tests said nothing
   about it because not one of them ran the code that shipped. A flag that changes the engine belongs here. */
/* `-w` SILENCES STYLE, NEVER A MISSING PROTOTYPE. C89's implicit-declaration rule assumes `int (...)`, so a
   function whose header was not included returns a 32-bit value — and a 64-bit POINTER comes back TRUNCATED.
   That is not a warning-shaped problem: it segfaulted the whole corpus with no output, and it read as a crash
   in the engine rather than as a missing #include, which is the most expensive shape a diagnostic can take.
   -Werror on that one diagnostic makes it a build failure naming the function. */
/* AND THE SAME COMPILER, which is the same argument as ENABLE_DUMPS one level up. This built with gcc while
   every artifact that ships is built by emcc, i.e. clang — so the oracle certified a translation of these
   sources that nothing runs, and the two disagree in exactly the places that matter here. The tree already
   paid for that once: quickjs-step.h's three struct tags are at file scope because each was a fresh type
   belonging to its own prototype, an incompatible-pointer error in every visitor, and "gcc -w hid all three;
   the project's clang build does not". CLAUDE.md records the harder instance — a data pointer in
   JSCFunctionType that passes at -O0 and under gdb and segfaults a whole directory at -O1 with no single file
   reproducing. A gate on the wrong compiler is green through a miscompile that ships, and red on one that does
   not, which is worse: it sends the next reader hunting a phantom. -Wno-unknown-warning-option is what lets
   the gcc-shaped -Wno flags below stay harmlessly. */
const cc = spawnSync("clang", ["-O1", "-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses", "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow", "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer", "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration", "-DNDEBUG", "-D_GNU_SOURCE", "-DCONFIG_VERSION=\"t262\"",
  "-DAPICLIENT_DEV=1", "-DENABLE_DUMPS", "-I.", ...SRCS, "-o", bin, "-lm", "-lpthread"], { cwd: QJS, encoding: "utf8" });
if (cc.status !== 0) { console.error("[test262] build FAILED\n" + (cc.stderr || "")); process.exit(1); }

console.log(`[test262] FEATURE run (forced time-travel, no fallback) on ${sub || "WHOLE CORPUS"}…`);
/* NAMING A DIRECTORY IS ASKING FOR IT. The config excludes whole trees from the GATE — staging/sm asserts
   SpiderMonkey extensions no spec pins — but they are still worth running by hand, which is the reason the
   exclusion comment gives for keeping them. So a run that names a subdirectory re-includes it (-I) while every
   FILE-level exclusion still applies: those are the per-file judgements, each with a stated reason, and they
   are the ones that must survive. A whole-corpus run names nothing and gets every exclusion. */
const args = ["-c", "test262.conf",
              ...(isFile ? ["-f", target] : dir ? ["-d", dir, "-I", dir] : []), ...passthru];
                                                    // cwd=QJS so relative harnessdir resolves
/* THE BUDGET IS CPU, NOT WALL CLOCK — the same correction the WPT gate needed, for the same reason and after the
   same false red. This was `timeout: 590_000`: 590 seconds of ELAPSED time, killed with SIGTERM, and reported
   through a message that lists the three causes it cannot tell apart ("segfault/abort/timeout"). On a box under
   load that is not a fact about the engine at all — a healthy whole-corpus run was reported as DID-NOT-COMPLETE
   with an EMPTY tail, which is the signature of being killed while still running rather than of a crash, since a
   crash leaves the output that preceded it.
   RLIMIT_CPU counts the CPU SECONDS the child consumed, so a run starved by other work is never killed for
   waiting. The wall backstop stays, generously, for a child that consumes no CPU because it is deadlocked.
   The two causes report as DIFFERENT SIGNALS — SIGXCPU from the kernel, SIGTERM from node's own timeout — so the
   summary below can name which one fired instead of collapsing both into a crash. The hard limit is set above
   the soft one because dash makes them equal when given a single value, and the first notification is then the
   kill itself; SIGKILL is read as the CPU cause too. */
const CPU_BUDGET_S = 3600;
const WALL_BACKSTOP_MS = 7_200_000;
/* THE CPU THE CHILD ACTUALLY CONSUMED, MEASURED — because the summary below asserted things about it while
   holding no measurement at all: it called every SIGKILL "the corpus genuinely needs more than this" when
   SIGKILL is the OOM killer's usual signal, and it told the reader a SIGTERM happened "while consuming little
   CPU" having never read a meter. Both are the shape this project keeps finding — a plausible sentence that is
   indistinguishable from a measurement — and the second is its purest form, an assertion about a quantity
   nothing in the program computes. `cutime`/`cstime` accumulate the CPU of REAPED children and spawnSync reaps
   before it returns, so the delta across the call is this child's CPU. */
const cpu0 = childCpuSeconds();
const r = spawnSync("/bin/sh",
  ["-c", `ulimit -H -t ${CPU_BUDGET_S + 60} 2>/dev/null; ulimit -S -t ${CPU_BUDGET_S}; cd "$1" && shift && exec "$@"`,
   "sh", QJS, bin, ...args],
  { cwd: QJS, encoding: "utf8", maxBuffer: 1 << 30,
    env: { ...process.env, FORK_PREEMPT: "1" }, timeout: WALL_BACKSTOP_MS });
const cpuUsed = childCpuDelta(cpu0, childCpuSeconds());
const out = (r.stdout || "") + (r.stderr || "");
/* SINGLE-FILE MODE HAS NO SUMMARY LINE — run-test262 prints one only for a directory run, and a passing file
   prints nothing at all. Reporting the absence of a summary as DID-NOT-COMPLETE told the exact lie this
   harness exists to prevent: a clean run read as a crash. In -f mode the result IS the exit status plus
   whatever the file printed. */
if (isFile) {
  const failed = r.status !== 0 || r.signal ||
                 /^\S+\.js:\d+:/m.test(out) || /@WHY|@E /.test(out);
  const body = out.split("\n").filter((l) => l && !/ignoring testdir/.test(l)).join("\n");
  console.log("\n==================== test262 (feature, one file) ====================");
  console.log(failed ? "  FAIL" : "  PASS");
  if (body) console.log(body);
  if (r.signal) console.log(`  signal=${r.signal}`);
  console.log("====================================================================");
  process.exit(failed ? 1 : 0);
}

const m = out.match(/Result: (\d+)\/(\d+) errors/);
/* WHAT WAS NOT RUN. test262.conf carries an [exclude] list inherited from upstream (intl402, and staging as
   "frequently broken"), and `features` gates the rest — so a "0/43222 errors" summary was printed while
   4786 files were excluded and 5417 skipped, and it read as "the corpus is green" when it meant "the part
   of the corpus we chose is green". Running the excluded staging directory is how four capability gaps
   were found in one sitting: a C-side proxy [[Get]] on a handler, a JSON cycle stack that walked
   Array.prototype, an assert about argc that was simply false, and a TypedArray write coercing from C.
   The numbers are printed so that gap is visible rather than inferred. */
const excl = out.match(/errors?, (\d+) excluded/);
const skip = out.match(/(\d+) skipped/);
/* AND WHICH KIND OF NOT-RUN EACH ONE IS. The line above reported the whole skipped total as "skipped for
   unlisted features", and the unlisted count was ZERO: every one of them was a file matching a deliberate
   `=skip` in test262.conf's [features]. That is the defect this project keeps finding — several states behind
   one number — performed on the very line built to expose it, and the two states it merged fail in opposite
   directions: a deliberate skip is a decision to re-examine when a capability lands, while an unlisted feature
   is a name nobody wrote down, which the next corpus update produces silently and for free.
   run-test262 distinguishes them itself: it prints `unknown feature: X` for a name absent from [features] and
   nothing at all for one carrying `=skip`. So the split is MEASURED here, never inferred. The warning is
   emitted once per unknown feature per file, so its LINE COUNT is not a file count and is not reported as one —
   what it establishes is the NAMES, and whether the number is zero. */
const unknownFeatLines = out.match(/^\S+\.js:\d+: unknown feature: \S+/gm) || [];
const unknownFeats = [...new Set(unknownFeatLines.map((l) => l.replace(/^.*unknown feature: /, "")))].sort();
/* TWO LEAK DETECTORS, AND THIS COUNTED ONLY ONE. `[gcleak]` is the gc_obj_list walk in JS_FreeRuntime — it
   sees a leaked GC OBJECT. run-test262 keeps its OWN malloc accounting and prints `Memory leak: N bytes lost
   in 1 block` for a raw allocation that never came back, which the gc walk cannot see because it is not a GC
   object at all. A parser frame stack, a dbuf, a JSAtom table entry all leak that way.
   Counting only [gcleak] printed "0 leaks" over runs that were emitting hundreds of the other kind. A gate
   that reports a number it did not measure is worse than one that reports nothing. */
const gcLeaks = (out.match(/\[gcleak\]/g) || []).length;
const mallocLeaks = (out.match(/Memory leak: \d+ bytes lost/g) || []).length;
const leaks = gcLeaks + mallocLeaks;
/* ZERO, AND THE 606 WERE ONE MISSING FREE. Turning the second detector on reported 606 raw-allocation leaks,
   the same count the unmodified pre-conversion parser produced, so they were nobody's recent doing and the
   comment here treated them as a body of work to chip away at. They were one line: js_create_function — the
   SUCCESS path of compiling a function — released fd->using_decls and not fd->annexb_vars, while
   js_free_function_def, the failure path, released both. Every compile that RECORDED an Annex B provisional
   store leaked its table, which is any sloppy-mode block-level function declaration; `{ function f() {} }` at
   top level was enough. 342 of the 606 came from annexB/language/eval-code alone.
   The number being large said nothing about the number of causes, and treating it as a backlog is what kept
   it unexamined — a single file bisected to the construct in five probes.
   It ratchets like engine/check_recursion.mjs: MORE is a regression the build refuses, FEWER must be recorded
   here so the gain cannot be given back. At zero the two detectors finally agree, and any raw-allocation leak
   is now as loud as a gc-object one. The ceiling applies only to a whole-corpus run; a subdirectory run
   reports its count and enforces nothing, because the number is not comparable. */
const MALLOC_LEAK_CEILING = 0;
const wholeCorpus = !sub;
/* WHICH TESTS FAILED, not just how many. The summary discarded the names, so a `1/43239 errors` result sent
   the next step into a directory-by-directory hunt for one file. run-test262 already names each failure as
   `<file>:<line>: <message>`; keep the first handful. */
/* `Memory leak` is excluded ANYWHERE in the line, not just right after the `:N: `. run-test262 is
   multi-threaded and two workers can interleave mid-line, producing `fileA:1: fileB:1: Memory leak: …` — a
   lookahead anchored at the prefix lets that through and the summary then lists non-failures as failures. */
/* AND `unknown feature` IS THE SAME TRAP, ONE LINE LATER. run-test262 emits its unlisted-feature warning in the
   IDENTICAL `<file>:<line>: <message>` shape as a failure, so a corpus update that introduces a feature nobody
   has listed would fill this FAILING list with files that did not fail — they were SKIPPED. It is invisible
   while the unlisted count is zero, which is exactly why it has to be excluded now rather than when it fires:
   the verdict is unaffected (that comes from run-test262's own test_failed) so nothing would go red, and the
   only symptom would be a reader hunting phantom failures in files that never ran. */
const failLines = (out.match(/^\S+\.js:\d+: [^\n]*/gm) || [])
  .filter((l) => !/Memory leak/.test(l) && !/unknown feature:/.test(l));
/* FEATURE ENGAGEMENT: a passing result does NOT prove time-travel ran on the test logic. The engine reports
   preempt-requested vs fired; requested>fired means the feature was gated somewhere (nested async/generator
   activation) and the run passed tests it silently SKIPPED — a fake green. A well-engineered test for all
   categories must FAIL on that, regardless of codebase state, not report a hollow 0/N. */
const f = out.match(/Feature: (\d+) preempt-requested, (\d+) fired \(([\d.]+)% engaged\), (\d+) nested-gap/);
/* An explicit NOT-ENGAGED line: the run reached ZERO back-edges, so it exercised no suspend/resume and proves
   nothing about the feature. That is a FAILURE, never a hollow "100%". */
const notEngaged = /Feature: NOT ENGAGED/.test(out);
const d2c = out.match(/DriveToCompletion: (\d+)/);   /* automatic drive-to-completion detector (structural, corpus-wide) */

console.log("\n==================== test262 (feature) ====================");
/* WHICH OF THEM IT WAS, said out loud. "segfault/abort/timeout" named three causes and distinguished none, so a
   run killed on the clock read exactly like a corpus-wide crash — and did, for a healthy binary, under load. */
/* AND `SIGXCPU || SIGKILL` WAS ITSELF A COLLAPSE, one step short of the correction above it. SIGXCPU is the
   kernel naming the SOFT rlimit this driver installed; SIGKILL is that rlimit's HARD escalation OR the OOM
   killer, and only the CPU consumed tells those apart. Reading them as one clause asserted "the corpus
   genuinely needs more than this" — a sentence about the corpus — for a run the kernel had killed for memory.
   The CPU figure is printed on every arm, because it is what the reader needs and what none of them carried. */
const budgetCause =
    (r.signal === "SIGXCPU" && (cpuUsed === null || cpuUsed >= CPU_BUDGET_S - 1)) ||
    (r.signal === "SIGKILL" && cpuUsed !== null && cpuUsed >= CPU_BUDGET_S)
    ? `the ${CPU_BUDGET_S}s CPU budget — ${cpuText(cpuUsed)} of real CPU consumed, not elapsed, so the corpus genuinely needs more than this`
  : r.signal === "SIGXCPU"
    /* The kernel raises SIGXCPU only at the soft rlimit, so a meter reading materially below it contradicts the
       signal. The signal is the kernel's and the number is derived: the number is the one that is wrong. */
    ? `SIGXCPU — the kernel says this child spent the ${CPU_BUDGET_S}s soft rlimit and this driver's meter read ${cpuText(cpuUsed, 2)} for it. Both cannot be true; the meter (engine/gate_cpu.mjs) is the derived one and is wrong. NOTHING here was measured.`
  : r.signal === "SIGKILL"
    ? `SIGKILL after ${cpuText(cpuUsed, 2)} of CPU — ` + (cpuUsed === null
        ? `unmeasured, so this driver cannot tell its own ${CPU_BUDGET_S + 60}s HARD rlimit from an outside kill.`
        : `far below the ${CPU_BUDGET_S}s budget, so it is NOT a limit this driver installed. The OOM killer is the usual one (dmesg names it).`)
  : r.signal === "SIGTERM"
    ? `the ${WALL_BACKSTOP_MS / 1000}s WALL BACKSTOP having consumed ${cpuText(cpuUsed, 2)} of CPU — ` + (cpuUsed === null
        ? "unmeasured, so this driver cannot say whether that is a DEADLOCK or a saturated box. Re-run on a quiet machine before believing it."
        : cpuUsed < 1
        ? "that is a DEADLOCK, not load: the process was asleep waiting for something that never came, NOT a crash."
        : "that is a SATURATED BOX, not a deadlock — it was starved of the thread, NOT a crash. Re-run on a quiet machine before believing it.")
    : null;
if (!m) { console.log(budgetCause
    ? `  DID-NOT-COMPLETE — killed by ${budgetCause}`
    : "  DID-NOT-COMPLETE — a HARD crash before the summary (segfault or abort). FAIL LOUD:");
  /* Dump the captured tail + the child's exit signal/status so a corpus-wide crash names itself instead of hiding
     behind a bare "DID-NOT-COMPLETE" — this is what surfaces the un-routed drive-to-completion / memory bugs. */
  console.log("---- last 60 lines of captured output ----\n" + out.split(/\r?\n/).slice(-60).join("\n"));
  console.log(`---- child signal=${r.signal} status=${r.status}` +
    (r.status === 3221225477 ? " (0xC0000005 ACCESS_VIOLATION — memory bug: run under ASan)" : "") + " ----"); }
else {  console.log(`  ${m[1]}/${m[2]} errors, ${leaks} leaks` +
          (leaks ? ` (${gcLeaks} gc-object, ${mallocLeaks} raw-allocation)` : "") +
          `   (errors = spec-wrong under time-travel; bisect vs d0c2272)`);
        if (failLines.length)
          console.log("  FAILING:\n" + failLines.slice(0, 12).map((l) => "    " + l).join("\n") +
                      (failLines.length > 12 ? `\n    … and ${failLines.length - 12} more` : ""));
        if ((excl && +excl[1] > 0) || (skip && +skip[1] > 0))
          console.log(`  NOT RUN: ${excl ? excl[1] : 0} excluded by test262.conf, ${skip ? skip[1] : 0} skipped ` +
                      `for a feature — the count above is over what remained`);
        if (skip && +skip[1] > 0)
          console.log(unknownFeats.length
            ? `    …of which ${unknownFeats.length} feature name(s) are in NOBODY'S LIST (${unknownFeatLines.length} warning ` +
              `line(s)) — list or =skip them in test262.conf [features]: ${unknownFeats.join(", ")}`
            : `    …NONE for an unlisted feature: every one matches a deliberate =skip in test262.conf [features], ` +
              `each of which names the capability whose arrival retires it —\n` +
              wrapList(confSkipFeatures, 6, "      "));
     }
/* WHICH EXECUTIONS THE DENOMINATOR IS OVER, said at the place the number is read. run-test262 counts test_count
   per EXECUTION, so under a both-modes config the total is executions and under any other it is files — the
   same printed number meaning two different things with nothing to distinguish them. */
if (m) console.log(modeRunsBoth
  ? `  mode=${confMode} — each collected file executed BOTH sloppy and strict unless flagged ` +
    `noStrict/onlyStrict/module/raw, so the count above is EXECUTIONS, not files`
  : `  mode=${confMode}  <-- HALF-RUN: a file flagged none of noStrict/onlyStrict/module/raw is executed in ONE\n` +
    `        mode where test262 INTERPRETING.md "Strict Mode" requires TWO. Those strict executions are in no\n` +
    `        exclusion list and no skip count — the FILE was collected, so the total above looks complete.`);
/* THE BUDGET, MEASURED, ON EVERY RUN AND NOT ONLY WHEN IT FIRES. It was printed only on the kill arms, so the
   one question a reader needs in order to size it — how much of it a healthy run actually spends — was never
   answered by a healthy run. A limit nobody can see the headroom of is a limit that gets raised by guesswork. */
if (m) console.log(cpuUsed === null
  ? `  cpu: ${cpuText(cpuUsed)} — this run's headroom under the ${CPU_BUDGET_S}s budget is therefore UNKNOWN`
  : `  cpu: ${cpuText(cpuUsed, 2)} consumed of the ${CPU_BUDGET_S}s budget`);
if (notEngaged) {
  console.log("  feature: NOT ENGAGED (0 back-edges)  <-- proves NOTHING: no loop ever suspended/resumed in this run");
} else if (f) {
  const [, req, fired, eng, gap] = f;
  const fake = +gap > 0;
  console.log(`  feature: ${eng}% engaged (${fired}/${req} back-edges), ${gap} nested-gap` +
    (fake ? "  <-- FAKE-GREEN: the feature was SKIPPED here (nested async/generator bodies not routable yet)" : ""));
} else if (m) {
  console.log("  feature: NO ENGAGEMENT LINE — harness did not report engagement (FORK_PREEMPT off or old engine)");
}
/* WHAT THIS ZERO DOES AND DOES NOT SAY. The counter answers ONE question: was a BYTECODE BODY entered by C
   recursion while a flow existed. It is blind to C-to-C recursion that never re-enters the interpreter — the
   parser descending on nested parens, JS_ReadObjectRec on a nested structured clone, re_parse_disjunction on a
   crafted pattern. Every one of those is C stack this engine cannot suspend, park or resume, and every one of
   them leaves this counter at zero.
   So it must NOT print "pure suspend/resume-at-any-depth". It did, and that label was read back as evidence of
   resumability it cannot provide — a claim about the whole engine derived from a probe scoped to one path. The
   number that covers the rest is engine/check_recursion.sh, which is STATIC and whole-program; this line now
   says which question it answered and points at the one that answers the others. */
const driveN = d2c ? +d2c[1] : 0;
if (d2c) console.log(`  drive-to-completion: ${driveN}` +
  (driveN > 0 ? "  <-- some coroutine body ran to COMPLETION off-tramp (not suspend/resume) — route it onto the tramp chain"
              : "  (no bytecode body was entered by C recursion under a flow — says NOTHING about C-to-C\n" +
                "                          recursion, which only engine/check_recursion.sh measures)"));
/* THE REVISION IN THE TAIL, because the tail is what gets pasted — see engine/wpt.mjs's summary for the run
   whose nine-atom report was quoted for hours against a tree that had already fixed all three of its roots. */
for (const l of revisionLines(REV_AT_START)) console.log(l);
{
  const moved = revisionMoved(REV_AT_START);
  console.log(moved ? `[rev] AND IT MOVED WHILE THIS RAN — ${moved}`
                    : "[rev] the engine did not move during this run");
}
console.log("===========================================================");
/* fail on: crash, spec errors, leaks, a NOT-ENGAGED run (0 back-edges proves nothing), fake-green (engagement
   < 100%), a HALF-RUN config, OR any drive-to-completion. */
/* A HALF-RUN CANNOT REPORT GREEN. An excluded test is a failure, and a required execution that never happens is
   an excluded test whose file was collected — so the only thing standing between it and a green summary is this
   line. It is gated on a WHOLE-CORPUS run for the same reason the leak ceiling is: a one-off `-d` probe under
   `mode=strict` is a deliberate question about one axis, not the gate, and its number is not comparable. */
const halfRun = wholeCorpus && !!m && !modeRunsBoth;
if (halfRun)
  console.error(`test262.conf has mode=${confMode}: this run performed ONE execution for every file the suite ` +
                `requires TWO of. Set mode=all — a gate that under-runs must not exit 0.`);
const fakeGreen = notEngaged ? true : (f ? (+f[4] > 0) : true);
let leakBad = gcLeaks > 0;
if (wholeCorpus && m) {
  if (mallocLeaks > MALLOC_LEAK_CEILING) {
    console.error(`raw-allocation leaks: ${mallocLeaks} > ceiling ${MALLOC_LEAK_CEILING}. This change leaks.`);
    leakBad = true;
  } else if (mallocLeaks < MALLOC_LEAK_CEILING) {
    console.error(`raw-allocation leaks: ${mallocLeaks} < ceiling ${MALLOC_LEAK_CEILING} — LOWER it in ` +
                  `engine/test262.mjs so the gain cannot be given back.`);
    leakBad = true;
  }
}
process.exit((!m || +m[1] > 0 || leakBad || fakeGreen || driveN > 0 || halfRun) ? 1 : 0);
