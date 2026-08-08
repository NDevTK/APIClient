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
import { existsSync, readdirSync, mkdtempSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, relative, sep } from "node:path";
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
                   "webmessaging",
                   /* HTML §7.2.5.1 AND §7.4 — WindowProxy and popups. `window.open`, `opener`, `parent`,
                      `top`, `frames`, named access, and what a cross-origin WindowProxy may expose. This
                      engine has just grown a WindowProxy member surface and §7.4's open(), and neither had a
                      spec directory: a component whose spec directory is not checked out is a component whose
                      gate cannot fail, which is the same defect as asserting only the spelling that existed
                      when the assertion was written. */
                   "html/browsers/windows", "html/browsers/the-window-object",
                   /* THE HELPERS A CHECKED-OUT TEST'S META BLOCK NAMES, which are not areas and are checked out
                      to BE USED — the same standing as `resources` and `common`. Four webmessaging files were
                      reported as "META script not checked out", which is an EXCLUDED TEST wearing a reason:
                      the gate had decided what it measures and then not measured four of them. Naming the
                      helper's own `resources` directory checks out no test file at all, so nothing is added to
                      the total except the four files that can now run. */
                   "html/browsers/browsing-the-web/remote-context-helper/resources",
                   "html/browsers/browsing-the-web/back-forward-cache/resources",
                   "service-workers/service-worker/resources"];

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

const cc = spawnSync("gcc", ["-O1", "-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses", "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow", "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer", "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration", "-DNDEBUG", "-D_GNU_SOURCE", '-DCONFIG_VERSION="wpt"',
  "-DAPICLIENT_DEV=1", "-DENABLE_DUMPS",
  "-I" + join(ENGINE, "qjs"), "-I" + join(ENGINE, "host"), "-I" + join(ENGINE, "host", "browser"),
  "-I" + join(LEXBOR_SRC, "source"),
  ...SRCS, LEXBOR_LIB, "-o", bin, "-lm", "-lpthread"], { encoding: "utf8" });
if (cc.status !== 0) { console.error("[wpt] runner build FAILED\n" + (cc.stderr || "")); process.exit(1); }

/* Which files to run. A `.any.js` is the portable form — it carries no HTML around it, which is what lets a
   non-browser host run the real file rather than a copy of it.
   A `.window.js` is the same idea for tests that need a WINDOW: WPT wraps it in a minimal document with
   testharness and nothing else, so the file itself is a plain script against a Window global — which is what
   this runner provides. It was excluded, and the cost of that was invisible: html/browsers/windows and
   html/browsers/the-window-object contain ZERO .any.js files and twenty .window.js ones, so checking those
   directories out changed no total at all. self-et-al.window.js is the WindowProxy identity test — window,
   self, frames, parent and top all naming one object — and it is precisely the gate this engine's new proxy
   surface needs. */
/* AND `.html`, WHICH IS MOST OF THE CORPUS. 523 of the 778 test files checked out here are HTML documents, and
   this collector took none of them: they were not run, not reported and not counted, so the total LOOKED
   complete while two thirds of the corpus was excluded. An excluded test is a failure whatever the reason —
   the same defect as a directory that is never checked out, and harder to notice.
   WHICH .html FILES ARE TESTS is not a naming convention, and guessing from one would take reftest references
   and support pages as tests. WPT's own manifest classifies by what the file LOADS: a testharness test is a
   document that pulls in /resources/testharness.js. That is the check, read from the bytes. */
function isTestDocument(p) {
  try { return readFileSync(p, "utf8").includes("/resources/testharness.js"); } catch { return false; }
}
function collect(dir, out) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) collect(p, out);
    else if (e.name.endsWith(".any.js") || e.name.endsWith(".window.js")) out.push(p);
    else if (e.name.endsWith(".html") && isTestDocument(p)) out.push(p);
  }
  return out;
}
const arg = process.argv[2] || "";
/* A NO-ARGUMENT RUN IS EVERY PATH THAT IS CHECKED OUT, not one hard-coded directory. It said "fetch", so
   widening WPT_PATHS to `url` checked the corpus out and then never ran it — the gate reported the same number
   as before and looked like the new component had changed nothing. The default is derived from WPT_PATHS for
   the same reason the META scripts are read from the file: the two must not be able to disagree. */
const root = arg ? join(WPT, arg) : WPT;
const files = (arg.endsWith(".js") || arg.endsWith(".html")) ? [root]
            : arg ? collect(root, []).sort()
            /* `resources`, `common` and `tools` are checked out to BE USED, not to be tested: testharness.js and
               its helpers, and WPT's own server. `tools` carries the harness's SELF-tests, which reference
               testharness.js and therefore look like tests to the collector — 27 files of them, measuring
               wptserve rather than this engine. Excluding a support path is not excluding a test. */
            /* A `resources` DIRECTORY IS A SUPPORT PATH WHEREVER IT SITS, not only at the root: the helper
               directories a META block names carry support documents that pull in testharness.js, and
               collecting those would count another spec's fixtures as this gate's tests. The rule is the
               path's LAST SEGMENT, so a helper added later is excluded by the same sentence. */
            : WPT_PATHS.filter((p) => p !== "common" && p !== "tools" &&
                                      p !== "resources" && !p.endsWith("/resources"))
                       .flatMap((p) => collect(join(WPT, p), [])).sort();
if (!files.length) { console.error(`[wpt] no test files under ${root}`); process.exit(1); }

