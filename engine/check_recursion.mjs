/* THE C STACK MUST BE FLAT — checked STATICALLY, over the WHOLE PROGRAM, not inferred from a green test run.
 *
 * SyncDriveToCompletion, the counter this project drove to zero, answers one question: was a BYTECODE BODY
 * entered by C recursion while a flow existed. It is silent about C-to-C recursion that never re-enters the
 * interpreter, and it only sees what a test actually executes. Neither limitation is fixable by running more
 * tests.
 *
 * This reports every SELF-CONTAINED CYCLE in the DIRECT call graph. Direct calls only — no indirect edges, no
 * signature guessing — so every cycle it names provably exists in the binary, with zero false positives.
 *
 * IT USED TO READ ONLY quickjs.c, AND THAT WAS THE BIGGEST THING WRONG WITH IT. The build compiles fifteen
 * translation units; checking one hid six cycles, among them libregexp's recursive-descent PATTERN parser
 * (re_parse_disjunction / re_parse_alternative / re_parse_term), whose depth `new RegExp("(".repeat(n))` picks.
 * A checker that silently covers a fraction of the program is worse than none, because its zero is believed.
 * So COVERAGE IS ITSELF CHECKED: every unit the build compiles is listed below, and one that is not in the
 * linked module is a reported failure, never a quiet omission.
 *
 * Build the input with engine/check_recursion.sh, or by hand:
 *   clang -O0 -w -S -emit-llvm -DNDEBUG -D_GNU_SOURCE -DCONFIG_VERSION='"t262"' -DAPICLIENT_DEV=1 \
 *         -Iengine/qjs -Iengine/host -Iengine/host/browser -Iengine/lexbor/source <unit>.c -o <unit>.ll
 *   llvm-link -S *.ll -o all.ll
 * Usage: node engine/check_recursion.mjs all.ll
 *
 * WHAT IT STILL CANNOT SEE is a cycle that closes through a FUNCTION POINTER. That is tractable here — the
 * engine funnels indirect calls through a few declared tables, so those edges can be resolved exactly rather
 * than guessed — but it is not modelled, and this file does not pretend otherwise.
 */
import { readFileSync } from 'node:fs'

const paths = process.argv.slice(2).filter(a => a.endsWith('.ll'))
if (!paths.length) {
  console.error('usage: node engine/check_recursion.mjs <linked.ll> [more.ll ...]')
  process.exit(2)
}
const ir = paths.map(p => readFileSync(p, 'utf8')).join('\n')

/* COVERAGE IS THE DRIVER'S JOB, because it is the one that knows what compiled: engine/check_recursion.sh
   builds every unit engine/build.mjs builds, fails hard on any that will not compile, and passes the count it
   linked. A mismatch here means the module is a FRACTION of the program, and a ceiling met over a fraction is
   not a result. Probing the linked module for witness function names instead was guesswork that reported units
   uncovered when they were present. */
const EXPECTED_UNITS = 15
const uIdx = process.argv.indexOf('--units')
const linkedUnits = uIdx > 0 ? Number(process.argv[uIdx + 1]) : -1

