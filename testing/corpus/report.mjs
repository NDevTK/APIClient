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
//   RAN            no abort: the engine ran flows. `terminal` then says what those runs ENDED as, in the
//                  producer's own words, and `fin/n` counts the passes that reached `complete`. Read the two
//                  TOGETHER and never `fin/n` alone: a `0/n` beside `partial x n` is a dwell that expired
//                  while the engine was still exploring -- unbounded exploration behaving correctly -- and a
//                  `0/n` beside anything else is a different fact entirely.
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { siteList } from './list.mjs';

const ROOT = new URL('.', import.meta.url).pathname;
/* THE RUN-OUTCOME VOCABULARY, DERIVED FROM THE PRODUCER'S OWN DECLARATION AND NEVER RESTATED HERE. Four
   words leave extension/bridge.js on every run record and it declares them in one line; a copy typed into
   this file would be the second copy CLAUDE.md forbids (an auditor DERIVES the rule it checks, which is why
   idlgen reads the real .idl and the type-crossing audit parses the switch it audits). The day a fifth word
   arrives, a hand-kept reader buckets it into "did not finish" and reports a NEW run state as the old one --
   silently, because every column below partitions by exactly this vocabulary. Read from the declaration, an
   unknown word THROWS instead, which is the only arm that cannot mis-report. */
