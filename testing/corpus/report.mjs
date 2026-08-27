// THE CENSUS TABLE. One row per site: outcome, the abort's file:line, endpoints learned, flows/switches,
// whether the analysis finished. Then the distinct crash signatures ranked by how many sites hit each.
//
// THE OUTCOMES ARE NOT MERGED (an earlier pass conflated them and it cost real time):
//   ENGINE-ABORT   an @WHY/@E from the engine or the trusted zone — a DCHECK/CHECK naming an unbuilt
//                  capability or a violated invariant. The renderer dies; the run has no result document.
//   ABORT(unnamed) the run crashed and this file could not name the assert — the ranked queue below is SHORT
//                  by that entry, so it is its own outcome and never folded into ENGINE-ABORT. A site that
//                  is both across passes reads ABORT(part unnamed). See the shouted section under the queue.
//   PAGE-THROW     the page's own uncaught throw on a capability this engine honestly lacks. It arrives as
//                  a `pageErrors` row that is NOT an engine assert, or as a @LOG the page itself emitted.
//   FIXTURE/NET    the site did not deliver a document (HTTP 5xx, proxy refusal, navigation failure). Not a
//                  statement about the engine at all.
//   RAN            no abort: the engine ran flows. `finished` then says whether the analysis RETURNED
//                  within the dwell (a result document stored on the doc) or was still exploring.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = new URL('.', import.meta.url).pathname;
/* SEVERAL PASSES, BECAUSE ONE RUN IS NOT A MEASUREMENT. Pass every census file and the table reports each
   site's SPREAD across them; pass one and the spread is a single value, which is honest about what it is.
   The pass label is taken from the filename (census-p1.jsonl -> p1) purely to name the log a signature came
   from -- the signature itself is read from the ROW, which carries its own `why`/`atE`, so an aggregate can
   never attribute one pass's abort to another pass's log. */
const files = process.argv.slice(2);
if (!files.length) files.push('census.jsonl');
const passes = files.map((f) => ({
  label: (f.match(/census-(.+)\.jsonl$/) || [, f])[1],
  rows: readFileSync(join(ROOT, f), 'utf8').trim().split('\n').filter(Boolean).map((l) => JSON.parse(l)),
}));
const stacks = new Map(readFileSync(join(ROOT, 'sites.tsv'), 'utf8').trim().split('\n')
  .map((l) => l.split('\t')).map(([id, u, s]) => [id, s]));

