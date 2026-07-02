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
import { mkdirSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const QJS = join(ENGINE, "qjs");
const HOST = join(ENGINE, "host");
const OUT = join(HOST, "out");
const EMSDK = join(ENGINE, ".work", "emsdk");
const EMCC = join(EMSDK, "upstream", "emscripten", process.platform === "win32" ? "emcc.bat" : "emcc");

if (!existsSync(EMCC)) { console.error("[build] emcc not found at " + EMCC); process.exit(1); }
mkdirSync(OUT, { recursive: true });

const sources = ["quickjs.c", "libregexp.c", "libunicode.c", "dtoa.c", "quickjs-libc.c"]
  .map((f) => join(QJS, f))
  .concat([join(HOST, "main.c")]);

const args = [
  ...sources,
  "-I", QJS,
  "-O1", "-w",
  "-D_GNU_SOURCE",
  "-sALLOW_MEMORY_GROWTH=1",
  "-sSTACK_SIZE=8388608",
  "-sEXIT_RUNTIME=0",
  "-sINVOKE_RUN=0",
  "-sMODULARIZE=1",
  "-sEXPORT_ES6=1",
  "-sEXPORTED_RUNTIME_METHODS=callMain,FS",
  "-sNODERAWFS=0",
  "-o", join(OUT, "qjs.mjs"),
];

console.log("[build] emcc " + sources.length + " sources -> engine/host/out/qjs.mjs");
const r = spawnSync(EMCC, args, { stdio: "inherit", shell: true, cwd: QJS });
if (r.status !== 0) { console.error("[build] FAILED rc=" + r.status); process.exit(r.status || 1); }
console.log("[build] OK -> " + resolve(join(OUT, "qjs.mjs")));
