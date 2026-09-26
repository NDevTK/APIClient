/* THE RECORD-LANDING GATE FOR CLAUDE.md — the construction that closes the ratchet its own opening measures.
 *
 * CLAUDE.md names this file's job in its own words, as a landed residual whose next diff it states:
 *
 *   "RETIREMENT: this record goes when a recorded argument here cannot LAND without a stated retirement
 *    condition — a check in the build that reads this file and refuses an emphasised record carrying none —
 *    because the ratchet is then closed by construction rather than by the sweep this paragraph forbids."
 *
 * THE ARGUMENT, WHICH DECIDES EVERY DESIGN CHOICE BELOW. That file's recording rules only ever ADD: a retired
 * argument is REWRITTEN RATHER THAN DELETED, a wrong next-diff clause is RECORDED WHERE IT WAS WRITTEN, an
 * incident is KEPT AT THE SITE. Each is correct and load-bearing and each is a ratchet, and the measured cost
 * is a legibility defect with a number on it — a reader scanning for principles finds the principle and stops,
 * so the one record that names their exact artifact never surfaces. The cure that file prescribes is a
 * RETIREMENT CONDITION per record. The cure it FORBIDS is a sweep: "a condition quantified over THIS FILE'S
 * OWN PROSE IS A SWEEP TARGET AND NEVER A REMOVAL LICENCE, BECAUSE AN EDIT SATISFIES IT AND ONLY A
 * CONSTRUCTION MAKES A RULE UN-RE-DERIVABLE."
 *
 * SO THIS IS SCOPED TO WHAT THIS CHECKOUT IS LANDING, AND IT IS NOT A BACKLOG IN ITS EXIT CODE. It asks ONE
 * question — did the work this checkout has not published yet put a new emphasised record into CLAUDE.md
 * without a retirement condition — and it answers it at the moment the record LANDS, which is the word the
 * residual uses. It publishes NO count of records that already carry none, because that same file rates such a
 * count as the worst status of all: "IT SHRINKS AS PEOPLE DO THE WORK AND ITS ONLY READER IS THE PERSON ABOUT
 * TO INVALIDATE IT." What replaces one is the three things that section names, and all three are here: the
 * DERIVATION as a command a reader runs (this file), the corrections that command cannot make for itself (the
 * BLIND SPOTS block), and the STRUCTURAL fact that does not rot — once this gate stands, the population of
 * uncovered records is FROZEN and can only shrink, which is the whole content of "closed by construction".
 *
 * ── THREE CHANNELS, AND THE FIRST TWO ARE THE THIRD'S BLIND SPOT ─────────────────────────────────────────
 * This file's emphasis convention is CAPITALISATION inside `**…**`, so a record announces itself with an
 * emphasised capitalised clause. Two things can make that unreadable, and both were found by building this
 * gate rather than assumed:
 *
 *   UNTERMINATED  a paragraph whose `**` count is ODD carries an emphasis that never closes. Markdown renders
 *                 the unmatched marker as literal asterisks and INVERTS everything after it on that line.
 *   INVERTED      a paragraph whose count is EVEN and whose pairing is nevertheless swapped, because it
 *                 carries an EVEN number of marker errors. Parity cannot see this, and it is not rare.
 *
 * Both are the same authoring slip with one signature, which is what makes them worth detecting as a pair: a
 * lane appending a record to a paragraph that ends in an emphasised run writes `…text. **AND THE NEW
 * HEADLINE…` and omits the closing `**` of the run it is appending after, so the new record's OPENER is
 * consumed as the old run's CLOSER. That is §AN-ARGUMENT-IS-A-HEADLINE-PLUS-ITS-TAIL arriving as markup.
 *
 * THE BLIND-SPOT RELATION IS MEASURED, NOT FEARED. At the revision this gate was built, CLAUDE.md carried
 * THREE unterminated paragraphs, and of the forty most recent commits that touched the file, FOUR landed a
 * record that the RECORD channel could not see — every one of the four on one of those three paragraphs.
 * Four of four. Repairing the markers is what arms the record channel.
 *
 * ── HOW INVERSION IS DETECTED WITHOUT ACCUSING CORRECT PROSE ─────────────────────────────────────────────
 * A capitalised clause sitting outside emphasis looks like a swapped pairing and very often is not: this file
 * writes capitalised clauses in plain prose deliberately, and a bare test for one answers 47 on a subject
 * whose real defect count is a fraction of that. CLAUDE.md: "an excluder that under-covers ACCUSES; an
 * includer that under-covers goes quiet … the accusing one needs more [suspicion], because its output looks
 * like a result." So the test is the SWAP SIGNATURE and not a single segment, and it is two-sided:
 *   (1) two capitalised clauses at ADJACENT prose indices — k and k+2, which a single deliberate caps clause
 *       in prose cannot produce; and
 *   (2) the emphasised segment BETWEEN them reading as PROSE — the other half of the swap.
 * Measured over the whole subject, that rule selects exactly the paragraphs whose pairing is demonstrably
 * swapped and nothing else, and the corroborating half is unanimous: every emphasised segment inside every
 * selected region reads as prose. A cluster of one is not reported at all.
 *
 * ── WHAT A HEADLINE IS, DERIVED FROM THE FILE'S OWN CONVENTIONS AND MEASURED BEFORE IT WAS BELIEVED ──────
 * CLAUDE.md states the convention itself — "this tree's emphasis convention is CAPITALISATION, so the
 * load-bearing statement of a mechanism is the one written in capitals" — so a headline is a `**…**` run whose
 * letters are all upper case. Capitalisation ALONE is too wide: it also takes `**AND**`, `**50**` and
 * `**three**`, single tokens that are ordinary emphasis inside a sentence and announce nothing. So a headline
 * is additionally a CLAUSE — three words and twelve letters at least. Measured over the forty most recent
 * commits that touched CLAUDE.md, that rule selects 3-5 runs per record-landing commit against exactly ONE
 * retirement condition, which is why the demand below is made PER PARAGRAPH and never per run: a record is
 * several emphasised runs and one condition, and a gate demanding one condition per run would accuse every
 * correct landing there has ever been.
 *
 * ── WHY THE EXIT CODE IS SCOPED AND THE LIST IS NOT ──────────────────────────────────────────────────────
 * A gate that carries a pre-existing population in its exit code is red on every build, so the change that is
 * the signal never comes and nobody opens the body — CLAUDE.md measured that on this tree's own record-field
 * gate. But a gate that stays SILENT about that population is worse: its green is a claim about paragraphs it
 * could not read. So the two are separated the way that file requires. The EXIT CODE carries only what this
 * checkout INTRODUCED, which is §Testing's own confound-free measurement — "keep only the findings … your diff
 * added … that answers *what did I introduce*". Everything already there is printed as a WORK QUEUE with line
 * numbers, every run, including the clean day. That is a list of DEFECTS PRESENT and not a count of
 * conditions ABSENT, which is the status the file bans.
 *
 * A PARAGRAPH THE BASE COULD NOT BE READ FOR IS NEVER ACCUSED. Where the base paragraph was itself
 * unterminated its pairing is unknown, so its inversion count is not comparable and no inversion finding may
 * be drawn from it — repairing a parity defect must not read as introducing the inversion it reveals.
 *
 * ── WHAT IT DOES NOT DO, STATED BECAUSE THE OMISSION IS DELIBERATE ───────────────────────────────────────
 * It never asks whether a retirement condition is a GOOD one. CLAUDE.md spends several records on how such a
 * condition goes wrong — one satisfied by an ABSENCE rather than by a construction, one quantified over this
 * file's own prose — and every one of those is a judgement about a sentence's meaning that no text scan can
 * make. A gate that guessed would accuse, and the accusation would land on the author who had thought hardest
 * about the condition. It asks only whether one was STATED, which is the half a machine can settle.
 *
 * Usage:  node engine/mdgate.mjs [--base <rev>] [--subject <path>] [--md <path>]
 *         --base     what this checkout is landing ON TOP OF; default `origin/main`.
 *         --subject  the file to judge; default the working tree's CLAUDE.md, because a record is refused at
 *                    the moment it lands and that is before it is committed.
 *         --md       the path within the base revision; default CLAUDE.md.
 */
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { publicationBase } from "./gate_revision.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..");

