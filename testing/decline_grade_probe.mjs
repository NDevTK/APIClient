// WHICH REFUSALS THE CHOKEPOINT GRADES `decline` FOR A §7.4 NAVIGATION, ASKED OF `safeFetch` ITSELF.
//
// `extension/bridge.js`'s navigation loader aborts where a `decline` reaches a caller whose refusal arm is
// `none` (see DOC_REFUSAL_ARMS), so "can THIS caller be declined at all?" decides what is owed at each of its
// four call sites. `testing/egress_arm_probe.mjs` answers the FIRING question and cannot answer this one: the
// firing policy is two of six `decline` producers in `extension/lib/safe-fetch.js`, and the DESTRUCTIVE DENY
// LIST is not a signal arm — `safeFetchFiringRefusal` knows nothing about it. So the question has to be asked
// of the whole ladder, which means asking `safeFetch`; the census at the end counts every producer.
//
// IT RESTATES NOTHING. The chokepoint is loaded VERBATIM into a realm of its own, exactly as
// `engine/trusted.mjs` and the arm probe load it, and every verdict below is that file's own answer. A second
// copy of the deny list or of the gate order here would be the copy that drifts.
//
// THE PLATFORM EDGE THROWS A MARKED ERROR AND THAT IS THE POSITIVE CONTROL: a refusal probe must first let
// something through. The edge is `fetch` for a network URL and `swRpc`, the relay a credentialed send's identity
// reads its jar through; a vector reaching either passed every pre-request gate, and no socket opens. A `data:`
// URL is answered by the host's own fetch, Fetch §4.3's "data" arm without a network.
//
// THE VECTORS ARE TRANSCRIBED OFF THE CALL SITES, WITH THE GREP NAMED, which is the one thing here a reader
// must check rather than trust — a vector whose call site has moved is a row about nothing.
//
// WHAT IT ESTABLISHED WHEN IT WAS WRITTEN, recorded as the incident and not as a population, because the
// figure is a property of `_DESTRUCTIVE` and of the arm table and both move: a ROUTE SEED fires at an ordinary
// route and is DECLINED `blocked-destructive:<token>` at `/logout`, `/settings/reset`, `/account/delete` and
// `/subscription/cancel` — ordinary routes of any application with authentication — while the notice's own
// `_seedRefusal` pre-screen asks only `safeFetchFiringRefusal` and so lets every one of them be enqueued. The
// AMBIENT seed is exempt from both destructive gates by `provenance !== "observed"` and fires everywhere.
//
//   node testing/decline_grade_probe.mjs
"use strict";
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createContext, runInContext } from 'node:vm';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const EXT = join(dirname(fileURLToPath(import.meta.url)), '..', 'extension');
const REACHED = 'DECLINE_GRADE_PROBE_REACHED_THE_WIRE';

/* The edge of the default realm: no network, no relay. */
const NO_EDGE = {
  fetch: (u, init) => {
    if (String(u).startsWith('data:')) return fetch(u, init);
    throw new Error(REACHED);
  },
  swRpc: () => { throw new Error(REACHED); },
};

function zone(edge, table) {
  const sandbox = { console, URL, TextDecoder, TextEncoder, Headers, fetch: edge.fetch, swRpc: edge.swRpc };
  sandbox.self = sandbox; sandbox.globalThis = sandbox;
  createContext(sandbox);
  /* The ast-worker.html order for the files a credentialed navigation's identity is built and spoken by. */
  for (const f of ['check.js', 'lib/public-suffix-table.js', 'lib/site.js', 'lib/referrer-policy.js',
                   'lib/request-identity.js', 'lib/cookie-store.js', 'speak-as-rule.js', 'lib/speak-as.js',
                   'lib/navigable-notice.js',
                   'lib/document-record.js', 'lib/safe-fetch.js'])
    runInContext(readFileSync(join(EXT, f), 'utf8'), sandbox, { filename: join(EXT, f) });
  for (const n of ['safeFetch', 'safeFetchEgressStated'])
    if (typeof sandbox[n] !== 'function')
      throw new Error(`extension/lib/safe-fetch.js installed no \`${n}\` — this probe reads the chokepoint's ` +
                      'own answer rather than restating its rules, so a load that installs nothing leaves it ' +
                      'with nothing to ask and a clean-looking table of silence');
  /* THE DEFAULT TABLE, STATED ONCE AND EMPTY, WHICH IS WHAT AN UNCONFIGURED ORIGIN HAS — and the chokepoint
     ABORTS on a firing question asked before any host has spoken, so this is not setup: it is the one thing
     that makes every answer below be about the DEFAULT policy, except at an origin a row's realm widens. */
  sandbox.safeFetchEgressStated(table || {});
  return sandbox;
}

