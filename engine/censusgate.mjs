/* THE NUMERIC-GATED FAMILY AUDIT — a census row a consumer SELECTED and its own type clause cannot admit.
 *
 * WHAT THIS IS FOR, AND IT IS ONE QUESTION AND NOT THE OBVIOUS ONE. A census row a consumer never carries is
 * a CURATION and not a defect: `testing/census_rows.js` says so at its own banner ("A SUBSET IS NOT A DEFECT
 * and cannot be made one in general"), `testing/live-run.js` carries a deliberate minority of
 * `result_cold_json` by design, and the count of published names no consumer spells is a large fraction of the
 * whole — PRINTED ON EVERY RUN as this gate's CONSUMER SIDE line rather than quoted here, for the reason the
 * next paragraph gives. A gate accusing those would be red on every run for ever, which CLAUDE.md
 * §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN rates as the worst band to land in: a verdict with no change to notice
 * becomes furniture and the body under it is where nobody goes.
 *   THE FIGURE IS DERIVED AND NOT WRITTEN, AND THAT IS A CORRECTION THIS FILE'S AUTHOR MADE TO HIMSELF — the
 * sentence here said "147 of 298" from a probe written an hour before this gate, and the gate says NINETY. The
 * probe's population was a HAND-TYPED LIST OF SIX CONSUMER PATHS and it omitted `testing/live-wfq.js`, which
 * spells 84 published rows: CLAUDE.md §AND-THE-COMMONEST-WAY-A-POPULATION-COMES-OUT-SHORT, whose tell is that
 * "your sweep's command contains a path you typed, and its finding is a claim about the tree", committed by the
 * author of a gate whose own population is `git ls-files` precisely so that it cannot be. The retired number is
 * recorded rather than deleted because the SHAPE of the error is what a reader re-derives: a banner figure over
 * a census subsystem whose composers grow is dated status in a file whose job is to end a drift, and the only
 * form of it that cannot rot is the line the gate prints.
 *   WHAT IS A DEFECT IS A ROW THE CONSUMER *SELECTED* AND STRUCTURALLY CANNOT RECEIVE. These consumers derive
 * their row sets rather than hand-typing them — which is CLAUDE.md §AN-AUDITOR-DERIVES-THE-RULE obeyed — and
 * every one of those derivations ends in a TYPE CLAUSE, `typeof c[k] === "number"`. A published row whose
 * value is an OBJECT reaching such a clause is dropped on every artifact that publishes it, for ever, and the
 * consumer's own curation says it wanted the row: the family is the claim. The symptom is the one
 * §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES names — an ABSENT reading and a ZERO reading are different facts and
 * this folds them — and it is the sibling of `census_rows.js`'s `requireFrom`, where a row read out of the
 * WRONG OBJECT is `null` on every run for ever. Same silence, one clause over.
 *
 * MEASURED, AND IT IS WHY THIS IS A GATE AND NOT A NOTE. At 011e086, `testing/corpus/site.mjs` selected
 * `Object.keys(c).filter(k => k !== "outOfPrograms" && k.startsWith("outOfPrograms") && typeof c[k] ===
 * "number")` and `solver/result.c` publishes `outOfProgramsAtTheLadderUnits` with `%s`. The row matched the
 * prefix, the type clause dropped it, and `solver/cold.h` built that partition for the ONE question the
 * scalar beneath it cannot answer — mass above the orphan rung is a precondition, below it a fact about the
 * heap, on a frame-clearing arm the pick, and those three take opposite work. A grep for the key in that file
 * answered ONE (`grep -cF`, and `grep -oF | wc -l` also ONE) and that one line TALKS ABOUT the row. It was
 * found by a lane reading, which is how all twelve instances this subsystem counts in its own prose were
 * found: `grep -oi "consumer that never asked"` answers 8 in that file, 4 in `testing/corpus/report.mjs` and 1
 * in `testing/live-run.js`, and `report.mjs` numbers its own at "tenth, eleventh and twelfth".
 *
 * WHAT IT DOES NOT RE-IMPLEMENT, WHICH IS THE WHOLE OF ITS RIGHT TO EXIST. The producer's row set and the
 * numeric/object split are `censusKinds()`'s, called and not copied — so a composer that grows a row is
 * covered on the run it lands, and there is no second walk of `result.c`'s format strings to drift from the
 * two that already exist (`census_rows.js`'s and `build.mjs`'s `censusRowSet`). CLAUDE.md
 * §AN-AUDITOR-DERIVES-THE-RULE cuts both ways: a third copy would be the one nobody runs against reality.
 *   AND CALLING IT DISCHARGES A NAMED RESIDUAL RATHER THAN ADDING A MECHANISM. `census_rows.js` carries one
 * whose WHAT-THE-NEXT-DIFF-BUILDS clause is "a build stage that CALLS this reader rather than re-deriving its
 * rule", and whose HOW-ITS-ABSENCE-SHOWS clause is that an undeclared row "rides green through every build it
 * is present for and refuses the first driver run afterwards, so the diff that meets the refusal is reliably
 * not the diff that introduced it". `censusKinds()` THROWS when a composer's published set and its declared
 * `@kind` set disagree in either direction, so this stage buys that refusal's TEMPO for free — which is
 * exactly what the residual says such a stage buys and all it says it buys. VERIFIED rather than assumed:
 * `grep -cF census_rows engine/build.mjs` answered 0 before this landed, against `fieldgate` 2, `idlgen` 4,
 * `mdgate` 1, `jscommentgate` 1 and `argaudit` 2 as the armed control.
 *
 * WHAT `fieldgate.mjs` ALREADY ASKS AND WHY IT CANNOT ASK THIS. Its WRITE-WITH-NO-READER category diffs the C
 * engine's emitted names against what any consumer spells, and its namespace-holder band HOLDS a name a
 * consumer lists as a literal and reads computed — correctly, because that IS a reader. Both of this gate's
 * controls were in that band: at 011e086 `outOfProgramsAtTheLadderUnits` is a literal in `live-run.js`'s own
 * list AND a real member read in `extension/popup.js:1709`, and the replay triple is a literal in
 * `live-run.js`'s. So neither was a write with no reader, and a gate asking that question was right to be
 * quiet. The question here is about ONE consumer's own selection, which a corpus-wide name grep cannot
 * express.
 *
 * THE THREE BANDS ARE THREE VERDICTS AND NEVER ONE SUM, on §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN's rule that an
 * instrument which cannot see something has not found anything:
 *   FINDING     a published OBJECT row inside a numeric-gated family, which that consumer does not separately
 *               carry. Carries the exit code.
 *   DECISION    the same, declared at the consumer with a reason through `@census-declines`. Counted and
 *               printed; no exit code. The declaration is the artifact: a row the next reader meets is either
 *               carried or argued for, never merely forgotten.
 *   BLIND SPOT  a numeric gate whose family this cannot resolve — a computed prefix, a list that is a
 *               parameter, a name no `const` in the file binds. Printed with its place and NOT counted as
 *               clean, because `testing/corpus/report.mjs` holds one (`k.startsWith(d.pre + "Ask")`) and a
 *               silently-skipped gate is the absence of a question.
 * The denominator prints on every run including the clean one, for the reason that paragraph gives: a line
 * that appears only on the bad day is one nobody learns to look for.
 *
 * IT PROVES IT CAN FAIL, EVERY RUN. CLAUDE.md §THE-ORDER-IS-FIXED-AND-IT-IS-TWO-RUNS — a control that has
 * never produced a finding is not a control, and this gate's whole claim is about a silence, so the claim is
 * measured rather than asserted. It derives a corrupt copy of a real consumer by splicing a published OBJECT
 * row's name into a numeric-gated inline family, requires the scanner to NAME that row on the copy, requires
 * the pristine file not to name it, and FAILS if either is absent. The copy is a string in memory: nothing is
 * written, and §A-PRIVATE-INDEX's rule about exercising a working `.mjs` on a COPY is why.
 *
 * ITS POPULATION IS A CONSTRUCT AND NO PATH IS TYPED. `git ls-files` filtered by extension over the two trees
 * that read a census, so a consumer added tomorrow is in it — §a-sweep's-command-contains-a-path-you-typed.
 *
 * NAMED RESIDUAL. NOT COVERED: a family whose members reach the type clause through a CALL rather than through
 * a `for … of` or a `.filter`, which this resolves by neither — the three shapes it reads are the three this
 * tree writes, and a fourth is a deliberate edit here rather than something it guesses past. WHAT THE NEXT
 * DIFF BUILDS: resolution of a family handed to a helper, by reading that helper's own declaration in the same
 * file as `census_rows.js` reads a region locator's. HOW ITS ABSENCE WOULD SHOW: a numeric gate printed in the
 * BLIND SPOT band whose family a reader can name by eye — the band is the report of it, which is why the band
 * is printed in full rather than counted. */

