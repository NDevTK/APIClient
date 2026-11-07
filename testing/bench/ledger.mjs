/* What the pick ledger (engine/host/solver/pick_ledger.h) says about the Level 2 terms: for each term, alone and as
 * the picked member's rank among a sample of the frontier at the pick, how well it orders the picks by the novel
 * yield their tenures produced per unit of work. A reading, not a model: it decides nothing.
 *   node testing/bench/ledger.mjs <bench-out-dir>… [--bins <term>[,<term>…]] [--all-sales]
 * It reads the logs/<page>.<run>.picks.tsv.gz of every run runs.jsonl records as finished. A tenure is counted only when it closed before its
 * instance's first memory-pressure sale (`endSold` 0), because past a sale the picks depend on memory; `--all-sales`
 * counts every tenure.
 *
 * The statistic is a concentration index over work: order the picks by the term, highest first (ties as one block),
 * and walk the cumulative share of work against the cumulative share of yield. Twice the area under that curve less
 * one is +1 when the highest values hold all the yield in no work, 0 when the term orders nothing, and -1 when the
 * lowest values do. It weighs a pick by its work, so it asks the question the scheduler's currency asks. */

import { readFileSync, readdirSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { join } from 'node:path';

function args(argv) {
  const o = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    if (!argv[i].startsWith('--')) { o._.push(argv[i]); continue; }
    const k = argv[i].slice(2);
    if (k === 'all-sales') { o[k] = true; continue; }
    if (argv[i + 1] === undefined) throw new Error(`--${k} takes a value`);
    o[k] = argv[++i];
  }
  return o;
}

const need = (ok, what) => { if (!ok) throw new Error(`a pick-ledger stream breaks its contract: ${what}`); };

/* The terms read off a row, each as the scheduler's own quantity. `rank` names the row's frontier-rank column. */
const TERMS = [
  { name: 'w', rank: 'r_w' }, { name: 'reward', rank: 'r_reward' }, { name: 'famNotch', rank: 'r_famNotch' },
  { name: 'x', rank: 'r_x' }, { name: 'served', rank: 'r_served' }, { name: 'seen' },
  { name: 'priced', rank: 'r_priced' }, { name: 'ikey', rank: 'r_ikey' }, { name: 'turn', rank: 'r_turn' },
  { name: 'ownSilence', rank: 'r_ownSilence' }, { name: 'seq', rank: 'r_seq' }, { name: 'visits' },
  { name: 'picks' }, { name: 'prevTenureWork' }, { name: 'onLedger' }, { name: 'displaced' },
  /* Not a term: the pick's place in its instance's run, read beside the terms because most of them move with it. */
  { name: 'at' },
];
const CLS = [['candidate', 1], ['drive', 2], ['post', 4], ['victim', 8], ['attacker', 16], ['forced', 32],
             ['observed', 64]];
const OUTCOMES = ['paid', 'value', 'newSites'];

/* One run's stream: every instance's rows, as numeric columns. */
function readRun(file) {
  const text = gunzipSync(readFileSync(file)).toString('utf8');
  const cols = new Map();
  const inst = new Map();
  for (const line of text.split('\n')) {
    if (line === '') continue;
    const t1 = line.indexOf('\t'), t2 = line.indexOf('\t', t1 + 1);
    const itag = line.slice(0, t1), marker = line.slice(t1 + 1, t2), body = line.slice(t2 + 1).split('\t');
    if (marker === '@PICKCOLS') { cols.set(itag, body); continue; }
    need(marker === '@PICK', `an unknown marker ${marker}`);
    const names = cols.get(itag);
    need(names !== undefined && names.length === body.length, `a row of [${itag}] before or unlike its @PICKCOLS`);
    if (!inst.has(itag)) inst.set(itag, []);
    const r = {};
    names.forEach((n, i) => { r[n] = body[i]; });
    inst.get(itag).push(r);
  }
  return { cols, inst };
}

