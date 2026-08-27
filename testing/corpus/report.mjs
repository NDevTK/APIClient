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
import { siteList } from './list.mjs';

const ROOT = new URL('.', import.meta.url).pathname;
/* SEVERAL PASSES, BECAUSE ONE RUN IS NOT A MEASUREMENT. Pass every census file and the table reports each
   site's SPREAD across them; pass one and the spread is a single value, which is honest about what it is.
   The pass label is taken from the filename (census-p1.jsonl -> p1) purely to name the log a signature came
   from -- the signature itself is read from the ROW, which carries its own `why`/`atE`, so an aggregate can
   never attribute one pass's abort to another pass's log. */
const files = process.argv.slice(2);
if (!files.length) files.push('census.jsonl');
const passes = files.map((f) => {
  const rows = readFileSync(join(ROOT, f), 'utf8').trim().split('\n').filter(Boolean).map((l) => JSON.parse(l));
  /* WHETHER THIS PASS'S INSTRUMENT COULD ANSWER THE @S ARRIVAL QUESTION AT ALL — which is a fact about the
     site.mjs that wrote it, not about the pages it measured, and the two are indistinguishable in the table.
     A pass whose rows carry no `sourceReads` FIELD predates the counter; a pass whose rows carry it as `null`
     is one whose runs produced no counters. Both print `-`, and a comment in this file used to be the only
     place that said so — which is asking a reader to hold a fact the output cannot show them. Measured on
     this project's own baseline: of four passes quoted as one four-pass measurement, exactly ONE carried
     these fields, and its `70>0>0>0` — the observation the whole security half was argued from — is n=1. */
  const measured = rows.filter((r) => !r.fatal);
  return { label: (f.match(/census-(.+)\.jsonl$/) || [, f])[1], rows,
           /* THREE STATES, NOT TWO: this pass carried the field, this pass predates it, or this pass measured
              nothing to ask (every row fatal). A pass with no measured rows is not evidence either way and
              must not be reported as an old instrument. */
           arrival: measured.length === 0 ? 'nothing-measured'
             : measured.some((r) => 'sourceReads' in r) ? 'carried' : 'predates' };
});
/* THE LIST THIS CENSUS MEASURED, NAMED AND THEN CHECKED AGAINST THE ROWS. This file used to read `sites.tsv`
   unconditionally and look every row's id up in it — and the app-page census walks twelve ids that appear in
   NO line of that file, so all twelve missed and `|| ''` turned every miss into a blank column. A report that
   was handed the wrong list printed as a complete table, which is the defaulted-field defect performed in the
   one artifact a reader trusts to say what was measured. The list is now a parameter (`SITES`, the spelling
   run.sh already used) and a row whose id it does not contain is FATAL: that row's every other column is a
   claim about a corpus nobody selected. */
const list = siteList();
const stacks = new Map([...list.byId].map(([id, r]) => [id, r.stack]));

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
   be the shape nobody reads while its DCHECK sibling is read.

   THE FIELD THAT NAMES WHAT TO BUILD IS `reason`, AND THIS FILE READ `cond` INSTEAD. check.h writes FOUR
   fields and this parser took two of them, so every check.h assert in the ranked queue below printed its
   `cond` -- which for `DFAIL` is the fixed string check.h's own `#define` interpolates ("unreachable", the
   macro's way of saying there was no boolean to test) and therefore says NOTHING WHATEVER about the site. The
   queue's entire purpose is to name the next capability to build; five of ten entries named it `unreachable`
   while the producer had written a spec-cited paragraph into `reason` on the same line. That is the
   defaulted-field defect performed by a MEASUREMENT: nothing was absent, nothing crashed, and the constant
   read exactly like a finding. It cost a lane, dispatched to write three DFAIL messages that already existed
   in full with their section numbers and titles.
   SO THE PAYLOAD IS `reason`, AND A RECORD THIS CANNOT DECOMPOSE IS FATAL rather than degraded to the part
   that did parse -- the same rule `srcref` above enforces for a location. `cond` is printed BESIDE the reason
   only where it carries the failing C EXPRESSION (a `DCHECK`), because that expression is the operand that
   reached the gap; where it is the unconditional constant it is the macro's spelling and not an observation,
   and printing it is what made the queue unreadable. */
