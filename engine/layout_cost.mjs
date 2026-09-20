/* HOW A RENDER'S COST GROWS WITH A DOCUMENT'S SHAPE — the derivation behind every layout-cost number this
 * tree quotes, as a command rather than as a figure.
 *
 * `node engine/layout_cost.mjs <binary> [maxN]`, after `node engine/build.mjs native`. It writes its own
 * fixtures, composes `test_forced.c`'s `--abi` record for each the way engine/one_document.mjs does, and
 * counts CALLS — not time — with gdb breakpoints, printing one row per function per shape.
 *
 * IT COUNTS AND DOES NOT TIME, WHICH IS THE WHOLE REASON IT CAN BE QUOTED AT ALL. CLAUDE.md's §Testing rules
 * out a wall clock for anything a loaded machine can bend, and this box is shared: several agents build and
 * drive on it at once, so an elapsed figure taken here is an artifact of the hour. A breakpoint hit count is
 * a property of the PROGRAM AND THE DOCUMENT and nothing else — it is identical under any load, on any core
 * count, at any niceness — so a number from this file is comparable across runs, across machines and across
 * revisions in a way no duration is.
 *
 * THE TWO SHAPES ARE THE EXPERIMENT AND NOT A SAMPLE. `flat` is N sibling boxes at a constant depth and
 * `deep` is N nested boxes with one child each; both hold the same number of elements and the same number of
 * text runs, so a column that grows differently between them is a fact about the SHAPE rather than about the
 * size. That is what separates the two multipliers a render pays: a cost per CONTAINER CHILD shows in `flat`
 * and is flat in `deep`, a cost per ANCESTOR shows in `deep` and is constant in `flat`, and a single
 * document of either kind cannot tell them apart. Real pages are deep; the fixtures a gate happens to carry
 * are usually flat, which is how a depth multiplier goes unmeasured.
 *
 * IT PRINTS DIFFERENCES BECAUSE THAT IS THE CLAIM. A column's second difference is zero exactly when it is
 * linear in N and its third is zero exactly when it is quadratic, so the table answers "is this still
 * superlinear" without anybody fitting a curve or trusting one. A row whose second difference is a non-zero
 * CONSTANT is quadratic; a row whose third is, is cubic. Those verdicts are printed beside the numbers.
 *
 * WHAT IT CANNOT SEE, stated because an instrument trusted past its evidence is worse than none: it counts
 * the functions named in `PROBES` and nothing else, so a cost that moved into a function not on that list
 * reads here as a cost that went away. The list is the contract — widen it in the same diff as any claim
 * that rests on it. It also says nothing about what a render PRODUCES: `engine/one_document.mjs` renders one
 * document and is the entry for that question. And gdb must be on PATH; there is no fallback, because a
 * silently skipped count and a count of zero are the same characters on a terminal.
 *
 * HOW TO READ A CHANGE BETWEEN TWO REVISIONS, WHICH IS THE ONLY THING ANYBODY EVER USES THIS FOR AND IS
 * WHERE A CORRECT DIFF GETS REVERTED. The ORDER is the claim. A COEFFICIENT is not, and a fix that collapses
 * a per-ANCESTOR cost lowers the coefficient at EVERY depth, because a constant-depth document still has
 * ancestors — `flat` here is depth THREE, not depth zero. Predicting that a depth fix leaves `flat` untouched
 * is therefore an over-claim that one run refutes, and refutes in the direction that argues for reverting
 * something correct.
 * MEASURED, on the commit that made this rule necessary: `flat block_flow_child_top` fell from `12N + 4` to
 * `2N + 2` while staying LINEAR, and `2N + 2` is EXACTLY one derivation per §10.1-second-case element per
 * pass — two passes over N divs plus the body — at every N from 1 to 6 with no residue. That is the memo
 * working at its limit, and a reader holding only "flat moved" would have called it a reach.
 * SO THE INVARIANT TO PREDICT IS TWO THINGS AND NEITHER IS "a row did not move": the ASK rows
 * (`element_view_bounding_box_px`, `bp_visit`) are UNCHANGED, because they say what the consumer asked for
 * and a diff that changes them changed the render rather than its cost; and NO ORDER moves where it was not
 * aimed. Both of those are falsifiable, and a DERIVATION row falling is what success looks like.
 *
 * RETIREMENT: this file goes when the engine publishes these counts itself for an arbitrary document — the
 * `_layout` census in `@RESULT` publishes two of them already — because the derivation is then a run of the
 * product rather than a debugger attached to it. */
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [bin, maxArg] = process.argv.slice(2);
if (!bin) {
  throw new Error('usage: node engine/layout_cost.mjs <binary> [maxN]\n' +
                  '  <binary> is engine/build.mjs native\'s output. maxN defaults to 6; the cost is\n' +
                  '  superlinear in N by construction, so raising it raises the run time faster than N.');
}
const MAX = Number(maxArg || 6);

/* The functions counted, and each is one question. `bf_layout` is CSS 2.1 §9.4.1 "Block formatting
   contexts"' walk and `bf_box` is one box's contribution to it; `block_flow_child_top`,
   `block_flow_auto_height` and `flow_border_box_origin` are the asks a render makes of geometry;
   `element_view_bounding_box_px` is CSSOM VIEW §6's rectangle a paint mark needs; `bp_visit` is CSS 2.1
   §E.2 "Painting order"'s offer, which is the DENOMINATOR the others are a cost per.
   THE ASKS AND THE WORK BENEATH THEM ARE BOTH HERE ON PURPOSE, because their ORDERS DIFFERING is the one
   reading that separates a multiplier from an inherent cost: a linear number of asks over quadratic work
   means a walk is re-deriving what another walk already established, and asks that are themselves
   superlinear mean the multiplier is in the caller instead. Neither number says it alone. */
