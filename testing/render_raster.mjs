/* render_raster.mjs — AN IMAGE OF ONE WORLD, OUT OF AN ARTIFACT THAT CAN NAME ITS REVISION.
 *
 * THE GAP THIS CLOSES IS NOT "NOTHING PAINTS". The native host paints already: test_forced.c's `abi_paint`
 * reads the thirteen paint entries and writes a PAM per world, and engine/one_document.mjs and
 * engine/trusted.mjs drive it. What no artifact could do until this file is paint AND say which revision the
 * picture belongs to — the native binary carries no `.build.json`, and the stamped wasm artifact, whose paint
 * entries are exported and reachable, had no caller anywhere that asked one. So a reader holding a picture of
 * this engine could have the image or the revision and never both, and §Testing rates a result quoted without
 * the revision it came from as not a measurement at all.
 *
 * IT DOES NOT RETIRE qjs_abi.h's PAINT RESIDUAL AND MUST NOT BE READ AS DOING SO. That record says in as many
 * words that a Node driver loading the glue and `ccall`ing these entries "is outside `engine/host/`… and such
 * a driver is worth making and is not this record's subject", because its observation clause is about the
 * SHIPPED extension's own step loop. This is that driver. The residual's condition — the extension's step
 * loop asking a paint entry on its ENGINE_STEP_YIELD arm — is untouched by this file and still answers DEFER.
 *
 * THE ORDER OF THE ABI CALLS IS THE ENGINE'S AND NOT A PREFERENCE, and it is `abi_paint`'s order because that
 * is the same read already written against the same ABI. `qjs_paint` PERFORMS the render and every entry
 * beside it states a fact about what that call just produced; `qjs_paint_bytes` enforces the pair's order
 * rather than documenting it, so a length asked first is an abort instead of a zero that reads exactly like
 * the length of an empty image. The address is valid until the next `qjs_paint` or `qjs_teardown` and this
 * host never frees it — across the wasm boundary that means the `subarray` below aliases linear memory and is
 * copied into the file before the next step can invalidate it.
 *
 * ONE `qjs_paint` PER ROUND BOUNDARY, WHICH IS A COPIED CADENCE AND NOT A CHOSEN ONE. `qjs_request_paint_every_world`
 * is a STANDING ask made once after `qjs_begin` seeds the frontier; what it buys is that the world standing at
 * a boundary is one somebody asked for, and a host is only ever AT a boundary — so a host that painted every
 * Nth round would simply miss the worlds presented on the others. test_forced.c records having tried a cadence
 * and having measured that no cadence it could choose reaches an arm forked, run and ended inside one step.
 * There is therefore no `--paint-every` flag here, deliberately.
 *
 * THE OUTCOME AND DECLINE LABELS ARE DERIVED FROM THE HEADER AT THE STAMPED REVISION AND NEVER TYPED HERE.
 * `abi_paint` asserts its label tables against the enums because C cannot miss a member of an array of string
 * literals; this host is worse off than that — it is a different language and a different build — so it reads
 * `box_paint.h` OUT OF THE COMMIT THE ARTIFACT WAS BUILT FROM (`git show <head>:<path>`) rather than out of the
 * working tree, which is a file other lanes edit continuously and which is not the program that ran. A label
 * table and an engine one member apart is a bucket printed under its neighbour's name, which reads as a
 * different finding rather than as a number that is missing.
 *
 * IT DRIVES AN ALREADY-BUILT ARTIFACT AND COMPILES NOTHING. A lane in this project may not build.
 *
 * WHAT IT CANNOT SEE, STATED HERE BECAUSE AN INSTRUMENT TRUSTED PAST ITS EVIDENCE IS WORSE THAN NONE. Without
 * `--serve` this fetches NOTHING, so a document whose appearance comes from its stylesheets renders as its own
 * UA defaults and one whose DOM its scripts build renders as its server-sent tree — engine/one_document.mjs's
 * banner states that trade and it is the same one here. With `--serve` it answers PROGRAM LOADS ONLY, which is
 * CLAUDE.md's own default arm for an origin nobody has widened: a script, a module, a chunk or a stylesheet the
 * page's own markup or its own running code names, same-origin, which is the page loading itself. Everything
 * else is `qjs_decline`d BY NAME — which is not a failure but the other half of paying, and the engine forks
 * the arm that runs the page's failure path so the `catch` is explored without the wait being spent.
 *
 * usage: node testing/render_raster.mjs paint --doc <file.html> --url <addr> --out <dir> [--serve] [--steps N]
 *        node testing/render_raster.mjs paint --doc f.html --url u --out d --glue <qjs.mjs>
 */

import { readFileSync, writeFileSync, mkdirSync, openSync, writeSync, closeSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { abiOperands } from './../engine/renderer_abi.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');
const require_ = createRequire(import.meta.url);

