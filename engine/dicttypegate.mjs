/* THE DICTIONARY-MEMBER TYPE AXIS — the fifth question over the platform's Web IDL surface, and the one that
 * let a page observe a getter its browser never reaches.
 *
 *   node engine/dicttypegate.mjs            audit every IdlDictMember row this engine declares
 *   node engine/dicttypegate.mjs --all      print every row of every band rather than the first 200
 *   node engine/dicttypegate.mjs --confirm  also print what it CONFIRMED
 *
 * WHY A FIFTH, AND WHAT THE OTHER FOUR ANSWER. Each instrument here partitions, and each is blind exactly
 * where the others look. idlgen.mjs asks about NAMES — which members a spec lists that no component installs,
 * and, for a dictionary, which member names no `IdlDictMember` declaration carries. argaudit.mjs asks whether
 * a BODY re-converts an argument whose declared type crosses concolically. argtypegate.mjs asks whether an
 * ARGUMENT POSITION's declared type is the type the IDL gives it. rettypegate.mjs asks the same of a RETURN
 * type. A DICTIONARY MEMBER's declared type is none of those, and a member named correctly at the wrong type
 * reads COMPLETE to every one of them.
 *
 * THE DEFECT THAT NAMES THIS FILE. `MouseEventInit.relatedTarget` and `UIEventInit.view` were declared IDL_ANY
 * and brand-checked inside the member bodies. Web IDL §3.2.17 Dictionary types converts the members in a fixed
 * order, each with `? Get(jsDict, key)` followed by that member's own conversion, so a TypeError owed at
 * `relatedTarget` is owed BEFORE `screenX` is read at all — and a page writing
 * `new MouseEvent("m", {relatedTarget: 42, get screenX(){ … }})` RAN its getter here where a browser throws
 * first. Nothing crashed; the engine simply told the page a different story about its own code. Every gate in
 * this tree was green, because the member was NAMED, its ARITY was irrelevant, no ARGUMENT was involved and
 * nothing RETURNED. The axis with no instrument is where the defect was, which is the standing lesson: when a
 * defect escapes every instrument, the question is which axis has no instrument yet.
 *
 * WHY WPT'S idlharness CANNOT ANSWER IT — the first question to ask before building an auditor, because an
 * oracle you HAVE beats one you build. It cannot, and it is not close. `resources/idlharness.js` handles a
 * dictionary in one arm whose entire body is a comment:
 *
 *     case "dictionary":
 *         // Nothing to test, but we need the dictionary info around for type checks
 *         this.members[parsed_idl.name] = new IdlDictionary(parsed_idl);
 *
 * `IdlDictionary.prototype` is `Object.create(IdlObject.prototype)` and adds only
 * `get_reverse_inheritance_stack`, so `IdlArray.test()`'s `this.members[name].test()` reaches
 * `IdlObject.prototype.test`, whose own comment reads, in that file and not in any standard:
 *
 *     By default, this does nothing, so no actual tests are run for IdlObjects
 *     that don't define any (e.g. IdlDictionary at the time of this writing).
 *
 * The two places that touch a dictionary's member types touch them for something else: `is_json_type` walks
 * them to decide
 * whether an INTERFACE's `toJSON` returns a JSON type, and `assert_type_is`'s dictionary arm is the single
 * line `// TODO: Test when we actually have something to test this on`. So the corpus asserts NOTHING about a
 * dictionary member — not its existence, not its type, not `required`, not its default, and not §3.2.17's read
 * order. A green WPT run is not a weak signal on this axis; it is no signal at all.
 *
 * THE RULE IS DERIVED, NOT TABULATED. A table of member names, or of "the type this member should be", would
 * be the second copy that drifts — precisely the fact this audit exists to catch, restated by the auditor. So
 * every leg is read from whatever already owns it:
 *
 *   - THE ENGINE'S SIDE is read by engine/idl_dictdecl.mjs, the ONE reader of `IdlDictMember` initialiser
 *     lists, which idlgen.mjs also uses for its membership question. A member's declared `IdlArgType` is field
 *     1 of that struct.
 *   - THE SPEC'S SIDE is idl_members.mjs's `dictMembers`, which flattens §2.7's inheritance chain into
 *     §3.2.17's read order and carries each member's `idlType`, its `required` flag, whether the IDL writes a
 *     default, and the field's own extended attributes.
 *   - THE CORRESPONDENCE is engine/idl_typename.mjs, the enumerator-naming convention core/idl_args.h owns,
 *     shared with argtypegate.mjs because §3.2's conversions are per TYPE and know nothing about whether the
 *     type was written at an argument or at a member.
 *   - WHETHER A COMPOSED NAME EXISTS is asked of the `IdlArgType` enum itself, read out of the header by
 *     idl_argdecl.mjs's `contract`.
 *
 * THE ONE THING THE CONVENTION SPELLS HERE AND NOT AT AN ARGUMENT, and it is derived rather than excepted.
 * §3.2.17 step 4.1.5 gives a member with a declared default a THIRD state beside present and absent: it EXISTS
 * on the converted dictionary carrying that value. A member with no default has only two. core/idl_args.h
 * spells that difference as a suffix — `IDL_BOOLEAN_NO_DEFAULT` beside `IDL_BOOLEAN`, and its own declaration
 * says why: "ToBoolean(undefined) is false, so IDL_BOOLEAN folds the two together", which for
 * `MutationObserverInit` is the difference between `observe(t,{attributeFilter:[]})` succeeding and
 * `observe(t,{attributes:false,attributeFilter:[]})` being a TypeError. So the composition APPENDS `_NO_DEFAULT`
 * where the IDL writes no default and ASKS THE ENUM whether such an enumerator exists — the same move the stem
 * itself is composed by. Nothing here lists which types have the suffix: the day `IDL_DOMSTRING_NO_DEFAULT` is
 * declared, this audit requires it at every defaultless DOMString member without being edited.
 *
 * AND A REQUIRED MEMBER TAKES EITHER SPELLING, which is a spec fact and not an allowance. §3.2.17 step 4.1.6
 * throws a TypeError when a required member is absent, so a required member HAS no absent state for the suffix
 * to be about, and the two enumerators name ONE conversion. The engine's own member loop performs the steps in
 * that order — the required throw stands above the absent-member rewrite — so the pair is unobservable there
 * too. The prediction is therefore the suffixed name for every defaultless member, required or not, and the
 * PLAIN stem is carried beside it as the second acceptable spelling for a required one; a row matching either
 * is confirmed, and the second is counted under its own name so the allowance is visible rather than folded
 * into the confirmed total. IT WAS MEASURED BEFORE IT WAS WRITTEN. A first cut predicted the plain stem for a
 * required member instead, and it put `IntersectionObserverEntryInit`'s `isIntersecting` and `isVisible` —
 * both `required boolean`, both declared IDL_BOOLEAN_NO_DEFAULT — into the accusing band, two rows out of
 * eight that a reader could have done nothing with. A gate that spends a quarter of its accusations on a
 * distinction no page can observe is a gate that gets muted.
 *
 * WHICH DICTIONARY A DECLARATION IS OF, AND WHAT IS BELIEVED. An `IdlDictDecl` NAMES its dictionary, so those
 * rows are compared against that dictionary and nothing else. A bare `IdlDictMember` array names nothing, and
 * this audit does not guess: it takes the one thing TRUE of every candidate — an array can only declare a
 * dictionary that has all of its members — and judges a row only where EVERY candidate predicts the SAME
 * enumerator. Where they disagree the row is AMBIGUOUS and judged against nothing. That is sound without an
 * identity, which is why this axis did not have to wait for idlgen.mjs's named residual to be closed; when it
 * is, the ambiguous band shrinks and nothing else about this file changes.
 *
 * ITS BLIND SPOTS ARE PART OF THE INSTRUCTION, because a checker trusted past its evidence is worse than none:
 *
 *   - IT JUDGES THE DECLARED TYPE AND NOT THE BODY. A member declared correctly whose body then mis-reads the
 *     converted value is invisible here, exactly as argtypegate is blind to a body and argaudit is blind to a
 *     declaration.
 *   - IT DOES NOT COMPARE DECLARED DEFAULT *VALUES*. `IdlDictMember::dflt` states §3.2.17 step 4.1.5's value
 *     and the IDL states it again with `= …`; whether the engine spells `= 1` as IDL_DEFAULT_ONE is a question
 *     about the VALUE, and this audit asks only whether a default EXISTS, through the suffix above. The value
 *     axis is the next one, not a gap in this one.
 *   - IT DOES NOT CHECK §3.2.17's READ ORDER OR `required`. idlgen.mjs asks both, for the dictionaries it can
 *     identify; this file deliberately does not restate them.
 *   - A UNION, A SEQUENCE AND A GENERIC ARE COUNTED AND NEVER JUDGED. §3.2.25's enumerators are per-member
 *     compounds (IDL_SEQUENCE_STRING_OR_DICT, IDL_BODYINIT_NULLABLE) that no convention composes.
 *   - IT READS THE CORPUS `@webref/idl` SHIPS. Where that carries a different edition from the engine's, the
 *     row is a finding about the pair and not about the engine alone — and this is not hypothetical here:
 *     `CSSOMString` is USED by CSSOM's own IDL and DEFINED by no declaration in the corpus at all.
 *
 * IT IS NOT A BUILD GATE, for citegen.mjs's and argtypegate.mjs's reason: it reads source text joined to a
 * corpus that moves under it, so a checker that fails a lane for a finding the lane did not introduce gets
 * muted exactly as fast as one that cries wolf. It prints; the human decides. It exits 0.
 */
