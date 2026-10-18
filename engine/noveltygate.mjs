/* Novelty against the product: `node engine/noveltygate.mjs <binary>`. Each document is its own `--abi` instance
 * (two tabs are two agent clusters); this file plays the zone with extension/lib/novelty-set.js loaded verbatim,
 * absorbing each `@RESULT`'s `_novelty` into the origin's partition and handing it back as `novelty` records.
 * Every other fetch gets a network error so frontiers drain; requests are recorded at Fetch §5.6 step 12.
 * Endpoints, over https://app.test where B adds one endpoint and one header to A's two requests: A, B-alone are
 * handed nothing (every ask paid); B-known A's identities (positive control: paid = |B - A|, knownUnpaid =
 * |B ∩ A|, B's header paid); B-invented lines nothing emitted (negative control); B-stale A's lines under another
 * spelling (refused whole); B-late A's identities in round 1 (later mints unpaid). Drives of uncalled code, over C
 * and D of https://shop.test sharing /bundle.js, D adding its own function, each handed C's identities except C,
 * D-alone (every count 0) and D-invented (negative control: nothing lowered). D-known: the bundle's drives read
 * C's count and all run; D-rank: below D's own code at the same unit count. E-moved: the same bytes at
 * `/static/bundle.js?v=2` give C's identities; F-changed: one appended comment gives none. G-twice: a script
 * compiled twice drives one identity twice, the second lowered by the first. Z-own: the zone hands an instance
 * its partition less its own drives; Z-grew: two of its results between handovers hand each drive once, at the
 * total. Exit 0 when every row holds, 1 when one fails, 3 when none fails and a row's witness was not reached. */
import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createContext, runInContext } from 'node:vm';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { topLevelFactFields } from './top_level_facts.mjs';

const [bin] = process.argv.slice(2);
if (!bin) throw new Error('usage: node engine/noveltygate.mjs <native binary built from engine/host/test_forced.c>');

const EXT = join(dirname(fileURLToPath(import.meta.url)), '..', 'extension');
const ZONE = (() => {
  const sb = { console };
  sb.self = sb;
  sb.globalThis = sb;
  createContext(sb);
  for (const f of ['check.js', 'lib/novelty-set.js'])
    runInContext(readFileSync(join(EXT, f), 'utf8'), sb, { filename: join(EXT, f) });
  return sb;
})();

const ORIGIN = 'https://app.test';
const DOC_A = `<!doctype html><script>
fetch('/api/users', { headers: { 'x-api-key': 'k' } }).catch(function () {});
fetch('/api/items?page=1').catch(function () {});
</script>`;
/* The second document's later requests are issued from the first one's rejection, so a round stands between
   its first mint and the rest (B-late reads that order). */
const DOC_B = `<!doctype html><script>
fetch('/api/users', { headers: { 'x-api-key': 'k', 'authorization': 'Bearer t' } }).catch(function () {
  fetch('/api/items?page=1').catch(function () {});
  fetch('/api/orders').catch(function () {});
});
</script>`;

/* One bundle both documents ship, holding a function nothing calls; D adds an uncalled function of its own. */
const BUNDLE = `function shared() { fetch('/api/shared').catch(function () {}); }
`;
const DOC_C = `<!doctype html><script src="/bundle.js"></script>`;
const DOC_D = `<!doctype html><script src="/bundle.js"></script><script>
function only() { fetch('/api/only').catch(function () {}); }
</script>`;
/* The same bytes at a second address, and changed bytes: the drive key is the script's content, not its URL. */
const DOC_E = `<!doctype html><script src="/static/bundle.js?v=2"></script>`;
const BUNDLE_F = BUNDLE + '// v3\n';
const DOC_F = `<!doctype html><script src="/bundle-f.js"></script>`;
/* A script loaded twice: each load compiles its own body of one function, and the array keeps both alive. */
const TWICE = `(window.keep = window.keep || []).push(function twice() { fetch('/api/twice').catch(function () {}); });
`;
const DOC_G = `<!doctype html><script src="/twice.js"></script><script src="/twice.js"></script>`;

const b64 = (s) => Buffer.from(s).toString('base64');

/* One instance: `boot` is set text handed before the document (or null), `late` set text handed in its first
   round (or null). Resolves with the last @RESULT parsed. */
