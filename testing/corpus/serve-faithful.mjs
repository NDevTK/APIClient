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
const DOCURL = site.finalUrl || site.requestedUrl;
let html = doc.toString('utf8');

/* REWRITE THE ATTRIBUTE THE DOCUMENT ACTUALLY WROTE, RESOLVED THE WAY A BROWSER RESOLVES IT -- never by
   searching the document for the manifest's absolute URL.
   The manifest-string search matched only two spellings, the absolute URL and the same-host absolute PATH,
   and a document is free to write a third: material.angular.dev ships `src="polyfills-TKX23P4F.js"`, a bare
   relative specifier, and telegram/wikimedia/parabank do the same. None of those matched, so nothing was
   rewritten, the browser resolved them against 127.0.0.1 and every one landed outside `/_m/` -- and this
   server answered `not mirrored` as text/plain for ALL of them. `served 0, missed 20`.
   THAT WAS NOT A SILENT ZERO, IT WAS A CONFIDENT WRONG ANSWER: the engine parsed the 404 body as a classic
   script, `not mirrored` is two identifiers, and the abort it raised -- "did not COMPILE ... expecting ';'"
   at line 1 column 5, which is where `mirrored` starts -- ranked as the corpus's NUMBER ONE engine defect
   across five sites. A fixture that answers 200-with-prose where it means 404 manufactures engine bugs. */
/* DECODED THE SAME WAY THE BUILDER DECODES IT, so the URL this server looks up is the URL mirror.mjs saved
   under. An attribute value is HTML-escaped; the URL is what it decodes to. */
const unent = (s) => s.replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
  .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
  .replace(/&(lt|gt|quot|apos|amp);/g, (_, n) => ({ lt: '<', gt: '>', quot: '"', apos: "'", amp: '&' }[n]));
/* THE DOCUMENT BASE URL (HTML §2.4.3 "Document base URLs"), which is the first descendant `base` element's
   `href` (§4.2.3 "The base element") and only otherwise the document's own address. Three corpus documents
   declare one. Everything below resolves against THIS, not against DOCURL. */
const baseEl = html.match(/<base\b[^>]*\bhref\s*=\s*["']([^"']+)["']/i);
let BASEURL = site.baseUrl || DOCURL;
if (!site.baseUrl && baseEl) { try { BASEURL = new URL(unent(baseEl[1]), DOCURL).href; } catch { } }

/* AND THE `base` ELEMENT ITSELF IS LOCALIZED RATHER THAN LEFT OR DELETED. Left, it points at the real origin
   and every URL the page builds at RUNTIME leaves the fixture; deleted, those same URLs resolve against
   127.0.0.1 and reach a path the mirror never stored. Rewritten to the localized base, a runtime-relative
   specifier lands on exactly the resource the original would have. */
html = html.replace(/(<base\b[^>]*?\bhref\s*=\s*)(["'])([^"']*)\2/i,
  (m, head, q) => head + q + localize(BASEURL, DOCURL).replace(/[^/]*$/, '') + q);
html = html.replace(/(<(?:script|link)\b[^>]*?\b(?:src|href)\s*=\s*)(["'])([^"']*)\2/gi,
  (m, head, q, val) => (/^(data:|about:|javascript:|#)/i.test(val) || /<base\b/i.test(head) ? m
    : head + q + localize(unent(val), BASEURL) + q));

let served = 0, missed = 0;
createServer((req, res) => {
  const p = new URL(req.url, 'http://x');
  if (p.pathname === '/' || p.pathname === '/index.html') {
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
    return res.end(html);
  }
  /* THE CANDIDATE ORIGINAL URLS FOR THIS REQUEST, in the order a browser would have produced them.
     `/_m/<host><path>` is the rewritten form. Anything else is a SAME-ORIGIN request the page made at
     RUNTIME -- a chunk loaded by the bundler, an `import()` specifier -- which the attribute rewrite above
     never sees, so it must be resolved here against the document's own address. Both spellings are tried:
     against the origin root, and against the document's directory, because a bundler that ships
     `src="chunk.js"` from `/components/button/overview` means the second one. */
  const cands = [];
  if (p.pathname.startsWith('/_m/')) {
    try { cands.push(new URL('https://' + p.pathname.slice(4) + p.search)); } catch { }
  } else {
    // as an absolute path on the base's origin, then relative to the base URL, then to the document's own
    try { cands.push(new URL(p.pathname + p.search, new URL(BASEURL).origin)); } catch { }
    try { cands.push(new URL(p.pathname.replace(/^\/+/, '') + p.search, BASEURL)); } catch { }
    try { cands.push(new URL(p.pathname + p.search, new URL(DOCURL).origin)); } catch { }
  }
  for (const u of cands) {
    const f = savedPath(u);
    if (!existsSync(f)) continue;
    served++;
    const r = byUrl.get(u.href);
    res.writeHead(200, { 'content-type': (r && r.contentType) || 'application/javascript' });
    return res.end(readFileSync(f));
  }
  /* A 404 here is a REAL fact about the mirror and is counted, not hidden: a fixture that silently serves
     an empty body for a missing chunk is the flattening defect in another costume.
     THE BODY IS EMPTY AND THE TYPE IS `text/javascript` FOR A SCRIPT-SHAPED PATH, because the previous body
     -- the words `not mirrored`, served as text/plain -- was VALID-ENOUGH JAVASCRIPT TO PARSE-ERROR, and the
     engine's abort on it was ranked as this corpus's top defect across five sites. A missing chunk must be
     missing, not a program. The status stays 404 so the count above is still the honest one. */
  missed++;
  console.error('MISS ' + p.pathname + p.search);
  res.writeHead(404, { 'content-type': /\.m?js(\?|$)/i.test(p.pathname) ? 'text/javascript' : 'text/plain' });
  res.end('');
}).listen(port, '127.0.0.1', () => {
  console.log(`${id} on ${port} — ${(site.resources || []).length} resources, doc ${(doc.length / 1024).toFixed(0)}KiB`);
});
process.on('SIGTERM', () => { console.log(`served ${served}, missed ${missed}`); process.exit(0); });
