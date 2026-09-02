/* THE RETURN-TYPE AXIS — the fourth question over an IDL member, and the one no other instrument here asks.
 *
 *   node engine/rettypegate.mjs           audit every operation the engine installs
 *   node engine/rettypegate.mjs --all     print every row of every band rather than the first 200
 *   node engine/rettypegate.mjs --confirm also print what it CONFIRMED
 *
 * WHY A FOURTH. There are three axes over a member and each is blind where the others look. idlgen.mjs asks
 * about NAMES: which members the spec lists that no component installs. argaudit.mjs asks whether a BODY
 * re-converts an argument whose declared type crosses concolically. argtypegate.mjs asks whether the
 * DECLARATION says what the IDL says about the ARGUMENTS — and its own header names the hole it leaves in the
 * same breath: "IT ASKS ABOUT ARGUMENTS AND NOTHING ELSE. A wrong RETURN type, a missing [CEReactions], a
 * getter's value: none of them are visible here, and a clean run says nothing about them." A member whose name
 * and arity and argument types are all correct and whose RETURN TYPE is wrong reads COMPLETE to all three.
 *
 * WHY WPT'S idlharness CANNOT ANSWER IT EITHER, WHICH IS THE FIRST QUESTION TO ASK BEFORE BUILDING AN AUDITOR.
 * `resources/idlharness.js` reads the published IDL and diffs the whole surface, so it is the oracle for
 * `.length` and for `.name` and for the property descriptor. For an OPERATION it never learns the return type,
 * because it never calls one with a valid receiver — `do_member_operation_asserts` asserts the descriptor, the
 * arity and `typeof === "function"`, and the only invocations it makes are the wrong-receiver and too-few-
 * arguments calls that must fail. (For an ATTRIBUTE it does check the value's type, via `assert_type_is`, but
 * only where a test document handed it an instance through `add_objects`.) What it DOES read the return type
 * for is the one thing this file is about: `throwOrReject` branches on `idlType.generic !== "Promise"` to
 * decide whether a bad call must THROW or must REJECT. So the corpus can observe part of this axis AT RUNTIME,
 * for the interfaces its documents cover, on the error paths it exercises, after a build and a WPT run — and
 * it reports a failing assertion rather than a SITE. This reads the declaration statically, covers every
 * installed operation rather than the ones a document names, and names the file and line. The two are worth
 * having together; neither is the other's substitute, and a green WPT run is not a clean bill on this axis.
 *
 * WHAT IT CHECKS. Web IDL §3.7.7 Operations' create an operation function wraps the brand check, the overload
 * resolution, every argument conversion AND the method steps in one `Try`, and closes it with:
 *
 *   "And then, if an exception E was thrown: If op has a return type that is a promise type, then return !
 *    Call(%Promise.reject%, %Promise%, «E»). Otherwise, end these steps and allow the exception to propagate."
 *
 * So a promise-returning operation NEVER throws synchronously — not for a wrong receiver, not for too few
 * arguments, not for a failed argument conversion, not for its own algorithm. A page that wrapped the call in
 * `.catch` and nothing else is relying on exactly that, and a member that throws instead takes the whole flow
 * down at a call site the bundle believed it had covered. THIS ENGINE STATES THAT AS A DECLARATION —
 * `idl_returns_promise()`, a post-declaration modifier naming the member the last declaration made — and
 * core/idl_args.c performs it at the ONE point every pool member's completion passes through. So the fact is
 * declared, the spec states it again, and the two can be diffed. That is the whole of what this asks.
 *
 * THE RULE IS DERIVED, NOT TABULATED, which is CLAUDE.md's standing rule for every auditor here: an auditor
 * derives what it checks from the code that owns the fact rather than restating it, because a restatement is a
 * second copy and the copy that drifts is the one nobody runs against reality. (That sentence is this project's
 * own, not any standard's — it is written without quotation marks on purpose, since a quoted run standing under
 * a citation is read by citegen.mjs as a claim about the SECTION, and this tree's prose in quotes is exactly
 * what its own report says nothing mechanical can separate from a fabricated one.)
 * There is no list of promise-returning members here and there is no list of modifier names:
 * the members come from the corpus's own return types, and whether `returns_promise` is a modifier at all is
 * read out of idl_args.h by contract(), which derives the modifier set from the header's own `void idl_x(…)`
 * shape. The day a second return-type fact becomes declarable, this file learns it the same way.
 *
 * THE BANDS ARE THREE AND THEY ARE THREE DIFFERENT QUESTIONS, WHICH IS WHY THEY ARE NOT ONE NUMBER. A member
 * whose declaration is MISSING and a member that has no declaration for this axis to read are different
 * states, and collapsing them would accuse the second population of the first's defect:
 *
 *   MISSING / EXTRA — the member reaches its body through core/idl_args.c's pool (an `idl_method_id`,
 *     `_ext`, `_dict` or `_step` declaration named by the install's step id), so the §3.7.7 conversion at that
 *     pool's step return is its mechanism and the declaration is either there or it is not. ACCUSED.
 *   OFF-POOL — the member is installed from a step id handed out by `JS_RegisterStepDef`, so it is its own
 *     JSTrampStepDef with its own step and fini and it never enters the pool's completion path at all.
 *     `idl_returns_promise` is not its mechanism; such a member builds and returns its promise itself, and its
 *     throws are its own algorithm's business. LISTED, judged against nothing.
 *   UNJOINED — the install at this line names no step id this reader can read, or names one no declaration in
 *     scope assigns. That is a statement about this audit's reach and never about the engine. LISTED.
 *
 * ITS BLIND SPOTS ARE PART OF THE INSTRUCTION, because a checker trusted past its evidence is worse than none:
 *
 *   - IT JUDGES ONE RETURN TYPE: whether the member's is a promise type. Every OTHER return type — an
 *     `undefined` operation that hands the page a value, a `boolean` one that hands back a number — is
 *     invisible here, and a clean run says nothing whatever about them. That is not an oversight to be filled
 *     by a table of expected return types: this engine declares no other return-type fact, so there is nothing
 *     to derive one from. The value of a non-promise member is carried by the body, as the JSValue an
 *     `IdlBody` returns or as what an `IdlStepBody` writes through its out-result parameter, and reading it
 *     means dataflow through a body a magic argument may share between several members with different return
 *     types. That is the axis after this one, not a gap in it.
 *   - IT ASKS ABOUT OPERATIONS AND NOT ATTRIBUTES. §3.7.7's promise clause is §3.7.7's; §3.7.6 Attributes'
 *     create an attribute getter has no such step, so a promise-valued attribute is not the same question and
 *     is not asked here. That is scope read off the spec, not a narrowing.
 *   - IT JUDGES AGAINST THE COMMITTED CORPUS EDITION. The return type comes from @webref/idl, which carries
 *     whichever edition it was built from, and where an editor's draft has moved ahead of what browsers ship
 *     this reports against the draft — which is the right answer under CLAUDE.md's rule that real Chrome is
 *     confirmation and the spec is the source of truth, and is still worth knowing when a row surprises you.
 *   - A NAME WHOSE CORPUS MEMBERS DISAGREE ABOUT BEING A PROMISE IS COUNTED AND NEVER JUDGED, and the row says
 *     which of the two shapes it is. A §3.6 overload set is one property, so one declaration cannot state two
 *     answers and which signature it means is a question about the algorithm. A STATIC and a regular member
 *     sharing a name are two properties in two places, and the install record names the member without naming
 *     the object it landed on, so this reader cannot say which was installed.
 *   - IT READS THE INSTALL'S `stepid` AS TEXT, exactly as argtypegate.mjs does, and inherits that reach: a step
 *     id reached through an array element, a local or a helper's parameter is UNJOINED, named with its file
 *     and line, never dropped and never guessed.
 *   - IT SCANS THE CORPUS `idl_installed.mjs` IS POINTED AT. A member installed outside it is not seen.
 *
 * IT IS NOT A BUILD GATE, for citegen.mjs's and argtypegate.mjs's reason: it reads SOURCE TEXT joined to a
 * corpus that moves under it, so a checker that fails a lane for a finding the lane did not introduce gets
 * muted exactly as fast as one that cries wolf. It prints; the human decides. It exits 0.
 */
