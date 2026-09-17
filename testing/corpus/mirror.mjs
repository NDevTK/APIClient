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
import { execFileSync, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { dirname, join } from 'node:path';
import { readFileSync, rmSync, readdirSync, rmdirSync, existsSync, openSync, renameSync } from 'node:fs';
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
/* WRITTEN THROUGH A RENAME, because this file is READ BY EVERY CONCURRENT `serve-faithful.mjs` and a
   `writeFileSync` over 180 KiB of JSON is not one operation. A reader that lands mid-write gets a truncated
   document, `JSON.parse` throws, and that row's fixture never starts -- which presents as a site that would
   not load rather than as a corpus file that was half written, so nothing points at the cause. It mattered
   little while this was written ONCE at the end of a run; `--runtime` writes it once per ROUND, so the
   window stopped being theoretical. `rename(2)` within one directory is atomic, so a reader sees the old
   document or the new one and never a part of either. */
const writeManifest = () => {
  const dst = join(ROOT, 'provenance.json');
  const tmp = dst + '.' + process.pid + '.tmp';
  writeFileSync(tmp, JSON.stringify(manifest, null, 1));
  renameSync(tmp, dst);
};
const put = (rec) => { const i = manifest.findIndex(r => r.id === rec.id); if (i >= 0) manifest[i] = rec; else manifest.push(rec); };
/* `--runtime` IS A DIFFERENT CAPTURE AND NOT A SECOND WAY OF DOING THIS ONE, so nothing here selects
   between two implementations of one job: the loop below reads what the document's MARKUP names, and the
   capture at the foot of this file reads what the page FETCHES, and those two populations are disjoint by
   construction -- a lazy chunk is precisely an address the markup does not name. Apply CLAUDE.md
   §C-stack's own test: delete the markup loop and the runtime capture still needs a captured DOCUMENT to
   replay, so this is routing and not a fallback. It never fetches a document itself, which is what keeps a
   frozen row frozen. */
if (process.argv.includes('--runtime')) {
  if (!only || only.startsWith('--')) {
    console.error('usage: node mirror.mjs <id> --runtime [--rounds N] [--dwell MS]');
    process.exit(2);
  }
  const numArg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? Number(process.argv[i + 1]) : d; };
  await runtimeCapture(only, numArg('--rounds', 6), numArg('--dwell', 8000));
  process.exit(0);
}

