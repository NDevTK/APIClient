// INSTRUMENT CONTROL ONLY — not a corpus site. It answers one question the live corpus cannot: does a
// fetchCallSite the engine certainly sees reach `_engineLog[].endpoints` and `doc._astResults[].fetchCallSites`
// through THIS probe. A corpus-wide zero is a finding only once this control is non-zero.
import { createServer } from 'node:http';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const D = dirname(fileURLToPath(import.meta.url));
createServer((req, res) => {
  const p = new URL(req.url, 'http://x').pathname;
  if (p.startsWith('/api/')) { res.writeHead(200, { 'content-type': 'application/json' }); return res.end('{"ok":true}'); }
  const f = p === '/' ? join(D, 'index.html') : join(D, p.replace(/^\//, ''));
  if (!existsSync(f)) { res.writeHead(404); return res.end('no'); }
  res.writeHead(200, { 'content-type': f.endsWith('.js') ? 'application/javascript' : 'text/html; charset=utf-8' });
  res.end(readFileSync(f));
}).listen(8899, '127.0.0.1');
console.log('control on 8899');