const PROBES = ['bf_layout', 'bf_box', 'block_flow_child_top', 'block_flow_auto_height',
                'flow_border_box_origin', 'element_view_bounding_box_px', 'bp_visit',
                'used_value_border_edge_px'];

const flat = (n) => '<!DOCTYPE html><html><head><title>t</title></head><body>' +
  Array.from({ length: n }, (_, i) => `<div>r${i}</div>`).join('') + '</body></html>';
const deep = (n) => '<!DOCTYPE html><html><head><title>t</title></head><body>' +
  '<div>'.repeat(n) + 'x' + '</div>'.repeat(n) + '</body></html>';

/* The record's eleven facts are engine/one_document.mjs's `topLevelFacts`, copied for the reason that file
   states about copying them from engine/trusted.mjs: importing would mean loading a zone that fetches in
   order to count calls. `abi_take` refuses an absent field, so a record that has drifted short stops rather
   than seating a document on defaults. */
const b64 = (s) => Buffer.from(s).toString('base64');
const record = (html, url) => ['document', url, 'offline', b64('content-type: text/html; charset=utf-8\n'),
  b64(html), url, b64(''), '', 'unsafe-none', '', 'unsafe-none', '', 'u', 'null', 'none', 'none', '0'].join('\t') + '\n';

const dir = mkdtempSync(join(tmpdir(), 'layout-cost-'));
const counts = (html) => {
  const rec = join(dir, 'doc.rec');
  writeFileSync(rec, record(html, 'https://layout-cost.invalid/'));
  const script = join(dir, 'count.gdb');
  writeFileSync(script, ['set pagination off', 'set confirm off', 'set height 0',
    ...PROBES.map((p) => `break ${p}`),
    ...PROBES.map((_, i) => `ignore ${i + 1} 1000000000`)].join('\n') + '\n');
  const r = spawnSync('gdb', ['-batch', '-x', script,
    '-ex', `run --abi --paint-dir ${dir} < ${rec} > /dev/null`, '-ex', 'info breakpoints', bin],
    { encoding: 'utf8', maxBuffer: 1 << 28 });
  if (r.error) throw new Error(`gdb could not be run: ${r.error.message}`);
  const out = { ...Object.fromEntries(PROBES.map((p) => [p, 0])) };
  let cur = null;
  for (const line of (r.stdout + r.stderr).split('\n')) {
    const m = /in (\w+) at/.exec(line);
    if (m && out[m[1]] !== undefined) cur = m[1];
    const h = /already hit (\d+) time/.exec(line);
    if (h && cur) { out[cur] = Number(h[1]); cur = null; }
  }
  if (PROBES.every((p) => out[p] === 0)) {
    throw new Error('every probe counted zero, which no run that painted can produce — the binary is not a ' +
                    'native build with symbols, or --paint-dir wrote nothing. A zero table is reported as an ' +
                    'error rather than printed, because it reads exactly like a cost that went away.');
  }
  return out;
};

/* The order of a column, read off its own differences rather than fitted: the k-th difference of a degree-k
   polynomial is a non-zero constant and the (k+1)-th is zero. Answers `unknown` where there are too few
   points to decide, which is a statement about maxN and never about the column. */
const order = (v) => {
  let d = v.slice();
  /* THE LOOP RUNS ONE FURTHER THAN THE HIGHEST DEGREE IT NAMES, which is the off-by-one this verdict was
     shipped with for exactly one run: degree k is established by the (k+1)-th difference being ZERO, not by
     the k-th being constant, so stopping at k = 3 reported a genuinely CUBIC column as `higher than cubic`.
     That is the direction that costs something — a reader is told a cost is worse than it is and goes
     looking for a multiplier that is not there — and it was caught by running the instrument against a
     column whose order was already known, which is the only reason to have such a column. */
  for (let k = 0; k <= 4; k++) {
    if (d.length === 0) return 'unknown (too few N to decide — raise maxN)';
    if (d.every((x) => x === 0)) return k === 0 ? 'zero' : ['constant', 'linear', 'quadratic', 'cubic'][k - 1];
    d = d.slice(1).map((x, i) => x - d[i]);
  }
  return 'higher than cubic';
};

try {
  const ns = Array.from({ length: MAX }, (_, i) => i + 1);
  for (const [name, gen] of [['flat (N siblings, constant depth)', flat], ['deep (N nested, one child each)', deep]]) {
    const rows = ns.map((n) => counts(gen(n)));
    console.log(`\n${name} — CALL COUNTS, N = ${ns.join(' ')}`);
    for (const p of PROBES) {
      const v = rows.map((r) => r[p]);
      console.log(`  ${p.padEnd(30)} ${v.map((x) => String(x).padStart(6)).join('')}   ${order(v)} in N`);
    }
  }
  console.log('\nA row reading `linear in N` is a cost per box. `quadratic` is a cost per box times a cost');
  console.log('per container child (flat) or per ancestor (deep); `cubic` is both at once. `bp_visit` is');
  console.log('CSS 2.1 §E.2\'s offer count and is linear by construction — a row above it that is not is a');
  console.log('cost this render pays per offer rather than per document.');
  console.log('\nREADING A CHANGE BETWEEN TWO REVISIONS: the ORDER is the claim and a COEFFICIENT is not.');
  console.log('A fix that collapses a per-ANCESTOR cost lowers the coefficient at EVERY depth — a constant-');
  console.log('depth document still has ancestors — and changes the ORDER only where the depth grows. So');
  console.log('`the other shape must not move` is the wrong invariant to predict and refutes correct diffs;');
  console.log('what must hold is that the ASK rows are unchanged and no order moves where it was not aimed.');
} finally {
  rmSync(dir, { recursive: true, force: true });
}
