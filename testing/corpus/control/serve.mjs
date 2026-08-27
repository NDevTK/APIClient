// INSTRUMENT CONTROL ONLY — not a corpus site. It answers the questions the live corpus cannot, and there
// are TWO of them because the census has two columns that can each read zero for opposite reasons:
//   - does a fetchCallSite the engine certainly sees reach `_engineLog[].endpoints` and
//     `doc._astResults[].fetchCallSites` through THIS probe;
//   - does an attacker source the engine certainly reads reach a code-execution sink and open a search —
//     `_sourceReads`, `_sinkReached`, `_sinkTainted`, `securitySinks`.
// A corpus-wide zero in EITHER column is a finding only once this control's own value in that column is
// non-zero. The @S half did not exist for the whole of the census that published `sinks: 0` on twelve
// sites, so that zero had no control behind it; index.html says what each rung proves and which counter
// carries it.
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