const argOf = (name, dflt) => {
  const i = process.argv.indexOf(name);
  if (i < 0) return dflt;
  if (i + 1 >= process.argv.length)
    throw new Error(`[md-gate] ${name} was given with no value. A flag that names nothing selects whatever a ` +
                    `default picked and reads as a deliberate answer.`);
  return process.argv[i + 1];
};
const MD_PATH = argOf("--md", "CLAUDE.md");
const SUBJECT = argOf("--subject", join(ROOT, MD_PATH));
/* THE BASE IS RECOVERED, NEVER SPELLED — see engine/gate_revision.mjs's publicationBase for why the obvious
   spelling is wrong wherever this gate is most likely to run. `origin/main` stood here as the default and it
   is a claim about the PUBLICATION TIP only in a checkout whose origin is the real remote; inside the frozen
   snapshot this project's own testing rule mandates, that ref was rebuilt by `git clone` out of the source's
   LOCAL branches, so it named a branch nobody advances and the base was 499 commits stale. This gate PRINTED
   the resolved sha every run and nobody read it, which is the chronic-red shape one level up: the stage was
   the deciding one, it was red on every frozen build, and its own text told the reader that everything it
   listed was in work this checkout had not published — an accusation aimed at whoever was holding the tree.
   AN EXPLICIT --base STILL WINS AND IS RESOLVED HERE, because a caller who names a revision is stating one;
   what is removed is the DEFAULT, which is the half that could be silently wrong. */
