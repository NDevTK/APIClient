// The egress default and the fired-send record (extension/lib/safe-fetch.js) as engine/trusted.mjs's zone runs
// them: every row is a safeFetch, or a firing question, through that zone against a loopback server that counts
// what reached it. A row reads the reply record's `egress` list (one entry per send the request asked for: the
// signal vector the zone judged, the arm that admitted it, and whether the wire was reached) beside the server's
// own count. The control runs first: an unforced page request reaches the server, so a count of zero in a later
// row is the policy and not a server that counts nothing.
//
//   node testing/egress_rows.mjs
"use strict";
import { createServer } from 'node:http';
import { trustedZone } from '../engine/trusted.mjs';
import { siteRegistrableDomains } from '../engine/psl.mjs';
import { NAVIGABLE_NOTICE } from '../engine/noticegen.mjs';

const hits = new Map();
const count = (method, path) => hits.get(method + ' ' + path) || 0;
const server = createServer((req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  const key = req.method + ' ' + path;
  hits.set(key, (hits.get(key) || 0) + 1);
  if (path === '/hop') {
    res.writeHead(302, { Location: '/landed', 'Cache-Control': 'no-store' });
    res.end();
    return;
  }
  res.writeHead(200, { 'Content-Type': path.endsWith('.js') ? 'text/javascript' : 'text/plain',
                       'Access-Control-Allow-Origin': '*', 'Cache-Control': 'no-store' });
  res.end(`${key} #${hits.get(key)}`);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const PORT = server.address().port;
/* Two origins on one server: A stays as every origin starts, W is widened by the person. */
const A = `http://127.0.0.1:${PORT}`;
const W = `http://localhost:${PORT}`;

const ZONE = trustedZone();
ZONE.httpCacheStated(null, null);
ZONE.safeFetchEgressStated({});
ZONE.safeFetchWiden(W);
ZONE.swRpc = async (api, arg) => {
  if (api === 'site.registrableDomains') return siteRegistrableDomains(arg);
  throw new Error('this test serves no rpc ' + JSON.stringify(api));
};

const docOf = (page, reach, driven = 'undriven', pinned = 'unpinned') =>
  ZONE.docRecord(reach, driven, pinned, NAVIGABLE_NOTICE.sandbox.none, ZONE.safeFetchDocumentAddressSpace(page, null));

/* A page request as engine/trusted.mjs's workFetch states it, from the document `docKey` at `page`. */
async function ask(page, url, docKey, { destination = '', mode = 'cors', provenance = 'derived', pinned = 'unpinned',
                                        driven = 'undriven', reach = 'observed' } = {}) {
  const origin = new URL(page).origin;
  return ZONE.safeFetch(url, {
    pageOrigin: origin, destination, mode, provenance, pinned, driven,
    actor: 'page', credentialed: false, credentials: 'same-origin', useCorsPreflight: false,
    doc: docOf(page, reach),
    identity: await ZONE.pageRequestIdentity(origin, page, docKey, url, 'same-origin', mode,
                                             'strict-origin-when-cross-origin', page) });
}
const SIGNAL_NAMES = ZONE.safeFetchSignals().map((s) => s.name).sort().join(',');
/* The one entry of a reply record's `egress` list, or a word saying why there is not exactly one. */
const only = (r) => !Array.isArray(r.egress) ? 'no-egress-list' : r.egress.length !== 1 ? `entries=${r.egress.length}` : r.egress[0];
const names = (e) => e && e.vector ? Object.keys(e.vector).sort().join(',') : 'no-vector';
const hows = (r) => Array.isArray(r.egress) ? r.egress.map((e) => e.how).join(',') : 'no-egress-list';
const arm = (e) => !e || typeof e !== 'object' ? 'no-entry' : e.admittedBy === null ? 'none' : e.admittedBy.kind;

const PAGE_A = `${A}/page`, PAGE_W = `${W}/page`;
const ROWS = [
  { label: 'CONTROL: an unforced page request is sent, and its send records the vector and the admitting arm',
    run: async () => {
      const r = await ask(PAGE_A, `${A}/own`, 'engine:own'), e = only(r);
      return [['GET /own', count('GET', '/own'), 1], ['how', e.how, 'sent'], ['admitted by', arm(e), 'default'],
              ['the vector names every signal', names(e), SIGNAL_NAMES],
              ['an undetermined signal is written', e.vector && e.vector.lineage, 'unknown'],
              ['the arm states why', typeof (e.admittedBy && e.admittedBy.why), 'string']]; } },
  { label: 'a refused request records the vector it was judged on, and that it was not sent', run: async () => {
      const r = await ask(PAGE_A, `${A}/pin`, 'engine:pin', { provenance: 'forced', pinned: 'pinned' }), e = only(r);
      return [['GET /pin', count('GET', '/pin'), 0], ['grade', r.refusal && r.refusal.kind, 'decline'],
              ['how', e.how, 'not-sent'], ['admitted by', arm(e), 'none'],
              ['witness', e.vector && e.vector.witness, 'pinned'],
              ['the refusal rides the entry', typeof e.refusal, 'string']]; } },
  { label: 'a request a widened origin admits says so', run: async () => {
      const r = await ask(PAGE_W, `${W}/wide`, 'engine:wide', { provenance: 'forced', pinned: 'pinned' }), e = only(r);
      return [['GET /wide', count('GET', '/wide'), 1], ['how', e.how, 'sent'], ['admitted by', arm(e), 'widened'],
              ['the widened origin', e.admittedBy && e.admittedBy.origin, W]]; } },
  { label: 'a byte-identical ask of one document is answered from its record and reaches no other party',
    run: async () => {
      const one = await ask(PAGE_A, `${A}/twice`, 'engine:twice'), two = await ask(PAGE_A, `${A}/twice`, 'engine:twice');
      return [['GET /twice', count('GET', '/twice'), 1], ['first', only(one).how, 'sent'],
              ['second', only(two).how, 'answered-locally']]; } },
  { label: 'each hop of a redirect is its own send with its own entry', run: async () => {
      const r = await ask(PAGE_A, `${A}/hop`, 'engine:hop');
      return [['GET /hop', count('GET', '/hop'), 1], ['GET /landed', count('GET', '/landed'), 1],
              ['entries', hows(r), 'sent,sent'],
              ['the second entry is the hop\'s address', Array.isArray(r.egress) && r.egress.length === 2 && r.egress[1].url,
               `${A}/landed`]]; } },
  { label: 'a request no gate let as far as the firing question records no send', run: async () => {
      const r = await ask(PAGE_A, 'ftp://127.0.0.1/x', 'engine:scheme');
      return [['grade', r.refusal && r.refusal.kind, 'network'], ['entries', hows(r), '']]; } },
  { label: 'the zone counts its sends by how each left and what admitted it', run: async () => {
      const c = typeof ZONE.safeFetchEgressCensus === 'function' ? ZONE.safeFetchEgressCensus() : null;
      const sum = c === null ? -1 : Object.values(c).reduce((a, n) => a + n, 0);
      /* own, wide, twice, hop and landed were sent; pin was not; twice's second ask was answered locally. */
      return [['entries counted', sum, 7], ['sent by a default arm', c && c['sent default'], 4],
              ['sent at a widened origin', c && c['sent widened'], 1], ['not sent', c && c['not-sent none'], 1],
              ['answered locally', c && c['answered-locally default'], 1]]; } },
];

let bad = 0;
try {
  for (const row of ROWS) {
    const notes = (await row.run()).map(([what, have, want]) => ({ what, have, want }));
    const ok = notes.every((n) => n.have === n.want);
    if (!ok) bad++;
    console.log(`  ${ok ? 'ok  ' : 'BAD '} ${row.label}\n       ` +
                notes.map((n) => `${n.what}: ${n.have} (want ${n.want})`).join('; '));
  }
} finally {
  server.close();
}
/* The control's send reached the server, or nothing above is a reading. */
if (count('GET', '/own') < 1) { console.log('  CONTROL FAILED: the unforced page request never reached the server'); process.exit(2); }
console.log(bad === 0 ? '  every row agreed' : `  ${bad} ROW(S) DISAGREED`);
process.exit(bad === 0 ? 0 : 1);
