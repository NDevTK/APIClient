// COW write-barrier pass (#5/#7/#10 keystone). Instruments the engine wasm so every
// memory WRITE marks its 64KB page(s) dirty (qjs_cow_mark_dirty / _range) — incremental
// dirty-tracking with NO O(memory) diff, feeding the per-flow COW snapshot (don't reuse
// another code path's heap). In-place transforms; verified standalone (validate +
// selftest) BEFORE it touches the build — a corrupt wasm-transform breaks the extension.
//
// WRITE-OP COVERAGE (provably complete via throw-on-unknown):
//  - store (incl. atomic store: id is Store w/ isAtomic) -> mark the ptr page.
//  - memory.fill / memory.copy / memory.init -> mark the [dest, dest+size) RANGE.
//  - atomic RMW/cmpxchg: the wasm is single-threaded (no shared memory) -> ABSENT; the
//    walk THROWS if any appear (then add handling), never silently skips (= a leak).
import binaryenImport from "binaryen";
const b = binaryenImport.default || binaryenImport;

// Read-only child refs per expression id; the walk recurses these to reach every write.
// Atomic-RMW/cmpxchg/wait/notify are intentionally ABSENT -> throw-on-unknown if present.
function childRefs(info) {
  switch (info.id) {
    case b.BlockId: return info.children;
    case b.IfId: return [info.condition, info.ifTrue, info.ifFalse].filter(Boolean);
    case b.LoopId: return [info.body];
    case b.BreakId: return [info.value, info.condition].filter(Boolean);
    case b.SwitchId: return [info.condition, info.value].filter(Boolean);
    case b.CallId: return info.operands;
    case b.CallIndirectId: return [info.target, ...info.operands];
    case b.LocalSetId: return [info.value];
    case b.GlobalSetId: return [info.value];
    case b.LoadId: return [info.ptr];
    case b.StoreId: return [info.ptr, info.value];
    case b.UnaryId: return [info.value];
    case b.BinaryId: return [info.left, info.right];
    case b.SelectId: return [info.ifTrue, info.ifFalse, info.condition];
    case b.DropId: return [info.value];
    case b.ReturnId: return info.value ? [info.value] : [];
    case b.MemoryGrowId: return [info.delta];
    case b.MemoryFillId: return [info.dest, info.value, info.size];
    case b.MemoryCopyId: return [info.dest, info.source, info.size];
    case b.MemoryInitId: return [info.dest, info.offset, info.size];
    case b.TryId: return [info.body, ...(info.catchBodies || [])];
    case b.ThrowId: return info.operands;
    default: throw new Error("cow-barrier: UNHANDLED expr id " + info.id + " (add to childRefs for provable completeness)");
  }
}

// field-less leaf ids: no children + never a write, and binaryen.js getExpressionInfo
// has no schema for them (Object.keys(undefined) throws) — short-circuit on getExpressionId.
const LEAF = new Set([b.NopId, b.UnreachableId, b.PopId, b.MemorySizeId, b.ConstId,
  b.LocalGetId, b.GlobalGetId, b.RefNullId, b.RefFuncId, b.DataDropId, b.ElemDropId,
  b.RethrowId] // wasm-EH rethrow: re-throws by catch label, no expr children, no write
  .filter((x) => x !== undefined));

function collectWrites(expr, ptrW, rangeW) {
  if (!expr) return;
  const id = b.getExpressionId(expr);
  if (LEAF.has(id)) return;
  if (id === b.StoreId) ptrW.push(expr);
  else if (id === b.MemoryFillId || id === b.MemoryCopyId || id === b.MemoryInitId) rangeW.push({ expr, id });
  const info = b.getExpressionInfo(expr);
  for (const c of childRefs(info)) collectWrites(c, ptrW, rangeW);
}

const rGetDest = (e, id) => id === b.MemoryFillId ? b._BinaryenMemoryFillGetDest(e) : id === b.MemoryCopyId ? b._BinaryenMemoryCopyGetDest(e) : b._BinaryenMemoryInitGetDest(e);
const rSetDest = (e, id, v) => id === b.MemoryFillId ? b._BinaryenMemoryFillSetDest(e, v) : id === b.MemoryCopyId ? b._BinaryenMemoryCopySetDest(e, v) : b._BinaryenMemoryInitSetDest(e, v);
const rGetSize = (e, id) => id === b.MemoryFillId ? b._BinaryenMemoryFillGetSize(e) : id === b.MemoryCopyId ? b._BinaryenMemoryCopyGetSize(e) : b._BinaryenMemoryInitGetSize(e);
const rSetSize = (e, id, v) => id === b.MemoryFillId ? b._BinaryenMemoryFillSetSize(e, v) : id === b.MemoryCopyId ? b._BinaryenMemoryCopySetSize(e, v) : b._BinaryenMemoryInitSetSize(e, v);

function findExport(m, name, kind) {
  for (let i = 0; i < m.getNumExports(); i++) {
    const ei = b.getExportInfo(m.getExportByIndex(i));
    if (ei.name === name && ei.kind === kind) return ei.value;
  }
  return null;
}

