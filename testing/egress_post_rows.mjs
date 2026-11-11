// The page's own non-GET through the whole native host: engine/trusted.mjs over the native `--abi` binary loads
// a page from a loopback server that records every request, so each row reads what reached the server beside the
// zone's `@EGRESS` lines. The page's unforced boot POSTs JSON and reads the reply; it POSTs two bodies to one
// address; and on an arm that assumes absent state it POSTs to a new address and repeats a POST its unforced run
// already made. It also sends a header whose value is an unknown with an example (`navigator.language`) and, on
// the arm that contradicts that example, the same unknown with no bytes, by fetch() and by XMLHttpRequest. Its
// XMLHttpRequests POST JSON and read the reply, POST binary bytes, and POST on the forced arm. The control is
// the seed load: the server served the page, or nothing below is a reading.
//
//   node testing/egress_post_rows.mjs [path-to-native-binary]      (after `node engine/build.mjs native`)
"use strict";
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ENGINE = join(dirname(fileURLToPath(import.meta.url)), '..', 'engine');
const bin = process.argv[2] || join(ENGINE, 'host', 'out', 'qjs-native-none');
if (!existsSync(bin)) { console.log(`  NO BINARY: ${bin} does not exist; build it with \`node engine/build.mjs native\``); process.exit(2); }

/* `__FLAGS` is absent from the page, so the arm that takes it as present rests on an assumption and its path is
   forced; everything outside that arm is the page's own unforced run. The forced arm starts after the unforced
   POST to /api/same has been answered, so the repeat finds that send's record. */
const PAGE = `<!doctype html><meta charset="utf-8"><title>post</title><script>
fetch('/api/echo', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Page': 'own' },
                     body: JSON.stringify({ a: 1 }) })
  .then(function (r) { return r.json(); })
  .then(function (j) { return fetch('/api/read?n=' + j.n); });
fetch('/api/two', { method: 'POST', body: 'b=1' });
fetch('/api/two', { method: 'POST', body: 'b=2' });
fetch('/api/same', { method: 'POST', body: 's=1' }).then(function () {
  if (window.__FLAGS && window.__FLAGS.role === 'admin') {
    fetch('/api/forced', { method: 'POST', body: 'f=1' });
    fetch('/api/same', { method: 'POST', body: 's=1' });
    var xf = new XMLHttpRequest(); xf.open('POST', '/api/xhr-forced'); xf.send('f=2');
  }
});
var xp = new XMLHttpRequest(); xp.open('POST', '/api/xhr-post'); xp.setRequestHeader('Content-Type', 'application/json');
xp.onload = function () { fetch('/api/xhr-read?n=' + JSON.parse(xp.responseText).n); };
xp.send(JSON.stringify({ x: 1 }));
var xb = new XMLHttpRequest(); xb.open('POST', '/api/xhr-bin'); xb.send(new Uint8Array([0, 255, 1, 128]));
fetch('/api/hdr-ex', { headers: { 'X-Probe': navigator.language } });
var xe = new XMLHttpRequest(); xe.open('GET', '/api/xhr-ex'); xe.setRequestHeader('X-Probe', navigator.language); xe.send();
if (navigator.language !== 'en-US') {
  fetch('/api/hdr-unk', { headers: { 'X-Probe': navigator.language } });
  var xu = new XMLHttpRequest(); xu.open('GET', '/api/xhr-unk'); xu.setRequestHeader('X-Probe', navigator.language); xu.send();
}
</script>`;

/* Every request that arrived, as "METHOD path?query <body> <content-type> <x-page>". */
const arrived = [];
const server = createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  const u = new URL(req.url, 'http://x');
  arrived.push({ method: req.method, path: u.pathname, query: u.search, body: Buffer.concat(chunks).toString('utf8'),
                 hex: Buffer.concat(chunks).toString('hex'),
                 type: req.headers['content-type'] || '', page: req.headers['x-page'] || '',
                 probe: req.headers['x-probe'] === undefined ? '-' : req.headers['x-probe'] });
  if (u.pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(PAGE);
    return;
  }
  res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify({ n: arrived.filter((a) => a.method === req.method && a.path === u.pathname).length }));
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const ORIGIN = `http://127.0.0.1:${server.address().port}`;

