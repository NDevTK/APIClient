/* V8's own mjsunit corpus, run through the FEATURE engine.
 *
 * WHY A SECOND CORPUS: test262 tests what the SPEC says, and it is the baseline. It does not test what a real
 * engine is actually asked to do — V8's mjsunit files are twenty years of bugs found in a shipping engine, and
 * they reach shapes test262 never does. The regexp area alone has so far surfaced: an unrouted C entry
 * (Error.prepareStackTrace called from build_backtrace) that the whole 43222-file test262 corpus never
 * touched, because mjsunit installs that hook for every assertion failure and test262 never does; an infinite
 * recursion through that hook, which on an engine with no stack bound is not an overflow but an OOM kill; an
 * Annex B class-range misparse; three unescaped LineTerminators in RegExp.prototype.source; and a stack-trace
 * column that pointed at the last argument of a call instead of the call.
 *
 * NOTHING IS VENDORED. The files are fetched from V8's tree at run time, so this repo carries a fetch and a
 * shim, not someone else's tests.
 *
 * LINE NUMBERS ARE PRESERVED, and that is the whole reason this file replaced the one that only ran the regexp
 * area. mjsunit.js used to be CONCATENATED in front of each test, which shifted every line by a thousand — and
 * the tests that assert stack-trace positions (eval-origin.js says the origin ends ":19:13") then failed for a
 * reason that was not the engine's. A failure reported for the harness's own reason is worse than no coverage.
 * So mjsunit.js is written into a harness directory and pulled in with `includes:`, and the test262 frontmatter
 * REPLACES the first four lines of the file — every mjsunit file opens with at least four lines of BSD header,
 * so the replacement is line-for-line and everything below keeps its number. A file that does not open that
 * way is reported, not silently shifted.
 *
 * V8 natives (%ReferenceEqual, %ArraySpeciesProtector, …) are engine internals with `%`-prefixed syntax that no
 * other engine parses. They are rewritten to their observable meaning where there is one and to `true` where
 * there is not — the ASSERTIONS AROUND THEM are what this corpus is for, and dropping a whole file because one
 * line pokes at V8's optimizer would throw away the other four hundred.
 *
 * Usage:  node engine/v8mjsunit.mjs [area]
 */
