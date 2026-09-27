/* WHAT A STATIC ANALYSER THIS PROJECT DID NOT WRITE RECOVERS FROM THE SAME DOORS, SO "NO PARSE CAN STATE IT"
 * STOPS BEING A CLAIM ABOUT ONE PARSE.
 *
 * THE BAR CLAUDE.md §What-the-tool-produces SETS IS "an address, a key or a value that NO PARSE of the served
 * bytes can state, because it exists only at run time." That is a claim quantified over EVERY parse, and until
 * this file landed the tree had tested exactly ONE: `testing/static_surface.mjs`, which this project wrote.
 * §What-the-tool-produces is explicit that the baseline IS the competitor and must be made as strong as anyone
 * can make it, because a recall hole in the baseline makes the baseline look weak, which makes the engine look
 * strong, and that is the one result the project must not manufacture. The incentive runs the wrong way: every
 * hour spent strengthening our own parser lowers our own score. So a razor point can currently be bought
 * either by making the engine better or by leaving our parser worse, and NOTHING IN THE TREE CAN TELL THOSE
 * TWO APART. This file is the instrument that can.
 *
 * IT IS A CONTROL AND NEVER A TARGET, on exactly the terms `static_surface.mjs`'s own header states for
 * itself and §What-the-tool-produces states for `netdiff --unused`. A number here going DOWN is not progress.
 * The only result this file exists to produce is a LIST: the addresses this project currently credits to
 * execution that a third-party tool states from the text. Finding one LOWERS our score, and that is the point.
 *
 * ── WHICH ADVERSARY, AND WHAT MACHINERY IT HAS THAT OURS DOES NOT ────────────────────────────────────────
 * `typescript`, driven in-process through its own `createProgram` / `getTypeChecker` API. It was chosen over
 * every other analyser reachable here and the reason is machinery rather than convenience:
 *   - A REAL BINDER AND SYMBOL TABLE. `static_surface.mjs` hand-rolls a scope pre-pass (`collectBinds`) and
 *     says so; TypeScript's binder is a production implementation of the same idea that this project did not
 *     write and has no interest in weakening.
 *   - MODULE RESOLUTION ACROSS FILES. This is the decisive one. Our baseline folds a binding only where it is
 *     "declared once in its own scope and written never" — WITHIN ONE FILE. TypeScript builds a module graph
 *     and binds an imported name to its declaration in ANOTHER file, then infers that declaration's type.
 *     MEASURED on one site of the corpus this ran against: 5227 of 5704 named import bindings resolved to a
 *     declaration in a different file. Our baseline can see none of them.
 *   - A CHECKER WITH LITERAL TYPE INFERENCE. `getTypeAtLocation` on an expression can return a StringLiteral
 *     type, which is a VALUE recovered from a non-literal expression — the thing this comparison is about.
 *   - A UNION OF LITERAL TYPES, WHICH OURS HAS NO ANALOGUE OF AT ALL. `const u = c ? A : B` types as
 *     `"a" | "b"`, so ONE site yields TWO complete addresses; and `function pick(c){return c?A:B}` called
 *     anywhere types as `"a" | "b"` too, which is interprocedural literal recovery through a return. Our
 *     baseline folds to ONE value or to none, so every member of such a union past the first is an address it
 *     structurally cannot state.
 * A SECOND AST WALKER WOULD HAVE BEEN WORTHLESS, because it asks the same question the same way. A scope
 * analyser (`eslint-scope`) would have been middling: it has the symbol table and neither the module graph nor
 * the inference. Neither is in this tree; `typescript` was not either, and is declared in `package.json` by
 * the same commit that landed this file.
 *
 * AND WHAT OURS HAS THAT THE ADVERSARY DOES NOT, MEASURED RATHER THAN ASSUMED, BECAUSE A COMPARISON THAT
 * REPORTS ONLY THE DIRECTION THAT FLATTERS THE PROJECT IS THE DEFECT THIS FILE EXISTS TO CLOSE. The probe that
 * chose the tool asked its checker for the type of twelve shapes and the answer is a PARTITION and not an
 * ordering. TypeScript folds `const x = "/lit/"`, folds a cross-file `import { BASE }`, and folds
 * `` `/api/${REG}` `` to `"/api/us"`. It does NOT fold `"/a" + "/b"` (the checker answers `string`), does not
 * fold an object-literal property, and does not inline a function return whose body returns one literal
 * directly. `static_surface.mjs` folds all three. So neither tool dominates, the two columns below are a
 * PARTITION, and a row in one and not the other is a finding about that row rather than about either tool's
 * strength.
 * AND THE ADVERSARY HAS NO `shape` VERDICT AT ALL, WHICH IS A MEASUREMENT AND WAS A SURPRISE. This file was
 * written expecting TypeScript's template-literal types to answer a partially-known template the way our
 * `shape` kind does, and its own selftest REFUSED TO ARM that verdict. Probed directly: a template whose
 * placeholder is a single literal type folds WHOLE (`` `/w/${A}` `` with `const A="a"` types as `"/w/a"`), and
 * a template whose placeholder is anything else — a `string`, an `any`, even a UNION OF TWO LITERALS — types
 * as bare `string`. So for a VALUE expression the checker is all-or-nothing: it recovers the complete address
 * or it recovers no text whatsoever, where our baseline still hands a reader `/api/v2/{?}`. That is a
 * capability OURS has and this adversary does not, it is the reason the `shape` column below reads 0, and the
 * selftest ASSERTS it stays 0 so the day TypeScript changes it, this file fails instead of quietly
 * re-classifying. The `shape` branch in `tsVerdict` is kept for exactly that day.
 *
 * ── THE ROW UNIT, WHICH IS THE ONE THING A COMPARISON OF TWO INSTRUMENTS CAN GET CATEGORICALLY WRONG ──────
 * CLAUDE.md §AND-TWO-INSTRUMENTS-CAN-DISAGREE: differencing two instruments whose ROWS count different things
 * is a category error and not a measurement, and the dominant term is usually the unit rather than any blind
 * spot. So the primary column here is deliberately NOT a site count:
 *   PRIMARY   — ROW UNIT: ONE DISTINCT ADDRESS STRING. A set of the complete address strings each side can
 *               state from the text, per site, and the two SET DIFFERENCES between them. An address is an
 *               address, so the units are identical BY CONSTRUCTION and the difference is a measurement.
 *               `B \ A` is the deliverable: what the adversary states and we credit to a run.
 *   SECONDARY — ROW UNIT: ONE SYNTACTIC CALL SITE. How many door sites each side's walk FOUND. This is a
 *               PARTITION and never a difference: the two walks are two selectors over one corpus, so a gap
 *               is a statement about door recall and says nothing about value recovery.
 * THE ENGINE'S OWN COLUMN IS NOT MEASURED HERE AND THE REASON IS STATED RATHER THAN LEFT TO BE INFERRED: the
 * engine's number is an EMITTED-ADDRESS count from a built artifact, this lane may not build, and a reach
 * column read off an artifact of another revision is a figure belonging to no revision. `node engine/build.mjs`
 * then a drive is what produces it; a reader wanting three columns runs that and joins on the address string,
 * which is why the primary unit here is the address and not the site.
 *
 * ── THE DOOR SET IS DERIVED FROM THE FILE THAT OWNS IT AND NEVER RETYPED ──────────────────────────────────
 * CLAUDE.md §AN-AUDITOR-DERIVES-THE-RULE: a restated rule is a second copy, and the one that drifts is the
 * copy nobody runs against reality. `static_surface.mjs`'s `DOORS` table is the artifact that owns which call
 * shapes are requests — each of its rows already names the engine file whose `endpoint_record` call it mirrors
 * — so this file PARSES that table out of that file's source with `@babel/parser` and THROWS on any shape it
 * cannot read. A door added there is a door here on the next run, and a door table that changes shape fails
 * loudly instead of silently narrowing this comparison. It is not IMPORTED because that file calls `main()` at
 * module scope, so importing it would run it.
 *
 * ── CALIBRATION, BEFORE ANY BREAKDOWN OF ANYTHING IS BELIEVED ─────────────────────────────────────────────
 * Two published totals are reproduced and both are ASSERTED rather than printed and hoped for:
 *   1. `engine/corpus_programs.mjs` publishes `nProgram`/`nDocument` for the corpus. This file walks the same
 *      population through the same function, so a disagreement is impossible by construction and the
 *      function's own four accounting throws are inherited.
 *   2. `static_surface.mjs` publishes `total.sites`, and this file reads its rows through `--examples`. If the
 *      row count it hands back is not exactly that total, the OURS column is a SUBSET and every set difference
 *      below would read as the adversary winning. That is the flattering direction, so it THROWS.
 *
 * ── WHAT THIS FILE CANNOT SEE, STATED FIRST BECAUSE A FIGURE WITHOUT ITS DENOMINATOR IS READ AS A TOTAL ───
 *   - A DOOR NEITHER SIDE READS. Both walks key on the same derived `DOORS`, so a library wrapper
 *     (`axios.get`, an SDK `request()`) is invisible to both and is in no column. The comparison is FAIR and
 *     is not COMPLETE, and its site counts are floors for both sides.
 *   - A VALUE BEHIND A SHAPE NEITHER TOOL FOLDS. Where both answer SHAPE, this file says nothing about
 *     whether some third tool could fold it. One adversary agreeing is not "no parse can state this" and must
 *     never be reported as if it were.
 *   - THE PARAMETER KEYS AND VALUES OF A QUERY STRING, except as they appear inside a recovered address
 *     string. Neither side models a request body, and a key recovered only as part of an address is counted
 *     as part of that address and not separately.
 *   - AN ADDRESS ONE SIDE NORMALISES AND THE OTHER DOES NOT. Both sides here report the raw recovered string,
 *     so `./chunk.js` stays relative on both; the set difference is exact on that population and would be
 *     noise if either side resolved against a base.
 *
 * ── WHAT THIS ADVERSARY IS NOT, AND THE ONE THAT WOULD BE STRONGER — A NAMED RESIDUAL ────────────────────
 * WHAT IS NOT COVERED: A CROSS-FILE CONSTANT FEEDING A CONCATENATION. The two tools compared here have
 * COMPLEMENTARY halves of one capability and NEITHER has the intersection, which is a property of the two
 * tools rather than of any corpus. TypeScript binds an imported name to a declaration in another file and
 * infers its literal type, and does NOT fold binary `+`. `static_surface.mjs` folds `+` and is strictly
 * per-file. So `import { BASE } from "./cfg.js"; fetch(BASE + "/v1/users")` is stated by NEITHER — ours cannot
 * reach across the file and theirs cannot join the two halves — and every address of that shape is currently
 * credited to execution with no parse having been asked that could state it.
 * WHAT THE NEXT DIFF BUILDS: a third column from a bundler that does BOTH, which `esbuild` is and which this
 * tree already declares as a devDependency. Bundle each site's entry with `bundle: true, minify: true`, which
 * links the module graph AND constant-folds across it, then run this file's own door walk over the OUTPUT and
 * take the same address set. The comparison is then a bundler's real cross-module folding against ours, and its
 * row unit is the same distinct address string, so it joins the two columns below without any new unit.
 * HOW ITS ABSENCE WOULD SHOW: a reader would observe, in the output below, a `theirsOnly` of 0 sitting beside a
 * `folded` count of ONE over 86 MiB — a checker that recovered a single address from a single non-literal
 * expression in the whole corpus — and would read the 0 as evidence that no parse states these addresses when
 * what it is evidence of is that THIS parse folds almost nothing on minified input. The two readings take
 * opposite work: the first says the razor holds, the second says the adversary has not been built yet.
 * AND THE `theirsOnly` OF 0 IS REPORTED AS EXACTLY WHAT IT IS AND NO MORE. It is the FIRST INDEPENDENT
 * evidence that this project's baseline is not artificially weak, over one corpus at one hour against one
 * adversary. It is not "no parse can state this", it does not become that by being re-run, and this file's
 * whole reason for existing is that nothing in the tree could previously tell those two claims apart.
 *
 * ── THE CORPUS IS NOT TRACKED AND THE DRIVER IS ───────────────────────────────────────────────────────────
 * `testing/corpus/README.md` is the decision: this repository carries no copy of anybody else's site. So every
 * figure this prints is a fact about ONE FETCH at the instant its manifest names, and the instant is printed
 * with the numbers. With no corpus it THROWS and the throw carries the command that makes one.
 */
