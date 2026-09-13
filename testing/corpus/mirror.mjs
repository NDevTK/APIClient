// PROVENANCE + A FROZEN COPY, so a later run can tell A SITE CHANGING from THE ENGINE CHANGING.
//
// For each corpus row: fetch the document (following redirects), then every `<script src>` and every
// `<link rel=modulepreload|preload>` it names -- EVERY preload, WHATEVER ITS `as`, which this line used
// to deny. It said `preload as=script` and the matcher below has never read `as` at all: MEASURED over
// the frozen corpus, 46 of 445 resources across 10 of 18 rows are not JavaScript (16 woff2, 15 svg, 13
// css, one json, one html), and the oldest of those rows was frozen long before anybody doubted the
// sentence. THE CODE IS RIGHT AND THE SENTENCE WAS WRONG, so the sentence changed: serve-faithful.mjs
// replays each resource under its RECORDED contentType, so a captured font or stylesheet answers as the
// origin's did, and narrowing the capture to `as=script` would turn all 46 into fixture 404s the real
// page never saw. A reader who wants `the app's JavaScript` filters the record by contentType, which is
// already recorded per resource; no field is needed for it and none is added. Saved at its ORIGINAL path
// under mirror/<id>/<host>/<path> — NOT flattened to s0.js, because a flattened tree throws away the module
// graph (a `type="module"` chunk's own relative `import` resolves against ITS url, not the document's) and
// that fixture defect has already produced false crashes in this session's earlier census.
//
// The manifest records, per resource: absolute URL, HTTP status, content-type, byte length, sha256, and the
// UTC instant it was read. That triple (url, date, hash) is the whole point: a census row that changes
// against an unchanged hash is the engine; against a changed hash it is the site.
//
// AND THE DOCUMENT'S `Content-Security-Policy`, WHICH IS PART OF WHAT WAS SERVED AND WAS THE ONE PART THIS
// RECORD THREW AWAY. The header was already in hand — `get` writes the whole response header block to a dump
// and reads `content-type` out of it — and the policy beside it was dropped, so a site whose CSP changed
// looked identical here to one whose CSP did not, which is the single class of change this file exists to
// separate: CLAUDE.md §@S(a) measures EVERY breakout against the document's actual policy, so a policy edit
// alone can flip every security verdict on a row while the bytes, the status and every hash hold.
// IT IS ALSO WHAT A FAITHFUL REPLAY NEEDS AND DOES NOT YET GET — see the residual at `rec.csp` below.
//
// THE FIELD IS ALWAYS WRITTEN, `""` FOR A RESPONSE THAT STATED NONE, because absent and empty are different
// facts and a reader must not merge them: `""` is "this response carried no policy", and ABSENT is "this row
// was captured before this field existed and says nothing about the policy either way". A row re-mirrored
// after this diff gains the field; the rows already in provenance.json keep neither claim.
import { mkdirSync, writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { dirname, join } from 'node:path';
import { readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { siteList } from './list.mjs';

const ROOT = new URL('.', import.meta.url).pathname;
/* THE LIST IS A PARAMETER (`SITES`), because a corpus that cannot be FROZEN cannot carry a before/after.
   This read `sites.tsv` and nothing else, so the twelve app pages the census actually walks had no way to be
   mirrored at all — and CLAUDE.md §Testing puts a before/after on frozen bytes, "where the only thing that
   changed is the engine". A list you can drive live but not freeze is a corpus you can only ever measure
   once. The mirror TREE and `provenance.json` stay one shared store keyed by id: a list is a SELECTION of
   sites, not a second corpus, so freezing one list leaves every other list's rows untouched (`put` merges). */
const rows = siteList().rows.map(r => [r.id, r.url, r.stack]);
const only = process.argv[2];

/* THE HEADER DUMP IS PER PROCESS AND IS REMOVED BEFORE EVERY FETCH, because it is SHARED STATE with a name
   any second mirror would also have chosen, and what it carries is not scratch: the content type and the
   policy read out of it are WRITTEN INTO provenance.json and frozen there. A fixed path under the world's
   temp directory means two mirrors running at once read each other's headers, and the corruption is silent
   and permanent — a row keeps another site's policy for ever, and the solver half reads a mirrored policy to
   decide whether a breakout it found is real, so a borrowed one makes a dead vector report as live or hides
   a live one. Naming it after the PROCESS is what makes the collision impossible rather than unlikely.
   AND IT IS UNLINKED BEFORE EACH CALL rather than merely truncated by the next writer, which closes the
   second staleness inside ONE run: curl writes this file only when it gets a response, so a transfer that
   fails after the request went out would otherwise leave the PREVIOUS url's block on disk for this url's
   read to find. Removing it first makes that read throw, which the catch below turns into the empty string —
   the honest "no headers observed" — instead of a neighbouring row's answer wearing this row's name. */
const HDR = join(tmpdir(), `.mirror-headers-${process.pid}.txt`);

function get(url) {
  const hdr = HDR;
  rmSync(hdr, { force: true });
  let body;
  try {
    /* `--compressed` IS WHAT A BROWSER DOES AND WITHOUT IT THIS STORED BYTES NO PARSER CAN READ. curl was
       sending no `Accept-Encoding` and decoding nothing, while the UA above says Chrome — and a CDN that
       gzips on that basis then handed back gzip which was written to disk verbatim. MEASURED: 15 `.js`
       files under mirror/ began with the gzip magic `1f 8b` (slack 14, vscodedev 1), `file(1)` naming one
       outright as "gzip compressed data … original size modulo 2^32 1106702".
       WHAT IT COST WAS A FALSE ENGINE ABORT, which is worse than a missing fixture: serve-faithful handed
       those bytes over as JavaScript, the compile refused at byte one, and the census row read
       ENGINE-ABORT at engine/host/solver/engine.c:8976 — the SAME site as a real module-vs-classic
       failure — so a census counting abort SITES would have scored it as a second instance of a compile
       ceiling it has nothing to do with. The discriminator is the TOKEN: `unsupported keyword: export` is
       an engine gap, `unexpected token in expression: '\x1f'` is this defect wearing its clothes.
       THE ENGINE WAS RIGHT AND THE FIXTURE WAS WRONG, which is the direction that costs the most to
       diagnose, because everything downstream behaves exactly as it should. */
    body = execFileSync('curl', ['-sS', '-L', '--compressed', '--max-time', '40', '-D', hdr,
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
  /* AND A BODY THAT IS STILL COMPRESSED IS REFUSED RATHER THAN STORED, which is the difference between this
     defect being impossible and being merely fixed. `--compressed` above asks curl to decode, and a body that
     arrives with the gzip magic anyway is one curl could not decode — an encoding it did not negotiate, or a
     server ignoring the negotiation. Storing it writes bytes that are neither what the page was served
     semantically nor anything a parser can read, and the only thing downstream can do with them is
     manufacture a compile failure at byte one. A resource that could not be decoded is a resource this
     mirror does not have, and saying so is the honest record — an absent fixture is a fixture server 404,
     which `serve-faithful` already reports loudly and keeps a count of, while a corrupt one is an engine
     abort nobody can attribute. */
  if (buf.length >= 2 && buf[0] === 0x1f && buf[1] === 0x8b)
    return { err: 'body is still gzip-compressed after --compressed (magic 1f 8b, ' + buf.length + ' B) — '
                + 'curl could not decode what this server sent, so the bytes are unreadable to any parser '
                + 'and are NOT stored; a fixture 404 is reported and counted, a corrupt fixture is not' };
  let ct = '', csp = '';
  try {
    const h = readFileSync(hdr, 'utf8');
    const all = [...h.matchAll(/^content-type:\s*(.*)$/gim)];
    ct = all.length ? all[all.length - 1][1].trim() : '';
    /* THE FINAL RESPONSE'S BLOCK AND NOT THE WHOLE DUMP. `-L` writes one header block per hop, and a proxied
       run writes the tunnel's `200 Connection Established` block ahead of all of them, so a match taken over
       the dump is a match against whichever hop happened to state a policy — and a redirect's CSP governs the
       redirect, never the document that was finally served. The `content-type` read above is deliberately NOT
       moved onto this block: it answers correctly today for every mirrored row (checked against the recorded
       `contentType`), and narrowing it would change a value already frozen in provenance.json for a reason
       that has nothing to do with this field. */
    const hops = h.split(/^HTTP\/\S+ /m);
    /* Fetch §2.2.2 "get a header name name from a header list list": the values of EVERY header with this
       name, in order, joined by 0x2C 0x20 — which core/fetch/headers.h implements with that exact join and
       which for CSP §2.2 "Policies" is precisely a policy LIST, U+002C being its delimiter. So two
       `Content-Security-Policy` headers stay TWO INDEPENDENTLY ENFORCED POLICIES rather than collapsing to
       the last one, which is the direction that matters: a page narrowed by a second policy would otherwise
       replay as a page that never sent it.
       `-Report-Only` IS EXCLUDED BY THE ANCHOR, deliberately and not incidentally: core/frame/
       navigation_params.c does not read it either ("every policy this engine parses has disposition
       ENFORCE"), so capturing it would put a policy into the record that nothing enforces and that a reader
       would take for one that does. */
    csp = [...hops[hops.length - 1].matchAll(/^content-security-policy:[ \t]*(.*)$/gim)]
      .map(m => m[1].trim()).join(', ');
  } catch { }
  return { buf, final, code, ct, csp };
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
  /* THE DOCUMENT'S POLICY, AND ONLY THE DOCUMENT'S. HTML §7.1.7 "Policy containers" gives a policy container
     to a DOCUMENT; a subresource's own `Content-Security-Policy` governs nothing about the page that loaded
     it, so a `csp` on a `resources` entry would be a field whose only possible reading is the wrong one.
     NAMED RESIDUAL — this is CAPTURED and not yet REPLAYED. serve-faithful.mjs serves the document with
     `content-type` alone, so a mirrored run still reaches the engine with no policy and every @S finding on
     it is judged under "no CSP" — which by CLAUDE.md §A-SHAPE-STATES-TWO-FACTS is read as the positive
     statement that the policy allowed the vector. Replaying it is NOT a one-line `writeHead` addition and
     that is why it is not here: this server rewrites every script's ORIGIN (`/_m/<host><path>`), and a
     policy's source expressions name origins too, so a verbatim replay judges the rewritten scripts against
     the ORIGINAL hosts — for a `script-src` that names hosts without `'self'`, every mirrored script is then
     blocked and the fixture runs no bundle at all. The next diff is the source-expression rewrite that
     `localize()` already performs for URLs, applied to the policy's host-sources. HOW ITS ABSENCE SHOWS: a
     mirrored document whose captured `csp` is non-empty produces @S findings carrying no `cspBlocks`, so the
     record and the report disagree about the same response in the same run. */
  const rec = {
    id, stack, requestedUrl: url, finalUrl: doc.final, status: doc.code,
    contentType: doc.ct, csp: doc.csp, bytes: doc.buf.length, sha256: sha(doc.buf), fetchedAt: now(),
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
