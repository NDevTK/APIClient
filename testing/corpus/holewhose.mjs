// WHOSE UNKNOWN PAYS §What-the-tool-produces' BAR. One row per site: how many of its emitted addresses the
// razor graded `runtime-only`, and how many of its HOLE-BEARING addresses name a hole the PAGE read against
// one THIS ENGINE MINTED so that a drive of a never-called function could happen at all.
//
// IT EXISTS BECAUSE solver/endpoint.c's `address_class_of` CARRIES THAT CLAIM AS A RESIDUAL, AND A
// NOT-COVERED CLAUSE IS A HYPOTHESIS UNTIL SOMETHING RUNS. CLAUDE.md grades such a clause by its SUBJECT:
// about a document anyone can fetch it is evidence, and about THIS TREE it is "a hypothesis wearing the one
// grammatical position this file grades as evidence". That residual's clause is about this tree, so it is
// owed a derivation, and this is the derivation. Its own HOW-ITS-ABSENCE-WOULD-SHOW clause names the
// observation: "a run whose razor reads `runtime-only` for rows whose `addressRoot` names a source no
// document and no server supplied".
//
// THIS READS THE `url`'S BRACES AND NOT THE ROOT, WHICH IS THE AXIS THAT RESIDUAL REFUTES, AND IT SAYS SO PER
// ROW RATHER THAN IN THIS BANNER ALONE. The per-record `addressRoot` key is the right axis and no capture in
// this corpus carries it -- `grep -l addressRoot census-*.jsonl` answers five files and in every one it is a
// NOTE somebody wrote, not a row -- so against the artifacts on disk the braces are the only route, which is
// what that residual already says of its own fallback clause. The two axes CANNOT DISAGREE for an address
// that is WHOLLY ONE BRACE, because then the composed identity IS the single root; they can for a composed
// one, where `origin + "/" + <an orphan argument>` is a page-composed string around an engine mint. So the
// WHOLLY column is printed beside the count, and a reader who wants the strong claim reads that column.
//
// THE CLASSIFIER IS A SPELLING AND IS NOT THE DISCRIMINATOR THE ENGINE SHOULD USE. `{orphan<hex>.argN}` is
// what solver/engine.c's orphan drive happens to name its arguments today, and CLAUDE.md §RUN-DON'T-MATCH
// plus that residual's own text ("It is NOT a prefix match on `{orphan`: that is a count of a spelling")
// forbid keying the ENGINE on it. It is admissible HERE for one reason: this is a reader over captures that
// already exist, where no mark can be added retroactively, and its answer is a claim about those captures
// and never a branch in the product. The day the mint states whose source it is, this file reads THAT.
//
// THE INPUT IS UNTRACKED BY DESIGN -- other people's bundles are not this project's to commit -- so the
// FIGURES THIS PRINTS ARE NOT REPRODUCIBLE FROM A CLONE and the derivation is the artifact. The driver that
// PRODUCES the input is named in the throw below, which is the pairing CLAUDE.md asks of an instrument whose
// population it does not carry.
//
// BARE DERIVES ITS OWN POPULATION; `--named <files>` says the population is the reader's. That is report.mjs's
// split and for its reason: a placeholder for a population is a hand-chosen scope wearing a derivation's
// output format.
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const HERE = new URL('.', import.meta.url).pathname;
const argv = process.argv.slice(2);
const namedAt = argv.indexOf('--named');
const named = namedAt >= 0 ? argv.slice(namedAt + 1) : null;

if (named && named.length === 0) {
  console.error('holewhose: --named with no files states a population and names none. Either pass the files,');
  console.error('           or run BARE, which derives the population from every census beside this file.');
  process.exit(1);
}
if (!named && argv.length) {
  console.error(`holewhose: refusing a filename without --named. BARE derives its own population; a named`);
  console.error(`           set says the population is yours. You meant one of:`);
  console.error(`             node testing/corpus/holewhose.mjs`);
  console.error(`             node testing/corpus/holewhose.mjs --named ${argv.join(' ')}`);
  process.exit(1);
}

const files = named
  ? named.map(f => (f.includes('/') ? f : join(HERE, f)))
  : readdirSync(HERE).filter(f => /^census-.*\.jsonl$/.test(f)).sort().map(f => join(HERE, f));