import { readFileSync, existsSync, statSync, readdirSync } from "node:fs";
import { resolve, relative, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { parse as babelParse } from "@babel/parser";
import ts from "typescript";
import { corpusPrograms, essenceOf, PROGRAM, DOCUMENT } from "../engine/corpus_programs.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..");
const STATIC_SURFACE = resolve(HERE, "static_surface.mjs");
const TAG = "[ts_surface]";
const die = (m) => { throw new Error(`${TAG} ${m}`); };

/* ── THE DOOR SET, READ OUT OF `static_surface.mjs`'S OWN `DOORS` TABLE ───────────────────────────────────
   A pure function of that file's text, so it can be armed with a string and so a reader can diff the two
   without running anything. Every refusal THROWS: a row this pass cannot read would be a door silently
   missing from the adversary's walk, and a narrower adversary is the flattering direction. */
export function doorsFromSource(src, where) {
  const ast = babelParse(src, { sourceType: "module", errorRecovery: false });
  let arr = null;
  for (const st of ast.program.body) {
    if (st.type !== "VariableDeclaration") continue;
    for (const d of st.declarations)
      if (d.id && d.id.type === "Identifier" && d.id.name === "DOORS") arr = d.init;
  }
  if (!arr) die(`${where} declares no top-level \`DOORS\`, so the door set this comparison walks would be ` +
                `invented here instead of derived from the file that owns it.`);
  if (arr.type !== "ArrayExpression")
    die(`${where}'s \`DOORS\` is a ${arr.type} and this pass can only read an ArrayExpression of object ` +
        `literals. The shape changed; widen this reader rather than letting the door set go short.`);
  const out = [];
  for (const el of arr.elements) {
    if (!el) die(`${where}'s \`DOORS\` has a hole in it.`);
    if (el.type !== "ObjectExpression")
      die(`${where}'s \`DOORS\` carries a ${el.type} element, which this pass cannot read.`);
    const row = {};
    for (const p of el.properties) {
      if (p.type !== "ObjectProperty" || p.computed || p.key.type !== "Identifier")
        die(`${where}'s \`DOORS\` carries a property this pass cannot read (${p.type}).`);
      const v = p.value;
      if (v.type === "StringLiteral") row[p.key.name] = v.value;
      else if (v.type === "NumericLiteral") row[p.key.name] = v.value;
      else if (v.type === "BooleanLiteral") row[p.key.name] = v.value;
      else if (v.type === "NullLiteral") row[p.key.name] = null;
      else die(`${where}'s \`DOORS\` row \`${row.id || "?"}\` carries a ${v.type} for \`${p.key.name}\`, ` +
               `which this pass cannot read.`);
    }
    for (const k of ["id", "cls", "kind", "name"])
      if (typeof row[k] !== "string") die(`a \`DOORS\` row in ${where} has no string \`${k}\`.`);
    if (typeof row.urlArg !== "number") die(`\`DOORS\` row ${row.id} in ${where} has no numeric \`urlArg\`.`);
    out.push(row);
  }
  if (!out.length) die(`${where}'s \`DOORS\` resolved to no rows at all.`);
  return out;
}

