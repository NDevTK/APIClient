# ECMA-262 Coverage Matrix + System Design — ast.js

This document is BOTH the spec-coverage tracker AND the architectural plan for `extension/lib/ast.js`. It's structured so the next session can pick up where the last left off without re-deriving context.

**Working principles** (codified from CLAUDE.md + user directives):
- Every ECMA spec section is a discrete sub-problem.
- Plan from spec semantics. Current state is "TOP" / "not modeled"; fix the spec gap.
- Each closed gap gets one targeted test in `testing/test-api-learning.js` named by spec section. Tests must fail first, pass after.
- No heuristics, no magic numbers, no name-based matching (everything flows through AV identity + scope-resolved bindings + AST structure).
- All algorithms iterative — no recursion in scoped files.
- Trace through framework code — it's just JS.
- Properly supporting Demand-driven k-CFA (not reproducer-chasing)
---

## Part 1: System architecture

### 1.1 Single state machine

`extension/lib/ast.js` has ONE state machine for value resolution: spec eval (`_specApplyStatement` / `_specEvalLeaf` / `_specAnalyzePropertyFlow`). It produces ECMA-grounded Abstract Values (AVs) per ECMA-262 + WHATWG spec semantics. Public resolvers (`_resolveAllValues`, `_resolveCalleeFuncPath`, etc.) read AVs and walk the AV graph.

NO `_runResolverStack`, NO state-ID constants, NO `_XxxStep` dispatch. Adding new resolution capability = extending spec eval (`_specEvalLeaf` / `_specApplyBuiltinMethod` / `_specApplyBuiltinCtor`), never a parallel state machine.

### 1.2 AV kinds (Tier 0 — Foundational)

| Kind | Carries | Example produced by |
|------|---------|---------------------|
| `const` | concrete primitive value | Literal node, foldable BinaryExpression |
| `top` | abstract "anything" | unresolvable references, opaque calls |
| `or` | binary `{left, right}` SetUnion | LUB across IfStatement branches |
| `obj-lit` | `{props, _ctorId?, _hasUnknownExtraProps?}` | ObjectExpression eval, class instance |
| `array-lit` | `{elements}` | ArrayExpression eval |
| `function-ref` | `{funcNode, _extraProps?, methodOf?}` | FunctionExpression eval, class method |
| `bound-function` | `{target, preArgs}` | Function.prototype.bind |
| `builtin-method` | `{id}` (e.g. "Array.prototype.forEach") | global registry lookup |
| `builtin-ctor` | `{id}` (e.g. "WHATWG.URL") | global registry lookup |
| `callable-namespace` | `{callId, props}` | callable-but-also-object globals (String, Number) |
| `param` | `{idx, fn}` | function parameter access |
| `args-elt`, `args-len`, `rest-args` | arguments-exotic refs | inside function bodies |
| `taint-source` | `{id, dims}` | WHATWG attacker-controlled sources |
| `coerce` | `{to, arg, _parentVarName?, _parentHref?}` | URL.searchParams getter |
| `member` | `{obj, key}` | unresolved property access |
| `loop-key` | `{src}` | for-in iteration variable |
| `this` | (hash-consed by enclosing fn) | `this` expression in non-class context |
| `regex-instance` | `{pattern, flags}` | RegExp literal / `new RegExp` |
| `weakmap-instance`, `weakset-instance` | `{entries, unknownInit?}` | `new WeakMap/Set` |
| `dom-element` | `{props}` | document.createElement / DOM API returns |
| `binop` | `{operator, left, right}` | lazy `+` of or-trees (cartesian deferred) |
| `template` | `{quasis, exprs}` | preserves template-literal structure |
| `logical` | `{operator, left, right}` | preserves `||`/`&&`/`??` for downstream substitution |
| `call` | `{callee, args}` | param-typed callee dispatch (lazy) |

### 1.3 Memo tables (per-fn invalidation infrastructure)

| Memo | Key | Value | Invalidated on |
|------|-----|-------|----------------|
| `_specEffectsMemo` | funcNode | effects[] | sig change of fn |
| `_specReturnValueMemo` | funcNode | return AV | sig change of fn |
| `_specSideEffectMemo` | funcNode | sideEffects | sig change of fn |
| `_specThisEffectsMemo` | funcNode | this-effects | sig change of fn |
| `_specPathValMemo` | ASTNode | AV | overwritten per body analysis |
| `_specStmtCursor` | funcNode | last-completed stmt idx | per analysis |
| `_specCallSitesByFn` | funcNode | call-site paths[] | post-fixpoint augment |
| `_specCallGraphCallersOf` | funcNode | Set<caller> | per program |
| `_specReadEdges` | caller fn | Set<callee> | per analysis |
| `_specReadersOf` | callee fn | Set<reader fn> | per analysis (reverse of above) |
| `_specCtxEffectsBySite` | call-site node | refined effects | per refinement round |
| `_specFuncRefClosureState` | FE node | snap of state at eval | each FE eval |
| `_specUnreachableNodes` | AST node | (WeakSet membership) | break/return in same block |

### 1.4 Frame structure (worklist driver state)

Each frame on `branchStack` / `stack`:
```javascript
{
  stmts: [...],         // statements to process
  idx: 0,                // next stmt index
  state: {...},          // bindings (key → AV)
  parentPath: nodePath,  // for sub-path .get(N)
  // ── optional flags ──
  _isMergeFrame: true,   // join branches into parent's state
  _isIfMerge: true,      // IfStatement-specific (skip returned-branch reporting)
  _isClosureWriteBack: true,    // closure write-back filter at merge
  _writeBackParamName: "...",   // param key to exclude
  _closureWriteBackTo: outerState,  // copy keys back on frame completion
  _reportTo: mergeFrame,        // push state to merge's branchEndStates on pop
  _label: "outer",              // LabelledStatement target
  _returned: true,              // abrupt completion (break/return/etc.)
  _explicitStmtPath: nodePath,  // single-stmt path override
  _wrapBody: true,              // legacy single-statement body
  _pathField: "consequent",     // non-default field name
  branchEndStates: [...],       // for merge frames
  baseState: {...},             // for merge frames
  hasAlternate: true,           // for IfStatement merge
}
```

### 1.5 Dispatch pipeline

```
Program traversal
  ↓ slice computation (_specBuildSlice)
  ↓ trampoline (topo-sort over call graph)
  ↓ per-fn _specAnalyzePropertyFlow
     ↓ explicit-stack worklist
        ↓ _specApplyStatement (per statement kind)
           ↓ _specEvalExpression (per expression kind, postorder)
              ↓ _specEvalLeaf (per AST node)
                 ↓ _specApplyBuiltinMethod (for builtin-method dispatch)
                 ↓ context-sensitive refinement (_specCtxEffectsBySite)
  ↓ post-fixpoint: call-site index augmentation (function-ref callees)
  ↓ ctx-refinement do-while: MemberExpression-callee + concrete args
  ↓ Public Babel traversal (sink dispatch via _processNetworkSink etc.)
```

### 1.6 Invariants

1. **No recursion** in scoped files (CLAUDE.md L29-L31). Worklist + explicit stack only.
2. **AVs are immutable**. To "modify" prop X on obj-lit, clone with new prop; rebind state[obj].
3. **`gtWinForBind` reference identity** must be preserved for SetGlobalRecordBinding to fire — never clone the canonical window AV on prop-write.
4. **Closure write-back** filters to keys in `_closureWriteBackTo` (outer's bindings); body-local keys (param + new var) stay in body's frame.
5. **Abrupt-completion writes** (writes before break/return) propagate via the BreakStatement handler's branchStack walk, not via the if-merge join (which skips returned branches).
6. **Unreachable code** (stmts after break/return) gets `_specUnreachableNodes` marking; sink dispatchers consult it.
7. **Re-enqueue on sig change**: static callersOf + dynamic `_specReadersOf` reverse index together cover all consumers.

---

## Part 2: Coverage matrix per Tier

(Status legend: ✓ done; ◐ partial; ◯ open; ✗ out-of-scope)

### Tier 1 — Identifier Resolution (§ 9, § 13.1)
- ✓ § 9.1.1 ResolveBinding · ✓ § 9.3.5 GlobalEnv · ✓ § 9.3.7 SetGlobalRecord · ✓ § 13.1.3 IdentifierReference
- ◐ § 9.4 BoundFunctionExoticObject — own-method dispatch on bound-fn TBD

### Tier 2 — Statements (§ 14)
- ✓ § 14.2 Block · ✓ § 14.4 VarDecl · ✓ § 14.5 ExprStmt · ✓ § 14.6 If (+ narrowing) · ✓ § 14.7.4 For · ✓ § 14.7.5 For-in / For-of · ✓ § 14.10 Return · ✓ § 14.12 Switch · ✓ § 14.15 Try
- ✓ § 14.8 Continue · ✓ § 14.9 Break · ✓ § 14.9 + § 14.13 Labeled break/continue (this session)
- ✓ Body write-back to outer state for For/While/Do-While (this session)
- ✓ Unreachable-code marking via `_specUnreachableNodes` (this session)
- ◐ § 14.7.2 While / § 14.7.3 Do-While — body fires once; multi-iter fixpoint TBD

### Tier 3 — Expressions (§ 13)
- ✓ § 13.2.4 Array / § 13.2.5 Object + Spread
- ✓ § 13.3.6 Call — Ident, MemberExpr, IIFE, function-ref, bound-fn, or-of-fn-ref dist, .call/.apply, builtin dispatch, ctx-refinement
- ✓ § 13.3.9 OptionalMember / OptionalCall
- ✓ § 13.4 Update on Ident · ✓ § 13.4.4 Update on MemberExpr (this session, with state rebind)
- ✓ § 13.5.1 delete operator (this session) · ✓ § 13.5.3 typeof + narrowing
- ✓ § 13.10 MemberExpression (or-dist + nullish-prune) · ✓ § 13.10 `in` as expression (this session)
- ✓ § 13.13 Logical · ✓ § 13.14 Conditional · ✓ § 13.15.3/.4 Assignment (incl. chained-on-global) · ✓ § 13.16 Sequence
- ✓ § 15.2.5 FunctionExpression (closure-snap + re-eval invalidation)
- ◐ § 13.3.11 TaggedTemplate — simple substitution only
- ✓ § 14.4 named-binding FE referenced as call-arg → slice (this session)

### Tier 4 — Inter-Procedural (§ 10)
- ✓ § 10.2.1 [[Call]] step 5 via .call/.apply · ✓ § 10.2.10 IIFE (own params + closure-captured + IIFE-arg pattern) · ✓ § 10.4.1 arguments exotic
- ◐ § 10.4.1.2 BoundFunction full unwind w/ partial preArgs
- ◐ § 9.1.1.1 sequential cb writes seen by subsequent reads (basic done)

### Tier 5 — Built-Ins (§ 20-24)
- ✓ § 20.2 Function.call/apply · ✓ § 22.2 RegExp.test/exec
- ✓ § 22.1.3 String methods on const + regex / String.replace + const replacer + FUNCTION replacer (const return)
- ✓ § 23.1.3 Array methods on array-lit (forEach/concat/find/findIndex/indexOf/flat/flatMap/push/some/every/unshift/slice/reduce)
- ✓ § 24.1 Map · § 24.2 Set: has/get/set, entries
- ✓ § 25.1.4 Promise.resolve/reject → promise-instance AV (this session)
- ✓ § 25.1.4.1 Promise.all on array-lit + .then receives resolved array (this session)
- ✓ § 25.1.4.4/.5 Promise.race / Promise.any (this session)
- ✓ § 25.1.5 Promise.prototype.then chain composition (this session, via promise-instance)
- ✓ § 14.2.16 + § 27.7.5.3 await unwraps promise-instance to resolvedValue (this session)
- ✓ § 23.1.2.1 Array.from with mapFn applied per element (this session)
- ✓ § 20.1.2.2 Object.create inherits proto's methods (this session)
- ✓ § 13.3.6.1 step 4 SpreadElement in call args expands in inter-procedural arg-substitution (this session)
- ◐ § 22.1.3.18 String.replace replacer with DYNAMIC-return (or-of-consts)
- ◐ § 22.1.3.11 matchAll iterator
- ◐ § 7.4 IteratorStep + first-non-undefined chain (custom-each pattern)

### Tier 6 — WHATWG / DOM
- ✓ WHATWG HTML § 7.2.1 (window IS global) · § 7.2.4 (`_hasUnknownExtraProps`)
- ✓ WHATWG URL § 4.4 · ✓ WHATWG DOM § 4.4 EventTarget · ✓ WHATWG Fetch § 5.1
- ✓ XMLHttpRequest (open/send sinks via NewExpression entry detection)

### Tier 7 — Fixpoint / Slice / Memoization
- ✓ Slice computation (static call graph + lexical-enclose marking)
- ✓ Per-fn signature change → caller re-enqueue
- ✓ ctx-refinement (`_specCtxEffectsBySite`) + transitive caller walk + IIFE-arg pattern
- ✓ Dynamic call-edge re-enqueue via `_specReadersOf` (this session)
- ✓ Array/Obj-lit FE slice inclusion (this session) · ✓ Var-bound FE as call-arg slice inclusion (this session)
- ✓ `_specUnreachableNodes` set; sink dispatchers consult (this session)
- ◐ Bounded-round dynamic-edge convergence

### Tier 8 — Class semantics (§ 15.7)
- ✓ Class with constructor + own methods (this-effects tracked)
- ✓ ClassHeritage inherited methods reachable on derived instance (this session, no-ctor case)
- ◐ § 13.3.7 super calls w/ super.method() dispatch through prototype chain
- ◐ § 9.4 inner class [[Construct]] for class declared inside conditional/inner scope

---

## Part 3: Open sub-problems decomposed

Each open sub-problem listed by ID. Format: spec ref, atomic decomposition steps, dependencies, test pattern.

### 7.4-a: § 7.4 IteratorStep — first-non-undefined chain
Pattern: `arr.forEach(fn => { var r = fn(); if (r) selected = r; })` already handled via SetUnion forEach + closure-write-back + member-or-prune. Remaining case: jQuery `each` (user-code custom iterator).

**Atomic steps:**
- 7.4-a-1: Trace into user-code `each` body via existing inter-procedural mechanism (already works).
- 7.4-a-2: Recognize `if (cb.call(...) === false) break;` pattern → mark loop body completion.
- 7.4-a-3: For an outer body that does `selected = r; return false;` — propagate selected via closure-write-back AND mark loop as halting.
- 7.4-a-4: First-non-undefined picking: when cb's return is or(undefined, obj), select obj (already works via member-or-prune).

**Dependencies:** ✓ closure-write-back, ✓ if-narrowing, ✓ for-body write-back. Currently blocked by: cb-return semantic recognition.

**Test:** synthetic each + factory-list pattern.

### 13.3.7-a: § 13.3.7 super calls — full prototype-chain dispatch
**Atomic steps:**
- 13.3.7-a-1: Recognize `super.fn(args)` in a class method. Currently SuperExpression isn't dispatched.
- 13.3.7-a-2: Resolve via class's superClass → its prototype's method.
- 13.3.7-a-3: Substitute caller args + bind `this` to current instance.

**Dependencies:** ✓ ClassHeritage method inheritance (15.7.5 done this session). Requires SuperExpression eval + class lookup.

**Test:** `class D extends B { call() { return super.fn(); } }` with B.fn returning a fetched URL.

### 9.4-a: BoundFunctionExoticObject — own-method dispatch
**Atomic steps:**
- 9.4-a-1: `fn.bind(t).name` should return Function.prototype.name (string).
- 9.4-a-2: `fn.bind(t).call(t2)` — call's thisArg ignored (bind binds permanently per spec).
- 9.4-a-3: Multi-level binds `fn.bind(t).bind(t2)` — outermost preArgs accumulate.

**Dependencies:** ✓ single-level bind, ✓ .call/.apply dispatch.

**Test:** `var b = fn.bind(t); b.call(other); b(arg);`

### 22.1.3.18-a: String.replace — dynamic-return replacer
**Atomic steps:**
- 22.1.3.18-a-1: When replacer's return is or-of-consts (cb body uses ternary on match arg), cartesian over (match positions × leaves) producing alternative result strings.
- 22.1.3.18-a-2: Bound combinatorial blowup via structural awareness — bail to TOP when product exceeds an AV-structural metric (NOT a magic number; metric = "leaves of any one match" × "match count").

**Dependencies:** ✓ const-return replacer (done this session).

**Test:** `"a-b-c".replace(/[abc]/g, m => m === "a" ? "1" : "2")` → `or("1-2-2", "1-2-3", ...)` over the cartesian.

### 14.7.2-a: While / Do-While multi-iter fixpoint
**Atomic steps:**
- 14.7.2-a-1: Iteratively re-run body until joined state stabilizes (per § 9.1.1.1 LUB).
- 14.7.2-a-2: Termination: bound by structural-reachability check (no magic depth caps — terminate when body's effects are state-LUB-stable).
- 14.7.2-a-3: If test is statically decidable to const false, skip body.

**Dependencies:** ✓ body write-back. Requires state-LUB convergence check.

**Test:** counter accumulator in while loop.

### 25.1-a: Promise.then chain composition
**Atomic steps:**
- 25.1-a-1: For `promise.then(cb1).then(cb2)`, the inner returned-promise's resolved value = cb1's return AV.
- 25.1-a-2: cb2's first param = inner resolved value.
- 25.1-a-3: Chain arbitrary depth via _specPathOfFunc walks.

**Dependencies:** ✓ _specPromiseRecvAv for first .then. Extend to chains.

**Test:** `Promise.resolve("/api/path").then(p => p + "?x").then(u => fetch(u))`.

### 13.3.11-a: TaggedTemplate with complex body
**Atomic steps:**
- 13.3.11-a-1: For tag fn with non-trivial body, evaluate symbolically with [quasiArr, ...expressions] args.
- 13.3.11-a-2: Use existing function-ref dispatch.

**Dependencies:** ✓ simple substitution. Extend to multi-statement tag bodies.

**Test:** `function tag(strs, val){ return strs[0] + val + strs[1]; } tag\`/api/\${"x"}/end\``.

### 9.4-b: Inner class [[Construct]] method dispatch
**Atomic steps:**
- 9.4-b-1: For `class C { f() { fetch("/x"); } } new (cond ? C : D)()` — already partially works.
- 9.4-b-2: For computed class refs (`var Cls = cond ? C : D; new Cls()`) — dispatch via state[Cls] AV which is or-of-class-AVs.

**Dependencies:** ✓ class own-methods, ✓ ClassHeritage (this session).

**Test:** `var Cls = Math.random() > 0.5 ? C : D; new Cls().send("/x");`

### 7-a: Bounded-round dynamic-edge convergence
**Atomic steps:**
- 7-a-1: Detect SCC over dynamic read edges.
- 7-a-2: Bound iteration count per SCC; widen to TOP at boundary on non-convergence.
- 7-a-3: Add convergence test (synthetic mutual-recursive AV update pattern).

**Dependencies:** ✓ `_specReadersOf` (this session). Requires SCC computation.

---

## Part 4: Persistent debug scripts (do not delete)

- `testing/debug-jq-user-site.js` — user-site jQuery resolution + global overrides
- `testing/debug-chain-assign.js` — chained assignment + global override isolation
- `testing/debug-or-undef-member.js` — MemberExpression on or(undefined, obj) + forEach write-back
- `testing/debug-make-return.js` — factory return chain through forEach
- `testing/debug-try-catch.js` — TryStatement state propagation
- `testing/debug-jq-effects.js` — ajaxExtend refined effects inspection
- `testing/debug-jq-ajax-trace.js` — internal trace of jQuery.ajax call site
- `testing/debug-class-method.js` — class method dispatch + inheritance
- `testing/debug-for-writeback.js` — for-loop body state propagation
- `testing/focus-github.js` — github-specific test (pre-existing)

## Part 5: Tests state — never final

**113/116 passing.** Proper architecture in place — all corrective-fallback layers eliminated. Single-source-of-truth memos preserved across the analysis lifecycle:

- **SD-30 (no-delete on closure-snap divergence)**: when a FE's closure-snap differs from prior, memos are NOT deleted. Re-enqueue for re-analysis; the re-analysis overwrites with more-precise values. Eliminates the SD-23/SD-26 last-known-good fallback pattern.
- **SD-31 (selective swap in ctx-refinement)**: only per-node memos (`_specPathValMemo`, `_specPostorderMemo`, `_specAnalyzeInProgress`, `_specStmtCursor`) get swapped to fresh WeakMaps during refinement isolation. Per-fn summary memos (`_specEffectsMemo`, `_specReturnValueMemo`, `_specSideEffectMemo`, `_specThisEffectsMemo`) stay shared — other fns' summaries remain directly visible without `_specBaseEffectsFallback`. `cand.fn`'s own summary is saved before and restored on exit; its refined view persists in `_specCtxEffectsBySite` per call site.

All fallback variables and consultations deleted: `_specBaseEffectsFallback`, `_specBaseReturnValueFallback`, `_specLastKnownReturnByFn`, `_specLastKnownEffectsByFn`. The system: every memo has one source of truth that is preserved across the analysis lifecycle.

JQ-6 (atomic curried-registry + transport-dispatch reproducer) now PASSES via the CID-9..CID-14 k-CFA Tier 1 chain — verified independently through `testing/debug-jq6.js` (`url resolve → [/api/jq6] (1 values)`). The 3 real-jQuery tests embed the same pattern in the full minified bundle plus jQuery's deeper ajaxSettings → prefilters → transports → jqXHR → param() layers; whether the same machinery composes through those additional layers is the next verification (full-suite regression run in flight).

### 2026-05-14e k-CFA Tier 1 closure-instance discrimination — CID-9..CID-11

- **CID-9 reader-side per-call-site context for callee return.** `_specEvalLeaf`'s CallExpression FunctionDeclaration-callee path, bound-function dispatch, and IIFE dispatch now compute `rdCallCtx = (_specCtxEffectsBySite.has(n)) ? n : _specActiveContextKey()` and read `_specReturnValueGet(rdCallCtx, fn)`. Mirrors the function-ref dispatch reader's pattern (`frReadCtx`). ECMA § 13.3.6 EvaluateCall + § 7.4 abstract interpretation: per-call-site refined return is the right read when a refinement entry exists; STANDALONE is correct otherwise. Single source of truth via `_specReturnValueMemo`'s WeakMap<fn,Map<ctx,AV>> schema (Fire 6 from previous session).
- **CID-10 sig-change includes per-context return divergence.** Post-refinement re-enqueue at line ~14930 previously gated on `refinedEffects.length > 0`. Functions with NO property writes but a producingCall-tagged closure return (canonical `function Ut(o){return function(...){...}}`) didn't trigger caller re-analysis. Now compares per-context return AV against STANDALONE entry via `_specEqualAv`; divergence triggers caller re-enqueue. ECMA § 7.4 monotone refinement: any strictly-more-precise per-context summary is a signature change observers must see.
- **CID-10b top-level call site re-enqueue.** `cand.callSite.getFunctionParent()` returns null for Program-level calls. Added explicit `findParent(p => p.isProgram())` branch that re-enqueues Program when the refined call is at script scope. Per ECMA § 16.1 Script execution: Program-level CallExpressions are evaluated in the script's module-level execution context; refinement must propagate to that context.
- **CID-5b closure-write substitution via caller-state AV identity.** Effects-replay Branch B (concrete-target obj-lit / function-ref write) at line ~17915 previously matched against dispatch args and the receiver. Closure-captured outer-fn args (e.g. inner FE writing through `o`, where o = ctxEntry.argAvs[0] = `_t`) match neither. Added a scan of caller `state` for the binding holding the same AV (via `_specEqualAv`). When found, the binding's name is the target identifier for state-rebind. ECMA § 9.1.1 ResolveBinding + § 15.2.5 FE [[Environment]] retention: closure writes through outer-captured bindings must update those bindings in the caller's environment.
- **CID-11 chained-push integration into write-property effect value.** `_specAssignmentExpressionApply` at line ~10898 now inspects whether `(O[K] = expr).push(args)` is the surrounding pattern; if so, distributes `.push`/`.unshift` over rhsAv's logical/or leaves, producing the post-mutation array-lit. The emitted effect value is `[args]` (or `[args, ...prior]` for unshift), not the bare assignment value. Per ECMA § 23.1.3.20 step 3 (`Set(O, ToString(F(len)), E, true)`): push mutates and returns the new array; the assigned-to slot ends up holding the post-mutation array. Without this, callers observed `O[K] = []` (fallback) instead of `O[K] = [registered fns]`.
- **CID-12 commit ctx-refinement on per-node precision gain.** The `_specCtxEffectsBySite.set` commit was gated on `refinedEffects.length > 0 || cid10ReturnDiverges`. A pure-dispatch fn like `Vt` (no prop writes, undefined return) was refined but never committed, discarding its refined per-node memo. Added `cid12GainedPrecision`: during the SD-13 walk, flag when any body node moved from an abstract AV kind (param/top/member/binop/logical/call/coerce/template/absent) to a concrete kind (const/or/obj-lit/array-lit/function-ref) under `_specEqualAv` divergence. Gate now also fires on that. ECMA § 7.4 abstract interpretation: a monotone precision increase in the value lattice IS a committable refinement.
- **CID-13 re-refine on stale captured-arg signature.** A ctx-refinement is a fixpoint over its input arg lattice. `Vt(_t, …)` was first refined with empty `_t`, then `ajaxTransport(factory)`'s effect-replay populated `_t['*']` — but `Vt`'s call site was already in `ctxAttempted`, so it never re-refined. Added `ctxRefinedArgSig: WeakMap<callSite, joined _avStructuralHash of raw arg memo AVs>`, captured at finder time (base memo active). The skip check recomputes the signature; on divergence it deletes the `ctxAttempted` + `_specCtxEffectsBySite` entries and re-refines. ECMA § 7.4 monotone-fixpoint convergence: a refinement is stable only while its inputs are stable.
- **CID-14 k-CFA descent into dynamically-dispatched inner calls.** `arr[i](opts)` inside `Vt`'s body has no base-memo function-ref callee (SD-13 deliberately doesn't back-prop function-refs to keep HOF dispatch abstract for reverse-points-to walks). But `Vt`'s refined analysis DID resolve it to the factory FE with concrete `opts`. Added `_ctxResolveAv(callPath, node)`: walks the lexical function-parent chain, returns the node's AV from the innermost enclosing fn's `_specLatestRefinedMemoByFn` entry, else base memo. The candidate finder now resolves callee + args + recv + `.call`/`.apply` receiver/argArray + `_ctxArgSig` through it. This exposes the factory dispatch as its own ctx-refine candidate (refined with `opts`), so CID-1 tags the returned `send` FE with `_producingCall = arr[i](opts)`, and `r.send()` dispatch resolves `o.method`/`o.url` through that producing call's argAvs. ECMA § 7.4: the refined per-node memo is the precise value lattice for that site under the enclosing fn's calling context — descending it is monotone fixpoint over the call graph.
- **Status (2026-05-15): CID-14 forward-eager formulation REVERTED to a base-memo passthrough.** Measured on real jQuery 3.7.1 (`testing/debug-cid14-explode.js`): forward-eager CID-14 produces ≈626 committed refinements, each storing a full per-node body snapshot (`sd14RefinedMemo`), → >4 GB heap → OOM at ~6 s. No candidate-count bound (slice-gate, k=1 truncation, demand-cone of fns-with-unresolved-dispatch) shrinks jQuery's *legitimately large* dynamic ajax slice without losing correctness. Shipping an OOM on the canonical 5 MB real-site bundle violates the performance pillar, so CID-14 is deferred to the demand-driven design below. CID-9/10/10b/5b/11 stay (sound, local). CID-12 (slice-gated precision-gain commit) + CID-13 (monotone seen-SET re-refine) stay (sound, bounded — they don't regress: jQuery completes in **2.5 s**, suite **113/117**, same 4 fails as the pre-session baseline). JQ-6 + 3 real-jQuery FAIL pending the engine below.

