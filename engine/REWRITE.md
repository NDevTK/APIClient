# Forced-execution engine — rewrite architecture (fresh quickjs-ng fork)

Status: design blueprint for a principled re-fork. NOT a from-scratch rewrite — re-fork clean
quickjs-ng, design the THREE cores correctly, and PORT the proven mechanisms. The current fork
(`engine/qjs`) accreted three debts that cannot be retrofitted; each blocks the next-best design.

## Why re-fork (the three un-retrofittable debts)

1. **Forced exploration = schedule-string + cursor + per-schedule RE-RUN.** The orphan-enum BFS
   re-executes a function from baseline for every schedule, re-deciding the whole prefix each time
   (`qjs_fe_sched`/`qjs_fe_cur`, the loop-revisit fixpoint, `lcap` all keyed on the schedule string).
   The right model is **branch-fork** — snapshot at the branch, explore both arms as parked siblings,
   re-decide nothing, re-run nothing. Fork and schedule-rerun are mutually-exclusive cores; you cannot
   bolt fork on (proven: attempting it collides with `qjs_fe_sched` and the per-arm COW reverts).
2. **Dual COW.** A byte word-log (`qjs_cow_undo`) plus FOUR typed trails bolted on for refcount-exact
   reverts (`vt` shared-scope slots, buffer-trail for `js_realloc`'d buffers, shape-trail for refcounted
   shapes). They grew independently; sharing a forked heap means reconciling four trails. **One typed
   undo-log** (every logged write tagged ref-vs-raw, so revert is refcount-exact uniformly) erases the
   whole patchwork and makes fork trivial.
3. **Per-call-form trampoline patches.** Normal / method / `.call` / `.apply` / `map` were each
   heap-switched as a separate patch onto a C-recursive `JS_CallInternal`. In a clean design the
   **heap trampoline is the calling convention**, ground-up — no per-form special case, no C recursion
   to overflow, every frame on the segmented heap stack, per-opcode preemptible.

## The four cores — design these FRESH

1. **Calling convention: heap-trampolined by default.** `JS_CallInternal` never C-recurses for a JS→JS
   call; `setup_callee` pushes the callee frame on the segmented arena and continues the dispatch loop;
   `OP_return` pops + splices. Async/generator/`.call`/`.apply`/builtin-callback (map/forEach/reduce)
   are the SAME path, not exceptions. Overflow is impossible by construction; the per-opcode return-to-
   scheduler (`YIELD_POLL`) is universal, boot included.

2. **COW: one typed undo-log.** Each store through the barrier logs `{addr, old, kind}` where `kind ∈
   {RAW, JSVALUE, SHAPEREF, BUFPTR}`. Revert dispatches on `kind` (raw byte-restore; JSValue
   decref-current+restore-old; shaperef `js_free_shape`+restore; bufptr defer-free + restore). One log,
   one revert, one capture, one apply — the buffer-trail/shape-trail/vt-trail all collapse into it.
   Per-flow snapshot = a segment `[mark, n)` of this one log. Eviction blob = serialize a segment.

3. **Forced exploration: branch-fork (the fork IS the BFS).** At an opaque branch: take arm A in the
   current flow; snapshot a sibling (heap-stack copy + COW-log segment, NO revert — current keeps
   running) parked at arm B's PC. The WFQ resumes siblings by value-of-information. No schedule string,
   no cursor, no loop-revisit fixpoint, no re-execution. A loop is a self-fork that the WFQ starves by
   emitted output. Z3 prunes infeasible sibling arms before they're enqueued (`Φ ∧ predicate` UNSAT →
   don't fork). Opaque inputs (`location.*`, `postMessage`, fetch replies) stay opaque for control flow
   (never force a branch) but carry a concrete example value per-param when a real sample exists.

4. **Scheduling: ONE WFQ at both levels.** `priority.js`'s weight (`1 + epRate + UCB-explore +
   fairness-floor`, order-only, never drops) governs BOTH the host's cross-page fibers AND the engine's
   within-page flows (fork siblings / orphans / cb-drives) — same policy, virtual-time fair-share,
   value = emitted output (@H endpoint / @S sink, weighted equally), never identity, never a static
   reach guess, never a no-progress count. Nothing terminates; the unproductive is starved to ~0 CPU,
   snapshot-able, evictable to IndexedDB (RAM = hot working set, disk = the platform floor), cross-
   session resumable.

## PORT (proven, orthogonal to the architecture — do NOT re-derive)

- Lexbor DOM (`qjs_dom.c`) + the JS prelude (event/classList/dataset).
- Z3 path-satisfiability + branch pruning (security-only; never invents API values).
- Host-edge model (`hostedge.js`): fetch/XHR/eval recording → @H, taint → @S, opaque/concrete inputs,
  the event-loop pump, the safe-fetch SOP/CORS discipline, the one-WASM-instance-per-page boundary.
- Security model (`SECURITY.md`): auth on `sender.tab.url`, principal = `MessageSender.origin`, state
  in offscreen + IndexedDB, all network via `safe-fetch.js`/`pageContextFetch`.
- Trampoline GC/async/generator edge-case handling + COW refcount-exactness RULES (lift the hard-won
  correctness; re-derive nothing).
- Eviction blob format + the heap-stack chunk pointer-fixup (`qjs_drive_fixup` — verified this session).
- Build pipeline (`build.mjs cow`, the binaryen cow-barrier pass) — re-target, don't redesign.

## Build order (incremental, design-fixtures as the harness)

1. Fork clean quickjs-ng (`fork` remote, new branch) + the COW WASM build.
2. Heap-trampoline-by-default in `JS_CallInternal` (the calling convention). Verify deep/infinite
   recursion is overflow-proof + per-opcode preemptible.
3. One typed COW undo-log. Verify refcount-exact revert on array/property/shape/closure mutations.
4. Branch-fork scheduler + the one WFQ (fork = BFS; siblings parked; starve by output). Verify forced
   multi-path reaches both arms without re-running.
5. Port DOM / Z3 / host-edge / security model.
6. Eviction (serialize a COW-log + heap-stack segment to IDB; resume cross-session).

## Verification discipline (unchanged)

Design fixtures (`testing/fixtures/c_*`), live Chrome harness (`restart`), QUALITATIVE design
correctness only — responsive (infinite recursion/loops STARVE, never hang), value-correct (map=84,
reduce=6, sort=1), produces its endpoint, no heap corruption. NEVER a count-regression test, NEVER a
CDN bundle as a progress metric, NEVER native/node-CLI. One WASM instance per page; clear storage
before concluding a bug.

## What carries over from the current session (the right primitives, wrong substrate)

The park / per-flow COW capture / heap-stack snapshot+relocate+serialize / eviction primitives built
this session are the CORRECT primitives — they just need to sit under a fork-native scheduler and the
one typed COW log instead of being patched onto the rerun model + dual COW. `qjs_drive_fixup` (the
chunk-internal pointer fixup, verified) ports directly.
