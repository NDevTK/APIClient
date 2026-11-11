// The egress default and the fired-send record (extension/lib/safe-fetch.js) as engine/trusted.mjs's zone runs
// them: every row is a safeFetch, or a firing question, through that zone against a loopback server that counts
// what reached it. A row reads the reply record's `egress` list (one entry per send asked for: the signal vector
// judged, what admitted it, whether the wire was reached) beside the server's count. The control runs first: an
// unforced page request reaches the server, so a later zero is the policy. The groups, in order:
// the default (a request leaves an unwidened origin only as the page's own act on its unforced run, whatever
// its destination; the seed is the one load this zone composes that leaves; a refusal names the fact that
// decided); the record (a refused request byte-identical to a send the document's unforced run made is answered
// from that send's record); the address class (shown, deciding nothing by default; an address with no bytes
// never leaves); the method (the page's own non-GET on its unforced run is sent as composed, no widening sends
// one, a redirect rewrites it as Fetch §4.5 says); the request's bytes (one row states the weakest class over
// the address, each header value and the body; a part with no bytes never leaves, whichever part it is).
//
//   node testing/egress_rows.mjs
"use strict";
import { createServer } from 'node:http';
import { trustedZone } from '../engine/trusted.mjs';
import { siteRegistrableDomains } from '../engine/psl.mjs';
import { NAVIGABLE_NOTICE } from '../engine/noticegen.mjs';

