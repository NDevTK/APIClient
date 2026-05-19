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
             "dtoa.c", "quickjs-libc.c", "qjs_dom.c", ...lexborSrcs()];
const CFLAGS = ["-O1", "-w", "-D_GNU_SOURCE", "-DLEXBOR_STATIC",
                "-I.", "-I../lexbor/source"];
// QuickJS's own recursion (parser/GC) overflows emscripten's 64KB
// default stack on real minified bundles; grow it + the heap. The
// heap grows on demand from 64MB; MAXIMUM_MEMORY is the wasm32
// ceiling (2^32 = 4 GiB = 65536 64KiB pages — the most a wasm32
// linear memory can address, and the most Chrome will grant). Without
// this, emscripten's ALLOW_MEMORY_GROWTH default caps at 2GB, so a
// large real-site bundle re-executed across forced schedules could
// OOM while half the addressable space sat unused.
const WMEM = ["-sSTACK_SIZE=8388608", "-sINITIAL_MEMORY=67108864",
              "-sALLOW_MEMORY_GROWTH=1", "-sMAXIMUM_MEMORY=4294967296"];

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
  run(EM_PY, [EMCC, ...CFLAGS, ...SRC, ...WMEM, ...extra, "-o", out],
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
  const rsp = "native.rsp";
  writeFileSync(join(QJS, rsp),
    [...CFLAGS, "-o", "qjs.exe", ...SRC, "-lm"].join("\n"));
  run("gcc", ["@" + rsp]);
  console.log("[build] " + size("qjs.exe"));
}
function cli() {
  // NODERAWFS: the wasm CLI reads script files + writes the trace on
  // the real FS, exactly like native — drive.mjs treats them the same.
  emcc(["-sNODERAWFS"], "qjs_wasm.js");
  console.log("[build] " + size("qjs_wasm.js") + " " + size("qjs_wasm.wasm"));
}
function mod() {
  // ES6 module, no auto-run: the extension worker does
  // `factory({...})` then `m.callMain(argv)` per forced schedule, with
  // script files in MEMFS and the trace read back from MEMFS.
  emcc(["-sMODULARIZE=1", "-sEXPORT_ES6=1",
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV",
        "-sINVOKE_RUN=0", "-sEXIT_RUNTIME=0",
        "-sENVIRONMENT=web,worker,node", "--pre-js", "prejs.js"],
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
  emcc(["-sMODULARIZE=1", "-sEXPORT_NAME=createQJS", "-sSINGLE_FILE=1",
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV",
        "-sINVOKE_RUN=0", "-sEXIT_RUNTIME=0",
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
