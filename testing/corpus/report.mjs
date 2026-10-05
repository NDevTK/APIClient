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
//                  TOGETHER and never `fin/n` alone, AND READ `dwellMs` BESIDE BOTH: a `0/n` beside
//                  `partial x n` is ONE OF TWO FACTS and this header used to assert the flattering one.
//                  THE RETIRED WORDING, KEPT BECAUSE A READER WHO RE-DERIVES IT FROM §NO BOUNDS WILL WRITE IT
//                  AGAIN, read: "a `0/n` beside `partial x n` IS a dwell that expired while the engine was
//                  still exploring -- unbounded exploration behaving correctly". That is one of the two reasons
//                  the `finished` residual 440 lines below says leave the SAME WORD, and the other is that the
//                  run reached `finish` and something under it failed. So the header was stating as settled
//                  the thing the file's own residual declares undecidable, in the READING POSITION, where a
//                  reader who meets it last obeys it -- CLAUDE.md §A-CONTRADICTION-INSIDE-ONE-BULLET inside
//                  one instrument. Both halves were written by people who had the other in front of them.
//                  WHAT IS CHECKABLE IS THE MAGNITUDE OF THE FIRST REASON, AND IT WAS ON DISK THE WHOLE TIME:
//                  site.mjs writes `dwellMs` on every row and NOTHING in this tree read it, so the one
//                  question the residual's own absence clause names -- "whether a longer dwell would change
//                  any of it" -- had no column. The `dwellMs` column and the WOULD-A-LONGER-DWELL section
//                  answer it per site, which kills "it just needed longer" where the span is wide and says
//                  UNSPANNED where a site was measured at one dwell. It does NOT settle which reason holds:
//                  a count of an OUTCOME cannot (§AN-INVARIANT-OVER-A-GATED-OPERATION), and stratifying one
//                  by the dwell is still a count of outcomes. A `0/n` beside anything other than
//                  `partial x n` is a different fact again.
import { readFileSync, existsSync, readdirSync, writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { siteList } from './list.mjs';
import { condVerdict, armed as abortliveArmed, VERDICT_PRESENT, VERDICT_CHANGED, VERDICT_ABSENT }
  from '../../engine/abortlive.mjs';

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
/* THE LIST IS READ BEFORE THE FILES ARE CHOSEN, BECAUSE THE LIST IS WHAT DECIDES WHICH FILES PAIR WITH IT.
   It used to be read 90 lines below, after every census had already been parsed, which is why the only way to
   select a population was to NAME it on the command line. */
const list = siteList();
const stacks = new Map([...list.byId].map(([id, r]) => [id, r.stack]));

/* THE PASS SET IS DERIVED FROM THE LIST OR IT IS NAMED, AND `--pair` IS THE DERIVATION. A reader handed
   `SITES=<list>.tsv node report.mjs <census files>` has been handed a PLACEHOLDER for the population, and the
   row-to-site guard below THROWS on a census this list does not name — correctly, because a census is a
   measurement OF a list. Those two together mean the only way to get an answer was to hand-pick a set that
   does not throw and report whatever it carried, which is a hand-chosen scope wearing a derivation's clothes:
   MEASURED, the easy selection is the OLD passes, and this reader was quoted as answering
   `passesCarryingTheField: 0` over a corpus in which 110 of 119 paired passes carry it. The figure was never
   drift — it was the placeholder.
   SO THE TOOL'S OWN REFUSAL BECOMES THE SELECTOR: `--pair` walks every `census-*.jsonl` beside this file and
   keeps the ones every row of which this list names. That is a scope derived from a CONSTRUCT (does this
   census measure this list) rather than from a NAME, so it cannot be short by whichever files a reader
   happened to type.
   IT PRINTS BOTH SETS AND NEVER ONLY THE ONE IT KEPT, which is the whole difference between a derivation and
   a silent filter. An excluded census is not noise: it is a measurement of a DIFFERENT list, and a reader who
   sees it excluded can go and pair it with the right one. A selector that printed only its survivors would be
   the certified-survivor shape — nobody re-examines a population somebody has just filtered.
   AND NAMING FILES BESIDE IT IS REFUSED rather than merged, because the two are opposite claims about where
   the population comes from and silently preferring either one would answer a question the reader did not
   ask. The NAMED mode keeps its throw exactly as it was: a reader who names a file and gets it wrong must be
   told, and that refusal is this reader working. */
/* THE DERIVATION IS THE DEFAULT AND A HAND-NAMED SET IS THE FLAG, WHICH IS THE ONLY SHAPE IN WHICH A
   PLACEHOLDER IS UNSPELLABLE. `--pair` landed as an OPT-IN, and an opt-in closes nothing: a reader who typed
   the bare command still got a single-file default, so `node report.mjs <census files>` stayed a sentence
   somebody could write and whatever set they picked stayed the population. CLAUDE.md records the cost of
   exactly that placeholder -- this file THROWS on a census whose rows its site list does not name, so a reader
   obeying the published command hand-picks a set that does not throw and reports whatever that set happens to
   carry, which is a hand-chosen scope wearing a derivation's clothes and which answered `0` for a field that
   110 of 119 passes carry. Inverting the default is what makes the derived population the thing a BARE command
   reports over, and a hand-chosen one the thing somebody has to ask for by name.
   A BARE FILENAME IS REFUSED RATHER THAN SILENTLY HONOURED, and the refusal names both forms. Honouring it
   would leave the old hole open under a new spelling; refusing it is the one answer that cannot be mistaken
   for either mode, which is the same reason `--pair` beside a named file was refused rather than arbitrated.
   `--pair` IS KEPT AS A NO-OP ALIAS so a command somebody already wrote still means what it meant -- it is not
   a second mode, it names the default. */
const argv = process.argv.slice(2);
const NAMED = argv.includes('--named');
const named = argv.filter((a) => a !== '--pair' && a !== '--named');
if (!NAMED && named.length)
  throw new Error('report.mjs: ' + named.length + ' file(s) were named without `--named`. The DERIVED pass ' +
    'set is the default now, because a command with a placeholder for its own population is a hand-chosen ' +
    'scope wearing a derivation\'s clothes. Run `node report.mjs` ALONE for the derivation over every census ' +
    'beside this one that measures the site list, or `node report.mjs --named ' + named.join(' ') + '` to ' +
    'report over exactly those files and nothing else.');
let files;
if (!NAMED) {
  /* The named-beside-derived refusal moved to the argv block above, where it covers BOTH spellings instead
     of only `--pair` -- which is what it has to cover now that the derivation is what a bare command does. */
  const all = readdirSync(ROOT).filter((f) => /^census-.*\.jsonl$/.test(f)).sort();
  const paired = [], excluded = [], rowsOf = new Map();
  for (const f of all) {
    let rows;
    try {
      rows = readFileSync(join(ROOT, f), 'utf8').trim().split('\n').filter(Boolean).map((l) => JSON.parse(l));
    } catch (e) { excluded.push([f, `unreadable or not JSONL: ${e.message}`]); continue; }
    const stray = rows.map((r) => r && r.id).filter((id) => !stacks.has(id));
    if (stray.length)
      excluded.push([f, `${stray.length} of ${rows.length} row(s) name a site \`${list.rel}\` does not — ` +
        `first \`${String(stray[0]).replace(/\s+/g, ' ').slice(0, 48)}\``]);
    else { paired.push(f); rowsOf.set(f, rows.length); }
  }
  console.log(`# --pair DERIVED the pass set from \`${list.rel}\`: ${paired.length} of ${all.length} ` +
    `census file(s) beside this one measure that list.`);
  console.log(`#   the selector is the row-to-site guard below, so this population is a CONSTRUCT and not a ` +
    `set anybody typed.`);
  for (const [f, why] of excluded) console.log(`#   EXCLUDED ${f} — ${why}`);
  if (!excluded.length) console.log('#   EXCLUDED none — every census beside this one measures this list.');
  if (!paired.length)
    throw new Error(`report.mjs: \`--pair\` found ${all.length} census file(s) beside this one and NONE of ` +
      `them measures \`${list.rel}\`. That is a statement about the pairing and not about the engine: the ` +
      `rows are scratch by design (\`.gitignore\` ignores them), so either this list is the wrong one for ` +
      `the corpus on this disk, or no pass has been taken against it. Try the other lists in this directory.`);
  /* THE DERIVED POPULATION IS WRITTEN TO A FILE BEFORE ANY FIGURE IS PRINTED, BECAUSE A DERIVATION THAT
     EXISTS ONLY IN A TERMINAL IS A DERIVATION THAT HAS TO BE TRUSTED. Everything above goes to stdout, which
     is the right place for it and is not a record: a reader who captures the figures (or whose scrollback
     scrolls, or who reads a relayed quotation of one line) holds a number whose population they cannot
     reproduce from any artifact on the disk. CLAUDE.md's standing demand is that a figure travel with the
     COMMAND that derives it, and the command here is bare `node report.mjs` — which is reproducible only if
     the SET that bare command resolved to is recorded somewhere, since the set is a function of which census
     files happen to be beside this one at the moment it ran, and those are scratch by design.
     IT GOES UNDER `logs/`, WHICH `.gitignore` ALREADY COVERS, and that is a choice rather than a convenience:
     a run's captured output is exactly what that directory is for, and writing a sibling of the censuses
     instead would put an untracked file in a SHARED checkout, where it is noise on every peer's
     `git status` for as long as it sits there.
     A FAILED WRITE THROWS AND PUBLISHES NOTHING. The alternative — print the figures and warn — leaves a
     reader holding exactly the state this record exists to end, with a warning they did not capture either;
     and a report that cannot say what it measured is §AN-UNSTAMPED-ARTIFACT's refusal arriving one level out,
     where the unnameable thing is the population rather than the revision.
     IT IS WRITTEN AFTER THE EMPTY-PAIRING THROW, deliberately: an empty population is not a population to
     record, and recording one would leave a file asserting that this list has no passes when the real finding
     is that the wrong list was handed in. */
  const RECDIR = join(ROOT, 'logs');
  const RECORD = join(RECDIR, `population-${list.rel.replace(/[^A-Za-z0-9._-]+/g, '_')}.txt`);
  const rec = [
    `# THE DERIVED PASS SET, written by report.mjs before it printed one figure. This file IS the population`,
    `# every number of that run is a part of; a figure quoted without it is a figure whose scope is a memory.`,
    `# written: ${new Date().toISOString()}`,
    `# site list: ${list.rel} (${list.byId.size} site(s))`,
    `# selector: every census-*.jsonl beside report.mjs EVERY ROW of which names a site this list names.`,
    `#   That is a CONSTRUCT and not a set anybody typed — but it is a NECESSARY condition only: a census`,
    `#   measuring a DIFFERENT list whose ids all happen to occur in this one is kept, and no row-to-site`,
    `#   test can see that. Read the exclusions below for what the construct rejected and why.`,
    `# re-run, exactly: cd testing/corpus && SITES=${list.rel} node report.mjs`,
    `# PAIRED ${paired.length} of ${all.length}`,
    ...paired.map((f) => `  ${f}\t${rowsOf.get(f)} row(s)`),
    `# EXCLUDED ${excluded.length}${excluded.length ? '' : ' — every census beside report.mjs measures this list'}`,
    ...excluded.map(([f, why]) => `  ${f}\t${why}`),
    '',
  ].join('\n');
  try {
    mkdirSync(RECDIR, { recursive: true });
    writeFileSync(RECORD, rec);
  } catch (e) {
    throw new Error(`report.mjs: the derived pass set could not be RECORDED at \`${RECORD}\` ` +
      `(${e.code || e.message}), so nothing is printed. This is not a tidiness check: the population a bare ` +
      `\`node report.mjs\` resolves to is a function of which census files are beside it at the moment it ` +
      `runs, and those are scratch by design — so a figure published without that set recorded is a figure ` +
      `whose scope exists nowhere and has to be taken on trust. Make that path writable, or name the ` +
      `population yourself with \`--named\`, which is the mode that says the scope is YOURS to state.`);
  }
  console.log(`#   RECORDED at logs/${RECORD.split('/').pop()} — the set above, as a file, so a figure from ` +
    `this run can be re-scoped by someone who did not watch it.`);
  files = paired;
} else {
  /* `--named` WITH NO FILES IS REFUSED RATHER THAN DEFAULTED TO ONE CONVENTIONAL NAME. The flag's whole
     content is that the population is YOURS rather than derived, so an empty one is a claim with nothing
     behind it -- and falling back to a single filename is the placeholder this flag exists to make explicit,
     reappearing as a default nobody typed. */
  if (!named.length)
    throw new Error('report.mjs: `--named` was given no files. It is the flag that says the population is ' +
      'yours rather than derived, so it cannot be empty, and it may not fall back to one conventional ' +
      'filename -- that fallback IS the placeholder. Name the files, or drop `--named` for the derivation.');
  files = named;
}
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
             : measured.some((r) => 'orphansAsked' in r) ? 'carried' : 'predates',
           /* AND THE SAME QUESTION ASKED A THIRD TIME, OF THE @S POLICY ENVELOPE, for the reason the pair
              above is already asked twice: the three censuses entered site.mjs at three different commits,
              so a pass can carry any of them and predate the others. Folding this one into `arrival` would
              report an instrument that could not ask whether a finding is policy-dead as one that could --
              and in THIS column that is the difference between a corpus of XSS findings and a corpus of
              findings the page's own policy kills, which is the one distinction §@S says must never be made
              by omission. */
           policy: measured.length === 0 ? 'nothing-measured'
             : measured.some((r) => 'policyEnvelope' in r) ? 'carried' : 'predates',
           /* AND A FOURTH, FOR THE ABSENT-GLOBAL CENSUS, asked separately for the reason the three above
              are. IT HAS A STATE THE OTHERS DO NOT, and folding it in would be the defect the field exists
              to report: site.mjs derives absent.c's member names through testing/absent_census.js and writes
              `absentFatal` when the member SET that composer declares is not the set the census on the record
              carries, so a row carrying that is the ARTIFACT AND THE TREE DISAGREEING ABOUT THE CENSUS and
              not an old instrument. `'absentAsked' in r` is false for both, so a probe that asked only that
              would print `predates` over a live drift and the column would go quietly dark while a shout said
              the pass was simply old. The two are separated here and the fatal one is shouted below with the
              message the row carries.
              THIS USED TO SAY A DISTINCTIVE SUBSTRING MATCHING OTHER THAN EXACTLY ONE KEY, which is the
              mechanism site.mjs had before it derived, and it is rewritten rather than deleted because the
              CONCLUSION is unchanged and a reader who re-derives the old mechanism re-introduces the old
              reading with it. What moved is what the state MEANS: under a substring match a reword in the
              composer was fatal even against a freshly built artifact, and under derivation it is not — both
              sides move together — so the only thing left that can fire this is the pair being out of step. */
           absent: measured.length === 0 ? 'nothing-measured'
             : measured.some((r) => 'absentFatal' in r) ? 'fatal'
             : measured.some((r) => 'absentAsked' in r) ? 'carried' : 'predates',
           /* AND A FIFTH, FOR THE OWED NAMES, ASKED SEPARATELY FROM THE PAIR IT RIDES BESIDE — which is the
              same argument the four above make and which is LIVE rather than prospective: site.mjs began
              writing `absentAsked` at one commit and `absentOwedNames` at a later one, so a pass between them
              carries the DIGIT and no queue. Folding the two would print an empty work queue over a pass that
              could not be asked, and an empty queue is this file's positive statement that no site owed a
              name — which is the clean bill. Opposite findings, one silence.
              IT INHERITS THE FATAL ARM RATHER THAN RE-DERIVING ONE, so the two states cannot disagree about
              which pass a row belongs to: a census whose members are not this tree's composer's yields no
              pair AND no names, and that is one fact about the artifact rather than two about two fields. */
           absentNames: measured.length === 0 ? 'nothing-measured'
             : measured.some((r) => 'absentFatal' in r) ? 'fatal'
             : measured.some((r) => 'absentOwedNames' in r) ? 'carried' : 'predates' ,
           /* AND A SIXTH, FOR THE COLUMN THIS PRODUCT IS ACTUALLY JUDGED BY, asked separately for the
              reason the five above are and with one more: CLAUDE.md §What-the-tool-produces' HARD BAR is
              "an address, a key or a value that NO PARSE of the served bytes can state, because it exists
              only at run time", solver/endpoint.c composes it PER ROW as `razorClass`, and until site.mjs
              began carrying it the bar was scored one DOCUMENT at a time and over no corpus at all.
              IT IS A QUESTION ABOUT THE INSTRUMENT AND NOT ABOUT THE ARTIFACT, which is why there are TWO
              channels and not one. This state answers whether the site.mjs that wrote the pass carried the
              FIELD -- a missing key. Whether the WASM that answered stated the fact is a DIFFERENT fact,
              carried per row as the string `(field-absent)` inside a key that is present, and it is read
              per site below. Folding the two would report a census taken against an old artifact as one
              taken by an old instrument, and those prescribe opposite work: rebuild and reinstall, against
              wait for a newer pass. */
           razor: measured.length === 0 ? 'nothing-measured'
             : measured.some((r) => 'endpointRazorClass' in r) ? 'carried' : 'predates',
           /* AND THE SAME TWO-CHANNEL SPLIT FOR THE DATA DOORS' ASK LADDER, for the reason stated one clause
              up and not restated: whether the site.mjs that wrote the pass carried the FIELD is a question
              about the INSTRUMENT, and whether the wasm that answered DECLARED the edge is a different fact
              carried per row and read per site below. They prescribe opposite work — wait for a newer pass,
              against a host that installs no such machine — so they are two channels here too. */
           netAsk: measured.length === 0 ? 'nothing-measured'
             : measured.some((r) => 'netDoorAsk' in r) ? 'carried' : 'predates' };
});
/* THE LIST THIS CENSUS MEASURED, NAMED AND THEN CHECKED AGAINST THE ROWS. This file used to read `sites.tsv`
   unconditionally and look every row's id up in it — and the app-page census walks twelve ids that appear in
   NO line of that file, so all twelve missed and `|| ''` turned every miss into a blank column. A report that
   was handed the wrong list printed as a complete table, which is the defaulted-field defect performed in the
   one artifact a reader trusts to say what was measured. The list is now a parameter (`SITES`, the spelling
   run.sh already used) and a row whose id it does not contain is FATAL: that row's every other column is a
   claim about a corpus nobody selected. */
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
/* THE PAIR BEHIND EACH SIGNATURE KEY, MEMOED WHERE IT IS PARSED AND NOWHERE ELSE. The ranking below needs the
   `cond` and the `reason` as TWO fields to ask whether the abort it names would still be the same abort, and the
   key it has is the two JOINED. Re-splitting that join is not available: the separator is ` -- ` and a reason is
   English prose that carries one, so a split would read a sentence as a C expression for exactly the records
   whose cond is the macro's unconditional constant. The key DETERMINES the pair -- it is built from it, one
   key to one pair -- so this is a memo and not a second answer that could disagree. */
