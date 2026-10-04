/* IS THIS DIFF PROSE-ONLY? ASK THE PREPROCESSOR, NOT THE AUTHOR.
 *
 * CLAUDE.md requires a large share of this project's diffs to be comment-only: a retired argument is REWRITTEN
 * rather than deleted, an incident is kept at its site, a wrong next-diff clause is recorded where it was
 * written. The evidence offered for such a diff is always the same sentence — `I only added comments` — and that
 * is a claim about the EMITTED PROGRAM which no gate here measures, because a gate measures behaviour and a
 * comment changes none. A green build is consistent with a prose diff AND with a statement you did not notice
 * you were adding, and §AN-ASSERT-WHOSE-TWO-SIDES-CANNOT-DISAGREE is the name for a check that cannot tell them
 * apart.
 *
 * WHY IT IS A FILE AND NOT A PARAGRAPH. CLAUDE.md states this pipeline in prose and names the construction that
 * would end it: "this record goes when a prose-only residue in this tree is produced by a committed helper that
 * drops blank lines by construction, so the filter cannot be left out of the pipeline a reader types." Every
 * clause of that prose has been got wrong at least once BY ITS OWN AUTHOR, and each mistake is a stage of the
 * pipeline below rather than a lapse of care:
 *   §A-MEASUREMENT-CAN-OUTLIVE-ITS-INSTRUMENT — the pipeline was typed into a scratch directory, used to verify
 *     a published commit, and reclaimed with the container. An instrument whose output anyone quotes is
 *     COMMITTED, in the same diff as the first quotation of its number.
 *   THE `__FILE__` ARTIFACT — preprocessing two copies at two PATHS makes `__FILE__` differ, and in this tree a
 *     `DCHECKF` expands its own file name, so the raw diff is dominated by an artifact of HOW it was asked. One
 *     measured run reported 232 differing lines of which every one was that, and another reported 1216 because
 *     the normalising substitution lacked a `/g` and a second occurrence survived on the same line.
 *   THE BLANK-LINE HOLE — `clang -E` replaces a comment with its NEWLINES, so a comment's HEIGHT survives into
 *     the output, and a mask over CHARACTERS cannot touch a line that has none. Changing a comment's height is
 *     the MODAL prose diff here, so the recipe's safest input was the one that failed it: 8 differing lines,
 *     every one blank, read as the recipe's own strongest refusal (`a NON-NUMERIC difference is never
 *     admissible`) and therefore as a statement the author had not noticed writing.
 *   THE PARTIAL-SUM HOLE — a stamp below an insertion shifts by every insertion ABOVE it, so a diff with N
 *     insertion hunks leaves the set of CUMULATIVE PARTIAL SUMS of the hunk nets and only the LAST equals NET.
 *     The rule was written as `equal to your NET count` from two SINGLE-HUNK examples, which is the one shape
 *     where a hunk's partial sum IS the net — a cure validated by the absence of the case it cannot see.
 *   INSERTIONS AGAINST NET — `git diff --stat` prints INSERTIONS first, so the number a reader reaches for is
 *     the larger one, and a rewrite-rather-than-delete commit has deletions in it. The residue is then SMALLER
 *     than the figure being checked against, which reads as a shift that does not account for itself.
 * Each of those failed in the ACCUSING direction — toward reporting a correct commit as having emitted code —
 * which §WHEREVER-AN-INSTRUMENT-PARTITIONS-TEXT rates as needing more suspicion than the quiet one, because its
 * output looks like a result.
 *
 * WHAT IT ANSWERS, AND IN WHICH ORDER. Two questions, kept apart, because they are not alike in what a reader
 * may conclude:
 *   (1) IS ANY NON-NUMERIC TEXT DIFFERENT. Answered with every integer masked, so it is decided without
 *       interpreting a single number. Nonzero is a STATEMENT, an expression or a datum, and the lines are
 *       printed.
 *   (2) ARE THE NUMERIC DIFFERENCES ACCOUNTED FOR BY THE LINE SHIFT. The admissible residue is the set of
 *       cumulative partial sums of the per-hunk nets, whose MAXIMUM is the net; a value above the net, or a
 *       value no partial sum explains, is not a line shift.
 * A prose-only claim needs (1) at zero AND (2) accounted. Either one alone is half the check.
 *
 * IT NEVER TOUCHES THE SHARED TREE. Both sides are COPIES in a temp directory — the working tree is READ and
 * never written, because an earlier form of this pipeline swapped the base file into place to preprocess it,
 * which in a shared checkout is a window in which a peer reads a stale file and a crash leaves it there.
 *
 * ITS INCLUDE ROOTS ARE DERIVED FROM `build.mjs`, NEVER LISTED HERE, for §AN-AUDITOR-DERIVES-THE-RULE's reason:
 * a hand-kept copy of the compiler's own flag set is a second copy, and the one that drifts is the copy nobody
 * runs against a compile. It parses the `-I` line that is about to compile the tree and resolves each name from
 * that same file's own `const` definitions, and it THROWS rather than falling back when it cannot — a silent
 * fallback to a stale list is how a residue check comes to preprocess a different program than the build does.
 *
 * THE CONTROL RUNS BY ITSELF AND A CLEAN BILL IS REFUSED WITHOUT IT. §A-CONTROL-ARMS-ONLY-ON-A-SITE: probing an
 * instrument for silence rests on a step nobody performs — DEMONSTRATING THAT THE PROBE ARMS THE CHECK AT ALL —
 * and skipping it makes a zero mean `my input never reached this check`, which renders identically to `this
 * check cannot see this`. So before reporting any file as prose-only, this injects ONE statement into the new
 * copy and requires the pipeline to report it. If the control does not speak, nothing below it is published.
 *
 * NAMED RESIDUAL — CORRECT AND NARROWER. WHAT IS NOT COVERED: a change inside a HEADER, whose emission this
 * cannot see except through a `.c` that includes it — so a header-only diff is reported as `no translation unit`
 * rather than as prose-only, and the honest way to clear one is to run this over a `.c` that includes it.
 * WHAT THE NEXT DIFF BUILDS: resolve each named header to its includers by parsing their `#include` lines and
 * run the residue over those, reporting the union — which needs no new mechanism, only the includer set.
 * HOW ITS ABSENCE WOULD SHOW: a reader passes a `.h` to this, is told there is no translation unit, and either
 * stops (a prose claim nothing checked) or picks an includer by hand and reports a residue over whichever one
 * they happened to choose. */
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, basename, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(ENGINE, "..");
const die = (code, ...lines) => { for (const l of lines) process.stderr.write("[prosediff] " + l + "\n");
                                  process.exit(code); };
