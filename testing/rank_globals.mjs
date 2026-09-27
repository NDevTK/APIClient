/* WHAT REAL BUNDLES REACH FOR — the ranking half of the absent-globals question.
 *
 *   node testing/rank_globals.mjs --corpus <dir>/mirror                  rank every platform global name
 *   node testing/rank_globals.mjs --corpus <dir>/mirror --absent <file>  rank only the names in that file
 *   node testing/rank_globals.mjs --corpus <dir>/mirror --sites          per-artifact detail for the head
 *
 * What writes a `--corpus` is `NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs`, which
 * prints the path to pass here; `--absent` takes the `PROBE-NAMES` payload of `testing/probe_globals.mjs`.
 *
 * WHY A RANKING AND NOT A LIST. `probe_globals.mjs` answers WHICH platform global names this engine does not
 * answer. That answer is a census of absences, and CLAUDE.md's opening forbids one as a deliverable: its only
 * reader is the person about to invalidate it, and it is wrong in the direction that makes the work look
 * bigger than it is. What makes an absence worth building is how often real code REACHES for the name, which
 * is a fact about bundles rather than about this engine — so it is measured here, separately, against frozen
 * bytes, and intersected with the absence list at the end.
 *
 * THE NAME LIST IS DERIVED. `testing/platform_names.mjs` reads the generated header; there is no copy here.
 *
 * THE INPUTS BELONG TO A REVISION OR THIS REFUSES TO RUN — REWRITTEN RATHER THAN DELETED, BECAUSE IT IS THE
 * REASONING A READER RE-DERIVES AND OBEYING IT UNCHANGED IS WHAT MADE THIS FILE UNRUNNABLE. It read: a frozen
 * bundle that git does not track produces a number belonging to whoever's working tree it ran in, and that is
 * not visible in the result; so every input is asserted tracked with `git ls-files --error-unmatch` before a
 * byte of it is read, and an untracked one is a loud failure rather than a skipped file. Every clause of that
 * is correct about a LIBRARY, which is a vendored file this repository carries — and it is what made
 * `node testing/rank_globals.mjs` THROW before reading a byte for as long as it stood: its only APPLICATION
 * group named a committed capture of somebody else's site, `git ls-files` over that path answers NOTHING, and
 * the refusal to skip a group it could not find was the one part behaving as designed.
 * A CAPTURE OF SOMEBODY ELSE'S SITE IS NOT AN ARTIFACT THIS REPOSITORY MAY CARRY, so `tracked` is the wrong
 * IDENTITY for half of this corpus and demanding it does not make a figure belong to a revision — it makes
 * the figure not exist. testing/corpus/README.md is that decision; a page's scripts are FETCHED AT RUNTIME
 * and are not a thing this tree vendors. So the discipline SPLITS BY WHAT EACH INPUT IS. A library is tracked
 * and asserted so, unchanged. An application arrives through a required `--corpus <dir>` and is identified by
 * WHEN IT WAS FETCHED, printed beside every figure, because that is the identity a fetched corpus has: a path
 * is not one, since the next drive writes a different corpus at the same `--out`.
 * RETIREMENT — MET BY CONSTRUCTION, AND THE RECORD IS REWRITTEN RATHER THAN DELETED BECAUSE WHAT A READER
 * RE-DERIVES IS THE `tracked` RULE AND NOT THE CHECK. The condition first written here was `when no tracked
 * path in this file names a capture of a third-party site`, which quantifies over THIS FILE and is satisfied
 * by an ABSENCE — met the moment the capture is deleted, with nothing built and the rule still re-derivable,
 * which is the trap CLAUDE.md §AND-A-CONDITION-QUANTIFIED-OVER-THIS-FILE'S-OWN-PROSE names. The construction
 * that closes it is the assert at the `--corpus` argument below: a corpus holding a TRACKED file is REFUSED,
 * so committing one is no longer a way to make this instrument run and the rule is enforced where the
 * argument arrives. This record goes when nothing in this tree asserts a path tracked in order to admit it
 * as corpus bytes, because the confusion it warns about is then unspellable rather than merely answered.
 *
 * THE PROGRAM POPULATION IS THE MANIFEST'S AND NOT A FILENAME'S. This file used to decide which mirrored
 * files were programs with `/\.(js|mjs|html)$/` over a `git ls-files` expansion, which is precisely the
 * defect engine/corpus_programs.mjs was built to end: a fetcher folds a URL's query into the saved name, so
 * real shipped bundles are spelled with suffixes no list anybody writes will reach, and a corpus that is
 * quietly smaller reports a smaller absence — which reads as progress. The population, and the four
 * accounting throws that keep it whole, are imported from that file rather than restated here. The price of
 * the filter is not argued, it is measured, and the command is the deliverable rather than the figure:
 *   node -e 'import("./engine/corpus_programs.mjs").then(({corpusPrograms})=>{const{files}=corpusPrograms(process.argv[1],"x");console.log(files.length,files.filter(f=>!/\.(js|mjs|html)$/i.test(f)).length)})' <dir>/mirror
 * Its second number is what an extension test drops, and on a fresh drive it is not zero: a site's own
 * DOCUMENT is saved with no extension at all, so the filter discards the whole `.html` channel this file
 * ranks — the half its own author believed was covered.
 *
 * THE SPELLINGS ARE STATED, AND THE FIGURE IS A FLOOR. This is a static sweep over source text, so it counts
 * a SPELLING and never a population: a constructed name (`window[k]`), a destructured import, an aliased
 * reference (`const R = Response`) and a name reached through a bundler's own module registry are every one of
 * them invisible to it. CLAUDE.md §a-count-over-source-text-is-a-count-of-a-spelling — a completed sweep that
 * reports 0 means 0 OF THE SPELLING SEARCHED, and reading that as "no real code wants this" is the one reading
 * that closes the question. So each channel is named in the output and every total prints as a floor.
 *
 * THE `.member` CHANNEL IS REFUSED, NOT WEIGHTED. In minified code `.name`, `.length` and `.replace` are
 * ordinary object properties on every object in the bundle, so a member-access channel does not rank names, it
 * ranks how common the word is. It is the discard that makes the rest mean anything. A BARE IDENTIFIER is
 * refused for the same reason one step weaker: a minifier cannot rename a free global, but it also cannot stop
 * a module declaring its own `class Response`, and nothing in the text tells those apart.
 *
 * AMBIGUITY IS BANDED, NEVER SUMMED WITH THE FINDINGS. CLAUDE.md §a-count-that-mixes-i-found-a-defect-with-
 * i-cannot-see-this-construct. A file that DECLARES an identifier (`class X`, `function X`, `var|let|const X`)
 * can produce `new X(` and `instanceof X` sites that are nothing to do with the platform, so this file's hits
 * for that name are banded AMBIGUOUS and kept out of the rank. A file that ASSIGNS `window.X =` is declaring
 * its own global, so its qualified-global hits are banded the same way. Both bands are printed: a name whose
 * whole evidence is ambiguous is a name this instrument cannot see, which is a different fact from a name no
 * bundle reaches for.
 *
 * THE UNIT IS THE ARTIFACT, NOT THE SITE. Ranking by raw site count lets one 3.3 MB chunk that calls one
 * constructor ten times outrank a name six independent artifacts each reach once. The primary key is how many
 * distinct frozen artifacts hold at least one unambiguous site; the site total breaks ties and is printed
 * beside it, because a count and the population it is drawn from are one reading. */

import { readFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join, relative, resolve, sep } from "node:path";
import { platformNames, PLATFORM_NAMES_HEADER } from "./platform_names.mjs";
import { corpusPrograms } from "../engine/corpus_programs.mjs";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");

/* THE TRACKED HALF OF THE CORPUS. Every entry is a path this REVISION contains, and each is asserted so below.
   These are vendored libraries, which a repository may carry; the APPLICATIONS come from `--corpus` because a
   capture of somebody else's site is not an artifact this tree vendors. `group` is the unit the rank counts:
   a production application's many chunks are ONE artifact whose author is one author, so counting its chunks
   separately would let one app outvote every independent library. */
const LIBRARIES = [
    { group: "jquery-3.7.1", kind: "library (minified)", files: ["jquery-3.7.1.min.js"] },
    { group: "axios", kind: "library (minified)", files: ["axios.min.js"] },
    { group: "superagent", kind: "library (minified)", files: ["superagent.min.js"] },
    { group: "ky", kind: "library (minified)", files: ["ky.min.js"] },
    { group: "redaxios", kind: "library (minified)", files: ["redaxios.min.js"] },
    { group: "unfetch", kind: "library (minified)", files: ["unfetch.min.js"] },
];

/* `git ls-files --error-unmatch` is the membership test, asking the REVISION rather than the disk, so a file a
   peer left lying about cannot join the corpus. It is asked of each library BY NAME; the directory-expanding
   form this file also had is gone with the committed capture it expanded, and its replacement is
   corpusPrograms, which asks a manifest rather than a filename. */
