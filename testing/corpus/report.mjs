// THE CENSUS TABLE. One row per site: outcome, the abort's file:line, endpoints learned, flows/switches,
// whether the analysis finished. Then the distinct crash signatures ranked by how many sites hit each.
//
// OUTCOME IS THREE-WAY AND THE THREE ARE NOT MERGED (an earlier pass conflated them and it cost real time):
//   ENGINE-ABORT   an @WHY/@E from the engine or the trusted zone — a DCHECK/CHECK naming an unbuilt
//                  capability or a violated invariant. The renderer dies; the run has no result document.
//   PAGE-THROW     the page's own uncaught throw on a capability this engine honestly lacks. It arrives as
//                  a `pageErrors` row that is NOT an engine assert, or as a @LOG the page itself emitted.
//   FIXTURE/NET    the site did not deliver a document (HTTP 5xx, proxy refusal, navigation failure). Not a
//                  statement about the engine at all.
//   RAN            no abort: the engine ran flows. `finished` then says whether the analysis RETURNED
//                  within the dwell (a result document stored on the doc) or was still exploring.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = new URL('.', import.meta.url).pathname;
/* SEVERAL PASSES, BECAUSE ONE RUN IS NOT A MEASUREMENT. Pass every census file and the table reports each
   site's SPREAD across them; pass one and the spread is a single value, which is honest about what it is.
   The pass label is taken from the filename (census-p1.jsonl -> p1) purely to name the log a signature came
   from -- the signature itself is read from the ROW, which carries its own `why`/`atE`, so an aggregate can
   never attribute one pass's abort to another pass's log. */
const files = process.argv.slice(2);
if (!files.length) files.push('census.jsonl');
const passes = files.map((f) => ({
  label: (f.match(/census-(.+)\.jsonl$/) || [, f])[1],
  rows: readFileSync(join(ROOT, f), 'utf8').trim().split('\n').filter(Boolean).map((l) => JSON.parse(l)),
}));
const stacks = new Map(readFileSync(join(ROOT, 'sites.tsv'), 'utf8').trim().split('\n')
  .map((l) => l.split('\t')).map(([id, u, s]) => [id, s]));