/* bridge.js's `navigationLoad` opts, transcribed:
   git grep -n 'const r = await self.safeFetch(abs, { pageOrigin: principalOrigin' extension/bridge.js
   The route seed states the declaration's words (derived, page, its drive and witness marks, here an
   undriven, unpinned declaration) and the ambient seed the tool's (observed, tool, unstated, unstated):
   git grep -n 'navigationLoad(seed.url' extension/bridge.js
   git grep -n 'navigationLoad(msg.seedUrl' extension/bridge.js
   Both are credentialed for a same-origin document, so both speak as `navigationIdentity`'s identity, which
   is identityFor over the speaker. A declared route's speaker is engineIdentity of the declaring document,
   with no referrer and no policy; the ambient seed's is the browser frame's (seedSpeaker), which this realm
   has no browser to ask, so it states the same engine identity. No gate before the edge reads the identity's
   source. */
function navOpts(Z, principal, url, over) {
  const origin = new URL(principal).origin;
  const identity = Z.identityFor(Z.engineIdentity(origin, principal),
                                 { url, requestInit: { credentials: 'include', mode: 'navigate',
                                                       destination: 'document',
                                                       referrer: '', referrerPolicy: null } },
                                 'strict-origin-when-cross-origin');
  return Object.assign({ pageOrigin: origin,
                         destination: 'document', mode: 'navigate', provenance: 'derived',
                         doc: Z.docRecord('observed', 'undriven', 'unpinned', 'none',
                                          Z.safeFetchDocumentAddressSpace(principal, null)),
                         actor: 'page', driven: 'undriven', pinned: 'unpinned', credentials: 'include',
                         credentialed: true, identity }, over || {});
}
const AMBIENT = { provenance: 'observed', actor: 'tool', driven: 'unstated', pinned: 'unstated' };
const PINNED = { pinned: 'pinned' };
const PRINCIPAL = 'https://app.example.com/page';

const ROWS = [
  { label: 'ROUTE SEED, an ordinary route (CONTROL: must reach the wire)',
    url: 'https://app.example.com/dashboard/overview', want: 'WIRE' },
  { label: 'ROUTE SEED, /logout',           url: 'https://app.example.com/logout',              want: 'DECLINE',
    reason: 'blocked-destructive:' },
  { label: 'ROUTE SEED, /settings/reset',   url: 'https://app.example.com/settings/reset',      want: 'DECLINE',
    reason: 'blocked-destructive:' },
  { label: 'ROUTE SEED, /account/delete',   url: 'https://app.example.com/account/delete',      want: 'DECLINE',
    reason: 'blocked-destructive:' },
  { label: 'ROUTE SEED, /subscription/cancel', url: 'https://app.example.com/subscription/cancel', want: 'DECLINE',
    reason: 'blocked-destructive:' },
  /* A declaration whose path pinned a witness, at an origin nobody widened: the firing walk's own refusal. */
  { label: 'ROUTE SEED, a pinned declaration at an unwidened origin',
    url: 'https://app.example.com/dashboard/overview', want: 'DECLINE', reason: 'blocked-signal:', over: PINNED },
  /* AND THE SAME ADDRESSES AT THE AMBIENT SEED'S TWO WORDS. Both destructive gates are scoped
     `credentialed && provenance !== "observed"`, so this is the one navigation caller they do not reach — the
     loosening is argued at the gate itself and this is where it is exercised rather than assumed. */
  { label: 'AMBIENT SEED, /logout (observed: the deny list does not apply)',
    url: 'https://app.example.com/logout', want: 'WIRE', over: AMBIENT },
  { label: 'AMBIENT SEED, /account/delete (the same)',
    url: 'https://app.example.com/account/delete', want: 'WIRE', over: AMBIENT },
  /* navigationLoad's data: arm is Fetch §4.3's "data" arm and never asks safeFetch, so it cannot decline: a
     well-formed URL is a response and a malformed one (no comma, Fetch §6 "data: URLs") a network error,
     which the loader reports as unavailable. safeFetch itself refuses the scheme. */
  { label: 'DATA ARM, a well-formed data: URL (answered, never asked of safeFetch)',
    url: 'data:text/html,<p>x', want: 'ANSWERED', ask: (Z, row) => Z.safeFetchSchemeData(row.url) },
  { label: 'DATA ARM, a data: URL with no comma (a network error, not a decline)',
    url: 'data:text/html', want: 'REFUSE:network', ask: (Z, row) => Z.safeFetchSchemeData(row.url) },
  { label: 'safeFetch asked for a data: URL (the scheme is refused, not declined)',
    url: 'data:text/html,<p>x', want: 'REFUSE:network' },
  /* Two producers no navigation caller can reach, asked so the census holds all six: navigationLoad sends a
     GET and passes no `onChunk`. */
  { label: 'METHOD, a POST (not a navigation caller\'s: navigationLoad sends GET)',
    url: 'https://app.example.com/api', want: 'DECLINE', reason: 'blocked-method:',
    ask: (Z) => ({ refusal: Z.safeFetchMethodRefusal('POST') }) },
  { label: 'STREAM, a streamed script (not a navigation caller\'s: navigationLoad passes no onChunk)',
    url: 'https://app.example.com/app.js', want: 'DECLINE', reason: 'blocked-stream-body-gated:',
    over: { destination: 'script', mode: 'no-cors', credentials: 'same-origin', credentialed: false,
            identity: undefined, onChunk: () => {} } },
];

