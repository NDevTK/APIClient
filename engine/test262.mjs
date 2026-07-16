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
import { existsSync, mkdtempSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";

const QJS = join(import.meta.dirname, "qjs");
const CORPUS = join(QJS, "test262", "test");
const sub = process.argv[2] || "";
const dir = sub ? join(CORPUS, sub) : null;
const SRCS = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c", "quickjs-libc.c", "run-test262.c"];

if (!existsSync(CORPUS)) {
  console.error("[test262] corpus missing — check it out:\n" +
    "  cd engine/qjs && git -c submodule.test262.update=checkout submodule update --init --depth 1 test262");
  process.exit(1);
}

console.log("[test262] building native run-test262 (gcc, NDEBUG)…");
const bin = join(mkdtempSync(join(tmpdir(), "t262-")), "run262.exe");
const cc = spawnSync("gcc", ["-O1", "-w", "-DNDEBUG", "-D_GNU_SOURCE", "-DCONFIG_VERSION=\"t262\"",
  "-DAPICLIENT_DEV=1", "-I.", ...SRCS, "-o", bin, "-lm", "-lpthread"], { cwd: QJS, encoding: "utf8" });
if (cc.status !== 0) { console.error("[test262] build FAILED\n" + (cc.stderr || "")); process.exit(1); }

console.log(`[test262] FEATURE run (forced time-travel, no fallback) on ${sub || "WHOLE CORPUS"}…`);
const args = ["-c", "test262.conf", ...(dir ? ["-d", dir] : [])];   // cwd=QJS so relative harnessdir resolves
const r = spawnSync(bin, args, { cwd: QJS, encoding: "utf8", maxBuffer: 1 << 30,
  env: { ...process.env, FORK_PREEMPT: "1" }, timeout: 590_000 });
const out = (r.stdout || "") + (r.stderr || "");
const m = out.match(/Result: (\d+)\/(\d+) errors/);
const leaks = (out.match(/\[gcleak\]/g) || []).length;
/* FEATURE ENGAGEMENT: a passing result does NOT prove time-travel ran on the test logic. The engine reports
   preempt-requested vs fired; requested>fired means the feature was gated somewhere (nested async/generator
   activation) and the run passed tests it silently SKIPPED — a fake green. A well-engineered test for all
   categories must FAIL on that, regardless of codebase state, not report a hollow 0/N. */
const f = out.match(/Feature: (\d+) preempt-requested, (\d+) fired \(([\d.]+)% engaged\), (\d+) nested-gap/);

console.log("\n==================== test262 (feature) ====================");
if (!m) console.log("  DID-NOT-COMPLETE (crash before the summary — a hard bug: see stderr)");
else    console.log(`  ${m[1]}/${m[2]} errors, ${leaks} leaks   (errors = spec-wrong under time-travel; bisect vs d0c2272)`);
if (f) {
  const [, req, fired, eng, gap] = f;
  const fake = +gap > 0;
  console.log(`  feature: ${eng}% engaged (${fired}/${req} back-edges), ${gap} nested-gap` +
    (fake ? "  <-- FAKE-GREEN: the feature was SKIPPED here (nested async/generator bodies not routable yet)" : ""));
} else if (m) {
  console.log("  feature: NO ENGAGEMENT LINE — harness did not report engagement (FORK_PREEMPT off or old engine)");
}
console.log("===========================================================");
/* fail on: crash, spec errors, leaks, OR fake-green (feature gated -> engagement < 100%). */
const fakeGreen = f ? (+f[4] > 0) : true;
process.exit((!m || +m[1] > 0 || leaks > 0 || fakeGreen) ? 1 : 0);