function assertTracked(f) {
    try { execFileSync("git", ["-C", ROOT, "ls-files", "--error-unmatch", "-z", "--", f], { stdio: "pipe" }); }
    catch { throw new Error(`corpus input is UNTRACKED at this revision: ${f} — a number read from it belongs to no revision`); }
}

/* THE CHANNELS. Each is one regular expression capturing an IDENTIFIER at a site where a platform global name
   is what a page would be naming. `weak` channels corroborate and never rank on their own: `X.prototype` is
   how every class in a bundle is patched, so it is evidence about a name already evidenced elsewhere. */
const CHANNELS = [
    { id: "new",      weak: false, why: "construction",              re: /\bnew\s+([A-Za-z_$][\w$]*)\s*[(<]/g },
    { id: "global.",  weak: false, why: "qualified global reference", re: /\b(?:window|self|globalThis)\s*\.\s*([A-Za-z_$][\w$]*)/g },
    { id: "instanceof", weak: false, why: "brand test",              re: /\binstanceof\s+([A-Za-z_$][\w$]*)/g },
    { id: "typeof",   weak: false, why: "feature detection",          re: /\btypeof\s+([A-Za-z_$][\w$]*)/g },
    { id: "in global", weak: false, why: "feature detection",         re: /["']([A-Za-z_$][\w$]*)["']\s*\bin\b\s*(?:window|self|globalThis)\b/g },
    { id: "extends",  weak: false, why: "subclassing",               re: /\bextends\s+([A-Za-z_$][\w$]*)/g },
    { id: ".prototype", weak: true, why: "prototype patch",          re: /\b([A-Za-z_$][\w$]*)\s*\.\s*prototype\b/g },
];
/* REFUSED, and named so the reader knows what this instrument did not look at rather than reading its silence
   as absence: a bare `.member` access, and a bare free identifier. */
const REFUSED = [".member access (`.X`) — collides with every object property in a minified bundle",
                 "bare identifier (`X`) — a module may declare its own `class X` and the text cannot tell"];

const DECLARES = /\b(?:class|function|var|let|const)\s+([A-Za-z_$][\w$]*)/g;
const PAGE_GLOBAL = /\b(?:window|self|globalThis)\s*\.\s*([A-Za-z_$][\w$]*)\s*=(?!=)/g;

function scan(text, names) {
    const declared = new Set(), pageGlobal = new Set();
    for (const m of text.matchAll(DECLARES)) declared.add(m[1]);
    for (const m of text.matchAll(PAGE_GLOBAL)) pageGlobal.add(m[1]);
    const hits = new Map();                     /* name -> { channel -> {clear, ambiguous} } */
    for (const ch of CHANNELS) {
        for (const m of text.matchAll(ch.re)) {
            const n = m[1];
            if (!names.has(n)) continue;
            /* A qualified global read is only ambiguous when THIS file also assigns that global; every other
               channel is ambiguous when the file declares the identifier at all. */
            const amb = ch.id === "global." || ch.id === "in global" ? pageGlobal.has(n) : declared.has(n);
            let byCh = hits.get(n); if (!byCh) hits.set(n, byCh = new Map());
            let c = byCh.get(ch.id); if (!c) byCh.set(ch.id, c = { clear: 0, ambiguous: 0 });
            c[amb ? "ambiguous" : "clear"]++;
        }
    }
    return hits;
}

const argv = process.argv.slice(2);
/* THE CORPUS IS A REQUIRED ARGUMENT AND HAS NO DEFAULT, for the reason engine/absentrank.mjs and
   engine/nsguardrank.mjs already give at their own `--corpus`: the directory this file used to name was a
   committed copy of other people's sites and that copy is gone. A default would not be a convenience — a path
   that is ABSENT throws, but a path that EXISTS and holds nothing answers zero files, and a rank over zero
   files is a clean bill drawn from nothing. So it is asked for, and the message names what writes one, because
   a throw that names a hazard and offers no exit is the contract this file was standing in for a session. */
const corpusAt = argv.indexOf("--corpus");
const corpusArg = corpusAt >= 0 ? argv[corpusAt + 1] : null;
if (!corpusArg)
    throw new Error("[rank_globals] --corpus <dir> is REQUIRED. It names the `mirror` directory of responses a "
        + "real-network drive saved, with its provenance.json manifest beside it. There is no default: the "
        + "committed capture of other people's sites that used to be one is deleted, and ranking against an "
        + "empty directory would report a clean bill drawn from no files at all. What writes one is "
        + "`NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs`, which prints the --corpus path "
        + "to pass here and the instant it fetched — a figure ranked out of it is a fact about that instant and "
        + "about the sites that answered in it. The tracked libraries below need no argument and are ranked "
        + "either way.");
const CORPUS = resolve(corpusArg);
/* AND THE CORPUS MAY NOT BE A TRACKED PATH — the rule above, made true by CONSTRUCTION rather than argued.
   The reasoning it replaces is re-derivable by anybody: this file asserts its libraries TRACKED, so the
   obvious way to "fix" a corpus that cannot be found is to commit one, and that is the decision
   testing/corpus/README.md exists to hold. A capture of somebody else's site is not this project's to carry,
   and a committed one would also freeze a program no visitor is served — a page's scripts are fetched at
   runtime. So the refusal is enforced here, where the argument arrives, and not left to a paragraph. The test
   is only asked of a path INSIDE this repository: `git ls-files` refuses one outside it, and a corpus outside
   it cannot be tracked by this revision anyway. */
if (CORPUS === ROOT || CORPUS.startsWith(ROOT + sep)) {
    const trackedInCorpus = execFileSync("git", ["-C", ROOT, "ls-files", "-z", "--", CORPUS], { encoding: "utf8" })
        .split("\0").filter(Boolean);
    if (trackedInCorpus.length)
        throw new Error(`[rank_globals] --corpus ${relative(ROOT, CORPUS)} holds ${trackedInCorpus.length} `
            + `TRACKED file(s) (e.g. ${trackedInCorpus.slice(0, 3).join(", ")}). A corpus this revision carries `
            + `is a committed capture of somebody else's site, which testing/corpus/README.md decides this `
            + `project does not hold — and it would freeze a program no visitor is served, since a page's `
            + `scripts are fetched at runtime. Fetch one into an ignored path instead: `
            + `\`NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs\`.`);
}
const absentAt = argv.indexOf("--absent");
const wantSites = argv.includes("--sites");
let restrict = null, absentPath = null;
if (absentAt >= 0) {
    absentPath = argv[absentAt + 1];
    if (!absentPath) throw new Error("--absent needs a file of names, one per line");
    restrict = new Set(readFileSync(absentPath, "utf8").split(/\s+/).filter(Boolean));
    if (restrict.size === 0) throw new Error(`${absentPath}: parsed empty`);
}

const all = platformNames();
const names = new Set(restrict ? all.filter((n) => restrict.has(n)) : all);
if (restrict) {
    const unknown = [...restrict].filter((n) => !names.has(n));
    /* A name in the absent list that the header does not contain means the two were derived from different
       revisions of the header, and every figure below would then be about two populations. */
    if (unknown.length) throw new Error(
        `--absent names ${unknown.length} identifier(s) absent from the header itself (${unknown.slice(0, 5).join(" ")}…)` +
        " — the absence list and the name table came from different revisions");
}

/* THE GROUPS. A library is one named tracked file. An application is one SITE of the fetched corpus, and the
   site is read off the mirror's own layout — testing/corpus/fetch.mjs writes each site under a directory named
   for its id — rather than from a list here, which would be the second copy of a fact the corpus already
   states and the copy that goes short the day a site is added to apps.tsv. */
const corpus = corpusPrograms(CORPUS, "rank_globals");
const groups = [];
for (const entry of LIBRARIES) {
    for (const f of entry.files) assertTracked(f);
    groups.push({ ...entry, files: entry.files.map((f) => join(ROOT, f)) });
}
const bySite = new Map();
for (const abs of corpus.files) {
    const id = relative(CORPUS, abs).split(/[\\/]/)[0];
    if (!bySite.has(id)) bySite.set(id, []);
    bySite.get(id).push(abs);
}
for (const [id, files] of [...bySite].sort((a, b) => a[0].localeCompare(b[0])))
    groups.push({ group: id, kind: "app (fetched, typed by the server's own Content-Type)", files });

/* per name: group -> {clear, ambiguous, channels:Map} */
const tally = new Map();
for (const entry of groups) {
    for (const f of entry.files) {
        const hits = scan(readFileSync(f, "utf8"), names);
        for (const [n, byCh] of hits) {
            let g = tally.get(n); if (!g) tally.set(n, g = new Map());
            let rec = g.get(entry.group); if (!rec) g.set(entry.group, rec = { clear: 0, ambiguous: 0, ch: new Map() });
            for (const [chId, c] of byCh) {
                const ch = CHANNELS.find((x) => x.id === chId);
                if (!ch.weak) { rec.clear += c.clear; rec.ambiguous += c.ambiguous; }
                let e = rec.ch.get(chId); if (!e) rec.ch.set(chId, e = { clear: 0, ambiguous: 0 });
                e.clear += c.clear; e.ambiguous += c.ambiguous;
            }
        }
    }
}

const rows = [];
for (const [n, g] of tally) {
    let artifacts = 0, sites = 0, amb = 0; const chs = new Set(); const where = [];
    for (const [grp, rec] of g) {
        if (rec.clear > 0) { artifacts++; sites += rec.clear; where.push(`${grp}:${rec.clear}`); }
        amb += rec.ambiguous;
        for (const [chId, e] of rec.ch) if (e.clear > 0) chs.add(chId);
    }
    rows.push({ n, artifacts, sites, amb, chs: [...chs].sort(), where });
}
rows.sort((a, b) => b.artifacts - a.artifacts || b.sites - a.sites || a.n.localeCompare(b.n));

const clear = rows.filter((r) => r.artifacts > 0);
const onlyAmb = rows.filter((r) => r.artifacts === 0 && r.amb > 0);

const L = console.log;
L(`# rank_globals — what frozen bundles reach for`);
L(`# name table: ${PLATFORM_NAMES_HEADER_LABEL()} (${all.length} names)`);
if (restrict) L(`# restricted to ${names.size} of ${restrict.size} names from ${absentPath}`);
/* THE CORPUS'S OWN IDENTITY, BESIDE THE FIGURES AND NOT IN A HEADER. A fetched corpus is identified by WHEN
   it was fetched: the path is reused by the next drive, so two runs a week apart print the same path over two
   different corpora and a reader cannot tell which figures they hold. The window comes from the manifest
   through corpusPrograms, and a manifest with no instant in it says so rather than printing a blank — an
   absent instant and a known one are different facts. */
L(`# corpus ${relative(ROOT, CORPUS)} — ${corpus.files.length} file(s), ${corpus.bytes} byte(s): ` +
  `${corpus.nProgram} program + ${corpus.nDocument} document, ${corpus.nExcluded} other, ${corpus.onDisk} on ` +
  `disk, ${corpus.manifestRows} manifest row(s) — typed by the server's own Content-Type in provenance.json, ` +
  `never by extension`);
L(corpus.nFetchedAt
    ? `# corpus fetched ${corpus.fetchedFrom} .. ${corpus.fetchedTo} (${corpus.nFetchedAt} row(s) carry an ` +
      `instant) — every site figure below is a fact about that window and about the sites that answered in it`
    : `# corpus fetch instant UNKNOWN — no row of its provenance.json carries a fetchedAt, so the site figures ` +
      `below cannot be dated and must not be compared with another run's`);
L(`# groups (a library is one tracked file; an app is one site of the fetched corpus):`);
const pad = Math.max(...groups.map((g) => g.group.length));
for (const g of groups) L(`#   ${g.group.padEnd(pad)} ${String(g.files.length).padStart(3)} file(s)  ${g.kind}`);
L(`# channels ranked: ${CHANNELS.filter((c) => !c.weak).map((c) => c.id).join(", ")}`);
L(`# channels corroborating only: ${CHANNELS.filter((c) => c.weak).map((c) => c.id).join(", ")}`);
for (const r of REFUSED) L(`# channel REFUSED: ${r}`);
L(`# EVERY COUNT BELOW IS A FLOOR — this is a count of a spelling, not of a population. A name reached through`);
L(`# window[k], an alias, a destructured import or a bundler's module registry is invisible here.`);
L(``);
L(`artifacts  sites  amb  channels                       name`);
for (const r of clear) {
    L(`${String(r.artifacts).padStart(9)}  ${String(r.sites).padStart(5)}  ${String(r.amb).padStart(3)}  ${r.chs.join(",").padEnd(30)} ${r.n}`);
    if (wantSites) L(`${" ".repeat(24)}${r.where.join("  ")}`);
}
L(``);
L(`# names whose ONLY evidence is ambiguous (a corpus file declares the identifier itself) — this instrument`);
L(`# cannot see these, which is not the same fact as no bundle reaching for them:`);
L(`# ${onlyAmb.length ? onlyAmb.map((r) => `${r.n}(${r.amb})`).join(" ") : "(none)"}`);
L(``);
L(`# names in the ranked population with NO site of any ranked channel: ${names.size - clear.length - onlyAmb.length} of ${names.size}`);

/* The header's path comes from the module that owns the parse, so this file holds no second spelling of it. */
function PLATFORM_NAMES_HEADER_LABEL() { return relative(ROOT, PLATFORM_NAMES_HEADER); }
