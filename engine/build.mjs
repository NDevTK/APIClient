#!/usr/bin/env node
// Canonical forked-QuickJS build. Three artifacts from ONE patched
// source tree (engine/qjs) — no test-vs-ship divergence:
//
//   qjs.exe       native gcc       — fast Node-side X-Force iteration
//   qjs_wasm.js   emcc NODERAWFS   — wasm CLI, byte-identical to native
//   qjs_mod.mjs   emcc MODULARIZE  — ES6 module, Node-side model proof
//                 EXPORT_ES6         (mdrive.mjs)
//   qjs_worker.js emcc MODULARIZE  — classic-worker factory (no ES6,
//                 EXPORT_NAME        importScripts-able), MEMFS — this
//                                    is what the extension's offscreen
//                                    Web Worker runs, a fresh instance
//                                    per forced schedule
//
// The forced-execution controller (quickjs-forced.h, patched into
// quickjs.c) takes its config from qjsmain's --fe-* argv so it behaves
// identically across native, wasm-CLI and modular-wasm (emscripten
// getenv / post-init ENV are unreliable in the MODULARIZE build).
//
//   node engine/build.mjs            # all three
//   node engine/build.mjs native|cli|mod
import { spawnSync } from "node:child_process";
import { existsSync, statSync, readFileSync, writeFileSync, mkdirSync, readdirSync } from "node:fs";
import { join, resolve, dirname, relative } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = resolve(dirname(fileURLToPath(import.meta.url)));
const QJS = join(ENGINE, "qjs");
const EMSDK = join(ENGINE, ".work", "emsdk");
const EMCC = join(EMSDK, "upstream", "emscripten", "emcc.py");
const EM_PY = join(EMSDK, "python", "3.13.3_64bit", "python.exe");
const EM_CONFIG = join(EMSDK, ".emscripten");

// Lexbor (spec HTML5 parser + DOM + CSS selectors) is compiled INTO
// the same wasm as QuickJS — the bundle's `document`/elements ARE
// Lexbor nodes (qjs_dom.c binds them). All modules except the posix
// fs port (lexbor file IO is unused, and its S_IFLNK breaks native
// MSYS gcc; harmless to omit everywhere). Paths are relative to QJS
// (build commands run with cwd=QJS).
const LEXBOR = resolve(ENGINE, "lexbor", "source", "lexbor");
function lexborSrcs() {
  const out = [];
  (function walk(dir) {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name);
      if (e.isDirectory()) { if (e.name !== "ports") walk(p); continue; }
      if (e.name.endsWith(".c")) out.push(p);
    }
  })(LEXBOR);
  out.push(join(LEXBOR, "ports", "posix", "lexbor", "core", "memory.c"));
  out.push(join(LEXBOR, "ports", "posix", "lexbor", "core", "perf.c"));
  // emcc/gcc are invoked with cwd=QJS — make paths relative to it.
  return out.map((p) => relative(QJS, p).split(/[\\/]/).join("/"));
}

// Single source list — every target compiles the SAME patched TU set
// plus the qjs<->Lexbor binding and Lexbor itself.
const SRC = ["qjsmain.c", "quickjs.c", "libregexp.c", "libunicode.c",
             "dtoa.c", "quickjs-libc.c", "qjs_dom.c",
             // C++ trampoline for Z3 — converts a Z3 throw into a
             // Z3_ERROR verdict + @E diagnostic instead of crashing
             // the runtime. Both gcc and emcc auto-detect .cpp.
             "qjs_z3_shim.cpp",
             ...lexborSrcs()];
// Z3 SMT solver, linked statically. Used ONLY for security:
// @S tainted-sink path-satisfiability solve (REAL EXPLOIT vs
// INFEASIBLE forced path vs TAINT REACH ONLY). NEVER for API value
// resolution — API values stay assumed/forked/structural per CLAUDE.md
// (we want hidden/unreachable endpoints too). z3.h is C-ABI; the
// solver code lives inside quickjs.c, no JS bridge.
const Z3 = resolve(ENGINE, ".work", "z3");
const Z3_INC = `-I${relative(QJS, join(Z3, "src", "api")).split(/[\\/]/).join("/")}`;
const Z3_LIB_N = relative(QJS, join(Z3, "bn", "libz3.a")).split(/[\\/]/).join("/");
const Z3_LIB_W = relative(QJS, join(Z3, "bw", "libz3.a")).split(/[\\/]/).join("/");
const CFLAGS = ["-O1", "-w", "-D_GNU_SOURCE", "-DLEXBOR_STATIC",
                "-DQJS_HAVE_Z3=1", Z3_INC,
                "-I.", "-I../lexbor/source",
                // Opt-in extra -D defines (e.g. native diagnostic probes) via
                // QJS_EXTRA_DEFINES="-DQJS_SPIN_PROBE"; never set in ship builds.
                ...(process.env.QJS_EXTRA_DEFINES ? process.env.QJS_EXTRA_DEFINES.split(/\s+/).filter(Boolean) : [])];
