/* WHO COLLECTS THIS FILE — the accounting that makes "the gate is green" a statement about the whole
 * directory rather than about whatever the runner happened to enumerate.
 *
 * CLAUDE.md §Testing: "A TEST FILE THE GATE DOES NOT COLLECT IS AN EXCLUDED TEST, AND AN EXCLUDED TEST IS A
 * FAILURE … worse, because the total LOOKS complete." A driver that hands a tree to a runner and prints the
 * runner's own count is asserting nothing about the tree: the count is the runner's opinion of its own input,
 * and every file the runner passed over is invisible in exactly the same way a file that passed is.
 *
 * SO THE EXPECTED SET IS DERIVED FROM DISK AND THE RUNNER'S NUMBER IS COMPARED AGAINST IT. That comparison is
 * the caller's; what lives here is the half that decides WHAT THE DIRECTORY CONTAINS AND WHO ANSWERS FOR IT,
 * because that half is a pure function of a path and can therefore be exercised with one fixture tree and no
 * compiler at all. It was not, while it sat inline in a driver whose first act is to invoke clang — a
 * mechanism reachable only behind a build is a mechanism the lanes that cannot build cannot check, and this
 * project splits exactly along that line.
 *
 * THE STRUCTURAL PROPERTY: a new file dropped into the tree either RAISES the expected count (so a runner that
 * did not run it cannot report green) or FAILS NAMING ITSELF (so content this driver cannot run cannot sit
 * there unclaimed). Nobody maintains a list — the list IS the directory. An exemption is a POSITIVE STATEMENT
 * and never silence: `_FIXTURE.js` says so in the filename (test262's own convention, enforced in
 * run-test262's add_test_file), and anything else declares its owner in a `GATE` file in its own directory.
 *
 * Usage:  import { collectFixtures } from "./gate_collect.mjs";
 *         const { files, failures } = collectFixtures(TESTS, ROOT);
 */
