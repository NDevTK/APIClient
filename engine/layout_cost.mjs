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
import { topLevelFactFields } from './top_level_facts.mjs';

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
/* AND THE CASCADE BENEATH THEM, WHICH IS A COST PER ASK RATHER THAN A COST PER BOX AND WAS NOT ON THIS LIST
   WHILE IT WAS THE LARGEST THING A RENDER PAID. `cssom_cascaded_value` is css-cascade-5 §6 "Cascading"'s sort
   for ONE (element, property) and is the work every row above it triggers; `css_computed_value` and
   `css_cv_specified` are css-cascade-5 §7 "Defaulting"'s two halves above it, and the difference between them
   is §7.2's ancestor climb; `css_logical_partner_of` is css-logical-1 §4's pairing, which is a PREREQUISITE of
   the cascade and is itself two more computed values that inherit. `cssd_ua_value` and
   `css_presentational_hint` are the two flat scans each resolution runs, so a row of either that is LARGER
   than `cssom_cascaded_value` is the logical partner being asked for as well — which is a reading, not a
   defect. `lxb_css_stylesheet_parse` is the sheet re-parse a resolution performs when the document HAS author
   style, and it reads ZERO for a fixture that declares none: these two shapes declare none, so that row is
   the instrument saying which half of the cost it is looking at rather than saying the half is free.
   THE READING IS THE PAIR OF ORDERS AND NEVER ONE NUMBER: `bp_visit` is linear by construction, so a cascade
   row above it that is quadratic in `flat` names a per-SIBLING factor and one that rises to cubic in `deep`
   names a per-ANCESTOR factor on top of it. */
/* AND THE SIZE DERIVATION UNDER THOSE ASKS, PLUS THE CHAIN THAT RECORDS IT — `used_value_px`, `uv_sized` and
   `layout_question_repeat`, named rather than counted because a count beside its own list is the one claim
   here nobody adds up. They are on it because the list WITHOUT them supports a reading of this table that is
   false about the render, and the banner's blind-spot
   paragraph above says why in advance: a cost living in a function not on this list reads here as a cost that
   went away.
   MEASURED, and it is the reading the flat/deep split exists to make: `uv_sized` is LINEAR in `flat`
   (90 150 210 270 330 390) and QUADRATIC in `deep` (90 154 234 330 442 570), and so is the chain beside it.
   AN ORDER THAT MOVES BETWEEN THE TWO SHAPES IS A COST PER ANCESTOR, and NOT ONE ROW OF THE OLDER LIST MOVED
   ITS ORDER AT ALL — so a reader of that table could say `no order moved anywhere`, be right about every row
   in front of them, and be wrong about the engine. That sentence was in fact written and relayed.
   THE SHARPER READING IS THE PAIR, which is this instrument's own rule, AND THE `deep` TABLE DRAWS THE LINE
   BY ITSELF: `bf_layout`, `bf_box`, `bp_visit` and `used_value_border_edge_px` are ALL LINEAR there, and
   `used_value_px` and `uv_sized` directly beneath them are BOTH QUADRATIC. So the per-ancestor multiplier
   sits strictly INSIDE the used-value cluster — below every ask on this list and above nothing on it — and
   the border-edge entry reaches `uv_sized` UNCONDITIONALLY, through the box-edge helper, the content size
   and the used-value ask, with no branch anywhere on that path. A LINEAR number of asks over a QUADRATIC
   number of derivations is this file's own words for a walk re-deriving what another walk established, and
   until these rows were added no row here could state which side of the boundary it was on.
   `layout_question_repeat` IS ALSO THE WAY A CHAIN DIFF IS SCORED, and that is its second reason for being
   here: it counts the dev-only cycle test's asks, so declaring a question kind at a new entry MUST move it by
   that entry's own count and nothing else — a conservation identity a reader checks inside ONE table rather
   than across two runs. Its siblings `_push` and `_pop` are deliberately NOT on the list: they are equal to it
   on every healthy run, so carrying them would be three rows of one fact, and the identity is worth asserting
   in a targeted run rather than printed three times on every one.
   BOTH ROWS PASS THIS FILE'S OWN ADMISSION TEST, which is the one the dropped rule-list probe failed: each
   reads the SAME NUMBER TWICE, reproduced to the digit under two differently-composed probe sets, so neither
   is a single-location parse that happened to work once. */
