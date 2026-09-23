/* WHY A ROW IS STILL 0 — PARTITIONED BY A CONDITION OF THE RUN, NOT SUMMED OVER RUNS.
 *
 * `engine/smokerows.mjs` answers WHICH rows are still 0 across a set of logs. It cannot answer the next
 * question, which is the one that decides whose defect a zero is: does the row answer in runs where some
 * PRECONDITION held, and not otherwise? A row that answers whenever a condition holds is that condition's;
 * a row that answers in NEITHER bucket is a mechanism, and no amount of scheduling explains it.
 *
 *   node engine/rowsplit.mjs --key _jobsRun --rows fetch,clone-body --control pending,throw <log>...
 *
 * The key is read off the `@HWORK` work line, the rows off the `@H` tables, and each log is bucketed by
 * whether the key's maximum in that run is above zero.
 *
 * A CONTROL IS REQUIRED AND THE TOOL REFUSES WITHOUT ONE, because the finding this makes is almost always a
 * ZERO and a zero is what a broken parser also prints. A control row is one known to answer — if it does not
 * answer in the with-condition bucket, the partition is not measuring what it claims and the subject rows say
 * nothing. That is the positive-control discipline this project keeps paying for when it is skipped: an
 * unarmed probe reports a blind spot that does not exist.
 *
 * THE KEY IS A PROXY AND THE CALLER OWNS THAT CLAIM. Reading `_jobsRun` to stand for "anything was ever
 * rank-eligible" is licensed only by a separate measurement that the two separate totally; this tool cannot
 * check that and does not pretend to. It reports the key it read and the bucket sizes so a reader can see
 * what the partition was over.
 *
 * IT PRINTS NO EXPECTED TOTAL AND NO VERDICT. A fixture's row set is a property of the fixture at a revision
 * and the bucket sizes are a property of a corpus of runs; both move. This exits 0 always — it is a reading,
 * and a build that failed on it would be failing on how a scheduler spent a budget.
 */
import fs from "node:fs";
import { probeBest } from "./probe_rows.mjs";

const argv = process.argv.slice(2);
const opt = (name) => {
  const i = argv.indexOf(name);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : null;
};
const key = opt("--key");
const rows = (opt("--rows") || "").split(",").filter(Boolean);
const control = (opt("--control") || "").split(",").filter(Boolean);
const paths = argv.filter((a, i) => !a.startsWith("--") && !argv[i - 1]?.startsWith("--"));

if (!key || !rows.length || !control.length || !paths.length)
  throw new Error("usage: --key <workLineField> --rows a,b --control c,d <log>...\n" +
    "A CONTROL IS NOT OPTIONAL: the finding here is a zero, and a zero is also what a mis-addressed question " +
    "prints. Name at least one row known to answer, so a reader can tell the two apart.");

const KEYQ = key.replace(/[^\w]/g, "\\$&");
const KEYRE = new RegExp(`@HWORK \\{[^}]*"${KEYQ}":(-?\\d+)`, "g");
/* THE SAME KEY ON SOME OTHER MARKER — because a key name is only half of a question about this stream and
   the marker is the other half. `--key` names a field of the @HWORK line, and a caller who hands it a field
   of a DIFFERENT marker gets a run carrying the key on every line and no match here, which read as "a run
   older than the field" and is not: those two take opposite action, one being a corpus to widen and the
   other a question to re-address. MEASURED, and it is what this file was committed after: `workDone` is on
   @HWORK and on @WFQ, deliberately — one quantity, one spelling, sampled by two hooks — and an unanchored
   read of it off a log cost a wrong report. RETIREMENT: this record goes when the key and its marker arrive
   as one argument, so a marker cannot be omitted. */
const ELSEWHERE = new RegExp(`^(@[A-Z][A-Z0-9_-]*) \\{[^}]*"${KEYQ}":`, "gm");
const buckets = { with: { n: 0, ans: {} }, without: { n: 0, ans: {} } };
for (const b of Object.values(buckets)) for (const r of [...rows, ...control]) b.ans[r] = 0;
let noKey = 0, noTable = 0;
const otherMarkers = new Set();