/* A row as the numbers this reader uses; the kinds are whatever the producer named `paid_<kind>`. */
function rowOf(r, prevWork) {
  const num = (k) => { const v = Number(r[k]); need(r[k] !== undefined && Number.isFinite(v), `${k} is a number`); return v; };
  const kinds = Object.keys(r).filter((k) => k.startsWith('paid_')).map((k) => k.slice(5));
  let paid = 0, landed = 0, razor = 0;
  const paidBy = {};
  for (const k of kinds) {
    paidBy[k] = num(`paid_${k}`);
    paid += paidBy[k]; landed += num(`landed_${k}`); razor += num(`razor_${k}`);
  }
  const rank = {};
  const eligible = num('eligible');
  for (const t of TERMS) {
    if (!t.rank) continue;
    const m = /^(\d+):(\d+)$/.exec(r[t.rank]);
    need(m !== null, `${t.rank} is gt:eq`);
    /* The share of sampled indexed members the picked one stands at or above, ties counted half. */
    rank[t.name] = eligible === 0 ? NaN : 1 - (Number(m[1]) + Number(m[2]) / 2) / eligible;
  }
  const seq = num('seq');
  return {
    seq, work: num('endWork') - num('work'), at: num('work'), sold: num('sold'), endSold: num('endSold'),
    cls: num('cls'), eligible, rank,
    v: {
      w: num('w'), reward: num('reward'), famNotch: num('famNotch'), x: num('x'), served: num('served'),
      seen: num('seen'), priced: num('assumed') + num('againstEnv'), ikey: num('ikey'), turn: num('turn'),
      ownSilence: num('ownSilence'), seq, visits: num('visits'), picks: num('picks'),
      prevTenureWork: prevWork.has(seq) ? prevWork.get(seq) : NaN, onLedger: num('onLedger'),
      displaced: num('displaced'), at: num('work'),
    },
    y: { paid, value: razor + 0.25 * (landed - razor), newSites: num('newSites') },
    paidBy, landed, razor,
  };
}

/* The concentration index of `rows` ordered by `key` (highest first, NaN keys left out) for outcome `out`. */
function concentration(rows, key, out) {
  const xs = rows.filter((r) => Number.isFinite(key(r)));
  let W = 0, Y = 0;
  for (const r of xs) { W += r.work; Y += r.y[out]; }
  const yielding = xs.filter((r) => r.y[out] > 0).length;
  if (xs.length === 0 || W === 0 || Y === 0) return { n: xs.length, yielding, c: null };
  xs.sort((a, b) => key(b) - key(a));
  let cw = 0, cy = 0, area = 0;
  for (let i = 0; i < xs.length;) {
    let j = i, gw = 0, gy = 0;
    while (j < xs.length && key(xs[j]) === key(xs[i])) { gw += xs[j].work; gy += xs[j].y[out]; j++; }
    area += (gw / W) * (2 * cy + gy) / (2 * Y);   /* the trapezoid over this tie block */
    cw += gw; cy += gy; i = j;
  }
  return { n: xs.length, yielding, c: +(2 * area - 1).toFixed(3) };
}

/* The same index read inside time: each page's picks cut into five strata of equal work by their work coordinate,
   the index taken in each, and the strata's indices averaged weighted by their yield. Most terms move with the run's
   age, and early picks yield most, so the pooled index partly says when a pick happened; this one does not. */
function withinTime(pages, key, out) {
  let wsum = 0, csum = 0, yielding = 0;
  for (const rows of pages.values()) {
    const xs = [...rows].sort((a, b) => a.v.at - b.v.at);
    const W = xs.reduce((n, r) => n + r.work, 0);
    for (let s = 0, i = 0, acc = 0; s < 5 && i < xs.length; s++) {
      const part = [];
      while (i < xs.length && (acc < ((s + 1) * W) / 5 || s === 4)) { part.push(xs[i]); acc += xs[i].work; i++; }
      const c = concentration(part, key, out);
      if (c.c === null) continue;
      const y = part.reduce((n, r) => n + r.y[out], 0);
      wsum += y; csum += y * c.c; yielding += c.yielding;
    }
  }
  return { c: wsum === 0 ? null : +(csum / wsum).toFixed(3), yielding };
}