import { fileURLToPath } from "node:url";
import { dirname, join, relative } from "node:path";
import { loadEnvironment, installedMembers } from "./idl_installed.mjs";
import { loadIdl } from "./idl_members.mjs";
import { contract, declarations, strip, args, callText, lineOf } from "./idl_argdecl.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);
const HOST = join(HERE, "host");
const CORE = join(HOST, "browser/core");

const ALL = process.argv.includes("--all");
const CONFIRM = process.argv.includes("--confirm");
const CAP = ALL ? Infinity : 200;

const C = contract(join(CORE, "idl_args.h"));
/* THE MODIFIER THIS AUDIT IS ABOUT MUST BE ONE THE HEADER STILL DECLARES. contract() derives the modifier set
   from idl_args.h's own `void idl_x(…)` shape rather than from a list, so this is the one place the derivation
   can be checked against the question being asked: if the declaration is renamed or retired, every member
   reads as MISSING and the audit becomes a wall of false accusations. Refusing here says which fact moved. */
if (!C.modifiers.has("returns_promise"))
  throw new Error("idl_args.h no longer declares `void idl_returns_promise(void)` — this audit diffs that "
    + "declaration against Web IDL §3.7.7's promise-return rule, so there is nothing for it to read. If the "
    + "fact is now stated another way, this file reads THAT and not a list of member names.");

