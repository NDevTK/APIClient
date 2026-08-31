/* THE LEVEL-1 ORDER'S DRIVER — the trusted-zone sibling of `engine/route.mjs`, and the only thing that can
 * ask this scheduler a question at all.
 *
 * `node engine/level1.mjs`. It compiles nothing and links nothing: it loads `extension/bridge.js` verbatim
 * into a realm of its own and drives the shipped walk and the shipped record over populations it composes.
 *
 * WHY THIS FILE EXISTS, WHICH IS A MEASUREMENT GAP AND NOT A TEST GAP. §scheduler splits the WFQ in two.
 * Level-2 — the flows inside one document — is composed by the ENGINE, so its census rides the result
 * document and every gate that reads a result reads it. Level-1 is `engineWeight` per HOT INSTANCE against
 * `frontierWeight` per waiting address, declared route and cold row, and NO ENGINE CAN SEE ANOTHER ENGINE:
 * this order is composed entirely in `bridge.js`, so no result document can ever carry it and no corpus run
 * touches it. test262 links five quickjs sources and not one line of this zone; WPT judges by a browser
 * oracle; solvergate compares a document against itself through one host. The Level-1 order is outside all
 * three by construction.
 *
 * WHAT THAT COST, MEASURED RATHER THAN SUSPECTED. Both Level-1 defects found this session were RANKS FROZEN
 * AT A CONSTANT. `frontierWeight(FRONTIER_UNSERVED)` was the literal 1.0 for every waiting document however
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

const ENGINE = dirname(fileURLToPath(import.meta.url));
const EXT_DIR = join(ENGINE, '..', 'extension');

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
const ZONE = (() => {
  const sandbox = { console, fetch, URL, TextDecoder, TextEncoder, setTimeout, clearTimeout };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.module = { exports: {} };
  createContext(sandbox);
  for (const f of ['check.js', 'lib/safe-fetch.js', 'bridge.js'])
    runInContext(readFileSync(join(EXT_DIR, f), 'utf8'), sandbox, { filename: join(EXT_DIR, f) });
  return sandbox;
})();
const B = ZONE.module.exports;
for (const n of ['hostSchedule', 'engineWeight', 'frontierWeight', 'frontierReward', '_bestCandidate',
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
const job = (url, o = {}) => ({ msg: { sourceUrl: url, frameId: o.frameId || 0, origin: o.origin || '' } });
const row = (key, url, emit, visits, o = {}) =>
  ({ key, sourceUrl: url, emit, visits, shed: false, stranded: o.stranded === true,
     bytes: 0, rederivable: true });
const eng = (url, fkey) => ({ msg: { sourceUrl: url }, fkey: fkey || ('k|' + url) });
const idxOf = (...rows) => new Map(rows.map((r) => [r.key, r]));
const seedsOf = (...addrs) => new Map(addrs.map((a) => [a, { url: a, principalUrl: a,
                                                            principalOrigin: 'https://x.test',
                                                            provenance: 'derived' }]));
const pop = (o = {}) => ({ resident: o.resident || [], waiting: o.waiting || [],
                           seeds: o.seeds || new Map(), idx: o.idx || new Map() });

/* A ROUND, DRIVEN THROUGH THE SHIPPED RECORD AND READ BACK OFF `self`. `hot` is the resident half the round's
   scan produces; `cand` is what the walk answered, or the POSITIVE null that says the round never asked. */
function record(poolArr, hot, census, asks) {
  ZONE._level1 = undefined;
  B._level1Record(poolArr, { hot: hot, cand: census === undefined ? null : census, candAsk: asks || 0 });
  return ZONE._level1;
}
/* THE PRESENCE RULE, ASSERTED AS THE ONE RULE IT IS. §_level1Record: a COUNT is present whenever the walk
   that produces it ran, because a count of zero is a reading; a WEIGHT is present only over a NON-EMPTY
   population, because an extremum over nothing is not a number. Every conditional row of the non-resident
   half obeys it, so it is checked over the declarations rather than over a list of names kept here. */
function checkPresence(r, asked) {
  must(('cands' in r) === asked,
       'the total is present exactly when the order was asked', r);
  must(('candAsk' in r) === asked,
       'the ask count travels with the reading it qualifies', r);
  for (const k of COUNT_ROWS)
    must((k in r) === asked,
         'the count row `' + k + '` is present exactly when the walk ran (a zero is a READING)', r);
  const ranked = asked && r.cands > 0;
  for (const s of SPREAD_ROWS)
    must((s in r) === ranked,
         'the spread row `' + s + '` exists exactly over a NON-EMPTY ranked set', r);
  for (const k of B.CAND_KINDS)
    must((k.wMax in r) === (asked && r[k.pop] > 0),
         'the extremum `' + k.wMax + '` exists exactly over a non-empty `' + k.pop + '`', r);
}