function run(tag, url, html, boot, late) {
  return new Promise((resolve, reject) => {
    const child = spawn(bin, ['--abi'], { stdio: ['pipe', 'pipe', 'inherit'] });
    const say = (rec) => child.stdin.write(rec + '\n');
    let buf = '', result = null, owed = [], rounds = 0;
    if (boot !== null) say(`novelty\t${b64(boot)}`);
    say(['document', url, `noveltygate-${tag}`, b64('content-type: text/html; charset=utf-8\n'), b64(html),
         ...topLevelFactFields(url, 'none'), '0', 'observed,undriven,unpinned'].join('\t'));
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
      buf += chunk;
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl);
        buf = buf.slice(nl + 1);
        if (line.startsWith('@RESULT ')) { result = JSON.parse(line.slice(8)); continue; }
        if (line.startsWith('@') || line.startsWith('notice\t')) continue;
        if (line.startsWith('fetch\t')) { owed.push(line.split('\t').slice(1)); continue; }
        if (line.startsWith('request\t'))
          return reject(new Error(`[${tag}] the instance asked a host operation this zone does not perform: ${line}`));
        if (line === 'poll' || line === 'stalled') {
          if (rounds++ === 0 && late !== null) say(`novelty\t${b64(late)}`);
          /* METHOD DESTINATION INITIATOR PROVENANCE PINNED DRIVEN CREDENTIALS MODE REFERRER_POLICY REFERRER
             ASKERS URL (solver/engine.h's engine_pending_split); a network error is JSON `null` and no body. */
          for (const f of owed.splice(0)) {
            const abs = new URL(f[11], url).href;
            /* The bundles are the only bodies this zone serves; every other request is a network error. */
            const path = new URL(abs).pathname;
            const js = path === '/bundle.js' || path === '/static/bundle.js' ? BUNDLE
              : path === '/bundle-f.js' ? BUNDLE_F : path === '/twice.js' ? TWICE : null;
            const reply = js !== null
              ? [b64(JSON.stringify({ status: 200, statusText: 'OK', headers: [['content-type', 'text/javascript']],
                                      urlList: [abs], computedType: 'text/javascript', type: 'basic' })),
                 b64(js)]
              : [b64('null'), '-'];
            say(['provide', f[0], f[7], f[8], f[9], f[11], ...reply].join('\t'));
          }
          say('go');
          continue;
        }
        return reject(new Error(`[${tag}] the instance wrote a record this zone does not carry: ${line}`));
      }
    });
    child.on('close', (code, signal) => {
      if (result === null) return reject(new Error(`[${tag}] ended (code=${code} signal=${signal}) with no @RESULT`));
      resolve(result);
    });
  });
}

const kindOf = (nov, kind) => {
  const row = nov.kinds.find((k) => k.kind === kind);
  if (!row) throw new Error(`_novelty.kinds carries no row for ${kind}`);
  return row;
};
const linesOf = (nov, kind) => new Set(nov.identities.filter((l) => l.startsWith(kind + ' ')));
const setText = (spelling, lines) => spelling + '\n' + [...lines].map((l) => l + '\n').join('');

let failed = 0, unreached = 0;
function row(name, ok, what) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name.padEnd(11)} ${what}`);
  if (!ok) failed++;
}
/* A row whose claim needs a witness the run may not produce: with none it is unscored, never a pass. */
function rowReached(name, reached, ok, what) {
  if (reached) return row(name, ok, what);
  console.log(`NOT REACHED  ${name.padEnd(11)} ${what}`);
  unreached++;
}
const census = (nov) => nov.kinds.map((k) => `${k.kind} ${k.paid}/${k.asks} paid, ${k.knownUnpaid} known`).join('; ') +
                        `; known ${nov.known}, own ${nov.own} (${nov.ownKnown} of them known), ` +
                        `refusedTexts ${nov.refusedTexts}, scope ${nov.scope}`;

const A = await run('a', ORIGIN + '/a', DOC_A, null, null);
const a = A._novelty, aEp = kindOf(a, 'endpoint');
row('A', aEp.asks > 0 && aEp.paid === aEp.asks && aEp.knownUnpaid === 0 && a.known === 0 && a.scope === ORIGIN,
    census(a));
ZONE.noveltyAbsorb(ORIGIN, a, new Map());
const known = ZONE.noveltyOwed(ORIGIN, { spelling: null, n: 0 }, new Map());

const B0 = await run('b-alone', ORIGIN + '/b', DOC_B, null, null);
const b0 = B0._novelty, b0Ep = kindOf(b0, 'endpoint');
row('B-alone', b0Ep.paid === b0Ep.asks && b0Ep.knownUnpaid === 0, census(b0) +
    ` — ${b0Ep.paid} paid for ${B0.fetchCallSites.length} recorded endpoint(s), ` +
    `${[...linesOf(b0, 'endpoint')].filter((l) => linesOf(a, 'endpoint').has(l)).length} of them A's`);