/* THE REDIRECT FIXTURE: the hops the zone follows itself (Fetch §4.5 "HTTP-redirect fetch"), each judged by
   every gate before it is sent. A `/hop/<n>` path answers 302 to its entry in `ROUTES`, which the requested
   address does not spell (the deny list reads the query too), with that entry's headers; anything else 200. It
   listens on loopback, and the rows' principal is a loopback page, so Local Network Access admits the request.
   The realm reaches it through the host's fetch with redirect "manual", which a process answers with the 3xx
   itself. Its relay holds a one-cookie jar and keeps the speak-as rules a send installs, applied by exact URL
   and method as declarativeNetRequest applies them, so the server's log is what each hop carried, and it logs
   each cookie the zone writes. Origin A is widened, as `--explore` widens one, and B (the same server under
   `localhost`) is not. C (127.0.0.2) is widened and cross-site with A. D is a domain with a registrable
   domain, `app.example.test`, which the relay sends to A's address; its URL is not potentially trustworthy. */
const hits = [];
const ROUTES = new Map();
const server = createServer((req, res) => {
  hits.push({ path: new URL(req.url, 'http://fixture').pathname, cookie: req.headers.cookie || null,
              referer: req.headers.referer || null });
  const route = ROUTES.get(new URL(req.url, 'http://fixture').pathname);
  if (route) { res.writeHead(route.status, route.headers); res.end(); return; }
  res.writeHead(200, { 'Content-Type': 'text/html' });
  res.end('<p>fixture');
});
await new Promise((resolve) => server.listen(0, resolve));
const PORT = server.address().port;
const A = `http://127.0.0.1:${PORT}`, B = `http://localhost:${PORT}`;
const hop = (to, headers) => ({ status: 302, headers: Object.assign({ Location: to }, headers || {}) });
ROUTES.set('/hop/1', hop('/home')).set('/hop/1b', hop('/home2')).set('/hop/2', hop('/logout'))
  .set('/hop/3', hop(`${B}/landing`)).set('/hop/4', hop(`${B}/hop/5`)).set('/hop/5', hop(`${A}/home5`))
  .set('/hop/7', hop('/logout7')).set('/hop/8', hop(`${B}/hop/9`))
  .set('/hop/9', hop(`${A}/end9`, { 'Access-Control-Allow-Origin': '*' }))
  .set('/end9', { status: 200, headers: { 'Access-Control-Allow-Origin': '*', 'Content-Type': 'text/plain' } });
