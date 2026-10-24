/* THE LEVEL-1 ORDER'S DRIVER — the trusted-zone sibling of `engine/route.mjs`, and the only thing that can
 * ask this scheduler a question at all.
 *
 * `node engine/level1.mjs`. It compiles nothing and links nothing: it loads `extension/bridge.js` verbatim
 * into a realm of its own and drives the shipped walk and the shipped record over populations it composes.
 *
 * WHY THIS FILE EXISTS, WHICH IS A MEASUREMENT GAP AND NOT A TEST GAP. §scheduler splits the WFQ in two.
 * Level-2 — the flows inside one document — is composed by the ENGINE, so its census rides the result
 * document and every gate that reads a result reads it. Level-1 is `engineKey` per HOT INSTANCE against
 * `level1Key` per waiting address, declared route and cold row, and NO ENGINE CAN SEE ANOTHER ENGINE:
 * this order is composed entirely in `bridge.js`, so no result document can ever carry it and no corpus run
 * touches it. test262 links five quickjs sources and not one line of this zone; WPT judges by a browser
 * oracle; solvergate compares a document against itself through one host. The Level-1 order is outside all
 * three by construction.
 *
 * WHAT THAT COST, MEASURED RATHER THAN SUSPECTED. Both Level-1 defects found this session were RANKS FROZEN
 * AT A CONSTANT. The unserved weight was the literal 1.0 for every waiting document however
 * many times its address had already been admitted, fetched and found to demonstrate nothing — so an
 * address-varying cycle re-entered at the same rank as a page nobody had ever opened, for ever, and there was
 * no rank for a starvation to be expressed in. Beside it, a cold row excluded from the walk because a live
 * document held its address had its weight leave the order entirely while the item standing for it answered
 * that same constant. Neither was visible to anything. A third would be equally invisible today: the census
 * `_level1Record` writes can be READ (the popup renders it), and until this file the walk that produces it
 * could not be DRIVEN — so a row could go wrong in a direction no live session distinguishes from a quiet one.
 *
 * IT READS THE SHIPPED RECORD, NEVER A HARNESS-PRIVATE ONE. §Testing: "MEASURE WHAT THE SHIPPED PATH WRITES,
 * NOT WHAT A HARNESS PRINTS" — a marker a driver prints and production never emits reads zero for a session
 * that learned a thousand endpoints. Everything checked below is read off `self._level1`, which is the exact
 * object `_level1Record` publishes for `popup.js`, written by the exact call the round's `finally` makes.
 *
 * AND IT STORES NO EXPECTED OUTPUT, WHICH IS WHAT KEEPS IT FROM BEING A CHANGE DETECTOR. §Testing names a
 * per-document expected-emission list as the banned shape twice over — a change detector, and a second
 * hand-maintained copy of a declaration that already exists. Every check here is a RELATION: between rows of
 * ONE record (a spread exists exactly over a non-empty population; the populations account for the total), or
 * between TWO records taken over two populations that differ in one stated way (the same tie, once because
 * every candidate is unserved and once because two served candidates rank equal — which is the pair the
 * census exists to keep apart). A walk that ranks MORE, or a weight whose formula improves, passes unchanged;
 * what fails is the instrument losing the ability to tell two states apart.
 *
 * THE THREE FACTS IT REFUSES TO LET COLLAPSE are `_level1Record`'s own contract and are driven separately:
 * the order NOT ASKED (`cands` absent — the round spent its advance on a navigation, or a reservation held
 * admission), the order ASKED AND EMPTY (`cands: 0` with the exclusion counts beside it saying what was taken
 * out of it), and a FULL READING. A driver that collapsed them would measure less than the instrument
 * reports, which is the defect one layer up from the ones it is here to catch.
 */
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createContext, runInContext } from 'node:vm';
import { loadZone } from './zone.mjs';

const ENGINE = dirname(fileURLToPath(import.meta.url));

/* THE ZONE, LOADED VERBATIM INTO A CONTEXT OF ITS OWN — `engine/trusted.mjs`'s construction, one file over,
   for the same reason: evaluating `check.js` over THIS realm would install `DCHECK`/`CHECK` into a driver
   whose own code does not expect them, and a driver silently gaining assert machinery is a difference between
   what it runs and what production runs.
   THE LOAD LIST IS `ast-worker.html`'s, NARROWED TO WHAT THE LEVEL-1 ORDER REACHES, and the narrowing is a
   claim this load itself checks rather than a guess: `bridge.js` calls `_isRealOrigin` — safe-fetch.js's own
   predicate, shared rather than restated — to tell a sub-frame from a top-level document, and if the walk
   ever reaches a global neither file installs, this load throws at that line instead of answering. That is
   the forcing function and not a fragility: the alternative is stubbing whatever is missing, which is how a
   driver comes to exercise a program the extension does not run.
   `module` IS WHAT OPENS THE DOOR. In `ast-worker.html` there is no `module`, so the export block at the foot
   of `bridge.js` does not run and this zone adds nothing to what ships. */
/* This realm has no IndexedDB, so the zone's load-time restores (the egress table, the third-party-cookie
   setting) reject through frontierFail's DFAIL on the first turn of the event loop. They are not the Level-1
   order; each is counted and named, and any other rejection still kills the run. */
let restoresRefused = 0;
process.on('unhandledRejection', (e) => {
  if (e && e.apiclientFatal && /IndexedDB preference read failed \(indexedDB is not defined\)/.test(e.message)) {
    restoresRefused++;
    return;
  }
  throw e;
});
/* The browser's registrable domains for every host this driver names, declared here because this realm has no
   service worker to ask chrome.publicSuffix. lib/site.js's own siteAsk fills its memo through the `swRpc` below,
   which answers `site.registrableDomains` from this table and nothing else; a host absent from it is refused, so
   a fixture naming a new host fails at its ask rather than being given a site nobody declared. */
const SITE_TABLE = Object.freeze({
  'a.test': 'a.test', 'b.test': 'b.test', 'c.test': 'c.test', 'd.test': 'd.test', 'e.test': 'e.test',
  'm.test': 'm.test', 'n.test': 'n.test', 'p.test': 'p.test', 'x.test': 'x.test', 'z.test': 'z.test',
});
const SITE_TABLE_VERSION = 'level1-driver-table/1';
async function swRpc(method, hosts) {
  if (method !== 'site.registrableDomains') throw new Error('this driver answers no service-worker call `' + method + '`');
  const domains = {};
  for (const h of hosts) {
    if (!(h in SITE_TABLE)) throw new Error('the driver was asked the site of `' + h + '`, which SITE_TABLE does not declare');
    domains[h] = SITE_TABLE[h];
  }
  return { version: SITE_TABLE_VERSION, domains };
}
const ZONE = (() => {
  /* `_globalStoreReady` is offscreen-brain.js's read of the cumulative store, which this realm does not load;
     it never settles here, so bridge.js's start-up kick never drives the live pool under these rows. */
  const sandbox = { console, fetch, URL, TextDecoder, TextEncoder, setTimeout, clearTimeout, MessageChannel, swRpc,
                    _globalStoreReady: new Promise(() => {}) };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.module = { exports: {} };
  createContext(sandbox);
  loadZone(sandbox, ['bridge.js']);
  return sandbox;
})();
const B = ZONE.module.exports;
/* A block of service, in pulls (quanta of work, which is the unit the account states), so every fixture states a
   count; one slice of the mock drive is one pull. */