const B1 = await run('b-known', ORIGIN + '/b', DOC_B, known, null);
{
  const b1 = B1._novelty, ep = kindOf(B1._novelty, 'endpoint'), hd = kindOf(B1._novelty, 'header');
  const mine = linesOf(b1, 'endpoint'), theirs = linesOf(a, 'endpoint');
  const shared = [...mine].filter((l) => theirs.has(l)).length, unique = mine.size - shared;
  const newHeaders = [...linesOf(b1, 'header')].filter((l) => !linesOf(a, 'header').has(l)).length;
  row('B-known', ep.knownUnpaid === shared && ep.paid === unique && shared > 0 && unique > 0 &&
      ep.paid < B1.fetchCallSites.length && (newHeaders === 0 || hd.paid > 0),
      census(b1) + ` — ${shared} shared and ${unique} unique endpoint identit(ies), ${ep.paid} paid for ` +
      `${B1.fetchCallSites.length} recorded; ${newHeaders} header identit(ies) A did not emit`);
}

{
  const invented = new Set();
  for (let i = 0; i < 64; i++) invented.add('endpoint ' + (BigInt(i) * 0x9e3779b97f4a7c15n).toString(16).padStart(32, 'f'));
  const B2 = await run('b-invented', ORIGIN + '/b', DOC_B, setText(a.spelling, invented), null);
  const ep = kindOf(B2._novelty, 'endpoint');
  row('B-invented', ep.knownUnpaid === 0 && ep.paid === b0Ep.paid && B2._novelty.known === invented.size,
      census(B2._novelty));
}

{
  const B3 = await run('b-stale', ORIGIN + '/b', DOC_B, setText('novelty/0', linesOf(a, 'endpoint')), null);
  const ep = kindOf(B3._novelty, 'endpoint');
  row('B-stale', ep.knownUnpaid === 0 && B3._novelty.refusedTexts === 1 && B3._novelty.known === 0,
      census(B3._novelty));
}

{
  const B4 = await run('b-late', ORIGIN + '/b', DOC_B, null, known);
  const ep = kindOf(B4._novelty, 'endpoint'), ep1 = kindOf(B1._novelty, 'endpoint');
  row('B-late', ep.knownUnpaid > 0 && ep.knownUnpaid < ep1.knownUnpaid, census(B4._novelty) +
      ` — ${ep.knownUnpaid} known-unpaid against ${ep1.knownUnpaid} when handed at boot`);
}

/* The drive rows, under an origin of their own so their partition holds only C's identities. A drive's optimism
   is read off what the engine published at its seed. */
const SHOP = 'https://shop.test';
const opt = (d) => 1 / (1 + d.visits + d.seen);
const dlist = (R) => R._novelty.drives.list;
const dcensus = (R) => `drives ${R._novelty.drives.seeded} seeded, ${R._novelty.drives.lowered} lowered: ` +
  dlist(R).map((d) => `${d.identity.slice(6, 14)} seen ${d.seen} visits ${d.visits}`).join(', ');
