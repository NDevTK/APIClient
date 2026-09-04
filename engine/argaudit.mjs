/* THE CONCOLIC-CROSSING ARGUMENT AUDIT — the third axis, and the one nothing was asking.
 *
 *   node engine/argaudit.mjs                 audit engine/host/browser/core
 *   node engine/argaudit.mjs <path …>        audit those files/directories instead
 *   node engine/argaudit.mjs --all           print every finding rather than the first 200 of a kind
 *   node engine/argaudit.mjs --confirm       also print the sites that ARE guarded, and the counted-not-judged
 *
 * WHAT IT CHECKS. core/idl_args.h states the contract in capitals: "A BODY MAY NOT CALL JS_ToFloat64 ON ITS OWN
 * ARGUMENT." The reason is mechanical rather than stylistic. idl_concolic_rule answers IDL_CONCOLIC_CROSSES for
 * every numeric and every string type, so unknown external input reaches a member's body UNCONVERTED — that is
 * what makes opacity survive a coercion, which is the whole of why the solver can still fork on the value at a
 * later branch. A concolic is an Object, so JS_ToFloat64/JS_ToInt64/JS_ToInt32/JS_ToUint32/JS_ToNumber on one
 * reaches ECMAScript §7.1.1 ToPrimitive, which runs the page's own valueOf from a plain C activation with no
 * flow base under it — and this engine aborts THERE. The crash therefore lands inside the coercion, one line
 * shared by every caller in the tree, naming neither the member nor the argument: CLAUDE.md's "an assert that
 * names a remedy but not a site is a crash nobody can act on", with the site missing rather than wrong. What a
 * page loses is not an error message, it is the flow — `store.getAll(q, location.hash.length)` ended the
 * document instead of being explored.
 *
 * WHY AN AUDITOR AND NOT A DCHECK AT EACH SITE. A DCHECK at each coercion is N sites and a new one can forget,
 * which is the same reason idlgen.mjs exists rather than a comment per member. Making the coercion itself
 * refuse a concolic would be the root fix and it is not available here: those functions are quickjs's, inside
 * the submodule, so a change there is an ABI diff plus a gitlink bump — see CLAUDE.md's rule on cross-boundary
 * diffs before reaching for it. What IS available is the JOIN this tree already has the data for: a member
 * DECLARES its argument types beside its body, and idl_concolic_rule is the one function that says which of
 * those cross. Nothing was joining the two.
 *
 * THE SECOND QUESTION IT ASKS, AND THE ONE A SEARCH FOR A TERM CANNOT ASK. Everything above is about a term
 * that IS in the body — a coercion. An assert is wrong in the opposite direction: `DCHECK(JS_IsString(argv[0]))`
 * over a position the declaration declares DOMString is FALSE for exactly the input this engine exists to
 * explore, and it is wrong precisely because it FORGOT a term. A grep for `concolic` finds every site that
 * remembered and none that did not. So the ASSERT axis prices each `DCHECK`/`CHECK` condition over an `argv[K]`
 * whose declared type crosses: `ASSERT-NO-UNKNOWN` is a condition every one of whose priced terms is FALSE for
 * a concolic, which is an abort of the whole document on a line a real page can contain.
 * WHAT MAKES IT WORSE THAN A COERCION'S ABORT is what the crash SAYS. A coercion's `@WHY` names the conversion
 * boundary and the work; this one names a contract that HOLDS — the declaration really did convert every value
 * that is not unknown — so it reads as an engine bug rather than as an unbuilt fork. And where the body already
 * carries the honest `concolic_is` ask BELOW such an assert, the crash a reader actually gets is the one
 * describing the wrong problem, which is why the guard is scored EARLIER-IN-BODY and never merely present.
 *
 * IT IS ONE AXIS AND THE OTHERS ARE BLIND TO IT BY CONSTRUCTION. idlgen.mjs diffs the spec's member LIST
 * against what a component installs — a NAME check, silent about types. The arity audit compares COUNTS, and
 * its own lane wrote the verdict this file is named after: "it compares COUNTS and this is a TYPE, precisely
 * the blindness that let `remove` survive a `length` audit." argtypegate.mjs asks whether the DECLARATION
 * itself says what the spec's IDL says, which this one takes as GIVEN — a declaration that is wrong about the
 * spec is internally consistent here and reads clean. This one asks only whether a BODY re-converts what its
 * own declaration already converted. Four auditors, four axes; none of them is the gate.
 *
 * WHAT A DECLARATION SAYS IS READ BY engine/idl_argdecl.mjs, which argtypegate.mjs reads too — one parse of
 * `idl_method_id*`, because a second copy of it is the copy that drifts.
 *
 * IT IS NOT A BUILD GATE, DELIBERATELY, for citegen.mjs's reason: it reads SOURCE TEXT, so its recall is
 * bounded by what text can say, and a checker that fails a lane for a finding the lane did not introduce gets
 * muted exactly as fast as one that cries wolf. It prints; the human decides. It exits 0.
 *
 * ITS BLIND SPOTS ARE PART OF THE INSTRUCTION, because a checker trusted past its evidence is worse than none:
 *
 *   - IT READS TEXT, NOT A PARSE. A coercion whose argument was first copied into a local
 *     (`JSValueConst v = argv[0]; JS_ToFloat64(ctx, &d, v);`) is INVISIBLE to it. So is one reached through a
 *     helper that takes `argv` — those are reported as UNDECIDABLE rather than judged, and there are real ones
 *     (core/events/mouse_event.c's md_arg_i32/md_arg_u32 take the whole vector and an index).
 *   - A BODY SHARED BY SEVERAL DECLARATIONS IS JUDGED AGAINST THE UNION of their type lists, because `magic`
 *     picks among them and text cannot follow a magic. That OVER-reports: core/dom/range.c's js_range_member
 *     has IDL_UNSIGNED_LONG at position 1 under one declaration and IDL_INTERFACE under another. A union
 *     finding is still a real finding for the declaration that crosses, but the reader must check WHICH.
 *   - IT KNOWS NOTHING ABOUT REACHABILITY. A body whose argument no page can supply — an engine-internal door
 *     such as core/timing/timer.c's substep 9.11 re-arm, whose `previousId` this engine writes itself — is a
 *     finding here and is not a defect. Reachability is a fact about the caller, and text has no caller.
 *   - `JS_ToBool` IS COUNTED AND NEVER JUDGED. idl_args.h states it: ToBoolean "runs nothing", so it does not
 *     reach ToPrimitive and cannot abort. That an unknown is silently TRUE there is a different defect (a
 *     branch decided rather than forked) belonging to a different audit; calling it this one's would be the
 *     bucket collapse that makes a number unreadable.
 *   - A GUARD IS RECOGNISED BY `concolic_is(argv[K])` APPEARING EARLIER IN THE SAME BODY. It does not verify
 *     that the guard's own arm is correct, only that the question was asked. A guard that asks and then answers
 *     wrong reads here as clean.
 *   - IT SCANS ONE TREE. A member declared outside the audited paths is not seen at all.
 *
 * AND THE ASSERT AXIS HAS THREE OF ITS OWN, EVERY ONE OF WHICH SENDS A SITE TO `ASSERT-UNDECIDABLE` RATHER
 * THAN TO A VERDICT — an unpriced term is never scored clean, because "this tool did not understand it" and
 * "this asks nothing about the type" are two facts and a default is what averages them:
 *   - IT PRICES FOUR SPELLINGS AND NO OTHERS (see conditionTerms). An assert reaching the slot through a local
 *     alias — `JSValueConst names = argv[0]; … DCHECK(JS_IsArray(names))`, which core/indexeddb/idb_connection.c
 *     really writes — mentions no `argv[` at all and is outside the frame entirely, not undecidable within it.
 *   - IT DOES NOT READ THE CONNECTIVE. A condition mixing a term that admits a concolic with one that refuses
 *     one means different things under `&&` and `||`, so it is `ASSERT-MIXED` and read by hand, never scored.
 *   - THE GUARD TEST IS TEXTUAL AND POSITIONAL. `concolic_is(argv[K])` anywhere earlier in the same body counts,
 *     whether or not its arm is right; one LATER does not count at all, which is the whole point of it.
 *
 * WHAT TO DO WITH A VIOLATION. Not a guard that returns early — that is the flow being dropped with a different
 * spelling, which CLAUDE.md's razor calls a cap. Two answers exist in the tree already, and which one applies is
 * decided by what the member does with the number:
 *   - THE NUMBER: idl_args.h's `idl_number_of`, which answers the converted Number for a real value and, for an
 *     unknown, the SAME §3.2 conversion run on that value's own EXAMPLE — so a config-loaded count keeps the
 *     flow running on the number the app actually computed while the value itself stays unknown. Where the
 *     member's own algorithm then BRANCHES on that number, the branch is a `step_fork_run` and not an `if`;
 *     core/timing/timer.c does exactly this over HTML §8.7's step 4.
 *   - THE COLLECTION INDEX: `concolic_is(argv[K])` and then the answer that holds over the WHOLE domain, with a
 *     DCHECK naming the fork that must be built where it does not. core/dom/collections.c, core/dom/attr.c and
 *     core/css/style_sheet_list.c carry the shape.
 */
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { contract, declarations, strip, args, callText, functions, lineOf } from "./idl_argdecl.mjs";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const CORE = join(ROOT, "engine/host/browser/core");