const hits = new Map();
const count = (method, path) => hits.get(method + ' ' + path) || 0;
/* The request bodies each "METHOD path" arrived with, in arrival order. */
const bodies = new Map();
const bodiesOf = (method, path) => (bodies.get(method + ' ' + path) || []).join('|');
/* The `X-Probe` header each "METHOD path" arrived with, in arrival order; `-` for an arrival with none. */
const probes = new Map();
const probesOf = (method, path) => (probes.get(method + ' ' + path) || []).join('|');
const server = createServer(async (req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  const key = req.method + ' ' + path;
  hits.set(key, (hits.get(key) || 0) + 1);
  const chunks = [];
  for await (const c of req) chunks.push(c);
  bodies.set(key, (bodies.get(key) || []).concat(Buffer.concat(chunks).toString('utf8') + '<' +
                                                  (req.headers['content-type'] || '') + '>'));
  probes.set(key, (probes.get(key) || []).concat(req.headers['x-probe'] === undefined ? '-' : req.headers['x-probe']));
  if (path === '/post-303' || path === '/post-307' || path === '/hdr-303') {
    res.writeHead(Number(path.slice(-3)), { Location: { '/post-303': '/post-landed', '/post-307': '/post-kept',
                                                        '/hdr-303': '/hdr-landed' }[path],
                                            'Cache-Control': 'no-store' });
    res.end();
    return;
  }
  if (path === '/hop') {
    res.writeHead(302, { Location: '/landed', 'Cache-Control': 'no-store' });
    res.end();
    return;
  }
  res.writeHead(200, { 'Content-Type': path.endsWith('.js') ? 'text/javascript' : 'text/plain',
                       'Access-Control-Allow-Origin': '*', 'Cache-Control': 'no-store' });
  res.end(`${key} #${hits.get(key)}`);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const PORT = server.address().port;
/* Two origins on one server: A stays as every origin starts, W is widened by the person. */
const A = `http://127.0.0.1:${PORT}`;
const W = `http://localhost:${PORT}`;

const ZONE = trustedZone();
ZONE.httpCacheStated(null, null);
ZONE.safeFetchEgressStated({});
ZONE.safeFetchWiden(W);
/* An origin whose person permitted forced paths and no other row; nothing is sent to it. */
const PART = 'http://partial.test';
ZONE.safeFetchPermit(PART, 'provenance', 'forced', true);
ZONE.swRpc = async (api, arg) => {
  if (api === 'site.registrableDomains') return siteRegistrableDomains(arg);
  throw new Error('this test serves no rpc ' + JSON.stringify(api));
};

const docOf = (page, reach, driven = 'undriven', pinned = 'unpinned') =>
  ZONE.docRecord(reach, driven, pinned, NAVIGABLE_NOTICE.sandbox.none, ZONE.safeFetchDocumentAddressSpace(page, null));

/* A page request as engine/trusted.mjs's workFetch states it, from the document `docKey` at `page`. */
async function ask(page, url, docKey, { destination = '', mode = 'cors', provenance = 'derived', pinned = 'unpinned',
                                        driven = 'undriven', reach = 'observed', headers,
                                        address = 'concrete', method = 'GET', body = null,
                                        bodyClass = body === null ? undefined : 'concrete' } = {}) {
  const origin = new URL(page).origin;
  return ZONE.safeFetch(url, {
    pageOrigin: origin, destination, mode, provenance, pinned, driven, address,
    method, body, ...(bodyClass === undefined ? {} : { bodyClass }),
    actor: 'page', credentialed: false, credentials: 'same-origin', useCorsPreflight: false, headers,
    doc: docOf(page, reach),
    identity: await ZONE.pageRequestIdentity(origin, page, docKey, url, 'same-origin', mode,
                                             'strict-origin-when-cross-origin', page) });
}
/* A navigation as engine/trusted.mjs's navigate states it, started by the document at `page`. */
const navigate = (page, url, { provenance = 'derived', pinned = 'unpinned', driven = 'undriven', reach = 'observed' } = {}) =>
  ZONE.safeFetch(url, { destination: 'document', mode: 'navigate', provenance, pinned, driven, actor: 'page',
                        method: 'GET', body: null, address: 'unstated', doc: docOf(page, reach), credentialed: false });
/* A load this zone composes: the seed as engine/trusted.mjs states it, or (derived) a probe as
   lib/discovery-probe.js states one. */
const toolAsk = (page, url, provenance) =>
  ZONE.safeFetch(url, { pageOrigin: new URL(page).origin, destination: provenance === 'observed' ? 'document' : '',
                        mode: provenance === 'observed' ? 'navigate' : 'cors', provenance,
                        pinned: provenance === 'observed' ? 'unpinned' : 'unstated', actor: 'tool', driven: 'unstated',
                        address: 'concrete', method: 'GET', body: null,
                        doc: docOf(page, 'observed'), credentialed: false, credentials: 'omit', useCorsPreflight: false });
const text = (r) => new TextDecoder().decode(r.body);
const refusalOf = (r) => r.refusal ? r.refusal.reason : 'sent';
/* The firing question alone, for an act that sends nothing here. */
const firing = (url, facts) => ZONE.safeFetchFiringRefusal(Object.assign(
  { url, destination: '', provenance: 'derived', pinned: 'unpinned', actor: 'page', driven: 'undriven',
    address: 'concrete', bodyClass: 'none', method: 'GET', doc: docOf(PAGE_A, 'observed'), credentialed: false,
    headers: null }, facts));
const SIGNAL_NAMES = ZONE.safeFetchSignals().map((s) => s.name).sort().join(',');
/* The one entry of a reply record's `egress` list, or a word saying why there is not exactly one. */
const only = (r) => !Array.isArray(r.egress) ? 'no-egress-list' : r.egress.length !== 1 ? `entries=${r.egress.length}` : r.egress[0];
const names = (e) => e && e.vector ? Object.keys(e.vector).sort().join(',') : 'no-vector';
const hows = (r) => Array.isArray(r.egress) ? r.egress.map((e) => e.how).join(',') : 'no-egress-list';
const arm = (e) => !e || typeof e !== 'object' ? 'no-entry' : e.admittedBy === null ? 'none' : e.admittedBy.kind;
/* An entry's statement of what each part's bytes are, as one text; `no-parts` where it states none. */
const parts = (e) => !e || typeof e !== 'object' || !e.parts ? 'no-parts'
  : `address=${e.parts.address} headers=${e.parts.headers.map((h) => h.join(':')).join(',')} body=${e.parts.body}`;
/* The `bytes` row of the vector the chokepoint computes for a hypothetical request. */
const bytesOf = (facts) => {
  try {
    return ZONE.safeFetchSignalVector(Object.assign(
      { url: `${A}/x`, destination: '', provenance: 'derived', pinned: 'unpinned', actor: 'page', driven: 'undriven',
        address: 'concrete', bodyClass: 'none', method: 'GET', doc: docOf(PAGE_A, 'observed'), credentialed: false,
        headers: null }, facts)).bytes;
  } catch (_) { return 'aborted'; }
};

const PAGE_A = `${A}/page`, PAGE_W = `${W}/page`;
const FORM = [['Content-Type', 'application/x-www-form-urlencoded', 'concrete']];
const ROWS = [
  { label: 'CONTROL: an unforced page request is sent, and its send records the vector and the admitting arm',
    run: async () => {
      const r = await ask(PAGE_A, `${A}/own`, 'engine:own'), e = only(r);
      return [['GET /own', count('GET', '/own'), 1], ['how', e.how, 'sent'], ['admitted by', arm(e), 'default'],
              ['the vector names every signal', names(e), SIGNAL_NAMES],
              ['an undetermined signal is written', e.vector && e.vector.lineage, 'unknown'],
              ['the arm states why', typeof (e.admittedBy && e.admittedBy.why), 'string']]; } },
  { label: 'a refused request records the vector it was judged on, and that it was not sent', run: async () => {
      const r = await ask(PAGE_A, `${A}/pin`, 'engine:pin', { provenance: 'forced', pinned: 'pinned' }), e = only(r);
      return [['GET /pin', count('GET', '/pin'), 0], ['grade', r.refusal && r.refusal.kind, 'decline'],
              ['how', e.how, 'not-sent'], ['admitted by', arm(e), 'none'],
              ['witness', e.vector && e.vector.witness, 'pinned'],
              ['the refusal rides the entry', typeof e.refusal, 'string']]; } },
  { label: 'a request a widened origin admits says so', run: async () => {
      const r = await ask(PAGE_W, `${W}/wide`, 'engine:wide', { provenance: 'forced', pinned: 'pinned' }), e = only(r);
      return [['GET /wide', count('GET', '/wide'), 1], ['how', e.how, 'sent'], ['admitted by', arm(e), 'widened'],
              ['the widened origin', e.admittedBy && e.admittedBy.origin, W]]; } },
  { label: 'a byte-identical ask of one document is answered from its record and reaches no other party',
    run: async () => {
      const one = await ask(PAGE_A, `${A}/twice`, 'engine:twice'), two = await ask(PAGE_A, `${A}/twice`, 'engine:twice');
      return [['GET /twice', count('GET', '/twice'), 1], ['first', only(one).how, 'sent'],
              ['second', only(two).how, 'answered-locally']]; } },
  { label: 'each hop of a redirect is its own send with its own entry', run: async () => {
      const r = await ask(PAGE_A, `${A}/hop`, 'engine:hop');
      return [['GET /hop', count('GET', '/hop'), 1], ['GET /landed', count('GET', '/landed'), 1],
              ['entries', hows(r), 'sent,sent'],
              ['the second entry is the hop\'s address', Array.isArray(r.egress) && r.egress.length === 2 && r.egress[1].url,
               `${A}/landed`]]; } },
  { label: 'a request no gate let as far as the firing question records no send', run: async () => {
      const r = await ask(PAGE_A, 'ftp://127.0.0.1/x', 'engine:scheme');
      return [['grade', r.refusal && r.refusal.kind, 'network'], ['entries', hows(r), '']]; } },
  { label: 'an unpinned value request on a forced path is declined, and its record is still written', run: async () => {
      const r = await ask(PAGE_A, `${A}/forced-value`, 'engine:fv', { provenance: 'forced' }), e = only(r);
      return [['GET /forced-value', count('GET', '/forced-value'), 0], ['refusal', refusalOf(r), 'blocked-signal:provenance=forced'],
              ['how', e.how, 'not-sent'], ['the vector is written whole', names(e), SIGNAL_NAMES]]; } },
  { label: 'a program load from a document reached on an assumed navigation is declined', run: async () => {
      const r = await ask(PAGE_A, `${A}/assumed.js`, 'engine:assumed', { destination: 'script', reach: 'forced' });
      return [['GET /assumed.js', count('GET', '/assumed.js'), 0], ['refusal', refusalOf(r), 'blocked-signal:doc-reach=forced']]; } },
  { label: 'the same program load in the unforced run fires', run: async () => {
      const r = await ask(PAGE_A, `${A}/own.js`, 'engine:ownjs', { destination: 'script', provenance: 'observed' });
      return [['GET /own.js', count('GET', '/own.js'), 1], ['refusal', refusalOf(r), 'sent'], ['admitted by', arm(only(r)), 'default']]; } },
  { label: 'a document the unforced run itself navigated to loads its resources', run: async () => {
      const js = await ask(PAGE_A, `${A}/reached.js`, 'engine:reached', { destination: 'script', reach: 'derived' });
      const css = await ask(PAGE_A, `${A}/reached.css`, 'engine:reached', { destination: 'style', reach: 'derived' });
      return [['GET /reached.js', count('GET', '/reached.js'), 1], ['GET /reached.css', count('GET', '/reached.css'), 1],
              ['script', refusalOf(js), 'sent'], ['style', refusalOf(css), 'sent']]; } },
  { label: 'a widened origin fires the forced load', run: async () => {
      const r = await ask(PAGE_W, `${W}/wide.js`, 'engine:widejs', { destination: 'script', provenance: 'forced', reach: 'forced' });
      return [['GET /wide.js', count('GET', '/wide.js'), 1], ['admitted by', arm(only(r)), 'widened']]; } },
  { label: 'a subresource and a navigation are judged as a value request is: forced declined, unforced sent', run: async () => {
      const img = await ask(PAGE_A, `${A}/forced.png`, 'engine:kinds', { destination: 'image', mode: 'no-cors', provenance: 'forced' });
      const nav = await navigate(PAGE_A, `${A}/forced-nav`, { provenance: 'forced' });
      const own = await navigate(PAGE_A, `${A}/own-nav`);
      return [['GET /forced.png', count('GET', '/forced.png'), 0], ['image', refusalOf(img), 'blocked-signal:provenance=forced'],
              ['GET /forced-nav', count('GET', '/forced-nav'), 0], ['navigation', refusalOf(nav), 'blocked-signal:provenance=forced'],
              ['GET /own-nav', count('GET', '/own-nav'), 1], ['the page\'s own navigation', refusalOf(own), 'sent']]; } },
  { label: 'a post to another document is judged by the same rule', run: async () => {
      const post = (marks) => ZONE.safeFetchCrossDocumentRefusal({ targetOrigin: A, marks, doc: docOf(PAGE_A, 'observed') });
      return [['on a forced path', post('forced,undriven,unpinned'), 'provenance=forced'],
              ['on the unforced run', post('derived,undriven,unpinned'), null],
              ['from a document reached by forcing', ZONE.safeFetchCrossDocumentRefusal(
                 { targetOrigin: A, marks: 'derived,undriven,unpinned', doc: docOf(PAGE_A, 'forced') }), 'doc-reach=forced']]; } },
  { label: 'the seed leaves; a probe this tool composed and a request its drive minted do not', run: async () => {
      const seed = await toolAsk(PAGE_A, `${A}/seed`, 'observed');
      const probe = await toolAsk(PAGE_A, `${A}/probe`, 'derived');
      const driven = await ask(PAGE_A, `${A}/driven.js`, 'engine:driven', { destination: 'script', provenance: 'observed', driven: 'driven' });
      return [['GET /seed', count('GET', '/seed'), 1], ['seed', refusalOf(seed), 'sent'],
              ['GET /probe', count('GET', '/probe'), 0], ['probe', refusalOf(probe), 'blocked-signal:actor=tool'],
              ['GET /driven.js', count('GET', '/driven.js'), 0], ['driven', refusalOf(driven), 'blocked-signal:actor=drive']]; } },
  { label: 'a refusal names the fact that decided it', run: async () => {
      return [['a pinned address', firing(`${A}/x`, { pinned: 'pinned' }), 'witness=pinned'],
              ['forced and pinned', firing(`${A}/x`, { provenance: 'forced', pinned: 'pinned' }), 'provenance=forced'],
              ['forced where the person permitted forced paths and no other row',
               firing(`${PART}/x`, { provenance: 'forced' }), 'destination=value'],
              ['pinned at that origin', firing(`${PART}/x`, { provenance: 'forced', pinned: 'pinned' }), 'witness=pinned'],
              ['the unforced run at that origin', firing(`${PART}/x`, {}), null]]; } },
  { label: 'a forced world\'s request identical to one the unforced run sent is served from that send\'s record',
    run: async () => {
      const own = await ask(PAGE_A, `${A}/served`, 'engine:served');
      const forced = await ask(PAGE_A, `${A}/served`, 'engine:served', { provenance: 'forced' }), e = only(forced);
      return [['GET /served', count('GET', '/served'), 1], ['the unforced ask', only(own).how, 'sent'],
              ['the forced ask', refusalOf(forced), 'sent'], ['how', e.how, 'answered-locally'],
              ['admitted by', arm(e), 'record'], ['it names what refused the send', e.admittedBy && e.admittedBy.refused, 'provenance=forced'],
              ['the bytes are the recorded reply\'s', text(forced), 'GET /served #1']]; } },
  { label: 'a forced request that differs in one header, or is another document\'s, is no such send: declined',
    run: async () => {
      await ask(PAGE_A, `${A}/served-h`, 'engine:servedh', { headers: [['Accept', 'text/plain', 'concrete']] });
      const header = await ask(PAGE_A, `${A}/served-h`, 'engine:servedh', { provenance: 'forced', headers: [['Accept', 'text/html', 'concrete']] });
      const other = await ask(PAGE_A, `${A}/served-h`, 'engine:servedh-2', { provenance: 'forced', headers: [['Accept', 'text/plain', 'concrete']] });
      return [['GET /served-h', count('GET', '/served-h'), 1], ['another header', refusalOf(header), 'blocked-signal:provenance=forced'],
              ['another document', refusalOf(other), 'blocked-signal:provenance=forced'],
              ['how', only(header).how + ',' + only(other).how, 'not-sent,not-sent']]; } },
  { label: 'a record a widened send made is not the unforced run\'s: once the origin is narrowed it answers nothing',
    run: async () => {
      const made = await ask(PAGE_W, `${W}/tool-made`, 'engine:toolmade', { provenance: 'forced' });
      ZONE.safeFetchUnwiden(W);
      const after = await ask(PAGE_W, `${W}/tool-made`, 'engine:toolmade', { provenance: 'forced' });
      return [['GET /tool-made', count('GET', '/tool-made'), 1], ['made', arm(only(made)), 'widened'],
              ['after the narrowing', refusalOf(after), 'blocked-signal:provenance=forced']]; } },
  { label: 'an address that is its unknown\'s example is sent on the unforced run, and the entry states the class',
    run: async () => {
      const r = await ask(PAGE_A, `${A}/ex-own`, 'engine:addr', { address: 'example' }).catch(() => null);
      const e = r === null ? null : only(r);
      return [['GET /ex-own', count('GET', '/ex-own'), 1], ['admitted by', arm(e), 'default'],
              ['the class on the vector', e && e.vector && e.vector.bytes, 'example'],
              ['the part that holds it', parts(e), 'address=example headers= body=none'],
              ['the control\'s class', (only(await ask(PAGE_A, `${A}/own`, 'engine:own')).vector || {}).bytes, 'concrete']]; } },
  { label: 'an address with no bytes never leaves, at an unwidened origin or a widened one', run: async () => {
      const refused = async (page, url) => refusalOf(await ask(page, url, 'engine:addr', { address: 'unknown' })
                                                     .catch(() => ({ refusal: { reason: 'aborted' } })));
      const own = await refused(PAGE_A, `${A}/no-bytes`);
      ZONE.safeFetchWiden(W);
      const wide = await refused(PAGE_W, `${W}/no-bytes`);
      const exampleWide = ZONE.safeFetchFiringRefusal({
        url: `${W}/x`, destination: '', provenance: 'forced', pinned: 'unpinned', actor: 'page', driven: 'undriven',
        doc: docOf(PAGE_W, 'observed'), credentialed: false, headers: null, address: 'example', bodyClass: 'none',
        method: 'GET' });
      ZONE.safeFetchUnwiden(W);
      return [['GET /no-bytes', count('GET', '/no-bytes'), 0], ['unwidened', own, 'blocked-signal:bytes=unknown'],
              ['widened', wide, 'blocked-signal:bytes=unknown'],
              ['no grant can name it', ZONE.safeFetchSignalUsable('bytes', 'unknown'), 'never-permitted'],
              ['a widened origin admits a forced example address', exampleWide, null]]; } },
  { label: 'a request that states no address class is refused at the entry', run: async () => {
      const r = await ZONE.safeFetch(`${A}/unclassed`, {
        pageOrigin: A, destination: '', mode: 'cors', provenance: 'derived', pinned: 'unpinned', driven: 'undriven',
        method: 'GET', body: null, actor: 'page', credentialed: false, credentials: 'same-origin', useCorsPreflight: false,
        doc: docOf(PAGE_A, 'observed'),
        identity: await ZONE.pageRequestIdentity(A, PAGE_A, 'engine:addr', `${A}/unclassed`, 'same-origin', 'cors',
                                                 'strict-origin-when-cross-origin', PAGE_A) })
        .then(() => 'answered', () => 'aborted');
      return [['GET /unclassed', count('GET', '/unclassed'), 0], ['the ask', r, 'aborted']]; } },
  { label: 'an unforced POST is sent once with the method, content type and body the page composed', run: async () => {
      const r = await ask(PAGE_A, `${A}/post-own`, 'engine:post', { method: 'POST', body: 'a=1', headers: FORM })
        .catch(() => null);
      const e = r === null ? null : only(r);
      return [['POST /post-own', count('POST', '/post-own'), 1], ['GET /post-own', count('GET', '/post-own'), 0],
              ['what arrived', bodiesOf('POST', '/post-own'), 'a=1<application/x-www-form-urlencoded>'],
              ['admitted by', arm(e), 'default'], ['the method on the vector', e && e.vector && e.vector.method, 'POST'],
              ['the reply', r && text(r), 'POST /post-own #1']]; } },
  { label: 'two POSTs to one address with different bodies are two sends; an identical third is answered from the record',
    run: async () => {
      const post = (body) => ask(PAGE_A, `${A}/post-two`, 'engine:post', { method: 'POST', body, headers: FORM })
        .then(hows, () => 'aborted');
      const made = [await post('a=1'), await post('a=2'), await post('a=1')].join(' ');
      return [['POST /post-two', count('POST', '/post-two'), 2],
              ['what arrived', bodiesOf('POST', '/post-two'),
               'a=1<application/x-www-form-urlencoded>|a=2<application/x-www-form-urlencoded>'],
              ['how each ask was answered', made, 'sent sent answered-locally']]; } },
  { label: 'no widening sends a non-GET: a forced POST is refused at an unwidened origin and at a widened one',
    run: async () => {
      const refused = (page, url, over) => ask(page, url, 'engine:post', Object.assign(
        { method: 'POST', body: 'a=1', headers: FORM, provenance: 'forced' }, over)).then(refusalOf, () => 'aborted');
      const own = await refused(PAGE_A, `${A}/post-forced`);
      ZONE.safeFetchWiden(W);
      const wide = await refused(PAGE_W, `${W}/post-forced`);
      const wideGet = refusalOf(await ask(PAGE_W, `${W}/get-forced`, 'engine:post', { provenance: 'forced' }));
      ZONE.safeFetchUnwiden(W);
      return [['POST /post-forced', count('POST', '/post-forced'), 0],
              ['unwidened', own, 'blocked-signal:provenance=forced'], ['widened', wide, 'blocked-signal:method=POST'],
              ['the control, a forced GET at the widened origin', wideGet, 'sent'],
              ['no grant can name it', ZONE.safeFetchSignalUsable('method', 'POST'), 'default-only']]; } },
  { label: 'a forced POST identical to one the unforced run sent is answered from that send\'s record', run: async () => {
      const r = await ask(PAGE_A, `${A}/post-own`, 'engine:post', { method: 'POST', body: 'a=1', headers: FORM,
                                                                     provenance: 'forced' }).catch(() => null);
      const other = await ask(PAGE_A, `${A}/post-own`, 'engine:post', { method: 'POST', body: 'a=9', headers: FORM,
                                                                         provenance: 'forced' }).then(refusalOf, () => 'aborted');
      return [['POST /post-own', count('POST', '/post-own'), 1], ['how', r === null ? 'aborted' : hows(r), 'answered-locally'],
              ['the reply', r && text(r), 'POST /post-own #1'],
              ['another body is another request', other, 'blocked-signal:provenance=forced']]; } },
  { label: 'a redirect rewrites the method as Fetch §4.5 says: 303 to a GET with no body, 307 unchanged', run: async () => {
      const a = await ask(PAGE_A, `${A}/post-303`, 'engine:post', { method: 'POST', body: 'a=3', headers: FORM })
        .then(text, () => 'aborted');
      const b = await ask(PAGE_A, `${A}/post-307`, 'engine:post', { method: 'POST', body: 'a=7', headers: FORM })
        .then(text, () => 'aborted');
      return [['after the 303', a, 'GET /post-landed #1'], ['what the 303\'s target received', bodiesOf('GET', '/post-landed'), '<>'],
              ['after the 307', b, 'POST /post-kept #1'],
              ['what the 307\'s target received', bodiesOf('POST', '/post-kept'), 'a=7<application/x-www-form-urlencoded>']]; } },
  { label: 'a body that is its unknown\'s example is sent on the unforced run, and the entry states the class',
    run: async () => {
      const r = await ask(PAGE_A, `${A}/post-ex`, 'engine:body', { method: 'POST', body: 'q=en', headers: FORM,
                                                                    bodyClass: 'example' }).catch(() => null);
      const e = r === null ? null : only(r);
      const own = await ask(PAGE_A, `${A}/post-own`, 'engine:post', { method: 'POST', body: 'a=1', headers: FORM,
                                                                      bodyClass: 'concrete' }).catch(() => null);
      return [['POST /post-ex', count('POST', '/post-ex'), 1], ['admitted by', arm(e), 'default'],
              ['the class on the vector', e && e.vector && e.vector.bytes, 'example'],
              ['the part that holds it', parts(e), 'address=concrete headers=Content-Type:concrete body=example'],
              ['a body the page computed', own && parts(own.egress[0]),
               'address=concrete headers=Content-Type:concrete body=concrete'],
              ['a request with no body', parts(only(await ask(PAGE_A, `${A}/own`, 'engine:own'))),
               'address=concrete headers= body=none']]; } },
  { label: 'a body with no bytes never leaves, at an unwidened origin or a widened one', run: async () => {
      const refused = async (page, url) => refusalOf(await ask(page, url, 'engine:body', {
        method: 'POST', body: '{cfg.payload}', headers: FORM, bodyClass: 'unknown' })
        .catch(() => ({ refusal: { reason: 'aborted' } })));
      const own = await refused(PAGE_A, `${A}/post-shape`);
      ZONE.safeFetchWiden(W);
      const wide = await refused(PAGE_W, `${W}/post-shape`);
      ZONE.safeFetchUnwiden(W);
      return [['POST /post-shape', count('POST', '/post-shape'), 0], ['unwidened', own, 'blocked-signal:bytes=unknown'],
              ['widened', wide, 'blocked-signal:bytes=unknown']]; } },
  { label: 'a request with a body that states no class is refused at the entry', run: async () => {
      const r = await ZONE.safeFetch(`${A}/post-unclassed`, {
        pageOrigin: A, destination: '', mode: 'cors', provenance: 'derived', pinned: 'unpinned', driven: 'undriven',
        address: 'concrete', method: 'POST', body: 'a=1', headers: FORM, actor: 'page', credentialed: false,
        credentials: 'same-origin', useCorsPreflight: false, doc: docOf(PAGE_A, 'observed'),
        identity: await ZONE.pageRequestIdentity(A, PAGE_A, 'engine:body', `${A}/post-unclassed`, 'same-origin', 'cors',
                                                 'strict-origin-when-cross-origin', PAGE_A) })
        .then(() => 'answered', () => 'aborted');
      return [['POST /post-unclassed', count('POST', '/post-unclassed'), 0], ['the ask', r, 'aborted']]; } },
  { label: 'a header value with no bytes never leaves, at an unwidened origin or a widened one', run: async () => {
      const asked = (page, url) => ask(page, url, 'engine:hdr', { headers: [['X-Probe', '{cfg.token}', 'unknown']] })
        .catch(() => ({ refusal: { reason: 'aborted' }, egress: [] }));
      const own = await asked(PAGE_A, `${A}/hdr-shape`);
      ZONE.safeFetchWiden(W);
      const wide = await asked(PAGE_W, `${W}/hdr-shape`);
      ZONE.safeFetchUnwiden(W);
      return [['GET /hdr-shape', count('GET', '/hdr-shape'), 0], ['unwidened', refusalOf(own), 'blocked-signal:bytes=unknown'],
              ['widened', refusalOf(wide), 'blocked-signal:bytes=unknown'],
              ['the part that holds it', parts(only(own)), 'address=concrete headers=X-Probe:unknown body=none']]; } },
  { label: 'a header value that is its unknown\'s example is sent as those bytes, and the entry states the class',
    run: async () => {
      const r = await ask(PAGE_A, `${A}/hdr-ex`, 'engine:hdr', {
        headers: [['Accept', 'text/plain', 'concrete'], ['X-Probe', 'en-US', 'example']] }).catch(() => null);
      const e = r === null ? null : only(r);
      return [['GET /hdr-ex', count('GET', '/hdr-ex'), 1], ['what arrived', probesOf('GET', '/hdr-ex'), 'en-US'],
              ['admitted by', arm(e), 'default'], ['the class on the vector', e && e.vector && e.vector.bytes, 'example'],
              ['the part that holds it', parts(e), 'address=concrete headers=Accept:concrete,X-Probe:example body=none']]; } },
  { label: 'the row states the weakest class over the address, each header value and the body', run: async () => {
      const H = (...classes) => classes.map((c, i) => ['X-' + i, 'v', c]);
      return [['every part computed', bytesOf({ headers: H('concrete'), bodyClass: 'concrete', method: 'POST' }), 'concrete'],
              ['an example address', bytesOf({ address: 'example', bodyClass: 'concrete', method: 'POST' }), 'example'],
              ['one example header among computed ones', bytesOf({ headers: H('concrete', 'example', 'concrete') }), 'example'],
              ['an address of no stated class over an example body',
               bytesOf({ address: 'unstated', bodyClass: 'example', method: 'POST' }), 'unstated'],
              ['one header with no bytes over an unstated address',
               bytesOf({ address: 'unstated', headers: H('example', 'unknown') }), 'unknown'],
              ['a body with no bytes', bytesOf({ bodyClass: 'unknown', method: 'POST' }), 'unknown']]; } },
  { label: 'the address and the body are parts of that row, and no rows of their own', run: async () => {
      return [['the rows named address, body or bytes',
               ZONE.safeFetchSignals().map((s) => s.name).filter((n) => ['address', 'body', 'bytes'].includes(n)).join(','), 'bytes'],
              ['its values', (ZONE.safeFetchSignals().find((s) => s.name === 'bytes') || { values: [] }).values.join(','),
               'concrete,example,unknown,unstated'],
              ['a grant naming the address', ZONE.safeFetchSignalUsable('address', 'example'), 'not-a-signal'],
              ['a grant naming the body', ZONE.safeFetchSignalUsable('body', 'example'), 'not-a-signal']]; } },
  { label: 'a header value that states no class is refused at the entry', run: async () => {
      const r = await ask(PAGE_A, `${A}/hdr-unclassed`, 'engine:hdr', { headers: [['X-Probe', 'v']] })
        .then(() => 'answered', () => 'aborted');
      return [['GET /hdr-unclassed', count('GET', '/hdr-unclassed'), 0], ['the ask', r, 'aborted']]; } },
  { label: 'a redirect that drops a header drops its class with it', run: async () => {
      const r = await ask(PAGE_A, `${A}/hdr-303`, 'engine:hdr', {
        method: 'POST', body: 'a=3', headers: [['Content-Type', 'application/x-www-form-urlencoded', 'example'],
                                                ['X-Probe', 'kept', 'concrete']] }).catch(() => null);
      const es = r === null || !Array.isArray(r.egress) ? [] : r.egress;
      return [['POST /hdr-303', count('POST', '/hdr-303'), 1], ['GET /hdr-landed', count('GET', '/hdr-landed'), 1],
              ['what the target received', probesOf('GET', '/hdr-landed') + bodiesOf('GET', '/hdr-landed'), 'kept<>'],
              ['the first send', parts(es[0]), 'address=concrete headers=Content-Type:example,X-Probe:concrete body=concrete'],
              ['the hop', parts(es[1]), 'address=concrete headers=X-Probe:concrete body=none'],
              ['the hop\'s class', es[1] && es[1].vector && es[1].vector.bytes, 'concrete']]; } },
  { label: 'the zone counts its sends by how each left and what admitted it', run: async () => {
      const c = typeof ZONE.safeFetchEgressCensus === 'function' ? ZONE.safeFetchEgressCensus() : null;
      const sum = c === null ? -1 : Object.values(c).reduce((a, n) => a + n, 0);
      /* Sent by a default arm: own, twice, hop, landed, own.js, reached.js, reached.css, own-nav, seed. Sent
         widened: wide, wide.js. Not sent: pin, forced-value, assumed.js, forced.png, forced-nav, probe,
         driven.js. Twice's second ask was answered locally. The third group adds served and served-h sent by
         a default arm, tool-made sent widened, three asks not sent and one served from the record. The fourth
         adds ex-own sent by a default arm and the control's second ask answered locally, and two asks not sent. */
      /* The fifth group adds seven sent by a default arm (post-own, post-two twice, and each redirect's two
         hops), one sent widened, one answered locally, three asks not sent and one served from the record. */
      /* The sixth group adds post-ex sent by a default arm, post-own's repeat and the bodiless control each
         answered locally, and two asks not sent. */
      /* The seventh group adds hdr-ex and the redirect's two hops sent by a default arm, and two asks not sent. */
      return [['entries counted', sum, 53], ['sent by a default arm', c && c['sent default'], 23],
              ['sent at a widened origin', c && c['sent widened'], 4], ['not sent', c && c['not-sent none'], 19],
              ['answered locally', c && c['answered-locally default'], 5],
              ['served from the record', c && c['answered-locally record'], 2]]; } },
];

let bad = 0;
try {
  for (const row of ROWS) {
    const notes = (await row.run()).map(([what, have, want]) => ({ what, have, want }));
    const ok = notes.every((n) => n.have === n.want);
    if (!ok) bad++;
    console.log(`  ${ok ? 'ok  ' : 'BAD '} ${row.label}\n       ` +
                notes.map((n) => `${n.what}: ${n.have} (want ${n.want})`).join('; '));
  }
} finally {
  server.close();
}
/* The control's send reached the server, or nothing above is a reading. */
if (count('GET', '/own') < 1) { console.log('  CONTROL FAILED: the unforced page request never reached the server'); process.exit(2); }
console.log(bad === 0 ? '  every row agreed' : `  ${bad} ROW(S) DISAGREED`);
process.exit(bad === 0 ? 0 : 1);