const RUN_OUTCOMES = (() => {
  const decl = readFileSync(join(ROOT, '..', '..', 'extension', 'bridge.js'), 'utf8')
    .match(/const RUN_OUTCOMES = \[([^\]]*)\]/);
  if (!decl) throw new Error('report.mjs: extension/bridge.js no longer declares `const RUN_OUTCOMES = [...]`.\n' +
    '  This file reads the run-outcome words off that line rather than keeping its own copy, so a rename there\n' +
    '  has to be seen HERE rather than silently leaving every outcome unrecognised. Re-point this reader.');
  return JSON.parse('[' + decl[1] + ']');
})();
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
             : measured.some((r) => 'sourceReads' in r) ? 'carried' : 'predates',
           /* AND THE SAME QUESTION ASKED SEPARATELY OF THE ORPHAN PAIR, because the two censuses were added
              to site.mjs at different times and a pass can carry one and not the other — folding them into
              one `arrival` state would report an instrument that could not ask about drives as one that
              could, in whichever direction the @S half happened to answer. */
           orphan: measured.length === 0 ? 'nothing-measured'
             : measured.some((r) => 'orphansAsked' in r) ? 'carried' : 'predates' };
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
  /* WHAT THIS RUN ENDED AS -- THE FACT `finished` WAS INFERRING, AND IT HAD IT EXACTLY INVERTED.
     `r.docsAnswered > 0 && !r.crashedMine` stood here under a banner promising it says "whether the analysis
     RETURNED within the dwell ... or was still exploring", and it says NEITHER. `docsAnswered` counts
     documents carrying a TERMINAL `_astRun`, and offscreen-brain.js writes that word on exactly three arms --
     `complete`, `crashed`, `nothing-to-run`, which is its own DCHECK's list -- while a `partial` never reaches
     it at all. So the left conjunct is TRUE for a run that DIED and FALSE for a run that learned seventy
     addresses and is still exploring, which is the opposite of what this column is read for. The
     `&& !crashedMine` beside it is not a second question: it is a patch cancelling the left half's error, and
     the two together are MUTUALLY EXCLUSIVE over every row in which an engine ran.
     MEASURED, over this corpus's 54 rows (derive it: read every census-cc-*.jsonl and count):
     `docsAnswered > 0 && !crashedMine` is true for TWO, both carrying `runsMine: 0` and `runOutcomesMine: {}`
     -- rows where NO ENGINE RAN AT ALL. Zero counterexamples to `docsAnswered > 0 => crashedMine > 0` among
     rows that ran. The column was a predicate that can only be true when nothing happened, which is
     CLAUDE.md's assert-whose-two-sides-cannot-disagree arriving in a report column instead of in a C assert:
     a non-check wearing the syntax of a check, printing a plausible `0/n` for all 18 sites over 48 passes.
     IT ALSO CREDITED THE ONE OUTCOME THAT HAS NO DOCUMENT BY CONSTRUCTION. bridge.js documents
     `nothing-to-run` as "No document, and NOT a crash", and both surviving true rows are that -- so the
     column read "the analysis finished" off the arm where there is nothing to have finished.
     `complete` IS THE FACT and is read off the RUN RECORD rather than inferred from a document slot.
     WHY THIS IS NOT A MOVE INTO A DEAD CHANNEL, which is the one thing that would make it worse rather than
     better: `complete` has fired ZERO times in 63 run records here (36 `partial`, 27 `crashed`), so a bare
     `fin 0/n` off the new predicate would be precisely the silent zero the old one already printed. The
     PARTITION is published beside it, so `0/5` now reads as `partial x5` -- a 60s dwell that expired while
     the engine was still exploring, which under §NO BOUNDS is the engine behaving correctly and not a failure
     -- and can never again be read as a result the trusted zone dropped. The discriminator is the deliverable;
     the predicate is only what it explains.
     NAMED RESIDUAL -- WHAT IS NOT COVERED: this pair says what a run ENDED as and cannot say WHY a `partial`
     never became a `complete`, because the two candidate reasons leave the same word: the dwell expired with
     the frontier still draining, or the run reached `finish` and something below it failed. WHAT THE NEXT DIFF
     BUILDS: site.mjs carrying the engine's own frontier-drained statement off the last partial's document, so
     a `partial` at dwell-end is separable from a frontier that emptied. HOW ITS ABSENCE WOULD SHOW: a corpus
     in which every row reads `partial` and no column can say whether a longer dwell would change any of it. */
  const outc = r.runOutcomesMine || {};
  for (const w of Object.keys(outc))
    if (!RUN_OUTCOMES.includes(w))
      throw new Error(`report.mjs: row \`${r.id}\` (pass ${p.label}) carries the run outcome \`${w}\`, which is\n` +
        `  not one of the words extension/bridge.js declares (${RUN_OUTCOMES.join(', ')}). Every column below\n` +
        `  partitions by that vocabulary, so an unknown word is counted as "did not finish" and a NEW run state\n` +
        `  is reported as the old one. Teach this file the word, or fix the producer.`);
  /* AND THE PARTS SUM TO THE TOTAL, asserted where both are in one hand. A partition whose members can drift
     from the count they are drawn from is one nobody can reason from, and this is the single check that makes
     `terminal` quotable beside `runs`. site.mjs builds both off the SAME `myRuns` array, so it holds for all
     54 rows today: it costs nothing and can only fire on a real regression. */
  const outcSum = Object.values(outc).reduce((a, b) => a + b, 0);
  if (outcSum !== r.runsMine)
    throw new Error(`report.mjs: row \`${r.id}\` (pass ${p.label}) reports ${r.runsMine} runs at its origin\n` +
      `  while its runOutcomesMine sums to ${outcSum}. site.mjs composes both from the same \`myRuns\` array,\n` +
      `  so a disagreement means one of them is being built over a different population and the outcome\n` +
      `  breakdown describes runs that the count does not.`);
  if (!seen.has(r.id)) seen.set(r.id, []);
  seen.get(r.id).push({
    pass: p.label, outcome, finished: (outc.complete || 0) > 0, outc,
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
    /* THE ORPHAN PAIR, WHICH IS THE HEADLINE SURFACE AND HAD NO COLUMN AT ALL. §What-the-tool-produces makes
       the drive of code the bundle shipped and never ran the whole proposition — "a sniffer shows what FIRED;
       this shows what the bundle CAN do but didn't" — and every layer between the engine and this line was
       already carrying it: solver/result.c emits `_orphansDriven`/`_orphansAsked` in the cost snprintf,
       bridge.js asserts both are numbers and forwards them onto every run record, site.mjs writes both into
       every census row under a comment saying they are read BOTH OR NEITHER. This file, the one that RANKS
       the corpus, was the consumer that never asked — so the pair was computed, asserted, relayed, stored,
       and rendered by nothing, which is the write-with-no-reader half of the contract site.mjs's own comment
       names one hop earlier ("harder to see, because the value is real and asserted and consumed by
       nothing"). It is the third time this row has been that consumer; `candidates` and `unitsDone` were the
       first two, and both of those have their own comments above saying so. */
    oask: r.orphansAsked, odrv: r.orphansDriven,
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
  /* THE OUTCOME PARTITION ACROSS EVERY PASS, WHICH IS WHAT MAKES `fin/n` A READING RATHER THAN A SHRUG.
     Summed over the passes rather than listed per pass, because the question a reader brings to a `0/n` is
     "what did those n runs end as", and the answer is a distribution. Printed in the PRODUCER's declaration
     order so two sites are always comparable left to right, and `no-run` where no engine ran at this origin
     at all -- which is a third fact and had been sharing a column with the other two. */
  terminal: (() => {
    const t = {};
    for (const m of ms) for (const w of Object.keys(m.outc)) t[w] = (t[w] || 0) + m.outc[w];
    const parts = RUN_OUTCOMES.filter((w) => t[w]).map((w) => w + '\u00d7' + t[w]);
    return parts.length ? parts.join(' ') : 'no-run';
  })(),
  n: ms.length,
  ep: spread(ms, 'endpoints'), fl: spread(ms, 'flows'), sw: spread(ms, 'switches'),
  sk: spread(ms, 'sinks'), rn: spread(ms, 'runs'), ld: spread(ms, 'load'),
  /* `src>reach>taint>sup` READ LEFT TO RIGHT IS WHERE THE @S SEARCH GOT TO. A `-` here is one of two facts
     and only the shouted line under the table tells them apart: the pass's INSTRUMENT could not answer (its
     rows carry no such field at all), or its RUNS carried no counters. */
  arrival: ['src', 'reach', 'taint', 'sup'].map((k) => spread(ms, k)).join('>'),
  /* `ask>drv` READ LEFT TO RIGHT IS WHERE ORPHAN-DRIVING GOT TO, in the order the mechanism travels — a flow
     runs out of work and ASKS whether the page shipped code nothing called, and a body is then DRIVEN. That
     order is what makes the zero readable, and it is the whole reason the pair is one column: `0>0` is a
     frontier that never reached the question (a scheduling result to act on), `N>0` is a walk that ran and
     found the heap empty (a fact about the page), and only the two together tell them apart. Printed as one
     spread per number rather than as a ratio, because a ratio of two spreads is a number nobody measured. */
  orphans: ['oask', 'odrv'].map((k) => spread(ms, k)).join('>'),
  epMax: Math.max(-1, ...ms.map((m) => m.endpoints).filter((x) => typeof x === 'number')),
  epAnswered: ms.filter((m) => typeof m.endpoints === 'number').length,
  sigs: [...new Set(ms.flatMap((m) => m.sigs))],
  wasm: [...new Set(ms.map((m) => m.wasm))],
}));

