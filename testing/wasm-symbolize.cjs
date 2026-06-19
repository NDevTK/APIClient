// wasm-symbolize.cjs — resolve wasm trap stack frames (wasm-function[N]) to names.
//
// WHY THIS EXISTS: the COW build strips the wasm name section from the final blob
// (cow-barrier's binaryen round-trip drops internal names; only exports keep them),
// so a `RuntimeError: memory access out of bounds … wasm-function[35189]` from the
// grind is otherwise an opaque index. This was the single tool that turned the
// cold-orphan-drive OOB from five wrong guesses into a root cause: it named the
// frames as JS_NewObjectFromShape/js_def_malloc/qjs_t_leaf/strcmp/qjs_opq_make, which
// localized the corruption to engine-metadata-in-the-COW-heap and then to the
// fork-snapshot. Keep it for the per-flow-arena work (verifying its traps).
//
// WORKFLOW:
//   1. Build with names retained:           WASM_NAMES=1 node engine/build.mjs cow
//      (adds --profiling-funcs; the STANDALONE engine/qjs/qjs_worker.wasm keeps the
//       name section even though the instrumented blob does not).
//   2. Get the trap's frame indices from the worker stack (ast-thread _whyRecords
//      deep_callmain_throw .stack, "wasm-function[N]"). Binaryen preserves function
//      ORDER, so blob indices == qjs_worker.wasm indices — same N symbolizes.
//   3. node testing/wasm-symbolize.cjs engine/qjs/qjs_worker.wasm <N> [<N>...]
//
// Parses only the custom "name" section's function-name subsection (id=1).
const fs = require("fs");
const wasmPath = process.argv[2];
const want = process.argv.slice(3).map(Number);
if (!wasmPath) { console.error("usage: node wasm-symbolize.cjs <wasm> <idx> [<idx>...]"); process.exit(2); }
const buf = fs.readFileSync(wasmPath);
let p = 0;
function u32() { let r = 0, s = 0, b; do { b = buf[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return r >>> 0; }
if (buf.readUInt32LE(0) !== 0x6d736100) throw new Error("not a wasm file (bad magic)");
p = 8;
let nameSecStart = -1, nameSecEnd = -1;
while (p < buf.length) {
  const id = buf[p++];
  const size = u32();
  const body = p;
  if (id === 0) { // custom section
    const save = p;
    const nlen = u32();
    const nm = buf.toString("utf8", p, p + nlen);
    if (nm === "name") { nameSecStart = save + 1 + nlen; nameSecEnd = body + size; }
    p = save;
  }
  p = body + size;
}
if (nameSecStart < 0) { console.log("NO name section — rebuild with WASM_NAMES=1"); process.exit(0); }
p = nameSecStart;
const names = {};
let totalFns = 0;
while (p < nameSecEnd) {
  const sub = buf[p++];
  const subsize = u32();
  const subend = p + subsize;
  if (sub === 1) { // function names
    const count = u32();
    for (let i = 0; i < count; i++) {
      const idx = u32();
      const nlen = u32();
      names[idx] = buf.toString("utf8", p, p + nlen);
      p += nlen;
      totalFns++;
    }
  }
  p = subend;
}
console.log("total named fns:", totalFns);
for (const idx of want) console.log(`  [${idx}] = ${names[idx] || "(no name / out of range)"}`);
