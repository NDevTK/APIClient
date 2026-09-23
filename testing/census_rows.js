/* WHAT KIND OF NUMBER A CENSUS ROW IS, READ FROM THE PRODUCER THAT EMITS IT — one reader, every driver.
 *
 * WHAT THIS IS FOR. CLAUDE.md §A-GAUGE-AND-A-LIFETIME-COUNTER: a gauge states what is true NOW and may never
 * be differenced; a lifetime count states what has happened since the start and may; a constant is written
 * once at a seed and is neither; a maximum is monotone like a count and is a HIGH-WATER MARK, so a plateau in
 * one is not a ceiling and it is not comparable across two runs of different length. Four kinds, one shape of
 * number, and the names do not say which. A quantity whose kind a reader cannot name FROM ITS OUTPUT is one
 * they are not entitled to do arithmetic on, so the kind is part of publishing the row.
 *
 * WHY IT IS DERIVED AND WHAT IT REPLACES. `testing/live-run.js` kept SIX hand lists that were a kind
 * statement, and its own banner said why they were hand-kept: `engine/build.mjs`'s `coldFields()` derives
 * PRESENCE — which rows a composer publishes — and "no artifact in this tree states a @COLD row's kind at
 * all", so a kind list was "a fact only a reader of the producer's header can state, and a derivation that
 * guessed it from a name would be guessing". That is the gap this file closes: the producer states it, in a
 * machine-read declaration beside the composer, and every consumer derives.
 *
 * THE LISTS ANSWERED TWO QUESTIONS AND ONLY ONE OF THEM IS DERIVABLE, which is the whole of the design and is
 * why this file does not return "the rows". CLAUDE.md §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS, in a data
 * table: those lists said WHICH ROWS A DRIVER CARRIES and WHAT KIND EACH IS, and the two have different
 * owners. The curation is the DRIVER'S and is deliberately a subset — `live-run.js`'s `census()` banner
 * refuses to take everything in as many words, because "a driver that took everything would be a second copy
 * of the popup" — so deriving it would not repair the driver, it would replace its output. MEASURED at the
 * revision this landed: `result_cold_json` publishes 124 rows and that driver carries 56; `result_wfq_json`
 * publishes 107 and it carries 9. The KIND is the PRODUCER'S and is what no artifact stated. So a consumer
 * keeps one curated list and asks this file what each member is.
 *
 * THREE EMISSION SHAPES AND NOT ONE, which a consumer must not paper over. `solver/result.c` composes with
 * printf format strings; `solver/endpoint.c` composes its two host-edge row sets with a key-emitting macro
 * whose argument is a string literal, and splices them into the @COLD document through a bare conversion that
 * names no row. That third fact is why the eleven `epFetch*`/`epXhr*` rows are invisible to
 * `censusComposerFields`' `name-colon-conversion` match and therefore to `build.mjs`'s presence loop: an
 * unnamed splice lands in neither its numeric list nor its object one, so the mismatch that would report it
 * never fires. This file reads BOTH shapes, so those rows have a kind here even though nothing in this tree
 * can yet require their presence.
 *
 * THE CONTRACT IS TWO-SIDED AND BOTH SIDES THROW. A declared name the composer does not publish is a row that
 * has been renamed or dropped while a kind statement about it stands; a carried row with no declared kind is a
 * row a consumer is about to print under a kind statement that does not cover it. Neither is defaulted and
 * neither is guessed — CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS, where the default is the concealment. A second
 * declaration for one composer is REFUSED rather than resolved to the first, which is the cure for the hazard
 * CLAUDE.md records at exactly this shape: prose about a machine-read region that WRITES that region's
 * markers becomes one, the derived span collapses, and the contract silently empties. Prose here describes the
 * markers and does not write them, and the duplicate refusal is what makes that a backstop rather than the
 * mechanism.
 *
 * IT IS NOT COMPLETE AND IT SAYS SO IN ITS OWN OUTPUT. A composer row nobody has declared and nobody carries
 * is silent here, because a kind nobody has determined must not be invented — the wrong kind is worse than a
 * missing one, since it LICENSES the arithmetic the missing one merely fails to authorise. So `undeclared` is
 * returned beside the kinds and a consumer prints it, which makes the gap a figure that shrinks as the work is
 * done rather than a sentence that rots. CLAUDE.md: print the partition on the same line as the count.
 *
 * NO BUILD IS NEEDED AND THIS IS NOT A CROSS-BOUNDARY DIFF, which is established rather than assumed. The
 * declarations are C COMMENT TEXT: they change no emitted byte, so there is no half that goes live at a
 * different instant from the other, and this reader takes them from SOURCE exactly as `absent_census.js` takes
 * `absent.c`'s identifiers and `build.mjs` takes `result.c`'s format strings. A driver using it therefore
 * answers correctly against an artifact of any age — including one whose stamp predates the rows, where the
 * kind is stated and the row is absent, which are two different facts and stay two here.
 *
 * NO DEFAULT, NO FALLBACK, NO SUFFIX RULE. `endpoint.c` spells a lifetime count's kind into its row names with
 * a suffix and says at that line that "a comment stating it is read by nobody holding the number"; live-run.js
 * refused to key on that suffix because "a suffix rule standing beside these lists would be two mechanisms
 * answering one question with the partial one drifting". Both are right and this file keys on neither — the
 * suffix is the producer's note to a reader and the declaration is the producer's statement to a machine.
 *
 * RETIREMENT: this file's derivation goes when a census states each row's kind in the DOCUMENT it emits, at
 * which point a consumer reads the kind off the same object it reads the number off and has nothing to parse.
 * The `undeclared` count goes when every row of both composers carries a declaration. */