const unesc = (s) => s.replace(/\\\\/g, '\\').replace(/\\"/g, '"').replace(/\\n/g, '\n');

/* A SIGNATURE NAMES A DEFECT, SO A GENSYM IN IT MUST NOT SPLIT ONE INTO SEVERAL. The orphan-drive DFAIL
   quotes the operand's identity -- `{orphan3a153db70eec69f2.arg0}` on one site and `{orphanc59c489f051691b8
   .arg0}` on another -- which is the SAME unbuilt capability at the same file:line, and it ranked as two
   one-site entries instead of one two-site entry. Ranking is what this section is for, so the per-run name is
   folded out: a run of 8+ hex digits is an address or a hash, never part of what to build. */
const degensym = (s) => s.replace(/[0-9a-f]{12,}/gi, '<id>').replace(/\b0x[0-9a-f]{4,}\b/gi, '<addr>');

/* A SIGNATURE'S SOURCE LOCATION IS REPO-RELATIVE, BECAUSE THE PREFIX IS A FACT ABOUT THE BUILD MACHINE AND
   THE DEFECT IS NOT. The location arrives as whatever `__FILE__` baked into the object: `engine/build.mjs`
   passes `-ffile-prefix-map=<root>/=` so a current build already emits `engine/host/.../x.c`, but an artifact
   built before that flag -- or by any builder that does not pass it -- emits the ABSOLUTE path of the
   directory the build ran in, and §Testing REQUIRES that directory to be a frozen snapshot worktree that is
   deleted afterwards. Keying on it is therefore the gensym-splitting defect degensym exists to prevent, one
   level up: the same DFAIL measured from two builds ranks as two one-site entries, and the ranking is the
   whole point of this section. So the key is rooted at the last `engine/` or `extension/` path SEGMENT.
   AN ABSOLUTE PATH THAT CANNOT BE ROOTED IS FATAL, never kept as a key. A key carrying a build directory
   silently splits a defect and nothing in the output can contradict it -- the same reason harness.js refuses
   to launch on an ABI table it cannot parse rather than reporting agreement it never established. A path that
   is ALREADY relative needs no root marker: it is machine-independent as it stands, which is the property
   this function is protecting, so it is returned untouched. */
function srcref(raw) {
  const p = raw.trim();
  const m = [...p.matchAll(/(?:^|\/)(engine\/|extension\/)/g)].pop();
  if (m) return p.slice(m.index + (m[0].length - m[1].length));
  if (!p.startsWith('/')) return p;
  throw new Error(`report.mjs: an assert emitted the source location \`${p}\`, which is an ABSOLUTE path with\n` +
    `  no \`engine/\` or \`extension/\` segment to root it at. That location cannot be a ranking key: it names\n` +
    `  the directory the build ran in, so the same defect from the next build would rank as a second entry.\n` +
    `  Teach srcref() the source root this producer compiles from rather than letting the path through.`);
}

/* THE THREE FILES THAT EMIT AN ASSERTION, AND EVERY TAG EACH OF THEM WRITES. This list used to have three
   patterns too, and they covered ONE of the six shapes plus half of another -- so a whole half of the engine
   was invisible to the ranked queue while every row still printed as a classified ENGINE-ABORT. Measured on
   a live pass: astexplorer.net aborted at `engine/qjs/quickjs.c` DCHECK "the arg-list length is coerced once"
   and produced ZERO signatures, because the pattern for that shape required the location to begin with one
   particular checkout path -- a literal that the current build.mjs can no longer produce for anybody, so the
   pattern matched nothing for every legal artifact. That is the read-with-no-writer defect with the arrow
   reversed: four producers writing, nothing reading, and a default (`sigs.length ? ... : ...`) turning the
   silence into a plausible row instead of a crash.
     engine/host/check.h        `@WHY`/`@E {"phase":"assert","cond":…,"at":"file:line","reason":…}` (JSON)
     engine/qjs/quickjs-check.h `@WHY`/`@E <msg> (file:line)`  -- the submodule cannot include the host header,
                                so its EMIT is a plain line; this is the interpreter, the trampoline, every
                                step machine and libregexp, i.e. where forced execution actually runs.
     extension/check.js         `@WHY DCHECK failed:`/`@WHY DFAIL:`/`@E CHECK failed:`/`@E CHECK_FAIL:` -- a
                                thrown Error, so it carries a message and NO file:line; the file is the key.
   Each emitter gets ONE pattern covering BOTH its tags, so a CHECK (always fatal, dev AND release) can never
   be the shape nobody reads while its DCHECK sibling is read. */
const JS_MIRROR_TAGS = 'DCHECK failed|DFAIL|CHECK failed|CHECK_FAIL';
function signatures(text) {
  const t = unesc(unesc(text));
  const out = new Set();
  for (const m of t.matchAll(/"cond":"((?:[^"\\]|\\.)*)","at":"([^"]+)"/g))
    out.add(srcref(m[2]) + ' :: ' + degensym(m[1]));
  for (const m of t.matchAll(new RegExp(
        '@(?:WHY|E) (?!\\{)(?!(?:' + JS_MIRROR_TAGS + '):)([^\\n]*?) \\(([^()\\s]+:\\d+)\\)', 'g')))
    out.add(srcref(m[2]) + ' :: ' + degensym(m[1].trim()));
  for (const m of t.matchAll(new RegExp('@(?:WHY|E) (' + JS_MIRROR_TAGS + '): ([^\\n"]{4,200})', 'g')))
    out.add('extension/check.js (js side) :: ' + m[1] + ' ' + degensym(m[2].trim()).slice(0, 120));
  return [...out];
}

