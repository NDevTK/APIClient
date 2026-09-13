// REACH OVER THE ARTIFACT THE APP SHIPPED — what did the run learn that a static tool could not?
//
// §What-the-tool-produces: "A sniffer shows what FIRED; this shows what the bundle CAN do but didn't."
// Nothing measured that. The only number anyone quoted was "endpoints learned", and an endpoint count has
// no denominator: 90 against what? This file supplies the denominator and partitions the numerator, so the
// sentence a reader ends up with is a fraction of the shipped bundle rather than a bare count.
//
// THE PARTITION IS THE POINT AND IT HAS THREE CLASSES, NOT TWO. Every address the run learned is exactly one
// of:
//   MARKUP    the document names it. A plain HTML parser produces this address. No execution required.
//   IN-JS     the document does not name it, but it occurs VERBATIM in the shipped JavaScript. Reaching it
//             required running something, and a `grep` over the bundle would have found it anyway.
//   COMPOSED  it occurs in NEITHER. The address exists only because code BUILT it out of pieces.
// Only COMPOSED is beyond a static tool, so only COMPOSED is evidence for the product's claim. An earlier
// column collapsed IN-JS into COMPOSED; the middle class is what stops a bundled manifest of chunk names
// from reading as forced execution.
//
// COMPOSED IS A FLOOR IN THE SAFE DIRECTION, DELIBERATELY. Both of the other two tests are as GENEROUS as
// they can be made — every attribute value in the document is resolved, and the path is additionally sought
// as a raw substring of the document AND of every shipped script, in both its encoded and decoded spellings.
// A generous MARKUP or IN-JS test can only move an address OUT of COMPOSED, so this file can under-claim
// execution-derived learning and cannot over-claim it. That is the direction that cannot flatter the engine.
//
// WHAT THIS FILE CANNOT SEE IS PRINTED AS ITS OWN VERDICT, never folded into a number: an instrument that
// cannot see something has not found anything. See the BLIND SPOTS section at the end of the output.
//
//   node reach.mjs census-cc-fp-*.jsonl
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = new URL('.', import.meta.url).pathname;
const MIRROR = join(ROOT, 'mirror');

/* THE DOCUMENT BASE IS READ FROM THE PRODUCER'S OWN DECLARATION, NEVER RE-DERIVED HERE. mirror.mjs computes
   it once when it captures a site (HTML §4.2.3 "The base element" applied against the document's final
   address) and writes it to provenance.json as `baseUrl`; serve-faithful.mjs reads that same field to decide
   what the browser resolves against. A third implementation of §4.2.3 in this file would be the second copy
   CLAUDE.md forbids, and it would drift silently in the one direction that matters — a wrong base makes every
   relative specifier resolve somewhere the learned set cannot match, which INFLATES the COMPOSED class.
   A SITE WITH NO `baseUrl` IS FATAL rather than defaulted: `|| finalUrl` would be a plausible answer for a
   document that declares a <base>, and a plausible answer is indistinguishable from a measured one. */
const manifest = JSON.parse(readFileSync(join(ROOT, 'provenance.json'), 'utf8'));

/* AN ATTRIBUTE VALUE IS HTML-ESCAPED AND THE URL IS WHAT IT DECODES TO — the same decode mirror.mjs and
   serve-faithful.mjs each apply before resolving one. */
const unent = (s) => s.replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
  .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
  .replace(/&(lt|gt|quot|apos|amp);/g, (_, n) => ({ lt: '<', gt: '>', quot: '"', apos: "'", amp: '&' }[n]));

const decode = (s) => { try { return decodeURIComponent(s); } catch { return s; } };

/* AN ADDRESS'S IDENTITY IS host + path + query, AND THE HOST IS PART OF IT. The column this file replaces
   keyed on the PATHNAME alone, which erased the origin: a cross-origin image at `//covers.example/b/id/x.jpg`
   and a same-origin `/b/id/x.jpg` became one key, so one learned address could be matched against another
   site's markup and two genuinely distinct endpoints collapsed into one. The QUERY is kept for the reason
   serve-faithful.mjs keeps it — it is program input, and `/_next/image?url=a` and `?url=b` are two addresses.
   THREE SPELLINGS REACH THIS FUNCTION AND ALL THREE ARE THE SAME KIND OF FACT:
     `/_m/<host>/<path>`  the fixture's rewritten form for a resource served from the mirror;
     `127.0.0.1/<path>`   a same-origin request the page made at RUNTIME, which the fixture never rewrote, so
                          its real address is that path on the document's own origin;
     anything else        an absolute the page reached for directly, kept as it stands. */
