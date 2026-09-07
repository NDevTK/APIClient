/* Spec-citation DISAGREEMENT reporter — the sibling half of engine/citegen.mjs, and the only instrument here
 * that judges this tree's citations against EACH OTHER rather than against a standard.
 *
 *   node engine/citediff.mjs [path …]   report disagreements (default: the prose this project WROTE — see
 *                                       defaultTargets)
 *   node engine/citediff.mjs --all      every group of every band, rather than the head of each
 *   node engine/citediff.mjs --governed the second pass: every step claim filed under the section its own
 *                                       prose names, banded by whether that section was STATED beside the
 *                                       claim or merely CARRIED from earlier in the file, so one fetch of one
 *                                       algorithm settles a cluster rather than one claim
 *
 * WHAT IT DOES, AND THE WHOLE IDEA IS THE KEY. Take the ~95 characters that FOLLOW a spec coordinate, blank
 * every coordinate inside that text, and use what is left as a grouping key. Two sites whose following prose
 * is identical are two sites making the SAME claim; if the coordinates in front of that prose DIFFER, this
 * tree disagrees with itself about where that claim lives. No standard is fetched, no index is read, and no
 * section number is looked up: the evidence is entirely other sites in this tree.
 *
 * A GROUP IS NOT A DEFECT, AND THIS IS THE FIRST THING THE OUTPUT SAYS BECAUSE IT IS THE ONE WAY THIS REPORT
 * CAN COST SOMETHING. Most groups are legitimate. Two members of one algorithm family — an ordinary array
 * operation and its typed-array counterpart — share a sentence VERBATIM and genuinely stand at different
 * numbers, because they are different clauses of one standard saying one thing. So a group is a QUESTION
 * ("these sites claim one thing at several coordinates — which reading is right?"), never an accusation, and
 * the answer comes only from fetching the algorithm and reading it.
 *
 * AND THE SWEEP IS THE THING NOT TO DO. A reader who takes a group's MAJORITY coordinate and rewrites the odd
 * one out to match will break correct sites, and that is not hypothetical: the incident that produced this
 * tool contains a cluster where the majority was RIGHT and the odd one out needed coordinates that were in
 * NEITHER camp. A disagreement says that the sites cannot all be right; it says nothing whatever about which
 * of them is wrong, so EACH SIDE IS RESOLVED AGAINST THE FETCHED ALGORITHM INDIVIDUALLY. The group's value is
 * that it tells you which handful of sites to fetch for — that is all it is for.
 *
 * WHAT IS ALREADY SEEN AND WHAT IS NOT, STATED SO THIS TOOL DOES NOT OVERSELL ITSELF. The two bands below are
 * not alike in novelty and reporting them as one capability would be a false claim:
 *   — THE SECTION BAND IS NOT AN UNSEEN AXIS. citegen resolves a section coordinate against the standard's own
 *     index and had ALREADY REPORTED one member of the cluster that produced this tool, as a misattribution,
 *     and it stood unrepaired. The instrument was right and nobody acted on it. What this band adds is not
 *     sight but ORDERING: it puts the disagreeing siblings on one line, so a reader can see that the tree
 *     contradicts itself without resolving anything, which is a cheaper prompt to go and read than a single
 *     verdict in a long list. Where the two disagree, citegen is the one with evidence.
 *   — THE STEP BAND IS NOT AN UNSEEN AXIS EITHER, AND THIS PARAGRAPH SAID IT WAS. It read: citegen indexes
 *     SECTIONS and not the list items inside them, so a step claim is one it cannot resolve, and this band is
 *     the only mechanical statement anything here makes about that population. THAT IS FALSE, and it was false
 *     when it was written: citegen carries a STEP CHECK that walks a cited section's own lists and reports a
 *     component no reading of that section admits, under two headings of its own. A lane resolving this band
 *     found several of its groups already standing as citegen findings, one of them naming the very pair the
 *     group put on one line. The claim cost nothing here only because it was checked; what it WOULD have cost
 *     is the redundant second auditor CLAUDE.md forbids, argued for by a tool's own prose.
 *
 *     WHAT THIS BAND ACTUALLY ADDS IS THE HALF citegen'S OWN CHANNEL SAYS IT CANNOT DO: that channel asks
 *     whether a written component CAN EXIST in the section named, so a component that exists and is the WRONG
 *     one is admitted — where an algorithm's step holds several sub-items, two different sub-numbers are both
 *     in range and only MEANING separates them. This band is evidence about exactly that population, because
 *     two sites making ONE claim at two IN-RANGE coordinates cannot both be right and neither is out of range.
 *     So the two instruments partition rather than overlap: citegen falsifies a component against the list,
 *     and this falsifies a pair against each other. Neither says which member is wrong.
 *
 *     THE GENERAL LESSON, WHICH IS WHY THIS IS A CORRECTION AND NOT A DELETION: a tool's header stating what
 *     ANOTHER tool cannot see is a claim about THIS TREE, and it rots exactly like a stale crash — worse here,
 *     because its only reader is somebody deciding whether to build an instrument. Before writing that nothing
 *     else speaks about a population, RUN THE OTHER TOOL AND READ ITS CHANNEL LIST.
 *
 * A MEASURED BLIND-SPOT BOUNDARY, RECORDED AS AN OBSERVATION RATHER THAN AS A RULE. With a section coordinate
 * corrected, a shallow lettered sub-step stayed silent in citegen's step channel while a sub-step lettered far
 * enough down the alphabet DID raise an out-of-range verdict on the same line. A sibling site's silence was
 * probed the same way and was NOT explained. THE CAUSE IS OPEN: the obvious reading — that the shallow one is
 * in range and the deep one is not — is a hypothesis nobody has established, and naming it here would put a
 * mechanism in the tree that no measurement supports. What is known is the pair of observations.
 *
 * DESCRIBE A STEP COORDINATE, NEVER WRITE ONE. This file spells no step number anywhere in its prose, and that
 * is a rule rather than a style. A lane writing a note ABOUT this gap put a specimen step coordinate in
 * backticks, and citegen's step channel judged it — so the note demonstrating the gap became a finding of its
 * own. Backticks do not disclaim a coordinate: the only reliable disclaimer is not to type one.
 *
 * THE APPROACH THAT DOES NOT WORK, RECORDED SO NOBODY BUILDS IT AGAIN. The obvious alternative key is the
 * NAME beside the coordinate — group by the title a site states and report the sites whose numbers differ.
 * Measured, it produced ONLY false positives, for two reasons that are properties of the standards rather than
 * of the parse: a stated title is routinely truncated at a colon, so one title matches several; and one
 * standard gives FOUR DISTINCT SECTIONS the identical heading `Runtime Semantics: Evaluation`, which makes
 * every citation of any of them disagree with every citation of the others. A title is evidence about WHICH
 * STANDARD a citation belongs to — which is what citegen uses it for, correctly — and it is not evidence about
 * which SECTION. The following-prose key works because a sentence is long and a heading is short.
 *
 * WHAT IT CANNOT SEE, so that a zero here is read as what it is:
 *   — A LONE WRONG COORDINATE. The entire method is comparison between siblings, so a claim this tree makes
 *     exactly ONCE has nothing to disagree with and is invisible. This report's silence about a site is not a
 *     statement that the site is right; it is a statement that no other site says the same thing.
 *   — A DISAGREEMENT WHOSE SENTENCES DIVERGE. Two sites making one claim in DIFFERENT WORDS key differently
 *     and never meet. The key is exact after case-folding and whitespace collapse, so a rewritten comment
 *     leaves its sibling alone in its group.
 *   — WHICH SIDE IS WRONG, and whether either is. See above; that is a question for the standard.
 *   — A MIXED-KIND GROUP IS NOT A DISAGREEMENT AT ALL, and it is banded rather than dropped. A section
 *     coordinate and a step coordinate are different KINDS of address, so a group holding both is a group
 *     whose members cannot contradict each other — the following sentence merely happened to coincide. It is
 *     printed under its own heading with the reason, because a band a reader can see is a band a reader can
 *     disbelieve, and a filter is a claim about a population nobody counted.
 *
 * IT PRINTS NO EXPECTED TOTAL AND NO EXPECTED COUNT OF ITS OWN. Both rot, and a count of what is outstanding
 * shrinks as the work is done, so its only reader is the person about to invalidate it. What this file states
 * is the DERIVATION; the numbers belong beside the revision they were measured at, in whatever report quotes
 * them.
 *
 * REPORT, NOT GATE, for citegen's reason and one of its own. Citation prose is not something a red build may
 * block a lane on; and a group is a question rather than a finding, so a gate built on this would be red
 * permanently by construction and would mute itself — the chronic-red shape. The exit code is 0.
 *
 * TWO REGEX OBJECTS, NOT ONE, AND THIS IS LOAD-BEARING RATHER THAN TIDINESS. A single globally-flagged regex
 * used BOTH to walk a window with exec and to blank the coordinates inside a key with replace never
 * terminates: replace resets lastIndex to 0, so the exec cursor is dragged back to the start of the window on
 * every iteration and the same first match is returned for ever. Transcribing the method into one regex is a
 * hang and not a wrong answer, which is why the three instances below are separate and named.
 *
 * THIS FILE IS IN ITS OWN POPULATION when the gates are among the targets, which is why its prose above
 * carries no coordinate that could group with anything. An instrument that reads its own kind of file is a
 * member of the set it measures, and the honest response is to write prose that cannot be mistaken for a
 * claim rather than to exclude the file.
 */

