/* The row sets of the census objects the result document carries, read from the engine's own declarations: the
 * METRIC lines of solver/metrics/*.def for a census composed through the registry, and the composer's format
 * string for one composed by hand. engine/build.mjs's census readers and the trusted zone's acceptance of the
 * result document (extension/bridge.js, through the generated extension/lib/census-rows.js) take both kinds of
 * row from here, so a row the engine writes is never named in a list beside it.
 *
 *   node engine/census_rows.mjs --write   regenerates extension/lib/census-rows.js
 *   node engine/census_rows.mjs --check   exits 1 naming each row the file and the declarations disagree on */

import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
export const HOST_DIR = join(ROOT, "engine", "host");
export const CENSUS_ROWS_JS = join(ROOT, "extension", "lib", "census-rows.js");

/* A hand-composed census's rows: every `\"name\":%…` in the composer's format string between `from` and the first
   `to` after it, split by conversion (`%s` splices a nested composer's object, every other conversion is a number).
   Names are matched as raw bytes, never decoded, so a key that needed an escape is not found and the composer then
   names nothing, which throws. `label` names the composer in a refusal and `why` is the caller's reason. */
export function composerFields(src, from, to, label, why) {
  const open = src.indexOf(from);
  if (open < 0)
    throw new Error(`[census-rows] cannot find ${JSON.stringify(from)} in ${label} — ${why}, and the row set is ` +
                    `taken from that composer rather than from a list beside it; a renamed or moved composer ` +
                    `is a change to state, not one to guess a remembered list past.`);
  const close = src.indexOf(to, open);
  if (close < 0)
    throw new Error(`[census-rows] the composer at ${JSON.stringify(from)} in ${label} does not reach ` +
                    `${JSON.stringify(to)} — ${why}. An unterminated region yields a short row set.`);
  const body = src.slice(open, close).replace(/\/\*[\s\S]*?\*\//g, " ").replace(/^[ \t]*\/\/.*$/gm, " ");
  const numeric = [], object = [];
  for (const m of body.matchAll(/\\"([A-Za-z_][A-Za-z0-9_]*)\\":%([-0-9.]*)([a-zA-Z]+)/g)) {
    const into = m[3] === "s" ? object : numeric;
    if (!numeric.includes(m[1]) && !object.includes(m[1])) into.push(m[1]);
  }
  if (!numeric.length && !object.length)
    throw new Error(`[census-rows] the composer at ${JSON.stringify(from)} in ${label} names no field at all — ` +
                    `${why}. A composer with none is one whose shape this reader no longer recognises.`);
  return { numeric, object };
}

/* A registry census's rows: the METRIC lines of each family's declaration file, in emission order, split by
   representation (OBJECT is a nested composer's object, every other one a number). Every `METRIC(` is one row of
   the full grammar or a throw, so a row this reader cannot parse is never dropped. `defs` is [{label, src}]. */
export function defFields(defs, why) {
  const numeric = [], object = [];
  for (const { label, src } of defs) {
    const code = src.replace(/\/\*[\s\S]*?\*\//g, " ");
    const opens = (code.match(/\bMETRIC\s*\(/g) || []).length;
    let rows = 0;
    for (const m of code.matchAll(/\bMETRIC\(\s*\w+\s*,\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*[A-Z]+\s*,\s*"[^"]*"\s*,\s*[A-Z]+\s*,\s*"[^"]*"\s*,\s*([A-Z0-9]+)\s*\)/g)) {
      rows++;
      if (numeric.includes(m[1]) || object.includes(m[1]))
        throw new Error(`[census-rows] ${label} declares \`${m[1]}\` a second time in one census — ${why}.`);
      (m[2] === "OBJECT" ? object : numeric).push(m[1]);
    }
    if (!rows || rows !== opens)
      throw new Error(`[census-rows] ${label} holds ${opens} METRIC( row(s) and ${rows} parse — ${why}, and a ` +
                      `row this reader cannot read would leave the row set short.`);
  }
  return { numeric, object };
}

/* The result document's census objects whose row sets a declaration fixes. A registry census is composed from
   the families solver/metrics.c's FAMILIES table assigns it (its third column); the two hand-composed ones are
   read from their composers. `_forkAt` and `_absent` are keyed by what the run met (predicates, global names),
   so no declaration fixes them. */
export const COMPOSED_CENSUSES = Object.freeze([
  { key: "_heap", composer: ["solver/result.c", "char *result_heap_json(JSContext *ctx)", "\n}\n"] },
  { key: "_swap", composer: ["solver/result.c", "char *result_swap_json(void)", "\n}\n"] },
]);

/* Each registry family's census key from solver/metrics.c's FAMILIES table, in table order: {family, census}, with
   census null for a family composed at the document's top level. */
export function familyCensuses(metricsSrc) {
  const out = [];
  const opens = (metricsSrc.match(/^\s*\[METRIC_FAMILY_\w+\]\s*=/gm) || []).length;
  for (const m of metricsSrc.matchAll(/^\s*\[METRIC_FAMILY_\w+\]\s*=\s*\{\s*"(\w+)",\s*\w+,\s*(?:NULL|"(_\w+)")\s*\}/gm))
    out.push({ family: m[1], census: m[2] === undefined ? null : m[2] });
  if (!out.length || out.length !== opens)
    throw new Error(`[census-rows] solver/metrics.c's FAMILIES table holds ${opens} row(s) and ${out.length} ` +
                    `parse, so a family's census could not be read.`);
  return out;
}

/* Each census as {numeric, object}, keyed by its result-document key. `read(file)` returns the text of
   engine/host/<file>, so build.mjs passes its frozen sources and the CLI the working tree. */
export function resultCensusRows(read) {
  const why = "the trusted zone accepts a result document's census rows by kind, and takes both kinds from here";
  const byCensus = new Map();
  for (const { family, census } of familyCensuses(read("solver/metrics.c")))
    if (census !== null) byCensus.set(census, [...(byCensus.get(census) || []), family]);
  const out = {};
  for (const [key, families] of byCensus)
    out[key] = defFields(families.map((f) => ({ label: `engine/host/solver/metrics/${f}.def`,
                                                src: read(`solver/metrics/${f}.def`) })), why);
  for (const c of COMPOSED_CENSUSES)
    out[c.key] = composerFields(read(c.composer[0]), c.composer[1], c.composer[2], `engine/host/${c.composer[0]}`, why);
  return out;
}

/* The generated script's text: one frozen global the zone's classic scripts read. */
export function censusRowsScript(rows) {
  const body = Object.entries(rows).map(([k, v]) =>
    `  ${k}: Object.freeze({\n` +
    `    numeric: Object.freeze(${JSON.stringify(v.numeric)}),\n` +
    `    object: Object.freeze(${JSON.stringify(v.object)}),\n  }),`).join("\n");
  return "/* Generated by engine/census_rows.mjs from engine/host/solver/metrics/*.def and solver/result.c's census\n" +
         " * composers; regenerate with `node engine/census_rows.mjs --write`. Each census object of the result document\n" +
         " * that a declaration fixes, with its number rows and its object rows (bridge.js's resultDocumentRefusal). */\n" +
         "const CENSUS_ROWS = Object.freeze({\n" + body + "\n});\n";
}

/* Each row the file states and the declarations do not, or the reverse, as one line per disagreement; empty when
   they agree. */
export function censusRowsDrift(rows, fileText) {
  const want = censusRowsScript(rows);
  if (want === fileText) return [];
  let had;
  try { had = new Function(fileText + "\nreturn CENSUS_ROWS;")(); } catch (e) { return [`the file does not evaluate: ${e.message}`]; }
  const lines = [];
  for (const k of new Set([...Object.keys(rows), ...Object.keys(had)])) {
    for (const kind of ["numeric", "object"]) {
      const a = (rows[k] && rows[k][kind]) || [], b = (had[k] && had[k][kind]) || [];
      for (const n of a) if (!b.includes(n)) lines.push(`${k}.${n} is a ${kind} row the engine declares and the file omits`);
      for (const n of b) if (!a.includes(n)) lines.push(`${k}.${n} is a ${kind} row the file states and the engine does not declare`);
    }
  }
  return lines.length ? lines : ["the file's text differs from the generator's output in layout only"];
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const mode = process.argv[2];
  if (mode !== "--write" && mode !== "--check") throw new Error("usage: node engine/census_rows.mjs --write | --check");
  const rows = resultCensusRows((f) => readFileSync(join(HOST_DIR, f), "utf8"));
  if (mode === "--write") {
    writeFileSync(CENSUS_ROWS_JS, censusRowsScript(rows));
  } else {
    const drift = censusRowsDrift(rows, readFileSync(CENSUS_ROWS_JS, "utf8"));
    for (const l of drift) console.error(`[census-rows] ${l}`);
    process.exit(drift.length ? 1 : 0);
  }
}