/* ONE MEASUREMENT PER (site, pass). The signature comes from the ROW; the log is a supplement for a crash
   that happened before any result document existed and so had nowhere else to go. THE ROW NAMES ITS OWN
   TRANSCRIPT (`logFile`, written by site.mjs) and this file no longer GUESSES between two spellings: it tried
   `<label>-<id>.log` and fell back to `<id>.log`, and nothing in the tree ever wrote the first name, so the
   fallback was the only arm that ever ran -- and `<id>.log` is ONE path per site, overwritten by every later
   pass. Every row of every pass was therefore read against the LAST pass's console, which cross-attributes a
   crash: a site that RAN in pass 1 and aborted in pass 3 is published as an abort in both. A row without
   `logFile` is a census produced by an older site.mjs and is REFUSED rather than half-read, because the log
   it would be paired with is the wrong pass's by construction. */
const bySig = new Map();
const seen = new Map();      // id -> [per-pass measurement]
/* AN ABORT THIS FILE CANNOT NAME IS THE ONE THING THE QUEUE MUST SHOUT ABOUT, because it is the state in
   which the queue is INCOMPLETE and every other column still reads as a finished measurement. Collected per
   (site, pass) with the first assertion-shaped line found beside it, so a producer the patterns above do not
   speak announces itself with the text to teach them, instead of subtracting one entry from the ranking. */
const unnamed = [];
for (const p of passes) for (const r of p.rows) {
  if (!r.fatal && typeof r.logFile !== 'string')
    throw new Error(`report.mjs: row \`${r.id}\` in ${p.label} carries no \`logFile\`. site.mjs writes that\n` +
      `  field with the transcript it wrote; a row without one comes from a build of site.mjs that named the\n` +
      `  log after the SITE alone, so its transcript has already been overwritten by a later pass and pairing\n` +
      `  the two would attribute one pass's abort to another's counters. Re-run the census.`);
  let log = '';
  if (r.logFile) { try { log = readFileSync(join(ROOT, 'logs', r.logFile), 'utf8'); } catch { } }
  const blob = log + '\n' + JSON.stringify(r.pageErrors || []) + '\n' + JSON.stringify(r.why || []) + '\n' + JSON.stringify(r.atE || []);
  const sigs = signatures(blob);
  const netBad = r.fatal || r.nav !== 'ok' || (r.status !== 200 && r.status !== 304);
  const outcome = netBad ? 'NET/FIXTURE'
    : sigs.length ? 'ENGINE-ABORT'
      : r.crashedMine ? 'ABORT(unclassified)'
        : 'RAN';
  if (outcome === 'ABORT(unclassified)') {
    const line = (blob.match(/@(?:WHY|E)[^\n]{0,300}/) || [])[0];
    unnamed.push({ id: r.id, pass: p.label, line: line || '(no @WHY or @E line anywhere in this row or its log)' });
  }
  if (!seen.has(r.id)) seen.set(r.id, []);
  seen.get(r.id).push({
    pass: p.label, outcome, finished: r.docsAnswered > 0 && !r.crashedMine,
    runs: r.runsMine, crashed: r.crashedMine,
    /* THE HEADLINE IS `distinctEndpoints` -- distinct ADDRESSES learned, which is what "endpoints" has to
       mean in a report. `r.endpoints` is the last log entry's CUMULATIVE counter and is kept beside it under
       a name that says so, because it is still the right thing to watch a live run advance by. This column
       read the counter, so a page that learned one address could print a two-digit `ep`. */
    endpoints: r.distinctEndpoints, counter: r.endpoints,
    sinks: r.sinks, flows: r.flows, switches: r.switches,
    sigs, wasm: (r.artifact && r.artifact.wasmSha256 || '').slice(0, 12),
    /* THE ARTIFACT IS NAMED BY ITS HASH ALONE. This read `r.artifact.head`, a field site.mjs deliberately
       renamed to `builtFromHeadClaim` when it stopped being trustworthy, so it resolved to '' for every row
       and the artifact line printed a bare `@` -- a reader-with-no-writer, defaulted to empty by `|| ''`. */
    load: Array.isArray(r.loadAfter) ? r.loadAfter[0] : null,
  });
  for (const s of sigs) { if (!bySig.has(s)) bySig.set(s, new Set()); bySig.get(s).add(r.id); }
}

