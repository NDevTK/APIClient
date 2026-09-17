/* render_geometry_probe.mjs — WHAT THIS ENGINE ANSWERS WHEN `render_diff.js`'s COLLECTOR RUNS IN IT.
 *
 * IT IS NOT THE ENGINE SIDE OF THE DIFFERENTIAL AND MUST NOT BE MISTAKEN FOR ONE. It produces no
 * `render-geometry` artifact and nothing here reaches `compare`; what it produces is OBSERVATIONS about the
 * shape of what the engine answers, which is the question that has to be settled before an artifact can be
 * built at all. The artifact's own residual (testing/render_diff.js, foot of file) names what is missing.
 *
 * IT EXISTS BECAUSE ITS NUMBERS ARE QUOTED. CLAUDE.md's rule is that an instrument whose output anyone quotes
 * is committed in the same diff as the first quotation of its number — a figure whose tool lives in a scratch
 * directory is true, sound, and unreproducible the moment the container is reclaimed, and what survives into
 * the next reader's hands is the digit. Every measurement in `render_diff.js`'s corrected header and in its
 * one-side residual is re-derivable with this file and one already-built artifact.
 *
 * IT DRIVES AN ALREADY-BUILT ARTIFACT AND COMPILES NOTHING, which is what makes it a lane's instrument rather
 * than a build. It speaks the production ABI directly, as engine/route.mjs does, and takes its operand
 * placement from engine/renderer_abi.mjs for that file's reason: two drivers deriving one list two ways is one
 * derivation too many, and the list has gone short three times.
 *
 * THE STAMP IS PRINTED, WITH THE FILE IT WAS READ FROM AND THE DISTANCE TO THE BRANCH. A stale artifact is a
 * valid oracle only for paths byte-identical between its stamp and the revision a claim is about, and a stamp
 * is exactly as reassuring 2403 commits behind as 2 — so the number a reader needs beside any figure is the
 * distance, and this prints it rather than leaving it to be remembered. It does NOT decide whether the
 * distance is acceptable: that is a per-path question about the claim being made, and only the reader making
 * the claim holds it.
 *
 * EVERY WITNESS IN EVERY PROBE DOCUMENT BELOW CARRIES CONSTANTS ONLY. A witness composed from anything this
 * engine computed can itself become concolic, at which point the act is never performed and its absence reads
 * as the path not being taken — the more successfully the engine does the thing being measured, the less
 * likely such a witness is to fire. The channel is `qjs_pending`, read by this driver; it is deliberately not
 * the console, which the renderer does not tee and which renders an object as `[object]` in any case.
 *
 * ONE CASE HERE IS A REFUTATION RATHER THAN AN OBSERVATION, AND IT IS THE REASON THE ARTIFACT IS SERIALISED
 * BY THE HOST. `stringify` asks whether the PAGE can serialise its own artifact — the smaller-looking route,
 * and the one a reader reaches for first. It cannot: `JSON.stringify(ART)` reads `ART.toJSON`, which is a
 * member a published record does not hold and is therefore unknown INPUT (solver/absent.h), so ECMAScript
 * §25.5.4.2 SerializeJSONProperty step 2.b's `IsCallable` is a branch on a concolic and the flow FORKS — and the unknown carries no example, so
 * the machine derives a String with no text behind it. Both halves are in this case's own output: `_flows` 2,
 * a `_forkAt` entry naming that site by its operands, and `validValues` `[]` for the stringified artifact.
 *
 * usage:  node testing/render_geometry_probe.mjs --glue <path/to/qjs.mjs> [--case <name> | --doc <file.html>]
 *         node testing/render_geometry_probe.mjs --glue <…> --list
 */

import { readFileSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';
import { abiOperands } from './../engine/renderer_abi.mjs';

const ORIGIN = 'https://rd.test';
const DOC_URL = ORIGIN + '/doc';

function fail(msg) { throw new Error('@WHY render_geometry_probe: ' + msg); }

/* ONE PASS, so a flag's VALUE can never be read as a positional — the spelling render_diff.js arrived at after
   an `indexOf` parser read a repeated value as a file name. */
const FLAGS_WITH_VALUES = new Set(['--glue', '--case', '--doc', '--steps']);
function parseArgs(argv) {
  const flags = Object.create(null);
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (FLAGS_WITH_VALUES.has(a)) { if (i + 1 >= argv.length) fail(a + ' needs a value'); flags[a] = argv[++i]; }
    else if (a === '--list') flags[a] = true;
    else fail('unknown argument ' + a);
  }
  return flags;
}

