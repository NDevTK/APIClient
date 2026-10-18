// The HTTP cache (extension/lib/http-cache.js) as engine/trusted.mjs's zone runs it: every row is a
// safeFetch, or an operator send, through that zone against a loopback server that counts what reached it.
// A row passes when the server's count is what Fetch §4.6 "HTTP-network-or-cache fetch" over RFC 9111
// predicts. Two controls run with the rows: the first send of each row must reach the server, and a reply
// with no freshness must reach it twice, so a count of one is a cache and not a server that counts nothing.
//
//   node testing/http_cache_rows.mjs
"use strict";
import { createServer } from 'node:http';
import { ZONE } from '../engine/trusted.mjs';
import { NAVIGABLE_NOTICE } from '../engine/noticegen.mjs';

const hits = new Map();
const count = (method, path) => hits.get(method + ' ' + path) || 0;
const server = createServer((req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  const key = req.method + ' ' + path;
  hits.set(key, (hits.get(key) || 0) + 1);
  const head = { 'Content-Type': 'text/plain', 'Access-Control-Allow-Origin': '*' };
  if (path === '/fresh' || path === '/inval' || path === '/part') head['Cache-Control'] = 'max-age=600';
  if (path === '/nostore') head['Cache-Control'] = 'no-store, max-age=600';
  if (path === '/vary') { head['Cache-Control'] = 'max-age=600'; head.Vary = 'X-Probe'; }
  res.writeHead(200, head);
  res.end(`${key} #${hits.get(key)} probe=${req.headers['x-probe'] || ''}`);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const PORT = server.address().port;
const A = `http://127.0.0.1:${PORT}`;
const B = `http://localhost:${PORT}`;

ZONE.safeFetchEgressStated({});
/* This test plays the router for the operator tier: the one sender it vouches for is its own object. */
const PERSON = { origin: 'test:person' };
ZONE.senderIsExtensionDocument = (s) => s === PERSON;

/* A page request as engine/trusted.mjs's workFetch states it, from a document at `page`. */
async function pageGet(page, url, headers) {
  const origin = new URL(page).origin;
  const r = await ZONE.safeFetch(url, {
    pageOrigin: origin, destination: '', mode: 'cors', provenance: 'observed', pinned: 'unpinned',
    driven: 'undriven', actor: 'page', credentialed: false, credentials: 'same-origin', useCorsPreflight: false,
    headers, doc: ZONE.docRecord('observed', 'undriven', 'unpinned', NAVIGABLE_NOTICE.sandbox.none,
                                 ZONE.safeFetchDocumentAddressSpace(page, null)),
    identity: await ZONE.pageRequestIdentity(origin, page, 'engine:' + origin, url, 'same-origin', 'cors',
                                             'strict-origin-when-cross-origin', page) });
  if (r.refusal) throw new Error(`${url} was refused: ${r.refusal.reason}`);
  /* A store completes when the clone of the body has been read, a task after the reply is returned. */
  await new Promise((done) => setImmediate(done));
  return { status: r.status, text: new TextDecoder().decode(r.body), age: r.headers.age };
}
async function personPost(page, url) {
  const origin = new URL(page).origin;
  const r = await ZONE.safeFetchOperator(url, {
    sender: PERSON, method: 'POST', headers: [], body: 'x', mode: 'cors',
    stated: { origin: null, referer: null, jar: null },
    identity: await ZONE.pageRequestIdentity(origin, page, 'engine:' + origin, url, 'omit', 'cors',
                                             'strict-origin-when-cross-origin', page) });
  if (r.refusal) throw new Error(`the person's POST ${url} was refused: ${r.refusal.reason}`);
}

const PAGE_A = `${A}/page`, PAGE_B = `${B}/page`;
const ROWS = [
  { label: 'CONTROL: a reply with no freshness is fetched again', run: async () => {
      await pageGet(PAGE_A, `${A}/stale`); await pageGet(PAGE_A, `${A}/stale`);
      return [['GET /stale', count('GET', '/stale'), 2]]; } },
  { label: 'a fresh hit sends nothing, and answers the stored body with an Age', run: async () => {
      const one = await pageGet(PAGE_A, `${A}/fresh`), two = await pageGet(PAGE_A, `${A}/fresh`);
      return [['GET /fresh', count('GET', '/fresh'), 1], ['same body', one.text === two.text, true],
              ['Age', two.age, '0']]; } },
  { label: 'a no-store reply is fetched again', run: async () => {
      await pageGet(PAGE_A, `${A}/nostore`); await pageGet(PAGE_A, `${A}/nostore`);
      return [['GET /nostore', count('GET', '/nostore'), 2]]; } },
  { label: 'a Vary mismatch misses, and a match hits', run: async () => {
      await pageGet(PAGE_A, `${A}/vary`, [['X-Probe', 'a']]);
      await pageGet(PAGE_A, `${A}/vary`, [['X-Probe', 'b']]);
      const again = await pageGet(PAGE_A, `${A}/vary`, [['X-Probe', 'a']]);
      return [['GET /vary', count('GET', '/vary'), 2], ['answered as a', /probe=a$/.test(again.text), true]]; } },
  { label: 'an unsafe method the server accepted invalidates the entry', run: async () => {
      await pageGet(PAGE_A, `${A}/inval`); await pageGet(PAGE_A, `${A}/inval`);
      const before = count('GET', '/inval');
      await personPost(PAGE_A, `${A}/inval`);
      await pageGet(PAGE_A, `${A}/inval`);
      return [['GET /inval before the POST', before, 1], ['POST /inval', count('POST', '/inval'), 1],
              ['GET /inval', count('GET', '/inval'), 2]]; } },
  { label: 'two partitions do not share an entry', run: async () => {
      await pageGet(PAGE_A, `${A}/part`);
      await pageGet(PAGE_B, `${A}/part`);
      await pageGet(PAGE_A, `${A}/part`);
      await pageGet(PAGE_B, `${A}/part`);
      return [['GET /part', count('GET', '/part'), 2]]; } },
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
/* Every row's first send reached the server, or nothing above is a reading. */
const first = ['/stale', '/fresh', '/nostore', '/vary', '/inval', '/part'].every((p) => count('GET', p) >= 1);
if (!first) { console.log('  CONTROL FAILED: a row\'s first send never reached the server'); process.exit(2); }
console.log(bad === 0 ? '  every row answered as Fetch §4.6 predicts' : `  ${bad} ROW(S) DISAGREED`);
process.exit(bad === 0 ? 0 : 1);