export function instrument(wasmBytes) {
  const m = b.readBinary(wasmBytes);
  m.setFeatures(m.getFeatures() | b.Features.All); // memory64 + bulk-memory + EH for validation

  // SELF-REFERENCE: never instrument the COW infrastructure functions — their own
  // bitmap/restore stores would recurse through the barrier. Identify them by their
  // qjs_cow_ exports' internal names (names are dropped on round-trip; exports keep them).
  const cowFns = new Set();
  for (let i = 0; i < m.getNumExports(); i++) {
    const ei = b.getExportInfo(m.getExportByIndex(i));
    if (ei.kind === b.ExternalFunction && ei.name.includes("qjs_cow_")) cowFns.add(ei.value);
  }
  const ptrW = [], rangeW = [];
  let skipped = 0;
  for (let i = 0; i < m.getNumFunctions(); i++) {
    const fi = b.getFunctionInfo(m.getFunctionByIndex(i));
    if (cowFns.has(fi.name)) { skipped++; continue; } // COW infrastructure — never instrument
    if (fi.body) collectWrites(fi.body, ptrW, rangeW);
  }
  if (!ptrW.length && !rangeW.length) { m.dispose(); return { bytes: wasmBytes, stores: 0, ranges: 0 }; }

  const mark = findExport(m, "qjs_cow_mark_dirty", b.ExternalFunction);
  if (!mark) throw new Error("cow-barrier: export 'qjs_cow_mark_dirty' not found");
  // ptr/size index type from a real write (i32, or i64 under MEMORY64).
  const ptrType = ptrW.length ? b.getExpressionType(b._BinaryenStoreGetPtr(ptrW[0]))
    : b.getExpressionType(rGetDest(rangeW[0].expr, rangeW[0].id));
  const zero = (t) => t === b.i64 ? m.i64.const(0n) : m.i32.const(0);
  // scratch globals: $g store-ptr/nested, $d range-dest (survives value-eval), $s range-size.
  m.addGlobal("cow_g", ptrType, true, zero(ptrType));
  let markRange = null, sizeType = ptrType;
  if (rangeW.length) {
    markRange = findExport(m, "qjs_cow_mark_dirty_range", b.ExternalFunction);
    if (!markRange) throw new Error("cow-barrier: export 'qjs_cow_mark_dirty_range' not found");
    sizeType = b.getExpressionType(rGetSize(rangeW[0].expr, rangeW[0].id));
    m.addGlobal("cow_d", ptrType, true, zero(ptrType));
    m.addGlobal("cow_s", sizeType, true, zero(sizeType));
  }

  // store ptr -> block[ set $g=ptr ; mark($g) ; get $g ]  (addr evaluated once)
  for (const st of ptrW) {
    const ptr = b._BinaryenStoreGetPtr(st);
    b._BinaryenStoreSetPtr(st, m.block(null, [
      m.global.set("cow_g", ptr),
      m.call(mark, [m.global.get("cow_g", ptrType)], b.none),
      m.global.get("cow_g", ptrType),
    ], ptrType));
  }
  // range: dest -> tee into $d ; size -> tee into $s + mark_range($d,$s) (eval order
  // dest,value/source,size means $d is set before $s, and value's nested stores use $g).
  for (const { expr, id } of rangeW) {
    const dest = rGetDest(expr, id), size = rGetSize(expr, id);
    rSetDest(expr, id, m.block(null, [m.global.set("cow_d", dest), m.global.get("cow_d", ptrType)], ptrType));
    rSetSize(expr, id, m.block(null, [
      m.global.set("cow_s", size),
      m.call(markRange, [m.global.get("cow_d", ptrType), m.global.get("cow_s", sizeType)], b.none),
      m.global.get("cow_s", sizeType),
    ], sizeType));
  }

  if (!m.validate()) throw new Error("cow-barrier: module failed validation after instrumentation");
  const out = m.emitBinary();
  m.dispose();
  return { bytes: out, stores: ptrW.length, ranges: rangeW.length, skipped };
}

// file mode: `node cow-barrier.mjs in.wasm [out.wasm]` — instrument a real wasm.
if (process.argv[2] && process.argv[2].endsWith(".wasm")) {
  const { readFileSync, writeFileSync } = await import("node:fs");
  const inp = process.argv[2], outp = process.argv[3] || inp.replace(/\.wasm$/, ".cow.wasm");
  const t0 = process.hrtime.bigint();
  const r = instrument(new Uint8Array(readFileSync(inp)));
  writeFileSync(outp, r.bytes);
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`cow-barrier: ${inp} -> ${outp}: ${r.stores} stores + ${r.ranges} ranges instrumented, ${r.skipped} COW fns skipped, validated OK (${ms.toFixed(0)}ms, ${(r.bytes.length / 1048576).toFixed(1)}MB)`);
}
// selftest: a MEMORY64 module with a nested store + a memory.fill + a memory.copy.
else if (process.argv[2] === "selftest") {
  const m = new b.Module();
  m.setFeatures(b.Features.All);
  m.setMemory(1, 10, "memory", [], false, true); // MEMORY64
  m.addFunction("qjs_cow_mark_dirty", b.createType([b.i64]), b.none, [], m.nop());
  m.addFunctionExport("qjs_cow_mark_dirty", "qjs_cow_mark_dirty");
  m.addFunction("qjs_cow_mark_dirty_range", b.createType([b.i64, b.i64]), b.none, [], m.nop());
  m.addFunctionExport("qjs_cow_mark_dirty_range", "qjs_cow_mark_dirty_range");
  const body = m.block(null, [
    m.i64.store(0, 0, m.i64.const(8n), m.i64.const(1n)),                 // store
    m.i64.store(0, 0, m.i64.const(16n), m.i64.const(42n)),               // store
    m.memory.fill(m.i64.const(64n), m.i32.const(0), m.i64.const(128n)),  // range
    m.memory.copy(m.i64.const(256n), m.i64.const(64n), m.i64.const(32n)),// range
  ], b.none);
  m.addFunction("f", b.none, b.none, [], body);
  const bytes = m.emitBinary(); m.dispose();
  const r = instrument(bytes);
  console.log("selftest: stores =", r.stores, "(expect 2), ranges =", r.ranges, "(expect 2); validated OK");
}
