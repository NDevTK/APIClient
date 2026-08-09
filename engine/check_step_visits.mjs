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
 * And a FOURTH, which only became assertable when the last machine was converted: EVERY definition declares
 * one. While the series was in flight "no declaration" was the state a machine started in, and the fork's own
 * DCHECK was the right place to catch it; now that all 131 state structs carry one, letting the next machine
 * start undeclared is letting the ratchet slip. A machine that cannot be forked cannot be explored, and a
 * solver whose flows silently stop forking inside a builtin is the failure this whole series exists to remove.
 * The fork's DCHECK stays as the backstop for a state the build can no longer produce.
 *
 * AND THE HOST, which this checked NONE of. quickjs-step.h opened the declaration surface so a browser component
 * could declare a machine, and every one it has declared since — the fragment serialiser, isEqualNode,
 * compareDocumentPosition, normalize, customElements.define, the abort machines — was outside this file, which
 * read one path and stopped. The bug it exists to catch is not a quickjs.c bug; it is a two-name pairing bug, and
 * engine/host/browser has the same two names in a second spelling (IdlStepDecl's positional
 * `{ body, sizeof(State), visit, release }`). A gate that covers the half of the codebase that is not currently
 * growing is a gate that reports a number rather than holding a line.
 *
 * The host is also where a STRONGER check is available, and it is the direct form of the original bug: a host
 * visit is a small local function that casts `void *st` to its state struct, so the struct it CASTS TO can be
 * read and required to be the struct its definition declares. `js_iter_zip*_def` attaching JSIterZip's visit to
 * JSIterZipKeyed's definition would have been caught by that on its own, without needing two definitions to
 * disagree. */
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const ENGINE = dirname(fileURLToPath(import.meta.url));
const SRC = join(ENGINE, 'qjs', 'quickjs.c');
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
    out.push({ struct: m[1], fini: m[3], body: text.slice(m.index, i + 1), index: m.index });
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
    console.error(readsDecl.has(d.fini)
      ? `[step-visits] a ${d.struct} definition declares no ownership, but ${d.fini} releases through the ` +
        `declaration — that teardown frees NOTHING and every field the machine owns leaks`
      : `[step-visits] a ${d.struct} definition declares no ownership — it cannot be forked, so a concolic ` +
        `branch inside it aborts the fork instead of exploring both arms`);
    bad++;
    continue;
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

/* ---- THE HOST'S MACHINES ------------------------------------------------------------------------------------
 * Two spellings, because a component declares one of two things: an IdlStepDecl (a Web IDL member whose
 * algorithm is a machine — positional `{ body, sizeof(State), visit, release }`) or a raw JSTrampStepDef (a
 * machine that is not a member, like a custom-element reaction). Both carry the same pairing and both are read. */
function hostSources(dir) {
  const out = [];
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) out.push(...hostSources(p));
    else if (e.endsWith('.c')) out.push(p);
  }
  return out;
}

const hostByStruct = new Map();   // state struct -> Set(visit)
const hostByVisit = new Map();    // visit -> Set(state struct)
const hostVisitBody = new Map();  // visit -> the struct(s) its body casts void* to
let hostChecked = 0;