const setCookie = (fields) => ({ status: 200, headers: { 'Content-Type': 'text/plain', 'Set-Cookie': fields } });
ROUTES.set('/sc/1', setCookie(['host1=a; Path=/', 'dom1=b; Domain=.Example.test', 'forged=x; Domain=evil.example',
                               'pub=x; Domain=test', 'lax1=1; SameSite=Lax', 'sec=1; Secure', 'lock=evil; Path=/',
                               'none1=1; SameSite=None']))
  .set('/sc/2', setCookie(['__Host-ok=1; Secure; Path=/', '__Host-bad=1; Secure; Path=/; Domain=127.0.0.1',
                           'p1=1; Secure; Partitioned; SameSite=None', 'p2=1; Partitioned', 'gone=1; Max-Age=0',
                           'old=1; Expires=Wed, 21 Oct 2015 07:28:00 GMT', 'cap=1; Max-Age=999999999',
                          ]))
  .set('/sc/3', setCookie(['xs=1', 'xsl=1; SameSite=Lax', 'xsn=1; SameSite=None; Secure']))
  .set('/hop/sc', { status: 302, headers: { Location: '/home-sc', 'Set-Cookie': ['hopc=1; Path=/'] } })
  .set('/sc/5', setCookie(['xs=1', 'xsl=1; SameSite=Lax', 'xsn=1; SameSite=None; Secure',
                           'xsp=1; Secure; Partitioned; SameSite=None']))
  .set('/sc/4', setCookie(['pkn=1; Secure; Partitioned; SameSite=None', 'unn=1; Secure; SameSite=None',
                           'lxn=1; SameSite=Lax']));
const C = `http://127.0.0.2:${PORT}`, D = `http://app.example.test:${PORT}`;
ROUTES.set('/hop/tc', hop(`${C}/sc/4`)).set('/sc/6', ROUTES.get('/sc/5')).set('/sc/7', ROUTES.get('/sc/5'))
  .set('/sc/8', ROUTES.get('/sc/5'));
const cookieWrites = [];   // [{name, details, before: the number of server hits when it was written}]
/* C's jar: an unpartitioned None and Lax cookie, and one of whichever partition is asked for. */
const C_JAR = [{ name: 'tp', value: '1', path: '/', sameSite: 'no_restriction' },
               { name: 'tl', value: '1', path: '/', sameSite: 'lax' }];
const speakAsRules = new Map();   // ruleId -> { method, url, requestHeaders }
let zf = null;
const FIXTURE_EDGE = {
  fetch: (u, init) => {
    const s = String(u);
    if (s.startsWith('data:')) return fetch(u, init);
    if (![A, B, C, D].some((o) => s.startsWith(o + '/'))) throw new Error(REACHED);
    const headers = new Headers(init && init.headers ? init.headers : undefined);
    const method = zf.speakAsDnrMethod(init && init.method ? init.method : 'GET');
    for (const r of speakAsRules.values()) {
      if (r.url !== s || zf.speakAsDnrMethod(r.method) !== method) continue;
      for (const h of r.requestHeaders)
        if (h.operation === 'remove') headers.delete(h.header); else headers.set(h.header, h.value);
    }
    return fetch(s.startsWith(D + '/') ? A + s.slice(D.length) : s, Object.assign({}, init, { headers }));
  },
  swRpc: async (api, args) => {
    if (api === 'cookies.getAll' && args.name !== undefined)
      return [{ name: 'lock', value: 'held', domain: '.example.test', path: '/', secure: true }]
        .filter((c) => c.name === args.name);
    if (api === 'cookies.getAll' && args.url.startsWith(C + '/'))
      return args.partitionKey ? [{ name: 'tpp', value: '1', path: '/', sameSite: 'no_restriction',
                                    partitionKey: Object.assign({}, args.partitionKey) }] : C_JAR;
    if (api === 'cookies.getAll')
      return args.partitionKey ? [] : [{ name: 'sid', value: 'probe', path: '/', sameSite: 'lax' }];
    if (api === 'cookies.set') { cookieWrites.push({ name: args.name, details: args, before: hits.length });
                                 return { refused: null }; }
    if (api === 'speakAs.clearFamily') { speakAsRules.clear(); return 0; }
    if (api === 'speakAs.install') {
      for (const r of zf.speakAsCheckedRules(args))
        speakAsRules.set(r.ruleId, { method: r.method, url: args.url, requestHeaders: r.requestHeaders });
      return args.rules.map((r) => r.ruleId);
    }
    if (api === 'speakAs.release') { for (const id of args) speakAsRules.delete(id); return undefined; }
    throw new Error(`the fixture relay was asked \`${api}\`, which no send on this probe makes`);
  },
};
/* A navigation whose speaker sends a Referer (a sub-frame's, `referrer: "client"`), and the page's own
   `fetch()` (bridge.js's `fetched`: uncredentialed, `cors`, the engine's referrer). */