import { readFileSync } from "fs";
import { execFileSync } from "child_process";
import { createRequire } from "module";
import { join, dirname } from "path";
import { fileURLToPath } from "url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..");
const require = createRequire(import.meta.url);
const { censusKinds } = require(join(ROOT, "testing", "census_rows.js"));

/* ---- the producer side, DERIVED BY CALLING and never walked again -------------------------------------- */
/* A NAME'S CONVERSION IS PER COMPOSER AND IS FOLDED THE OBJECT WAY, which is the direction that cannot invent
   a finding: a name emitted with `%s` by ANY composer is one a numeric clause drops wherever that composer's
   document is the one in hand, and a consumer reading the other composer's object is holding a different
   number under one name — which is `requireFrom`'s question and not this one. Folding the NUMERIC way would
   let one composer's scalar spelling excuse another's object, so the union is the arm that stays sound. */
const kinds = censusKinds();                  /* THROWS when published and declared disagree — see the banner */
const publishedObject = new Map();            /* name -> [composer, …] that emit it with an object conversion */
const publishedAny = new Set();
for (const c of Object.keys(kinds)) {
  for (const n of kinds[c].numeric) publishedAny.add(n);
  for (const n of kinds[c].object) {
    publishedAny.add(n);
    if (!publishedObject.has(n)) publishedObject.set(n, []);
    publishedObject.get(n).push(c);
  }
}