// QuickJS's own recursion (parser/GC) overflows emscripten's 64KB
// default stack on real minified bundles; grow it. Memory64 lifts
// the wasm32 4 GiB cap to the wasm64 spec ceiling (2^64 bytes); the
// platform/host enforces the real bound, not an arbitrary build-
// time number. Memory64 ships in Chrome ≥ 133, Firefox ≥ 134, Node
// ≥ 24. All linked libraries must also be -sMEMORY64=1.
const WMEM = ["-sMEMORY64=1",
              "-sSTACK_SIZE=8388608",
              "-sALLOW_MEMORY_GROWTH=1",
              // -fwasm-exceptions = native WebAssembly exception handling
              // proposal (vs legacy -fexceptions which inserts JS invoke_*
              // shims around indirect calls). Z3's C++ throws still work,
              // but the indirect-call dispatch is now pure wasm — no JS
              // frame is left on the call stack between wasm functions,
              // which JSPI requires for WebAssembly.Suspending to unwind
              // the stack on a yield. Under -fexceptions the github 4.4MB
              // bundle aborted at ~7 .bc with "trying to suspend JS
              // frames" because the bytecode interpreter dispatches CFunc
              // calls through invoke_* (JS) on the EH-enabled build.
              // DISABLE_EXCEPTION_CATCHING isn't compatible with the new
              // EH model — it was the legacy mode's switch.
              "-fwasm-exceptions"];

function run(cmd, args, extraEnv) {
  console.log(`\n[build] ${cmd} ${args.join(" ")}`);
  const r = spawnSync(cmd, args, {
    cwd: QJS, stdio: "inherit",
    env: { ...process.env, ...(extraEnv || {}) },
    shell: process.platform === "win32" && !cmd.endsWith(".exe"),
  });
  if (r.status !== 0) { console.error(`[build] FAILED rc=${r.status}`); process.exit(r.status || 1); }
}
function emcc(extra, out) {
  // libz3.a (wasm) appended to the link line — emcc auto-links the
  // C++ runtime so no extra -lstdc++ needed; Z3 is single-threaded so
  // no -pthread either.
  run(EM_PY, [EMCC, ...CFLAGS, ...SRC, Z3_LIB_W, ...WMEM, ...extra, "-o", out],
      { EM_CONFIG, EMSDK });
}
function size(f) {
  const p = join(QJS, f);
  return existsSync(p) ? `${f} ${(statSync(p).size / 1024 | 0)}KB` : `${f} MISSING`;
}

function native() {
  // ~150 Lexbor TUs blow past Windows' 8191-char cmd.exe limit when
  // gcc is spawned through a shell. A GCC @response-file is the
  // canonical fix for a long link line (whitespace-separated tokens;
  // every path here is space-free, forward-slash, relative to QJS).
  //
  // libz3.a is C++ (libstdc++ + libgcc statically baked so the exe is
  // portable across mingw-runtime versions). -lpthread is required by
  // Z3's mutex init even with -DZ3_SINGLE_THREADED=ON on mingw.
  const rsp = "native.rsp";
  writeFileSync(join(QJS, rsp),
    [...CFLAGS, "-o", "qjs.exe", ...SRC, Z3_LIB_N,
     "-static-libstdc++", "-static-libgcc", "-lstdc++", "-lpthread", "-lm"].join("\n"));
  run("gcc", ["@" + rsp]);
  console.log("[build] " + size("qjs.exe"));
}
function cli() {
  // NODERAWFS: the wasm CLI reads script files + writes the trace on
  // the real FS, exactly like native — drive.mjs treats them the same.
  // DBG=1 adds ASSERTIONS so a failed/oversized malloc prints its exact
  // size + JS stack instead of a bare abort trap (diagnostic only).
  const dbg = process.env.DBG === "1" ? ["-sASSERTIONS=2"] : [];
  emcc(["-sNODERAWFS", ...dbg], "qjs_wasm.js");
  console.log("[build] " + size("qjs_wasm.js") + " " + size("qjs_wasm.wasm"));
}
function mod() {
  // ES6 module, no auto-run: the extension worker does
  // `factory({...})` then `m.callMain(argv)` per forced schedule, with
  // script files in MEMFS and the trace read back from MEMFS.
  // DBG=1 adds ASSERTIONS (prints the exact malloc size + JS stack on
  // an allocation failure) for diagnosing the wasm64 size-corruption
  // OOM; off by default (ships clean).
  const dbg = process.env.DBG === "1" ? ["-sASSERTIONS=2"] : [];
  emcc(["-sMODULARIZE=1", "-sEXPORT_ES6=1",
        // HEAPU8: mdrive.mjs's snapshot-schedule path images linear memory
        // (boot once, restore per drive) — the Node-side proof of the model
        // ast-thread.js will use (the worker build already exports HEAPU8).
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV,HEAPU8",
        "-sINVOKE_RUN=0", "-sEXIT_RUNTIME=0",
        "-sENVIRONMENT=web,worker,node", "--pre-js", "prejs.js", ...dbg],
       "qjs_mod.mjs");
  console.log("[build] " + size("qjs_mod.mjs") + " " + size("qjs_mod.wasm"));
}