for (const [id, url, stack] of rows) {
  if (only && only !== id) continue;
  const dir = join(ROOT, 'mirror', id);
  mkdirSync(dir, { recursive: true });
  /* THE ROW AS IT STANDS, read BEFORE `put` replaces it, because a runtime capture's resources live on it
     and the carry-forward below has to decide whether they are still about this document. */
  const prev = manifest.find(r => r.id === id);
  const doc = get(url);
  if (doc.err || !doc.buf) { put({ id, stack, requestedUrl: url, error: doc.err || 'no body', fetchedAt: now() }); console.log(id, 'FAILED', doc.err); continue; }
  const html = doc.buf.toString('utf8');
  writeFileSync(join(dir, 'index.html'), doc.buf);
  /* EVERY FILE THIS CAPTURE WRITES, so the prune below can tell them from a PREVIOUS capture's. */
  const written = new Set([join(dir, 'index.html')]);
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
    written.add(f);
    rec.resources.push({ url: abs, status: r.code, contentType: r.ct, bytes: r.buf.length, sha256: sha(r.buf), fetchedAt: now(), path: rel });
    n++;
  }
  /* A RUNTIME CAPTURE IS CARRIED FORWARD ONLY WHILE THE DOCUMENT IT WAS COMPOSED FROM IS STILL THE DOCUMENT
     ON DISK, and is DROPPED LOUDLY otherwise. `--runtime` stores addresses that exist because THESE bytes
     ran, so it is pinned to this document's hash and to nothing else: an unchanged hash means every one of
     them is still an address this page composes, and a changed hash means they are facts about a page this
     mirror no longer holds -- a content-hashed chunk name from the previous build, which the new document
     never mentions and which the prune below would otherwise leave on disk with nothing vouching for it.
     THE DROP IS COUNTED IN THE ROW'S LINE rather than left to be noticed, because a capture that costs a
     browser and several rounds must not disappear in silence. Without this block the prune ate them on the
     next ordinary re-mirror: they are not in `written`, the record is rebuilt empty, and the loud 404 comes
     back with no line anywhere saying a capture had been thrown away. */
  const prevRuntime = (prev && prev.resources || []).filter(x => x.via === 'runtime');
  const sameDoc = !!(prev && prev.sha256 === rec.sha256);
  let carried = 0;
  if (sameDoc) for (const x of prevRuntime) {
    if (rec.resources.some(y => y.url === x.url)) continue;
    rec.resources.push(x);
    if (x.path) written.add(join(dir, x.path.replace(/[^A-Za-z0-9._/@%+-]/g, '_')));
    carried++;
  }
  if (sameDoc && prev.runtimeCapture) rec.runtimeCapture = prev.runtimeCapture;

  /* THE ROW'S DIRECTORY HOLDS EXACTLY WHAT THIS CAPTURE WROTE, AND A LEFTOVER FILE IS NOT INERT -- IT IS
     SERVED. serve-faithful.mjs resolves a request BY DISK PATH and reads the record only for the status and
     the type, saying so in its own words: `A status is only ever read from the record; a resource that is on
     disk with NO recorded status is an older capture and keeps the 200 it has always been served with.` So a
     file no manifest entry vouches for comes back to the engine as 200 `application/javascript`, which is the
     manufactured-engine-defect shape this file's header is otherwise entirely about, arriving through what
     the builder FAILED TO REMOVE rather than through what it wrote.
     MEASURED, AND IT IS `--compressed` ABOVE THAT PRODUCED IT: asking for an encoding changes which DOCUMENT
     the origin serves, and a document that may name per-encoding resource paths then names DIFFERENT ONES.
     slack's names the brotli-variant CDN directory `a.slack-edge.com/bv1-13-br/...` where the previous
     capture's named `a.slack-edge.com/bv1-13/...` -- 21 references before, 22 after, NOT ONE PATH IN COMMON.
     So the re-mirror that fixed 14 gzip bodies wrote 14 correct files BESIDE the 14 corrupt ones and left
     every one of them reachable: the gzip derivation over the mirror still read 14 after a run that printed
     `17/17 scripts` and refused nothing. THE CAPTURE WAS RIGHT AND THE DIRECTORY WAS THE UNION OF TWO OF THEM,
     which no count of what the run fetched can show, because every number that run printed was true.
     PRUNED AFTER, NEVER CLEARED BEFORE. The document fetch above `continue`s without writing anything, so a
     row whose site is briefly unreachable keeps the capture it already has; a clear-first would empty it on
     the strength of a run that got nothing, which is the destructive direction and the irreversible one. A
     resource that failed THIS time is recorded with an `error` and no `path`, so its stale bytes go with the
     rest -- the record says this mirror does not have it, and disk must not be left disagreeing, because a
     fixture 404 is a fact serve-faithful reports and counts while a stale 200 is an abort nobody can
     attribute. The scope is this row's own directory and nothing above it. */
  const sweep = (d, files = [], dirs = []) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const q = join(d, e.name);
      if (e.isDirectory()) { dirs.push(q); sweep(q, files, dirs); } else files.push(q);
    }
    return { files, dirs };
  };
  const { files: onDisk, dirs: subDirs } = sweep(dir);
  let pruned = 0;
  for (const f of onDisk) if (!written.has(f)) { rmSync(f); pruned++; }
  /* Deepest first, so a directory emptied by the prune is gone before its parent is tried. `rmdirSync`
     refuses a non-empty one, which is exactly the test wanted and is why nothing here counts entries. */
  for (const d of subDirs.sort((a, b) => b.length - a.length)) { try { rmdirSync(d); } catch { } }
  put(rec);
  console.log(`${id}\t${doc.code}\t${(doc.buf.length / 1024).toFixed(0)}KiB doc\t${n}/${srcs.size} scripts\t${(rec.resources.reduce((a, x) => a + (x.bytes || 0), 0) / 1024).toFixed(0)}KiB js\t${pruned} stale pruned`);
  if (prevRuntime.length) console.log(`  runtime capture: ${carried} carried`
    + (sameDoc ? '' : `, ${prevRuntime.length} DROPPED — the document's sha256 changed, so every address they`
      + ` were composed from belongs to a build this mirror no longer holds; re-run \`--runtime\``));
}
writeManifest();
console.log('wrote provenance.json (' + manifest.length + ' sites)');