console.log('[level1] ── the three facts `_level1Record` keeps apart ──');

/* (1) THE ORDER WAS NOT ASKED. The round spent its advance on a navigation, or a reservation in flight held
   admission and the resident set was under the floor — so neither arm asked and a census of the non-resident
   half would be a reading of a walk that did not run. */
{
  const p = [eng('https://a.test/'), eng('https://b.test/')];
  const r = record(p, { n: 2, drained: 0, wTop: 3, wMin: 1, wRunner: 1 });
  checkPresence(r, false);
  must(r.wTop === 3 && r.wMin === 1 && r.wRunner === 1,
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
  const r = record([], { n: 0, drained: 0 }, pick.census, 1);
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
                  idx: idxOf(row('k1', 'https://c.test/', 6, 3)) });
  const pick = B._bestCandidate(P);
  const r = record([], { n: 0, drained: 0 }, pick.census, 1);
  checkPresence(r, true);
  must(r.cands === 3 && KIND_POP.every((k) => r[k] === 1),
       'every declared kind reaches the census as its own population', r);
  must(r.cands === B.CAND_KINDS.reduce((n, k) => n + r[k.pop], 0),
       'the populations account for the total, summed over the DECLARED kinds', r);
  must(pick.best.kind === 'cold' && kindWMax(r, 'cold') === B.frontierWeight(P.idx.get('k1')),
       'the winner is the item with the highest weight and the census reports THAT weight', r);
  console.log('[level1]   full reading     ' + JSON.stringify(r));
}

console.log('[level1] ── the discriminator: which tie is this ──');

/* TWO TIES, TWO READINGS. A spread over the WEIGHT can say that every candidate ties; it cannot say WHY, and
   the two answers take opposite actions. On a profile whose store is empty every candidate is unserved and
   every weight is 0 + 1/(0+1) = 1.0 — the order sitting correctly at its entry value. A tie ABOVE items that
   have history is a rank that is not moving with what those items have demonstrated, which is the defect
   class this instrument exists for. `candUnserved` and `candVisMax` are what separate them, and this drives
   both populations so the separation is a property of the census rather than of the sentence describing it. */
let entryTie, servedTie;
{
  const P = pop({ waiting: [job('https://a.test/'), job('https://b.test/')] });
  entryTie = B._bestCandidate(P).census;
  must(entryTie.candWMax === entryTie.candWMin && entryTie.candWMax === 1,
       'an empty store ranks every candidate at the entry value 1.0', entryTie);
  must(entryTie.candUnserved === entryTie.cands && entryTie.candVisMax === 0,
       'and says so: every member of the tie has never been served', entryTie);
}
/* EQUAL VISITS AS WELL AS EQUAL REWARD, WHICH IS WHAT MAKES THIS A TIE AT ALL: the optimism term is
   `1/(visits+1)`, so two items of the same emit-per-visit at DIFFERENT visit counts do not tie — they are
   correctly separated by how much each has already been tried, which is the term doing its job. */
{
  const P = pop({ idx: idxOf(row('k1', 'https://c.test/', 2, 2), row('k2', 'https://d.test/', 2, 2)) });
  servedTie = B._bestCandidate(P).census;
  must(servedTie.candWMax === servedTie.candWMin,
       'two served candidates of equal emit-per-visit tie as well', servedTie);
  must(servedTie.candUnserved === 0 && servedTie.candVisMax > 0,
       'and the census says THIS tie stands above items with history', servedTie);
}
must((entryTie.candWMax === entryTie.candWMin) && (servedTie.candWMax === servedTie.candWMin) &&
     ((entryTie.candVisMax === 0) !== (servedTie.candVisMax === 0)) &&
     ((entryTie.candUnserved === entryTie.cands) !== (servedTie.candUnserved === servedTie.cands)),
     'the SAME tie in the weight is two different readings — this is the whole discriminating power of the ' +
     'census, and it is what a rank frozen at a constant would erase',
     { entry: entryTie, served: servedTie });
console.log('[level1]   entry-value tie  ' + JSON.stringify(entryTie));
console.log('[level1]   served tie       ' + JSON.stringify(servedTie));