/* The run ends when the zone's process does. The backstop is wall time, a distinct ending the verdict states. */
const BACKSTOP_MS = 180000;
const child = spawn(process.execPath, [join(ENGINE, 'trusted.mjs'), ORIGIN + '/', bin],
                    { stdio: ['ignore', 'ignore', 'pipe'] });
let err = '';
child.stderr.on('data', (d) => { err += d; });
const ending = await new Promise((resolve) => {
  const timer = setTimeout(() => { child.kill('SIGTERM'); resolve('backstop'); }, BACKSTOP_MS);
  child.on('error', (e) => { clearTimeout(timer); resolve('never started: ' + e.message); });
  child.on('close', (code, signal) => { clearTimeout(timer); resolve(signal ? 'signal ' + signal : 'exit ' + code); });
});
server.close();

const got = (method, path) => arrived.filter((a) => a.method === method && a.path === path);
const egress = err.split('\n').map((l) => l.indexOf('@EGRESS ') < 0 ? null : JSON.parse(l.slice(l.indexOf('@EGRESS ') + 8)))
  .filter((e) => e !== null);
const sends = (method, path) => egress.filter((e) => e.method === method && new URL(e.url).pathname === path);
/* One entry as "how/admitting kind", then what refused the send a record answered, then the refusal. */
const said = (list) => list.map((e) => e.how + '/' + (e.admittedBy ? e.admittedBy.kind : 'none') +
                                       (e.admittedBy && e.admittedBy.refused ? ' ' + e.admittedBy.refused : '') +
                                       (e.refusal ? ' ' + e.refusal : '')).sort().join(' | ');

/* What the judged entries say each header's bytes are, after the row that folds them: the distinct statements,
   since a request asked again in several worlds is judged once per ask. */
const distinct = (words) => [...new Set(words)].sort().join(' | ');
const bytes = (list) => distinct(list.filter((e) => e.vector !== null).map((e) =>
  e.vector.bytes + ' ' + (e.parts ? e.parts.headers.map((h) => h.join(':')).join(',') : 'no-parts')));
