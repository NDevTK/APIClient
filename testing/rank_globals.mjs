/* WHAT REAL BUNDLES REACH FOR — the ranking half of the absent-globals question.
 *
 *   node testing/rank_globals.mjs                       rank every platform global name
 *   node testing/rank_globals.mjs --absent <file>        rank only the names in that file (one per line)
 *   node testing/rank_globals.mjs --sites                per-artifact detail for the ranked head
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
 * THE INPUTS BELONG TO A REVISION OR THIS REFUSES TO RUN. CLAUDE.md §for-every-input-a-gate-reads: a frozen
 * bundle that git does not track produces a number belonging to whoever's working tree it ran in, and that is
 * not visible in the result. Every input is asserted tracked with `git ls-files --error-unmatch` before a byte
 * of it is read, and an untracked one is a loud failure rather than a skipped file — an instrument that adds
 * a file with `if (exists(p))` prints a fraction of a population missing its largest member and says nothing.
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
import { dirname, join, relative } from "node:path";
import { platformNames } from "./platform_names.mjs";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");

/* THE CORPUS. Every entry is a path this REVISION contains, and each is asserted so below. `group` is the unit
   the rank counts: gitlab's seventeen webpack resources are ONE production application whose chunks share an
   author, so counting them as seventeen artifacts would let one app outvote six independent libraries. */
const CORPUS = [
    { group: "gitlab", kind: "app (webpack, production, incl. admin chunks)", glob: "testing/corpus/mirror/gitlab" },
    { group: "jquery-3.7.1", kind: "library (minified)", files: ["jquery-3.7.1.min.js"] },
    { group: "axios", kind: "library (minified)", files: ["axios.min.js"] },
    { group: "superagent", kind: "library (minified)", files: ["superagent.min.js"] },
    { group: "ky", kind: "library (minified)", files: ["ky.min.js"] },
    { group: "redaxios", kind: "library (minified)", files: ["redaxios.min.js"] },
    { group: "unfetch", kind: "library (minified)", files: ["unfetch.min.js"] },
];

/* `git ls-files` over a directory lists exactly the tracked files under it, which is both the membership test
   and the expansion — asking the revision rather than the disk, so a file a peer left lying about cannot join
   the corpus and a tracked file cannot be missed. */
function trackedUnder(p) {
    const out = execFileSync("git", ["-C", ROOT, "ls-files", "-z", "--", p], { encoding: "utf8" });
    return out.split("\0").filter(Boolean);
}
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

/* per name: group -> {clear, ambiguous, channels:Map} */
const tally = new Map();
const groups = [];
for (const entry of CORPUS) {
    const files = entry.glob ? trackedUnder(entry.glob) : entry.files;
    for (const f of files) assertTracked(f);
    const jsish = files.filter((f) => /\.(js|mjs|html)$/i.test(f));
    if (jsish.length === 0) throw new Error(`corpus group ${entry.group} matched no script or document under the revision`);
    groups.push({ ...entry, files: jsish });
    for (const f of jsish) {
        const hits = scan(readFileSync(join(ROOT, f), "utf8"), names);
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
L(`# corpus (every path asserted tracked at this revision):`);
for (const g of groups) L(`#   ${g.group.padEnd(14)} ${String(g.files.length).padStart(3)} file(s)  ${g.kind}`);
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

function PLATFORM_NAMES_HEADER_LABEL() { return relative(ROOT, new URL("../engine/host/browser/platform_names.h", import.meta.url).pathname); }