const BASE_ARG = argOf("--base", null);
const BASE = BASE_ARG ? { sha: null, repo: ROOT, hops: [], why: null } : publicationBase(ROOT, "main");
const BASE_REV = BASE_ARG ?? `origin/main`;

const log = (s) => console.log(`[md-gate] ${s}`);
const bad = (s) => console.error(`[md-gate] ${s}`);

/* ── THE PARSE ────────────────────────────────────────────────────────────────────────────────────────────
   ONE LINE IS ONE PARAGRAPH IN THIS FILE — not an assumption about Markdown but a measured property of the
   subject, and it is what makes a LINE-scoped diff useless here: every commit that has ever landed a record
   into CLAUDE.md reports `1 insertion, 1 deletion`, because appending a record REWRITES the paragraph it is
   appended to. A line-number intersection would therefore mark every record on that paragraph as introduced,
   which on the longest paragraphs is dozens. The unit of this gate's row is the RECORD, never the line. */
/* A CODE SPAN IS A SPELLING BEING SHOWN AND NEVER A CLAIM BEING MADE, SO IT IS MASKED BEFORE ANYTHING IS
   COUNTED — and this gate needs the rule more than most, because the one paragraph that must explain the
   convention is the one that has to WRITE `**` inside backticks to do it. CLAUDE.md states the general form:
   "wherever an instrument partitions text into CLAIMED and SHOWN, ask which half its failures fall into. An
   excluder that under-covers ACCUSES; an includer that under-covers goes quiet … the accusing one needs more
   [suspicion], because its output looks like a result."
   IT WAS FOUND BY USING THIS GATE ON ITS OWN LANDING RATHER THAN BY READING THE RULE. Without the mask, the
   diff that retires the record this gate was built for reads as breaking that paragraph's emphasis, because
   the sentence explaining what a headline IS contains `**AND**` and `**50**` as examples. Measured at the
   revision it landed: ZERO code spans in the subject contained `**` and backticks balanced on every line, so
   the mask changes no standing verdict — it is there for the paragraph that explains the convention and for
   the retirement clause a future author may quote inside one.
   THE MASK PRESERVES LENGTH AND CARRIES NO CAPITALS, so a span can neither hide a marker nor supply the
   capitalised clause the swap signature is made of. A line whose backticks do not pair is left alone and
   reported, because a guessed span boundary is the accusing direction twice over. */