/* Every refusal the entries carry, distinct; `none` where no ask was refused. */
const refusals = (list) => distinct(list.filter((e) => e.refusal).map((e) => e.refusal)) || 'none';
const echo = got('POST', '/api/echo');
const ROWS = [
  ['CONTROL: the seed load reached the server', [['GET /', got('GET', '/').length, 1]]],
  ['an unforced POST arrives once with the JSON body and the header list the page composed, and its reply is read',
   [['POST /api/echo', echo.length, 1], ['its body', echo.map((a) => a.body).join('|'), '{"a":1}'],
    ['its content type', echo.map((a) => a.type).join('|'), 'application/json'],
    ['the page\'s own header', echo.map((a) => a.page).join('|'), 'own'],
    ['the request the reply\'s value names', got('GET', '/api/read').map((a) => a.query).join('|'), '?n=1'],
    ['how the zone answered it', said(sends('POST', '/api/echo')), 'sent/default']]],
  ['two POSTs to one address with different bodies are two sends',
   [['POST /api/two', got('POST', '/api/two').length, 2],
    ['their bodies', got('POST', '/api/two').map((a) => a.body).sort().join('|'), 'b=1|b=2'],
    ['how the zone answered them', said(sends('POST', '/api/two')), 'sent/default | sent/default']]],
  ['a POST on a forced path is recorded and not sent',
   [['POST /api/forced', got('POST', '/api/forced').length, 0],
    ['how the zone answered it', said(sends('POST', '/api/forced')), 'not-sent/none provenance=forced']]],
  ['a forced POST identical to one the unforced run sent is answered from that send\'s record',
   [['POST /api/same', got('POST', '/api/same').length, 1],
    ['how the zone answered the two asks', said(sends('POST', '/api/same')),
     'answered-locally/record provenance=forced | sent/default']]],
  ['a fetch() header value that is its unknown\'s example leaves as the example\'s bytes, and the entry says so',
   [['GET /api/hdr-ex', got('GET', '/api/hdr-ex').length, 1],
    ['the header that arrived', got('GET', '/api/hdr-ex').map((a) => a.probe).join('|'), 'en-US'],
    ['how the zone answered it', said(sends('GET', '/api/hdr-ex')), 'sent/default'],
    ['what the entry states', bytes(sends('GET', '/api/hdr-ex')), 'example x-probe:example']]],
  ['a fetch() header value with no example is listed, judged and not sent',
   [['GET /api/hdr-unk', got('GET', '/api/hdr-unk').length, 0],
    ['what refused it', refusals(sends('GET', '/api/hdr-unk')), 'bytes=unknown'],
    ['what the entry states', bytes(sends('GET', '/api/hdr-unk')), 'unknown x-probe:unknown']]],
  ['an XMLHttpRequest header value is judged the same way: the example leaves, the value with no bytes does not',
   [['GET /api/xhr-ex', got('GET', '/api/xhr-ex').length, 1],
    ['the header that arrived', got('GET', '/api/xhr-ex').map((a) => a.probe).join('|'), 'en-US'],
    ['what its entry states', bytes(sends('GET', '/api/xhr-ex')), 'example X-Probe:example'],
    ['GET /api/xhr-unk', got('GET', '/api/xhr-unk').length, 0],
    ['what refused it', refusals(sends('GET', '/api/xhr-unk')), 'bytes=unknown'],
    ['what its entry states', bytes(sends('GET', '/api/xhr-unk')), 'unknown X-Probe:unknown']]],
  ['an unforced XMLHttpRequest POST arrives once with the body and content type the page composed, and its reply is read',
   [['POST /api/xhr-post', got('POST', '/api/xhr-post').length, 1],
    ['its body', got('POST', '/api/xhr-post').map((a) => a.body).join('|'), '{"x":1}'],
    ['its content type', got('POST', '/api/xhr-post').map((a) => a.type).join('|'), 'application/json'],
    ['the request the reply\'s value names', got('GET', '/api/xhr-read').map((a) => a.query).join('|'), '?n=1'],
    ['how the zone answered it', said(sends('POST', '/api/xhr-post')), 'sent/default']]],
  ['an XMLHttpRequest body of bytes arrives as those bytes',
   [['POST /api/xhr-bin', got('POST', '/api/xhr-bin').length, 1],
    ['its bytes in hex', got('POST', '/api/xhr-bin').map((a) => a.hex).join('|'), '00ff0180']]],
  ['an XMLHttpRequest POST on a forced path is recorded and not sent',
   [['POST /api/xhr-forced', got('POST', '/api/xhr-forced').length, 0],
    ['how the zone answered it', said(sends('POST', '/api/xhr-forced')), 'not-sent/none provenance=forced']]],
];
let bad = 0;
for (const [label, notes] of ROWS) {
  const ok = notes.every(([, have, want]) => have === want);
  if (!ok) bad++;
  console.log(`  ${ok ? 'ok  ' : 'BAD '} ${label}\n       ` +
              notes.map(([what, have, want]) => `${what}: ${have} (want ${want})`).join('; '));
}
console.log(`  the zone's process ended: ${ending}; ${egress.length} @EGRESS line(s); ${arrived.length} request(s) arrived`);
/* A zone that did not end cleanly says why on its last stderr lines (an engine abort's `@WHY` among them). */
if (ending !== 'exit 0')
  for (const l of err.split('\n').filter((x) => x !== '' && x.indexOf('@EGRESS') < 0).slice(-4))
    console.log('  the zone said: ' + l.slice(0, 500));
if (got('GET', '/').length < 1) { console.log('  CONTROL FAILED: the seed load never reached the server'); process.exit(2); }
console.log(bad === 0 ? '  every row agreed' : `  ${bad} ROW(S) DISAGREED`);
process.exit(bad === 0 ? 0 : 1);