/* AND §10.1's CHAIN ITSELF, WHICH IS WHERE THE PER-ANCESTOR MULTIPLIER IS PAID AND WHICH NO ROW ABOVE COULD
   NAME. The rows above localise the cost to the used-value cluster and stop there — `used_value_px` and
   `uv_sized` are QUADRATIC in `deep` while every ask over them is LINEAR — so the multiplier is below every
   ask on that list and above nothing on it, and not one row states which LINK pays it. These eleven do.
   `uv_px_ask` is `used_value_px`'s body under its chain node and must EQUAL it in every shape;
   `uv_pass_size` is CSS 2.1 §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'"' step 1 and
   equals `uv_sized` exactly when that section's two re-runs never fired; `uv_block_auto_width` is CSS 2.1
   §10.3.3 "Block-level, non-replaced elements in normal flow"' constraint equation, which is the arm a plain
   `<div>` takes; `used_value_containing_block_width` is CSS 2.1 §10.1 "Definition of 'containing block'"'
   answer and `uv_cb` is the walk that picks its case; `uv_surround`, `uv_margin` and `uv_edge_px` are the
   per-derivation reads every arm makes, which is what makes `used_value_px` a MULTIPLE of `uv_sized` rather
   than equal to it.
   `uv_icb` IS THE ROW THAT DECIDES IT, AND IT DECIDES IT INSIDE ONE TABLE RATHER THAN ACROSS TWO RUNS. It is
   §10.1's FIRST case — the viewport — which is the chain's BASE, so its count is the number of climbs that
   REACHED the base. A cost inherent to the algorithm runs one climb per ask and every climb reaches the
   base, so the base count and the step count have the SAME ORDER. MEASURED on `deep`: `uv_icb` is LINEAR
   (16N + 22) while `used_value_containing_block_width` is QUADRATIC (8N*N + 42N + 40). A LINEAR number of
   climbs taking a QUADRATIC number of steps is a climb whose LENGTH grows with the document, which is the
   per-ancestor factor stated as arithmetic over two rows rather than as a reading of one.
   AND THE CONTROL IS IN THE SAME TABLE, WHICH IS THE WHOLE REASON THE TWO `flow_placement` ROWS ARE HERE.
   CSS 2 §8.1 "Box dimensions"' border-box origin is the SAME §10.1 recursion over ancestors, asked by the
   same walk, inside the same span — and it is MEMOIZED. MEASURED on `deep`: `flow_border_box_origin` is
   6N + 10 and LINEAR, `flow_placement_origin_ask` is the same 6N + 10, and `flow_placement_origin_record` —
   the DERIVATIONS — is 2N + 4, so the record SERVED 4N + 6 and the pass was OPEN AND ANSWERING throughout
   the run the width chain climbed through. Two §10.1 recursions, one document, one binary, one span: the one
   routed through the record is LINEAR and the one that is not is QUADRATIC. No artifact of the hour, the
   machine or the revision can produce that pair, which is what this instrument is for.
   SO THE VERDICT IS NEITHER `THE ALGORITHM` NOR `A BUG` AND SAYING WHICH HALF IS WHICH IS THE POINT. The
   per-ancestor CONSULTATION is the ALGORITHM: §10.3.3's equation genuinely reads §10.1's rectangle and a box
   at depth d genuinely depends on d ancestors, so a fix that stopped consulting them would be wrong. The
   per-ancestor RE-DERIVATION is the DEFECT, and it is the reading where the mechanism EXISTS, is REACHED,
   and this question was never ROUTED to it — not the one where it exists, is reached, and correctly
   declines, which is the reading a persistent row invites and which `flow_placement`'s served count refutes
   in the same table.
   RETIREMENT: these eleven go when `_layout`'s own rows state each chain's ORDER for an arbitrary document.
   The census beside them publishes §10.1's width as an ASK and a DERIVATION, which is the two ends of a
   climb and not the STEPS between them, so the rows that name the LINK stay until the steps are published
   too. */