/* A group's yield rate against the rest's inside time: every page's picks cut into the same five equal-work strata as
   withinTime, and the Mantel-Haenszel rate ratio over the strata where both sides ran,
   sum(y1 * w0 / W) / sum(y0 * w1 / W), which a stratum where one side ran little cannot dominate; 1 says the group
   yields per unit of work as the rest of its time does. The strata it rests on and both sides' work and yield are
   stated beside it. */
function groupWithinTime(pages, inGroup, out) {
  const o = { strata: 0, work1: 0, work0: 0, y1: 0, y0: 0, num: 0, den: 0 };
  for (const rows of pages.values()) {
    const xs = [...rows].sort((a, b) => a.v.at - b.v.at);
    const W = xs.reduce((n, r) => n + r.work, 0);
    for (let s = 0, i = 0, acc = 0; s < 5 && i < xs.length; s++) {
      let w1 = 0, w0 = 0, y1 = 0, y0 = 0;
      while (i < xs.length && (acc < ((s + 1) * W) / 5 || s === 4)) {
        const r = xs[i];
        if (inGroup(r)) { w1 += r.work; y1 += r.y[out]; } else { w0 += r.work; y0 += r.y[out]; }
        acc += r.work; i++;
      }
      if (w1 === 0 || w0 === 0) continue;
      o.strata++; o.work1 += w1; o.work0 += w0; o.y1 += y1; o.y0 += y0;
      o.num += (y1 * w0) / (w1 + w0); o.den += (y0 * w1) / (w1 + w0);
    }
  }
  const perM = (y, w) => (w === 0 ? '—' : (y / (w / 1e6)).toFixed(3));
  return [o.strata, o.work1, +o.y1.toFixed(2), perM(o.y1, o.work1), o.work0, +o.y0.toFixed(2), perM(o.y0, o.work0),
          o.den === 0 ? '—' : (o.num / o.den).toFixed(3)];
}

const fmtC = (r) => (r.c === null ? '—' : (r.c >= 0 ? '+' : '') + r.c.toFixed(3));

/* Rows grouped into at most five bins of `key`: one per distinct value when there are few, else quintiles of the
   rows' values with ties kept in one bin. Each bin states its picks, work, and the outcomes' rates per M work. */
function bins(rows, key) {
  const xs = rows.filter((r) => Number.isFinite(key(r))).sort((a, b) => key(a) - key(b));
  const distinct = [...new Set(xs.map(key))];
  const groups = [];
  if (distinct.length <= 5) for (const d of distinct) groups.push({ lo: d, hi: d, rows: xs.filter((r) => key(r) === d) });
  else {
    let i = 0;
    for (let q = 1; q <= 5 && i < xs.length; q++) {
      const end = Math.min(xs.length, Math.ceil((q * xs.length) / 5));
      let j = Math.max(end, i + 1);
      while (j < xs.length && key(xs[j]) === key(xs[j - 1])) j++;
      groups.push({ lo: key(xs[i]), hi: key(xs[j - 1]), rows: xs.slice(i, j) });
      i = j;
    }
  }
  return groups.map((g) => {
    const work = g.rows.reduce((n, r) => n + r.work, 0);
    const o = { range: g.lo === g.hi ? `${num3(g.lo)}` : `${num3(g.lo)}..${num3(g.hi)}`, picks: g.rows.length, work };
    for (const out of OUTCOMES) {
      const y = g.rows.reduce((n, r) => n + r.y[out], 0);
      o[out] = y;
      o[`${out}PerM`] = work === 0 ? '—' : (y / (work / 1e6)).toFixed(3);
    }
    return o;
  });
}
const num3 = (v) => (Math.abs(v) >= 1e4 || (v !== 0 && Math.abs(v) < 1e-3) ? v.toExponential(2) : +v.toFixed(3));

