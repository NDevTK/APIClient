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
const file = process.argv[2] || 'census.jsonl';
const rows = readFileSync(join(ROOT, file), 'utf8').trim().split('\n').map((l) => JSON.parse(l));
const stacks = new Map(readFileSync(join(ROOT, 'sites.tsv'), 'utf8').trim().split('\n')
  .map((l) => l.split('\t')).map(([id, u, s]) => [id, s]));
const heads = new Map(readFileSync(join(ROOT, 'headers.tsv'), 'utf8').trim().split('\n')
  .map((l) => l.split('\t')).map((c) => [c[0], c.slice(1).join(' ')]));

const unesc = (s) => s.replace(/\\\\/g, '\\').replace(/\\"/g, '"').replace(/\\n/g, '\n');
function signatures(text) {
  const t = unesc(unesc(text));
  const out = new Set();
  for (const m of t.matchAll(/"cond":"((?:[^"\\]|\\.)*)","at":"([^"]+)"/g))
    out.add(m[2].replace('/home/user/APIClient/', '') + ' :: ' + m[1]);
  for (const m of t.matchAll(/@WHY ([^{][^\n]*?) \((\/home\/user\/APIClient\/[^):]+:\d+)\)/g))
    out.add(m[2].replace('/home/user/APIClient/', '') + ' :: ' + m[1].trim());
  for (const m of t.matchAll(/@WHY DCHECK failed: ([^\n"]{10,200})/g))
    out.add('extension/check.js (js side) :: ' + m[1].trim().slice(0, 120));
  return [...out];
}

const bySig = new Map();
const table = [];
for (const r of rows) {
  let log = '';
  try { log = readFileSync(join(ROOT, 'logs', r.id + '.log'), 'utf8'); } catch { }
  const blob = log + '\n' + JSON.stringify(r.pageErrors || []) + '\n' + JSON.stringify(r.why || []) + '\n' + JSON.stringify(r.atE || []);
  const sigs = signatures(blob);
  const netBad = r.fatal || r.nav !== 'ok' || (r.status !== 200 && r.status !== 304);
  const outcome = netBad ? 'NET/FIXTURE'
    : sigs.length ? 'ENGINE-ABORT'
      : r.crashedMine ? 'ABORT(unclassified)'
        : 'RAN';
  const finished = r.docsAnswered > 0 && !r.crashedMine;
  table.push({
    id: r.id, stack: stacks.get(r.id) || '', headers: heads.get(r.id) || '',
    status: r.status, outcome, finished,
    runs: r.runsMine, crashed: r.crashedMine,
    endpoints: r.endpoints, sinks: r.sinks, flows: r.flows, switches: r.switches,
    jobsRun: r.jobsRun, origins: (r.docsAllOrigins || []).length,
    sigs, wasm: (r.artifact && r.artifact.wasmSha256 || '').slice(0, 12),
    head: (r.artifact && r.artifact.head || '').slice(0, 8),
  });
  for (const s of sigs) { if (!bySig.has(s)) bySig.set(s, new Set()); bySig.get(s).add(r.id); }
}

const pad = (s, n) => String(s).padEnd(n).slice(0, n);
console.log(pad('site', 20) + pad('outcome', 14) + pad('fin', 4) + pad('runs', 5) + pad('ep', 4) + pad('sink', 5) + pad('flows', 7) + pad('sw', 6) + pad('orig', 5) + 'abort');
for (const t of table)
  console.log(pad(t.id, 20) + pad(t.outcome, 14) + pad(t.finished ? 'yes' : 'no', 4) + pad(t.runs, 5) +
    pad(t.endpoints, 4) + pad(t.sinks, 5) + pad(t.flows, 7) + pad(t.switches, 6) + pad(t.origins, 5) +
    (t.sigs[0] ? t.sigs[0].split(' :: ')[0] : '-'));

console.log('\n=== distinct crash signatures, ranked by sites hit ===');
for (const [s, ids] of [...bySig.entries()].sort((a, b) => b[1].size - a[1].size))
  console.log(`${ids.size}  ${s}\n     sites: ${[...ids].join(', ')}`);

const artifacts = new Set(table.map((t) => t.wasm + '@' + t.head));
console.log('\nartifacts measured: ' + [...artifacts].join('  '));
console.log('totals: ' + JSON.stringify({
  sites: table.length,
  engineAborts: table.filter((t) => t.outcome.startsWith('ENGINE-ABORT') || t.outcome.startsWith('ABORT')).length,
  ran: table.filter((t) => t.outcome === 'RAN').length,
  netFixture: table.filter((t) => t.outcome === 'NET/FIXTURE').length,
  finished: table.filter((t) => t.finished).length,
  endpointsTotal: table.reduce((n, t) => n + t.endpoints, 0),
  sinksTotal: table.reduce((n, t) => n + t.sinks, 0),
}));