/* THE HTTP-METHOD TEST A `arg0Method` DOOR APPLIES TO ITS FIRST ARGUMENT, DERIVED FOR THE SAME REASON THE
   DOORS ARE — AND THIS WAS A MEASURED FALSE POSITIVE OF THIS FILE AND NOT A THEORETICAL ONE. The first run of
   this comparison reported exactly ONE address the adversary stated and our baseline did not, and it was
   `"manual"`, out of `T.open("first","manual")` — a range or date API, not `XMLHttpRequest`. This file had
   required the first argument to be a string LITERAL where `static_surface.mjs` requires it to BE AN HTTP
   METHOD, so `.open(` matched every library's `open()` with two arguments. A door admitted too widely does not
   make the adversary look stronger in general: it manufactures a razor point against this project out of a
   string that is not an address, which is the ONE result this whole file exists to avoid manufacturing in the
   other direction.
   CLAUDE.md §AND-A-PROPOSED-NARROWING is what decides how to repair it: one false positive proves the rule
   admits at least one and says NOTHING about how many true rows share its spelling, so the fix is built as a
   classifier and its whole price is read. MEASURED over this corpus: the narrowing takes the adversary's
   `xhr.open` sites from 24 to the number the run below prints, and the run ASSERTS the refused population is
   the one our baseline already publishes as `xhrOpenSkipped` — so the two sides apply one test and a
   disagreement is a finding rather than a silent difference in precision. */
export function httpMethodTestFromSource(src, where) {
  const m = /if \(!m \|\| m\.type !== "StringLiteral" \|\| !\/\^\(([A-Z|]+)\)\$\/i\.test\(m\.value\)\)/.exec(src);
  if (!m) die(`${where} states no HTTP-method test this pass can read for its \`arg0Method\` doors. That test ` +
              `is the whole precision of the \`.open(\` door: without it this file admits every library's ` +
              `two-argument \`open()\` and manufactures razor points out of strings that are not addresses.`);
  const names = m[1].split("|");
  if (names.length < 4) die(`${where}'s HTTP-method test resolved to only ${names.length} name(s) ` +
                            `(${JSON.stringify(names)}), which is too few to be the set it owns.`);
  return new Set(names.map((x) => x.toUpperCase()));
}

/* THE GLOBAL OBJECT'S OWN NAMES, so `window.fetch(u)` is the same door as `fetch(u)` on both sides. Read out
   of `static_surface.mjs` for the same reason the doors are. */
export function globalObjectsFromSource(src, where) {
  const m = /const GLOBAL_OBJECTS = new Set\(\[([^\]]*)\]\)/.exec(src);
  if (!m) die(`${where} declares no \`GLOBAL_OBJECTS\` set this pass can read.`);
  const names = [...m[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]);
  if (!names.length) die(`${where}'s \`GLOBAL_OBJECTS\` resolved to no names.`);
  return new Set(names);
}

/* ── THE ADVERSARY'S VERDICT FOR ONE ARGUMENT ─────────────────────────────────────────────────────────────
   FOUR VERDICTS, and they are the four `static_surface.mjs` uses, for the reason its own header gives: a
   single count would decide the question wrongly, because a parser and an interpreter differ on exactly one
   axis — whether the value is in the TEXT or only in a RUN.
     literal  the argument node IS a string literal. Both tools have it; execution adds nothing.
     folded   the node is NOT a literal and the CHECKER's type for it is a string literal type. This is the
              column that matters: a value recovered from a non-literal expression, by a binder and a checker
              this project did not write.
     shape    the checker's type is a TemplateLiteral type — literal text plus at least one hole.
     opaque   no literal text at all.
   `guarded`, `alt` and the reach band are not mirrored: they are properties of the TEXT that both tools would
   answer identically, and mirroring them would be re-implementing our own instrument rather than asking a
   second one a question. */
function tsVerdict(ck, arg) {
  if (!arg) return { kind: "absent", text: null };
  if (ts.isStringLiteral(arg)) return { kind: "literal", text: arg.text, lit: "string" };
  /* A NO-SUBSTITUTION TEMPLATE IS A LITERAL HERE AND A `folded` ROW IN `static_surface.mjs`, WHICH IS A
     LABELLING DIFFERENCE AND NOT A RECALL ONE — AND IT IS THE WHOLE OF WHY THE TWO `literal` COLUMNS BELOW
     DISAGREE BY HUNDREDS. MEASURED on the corpus this ran against: 226 of our baseline's 235 `folded` rows
     have argShape `TemplateLiteral`, and 606 + 226 accounts for essentially all of this side's `literal`
     count. A reader comparing the two `literal` figures column-for-column would read that gap as this
     adversary finding hundreds of addresses ours does not, which is FALSE in the direction that flatters the
     adversary — so the two `literal` counts are NOT comparable label-for-label, only their UNION with
     `folded` is, and that union is exactly what the address sets below are built from. It is counted apart
     rather than argued, so the reconciliation is a number on every run. */
  if (ts.isNoSubstitutionTemplateLiteral(arg)) return { kind: "literal", text: arg.text, lit: "noSubTemplate" };
  let t;
  try { t = ck.getTypeAtLocation(arg); } catch { return { kind: "opaque", text: null }; }
  if (!t) return { kind: "opaque", text: null };
  if (t.isStringLiteral && t.isStringLiteral()) return { kind: "folded", text: t.value };
  if (t.flags & ts.TypeFlags.TemplateLiteral) {
    const texts = t.texts || [];
    if (texts.some((s) => s.length)) return { kind: "shape", text: texts.join("{?}") };
    return { kind: "opaque", text: null };
  }
  /* A UNION OF STRING LITERALS IS SEVERAL COMPLETE ADDRESSES AND NOT A SHAPE, which is a capability our
     baseline has no analogue of at all — it folds to ONE value or to none. Every member is a complete address
     the checker states, so every member is admitted. */
  if (t.isUnion && t.isUnion() && t.types.length && t.types.every((x) => x.isStringLiteral && x.isStringLiteral()))
    return { kind: "folded", text: null, texts: t.types.map((x) => x.value) };
  return { kind: "opaque", text: null };
}

/* ── THE ADVERSARY'S OWN DOOR WALK ────────────────────────────────────────────────────────────────────────
   It keys on the derived `DOORS` and on nothing else, so the two walks ask one question of one corpus and a
   difference in their site counts is a statement about door RECALL rather than about value recovery. */