function worker() {
  // Classic-worker factory: -sEXPORT_NAME makes importScripts() set
  // self.createQJS (no ES6 import — the extension worker is classic so
  // it can also importScripts sourcemap.js). MEMFS only: a browser
  // Worker has no real FS, so script files + the trace live in MEMFS,
  // exactly as mdrive.mjs already proved. node kept in ENVIRONMENT so
  // the same artifact is node-testable before the harness run.
  // SINGLE_FILE embeds the wasm as base64 in the JS — no separate
  // .wasm fetch, so the extension's MV3 CSP (default-src 'none', no
  // connect-src 'self') can't block it; importScripts + the
  // 'wasm-unsafe-eval' source is all that's needed.
  //
  // JSPI (JavaScript Promise Integration) instruments wasm IMPORTS so
  // the engine can yield from arbitrary bytecode positions back to the
  // host and be RESUMED later from where it paused — enabling the
  // prioritisation system to interleave unbounded code paths (a JS path
  // that does not naturally terminate still yields control periodically
  // and can emit @H records when resumed). Unlike ASYNCIFY, JSPI uses
  // REAL wasm stack switching (no fixed save buffer → no depth cap on
  // the paused call stack), so there is no analysis bound on how deep a
  // JS callchain can be at the yield point. Browser support: Chrome 137+
  // shipping, Firefox/Safari in progress. JSPI_EXPORTS lists the wasm
  // exports that may suspend; qjs_host_yield is the C function that
  // returns a Promise the engine awaits.
  emcc(["-sMODULARIZE=1", "-sEXPORT_NAME=createQJS", "-sSINGLE_FILE=1",
        // HEAPU8 is needed at runtime so ast-thread.js's wasmBytes() can
        // read the linear-memory size for the memory-watchdog recycle in
        // the bc-compile loop and the BFS schedule loop. Without it, the
        // watchdog's `m.HEAPU8.buffer.byteLength` reads as 0 every
        // iteration, baselineBytes stays 0, the `memNow > baseline * 2`
        // check never fires, and the wasm grows unbounded until V8 traps
        // "memory access out of bounds" on the 346th github chunk.
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV,HEAPU8",
        "-sINVOKE_RUN=0", "-sEXIT_RUNTIME=0",
        "-sJSPI=1",
        "-sJSPI_EXPORTS=callMain",
        "-DQJS_HAS_JSPI=1",
        "-sENVIRONMENT=worker,node"],
       "qjs_worker.js");
  console.log("[build] " + size("qjs_worker.js"));
}

function stage() {
  // Move the worker engine into the extension. hostedge.js is the
  // single source of truth (drive.mjs tests it); it ships to the
  // extension as an importScripts-able module that just hands its own
  // text to the worker — no fetch(), so the MV3 CSP can't block it.
  const dst = resolve(ENGINE, "..", "extension", "lib", "qjs");
  mkdirSync(dst, { recursive: true });
  const wjs = join(QJS, "qjs_worker.js");
  if (!existsSync(wjs)) { console.error("[build] run `worker` first"); process.exit(1); }
  writeFileSync(join(dst, "qjs_worker.js"), readFileSync(wjs));
  const he = readFileSync(join(QJS, "hostedge.js"), "utf8");
  const dv = readFileSync(join(QJS, "driver.js"), "utf8");
  writeFileSync(join(dst, "hostedge.gen.js"),
    "// GENERATED by engine/build.mjs from engine/qjs/{hostedge,driver}.js — do not edit.\n" +
    "self.__HOSTEDGE_SRC = " + JSON.stringify(he) + ";\n" +
    "self.__HOSTDRIVER_SRC = " + JSON.stringify(dv) + ";\n");
  const wkb = statSync(join(dst, "qjs_worker.js")).size / 1024 | 0;
  console.log(`[build] staged -> extension/lib/qjs/ : qjs_worker.js ${wkb}KB + hostedge.gen.js (he ${he.length}B, drv ${dv.length}B)`);
}

const step = process.argv[2];
const all = !step;
if (all || step === "native") native();
if (all || step === "cli") cli();
if (all || step === "mod") mod();
if (all || step === "worker") worker();
if (all || step === "stage") stage();
console.log(`[build] done: ${step || "all"}`);
