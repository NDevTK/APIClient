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

const CEILING_BLOB = 421     /* the interpreter cycle's size */
const CEILING_OWN = 23       /* self-contained recursions */
const CEILING_OWN_FUNCS = 72 /* functions in them — lowered as the audit found fewer; a ratchet gives nothing back */

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
const check = (name, got, want, what) => {
  if (got > want) { console.error(`${name}: ${got} > ceiling ${want}. ${what}`); bad = true }
  else if (got < want) {
    console.error(`${name}: ${got} < ceiling ${want} — LOWER it in engine/check_recursion.mjs so the gain ` +
                  `cannot be given back.`)
    bad = true
  }
}
check('interpreter cycle', blob.length, CEILING_BLOB,
      'A new DIRECT call joined the interpreter\'s own cycle — a C path that can re-enter JS_CallInternal.')
check('self-contained recursion cycles', own.length, CEILING_OWN,
      'A new C recursion was introduced. Flatten it onto an explicit frame stack.')
check('self-contained recursion functions', ownFuncs, CEILING_OWN_FUNCS,
      'An existing C recursion grew.')
process.exit(bad ? 1 : 0)