const CODE_SPAN = /`[^`]*`/g;
const maskSpans = (line) =>
  ((line.match(/`/g) || []).length % 2) ? line : line.replace(CODE_SPAN, (m) => "x".repeat(m.length));

const norm = (s) => s.replace(/\s+/g, " ").trim();
const lettersOf = (s) => s.replace(/[^A-Za-z]/g, "");
const upFrac = (s) => { const l = lettersOf(s); return l.length ? l.replace(/[^A-Z]/g, "").length / l.length : 0; };

/* A HEADLINE IS A CAPITALISED CLAUSE — see the banner. The thresholds are the file's own convention measured,
   not a tuning knob: three words and twelve letters is what separates a record's announcement from `**AND**`,
   `**50**` and `**three**`, which are ordinary emphasis inside a sentence and announce nothing. */
const HEADLINE_MIN_LETTERS = 12;
const HEADLINE_MIN_WORDS = 3;
const isHeadline = (t) =>
  lettersOf(t).length >= HEADLINE_MIN_LETTERS && lettersOf(t) === lettersOf(t).toUpperCase() &&
  norm(t).split(" ").length >= HEADLINE_MIN_WORDS;

/* THE SWAP SIGNATURE — see the banner for why a single capitalised clause outside emphasis is NOT one. */
const CAPS_CLAUSE = /[A-Z][^a-z]{39,}/g;
const CLAUSE_MIN_LETTERS = 40;
const hasCapsClause = (seg) =>
  [...seg.matchAll(CAPS_CLAUSE)].some((m) => lettersOf(m[0]).length >= CLAUSE_MIN_LETTERS &&
                                             upFrac(m[0]) >= 0.95 && norm(m[0]).split(" ").length >= 3);

/* THE RETIREMENT CLAUSE, IN THE FILE'S OWN SPELLING. `RETIREMENT:` is the canonical form and `RETIREMENT —`
   is the form a MET one takes; both are read. This gate never reads what such a clause SAYS, only that one
   was stated — see the banner for why the other half is deliberately not asked. */
const RETIREMENT = /RETIREMENT(?::| —)/;
const RETIREMENT_G = /RETIREMENT(?::| —)/g;
const RETIREMENT_TEXT = /RETIREMENT(?::| —)\s*([^*]{0,120})/g;

/* ONE PARAGRAPH'S READING. `markers` odd means the emphasis never closes and NOTHING about this paragraph's
   pairing can be trusted; `inverted` means the pairing is swapped although the count is even. Either way the
   runs are not returned, because a guessed pairing reports prose as a record and that is the accusing
   direction this gate spends its precision on. */
function readParagraph(line) {
  const parts = maskSpans(line).split("**");
  const markers = parts.length - 1;
  if (markers === 0) return { markers, state: "plain", runs: [] };
  if (markers % 2) return { markers, state: "unterminated", runs: [] };
  const proseCaps = [];
  for (let k = 0; k < parts.length; k += 2) if (hasCapsClause(parts[k])) proseCaps.push(k);
  const cluster = proseCaps.filter((k, j) => proseCaps[j + 1] === k + 2 || proseCaps[j - 1] === k - 2);
  /* THE SECOND SIDE OF THE SWAP: the emphasised segment between two clustered prose-caps reads as prose. */
  const between = cluster.slice(0, -1).map((k) => parts[k + 1]).filter(Boolean);
  const proseLike = between.filter((s) => lettersOf(s).length > 60 && upFrac(s) < 0.3).length;
  if (cluster.length >= 2 && proseLike === between.length && between.length)
    return { markers, state: "inverted", runs: [], cluster };
  const runs = [];
  for (let k = 1; k < parts.length; k += 2) runs.push(norm(parts[k]));
  return { markers, state: "read", runs };
}
const readFile = (text) => text.split("\n").map((line, i) => ({ n: i + 1, line, ...readParagraph(line) }));

