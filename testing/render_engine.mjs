/* render_engine.mjs — THE ENGINE SIDE OF THE RENDER DIFFERENTIAL: this engine producing a `render-geometry`
 * artifact that testing/render_diff.js's `compare` can join against Chrome's.
 *
 * IT RUNS render_diff.js's ONE COLLECTOR AND HAS NO ARTIFACT SHAPE OF ITS OWN. `collectorSource()` is imported
 * from that file and handed across the ABI verbatim, so what this engine evaluates is the same bytes Chrome
 * evaluates — design constraint (1) over there: a comparator whose two inputs were built by two programs is
 * measuring those programs. Nothing here knows what a row is, what `fields` holds, or which members the
 * artifact carries; it hands over a program, takes back a JSON text, and asks render_diff.js's own
 * `checkArtifactBody` whether what came out is an artifact.
 *
 * WHY THE SEAM IS `qjs_request_dump` AND NOT AN EVALUATION. The host calls an ABI entry BETWEEN two steps,
 * where `qjs_step` has just asserted that the cooperative quantum's slice is closed, the flow stamp is down
 * and the COW capture route is NULL. The interpreter may not be entered on that time: solver/engine.c's
 * `preempt_hook` aborts by name when the scheduler's preempt policy is consulted with no slice open, so an
 * entry that evaluated a program there would die at the first loop back-edge the collector reached. The ask is
 * RECORDED and the SCHEDULER runs it — the same division `qjs_request_park` already makes.
 *
 * ONE RECORD PER TIMELINE, AND THIS DRIVER REFUSES RATHER THAN PICKING ONE. The DOM is per-flow in this engine
 * (CLAUDE.md §State-isolation), so `getBoundingClientRect` has N true answers for N timelines and every record
 * names the WORLD that produced it. A driver that wrote "the" artifact out of several would be the one-slot
 * map solver/engine.c records having measured one layer over, where a page's `w.closed` came back `true` and
 * then `false` out of two contradictory timelines of one document. Name the world, or take them all.
 *
 * IT DRIVES AN ALREADY-BUILT ARTIFACT AND COMPILES NOTHING, and it prints that artifact's stamp with the
 * distance to the branch, for render_geometry_probe.mjs's reason: a stale artifact is an oracle only for paths
 * byte-identical across that span, per path, and the number a reader needs beside any figure is the distance.
 *
 * usage:  node testing/render_engine.mjs collect <out.json> --glue <qjs.mjs> --doc <file.html> [--world <w>]
 *         node testing/render_engine.mjs dump --glue <qjs.mjs> --doc <file.html>
 */

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { abiOperands } from './../engine/renderer_abi.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ORIGIN = 'https://rd.test';
const DOC_URL = ORIGIN + '/doc';

function fail(msg) { throw new Error('@WHY render_engine: ' + msg); }

/* render_diff.js guards its own `main()`, so requiring it does not run it — which is why it guards it, and why
   this may read the ONE collector and the ONE validator rather than keeping a second copy of either. */
const RD = createRequire(import.meta.url)(join(HERE, 'render_diff.js'));

/* ONE PASS, so a flag's VALUE can never be read as a positional — render_diff.js's own spelling, arrived at
   after an `indexOf` parser read a repeated value as a file name. */
const FLAGS_WITH_VALUES = new Set(['--glue', '--doc', '--url', '--world', '--steps']);
function parseArgs(argv) {
  const flags = Object.create(null), positional = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (FLAGS_WITH_VALUES.has(a)) { if (i + 1 >= argv.length) fail(a + ' needs a value'); flags[a] = argv[++i]; }
    else if (a.startsWith('--')) fail('unknown flag ' + a);
    else positional.push(a);
  }
  return { flags, positional };
}

/* THE STAMP, AND WHERE IT WAS READ FROM — a SIDECAR beside the glue and not a property of its bytes, which is
   the shape that has had four lanes on this project decline to measure on the ground that "the artifact
   carries no build stamp" while the artifact their driver loads was stamped the whole time. */
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
                          { cwd: HERE, encoding: 'utf8' }).trim();
  } catch { behind = null; }   /* a checkout without that revision cannot answer, and says so rather than 0 */
  return { sidecar, head: s.head, at: s.at, dirty: s.dirty, behind };
}

/* ONE DOCUMENT DRIVEN, AND THE DUMPS IT PRODUCED.
   THE ORDER IS LOAD-BEARING AND IS THE ENGINE'S, NOT A PREFERENCE. `qjs_request_dump` asserts that the
   frontier has been SEEDED (there is no timeline to run a program in before that) and appends the program to
   the TAIL of every live timeline's sequence — so asking immediately after `qjs_begin` puts the collector
   after every script the document carries, which is where a geometry dump belongs: it reads a laid-out
   document that has already run its own code. Asking LATER would be asking of whichever timelines happened to
   exist at that moment, which is a fact about the driver's timing and not about the page. */
