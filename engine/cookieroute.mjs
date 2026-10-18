/* The zone's Set-Cookie routing, driven: `node engine/cookieroute.mjs`.
 *
 * Every [URL, Set-Cookie field] pair a zone load answers belongs to the engine jar of the agent cluster its URL's
 * origin keys in the document's browsing-context group (bridge.js's zoneRouteSetCookies). That decision is made
 * only in bridge.js, so no engine fixture can see it; this driver loads bridge.js verbatim into a vm realm, as
 * engine/level1.mjs does, and drives the shipped functions over instances it composes. An instance here is the
 * record those functions read (`cluster`, `groupId`, `cookiesOpen`, and a renderer whose `receiveCookies` and
 * `hostAnswer` record what crossed).
 *
 * Every check is a relation over what crossed, never a stored expected output. Each relation is also run once
 * over an invented misroute (the negative control, which it must reject) and over the armed reading (which must
 * carry deliveries, so a relation over nothing cannot pass).
 */
import { createContext, runInContext } from 'node:vm';
import { loadZone } from './zone.mjs';


/* This realm has no IndexedDB, so the zone's load-time restores reject through frontierFail's DFAIL; they are
   not the routing, so each is counted and any other rejection kills the run (level1.mjs's rule). */
let restoresRefused = 0;
process.on('unhandledRejection', (e) => {
  if (e && e.apiclientFatal && /IndexedDB preference read failed \(indexedDB is not defined\)/.test(e.message)) {
    restoresRefused++;
    return;
  }
  throw e;
});
async function swRpc(method) { throw new Error('this driver answers no service-worker call `' + method + '`'); }
const ZONE = (() => {
  /* `_globalStoreReady` is offscreen-brain.js's read of the cumulative store, which this realm does not load;
     it never settles, so bridge.js's start-up kick never drives the live pool under these rows. */
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
for (const n of ['zoneRouteSetCookies', 'zoneAnswerSetCookies', 'zoneOpenJar', 'zoneReclaimHeldCookies',
                 'zoneTabClosed', 'engineDeliverDocument', 'clusterKeyOf'])
  if (typeof B[n] !== 'function')
    throw new Error('extension/bridge.js did not export `' + n + '` — this driver is the only reader of the ' +
                    'zone\'s cookie routing');
/* The zone's own registers, the ones the shipped functions read and write. */
const POOL = runInContext('_pool', ZONE);
const HELD = runInContext('_heldCookies', ZONE);

let checks = 0, failures = 0;
function must(cond, relation, reading) {
  checks++;
  if (cond) return;
  failures++;
  console.log('[cookieroute] BROKEN  ' + relation);
  if (reading !== undefined) console.log('[cookieroute]          reading: ' + JSON.stringify(reading));
}
const reset = () => { POOL.length = 0; HELD.clear(); };
const key = (groupId, origin) => B.clusterKeyOf({ groupId, origin });
/* An instance of cluster (groupId, origin), recording every pair list and answer that crosses to it. */
function inst(groupId, origin, open) {
  const e = { state: 'hot', groupId, origin, cluster: key(groupId, origin), cookiesOpen: open,
              msg: { groupId, origin }, got: [], answers: [] };
  e.r = { renderer: {
    receiveCookies: async ({ setCookies }) => { e.got.push(...JSON.parse(setCookies)); return {}; },
    hostAnswer: async ({ answer }) => { e.answers.push(JSON.parse(answer)); return {}; },
  } };
  POOL.push(e);
  return e;
}
const originOfPair = (p) => new URL(p[0]).origin;
const same = (a, b) => JSON.stringify([...a].sort()) === JSON.stringify([...b].sort());

/* The routing relation over one reading: every pair an instance received or was answered is its own cluster's,
   and the pairs that crossed plus the pairs held are exactly the pairs fed. `crossed` is [[instance, pairs]]. */
function routedToOwnCluster(crossed) {
  return crossed.every(([e, ps]) => ps.every((p) => key(e.groupId, originOfPair(p)) === e.cluster));
}
function conserved(fed, crossed, heldBefore) {
  const out = crossed.flatMap(([, ps]) => ps);
  const held = [...HELD.values()].flat().filter((p) => !heldBefore.includes(p));
  return same(fed.map(String), out.concat(held).map(String));
}

console.log('[cookieroute] ── a redirect hop on another origin ──');
{
  reset();
  const A = inst(7, 'https://a.test', true), P = inst(7, 'https://p.test', true);
  const Q = inst(8, 'https://p.test', true);   // the same origin in another group
  const fed = [['https://p.test/hop', 'h=1'], ['https://a.test/x', 'a=1'], ['https://z.test/hop', 'z=1']];
  await B.engineDeliverDocument(A, 41, { url: 'https://a.test/x', headers: {}, bytes: null, declined: null,
                                         setCookies: fed });
  must(A.answers.length === 1, 'the asking instance is answered once', A.answers);
  const answered = A.answers[0].setCookies;
  const crossed = [[A, answered], [P, P.got], [Q, Q.got]];
  must(answered.length > 0 && P.got.length > 0, 'armed: the reading carries an answered pair and a routed one',
       { answered, peer: P.got });
  must(routedToOwnCluster(crossed), 'every pair reached only the instance of its own (group, origin) cluster',
       { answered, peer: P.got, otherGroup: Q.got });
  must(Q.got.length === 0, 'an instance of the same origin in another group received nothing', Q.got);
  must(conserved(fed, crossed, []), 'answered, routed and held pairs are exactly the pairs the load carried',
       { fed, answered, peer: P.got, held: [...HELD] });
  must(HELD.has(key(7, 'https://z.test')) && !HELD.has(key(8, 'https://z.test')),
       'a pair of an origin with no instance is held for that origin\'s cluster in the asking group', [...HELD]);
  /* Negative control: the same relation over an invented misroute, where the peer pair went to the answer. */
  must(!routedToOwnCluster([[A, [fed[0]]]]), 'negative control: a peer\'s pair in the asking instance\'s answer ' +
       'is rejected by the routing relation');
}

console.log('[cookieroute] ── a document created in a peer finds its pairs ──');
{
  reset();
  const fed = [['https://n.test/', 'n=1; Path=/'], ['https://n.test/hop', 'n2=1']];
  await B.zoneRouteSetCookies(7, fed);
  must(HELD.has(key(7, 'https://n.test')) && HELD.get(key(7, 'https://n.test')).length === fed.length,
       'pairs for a cluster with no instance are held for it', [...HELD]);
  const N = inst(7, 'https://n.test', false);
  await B.zoneRouteSetCookies(7, [['https://n.test/late', 'l=1']]);
  must(N.got.length === 0, 'a booting instance whose jar is not open receives nothing yet', N.got);
  await B.zoneOpenJar(N);
  must(N.got.length === fed.length + 1 && routedToOwnCluster([[N, N.got]]),
       'the instance rooted for the cluster is handed every pair held for it, at its root', N.got);
  must(N.cookiesOpen === true && !HELD.has(N.cluster), 'its jar is open and nothing is held for it any more',
       [...HELD]);
  await B.zoneRouteSetCookies(7, [['https://n.test/joined', 'j=1']]);
  must(N.got.length === fed.length + 2 && !HELD.has(N.cluster),
       'a later document joined to the open instance has its pairs delivered at once', N.got);
  let threw = false;
  try { await B.zoneOpenJar(N); } catch (e) { threw = !!(e && e.apiclientFatal); }
  must(threw, 'opening a jar twice aborts');
  /* Negative control: an invented instance of another origin holding these pairs fails the relation. */
  must(!routedToOwnCluster([[inst(7, 'https://m.test', true), fed]]),
       'negative control: n.test pairs at an m.test instance are rejected by the routing relation');
}

console.log('[cookieroute] ── held pairs are reclaimed with their group ──');
{
  reset();
  /* A zone-minted group: its documents are the instances this zone holds for it. */
  const S = inst('swap:1', 'https://x.test', true);
  await B.zoneRouteSetCookies('swap:1', [['https://q.test/', 'q=1']]);
  await B.zoneRouteSetCookies(9, [['https://z.test/', 'z=1']]);
  await B.zoneRouteSetCookies(10, [['https://z.test/', 'z=1']]);
  const T = inst(9, 'https://a.test', true);
  must(HELD.has(key('swap:1', 'https://q.test')) && HELD.has(key(9, 'https://z.test')) &&
       HELD.has(key(10, 'https://z.test')), 'armed: three groups hold pairs', [...HELD]);
  B.zoneReclaimHeldCookies('swap:1');
  must(HELD.has(key('swap:1', 'https://q.test')), 'a group with an instance left keeps its held pairs', [...HELD]);
  POOL.splice(POOL.indexOf(S), 1);
  B.zoneReclaimHeldCookies('swap:1');
  must(!HELD.has(key('swap:1', 'https://q.test')), 'a zone group\'s held pairs go with its last instance', [...HELD]);
  await B.zoneRouteSetCookies('swap:1', [['https://q.test/late', 'q=2']]);
  must(!HELD.has(key('swap:1', 'https://q.test')), 'a load landing after its group is gone holds nothing', [...HELD]);
  B.zoneTabClosed(9);
  must(HELD.has(key(9, 'https://z.test')), 'a closed tab whose group still has an instance keeps its pairs',
       [...HELD]);
  POOL.splice(POOL.indexOf(T), 1);
  B.zoneReclaimHeldCookies(9);
  must(!HELD.has(key(9, 'https://z.test')), 'a tab group\'s held pairs go when its tab is closed and its last ' +
       'instance has left', [...HELD]);
  /* Negative control: an open tab with no instance is a live group, and nothing of it is reclaimed. */
  B.zoneReclaimHeldCookies(10);
  must(HELD.has(key(10, 'https://z.test')), 'negative control: an open tab\'s group keeps its held pairs', [...HELD]);
}

/* One turn of the event loop, so the load-time restores' rejections are counted before the verdict. */
await new Promise((r) => setTimeout(r, 0));
console.log('[cookieroute] ── verdict ──');
console.log('[cookieroute]   ' + restoresRefused + ' load-time IndexedDB restore(s) this realm cannot perform');
console.log('[cookieroute]   ' + checks + ' relation(s) checked, ' + failures + ' broken');
if (failures) {
  console.log('[cookieroute] FAILED');
  process.exit(1);
}
console.log('[cookieroute] OK');
process.exit(0);