/* ---- the consumer population, from a construct and not a path ------------------------------------------ */
const tracked = execFileSync("git", ["ls-files", "-z", "testing", "extension"], { cwd: ROOT, encoding: "utf8" })
  .split("\0").filter((p) => p.endsWith(".js") || p.endsWith(".mjs"));
if (!tracked.length)
  throw new Error("[censusgate] `git ls-files` named no JavaScript under testing/ or extension/ — an empty " +
                  "population passes every consumer that ever read a census, so it stops here rather than " +
                  "reporting a clean bill over nothing.");

/* ---- masking: comments blank, STRING LITERALS INTACT --------------------------------------------------- */
/* THE LITERALS ARE THE EVIDENCE HERE, WHICH IS WHY ONLY COMMENTS GO. A family's members and a prefix are
   string literals, so a mask that blanked them would leave this reading nothing to read; and a family name
   written in a COMMENT must not be one, which is the direction that manufactures accusations. Characters are
   replaced one for one so every offset is still the file's own and a line number is never computed from a
   shortened copy. A template literal is left alone: nothing in this tree spells a census row in one, and a
   nested `${}` is not a thing a character scan can balance. */
function maskComments(src) {
  let out = "";
  const N = src.length;
  for (let i = 0; i < N; ) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "*") {
      const e = src.indexOf("*/", i + 2);
      const end = e < 0 ? N : e + 2;
      for (let j = i; j < end; j++) out += src[j] === "\n" ? "\n" : " ";
      i = end; continue;
    }
    if (c === "/" && src[i + 1] === "/") {
      let e = src.indexOf("\n", i);
      if (e < 0) e = N;
      out += " ".repeat(e - i);
      i = e; continue;
    }
    if (c === '"' || c === "'" || c === "`") {
      /* A `"` OR `'` STRING CLOSES ON ITS OWN LINE OR IT IS NOT ONE, which is not a convenience — ECMAScript
         §12.9.4 makes an unescaped LineTerminator in a single- or double-quoted literal a syntax error, so a
         quote with no partner before the newline is a quote inside something else. The something else is almost
         always a REGEX LITERAL (`/["']/` is written in this corpus), and a scanner that took it as an opener
         would run to the next quote anywhere in the file — masking live code or unmasking a comment, in
         whichever direction the file happened to be written. Bounding the scan to the line is exactly the
         language's own rule and makes the worst case one line rather than one file. A backtick is NOT bounded,
         because a template literal legitimately spans lines. */
      let j = i + 1;
      const stop = c === "`" ? N : (src.indexOf("\n", i) < 0 ? N : src.indexOf("\n", i));
      while (j < stop && src[j] !== c) { if (src[j] === "\\") j++; j++; }
      if (j >= stop) { out += c; i++; continue; }
      out += src.slice(i, j + 1);
      i = j + 1; continue;
    }
    out += c; i++;
  }
  return out;
}
const lineOf = (src, off) => 1 + (src.slice(0, off).match(/\n/g) || []).length;

