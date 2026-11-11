// Send coalescing (extension/lib/send-coalesce.js) as engine/trusted.mjs's zone runs it: every row is a safeFetch
// through that zone against a loopback server that counts what reached it. Many forced worlds of one document
// asking one byte-identical request reach the server once, and each ask is answered as its own reply; a request
// that differs in a header, in its Cookie, or in its document is sent. The control runs first: one URL asked by
// two documents reaches the server twice, so a count of one is coalescing and not a server that counts nothing.
//
//   node testing/send_coalesce_rows.mjs
"use strict";
import { createServer } from 'node:http';
import { trustedZone } from '../engine/trusted.mjs';
import { siteRegistrableDomains } from '../engine/psl.mjs';
import { NAVIGABLE_NOTICE } from '../engine/noticegen.mjs';

const hits = new Map();
const count = (method, path) => hits.get(method + ' ' + path) || 0;
/* The Cookie each send of a path carried, in arrival order. */
const cookies = new Map();
const SITE_Y = 'http://127.0.0.1:2';
const server = createServer((req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  const key = req.method + ' ' + path;
  hits.set(key, (hits.get(key) || 0) + 1);
  cookies.set(path, (cookies.get(path) || []).concat([req.headers.cookie || null]));
  const head = { 'Content-Type': 'text/plain', 'Access-Control-Allow-Origin': '*', 'Cache-Control': 'no-store' };
  if (req.method === 'OPTIONS') {
    head['Access-Control-Allow-Headers'] = 'X-Probe';
    res.writeHead(204, head);
    res.end();
    return;
  }
  if (path === '/setc') head['Set-Cookie'] = 'c=1; Path=/';
  /* Grants one origin that is not the asker's, so the asker's CORS check refuses every answer. */
  if (path === '/acao') head['Access-Control-Allow-Origin'] = 'http://127.0.0.1:1';
  res.writeHead(200, head);
  /* A body in two parts with a pause between, so an ask can join while it is still arriving. */
  if (path === '/slow') {
    res.write(`${key} #${hits.get(key)} part one;`);
    setTimeout(() => res.end(' part two'), 300);
    return;
  }
  res.end(`${key} #${hits.get(key)} probe=${req.headers['x-probe'] || ''}`);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const PORT = server.address().port;
const A = `http://127.0.0.1:${PORT}`;
const B = `http://localhost:${PORT}`;

const ZONE = trustedZone();
ZONE.httpCacheStated(null, null);
ZONE.safeFetchEgressStated({});
/* A world whose address a fork pinned fires at a widened origin; B stays unwidened, so a pinned ask there is
   declined (safe-fetch.js's default arms). */
ZONE.safeFetchWiden(A);
/* The person's jar, as chrome.cookies would answer it, for the credentialed rows. */
const jar = [];
let cookieWrites = 0;
ZONE.swRpc = async (api, arg) => {
  if (api === 'site.registrableDomains') return siteRegistrableDomains(arg);
  if (api === 'cookies.getAll') {
    if (arg.partitionKey) return [];
    return jar.filter((c) => arg.name === undefined || c.name === arg.name);
  }
  if (api === 'cookies.set') {
    cookieWrites++;
    const at = jar.findIndex((c) => c.name === arg.name);
    if (at >= 0) jar.splice(at, 1);
    jar.push({ name: arg.name, value: arg.value, path: arg.path, sameSite: arg.sameSite || 'unspecified' });
    return { refused: null };
  }
  throw new Error('this test serves no rpc ' + JSON.stringify(api));
};

/* A page request as engine/trusted.mjs's workFetch states it, from the document `docKey` at `page`, in the
   world graded `provenance`. */
async function ask(page, url, docKey, { provenance = 'observed', pinned = 'unpinned', headers, credentialed = false,
                                        signal } = {}) {
  const origin = new URL(page).origin;
  const credentials = credentialed ? 'include' : 'same-origin';
  return ZONE.safeFetch(url, {
    pageOrigin: origin, destination: '', mode: 'cors', provenance, pinned, driven: 'undriven',
    address: 'concrete', method: 'GET', body: null, actor: 'page', credentialed, credentials, useCorsPreflight: false, headers, signal,
    doc: ZONE.docRecord('observed', 'undriven', 'unpinned', NAVIGABLE_NOTICE.sandbox.none,
                        ZONE.safeFetchDocumentAddressSpace(page, null)),
    identity: await ZONE.pageRequestIdentity(origin, page, docKey, url, credentials, 'cors',
                                             'strict-origin-when-cross-origin', page) });
}
const text = (r) => new TextDecoder().decode(r.body);
const answered = (r) => r.refusal === null && r.status === 200;
const census = (docKey) => JSON.stringify(ZONE.sendCoalesceCensus(docKey));
const WORLDS = ['observed', 'derived', 'forced'];

const PAGE_A = `${A}/page`, PAGE_B = `${B}/page`;
const ROWS = [
  { label: 'CONTROL: one URL asked by two documents is sent twice', run: async () => {
      const one = await ask(PAGE_A, `${A}/twin`, 'engine:twin-1'), two = await ask(PAGE_A, `${A}/twin`, 'engine:twin-2');
      return [['GET /twin', count('GET', '/twin'), 2], ['both answered', answered(one) && answered(two), true],
              ['second document read the second send', /#2 /.test(text(two)), true]]; } },
  { label: '60 worlds of one document (observed, derived, forced) asking one request: one send, 60 answers',
    run: async () => {
      const doc = 'engine:worlds';
      const together = await Promise.all(Array.from({ length: 40 }, (_, i) =>
        ask(PAGE_A, `${A}/boot?lang=en&v=3`, doc, { provenance: WORLDS[i % 3], pinned: i % 6 === 5 ? 'pinned' : 'unpinned' })));
      const later = [];
      for (let i = 0; i < 20; i++) later.push(await ask(PAGE_A, `${A}/boot?lang=en&v=3#frag${i}`, doc,
                                                       { provenance: WORLDS[i % 3] }));
      const all = together.concat(later);
      return [['GET /boot', count('GET', '/boot'), 1], ['answered', all.filter(answered).length, 60],
              ['every body is the one send\'s', all.every((r) => /^GET \/boot #1 /.test(text(r))), true],
              ['census', census(doc), JSON.stringify({ sends: 1, answers: 59, identities: 1 })]]; } },
  { label: 'the firing decision is each world\'s: at an unwidened origin a pinned ask is answered only from the ' +
           'record of a send the unforced run made',
    run: async () => {
      const doc = 'engine:gate';
      const seen = await ask(PAGE_B, `${B}/gate`, doc);
      const same = await ask(PAGE_B, `${B}/gate`, doc, { provenance: 'forced', pinned: 'pinned' });
      const alone = await ask(PAGE_B, `${B}/gate-alone`, doc, { provenance: 'forced', pinned: 'pinned' });
      return [['GET /gate', count('GET', '/gate'), 1], ['observed answered', answered(seen), true],
              ['the pinned ask of that send read its reply', answered(same) && text(same), 'GET /gate #1 probe='],
              ['GET /gate-alone', count('GET', '/gate-alone'), 0],
              ['a pinned ask of no such send declined', alone.refusal && alone.refusal.kind, 'decline']]; } },
  { label: 'a request differing in one header is another identity', run: async () => {
      const doc = 'engine:hdr';
      await ask(PAGE_A, `${A}/hdr`, doc, { headers: [['X-Probe', 'a']] });
      await ask(PAGE_A, `${A}/hdr`, doc, { headers: [['X-Probe', 'b']] });
      const again = await ask(PAGE_A, `${A}/hdr`, doc, { headers: [['X-Probe', 'a']] });
      return [['GET /hdr', count('GET', '/hdr'), 2], ['a answered from its own send', /#1 probe=a$/.test(text(again)), true],
              ['census', census(doc), JSON.stringify({ sends: 2, answers: 1, identities: 2 })]]; } },
  { label: 'each asker receives the Set-Cookie and stores it; a request differing only in Cookie is sent',
    run: async () => {
      const doc = 'engine:cookie';
      const together = await Promise.all(Array.from({ length: 10 }, () =>
        ask(PAGE_A, `${A}/setc`, doc, { credentialed: true })));
      const sendsBefore = count('GET', '/setc'), writesBefore = cookieWrites;
      const withCookie = await ask(PAGE_A, `${A}/setc`, doc, { credentialed: true });
      return [['GET /setc before the jar held c', sendsBefore, 1],
              ['each asker\'s reply carries the field', together.every((r) => r.setCookies.length === 1 &&
                                                                       r.setCookies[0][1] === 'c=1; Path=/'), true],
              ['jar writes, one per asker', writesBefore, 10],
              ['GET /setc', count('GET', '/setc'), 2], ['second send carried', cookies.get('/setc')[1], 'c=1'],
              ['answered', answered(withCookie), true]]; } },
  { label: 'an ask joining while the body arrives, and one after it ended, read the whole body', run: async () => {
      const doc = 'engine:slow';
      const first = ask(PAGE_A, `${A}/slow`, doc);
      await new Promise((r) => setTimeout(r, 100));
      const joined = ask(PAGE_A, `${A}/slow`, doc);
      const [one, two] = await Promise.all([first, joined]);
      const three = await ask(PAGE_A, `${A}/slow`, doc);
      const want = 'GET /slow #1 part one; part two';
      return [['GET /slow', count('GET', '/slow'), 1], ['first', text(one), want], ['joined', text(two), want],
              ['after', text(three), want]]; } },
  { label: 'one asker\'s abort ends its own answer and never the send', run: async () => {
      const doc = 'engine:abort';
      const ctl = new AbortController();
      const quitter = ask(PAGE_A, `${A}/slow`, doc, { signal: ctl.signal }).then(() => 'answered', (e) => e.name);
      const stayer = ask(PAGE_A, `${A}/slow`, doc);
      await new Promise((r) => setTimeout(r, 100));
      ctl.abort();
      const [quit, stay] = await Promise.all([quitter, stayer]);
      return [['GET /slow', count('GET', '/slow'), 2], ['the aborted ask', quit, 'TypeError'],
              ['the other ask', text(stay), 'GET /slow #2 part one; part two']]; } },
  { label: 'a replayed reply runs the CORS check as a fresh one does', run: async () => {
      const doc = 'engine:cors';
      const one = await ask(`${SITE_Y}/page`, `${A}/acao`, doc), two = await ask(`${SITE_Y}/page`, `${A}/acao`, doc);
      return [['GET /acao', count('GET', '/acao'), 1],
              ['first refused', /^blocked-cors:/.test(one.statusText), true],
              ['replay refused', /^blocked-cors:/.test(two.statusText), true]]; } },
  { label: 'a CORS preflight is one act too: one OPTIONS and one GET for two asks', run: async () => {
      const doc = 'engine:pf';
      const one = await ask(PAGE_B, `${A}/pf`, doc, { headers: [['X-Probe', 'a']] });
      const two = await ask(PAGE_B, `${A}/pf`, doc, { headers: [['X-Probe', 'a']] });
      return [['OPTIONS /pf', count('OPTIONS', '/pf'), 1], ['GET /pf', count('GET', '/pf'), 1],
              ['both answered', answered(one) && answered(two), true]]; } },
  { label: 'a document that ended forgets its sends: the same ask is sent again', run: async () => {
      const doc = 'engine:ended';
      await ask(PAGE_A, `${A}/ended`, doc);
      ZONE.sendCoalesceDocumentEnded(doc);
      const gone = ZONE.sendCoalesceCensus(doc);
      await ask(PAGE_A, `${A}/ended`, doc);
      return [['census after the end', gone, null], ['GET /ended', count('GET', '/ended'), 2]]; } },
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
const first = ['/twin', '/boot', '/gate', '/hdr', '/setc', '/slow', '/acao', '/pf', '/ended'].every((p) => count('GET', p) >= 1);
if (!first) { console.log('  CONTROL FAILED: a row\'s first send never reached the server'); process.exit(2); }
console.log(bad === 0 ? '  every row was sent once per identity' : `  ${bad} ROW(S) DISAGREED`);
process.exit(bad === 0 ? 0 : 1);
