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
// FOUR DOCUMENTS, ONE QUESTION EACH, AND EVERY SPLIT HERE IS MEASURED RATHER THAN TIDY:
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
//   - AND A ROW IS ISOLATED BY ORIGIN, NOT BY PATH, WHICH IS WHY THIS BINDS THREE PORTS AND NOT ONE. That was
//     the half the document split did not fix and it was measured the same way: `site.mjs` selects the runs
//     and documents that are the row's by `d.url.startsWith(origin)`, so three documents on ONE origin make
//     every row the UNION of whatever the engine reached on that origin during the dwell. All three rows came
//     back with an IDENTICAL six-endpoint list and `docsSeenMine 5`, and only the one whose own rungs produce
//     those six was measuring itself; the other two were reporting their neighbour's endpoints as their own.
//     A split into paths looks like isolation and is not, because nothing downstream is keyed by path.
//     Three ports, three origins, three rows — and the base port is a PARAMETER so a lane can take a private
//     one, which the shared 8899 already needed: two lanes ran the control at once and one lost its pass to
//     EADDRINUSE.
//
//   - AND A DATA BLOCK PUTS A FLOW ON THE FRONTIER FOR THE SAME REASON THE SECURITY RUNGS DO. 25.5.1 over a
//     text this engine does not have forks its two completions, so injected-state.html's rung competes with
//     index.html's orphan drive exactly as security.html's candidates do — the split is the same measurement
//     and not a second convention.
//
//
//   - AND A LOADED CONFIG IS THE SAME CHANNEL OVER THE NETWORK, WHICH IS WHY IT IS A FIFTH ORIGIN AND NOT A
//     SECOND RUNG ON THE FOURTH. loaded-config.html's record arrives in a REPLY, so the flows it forks are
//     seeded when that reply is provided rather than when the document parses — a different competition for
//     the one dwell, and a row that shared an origin with the data block would report the two channels'
//     endpoints as one union and could not say which channel produced which.
//
//
//   - AND THE SAME REPLY THROUGH THE OTHER INTERFACE IS A SIXTH ORIGIN, FOR THE SAME REASON AGAIN.
//     xhr-config.html reads the identical `/cfg.json` through XMLHttpRequest §3.6.9 The response getter and
//     §3.6.10 The responseText getter. Sharing loaded-config.html's origin would make the two interfaces one
//     union, which is precisely what this pair exists to tell apart: `fetch`'s Response and XMLHttpRequest are
//     two doors onto one fact, and a run in which one door's gates fork and the other's do not is one
//     capability wearing two names — a difference that reads as ordinary variation in a single row.
//
// Six rows, and a census wants all six (PORT sets the base; the others follow it):
//     node site.mjs control      http://127.0.0.1:8899/ <pass>
//     node site.mjs control-sec  http://127.0.0.1:8900/ <pass>
//     node site.mjs control-url  http://127.0.0.1:8901/ <pass>
//     node site.mjs control-data http://127.0.0.1:8902/ <pass>
//     node site.mjs control-cfg  http://127.0.0.1:8903/ <pass>
//     node site.mjs control-xhr  http://127.0.0.1:8904/ <pass>
import { createServer } from 'node:http';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const D = dirname(fileURLToPath(import.meta.url));
const BASE = Number(process.env.PORT || 8899);

/* ONE DOCUMENT PER ORIGIN, and `/` is the only path that serves it. A document reachable at a SECOND origin's
   root would put it back in that row's union, which is the whole defect this replaces — so each server
   answers its own document and 404s every other one. `/ext.js` and `/api/*` are per-origin too: a
   subresource fetched cross-origin is a different request with a different principal, and the endpoint half
   is measuring what the engine learns from a SAME-origin subresource. A document names its OWN subresource
   (`/ext.js`, `/state.js`) and every origin answers for it out of this directory, so which bundle a row loads
   is stated by that row's document rather than by a name every origin has to share. */
const DOCS = [
  ['index.html', 'control'],
  ['security.html', 'control-sec'],
  ['url-operands.html', 'control-url'],
  ['injected-state.html', 'control-data'],
  ['loaded-config.html', 'control-cfg'],
  ['xhr-config.html', 'control-xhr'],
];

/* THE ONE NON-SCRIPT SUBRESOURCE ANY ROW FETCHES, and it is answered by every origin for the same reason the
   `.js` bundles are: which document asks for it is stated by that document. It is NOT under `/api/`, because
   an `/api/` path is what this control's endpoint rungs are COUNTING and a config the bundle loads would then
   be indistinguishable from an endpoint the bundle learned. WHICH MEMBERS IT HOLDS IS THE RUNG: two the bundle
   gates on are present and `false`, and one it gates on is absent — see loaded-config.html, which names what
   each one is for and why answering either of them concretely loses an endpoint. */
const CONFIG = '{"region":"us-east-1","tier":"gold","admin":false,"nested":{"beta":false}}';

let bound = 0;
DOCS.forEach(([doc, name], i) => {
  const port = BASE + i;
  createServer((req, res) => {
    const p = new URL(req.url, 'http://x').pathname;
    if (p.startsWith('/api/')) {
      res.writeHead(200, { 'content-type': 'application/json' });
      return res.end('{"ok":true}');
    }
    if (p === '/cfg.json') {
      res.writeHead(200, { 'content-type': 'application/json' });
      return res.end(CONFIG);
    }
    const f = p === '/' ? join(D, doc)
            : /^\/[a-z0-9_-]+\.js$/.test(p) ? join(D, p.slice(1))
            : null;
    if (!f || !existsSync(f)) { res.writeHead(404); return res.end(''); }
    res.writeHead(200, { 'content-type': f.endsWith('.js') ? 'application/javascript' : 'text/html; charset=utf-8' });
    res.end(readFileSync(f));
  }).listen(port, '127.0.0.1', () => {
    /* PRINTED PER ORIGIN AND ONLY AFTER A SUCCESSFUL LISTEN, because run.sh's own rule is that this line is
       the proof the server is THIS run's — and with three of them, a driver that checked only the first
       would drive two rows against ports it never confirmed. The last line is the one to wait for. */
    console.log(`${name} on ${port}  (${doc})`);
    if (++bound === DOCS.length) console.log(`control ready on ${BASE}-${BASE + DOCS.length - 1}`);
  });
});