"use strict";

const { readFileSync } = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..");

/* THE KINDS, WHICH ARE THIS FILE'S ONE CLOSED SET AND ARE SPELLED HERE SO A TYPO IN A DECLARATION IS A THROW
   RATHER THAN A FIFTH KIND NOBODY DEFINED. What a consumer may do with each is the whole content:
     lifetime  raised and never lowered — MAY be differenced across two samples of ONE instance.
     gauge     a walk at one instant — may FALL, so differencing it reads a level as a rate.
     constant  written once at a seed and never again — neither differenced nor read as a level.
     maximum   monotone like a count and a HIGH-WATER MARK — it saturates and then plateaus, so a plateau is
               NOT a ceiling and the series length is part of quoting it. CLAUDE.md records a run of six
               censuses whose maximum read the same low number at every revision because the runs were short,
               and a whole cross-revision comparison that was a comparison of run lengths. It is filed apart
               from `lifetime` for that reason and for no other: both may be differenced and only one may be
               compared across two runs. */
const KINDS = ["lifetime", "gauge", "constant", "maximum"];

/* THE COMPOSERS THIS FILE KNOWS, EACH WITH THE SHAPE IT EMITS IN. `from`/`to` bound the function body the way
   `build.mjs`'s reader bounds it, and `shape` picks which match finds a row. A composer added here is one
   line; a composer whose shape is neither of these is a THIRD reader and a deliberate edit, never something
   this file guesses past. */
const COMPOSERS = {
  cold:      { file: "engine/host/solver/result.c",
               from: "char *result_cold_json(void)", to: "\n}\n", shape: "format" },
  wfq:       { file: "engine/host/solver/result.c",
               from: "char *result_wfq_json(void)",  to: "\n}\n", shape: "format" },
  fetchEdge: { file: "engine/host/solver/endpoint.c",
               from: "char *endpoint_fetch_edge_rows(void)", to: "\n}\n", shape: "key" },
  xhrEdge:   { file: "engine/host/solver/endpoint.c",
               from: "char *endpoint_xhr_edge_rows(void)",   to: "\n}\n", shape: "key" },
};

/* THE ROWS A COMPOSER PUBLISHES, FROM ITS OWN EMISSION AND NOT FROM A LIST HERE. The format shape matches the
   key and its conversion and splits on the object conversion, which is the same rule `build.mjs` states and
   for its reason: an object row is a nested composer's and takes a different check from a number. The key
   shape matches the macro's string-literal argument, which `endpoint.c` records cannot be anything else
   ("a `const char *` in that position is a syntax error").
   NO C STRING DECODING, on the rule the sibling readers in this tree already keep: a name is matched as plain
   bytes, so a key needing an escape is simply not found — and not-found is this function's LOUD arm. */
