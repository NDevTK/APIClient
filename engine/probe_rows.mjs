/* THE `@H` PROBE TABLE, READ ONCE — because `@H ` is a PREFIX TWO SHAPES SHARE and every reader that
 * selected on the prefix alone was parsing prose as a table.
 *
 * test_forced.c's `probes_report` writes TWO things under this marker: a NARRATION line per folded 0
 * (`@H   <row> @<work>: <why>`) and then the TABLE itself (`@H ` + every `name=<0|1>` pair + the verdict it
 * ends in). A `why` is a SENTENCE, so it carries whatever `k=0` pairs its author needed to make the sentence
 * say something, and a reader that takes every `name=<digits>` off any line beginning `@H ` reads those as
 * rows of the table.
 *
 * THE DISCRIMINATOR IS THE PRODUCER'S OWN TERMINATOR: the table is the line that ENDS in the verdict
 * `probes_report` writes after it, and a narration never carries one. That is a fact about the emitter rather
 * than a guess about how sentences are worded, which is why it is the whole of the test here and why a longer
 * list of things prose "usually" does is not wanted.
 *
 * IT LIVES IN ITS OWN MODULE BECAUSE THE PREDICATE IS NOT THE PROPERTY OF ANY ONE READER. It was landed, with
 * its measurement, inside a function of engine/build.mjs — which has no exports and does the build at import,
 * so no other reader could route to it, and the two that wanted it wrote their own. A second right answer to
 * one question is the shape that drifts, and here both copies were WRONG rather than merely second. Anything
 * that reads an `@H` table imports this; nothing re-derives the selection.
 *
 * WHAT THE PROSE COST, MEASURED. On one smoke log, a bare `(\S+)=([01])` over every `@H ` line read NINE rows
 * where the run printed ONE, eight of them prose. Over a corpus of twelve frozen-snapshot smoke logs, the
 * wider spelling the two readers used reads 269 distinct row names against the 267 the tables declare: the
 * two extra are `asked` and `driven`, out of an `(engine_orphan_census: asked=0, driven=0)` narration, and a
 * reader that folds by maximum reports them as rows NEVER ANSWERED IN ANY RUN — which is the strongest band
 * such a tool has and the one it exists to produce. So the defect is not a rounding error on a total; it
 * fabricates entries in the band a reader acts on. RETIREMENT: this record goes when the producer writes the
 * table under a marker no narration shares, because the selection is then not a judgement at all.
 *
 * A VALUE OUTSIDE {0,1} IS LOUD AND IS NOT A DROPPED LINE. Every row is a statement answered or not, so a
 * magnitude means a counter now shares the table and any reader folding it would turn that magnitude into a
 * boolean. The strict pattern below cannot match such a line, so the honest thing is not to let it fall
 * through to "prose": a line that is pairs-shaped all the way to the verdict and still does not parse is the
 * emitter having changed, and it THROWS. That is a real repair to every caller and not only to the two that
 * were selecting wrongly — the reader that already used the strict pattern dropped such a line SILENTLY, and
 * measuring a subset without saying so is the failure these tools exist to avoid. RETIREMENT: this record
 * goes when the value domain is asserted at the emitter in a form a release build keeps, so a reader cannot
 * be the first to notice.
 *
 * PAIRS-SHAPED IS PART OF THE THROW'S CONDITION AND NOT DECORATION. The throw must never fire on a narration
 * whose sentence happens to end in the verdict token, because a false accusation against a healthy log sends
 * a reader to fix a fixture that is fine. So the line must ALSO be nothing but `token=value` pairs from the
 * marker to the verdict, which no English sentence is. */

/* THE TABLE: pairs whose values are the only two a statement can take, then the producer's verdict. */
const TABLE = /^@H ((?:\S+=[01] )+)=> (?:OK|FAIL|INCOMPLETE)$/;
/* THE SAME SHAPE WITH ANY VALUE — what a table looks like once something that is not a statement is on it. */
const SHAPED = /^@H (?:\S+=\S* )+=> (?:OK|FAIL|INCOMPLETE)$/;

/* EVERY TABLE IN A STREAM, IN ORDER, AS THE ROWS IT DECLARES. `where` names the artifact the text came from
   and is REQUIRED: the one thing this function can throw about is a stream whose shape it no longer knows,
   and a reader meeting that message needs to know WHICH log said it. */
export function probeTables(text, where) {
  if (typeof where !== "string" || !where)
    throw new Error("probeTables() needs the name of the artifact its text came from — its one throw is " +
                    "about a stream this reader can no longer parse, and a reader meeting that message with " +
                    "no artifact on it cannot act on it.");
  const tables = [];
  for (const line of String(text).split("\n")) {
    if (!line.startsWith("@H ")) continue;
    const m = TABLE.exec(line);
    if (m) {
      const row = {};
      for (const [, k, v] of m[1].matchAll(/(\S+)=([01])\b/g)) row[k] = v === "1";
      if (Object.keys(row).length) tables.push(row);
      continue;
    }
    if (SHAPED.test(line))
      throw new Error(`${where}: an @H line is a table all the way to its verdict and carries a value that ` +
                      `is not 0 or 1 — @H rows are statements answered or not, so this is a counter sharing ` +
                      `the table, and every reader of this stream would fold its magnitude into a boolean ` +
                      `and report the row as answered. Separate them at the emitter.\n  ${line}`);
  }
  return tables;
}

/* AND THE FOLD BOTH LOG READERS WANT: a row is a statement, and a later table cannot un-answer one, so a
   run's reading of a row is the best it ever reached. The table COUNT is returned beside it because "no
   table in this log" and "tables carrying no rows" are different facts about a run and a caller that had
   only the map would have to infer one from an empty other. */
export function probeBest(text, where) {
  const tables = probeTables(text, where);
  const best = new Map();
  for (const row of tables)
    for (const k of Object.keys(row)) if (row[k] || !best.has(k)) best.set(k, best.get(k) || row[k]);
  return { tables: tables.length, best };
}