/* ── THE BASE ─────────────────────────────────────────────────────────────────────────────────────────────
   WHAT THIS CHECKOUT IS LANDING ON TOP OF, READ BY CONTENT AND NEVER BY ANCESTRY. CLAUDE.md records what a
   shallow clone does to `merge-base --is-ancestor` and to `rev-list --count` — a graft makes parent and child
   read as DIVERGED — and this repository IS shallow. So no ancestry question is asked: the base is a REVISION
   whose CLAUDE.md is read, the introduced set is a difference over its CONTENT, and the resolved SHA is
   printed beside every verdict. Where the base cannot be read, every scoped channel reports NOT ASKED rather
   than treating an unreadable base as an empty one — which would report the whole file as landing today. */
/* AND IT IS READ IN THE REPOSITORY THAT HAS THE OBJECT, WHICH IS NOT ALWAYS THIS ONE. Resolving the base sha
   is half the job: measured on this box, no snapshot carries an objects/info/alternates file and a clone
   brings only what the source's local heads reach, so a snapshot cannot name a commit that landed in its
   source after it was made — a perfectly real base sha, unreadable here. publicationBase returns the
   repository alongside the sha for exactly that reason, and this read is put to it. */
function readBase(rev, path, repo) {
  const git = (args) => execFileSync("git", args, { cwd: repo, encoding: "utf8", maxBuffer: 1 << 30,
                                                    stdio: ["ignore", "pipe", "ignore"] });
  try {
    const sha = git(["rev-parse", "--verify", `${rev}^{commit}`]).trim();
    return { sha, text: git(["show", `${sha}:${path}`]) };
  } catch (e) { return { sha: null, text: null, why: String(e.message || e).split("\n")[0] }; }
}

/* ── THE RUN ──────────────────────────────────────────────────────────────────────────────────────────────*/
const subjectText = readFileSync(SUBJECT, "utf8");
const subject = readFile(subjectText);
const base = (BASE_ARG || BASE.sha)
  ? readBase(BASE_ARG ?? BASE.sha, MD_PATH, BASE.repo ?? ROOT)
  : { sha: null, text: null, why: BASE.why };
const baseParas = base.text === null ? null : readFile(base.text);
const scoped = baseParas !== null;

const runsRead = subject.reduce((a, p) => a + p.runs.length, 0);
const headlines = subject.flatMap((p) => p.runs.filter(isHeadline).map((txt) => ({ n: p.n, txt })));
const retOccurrences = subject.reduce((a, p) => a + (maskSpans(p.line).match(RETIREMENT_G) || []).length, 0);
const retParagraphs = subject.filter((p) => RETIREMENT.test(maskSpans(p.line))).length;

log(`subject ${SUBJECT}`);
/* THE BASE LINE NAMES WHERE IT WAS READ, NOT ONLY WHAT IT RESOLVED TO. A sha alone is what this gate printed
   while it was reading a branch nobody advances: a real commit, with a real date, about the wrong tree. The
   repository and the hop count are the two facts that distinguish a publication tip from a ref a clone
   manufactured, and they are the ones a reader needs to disbelieve the number above them. */
log(`base    ${BASE_REV} → ${base.sha ?? "UNRESOLVED"}${base.sha ? "" : ` (${base.why})`}`);
log(`        read in ${BASE.repo ?? ROOT}` +
    (BASE_ARG ? ` (named by --base, resolved in this tree)`
     : BASE.hops.length ? ` — this tree is a clone ${BASE.hops.length} hop(s) down, so ITS OWN origin/main is `
                          + `a ref \`git clone\` rebuilt from that repository's local branches and is not a `
                          + `publication tip`
     : ` — origin is a remote, so this tree's own origin/main is the publication tip`));