/* THE COERCIONS THAT REACH ToPrimitive, split by what they run. The two lists are separate because the failure
   is the same and the CURE is not: a number's is idl_number_of, a string's is the shape the value carries
   (concolic_shape_c, or the component's own token_bytes), so merging them would name one remedy for two.
   `JS_ValueToAtom` IS IN THE STRING LIST AND ITS NAME IS WHY IT WAS NOT. Every other spelling here begins
   `JS_To`, and the scan below used to be anchored on that prefix, so the one coercion in quickjs.h whose name
   puts the verb in the MIDDLE was outside the frame — not judged clean, never looked at. It is a full
   ToPropertyKey: JS_ValueToAtomInternalAt sends anything that is not an int or a Symbol to
   JS_ToPropertyKeyInternal, which is JS_ToStringInternal, which is the abort. `console.time(location.hash)`
   ends the document at exactly such a call and this audit reported nothing about it. */
const NUMERIC = /^JS_To(Int32|Uint32|Int64|Uint64|Float64|Number|Index|BigInt|BigInt64|BigUint64)$/;
const STRINGY = /^(JS_To(CString|CStringLen|String|PropertyKey)|JS_ValueToAtom)$/;
const TOBOOL = /^JS_ToBool$/;
/* The scan's own alphabet: every spelling either list can name. It is derived from the two regexps rather than
   typed a third time — a name added above that this misses is a coercion nothing looks at, which is the defect
   `JS_ValueToAtom` just was. */