/* ============================================================================
   `node mirror.mjs <id> --runtime` — CAPTURE WHAT THE PAGE FETCHES, WHICH IS A
   STRICTLY LARGER SET THAN WHAT ITS MARKUP NAMES, AND FOR SOME ROWS IS THE ONLY
   SET THERE IS.
   The loop above reads the document's `<script src>` and its preload links. A
   LAZY CHUNK is BY DEFINITION an address the markup does not name -- the whole
   point of code splitting is that the runtime composes it -- so no capture that
   reads markup can ever hold one, at any revision, for any site. That is not a
   gap in one freeze; it is the shape of the instrument. MEASURED ON THE TREE
   THIS CAPTURE WAS WRITTEN AGAINST, and stated in the past tense because this
   diff is what makes it false: `find mirror/squoosh -type f` answered ONE file,
   `index.html`, because squoosh's document carries a single inline `<script>`
   with no `src` at all and every byte of the application is reached from it by
   `import()`. Its fixture answered `MISS` 1125 times in one census run, 431 of
   them for one chunk. WHAT IS DURABLE IS THE SHAPE AND NOT THE ROW -- a document
   whose only script has no `src` is invisible to a markup reader, and the number
   of rows in that state is a `find` any reader can run today.
   WHAT THAT COSTS IS THE HEADLINE. `reach.mjs` partitions each learned address
   into markup / in-JS / composed, and an address named INSIDE a chunk can only
   be learned if the chunk's BODY is on disk -- a 404 with an empty body teaches
   the run nothing. So the COMPOSED class is bounded above by what the fixture
   can serve, and for exactly the surface CLAUDE.md §What-the-tool-produces calls
   the headline (lazy-chunk, dead-but-shipped) that bound was ZERO. A number read
   off that partition was a fact about this builder.

   THE ADDRESSES COME FROM THE PAGE'S OWN RUNTIME AND NEVER FROM A PATTERN.
   Nothing here knows what a bundler is: `serve-faithful.mjs` replays the FROZEN
   document, real Chrome runs it, and every address it composes that the fixture
   does not hold is printed by that server as `MISS`. That is CLAUDE.md
   §RUN-DON'T-MATCH applied to the corpus builder -- a chunk-map reader would be
   a bundler recognizer, would be wrong for the next bundler, and would be a
   second implementation of the thing the engine is being measured on.

   THE BODIES COME FROM THE LIVE ORIGIN AND THE DOCUMENT IS NOT RE-FETCHED, which
   is what keeps the baseline. A live load would capture the build the site ships
   TODAY, whose chunk names the frozen document never mentions -- measured while
   writing this, squoosh's live document is 19359 B against the frozen 75215 B,
   a different build entirely. What makes the split possible is that a chunk URL
   carries a CONTENT HASH, so it is immutable by construction: all four chunk
   addresses the frozen squoosh document names still answered 200 from the live
   origin. Where a row's origin has since dropped them, the fetch fails, the
   record says so, and the chunk stays unservable -- which is the honest floor
   and is reported rather than hidden.

   IT IS A FIXPOINT, AND THAT IS NOT A CONVENIENCE -- IT IS THE ANSWER TO "A
   CAPTURE RECORDED AGAINST A BROKEN FIXTURE CAPTURES THE BROKENNESS". Round one
   drives a page whose first chunk 404s, so the page stops there and composes
   nothing further; round two drives the same page with that chunk present, gets
   further, and composes the next layer. Each round the fixture is LESS broken.
   The loop ends when a round stores nothing new, so the stopping condition is
   CONVERGENCE and not a count; `--rounds` is a safety maximum that is REPORTED
   when it is reached, so a truncated capture can never read as a converged one.

   AND IT CAN MAKE `reach.mjs`'s `composed` COLUMN FALL, WHICH IS THE
   CLASSIFIER GETTING MORE CORRECT AND NOT THE ENGINE GETTING WORSE. That
   partition is markup / in-JS / COMPOSED, and `composed` is defined as an
   address in NEITHER -- so it is a residue, and it holds two different things
   while this mirror is thin: an address the engine really did have to execute
   code to build, and one that is simply written inside a chunk the mirror does
   not have. Every body this capture adds moves members of the second kind into
   `inJs`, so a run whose engine did exactly what it did before can report a
   SMALLER `composed` afterwards. It is the falling-finding-count shape CLAUDE.md
   names: a number that drops both when a defect is fixed and when the instrument
   starts seeing, and here it is the second. THE FIGURE TO READ BESIDE IT IS THE
   MIRRORED JS CORPUS -- `reach.mjs` already prints `shipped JS` files and KiB on
   the same row -- and a `composed` compared across two revisions whose JS corpus
   differs is not a comparison. Nothing here is a reason to keep the mirror thin:
   an address that is in a chunk IS in the JS, and a partition that called it
   composed was wrong about it.

   IT IS MONOTONE, WHICH IS WHY A THIN CAPTURE IS SAFE TO RE-RUN AND NEVER A
   REASON TO WAIT FOR A QUIET MACHINE. A round's reach depends on how far the
   page gets inside `networkidle2` plus the dwell, so a loaded box composes fewer
   addresses and converges on a smaller set -- every capture is a FLOOR. Nothing
   here prunes and every resource merges into the row by url, so a second run can
   only add, and a capture taken under load is improved by re-running it rather
   than invalidated.

   NAMED RESIDUAL -- WHAT THIS DOES NOT REACH IS AN ADDRESS THE PAGE COMPOSES
   ONLY ON A BRANCH A BROWSER DOES NOT TAKE. The address source here is real
   Chrome performing an ordinary LOAD: no click, no route change, no file drop,
   and -- the part that matters for this project -- no FORCING. CLAUDE.md
   §Solver-half's whole subject is the arm a real run never takes, so the engine
   composes addresses this capture is structurally unable to see, and it is the
   gated/admin/gate-flag surface rather than a marginal one.
   WHAT THE NEXT DIFF BUILDS: the same fixpoint with the ENGINE as the address
   source instead of the browser. Both halves already exist and are already
   paired -- `run.sh` starts `serve-faithful.mjs` with its output at
   `logs/<id>.serve` and drives `site.mjs` against it in the same block -- so
   what is missing is the step that reads that MISS log back into this capture
   and re-freezes, not a new instrument.
   HOW ITS ABSENCE WOULD SHOW: a census's `logs/<id>.serve` holds a script-shaped
   MISS for a row whose capture CONVERGED -- an address a forced run asked for
   and no round of an ordinary load ever did.

   AND THE INVERSION BELOW IS CHECKED BY THE LOOP RATHER THAN TRUSTED. Turning a
   fixture request path back into the URL it stood for is the same knowledge
   serve-faithful.mjs uses to resolve one, written out a second time here -- and
   a second copy is exactly what this project distrusts. What makes it safe is
   that a WRONG inversion cannot survive: the next round replays the same page,
   the same path MISSes again, and the round stores nothing, so the run reports
   no progress instead of reporting a capture. The two must still move together;
   if serve-faithful's candidate list changes, this one does. */