import { fileURLToPath } from "node:url";
import { dirname, join, relative } from "node:path";
import { loadEnvironment } from "./idl_installed.mjs";
import { loadIdl } from "./idl_members.mjs";
import { readDictDecls } from "./idl_dictdecl.mjs";
import { typeConvention } from "./idl_typename.mjs";
import { contract } from "./idl_argdecl.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);
const HOST = join(HERE, "host");
const CORE = join(HOST, "browser/core");

const ALL = process.argv.includes("--all");
const CONFIRM = process.argv.includes("--confirm");
const at = (r) => `${relative(ROOT, r.file)}:${r.line}`;

const C = contract(join(CORE, "idl_args.h"));
const idl = await loadIdl();
const { enumeratorFor, resolveTypedef: resolveTypedefOf } = typeConvention(idl);
const env = loadEnvironment(HOST);
const { arrays, named, unreadable } = readDictDecls(env);

/* ---- what the SPEC says each dictionary's members are, in the engine's own vocabulary --------------------
 *
 * `want` is the enumerator the convention predicts. `alsoOk` is the OTHER spelling of the same conversion
 * where §3.2.17 makes two spellings one — see the required-member paragraph in the header. `null` means the
 * convention had nothing to say, which is this audit's reach and never the engine's defect. */
const NO_DEFAULT = "_NO_DEFAULT";
const specOf = new Map();
for (const d of idl.dictByName.keys()) {
  const m = new Map();
  for (const mem of idl.dictMembers(d)) {
    const stem = enumeratorFor(mem);
    let want = stem, alsoOk = null;
    if (stem && !mem.hasDefault && C.all.has(stem + NO_DEFAULT)) {
      want = stem + NO_DEFAULT;
      /* §3.2.17 step 4.1.6 throws for an absent required member, so its two spellings are one conversion. */
      if (mem.required) alsoOk = stem;
    }
    m.set(mem.name, { want, alsoOk, spec: specText(mem), base: baseName(mem) });
  }
  specOf.set(d, m);
}
/* The member's type name as the IDL WRITES IT, after §2.4's typedefs and before the convention upper-cases
   anything. The split below is about this string and never about the composed enumerator, which is upper-case
   by construction and so cannot be asked whether it is an §2.1 identifier. */