/* THE STAMP, AND WHERE IT WAS READ FROM. Four lanes on this project have declined to measure on the ground
   that "the artifact carries no build stamp" while the artifact the driver loads was stamped the whole time —
   two artifacts of one build, one stamped and one not, is the shape that produces that. So this names the
   file. The stamp is a SIDECAR rather than a property of the bytes, which is why the pairing is stated as
   "the sidecar beside this glue" and not as a fact about the wasm. */
function stampOf(gluePath) {
  const sidecar = gluePath + '.build.json';
  if (!existsSync(sidecar))
    fail('no build stamp beside ' + gluePath + ' (expected ' + sidecar + '). A number taken from an artifact ' +
         'that names no revision belongs to no revision, and reporting one is worse than reporting none.');
  const s = JSON.parse(readFileSync(sidecar, 'utf8'));
  if (typeof s.head !== 'string' || s.head.length === 0)
    fail(sidecar + ' names no head — an unstamped stamp is the thing it exists to prevent');
  let behind = null;
  try {
    behind = execFileSync('git', ['rev-list', '--count', s.head + '..origin/main'],
                          { cwd: dirname(new URL(import.meta.url).pathname), encoding: 'utf8' }).trim();
  } catch { behind = null; }   /* a checkout without that revision cannot answer, and says so rather than 0 */
  return { sidecar, head: s.head, at: s.at, dirty: s.dirty, cone: s.cone, behind };
}

/* THE PROBE DOCUMENTS. Each is one question. `%COLLECTOR%` is substituted with render_diff.js's own
   `collectorSource()`, so what runs here is the same bytes Chrome runs — the file's design constraint (1),
   which is the whole reason the collector is shipped as source. */
const CASES = {
  answers: {
    ask: 'does CSSOM VIEW §6 "Extensions to the Element Interface"\'s getBoundingClientRect answer over a ' +
         'laid-out document — and are the used values right',
    html: `<!doctype html><html><body><div id=a style="width:100px;height:50px">hello</div><script>
fetch("/rd-enter");
var r = document.getElementById("a").getBoundingClientRect();
fetch("/rd-gbcr-returned");
fetch("/rd-x=" + r.x); fetch("/rd-y=" + r.y); fetch("/rd-w=" + r.width); fetch("/rd-h=" + r.height);
var e = document.documentElement.getBoundingClientRect();
fetch("/rd-htmlh=" + e.height);
var b = document.body.getBoundingClientRect();
fetch("/rd-bodyw=" + b.width);
<\/script></body></html>`,
  },
  guard: {
    ask: 'does a `typeof` on a rectangle member fork — the branch the collector used to carry',
    html: `<!doctype html><html><body><div id=a style="width:100px;height:50px">x</div><script>
var fixed = document.getElementById("a").getBoundingClientRect();
if (typeof fixed.width === "number") { fetch("/rd-FIXED-number"); } else { fetch("/rd-FIXED-notnumber"); }
<\/script></body></html>`,
  },
  guardauto: {
    ask: 'the same `typeof`, on a member whose used value derives from the initial containing block',
    html: `<!doctype html><html><body><div id=a>x</div><script>
var auto = document.body.getBoundingClientRect();
if (typeof auto.width === "number") { fetch("/rd-AUTO-number"); } else { fetch("/rd-AUTO-notnumber"); }
<\/script></body></html>`,
  },
  pageobject: {
    ask: 'can page script read back a value it just computed — the reason a page cannot serialise this artifact',
    html: `<!doctype html><html><body><script>
var o = {a: "lit", n: 7};
fetch("/rd-o=" + o.a + "/" + o.n);
<\/script></body></html>`,
  },
  stringify: {
    ask: 'can the PAGE serialise the artifact — the route a reader reaches for before the host-side dump',
    html: `<!doctype html><html><body>
<div id=a style="width:100px;height:50px">hello</div><p>para</p>
<script>
fetch("/rd-enter");
var ART = %COLLECTOR%;
fetch("/rd-collector-returned");
var S = JSON.stringify(ART);
fetch("/rd-stringified");
fetch("/rd-typeof=" + (typeof S));
fetch("/rd-len=" + (S ? S.length : -1));
fetch("/rd-head=" + S);
fetch("/rd-done");
<\/script></body></html>`,
  },
  collector: {
    ask: 'does render_diff.js\'s own COLLECTOR run verbatim in this engine, and what does it return',
    html: `<!doctype html><html><body>
<div id=a style="width:100px;height:50px">hello</div><p>para</p>
<script>
fetch("/rd-enter");
var ART = %COLLECTOR%;
fetch("/rd-collector-returned");
fetch("/rd-artifact=" + ART.artifact + "/v" + ART.version + "/n" + ART.elementCount);
for (var i = 0; i < ART.rows.length; i++) {
  var R = ART.rows[i];
  fetch("/rd-row/" + R.key + "/" + R.tag);
}
fetch("/rd-done");
<\/script></body></html>`,
  },
};