function composerRowsFromText(src, spec, label) {
  const open = src.indexOf(spec.from);
  if (open < 0)
    throw new Error(label + ": no composer " + JSON.stringify(spec.from) + " — its shape moved, and this " +
                    "reader takes a census's row set from the composer rather than from a list beside it. A " +
                    "renamed composer is a change to be told about, not one to guess a remembered list past.");
  if (src.indexOf(spec.from, open + 1) >= 0)
    throw new Error(label + ": " + JSON.stringify(spec.from) + " occurs twice — one of them is prose quoting " +
                    "the other, and this reader cannot say which body it is meant to read. A region anchored " +
                    "on a string that its own surrounding prose also writes yields a SHORT or a WRONG row " +
                    "set, which is a contract that got weaker rather than one that broke.");
  const close = src.indexOf(spec.to, open);
  if (close < 0)
    throw new Error(label + ": the composer " + JSON.stringify(spec.from) + " is unterminated — an " +
                    "unterminated region yields a SHORT row set, which is a contract that silently got " +
                    "weaker rather than one that broke, so it stops here.");
  const body = src.slice(open, close).replace(/\/\*[\s\S]*?\*\//g, " ").replace(/^[ \t]*\/\/.*$/gm, " ");

  const numeric = [], object = [];
  if (spec.shape === "format") {
    for (const m of body.matchAll(/\\"([A-Za-z_][A-Za-z0-9_]*)\\":%([-0-9.]*)([a-zA-Z]+)/g)) {
      const to = m[3] === "s" ? object : numeric;
      if (!numeric.includes(m[1]) && !object.includes(m[1])) to.push(m[1]);
    }
  } else if (spec.shape === "key") {
    for (const m of body.matchAll(/json_buf_key\s*\(\s*&?[A-Za-z_][A-Za-z0-9_]*\s*,\s*"([^"\\]*)"\s*\)/g))
      if (!numeric.includes(m[1])) numeric.push(m[1]);
  } else {
    throw new Error(label + ": unknown emission shape " + JSON.stringify(spec.shape));
  }
  if (!numeric.length && !object.length)
    throw new Error(label + ": the composer " + JSON.stringify(spec.from) + " names no row at all — every " +
                    "row it publishes is written in its own emission, so a composer with none is one whose " +
                    "shape this reader no longer recognises, and an empty row set would pass every census " +
                    "ever printed.");
  return { numeric, object };
}

/* THE DECLARATION. Its header binds a block to ONE composer by that composer's function NAME — never by its
   signature, which is what the row reader anchors on and which prose must therefore not write. Each line of a
   block names one kind and the rows of that kind.
   A SECOND BLOCK FOR ONE COMPOSER IS REFUSED AND NOT RESOLVED TO THE FIRST, which is the cure for the hazard
   this file's banner names: a comment that writes the markers a derivation scans for BECOMES one, and the
   quiet failure is a span that collapses to the first match while every consumer goes on reading a shorter
   contract as a complete one. A duplicate is louder than prose discipline and does not depend on anybody
   remembering the rule.
   A ROW DECLARED TWICE IS REFUSED FOR THE SAME REASON — under one kind it is noise and under two it is a
   contradiction, and a reader that took the first would publish whichever the file happened to state first. */
function kindDeclarationFromText(src, composer, label) {
  const head = new RegExp("@kinds-of[ \\t]+" + composer + "\\b", "g");
  const hits = [...src.matchAll(head)];
  if (!hits.length)
    throw new Error(label + ": no kind declaration for the composer `" + composer + "` — this reader will " +
                    "not guess a row's kind from its name, because a WRONG kind is worse than a missing one: " +
                    "it licenses the arithmetic a missing one merely fails to authorise.");
  if (hits.length > 1)
    throw new Error(label + ": " + hits.length + " kind declarations for the composer `" + composer + "` — " +
                    "one of them is prose about the other, and a reader taking the first would publish a " +
                    "shorter contract as a complete one. This is the shape that empties a machine-read " +
                    "region in silence, so it stops here rather than resolving.");
  /* THE BLOCK RUNS FROM ITS HEADER TO THE END OF THE COMMENT IT IS IN, which is the one terminator a C
     comment cannot contain. Anchored forward from the header so a block is bounded by its own comment rather
     than by a count of lines, which an inserted sentence would move. */
  const start = hits[0].index;
  const end = src.indexOf("*/", start);
  if (end < 0)
    throw new Error(label + ": the kind declaration for `" + composer + "` is not inside a terminated " +
                    "comment — an unterminated block runs to the end of the file and would sweep every later " +
                    "row name into whichever kind it stated last.");
  const block = src.slice(start, end);

  const kindOf = Object.create(null);
  let lines = 0;
  for (const m of block.matchAll(/@kind[ \t]+([a-z]+)[ \t]*:([^\n]*)/g)) {
    lines++;
    if (!KINDS.includes(m[1]))
      throw new Error(label + ": the kind declaration for `" + composer + "` states the kind " +
                      JSON.stringify(m[1]) + ", which is not one of " + KINDS.join(", ") + ". A fifth kind " +
                      "is a real thing to add here deliberately; a typo silently files rows under a kind no " +
                      "consumer knows what to do with.");
    for (const name of m[2].split(/[\s,]+/).filter(Boolean)) {
      if (name in kindOf)
        throw new Error(label + ": the kind declaration for `" + composer + "` states `" + name + "` twice (" +
                        kindOf[name] + " and " + m[1] + ") — under one kind that is noise and under two it is " +
                        "a contradiction, and a reader taking the first would publish whichever this file " +
                        "happened to state first.");
      kindOf[name] = m[1];
    }
  }
  if (!lines)
    throw new Error(label + ": the kind declaration for `" + composer + "` states no kind line at all — an " +
                    "empty declaration is a parse that matched nothing rather than a census with no kinds, " +
                    "and it would leave every row undeclared while reading as a contract.");
  return kindOf;
}

/* THE JOIN, WHICH IS WHERE THE TWO-SIDED CHECK LIVES. A declared name the composer does not publish throws
   here and names both sides, because a kind statement about a row that is no longer emitted is a sentence
   about nothing that a consumer will go on printing. The other direction — a published row nobody declared —
   is COUNTED and not thrown, because completeness is work in progress and a kind nobody has determined must
   not be invented; a consumer that CARRIES such a row throws at `kindsOf` below, which is the moment the
   omission becomes a false statement rather than an absent one. */
function censusKindsFromText(src, key, spec, label) {
  const rows = composerRowsFromText(src, spec, label);
  const published = new Set([...rows.numeric, ...rows.object]);
  const kindOf = kindDeclarationFromText(src, key, label);
  const gone = Object.keys(kindOf).filter((n) => !published.has(n));
  if (gone.length)
    throw new Error(label + ": the kind declaration for `" + key + "` states a kind for [" + gone.join(", ") +
                    "], which " + JSON.stringify(spec.from) + " no longer publishes — the row has been " +
                    "renamed or dropped while a statement about what a reader may do with it still stands. " +
                    "Re-point the declaration at the row's new name, or drop it with the row.");
  const undeclared = [...published].filter((n) => !(n in kindOf));
  return { key, kindOf, numeric: rows.numeric, object: rows.object, undeclared };
}

let memo = null;
function censusKinds() {
  if (memo) return memo;
  const byFile = new Map();
  const out = Object.create(null);
  for (const [key, spec] of Object.entries(COMPOSERS)) {
    if (!byFile.has(spec.file)) byFile.set(spec.file, readFileSync(path.join(ROOT, spec.file), "utf8"));
    out[key] = censusKindsFromText(byFile.get(spec.file), key, spec, spec.file);
  }
  return (memo = out);
}

/* WHAT A CONSUMER ASKS FOR: the kinds of the rows IT carries, over one or more composers, as lists a header
   line can print. A carried row with no declared kind THROWS and names it — that row is about to be printed
   under a kind statement that does not cover it, which is the defect this whole file exists to end arriving
   one row over. A carried row the composers do not publish at all is a DIFFERENT fact and throws separately:
   the first is a producer that has not stated a kind, the second is a driver asking for a row nobody emits. */
function kindsOf(carried, keys) {
  const all = censusKinds();
  const ks = keys || Object.keys(all);
  const kindOf = Object.create(null), published = new Set();
  let undeclared = 0;
  for (const k of ks) {
    const c = all[k];
    if (!c) throw new Error("testing/census_rows.js: no composer `" + k + "` — " + Object.keys(all).join(", "));
    for (const n of Object.keys(c.kindOf)) kindOf[n] = c.kindOf[n];
    for (const n of [...c.numeric, ...c.object]) published.add(n);
    undeclared += c.undeclared.length;
  }
  const absent = carried.filter((n) => !published.has(n));
  if (absent.length)
    throw new Error("testing/census_rows.js: [" + absent.join(", ") + "] is carried by a consumer and " +
                    "published by none of the composers [" + ks.join(", ") + "] — a driver asking for a row " +
                    "nobody emits would print `null` for it for ever and read as an artifact too old to " +
                    "state it, which is a different fact and one this cannot tell it from.");
  const nokind = carried.filter((n) => !(n in kindOf));
  if (nokind.length)
    throw new Error("testing/census_rows.js: [" + nokind.join(", ") + "] is carried by a consumer and its " +
                    "producer states no kind for it — the row is about to be printed under a kind statement " +
                    "that does not cover it, and a reader who takes the nearest one differences a gauge. " +
                    "State its kind at the composer that emits it, where the person adding the row is the " +
                    "person stating what may be done with it.");
  const byKind = Object.create(null);
  for (const k of KINDS) byKind[k] = carried.filter((n) => kindOf[n] === k);
  return { byKind, undeclared };
}

module.exports = { KINDS, COMPOSERS, composerRowsFromText, kindDeclarationFromText, censusKindsFromText,
                   censusKinds, kindsOf };