/* WHAT THIS RUN READ, WHICH IS THE DENOMINATOR EVERY NUMBER BELOW IS A FRACTION OF. CLAUDE.md: "a coverage
   figure states WHAT IT IS A FRACTION OF, in the same line, or it is not a coverage figure." These count what
   EXISTS and never what is missing — the second is the status that file bans outright.
   BOTH COUNTING DERIVATIONS ARE PRINTED, because this file's lines are PARAGRAPHS and CLAUDE.md records a
   measured FIVEFOLD understatement from counting lines where occurrences were meant. `grep -c` answers how
   many PARAGRAPHS hold a match and `grep -o … | wc -l` answers how many MATCHES there are; on this subject
   they are different questions with different answers, and a reader handed one of them cannot tell which. */
const readable = subject.filter((p) => p.state === "read" || p.state === "plain").length;
log(`read    ${subject.length} paragraph(s), ${readable} whose emphasis this gate can pair, ` +
    `${runsRead} emphasised run(s), ${headlines.length} headline-shaped, ` +
    `${retOccurrences} retirement condition(s)`);
log(`        the two derivations, which answer different questions and disagree on this subject:`);
log(`          occurrences  grep -o 'RETIREMENT\\(:\\| —\\)' ${MD_PATH} | wc -l   → ${retOccurrences}`);
log(`          paragraphs   grep -c  'RETIREMENT\\(:\\| —\\)' ${MD_PATH}          → ${retParagraphs}`);

/* ── THE MARKER CHANNELS, SPLIT INTO WHAT THIS CHECKOUT INTRODUCED AND WHAT WAS ALREADY THERE ─────────────*/
const baseOf = (n) => (baseParas && baseParas[n - 1]) || null;
const broken = subject.filter((p) => p.state === "unterminated" || p.state === "inverted");
const introducedMarkers = [], standingMarkers = [], incomparable = [];
for (const p of broken) {
  const b = scoped ? baseOf(p.n) : null;
  if (!scoped) { standingMarkers.push(p); continue; }
  if (!b) { introducedMarkers.push(p); continue; }              /* a paragraph this checkout added */
  /* PARITY IS COMPARABLE WHATEVER THE BASE WAS, AND INVERSION IS NOT — which is the whole of this split and
     was wrong here until the gate failed to accuse the diff that landed it. An ODD marker count is a
     well-defined property of a paragraph whether or not its base copy paired, so a subject that is
     UNTERMINATED where the base was not is a regression this checkout introduced even if the base was
     INVERTED. An INVERSION, by contrast, cannot be read off a base whose pairing was unknown, so repairing a
     parity defect must never report the inversion it reveals as new. Getting this backwards is the quiet
     direction — it under-accuses — and it under-accused its own author: the rewrite that retired the record
     this gate was built for left that paragraph unterminated, and the gate filed it as STANDING. */
  if (p.state === "unterminated")
    { (b.state === "unterminated" ? standingMarkers : introducedMarkers).push(p); continue; }
  if (b.state === "unterminated") { incomparable.push(p); continue; }
  if (b.state === "inverted") { standingMarkers.push(p); continue; }
  introducedMarkers.push(p);
}

