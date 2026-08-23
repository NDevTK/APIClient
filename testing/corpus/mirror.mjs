// PROVENANCE + A FROZEN COPY, so a later run can tell A SITE CHANGING from THE ENGINE CHANGING.
//
// For each corpus row: fetch the document (following redirects), then every `<script src>` and
// `<link rel=modulepreload|preload as=script>` it names, saving each at its ORIGINAL path under
// mirror/<id>/<host>/<path> — NOT flattened to s0.js, because a flattened tree throws away the module
// graph (a `type="module"` chunk's own relative `import` resolves against ITS url, not the document's) and
// that fixture defect has already produced false crashes in this session's earlier census.
//
// The manifest records, per resource: absolute URL, HTTP status, content-type, byte length, sha256, and the
// UTC instant it was read. That triple (url, date, hash) is the whole point: a census row that changes
// against an unchanged hash is the engine; against a changed hash it is the site.
import { mkdirSync, writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { dirname, join } from 'node:path';
import { readFileSync } from 'node:fs';

const ROOT = new URL('.', import.meta.url).pathname;
const rows = readFileSync(join(ROOT, 'sites.tsv'), 'utf8').trim().split('\n').map(l => l.split('\t'));
const only = process.argv[2];

function get(url) {
  const hdr = '/tmp/.mh.txt';
  let body;
  try {
    body = execFileSync('curl', ['-sS', '-L', '--max-time', '40', '-D', hdr,
      '-A', 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36',
      '-w', '\\n@@FINAL@@%{url_effective}\\n@@CODE@@%{http_code}\\n', url],
      { maxBuffer: 128 * 1024 * 1024 });
  } catch (e) { return { err: String(e.message || e).slice(0, 160) }; }
  const s = body.toString('binary');
  const mF = s.lastIndexOf('\n@@FINAL@@');
  const tail = s.slice(mF + 1);
  const final = (tail.match(/@@FINAL@@(.*)/) || [, url])[1];
  const code = Number((tail.match(/@@CODE@@(\d+)/) || [, 0])[1]);
  const buf = Buffer.from(s.slice(0, mF), 'binary');
  let ct = '';
  try {
    const h = readFileSync(hdr, 'utf8');
    const all = [...h.matchAll(/^content-type:\s*(.*)$/gim)];
    ct = all.length ? all[all.length - 1][1].trim() : '';
  } catch { }
  return { buf, final, code, ct };
}
const sha = (b) => createHash('sha256').update(b).digest('hex');
const now = () => new Date().toISOString();

/* MERGE, never replace: a single-site re-fetch used to write a one-entry provenance.json over the other
   twenty-nine, which is exactly the shape of a measurement destroyed by a process that was never asked
   about it. The file is the corpus's identity, so a partial run updates its own rows and leaves the rest. */
let manifest = [];
try { manifest = JSON.parse(readFileSync(join(ROOT, 'provenance.json'), 'utf8')); } catch { }
const put = (rec) => { const i = manifest.findIndex(r => r.id === rec.id); if (i >= 0) manifest[i] = rec; else manifest.push(rec); };
for (const [id, url, stack] of rows) {
  if (only && only !== id) continue;
  const dir = join(ROOT, 'mirror', id);
  mkdirSync(dir, { recursive: true });
  const doc = get(url);
  if (doc.err || !doc.buf) { put({ id, stack, requestedUrl: url, error: doc.err || 'no body', fetchedAt: now() }); console.log(id, 'FAILED', doc.err); continue; }
  const html = doc.buf.toString('utf8');
  writeFileSync(join(dir, 'index.html'), doc.buf);
  const rec = {
    id, stack, requestedUrl: url, finalUrl: doc.final, status: doc.code,
    contentType: doc.ct, bytes: doc.buf.length, sha256: sha(doc.buf), fetchedAt: now(),
    resources: [],
  };
  /* AN ATTRIBUTE VALUE IS HTML-ESCAPED AND THE URL IS WHAT IT DECODES TO. Read raw, twitch's preload href
     came out holding `&#x3D;` and `&amp;` literally, so curl requested a URL the origin had never issued and
     saved its 43-byte ERROR body as the resource -- which the engine then compiled as a program and aborted
     on. A fixture that stores an error page under a real URL's name manufactures an engine defect, the same
     way serve-faithful's 404 prose did. Only the five predefined references can appear in an attribute value
     plus numeric character references, so this decodes exactly those rather than pulling in a parser. */
  const unent = (s) => s.replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
    .replace(/&(lt|gt|quot|apos|amp);/g, (_, n) => ({ lt: '<', gt: '>', quot: '"', apos: "'", amp: '&' }[n]));
  const srcs = new Set();
  for (const m of html.matchAll(/<script[^>]+src=["']([^"']+)["']/gi)) srcs.add(unent(m[1]));
  for (const m of html.matchAll(/<link[^>]+rel=["'](?:modulepreload|preload)["'][^>]*href=["']([^"']+)["']/gi)) srcs.add(unent(m[1]));
  for (const m of html.matchAll(/<link[^>]+href=["']([^"']+)["'][^>]*rel=["'](?:modulepreload)["']/gi)) srcs.add(unent(m[1]));
  /* RESOLVE AGAINST THE DOCUMENT BASE URL, WHICH IS NOT ALWAYS THE DOCUMENT'S ADDRESS. HTML §2.4.3 "Document
     base URLs": if the document has a descendant `base` element with an `href` (§4.2.3 "The base element"),
     that is the base; only otherwise is it the document's own address.
     THREE OF THE THIRTY CORPUS SITES DECLARE ONE and this resolved against `doc.final` instead, so it fetched
     addresses those origins had never published: material.angular.dev ships `<base href="/">` with
     `src="polyfills-TKX23P4F.js"`, which is `/polyfills-TKX23P4F.js` and was fetched as
     `/components/button/polyfills-TKX23P4F.js`. Every one came back 404 -- and a 404 has a BODY, so 21376
     bytes of `<!doctype html><title>Page Not Found</title>` were saved under a `.js` name and served to the
     engine as a classic script. The engine did the only correct thing with it and aborted ("unexpected token
     in expression: '<'"), and that abort was about this builder. */
  const baseEl = html.match(/<base\b[^>]*\bhref\s*=\s*["']([^"']+)["']/i);
  let baseUrl = doc.final;
  if (baseEl) { try { baseUrl = new URL(unent(baseEl[1]), doc.final).href; } catch { } }
  rec.baseUrl = baseUrl;
  let n = 0;
  for (const src of srcs) {
    if (/^data:/i.test(src)) continue;
    let abs; try { abs = new URL(src, baseUrl).href; } catch { continue; }
    const r = get(abs);
    if (r.err || !r.buf) { rec.resources.push({ url: abs, error: r.err || 'no body' }); continue; }
    const u = new URL(abs);
    let rel = u.host + u.pathname + (u.search ? '__q' + createHash('sha256').update(u.search).digest('hex').slice(0, 8) : '');
    if (rel.endsWith('/')) rel += 'index';
    const f = join(dir, rel.replace(/[^A-Za-z0-9._/@%+-]/g, '_'));
    mkdirSync(dirname(f), { recursive: true });
    writeFileSync(f, r.buf);
    rec.resources.push({ url: abs, status: r.code, contentType: r.ct, bytes: r.buf.length, sha256: sha(r.buf), fetchedAt: now(), path: rel });
    n++;
  }
  put(rec);
  console.log(`${id}\t${doc.code}\t${(doc.buf.length / 1024).toFixed(0)}KiB doc\t${n}/${srcs.size} scripts\t${(rec.resources.reduce((a, x) => a + (x.bytes || 0), 0) / 1024).toFixed(0)}KiB js`);
}
writeFileSync(join(ROOT, 'provenance.json'), JSON.stringify(manifest, null, 1));
console.log('wrote provenance.json (' + manifest.length + ' sites)');