const idl = await loadIdl();

/* Web IDL §2.4 Typedefs is pure abbreviation, so a `Promise<T>` reached through one is still a promise type and
   a reader that stops at the alias reports the member as returning something else. Bounded for the reason
   argtypegate.mjs states: §2.4 forbids a cycle and a corpus of a hundred specs is not a thing to trust on it. */
const typedefs = new Map();
for (const n of idl.declarations) if (n.type === "typedef" && n.name) typedefs.set(n.name, n.idlType);
function resolveTypedef(t, depth = 0) {
  if (!t || depth > 8) return t;
  if (typeof t.idlType === "string" && typedefs.has(t.idlType)) {
    const to = typedefs.get(t.idlType);
    return resolveTypedef({ ...to, nullable: to.nullable || t.nullable }, depth + 1);
  }
  return t;
}
/* §2.13 Promise types: `Promise<T>`. The PARAMETER is not read and must not be — §3.7.7's clause turns on the
   return type BEING a promise type and says nothing about what it resolves with, so `Promise<undefined>` is as
   much a promise return as `Promise<File>` is, and a reader that treated the first as an undefined return
   would exempt exactly the members whose name makes them look like they return nothing. */
const isPromise = (t) => { const r = resolveTypedef(t); return !!r && r.generic === "Promise"; };

const env = loadEnvironment(HOST);
const cFiles = [...env.sources.keys()].filter((p) => p.endsWith(".c"));

/* ---- the engine side: every pool declaration, keyed by the step id it is assigned to ---------------------- */
const declByLhs = new Map();          /* "file\0lhs" -> decl */
const declByLhsGlobal = new Map();    /* lhs -> [decl …] */
/* WHICH STEP IDS CAME FROM `JS_RegisterStepDef` INSTEAD. A member installed from one of these is not a pool
   member: it carries its own JSTrampStepDef and its completion never passes the pool's §3.7.7 conversion, so
   the absence of a declaration is not a defect there. Without this the OFF-POOL population is indistinguishable
   from the population whose install this reader merely could not read, and one number would be two answers. */
const runtimeStepid = new Map();      /* lhs -> {file, line} */
const stripped = new Map();
for (const path of cFiles) {
  const raw = env.sources.get(path).orig;
  const src = strip(raw);
  stripped.set(path, src);
  const { decls } = declarations(src, raw).read(C);
  for (const d of decls) {
    if (!d.lhs) continue;
    d.file = path; d.line = lineOf(src, d.at);
    declByLhs.set(`${path}\0${d.lhs}`, d);
    if (!declByLhsGlobal.has(d.lhs)) declByLhsGlobal.set(d.lhs, []);
    declByLhsGlobal.get(d.lhs).push(d);
  }
  for (const m of src.matchAll(/([A-Za-z_]\w*(?:\s*\[[^\]]*\])?)\s*=\s*JS_RegisterStepDef\s*\(/g))
    runtimeStepid.set(m[1].replace(/\s+/g, ""), { file: path, line: lineOf(src, m.index) });
}