for (const p of paths) {
  const text = fs.readFileSync(p, "latin1");
  const seen = [...text.matchAll(KEYRE)].map((m) => Number(m[1]));
  if (!seen.length) {
    noKey++;
    for (const m of text.matchAll(ELSEWHERE)) if (m[1] !== "@HWORK") otherMarkers.add(m[1]);
    continue;
  }
  /* WHICH LINES ARE TABLES IS engine/probe_rows.mjs's QUESTION AND NOT THIS FILE'S. It used to be answered
     here by the `@H ` prefix alone, and that prefix is shared by the TABLE and by one narration sentence per
     folded 0 — a `why` carries whatever `k=0` pairs its author needed to make the sentence say something, so
     the sentences were read as rows. A subject row named in a narration would have been counted out of prose
     and a control row could have been ANSWERED by one, which is the one failure a control exists to rule out.
     BEST-OVER-THE-RUN, because an `@H` row is a statement answered or not and a later table cannot un-answer
     one. The row-value invariant that used to stand here — a value outside {0,1} is a counter sharing the
     table, and folding its magnitude into a boolean would read it as answered — moved to that module with the
     selection, because it is a fact about the stream rather than about this reader. */
  const { tables, best } = probeBest(text, p);
  if (!tables) { noTable++; continue; }
  const b = Math.max(...seen) > 0 ? buckets.with : buckets.without;
  b.n++;
  for (const r of [...rows, ...control]) if (best.get(r) === true) b.ans[r]++;
}

/* NO @HWORK LINE ANYWHERE CARRIES IT AND ANOTHER MARKER DOES — so the question was addressed to the wrong
   stream and there is nothing here to read. Refused rather than reported, for the reason probe_rows.mjs's
   row-value check is a throw: this tool promises exit 0 on a READING, and a mis-addressed question is not one. It is
   narrow on purpose — a key that some logs DO carry on @HWORK is the caller's namespace working, and the
   logs without it are then genuinely older, which is the other reading and is left standing. */
if (buckets.with.n + buckets.without.n === 0 && otherMarkers.size)
  throw new Error(`no @HWORK line in any of these ${paths.length} log(s) carries \`${key}\`, and ` +
    `${[...otherMarkers].sort().join(", ")} does — \`--key\` names a field of the @HWORK work line, so this ` +
    `is a question addressed to the wrong marker rather than a corpus older than the field. Those two take ` +
    `opposite action. Re-ask it of a field @HWORK carries, or read the other marker's line directly.`);

console.log(`partitioned ${paths.length} log(s) by \`${key}\` — ${buckets.with.n} with >0, ` +
            `${buckets.without.n} at 0` +
            (noKey ? `, ${noKey} carrying no \`${key}\`` +
                     (otherMarkers.size
                        ? ` (${[...otherMarkers].sort().join(", ")} carries that name too, so some of these ` +
                          `may be a question about the wrong marker rather than a run older than the field)`
                        : ` (a run older than the field, not a run at zero)`) : "") +
            (noTable ? `, ${noTable} with no @H table` : ""));
const line = (r, tag) => `  ${tag}${r.padEnd(16)} ${String(buckets.with.ans[r]).padStart(4)}/${buckets.with.n}` +
                         `   ${String(buckets.without.ans[r]).padStart(4)}/${buckets.without.n}`;
console.log(`  ${"".padEnd(18)} with>0   at 0`);
for (const r of rows) console.log(line(r, ""));
console.log("  -- control, and the reading below depends on it --");
for (const r of control) console.log(line(r, ""));
/* AND WHETHER THERE WAS A CONTRAST AT ALL, WHICH THE CONTROL CANNOT ANSWER. The control says a bucket's zero
   is about the row; it says nothing about whether the two buckets are two populations. A key that cannot be
   0 puts every log on one side and the partition is then a tautology wearing a table — MEASURED on `workDone`,
   whose @HWORK value is at least the seed's flows at every table of every run, so `--key workDone` reads
   24 with >0 / 0 at 0 and separates nothing. Stated over the OUTPUT rather than as a list of such keys, so it
   covers the next one too. RETIREMENT: this record goes when a key states whether it can be zero. */
if (buckets.with.n === 0 || buckets.without.n === 0)
  console.log(`\nONE BUCKET IS EMPTY — every log with a \`${key}\` fell on the same side, so nothing here is a ` +
    `partition and the two columns are one population printed twice. Either this key cannot take both values ` +
    `(check what it reads when nothing has happened) or this corpus does not contain the contrast.`);
const dead = control.filter((r) => buckets.with.ans[r] === 0);
console.log(dead.length
  ? `\nCONTROL DID NOT ANSWER (${dead.join(", ")}) in the with-condition bucket. This partition establishes ` +
    `NOTHING about the subject rows: a zero here is as consistent with a mis-addressed question as with a ` +
    `finding, which is the state a control exists to rule out.`
  : `\nControl answered in the with-condition bucket, so a subject row reading 0 there is a fact about that ` +
    `row rather than about this partition. What it is NOT is a statement about WHICH mechanism, and it does ` +
    `not distinguish the subject rows from each other.`);
