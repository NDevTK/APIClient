/* The real-site request diff: what Chromium requests on a logged-out load against what an engine run sent.
 *   node testing/realsite/diff.mjs <captures>/<id> <wire.jsonl> <results.json[.gz]> [--json <out>]
 * Inputs: the chrome-<n>.json loads chrome_requests.cjs wrote, the wire log testing/bench/wire.mjs wrote for one
 * engine run (one line per send the trusted zone let out), and that run's result documents (one per instance).
 * A load counts only when its navigation answered 2xx/3xx and its page did not crash; the count is printed.
 * Chromium's STABLE set is the identities every counted load requested, in the first counted load's order;
 * the VARYING set is those some loads requested. The engine's sends are keyed the same way (identity.cjs).
 * Printed: stable requests the engine never sent, in Chromium's order, each with what the result documents
 * hold for that address (a record's provenance and door, or nothing); engine sends no Chromium load made,
 * grouped by origin and record; and
 * each instance's page errors and absent-global reads, which is where a first cause is looked for. */

import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { join } from 'node:path';
import { createRequire } from 'node:module';
const { describeUrl } = createRequire(import.meta.url)('./identity.cjs');

const argv = process.argv.slice(2);
const jsonAt = argv.indexOf('--json');
const jsonOut = jsonAt === -1 ? null : argv.splice(jsonAt, 2)[1];
const [capDir, wireFile, resultsFile] = argv;
if (!capDir || !wireFile || !resultsFile) {
  console.error('usage: node testing/realsite/diff.mjs <captures>/<id> <wire.jsonl> <results.json[.gz]> [--json <out>]');
  process.exit(2);
}

const readJson = (f) => JSON.parse(f.endsWith('.gz') ? gunzipSync(readFileSync(f)).toString('utf8') : readFileSync(f, 'utf8'));
const key = (method, identity) => method + ' ' + identity;
const pathKey = (method, origin, path) => method + ' ' + origin + path;

/* ── Chromium ── */
const loads = readdirSync(capDir).filter((f) => /^chrome-\d+\.json$/.test(f)).sort().map((f) => readJson(join(capDir, f)));
if (loads.length === 0) throw new Error(`${capDir} holds no chrome-<n>.json`);
/* A capture made before `crashed` was recorded shows a dead renderer only as `document.evalThrew`. */
const whole = (l) => l.crashed === undefined ? !(l.document && l.document.evalThrew) : !l.crashed;
const counted = loads.filter((l) => whole(l) && l.nav && typeof l.nav.status === 'number' && l.nav.status >= 200 && l.nav.status < 400);
const refused = loads.filter((l) => !counted.includes(l))
  .map((l) => ({ load: l.load, nav: l.nav, ...(whole(l) ? {} : { renderer: 'crashed' }) }));
if (counted.length === 0) throw new Error(`no load of ${capDir} answered 2xx/3xx: ${JSON.stringify(refused)}`);
const perLoad = counted.map((l) => {
  const m = new Map();
  for (const r of l.requests) {
    if (r.origin === null) continue;                 /* data:, blob: and the like reach no server */
    const k = key(r.method, r.identity);
    if (!m.has(k)) m.set(k, r);
  }
  return m;
});
const everyKey = new Set(perLoad.flatMap((m) => [...m.keys()]));
const stable = [...perLoad[0].keys()].filter((k) => perLoad.every((m) => m.has(k)));
const varying = [...everyKey].filter((k) => !perLoad.every((m) => m.has(k)));
const chromeRow = (k) => perLoad.find((m) => m.has(k)).get(k);