/* EVERY SPAWN CARRIES THE BUFFER AND EVERY SPAWN'S `error` IS READ, which is not defensive plumbing — it is the
   defect this instrument exists to catch, caught by this instrument against itself on its first real run.
   `spawnSync`'s default `maxBuffer` is ONE MEGABYTE and an exceeded buffer sets `error` to ENOBUFS and returns
   stdout TRUNCATED, which is the §AND-THE-CHEAPEST-WAY-TO-GET-THAT-LIST-WRONG shape with no caveat line to drop:
   a `git show` of a large translation unit came back 813 lines short, both sides preprocessed cleanly, and the
   residue read 817 gained where the pipeline it replaces read 3. The verdict was NOT PROSE-ONLY, which is this
   check's accusing arm, published against a commit whose three statements it had also correctly named — so the
   finding looked corroborated by its own first three lines.
   IT WAS FOUND BY DISAGREEMENT AND BY NOTHING ELSE, which is the part to keep: the preprocessed LINE COUNT of
   each side is printed on every run for that reason, because a residue is a difference and a difference cannot
   say which of its two operands is wrong. A reader with one number has no way to see a truncated operand; a
   reader with both sees 13593 against 14406 and the question answers itself. */
const MAXBUF = 1 << 29;
const run = (cmd, args, opts = {}) => {
  const r = spawnSync(cmd, args, { cwd: REPO, encoding: "utf8", maxBuffer: MAXBUF, ...opts });
  if (r.error) die(4, "`" + cmd + " " + args.join(" ") + "` failed: " + r.error.message,
                     "A truncated or failed spawn silently shortens one side of the comparison, which reads as",
                     "a residue rather than as a missing operand. Nothing is published off it.");
  return r;
};