const PROBES = ['bf_layout', 'bf_box', 'block_flow_child_top', 'block_flow_auto_height',
                'flow_border_box_origin', 'element_view_bounding_box_px', 'bp_visit',
                'used_value_border_edge_px', 'used_value_px', 'uv_sized', 'layout_question_repeat',
                'cssom_cascaded_value', 'css_computed_value', 'css_cv_specified',
                'css_logical_partner_of', 'cssd_ua_value', 'css_presentational_hint',
                'lxb_css_stylesheet_parse', 'cascade_emit', 'cssd_sheet_parsed',
                'style_sheet_list_add',
                'uv_px_ask', 'uv_pass_size', 'uv_block_auto_width',
                'used_value_containing_block_width', 'uv_cb', 'uv_icb',
                'uv_surround', 'uv_margin', 'uv_edge_px',
                'flow_placement_origin_ask', 'flow_placement_origin_record'];

const flat = (n) => '<!DOCTYPE html><html><head><title>t</title></head><body>' +
  Array.from({ length: n }, (_, i) => `<div>r${i}</div>`).join('') + '</body></html>';
const deep = (n) => '<!DOCTYPE html><html><head><title>t</title></head><body>' +
  '<div>'.repeat(n) + 'x' + '</div>'.repeat(n) + '</body></html>';

/* THE THIRD SHAPE, AND IT IS A DIFFERENT AXIS FROM THE OTHER TWO ON PURPOSE. `flat` and `deep` differ in the
   document's SHAPE and declare no author style at all, which is what lets a column that grows differently
   between them be a fact about the shape. `styled` is `deep` with ONE author sheet, and it differs on the
   axis those two hold fixed: whether css-cascade-5 §6.2 "Cascading Origins"' AUTHOR ORIGIN has anything in
   it. Without it the sheet-path rows read zero, and a zero there is the instrument saying it is looking at
   the other half of the cost rather than saying this half is free — which is a silence a reader is entitled
   to mistake for a clean bill.
   IT IS `deep` AND NOT A FOURTH GENERATOR so the pair is controlled: every row here is comparable with the
   same row of `deep` at the same N, and the only difference between them is the sheet. What that comparison
   answers is how much of the author-origin path a resolution runs, which no shape axis can reach.
   READ `cascade_emit` AGAINST `cssom_cascaded_value`: the first is the walk that SERIALIZES a sheet's rule
   objects for the cascade to read, and if the two are EQUAL then the whole sheet is flattened once per
   (element, property) resolution rather than once per render. `lxb_css_stylesheet_parse` is the re-parse of
   that text and is gated on the flatten having produced any, so the two are NOT one number and a zero in the
   second with a nonzero first is a real state: rules built, sheet in the list, nothing emitted.
   AND READ `lxb_css_stylesheet_parse` AGAINST `cssd_sheet_parsed`, WHICH IS THE PAIR AND NOT TWO ROWS. The
   second is every ASK the author walk makes of core/css/css_style_declaration.c's per-sheet parse table —
   hits and misses together — and the first is what a miss costs, so their DIFFERENCE is what the table
   served and a run where they are EQUAL is a table serving nothing. The emission row is the control for
   both: `cascade_emit` is unchanged by that table BY CONSTRUCTION, because the emission is what produces the
   key, so a diff that moves it moved something else. `lxb_css_stylesheet_parse` is not exclusive to that
   path — `cssom_parse_rules` calls it too, for CSS Syntax's "parse a stylesheet's contents" — so on the `styled`
   shape its floor is the sheets the document builds rather than zero.
   A PROBE IS ON THIS LIST ONLY IF IT READS THE SAME NUMBER TWICE, which is a rule about the READER below and
   not about the symbol. `css_rule_list_new` was on it and is not: over one fixture on one binary it answered
   15997 under one probe set and 0 under another, and a call count cannot legitimately differ between two
   readings of one run — so one of them is this file's `info breakpoints` parse, which pairs an `in <fn> at`
   line with the `already hit N time` line after it and has nothing to say about a breakpoint gdb resolved to
   SEVERAL locations. Every other row reproduced to the digit across both sets, so the parse is sound for a
   single-location symbol and is the thing to fix before that probe comes back. Dropping it is not a
   judgement about the cost it would have measured; it is that a row nobody can reproduce is worse than an
   absent one, because the absent one does not get quoted. */