function baseName(mem) {
  const t = resolveTypedefOf(mem.idlType);
  return t && typeof t.idlType === "string" ? t.idlType : null;
}
/* The member's type as the IDL writes it, for a finding to quote rather than paraphrase. */
function specText(mem) {
  const t = mem.idlType;
  const nm = typeof t.idlType === "string" ? t.idlType
           : t.union ? "(union)" : t.generic ? `${t.generic}<…>` : "?";
  const ext = (mem.extAttrs || []).map((a) => `[${a.name}] `).join("");
  return `${ext}${nm}${t.nullable ? "?" : ""}${mem.required ? " (required)" : ""}` +
         `${mem.hasDefault ? " = …" : ""}`;
}
const nameSets = new Map();
for (const [d, m] of specOf) nameSets.set(d, new Set(m.keys()));

/* An array that a named IdlDictDecl points at IS that dictionary's declaration; every other array is
   constrained by its member names and by nothing else. */
const identifiedBy = new Map();
for (const n of named) identifiedBy.set(`${n.arr.file}\0${n.arr.sym}`, n.name);

/* ---- the join --------------------------------------------------------------------------------------------
 *
 * EVERY BAND IS COUNTED AND EVERY BAND IS ONE QUESTION. Collapsing "the convention is silent about this type"
 * into "this row is wrong" is the bucket collapse that makes a count unreadable, and collapsing it the other
 * way is the false clean bill: the first manufactures work and the second hides it. */