const hasEndpoint = (R, path) => R.fetchCallSites.some((c) => c.url.endsWith(path));
const C = await run('c', SHOP + '/c', DOC_C, null, null);
const cDrives = new Set(dlist(C).map((d) => d.identity));
row('C', dlist(C).length > 0 && dlist(C).every((d) => d.seen === 0) && hasEndpoint(C, '/api/shared'), dcensus(C));
ZONE.noveltyAbsorb(SHOP, C._novelty, new Map());
const shopKnown = ZONE.noveltyOwed(SHOP, { spelling: null, n: 0 }, new Map());
{
  const D0 = await run('d-alone', SHOP + '/d', DOC_D, null, null);
  row('D-alone', dlist(D0).every((d) => d.seen === 0), dcensus(D0));
}
{
  const D1 = await run('d-known', SHOP + '/d', DOC_D, shopKnown, null);
  const bundled = dlist(D1).filter((d) => cDrives.has(d.identity));
  const own = dlist(D1).filter((d) => !cDrives.has(d.identity));
  /* Two drives are seeded at two completed-unit counts, and the optimism already differs by those; the rank
     claim is therefore made where the visits agree: a drive of a body the product explored ranks below a drive
     of one it did not, seeded at the same count. */
  const pairs = bundled.flatMap((b) => own.filter((o) => o.visits === b.visits).map((o) => [b, o]));
  row('D-known', bundled.length > 0 && own.length > 0 && bundled.every((d) => d.seen >= 1) &&
      own.every((d) => d.seen === 0) && bundled.every((d) => opt(d) < 1 / (1 + d.visits)) &&
      hasEndpoint(D1, '/api/shared') && hasEndpoint(D1, '/api/only'), dcensus(D1));
  rowReached('D-rank', pairs.length > 0, pairs.every(([b, o]) => opt(b) < opt(o)),
             `${pairs.length} equal-visit pair(s): ` +
             pairs.map(([b, o]) => `${opt(b).toFixed(4)} < ${opt(o).toFixed(4)}`).join(', '));
}
{
  const invented = new Set();
  for (let i = 1; i <= 16; i++)
    invented.add('drive ' + (BigInt(i) * 0x9e3779b97f4a7c15n).toString(16).padStart(32, 'e') + ' 7');
  const D2 = await run('d-invented', SHOP + '/d', DOC_D, setText(C._novelty.spelling, invented), null);
  row('D-invented', dlist(D2).every((d) => d.seen === 0) && D2._novelty.known === invented.size, dcensus(D2));
}
{
  const E = await run('e-moved', SHOP + '/e', DOC_E, shopKnown, null);
  row('E-moved', dlist(E).length > 0 && dlist(E).every((d) => cDrives.has(d.identity) && d.seen >= 1) &&
      hasEndpoint(E, '/api/shared'), dcensus(E));
  const F = await run('f-changed', SHOP + '/f', DOC_F, shopKnown, null);
  row('F-changed', dlist(F).length > 0 && dlist(F).every((d) => !cDrives.has(d.identity) && d.seen === 0) &&
      hasEndpoint(F, '/api/shared'), dcensus(F));
}
{
  const G = await run('g-twice', SHOP + '/g', DOC_G, null, null);
  /* Each repeated identity (the function and the callback its drive creates) reads 0, 1, … in seed order, and
     every later drive is lowered against its own visits and ranks below the first. */
  const by = new Map();
  for (const d of dlist(G)) by.set(d.identity, [...(by.get(d.identity) || []), d]);
  const repeated = [...by.values()].filter((ds) => ds.length > 1);
  rowReached('G-twice', repeated.length > 0,
             repeated.every((ds) => ds.every((d, k) => d.seen === k && (k === 0 ||
               (opt(d) < 1 / (1 + d.visits) && opt(d) < opt(ds[0]))))) && hasEndpoint(G, '/api/twice'),
             dcensus(G) + ` — ${repeated.length} repeated identit(ies)`);
}
{
  /* The zone half of the same count: an instance that reported C's drives is handed none of them back, and a
     fresh instance of the partition is handed every one. */
  const OWN = 'https://own.test', mine = new Map();
  ZONE.noveltyAbsorb(OWN, C._novelty, mine);
  const back = ZONE.noveltyOwed(OWN, { spelling: null, n: 0 }, mine);
  const fresh = ZONE.noveltyOwed(OWN, { spelling: null, n: 0 }, new Map());
  const drives = (t) => t === null ? 0 : t.split('\n').filter((l) => l.startsWith('drive ')).length;
  row('Z-own', drives(back) === 0 && drives(fresh) === cDrives.size && cDrives.size > 0,
      `${drives(back)} drive line(s) handed back to the reporter, ${drives(fresh)} to a fresh instance`);
}
{
  /* Two results of one instance between handovers, its drive counts rising: each identity is handed once, at
     the partition's total, and the reporter is handed none of its own back. */
  const GREW = 'https://grew.test', mine = new Map();
  const twice = { spelling: C._novelty.spelling, identities: C._novelty.identities.map((l) =>
    /^drive [0-9a-f]{32} \d+$/.test(l) ? l.replace(/\d+$/, (n) => String(2 * Number(n))) : l) };
  ZONE.noveltyAbsorb(GREW, C._novelty, mine);
  ZONE.noveltyAbsorb(GREW, twice, mine);
  const back = ZONE.noveltyOwed(GREW, { spelling: null, n: 0 }, mine);
  const fresh = ZONE.noveltyOwed(GREW, { spelling: null, n: 0 }, new Map());
  const lines = (t) => t === null ? [] : t.split('\n').filter((l) => l.startsWith('drive '));
  const want = twice.identities.filter((l) => l.startsWith('drive ')).sort();
  row('Z-grew', lines(back).length === 0 && JSON.stringify(lines(fresh).sort()) === JSON.stringify(want) &&
      want.length > 0,
      `${lines(back).length} drive line(s) handed back to the reporter, ${lines(fresh).length} of ` +
      `${want.length} to a fresh instance`);
}
console.log(failed ? `${failed} row(s) FAILED` : unreached ? `${unreached} row(s) NOT REACHED` : 'every row holds');
process.exit(failed ? 1 : unreached ? 3 : 0);
