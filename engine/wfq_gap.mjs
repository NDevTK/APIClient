/* The curDeepWGap split: the @WFQ census's `curDeepTerms` row (solver/result.c) carries the operands of the two
 * weights `curDeepWGap` is the difference of, and this recomposes each weight in solver/flow.c's
 * flow_weight_recompose order, so the doubles agree to the bit, and says how much of the gap each term holds.
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

/* The split of one census's gap: each term's difference between the front and the best deepest member, and the
   gap recomposed exactly from the two weights. Null when the census carries no `curDeepTerms`. */
export function curDeepTermSplit(census, where) {
  const t = census.curDeepTerms;
  if (t === undefined || t === null) return null;
  if (typeof t.ageQuantum !== "number" || typeof t.rungs !== "number" || typeof t.gap !== "number")
    throw new Error(`${where}: curDeepTerms carries no number ageQuantum, rungs and gap`);
  const top = termsOf(t.top, t.ageQuantum, t.rungs, where + " (top)");
  const deep = termsOf(t.deep, t.ageQuantum, t.rungs, where + " (deep)");
  if (top.w - deep.w !== t.gap)
    throw new Error(`${where}: the curDeepWGap recomposed from its two weights (${top.w - deep.w}) is not the ` +
                    `gap the census computed (${t.gap})`);
  const d = {};
  for (const k of ["coord", "branch", "own", "opt", "dist"]) d[k] = top[k] - deep[k];
  return { gap: t.gap, terms: d, top: t.top, deep: t.deep };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const path = process.argv[2];
  if (!path) { console.error("usage: node engine/wfq_gap.mjs <run-output>"); process.exit(2); }
  const text = readFileSync(path, "utf8");
  for (const m of text.matchAll(/@WFQ (\{.*\})$/gm)) {
    let c;
    try { c = JSON.parse(m[1]); } catch { continue; }
    const r = curDeepTermSplit(c, `${path} census ${c.censusSeq}`);
    if (!r) continue;
    const f = (x) => x.toFixed(6);
    console.log(`census ${c.censusSeq} curDeep ${c.curDeep} live ${c.curDeepLive} gap ${f(r.gap)} = coord ${f(r.terms.coord)} ` +
                `+ branch ${f(r.terms.branch)} + own ${f(r.terms.own)} + opt ${f(r.terms.opt)} + dist ${f(r.terms.dist)} | ` +
                `top born ${r.top.born} visits ${r.top.visits} k ${r.top.ownNotch} K+c ${r.top.coordNotch} picks ` +
                `${r.top.picks} owed ${r.top.hostOwed} running ${r.top.running} | deep born ${r.deep.born} visits ` +
                `${r.deep.visits} k ${r.deep.ownNotch} K+c ${r.deep.coordNotch} picks ${r.deep.picks} owed ` +
                `${r.deep.hostOwed} running ${r.deep.running}`);
  }
}