const REFERRING = (Z) => ({ identity: Z.identityFor(Z.engineIdentity(A, `${A}/page`),
                                                     { url: `${A}/hop/1b`, requestInit: { credentials: 'include',
                                                       mode: 'navigate', destination: 'document', referrer: null,
                                                       referrerPolicy: null } },
                                                     'strict-origin-when-cross-origin') });
const PAGE_FETCH = (Z, url) => ({ destination: '', mode: 'cors', credentials: 'same-origin', credentialed: false,
                                  useCorsPreflight: false,
                                  identity: Z.pageRequestIdentity(A, `${A}/page`, url, 'same-origin', 'cors',
                                                                  'strict-origin-when-cross-origin', `${A}/page`) });
/* A credentialed `cors` request of the page's own, which is not a navigation (RFC 6265bis §5.7 step 18). */
const CREDENTIALED_FETCH = (Z, url) => ({ destination: '', mode: 'cors', credentials: 'include', useCorsPreflight: false,
  identity: Z.identityFor(Z.engineIdentity(A, `${A}/page`), { url, requestInit: { credentials: 'include', mode: 'cors',
                          referrer: '', referrerPolicy: null } }, 'strict-origin-when-cross-origin') });
const wrote = (name) => cookieWrites.find((w) => w.name === name);
const FIXTURE_ROWS = [
  { label: 'REDIRECT, harmless: /hop/1 -> /home (CONTROL: must be answered)',
    url: `${A}/hop/1`, want: 'ANSWERED', list: [`${A}/hop/1`, `${A}/home`], sent: ['/home'] },
  { label: 'REDIRECT, a speak-as hop carries the identity\'s Cookie and Referer: /hop/1b -> /home2',
    url: `${A}/hop/1b`, want: 'ANSWERED', over: REFERRING, list: [`${A}/hop/1b`, `${A}/home2`],
    carried: { path: '/home2', cookie: 'sid=probe', referer: `${A}/page` } },
  { label: 'REDIRECT onto a destructive path, credentialed: /hop/2 -> /logout (declined before it is sent)',
    url: `${A}/hop/2`, want: 'DECLINE', reason: 'blocked-destructive-redirect:', list: [`${A}/hop/2`, `${A}/logout`],
    notSent: '/logout' },
  { label: 'REDIRECT onto a destructive path, uncredentialed: /hop/7 -> /logout7 (CONTROL: the list\'s scope, sent)',
    url: `${A}/hop/7`, want: 'ANSWERED', over: () => ({ credentialed: false, identity: undefined }),
    list: [`${A}/hop/7`, `${A}/logout7`], sent: ['/logout7'] },
  { label: 'REDIRECT across a signal: a pinned seed fires at widened A, /hop/3 -> unwidened B (not sent)',
    url: `${A}/hop/3`, want: 'DECLINE', reason: 'blocked-signal-redirect:', over: () => PINNED,
    list: [`${A}/hop/3`, `${B}/landing`], notSent: '/landing' },
  { label: 'REDIRECT, an intermediate cross-origin 3xx with no Access-Control-Allow-Origin: A -> B -> A',
    url: `${A}/hop/4`, want: 'REFUSE:network', reason: `blocked-cors:no-allow-origin:${B}`,
    over: (Z) => PAGE_FETCH(Z, `${A}/hop/4`), notSent: '/home5' },
  { label: 'REDIRECT, an intermediate cross-origin 3xx that allows it: A -> B -> A (CONTROL: answered, cors)',
    url: `${A}/hop/8`, want: 'ANSWERED', type: 'cors', over: (Z) => PAGE_FETCH(Z, `${A}/hop/8`),
    list: [`${A}/hop/8`, `${A}/end9`], sent: ['/hop/9', '/end9'] },
  /* RFC 6265bis §5.7 on a credentialed navigation's reply, row by row in `stored` and `ignored`. */
  { label: 'COOKIES, a navigation of app.example.test: host-only and Domain=example.test stored; a forged Domain, ' +
           'a public suffix, Secure over http, an overlay of a secure cookie and None without Secure ignored',
    principal: `${D}/page`, url: `${D}/sc/1`, want: 'ANSWERED',
    stored: ['host1', 'dom1', 'lax1'], ignored: ['forged', 'pub', 'sec', 'lock', 'none1'],
    check: () => (wrote('host1').details.domain === undefined && wrote('dom1').details.domain === 'example.test' &&
                  wrote('lax1').details.sameSite === 'lax' && wrote('host1').details.sameSite === 'unspecified' &&
                  wrote('host1').details.expirationDate === undefined) },
  { label: 'COOKIES, a navigation of 127.0.0.1 (potentially trustworthy): prefixes, Partitioned, lifetimes',
    url: `${A}/sc/2`, want: 'ANSWERED', stored: ['__Host-ok', 'p1', 'gone', 'old', 'cap'],
    ignored: ['__Host-bad', 'p2'],
    check: () => (JSON.stringify(wrote('p1').details.partitionKey) ===
                    JSON.stringify({ topLevelSite: 'http://127.0.0.1', hasCrossSiteAncestor: false }) &&
                  wrote('__Host-ok').details.partitionKey === undefined && wrote('gone').details.expirationDate === 0 &&
                  wrote('old').details.expirationDate === Date.UTC(2015, 9, 21, 7, 28) / 1000 &&
                  Math.abs(wrote('cap').details.expirationDate - (Date.now() / 1000 + 400 * 86400)) < 60) },
  { label: 'COOKIES, the page\'s credentialed cors fetch of cross-site 127.0.0.2, the third-party-cookie setting ' +
           'at its default (on): its None cookie and its partition\'s are sent, and only SameSite=None is stored',
    url: `${C}/sc/3`, want: 'REFUSE:network', over: (Z) => CREDENTIALED_FETCH(Z, `${C}/sc/3`),
    stored: ['xsn'], ignored: ['xs', 'xsl'], carried: { path: '/sc/3', cookie: 'tp=1; tpp=1', referer: null },
    check: () => wrote('xsn').details.partitionKey === undefined },
  /* This extension's third-party-cookie setting off: a cross-site request that is not a top-level navigation
     sends and stores Partitioned cookies only; a top-level navigation is first-party to its own site. */
  { label: 'THIRD-PARTY COOKIES OFF, the same cors fetch: only the Partitioned cookie is sent and stored',
    url: `${C}/sc/5`, want: 'REFUSE:network', over: (Z) => CREDENTIALED_FETCH(Z, `${C}/sc/5`), thirdPartyAllowed: false,
    stored: ['xsp'], ignored: ['xs', 'xsl', 'xsn'], carried: { path: '/sc/5', cookie: 'tpp=1', referer: null },
    check: () => JSON.stringify(wrote('xsp').details.partitionKey) ===
                   JSON.stringify({ topLevelSite: 'http://127.0.0.1', hasCrossSiteAncestor: true }) },
  { label: 'THIRD-PARTY COOKIES ON, stated: the same cors fetch sends and stores its unpartitioned cookies',
    url: `${C}/sc/6`, want: 'REFUSE:network', over: (Z) => CREDENTIALED_FETCH(Z, `${C}/sc/6`), thirdPartyAllowed: true,
    stored: ['xsn', 'xsp'], ignored: ['xs', 'xsl'], carried: { path: '/sc/6', cookie: 'tp=1; tpp=1', referer: null } },
  /* A per-site exception, the egress table's `third-party-cookies` row of the client's top-level site,
     decides in place of the global setting, both ways. */
  { label: 'THIRD-PARTY COOKIES OFF, 127.0.0.1 excepted "allow": the cors fetch sends and stores unpartitioned ones',
    url: `${C}/sc/7`, want: 'REFUSE:network', over: (Z) => CREDENTIALED_FETCH(Z, `${C}/sc/7`), thirdPartyAllowed: false,
    siteException: 'allow',
    stored: ['xsn', 'xsp'], ignored: ['xs', 'xsl'], carried: { path: '/sc/7', cookie: 'tp=1; tpp=1', referer: null } },
  { label: 'THIRD-PARTY COOKIES ON, 127.0.0.1 excepted "block": only the Partitioned cookie is sent and stored',
    url: `${C}/sc/8`, want: 'REFUSE:network', over: (Z) => CREDENTIALED_FETCH(Z, `${C}/sc/8`), thirdPartyAllowed: true,
    siteException: 'block',
    stored: ['xsp'], ignored: ['xs', 'xsl', 'xsn'], carried: { path: '/sc/8', cookie: 'tpp=1', referer: null } },
  { label: 'THIRD-PARTY COOKIES, the exception round-trips through the persisted table and is no egress grant',
    url: `${A}/`, want: 'ANSWERED', ask: (Z) => {
      Z.safeFetchSetSiteSetting('http://127.0.0.2', 'third-party-cookies', 'block');
      const table = Z.safeFetchEgressTable();
      Z.safeFetchSetSiteSetting('http://127.0.0.2', 'third-party-cookies', null);
      const restored = zone(FIXTURE_EDGE, table);
      const back = restored.safeFetchSiteSetting('http://127.0.0.2', 'third-party-cookies');
      const listed = restored.safeFetchWidenedOrigins().includes('http://127.0.0.2');
      if (back !== 'block' || listed) throw new Error(`restored ${back}, listed as widened ${listed}`);
      return { status: 200 };
    } },
  { label: 'THIRD-PARTY COOKIES OFF, a navigation that hops cross-site to 127.0.0.2: first-party there, its ' +
           'Lax cookie sent, and its Partitioned cookie keyed on the destination\'s site',
    url: `${A}/hop/tc`, want: 'REFUSE:network', thirdPartyAllowed: false,
    stored: ['pkn', 'unn', 'lxn'], ignored: [], carried: { path: '/sc/4', cookie: 'tp=1; tl=1; tpp=1', referer: null },
    check: () => JSON.stringify(wrote('pkn').details.partitionKey) ===
                   JSON.stringify({ topLevelSite: 'http://127.0.0.2', hasCrossSiteAncestor: false }) },
  { label: 'COOKIES, a 3xx\'s Set-Cookie is stored before its hop is sent',
    principal: `${D}/page`, url: `${D}/hop/sc`, want: 'ANSWERED', list: [`${D}/hop/sc`, `${D}/home-sc`],
    stored: ['hopc'], ignored: [],
    check: () => hits.findIndex((h) => h.path === '/home-sc') >= wrote('hopc').before },
  { label: 'COOKIES, an uncredentialed request stores nothing (CONTROL)',
    principal: `${D}/page`, url: `${D}/sc/1`, want: 'ANSWERED',
    over: () => ({ credentialed: false, identity: undefined }), stored: [], ignored: ['host1', 'dom1'] },
];