const unesc = (s) => s.replace(/\\\\/g, '\\').replace(/\\"/g, '"').replace(/\\n/g, '\n');

/* A SIGNATURE NAMES A DEFECT, SO A GENSYM IN IT MUST NOT SPLIT ONE INTO SEVERAL. The orphan-drive DFAIL
   quotes the operand's identity -- `{orphan3a153db70eec69f2.arg0}` on one site and `{orphanc59c489f051691b8
   .arg0}` on another -- which is the SAME unbuilt capability at the same file:line, and it ranked as two
   one-site entries instead of one two-site entry. Ranking is what this section is for, so the per-run name is
   folded out: a run of 8+ hex digits is an address or a hash, never part of what to build. */
const degensym = (s) => s.replace(/[0-9a-f]{12,}/gi, '<id>').replace(/\b0x[0-9a-f]{4,}\b/gi, '<addr>');
function signatures(text) {
  const t = unesc(unesc(text));
  const out = new Set();
  for (const m of t.matchAll(/"cond":"((?:[^"\\]|\\.)*)","at":"([^"]+)"/g))
    out.add(m[2].replace('/home/user/APIClient/', '') + ' :: ' + degensym(m[1]));
  for (const m of t.matchAll(/@WHY ([^{][^\n]*?) \((\/home\/user\/APIClient\/[^):]+:\d+)\)/g))
    out.add(m[2].replace('/home/user/APIClient/', '') + ' :: ' + degensym(m[1].trim()));
  for (const m of t.matchAll(/@WHY DCHECK failed: ([^\n"]{10,200})/g))
    out.add('extension/check.js (js side) :: ' + degensym(m[1].trim()).slice(0, 120));
  return [...out];
}

/* ONE MEASUREMENT PER (site, pass). The signature comes from the ROW; the log is a supplement for a crash
   that happened before any result document existed and so had nowhere else to go. It is read from the
   PASS-QUALIFIED name when the runner wrote one, because logs/<id>.log is overwritten by every later pass and
   reading it for an earlier pass's row attributes one run's abort to another's numbers. */
const bySig = new Map();
const seen = new Map();      // id -> [per-pass measurement]
for (const p of passes) for (const r of p.rows) {
  let log = '';
  for (const n of [p.label + '-' + r.id + '.log', r.id + '.log']) {
    try { log = readFileSync(join(ROOT, 'logs', n), 'utf8'); break; } catch { }
  }
  const blob = log + '\n' + JSON.stringify(r.pageErrors || []) + '\n' + JSON.stringify(r.why || []) + '\n' + JSON.stringify(r.atE || []);
  const sigs = signatures(blob);
  const netBad = r.fatal || r.nav !== 'ok' || (r.status !== 200 && r.status !== 304);
  const outcome = netBad ? 'NET/FIXTURE'
    : sigs.length ? 'ENGINE-ABORT'
      : r.crashedMine ? 'ABORT(unclassified)'
        : 'RAN';
  if (!seen.has(r.id)) seen.set(r.id, []);
  seen.get(r.id).push({
    pass: p.label, outcome, finished: r.docsAnswered > 0 && !r.crashedMine,
    runs: r.runsMine, crashed: r.crashedMine,
    /* THE HEADLINE IS `distinctEndpoints` -- distinct ADDRESSES learned, which is what "endpoints" has to
       mean in a report. `r.endpoints` is the last log entry's CUMULATIVE counter and is kept beside it under
       a name that says so, because it is still the right thing to watch a live run advance by. This column
       read the counter, so a page that learned one address could print a two-digit `ep`. */
    endpoints: r.distinctEndpoints, counter: r.endpoints,
    sinks: r.sinks, flows: r.flows, switches: r.switches,
    sigs, wasm: (r.artifact && r.artifact.wasmSha256 || '').slice(0, 12),
    /* THE ARTIFACT IS NAMED BY ITS HASH ALONE. This read `r.artifact.head`, a field site.mjs deliberately
       renamed to `builtFromHeadClaim` when it stopped being trustworthy, so it resolved to '' for every row
       and the artifact line printed a bare `@` -- a reader-with-no-writer, defaulted to empty by `|| ''`. */
    load: Array.isArray(r.loadAfter) ? r.loadAfter[0] : null,
  });
  for (const s of sigs) { if (!bySig.has(s)) bySig.set(s, new Set()); bySig.get(s).add(r.id); }
}

/* A SPREAD, NOT AN AVERAGE. min-max over the passes that ANSWERED; a pass that produced no number is counted
   in `n/N` and never folded into the range, because absent and zero are different facts. */
const spread = (ms, k) => {
  const v = ms.map((m) => m[k]).filter((x) => typeof x === 'number');
  if (!v.length) return '-';
  const lo = Math.min(...v), hi = Math.max(...v);
  return lo === hi ? String(lo) : lo + '-' + hi;
};
const table = [...seen.entries()].map(([id, ms]) => ({
  id, stack: stacks.get(id) || '', measurements: ms,
  /* THE OUTCOME IS THE WORST SEEN, and how often, so a site that aborts in two passes of three cannot be
     reported as one that runs. */
  outcome: ms.some((m) => m.outcome === 'NET/FIXTURE') ? 'NET/FIXTURE'
    : ms.some((m) => m.outcome.includes('ABORT')) ? 'ENGINE-ABORT' : 'RAN',
  abortedPasses: ms.filter((m) => m.outcome.includes('ABORT')).length,
  finishedPasses: ms.filter((m) => m.finished).length,
  n: ms.length,
  ep: spread(ms, 'endpoints'), fl: spread(ms, 'flows'), sw: spread(ms, 'switches'),
  sk: spread(ms, 'sinks'), rn: spread(ms, 'runs'), ld: spread(ms, 'load'),
  epMax: Math.max(-1, ...ms.map((m) => m.endpoints).filter((x) => typeof x === 'number')),
  epAnswered: ms.filter((m) => typeof m.endpoints === 'number').length,
  sigs: [...new Set(ms.flatMap((m) => m.sigs))],
  wasm: [...new Set(ms.map((m) => m.wasm))],
}));

/* ABSENT PRINTS AS `-`, NEVER AS 0. A run that produced no result document is not a page that was analysed
   and found clean, and `String(null)` would have printed "null" into a numeric column. */
const pad = (s, n) => String(s).padEnd(n).slice(0, n);
console.log(`${passes.length} pass(es): ${passes.map((p) => p.label + '(' + p.rows.length + ')').join(' ')}`);
console.log('\n' + pad('site', 20) + pad('outcome', 13) + pad('abort/n', 8) + pad('fin/n', 7) +
  pad('ep', 8) + pad('sinks', 7) + pad('flows', 12) + pad('switches', 12) + pad('load', 10) + 'signature');
for (const t of table)
  console.log(pad(t.id, 20) + pad(t.outcome, 13) + pad(t.abortedPasses + '/' + t.n, 8) +
    pad(t.finishedPasses + '/' + t.n, 7) + pad(t.ep, 8) + pad(t.sk, 7) + pad(t.fl, 12) + pad(t.sw, 12) +
    pad(t.ld, 10) + (t.sigs[0] ? t.sigs[0].split(' :: ')[0] : '-'));

/* THE WORK QUEUE. A DFAIL's reason names what to build, so it is printed rather than summarised -- but only
   the head of it, because one 1169-character reason per row buries the RANKING, which is the thing this
   section exists to show. `-v` prints them whole. */
const verbose = process.env.SIGS === 'full';
console.log('\n=== distinct crash signatures, ranked by sites hit ===');
for (const [s, ids] of [...bySig.entries()].sort((a, b) => b[1].size - a[1].size)) {
  const [at, ...rest] = s.split(' :: ');
  const why = rest.join(' :: ');
  console.log(`${ids.size}  ${at}\n     ${verbose || why.length <= 300 ? why : why.slice(0, 300) + ' …(SIGS=full for the rest)'}\n     sites: ${[...ids].join(', ')}`);
}

/* ONE ARTIFACT PER CENSUS OR THE CENSUS IS NOT ONE MEASUREMENT. More than one hash here means rows either
   side of a rebuild are two programs wearing one corpus, and the totals below are meaningless. */
const artifacts = new Set(table.flatMap((t) => t.wasm).filter(Boolean));
console.log('\nartifacts measured: ' + [...artifacts].join('  ') + (artifacts.size > 1 ? '   *** MIXED — NOT ONE MEASUREMENT ***' : ''));
const loads = passes.flatMap((p) => p.rows).map((r) => Array.isArray(r.loadAfter) ? r.loadAfter[0] : null).filter((x) => typeof x === 'number');
if (loads.length) console.log(`load average across rows: min ${Math.min(...loads).toFixed(2)} max ${Math.max(...loads).toFixed(2)} mean ${(loads.reduce((a, b) => a + b, 0) / loads.length).toFixed(2)} on ${passes.flatMap((p) => p.rows)[0].cores} cores`);

/* ABSENT AND ZERO ARE DIFFERENT FACTS AND ARE NEVER SUMMED TOGETHER. `reduce((n,t)=>n+t.endpoints,0)` made
   `null` into 0 (JS coerces it) and `undefined` into NaN, which JSON.stringify then printed as `null` -- so
   the total was either a number that had silently counted un-analysed sites as clean-and-empty, or the word
   "null" for the whole corpus. Report the count of sites that ANSWERED beside the sum over those sites.
   The per-site figure is that site's BEST pass, because the question a headline answers is what the tool
   CAN learn from the page; the spread column beside it is what says how reliably. */
const measurable = table.filter((t) => t.outcome !== 'NET/FIXTURE');
const withEp = table.filter((t) => t.epAnswered > 0);
console.log('totals: ' + JSON.stringify({
  sites: table.length,
  netFixture: table.filter((t) => t.outcome === 'NET/FIXTURE').length,
  measurable: measurable.length,
  everAborted: measurable.filter((t) => t.abortedPasses > 0).length,
  abortedEveryPass: measurable.filter((t) => t.abortedPasses === t.n).length,
  cleanEveryPass: measurable.filter((t) => t.abortedPasses === 0).length,
  finishedEveryPass: measurable.filter((t) => t.finishedPasses === t.n).length,
  sitesReportingAnEndpointCount: withEp.length,
  sitesLearningAtLeastOneEndpoint: withEp.filter((t) => t.epMax > 0).length,
  endpointsTotalBestPass: withEp.reduce((n, t) => n + Math.max(0, t.epMax), 0),
}));
