// ONE LIVE SITE, ONE VIRGIN BROWSER.
//
// Drives the already-built extension in real Chrome against a REAL URL over the internet, then reads the
// RESULT DOCUMENT out of the offscreen brain. It does NOT scrape the console for `@H`: no shipped path
// prints one. Endpoints arrive as `fetchCallSites` inside the engine's ONE `@RESULT` line, which bridge.js
// parses into the analysis object the brain stores at `doc._astResults`; `self._engineLog` carries the
// per-run counters (flows/switches/endpoints/sinks/park) or `{crashed:true}` for a run that produced no
// result document at all. Those two are the instrument.
//
// The console is read for one thing only: the ABORT LINE. renderer-host.js tees the renderer's STDERR to
// this document's console.debug, and a `@WHY` there carries the assert's cond + file:line — the crash
// signature the census ranks by.
//
// usage: node site.mjs <id> <url>      (caller restarts Chrome around each invocation)
import puppeteer from 'puppeteer';
import { writeFileSync, readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { loadavg, cpus } from 'node:os';

const id = process.argv[2], url = process.argv[3];
const DWELL = Number(process.env.DWELL || 40000);
const CDP = Number(process.env.CDP || 9451);
const OUT = new URL('./logs/', import.meta.url);

const b = await puppeteer.connect({ browserURL: `http://127.0.0.1:${CDP}` });

const sink = [];
const hook = (label, e) => { if (e.__w) return; e.__w = 1; e.on('console', m => sink.push(label + ' ' + m.text())); };
const attach = async (t) => {
  if (!t.url().startsWith('chrome-extension://')) return;
  try {
    if (t.type() === 'service_worker') { const w = await t.worker(); if (w) hook('[sw]', w); return; }
    const p = await t.page().catch(() => null);
    if (!p) return;
    hook('[' + t.url().split('/').pop().split('?')[0] + ']', p);
    for (const w of p.workers()) hook('[w]', w);
    p.on('workercreated', w => hook('[w]', w));
  } catch { }
};
for (const t of b.targets()) await attach(t);
b.on('targetcreated', attach);

let off = null;
for (let i = 0; i < 160 && !off; i++) {
  const t = b.targets().find(t => t.url().includes('ast-worker.html'));
  off = t ? await t.page().catch(() => null) : null;
  if (!off) await new Promise(r => setTimeout(r, 250));
}
if (!off) { console.log('ROW ' + JSON.stringify({ id, url, fatal: 'no offscreen document' })); process.exit(0); }
hook('[offscreen]', off);

/* All depths in ONE evaluate so they describe the same instant. `state.docs` is the brain's per-document
   model; `_astResults` is the analysis bridge.js built out of the engine's @RESULT. Nothing here defaults a
   producer's field with `|| x` where the producer is the engine — the arrays below are the ones bridge.js
   asserts field-for-field before it stores them; the `||` guards are for a doc the brain minted but the
   engine has not answered for yet, which is a state this probe must be able to SEE rather than crash on.
   `answered` READS `_astRun`, NOT `_astResults`: the incremental merge writes a snapshot into `_astResults`
   while the engine is still exploring, so that slot no longer answers "has this run returned" — and `run`
   beside it is what the run WAS, so a page whose findings came out of a crashed engine can never be counted
   as a completed analysis of the same page. */
const PROBE = `(() => ({
  runs: (self._engineLog || []).slice(),
  crashes: self._engineCrashOccurred || 0,
  docs: [...state.docs.values()].map(d => ({
    url: d.url || '',
    answered: !!(d._astRun || d._astError),
    run: d._astRun === undefined ? null : d._astRun,
    astError: d._astError ? String(d._astError).slice(0,300) : null,
    sites: (d._astResults || []).flatMap(a => (a.fetchCallSites || []).map(x => (x.method||'?') + ' ' + (x.url||''))),
    sinks: (d._astResults || []).reduce((n,a) => n + ((a.securitySinks||[]).length), 0),
    errs:  (d._astResults || []).flatMap(a => (a.resolverErrors || []).map(e => e.context + ': ' + e.message)),
  })),
  global: [...globalStore.endpoints.keys()],
}))()`;

const pg = await b.newPage();
let nav = 'ok', finalUrl = url, status = 0;
try {
  const r = await pg.goto(url, { waitUntil: 'domcontentloaded', timeout: 45000 });
  status = r ? r.status() : 0;
  finalUrl = pg.url();
  if (!r) nav = 'NO-RESPONSE';
} catch (e) { nav = 'goto:' + String(e && e.message || e).slice(0, 120); }

/* THE DWELL IS WALL-CLOCK, SO THE LOAD IS PART OF THE MEASUREMENT AND THE ROW CARRIES IT.
   A busy machine gives the engine less CPU inside the same 40 seconds, so a row's counters fall without
   anything about the engine changing -- which is how a run at load 92 on four cores was published as a
   regression. That is the elapsed-time defect CLAUDE.md names, and the honest fix here is not to switch
   this probe to a CPU clock (the engine is in another process, and the number wanted is "what does a user
   get in 40 seconds") but to REPORT the conditions beside the number, sampled at both ends of the dwell so
   a build that started mid-row is visible rather than averaged away. */
const loadBefore = loadavg();
await new Promise(r => setTimeout(r, DWELL));
const loadAfter = loadavg();

const cur = await off.evaluate(new Function('return (' + PROBE + ');'))
  .catch(e => ({ probeError: String(e && e.message || e), runs: [], docs: [], global: [] }));

const j = sink.join('\n');
const origin = (() => { try { return new URL(finalUrl).origin; } catch { return ''; } })();
const mine = (cur.docs || []).filter(d => d.url.startsWith(origin));
const runs = cur.runs || [];
const myRuns = runs.filter(r => (r.url || '').startsWith(origin));

/* THE REVISION THE NUMBER BELONGS TO, carried by the row itself. The shared checkout's artifact was rebuilt
   in the middle of the first pass of this census, so two rows that look comparable were measurements of two
   different programs. A row that names its own wasm hash and build head cannot be quoted against another
   build by accident. */
const EXT = process.env.HARNESS_EXT_DIR || '/home/user/APIClient/extension';
let artifact = { extDir: EXT };
try { artifact.wasmSha256 = createHash('sha256').update(readFileSync(EXT + '/lib/qjs/qjs.wasm')).digest('hex'); } catch (e) { artifact.wasmErr = String(e.message); }
/* THE HASH IS THE ONLY FIELD THAT IS TRUE. `head`/`qjsHead` are what the SOURCE TREE was called at build
   time, and the tree is edited continuously by other lanes -- so a build of a dirty tree records a revision
   whose sources do not contain the program that ran. That is not hypothetical: rows have carried a head whose
   quickjs.c lacks the very DFAIL text those same rows printed. They are kept because a wrong name is still a
   hint, and renamed so no reader can mistake them for the revision the number belongs to. */
try {
  const b = JSON.parse(readFileSync(EXT + '/lib/qjs/qjs.mjs.build.json', 'utf8'));
  artifact.builtFromHeadClaim = b.head; artifact.builtFromQjsHeadClaim = b.qjsHead; artifact.builtAt = b.at;
  /* THE STAMP ANSWERS A THREE-STATE QUESTION AND THIS ROW RECORDS ALL THREE. `dirty` alone used to carry both
     "asked git, nothing differs" and "could not ask git at all" -- and the second is the state in which the
     stamp knows NOTHING, so folding it into an empty `dirty` published the STRONGEST claim available (built
     from a clean revision) out of the weakest evidence. `unasked` is the paths whose state could not be read.
     ITS ABSENCE IS AGE, NOT A NEGATIVE ANSWER: an artifact stamped before that field existed has no opinion,
     which is a third thing again and is recorded as such rather than defaulted to `[]`. */
  if (b.dirty !== undefined) artifact.builtFromDirtyTree = b.dirty;
  if (b.unasked !== undefined) artifact.builtFromUnaskedPaths = b.unasked;
  /* A NON-EMPTY `dirty` IS A POSITIVE FACT and is reported as one whatever else the stamp does or does not
     carry -- paths that DIFFER were, necessarily, successfully asked about. Only an EMPTY list is ambiguous,
     and only then does `unasked` decide between "clean" and "nobody could tell". */
  artifact.cleanTreeClaimIs = b.dirty === undefined ? 'no-dirty-field(stamp predates it)'
    : b.dirty.length ? 'dirty(' + b.dirty.length + ' path(s) differ)'
      : b.unasked === undefined ? 'unprovable(stamp predates the unasked field — an empty dirty list here is NOT proof of a clean tree)'
        : b.unasked.length ? 'unprovable(' + b.unasked.length + ' path(s) git could not be asked about)'
          : 'clean(asked, nothing differs)';
} catch (e) { artifact.buildJsonErr = String(e.message); }

const row = {
  id, url, finalUrl, status, nav, artifact, measuredAt: new Date().toISOString(),
  dwellMs: DWELL, cores: cpus().length, loadBefore, loadAfter,
  runsTotal: runs.length,
  runsMine: myRuns.length,
  crashedRuns: runs.filter(r => r.crashed).length,
  crashedMine: myRuns.filter(r => r.crashed).length,
  /* EVERY ENTRY CARRIES THE RUN'S CUMULATIVE TOTAL, so a SUM counts one endpoint once per snapshot and a MAX
     over a log that is never cleared between page loads is non-decreasing by construction. This file reported
     37 for a page that had learned ONE, and reported three repetitions as a rising spread when the instrument
     could not have produced a falling one. `_engineLog` is declared at load and trimmed to 200 entries; it is
     not a per-run buffer. So: read the LAST entry, which is that run's own total, and count DISTINCT addresses
     for the headline. `endpointSnapshots` keeps the old quantity under a name that says what it is, because it
     is still the right thing to watch a live run advance by. */
  endpoints: (() => { const ok = myRuns.filter(r => !r.crashed); return ok.length ? ok[ok.length - 1].endpoints : null; })(),
  endpointSnapshots: myRuns.filter(r => !r.crashed).length,
  sinks: (() => { const ok = myRuns.filter(r => !r.crashed); return ok.length ? ok[ok.length - 1].sinks : null; })(),
  flows: (() => { const ok = myRuns.filter(r => !r.crashed); return ok.length ? ok[ok.length - 1].flows : null; })(),
  switches: (() => { const ok = myRuns.filter(r => !r.crashed); return ok.length ? ok[ok.length - 1].switches : null; })(),
  jobsRun: (() => { const ok = myRuns.filter(r => !r.crashed); return ok.length ? ok[ok.length - 1].jobsRun : null; })(),
  parked: (() => { const ok = myRuns.filter(r => !r.crashed); return ok.length ? ok[ok.length - 1].park : null; })(),
  docsAnswered: mine.filter(d => d.answered).length,
  docsSeenMine: mine.length,
  docsAllOrigins: [...new Set((cur.docs || []).map(d => { try { return new URL(d.url).origin; } catch { return d.url; } }))],
  /* THE HEADLINE NUMBER: distinct addresses learned, which is what "endpoints" has to mean in a report. */
  siteEndpoints: [...new Set(mine.flatMap(d => d.sites))],
  distinctEndpoints: new Set(mine.flatMap(d => d.sites)).size,
  pageErrors: [...new Set(mine.flatMap(d => d.errs))].slice(0, 40),
  globalEndpoints: (cur.global || []).length,
  /* THE ENGINE'S OWN RECORD FIRST, THE CONSOLE ONLY AS A SUPPLEMENT. A console scrape is the wrong surface by
     construction -- the renderer does not tee its stdout -- so a run whose abort reached the result document
     and not the console read `why: []`, and this harness reported a site that ABORTED as one that ran clean
     and learned nothing. Those are opposite findings, and the empty array was a property of the instrument.
     `errs` is what the engine itself recorded; the console is unioned in because a crash before the document
     exists has nowhere else to go. */
  why: [...new Set([...mine.flatMap(d => d.errs || []), ...(j.match(/@WHY[^\n]*/g) || [])]
                   .filter(x => typeof x === 'string' && x.includes('@WHY')))],
  atE: [...new Set(j.match(/@E [^\n]*/g) || [])].slice(0, 10),
  /* THE TWO HALVES OF ONE FACT, READ TOGETHER. A crashed run with no reason, or a reason with no crashed run,
     means one of the two producers is not being read -- which is the defect this row already suffered once.
     `@E` COUNTS AS A REASON. It was left out, so a run killed by a CHECK -- always-fatal in dev AND release,
     the loudest thing the engine can say -- was reported as a crash nobody could explain, while the row's own
     `atE` field held the cond and the file:line. Every abort on this corpus that fires a CHECK rather than a
     DCHECK read that way, which is a flag that raises itself precisely where it is least true. */
  crashWithoutReason: runs.filter(r => r.crashed).length > 0 &&
                      [...mine.flatMap(d => d.errs || []), ...(j.match(/@WHY[^\n]*/g) || []),
                       ...(j.match(/@E [^\n]*/g) || [])].length === 0,
  crashesFlag: cur.crashes,
  consoleLines: sink.length,
  probeError: cur.probeError,
};
try { writeFileSync(new URL(id + '.log', OUT), j); } catch (e) { row.logWriteErr = String(e.message); }
console.log('ROW ' + JSON.stringify(row));
await b.disconnect();