import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join, relative, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..");

/* THE ONE PATTERN, HELD AS A SOURCE STRING AND COMPILED THREE TIMES. See the header: sharing one compiled
 * global regex between exec and replace is an infinite loop rather than a mistake with an answer. */
const COORD_SRC = String.raw`§\d+(?:\.\d+)*|\bsteps?\s+\d+(?:\.[0-9a-z]+)*(?:\s*[-–—]\s*\d+(?:\.[0-9a-z]+)*)?`;
const scanRe = () => new RegExp(COORD_SRC, "ig");
const blankRe = () => new RegExp(COORD_SRC, "ig");
const hasRe = new RegExp(COORD_SRC, "i");

/* The window a claim's prose is read out of, and the slice of it used as the key. Both are POLICY INPUTS
 * rather than constants hidden in the walk: a reader who wants to know why two sites did not group can move
 * them and see. They are stated here so the run can print them, because a key length is the one number that
 * decides what this report can and cannot see. */
const WINDOW_LINES = 3;
const CONTEXT_CHARS = 95;
const CONTEXT_FLOOR = 40; /* below this a following-prose key is a fragment and groups on nothing */
const KEY_FLOOR = 45;     /* after blanking and collapsing, a key shorter than this is not a claim */
const HEAD = 12;          /* rows printed per band without --all */