/* A SPREAD, NOT AN AVERAGE. min-max over the passes that ANSWERED; a pass that produced no number is counted
   in `n/N` and never folded into the range, because absent and zero are different facts. */
const spread = (ms, k) => {
  const v = ms.map((m) => m[k]).filter((x) => typeof x === 'number');
  if (!v.length) return '-';
  const lo = Math.min(...v), hi = Math.max(...v);
  return lo === hi ? String(lo) : lo + '-' + hi;
};
const table = [...seen.entries()].map(([id, ms]) => ({
  id, stack: stacks.get(id) || '', measurements: ms,
  /* THE OUTCOME IS THE WORST SEEN, and how often, so a site that aborts in two passes of three cannot be
     reported as one that runs. AND AN ABORT NOBODY COULD NAME KEEPS ITS OWN WORD HERE. `.includes('ABORT')`
     folded `ABORT(unclassified)` into `ENGINE-ABORT`, so the row that proves the ranked queue is missing an
     entry printed as the row that proves it is complete -- the three-states-behind-one-answer defect, in the
     column a reader trusts most. Named and unnamed are different findings and one site can be both. */
  outcome: ms.some((m) => m.outcome === 'NET/FIXTURE') ? 'NET/FIXTURE'
    : ms.some((m) => m.outcome === 'ENGINE-ABORT')
      ? (ms.some((m) => m.outcome === 'ABORT(unclassified)') ? 'ABORT(part unnamed)' : 'ENGINE-ABORT')
      : ms.some((m) => m.outcome === 'ABORT(unclassified)') ? 'ABORT(unnamed)' : 'RAN',
  abortedPasses: ms.filter((m) => m.outcome.includes('ABORT')).length,
  finishedPasses: ms.filter((m) => m.finished).length,
  n: ms.length,
  ep: spread(ms, 'endpoints'), fl: spread(ms, 'flows'), sw: spread(ms, 'switches'),
  sk: spread(ms, 'sinks'), rn: spread(ms, 'runs'), ld: spread(ms, 'load'),
  epMax: Math.max(-1, ...ms.map((m) => m.endpoints).filter((x) => typeof x === 'number')),
  epAnswered: ms.filter((m) => typeof m.endpoints === 'number').length,
  sigs: [...new Set(ms.flatMap((m) => m.sigs))],
  wasm: [...new Set(ms.map((m) => m.wasm))],
}));

/* ABSENT PRINTS AS `-`, NEVER AS 0. A run that produced no result document is not a page that was analysed
   and found clean, and `String(null)` would have printed "null" into a numeric column. */
const pad = (s, n) => String(s).padEnd(n).slice(0, n);
console.log(`${passes.length} pass(es): ${passes.map((p) => p.label + '(' + p.rows.length + ')').join(' ')}`);
console.log('\n' + pad('site', 20) + pad('outcome', 20) + pad('abort/n', 8) + pad('fin/n', 7) +
  pad('ep', 8) + pad('sinks', 7) + pad('flows', 12) + pad('switches', 12) + pad('load', 10) + 'signature');
for (const t of table)
  console.log(pad(t.id, 20) + pad(t.outcome, 20) + pad(t.abortedPasses + '/' + t.n, 8) +
    pad(t.finishedPasses + '/' + t.n, 7) + pad(t.ep, 8) + pad(t.sk, 7) + pad(t.fl, 12) + pad(t.sw, 12) +
    pad(t.ld, 10) + (t.sigs[0] ? t.sigs[0].split(' :: ')[0] : '-'));

