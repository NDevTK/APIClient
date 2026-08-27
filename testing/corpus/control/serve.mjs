// INSTRUMENT CONTROL ONLY — not a corpus site. It answers the questions the live corpus cannot, and there
// are TWO of them because the census has two columns that can each read zero for opposite reasons:
//   - does a fetchCallSite the engine certainly sees reach `_engineLog[].endpoints` and
//     `doc._astResults[].fetchCallSites` through THIS probe;
//   - does an attacker source the engine certainly reads reach a code-execution sink and open a search —
//     `_sourceReads`, `_sinkReached`, `_sinkTainted`, `securitySinks`.
// A corpus-wide zero in EITHER column is a finding only once this control's own value in that column is
// non-zero. The @S half did not exist for the whole of the census that published `sinks: 0` on twelve
// sites, so that zero had no control behind it; each document below says what its rungs prove and which
// counter carries each one.
//
// THREE DOCUMENTS, ONE QUESTION EACH, AND BOTH SPLITS ARE MEASURED RATHER THAN TIDY:
//
//   - AN ABORT BLINDS EVERY COLUMN OF THE DOCUMENT IT HAPPENS IN. site.mjs reads the engine's result
//     document, so a run that produced none reports `endpoints: null, sinks: null, sinkReached: null`. A
//     rung whose REGRESSION MODE IS AN ABORT therefore cannot share a document with columns a census reads,
//     or one defect is published as "we measured nothing". url-operands.html's two rungs fail that way; every
//     other rung here fails with a number.
//
//   - AND THE TWO HALVES COMPETE FOR ONE DWELL. security.html's rungs put four to six @S CANDIDATE flows on
//     the frontier and each re-runs the whole page; index.html's orphan rung needs the opposite, because
//     solver/engine.c seeds an orphan drive only from the branch a flow reaches when it has run out of every
//     other kind of work, and takes ONE per call. Measured on one artifact, one dwell, two passes each: the
//     endpoint rungs ALONE report 6 endpoints twice with zero spread at flows=10; the same rungs with the
//     security rungs added report 6 then 5, and `/api/orphan-only` is the one that comes and goes; the
//     security rungs alone report 0 endpoints and sinks 2 / sinkReached 5 / sinkTainted 2. So the split costs
//     the security column nothing and is the whole of the endpoint column's stability.
//
// Three rows, and a census wants all three:
//     node site.mjs control     http://127.0.0.1:8899/                  <pass>
//     node site.mjs control-sec http://127.0.0.1:8899/security.html     <pass>
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