const mismatch = [];        /* the accusing band: one candidate reading, and the engine does not have it */
const missingEnum = [];     /* the convention names a type this engine's IdlArgType cannot spell */
const unresolvedName = [];  /* the convention names a type NO declaration in the corpus defines */
const ambiguous = [];       /* the candidate dictionaries do not agree on this member's type */
const silent = [];          /* union / generic / unspelled extended attribute: no prediction exists */
const unreadType = [];      /* the reader could not place this row's type field */
const noCandidate = [];     /* no dictionary in the corpus has all of this array's member names */
const confirmed = [];
const confirmedEither = []; /* required, and the engine writes the other spelling of the one conversion */

/* Web IDL §2.1 Names: an `identifier` is the production a DICTIONARY, INTERFACE, ENUMERATION or TYPEDEF name
   comes from, and every one this platform declares begins with an upper-case letter, while the primitive type
   names the grammar spells out (`short`, `unsigned long long`, `double`, `object`, `boolean`, …) are entirely
   lower-case. The three mixed-case primitives — DOMString, ByteString, USVString — are declared in the enum,
   so they never reach here. That is what separates a Web IDL type this engine cannot spell (an engine gap,
   and a finding) from a name the corpus uses and defines nowhere (a corpus gap, and this audit's reach).
   BOTH BANDS PRINT THE COMPOSED NAME AND THE SPEC'S OWN TYPE TEXT, so a reader can see the split is right
   rather than take it. */
const looksLikeIdentifier = (s) => /[A-Z]/.test(s);

for (const a of arrays) {
  const id = identifiedBy.get(`${a.file}\0${a.sym}`);
  const names = new Set(a.members.map((m) => m.name));
  let cands;
  if (id && specOf.has(id)) cands = [id];
  else {
    cands = [];
    for (const [d, s] of nameSets) if (names.size && [...names].every((n) => s.has(n))) cands.push(d);
  }
  for (const mem of a.members) {
    const row = { a, mem, id, cands };
    if (!mem.type) { unreadType.push(row); continue; }
    if (!cands.length) { noCandidate.push(row); continue; }

    /* One prediction, or none. A member the candidates disagree about is not a weaker finding — it is a
       different question, and answering it would need the identity this audit does not have. */
    const wants = new Set(), oks = new Set(), specs = new Set(), bases = new Set();
    let anySilent = false;
    for (const d of cands) {
      const e = specOf.get(d).get(mem.name);
      if (!e || e.want == null) { anySilent = true; continue; }
      wants.add(e.want);
      if (e.alsoOk) oks.add(e.alsoOk);
      specs.add(e.spec);
      if (e.base) bases.add(e.base);
    }
    if (!wants.size) { silent.push(row); continue; }
    if (wants.size !== 1 || anySilent) { row.wants = wants; ambiguous.push(row); continue; }
    row.want = [...wants][0];
    row.alsoOk = oks.size === 1 ? [...oks][0] : null;
    row.spec = specs.size === 1 ? [...specs][0] : `${specs.size} readings`;

    if (!C.all.has(row.want)) {
      /* One base name, or the split has nothing to ask about and the row goes to the reach band. */
      const base = bases.size === 1 ? [...bases][0] : null;
      row.base = base;
      (base && looksLikeIdentifier(base) ? unresolvedName : missingEnum).push(row);
      continue;
    }
    if (row.want === mem.type) { confirmed.push(row); continue; }
    if (row.alsoOk && row.alsoOk === mem.type) { confirmedEither.push(row); continue; }
    mismatch.push(row);
  }
}

/* ---- the report ------------------------------------------------------------------------------------------ */
const cap = (xs) => (ALL ? xs : xs.slice(0, 200));
const say = (s) => console.log(s);
const rows = arrays.reduce((n, a) => n + a.members.length, 0);

say("");
say(`[idl-dicttype] ${arrays.length} IdlDictMember declaration(s) read (${named.length} of them named by an ` +
    `IdlDictDecl), ${rows} member row(s); ${unreadable.length} declaration(s) the reader refused.`);
say(`[idl-dicttype] Web IDL §3.2.17 Dictionary types converts a member BY ITS DECLARED TYPE, so a wrong type ` +
    `is a wrong conversion at a point in a fixed order a page can observe with a getter.`);
say("");