function collectorSource() {
  /* render_diff.js guards its own `main()`, so requiring it does not run it — which is exactly why it guards
     it, and why this may read the ONE collector rather than keeping a second copy of it. A second copy is the
     drift the whole artifact is built to prevent: a comparator whose two inputs were built by two programs is
     measuring those programs. */
  return createRequire(import.meta.url)(join(dirname(new URL(import.meta.url).pathname), 'render_diff.js'))
    .collectorSource();
}

async function drive(gluePath, html, steps) {
  const factory = await import(pathToFileURL(gluePath).href);
  const boot = factory.default ?? factory;
  const M = await boot();
  const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
  const str = (f) => String(M.ccall(f, 'string', [], []) ?? '');
  /* THE DOCUMENT CROSSES AS A PAIR, because a zero byte is legal in a document and `strlen` would end the
     parse at the first one. The guard byte is what the extra allocation has always been for. */
  const bs = (b) => {
    const u8 = new TextEncoder().encode(b);
    const p = M._malloc(u8.length + 1);
    M.HEAPU8.set(u8, p); M.HEAPU8[p + u8.length] = 0;
    return [p, u8.length];
  };
  const operands = abiOperands('Init', 'qjs_init', {
    document: bs(html), url: DOC_URL, docId: 'rd', headers: '', topLevelUrl: DOC_URL,
    inheritedCsp: '', inheritedCspSelfOrigin: '',
    inheritedCoep: 'unsafe-none', inheritedCoepEndpoint: '',
    inheritedCoepReportOnly: 'unsafe-none', inheritedCoepReportOnlyEndpoint: '',
    parentNavigable: 'u', containerPolicy: 'null', ancestorOrigins: 'none', creationSandboxFlags: 'none',
  }, cs);
  M.ccall('qjs_init', 'number', operands.map(() => 'number'), operands);
  M.ccall('qjs_begin', 'void', ['number'], [cs('')]);

  /* `qjs_pending` is a SET keyed on the (method, URL) pair and is re-reported every step, so accumulating it
     across the pump loses nothing and a URL seen once is a request made once. */
  const seen = new Map();
  let code = -1;
  for (let i = 0; i < steps; i++) {
    code = M.ccall('qjs_step', 'number', [], []);
    for (const line of str('qjs_pending').split('\n').filter(Boolean)) {
      const t = line.split('\t');
      if (t.length !== 7) fail('a pending line is not the ABI\'s seven tab-separated fields: ' + line);
      if (!seen.has(t[6])) seen.set(t[6], t[0]);
    }
    if (code === 0) break;
  }
  const res = JSON.parse(str('qjs_result'));
  /* THE FRONTIER IS PARKED BEFORE TEARDOWN, AND THAT IS NOT TIDINESS — IT IS THE ONLY HONEST EXIT THIS DRIVER
     HAS. This probe DELIBERATELY answers no request: the requests ARE the measurement, and paying one would
     let a reply's own values into a run whose whole subject is what the engine computed by itself. A session
     ended with replies still owed drops every flow parked on one together with its continuation, so
     `qjs_teardown` asserts against it by name — "Provide them, step to DONE, or park the frontier" — and it
     fired here on the first run of this file, which is the engine refusing a driver rather than a defect in
     it. Of the three exits the assert names, the park is the one that matches what this driver is: a zone
     that will not spend the act. The step after it is what performs the park. */
  M.ccall('qjs_request_park', 'void', [], []);
  M.ccall('qjs_step', 'number', [], []);
  M.ccall('qjs_teardown', 'void', [], []);
  return { code, requests: [...seen].map(([u, m]) => m + ' ' + u), result: res };
}