/* THE WORK QUEUE. A DFAIL's reason names what to build, so it is printed rather than summarised -- but only
   the head of it, because one 1169-character reason per row buries the RANKING, which is the thing this
   section exists to show. `-v` prints them whole. */
const verbose = process.env.SIGS === 'full';
console.log('\n=== distinct crash signatures, ranked by sites hit ===');
for (const [s, ids] of [...bySig.entries()].sort((a, b) => b[1].size - a[1].size)) {
  const [at, ...rest] = s.split(' :: ');
  const why = rest.join(' :: ');
  console.log(`${ids.size}  ${at}\n     ${verbose || why.length <= 300 ? why : why.slice(0, 300) + ' …(SIGS=full for the rest)'}\n     sites: ${[...ids].join(', ')}`);
}
/* AND WHAT THE QUEUE ABOVE IS MISSING, PRINTED WHERE THE QUEUE IS READ. A ranking is only a work queue if a
   defect that cannot be named is louder than the ones that can -- otherwise the queue's own gaps are its
   quietest entries. The line is quoted verbatim so the fix is to teach signatures() that emitter's shape,
   which is the one repair a reader could not derive from a count. */
if (unnamed.length) {
  console.log('\n*** ' + unnamed.length + ' ABORT(S) THIS FILE COULD NOT NAME — the ranking above is SHORT by ' +
              'that many entries, and no count in this report can say so ***');
  for (const u of unnamed) console.log(`  ${u.id} [${u.pass}]  ${u.line}`);
}

/* ONE ARTIFACT PER CENSUS OR THE CENSUS IS NOT ONE MEASUREMENT. More than one hash here means rows either
   side of a rebuild are two programs wearing one corpus, and the totals below are meaningless. */
const artifacts = new Set(table.flatMap((t) => t.wasm).filter(Boolean));
console.log('\nartifacts measured: ' + [...artifacts].join('  ') + (artifacts.size > 1 ? '   *** MIXED — NOT ONE MEASUREMENT ***' : ''));
const loads = passes.flatMap((p) => p.rows).map((r) => Array.isArray(r.loadAfter) ? r.loadAfter[0] : null).filter((x) => typeof x === 'number');
if (loads.length) console.log(`load average across rows: min ${Math.min(...loads).toFixed(2)} max ${Math.max(...loads).toFixed(2)} mean ${(loads.reduce((a, b) => a + b, 0) / loads.length).toFixed(2)} on ${passes.flatMap((p) => p.rows)[0].cores} cores`);

/* ABSENT AND ZERO ARE DIFFERENT FACTS AND ARE NEVER SUMMED TOGETHER. `reduce((n,t)=>n+t.endpoints,0)` made
   `null` into 0 (JS coerces it) and `undefined` into NaN, which JSON.stringify then printed as `null` -- so
   the total was either a number that had silently counted un-analysed sites as clean-and-empty, or the word
   "null" for the whole corpus. Report the count of sites that ANSWERED beside the sum over those sites.
   The per-site figure is that site's BEST pass, because the question a headline answers is what the tool
   CAN learn from the page; the spread column beside it is what says how reliably. */
const measurable = table.filter((t) => t.outcome !== 'NET/FIXTURE');
const withEp = table.filter((t) => t.epAnswered > 0);
console.log('totals: ' + JSON.stringify({
  sites: table.length,
  netFixture: table.filter((t) => t.outcome === 'NET/FIXTURE').length,
  measurable: measurable.length,
  everAborted: measurable.filter((t) => t.abortedPasses > 0).length,
  abortedEveryPass: measurable.filter((t) => t.abortedPasses === t.n).length,
  cleanEveryPass: measurable.filter((t) => t.abortedPasses === 0).length,
  finishedEveryPass: measurable.filter((t) => t.finishedPasses === t.n).length,
  sitesReportingAnEndpointCount: withEp.length,
  sitesLearningAtLeastOneEndpoint: withEp.filter((t) => t.epMax > 0).length,
  endpointsTotalBestPass: withEp.reduce((n, t) => n + Math.max(0, t.epMax), 0),
}));