function walkDoors(sf, ck, doors, globals, methods, onSite) {
  let skippedNonMethod = 0, globalRecvNonMethod = 0;
  const byKindName = new Map();
  for (const d of doors) byKindName.set(d.kind + ":" + d.name, d);
  const nameOfCallee = (ex) => {
    if (ts.isIdentifier(ex)) return { kind: "callee-global", name: ex.text, recv: null };
    if (ts.isPropertyAccessExpression(ex) && ts.isIdentifier(ex.name)) {
      /* A PropertyAccessExpression's OBJECT is `.expression` in TypeScript's AST and `.object` in Babel's.
         Reaching for the Babel spelling here read `undefined` and crashed inside `ts.isIdentifier` — which is
         the one way this file could have gone WRONG rather than short, since a receiver that reads as absent
         would have mis-keyed every member door. */
      const o = ex.expression;
      if (!o) return null;
      if (ts.isIdentifier(o) && globals.has(o.text)) return { kind: "callee-global", name: ex.name.text, recv: o.text };
      const recv = ts.isIdentifier(o) ? o.text
        : (ts.isPropertyAccessExpression(o) && ts.isIdentifier(o.name)) ? o.name.text : null;
      return { kind: "member-call", name: ex.name.text, recv };
    }
    return null;
  };
  /* THE WALK IS ITERATIVE AND THAT IS NOT A STYLE CHOICE. A recursive `visit` consumes one C stack frame per
     level of expression nesting, and a minified bundle's concatenation chain is thousands of levels deep — so
     the depth this file survives is a property of how big its own closure happens to be. MEASURED: widening
     the `new` arm by four lines was enough to turn a run that had completed three times into
     `RangeError: Maximum call stack size exceeded` inside `forEachChildInBinaryExpression`, which means every
     earlier clean run was luck rather than a bound. Raising `--stack-size` would move the edge and keep it;
     and catching the overflow per file would make the adversary quietly narrower on exactly the largest
     bundles, which is the direction that manufactures a razor point for this project. An explicit worklist has
     no such edge at any depth. */
  const visit = (root) => {
    const work = [root];
    while (work.length) {
      const n = work.pop();
      step(n);
      ts.forEachChild(n, (c) => { work.push(c); });
    }
  };
  const step = (n) => {
    let hit = null;
    if (ts.isCallExpression(n) && n.expression) {
      if (n.expression.kind === ts.SyntaxKind.ImportKeyword) hit = byKindName.get("dynamic-import:import") || null;
      else {
        const c = nameOfCallee(n.expression);
        if (c) {
          hit = byKindName.get(c.kind + ":" + c.name) || null;
          /* THE ONE TERM THAT RECONCILES THIS WALK'S `arg0Method` REFUSALS WITH OUR BASELINE'S PUBLISHED
             COUNT, AND IT IS DERIVED RATHER THAN EXPLAINED. `static_surface.mjs` routes `window.open(u,
             "_blank")` into the `.open(` door as a CANDIDATE and then refuses it on the method test, counting
             the refusal; this walk classifies a global-object receiver as `callee-global:open` first, finds no
             such door, and never reaches the test — so the two refusal counts differ while the SITE totals
             agree exactly, because neither side admits the site. That gap was printed for two runs with the
             wrong mechanism beside it ("the two walks differ in door RECALL"), which had become false the
             moment the site totals reconciled. It is counted here so the identity is arithmetic on every run
             instead of a sentence: MEASURED on the corpus this ran against, 96 refusals here + 76 of these =
             172, which is our baseline's figure to the digit. */
          if (!hit && c.recv && globals.has(c.recv)) {
            for (const d of doors)
              if (d.arg0Method && d.name === c.name) {
                const a0 = n.arguments && n.arguments[0];
                if (!(a0 && ts.isStringLiteral(a0) && methods.has(a0.text.toUpperCase())) &&
                    (!d.minArgs || (n.arguments && n.arguments.length >= d.minArgs))) globalRecvNonMethod++;
              }
          }
          /* A RECEIVER-QUALIFIED DOOR names its receiver and is refused on any other. */
          if (!hit && c.kind === "member-call") {
            const q = byKindName.get("member-call-on:" + c.name);
            if (q && q.recv && c.recv === q.recv) hit = q;
          }
          if (hit && hit.kind === "member-call-on" && hit.recv && c.recv !== hit.recv) hit = null;
        }
      }
    } else if (ts.isNewExpression(n)) {
      /* A `new` DOOR REACHED THROUGH THE GLOBAL OBJECT, WHICH THIS WALK MISSED AND OURS DID NOT. Measured on
         the corpus this ran against: `new window.Worker("/public/stallwart.build.js")` is how a real bundle
         spells it, and requiring a bare Identifier callee lost the SITE and the literal address with it — four
         `new Worker` sites across three sites of the corpus. A narrower adversary is the direction that
         manufactures a razor point for this project, so the spelling is admitted here exactly as
         `static_surface.mjs`'s `globalReached` admits it there. */
      if (ts.isIdentifier(n.expression)) hit = byKindName.get("new:" + n.expression.text) || null;
      else if (ts.isPropertyAccessExpression(n.expression) && ts.isIdentifier(n.expression.name) &&
               n.expression.expression && ts.isIdentifier(n.expression.expression) &&
               globals.has(n.expression.expression.text))
        hit = byKindName.get("new:" + n.expression.name.text) || null;
    }
    if (hit) {
      const args = n.arguments ? [...n.arguments] : [];
      let ok = true;
      if (hit.minArgs && args.length < hit.minArgs) ok = false;
      /* `xhr.open`'s first argument is the METHOD and must be a literal, exactly as our baseline requires:
         without it the row's method is unknown and the site is not comparable. */
      if (ok && hit.arg0Method &&
          !(args[0] && ts.isStringLiteral(args[0]) && methods.has(args[0].text.toUpperCase()))) {
        ok = false; skippedNonMethod++;
      }
      /* A SPREAD ARGUMENT DOES NOT REFUSE THE SITE, IT MAKES ITS ADDRESS UNKNOWABLE — and those are two
         different facts. `fetch(...a)` is a request whose address no parse states, so our baseline counts the
         SITE with an opaque address; refusing it here dropped the site out of the partition entirely and made
         this walk read as having found less than it had. A spread cannot become a complete address by any
         route, so admitting it can only ever add an `opaque` row and can never manufacture a razor point. */
      if (ok) {
        const a = args[hit.urlArg];
        onSite(hit, (a && ts.isSpreadElement(a)) ? null : a, n, a && ts.isSpreadElement(a));
      }
    }
  };
  visit(sf);
  return { skippedNonMethod, globalRecvNonMethod };
}

/* ── THE SELFTEST, WHICH ARMS EVERY VERDICT BEFORE ANY CORPUS FIGURE IS BELIEVED ──────────────────────────
   CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE: a probe whose control has never produced a finding is reporting
   on its own probe. Each control below is a shape whose verdict this file CLAIMS, and the run asserts every
   verdict was actually produced at least once, plus an INVENTED negative that must read `opaque`. The
   cross-file control is the load-bearing one: it is the machinery the whole comparison rests on, and if the
   module graph is not resolving it must fail here rather than read as a corpus with nothing in it. */
