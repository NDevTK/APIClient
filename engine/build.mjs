#!/usr/bin/env node
// Canonical forked-QuickJS build. ONE artifact, from ONE patched source
// tree (engine/qjs):
//
//   qjs_worker.js emcc MODULARIZE  — classic-worker factory (no ES6,
//                 EXPORT_NAME        importScripts-able), MEMFS, JSPI —
//                                    this is what the extension's offscreen
//                                    Web Worker runs, a fresh instance
//                                    per forced schedule. The ONLY target.
//
// NATIVE (gcc qjs.exe) AND THE NODE WASM-CLI (qjs_wasm.js) + MODULAR
// NODE BUILD (qjs_mod.mjs) ARE BANNED. They are fake-browser test beds:
// no JSPI scheduler, no host fetch (so the live grind's safeFetch-loaded
// lazy chunks are never driven), no real Chrome DOM/crypto — so they
// produce FALSE confidence (a real bundle "converged" on the CLI while
// the live wasm spun on a fetched-chunk orphan the CLI never loaded;
// proven repeatedly — the CLI cannot reproduce a live spin). ALL testing
// is through the live Chrome harness (testing/harness.js). The node
// drivers (drive.mjs / mdrive.mjs) are deleted with the CLI targets.
//
// The forced-execution controller (quickjs-forced.h, patched into
// quickjs.c) takes its config from qjsmain's --fe-* argv.
//
//   node engine/build.mjs            # builds + stages the worker
//   node engine/build.mjs worker|stage
import { spawnSync } from "node:child_process";
import { existsSync, statSync, readFileSync, writeFileSync, mkdirSync, readdirSync, rmSync } from "node:fs";
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
const Z3_LIB_W = relative(QJS, join(Z3, "bw", "libz3.a")).split(/[\\/]/).join("/");
const CFLAGS = ["-O1", "-w", "-D_GNU_SOURCE", "-DLEXBOR_STATIC",
                "-DQJS_HAVE_Z3=1", Z3_INC,
                "-I.", "-I../lexbor/source"];
// Opt-in extra -D defines (QJS_EXTRA_DEFINES="-DQJS_SPIN_PROBE"; never in ship builds) are
// appended at BUILD-HELPER CALL TIME (see `cflags` below), NOT here: a module-level const
// captures the env at LOAD, before a build STEP can set it — that silently dropped cowBuild's
// -DQJS_COW (g_cow_enabled stayed 0, COW inert). Call-time re-read honors a step-set define.
// QuickJS's own recursion (parser/GC) overflows emscripten's 64KB
// default stack on real minified bundles; grow it. Memory64 lifts
// the wasm32 4 GiB cap to the wasm64 spec ceiling (2^64 bytes); the
// platform/host enforces the real bound, not an arbitrary build-
// time number. Memory64 ships in Chrome ≥ 133, Firefox ≥ 134, Node
// ≥ 24. All linked libraries must also be -sMEMORY64=1.
const WMEM = ["-sMEMORY64=1",
              "-sSTACK_SIZE=8388608",
              "-sALLOW_MEMORY_GROWTH=1",
              // Remove legacy MEMFS: its wasm-side libc fopen 404s on deep /x/<host>
              // slice paths that the JS-side FS resolves fine under -sMEMORY64=1 — a
              // legacy-MEMFS×Memory64 bug, not a path issue. WASMFS is the Memory64-
              // correct filesystem backend.
              "-sWASMFS=1",
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
  // ASAN=1 → a wasm32 (NOT MEMORY64 — emscripten ASan is incompatible with it)
  // diagnostic build WITHOUT z3 (its prebuilt .a is MEMORY64-only, and z3 is not
  // on the async-resume path being debugged): catches a stray/aliased heap write
  // AT RUNTIME in the live Chrome harness. Diagnostic only; never shipped.
  const asan = !!process.env.ASAN;
  const _extra = process.env.QJS_EXTRA_DEFINES ? process.env.QJS_EXTRA_DEFINES.split(/\s+/).filter(Boolean) : [];
  const cflags = (asan ? CFLAGS.map((f) => (f === "-DQJS_HAVE_Z3=1" ? "-DQJS_HAVE_Z3=0" : f)) : CFLAGS).concat(_extra);
  const src = asan ? SRC.filter((f) => !/z3_shim/i.test(f)) : SRC;
  const z3 = asan ? [] : [Z3_LIB_W];
  const wmem = asan ? WMEM.filter((f) => f !== "-sMEMORY64=1") : WMEM;
  run(EM_PY, [EMCC, ...cflags, ...src, ...z3, ...wmem, ...extra, "-o", out],
      { EM_CONFIG, EMSDK });
}
function size(f) {
  const p = join(QJS, f);
  return existsSync(p) ? `${f} ${(statSync(p).size / 1024 | 0)}KB` : `${f} MISSING`;
}