const PT = 100;
const SLICE = 1;
/* Every host the driver ranks is asked first, as each production entry point asks before its member joins. */
await ZONE.siteAsk(Object.keys(SITE_TABLE));
for (const n of ['hostSchedule', 'engineKey', 'engineRecordFacts', 'level1Index', 'level1Tree', 'level1Key', '_bestCandidate',
                 '_candPopulation', '_level1Record', '_candCensus', 'CAND_KINDS', 'CAND_SPREAD'])
  if (B[n] === undefined)
    throw new Error('extension/bridge.js did not export `' + n + '` — this driver is the only reader of the ' +
                    'Level-1 order, so an export that has gone means that half of the order is unaskable ' +
                    'again and the gap this file exists to close has silently reopened');

/* ─── THE LEDGER ────────────────────────────────────────────────────────────────────────────────────────
   A failure NAMES the relation it broke and the reading that broke it, and the run continues — one broken
   relation must not hide the next, because the whole subject here is a census going quiet. */
let checks = 0, failures = 0;
function must(cond, relation, reading) {
  checks++;
  if (cond) return;
  failures++;
  console.log('[level1] BROKEN  ' + relation);
  if (reading !== undefined) console.log('[level1]          reading: ' + JSON.stringify(reading));
}
/* THE ROWS A CENSUS THAT RAN CARRIES, TAKEN FROM THE ZONE'S OWN DECLARATION rather than restated here — a
   second copy would go stale in the direction that matters, silently agreeing with a walk that dropped a row.
   `_candCensus()` is `bridge.js`'s one statement of which rows a walk that RAN produces. */
const COUNT_ROWS = Object.keys(B._candCensus());
const SPREAD_ROWS = B.CAND_SPREAD.map((s) => s.row);
const KIND_POP = B.CAND_KINDS.map((k) => k.pop);
/* A KIND'S ROWS ARE REACHED THROUGH THE TABLE AND NEVER SPELLED HERE, and that is the same discipline the
   producer keeps rather than a way around it: `_candRanked` writes `cen[d.pop]` and `cen[d.wMax]` out of
   CAND_KINDS, so those three row names exist in exactly one place in this system and a driver that restated
   one would be the second copy — able to drift silently in the direction that agrees with a walk that had
   stopped writing the row. (The SPREAD rows are the opposite case by deliberate design: each is written with
   a LITERAL key at its own site, because a computed key is a write no grep and no gate can see, so those are
   spelled out above.) */
const kindOf = (kind) => {
  const d = B.CAND_KINDS.find((k) => k.kind === kind);
  if (d === undefined) throw new Error('this driver asked for a candidate kind `' + kind + '` that ' +
                                       'bridge.js does not declare — CAND_KINDS is the one statement of ' +
                                       'which work items have no instance, and a driver naming a kind ' +
                                       'outside it is measuring an order that does not exist');
  return d;
};
const kindPop = (cen, kind) => cen[kindOf(kind).pop];
const kindWMax = (cen, kind) => cen[kindOf(kind).wMax];

/* ─── FIXTURES ──────────────────────────────────────────────────────────────────────────────────────────
   The four halves of a population, built as the shapes the walk reads and nothing more. A cold row is what
   `frontierRow` projects out of a stored entry (the ranking view is deliberately not the cold tier); a
   waiting job is what `astDispatch` parks; a resident record is an engine holding an address and a frontier
   key; a seed is an address an application declared is a page of itself. */
/* Every member states its principal and whether its load carried the session (documentIdentityOf); a fixture
   names the address's own origin and a credentialed load unless it says otherwise. */
const originOfUrl = (url) => new URL(url).origin;
const job = (url, o = {}) => ({ msg: { sourceUrl: url, frameId: o.frameId || 0, origin: o.origin || originOfUrl(url),
                                       credentialed: o.credentialed !== false } });
const row = (key, url, value, pulls, o = {}) =>
  ({ key, sourceUrl: url, origin: o.origin || originOfUrl(url), credentialed: o.credentialed !== false,
     value, pulls, at: 1, shed: false, stranded: o.stranded === true, crashed: o.crashed === true,
     rest: o.rest === undefined ? null : o.rest, bytes: 0, rederivable: true });
/* The key a walk over `P` gives the document at `url` with account `acct`, read through the shipped functions. */
const keyOf = (P, url, acct) => B.level1Key(B.level1Tree(P.resident, P.idx), url, acct, true);
/* An instance as hostSchedule and engineKey read one: a hot record with an address and a stated account. */
const engM = (url, paid, pulls, o = {}) =>
  Object.assign({ state: 'hot', msg: { sourceUrl: url, credentialed: true }, origin: originOfUrl(url), prior: null,
                  _readyP: null, level1: { runnable: true, paid: paid, pulls: pulls,
                                           acct: { value: paid, pulls: pulls, at: 1 } } }, o);
const eng = (url, fkey, o = {}) => ({ state: 'hot', msg: { sourceUrl: url, credentialed: o.credentialed !== false },
                                      origin: o.origin || originOfUrl(url), fkey: fkey || ('k|' + url) });
const idxOf = (...rows) => new Map(rows.map((r) => [r.key, r]));
/* A declaration is keyed by the identity its load would have; `seedOf` names the principal and session state. */
const seedOf = (a, origin, credentialed) => {
  const identity = B.documentIdentityOf(a, origin, credentialed);
  return [identity, { url: a, identity, principalUrl: a, principalOrigin: origin, provenance: 'derived' }];
};
const seedsOf = (...addrs) => new Map(addrs.map((a) => seedOf(a, originOfUrl(a), true)));
const pop = (o = {}) => ({ resident: o.resident || [], waiting: o.waiting || [],
                           seeds: o.seeds || new Map(), idx: o.idx || new Map() });

/* A ROUND, DRIVEN THROUGH THE SHIPPED RECORD AND READ BACK OFF `self`. `hot` is the resident half the round's
   scan produces; `cand` is what the walk answered, or the POSITIVE null that says the round never asked. */
function record(poolArr, hot, census, asks) {
  ZONE._level1 = undefined;
  B._level1Record(poolArr, { hot: hot, cand: census === undefined ? null : census, candAsk: asks || 0,
                            shape: 'waited', stepCode: null });
  return ZONE._level1;
}
/* THE PRESENCE RULE, ASSERTED AS THE ONE RULE IT IS. §_level1Record: a COUNT is present whenever the walk
   that produces it ran, because a count of zero is a reading; a WEIGHT is present only over SERVED members,
   because an extremum over nothing is not a number and a never-served member weighs +Infinity, which is
   counted in `candUnserved` instead. Checked over the declarations rather than over a list of names. */
function checkPresence(r, asked) {
  must(('cands' in r) === asked,
       'the total is present exactly when the order was asked', r);
  must(('candAsk' in r) === asked,
       'the ask count travels with the reading it qualifies', r);
  for (const k of COUNT_ROWS)
    must((k in r) === asked,
         'the count row `' + k + '` is present exactly when the walk ran (a zero is a READING)', r);
  const ranked = asked && r.cands > r.candUnserved;
  for (const s of SPREAD_ROWS)
    must((s in r) === ranked,
         'the spread row `' + s + '` exists exactly over a ranked set with a served member', r);
  for (const k of B.CAND_KINDS)
    must(!(k.wMax in r) || (asked && r[k.pop] > 0),
         'the extremum `' + k.wMax + '` exists only over a non-empty `' + k.pop + '`', r);
}

