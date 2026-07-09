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

// The host mirrors the PROJECT IDENTITY "a browser with a BFS Time-Travel Solver": the BROWSER half is
// organized like a real browser (browser/, each file a web-platform component mapping to a Blink module —
// location=core/frame/Location, dom_element=core/dom/Element, forms=core/html/forms, …), the novel SOLVER half
// (concolic value, time-travel COW, @S/@H) lives in solver/, and main.c (the scheduler entry) + check.h (the
// DCHECK/CHECK infra) stay at the host root. Flat includes resolve via the -I flags below.
const BROWSER = (f) => join(HOST, "browser", f);   // spec-faithful web-platform components (Blink-mirroring)
const SOLVER = (f) => join(HOST, "solver", f);     // the Time-Travel Solver (the novel half)
const sources = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c", "quickjs-libc.c"]
  .map((f) => join(QJS, f))
  .concat([join(HOST, "main.c"),
    SOLVER("solve_html.c"), SOLVER("dom_cow.c"), SOLVER("opaque.c"), SOLVER("reply.c"), SOLVER("endpoint.c"), SOLVER("attr_shadow.c"), SOLVER("constraints.c"),
    BROWSER("csp.c"), BROWSER("dom_select.c"), BROWSER("storage.c"), BROWSER("indexeddb.c"), BROWSER("messaging.c"), BROWSER("url.c"), BROWSER("xhr.c"), BROWSER("fetch.c"),
    BROWSER("forms.c"), BROWSER("classlist.c"), BROWSER("docwrite.c"), BROWSER("urlobj.c"), BROWSER("module_loader.c"),
    BROWSER("domparser.c"), BROWSER("location.c"), BROWSER("dom_element.c"), BROWSER("document.c"), BROWSER("custom_elements.c"), BROWSER("formdata.c"),
    BROWSER("websocket.c"), BROWSER("worker.c"), BROWSER("navigator.c"), BROWSER("cssom.c"), BROWSER("observer.c"),
    BROWSER("idl.c"), BROWSER("abort.c"), BROWSER("intl.c"), BROWSER("notification.c"), BROWSER("media_element.c"), BROWSER("history.c"), BROWSER("cookie.c"),
    SOLVER("wfq.c")]);

const args = [
  ...sources,
  LEXBOR_LIB,                 // link the cached Lexbor DOM archive
  "-I", QJS,
  "-I", HOST, "-I", join(HOST, "browser"), "-I", join(HOST, "solver"),   // flat #include "x.h" resolves across the browser/ + solver/ split
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
  "-sALLOW_MEMORY_GROWTH=1",
  "-sSTACK_SIZE=8388608",
  "-sEXIT_RUNTIME=0",
  "-sINVOKE_RUN=0",
  "-sMODULARIZE=1",
  "-sEXPORT_ES6=1",
  "-sEXPORTED_RUNTIME_METHODS=callMain,FS,ccall,cwrap,stringToUTF8,lengthBytesUTF8,UTF8ToString,HEAPU8",
  "-sEXPORTED_FUNCTIONS=_main,_qjs_init,_qjs_bundle_id,_qjs_begin,_qjs_step,_qjs_emit_partial,_qjs_set_yield_floor,_qjs_request_park,_qjs_top_weight,_qjs_pending,_qjs_chunks,_qjs_provide,_qjs_finalize,_qjs_teardown,_malloc,_free",
  "-sNODERAWFS=0",
  "-o", join(OUT, "qjs.mjs"),
];

console.log("[build] emcc " + sources.length + " sources -> engine/host/out/qjs.mjs");
const r = spawnSync(EMCC, args, { stdio: "inherit", shell: true, cwd: QJS });
if (r.status !== 0) { console.error("[build] FAILED rc=" + r.status); process.exit(r.status || 1); }
console.log("[build] OK -> " + resolve(join(OUT, "qjs.mjs")));

/* Stage the ES6 module + wasm into the extension so the offscreen document (ast-worker.js) can
   import them at chrome-extension://<id>/lib/qjs/qjs.mjs (which fetches qjs.wasm alongside). This
   replaces the old sync.mjs staging of qjs_worker.js/hostedge.gen.js (dead with the fresh fork). */
const STAGE = join(ENGINE, "..", "extension", "lib", "qjs");
mkdirSync(STAGE, { recursive: true });
for (const f of ["qjs.mjs", "qjs.wasm"]) copyFileSync(join(OUT, f), join(STAGE, f));
console.log("[build] staged -> " + resolve(join(STAGE, "qjs.mjs")) + " (+ qjs.wasm)");