if (!files.length) {
  console.error('holewhose: no census is present, so there is nothing to read. The censuses are UNTRACKED by');
  console.error('           design and a clone has none; produce them with the driver that takes them:');
  console.error('             node testing/live-run.js            (one site, writes a census beside this file)');
  console.error('           and then run this BARE. An empty reading is not a clean bill.');
  process.exit(1);
}

/* THE UNIT IS THE RECORD AND NOT THE FILE, which is the one thing this reader had wrong and the reason it is
   stated here rather than left to the loop's shape. A census FILE holds one JSON record PER PASS, and a
   driver run puts SEVERAL SITES into one file, so a scrape over the file TEXT answers about whichever site's
   bytes happened to match -- CLAUDE.md §AND-A-SCRATCH-DIRECTORY-HOLDS-RUNS-OF-SEVERAL-SUBJECTS arriving
   inside a committed reader rather than inside a shell grep, where it needs no `-h` to throw the provenance
   away because a file-wide match never carried one.
   MEASURED at the revision that landed this, with the per-record parse as the control. `census-ord1.jsonl`
   holds FIVE sites, and the file-scoped reader published `https://squoosh.app/ ... endpoints=29` -- squoosh's
   url carrying CODESANDBOX's endpoint count, because the url was the FIRST match in the file and the
   histograms were the LAST; squoosh's own record reads 10. `excalidraw.com`, `astexplorer.net` and
   `codesandbox.io` appeared in NO row at all although each has a scorable record, with `vscode.dev` as the
   ARMED CONTROL: it did appear, because its captures are one site per file. `census-razorgp.jsonl` holds
   gitpod then gitlab, so `gitlab` was published at FIVE passes where SIX records carry it -- and gitpod lost
   its OWN 190-endpoint record there, which the file-scoped read replaced with gitlab's 48.
   THE DISCRIMINATING PER-PASS TABLE BELOW -- the one this file says is where no unit is crossed -- IS WHERE
   IT MANUFACTURED A FINDING RATHER THAN LOSING ONE, and the instance is `census-hostowed.jsonl`: it holds
   8931 then 8943, and the file-scoped read published the 8943 record's `endpoints: 1` AND its
   `{"unproven":1}` under the 8931 url, discarding 8931's own `{"unproven":6,"runtime-only":1}`. So that
   pass read razor=0 while holding a page hole, and this reader's LAST line then reported `1 pass(es) hold a
   PAGE hole while their razor reads ZERO -- a hole that reached no address, which is a DIFFERENT finding`,
   about nothing at all. The per-record read puts that pass in the nonzero band and the line goes away.
   AND THE INSTANCE THIS COMMENT FIRST NAMED WAS THE WRONG SIBLING, which is recorded because the METHOD is
   the finding and the row is only its symptom. It said `census-reqsplit.jsonl`, reasoned from the file's
   SHAPE -- a two-site file must carry its second site's razor -- and reqsplit's 8943 record PREDATES the
   razor key and carries no `endpoints` either, so the terminal match found only the 8931 record and that
   file was never affected. `census-hostreg.jsonl` likewise. The defect is LIVE in three files and LATENT in
   two, and which is which is a fact about what the NEIGHBOURING record happens to carry -- so it was
   derived by running the old extraction beside the new one per record, never argued from the file's shape.
   THE RETIRED ARGUMENT IS KEPT BECAUSE A READER RE-DERIVES IT. A `terminal()` helper took the LAST match in
   file order, and its reason was exactly right about a SERIES: "a census is a SERIES and its first line is
   composed before anything has happened, so `sort` would both reorder it and -- being LEXICAL -- rank 101
   below 6." Both halves still hold of any series this reader grows, and the parse below preserves file order
   for that reason. What the argument could not express is that the SERIES axis and the SUBJECT axis are the
   same lines, so reading the last one is reading the last SITE.
   AND IT WAS A LINE MATCH WHERE A PATH BELONGS, which is a SECOND and LATENT defect of the same lines: one
   record holds up to FOUR `"endpointRazorClass"` occurrences -- the top-level field and a nested `perRun[]`
   copy -- so even on a one-site file the terminal match read the per-run record rather than the field
   (CLAUDE.md §AND-THE-RIGHT-FILE-CAN-STILL-ANSWER-FROM-THE-WRONG-RECORD). MEASURED over all 88 scorable
   records, 40 of which carry a nested copy: the two agree 88 of 88, so that one has never yet cost a figure.
   That is LUCK and is exactly why the read is by PATH now rather than by position. */