console.log('[level1] ── the one index ──');
{
  const U = B.LEVEL1_UNSERVED, acct = (value, pulls) => ({ value, pulls, at: 1 });
  must(B.level1Index(U, 0) === Infinity && B.level1Index(U, 5 * PT) === Infinity,
       'a document never served indexes +Infinity whatever the total, so it is handed a quantum first');
  const a = acct(4, 2 * PT), b = acct(4, 4 * PT), c = acct(8, 2 * PT);
  const N = 10 * PT;
  must(Number.isFinite(B.level1Index(a, N)) && B.level1Index(a, N) > B.level1Index(b, N),
       'at equal value, more service ranks lower: both the rate and the bound fall with service');
  must(B.level1Index(c, N) > B.level1Index(a, N),
       'at equal service, more novel value ranks higher');
  must(B.level1Index(a, 2 * N) > B.level1Index(a, N),
       'the bound grows with all service, so a document left unserved while others run regains rank');
  must(B.level1Index(a, N) >= a.value / a.pulls && B.level1Index(acct(3, 0.5), 1) === 6,
       'the index is an upper bound on the rate, and is the rate itself where the parent has served one pull');
  /* The Poisson divergence at the answer is the threshold, to the double. */
  const lam = 0.3, beta = 0.05, q = B.level1KlUcb(lam, beta);
  must(Math.abs(q - lam + lam * Math.log(lam / q) - beta) < 1e-12 && q > lam,
       'the bound solves P d(v / P, q) = ln N', { q });
}

console.log('[level1] ── the order carries no unit ──');
{
  /* The engine states pulls as its work over ENGINE_QUANTUM_WORK (main.c's qjs_level1_account), so a work unit
     k times finer multiplies both and states the same pulls; the order is then a function of counts alone. */
  const state = [['https://a.test/', 4, 23 * 54000], ['https://a.test/x', 1, 9 * 54000],
                 ['https://b.test/', 9, 80 * 54000], ['https://c.test/', 0, 3 * 54000]];
  const orderAt = (k) => {
    const idx = idxOf(...state.map(([u, v, w], i) => row('k' + i, u, v, (w * k) / (54000 * k))));
    const tree = B.level1Tree([], idx);
    const ks = state.map(([u], i) => ({ u, key: B.level1Key(tree, u, idx.get('k' + i), true, B.LEVEL1_DOOR.cold) }));
    ks.sort((x, y) => (B.level1Ahead(x.key, y.key) ? -1 : B.level1Ahead(y.key, x.key) ? 1 : 0));
    return ks.map((x) => x.u + ' ' + x.key.siteW + ' ' + x.key.docW).join(' | ');
  };
  must(orderAt(1) === orderAt(7) && orderAt(1) === orderAt(1000) && orderAt(1) === orderAt(1 / 64),
       'the same state ranks identically, to the last digit, at work units 1, 7, 1000 and 1/64 times as fine',
       orderAt(1));
}

console.log('[level1] ── a barren document falls below a productive one within ln N / r pulls ──');
{
  /* A sibling serving at rate r indexes at least r; a barren one indexes ln N / P exactly (the Poisson bound at
     zero), so it is outranked once P > ln N / r and not before. */
  const N = 1000, r = 0.5, bound = Math.log(N) / r;
  const barren = (P) => B.level1Index({ value: 0, pulls: P, at: 1 }, N);
  const productive = B.level1Index({ value: r * 50, pulls: 50, at: 1 }, N);
  must(barren(Math.ceil(bound)) < productive && barren(Math.ceil(bound)) < r &&
       barren(Math.floor(bound)) > r,
       'a barren document with ln N / r pulls is the crossing: just past it it is outranked by any sibling of rate ' +
       'r, just before it it is not', { bound, at: barren(Math.ceil(bound)), before: barren(Math.floor(bound)) });
}

console.log('[level1] ── value the product has absorbed stops predicting ──');
{
  /* An account credited at product size 10 and read at 40 keeps its rate and loses three quarters of its
     counts, so its bound widens: a document whose finds the product has since absorbed from elsewhere is
     explored again rather than trusted. */
  const a = { value: 30, pulls: 60, at: 10 }, now = B.level1At(a, 40);
  must(now.value === 7.5 && now.pulls === 15 && now.at === 40 &&
       B.level1Index(now, 400) > B.level1Index(B.level1At(a, 10), 400),
       'growth of the product scales value and pulls alike, so the rate holds and the bound widens', now);
  const credited = B.level1Credit(a, 2, 1, 20);
  must(credited.value === 17 && credited.pulls === 31 && credited.at === 20,
       'a credit lands on the account held at the product size it arrives at', credited);
}

console.log('[level1] ── a ranked member is an asked one ──');
{
  let threw = false;
  try { B._bestCandidate(pop({ idx: idxOf(row('k1', 'https://never.test/', 1, 1 * PT)) })); }
  catch (e) { threw = !!(e && e.apiclientFatal) && /never asked/.test(e.message) && /cold row/.test(e.message); }
  must(threw, 'ranking a cold row whose host was never asked aborts, naming the entry point that skipped the ask');
}

console.log('[level1] ── the three facts `_level1Record` keeps apart ──');

/* (1) THE ORDER WAS NOT ASKED. The round spent its advance on a navigation, or a reservation in flight held
   admission and the resident set was under the floor — so neither arm asked and a census of the non-resident
   half would be a reading of a walk that did not run. */
{
  const p = [eng('https://a.test/'), eng('https://b.test/')];
  const r = record(p, { n: 2, drained: 0, unserved: 0, unservedSites: 0, wTop: 3, wMin: 1 });
  checkPresence(r, false);
  must(r.wTop === 3 && r.wMin === 1 && !('wRunner' in r),
       'the RESIDENT half is a full reading even when the non-resident order was never asked', r);
  console.log('[level1]   not asked        ' + JSON.stringify(r));
}

/* (2) THE ORDER WAS ASKED AND RANKED NOTHING. A sub-frame never roots a cluster — its embedder names it — so
   it is not admissible and therefore not a candidate; it is COUNTED rather than skipped, because "every
   waiting document is a sub-frame" and "the order was asked and found nothing at all" are two states and a
   bare `cands: 0` cannot say which. */
{
  const P = pop({ waiting: [job('https://a.test/f', { frameId: 7, origin: 'https://a.test' })] });
  const pick = B._bestCandidate(P);
  must(pick.best === null, 'a walk that ranked nothing picks nothing', pick.census);
  must(pick.census.cands === 0 && pick.census.exclSub === 1,
       'a sub-frame leaves the order as an EXCLUSION and not as a silence', pick.census);
  const r = record([], { n: 0, drained: 0, unserved: 0, unservedSites: 0 }, pick.census, 1);
  checkPresence(r, true);
  console.log('[level1]   asked, empty     ' + JSON.stringify(r));
}

/* (3) A FULL READING — one work item of every declared kind, which is exactly the shape that used to abort.
   The accounting assert read `cands === candDocs + candCold` after a THIRD kind was already being ranked into
   the total, so the first application whose bundle named a route it does not link killed the round from its
   own `finally` — an instrument built so a frozen rank could not hide killing the loop it was measuring, on
   correct code. This drives all three kinds through the record in one census, so that sum is exercised with
   the kind that was missing from it rather than trusted to be complete. */
{
  const P = pop({ waiting: [job('https://a.test/')],
                  seeds: seedsOf('https://a.test/admin'),
                  idx: idxOf(row('k1', 'https://c.test/', 6, 3 * PT)) });
  const pick = B._bestCandidate(P);
  const r = record([], { n: 0, drained: 0, unserved: 0, unservedSites: 0 }, pick.census, 1);
  checkPresence(r, true);
  must(r.cands === 3 && KIND_POP.every((k) => r[k] === 1),
       'every declared kind reaches the census as its own population', r);
  must(r.cands === B.CAND_KINDS.reduce((n, k) => n + r[k.pop], 0),
       'the populations account for the total, summed over the DECLARED kinds', r);
  must(pick.best.key.siteW === Infinity && pick.best.kind !== 'cold' && r.candUnserved === 2 &&
       r.candSiteUnserved === 2,
       'an item of a never-served site wins over a served one, and the census counts both levels', r);
  must(kindWMax(r, 'cold') === keyOf(P, 'https://c.test/', P.idx.get('k1')).siteW &&
       r.candWMax === kindWMax(r, 'cold'),
       'the served item is reported at the site weight the walk ranked it by', r);
  console.log('[level1]   full reading     ' + JSON.stringify(r));
}

