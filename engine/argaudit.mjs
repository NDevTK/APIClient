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
   (concolic_shape_c, or the component's own token_bytes), so merging them would name one remedy for two. */
const NUMERIC = /^JS_To(Int32|Uint32|Int64|Uint64|Float64|Number|Index|BigInt|BigInt64|BigUint64)$/;
const STRINGY = /^JS_To(CString|CStringLen|String|PropertyKey)$/;
const TOBOOL = /^JS_ToBool$/;

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

function auditFile(path, C, findings) {
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

  for (const m of src.matchAll(/\b(JS_To[A-Za-z0-9_]*)\s*\(/g)) {
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
    const crossing = [...types].filter((t) => C.crosses.has(t));
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
const findings = [];
for (const f of files) auditFile(f, C, findings);

console.log(`concolic-crossing argument audit — ${files.length} .c file(s)`);
console.log(`idl_concolic_rule places ${C.crosses.size} of ${C.all.size} declared types at CROSSES; `
  + `the ${C.notCrossing.size} that do not are ${[...C.notCrossing].join(", ")}\n`);

const ORDER = ["VIOLATION", "VIOLATION-STRING", "UNDECIDABLE", "GUARDED", "SAFE-TYPE", "TOBOOL"];
const JUDGED = new Set(["VIOLATION", "VIOLATION-STRING", "UNDECIDABLE"]);
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
