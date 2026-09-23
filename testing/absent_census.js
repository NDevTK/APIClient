/* THE OWED-GLOBALS CENSUS, READ THROUGH ITS EMITTER'S OWN COMPOSER — one reader, two drivers.
 *
 * WHAT THE CENSUS IS. `engine/host/solver/absent.c`'s `absent_json` counts the names a STANDARD owns that a
 * document READ and this realm did not answer, beside the population of global reads they are drawn from.
 * CLAUDE.md §NO-STUBS names it as the one absence this project's forcing function cannot surface: a page
 * writes `if (window.X)`, this engine does not have `X`, the read is CORRECTLY decided false, the fallback
 * branch runs, and every endpoint and sink behind the true branch is unreachable with nothing throwing. It is
 * discoverable only by COMPARISON, so it needs an instrument rather than a crash — and the instrument is only
 * as good as its readers. `solver/result.c` publishes it, `extension/bridge.js` relays it onto every run
 * record, `extension/popup.js` renders it; the drivers are where it had one reader and one gap.
 *
 * WHY THIS IS A MODULE. `testing/corpus/site.mjs` resolved two of the census members by a distinctive
 * SUBSTRING of their prose and asserted each matched exactly one key, and left a residual saying the names
 * should be DERIVED from `absent_json()`'s own composer rather than matched against copies of them. A second
 * driver needing the same two numbers cannot copy that extractor: CLAUDE.md §AN-AUDITOR-DERIVES-THE-RULE — a
 * restated rule is a second copy, and the one that drifts is the copy nobody runs against reality. This tree
 * has already paid for that twice in this directory, at `testing/artifact_stamp.js` ("It was a copy in each
 * driver and only this one carried the refusal") and at `testing/platform_names.mjs`, whose shape this file
 * follows deliberately: parse the generated C, fail loud when its shape moves, and give every consumer ONE
 * reader so the day the emitter changes, one thing fails rather than one failing and the other silently
 * measuring something else.
 *
 * WHAT IS COPIED HERE IS THE TOKEN AND NOT THE PROSE, WHICH IS THE WHOLE OF THE REPAIR. `absent_json` names
 * its members with long prose sentences on purpose — `popup.js` renders a census GENERICALLY as `key value`,
 * so a terse field name arrives at a person in the exact shape of a row, and a row there is the name of a
 * component this engine owes. A driver that holds a fragment of that sentence has frozen a HUMAN SURFACE: the
 * prose can no longer be reworded without a reader noticing. A driver that holds `KEY_OWED` has frozen
 * nothing a person reads — the C identifier is the stable token, its value is free to be rewritten, and a
 * reword reaches every consumer unedited.
 *
 * AND THE ALTERNATIVE THAT LOOKS LIKE THIS ONE IS REFUTED BY THE RENDERER: emitting a short machine key
 * BESIDE the prose label would put a second member on the census, and `popup.js` prints every member it
 * finds, so a person would meet the same number twice under two names. That is the rule `absent_json` already
 * states for its own row total ("THE TOTAL IS NOT A FOURTH BUCKET … CLAUDE.md §EVIDENCE-INFLATION in a JSON
 * object"), and it would buy a stable token this file gets for nothing. It would also be a cross-boundary
 * diff — C, trusted zone and drivers — where this is a JS-only one.
 *
 * MEMBERS OPEN ON `_` AND ROWS CANNOT. That is `absent.c`'s own namespace rule, asserted there with a DCHECK
 * over the generated tables, and it is what lets this file tell the six-plus-three MEMBER keys from
 * `KEY_ENTRY`'s bucket names with no list of names here. A consumer sums the ROWS to get the reads and reads
 * the MEMBERS as the population they are drawn from.
 *
 * THE POPULATION IS ASSERTED TO AGREE BEFORE A NUMBER IS TAKEN, AND THE ARTIFACT IS ASKED RATHER THAN A
 * REVISION. CLAUDE.md §AND-A-STAMP-ANSWERS-WHICH-ARTIFACT-ANSWERED: an instrument that derives its question
 * from the tree and asks it of something BUILT can disagree with that build about the POPULATION while every
 * stamp check passes, and the direction is the flattering one — a shorter derived list finds fewer absences
 * and reads as progress. `platform_names.mjs` answers that by re-reading its header at the artifact's stamped
 * revision. This file does not have to: the census IS the artifact speaking, so the derived member set is
 * compared against the census's OWN `_`-prefixed keys. Equal means the composer on disk is the composer that
 * wrote this object. Unequal is a FACT ABOUT THE PAIR and is reported as one, naming both sides, rather than
 * silently resolving what it can — a reading taken from a census a different composer wrote belongs to
 * neither revision.
 *
 * NO DEFAULT AND NO FALLBACK, in either direction. A census this driver cannot resolve reports the refusal;
 * it never yields 0, which would report a clean engine for as long as the drift stood, and it never falls
 * back to a substring, which would be the superseded reader kept as a safety net.
 *
 * RETIREMENT: this record's middle three paragraphs go when `absent_json` states its member identities in a
 * way a consumer can read without parsing C — at which point the parse below has nothing to derive from and
 * this file becomes the resolver alone. */