const TEXT = /\.(c|h|mjs|js|md)$/;

function walk(dir, out = []) {
  /* THE CHECKOUT IS SHARED AND EDITED UNDER THIS WALK — citegen's rule and its reason: an editor's temporary
   * file appears between the readdir and the stat and is gone before it, and a vanished entry is a file that
   * was never in the tree this run measured, not an error. */
  let names;
  try { names = readdirSync(dir); } catch { return out; }
  for (const e of names) {
    if (e === "node_modules" || e === ".git" || e === "lexbor" || e === "qjs" || e === "out" ||
        e === ".work" || e.includes(".tmp.")) continue;
    const p = join(dir, e);
    let st;
    try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) walk(p, out);
    else if (TEXT.test(e)) out.push(p);
  }
  return out;
}

function defaultTargets(notify = () => {}) {
  const out = [];
  /* The default is the prose THIS PROJECT WROTE, which is the same population citegen judges and for the same
   * reason: a file the report does not collect is an excluded file and the total looks complete either way. */
  out.push(...walk(join(HERE, "host")));
  out.push(...walk(join(ROOT, "extension")));
  for (const doc of ["CLAUDE.md", "SECURITY.md"]) {
    const p = join(ROOT, doc);
    if (existsSync(p)) out.push(p);
  }
  for (const e of readdirSync(HERE)) if (/\.mjs$/.test(e)) out.push(join(HERE, e));

  /* THE FORK'S OWN FILES ARE DERIVED, NOT LISTED. engine/qjs is mostly upstream and upstream's citations are
   * not this tree's to answer for — but the fork annotates several of its translation units with this
   * project's citation convention, and a hand-kept list of WHICH ones is the second copy of a fact the files
   * themselves already state. Upstream writes no section marks at all, so a top-level source in the submodule
   * carrying one is a file this project wrote in. The rule reads the artifact; a list would drift from it, and
   * a list is how the largest body of citations in this tree came to be missed once already. */
  const sub = join(HERE, "qjs");
  let subNames = null;
  try { subNames = readdirSync(sub); } catch { subNames = null; }
  if (!subNames || subNames.length === 0) {
    /* ABSENT AND ZERO ARE DIFFERENT FACTS, and this is the line that says which. engine/qjs is a SUBMODULE:
     * a plain clone, and the frozen-snapshot procedure this project prescribes for every instrument that
     * reads the tree, leave it EMPTY. Nothing else in the output distinguishes "the fork disagrees with
     * itself nowhere" from "the fork was not read". */
    notify(`[citediff] NOT READ — engine/qjs is empty in this checkout, so every citation the fork carries ` +
           `is OUTSIDE this run and the counts below are a fraction of a population without it. It is a ` +
           `submodule: a plain clone does not populate it. This is an ABSENCE, not a clean result.`);
  } else {
    for (const e of subNames.sort()) {
      if (!/\.(c|h)$/.test(e)) continue;
      const p = join(sub, e);
      let body;
      try { body = readFileSync(p, "utf8"); } catch { continue; }
      if (body.includes("§")) out.push(p);
    }
  }
  return out;
}

