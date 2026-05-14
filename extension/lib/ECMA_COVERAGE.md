# ECMA-262 Coverage Matrix + System Design — ast.js

This document is BOTH the spec-coverage tracker AND the architectural plan for `extension/lib/ast.js`. It's structured so the next session can pick up where the last left off without re-deriving context.

**Working principles** (codified from CLAUDE.md + user directives):
- Every ECMA spec section is a discrete sub-problem.
- Plan from spec semantics. Current state is "TOP" / "not modeled"; fix the spec gap.
- Each closed gap gets one targeted test in `testing/test-api-learning.js` named by spec section. Tests must fail first, pass after.
- No heuristics, no magic numbers, no name-based matching (everything flows through AV identity + scope-resolved bindings + AST structure).
- All algorithms iterative — no recursion in scoped files.
- Trace through framework code — it's just JS.

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

**95/97 passing.** Each closed gap is a regression guard for one specific ECMA semantic. The 2 IIFE-export diagnostic tests now PASS after Phase B2-5 wildcard slot propagation through global override capture + member-access fall-through. Real jQuery now has the WILDCARD slot in _extraProps (verified via debug script — jQuery._extraProps has 90 keys including the sentinel), but the inner cb body invocation chain through `jQuery.ajax`'s full state machine still doesn't complete → fetchCallSites still 0 for `$.ajax` / `$.get`. The remaining gap is in jQuery's ajax body's ctx-refinement composition (SD-10) — the inner fn that user-code reaches via wildcard returns `ce.ajax({...})`, which then enters the full ajaxSettings → prefilters → transports → jqXHR chain that needs proper inter-procedural opts substitution through every hop.

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

### 6.7 Implementation priority order (next session)

1. **B2-1..B2-7** (obj-lit prop writes) — unblocks C7, B6, real-jQuery transport registry dispatch
2. **B3'-1..B3'-4** (capture-independent declarator walk) — unblocks bindings with no capturing fn
3. **C7-1..C7-3** (obj-prop dispatch consumer) — pairs with B2
4. **B5-1..B5-3** (return propagation) + **C8** (return-of-fn dispatch) — covers factory pattern
5. **B4-1..B4-5** (iteration cb-element-param) — unblocks transport `arr.forEach(cb)` in jQuery
6. **D-1..D-4** (resolver fallback) — closes the loop for missed sites
7. **E-1..E-3** (full structural-edge dedup) — termination guarantee
8. **F-1..F-20** (per-sub-problem tests)