"use strict";

const { readFileSync } = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..");
const COMPOSER = path.join(ROOT, "engine/host/solver/absent.c");

/* THE TOKENS A DRIVER ASKS FOR. These are C IDENTIFIERS in `absent_json`, never its strings, and they are the
   only copy of anything this file holds. BOTH OR NEITHER is the consumers' rule and not this file's: the
   FRACTION is the whole point, and `owed: 0` against `asked > 0` is the positive statement that this engine
   answered every name the standards were asked for while `owed: 0` against `asked: 0` is a census that was
   never reached — opposite findings, one digit. They are exported so a caller states which it wants rather
   than this file deciding for it. */
const TOKEN_ASKED = "KEY_READS";
const TOKEN_OWED = "KEY_OWED";

/* THE PARSE, OVER TEXT, ANCHORED ON THE COMPOSER'S OWN FUNCTION. Scoped to `absent_json`'s body so a `KEY_`
   declaration added elsewhere in the file cannot enter this set, and closed on a brace in column zero, which
   is the only `}` C puts there inside a translation unit of this shape.
   IT THROWS RATHER THAN RETURNING SHORT. An instrument that silently measures a subset of a census reports a
   smaller absence and reads as progress, which is the direction this whole census exists to make visible. */
function absentMembersFromText(src, label) {
  const open = src.indexOf("char *absent_json(void)");
  if (open < 0) throw new Error(label + ": no `absent_json` composer — solver/absent.c's shape changed");
  const close = src.indexOf("\n}\n", open);
  if (close < 0) throw new Error(label + ": `absent_json` is unterminated");
  const body = src.slice(open, close);

  const byToken = Object.create(null);
  const add = (tok, val) => {
    if (tok in byToken)
      throw new Error(label + ": `absent_json` declares " + tok + " twice — this file resolves a census " +
                      "member by that token and cannot say which of two it means");
    byToken[tok] = val;
  };
  for (const m of body.matchAll(/static\s+const\s+char\s+(KEY_[A-Z_0-9]+)\s*\[\s*\]\s*=\s*"((?:[^"\\]|\\.)*)"/g))
    add(m[1], m[2]);
  /* AND THE PER-STANDARD KEYS, WHICH ARE AN ARRAY AND NOT SIX SCALARS — `absent.c`'s header promises one row
     per vocabulary indexed by its enum, so a third standard adds a member here with nothing else to edit.
     Their token is the array's name with the index appended, which no scalar can collide with. */
  for (const m of body.matchAll(/static\s+const\s+char\s*\*\s*const\s+(KEY_[A-Z_0-9]+)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}/g)) {
    const lits = [...m[2].matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((x) => x[1]);
    if (!lits.length) throw new Error(label + ": `absent_json`'s " + m[1] + " parsed empty");
    lits.forEach((v, i) => add(m[1] + "[" + i + "]", v));
  }

  /* MEMBERS ARE THE KEYS THAT OPEN ON `_`, WHICH IS THE EMITTER'S OWN RULE AND NOT A LIST HERE — so
     `KEY_ENTRY`'s bucket names ("read", "typeof", "in"), which are written INSIDE a row rather than as
     members of the object, fall out of this set by the same fact `absent.c` asserts about them. */
  const members = Object.keys(byToken).filter((t) => byToken[t].startsWith("_")).map((t) => byToken[t]);
  if (!members.length) throw new Error(label + ": `absent_json` declares no member keys — every member of " +
                                       "that object opens on `_`, so an empty set is a parse that matched " +
                                       "nothing rather than a census with no members");
  if (!(TOKEN_ASKED in byToken) || !(TOKEN_OWED in byToken))
    throw new Error(label + ": `absent_json` no longer declares " + TOKEN_ASKED + " and " + TOKEN_OWED +
                    " — those are the two this tree's drivers resolve the owed FRACTION from, and a renamed " +
                    "token is a deliberate edit to re-point here rather than a number to default");
  return { byToken, members };
}

let memo = null;
function absentMembers(composer = COMPOSER) {
  if (composer === COMPOSER && memo) return memo;
  const m = absentMembersFromText(readFileSync(composer, "utf8"), path.relative(ROOT, composer));
  if (composer === COMPOSER) memo = m;
  return m;
}

/* THE READING. Returns exactly one of three shapes and never mixes them, because they are three different
   facts about this run and a consumer acts differently on each:
     { asked: null, owed: null }  the question was NOT ASKED — no census on the record at all, which is a run
                                  that crashed before composing one or an artifact predating the census.
     { err }                      a census IS present and this file cannot resolve it — the composer that
                                  wrote it is not the composer on disk. Never a zero.
     { asked, owed }              both numbers, from the keys the composer names.
   `absent === undefined` and `absent === null` are the same fact here and are deliberately not split: the
   engine emits this member on every document it composes, so both mean the record carries none. */
