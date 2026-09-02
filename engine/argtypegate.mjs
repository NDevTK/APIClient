/* THE DECLARATION-AGAINST-SPEC ARGUMENT AUDIT — the fourth axis, and the one the other three cannot reach.
 *
 *   node engine/argtypegate.mjs              audit every member the engine installs
 *   node engine/argtypegate.mjs --all        print every finding rather than the first 200 of a kind
 *   node engine/argtypegate.mjs --confirm    also print what it CONFIRMED and what it could not place
 *
 * WHAT IT CHECKS, AND WHY THE OTHER AUDITS CANNOT. idlgen.mjs diffs the spec's member LIST against what a
 * component installs: a NAME check, and a member that exists under a WRONG SIGNATURE reads COMPLETE to it.
 * argaudit.mjs joins a member's DECLARED argument types against its own BODY, so it catches a body that
 * re-converts what the declaration already converted — but it takes the declaration as given, and a
 * declaration that says `DOMString` where the IDL says `USVString` is internally consistent and silently
 * wrong. WPT's idlharness reads `.length` off every member, which is the ARITY of what is installed and says
 * nothing about which TYPE each position carries. Nothing was asking whether the declaration matches the
 * IDL — so this asks exactly that, and nothing else.
 *
 * The join is three-legged and every leg is read rather than assumed: the INSTALL names the member and the
 * object it lands on (engine/idl_installed.mjs already solves which interface that object is, off Web IDL
 * §3.7.3's @@toStringTag); the install's `stepid` argument names the DECLARATION that made it; and the
 * declaration states the types, the arity, the variadic tail and where the optional arguments start
 * (engine/idl_argdecl.mjs reads those, once, for this audit and for argaudit.mjs both).
 *
 * THE TYPE NAMES ARE DERIVED, NOT TABULATED. CLAUDE.md: "an auditor DERIVES the rule it checks from the code
 * that owns it, NEVER restates it — a restated rule is a second copy, and the one that drifts is the copy
 * nobody runs against reality." A table mapping `unsigned long` to IDL_UNSIGNED_LONG would be that copy. What
 * the engine actually owns is the enum in core/idl_args.h, and it owns a CONVENTION with it: the enumerator is
 * the IDL type's own spelling, upper-cased, with spaces as underscores, and an extended attribute or a
 * nullable wrapper appended as a suffix. So this composes the name that convention predicts and asks the enum
 * — read out of the header — whether it has it. A type the convention cannot place is reported UNPLACEABLE and
 * judged against nothing: that is a statement about this audit's reach and never about the engine, and
 * collapsing the two into one number is the bucket collapse that makes a count unreadable.
 *
 * ITS BLIND SPOTS ARE PART OF THE INSTRUCTION, because a checker trusted past its evidence is worse than none:
 *
 *   - AN OVERLOADED MEMBER IS COUNTED AND NEVER JUDGED. Web IDL §3.6 gives one member several signatures and
 *     this engine declares ONE entry with the union types the enum carries for exactly that purpose
 *     (IDL_USVSTRING_OR_DICT, IDL_STRING_UNLESS_CALLABLE). Which spec signature a declaration is meant to be is
 *     a question about the member's own algorithm, and answering it from a type list would manufacture
 *     findings. They are listed under --confirm so the set is visible rather than absent.
 *   - IT READS THE INSTALL'S `stepid` AS TEXT. A step id reached through an array element, a local, or a
 *     helper's parameter is UNJOINED — named with its file and line, never dropped and never guessed.
 *   - A MEMBER INSTALLED FROM ONE FILE AND DECLARED IN ANOTHER is joined only when the declaration's
 *     assignment target is unique corpus-wide; where two files declare the same identifier it is UNJOINED.
 *   - IT COMPARES A POSITION'S ENUMERATION VALUES AND NEVER JUDGES THEM. `idl_arg_enum` states §3.2.18's value
 *     list at a position and the IDL states it again, so the join exists — but the corpus carries whichever
 *     edition of an enumeration it was built from, and for `KeyFormat` that is a WICG draft's widened eight
 *     rather than Web Cryptography API Level 2's four. A judged comparison would report a correct declaration
 *     as a defect, so ENUMVALUES prints both lists and is counted. What IS judged is whether the position is
 *     an enumeration at all, which no edition disagrees about.
 *   - IT COMPARES A POSITION'S NAMED INTERFACE ONLY WHERE THE DECLARATION NAMES ONE. `idl_iface_brand` states
 *     a CLASS ID and no identifier, so a member branded that way has nothing for the spec's interface name to
 *     be compared against and the IFACE axis is silent about it — which is most of the platform. That silence
 *     is this audit's reach, not a clean bill.
 *   - IT ASKS ABOUT ARGUMENTS AND NOTHING ELSE. A wrong RETURN type, a missing [CEReactions], a getter's
 *     value: none of them are visible here, and a clean run says nothing about them.
 *   - IT DOES NOT COMPARE DECLARED DEFAULTS. `idl_arg_default` states §3.6's third state at a position and the
 *     IDL states it with `= …`, so the join exists — but the two disagree wherever the declared type's
 *     conversion of `undefined` ALREADY IS the IDL's default (§7.1.2's second step makes `optional boolean
 *     x = false` such a case, and the legacy initializers leave `= false` out at their boolean positions for
 *     that reason while STATING `= null` at their interface positions, where §3.6's absent-optional arm runs
 *     ahead of §3.2.20's null rule and the body would otherwise be handed `undefined`), so a diff of
 *     the two lists is mostly rows a page cannot observe. Checking it means asking whether the default and the
 *     absent-optional conversion differ, which is a question about the TYPE's conversion and not about the
 *     declaration — the axis after this one, not a gap in it.
 *   - IT SCANS THE CORPUS `idl_installed.mjs` IS POINTED AT. A member installed outside it is not seen.
 *
 * IT IS NOT A BUILD GATE, deliberately and for citegen.mjs's reason: it reads SOURCE TEXT joined to a corpus
 * that moves under it, so a checker that fails a lane for a finding the lane did not introduce gets muted
 * exactly as fast as one that cries wolf. It prints; the human decides. It exits 0.
 */
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, relative } from "node:path";
import { loadEnvironment, installedMembers } from "./idl_installed.mjs";
import { loadIdl } from "./idl_members.mjs";
import { contract, declarations, strip, args, callText, lineOf, INTERPRETED_MODS } from "./idl_argdecl.mjs";
import { typeConvention } from "./idl_typename.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);
const HOST = join(HERE, "host");
const CORE = join(HOST, "browser/core");

