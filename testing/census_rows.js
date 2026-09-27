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
 * THE CONTRACT IS A SET EQUALITY AND EVERY SIDE OF IT THROWS. Declared-but-not-published is a row that has
 * been renamed or dropped while a kind statement about it stands; published-but-not-declared is a row whose
 * producer has stated nothing about what may be done with it. Neither is defaulted and neither is guessed —
 * CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS, where the default is the concealment. A second declaration for one
 * composer is REFUSED rather than resolved to the first, which is the cure for the hazard CLAUDE.md records at
 * exactly this shape: prose about a machine-read region that WRITES that region's markers becomes one, the
 * derived span collapses, and the contract silently empties. Prose here describes the markers and does not
 * write them, and the duplicate refusal is what makes that a backstop rather than the mechanism.
 *
 * COMPLETENESS IS REQUIRED AND IS NO LONGER COUNTED, AND THE COUNT IS DELETED RATHER THAN KEPT BESIDE THE
 * REFUSAL. This file used to return an `undeclared` count and a driver printed it, on the ground that a kind
 * nobody has determined must not be invented — a WRONG kind is worse than a missing one, since it LICENSES the
 * arithmetic a missing one merely fails to authorise. That argument is still exactly right about the KIND and
 * was never a reason to leave the ROW unnamed: the figure's only reader was whoever re-read a header line, so
 * an omission stayed an omission for as long as nobody did, and a count that shrinks as the work is done is
 * read by exactly the population about to invalidate it. It is a THROW now. The count does not survive beside
 * it, because a count that can no longer be nonzero is dead reporting that reads as live — CLAUDE.md
 * §A-superseded-system-is-DELETED — and a consumer's header clause printing a permanent zero is the same
 * statement in the same costume.
 *
 * WHAT THAT COSTS IS BORNE DELIBERATELY AND IS NOT A GAP. A composer row nobody carries must now be declared
 * anyway, so the person adding a row states its kind whether or not any driver has yet asked for it. That is
 * the whole of the price and it is paid by the one person who can pay it cheaply: they are already reading the
 * site that raises the row, which is the only place the kind can be determined from.
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
 * A SECOND CONDITION STOOD HERE AND HAS BEEN MET RATHER THAN DROPPED — the `undeclared` count was to go when
 * every row of every composer carried a declaration, and it has gone. It is recorded as met rather than
 * deleted because the argument a reader re-derives is the one that put the count there: a kind must not be
 * invented, so an undeclared row cannot be given a default, so the reader has to do SOMETHING other than
 * answer — and counting is the move that suggests itself. The paragraph above says why the refusal is that
 * something, so a reader who re-derives the premise does not re-derive the count.
 *
 * NAMED RESIDUAL. NOT COVERED: the TEMPO of the refusal rather than its subject. This reader throws when a
 * DRIVER runs it, and no stage of `engine/build.mjs` asks it anything — verified, not assumed: that file names
 * this one nowhere — so the interval between a row landing with no kind and anybody meeting the refusal is
 * whatever interval separates a commit from the next driver run, which nothing bounds. WHAT THE NEXT DIFF
 * BUILDS: a build stage that CALLS this reader rather than re-deriving its rule, because a second
 * implementation of the composer walk and the declaration walk is CLAUDE.md §AN-AUDITOR-DERIVES-THE-RULE's
 * redundant copy and the copy that drifts is the one nobody runs against reality; what such a stage buys is
 * tempo and never coverage, and saying so is part of proposing it. HOW ITS ABSENCE SHOWS: an undeclared row
 * rides green through every build it is present for and refuses the first driver run afterwards, so the diff
 * that meets the refusal is reliably not the diff that introduced it, and the reader who meets it is reliably
 * not its author. */

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
  /* THE INVOKER RUNGS' DENOMINATOR, spliced into `_cold` beside `stepUnitRuns` exactly as the two edges above
     are spliced beside `epAsks` — so its rows are this composer's and not `cold`'s, and the kind statement that
     covers them has to name THIS composer or it names nothing. `key` shape for the edges' reason: the rows go
     out through `json_buf_key`, whose argument the compiler already refuses to let be anything but a literal. */
  rungEntry: { file: "engine/host/solver/rung_entry.c",
               from: "char *rung_entry_rows(void)",          to: "\n}\n", shape: "key" },
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