function missToCandidates(reqPath, BASEURL, DOCURL) {
  const p = new URL(reqPath, 'http://x');
  const out = [];
  const push = (f) => { try { out.push(f().href); } catch { } };
  if (p.pathname.startsWith('/_m/')) {
    push(() => new URL('https://' + p.pathname.slice(4) + p.search));
  } else {
    push(() => new URL(p.pathname + p.search, new URL(BASEURL).origin));
    push(() => new URL(p.pathname.replace(/^\/+/, '') + p.search, BASEURL));
    push(() => new URL(p.pathname + p.search, new URL(DOCURL).origin));
  }
  return [...new Set(out)];
}

/* WHAT IS WORTH BYTES IS DECIDED BY WHAT THE SERVER SAID, NEVER BY THE URL. The
   mirror is TRACKED, so every stored byte is a byte in every clone for ever, and
   a corpus builder that stores a site's photography buys nothing an engine can
   read. A URL SUFFIX is the wrong discriminator and CLAUDE.md says so by name
   (§Static assets are NEVER endpoints -- "magic-byte + content-type, not URL
   suffix"), so the fetch happens first and the `Content-Type` decides. An
   address whose reply is not one of these is RECORDED with its status, its type
   and its length and its bytes are NOT stored: the record then says this mirror
   SAW it and declined it, which is a different fact from never having looked,
   and the fixture goes on answering its honest 404. */
/* A DECLARATION AND NOT A `const`, because the dispatch that calls into this section runs ABOVE it and a
   `const` here is in its temporal dead zone at that point -- which throws at the first resource of the
   first round, after a browser and a server have already been started. */
