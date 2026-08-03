/* APIClient v2 build — minimal emscripten WASM build of CLEAN quickjs-ng + the
 * host scheduler entry (engine/host/main.c). Deliberately small: the old build
 * (COW barrier post-processing, wasm64, JSPI, Lexbor/Z3 link) is gone with the
 * fresh fork. Re-add each capability ONLY when the scheduler needs it, verified.
 *
 *   node engine/build.mjs           -> engine/host/out/qjs.mjs + qjs.wasm (node smoke test)
 *
 * Build success/failure is the milestone-0 signal (does clean quickjs-ng compile
 * + link + boot). Design-correctness verification stays on the live Chrome
 * harness once the browser target is wired.
 */
import { spawnSync } from "node:child_process";
import { mkdirSync, existsSync, copyFileSync, readdirSync, writeFileSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const HOST = join(ENGINE, "host");
const OUT = join(HOST, "out");
const EXT_QJS = join(ENGINE, "..", "extension", "lib", "qjs");   // where bridge.js imports the engine from

/* The recognizer ratchet runs BEFORE anything is compiled (CLAUDE.md §C-stack). The ban was written down and then
   violated four times in one session with the rule already in the file — so it is BUILT, not written. A detector
   added back under any name fails the build here rather than needing to be caught in review. */
{
  const r = spawnSync(process.execPath, [join(ENGINE, "check_recognizers.mjs")], { stdio: "inherit" });
  if (r.status !== 0) process.exit(r.status ?? 1);
}
/* The step-machine ownership declarations are paired with their state structs by editing pattern, in batches.
   That pairing is a type error C cannot see — a visit attached to the wrong struct compiles and passes the
   fixture — so it is asserted before anything is compiled, for the reason the ratchet above is. */
{
  const r = spawnSync(process.execPath, [join(ENGINE, "check_step_visits.mjs")], { stdio: "inherit" });
  if (r.status !== 0) process.exit(r.status ?? 1);
}
const WORK = join(ENGINE, ".work");
const EMSDK = join(WORK, "emsdk");
const EMCC = join(EMSDK, "upstream", "emscripten", process.platform === "win32" ? "emcc.bat" : "emcc");

if (!existsSync(EMCC)) { console.error("[build] emcc not found at " + EMCC); process.exit(1); }
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
const LEXBOR_SRC = join(WORK, "lexbor-src", "source");
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
const ABI = process.argv.includes("abi");          // build the production qjs_* entry instead of the smoke main()
const SOLVER = (f) => join(HOST, "solver", f);     // the Time-Travel Solver (the novel half)
const sources = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c"]
  .map((f) => join(QJS, f))
  .concat([
    SOLVER("cow.c"), SOLVER("engine.c"), SOLVER("flow.c"), SOLVER("decide.c"),   // per-flow COW + interleaving scheduler + weight + fork
    SOLVER("concolic.c"), SOLVER("endpoint.c"), SOLVER("solve.c"),               // concolic value + @H surface + @S solver
    SOLVER("absent.c"),                                                          // absent global: unknown app state vs a component this engine owes
    SOLVER("result.c"),                                                          // the ONE result document the host reads
    SOLVER("dom_cow.c"), SOLVER("attr_shadow.c"),                                // DOM time-travel delta + DOM-attribute taint shadow
    join(HOST, "browser", "core", "loader", "document_scripts.c"),               // Lexbor <script> inventory + bundle identity
    join(HOST, "browser", "core", "fetch", "fetch.c"),
    join(HOST, "browser", "core", "fetch", "response.c"),                       // Response: the reply a fetch promises
    join(HOST, "browser", "core", "loader", "module_loader.c"),                 // dynamic import: the lazy-chunk register                           // the Fetch API: every reached request funnels into the @H surface
    join(HOST, "browser", "core", "frame", "location.c"),                       // Location: concrete principal + concolic search/hash
    join(HOST, "browser", "core", "frame", "window.c"),                         // Window: which browsing context this is, and window.name as attacker input
    join(HOST, "browser", "core", "idl_args.c"),                                // the ONE coerce-then-call machine every DOMString member shares
    join(HOST, "browser", "core", "dom", "abort.c"),                            // AbortController/AbortSignal: the controller's flag real, a timeout's unknown
    join(HOST, "browser", "core", "frame", "navigator.c"),
    join(HOST, "browser", "core", "frame", "screen.c"),                         // Screen: every member the environment, so every member forks                      // Navigator: spec-fixed identity concrete, the gated environment concolic
    join(HOST, "browser", "core", "timing", "timer.c"),                         // Timers: the timer task source, each expiry a flow
    join(HOST, "browser", "core", "dom", "document.c"),                         // Document: parsed facts concrete, cookie/referrer input
    join(HOST, "browser", "core", "dom", "node.c"),                             // Node: identity, the tree, and the CharacterData nodes
    join(HOST, "browser", "core", "dom", "element.c"),
    join(HOST, "browser", "core", "events", "event_target.c"),                  // EventTarget: listeners + the load event                          // Element: node identity, taint-carrying attributes, innerHTML sink
    // THE ENTRY. `abi` builds the production qjs_* surface the extension bridge drives; the default builds
    // test_forced.c's main() as the node smoke test. They are alternatives, never both: test_forced.c owns
    // main() and runs on load, which a bridge-loaded module must not do.
    join(HOST, ABI ? "main.c" : "test_forced.c"),
  ]);

// The exports the bridge ccalls. Emscripten drops anything not named here, so a function missing from this
// list is a runtime "no such symbol" in the extension rather than a link error — the list IS the ABI.
const QJS_ABI = ["qjs_init", "qjs_bundle_id", "qjs_begin", "qjs_step", "qjs_result", "qjs_teardown",
                 "qjs_pending", "qjs_chunks", "qjs_provide", "qjs_top_weight", "qjs_set_yield_floor",
                 "qjs_request_park", "qjs_emit_partial"];

const args = [
  ...sources,
  LEXBOR_LIB,                 // link the cached Lexbor DOM archive
  "-I", QJS,
  "-I", HOST, "-I", join(HOST, "browser"),   // include by FULL path from the host root: a browser component is "core/dom/dom_element.h", a solver component "solver/concolic.h" — the layer is always explicit (no bare-name -I solver shortcut, so a cross-layer include names its layer)
  "-I", LEXBOR_INC,           // <lexbor/html/html.h> etc for main.c's DOM host-edges
  "-O1", "-w",
  "-D_GNU_SOURCE", "-DENABLE_DUMPS",
  // Offensive-programming build mode (check.h): DEV (default) keeps every DCHECK live so a should-never-happen
  // aborts LOUD at its origin; a `release` arg compiles them out (the release exemption — the user is not
  // crashed on an unsupportable state). CHECK (OOM/security) stays fatal in both.
  "-DAPICLIENT_DEV=" + (process.argv.includes("release") ? "0" : "1"),
  // Opt-in `assert` build: emscripten ASSERTIONS=2 turns a bare terse `Aborted()` into an INFORMATIVE crash
  // (the failing C assert + file:line — e.g. a refcount/gc_obj_list leak), the offensive-programming ideal of a
  // LOUD *and* diagnosable dev failure. Off by default so normal dev builds stay fast; enable when debugging.
  ...(process.argv.includes("assert") ? ["-sASSERTIONS=2"] : []),
  // AddressSanitizer build (`asan` arg) — the FIRST-CLASS memory-debugging tool a browser engineer expects:
  // intercepts malloc/free (quickjs's js_free routes to system free), so a double-free / use-after-free is
  // reported DETERMINISTICALLY with the alloc stack + BOTH free stacks (function names via --profiling-funcs).
  // ASan's shadow memory is incompatible with memory GROWTH, so it pins a fixed 1 GiB heap instead.
  ...(process.argv.includes("asan")
      ? ["-fsanitize=address", "--profiling-funcs", "-sINITIAL_MEMORY=1073741824",
         ...(process.argv.includes("dwarf") ? ["-g"] : [])]
      : ["-sALLOW_MEMORY_GROWTH=1"]),
  "-sSTACK_SIZE=8388608",
  // The smoke entry RUNS on load and exits with the @H/@S pass code; the ABI entry is driven by the bridge
  // through ccall, so its runtime must stay alive across qjs_step re-entries and be importable as an ES module.
  ...(ABI
      ? ["-sEXPORTED_FUNCTIONS=" + JSON.stringify(QJS_ABI.map((f) => "_" + f).concat(["_malloc", "_free"])),
         "-sEXPORTED_RUNTIME_METHODS=" + JSON.stringify(["ccall", "lengthBytesUTF8", "stringToUTF8"]),
         "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createQJS", "-sINVOKE_RUN=0"]
      : ["-sEXIT_RUNTIME=1"]),
  /* THE ABI ARTIFACT STAGES WHERE THE EXTENSION LOADS IT: bridge.js does import("./lib/qjs/qjs.mjs"), so that
     is the output path, not engine/host/out. It also keeps the two targets from colliding — emcc derives the
     .wasm name from the -o basename, so both emitting into out/qjs.* shared one qjs.wasm and overwrote each
     other, leaving a loader pair that was valid only until the next smoke build. Two artifacts, two homes. */
  "-o", ABI ? join(EXT_QJS, "qjs.mjs") : join(OUT, "qjs.js"),
];

console.log("[build] emcc " + sources.length + " sources -> engine/host/out/qjs." + (ABI ? "mjs (production ABI)" : "js (new-world smoke test)"));
const r = spawnSync(EMCC, args, { stdio: "inherit", shell: true, cwd: QJS });
if (r.status !== 0) { console.error("[build] FAILED rc=" + r.status); process.exit(r.status || 1); }
console.log("[build] OK -> " + resolve(join(OUT, "qjs.js")));

// Milestone smoke test: run test_forced.c's main (the @H merge + @S sink fire-verification on a fixture doc) —
// the design-correctness signal until the live-Chrome harness is re-wired to a rebuilt production ABI entry.
// (The old ES6-module + qjs.wasm staging into extension/lib/qjs served the deleted qjs_* entry; it returns when
// that entry is rebuilt.)
if (ABI) {
  console.log("[build] OK -> " + resolve(join(EXT_QJS, "qjs.mjs")) + " (no smoke run: the ABI entry has no main())");
} else {
  const t = spawnSync(process.execPath, [join(OUT, "qjs.js")], { stdio: "inherit", shell: false });
  if (t.status !== 0) { console.error("[build] smoke test FAILED rc=" + (t.status ?? "signal")); process.exit(t.status || 1); }
  console.log("[build] smoke test PASS (new-world @H + @S)");
}