const CHECK_H_UNCONDITIONAL = 'unreachable';   // check.h's `DFAIL`/`CHECK_FAIL` condstr; see engine/host/check.h
const CHECK_H_RECORD = /"phase":"assert","cond":"([\s\S]*?)","at":"([^"]*)","reason":"([\s\S]*?)"\}(?=$|[\s,"\]])/;
const JS_MIRROR_TAGS = 'DCHECK failed|DFAIL|CHECK failed|CHECK_FAIL';
function signatures(text) {
  const t = unesc(unesc(text));
  const out = new Set();
  /* Anchored on each record's OWN opening rather than swept with a global regex, so a record that fails to
     decompose is a named throw at a known offset instead of one fewer entry in a ranking that still prints as
     complete. */
  for (let i = t.indexOf('"phase":"assert"'); i !== -1; i = t.indexOf('"phase":"assert"', i + 1)) {
    const m = CHECK_H_RECORD.exec(t.slice(i));
    if (!m || m.index !== 0 || !m[3] || m[3].includes('"phase":"assert"'))
      throw new Error(`report.mjs: a check.h assertion record could not be decomposed into cond/at/reason:\n` +
        `  ${t.slice(i, i + 300)}\n` +
        `  check.h's APICLIENT_ASSERT_EMIT writes all four fields on one line, so this is that emitter and\n` +
        `  this parser having come apart -- teach this pattern the shape rather than letting the record\n` +
        `  through, because the ONE field it carries that a work queue can act on is \`reason\`.`);
    out.add(srcref(m[2]) + ' :: ' +
            (m[1] === CHECK_H_UNCONDITIONAL ? '' : degensym(m[1]) + ' -- ') + degensym(m[3]));
  }
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
  if (!stacks.has(r.id))
    throw new Error(`report.mjs: census row \`${r.id}\` (pass ${p.label}) is not in \`${list.rel}\`, the site\n` +
      `  list this report was handed. A census is a measurement OF a list, so a row the list does not name\n` +
      `  means the two were paired by accident and every column beside it describes a corpus nobody selected.\n` +
      `  Pass the list the census walked:  SITES=<list>.tsv node report.mjs <census files>`);
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
    /* THE @S ARRIVAL CENSUS, WHICH IS WHAT MAKES `sinks: 0` A FINDING RATHER THAN A SHRUG. Read in the order
       a search travels -- a source is read, a sink is reached, taint arrives at one, the search is declined
       as unforgeable -- so the column says WHERE the zero starts, and a corpus-wide `sinks: 0` stops being
       one number with three opposite meanings. site.mjs carries these off the run record bridge.js writes. */
    src: r.sourceReads, reach: r.sinkReached, taint: r.sinkTainted, sup: r.sinkSuppressed,
    sigs, wasm: (r.artifact && r.artifact.wasmSha256 || '').slice(0, 12),
    /* THE ARTIFACT IS NAMED BY ITS HASH ALONE. This read `r.artifact.head`, a field site.mjs deliberately
       renamed to `builtFromHeadClaim` when it stopped being trustworthy, so it resolved to '' for every row
       and the artifact line printed a bare `@` -- a reader-with-no-writer, defaulted to empty by `|| ''`. */
    load: Array.isArray(r.loadAfter) ? r.loadAfter[0] : null,
  });
  /* KEYED ON THE SOURCE LOCATION, NOT ON THE LOCATION PLUS ITS REASON TEXT. A `check.h` assert states ONE
     capability per line and every site that reaches it carries the same words, so those merge on their own; a
     `quickjs-check.h` assert's whole MESSAGE is the
     cond, and it quotes the OPERAND — so `quickjs.c:6462` reached over `navigator.language.toLowerCase()` on
     one site and over a different expression on another, and ONE unbuilt capability at ONE line ranked as two
     one-site entries. That is precisely the split `degensym` above exists to prevent, one level up: it folds
     out an address or a hash and cannot fold out a source expression, because a source expression is not
     noise — it is the operand that reached the gap, and it is worth PRINTING rather than keying on. So the
     key is the line and the reasons collect under it. The measured cost of getting this wrong: with the
     reasons in the key, the top of this ranking read "every signature hit exactly one site", which is false
     of the corpus and is the one sentence a work queue must not get wrong. */
  for (const s of sigs) {
    const [at, ...rest] = s.split(' :: ');
    if (!bySig.has(at)) bySig.set(at, { sites: new Set(), reasons: new Set() });
    bySig.get(at).sites.add(r.id);
    bySig.get(at).reasons.add(rest.join(' :: '));
  }
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
  /* `src>reach>taint>sup` READ LEFT TO RIGHT IS WHERE THE @S SEARCH GOT TO. A `-` here is one of two facts
     and only the shouted line under the table tells them apart: the pass's INSTRUMENT could not answer (its
     rows carry no such field at all), or its RUNS carried no counters. */
  arrival: ['src', 'reach', 'taint', 'sup'].map((k) => spread(ms, k)).join('>'),
  epMax: Math.max(-1, ...ms.map((m) => m.endpoints).filter((x) => typeof x === 'number')),
  epAnswered: ms.filter((m) => typeof m.endpoints === 'number').length,
  sigs: [...new Set(ms.flatMap((m) => m.sigs))],
  wasm: [...new Set(ms.map((m) => m.wasm))],
}));