const whyOf = new Map();
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
    const k = srcref(m[2]) + ' :: ' +
            (m[1] === CHECK_H_UNCONDITIONAL ? '' : degensym(m[1]) + ' -- ') + degensym(m[3]);
    out.add(k);
    whyOf.set(k, { at: srcref(m[2]), cond: m[1], reason: m[3] });
  }
  for (const m of t.matchAll(new RegExp(
        '@(?:WHY|E) (?!\\{)(?!(?:' + JS_MIRROR_TAGS + '):)([^\\n]*?) \\(([^()\\s]+:\\d+)\\)', 'g')))
    {
      /* THE SUBMODULE'S EMITTER WRITES ONE LINE AND NO COND FIELD, so the whole message is the reason -- and by
         quickjs's own `DCHECK(expr, "expr")` convention that message is very often the stringified expression,
         which is why the checker asks its reason of the message corpus AND of the cond corpus rather than
         choosing between them from the emitter. */
      const k = srcref(m[2]) + ' :: ' + degensym(m[1].trim());
      out.add(k);
      whyOf.set(k, { at: srcref(m[2]), cond: '', reason: m[1].trim() });
    }
  for (const m of t.matchAll(new RegExp('@(?:WHY|E) (' + JS_MIRROR_TAGS + '): ([^\\n"]{4,200})', 'g'))) {
    const k = 'extension/check.js (js side) :: ' + m[1] + ' ' + degensym(m[2].trim()).slice(0, 120);
    out.add(k);
    /* THE PAIR IS MEMOED WITH NO FILE AND THAT IS THE ANSWER RATHER THAN A HOLE. `extension/check.js` is where
       the MACRO is defined and never where the assert is written: the js mirror throws an Error, so it carries a
       message and NO file:line, and the message literal lives in whichever `extension/*.js` raised it. Keying
       the construct check on `check.js` would read every one of these as ABSENT -- a false retirement for the
       whole js side, which is the direction §AN-UNDER-CLAIM-IS-NOT-FOUND-BY-ACTING-ON-IT rates worst. So the
       pair is recorded with an EMPTY path and the verdict says why it cannot be asked.
       RESIDUAL — MET, AND ITS WORDING IS KEPT BELOW THE VERDICT BECAUSE WHAT A READER RE-DERIVES IS THE
       ARGUMENT FOR THE EMPTY PATH AND NOT THE ROUTING. `engine/abortlive.mjs` now takes a `zone` flag and
       searches the string literals of every tracked `.js` under `extension/`, which is what the clause named;
       the record states that corpus instead of leaving the path empty, so the band grades and each verdict NAMES
       THE FILE THAT HOLDS THE MESSAGE — which is the second half of what the band cost, since a key on the
       macro's own file buckets unrelated aborts under one entry and inflates its count.
       IT IS A FLAG AND NOT A PATH, which is the whole of why the argument below still stands: `check.js` is
       still the wrong file to ask, and nothing here asks it.
       The retired wording: "NAMED RESIDUAL. WHAT IS NOT COVERED: every js-side assert, which is the band this
       ranking reports with four sites hit and no verdict. WHAT THE NEXT DIFF BUILDS: a construct check whose
       corpus is the string literals of the whole `extension/` tree rather than one named file, which is the same
       reassembly `engine/abortlive.mjs` already does per file. HOW ITS ABSENCE WOULD SHOW: a js-side signature
       standing in this ranking under `cannot ask` while its message has been reworded or deleted, so a reader
       cannot tell a live js abort from a retired one at all." */
    whyOf.set(k, { at: '', cond: '', reason: m[2].trim(), zone: true });
  }
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
  /* AND THE `why` CHANNEL'S OWN TWO-SIDED CHECK, READ HERE BECAUSE THIS IS WHERE `why` IS CONSUMED. site.mjs
     drops the lines that QUOTE an abort — extension/bridge.js composes a crash record's `err` as
     `<message> | ROOT: <the @WHY line>`, so one abort used to render as TWO entries with the second
     JSON-escaped a level deeper and parseable by nothing. `whyQuotedWithoutRaw` is the assertion that the drop
     is LOSSLESS: empty string means every quotation's payload is contained in some raw one, and a non-empty
     value names payloads that exist ONLY as a quotation — which would mean that filter is discarding the only
     copy of an abort, and every signature below it is drawn from a `why` with a hole in it.
     IT IS A THROW AND NOT A COLUMN, for the reason this file already throws on a missing `logFile`: a signature
     ranking composed from an incomplete `why` is not a worse ranking, it is a ranking of a different thing, and
     the row that would explain it is the one being dropped. NULL IS NOT A FAILURE — site.mjs writes null when
     the transcript holds no `@WHY` at all, which is every non-crashing run, and `no abort` and `every quotation
     has its raw twin` are different facts. UNDEFINED is a row from a build of site.mjs that predates the field,
     and is left alone rather than read as clean, which is the polarity the span block records as having made
     its own `predates` arm dead. */
  if (typeof r.whyQuotedWithoutRaw !== 'undefined' && r.whyQuotedWithoutRaw !== null &&
      r.whyQuotedWithoutRaw !== '') {
    throw new Error(`report.mjs: row \`${r.id}\` in ${p.label} holds @WHY payloads that appear ONLY as a\n` +
      `  quotation inside a crash record, with no raw line anywhere in the transcript:\n` +
      `    ${JSON.stringify(r.whyQuotedWithoutRaw)}\n` +
      `  site.mjs drops ROOT-quoted lines so one abort stops reading as two, and that drop is sound only while\n` +
      `  every quotation has a raw twin. These do not, so the \`why\` this ranking is built from is MISSING an\n` +
      `  abort rather than merely de-duplicated. Read the transcript directly and fix the filter, not the row.`);
  }
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
     PARTITION is published beside it, so `0/5` now reads as `partial x5` -- and can never again be read as a
     result the trusted zone dropped. The discriminator is the deliverable; the predicate is only what it
     explains.
     THIS SENTENCE WENT ON "-- a 60s dwell that expired while the engine was still exploring, which under
     §NO BOUNDS is the engine behaving correctly and not a failure", AND IT IS KEPT HERE BECAUSE IT IS THE
     READING A READER RE-DERIVES FROM §NO BOUNDS AND IS WRONG IN TWO WAYS AT ONCE. It states as a FACT the
     first of the two reasons this comment's own residual declares undecidable, which is the same assertion
     the file's header carried and is answered there. And its coordinate is narrow: `60s` is one of SIX dwell
     values this corpus holds (60, 90, 120, 150, 180, 240 seconds -- read it off `dwellSecondsSeenAcross-
     ThePairedSet`, never off this line), so a sentence pricing the whole column at the shortest of them
     describes 47 of 125 rows. A magnitude written into prose beside a field the producer publishes is the
     coordinate CLAUDE.md's opening calls status: read the column.
     NAMED RESIDUAL -- HALF OF IT IS DISCHARGED BY THE DWELL COLUMN AND THE ABSENCE CLAUSE WAS EXACT.
     IT READ, AND THE WORDING IS KEPT BECAUSE A READER WHO RE-DERIVES IT WILL WRITE IT AGAIN: "this pair says
     what a run ENDED as and cannot say WHY a `partial` never became a `complete`, because the two candidate
     reasons leave the same word: the dwell expired with the frontier still draining, or the run reached
     `finish` and something below it failed. HOW ITS ABSENCE WOULD SHOW: a corpus in which every row reads
     `partial` and no column can say whether a longer dwell would change any of it."
     THE ABSENCE SHOWED EXACTLY THAT WAY AND THE SHOWING WAS THE DELIVERABLE, which is the one thing a
     three-clause residual is for. MEASURED at the revision the dwell column landed, over the DERIVED pass set
     (`SITES=apps.tsv node report.mjs`, bare -- a filename without `--named` is refused): 119 of 129 censuses
     pair, 125 rows, `complete` occurs in ZERO of them, and the partition is 184 `partial` + 37 `crashed` over
     221 run records. Every row of seven sites read `partial` or `crashed` and no column could say what a
     longer dwell would do -- while site.mjs had been writing `dwellMs` on every one of those rows and
     `git grep -in dwellms` answered ONE line in the whole tree, the write. The question was answerable from
     data already on disk and the field had no reader.
     WHAT THE DWELL HALF NOW SAYS, AND IT IS NOT A MAGNITUDE PROBLEM: the dwell spans SIX values over that set
     -- 60s(47 rows) 90s(48) 120s(8) 150s(14) 180s(1) 240s(1) -- and ONE SITE, gitpod, is measured at all six,
     101 rows and 200 run records, with `complete` ZERO at every value of a FOURFOLD span. So "it just needed
     a longer dwell" is refuted WITHIN ONE SITE at 4x, which is a controlled comparison rather than an argument
     because the document is held fixed. gitlab spans 60s..90s and reads the same; the other five sites are
     UNSPANNED and this instrument cannot ask of them.
     WHAT IS STILL NOT COVERED IS THE OTHER REASON, UNCHANGED: a `partial` that reached `finish` and failed
     under it. The dwell stratification cannot reach it -- CLAUDE.md §AN-INVARIANT-OVER-A-GATED-OPERATION, a
     count of an OUTCOME cannot separate two causes of that outcome, and stratifying one by an input is still
     a count of outcomes. WHAT THE NEXT DIFF BUILDS, UNCHANGED AND NOW THE WHOLE OF IT: site.mjs carrying the
     engine's own frontier-drained statement off the last partial's document, so a `partial` at dwell-end is
     separable from a frontier that emptied. Grep for it before building it -- this clause is a claim about a
     tree that moves, and the engine's `_cold` census is where such a statement would come from.
     HOW ITS REMAINING ABSENCE WOULD SHOW: a site whose dwell span is wide, whose `complete` is 0 across it,
     and for which no column can say whether its frontier ever emptied -- so a reader cannot tell unbounded
     exploration behaving correctly from a `finish` step that is failing, which are the two readings the header
     of this file used to collapse into the first. */
  const outc = r.runOutcomesMine || {};
  for (const w of Object.keys(outc))
    if (!RUN_OUTCOMES.includes(w))
      throw new Error(`report.mjs: row \`${r.id}\` (pass ${p.label}) carries the run outcome \`${w}\`, which is\n` +
        `  not one of the words extension/bridge.js declares (${RUN_OUTCOMES.join(', ')}). Every column below\n` +
        `  partitions by that vocabulary, so an unknown word is counted as "did not finish" and a NEW run state\n` +
        `  is reported as the old one. Teach this file the word, or fix the producer.`);
  /* AND THE PARTS SUM TO THE TOTAL, asserted where both are in one hand. A partition whose members can drift
     from the count they are drawn from is one nobody can reason from, and this is the single check that makes
     `terminal` quotable beside `runs`. site.mjs builds both off the SAME `myRuns` array, so where a row was
     measured at all the two cannot legitimately disagree.
     AND IT IS GUARDED BY `!r.fatal`, LIKE ITS THREE SIBLINGS ABOVE, BECAUSE A FATAL ROW HAS NO POPULATION FOR
     THE PARTS TO BE DRAWN FROM. A fatal row never drove the site: `runsMine` is `undefined` and `outc` is `{}`,
     so `0 !== undefined` and this throws -- the invariant has no SUBJECT there rather than being violated, which
     is the §AN-INVARIANT-OVER-A-GATED-OPERATION shape, an outcome check firing on an arm that correctly did
     nothing.
     THE RETIRED ARGUMENT IS KEPT BECAUSE A READER WHO RE-DERIVES IT WILL RE-REMOVE THE GUARD. This comment used
     to end "it holds for all 54 rows today: it costs nothing and can only fire on a real regression", and that
     is exactly §A-CURE-VALIDATED-ON-A-SHORT-EXAMPLE: the sentence is true, and it was checked against a census
     with no fatal row in it -- the one population where the defect cannot appear. It is not rare. run.sh emits a
     fatal row whenever a fixture server fails to bind, and a frozen apps.tsv census ALWAYS carries one, because
     that list deliberately keeps a row with no mirror. So the throw is GUARANTEED on precisely the census this
     project runs against frozen bytes.
     AND THE DIRECTION IS THE ONE THAT COSTS MOST: this does not make a number wrong, it makes the ENTIRE report
     unreadable, so one unbound fixture server hides every other site's measurement behind a stack trace about a
     row nobody was asking about. A lane hit it and worked around it by filtering the fatal row out of its own
     input, which is a reader repairing an instrument's refusal at the one place the repair cannot be seen. */
  const outcSum = Object.values(outc).reduce((a, b) => a + b, 0);
  if (!r.fatal && outcSum !== r.runsMine)
    throw new Error(`report.mjs: row \`${r.id}\` (pass ${p.label}) reports ${r.runsMine} runs at its origin\n` +
      `  while its runOutcomesMine sums to ${outcSum}. site.mjs composes both from the same \`myRuns\` array,\n` +
      `  so a disagreement means one of them is being built over a different population and the outcome\n` +
      `  breakdown describes runs that the count does not.`);
  if (!seen.has(r.id)) seen.set(r.id, []);
  seen.get(r.id).push({
    pass: p.label, outcome, finished: (outc.complete || 0) > 0, outc,
    /* THE BUDGET THE RUN WAS CUT AT, WHICH IS THE DENOMINATOR OF `fin/n` AND HAD NO READER ANYWHERE.
       `(outc.complete || 0)` above is a `|| 0` on a producer's field, which CLAUDE.md
       §A-FIELD-A-CONSUMER-DEFAULTS forbids by name -- and it is SAFE HERE for a reason that is asserted
       rather than assumed: the `outcSum !== r.runsMine` throw below fires on any non-fatal row that
       carries no `runOutcomesMine`, so a census predating the field cannot reach this line at all.
       MEASURED over the derived pass set (`SITES=apps.tsv node report.mjs`, bare): 119 of 125 rows carry
       `runOutcomesMine` and the SIX that do not are exactly the six `fatal` rows -- rows where no engine
       ran, which `terminal` already prints as `no-run`. So `fin 0/n` is a fact about 221 run records
       (184 partial, 37 crashed) and NOT an artifact of the default. It was worth checking: the symptom of
       the forbidden shape and the symptom of a real zero are the same digit.
       WHAT HAD NO READER IS THE DWELL. site.mjs:1278 writes `dwellMs` on every row it builds and
       `git grep -in dwellms` answered ONE line in the whole tree -- that write -- which is the
       write-with-no-reader half of the same rule, standing on the one field that prices the first of the
       two reasons a `partial` can have. A reader meeting `fin 0/106 partial x169` could not tell a 60s
       cut from a 240s one, and the corpus holds both. NOTHING IS ADDED TO ANY CENSUS BY THIS: the field is
       the producer's own, under the producer's own name and in the producer's own unit (the `Ms` is in the
       key so no consumer can read it as seconds), so no existing query over these files changes meaning --
       CLAUDE.md's new-key-matching-an-old-query hazard has no subject here. `git grep -in dwell` over the
       tree answers no programmatic reader at any spelling, so there was none to collide with either. */
    dwell: r.dwellMs,
    runs: r.runsMine, crashed: r.crashedMine,
    /* THE HEADLINE IS `distinctEndpoints` -- distinct ADDRESSES learned, which is what "endpoints" has to
       mean in a report. `r.endpoints` is the last log entry's CUMULATIVE counter and is kept beside it under
       a name that says so, because it is still the right thing to watch a live run advance by. This column
       read the counter, so a page that learned one address could print a two-digit `ep`. */
    endpoints: r.distinctEndpoints, counter: r.endpoints,
    sinks: r.sinks, flows: r.flows, switches: r.switches,
    /* MEMBERS MINTED PER COMPLETED UNIT OF WORK — the column that separates a frontier that BRANCHES from
       one that RUNS, and the row site.mjs writes into every census that this file has never read.
       WHY THE PAIR AND NEVER EITHER ALONE. `flows` is a LIFETIME TOTAL of every flow ever made — solver/
       result.c calls it "a TOTAL among totals" and renamed the frontier's live size to `live` precisely so
       the two could not share a word. `unitsDone` is solver/engine.c's `g_units_done`, incremented at
       flow_credit_visit's own site so the two cannot come to mean different things, and that site is HTML
       §8.1.4.4 "Calling scripts" step 3's empty-execution-context-stack boundary — so it counts COMPLETED
       UNITS OF WORK and nothing else. A flow count alone says a document is BUSY; a unit count alone says it
       is PRODUCTIVE; only the ratio says which of the two a run spent itself on, and that is the question
       §scheduler's aging term is structurally unable to answer — it charges the thread a member BURNED and a
       fork COPIES that charge, so a flow that forks instead of running is immune to the one term written to
       demote it.
       WHAT IT SEPARATES, MEASURED OVER THE ROWS OF THIS CORPUS THAT CARRY BOTH — one wasm artifact, eleven
       sites, mirror and live transports: astexplorer mints 5238 members off TEN units of work and replit
       completes 494 units for EIGHT members, a range of four and a half orders of magnitude. The reading is
       stable within a site across passes AND across transports to within 1.0-1.23x for NINE of the eleven and
       2.7x for helix. Gitlab alone is bimodal on one binary — seven mirror passes at 2, 2, 5, 151, 163, 192,
       193 flows, the low mode at loadBefore 3.4+ on a 4-core box, which is the wall-denominated bimodality
       this project already records rather than a property of the document. Its eighth row is NOT in that
       comparison and must not be pooled with them: its `finalUrl` is gitlab.com and the other seven are
       127.0.0.1, so it is a different document, and the transport is in the row for anyone to check.
       A between-site range that far above the within-site one is what makes this a reading of the DOCUMENT
       rather than of the run — the property §Testing says no reach column in this tree has been able to
       claim, and the reason this one is worth a column at all.
       WHAT IT CANNOT DO, AND THE CLAIM IT MUST NOT BE READ AS. It is a WHOLE-FRONTIER reading. It says
       whether a run spent itself branching; it says NOTHING WHATEVER about the two sides of any ONE branch,
       which is what solver/flow.h's `br_crowd_live`/`br_crowd_born`/`br_crowd_us` are for. A high ratio here
       is a reason to point that instrument at this site and is never a substitute for it.
       AND ITS DENOMINATOR HAS TWO SILENCES THIS COLUMN CANNOT SPLIT: solver/engine.c's own banner says a
       `unitsDone` near zero is either flows reaching that boundary and finding nothing to do, or NO FLOW
       EVER REACHING IT. `jobsQueued`/`jobsRun` is the pair that separates them, and site.mjs carries both.
       DERIVATION -- AND THE ONE THAT STOOD HERE DID NOT RUN, WHICH IS WORSE THAN A QUOTED FIGURE BECAUSE IT
       PROMISED ONE THAT DID. The sentence that used to be here read "the figure above is re-runnable
       rather than quoted: `node
       testing/corpus/report.mjs` over the committed `census-cc-*.jsonl` rows", and BOTH halves are false.
       That command reads `census.jsonl`, which this argv default names and which does not exist, so it exits
       ENOENT before parsing a row; and with the files passed it exits again demanding `SITES=`, because the
       cc corpus walked `apps.tsv` and this reader defaults to `sites.tsv`. Nor are the rows committed --
       `.gitignore` ignores `testing/corpus/census-*.jsonl` on purpose, so a fresh clone has none of them.
       Handing a reader a command is the right instinct and is what this file's own header asks for; handing
       them one nobody ran is the §A-CLAUSE-THAT-NAMES-A-MECHANISM defect, and it is read by exactly one
       person, once, at the moment they have decided to check. What actually runs, from this directory, is the
       list the census walked and the files it wrote:
         SITES=apps.tsv node report.mjs $(ls census-cc-*.jsonl)
       and it runs only against a corpus a session produced, because the rows are scratch by design. TESTED
       RATHER THAN OFFERED, which is the whole point of replacing the sentence above: on the disk this was
       written from that command exits 1, because ONE scratch file carries a row whose `id` is a paragraph of
       prose and the guard at the row-to-site check refuses it by name. That refusal is this reader WORKING --
       a census is a measurement OF a list, and a row the list does not name is exactly what it must not
       average in -- so the command is right and one input was not. Drop the offending file and it exits 0
       over the other 49. State a command you have run, and say what it did. */
    units: r.unitsDone,
      /* AND THE CLOCK BOUNDARY'S ARRIVALS, WHICH RIDE `frontierPrograms` RATHER THAN THE ROW'S TOP LEVEL (see
       site.mjs for why, and for the fact that its own author read the top level and concluded they were
       absent). `fire-due-timer` is an OUTCOME over a gated operation, so a 0 there is two opposite findings
       and these are the ask that separates them. */
    clockChain: (r.frontierPrograms || {}).stepReachedRenderingLife,
    clockTimerAsk: (r.frontierPrograms || {}).stepReachedTimerLife,
    clockIdleAsk: (r.frontierPrograms || {}).stepReachedIdleLife,
    clockTimerFired: (r.stepUnitRuns || {})['fire-due-timer'],
    /* THE ORDER-OR-THREAD ROWS, CARRIED VERBATIM — see the `ORDER OR THREAD` section for what each decides.
       They are copied rather than folded here because the verdict is composed once, at the print, and a
       fold at the read would put the boundary in two places. */
    picksLifetime: r.picksLifetime, starvedPicks: r.starvedPicks, starvedPicksIdle: r.starvedPicksIdle,
    neverPicked: r.neverPicked, neverPickedGap: r.neverPickedGap, neverPickedAtTop: r.neverPickedAtTop,
    picksLive: r.picksLive, picksMax: r.picksMax,
    visMin: r.visMin, visMax: r.visMax, visZero: r.visZero, wfqMembers: r.wfqMembers,
    plateauAsked: r.plateauAsked, plateauHeld: r.plateauHeld,
    plateauRuns: r.plateauRuns, plateauHeldIdle: r.plateauHeldIdle,
    fpu: (typeof r.flows === 'number' && typeof r.unitsDone === 'number' && r.unitsDone > 0)
      ? Math.round(100 * r.flows / r.unitsDone) / 100 : null,
    /* AND WHETHER ANYTHING EVER LEFT, READ OFF THE ENGINE'S OWN ROW RATHER THAN DERIVED FROM TWO OTHERS.
       This was `flows - wfqMembers` under a guard that the two halves came from one census entry, and the
       REPLACEMENT IS NOT A REFUTATION OF THAT READING: `_flows` is flow_created_count(), whose increment sits
       in flow_new beside `g_arrivals++` -- one constructor, one increment each, neither ever decremented --
       so the difference WAS `g_departures` exactly, and flow.c asserts that identity both at registry teardown
       and over the census it publishes. The subtraction was right and it was DERIVED, and solver/result.c
       already argues at length why a half like that is emitted rather than left to a consumer: it "is a number
       for every pair of inputs, including the pair where one of them stopped being written". The engine emits
       both halves into `_wfq`, bridge.js relays that object WHOLE, and site.mjs now reads them through the
       same `wfqRow` accessor as `members` -- so this is ONE SAMPLE BY CONSTRUCTION and the `wfqFrom ===
       countersFrom` guard is not weakened here, it is UNNECESSARY here, because there are no longer two
       differently-selected halves to reconcile.
       MEASURED, and the finding it was built to state STANDS: over the 50 `census-cc-*.jsonl` files present on
       one disk, 35 rows carry `flows`, `wfqMembers` and both indices; the indices AGREE in all 35 and the
       difference is ZERO in all 35, including rows minting 8940, 8220, 5355 and 4291 members -- so it is not a
       small-sample artifact. `parked` reads 0 in all 35 beside it.
       AND 19 OF THOSE 35 CARRY NO INFORMATION ABOUT RECLAMATION, WHICH THE HEADLINE COUNT HID. The question
       a sibling lane raised of a different instrument is owed here: is a zero a property of WHEN the table
       was read rather than of what the engine did? Asked and answered in two parts. It is NOT the sampler
       defect -- `_wfq` is composed by `result_wfq_json()` once per RESULT DOCUMENT, not by a tick sampler, so
       there is no first-tick table to mistake for a terminal one. And it is not structural, because the
       identity `arrivals - departures == members` is DCHECKed INSIDE the census "at the one moment all three
       terms are in one hand", so `flows - members` is a TRUE reading of `departures` at whatever instant the
       entry was composed at. What survives is the WEAK form, and it is real: a reading taken early is true and
       UNINFORMATIVE, because nothing has yet had time to depart. So the rows split. SIXTEEN were composed
       after work that forbids an early reading -- 8940 members minted against 7648 context switches, 8220
       against 6916, 5355 against 4210, 4291 against 3124 -- and those carry the finding. NINETEEN read
       `flows` at 1, 2, 4 or 8, which is indistinguishable from a run that barely started, and their zero says
       nothing either way. THE FINDING RESTS ON THE SIXTEEN. Quoting 35 was counting rows that cannot speak
       alongside rows that can, which is the evidence-inflation shape one row up from the usual one: not a
       derived row counted as independent, but an UNARMED row counted as a witness.
       AND THE WORD THAT STOOD HERE WAS `COMMITTED`, WHICH IS FALSE AND IS THE ONE WORD A READER ACTS ON.
       `git ls-tree -r origin/main -- testing/corpus/` names ZERO of them: `.gitignore` ignores
       `testing/corpus/census-*.jsonl`, DELIBERATELY and with its reason written beside it -- a live run's
       transcript is one sample, "what survives a live run is the distilled report ... not the transcript".
       That decision is right and this comment does not ask to undo it. What it means is that this figure is a
       QUOTE and not a derivation a fresh clone can re-run, so the sentence that offers it must not promise
       one: a reader who takes `committed` at its word greps a revision, finds nothing, and cannot tell a
       measurement nobody made from one whose evidence was never trackable. The gitignore's own next clause is
       the standing instruction -- anything here that deserves to last is quoted somewhere that is not that
       directory -- and this is that quote. To re-derive rather than trust it, run the corpus again. NOTHING THIS ENGINE MINTS ON A REAL DOCUMENT HAS EVER
       LEFT THE FRONTIER, and the caller table says why rather than leaving it to be guessed: `flow_remove` has
       exactly ONE call site, the last line of `flow_release`, and `flow_release` has four -- flow_finish (the
       flow COMPLETED), the pager's tail sale (RAM pressure), and twice in flow_registry_free's teardown drain.
       Teardown runs after the document is composed, so out there `departures` is `finished + sold`, and both
       are zero: nothing completes and nothing is paged.
       AND THE COMMITTED ROWS READ `-` RATHER THAN 0, WHICH IS A STATEMENT ABOUT THIS READER AND NOT ABOUT THE
       ENGINE. The artifact that produced them emits `departures`; site.mjs did not ask for it, so the rows do
       not carry it. A re-run fills them. Defaulting the absence to 0 would turn "this census predates the
       reader" into "the engine measured no departures", which is the same number reached by a route that
       proves nothing -- and on this row of all rows, since 0 is the finding.
       NOT COVERED: `departures` is ONE number for the THREE mechanisms above, so it cannot say whether a
       frontier that stopped growing retired its members or sold them, which are opposite verdicts. The engine
       already splits it -- `finished`/`finishedFlows`/`finishedCands` and `sold`/`soldFlows`/`soldCands` are
       composed into `_cold`, and bridge.js relays `_cold` onto every run record beside `wfq` (GREPPED: the
       relay loop over `["_cold", "_heap", "_swap", "_forkAt", "_absent"]`, and the record write `cold:
       result._cold`). THE NEXT DIFF reads those in site.mjs and prints them as columns here -- AND IT READS
       `pagedAsks` WITH THEM, WHICH IS THE HALF THAT MAKES `sold` READABLE AND WHICH THIS RESIDUAL NAMED ONLY
       THE OUTCOME OF. The sale is not a thing the scheduler decides: `reclaim_install(JS_GetRuntime(ctx),
       engine_reclaim_tail, NULL)` puts it on the ALLOCATOR'S REFUSAL EDGE, so `sold: 0` is both "the floor was
       never reached, nothing was ever asked" and "the pager was asked and declined" -- opposite findings, one
       about a dwell that never filled the heap and one about the pager itself. `pagedAsks` is engine.h's
       "times the allocator's refusal edge reached this engine" and is composed into `_cold` beside them, which
       is the record-at-the-ASK that this project's own rule asks for over any gated operation. A next diff
       that took the two outcome rows alone would rebuild the three-states-behind-one-answer shape this row
       just spent a commit getting out of. It is not
       done in this one because `_cold` is a SIBLING object of `_wfq`, so a reader of it needs its own sample
       index and its own agreement check against `countersFrom` -- exactly the reconciliation this row just
       stopped needing, and adding it back untested beside the row that shed it is how the two get confused.
       ITS ABSENCE SHOWS as a corpus in which `gone` is nonzero on some row and no column anywhere says which
       of retirement or paging produced it -- the reader is then back to the state `parked` alone was in. */
    gone: (typeof r.wfqDepartures === 'number') ? r.wfqDepartures : null,
    /* THE @S ARRIVAL CENSUS, WHICH IS WHAT MAKES `sinks: 0` A FINDING RATHER THAN A SHRUG. Read in the order
       a search travels -- a source is read, a sink is reached, taint arrives at one, the search is declined
       as unforgeable -- so the column says WHERE the zero starts, and a corpus-wide `sinks: 0` stops being
       one number with three opposite meanings. site.mjs carries these off the run record bridge.js writes. */
    src: r.sourceReads, reach: r.sinkReached, taint: r.sinkTainted, sup: r.sinkSuppressed,
    /* THE @S POLICY ENVELOPE, WHICH IS WHAT MAKES `sinks: N` A SECURITY FINDING RATHER THAN A COUNT.
       CLAUDE.md §@S: a firing breakout in the model is NOT yet a working exploit -- it has to run under the
       page's ACTUAL policy, and "sink REAL, CSP blocks: needs X" is a different verdict from a bare XSS.
       solver/solve.c has answered that on every detected sink all along and lib/popup-security.js badges out
       of the answer; site.mjs now carries it onto every row, and this file -- the one that RANKS the corpus
       -- is where it becomes readable. Until both landed NO INSTRUMENT ANYWHERE READ EITHER FIELD, and the
       derivation for that is run AT THE PARENT rather than at the tip, or it returns these lines and reads
       as a repair nobody needed. `git grep -c 'cspBlocks\|trustedTypes' <this commit>^ -- testing/` answers
       THREE files, none of them an instrument: the control pair stating the claim in its own prose, and
       control/serve.mjs's two lines arguing for the pair. Pipe it through `grep -v '[.]html:'` and only
       serve.mjs survives -- a glob pathspec is not written here because a star followed by a slash ends
       this comment. Both are stated because the
       unfiltered command is the one a reader types and its extra hits are the fixtures saying what nothing
       measured.
       ALL THREE NUMBERS OR NONE, because the two counts are worthless without their denominator and each
       other. `ent` is the sinks this walk actually looked at -- site.mjs's own count off the same arrays,
       never the `sinks` column beside it, which is read off the run record's last log entry and is a
       different population. `csp` and `tt` are TWO INDEPENDENT facts (popup-security.js's own banner) of
       which either one alone means the payload does not run on the real page, so reading one and not the
       other is how a sink under `require-trusted-types-for 'script'` gets badged a clean HIGH.
       `-` HAS TWO READINGS HERE AND THE SHOUT BELOW SEPARATES THEM: a pass that predates the field, and a
       site no document of which was ever answered. Neither is "this page's findings all survive its
       policy", which is `0>0` over a nonzero `ent`. */
    pent: r.policyEnvelope ? r.policyEnvelope.entries : null,
    pcsp: r.policyEnvelope ? r.policyEnvelope.cspBlocked : null,
    ptt:  r.policyEnvelope ? r.policyEnvelope.ttRequired : null,
    /* THE ORPHAN PAIR, WHICH IS THE HEADLINE SURFACE AND HAD NO COLUMN AT ALL. §What-the-tool-produces makes
       the drive of code the bundle shipped and never ran the whole proposition — "a sniffer shows what FIRED;
       this shows what the bundle CAN do but didn't" — and every layer between the engine and this line was
       already carrying it: solver/result.c emits `_orphansDriven`/`_orphansAsked` in the cost snprintf,
       bridge.js asserts both are numbers and forwards them onto every run record, site.mjs writes both into
       every census row under a comment saying they are read BOTH OR NEITHER. This file, the one that RANKS
       the corpus, was the consumer that never asked — so the pair was computed, asserted, relayed, stored,
       and rendered by nothing, which is the write-with-no-reader half of the contract site.mjs's own comment
       names one hop earlier ("harder to see, because the value is real and asserted and consumed by
       nothing"). AND THE SENTENCE THAT STOOD HERE CLAIMED TWO EARLIER FIXES THAT HAD NEVER HAPPENED —
       it said `candidates` and `unitsDone` "were the first two, and both of those have their own comments
       above saying so", and at the revision that sentence was written NEITHER NAME OCCURRED ANYWHERE IN
       THIS FILE EXCEPT INSIDE IT. That is the removal-announcement defect with the arrow reversed: a
       comment announcing a REPAIR closes the question exactly as one announcing a deletion does, so
       nobody greps, and the field goes on being computed, asserted, relayed and stored with no reader
       while a paragraph certifies that it has one. `unitsDone` is read above now; `candidates` still is
       not, and saying so is the only thing that keeps that true or false rather than merely claimed. */
    oask: r.orphansAsked, odrv: r.orphansDriven,
    /* AND WHICH EXIT EACH OF THOSE ASKS TOOK, which the pair above cannot say and which this file was again
       the consumer that never asked. The pair answers HOW FAR the question got; it is silent about what the
       ask DID, and the two readings of a high `asked` take OPPOSITE repairs — the generation cache answering
       with no walk at all (`memo`), the walk running and the heap holding no takeable body (`empty`), or the
       walk handing a body over (`took`). They sum to `asked`, which solver/result.c asserts where all four
       were read together and which `-DAPICLIENT_DEV=0` compiles out, so site.mjs carries the release-mode
       verdict and this line reads it rather than recomputing it.
       WHY THIS COLUMN RATHER THAN SOME OTHER: measured over three drives of one release artifact on one real
       app, `seed-one-orphan-flow` overran the cooperative slice in 204 of 221, 237 of 242 and 159 of 190 of
       its OWN runs — 84%, 85% and 83% of ALL overrunning turns in the run — and `JS_OrphanTakeOne` enumerates
       `rt->gc_obj_list` with no step boundary in it. That localisation came off `orphansDriven` sitting within
       one or two of `orphansAsked`, which is an INFERENCE from two rows published for a different question;
       these three STATE it, and they are the row that says whether the repair is per WALK or per ASK. */
    omemo: r.orphanAskMemo, oempty: r.orphanAskEmpty, otook: r.orphanAskTook,
    oexitWrong: r.orphanAskSumsWrong,
    /* …AND WHAT THOSE WALKS COST. Two means with two DIFFERENT denominators, which is why four numbers travel
       and no quotient is composed here: `oentries / owalks` is the mean object-list length one take reads, and
       `ofull / owfull` is the mean candidate population over the walks that ran to the END of the list. A walk
       that exited early saw a FLOOR of the candidate set and is excluded from the second pair on purpose, so
       `owfull` is its denominator rather than a detail. `owCountWrong` is site.mjs's release-mode reader for
       `walks == empty + took` — one take is ONE enumeration — which solver/result.c asserts in dev. */
    owalks: r.orphanWalks, oentries: r.orphanWalkEntries,
    owfull: r.orphanWalksFull, ofull: r.orphanWalkFullCandidates,
    owCountWrong: r.orphanWalkCountWrong,
    /* THE ABSENT-GLOBAL PAIR, WHICH IS THE ONE ABSENCE THIS PROJECT'S FORCING FUNCTION CANNOT SURFACE AND
       WHICH THIS FILE WAS AGAIN THE CONSUMER THAT NEVER ASKED. §NO STUBS makes an unbuilt web API an HONEST
       absence whose forcing function is the page's own throw, and that argument rests on the page THROWING:
       a real bundle writes `if (window.X)`, the solver correctly declines to fork a read whose answer a real
       browser without X also gives, the fallback branch runs, and every endpoint and sink behind the true
       branch is unreachable with NOTHING ANYWHERE SAYING SO. It is the inverse of every other absence here —
       a crash names what to build, an honest throw names the component, an empty grep leaves a near miss,
       and this one produces a run that completes, emits, and looks healthy.
       THE CHAIN WAS BUILT TO THIS LINE AND STOPPED AT IT, exactly as the orphan pair above did and for the
       same reason: solver/absent.c counts the misses and classifies each against the three generated global
       vocabularies, solver/result.c emits `_absent`, extension/bridge.js asserts its shape and relays it onto
       every run record, and site.mjs writes `absentAsked`/`absentOwed` into every census row under a comment
       arguing at length that BOTH NUMBERS OR NEITHER is the whole point. MEASURED before this diff, with the
       orphan pair as the armed positive control: `git grep -l absentAsked` answered ONE path — the file that
       WRITES it, whose other occurrences are its own prose — against ten-odd for `orphansAsked`. Computed,
       asserted, relayed, stored, and read by nobody, on the column that answers the product loss.
       BOTH OR NEITHER, AND THE ORDER IS THE MECHANISM'S. `miss` is every read of the global object the hook
       was asked about; `owed` is the cut of those on a name one of the three standards puts on a global and
       this realm has none of. So `N>0` is the POSITIVE statement that this engine answered every standard
       name the page asked for, `N>M` is M reads of a component this build owes, and `0>0` is a census that
       was never reached — which is a scheduling result about the run and not a fact about the page. A
       numerator alone reproduces the ambiguity the pair exists to remove. */
    aask: r.absentAsked, aowed: r.absentOwed,
    /* AND WHICH NAMES, WHICH IS THE WORK QUEUE THE PAIR IS A NUMERATOR OF. It gets NO COLUMN and that is a
       decision rather than an omission — see the queue printed under the table, which states what a reader
       loses by it. Carried onto the measurement so the queue can be composed per pass and per site without
       re-reading the rows, and left as whatever site.mjs wrote: `[]` is a clean bill, `null` is a run that
       stated no census, and ABSENT is a pass that predates the field. Three facts, none defaulted into
       another — a `|| []` here would turn all three into the clean bill, which is the one this file most
       exists not to publish. */
    anames: r.absentOwedNames,
    /* THE HARD BAR PER ADDRESS, AND THE OPERAND IT IS COMPOSED OUT OF, which is the one column in this file
       that can tell a run that learned a GATED API SURFACE from one that counted a `<head>` back.
       §What-the-tool-produces' bar is "an address, a key or a value that NO PARSE of the served bytes can
       state, because it exists only at run time"; solver/endpoint.c composes `razorClass` per row out of
       `endpoint_razor_class_of`, extension/bridge.js relays it onto every engine-run record, and
       engine/build.mjs, extension/popup.js and testing/live-run.js all read it. This file was the consumer
       that never asked -- so the bar was scored per DOCUMENT and over no CORPUS, which is the write-with-no-
       reader half of the contract on the column the product is measured by.
       THE UNION IS NEVER ASSEMBLED HERE. A consumer unioning `doors` with `addressClass` by hand builds the
       figure CLAUDE.md DEMOTED, because `EPR_BEYOND` is what a MARKUP parse cannot reach and `fetch`, `xhr`
       and `module-import` are all in it. `epRazor` arrives already composed; `epAddr` is carried BESIDE it
       as the operand that cannot state the bar alone, never as an input to a sum made here.
       THEY ARE TWO FIELDS AND ONE OBSERVATION MORE THAN `epAddr` ALONE (CLAUDE.md §EVIDENCE-INFLATION):
       `epRazor` is DERIVED from `epAddr` and from the door's own bytes column, so a reader counting them as
       two independent signals is counting one and a half.
       LEFT AS WHATEVER site.mjs WROTE, which is three facts and no default: an OBJECT is a stated
       partition (`{}` being the partition of an EMPTY surface, a finding about the page), the string
       `(field-absent)` is an ARTIFACT older than the field, `null` is a pass whose runs carried no counters
       at all, and ABSENT is a pass predating the field entirely. A `|| {}` here would turn all four into
       the one this file most exists not to publish -- an engine that answered and proved nothing. */
    epRazor: r.endpointRazorClass, epAddr: r.endpointAddressClass,
    /* AND THE BOUND ON THE ONE POPULATION THE BAR CANNOT SIZE, LEFT AS WHATEVER site.mjs WROTE for `epRazor`'s
       reason exactly. It is NEVER summed with the bar: `may-rest-on` is a MAY — a pin is a fact about the PATH
       that composed the address and says nothing about whether this address read that source — so adding it to
       `runtime-only` composes an over-claim out of a floor. It is read AT THE SAME PASS as the bar below, not
       at its own best, because the pair is one observation of one run. */
    epWitness: r.endpointWitnessClass,
    /* AND THE TWO DATA DOORS' ASK LADDER, LEFT AS WHATEVER site.mjs WROTE for `epRazor`'s reason exactly —
       an OBJECT is a stated ladder, `null` is a pass whose runs carried no counters, and ABSENT is a pass
       predating the field. A `|| {}` would turn all three into the one state this file most exists not to
       publish. It is NOT an input to the hard bar and is never summed with it: the bar is about WHAT AN
       ADDRESS WAS, and this is about whether a door's machine was REACHED AT ALL. */
    epNetAsk: r.netDoorAsk,
    /* AND THE DOOR × WITNESS JOIN WITH ITS OWN THREE DENOMINATORS, which is what makes this file its first
       reader. site.mjs composed the join and NOTHING ANYWHERE READ IT — a write with no reader on the one
       statement neither margin can make, scored by a coordinator's grep one document at a time, which is the
       shape CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS names and the ninth time this corpus reader has been the
       consumer that never asked.
       WHAT IS READ IS THE PAIRING AND NOT THE MAGNITUDE. The join's counts are a union over every document at
       the origin and every analysis each held, so they are a DIFFERENT UNIT from `epRazor`'s — those are one
       engine run — and site.mjs's own banner says differencing them is a category error. A PAIRING is
       scope-invariant where a count is not: which door goes with which witness class holds in the union exactly
       where it holds per document, so the KEY SET is the finding and `doorWitnessRows` is what the counts are
       checkable against.
       LEFT AS WHATEVER site.mjs WROTE, for `epRazor`'s reason: ABSENT is a pass predating the field, and an
       object is a stated join whose `{}` is an empty surface. */
    epJoin: r.doorWitness, epJoinRows: r.doorWitnessRows,
    epJoinSnaps: r.doorWitnessSnaps, epJoinDocs: r.doorWitnessDocs,
    /* AND THE THREE ROWS THAT SAY WHETHER THE ENGINE RAN AT ALL AND WHETHER IT COULD TAKE A DELIVERY, left as
       whatever site.mjs wrote for `epRazor`'s reason. All three were write-with-no-reader in THIS file — the
       tenth, eleventh and twelfth time this corpus reader has been the consumer that never asked — and they
       are the three the endpoint columns above cannot be read without: a surface of 98 addresses and one of 190
       are the same engine on the same bytes, and what separates them is how much of the instance's span its own
       loop actually got.
       THEY ARE ONE QUESTION AT THREE GRAINS AND ARE NEVER SUMMED. `wallSpan` is the instance's own span split
       into loop and between-slices; `replyDoor` is what the reply seam was asked and answered; `deliverGuard`
       is how many flows could RECEIVE one. The first two are lifetime counts and `deliverGuard` is a GAUGE, so
       only the first two may be differenced across samples — which is why nothing below does that to the
       third. */
    span: r.wallSpan, rdoor: r.replyDoor, dguard: r.deliverGuard,
    /* AND WHICH TURNS SPENT THAT SPAN, which `span` hands off and cannot give — see site.mjs's
       `TURN_PHASE_ROWS` for the fork's third arm and for why `stepUs/steps` cannot read it. `tphase` is
       `stepUs`'s own partition into the step phase and everything in the turn that is NOT the step, plus the
       slice-bound fraction's numerator and the pair that says whether those turns offered a suspend point at
       all; `suo` is the arm each overrunning turn ended in, which is the only row here that names a COMPONENT.
       LEFT RAW for `span`'s reason: site.mjs already publishes both verdicts (`turnPhaseSumsWrong`,
       `stepUnitOverrunArmsWrong`) and a second copy of that arithmetic here is the one
       §AN-AUDITOR-DERIVES-THE-RULE forbids. What this file adds is the per-pass DISTRIBUTION, which is the
       cross-run fact no single census can state. All lifetime, all off the entry `countersFrom` names. */
    tphase: r.turnPhase, tphaseWrong: r.turnPhaseSumsWrong,
    suo: r.stepUnitOverruns, suoWrong: r.stepUnitOverrunArmsWrong,
    /* AND THE THIRD MEMBER OF THAT TRIPLE — which of an arm's overrunning turns offered NO suspend point.
       `suo` against `sur` says whether an arm can REST; this against `suo` says whether the thread was inside
       C that declares no step boundary, which is a step-machine conversion, against the page choosing a
       back-edge-free stretch, which no ordering reaches. Two components, two diffs, and neither the arm
       histogram nor the seamless SCALAR can tell them apart on its own. */
    suoSeam: r.stepUnitOverrunSeamlessArms, suoSeamWrong: r.stepUnitOverrunSeamlessArmsWrong,
    /* AND THE TWO DENOMINATORS THE FOUR SHARES ABOVE ARE OVER, which this file read off `r` directly in the
       `tlad` block and so could not reach from a measurement. `steps` is `sliceOverruns`' denominator and
       `stepUnitRuns` is `stepUnitOverruns`' — per arm, which is the whole of the producer's pair reading — and
       without them the overrun fraction and the arm's own rate are READERS WITH NO WRITER: they would resolve
       to `undefined`, print `-`, and void two of the four quantities on the cell while the cell still rendered.
       Both are LIFETIME and both come off the entry `countersFrom` names, the same one `tphase` does. */
    steps: r.steps, sur: r.stepUnitRuns,
    /* AND WHAT THE ORDER COST INSIDE THOSE TURNS, which the phase split hands off: `schedUs` bounds the PICK,
       and the preempt hook's own rescan is called FROM THE INTERPRETER and is charged to `sliceUs`, so a small
       `schedUs` says nothing about it. Left RAW for `span`'s reason — site.mjs publishes the verdict
       (`scanCostSumsWrong`) and a second copy of that arithmetic here is the one
       §AN-AUDITOR-DERIVES-THE-RULE forbids. All lifetime, and all off the entry `wfqFrom` names rather than
       `countersFrom`: these rows are published in the @WFQ block beside `preemptAsks` and the rival-miss
       partition, which is what makes the two identities hold at ONE instant and what makes a join to a `_cold`
       row a join of two walks at two moments. */
    scost: r.scanCost, scostWrong: r.scanCostSumsWrong,
    /* AND THE FORK PAIR, LEFT AS WHATEVER site.mjs WROTE. It is the population size for a refusal that is NOT
       YET BUILT, so what this file does with it is print the FRACTION and never a verdict: the numerator alone
       is meaningless (§Solver-half's pin MINT arms make most such branches unreachable at a hook) and the pair
       is carried off ONE census entry, so the containment the engine asserts is checkable here too. */
    fkPair: r.forkPinned,
    /* AND THE HOST'S LAST ROUND, BECAUSE THE SHARE ABOVE IS THE ENGINE'S HALF OF ONE FACT AND THIS IS THE
       DRIVER'S. A loop share says how little of its instance's span an engine got; it cannot say WHY, and the
       two readings that answer that — the host was servicing somebody's fetch, or the host was waiting —
       are this row's own partition. Left as whatever site.mjs wrote, which keeps FOUR states apart by
       presence (see its banner): `(field-absent)` is a probe that threw, `(relay-absent)` is bridge.js not
       loaded in the realm it ran in, `null` is no round completed, and an object is a round. */
    hround: r.hostRound,
    /* AND WHICH ARM OF `flow_step` TOOK THE STEP OF A MEMBER HOLDING A RUNNABLE TASK, which is the one
       reading the job-split rows above hand off and cannot give, and which was write-with-no-reader in THIS
       file. The four are LIFETIME counts off ONE `_wfq` object and `run-a-task` is `stepUnitRuns`' row off
       `countersFrom`, so BOTH INDICES travel with them: the identity
       `taskArmOlderLifetime + taskArmNoRowLifetime == run-a-task` is EQUALITY only where the two entries are
       the same one, and CONTAINMENT otherwise. Carried as five numbers and two indices rather than a verdict,
       because site.mjs already publishes the verdict (`taskArmPartitionDisagrees`) and a second copy of that
       arithmetic here would be the one §AN-AUDITOR-DERIVES-THE-RULE forbids — what this file adds is the
       per-pass DISTRIBUTION, which is the cross-run fact no single census can state.
       THE TWO SILENCES ARE SEPARATED HERE AND NOT AT THE CONSUMER, which is where this got it wrong once:
       the mapping always assigns this key, so an `'tlad' in m` test — the shape `span` above uses, which is
       sound because `span` is mapped RAW — can never be false and the `no-field` arm was DEAD, folding "this
       pass predates the four rows" into "no census of it observed a standing frontier". Those are a fact
       about the INSTRUMENT and a fact about the RUN and they take opposite work, so the three states are
       VALUES here: `null` is predates, the string is a stated absence, and an object is a reading. */
    tlad: (() => {
      const K = ['taskHeldDelivLifetime', 'taskHeldSeqLifetime',
                 'taskArmOlderLifetime', 'taskArmNoRowLifetime'];
      if (!K.some((k) => k in r)) return null;
      const sur = (r.stepUnitRuns && typeof r.stepUnitRuns === 'object'
                   && !Array.isArray(r.stepUnitRuns)) ? r.stepUnitRuns : null;
      const o = { deliv: r.taskHeldDelivLifetime, seq: r.taskHeldSeqLifetime,
                  older: r.taskArmOlderLifetime, noRow: r.taskArmNoRowLifetime,
                  task: sur && typeof sur['run-a-task'] === 'number' ? sur['run-a-task'] : null,
                  wfqFrom: typeof r.wfqFrom === 'number' ? r.wfqFrom : null,
                  cFrom: typeof r.countersFrom === 'number' ? r.countersFrom : null };
      return ['deliv', 'seq', 'older', 'noRow'].some((k) => typeof o[k] === 'number')
        ? o : 'no-live-frontier';
    })(),
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
    if (!bySig.has(at)) bySig.set(at, { sites: new Set(), reasons: new Set(), revs: new Set() });
    bySig.get(at).sites.add(r.id);
    bySig.get(at).reasons.add(rest.join(' :: '));
    /* AND THE REVISION THE ARTIFACT THAT SAW IT WAS BUILT FROM -- EVERY ONE, not the newest. See the staleness
       column at the ranking for why the set and not an extremum. A row from a build of site.mjs that predates
       the field carries no claim and contributes nothing, which the column reports as CANNOT ASK. */
    if (r.artifact && typeof r.artifact.builtFromHeadClaim === 'string' && r.artifact.builtFromHeadClaim)
      bySig.get(at).revs.add(r.artifact.builtFromHeadClaim);
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
  /* THE BRANCH/RUN READING AND ITS TWO HALVES, SPREAD LIKE EVERY OTHER COLUMN so a site that answers once
     cannot be read as one that answers the same way twice. `un` is solver/engine.c's `g_units_done` — a
     COMPLETED unit of work, HTML §8.1.4.4 step 3's boundary — and it is NOT `workDone`, which is
     `engine_work_done()`'s four-addend composition and which that function's own banner says is a bad
     numerator for anything. Two names, two accessors, two quantities, and this file reads the second.
     READ `fl/un` WITH `un` BESIDE IT AND NEVER ALONE, because the ratio moves for two reasons and only the
     pair says which: a site whose denominator holds while its numerator swings is a document that sometimes
     forks and sometimes does not, and a site whose denominator moves is a run that got a different distance.
     Measured on this corpus, gitlab is the first kind — `un` 39-48 across seven mirror passes while `flows`
     goes 2 to 193 — so its 116x is in the numerator, which is the quantity this column is about. */
  /* A SPREAD LIKE EVERY OTHER COLUMN, AND FOR A SHARPER REASON THAN THE REST: the dwell is an INPUT the
     drive chose, so a single value is a parameter and a RANGE is a controlled comparison. `spread` already
     refuses to fold an absent operand into a range, which is what keeps the six fatal rows out of it. */
  dwell: spread(ms, 'dwell'),
  un: spread(ms, 'units'), fpu: spread(ms, 'fpu'), gone: spread(ms, 'gone'),
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
  policy: ['pent', 'pcsp', 'ptt'].map((k) => spread(ms, k)).join('>'),
  /* `miss>owed` READ LEFT TO RIGHT IS HOW FAR THE GLOBAL-ABSENCE QUESTION GOT, in the order the mechanism
     travels — a read misses on the global object and is COUNTED, and it is then classified against the
     vocabularies and may be OWED. Two spreads and never a ratio, for the reason `ask>drv` gives: a ratio of
     two spreads is a number nobody measured. */
  absent: ['aask', 'aowed'].map((k) => spread(ms, k)).join('>'),
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
/* THE DWELL COLUMN SIZES ITSELF LIKE `terminal` DOES, for the reason stated there: `pad` TRUNCATES, and a
   truncated range is not a narrower range, it is a WRONG one -- `60000-240000` clipped to `60000-24` reads
   as a site measured at one budget. A width chosen today starts lying the first time a drive uses a longer one. */
const dwellW = Math.max('dwellMs'.length, ...table.map((t) => String(t.dwell).length)) + 2;
console.log('\n' + pad('site', 20) + pad('outcome', 20) + pad('abort/n', 8) + pad('fin/n', 7) +
  pad('dwellMs', dwellW) + pad('terminal', termW) +
  pad('ep', 8) + pad('sinks', 7) + pad('src>reach>taint>sup', 21) + pad('ask>drv', 13) +
  pad('sink>csp>tt', 16) + pad('miss>owed', 13) +
  pad('flows', 12) + pad('switches', 12) + pad('units', 9) + pad('fl/unit', 14) + pad('gone', 6) +
  pad('load', 10) + 'signature');
for (const t of table)
  console.log(pad(t.id, 20) + pad(t.outcome, 20) + pad(t.abortedPasses + '/' + t.n, 8) +
    pad(t.finishedPasses + '/' + t.n, 7) + pad(t.dwell, dwellW) + pad(t.terminal, termW) +
    pad(t.ep, 8) + pad(t.sk, 7) + pad(t.arrival, 21) +
    pad(t.orphans, 13) +
    pad(t.policy, 16) + pad(t.absent, 13) +
    pad(t.fl, 12) + pad(t.sw, 12) + pad(t.un, 9) + pad(t.fpu, 14) + pad(t.gone, 6) +
    pad(t.ld, 10) + (t.sigs[0] ? t.sigs[0].split(' :: ')[0] : '-'));

/* WOULD A LONGER DWELL CHANGE ANY OF IT -- the `finished` residual's own HOW-ITS-ABSENCE-WOULD-SHOW clause,
   answered from data that was on disk before the clause was written.
   WHY THIS IS A SECTION AND NOT A SECOND COLUMN. The `dwellMs` spread above says a site was measured across a
   RANGE; it cannot say what the runs at each END of that range ended as, and that pairing is the whole question.
   Printed per site, per dwell, in the producer's own words, so the reading is WITHIN ONE SITE: a terminal
   partition compared across two SITES is a comparison of two documents, which is the confound this file's own
   endpoint columns are careful about, and it would be worse here because the dwell is NOT randomly assigned --
   a drive lengthens it for whichever site it is currently interested in.
   THE SPAN IS OVER DWELLS AT WHICH THE SITE PRODUCED RUN RECORDS, WHICH IS THE REACHABILITY WITNESS AND NOT A
   REFINEMENT. A dwell at which `runsMine` is 0 is a pass where no engine ran, so it is not one end of a
   comparison -- it is the absence of one, and CLAUDE.md §THE-SAME-HOLE-SWALLOWS-A-PREDICTION is why: an
   absence is confirmed identically by a correct answer and by a path nobody took. MEASURED: excalidraw's two
   dwells are 60s with ZERO run records and 120s with one, so its apparent 2x span is ONE measurement, and this
   section says UNSPANNED for it rather than quoting a ratio over a cell that never ran.
   WHAT IT KILLS AND WHAT IT DOES NOT. Where the span is wide and `complete` is 0 at both ends, "it just needed
   a longer dwell" is refuted AT THAT RATIO and nothing else is: the other reason -- the run reached `finish`
   and something under it failed -- is untouched, and CLAUDE.md §AN-INVARIANT-OVER-A-GATED-OPERATION is why no
   stratification of an OUTCOME count will ever separate them. Where the span is 1 the honest word is UNSPANNED,
   which is this instrument saying it CANNOT ASK and never a site for which the dwell is known to be enough. */
{
  const strat = table.map((t) => {
    const byDwell = new Map();
    for (const m of t.measurements) {
      if (typeof m.dwell !== 'number') continue;
      const e = byDwell.get(m.dwell) || { rows: 0, runs: 0, outc: {} };
      e.rows++; e.runs += (typeof m.runs === 'number' ? m.runs : 0);
      for (const w of Object.keys(m.outc)) e.outc[w] = (e.outc[w] || 0) + m.outc[w];
      byDwell.set(m.dwell, e);
    }
    const cells = [...byDwell.entries()].sort((a, b) => a[0] - b[0]);
    const withRuns = cells.filter(([, e]) => e.runs > 0).map(([d]) => d);
    const noDwell = t.measurements.filter((m) => typeof m.dwell !== 'number').length;
    return { id: t.id, n: t.n, noDwell, cells,
             span: withRuns.length > 1 ? [withRuns[0], withRuns[withRuns.length - 1]] : null,
             completeAcross: cells.reduce((a, [, e]) => a + (e.outc.complete || 0), 0) };
  });
  const dwellCarried = strat.reduce((a, s) => a + (s.n - s.noDwell), 0);
  const dwellRows = strat.reduce((a, s) => a + s.n, 0);
  console.log('\nWOULD A LONGER DWELL CHANGE ANY OF IT — OVER ' + dwellCarried + '/' + dwellRows +
    ' ROW(S) CARRYING `dwellMs`, which site.mjs writes on every row it builds.');
  console.log('  A row without one is a `fatal` row that never drove the site (or predates the field): it is counted');
  console.log('  in `n/N` and NEVER folded into a span. The `×` words are the producer\'s own, summed over the');
  console.log('  rows at that dwell, and `(Nr/Mrun)` is how many rows and how many RUN RECORDS stand behind the cell.');
  for (const s of strat) {
    const cells = s.cells.map(([d, e]) => {
      const parts = RUN_OUTCOMES.filter((w) => e.outc[w]).map((w) => w + '×' + e.outc[w]);
      return (d / 1000) + 's ' + (parts.length ? parts.join(' ') : 'no-run') +
             ' (' + e.rows + 'r/' + e.runs + 'run)';
    }).join(' | ');
    const verdict = s.span
      ? 'SPANNED ' + (s.span[0] / 1000) + 's..' + (s.span[1] / 1000) + 's (' +
        (s.span[1] / s.span[0]).toFixed(1) + 'x), complete×' + s.completeAcross + ' across it'
      : 'UNSPANNED (one dwell with runs — this section cannot ask)';
    console.log('  ' + pad(s.id, 14) + pad(verdict, 62) + (cells || 'no dwell carried'));
  }
}

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

/* AND THE THIRD, FOR THE @S POLICY ENVELOPE. Asked separately for the reason the two above are separate --
   three censuses, three commits, and a pass can carry any of them -- and it matters MOST here, because this
   column's `-` and its `0>0` are the two readings a security report cannot survive confusing. `-` is this
   file unable to ask whether a finding is policy-dead; `0>0` over a nonzero `sink` is the positive statement
   that every detected vector SURVIVES the page's own policy, which is the finding. Reading the first as the
   second publishes a corpus of clean XSS verdicts that no oracle has contradicted, which is exactly the
   state CLAUDE.md records for this field and exactly what the control pair was built to end. */
const pPredates = passes.filter((p) => p.policy === 'predates').map((p) => p.label);
const pCarried = passes.filter((p) => p.policy === 'carried').map((p) => p.label);
if (pPredates.length)
  console.log('\n*** THE `sink>csp>tt` COLUMN IS OVER ' + pCarried.length + ' OF ' + passes.length +
    ' PASS(ES) — ' + pPredates.join(', ') + ' predate(s) the @S policy envelope entirely (the rows carry no ' +
    '`policyEnvelope` field), so a `-` there is this instrument being unable to ask, NOT a corpus whose ' +
    'findings all survive their pages\' policies. A `-` on a CARRIED pass is a site no document of which ' +
    'was ever answered, which is a third fact again. ***');

/* AND THE FOURTH, FOR THE ABSENT-GLOBAL PAIR, WITH A SECOND LINE THE OTHER THREE DO NOT NEED. The predates
   shout is the same argument as theirs. The FATAL one is not a variant of it: `absentFatal` means the member
   set solver/absent.c's composer declares in THIS TREE is not the set the census on that row carries, which
   is the artifact that answered and the source that was read being two different composers — so the column
   is dark because the PAIR moved, not because the pass is old, and the two prescribe opposite work (rebuild
   and reinstall the artifact, or read the row at the revision it was stamped at, against wait for a newer
   pass). It is shouted with the row's own message because that message names both member counts and the
   members each side has that the other does not, which is the whole of what the next reader needs and is not
   recoverable from a `-`. */
const aPredates = passes.filter((p) => p.absent === 'predates').map((p) => p.label);
const aCarried = passes.filter((p) => p.absent === 'carried').map((p) => p.label);
const aFatal = passes.filter((p) => p.absent === 'fatal').map((p) => p.label);
if (aPredates.length)
  console.log('\n*** THE `miss>owed` COLUMN IS OVER ' + aCarried.length + ' OF ' + passes.length +
    ' PASS(ES) — ' + aPredates.join(', ') + ' predate(s) the absent-global census entirely (the rows carry ' +
    'no `absentAsked` field), so a `-` there is this instrument being unable to ask, NOT a page that read ' +
    'no absent global and NOT an engine that answered every name one was asked for. ***');
if (aFatal.length)
  console.log('\n*** THE `miss>owed` COLUMN IS DARK ON ' + aFatal.join(', ') + ' BECAUSE THE ENGINE\'S ' +
    'CENSUS KEYS MOVED, NOT BECAUSE THE PASS IS OLD — the members solver/absent.c declares in this tree ' +
    'are not the members the census on that row carries, so the artifact that answered and the source ' +
    'that was read are two different composers. This is a pair to bring back into step (rebuild and ' +
    'reinstall, or read the row at its stamped revision) rather than a measurement to wait for. ***\n' +
    passes.filter((p) => p.absent === 'fatal')
      .map((p) => '    ' + p.label + ': ' +
        (p.rows.filter((r) => r.absentFatal).map((r) => r.id + ' — ' + r.absentFatal)[0] || '(no message)'))
      .join('\n'));

/* THE CRASH-WITHOUT-REASON FLAG, WHICH site.mjs HAS WRITTEN SINCE IT WAS BUILT AND NOTHING HAS EVER READ.
   Measured before adding this: `grep -rn 'crashWithoutReason' --include=*.mjs --include=*.js testing/ engine/
   extension/` answers exactly TWO lines, both in site.mjs -- the write at its composer and the paragraph
   explaining what it is for. That is the write-with-no-reader half of CLAUDE.md's defaulted-field defect, in
   the instrument that file's own banner says has been "the consumer that never asked for the field written to
   answer its own ambiguity" seven times, and this is the eighth made good rather than a ninth.
   IT IS NOT A COLUMN, AND THAT IS THE WHOLE OF WHY IT IS HERE INSTEAD. A column is a property OF A SITE that a
   reader compares across sites; this flag is a statement about THIS INSTRUMENT -- site.mjs's own words are
   that a crashed run with no reason "means one of the two producers is not being read" -- so a `true` is not
   a page that crashed mysteriously, it is a channel of this file that has stopped answering. Printed as a
   column it would read as a site's misfortune and be compared against sites that have none; printed as a
   shout it says the one thing it means. Its history is exactly that failure: site.mjs records that the flag
   read a boolean bridge.js had DELETED, so it "could never raise", and separately that `@E` was left out of
   its reason set, so every run killed by a CHECK -- the loudest thing the engine can say -- read as a crash
   nobody could explain. A flag with that history and no reader is a flag whose next silent breakage nothing
   would catch.
   AND THE THREE STATES ARE BANDED LIKE EVERY SIBLING ABOVE, for the same reason: a row that PREDATES the
   field carries no such key, and `false` on such a row is this file unable to ask rather than a run whose
   reason was found. Summing those two is the absent-versus-zero pair CLAUDE.md refuses, and it would read in
   the flattering direction -- as a corpus in which every crash is explained. */
const cwrAll = passes.flatMap((p) => p.rows);
const cwrPredates = cwrAll.filter((r) => !('crashWithoutReason' in r));
const cwrRaised = cwrAll.filter((r) => r.crashWithoutReason === true);
if (cwrPredates.length)
  console.log('\n*** THE CRASH-WITHOUT-REASON CHECK IS OVER ' + (cwrAll.length - cwrPredates.length) +
    ' OF ' + cwrAll.length + ' ROW(S) — ' + cwrPredates.length + ' carry no `crashWithoutReason` key at ' +
    'all, so their silence is this instrument being unable to ask and NOT a crash whose reason some ' +
    'producer gave. ***');
if (cwrRaised.length)
  console.log('\n*** ' + cwrRaised.length + ' ROW(S) CRASHED WITH NO REASON FROM ANY PRODUCER, WHICH IS A ' +
    'CLAIM ABOUT THIS INSTRUMENT AND NOT ABOUT THOSE PAGES — site.mjs: a crashed run with no reason "means ' +
    'one of the two producers is not being read". The row has a crashed run and NOTHING in its page errors, ' +
    'its `@WHY` lines or its `@E` lines, so a channel this file reads has stopped answering rather than a ' +
    'page having failed inexplicably. Open the row\'s own `logFile` before reading any counter off it. ***\n' +
    cwrRaised.map((r) => '    ' + (r.pass || '?') + '/' + r.id + '  logFile=' + (r.logFile || '(unnamed)') +
      '  atE=' + ((r.atE && r.atE.length) ? r.atE.length : 0) + ' @E line(s) held on the row itself')
      .join('\n'));

/* WHETHER A REACH SHORTFALL IS THE ORDER'S OR THE THREAD'S, COMPOSED RATHER THAN LEFT AS A QUOTIENT. This is
   the retirement condition site.mjs stated when it began carrying these rows, and it is MET here. The rows
   alone are not the answer: solver/flow.c's `never_picked` block names THREE states behind one starved tail,
   says two of them take DIFFERENT weight changes and the third takes NONE AT ALL, and records the same
   figures having produced the throughput-read-as-ordering report TWICE. A reader handed `neverPicked` and
   `picksLifetime` has to compose the verdict, and composing it is exactly where it went wrong before.
   THE DECISIVE ROW IS `starvedPicksIdle` AND THE GAUGES ARE NOT. flow.c: it is the only instrument here that
   asks whether a pick ever PASSED OVER a member, which is what "is the order wrong" actually asks. The
   gauges say a tied tail EXISTS at some instant; this says a dispatch CHOSE against it. `neverPickedGap` is
   the second decisive one and points the OTHER WAY from how it reads — a gap of zero means the best
   never-picked member stands AT the top, so nothing is ranked ahead of it and the order is ready to serve it
   the moment a dispatch exists. flow.c's words: an order with nothing to answer for.
   THE BOUNDARIES BELOW ARE PRESENTATION AND NOT BOUNDS (§NO BOUNDS is about work, not about a label). Every
   quantity the verdict is derived from is PRINTED beside it, so a reader who disagrees with where the line
   falls can read the numbers and overrule the word — which is the only form in which a composed verdict is
   better than the quotient it replaces. */
const THR_SWEEP = 2.0;   /* T/P at or below this is one dispatch per member reached: nothing swept twice */
const THR_HOG = 0.5;     /* picksMax at or above this share of T is one member holding most of the thread */
const ordState = (m) => {
  const T = m.picksLifetime, M = m.wfqMembers, np = m.neverPicked, pm = m.picksMax;
  const si = m.starvedPicksIdle, gap = m.neverPickedGap;
  if ([T, M, np, pm].some((x) => typeof x !== 'number')) return { w: '-', why: 'rows absent on this pass' };
  const P = M - np;
  if (typeof si === 'number' && si > 0)
    return { w: 'ORDER', why: 'a pick PASSED OVER a member ' + si + ' time(s) of ' + T };
  /* …AND THE TIE, WHICH THE ROW ABOVE IS STRUCTURALLY BLIND TO. `starvedPicks` is raised only where the pick
     DISPLACES, so starvation AT EQUALITY reads zero there: a never-run member at EXACTLY the incumbent's
     weight loses every tie to incumbency and no pick ever ranks a served member strictly above it. This is
     the complement solver/flow.c raises for that reason, and the IDLE subset is the decisive one — a
     retention MID-PROGRAM is §Attention's value yield working as specified, and a retention BETWEEN UNITS
     against a level never-run member is the same event with nothing left to justify it. */
  const phi = m.plateauHeldIdle, ph = m.plateauHeld, pa = m.plateauAsked;
  if (typeof phi === 'number' && phi > 0)
    return { w: 'ORDER(tie)', why: 'the incumbent KEPT the thread BETWEEN UNITS against a level never-run '
      + 'member ' + phi + ' time(s) — of ' + ph + ' retention(s) over ' + pa + ' scan(s) that could ask' };
  if (typeof ph === 'number' && ph > 0)
    return { w: 'PLATEAU', why: ph + ' retention(s) of ' + pa + ' askable scan(s), none of them between '
      + 'units — a value yield doing what it is specified to do, so no ordering claim' };
  if (typeof pm === 'number' && T > 0 && pm >= THR_HOG * T)
    return { w: 'MONOPOLIZER', why: 'picksMax ' + pm + ' of ' + T + ' dispatches on ONE member' };
  if (P <= 0) return { w: 'UNREACHED', why: 'no member was ever picked: ' + T + ' dispatch(es)' };
  const tp = T / P;
  if (tp <= THR_SWEEP)
    return { w: 'THROUGHPUT', why: 'T/P ' + tp.toFixed(2) + ' (' + T + '/' + P + ') — the frontier grows '
      + 'faster than one thread serves it, and NO weight change reaches that' };
  return { w: 'COHORT', why: 'T/P ' + tp.toFixed(2) + ' (' + T + '/' + P + ') with picksMax ' + pm
    + ' — a reachable cohort swept while the tail waits, which a TERM must answer for'
    /* …AND WHICH TERM IS THE QUESTION THE PLATEAU ROWS ANSWER, SO THEIR ABSENCE IS STATED RATHER THAN
       SWALLOWED. A COHORT reached by the RATIO alone is a verdict that a term is owed and is SILENT about
       whether the event is a DISPLACEMENT or a TIE — which are the two sides of this one state and take two
       different terms. A pass whose artifact predates `plateauHeld` cannot be asked, and an unasked question
       renders identically to an answered one unless the row says so. */
    + (typeof ph === 'number' ? ''
      : ' [plateau rows ABSENT on this pass, so DISPLACEMENT vs TIE was not asked — re-run to split it]') };
};
const ordRows = table.map((t) => ({ id: t.id, st: t.measurements.map(ordState) }))
  .filter((r) => r.st.some((s) => s.w !== '-'));
if (ordRows.length) {
  console.log('\n*** ORDER OR THREAD — the three states solver/flow.c says take DIFFERENT work, composed per '
    + 'pass. The decisive rows are `starvedPicksIdle` for a PASS-OVER and `plateauHeldIdle` for a TIE, and '
    + 'the first is blind to the second by construction (it is raised only where the pick DISPLACES). Every '
    + 'ratio is printed so the word can be overruled. A THROUGHPUT verdict means no weight change reaches '
    + 'it; an ORDER or ORDER(tie) one means a term does. ***');
  for (const r of ordRows)
    for (const s of r.st)
      console.log('    ' + pad(r.id, 16) + pad(s.w, 14) + s.why);
  /* …AND THE CLOCK, PRINTED IN THE SAME SECTION BECAUSE IT IS THE SAME QUESTION ONE BOUNDARY DOWN: the
     ORDER decides which member runs, and the CLOCK decides whether the member that runs can make a timer
     due. A run whose chain arrivals are large and whose fired count is ZERO is a clock that was asked and
     declined every time — which on this corpus is the whole difference between a drive that composes
     addresses no parse can state and one that composes none. */
  const clk = table.map((t) => t.measurements.map((m) => [t.id, m.pass, m.clockChain, m.clockTimerAsk,
    m.clockIdleAsk, m.clockTimerFired])).flat()
    .filter((v) => typeof v[2] === 'number' || typeof v[5] === 'number');
  for (const [id, pass, chain, tAsk, iAsk, fired] of clk)
    console.log('    ' + pad(id, 16) + pad('clock', 14)
      + 'chain ' + chain + ' -> timer-rung ' + tAsk + ' -> idle-rung ' + iAsk
      + ' | fired ' + fired
      + (typeof chain === 'number' && chain > 0 && fired === 0
        ? '  <-- ASKED AND NEVER FIRED: the rung was reached and declined every descent'
        : typeof chain !== 'number' ? '  [chain arrivals ABSENT on this pass — not carried]' : ''));
  const vis = table.map((t) => t.measurements.map((m) => [m.visMax, m.visZero, m.wfqMembers]))
    .flat().filter((v) => typeof v[0] === 'number');
  const dead = vis.filter((v) => v[0] === 0);
  if (dead.length)
    console.log('    AND ' + dead.length + ' of ' + vis.length + ' pass(es) read `visMax 0` — NOT ONE MEMBER '
      + 'REACHED THE END OF A PROGRAM, so not one queued job can have run whatever the switch and fork '
      + 'counts say. That is the row that separates "served fairly" from "finishing nothing".');
}

/* AND THE SHOUT FOR THE NAMES, WHICH IS NOT DECORATION ON THE `miss>owed` ONE ABOVE. Those two report on
   DIFFERENT FIELDS that entered site.mjs at different commits, so a pass can carry the pair and not the
   queue; a single shout covering both would report one instrument's silence as the other's answer, which is
   the reason every triage in this file is asked separately. It matters here because the queue's own empty
   state is a CLEAN BILL — "no site owed a name a standard puts on a global" — and a pass that could not be
   asked renders identically to it. */
const nPredates = passes.filter((p) => p.absentNames === 'predates').map((p) => p.label);
const nCarried = passes.filter((p) => p.absentNames === 'carried').map((p) => p.label);
if (nPredates.length)
  console.log('\n*** THE OWED-GLOBALS QUEUE BELOW IS OVER ' + nCarried.length + ' OF ' + passes.length +
    ' PASS(ES) — ' + nPredates.join(', ') + ' predate(s) the NAMES (their rows carry `absentAsked` and no ' +
    '`absentOwedNames`), so an EMPTY queue over those passes is this instrument unable to ask, NOT a corpus ' +
    'that owed nothing. The `miss>owed` column above is over a DIFFERENT set of passes and the two numbers ' +
    'are not each other. ***');

/* THE OTHER WORK QUEUE, AND IT IS THE ONE §NO-STUBS SAYS NO CRASH WILL EVER PRODUCE. A `DFAIL` names what to
   build and the queue below prints it; an absent global names what to build and NOTHING THROWS — the page
   writes `if (window.X)`, the read is correctly decided false, the fallback runs, and every endpoint and sink
   behind the true branch is unreachable in silence. So this is the one queue in this file whose entries have
   no signature, no stack and no abort, and until this diff the corpus's own census carried the COUNT of them
   and threw the list away one property access from where it arrived.
   IT IS A SECTION AND NOT A COLUMN, WHICH IS A DECISION AND NOT AN OMISSION. `pad` TRUNCATES, and this file
   already records what that costs on a partition (`crashed x6 partial x3` clipped to `crashed x6 par` reads
   as a site that only ever crashed). A truncated NAME is strictly worse than a truncated partition: it is a
   spelling that exists nowhere, so a reader greps the tree for it, gets zero, and reads that as the component
   being absent from the standard rather than from their column — an instrument manufacturing the perfect
   silence CLAUDE.md teaches readers to distrust. And a list has no RANGE, so `spread` cannot express it
   across passes at all; `terminal` and `cold` are already printed as sequences for that same reason.
   WHAT A READER LOSES BY THAT, SAID PLAINLY: scanning ONE site's row in the table, they cannot see which
   names that site owed — the row gives them `miss>owed` and sends them here. The mitigation is that this
   queue prints the SITES under each name, so the mapping is recoverable in the other direction and the
   question "which sites owe this" — which is the one a work queue is actually read for, because a name owed
   by six sites generalises and one owed by one may not — is answered directly rather than by eye.
   RANKED BY SITES AND NOT BY READS, BECAUSE READS ARE NOT AVAILABLE HERE. NAMED RESIDUAL — CORRECT AND
   NARROWER. WHAT IS NOT COVERED: how MANY times each name was read. solver/absent.c keys a per-entry
   histogram under every owed name and testing/absent_census.js walks those buckets — it sums them to check
   the total — and then returns the KEYS alone, so the per-name counts are computed on every census of every
   run and reach no consumer. WHAT THE NEXT DIFF BUILDS: `absentPair` returning that per-name total beside
   `names`, which both drivers already call and neither would have to learn a new shape for. HOW ITS ABSENCE
   WOULD SHOW: a name read once to feature-detect and a name read in a loop rank identically here, so a queue
   whose head and tail are one site each states no order at all.
   THE DENOMINATOR IS PRINTED WITH THE QUEUE and not left to the reader: a list of names over an unstated
   number of sites is a count whose fraction nobody can take. */
{
  const owed = new Map();                       // name -> Set of site ids
  let asked = 0, clean = 0, unstated = 0;
  /* THE THREE STATES ARE READ OFF THE VALUE AND NOT OFF THE KEY, WHICH IS THE ONE THING A SCRATCH HARNESS
     WILL NOT TEACH YOU. The triage above asks `'absentOwedNames' in r` of the RAW jsonl row, where the key
     really is absent on an old pass; by the time a row reaches here it has been through the measurement
     mapper, which writes `anames:` UNCONDITIONALLY — so the key is always present and only its VALUE says
     which state this is. An `in` test here is true for all three, and the first thing it reaches is
     `undefined.length`. Found by running the instrument rather than by the harness that exercised this block
     against a hand-built `{}` no mapper produces: the harness modelled the shape I meant instead of the shape
     the file makes, which is the mis-addressed question arriving inside my own control. */
  for (const t of table) for (const m of t.measurements) {
    if (m.anames === undefined) continue;       // a pass that predates the field: shouted above, never counted
    if (m.anames === null) { unstated++; continue; }   // a run that stated no census — not a clean bill
    asked++;
    if (!m.anames.length) clean++;
    for (const n of m.anames) {
      if (!owed.has(n)) owed.set(n, new Set());
      owed.get(n).add(t.id);
    }
  }
  if (asked || unstated) {
    console.log('\n=== absent globals owed, ranked by sites that owed them ===');
    console.log(owed.size + ' distinct name(s) over ' + asked + ' measurement(s) that stated the census (' +
                clean + ' of those owed nothing, which is the clean bill; ' + unstated +
                ' more stated none at all, which is not one)');
    /* SORTED BY SITES DESCENDING AND THEN BY NAME, so two runs of one corpus print the same order — the
       insertion order here is the order sites happened to be walked, which is not a fact about the corpus. */
    for (const [n, sites] of [...owed.entries()].sort((a, b) => b[1].size - a[1].size || (a[0] < b[0] ? -1 : 1)))
      console.log(`${sites.size}  ${n}\n     sites: ${[...sites].sort().join(', ')}`);
    if (!owed.size)
      console.log('    (none — over the measurements that stated the census, every standard name a document ' +
                  'read was one this engine answers)');
  }
}

/* THE WORK QUEUE. A DFAIL's reason names what to build, so it is printed rather than summarised -- but only
   the head of it, because one 1169-character reason per row buries the RANKING, which is the thing this
   section exists to show. `-v` prints them whole. */
const verbose = process.env.SIGS === 'full';
/* WHETHER THE ABORT A SIGNATURE NAMES WOULD STILL BE THE SAME ABORT, BESIDE IT, BECAUSE A RANKED FINDING LIST
   DECAYS FROM ITS TOP AND THE TOP IS WHAT GETS DISPATCHED. The ordering here is staler than the rows: the
   highest entries are the ones lanes were sent at, so they are the ones most likely to have a landed fix, and
   NOTHING IN THIS ARTIFACT SAID SO -- each coordinate is a true fact about the artifact that measured it, so
   every one checks out individually while the ORDER is about a tree nobody has had for hours.
   THE VERDICT IS THE CONSTRUCT'S AND NEVER THE COORDINATE'S, AND THE COLUMN THAT GRADED THE COORDINATE IS GONE
   RATHER THAN KEPT BESIDE IT. `engine/abortlive.mjs` keys on the `cond` the macro stringified and the message
   literal it compiled in, reassembled across the line wraps a C reason is written with, out of the text with the
   COMMENTS STRIPPED -- so it ignores the `:line` entirely, which is the whole point: a census line number drifts
   the moment anybody edits above the assert, and it drifts in BOTH directions, so a deletion above it leaves the
   recorded line RESOLVING, to code that is real, current and about something else. Measured on this corpus:
   `engine.c:3275` is the line every gitpod row names and the construct stands seven lines earlier.
   IT ANSWERS THREE VERDICTS, WHICH IS WHAT THE PATH COLUMN COULD NOT. PRESENT means this cond and this message
   both still compile in. ABSENT means neither does. CHANGED is the one neither hand method has and is the one
   this corpus most needs: a repair that fixes a WRONG INVARIANT removes nothing, so the assert stands with a
   different predicate or a reworded refusal under it -- a reader who greps the old message gets 0 and files it
   RETIRED, a reader who greps the cond gets a hit and files it LIVE, and both are wrong about the same site.
   MEASURED over this corpus's eleven distinct records: 5 PRESENT, 4 CHANGED, 1 ABSENT, 1 unaskable.
   AND THE REVISIONS ARE PRINTED RATHER THAN DIFFED, which is the half of the retired column worth keeping. The
   raw fact a reader needs is WHICH artifact saw a signature, because a row measured before a landed repair is a
   row about a program nobody runs: both artifacts that produced this corpus's `no navigable of THIS TIMELINE`
   abort carry `navigable_seed_scripts` BEFORE `window_proxy_navigate`, which is the inversion `042c3362` landed
   the fix for, and the next artifact measured answered 0 of 10.
   RETIREMENT -- MET BY A CONSTRUCTION, AND THE RETIRED WORDING IS KEPT BELOW THE VERDICT BECAUSE A READER WHO
   RE-DERIVES THE PATH ARGUMENT WILL WRITE THE PATH COLUMN AGAIN. It read: "this column goes when a signature's
   identity is carried as the CONSTRUCT its abort is written in rather than as a `file:line`, because a drifted
   coordinate is then unspellable and there is nothing to grade the staleness of." That is this, so the column
   goes with it -- a path diff standing beside a construct verdict is the legacy fallback §A-superseded-system-is-
   DELETED forbids, and its own reasoning was already a confession: `path unmoved` is NECESSARY AND NOT
   SUFFICIENT, which is to say it is not an answer. Its load-bearing measurement is the one restated above and is
   why the revision SET and never an extremum reaches this line: the newest artifact in a corpus is usually one
   that never saw the signature at all, so an extremum answers UNMOVED for a file whose abort is retired, in the
   direction that dispatches a lane at work already done.
   RETIREMENT: this column goes when a signature's verdict carries a REACHABILITY WITNESS beside it -- the census
   row that is nonzero when the assert's own component was executed in the run being graded -- because PRESENT is
   a fact about a LINE until something says the run reached it, and §AN-ABSENT-CRASH-IS-NOT-A-CORRECT-VALUE's
   mirror is that a present assert is not a live blocker. MEASURED ABSENT before being written, and the command is keyed on a
   DEFINITION and never on the bare token, because a measured-absent clause that greps a bare name is satisfied by its
   own text the moment the name is written into it -- the bare form of this grep answers 1 and the file it answers is
   this one: `grep -cE 'const (sigReachWitness|witnessRowFor) *='` over this file answers 0, against the same shape over
   `sigConstructVerdict` answering 1 as the armed control and `const zzNope *=` answering 0. */
const _sigVerdict = new Map();
const sigConstructVerdict = (at, reasonsJoined, revs) => {
  const why = whyOf.get(at + ' :: ' + reasonsJoined);
  /* A KEY WITH NO PAIR BEHIND IT IS REPORTED AS UNASKABLE AND NEVER GUESSED AT. The memo is filled by the same
     parse that built the key, so a miss means a signature reached this ranking through a path that did not go
     through `signatures()` -- which is a finding about this file and not about the tree. */
  if (!why) return 'VERDICT: cannot ask — this signature key carries no cond/reason pair from the parse';
  const k = at + '\u0000' + reasonsJoined;
  if (!_sigVerdict.has(k)) _sigVerdict.set(k, condVerdict(why));
  const v = _sigVerdict.get(k);
  const seen = revs && revs.size ? '  [seen at ' + [...revs].map((r) => r.slice(0, 8)).join(' ') + ']' : '';
  const head =
    v.verdict === VERDICT_PRESENT ? 'VERDICT: PRESENT at the tip BY CONSTRUCT — ' + v.why +
      '. This is the assert that fired and it is still written; whether it would still FIRE is a different fact ' +
      'and only a run produces it'
    : v.verdict === VERDICT_CHANGED ? 'VERDICT: CHANGED at the tip BY CONSTRUCT — ' + v.why +
      '. Re-derive before dispatching: a widened predicate NARROWS the population an assert accuses and does ' +
      'not remove it, and a reworded refusal is a different refusal'
    : v.verdict === VERDICT_ABSENT ? 'VERDICT: ABSENT at the tip BY CONSTRUCT — ' + v.why +
      '. This abort is RETIRED; a row naming it is a row about a program nobody runs'
    : 'VERDICT: cannot ask — ' + v.why;
  return head + seen;
};
/* THE CHECKER'S OWN CONTROL SPEAKS BEFORE ONE VERDICT IS PUBLISHED, AND IT THROWS RATHER THAN WARNS. A liveness
   verdict nobody calibrated is a verdict about the probe, and the cheap failure is silent in the direction that
   costs most: every record would read ABSENT and every retired-looking row would be a row somebody stops working
   on. `armed()` runs eight synthetic cases that separate PRESENT, CHANGED and ABSENT -- synthetic rather than
   taken from this tree, because a control keyed on a real file's content is a coordinate that rots and then
   reads as this checker breaking on the day somebody repairs that file. */
const abortliveCases = abortliveArmed();
console.log('\n=== distinct crash signatures, ranked by sites hit ===');
console.log(`# each signature's verdict is its CONSTRUCT asked of the working tree by engine/abortlive.mjs, whose ` +
            `control separates\n#   PRESENT / CHANGED / ABSENT on ${abortliveCases} synthetic cases before any ` +
            `verdict here is published. The \`:line\` is NOT the key: it drifts.`);
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
              why.map((w) => '\n     ' + w).join('') + `\n     sites: ${sites}` +
              /* ONE VERDICT PER DISTINCT REASON AND NEVER ONE PER SIGNATURE, because the reasons under one line
                 are the OPERANDS that reached the gap and a repair can retire one of them and keep the others.
                 Measured on this corpus: `engine.c`'s C-builtin-fork line carries two reasons that differ only
                 in their remedy tail, and at the tip ONE of the two survives -- so a single verdict for the line
                 would have called a merged-away wording live or a standing one retired. */
              [...e.reasons].map((w) => '\n     ' + sigConstructVerdict(at, w, e.revs)).join(''));
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
/* WHERE THE LEARNED ADDRESSES CAME FROM IS ASKED BY `reach.mjs`, NOT HERE, AND THE COLUMN THAT USED TO STAND
   AT THIS POINT IS DELETED RATHER THAN LEFT BESIDE IT. It compared a learned address STRIPPED TO ITS PATHNAME
   against markup attribute values taken RAW, read only `src`/`href`/`img src` case-sensitively, and resolved
   nothing — so an absolutely-specified resource could never match, a `srcSet` list was invisible, a
   document-relative `./chunk.js` never matched, and a percent-encoded learned spelling never met its raw
   attribute. Every one of those misses INFLATES the complement, which was the column a reader was told to
   trust: it published 122 addresses as named-nowhere-in-markup where the sound answer over the same rows and
   the same mirror is 0. A second, correct implementation standing next to it would be the dual-system rot —
   and the broken one is the one already in everybody's fingers, so it goes.
   THE CENSUS TABLE'S CONTRACT IS THE RUN OUTCOME AND THE ABORT QUEUE; provenance is a different axis over
   different inputs (the TRACKED mirror rather than these untracked rows) and it lives in its own file:
       node reach.mjs <the same census files> */
/* CLAUDE.md §What-the-tool-produces' HARD BAR, OVER THE CORPUS, WHICH NOTHING HAS EVER SCORED. That bar is
   "an address, a key or a value that NO PARSE of the served bytes can state, because it exists only at run
   time". solver/endpoint.c composes it PER ROW, extension/bridge.js relays the partition onto every
   engine-run record, and three consumers read it -- all of them at the grain of ONE DOCUMENT. The retirement
   condition CLAUDE.md states for that record names exactly the construction missing: a corpus row saying,
   with its RUN COUNT and its SPREAD, how many of a drive's addresses cleared the bar. This is that row.
   IT IS A FLOOR, A DIAGNOSTIC, AND NEVER A TARGET, which is the first thing a reader of it must hold.
   `runtime-only: 0` against a nonzero emitted surface is this engine's REFUSAL TO CLAIM the bar on that
   document -- solver/endpoint.h enumerates the three populations `concrete` hides, of which a source a flow
   PINNED AND RE-READ really is past every parse and cannot be said so by any field -- so a low number is an
   under-claim published as a floor and not a weak capability. Optimising toward this column is optimising
   toward a measurement, which §netdiff already refuses by name for `--unused`.
   THE UNION IS NOT COMPOSED HERE AND MAY NOT BE. The engine composes it at the one line where both operands
   are in hand; a consumer assembling one out of `doors` and `addressClass` builds the figure CLAUDE.md
   DEMOTED, since `EPR_BEYOND` is what a MARKUP parse cannot reach and `fetch`, `xhr` and `module-import` are
   all in it. The `unproven` margin beside the `runtime-only` one is the SAME partition's other bucket and is
   not a second observation of anything (CLAUDE.md §EVIDENCE-INFLATION): `razorClass` is DERIVED from
   `addressClass` and the door's own bytes column, so these two numbers are one fact about a surface read two
   ways round, and a reader counting them as corroborating signals is counting one.
   FOUR STATES AND NONE FOLDED, because a zero on this column has four readings that take different work and
   three of them are not about the page at all. `no-field` is a pass written by a site.mjs predating the
   field; `artifact-predates` is a run record that carried counters and not this key, which is a fact about
   the BUILD and prescribes a rebuild rather than a wait; `no-counters` is a pass whose runs carried none at
   all, which bridge.js writes deliberately for a crashed run "because seven zeroes read as a run that
   explored nothing"; and `{}` is a stated partition of an EMPTY SURFACE, which is the only one of the four
   that is a finding about the page. A `|| {}` anywhere above would publish all four as the last.
   THE PER-SITE SEQUENCE IS PRINTED AND NOT ONLY A TOTAL, which is `cold`'s rule in testing/live-run.js and
   holds for its reason: a RANGE over bucket names is not a quantity, and one run of a live site is not a
   measurement -- so the passes are listed in order, the spread of the cleared count is stated beside them,
   and the run count is the denominator of both. A site answering once cannot then be read as one answering
   the same way twice.
   THE PAIR IS TAKEN FROM ONE PASS PER SITE AND THE TOTALS SAY SO. `endpointsTotalBestPass` beside it already
   sums a per-site BEST, and the question a headline answers is what the tool CAN prove about the page -- so
   the site's pass with the most `runtime-only` rows is chosen and BOTH buckets are read off THAT pass. Taking
   each margin from whichever pass maximised it would be two margins of two different observations summed into
   one line, which is the two-moments defect this file has already had to publish two indices to refuse.
   IT IS A SECTION AND NOT A COLUMN, for the owed-globals queue's reason: `pad` TRUNCATES, and a truncated
   histogram is a number whose denominator the reader cannot see. */
const RAZOR_RUNTIME = 'runtime-only', RAZOR_UNPROVEN = 'unproven';
const rzPredates = passes.filter((p) => p.razor === 'predates').map((p) => p.label);
const rzCarried = passes.filter((p) => p.razor === 'carried').map((p) => p.label);
if (rzPredates.length)
  console.log('\n*** THE HARD-BAR SECTION BELOW IS OVER ' + rzCarried.length + ' OF ' + passes.length +
    ' PASS(ES) — ' + rzPredates.join(', ') + ' predate(s) the razor field entirely (their rows carry no ' +
    '`endpointRazorClass`), so a site reading `no-field` over those passes is this instrument being unable ' +
    'to ask. It is NOT a page whose every address a parse of the served bytes could have stated, and it is ' +
    'NOT an artifact too old to say — that one is `artifact-predates` and prescribes a rebuild. ***');
/* WHAT ONE PASS SAID ABOUT THE BAR, AS A TOKEN OR AS THE PARTITION, with the four states above kept apart at
   the point the string is composed rather than by a reader holding the convention in their head. */
const rzOne = (m) => {
  if (!('epRazor' in m) || m.epRazor === undefined) return { tok: 'no-field' };
  if (m.epRazor === null) return { tok: 'no-counters' };
  if (typeof m.epRazor === 'string') return { tok: 'artifact-predates' };
  const h = m.epRazor;
  let tot = 0;
  for (const k of Object.keys(h)) tot += h[k];
  return { ro: h[RAZOR_RUNTIME] || 0, un: h[RAZOR_UNPROVEN] || 0, tot, h };
};
/* …AND THE SAME FOUR STATES OVER THE WITNESS PARTITION, which is a SEPARATE reader rather than a field of
   `rzOne` because the two partitions can arrive independently: a pass written by a site.mjs that carries the
   razor field and predates this one leaves `epWitness` missing while `epRazor` is an object, and folding them
   would report the pair as unaskable when half of it is there. */
const wzOne = (m) => {
  if (!('epWitness' in m) || m.epWitness === undefined) return { tok: 'no-field' };
  if (m.epWitness === null) return { tok: 'no-counters' };
  if (typeof m.epWitness === 'string') return { tok: 'artifact-predates' };
  const h = m.epWitness;
  let tot = 0;
  for (const k of Object.keys(h)) tot += h[k];
  return { may: h['may-rest-on'] || 0, none: h['no-witness'] || 0, un: h['unasked'] || 0, tot, h };
};
const rzRows = table.map((t) => {
  const per = t.measurements.map(rzOne);
  const stated = per.filter((x) => x.h);
  /* THE PASS WHOSE PARTITION PROVED THE MOST, AND BOTH BUCKETS READ OFF IT. Ties keep the FIRST, so the
     choice is the earliest pass that reached the maximum rather than whichever the sort happened to leave. */
  let best = null, bestAt = -1;
  for (let i = 0; i < per.length; i++)
    if (per[i].h && (!best || per[i].ro > best.ro)) { best = per[i]; bestAt = i; }
  /* THE WITNESS READ AT THE BAR'S OWN PASS AND NEVER AT ITS OWN BEST — CLAUDE.md
     §A-CONSERVATION-IDENTITY-HOLDS-WITHIN-ONE-SAMPLE. The bound and the bar are a PAIR and the sentence a
     reader composes from them ("part of this run's `unproven` is a population the bar could not look at") is
     about ONE run; taking each from the pass that maximises it would be two runs' margins read as one
     document, which is the defect this file's own data-door section records having committed once. */
  const wAtBest = bestAt >= 0 ? wzOne(t.measurements[bestAt]) : null;
  /* AND THE SPREAD OF THE BOUND OVER EVERY PASS THAT STATED IT, WHICH IS NOT DECORATION BESIDE THE
     AT-BEST VALUE — IT IS WHAT MAKES THAT VALUE READABLE AT ALL. The at-best pass is chosen by MAX
     `runtime-only` with ties keeping the first, and on a document where the bar reads 0 on every pass EVERY
     PASS TIES, so the choice is whichever ran first and the bound printed beside it is one sample of a
     quantity that moves.
     MEASURED, AND IT IS THE FLATTERING DIRECTION, WHICH IS WHY THIS IS A REPAIR AND NOT A REFINEMENT: two
     gitpod passes off ONE artifact, one site, one dwell, no abort in either, both partitions summing to their
     own 190 — `{unasked:1, no-witness:189}` and `{unasked:1, no-witness:97, may-rest-on:92}`. The first says
     the bar's zero is the engine having genuinely proved nothing and sends the next diff to the SOLVER; the
     second says 92 of 190 addresses are a population the bar could not look at and sends it to the PIN. Those
     are opposite diffs, the bar ties at 0 across both, and the at-best rule prints the FIRST.
     THE REASON THE BOUND MOVES AND THE BAR DOES NOT IS THAT IT IS A FACT ABOUT THE PATH. `may-rest-on` says
     the path that composed the address had pinned a source's value; the same address composed on two
     interleavings genuinely has two answers, and the record merges MAX over sightings — so a spread here is
     the field working rather than noise, and a single pass of it is not a measurement. */
  const wStated = per.map((x, i) => (x.h ? wzOne(t.measurements[i]) : null)).filter((w) => w && w.h);
  const wMays = wStated.map((w) => w.may);
  const wMax = wMays.length ? Math.max(...wMays) : 0;
  const wSpread = wMays.length
    ? (() => { const lo = Math.min(...wMays);
               return lo === wMax ? String(lo) : lo + '-' + wMax; })()
    : '-';
  return { id: t.id, n: t.measurements.length, per, stated: stated.length, best, wAtBest,
           wSpread, wMax, wStatedN: wStated.length,
           /* A SPREAD OVER THE PASSES THAT STATED A PARTITION, never over the ones that could not be asked --
              `-` where none did, which is absent and is not a zero. */
           spread: stated.length ? (() => {
             const v = stated.map((x) => x.ro), lo = Math.min(...v), hi = Math.max(...v);
             return lo === hi ? String(lo) : lo + '-' + hi;
           })() : '-' };
});
const rzStated = rzRows.filter((r) => r.stated > 0);
const rzCleared = rzStated.filter((r) => r.best.ro > 0);
console.log('\nTHE HARD BAR (CLAUDE.md §What-the-tool-produces) OVER ' + rzCarried.length + '/' + passes.length +
  ' PASS(ES) — an address, key or value NO PARSE of the served bytes can state. A FLOOR AND A DIAGNOSTIC:\n' +
  '  `runtime-only: 0` beside a nonzero surface is this engine REFUSING TO CLAIM the bar on that document,\n' +
  '  never a smaller capability, and `unproven` is the SAME partition\'s other bucket rather than a second fact.');
/* EVERY COLUMN SIZES ITSELF TO ITS WIDEST VALUE, for `termW`'s reason one table up: `pad` TRUNCATES, and a
   clipped `artifact-predates` is not a narrow value but a wrong one -- `0-4` clipped to `0` is the difference
   between a site that cleared the bar in one pass and one that never did.
   AND THE STACK IS NOT A COLUMN HERE, WHICH IS WHY THE RULE ABOVE IS NOT ENOUGH ON ITS OWN. A width derived
   from the rows is only an improvement over a fixed one where the field is a VALUE; `list.byId`'s third
   column in `apps.tsv` is a PARAGRAPH -- a site's whole evidence note, hundreds of characters with its own
   prose and citations -- so deriving a width from it printed a 1200-column line and a fixed width would have
   clipped it into a sentence that stops mid-claim. The main table above prints no stack for exactly that
   reason and this section does the same; a reader wanting it reads the list. MEASURED on this corpus's own
   `apps.tsv` while building this section, which is the one artifact that could have shown it. */
const rzShown = rzRows.filter((r) => r.n > 0);
const rzCell = (x) => x.h ? (x.ro + '+' + x.un + '=' + x.tot) : x.tok;
const rzClear = (r) => r.spread + ' of ' + r.stated + '/' + r.n + ' pass(es)';
const rzIdW = Math.max('site'.length, ...rzShown.map((r) => r.id.length)) + 2;
const rzClW = Math.max('cleared'.length, ...rzShown.map((r) => rzClear(r).length)) + 2;
/* THE BOUND'S OWN CELL, SIZED LIKE EVERY OTHER COLUMN HERE AND FOR ITS REASON. It reads `-` only where no
   pass stated a bar partition at all, so there was no pass to read it AT; a pass that stated the bar and not
   the bound prints its own token, which is the instrument or the artifact and never a zero. */
const wzCell = (r) => r.wAtBest === null ? '-'
  : !r.wAtBest.h ? r.wAtBest.tok
  /* THE SPREAD FIRST AND THE AT-BEST VALUE SECOND, because the spread is the claim and the at-best value is
     one sample of it. They are printed TOGETHER and never one without the other: where they differ, the
     at-best pass was chosen by a tie and the spread is what a reader may act on. */
  : r.wSpread + ' may (' + r.wAtBest.may + ' at best) / ' + r.wAtBest.un + ' unasked of ' + r.wAtBest.tot
    + ' over ' + r.wStatedN + '/' + r.n;
const rzWW = Math.max('may-rest-on, spread'.length, ...rzShown.map((r) => wzCell(r).length)) + 2;
if (rzShown.length) {
  console.log('  ' + pad('site', rzIdW) + pad('cleared', rzClW) + pad('may-rest-on, spread', rzWW) +
    'runtime-only+unproven=emitted, PER PASS IN ORDER');
  for (const r of rzShown)
    console.log('  ' + pad(r.id, rzIdW) + pad(rzClear(r), rzClW) + pad(wzCell(r), rzWW) +
      r.per.map(rzCell).join(' | '));
  console.log('  `may` BOUNDS the part of `unproven` the bar could not look at and is NEVER added to');
  console.log('  `runtime-only`: a pin is a fact about the PATH that composed the address. The SPREAD is the');
  console.log('  claim and the `at best` figure is one sample of it — where they differ, the at-best pass was');
  console.log('  chosen by a TIE, because that rule maximises `runtime-only` and every pass ties at 0 on a');
  console.log('  document the bar refuses. MEASURED: two gitpod passes off one artifact read 0 and 92 of 190,');
  console.log('  which are opposite next diffs (the SOLVER against the PIN), so one pass of this is no answer.');
}
/* AND THE CORPUS FIGURE, WITH ITS RUN COUNT AND THE DENOMINATOR OF EVERY SHARE IN IT ON THE SAME LINE. A
   count over a corpus that does not state how many passes it is over, and over how many sites could be asked
   at all, is a figure belonging to a population a reader cannot name. */
console.log('hard bar totals: ' + JSON.stringify({
  passes: passes.length, passesCarryingTheField: rzCarried.length,
  sitesStatingAPartition: rzStated.length,
  sitesClearingTheBar: rzCleared.length,
  /* BOTH BUCKETS OFF ONE PASS PER SITE -- the site's best by `runtime-only` -- so the pair is one observation
     and their sum is that pass's own emitted surface rather than two passes' margins added together. */
  runtimeOnlyBestPass: rzStated.reduce((n, r) => n + r.best.ro, 0),
  unprovenAtThatSamePass: rzStated.reduce((n, r) => n + r.best.un, 0),
  emittedAtThatSamePass: rzStated.reduce((n, r) => n + r.best.tot, 0),
  /* THE BOUND AT THAT SAME PASS, AND THE COUNT OF SITES IT COULD BE ASKED OF — two figures and not one,
     because a corpus total over the bound alone cannot say whether a 0 is every site answering 0 or most
     sites not carrying the field. Never added to `runtimeOnlyBestPass`. */
  sitesStatingTheBound: rzStated.filter((r) => r.wAtBest && r.wAtBest.h).length,
  mayRestOnAtThatSamePass: rzStated.reduce((n, r) => n + ((r.wAtBest && r.wAtBest.h) ? r.wAtBest.may : 0), 0),
  unaskedAtThatSamePass: rzStated.reduce((n, r) => n + ((r.wAtBest && r.wAtBest.h) ? r.wAtBest.un : 0), 0),
  /* AND THE OTHER END OF THE SPREAD, LABELLED AS A DIFFERENT PASS AND NEVER SUMMED WITH THE FOUR FIGURES
     ABOVE. The row above is the bound at the BAR'S pass, which on a document the bar refuses is whichever pass
     ran first — so a corpus total of it is a sum of arbitrary samples. This is the bound at ITS OWN maximum,
     per site, and the pair brackets what the bar could not look at. They are two passes of one document by
     construction and are therefore NOT an identity with anything here: adding either to
     `runtimeOnlyBestPass` composes an over-claim out of a floor, and adding them to each other counts one
     surface twice. Read the two as a RANGE, which is the only thing a moving quantity supports. */
  mayRestOnAtItsOwnBestPass: rzStated.reduce((n, r) => n + r.wMax, 0),
}));
/* WHICH DOOR PAIRS WITH WHICH WITNESS CLASS, OVER THE CORPUS — the one statement the two margins above
   structurally cannot make, and the column that had no reader anywhere until this block. A reader holding
   `doors {document-script:1, link-element:97, module-import:92}` beside `witness {unasked:1, no-witness:97,
   may-rest-on:92}` can see the three numbers AGREE and cannot see that they agree ROW BY ROW: two partitions
   of one population whose buckets happen to be the same sizes are consistent with every pairing there is.
   THE KEY SET IS THE FINDING AND THE COUNTS ARE NOT, which is why this prints pairings and not magnitudes.
   site.mjs's join unions every document at the origin and every analysis each held, so its counts are a
   DIFFERENT UNIT from `epRazor`'s — one engine run at `countersFrom` — and differencing the two is
   CLAUDE.md §AND-TWO-INSTRUMENTS-CAN-DISAGREE's category error. A PAIRING survives that where a count does
   not: which door goes with which class holds in the union exactly where it holds per document.
   SO THE ARITHMETIC CHECKED HERE IS THE JOIN'S OWN. `epJoinRows` counts those same arrays by LENGTH while the
   join counts them by ITERATION, so a row carrying neither key still lands in `(no-door-key)|(no-witness-key)`
   and the two agree; a disagreement is this walk having FILTERED rows the length still counts, which is the
   one defect this column can have and the only one worth a verdict. `epJoinSnaps` and `epJoinDocs` are the
   union's axes, printed so no reader reaches for a number of another unit to check it.
   AND A `(no-…-key)` BUCKET IS AN ARTIFACT FACT AND NEVER A PAGE FACT, kept apart from every pairing for
   that reason: the engine writes both keys unconditionally, so such a bucket is a wasm older than one of them
   and says nothing about an address. */
const jnOne = (m) => {
  if (!('epJoin' in m) || m.epJoin === undefined) return { tok: 'no-field' };
  if (m.epJoin === null) return { tok: 'no-counters' };
  if (typeof m.epJoin === 'string') return { tok: 'artifact-predates' };
  const keys = Object.keys(m.epJoin);
  const sum = keys.reduce((n, k) => n + m.epJoin[k], 0);
  const rows = typeof m.epJoinRows === 'number' ? m.epJoinRows : null;
  return { h: m.epJoin, keys, sum, rows,
           snaps: typeof m.epJoinSnaps === 'number' ? m.epJoinSnaps : null,
           docs: typeof m.epJoinDocs === 'number' ? m.epJoinDocs : null,
           /* THREE STATES AND NOT TWO: a pass that carries no `epJoinRows` cannot be checked at all, which is
              an ARTIFACT fact, and folding it into `filtered` would accuse a walk nothing measured. */
           held: rows === null ? null : sum === rows,
           stray: keys.filter((k) => k.includes('(no-door-key)') || k.includes('(no-witness-key)')) };
};
const jnSites = table.map((t) => {
  const per = t.measurements.map(jnOne);
  const stated = per.filter((j) => j.h);
  /* THE UNION OF PAIRINGS OVER EVERY PASS OF THIS SITE, which is the scope-invariant half. A key present in
     one pass and not another is a pass that did not reach that door, never a pairing that changed. */
  const keys = [...new Set(stated.flatMap((j) => j.keys))].sort();
  return { id: t.id, n: per.length, statedN: stated.length, keys,
           /* A VERDICT OVER THE PASSES THAT COULD BE CHECKED, and the count of them beside it, so `ok` over
              zero checkable passes cannot read as a clean bill. */
           checkable: stated.filter((j) => j.held !== null).length,
           filtered: stated.filter((j) => j.held === false).length,
           stray: [...new Set(stated.flatMap((j) => j.stray))].sort(),
           /* `null` WHERE NO PASS STATED THE AXIS AND NEVER A ZERO, which the first version of this cell got
              wrong in exactly the direction this file spends pages on: it read `j.snaps === null ? 0` and then
              took a MAX, so a pair of passes predating the two denominators printed `0 doc(s) 0 analys(es)` —
              a measurement of nothing rendered as a measurement of zero, on the row whose whole purpose is to
              carry a denominator. It was caught by RUNNING the reader over the archived passes rather than by
              reading it, which is why that run happened before the commit. */
           snaps: (() => { const v = stated.map((j) => j.snaps).filter((x) => x !== null);
                           return v.length ? Math.max(...v) : null; })(),
           docs: (() => { const v = stated.map((j) => j.docs).filter((x) => x !== null);
                          return v.length ? Math.max(...v) : null; })(),
           tok: stated.length ? null : (per.length ? per[0].tok : 'no-pass') };
});
const jnShown = jnSites.filter((r) => r.statedN || r.tok !== 'no-field');
if (jnShown.length) {
  const jnIdW = Math.max('site'.length, ...jnShown.map((r) => r.id.length)) + 2;
  console.log('');
  console.log('door \u00d7 witness pairings (site, passes stating the join, its own denominators, the pairings):');
  for (const r of jnShown)
    console.log('  ' + pad(r.id, jnIdW) +
      pad(r.statedN ? r.statedN + '/' + r.n + ' stated' : (r.tok || '-'), 16) +
      pad(r.checkable ? (r.filtered ? r.filtered + '/' + r.checkable + ' FILTERED'
                                    : 'sums ' + r.checkable + '/' + r.checkable) : 'uncheckable', 20) +
      pad(r.docs === null || r.snaps === null ? 'axes not stated'
                                                : r.docs + ' doc(s) ' + r.snaps + ' analys(es)', 24) +
      (r.keys.length ? r.keys.join(' ') : '(empty surface)') +
      (r.stray.length ? '   ARTIFACT-OLDER-THAN-A-KEY: ' + r.stray.join(' ') : ''));
  console.log('  the PAIRINGS are the finding; the counts are a union over documents and are NOT `epRazor`\'s');
  console.log('  unit, so they are checked against `epJoinRows` on the same row and never against `emitted`');
}
/* HOW MUCH OF THE INSTANCE'S SPAN THE ENGINE'S OWN LOOP GOT, AND HOW MANY FLOWS COULD TAKE A DELIVERY — the
   two readings the endpoint columns above cannot be read without, and the pair that reconciles two measurements
   of this corpus that each refuted the other.
   THE SHARE IS A RATIO OF TWO FIELDS OF ONE ROW, which is the only form a span figure may take here: `loopUs`
   and `instanceUs` are read WITHIN one sample, so no two-moments comparison is being made
   (CLAUDE.md §A-CONSERVATION-IDENTITY-HOLDS-WITHIN-ONE-SAMPLE), and the producer asserts
   `instanceUs == loopUs + betweenSlicesUs` so the complement needs no second field here.
   MEASURED, AND IT IS WHY THIS BLOCK EXISTS RATHER THAN A PARAGRAPH: on gitpod the share SEPARATES the endpoint
   mode where `slices` does not. Every pass whose `module-import` door is nonzero reads a share at or above
   10.3% and every pass whose door is zero reads at or below 2.2%, five each. `slices` does not merely overlap,
   it INVERTS on a pair: a LOW pass at ELEVEN slices sits beside a HIGH pass at SIX, so the slice count orders
   those two the wrong way round and a scheduler reading taken off it was never able to tell the modes apart.
   That is also why the share and the count are printed TOGETHER rather than one standing for the other — the
   same number of visits can be 0.9% or 37.3% of a span. Two earlier readings of this corpus therefore each described ONE mode and were quoted as
   the engine's: "0.5-2.2% of its span in 2-3 slices" is exactly the four low-mode passes, and "48-96% in 12-16
   slices" is exactly the high-mode pair. Neither was wrong and both were under-scoped, which is
   §a-bare-count-over-a-population-you-have-not-partitioned read across runs instead of within one.
   AND `stackEmpty` IS THE ONE FIGURE THAT DOES NOT MOVE, which no producer can see because it is a CROSS-RUN
   fact. The engine asserts `canDeliver <= stackEmpty <= live` WITHIN a sample and this file does not restate it
   (§AN-AUDITOR-DERIVES-THE-RULE); what it publishes is the DISTRIBUTION of that gauge over the passes, and on
   this corpus it is a single value at every one of them while `pend` runs into the hundreds. A constant
   receiving capacity beside a queue that is not constant is a statement about the SEAM rather than about any
   run, and it is invisible from inside one.
   AND THE DRIVER'S OWN ROUND IS ON THE SAME CELL BECAUSE THE SHARE ALONE CANNOT SAY WHY. A low share has two
   readings that take opposite work — the HOST did not re-enter this engine's loop, or the engine had nothing
   runnable — and the round partition is what separates them. MEASURED over the five gitpod passes that carry
   both rows, sorted by share descending: the fraction of rounds ENDING IN A FETCH SERVICE rises monotonically
   as the share falls (54, 64, 72, 78, 91 per cent against 48.3, 37.3, 11.4, 10.3, 0.9), and so does the POOL
   the level-1 order was choosing between (2, 3, 4, 5, 6). Five of five, no exception.
   AND THE MECHANISM THAT CORRELATION INVITES IS REFUTED BY THE PRODUCER, which is recorded here because the
   commit that landed this block published it: it said the share is "what one engine gets when the one thread
   is shared N ways and MOST ROUNDS GO TO SERVING SOMEBODY'S BYTES", and a serviced round is not a round the
   engine did not get. bridge.js sets `rd.shape = "serviced"` AFTER `ops.step(target)` has already answered —
   `st !== 0` IS that answer — so every serviced round STEPPED this engine and then paid what it asked for. The
   N-ways half stands and the serving half does not, and a reader who takes `rServiced` for a round spent
   elsewhere has the arithmetic backwards.
   SO THE MECHANISM IS NOT ESTABLISHED BY ANY ROW HERE, WHICH IS THE HONEST STATE RATHER THAN A GAP TO FILL
   WITH A GUESS. Three candidates are each refuted or unseparated by what is printed: a DELIVERY ceiling cannot
   be it, since `canDeliver` and `stackEmpty` are 4 at every pass and `pendReady` does not predict the share in
   either direction (0 ready reads 37.3% and 10.3%, 279 ready reads 0.9%); WAITING ON THE NETWORK cannot be it
   alone, since the highest-share pass asked for the MOST replies (479) and still ran 96.1%; and `pool` against
   the service fraction cannot be separated at all, because they move together in every pass this corpus has.
   `round` is also the HOST's count over the WHOLE pool while `slices` is this engine's own, so the two are
   §AND-TWO-INSTRUMENTS-CAN-DISAGREE's different units and their quotient is not a quantity — which is why
   neither is divided by the other here. What IS established is that the reach mode is partly a property of the
   DRIVE — how many documents of the origin the harness had open — which is §A-FIXTURE-BUILT-TO-EXERCISE-EVERY-
   MECHANISM's hazard read from the other end: a figure a reader takes for the engine's.
   AND THE OTHER SIX ARMS ARE ZERO AT EVERY ONE OF THOSE PASSES, which is why only two are printed: the
   partition sums to `round` exactly in all five, so the host never found nothing to do, never released, never
   finished an engine and never threw. A reader wanting the full vocabulary reads `hostRound` whole on the row,
   which site.mjs relays unsummarised for exactly that reason.
   NOTHING IS DIFFERENCED AND NO VERDICT IS COMPOSED. `deliverGuard` is a GAUGE, so a difference of it across
   two passes is not a quantity; the share is per-pass and is printed per-pass; and the mode is read off the
   endpoint door rather than inferred from either, so the separation above is a statement a reader can check
   against the door column and not one this block asserts. */
const spOne = (m) => {
  const w = (m.span && typeof m.span === 'object') ? m.span : null;
  const d = (m.dguard && typeof m.dguard === 'object') ? m.dguard : null;
  const r = (m.rdoor && typeof m.rdoor === 'object') ? m.rdoor : null;
  if (!w && !d && !r && !(m.hround && typeof m.hround === 'object')
        && !(m.fkPair && typeof m.fkPair === 'object'))
    return { tok: !('span' in m) ? 'no-field' : (m.span === null ? 'no-counters' : String(m.span)) };
  const n = (o, k) => (o && typeof o[k] === 'number' ? o[k] : null);
  const loop = n(w, 'loopUs'), inst = n(w, 'instanceUs');
  const hr = (m.hround && typeof m.hround === 'object') ? m.hround : null;
  const fk = (m.fkPair && typeof m.fkPair === 'object') ? m.fkPair : null;
  return { w, d, r, hr, fk,
           /* `null` AND NEVER A ZERO where either operand is absent, and never where `instanceUs` is 0 — a
              share of a span that did not happen is not a small share. */
           share: (loop === null || !inst) ? null : (100 * loop / inst),
           slices: n(w, 'slices'), stackEmpty: n(d, 'stackEmpty'), canDeliver: n(d, 'canDeliver'),
           pend: n(d, 'pend'), pendReady: n(d, 'pendReady'),
           asked: n(r, 'replyAsked'), answered: n(r, 'replyAnswered'), outstanding: n(r, 'replyOutstanding'),
           /* THE TWO ARMS THAT HAVE EVER BEEN NONZERO, AND THE SHARE OF ROUNDS THAT ENDED IN A FETCH SERVICE.
              It is a ratio of two fields of ONE round object, so it is read within one sample like the span
              share. `pool` is how many engines the level-1 order was choosing between, carried because the
              two move together on this corpus and no sample here can separate them. */
           round: n(hr, 'round'), rServ: n(hr, 'rServiced'), rWait: n(hr, 'rWaited'), pool: n(hr, 'pool'),
           servShare: (n(hr, 'round') && n(hr, 'rServiced') !== null) ? (100 * hr.rServiced / hr.round) : null,
           /* THE PAIR AND ITS SHARE, `null` WHERE EITHER OPERAND IS ABSENT AND NEVER A ZERO — including for a
              zero DENOMINATOR, because a share of no forks is not a small share: a document that forked nothing
              has not declined to re-fork a proved subject, it was never asked. */
           forks: n(fk, 'forks'), forkPinned: n(fk, 'forkOverPinned'),
           pinShare: (n(fk, 'forks') && n(fk, 'forkOverPinned') !== null)
             ? (100 * fk.forkOverPinned / fk.forks) : null };
};
const spRows = table.map((t) => {
  const per = t.measurements.map(spOne);
  const stated = per.filter((x) => x.w || x.d || x.r || x.hr || x.fk);
  return { id: t.id, n: per.length, per, stated,
           tok: stated.length ? null : (per.length ? per[0].tok : 'no-pass') };
});
const spShown = spRows.filter((r) => r.stated.length);
if (spShown.length) {
  const spIdW = Math.max('site'.length, ...spShown.map((r) => r.id.length)) + 2;
  const cell = (x) => (x.share === null ? '-' : x.share.toFixed(1) + '%') +
    '/' + (x.slices === null ? '-' : x.slices + 'sl') +
    ' ' + (x.canDeliver === null ? '-' : x.canDeliver) + '≤' +
    (x.stackEmpty === null ? '-' : x.stackEmpty) + ' of ' +
    (x.pendReady === null ? '-' : x.pendReady) + '/' + (x.pend === null ? '-' : x.pend) +
    ' ' + (x.servShare === null ? '-' : x.servShare.toFixed(0) + '%serv') +
    '/' + (x.pool === null ? '-' : 'pool' + x.pool) +
    ' ' + (x.forkPinned === null ? '-' : x.forkPinned) + '/' +
    (x.forks === null ? '-' : x.forks) + 'fk';
  console.log('');
  console.log('ENGINE SPAN, DELIVERY CAPACITY AND THE DRIVER\'S OWN ROUND (site, then PER PASS IN ORDER):');
  /* EVERY QUANTITY THE CELL PRINTS IS NAMED HERE, which is not decoration: a reader takes a legend as the
     key to a row, so a cell carrying more than the legend names is read as whichever of its fields the
     legend happens to list — the orientation defect CLAUDE.md records for a ratio, arriving in a column. */
  console.log('  <loop share of the instance\'s span>/<slices>  <canDeliver>≤<stackEmpty> of ' +
              '<pendReady>/<pend>  <rounds ending in a fetch service>serv/pool<engines the order chose ' +
              'between>  <forks over an already-proved subject>/<forks>fk');
  for (const r of spShown)
    console.log('  ' + pad(r.id, spIdW) + r.stated.map(cell).join(' | '));
  /* THE GAUGE'S DISTRIBUTION OVER THE PASSES, which is the cross-run fact no single run can state and the
     reason this block is not just a span column. Printed as the set of values with how many passes each, so a
     constant is visible as a constant rather than having to be inferred from a column. */
  const seen = {};
  for (const r of spShown) for (const x of r.stated)
    if (x.stackEmpty !== null) seen[x.stackEmpty] = (seen[x.stackEmpty] === undefined ? 0 : seen[x.stackEmpty]) + 1;
  const vals = Object.keys(seen);
  console.log('  stackEmpty over every pass stating it: ' + JSON.stringify(seen) +
    (vals.length === 1 ? '  — ONE value at all of them, while `pend` is not constant: a RECEIVING CAPACITY'
                       : '  — more than one value, so it is not the constant this block was written about'));
  console.log('  the share is read WITHIN one row (loopUs/instanceUs); `deliverGuard` is a GAUGE and is never');
  console.log('  differenced across passes; the MODE is read off the door column above and not inferred here');
}

/* WHICH TURNS SPENT THE SPAN THE BLOCK ABOVE MEASURES — the reading that block hands off and the one this
   corpus has never been able to take. The fork solver/result.c states has two named arms (`loopUs` small says
   the DRIVER; `loopUs` large with `stepUs` small says this scheduler) and a THIRD it is silent about: `loopUs`
   large with `stepUs` large too, the loop GIVEN the thread and spending it inside TURNS. MEASURED over three
   drives of one release artifact on one real app, that third arm is the arm this corpus's real sites take, and
   the share column above is where a reader checks it rather than this sentence.
   `stepUs/steps` IS NOT PRINTED HERE AND IS NOT THE QUESTION, because the producer calls it "A MEAN NO TURN IS
   NEAR" and the same three drives give 7613, 48405 and 14836 microseconds per step — a 6.4x spread on one
   binary, one site and one dwell, so the quotient reads which MODE a drive landed in and says nothing about a
   turn. What is printed instead is the PARTITION and the FRACTION the producer names in its place.
   THREE QUANTITIES, THREE COMPONENTS, AND THEY ARE NEVER SUMMED ACROSS EACH OTHER. The `sched` share is
   `schedUs/stepUs` — everything in the turn that is not the step, which telescopes the previous iteration's
   tail, so a large one is the pick and the delta costing more than the work they order and the diff is in this
   scheduler. The `over` fraction is `sliceOverruns/steps` — the slice-bound share, which a mean cannot give.
   And `seam` is `sliceOverrunSeamless/sliceOverruns`: all-seamless says the thread was inside C declaring no
   step boundary and the answer is a step-machine conversion, while a large `asks` beside a small `seam` says
   the suspend points were THERE and the stretch ran anyway, which is a question about the page. The producer
   says in as many words that the sum alone "can be carried by a single chatty turn, so the pair is the
   reading", which is why `asks` is printed beside the fraction and not folded into it.
   EVERY SHARE IS READ WITHIN ONE PASS and nothing is differenced across two — these are lifetime counts and may
   be differenced BY THEIR OWN KIND, but two passes are two runs and that is a different comparison. A `null`
   is printed as `-` and never as a zero wherever an operand is absent OR a denominator is zero: a share of a
   partition that did not happen is not a small share, and a seamless fraction of no overruns is not zero
   seamless turns — it is the POSITIVE statement that no turn overran, which the `over` cell already carries.
   THE ARM IS NAMED AND NEVER RANKED HERE. `suo` is a histogram and its top arm is what names the component; it
   is printed as the single largest arm with its own `stepUnitRuns` denominator beside it, because the producer's
   reading is the PAIR — "an arm with many runs and no overruns is cheap however often it is taken, and an arm
   whose two counts are EQUAL is a step that cannot rest" — and an arm without its run count cannot be read
   either way.
   ABSENT IS NOT ZERO AT EITHER GRAIN. `no-field` is a pass predating these rows, `no-counters` is a pass no
   census of which carried a cold document, and a verdict string from site.mjs is printed VERBATIM because a
   partition that does not close makes every share on the cell unreadable and that is the finding. */
const tpOne = (m) => {
  const p = (m.tphase && typeof m.tphase === 'object') ? m.tphase : null;
  const h = (m.suo && typeof m.suo === 'object' && !Array.isArray(m.suo)) ? m.suo : null;
  const runs = (m.sur && typeof m.sur === 'object' && !Array.isArray(m.sur)) ? m.sur : null;
  const seamArms = (m.suoSeam && typeof m.suoSeam === 'object' && !Array.isArray(m.suoSeam)) ? m.suoSeam : null;
  /* THREE STATES READ OFF THE VALUE AND NEVER OFF THE KEY, and this block got it wrong once in the way the
     `tlad` block below already records: the mapping assigns `tphase` UNCONDITIONALLY, so an `'tphase' in m`
     test can never be false and the `predates` arm it guarded was DEAD — every pass older than these rows
     printed as `no-counters`, which is a fact about the RUN, when the truth was a fact about the DRIVER.
     The warning was in THIS FILE, specific, about this exact construct, and it did not fire, which is why it
     is recorded here rather than quietly fixed. `undefined` is site.mjs not having written the field at all;
     `null` is site.mjs having written it with no cold document to read; a string is a stated absence. */
  if (!p && !h)
    return { tok: m.tphase === undefined ? 'predates-the-rows'
             : (m.tphase === null ? 'no-counters' : String(m.tphase)) };
  const n = (o, k) => (o && typeof o[k] === 'number' ? o[k] : null);
  const stepUs = n(p, 'stepUs'), sched = n(p, 'schedUs'), slice = n(p, 'sliceUs');
  const over = n(p, 'sliceOverruns'), asks = n(p, 'sliceOverrunAsks'), seam = n(p, 'sliceOverrunSeamless');
  const steps = typeof m.steps === 'number' ? m.steps : null;
  /* THE SINGLE LARGEST ARM AND ITS OWN RUN COUNT, picked by value with the key as a stable tiebreak so two
     passes of one shape name the same arm. A histogram of all zeros names NO arm rather than its first key. */
  let top = null;
  if (h) for (const k of Object.keys(h).sort())
    if (typeof h[k] === 'number' && h[k] > 0 && (top === null || h[k] > h[top])) top = k;
  return { p, h,
           schedShare: (sched === null || !stepUs) ? null : (100 * sched / stepUs),
           sliceShare: (slice === null || !stepUs) ? null : (100 * slice / stepUs),
           overShare: (over === null || !steps) ? null : (100 * over / steps),
           seamShare: (seam === null || !over) ? null : (100 * seam / over),
           over, asks, steps,
           topArm: top, topOver: top === null ? null : h[top],
           topRuns: (top !== null && runs && typeof runs[top] === 'number') ? runs[top] : null,
           /* THE THIRD MEMBER OF THE TRIPLE FOR THAT SAME ARM, so the cell answers the question the pair
              hands off instead of leaving a reader to join a histogram to a scalar. `null` where the row is
              absent, which is an artifact predating it and NOT an arm whose turns all offered a point. */
           topSeam: (top !== null && seamArms && typeof seamArms[top] === 'number') ? seamArms[top] : null,
           wrong: [m.tphaseWrong, m.suoWrong, m.suoSeamWrong]
             .filter((x) => typeof x === 'string' && x !== '').join('; ') };
};
const tpRows = table.map((t) => {
  const per = t.measurements.map(tpOne);
  const stated = per.filter((x) => x.p || x.h);
  return { id: t.id, per, stated, tok: stated.length ? null : (per.length ? per[0].tok : 'no-pass') };
});
const tpShown = tpRows.filter((r) => r.stated.length);
/* THE HEADER AND THE SILENT LIST PRINT WHETHER OR NOT ANY PASS STATED THE ROWS, which is the one way this
   block differs from the span block above and is not a style choice. These six rows are NEW, so every census
   file already on disk predates them and `tpShown` is empty for all of them — and a section that renders
   NOTHING in that state is indistinguishable from a section that does not exist, which is the
   §A-DIAGNOSTIC-GATED-ON-A-FINDING defect arriving in a reader: the run that explains nothing is the run whose
   explanation is wanted, and gating the explanation on there being something to explain silences it exactly
   then. It is also the only thing that ARMS this reader before a drive: a section that has only ever printed
   when stated has never shown its absent arm speaking. */
if (tpRows.length) {
  const tpIdW = Math.max('site'.length, ...tpRows.map((r) => r.id.length)) + 2;
  const pc = (x) => (x === null ? '-' : x.toFixed(1) + '%');
  const cell = (x) => (x.wrong ? '[' + x.wrong + '] ' : '') +
    pc(x.schedShare) + 'sched ' +
    (x.over === null ? '-' : x.over) + '/' + (x.steps === null ? '-' : x.steps) + 'over' +
    '(' + pc(x.overShare) + ') ' +
    pc(x.seamShare) + 'seam/' + (x.asks === null ? '-' : x.asks) + 'asks ' +
    (x.topArm === null ? '-' : x.topArm + ' ' + x.topOver + '/' +
      (x.topRuns === null ? '-' : x.topRuns) + ' seam ' +
      (x.topSeam === null ? '-' : x.topSeam));
  console.log('');
  console.log('WHICH TURNS SPENT THE SPAN (site, then PER PASS IN ORDER):');
  /* EVERY QUANTITY THE CELL PRINTS IS NAMED HERE, for the span block's reason: a reader takes a legend as the
     key to a row, so a cell carrying more than its legend names is read as whichever field the legend lists. */
  console.log('  <schedUs as a share of stepUs>sched  <sliceOverruns>/<steps>over(<fraction>)  ' +
              '<seamless share of those overruns>seam/<suspend points they offered>asks  ' +
              '<largest overrunning arm> <its overruns>/<its runs> seam <of those overruns, how many offered ' +
              'NO suspend point>');
  for (const r of tpShown)
    console.log('  ' + pad(r.id, tpIdW) + r.stated.map(cell).join(' | '));
  console.log('  a `-` is an absent operand or a zero denominator and NEVER a zero share; a bracketed string is');
  console.log('  site.mjs\'s own partition verdict, which makes every share on that cell unreadable');
  const silent = tpRows.filter((r) => !r.stated.length);
  if (silent.length)
    console.log('  NOT STATED: ' + silent.map((r) => r.id + '(' + r.tok + ')').join(' ') +
      '  — `predates-the-rows` is a fact about the DRIVER that wrote the census and `no-counters` one about' +
      ' the RUN; neither is a fact about the engine');
}

/* WHAT THE ORDER COST INSIDE THOSE TURNS — the half the block above hands off and the half that decides
   whether the throughput question is the SCHEDULER's at all. solver/engine.h states the trap in its own words:
   "A reader who takes a small `sched_us` for 'the ordering is not the cost' has bounded the pick and said
   nothing about the hook" — `flow_next_to_run` runs before the step bracket opens and lands in `schedUs`, while
   the preempt hook's rescan is called FROM THE INTERPRETER, so an O(members) walk through `flow_rival_of` is
   charged to `sliceUs`, inside the very turns `sliceOverruns` counts. Measured over three drives of one release
   artifact, `schedUs` is 0.026-0.117% of `stepUs` and the seamless count is NOT equal to the overrun count, so
   engine.h's own exclusion for the hook does not hold and those turns offered about a MILLION consultations.
   THREE QUESTIONS, THREE DENOMINATORS, AND THEY ARE NEVER MIXED. `rivalW/rivalR` is the frontier a rescan
   actually WALKED, which no other row carries. `rivalR/asks` is the MISS RATE — "the share of CONSULTATIONS
   that bought a walk" — and it separates a CACHE that absorbs nothing (repair at the cache) from a GENERATION
   moving as fast as the hook is consulted (the page branching, and the repair is nowhere near it); both print
   the same rival count, which is why the rate and not the count is the reading. And `rivalW` as a share of ALL
   weighing says how much of this engine's frontier-pricing the hook does.
   THE CENSUS WALK IS COUNTED APART AND IS NEVER IN THAT DENOMINATOR'S NUMERATOR BY ACCIDENT: its cadence is the
   REPORT's, so folding it in "would put the instrument's own cost inside the rate that exists to price the
   dispatch". It is printed as its own share so a reader can see whether the instrument is heavy enough to change
   the run it samples, which is the only way that question can be settled.
   THE THREE MISS ARMS ARE A PARTITION AND THE THIRD IS NOT A ROUNDING ROW, which is why they are printed as
   three numbers and never summed into a verdict. engine.h: "a large `cur` beside a large `both` and a large
   `cur` beside a zero `both` recommend the same work at completely different prices, and no arithmetic over two
   rows can separate them" — where both invalidators moved in one interval, removing one buys NOTHING. `gen` is
   the frontier generation having moved, `cur` the incumbent having changed, `both` both in one interval.
   EVERY SHARE IS READ WITHIN ONE PASS and nothing is differenced across two; a `null` prints as `-` wherever an
   operand is absent OR a denominator is zero, because a share of a walk that did not happen is not a small
   share and a miss rate over no consultations is not a cache that absorbed everything — that is
   `asks 0`, which the cell carries. The header and the silent list print unconditionally for the phase block's
   reason: these rows are new, so every census already on disk predates them, and a section that renders nothing
   in that state is indistinguishable from one that does not exist. */
const scOne = (m) => {
  const s = (m.scost && typeof m.scost === 'object') ? m.scost : null;
  if (!s) return { tok: m.scost === undefined ? 'predates-the-rows'
                   : (m.scost === null ? 'no-counters' : String(m.scost)) };
  const n = (k) => (typeof s[k] === 'number' ? s[k] : null);
  const rivalR = n('scanRivalRuns'), rivalW = n('scanRivalWeights');
  const nextR = n('scanNextRuns'), nextW = n('scanNextWeights');
  const otherW = n('scanOtherWeights'), censusW = n('scanCensusWeights'), censusR = n('scanCensusRuns');
  const asks = n('preemptAsksLifetime');
  const allW = [nextW, rivalW, otherW, censusW].every((x) => x !== null)
    ? nextW + rivalW + otherW + censusW : null;
  return { s,
           rivalR, rivalW, nextR, nextW, censusR, censusW, asks,
           rivalFrontier: (rivalR ? rivalW / rivalR : null),
           nextFrontier: (nextR ? nextW / nextR : null),
           missRate: (asks && rivalR !== null) ? (100 * rivalR / asks) : null,
           hookShare: (allW && rivalW !== null) ? (100 * rivalW / allW) : null,
           censusShare: (allW && censusW !== null) ? (100 * censusW / allW) : null,
           gen: n('rivalMissGen'), cur: n('rivalMissCur'), both: n('rivalMissBoth'),
           wrong: typeof m.scostWrong === 'string' && m.scostWrong !== '' ? m.scostWrong : '' };
};
const scRows = table.map((t) => {
  const per = t.measurements.map(scOne);
  const stated = per.filter((x) => x.s);
  return { id: t.id, per, stated, tok: stated.length ? null : (per.length ? per[0].tok : 'no-pass') };
});
if (scRows.length) {
  const scIdW = Math.max('site'.length, ...scRows.map((r) => r.id.length)) + 2;
  const pc = (x) => (x === null ? '-' : x.toFixed(1) + '%');
  const q = (x) => (x === null ? '-' : x.toFixed(1));
  const cell = (x) => (x.wrong ? '[' + x.wrong + '] ' : '') +
    'hook ' + pc(x.hookShare) + ' walks ' + (x.rivalR === null ? '-' : x.rivalR) + '/' +
    (x.asks === null ? '-' : x.asks) + '(' + pc(x.missRate) + ') ' +
    'front ' + q(x.rivalFrontier) + 'v' + q(x.nextFrontier) + ' ' +
    'miss ' + (x.gen === null ? '-' : x.gen) + '/' + (x.cur === null ? '-' : x.cur) + '/' +
    (x.both === null ? '-' : x.both) + ' ' +
    'census ' + pc(x.censusShare);
  console.log('');
  console.log('WHAT THE ORDER COST INSIDE THOSE TURNS (site, then PER PASS IN ORDER):');
  /* EVERY QUANTITY THE CELL PRINTS IS NAMED HERE, for the span block's reason: a cell carrying more than its
     legend names is read as whichever field the legend lists. */
  console.log('  hook <rival weights as a share of ALL frontier-weighing>  walks <rescans>/<hook ' +
              'consultations>(<miss rate>)  front <members a rescan walked>v<members a PICK walked>  ' +
              'miss <gen>/<cur>/<both>  census <the instrument\'s own share of all weighing>');
  for (const r of scRows.filter((x) => x.stated.length))
    console.log('  ' + pad(r.id, scIdW) + r.stated.map(cell).join(' | '));
  console.log('  a `-` is an absent operand or a zero denominator and NEVER a zero share; the three miss arms');
  console.log('  are a PARTITION of <rescans> and are never summed into a verdict — `both` says removing one');
  console.log('  invalidator alone buys nothing; a bracketed string is site.mjs\'s own partition verdict');
  const scSilent = scRows.filter((r) => !r.stated.length);
  if (scSilent.length)
    console.log('  NOT STATED: ' + scSilent.map((r) => r.id + '(' + r.tok + ')').join(' ') +
      '  — `predates-the-rows` is a fact about the DRIVER and `no-counters` one about the RUN');
}

/* WHERE EACH ORPHAN ASK WENT — the half the `ask>drv` column hands off, and the one row that says which of two
   opposite repairs the orphan walk needs. The column above answers HOW FAR the question got and is silent about
   what the ask DID: a run with `asked` in the hundreds is consistent with a cache absorbing nearly all of them
   and a handful of expensive walks, and equally consistent with nearly every ask being a full enumeration of
   `rt->gc_obj_list`, and those two name repairs in different files. `memo` is the generation cache answering with
   NO walk; `empty` is a walk that ran and found no takeable body, which solver/engine.c's residual at the take
   states is a fact about the HEAP and not about the bundle, since the walk can only see a body with a live
   function object of its own; `took` is a walk that handed one over.
   WHY IT IS WORTH ITS OWN SECTION: measured over three drives of one release artifact on one real app,
   `seed-one-orphan-flow` overran the cooperative slice in 204 of 221, 237 of 242 and 159 of 190 of its own runs
   — 84%, 85% and 83% of ALL overrunning turns in the run — while the PICK was 0.16-0.24% of the thread and the
   preempt hook's cache absorbed 99.5% of two to three million consultations, so the ordering is exonerated by
   measurement and this walk is what is left. The repair's ADDRESS is what these three decide.
   A PARTITION AND NEVER A VERDICT: the three are printed as three numbers with their total beside them, because
   a share alone cannot separate `memo 0` from a census that never reached the question — that is `ask 0`, which
   the cell carries. The bracketed string is site.mjs's own release-mode partition check and is reproduced here
   rather than recomputed, for the span block's reason: a second copy of that arithmetic is the one that drifts.
   The header and the silent list print UNCONDITIONALLY, also for the span block's reason — these rows are new,
   so every census already on disk predates them, and a section that renders nothing in that state is
   indistinguishable from one that does not exist. */
const oxOne = (m) => {
  const n = (k) => (typeof m[k] === 'number' ? m[k] : null);
  const memo = n('omemo'), empty = n('oempty'), took = n('otook'), ask = n('oask');
  const stated = [memo, empty, took].every((x) => x !== null);
  const walks = n('owalks'), entries = n('oentries'), wfull = n('owfull'), full = n('ofull');
  return { stated, memo, empty, took, ask, drv: n('odrv'),
           walks, entries, wfull, full,
           perWalk: (walks ? entries / walks : null),
           perFull: (wfull ? full / wfull : null),
           cWrong: typeof m.owCountWrong === 'string' && m.owCountWrong !== '' ? m.owCountWrong : '',
           /* THE ABSENCE IS READ OFF THE VALUE AND NEVER OFF `in`, which is the construct the span block's own
              comment records as having made its `predates` arm dead: the mapping assigns these keys
              unconditionally, so `'omemo' in m` is true of a pass whose rows carry nothing. `undefined` is a
              fact about the DRIVER that wrote the census and `null` one about the RUN it measured. */
           tok: m.omemo === undefined ? 'predates-the-rows' : 'no-counters',
           wrong: typeof m.oexitWrong === 'string' && m.oexitWrong !== '' ? m.oexitWrong : '' };
};
const oxRows = table.map((t) => {
  const per = t.measurements.map(oxOne);
  const stated = per.filter((x) => x.stated);
  return { id: t.id, per, stated, tok: stated.length ? null : (per.length ? per[0].tok : 'no-pass') };
});
if (oxRows.length) {
  const oxIdW = Math.max('site'.length, ...oxRows.map((r) => r.id.length)) + 2;
  const v = (x) => (x === null ? '-' : String(x));
  const q = (y) => (y === null ? '-' : y.toFixed(0));
  const cell = (x) => (x.wrong ? '[' + x.wrong + '] ' : '') + (x.cWrong ? '[' + x.cWrong + '] ' : '') +
    'ask ' + v(x.ask) + ' = memo ' + v(x.memo) + ' + empty ' + v(x.empty) + ' + took ' + v(x.took) +
    ' (drv ' + v(x.drv) + ')  walks ' + v(x.walks) + ' x ' + q(x.perWalk) + ' entries, cand ' +
    q(x.perFull) + ' over ' + v(x.wfull) + ' full';
  console.log('');
  console.log('WHERE EACH ORPHAN ASK WENT (site, then PER PASS IN ORDER):');
  /* EVERY QUANTITY THE CELL PRINTS IS NAMED HERE, for the span block's reason: a cell carrying more than its
     legend names is read as whichever field the legend lists. */
  console.log('  ask <orphan asks> = memo <the generation cache answered, NO walk> + empty <walked, heap held ' +
              'no takeable body> + took <walked and handed one over>  (drv <bodies actually driven>)');
  console.log('  walks <enumerations of the object list> x <mean entries ONE walk stepped over>, ' +
              'cand <mean candidates> over <walks that ran to the END of the list, which is that mean\'s own ' +
              'denominator and not the first one>');
  for (const r of oxRows.filter((x) => x.stated.length))
    console.log('  ' + pad(r.id, oxIdW) + r.stated.map(cell).join(' | '));
  console.log('  a high `memo` says the cache absorbs and the walks that happen are few; a low one says the');
  console.log('  orphan generation moves as fast as flows run out of work, so nearly every ask enumerates the');
  console.log('  GC object list. READ IT BESIDE `took`: where took is nearly every ask the walks are PRODUCTIVE,');
  console.log('  and no cache can skip a walk that succeeds — the repair is then the WALK, which the `walks`');
  console.log('  and `cand` cells price; a bracketed string is site.mjs\'s own partition or walk-count verdict');
  const oxSilent = oxRows.filter((r) => !r.stated.length);
  if (oxSilent.length)
    console.log('  NOT STATED: ' + oxSilent.map((r) => r.id + '(' + r.tok + ')').join(' ') +
      '  — `predates-the-rows` is a fact about the DRIVER and `no-counters` one about the RUN');
}

/* WHICH ARM OF `flow_step` TOOK THE STEP OF A MEMBER THAT WAS HOLDING A RUNNABLE TASK — the row the job split
   hands off to, and the one number that says WHICH DIFF a stalled task queue names. The split above says a
   backlog is RANK-ELIGIBLE (`jobsReady`) and `jobWGap` says whether the order is what holds it; where that
   pair answers "the order is fine and the queue is not moving" the next question is the ladder's, and until
   this block nothing in this corpus could ask it.
   WHICH SIZE NAMES WHICH DIFF, taken from the RAISE SITES in solver/engine.c and not from solver/engine.h's
   legend — the two disagree about `deliv` and the header is the stale copy, which engine.c's own block says in
   as many words (retired and kept, so a reader meeting a large value does not re-derive it).
   `dl` IS TWO QUANTITIES EITHER SIDE OF ONE DIFF, WHICH IS WHY THIS COLUMN PRINTS IT AND NEVER TRENDS IT. The
   RETIRED reading: the arm stood above the whole arrival chain and was not in it, a reply register entry
   carrying no stamp, so a delivery preceded every row and every queued callback whatever their ages, and the
   row was the size of that EXCLUSION. The stamp is BUILT (`PEND_WORK_SEQ`) and the arm asks
   `flow_task_precedes` with it, so `dl` is now the size of the arm going in front of a YOUNGER task — the
   arrival order WORKING — and the old reading is what a NEAR-ZERO means afterwards. One large number, two
   opposite findings, and no cell here can say which: the discriminator is the ARTIFACT
   (`git grep -c PEND_WORK_SEQ <the pass's stamp> -- engine/host`), not the census, so two `dl` cells of one
   row are comparable only once that grep has answered the same way at both. MEASURED: it is ABSENT at the head
   the installed artifact carries and PRESENT at `HEAD`, so this corpus is about to hold both quantities in
   this column. A large `seq` says the ARRIVAL COMPARISON answered NO: the
   queued task is YOUNGER than the row at the cursor, so the sequence goes first, and the diff is at that
   comparison. `older` above zero REFUTES both for the steps it counts — the comparison does hand the queue
   the thread ahead of a startable row. `noRow` is the arm reached with no row to compare against at all,
   which on a real page is most of it, and it is the row that says the sequence is not what excludes those
   members.
   NOTHING HERE IS A VERDICT AND THE ARITHMETIC IS NOT RESTATED. `older + noRow == run-a-task` is asserted in
   `engine_ladder_task_census` and read on the installed artifact by site.mjs's `taskArmPartitionDisagrees`,
   which is the release-mode reader of it; this block prints the five OPERANDS and the relation it is entitled
   to claim, so a reader checks the sum by eye against the cell rather than trusting either of us
   (§AN-AUDITOR-DERIVES-THE-RULE — a second copy of that check here is the copy that would drift).
   `=` AND `<=` ARE PRINTED AND NEVER COLLAPSED, because the claim is weaker where the operands are. The four
   come off whichever entry `wfqFrom` names and `run-a-task` off `countersFrom`; `wfqLive`'s backward walk can
   only land at or before the last counted entry, so equal indices make the sum an EQUALITY by construction
   and unequal ones make it a CONTAINMENT over two moments. A cell printing `=` is one sample and a cell
   printing `<=` is two, which is the distinction CLAUDE.md §A-CONSERVATION-IDENTITY-HOLDS-WITHIN-ONE-SAMPLE
   records this corpus having paid three wrong mechanisms for, and it is a property of the PASS rather than of
   the engine.
   FIVE LIFETIME COUNTS AND NO GAUGE AMONG THEM, which is why a SHARE of `run-a-task` is printed for the two
   that partition it and NOT for `deliv` or `seq`: those two are counts of steps the task arm never got, so
   they have no denominator on this cell at all and dividing them by `run-a-task` would be a fraction of the
   wrong population. Nothing is differenced across passes either — these may be differenced by their own
   kind, but two passes are two runs, which is a different comparison and not one this column makes.
   ABSENT IS NOT ZERO AT EITHER GRAIN. `no-field` is a pass predating the four rows, `no-live-frontier` is a
   pass no census of which ever observed a standing frontier (`result_wfq_json` returns `{"members":0}` and no
   term row at all when the frontier is empty, so a run that FINISHED carries none of these), and `-` in a
   cell is one operand absent. A run that reached the arm zero times reads `0`, which result.c states is the
   ladder never having reached it, and that is a finding rather than a silence. */
const tlOne = (m) => {
  /* THREE STATES READ OFF THE VALUE AND NEVER OFF THE KEY — see the mapping, where a key that is always
     assigned made the `in` test this originally used unable to be false. */
  const t = (m.tlad && typeof m.tlad === 'object') ? m.tlad : null;
  if (!t) return { tok: m.tlad === null || m.tlad === undefined ? 'no-field' : String(m.tlad) };
  return { t, oneSample: t.wfqFrom !== null && t.wfqFrom === t.cFrom,
           /* `null` AND NEVER A ZERO where either operand is absent, and never where `run-a-task` is 0 — a
              share of an arm that was never reached is not a small share. */
           olderShare: (t.task && typeof t.older === 'number') ? (100 * t.older / t.task) : null };
};
const tlRows = table.map((t) => {
  const per = t.measurements.map(tlOne);
  const stated = per.filter((x) => x.t);
  /* THE TOKEN IS THE SET OVER THE PASSES AND NOT THE FIRST PASS'S, which is the same separation the
     mapping makes one level down: a site can predate the rows in one pass and have observed no standing
     frontier in another, and `per[0].tok` reports whichever came first as though it were the site's. */
  const toks = [...new Set(per.map((x) => x.tok).filter(Boolean))].sort();
  return { id: t.id, n: per.length, per, stated,
           tok: stated.length ? null : (toks.length ? toks.join('+') : 'no-pass') };
});
const tlShown = tlRows.filter((r) => r.stated.length);
/* THE PASSES THAT COULD NOT BE ASKED, COMPUTED HERE BECAUSE THE GATE BELOW HAS TO BE ABLE TO SEE THEM — the
   mute line used to sit INSIDE `if (tlShown.length)`, so on a census where EVERY row is mute the whole
   section printed nothing: not the header, not the legend, and not the line whose entire purpose is to name
   those rows. That is the absence of a question rendering as a clean bill (CLAUDE.md
   §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN's banding argument, one level in), and it fired on exactly the
   population the line was written for. MEASURED: six passes over two real app pages, every one an engine
   abort before any census observed a standing frontier, and `report.mjs` emitted NO ladder section at all —
   so a reader could not tell this report.mjs from one that does not carry the rows. */
const tlMute = tlRows.filter((r) => !r.stated.length && r.tok);
if (tlShown.length || tlMute.length) {
  const tlIdW = Math.max('site'.length, ...tlShown.map((r) => r.id.length)) + 2;
  const num = (x) => (typeof x === 'number' ? String(x) : '-');
  const cell = (x) => num(x.t.deliv) + 'dl/' + num(x.t.seq) + 'sq  ' +
    num(x.t.older) + '+' + num(x.t.noRow) + (x.oneSample ? '=' : '<=') + num(x.t.task) +
    (x.olderShare === null ? '' : ' (' + x.olderShare.toFixed(0) + '% older)');
  console.log('');
  console.log('WHICH LADDER ARM TOOK THE STEP OF A TASK HOLDER (site, then PER PASS IN ORDER):');
  /* EVERY QUANTITY THE CELL PRINTS IS NAMED HERE, for the span legend's reason: a reader takes a legend as
     the key to a row, so a cell carrying more than the legend names is read as whichever fields the legend
     happens to list. AND IT IS PRINTED ONLY WHERE A CELL WAS, because a legend for cells nobody emitted is
     the shape it exists to prevent — a reader takes it as the key to a row and there is no row. */
  if (tlShown.length) {
  console.log('  <taskHeldDelivLifetime>dl/<taskHeldSeqLifetime>sq  ' +
              '<taskArmOlderLifetime>+<taskArmNoRowLifetime>{=|<=}<run-a-task> (<older> as % of run-a-task)');
  console.log('  `=` is ONE census entry (wfqFrom === countersFrom) and `<=` is TWO, so only `=` cells carry');
  console.log('  the engine\'s asserted equality; `dl` and `sq` are steps the task arm never got and have NO');
  console.log('  denominator here. dl is TWO quantities: before PEND_WORK_SEQ a large dl is the delivery arm');
  console.log('  EXCLUDING the queue, after it a large dl is the arm going in front of a YOUNGER task (the');
  console.log('  order working) and a NEAR-ZERO dl is the old exclusion -- so compare two dl cells only after');
  console.log('  `git grep -c PEND_WORK_SEQ <each pass\'s stamp> -- engine/host` answers the same at both.');
  console.log('  sq large -> the arrival comparison is answering NO; older>0 refutes both for the steps it');
  console.log('  counts; noRow is the arm reached with no row to compare at all. All five are LIFETIME counts.');
  }
  for (const r of tlShown)
    console.log('  ' + pad(r.id, tlIdW) + r.stated.map(cell).join(' | '));
  /* …AND NAMED RATHER THAN OMITTED — a site absent from the column above is either a build predating the
     four rows or a run that never observed a standing frontier, and those are different findings. The
     computation is hoisted above the gate; see the comment there for what nesting it here cost. */
  if (tlMute.length)
    console.log('  not asked: ' + tlMute.map((r) => r.id + '(' + r.tok + ')').join(' '));
}
/* WHICH READING A ZERO DATA-DOOR ROW IS, OVER THE CORPUS — the one discrimination the hard-bar section above
   structurally cannot make, and the question this corpus's own measurement opened. Driven over two real app
   bundles, `endpointDoors` read addresses through `document-script`, `link-element` and `module-import` and
   ZERO through `fetch` or `xhr`; that zero has at least three readings that take OPPOSITE work — the bundle
   names no such call at all, or it names one and the machine never ran, or the machine ran to the door and
   the SURFACE SUPPRESSED the record — and a door histogram alone cannot separate any of them.
   THE LADDER ALONE ANSWERS THE FIRST THREE AND NEEDS NO DOOR COLUMN TO DO IT, which is why this is a
   composition and not the union CLAUDE.md demoted. `…AskNamedLife` is whether the bundle SPELLS the name,
   `…AskBeganLife` whether the machine was ENTERED, `…AskOfferedLife` whether it reached the line BEFORE
   `endpoint_record` at Fetch §5.6 step 12. The FOURTH reading — reached the door and was suppressed — is the
   one thing composed from TWO columns, so it is NOT composed here: read `reached-the-door` BESIDE the doors
   column, because the slack between an offer and a door row IS the suppression and the producer asserts no
   identity between them (asserting one would destroy the measurement with the check meant to guard it).
   BOTH DOORS AND NEVER SUMMED. The fetch ladder read alone IS the misreading solver/endpoint.h names by
   name: a page taken for having reached no network call site when what it reached was XMLHttpRequest, which
   axios's browser adapter IS. The two count states of two DIFFERENT machines whose stages are each their own,
   so there is no total of them and none is printed.
   IT IS A LADDER WITH PARTIAL ENTAILMENT AND NOT FOUR SIGNALS (CLAUDE.md §EVIDENCE-INFLATION). The producer
   asserts NO containment between NAMED and CALLED in EITHER direction — `window.fetch(u)` is a property read
   and a bundle shadowing the name uses a local slot — so the verdict below reads NAMED-or-CALLED as one rung
   and never as two, and a reader counting agreeing rungs as independent confirmations is counting one chain.
   FIVE STATES AND NONE FOLDED INTO ANOTHER, which is the whole reason this is a helper. `no-field` is a pass
   whose site.mjs predates the ladder (the pass channel above says so for the corpus); `no-counters` is a run
   that carried none; `edge-absent` is an artifact whose engine DECLARED NO SUCH EDGE — solver/endpoint.c omits
   a whole edge's rows, the empty string and not five zeroes, because "a host that installs no fetch runs no
   fetch machine, so there is no population" — and `rows-partial` is an artifact mid-way through gaining them.
   Only the remaining four are statements about a PAGE.
   AND `edge-absent` HAS A DISCRIMINATOR THAT NEEDS NO SECOND ARTIFACT, PRINTED RATHER THAN LEFT TO A READER.
   The two edges are declared INDEPENDENTLY, so one door answering numbers while the other reads `edge-absent`
   is a CURRENT artifact whose absent machine was never installed, and BOTH absent is the older artifact. That
   is a fact about the pair and it is composed on the one line that holds both. */
const NET_DOORS = [{ key: 'fetch', pre: 'epFetch' }, { key: 'xhr', pre: 'epXhr' }];
const naOne = (m) => {
  if (!('epNetAsk' in m) || m.epNetAsk === undefined) return { tok: 'no-field' };
  if (m.epNetAsk === null) return { tok: 'no-counters' };
  const o = m.epNetAsk;
  if (!o || typeof o !== 'object' || Array.isArray(o)) return { tok: 'malformed' };
  const doors = {};
  for (const d of NET_DOORS) {
    const rows = Object.keys(o).filter((k) => k.startsWith(d.pre + 'Ask'));
    const nums = rows.filter((k) => typeof o[k] === 'number');
    const g = (suf) => { const v = o[d.pre + 'Ask' + suf + 'Life']; return typeof v === 'number' ? v : null; };
    doors[d.key] = !rows.length ? { tok: 'no-rows' }
      : !nums.length ? { tok: 'edge-absent' }
      : nums.length !== rows.length ? { tok: 'rows-partial' }
      /* AND THE TWO RUNGS THIS CELL USED TO COLLECT INTO `rows` AND THEN NOT READ, which is a narrowing of
         THIS FILE and not of the producer: `startsWith(d.pre + 'Ask')` already pulled them into the
         partial-rows test, so an artifact carrying them was judged on them and a reader was never shown
         them. Neither enters `naVerdict`, deliberately, for the monotonicity reason stated at the property
         spelling below — a rung whose zero can sit under a nonzero one makes `the lowest 0 is the
         localisation` false of the whole ladder. They are PRINTED because each carries a split the producer
         declares load-bearing and no other column here can state. */
      : { named: g('Named'), prop: g('NamedProp'), typeof_: g('NamedTypeof'), called: g('Called'),
          began: g('Began'), placed: g('Placed'), offered: g('Offered') };
  }
  return { doors };
};
/* THE VERDICT FOR ONE DOOR OF ONE PASS, READ DOWN THE LADDER AND STOPPING AT THE HIGHEST RUNG THAT SPOKE —
   which is the lowest-0-is-the-localisation discipline, with NAMED and CALLED as ONE rung for the reason in
   the paragraph above. A rung the artifact did not carry makes the verdict `rows-partial` rather than being
   defaulted to zero, because a missing rung and a zero rung are the two facts this whole section is about. */
const naVerdict = (x) => {
  if (x.tok) return x.tok;
  if (x.offered === null || x.began === null || (x.named === null && x.called === null)) return 'rows-partial';
  if (x.offered > 0) return 'reached-the-door';
  if (x.began > 0) return 'began-never-offered';
  /* THE PROPERTY SPELLING SITS ON THE **NAMED** RUNG AND NOT ON ONE OF ITS OWN, because the rung is a claim
     about what the COMPILER RESOLVED in this page's source and `globalThis.fetch` resolves the same door's
     entry name as a bare `fetch` does — one fact, two spellings. Giving it a rung would make the ladder
     non-monotone: a page that only ever writes the member form would read 0 on a rung BELOW a nonzero one,
     and `the lowest 0 is the localisation` is false of a ladder whose rungs are not entailed. It is NOT
     folded into `named` at the producer either: which spelling a bundle uses is a fact about the bundle, and
     the cell below prints both so a zero on one with a nonzero on the other is readable rather than summed.
     `prop` is absent on an older artifact and is therefore OR'd as 0 rather than making the pass partial —
     the rung's own two operands already decide `rows-partial` above. */
  if ((x.named || 0) > 0 || (x.prop || 0) > 0 || (x.called || 0) > 0) return 'named-never-began';
  return 'never-named';
};
const naPredates = passes.filter((p) => p.netAsk === 'predates').map((p) => p.label);
const naCarried = passes.filter((p) => p.netAsk === 'carried').map((p) => p.label);
if (naPredates.length)
  console.log('\n*** THE DATA-DOOR SECTION BELOW IS OVER ' + naCarried.length + ' OF ' + passes.length +
    ' PASS(ES) — ' + naPredates.join(', ') + ' predate(s) the ask ladder entirely (their rows carry no ' +
    '`netDoorAsk`), so a site reading `no-field` over those passes is this instrument being unable to ask. ' +
    'It is NOT a page that reached no network call, and it is NOT an artifact whose engine declared no such ' +
    'edge — that one is `edge-absent` and is a fact about the BUILD. ***');
const naRows = table.map((t) => {
  const per = t.measurements.map(naOne);
  const stated = per.filter((x) => x.doors);
  /* ONE PASS PER SITE AND PER DOOR: the HIGHEST rung any pass reached, because these are LIFETIME counts and
     a pass that got further is a pass that saw more — never an average, and never a sum across passes, which
     would add two runs' margins and belong to no moment. Ties keep the EARLIEST such pass. */
  const best = {};
  for (const d of NET_DOORS) {
    const order = ['never-named', 'named-never-began', 'began-never-offered', 'reached-the-door'];
    let b = null;
    for (const x of stated) {
      const v = naVerdict(x.doors[d.key]);
      const r = order.indexOf(v);
      if (r < 0) continue;
      if (!b || r > b.rank) b = { rank: r, verdict: v, cell: x.doors[d.key] };
    }
    best[d.key] = b;
  }
  return { id: t.id, n: t.measurements.length, per, stated: stated.length, best };
});
const naShown = naRows.filter((r) => r.n > 0);
/* THE CELL PRINTS A UNIT BOUNDARY AND NOT FIVE SLASH-SEPARATED NUMBERS, WHICH IS A REPAIR AND NOT A STYLE
   CHOICE — this cell read `92/4/11/11/11`, and five numbers in one bracket IS a ladder to a reader, so it was
   taken for `92 sites of which 11 were called` and the 81 for a reach gap. solver/endpoint.c forbids exactly
   that, in its own capitals at the field: "IT IS A FLOOR OVER ONE SPELLING AND NO CONTAINMENT WITH `called`
   MAY BE ASSERTED IN EITHER DIRECTION, which is not a caution but the reason there is no `DCHECKF` under these
   two rows where there is one under every other pair in this file. `called > named` is ORDINARY … `named >
   called` is ordinary too … this counts COMPILER RESOLUTIONS and never source sites — it is read as a BIT and
   never as a magnitude."
   SO THE SUBTRACTION IS NOT A QUANTITY AND THE CELL NOW SAYS SO WHERE IT IS READ. The banner above already
   stated the partial entailment and the VERDICT already honours it (NAMED-or-CALLED is one rung), and neither
   reached a reader of the OUTPUT: a banner lives in this file and a legend is what travels. That is this file's
   own legend rule — "a reader takes a legend as the key to a row, so a cell carrying more than the legend
   names is read as whichever of its fields the legend happens to list" — and the legend named five rungs.
   MEASURED COST, BY THE AUTHOR OF THE CELL: the `92` and the `11` were carried into three separate task
   records and into one coordinator reading as a reach gap of 81 `fetch` call sites, which is a magnitude of a
   row declared to be a bit, over a denominator that is resolutions rather than sites. Nothing downstream
   contradicts such a reading, because both numbers are real and both are correctly measured.
   WHAT IS ASSERTED AND WHAT IS NOT, so the `>=` is a claim and not decoration. `called >= began` is asserted by
   a `DCHECKF` at BOTH edges, and this prints the violation because `-DAPICLIENT_DEV=0` compiles that assert out
   and this driver measures whatever artifact is installed. `offered` gets NO `>=`: at the fetch edge it is that
   edge's own completed-construction row and is a subset of `began` by its field's own words, while at the xhr
   edge it is a SEPARATE counter whose asserted relation is to the whole surface's ask population and not to
   `began` — one spelling for two relations would be the drifting claim, so neither is claimed. */
const naCell = (x) => {
  if (x.tok) return x.tok;
  const num = (v) => (v === null ? '-' : String(v));
  return NET_DOORS.map((d) => {
    const c = x.doors[d.key];
    if (c.tok) return d.key + '=' + c.tok;
    /* THE ONE ASSERTED RELATION ON THIS CELL, READ IN RELEASE WHERE THE `DCHECKF` IS ABSENT. A violation is
       printed rather than silencing the cell, because the other terms are still facts. */
    const broke = (typeof c.called === 'number' && typeof c.began === 'number' && c.began > c.called)
      ? '!began>called ' : '';
    return d.key + '=' + naVerdict(c) + '(' + broke +
      'res ' + num(c.named) + '/' + num(c.prop) + '/' + num(c.typeof_) + ' \u2016 call ' + num(c.called) +
      '\u2265' + num(c.began) + ' plc ' + num(c.placed) + ' off ' + num(c.offered) + ')';
  }).join(' ');
};
if (naShown.length) {
  console.log('\nTHE TWO DATA DOORS\' ASK LADDER OVER ' + naCarried.length + '/' + passes.length +
    ' PASS(ES) — WHICH READING A ZERO `fetch` OR `xhr` DOOR ROW IS.\n' +
    '  THE CELL IS  res <named>/<prop>/<typeof> \u2016 call <called>\u2265<began> plc <placed> off <offered>\n' +
    '  AND THE \u2016 IS A UNIT\n' +
    '  BOUNDARY. Left of it the engine counts COMPILER RESOLUTIONS of the entry name — a program is recompiled\n' +
    '  by every flow that replays it, so those are read as a BIT (zero against nonzero) and NEVER as a\n' +
    '  magnitude, and `prop` is the SAME rung as `named` (`globalThis.fetch` resolves the same entry name),\n' +
    '  printed apart because which spelling a bundle uses is a fact about the bundle. Right of it are stages\n' +
    '  of ONE CALL.\n' +
    '  `typeof` AND `plc` ENTER NO VERDICT AND ARE THE TWO RUNGS THIS CELL USED TO DROP. solver/endpoint.c\n' +
    '  calls `named_typeof` the discriminator that keeps the finding honest: quickjs patches an ordinary\n' +
    '  read into the non-throwing form only for `typeof`, so a bundle that merely PROBES for an entry and\n' +
    '  uses something else raises that row and NEITHER of the two beside it \u2014 which means a probe-only\n' +
    '  page reads `never-named` on this ladder and `typeof` is the only column that says otherwise. It is NOT\n' +
    '  every guard shape: a `window.X` and an `in` test are property reads and reach none of the three.\n' +
    '  `plc` IS THE XHR EDGE\u0027S OWN MIDDLE STAGE and is why that edge has SIX rows where fetch has five\n' +
    '  \u2014 solver/endpoint.h: core/fetch OFFERS inside the machine that CONSTRUCTS, while XMLHttpRequest\n' +
    '  splits that across send() and the lifecycle machine it mints at XHR \u00a73.5.6 step 12/13, so `plc` is\n' +
    '  row that separates A REQUEST THAT WAS BUILT from AN ADDRESS THAT REACHED THE SURFACE. A cell reading\n' +
    '  `call N\u2265N plc N off 0` is a page whose every request was constructed and whose every address was\n' +
    '  lost at the task hop, and WITHOUT `plc` that is the same text as a page whose sends all died inside\n' +
    '  send(). NO CONTAINMENT IS CLAIMED FOR EITHER, which is why neither carries a `\u2265`:\n' +
    '  `placed <= began` is FALSE at this edge (a step state is BYTE-COPIED at a deep fork and the copy\n' +
    '  inherits the capture flag) and `typeof` is on no ladder at all.\n' +
    '  AND A ZERO `xhr` DOOR OVER THIS CORPUS IS A DECISION AND NOT A GAP, WHICH THE PARTITION ON THE TOTALS\n' +
    '  LINE BELOW IS WHAT MAKES CHECKABLE. The one apps.tsv row MEASURED to reach this door --\n' +
    '  app.slack.com, at called/began/placed 379 and offered ZERO, localised to the task hop in\n' +
    '  solver/endpoint.h\u0027s own words -- has NEVER produced a census row, so this corpus zero is not evidence\n' +
    '  about the XHR door at all: it is evidence that the pages this corpus can measure construct none.\n' +
    '  THE REASON IS ABOUT XHR AND REACHES NEITHER `fetch` NOR THE XHR DOOR IN GENERAL. A reader dispatching\n' +
    '  at `xhrReachedTheDoor: 0` is dispatching at the wrong page; the edge\u0027s own paragraph carries the\n' +
    '  retirement condition for the real loss.\n' +
    '  NO CONTAINMENT HOLDS ACROSS THE \u2016 IN EITHER DIRECTION — solver/endpoint.c: `called > named` is\n' +
    '  ORDINARY (`window.fetch(u)` is a property read and a shadowed parameter is a local slot) and so is\n' +
    '  `named > called`. SO `named - called` IS NOT A QUANTITY: a cell reading `res 92/4/0 \u2016 call 11` is not\n' +
    '  81 of anything, and this cell was read that way into three task records before the bar was printed.\n' +
    '  `\u2265` IS THE ONE ASSERTED RELATION (a `DCHECKF` at both edges, compiled out in release, so a\n' +
    '  `!began>called` prefix is this cell reading it back). `off` carries no `\u2265` deliberately: at the\n' +
    '  fetch edge it is that edge\'s completed-construction row and at the xhr edge it is a separate counter\n' +
    '  whose asserted relation is to the whole surface\'s ask population, so neither relation is claimed here.\n' +
    '  `-` is a rung the artifact did not carry. `reached-the-door` BESIDE a doors column with no such key is\n' +
    '  the SURFACE having suppressed the record; the two are not in an identity and are not summed.');
  const naIdW = Math.max('site'.length, ...naShown.map((r) => r.id.length)) + 2;
  console.log('  ' + pad('site', naIdW) + 'PER PASS IN ORDER');
  for (const r of naShown) console.log('  ' + pad(r.id, naIdW) + r.per.map(naCell).join(' | '));
}
/* WHY THE `xhr` DOOR READING ZERO OVER THIS CORPUS IS A DECISION, AND EXACTLY WHAT THE REASON COVERS. It is
   recorded because this row had neither a gap nor a paragraph and a coordinator reading the totals line below
   nearly dispatched a lane at it; CLAUDE.md §AND-THE-MIRROR-OF-IT-IS-A-ROW-THAT-IS-DELIBERATE is the rule and
   the one sentence of this that reaches the OUTPUT is in the banner above, because a source comment is read by
   somebody editing this file and the zero is read by somebody holding a work queue.
   WHAT THE READING IS NOT. `called 0` beside a nonzero `res` is NOT `named > 0 && called == 0`'s interesting
   state here, and the four readings a zero door could be -- never named; named in dead code a browser also
   never runs; named in live code a browser runs and this engine does not reach; named, reached and refused --
   are separated by OPENING THE BYTES rather than by any column. Derived with the tree's own instruments,
   never by eye, over `engine/.work/sitecorpus/mirror` (the fetched corpus is untracked by design, so what is
   handed over is the DERIVATION): a fixed-string walk keyed on the IDENTIFIER and never on a receiver, then
   `engine/js_guard_shape.mjs`'s `guardShapeReader()` -- 73 armed controls, plus 53 for the member reader --
   asked for a verdict at every occurrence's own string offset.
   IT RECONCILES TO THE DIGIT, RUNG BY RUNG, WHICH IS WHAT MAKES THIS A MEASUREMENT RATHER THAN AN ARGUMENT.
   The dominant site's bundle holds ELEVEN textual occurrences across TWO files, and every one is accounted
   for: SEVEN are free-identifier reads, of which SIX are ordinary (`res <named>`) and ONE is a `typeof` probe
   (`res <typeof>`); ONE is a `self.`-spelled global property (`res <prop>`); TWO are members of a MODULE
   NAMESPACE, which no rung sees because the receiver is not the global object and which the guard reader
   gives no verdict to by design; and ONE is inside a TextMate grammar's regex STRING and is not a use at all.
   So the cell's left-hand numbers are not a site count and they are not a floor either -- they are the whole
   free-identifier population of that bundle, which is why the BIT discipline above costs nothing here.
   AND NOT ONE OF THE CONSTRUCTIONS IS ON A DOCUMENT-LOAD PATH, which is the answer to `would a real browser
   construct one`. Two are a media player's (an EME licence request, and its loader's `new self.X`), reached
   only by playing HLS; TWO are the XHR arm of a `typeof fetch === 'function' ? fetch.bind() : <XHR>` polyfill,
   so they are dead in any engine that HAS `fetch` and this one does; ONE is inside a `try` whose handler the
   reader reports as `caught`; and the module-namespace pair sits behind a presence test after a
   fetch-keepalive path. A browser on that logged-out document constructs none either, so the engine AGREES
   with it and the right next diff for this row is NOTHING.
   WHAT THIS DOES NOT COVER, STATED BECAUSE A REASON ABOUT XHR DOES NOT REACH ITS NEIGHBOUR. It says nothing
   about `fetch`, whose own zero on the same pages is a different question with a different bundle population.
   It says nothing about the XHR door in general -- solver/endpoint.h measures that door REACHED 379 times on
   a page this corpus has never measured, and its loss is one task hop further on. And it says nothing about a
   site appended to apps.tsv later: the derivation is per bundle, so a new row owes it again.
   NAMED RESIDUAL -- CORRECT AND NARROWER, AND IT IS THIS FILE'S VERDICT WORD RATHER THAN THE PRODUCER'S ROWS.
   WHAT IS NOT COVERED: a page that names a door ONLY as a member of a NON-GLOBAL receiver raises no rung at
   all, so it reads `never-named` -- and the banner above calls `never-named` a statement about a PAGE, which
   for that population it is not. The producer's exclusion is DELIBERATE and read rather than assumed:
   solver/concolic.c's `compile_global_member_dispatch` returns early unless the base is one of four source
   spellings of the global, saying in its own words that a wrapper's member and a bundler re-export shim
   `would raise a denominator neither census owes`; and quickjs reports `<free identifier>.<member>` ONE field
   deep, so the tail of a two-deep chain reaches no hook whatever the receiver is. Both are right where they
   are, which is why the narrowing is here.
   WHAT THE NEXT DIFF BUILDS: a verdict this file can distinguish -- `no rung rose` apart from `this page
   spells the entry somewhere no rung can see` -- which needs ONE row this ladder does not yet get, a count of
   the entry name reported at a receiver the dispatch declined, raised beside the three it already reports and
   consumed here as its own token. Until that row exists the distinction is not computable from a census,
   because the only other operand is the page's bytes and those are not this instrument's to read.
   HOW ITS ABSENCE WOULD SHOW: a site whose cell reads `never-named` for a door while a fixed-string walk of
   that site's own mirrored bundle, keyed on the entry IDENTIFIER and never on a receiver, answers nonzero.
   Observed by running that walk over `engine/.work/sitecorpus/mirror/<id>` beside the site's row -- the
   corpus is untracked, so this is an observation a reader takes and never a column this file can print.
   RETIREMENT: this record goes when a `never-named` cell in this ladder cannot be printed for a door whose
   entry name the producer reported at a declined receiver. */
/* AND THE CORPUS FIGURE, WITH EVERY SHARE'S DENOMINATOR ON THE SAME LINE. A count over a corpus that does not
   say how many passes it is over, and over how many sites could be asked at all, belongs to a population a
   reader cannot name — and the two doors are counted SEPARATELY for the reason above. */
console.log('data-door totals: ' + JSON.stringify(Object.assign({
  passes: passes.length, passesCarryingTheField: naCarried.length,
  sitesStatingALadder: naRows.filter((r) => r.stated > 0).length,
  /* AND THE ONE DENOMINATOR THE THREE ABOVE CANNOT STATE: HOW MANY LADDER READINGS EACH SITE CONTRIBUTED.
     `sitesStatingALadder` counts SITES and every door share below is a count of SITES, so a reader takes
     `xhrReachedTheDoor: 0` of 7 for a statement about seven pages — and it is not one, because a site's
     verdict is a MAXIMUM over its own passes and the passes are not distributed evenly across sites. A
     corpus whose readings are nine tenths one page states a door's verdict for that page and carries six
     other pages at one reading each; CLAUDE.md §a-coverage-figure-states-what-it-is-a-fraction-of is the
     rule and this is its operand. It is a PARTITION and never a share, so `ladderRows` is printed beside it
     and the per-site counts SUM to it — an identity a reader checks on this line without running anything,
     which is the whole reason the breakdown is here rather than in a second command.
     IT IS NOT THE SAME QUANTITY AS `passesCarryingTheField`, and the two are printed together so neither
     can be read as the other: that one counts PASSES whose rows carry `netDoorAsk` at all, and this counts
     (site, pass) readings that actually stated a ladder. A pass can carry the field and state no ladder for
     a site it did not measure. */
  ladderRows: naRows.reduce((a, r) => a + r.stated, 0),
  ladderRowsPerSite: Object.fromEntries(naRows.filter((r) => r.stated > 0)
    .sort((a, b) => b.stated - a.stated).map((r) => [r.id, r.stated])),
}, ...NET_DOORS.map((d) => ({
  [d.key + 'ReachedTheDoor']: naRows.filter((r) => r.best[d.key] && r.best[d.key].verdict === 'reached-the-door').length,
  [d.key + 'NeverNamed']: naRows.filter((r) => r.best[d.key] && r.best[d.key].verdict === 'never-named').length,
  [d.key + 'EdgeAbsentEverywhere']: naRows.filter((r) => r.stated > 0 && !r.best[d.key]).length,
})))));
/* THE PAIR FACT, WHICH NO PER-DOOR COLUMN ABOVE CAN STATE — AND IT IS ASKED PER PASS, NEVER OF THE BEST PASS.
   One door answering numbers while the other reads `edge-absent` is a CURRENT artifact whose absent machine was
   never installed; BOTH absent is an older artifact. Both are facts about ONE ARTIFACT, so they are read within
   one pass: a site's `best` is a maximum over passes, and asking the pair of it compares two edges observed in
   two different builds, which is the cross-sample comparison CLAUDE.md
   §A-CONSERVATION-IDENTITY-HOLDS-WITHIN-ONE-SAMPLE forbids and which the paragraph above already cites about
   this very ladder. THE FIRST VERSION OF THIS BLOCK DID EXACTLY THAT and is recorded rather than quietly
   repaired: it read the pair off `r.best`, so a corpus holding ONE pass whose fetch edge reached the door made
   `best.fetch` non-null and the split went UNREPORTED for every other pass of that site — the line printed
   nothing at all on a corpus deliberately built to make it speak. A reader who re-derives the aggregation from
   the per-door totals beside it will reach for `best` again, which is why the reason is here and not just the
   fix.
   IT IS PRINTED ONLY WHEN IT HAS SOMETHING TO SAY, because a line that reads `none` on every clean run is
   furniture that buries the next real thing under it; and it names the PASS as well as the site, since which
   build said so is the whole content of a claim about a build. */
{
  const naPairs = [];
  naRows.forEach((r) => r.per.forEach((x, i) => {
    if (!x.doors) return;
    const absent = NET_DOORS.filter((d) => x.doors[d.key].tok === 'edge-absent');
    if (!absent.length) return;
    naPairs.push({ id: r.id, pass: passes[i] ? passes[i].label : '#' + i,
                   all: absent.length === NET_DOORS.length,
                   which: absent.map((d) => d.key).join('+') });
  }));
  const split = naPairs.filter((x) => !x.all), bothOut = naPairs.filter((x) => x.all);
  if (split.length)
    console.log('  ONE EDGE DECLARED AND THE OTHER NOT, on a CURRENT artifact — that machine was never ' +
      'installed, which is a fact about the BUILD and not about the page: ' +
      split.map((x) => x.id + '@' + x.pass + '(' + x.which + ')').join(' '));
  if (bothOut.length)
    console.log('  NEITHER EDGE DECLARED — that artifact predates both row sets, so the site is UNASKED ' +
      'rather than silent in that pass: ' + bothOut.map((x) => x.id + '@' + x.pass).join(' '));
}
console.log('totals: ' + JSON.stringify({
  sites: table.length,
  netFixture: table.filter((t) => t.outcome === 'NET/FIXTURE').length,
  measurable: measurable.length,
  /* AND THE ROWS BEHIND THAT DENOMINATOR, ON THE SAME LINE, BECAUSE THE FOUR FIGURES UNDER IT ARE READ AS
     FACTS ABOUT THIS CORPUS AND ARE FACTS ABOUT WHATEVER SURVIVED AN ANY-PASS QUANTIFIER. `outcome` is the
     WORST SEEN -- `ms.some(...)`, which is the right answer for a COLUMN that must not report a site aborting
     in two passes of three as one that runs -- and `measurable` then inherits that quantifier for a DIFFERENT
     question, so ONE net or nav failure in ONE pass drops a site out of every figure below whatever its other
     passes did. THE TWO HALVES ARE WILDLY UNEQUAL IN WEIGHT AND NOTHING SAID SO: the sites this keeps carry a
     handful of rows and the sites it drops carry nearly all of them, so `cleanEveryPass` reads as a statement
     about the corpus and is a statement about a few single-pass sites. No count anywhere could have said that,
     because the only row total printed here is over the WHOLE table.
     THE NUMBER IS NOT RELOCATED AND THE QUANTIFIER IS NOT CHANGED, which is the half a reader will reach for
     and the half that would cost something. Re-keying `measurable` to a per-pass test gives these four figures
     a DIFFERENT MEANING and leaves the old one unread -- a site that aborts in a third of its passes is not a
     site that runs, and the column above exists to say so. What was missing is the DENOMINATOR, and a fraction
     whose numerator is published and whose denominator is not is the whole of the defect. A reader holding both
     counts cannot take the one for the other.
     RETIREMENT: this pair goes when the totals object is emitted by a helper that takes a PARTITION and refuses
     to print a count over one side without the rows behind both, so a site-level denominator cannot be
     published bare at all. MEASURED ABSENT with the command, so this condition is not born met: `grep -c` over
     this file answers 0 for each of `partitionWithRows`, `emitPartition` and `rowsBehind`, against `rowsMeasured`
     answering 1 as the armed control and an invented token answering 0. */
  rowsBehindMeasurable: measurable.reduce((a, t) => a + t.n, 0),
  rowsBehindNetFixture: table.filter((t) => t.outcome === 'NET/FIXTURE').reduce((a, t) => a + t.n, 0),
  everAborted: measurable.filter((t) => t.abortedPasses > 0).length,
  abortedEveryPass: measurable.filter((t) => t.abortedPasses === t.n).length,
  cleanEveryPass: measurable.filter((t) => t.abortedPasses === 0).length,
  finishedEveryPass: measurable.filter((t) => t.finishedPasses === t.n).length,
  /* THE BUDGET BESIDE THE COUNT, BECAUSE `finishedEveryPass` IS A FRACTION WHOSE DENOMINATOR IS A WALL-CLOCK
     CUT AND HAD NEVER BEEN PRINTED. `0` here is read as a statement about the ENGINE and it is a statement
     about the engine AND the dwell together, so the pair of extremes goes on the same line: a reader who sees
     the span is 1 knows this corpus cannot say whether a longer drive would move it, and one who sees it wide
     knows a longer drive was tried. `spanned` counts SITES whose dwells-with-runs are not all equal -- the
     only cells a within-site comparison can be built from -- and `completeAtAnySpannedDwell` is the claim.
     IT IS NOT A VERDICT ON WHY A `partial` STAYED ONE. See the WOULD-A-LONGER-DWELL section for which of the
     two reasons this does and does not reach; a stratified count of an outcome reaches neither on its own. */
  rowsMeasured: table.reduce((a, t) => a + t.n, 0),
  rowsCarryingTheDwell: table.reduce((a, t) =>
    a + t.measurements.filter((m) => typeof m.dwell === 'number').length, 0),
  dwellSecondsSeenAcrossThePairedSet: [...new Set(table.flatMap((t) =>
    t.measurements.map((m) => m.dwell).filter((d) => typeof d === 'number')))].sort((a, b) => a - b)
    .map((d) => d / 1000),
  sitesSpannedByMoreThanOneDwellWithRuns: table.filter((t) =>
    new Set(t.measurements.filter((m) => typeof m.dwell === 'number' && m.runs > 0)
      .map((m) => m.dwell)).size > 1).length,
  completeAtAnySpannedDwell: table.filter((t) =>
    new Set(t.measurements.filter((m) => typeof m.dwell === 'number' && m.runs > 0)
      .map((m) => m.dwell)).size > 1)
    .reduce((a, t) => a + t.measurements.reduce((b, m) => b + (m.outc.complete || 0), 0), 0),
  sitesReportingAnEndpointCount: withEp.length,
  sitesLearningAtLeastOneEndpoint: withEp.filter((t) => t.epMax > 0).length,
  endpointsTotalBestPass: withEp.reduce((n, t) => n + Math.max(0, t.epMax), 0),
}));