console.log('[level1] ── the discriminator: which tie is this ──');

/* TWO TIES, TWO READINGS. On a profile whose store is empty every candidate is unserved — the order sitting
   correctly at its entry value, every member at +Infinity, which the census states as `candUnserved === cands`
   with no spread at all. A tie between SERVED items is a reading of their accounts. */
{
  const entryTie = B._bestCandidate(pop({ waiting: [job('https://a.test/'), job('https://b.test/')] })).census;
  must(entryTie.candUnserved === entryTie.cands && !('candWMax' in entryTie),
       'an empty store ranks every candidate as never served, and the census reports no finite spread', entryTie);
  const servedTie = B._bestCandidate(pop({ idx: idxOf(row('k1', 'https://c.test/', 2, 1 * PT),
                                                       row('k2', 'https://d.test/', 2, 1 * PT)) })).census;
  must(servedTie.candWMax === servedTie.candWMin && servedTie.candUnserved === 0 && servedTie.candSvcMax === PT,
       'two served candidates of equal account tie, and the census says this tie stands above service', servedTie);
  console.log('[level1]   entry-value tie  ' + JSON.stringify(entryTie));
  console.log('[level1]   served tie       ' + JSON.stringify(servedTie));
}

/* A waiting document whose ADDRESS this store already knows ranks by that address's own account, not as never
   served. */
{
  const known = row('k1', 'https://a.test/', 9, 3 * PT);
  const P = pop({ waiting: [job('https://a.test/'), job('https://z.test/')], idx: idxOf(known) });
  const cen = B._bestCandidate(P).census;
  must(kindWMax(cen, 'doc') === keyOf(P, 'https://a.test/', known).siteW,
       'a waiting document ranks by what this profile has already spent and learned AT ITS ADDRESS', cen);
  must(cen.candSvcMax === known.pulls && cen.candUnserved === 1,
       'the census reports the service that produced that rank, beside the one never-served address', cen);
  console.log('[level1]   doc by history   ' + JSON.stringify(cen));
}

console.log('[level1] ── the weight that leaves the order ──');

/* AN EXCLUSION MOVES A ROW'S WEIGHT; IT DOES NOT DELETE IT. A parked entry and the document that holds its
   address are ONE work item, so the row leaves this order and the item standing for it must carry what it was
   worth. Each exclusion row is driven at least once here: a row nobody has ever produced is a row nobody
   knows works, and every one of them is a place a weight can leave the order in silence. */
{
  const held = row('k1', 'https://a.test/', 9, 3 * PT);
  const P = pop({ waiting: [job('https://a.test/')], idx: idxOf(held) });
  const cen = B._bestCandidate(P).census;
  must(cen.exclLive === 1 && kindPop(cen, 'cold') === 0,
       'a residue whose address a waiting document holds leaves the cold order', cen);
  must(kindWMax(cen, 'doc') === keyOf(P, 'https://a.test/', held).siteW,
       'and the document that excluded it carries exactly the weight that left', cen);
  console.log('[level1]   exclLive         ' + JSON.stringify(cen));
}
{
  const r1 = row('k1', 'https://a.test/', 4, 2 * PT);
  const cen = B._bestCandidate(pop({ resident: [eng('https://b.test/', 'k1')], idx: idxOf(r1) })).census;
  must(cen.exclHeld === 1 && kindPop(cen, 'cold') === 0 && cen.cands === 0,
       'a residue an instance already holds by KEY is not a candidate for a second instance', cen);
  console.log('[level1]   exclHeld         ' + JSON.stringify(cen));
}
{
  const cen = B._bestCandidate(pop({ idx: idxOf(row('k1', 'https://a.test/', 4, 2 * PT, { stranded: true })) })).census;
  must(cen.exclStranded === 1 && cen.cands === 0,
       'a stranded residue has no bytes to build an instance over, so it is excluded and COUNTED', cen);
  console.log('[level1]   exclStranded     ' + JSON.stringify(cen));
}
{
  const cen = B._bestCandidate(pop({ idx: idxOf(row('k1', 'https://a.test/', 4, 2 * PT, { crashed: true })) })).census;
  must(cen.exclCrashed === 1 && cen.cands === 0,
       'a residue whose resume aborted this engine is not re-admitted, and is COUNTED', cen);
  console.log('[level1]   exclCrashed      ' + JSON.stringify(cen));
}
{
  /* A rest is read against the engine this zone runs and the person's egress table, so both are stated first, as
     the offscreen document states them before its first round. */
  ZONE.rendererProgramDigest = async () => 'a'.repeat(64);
  await B.programDigest();
  ZONE.safeFetchEgressStated({});
  const rest = { program: 'a'.repeat(64), policy: B.egressPolicyNow() };
  const walk = (r) => B._bestCandidate(pop({ idx: idxOf(row('k1', 'https://a.test/', 4, 2 * PT, { rest: r })) })).census;
  let cen = walk(rest);
  must(cen.exclResting === 1 && cen.cands === 0,
       'a residue whose every member rests on a refusal is not re-admitted under the same engine and policy', cen);
  console.log('[level1]   exclResting      ' + JSON.stringify(cen));
  cen = walk({ program: 'b'.repeat(64), policy: rest.policy });
  must(cen.exclResting === 0 && kindPop(cen, 'cold') === 1, 'another engine ends the rest', cen);
  cen = walk({ program: rest.program, policy: rest.policy + ' widened' });
  must(cen.exclResting === 0 && kindPop(cen, 'cold') === 1, 'another egress policy ends the rest', cen);
}
{
  const cen = B._bestCandidate(pop({ resident: [eng('https://a.test/admin')],
                                     seeds: seedsOf('https://a.test/admin') })).census;
  must(cen.exclSeedLive === 1 && kindPop(cen, 'seed') === 0,
       'a declared route a live document is already exploring is not a second fetch', cen);
  console.log('[level1]   exclSeedLive     ' + JSON.stringify(cen));
}
{
  const cen = B._bestCandidate(pop({ seeds: seedsOf('https://a.test/admin'),
                                     idx: idxOf(row('k1', 'https://a.test/admin', 3, 1 * PT)) })).census;
  must(cen.exclSeedParked === 1 && kindPop(cen, 'seed') === 0 && kindPop(cen, 'cold') === 1,
       'a declared route this profile already holds a residue for is admitted as THAT residue, once', cen);
  console.log('[level1]   exclSeedParked   ' + JSON.stringify(cen));
}

{
  /* One address, two documents: a declaration from an opaque principal loads without the session, so a
     credentialed member at that address does not hold it, and the two declarations are two seeds. */
  const seeds = new Map([seedOf('https://a.test/admin', 'null:opaque-1', false),
                         seedOf('https://a.test/admin', 'https://a.test', true)]);
  must(seeds.size === 2, 'an opaque and a credentialed declaration of one address are two identities', [...seeds.keys()]);
  const cen = B._bestCandidate(pop({ resident: [eng('https://a.test/admin')], seeds })).census;
  must(cen.exclSeedLive === 1 && kindPop(cen, 'seed') === 1,
       'a credentialed member holds the credentialed declaration and not the opaque one', cen);
  console.log('[level1]   identity split   ' + JSON.stringify(cen));
}
{
  /* A loading seat is a member: it states the identity it loads, so a second declaration lands on it. */
  const [id, seed] = seedOf('https://a.test/admin', 'https://a.test', true);
  const seat = { state: 'loading', what: { kind: 'seed', seed, identity: id }, identity: id, msg: null };
  const cen = B._bestCandidate(pop({ resident: [seat], seeds: new Map([[id, seed]]) })).census;
  must(cen.exclSeedLive === 1 && kindPop(cen, 'seed') === 0,
       'a declaration of the document a loading seat is fetching is that seat, not a second load', cen);
  const held = B._frontierMembers(pop({ resident: [seat] })).get(id);
  must(held !== undefined && /loading seed seat/.test(held.label), 'the member names the seat', held);
  console.log('[level1]   seat member      ' + JSON.stringify(cen));
}