/* The AREA a file belongs to: the checked-out path it lives under, which is what WPT_PATHS names. */
const areas = new Map();
function byArea(rel) {
  const p = WPT_PATHS.find((d) => rel === d || rel.startsWith(d + "/")) || rel.split("/")[0];
  let a = areas.get(p);
  if (!a) areas.set(p, (a = { files: 0, pass: 0, fail: 0, aborted: 0 }));
  return a;
}

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
/* A `.sub.js` META SCRIPT IS NOT ITS BYTES ON DISK. wptserve SUBSTITUTES `{{host}}`, `{{ports[http][0]}}`
   and friends when it serves one, and the whole point of common/get-host-info.sub.js is to hand a test the
   REAL alternate hosts and ports of the server it is running against. Read off disk it hands back the
   placeholders instead, so `get_host_info().HTTP_REMOTE_ORIGIN` is the literal string `http://{{hosts[alt][]}}`
   — which is not a URL, which is why every cross-origin `window.open` in the corpus failed with "the URL to
   open is not a URL" and every test built on one timed out. That is a GATE defect reported as an engine one.
   Everything a test FETCHES already goes through the real server; a META script is the one input the driver
   hands over itself, so it is the one that has to be fetched here. Memoized: the same helper is named by many
   files and substitution is the server's work, not this loop's. */
const g_subbed = new Map();
async function substituted(dep) {
  if (!/\.sub\.[a-z]+$/.test(dep)) return dep;
  if (g_subbed.has(dep)) return g_subbed.get(dep);
  const path = "/" + relative(WPT, dep).split(sep).join("/");
  const r = await fetch("http://" + serverAddr + path);
  if (!r.ok) return dep;   /* the corpus server does not serve it; the missing-dep report below names it */
  const out = join(dirname(bin), relative(WPT, dep).split(sep).join("__"));
  writeFileSync(out, await r.text());
  g_subbed.set(dep, out);
  return out;
}

let pass = 0, fail = 0, aborted = 0;
const failures = [];

for (const f of files) {
  const rel = relative(WPT, f);
  byArea(rel).files++;
  const deps = (await Promise.all(metaScripts(f).map(substituted)));
  const missing = deps.filter((d) => !existsSync(d));
  if (missing.length) {
    /* A META script the sparse checkout does not have is a GATE defect, not a test result: the file would run
       against a corpus it was not written for. Name the paths so WPT_PATHS can be widened. */
    aborted++; byArea(rel).aborted++;
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
  /* THE TWO @WHY SHAPES, because there are two emitters and losing one loses the name. The HOST's check.h
     prints a machine-readable JSON line whose `reason` carries the message; the ENGINE's quickjs-check.h prints
     `@WHY <msg> (file:line)` as plain text. Matching only the first reported the second as a bare
     "signal SIGABRT" — an abort with the name thrown away, which is the one thing an abort is FOR. It cost a
     round trip per diagnosis and taught nothing on its own. */
  const why = out.match(/@WHY .*"reason":"([^"]*)/) || out.match(/^@WHY (.+)$/m);
  const abortedHere = Boolean(why || r.signal);
  if (abortedHere) {
    aborted++; byArea(rel).aborted++;
    failures.push(`  ABORT  ${rel}\n         ${why ? why[1].slice(0, 160) : "signal " + r.signal}`);
  }
  /* A TEST THAT ASKS FOR A wptserve HANDLER cannot run here, and that is the GATE's limitation rather than a
     result about the engine. WPT's server imports the named `.py` and calls its `main`; this runner serves the
     corpus off disk. Counting those as engine failures would put a number on this runner's reach and call it
     the browser's. */
  const handler = out.match(/^@WPTHANDLER (.*)$/m);
  if (handler) {
    aborted++; byArea(rel).aborted++;
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
    aborted++; byArea(rel).aborted++;
    failures.push(`  ERROR  ${rel}\n         ${err[1].slice(0, 200)}`);
    continue;
  }
  /* A FILE THAT NEVER COMPLETED IS NOT A FILE WITH NOTHING IN IT. testharness reports through its completion
     callback, so a run that ends without @WPTDONE — an async test that never settles, a harness that never
     reached all_done — emitted no @WPT lines at all and contributed ZERO to every column. It read as though the
     file held no tests, which is the same silent truncation as leaving a directory out of the checkout: the
     number goes down and nothing says why. It is counted and NAMED. */
  if (!abortedHere && !/^@WPTDONE /m.test(out)) {
    aborted++; byArea(rel).aborted++;
    failures.push(`  ABORT  ${rel}\n         the harness never completed — no @WPTDONE, so its ` +
                  `${filePass + fileFail} reported subtest(s) are not the whole file`);
    continue;
  }
  pass += filePass;
  fail += fileFail;
  byArea(rel).pass += filePass;
  byArea(rel).fail += fileFail;
}

/* PER-AREA, NOT ONE NUMBER. `encoding` alone answers three quarters of a million subtests — one per code point
   per legacy encoder — so a single total is a number in which every other area is invisible: a component that
   lost a hundred subtests and one that gained them read identically. The areas are the checked-out paths, which
   is the same list that decides what runs, so the breakdown cannot drift from the corpus. */
console.log("\n==================== web-platform-tests ====================");
for (const l of failures) console.log(l);
{
  const names = [...areas.keys()].sort();
  const w = Math.max(...names.map((n) => n.length));
  for (const n of names) {
    const a = areas.get(n);
    console.log(`  ${n.padEnd(w)}  files ${String(a.files).padStart(4)}  pass ${String(a.pass).padStart(7)}` +
                `  fail ${String(a.fail).padStart(7)}  aborted ${String(a.aborted).padStart(3)}`);
  }
}
console.log(`  files ${files.length}   subtests ${pass + fail}   pass ${pass}   fail ${fail}   aborted-files ${aborted}`);
console.log("===========================================================");
process.exit(fail || aborted ? 1 : 0);