const ALL = process.argv.includes("--all");
const CONFIRM = process.argv.includes("--confirm");

const C = contract(join(CORE, "idl_args.h"));
const idl = await loadIdl();

/* ---- what KIND of thing a spec type name is, and the enumerator the convention predicts for it ------------
 *
 * BOTH ARE engine/idl_typename.mjs's AND NOT THIS FILE'S. They were written here and moved out unchanged when
 * dicttypegate.mjs needed the same composition at a DICTIONARY MEMBER: §3.2's conversions are per TYPE and
 * know nothing about where the type was written, so an argument position and a dictionary member ask one
 * question — and two copies of it would be the restated rule CLAUDE.md's auditor discipline exists to stop,
 * with the copy that drifts being whichever gate is run less often. */
const { enums, resolveTypedef, enumeratorFor } = typeConvention(idl);

/* A VARIADIC `any...` TAIL IS THE ONE THE DECLARATION MAY OMIT, and this is read off idl_args.h rather than
   decided here: idl_method_id's own paragraph says "A position the IDL does not list is passed through
   unconverted, which is what a variadic `any...` tail means and what an optional argument beyond the listed
   ones means." An undeclared position and an IDL_ANY position therefore run the SAME conversion — none — so
   there is no observable difference to report, and reporting one would fail every timer and every
   `postMessage` for stating the platform correctly. A tail of any OTHER type is a real difference: only its
   first position is converted and every further argument reaches the body unconverted. */
const anyTail = (op) => {
  const last = (op.arguments || []).at(-1);
  if (!last || !last.variadic) return false;
  const t = resolveTypedef(last.idlType);
  return !!t && !t.union && !t.generic && t.idlType === "any";
};

/* ---- the engine side: every declaration, keyed by the step-id it is assigned to --------------------------- */
const env = loadEnvironment(HOST);
const cFiles = [...env.sources.keys()].filter((p) => p.endsWith(".c"));

const declByLhs = new Map();        /* "file\0lhs" -> decl */
const declByLhsGlobal = new Map();  /* lhs -> [ {file, decl} … ] */
/* §3.2.18's value lists, corpus-wide: identifier -> [ {file, values} … ]. It is GLOBAL because an enumeration
   two members share is written once and declared `extern` (IDBCursorDirection is written in idb_cursor.c and
   declared at four positions in two other files), so resolving it from the declaring file alone would report
   every shared list as unreadable. A name two files define is carried as two entries and reported as
   ambiguous, exactly as a step id assigned twice is — the audit says which question it could not answer rather
   than picking an answer. */
