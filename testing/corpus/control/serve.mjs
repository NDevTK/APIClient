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
//
// TWO DOCUMENTS, AND THE SPLIT IS NOT TIDINESS. An abort blinds every column of the document it happens in —
// site.mjs reads the engine's result document, and a run that produced none reports `endpoints: null,
// sinks: null, sinkReached: null` — so a rung whose REGRESSION MODE IS AN ABORT must not share a document
// with the columns a census reads, or one defect is published as "we measured nothing". index.html's rungs
// answer with a number when they fail; url-operands.html's answer by taking the renderer down. Each is one
// row:
//     node site.mjs control     http://127.0.0.1:8899/                  <pass>
//     node site.mjs control-url http://127.0.0.1:8899/url-operands.html <pass>
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
