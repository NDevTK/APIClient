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
 * What this does NOT check, stated so it is not mistaken for more than it is: a machine with no declaration at
 * all is not an error here. That is the state every machine starts in, and it already fails loudly and
 * precisely — at the fork, with a DCHECK naming the machine — which is the right place for it. This file is
 * only about a declaration that is present and attached to the wrong thing.
 */
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const SRC = join(dirname(fileURLToPath(import.meta.url)), 'qjs', 'quickjs.c');
const src = readFileSync(SRC, 'utf8');

const byStruct = new Map();   // state struct -> Set(visit)
const byVisit = new Map();    // visit -> Set(state struct)
let checked = 0;

const DEF = /static const JSTrampStepDef (js_\w+)(?:\[[^\]]*\])?\s*=\s*\{([^}]*)\}/g;
for (const m of src.matchAll(DEF)) {
  const [, name, body] = m;
  const visit = body.match(/\.visit\s*=\s*(\w+)/);
  if (!visit) continue;                       // undeclared: the fork's own DCHECK owns that case
  const struct = body.match(/sizeof\((\w+)\)/);
  if (!struct) {
    console.error(`[step-visits] ${name} declares a visit but its state struct is not a plain sizeof(T) — ` +
                  `the pairing cannot be checked, so it cannot be trusted`);
    process.exit(1);
  }
  checked++;
  if (!byStruct.has(struct[1])) byStruct.set(struct[1], new Set());
  if (!byVisit.has(visit[1])) byVisit.set(visit[1], new Set());
  byStruct.get(struct[1]).add(visit[1]);
  byVisit.get(visit[1]).add(struct[1]);
}

let bad = 0;
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