function addrKey(raw, baseOrigin) {
  let u;
  try { u = new URL(String(raw).replace(/^[A-Z]+ /, '')); } catch { return null; }
  if (u.pathname.startsWith('/_m/')) {
    try { u = new URL('https://' + u.pathname.slice(4) + u.search); } catch { }
  } else if (/^(127\.0\.0\.1|localhost|\[::1\])$/.test(u.hostname)) {
    try { u = new URL(u.pathname + u.search, baseOrigin); } catch { }
  }
  return u.host + u.pathname + u.search;
}
const pathOf = (key) => key.slice(key.indexOf('/'));

/* EVERY URL THE DOCUMENT NAMES, WITH NO ATTRIBUTE ALLOWLIST. A list of attribute names is a list that drifts,
   and it has already cost this measurement twice: reading only `src`/`href` missed `srcset` entirely, and the
   case-sensitive spelling `srcset` missed React's serialized `srcSet`. Both misses INFLATE the COMPOSED class,
   which is the one direction a reader must never be misled in. So every quoted attribute value in the document
   is resolved against the base and every comma-separated candidate inside it is taken (an `srcset` is a list).
   RESOLVING A NON-URL IS HARMLESS AND IS WHY THERE IS NO FILTER: `class="a b"` resolves to a key no learned
   address can equal. The cost of over-reading is a junk entry; the cost of under-reading is a false finding. */
function namedByMarkup(html, base) {
  const named = new Set();
  for (const m of html.matchAll(/\s[-\w:.]+\s*=\s*("([^"]*)"|'([^']*)')/g)) {
    const v = unent(m[2] ?? m[3] ?? '');
    for (const cand of v.split(',').map((x) => x.trim().split(/\s+/)[0]).filter(Boolean)) {
      try { const u = new URL(cand, base); named.add(u.host + u.pathname + u.search); } catch { }
    }
  }
  return named;
}

/* THE SHIPPED JAVASCRIPT, SELECTED BY THE CONTENT TYPE THE SERVER STATED AND NEVER BY FILE EXTENSION. A
   bundler is free to serve a program from a path ending `.json` or from no extension at all, and the capture
   already recorded what each response declared itself to be. */
function shippedJs(site) {
  const dir = join(MIRROR, site.id);
  let text = '', files = 0, bytes = 0;
  for (const r of site.resources || []) {
    if (!/javascript|ecmascript/i.test(r.contentType || '')) continue;
    const p = join(dir, r.path || '');
    if (!r.path || !existsSync(p)) continue;
    text += readFileSync(p, 'utf8') + '\n';
    files++; bytes += statSync(p).size;
  }
  return { text, files, bytes };
}

/* A COUNT OF A SPELLING, AND THE SPELLING IS PRINTED WITH IT. This is NOT an estimate of how many request
   call sites the bundle has: an alias (`const f = fetch`) is invisible to it and a string literal `"fetch("`
   inflates it, so it is neither a floor nor a ceiling on call sites. What it IS is a fact about the shipped
   bytes that is stable across runs and moves only when the app ships different code, which is what a
   denominator has to be. These identifiers are PLATFORM names, which is the property that makes a text count
   worth anything here at all — a minifier renames locals and cannot rename `fetch` or `XMLHttpRequest`. */