/* Strip the comment furniture a line carries so that the prose either side of a marker joins up. This is the
 * one place the reader of a C comment and the reader of a Markdown paragraph are made to look alike. */
function stripMarker(line) {
  return line.replace(/^\s*(\/\*+|\*+\/|\*|\/\/)\s?/, "");
}

/* Normalise a coordinate to its KIND and its NUMBER, so that a singular and a plural spelling of one step
 * claim are one token rather than a guaranteed-false disagreement with itself. Nothing else about the
 * spelling survives; the dash inside a range is folded to one character for the same reason. */
function normCoord(tok) {
  const t = tok.toLowerCase().replace(/[–—]/g, "-").replace(/\s+/g, " ").trim();
  if (t.startsWith("§")) return { kind: "section", token: t };
  const n = t.replace(/^steps?\s+/, "").replace(/\s*-\s*/, "-");
  return { kind: "step", token: `step ${n}` };
}

/* EVERY WINDOW A COORDINATE APPEARS IN YIELDS A KEY, AND THE DE-DUPLICATION HAPPENS INSIDE THE GROUP RATHER
 * THAN HERE. A window opens at every prose line, so one coordinate is walked up to WINDOW_LINES times with a
 * different amount of following prose each time, and the obvious economy — keep only the occurrence with the
 * longest prose — is a NARROWING that costs groups: two sites whose long contexts diverge can still agree over
 * a shorter one, and that agreement is exactly what this report exists to see. So every occurrence is emitted
 * and a group's MEMBERSHIP is de-duplicated by the coordinate's own (file, line, column), which is what stops
 * one coordinate counting as two sites making one claim twice. The census below prints both figures, because
 * a count of occurrences and a count of coordinates are different facts. */
