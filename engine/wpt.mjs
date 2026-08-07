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
import { spawnSync, spawn } from "node:child_process";
import { existsSync, readdirSync, mkdtempSync, mkdirSync, readFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { tmpdir, cpus } from "node:os";

function walk(dir) {
  const out = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...walk(p));
    else if (e.name.endsWith(".c")) out.push(p);
  }
  return out;
}

const ENGINE = import.meta.dirname;
const WORK = join(ENGINE, ".work");
const WPT = join(WORK, "wpt");
/* PINNED. The corpus is an oracle, and an oracle that changes under you turns "this regressed" into "did it?".
   Re-pin deliberately, as a commit of its own, so the diff in results has one cause. */
const WPT_REV = "bf4714d";
/* WHAT IS CHECKED OUT. A sparse list rather than the whole 1 GB tree, and it grows as areas are covered — an
   area absent here is honestly untested, which is a different statement from "passes". */
/* `tools` is not a test area — it is WPT'S OWN SERVER and its vendored dependencies. The corpus is SERVED by
   it rather than read off disk, because a `.py` path is a handler the server imports and calls, and because
   the rewrites and content types are then wptserve's own rather than a table copied into this file. */
const WPT_PATHS = ["resources", "fetch/api/headers", "fetch/api/response", "fetch/api/resources", "url", "common",
                   "FileAPI/blob", "FileAPI/file", "FileAPI/support", "FileAPI/url", "encoding", "tools",
                   /* THE COMPONENT'S OWN SPEC TESTS. readable_stream.c was written, and then measured against
                      fetch and FileAPI — which use a stream but assert almost nothing ABOUT one. A component
                      whose spec directory is not checked out is a component whose gate cannot fail, which is
                      the same defect as a gate that only reads the spelling that existed when it was written. */
                   "streams/readable-streams", "streams/resources", "streams/writable-streams",
                   "streams/piping", "streams/transform-streams",
                   /* HTML 9.4's MESSAGING. Cross-document and cross-worker messaging is where popups, iframes
                      and this engine's one-WASM-instance-per-DOCUMENT rule meet, and it is also where the
                      solver's `message.origin` attacker source comes from. None of it exists yet, so this
                      directory is the honest measurement of that: a component whose spec directory is not
                      checked out is a component whose gate cannot fail. */
                   "webmessaging"];

if (!existsSync(join(WPT, "resources", "testharness.js"))) {
  /* NO --depth 1. The corpus is PINNED, and a depth-1 clone has only the tip — `git checkout bf4714d` in it
     fails with "reference is not a tree", so the instructions this gate printed could not be followed. The
     blob filter is what keeps a full-history clone cheap: it fetches commits and trees, and file contents only
     for the paths the sparse checkout names. */
  console.error("[wpt] corpus missing — provision it with:\n" +
    `  git clone --filter=blob:none --sparse https://github.com/web-platform-tests/wpt.git ${WPT}\n` +
    `  cd ${WPT} && git sparse-checkout set ${WPT_PATHS.join(" ")} && git checkout ${WPT_REV}`);
  process.exit(1);
}

/* THE CHECKOUT IS ENFORCED, NOT ASSUMED. WPT_PATHS is this gate's statement of what it measures, and until now
   nothing made the working tree agree with it: the presence of testharness.js was the whole test, so a corpus
   missing an entire directory answered "provisioned" and every file in that directory silently stopped being
   run. That is the same defect the list's own comment names one line up — a directory that is not checked out
   is a directory whose gate cannot fail — except that here the gate LOOKED green rather than looking absent,
   which is worse. It happened: `webmessaging` was added to this list, measured at 0/52, and then reverted out
   of the working tree by an unrelated restore; the next run reported the same total as before and nothing said
   the area had gone. `sparse-checkout set` is idempotent and takes about a second when the list already
   matches, so it runs every time rather than being guarded by a check that can itself be wrong. */
{
  const set = spawnSync("git", ["sparse-checkout", "set", ...WPT_PATHS], { cwd: WPT, encoding: "utf8" });
  if (set.status !== 0) {
    console.error("[wpt] could not apply the sparse checkout this gate measures:\n" + (set.stderr || set.stdout));
    process.exit(1);
  }
  const missing = WPT_PATHS.filter((p) => !existsSync(join(WPT, p)));
  if (missing.length) {
    console.error(`[wpt] the corpus has no ${missing.join(", ")} — the pinned revision ${WPT_REV} does not ` +
                  "contain it, so this gate would report a total that silently excludes it");
    process.exit(1);
  }
}

/* THE RUNNER IS BUILT NATIVELY, like run-test262 and for the same reason: the gate is run per change, and an
   eight-minute wasm link per iteration is a gate nobody runs. The flags mirror the shipped build where they
   change the engine — ENABLE_DUMPS alters the interpreter's dispatch macros, and building the oracle without it
   tested a different interpreter once already. */