/* AND THE DEFECT ITSELF, DRIVEN. A waiting document whose ADDRESS this store already knows must rank by that
   address's own history — the fetches spent there and the surface they demonstrated — and not by the
   unserved constant. This is the exact shape that stood here: every waiting document at 1.0 for ever. */
{
  const known = row('k1', 'https://a.test/', 9, 3);
  const P = pop({ waiting: [job('https://a.test/'), job('https://z.test/')], idx: idxOf(known) });
  const cen = B._bestCandidate(P).census;
  must(kindWMax(cen, 'doc') === B.frontierWeight(known) && kindWMax(cen, 'doc') > 1,
       'a waiting document ranks by what this profile has already spent and learned AT ITS ADDRESS — a ' +
       '`candDocWMax` of exactly 1.0 beside a known address is the frozen constant back again', cen);
  must(cen.candVisMax === known.visits && cen.candUnserved === 1,
       'the census reports the history that produced that rank, so the number is checkable rather than ' +
       'merely non-constant', cen);
  console.log('[level1]   doc by history   ' + JSON.stringify(cen));
}

console.log('[level1] ── the weight that leaves the order ──');

/* AN EXCLUSION MOVES A ROW'S WEIGHT; IT DOES NOT DELETE IT. A parked entry and the document that holds its
   address are ONE work item, so the row leaves this order and the item standing for it must carry what it was
   worth. Each exclusion row is driven at least once here: a row nobody has ever produced is a row nobody
   knows works, and every one of them is a place a weight can leave the order in silence. */
{
  const held = row('k1', 'https://a.test/', 9, 3);
  const P = pop({ waiting: [job('https://a.test/')], idx: idxOf(held) });
  const cen = B._bestCandidate(P).census;
  must(cen.exclLive === 1 && kindPop(cen, 'cold') === 0,
       'a residue whose address a waiting document holds leaves the cold order', cen);
  must(kindWMax(cen, 'doc') === B.frontierWeight(held),
       'and the document that excluded it carries exactly the weight that left', cen);
  console.log('[level1]   exclLive         ' + JSON.stringify(cen));
}
{
  const r1 = row('k1', 'https://a.test/', 4, 2);
  const cen = B._bestCandidate(pop({ resident: [eng('https://b.test/', 'k1')], idx: idxOf(r1) })).census;
  must(cen.exclHeld === 1 && kindPop(cen, 'cold') === 0 && cen.cands === 0,
       'a residue an instance already holds by KEY is not a candidate for a second instance', cen);
  console.log('[level1]   exclHeld         ' + JSON.stringify(cen));
}
{
  const cen = B._bestCandidate(pop({ idx: idxOf(row('k1', 'https://a.test/', 4, 2, { stranded: true })) })).census;
  must(cen.exclStranded === 1 && cen.cands === 0,
       'a stranded residue has no bytes to build an instance over, so it is excluded and COUNTED', cen);
  console.log('[level1]   exclStranded     ' + JSON.stringify(cen));
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
                                     idx: idxOf(row('k1', 'https://a.test/admin', 3, 1)) })).census;
  must(cen.exclSeedParked === 1 && kindPop(cen, 'seed') === 0 && kindPop(cen, 'cold') === 1,
       'a declared route this profile already holds a residue for is admitted as THAT residue, once', cen);
  console.log('[level1]   exclSeedParked   ' + JSON.stringify(cen));
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
  const e1 = { state: 'hot', w: 5, _readyP: null };
  poolArr.push(e1);
  const asked = pop({ waiting: [job('https://a.test/')],
                      idx: idxOf(row('k1', 'https://c.test/', 6, 3)) });
  let admits = 0, steps = 0;
  const ops = {
    weight: (e) => e.w,
    /* THE ADMISSION ASKS THE ORDER ON THE FIRST ROUND AND ANSWERS THE POSITIVE `null` AFTERWARDS — the two
       shapes hostSchedule asserts at the seam, both driven. */
    admit: async () => (admits++ === 0 ? B._bestCandidate(asked).census : null),
    setFloor: async () => {},
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

console.log('[level1] ── verdict ──');
console.log('[level1]   ' + checks + ' relation(s) checked over the shipped `self._level1`, ' +
            failures + ' broken');
if (failures) {
  console.log('[level1] FAILED — the Level-1 order is composed entirely in bridge.js and no result document ' +
              'can carry it, so a broken relation here is a defect nothing else in this tree can see.');
  process.exit(1);
}
console.log('[level1] OK');
