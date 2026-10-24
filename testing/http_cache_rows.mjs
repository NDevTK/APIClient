// The HTTP cache (extension/lib/http-cache.js) as engine/trusted.mjs's zone runs it: every row is a
// safeFetch, or an operator send, through that zone against a loopback server that counts what reached it.
// A row passes when the server's count is what Fetch §4.6 "HTTP-network-or-cache fetch" over RFC 9111
// predicts. Two controls run with the rows: the first send of each row must reach the server, and a reply
// with no freshness must reach it twice, so a count of one is a cache and not a server that counts nothing.
//
//   node testing/http_cache_rows.mjs
"use strict";
import { createServer } from 'node:http';
import { ZONE, trustedZone } from '../engine/trusted.mjs';
import { NAVIGABLE_NOTICE } from '../engine/noticegen.mjs';

const hits = new Map();
const count = (method, path) => hits.get(method + ' ' + path) || 0;
/* The validators each send carried, by path, in arrival order. */
const conditions = new Map();
const OLD = 'Wed, 01 Jan 2020 00:00:00 GMT';
const server = createServer((req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  const key = req.method + ' ' + path;
  hits.set(key, (hits.get(key) || 0) + 1);
  const inm = req.headers['if-none-match'] || null, ims = req.headers['if-modified-since'] || null;
  conditions.set(path, (conditions.get(path) || []).concat([[inm, ims]]));
  const head = { 'Content-Type': 'text/plain', 'Access-Control-Allow-Origin': '*' };
  if (path === '/fresh' || path === '/inval' || path === '/part' || path === '/pf' ||
      path === '/kept' || path === '/opq') head['Cache-Control'] = 'max-age=600';
  if (req.method === 'OPTIONS') head['Access-Control-Allow-Headers'] = 'X-Probe';
  if (path === '/nostore') head['Cache-Control'] = 'no-store, max-age=600';
  if (path === '/vary') { head['Cache-Control'] = 'max-age=600'; head.Vary = 'X-Probe'; }
  /* Stale at once, with an entity tag the server always matches. */
  if (path === '/etag') {
    Object.assign(head, { 'Cache-Control': 'max-age=0', ETag: '"v1"' });
    if (inm === '"v1"') { res.writeHead(304, head); res.end(); return; }
  }
  /* `no-cache` with only a date; its 304 carries no validator, as Python's http.server answers one. */
  if (path === '/lm') {
    Object.assign(head, { 'Cache-Control': 'no-cache', 'Last-Modified': OLD });
    if (ims === OLD) { res.writeHead(304, { 'Access-Control-Allow-Origin': '*' }); res.end(); return; }
  }
  /* A new entity tag on every send, so a validation is always answered in full. */
  if (path === '/changed') Object.assign(head, { 'Cache-Control': 'max-age=0', ETag: `"v${hits.get(key)}"` });
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
/* `docKey` names the document; an opaque one (a `data:` page) is given the loopback space its creator had. */
async function pageGet(page, url, headers, zone = ZONE, docKey = 'engine:' + new URL(page).origin) {
  const origin = new URL(page).origin;
  const space = origin === 'null' ? 'loopback' : zone.safeFetchDocumentAddressSpace(page, null);
  const r = await zone.safeFetch(url, {
    pageOrigin: origin, destination: '', mode: 'cors', provenance: 'observed', pinned: 'unpinned',
    driven: 'undriven', actor: 'page', credentialed: false, credentials: 'same-origin', useCorsPreflight: false,
    headers, doc: zone.docRecord('observed', 'undriven', 'unpinned', NAVIGABLE_NOTICE.sandbox.none, space),
    identity: await zone.pageRequestIdentity(origin, page, docKey, url, 'same-origin', 'cors',
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
  /* `Date` has a resolution of one second, so the apparent age (RFC 9111 §4.2.3) of an immediate hit is 0 or
     1; the row asks that `Age` is one of the two, not which. */
  { label: 'a fresh hit sends nothing, and answers the stored body with an Age', run: async () => {
      const one = await pageGet(PAGE_A, `${A}/fresh`), two = await pageGet(PAGE_A, `${A}/fresh`);
      return [['GET /fresh', count('GET', '/fresh'), 1], ['same body', one.text === two.text, true],
              ['Age is 0 or 1', /^[01]$/.test(two.age), true]]; } },
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
  { label: 'two opaque top-level documents are two partitions, and each hits its own entry', run: async () => {
      const page = 'data:text/html,<p>opaque';
      await pageGet(page, `${A}/opq`, undefined, ZONE, 'engine:opaque-1');
      await pageGet(page, `${A}/opq`, undefined, ZONE, 'engine:opaque-1');
      await pageGet(page, `${A}/opq`, undefined, ZONE, 'engine:opaque-2');
      return [['GET /opq', count('GET', '/opq'), 2]]; } },
  { label: 'two partitions do not share an entry', run: async () => {
      await pageGet(PAGE_A, `${A}/part`);
      await pageGet(PAGE_B, `${A}/part`);
      await pageGet(PAGE_A, `${A}/part`);
      await pageGet(PAGE_B, `${A}/part`);
      return [['GET /part', count('GET', '/part'), 2]]; } },
  { label: 'a second session restored from what the first wrote back answers a fresh entry with no send',
    run: async () => {
      await pageGet(PAGE_A, `${A}/kept`);
      const kept = ZONE.httpCacheTakeDirty();
      /* A host's store: the second zone states a restore, restores every record, then settles it. */
      const next = trustedZone();
      next.safeFetchEgressStated({});
      let restored;
      next.httpCacheStated(new Promise((done) => { restored = done; }), null);
      for (const rec of kept) if (rec.entries.length > 0) next.httpCacheRestore(rec);
      restored();
      const two = await pageGet(PAGE_A, `${A}/kept`, undefined, next);
      return [['records written back', kept.some((r) => r.target === `${A}/kept`), true],
              ['GET /kept', count('GET', '/kept'), 1], ['stored body', /#1 /.test(two.text), true]]; } },
  { label: 'a fresh hit that needs a CORS preflight sends the preflight and not the request', run: async () => {
      const one = await pageGet(PAGE_B, `${A}/pf`, [['X-Probe', 'a']]);
      const two = await pageGet(PAGE_B, `${A}/pf`, [['X-Probe', 'a']]);
      return [['OPTIONS /pf', count('OPTIONS', '/pf'), 2], ['GET /pf', count('GET', '/pf'), 1],
              ['stored body', two.text, one.text]]; } },
  { label: 'a stale entry is validated with its entity tag, and a 304 answers the stored body', run: async () => {
      const one = await pageGet(PAGE_A, `${A}/etag`), two = await pageGet(PAGE_A, `${A}/etag`);
      return [['GET /etag', count('GET', '/etag'), 2], ['If-None-Match sent', conditions.get('/etag')[1][0], '"v1"'],
              ['status', two.status, 200], ['stored body', two.text, one.text]]; } },
  { label: 'a no-cache entry is validated with its date, and a validator-less 304 answers it', run: async () => {
      const one = await pageGet(PAGE_A, `${A}/lm`), two = await pageGet(PAGE_A, `${A}/lm`);
      return [['GET /lm', count('GET', '/lm'), 2], ['If-Modified-Since sent', conditions.get('/lm')[1][1], OLD],
              ['status', two.status, 200], ['stored body', two.text, one.text]]; } },
  { label: 'a full reply to a validation replaces the entry', run: async () => {
      await pageGet(PAGE_A, `${A}/changed`);
      const two = await pageGet(PAGE_A, `${A}/changed`);
      await pageGet(PAGE_A, `${A}/changed`);
      return [['new body', /#2 /.test(two.text), true],
              ['third send validates the second', conditions.get('/changed')[2][0], '"v2"']]; } },
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
const first = ['/stale', '/fresh', '/nostore', '/vary', '/inval', '/part', '/pf', '/etag', '/lm', '/changed', '/kept', '/opq']
  .every((p) => count('GET', p) >= 1);
if (!first) { console.log('  CONTROL FAILED: a row\'s first send never reached the server'); process.exit(2); }
console.log(bad === 0 ? '  every row answered as Fetch §4.6 predicts' : `  ${bad} ROW(S) DISAGREED`);
process.exit(bad === 0 ? 0 : 1);
