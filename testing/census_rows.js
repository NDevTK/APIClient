/* What kind of number a census row is, read from the declaration its producer composes it from.
 *
 * A lifetime count may be differenced across two samples of one instance; a gauge is a level and may fall; a
 * maximum is a high-water mark, so a plateau is not a ceiling and runs of different length do not compare; a
 * constant is written once; a regime is a policy the rows beside it are read under.
 *
 * A composer is a family of solver/metrics.h's registry: its rows, their kinds and representations are the
 * METRIC lines of `engine/host/solver/metrics/<family>.def`, and solver/metrics.c composes exactly those keys,
 * asserting at composition that every declared row was set or stated absent. So the row set and the kinds
 * are one declaration, read here from source and therefore right about an artifact of any age (an artifact
 * older than a row has its kind stated and the row absent, two different facts). Which rows a driver prints
 * is the driver's curation and is not derived here; a consumer keeps its list and asks `kindsOf`,
 * `requireWhole` and `requireFrom`. The row counts per composer:
 *     node -e 'const {censusKinds}=require("./testing/census_rows.js"), a=censusKinds();
 *              for (const k of Object.keys(a)) console.log(k, Object.keys(a[k].kindOf).length)' */
"use strict";

const { readFileSync } = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..");

/* solver/metrics.h's MetricKind, spelled as metrics.c's schema spells it. A token outside it is a throw. */
const KINDS = ["lifetime", "gauge", "constant", "maximum", "regime"];
const KIND_TOKENS = { LIFETIME: "lifetime", GAUGE: "gauge", CONSTANT: "constant", MAXIMUM: "maximum",
                      REGIME: "regime" };
/* solver/metrics.h's MetricRepr; an OBJECT row is a nested composer's object, every other row a number. */
const REPRS = ["INT", "FIXED1", "FIXED3", "OBJECT"];

/* The composers, each with the declaration files of the families it composes, in emission order. */
const COMPOSERS = {
  cold:      { defs: ["engine/host/solver/metrics/cold.def"] },
  /* `members` alone on an empty frontier, the second family's rows after it while a member stands. */
  wfq:       { defs: ["engine/host/solver/metrics/wfq.def", "engine/host/solver/metrics/wfq_frontier.def"] },
  fetchEdge: { defs: ["engine/host/solver/metrics/fetch_edge.def"] },
  xhrEdge:   { defs: ["engine/host/solver/metrics/xhr_edge.def"] },
  /* Spliced into `_cold` beside `stepUnitRuns`, as the two edges are spliced beside `epAsks`. */
  rungEntry: { defs: ["engine/host/solver/metrics/rung_entry.def"] },
};

/* A declaration file's rows, in declaration order, which is the order the composer emits them in. Every
   `METRIC(` is one row of the full grammar or a throw, so a row this reader cannot parse is never skipped. */
function defRowsFromText(src, label) {
  const code = src.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/\/\/.*$/gm, " ");
  const opens = (code.match(/\bMETRIC\s*\(/g) || []).length;
  const ROW = /\bMETRIC\(\s*([A-Za-z_]\w*)\s*,\s*"([^"\\]*)"\s*,\s*([A-Z]+)\s*,\s*"([^"\\]*)"\s*,\s*([A-Z]+)\s*,\s*"([^"\\]*)"\s*,\s*([A-Z0-9]+)\s*\)/g;
  const kindOf = Object.create(null), numeric = [], object = [];
  let rows = 0;
  for (const m of code.matchAll(ROW)) {
    rows++;
    const [, , key, kindTok, , , , repr] = m;
    const kind = KIND_TOKENS[kindTok];
    if (!kind)
      throw new Error(label + ": `" + key + "` is declared with the kind " + kindTok + ", which is not one of " +
                      Object.keys(KIND_TOKENS).join(", ") + " — solver/metrics.h's MetricKind would not compile " +
                      "it either, so this file and that enum have parted.");
    if (!REPRS.includes(repr))
      throw new Error(label + ": `" + key + "` is declared with the representation " + repr + ", which is " +
                      "not one of " + REPRS.join(", ") + ".");
    if (key in kindOf)
      throw new Error(label + ": `" + key + "` is declared twice — one composition would emit the key twice, " +
                      "and a JSON reader keeps whichever came last.");
    kindOf[key] = kind;
    (repr === "OBJECT" ? object : numeric).push(key);
  }
  if (rows !== opens)
    throw new Error(label + ": " + opens + " METRIC( rows and " + rows + " of them parse — a row this reader " +
                    "cannot read would be a published row with no kind.");
  if (!rows)
    throw new Error(label + ": a declaration file with no METRIC row — its family emits nothing, and an empty " +
                    "row set would pass every census ever printed.");
  return { kindOf, numeric, object };
}

/* A composer's rows over its families, refusing a key two of them declare. */
function composerRowsFromDefs(defs, read, key) {
  const kindOf = Object.create(null), numeric = [], object = [];
  for (const d of defs) {
    const r = defRowsFromText(read(d), d);
    for (const n of Object.keys(r.kindOf)) {
      if (n in kindOf)
        throw new Error(d + ": `" + n + "` is declared by two families the composer `" + key + "` splices.");
      kindOf[n] = r.kindOf[n];
    }
    numeric.push(...r.numeric);
    object.push(...r.object);
  }
  return { key, kindOf, numeric, object };
}