function collect(files) {
  const recs = [];
  const seen = new Set();
  let scanned = 0;
  for (const f of files) {
    let body;
    try { body = readFileSync(f, "utf8"); } catch { continue; }
    const rel = relative(ROOT, f);
    const lines = body.split("\n");
    for (let i = 0; i < lines.length; i++) {
      if (!hasRe.test(lines[i])) continue;
      /* Build the window and, beside it, the map from each window character back to the line and column it
       * came from — so a match can be addressed at its own source position even though the marker stripping
       * and the join have moved it. */
      let win = "";
      const at = [];
      for (let k = i; k < Math.min(i + WINDOW_LINES, lines.length); k++) {
        const raw = lines[k];
        const stripped = stripMarker(raw);
        const off = raw.length - stripped.length;
        if (win.length) { win += " "; at.push(null); }
        for (let c = 0; c < stripped.length; c++) at.push({ line: k + 1, col: off + c + 1 });
        win += stripped;
      }
      const re = scanRe();
      let m;
      while ((m = re.exec(win)) !== null) {
        scanned++;
        const end = m.index + m[0].length;
        const ctx = win.slice(end, end + CONTEXT_CHARS);
        if (ctx.length < CONTEXT_FLOOR) continue;
        const pos = at[m.index];
        if (!pos) continue;
        const id = `${rel}:${pos.line}:${pos.col}`;
        seen.add(id);
        const { kind, token } = normCoord(m[0]);
        recs.push({ id, file: rel, line: pos.line, kind, token, raw: m[0], ctx });
      }
    }
  }
  return { recs, coords: seen.size, scanned };
}

/* A COORDINATE IS REPLACED BY A PLACEHOLDER AND NOT BY WHITESPACE, AND THE DIFFERENCE IS THE WHOLE BAND.
 * Two coordinates written side by side — a section and the step inside it, a pair of numbers in one cell of a
 * table — are one claim stated once. Blanked to nothing, the first coordinate's following prose becomes the
 * second's following prose exactly, so every such citation groups WITH ITSELF and the report fills with
 * disagreements between a site and its own next word. A placeholder holds the position the coordinate
 * occupied, so a claim that reads "at ‹#› ‹#› do X" and one that reads "at ‹#› do X" are different claims,
 * while two sites making the same claim at different numbers still key alike — which is the one thing the
 * method needs. MEASURED: with the blank, the mixed-kind band was two orders of magnitude larger and
 * essentially every row of it was a coordinate PAIR AT ONE FILE AND ONE LINE — a site reported as
 * disagreeing with itself. The tell that a report has this defect is a group whose members share a line. */
const PLACEHOLDER = " \u2039#\u203a ";
/* A RUN OF ONE PUNCTUATION CHARACTER IS A RULE, NOT PROSE, so it is collapsed before the length floor is
 * applied and a banner's underline stops being a key. A section banner ends in a long row of dashes, and
 * ninety-odd characters of dashes clears any floor measured in CHARACTERS while carrying no claim at all —
 * so every banner in the tree groups with every other banner and reports components that share nothing but a
 * house style as disagreeing about a standard.
 *
 * THE OBVIOUS WIDER FIX WAS BUILT AS A CLASSIFIER, RUN OVER THE WHOLE POPULATION, AND REFUSED. A floor
 * measured in WORDS rather than characters retires the banners and reads as the principled version of this —
 * a key with no words is not a claim. Measured at a six-word floor it also retires a sixth of the STEP band,
 * and the rows it takes are the stage-label pairs that are this report's best signal: a label is an
 * algorithm's own step written as identifiers and punctuation, so it carries almost no dictionary words while
 * being the most explicit claim in the tree. The narrow rule keeps them and the wide one does not, which is
 * the whole reason the narrow one is here. */
function keyOf(ctx) {
  return ctx.replace(blankRe(), PLACEHOLDER)
            .replace(/([^\p{L}\p{N}\s])\1{3,}/gu, "$1")
            .replace(/\s+/g, " ").trim().toLowerCase();
}