function runtimeStorable(ct) { return /javascript|ecmascript|json|\/css|\/html|\/xml/i.test(ct || ''); }
/* `text/plain` IS NOT IN THIS SET, AND THE REASON IS A MEASUREMENT RATHER THAN A PREFERENCE. It was, on the
   argument that a CDN sometimes serves JavaScript that way -- which is true in general and buys NOTHING
   here: across the 445 resources the markup pass had already frozen, the content types are 233
   `text/javascript`, 166 `application/javascript`, 16 `font/woff2`, 15 `image/svg+xml`, 13 `text/css`, one
   `application/json` and one `text/html`, and `text/plain` is ZERO of them
   (`node -e` over provenance.json, counting `contentType` before the `;`). What it DID catch on its first
   run was a 165 KiB TrueType font that astexplorer's origin labels `text/plain`, which is 165 KiB in every
   clone of this repository for ever and which no engine can read. A widening is priced in what it takes
   with it, and this one took only that.
   THE COST OF BEING WRONG IS ONE DECLINED CHUNK AND IT IS VISIBLE: a script a server really does label
   `text/plain` is recorded with its status, its type and its length under `declined`, and the fixture goes
   on answering the honest 404 it answers today -- so the failure direction is the one that changes
   nothing, and it leaves a row saying which type was refused. */