import { readdirSync, readFileSync, existsSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

/* run-test262's add_test_file, restated. A MIRROR, and stated as one: if the two rules drift the gate reddens
   on a difference that is about this file rather than about the corpus, which is the honest failure of the
   pair and the one that gets fixed. */
export const isTest = (name) => name.endsWith(".js") && !name.endsWith("_FIXTURE.js");

/* ONE MATCHER FOR THE CLAIM, READ FROM BOTH ENDS. `collectFixtures` asks of a FILE "is anyone claiming this"
   and `claimedCorpus` asks of a RUNNER "which files is my claim selecting"; that is one predicate, and
   written twice it is two facts that can disagree with nothing comparing them. */
const claimMatches = (name, claim) => name === claim || name.endsWith(claim);

/* A `GATE` file is one claim per line: `<runner path from the repo root>  <suffix-or-exact-filename>`, with
   `#` starting a comment. It is read as a POSITIVE statement about content the calling gate does not run, so
   the runner it names is required to EXIST — a claim pointing at a deleted gate is the stale-`DFAIL` shape, a
   sentence that reads as authoritative while the thing it names is gone, and it is one `existsSync` to catch. */
function readGate(dir, root, fail) {
  const path = join(dir, "GATE");
  if (!existsSync(path)) return [];
  const claims = [];
  for (const raw of readFileSync(path, "utf8").split("\n")) {
    const line = raw.replace(/#.*/, "").trim();
    if (!line) continue;
    const [runner, claim] = line.split(/\s+/);
    if (!runner || !claim) {
      fail(`${relative(root, path)}: "${raw.trim()}" is not "<runner> <suffix-or-filename>". A GATE line that ` +
           "cannot be parsed claims nothing, which is the silence it exists to replace.");
      continue;
    }
    if (!existsSync(join(root, runner)))
      fail(`${relative(root, path)} names ${runner} as the gate that collects "${claim}", and ${runner} does ` +
           "not exist. Either the runner moved and this claim must follow it, or the content it claims is now " +
           "collected by nobody.");
    claims.push({ runner, claim });
  }
  return claims;
}

/* Recursive walk. `files` is what the caller's runner is expected to have RUN; everything else must be
   claimed, by the filename convention or by a GATE in ITS OWN directory.
   A CLAIM DOES NOT REACH INTO A SUBDIRECTORY, and that is the whole difference between a positive statement
   and a silent widening. The walk here is RECURSIVE because run-test262's is; the runners a GATE names need
   not be, and the one standing in this tree is not: it reads ONE directory, through `claimedCorpus` below.
   Let a parent's claim inherit and a `solver/sub/x.html` reads as COLLECTED while that runner never opens the
   directory: an excluded test whose exclusion is stated nowhere, arrived at THROUGH the accounting that
   exists to end exactly that. So each directory answers for itself, and a new one fails NAMING ITS OWN PATH —
   the GATE to write is the one the message names.
   THIS PARAGRAPH USED TO QUOTE THAT RUNNER'S ENUMERATION EXPRESSION HERE, and it is rewritten rather than
   deleted because a reader who re-derives "say which files it takes" will quote it again. It was a THIRD
   statement of one fact — the `GATE` line said it, the runner spelled it, this said it about them — and
   nothing compared any two. There is one copy now and the runner reads it; this paragraph's job is to say
   so, never to restate it. */
export function collectFixtures(dir, root) {
  const failures = [];
  const seen = [];                 /* every claim read, so a caller can state what its verdict is a fraction OF */
  const fail = (m) => failures.push(m);
  const walk = (d) => {
    const claims = readGate(d, root, fail);
    for (const c of claims) seen.push({ dir: d, ...c });
    const found = [];
    for (const e of readdirSync(d, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
      const path = join(d, e.name);
      if (e.isDirectory()) { found.push(...walk(path)); continue; }
      if (isTest(e.name)) { found.push(path); continue; }
      if (e.name === "GATE") continue;                                 /* the statement itself */
      if (e.name.endsWith("_FIXTURE.js")) continue;                    /* test262's own convention, in the name */
      if (!claims.find((c) => claimMatches(e.name, c.claim)))
        fail(`${relative(root, path)} is under ${relative(root, dir)} and NO GATE COLLECTS IT — the driver ` +
             "here runs .js only and nothing says who runs the rest. Either it is a fixture (say so in the " +
             `filename), or the gate that owns it declares itself in ${relative(root, join(d, "GATE"))} — ` +
             "that file and not an ancestor's, because the runner it names may not descend.");
    }
    return found;
  };
  const files = walk(dir);
  /* AN EMPTY CORPUS REPORTS THE SAME GREEN AS A PASSING ONE, which is the exact thing this accounting exists
     to make impossible, so it is a failure here rather than a zero the caller has to notice. */
  if (files.length === 0)
    fail(`${relative(root, dir)} holds no runnable fixture at all — a gate with an empty corpus cannot fail, ` +
         "so its green says nothing about anything.");
  return { files, claims: seen, failures };
}

/* ─── the other end of the claim ────────────────────────────────────────────────────────────────────────── */

/* THE RUNNER READS ITS OWN CLAIM, so the suffix in a `GATE` line and the suffix that runner enumerates are ONE
   fact in ONE place. They were two. `readGate` above requires the named runner to EXIST and asks nothing
   whatever about what it collects, so a runner narrowed to another suffix, to a hand-listed set, or to no
   enumeration at all would leave every file in that directory CLAIMED BY A LINE AND RUN BY NOBODY, while the
   calling gate reported an honest green over a corpus it had never asked about — §Testing's excluded test,
   reached THROUGH the accounting written to end it. §AN-AUDITOR-DERIVES-THE-RULE is why the repair is a
   DELETED COPY and not a comparator: two hand-kept spellings need a check, one spelling needs nothing.
   MEASURED at the revision this landed, both populations derived from constructs: 19 against 19, symmetric
   difference EMPTY. Nothing was going unrun. What was missing was anything that could have said so, which is
   why the finding is the second copy and never a count of excluded documents.
   NAMED RESIDUAL. NOT COVERED: that a runner a `GATE` names is ever INVOKED at all, and that a runner which
   does not call this function consumes its own claim — what is established here is only that a CALLER's
   corpus IS its claim. NEXT DIFF: that runner calling `claimedCorpus(dir, <its own path>, root)` in place of
   a spelled suffix. ABSENCE SHOWS AS: a `GATE` claim whose suffix can be edited without changing which files
   the runner it names enumerates.
   IT FAILS AND NEVER FALLS BACK, in all three directions, because each is a claim gone silent: no `GATE`, no
   line naming this runner, or a line selecting nothing. An empty corpus reports the same green as a passing
   one — `collectFixtures` refuses that above and so does this. */
export function claimedCorpus(dir, runner, root) {
  const failures = [];
  const fail = (m) => failures.push(m);
  const gate = join(dir, "GATE");
  if (!existsSync(gate))
    fail(`${relative(root, dir)} has no GATE file, so nothing in it is claimed by anybody while ${runner} ` +
         "takes its corpus from it. Write the GATE: one line, " + `"${runner}  <suffix-or-filename>".`);
  const claims = readGate(dir, root, fail).filter((c) => c.runner === runner);
  if (existsSync(gate) && !claims.length)
    fail(`${relative(root, gate)} holds no claim naming ${runner}, so this directory is claimed by some other ` +
         "runner or by nobody. A runner taking its corpus from a claim it does not hold is taking it from " +
         "nowhere, and the accounting that reads this file would report the content as collected regardless.");
  const files = readdirSync(dir, { withFileTypes: true })
    .filter((e) => !e.isDirectory() && e.name !== "GATE" && claims.some((c) => claimMatches(e.name, c.claim)))
    .map((e) => e.name)
    .sort();
  if (claims.length && !files.length)
    fail(`${relative(root, gate)} claims ${claims.map((c) => `"${c.claim}"`).join(", ")} for ${runner} and no ` +
         `file in ${relative(root, dir)} matches it — a gate with an empty corpus cannot fail, so its green ` +
         "says nothing about anything.");
  return { files, claims, failures };
}

/* ─── the program ───────────────────────────────────────────────────────────────────────────────────────── */

/* RUN ONLY WHEN THIS FILE IS THE PROGRAM (engine/placeaudit.mjs's guard, for its reason: a module that works
   at import is one its importers cannot import).
   IT IS A PROGRAM AT ALL SO THAT THE ACCOUNTING RUNS. Its only caller was engine/features.mjs, which invokes
   clang and is therefore a BULK GATE the lanes that cannot build must not run — so a walk costing
   milliseconds, compiling nothing and reading no artifact was reachable only behind a whole-corpus fixture
   run, and an unclaimed file dropped into engine/tests raised nothing until somebody happened to make one.
   That is §Testing's excluded test with a delay on it, and the delay is the whole defect: the total looks
   complete in the meantime. On engine/build.mjs's stage list it is STAGE_HOST.SOURCE on the same argument
   the three audits beside it state — it compiles no C and reads no artifact, so it asks its question of the
   SOURCES whatever the programs did, and a link failure can never take it out of the run.
   WHAT IS NOT ON THAT LIST, DELIBERATELY, IS THE RUNNER A CLAIM NAMES. engine/solvergate.mjs is 19 documents
   against seven schedules at up to 120s of CPU each; CLAUDE.md §A-BULK-GATE-IS-RUN-ONCE names it with test262
   and WPT as the whole-corpus runs the main agent serialises, and a build that ran one would be the
   loaded-machine defect on every build. The accounting belongs here; its subject's runner does not.
   THE TWO VERDICTS ARE SEPARATE AND ONLY ONE CARRIES THE EXIT CODE. An instrument that cannot see something
   has not found anything, so the blind spots print on the CLEAN DAY as well — a list that appears only when
   something is wrong is a list nobody learns to look for. */
if (process.argv[1] && resolve(process.argv[1]) !== fileURLToPath(import.meta.url)) {
  /* imported, not run */
} else {
  const ENGINE = dirname(fileURLToPath(import.meta.url));
  const ROOT = join(ENGINE, "..");
  const TESTS = join(ENGINE, "tests");
  const { files, claims, failures } = collectFixtures(TESTS, ROOT);

  /* THE DENOMINATOR, BECAUSE A VERDICT WITHOUT ONE IS A FRACTION WITH ITS BOTTOM HALF UNSTATED. */
  console.log(`[gate_collect] ${relative(ROOT, TESTS)}: ${files.length} collectable fixture(s), ` +
              `${claims.length} GATE claim(s) — ` +
              (claims.length
                 ? claims.map((c) => `${relative(ROOT, c.dir)} -> ${c.runner} "${c.claim}"`).join("; ")
                 : "none"));

  console.log("[gate_collect] BLIND SPOTS — this run found NOTHING about any of these, on the clean day too:");
  console.log("  * WHETHER A RUNNER A CLAIM NAMES IS EVER INVOKED. `readGate` requires it to EXIST and asks " +
              "nothing else; no stage of engine/build.mjs runs one, and this walk cannot see a stage list. A " +
              "claim honoured by a runner nobody runs is an excluded test with a certificate on it.");
  console.log("  * WHETHER A RUNNER HONOURS ITS OWN CLAIM. A runner that takes its corpus from " +
              "`claimedCorpus` cannot drift from the line that claims it, by construction. One that spells a " +
              "suffix of its own can, and nothing here compares the two — which is exactly the state " +
              "engine/solvergate.mjs and engine/tests/solver/GATE were in.");
  console.log("  * WHETHER `isTest` STILL MIRRORS run-test262's add_test_file. It is a restatement and says " +
              "so; if the two drift, this gate reddens on a difference about THIS FILE rather than about the " +
              "corpus, which is the honest failure of the pair and not a finding about the tree.");

  if (!failures.length) {
    console.log(`[gate_collect] PASS (findings) — every file under ${relative(ROOT, TESTS)} is either a ` +
                "fixture run-test262 collects or is claimed by a GATE in its own directory naming a runner " +
                "that exists. That verdict is over the population on the line above and over nothing else.");
    process.exit(0);
  }
  console.error("[gate_collect] FINDINGS — the corpus a gate would report on is not the corpus on disk:");
  for (const f of failures) console.error("  " + f);
  console.error(`[gate_collect] FAILED — ${failures.length} finding(s). There is no baseline to update and no ` +
                "allowlist: an exemption is a POSITIVE STATEMENT (a `_FIXTURE.js` name, or a GATE line in the " +
                "file's OWN directory) and never silence.");
  process.exit(1);
}