async function drive(gluePath, html, steps) {
  const factory = await import(pathToFileURL(gluePath).href);
  const boot = factory.default ?? factory;
  const M = await boot();
  const cs = (s) => { const n = M.lengthBytesUTF8(s) + 1, p = M._malloc(n); M.stringToUTF8(s, p, n); return p; };
  const str = (f) => String(M.ccall(f, 'string', [], []) ?? '');
  /* THE DOCUMENT CROSSES AS A PAIR, because a zero byte is legal in a document and `strlen` would end the
     parse at the first one. The PROGRAM below crosses as a string for the opposite reason, stated at the ABI
     entry: it is the trusted zone's own instrument text, so its terminator IS its length and a second
     statement of that fact would be free to disagree with it. */
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
  M.ccall('qjs_request_dump', 'void', ['number'], [cs(RD.collectorSource())]);

  /* THE REGISTER IS DRAINED BY THE READ, so it is read every step and accumulated here — a record left
     unread until the end would be lost to any earlier read, and reading it once at the end would be a
     protocol that happens to work only while nothing else reads it. */
  const records = [];
  let code = -1;
  for (let i = 0; i < steps; i++) {
    code = M.ccall('qjs_step', 'number', [], []);
    for (const line of str('qjs_dumps').split('\n').filter(Boolean)) records.push(line);
    if (code === 0) break;
  }
  const res = JSON.parse(str('qjs_result'));
  /* THE FRONTIER IS PARKED BEFORE TEARDOWN, AND THAT IS THE ONLY HONEST EXIT THIS DRIVER HAS. It answers no
     request — a geometry dump is about what the engine computed by ITSELF, and paying a reply would let a
     server's values into it — and a session ended with replies still owed drops every flow parked on one
     together with its continuation, which `qjs_teardown` asserts against by name. Of the three exits that
     assert names, the park is the one that matches what this driver is. The step after it performs it. */
  M.ccall('qjs_request_park', 'void', [], []);
  M.ccall('qjs_step', 'number', [], []);
  M.ccall('qjs_teardown', 'void', [], []);
  return { code, records, result: res };
}

/* ONE RECORD SPLIT — `<world><TAB><json>`. The world is the FIRST field and the JSON is the REMAINDER, which
   is the same rule every other tab-delimited record in this project keeps and for the same reason: only the
   last field may contain the delimiter, and here it provably cannot (value_dump.h). Split on the FIRST tab so
   a future field added in front of the JSON is a loud failure rather than a silently truncated artifact. */
function splitRecord(line) {
  const t = line.indexOf('\t');
  if (t < 0) fail('a dump record carries no TAB — the ABI writes `<world><TAB><json>`, so a record without ' +
                  'one is a world with no artifact or an artifact with no world, and neither can be ' +
                  'attributed: ' + line.slice(0, 120));
  return { world: line.slice(0, t), json: line.slice(t + 1) };
}

function artifactsOf(out, stamp) {
  const seen = new Map();
  for (const line of out.records) {
    const { world, json } = splitRecord(line);
    if (seen.has(world))
      fail('two dump records name the SAME world ' + world + ' — one timeline ran the program once, so a ' +
           'second record under its name is a relay that delivered one answer twice, and keeping either ' +
           'would be choosing between two claims about one document');
    let body;
    try { body = JSON.parse(json); }
    catch (e) { fail('a dump record\'s JSON did not parse (' + (e && e.message) + ') — value_dump.c composes ' +
                     'it and asserts it carries no record separator, so this is that contract broken rather ' +
                     'than a truncation here: ' + json.slice(0, 200)); }
    RD.checkArtifactBody(body, 'the engine dump for world ' + world);
    /* THE DRIVER OWES THE STAMP AND THE COLLECTOR OWED THE BODY — render_diff.js splits the two because the
       page genuinely cannot know which engine is running it. `producer` names the ENGINE and the REVISION it
       was built from, because a row's worth depends on which one made it and "apiclient-engine" alone would
       be a claim about every build there has ever been. */
    body.producer = 'apiclient-engine@' + stamp.head.slice(0, 8);
    body.world = world;
    seen.set(world, body);
  }
  return seen;
}