// Keeping the vendored quickjs-ng base current is MANDATORY (CLAUDE.md), so a
// fork that is BEHIND upstream is a BUILD ERROR — not a notification a human is
// left to act on. Fetches upstream (cached to once/hour so rapid rebuilds stay
// fast), then refuses to build when apiclient-fork is missing upstream commits.
// Offline (fetch fails) → warn + proceed (never block a build for lack of
// network). SKIP_UPSTREAM_CHECK=1 bypasses for offline iteration ONLY — a stale
// engine must never be LANDED. The remediation is `node engine/sync.mjs apply`.
function gitQ(args) {
  return spawnSync("git", args, { cwd: QJS, encoding: "utf8",
    shell: process.platform === "win32" });
}
// True upstream is quickjs-ng/quickjs — resolve it by URL, NOT by the remote
// NAME. A clean clone's submodule `origin` is the FORK (per .gitmodules:
// url=APIClient-quickjs), so comparing against `origin/master` would diff the
// fork's (possibly stale) mirror, not real upstream — the bug that made this
// check pass only on a machine where `origin` happened to be quickjs-ng. Adds an
// `upstream` remote if no existing remote points at quickjs-ng.
function qjsngRemote() {
  const rv = gitQ(["remote", "-v"]).stdout || "";
  for (const line of rv.split("\n")) {
    const m = line.match(/^(\S+)\s+\S*quickjs-ng\/quickjs(?:\.git)?\s+\(fetch\)/);
    if (m) return m[1];
  }
  gitQ(["remote", "add", "upstream", "https://github.com/quickjs-ng/quickjs.git"]);
  return "upstream";
}
function upstreamSyncCheck() {
  if (process.env.SKIP_UPSTREAM_CHECK === "1") {
    console.warn("[build] upstream sync check SKIPPED (SKIP_UPSTREAM_CHECK=1) — offline iteration only; do NOT land a stale engine");
    return;
  }
  try {
    const REM = qjsngRemote();
    const stamp = join(ENGINE, ".work", ".upstream-fetch-stamp");
    const fresh = existsSync(stamp) && (Date.now() - statSync(stamp).mtimeMs) < 3600_000;
    if (!fresh) {
      const f = gitQ(["fetch", REM, "--quiet", "--tags"]);
      if (f.status !== 0) {
        console.warn(`[build] upstream sync check: git fetch failed (offline?) — proceeding without it\n        ${String(f.stderr || "").trim().split("\n")[0]}`);
        return;
      }
      try { mkdirSync(dirname(stamp), { recursive: true }); writeFileSync(stamp, ""); } catch {}
    }
    const base = gitQ(["merge-base", "HEAD", `${REM}/master`]).stdout.trim();
    if (!base) { console.warn(`[build] upstream sync check: no merge-base with ${REM}/master — proceeding`); return; }
    const behind = parseInt(gitQ(["rev-list", "--count", `${base}..${REM}/master`]).stdout.trim() || "0", 10);
    if (behind > 0) {
      const up = gitQ(["describe", "--tags", `${REM}/master`]).stdout.trim() || "?";
      console.error(`\n[build] REFUSING TO BUILD: engine is ${behind} commit(s) behind quickjs-ng upstream (${up}).`);
      console.error(`[build]   Keeping the fork current is mandatory. Sync first:`);
      console.error(`[build]     node engine/sync.mjs apply      # FF master + 3-way merge into apiclient-fork`);
      console.error(`[build]   then re-run the build, verify the FULL gate set + _idxdocs, and land.`);
      console.error(`[build]   (offline-iteration-only override, NEVER to land: SKIP_UPSTREAM_CHECK=1)\n`);
      process.exit(3);
    }
  } catch (e) {
    console.warn(`[build] upstream sync check skipped (${String((e && e.message) || e).split("\n")[0]})`);
  }
}