function group(recs) {
  const byKey = new Map();
  let short = 0;
  for (const r of recs) {
    const k = keyOf(r.ctx);
    if (k.length < KEY_FLOOR) { short++; continue; }
    if (!byKey.has(k)) byKey.set(k, new Map());
    /* One coordinate reached through two windows is ONE site, not two. Keeping the first record per position
     * is safe because every record for a position carries the same coordinate; only the prose length differs,
     * and the prose is already the key. */
    const g = byKey.get(k);
    if (!g.has(r.id)) g.set(r.id, r);
  }
  const bands = { section: [], step: [], mixed: [] };
  let agreed = 0;
  for (const [k, g] of byKey) {
    const members = [...g.values()];
    const tokens = new Set(members.map((m) => m.token));
    if (tokens.size < 2) { agreed++; continue; }
    const kinds = new Set(members.map((m) => m.kind));
    const band = kinds.size > 1 ? "mixed" : [...kinds][0];
    bands[band].push({ key: k, members, tokens });
  }
  return { bands, keys: byKey.size, agreed, short };
}

/* ORDERED BY HOW MANY FILES THE GROUP SPANS, FEWEST FIRST, AND THE COUNT IS PRINTED. This is an ordering and
 * not a filter: every group is still here. A claim stated twice in ONE file is one author contradicting
 * themselves about one algorithm and is the sharpest thing this report finds; a claim keyed alike across a
 * dozen components is usually a SHARED IDIOM — a house-style assertion message, a repeated banner sentence —
 * whose sites cite different standards for different reasons and were never making one claim. Sorting by the
 * spread rather than by the size puts the first kind at the top, where a reader stops. */
function printBand(name, rows, blurb, all) {
  const spread = (g) => new Set(g.members.map((m) => m.file)).size;
  rows.sort((a, b) => spread(a) - spread(b) || a.members.length - b.members.length);
  console.log(`\n${name} — ${rows.length}`);
  console.log(`  ${blurb}`);
  if (!rows.length) return;
  const show = all ? rows : rows.slice(0, HEAD);
  for (const g of show) {
    const sp = spread(g);
    if (sp > 1) console.log(`  [${sp} files]`);
    console.log(`  · ${g.key.length > 110 ? g.key.slice(0, 110) + "…" : g.key}`);
    const byTok = new Map();
    for (const m of g.members) {
      if (!byTok.has(m.token)) byTok.set(m.token, []);
      byTok.get(m.token).push(`${m.file}:${m.line}`);
    }
    for (const [t, sites] of [...byTok].sort()) console.log(`      ${t.padEnd(14)} ${sites.join("  ")}`);
  }
  if (!all && rows.length > HEAD) console.log(`  … ${rows.length - HEAD} more — node engine/citediff.mjs --all`);
}

/* THE SECOND PASS: WHICH ALGORITHM EACH STEP CLAIM IS A CLAIM ABOUT. A step coordinate addresses a list INSIDE
 * a section, so it can only be checked by someone holding that section — and the expensive half of checking one
 * is FETCHING the algorithm, which is paid once per section and not once per claim. Filing every step claim
 * under the section its surrounding prose names turns a long list of individually unresolvable claims into a
 * short list of documents to fetch, each of which settles a cluster. This pass resolves nothing; it orders.
 *
 * THE ATTRIBUTION IS BANDED BY ITS EVIDENCE RATHER THAN ASSERTED, because the two ways a claim acquires a
 * section are not equally good and a single list would hide which one paid for each row:
 *   — STATED. The section is named on the claim's OWN LINE, or in the comment block the claim sits in. The
 *     author wrote both, so the attribution is the prose's own statement and is the strongest evidence
 *     available without a fetch. The same-line arm is why this band is not simply "in a comment block": a
 *     stage label is a string literal on a CODE line and routinely carries its section and its step together,
 *     which is the claim naming its own algorithm as plainly as a comment does. Banding those by proximity
 *     instead — which this pass did until the distances were read — files the tree's most explicit
 *     attributions under its weakest evidence, and they arrive at distance zero where they look like a
 *     coincidence of layout.
 *   — CARRIED. No section in the claim's own block, so the nearest section EARLIER IN THE FILE is offered with
 *     the distance in lines beside it. This is PROXIMITY and not evidence: a step claim inside a stage-label
 *     table, or in a block that names no algorithm, is very often about the section its function's banner
 *     names, and just as often is not. The distance is printed so a reader can price the guess rather than
 *     inherit it.
 *   — UNPLACED. No section anywhere earlier in the file. Counted, never attached to anything. An unplaced
 *     claim and a claim placed by proximity are different facts, and a default that merged them would turn
 *     "this report cannot say" into a plausible answer, which is the shape that makes a report worse than
 *     silence.
 * Every band is printed with its own count, so the ratio between them is the reader's, not this file's. */
