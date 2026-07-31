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
import { mkdtempSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";

const QJS = join(import.meta.dirname, "qjs");
const TESTS = join(import.meta.dirname, "tests");
const SRCS = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c", "quickjs-libc.c", "run-test262.c"];

console.log("[features] building native run-test262 (gcc, NDEBUG)…");
const bin = join(mkdtempSync(join(tmpdir(), "feat-")), "run262.exe");
const cc = spawnSync("gcc", ["-O1", "-w", "-DNDEBUG", "-D_GNU_SOURCE", "-DCONFIG_VERSION=\"t262\"",
  "-DAPICLIENT_DEV=1", "-I.", ...SRCS, "-o", bin, "-lm", "-lpthread"], { cwd: QJS, encoding: "utf8" });
if (cc.status !== 0) { console.error("[features] build FAILED\n" + (cc.stderr || "")); process.exit(1); }

const r = spawnSync(bin, ["-c", "test262.conf", "-d", TESTS], {
  cwd: QJS, encoding: "utf8", maxBuffer: 1 << 28,
  env: { ...process.env, FORK_PREEMPT: "1" }, timeout: 1_800_000,
});
const out = (r.stdout || "") + (r.stderr || "");

const why = out.match(/@WHY .*/);
const m = out.match(/Result: (\d+)\/(\d+) errors?/);
const feat = out.match(/Feature: (\d+) preempt-requested, (\d+) fired/);
const sync = out.match(/SyncDriveToCompletion: (\d+)/);

for (const line of out.split("\n")) if (/unexpected error/.test(line)) console.log("  " + line.trim());
if (why) console.log("  " + why[0]);

console.log("\n==================== fork capabilities ====================");
console.log(`  ${m ? `${m[1]}/${m[2]} errors` : "NO RESULT — the run died"}`);
/* Engagement is reported for the same reason it is on the corpus: a green result proves nothing the run did
   not exercise, and these files exist to exercise the suspending paths. */
if (feat) {
  const [, req, fired] = feat;
  const pct = req === "0" ? 0 : (100 * Number(fired)) / Number(req);
  console.log(`  feature: ${pct.toFixed(1)}% engaged (${fired}/${req} back-edges)`);
}
if (sync) console.log(`  drive-to-completion: ${sync[1]}  (>0 = a body ran by C recursion, unable to suspend)`);
console.log("===========================================================");

process.exit(why || !m || m[1] !== "0" ? 1 : 0);
