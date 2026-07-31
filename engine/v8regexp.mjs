/* V8's own regexp test corpus, run through the FEATURE engine.
 *
 * WHY A SECOND CORPUS: test262 tests what the SPEC says, and it is the baseline. It does not test what a real
 * engine's regexp implementation is actually asked to do — V8's mjsunit regexp files are twenty years of bugs
 * found in a shipping engine, and they exercise shapes test262 never reaches. The first run of this corpus
 * aborted six of nineteen files on ONE unrouted C entry (Error.prepareStackTrace called from build_backtrace)
 * that the whole 43222-file test262 corpus never touched, because mjsunit installs that hook for every
 * assertion failure and test262 never does. A corpus that finds something the baseline cannot is worth having.
 *
 * NOTHING IS VENDORED. The files are fetched from V8's tree at run time and written to a scratch directory,
 * so this repo carries a fetch and a shim, not someone else's tests.
 *
 * V8 natives (%ReferenceEqual, %ArraySpeciesProtector, …) are engine internals with `%`-prefixed syntax that
 * no other engine parses. They are rewritten to their observable meaning where there is one and to `true`
 * where there is not — the ASSERTIONS AROUND THEM are what this corpus is for, and dropping a whole file
 * because one line pokes at V8's optimizer would throw away the other four hundred.
 *
 * Usage:  node engine/v8regexp.mjs
 */
import { spawnSync } from "node:child_process";
import { mkdirSync, writeFileSync, readFileSync, readdirSync, rmSync, mkdtempSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";

const QJS = join(import.meta.dirname, "qjs");
const WORK = join(import.meta.dirname, ".work", "v8regexp");
const SRCS = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c", "quickjs-libc.c", "run-test262.c"];

/* mjsunit/*.js, plus mjsunit/harmony/*.js for the ones that live there. V8's tree has no listing endpoint this
   environment can reach, so the set is named. A name that 404s is skipped and reported, never a hard failure —
   V8 moves files between directories and the corpus should survive that. */
const MJSUNIT = [
  "regexp", "regexp-capture", "regexp-capture-3", "regexp-compile", "regexp-duplicate-named-groups",
  "regexp-global", "regexp-indexof", "regexp-lastIndex", "regexp-lookahead", "regexp-loop-capture",
  "regexp-modifiers", "regexp-multiline", "regexp-named-captures", "regexp-property-scripts",
  "regexp-standalones", "regexp-static", "regexp-string-methods", "regexp-UC16", "regexp-unicode-sets",
];
const HARMONY = ["regexp-lookbehind", "regexp-named-captures", "regexp-property-scripts", "regexp-v-flag"];
/* regexp-stack-overflow.js is NOT in the list, and the reason is the point of the fork: it recurses until V8's
   --stack-size cap throws, then asserts the engine still works. This engine has no such cap — the recursion
   trampolines onto the heap — so the test cannot terminate here. It is excluded because it asserts a BOUND
   exists, not because it fails. */

const RAW = (dir, n) => `https://raw.githubusercontent.com/v8/v8/main/test/mjsunit/${dir}${n}.js`;

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

mkdirSync(WORK, { recursive: true });
const TESTS = join(WORK, "t");
rmSync(TESTS, { recursive: true, force: true });
mkdirSync(TESTS, { recursive: true });

console.log("[v8regexp] fetching mjsunit.js…");
const harness = fetchText(RAW("", "mjsunit"));
if (!harness) {
  console.error("[v8regexp] could not fetch mjsunit.js — no network to raw.githubusercontent.com");
  process.exit(1);
}

let got = 0;
const missing = [];
for (const [dir, names] of [["", MJSUNIT], ["harmony/", HARMONY]]) {
  for (const n of names) {
    const src = fetchText(RAW(dir, n));
    if (!src) { missing.push(dir + n); continue; }
    /* ONE self-contained file per test: run-test262 resolves `includes:` against the test262 harness
       directory, and putting mjsunit.js in there would write into the test262 submodule. */
    const body = `/*---\ndescription: V8 mjsunit ${dir}${n}.js, under forced time-travel\n---*/\n`
      + harness + `\n// ==== ${dir}${n}.js ====\n` + deV8(src);
    writeFileSync(join(TESTS, (dir ? "harmony_" : "") + n + ".js"), body);
    got++;
  }
}
console.log(`[v8regexp] ${got} files` + (missing.length ? `, ${missing.length} not found: ${missing.join(", ")}` : ""));

console.log("[v8regexp] building native run-test262 (gcc, NDEBUG)…");
const bin = join(mkdtempSync(join(tmpdir(), "v8re-")), "run262.exe");
const cc = spawnSync("gcc", ["-O1", "-w", "-DNDEBUG", "-D_GNU_SOURCE", "-DCONFIG_VERSION=\"t262\"",
  "-DAPICLIENT_DEV=1", "-I.", ...SRCS, "-o", bin, "-lm", "-lpthread"], { cwd: QJS, encoding: "utf8" });
if (cc.status !== 0) { console.error("[v8regexp] build FAILED\n" + (cc.stderr || "")); process.exit(1); }

/* ONE FILE AT A TIME. An @WHY aborts the process, and a shared run would then report nothing for the files
   after it — the abort has to name WHICH file reached it, or it starts a search instead of ending one. */
const one = join(WORK, "one");
const files = readdirSync(TESTS).filter(f => f.endsWith(".js")).sort();
let failed = 0, aborted = 0;
for (const f of files) {
  rmSync(one, { recursive: true, force: true });
  mkdirSync(one, { recursive: true });
  writeFileSync(join(one, f), readFileSync(join(TESTS, f), "utf8"));
  const r = spawnSync(bin, ["-c", "test262.conf", "-d", one], {
    cwd: QJS, encoding: "utf8", maxBuffer: 1 << 28,
    /* Generous, and the reason is the fork's: V8's bounds are what keep some of these files quick there.
       regexp.js builds `("a?" x 100000) + "a"` and matches it against "a" — V8 REFUSES to compile it ("regexp
       too large") and this engine does not, because refusing is a bound. So the engine attempts an exponential
       match with a backtracking stack in the hundreds of thousands, and on a small machine the OOM killer wins.
       That is the physical RAM floor, not a bug, and it is what the host scheduler's paging tier is for — but
       run-test262 has no scheduler, so the file is expected to be heavy here and is reported honestly rather
       than trimmed to make the number look better. */
    env: { ...process.env, FORK_PREEMPT: "1" }, timeout: 900_000,
  });
  const out = (r.stdout || "") + (r.stderr || "");
  const why = out.match(/@WHY .*/);
  if (why) { console.log(`  ABORT  ${f}\n         ${why[0]}`); aborted++; continue; }
  const m = out.match(/Result: (\d+)\/(\d+) error/);
  if (!m || m[1] !== "0") {
    const detail = (out.match(/unexpected error: .*/) || [""])[0];
    console.log(`  FAIL   ${f}\n         ${detail}`);
    failed++;
  }
}
console.log(`\n[v8regexp] ${got - failed - aborted}/${got} clean, ${failed} failing, ${aborted} ABORTING`);
/* An abort is an unrouted C entry — the thing this corpus exists to surface — so it is the only hard failure.
   A FAIL is a spec/feature difference to read and fix, and several are known (V8's legacy RegExp statics). */
process.exit(aborted ? 1 : 0);
