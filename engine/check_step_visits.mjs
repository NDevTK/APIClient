/* Build-time check for the STEP-MACHINE OWNERSHIP DECLARATION (JSTrampStepDef.visit).
 *
 * A machine declares what it owns once, and the teardown and the deep-fork clone read that one declaration
 * (Blink's Trace). Attaching a declaration to a definition is a two-name pairing — the state STRUCT and the
 * VISIT written for it — and it has been done by editing pattern, in batches, for a dozen commits.
 *
 * That pairing went wrong exactly once, and the way it went wrong is why this file exists: a pattern matching
 * `js_iter_zip*_def` attached JSIterZip's visit to JSIterZipKeyed's and JSIterZipDrive's definitions. Those are
 * DIFFERENT STRUCTS, so the visit would have read fields at another type's offsets — garbage pointers dup'd on
 * clone, garbage freed on teardown. It COMPILED CLEANLY (a visit is a void*-taking function pointer, so C has
 * nothing to object to) and the forced-execution fixture PASSED (it does not exercise Iterator.zip). Both
 * checks the conversion series relies on were blind to it; only reading the emitted lines caught it.
 *
 * So the pairing is asserted here rather than trusted:
 *   - two definitions over the SAME state struct may not name different visits;
 *   - one visit may not be named by definitions over DIFFERENT state structs.
 * Neither is a style rule. Each is a type error C cannot see, in a place where being wrong is silent.
 *
 * A THIRD pairing is asserted, and it was added because the second bug in this series was worse than the first:
 *   - a definition whose TEARDOWN reads the declaration must HAVE one.
 * The shared coerce-then-compute and OrdinaryCreateFromConstructor teardowns were converted to release through
 * the declaration while the definitions built by PRIMARGS_DEF_FULL / CREATECTOR_DEF_FULL still declared none —
 * the attaching edit worked by pattern over explicit initializers and a macro body is not one. Those teardowns
 * then freed NOTHING, and the whole corpus leaked 41,820 objects while reporting zero errors. tramp_step_visit_free
 * now DCHECKs the declaration so that state aborts instead of leaking; this check is why it never has to.
 *
 * What this does NOT check, stated so it is not mistaken for more than it is: a machine that carries NO
 * declaration and whose teardown does not read one is not an error here. That is the state every machine starts
 * in, and it already fails loudly and precisely — at the fork, with a DCHECK naming the machine.
 */
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const SRC = join(dirname(fileURLToPath(import.meta.url)), 'qjs', 'quickjs.c');
const src = readFileSync(SRC, 'utf8');

/* A definition is found by its SHAPE — `{ sizeof(State), step, fini, …` — not by the declaration that holds it.
 * Two reasons, both learned from the bugs above. A definition can live inside a #define (PRIMARGS_DEF_FULL and
 * CREATECTOR_DEF_FULL between them build most of the coerce-then-compute and constructor machines), so keying on
 * `static const JSTrampStepDef` would leave the majority of the file unexamined. And the body has to be taken by
 * BALANCED BRACES, because every such initializer contains a nested `{ .proto = fn }` — a lazy `[^}]*` stops
 * there and never sees the `.visit` that follows it, which is to say it reports "no declaration" for definitions
 * that have one and "declaration present" for none of the ones this file exists to check. */
function defs(text) {
  const out = [];
  const HEAD = /\{\s*sizeof\((\w+)\)\s*,\s*(\w+)\s*,\s*(\w+)\s*,/g;
  for (const m of text.matchAll(HEAD)) {
    if (!/_v?fini$/.test(m[3])) continue;      // the fini slot is what makes this a step definition
    let depth = 0, i = m.index;
    for (; i < text.length; i++) {
      if (text[i] === '{') depth++;
      else if (text[i] === '}' && --depth === 0) break;
    }
    out.push({ struct: m[1], fini: m[3], body: text.slice(m.index, i + 1) });
  }
  return out;
}

/* Which teardowns release through the declaration? Those are the ones for which a missing declaration is not
 * "nothing to free" but "everything leaked". */
const readsDecl = new Set();
for (const m of src.matchAll(/static JSValue (js_\w+fini)\(JSContext \*ctx, void \*st, bool take_result\)\s*\n\{([\s\S]*?)\n\}/g))
  if (m[2].includes('tramp_step_visit_free')) readsDecl.add(m[1]);

const byStruct = new Map();   // state struct -> Set(visit)
const byVisit = new Map();    // visit -> Set(state struct)
let checked = 0, bad = 0;

for (const d of defs(src)) {
  const visit = d.body.match(/\.visit\s*=\s*(\w+)/);
  if (!visit) {
    if (readsDecl.has(d.fini)) {
      console.error(`[step-visits] a ${d.struct} definition declares no ownership, but ${d.fini} releases ` +
                    `through the declaration — that teardown frees NOTHING and every field the machine owns leaks`);
      bad++;
    }
    continue;                                  // undeclared with its own teardown: the fork's DCHECK owns that case
  }
  checked++;
  if (!byStruct.has(d.struct)) byStruct.set(d.struct, new Set());
  if (!byVisit.has(visit[1])) byVisit.set(visit[1], new Set());
  byStruct.get(d.struct).add(visit[1]);
  byVisit.get(visit[1]).add(d.struct);
}

for (const [struct, visits] of byStruct)
  if (visits.size > 1) {
    console.error(`[step-visits] ${struct} is declared by two different visits: ${[...visits].join(', ')} — ` +
                  `one state has one owner list`);
    bad++;
  }
for (const [visit, structs] of byVisit)
  if (structs.size > 1) {
    console.error(`[step-visits] ${visit} is attached to definitions over different structs: ` +
                  `${[...structs].join(', ')} — it reads one struct's fields at another's offsets`);
    bad++;
  }

if (bad) process.exit(1);
console.log(`[step-visits] ${checked} declarations, each paired with exactly one state struct`);