function selftest(doors, globals, methods) {
  const A = `export const BASE = "/xfile/";\nexport const REG = "us";\n`;
  const B = [
    `import { BASE, REG } from "./a.js";`,
    `const LOCAL = "/local/";`,
    `const TPL = \`/tpl/\${REG}\`;`,
    `const PLUS = "/p" + "/q";`,
    `const U1 = "/u1/"; const U2 = "/u2/";`,
    `const UNION = true ? U1 : U2;`,
    `function viaReturn(c) { return c ? U1 : U2; }`,
    `fetch("/direct/");`,                  // literal
    `fetch(LOCAL);`,                       // folded, same file
    `fetch(BASE);`,                        // folded, CROSS FILE — the machinery ours lacks
    `fetch(TPL);`,                         // folded through a template literal
    `fetch(\`/hole/\${OPAQUE_HOLE}\`);`,   // opaque: a placeholder the checker cannot type is NOT a shape
    `fetch(INVENTED_NEGATIVE_CONTROL);`,   // opaque
    `fetch(PLUS);`,                        // ours folds this; TS does not — the partition, armed
    `fetch(UNION);`,                       // folded to TWO addresses at ONE site — ours has no analogue
    `fetch(viaReturn(1));`,                // folded through a FUNCTION RETURN, which ours does not cross
    `new Worker("/w.js");`,                // a second door, so the door derivation is exercised
    `x.open("GET", LOCAL);`,               // xhr.open with a real HTTP method
    `x.open(m, LOCAL);`,                   // refused: method not a literal
    `x.open("first", LOCAL);`,             // refused: a literal that is NOT an HTTP method — the measured
                                           //   false positive that produced this file's only `theirsOnly` row
  ].join("\n");
  const files = new Map([["/st/a.js", A], ["/st/b.js", B]]);
  const cache = new Map();
  const host = {
    getSourceFile: (n, lv) => {
      if (cache.has(n)) return cache.get(n);
      if (!files.has(n)) return undefined;
      const sf = ts.createSourceFile(n, files.get(n), lv, true, ts.ScriptKind.JS);
      cache.set(n, sf); return sf;
    },
    writeFile() {}, getDefaultLibFileName: () => "lib.d.ts", useCaseSensitiveFileNames: () => true,
    getCanonicalFileName: (x) => x, getCurrentDirectory: () => "/st", getNewLine: () => "\n",
    fileExists: (x) => files.has(x), readFile: (x) => files.get(x),
    directoryExists: () => true, getDirectories: () => [],
  };
  const prog = ts.createProgram(["/st/b.js", "/st/a.js"], TS_OPTS, host);
  const ck = prog.getTypeChecker();
  const got = [];
  const w = walkDoors(prog.getSourceFile("/st/b.js"), ck, doors, globals, methods, (door, arg) => {
    got.push({ door: door.id, ...tsVerdict(ck, arg) });
  });
  const seen = new Map();
  for (const g of got) seen.set(g.kind, (seen.get(g.kind) || 0) + 1);
  for (const k of ["literal", "folded", "opaque"])
    if (!seen.get(k)) die(`the selftest produced no \`${k}\` row, so that verdict is unarmed and a zero in ` +
                          `that column below would be a statement about this probe and not about the corpus. ` +
                          `Verdicts seen: ${JSON.stringify([...seen])}`);
  /* THE `shape` VERDICT IS ASSERTED ABSENT RATHER THAN ARMED, AND THE HEADER SAYS WHY AT LENGTH: measured,
     this checker answers a partially-known template with bare `string` and never with a template-literal
     type, so it has no shape verdict for a value expression. Two controls stand behind that here — a template
     over an untypable placeholder, and the binary `+` below — and if either ever produces a shape this fires
     and the header's claim is the thing to re-measure. */
  if (seen.get("shape")) die(`the selftest produced ${seen.get("shape")} \`shape\` row(s). This file's header ` +
                             `states, from a direct probe, that this checker has NO shape verdict for a value ` +
                             `expression — it recovers the whole address or no text at all. That measurement ` +
                             `is now false and the header is what to fix before any column below is read.`);
  const xfile = got.find((g) => g.text === "/xfile/");
  if (!xfile || xfile.kind !== "folded")
    die(`the CROSS-FILE control did not fold: an \`import { BASE } from "./a.js"\` whose declaration is ` +
        `\`const BASE = "/xfile/"\` read back as ${JSON.stringify(xfile)}. The module graph is the machinery ` +
        `this whole comparison rests on, so a corpus run now would report the adversary as weak when what is ` +
        `broken is the host wiring.`);
  const tpl = got.find((g) => g.text === "/tpl/us");
  if (!tpl || tpl.kind !== "folded")
    die(`the TEMPLATE control did not fold to "/tpl/us" (${JSON.stringify(tpl)}).`);
  const neg = got.filter((g) => g.kind === "opaque");
  if (neg.length < 2) die(`the INVENTED negative control and the untypable-placeholder template did not both ` +
                          `read opaque (${neg.length} opaque row(s)).`);
  /* THE UNION CONTROL, WHICH IS THE ONE CAPABILITY THIS ADVERSARY HAS THAT OURS HAS NO ANALOGUE OF: one site,
     two complete addresses. It is armed twice — through a conditional binding and through a FUNCTION RETURN,
     which is the interprocedural half. */
  const unions = got.filter((g) => Array.isArray(g.texts));
  if (unions.length !== 2)
    die(`the UNION controls did not both fold: expected two sites typing as a union of string literals (one ` +
        `through \`c ? U1 : U2\` and one through a function return) and got ${unions.length}. ` +
        `${JSON.stringify(unions)}`);
  for (const u of unions)
    if (!(u.texts.includes("/u1/") && u.texts.includes("/u2/")))
      die(`a UNION control folded to ${JSON.stringify(u.texts)} rather than both members.`);
  /* THE PARTITION, ARMED IN THE DIRECTION THAT DOES NOT FLATTER THIS FILE: `"/p" + "/q"` is a shape our own
     baseline FOLDS and this adversary does not, and the run asserts that asymmetry is live rather than
     asserted in prose. If TypeScript ever starts folding `+`, this fires and the header's partition claim has
     to be re-measured. */
  const plus = got.filter((g) => g.text === "/p/q");
  if (plus.length) die(`the adversary folded \`"/p" + "/q"\` to ${JSON.stringify(plus)}. This file's header ` +
                       `states the two tools PARTITION rather than order, on the measurement that ours folds ` +
                       `binary \`+\` and this one does not. That measurement is now false and the header is ` +
                       `the thing to fix before any column below is read.`);
  const workers = got.filter((g) => g.door === "new Worker");
  if (workers.length !== 1) die(`the door derivation did not reach \`new Worker\` exactly once (${workers.length}).`);
  const opens = got.filter((g) => g.door === "xhr.open");
  if (opens.length !== 1) die(`\`xhr.open\`'s method refusal is not firing: ${opens.length} row(s) admitted ` +
                              `where exactly one of the three \`.open(\` calls has a real HTTP method.`);
  if (w.skippedNonMethod !== 2)
    die(`the \`arg0Method\` refusal counted ${w.skippedNonMethod} skip(s) where the controls are two — a ` +
        `non-literal method and the literal \`"first"\` that is not an HTTP method. That second one is the ` +
        `measured false positive this file was caught by, so its control may not go unarmed.`);
  return { rows: got.length, kinds: [...seen], skippedNonMethod: w.skippedNonMethod };
}

const TS_OPTS = {
  allowJs: true, checkJs: false, noLib: true, noResolve: false,
  module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ESNext,
  moduleResolution: ts.ModuleResolutionKind.Bundler,
};

/* ── OUR OWN SIDE, TAKEN FROM THE INSTRUMENT THAT OWNS IT ─────────────────────────────────────────────────
   `static_surface.mjs` is RUN rather than re-implemented, and its published `total.sites` is asserted against
   the rows it hands back: `--examples N` slices per site, so a value too small silently returns a SUBSET, and
   a subset makes the adversary look stronger than it is. That is the flattering direction, so it throws. */
function oursRows(corpusDir, cached) {
  let out;
  if (cached) out = JSON.parse(readFileSync(cached, "utf8"));
  else {
    const r = spawnSync(process.execPath, ["--max-old-space-size=6000", STATIC_SURFACE,
      "--corpus", corpusDir, "--json", "--examples", "100000000"],
      { cwd: ROOT, encoding: "utf8", maxBuffer: 1 << 30 });
    if (r.status !== 0)
      die(`static_surface.mjs exited ${r.status}. Its own refusal is the finding, not this file's:\n${r.stderr}`);
    out = JSON.parse(r.stdout);
  }
  if (!out.total || !Array.isArray(out.examples))
    die(`static_surface.mjs produced no \`total\`/\`examples\`; its JSON shape changed.`);
  if (out.examples.length !== out.total.sites)
    die(`static_surface.mjs published total.sites=${out.total.sites} and handed back ` +
        `${out.examples.length} row(s). The OURS column is a SUBSET, which makes every set difference below ` +
        `read as the adversary winning — the flattering direction. Raise \`--examples\`.`);
  return out;
}

