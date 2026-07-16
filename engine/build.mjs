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
const WORK = join(ENGINE, ".work");
const EMSDK = join(WORK, "emsdk");
const EMCC = join(EMSDK, "upstream", "emscripten", process.platform === "win32" ? "emcc.bat" : "emcc");

if (!existsSync(EMCC)) { console.error("[build] emcc not found at " + EMCC); process.exit(1); }
mkdirSync(OUT, { recursive: true });

/* ── Lexbor DOM (HTML5 parser + DOM + CSS selectors) ─────────────────────────────
   The moat runs the page's real bundle against a real DOM. Lexbor is pure C, compiles
   to wasm, and links in the same module as quickjs. It's slow to compile (213 files),
   so build it ONCE into a cached static archive (liblexbor.a) and link that; rebuilds
   of the engine (quickjs + main.c) then stay fast. Rebuild the archive with
   `node engine/build.mjs lexbor`. */
const LEXBOR_SRC = join(ENGINE, "lexbor", "source");
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
const SOLVER = (f) => join(HOST, "solver", f);     // the Time-Travel Solver (the novel half)
const sources = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c"]
  .map((f) => join(QJS, f))
  .concat([
    SOLVER("cow.c"), SOLVER("engine.c"), SOLVER("flow.c"), SOLVER("decide.c"),   // per-flow COW + interleaving scheduler + weight + fork
    SOLVER("concolic.c"), SOLVER("endpoint.c"), SOLVER("solve.c"),               // concolic value + @H surface + @S solver
    SOLVER("dom_cow.c"), SOLVER("attr_shadow.c"),                                // DOM time-travel delta + DOM-attribute taint shadow
    join(HOST, "browser", "core", "loader", "document_scripts.c"),               // the one live browser piece: Lexbor <script> inventory
    join(HOST, "test_forced.c"),                                                 // the node smoke-test entry (@H merge + @S sink fire-verify)
  ]);

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
  "-sEXIT_RUNTIME=1",         // test_forced.c's main() runs on load and exits with the @H/@S pass code
  "-o", join(OUT, "qjs.js"),
];

console.log("[build] emcc " + sources.length + " sources -> engine/host/out/qjs.js (new-world smoke test)");
const r = spawnSync(EMCC, args, { stdio: "inherit", shell: true, cwd: QJS });
if (r.status !== 0) { console.error("[build] FAILED rc=" + r.status); process.exit(r.status || 1); }
console.log("[build] OK -> " + resolve(join(OUT, "qjs.js")));

// Milestone smoke test: run test_forced.c's main (the @H merge + @S sink fire-verification on a fixture doc) —
// the design-correctness signal until the live-Chrome harness is re-wired to a rebuilt production ABI entry.
// (The old ES6-module + qjs.wasm staging into extension/lib/qjs served the deleted qjs_* entry; it returns when
// that entry is rebuilt.)
const t = spawnSync(process.execPath, [join(OUT, "qjs.js")], { stdio: "inherit", shell: false });
if (t.status !== 0) { console.error("[build] smoke test FAILED rc=" + (t.status ?? "signal")); process.exit(t.status || 1); }
console.log("[build] smoke test PASS (new-world @H + @S)");