function governed(files) {
  const inBlock = new Map();
  const carried = new Map();
  let claims = 0, unplaced = 0;
  for (const f of files) {
    let body;
    try { body = readFileSync(f, "utf8"); } catch { continue; }
    const rel = relative(ROOT, f);
    const lines = body.split("\n");
    const md = /\.md$/.test(f);
    /* A comment run is what bounds the strong band. In Markdown every line is prose, so the whole file is one
     * run and nothing lands in CARRIED — which is correct: a heading-structured document has no code lines to
     * break a block on, and pretending otherwise would invent a boundary the file does not have. */
    let blockSec = null, fileSec = null, fileSecLine = 0;
    for (let i = 0; i < lines.length; i++) {
      const isProse = md || /^\s*(\*|\/\/|\/\*)/.test(lines[i]);
      if (!isProse) blockSec = null;
      let lineSec = null;
      const re = scanRe();
      let m;
      const text = isProse ? stripMarker(lines[i]) : lines[i];
      while ((m = re.exec(text)) !== null) {
        const { kind, token } = normCoord(m[0]);
        if (kind === "section") {
          fileSec = token; fileSecLine = i + 1; lineSec = token;
          if (isProse) blockSec = token;
          continue;
        }
        claims++;
        const site = `${rel}:${i + 1}`;
        const stated = lineSec || blockSec;
        if (stated) {
          if (!inBlock.has(stated)) inBlock.set(stated, []);
          inBlock.get(stated).push({ token, site });
        } else if (fileSec) {
          if (!carried.has(fileSec)) carried.set(fileSec, []);
          carried.get(fileSec).push({ token, site, away: i + 1 - fileSecLine });
        } else unplaced++;
      }
    }
  }
  return { inBlock, carried, claims, unplaced };
}

