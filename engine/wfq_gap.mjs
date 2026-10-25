/* The gap splits: the @WFQ census's `curDeepTerms` and `neverPickedTerms` rows (solver/result.c) carry the operands
 * of the two weights `curDeepWGap` and `neverPickedGap` are the differences of, and this recomposes each weight in
 * solver/flow.c's flow_weight_recompose order, so the doubles agree to the bit, and says how much of a gap each term
 * holds.
 *   node engine/wfq_gap.mjs <run-output>   prints one line per @WFQ census that carries the row. */

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const END_FIELDS = ["w", "reward", "coordNotch", "ownNotch", "visits", "orphanSeen", "born", "replay", "surv",
                    "rung", "picks", "hostOwed", "running"];

/* The five terms of one member's weight, each as flow_order_key composes it, and the weight they sum to. */
function termsOf(o, q, rungs, where) {
  for (const k of END_FIELDS)
    if (typeof o[k] !== "number")
      throw new Error(`${where}: a curDeepTerms end carries no number \`${k}\` — solver/result.c's ` +
                      `weight_operands_json writes all of ${END_FIELDS.join(", ")}`);
  const coord = o.reward - o.coordNotch * q;
  const branch = 1.0 / o.born;
  const own = -o.ownNotch * q;
  const opt = 1.0 / (1.0 + o.visits + o.orphanSeen);
  const dist = (o.replay + o.surv + o.rung) / rungs;
  const w = coord + (branch + (own + opt + dist));
  if (w !== o.w)
    throw new Error(`${where}: a curDeepTerms end does not recompose to its weight (published ${o.w}, ` +
                    `recomposed ${w}) — this reader and solver/flow.c's flow_weight_recompose compose ` +
                    `different expressions`);
  return { coord, branch, own, opt, dist, w };
}

/* The split of one gap row: each term's difference between the front (`top`) and the gap's far end (`far`), and
   the gap recomposed exactly from the two weights. Null when the census carries no such row. */
function termSplit(t, far, row, where) {
  if (t === undefined || t === null) return null;
  if (typeof t.ageQuantum !== "number" || typeof t.rungs !== "number" || typeof t.gap !== "number")
    throw new Error(`${where}: ${row} carries no number ageQuantum, rungs and gap`);
  const top = termsOf(t.top, t.ageQuantum, t.rungs, where + " (top)");
  const end = termsOf(t[far], t.ageQuantum, t.rungs, where + ` (${far})`);
  if (top.w - end.w !== t.gap)
    throw new Error(`${where}: the ${row} gap recomposed from its two weights (${top.w - end.w}) is not the ` +
                    `gap the census computed (${t.gap})`);
  const d = {};
  for (const k of ["coord", "branch", "own", "opt", "dist"]) d[k] = top[k] - end[k];
  return { gap: t.gap, terms: d, top: t.top, far: t[far] };
}

/* curDeepWGap's split: the front against the best member at the deepest program row. */
export function curDeepTermSplit(census, where) {
  return termSplit(census.curDeepTerms, "deep", "curDeepTerms", where);
}

/* neverPickedGap's split: the front against the best member never handed the thread. */
export function neverPickedTermSplit(census, where) {
  return termSplit(census.neverPickedTerms, "never", "neverPickedTerms", where);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const path = process.argv[2];
  if (!path) { console.error("usage: node engine/wfq_gap.mjs <run-output>"); process.exit(2); }
  const text = readFileSync(path, "utf8");
  for (const m of text.matchAll(/@WFQ (\{.*\})$/gm)) {
    let c;
    try { c = JSON.parse(m[1]); } catch { continue; }
    const f = (x) => x.toFixed(6);
    const end = (e) => `born ${e.born} visits ${e.visits} k ${e.ownNotch} K+c ${e.coordNotch} picks ${e.picks} ` +
                       `owed ${e.hostOwed} running ${e.running}`;
    for (const [name, r] of [["curDeep", curDeepTermSplit(c, `${path} census ${c.censusSeq}`)],
                             ["neverPicked", neverPickedTermSplit(c, `${path} census ${c.censusSeq}`)]]) {
      if (!r) continue;
      console.log(`census ${c.censusSeq} ${name} gap ${f(r.gap)} = coord ${f(r.terms.coord)} + branch ` +
                  `${f(r.terms.branch)} + own ${f(r.terms.own)} + opt ${f(r.terms.opt)} + dist ${f(r.terms.dist)} | ` +
                  `top ${end(r.top)} | far ${end(r.far)}`);
    }
  }
}