const REQUEST_TOKENS =
  /(?<![$\w.])(?:fetch|XMLHttpRequest|EventSource|WebSocket|importScripts|sendBeacon)\s*\(|(?<![$\w.])import\s*\(/g;

/* THE PARTITION. Returns which of the three classes an address falls in, and nothing else decides it. */
function classify(key, ref) {
  const p = pathOf(key), d = decode(p);
  if (ref.named.has(key) || ref.html.includes(p) || ref.html.includes(d)) return 'markup';
  if (ref.js.includes(p) || ref.js.includes(d)) return 'inJs';
  return 'composed';
}

/* THE CLASSIFIER IS ARMED BEFORE IT IS BELIEVED, EVERY RUN. Every site below reports `composed: 0`, and a
   classifier whose COMPOSED branch is unreachable reports exactly that for every input — the two render
   identically and only a control separates them. §A-CONTROL-ARMS-ONLY-ON-A-SITE-THE-INSTRUMENT-CAN-JUDGE:
   show the check speaking before reading its silence. Four inputs, one per branch, including the RELATIVE
   specifier that only attribute resolution can catch and the PERCENT-ENCODED spelling that only the decode
   arm can. A failure THROWS rather than warning: a zero from an unarmed classifier is worse than no number. */
function selftest() {
  const base = 'https://example.test/dir/';
  const html = '<img src="/named.png"><script src="./rel.js"></script><img srcSet="/sp ace.png 2x">'
             + '<img srcSet="./img/a.png 1x, ./img/b.png 2x">';
  const ref = { html, js: 'var a="/only-in-bundle.json";', base, named: namedByMarkup(html, base) };
  const want = [
    ['example.test/named.png', 'markup', 'an absolute path the document names'],
    ['example.test/dir/rel.js', 'markup', 'a document-relative specifier, which only attribute resolution finds'],
    ['example.test/sp%20ace.png', 'markup', 'a percent-encoded learned form against a raw-space attribute'],
    ['example.test/dir/img/b.png', 'markup', 'the SECOND candidate of a camelCase `srcSet` list, reachable '
      + 'only by reading every attribute name in any case AND splitting the list — the two misses that '
      + 'inflated the column this file replaces'],
    ['example.test/only-in-bundle.json', 'inJs', 'a literal present in the shipped script and not the document'],
    ['example.test/api/v1/orders/4172', 'composed', 'an address in neither'],
  ];
  /* AND `addrKey` IS EXERCISED TOO, BECAUSE THE PARTITION IS ONLY EVER AS GOOD AS THE NORMALISATION
     FEEDING IT. A first version of this control tested `classify` alone and stayed silent when the HOST was
     dropped from the key — which is exactly the defect in the column this file replaces, so the one
     regression most likely to be re-made was the one the control could not see. All three spellings that
     reach addrKey are here, and the cross-origin case fails loudly the moment the host stops being part of
     an address's identity. */
  const O = 'https://app.example';
  for (const [raw, expect, why] of [
    ['GET http://127.0.0.1:8973/_m/app.example/static/x.js', 'app.example/static/x.js',
     'the fixture\'s rewritten form maps back to the host it was mirrored from'],
    ['GET http://127.0.0.1:8973/runtime.js', 'app.example/runtime.js',
     'a same-origin RUNTIME request the fixture never rewrote resolves on the document\'s own origin'],
    ['GET https://covers.example/b/id/1.jpg', 'covers.example/b/id/1.jpg',
     'a cross-origin absolute keeps its OWN host — dropping it collides two distinct addresses into one key'],
    ['GET http://127.0.0.1:8973/_m/a.example/i?u=1', 'a.example/i?u=1',
     'the query is part of an address because it is program input'],
  ]) {
    const got = addrKey(raw, O);
    if (got !== expect) throw new Error(`reach.mjs selftest: addrKey(\`${raw}\`) gave \`${got}\`, expected ` +
      `\`${expect}\` — ${why}.`);
  }
  for (const [key, expect, why] of want) {
    const got = classify(key, ref);
    if (got !== expect) throw new Error(`reach.mjs selftest: \`${key}\` classified \`${got}\`, expected ` +
      `\`${expect}\` — ${why}.\n  The partition below is not trustworthy until this passes: an unarmed branch ` +
      `reports the same zero as an empty population, and this file exists to tell those apart.`);
  }
}
selftest();

const files = process.argv.slice(2);
if (!files.length) { console.error('usage: node reach.mjs <census .jsonl files>'); process.exit(2); }
/* A CENSUS ROW IS READ FOR ITS OWN `siteEndpoints` AND THE FILENAME DECIDES NOTHING. */
const rowsById = new Map();
for (const f of files) {
  for (const line of readFileSync(join(ROOT, f), 'utf8').trim().split('\n').filter(Boolean)) {
    const r = JSON.parse(line);
    if (!rowsById.has(r.id)) rowsById.set(r.id, []);
    rowsById.get(r.id).push(r);
  }
}

const out = [], notMeasured = [];
/* A CENSUS ROW FOR A SITE THIS MIRROR NEVER CAPTURED IS SAID, NOT DROPPED. `run.sh AT=live` drives a row's
   own URL over the internet and writes a row with no frozen bytes behind it, so such an id is legitimately
   unpartitionable here rather than a pairing accident — but a loop that iterates the MANIFEST and looks each
   id up would skip it in silence, and a reader comparing this table's site count against the census's would
   have no way to see the difference. An absent split and a split of zero are different facts. */
for (const id of rowsById.keys()) {
  if (!manifest.some((m) => m.id === id)) notMeasured.push(`${id}: census rows exist but provenance.json `
    + `records no frozen capture (a live row has no shipped bytes to partition against)`);
}
let orphanAsked = 0, orphanDriven = 0, orphanReporting = 0, orphanSilent = 0;
for (const site of manifest) {
  const rows = rowsById.get(site.id);
  if (!rows) { notMeasured.push(`${site.id}: mirrored, but no census file passed named it`); continue; }
  const docPath = join(MIRROR, site.id, 'index.html');
  if (!existsSync(docPath)) { notMeasured.push(`${site.id}: census rows exist but no mirrored document`); continue; }
  if (!site.baseUrl) throw new Error(`reach.mjs: provenance.json entry \`${site.id}\` declares no \`baseUrl\`.\n` +
    `  That field is mirror.mjs's own record of what the captured document resolves against. Without it every\n` +
    `  relative specifier in the markup would resolve somewhere arbitrary and be reported as COMPOSED — the\n` +
    `  one direction this file must never be wrong in. Re-capture the site rather than defaulting the base.`);

  const html = readFileSync(docPath, 'utf8');
  const js = shippedJs(site);
  const ref = { html, js: js.text, base: site.baseUrl, named: namedByMarkup(html, site.baseUrl) };
  const origin = new URL(site.baseUrl).origin;

  const learned = new Set();
  for (const e of rows.flatMap((r) => r.siteEndpoints || [])) {
    const k = addrKey(e, origin);
    if (k) learned.add(k);
  }
  /* EVERY ADDRESS LANDS IN A DECLARED CLASS, ASSERTED WHERE THE CLASS IS BORN. The obvious check here is
     that the three buckets SUM to `learned.size` — CLAUDE.md asks for exactly that of any published count —
     and over this partition it is VACUOUS: the buckets are filled by indexing on classify's own return, so a
     fourth class raises a TypeError on the push and the sum can never disagree with itself. An assert whose
     two sides cannot differ reports as a passing check and is not one, and the more prominent it is the more
     thoroughly it certifies what it never examined. It is recorded here rather than deleted silently because
     `assert the parts sum to the total` is a rule a reader will re-derive and re-add.
     WHAT CAN ACTUALLY FAIL is the class NAME, checked against the declared set at the moment it is produced,
     which is also where a reader adding a fourth class has to meet it. */
  const CLASSES = ['markup', 'inJs', 'composed'];
  const bucket = { markup: [], inJs: [], composed: [] };
  for (const k of learned) {
    const c = classify(k, ref);
    if (!CLASSES.includes(c)) throw new Error(`reach.mjs: classify() returned \`${c}\` for \`${k}\`, which ` +
      `is not one of ${CLASSES.join('/')}. Every learned address falls in exactly one declared class or the ` +
      `fractions this file prints are fractions of nothing — add the class to CLASSES, to the bucket, to the ` +
      `header and to selftest() together, never to the classifier alone.`);
    bucket[c].push(k);
  }

  /* THE ORPHAN PAIR IS READ FROM THE ROWS, AND A ROW THAT CARRIES NEITHER IS COUNTED AS SILENT RATHER THAN
     AS ZERO. §Testing: an absent count and a zero count are different facts and must never be averaged. */
  const withOrphans = rows.filter((r) => typeof r.orphansAsked === 'number');
  if (withOrphans.length) {
    orphanReporting++;
    orphanAsked += Math.max(...withOrphans.map((r) => r.orphansAsked));
    orphanDriven += Math.max(...withOrphans.map((r) => r.orphansDriven || 0));
  } else orphanSilent++;

  out.push({ id: site.id, learned: learned.size, ...bucket, js, tokens: (js.text.match(REQUEST_TOKENS) || []).length,
             orphans: withOrphans.length ? `${Math.max(...withOrphans.map((r) => r.orphansAsked))}` : '-' });
}

const pad = (v, n) => String(v).padStart(n);
console.log('ADDRESS PROVENANCE — every learned address is exactly one class; `composed` is the only one a '
          + 'static tool cannot produce.\n');
console.log('site           learned   markup    in-JS  COMPOSED |  shipped JS   req-token  orphans');
console.log('                                                   |  files  KiB   occurs      asked');
for (const r of out) {
  console.log(`${r.id.padEnd(13)} ${pad(r.learned, 7)} ${pad(r.markup.length, 8)} ${pad(r.inJs.length, 8)} `
    + `${pad(r.composed.length, 9)} | ${pad(r.js.files, 6)} ${pad(Math.round(r.js.bytes / 1024), 5)} `
    + `${pad(r.tokens, 9)} ${pad(r.orphans, 10)}`);
}
const T = out.reduce((a, r) => ({ learned: a.learned + r.learned, markup: a.markup + r.markup.length,
  inJs: a.inJs + r.inJs.length, composed: a.composed + r.composed.length,
  jsFiles: a.jsFiles + r.js.files, jsBytes: a.jsBytes + r.js.bytes, tokens: a.tokens + r.tokens }),
  { learned: 0, markup: 0, inJs: 0, composed: 0, jsFiles: 0, jsBytes: 0, tokens: 0 });
console.log(`${'TOTAL'.padEnd(13)} ${pad(T.learned, 7)} ${pad(T.markup, 8)} ${pad(T.inJs, 8)} `
  + `${pad(T.composed, 9)} | ${pad(T.jsFiles, 6)} ${pad(Math.round(T.jsBytes / 1024), 5)} ${pad(T.tokens, 9)}`);
console.log(`\ncomposed / learned            : ${T.composed} / ${T.learned}`
  + `   (addresses that required executing code, out of every address the run learned)`);
console.log(`sites with a composed address : ${out.filter((r) => r.composed.length).length} / ${out.length}`);
console.log(`orphan drive asked / reported : ${orphanAsked} / ${orphanReporting} sites`
  + `   (${orphanSilent} of ${out.length} sites reported no orphan counter at all — silent, not zero)`);
console.log(`orphan bodies driven          : ${orphanDriven}`);
for (const r of out) if (r.composed.length) {
  console.log(`\n  ${r.id} composed (${r.composed.length}):`);
  for (const k of r.composed) console.log(`    ${k}`);
}
if (notMeasured.length) {
  console.log('\nNOT MEASURED HERE (stated rather than skipped — an absent split and a split of zero are '
            + 'different facts):');
  for (const n of notMeasured) console.log('  ' + n);
}

/* WHAT THIS INSTRUMENT DOES NOT ASK. Printed on every run, including a clean one: a blind spot named only on
   the day it bites is one nobody learns to look for, and a reader who takes a `0` above without this section
   has taken a number for a question that was never put. */
console.log(`
BLIND SPOTS — this instrument owns ONE axis (what a learned ADDRESS could only have come from) and these are
the questions it does not answer. None of them is folded into a figure above.

  FUNCTIONS ENTERED, CALL SITES EVALUATED. "of the functions the bundle ships, how many ran" and "of its
    request call sites, how many were evaluated" are the other two halves of reach and NO census field
    answers either. The denominators are on this page (shipped JS, req-token occurs); the numerators would
    have to be counted by the engine and emitted per run. Nothing here can infer them, and a ratio built out
    of the columns above would be a fraction whose numerator does not exist.

  req-token occurs IS A COUNT OF A SPELLING, not of call sites. The spelling is
    ${REQUEST_TOKENS.source}
    An aliased call (\`const f = fetch\`) is invisible to it and a matching string literal inflates it, so it
    is neither a floor nor a ceiling on how many request sites the bundle has. It is a fact about the bytes.

  COMPOSED IS A FLOOR, IN THE SAFE DIRECTION. The markup and in-JS tests are deliberately over-generous —
    every attribute value resolved, plus raw-substring tests in both encoded and decoded spellings against
    the document and against every shipped script. A generous test can only move an address OUT of composed,
    so this file can UNDER-state execution-derived learning and cannot over-state it.

  THE DENOMINATOR BELONGS TO A REVISION AND THE NUMERATOR DOES NOT. Everything left of the bar is derived
    from testing/corpus/mirror + provenance.json, which are TRACKED, so it is reproducible at a commit.
    The census .jsonl files this run was handed are NOT tracked by git: quote a figure from them with the
    artifact and the run that produced it, never as a property of a revision.

  A LEARNED ADDRESS IS NOT AN ENDPOINT. This partitions what the run recorded; whether a recorded address is
    an API endpoint or a subresource is a different question and no column here asks it.`);