function worker() {
  upstreamSyncCheck();
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
  emcc(["-sMODULARIZE=1", "-sEXPORT_NAME=createQJS",
        // SINGLE_FILE embeds the wasm (MV3 CSP can't fetch a .wasm). COW_NO_SINGLE_FILE
        // emits a standalone qjs_worker.wasm for the write-barrier pass to instrument;
        // it is then supplied back via Module.wasmBinary (the loader honours it), so the
        // CSP constraint is met by an importScripts'd base64 blob, not a fetch.
        ...(process.env.COW_NO_SINGLE_FILE ? [] : ["-sSINGLE_FILE=1"]),
        // HEAPU8 is needed at runtime so ast-thread.js's wasmBytes() can
        // read the linear-memory size for the memory-watchdog recycle in
        // the bc-compile loop and the BFS schedule loop. Without it, the
        // watchdog's `m.HEAPU8.buffer.byteLength` reads as 0 every
        // iteration, baselineBytes stays 0, the `memNow > baseline * 2`
        // check never fires, and the wasm grows unbounded until V8 traps
        // "memory access out of bounds" on the 346th github chunk.
        "-sEXPORTED_RUNTIME_METHODS=ccall,callMain,ENV,HEAPU8",
        "-sINVOKE_RUN=0", "-sEXIT_RUNTIME=0",
        "-sJSPI=1",
        "-sJSPI_EXPORTS=callMain",
        "-DQJS_HAS_JSPI=1",
        "-sENVIRONMENT=worker,node",
        // Function names in the wasm name section make a deep-grind overflow stack
        // name the recursing C fn (e.g. qjs_t_free) instead of wasm-function[N] —
        // the right context to fix it. Off by default (+~3.5MB to the per-page
        // worker); `WASM_NAMES=1 node engine/build.mjs worker` turns it on.
        ...(process.env.WASM_NAMES ? ["--profiling-funcs"] : []),
        // ASAN=1 builds the wasm worker with AddressSanitizer to catch a stray
        // write / use-after-free AT RUNTIME in the live Chrome harness (the real
        // target — NOT a native test bed, so the JSPI/safeFetch/crypto path is
        // exercised). Names so the violation stack is readable. Heavy (shadow
        // memory) + slow; diagnostic only. `ASAN=1 node engine/build.mjs worker`.
        ...(process.env.ASAN ? ["-fsanitize=address", "--profiling-funcs", "-sERROR_ON_UNDEFINED_SYMBOLS=0"] : [])],
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
  // Remove any stale COW blob so a PRODUCTION (SINGLE_FILE) build never picks it up via
  // Module.wasmBinary (which would run the instrumented g_cow_enabled wasm instead of the
  // embedded one). cowBuild() calls stage() THEN re-writes the blob, so the COW path is fine.
  rmSync(join(dst, "qjs_wasm.cow.gen.js"), { force: true });
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

// COW pipeline (`node engine/build.mjs cow`): standalone wasm with COW enabled
// (-DQJS_COW -> g_cow_enabled=1) -> cow-barrier instruments every store -> stage the
// instrumented wasm as a base64 blob the worker importScripts (-> Module.wasmBinary).
// Separate from the production SINGLE_FILE build (which stays un-instrumented, COW off).
async function cowBuild() {
  process.env.COW_NO_SINGLE_FILE = "1";
  process.env.QJS_EXTRA_DEFINES = ((process.env.QJS_EXTRA_DEFINES || "") + " -DQJS_COW").trim();
  worker();   // -> qjs_worker.js (loader) + qjs_worker.wasm (standalone, COW on)
  stage();    // copies the non-SINGLE_FILE loader + hostedge.gen.js into the extension
  const { instrument } = await import("./cow-barrier.mjs");
  const r = instrument(new Uint8Array(readFileSync(join(QJS, "qjs_worker.wasm"))));
  const dst = resolve(ENGINE, "..", "extension", "lib", "qjs");
  const b64 = Buffer.from(r.bytes).toString("base64");
  writeFileSync(join(dst, "qjs_wasm.cow.gen.js"),
    "// GENERATED by engine/build.mjs COW pipeline — the cow-barrier-instrumented wasm.\n" +
    "self.__QJS_WASM = Uint8Array.from(atob(" + JSON.stringify(b64) + "), function (c) { return c.charCodeAt(0); });\n");
  console.log(`[build] COW: ${r.stores} stores + ${r.ranges} ranges instrumented, ${r.skipped} COW fns skipped; blob ${(r.bytes.length / 1048576).toFixed(1)}MB`);
}

const step = process.argv[2];
if (step === "native" || step === "cli" || step === "mod") {
  console.error("[build] `" + step + "` is BANNED — native (qjs.exe), the node wasm-CLI (qjs_wasm.js), and the modular node build (qjs_mod.mjs) are all fake-browser test beds (no JSPI scheduler / no host fetch / no real Chrome DOM+crypto) that give FALSE confidence. The CLI cannot reproduce a live spin. Build only the worker; test ONLY through the live Chrome harness (testing/harness.js).");
  process.exit(2);
}
const all = !step;
if (step === "cow") { await cowBuild(); }
else {
  if (all || step === "worker") worker();
  if (all || step === "stage") stage();
}
console.log(`[build] done: ${step || "all"}`);