for (const f of hostSources(join(ENGINE, 'host'))) {
  const text = readFileSync(f, 'utf8');
  const where = relative(ENGINE, f);

  /* Every visit in this file, and what it actually casts its opaque state to. A visit that names no struct at
     all owns nothing (the DOM walkers are all of them) and is exempt — there is no offset to get wrong. */
  for (const m of text.matchAll(/static void (\w+)\(JSContext \*ctx, void \*st, JSStepVisit \*v\)\s*\n\{([\s\S]*?)\n\}/g)) {
    const casts = new Set();
    for (const c of m[2].matchAll(/\b([A-Z]\w+)\s*\*\s*\w+\s*=\s*(?:\(\s*\1\s*\*\s*\)\s*)?st\b/g)) casts.add(c[1]);
    hostVisitBody.set(m[1], casts);
  }

  const found = [];
  for (const m of text.matchAll(/IdlStepDecl\s+\w+\s*=\s*\{\s*(\w+)\s*,\s*sizeof\((\w+)\)\s*,\s*(\w+)\s*,/g))
    found.push({ where, line: text.slice(0, m.index).split('\n').length, struct: m[2], visit: m[3] });
  /* BY SHAPE, exactly as the engine half is found, and for the reason written there. Keyed on the declaration
     `JSTrampStepDef <name> =` this read one spelling and stopped: Response's four body readers are one def per
     body kind held in a table of `{ name, def }` rows, which is a definition with no `=` of its own — so all
     four were invisible here, visit-less or not. A gate that only sees the spelling that happened to exist when
     it was written is the gate this file's own header calls worse than none. */
  for (const d of defs(text))
    found.push({ where, line: text.slice(0, d.index).split('\n').length, struct: d.struct,
                 visit: (d.body.match(/\.visit\s*=\s*(\w+)/) || [, 'NULL'])[1] });

  for (const d of found) {
    if (d.visit === 'NULL') {
      console.error(`[step-visits] ${d.where}:${d.line}: a ${d.struct} machine declares no ownership — it cannot ` +
                    `be forked, so a concolic branch inside it aborts the fork instead of exploring both arms`);
      bad++;
      continue;
    }
    hostChecked++;
    if (!hostByStruct.has(d.struct)) hostByStruct.set(d.struct, new Set());
    if (!hostByVisit.has(d.visit)) hostByVisit.set(d.visit, new Set());
    hostByStruct.get(d.struct).add(d.visit);
    hostByVisit.get(d.visit).add(d.struct);

    /* THE DIRECT FORM: the visit reads its state at some struct's offsets, and that struct must be the one the
       definition declares. Getting this wrong compiles cleanly — a visit takes void* — and passes any fixture
       that does not happen to fork inside that machine. */
    const casts = hostVisitBody.get(d.visit);
    if (casts && casts.size && !casts.has(d.struct)) {
      console.error(`[step-visits] ${d.where}:${d.line}: ${d.visit} reads ${[...casts].join('/')} but is ` +
                    `declared over ${d.struct} — it would read one struct's fields at another's offsets`);
      bad++;
    }
  }
}

for (const [struct, visits] of hostByStruct)
  if (visits.size > 1) {
    console.error(`[step-visits] host: ${struct} is declared by two different visits: ${[...visits].join(', ')}`);
    bad++;
  }
for (const [visit, structs] of hostByVisit)
  if (structs.size > 1) {
    console.error(`[step-visits] host: ${visit} is attached to definitions over different structs: ` +
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

/* ---- THE STAGE DECLARATION -----------------------------------------------------------------------------
 * A definition also declares WHICH ALGORITHM it is and which of its steps each stage rests at
 * (JSTrampStepDef.algorithm / .steps, and IdlStepDecl's two trailing fields). That declaration is what the
 * driver asserts against at do_step_step, and a machine that carries neither is simply not converted yet — so
 * the COUNT of those is the conversion's work queue, and this is where it is reported. quickjs-step.h says so;
 * until this section existed it said so about a gate that did not exist, which is a comment standing in for a
 * mechanism.
 *
 * The pairing is asserted too, for the reason every other pairing in this file is: an algorithm with no steps
 * names a resume point nothing can check, and steps belonging to no algorithm print a stage number against
 * "(unnamed algorithm)". Both compile. The host's positional `{ body, sizeof(State), visit, release,
 * algorithm, steps }` is read by counting its TOP-LEVEL elements, because a positional initializer cannot be
 * asked which field is which — and its labels are string literals full of commas, so they are removed first. */
function topLevelElements(init) {
  const t = init.replace(/"(?:[^"\\]|\\.)*"/g, '""').replace(/\/\*[\s\S]*?\*\//g, '');
  const out = [];
  let depth = 0, cur = '';
  for (let i = 1; i < t.length - 1; i++) {   // inside the outermost braces
    const c = t[i];
    if (c === '(' || c === '{' || c === '[') depth++;
    else if (c === ')' || c === '}' || c === ']') depth--;
    if (c === ',' && depth === 0) { out.push(cur.trim()); cur = ''; continue; }
    cur += c;
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}
function balancedFrom(text, at) {
  const start = text.indexOf('{', at);
  let depth = 0, i = start;
  for (; i < text.length; i++) {
    if (text[i] === '{') depth++;
    else if (text[i] === '}' && --depth === 0) break;
  }
  return text.slice(start, i + 1);
}

/* AND THE LABELS AND THE STAGE CONSTANTS ARE ONE DECLARATION. `steps[stage]` is the spec step a stage rests at
 * and the enum is what the machine's code writes into `stage`; written as two lists they are two statements of
 * one fact, and inserting a step into one and not the other renames every stage after it with nothing to say
 * so — silently at runtime, and across a session it resumes a parked flow at a different step of its
 * algorithm. quickjs-step.h's JS_STEP_STAGE_ENUM / JS_STEP_STAGE_LABEL make the pair one X-list; this refuses
 * a `.steps` array written any other way, because the drift is invisible in the source that has it. */
/* STRINGS AND COMMENTS MASKED, LENGTHS PRESERVED, so brace matching is over the code and the offsets still
   index the original. A step LABEL is a sentence — "22.2.7.2 step 2 (Get(R, \"lastIndex\"))" — and reading the
   array body with a regex that stopped at the first `;` silently skipped twelve of the nineteen arrays: a
   check that cannot parse its input and says nothing is an excluded check, which is the same defect as a test
   the gate does not collect. */
const maskLiterals = (text) => text.replace(/"(?:[^"\\]|\\.)*"|\/\*[\s\S]*?\*\//g, (s) => ' '.repeat(s.length));
const stageListName = (text, arrayName) => {
  const masked = maskLiterals(text);
  const decl = new RegExp(`\\b${arrayName}\\s*\\[\\s*\\]\\s*=`).exec(masked);
  if (!decl) return null;                                     // not in this file: the caller reports it
  const body = balancedFrom(masked, decl.index);
  const x = /(\w+)\s*\(\s*JS_STEP_STAGE_LABEL\s*\)/.exec(body);
  return x ? x[1] : '';                                       // '' = a hand-written list
};
const twoList = [];   // declared, but its constants and its labels are still two lists — the conversion queue
const stageListCheck = (where, text, name, stepsExpr) => {
  const arrayName = /^&?(\w+)/.exec(stepsExpr.trim());
  if (!arrayName) return;
  const list = stageListName(text, arrayName[1]);
  const line = () => text.slice(0, text.indexOf(arrayName[1])).split('\n').length;
  if (list === null) {
    console.error(`[step-steps] ${where}:${line()}: ${name} names ${arrayName[1]} as its steps and this file ` +
                  `does not declare it — the gate cannot see the labels, and a check that skips what it cannot ` +
                  `parse is an excluded check`);
    bad++;
    return;
  }
  /* A HALF-EXPANSION IS BROKEN, not unconverted: the labels come from an X-list and the constants do not, so
     the one declaration is a claim the source does not keep. That fails. A machine still carrying two
     hand-written lists is the same conversion in progress the count below reports, and is named so the queue
     is a list of machines rather than a number. */
  if (list === '') twoList.push(`${where}:${line()} ${name} (${arrayName[1]})`);
  else if (!new RegExp(`\\b${list}\\s*\\(\\s*JS_STEP_STAGE_ENUM\\s*\\)`).test(text)) {
    console.error(`[step-steps] ${where}:${line()}: ${name}'s labels expand ${list} but no enum does — the ` +
                  `stage constants are still a second list, which is the drift the X-list exists to prevent`);
    bad++;
  }
};

let declared = 0, undeclared = 0;
const stageRow = (where, line, name, hasAlg, hasSteps) => {
  if (hasAlg !== hasSteps) {
    console.error(`[step-steps] ${where}:${line}: ${name} declares ` +
                  (hasAlg ? 'an algorithm with no steps' : 'steps belonging to no algorithm') +
                  ' — a stage the driver asserts against needs both halves');
    bad++;
    return;
  }
  if (hasAlg) declared++; else undeclared++;
};
const stepsExprOf = (body) => (/\.steps\s*=\s*([^,}]+)/.exec(body) ?? [])[1] ?? '';
for (const d of defs(src)) {
  stageRow('quickjs.c', src.slice(0, d.index).split('\n').length, d.struct,
           /\.algorithm\s*=/.test(d.body), /\.steps\s*=/.test(d.body));
  if (/\.steps\s*=/.test(d.body)) stageListCheck('quickjs.c', src, d.struct, stepsExprOf(d.body));
}
for (const f of hostSources(join(ENGINE, 'host'))) {
  const text = readFileSync(f, 'utf8');
  const where = relative(ENGINE, f);
  for (const m of text.matchAll(/IdlStepDecl\s+(\w+)\s*=\s*\{/g)) {
    const els = topLevelElements(balancedFrom(text, m.index));
    const has = (i) => els.length > i && els[i] !== '' && els[i] !== 'NULL';
    stageRow(where, text.slice(0, m.index).split('\n').length, m[1], has(4), has(5));
    if (has(5)) stageListCheck(where, text, m[1], els[5]);
  }
  for (const d of defs(text)) {
    stageRow(where, text.slice(0, d.index).split('\n').length, d.struct,
             /\.algorithm\s*=/.test(d.body), /\.steps\s*=/.test(d.body));
    if (/\.steps\s*=/.test(d.body)) stageListCheck(where, text, d.struct, stepsExprOf(d.body));
  }
}

if (bad) process.exit(1);
console.log(`[step-visits] ${checked} engine + ${hostChecked} host declarations, each paired with exactly one ` +
            `state struct; ${hostChecked} host visits also checked against the struct they cast to`);
console.log(`[step-steps] ${declared} of ${declared + undeclared} machines rest at a declared spec step; ` +
            `${undeclared} still number their stages privately`);
if (twoList.length)
  console.log(`[step-steps] ${twoList.length} of the ${declared} declare their constants and their labels as ` +
              `TWO lists — one X-list expanded twice makes a renumber carry its label with it:\n  ` +
              twoList.join('\n  '));