/* EVERY BROWSER AND SOLVER SOURCE, not a hand-picked list. The list was picked per component, and each new one
   arrived by chasing undefined symbols until the link succeeded — which is a list that only ever describes what
   was needed last time. Streams §5.4's AbortSignal is what made that untenable: it is an EventTarget, which
   reaches the solver's decision hook, which reaches the scheduler, whose COW covers the DOM. The gate links
   what the project SHIPS, and a component that cannot be linked is a component that cannot be tested. */
const SRCS = [
  "qjs/quickjs.c", "qjs/libregexp.c", "qjs/libunicode.c", "qjs/dtoa.c",
  ...walk(join(ENGINE, "host", "browser")).map((f) => relative(ENGINE, f)),
  ...walk(join(ENGINE, "host", "solver")).map((f) => relative(ENGINE, f)),
  "host/wpt_runner.c",
].map((f) => join(ENGINE, f));

console.log("[wpt] building the native runner…");
const bin = join(mkdtempSync(join(tmpdir(), "wpt-")), "wpt.exe");
/* `-w` SILENCES STYLE, NEVER A MISSING PROTOTYPE. C89's implicit-declaration rule assumes `int (...)`, so a
   function whose header was not included returns a 32-bit value — and a 64-bit POINTER comes back TRUNCATED.
   That is not a warning-shaped problem: it segfaulted the whole corpus with no output, and it read as a crash
   in the engine rather than as a missing #include, which is the most expensive shape a diagnostic can take.
   -Werror on that one diagnostic makes it a build failure naming the function. */
/* LEXBOR, BUILT NATIVELY ONCE. Streams §5.4 gives every writable controller a real AbortSignal, which is an
   EventTarget, which reaches the solver's decision hook and through it the scheduler — and the scheduler's COW
   covers the DOM, so the gate needs the same tree the shipped build has. Vendored and built on first use, like
   the corpus itself; the .a is committed to nothing and rebuilt if the source moves. */
const LEXBOR_SRC = join(WORK, "lexbor-src");
const LEXBOR_BUILD = join(WORK, "lexbor-native");
const LEXBOR_LIB = join(LEXBOR_BUILD, "liblexbor_static.a");
if (!existsSync(LEXBOR_LIB)) {
  console.log("[wpt] building lexbor natively (once)…");
  mkdirSync(LEXBOR_BUILD, { recursive: true });
  for (const [cmd, args] of [["cmake", ["-DCMAKE_BUILD_TYPE=Release", "-DLEXBOR_BUILD_SHARED=OFF",
                                        "-DLEXBOR_BUILD_STATIC=ON", "-DLEXBOR_BUILD_TESTS=OFF",
                                        "-DLEXBOR_BUILD_EXAMPLES=OFF", LEXBOR_SRC]],
                             ["make", ["-j" + (cpus().length || 4)]]]) {
    const b = spawnSync(cmd, args, { cwd: LEXBOR_BUILD, encoding: "utf8" });
    if (b.status !== 0) { console.error(`[wpt] lexbor ${cmd} FAILED\n` + (b.stderr || "")); process.exit(1); }
  }
}

const cc = spawnSync("gcc", ["-O1", "-w", "-Werror=implicit-function-declaration", "-DNDEBUG", "-D_GNU_SOURCE", '-DCONFIG_VERSION="wpt"',
  "-DAPICLIENT_DEV=1", "-DENABLE_DUMPS",
  "-I" + join(ENGINE, "qjs"), "-I" + join(ENGINE, "host"), "-I" + join(ENGINE, "host", "browser"),
  "-I" + join(LEXBOR_SRC, "source"),
  ...SRCS, LEXBOR_LIB, "-o", bin, "-lm", "-lpthread"], { encoding: "utf8" });
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
/* A NO-ARGUMENT RUN IS EVERY PATH THAT IS CHECKED OUT, not one hard-coded directory. It said "fetch", so
   widening WPT_PATHS to `url` checked the corpus out and then never ran it — the gate reported the same number
   as before and looked like the new component had changed nothing. The default is derived from WPT_PATHS for
   the same reason the META scripts are read from the file: the two must not be able to disagree. */
const root = arg ? join(WPT, arg) : WPT;
const files = arg.endsWith(".js") ? [root]
            : arg ? collect(root, []).sort()
            : WPT_PATHS.filter((p) => p !== "resources" && p !== "common")
                       .flatMap((p) => collect(join(WPT, p), [])).sort();
if (!files.length) { console.error(`[wpt] no .any.js under ${root}`); process.exit(1); }

const HARNESS = join(WPT, "resources", "testharness.js");

/* WPT'S OWN SERVER, started once for the run. Everything the corpus fetches goes through it, so the handlers
   are the real ones and the rewrites are the real ones. */