const styled = (n) => '<!DOCTYPE html><html><head><title>t</title>' +
  '<style>div{color:red}p{margin-top:2px}.q{padding-left:1px}</style></head><body>' +
  '<div>'.repeat(n) + 'x' + '</div>'.repeat(n) + '</body></html>';

/* The record's eleven facts are `engine/top_level_facts.mjs`'s, which is the ONE statement of HTML §7.5.1's
   answers for a document nothing embeds.
   THE COMMENT THIS REPLACES IS REWRITTEN RATHER THAN DELETED, because its reasoning is what the next reader
   re-derives and the conclusion it drew from that reasoning is the thing that was wrong. It said the eleven
   were "engine/one_document.mjs's `topLevelFacts`, copied for the reason that file states about copying them
   from engine/trusted.mjs: importing would mean loading a zone that fetches in order to count calls" — a
   retired argument inherited at one remove, and the clearest evidence that the copy propagates: `one_document`
   copied `trusted`, this copied `one_document`, and each carried the previous one's justification forward
   without re-deriving it. What it establishes is that the facts must not come from THE ZONE; it establishes
   nothing about copying, and the module fetches nothing. Its claim about `abi_take` is also only half the
   protection it sounds like — that entry refuses an ABSENT field and nothing refuses a SHORT record, so a
   drifted one seats a document with every later fact read one slot early; the module refuses both directions
   of that skew at the composer, which is why it is a narrowing rather than a relocation. */
const b64 = (s) => Buffer.from(s).toString('base64');
const record = (html, url) => ['document', url, 'offline', b64('content-type: text/html; charset=utf-8\n'),
  b64(html), ...topLevelFactFields(url), '0'].join('\t') + '\n';

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
  for (const [name, gen] of [['flat (N siblings, constant depth)', flat],
                             ['deep (N nested, one child each)', deep],
                             ['styled (deep, plus ONE author style sheet)', styled]]) {
    const rows = ns.map((n) => counts(gen(n)));
    console.log(`\n${name} — CALL COUNTS, N = ${ns.join(' ')}`);
    for (const p of PROBES) {
      const v = rows.map((r) => r[p]);
      console.log(`  ${p.padEnd(30)} ${v.map((x) => String(x).padStart(6)).join('')}   ${order(v)} in N`);
    }
    /* THE CHAIN'S OWN CONSERVATION IDENTITY, PRINTED RATHER THAN INFERRED. Every question the dev-only cycle
       test is asked comes from an entry that DECLARES a kind, and there is no other way to reach it — the
       macro is expanded at the asking sites and nowhere else. So the ask count must equal the sum of those
       entries' own call counts, EXACTLY, at every N. It is printed per shape because that is a check a reader
       makes inside ONE table: a cross-run comparison of this number is a comparison of two binaries, and this
       one is a comparison of a run with itself.
       IT IS HOW A QUESTION-KIND DIFF IS SCORED. Declaring a kind at a new entry moves the ask count by that
       entry's own count and by nothing else, so a node that was never reached leaves the identity SHORT by
       exactly that entry's row — which is why `no assert fired` is not the claim to make about such a diff
       and this is. A kind whose entry is NOT on the list above makes it short too, and that is the same
       finding wearing the other hat: the list and the declared kinds are one contract.
       IT REPORTS RATHER THAN THROWING, like every other verdict here — a shortfall is a real reading about a
       real run and not a meaningless table, which is the one case this file does refuse to print. */
    {
      const parts = ['used_value_px', 'uv_sized', 'bf_box'].filter((k) => PROBES.includes(k));
      if (PROBES.includes('layout_question_repeat')) {
        const bad = ns.map((_, i) => [i + 1, rows[i].layout_question_repeat,
                                      parts.reduce((a, k) => a + rows[i][k], 0)])
                      .filter(([, got, want]) => got !== want);
        console.log(`  chain identity: layout_question_repeat == ${parts.join(' + ')}  ` +
          (bad.length === 0
            ? `HOLDS at every N`
            : `SHORT/LONG at ${bad.map(([n, g, w]) =>
                `N=${n} (${g} vs ${w}, ${g - w >= 0 ? '+' : ''}${g - w})`).join(', ')}`));
      }
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