console.log('[level1] ── the population is an input, and an absent half is not an empty one ──');
console.log('[level1]   (the four @WHY lines below are the aborts this section REQUIRES — check.js writes ' +
            'them to the console on its way to throwing, and their absence is what would fail here)');

/* THE ONE HOLE THE CENSUS CANNOT CLOSE FROM INSIDE. A population handed with a kind's half MISSING produces
   that kind's population as 0 — a count of zero is a READING, so the record agrees with itself about a walk
   that never looked, and a whole kind of work item leaves the Level-1 order with nothing anywhere to say so.
   It is asserted at the door instead, against the kind table, so a kind added there cannot be walked over a
   population nobody supplied. This drives that door for every declared kind. */
for (const k of B.CAND_KINDS) {
  const P = pop();
  delete P[k.from];
  let threw = false;
  try { B._bestCandidate(P); } catch (e) { threw = !!(e && e.apiclientFatal); }
  must(threw, 'a population missing its `' + k.from + '` half ABORTS rather than reporting `' + k.pop +
              ': 0`, which would be a reading of a walk that never ran');
}
{
  let threw = false;
  try { B._bestCandidate({ waiting: [], seeds: new Map(), idx: new Map() }); }
  catch (e) { threw = !!(e && e.apiclientFatal); }
  must(threw, 'a population with no RESIDENT set aborts — without it every excluded item silently becomes a ' +
              'candidate and the exclusion counts that say so read zero');
}
/* AND THE SHAPE CHECK IS A SHAPE CHECK, WHICH IS WHY A DRIVER CAN ASK AT ALL. Every container above is minted
   in THIS realm, so an `instanceof Map` at that door would reject all of it — a false abort, which is
   strictly worse than the wrong answer it guards against. That this file runs is the check; it is stated
   because the next person to tighten that door will reach for `instanceof` first. */
must(B._candPopulation(new Map()).resident !== undefined,
     'the production composer still states the whole population in one place');

console.log('[level1] ── hostSchedule, driven with mock engines ──');

/* THE CLAIM AT THE HEAD OF `hostSchedule` — "the PURE scheduler policy (no wasm knowledge — engine ops are
   injected, so this is unit-testable with mock engines)" — which nothing in this tree had ever taken up. What
   it buys is the ROUND: the census's unit is a round and not a pick, `_level1Record` runs from the round's
   own `finally` on EVERY exit, and `candAsk === 2` exists only where one round asks the order twice (an
   admission that seats a document can be what puts the working set at the floor, and the eviction then asks
   the same order again over a pool that has changed underneath it).
   AND IT IS THE ONE PLACE THE FAILURE MODE THAT KILLED THE LOOP IS REPRODUCIBLE. An assert that fires inside
   `_level1Record` throws out of a `finally`, which is every exit of the round — so it does not report a bad
   census, it takes the whole Level-1 scheduler down. That is caught here and REPORTED, because a driver that
   died with it would say the same thing the extension said: nothing. */
{
  const rounds = [];
  const poolArr = [];
  const e1 = engM('https://e.test/', 5, 1 * PT);
  poolArr.push(e1);
  /* Served rows only: an unserved candidate would spend the round on its admission (the `admitted` arm). */
  const asked = pop({ idx: idxOf(row('k1', 'https://c.test/', 6, 3 * PT)) });
  let admits = 0, steps = 0;
  const ops = {
    key: B.engineKey,
    arrival: () => new Promise(() => {}),   // nothing arrives during this drive
    tree: () => B.level1Tree(poolArr, new Map()),
    /* THE ADMISSION ASKS THE ORDER ON THE FIRST ROUND AND ANSWERS THE POSITIVE `null` AFTERWARDS — the two
       shapes hostSchedule asserts at the seam, both driven. */
    admit: async () => (admits++ === 0 ? B._bestCandidate(asked).census : null),
    /* AND THE EVICTION ASKS IT AGAIN IN THAT SAME ROUND, WITHOUT EVICTING — which is `candAsk === 2`, the
       state an assert that the two arms are exclusive would have fired on. */
    evictee: async () => (admits === 1 ? { evict: null, cand: B._bestCandidate(asked).census }
                                       : { evict: null, cand: null }),
    requestPark: async () => {},
    release: async () => {},
    /* YIELD on the first step (which parks the engine into `fetching` and takes the wait arm on the round
       after it), then DONE, which finalizes and empties the pool so the loop breaks. */
    step: async () => (steps++ === 0 ? 2 : 0),
    /* SETTLING ON A LATER TURN IS THE POINT, NOT A DELAY. The engine is still `fetching` when the next round
       ranks, so that round finds NO hot engine and takes the arm that waits on `_readyP` — two states reach
       it (a reply body, a reservation still provisioning) and it is the arm whose ABSENCE was a full-speed
       spin through admit on a condition only the boot it refused to wait for could change. */
    serviceFetch: () => new Promise((res) => setTimeout(res, 5)),
    finish: async (e) => { const i = poolArr.indexOf(e); if (i >= 0) poolArr.splice(i, 1); },
  };
  /* EVERY ROUND'S RECORD IS COLLECTED BY WATCHING THE PUBLICATION, NEVER BY REPLACING THE WRITER. A round
     overwrites `self._level1`, so a driver that read the property after the loop would see only the last one
     — and a driver that wrapped `_level1Record` would be measuring its own function. This observes the exact
     property `popup.js` is served from, so what is collected is what the shipped path wrote, in order. */
  const seen = [];
  const origRecord = B._level1Record;
  let published;
  Object.defineProperty(ZONE, '_level1', {
    configurable: true,
    get: () => published,
    set: (v) => { published = v; seen.push(v); },
  });
  let died = null;
  try { await B.hostSchedule(poolArr, ops); } catch (e) { died = e; }
  delete ZONE._level1;
  must(origRecord === B._level1Record, 'the driver did not replace the shipped record writer');
  must(died === null,
       'hostSchedule ran to completion over mock engines — a throw here is the round dying inside its own ' +
       '`finally`, which takes the Level-1 scheduler down rather than reporting a bad census',
       died ? String(died.message || died) : undefined);
  must(seen.length > 0, 'every round published a census', seen.length);
  const twice = seen.filter((r) => r.candAsk === 2);
  must(twice.length >= 1,
       'a round that asks the non-resident order TWICE says so — the later reading is the one the eviction ' +
       'comparison was made against, and a superseded reading must never be a silent one',
       seen.map((r) => r.candAsk));
  for (const r of seen) rounds.push(r);
  for (const r of rounds) checkPresence(r, 'cands' in r);
  must(rounds.some((r) => !('cands' in r)),
       'at least one round of a real loop never asked the order at all — the fact the census keeps apart ' +
       'from an order that was asked and found nothing',
       rounds.map((r) => ('cands' in r ? r.cands : 'not asked')));
  /* THE ROUND THAT WAITS. A rankable set of NONE is a reading and not an absence — the row is present and the
     weights are not, because an extremum over nothing is not a number. */
  must(rounds.some((r) => r.hot === 0 && !('wTop' in r)),
       'a round that found nothing rankable records `hot: 0` and NO weights — every live engine is ' +
       'mid-something and the round waits on the earliest to become rankable, which is a reading of that ' +
       'order and not a silence',
       rounds.map((r) => r.hot));
  /* THE TERMINAL ROUND, NAMED, BECAUSE IT IS THE ONE THIS DRIVE FOUND ABORTING. `finish` splices the engine
     out of the pool and the record runs from the round's own `finally` afterwards, so `hot` (read at the
     rank) legitimately OUTRUNS `pool` (read at the end) on the round that completes a document — an
     ordinary single-tab pool is all-hot, so this was every terminal round, and an assert that the two are
     one instant took the whole Level-1 scheduler down there rather than reporting anything. */
  must(rounds.some((r) => r.hot > r.pool),
       'a round that FINALIZED its engine still records — `hot` is the rank\'s instant and `pool` is the ' +
       'end of the round, and the difference between them is the fact that the round completed a document',
       rounds.map((r) => r.hot + '/' + r.pool));
  for (const r of rounds) console.log('[level1]   round ' + r.round + '  ' + JSON.stringify(r));
}