let memo = null;
function censusKinds() {
  if (memo) return memo;
  const byFile = new Map();
  const read = (f) => {
    if (!byFile.has(f)) byFile.set(f, readFileSync(path.join(ROOT, f), "utf8"));
    return byFile.get(f);
  };
  const out = Object.create(null);
  for (const [key, spec] of Object.entries(COMPOSERS))
    out[key] = composerRowsFromDefs(spec.defs, read, key);
  return (memo = out);
}

/* The kinds of the rows a consumer carries, over one or more composers, as lists a header line can print.
   A carried row no composer publishes is refused: it would print `null` for ever and read as an artifact too
   old to state it. Every published row has a kind, so the partition below is total. */
function kindsOf(carried, keys) {
  const all = censusKinds();
  const ks = keys || Object.keys(all);
  const kindOf = Object.create(null), published = new Set();
  for (const k of ks) {
    const c = all[k];
    if (!c) throw new Error("testing/census_rows.js: no composer `" + k + "` — " + Object.keys(all).join(", "));
    for (const n of Object.keys(c.kindOf)) kindOf[n] = c.kindOf[n];
    for (const n of [...c.numeric, ...c.object]) published.add(n);
  }
  const absent = carried.filter((n) => !published.has(n));
  if (absent.length)
    throw new Error("testing/census_rows.js: [" + absent.join(", ") + "] is carried by a consumer and " +
                    "published by none of the composers [" + ks.join(", ") + "] — a driver asking for a row " +
                    "nobody emits would print `null` for it for ever and read as an artifact too old to " +
                    "state it, which is a different fact and one this cannot tell it from.");
  const byKind = Object.create(null);
  for (const k of KINDS) byKind[k] = carried.filter((n) => kindOf[n] === k);
  return { byKind };
}

/* A consumer that takes a composer whole carries every row it publishes. A subset is a curation and is not a
   defect; a subset of a composer the consumer declared whole is, because those rows are spliced into their
   document under no name and nothing else can require them. The names stay spelled at the consumer, where
   engine/fieldgate.mjs reads them; this asserts the list is complete. */
function requireWhole(carried, keys) {
  const all = censusKinds();
  const have = new Set(carried);
  for (const k of keys) {
    const c = all[k];
    if (!c)
      throw new Error("testing/census_rows.js: no composer `" + k + "` for a consumer to take WHOLE — " +
                      Object.keys(all).join(", ") + ". A composer named here and nowhere else is a claim " +
                      "about a census that does not exist, and crediting it would assert completeness over " +
                      "an empty row set — which passes for every consumer that ever carried nothing.");
    const missing = [...c.numeric, ...c.object].filter((n) => !have.has(n));
    if (missing.length)
      throw new Error("testing/census_rows.js: the composer `" + k + "` publishes [" + missing.join(", ") +
                      "] and the consumer that declares it TAKEN WHOLE does not carry " +
                      (missing.length === 1 ? "it" : "them") + ". CARRY THE ROW, or stop declaring `" + k +
                      "` whole — those are the two answers and a silent subset is neither. A row whose " +
                      "producer says it is read WITH its siblings — a ladder, a pair, a partition — is a row " +
                      "whose omission makes the siblings unreadable rather than merely absent.");
  }
}

/* A consumer reads each row out of the object that publishes it. An object is not one composer: a census
   document splices several (`_cold` carries `rungEntry`, `fetchEdge` and `xhrEdge` beside `cold`), so the
   caller states the union spliced into each object, which only the producer's arrangement decides. A row
   looked up in the wrong object is `null` on every run, and the message names the composer that owns it. */
function requireFrom(spec) {
  const all = censusKinds();
  for (const k of Object.keys(spec)) {
    const names = spec[k].composers;
    const rows = spec[k].rows;
    const pub = new Set();
    for (const cn of names) {
      const c = all[cn];
      if (!c)
        throw new Error("testing/census_rows.js: no composer `" + cn + "` named as spliced into the `" + k +
                        "` object — " + Object.keys(all).join(", ") + ". A composer named here and nowhere " +
                        "else would widen this check to a row set that does not exist, which passes for any " +
                        "placement at all.");
      for (const n of [...c.numeric, ...c.object]) pub.add(n);
    }
    const wrong = rows.filter((n) => !pub.has(n));
    if (wrong.length) {
      const owner = (n) => {
        const o = Object.keys(all).filter((x) => all[x].numeric.includes(n) || all[x].object.includes(n));
        return o.length ? o.join("/") : "NO COMPOSER";
      };
      throw new Error("testing/census_rows.js: the consumer reads [" +
                      wrong.map((n) => n + " (published by `" + owner(n) + "`)").join(", ") +
                      "] out of the `" + k + "` object, which splices [" + names.join(", ") +
                      "] and does not publish " +
                      (wrong.length === 1 ? "it" : "them") + ". A row looked up in the wrong census object is " +
                      "`null` on every run for ever and renders identically to an artifact too old to state " +
                      "it — two different facts, and a consumer cannot tell them apart. MOVE the row to the " +
                      "list that reads its own composer's object; the owner is named above so the repair is " +
                      "the move and not another reading of the producer's format string.");
    }
  }
}

module.exports = { KINDS, COMPOSERS, defRowsFromText, censusKinds, kindsOf, requireWhole, requireFrom };