/* THE COMPILER'S OWN INCLUDE ROOTS, READ OFF THE LINE THAT COMPILES THIS TREE. The `-I` flags are built from
   four names in build.mjs and each is a `join` of `ENGINE` with a literal, so the resolution is: find the flag
   line, take its expressions in order, and resolve every identifier from that file's own `const <id> = …`. A
   shape this cannot parse THROWS, because the alternative — a hand-written list — is the second copy this
   derivation exists to avoid, and a residue taken against the wrong include set is a residue of a different
   program. */
function includeRoots() {
  const src = readFileSync(join(ENGINE, "build.mjs"), "utf8");
  const line = src.split("\n").find((l) => /^\s*"-I"\s*\+/.test(l));
  if (!line) die(3, "build.mjs has no `\"-I\" + …` flag line — its shape moved, so the include roots this",
                    "would preprocess with are no longer derivable. Fix THIS parser against that line; do not",
                    "paste a list here, which is the second copy the derivation exists to avoid.");
  /* SPLIT ON TOP-LEVEL COMMAS ONLY. `"-I" + join(HOST, "browser")` carries a comma INSIDE a call, so a plain
     `split(",")` hands the resolver the fragment `join(HOST` and it refuses — which is the right failure
     direction and was the first thing this parser did. Depth-counting is the fix, and the refusal above is what
     made it visible rather than a wrong root silently reached. */
  const exprs = [];
  { let depth = 0, cur = "";
    for (const ch of line) {
      if (ch === "(") depth++;
      else if (ch === ")") depth--;
      if (ch === "," && depth === 0) { exprs.push(cur.trim()); cur = ""; continue; }
      cur += ch;
    }
    exprs.push(cur.trim()); }
  const flags = exprs.filter((s) => s.startsWith('"-I"'));
  const defs = new Map();
  for (const m of src.matchAll(/^const\s+([A-Z_]+)\s*=\s*(.+?);\s*$/gm)) defs.set(m[1], m[2]);
  const resolveName = (id, depth = 0) => {
    if (depth > 4) die(3, "include-root name `" + id + "` resolves through more than four `const`s");
    if (id === "ENGINE") return ENGINE;
    const rhs = defs.get(id);
    if (!rhs) die(3, "build.mjs defines no `const " + id + "` — the include-root derivation cannot resolve it");
    const j = /^join\(\s*([A-Za-z_]+)\s*,\s*"([^"]+)"\s*\)$/.exec(rhs);
    if (j) return join(resolveName(j[1], depth + 1), j[2]);
    const alias = /^([A-Z_]+)$/.exec(rhs);
    if (alias) return resolveName(alias[1], depth + 1);
    die(3, "cannot resolve `const " + id + " = " + rhs + "` to a directory");
  };
  const roots = [];
  for (const e of flags) {
    const tail = e.replace(/^"-I"\s*\+\s*/, "");
    const j = /^join\(\s*([A-Za-z_]+)\s*,\s*"([^"]+)"\s*\)$/.exec(tail);
    roots.push(j ? join(resolveName(j[1]), j[2]) : resolveName(tail));
  }
  if (!roots.length) die(3, "the `-I` line parsed to no roots at all");
  return roots;
}