/* THE STAMP READER IS testing/artifact_stamp.js AND IS NOT RE-IMPLEMENTED HERE. That module holds a refusal
   this file would not have thought to write — an artifact NEWER than the stamp that claims to describe it,
   which is how a fresh wasm copied in beside an old sidecar files every row under a revision ~350 commits
   away — and it holds it because two copies of one check is the shape where the copy nobody runs against
   reality is the one that drifts. testing/render_engine.mjs has its own `stampOf`, which this deliberately
   does NOT import: that file `require`s testing/render_diff.js at module scope and that file requires
   PUPPETEER, so importing it to read a JSON sidecar would launch a browser dependency to answer a question
   about a file on disk. */
const { artifactStamp } = require_(join(HERE, 'artifact_stamp.js'));

function fail(msg) { throw new Error('@WHY render_raster: ' + msg); }

/* ONE DOOR FOR EVERY ALLOCATION IN THE WASM HEAP. `_malloc` reports failure by RETURNING ZERO rather than by
   throwing, so no `catch` anywhere in this file sees one: every caller here then writes its operand over the
   null page and hands the ENGINE that address, which is a corrupt reply the engine cannot tell from a real
   one. Three sites were asking that question three ways and not one of them had anything downstream to make
   the silence loud, so this is a route to a canonical spelling rather than three correct answers — the shape
   that drifts. §CHECK's own category: an allocation failure is fatal in dev and in release alike, because a
   dropped reply corrupts the frontier. */
function heapAlloc(M, n, what) {
  const p = M._malloc(n);
  if (p === 0)
    fail('_malloc refused ' + n + ' byte(s) for ' + what + ' — the WASM heap is out. That is the physical ' +
         'floor §CHECK names and never something to answer a request past: a zero here is an ADDRESS as far ' +
         'as every line below is concerned, so the alternative to this crash is the engine parsing whatever ' +
         'happens to sit at offset 0 as a reply nobody sent.');
  return p;
}

const FLAGS_WITH_VALUES = new Set(['--glue', '--doc', '--url', '--out', '--doc-id', '--steps', '--ext']);
const FLAGS_BARE = new Set(['--serve']);
function parseArgs(argv) {
  const flags = Object.create(null), positional = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (FLAGS_WITH_VALUES.has(a)) { if (i + 1 >= argv.length) fail(a + ' needs a value'); flags[a] = argv[++i]; }
    else if (FLAGS_BARE.has(a)) flags[a] = true;
    else if (a.startsWith('--')) fail('unknown flag ' + a);
    else positional.push(a);
  }
  return { flags, positional };
}

/* THE DISTANCE, WHICH THE STAMP ITSELF DOES NOT CARRY AND WHICH IS THE NUMBER A READER NEEDS BESIDE IT.
   A stamp says WHICH revision and `treeAtBuild` says the cone was clean AT that revision; neither says HOW
   FAR, and a reader who has satisfied both reads the pair as a clean bill. An artifact that is clean and
   2403 commits old is an oracle for nothing, and it passes every other check in this file. It reports
   UNANSWERABLE rather than 0 where the revision is not in this checkout, because those are different facts. */