const server = spawn("python3", [join(ENGINE, "wptserve.py"), WPT, "0"], { stdio: ["ignore", "pipe", "pipe"] });
const serverAddr = await new Promise((resolve, reject) => {
  let out = "";
  const fail = setTimeout(() => reject(new Error("wptserve did not report READY within 60s")), 60_000);
  server.stdout.on("data", (d) => {
    out += d;
    const m = /READY (\d+)/.exec(out);
    if (m) { clearTimeout(fail); resolve("127.0.0.1:" + m[1]); }
  });
  server.on("exit", (c) => { clearTimeout(fail); reject(new Error("wptserve exited with " + c)); });
}).catch((e) => { console.error("[wpt] " + e.message); process.exit(1); });
process.on("exit", () => server.kill());

/* WPT's server does not serve every path from a file of that name. This is its rewrite table (tools/serve's
   `rewrites`), reproduced for the entries the corpus actually asks for: /resources/WebIDLParser.js is the
   webidl2 library under its historical name. A driver that skipped this would report the file as missing —
   which is what it did — rather than as served from somewhere else. */
const SERVER_REWRITES = {
  "/resources/WebIDLParser.js": "/resources/webidl2/lib/webidl2.js",
};

/* `// META: script=` IS PART OF THE FILE. WPT's server reads those lines and emits a wrapper that loads each
   named script before the test — a file whose META names idlharness.js is not a file that happens to want it,
   it is a file that does not run without it. Reading them here is what makes the corpus run AS WRITTEN; the
   runner keeps its one job of executing a list of programs in order.
   A `/`-rooted path is WPT-root-relative and anything else is relative to the test's own directory, which is
   the server's own resolution rule. */
function metaScripts(file) {
  const src = readFileSync(file, "utf8");
  const out = [];
  for (const line of src.split("\n")) {
    if (!line.startsWith("// META:")) {
      if (line.startsWith("//") || line.trim() === "") continue;
      break;   /* the META block is a prefix; past it the file is code */
    }
    const m = line.match(/^\/\/ META:\s*script=(.*)$/);
    if (!m) continue;
    /* THE REWRITE APPLIES HERE AND NOWHERE ELSE. A META script is a PROGRAM INPUT — the driver hands the runner
     its file to execute before the test — so this path is resolved on disk and needs wptserve's rewrite table
     to find /resources/WebIDLParser.js, which is the webidl2 library under its historical name. Everything the
     test FETCHES goes through the real server, which applies its own rewrites; this table is not a second copy
     of those, it is the one entry the driver itself must resolve. */
  const ref = SERVER_REWRITES[m[1].trim()] || m[1].trim();
    out.push(ref.startsWith("/") ? join(WPT, ref.slice(1)) : join(dirname(file), ref));
  }
  return out;
}
let pass = 0, fail = 0, aborted = 0;
const failures = [];

for (const f of files) {
  const rel = relative(WPT, f);
  const deps = metaScripts(f);
  const missing = deps.filter((d) => !existsSync(d));
  if (missing.length) {
    /* A META script the sparse checkout does not have is a GATE defect, not a test result: the file would run
       against a corpus it was not written for. Name the paths so WPT_PATHS can be widened. */
    aborted++;
    failures.push(`  ABORT  ${rel}\n         META script not checked out: ${missing.map((d) => relative(WPT, d)).join(", ")}`);
    continue;
  }
  const r = spawnSync(bin, [HARNESS, ...deps, f],
                      { encoding: "utf8", maxBuffer: 1 << 28, timeout: 60_000,
                        env: { ...process.env, WPT_SERVER: serverAddr } });
  const out = (r.stdout || "") + (r.stderr || "");
  /* An ABORT is a result about this file, not an accident: it is a DCHECK naming a capability the browser half
     does not have, which is exactly what this gate is for. It is counted apart from a FAIL because the two ask
     for different work — a fail is a wrong answer, an abort is a missing one. */
  /* AN ABORT DOES NOT ERASE WHAT THE FILE ALREADY REPORTED. A DCHECK ends the process, but the results printed
     before it are results — and discarding them here counted a file that had failed four subtests and then
     leaked as a file that produced NOTHING, which is the "same failures with the count hidden" shape this gate
     exists to avoid. It hid them from me, too: I diagnosed that file as ending with no results and went looking
     for what it was waiting on. The abort is reported AND the subtests are counted; they are different facts. */
  const why = out.match(/@WHY .*"reason":"([^"]*)/);
  const abortedHere = Boolean(why || r.signal);
  if (abortedHere) {
    aborted++;
    failures.push(`  ABORT  ${rel}\n         ${why ? why[1].slice(0, 160) : "signal " + r.signal}`);
  }
  /* A TEST THAT ASKS FOR A wptserve HANDLER cannot run here, and that is the GATE's limitation rather than a
     result about the engine. WPT's server imports the named `.py` and calls its `main`; this runner serves the
     corpus off disk. Counting those as engine failures would put a number on this runner's reach and call it
     the browser's. */
  const handler = out.match(/^@WPTHANDLER (.*)$/m);
  if (handler) {
    aborted++;
    failures.push(`  ABORT  ${rel}\n         needs the wptserve handler ${handler[1]}, which this runner cannot execute`);
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
  if (!abortedHere && err && !filePass && !fileFail) {
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
