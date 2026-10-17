/* NOVELTY AGAINST THE PRODUCT, OVER TWO DOCUMENTS OF ONE APP — `node engine/noveltygate.mjs <binary>`, after
 * `node engine/build.mjs native`.
 *
 * Two documents of https://app.test share two requests and the second adds one endpoint and one header. Each
 * runs as its own `--abi` instance, as two tabs of one app are two agent clusters. This file plays the trusted
 * zone's two novelty duties with extension/lib/novelty-set.js loaded verbatim (the same file bridge.js and
 * trusted.mjs load): absorb every `@RESULT`'s `_novelty` into the cluster origin's partition, and hand the
 * partition to an instance as `novelty` records, at boot or in a round. Every fetch is answered with a network
 * error, so each frontier drains without a network; the requests are recorded at Fetch §5.6 step 12, before
 * the park.
 *
 * Rows, each read off one instance's last `@RESULT` (`_novelty`, `fetchCallSites`):
 *   A            the first document, handed nothing: every endpoint ask paid.
 *   B-alone      the second document, handed nothing: the pre-channel behaviour, every ask paid.
 *   B-known      the second document handed A's identities at boot (positive control): endpoint paid equals
 *                |B's endpoint identities - A's| and knownUnpaid |B's ∩ A's|, both nonzero, and paid is
 *                below B's recorded endpoint count; the header it adds is paid.
 *   B-invented   handed identities nothing emitted (invented negative control): nothing known-unpaid, every
 *                ask paid as in B-alone, and the set's size counted as known.
 *   B-stale      handed A's lines under another spelling: refused whole, nothing known-unpaid.
 *   B-late       handed A's identities in its first round, after its first request was minted: the requests
 *                minted after the round are known-unpaid and the first is paid.
 * Exit status 0 only when every row holds; each row prints what it read.
 */
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
          for (const f of owed.splice(0)) say(['provide', f[0], f[7], f[8], f[9], f[11], b64('null'), '-'].join('\t'));
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

let failed = 0;
function row(name, ok, what) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name.padEnd(11)} ${what}`);
  if (!ok) failed++;
}
const census = (nov) => nov.kinds.map((k) => `${k.kind} ${k.paid}/${k.asks} paid, ${k.knownUnpaid} known`).join('; ') +
                        `; known ${nov.known}, own ${nov.own} (${nov.ownKnown} of them known), ` +
                        `refusedTexts ${nov.refusedTexts}, scope ${nov.scope}`;

const A = await run('a', ORIGIN + '/a', DOC_A, null, null);
const a = A._novelty, aEp = kindOf(a, 'endpoint');
row('A', aEp.asks > 0 && aEp.paid === aEp.asks && aEp.knownUnpaid === 0 && a.known === 0 && a.scope === ORIGIN,
    census(a));
ZONE.noveltyAbsorb(ORIGIN, a);
const known = ZONE.noveltyOwed(ORIGIN, { spelling: null, n: 0 });

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

console.log(failed ? `${failed} row(s) FAILED` : 'every row holds');
process.exit(failed ? 1 : 0);