function main() {
  const o = args(process.argv.slice(2));
  need(o._.length >= 1, 'usage: node testing/bench/ledger.mjs <bench-out-dir>… [--bins a,b] [--all-sales]');
  const dir = o._.join(' ');
  /* Only the runs the bench has finished (runs.jsonl), since a running one's stream is still being written. A run
     named in two directories is refused rather than read twice. */
  const done = new Map(), files = [];
  for (const d of o._) {
    for (const r of readFileSync(join(d, 'runs.jsonl'), 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l))) {
      need(!done.has(`${r.page}.${r.run}`), `run ${r.page}.${r.run} is finished in two of the directories`);
      done.set(`${r.page}.${r.run}`, r);
      files.push(join(d, 'logs', `${r.page}.${r.run}.picks.tsv.gz`));
    }
  }
  files.sort((a, b) => a.split('/').pop().localeCompare(b.split('/').pop()));
  need(files.length > 0, `${dir} holds no finished run`);
  const pages = new Map();
  const census = [];
  for (const f of files) {
    const page = f.split('/').pop().replace(/\.picks\.tsv\.gz$/, '');
    const { inst } = readRun(f);
    const rows = [];
    let total = 0, firstSale = null, totalWork = 0;
    for (const [, rs] of inst) {
      const prev = new Map();
      for (const r of rs) {
        const x = rowOf(r, prev);
        prev.set(x.seq, x.work);
        total++; totalWork += x.work;
        if (x.endSold > 0 && (firstSale === null || x.at < firstSale)) firstSale = x.at;
        if (o['all-sales'] || x.endSold === 0) rows.push(x);
      }
    }
    pages.set(page, rows);
    const kinds = {};
    for (const r of rows) for (const [k, v] of Object.entries(r.paidBy)) kinds[k] = (kinds[k] || 0) + v;
    const run = done.get(page);
    need(typeof run.endedBy === 'string' && Number.isSafeInteger(run.stepWorkReached),
         `runs.jsonl's ${page} states endedBy and stepWorkReached`);
    census.push({ page, ended: run.endedBy, reached: run.stepWorkReached,
                  why: run.abort === null ? '-' : run.abort.replace(/^@WHY /, '').slice(0, 90).replace(/\t/g, ' '),
                  instances: inst.size, total, totalWork, rows: rows.length,
                  work: rows.reduce((n, r) => n + r.work, 0), firstSale,
                  paid: rows.reduce((n, r) => n + r.y.paid, 0), kinds,
                  landed: rows.reduce((n, r) => n + r.landed, 0), razor: rows.reduce((n, r) => n + r.razor, 0),
                  value: +rows.reduce((n, r) => n + r.y.value, 0).toFixed(2),
                  newSites: rows.reduce((n, r) => n + r.y.newSites, 0),
                  paidPicks: rows.filter((r) => r.y.paid > 0).length });
  }
  const pooled = [...pages.values()].flat();
  const out = [];
  out.push(`# pick ledger over ${dir}: ${o['all-sales'] ? 'every tenure' : 'tenures closed before the instance\'s first sale'}`);
  out.push('', '## the population each reading rests on (counts; work in stepWork units)',
           ['page', 'ended', 'reached', 'inst', 'picks', 'work', 'readPicks', 'readWork', 'firstSaleAt', 'paidPicks', 'paid', 'landed',
            'razor', 'value', 'newSites', 'paidByKind', 'abort'].join('\t'));
  for (const c of census)
    out.push([c.page, c.ended, c.reached, c.instances, c.total, c.totalWork, c.rows, c.work, c.firstSale ?? 'none', c.paidPicks, c.paid,
              c.landed, c.razor, c.value, c.newSites,
              Object.entries(c.kinds).filter(([, v]) => v > 0).map(([k, v]) => `${k}:${v}`).join(',') || '-', c.why].join('\t'));
  const sum = (k) => census.reduce((n, c) => n + c[k], 0);
  out.push(['pooled', '', '', '', sum('total'), sum('totalWork'), sum('rows'), sum('work'), '', sum('paidPicks'), sum('paid'),
            sum('landed'), sum('razor'), +sum('value').toFixed(2), sum('newSites'), ''].join('\t'));

  const pageNames = [...pages.keys()];
  for (const outc of OUTCOMES) {
    for (const how of ['value', 'rank']) {
      out.push('', `## concentration of ${outc} per unit of work, picks ordered by the term's ${how === 'value' ?
        'value' : 'rank among the sampled frontier at the pick'} (highest first); n = picks with a reading, ` +
        `yielding = those whose tenure produced any`);
      out.push(['term', ...pageNames, 'pooled', 'n', 'yielding', 'withinTime', 'wtYielding'].join('\t'));
      for (const t of TERMS) {
        if (how === 'rank' && !t.rank) continue;
        const key = how === 'value' ? (r) => r.v[t.name] : (r) => r.rank[t.name];
        const pooledC = concentration(pooled, key, outc);
        const wt = withinTime(pages, key, outc);
        out.push([t.name, ...pageNames.map((p) => fmtC(concentration(pages.get(p), key, outc))), fmtC(pooledC),
                  pooledC.n, pooledC.yielding, fmtC(wt), wt.yielding].join('\t'));
      }
      if (how === 'value')
        for (const [name, bit] of CLS) {
          const key = (r) => ((r.cls & bit) ? 1 : 0);
          const pooledC = concentration(pooled, key, outc), wt = withinTime(pages, key, outc);
          out.push([`cls:${name}`, ...pageNames.map((p) => fmtC(concentration(pages.get(p), key, outc))),
                    fmtC(pooledC), pooled.filter((r) => r.cls & bit).length,
                    pooled.filter((r) => (r.cls & bit) && r.y[outc] > 0).length, fmtC(wt), wt.yielding].join('\t'));
        }
    }
  }
  out.push('', '## the classes, pooled: picks, work and yield per M work with the bit set and clear',
           ['class', 'picks1', 'work1', 'paid/M1', 'value/M1', 'sites/M1', 'picks0', 'work0', 'paid/M0', 'value/M0',
            'sites/M0'].join('\t'));
  for (const [name, bit] of CLS) {
    const side = (set) => {
      const rs = pooled.filter((r) => ((r.cls & bit) !== 0) === set), w = rs.reduce((n, r) => n + r.work, 0);
      const per = (k) => (w === 0 ? '—' : (rs.reduce((n, r) => n + r.y[k], 0) / (w / 1e6)).toFixed(3));
      return [rs.length, w, per('paid'), per('value'), per('newSites')];
    };
    out.push([name, ...side(true), ...side(false)].join('\t'));
  }
  for (const outc of OUTCOMES) {
    out.push('', `## groups inside time, ${outc}: the group's rate against the rest's, Mantel-Haenszel over five ` +
             'equal-work strata per page (only strata where both sides ran; 1 = no difference)',
             ['group', 'strata', 'work1', `${outc}1`, '/M1', 'work0', `${outc}0`, '/M0', 'rateRatioMH'].join('\t'));
    for (const [name, bit] of CLS) out.push([`cls:${name}`, ...groupWithinTime(pages, (r) => (r.cls & bit) !== 0, outc)].join('\t'));
    out.push(['cls:forced|drive|candidate', ...groupWithinTime(pages, (r) => (r.cls & 35) !== 0, outc)].join('\t'));
    for (const k of [0, 1, 2]) out.push([`priced=${k}`, ...groupWithinTime(pages, (r) => r.v.priced === k, outc)].join('\t'));
    out.push(['priced>=3', ...groupWithinTime(pages, (r) => r.v.priced >= 3, outc)].join('\t'));
  }
  const want = o.bins ? o.bins.split(',') : ['at', 'x', 'served', 'famNotch', 'turn', 'priced'];
  for (const name of want) {
    need(TERMS.some((t) => t.name === name), `--bins names no term ${name}`);
    for (const how of ['value', 'rank']) {
      const t = TERMS.find((u) => u.name === name);
      if (how === 'rank' && !t.rank) continue;
      out.push('', `## ${name} by ${how}, pooled`, ['bin', 'picks', 'work', ...OUTCOMES.flatMap((k) => [k, `${k}/M`])].join('\t'));
      for (const b of bins(pooled, how === 'value' ? (r) => r.v[name] : (r) => r.rank[name]))
        out.push([b.range, b.picks, b.work, ...OUTCOMES.flatMap((k) => [b[k], b[`${k}PerM`]])].join('\t'));
    }
  }
  console.log(out.join('\n'));
}

main();