console.log('[level1] ── the pick: a site, then a document within it ──');

/* A ROUND WHOSE ADMISSION OFFERED AN ITEM OF A NEVER-SERVED SITE SPENDS ITS ADVANCE ON THAT ADMISSION; a
   never-served document of a served site does not, and a round that steps a served site beside a never-served
   one aborts. A yielded engine ahead of every hot one is waited for. */
async function drive(poolArr, ops) {
  const seen = [];
  let published;
  Object.defineProperty(ZONE, '_level1', { configurable: true, get: () => published,
                                           set: (v) => { published = v; seen.push(v); } });
  let died = null;
  try { await B.hostSchedule(poolArr, ops); } catch (e) { died = e; }
  delete ZONE._level1;
  return { seen, died };
}
const baseOps = (poolArr, o, all) => Object.assign({
  key: B.engineKey, arrival: () => new Promise(() => {}), tree: () => B.level1Tree(all || poolArr, new Map()), admit: async () => null,
  evictee: async () => ({ evict: null, cand: null }), requestPark: async () => {}, release: async () => {},
  step: async () => 0, serviceFetch: async () => {},
  finish: async (e) => { const i = poolArr.indexOf(e); if (i >= 0) poolArr.splice(i, 1); },
}, o);
{
  const poolArr = [engM('https://e.test/', 5, 1 * PT)];
  const newSite = pop({ waiting: [job('https://n.test/')] });
  let admits = 0, stepped = 0;
  const ops = baseOps(poolArr, { admit: async () => (admits++ === 0 ? B._bestCandidate(newSite).census : null),
                                 step: async () => { stepped++; return 0; } });
  const { seen, died } = await drive(poolArr, ops);
  must(died === null && seen[0].rAdmitted === 1 && seen[0].hot === 0 && stepped === 1,
       'the round whose admission seated a never-served SITE stepped nothing; the next round stepped',
       died ? String(died.message || died) : seen);
}
{
  /* The same never-served address, but of the site the hot engine is already serving: it is ahead only within
     that site, so the round steps rather than spending itself on the admission. */
  const poolArr = [engM('https://e.test/', 5, 1 * PT)];
  const sameSite = pop({ resident: poolArr, waiting: [job('https://e.test/next')] });
  let admits = 0, stepped = 0;
  const ops = baseOps(poolArr, { admit: async () => (admits++ === 0 ? B._bestCandidate(sameSite).census : null),
                                 step: async () => { stepped++; return 0; } });
  const { seen, died } = await drive(poolArr, ops);
  must(died === null && seen[0].hot === 1 && seen[0].candUnserved === 1 && seen[0].candSiteUnserved === 0 &&
       stepped === 1,
       'a never-served document of a served site does not take the round from its site\'s own deepening',
       died ? String(died.message || died) : seen);
}
{
  const poolArr = [engM('https://e.test/', 5, 1 * PT)];
  const newSite = pop({ waiting: [job('https://n.test/')] });
  const ops = baseOps(poolArr, { evictee: async () => ({ evict: null, cand: B._bestCandidate(newSite).census }) });
  const { died } = await drive(poolArr, ops);
  must(died !== null && !!died.apiclientFatal,
       'stepping a served site while the order this round asked held a never-served site aborts');
}
{
  /* B yields and its service round is in flight; it is ahead of A, but the round steps A rather than idle the
     thread on B's reply, and B takes the slice after its round lands. */
  const A = engM('https://a.test/', 1, 1 * PT, { name: 'A' });
  const Bk = engM('https://b.test/', 9, 1 * PT, { name: 'B' });
  const poolArr = [A, Bk];
  const order = [];
  let n = 0;
  const ops = baseOps(poolArr, {
    step: async (e) => { order.push(e.name); return ++n >= 3 ? 0 : 2; },
    serviceFetch: () => new Promise((res) => setTimeout(res, 5)),
  });
  const { died } = await drive(poolArr, ops);
  must(died === null && order[0] === 'B' && order[1] === 'A' && order[2] === 'B',
       'a hot lesser document takes the thread while the leader\'s service round is in flight, and the leader ' +
       'takes it back once its round lands',
       died ? String(died.message || died) : order);
}

/* A round waiting on the only engine's in-flight service round wakes on an arrival and asks admission again
   before that reply lands; with no wake it asks only once the reply lands, which is the control. */
async function arrivalDrive(wakes) {
  const lead = engM('https://b.test/', 9, 1 * PT, { name: 'B' });
  const poolArr = [lead];
  const newSite = pop({ waiting: [job('https://n.test/')] });
  let wake = null, arrived = false, landed = false, settle = null, inFlight = null, n = 0;
  const ops = baseOps(poolArr, {
    arrival: () => new Promise((r) => { wake = r; }),
    admit: async () => {
      if (!arrived || inFlight !== null) return null;
      inFlight = !landed;
      return B._bestCandidate(newSite).census;
    },
    step: async (e) => (e === lead && n++ === 0 ? 2 : 0),
    serviceFetch: () => new Promise((res) => { settle = res; }),
  });
  const run = drive(poolArr, ops);
  await new Promise((r) => setTimeout(r, 20));
  arrived = true;
  if (wakes && wake !== null) wake();   // null: no round took a latch, which the relation below reports
  await new Promise((r) => setTimeout(r, 20));
  landed = true;
  settle();
  const { died } = await run;
  return { died, inFlight };
}
{
  const woke = await arrivalDrive(true), ctrl = await arrivalDrive(false);
  must(woke.died === null && ctrl.died === null && woke.inFlight === true && ctrl.inFlight === false,
       'a never-served site arriving while the round waits on the only service round is admitted before that ' +
       'reply lands, and only after it without the wake',
       [woke.died ? String(woke.died.message || woke.died) : woke.inFlight,
        ctrl.died ? String(ctrl.died.message || ctrl.died) : ctrl.inFlight]);
}

console.log('[level1] ── a site that mints addresses spends only its own share ──');

/* Site M mints 1000 never-served addresses beside its served root; site P has one productive document. Each
   slice is one 12 ms quantum of service; P earns 0.6 per quantum falling to nothing over its first 1000, M's
   documents earn nothing. The same drive
   without the minted addresses is the control. The tree's site level sees only site accounts, so M's minting
   moves neither site's share, and M's new documents each take their first quantum within M's slices, ahead of
   M's root. */
