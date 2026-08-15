/* APIClient v2 build — minimal emscripten WASM build of CLEAN quickjs-ng + the
 * host scheduler entry (engine/host/main.c). Deliberately small: the old build
 * (COW barrier post-processing, wasm64, JSPI, Lexbor/Z3 link) is gone with the
 * fresh fork. Re-add each capability ONLY when the scheduler needs it, verified.
 *
 *   node engine/build.mjs                  -> engine/host/out/qjs.mjs + qjs.wasm (node smoke test)
 *   node engine/build.mjs native [min]     -> the smoke fixture built and run NATIVELY (the memory series)
 *   node engine/build.mjs native leak      -> the same under LeakSanitizer
 *   node engine/build.mjs native address   -> the same under AddressSanitizer
 *
 * Build success/failure is the milestone-0 signal (does clean quickjs-ng compile
 * + link + boot). Design-correctness verification stays on the live Chrome
 * harness once the browser target is wired.
 */
import { spawnSync, spawn } from "node:child_process";
import { mkdirSync, existsSync, copyFileSync, readdirSync, writeFileSync, statSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { cpus } from "node:os";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const HOST = join(ENGINE, "host");
const OUT = join(HOST, "out");
const EXT_QJS = join(ENGINE, "..", "extension", "lib", "qjs");   // where bridge.js imports the engine from

/* `--list-sources` answers WHAT THE PROGRAM IS and exits — check_recursion.sh shells back into this file to ask,
   and a list with build output in front of it is not a list. */
const LIST_SOURCES = process.argv.includes("--list-sources");

const WORK = join(ENGINE, ".work");
const EMSDK = join(WORK, "emsdk");
const EMCC = join(EMSDK, "upstream", "emscripten", process.platform === "win32" ? "emcc.bat" : "emcc");

if (!existsSync(EMCC)) {
  /* A fresh container has no toolchain, and "emcc not found" on its own sends whoever hits it hunting for the
     version that was used. Say exactly what to run — the same reason the lexbor tag below is pinned here. */
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
/* The vendored checkout lives beside its build product in .work, which is where it actually is and where
   liblexbor.o was compiled from. It used to be named at engine/lexbor/source — a second, git-ignored location
   that is empty in a fresh container, so the build died on a missing <lexbor/html/html.h> while the headers sat
   in .work. One location, no fallback. */
/* PINNED, and provisioned here. Lexbor's C API moves between releases — a container that cloned its default
   branch instead got a css_declaration_list_parse with a different arity and the build died in the CSSOM. The
   version is part of the build, so it is stated in the build rather than in whoever's shell history. */
const LEXBOR_TAG = "v2.7.0";
const LEXBOR_DIR = join(WORK, "lexbor-src");
if (!existsSync(join(LEXBOR_DIR, "source", "lexbor"))) {
  console.log("[build] lexbor " + LEXBOR_TAG + " not present — cloning it");
  const r = spawnSync("git", ["clone", "--depth", "1", "--branch", LEXBOR_TAG,
                              "https://github.com/lexbor/lexbor.git", LEXBOR_DIR], { stdio: "inherit" });
  if (r.status !== 0) { console.error("[build] could not clone lexbor " + LEXBOR_TAG); process.exit(1); }
}
const LEXBOR_SRC = join(LEXBOR_DIR, "source");
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
function buildLexbor(force) {
  if (!force && existsSync(LEXBOR_LIB)) return;
  const srcs = findC(join(LEXBOR_SRC, "lexbor"), []);
  console.log("[build] lexbor: compiling " + srcs.length + " sources -> liblexbor.a (once, ~minutes)");
  const rsp = join(WORK, "lexbor.rsp");
  const fwd = (s) => s.replace(/\\/g, "/");   // response-file backslashes are clang escapes -> forward-slash paths
  writeFileSync(rsp, [...srcs.map(fwd), "-I", fwd(LEXBOR_INC), "-O2", "-w", "-D_GNU_SOURCE", "-DENABLE_DUMPS", "-r", "-o", fwd(LEXBOR_LIB)].join("\n"));
  const r = spawnSync(EMCC, ["@" + rsp], { stdio: "inherit", shell: true, cwd: ENGINE });
  if (r.status !== 0) { console.error("[build] lexbor FAILED rc=" + r.status); process.exit(r.status || 1); }
  console.log("[build] lexbor OK -> " + LEXBOR_LIB);
}
buildLexbor(process.argv[2] === "lexbor");
if (process.argv[2] === "lexbor") { console.log("[build] lexbor archive rebuilt; re-run without arg to build the engine."); process.exit(0); }

// (The old browser IDL codegen — idlgen.mjs -> idl_generated.h — drove the deleted Blink-mirroring browser
// components. With the browser half removed pending re-build against the rewritten fork, there is nothing to
// generate; re-add the idlgen step when those components are rebuilt.)

// THE NEW-WORLD SOLVER CORE. The OLD main.c scheduler + heap_cow + the entire browser half were deleted as
// legacy: they were built on the fork's PREVIOUS hook API (JS_SetCowCaptureHooks / JS_SetFlowYieldHook /
// JS_SetArrayIterHook / …), which the rewritten fork replaced with the time-travel hook system
// (JS_SetTimeTravelHooks + JS_FlowNew/Resume/Clone + JS_SetBranchHook/ForkHook/PreemptHook). These are the
// files that actually link against the current fork. There is no qjs_* extension ABI entry yet — test_forced.c
// is the node smoke-test main (build.mjs's milestone signal: does the new world compile + link + run + PASS its
// @H/@S fixture). Rebuilding the production ABI entry that wraps engine.c's scheduler for the offscreen
// document — and re-growing the browser components against the new fork — is the next keystone.
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
   qjs_* ABI the bridge drives through ccall and must not run on load. */
const ENTRY_SMOKE = join(HOST, "test_forced.c");
const ENTRY_ABI   = join(HOST, "main.c");
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
const sources = SHARED_SOURCES.concat([ENTRY_SMOKE, ENTRY_ABI]);

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
  /* The quiet list and -Werror=implicit-function-declaration are the SHIPPED build's, taken from the same place
     rather than restated, so the sanitized program is the program. */
  const quiet = ["-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses",
                 "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow",
                 "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer",
                 "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration"];
  const cc = spawnSync("clang", [
    "-O1", "-g", "-fno-omit-frame-pointer",
    ...(kind === "none" ? [] : ["-fsanitize=" + kind]),
    ...quiet,
    "-D_GNU_SOURCE", "-DENABLE_DUMPS", '-DCONFIG_VERSION="native"', "-DAPICLIENT_DEV=1",
    "-I" + QJS, "-I" + HOST, "-I" + join(HOST, "browser"), "-I" + join(WORK, "lexbor-src", "source"),
    /* THE SMOKE ENTRY, always: this target BUILDS AND RUNS a fixture, and main.c has no main() to run. */
    ...SHARED_SOURCES, ENTRY_SMOKE, LEXBOR_NATIVE, "-o", bin, "-lm", "-lpthread",
  ], { stdio: "inherit" });
  if (cc.status !== 0) { console.error("[build] native build FAILED rc=" + cc.status); process.exit(cc.status || 1); }
  console.log("[build] OK -> " + bin);
  /* AND IT IS RUN, because a target that is only built is the excluded test one layer down: the whole point is
     the stream it prints and the report it ends with, and nothing else in the tree produces either. */
  const t = spawnSync(bin, MIN ? ["--min"] : [], { stdio: "inherit" });
  if (t.status !== 0) {
    console.error("[build] the native run reported rc=" + (t.status ?? "signal") +
                  " — a LeakSanitizer summary above is a real leak, and an AddressSanitizer report a real fault");
    process.exit(t.status || 1);
  }
  console.log("[build] native run PASS (" + kind + (MIN ? ", minimal document" : "") + ")");
  process.exit(0);
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
const QJS_ABI = ["qjs_init", "qjs_bundle_id", "qjs_begin", "qjs_step", "qjs_result", "qjs_teardown",
                 "qjs_pending", "qjs_chunks", "qjs_provide", "qjs_top_weight", "qjs_set_yield_floor",
                 "qjs_request_park", "qjs_emit_partial",
                 "qjs_host_requests", "qjs_host_answer", "qjs_host_notices", "qjs_route",
                 "qjs_perform", "qjs_host_answer_remote"];

/* THE LIST IS THE ABI, SO THE ENTRY POINT AND THE LIST ARE ONE FACT AND ARE CHECKED AGAINST EACH OTHER. Both
   directions are a real defect and neither has a symptom at build time: an entry main.c defines and this omits
   is a capability the extension cannot call (or can, until a setting changes); a name here that main.c does not
   define is `--export=` of a symbol that does not exist, which wasm-ld reports as an undefined export only
   because ERROR_ON_UNDEFINED_SYMBOLS happens to be on. Read from the source rather than restated: `QJS_EXPORT`
   is exactly the marker main.c puts on every ABI body. */
{
  const _mainSrc = readFileSync(join(HOST, "main.c"), "utf8");
  const _defined = [...new Set([..._mainSrc.matchAll(/QJS_EXPORT\s+[\w \t*]+?\b(qjs_\w+)\s*\(/g)].map((m) => m[1]))];
  const _missing = _defined.filter((f) => !QJS_ABI.includes(f));
  const _phantom = QJS_ABI.filter((f) => !_defined.includes(f));
  if (_missing.length || _phantom.length) {
    console.error("[build] the ABI list and main.c's QJS_EXPORT entries disagree — the list IS the ABI, so a\n" +
                  "[build] disagreement is either an entry nothing can call or an export of nothing:\n" +
                  (_missing.length ? "[build]   defined in main.c, missing from QJS_ABI: " + _missing.join(", ") + "\n" : "") +
                  (_phantom.length ? "[build]   named in QJS_ABI, defined nowhere:       " + _phantom.join(", ") + "\n" : ""));
    process.exit(1);
  }
}

/* COMPILE FLAGS AND LINK FLAGS ARE SEPARATED, and that separation is what lets both entries be verified.
   They used to be one list handed to one emcc invocation that compiled and linked together, which forced two
   things: every build recompiled all 130-odd sources from scratch, and only ONE entry could be produced per
   run. The consequence was that the shipped ABI entry — the one the extension actually loads — was never
   LINKED by any default gate. It was compiled to a throwaway object, which catches a missing #include and a
   bad declaration and catches NOTHING about an undefined symbol; a qjs_* body calling a function deleted from
   the tree passed that check and would have failed only in the extension. §Testing's rule is that the shipped
   entry is the one that rots, and half a check is how it rots quietly.
   The two programs differ ONLY in which entry object enters the link, so compiling once into shared objects
   and linking twice costs one extra link and closes that hole completely. */
const CFLAGS = [
  "-I", QJS,
  "-I", HOST, "-I", join(HOST, "browser"),   // include by FULL path from the host root: a browser component is "core/dom/dom_element.h", a solver component "solver/concolic.h" — the layer is always explicit (no bare-name -I solver shortcut, so a cross-layer include names its layer)
  "-I", LEXBOR_INC,           // <lexbor/html/html.h> etc for main.c's DOM host-edges
  /* -Werror ON IMPLICIT DECLARATIONS, and the reason `-w` is NOT here beside it. A missing #include makes C
     assume `int (...)`, so a returned 64-bit POINTER comes back TRUNCATED — a segfault with no diagnostic, and
     it happened: window.c called window_proxy_name without its header and the whole corpus segfaulted inside
     strcmp. This line used to read `-w -Werror=implicit-function-declaration`, which does NOT work: `-w`
     suppresses the diagnostic outright, so the -Werror= promotion has nothing left to promote. The quiet list
     is explicit for that reason, and the moment it went in the gate found three more missing includes. */
  "-O1", "-Wno-unknown-warning-option", "-Wno-unused", "-Wno-sign-compare", "-Wno-parentheses", "-Wno-format-truncation", "-Wno-format-overflow", "-Wno-array-bounds", "-Wno-stringop-overflow", "-Wno-maybe-uninitialized", "-Wno-misleading-indentation", "-Wno-dangling-pointer", "-Wno-char-subscripts", "-Wno-implicit-fallthrough", "-Werror=implicit-function-declaration",
  "-D_GNU_SOURCE", "-DENABLE_DUMPS",
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
  "-sEXPORTED_RUNTIME_METHODS=" + JSON.stringify(["ccall", "lengthBytesUTF8", "stringToUTF8"]),
  "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createQJS", "-sINVOKE_RUN=0",
];

/* ── OBJECTS, COMPILED ONCE AND CACHED ────────────────────────────────────────────────────────────────────
   An object is rebuilt when it is missing, when its source is newer, or when any HEADER it included is newer —
   the last of which is the one a hand-rolled cache always gets wrong, so it is not hand-rolled: clang emits the
   real dependency list with -MMD and this reads it back. A cache that misses a header edit is worse than no
   cache, because it reports a stale binary as a fresh one. */
const OBJDIR = join(WORK, "obj");
mkdirSync(OBJDIR, { recursive: true });
const objPath = (src) => join(OBJDIR, resolve(src).replace(/[\\/:]/g, "_") + ".o");

function objIsStale(src, obj) {
  if (!existsSync(obj)) return true;
  const objTime = statSync(obj).mtimeMs;
  if (statSync(src).mtimeMs > objTime) return true;
  const dep = obj.replace(/\.o$/, ".d");
  if (!existsSync(dep)) return true;   /* no recorded deps is not "no deps" — it is no information */
  for (const f of readFileSync(dep, "utf8")
                    .replace(/\\\r?\n/g, " ").split(/\s+/).slice(1).filter(Boolean)) {
    if (!existsSync(f)) return true;   /* a header that vanished changes the compile */
    if (statSync(f).mtimeMs > objTime) return true;
  }
  return false;
}

/* Both entries are compiled every time, because both are LINKED every time. */
const TO_COMPILE = SHARED_SOURCES.concat([ENTRY_SMOKE, ENTRY_ABI]);
const stale = TO_COMPILE.filter((s) => objIsStale(s, objPath(s)));
console.log("[build] " + TO_COMPILE.length + " sources, " + stale.length + " to compile" +
            (stale.length < TO_COMPILE.length ? " (rest cached)" : ""));

if (stale.length) {
  /* IN PARALLEL, bounded by the cores actually present. A cold build is ~130 translation units and the machine
     is otherwise idle while each one runs. */
  const JOBS = Math.max(1, cpus().length - 1);
  let next = 0, failed = 0, running = 0;
  await new Promise((done) => {
    const pump = () => {
      while (running < JOBS && next < stale.length) {
        const src = stale[next++], obj = objPath(src);
        running++;
        const p = spawn(EMCC, [...CFLAGS, "-MMD", "-MF", obj.replace(/\.o$/, ".d"), "-c", src, "-o", obj],
                        { stdio: "inherit", shell: true, cwd: QJS });
        p.on("exit", (code) => {
          if (code !== 0) failed++;
          running--;
          if (next >= stale.length && running === 0) done();
          else pump();
        });
      }
      if (next >= stale.length && running === 0) done();
    };
    pump();
  });
  if (failed) { console.error("[build] FAILED — " + failed + " source(s) did not compile"); process.exit(1); }
}

const OBJS_SHARED = SHARED_SOURCES.map(objPath);

/* ── LINK BOTH PROGRAMS ───────────────────────────────────────────────────────────────────────────────────
   THE ABI ARTIFACT STAGES WHERE THE EXTENSION LOADS IT: bridge.js does import("./lib/qjs/qjs.mjs"), so that is
   the output path, not engine/host/out. Two artifacts, two homes — emcc derives the .wasm name from the -o
   basename, so both emitting into out/qjs.* would share one qjs.wasm and overwrite each other. */
function link(what, entryObj, ldflags, out) {
  const l = spawnSync(EMCC, [...OBJS_SHARED, entryObj, ...LDFLAGS_COMMON, ...ldflags, "-o", out],
                      { stdio: "inherit", shell: true, cwd: QJS });
  if (l.status !== 0) { console.error("[build] " + what + " LINK FAILED rc=" + l.status); process.exit(l.status || 1); }
  console.log("[build] OK -> " + out);
}
link("smoke", objPath(ENTRY_SMOKE), LDFLAGS_SMOKE, join(OUT, "qjs.js"));
link("production ABI", objPath(ENTRY_ABI), LDFLAGS_ABI, join(EXT_QJS, "qjs.mjs"));

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

/* The trailing arguments reach main()'s argv — the channel getenv could not be, since emscripten's ENV never
   merges the launching process's environment. */
{
  const t = spawnSync(process.execPath, [join(OUT, "qjs.js"), ...(MIN ? ["--min"] : [])],
                      { stdio: "inherit", shell: false });
  if (t.status !== 0) { console.error("[build] smoke test FAILED rc=" + (t.status ?? "signal")); process.exit(t.status || 1); }
  console.log("[build] smoke test PASS (new-world @H + @S" + (MIN ? ", minimal document" : "") + ")");
}

/* THE SHIPPED ENTRY, DRIVEN THROUGH THE SURFACE THE BRIDGE ACTUALLY CALLS. "The ABI entry has no main()" was
   true and was not a reason: the entry is DRIVEN, so its smoke test is a driver, and engine/route.mjs is one —
   it boots the module just linked, provisions a SECOND instance from the create notice, and routes a post
   between them. That is the shipped qjs_init/qjs_begin/qjs_step/qjs_pending/qjs_host_notices/qjs_route path,
   end to end. It is also the only thing in the tree that provisions two instances at all, which §SECURITY makes
   the precondition for believing any cross-instance mechanism has ever run — and it used to run only when
   somebody remembered to pass `abi`, which is to say almost never. */
{
  const t = spawnSync(process.execPath, [join(ENGINE, "route.mjs")], { stdio: "inherit", shell: false });
  if (t.status !== 0) {
    console.error("[build] the two-instance ABI drive FAILED rc=" + (t.status ?? "signal"));
    process.exit(t.status || 1);
  }
  console.log("[build] two-instance ABI drive PASS");
}