const z = zone(NO_EDGE);
zf = zone(FIXTURE_EDGE);
zf.safeFetchWiden(A);
zf.safeFetchWiden(C);
zf.safeFetchWiden(D);
let bad = 0, wire = 0, decl = 0;
const census = new Map();
async function run(Z, principal, row) {
  let got, detail, r = null;
  try {
    const over = typeof row.over === 'function' ? row.over(Z) : row.over;
    cookieWrites.length = 0;
    /* The host's restore with nothing stored, unless the row states the person's setting. */
    if (Z === zf) Z.identityThirdPartyCookiesStated(row.thirdPartyAllowed === undefined
      ? Z.identityThirdPartyCookiesRestored(null) : row.thirdPartyAllowed);
    if (Z === zf) Z.safeFetchSetSiteSetting('http://127.0.0.1', 'third-party-cookies', row.siteException || null);
    r = row.ask ? await row.ask(Z, row)
      : await Z.safeFetch(row.url, navOpts(Z, row.principal || principal, row.url, over));
    if (r && r.refusal) { got = r.refusal.kind === 'decline' ? 'DECLINE' : 'REFUSE:' + r.refusal.kind;
                          detail = r.refusal.reason; }
    else { got = 'ANSWERED'; detail = 'status ' + (r && r.status); }
  } catch (e) {
    if (String(e && e.message).indexOf(REACHED) >= 0) { got = 'WIRE'; detail = 'no pre-request gate refused'; }
    else { got = 'THREW'; detail = String(e && e.message); }
  }
  if (got === 'WIRE') wire++;
  if (got === 'DECLINE') {
    decl++;
    const producer = detail.slice(0, detail.indexOf(':') + 1);
    census.set(producer, (census.get(producer) || 0) + 1);
  }
  const notes = [];
  let ok = got === row.want && (row.reason === undefined || detail.startsWith(row.reason));
  /* The cookies the zone wrote, by name: every `stored` one and no `ignored` one, and the row's check over
     what each write stated. */
  if (row.stored) {
    const names = cookieWrites.map((w) => w.name);
    notes.push('wrote ' + JSON.stringify(names));
    if (row.stored.some((n) => !names.includes(n)) || row.ignored.some((n) => names.includes(n))) ok = false;
    if (row.check && row.stored.length && !row.check()) { ok = false; notes.push('a write stated the wrong fields'); }
  }
  /* Fetch §2.2.6 "Responses"' URL list, the requested URL then the landed one: only this zone followed the
     chain, so only its record can say where the bytes came from. */
  if (row.list) {
    const list = r && Array.isArray(r.urlList) ? r.urlList : null;
    notes.push('urlList ' + JSON.stringify(list));
    if (JSON.stringify(list) !== JSON.stringify(row.list)) ok = false;
  }
  /* The server's own log says which hops were sent: a hop the zone declined or refused never was. */
  for (const path of row.sent || []) {
    const was = hits.some((h) => h.path === path);
    notes.push(`${path} ${was ? 'reached' : 'did not reach'} the server`);
    if (!was) ok = false;
  }
  if (row.notSent) {
    const was = hits.some((h) => h.path === row.notSent);
    notes.push(`${row.notSent} ${was ? 'reached' : 'did not reach'} the server`);
    if (was) ok = false;
  }
  if (row.carried) {
    const h = hits.find((x) => x.path === row.carried.path);
    notes.push(`${row.carried.path} carried Cookie ${JSON.stringify(h && h.cookie)} and Referer ` +
               JSON.stringify(h && h.referer));
    if (!h || h.cookie !== row.carried.cookie || h.referer !== row.carried.referer) ok = false;
  }
  if (row.type) {
    notes.push(`type ${JSON.stringify(r && r.type)}`);
    if (!r || r.type !== row.type) ok = false;
  }
  if (!ok) bad++;
  console.log(`  ${ok ? 'ok  ' : 'BAD '} ${row.label}\n       ${got}  ${detail}` +
              (notes.length ? `\n       ${notes.join('; ')}` : ''));
}
try {
  for (const row of ROWS) await run(z, PRINCIPAL, row);
  for (const row of FIXTURE_ROWS) await run(zf, `${A}/page`, row);
} finally {
  server.close();
}
console.log('');
/* The decline producers safe-fetch.js holds, each counted by the token it writes. */
const PRODUCERS = ['blocked-method:', 'blocked-stream-body-gated:', 'blocked-signal:', 'blocked-destructive:',
                   'blocked-destructive-redirect:', 'blocked-signal-redirect:'];
