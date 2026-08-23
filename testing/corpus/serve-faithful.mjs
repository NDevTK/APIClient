// SERVE THE MIRROR WITHOUT FLATTENING, BECAUSE THE QUERY STRING IS PROGRAM INPUT.
//
// The earlier fixture server rewrote every `<script src>` to `/sN.js`. That is not a cosmetic
// simplification -- it deletes two things the page reads:
//
//   1. THE QUERY. vuejs.org's banner code does `params = parse(document.currentScript.getAttribute('src'))`
//      and its FIRST guard is `affiliateCode = params.affiliate || params.from; if (!affiliateCode) return;`.
//      Served as `/s2.js` that guard returns at line 28 and both of its fetch sites are dead -- and REAL
//      CHROME DOES THE SAME ON THOSE BYTES, so a zero measured there is a fact about the fixture, not the
//      engine. 122 of the 740 mirrored resources carry a query.
//   2. THE MODULE GRAPH. A `type="module"` chunk's own relative `import` resolves against ITS url, not the
//      document's, so flattening every script to one directory 404s every relative specifier.
//
// So: each resource is served at its ORIGINAL host+path+query under /_m/, and the document's script
// references are rewritten only in their ORIGIN -- path and query survive byte-for-byte. mirror.mjs stores a
// query as a `__q<sha256[0:8]>` filename suffix, so the lookup recomputes that from the request's own query
// rather than keeping a second index that could disagree with the tree.
import { createServer } from 'node:http';
import { readFileSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = dirname(fileURLToPath(import.meta.url));
const MIRROR = join(ROOT, 'mirror');
const manifest = JSON.parse(readFileSync(join(ROOT, 'provenance.json'), 'utf8'));

const id = process.argv[2];
const port = Number(process.argv[3] || 8900);
const site = manifest.find(m => m.id === id);
if (!site) { console.error('no such id: ' + id + '\nhave: ' + manifest.map(m => m.id).join(' ')); process.exit(2); }

/* The saved path for a URL, by mirror.mjs's own rule -- recomputed, never cached, so this server cannot
   disagree with the tree it serves. */
const savedPath = (u) => {
  let rel = u.host + u.pathname + (u.search ? '__q' + createHash('sha256').update(u.search).digest('hex').slice(0, 8) : '');
  if (rel.endsWith('/')) rel += 'index';
  return join(MIRROR, id, rel.replace(/[^A-Za-z0-9._/@%+-]/g, '_'));
};

const byUrl = new Map();
for (const r of site.resources || []) if (r.url && r.path) byUrl.set(r.url, r);

/* Rewrite ONLY the origin. A cross-origin script becomes /_m/<host><path><query>; a same-origin one keeps
   its own path. The attribute the page reads back therefore still carries everything after the host. */
const localize = (abs, docUrl) => {
  try {
    const u = new URL(abs, docUrl);
    return '/_m/' + u.host + u.pathname + u.search;
  } catch { return abs; }
};

/* mirror.mjs writes the DOCUMENT as index.html and gives it no `path` in the manifest -- only resources
   carry one. Read it where the builder puts it rather than deriving a path the builder never used. */
const doc = readFileSync(join(MIRROR, id, 'index.html'));
let html = doc.toString('utf8');
for (const r of site.resources || []) {
  if (!r.url) continue;
  const loc = localize(r.url, site.finalUrl || site.requestedUrl);
  // Replace the exact attribute value the document used, in every quoting style it may have used.
  for (const q of ['"', "'"]) html = html.split(q + r.url + q).join(q + loc + q);
  try {
    const rel = new URL(r.url).pathname + new URL(r.url).search;
    if (new URL(r.url).host === new URL(site.finalUrl || site.requestedUrl).host)
      for (const q of ['"', "'"]) html = html.split(q + rel + q).join(q + loc + q);
  } catch { /* not a URL we can relativise */ }
}

let served = 0, missed = 0;
createServer((req, res) => {
  const p = new URL(req.url, 'http://x');
  if (p.pathname === '/' || p.pathname === '/index.html') {
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
    return res.end(html);
  }
  if (p.pathname.startsWith('/_m/')) {
    const rest = p.pathname.slice(4);
    const u = new URL('https://' + rest + p.search);
    const f = savedPath(u);
    if (existsSync(f)) {
      served++;
      const r = byUrl.get(u.href);
      res.writeHead(200, { 'content-type': (r && r.contentType) || 'application/javascript' });
      return res.end(readFileSync(f));
    }
  }
  /* A 404 here is a REAL fact about the mirror and is counted, not hidden: a fixture that silently serves
     an empty body for a missing chunk is the flattening defect in another costume. */
  missed++;
  console.error('MISS ' + p.pathname + p.search);
  res.writeHead(404, { 'content-type': 'text/plain' });
  res.end('not mirrored');
}).listen(port, '127.0.0.1', () => {
  console.log(`${id} on ${port} — ${(site.resources || []).length} resources, doc ${(doc.length / 1024).toFixed(0)}KiB`);
});
process.on('SIGTERM', () => { console.log(`served ${served}, missed ${missed}`); process.exit(0); });