/* ABSENT PRINTS AS `-`, NEVER AS 0. A run that produced no result document is not a page that was analysed
   and found clean, and `String(null)` would have printed "null" into a numeric column. */
const pad = (s, n) => String(s).padEnd(n).slice(0, n);
console.log(`${passes.length} pass(es): ${passes.map((p) => p.label + '(' + p.rows.length + ')').join(' ')}`);
console.log(`list: ${list.rel} (${list.rows.length} sites, ${table.length} measured here)`);
console.log('\n' + pad('site', 20) + pad('outcome', 20) + pad('abort/n', 8) + pad('fin/n', 7) +
  pad('ep', 8) + pad('sinks', 7) + pad('src>reach>taint>sup', 21) + pad('flows', 12) + pad('switches', 12) +
  pad('load', 10) + 'signature');
for (const t of table)
  console.log(pad(t.id, 20) + pad(t.outcome, 20) + pad(t.abortedPasses + '/' + t.n, 8) +
    pad(t.finishedPasses + '/' + t.n, 7) + pad(t.ep, 8) + pad(t.sk, 7) + pad(t.arrival, 21) +
    pad(t.fl, 12) + pad(t.sw, 12) + pad(t.ld, 10) + (t.sigs[0] ? t.sigs[0].split(' :: ')[0] : '-'));

/* WHICH PASSES COULD ANSWER THE @S ARRIVAL QUESTION AT ALL, printed where the column is read. `n/N` above
   counts the passes that MEASURED; it says nothing about how many of them carried this particular counter,
   and a reader cannot recover the difference from a `-`. This is not hypothetical bookkeeping: this
   project's own baseline was quoted as a four-pass measurement in which one site read `70>0>0>0`, and that
   observation — the one the whole security-half argument rested on — came from a SINGLE pass, because the
   other three predate the field. An n of 1 stated as an n of 4 is the loaded-machine defect in reverse: not
   a number about nothing, a number about less than it claims. */
const predates = passes.filter((p) => p.arrival === 'predates').map((p) => p.label);
const carried = passes.filter((p) => p.arrival === 'carried').map((p) => p.label);
if (predates.length)
  console.log('\n*** THE `src>reach>taint>sup` COLUMN IS OVER ' + carried.length + ' OF ' + passes.length +
    ' PASS(ES) — ' + predates.join(', ') + ' predate(s) those counters entirely (the rows carry no such ' +
    'field), so a `-` there is this instrument being unable to ask, NOT a page with no attacker sources. ' +
    'Every other column is over all ' + passes.length + '. ***');