async function mintDrive(minted, slices) {
  const P = engM('https://p.test/', 100, 2 * PT, { name: 'P', rate: 0.6, decay: 0.0006 });
  const root = engM('https://m.test/', 40, 2 * PT, { name: 'M', rate: 0, decay: 0 });
  const kids = [];
  for (let i = 0; i < minted; i++) kids.push(engM('https://m.test/p' + i, 0, 0, { name: 'm' + i, rate: 0, decay: 0 }));
  const poolArr = [P, root, ...kids];
  const all = poolArr.slice();
  const order = [];
  const ops = baseOps(poolArr, {
    step: async (e) => {
      if (order.length >= slices) return 0;
      order.push(e.name);
      e.level1.acct = B.level1Credit(e.level1.acct, e.rate, SLICE, 1);
      e.level1.pulls += SLICE; e.level1.paid += e.rate; e.rate = Math.max(0, e.rate - e.decay);
      return 2;
    },
  }, all);
  const { died } = await drive(poolArr, ops);
  return { order, died };
}
{
  const mint = await mintDrive(1000, 3000), ctrl = await mintDrive(0, 3000);
  const count = (o, f) => o.filter(f).length;
  const pM = count(mint.order, (x) => x === 'P'), pC = count(ctrl.order, (x) => x === 'P');
  const mM = mint.order.length - pM, mC = ctrl.order.length - pC;
  const kidsServed = new Set(mint.order.filter((x) => x[0] === 'm' && x !== 'M')).size;
  const firstRoot = mint.order.indexOf('M');
  const kidSlicesBeforeRoot = firstRoot < 0 ? count(mint.order, (x) => x[0] === 'm' && x !== 'M')
                                            : count(mint.order.slice(0, firstRoot), (x) => x[0] === 'm' && x !== 'M');
  must(mint.died === null && ctrl.died === null, 'both drives ran to completion',
       String((mint.died || ctrl.died || {}).message));
  must(pM === pC && pM > mM,
       'the productive site keeps its UCB share against a site minting 1000 addresses — exactly the share it ' +
       'has against that site with none',
       { productiveMint: pM, productiveControl: pC, mintingSite: mM, mintingControl: mC });
  must(mM === mC && kidsServed === Math.min(1000, mM) &&
       kidSlicesBeforeRoot === Math.min(1000, mM),
       'the minting site\'s new documents each take their first quantum only within that site\'s share, ahead of ' +
       'its own served root',
       { mintingSite: mM, kidsServed, kidSlicesBeforeRoot, firstRoot });
  console.log('[level1]   minting          P ' + pM + ' / M ' + mM + ' of ' + mint.order.length +
              ' slices (control P ' + pC + ' / M ' + mC + '); minted documents served ' + kidsServed);
}

/* ─── THE ENGINE'S ACCOUNT, AS main.c COMPOSES IT ───────────────────────────────────────────────────────
   engineRecordFacts reads `getLevel1Account`'s reply, which engine/host/main.c's qjs_level1_account composes
   with one snprintf. The reply here is formatted from that format string, read out of main.c at run time, so a
   field the composer renames or drops fails this relation rather than the first live engine. */
{
  const src = readFileSync(join(ENGINE, 'host', 'main.c'), 'utf8');
  const fn = src.indexOf('qjs_level1_account(');
  const m = fn < 0 ? null : /snprintf\(buf, sizeof buf, "((?:[^"\\]|\\.)*)"/.exec(src.slice(fn));
  if (!m)
    throw new Error('engine/host/main.c no longer composes qjs_level1_account with one snprintf into `buf` — ' +
                    'this relation formats the reply from that string, so re-point it at the composer');
  const fmt = m[1].replace(/\\"/g, '"');
  const argsOf = (vals) => { let i = 0; return fmt.replace(/%(?:d|\.17g)/g, () => String(vals[i++])); };
  const reply = (vals) => async () => ({ account: argsOf(vals), workingSetBytes: 65536 });
  const e = { state: 'hot', msg: { sourceUrl: 'https://a.test/' }, lines: [],
              r: { renderer: { getLevel1Account: reply([1, 3, 2]) } } };
  let died = null, answers = [];
  try {
    answers.push(await B.engineRecordFacts(e));
    e.r.renderer.getLevel1Account = reply([1, 5, 4]);
    answers.push(await B.engineRecordFacts(e));
  } catch (x) { died = x; }
  must(died === null && answers.every((x) => x === null) && !e._crashed && e.level1.runnable === true &&
       e.level1.paid === 5 && e.level1.pulls === 4 && e.level1.acct.value === 5 && e.level1.acct.pulls === 4,
       'engineRecordFacts takes the account main.c composes and credits its counters\' differences to the ranked ' +
       'account',
       died ? String(died.message || died) : { format: fmt, answers, level1: e.level1 });
  /* The controls: replies main.c never composes, each refused as the instance's answer. The instance keeps the
     facts of its last accepted reply, is marked crashed, and the run its lines compose reads crashed with a
     `result-refused` row naming the field. The ranked account's own shape is the first. */
  const invented = [
    ['the ranked shape, with no `paid`', '{"runnable":1,"value":3,"pulls":2,"at":1}', 65536, '`paid`'],
    ['`runnable` outside 0 and 1', argsOf([2, 5, 4]), 65536, '`runnable`'],
    ['counters below the last reading', argsOf([1, 4, 4]), 65536, 'fell below'],
    ['an account that is not JSON', '{"runnable":1,', 65536, 'is not an object'],
    ['a working set off the page grain', argsOf([1, 5, 4]), 65537, 'WASM pages'],
  ];
  for (const [label, account, workingSetBytes, names] of invented) {
    const ctrl = { state: 'hot', msg: { sourceUrl: 'https://a.test/' }, lines: [],
                   level1: { runnable: true, paid: 5, pulls: 4, acct: { value: 5, pulls: 4, at: 1 } },
                   r: { renderer: { getLevel1Account: async () => ({ account, workingSetBytes }) } } };
    const kept = JSON.stringify(ctrl.level1);
    let why = null, threw = null;
    try { why = await B.engineRecordFacts(ctrl); } catch (x) { threw = x; }
    const run = threw ? null : ZONE.linesToAnalysis(ctrl.lines, ctrl.msg, 'crashed', null);
    const rows = run ? run.resolverErrors.filter((x) => x.context === 'result-refused') : [];
    must(threw === null && typeof why === 'string' && why.includes(names) && ctrl._crashed === true &&
         JSON.stringify(ctrl.level1) === kept && run._run === 'crashed' && rows.length === 1 &&
         rows[0].message === why,
         'a Level-1 reply with ' + label + ' is refused as the instance\'s answer and its run reads crashed',
         threw ? String(threw.message || threw) : { why, crashed: ctrl._crashed, run: run && run._run, rows });
  }
  console.log('[level1]   engine account   format ' + fmt + '; ' + invented.length + ' invented replies driven');
}