function main() {
  const argv = process.argv.slice(2);
  const all = argv.includes("--all");
  const wantGoverned = argv.includes("--governed");
  const targets = argv.filter((a) => !a.startsWith("--"));
  const files = targets.length
    ? targets.map((t) => (statSync(t).isDirectory() ? walk(t) : [t])).flat()
    : defaultTargets((m) => console.log(m));

  console.log(`[citediff] ${files.length} files · window ${WINDOW_LINES} lines · key = the ${CONTEXT_CHARS} ` +
              `chars after a coordinate, coordinates blanked, floor ${KEY_FLOOR}`);
  console.log(`[citediff] A GROUP IS A QUESTION, NOT A FINDING. Sites whose following prose is identical make ` +
              `one claim at several\n           coordinates, so they cannot all be right — and this report ` +
              `has no evidence about WHICH is wrong.\n           Resolve EACH SIDE against the fetched ` +
              `algorithm on its own. Rewriting the odd one out to match the\n           majority breaks ` +
              `correct sites: a family whose members share a sentence and legitimately stand at\n           ` +
              `different coordinates groups here exactly like a defect does.`);

  const { recs, coords, scanned } = collect(files);
  const { bands, keys, agreed, short } = group(recs);

  console.log(`\n[citediff] ${coords} distinct coordinates carrying enough following prose to key on, seen as ` +
              `${recs.length}\n           occurrences out of ${scanned} matches walked — a window opens at ` +
              `every prose line, so one\n           coordinate is read with up to ${WINDOW_LINES} different ` +
              `amounts of prose after it and each one can\n           group. ${short} occurrences keyed too ` +
              `short to be a claim.`);
  console.log(`[citediff] ${keys} distinct claims; ${agreed} carry one coordinate across every site that ` +
              `states them.`);

  printBand("SECTION DISAGREEMENT", bands.section,
    "One claim, several section coordinates. citegen resolves this axis against the standards' own indexes " +
    "and\n  has evidence this report does not — where the two disagree, citegen is the one that read a spec. " +
    "What\n  this adds is that the disagreeing siblings are on one line.", all);

  printBand("STEP DISAGREEMENT", bands.step,
    "One claim, several step coordinates. This is the axis no other instrument here can see: citegen indexes " +
    "\n  SECTIONS and not the list items inside them, so a claim about a numbered step within an algorithm is " +
    "one\n  it cannot resolve. Nothing else in this tree speaks about this population.", all);

  printBand("MIXED KIND — NOT A DISAGREEMENT", bands.mixed,
    "A section coordinate and a step coordinate addressing the same following prose. These are different " +
    "KINDS\n  of address and cannot contradict each other, so no reading of this band is an accusation. It " +
    "is printed\n  rather than filtered because a band a reader can see is one a reader can disbelieve.", all);

  if (wantGoverned) {
    const { inBlock, carried, claims, unplaced } = governed(files);
    const n = (m) => [...m.values()].reduce((a, v) => a + v.length, 0);
    console.log(`\nSTEP CLAIMS BY GOVERNING SECTION — ${claims} claims; ${n(inBlock)} named on their ` +
                `own line or in their own block, under\n  ${inBlock.size} sections, ${n(carried)} carried from the nearest ` +
                `section earlier in the file under ${carried.size}\n  sections, ${unplaced} with no section ` +
                `anywhere earlier in their file.`);
    console.log(`  Checking a step claim costs one fetch of ITS ALGORITHM, and the fetch is per SECTION, so ` +
                `ordering by\n  section spends one read on a cluster. STATED is the prose's own statement ` +
                `of which algorithm it is\n  about. CARRIED is PROXIMITY, not evidence — the distance in ` +
                `lines is printed so the guess can be priced.\n  UNPLACED is counted and attached to nothing.`);
    /* CARRIED IS ORDERED BY DISTANCE AND NOT BY POPULATION, because its rows are not alike in worth and
     * ordering by count puts the worst first. A claim a few lines below the section its file last named is a
     * good bet; one several thousand lines below it is a coincidence of file layout — and a stage-label table
     * far from any banner produces hundreds of the latter, which would head the band and bury the former. The
     * distance is the whole of what a reader has to price this band with, so it is what the band sorts on. */
    const show = (label, m, withAway) => {
      const near = (cs) => Math.min(...cs.map((c) => c.away));
      const rows = withAway
        ? [...m].sort((x, y) => near(x[1]) - near(y[1]))
        : [...m].sort((x, y) => y[1].length - x[1].length);
      console.log(`\n  ${label} — ${rows.length} sections`);
      for (const [sec, cs] of (all ? rows : rows.slice(0, HEAD * 2))) {
        const allToks = [...new Set(cs.map((c) => c.token))].sort();
        /* The coordinate list is TRUNCATED and says by how much. A section carrying two hundred distinct step
         * claims is a row nobody reads to the end, and a row that runs off the screen buries every row under
         * it — which for an ordering aid is the whole of the harm it can do. */
        const toks = allToks.length > 10 ? allToks.slice(0, 10).concat([`… +${allToks.length - 10}`]) : allToks;
        const away = withAway ? `  (${near(cs)}-${Math.max(...cs.map((c) => c.away))} lines below it)` : "";
        console.log(`    ${sec.padEnd(16)} ${String(cs.length).padStart(3)} claims  ${toks.join(" ")}${away}`);
      }
      if (!all && rows.length > HEAD * 2) console.log(`    … ${rows.length - HEAD * 2} more — --all`);
    };
    show("STATED — the claim's own line or comment block names the section", inBlock, false);
    show("CARRIED — nearest section earlier in the file; proximity, not evidence", carried, true);
  } else {
    console.log(`\n(--governed files every step claim under the section its own prose names, so one fetch of ` +
                `one algorithm\n settles a cluster rather than one claim.)`);
  }
}

main();