/* THE JOIN, WHICH IS WHERE THE SET EQUALITY LIVES AND WHERE BOTH OF ITS DIRECTIONS THROW. A declared name the
   composer does not publish is a kind statement about a row that is no longer emitted — a sentence about
   nothing that a consumer will go on printing. A published row no line declares is the other direction, and it
   is a REFUSAL rather than a count: the count it replaces could only be read by somebody re-reading a header,
   which is nobody, while a throw is met by every consumer at once.
   IT FIRES FOR EVERY COMPOSER AND NOT ONLY FOR THE ROWS A CONSUMER ASKED ABOUT, because `censusKinds` walks
   all of them before any consumer selects a subset. That is deliberate and is the whole forcing function: a row
   added to a census no driver carries still stops the next driver run, so the kind is stated by the person who
   added the row rather than by whoever later needs it.
   AND IT IS WHY `kindsOf` NO LONGER ASKS ITS OWN VERSION OF THIS QUESTION. That function used to throw on a
   CARRIED row with no declared kind; with this refusal standing, the declared set and the published set are one
   set, so its carried-and-published check already decides it and a carried-and-undeclared row cannot exist to
   be caught. It was DELETED rather than kept as a backstop — CLAUDE.md §AN-ASSERT-WHOSE-TWO-SIDES-CANNOT-
   DISAGREE: a check that cannot fail is not a weak check but a non-check that certifies what it never
   examined. Its REMEDY sentence survives, in the message below, at the site that can actually fire. */
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
  /* THE TWO MARKER LITERALS IN THE MESSAGE BELOW ARE SPLIT ACROSS A CONCATENATION ON PURPOSE AND ARE NOT A
     TYPO. CLAUDE.md records the defect: prose that WRITES the delimiter a derivation scans for BECOMES one,
     the derived span collapses, and the contract silently empties. Nothing scans this file today — the
     declaration reader above is handed the C composer's source, never this — so the split is insurance against
     the day somebody points a reader at `testing/` and not a present requirement. It costs two characters and
     removes the only way this message could ever falsify the thing it is reporting on.
     THE TEMPLATE NAMES ONE ROW AND NEVER THE WHOLE LIST, because a kind is determined per row: several rows
     with no kind are several questions, and a template spelling them on one line would suggest filing them
     under whichever kind the first one turned out to be. The full list is in the sentence above it. */
  const undeclared = [...published].filter((n) => !(n in kindOf));
  if (undeclared.length)
    throw new Error(label + ": " + JSON.stringify(spec.from) + " publishes [" + undeclared.join(", ") + "] " +
                    "and the `@kinds" + "-of " + key + "` block in that same file states no kind for " +
                    (undeclared.length === 1 ? "it" : "them") + ". STATE THE KIND THERE — one line per kind, " +
                    "`@kin" + "d <" + KINDS.join("|") + ">: " + undeclared[0] + "`, and a row per row — " +
                    "DETERMINED FROM " +
                    "THE SITE THAT RAISES THE ROW AND NEVER FROM ITS NAME: a key spelling `Life` is a lifetime " +
                    "counter of its own BUCKET while the ROW may be an extremum or a sum over whichever " +
                    "buckets a walk reached, so it can FALL, and a kind read off the key is read off the wrong " +
                    "noun. The kind is stated at the emitter because the person adding a row is the person " +
                    "who knows what may be done with it; a consumer two directories away has only the name. " +
                    "THIS IS A REFUSAL AND NOT A COUNT, which is a decision and not an oversight: a figure " +
                    "for how many rows had no kind was read by whoever re-read a driver's header line, so it " +
                    "left an omission standing for as long as nobody did, and it is gone. Declaring the row " +
                    "is the only way past this, because a kind nobody has determined must not be invented — a " +
                    "WRONG kind licenses the arithmetic a missing one merely fails to authorise.");
  return { key, kindOf, numeric: rows.numeric, object: rows.object };
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
   line can print. ONE THING IS REFUSED HERE AND IT IS NOT THE MISSING KIND — a carried row the composers do
   not publish at all is a DRIVER asking for a row nobody emits, which is this function's own question, and it
   would otherwise print `null` for ever while reading as an artifact too old to state the row.
   THE MISSING KIND IS REFUSED ONE LEVEL UP AND THIS FUNCTION MUST NOT RE-ASK IT. `censusKindsFromText` now
   makes the declared set and the published set ONE set per composer, so `carried ⊆ published` — the check
   below — already entails `carried ⊆ declared`, and a second `if` testing the second would be a predicate
   whose two sides cannot disagree. There WAS such a check here and it is deleted rather than demoted to a
   backstop, because the transcript of a check that can never fire is the harm: it reads as the thing that
   guarantees the property while the guarantee is entirely upstream, and it would go on reading that way if
   the upstream refusal were ever weakened to a count again. Its remedy sentence moved to the refusal that
   can fire.
   SO THE PARTITION BELOW IS TOTAL BY CONSTRUCTION, which is worth stating because a reader is entitled to
   ask: every carried name is published, every published name is declared, and every declared kind is one of
   `KINDS` — so the four lists sum to `carried` and a name cannot fall out of all of them. */
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

module.exports = { KINDS, COMPOSERS, composerRowsFromText, kindDeclarationFromText, censusKindsFromText,
                   censusKinds, kindsOf };