function report(name, c, stamp, out) {
  const lines = [];
  lines.push('render-geometry probe  case=' + name);
  lines.push('  asks: ' + c.ask);
  lines.push('  artifact stamp ' + stamp.head + '  (read from ' + stamp.sidecar + ', built ' + stamp.at + ')');
  lines.push('    dirty=' + JSON.stringify(stamp.dirty) + '  cone=' + JSON.stringify(stamp.cone) +
             '  behind origin/main: ' + (stamp.behind === null ? 'UNANSWERABLE in this checkout' : stamp.behind) +
             '   — a stale artifact is an oracle only for paths byte-identical across that span, per path');
  lines.push('  qjs_step ended ' + out.code + '   (0=DONE 2=YIELD 3=STALLED)');
  lines.push('  requests the engine made (the witness channel — every URL below is a literal in the document,');
  lines.push('  so a shape in one is the ENGINE\'s rendering of a concolic and not a composed payment):');
  for (const r of out.requests.sort()) lines.push('    ' + r);
  lines.push('  _flows=' + JSON.stringify(out.result._flows) +
             '  _switches=' + JSON.stringify(out.result._switches) +
             '  pageErrors=' + JSON.stringify(out.result.pageErrors));
  /* THE FORK CENSUS IS THE ONE ROW THAT NAMES A BRANCH BY ITS OWN OPERANDS, so it is printed whole rather
     than counted: a count says how many forks, and the key says WHICH PREDICATE over WHICH source. */
  lines.push('  _forkAt=' + JSON.stringify(out.result._forkAt));
  /* AND THE EXAMPLES, WHICH IS WHERE A CONCOLIC'S CONCRETE VALUE ACTUALLY REACHES A HOST TODAY. It is printed
     to show that the value EXISTS and that this route is text-only — it cannot tell the Number 0 from the
     String "0" — which is why it is not the channel an artifact may use. */
  lines.push('  fetchCallSites=' + JSON.stringify(out.result.fetchCallSites));
  return lines.join('\n');
}

async function main() {
  const flags = parseArgs(process.argv.slice(2));
  if (flags['--list']) {
    return Object.keys(CASES).map((k) => k + '  — ' + CASES[k].ask).join('\n');
  }
  const glue = flags['--glue'];
  if (!glue) fail('--glue must name a built qjs.mjs. This drives an artifact; it does not make one, and a ' +
                  'lane may not build. Preserved builds are under /tmp/apiclient-frozen/ARTIFACT-<rev>/.');
  const stamp = stampOf(glue);
  const steps = flags['--steps'] === undefined ? 400 : Number(flags['--steps']);
  if (!Number.isFinite(steps) || steps <= 0) fail('--steps wants a positive count, got ' + flags['--steps']);

  if (flags['--doc']) {
    const html = readFileSync(flags['--doc'], 'utf8');
    const c = { ask: 'the document at ' + flags['--doc'], html };
    return report('doc', c, stamp, await drive(glue, html, steps));
  }
  const names = flags['--case'] ? [flags['--case']] : Object.keys(CASES);
  for (const n of names) if (!CASES[n]) fail('no such case ' + n + ' — `--list` names them');
  const out = [];
  for (const n of names) {
    const c = CASES[n];
    const html = c.html.replace('%COLLECTOR%', () => collectorSource());
    out.push(report(n, c, stamp, await drive(glue, html, steps)));
  }
  return out.join('\n\n');
}

/* THE WORK IS BEHIND A GUARD, for render_diff.js's reason one directory over: a `.mjs` whose top level IS the
   work is a file that RUNS when it is imported, and this project has already had an agent compile 302
   translation units and overwrite the shared tree's artifacts by importing a module to see whether it loaded.
   Guarded, `CASES` and `drive` are readable and reusable without booting anything. */
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try { console.log(await main()); }
  catch (e) { console.error(e.stack || e.message || e); process.exit(1); }
}

export { CASES, drive, stampOf };