/* ABSENT PRINTS AS `-`, NEVER AS 0. A run that produced no result document is not a page that was analysed
   and found clean, and `String(null)` would have printed "null" into a numeric column. */
const pad = (s, n) => String(s).padEnd(n).slice(0, n);
/* THE `terminal` COLUMN SIZES ITSELF TO ITS WIDEST VALUE, because `pad` TRUNCATES and a truncated partition
   is not a narrow partition, it is a wrong one -- `crashed x6 partial x3` clipped to `crashed x6 par` reads
   as a site that only ever crashed. A fixed width chosen today is a width that silently starts lying the
   first time a site produces three different outcomes. Derived from the rows, it cannot. */
const termW = Math.max('terminal'.length, ...table.map((t) => t.terminal.length)) + 2;
console.log(`${passes.length} pass(es): ${passes.map((p) => p.label + '(' + p.rows.length + ')').join(' ')}`);
console.log(`list: ${list.rel} (${list.rows.length} sites, ${table.length} measured here)`);
console.log('\n' + pad('site', 20) + pad('outcome', 20) + pad('abort/n', 8) + pad('fin/n', 7) +
  pad('terminal', termW) +
  pad('ep', 8) + pad('sinks', 7) + pad('src>reach>taint>sup', 21) + pad('ask>drv', 13) +
  pad('flows', 12) + pad('switches', 12) +
  pad('load', 10) + 'signature');