const COERCION = /\b(JS_To[A-Za-z0-9_]*|JS_ValueToAtom)\s*\(/g;

/* ---- the join ---------------------------------------------------------------------------------------------
 *
 * A member's declared types reach a body three ways and all three are read: a named `static const IdlArgType`
 * array, an inline compound literal, and a setter's single type. `idl_method_id_step` names an IdlStepDecl
 * instead of a body, and that struct's FIRST field is the step function — so the decl table is read too, and a
 * step machine's argv is audited exactly as a plain body's is. */
function declaredTypes(src, raw, C) {
  /* body name → { pos → Set(type) }, unioned over every declaration that names it — `magic` picks among them
     and text cannot follow a magic, so a shared body is judged against the union and the reader must check
     WHICH declaration a finding belongs to. */
  const decl = new Map();
  for (const d of declarations(src, raw).read(C).decls) {
    if (!d.body || !d.types) continue;
    let m = decl.get(d.body);
    if (!m) decl.set(d.body, (m = new Map()));
    d.types.forEach((t, i) => { if (!m.has(i)) m.set(i, new Set()); m.get(i).add(t); });
    if (d.variadic && d.types.length) m.set("tail", new Set([d.types[d.types.length - 1]]));
  }
  return decl;
}

/* ---- the assert axis --------------------------------------------------------------------------------------
 *
 * THE ONE TYPE REWRITE THAT HAPPENS BEFORE THE CROSSING TEST, DERIVED FROM THE CONVERSION ITSELF — and it
 * changes a verdict, which is why it is read rather than assumed away. idl_args.c collapses a nullable
 * interface to its un-nullable type ABOVE the `idl_concolic_rule(t) == IDL_CONCOLIC_CROSSES` line, once the
 * value has been established not to be null: so a concolic at an `IDL_INTERFACE_NULLABLE` position is asked the
 * rule for IDL_INTERFACE, which is UNASKED, and reaches Web IDL §3.2.15's brand test and its TypeError instead
 * of the body. Joining on the DECLARED type alone reports such a site as an abort a page can reach, and it
 * cannot: the member never runs. `DOMNode.insertBefore(n, location.hash)` is exactly that shape.
 * IT THROWS RATHER THAN FALLING BACK, for concolicAdmits's reason — a join whose premise has moved has no
 * findings to offer, and reverting to the declared type would silently restore the over-report. */
function preCrossingRewrite(idlArgsCPath) {
  const src = strip(readFileSync(idlArgsCPath, "utf8"));
  const cross = src.indexOf("idl_concolic_rule(t) == IDL_CONCOLIC_CROSSES");
  if (cross < 0) throw new Error("idl_args.c no longer tests idl_concolic_rule(t) == IDL_CONCOLIC_CROSSES in "
    + "the argument conversion — this audit joins a declaration against exactly that test, so it cannot say "
    + "which declared types reach a body as themselves until the test is found again");
  const map = new Map();
  for (const m of src.slice(0, cross)
      .matchAll(/\bt\s*=\s*\(\s*t\s*==\s*(IDL_[A-Z0-9_]+)\s*\)\s*\?\s*(IDL_[A-Z0-9_]+)\s*:\s*(IDL_[A-Z0-9_]+)\s*;/g)) {
    map.set(m[1], m[2]);
    /* The `:` arm belongs to whichever OTHER type the enclosing guard admitted; the guard is the only place
       that names it, so it is read from there rather than guessed. */
    const guard = src.slice(0, m.index).lastIndexOf("if (t ==");
    const names = [...src.slice(guard, m.index).matchAll(/t\s*==\s*(IDL_[A-Z0-9_]+)/g)].map((g) => g[1]);
    for (const n of names) if (n !== m[1] && !map.has(n)) map.set(n, m[3]);
  }
  /* THE SECOND PRE-CROSSING REWRITE, AND IT IS NOT A TERNARY — Web IDL §3.6 Overload resolution algorithm
     steps 3-4 remove the shorter overload entry from the ARGUMENT COUNT alone, and the conversion performs
     that as `if (step4_only_longer) t = idl_split_longer_type(t);`, also above the crossing test. The regex
     above cannot see it, so a length-splitting union read as its DECLARED type answers whatever rule that row
     carries, when what a body at the longer arity actually receives is the longer entry's own type: a
     concolic at `postMessage(m, x, [])` reaches the USVString, which CROSSES, and one at `el.scrollTo(x, y)`
     reaches the unrestricted double, which crosses too.
     IT IS READ FROM THE FUNCTION THAT OWNS THE ANSWER rather than listed here — that function is a total
     switch over the split rows and crashes for anything else, so a row added to it is a row this audit learns
     about on the day it lands, and a hand list here would be the second copy this file exists to catch. The
     rewrite is CONDITIONAL on arity where the nullable collapse is conditional on the value, so joining it
     unconditionally is the over-reporting direction: a body IS reachable with the crossed value at the longer
     arity, and a fork at the shorter one can place the unknown as itself too. */
  {
    const fn = src.match(/idl_split_longer_type\(IdlArgType t\)\s*\{([\s\S]*?)\n\}/);
    if (!fn) throw new Error("idl_args.c no longer defines idl_split_longer_type — that function IS the "
      + "statement of which type §3.6 steps 3-4 leave standing at a length-differing split, so without it "
      + "this audit cannot say what a body at the longer arity receives");
    for (const m of fn[1].matchAll(/case\s+(IDL_[A-Z0-9_]+)\s*:\s*return\s+(IDL_[A-Z0-9_]+)\s*;/g))
      map.set(m[1], m[2]);
  }
  if (!map.size) throw new Error("idl_args.c's argument conversion no longer rewrites any declared type above "
    + "its crossing test — that rewrite is what makes a nullable interface answer IDL_INTERFACE's rule, so if "
    + "it is gone the join below is measuring a conversion this tree no longer performs");
  return map;
}

/* WHICH POSITIVE TYPE TESTS A CONCOLIC SATISFIES, DERIVED FROM THE FILE THAT MINTS ONE — and it is NOT only
 * "an Object", which is the half that decides three of this axis's findings.
 *   - IT IS AN OBJECT, from the mint: JS_NewObjectClass(ctx, g_concolic_class). So JS_IsObject is true for one
 *     and JS_VALUE_GET_TAG answers JS_TAG_OBJECT.
 *   - IT IS ALSO A FUNCTION, WHENEVER ITS CLASS INSTALLS `.call` — and it does, because a concolic must answer
 *     `document.cookie.indexOf("role=admin")` with another unknown instead of throwing "not a function" and
 *     taking the rest of the program with it. quickjs's JS_IsFunction ends in
 *     `class_array[p->class_id].call != NULL`, so it answers TRUE for a concolic. THAT MAKES EVERY
 *     `DCHECK(JS_IsFunction(ctx, argv[0]))` OVER AN IDL_CALLBACK POSITION CORRECT AS WRITTEN: Web IDL §3.2.19
 *     Callback function types asks "If Type(V) is not Object, or IsCallable(V) is false, throw a TypeError",
 *     a concolic passes both clauses, the value converts, the body runs and the assert holds. Priced as if
 *     JS_IsObject were the only admitting brand, this audit reports three aborts that cannot happen — which is
 *     the crying-wolf failure its own header says gets a checker muted.
 * IT IS READ AND NEVER RESTATED, and it THROWS where it cannot establish either fact: a checker that cannot
 * state its own premise has no findings to offer, and silence would read as a pass. */
function concolicAdmits(concolicPath) {
  const src = strip(readFileSync(concolicPath, "utf8"));
  if (!/JS_NewObjectClass\s*\(\s*ctx\s*,\s*g_concolic_class\s*\)/.test(src))
    throw new Error("solver/concolic.c no longer mints its value with JS_NewObjectClass(ctx, g_concolic_class) "
      + "— the assert axis rests entirely on a concolic wearing JS_TAG_OBJECT and on which brands it carries, "
      + "so it cannot classify one assert until that fact is re-established from whatever mints one now");
  const def = src.match(/JSClassDef\s+def\s*=\s*\{([\s\S]*?)\}\s*;/);
  if (!def)
    throw new Error("solver/concolic.c no longer declares a `JSClassDef def = { … };` for the concolic class — "
      + "which type tests a concolic satisfies is derived from that initializer, and guessing it is how an "
      + "audit comes to report an abort at a site whose assert is correct");
  const admits = new Set(["Object"]);
  if (/\.\s*call\s*=/.test(def[1])) admits.add("Function");
  return admits;
}

/* THE TERMS AN ASSERT'S CONDITION CAN STATE ABOUT ONE ARGUMENT SLOT, and what each is worth over an unknown.
 * `admits` means TRUE for a concolic, `refuses` means FALSE for one. There are only four spellings in this
 * tree and each is here with its arithmetic:
 *   concolic_is(argv[K])                        — the question itself
 *   JS_Is<T>(… argv[K])                         — priced against concolicAdmits's DERIVED brand set
 *   JS_VALUE_GET_TAG(argv[K]) ==/!= JS_TAG_<T>  — the same fact spelled at the tag
 *   JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(argv[K]))
 * An occurrence of `argv[K]` inside the condition matching NONE of them is an UNMODELLED term and makes the
 * whole assert undecidable — never clean, because "this tool did not understand it" and "this asks nothing
 * about the type" are two facts and averaging them is what a default does. */
function conditionTerms(cond, pos, ADMITS) {
  const A = `argv\\s*\\[\\s*${pos}\\s*\\]`;
  const terms = [];
  const seen = [];
  let asked = 0, refusesAsked = 0;
  const push = (m, verdict) => { terms.push(verdict); seen.push([m.index, m.index + m[0].length]); };

  /* THE ONE SPELLING THAT IS THE QUESTION RATHER THAN A TYPE TEST, counted apart from the rest — because
     `DCHECK(!concolic_is(argv[0]), "… Build concolic bytes in a typed array's backing store")` also aborts on a
     page-supplied unknown, and it is a DIFFERENT FACT: that site ASKED and named the capability it lacks, which
     is what CLAUDE.md's §NAMED RESIDUAL and §DFAIL prescribe. Scoring the two together would put a correct
     crash and a forgotten term under one number, which is the bucket collapse this file's own ToBool note
     refuses to make. */
  for (const m of cond.matchAll(new RegExp(`(!\\s*)?\\bconcolic_is\\s*\\(\\s*${A}\\s*\\)`, "g"))) {
    asked++;
    if (m[1]) refusesAsked++;
    push(m, m[1] ? "refuses" : "admits");
  }
  for (const m of cond.matchAll(new RegExp(`(!\\s*)?\\bJS_Is([A-Za-z0-9_]+)\\s*\\(\\s*(?:ctx\\s*,\\s*)?${A}\\s*\\)`, "g")))
    push(m, ADMITS.has(m[2]) === !m[1] ? "admits" : "refuses");
  for (const m of cond.matchAll(new RegExp(`\\bJS_VALUE_GET_TAG\\s*\\(\\s*${A}\\s*\\)\\s*(==|!=)\\s*(JS_TAG_[A-Z0-9_]+)`, "g")))
    push(m, (m[2] === "JS_TAG_OBJECT") === (m[1] === "==") ? "admits" : "refuses");
  for (const m of cond.matchAll(new RegExp(`(!\\s*)?\\bJS_TAG_IS_FLOAT64\\s*\\(\\s*JS_VALUE_GET_TAG\\s*\\(\\s*${A}\\s*\\)\\s*\\)`, "g")))
    push(m, m[1] ? "admits" : "refuses");

  /* Every mention of the slot must be inside one of the spans above, or the condition says something about it
     this tool cannot price. A tag read that FEEDS one of the four is already inside its span. */
  let unmodelled = 0;
  for (const m of cond.matchAll(new RegExp(A, "g")))
    if (!seen.some(([a, b]) => m.index >= a && m.index + m[0].length <= b)) unmodelled++;
  return { admits: terms.filter((t) => t === "admits").length,
           refuses: terms.filter((t) => t === "refuses").length,
           asked, refusesAsked, unmodelled, any: terms.length };
}

/* THE JOIN, IN ONE PLACE FOR BOTH AXES: what the conversion asks of a concolic at a position DECLARED `t`,
   which is the rule for `t` after the pre-crossing rewrite and not for `t` as written. */
const crossesAfterRewrite = (C, R, t) => C.crosses.has(R.get(t) || t);

function auditFile(path, C, R, ADMITS, findings) {
  const raw = readFileSync(path, "utf8");
  const src = strip(raw);
  const rel = relative(ROOT, path);
  const decl = declaredTypes(src, raw, C);
  const fns = functions(src);

  /* Which function each offset is inside — the innermost definition containing it. */
  const owner = (off) => {
    let best = null;
    for (const [name, f] of fns) if (off > f.from && off < f.to && (!best || f.from > fns.get(best).from)) best = name;
    return best;
  };

  /* THE THIRD AXIS. The two above search for a term that IS there — a coercion, a declared type — and both are
     structurally blind to an assert that is wrong precisely because it FORGOT one. A body whose declaration
     crosses receives an unknown as itself, so `DCHECK(JS_IsString(argv[0]))` over an IDL_DOMSTRING position is
     false for exactly the input this engine exists to explore, and the abort takes the whole document with it.
     It is worse than a coercion's abort in one way: a coercion's `@WHY` names the conversion boundary and the
     work, while this one names a contract that HOLDS — the declaration really did convert every value that is
     not unknown — so it reads as an engine bug rather than as an unbuilt fork.
     WORSE STILL WHERE THE HONEST CRASH IS ALREADY WRITTEN. A body can carry the correct `concolic_is` ask,
     naming the fork that is owed, BELOW a positive type assert that refuses the same value — and then the
     crash a reader gets is the one that describes the wrong problem. That is why the guard must be EARLIER in
     the body and never merely present in it. */
  for (const m of src.matchAll(/\b(DCHECK|DCHECKF|CHECK|CHECKF)\s*\(/g)) {
    const macro = m[1];
    const { text } = callText(src, m.index + m[0].length - 1);
    const cond = args(text)[0];
    if (!cond || !/\bargv\s*\[/.test(cond)) continue;
    const line = lineOf(src, m.index);
    const fn = owner(m.index);
    const at = `${rel}:${line}`;
    const d = fn ? decl.get(fn) : null;

    for (const im of new Set([...cond.matchAll(/\bargv\s*\[\s*([^\]]*?)\s*\]/g)].map((x) => x[1].trim()))) {
      const site = `${fn || "<file scope>"} → ${macro}(… argv[${im}] …)`;
      if (!/^\d+$/.test(im)) {
        findings.push({ kind: "ASSERT-UNDECIDABLE", at, site,
          why: "the index is not a literal, so which declared type this position carries cannot be read from text" });
        continue;
      }
      const pos = Number(im);
      if (!d) {
        findings.push({ kind: "ASSERT-UNDECIDABLE", at, site,
          why: "no idl_method_id* declaration in this file names this function as a body, so what its "
            + "declaration does with unknown external input is not in hand" });
        continue;
      }
      const types = d.get(pos) || d.get("tail");
      if (!types) {
        findings.push({ kind: "ASSERT-UNDECIDABLE", at, site,
          why: `position ${pos} is past every declaration's type list` });
        continue;
      }
      const t = conditionTerms(cond, pos, ADMITS);
      if (t.unmodelled || !t.any) {
        findings.push({ kind: "ASSERT-UNDECIDABLE", at, site,
          why: `the condition mentions this slot in a shape this tool does not price (${t.unmodelled} such `
            + `mention(s), ${t.any} priced term(s))` });
        continue;
      }
      if (!t.refuses) { findings.push({ kind: "ASSERT-ADMITS", at, site, why: "every priced term over this slot is true for a concolic" }); continue; }
      if (t.admits) {
        findings.push({ kind: "ASSERT-MIXED", at, site,
          why: `${t.admits} term(s) admit a concolic and ${t.refuses} refuse one — which the condition means `
            + "depends on the connective between them, so this is read by hand and never scored" });
        continue;
      }
      if (t.refusesAsked === t.refuses) {
        findings.push({ kind: "ASSERT-REFUSES-KNOWINGLY", at, site,
          why: "this assert refuses a concolic through `!concolic_is` and nothing else — it ASKED the question, "
            + "so it is a named capability gap and not a forgotten term. It still aborts the document on a "
            + "page-supplied unknown; what it owes is that its message name what to build and how its absence "
            + "would show, which this axis cannot read" });
        continue;
      }
      if (![...types].some((x) => crossesAfterRewrite(C, R, x))) {
        findings.push({ kind: "ASSERT-SAFE-TYPE", at, site,
          why: `declared ${[...types].join("/")} — idl_concolic_rule does not answer CROSSES for it (after the `
            + "conversion's own pre-crossing rewrite), so no unknown reaches this slot as itself" });
        continue;
      }
      const guarded = new RegExp(`concolic_is\\s*\\(\\s*argv\\s*\\[\\s*${pos}\\s*\\]`).test(src.slice(fns.get(fn).from, m.index));
      findings.push({ kind: guarded ? "ASSERT-GUARDED" : "ASSERT-NO-UNKNOWN", at, site,
        why: `declared ${[...types].join("/")} — idl_concolic_rule answers CROSSES, so unknown external input `
          + `reaches this body as itself and every priced term of this condition is FALSE for it`
          + (guarded ? ", but an earlier concolic_is over this slot already asks the question" : "") });
    }
  }

  for (const m of src.matchAll(COERCION)) {
    const fname = m[1];
    const kind = NUMERIC.test(fname) ? "num" : STRINGY.test(fname) ? "str" : TOBOOL.test(fname) ? "bool" : null;
    if (!kind) continue;
    const { text } = callText(src, m.index + m[0].length - 1);
    const a = args(text);
    const hit = a.map((x, i) => [x, i]).find(([x]) => /\bargv\s*\[/.test(x));
    if (!hit) continue;
    const idx = hit[0].match(/\bargv\s*\[\s*([^\]]*)\s*\]/);
    const line = lineOf(src, m.index);
    const fn = owner(m.index);
    const at = `${rel}:${line}`;
    const site = `${fn || "<file scope>"} → ${fname}(argv[${idx ? idx[1].trim() : "?"}])`;

    if (kind === "bool") { findings.push({ kind: "TOBOOL", at, site, why: "ToBoolean runs nothing — counted, never judged" }); continue; }
    if (!idx || !/^\d+$/.test(idx[1].trim())) {
      findings.push({ kind: "UNDECIDABLE", at, site,
        why: "the index is not a literal, so which declared type this position carries cannot be read from text" });
      continue;
    }
    const pos = Number(idx[1].trim());
    const d = fn ? decl.get(fn) : null;
    if (!d) {
      findings.push({ kind: "UNDECIDABLE", at, site,
        why: fn && /JSValueConst\s*\*\s*argv/.test(fns.get(fn).params)
          ? "this function takes an argv vector but no declaration in this file names it as a body — a helper "
            + "reached from one, or a member registered elsewhere; its declared types are not in hand"
          : "no idl_method_id* declaration in this file names this function as a body" });
      continue;
    }
    const types = d.get(pos) || d.get("tail");
    if (!types) {
      findings.push({ kind: "UNDECIDABLE", at, site,
        why: `position ${pos} is past every declaration's type list — an undeclared position is IDL_ANY, which `
          + "does not cross, but a body reading past its own arity is its own question" });
      continue;
    }
    const crossing = [...types].filter((t) => crossesAfterRewrite(C, R, t));
    const unknown = [...types].filter((t) => !C.all.has(t));
    if (unknown.length) {
      findings.push({ kind: "UNDECIDABLE", at, site, why: `declared type ${unknown.join("/")} is not a member of IdlArgType` });
      continue;
    }
    if (!crossing.length) {
      findings.push({ kind: "SAFE-TYPE", at, site, why: `declared ${[...types].join("/")} — idl_concolic_rule does not answer CROSSES` });
      continue;
    }
    const guarded = new RegExp(`concolic_is\\s*\\(\\s*argv\\s*\\[\\s*${pos}\\s*\\]`).test(src.slice(fns.get(fn).from, m.index));
    findings.push({
      kind: guarded ? "GUARDED" : (kind === "num" ? "VIOLATION" : "VIOLATION-STRING"),
      at, site,
      why: `declared ${[...types].join("/")} — idl_concolic_rule answers CROSSES, so unknown external input `
        + `reaches this body unconverted and ${kind === "num" ? "ToNumber" : "ToString"} on it reaches ToPrimitive`,
    });
  }
}

function walk(p, out) {
  const st = statSync(p);
  if (st.isDirectory()) { for (const e of readdirSync(p)) walk(join(p, e), out); return; }
  if (/\.c$/.test(p)) out.push(p);
}

const argv = process.argv.slice(2);
const all = argv.includes("--all");
const confirm = argv.includes("--confirm");
const paths = argv.filter((a) => !a.startsWith("--"));
const files = [];
for (const p of (paths.length ? paths.map((p) => resolve(ROOT, p)) : [CORE])) walk(p, files);
files.sort();

const C = contract(join(CORE, "idl_args.h"));
const ADMITS = concolicAdmits(join(ROOT, "engine/host/solver/concolic.c"));
const R = preCrossingRewrite(join(CORE, "idl_args.c"));
const findings = [];
for (const f of files) auditFile(f, C, R, ADMITS, findings);

console.log(`concolic-crossing argument audit — ${files.length} .c file(s)`);
console.log(`idl_concolic_rule places ${C.crosses.size} of ${C.all.size} declared types at CROSSES; `
  + `the ${C.notCrossing.size} that do not are ${[...C.notCrossing].join(", ")}\n`);

const ORDER = ["VIOLATION", "VIOLATION-STRING", "ASSERT-NO-UNKNOWN", "ASSERT-MIXED", "ASSERT-REFUSES-KNOWINGLY", "UNDECIDABLE",
               "ASSERT-UNDECIDABLE", "GUARDED", "ASSERT-GUARDED", "SAFE-TYPE", "ASSERT-SAFE-TYPE",
               "ASSERT-ADMITS", "TOBOOL"];
const JUDGED = new Set(["VIOLATION", "VIOLATION-STRING", "ASSERT-NO-UNKNOWN", "ASSERT-MIXED", "UNDECIDABLE"]);
for (const kind of ORDER) {
  const rows = findings.filter((f) => f.kind === kind);
  if (!rows.length) continue;
  if (!JUDGED.has(kind) && !confirm) { console.log(`${kind}: ${rows.length}   (--confirm to list)`); continue; }
  console.log(`\n${kind}: ${rows.length}`);
  for (const f of (all ? rows : rows.slice(0, 200))) {
    console.log(`  ${f.at}`);
    console.log(`      ${f.site}`);
    console.log(`      ${f.why}`);
  }
  if (!all && rows.length > 200) console.log(`  … ${rows.length - 200} more (--all)`);
}
console.log(`\nThis audit REPORTS and exits 0 — see the header for why, and for what it cannot see.`);
