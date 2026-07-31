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

/* A KEYED WRITE'S VALUE IS BORROWED. step_setprop_run and step_defidx_run place (obj, value) in the header's
 * request buffer and take NO reference to either — the machine holds them in its own slots across the
 * suspension, which is what makes the teardown's list complete. A caller that hands one a `js_dup(...)` creates
 * a reference nobody frees, and because the value is normally reachable from the page's graph, that one leaked
 * reference pins the whole runtime: RegExp.prototype[Symbol.search] restoring an OBJECT-valued lastIndex leaked
 * 383 objects per test file, and reported nothing else wrong. C cannot object — js_dup returns a JSValue and
 * the parameter is a JSValueConst — so the pairing is asserted here, exactly like the two above.
 * A NON-refcounted immediate (js_int32/js_int64/js_uint32/JS_UNDEFINED) is not a reference and is fine. */
for (const m of src.matchAll(/step_(setprop|defidx)_run\s*\(([\s\S]{0,400}?)\)\s*;/g)) {
  if (!/js_dup\s*\(/.test(m[2])) continue;
  const line = src.slice(0, m.index).split('\n').length;
  console.error(`[step-visits] quickjs.c:${line}: step_${m[1]}_run is handed a js_dup — a keyed write's value ` +
                `is BORROWED, so that reference is leaked and pins everything it reaches`);
  bad++;
}

/* AN ARRAY'S DECLARED CAPACITY IS WHAT IT WAS ALLOCATED WITH, never how much of it is live. The `array`
 * operation takes both because they differ: the clone must hand the sibling a block of the same SIZE, or the
 * sibling's next append sees `n != cap`, skips its realloc, and writes past the end. Four landed visits passed
 * the live count for both — Iterator.zip's inputs and its pads, Object.defineProperties' descriptor list,
 * JSON.stringify's PropertyList — each a heap overflow on a fork taken mid-collection, and none of them
 * something C or a test could notice, because the state is only wrong on the CLONE and only once it grows.
 *
 * So the allocation and the declaration are paired by NAME: for every (state struct, field) the file allocates,
 * take the element-count expression out of the allocation, and require the visit's `cap` argument to be it.
 * Scoping by struct is load-bearing rather than tidiness — half a dozen machines call their request buffer `cb`,
 * and a field-name-only map merges their sizes into one set that matches nothing.
 * A size expression that is only a `sizeof` allocates ONE element; that is what the pointer-to-one-object
 * fields (an enum-keys cursor) are, and they declare a capacity of 1 or 0. */
{
  const norm = (t) => t.replace(/\(\s*(?:size_t|int|uint32_t|int64_t|uint64_t|unsigned)\s*\)/g, '')
                       .replace(/sizeof\s*\([^)]*\)/g, '')
                       .replace(/[\s*]/g, '')
                       .replace(/^\(([^()]*)\)$/, '$1')
                       /* `E>0?E:1` and `E` are the SAME capacity: the operation's own dup allocates
                          `cap > 0 ? cap : 1`, so a machine writing that guard into its malloc and passing the
                          bare count to the visit has stated one number twice, not two numbers. */
                       .replace(/^(.+)>0\?\1:1$/, '$1');
  /* which C variable names denote which state struct, per function body */
  const FUNC = /^static [^\n]*?\b(\w+)\s*\([^)]*\)\s*\n\{/gm;
  const bounds = [];
  for (const m of src.matchAll(FUNC)) bounds.push(m.index);
  bounds.push(src.length);
  const allocCap = new Map();   // "JSIterZip.iters" -> Set(size expressions)
  const capOf = (struct, field) => allocCap.get(struct + '.' + field);
  for (let b = 0; b + 1 < bounds.length; b++) {
    const body = src.slice(bounds[b], bounds[b + 1]);
    const vars = new Map();
    for (const d of body.matchAll(/\b(JS\w+)\s*\*\s*(\w+)\s*(?:=\s*st\b|[,)])/g)) vars.set(d[2], d[1]);
    /* every `x->f = <expr>;` in this function, normalised — a machine that allocates N elements and then records
       N in a field is stating the SAME capacity by another name, and that name is what the visit will use. */
    const named = new Map();   // normalised expression -> Set("s->ncb")
    for (const w of body.matchAll(/\b(\w+)->(\w+)\s*=\s*([^;=][^;]*?);/g)) {
      if (!vars.has(w[1])) continue;
      const e = norm(w[3]);
      if (!e) continue;
      if (!named.has(e)) named.set(e, new Set());
      named.get(e).add(w[1] + '->' + w[2]);
    }
    /* EVERY allocation in this function, with the field it ends up in. Three shapes, and the third is the one
       that matters: the GROWTH idiom assigns js_realloc's result to a LOCAL, checks it, and only then stores it
       into the field — so a rule that only reads `x->f = js_realloc(...)` is blind to exactly the arrays that
       grow, which is to say to exactly the arrays whose live count and capacity differ. */
    const record = (st, field, sizeExpr) => {
      const k = st + '.' + field, e = norm(sizeExpr) || '1';
      if (!allocCap.has(k)) allocCap.set(k, new Set());
      allocCap.get(k).add(e);
      for (const alias of (named.get(e) || [])) allocCap.get(k).add(norm(alias));
    };
    /* Three INDEPENDENT patterns rather than one alternation, because a single regex that has to match the
       assignment TARGET misses the growth idiom's second realloc — `nn = js_realloc(ctx, s->nexts, …)` has no
       type word in front of it, so an alternation anchored on one silently skips the statement entirely, and a
       gate that skips the shape it exists to check is worse than no gate. */
    for (const a of body.matchAll(/\b(\w+)->(\w+)\s*=\s*js_(?:m|re)alloc[a-z_]*\(\s*ctx\s*,(?:\s*[\w>.-]+\s*,)?([^;]*?)\);/g))
      if (vars.get(a[1])) record(vars.get(a[1]), a[2], a[3]);
    /* a REALLOC of x->f keeps living in x->f, whatever local it lands in first */
    for (const a of body.matchAll(/js_realloc[a-z_]*\(\s*ctx\s*,\s*(\w+)->(\w+)\s*,([^;]*?)\);/g))
      if (vars.get(a[1])) record(vars.get(a[1]), a[2], a[3]);
    /* a MALLOC into a local, stored into the field afterwards */
    for (const a of body.matchAll(/\b(\w+)\s*=\s*js_malloc[a-z_]*\(\s*ctx\s*,([^;]*?)\);/g)) {
      const st2 = new RegExp('\\b(\\w+)->(\\w+)\\s*=\\s*' + a[1] + '\\s*;').exec(body);
      if (st2 && vars.get(st2[1])) record(vars.get(st2[1]), st2[2], a[2]);
    }
  }
  const CALL = /v->array\(\s*ctx\s*,\s*\(void \*\*\)&(\w+)->(\w+)\s*,([^;]*?)\);/g;
  for (let b = 0; b + 1 < bounds.length; b++) {
    const body = src.slice(bounds[b], bounds[b + 1]);
    const vars = new Map();
    for (const d of body.matchAll(/\b(JS\w+)\s*\*\s*(\w+)\s*(?:=\s*st\b|[,)])/g)) vars.set(d[2], d[1]);
    for (const m of body.matchAll(CALL)) {
      const st = vars.get(m[1]);
      if (!st) continue;
      const sizes = capOf(st, m[2]);
      if (!sizes) continue;                     // allocated where this cannot see it: not reported
      const args = m[3].split(',');
      if (args.length < 4) continue;
      /* the rule is about the ELEMENT COUNT, so it can only be checked when the element SIZE is a plain
         sizeof(T). A flexible-array record is ONE element that states its extent in the size argument instead —
         Iterator.concat's (iterator, method) pairs are that — and there is no count there to pair. */
      if (!/^\s*sizeof\s*\([^()]*\)\s*$/.test(args[0])) continue;   // no nested parens: a lone sizeof, nothing added to it
      const cap = norm(args[args.length - 2]) || '1';
      if ([...sizes].some(e => e === cap)) continue;
      const line = src.slice(0, bounds[b] + m.index).split('\n').length;
      console.error(`[step-visits] quickjs.c:${line}: ${st}.${m[2]} is declared with capacity \`${cap}\` but ` +
                    `allocated with \`${[...sizes].join(' | ')}\` — a clone given the smaller block overflows ` +
                    `it on the next append`);
      bad++;
    }
  }
}

if (bad) process.exit(1);
console.log(`[step-visits] ${checked} declarations, each paired with exactly one state struct`);