/* The `stepid` expression of the install written at this file and line. The forms that take one are named by
   idl_args.h itself: idl_install_method / _unforgeable / _exposed put it at index 3, idl_install_step_method
   declares a `length` first and puts it at index 4. Read the same way argtypegate.mjs reads it, off the same
   stripped text, so the two audits joining the same install cannot disagree about which id it names. */
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

/* ---- the join ---------------------------------------------------------------------------------------------- */
const world = installedMembers(cFiles, env);
const tally = new Map();
const bump = (k) => tally.set(k, (tally.get(k) || 0) + 1);
const MISSING = [], EXTRA = [], OFFPOOL = [], UNJOINED = [], SPLIT = [], OK = [];

for (const rec of world.records) {
  if (!rec.ifaces || rec.ifaces.length !== 1) continue;   /* attribution is idlgen's question, not this one */
  const iface = rec.ifaces[0];
  if (!idl.byName.has(iface)) continue;
  const ops = idl.flatten(iface).filter((m) => m.type === "operation" && m.name === rec.name);
  if (!ops.length) continue;                              /* an absent-from-spec member is idlgen's finding */

  const at = `${relative(ROOT, rec.file)}:${rec.line}`;
  const who = `${iface}.${rec.name}`;
  bump("considered");

  const proms = ops.map((o) => isPromise(o.idlType));
  if (proms.some(Boolean) && !proms.every(Boolean)) {
    /* WHY THE REASON IS PART OF THE ROW. Two spellings reach here and they are not the same finding. A genuine
       §3.6 overload set is ONE identifier with several signatures on ONE property, so a declaration must state
       one answer for all of them and which one is a question about the algorithm. A STATIC member and a
       regular member sharing a name are TWO properties in two places — §3.7.7 puts a static operation on the
       interface object and a regular one on the interface prototype object — so they are two independent
       members that this reader cannot tell apart, because the install record names the member and not which
       object it landed on. Printing one reason for both would make the second look like the first. */
    const statics = ops.filter((o) => o.special === "static").map((o) => isPromise(o.idlType));
    const regulars = ops.filter((o) => o.special !== "static").map((o) => isPromise(o.idlType));
    const byLocation = statics.length && regulars.length
      && statics.every((v) => v === statics[0]) && regulars.every((v) => v === regulars[0])
      && statics[0] !== regulars[0];
    SPLIT.push({ who, at, n: ops.length, why: byLocation
      ? `a static member and a regular member share this name — two properties in two places, and this reader `
        + `cannot tell which object the install targeted`
      : `${ops.length} signatures of one §3.6 overload set disagree about being a promise type` });
    bump("NAME-SPLIT"); continue;
  }
  const specPromise = proms[0];

  const sid = stepidAt(rec.file, rec.line);
  if (!sid) {
    bump("UNJOINED");
    if (specPromise) UNJOINED.push({ who, at, why: "the install at this line names no step id this reader can "
      + "read — an accessor, a generated form, or a construct whose stepid argument is not written here" });
    continue;
  }
  if (runtimeStepid.has(sid.expr)) {
    const w = runtimeStepid.get(sid.expr);
    bump("OFF-POOL");
    if (specPromise) OFFPOOL.push({ who, at, expr: sid.expr, def: `${relative(ROOT, w.file)}:${w.line}` });
    continue;
  }
  let decl = declByLhs.get(`${rec.file}\0${sid.expr}`);
  if (!decl) {
    const g = declByLhsGlobal.get(sid.expr) || [];
    if (g.length === 1) decl = g[0];
    else if (g.length > 1) {
      bump("UNJOINED");
      if (specPromise) UNJOINED.push({ who, at, why: `${g.length} files assign \`${sid.expr}\`, so which `
        + "declaration this install names is ambiguous" });
      continue;
    }
  }
  if (!decl) {
    bump("UNJOINED");
    if (specPromise) UNJOINED.push({ who, at, why: `no pool declaration in scope assigns \`${sid.expr}\`, and `
      + "no JS_RegisterStepDef does either" });
    continue;
  }

  const declared = decl.mods.has("returns_promise");
  const where = `${relative(ROOT, decl.file)}:${decl.line}`;
  bump("joined");
  if (specPromise && !declared) {
    MISSING.push({ who, at, where, kind: decl.kind, ret: fmt(ops[0].idlType) }); bump("MISSING");
  } else if (!specPromise && declared) {
    EXTRA.push({ who, at, where, kind: decl.kind, ret: fmt(ops[0].idlType) }); bump("EXTRA");
  } else {
    if (specPromise) OK.push({ who, where, ret: fmt(ops[0].idlType) });
    bump(specPromise ? "CONFIRMED-promise" : "CONFIRMED-not-promise");
  }
}

