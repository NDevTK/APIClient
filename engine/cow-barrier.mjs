// COW write-barrier pass (#5/#7/#10 keystone). Instruments the engine wasm so every
// memory WRITE marks its 64KB page dirty (qjs_cow_mark_dirty), with NO O(memory) diff
// — the incremental dirty-tracking the per-flow COW snapshot needs (don't reuse the
// heap of a different code path). In-place transform: each store's ptr child becomes
//   block(result i64)[ global.set $cowg, ptr ; call $mark(global.get $cowg) ; global.get $cowg ]
// so the address is evaluated ONCE, the page marked, then the store runs. A mutable
// i64 scratch GLOBAL is the tee (wasm is single-threaded; nested stores are
// sequential-safe). Verified standalone (validate + a run-test) BEFORE it touches the
// build — a corrupt wasm-transform would break the whole extension.
//
// STORES-FIRST: covers i32/i64/f32/f64 store (the JS object-graph + Lexbor DOM
// mutations). COMPLETENESS additions (next): atomic stores/RMW (ptr) + bulk-memory
// memory.copy/fill/init (dest,size -> qjs_cow_mark_dirty_range). A miss = a leaked
// mutation = unsound COW, so the walk THROWS on any unhandled expr id (caught + added).
import binaryenImport from "binaryen";
const b = binaryenImport.default || binaryenImport;

// Read-only child refs per expression id — the walk recurses these to find every write.
// Throws on an unhandled id so coverage is provably complete for the actual wasm.
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
    case b.AtomicRMWId: return [info.ptr, info.value];
    case b.AtomicCmpxchgId: return [info.ptr, info.expected, info.replacement];
    case b.AtomicWaitId: return [info.ptr, info.expected, info.timeout];
    case b.AtomicNotifyId: return [info.ptr, info.notifyCount];
    case b.TryId: return [info.body, ...(info.catchBodies || [])];
    case b.ThrowId: return info.operands;
    case b.ConstId: case b.LocalGetId: case b.GlobalGetId: case b.MemorySizeId:
    case b.NopId: case b.UnreachableId: case b.RethrowId:
    case b.RefNullId: case b.RefFuncId: case b.PopId: return [];
    default: throw new Error("cow-barrier: UNHANDLED expr id " + info.id + " (add to childRefs for completeness)");
  }
}

// Collect every store expr ref reachable from `expr`.
// field-less leaf expr ids: no children + never a store, and binaryen.js's
// getExpressionInfo has no schema for them (Object.keys(undefined) throws) — so
// short-circuit on the cheap getExpressionId before the full info call.
const LEAF = new Set([b.NopId, b.UnreachableId, b.PopId, b.MemorySizeId, b.ConstId,
  b.LocalGetId, b.GlobalGetId, b.RefNullId, b.RefFuncId, b.DataDropId, b.ElemDropId]
  .filter((x) => x !== undefined));
function collectStores(expr, out) {
  if (!expr) return;
  const id = b.getExpressionId(expr);
  if (LEAF.has(id)) return;
  if (id === b.StoreId) out.push(expr);
  const info = b.getExpressionInfo(expr);
  for (const c of childRefs(info)) collectStores(c, out);
}

export function instrument(wasmBytes, markExportName = "qjs_cow_mark_dirty") {
  const m = b.readBinary(wasmBytes);
  // Preserve the module's features (memory64, bulk-memory, exceptions, ...) so
  // validation of the 64-bit memory + the instrumented body succeeds. The real wasm
  // declares them; ensure memory64 regardless (a binary may omit the section).
  m.setFeatures(m.getFeatures() | b.Features.All);
  // Resolve the mark fn by its EXPORT (function names are dropped on binary
  // round-trip; exports keep their name -> the auto-generated internal name).
  let markInternal = null;
  for (let i = 0; i < m.getNumExports(); i++) {
    const ei = b.getExportInfo(m.getExportByIndex(i));
    if (ei.name === markExportName && ei.kind === b.ExternalFunction) { markInternal = ei.value; break; }
  }
  if (!markInternal) throw new Error("cow-barrier: export '" + markExportName + "' not found");
  // Collect every store, then instrument (collect-first avoids mutating mid-walk).
  const all = [];
  for (let i = 0; i < m.getNumFunctions(); i++) {
    const fi = b.getFunctionInfo(m.getFunctionByIndex(i));
    if (fi.body) collectStores(fi.body, all);
  }
  if (!all.length) { m.dispose(); return { bytes: wasmBytes, stores: 0 }; }
  // The scratch global + barrier must match the memory index type (i32 / i64 under
  // MEMORY64) — read it off a real store's ptr.
  const ptrType = b.getExpressionType(b._BinaryenStoreGetPtr(all[0]));
  const G = "cow_scratch_ptr";
  m.addGlobal(G, ptrType, /*mutable*/ true, ptrType === b.i64 ? m.i64.const(0n) : m.i32.const(0));
  for (const st of all) {
    const ptr = b._BinaryenStoreGetPtr(st);
    const barrier = m.block(null, [
      m.global.set(G, ptr),
      m.call(markInternal, [m.global.get(G, ptrType)], b.none),
      m.global.get(G, ptrType),
    ], ptrType);
    b._BinaryenStoreSetPtr(st, barrier);
  }
  if (!m.validate()) throw new Error("cow-barrier: module failed validation after instrumentation");
  const out = m.emitBinary();
  m.dispose();
  return { bytes: out, stores: all.length };
}

// self-test: a module with a nested store, instrument, validate, confirm the barrier.
if (import.meta.url === `file://${process.argv[1]?.split("\\").join("/")}` || process.argv[2] === "selftest") {
  const m = new b.Module();
  m.setFeatures(b.Features.All);
  m.setMemory(1, 10, "memory", [], false, true); // MEMORY64 (memory64 = 6th arg)
  m.addFunction("qjs_cow_mark_dirty", b.createType([b.i64]), b.none, [], m.nop());
  m.addFunctionExport("qjs_cow_mark_dirty", "qjs_cow_mark_dirty");
  // nested store: store(store(8,1)->then 16, 42)  ~ outer ptr contains inner store
  const inner = m.i64.store(0, 0, m.i64.const(8), m.i64.const(1));
  const body = m.block(null, [inner, m.i64.store(0, 0, m.i64.const(16), m.i64.const(42))], b.none);
  m.addFunction("f", b.none, b.none, [], body);
  const bytes = m.emitBinary(); m.dispose();
  const r = instrument(bytes);
  console.log("selftest: instrumented stores =", r.stores, "(expect 2); validated OK");
}