const band = (xs, title) => { say(`[idl-dicttype] ${String(xs.length).padStart(6)}  ${title}`); };
band(mismatch, "member rows whose declared IdlArgType is not the type the IDL gives that member");
band(missingEnum, "member rows whose IDL type this engine's IdlArgType cannot spell at all");
band(unresolvedName, "member rows whose IDL type names a declaration no spec in @webref/idl defines (this audit's reach)");
band(ambiguous, "member rows whose candidate dictionaries do not agree on the type (needs the declaration's identity)");
band(silent, "member rows at a union/generic/unspelled-attribute type the convention does not compose (this audit's reach)");
band(unreadType, "member rows whose type field this reader could not place");
band(noCandidate, "member rows in an array no corpus dictionary could be the declaration of");
band(confirmed, "member rows CONFIRMED against the IDL");
band(confirmedEither, "member rows CONFIRMED where §3.2.17 step 4.1.6 makes the two spellings one conversion");

if (mismatch.length) {
  say("");
  say("--- DECLARED TYPE DISAGREES WITH THE IDL ------------------------------------------------------------");
  say("Each row is a conversion Web IDL §3.2.17 step 4.1.4.1 performs by the declared type. Fix it at the");
  say("declaration; where the engine has no enumerator for the IDL's type, that is the type to add.");
  for (const r of cap(mismatch)) {
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name}`);
    say(`      declared ${r.mem.type}`);
    say(`      IDL says \`${r.spec}\` = ${r.want}` +
        `   [${r.id ? `IdlDictDecl names ${r.id}` : `${r.cands.length} candidate dictionar${r.cands.length === 1 ? "y" : "ies"}: ${r.cands.slice(0, 4).join(", ")}`}]`);
  }
}
if (missingEnum.length) {
  say("");
  say("--- A WEB IDL TYPE THIS ENGINE CANNOT SPELL --------------------------------------------------------");
  say("The convention composed a name core/idl_args.h's IdlArgType does not declare. That is a MISSING TYPE,");
  say("not a reach limit: whatever the member is declared instead runs a different §3.2 conversion.");
  for (const r of cap(missingEnum))
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name}  declared ${r.mem.type}  IDL says \`${r.spec}\` = ${r.want} (undeclared)`);
}
if (unresolvedName.length) {
  say("");
  say("--- A TYPE NAME THE CORPUS USES AND DEFINES NOWHERE ------------------------------------------------");
  for (const r of cap(unresolvedName))
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name}  declared ${r.mem.type}  IDL says \`${r.spec}\` -> ${r.want}`);
}
if (ambiguous.length) {
  say("");
  say("--- THE CANDIDATE DICTIONARIES DISAGREE ------------------------------------------------------------");
  say("A bare IdlDictMember array names no dictionary, so this audit judges only where every dictionary that");
  say("could own the array predicts one type. Pairing an array with its dictionary through the");
  say("idl_method_id_dict site that USES it is what turns these into judgements — idlgen.mjs's residual.");
  for (const r of cap(ambiguous))
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name}  declared ${r.mem.type}  candidates predict {${[...r.wants].join(", ")}}` +
        ` over ${r.cands.length} dictionar${r.cands.length === 1 ? "y" : "ies"}`);
}
if (unreadType.length) {
  say("");
  say("--- THE READER COULD NOT PLACE THE TYPE FIELD ------------------------------------------------------");
  for (const r of cap(unreadType))
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name}  field 1 holds \`${r.mem.typeText}\``);
}
if (noCandidate.length) {
  say("");
  say("--- NO CANDIDATE DICTIONARY -----------------------------------------------------------------------");
  for (const r of cap(noCandidate))
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name}  declared ${r.mem.type}`);
}
if (unreadable.length) {
  say("");
  say("--- DECLARATIONS THIS READER REFUSED --------------------------------------------------------------");
  for (const u of cap(unreadable)) say(`  ${at(u)}  ${u.sym}: ${u.why}`);
}
if (CONFIRM) {
  say("");
  say("--- CONFIRMED -------------------------------------------------------------------------------------");
  for (const r of cap(confirmed))
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name} = ${r.mem.type}  (\`${r.spec}\`)`);
  say("");
  say("--- CONFIRMED, EITHER SPELLING (required member: §3.2.17 step 4.1.6 removes the absent state) -------");
  for (const r of cap(confirmedEither))
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name} = ${r.mem.type}  (\`${r.spec}\`; ${r.want} is the other spelling)`);
  say("");
  say("--- THE CONVENTION IS SILENT ----------------------------------------------------------------------");
  for (const r of cap(silent))
    say(`  ${at(r.a)}  ${r.a.sym}.${r.mem.name} = ${r.mem.type}`);
}
say("");
say("This audit REPORTS and exits 0 — see the header for why, and for what it cannot see.");