function main(argv) {
  const arg = (k, d) => { const i = argv.indexOf(k); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };
  const corpusDir = resolve(arg("--corpus", "engine/.work/sitecorpus/mirror"));
  const cached = arg("--ours", null);
  const wantJson = argv.includes("--json");
  const nExamples = parseInt(arg("--examples", "0"), 10) || 0;

  const ssSrc = readFileSync(STATIC_SURFACE, "utf8");
  const doors = doorsFromSource(ssSrc, relative(ROOT, STATIC_SURFACE));
  const globals = globalObjectsFromSource(ssSrc, relative(ROOT, STATIC_SURFACE));
  const methods = httpMethodTestFromSource(ssSrc, relative(ROOT, STATIC_SURFACE));
  const st = selftest(doors, globals, methods);

  let stat = null;
  try { stat = statSync(corpusDir); } catch { /* handled below */ }
  if (!stat || !stat.isDirectory())
    die(`no corpus at ${corpusDir}. THIS REPOSITORY CARRIES NO COPY OF ANYBODY ELSE'S SITE — the driver is ` +
        `tracked and the corpus is not (testing/corpus/README.md), so there is nothing to fall back on and ` +
        `this is not a defect. Make one, then re-run:\n` +
        `    NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs --out ${dirname(corpusDir)}\n` +
        `    node testing/ts_surface.mjs --corpus ${corpusDir}`);

  /* CALIBRATION 1: the population, through the function that publishes it. */
  const cp = corpusPrograms(corpusDir, TAG);
  /* CALIBRATION 2: our own side's published total against the rows it hands back. */
  const ours = oursRows(corpusDir, cached);

  /* SITE ATTRIBUTION BY CONTENT, exactly as `static_surface.mjs` does it: the manifest's row carries the site
     id and the digest carries the file, and a path rule here would be the second copy
     `engine/corpus_programs.mjs` refuses by name. */
  const manifest = JSON.parse(readFileSync(resolve(corpusDir, "..", "provenance.json"), "utf8"));
  const rows = Array.isArray(manifest) ? manifest : Object.values(manifest);
  const siteOf = new Map(), essOf = new Map();
  let fetchedFrom = null, fetchedTo = null;
  const note = (sha, id, ct, at) => {
    if (!sha) return;
    if (!siteOf.has(sha)) siteOf.set(sha, new Set());
    siteOf.get(sha).add(id);
    essOf.set(sha, essenceOf(ct));
    if (at) { if (!fetchedFrom || at < fetchedFrom) fetchedFrom = at; if (!fetchedTo || at > fetchedTo) fetchedTo = at; }
  };
  for (const r of rows) {
    note(r.sha256, r.id, r.contentType, r.fetchedAt);
    for (const s of r.resources || []) note(s.sha256, r.id, s.contentType, s.fetchedAt);
  }
  const sha = (b) => createHash("sha256").update(b).digest("hex");

  /* GROUP THE CORPUS'S PROGRAM FILES BY SITE. One TypeScript Program per site, so the module graph is the
     site's own and a specifier cannot resolve across two unrelated origins. */
  const bySite = new Map();
  let nProgramSeen = 0, nDocumentSeen = 0;
  for (const f of cp.files) {
    const buf = readFileSync(f);
    const ess = essOf.get(sha(buf));
    if (DOCUMENT.has(ess)) { nDocumentSeen++; continue; }
    if (!PROGRAM.has(ess)) die(`${relative(corpusDir, f)} is neither program nor document by the manifest.`);
    nProgramSeen++;
    const ids = siteOf.get(sha(buf));
    if (!ids || !ids.size) die(`no manifest row names the site of ${relative(corpusDir, f)}`);
    const id = [...ids].sort()[0];
    if (!bySite.has(id)) bySite.set(id, []);
    bySite.get(id).push(f);
  }
  if (nProgramSeen !== cp.nProgram)
    die(`this probe typed ${nProgramSeen} program(s) where corpusPrograms published ${cp.nProgram}. The two ` +
        `selectors disagree about the population, which is the finding — read nothing below this line.`);
  if (nDocumentSeen !== cp.nDocument)
    die(`this probe typed ${nDocumentSeen} document(s) where corpusPrograms published ${cp.nDocument}.`);

  /* OUR SIDE, PER SITE: the complete address strings our baseline states from the text. A row whose kind is
     `literal` or `folded` carries no hole by that instrument's own construction, and this asserts it rather
     than trusting it — a hole leaking into this set would make an incomplete address look complete on OUR
     side, which is the direction that hides a finding. */
  const oursComplete = new Map(), oursSites = new Map(), oursKind = new Map();
  const bucketSet = (m, id) => { if (!m.has(id)) m.set(id, new Set()); return m.get(id); };
  for (const r of ours.examples) {
    const id = r.file.split("/")[0];
    oursSites.set(id, (oursSites.get(id) || 0) + 1);
    if (!oursKind.has(id)) oursKind.set(id, { literal: 0, folded: 0, shape: 0, opaque: 0 });
    oursKind.get(id)[r.kind]++;
    if (r.kind === "literal" || r.kind === "folded") {
      if (r.holes !== 0) die(`static_surface row at ${r.file}:${r.line} is \`${r.kind}\` with ${r.holes} ` +
                             `hole(s); the complete-from-text set would carry an incomplete address.`);
      bucketSet(oursComplete, id).add(r.url);
    }
  }

  /* THE ADVERSARY'S SIDE, PER SITE. */
  const per = [];
  const t0 = Date.now();
  let xfileResolved = 0, xfileUnresolved = 0;
  for (const [id, files] of [...bySite.entries()].sort()) {
    /* THE CORPUS IS SAVED UNDER THE SERVER'S OWN PATHS AND `ts.createProgram` DROPS A ROOT WHOSE EXTENSION IT
       DOES NOT RECOGNISE, SO A FILE IS PRESENTED TO TYPESCRIPT UNDER A NAME IT ACCEPTS.
       THIS WAS A MEASURED DEFECT OF THIS FILE AND IT WAS IN THE DIRECTION THAT FLATTERS THIS PROJECT. The
       first two runs of this comparison counted such a file as a `parseFail` and walked on, so SIX genuine
       shipped bundles were invisible to the adversary while its own site total read as merely a little short
       of ours — and every address in them would have been credited to execution unopposed. The shape is not
       exotic and is the exact one `engine/corpus_programs.mjs`'s whole header is about: a bundle fetched with
       a query is saved as `ol-components.js_v_a3c7cc6d...`, whose extension is in no list anybody would write.
       That file's answer was to type a corpus file by the SERVER'S `Content-Type` rather than by its name, and
       this is the same answer one layer on — the manifest says these bytes are a program, so TypeScript is
       handed them under a name that lets it agree.
       APPENDING RATHER THAN REPLACING is deliberate: `X.js_v_abc` becomes `X.js_v_abc.js`, so a specifier
       elsewhere reading `./X.js_v_abc` now RESOLVES where before it could not, and no basename another file
       already imports is changed. Every root is then ASSERTED to be in the program, so this can never again
       fail quietly. */
    const tsPathOf = (f) => (/\.(js|mjs|cjs|jsx)$/i.test(f) ? f : f + ".js");
    const txt = new Map(), backFrom = new Map();
    for (const f of files) { const t = tsPathOf(f); txt.set(t, readFileSync(f, "utf8")); backFrom.set(t, f); }
    const cache = new Map();
    const host = {
      getSourceFile: (n, lv) => {
        if (cache.has(n)) return cache.get(n);
        if (!txt.has(n)) return undefined;
        let sf = null;
        try { sf = ts.createSourceFile(n, txt.get(n), lv, true, ts.ScriptKind.JS); } catch { sf = null; }
        cache.set(n, sf); return sf || undefined;
      },
      writeFile() {}, getDefaultLibFileName: () => "lib.d.ts", useCaseSensitiveFileNames: () => true,
      getCanonicalFileName: (x) => x, getCurrentDirectory: () => corpusDir, getNewLine: () => "\n",
      fileExists: (x) => txt.has(x), readFile: (x) => txt.get(x),
      directoryExists: () => true, getDirectories: () => [],
    };
    let prog, ck;
    const roots = files.map(tsPathOf);
    try { prog = ts.createProgram(roots, TS_OPTS, host); ck = prog.getTypeChecker(); }
    catch (e) { per.push({ site: id, programs: files.length, failed: String(e.message).slice(0, 200) }); continue; }

    const b = { site: id, programs: files.length, sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0,
                absent: 0, complete: new Set(), rows: [], parseFail: 0, byDoor: {}, skippedNonMethod: 0,
                lit: { string: 0, noSubTemplate: 0 }, globalRecvNonMethod: 0,
                /* HOW MANY OF THIS SITE'S BUNDLES TYPESCRIPT WOULD HAVE REFUSED BY EXTENSION. It is a
                   number on every run because it was a silent six once. */
                renamedForTs: files.filter((f) => tsPathOf(f) !== f).length };
    /* THE MODULE-GRAPH WITNESS, per site, so a site whose specifiers do not resolve cannot read as a site
       with nothing to find. It is the machinery the whole comparison rests on and its reach is a NUMBER
       rather than a claim. */
    for (const f of files) {
      const sf = prog.getSourceFile(tsPathOf(f));
      /* A ROOT FILE THE PROGRAM DOES NOT HOLD IS A SILENTLY SMALLER ADVERSARY, WHICH IS THE FLATTERING
         DIRECTION, SO IT THROWS RATHER THAN BEING COUNTED. `ts.createProgram` drops a root whose extension it
         does not recognise, and this corpus is saved under the SERVER'S paths — a bundle fetched with a query
         is saved as `all.js__q54b3907e`, whose extension is in no list. `engine/corpus_programs.mjs`'s whole
         header is about that exact filename, so a file the manifest types as a program and TypeScript refuses
         by extension is a real and expected shape here; what may not happen is for it to vanish into a
         `parseFail` tally nobody reads. */
      if (!sf) die(`the ${id} program does not hold its own root ${relative(corpusDir, f)}, presented to ` +
                   `TypeScript as ${relative(corpusDir, tsPathOf(f))}. A root the program drops is a genuine ` +
                   `shipped bundle the adversary never reads, which makes it quietly narrower — the direction ` +
                   `that manufactures a razor point for this project. Widen \`tsPathOf\` rather than counting ` +
                   `this as a parse failure.`);
      if (sf.parseDiagnostics && sf.parseDiagnostics.length) b.parseFail++;
      /* ITERATIVE for the reason `walkDoors` is: a recursive walk's depth is bounded by the corpus's own
         expression nesting and this corpus is minified. */
      const work = [sf];
      while (work.length) {
        const n = work.pop();
        if (ts.isImportDeclaration(n) && n.importClause && n.importClause.namedBindings &&
            ts.isNamedImports(n.importClause.namedBindings)) {
          for (const el of n.importClause.namedBindings.elements) {
            let sym = null;
            try { sym = ck.getSymbolAtLocation(el.name); } catch { /* counted as unresolved */ }
            let al = sym;
            try { if (sym && (sym.flags & ts.SymbolFlags.Alias)) al = ck.getAliasedSymbol(sym); } catch { al = null; }
            const d = al && al.declarations && al.declarations[0];
            if (d && d.getSourceFile().fileName !== sf.fileName) xfileResolved++; else xfileUnresolved++;
          }
        }
        ts.forEachChild(n, (c) => { work.push(c); });
      }
      const w = walkDoors(sf, ck, doors, globals, methods, (door, a, node, spread) => {
        /* A SPREAD READS `opaque` AND NEVER `absent`: `absent` means the door was called with no argument at
           that position at all, which is a different fact and must stay its own row. */
        const v = spread ? { kind: "opaque", text: null } : tsVerdict(ck, a);
        b.sites++;
        b.byDoor[door.id] = (b.byDoor[door.id] || 0) + 1;
        b[v.kind]++;
        if (v.lit) b.lit[v.lit]++;
        const texts = v.texts || (v.text !== null && (v.kind === "literal" || v.kind === "folded") ? [v.text] : []);
        for (const s of texts) b.complete.add(s);
        if (nExamples && v.kind === "folded")
          b.rows.push({ door: door.id, cls: door.cls, kind: v.kind, url: v.text, urls: v.texts || undefined,
                        file: relative(corpusDir, f), tsPath: tsPathOf(f) === f ? undefined : "renamed",
                        line: sf.getLineAndCharacterOfPosition(node.getStart()).line + 1 });
      });
      b.skippedNonMethod += w.skippedNonMethod;
      b.globalRecvNonMethod += w.globalRecvNonMethod;
    }
    /* THE VERDICT PARTITION IS ASSERTED, so no row can fall out of all five and make a site read as having
       found less than it did. */
    if (b.literal + b.folded + b.shape + b.opaque + b.absent !== b.sites)
      die(`the ${id} verdict partition does not sum against ${b.sites}.`);
    if (b.lit.string + b.lit.noSubTemplate !== b.literal)
      die(`the ${id} literal partition does not sum against ${b.literal}: a row falling out of both arms ` +
          `would hide the labelling difference that is the whole reason the two \`literal\` columns differ.`);
    per.push(b);
  }
  const ms = Date.now() - t0;

  /* ── THE TWO SET DIFFERENCES, WHICH ARE THE WHOLE OUTPUT ────────────────────────────────────────────── */
  const theirsOnly = [], oursOnly = [];
  for (const b of per) {
    if (b.failed) continue;
    const A = oursComplete.get(b.site) || new Set();
    for (const s of b.complete) if (!A.has(s)) theirsOnly.push({ site: b.site, url: s });
    for (const s of A) if (!b.complete.has(s)) oursOnly.push({ site: b.site, url: s });
  }

  const tot = { sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, absent: 0, programs: 0, parseFail: 0,
                skippedNonMethod: 0, renamedForTs: 0, globalRecvNonMethod: 0 };
  const totLit = { string: 0, noSubTemplate: 0 };
  for (const b of per) { if (b.failed) continue;
    for (const k of Object.keys(tot)) if (typeof b[k] === "number") tot[k] += b[k];
    totLit.string += b.lit.string; totLit.noSubTemplate += b.lit.noSubTemplate; }

  /* THE `arg0Method` REFUSAL IS CROSS-CHECKED AGAINST THE COUNT OUR BASELINE ALREADY PUBLISHES. Both sides
     apply one derived test to one corpus, so the two refusal counts are the SAME QUANTITY and a disagreement
     is a difference in precision between the two walks — which would put a string that is not an address on
     one side of the comparison. It is a WARNING and not a throw because the two walks legitimately differ in
     door RECALL (their site totals differ), and a refusal count is downstream of recall: a site one walk never
     reached cannot be refused by it. */
  const methodRefusalAgrees = tot.skippedNonMethod + tot.globalRecvNonMethod === ours.total.xhrOpenSkipped;

  const out = {
    tool: { name: "typescript", version: ts.version },
    corpus: { dir: relative(ROOT, corpusDir), programs: cp.nProgram, documents: cp.nDocument,
              bytes: cp.bytes, fetchedFrom, fetchedTo },
    doors: doors.map((d) => d.id),
    selftest: st,
    moduleGraph: { namedImportBindingsResolvedCrossFile: xfileResolved, unresolved: xfileUnresolved },
    theirs: { rowUnit: "one syntactic call site", ...tot, literalBy: totLit },
    methodRefusal: { theirs: tot.skippedNonMethod, globalReceiverCandidates: tot.globalRecvNonMethod,
                     sum: tot.skippedNonMethod + tot.globalRecvNonMethod, ours: ours.total.xhrOpenSkipped,
                     agrees: methodRefusalAgrees },
    ours: { rowUnit: "one syntactic call site", sites: ours.total.sites, literal: ours.total.literal,
            folded: ours.total.folded, shape: ours.total.shape, opaque: ours.total.opaque },
    addresses: {
      rowUnit: "one distinct address string, per site",
      ours: [...oursComplete.values()].reduce((n, s) => n + s.size, 0),
      theirs: per.filter((b) => !b.failed).reduce((n, b) => n + b.complete.size, 0),
      theirsOnly: theirsOnly.length, oursOnly: oursOnly.length,
    },
    theirsOnlyAddresses: nExamples ? theirsOnly.slice(0, nExamples) : undefined,
    oursOnlyAddresses: nExamples ? oursOnly.slice(0, nExamples) : undefined,
    foldedExamples: nExamples ? per.flatMap((b) => (b.rows || []).slice(0, nExamples)) : undefined,
    perSite: per.map((b) => ({ ...b, complete: b.complete ? b.complete.size : 0, rows: undefined,
      oursSites: oursSites.get(b.site) || 0, oursComplete: (oursComplete.get(b.site) || new Set()).size,
      oursKind: oursKind.get(b.site) || null })),
  };
  if (wantJson) { console.log(JSON.stringify(out, null, 1)); return; }

  const pct = (n, d) => (d ? (100 * n / d).toFixed(1) : "0.0") + "%";
  console.log(`# ts_surface — AN ADVERSARY THIS PROJECT DID NOT WRITE, ASKED THE SAME QUESTION.`);
  console.log(`adversary: typescript ${ts.version} — binder + module graph + checker literal inference.`);
  console.log(`selftest ARMED: ${st.rows} control row(s), verdicts ${JSON.stringify(st.kinds)}; the cross-file,`);
  console.log(`  template, invented-negative and ours-folds-\`+\` controls all fired.`);
  console.log(`doors DERIVED from testing/static_surface.mjs: ${doors.length} — ${out.doors.join(" ")}`);
  console.log(`corpus ${out.corpus.dir}: ${cp.nProgram} program(s), ${cp.nDocument} document(s), ` +
              `${(cp.bytes / 1048576).toFixed(1)} MiB, fetched ${fetchedFrom} .. ${fetchedTo}`);
  console.log(`module graph: ${xfileResolved} named import binding(s) resolved CROSS-FILE, ${xfileUnresolved} not ` +
              `— the machinery our per-file baseline has none of.`);
  console.log(`walked in ${ms} ms — ${tot.renamedForTs} bundle(s) presented to TypeScript under an appended ` +
              `\`.js\` because it refuses a root whose extension it does not know; every root is asserted to ` +
              `be in its program, so none can go unread.`);
  console.log(`\`arg0Method\` refusals — ${tot.skippedNonMethod} here + ${tot.globalRecvNonMethod} that our ` +
              `baseline reaches through a global-object receiver = ${tot.skippedNonMethod + tot.globalRecvNonMethod}, ` +
              `against its published ${ours.total.xhrOpenSkipped}` +
              `${methodRefusalAgrees ? " — AGREE EXACTLY (one derived HTTP-method test, one corpus)"
                                     : " — UNRECONCILED, and that is the finding rather than anything below it"}`);
  console.log(``);
  console.log(`SITE COUNTS — ROW UNIT: ONE SYNTACTIC CALL SITE. A PARTITION AND NEVER A DIFFERENCE: two`);
  console.log(`selectors over one corpus, so a gap is door RECALL and says nothing about value recovery.`);
  console.log(`  ours   sites ${String(out.ours.sites).padStart(6)}  literal ${String(out.ours.literal).padStart(5)}` +
              `  folded ${String(out.ours.folded).padStart(5)}  shape ${String(out.ours.shape).padStart(4)}` +
              `  opaque ${String(out.ours.opaque).padStart(5)}`);
  console.log(`  theirs sites ${String(tot.sites).padStart(6)}  literal ${String(tot.literal).padStart(5)}` +
              `  folded ${String(tot.folded).padStart(5)}  shape ${String(tot.shape).padStart(4)}` +
              `  opaque ${String(tot.opaque).padStart(5)}  absent ${tot.absent}`);
  /* THE STRONGEST CALIBRATION THIS COMPARISON HAS, AND IT IS FREE. Two door walks written independently —
     one over `@babel/parser`, one over TypeScript's AST — over one corpus. Where their SITE totals and their
     DIRECT-STRING-LITERAL counts agree to the digit, the two selectors have been shown to walk one population,
     which is what makes every breakdown below worth reading; where they do not, the gap is door RECALL and is
     the first thing to explain. It is printed rather than thrown because a future corpus may legitimately
     divide them, and a reader needs the number either way. */
  const dSites = tot.sites - ours.total.sites, dLit = totLit.string - ours.total.literal;
  console.log(`  RECONCILIATION: site totals ${dSites === 0 ? "AGREE EXACTLY" : `differ by ${dSites}`}, ` +
              `direct-string-literal counts ${dLit === 0 ? "AGREE EXACTLY" : `differ by ${dLit}`} ` +
              `(${totLit.string} vs ${ours.total.literal}) — two selectors, two parsers, one corpus.`);
  console.log(`  THE TWO \`literal\` COLUMNS ARE NOT COMPARABLE LABEL-FOR-LABEL and their gap is a CONVENTION:`);
  console.log(`  a no-substitution template is \`literal\` here and \`folded\` there. theirs literal splits ` +
              `${totLit.string} string + ${totLit.noSubTemplate} no-sub-template;`);
  console.log(`  ours splits 606-style StringLiteral + TemplateLiteral inside \`folded\`. Only literal+folded ` +
              `is one quantity, which is what`);
  console.log(`  the address sets below are built from: ours ${ours.total.literal + ours.total.folded} ` +
              `site(s), theirs ${tot.literal + tot.folded}.`);
  console.log(``);
  console.log(`ADDRESSES — ROW UNIT: ONE DISTINCT ADDRESS STRING PER SITE. Identical units, so this IS a`);
  console.log(`difference. THIS IS THE ANSWER.`);
  console.log(`  complete-from-text, ours:   ${out.addresses.ours}`);
  console.log(`  complete-from-text, theirs: ${out.addresses.theirs}`);
  console.log(`  >>> THEIRS AND NOT OURS: ${out.addresses.theirsOnly}  ` +
              `(${pct(out.addresses.theirsOnly, out.addresses.theirs)} of theirs) — every one of these is an ` +
              `address this project is crediting to execution that a third-party parse states from the text.`);
  console.log(`      OURS AND NOT THEIRS: ${out.addresses.oursOnly}  ` +
              `(${pct(out.addresses.oursOnly, out.addresses.ours)} of ours) — the other half of the partition; ` +
              `one adversary agreeing is NOT "no parse can state this".`);
  console.log(``);
  console.log(`PER SITE  ${"site".padEnd(14)} ${"progs".padStart(5)} ${"ourSt".padStart(5)} ${"thrSt".padStart(5)} ` +
              `${"ourAd".padStart(5)} ${"thrAd".padStart(5)} ${"only+".padStart(5)} ${"only-".padStart(5)} ${"fold".padStart(4)}`);
  for (const b of out.perSite.sort((x, y) => (y.sites || 0) - (x.sites || 0))) {
    if (b.failed) { console.log(`  ${b.site.padEnd(14)} FAILED: ${b.failed}`); continue; }
    const to = theirsOnly.filter((x) => x.site === b.site).length;
    const oo = oursOnly.filter((x) => x.site === b.site).length;
    console.log(`  ${b.site.padEnd(14)} ${String(b.programs).padStart(5)} ${String(b.oursSites).padStart(5)} ` +
                `${String(b.sites).padStart(5)} ${String(b.oursComplete).padStart(5)} ${String(b.complete).padStart(5)} ` +
                `${String(to).padStart(5)} ${String(oo).padStart(5)} ${String(b.folded).padStart(4)}`);
  }
  if (nExamples) {
    console.log(``);
    console.log(`THEIRS AND NOT OURS — the deliverable, first ${nExamples}:`);
    for (const r of theirsOnly.slice(0, nExamples)) console.log(`  ${r.site.padEnd(14)} ${r.url.slice(0, 150)}`);
    console.log(``);
    console.log(`OURS AND NOT THEIRS — first ${nExamples}:`);
    for (const r of oursOnly.slice(0, nExamples)) console.log(`  ${r.site.padEnd(14)} ${r.url.slice(0, 150)}`);
    console.log(``);
    console.log(`THE ADVERSARY'S \`folded\` ROWS — a value its CHECKER recovered from a non-literal expression:`);
    for (const r of out.foldedExamples.slice(0, nExamples))
      console.log(`  [${r.door}] ${JSON.stringify(r.urls || r.url).slice(0, 110)}   ${r.file}:${r.line}`);
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url))
  main(process.argv.slice(2));