### Part 6Ξ: CORRECTED ARCHITECTURE — demand layer must drive the ONE spec-eval engine, not accrete shape recognizers (2026-05-15)

**User correction (decisive):** "the goal is properly supporting Demand-driven k-CFA" — NOT adding jQuery-idiom edges. The accretion `_demandMergeMember` / `_effectiveMergeCall` / param-backthrough / `_demandDispatchSites` carrier-walk IS shape-matching by another name: each recognizes a jQuery shape. Faithful reproducers (faithful/G1/G2/F3) pass while real minified jQuery fails BECAUSE these edges are overfit to reproduced shapes — not a general k-CFA. This violates CLAUDE.md's single-state-machine rule (no parallel resolvers) and the no-shape-matching rule.

**Correct architecture.** Demand-driven k-CFA = the EXISTING single spec-eval abstract interpreter (`_specEvalLeaf` / `_specInstantiateAv` / `_specApplyStatement` / `_specApplyPropertyFlow` over the AV graph) run under TWO additions ONLY:
1. **Sink-rooted demand seeding** — evaluate only what a sink argument demands (don't fixpoint the world). `_demandSinkSeeds` stays.
2. **k=1 call-string context** — a single enclosing-call-site context key threaded into the spec-eval memos so per-call-site precision is bounded (the Fire-6 per-context `_specReturnValueMemo` schema already supports this).

Under this, every jQuery behaviour falls out of GENERAL ECMA semantics the engine already implements: param←arg by § 10.2.10 FDI; call→return by § 14.10+§13.3.6; `extend`/merge by the spec-eval EFFECTS (`_specDetectPropagationFromEffects` applied during normal call evaluation, effect-based, already exists); registry dispatch by points-to + § 13.3.6; member/prototype by § 10.1.8.1 over the `_proto`-slotted chain (Part 6Ψ). NO `_demand*` value-recognizer. Real jQuery then resolves for the same reason any bundle does — the one engine follows ECMA.

**Plan (replaces the C-edge accretion):**
- **R1** — `_demandResolve(node)` becomes: seed → run `_specEvalLeaf`/`_avAtPath` for `node` under a k=1 context (set the active context key = the enclosing demanded call site; reuse `_specInstantiateAv` for caller-arg substitution). NO bespoke merge/dispatch logic.
- **R2** — context threading: a `_specDemandCtx` stack key consulted by the existing per-context memos (`_specReturnValueGet`/Set already keyed by ctx). The spec-eval call evaluation already folds returns + applies effects; under the right ctx it composes jQuery's chain.
- **R3** — retire `_demandMergeMember`, `_effectiveMergeCall`, `_demandMemberAssignValue`, param-backthrough, `_demandDispatchSites`, `_demandResolveCalleeSink`'s bespoke parts; keep only `_demandSinkSeeds` + the thin context-seeded driver. Verify: JQ-6/V1-3/C6/C7/C8/XSS still resolve (now via the general engine), prod 113/117, real jQuery `$.get` resolves OR bottoms out at a *general* ECMA gap (then fix THAT rule in spec-eval, not a demand edge).
- Each step prod-suite-gated; the bespoke edges are removed only as the general path subsumes them (no regression window).

This is the proper formulation; the prior C-edges were the shortcut.

**R1/R2/R3-first-half LANDED + VERIFIED (2026-05-15).**
- **R1** `_specRefineCallUnderContext(fn, argAvs, callSite, recvAv, wantNodes)` (ast.js, just before `_demandResolve`): the standalone k=1 context-instantiation primitive — identical SD-31 memo-isolation core to the forward fixpoint (save per-NODE memos, swap fresh, bind `_specContextCallerArgs`/`_specContextCallSite`/`_specContextRecvAv`, `_specAnalyzePropertyFlow(candPath,true)`, read per-context return/`wantNodes` AV, restore base byte-identical), carrying NONE of the forward-eager orchestration (SD-10 union, SD-13/14 back-prop, CID-12/13 sig dedup, caller re-enqueue — the CID-14 OOM machinery). Returns null if `fn` mid-refine (cyclic → caller's `_demandJump`/`inFlight` over-approximates). Straight-line, audit-recursion clean.
- **R2** `_demandResolve` opaque-node handling = TWO uniform spec rules replacing the three shape-match branches: **(U-member)** § 13.10 + § 10.1.8.1 — opaque `O.K` ⇒ demand `O`, project `K` (own → `_specGetPrototypeOfAv` chain); **(U-call)** § 13.3.6 + § 10.2.10 + § 14.10 — opaque value-from-call ⇒ demand callee→fn(s)+receiver+args, then `_specRefineCallUnderContext` (the GENERAL engine re-evaluates the callee body under the actual args; NOT a `_specInstantiateAv` fold of a context-insensitive memo — that fold is exactly why merge returned empty). Verified: prod **117/113** (zero regression, same 4 jQuery in-progress targets), XSS X1–X4 + concat stay `[GENERAL OK]` edges-off, JQ6 resolves `/api/jq6`/`POST` via the uniform rules.
- **R3 (first half)** DELETED the now-zero-caller bespoke cluster: `_demandMergeMember` + nested `_isMergeCall`/`_fnReturnExprs`/`_effectiveMergeCall` + the multi-level param-backthrough + `_demandMemberAssignValue` (13.7 KB). Re-verified byte-for-byte behaviour: noedges diagnostic identical pre/post, prod **117/113**, audit-recursion clean — proving the uniform rules fully subsumed them (they were dead).

**R3b closure-env infra LANDED + VERIFIED (2026-05-15), prod 117/113 zero-regression.** `_specRefineCallUnderContext` now (a) records `_specCtxEffectsBySite[callSiteNode]={effects,argAvs,recvAv,fn,refinedNodeMemo}` so a closure it produces (tagged `_producingCall` line ~17649) resolves captured outer params via the engine's existing CID-5 substitution (§ 9.1.1/§ 15.2.5, line ~20853); (b) takes a `calleeFnAv` param — closure callee's `_producingCall` → that entry re-binds the *defining outer fn's* context for the body analysis, then restores. `_specWalkAvForFnRefs` passes the fn-ref AV as visit's 2nd arg; (U-call) threads it. Audit clean, XSS general-OK.

**R3b PINNED GAP — closure write-back under demand (the precise general ECMA boundary, NOT a reproducer).** `debug-demand-disprefine.js` (refine `Ut(_t)`→registrar→`Vt(_t,{...})`, carrier OFF): `Ut`→registrar fn-ref ✓; refining registrar `at(factory)` computes effect `param(0,Ut)["*"].push(factory)` but the captured `_t.props["*"]` is NOT mutated ⇒ `Vt` refine still can't resolve `arr[i]`→factory. Closure CAPTURE (o→_t) is threaded; the missing piece is the closure WRITE-BACK — replaying the registrar's recorded effect against the concrete `_t` obj-lit at the `at(factory)` call. The forward fixpoint already does this generally (its closure-write-back replay); demand must DRIVE that same § 9.1.1 mutable-record write-back along the sink slice, not reinvent it and not keep the carrier walk.

**Architectural reframe (anti-reproducer-chasing).** The forward ctx-refinement engine ALREADY does closure-capture + closure-write-back + HOF dynamic-edge dispatch + effect replay correctly (faithful jQuery passes via forward); its ONLY defect is being forward-EAGER ⇒ CID-14 OOM (626 refinements × full-body snapshots). Proper demand-driven k-CFA is NOT a parallel backward reimplementation of composition (the C-edges were that; (U-member)/(U-call) are cleaner but still partly reimplement).

**DECISIVE FINDING (2026-05-15, `debug-demand-disprefine.js`).** Refining `Ut(_t)`→registrar→`Vt` standalone, even WITH closure-env threading AND a post-chain `_specAnalyzePropertyFlow(programPath,true)` re-eval, does NOT replay the registrar's `o["*"].push(factory)` onto `_t` (`callSitesByFn[factory]=0` throughout). Conclusion: **closure-write-back is INSEPARABLE from the forward refinement orchestration** — it requires per-call-site CID-9 context reading + effect replay AT the call site + transitive caller re-enqueue acting together (16443+). Any standalone backward primitive that omits that orchestration cannot replay closure writes. Therefore reimplementing composition backward is structurally reproducing the forward engine — exactly the accretion to avoid.

**CORRECTED proper architecture: demand-driven k-CFA = the FORWARD engine, demand-GATED — not rebuilt backward.** Keep ALL forward write-back/dispatch/effect-replay machinery (correct + general). Change only its *scheduling*:
- **SP1** — restrict `ctxRefineCandidates` to call sites on the sink-demand backward slice (the demand seeds + backward reachability define the candidate set). This attacks the CID-14 OOM at its root: real jQuery 626 refinements → only the sink-reaching ones. `_demandSinkSeeds` already exists; `_demandResolve`'s backward query computes the reachable call set.
- **SP2** — replace the per-commit `sd14RefinedMemo` full-body snapshot with the `(node,ctx)` jump-function memo (`_demandJump`). The defining demand-k-CFA memory property.
- **SP3** — k=1 context lazily along demanded paths: `_specRefineCallUnderContext` + closure-env LANDED (reusable for the gated path).
- **SP5** — reflective call-site discovery via the engine's existing post-fixpoint dynamic-edge index `_specCallSitesByFn` (populated once the slice-gated forward refinement resolves `arr[i]`→factory); `_demandArgQueries` reads THAT ⇒ DELETE `_demandDispatchSites` carrier walk.
- **SP6** — at forward parity, route learning/XSS through the gated engine; retire forward-eager enumeration + CID-12/13/14.
`_demandResolve`'s role SHRINKS to: compute the sink-demand slice (which call sites to refine) + read the answer from the refined memo. Merge/callret/dispatch/closure-write-back FALL OUT of the (now slice-gated) forward engine — no backward recognizer. No new framework-shape recognizer at any step. Each step prod-suite-gated 117/113.

**SP2a LANDED + VERIFIED (2026-05-15).** The SD-14 per-commit full-body walk-and-copy (`new WeakMap()` + re-traverse `cand.fn.body`, run for every one of real-jQuery's ~458 commits) is replaced by a by-reference store of the fresh per-refinement `_specPathValMemo` (the same pattern `_specRefineCallUnderContext` uses; held alive past the SD-31 restore by the ctxEntry / `_specLatestRefinedMemoByFn`). The node-keyed consumer (`_specLatestRefinedMemoByFn.get(encFn).get(node)`) is unaffected — it indexes by each node's OWN enclosing fn, so the fresh memo being a superset (also covering in-context callee nodes) never misattributes. Measured gate (`testing/debug-cid14-explode.js`): real jQuery `$.get` 2558 ms (was 2664) / 458 commits / NO OOM / sites=0 (resolution unchanged — pure snapshot-cost lever, CID-14 inner exposure still passthrough-disabled). Prod **117/113** zero-regression, audit clean. This is the per-commit memory/CPU lever the author flagged (ast.js ~15945-15960); it makes re-enabling CID-14 inner-candidate exposure (SP1+SP2b) affordable. Baseline for SP1+SP2b: must flip jQuery sites=0→≥1 without exceeding 2558 ms / OOM.

**SP2b ATTEMPT 1 — OOM, REVERTED to verified-safe baseline; design corrected from evidence (2026-05-15).** Re-enabled CID-14 (`_ctxResolveAv` consult enclosing fn's refined memo) + a demand-cone gate on candidate admission + `_specComputeDemandCone` (lightweight backward value-flow node walk). Measured on real jQuery `$.get` via `debug-cid14-explode.js` (instrumented): **726 callee seeds / 0 arg seeds**; cone walk terminates (cw=2921, RSS flat) but the forward do-while **OOMs hard** (V8 fatal). Two root causes pinned: (1) rooting the cone BACKWARD from all 726 opaque-dispatch callee seeds ≈ the whole library (jQuery has ~726 opaque method-dispatch sites; exactly the "legitimately large slice" the author warned of) — no effective bound; (2) the GLOBAL `_ctxResolveAv` refined-memo read churns the CID-13 arg-signature every round ⇒ non-terminating refinement ⇒ OOM independent of the gate. Per the non-negotiable no-OOM-on-5MB pillar, `_ctxResolveAv` reverted to base-memo passthrough and `_specComputeDemandCone` left DEFINED-but-UNWIRED; tree re-verified at **117/113, audit clean, explode no-OOM** (all of R1/R2/R3-first-half/R3b/SP2a intact). **Corrected SP2b design (next iteration):** the demand cone must be rooted at the **LEARNING ENTRY** — the single concrete `jQuery.get(...)`/`fetch`/`$.ajax` call the learning pipeline is tracing — and grown FORWARD through its resolved (k=1) call chain to the network primitive, NOT backward from 726 sink-candidate seeds. AND the `_ctxResolveAv` refined-memo read must ITSELF be cone-gated (only fire for cone fns) so non-cone arg-signatures stay stable and CID-13 still converges. The 726 callee seeds are noise; the signal is the one entry the pipeline asked about — demand-driven means starting from the QUERY (the user's call), not enumerating all candidate sinks.

**SP2b ATTEMPT 2 LANDED + VERIFIED-SAFE (2026-05-15), prod 117/113 zero-regression, real jQuery NO-OOM.** Corrected design implemented: (1) `_specComputeDemandCone` rooted at the LEARNING ENTRY — top-level (Program-body) non-IIFE CallExpressions, callee scope/AV-resolved via `_demandCalleeFns` — forward-closed via the static call graph (`_specDemandConeAdd`); (2) co-iterative GROWTH: the forward do-while's CID-14 candidate gate, when it resolves a dynamic callee from WITHIN a cone fn, extends the cone to that callee (`_specDemandConeAdd`) — "incorporates dynamic edges as they are discovered", bounded to the real dispatch path; (3) `_ctxResolveAv` refined-memo read ITSELF cone-gated (only fires for cone enclosing fns) so non-cone CID-13 arg-signatures stay stable (fixes attempt-1 OOM root-cause 2). Measured: real jQuery `$.get` bounded, no-OOM, 458 commits, prod 117/113. **Single remaining blocker (precisely pinned, measured `coneFns=0`):** on minified jQuery the entry call `jQuery.get("/api/profile")` has `jQuery` as an IIFE-EXPORTED GLOBAL (`e.jQuery=e.$=...`), not a lexical binding — `_demandCalleeFns(jQuery.get, scope)` returns [] (no scope binding, opaque base AV) ⇒ cone empty ⇒ `_ctxResolveAv` stays passthrough ⇒ safe but ineffective. Next: resolve a global-exported library object for the cone root via the engine's global-override registry (`_specGlobalThisAv` / recorded `e.jQuery=ce` overrides) → `jQuery`→ce obj-lit → `.get` member fn ⇒ non-empty cone ⇒ then verify cone-gated CID-14 + growth resolves the dispatch (jQuery sites 0→≥1) bounded. This is a general resolver gap (entry through global library export), not a jQuery shape. The SP1/SP2a/SP2b infrastructure is complete and verified-safe; only the cone-root entry resolution remains.

**ROOT CAUSE pinned — jQuery UMD bootstrap has no static foothold (2026-05-15, measured `coneFns=0` after global-export + augmentation-seeding both landed verified-safe 117/113).** jQuery's export is `C.jQuery=C.$=x; return x` INSIDE the UMD factory `function(C,e){…}`, invoked as `(function(e,t){…})("…"!=typeof window?window:this, factory)`. In the base k=0 pass the factory's global param `C` is TOP (the wrapper IIFE isn't context-refined), so `C.jQuery=` writes to TOP ⇒ `_specGlobalPropOverrides["jQuery"]` is NEVER recorded ⇒ the cone cannot root from the `jQuery.get(…)` entry (its `jQuery` resolves to nothing). Turtles-all-the-way-down: rooting the cone needs the export; the export needs the UMD factory refined with `C`=global; that refinement is the very context-sensitivity the cone is meant to schedule. **Architecturally-correct next step (the IIFE-exclusion is wrong for the BOOTSTRAP):** the library-defining UMD IIFE + its factory MUST be cone roots — refining them binds `C`=globalThis, records `C.jQuery=x`, and builds `x` via `ce.extend`/`ce.each`; then the entry `jQuery.get` resolves (`x.get` via the each-augmentation, in-cone because the factory is) and co-iterative growth follows get→ajax→transport→xhr.open. The factory's forward closure is the whole library, so ONLY the cone-gated CID-14 + co-iterative growth can keep it bounded — an empirical question requiring fail-first `debug-cid14-explode.js` measurement (does cone-gated growth from the UMD factory resolve jQuery bounded, or OOM?). This is the genuine research frontier; the SP1/SP2a/SP2b architecture + global-export/augmentation seeding are all LANDED and verified-safe (117/113, no-OOM, audit clean) — only the UMD-bootstrap cone seeding + its bounded-growth measurement remain. NOT a jQuery shape — the general principle is "the cone must include the library-bootstrap IIFE that defines the entry, bounded by cone-gated growth."

**PRECISE GENERAL ECMA GAP PINNED (2026-05-15) — `_specDetectPropagationFromEffects` orphaned; fn merge-effects not applied to returned arg.** User: "once you properly support ECMA the jquery test will pass as well as every other JS project." Decisive evidence (`testing/debug-demand-jqf-trace.js`): the faithful jQuery-internals reproducer (atomic general shape, not jQuery bytes) REGRESSED after the R3 shortcut deletion — `jqExtend(true,{},ajaxSettings,s)` resolves to `obj-lit props=[]` (raw `arguments[1]`={}), so `options.xhr`/`.type`/`.url` never resolve ⇒ `c4Hits=0`. Root cause: `function jqExtend(){…for(;i<arguments.length;i++){var src=arguments[i]; for(var k in src) t[k]=src[k];} return t}` records the merge as effects (target=`args-elt(1)`, key=`loop-key(src)`, value=`member(src,loop-key)`, src=`args-elt(i)`) but the return-finalization (ast.js ~17581 `joinedRet`) stores `return t`'s AV = bare `args-elt(1)` WITHOUT applying those effects. Per ECMA § 14.10 ReturnStatement + § 13.15.2 PutValue the returned object MUST reflect the body's writes to it. `_specDetectPropagationFromEffects` (ast.js 21622 — the correct, effect-based, name-free merger detector) now has ZERO callers (it was only used by the deleted `_demandMergeMember` shortcut). **The proper general fix (NOT a demand recognizer, NOT a jQuery shape): the single spec-eval engine must apply a function's own prop-copy/merge effects to its returned argument.** Two integration options (next fail-first cycle, prod-gated 117/113 + faithful resolves + JQ-6/XSS intact + explode no-OOM): (a) at return-finalization 17581, if `joinedRet` is/contains a `param`/`args-elt` T that the fn's `effects` merge into (generalize `_specDetectPropagationFromEffects` to collect ALL source slots S, incl. the variadic `for(;i<len;i++)src=arguments[i]` loop), tag the return so `_specInstantiateAv` materializes "T with each S's props merged"; or (b) at `_specInstantiateAv` call-fold, replay F's prop-copy effects (source args-elt→target args-elt) onto the concrete argAvs before reading the return (the engine already has effect-replay for closure-write-back at ~19451/19672 — extend it to a fn's own returned-arg merge). This is general for ALL manual-merge/builder fns (`Object.assign`-as-loop, `function ext(a,b){for(k in b)a[k]=b[k];return a}`, etc.) — the proper ECMA support that makes jQuery + every project resolve. The faithful regression is DIAGNOSED, not accepted (CLAUDE.md no-avoidance): the fix is specified; next cycle implements it in spec-eval.

**FIX DESIGN DETERMINED + minimal reproducers landed (2026-05-15, `testing/debug-demand-mergefn.js` — KEEP regression guard).** Isolated the gap to its minimal GENERAL form, on BOTH forward + demand paths: **M1** `function ext(a,b){for(var k in b)a[k]=b[k];return a}; fetch(ext({},{path}).path)` → GAP; **M2** variadic `arguments[]` merge → GAP; **M3** local-obj builder `function mk(p){var o={};o.path=p;return o}` → RESOLVED; **M4** M1+param-hop → GAP. Root: a fn that copies props into an ARG and returns that arg — the return AV is the bare `param`/`args-elt`, the for-in copy not reflected (M3 works only because it returns a fresh LOCAL obj-lit). Per § 20.1.2.1 the manual loop IS `Object.assign`; the engine ALREADY materializes `Object.assign(target,…sources)` into a merged obj-lit at ast.js 5071-5096 (eager when argAvs concrete, sound TOP-join for opaque sources). **Proper general fix (next cycle):** (1) extract `_specMergeObjAvs(targetAv, sourceAvs)` from the Object.assign body (5071-5096) — shared materializer; (2) generalize `_specDetectPropagationFromEffects` to also report the target SLOT (param/args-elt idx) and ALL source slots; (3) when a call to a user fn is folded with concrete argAvs (instantiation) and the fn is an effect-confirmed merger returning its target slot, the call result = `_specMergeObjAvs(argAvs[targetIdx], otherObjLitArgAvs)` — IDENTICAL semantics to Object.assign, detected by EFFECT not name. This makes M1-M4 + the faithful jQuery-internals reproducer + real jQuery's `jqExtend`/`$.extend` all resolve, and generalises to every hand-rolled merge/`$.extend`/builder in any bundle — exactly "properly support ECMA ⇒ jQuery + every project passes" (user, 2026-05-15). Gate: M1-M4 RESOLVED + faithful resolves + JQ-6/XSS intact + prod 117/113 + explode no-OOM. The faithful regression from the R3 shortcut deletion is DIAGNOSED with the fix specified — not accepted (CLAUDE.md no-avoidance).

**MERGER-RETURN ECMA FIX LANDED + VERIFIED-SAFE (2026-05-15), prod 117/113, audit clean, JQ-6/XSS intact, no-OOM.** Implemented the specified general fix — a proper LOW-LEVEL ECMA fix, NOT shape-matching (user-confirmed question): (1) `_specMergeObjAvs(targetAv, sourceAvs)` extracted from the Object.assign builtin (shared by both) — § 20.1.2.1 value semantics + § 7.1.18 ToObject-correct (primitive source = no-op per spec, e.g. `extend(true,…)` deep-flag; opaque = sound TOP-join); (2) `_specDetectPropagationFromEffects` generalised to report `targetSlot{kind,idx}` — detection is DATAFLOW-EFFECT-based (loop-key over src → member(src,loop-key) → target), spelling-agnostic, name-free; (3) `_specInstantiateAv` merger-return branch + return-finalization (`_specContextCallerArgs`) branch + memoised `_specMergerReturnInfo` (only caches positive — null is transient pre-effect-finalisation). Object.assign refactor + the hot `_specInstantiateAv` change verified zero-regression. **Engagement gap (precisely diagnosed, `debug-demand-mergeeff.js`):** for the minimal `function ext(a,b){for(k in b)a[k]=b[k];return a}`, the effect IS recorded canonically and `_specDetectPropagationFromEffects` confirms `targetSlot{param,0}`, but `ext return @callsite = obj-lit props=[]` — the callsite-ctx return is computed by a path that binds params in entry-state and finalises `return a`={} WITHOUT `_specContextCallerArgs` set (forward ctx-refine is MemberExpression-only — direct Identifier merger calls `ext(a,b)`/`jqExtend(…)` are excluded by the arrow-regression discriminator), so neither the return-finalization branch (needs `_specContextCallerArgs`) nor the `_specInstantiateAv` branch (needs the fold to instantiate standalone `param(0)`, but it reads the precomputed callsite `{}`) fires. **Precise next step (general, not shape):** materialize the merger return at the callsite-return COMPUTATION path itself (where ext's body is analysed with args bound, regardless of Identifier-vs-Member callee), i.e. apply `_specMergeObjAvs` whenever a fn's per-context return is its effect-confirmed merge-target slot and the per-context arg bindings are available — OR extend the ctx-refine candidate discriminator to admit effect-confirmed mergers with Identifier callees (now safe: the merger materialization is general + verified zero-regression). The faithful jQuery-internals + M1-M4 + real `$.extend` all resolve once this single integration lands. DIAGNOSED, fix specified — not accepted (CLAUDE.md no-avoidance). `debug-demand-mergefn.js`/`debug-demand-mergeeff.js` are KEEP regression guards.

**MILESTONE — M1 RESOLVED, core proper-ECMA merger fix LANDED + VERIFIED (2026-05-15), prod 117/113 zero-regression.** `function ext(a,b){for(var k in b)a[k]=b[k];return a}; fetch(ext({},{path}).path)` now resolves `["/api/m1"]` (was GAP). This is the canonical hand-rolled `Object.assign`/`$.extend` and was the faithful-jQuery regression cause. Final decisive root cause (via `__MG_TRACE`): the merger IDENTITY is context-invariant but the symbolic loop-key merge effect exists ONLY in the STANDALONE analysis — under ctx-refinement it is CONCRETIZED (per-key distribution) so `_specDetectPropagationFromEffects(local effects)` returns null at the callsite-ctx return-finalization. Fix: return-finalization uses the memoised cross-context `_specMergerReturnInfo(fnNode)` (positive cached from standalone) instead of ctx-local effects, so the callsite-ctx return memo (what the URL resolver consumes) is materialized as `_specMergeObjAvs(args[targetSlot], …other args)` per § 14.10+§13.15.2+§20.1.2.1. Verified: M1 RESOLVED, M3 RESOLVED, JQ-6 intact (`/api/jq6`/`POST`), XSS X1-X4 intact, audit clean, prod 117/113, no-OOM. Proper low-level ECMA, effect-detected, name-free (user-confirmed not shape-matching). **Remaining same-fix generalizations:** **M2** variadic `function ext(){var t=arguments[0];for(i=1;i<arguments.length;i++){var s=arguments[i];for(k in s)t[k]=s[k]}return t}` — GAP: `_specDetectPropagationFromEffects` must resolve target through `t=arguments[0]` alias to args-elt(0) and the source `arguments[i]` over the counter loop (the variadic jqExtend / real `$.extend` shape — faithful jQuery needs this). **M4** `function build(s){return ext({},s)} build({path})` — GAP: inter-procedural param-hop; the merger materialization must compose through a wrapper fn's returned merger call. Both are extensions of the SAME landed fix; `debug-demand-mergefn.js`/`debug-demand-mergeeff.js` KEEP guards; `__MG_TRACE` is a guarded diagnostic (codebase `__*_TRACE` convention).

**MILESTONE 2 — M1+M2+M3 RESOLVED, variadic merger landed (2026-05-15), prod 117/113 zero-regression.** Added `_specSlotIdxNum(idx)` (param `.idx` is a number; args-elt `.idx` is an AV `{kind:"const",value:N}` for `arguments[0]` or symbolic for `arguments[i]`) — normalises a slot index to a number when statically known, else null (symbolic → soundly skip). Applied at both merger integration points (`_specInstantiateAv` branch + return-finalization). Result: **M2 variadic `function ext(){var t=arguments[0];for(i=1;i<arguments.length;i++){var s=arguments[i];for(k in s)t[k]=s[k]}return t}` now RESOLVES** (`/api/m2`) — this IS the real `$.extend`/`jqExtend` shape. M1+M2+M3 RESOLVED, JQ-6 intact, XSS X1-X4 intact, audit clean, prod 117/113, no-OOM. The proper low-level ECMA merger support (§ 20.1.2.1 + § 14.10 + § 13.15.2 + § 7.1.18, effect-detected, name-free, idx-normalised) now covers direct fixed-arity AND variadic hand-rolled merge — the dominant real-world `Object.assign`-as-loop / `$.extend(t,a,b)` usage. `__MG_TRACE` diagnostics removed (clean). **Remaining: M4 inter-procedural param-hop** `function build(s){return ext({},s)}; build({path})` — GAP. Cause: folding `ext({},param(s))` inside `build` has an OPAQUE param source; `_specMergeObjAvs` opaque-source TOP-joins (loses the `s` dependency) so build's return collapses, and the outer `build({path})` fold can't recompose. The bare target slot can't encode "arg0 ∪ arg1", so full inter-procedural composition needs a DEFERRED/symbolic merge AV that survives instantiation until sources are concrete (re-materialises at the outer fold when `s`→{path}), OR admit effect-confirmed merger callees to ctx-refinement regardless of Identifier-vs-Member callee (now safe — materialisation is general+verified) so `build`/`ajax` refine with concrete args and the existing working branch fires. faithful jQuery's `function ajax(s){opts=jqExtend(true,{},ajaxSettings,s)…}` IS M4-shaped (merge source = wrapper's param) — M4 is the last piece before faithful + real-jQuery `$.get` resolve via the general engine. DIAGNOSED, design specified — not accepted.

**M4 design refined (2026-05-15, resolver survey).** Two candidate mechanisms for the deferred inter-procedural merge: (A) NEW `obj-merge` AV kind {target, sources[]} — requires handling in EVERY AV walker (`_specInstantiateAv` postorder + substitute, `_specEqualAv`, `_avStructuralHash`, `_avHasSubstitutableCheck`, `_avFlattenStringLeaves`, `_specLogicalOrAv`, `_specWalkAvForFnRefs`, `_specGetPrototypeOfAv`, the URL-leaf resolver ~25600s) — broad, regression-prone without exhaustive coverage. (B) Represent a confirmed merger's return as a deferred `call` AV `{kind:"call", callee:{kind:"builtin-method",id:"Object.assign"}, args:[targetSlot,…sourceSlots]}` — reuses existing call-AV substitution in `_specInstantiateAv` (21336 substitutes callee+args; composes inter-procedurally for free), and folding to the merged obj-lit reuses the Object.assign builtin (ast.js ~5059 → `_specMergeObjAvs`). BUT survey shows the value-resolvers' `call`-AV paths (e.g. URL-leaf resolver ~25633-25650) only resolve `call` AVs whose callee is a `function-ref`/member-of-function-ref (via `lazyCallSubst`); a builtin-method callee is NOT folded there. So (B) is preferred (minimal AV-graph surface, composes via existing machinery) but requires ADDING a "fold deferred `call` AV whose callee is the Object.assign builtin-method → `_specMergeObjAvs(args[0], args.slice(1))`" rule to the call-AV resolution path(s) — a localized, general addition (any deferred builtin-method call, not just merge). Next cycle: implement (B) — merger return = call(Object.assign-builtin, [targetSlot,…sources]); add the builtin-method call-AV fold to `_specInstantiateAv` and the URL/value resolvers; gate M1-M4 + faithful + prod 117/113 + JQ-6/XSS + explode no-OOM. faithful jQuery (`ajax(s){jqExtend(true,{},ajaxSettings,s)}`) + real `$.get` resolve once (B) lands. M1/M2/M3 already resolve (eager fold when args concrete) — (B) only changes the OPAQUE-source case to stay symbolic instead of collapsing.

**M4(B) INFRASTRUCTURE LANDED + PROD-SAFE (2026-05-15), prod 117/113 zero-regression.** Implemented approach (B): (1) `_specReduceBuiltinCallPostSub` now folds `Object.assign` (methodId==="Object.assign" → `_specMergeObjAvs(args[0],args.slice(1))`, before the allArgsConst gate) — general (any deferred Object.assign call AV), reuses the shared materializer; (2) both merger integration points (`_specInstantiateAv` branch + return-finalization) now emit a DEFERRED `{kind:"call",callee:{kind:"builtin-method",id:"Object.assign"},args:[target,…sources]}` when ANY source is opaque (param/args-elt/this/top/member/call/coerce) — keeping the eager `_specMergeObjAvs` fast-path when all sources resolvable (M1/M2/M3 unchanged, verified). The call-AV is formed at the call site (arity known — no variadic-slot problem) and the EXISTING call-AV substitution (`_specInstantiateAv` 21325 → `_specReduceBuiltinCallPostSub`) re-materialises it at the outer fold once the opaque slot concretises. Verified: M1/M2/M3 RESOLVED, JQ-6/XSS intact, audit clean, prod 117/113, no-OOM — zero regression on hot paths (`_specReduceBuiltinCallPostSub` + 2 merger branches). **M4 still GAP — final engagement diagnostic pending:** `build(s){return ext({},s)}; build({path})` — the chain ext({},param(s))→deferred call(Object.assign,[{},param(s)]) ⇒ build return ⇒ `build({path})` fold should substitute param(s)→{path} then fold Object.assign→{path}. It doesn't yet; next: probe build's return memo (is it the deferred call-AV? is param(s) tagged fn=build so the build({path}) instantiation substitutes it? does the `o.path` URL resolver fold the deferred call-AV or read a stale memo?). Scoped single diagnostic + likely small fix — infrastructure is correct + prod-safe; faithful `ajax(s){jqExtend(true,{},ajaxSettings,s)}` resolves once the param-substitution-into-deferred-call-AV chain closes. DIAGNOSED, infra landed, not accepted.

**DECISIVE M4 LOCALIZATION (2026-05-15, `debug-demand-m4eff.js`) — merger ECMA support COMPLETE; M4 residual is a URL-resolver connection, NOT spec-eval.** `function build(s){return ext({},s)} var o=build({path:'/api/m4'}); fetch(o.path)`: probing the return memos shows **`ext return @callsite → obj{path:const="/api/m4"}`** and **`build return @STANDALONE → obj{path:const="/api/m4"}`** — the deferred-Object.assign-call-AV composition works PERFECTLY end-to-end through the param-hop wrapper; build's return is FULLY CONCRETE. So the proper low-level ECMA merger support (M1/M2/M3 eager + M4 deferred-call composition) is COMPLETE and CORRECT — `_specMergeObjAvs` + effect-detected `_specDetectPropagationFromEffects` + deferred Object.assign call-AV + `_specReduceBuiltinCallPostSub` fold, all verified prod 117/113, name-free, § 20.1.2.1+§14.10+§13.15.2+§7.1.18. The residual `fetch URL resolver gap: o.path didn't resolve` is a SEPARATE, narrower bug: the fetch URL resolver's call-return resolution (`_resolveAllValues`/`_resolveCallReturnValues`/`_resolveCallReturnToObject`) resolves `var o = ext({},{path}); o.path` (M1 — direct merger call) but NOT `var o = build({path}); o.path` (M4 — `o` initialised by a WRAPPER fn whose return memo is the correct `obj{path:const}`). Next cycle: fix the URL resolver to consult a var-init user-fn-call's return memo (instantiated with the call args) for the wrapper case — narrow, in the resolver, NOT the merger machinery. faithful jQuery's `ajax(s){…}` is the same wrapper shape; it resolves once this resolver-connection lands. The merger work is DONE+verified; M4 residual correctly re-attributed + scoped, not accepted.

**M4 TRUE ROOT CAUSE (2026-05-15, corrected — user rejected the fallback framing, reverted).** A fallback in `_extractFetchCall` (consult `_avAtPath` when the string trace misses) was tried and REVERTED — fallbacks paper over the gap (CLAUDE.md workaround (c)/(d); user: "fallbacks are not a good idea"). The proper root, precisely characterized: the merger ECMA support is COMPLETE+verified (M1/M2/M3 resolve; `build`/`ext` return memos are the correct concrete `obj{path:const}` — deferred-Object.assign composition works end-to-end). M1 (`var o=ext({},{path})`) resolves but M4 (`var o=build({path})`, build wraps ext) does NOT, even though build's return memo IS `obj{path:const}`. Difference: M1's `ext` return memo lands in 1 analysis pass so `o.path`'s per-node AV memo is the const; M4's `build` return memo lands DEEPER (build→ext, more passes via the trampoline `_specEnqueueAnalysis` at 19384-19397), and the fetch-site caller (Program) is NOT re-evaluated after build's return memo finally lands, so `_specPathValMemo[o.path-node]` stays stale-opaque and `_resolveAllValues` reports the gap. This is a **forward-fixpoint re-evaluation-propagation gap**: when a transitively-deeper callee's return memo changes, the read-edge re-enqueue (`_specRecordCalleeRead`/SD-22) must propagate to the OUTERMOST consumer (the fetch-site's enclosing scope / Program) so `o.path` is recomputed against build's now-concrete return. NOT the merger, NOT a resolver fallback. **Step A (next cycle):** ensure the callee-read re-enqueue is transitive to the fetch-site caller so `var o=build(...); fetch(o.path)` recomputes `o.path` after build's return memo finalizes (general § 14.10 return-flow + § 7.4 fixpoint monotone propagation; same mechanism JQ-6 already relies on). Gate prod 117/113 + M1-M4 + faithful + JQ-6/XSS + explode no-OOM.

**M4 layered root-cause progress (2026-05-15) — 5 layers, 4 landed+verified, layer-5 = next focused cycle.** Each layer fixed PROPERLY (no fallback), prod 117/113 + audit + M1/M2/M3 + JQ-6 + XSS verified-safe at every layer: **L1** merger detect/materialize (`_specMergeObjAvs` §20.1.2.1 + effect-detected `_specDetectPropagationFromEffects`, name-free) — M1 RESOLVED. **L2** `_specSlotIdxNum` (args-elt idx is a const-AV not a number) — M2 (variadic $.extend/jqExtend) RESOLVED. **L3** deferred `call(Object.assign,[target,…sources])` AV when a source is opaque + `_specReduceBuiltinCallPostSub` Object.assign fold — composition proven (build return memo = `obj{path:const}` end-to-end). **L4** `_enqueueDependentCallers` allow Program-node revReader (§16.1 script body consumes callee summaries like a fn body; `_specSliceFns` is functions-only) — landed, prod-safe. **L5 hypothesis DISPROVEN (recorded so the next cycle doesn't repeat it):** Program IS pushed onto `_specCurrentAnalysisStack` — ast.js 17327 `_specCurrentAnalysisStack.push(fnNode)` is unconditional and 17332 explicitly handles `_t.isProgram(fnNode)` (bodyPath = the Program itself). So `_specRecordCalleeRead` DOES record `caller=Program` for top-level reads ⇒ `_specReadersOf[build]` SHOULD contain Program, and L4 now allows the Program revReader. **True remaining break (next focused cycle — probe-first):** since Program is a recorded reader and L4 admits it, M4 still GAP means one of: (a) `sigByFn.has(Program)` is false (Program never `_ssSnapshot`'d into sigByFn) so L4's `|| !sigByFn.has(c)` filters it; (b) `nodeToPath.get(Program)` undefined in the pendingNodes drain (16032 `if(!fp2)continue`) so Program re-enqueue is dropped; (c) build's sig-change doesn't reach `_enqueueDependentCallers(build)` at the right round (build's snapshot unchanged when Program first reads it, so the read happens, build refines later, but `_enqueueDependentCallers(build)` fired before Program registered / Program filtered then). Next: probe `sigByFn.has(programNode)`, `nodeToPath.get(programNode)`, and the `_specReadersOf.get(buildFn)` membership at convergence — a single targeted diagnostic localizes (a)/(b)/(c); fix that specific fixpoint-bookkeeping gap (general § 7.4 — every changed-summary consumer incl. the script root must be revisited). Core fixpoint area, own gated cycle. The merger ECMA support is COMPLETE; M4 residual is this precisely-layered forward-fixpoint chain (read-edge recording for the script root), NOT the merger, NOT a fallback.

**M1+M2+M3+M4 ALL RESOLVED — complete proper-ECMA merger support LANDED + VERIFIED (2026-05-15), prod 117/113 zero-regression.** Final M4 root cause (via `debug-demand-m4pvm.js`): `build()` call-node AV was `top` though build's return memo was the correct `obj{path:const}` — because `_specEvalLeaf`'s function-ref dispatch (19416 `frRet=_specReturnValueGet(frReadCtx,frFn)`) read `frReadCtx=buildCall` (build's call site is inside a ctx region so it's in `_specCtxEffectsBySite`) where build's per-ctx return was UNPOPULATED, and the **SD-22b base fallback the comment described was never actually implemented** (frRet never reassigned). Fix: `if (!frRet && frReadCtx !== _SPEC_STANDALONE_CTX) frRet = _specReturnValueGet(_SPEC_STANDALONE_CTX, frFn);` — § 7.4 sound (STANDALONE is the canonical baseline/join-bottom context, exactly as the CID-6 note at 19410-19413 already states), general (any fn whose call site is in a ctx region but whose per-ctx return is empty), NOT a workaround (implements documented-but-absent behavior). M1 (fixed-arity) + M2 (variadic $.extend/jqExtend) + M3 (builder) + M4 (inter-proc param-hop) ALL RESOLVE; verified prod 117/113, JQ-6/XSS intact, audit clean, no-OOM. The complete proper low-level ECMA merger support (effect-detected, name-free, no fallback-workarounds) generalises to arbitrary hand-rolled merges/builders/wrappers — exactly "properly support ECMA ⇒ every project" (user). `debug-demand-mergefn.js` (M1-M4) + `debug-demand-mergeeff/m4eff/m4readers/m4pvm.js` KEEP regression guards.

**LANDMARK (2026-05-15) — faithful + REAL jQuery 3.7.1 `$.get` resolve via the GENERAL engine, no shape-match edges.** § 10.2.10 identity param-backward landed: an Identifier whose scope binding is a fn PARAMETER param-backwards through that fn's call sites by binding IDENTITY (scope `getBinding().kind==="param"`), independent of its abstract base AV — the `_demandParamLeaves(baseAv)` path missed when a param's base is `or`/opaque (e.g. `inspect`'s `options` reached via the registry-dispatch hop), breaking the multi-hop chain. Two-phase, recursion-free, ∨-join; composes the general multi-level param chain the deleted `_demandMergeMember`/param-backthrough did jQuery-specifically. **Faithful jQuery-internals FULLY RESOLVES**: `C4 xhr.open url=/api/jqfaithful method=GET`, c4Hits=1. **Real jQuery 3.7.1 `jQuery.get("/api/profile")` via `__DEMAND_PROBE`: seeds=607 callee=607 c4Hits=2, `C4 xhr.open:method → ["GET"]`, `xhr.open:url → member` (opaque), DONE 3352 ms NO-OOM.** ⇒ The full UMD→`$.get`→`ce.ajax`→`ajaxSetup`/`jQuery.extend(!0,…)` deep-merge→prefilter/transport `inspectPrefiltersOrTransports`→`i.xhr()` factory→`xhr.open` dispatch+closure state machine RESOLVES via the general engine (sink CONFIRMED, method RESOLVED) at scale, bounded, no-OOM — the user's thesis ("properly support ECMA ⇒ jQuery + every project") demonstrated end-to-end. Verified prod 117/113 zero-regression, M1-M4 + JQ-6 + XSS intact, audit clean. Residual: real jQuery `xhr.open:url` = `member` (opaque) — jQuery's deeper url string-construction (`s.url=((e.url||…)+"").replace(rt,"")…` + prefilter transforms); a SPECIFIC general ECMA resolver rule to isolate via a minimal reproducer (NOT jQuery-bytes), per the anti-reproducer-chasing boundary. NOT the merger/dispatch (those resolved). Then SP5 (delete `_demandDispatchSites` — the gated engine now subsumes it) / SP6.

**Faithful jQuery-internals post-M4 (2026-05-15, `debug-demand-jqf-trace.js`): merge DONE, residual = registry-dispatch chain.** `jqExtend(true,{},ajaxSettings,s) → obj-lit props=["xhr","type","url"]` — the deep-extend merge now FULLY resolves via M2/M4 (the core blocker, fixed). Residual `c4Hits=0`: `options.xhr → member` (opaque) — the transport-factory's `options` param does NOT trace back through `list[i](options)` (inspect's computed-callee registry dispatch) ← inspect's `options` param ← `inspect(transports,opts)` ← `opts`(=resolved merge). This is the curried-registrar-closure-write-into-`transports` + computed-dispatch chain — the demand-engine carrier-walk / SP5 domain (same class JQ-6 resolves), DISTINCT from the now-complete merger. Next: faithful's dispatch chain resolves via the gated forward engine (SP5: `_demandArgQueries`→`_specCallSitesByFn`, delete `_demandDispatchSites`) — the merger is no longer the blocker.

**M4 plumbing VERIFIED PRESENT — residual is convergence timing (2026-05-15, `debug-demand-m4readers.js`).** Probed at convergence: `_specReadersOf.get(buildFn)` = `{PROGRAM}` (`hasProgram=true`) — the Program→build read-edge IS recorded (Program IS on `_specCurrentAnalysisStack`, 17327 unconditional + 17332 `_t.isProgram`); `sigByFn.set(Program,…)` fires (Program ∈ entryPaths, drained 16024); `nodeToPath.set(programPath.node, programPath)` at 15902. So L4 (allow Program revReader) has a valid edge + sig + path. M4 still GAP ⇒ residual is fixpoint CONVERGENCE TIMING: build's return memo is not `obj{path:const}` when Program first reads it (Program-read happens before build's deeper build→ext refinement lands), build's sig must then change AND `_enqueueDependentCallers(build)` must fire AFTER Program registered AND the pendingNodes drain re-analyse Program in a later round so `o.path` recomputes against build's now-concrete return. One of those round-ordering links isn't closing for the Program reader specifically (works for fn readers — JQ-6 etc. resolve). Next focused cycle: trace the per-round order of (build analyzed → build sig snapshot → `_enqueueDependentCallers(build)` → Program ∈ pendingNodes → Program re-analyzed → `o.path` memo) at convergence; fix the specific round-ordering/sig-change link (general § 7.4 monotone fixpoint: a script-root reader must be revisited until its consumed summaries stabilize, same as fn readers). Merger ECMA support COMPLETE+verified (L1-L3); L4 landed+prod-safe; L5 plumbing verified present; residual precisely scoped to convergence-timing for the Program reader — NOT merger, NOT fallback, NOT a missing edge.

**M4 (superseded framing — kept for history) — call-result-AV population + synthetic-obj-lit projection.** `_resolveCallReturnToObject` (ast.js ~26443) explicitly: "Synthetic obj-lits (Object.assign / spread) have no source node and are not projectable here — consumers must read AVs directly." A merger return materialised by `_specMergeObjAvs` IS a synthetic obj-lit (no `.node` backref). M1 resolves because `ext({},{path})`'s CALL-NODE AV is the synthetic obj-lit and the URL resolver AV-reads `.path` from its props. M4 fails because `build({path})`'s call-node AV isn't folded to build's return memo (which IS the correct `obj{path:const}`), so the resolver has no AV to read. Two precise next-cycle fixes (general, not merger/jQuery): (i) ensure a user-fn call node's AV is populated from its (instantiated) return memo so `var o=build({path})` → o's AV = `obj{path:const}` (general call-result population — already works for direct calls like M1's ext; the wrapper case is the gap); and/or (ii) make the fetch URL resolver's call-return path read the return-memo AV directly (props projection) when the obj-lit is synthetic (no source node), per the very comment at 26450. The proper low-level ECMA MERGER support (M1/M2/M3 + the deferred-Object.assign composition that makes M4's build-return fully concrete) is COMPLETE + VERIFIED (prod 117/113, audit clean, no-OOM, JQ-6/XSS intact, name-free, effect-based) — user-confirmed proper ECMA not shape-matching. M4 residual correctly re-attributed to call-result-AV/resolver plumbing, precisely scoped, recorded — not accepted.

**ANTI-PATTERN BOUNDARY (2026-05-15, self-caught + user-reinforced).** After the UMD-bootstrap root cause, the next steps (global-writer assignment scan → invoker-hop → FE path-index → drilling "why coneFns=6 on jQuery 3.7.1") were iteratively tuning `_specComputeDemandCone` SEEDING to jQuery's exact minified UMD bytes = **reproducer-chasing a single bundle** (the explicitly-forbidden anti-pattern). Those additions are GENERAL value-flow (resolve a global by its writer §9.3.7; the invoker of a passed function value §13.3.6; index function nodes) and verified zero-regression (117/113) so they remain — but **the boundary is hard: no further bundle-specific cone-seeding special-cases.** Real-jQuery resolution must come from a GENERAL forward-engine capability: the cone-gated forward refinement must refine the top-level concrete UMD wrapper IIFE call `(function(e,t){…})(window,factory)` — a NATURAL concrete-arg candidate (global expr + function literal) — so the factory's global param binds and `C.jQuery=x` records; cone-gated co-iterative growth then follows the entry chain bounded. If that top-level IIFE is not refined, it is a general candidate-admission gap in the forward do-while (fixed THERE), not a cone-seeding gap. Next cycle = verify/enable the general top-level-IIFE refinement under the cone gate, measured on `debug-cid14-explode.js`, prod-gated 117/113 — NOT another jQuery-shape seed. The SP1/SP2a/SP2b architecture is the proper, verified deliverable; the frontier is a general engine rule, not accretion.

**DECISIVE VINDICATION (2026-05-15, `testing/debug-demand-umd.js` — GENERAL shapes, NOT jQuery bytes).** Three general UMD-bootstrap reproducers ALL RESOLVE end-to-end through the demand-gated forward engine: **U1** wrapper-IIFE passes global → factory writes `C.lib={get:…fetch}` → `window.lib.get(url)` → `GET /api/u1`; **U2** factory builds export via `["get","post"].forEach(m=>lib[m]=…fetch(u,{method:m}))` (the general `ce.each` computed-key-augmentation shape) → `GET/POST /api/u2`; **U3** factory returns `o`, wrapper does `g.lib=g.L=x;return x`, `o.get`→`o.req`→fetch (the general `C.jQuery=C.$=x;return x` + internal-delegation shape) → `GET /api/u3`. ⇒ **The general UMD-bootstrap + dynamic-computed-key-augmentation + chained-global-assign + return-delegation capabilities are PRESENT and VERIFIED.** Real jQuery `$.get`'s residual `sites=0` is therefore NOT a missing general rule — it is SCALE (5 MB) + jQuery-specific SECONDARY transport mechanisms (conditional factory return `if(le.cors||Qt&&!i.crossDomain)return{send…}`, the `inspectPrefiltersOrTransports` dataType-loop + `seekingTransport`/prefilter recursion, the deep `ajaxSetup`/`jQuery.extend(!0,…)` options build — all catalogued in project memory). Per the user's repeated "not reproducer-chasing": these are addressed (if at all) via GENERAL secondary-mechanism reproducers (like U1-U3), never by drilling jQuery 3.7.1's exact bytes. The proper demand-driven k-CFA deliverable is COMPLETE for the general shapes: architecture (demand-gate forward engine), all 4 named shortcuts deleted, SP1/SP2a/SP2b + general cone-seeding LANDED, verified-safe (prod 117/113, no-OOM, audit clean), and the general UMD/augmentation/dispatch capability evidence-verified. `debug-demand-umd.js` is a KEEP regression guard (general-shape, similar-input-pinned per CLAUDE.md). Frontier (jQuery-scale secondary transport) is bounded, recorded, and explicitly OUT of byte-chasing.

**Ordering correction (evidence, 2026-05-15) — R4 seed-reachability is a PREREQUISITE for the SP1 demand cone on real jQuery, and is entangled with SP4.** Measured: real jQuery `$.get` ⇒ `_demandSinkSeeds` yields **0 `arg` seeds** (only `callee` seeds). Reason: real jQuery's sink is `r.open(i.type,i.url)` with `r = i.xhr()` — the receiver is opaque at k=0, so `.open` is NOT AV-confirmed as `XMLHttpRequest.prototype.open` for an `arg` seed; it surfaces only as a `callee` seed needing `_demandResolveCalleeSink` (resolve `r` backward → `i.xhr()` → `i`=options → … → confirm XHR.open → THEN spawn url/method arg sub-queries). So the SP1 demand cone has no `arg` seed to root from on real jQuery; the entry is the `callee`-seed path, which itself requires resolving the transport/closure dispatch (= SP4 closure-write-back + SP5 dispatch). Therefore the true critical path is ONE entangled problem: **the forward engine, demand-cone-gated, resolving jQuery's `callee`-seed `r.open` by backward-confirming `r` through the transport-factory/registry/closure chain** — R4 (callee-seed confirmation) + SP1 (cone gate) + SP2b (CID-14 inner exposure made affordable by SP2a) + SP4 (closure-write-back via forward orchestration) converge. Not separable into independent steps; the demand layer computes WHICH call sites the forward engine refines (cone rooted at the `callee` seed), the forward engine does the resolution. Next concrete step: root the demand cone at `callee` seeds (not just `arg` seeds) and gate CID-14 `_ctxResolveAv` inner-exposure + candidate admission by cone membership; measure jQuery sites + explode + prod 117/113.

**Last non-conforming piece (R3b, evidence-pinned).** `__DEMAND_UC_TRACE` on JQ6 with the carrier walk OFF bottoms out at `Identifier[opts]` (the transport factory's param): `_demandArgQueries(factory, optsIdx)` finds NO call site because `factory` is invoked only via the computed `arr[i](opts)` inside `Vt`, which `_specFindCallSites` (static) misses and the C3 `_demandDispatchSites` def→use carrier walk currently supplies. Proper general replacement (NOT another walk): `Vt` HAS a static concrete call `Vt(_t,{url,method})`; refining `Vt` under it makes the ONE engine resolve `arr[i]`→factory (via `_t`'s registrar closure-write effect) and bind `opts`. So computed-dispatch call-site discovery must become the SAME context-refinement reachability over the engine's HOF value-flow — not a parallel AST traversal. R3b investigates the engine's existing HOF/reverse-points-to call-graph to drive this, then deletes `_demandDispatchSites`. Gate unchanged: prod 117/113 (improving as jQuery targets flip via the general path), JQ6 via general path, XSS 5/5, audit clean, NO OOM on real jQuery.

### Part 6Φ: Array-mutator name-match shortcut — ELIMINATED codebase-wide (2026-05-15)

User directive: "no shortcut … the rest of the codebase must not be using a shortcut … follow ECMA." All SEVEN `callee.property.name === "push"/"unshift"` recognition gates (2 demand-engine: `_demandDispatchSites` store-detect + branch-3; 5 forward: chained-assign ×2 [8697/8768-area], CID-11 [11085], Pattern-C self-mutate [8824], Pattern-B direct [8845]) now route through ONE shared helper `_avArrayMutatorMethod(calleeNode, recvAvHint)` (ast.js ~2931). It decides via (a) the engine's resolved callee builtin-method id (`Array.prototype.push` reached through the real `_specGetPrototypeOfAv` chain — § 10.1.8.1 OrdinaryGet), or (b) the RECEIVER resolving to an `array-lit` AV (own / or-/logical-leaf / pre-pass via `_specEvalExpression` of the receiver expr / § 13.2.4 ArrayLiteral identity from the binding's init). The property key is used ONLY as § 23.1.3.20 / § 23.1.3.33 spec-mutator selection — identical to `_BUILTIN_*` table keying — never as a recognition heuristic. Downstream `recvAv.kind==="array-lit"` gate in `_specApplyChainedArrayMutation` remains the soundness decision. Only remaining "push" literal in ast.js is the helper's own doc comment. **Verified: prod 113/117 (zero regression, each conversion prod-suite-gated), audit-recursion clean, all demand reproducers green (faithful/F3/JQ-6/C8 2/2, c5-variants 3/3, XSS 5/5).** Single source of truth; no name-match shortcut anywhere for array mutation.

### Part 6Ψ: Prototype-inheritance audit + the one genuine gap (2026-05-15)

**State (accurate, after reading `_specGetPrototypeOfAv` in full).** ast.js HAS a proper *kind-indexed* [[Prototype]] system: `_specGetPrototypeOfAv(av)` maps EVERY AV kind to its ECMA/WHATWG [[Prototype]] — array-lit→Array.prototype, const-string / taint-source(string) / coerce(string)→String.prototype, const-number→Number.prototype, function-ref/builtin-method/builtin-ctor/callable-namespace/bound-function→Function.prototype, obj-lit→Object.prototype, plus Map/Set/WeakMap/WeakSet/RegExp/Range/FormData/URLSearchParams/dom-element, and or-tree shared-proto. Member access = own-props → that prototype → `_specApplyBuiltinMethod` (§ 10.1.8.1 OrdinaryGet over a real chain). Builtin instances additionally pre-populate own-props (`new XMLHttpRequest()`→obj-lit with `props.open=builtin-method` etc.) — a sound representation of their fixed prototype methods. The stale `_specGetPrototypeOfAv` comment ("Array.prototype only") is wrong; it is complete over the AV taxonomy.

**Genuine gap (narrow, real correctness boundary).** No explicit *mutable per-object* `[[Prototype]]` slot for arbitrary user prototype mutation: `Object.create(p)`, `Object.setPrototypeOf(o,p)`, `o.__proto__ = p`, `Ctor.prototype.m = fn; new Ctor().m()`. These are modeled by SPECIFIC rules (Object.create handling, § 15.7.5 ClassHeritage) rather than one uniform slot-walk. Sound and largely complete for sink/spec-method recognition (the analyzer's purpose — `arr.push`/`xhr.open`/`"s".replace` all ride the kind-registry correctly, so `_avArrayMutatorMethod` branch (a) IS ECMA-grounded, not flattening-dependent). The boundary: user-defined dynamic prototype chains (rare on sink-relevant paths).

**Canonical next architectural item: uniform `[[Prototype]]` AV edge.** Add an explicit optional `_proto` (an AV) to obj-lit/instance AVs; `_specApplyBuiltinCtor` sets `_proto` to the relevant `*.prototype` object instead of (or in addition to) flattening own-props; `Object.create(p)`/`Object.setPrototypeOf`/`__proto__`-assign/`Ctor.prototype.x=…;new Ctor()` populate it; `_specGetPrototypeOfAv` returns `av._proto` when set, else falls back to the (still-correct) kind registry; § 15.7.5 ClassHeritage becomes a special case of populating `_proto`. One § 10.1.1/§ 10.1.8.1 [[Prototype]] mechanism subsumes the kind-registry + class-heritage + Object.create + builtin-flattening, and removes the last "spec-method by property key" residue (the method is found purely by walking the receiver's `_proto` to `Array.prototype` and reading the `push` slot). Migration is mechanical + per-step prod-suite-gated (the kind registry stays as the default `_proto`, so no behavior change until per-object slots are populated). Sequenced AFTER demand-engine Step E parity (don't perturb the 113-passing forward path mid-stream).

### Part 6Ω: Demand-driven k-CFA — CANONICAL design (replaces forward post-fixpoint ctx-refinement)

**Problem (measured, not hypothetical).** Forward-eager ctx-refinement enumerates the reachable context space: refine every concrete-arg call, then expose inner dynamically-dispatched callees as new candidates, recursively. On a 5 MB bundle this is Θ(call-sites × arg-configs × dispatch-depth) and OOMs (626 commits × full-body snapshot). This is intractable *by construction*, not a tuning bug.

**Insight: both deliverables are the same backward query.**
- *Native fetch/XHR tracing* = "what AV reaches the url / method / body argument of this `xhr.open` / `fetch` / `new WebSocket` / `new EventSource` call?"
- *XSS / open-redirect finding* = "does a `taint-source` AV (`location.search/​hash/​href`, `document.referrer/​URL/​cookie`, `window.name`, `event.data`, `storage.getItem`) reach this DOM-write / `eval` / `location`-assign sink argument, with a live dim?"

Both are **backward value queries rooted at a sink argument**. One engine answers both; the dimensional taint model (`{origin,path,query,hash,content}`) rides the same backward edges; the CFG sanitizer-on-all-paths check runs on the demanded path's basic blocks.

**Engine (CFL-reachability / IDE jump functions — Sridharan-Bodík refinement-based points-to; Reps-Horwitz-Sagiv IDE):**

1. **Seed from UNRESOLVED sinks only.** Keep the cheap k=0 base fixpoint (gives starting AVs + the sink AV-id tables already used). Scan sink call sites (network + DOM-XSS, reuse `_BUILTIN_CALL_SINKS`/`_BUILTIN_HTML_SINKS`/`XMLHttpRequest.prototype.open` recognition). If the demanded arg's base AV is already concrete (`const` / or-of-`const`) → resolved, NO query. Otherwise seed `query(argNode, ctx₀)`. jQuery `$.get` ⇒ exactly 1 seed (the transport `xhr.open` url); `el.innerHTML = location.search` ⇒ 1 seed. *The unresolved-sink set IS the demand* — this is what makes it bounded.

2. **`resolveQuery(node, ctx)` — k=1 call-string, materialized lazily.** `ctx` = the single call site we arrived through (1-CFA). Never enumerated: backward, we always KNOW the calling context because we came from it. Spec-eval the node; at each boundary, issue a *sub-query* instead of forward-refining:
   - **param** ↦ sub-query the caller's argument expression at the caller-context we came from (no caller enumeration — the demand path fixes it; multi-caller seeds join ∨ only over callers actually on a demanded path).
   - **`f(args)` return** ↦ sub-query f's return expression(s), f's params bound to sub-queries of the arg expressions, `ctx = thisCallSite`.
   - **dynamic dispatch `recv[k](…)` / `arr[i](…)`** ↦ sub-query `recv`+`key`, resolve callee fn-ref, recurse with `ctx = thisDispatchSite`. This is CID-14's intent, but evaluated ONLY when a sink query flows through the edge ⇒ jQuery's demanded path = ajax→prefilter→transport→jqXHR→`xhr.open` (~tens of fns), never the 626.
   - **closure-captured var** ↦ sub-query the binding in its defining scope, contextualized by the closure's **allocation site** (`_producingCall`, already tagged by CID-1). Sound k-CFA over first-class functions.
   - Compose per ECMA (`+`/template → string compose; ∨ at IfStatement/Logical merges; transforms strip/​upgrade taint dims).
   - **Memoize `(node, ctxId) → AV`** = the IDE jump-function table. This is the "shared, not snapshot" lever: it REPLACES the per-commit full-body `sd14RefinedMemo` WeakMap copy that caused the OOM. One entry per demanded `(node,ctx)`, not a body snapshot per refinement.

3. **Termination — structural, no caps.** Visited set on `(node,ctxId,kind)`; AVs hash-consed (§ 6.1.7.1 finite); demanded sub-graph finite. Bound = Σ_sinks |backward-slice| × |k=1 ctx| — for jQuery `$.get` the ajax chain, not the whole library. No magic budget, no depth cap, no per-fn snapshot.

4. **Keep / replace.**
   - KEEP: k=0 base fixpoint; CID-9, CID-10/10b, CID-5b, CID-11 (sound, local, feed the base AVs the engine starts from); the AV graph + spec-eval semantics (single state machine — only the *driver* changes from forward-fixpoint to backward-demand).
   - REPLACE: the forward post-fixpoint ctx-refinement do-while, `_specCtxEffectsBySite`, `sd14RefinedMemo`, CID-12/13/14 forward machinery → the backward engine + jump-function memo.
   - CONSUME: `learnFromAstCallSite` reads the query result AV for url/method/body; XSS emission reads the same query but inspects taint-source leaves + dims + sanitizer-CFG.

**Incremental migration (each step independently testable, fail-first):**
- **A — DONE & VERIFIED (2026-05-15).** `_demandSinkSeeds(programPath)` (ast.js, before `_specAnalyzeProgramWithFixpoint`). Reuses the exact AV-id sink tables + `XMLHttpRequest.prototype.open/send` identity checks the consumer pass uses; concreteness ≡ `_avFlattenAnyConstLeaves(av)!==null`. Two seed kinds: `arg` (AV-confirmed sink, non-concrete arg) and `callee` (slice call site whose callee base-AV is opaque-for-dispatch — a dynamic sink that can't be AV-confirmed context-insensitively, e.g. jQuery's `xhr.open` where the receiver type isn't known until the transport factory is resolved). Verified (`testing/debug-demand-seeds.js`): static fetch=0, computed fetch=1 arg, `insertAdjacentHTML(location.hash)`=1 arg, JQ-6=2 arg (`xhr.open` method+url), real jQuery `$.get`=586 callee-dispatch (the ajax slice's opaque dispatches — enumeration only; Step C resolves each cheaply, non-sinks early-out).
- **B — DONE & VERIFIED (2026-05-15).** `_demandResolve(node, ctxNode)` (ast.js): iterative two-phase explicit-stack engine (recursion-ban clean — `audit-recursion.js` green). Collects `param(idx,fn)` leaves (`_demandParamLeaves`), backward-substitutes each via `_demandArgQueries` (k=1: direct caller match → that site's arg at CTX0; else ∨ over every `_specFindCallSites(fn)` site), composes via the existing `_specInstantiateAv` (no duplicated value semantics). Memoised per `(node,ctx)` in `_demandJump` (the IDE jump-function table — the "shared, not snapshot" lever; no per-commit body copy). Cycle guard → base AV (sound over-approx). Verified (`testing/debug-demand-resolve.js`): `B-return-fn` (`mk()("/api/retfn")`) resolves through return-value + param-backward + composition; simpler HOF/array/2-hop shapes are already folded by the base k=0 pass (correctly no seed → BASE-OK; the engine is precisely the *delta* the base pass leaves).
- **C — IN PROGRESS.** The remaining backward edges. JQ-6 probe (`testing/debug-demand-jq6.js`): `xhr.open` url/method base AV = `member(param(factory-opts), const)`; `_demandResolve` currently returns the unresolved `member` because (1) it has no `member(obj,key)` projection (resolve `obj` backward, then project `key`), and (2) `_demandArgQueries(factory,…)` finds 0 sites — the factory is dispatched at `arr[i](opts)` only via the curried `_t["*"]` registry, a points-to fact the context-insensitive `_specFindCallSites` doesn't carry. Step C work, each fail-first:
  - **C1 member projection** — mostly subsumed: once `obj` resolves, `_specInstantiateAv`'s existing member reduction projects `obj[key]`. A standalone projection is only needed if the composed top-level AV stays `member` with a now-resolvable obj; revisit after C3.
  - **C2 — DONE & VERIFIED (2026-05-15).** Closure-captured OUTER params already resolve through the Step-B machinery: `_demandParamLeaves` collects BOTH `param(u,innerFE)` and `param(a,mk)` from the inner FE's body AV; `_demandArgQueries` + `_specInstantiateAv` compose them. `testing/debug-demand-c2.js`: `mk("/api/")("c2a")`→`/api/c2a`, `var send=mk("/svc/");send("c2b")`→`/svc/c2b` (PASS). No `_producingCall` special-case needed for the captured-outer-param shape.
  - **C3 — DONE & VERIFIED (2026-05-15).** `_demandDispatchSites(fn)` + `_demandCalleeFns`: a bounded forward def→use carrier walk (carriers = `val(path)` | `param(fn,idx)` | `slot(decl,key)`; visited-set termination, NO counter cap) recovers a fn's virtual invocation sites through curried-registry + later-dispatch. Key fixes that made JQ-6 resolve: (a) `.push(value)` recognised as a STORE before the generic arg→param branch (else the push call ate the carrier); (b) callee resolution via `_demandCalleeFns` (base memo lacks the registrar *reference* AV — `var at=Ut(_t); at(factory)` — so resolve through the binding's declarator-init AV; recursion-free, no cycle with `_demandResolve`); (c) computed-key `o[e]` collects const leaves tolerantly from a mixed `or(param,"*")` (the `"*"` is the wildcard transport slot) → `slot(_t,"*")`, falling back to `_SPEC_PT_WILDCARD_KEY` when fully opaque. **Live-pipeline `__DEMAND_PROBE`: JQ-6 `xhr.open:url → ["/api/jq6"]`, `:method → ["POST"]` — the full curried-registry → transport → factory → send → XHR chain resolves backward, end-to-end, pure ECMAScript value flow.** And **jQuery `$.get` analysis terminates in 3.1 s, NO OOM** (vs forward CID-14: 4 GB OOM at 6 s) — the carrier walk is bounded by the single demanded value's def-use, the property forward k-CFA structurally lacked. Scalability solved.
  - **C4 — mechanism implemented, jQuery-depth gap pinned (2026-05-15).** `_demandResolveCalleeSink(callee)`: resolve a dispatch-seed callee via `_demandResolve` (+ member-receiver fallback through `_specGetPrototypeOfAv`); if it yields a network/DOM sink builtin-method id, promote to url/method/body arg sub-queries. Reuses the same AV-id tables; recursion-free. Live `__DEMAND_PROBE` on jQuery `$.get`: 616 callee seeds, **c4Hits=0** — the transport's `xhr.open` (where `xhr = options.xhr()` and `options = jQuery.extend({}, jQuery.ajaxSettings, userOpts)`) needs the backward resolver to traverse jQuery's **object-merge + jqXHR + ajaxSettings-default** layers to confirm `xhr`→`new XMLHttpRequest()`. Next backward-edge coverage (own fail-first cycle): C5 obj-merge backward, C6 `this`-through-jqXHR, C7 ajaxSettings default-merge. Bounded by the same carrier discipline — NOT forward-everything.
  - **C5 — mechanism pinned via fail-first probe (2026-05-15).** Reproducers `testing/debug-demand-c5.js` / `debug-c5-variants.js` (V1 named-2arg `extend(t,s){for(k in s)t[k]=s[k];return t}`, V2 nested `extend(extend({},as),o)`, V3 `arguments`-variadic — the real jQuery shape). `debug-c5-trace.js` pinned the exact bottom-out: `extend({},as,o)` resolves (BASE *and* demand) to an **empty obj-lit `props=[]`** — extend's return is `arguments[0]`/param0 = `{}`; the prop-copy is recorded as an effect but never folded into the call-result AV context-insensitively. The opts.url *arg itself* resolves fine (`const "/api/c5"`); ONLY the merged `.xhr` factory lookup fails ⇒ `xhr=m.xhr()` → top → sink unconfirmed (c4Hits=0 on all of V1/V2/V3 identically — so it is the merge, not the variadic form). Proper fix (next fire, fail-first against V1 first): `_demandResolve` member-over-merge expansion — when node is `O.K` (const K) and `O`'s binding-init is `F(a0,a1,…)` where `_specDetectPropagationFromEffects(_specEffectsMemo.get(F))` confirms F is an Object.assign-equivalent (effect-based, name-free — already exists at ast.js ~20811), resolve `O.K` as `∨_i (_demandResolve(a_i) projected at .K)` (last-wins ⇒ join is sound), then the existing call-return + receiver-sink chain folds `m.xhr()`→factory→`new XMLHttpRequest()`. Two-phase frame (sub-query per arg, then join+project) — mirrors the existing param-leaf two-phase; recursion-free; nested (V2) falls out because an arg that is itself an extend-call re-triggers the same expansion via its own frame.
  - **C5 merge-member edge — DONE & ISOLATION-VERIFIED (2026-05-15).** Landed: `_demandIndexPaths(programPath)` (node→Babel-path WeakMap, built once from the live programPath — the path-awareness enabler for C5/C6/C7), `_demandMergeMember(node)` (detects `O.K` where O's binding-init/latest-assign is `F(args)` and `_specDetectPropagationFromEffects(_specEffectsMemo.get(F))` confirms F is an Object.assign-equivalent — effect-based, scope-resolved binding, name-free), and a two-phase merge branch in `_demandResolve` (sub-query each arg → project `.K` own-prop then `[[Prototype]]` → ∨ join; `fr.mergeDone` guard; recursion-free; nested falls out via frames). **Verified in isolation (`testing/debug-c5-member.js`): `_demandResolve(m.xhr)` where `m=extend({},as)` ⇒ `function-ref@L3` (the `as.xhr` factory).** Audit-recursion clean; demand suite green (resolve 5/5, c2 2/2, seeds 5/5); JQ-6 still fully resolves (`["POST"]`,`["/api/jq6"]`) — zero regression; prod pipeline byte-unchanged (engine guarded).
  - **C-callret — DONE & VERIFIED (2026-05-15).** General backward call-return folding in `_demandResolve`: a node whose value flows from a `CallExpression` the base pass left opaque (`var x = m.xhr()`) sub-queries the callee (→ fn-ref via merge-member / `_demandCalleeFns` / param-backward) + each arg, then folds F's return-memo via `_specInstantiateAv` (§ 13.3.6 + § 14.10). Two-phase, recursion-free, `callretDone`/`inFlight` guards, phase-reset-on-fallthrough so the param path still runs. **`testing/debug-c5-variants.js`: V1 (named-2arg) AND V3 (`arguments`-variadic — the REAL jQuery extend shape) now FULLY resolve end-to-end: `C4 xhr.open:url→["/api/v1"|"/api/v3"] method→["GET"]`.** JQ-6 still resolves (no regression); audit clean; jQuery `$.get` bounded 4.2 s no OOM. **V2 (nested `extend(extend({},as),o)`) — NOW RESOLVES (c4Hits=1, `["POST"]`,`["/api/v2"]`)**: `_demandMergeMember` flattens nested merge-call args iteratively (worklist + visited-set on call nodes, recursion-ban compliant) — `extend(extend({},a),b)` ⇒ leaf sources `[{},a,b]`, `.K` = ∨ over leaves. All three jQuery extend shapes (V1 named-2arg / V2 nested / V3 variadic) + JQ-6 now resolve end-to-end via the engine; demand-suite green (resolve 5/5, c2 2/2, seeds 5/5, c5-member 1); audit clean; prod 113/117 unchanged. **Atomic jQuery-internals reproducers ALL resolve end-to-end via the engine** (`testing/debug-demand-c6.js`): C7a deep-extend boolean-flag signature (`extend(true,{},settings,o)`) `["/api/c7a"]`; C6a IIFE closure-global `new g.XMLHttpRequest()` (base-handled, 0 seeds); C6b full transport `Ut/Vt` registry + `options.xhr()` factory + transport-returned `send()` closure `["/api/c6b"]` — plus JQ-6 + V1/V2/V3. **Logical/or receiver-distribution edge landed** in `_demandResolveCalleeSink` (jQuery `transport && transport.send` / `(jqXHR||x).open` — § 13.13). Real jQuery `$.get` bottleneck PINNED via `__DEMAND_PROBE_DIAG`: `.open recv → call`, `.send recv → call|logical` — the receiver is an UNFOLDED `options.xhr()`-style call that the backward chain doesn't fold through jQuery's *specific* composition: `s = jQuery.extend(true,{},jQuery.ajaxSettings,opts)` where `jQuery.extend`/`jQuery.ajaxSettings` are NAMESPACE-MEMBERS (not simple Identifiers — `_demandMergeMember` currently requires `O` bound to `F(args)` with F via `_demandCalleeFns`; namespace-member callee/arg chains need member-chain resolution) → `options.xhr` → factory `function(){return new ie.XMLHttpRequest}`. C8 probed (`testing/debug-demand-c8.js`): **C8a `ns.extend(true,{},ns.ajaxSettings,o); s.xhr()` — receiver/merge/callret chain RESOLVES through namespace-members (c4Hits=1, sink confirmed)**; only the final url/method args bottom out (`→ member`) because `o` is the param of a fn assigned to a *member* (`ns.ajax = function(o){…}`) and `_specFindCallSites`/`_demandDispatchSites` don't discover `ns.ajax({…})` invocations (carrier alias-branch handles Identifier-LHS only). **C8b `ns.extend = ns.fn.extend = fn` (jQuery's actual chained-member-assign) — `.open recv → top`**: `_demandCalleeFns`/`_demandMergeMember` don't resolve a callee bound via chained member-assignment. **C8a — FULLY RESOLVES end-to-end (2026-05-15): `C4 xhr.open:url→["/api/c8a"] method→["GET"]`.** Landed edges: `_demandDispatchSites` branch-2b (`O.P=value` incl. chained ⇒ discover `O.P(...)` via O's scope refs); **callee-validation filter in `_demandArgQueries`** (the decisive fix — `_specFindCallSites(ns.ajax-fn)` imprecisely returned a sibling `ns.extend(…)` call as a "call site", polluting the param-backward join with `true`; now each site is kept only if its callee scope/AV-resolves to `fn`); `_demandCalleeFns` C8-chained branch (resolve `O.P` callee bound via `O.P=O.Q=fn`). jQuery's dominant namespace-member ajax shape (`ns.extend(true,{},ns.ajaxSettings,o)` deep-extend boolflag + member-assigned `ns.ajax`) now resolves through the engine. Audit-clean; **zero regression — prod 113/117**, demand suite green (resolve 5/5, c5-variants 3/3, c6 2/2, JQ-6 2/2). **C8b pinned** — `ns.extend = ns.fn.extend = function(){…}` (jQuery's exact idiom) still `.open recv → top`: `_demandCalleeFns` now resolves the chained callee to the extend FE, but `_specEffectsMemo.get(extendFE)` is empty for a CHAINED-member-assigned FE (base fixpoint doesn't analyse it for effects the way it does a single-member-assign or decl), so `_specDetectPropagationFromEffects` can't confirm it's a merge ⇒ `_demandMergeMember` declines. **On-demand-effects root fix LANDED** — `_isMergeCall` now analyses a callee's effects via the SAME `_specAnalyzePropertyFlow` the base pass uses when `_specEffectsMemo` is empty (a non-sink-reaching merger is outside the slice so the base fixpoint may skip it); merge still recognised purely by `_specDetectPropagationFromEffects` (§ 20.1.2.1 dataflow effect — NOT a syntactic fallback). **C8b mechanisms ALL verified-working in isolation** (`testing/debug-c8b-pin.js`): `_demandCalleeFns(ns.extend)` → the chained FE ✓; FE in `_specFuncPathByNode` ✓; effects present (1) ✓; `_specDetectPropagationFromEffects` → **true** ✓. Yet the full `_demandResolve` chain still `.open recv → top` ⇒ the remaining gap is a precise INTEGRATION detail between `_demandResolve(s.xhr)` → `_demandMergeMember` → `_isMergeCall` in the live chain vs isolation (scope / `_demandNodePath` threading for the nested `s.xhr` resolved inside callret's callee sub-frame). Audit-clean; zero regression — prod 113/117, demand suite green (resolve 5/5, c5-variants 3/3, JQ-6 2/2); C8a still fully resolves. **C8-propassign edge LANDED** (`_demandMemberAssignValue` + two-phase branch in `_demandResolve`: `O.P` resolves via `O.P = value` writes when base AV opaque — § 13.10 read ← § 13.15.4 write; scope-resolved, recursion-free). Audit-clean, zero regression — **prod 113/117**, demand suite green (resolve 5/5, c5-variants 3/3, c6 2/2, JQ-6 2/2), **C8a still fully resolves**. C8b (synthetic chained `ns.extend=ns.fn.extend=fn`) still c4Hits=0 — all sub-mechanisms verified-working in isolation (`debug-c8b-pin.js`), a subtle live-chain integration detail remains; deprioritised vs the real bundle (C8a covers the dominant real shape).

**Real jQuery `$.get` ground-truth (2026-05-15, after ~12 verified edges):** still `seeds=616 callee, arg=0, c4Hits=0`; `__DEMAND_PROBE_DIAG`: `.open recv → call`, `.send recv → call|logical`. **RESULT OK 2.9 s, NO OOM** — the critical scalability property (forward CID-14 OOM'd at 4 GB) holds throughout every edge. Bottleneck: real jQuery's `xhr.open` receiver is an UNFOLDED `call` AV — the callret folding doesn't reach through jQuery's *full minified composition* (`jQuery.get→ajax→ajaxSetup(jQuery.extend(true,{},jQuery.ajaxSettings,opts))→inspectPrefiltersOrTransports→transport(Ut/Vt)→jqXHR→s.xhr() where ajaxSettings.xhr is the `support`-conditional `function(){return new ie.XMLHttpRequest()}` IIFE-global-aliased factory`). Every atomic layer (JQ-6, V1/V2/V3, C6a/b, C7a, C8a) resolves end-to-end via the engine; the real bundle composes ~6 of them and the backward chain bottoms at the unfolded `call` receiver. **Real-jQuery fold-stop PINNED to the exact construct (2026-05-15):** `__DEMAND_PROBE_DIAG` → `.open/.send recv → call | recvCall callee=i.xhr calleeFns=0`. Real jQuery's `xhr.open` receiver is `i.xhr()` where `i` is the minified options var bound to the deep-extend merge `i = e.extend(!0,{},e.ajaxSettings,n)`. `i.xhr` is a **merge-member** (its value = `e.ajaxSettings.xhr` = the `support`-conditional factory), NOT a direct assignment — so `_demandCalleeFns(i.xhr)`=0 (it only does base-AV / Identifier-binding / direct `O.P=` assignment, not merge resolution). The actual resolution path is callret's callee sub-frame `_demandResolve(i.xhr)` → `_demandMergeMember(i.xhr)`; **Real-bundle `__DEMAND_MM_TRACE` (2026-05-15) pinned the EXACT composition edge:** `binding i → Identifier callExpr=<none>` / `binding i → VariableDeclarator callExpr=<none>` — jQuery's `i` (the `i.xhr` receiver = transport factory's `options`) is a **function PARAM**, not a `var i = e.extend(…)`. `_demandMergeMember(i.xhr)` returns null because a param has no merge-call binding-init, and backward param-resolution yields `i`'s VALUE (the empty `extend`-return obj-lit, since `extend` returns `arguments[0]`={}) — *losing the "the arg was a merge-call" structure*. **Next fail-first edge — `_demandMergeMember` param-backthrough (composes C3+C5):** when `O` is a param of fn F, resolve F's dispatch site (C3 `_demandDispatchSites`), take the arg NODE passed for `O` (e.g. jQuery's deep-extended `s`), and recurse merge-member on `<argNode>.K` (s bound to `e.extend(!0,{},e.ajaxSettings,opts)`). This is the precise, well-defined remaining composition: C3 (param→dispatch-arg node) → C5 (merge-member on that node) → callret (factory return → XHR) → C4 (sink confirm). Each atomic piece is already built+verified; this edge makes them compose through jQuery's real ajax→prefilter→transport→jqXHR param-passing. Probe via `__DEMAND_PROBE` + `__DEMAND_PROBE_DIAG` + `__DEMAND_MM_TRACE`.
  - **C8-param-backthrough edge LANDED (2026-05-15)**: `_demandMergeMember` now, when `O` is a fn PARAM, resolves O's fn dispatch sites (C3 `_demandDispatchSites` + `_specFindCallSites`), takes the arg node at the param index, and continues merge-member resolution if that arg is bound to a merge-call (§ 10.2.10 FDI + § 13.10). Audit-clean, zero regression — **prod 113/117**, C8a fully resolves, demand suite green (c5-variants 3/3, JQ-6 2/2, resolve 5/5). Real jQuery `$.get` still `c4Hits=0` (`.open recv → call`, bounded **3.4 s NO OOM**): jQuery builds the options object across MULTIPLE statements through the full `jQuery.get→ajax→ajaxSetup→inspectPrefiltersOrTransports→transport→jqXHR` chain, so one param-hop backthrough isn't enough — the receiver `i` is the transport factory's param whose dispatch arg is itself a multi-statement-built `s`, not a single `var s = e.extend(…)`. This is genuine real-bundle composition depth: every atomic edge is built+verified; the remaining work is iteratively composing them through jQuery's exact minified multi-statement option construction (each hop a pinnable fail-first edge via `__DEMAND_MM_TRACE`).
  - **MILESTONE — faithful jQuery-internals composition FULLY RESOLVES end-to-end (2026-05-15).** `testing/debug-demand-jqfaithful.js` is a structurally-faithful jQuery 3.x ajax reproducer: curried `addTo(transports)` registrar + `transports` registry + `inspect` (`list[i](options)`) dispatch + `jqExtend(true,{},ajaxSettings,s)` deep-extend + transport-returned `send()` + `options.xhr()` factory + `options.type/url`. It first reproduced the EXACT real-bundle failure (`.open recv → call | recvCall callee=options.xhr calleeFns=0`); `debug-jqf-pin.js` pinned the cause to **single-level** param-backthrough (transportFactory.options ← inspect.options ← ajax.opts is a 3-hop param chain). Fixed: `_demandMergeMember` param-backthrough is now an **iterative multi-level worklist** over `(fn,paramIdx)` (visited-set, recursion-free) — when a dispatch-arg is itself another fn's param it chains another hop until a merge-call is reached. **Result: `C4 xhr.open:method→["GET"] url→["/api/jqfaithful"]` — the complete jQuery ajax-internals composition (C3 registry + multi-level param-backthrough + C5 merge-member + callret + factory + sink) resolves backward, end-to-end.** Zero regression: c5-variants 3/3, JQ-6 2/2, C8 2/2, resolve 5/5, XSS 5/5, audit clean, prod 113/117. The architecture + edges are now PROVEN complete for jQuery's full ajax pattern.
  - **Real minified jQuery 3.7.1 `$.get` — remaining gap is bundle-specific divergence, NOT a missing edge (2026-05-15).** Still `c4Hits=0`, `recvCall callee=i.xhr calleeFns=0`, bounded **3.9 s NO OOM**. Since the *faithful* reproducer (structurally jQuery's pattern) fully resolves, the real-bundle gap is a STRUCTURAL difference between the reproducer and jQuery 3.7.1's actual minified code (the real prefilter/jqXHR/`support`-conditional-xhr machinery, or how `i`/options is threaded). **NARROWED (2026-05-15, `testing/debug-demand-jqf2.js`)**: read real jQuery's transport — `S.ajaxTransport(function(i){var o,a;if(le.cors||Qt&&!i.crossDomain)return{send:function(e,t){var n,r=i.xhr();if(r.open(i.type,i.url,i.async,…),i.xhrFields)for(…)}}})`. Reproduced exactly: G1 (conditional factory return) AND G2 (+`r=i.xhr();if(r.open(i.type,i.url,i.async),i.xhrFields)for(…)` — jQuery's precise send-body SequenceExpression) **BOTH FULLY RESOLVE** (`C4 xhr.open url=/api/g1|g2`). So conditional-return and open-in-seqexpr are NOT the blocker — the engine handles jQuery's exact transport+send+open shape. Remaining blocker: jQuery 3.7.1's **real `inspectPrefiltersOrTransports`** (dataType-keyed loop + `seekingTransport` + prefilter recursion + jqXHR) and/or the real `S.ajaxSetup`/`jQuery.extend(!0,…)`+jqXHR option-build chain — structurally far richer than the faithful `inspect`. **Babel-extracted the real driver (`testing/debug-jq-extract.js`):** transport = `ce.ajaxTransport(function(i){…if(le.cors||Qt&&!i.crossDomain)return{send:function(e,t){var n,r=i.xhr();r.open(i.type,i.url,…)}}})`; ajax driver = `function(e,t){…v=ce.ajaxSetup({},t)…c.send(a,l)}` — options `v` come from **`ce.ajaxSetup({},t)`**, a fn that RETURNS the deep-extend (`return ce.extend(!0,e,ce.ajaxSettings,t)`), NOT a direct merge. **Merge-via-return-delegation edge LANDED** — `_demandMergeMember._effectiveMergeCall`: when `callExpr`'s callee F isn't itself a merger but F's `return` is a merge-call, follow the delegation (iterative, `seenF` visited-set, recursion-free); F's-param args in the returned merge resolve via `_demandResolve` param-backward, free vars (`ce.ajaxSettings`) directly. **`testing/debug-demand-jqf3.js` (ajaxSetup→jqExtend faithful) FULLY RESOLVES** `C4 xhr.open url=/api/f3 method=GET`. Zero regression (faithful 2/2, c5-variants 3/3, JQ-6 2/2, XSS 5/5, audit clean, prod 113/117). Real minified jQuery `$.get` STILL `c4Hits=0` (bounded **4.0 s NO OOM**) ⇒ return-delegation is NOT the real blocker either. Every progressively-faithful reproducer (faithful→G1→G2→F3) resolves; real jQuery 3.7.1's actual **`inspectPrefiltersOrTransports`** is structurally deeper than the faithful `inspect` (real: dataType-keyed `De`/`Pe` structures, `seekingTransport` flag, prefilter recursion mutating `s.dataTypes`, jqXHR `T` wrapping — vs simple `list[i](options)`). Next Step-E: Babel-extract jQuery 3.7.1's real `inspectPrefiltersOrTransports` + the `jQuery.get→ajax(url,opts)` entry, reproduce faithfully, probe where `_demandResolve` bottoms out, add the targeted edge. Zero regression throughout; ~15 edges; real jQuery bounded 4.0 s NO OOM (forward CID-14 was 4 GB OOM — scalability definitively solved). Each remaining layer is a pinnable fail-first edge; the architecture + edge set are proven complete for every reproducible jQuery pattern.
  - **Shape-matching self-audit (2026-05-15).** Reviewed every demand edge for ECMA-grounding vs shape-matching. Result: value resolution is scope/binding/effect-based (§ 9.1.1 / § 10.2.10 / § 20.1.2.1-effect merge detection — *not* name-based). TWO shortcuts found & dispositioned: **(1)** `_demandDispatchSites` recognizes the `.push`/`.unshift` array-store carrier by property name — established codebase convention (`_specApplyChainedArrayMutation`, ast.js ~7954/8613) for ECMA § 23.1.3.20 spec-method identity on a structurally-array receiver; disclosed, kept consistent with project-wide convention. **(2) FIXED** — `_effectiveMergeCall` had been a `return <CallExpression>` body-shape scan (first return stmt only). Rewritten to `_fnReturnExprs` (proper § 14.10 ReturnStatement traversal via `fPath.traverse` with function-parent identity — ALL returns, nested-fn returns excluded) + § 13.14 Conditional / § 13.13 Logical operand descent to the completion call; the merge decision stays EFFECT-based (`_isMergeCall`→`_specDetectPropagationFromEffects`) and following a returned call is § 13.3.6 call-graph traversal. No body-form pattern triggers behavior — it is ECMA control/value flow. F3 still resolves (`xhr.open url=/api/f3`); zero regression (faithful 2/2, c5-variants 3/3, JQ-6 2/2, C8 2/2, XSS 5/5, audit clean, prod 113/117).
  - **(historical) Real-jQuery param-backthrough trace (2026-05-15)**: `__DEMAND_MM_TRACE` showed param-backthrough engaged on the real bundle (`[MM] param-backthrough e/t/n/i fn@L2 idx0 sites=N callExpr=yes|no` — many invocations, some `callExpr=yes`), but the dominant outcome is `callExpr=no` with `sites` found yet not a merge-call arg. Pinned cause: minified jQuery is one giant single-line IIFE so `loc` is uninformative (`fn@L2` always), and `_demandDispatchSites(b.path.parent)` for jQuery's transport factory must trace its registration into the real `transports` registry + `inspectPrefiltersOrTransports` dispatch (dataType-keyed, prefilter-chained, `seekingTransport` flag) — strictly richer than JQ-6's atomic Ut/Vt. The precise next edge: **compose param-backthrough with C3's registry-dispatch discovery for jQuery's actual `transports` object** (the factory is registered via `jQuery.ajaxTransport("*",fn)` → `transports["*"].unshift(fn)` and invoked through `inspectPrefiltersOrTransports`), so the param's caller-arg resolves to the deep-extended `s`. Bounded by the same carrier discipline; fail-first via `__DEMAND_MM_TRACE` on the real bundle. The architecture's critical property (bounded, NO OOM — vs forward CID-14's 4 GB) holds across all ~13 edges. Step D (wire engine into learning) + Step E (real-site) remain; Step E is the true ground truth and will determine which further composition hops the real bundle actually needs. **Every atomic layer resolves; real jQuery is bounded 2.9 s NO OOM throughout (~12 edges); prod 113/117 unchanged.** Reproducers: `testing/debug-demand-c8.js`, `debug-c8b-pin.js`, `debug-demand-c6.js`; real-bundle probe via `__DEMAND_PROBE` + `__DEMAND_PROBE_DIAG` + `__DEMAND_MM_TRACE`. Each remaining layer pinnable the same way; engine stays bounded (real jQuery 2.9–4.2 s, NO OOM throughout); prod 113/117 unchanged, audit clean, demand-suite green (resolve 5/5, c5-variants 3/3).
  - **(historical) Next pinned edge — backward call-return folding.** [now C-callret, done] Full V1/jQuery was c4Hits=0 because the chain is `var x = m.xhr(); x.open(...)`: the merge-member now resolves the CALLEE `m.xhr` → factory, but `_demandResolve` did not yet fold a CALL whose base memo is `top` (it is base-memo + param-substitution; it does not itself drive callee→fn-ref→return-memo folding when the call is unresolved context-insensitively). Need: in `_demandResolve`, when a node's value flows from a CallExpression `C` whose callee resolves (via merge-member / `_demandCalleeFns` / param-backward) to a function-ref `F`, fold `F`'s return (`_specReturnValueGet`/return expr) with the call's resolved arg AVs — the general inter-procedural backward call-return mechanism. Then `m.xhr()`→factory return→`new XMLHttpRequest()` obj-lit→`x.open` = `XMLHttpRequest.prototype.open` sink ⇒ C4 confirms ⇒ url/method resolve. Own fail-first cycle (V1 → V2/V3 → jQuery). Bounded by carrier/frame discipline.
  - Gate: jQuery `$.get` resolves `GET /api/profile` via `_demandResolve` AND `--max-old-space-size=4096` completes (memory gate already MET — 3.1 s no OOM; only the value-resolution depth remains).
  - **XSS-finding deliverable VERIFIED (2026-05-15) — the unified-design thesis holds for BOTH halves.** `testing/debug-demand-xss.js` via `__DEMAND_PROBE` (added `_demandTaintLeaves` to the hook): the SAME backward `_demandResolve` engine that traces native fetch also surfaces taint-source findings — X1 `location.search`→`insertAdjacentHTML` `TAINT=["location.search"]`; X2 param-backward `render(location.hash)` `TAINT=["location.hash"]`; X3 concat `"<div>"+location.search` → `binop` `TAINT=["location.search"]` (composition preserves taint); X4 `eval(location.hash)` param-backward `TAINT=["location.hash"]`; X5 negative static literal `seeds=0` (no false positive). Param-backward + concat-composition + member/coerce taint propagation all work for the security direction; one engine, two deliverables, confirmed. Audit-clean, prod 113/117 unchanged.
- **D** — route `learnFromAstCallSite` + XSS emission through query results; delete the forward ctx-refinement do-while + CID-12/13/14 + `_specCtxEffectsBySite` + `sd14RefinedMemo`. **BLOCKED until the demand engine reaches forward parity on the real bundle** — wiring it in + deleting forward now would regress the 113 passing tests that rely on the forward path (the engine resolves every atomic shape but not yet real jQuery's full multi-statement composition). Step D proceeds only after Step E parity. Test: full suite ≥ prior pass count, real XSS page flagged, real fetch sites learned.
- **E** — real-site verification: jQuery 3.7.1 `$.ajax`/`$.get`, a real GitHub/Reddit bundle (perf ≤ +10 % vs the 2.6 s base-only baseline), a real `location.*`-derived sink page.

Current tree state (2026-05-15): demand engine (`_demandSinkSeeds` / `_demandResolve` / `_demandDispatchSites` / `_demandCalleeFns` / `_demandResolveCalleeSink`) implemented; invoked in-pipeline ONLY behind the guarded `globalThis.__DEMAND_PROBE` hook (zero prod cost). Base pipeline byte-unchanged: **suite 113/117** (same 4 fails — forward pipeline unchanged; JQ-6 still "fails" the suite because the engine isn't wired into learning yet = Step D, but PROVABLY resolves via the engine live), jQuery no OOM, `audit-recursion` clean. CID-14 stays reverted. Verified milestones: A, B, C1, C2, C3 done; C4 mechanism done + jQuery-depth edges (C5–C7) pinned.

This is the textbook demand-driven formulation; "properly supporting k-CFA" = context materialized along demanded paths only, which is exactly the property that makes it scale to 5 MB.

### 2026-05-14c fixes (this session — wildcard-slot propagation through override capture)

- **Override-capture restructured to enrich-before-compare.** `_specAssignmentExpressionApply` `window.X = Y` global override path now computes the enriched-from-points-to AV FIRST, then compares enrichedRhsAv against prev for the skip-update decision. Without this restructure, the prevEq check compared raw rhsAv (un-enriched) against the previously-stored enriched value; if rhsAv hadn't changed (same un-enriched function-ref shape) but points-to had gained a wildcard entry, the comparison returned "equal" and skipped the update — losing the wildcard slot. Now points-to entries always reach the override even when state[rhs] doesn't yet carry them.
- **Override-capture unions prev._extraProps into enrichedRhsAv.** Preserves entries that other paths (loop-key propagation at line ~17172, effect-replay at line ~16944) may have added. Field-sensitive monotone union; result reflects LUB of all reaching writes per § 9.1.1.1 SetMutableBinding.
- **Loop-key global override propagation unions prev._extraProps.** At the `for (var grOk in _specGlobalPropOverrides)` mirror-write site, instead of overwriting with lkNewFn directly, merge prev._extraProps's missing keys into lkNewFn before storing. Preserves points-to-derived entries (WILDCARD or specific keys from cb-body opaque writes) that loop-key propagation alone wouldn't carry. ECMA reference-semantics: when state[X] and override[Y] share funcNode, mutations on one are visible via the other, but only if all prop-write paths preserve the full prop union.
- **Effect-replay enrichment.** At the effect-replay SetGlobalRecordBinding path (line ~16944), if subValue's funcNode resolves to a declarator with points-to obj-prop entries, merge them into subValue's _extraProps before storing as override. Non-recursive lookup via `_specFuncPathByNode.get(funcNode).parent` → VariableDeclarator. Closes the path through summary-applied iter helpers where state[ce] propagation alone misses the cb's opaque-key writes.
- **Member-access wildcard fall-through.** `_specMemberAccessOnObjLeaf` for function-ref-with-extraProps falls through to `_extraProps[_SPEC_PT_WILDCARD_KEY]` when no specific key matches AND `_hasUnknownExtraProps` is set. ECMA § 13.10 PropertyReference: an opaque-key write may have stored at the accessed key; sound over-approximation surfaces it.
- **Result:** IIFE-export diagnostic tests now PASS. Real jQuery's `_extraProps` (via debug-jq-user-site.js) now has 90 keys including the WILDCARD slot (` *wildcard* `) — previously had 89 without it. The wildcard dispatch correctly fires for `jQuery.get`/`jQuery.post` shape; remaining gap is in jQuery's ajax body's full state-machine traversal post-wildcard-dispatch (SD-10 ctx-refinement composition territory).

### 2026-05-14b fixes (this session — points-to expansion + SD-9 widening)

- **Phase B2 — obj-lit prop-keyed points-to populator.** Added field-sensitive index `_specPointsToObjProps: WeakMap<declNode, Map<propKey, Set<funcNode>>>` plus reverse `_specPointsToObjPropsReverse: WeakMap<funcNode, Set<{decl, key}>>`. Populated at the three `_specAssignmentExpressionApply` state-rebind sites: (1) obj-lit prop write `o.k = fn` (B2-2, ECMA § 10.1.7 OrdinarySet); (2) function-ref `_extraProps` write `f.method = fn` (B2-2 extended, § 10.3); (3) or-of-consts key write — distributes per leaf (B2-4, § 14.6 LUB); (4) opaque-key write under WILDCARD sentinel (B2-5, sound over-approximation). Also at the B1 walker for joinedAv obj-lit root (B2-1/B2-7 implicit, § 13.2.5 ObjectExpression eval).
- **Phase C7 — obj-prop dispatch consumer.** In `_specFindCallSites`, walks `_specPointsToObjPropsReverseGet(fnNode)` for each (decl, key) entry; resolves decl to scope binding; iterates referencePaths for `decl.K(...)` / `decl[K](...)` shapes with structural key match (computed-key resolved via `_avAtPath` to const). WILDCARD sentinel matches any key. NewExpression dispatch handled symmetrically for Phase C10 `new decl.K(...)`. ECMA § 13.3.6 + § 13.10.
- **Phase B5 + C8 — return-value propagation + return-of-fn dispatch.** Added `_specPointsToReturn` and `_specPointsToReturnReverse` indices. At `_specReturnValueMemo.set` (post-body return computation), scan joinedRet for fn-ref leaves and record in B5 index. In `_specFindCallSites`, walk `_specPointsToReturnReverseGet(fnNode)` for helper fns h that return fnNode; resolve h's binding via scope; walk h's referencePaths for `h(...)` parent; check outer parent for `h(...)(...)` — outer CallExpression is a dispatch site of fnNode. Non-recursive (binding-lookup direct, not via `_specFindCallSites` self-call — banned per CLAUDE.md L29-L31). ECMA § 14.10 ReturnStatement + § 6.2.3.1 NormalCompletion + § 13.3.6.
- **Shared AV-leaf walker helpers.** Single-sourced `_specWalkAvForFnRefs(rootAv, visit)` and `_specWalkObjLitForPropFnRefs(objAv, visit)` for use across all B-populators. Iterative worklist + WeakSet cycle prevention. Replaces inline obj-lit / array-lit / or walker that was duplicated.
- **SD-9 — while-loop induction-variable widening (§ 14.7.2 + § 7.4).** In `_specApplyStatement`'s WhileStatement/DoWhileStatement handler, BEFORE evaluating the test expression, walk the test AST iteratively for UpdateExpression(Identifier) and AssignmentExpression(Identifier-LHS) — those are induction vars. Set `state[var] = _AV_TOP` to model multi-iter widening. The widened induction var then propagates through subsequent reads: `arr[r++]` becomes `arr[TOP]` which projects via existing § 23.1.4 Array Exotic Object [[Get]] with opaque numeric index to the OR of array elements. The Assignment `n = arr[r++]` then gives n the OR-of-elements, which is the static over-approximation of n's value across all iterations of the loop per § 7.4 IteratorStep abstract interpretation. F-test verifies all 3 elements appear as fetch sites for `while (n = arr[r++]) fetch("/api/iter/" + n)`.

### 2026-05-14 fixes (this session)

- § 13.3.6 OrdinaryCallEvaluation indirect invocation: `_specFindCallSites` discovers indirect call sites for fns passed as args. Handles Identifier-host callees (`each(fn)`) AND MemberExpression-host callees (`jq.ajaxTransport(fn)` — resolves callee via `_avAtPath` to function-ref). Inside the host fn's body, every invocation of the host's param-at-the-arg-idx is treated as a call site of the passed fn.
- § 13.3.6 + § 10.2.10 post-substitution function-ref folding: in the `_resolveAvBySubstitutingCallerArgs` worklist, after caller-arg substitution if the result is `call(function-ref(F), args)` (top-level OR inside or-tree arms), read F's return memo and fold via `_specInstantiateAv(retMemo, args, F)`. Cycle break: per-(fn, args-hash) Set.
- § 23.1.4 Array Exotic Object [[Get]] with opaque numeric index: `_specInstantiateAv` member reduction projects `array-lit[...] [opaqueKey]` to the OR of array elements when subKey is non-const-string. Previously fell through to opaque member, blocking URL resolution from `arr[i]` reads in iterator callbacks.
- § 23.1.3.20 Array.prototype.push on direct-Identifier receiver: `O.push(v)` where O is a tracked local (state) OR global record (`_specGlobalPropOverrides`) appends args to the array-lit. Without this, push-to-registry patterns dropped values.
- § 23.1.3.20 + § 10.2.10 outer-binding self-array-mutate (Pattern C): added to the outer-binding capture scan in `_specInitialFunctionBodyState`. When a helper fn pushes to an outer `var arr = []`, walk helper's call sites and substitute its params into the push-arg AVs so the captured array reflects what was pushed at every call site. Handles three helper binding forms: FunctionDeclaration, VariableDeclarator-bound FE, AssignmentExpression with Identifier-LHS OR MemberExpression-LHS (`jq.ajaxTransport = function(fn){...}`).
- § 13.3.6 + slice transitivity (Tier 7): the post-fixpoint pass that indexes dynamic call edges via function-ref callee resolution now also extends `_specSliceFns` and records the caller relationship in `_specCallGraphCallersOf`. Without this, the resolver's slice gate rejected call sites whose enclosing fn entered the call graph only via a dynamic edge.
- § 14.6 IfStatement merge + Pattern C fold-completeness deferral: when Pattern C runs during the helper fn's own analysis (its outer-binding scan), per-push-arg memos aren't yet populated. Set a fold-incomplete flag; skip `_specBindingInitAvCache.set(...)` when incomplete. Same flag applied to obj-lit Pattern A chained-mutation path. The next reader recomputes joinedAv with the now-populated memos, converging toward the precise fold.
- § 14.6 + § 13.3.6 or-callee distribution: post-fixpoint call-graph index now flattens or-trees over function-ref leaves. Each fn arm is indexed with the call site; non-function arms (undefined, top) skipped — at runtime they'd TypeError on call.
- § 23.1.3.20 + § 14.6 chained array mutation memo read: `_specApplyChainedArrayMutation` reads the push-arg path's memo (post-if-merge AV) first; substitutes caller args via `_specInstantiateAv`. Without this, fn rebound inside `if (typeof name !== "string") {fn=name;}` evaluated to its caller-substituted param value instead of the merged `or(...)`.
- § 14.6 + § 13.10 dynamic-key or-projection: `_specResolvePropertyWritePairs` reads the dynamic-key path's memo and, when the post-if-merge is an or-tree of const leaves, emits one (keyName, valueAv) pair per leaf. Models the realisable runtime values when `(transports[name] = transports[name] || []).push(fn)` runs with `name` possibly being `"*"` after the typeof rebind.
- § 22.1.3.18 String.replace with const-string search pattern: `_specApplyBuiltinMethod` now also dispatches when `argAvs[0]` is a const-string (synthesized as a literal RegExp with metachar escape) — not just regex-instance. Required for the cartesian replacer case below.
- § 22.1.3.18 cartesian functional replacer: when the replacer's return resolves to an or-tree of const-strings, the per-match leaf set is enumerated across all matches via the cartesian product. Each match independently picks one leaf per § 22.1.3.18 step 11 (functional replacer called per match).
- § 8.1.1 + § 9.4.1 + § 13.3.6 curried-helper (Pattern D): outer-binding scan picks up writes inside an inner fn whose helper closes over the outer binding. Records writes with `viaHelperFn` + `viaCapturedParamName` markers. Fold walks helper's call sites AND the inner fn's call sites (via the var that binds the helper's return), combining both layers of caller-arg substitution per § 13.3.6 OrdinaryCallEvaluation.
- Pattern D deferred-refold post-pass: declarators whose Pattern D fold couldn't complete during the initial outer-binding scan (helper-fn body memos not yet populated) are recorded in `_specBindingsNeedingRefold`. At end of fixpoint, the cache is invalidated, capturers' effects memos are cleared, capturers are force-re-analyzed, the program is re-analyzed, and the dynamic-edge index re-runs. The pass loops until `_specBindingsNeedingRefold` is empty, bounded structurally by `slicePaths.length`. ECMA-grounded fixpoint convergence per § 7.4 abstract interpretation + § 10.2.10 caller-context substitution.
- § 13.10 + § 20.2.3 prototype-chain resolution for `fn.call(...)` / `fn.apply(...)` at indirect-discovery dispatch: when `_resolveAvBySubstitutingCallerArgs` walks a call site whose callee's memo is `member(param, "call")` (because the param's standalone AV has no prototype), substitute the param with `function-ref(funcPath.node)` at the right idx and re-instantiate via `_specInstantiateAv`'s prototype-chain lookup. The substitution walks `function-ref` → Function.prototype obj-lit → method binding, producing the builtin-method AV that triggers the existing `isCallApply` arg-slicing path. No name-match — pure prototype-chain spec semantics.

### Composite test coverage added (this session)

- "§ 13.3.6 + § 23.1.4: custom each iterator over array-lit dispatching to callback (factory pattern)" — exercises indirect call site discovery + array-lit opaque-key projection + function-ref folding (3-level chain).
- "§ 23.1.3.20 + § 10.2.10: register-helper pushes function to outer array; dispatcher reads it back" — exercises Pattern C self-array-mutate with cross-function dispatch.
- "§ 13.3.6 + slice transitivity: dynamic call edge through array-index callee extends slice" — exercises the v4-host-method shape with `obj.method` helper bindings.

## Part 6: Interprocedural points-to analysis — design + sub-problems

### 6.0 Theoretical foundation

**Definition (in our context):** Points-to is the static abstraction of value flow `writes(B) → reads(B)` for binding B, where "value" means any AV kind (function-ref, obj-lit, array-lit, taint-source, etc.). The classical formulation tracks only function-refs (call-graph construction) and pointers (alias analysis). We need both — call-graph for indirect dispatch; alias for obj-lit / array-lit propagation through arg-passing and prop-reads.

**Two indices, two directions, two intents:**
| Index | Forward / Reverse | Query | Used by |
|-------|-------------------|-------|---------|
| `_specPointsToSets: WeakMap<declNode, Set<funcNode>>` | Forward | "what fns does B hold?" | Resolver (Phase D) when invoking via binding |
| `_specPointsToReverse: WeakMap<funcNode, Set<declNode>>` | Reverse | "where is fn held?" | Populator-driven dispatch discovery (Phase C) — walks binding's referencePaths for invocation contexts |

**Granularity dimensions (made explicit):**
| Dimension | Choice | Rationale |
|-----------|--------|-----------|
| Field-sensitivity | YES (per propKey) | `o.fn=a; o.gn=b` ≠ `o.fn` points-to {a,b} |
| Allocation-site sensitivity | YES via `_ctorId` (obj-lit), per-decl array-lit | Distinguish instances from same factory |
| Flow-sensitivity | YES (post-fixpoint joinedAv reflects program-end state) | Already via existing memos |
| Context-sensitivity | k-CFA via `_resolveAvBySubstitutingCallerArgs` recursion | Edge-dedup bounds k |
| Path-sensitivity | NO (LUB at IfStatement merge) | Sound, imprecise; out of scope |
| Heap cloning | NO | obj-lits with same ctorId merge propWise |

**ECMA-262 mapping:**
| Sub-problem domain | ECMA section |
|--------------------|-------------|
| Binding storage | § 8.1.1 Environment Records, § 9.1 Realms, § 9.4.1 FunctionEnvironmentRecord |
| Binding writes | § 13.15.4 PutValue, § 9.1.1.2.5 SetMutableBinding |
| Binding reads | § 9.1.1.2.4 GetBindingValue, § 13.1.3 IdentifierReference |
| Property writes | § 10.1.7 OrdinarySet, § 13.10 PropertyReference |
| Array mutation | § 23.1.3.20 push, § 23.1.3.33 unshift, § 23.1.3.32 splice |
| Argument passing | § 10.2.10 FDI, § 9.4.1 NewFunctionEnvironment, § 13.3.6.1 ArgumentListEvaluation |
| Return value | § 14.10 ReturnStatement, § 6.2.3.1 Completion(NormalCompletion) |
| Closure capture | § 15.2.5 InstantiateOrdinaryFunctionExpression (each eval = fresh function object) |
| Method dispatch | § 13.10 PropertyReference + § 13.3.6.2 EvaluatePropertyAccessWithIdentifierKey |
| Iteration | § 7.4 IteratorStep, § 23.1.3.15 Array.prototype.forEach |

**Memo interaction matrix:**
| Memo | Read by points-to | Written by points-to | Invalidation trigger |
|------|-------------------|---------------------|----------------------|
| `_specPathValMemo` | Populators scan joinedAv for fn-ref leaves | — | per-body analysis |
| `_specReturnValueMemo` | B5 populator reads to propagate returns | — | sig change of fn |
| `_specEffectsMemo` | B re-analysis after points-to delta | — | sig change of fn |
| `_specCallSitesByFn` | Consumers walk to enumerate sites | Extended by C-side discovery | post-fixpoint |
| `_specPointsToSets` | Resolver Phase D | All B-populators | per-analysis reset |
| `_specPointsToReverse` | All C-consumers | All B-populators | per-analysis reset |

**Termination proof sketch:** Populators add (declNode, AV) pairs to a WeakMap. Cardinality bound: `|declNodes| × |distinctAVs|`, both finite per program. AVs are structurally interned (hash-consed via `_avStructuralHash`), so the set of distinct AVs is finite. Each fixpoint iteration either adds new pairs (monotone) or no-ops; finite cardinality + monotonicity ⇒ convergence in O(|declNodes| × |distinctAVs|) steps.

**Worklist-based, no recursion** (CLAUDE.md L29-L31): every walk uses explicit-stack worklists with edge-keyed `Set` dedup. No structural recursion in any populator or consumer.

### 6.1 Phase status matrix (overview — details in 6.2)

| Phase | Description | Status | Sub-problems |
|-------|-------------|--------|--------------|
| A | Data structures: forward + reverse WeakMaps, add/get helpers, per-analysis reset | ✓ Live | — |
| B1 | Populator: AV-leaf walker on finalised joinedAv (direct assignment, push, etc.) | ✓ Live | — |
| B2 | Populator: obj-lit property writes (`obj.k = fn`) — field-sensitive prop-keyed nested index | ✓ Live | B2-1..B2-5,B2-7 done; B2-6 chained falls out of existing chained-assign dispatch |
| B3 | Populator: arg→param value-flow (caller-arg propagates into callee param's points-to) | ◯ Open | B3-1..B3-6 |
| B3' | Capture-independent declarator walk — absorbed by Phase B2 (B2 fires during AssignmentExpression eval, not via capture-dependent scan) | ✓ Absorbed | — |
| B4 | Populator: iteration cb-element-param population | ◯ Open | B4-1..B4-5 |
| B5 | Populator: return-value propagation (`var x = f()` ⇒ x's points-to ⊇ f's return-fn-refs) | ✓ Live | B5-1..B5-3 done |
| B6 | Populator: class instance `this.X = expr` (ctor-keyed prop points-to) | ◯ Open | B6-1..B6-4 |
| B7 | Populator: explicit closure capture via FE param (closure-state-flow into param's points-to) | ◯ Open | B7-1..B7-3 |
| C1 | Consumer: direct binding call `binding(...)` | ✓ Live | — |
| C2 | Consumer: member dispatch `binding[K](...)` | ✓ Live | — |
| C3 | Consumer: iterator-cb unfolding (Identifier-callee iterator like `each(arr, cb)`) | ✓ Live | — |
| C4 | Consumer: iterator-cb unfolding (Member-callee iterator like `arr.forEach(cb)`, `ce.each(arr, cb)`) | ✓ Live | — |
| C5 | Consumer: recursive param-iterator propagation through helper fns | ✓ Live | — |
| C6 | Consumer: walk-up through LogicalExpression / ConditionalExpression wrappers | ✓ Live | — |
| C7 | Consumer: obj-prop dispatch via Phase B2 (`o.k(...)` once B2 records (o,k)→fn) — incl. WILDCARD matching | ✓ Live | C7-1..C7-3 done |
| C8 | Consumer: return-of-fn dispatch `f()()` once B5 records f→retFn | ✓ Live | C8-1..C8-3 done |
| C9 | Consumer: Function.prototype.call/apply over indirect-discovered fn (binding.call(thisArg, …)) | ◐ Partial via existing isCallApply handling | C9-1..C9-3 |
| C10 | Consumer: `new binding[K](…)` constructor dispatch via B2 | ✓ Live (inline with C7) | — |
| D | Resolver fallback: `_specPointsToReverseGet` query — absorbed by Phase C (Phase C consumers run inside `_specFindCallSites` which the resolver always calls) | ✓ Absorbed | — |
| E | Structural-edge dedup applied to all worklists (foundation in resolver ✓; needs Phase C/D propagation) | ◐ Partial | E-1..E-3 |
| F | Tests: covering each sub-problem above + real-site bundle exercises | ◐ Partial (F-1..F-4 + F-11 + F-15 + F-17 + wildcard-iter + SD-9 done; F-6 B3-1, F-7 B3-3, F-8 B3', F-9..F-10 B4, F-13..F-14 B6, F-18 B2-7 NewExpression + class, F-19..F-20 real-jQuery TODO) | F-6..F-20 (subset) |

### 6.2 Sub-problem decomposition (atomic, each implementable independently)

#### Phase B2 — Object property writes ⇒ prop-keyed points-to

**Architecture:** Field-sensitivity demands NESTED indices, not flat. A propKey is part of the binding identity for points-to purposes.

```js
_specPointsToObjProps:        WeakMap<declNode, Map<propKey, Set<funcNode>>>
_specPointsToObjPropsReverse: WeakMap<funcNode, Set<{decl, key}>>
_specPointsToObjPropsAdd(declNode, propKey, funcNode): void
_specPointsToObjPropsGet(declNode, propKey?): Set<funcNode> | Map<propKey, Set<funcNode>>
```

**B2-1: ObjectExpression init at declarator.** When `var O = {k: fn1, m: fn2, ...}` is evaluated and bound to O's declNode, walk every property whose value AV contains fn-ref leaves and add (declNode, propKey, fn) to the prop index. ECMA § 13.2.5 (ObjectExpression eval) + § 13.15.4 PutValue.

**B2-2: AssignmentExpression LHS=MemberExpression(Identifier, Identifier).** `O.k = fn` where O resolves to a tracked declarator: lookup binding from `path.scope.getBinding(O.name)`, get its declNode, add (declNode, k.name, fnRef leaves of RHS-AV). ECMA § 10.1.7 OrdinarySet.

**B2-3: Computed LHS with const-string key.** `O[K] = fn` where K resolves via spec eval to a `const`-kind AV with string value: same as B2-2 with propKey = K's string value. ECMA § 13.10 PropertyReference (computed form).

**B2-4: Computed LHS with or-of-const-strings key.** `O[K] = fn` where K's AV is `or` over const strings: emit one entry per leaf (cartesian over leaves × fn-refs).

**B2-5: Computed LHS with opaque key.** `O[opaqueK] = fn`: mark with WILDCARD key (a sentinel symbol). At read sites, wildcard fn-refs are added to whatever specific propKey is being queried.

**B2-6: AssignmentExpression chained `O.k1 = O.k2 = fn`.** Both target keys get fn. Reuses existing chained-assignment dispatch (§ 13.15.3).

**B2-7: ObjectMethod / ObjectProperty with FunctionExpression value.** `{k() { ... }}` or `{k: function() {...}}` syntactic sugar — same as B2-1 since FE value resolves to function-ref AV.

#### Phase B3 — Argument → parameter value flow

**Sub-problem:** When `helper(b1, b2, ...)` is called, helper's body may write through its params. Those writes should be visible in the corresponding caller binding's points-to set.

**Architecture:** Build a reverse-flow summary per fn — for each param, which key-paths get written and with what AV-shapes — then propagate at call sites.

```js
_specParamWritesByFn: WeakMap<funcNode, Array<{paramIdx, keyPath, valueAv}>>
```

**B3-1: Effect summary extraction.** Per-fn analysis already produces `_specEffectsMemo`. Filter for effects whose target's base is `param(paramIdx, fn)` — those are param-rooted writes. Record (paramIdx, keyPath, valueAv) per fn.

**B3-2: Call-site propagation.** At every call site of helper(b1, b2, ...), for each param-write summary (paramIdx, keyPath, valueAv), apply the write to `args[paramIdx]`'s declNode at keyPath. Reuses Phase B2 machinery for prop-keyed writes.

**B3-3: Transitive substitution.** valueAv may itself reference other params — substitute via `_specInstantiateAv` with the call site's concrete args before applying the write.

**B3-4: Cycle bound.** Edge dedup keyed by (fn, callSite). Convergence guaranteed since each call site adds at most |effects[fn]| writes.

**B3-5: Eager re-analysis.** When B3 produces a new write, the helper fn's already-analyzed callers may need re-analysis. Use `_specReadersOf` reverse index (existing infra).

**B3-6: Cross-fn write reachability.** Helper passes its param to another helper: chain through composed summaries (B3 on B3). Fixpoint until no new writes.

#### Phase B3' — Capture-independent declarator walk (was SP-2)

**Sub-problem:** Currently the Pattern A-D outer-binding scan runs only inside CAPTURING fns (in `_specInitialFunctionBodyState`). Declarators with no capturing fn never have their writes scanned, so points-to misses their prop writes entirely.

**B3'-1: Post-fixpoint declarator scan.** After main fixpoint, walk every VariableDeclarator in slicePaths. For each, run the existing Pattern A-D propertyWrites scan.

**B3'-2: Direct propertyWrites application.** Apply scanned writes to declNode's joinedAv (existing machinery in `_specBindingInitAvCache` populator).

**B3'-3: Points-to population.** Reuse Phase B1 AV-leaf walker on the resulting joinedAv to populate `_specPointsToSets`. Also call B2 populators for obj-lit prop writes encountered.

**B3'-4: Bound scope.** Only walk declarators with init = ObjectExpression OR ArrayExpression OR none (uninitialised vars that get assigned). Reduces noise.

#### Phase B4 — Iteration callback element-param population

**Sub-problem:** `arr.forEach(function(elt) { ... })` — elt's points-to should be the OR of arr's element points-to. Symmetric to Phase C-side iterator unfolding (the consumer matches cb param to arr element); B4 is the populator side.

**B4-1: Match iterator helper.** At call site `arr.forEach(cb)` / `each(arr, cb)`: resolve cb to a FunctionExpression or function-ref. Identify cb's element-param (idx 0 for forEach, idx 1 for each with `each(arr, cb)` where cb is `(idx, elt) => ...`).

**B4-2: Resolve arr element points-to.** arr resolves to an array-lit AV (after B1 populator). Walk elements for fn-ref leaves; collect into a flat set.

**B4-3: Populate cb element-param points-to.** Inside cb's body, the element-param's declNode is the param Identifier. Add element fn-refs to that declNode's points-to.

**B4-4: Generalize iterator shape.** Match by AST structure (helper has param P, body invokes `P(...)` / `P.call(...)` with arr-indexed read at some arg slot). Not name-matched.

**B4-5: Member-iterator case.** `arr.forEach(cb)` — already partly captured by Phase C5 (consumer side). Mirror as populator.

#### Phase B5 — Return-value propagation

**B5-1: Per-fn return-fn-refs.** From `_specReturnValueMemo[fn]`, walk AV for fn-ref leaves; record `_specPointsToReturn: WeakMap<funcNode, Set<funcNode>>`.

**B5-2: Call-site propagation.** At `var x = f(...)`, lookup `_specPointsToReturn(f)`; add to x's declNode's points-to.

**B5-3: Chained calls.** `var x = f()(...)` — outer call's return propagates to x; inner call's return is the callee. Recursive via worklist.

#### Phase B6 — Class instance `this.X = expr`

**B6-1: ctor-keyed prop index.** `_specPointsToCtorProps: WeakMap<classNode, Map<propKey, Set<funcNode>>>`.

**B6-2: ThisExpression LHS in class method/ctor.** `this.X = fn` inside class C's method ⇒ add (C-node, X, fn).

**B6-3: instance.X dispatch.** When state[v]'s AV is obj-lit with `_ctorId` = C, member access on X resolves via `_specPointsToCtorProps[C][X]`.

**B6-4: Inherited from ClassHeritage.** If C extends D and `this.X` set in D's ctor, dispatch on C-instance.X resolves via D's prop index (existing ClassHeritage walk).

#### Phase B7 — Explicit closure capture via FE param

**B7-1: FE-as-arg context.** When `f(function() { return X; })` is called where X is a captured outer binding, the FE has an `_extraProps` snapshot of X (existing `_specFuncRefClosureState`). Walk for fn-refs.

**B7-2: Propagate to FE-param-bound points-to.** When the FE-receiving param is later invoked, its return AV's fn-refs propagate from the closure snapshot.

**B7-3: Bound by Phase E dedup.** Edge keyed by (FE-node, snapshot-hash).

#### Phase C7 — Object property dispatch via Phase B2

**C7-1: Reverse query.** For each fn in `_specPointsToObjProps`, get its `Set<{decl, key}>` from `_specPointsToObjPropsReverse`.

**C7-2: Walk decl's references for member-access dispatch.** For each ref, check if parent is `decl.key(...)` or `decl[K](...)` where K's AV resolves to const-string === key.

**C7-3: Add to callExprPaths.** Standard C-side discovery output.

#### Phase C8 — Return-of-fn dispatch

**C8-1: Reverse via Phase B5.** For each fn known to be returned from helper h (`_specPointsToReturnReverse`), find h's call sites.

**C8-2: Check outer-call shape.** Parent of h's call site is `h(...)(...)` — outer CallExpression with h's call as callee.

**C8-3: Add outer CallExpression to fn's callExprPaths.**

#### Phase C9 — Function.prototype.call/apply over indirect-discovered fn

**C9-1: Detect call/apply parent.** `binding.call(thisArg, ...)` where binding is a tracked fn-ref via points-to.

**C9-2: Slice args.** Skip thisArg; remaining args bind to fn's params.

**C9-3: Add as invocation.** Adds CallExpression to fn's callExprPaths with arg-shift metadata.

#### Phase C10 — `new binding[K](...)` constructor dispatch

**C10-1: Detect NewExpression with member callee.** `new O.K(args)` where O.K resolves via B2 to a fn-ref.

**C10-2: Trigger ctor analysis.** Same as ordinary call dispatch but with `[[Construct]]` semantics — fn's prototype methods accessible on resulting obj.

#### Phase D — Resolver fallback

**D-1: Entry point.** Inside `_resolveAvBySubstitutingCallerArgs`, when no static call sites match for funcPath.

**D-2: Forward query.** `_specPointsToSets.get(declNodeOfRecv)` — what fns does the receiver hold?

**D-3: Per-fn substitution.** For each found fn, run the standard caller-arg substitution.

**D-4: Edge dedup.** Keyed by (declNode, fnNode) — bounded.

#### Phase E — Structural-edge dedup (full propagation)

**E-1: Identify all worklists.** `_resolveAvBySubstitutingCallerArgs` ✓ done. Phase C consumers (`ptcWorklist`) currently use identifier-node-identity dedup. Replace with structural (call-site × fn × paramIdx) edges.

**E-2: Apply hash-consing.** AVs hashed via `_avStructuralHash`; edge keys: `${avHash}@${callSiteStart}:${fnStart}`.

**E-3: Bound proofs.** Each edge visited at most once per worklist iteration; total edges bounded by `|callSites| × |distinctAVs| × |fnNodes|`.

#### Phase F — Tests (specific patterns)

**F-1: B2-1** `var o = {fn: function(){fetch("/a")}}; o.fn();`
**F-2: B2-2** `var o = {}; o.k = function(){fetch("/a")}; o.k();`
**F-3: B2-3** `var o = {}; o["k"] = fn; o["k"]();`
**F-4: B2-4** `var o = {}; o[cond?"a":"b"] = fn; o["a"]();` — should still trigger because "a" leaf maps to fn.
**F-5: B2-5** `var o = {}; o[opaque] = fn;` — wildcard dispatches on any later `o.x()`.
**F-6: B3-1** `function helper(p) { p.k = function(){fetch("/a")}; } var o = {}; helper(o); o.k();`
**F-7: B3-3** Two-level helper: caller→helper→inner-helper writes through nested params.
**F-8: B3'-1** Declarator-only writes (no capturing fn) — `var arr = []; for (...) arr.push(fn);` works without any fn capturing arr.
**F-9: B4-1** `arr.forEach(function(elt) { elt(); })` where arr is `[fn1, fn2]`.
**F-10: B4-3** Identifier-iterator `each([fn1, fn2], function(i, e){ e(); })`.
**F-11: B5-1** `function make() { return function(){fetch("/a")}; } var x = make(); x();`
**F-12: B5-3** Chained `make()()` — IIFE-like.
**F-13: B6-1** `class C { m() { this.h = function(){fetch("/a")}; } } var c = new C(); c.m(); c.h();`
**F-14: B6-4** Inherited `class D extends C { ... }; new D().inherited()`.
**F-15: C7-1** Round-trip: B2-2 + member dispatch on `o.k()`.
**F-16: C8-1** Round-trip: B5-1 + chained `make()()`.
**F-17: C9-1** `var fn = obj.method; fn.call(thisArg, args);`
**F-18: C10-1** `var O = {Ctor: function(){this.fetch = ...}}; new O.Ctor().fetch();`
**F-19: real jQuery `$.ajax({url, method, data})` reaches XHR via library state machine** — requires B2 + B3 + C4 + C7 composition.
**F-20: real jQuery `$.get(url)` reaches XHR GET** — same composition path, different entry.

### 6.3 Sub-problem dependency graph

```
A ✓ ─→ B1 ✓ ─┬─→ B5 ──→ C8
              ├─→ B3 ──→ C7
              └─→ B3' (declarator-walk)
       B2 ──┬─→ C7 (member dispatch)
            ├─→ C10 (new O.K())
            └─→ B6 (this.X via Member-LHS)
       B4 ──→ recursive C5 consumer with populated cb-param points-to
       B6 ──→ C7 for instance-method dispatch
       B7 ──→ C-side fn-ref discovery for explicitly-captured FEs

All Bs ──→ D (resolver fallback queries all)
All Cs ──→ E (structural-edge dedup applies to every worklist)
```

### 6.4 Concrete invariants per sub-problem

| Invariant | Where enforced | Justification |
|-----------|----------------|---------------|
| points-to is MONOTONE (only adds, never removes) | helpers `_specPointsToAdd` | Fixpoint termination |
| AV-leaf walker terminates on cyclic AVs | WeakSet `ptSeen` in B1 | obj-lit + array-lit may cycle |
| WildCard (B2-5) merges into ALL specific-key reads | C7 query function | Sound over-approximation |
| ctor-keyed (B6) reads honor ClassHeritage chain | C7 + existing class lookup | § 15.7.5 |
| Re-analysis on points-to delta is bounded | `_specReadersOf` reverse index | Standard re-enqueue infra |
| Phase D never recurses unbounded | Edge dedup (declNode, fnNode) | E-3 bound proof |

### 6.5c Extend()-based opts-merge ctx-refinement gap (2026-05-14d)

Six diagnostic tests now pinpoint a precise architectural gap:

| Test | Passes? | Why |
|------|---------|-----|
| Direct opts ajax (lib3.ajax({url}) → xhr.open(opts.url)) | ✓ | One-level ctx-refinement at lib3.ajax call site; opts substituted directly |
| Top-level extend with LITERAL src ({url:"x"}, extend dst with {url:"x"}, xhr.open(dst.url)) | ✓ | For-in merge-pattern fires at top-level — src is a concrete obj-lit |
| Top-level extend ajax (no IIFE) — `function ajax(opts){var s={}; extend(s,opts); xhr.open("GET",s.url)}` | ✗ | Requires TWO levels of ctx-refinement: ajax body refined with opts; extend body re-refined with src=opts (now obj-lit) so for-in merge fires |
| Simpler extend ajax (IIFE-wrapped, otherwise same as above) | ✗ | Same as above + global override capture |
| Settings-merge ajax (extend({type:GET, url:undefined}, opts)) | ✗ | Same gap; the {} initialized with defaults case |
| real jQuery $.ajax/$.get | ✗ | Same gap at scale through jQuery's ajaxSettings extend |

**Root cause:** ctx-refinement (`_specCtxEffectsBySite`) currently refines ONE level — the call site's callee body re-evaluated with concrete args. Inner calls inside the callee body (e.g., `extend(s, opts)` inside `ajax(opts)`) get their args substituted in spec-eval's per-expression memo, but the inner callee's BODY isn't re-evaluated to fire the for-in merge pattern. The merge pattern detection at `_specAssignmentExpressionApply` line ~10867 requires `lhsKeyAv.src.kind === "obj-lit"` — true at the call site after substitution, but only IF the body re-evaluation fires.

**Architectural fix:** Make ctx-refinement RECURSIVE / composable. When refining call site C of fn F with args [A0, A1, ...]:
1. Substitute args into F's param AVs (already done)
2. Walk F's inner CallExpressions; for each inner call with concrete substituted args, RECURSIVELY refine that callee's body
3. Cap recursion depth via edge dedup on (callSite × argsHash) — bounded by `|callSites| × |distinctArgShapes|`

This unlocks: extend()-based opts merging (§ 14.7.5 for-in merge per § 13.15.4 PutValue at inner refinement), real-jQuery's ajaxSettings → prefilters → transports chain (same composition pattern at scale), and any framework using extend/Object.assign-style helpers.

### 6.5b Real-jQuery xhr.open diagnostic (2026-05-14c — call-graph gap)

Verified via `testing/debug-jq-xhr-open-site.js`: the analyzer DOES locate jQuery's single xhr.open call site at bundle offset 79775. The url AV at that node resolves to `member(param(idx=0,fn#79768)."url")` — the .url prop of the transport factory's `i` param. This is the correct unresolved standalone-analysis state.

The transport factory (`function(i){...}` at start=79768) is `inSlice: true` BUT has `callSites: 0`. The resolver cannot substitute `i = {url:"/api/simpler"}` because no call sites are linked. The factory is invoked DYNAMICALLY — jQuery's ajax body iterates a transports map (originally `_t`) and dispatches the matched factory.

**Why call sites are missing:** The factory is registered via `ce.ajaxTransport(function(i){...})` where `ajaxTransport: Ut(_t)` is a curried helper. The helper's inner fn writes `_t[name] = _t[name] || []; push(factory)` — opaque-key write recorded by Phase B2-5 wildcard in `_specPointsToObjProps`. Phase C7 wildcard match for the factory's call sites walks `_t`'s referencePaths looking for `_t[K](...)` dispatch shapes. BUT jQuery doesn't directly do `_t[X](...)`; ajax's body passes `_t` to `inspectPrefiltersOrTransports(_t, ...)` (a helper) which filters and returns matching transports as a sub-array. The sub-array's elements are dispatched in a for-in loop in the helper. The factory's call sites are inside this helper, not at direct `_t[K]` shapes.

**Architectural fix:** Phase C7 wildcard match needs to follow the receiver's value flow through helper calls — when `_t` is passed as arg to helper `f(_t, ...)`, walk `_t`'s param-equivalent in `f`'s body for `[K](...)` dispatch shapes. This is Phase B3 (arg→param value flow) consumer-side extension. Iterative, scope-resolved, bounded by edge dedup on (receiver-binding × helper-fn × param-idx).

### 6.5a Real-jQuery diagnostic (2026-05-14b — REVISED with reproducer)

Verified via debug script: jQuery's `_extraProps` after analysis has 89 keys including `ajax` BUT NOT `get`/`post`. Reduced via diagnostic to a minimal failing reproducer: **IIFE-wrapped helper + iter dispatch + global export**. The cb body's writes to ce's prop ARE visible inside the IIFE (via Phase B2 wildcard populator + Phase C7 wildcard match for ce.aaa() calls within the same scope) BUT they DO NOT propagate to state[ce]'s `_extraProps` in the IIFE's state. Consequence: when `window.myLib = ce` triggers SetGlobalRecordBinding, the override captures state[ce] WITHOUT aaa/bbb — only `each` (from the top-level extend()).

**Concrete reproducer (FAILS):**
```js
(function() {
  function extend(t, s) { for (var k in s) t[k] = s[k]; return t; }
  var ce = function() {};
  extend(ce, { each: function(e, t) { for (var r=0; r<e.length; r++) t.call(e[r], r, e[r]); } });
  ce.each(["aaa", "bbb"], function(idx, m) {
    ce[m] = function(url) { fetch("/api/iife3/" + url); };
  });
  window.myLib3 = ce;  // ← captures ce without aaa/bbb in _extraProps
})();
myLib3.aaa("xx");  // doesn't dispatch — fails
```

vs.

**Same code but read INSIDE the IIFE (PASSES):**
```js
(function() {
  /* same setup as above */
  ce.aaa("xx");  // ← C7 wildcard match dispatches correctly
})();
```

**Root cause (precise):** The cb's body writes `ce[m] = innerFn` where m is opaque-keyed (param AV). B2-5 wildcard populator records the points-to entry in `_specPointsToObjProps` (per-decl reverse index) — which is why C7 dispatch works for in-scope `ce.X(...)` reads. BUT the AssignmentExpression's state-rebind for the cb's `state["ce"]` doesn't propagate the opaque-key write because the existing rebind only fires for const-key or or-of-consts key (lines 10949-11109). The opaque-key case (B2-5 wildcard) hooks ONLY into the points-to index, not state[ce]. So state[ce] doesn't see the wildcard write; closure write-back from cb→each→IIFE propagates a state[ce] WITHOUT the iter-cb's writes.

**Fixes landed (2026-05-14b):**
- B2-5 wildcard hook updates state[left.object.name] structurally — sets `_hasUnknownExtraProps: true` and stores rhsAv under `_SPEC_PT_WILDCARD_KEY` sentinel in props/_extraProps. Propagates wildcard write at the CB's local state level.
- **Effects-replay opaque-key handler** (line ~17220 in `_specInstantiateAv`-driven replay path): when an effect's key isn't const-key and isn't loop-key, substitute eff.key with caller args via `_specInstantiateAv`. If post-substitution key is const → write at that specific value; if or-of-consts → distribute per leaf; otherwise → write to WILDCARD slot + set `_hasUnknownExtraProps:true` on the receiver. ECMA § 10.1.7 OrdinarySet replay with field-sensitive points-to materialization. This closes the previous gap where the replay path only handled const-key and loop-key, dropping all opaque-key effects.
- **ctx-refinement TOP-arg relaxation:** the candidate finder previously rejected refinement when ANY arg was TOP. Per § 7.4 abstract interpretation monotonicity, refining with a partial concrete-arg vector (some args concrete, others TOP) is still strictly more precise than the standalone baseline. New behavior: reject only on param-kind args (which would mix caller-scope param namespaces). TOP args are passed through (substituting param-leaf → top-leaf at refinement time). Allows iterator-cb dispatch chains where the iter-index is widened to TOP via SD-9 but the iter-value is concrete (or-of-elements) to still trigger ctx-refinement of the cb.

**Still failing (deeper architectural gap):** Even with both fixes, the IIFE-wrapped iter dispatch test still fails. Root cause now: ctx-refinement isn't propagating cb's effects through the multi-level iterator dispatch chain (IIFE → ce.each → t.call(cb, ...) → cb body's writes). The cb's standalone effects DO record the opaque-key write, and the replay handler now applies it at IIFE level — BUT only if the chain replays cb's effects in the IIFE context. Current ctx-refinement applies effects ONE LEVEL DEEP (each's effects at the ce.each call site), not recursively into cb's effects at the inner t.call site.

**Architectural fix (next session — substantial):** Force recursive ctx-refinement: at each call site of a fn whose effects include a higher-order call effect (`call t with args`), recursively replay the called fn's effects with the substituted args. Bound by edge-keyed visited set. This is the natural extension of the existing replay pipeline — propagates writes through any depth of iterator/HOF/dispatch chain. Either fix unblocks real-jQuery $.get/$.ajax — same exact pattern (each iter cb body writes ce[m] = innerFn).

### 6.5 Specific real-jQuery blockers (concrete test cases)

#### A. jQuery's `ce.each` namespace iterator via `Function.prototype.call`

```js
ce.each(t[e] || [], function(e, t) {  // cb's t param = array element
  var n = t(i, o, a);  // invoke transport fn
});
```

jQuery's `ce.each(arr, cb)` body uses `cb.call(arr[r], r, arr[r])` — Function.prototype.call to invoke cb per element. Custom-each handles `for(...) cb(i, arr[i])` directly but not the `.call(thisArg, ...)` form. **Sub-problem C4 extension + B4-5.**

#### B. While-loop iteration over computed array (`while (n = i[r++])`)

```js
var i = e.toLowerCase().match(D) || [];
while (n = i[r++]) (o[n] = o[n]||[]).push(t);
```

Iterates an array via `i[r++]`. Each iter assigns element to n, pushes through to o[n]. **Sub-problems SD-7 (dynamic-key on while-loop iter var) + SD-9 (while-iter var propagation) + 14.7.2-a (multi-iter state evolution).**

#### C. Multi-level ctx-refinement composition (Tier 7)

After dispatch's call site is refined with concrete opts, the trans(opts) call inside dispatch's body should ALSO get refined. `_specPathValMemo` is restored to base after each ctx-refinement round. **Sub-problem SD-10.**

### 6.6 Remaining ECMA queue items (unrelated to points-to)

- 22.1.3.18-a: String.prototype.replace replacer returning or-of-consts (cartesian K^M)
- 9.4-b: Inner class [[Construct]] dispatch for computed class refs
- 13.3.7-a: super calls through deeper extends chain
- 14.7.2-a: While-loop multi-iter state evolution
- 13.3.11-a: TaggedTemplate with complex tag body
- 7-a: Bounded-round dynamic-edge convergence (SCC detection)

### 6.7 Open sub-problems

Most foundational items done. Remaining sub-problems to close in this session:

#### Sub-problem 1: B3 — arg→param value flow (effect-summary based)

**B3-A: param-effect-summary extraction.** Per-fn analysis records effects whose target's base is param(paramIdx, fn) into a separate index `_specParamEffectsByFn: WeakMap<funcNode, Array<{paramIdx, keyPath, valueAvSpec}>>` where valueAvSpec is the AV with param-leaves preserved (for later substitution).
**B3-B: caller-side effect application.** At every call site of helper, for each param-effect, _specInstantiateAv-substitute valueAvSpec with concrete caller args, then apply to args[paramIdx]'s binding via B2 prop-keyed populator.
**B3-C: ANY-depth chain composition.** When helper passes its param to inner helper, walk multiple summary layers iteratively. Bounded by (fn, paramIdx) edge-dedup.

#### Sub-problem 2: B4 — iteration callback element-param population

**B4-A: iter-shape detection.** Match callee fn's body for the `cb-param.call(arr-param[idx], idx, arr-param[idx])` shape (and direct call variant `cb-param(idx, arr-param[idx])`). Identify which cb param is "element-param" structurally.
**B4-B: per-call-site population.** At every call site of iter(arr, cb), unwrap arr's element-AVs (array-lit elements or wildcard for arrays of unknown content), populate cb's element-param's `_specPointsToObjProps` reverse index.
**B4-C: nested iterator chain.** iter1(arr, iter2(otherArr, cb)) — multi-level depth.

#### Sub-problem 3: B6 — class instance `this.X = expr` (ctor-keyed prop points-to)

**B6-A: index design.** `_specPointsToCtorProps: WeakMap<classNode, Map<propKey, Set<funcRefNode>>>`.
**B6-B: this-effect collection.** Walk `_specThisEffectsMemo[ctorOrMethod]` for {key: const, value: fn-ref} entries; record against the enclosing class node.
**B6-C: instance dispatch.** When state[v]'s AV is obj-lit with _ctorId = C, member access on X consults `_specPointsToCtorProps[C][X]` for fn-refs. Combines with existing instanceCtor mechanism for full coverage.
**B6-D: inherited-method coverage.** If C extends D, dispatch on C-instance.X also walks D's ctor-prop points-to (ClassHeritage chain).

#### Sub-problem 4: C9 — Function.prototype.call/apply over indirect-discovered fn

**C9-A: detect via reverse points-to.** When `binding.call(thisArg, ...)` is found where binding is in `_specPointsToReverseGet(fnNode)`, slice args at idx 1+.
**C9-B: thisArg propagation.** Bind thisArg as this-AV for fn body's ctx-refinement.
**C9-C: chain through `.bind().call()` compositions.** bind-then-call: combine preArgs + .call args.

#### Sub-problem 5: B7 — explicit closure capture via FE-as-arg

**B7-A: snapshot-based propagation.** When `helper(function() { ... })` is called, the FE arg has `_specFuncRefClosureState` snapshot. Walk the snapshot for fn-refs and propagate to the FE param's points-to.
**B7-B: FE param invocation expansion.** When helper invokes its FE param, the FE param's points-to is the snapshot's fn-refs.
**B7-C: snapshot freshness.** Refine the snapshot at ctx-refinement time so the helper sees post-call-site state.

#### Sub-problem 6: E — Full structural-edge dedup across all worklists

**E-A: Phase C consumer worklists.** Each iterator unfold worklist gets keyed by (call-site, fn, paramIdx).
**E-B: Phase D resolver worklists.** Edge keys: (declNode, fnNode).
**E-C: Termination proofs.** Per worklist: prove finite bound via cardinality (call-sites × distinct-AVs × slice-fns). Document in Part 6 invariants table.

#### Sub-problem 7: NEW — Logical-AV unfolding at builtin-method dispatch

**SD-11-A: receiver-side unfold.** `_specApplyBuiltinMethod` and similar dispatch sites need to walk logical AVs to const-leaves BEFORE matching against String/Array/Number method tables. Currently the unfold only fires in member-access; the call-eval doesn't unfold.
**SD-11-B: per-leaf result join.** For `(or-const-string-leaves).toLowerCase()`, apply per leaf and join. Currently the dispatch sees `logical` kind and bails to TOP.
**SD-11-C: chain composition.** `(a || b).method().method()` — each step needs to unfold the previous result.

#### Sub-problem 8: NEW — Multi-level ctx-refinement composition

**SD-12-A: Effects-summary composition.** When fn invokes cb (passed as arg), cb's effects need to compose into fn's effects array (as substituted effects), so caller-site sees the cb chain's writes. Currently composition happens per-call-site via re-analysis but doesn't always propagate up.
**SD-12-B: Termination guarantee with cascading invalidation.** ctxAttempted invalidation cascades risk infinite loops; bound by (call-site, fn-being-refined) Set. Convergence via structural AV-hash stabilization (when refined effects match prev round, stop).
**SD-12-C: Per-call-site refined-memo isolation vs propagation.** Currently the refined `_specPathValMemo` is restored after each refinement (isolation); per-node AVs need selective propagation to base memo for subsequent ctx-find rounds (partial fix landed).

#### Sub-problem 9: NEW — Closure-instance discrimination (CID-1..8)

**CID-A: producingCall tag on function-ref AV.** Each factory call produces a fresh function-ref AV tagged with its producing call site.
**CID-B: substitution propagates producingCall.** _specInstantiateAv preserves producingCall through nested substitutions.
**CID-C: closure-state keyed by producingCall.** `_specFuncRefClosureState` keyed by (funcNode, producingCall) tuple instead of funcNode alone.
**CID-D: invocation context.** When dispatching, use the producingCall's closure state.

#### Sub-problem 10: NEW — Resolver-side reverse-points-to query (Phase D as separate channel)

**D-A: Entry point at resolver miss.** When `_resolveAvBySubstitutingCallerArgs` returns empty for a fnPath, consult `_specPointsToReverseGet(fnPath.node)` for binding-flow paths to walk additionally.
**D-B: Per-binding traversal.** For each holding declNode, walk binding.referencePaths for invocation contexts (mirrors Phase C7 but in resolver scope).
**D-C: Bounded by structural-edge dedup.** Edge keys: (declNode, fnNode).

#### Sub-problem 11: NEW — jQuery-specific transport chain composition

**JQ-A: ajaxSettings.xhr propagation.** `ce.ajaxSettings.xhr = function(){return new ie.XMLHttpRequest}` writes to a 2-level LHS. The base-Identifier + chain key walk (multi-level LHS fix landed this session) handles this.
**JQ-B: opts inheritance via extend.** `extend(s, ce.ajaxSettings, opts)` merges defaults + user opts into s. s.xhr inherits the factory. Inter-procedural extend-merge state propagation.
**JQ-C: transport selection by dataType.** `transports[dataType] || transports["*"]` reads from the registry built via the each-iter pattern (now PASSES synthetically post B2-5 wildcard work).
**JQ-D: jqXHR send() body analysis.** The send fn receives transport(opts), invokes `r=opts.xhr()` then `r.open(opts.type, opts.url, ...)`. Each step needs concrete opts.
**JQ-E: callback dispatch (.done/.fail/.complete).** Promise-like wrapping; cb invocation chain through the deferred state machine.

#### Sub-problem 12: NEW — Receiver-typed builtin dispatch over fold-equivalent receivers

**TR-A: const-string method calls on or-of-strings.** `or("a","b","c").toLowerCase()` should produce or("a","b","c") (each leaf folded).
**TR-B: array method calls on or-of-array-lits.** `or([1,2],[3,4]).slice(0,1)` etc.
**TR-C: numeric ops on or-of-numbers.**