function distanceOf(head) {
  try {
    return execFileSync('git', ['rev-list', '--count', head + '..origin/main'],
                        { cwd: ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
  } catch { return null; }
}

/* THE TWO ENUMS, READ OUT OF THE COMMIT THE ARTIFACT WAS BUILT FROM. Parsed from the header's own member
   names rather than from a list typed here, so a member added to either enum is a row this file prints and
   never a number that silently moves one column left. The names are lowercased and stripped of their common
   prefix, which is exactly `abi_paint`'s own label spelling. */
function enumsAt(head) {
  const P = 'engine/host/browser/core/paint/box_paint.h';
  let src;
  try {
    src = execFileSync('git', ['show', head + ':' + P], { cwd: ROOT, encoding: 'utf8' });
  } catch (e) {
    fail('could not read ' + P + ' at the artifact\'s own revision ' + head.slice(0, 12) + ' (' + e.message +
         '). The label tables must come from the commit the ENGINE was built from, not from the working ' +
         'tree — a table one member apart from the engine\'s prints a bucket under its neighbour\'s name.');
  }
  const members = (re) => {
    const m = src.match(re);
    if (!m) fail('no ' + re + ' in ' + P + ' at ' + head.slice(0, 12));
    return m[1].split(/[\n,]/).map((s) => s.replace(/\/\*[\s\S]*?\*\//g, '').trim())
               .map((s) => s.split(/\s|\//)[0].trim())
               .filter((s) => /^BOX_PAINT_[A-Z_]+$/.test(s));
  };
  const strip = (a, p) => a.map((s) => s.replace(p, '').toLowerCase().replace(/_/g, '-'));
  /* The TERMINATOR of each enum is its own count and is not a bucket — it is dropped here, and the length
     that remains IS `BOX_PAINT_OUTCOMES` / `BOX_PAINT_DECLINES` by construction rather than by a second
     number this file would have to keep in step. */
  /* THE BODY MAY NOT CONTAIN A BRACE, WHICH IS WHAT MAKES EACH PATTERN MATCH ONE ENUM RATHER THAN A SPAN OF
     TWO. Written with `[\s\S]*?` this matched from the FIRST `typedef enum {` in the file to the LAST enum's
     terminator, so the decline table silently began with the four OUTCOME members and every decline bucket
     was printed five columns to the right of its own name — a number reading as a different finding rather
     than a number that is missing, which is exactly what `abi_paint` asserts its own tables against the
     enums to prevent. `[^}]` cannot span two enum bodies at all, so the property is structural here rather
     than a thing this file remembers. */
  const out = members(/typedef enum \{([^}]*?)\} BoxPaintOutcome;/);
  const dec = members(/typedef enum \{([^}]*?)\} BoxPaintDecline;/);
  const outs = out.filter((s) => s !== 'BOX_PAINT_OUTCOMES');
  const decs = dec.filter((s) => s !== 'BOX_PAINT_DECLINES');
  if (outs.length !== out.length - 1 || decs.length !== dec.length - 1)
    fail('box_paint.h at ' + head.slice(0, 12) + ' does not end each enum with its own count — the label ' +
         'table\'s length would then be a number typed here rather than the enum\'s own');
  return { outcome: strip(outs, /^BOX_PAINT_/), decline: strip(decs, /^BOX_PAINT_DECLINE_/) };
}

/* THE FILE NAME, COMPOSED AS `abi_paint` COMPOSES IT — `<doc>.<world>.pam`, every byte outside a portable
   file-name set replaced by `_`. It is the same rule and not a similar one on purpose: a directory written
   by this host and one written by the native host must be readable by the SAME pruner and the same
   converter, and a name that differed would be a second grammar for one artifact. The world is what makes a
   directory of these a picture of a solver rather than N reads of one timeline. A TRUNCATED WORLD IS A
   DIFFERENT WORLD, so this refuses rather than writing one — two timelines whose names share a prefix would
   overwrite each other's image while both reported success. */
const NAME_MAX = 512;
function pamName(docId, world) {
  const san = (s) => s.replace(/[^A-Za-z0-9.\-_]/g, '_');
  const d = san(docId), w = san(world);
  if (d.length === 0) fail('this instance was provisioned under an EMPTY document name, so the image of it ' +
                           'has nothing to be called');
  const n = d + '.' + w + '.pam';
  if (n.length + 1 >= NAME_MAX)
    fail('the image\'s name does not fit ' + NAME_MAX + ' bytes (' + n.length + ') — a truncated world is a ' +
         'DIFFERENT world, so two timelines sharing a prefix would overwrite each other\'s picture and each ' +
         'would report that it had written one: ' + n.slice(0, 120));
  return n;
}

/* THE PAM HEADER, IN `abi_paint`'s OWN GRAMMAR AND DERIVED FROM IT RATHER THAN INVENTED. netpbm's `P7`,
   header lines in any order, `ENDHDR`, then the raster with no delimiter of any kind. `DEPTH 4` and
   `TUPLTYPE RGB_ALPHA` are what make the fourth plane the OPACITY plane; `MAXVAL 255` is what makes each
   sample one byte, which is what makes the raster this engine's own bytes rather than a conversion of them.
   THE `#` LINES ARE THE HONESTY AND THEY ARE WHY THIS IS NOT A BARE BITMAP. A surface with no ink on it
   means the walk reached no box, or reached boxes that painted nothing, or was STOPPED, or there was no
   rendered region — and a reader handed only the pixels cannot separate them. `complete` FALSE is a
   FRAGMENT of a page, which is the one of those a host must never publish as a whole one, so the word
   STOPPED goes in the artifact and not into a log a copy of the file would lose.
   A NEWLINE IN ANY VALUE ENDS A `#` COMMENT AND LEAVES THE REST TO BE READ AS HEADER SYNTAX, which is a file
   describing an image with the wrong dimensions or no ENDHDR at all. `abi_paint` asserts that from C; this
   host takes values across the same ABI plus a `--url` a person typed, so it asserts the same property. */
function pamHeader(o) {
  for (const [k, v] of [['url', o.url], ['world', o.world], ['forced', o.forced], ['docId', o.docId]])
    if (String(v).includes('\n'))
      fail('the ' + k + ' reaching this PAM header carries a newline, which TERMINATES the `#` comment it ' +
           'is written into — the rest of the value would be read as header syntax and the file would ' +
           'describe an image with the wrong dimensions or no ENDHDR at all: ' + JSON.stringify(String(v).slice(0, 80)));
  const parts = o.outcome.map((n, i) => n + ' ' + o.outcomeLabel[i]).join(', ');
  const fired = o.decline.map((n, i) => [n, o.declineLabel[i]]).filter(([n]) => n > 0);
  const whys = fired.length ? fired.map(([n, l]) => n + ' ' + l).join(', ') : 'none — no offer declined';
  return 'P7\n' +
    '# rendered by APIClient\'s engine from ' + o.url + '\n' +
    '# world ' + o.world + '\n' +
    '# the path that laid this ink is ' + o.forced + '\n' +
    '#   `forced` means this timeline took an arm its own concrete example CONTRADICTS, so\n' +
    '#   this is a picture of a page no session reaches. `unforced` is NECESSARY and NOT\n' +
    '#   SUFFICIENT for the appearance a browser would have produced. `baseline` is the\n' +
    '#   document as no flow has written it, in which none of the page\'s own code has run.\n' +
    '# CSS 2.1 §E.2 "Painting order" offered ' + o.offers + ' step(s) and laid ' + o.marks + ' mark(s)\n' +
    '#   of those offers: ' + parts + '\n' +
    '# and why the offers that laid nothing laid nothing: ' + whys + '\n' +
    '#   These do NOT partition the offers above and are not meant to: one offer is one\n' +
    '#   §E.2 STEP, and a step\'s sub-list can decline twice. Their denominator is the offer count.\n' +
    '# the walk covered ' + o.elements + ' element(s) of DOM §4.8\'s shadow-including tree under the root\n' +
    '#   That is the DOCUMENT\'s own size and every number above is the WALK\'s, taken at the\n' +
    '#   same instant by the same walker. No inequality holds between the two and none is asserted.\n' +
    '# the walk ' + (o.complete
      ? 'FINISHED: nothing was left unpainted that this engine paints'
      : 'STOPPED: this picture is PARTIAL — the painter met an operand it could not compute and every ' +
        'mark it had already laid is in the image') + '\n' +
    '# written by testing/render_raster.mjs from the artifact stamped ' + o.head + '\n' +
    'WIDTH ' + o.w + '\n' + 'HEIGHT ' + o.h + '\n' + 'DEPTH 4\n' + 'MAXVAL 255\n' +
    'TUPLTYPE RGB_ALPHA\nENDHDR\n';
}

/* THE RENDER, AND THE READINGS OF IT — `abi_paint`'s order, which is the ABI's and not a preference. */
function readPaint(M, labels) {
  const u = (f) => M.ccall(f, 'number', [], []);
  const px = M.ccall('qjs_paint', 'number', [], []);       /* PERFORMS; everything below states a fact about it */
  const n = u('qjs_paint_bytes');
  const w = u('qjs_paint_width'), h = u('qjs_paint_height');
  const offers = u('qjs_paint_offers');
  const elements = u('qjs_paint_elements');
  const outcome = labels.outcome.map((_, i) => M.ccall('qjs_paint_offer_outcome', 'number', ['number'], [i]));
  const decline = labels.decline.map((_, i) => M.ccall('qjs_paint_decline', 'number', ['number'], [i]));
  const marks = u('qjs_paint_marks');
  const complete = u('qjs_paint_complete') !== 0;
  const world = String(M.ccall('qjs_paint_world', 'string', [], []) ?? '');
  const forced = String(M.ccall('qjs_paint_forced', 'string', [], []) ?? '');
  /* THE SHAPE AND THE EXTENT ARE ONE FACT — RGBA8 is four bytes per pixel, so a disagreement here is two
     renders read as one and the raster written from them would be a prefix or an overrun of whichever is
     real. `abi_paint` makes this a `CHECK` and not a `DCHECK` because the product of the failure is an
     artifact somebody LOOKS AT, and that argument is unchanged on this side of the ABI. */
  if (n !== w * h * 4)
    fail('this host read an image of ' + w + ' x ' + h + ' pixels and an extent of ' + n + ' bytes, which ' +
         'are not the same picture — RGBA8 is four bytes per pixel, so these are two renders read as one');
  /* AN ABSENT RUN AND AN EXTENT OF ZERO ARE ONE FACT, asserted from both ends inside the engine, so a
     disagreement HERE is that pair having come apart across the ABI. NULL is an ANSWER and not an error. */
  if ((n === 0) !== (px === 0))
    fail('the ABI answered an image whose pointer (' + px + ') and whose extent (' + n + ') disagree about ' +
         'whether there is a picture');
  /* THE PARTITION'S SUM, CHECKED OVER THE NUMBERS THAT CAME BACK ACROSS THE ABI — one call per bucket. The
     engine already asserts this identity internally over the counters as it wrote them; what this adds is a
     reading of what arrived, which is where an entry indexing a different array, or a host whose enum is one
     member behind the engine's, lands. It is the one cross-ABI check this host can make about the labels. */
  const part = outcome.reduce((a, b) => a + b, 0);
  if (part !== offers)
    fail('this host read ' + offers + ' offer(s) and ' + part + ' outcome(s) for them across ' +
         outcome.length + ' bucket(s). CSS 2.1 §E.2\'s outcomes PARTITION the offers, so a disagreement ' +
         'HERE is this host\'s enum being a different number than the engine was built with');
  return { px, n, w, h, offers, marks, complete, elements, outcome, decline, world, forced };
}

/* WHAT THIS HOST WILL PAY FOR, WHICH IS CLAUDE.md's DEFAULT ARM AND NOT A POLICY OF THIS FILE'S OWN. For an
   origin nobody has deliberately widened the default is PROGRAM LOADS ONLY — a script, a module import, a
   lazy chunk, and the styles the page's own markup names, same-origin, which is the page loading itself and
   is a request the person's own browser would have made. A data fetch, an API call and a discovery probe
   need the origin widened and this file has no flag that widens one.
   THE DESTINATION IS READ OFF THE PENDING LINE AND IS NEVER DERIVED FROM THE URL. Fetch §2.2.5's destination
   rides every pending line because the algorithm that CREATED the request is the only party that knows it: a
   `<link rel=preload as=script>` and a `fetch()` of the same address are opposite answers to "may this reply
   be compiled", so a suffix-to-kind table here would be a second producer of a fact the request carries.
   A REFUSAL IS THE OTHER HALF OF PAYING AND NOT A KIND OF FAILURE. `qjs_decline` is the answer wherever no
   real browser performing this request would produce a network error: nothing is fabricated about the
   origin, the flow keeps its park, and the engine forks the arm that runs the page's failure path. */
const PROGRAM_DESTINATIONS = new Set(['script', 'style', 'worker', 'sharedworker', 'serviceworker']);

function splitPending(line) {
  /* SPLIT WHERE THE ENGINE JOINED IT — `method\tdestination\tinitiator\tprovenance\tpinned\tcredentials\turl`,
     engine.h's `engine_pending_split` order. Only the LAST field may contain the delimiter and the URL
     provably cannot (URL Standard §4.4 removes every tab before anything else), so this splits on the first
     six tabs and takes the remainder. A line with a different field count is a loud failure rather than a
     silently mis-assigned address. */
  const f = line.split('\t');
  if (f.length !== 7)
    fail('a pending line carries ' + f.length + ' field(s) where engine_pending_split names seven ' +
         '(method, destination, initiator, provenance, pinned, credentials, url) — this host is splitting a ' +
         'record the engine joined differently, and guessing would attribute one request\'s address to ' +
         'another\'s method: ' + JSON.stringify(line.slice(0, 200)));
  return { method: f[0], destination: f[1], initiator: f[2], provenance: f[3],
           pinned: f[4], credentials: f[5], url: f[6] };
}

async function serveOne(M, cs, req, docOrigin, log) {
  const decline = (why) => {
    M.ccall('qjs_decline', 'void', ['number', 'number', 'number'],
            [cs(req.method), cs(req.url), cs(why)]);
    log.push({ url: req.url, destination: req.destination, action: 'decline', why });
  };
  if (!PROGRAM_DESTINATIONS.has(req.destination))
    return decline('this host answers PROGRAM LOADS ONLY (CLAUDE.md\'s default arm for an origin nobody ' +
                   'has widened); Fetch §2.2.5 destination `' + req.destination + '` is a value fetch');
  let u;
  try { u = new URL(req.url); } catch { return decline('unparseable address'); }
  if (u.origin !== docOrigin)
    return decline('cross-origin (' + u.origin + ' vs the document\'s ' + docOrigin + '); a program load is ' +
                   'the page loading ITSELF, and a stranger\'s origin is not that');
  if (req.method !== 'GET') return decline('method ' + req.method + ' is outside RFC 9110 §9.2.1\'s safe set');
  /* THE TRY COVERS EXACTLY THE ACT A NETWORK ERROR CAN COME OUT OF AND NOTHING THIS HOST DOES AFTERWARDS.
     Which failures may be answered rather than asserted is decided by WHOSE BYTES STATE THE VALUE: a `fetch`
     rejection is a fact about a STRANGER'S server and the wire, so the answer is a refusal yielding the
     record's declared absence — Fetch §5.6's network error, which is what `qjs_provide` with the JSON `null`
     is, and not a decline, which would be this host refusing to spend an act it did in fact spend.
     EVERYTHING BELOW THE CATCH IS THIS HOST'S OWN WORK AND THE ENGINE'S: a composition, an allocation in the
     WASM heap, a write into it, and an ABI call whose abort surfaces here as a thrown `RuntimeError`. Those
     are broken invariants of this codebase, and the catch that used to cover them turned each one into a
     PLAUSIBLE DATUM — the engine told the WIRE had failed, the flow forking the page's own failure path, and
     the run reporting a clean result with a reply nobody sent. It also delivered `qjs_provide` a SECOND time
     into a runtime that had just aborted inside the first. Widening this back is not a smaller fix; it is
     the swallow, and the reason it looked harmless is that its one legitimate member — the fetch — really
     does belong in it. */
  let r, body;
  try {
    r = await fetch(req.url, { redirect: 'follow' });
    body = new Uint8Array(await r.arrayBuffer());
  } catch (e) {
    M.ccall('qjs_provide', 'void', ['number', 'number', 'number', 'number', 'number'],
            [cs(req.method), cs(req.url), cs('null'), 0, 0]);
    log.push({ url: req.url, destination: req.destination, action: 'network-error',
               why: (e && e.message) || String(e) });
    return;
  }
  /* THE REPLY'S METADATA CROSSES AS JSON AND THE BODY CROSSES AS BYTES, BESIDE IT, because JSON cannot say
     a byte sequence and the only way to put one in JSON is to run an algorithm over it first — which is the
     defect the ABI's own entry records: a script served `charset=windows-1252` decoded here would reach
     HTML §8.1.4.2's classic decode already mangled. No transform happens on this line. */
  const reply = JSON.stringify({
    status: r.status, statusText: r.statusText || '',
    headers: [...r.headers].map(([k, v]) => [k, v]),
    urlList: [req.url, ...(r.url && r.url !== req.url ? [r.url] : [])],
  });
  const bp = heapAlloc(M, body.length + 1, 'the body of ' + req.url);
  M.HEAPU8.set(body, bp); M.HEAPU8[bp + body.length] = 0;
  M.ccall('qjs_provide', 'void', ['number', 'number', 'number', 'number', 'number'],
          [cs(req.method), cs(req.url), cs(reply), bp, body.length]);
  log.push({ url: req.url, destination: req.destination, action: 'provide',
             status: r.status, bytes: body.length });
}

async function drive(opts) {
  const factory = await import(pathToFileURL(opts.glue).href);
  const M = await (factory.default ?? factory)();
  const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = heapAlloc(M, n, 'a string operand');
                      M.stringToUTF8(s, p, n); return p; };
  const bs = (b) => {
    /* THE DOCUMENT CROSSES AS A PAIR because a zero byte is legal in a document and `strlen` would end the
       parse at the first one. */
    const u8 = new TextEncoder().encode(b);
    const p = heapAlloc(M, u8.length + 1, 'a document operand');
    M.HEAPU8.set(u8, p); M.HEAPU8[p + u8.length] = 0;
    return [p, u8.length];
  };
  const operands = abiOperands('Init', 'qjs_init', {
    document: bs(opts.html), url: opts.url, docId: opts.docId, headers: '', topLevelUrl: opts.url,
    inheritedCsp: '', inheritedCspSelfOrigin: '',
    inheritedCoep: 'unsafe-none', inheritedCoepEndpoint: '',
    inheritedCoepReportOnly: 'unsafe-none', inheritedCoepReportOnlyEndpoint: '',
    parentNavigable: 'u', containerPolicy: 'null', ancestorOrigins: 'none', creationSandboxFlags: 'none',
  }, cs);
  M.ccall('qjs_init', 'number', operands.map(() => 'number'), operands);
  /* NO RESIDUE, STATED — `qjs_begin`'s argument is the parked frontier a previous session left. */
  M.ccall('qjs_begin', 'void', ['number'], [cs('')]);
  /* AFTER `qjs_begin` AND NOT BEFORE, because the frontier is SEEDED by that call: asked earlier there is no
     member to mark. ONE ask, standing, for the whole run — test_forced.c measured that no cadence a host can
     choose reaches an arm forked, run and ended inside a single step. */
  M.ccall('qjs_request_paint_every_world', 'void', [], []);

  const docOrigin = new URL(opts.url).origin;
  const frames = [], served = [], errors = [];
  let code = -1, painted = 0;
  const paintNow = (why) => {
    const r = readPaint(M, opts.labels);
    if (r.n === 0) {
      /* CSS 2.1 §2.3.1 "The canvas": a document no navigable presents establishes no rendered region. It is a
         STATE and not a failure, so it is reported rather than crashed on — and reported rather than left
         silent because an absent file and a run never asked to paint look identical on disk. It names the
         WORLD because the region is a fact about the realm this TIMELINE is standing in: two arms of one fork
         have two viewports, so one world can establish a region while its sibling does not. */
      frames.push({ world: r.world, forced: r.forced, bytes: 0, file: null, why,
                    offers: r.offers, marks: r.marks, complete: r.complete, elements: r.elements,
                    outcome: r.outcome, decline: r.decline, noRegion: true });
      return;
    }
    const name = pamName(opts.docId, r.world);
    const head = pamHeader({ ...r, url: opts.url, docId: opts.docId, head: opts.stamp.head,
                             outcomeLabel: opts.labels.outcome, declineLabel: opts.labels.decline });
    /* THE RASTER IS COPIED OUT OF LINEAR MEMORY BEFORE ANYTHING ELSE RUNS. `subarray` is a VIEW: the address
       is valid only until the next `qjs_paint` or `qjs_teardown`, and a wasm heap can be detached and
       replaced wholesale by a growth, so a view held across a step would be read against a different memory. */
    const raster = Buffer.from(M.HEAPU8.subarray(r.px, r.px + r.n));
    const fd = openSync(join(opts.out, name), 'w');
    try { writeSync(fd, head); writeSync(fd, raster); } finally { closeSync(fd); }
    painted++;
    frames.push({ world: r.world, forced: r.forced, bytes: r.n, file: name, why,
                  offers: r.offers, marks: r.marks, complete: r.complete, elements: r.elements,
                  outcome: r.outcome, decline: r.decline, w: r.w, h: r.h, noRegion: false });
  };

  /* THE BASELINE, TAKEN BEFORE THE FIRST STEP, WHICH IS THE ONLY MOMENT IT IS GUARANTEED TO EXIST.
     `qjs_paint_world` names the world of the RUNNING FLOW, and `paint_world_name(NULL)` is the single word
     `baseline` — so the baseline image is the one taken while no flow is running. A host that only painted at
     round boundaries gets one IF the frontier drains, and a real page under a wall-denominated slice STALLS
     instead: measured here, a four-world run of a real document produced no baseline at all, which left the
     one comparison this directory exists to support with only one side. `qjs_begin` has SEEDED the frontier
     by this line and no flow has been dispatched, so the DOM is the parsed document with none of the page's
     own code having run — which is what CLAUDE.md §Boot means by a PRE-boot COW baseline, and is exactly the
     other side the predicate needs. It is asserted rather than assumed: a render here that named any other
     world would mean this moment is not the one described, and comparing against it would silently make the
     predicate a diff between two timelines rather than against the unwritten document. */
  {
    const b = readPaint(M, opts.labels);
    if (b.world !== 'baseline')
      fail('the pre-step render named the world `' + b.world + '` where `baseline` was expected. This host ' +
           'paints before the first `qjs_step` precisely because no flow is running there, so another name ' +
           'means a flow was already dispatched and this image is a TIMELINE rather than the document as no ' +
           'flow has written it — against which every later comparison would be measuring the wrong thing.');
  }
  paintNow('pre-step (no flow running — the document as no flow has written it)');

  for (let i = 0; i < opts.steps; i++) {
    code = M.ccall('qjs_step', 'number', [], []);
    /* THE PAINT STANDS BEFORE THE PAYMENT, for test_forced.c's reason: the walk is over the world the
       scheduler has just handed back, and answering a request first would step the engine past it. */
    paintNow('round ' + i + ' (qjs_step=' + code + ')');
    if (opts.serve) {
      const pend = String(M.ccall('qjs_pending', 'string', [], []) ?? '').split('\n').filter(Boolean);
      for (const line of pend) await serveOne(M, cs, splitPending(line), docOrigin, served);
    }
    if (code === 0) break;
  }
  let result = null;
  try { result = JSON.parse(String(M.ccall('qjs_result', 'string', [], []) ?? '{}')); }
  catch (e) { errors.push('qjs_result did not parse: ' + ((e && e.message) || e)); }
  /* THE FINAL PAINT STANDS AFTER THE RESULT, deliberately and for test_forced.c's stated reason: a painter
     aborting at a capability this engine has not built then costs a PICTURE rather than every endpoint and
     every verified sink the run learned. */
  paintNow('final (after qjs_result)');
  /* THE PARK IS FOR A SESSION THAT IS STILL LIVE, AND ONLY FOR ONE. `qjs_step` answering 0 is DONE — the
     frontier drained — and `engine_request_park` aborts BY NAME when asked of an engine with no live session,
     because "there is no frontier to write out, and the host would then store an empty residue over a real
     one". Measured here on the first run of this driver: an unconditional park after a document that FINISHED
     aborted at that assert, having completed every render before it. The condition is the step code and not a
     flag of this file's own. */
  if (code !== 0) {
    M.ccall('qjs_request_park', 'void', [], []);
    M.ccall('qjs_step', 'number', [], []);
  }
  M.ccall('qjs_teardown', 'void', [], []);
  return { code, frames, served, result, errors, painted };
}

/* THE SCALARS BESIDE THE VERDICT, WHICH ARE WHERE THE FALSIFYING WORK IS DONE AND NOT IN THE PIXELS. A blank
   image has FOUR causes the bitmap cannot separate — the walk reached no box, reached boxes that painted
   nothing, was STOPPED, or there was no rendered region at all — and THREE of those are facts about the
   PAINTER while one is a fact about the LOAD. The element count is the one number that is not the walk's:
   seven offers over three elements is a page with nothing in it and seven over nine hundred is a walk that
   reached almost none of a page that HAS something in it, and no inequality holds between them. */
function frameLine(f, labels) {
  const parts = f.outcome.map((n, i) => n + ' ' + labels.outcome[i]).join(', ');
  const fired = f.decline.map((n, i) => [n, labels.decline[i]]).filter(([n]) => n > 0)
                         .map(([n, l]) => n + ' ' + l).join(', ') || 'none';
  return '    ' + f.world + '  [' + f.forced + ']  ' +
    (f.noRegion ? 'NO RENDERED REGION (CSS 2.1 §2.3.1 — nowhere for a picture to be)'
                : f.bytes + ' bytes @ ' + f.w + 'x' + f.h + '  ' + f.file) +
    '\n      offers=' + f.offers + '  marks=' + f.marks + '  elements=' + f.elements +
    '  complete=' + f.complete + (f.complete ? '' : '  ** PARTIAL PICTURE **') +
    '\n      outcome: ' + parts + '\n      declined: ' + fired;
}

function stampLines(stamp, behind) {
  return ['  artifact stamp ' + stamp.head + '   (built ' + stamp.at + ')',
          '    treeAtBuild=' + stamp.treeAtBuild,
          '    trustedAtRun=' + stamp.trustedAtRun,
          '    behind origin/main: ' + (behind === null ? 'UNANSWERABLE in this checkout' : behind) +
          '   — a stale artifact is an oracle only for paths byte-identical across that span, PER PATH'];
}

const CMDS = {
  paint: async (flags) => {
    const ext = flags['--ext'] || join(ROOT, 'extension');
    const glue = flags['--glue'] || join(ext, 'lib', 'qjs', 'qjs.mjs');
    if (!flags['--doc']) fail('--doc must name an HTML file — this engine is handed a document, it does not ' +
                              'fetch one, and a driver that invented an empty one would report a picture of ' +
                              'a page nobody asked about');
    if (!flags['--url']) fail('--url must name the address this document was served from — every relative ' +
                              'URL in the page resolves against it, so a default here would be this file ' +
                              'choosing an address on a caller\'s behalf');
    if (!flags['--out']) fail('--out must name a directory to write the images into');
    const steps = flags['--steps'] === undefined ? 400 : Number(flags['--steps']);
    if (!Number.isFinite(steps) || steps <= 0) fail('--steps wants a positive count, got ' + flags['--steps']);
    /* THE STAMP IS READ BEFORE ANYTHING IS DRIVEN AND IT REFUSES RATHER THAN WARNING — every line below is
       filed under the revision it names, so a warning would be a line the reader scrolls past on the way to
       the numbers it invalidates. */
    const stamp = artifactStamp(ext);
    const behind = distanceOf(stamp.head);
    const labels = enumsAt(stamp.head);
    const docId = flags['--doc-id'] || 'raster';
    mkdirSync(flags['--out'], { recursive: true });
    const out = await drive({
      glue, html: readFileSync(flags['--doc'], 'utf8'), url: flags['--url'], docId,
      out: flags['--out'], steps, serve: flags['--serve'] === true, stamp, labels,
    });
    /* ONE ROW PER WORLD — the LAST frame each world produced, which is the file that is on disk, because the
       name carries the world and every later render of one world overwrites its own image. The count of
       FRAMES is kept beside it because a world painted forty times and a world painted once are different
       facts about the run even though the directory cannot tell them apart. */
    const last = new Map(), seen = new Map();
    for (const f of out.frames) { last.set(f.world, f); seen.set(f.world, (seen.get(f.world) || 0) + 1); }
    const lines = ['render-raster  doc=' + docId + '  url=' + flags['--url'] +
                   (flags['--serve'] ? '  [--serve: PROGRAM LOADS ONLY]' : '  [no network]'),
                   ...stampLines(stamp, behind),
                   '  qjs_step ended ' + out.code + '   (0=DONE 2=YIELD 3=STALLED)' +
                   '   rounds painted=' + out.frames.length + '   files written=' + out.painted,
                   '  worlds: ' + last.size];
    for (const [w, f] of last) lines.push(frameLine(f, labels) + '\n      frames for this world: ' + seen.get(w));
    /* THE REQUEST LOG PRINTS WHENEVER `--serve` WAS ON, INCLUDING WHEN IT ANSWERED NOTHING — a run that
       served no request and a run that was never asked to serve are different facts, and a block that
       appeared only when there was something in it would render them identically. */
    if (flags['--serve'] === true || out.served.length) {
      const prov = out.served.filter((s) => s.action === 'provide').length;
      const dec = out.served.filter((s) => s.action === 'decline').length;
      const ne = out.served.filter((s) => s.action === 'network-error').length;
      lines.push('  requests: provided=' + prov + '  declined=' + dec + '  network-error=' + ne);
      for (const s of out.served.slice(0, 24))
        lines.push('    ' + s.action + '  ' + s.destination + '  ' + s.url.slice(0, 110) +
                   (s.status ? '  ' + s.status + '  ' + s.bytes + 'B' : '') + (s.why ? '  ' + s.why.slice(0, 80) : ''));
      if (out.served.length > 24) lines.push('    … ' + (out.served.length - 24) + ' more');
    }
    if (out.result && out.result.pageErrors)
      lines.push('  pageErrors=' + JSON.stringify(out.result.pageErrors).slice(0, 600));
    for (const e of out.errors) lines.push('  ! ' + e);
    /* THE MACHINE-READABLE SIDE, BESIDE THE IMAGES, so the predicate can read the scalars WITHOUT re-parsing
       a human line — and so a reader holding the directory a week later still has what was true at the run. */
    writeFileSync(join(flags['--out'], 'RASTER.json'), JSON.stringify({
      docId, url: flags['--url'], serve: flags['--serve'] === true, steps,
      stamp, behind, labels, code: out.code, frames: out.frames,
      served: out.served, pageErrors: (out.result && out.result.pageErrors) || null,
    }, null, 1));
    return lines.join('\n');
  },
};

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  const fn = cmd && Object.prototype.hasOwnProperty.call(CMDS, cmd) ? CMDS[cmd] : null;
  if (!fn) {
    console.error('usage: node testing/render_raster.mjs paint --doc <f.html> --url <addr> --out <dir> ' +
                  '[--serve] [--steps N] [--doc-id N] [--glue <qjs.mjs>]');
    process.exit(2);
  }
  const { flags } = parseArgs(rest);
  return fn(flags);
}

/* THE WORK IS BEHIND A GUARD: a `.mjs` whose top level IS the work RUNS when it is imported, and this project
   has already had an agent compile 302 translation units and overwrite the shared tree's artifacts by
   importing a module to see whether it loaded. */
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try { console.log(await main()); }
  catch (e) { console.error(e.stack || e.message || e); process.exit(1); }
}

export { drive, readPaint, pamHeader, pamName, enumsAt, splitPending };