const sum = o => (o && typeof o === 'object' ? Object.values(o).reduce((a, b) => a + b, 0) : null);
const rows = [], skipped = [];
let read = 0;
for (const p of files) {
  const base = p.replace(/^.*\//, '');
  let txt;
  try { txt = readFileSync(p, 'latin1'); }
  catch (e) { skipped.push([base, `unreadable: ${e.code || e.message}`]); continue; }
  /* `latin1` AND NOT `utf8`, DELIBERATELY, and it is the hole scrape's requirement rather than the parse's:
     a raw byte above 0x7F stays ONE character instead of becoming a replacement, which keeps an address
     spelling intact. Every field read by PATH below is ASCII and JSON forbids only U+0000..U+001F
     unescaped, so the parse is unaffected -- and a record that does not parse is BANDED with that reason
     rather than skipped, because a reader that drops a record it cannot read reports a smaller population
     and reads as a cleaner one. */
  const lines = txt.split('\n').filter(l => l.trim());
  lines.forEach((line, i) => {
    const at = `${base}#${i}`;
    let o = null;
    read++;
    try { o = JSON.parse(line); } catch { o = null; }
    if (!o || typeof o !== 'object') {
      skipped.push([at, `does not parse as one JSON record -- NOT SCORABLE (${line.length} byte(s))`]);
      return;
    }
    const url = typeof o.url === 'string' ? o.url : '?';
    if (!o.endpointRazorClass) {
      skipped.push([`${at} ${url}`,
                    'carries no `endpointRazorClass` -- taken before that key existed, so it is NOT ' +
                    'SCORABLE on this axis rather than a capture whose razor claimed nothing']);
      return;
    }
    const rz = o.endpointRazorClass, ad = o.endpointAddressClass, eps = o.endpoints;
    /* THE ARITHMETIC THAT SAYS A CAPTURE IS SCORABLE IS FREE AND IS IN THE DOCUMENT: both histograms SUM to
       its own `endpoints`. A capture that fails it is BANDED and never averaged in -- an absent histogram
       read as a row of zeroes scores it as a capture whose razor claimed nothing. */
    if (!ad || !Number.isFinite(eps) || sum(rz) !== eps || sum(ad) !== eps) {
      skipped.push([`${at} ${url}`,
                    `its two histograms do not both sum to its own \`endpoints\` (${eps}): ` +
                    `addr=${sum(ad)} razor=${sum(rz)} -- NOT SCORABLE`]);
      return;
    }
    /* THE SCRAPE IS OVER THIS RECORD'S OWN LINE, which is the whole of the repair: the pattern is unchanged
       and only its SUBJECT is, so a hole pooled onto a neighbouring site's url is unspellable rather than
       merely avoided. */
    const holes = [...new Set(line.match(/"[A-Z]+ [^"]*\{[^"]*"/g) || [])];
    const cls = holes.map(h => {
      const addr = h.replace(/^"[A-Z]+ /, '').replace(/"$/, '');
      return { h, addr,
               orphan: /\{orphan[0-9a-f]+\./.test(addr),
               wholly: /^\{[^{}]*\}$/.test(addr) };   /* the address is ONE brace and nothing else */
    });
    rows.push({ base, at, url, eps, ro: rz['runtime-only'] || 0, cls });
  });
}

console.log(`## the population, DERIVED${named ? ' -- NO, NAMED: this set is the reader\'s' : ''}`);
/* THE NUMERATOR AND THE DENOMINATOR HAVE THE SAME ROW, which this line used to get wrong by printing a
   RECORD count over a FILE count as though it were a share of itself (CLAUDE.md
   §a-coverage-figure-states-what-it-is-a-fraction-of). The file count is still printed, as a separate fact
   about where the records came from, and never as something the scorable count is a fraction of. */
console.log(`   ${rows.length} scorable of ${read} record(s), read from ${files.length} census ` +
            `file(s) beside this reader`);
for (const [b, why] of skipped) console.log(`   EXCLUDED ${b}: ${why}`);
console.log('');

const bySite = new Map();
for (const r of rows) {
  const a = bySite.get(r.url) || { n: 0, eps: 0, ro: 0, orph: 0, page: 0, orphW: 0, pageW: 0, ex: [] };
  a.n++; a.eps += r.eps; a.ro += r.ro;
  for (const c of r.cls) {
    if (c.orphan) { a.orph++; if (c.wholly) a.orphW++; }
    else { a.page++; if (c.wholly) a.pageW++; if (a.ex.length < 4) a.ex.push(c.addr); }
  }
  bySite.set(r.url, a);
}

/* THE TWO COUNTS BELOW HAVE DIFFERENT ROWS AND MAY NEVER BE DIFFERENCED, which is stated here because they
   print on one line and a reader who subtracts them is reading nothing. `runtime-only` is a HISTOGRAM BUCKET
   summed over passes, so its row is ONE EMITTED ENDPOINT RECORD. `engine-holes`/`page-holes` are DISTINCT
   ADDRESS STRINGS within each census, so their row is ONE SPELLING -- a census that emitted one hole-bearing
   address twice contributes one. CLAUDE.md §AND-TWO-INSTRUMENTS-CAN-DISAGREE is exactly this, arriving inside
   one reader, and the measurement that caught it is that these two read 136 and 135 on one site. So the
   DISCRIMINATING claim is asked PER PASS below, where no unit is crossed. */
console.log('## per site. `runtime-only` counts EMITTED RECORDS (a histogram bucket, summed over passes).');
console.log('## `engine-holes`/`page-holes` count DISTINCT ADDRESS SPELLINGS per census. DIFFERENT ROWS --');
console.log('## they are printed together and must not be differenced. `wholly` is how many of each are ONE');
console.log('## brace and nothing else, which is where this proxy and the ROOT axis cannot disagree.');
for (const [u, a] of [...bySite].sort()) {
  console.log(`   ${u}`);
  console.log(`      passes=${a.n}  endpoints=${a.eps}  runtime-only=${a.ro} record(s)` +
              `   engine-holes=${a.orph} spelling(s) (wholly ${a.orphW})  page-holes=${a.page} (wholly ${a.pageW})`);
  if (a.ex.length) console.log(`      page holes, e.g.: ${a.ex.join('  ')}`);
}
console.log('');
/* THE CLAIM, PER PASS, WHERE NO UNIT IS CROSSED: of the passes whose razor is NONZERO, how many have a PAGE
   hole at all. A pass is one row on both sides of this question. */
const nz = rows.filter(r => r.ro > 0);
const nzPage = nz.filter(r => r.cls.some(c => !c.orphan));
const zPage  = rows.filter(r => r.ro === 0 && r.cls.some(c => !c.orphan));
console.log(`## ${nz.length} pass(es) of ${rows.length} have a NONZERO razor.`);
console.log(`## of those, ${nzPage.length} have a hole the PAGE read and ${nz.length - nzPage.length} have ONLY`);
console.log('## holes this engine minted. A pass in the second group is one whose whole razor is paid by this');
console.log("## engine's own instrument -- the residual's claim, read here rather than argued.");
for (const r of nz) {
  const pg = r.cls.filter(c => !c.orphan).length, en = r.cls.filter(c => c.orphan).length;
  /* THE RECORD AND NOT THE FILE, for the loop above's reason: a file holding several sites prints
     several rows here and `#<i>` is which one. */
  console.log(`   ${r.at.padEnd(33)} ${r.url.padEnd(28)} razor=${String(r.ro).padEnd(4)}` +
              ` engine=${String(en).padEnd(3)} page=${pg}${pg ? '' : '   <- PAID ENTIRELY BY THIS ENGINE'}`);
}
if (zPage.length)
  console.log(`## and ${zPage.length} pass(es) hold a PAGE hole while their razor reads ZERO -- a hole that ` +
              `reached no address,\n##     which is a different finding and is not this one.`);
console.log('## A ZERO EVERYWHERE IS A REFUSAL TO CLAIM THE CAPABILITY AND NOT A SMALLER VERSION OF IT.');