function fmt(t) {
  if (!t) return "?";
  if (t.union) return "(" + (t.idlType || []).map(fmt).join(" or ") + ")";
  if (t.generic) return `${t.generic}<${(Array.isArray(t.idlType) ? t.idlType : [t.idlType]).map(fmt).join(", ")}>`;
  return (typeof t.idlType === "string" ? t.idlType : "?") + (t.nullable ? "?" : "");
}

/* ---- the report ---------------------------------------------------------------------------------------------
 * EVERY POPULATION IS PRINTED WITH ITS DENOMINATOR ON THE SAME LINE. CLAUDE.md: "a coverage figure states WHAT
 * IT IS A FRACTION OF, in the same line, or it is not a coverage figure" — and a count of accusations with no
 * count of what was asked is exactly the shape that reads as a clean bill when the join has collapsed. */
const n = (k) => tally.get(k) || 0;
const promiseJoined = n("MISSING") + n("CONFIRMED-promise");
console.log(`[rettype] installed operations resolved to one interface and found in the corpus: ${n("considered")}`);
console.log(`[rettype] of those, joined to a pool declaration: ${n("joined")}`
  + `  |  off-pool (JS_RegisterStepDef): ${n("OFF-POOL")}  |  unjoined: ${n("UNJOINED")}`
  + `  |  name-split: ${n("NAME-SPLIT")}`);
console.log(`[rettype] promise-returning among the joined: ${promiseJoined}`
  + `  —  declared: ${n("CONFIRMED-promise")}, MISSING the declaration: ${n("MISSING")}`);
console.log(`[rettype] non-promise among the joined: ${n("CONFIRMED-not-promise") + n("EXTRA")}`
  + `  —  correct: ${n("CONFIRMED-not-promise")}, EXTRA declaration: ${n("EXTRA")}`);

const band = (title, rows, render) => {
  console.log(`\n== ${title} (${rows.length})`);
  for (const r of rows.slice(0, CAP)) console.log("  " + render(r));
  if (rows.length > CAP) console.log(`  … ${rows.length - CAP} more; --all to print them`);
};

band("MISSING idl_returns_promise — §3.7.7 rejects, this member THROWS", MISSING,
  (r) => `${r.who.padEnd(46)} ${r.ret}\n      install ${r.at}\n      declared ${r.where} (idl_${r.kind})`);
band("EXTRA idl_returns_promise — the spec throws, this member REJECTS", EXTRA,
  (r) => `${r.who.padEnd(46)} spec returns ${r.ret}\n      install ${r.at}\n      declared ${r.where} (idl_${r.kind})`);
band("OFF-POOL promise members — own JSTrampStepDef, judged against nothing", OFFPOOL,
  (r) => `${r.who.padEnd(46)} stepid \`${r.expr}\` from JS_RegisterStepDef at ${r.def}\n      install ${r.at}`);
band("UNJOINED promise members — this audit's reach, not the engine's defect", UNJOINED,
  (r) => `${r.who.padEnd(46)} ${r.at}\n      ${r.why}`);
band("NAME-SPLIT — the corpus's members under this name disagree about being a promise type", SPLIT,
  (r) => `${r.who.padEnd(46)} ${r.at}\n      ${r.why}`);

if (CONFIRM)
  band("CONFIRMED promise members", OK, (r) => `${r.who.padEnd(46)} ${r.ret}  declared ${r.where}`);