/* NORMALISE, IN THE ORDER THE STAGES DEPEND ON. Linemarkers first (they shift under any insertion and re-align
   `diff` so untouched lines read as changed); BLANK LINES SECOND AND BEFORE THE MASK, because a blank line is
   not a numeric change and a mask applied first has already classified it as one; the path last of the three,
   GLOBALLY, since one line can expand `__FILE__` twice. The integer mask is applied on top as a SECOND view
   rather than in place, so question (1) and question (2) read the same text differently instead of one of them
   reading text the other has already destroyed. */
const stripMarkers = (s) => s.split("\n").filter((l) => !/^# [0-9]/.test(l) && l.trim() !== "").join("\n");
const normPaths = (s, paths) => { for (const p of paths) s = s.split(p).join("NORMALISED"); return s; };
const maskInts = (s) => s.replace(/[0-9]+/g, "N");

function preprocess(file, roots, dev, paths) {
  const r = run("clang", ["-E", "-DAPICLIENT_DEV=" + dev,
                          ...roots.flatMap((x) => ["-I", x]), "-I", dirname(file), file],
                {});
  if (r.status !== 0)
    die(4, "clang -E failed on " + file + " at -DAPICLIENT_DEV=" + dev,
           (r.stderr || "").split("\n").slice(0, 6).join("\n"),
           "A residue check cannot be read off a file that does not preprocess; fix the compile first.");
  return normPaths(stripMarkers(r.stdout), paths);
}

/* THE PER-HUNK NETS, AND THE CUMULATIVE PARTIAL SUMS THAT ARE THE ADMISSIBLE RESIDUE. Read off the unified diff
   rather than off `--numstat`, because `--numstat` gives the FILE's totals and what a line stamp shifts by is
   the sum of the hunks ABOVE it. The last partial sum is the net; the others are smaller and are equally
   admissible, which is the clause two single-hunk measurements could not have found. */
function hunkPartialSums(path, base) {
  const d = run("git", ["diff", "-U0", base, "--", path]).stdout || "";
  const sums = [];
  let add = 0, del = 0, seen = false, acc = 0;
  const flush = () => { if (seen) { acc += add - del; sums.push(acc); } add = del = 0; };
  for (const l of d.split("\n")) {
    if (l.startsWith("@@")) { flush(); seen = true; continue; }
    if (!seen) continue;
    if (l.startsWith("+") && !l.startsWith("+++")) add++;
    else if (l.startsWith("-") && !l.startsWith("---")) del++;
  }
  flush();
  return sums;
}

/* THE DIFF AS COMMANDS AND NEVER AS TWO FLAT LISTS, which is a correctness requirement of question (2) rather
   than tidiness. A line-stamp shift appears as a CHANGE command whose `<` and `>` blocks pair up inside that
   command; an inserted statement appears as an APPEND with no `<` side at all. Zipping the two flat lists by
   index mixes them: the first gained line is then paired with the first lost line regardless of which command
   each came from, every later pair is off by the number of pure insertions, and the mask test rejects nearly all
   of them — so a file with BOTH a statement and a stamp shift reported an EMPTY delta set, which reads as `no
   numeric difference at all` on exactly the diff that has the most of them. Measured on this instrument's own
   second run, against a file whose three statements it had correctly named one line above.
   THE FLAT READING WAS CORRECT FOR THE PROSE-ONLY CASE AND THAT IS WHY IT SURVIVED: a comment-only diff produces
   change commands and nothing else, so the lists align by luck and the delta set is right. §A-CURE-VALIDATED-ON-
   A-SHORT-EXAMPLE, with the validating case being the one the instrument is mostly pointed at. */
function diffCommands(a, b) {
  const dir = mkdtempSync(join(tmpdir(), "prosediff-"));
  try {
    writeFileSync(join(dir, "a"), a); writeFileSync(join(dir, "b"), b);
    const d = run("diff", [join(dir, "a"), join(dir, "b")]).stdout || "";
    const cmds = [];
    let cur = null;
    for (const l of d.split("\n")) {
      const m = /^[0-9]+(?:,[0-9]+)?([acd])[0-9]+(?:,[0-9]+)?$/.exec(l);
      if (m) { cur = { op: m[1], lost: [], gained: [] }; cmds.push(cur); continue; }
      if (!cur) continue;
      if (l === "---") continue;
      if (l.startsWith("< ")) cur.lost.push(l.slice(2));
      else if (l.startsWith("> ")) cur.gained.push(l.slice(2));
      else if (l === "<") cur.lost.push("");
      else if (l === ">") cur.gained.push("");
    }
    return cmds;
  } finally { rmSync(dir, { recursive: true, force: true }); }
}
const flatten = (cmds) => ({ gained: cmds.flatMap((c) => c.gained), lost: cmds.flatMap((c) => c.lost) });

function main() {
  const argv = process.argv.slice(2);
  const bi = argv.indexOf("--base");
  const base = bi >= 0 ? argv[bi + 1] : "origin/main";
  const dev = argv.includes("--release") ? "0" : "1";
  const paths = argv.filter((a, i) => !a.startsWith("--") && argv[i - 1] !== "--base");
  if (!paths.length)
    die(2, "usage: node engine/prosediff.mjs <path.c>… [--base <rev>] [--release]",
           "  Answers whether a diff is PROSE-ONLY by preprocessing both sides and comparing.",
           "  A `.h` has no translation unit of its own — pass a `.c` that includes it (see the residual).");
  const roots = includeRoots();
  console.log("# include roots DERIVED from engine/build.mjs's own `-I` line:");
  for (const r of roots) console.log("#   " + r);
  console.log("# base " + base + " (" + (run("git", ["rev-parse", "--short", base]).stdout || "?").trim() +
              ")  -DAPICLIENT_DEV=" + dev);

  let worst = 0;
  for (const p of paths) {
    /* A PATH THIS CANNOT ANSWER ABOUT IS DECLINED BY NAME AND NEVER ATTEMPTED, in two kinds, because an
       attempt would produce an answer. A header has no translation unit; anything that is not C has no
       preprocessor at all, and `clang -E` on a `.mjs` or a `.js` does not refuse — it returns the text with its
       `//` comments intact and its `#`-less lines untouched, so the comparison would run, the control would
       speak, and a VERDICT would be printed over a pipeline that models nothing about how that file executes.
       That is the §A-PROBE-FOR-LIVENESS shape: a check answering about the wrong artifact is worse than one
       refusing, because its output looks like a result. Both arms exit nonzero, so a `--prose` claim naming one
       REFUSES rather than passing vacuously. */
    if (!p.endsWith(".c")) {
      const why = p.endsWith(".h")
        ? "NO TRANSLATION UNIT — a header emits nothing on its own, so this cannot answer about it. Pass a `.c`"
          + "\n  that includes it; a silence here is the instrument declining, never a prose-only verdict. See"
          + "\n  this file's named residual."
        : "NOT A C TRANSLATION UNIT — this measures what a C PREPROCESSOR emits, and `clang -E` over a non-C"
          + "\n  file does not refuse: it would return text and this would print a verdict over a pipeline that"
          + "\n  models nothing about how that file runs. Declined rather than attempted.";
      console.log("\n" + p + ": " + why);
      worst = Math.max(worst, 1);
      continue;
    }
    const dir = mkdtempSync(join(tmpdir(), "prosediff-src-"));
    try {
      const b = basename(p);
      const newF = join(dir, "new_" + b), oldF = join(dir, "old_" + b), ctlF = join(dir, "ctl_" + b);
      const cur = readFileSync(join(REPO, p), "utf8");
      const sh = run("git", ["show", base + ":" + p]);
      const old = sh.stdout;
      if (sh.status !== 0 || !old)
        die(4, "`git show " + base + ":" + p + "` returned nothing — the path does not exist at that revision");
      writeFileSync(newF, cur); writeFileSync(oldF, old);
      /* THE CONTROL IS A REAL STATEMENT AT THE REAL FILE'S END, so it shares the whole pipeline with the
         subject and differs only in carrying something the subject claims not to. A control that shares less
         than that is a control for a different proposition. */
      writeFileSync(ctlF, cur + "\nstatic int apiclient_prosediff_control_" + "sentinel = 1;\n");
      const nm = [newF, oldF, ctlF, join(REPO, p), b];
      const N = preprocess(newF, roots, dev, nm);
      const O = preprocess(oldF, roots, dev, nm);
      const C = preprocess(ctlF, roots, dev, nm);

      const ctl = flatten(diffCommands(maskInts(N), maskInts(C)));
      if (!ctl.gained.length) {
        console.log("\n" + p + ": CONTROL DID NOT SPEAK — an injected statement produced no masked difference,");
        console.log("  so a zero below would mean `my input never reached this check` and is not published.");
        worst = Math.max(worst, 5);
        continue;
      }

      const masked = flatten(diffCommands(maskInts(O), maskInts(N)));
      const rawCmds = diffCommands(O, N);
      const sums = hunkPartialSums(p, base);
      const net = sums.length ? sums[sums.length - 1] : 0;

      /* THE NUMERIC RESIDUE AS A SET OF DELTAS, which is what a line-stamp shift looks like: each differing
         pair is one stamp and the delta is how far it moved. Composed only over pairs that differ ONLY in their
         numbers, since a pair whose text also differs is question (1)'s business and not this one's. */
      const deltas = new Set();
      for (const c of rawCmds) {
        if (c.op !== "c") continue;                 /* an append or a delete is not a stamp that moved */
        const n = Math.min(c.gained.length, c.lost.length);
        for (let i = 0; i < n; i++) {
          const g = c.gained[i], l = c.lost[i];
          if (maskInts(g) !== maskInts(l)) continue;
          const gn = (g.match(/[0-9]+/g) || []).map(Number), ln = (l.match(/[0-9]+/g) || []).map(Number);
          for (let k = 0; k < Math.min(gn.length, ln.length); k++)
            if (gn[k] !== ln[k]) deltas.add(gn[k] - ln[k]);
        }
      }
      const unexplained = Array.from(deltas).filter((d) => d !== 0 && !sums.includes(d));

      console.log("\n" + p);
      console.log("  PREPROCESSED LINES old=" + O.split("\n").length + " new=" + N.split("\n").length +
                  " ctl=" + C.split("\n").length + " — printed because a residue is a DIFFERENCE and a " +
                  "difference cannot say which operand is wrong; a truncated side was found this way");
      console.log("  CONTROL ARMED: an injected statement produced " + ctl.gained.length + " masked line(s)");
      console.log("  (1) NON-NUMERIC: gained " + masked.gained.length + ", lost " + masked.lost.length +
                  (masked.gained.length + masked.lost.length === 0
                    ? "  — nothing but numbers differs" : "  — A STATEMENT, EXPRESSION OR DATUM"));
      for (const l of masked.gained.slice(0, 6)) console.log("      + " + l.slice(0, 160));
      for (const l of masked.lost.slice(0, 6)) console.log("      - " + l.slice(0, 160));
      console.log("  (2) NUMERIC: delta set {" + Array.from(deltas).sort((x, y) => x - y).join(", ") +
                  "} against hunk partial sums {" + sums.join(", ") + "}, net " + net);
      if (unexplained.length)
        console.log("      UNEXPLAINED: {" + unexplained.join(", ") + "} — no hunk boundary accounts for these,");
      if (masked.gained.length + masked.lost.length === 0 && !unexplained.length)
        console.log("  VERDICT: PROSE-ONLY — every difference is a line stamp a hunk above it explains.");
      else {
        console.log("  VERDICT: NOT PROSE-ONLY.");
        worst = Math.max(worst, 1);
      }
    } finally { rmSync(dir, { recursive: true, force: true }); }
  }
  process.exit(worst);
}
main();