async function runtimeCapture(id, maxRounds, dwellMs) {
  const rec0 = manifest.find((r) => r.id === id);
  if (!rec0 || rec0.error) {
    console.error(`no mirrored document for \`${id}\` — capture it first:  node mirror.mjs ${id}`);
    process.exit(2);
  }
  const dir = join(ROOT, 'mirror', id);
  if (!existsSync(join(dir, 'index.html'))) {
    console.error(`provenance.json has \`${id}\` but mirror/${id}/index.html is not on disk`);
    process.exit(2);
  }
  const DOCURL = rec0.finalUrl || rec0.requestedUrl;
  const BASEURL = rec0.baseUrl || DOCURL;

  /* A PRIVATE PORT, PROFILE AND LOG, because this checkout runs several agents at
     once and each of them has a Chrome and a fixture server. CLAUDE.md's own
     scratch-collision rule names the failure: a name any second lane would also
     have chosen is shared state, and here it would be a browser answering
     another lane's debugger and a server serving another lane's row. */
  const tag = process.env.MIRROR_TAG || ('rt' + process.pid);
  const PORT = Number(process.env.MIRROR_PORT || 8930);
  const CDP = Number(process.env.MIRROR_CDP || 9481);
  const PROF = process.env.MIRROR_PROFILE || `/tmp/mirror-${tag}/prof`;
  const CHROME = process.env.MIRROR_CHROME || '/opt/chrome-for-testing/chrome';

  /* THE SANDBOX STAYS ON AND ROOT IS WHAT GETS FIXED, which is testing/harness.js's
     rule and its reasoning transfers verbatim: the documents this drives are the
     attacker-controlled pages the tool exists to analyse, and a frozen copy of one
     is no safer than the original. Chrome refuses to run as root with the sandbox
     on, so the privileges are dropped to the same unprivileged account the harness
     uses rather than the sandbox being turned off. */
  let uid, gid;
  if (typeof process.getuid === 'function' && process.getuid() === 0) {
    const who = process.env.HARNESS_USER || 'chromerun';
    try {
      uid = Number(execFileSync('id', ['-u', who]).toString().trim());
      gid = Number(execFileSync('id', ['-g', who]).toString().trim());
    } catch {
      console.error(`running as root and the unprivileged account \`${who}\` does not exist (useradd -m ${who})`);
      process.exit(2);
    }
  }
  rmSync(PROF, { recursive: true, force: true });
  mkdirSync(PROF, { recursive: true });
  if (uid !== undefined) execFileSync('chown', ['-R', `${uid}:${gid}`, PROF]);

  /* The container states its egress path in the environment and this does not
     invent one — the same line harness.js takes, so the browser that DISCOVERS an
     address has the same reach as the browser a census drives. TLS is capped for
     the same measured reason stated there: Chrome's post-quantum key share makes
     a hello this interceptor drops. The capped leg is Chrome to a loopback process
     that is already reading everything in the clear. */
  const proxy = process.env.HARNESS_NO_PROXY === '1' ? null
    : (process.env.HARNESS_PROXY || process.env.HTTPS_PROXY || process.env.https_proxy || null);
  const chromeLog = join(dirname(PROF), 'chrome.out');
  const chromeFd = openSync(chromeLog, 'w');
  const chrome = spawn(CHROME, [
    `--remote-debugging-port=${CDP}`,
    `--user-data-dir=${PROF}`,
    '--no-first-run', '--no-default-browser-check', '--headless=new',
    ...(proxy ? [`--proxy-server=${proxy}`, '--disable-quic', '--ssl-version-max=tls1.2'] : []),
  ], { detached: true, stdio: ['ignore', chromeFd, chromeFd], ...(uid !== undefined ? { uid, gid } : {}) });
  chrome.unref();
  console.log(`chrome pid ${chrome.pid} on ${CDP}, profile ${PROF}${proxy ? `, egress via ${proxy}` : ''}`);

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  /* WAITED FOR BY ASKING THE PORT, never by sleeping a guess — the condition a
     sleep would be guessing at is exactly "is the debugger answering". */
  let ver = null;
  for (let i = 0; i < 120 && !ver; i++) {
    try { ver = await fetch(`http://127.0.0.1:${CDP}/json/version`).then((r) => r.json()); } catch { await sleep(250); }
  }
  const puppeteer = (await import('puppeteer')).default;

  const attempted = new Map();   // original url -> record (or null while in flight)
  const seenMiss = new Set();    // fixture request paths already inverted
  const fresh = [];              // records added by this run
  /* COUNTED IN THE BODY AND NOT READ OFF THE LOOP VARIABLE. `for (r = 1; r <= max; r++)` leaves `r` at
     max + 1 when the maximum is what ended it, so a run of three rounds reported four -- a count that
     cannot be true, in the one line a reader quotes. */
  let rounds = 0, truncated = false;

  try {
    if (!ver) throw new Error(`chrome did not open ${CDP} — see ${chromeLog}`);
    const browser = await puppeteer.connect({ browserURL: `http://127.0.0.1:${CDP}`, protocolTimeout: 180000 });
    for (let n = 1; n <= maxRounds; n++) {
      rounds = n;
      /* THE SERVER IS RESTARTED EACH ROUND rather than left up, because it reads
         the manifest ONCE at startup to learn each resource's status and type.
         A file written mid-round is served by disk path with the default 200
         `application/javascript` — which is the "resource on disk with no
         recorded status" case serve-faithful.mjs names in its own words — so a
         round that did not restart would replay this round's captures under a
         type their origin never stated. */
      writeManifest();
      const missed = [];
      const serve = spawn(process.execPath, [join(ROOT, 'serve-faithful.mjs'), id, String(PORT)],
        { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
      serve.stderr.on('data', (b) => {
        for (const line of b.toString().split('\n')) if (line.startsWith('MISS ')) missed.push(line.slice(5).trim());
      });
      serve.stdout.on('data', () => { });
      for (let i = 0; i < 60; i++) { try { await fetch(`http://127.0.0.1:${PORT}/`); break; } catch { await sleep(100); } }

      /* A FRESH BROWSER CONTEXT PER ROUND. squoosh registers a service worker, and
         a SW installed in round one would answer round two out of ITS cache — so
         the second round would replay the first round's brokenness and the loop
         would converge on a lie. A separate context has its own storage, its own
         SW registrations and its own HTTP cache. */
      const ctx = await (browser.createBrowserContext ? browser.createBrowserContext() : browser.createIncognitoBrowserContext());
      const page = await ctx.newPage();
      await page.setCacheEnabled(false);
      let nav = 'ok';
      try { await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: 'networkidle2', timeout: 60000 }); }
      catch (e) { nav = String(e && e.message || e).slice(0, 90); }
      await sleep(dwellMs);
      await page.close().catch(() => { });
      await ctx.close().catch(() => { });

      serve.kill('SIGTERM');
      await new Promise((r) => { serve.on('exit', r); setTimeout(r, 4000); });

      let stored = 0, declined = 0, failed = 0, fetched = 0;
      for (const p of missed) {
        if (seenMiss.has(p)) continue;
        seenMiss.add(p);
        const cands = missToCandidates(p, BASEURL, DOCURL);
        let got = null, gotUrl = null;
        for (const abs of cands) {
          if (attempted.has(abs)) { got = 'already'; break; }
          const r = get(abs);
          fetched++;
          if (r.err || !r.buf) continue;
          if (r.code >= 200 && r.code < 300) { got = r; gotUrl = abs; break; }
        }
        if (got === 'already') continue;
        /* AN ADDRESS NO ORIGIN WILL ANSWER IS RECORDED, NOT MERELY PRINTED. This is the honest floor of the
           whole design -- a chunk name carries a CONTENT HASH, so it survives a rebuild only while the build
           does, and a row frozen before the site shipped again names chunks the origin has since dropped.
           MEASURED four days after one freeze: jsoncrack's document still answers 200 and TWELVE of the chunk
           addresses it composes answer 404, so that row's lazy surface is unreachable until the row is
           re-frozen. Leaving that in a console line makes `never tried` and `tried and the origin has dropped
           it` render identically in the record, which is the absent-versus-zero pair CLAUDE.md forbids
           merging: one says re-run the capture, the other says re-freeze the document, and they are the
           opposite instruction. */
        if (!got) {
          failed++;
          const dead = { url: cands[0] || p, via: 'runtime', composedFrom: p, fetchedAt: now(),
            error: `no candidate origin answered 2xx (tried ${cands.length}: ${cands.join(' ')})` };
          attempted.set(cands[0] || p, dead);
          rec0.resources = rec0.resources || [];
          const was = rec0.resources.findIndex((x) => x.url === dead.url);
          if (was >= 0) rec0.resources[was] = dead; else rec0.resources.push(dead);
          fresh.push(dead);
          console.log(`  no origin answered 2xx for ${p}  (tried ${cands.length})`);
          continue;
        }
        const u = new URL(gotUrl);
        let rel = u.host + u.pathname + (u.search ? '__q' + createHash('sha256').update(u.search).digest('hex').slice(0, 8) : '');
        if (rel.endsWith('/')) rel += 'index';
        const entry = {
          url: gotUrl, status: got.code, contentType: got.ct, bytes: got.buf.length,
          sha256: sha(got.buf), fetchedAt: now(), via: 'runtime', composedFrom: p,
        };
        /* A REPLY WHOSE TYPE CONTRADICTS THE ADDRESS IS A SOFT 404 AND IS NOT STORED. This file's own header
           records the incident: material.angular.dev answered 21376 bytes of `<!doctype html><title>Page Not
           Found</title>` for addresses it had never published, those bytes were saved under `.js` names, and
           the engine did the only correct thing with them and aborted on `'<'` -- "that abort was about this
           builder". The capture here is MORE exposed to it than the markup pass, because the address it asks
           for was composed by a page running against a fixture and the origin is free to answer a SPA's
           catch-all route with 200 text/html rather than a 404.
           IT IS A CONTRADICTION AND NOT A CLASSIFIER, which is why a suffix appears here at all: nothing
           decides what a resource IS from its URL -- the type is the server's word, as everywhere else in
           this file -- and this asks only whether the server's word DISAGREES with the address the page
           composed. The failure direction is the harmless one: a real HTML document at a `.js` address is
           declined, the fixture answers its honest 404, and the record names the type that was refused. */
        /* THE DOCUMENT IS NEVER RE-FETCHED HERE, AND THAT IS THE WHOLE REASON THIS CAPTURE IS SAFE TO RUN
           AGAINST A FROZEN ROW. A page that references its own address -- a self-link the fixture rewrites
           to `/_m/<host>/`, a navigation preload, a manifest `start_url` -- produces a MISS whose candidate
           is the row's OWN document URL, and fetching that returns the build the site ships TODAY. Storing
           it would put a SECOND, NEWER document in the tree beside the frozen one, served at `/_m/<host>/`
           while `/` serves the frozen bytes: two documents for one row, disagreeing, with every chunk name
           in the newer one naming a build the mirror does not hold. MEASURED before this guard: 342144
           bytes of the live vscode.dev document were stored that way.
           A DOCUMENT IS RE-CAPTURED BY THE MARKUP PASS, DELIBERATELY AND ON ITS OWN, because that pass
           re-computes the row's hash and the carry-forward above then decides what the old capture is still
           about. Doing it here would change the row's document without changing its record. */
        const isOwnDocument = gotUrl === (rec0.finalUrl || '') || gotUrl === (rec0.requestedUrl || '');
        const softFail = /\.(?:m?js|css|json)$/i.test(new URL(gotUrl).pathname) && /\/html/i.test(got.ct || '');
        if (isOwnDocument) {
          entry.declined = 'this is the row\'s own document URL — re-fetching it here would store a NEWER '
            + 'build beside the frozen one; a document is only ever captured by the markup pass';
          declined++;
        } else if (softFail) {
          entry.declined = `the address ends .js/.css/.json and the origin answered ${got.ct} — a soft 404, `
            + 'not stored; storing it would put an error page under a real URL\'s name';
          declined++;
        } else if (runtimeStorable(got.ct)) {
          const f = join(dir, rel.replace(/[^A-Za-z0-9._/@%+-]/g, '_'));
          /* A PATH THAT IS BOTH A FILE AND A DIRECTORY CANNOT BE BOTH ON DISK, AND THE COLLISION IS RECORDED
             RATHER THAN CRASHED ON. The saved-path rule -- `<host><path>`, with `index` appended only for a
             TRAILING SLASH -- is shared with serve-faithful.mjs, which recomputes it rather than keeping an
             index, so it may not be changed here alone. It has no answer for `/a` and `/a/b` both being
             resources, which the markup pass never met (a `<script src>` ends in a file name) and this one
             does: an API address with no extension is a file, and the page then composes a longer path
             under it. MEASURED: grafana's document reaches `…/grafanacom-api/orgs` and then a path beneath
             it, and the `mkdirSync` threw EEXIST -- which aborted the WHOLE capture and lost every resource
             that row had already stored, so the cost of not handling it was not one chunk but a site.
             ONLY THESE TWO ERRNOS ARE CAUGHT and everything else still propagates: this is not a swallow but
             a NAMED RESIDUAL, because the code is correct for what it does and narrower than the scheme
             needs to be. WHAT THE NEXT DIFF BUILDS: one saved-path rule, changed in mirror.mjs AND
             serve-faithful.mjs in the same commit, under which a path that is also a directory prefix gets a
             reserved leaf (the `index` rule generalised from a trailing slash to any collision). HOW ITS
             ABSENCE WOULD SHOW: a row whose record carries a `declined` naming a path collision, whose
             fixture then answers a loud 404 for an address the real origin served. */
          let wrote = true;
          try {
            mkdirSync(dirname(f), { recursive: true });
            writeFileSync(f, got.buf);
          } catch (e) {
            if (e && (e.code === 'EEXIST' || e.code === 'EISDIR' || e.code === 'ENOTDIR')) {
              wrote = false;
              entry.declined = `saved-path collision (${e.code}) at ${rel} — this scheme cannot hold a path `
                + 'that is both a resource and the prefix of another; not stored, the fixture 404s it';
              declined++;
            } else throw e;
          }
          if (wrote) { entry.path = rel; stored++; }
        } else {
          entry.declined = `content-type ${got.ct || '(none)'} — seen and not stored; the fixture keeps its 404`;
          declined++;
        }
        attempted.set(gotUrl, entry);
        rec0.resources = rec0.resources || [];
        const at = rec0.resources.findIndex((x) => x.url === gotUrl);
        if (at >= 0) rec0.resources[at] = entry; else rec0.resources.push(entry);
        fresh.push(entry);
      }
      console.log(`round ${n}\tnav=${nav}\t${missed.length} MISS (${seenMiss.size} distinct so far)\t`
        + `${fetched} fetched\t${stored} stored\t${declined} declined\t${failed} unresolvable`);
      if (stored === 0) break;
      if (n === maxRounds) truncated = true;
    }
    browser.disconnect();
  } finally {
    try { process.kill(chrome.pid, 'SIGTERM'); } catch { }
  }

  rec0.runtimeCapture = {
    at: now(), rounds, converged: !truncated,
    docSha256: rec0.sha256,
    stored: fresh.filter((e) => e.path).length,
    declined: fresh.filter((e) => e.declined).length,
    unresolvable: fresh.filter((e) => e.error).length,
    storedBytes: fresh.reduce((a, e) => a + (e.path ? e.bytes : 0), 0),
  };
  writeManifest();
  console.log(`\n${id}: ${rec0.runtimeCapture.stored} stored `
    + `(${(rec0.runtimeCapture.storedBytes / 1024).toFixed(0)} KiB), `
    + `${rec0.runtimeCapture.declined} declined, ${rec0.runtimeCapture.unresolvable} unresolvable, ${rounds} round(s), `
    + (truncated ? `NOT CONVERGED — stopped at the --rounds maximum, so this capture is a FLOOR`
      : `converged (a round stored nothing new)`));
  /* THE CAPTURE IS PINNED TO THE DOCUMENT IT WAS COMPOSED FROM. These addresses
     exist because THESE bytes ran; a later re-fetch of the document that changes
     its hash makes every one of them a fact about a page this mirror no longer
     holds, which the curl loop above reads to decide whether to carry them. */
}