function stampLines(stamp) {
  return ['  artifact stamp ' + stamp.head + '  (read from ' + stamp.sidecar + ', built ' + stamp.at + ')',
          '    dirty=' + JSON.stringify(stamp.dirty) + '  behind origin/main: ' +
          (stamp.behind === null ? 'UNANSWERABLE in this checkout' : stamp.behind) +
          '   — a stale artifact is an oracle only for paths byte-identical across that span, per path'];
}

async function run(flags) {
  const glue = flags['--glue'];
  if (!glue) fail('--glue must name a built qjs.mjs. This drives an artifact; it does not make one, and a ' +
                  'lane may not build.');
  if (!flags['--doc']) fail('--doc must name an HTML file — this engine is handed a document, it does not ' +
                            'fetch one, and a driver that invented an empty one would report geometry about ' +
                            'a page nobody asked about');
  const steps = flags['--steps'] === undefined ? 400 : Number(flags['--steps']);
  if (!Number.isFinite(steps) || steps <= 0) fail('--steps wants a positive count, got ' + flags['--steps']);
  const stamp = stampOf(glue);
  const out = await drive(glue, readFileSync(flags['--doc'], 'utf8'), steps);
  return { stamp, out, arts: artifactsOf(out, stamp) };
}

const CMDS = {
  /* THE ARTIFACT, WRITTEN — one file, so `render_diff.js compare` has a second side. */
  collect: async (flags, positional) => {
    if (positional.length !== 1)
      fail('usage: render_engine.mjs collect <out.json> --glue <qjs.mjs> --doc <f.html> [--world <w>]');
    const { stamp, out, arts } = await run(flags);
    if (arts.size === 0)
      fail('this document produced NO dump record. The program was appended to every timeline live at ' +
           '`qjs_begin`; a frontier that never reached it answers nothing, which is a fact about the RUN ' +
           '(qjs_step ended ' + out.code + ', 0=DONE 2=YIELD 3=STALLED) and not about the page\'s geometry');
    let world = flags['--world'];
    if (world === undefined) {
      if (arts.size !== 1)
        fail('this document has ' + arts.size + ' live timelines and each answered about its OWN document — ' +
             'name one with --world, because writing "the" artifact out of several is choosing between ' +
             'contradictory claims about one page. The worlds are: ' + [...arts.keys()].join(' '));
      world = [...arts.keys()][0];
    }
    if (!arts.has(world))
      fail('no timeline named ' + world + ' answered. The worlds that did: ' + [...arts.keys()].join(' '));
    const art = arts.get(world);
    writeFileSync(positional[0], JSON.stringify(art, null, 1));
    return ['wrote ' + positional[0] + '  producer=' + art.producer + '  world=' + world + '  ' +
            art.elementCount + ' elements @ ' + art.viewport.width + 'x' + art.viewport.height,
            ...stampLines(stamp),
            '  timelines that answered: ' + arts.size + '   qjs_step ended ' + out.code +
            '   pageErrors=' + JSON.stringify(out.result.pageErrors)].join('\n');
  },
  /* EVERY TIMELINE'S ANSWER, READ — what the engine actually said, before anybody picks one. */
  dump: async (flags, positional) => {
    if (positional.length !== 0) fail('usage: render_engine.mjs dump --glue <qjs.mjs> --doc <f.html>');
    const { stamp, out, arts } = await run(flags);
    const lines = ['render-engine dump', ...stampLines(stamp),
                   '  qjs_step ended ' + out.code + '   (0=DONE 2=YIELD 3=STALLED)',
                   '  _flows=' + JSON.stringify(out.result._flows) +
                   '  pageErrors=' + JSON.stringify(out.result.pageErrors),
                   '  timelines that answered: ' + arts.size];
    for (const [w, a] of arts)
      lines.push('    ' + w + '  ' + a.elementCount + ' elements @ ' + a.viewport.width + 'x' +
                 a.viewport.height + '  url=' + a.url);
    return lines.join('\n');
  },
};

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  const fn = cmd && Object.prototype.hasOwnProperty.call(CMDS, cmd) ? CMDS[cmd] : null;
  if (!fn) {
    console.error('usage: node testing/render_engine.mjs <collect | dump> [args…]');
    process.exit(2);
  }
  const { flags, positional } = parseArgs(rest);
  return fn(flags, positional);
}

/* THE WORK IS BEHIND A GUARD, for render_diff.js's reason and render_geometry_probe.mjs's: a `.mjs` whose top
   level IS the work RUNS when it is imported, and this project has already had an agent compile 302
   translation units and overwrite the shared tree's artifacts by importing a module to see whether it loaded. */
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try { console.log(await main()); }
  catch (e) { console.error(e.stack || e.message || e); process.exit(1); }
}

export { drive, artifactsOf, splitRecord };