function absentPair(census, composer = COMPOSER) {
  if (census === undefined || census === null) return { asked: null, owed: null };
  if (typeof census !== "object") return { err: "the `absent` census on this record is not an object (" +
                                                typeof census + ")" };
  const { byToken, members } = absentMembers(composer);

  /* THE POPULATION AGREEMENT, ASSERTED OVER THE MEMBER KEYS AND PRINTED IN THE REFUSAL. Compared as SETS
     rather than in order: the order of the emitted members is not a fact a consumer may key on, and an
     ordering change is not a change to the population. */
  const here = new Set(members);
  const there = Object.keys(census).filter((k) => k.startsWith("_"));
  const thereSet = new Set(there);
  const missing = members.filter((k) => !thereSet.has(k));
  const extra = there.filter((k) => !here.has(k));
  if (missing.length || extra.length) {
    const show = (a) => a.slice(0, 4).map((s) => JSON.stringify(s)).join(", ") +
                        (a.length > 4 ? " … (+" + (a.length - 4) + ")" : "");
    return { err: "the `absent` census on this record was written by a different composer than the one in " +
                  "this tree — " + members.length + " members here against " + there.length + " on the " +
                  "record" +
                  (missing.length ? "; " + missing.length + " declared here and absent there: " + show(missing) : "") +
                  (extra.length ? "; " + extra.length + " present there and not here: " + show(extra) : "") +
                  ". Resolving what matched would take a number out of a census no revision of solver/" +
                  "absent.c composed. Rebuild and reinstall the artifact, or read this row at the revision " +
                  "the artifact was stamped at." };
  }

  const num = (tok) => {
    const k = byToken[tok];
    const v = census[k];
    if (typeof v !== "number")
      return { err: "the `absent` census states " + JSON.stringify(k) + " (solver/absent.c's " + tok +
                    ") as a non-number" };
    return { v };
  };
  const a = num(TOKEN_ASKED), o = num(TOKEN_OWED);
  if (a.err || o.err) return { err: a.err || o.err };

  /* AND WHICH NAMES THEY WERE, BECAUSE `owed 3` IS A NUMERATOR NOBODY CAN ACT ON. The count says a document
     asked for something this realm could not answer; the NAMES say what to build, and they are the whole
     work queue — `extension/popup.js` already puts them in front of a person for exactly that reason ("a row
     reading `owed 0` is a real clean bill and a row naming three interfaces is a work queue"), and until this
     the drivers carried the digit and dropped the list.
     A ROW IS EVERY KEY THAT DOES NOT OPEN ON `_`, which is the same emitter rule the member set above uses
     read the other way round, so there is no second list of names at either end. Sorted, because the emission
     order is the order names were first missed and two runs of one page would otherwise differ on a field a
     reader compares by eye.
     AND THE PARTS ARE ASSERTED TO SUM TO THE TOTAL, which is the one property of this pair a consumer can
     check without re-deriving the mechanism: `absent.c` raises exactly one bucket per owed read and asserts
     each row's buckets sum to that row's own reads, so the buckets of every row sum to `owed`. A count that
     cannot be true is caught here rather than carried — and it is a REFUSAL rather than a thrown assert
     because these bytes came out of an artifact this process did not build. */
  let bucketSum = 0;
  const names = Object.keys(census).filter((k) => !k.startsWith("_")).sort();
  for (const n of names) {
    const row = census[n];
    if (!row || typeof row !== "object")
      return { err: "the `absent` census states the owed name " + JSON.stringify(n) + " as " + typeof row +
                    " rather than as the per-entry histogram solver/absent.c writes for every row" };
    for (const b of Object.keys(row)) {
      if (typeof row[b] !== "number")
        return { err: "the `absent` census states bucket " + JSON.stringify(b) + " of owed name " +
                      JSON.stringify(n) + " as a non-number" };
      bucketSum += row[b];
    }
  }
  if (bucketSum !== o.v)
    return { err: "the `absent` census's owed rows do not sum to the owed total it states (" + bucketSum +
                  " by the buckets of " + names.length + " row(s), " + o.v + " by the member) — solver/" +
                  "absent.c raises exactly one bucket per owed read and asserts that identity at the " +
                  "composer, so a disagreement here is a row lost between the engine and this reader and " +
                  "every fraction taken off these numbers would be a fraction of a denominator that is not " +
                  "the number of owed reads" };
  return { asked: a.v, owed: o.v, names };
}

module.exports = { ABSENT_COMPOSER: COMPOSER, TOKEN_ASKED, TOKEN_OWED,
                   absentMembersFromText, absentMembers, absentPair };