for (const t of table)
  console.log(pad(t.id, 20) + pad(t.outcome, 20) + pad(t.abortedPasses + '/' + t.n, 8) +
    pad(t.finishedPasses + '/' + t.n, 7) + pad(t.terminal, termW) +
    pad(t.ep, 8) + pad(t.sk, 7) + pad(t.arrival, 21) +
    pad(t.orphans, 13) +
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

/* AND THE SAME SHOUT FOR `ask>drv`, ASKED SEPARATELY. It is not decoration on the line above: the two
   censuses entered site.mjs at different commits, so a pass can carry one and predate the other, and a
   single shout covering both would report one instrument's silence as the other's answer. It matters more
   here than there, because the orphan column's `-` and its `0>0` are the two readings this project is most
   likely to conflate — "the frontier never reached the question" and "this file could not ask" — and only
   this line separates them. */
const oPredates = passes.filter((p) => p.orphan === 'predates').map((p) => p.label);
const oCarried = passes.filter((p) => p.orphan === 'carried').map((p) => p.label);
if (oPredates.length)
  console.log('\n*** THE `ask>drv` COLUMN IS OVER ' + oCarried.length + ' OF ' + passes.length +
    ' PASS(ES) — ' + oPredates.join(', ') + ' predate(s) the orphan census entirely (the rows carry no ' +
    '`orphansAsked` field), so a `-` there is this instrument being unable to ask, NOT a frontier that ' +
    'never reached the question and NOT a bundle that ships no uncalled code. ***');

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
/* WHERE THE LEARNED ADDRESSES CAME FROM, WHICH IS THE ONE QUESTION THIS TABLE COULD NOT ASK AND THE ONLY ONE
   THE PRODUCT IS ABOUT. §What-the-tool-produces is 'what the bundle CAN do but didn't' — addresses the code
   COMPUTED — and an endpoint COUNT is silent about that: an address harvested from markup and one derived by
   running the bundle are one number here, and a plain HTML parse finds the first without a solver at all.
   MEASURED, BY HAND, BEFORE THIS EXISTED — which is why it exists. On gitpod the document's 89
   `<link rel=modulepreload>` hrefs were learned 89 of 89 and its ONE `<script src>` — the module entry it
   actually loads — was learned 0 of 1. On gitlab, three independent passes identical: 17 `<script src>`,
   ZERO learned, and the four addresses it did learn are `.woff2` FONTS. Two bundler shapes, three real
   sites, and not one address that could only have come from executing something.
   AND THE INFERENCE I FIRST DREW FROM THAT WAS WRONG, WHICH IS RECORDED HERE BECAUSE A WRONG CONCLUSION FROM
   RIGHT EVIDENCE IS INHERITED AS METHOD. This comment said `hints 89/89, scripts 0/1` reads as a solver that
   has not run. It does not. A lane READ the sites: `endpoint_record` has exactly ONE call in html_script.c
   and it is on the TAINTED-`src` arm, with a `return` after it — so a parser-inserted `<script src>` with a
   CONCRETE address never records at all, by design, because a bundle's own chunk is a program load and not
   an API endpoint. The 89 hints are html_link.c recording deliberately (`a modulepreloaded chunk is an
   address the bundle NAMED`), and gitlab's fonts are the `<link rel=preload>` arm of the same policy.
   ALL THREE POPULATIONS ARE MARKUP-RECORDING SITES BY DESIGN, so a comparison of learned-against-markup
   cannot tell `learned by executing` from `recorded at the element` — it asks only about the population
   execution would NOT produce. The arithmetic was sound and the conclusion did not follow from it.
   SO THE COLUMN THAT ANSWERS THE QUESTION IS THE COMPLEMENT, and that lane named it: an address in the
   learned set that the document names NOWHERE — not a script, not a link, not an image. That is the only
   population code could have composed. MEASURED: gitpod 0 of 90, gitlab 0 of 4. It is a FLOOR IN THE SAFE
   DIRECTION, because a regex can only UNDER-count the markup, which INFLATES this set — and it came back
   empty anyway. It does not prove nothing executed; it proves no learned address REQUIRED execution, which
   is the strongest claim the data supports and is all this prints.
   FROZEN ROWS ONLY, AND THE ABSENCE IS SAID RATHER THAN SKIPPED. A live row has no mirrored document to
   compare against, so it is reported as unmeasurable HERE rather than dropped — an absent split and a split
   of zero are different facts. The markup read is a REGEX over the served bytes and not a parse, so it is a
   floor: a script a document writes from script is not in it, which is the direction that UNDERSTATES the
   inversion and therefore cannot manufacture one. */
const strip = (s) => { const u = String(s).replace(/^[A-Z]+ /, '');
                       try { return new URL(u).pathname.replace(/^\/_m\/[^/]+/, ''); } catch { return u; } };
const provenanceSplit = [];
for (const t of table) {
  const doc = join(ROOT, 'mirror', t.id, 'index.html');
  if (!existsSync(doc)) { provenanceSplit.push({ id: t.id, split: 'NO MIRROR — not measurable here' }); continue; }
  const html = readFileSync(doc, 'utf8');
  const scripts = [...new Set([...html.matchAll(/<script[^>]*\ssrc="([^"]+)"/g)].map((m) => m[1]))];
  /* EVERY URL THE MARKUP NAMES, not only the script-like ones: the complement is only meaningful against
     the WHOLE document, since a font or an icon the engine recorded is markup-derived too. */
  const named = new Set([...scripts,
    ...[...html.matchAll(/<link[^>]*\shref="([^"]+)"/g)].map((m) => m[1]),
    ...[...html.matchAll(/<img[^>]*\ssrc="([^"]+)"/g)].map((m) => m[1])]);
  const hints = [...new Set([...html.matchAll(/rel="(?:modulepreload|preload)"[^>]*href="([^"]+)"/g)]
                            .map((m) => m[1]))];
  /* THE RAW ROWS AND NOT `t` — MY OWN FIRST VERSION READ `t.siteEndpoints` AND THE TABLE ROW DOES NOT
     CARRY IT, so it printed `hintsLearned: 0` where the truth is 89 of 89. That is not a smaller number,
     it is the INVERSE FINDING: `0/89` reads as an engine that learned nothing from the document, and
     `89/89` reads as one that learned the document's markup and none of its code. Caught only because
     the totals line two lines down said 94 endpoints while this said zero — two numbers from one run
     disagreeing, which is the cheapest check there is and needed no second command.
     SO IT READS THE CENSUS FILES DIRECTLY, independent of this file's own table shape, which is also
     what stops it drifting the next time a column is added. */
  const learned = new Set(passes.flatMap((pp) => pp.rows.filter((r) => r.id === t.id))
                                .flatMap((r) => r.siteEndpoints || []).map(strip));
  provenanceSplit.push({ id: t.id, learned: learned.size,
                         scriptsInDoc: scripts.length, scriptsLearned: scripts.filter((s) => learned.has(s)).length,
                         hintsInDoc: hints.length, hintsLearned: hints.filter((s) => learned.has(s)).length,
                         /* THE ONLY COLUMN THAT COULD SHOW EXECUTION-DERIVED LEARNING. */
                         learnedButNamedNowhereInMarkup: [...learned].filter((s) => !named.has(s)).length });
}
console.log('provenance split (frozen rows; a learned address the document NAMES was not derived by running '
            + 'anything): ' + JSON.stringify(provenanceSplit, null, 1));
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