/* ── Engine ── */
const wire = readFileSync(wireFile, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
const sent = new Map();                              /* key -> {order, n, status|error} */
const sentByPath = new Map();
wire.forEach((w, i) => {
  const d = describeUrl(w.url);
  const k = key(w.method, d.identity);
  if (!sent.has(k)) sent.set(k, { order: i, n: 0, answer: w.status !== undefined ? w.status : 'error: ' + w.error, url: d });
  sent.get(k).n++;
  const pk = pathKey(w.method, d.origin, d.path);
  if (!sentByPath.has(pk)) sentByPath.set(pk, k);
});
const docs = readJson(resultsFile);
/* A result record's URL carries no query (its params do), so records are looked up by method, origin and path. */
const records = new Map();
for (const d of docs) {
  for (const r of d.fetchCallSites) {
    const u = describeUrl(r.url);
    const pk = pathKey(r.method, u.origin, u.path);
    if (!records.has(pk)) records.set(pk, []);
    const assumed = (r.witness || []).filter((w) => w !== null)
      .map((w) => w.assumed.filter((a) => a.source !== undefined).map((a) => a.source));
    records.get(pk).push({ scope: d._novelty.scope, provenance: r.provenance, door: r.door, driven: r.driven,
                           addressClass: r.addressClass, witnessClass: r.witnessClass,
                           /* The fewest assumed sources any witness of this record rests on; null where none was asked. */
                           fewestAssumed: assumed.length ? Math.min(...assumed.map((a) => a.length)) : null });
  }
}
const recordsOf = (row) => records.get(pathKey(row.method, row.origin, row.path)) || [];
const recText = (rs) => rs.length === 0 ? 'no record'
  : [...new Set(rs.map((r) => `${r.provenance}/${r.door}${r.driven ? '/driven' : ''}` +
                              (r.fewestAssumed === null ? '' : `/assumes>=${r.fewestAssumed}`)))].join(', ');

const missing = stable.filter((k) => !sent.has(k)).map((k) => {
  const r = chromeRow(k);
  /* The engine may have sent the same path under other query keys; say so rather than call it missing outright. */
  const near = sentByPath.get(pathKey(r.method, r.origin, r.path)) || null;
  return { key: k, order: r.order, atMs: r.atMs, resourceType: r.resourceType, serviceWorker: r.serviceWorker,
           mainFrame: r.mainFrame, sameOrigin: r.sameOriginToDocument, initiator: r.initiator, status: r.status,
           sentUnderOtherQuery: near, records: recordsOf(r) };
});
const extra = [...sent.entries()].filter(([k]) => !everyKey.has(k)).map(([k, s]) => ({
  key: k, order: s.order, sends: s.n, answer: s.answer,
  records: records.get(pathKey(k.split(' ')[0], s.url.origin, s.url.path)) || [] }));

const out = [];
const P = (s = '') => out.push(s);
P(`# ${capDir}`);
P(`Chromium ${counted[0].chromium}: ${counted.length} of ${loads.length} loads counted` +
  (refused.length ? `; not counted: ${JSON.stringify(refused)}` : '') +
  `; windows ended by ${counted.map((l) => l.endedBy).join(',')}; load1 ${counted.map((l) => l.load1.toFixed(1)).join(',')}`);
P(`Chromium request identities: ${stable.length} in every counted load, ${varying.length} in some, ` +
  `${everyKey.size} in any; requests per load ${counted.map((l) => l.requests.length).join(',')}`);
P(`engine: ${wire.length} sends, ${sent.size} identities (${wireFile}); ${docs.length} instance(s): ` +
  docs.map((d) => `${d._novelty.scope} stepWork ${d._cold.stepWork}`).join('; '));
const stableSent = stable.filter((k) => sent.has(k));
P(`of the ${stable.length} stable identities the engine sent ${stableSent.length} and never sent ${missing.length}; ` +
  `the engine sent ${extra.length} identities no Chromium load made`);
P();
P('## stable Chromium requests the engine never sent, in Chromium\'s order');
P('order\tatMs\ttype\twho\torigin\trequest\tinitiator\tengine record');
for (const m of missing) {
  const who = m.serviceWorker ? 'service-worker' : m.mainFrame === false ? 'subframe' : m.mainFrame === null ? 'worker' : 'main';
  const ini = m.initiator ? `${m.initiator.type} ${m.initiator.at || ''}` : '-';
  P([m.order, m.atMs, m.resourceType, who, m.sameOrigin ? 'same' : 'cross', m.key, ini,
     recText(m.records) + (m.sentUnderOtherQuery ? ` (sent as ${m.sentUnderOtherQuery})` : '')].join('\t'));
}
P();
P('## requests in only some Chromium loads');
for (const k of varying) {
  const r = chromeRow(k);
  P(`${perLoad.filter((m) => m.has(k)).length}/${counted.length}\t${r.resourceType}\t${k}\t${sent.has(k) ? 'engine sent' : 'engine did not send'}`);
}
P();
P('## engine sends no Chromium load made, in the engine\'s order');
P('order\tsends\tanswer\trequest\tengine record');
/* One line per (origin, record) group, naming its first send: a forced document's subresources are one fact. */
const groups = new Map();
for (const e of extra) {
  const origin = e.key.split(' ')[1].split('/').slice(0, 3).join('/');
  const g = origin + '\t' + recText(e.records);
  if (!groups.has(g)) groups.set(g, { first: e, n: 0 });
  groups.get(g).n++;
}
for (const [g, v] of groups)
  P([v.first.order, v.first.sends, v.first.answer, v.first.key + (v.n > 1 ? `  (+${v.n - 1} more of this origin and record)` : ''),
     g.split('\t')[1]].join('\t'));
P();
P('## each instance: what its result document says of the page\'s own run');
for (const d of docs) {
  const c = d._cold, w = d._wfq;
  const units = Object.entries(c.stepUnitRuns).filter(([k, v]) => v && /timer|rendering|idle/.test(k)).map(([k, v]) => `${k} ${v}`);
  P(`${d._novelty.scope}: stepWork ${c.stepWork}, flows made ${d._flows}, members ${w.members} ` +
    `(observed ${w.observedMembers}, on an assumption ${w.assumedMembers}), root programs ${c.rootPrograms}, ` +
    `deepest completed ${c.completed}, cursors ${JSON.stringify(Object.fromEntries(Object.entries(c.programCursors).filter(([, v]) => v)))}`);
  P(`  replies asked ${c.replyAsked} answered ${c.replyAnswered} declined ${c.replyDeclined} dropped ${c.replyDropped} outstanding ${c.replyOutstanding}; ` +
    `fetch() called ${c.epFetchAskCalledLife}, XHR send() called ${c.epXhrAskCalledLife}; ` +
    `clock advance asked ${c.clockAdvanceAskedLife} declined ${c.clockAdvanceDeclinedLife}; ${units.join(', ') || 'no timer, rendering or idle unit ran'}`);
  P(`  page errors ${d.pageErrors.length}` + (d.pageErrors.length ? ':' : ''));
  for (const e of d.pageErrors.slice(0, 12)) P('    ' + e.slice(0, 300));
  const absent = Object.entries(d._absent).filter(([k, v]) => !k.startsWith('_') && typeof v === 'object')
    .map(([k, v]) => `${k} (read ${v.read}, typeof ${v.typeof}, in ${v.in})`);
  P(`  globals a standard owns that this realm lacks, as the page asked for them: ${absent.join('; ') || 'none'}`);
}
P();
P('## Chromium console errors and uncaught exceptions (first counted load)');
for (const e of counted[0].errors.slice(0, 20)) P(`${e.atMs}ms ${e.kind}: ${e.text.split('\n')[0].slice(0, 300)}`);
if (counted[0].errors.length === 0) P('none');
console.log(out.join('\n'));
if (jsonOut) writeFileSync(jsonOut, JSON.stringify({ capDir, wireFile, resultsFile, loads: loads.length, counted: counted.length,
  refused, stable, varying, missing, extra }, null, 1));