/* THE WORK QUEUE. A DFAIL's reason names what to build, so it is printed rather than summarised -- but only
   the head of it, because one 1169-character reason per row buries the RANKING, which is the thing this
   section exists to show. `-v` prints them whole. */
const verbose = process.env.SIGS === 'full';
console.log('\n=== distinct crash signatures, ranked by sites hit ===');
/* EACH SITE CARRIES ITS OBSERVED STACK HERE, WHICH IS WHAT MAKES THE RANKING GENERALISE. A signature hit by
   three sites is one number; a signature hit by three sites that all ship the same bundler is a statement
   about what to reproduce, and one hit by three unrelated stacks is a statement that it is not the bundler.
   This is also the reader the `stack` column never had: it was looked up for every row and printed nowhere,
   so the lookup could be wrong for an entire census — as it was — with nothing in the output to show it. */
for (const [at, e] of [...bySig.entries()].sort((a, b) => b[1].sites.size - a[1].sites.size)) {
  const sites = [...e.sites].map((i) => i + ' [' + (stacks.get(i) || '').slice(0, 44) + ']').join('\n            ');
  /* EVERY DISTINCT REASON THIS LINE GAVE, because when one line has several the difference between them is
     the OPERAND that reached the gap — which is the most specific thing this report can hand the next
     reader, and the thing a merged count would throw away in the act of merging. */
  const why = [...e.reasons].map((w) => verbose || w.length <= 300 ? w : w.slice(0, 300) + ' …(SIGS=full for the rest)');
  console.log(`${e.sites.size}  ${at}` + (e.reasons.size > 1 ? `   (${e.reasons.size} distinct operands)` : '') +
              why.map((w) => '\n     ' + w).join('') + `\n     sites: ${sites}`);
}
/* THE SAME QUEUE ONE LEVEL COARSER, BECAUSE A DEFECT FAMILY OUTRANKS ITS MEMBERS AND THE FINE RANKING HIDES
   IT. The queue above keys on `file:line`, which is right for "what do I open" and wrong for "what is the
   biggest thing wrong" — the previous census's top cause was three layout files (`flow_position.c`,
   `scrolling_area.c`, `replaced_element.c`) hitting three different sites, and every one of them ranked as a
   one-site entry BELOW a two-site entry, so the thing to build read as three small things. Nobody derived
   "the layout family is #1" from this output; they derived it by eye, which means the output was not the
   work queue it claims to be. Grouping by the signature's DIRECTORY is the coarsening that costs nothing and
   is not a guess: `browser/core/layout` is a component boundary this tree already draws. Both views print,
   because a family with one member is exactly as informative as the file ranking and a family with five is
   not something the file ranking can say at all. */
const byDir = new Map();
for (const [at, sig] of bySig) {
  /* `at` IS ALREADY `file:line` -- it is the key this map was built under and the string printed verbatim by
     the ranking above, so a ` :: ` split here parsed a shape that never reaches this line. The value beside
     it is `{sites, reasons}` and never a bare list; iterating it directly threw on the FIRST entry, which is
     why nothing below this loop -- the component ranking, the unnamed-abort count, and the one-artifact
     check that is the whole guarantee a census is ONE measurement -- has ever printed a line. */
  const dir = at.replace(/\/[^/]*:\d+$/, '') || '(root)';
  if (!byDir.has(dir)) byDir.set(dir, { sites: new Set(), sigs: new Set() });
  const e = byDir.get(dir);
  for (const i of sig.sites) e.sites.add(i);
  e.sigs.add(at);
}
console.log('\n=== the same aborts grouped by COMPONENT, ranked by sites hit ===');
for (const [dir, e] of [...byDir.entries()].sort((a, b) => b[1].sites.size - a[1].sites.size ||
                                                          b[1].sigs.size - a[1].sigs.size))
  console.log(`${e.sites.size} site(s) hit, ${e.sigs.size} distinct source location(s)  ${dir}\n` +
              `     ${[...e.sigs].map((x) => x.split('/').pop()).join(' ')}\n` +
              `     sites: ${[...e.sites].join(', ')}`);

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
