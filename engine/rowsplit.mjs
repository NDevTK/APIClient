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

const KEYRE = new RegExp(`@HWORK \\{[^}]*"${key.replace(/[^\w]/g, "\\$&")}":(-?\\d+)`, "g");
const buckets = { with: { n: 0, ans: {} }, without: { n: 0, ans: {} } };
for (const b of Object.values(buckets)) for (const r of [...rows, ...control]) b.ans[r] = 0;
let noKey = 0, noTable = 0;

for (const p of paths) {
  const text = fs.readFileSync(p, "latin1");
  const seen = [...text.matchAll(KEYRE)].map((m) => Number(m[1]));
  if (!seen.length) { noKey++; continue; }
  const tables = text.split("\n").filter((l) => l.startsWith("@H "));
  if (!tables.length) { noTable++; continue; }
  /* BEST-OVER-THE-RUN, because an `@H` row is a statement answered or not and a later table cannot un-answer
     one. Anything other than {0,1} means a counter now shares the table and this tool would read any non-zero
     as answered — that is a defect at the emitter, not something to normalise here. */
  const best = {};
  for (const line of tables)
    for (const m of line.matchAll(/([a-z][a-z0-9-]*)=(\d+)/g)) {
      const v = Number(m[2]);
      if (v !== 0 && v !== 1)
        throw new Error(`${p}: row \`${m[1]}\` has value ${v} — @H rows are statements answered or not, so a ` +
                        `value outside {0,1} means a counter shares the table. Separate them at the emitter.`);
      best[m[1]] = Math.max(best[m[1]] ?? 0, v);
    }
  const b = Math.max(...seen) > 0 ? buckets.with : buckets.without;
  b.n++;
  for (const r of [...rows, ...control]) if (best[r] === 1) b.ans[r]++;
}

console.log(`partitioned ${paths.length} log(s) by \`${key}\` — ${buckets.with.n} with >0, ` +
            `${buckets.without.n} at 0` +
            (noKey ? `, ${noKey} carrying no \`${key}\` (a run older than the field, not a run at zero)` : "") +
            (noTable ? `, ${noTable} with no @H table` : ""));
const line = (r, tag) => `  ${tag}${r.padEnd(16)} ${String(buckets.with.ans[r]).padStart(4)}/${buckets.with.n}` +
                         `   ${String(buckets.without.ans[r]).padStart(4)}/${buckets.without.n}`;
console.log(`  ${"".padEnd(18)} with>0   at 0`);
for (const r of rows) console.log(line(r, ""));
console.log("  -- control, and the reading below depends on it --");
for (const r of control) console.log(line(r, ""));
const dead = control.filter((r) => buckets.with.ans[r] === 0);
console.log(dead.length
  ? `\nCONTROL DID NOT ANSWER (${dead.join(", ")}) in the with-condition bucket. This partition establishes ` +
    `NOTHING about the subject rows: a zero here is as consistent with a mis-addressed question as with a ` +
    `finding, which is the state a control exists to rule out.`
  : `\nControl answered in the with-condition bucket, so a subject row reading 0 there is a fact about that ` +
    `row rather than about this partition. What it is NOT is a statement about WHICH mechanism, and it does ` +
    `not distinguish the subject rows from each other.`);