/* ── THE RECORD CHANNEL ───────────────────────────────────────────────────────────────────────────────────*/
const recordFindings = [];
if (scoped) {
  const baseRuns = new Set(baseParas.flatMap((p) => p.runs));
  /* THE RE-MARKING DISCRIMINATOR. A run whose text is already in the base, in ANY form, is text that was
     already there; making it parse as emphasis is a REPAIR and not a new argument. Without this, repairing an
     unterminated paragraph reads as dozens of records landing at once — measured on the commit that repaired
     two such paragraphs, 66 runs became newly parseable and every one of their texts was already in the base. */
  const baseFlat = norm(base.text);
  const baseRets = new Set();
  for (const p of baseParas) for (const m of maskSpans(p.line).matchAll(RETIREMENT_TEXT)) baseRets.add(norm(m[1]));
  const byPara = new Map();
  for (const h of headlines)
    if (!baseRuns.has(h.txt) && !baseFlat.includes(h.txt))
      (byPara.get(h.n) ?? byPara.set(h.n, []).get(h.n)).push(h.txt);
  for (const [n, runs] of byPara) {
    const gained = [...maskSpans(subject[n - 1].line).matchAll(RETIREMENT_TEXT)]
      .map((m) => norm(m[1])).filter((c) => !baseRets.has(c) && !baseFlat.includes(c.slice(0, 60)));
    if (!gained.length) recordFindings.push({ n, runs });
  }
}

/* ── THE VERDICTS, NEVER SUMMED ───────────────────────────────────────────────────────────────────────────
   CLAUDE.md: "a gate states its FINDINGS and its BLIND SPOTS as separate verdicts, because an instrument that
   cannot see something has not found anything", and the measured consequence of summing them is a gate that
   is red on every build, so the change that is the signal never comes and nobody opens the body. Every band
   below prints on EVERY run including the clean day, "because a line that appears only on the bad day is one
   nobody learns to look for." */
log("");
log("STANDING — printed every run, carried by NO exit code, because it was here before this checkout:");
if (!standingMarkers.length && !incomparable.length)
  log(`  nothing. Every paragraph's emphasis pairs, so the record channel read all ${subject.length} of them.`);
for (const p of standingMarkers)
  log(`  ${MD_PATH}:${p.n}  ${p.state.toUpperCase()} (${p.markers} \`**\`) — a record landing in this ` +
      `paragraph is INVISIBLE to the record channel. The repair is a MARKER and never a word: find the ` +
      `emphasised run whose closing \`**\` was consumed as the opener of the headline appended after it.`);
for (const p of incomparable)
  log(`  ${MD_PATH}:${p.n}  ${p.state.toUpperCase()} (${p.markers} \`**\`) — NOT COMPARABLE: the base's copy ` +
      `of this paragraph was itself unterminated, so its pairing is unknown and no introduction can be read ` +
      `from the difference. Repairing a parity defect must not report the inversion it reveals as new.`);
log("");
log("BLIND SPOTS — stated, separately, and no exit code carries them:");
if (!scoped)
  log(`  EVERY SCOPED CHANNEL WAS NOT ASKED: ${BASE_REV} could not be read, so there is no base to take an ` +
      `introduced set against. An unreadable base is not an empty one.`);
log(`  NOT ASKED BY DESIGN: whether a stated retirement condition is a GOOD one. A condition satisfied by an ` +
    `ABSENCE, or quantified over this file's own prose, is the failure CLAUDE.md spends records on — and it ` +
    `is a judgement about a sentence's meaning, which a text scan that guessed would get wrong in the ` +
    `accusing direction. This gate asks only that one was STATED.`);
log(`  NOT COUNTED BY DESIGN: how many records already carry no condition. CLAUDE.md: "A COUNT OF WHAT IS ` +
    `MISSING IS THE WORST STATUS OF ALL, BECAUSE IT SHRINKS AS PEOPLE DO THE WORK AND ITS ONLY READER IS THE ` +
    `PERSON ABOUT TO INVALIDATE IT." This gate is the derivation that replaces it, and the structural fact ` +
    `that does not rot is this: with it standing, that population is FROZEN and can only shrink.`);

log("");
/* AN UNASKABLE QUESTION MAY NOT REPORT A PASS, AND THIS GATE'S OWN BLIND-SPOT LINE IS WHAT SAYS SO — it reads
   AN UNREADABLE BASE IS NOT AN EMPTY ONE, and the exit code then said it was: with no base, nothing lands in
   the scoped channels, the terminal line claimed this checkout had landed no record without a condition, and
   the process exited 0. That is a claim made on no evidence at the one place a reader stops, and it is the
   defaulted-field defect performed on a verdict — absent rendered as clean. It is also the flattering
   direction, so nothing downstream ever contradicts it.
   IT IS A REFUSAL AND NOT A FINDING, so it is worded and counted as one: the findings ARE the disagreement,
   and a base that could not be read is a disagreement with nothing. The remedy is named because the caller
   can always supply one. */