/* One node per DEFINED function; a declaration (an libc import) has no body to recurse through. */
const edges = new Map()
let cur = null
for (const line of ir.split('\n')) {
  if (line.startsWith('define ')) {
    const m = /@"?([A-Za-z0-9_.$]+)"?\(/.exec(line)
    cur = m ? m[1] : null
    if (cur && !edges.has(cur)) edges.set(cur, new Set())
    continue
  }
  if (line === '}') { cur = null; continue }
  if (!cur) continue
  /* a DIRECT call names its callee as a global right before the argument list. `call ... @f(` and its invoke /
   * musttail / notail spellings all match; an INDIRECT call names a register (%v) and is deliberately skipped. */
  for (const m of line.matchAll(/\b(?:call|invoke)\b[^@\n]*@"?([A-Za-z0-9_.$]+)"?\(/g))
    edges.get(cur).add(m[1])
}
for (const [, outs] of edges) for (const t of [...outs]) if (!edges.has(t)) outs.delete(t)

/* Tarjan, iterative: the IR has ~4k functions and a recursive walk in the checker would be its own joke. */
const index = new Map(), low = new Map(), onStack = new Set()
const stack = [], sccs = []
let counter = 0
for (const root of edges.keys()) {
  if (index.has(root)) continue
  const work = [[root, 0]]
  while (work.length) {
    const frame = work[work.length - 1]
    const [v, i] = frame
    if (i === 0) { index.set(v, counter); low.set(v, counter); counter++; stack.push(v); onStack.add(v) }
    const outs = [...edges.get(v)]
    if (i < outs.length) {
      frame[1]++
      const w = outs[i]
      if (!index.has(w)) work.push([w, 0])
      else if (onStack.has(w)) low.set(v, Math.min(low.get(v), index.get(w)))
      continue
    }
    work.pop()
    if (work.length) { const p = work[work.length - 1][0]; low.set(p, Math.min(low.get(p), low.get(v))) }
    if (low.get(v) === index.get(v)) {
      const comp = []
      for (;;) { const w = stack.pop(); onStack.delete(w); comp.push(w); if (w === v) break }
      const selfLoop = comp.length === 1 && edges.get(comp[0]).has(comp[0])
      if (comp.length > 1 || selfLoop) sccs.push(comp.sort())
    }
  }
}

sccs.sort((a, b) => b.length - a.length || a[0].localeCompare(b[0]))
const total = sccs.reduce((n, c) => n + c.length, 0)

/* TWO POPULATIONS, and conflating them would make the number useless.
 *
 * THE INTERPRETER BLOB is the one cycle containing JS_CallInternal. It is a transitive artifact: JS_CallInternal
 * reaches JS_ToPrimitiveFree reaches JS_CallFree reaches JS_CallInternal, and once those three are in a cycle so
 * is everything either of them can reach. Static analysis cannot know that most of those paths are DFAIL-guarded
 * and dynamically unreachable, so its SIZE is the useful signal — it shrinks as each C-driven path is deleted —
 * and it is tracked on its own.
 *
 * THE SELF-CONTAINED CYCLES are the honest ones: each is a named recursion with nothing to argue about. Several
 * have PAGE-CONTROLLED depth, which is the part that matters for "the C stack is a non-limit" — a rope's depth,
 * a proxy chain's length, JSON nesting and source nesting are all chosen by the input.
 *
 * Both are ratcheted. Lowering either is the work; raising either is a regression the build refuses. */
const MAIN = sccs.findIndex(c => c.includes('JS_CallInternal'))
const blob = MAIN >= 0 ? sccs[MAIN] : []
const own = sccs.filter((_, i) => i !== MAIN)
const ownFuncs = own.reduce((n, c) => n + c.length, 0)

/* NOT RAISED, and the guidance that used to sit here was WRONG. It said twelve functions had joined the cycle
   unobserved and that "the twelve are the work". That was a guess from a delta, and --why-blob now measures
   the thing itself: recompute the SCC with one call edge deleted, and an edge whose removal collapses the
   cycle IS the cycle's cause. The answer is not twelve functions and never was.

     JS_CallFree -> JS_CallInternal      433 -> 16     (-417)
     JS_ThrowError -> JS_ThrowError2     433 -> 163    (-270)
     js_malloc -> JS_ThrowOutOfMemory    433 -> 336    (-97)

   ONE edge holds 417 of the 433. JS_CallFree is a four-line wrapper — run the interpreter, free the callee —
   so the edge is not a defect in it; it is the five C-DRIVES-JS callers that reach it: JS_ToPrimitiveFree
   (valueOf/toString), JS_GetPropertyInternal (a getter read), call_setter (a setter write), JS_Invoke, and
   JS_EvalFunctionInternal. Each is a place C runs JS instead of routing onto the trampoline, which is exactly
   what the C-stack rules say a continuation-holding path must not do. Convert them and JS_CallFree has no
   callers left; 417 passengers leave with it.
   The second cluster is independent and cheaper to reason about: error CONSTRUCTION reaches back into the
   interpreter, so every allocation failure path is transitively in the cycle. That is what drags js_malloc in.
   So the work is five named call sites and an error-construction path, not a 433-line list, and the check
   stays failing until they are done.

   THEY ARE DONE, AND 433 IS NOW 16. JS_CallFree no longer exists: every one of its six sites was instrumented
   with a DCHECK, measured over the whole corpus, and DELETED where it was never reached — the ToPrimitive
   body, JS_GetPropertyInternal's getter and its exotic [[GetOwnProperty]] accessor arm, call_setter,
   JS_IteratorClose's `return`, JS_EvalFunctionInternal's program body, and JS_Invoke. The last one to still
   FIRE was the test262 harness's agent thread, which ran its script through plain JS_Eval; it runs as a flow
   now, and deleting the harness's whole FORK_PREEMPT off-mode is what made that visible.
   What is left in the cycle is 16 functions, and --why-blob says they are the CONSTRUCTOR path (8) and JS_Call
   reached from an async-generator settlement (6) — genuinely different work from the six above, not a residue
   of them.

   THE SELF-CONTAINED COUNT WENT UP, AND THAT IS THE SAME MEASUREMENT, NOT A REGRESSION. 17 functions became
   204 because a 176-function cycle — property definition, error construction, allocation, GC, the string
   builder — was INSIDE the interpreter blob and is now its own SCC. Nothing recursive was added; the blob was
   hiding it, exactly as the header says a blob does. It is the second cluster this comment already named: error
   CONSTRUCTION reaches back into property definition, so every allocation-failure path joins, and that is what
   drags js_malloc and JS_RunGC in with it.
   Reverting to make the number look like 17 would put those 176 back under a blob that no longer has any
   reason to exist. The ceilings therefore record what is actually there.

   THAT CYCLE IS NOW 101, AND THE PART OF IT THAT WAS REAL IS GONE. --why=JS_MakeError2 named one edge worth
   most of it: js_malloc -> JS_ThrowOutOfMemory, where allocation failure BUILT an InternalError, whose object,
   message and shape each allocate, each of which can fail again. Upstream suppressed that with an
   `in_out_of_memory` re-entrancy flag — a recursion made not-to-happen rather than impossible. The error is
   pre-allocated per context now, while there is still memory, so the throw is a refcount bump and the flag is
   deleted. 204 -> 158 functions, and the error cycle 177 -> 101.
   The count went 18 -> 19 for the same reason it went 14 -> 18: breaking an edge splits a blob, and the piece
   that came out (29 functions: JS_Throw frees the previous exception, freeing can enqueue a FinalizationRegistry
   job, enqueueing allocates) was inside the 176 all along.
   THE ERROR CYCLE IS GONE — 97 to nothing, and the whole of it was ONE invariant: A CAPTURE CANNOT THROW.
   build_backtrace runs with an exception already in flight and has nowhere to throw to, so anything in it that
   could throw built an error, which captured a stack, which is this. Every step was a general operation asked
   a question whose answer was already fixed, and each one inherited every path that operation can take:
     - It RENDERED the default stack during construction. V8 stores raw frames and formats on the `.stack`
       read; this engine already did that for Error.prepareStackTrace and only rendered eagerly when no hook
       was installed. Now it parks the frames in both cases and the accessor chooses.
     - It BOXED a primitive receiver (10.2.1.2 step 5) during the walk, which allocates a wrapper and defines
       its properties. The frame-dependent half stays at capture; the box moved to CallSite.getThis, which has
       a caller to throw to.
     - It read a function's `name` — already checked to be a flat string — through JS_ToCString, whose first
       act is ToString, and then rebuilt a JS string from the C one. The record keeps the string it found.
     - It built the filename, and JS_MakeError2 its message, with JS_NewStringLen, whose length RangeError is
       itself an error to construct. Both use the non-throwing constructor; JS_MakeError2's retry with
       "Invalid error message" was that recursion's brake and is unnecessary once nothing can throw.
     - Error.stackTraceLimit is read as a Number, as V8 defines it, rather than coerced through the page's
       valueOf; a CallSite comes from the realm's intrinsic rather than OrdinaryCreateFromConstructor's
       `prototype` read; `message` and a DOMException's `stack` are own-slot adds rather than defines.
   204 -> 67 functions. What is left is small and legible, which is the whole point of taking a blob apart —
   every one of these was inside it and none could be named.

   TWO MORE WENT THE SAME WAY, and they are the same defect a third and fourth time: a general operation asked
   a question whose answer the caller already had.
      12  ToNumber of a STRING reached ToString. Its string arm called JS_ToCStringLen, whose first act is
          js_force_tostring — which for a non-string reads a `message` property — so converting "1" to a number
          statically reached the whole property machinery, which reaches ToNumber. It encodes instead, and
          linearizes a rope first because the encoder wants a flat string and says so.
       4  LINEARIZING a rope reached ToString, which linearizes. string_buffer_concat_value coerces because
          most of its callers hand it arbitrary values; the rope walk hands it strings and ropes by
          construction. Split, and the walk itself is no longer recursive either — it uses the flat rope
          ITERATOR that already existed for hash_string_rope, rather than a third traversal of the same tree.

   THE ALLOCATION HALF OF THE FREE CYCLE IS GONE, and it was 9.13 being performed in the wrong place: a
   FinalizationRegistry cleanup job was enqueued where the target is FREED, so js_malloc failing ->
   JS_ThrowOutOfMemory -> JS_Throw freeing the previous exception -> a finalizer -> JS_EnqueueJob -> js_malloc.
   HostEnqueueFinalizationRegistryCleanupJob is the host's, "at some future time"; V8 posts it from the GC
   epilogue. The dead entry is now MOVED onto a runtime list (no allocation at all, and the references it
   already holds keep the callback and held value alive), and the job pump turns it into a job where allocating
   is ordinary. js_malloc, JS_EnqueueJob, JS_Throw and JS_ThrowOutOfMemory all left the cycle.

   THE QUEUE, each named by the edge that holds it:
      25  free_object -> free_property -> JS_FreeValueRT -> free_zero_refcount -> free_gc_object -> free_object.
          THIS ONE IS ALREADY FLAT, and saying so is a measurement rather than a reading of the code. The
          cascade IS an explicit worklist: a dying object's properties are released with gc_phase set to
          DECREF, so js_free_value_rt only appends to gc_zero_ref_count_list and the outer loop drains it.
          `for (;;) a = {next: a}` then dropped is one iteration per link and no C frames at all. A static walk
          cannot see a phase flag, so the cycle is reported; free_zero_refcount now DCHECKs that it is never
          re-entered, and that assert is silent over the whole corpus, which is what makes the claim measured.
          What would remove the reported cycle is splitting the drain from the enqueue — a JS_FreeValueRT that
          drains and a variant the cascade's own internals use that only enqueues — so nothing free_object
          reaches can call the loop. That is a wide edit across every release inside teardown, and it buys a
          number rather than a behaviour, so it is recorded here rather than done ahead of work that changes
          what the engine can do.
   THE STEP-CHAIN TEARDOWN CYCLE IS GONE, six functions and all of it depth the page picked. It was one shape
   at three levels: a link's teardown WALKED its requester chain by calling the walk, which reached that link's
   kind of teardown again. STEP/TOPRIM alternation went first, then the consume and from-ctor hooks.
   The move that took the last two is worth stating, because "22 callers" looked like a reason not to: the
   hooks keep their existing void contract for the eight sites outside the walk, and gained _upto forms that
   REPORT the requester chain instead of walking it. The walk uses those and continues its own loop; the chain
   teardown still lives in exactly one place, so what js_iter_consume_end's comment warns about — a list of
   sites the next one added forgets — stays impossible. A contract change across 22 call sites was never the
   only way to do it, and reading it as one is what deferred it twice.
       2  tramp_step_state_free <-> tramp_step_hdr_release, over a machine's `delegate`. The DEPTH is gone: the
          chain is detached before it is freed, so each link's release finds no delegate and the pair bottoms
          out at one level. The cycle remains because a static walk cannot see the detach, like
          free_zero_refcount's phase flag.
       (the fast-array read cycle is gone: JS_GetPropertyInternal reads the element itself instead of reaching
          it back through the general indexed entry)
       4  JS_DefineProperty -> JS_SetPropertyValue -> JS_SetPropertyInternal
       4  js_new_string_rope <-> js_rebalance_string_rope. What this WAS is the rebalance's tree walk, a
          recursion over a structure the PAGE sizes — `s = s + x` in a loop adds a rope level per iteration —
          and it is gone: the walk uses the flat leaf iterator that already existed for hash_string_rope. What
          is left is the mutual pair, and it is bounded by construction rather than by a check: the leaf step
          builds bucket concatenations, building a rope is what can ask for a rebalance, and the ropes built
          out of buckets are balanced, so the depth never reaches the rebalance point again.
       3  lre_case_conv, 2+2, and eleven single-function recursions. */
const CEILING_BLOB = 16      /* the interpreter cycle's size */
const CEILING_OWN = 7       /* self-contained recursions */
/* 70 -> 65, the parser cycle 29 -> 24, as js_parse_descent's explicit frame stack absorbed the precedence
   ladder (expr_binary / logical_and_or / coalesce_expr / cond_expr) and then UnaryExpression (unary / delete).
   BE PRECISE ABOUT WHICH HALF PAID. The ladder was never deep: `a|b|c` is left-nested and the recursive version
   already looped over the operator, so 200 000 `|` operands parsed fine before AND after, and all it bought was
   paren depth ~800 -> ~1000. UnaryExpression is the opposite — every prefix operator takes a UnaryExpression
   operand, so it was one C frame per character: `"!".repeat(200000)` and `"typeof ".repeat(200000)` and
   `"delete ".repeat(100000)` each threw RangeError before and each parses now.
   65 -> 62 adds Expression and the parenthesized expression, and moved the descent's frame stack out of the
   driver into JSParseState. That move was forced by a REGRESSION this conversion caused: a per-activation
   inline array put the descent's working set back on the C stack, and because unconverted productions sit
   between converted ones the driver pays for it twice per paren level — nested parens began failing at 800
   where the pre-conversion parser passed. Shared stack, per-activation base index: 800 parses again, 1200
   parses where the original failed.
   62 -> 60, the parser cycle 21 -> 19, adds AssignmentExpression. Three of its productions take an
   AssignmentExpression operand (`yield x`, `a = x`, `a ??= x`), so it was a C frame per `=` in `a=b=c=…` and
   per `?:` in a ternary chain. 100 000 nested ternaries went from RangeError to parsing; a 100 000-deep
   assignment chain went from RangeError (the PARSER's guard) to InternalError (the INTERPRETER's operand
   stack) — the parse now succeeds and the remaining limit belongs to a different subsystem.
   60 -> 58, the parser cycle 19 -> 17, adds MemberExpression/CallExpression (js_parse_postfix_expr and
   js_parse_left_hand_side_expr) — the production nested parens, nested calls and nested index expressions all
   descend through, and so the last big holder of source-controlled depth in the EXPRESSION grammar.
   58 -> 57 adds ArrayLiteral, which CLOSES `[[[[…]]]]` — 20000 nested `[` parses where the original throws.
   Worth recording how that nearly did not happen: rewriting the call site to js_parse_descent(...) left it a
   nested C ACTIVATION of the driver, one frame per level, and the depth probe was identical to the unconverted
   parser. A converted production's call sites INSIDE the driver must be PD_CALLs.
   45 -> 43, the parser cycle 4 -> 2, adds FunctionDeclaration/Expression/method/arrow — the LAST recursive
   body in the parser. js_parse_function_decl2 and its js_parse_function_decl wrapper are both deleted; all
   thirteen call sites are PD_CALL_Ps, so a function body no longer costs a C activation at any nesting depth.
   `'function f(){'*n` parsed to 60 000 before and after; at 65 000 the old parser answers `SyntaxError:
   missing formal parameter` — a WRONG error, not a clean stack report — where the new one parses.
   THE EXPENSIVE HALF WAS NOT THE TRANSFORM, IT WAS THE DRIVER LOCALS THE TRANSFORM INVALIDATED. Six
   productions kept state in C locals that were safe only because the function-body descent was a C recursion
   giving each nesting level its own C frame. Making it a PD_CALL puts an inner function in the SAME driver
   activation as the outer, so every one of those became a live-across-suspension hazard at once: the class
   production's field table / constructor / source start, its is_set and method_fd, its func_type, the object
   literal's is_getset, this production's own name / rest / idx / label, and — worst — the body's BlockEnv,
   whose ADDRESS push_break_entry links into fd->top_break, so a nested function's pop unlinked the outer's.
   Each moved to the frame or was respelled from a field it derives from. The rule that catches all of them:
   converting a call site invalidates every liveness claim across it, so re-audit the WHOLE production, not
   the lines that changed.
   43 -> 42 deletes js_parse_property_name, the last signature adapter, so the parser's MUTUAL recursion is
   gone: what remains is js_parse_descent as a SELF-loop, from six call sites inside the driver still spelled
   js_parse_descent(s, ...) — a nested C ACTIVATION per nesting level, exactly the trap ArrayLiteral fell into
   above. Those six (three PDS_DESTR, two PDS_CLASS, one PDS_EXPORT) are the work; next_token's
   js_check_stack_overflow is deleted when they are PD_CALLs, not before.
   The adapter's deletion was not free either: the class member's start_ptr spans the key descent AND the body
   descent, so a computed key containing a class of its own would have overwritten the outer member's source
   start. Same rule as the entry above — the conversion, not the code, is what invalidated the liveness.
   42 -> 41 and the cycle count 21 -> 20 convert those six, so js_parse_descent leaves the list entirely and
   THE PARSER HAS NO C RECURSION AT ALL. The one js_parse_descent(s, ...) left in the file is
   js_parse_program's, which is the driver's C ENTRY — the base activation, not a nested one.
   What this did NOT buy is the depth those paths still cap at, and the reason is worth recording because it
   was almost mis-attributed to this work: `var {a:{…}}` still fails at ~255, identically before and after,
   and a backtrace at the failure shows a FLAT C stack with one js_parse_descent frame. It is not recursion.
   js_parse_skip_parens_token holds `char state[256]`, a fixed bracket stack that silently `goto done`s when
   the nesting outruns it and returns a token the caller then mis-reports as "variable name expected". A
   banned bound AND a wrong answer on valid source, and it is next.
   41 -> 40 converts gather_available_ancestors — GatherAvailableAncestors — from C recursion to a walk, and
   deletes its stack guard with it. The depth was the async module GRAPH's, which the imported source picks, so
   the guard turned a graph the algorithm answers into a RangeError. exec_list is its own worklist: every module
   it appends is one whose dependencies just reached zero, which is exactly the set the recursion descended
   into, so a cursor over this call's appends visits each once and no second array exists to get wrong. Order
   goes depth-first to breadth-first, which is not observable — same set reached, and the caller sorts by
   async_evaluation_timestamp before using it.
   40 -> 39 converts its sibling js_async_module_execution_rejected, which I had first declined on the grounds
   that its rejection order is observable. That was the wrong conclusion from a true fact: order is exactly why
   it takes a LIFO STACK rather than the cursor above. Popping and pushing the parents in REVERSE reproduces
   pre-order depth-first settlement precisely, so the constraint picks the data structure instead of forbidding
   the work. The recursion also allocated a JSValue per edge purely to re-enter through its own callback
   signature; walking directly deletes that too. rejection-order.js and fulfillment-order.js are the oracle.
   39 -> 30 is the whole nine-function bytecode WRITER cycle, converted to a work stack in BCWriterState and
   its stack guard deleted. Output is a pre-order byte stream, so a LIFO stack reproduces it exactly: pop an
   item, emit its header, push the children so the first pops next. Two things the shape forces, both of which
   I got wrong first and fixed before testing rather than after — children are fetched ONE PER POP, because an
   element getter must not run ahead of the previous element's own nested getters; and an owned value's release
   is QUEUED, not done when its step returns, because the step leaves cursors on the stack still pointing at
   it. Verified byte-identical against the previous writer over arrays, nested objects, Map, Set, typed arrays,
   boxed primitives, Date, RegExp and object references: 938 bytes, same hash. Depth 20000 now writes where it
   threw between 5000 and 20000 before.
   30 -> 19 finishes the pair: the eleven-function READER, and its stack guard with it. It needed a different
   shape — the writer emits as it descends, so a work stack of pending items sufficed, while the reader BUILDS
   and every child has to be placed into a half-made parent. So its frames carry the container AND a cursor,
   the finished child arrives in a result register, and the whole thing lives in BCReaderState rather than in
   C locals: everything live at a frame boundary is in the struct, which is what makes the walk re-enterable
   rather than merely flat.
   Two orderings the arms had to keep, neither of which a depth test would have caught. Object reference ids
   are assigned by the ORDER of BC_add_object_ref, so an arm that registered its container before its children
   still must, and the typed array still reserves its slot before reading the buffer and fills it after. And
   Date reads its number BEFORE creating the object while Array creates the container FIRST — opposite orders,
   both preserved.
   Verified by ROUND TRIP, because byte-identical output only ever tested the writer: read the stream back and
   re-serialise the reconstruction. Same 938 bytes, same hash, over arrays, nested objects, Map, Set, typed
   arrays, boxed primitives, Date, RegExp and object references. Depth 3000-5000 became 200000.
   19 -> 17 takes both rope walkers, and neither needed a state machine — which is the point of looking before
   building one. string_rope_get's two descents were both TAIL calls, so the C frame carried nothing but the
   argument and re-assigning it says the same thing. hash_string_rope was a duplicate traversal of a structure
   that ALREADY had a flat one: string_rope_iter_next yields the leaves left to right, exactly the order the
   recursion visited them. Writing a third walker there would have been the workaround.
   NOT converted, and deliberately: lre_case_conv / lre_case_conv1 / lre_case_conv_entry are a mutual cycle
   whose depth bottoms out at about two — a converted character converted once more. Flattening it would be
   churn dressed as progress, and the audit counting it is the audit being honest about direct calls rather
   than a debt. The same is true of rope DEPTH generally: JS_STRING_ROPE_MAX_DEPTH is 60 with a flatten above
   it, so no rope walk was ever input-deep. */
const CEILING_OWN_FUNCS = 39 /* functions in them — the largest is 25, the refcount teardown cascade */

for (const c of own) console.log(`  [${c.length}] ${c.join(' ')}`)
console.log(`interpreter cycle: ${blob.length} functions`)
console.log(`self-contained recursion: ${own.length} cycles over ${ownFuncs} functions`)

let bad = false
/* COVERAGE FIRST: a ceiling met over part of the program means nothing. */
if (linkedUnits !== EXPECTED_UNITS) {
  console.error(`COVERAGE: ${linkedUnits} translation units linked, expected ${EXPECTED_UNITS}.`)
  console.error(`  Run engine/check_recursion.sh, which builds every unit engine/build.mjs builds and fails on`)
  console.error(`  any it cannot compile. A ceiling met over part of the program is not a result.`)
  bad = true
}
/* WHICH functions, not just how many. A ratchet that reports "433 > 421" and stops has the same defect the WPT
   gap list had before it learned to name files: the number tells you something regressed and nothing about
   where, so the next step is a bisection instead of a look. `--list-blob` prints the interpreter cycle's
   members, `--list-own` the self-contained ones. */
if (process.argv.includes('--list-blob')) { for (const f of [...blob].sort()) console.log(f) }
if (process.argv.includes('--list-own')) { for (const c of own) console.log('[' + c.length + '] ' + c.join(' ')) }

/* WHICH EDGE HOLDS IT, which is the question `--list-blob` cannot answer. The interpreter cycle is a
   TRANSITIVE artifact: naming its 433 members says nothing about which of them to touch, and the guidance
   above this file used to carry — "twelve functions joined it unobserved, the twelve are the work" — was a
   guess dressed as a work list. Recomputing the SCC with one call edge deleted says it exactly: an edge whose
   removal collapses the cycle is the cycle's cause, and everything else in it is a passenger.
   Run with --why-blob. The answer was one line long — JS_CallFree -> JS_CallInternal, 433 -> 16 — and acting
   on it deleted JS_CallFree and the six C-drives-JS sites that reached it.
   IT IS NOT ABOUT THE INTERPRETER, so it no longer hardcodes JS_CallInternal. --why-blob still anchors there;
   --why=NAME anchors on any function, which is what the 176-function property/error/allocation cycle needs —
   that one was a passenger of the interpreter blob until the blob went, and asking the same question of it is
   the whole of the next step. A diagnostic that answers only for the cycle that happened to be biggest when it
   was written has to be rewritten every time it succeeds. */
const whyArg = process.argv.find(a => a.startsWith('--why='))
if (whyArg || process.argv.includes('--why-blob')) {
  const anchor = whyArg ? whyArg.slice(6) : 'JS_CallInternal'
  const cycle = sccs.find(c => c.includes(anchor))
  if (!cycle) { console.error(`--why: no cycle contains ${anchor}`); process.exit(1) }
  const sccSize = (dropFrom, dropTo) => {
    const ix = new Map(), lo = new Map(), st = [], onSt = new Set()
    let n = 0, found = 0
    for (const root of edges.keys()) {
      if (ix.has(root)) continue
      const work = [[root, 0]]
      while (work.length) {
        const fr = work[work.length - 1], v = fr[0]
        if (fr[1] === 0) { ix.set(v, n); lo.set(v, n); n++; st.push(v); onSt.add(v) }
        const outs = [...edges.get(v)].filter(w => !(v === dropFrom && w === dropTo))
        if (fr[1] < outs.length) {
          const w = outs[fr[1]++]
          if (!ix.has(w)) work.push([w, 0])
          else if (onSt.has(w)) lo.set(v, Math.min(lo.get(v), ix.get(w)))
          continue
        }
        work.pop()
        if (work.length) { const p = work[work.length - 1][0]; lo.set(p, Math.min(lo.get(p), lo.get(v))) }
        if (lo.get(v) === ix.get(v)) {
          const comp = []
          for (;;) { const w = st.pop(); onSt.delete(w); comp.push(w); if (w === v) break }
          if (comp.includes(anchor)) found = comp.length
        }
      }
    }
    return found
  }
  const base = sccSize(null, null)
  /* --from=NAME restricts the candidates to that function's OWN out-edges. The ranked list answers "what holds
     this cycle"; once the answer is a function rather than a leaf edge — JS_MakeError2 -> build_backtrace is
     worth 94 and build_backtrace is a hundred lines with a dozen callees — the next question is which of ITS
     calls carries the cycle, and asking it with a grep over the member list is guessing again. */
  const fromArg = process.argv.find(a => a.startsWith('--from='))
  const from = fromArg ? fromArg.slice(7) : null
  const seen = []
  for (const [f, outs] of edges)
    for (const t of outs)
      if (cycle.includes(t) && cycle.includes(f) && (!from || f === from)) {
        const after = sccSize(f, t)
        if (after < base) seen.push([base - after, f, t, after])
      }
  seen.sort((a, b) => b[0] - a[0])
  console.log(`cycle containing ${anchor}: ${base}; edges whose removal shrinks it:`)
  for (const [d, f, t, after] of seen.slice(0, from ? 40 : 10))
    console.log(`  -${String(d).padEnd(5)} ${f} -> ${t}   => ${after}`)
  if (!seen.length) console.log('  none individually — several edges hold it at once')
}

/* THE INVARIANT THAT ACTUALLY GUARDS THE LAYERING: not one browser or solver component may sit in the
   interpreter's own cycle. A host edge is C code the interpreter calls; if one of them could re-enter
   JS_CallInternal it would be running the page's code from inside a C activation with no flow base, which is
   the drive-to-completion this engine exists to avoid — and unlike a size ceiling, this is a claim with a
   RIGHT answer, so it ratchets at zero and stays there. Measured over the whole linked program, so it covers
   paths no test happens to run. */
/* WHY BOTH NUMBERS BELOW ARE THE QUEUE, AND NEITHER IS EXCUSED. An earlier version of this file was about to
   single out the cycle members that call USER code from C, as though recursion mattered only when it ran the
   page's own program. It does not. ALL RECURSION IS BANNED: a recursive descent through js_parse_expr on
   attacker-controlled source, JS_ReadObjectRec on a nested structured clone, re_parse_disjunction on a crafted
   pattern — none of them touch user code and every one of them is C stack this engine cannot suspend, park or
   resume. A flow that cannot be snapshot at an arbitrary depth is not a flow.
   So the interpreter cycle's 433 and the 72 functions across 23 self-contained cycles are one work queue, not
   a headline and a footnote, and "it never re-enters the interpreter" is not a defence. --list-blob and
   --list-own name every member. */

const HOST_PREFIX = /^(document|element|node_|timer|window_|event_target|fetch_|response_|module_loader|engine_|concolic|solve|absent|endpoint|result_|decide|attr_shadow|dom_cow|qjs_)/
const hostInBlob = [...blob].filter(f => HOST_PREFIX.test(f)).sort()

const check = (name, got, want, what) => {
  if (got > want) { console.error(`${name}: ${got} > ceiling ${want}. ${what}`); bad = true }
  else if (got < want) {
    console.error(`${name}: ${got} < ceiling ${want} — LOWER it in engine/check_recursion.mjs so the gain ` +
                  `cannot be given back.`)
    bad = true
  }
}
if (hostInBlob.length) {
  console.error('host components inside the interpreter cycle: ' + hostInBlob.join(' ') +
                '\n  A browser/solver component that can re-enter JS_CallInternal runs the page\'s code from a ' +
                'C activation with no flow base. Route it through the flow machinery instead.')
  bad = true
}
check('interpreter cycle', blob.length, CEILING_BLOB,
      'A new DIRECT call joined the interpreter\'s own cycle — a C path that can re-enter JS_CallInternal.')
check('self-contained recursion cycles', own.length, CEILING_OWN,
      'A new C recursion was introduced. Flatten it onto an explicit frame stack.')
check('self-contained recursion functions', ownFuncs, CEILING_OWN_FUNCS,
      'An existing C recursion grew.')
process.exit(bad ? 1 : 0)
