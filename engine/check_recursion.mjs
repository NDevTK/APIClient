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

/* NOT RAISED. The audit was unrunnable for a long stretch (see check_recursion.sh: it compiled against a lexbor
   path the build had stopped using), and when it was restored the interpreter cycle measured 433 against this
   421. Twelve functions joined it unobserved; exactly one is attributable — an own-slot probe added for the
   unknown-property-key read, measured by disabling it and watching 433 become 432.
   The temptation is to call 421 stale and re-baseline, and that is wrong. QUICKJS INTERNALS ARE NOT EXEMPT:
   the entire point of the trampoline is that the C stack is flat, and a quickjs function in this cycle is a C
   path that can re-enter JS_CallInternal exactly as a host function would be. This number is load-bearing for
   the same reason the host check below it is. It stays where it was, the check stays failing, and the twelve
   are the work — `--list-blob` names them. */
const CEILING_BLOB = 421     /* the interpreter cycle's size */
const CEILING_OWN = 17       /* self-contained recursions */
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
   What remains of that pair is the eleven-function READER, which is harder: the writer emits as it descends
   while the reader must BUILD bottom-up, so its stack has to carry partially-built containers and place each
   child into its parent. It still traps between depth 3000 and 5000. */
const CEILING_OWN_FUNCS = 30 /* functions in them */

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