if (!scoped) {
  bad(`FAILED (no base) — ${BASE_REV} could not be read, so the introduced set is UNKNOWN and this gate has ` +
      `no answer to give. ${base.why ?? "no reason was recorded"}. It does not pass: a verdict that cannot ` +
      `be reached is not a clean one, and reporting it as clean is how an unasked question becomes a green ` +
      `stage. Name a revision with \`--base <rev>\` — one this gate can READ, which for a checkout cloned ` +
      `from another repository means a revision that repository still has.`);
  process.exit(1);
}
if (!introducedMarkers.length && !recordFindings.length) {
  log(`PASS (findings) — this checkout landed no emphasised record into ${MD_PATH} without a retirement ` +
      `condition, and broke no paragraph's emphasis. That verdict covers the ${readable} paragraph(s) this ` +
      `gate can pair and says nothing about the ${standingMarkers.length + incomparable.length} listed above.`);
  process.exit(0);
}
bad("FINDINGS — introduced by this checkout, and the exit code carries these:");
for (const p of introducedMarkers)
  bad(`  ${MD_PATH}:${p.n}  ${p.state === "unterminated" ? "UNTERMINATED EMPHASIS" : "INVERTED EMPHASIS"} ` +
      /* THE VERDICT NAMES THE PROPERTY THAT MOVED AND NEVER MORE THAN THAT. A subject that is UNTERMINATED
         where the base was INVERTED is a parity regression and its base did NOT pair, so a line claiming it
         did would be a gate stating a falsehood about its own evidence — which is the one thing an instrument
         may not do, since its reader has no second copy to check it against. */
      `(${p.markers} \`**\`) — ${p.state === "unterminated"
        ? `this paragraph's \`**\` count is EVEN in ${BASE_REV} and ODD here`
        : `this paragraph's emphasis pairs in ${BASE_REV} and is swapped here`}. ` +
      `${p.state === "unterminated" ? "One marker has no partner" : "An even number of markers is misplaced"}` +
      `, so Markdown renders the run as literal asterisks and INVERTS the emphasis after it: the headlines ` +
      `render as prose and the prose renders as a headline. The repair is a MARKER and never a word — the ` +
      `usual slip is a record appended after an emphasised run whose closing \`**\` was then consumed as the ` +
      `new headline's opener.`);
for (const { n, runs } of recordFindings) {
  bad(`  ${MD_PATH}:${n}  RECORD LANDED WITH NO RETIREMENT CONDITION — this paragraph gained ${runs.length} ` +
      `emphasised headline(s) against ${BASE_REV} and gained no \`RETIREMENT:\` clause. Every recorded ` +
      `argument states what would retire it, on the condition CLAUDE.md names: the rule it warns against must ` +
      `no longer be RE-DERIVABLE, usually because the code has made the state impossible. State it in the ` +
      `same paragraph, and state it over the TREE, the BUILD or an ARTIFACT — a condition satisfied by ` +
      `something NOT EXISTING, or quantified over this file's own prose, is met by an edit while nothing has ` +
      `been built, which is a sweep target and never a removal licence.`);
  for (const r of runs) bad(`      + ${r.slice(0, 140)}`);
}
bad(`FAILED — ${introducedMarkers.length} paragraph(s) whose emphasis this checkout broke, ` +
    `${recordFindings.length} record(s) it landed with no retirement condition. There is no baseline to ` +
    `update and no allowlist: the findings ARE the disagreement, and every one of them is in work this ` +
    `checkout has not published yet.`);
process.exit(1);