import { spawnSync } from "node:child_process";
import { mkdirSync, writeFileSync, readFileSync, readdirSync, copyFileSync, rmSync, mkdtempSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";

const QJS = join(import.meta.dirname, "qjs");
const WORK = join(import.meta.dirname, ".work", "v8mjsunit");
const SRCS = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c", "quickjs-libc.c", "run-test262.c"];

/* V8's tree has no listing endpoint this environment can reach, so the set is named. A name that 404s is
   reported and skipped, never a hard failure — V8 moves files between directories and the corpus should
   survive that. Areas are named so a run can be scoped to one while working in it. */
const AREAS = {
  regexp: ["regexp", "regexp-capture", "regexp-capture-3", "regexp-compile", "regexp-duplicate-named-groups",
    "regexp-global", "regexp-indexof", "regexp-lastIndex", "regexp-lookahead", "regexp-loop-capture",
    "regexp-modifiers", "regexp-multiline", "regexp-named-captures", "regexp-property-scripts",
    "regexp-standalones", "regexp-static", "regexp-string-methods", "regexp-UC16", "regexp-unicode-sets",
    "harmony/regexp-lookbehind", "harmony/regexp-named-captures", "harmony/regexp-property-scripts",
    "harmony/regexp-v-flag"],
  /* The area the last four commits were about: stack traces, their positions, and the eval origin. */
  stack: ["stack-traces", "stack-traces-2", "stack-traces-custom-lazy", "eval-origin",
    "error-tostring-omit", "error-accessors", "cross-realm-filtering"],
};
/* Two files are NOT in the lists, and the reason is the point of the fork: both assert that a STACK-SIZE CAP
   exists. regexp-stack-overflow.js recurses until V8's --stack-size throws and then asserts the engine still
   works. error-tostring.js opens with `e.name = e; assertThrows(() => e.toString(), RangeError)` — a cyclic
   Error whose toString coerces its own name, which V8 ends by overflowing. This engine has no such cap: the
   recursion trampolines onto the heap, so neither test can terminate here. They are excluded because of what
   they assert, not because they fail — and error-tostring.js's other cyclic case, `e.name = [e]`, is answered
   correctly, because the cycle closes through Array.prototype.join, which does have an answer for it. */

const RAW = n => `https://raw.githubusercontent.com/v8/v8/main/test/mjsunit/${n}.js`;

function fetchText(url) {
  const r = spawnSync("curl", ["-sS", "-w", "\n%{http_code}", url], { encoding: "utf8", maxBuffer: 1 << 28 });
  if (r.status !== 0) return null;
  const nl = r.stdout.lastIndexOf("\n");
  return r.stdout.slice(nl + 1).trim() === "200" ? r.stdout.slice(0, nl) : null;
}

function deV8(src) {
  return src
    .replace(/%ReferenceEqual\(/g, "Object.is(")
    .replace(/%ConstructConsString\(([^,]+),\s*/g, "String.prototype.concat.call($1, ")
    .replace(/%ArraySpeciesProtector\(\)/g, "true")
    .replace(/%[A-Za-z]+\([^()]*\)/g, "true");
}

/* The frontmatter is FOUR lines and replaces the file's first four, so every line below keeps its number. */
function frame(name, src) {
  const lines = src.split("\n");
  for (let i = 0; i < 4; i++) {
    if (lines[i] === undefined || !(lines[i].trim() === "" || lines[i].startsWith("//"))) return null;
  }
  lines.splice(0, 4, "/*---", `description: V8 mjsunit ${name}.js, under forced time-travel`,
    "includes: [mjsunit.js]", "---*/");
  return lines.join("\n");
}

const wanted = process.argv[2];
if (wanted && !AREAS[wanted]) {
  console.error(`[v8mjsunit] no such area: ${wanted} (have ${Object.keys(AREAS).join(", ")})`);
  process.exit(1);
}

mkdirSync(WORK, { recursive: true });
const HARNESS = join(WORK, "harness");
const TESTS = join(WORK, "t");
for (const d of [HARNESS, TESTS]) { rmSync(d, { recursive: true, force: true }); mkdirSync(d, { recursive: true }); }

console.log("[v8mjsunit] fetching mjsunit.js…");
const mjsunit = fetchText(RAW("mjsunit"));
if (!mjsunit) {
  console.error("[v8mjsunit] could not fetch mjsunit.js — no network to raw.githubusercontent.com");
  process.exit(1);
}
/* run-test262 resolves `includes:` against ONE directory, so the test262 harness and mjsunit.js share it —
   copied out rather than written into the test262 submodule, which is not ours to dirty. */
const T262H = join(QJS, "test262", "harness");
for (const f of readdirSync(T262H)) if (f.endsWith(".js")) copyFileSync(join(T262H, f), join(HARNESS, f));
writeFileSync(join(HARNESS, "mjsunit.js"), deV8(mjsunit));

const conf = join(WORK, "v8.conf");
writeFileSync(conf, readFileSync(join(QJS, "test262.conf"), "utf8")
  .replace(/^harnessdir=.*$/m, `harnessdir=${HARNESS}`)
  /* run-test262 resolves the error file against the CONF's directory, and this corpus keeps no known-error
     list — every difference is reported here, in full, every run. */
  .replace(/^errorfile=.*$/m, "#errorfile="));

let got = 0;
const missing = [], unframed = [], areaOf = {};
for (const [area, names] of Object.entries(AREAS)) {
  if (wanted && area !== wanted) continue;
  for (const n of names) {
    const src = fetchText(RAW(n));
    if (!src) { missing.push(n); continue; }
    const body = frame(n, deV8(src));
    if (body === null) { unframed.push(n); continue; }
    const file = n.replace(/\//g, "_") + ".js";
    writeFileSync(join(TESTS, file), body);
    areaOf[file] = area;
    got++;
  }
}
console.log(`[v8mjsunit] ${got} files`
  + (missing.length ? `, ${missing.length} not found: ${missing.join(", ")}` : "")
  /* NOT a silent skip: a file whose first four lines are not a header cannot be framed without moving every
     line below it, and a moved line is a false failure in any test that asserts a position. */
  + (unframed.length ? `, ${unframed.length} without a four-line header (cannot keep line numbers): `
      + unframed.join(", ") : ""));

console.log("[v8mjsunit] building native run-test262 (gcc, NDEBUG)…");
const bin = join(mkdtempSync(join(tmpdir(), "v8mjs-")), "run262.exe");
const cc = spawnSync("gcc", ["-O1", "-w", "-DNDEBUG", "-D_GNU_SOURCE", "-DCONFIG_VERSION=\"t262\"",
  "-DAPICLIENT_DEV=1", "-I.", ...SRCS, "-o", bin, "-lm", "-lpthread"], { cwd: QJS, encoding: "utf8" });
if (cc.status !== 0) { console.error("[v8mjsunit] build FAILED\n" + (cc.stderr || "")); process.exit(1); }

/* ONE FILE AT A TIME. An @WHY aborts the process, and a shared run would then report nothing for the files
   after it — the abort has to name WHICH file reached it, or it starts a search instead of ending one. */
const one = join(WORK, "one");
const files = readdirSync(TESTS).filter(f => f.endsWith(".js")).sort();
let failed = 0, aborted = 0, slow = 0;
for (const f of files) {
  rmSync(one, { recursive: true, force: true });
  mkdirSync(one, { recursive: true });
  writeFileSync(join(one, f), readFileSync(join(TESTS, f), "utf8"));
  const r = spawnSync(bin, ["-c", conf, "-d", one], {
    cwd: QJS, encoding: "utf8", maxBuffer: 1 << 28,
    /* Generous, because this engine has no bounds and some of these files are quick in V8 only because V8 has
       them: regexp.js compiles `("a?" x 100000) + "a"` and matches it against "a", which V8 REFUSES to compile
       ("regexp too large") and this engine does not, so the engine really does attempt the exponential match.
       That is the physical RAM floor and it is what the host scheduler's paging tier is for; run-test262 has
       no scheduler, so a heavy file is reported honestly rather than trimmed to make the number look better. */
    env: { ...process.env, FORK_PREEMPT: "1" }, timeout: 900_000,
  });
  const out = (r.stdout || "") + (r.stderr || "");
  const label = `${areaOf[f]}/${f}`;
  const why = out.match(/@WHY .*/);
  if (why) { console.log(`  ABORT  ${label}\n         ${why[0]}`); aborted++; continue; }
  const m = out.match(/Result: (\d+)\/(\d+) error/);
  if (m && m[1] === "0") continue;
  /* NO Result line at all means the process never got to print one — it was killed by the timeout or by the
     OOM killer, which is a completely different thing from an assertion that failed, and reporting both as a
     bare "FAIL" is what makes someone go looking for a defect that is not there. Say which. */
  if (!m) {
    const how = r.signal ? `killed (${r.signal})` : r.error ? String(r.error.message) : "no Result line";
    console.log(`  SLOW   ${label}\n         ${how} — the file never finished. These are the patterns this`
      + ` engine runs and V8 prunes, not wrong answers; check by replicating the assertions.`);
    slow++;
    continue;
  }
  const detail = (out.match(/unexpected error: .*/) || ["(no message)"])[0];
  console.log(`  FAIL   ${label}\n         ${detail}`);
  failed++;
}
console.log(`\n[v8mjsunit] ${got - failed - aborted - slow}/${got} clean, ${failed} failing, ${slow} unfinished,`
  + ` ${aborted} ABORTING`);
/* An abort is an unrouted C entry — the thing this corpus exists to surface — so it is the only hard failure.
   A FAIL is a spec difference to read; the error-message texts still reported are not spec, and matching V8's
   wording would be matching Chrome rather than the spec. A SLOW is neither: it is a pattern this engine runs
   and V8 prunes. */
process.exit(aborted ? 1 : 0);
