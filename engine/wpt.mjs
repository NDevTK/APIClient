/* WEB-PLATFORM-TESTS — the browser half's correctness gate.
 *
 * WHY: test262 is the JS half's oracle, and the browser half had nothing equivalent. Its only checks were the
 * IDL audit, which measures COVERAGE and can only say what is ABSENT, and a fixture of probes written by
 * whoever wrote the component — which tests what that person already thought of. Both missed the same things:
 * the audit had no row for Headers at all, then reported it COMPLETE with four members missing, and the fixture
 * agreed because I wrote both. WPT is written by the people who wrote the spec, so it disagrees.
 *
 * ONE PROCESS PER TEST FILE, deliberately. A DCHECK is an abort — that is the whole point of it — so a runner
 * that ran the corpus in one process would report the first unbuilt capability and nothing after it. Per-file
 * isolation makes an abort a RESULT for that file and leaves the rest of the picture intact, which is the
 * difference between a gate and a bisect.
 *
 * The corpus is pinned, like lexbor and test262: a moving corpus turns a regression into an argument.
 *
 * Usage:  node engine/wpt.mjs [subdir-or-file]
 */
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync, mkdtempSync } from "node:fs";
import { join, relative } from "node:path";
import { tmpdir } from "node:os";

const ENGINE = import.meta.dirname;
const WORK = join(ENGINE, ".work");
const WPT = join(WORK, "wpt");
/* PINNED. The corpus is an oracle, and an oracle that changes under you turns "this regressed" into "did it?".
   Re-pin deliberately, as a commit of its own, so the diff in results has one cause. */
const WPT_REV = "bf4714d";
/* WHAT IS CHECKED OUT. A sparse list rather than the whole 1 GB tree, and it grows as areas are covered — an
   area absent here is honestly untested, which is a different statement from "passes". */
const WPT_PATHS = ["resources", "fetch/api/headers"];

if (!existsSync(join(WPT, "resources", "testharness.js"))) {
  console.error("[wpt] corpus missing — provision it with:\n" +
    `  git clone --filter=blob:none --sparse --depth 1 https://github.com/web-platform-tests/wpt.git ${WPT}\n` +
    `  cd ${WPT} && git sparse-checkout set ${WPT_PATHS.join(" ")} && git checkout ${WPT_REV}`);
  process.exit(1);
}

/* THE RUNNER IS BUILT NATIVELY, like run-test262 and for the same reason: the gate is run per change, and an
   eight-minute wasm link per iteration is a gate nobody runs. The flags mirror the shipped build where they
   change the engine — ENABLE_DUMPS alters the interpreter's dispatch macros, and building the oracle without it
   tested a different interpreter once already. */
const SRCS = [
  "qjs/quickjs.c", "qjs/libregexp.c", "qjs/libunicode.c", "qjs/dtoa.c",
  "host/solver/concolic.c", "host/solver/flow.c", "host/solver/absent.c",
  "host/browser/core/idl_args.c",
  "host/browser/core/fetch/headers.c",
  "host/wpt_runner.c",
].map((f) => join(ENGINE, f));

console.log("[wpt] building the native runner…");
const bin = join(mkdtempSync(join(tmpdir(), "wpt-")), "wpt.exe");
const cc = spawnSync("gcc", ["-O1", "-w", "-DNDEBUG", "-D_GNU_SOURCE", '-DCONFIG_VERSION="wpt"',
  "-DAPICLIENT_DEV=1", "-DENABLE_DUMPS",
  "-I" + join(ENGINE, "qjs"), "-I" + join(ENGINE, "host"), "-I" + join(ENGINE, "host", "browser"),
  ...SRCS, "-o", bin, "-lm", "-lpthread"], { encoding: "utf8" });
if (cc.status !== 0) { console.error("[wpt] runner build FAILED\n" + (cc.stderr || "")); process.exit(1); }

/* Which files to run. A `.any.js` is the portable form — it carries no HTML around it, which is what lets a
   non-browser host run the real file rather than a copy of it. */
function collect(dir, out) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) collect(p, out);
    else if (e.name.endsWith(".any.js")) out.push(p);
  }
  return out;
}
const arg = process.argv[2] || "";
const root = arg ? join(WPT, arg) : join(WPT, "fetch");
const files = arg.endsWith(".js") ? [root] : collect(root, []).sort();
if (!files.length) { console.error(`[wpt] no .any.js under ${root}`); process.exit(1); }

const HARNESS = join(WPT, "resources", "testharness.js");
let pass = 0, fail = 0, aborted = 0;
const failures = [];

for (const f of files) {
  const rel = relative(WPT, f);
  const r = spawnSync(bin, [HARNESS, f], { encoding: "utf8", maxBuffer: 1 << 28, timeout: 60_000 });
  const out = (r.stdout || "") + (r.stderr || "");
  /* An ABORT is a result about this file, not an accident: it is a DCHECK naming a capability the browser half
     does not have, which is exactly what this gate is for. It is counted apart from a FAIL because the two ask
     for different work — a fail is a wrong answer, an abort is a missing one. */
  const why = out.match(/@WHY .*"reason":"([^"]*)/);
  if (why || r.signal) {
    aborted++;
    failures.push(`  ABORT  ${rel}\n         ${why ? why[1].slice(0, 160) : "signal " + r.signal}`);
    continue;
  }
  let filePass = 0, fileFail = 0;
  for (const line of out.split("\n")) {
    const m = line.match(/^@WPT (\{.*\})$/);
    if (!m) continue;
    let t; try { t = JSON.parse(m[1]); } catch { continue; }
    if (t.status === 0) { filePass++; }
    else { fileFail++; failures.push(`  FAIL   ${rel} :: ${t.name}\n         ${(t.message || "").slice(0, 200)}`); }
  }
  const err = out.match(/^@WPTERR (.*)$/m);
  if (err && !filePass && !fileFail) {
    aborted++;
    failures.push(`  ERROR  ${rel}\n         ${err[1].slice(0, 200)}`);
    continue;
  }
  pass += filePass;
  fail += fileFail;
}

console.log("\n==================== web-platform-tests ====================");
for (const l of failures) console.log(l);
console.log(`  files ${files.length}   subtests ${pass + fail}   pass ${pass}   fail ${fail}   aborted-files ${aborted}`);
console.log("===========================================================");
process.exit(fail || aborted ? 1 : 0);