for (const p of PRODUCERS) console.log(`  census ${p.padEnd(30)} ${census.get(p) || 0}`);
for (const p of census.keys()) if (!PRODUCERS.includes(p)) { console.log(`  census UNLISTED ${p}`); bad++; }
const unseen = PRODUCERS.filter((p) => !census.has(p));
/* THE CONTROL IS TWO-SIDED AND THE CLEAN BILL IS WITHHELD UNLESS BOTH SPOKE. A run in which nothing reached
   the wire is a run whose zone refused everything for some reason of its own; a run in which nothing was
   declined is a run that never exercised a deny arm. Either way nothing below is a reading. */
if (wire === 0 || decl === 0) {
  console.log(`  CONTROL DID NOT SEPARATE (${wire} reached the wire, ${decl} were declined) — nothing above ` +
              'is a reading about the deny list.');
  process.exit(2);
}
console.log(`  CONTROLS SEPARATED: ${wire} reached the wire and ${decl} were declined.`);
if (unseen.length) { console.log(`  PRODUCERS NOT EXERCISED: ${unseen.join(' ')}`); bad++; }
console.log(bad === 0 ? '  every row answered as its label expects'
                      : `  ${bad} ROW(S) DISAGREED — read the reason beside each`);
process.exit(bad === 0 ? 0 : 1);