/* ---- the declaration, which is the artifact ------------------------------------------------------------ */
/* `@census-declines <row> — <reason>` IN A COMMENT OF THE CONSUMER THAT DECLINES IT, read from the UNMASKED
   source for the one reason the mask exists: a decline is PROSE and a mask built to keep code would blank it.
   A reason is REQUIRED and is not a courtesy — a bare name is a silent exemption with a marker on it, which is
   the allowlist this gate has none of. */
const DECLINE = /@census-declines[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*(?:—|--)[ \t]*(\S[^\n]*)/g;

/* ---- the consumer scan -------------------------------------------------------------------------------- */
/* A NUMERIC GATE IS `typeof <recv>[<var>] === "number"`, matched as a CONSTRUCT and with its own variable
   captured, because what makes a family numeric-gated is that the clause tests the member the family names and
   not some other key in scope. The reversed spelling is admitted for the same reason a fixed-string search is
   preferred elsewhere: both are what the language permits and neither is a different fact. */
const GATE = /typeof\s+([A-Za-z_$][\w$]*)\s*\[\s*([A-Za-z_$][\w$]*)\s*\]\s*===\s*["']number["']|["']number["']\s*===\s*typeof\s+([A-Za-z_$][\w$]*)\s*\[\s*([A-Za-z_$][\w$]*)\s*\]/g;
const LITS = /["']([A-Za-z_][A-Za-z0-9_]*)["']/g;

/* HOW FAR A GATE MAY STAND FROM ITS FAMILY, AND IT IS A WINDOW RATHER THAN A SCOPE BECAUSE A SCOPE WOULD MEAN
   PARSING. Measured over the three shapes in this tree the furthest is `site.mjs`'s prefix filter at 71
   characters between the `startsWith(` and the `typeof`; the window is an order above that so a comment-free
   reformat cannot move a family out of it, and a gate whose family falls outside is a BLIND SPOT and not a
   silence — it lands in the band with its place, which is the whole reason the band is printed. */
const WINDOW = 700;

const findings = [], decisions = [], blind = [];
let gatesSeen = 0, familiesResolved = 0, consumersWithGates = 0, membersConsidered = 0;
/* THE CONSUMER-SIDE DENOMINATOR, WHICH IS PRINTED BECAUSE IT IS THE EVIDENCE FOR THIS GATE'S OWN SCOPE AND NOT
   BECAUSE IT IS A FINDING. The argument for asking about FAMILIES rather than about carriage is arithmetic — a
   published row no consumer spells is a curation and there are very many of them — and an argument resting on a
   figure in a banner is one that rots the day a composer grows (CLAUDE.md §A-COUNT-IS-THE-ONE-DERIVED-
   COORDINATE). So the figure is re-derived on every run and the banner's number is a reading rather than a
   claim. It is counted over EVERY tracked file and not only the ones holding a gate, because a consumer that
   carries rows and tests no type is still a consumer. */
const spelledBySomeone = new Set();
const perConsumer = [];
for (const rel of tracked) {
  const code0 = maskComments(readFileSync(join(ROOT, rel), "utf8"));
  const hits = new Set();
  for (const m of code0.matchAll(/\.([A-Za-z_][A-Za-z0-9_]*)\b/g)) if (publishedAny.has(m[1])) hits.add(m[1]);
  for (const m of code0.matchAll(/["']([A-Za-z_][A-Za-z0-9_]*)["']/g)) if (publishedAny.has(m[1])) hits.add(m[1]);
  if (hits.size) perConsumer.push([rel, hits.size]);
  for (const n of hits) spelledBySomeone.add(n);
}
perConsumer.sort((a, b) => b[1] - a[1]);

for (const rel of tracked) {
  const src = readFileSync(join(ROOT, rel), "utf8");
  if (!/typeof/.test(src)) continue;
  const code = maskComments(src);

  const declined = new Map();
  for (const m of src.matchAll(DECLINE)) declined.set(m[1], m[2].trim());

  /* WHAT THIS CONSUMER CARRIES SEPARATELY, AND IT IS EVERY SPELLING AND NOT THE REPAIR'S OWN CONSTRUCT —
     WHICH IS A CORRECTION MADE BY RUNNING THIS, AND THE RETIRED RULE IS KEPT BECAUSE IT IS THE INTUITIVE ONE.
     It tested `.row` and `["row"]` only, on the ground that both real repairs spell `const lu =
     c.outOfProgramsAtTheLadderUnits;`. That is how the repairs happened to be written and is not what carriage
     IS: `testing/live-run.js` carries the same row as a LITERAL on `COLD_FRONTIER` and reads the whole list
     through `(k in c)` rather than a numeric clause, so the row arrives and the narrow test reported it
     dropped. The file SAYS SO at its own line — "`outOfProgramsAtTheLadderUnits` IS A NAMED ROW AND SITS ON
     THE LIST, because it is an OBJECT" — which is §THE-REFUTATION-IS-INSIDE-THE-TEXT-YOU-ARE-ABOUT-TO-QUOTE,
     and the direction is the one CLAUDE.md rates as needing more suspicion than the quiet one: an accusation
     against correct code is acted on by editing it, and the edit here would have been a second carriage of a
     row already carried.
     SO THE TEST IS A SPELLING IN CODE, which is the weakest claim that is still sound: a consumer that writes
     a row's name has MADE A DECISION about that row, whatever route it then reads it by, and one that writes it
     nowhere has not. A comment does not count, which the mask already arranges — at 011e086 the name occurs
     exactly ONCE in `site.mjs` and that one line talks about the row. */
  const spelled = new Set();
  for (const m of code.matchAll(/\.([A-Za-z_][A-Za-z0-9_]*)\b/g)) if (publishedAny.has(m[1])) spelled.add(m[1]);
  for (const m of code.matchAll(/["']([A-Za-z_][A-Za-z0-9_]*)["']/g)) if (publishedAny.has(m[1])) spelled.add(m[1]);

  const gates = [...code.matchAll(GATE)];
  if (!gates.length) continue;
  consumersWithGates++;
  gatesSeen += gates.length;

  for (const g of gates) {
    const gvar = g[2] || g[4];
    const at = g.index;
    const before = code.slice(Math.max(0, at - WINDOW), at);
    const place = `${rel}:${lineOf(src, at)}`;
    /* A FAMILY'S OWN LIST IS NOT SEPARATE CARRIAGE OF ITS OWN MEMBERS, which is why the spelling set above is
       asked MINUS this family's text. An inline or named list spells every member it holds, so crediting that
       spelling would make the inline and named arms unable to fire at all — a check whose two sides cannot
       disagree. A PREFIX family spells none of its members, so nothing is subtracted there and the whole
       spelling set answers, which is the arm both controls exercise. */
    const members = [];        /* {name, how} */
    let ownText = "";
    const why = [];

    /* (1) AN INLINE FAMILY: `for (const k of ['a','b'])` — the list is its own evidence. */
    const inl = [...before.matchAll(new RegExp(String.raw`\bof\s*\(?\s*\[([^\]]*)\]`, "g"))].pop();
    const forVar = [...before.matchAll(/\bfor\s*\(\s*(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s+of\s/g)].pop();
    if (inl && forVar && forVar[1] === gvar) {
      for (const lm of inl[1].matchAll(LITS)) if (publishedAny.has(lm[1])) members.push({ name: lm[1], how: "an inline list" });
      if (members.length) { why.push("an inline list"); ownText = inl[1]; }
    }

    /* (2) A NAMED FAMILY: `for (const k of ROWS)` with `const ROWS = [ … ]` bound in this same file. A name no
       `const` here binds is NOT guessed at — it is the BLIND SPOT the band exists for. */
    if (!members.length && forVar && forVar[1] === gvar) {
      const named = [...before.matchAll(/\bfor\s*\(\s*(?:const|let|var)\s+[A-Za-z_$][\w$]*\s+of\s+([A-Za-z_$][\w$]*)\s*\)/g)].pop();
      if (named) {
        const bind = new RegExp(String.raw`\b(?:const|let|var)\s+` + named[1] + String.raw`\s*=\s*\[([\s\S]*?)\]`);
        const b = bind.exec(code);
        if (b) {
          for (const lm of b[1].matchAll(LITS)) if (publishedAny.has(lm[1])) members.push({ name: lm[1], how: "the list `" + named[1] + "`" });
          if (members.length) { why.push("the list `" + named[1] + "`"); ownText = b[1]; }
        } else {
          blind.push({ place, what: "a numeric gate over `" + named[1] + "`, which no `const … = [ … ]` in " +
                                    "this file binds — the family is real and its members are not readable here" });
          continue;
        }
      }
    }

    /* (3) A PREFIX FAMILY: `startsWith("<lit>")` beside the gate. The members are the PRODUCER'S rows with
       that prefix, which is why this arm needs no list at the consumer at all — and a prefix that is not a
       literal is the band and never a guess. */
    if (!members.length) {
      const sw = [...before.matchAll(/\.startsWith\s*\(\s*([^)]*)\)/g)].pop();
      if (sw) {
        const lit = /^\s*["']([A-Za-z_][A-Za-z0-9_]*)["']\s*$/.exec(sw[1]);
        if (!lit) {
          blind.push({ place, what: "a numeric gate over a prefix family whose prefix is COMPUTED (`" +
                                    sw[1].trim().slice(0, 60) + "`) — the family is real and this cannot " +
                                    "enumerate it from source" });
          continue;
        }
        for (const n of publishedAny) if (n !== lit[1] && n.startsWith(lit[1]))
          members.push({ name: n, how: 'the prefix "' + lit[1] + '"' });
        if (members.length) why.push('the prefix "' + lit[1] + '"');
      }
    }

    if (!members.length) {
      blind.push({ place, what: "a numeric gate whose family reaches it through neither a `for … of` list nor " +
                                "a `startsWith` prefix within " + WINDOW + " characters — see the residual" });
      continue;
    }
    familiesResolved++;
    membersConsidered += members.length;

    /* THE SPELLINGS THIS FAMILY'S OWN LIST CONTRIBUTES, SUBTRACTED ONCE PER FAMILY RATHER THAN GLOBALLY —
       because a row on one numeric-gated list and ALSO on another list of this consumer's is carried, and a
       global subtraction would lose that. */
    const own = new Set();
    for (const lm of ownText.matchAll(LITS)) own.add(lm[1]);
    const elsewhereCount = (n) => {
      let total = 0;
      for (const m of code.matchAll(LITS)) if (m[1] === n) total++;
      for (const m of code.matchAll(/\.([A-Za-z_][A-Za-z0-9_]*)\b/g)) if (m[1] === n) total++;
      let mine = 0;
      for (const m of ownText.matchAll(LITS)) if (m[1] === n) mine++;
      return total - mine;
    };

    for (const m of members) {
      if (!publishedObject.has(m.name)) continue;
      if (own.has(m.name) ? elsewhereCount(m.name) > 0 : spelled.has(m.name)) continue;
      const row = { place, row: m.name, how: m.how, by: publishedObject.get(m.name).join("/") };
      if (declined.has(m.name)) decisions.push({ ...row, reason: declined.get(m.name) });
      else findings.push(row);
    }
  }
}

/* ---- THE CONTROL, WHICH RUNS BEFORE THE VERDICT IS BELIEVED -------------------------------------------- */
/* TWO RUNS AND NEVER ONE. The scanner above is a function of a file's bytes, so the control is the same
   scanner over bytes that are KNOWN to hold the defect — a published OBJECT row spliced into an inline
   numeric-gated family, in a file with no other reference to it — and over bytes that are known not to. A
   green verdict from a scanner that has not been shown FIRING is a statement about the probe and not about
   the tree, so a control that does not arm FAILS this stage. */
function scanOne(src, publishedObjectNames) {
  const code = maskComments(src);
  const declined = new Set();
  for (const m of src.matchAll(DECLINE)) declined.add(m[1]);
  const elsewhere = new Set();
  for (const m of code.matchAll(/\.([A-Za-z_][A-Za-z0-9_]*)\b/g)) elsewhere.add(m[1]);
  const find = [], dec = [];
  for (const g of code.matchAll(GATE)) {
    const gvar = g[2] || g[4];
    const before = code.slice(Math.max(0, g.index - WINDOW), g.index);
    const inl = [...before.matchAll(new RegExp(String.raw`\bof\s*\(?\s*\[([^\]]*)\]`, "g"))].pop();
    const forVar = [...before.matchAll(/\bfor\s*\(\s*(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s+of\s/g)].pop();
    if (!inl || !forVar || forVar[1] !== gvar) continue;
    for (const lm of inl[1].matchAll(LITS)) {
      if (!publishedObjectNames.has(lm[1]) || elsewhere.has(lm[1])) continue;
      (declined.has(lm[1]) ? dec : find).push(lm[1]);
    }
  }
  return { find, dec };
}
const objNames = new Set(publishedObject.keys());
const CTL_ROW = [...objNames][0];
if (!CTL_ROW)
  throw new Error("[censusgate] no composer publishes an OBJECT-valued row, so this gate's subject is empty " +
                  "and its control cannot arm. An empty subject passes for every consumer there has ever " +
                  "been, which is the clean bill CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE forbids reporting.");
const FAM = (rows) => "const o = {};\nfor (const k of [" + rows.map((r) => "'" + r + "'").join(", ") +
                      "])\n  if (typeof c[k] === 'number') o[k] = c[k];\n";
const ctlQuiet = scanOne(FAM(["live", "steps"]), objNames);
const ctlFired = scanOne(FAM(["live", CTL_ROW]), objNames);
/* AND THE DECLINE ARM IS ARMED TOO, WHICH IS NOT BELT-AND-BRACES: the DECISION band is the one thing here that
   can turn a finding into a silence, so a band nobody has seen fire is a band that might be swallowing rows
   through a marker nobody has shown works. Same bytes, one comment added. */
const ctlDeclined = scanOne("/* @census-declines " + CTL_ROW + " — a control, to show this band can fire */\n" +
                            FAM(["live", CTL_ROW]), objNames);
const armed = ctlFired.find.includes(CTL_ROW) && !ctlQuiet.find.length && !ctlQuiet.dec.length &&
              ctlDeclined.dec.includes(CTL_ROW) && !ctlDeclined.find.includes(CTL_ROW);
if (!armed)
  throw new Error(`[censusgate] THE CONTROL DID NOT ARM, so no verdict below it is about this tree. Over a ` +
                  `numeric-gated inline family holding the published OBJECT row \`${CTL_ROW}\` the scanner ` +
                  `found [${ctlFired.find.join(", ")}] and should have named it; over the same family with an ` +
                  `\`@census-declines\` beside it, DECISIONS [${ctlDeclined.dec.join(", ")}] and FINDINGS ` +
                  `[${ctlDeclined.find.join(", ")}] — the first should name it and the second must not; over a ` +
                  `family of two scalars, FINDINGS [${ctlQuiet.find.join(", ")}] and DECISIONS ` +
                  `[${ctlQuiet.dec.join(", ")}], both of which should be empty. A clean verdict from a scanner ` +
                  `nobody has seen reject anything is a statement about the probe (CLAUDE.md ` +
                  `§THE-ORDER-IS-FIXED-AND-IT-IS-TWO-RUNS), and a DECISION band nobody has seen fire is a ` +
                  `band that may be swallowing findings through a marker that does not work.`);

/* ---- the report: three verdicts, a denominator on every run ------------------------------------------- */
const composers = Object.keys(kinds);
console.log(`[censusgate] CONTROL ARMED — on a corrupt copy the scanner NAMED \`${CTL_ROW}\` as a finding, ` +
            `with an \`@census-declines\` beside it moved the same row to DECISIONS and out of findings, and ` +
            `on a clean copy named nothing in either band.`);
console.log(`[censusgate] DENOMINATOR: ${composers.length} composer(s) [${composers.join(", ")}] publishing ` +
            `${publishedAny.size} distinct row(s), ${publishedObject.size} of them OBJECT-valued ` +
            `[${[...publishedObject.keys()].join(", ")}]; ${tracked.length} tracked JS file(s) under testing/ ` +
            `and extension/, ${consumersWithGates} of them holding a numeric gate; ${gatesSeen} gate(s), ` +
            `${familiesResolved} famil(ies) resolved over ${membersConsidered} member(s).`);
console.log(`[censusgate] the published row set and every row's kind are \`censusKinds()\`'s and are not ` +
            `re-derived here; its own published-vs-declared refusal ran above.`);
console.log(`[censusgate] CONSUMER SIDE: ${spelledBySomeone.size} of those ${publishedAny.size} row(s) are ` +
            `spelled in some consumer's code and ${publishedAny.size - spelledBySomeone.size} in none — which ` +
            `is a CURATION and is this gate's reason for asking about selected families instead: a verdict over ` +
            `that second number would be red on every run. Per consumer: ` +
            perConsumer.map(([f, n]) => `${f} ${n}`).join(", ") + ".");

if (blind.length) {
  console.log(`[censusgate] BLIND SPOTS: ${blind.length} numeric gate(s) whose family this cannot enumerate. ` +
              `An instrument that cannot see something has not found anything, so these are printed and are ` +
              `not counted as clean:`);
  for (const b of blind) console.log(`  ${b.place}  ${b.what}`);
}
if (decisions.length) {
  console.log(`[censusgate] DECISIONS: ${decisions.length} row(s) a consumer selected, cannot receive, and ` +
              `DECLARED declined with a reason. Not a finding:`);
  for (const d of decisions) console.log(`  ${d.place}  ${d.row} (${d.by}, via ${d.how}) — ${d.reason}`);
}
if (!findings.length) {
  console.log(`[censusgate] 0 FINDING(S): no consumer selects a published OBJECT row through a numeric type ` +
              `clause without carrying it separately.`);
  process.exit(0);
}
console.log(`[censusgate] ${findings.length} FINDING(S) — a row the consumer SELECTED and its own type clause ` +
            `drops on every artifact that publishes it, for ever:`);
for (const f of findings)
  console.log(`  ${f.place}  \`${f.row}\` is published by \`${f.by}\` with an OBJECT conversion and is in this ` +
              `gate's family by ${f.how}, so \`typeof === "number"\` excludes it BY CONSTRUCTION. The reading ` +
              `is ABSENT on every run and renders identically to an artifact too old to state the row — two ` +
              `facts, and the consumer cannot tell them apart. CARRY it on a line of its own, or DECLINE it ` +
              `with \`@census-declines ${f.row} — <reason>\` at this consumer; a silent drop is neither.`);
process.exit(1);