/* ─── THE ENGINE'S OTHER TWO ANSWERS: A STEP CODE AND A REQUEST-POLICY VERDICT ──────────────────────────
   Both are the instance's statements. The step op takes DONE, YIELD and STALLED and refuses any other code
   before recording anything, so the instance keeps its last account; the policy question takes `allowed` and
   a refusal naming its directive, and answers any other verdict `refused`, never allowing, without throwing.
   Each refused instance is marked crashed and its lines compose a crashed run with one `result-refused` row. */
{
  const ops = runInContext('_hostOps', ZONE);
  const pool = runInContext('_pool', ZONE);
  const account = async () => ({ account: '{"runnable":1,"paid":5,"pulls":4}', workingSetBytes: 65536 });
  const inst = (o) => Object.assign({ state: 'hot', msg: { sourceUrl: 'https://a.test/' }, lines: [], origin: 'https://a.test',
                                      _novelty: { spelling: null, n: 0 }, _noveltyCounted: new Map(),
                                      level1: { runnable: true, paid: 3, pulls: 2, acct: { value: 3, pulls: 2, at: 1 } } }, o);
  const refusedRun = (e) => {
    const run = ZONE.linesToAnalysis(e.lines, e.msg, 'crashed', null);
    return { run: run._run, rows: run.resolverErrors.filter((x) => x.context === 'result-refused').map((x) => x.message) };
  };
  for (const code of [0, 2, 3, 1, 4, '2', null, undefined]) {
    let asked = 0;
    const e = inst({ r: { renderer: { step: async () => ({ code }), getLevel1Account: async () => { asked++; return account(); } } } });
    let st, threw = null;
    try { st = await ops.step(e); } catch (x) { threw = x; }
    const accepted = code === 0 || code === 2 || code === 3;
    const r = e._crashed ? refusedRun(e) : null;
    must(threw === null && (accepted
           ? st === code && !e._crashed && asked === 1 && e.level1.paid === 5
           : st === 0 && e._crashed === true && asked === 0 && e.level1.paid === 3 && r.run === 'crashed' &&
             r.rows.length === 1 && r.rows[0].includes(JSON.stringify(code))),
         'the step code ' + JSON.stringify(code) + ' is ' + (accepted ? 'taken and the round recorded' :
           'refused as the instance\'s answer before anything is recorded, and the op answers DONE'),
         threw ? String(threw.message || threw) : { st, crashed: e._crashed, asked, level1: e.level1, r });
  }
  const verdicts = [['allowed', 'allowed', ''], ['blocked\tconnect-src', 'blocked', 'connect-src'],
                    ['report-only\tdefault-src', 'report-only', 'default-src'], ['allowed\tconnect-src', null],
                    ['blocked', null], ['blocked\t', null], ['denied\tconnect-src', null], [7, null],
                    ['blocked\tconnect-src\tx', null]];
  for (const [raw, verdict, directive] of verdicts) {
    let asked = 0;
    const e = inst({ docId: 'policy-doc', joinedDocIds: [],
                     r: { renderer: { checkRequestPolicy: async () => { asked++; return { verdict: raw }; } } } });
    pool.push(e);
    let a, again, threw = null;
    try {
      a = await ZONE.engineRequestPolicyVerdict('policy-doc', 'https://a.test/api');
      again = await ZONE.engineRequestPolicyVerdict('policy-doc', 'https://a.test/api');
    } catch (x) { threw = x; }
    pool.splice(pool.indexOf(e), 1);
    const r = e._crashed ? refusedRun(e) : null;
    must(threw === null && (verdict !== null
           ? a.verdict === verdict && a.directive === directive && a.reason === undefined && !e._crashed &&
             again.verdict === verdict && asked === 2
           : a.verdict === 'refused' && a.directive === '' && typeof a.reason === 'string' &&
             a.reason.includes(JSON.stringify(raw)) && e._crashed === true && r.run === 'crashed' &&
             r.rows.length === 1 && r.rows[0] === a.reason && again.verdict === 'unknown' && asked === 1),
         'the request-policy verdict ' + JSON.stringify(raw) + ' is ' + (verdict !== null ? 'answered as `' + verdict +
           '`' : 'answered `refused` with its reason, never as allowing, and the instance is retired and asked nothing more'),
         threw ? String(threw.message || threw) : { a, again, asked, crashed: e._crashed, r });
  }
  console.log('[level1]   engine answers   8 step codes and ' + verdicts.length + ' request-policy verdicts driven');

  /* A refusal from outside the round (the Send panel's policy question) landing while a round is in flight:
     at the round's own call, or while the record's account reply is in flight. The rounds are the step, the
     service round and the record that ends a join, an unload and a seed (engineRecordFacts, called as they
     call it). A refused instance keeps its last accepted account and working set, its run carries the one
     refusal, and an accepted verdict in the same interleaving records the round (the control). */
  const deferred = () => { let resolve; const p = new Promise((r) => { resolve = r; }); return { p, resolve }; };
  const rounds = { step: (e) => ops.step(e), 'service round': (e) => ops.serviceFetch(e, false),
                   'join/unload/seed record': (e) => B.engineRecordFacts(e) };
  const cases = [];
  for (const round of Object.keys(rounds)) for (const at of ['round', 'record']) for (const v of ['denied\tx', 'allowed'])
    cases.push([round, at, v]);
  let gapRows = 0;
  for (const [round, at, v] of cases) {
    if (round === 'join/unload/seed record' && at === 'round' && v === 'allowed') continue;
    const gate = deferred();
    let asked = 0;
    const later = (x) => (at === 'round' ? gate.p.then(() => x) : Promise.resolve(x));
    const e = inst({ docId: 'gap-doc', joinedDocIds: [], _inflight: new Map(), residentBytes: 131072,
                     r: { renderer: {
                       step: () => later({ code: 2 }), getPending: async () => ({ requests: '' }),
                       getHostNotices: async () => ({ notices: '' }), getHostRequests: () => later({ requests: '' }),
                       getLevel1Account: () => { asked++; return at === 'record' ? gate.p.then(account) : account(); },
                       checkRequestPolicy: async () => ({ verdict: v }) } } });
    pool.push(e);
    let out, askedAtRefusal = null, threw = null;
    try {
      let running;
      if (round === 'join/unload/seed record' && at === 'round') {
        await ZONE.engineRequestPolicyVerdict('gap-doc', 'https://a.test/api');
        askedAtRefusal = asked;
        running = rounds[round](e);
      } else {
        running = rounds[round](e);
        await new Promise((r) => setImmediate(r));   // the round reaches its gate
        await ZONE.engineRequestPolicyVerdict('gap-doc', 'https://a.test/api');
        askedAtRefusal = asked;
        gate.resolve();
      }
      out = await running;
    } catch (x) { threw = x; }
    pool.splice(pool.indexOf(e), 1);
    const refused = v !== 'allowed';
    const r = e._crashed ? refusedRun(e) : null;
    const reachedGate = askedAtRefusal === (at === 'record' ? 1 : 0);
    /* What each round answers: the step op its code, the service round nothing, the record its refusal or null. */
    const answered = round === 'step' ? out === (refused ? 0 : 2)
      : round === 'service round' ? out === undefined : refused ? typeof out === 'string' : out === null;
    must(threw === null && reachedGate && answered && (refused
           ? e._crashed === true && e.level1.paid === 3 && e.residentBytes === 131072 && r.run === 'crashed' &&
             r.rows.length === 1 && r.rows[0].includes('denied')
           : !e._crashed && e.level1.paid === 5),
         'a ' + (refused ? 'refusal' : 'policy answer (the control)') + ' landing ' +
           (at === 'round' ? 'at the ' + round + '\'s own call'
                           : 'while the ' + round + '\'s account reply is in flight') + ' ' +
           (refused ? 'leaves the instance\'s last accepted facts unrecorded over' : 'lets the round record'),
         threw ? String(threw.message || threw)
               : { round, at, out, askedAtRefusal, crashed: e._crashed, level1: e.level1, r });
    gapRows++;
  }
  console.log('[level1]   retired rounds   ' + gapRows + ' interleavings of a policy refusal and a round driven');
}

console.log('[level1] ── verdict ──');
console.log('[level1]   ' + restoresRefused + ' load-time IndexedDB restore(s) this realm cannot perform');
console.log('[level1]   ' + checks + ' relation(s) checked over the shipped `self._level1`, ' +
            failures + ' broken');
if (failures) {
  console.log('[level1] FAILED — the Level-1 order is composed entirely in bridge.js and no result document ' +
              'can carry it, so a broken relation here is a defect nothing else in this tree can see.');
  process.exit(1);
}
console.log('[level1] OK');
/* The zone's MessageChannel (bridge.js's macrotask source) holds the event loop open. */
process.exit(0);