const enumListsGlobal = new Map();
const stripped = new Map();
for (const path of cFiles) {
  const raw = env.sources.get(path).orig;
  const src = strip(raw);
  stripped.set(path, src);
  const reader = declarations(src, raw);
  for (const [name, values] of reader.enumLists) {
    if (!enumListsGlobal.has(name)) enumListsGlobal.set(name, []);
    enumListsGlobal.get(name).push({ file: path, values });
  }
  const { decls } = reader.read(C);
  for (const d of decls) {
    if (!d.lhs) continue;
    d.file = path;
    d.line = lineOf(src, d.at);
    declByLhs.set(`${path}\0${d.lhs}`, d);
    if (!declByLhsGlobal.has(d.lhs)) declByLhsGlobal.set(d.lhs, []);
    declByLhsGlobal.get(d.lhs).push(d);
  }
}

/* The `stepid` expression of the install written at this file and line — the third leg of the join. The forms
   that take one are named by their own header: idl_install_method / _unforgeable / _exposed put it at index 3,
   idl_install_step_method declares a `length` first and puts it at index 4. */
const STEPID_AT = { idl_install_method: 3, idl_install_method_unforgeable: 3, idl_install_method_exposed: 3,
                    idl_install_step_method: 4 };
function stepidAt(path, line) {
  const src = stripped.get(path);
  if (!src) return null;
  for (const m of src.matchAll(/\b(idl_install_(?:method|method_unforgeable|method_exposed|step_method))\s*\(/g)) {
    if (lineOf(src, m.index) !== line) continue;
    const { text } = callText(src, m.index + m[0].length - 1);
    const a = args(text);
    const at = STEPID_AT[m[1]];
    return a[at] == null ? null : { expr: a[at].replace(/\s+/g, ""), form: m[1] };
  }
  return null;
}

/* ---- the join --------------------------------------------------------------------------------------------- */
const world = installedMembers(cFiles, env);
const findings = [];
const push = (kind, r) => findings.push({ kind, ...r });

/* An operation node's positional list, with the two facts §3.6 reads off it. */
const opShape = (op) => ({
  n: (op.arguments || []).length,
  variadic: (op.arguments || []).some((a) => a.variadic),
  firstOptional: (() => {
    const i = (op.arguments || []).findIndex((a) => a.optional || a.variadic);
    return i < 0 ? (op.arguments || []).length : i;
  })(),
});

let joined = 0, considered = 0;
for (const rec of world.records) {
  if (!rec.ifaces || rec.ifaces.length !== 1) continue;   /* attribution is idlgen's question, not this one */
  const iface = rec.ifaces[0];
  const node = idl.byName.get(iface);
  if (!node) continue;
  const ops = idl.flatten(iface).filter((m) => m.type === "operation" && m.name === rec.name);
  if (!ops.length) continue;                              /* an absent-from-spec member is idlgen's finding */
  considered++;
  const at = `${relative(ROOT, rec.file)}:${rec.line}`;
  const who = `${iface}.${rec.name}`;

  const sid = stepidAt(rec.file, rec.line);
  if (!sid) { push("UNJOINED", { at, who, why: "the install at this line names no step id this can read — an "
    + "accessor, a generated form, or a construct whose stepid argument is not written here" }); continue; }
  let decl = declByLhs.get(`${rec.file}\0${sid.expr}`);
  if (!decl) {
    const g = declByLhsGlobal.get(sid.expr) || [];
    if (g.length === 1) decl = g[0];
    else { push("UNJOINED", { at, who, why: g.length
      ? `the step id \`${sid.expr}\` is assigned by ${g.length} declarations across the corpus, so which one `
        + "this install names cannot be read from text"
      : `the step id \`${sid.expr}\` is not the target of any idl_method_id* assignment this can see — an array `
        + "element, a local, or a declaration made through a helper" }); continue; }
  }
  joined++;

  if (ops.length > 1) {
    push("OVERLOADED", { at, who, why: `§3.6 gives this member ${ops.length} signatures and the engine declares `
      + `one entry of ${decl.nargs} position(s) — which signature the declaration is meant to be is a question `
      + "about the member's algorithm, so it is counted and not judged" });
    continue;
  }
  const op = ops[0];
  const spec = opShape(op);
  const dat = `${relative(ROOT, decl.file)}:${decl.line}`;

  /* (1) ARITY — how many positions the IDL lists against how many the declaration does. An `any...` tail may
     be omitted or listed and both spell the same conversion, so both counts are accepted for one. */
  const omissible = anyTail(op) ? spec.n - 1 : spec.n;
  if (decl.nargs == null)
    push("UNJOINED", { at, who, why: `the declaration at ${dat} states \`${decl.nargsExpr}\` positions, which is `
      + "not a literal this can read" });
  else if (decl.nargs !== spec.n && decl.nargs !== omissible)
    push("ARITY", { at, who, why: `the IDL lists ${spec.n} argument(s) and the declaration at ${dat} states `
      + `${decl.nargs}${spec.variadic ? " (the IDL's tail is variadic)" : ""}` });

  /* (2) THE VARIADIC TAIL — §3.6 step 16.2 makes a variadic head behave unlike every other position, and the
     engine states it per member for the reason idl_args.h gives: assuming it once converted addEventListener's
     CALLBACK to a string. Nothing in this tree was checking the two agree. */
  if (spec.variadic && !decl.variadic && !anyTail(op))
    push("VARIADIC", { at, who, why: `the IDL declares a variadic tail (\`${op.arguments.at(-1).name}\`) and the `
      + `declaration at ${dat} is \`idl_${decl.kind}\` with no variadic flag, so the tail's declared type `
      + "applies to one position and every further argument crosses as IDL_ANY" });
  else if (!spec.variadic && decl.variadic)
    push("VARIADIC", { at, who, why: `the declaration at ${dat} sets variadic and the IDL declares no variadic `
      + "tail, so a call past the listed arity converts arguments the IDL does not list" });

  /* (3) HOW MANY ARGUMENTS THE MEMBER REQUIRES — §3.7.7 Operations' `length`, which is also the number §3.6
     step 5's arity check throws below. It is compared here rather than `idl_optional_from`'s raw index for the
     reason idl_args.c's own idl_member_length_of gives: for a variadic member the two are NOT the same number,
     and reading the raw index reported `classList.add()` as throwing when §2.5.8 Overloading appends the empty
     tuple for a final variadic argument and the engine already derives that. On the spec side it is the index
     of the first optional-or-variadic argument; on the engine's it is min(first optional, declared positions),
     where a variadic tail is not a declared position. Both sides say what a page reads off `fn.length`. */
  if (decl.nargs != null) {
    if (decl.optionalFromExpr != null && decl.optionalFrom == null)
      push("UNJOINED", { at, who, why: `idl_optional_from at ${dat} states \`${decl.optionalFromExpr}\`, which `
        + "is not a literal this can read" });
    else {
      const declaredPositions = decl.variadic ? decl.nargs - 1 : decl.nargs;
      const firstOpt = decl.optionalFrom == null ? decl.nargs : decl.optionalFrom;
      const engineLength = Math.min(firstOpt, declaredPositions);
      if (engineLength !== spec.firstOptional)
        push("LENGTH", { at, who, why: `§3.7.7 Operations makes this member's length ${spec.firstOptional} — the `
          + `IDL's first optional-or-variadic argument — and the declaration at ${dat} derives `
          + `${engineLength} from ${decl.optionalFrom == null ? "no idl_optional_from" : `idl_optional_from(${decl.optionalFrom})`}`
          + ` over ${declaredPositions} declared position(s)` });
    }
  }

  /* Anything the declaration said that this reader did not take account of, named rather than assumed away. */
  for (const name of decl.mods.keys())
    if (!INTERPRETED_MODS.has(name))
      push("MODIFIER", { at, who, why: `the declaration at ${dat} carries \`idl_${name}\`, which idl_argdecl.mjs `
        + "does not interpret — every judgement above was made without it" });

  /* (4) THE TYPE AT EACH POSITION. */
  if (!decl.types) {
    push("UNJOINED", { at, who, why: `the declaration at ${dat} names a type list this cannot resolve` });
    continue;
  }
  const n = Math.min(decl.types.length, spec.n);
  for (let i = 0; i < n; i++) {
    const arg = op.arguments[i];
    const want = enumeratorFor(arg);
    const got = decl.types[i];
    if (!want || !C.all.has(want)) {
      push("UNPLACEABLE", { at, who, why: `position ${i} (\`${arg.name}\`) is ${typeText(arg)}, which this `
        + `audit's naming convention ${want ? `spells \`${want}\` — a name idl_args.h does not declare` : "cannot spell"}`
        + `; declared \`${got}\`` });
      continue;
    }
    if (want === got) { push("CONFIRMED", { at, who, why: `position ${i} (\`${arg.name}\`) is ${typeText(arg)} = ${got}` }); continue; }
    push("TYPE", { at, who, why: `position ${i} (\`${arg.name}\`) is ${typeText(arg)}, which idl_args.h declares `
      + `as \`${want}\`, and the declaration at ${dat} states \`${got}\`` });
  }

  /* (5) AND WHICH INTERFACE A POSITION THAT NAMES ONE SAYS IT IS. `idl_arg_iface` states §3.2.15's `I` as a
     PREDICATE plus the interface's IDL identifier, and the identifier is the subject of the TypeError the
     conversion throws — so it is a claim about the spec that the spec can answer, and one no other axis here
     sees: the type check above compares `IDL_INTERFACE_NULLABLE` against `IDL_INTERFACE_NULLABLE` and is
     satisfied whichever interface the position meant. Without this the identifier is a string nobody verifies,
     which is the same fact stated in two places with only one of them checked. */
  for (const ai of decl.argIfaces) {
    if (ai.index == null || ai.index >= spec.n) {
      push("UNJOINED", { at, who, why: `idl_arg_iface at ${dat} names position \`${ai.index}\`, which is not a `
        + `position the IDL's ${spec.n} argument(s) list` });
      continue;
    }
    const t = resolveTypedef(op.arguments[ai.index].idlType);
    const base = t && !t.union && !t.generic && typeof t.idlType === "string" ? t.idlType : null;
    if (ai.iface == null || base == null) {
      push("UNPLACEABLE", { at, who, why: `idl_arg_iface at ${dat} states position ${ai.index}'s interface and `
        + `${ai.iface == null ? "this reader could not read the identifier it named"
                              : `the IDL's type there (${typeText(op.arguments[ai.index])}) names no single interface`}` });
      continue;
    }
    if (base === ai.iface) {
      push("CONFIRMED", { at, who, why: `position ${ai.index} (\`${op.arguments[ai.index].name}\`) brands `
        + `against \`${ai.iface}\` (${ai.pred})` });
      continue;
    }
    push("IFACE", { at, who, why: `position ${ai.index} (\`${op.arguments[ai.index].name}\`) is `
      + `${typeText(op.arguments[ai.index])}, and idl_arg_iface at ${dat} names the interface \`${ai.iface}\` — `
      + "the identifier is the subject of the TypeError §3.2.15 throws, so a page is told it failed an "
      + "interface the IDL does not declare there" });
  }

  /* (6) AND WHICH ENUMERATION A POSITION THAT NAMES ONE SAYS IT IS — §3.2.18's `E`, the same shape as the
     interface axis directly above and blind in the same place: silent about a position that declares none.
     TWO QUESTIONS, AND THEY ARE SPLIT BECAUSE ONLY ONE OF THEM IS EDITION-STABLE.
       - THE POSITION is judged. Whether the IDL's type there is an enumeration at all (its own, or the element
         type of a `sequence<E>`) is a fact every edition of a spec agrees on, and a value list stated at a
         position the IDL does not make an enumeration is a declaration about a member that is not this one.
       - THE VALUES are counted and never judged, for the OVERLOADED bucket's reason one axis over: which
         EDITION of an enumeration a declaration is meant to be is a question the corpus cannot answer, and it
         is not hypothetical here. `enum KeyFormat` is not written in webref's `webcrypto.idl` at all — the
         corpus's only definition of it comes from the WICG "Modern Algorithms in the Web Cryptography API"
         draft, which widens it from the published Web Cryptography API Level 2's four values to eight. A
         judged comparison would report the engine's correct §14.1 declaration as a defect. So both lists are
         printed and a human decides. */
  for (const ae of decl.argEnums) {
    if (ae.index == null || ae.index >= spec.n) {
      push("UNJOINED", { at, who, why: `idl_arg_enum at ${dat} names position \`${ae.index}\`, which is not a `
        + `position the IDL's ${spec.n} argument(s) list` });
      continue;
    }
    const arg = op.arguments[ae.index];
    const name = enumAt(arg);
    if (!name) {
      push("ENUM", { at, who, why: `position ${ae.index} (\`${arg.name}\`) is ${typeText(arg)}, which names no `
        + `enumeration, and idl_arg_enum at ${dat} states §3.2.18's values there — no conversion reads them, so `
        + "the declaration describes a position that never tests against them" });
      continue;
    }
    const defs = enumListsGlobal.get(ae.list) || [];
    if (defs.length !== 1) {
      push("UNPLACEABLE", { at, who, why: `idl_arg_enum at ${dat} names the value list \`${ae.list}\` for `
        + `position ${ae.index} (the IDL's \`${name}\`), and ${defs.length
          ? `${defs.length} files define that identifier, so which list this names cannot be read from text`
          : "no file this audit scanned defines that identifier"}` });
      continue;
    }
    const got = defs[0].values, want = enums.get(name) || [];
    if (got.length === want.length && got.every((v, i) => v === want[i])) {
      push("CONFIRMED", { at, who, why: `position ${ae.index} (\`${arg.name}\`) declares \`${name}\` as `
        + `\`${ae.list}\` = [${got.join(", ")}]` });
      continue;
    }
    push("ENUMVALUES", { at, who, why: `position ${ae.index} (\`${arg.name}\`) is ${typeText(arg)}; the corpus's `
      + `\`${name}\` is [${want.join(", ")}] and \`${ae.list}\` (${relative(ROOT, defs[0].file)}) is `
      + `[${got.join(", ")}] — counted and not judged, since which EDITION of the enumeration the declaration `
      + "is meant to be is a question about the member's own algorithm" });
  }
}

/* THE ENUMERATION AN ARGUMENT'S IDL TYPE NAMES, or null. §3.2.18's `E` is stated at a position two ways and
   both are one question here: the position's own type is an enumeration, or it is a `sequence<E>` /
   `FrozenArray<E>` whose ELEMENT type is one — which is what IDL_SEQUENCE_ENUM declares, and the reason this
   sees through the generic rather than asking about the position's outermost type. */
function enumAt(arg) {
  const t = resolveTypedef(arg.idlType);
  if (!t || t.union) return null;
  if (t.generic) {
    if (!["sequence", "FrozenArray"].includes(t.generic)) return null;
    const el = resolveTypedef(Array.isArray(t.idlType) ? t.idlType[0] : t.idlType);
    if (!el || el.union || el.generic || typeof el.idlType !== "string") return null;
    return enums.has(el.idlType) ? el.idlType : null;
  }
  return typeof t.idlType === "string" && enums.has(t.idlType) ? t.idlType : null;
}

function typeText(arg) {
  const t = arg.idlType;
  const raw = (function name(x) {
    if (!x) return "?";
    if (x.union) return "(" + x.idlType.map(name).join(" or ") + ")";
    if (x.generic) return `${x.generic}<${(Array.isArray(x.idlType) ? x.idlType : [x.idlType]).map(name).join(", ")}>`;
    return typeof x.idlType === "string" ? x.idlType : name(x.idlType);
  })(t) + (t && t.nullable ? "?" : "");
  const ext = [...(t.extAttrs || []), ...(arg.extAttrs || [])].map((a) => `[${a.name}]`).join("");
  return "`" + ext + (ext ? " " : "") + raw + "`" + (arg.optional ? " optional" : "") + (arg.variadic ? " variadic" : "");
}

/* ---- the verdict ------------------------------------------------------------------------------------------ */
console.log(`declaration-against-spec argument audit — ${cFiles.length} .c file(s), `
  + `${world.records.length} install record(s)`);
console.log(`${considered} installed operation(s) the corpus declares; ${joined} joined to a C declaration\n`);

const ORDER = ["TYPE", "IFACE", "ENUM", "ARITY", "VARIADIC", "LENGTH", "UNJOINED", "UNPLACEABLE", "OVERLOADED",
               "ENUMVALUES", "MODIFIER", "CONFIRMED"];
const JUDGED = new Set(["TYPE", "IFACE", "ENUM", "ARITY", "VARIADIC", "LENGTH"]);
for (const kind of ORDER) {
  const rows = findings.filter((f) => f.kind === kind);
  if (!rows.length) continue;
  if (!JUDGED.has(kind) && !CONFIRM) { console.log(`${kind}: ${rows.length}   (--confirm to list)`); continue; }
  console.log(`\n${kind}: ${rows.length}`);
  for (const f of (ALL ? rows : rows.slice(0, 200))) {
    console.log(`  ${f.at}`);
    console.log(`      ${f.who}`);
    console.log(`      ${f.why}`);
  }
  if (!ALL && rows.length > 200) console.log(`  … ${rows.length - 200} more (--all)`);
}
console.log(`\nThis audit REPORTS and exits 0 — see the header for why, and for what it cannot see.`);
