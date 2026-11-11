// The page's own non-GET through the whole native host: engine/trusted.mjs over the native `--abi` binary loads
// a page from a loopback server that records every request, so each row reads what reached the server beside the
// zone's `@EGRESS` lines. The page's unforced boot POSTs JSON and reads the reply; it POSTs two bodies to one
// address; and on an arm that assumes absent state it POSTs to a new address and repeats a POST its unforced run
// already made. The control is the seed load: the server served the page, or nothing below is a reading.
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
  }
});
</script>`;

/* Every request that arrived, as "METHOD path?query <body> <content-type> <x-page>". */
const arrived = [];
const server = createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  const u = new URL(req.url, 'http://x');
  arrived.push({ method: req.method, path: u.pathname, query: u.search, body: Buffer.concat(chunks).toString('utf8'),
                 type: req.headers['content-type'] || '', page: req.headers['x-page'] || '' });
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
