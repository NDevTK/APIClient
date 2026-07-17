/* APIClient — NATIVE AddressSanitizer verification (`node engine/asan.mjs`).
 *
 * WHY THIS EXISTS: the emcc/wasm build's `-fsanitize=address` is REAL ASan, but run under node it is ~100x and,
 * on the full forced-multi-path smoke, takes 10+ minutes — impractical as a verification gate. This builds the
 * SAME test_forced.c smoke test NATIVELY with clang + real ASan (llvm-mingw, which ships the windows-gnu ASan
 * runtime that the stock mingw gcc lacks). Native ASan runs the full 19-fixture smoke in ~110s and reports a
 * heap-buffer-overflow / use-after-free deterministically with a symbolized stack — the memory gate for any
 * change to the COW delta / frame clone / array capture.
 *
 * PROCESS RULES (learned the hard way — see CLAUDE.md Testing):
 *   - Run ONE ASan build at a time. Concurrent 1 GiB ASan builds thrash the machine into swap for the session.
 *   - It is a periodic MEMORY gate, not the fast inner loop. Correctness = `node engine/build.mjs cow` (non-ASan,
 *     seconds) + test262; run this before landing a memory-touching change.
 *
 * Toolchain + Lexbor archive are cached under engine/.work (llvm-mingw is downloaded once, ~187 MB).
 */
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, writeFileSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const HOST = join(ENGINE, "host");
const WORK = join(ENGINE, ".work");
const LEXBOR_SRC = join(ENGINE, "lexbor", "source");
const LX_ARCHIVE = join(WORK, "liblexbor_clang.a");
const MINGW_DIR = join(WORK, "llvm-mingw-extract");
mkdirSync(WORK, { recursive: true });

function sh(cmd, args, opts = {}) { return spawnSync(cmd, args, { stdio: "inherit", shell: true, ...opts }); }
function findFile(dir, name) {
  if (!existsSync(dir)) return null;
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) { const r = findFile(p, name); if (r) return r; }
    else if (e.name === name) return p;
  }
  return null;
}
function findC(dir, out) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) { if (p.includes("windows_nt")) continue; findC(p, out); }   // posix port (mingw)
    else if (e.name.endsWith(".c")) out.push(p);
  }
  return out;
}

/* 1) llvm-mingw clang (native + windows-gnu ASan runtime). Downloaded once. */
function ensureClang() {
  let clang = findFile(MINGW_DIR, "clang.exe");
  if (clang) return clang;
  console.log("[asan] llvm-mingw not found — downloading (~187 MB, once)…");
  const api = spawnSync("curl", ["-s", "https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest"], { encoding: "utf8" });
  const url = (api.stdout.match(/https[^"']*ucrt-x86_64\.zip/) || [])[0];
  if (!url) { console.error("[asan] could not resolve llvm-mingw release URL"); process.exit(1); }
  const zip = join(WORK, "llvm-mingw.zip");
  if (sh("curl", ["-L", url, "-o", zip]).status !== 0) { console.error("[asan] download failed"); process.exit(1); }
  mkdirSync(MINGW_DIR, { recursive: true });
  if (sh("powershell", ["-NoProfile", "-Command", `Expand-Archive -Force '${zip}' '${MINGW_DIR}'`]).status !== 0) {
    console.error("[asan] extract failed"); process.exit(1);
  }
  clang = findFile(MINGW_DIR, "clang.exe");
  if (!clang) { console.error("[asan] clang.exe not found after extract"); process.exit(1); }
  return clang;
}

/* 2) Lexbor archive built with the same clang (no ASan — verifying the ENGINE; ASan's malloc interception still
 *    guards Lexbor-allocated heap). Cached; rebuild by deleting engine/.work/liblexbor_clang.a. */
function ensureLexbor(clang) {
  if (existsSync(LX_ARCHIVE)) return;
  const ar = join(dirname(clang), "llvm-ar.exe");
  const srcs = findC(join(LEXBOR_SRC, "lexbor"), []);
  console.log(`[asan] building Lexbor (${srcs.length} files, clang, once)…`);
  const objDir = join(WORK, "lxobj_clang"); mkdirSync(objDir, { recursive: true });
  const objs = [];
  for (let i = 0; i < srcs.length; i++) {
    const o = join(objDir, `lx_${i}.o`);
    const r = spawnSync(clang, ["-c", "-O2", "-w", "-D_GNU_SOURCE", "-DENABLE_DUMPS", "-DLEXBOR_STATIC",
      "-I", LEXBOR_SRC, srcs[i], "-o", o], { encoding: "utf8" });
    if (r.status === 0) objs.push(o);   // fs.c (posix S_IFLNK) fails on mingw + is unreferenced — skip it
  }
  if (spawnSync(ar, ["rcs", LX_ARCHIVE, ...objs]).status !== 0) { console.error("[asan] lexbor archive failed"); process.exit(1); }
  console.log("[asan] Lexbor archive cached -> " + LX_ARCHIVE);
}

const clang = ensureClang();
ensureLexbor(clang);

const SOLVER = (f) => join(HOST, "solver", f);
const sources = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c"].map((f) => join(QJS, f)).concat([
  SOLVER("cow.c"), SOLVER("engine.c"), SOLVER("flow.c"), SOLVER("decide.c"),
  SOLVER("concolic.c"), SOLVER("endpoint.c"), SOLVER("solve.c"),
  SOLVER("dom_cow.c"), SOLVER("attr_shadow.c"),
  join(HOST, "browser", "core", "loader", "document_scripts.c"),
  join(HOST, "test_forced.c"),
]);
const exe = join(WORK, "test_asan.exe");
const args = [
  "-fsanitize=address", "-g", "-O1", "-w",
  "-D_GNU_SOURCE", "-DENABLE_DUMPS", "-DLEXBOR_STATIC",
  "-DAPICLIENT_DEV=" + (process.argv.includes("release") ? "0" : "1"),
  "-I", QJS, "-I", HOST, "-I", join(HOST, "browser"), "-I", LEXBOR_SRC,
  ...sources, LX_ARCHIVE, "-o", exe,
];
console.log(`[asan] native clang build (${sources.length} sources + Lexbor)…`);
if (spawnSync(clang, args, { stdio: "inherit" }).status !== 0) { console.error("[asan] build FAILED"); process.exit(1); }

// The ASan runtime DLL lives in clang's bin dir — put it on PATH for the run. Default to the MINIMAL clone/COW
// fixture (seconds) — the per-change memory gate; pass `full` for the whole suite (rare, minutes: the fork tree
// is exponential, so it is a pre-land gate, never the inner loop). See CLAUDE.md §sanitizer.
const full = process.argv.includes("full");
const env = { ...process.env, PATH: dirname(clang) + ";" + process.env.PATH };
if (!full) env.APICLIENT_ASAN_MIN = "1";
console.log(full ? "[asan] running FULL native ASan smoke (exponential fork tree, minutes)…"
                 : "[asan] running MINIMAL native ASan gate (clone/COW/generator paths, ~seconds)…");
const t = spawnSync(exe, [], { stdio: "inherit", env });
if (t.status !== 0) { console.error("[asan] FAILED rc=" + (t.status ?? "signal") + " (ASan error or @H/@S fail above)"); process.exit(t.status || 1); }
console.log("[asan] PASS — native ASan clean + @H/@S green" + (full ? " (full)" : " (minimal)"));
