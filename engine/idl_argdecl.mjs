/* WHAT A MEMBER DECLARES ABOUT ITS ARGUMENTS, read once out of the C — the shared half of two audits that ask
 * different questions of the same declaration.
 *
 * There are three axes over an IDL member and each is blind where the others look. idlgen.mjs asks about
 * NAMES: which members the spec lists that no component installs. argaudit.mjs asks whether a BODY re-converts
 * an argument whose declared type crosses concolically. argtypegate.mjs asks whether the DECLARATION ITSELF
 * says what the spec's IDL says. The first needs nothing from here; the second and third both need the same
 * parse of `idl_method_id*` — and a second copy of that parse is the defect CLAUDE.md names by construction,
 * because the copy that drifts is the one nobody runs against reality. So the parse lives here once.
 *
 * IT READS TEXT AND NOT A PARSE, and everything downstream inherits that: an expression it cannot evaluate is
 * reported by its consumers rather than assumed. The one thing it will not do is guess — a types list it
 * cannot resolve comes back null, and a `nargs` that is not a literal comes back null, so a consumer that
 * needs a number has to say what it does without one.
 */
import { readFileSync } from "node:fs";

/* Comments and string literals blanked, LINE NUMBERS PRESERVED — a citation lives inside a DCHECK message and
   a coercion is named in idl_args.h's own contract paragraph, so scanning raw text counts prose as code. */
export function strip(src) {
  let out = "", i = 0, n = src.length;
  while (i < n) {
    const c = src[i], d = src[i + 1];
    if (c === "/" && d === "*") { const e = src.indexOf("*/", i + 2); const seg = src.slice(i, e < 0 ? n : e + 2);
      out += seg.replace(/[^\n]/g, " "); i = e < 0 ? n : e + 2; continue; }
    if (c === "/" && d === "/") { const e = src.indexOf("\n", i); const seg = src.slice(i, e < 0 ? n : e);
      out += seg.replace(/[^\n]/g, " "); i = e < 0 ? n : e; continue; }
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c) { if (src[j] === "\\") j++; j++; }
      out += src.slice(i, Math.min(j + 1, n)).replace(/[^\n]/g, " "); i = j + 1; continue;
    }
    out += c; i++;
  }
  return out;
}

export const lineOf = (src, off) => src.slice(0, off).split("\n").length;

/* Split a call's argument text on TOP-LEVEL commas — a type list, a nested call and a compound literal all
   contain commas that are not argument separators. */
export function args(text) {
  const out = []; let d = 0, cur = "";
  for (const ch of text) {
    if (ch === "(" || ch === "[" || ch === "{") d++;
    else if (ch === ")" || ch === "]" || ch === "}") d--;
    if (ch === "," && d === 0) { out.push(cur.trim()); cur = ""; continue; }
    cur += ch;
  }
  if (cur.trim()) out.push(cur.trim());
  return out;
}

/* The text between the `(` at `open` and its matching `)`. */
export function callText(src, open) {
  let d = 0, i = open;
  for (; i < src.length; i++) {
    if (src[i] === "(") d++;
    else if (src[i] === ")") { d--; if (!d) break; }
  }
  return { text: src.slice(open + 1, i), end: i };
}

/* Every function DEFINITION in a file, as name → { from, to }. A definition is a signature at column 0 whose
   parameter list is followed by `{`; a prototype ends in `;` and is skipped by that same test. */
export function functions(src) {
  const out = new Map();
  const re = /\n([A-Za-z_][A-Za-z_0-9 \t*]*?)\b([A-Za-z_][A-Za-z_0-9]*)\s*\(/g;
  let m;
  while ((m = re.exec(src))) {
    const open = m.index + m[0].length - 1;
    if (/^\s*(if|for|while|switch|return|sizeof)\s*$/.test(m[2])) continue;
    const { end } = callText(src, open);
    let j = end + 1;
    while (j < src.length && /\s/.test(src[j])) j++;
    if (src[j] !== "{") continue;
    let d = 0, k = j;
    for (; k < src.length; k++) { if (src[k] === "{") d++; else if (src[k] === "}") { d--; if (!d) break; } }
    out.set(m[2], { from: j, to: k, params: src.slice(open + 1, end) });
    re.lastIndex = k;
  }
  return out;
}

/* ---- the contract, read out of idl_args.h and never restated ----------------------------------------------
 *
 * The declared types are the enum's own members and the set that CROSSES is derived from idl_concolic_rule's
 * own switch. idl_args.h's comment names why: it had TWO statements of this and they DISAGREED, and "a
 * hand-maintained mirror of a hand-maintained list is the defect, not either list". A third copy in a checker
 * would be the same defect with a checker's authority behind it — so this reads the function, and a type it
 * cannot place is a finding for the consumer rather than an assumption here. */
export function contract(idlArgsHeaderPath) {
  const src = readFileSync(idlArgsHeaderPath, "utf8");
  const all = new Set();
  const enumBody = src.match(/typedef enum \{([\s\S]*?)\} IdlArgType;/);
  if (!enumBody) throw new Error("idl_args.h no longer declares `typedef enum { … } IdlArgType;` — the audits "
    + "read the declared types out of that enum, so they cannot run until it is found again");
  for (const m of strip(enumBody[1]).matchAll(/\b(IDL_[A-Z0-9_]+)\b/g)) all.add(m[1]);

  const fn = src.match(/idl_concolic_rule\(IdlArgType t\)\s*\{([\s\S]*?)\n\}/);
  if (!fn) throw new Error("idl_args.h no longer defines idl_concolic_rule — that function IS the discriminator "
    + "the crossing audit joins declarations against, so there is nothing to check without it");
  const body = strip(fn[1]);
  const notCrossing = new Set();
  let pending = [];
  for (const line of body.split("\n")) {
    const c = line.match(/case\s+(IDL_[A-Z0-9_]+)\s*:/);
    if (c) { pending.push(c[1]); continue; }
    const r = line.match(/return\s+(IDL_CONCOLIC_[A-Z]+)\s*;/);
    if (r) {
      if (r[1] !== "IDL_CONCOLIC_CROSSES") for (const t of pending) notCrossing.add(t);
      pending = [];
    }
  }
  const crosses = new Set([...all].filter((t) => !notCrossing.has(t)));

  /* THE POST-DECLARATION MODIFIERS, DERIVED AND NOT LISTED. Each of them "names the member the LAST
     declaration made" and each is declared in this header as a `void idl_x(…)` taking no JSContext — a
     modifier acts on the pool's last entry, so it has no realm to be handed. A HAND-WRITTEN LIST HERE IS WHAT
     THIS REPLACES, and the cost of the list was measured: it omitted `idl_variadic`, which composes with every
     declaration form, so two members that state their variadic tail correctly were reported as omitting it —
     a checker inventing work at the one place it claims authority. The set is read; a modifier a consumer
     cannot interpret is REPORTED by that consumer rather than ignored, so the next one added says so.
     THE PARAMETER LIST IS READ AS ONE BALANCED GROUP AND ITS NESTED ONES ARE REMOVED BEFORE THE `JSContext`
     TEST, and both halves of that were wrong in the same direction. `[^)]*` stops at the FIRST `)`, so a
     modifier taking a FUNCTION POINTER is read with its parameter list cut in half; and the token `JSContext`
     inside that pointer's own parameters is not this function being handed a realm, which is the rule the
     comment above states. Both together made `idl_arg_iface(int, bool (*)(JSContext *, JSValueConst), const
     char *)` invisible — the drift this derivation exists to prevent, arriving through the READING rather than
     through a list, and silently, since a modifier nobody sees is a modifier no consumer reports. */
  const modifiers = new Set();
  const stripped = strip(src);
  for (const m of stripped.matchAll(/\bvoid\s+idl_([a-z_0-9]+)\s*\(/g)) {
    const { text, end } = callText(stripped, m.index + m[0].length - 1);
    if (!/^\s*;/.test(stripped.slice(end + 1))) continue;   /* a definition, not a prototype */
    let own = text, prev;
    do { prev = own; own = own.replace(/\([^()]*\)/g, " "); } while (own !== prev);
    if (!/\bJSContext\b/.test(own)) modifiers.add(m[1]);
  }
  if (!modifiers.has("optional_from"))
    throw new Error("idl_args.h no longer declares `void idl_optional_from(int)` with no JSContext — the "
      + "post-declaration modifiers are read by that shape, so the read has stopped finding them");

  return { all, crosses, notCrossing, modifiers };
}

/* ---- every argument declaration one file makes -------------------------------------------------------------
 *
 * A member's declared types reach a body three ways and all three are read: a named `static const IdlArgType`
 * array, an inline compound literal, and a setter's single type. `idl_method_id_step` names an IdlStepDecl
 * instead of a body, and that struct's FIRST field is the step function — so the decl table is read too, and a
 * step machine's argv is audited exactly as a plain body's is.
 *
 * THE POST-DECLARATION MODIFIERS BELONG TO THE DECLARATION THEY FOLLOW, which is what idl_args.h says of every
 * one of them in the same words: each "names the member the LAST declaration made". So they are attached by
 * TEXT ORDER, to the nearest preceding declaration in the same file, and a modifier standing before any
 * declaration is carried as an orphan rather than silently dropped — the C's own seal asserts the same
 * association at runtime, so a disagreement here is this reader's to report and not to smooth over.
 *
 * WHICH FUNCTIONS THEY ARE IS `contract()`'s ANSWER, NOT THIS FILE'S. Every one of them is carried on the
 * declaration as written (`mods`), and only a few are INTERPRETED into fields; a consumer that needs a
 * modifier this reader does not interpret can see that it was there. That asymmetry is deliberate and it is
 * what a hand-written list could not give: an uninterpreted modifier is visible, where an unlisted one was
 * invisible and produced a confident false finding.
 */
const DECL_RE = /\bidl_(method_id|method_id_ext|method_id_dict|method_id_step|setter_id|setter_id_step)\s*\(/g;

/* The modifiers this reader turns into FIELDS. Written beside the switch that reads them so the two cannot
   drift, and exported so a consumer can say which of a declaration's modifiers it took no account of — the
   difference between "there was nothing there" and "there was something this did not read". */
export const INTERPRETED_MODS = new Set(["optional_from", "overload_split_optional_from", "variadic",
                                         "iface_brand", "arg_default", "arg_iface"]);

/* THE RAW SOURCE IS A SECOND, REQUIRED PARAMETER AND NOT AN OPTIONAL ONE. `strip` blanks a string literal to
   spaces of the SAME LENGTH, so a declaration's own string arguments — the identifier `idl_arg_iface` names its
   interface with — survive only at the same offsets in the unstripped text. Reading them means holding both,
   and holding both means they must be the same program: an optional parameter here would let a consumer that
   forgot it read every identifier as the empty string, which is the absent-versus-zero confusion this file
   already fixed once for a type list. The length check is the whole of what makes "the same program" checkable,
   and it is exactly what `strip`'s own contract guarantees. */
export function declarations(strippedSrc, rawSrc) {
  const src = strippedSrc;
  if (typeof rawSrc !== "string" || rawSrc.length !== src.length)
    throw new Error("idl_argdecl: declarations() needs the STRIPPED source and the RAW source of the same file "
      + "— `strip` preserves length, so a length that differs (or a missing second argument) means the two are "
      + "not one program and every offset read out of one is meaningless in the other");

  const arrays = new Map();
  for (const m of src.matchAll(/\b(?:static\s+)?const\s+IdlArgType\s+([A-Za-z_][A-Za-z_0-9]*)\s*\[[^\]]*\]\s*=\s*\{([\s\S]*?)\}\s*;/g))
    arrays.set(m[1], args(m[2]).map((t) => t.split(/\s+/)[0]));

  const stepFn = new Map();
  for (const m of src.matchAll(/\b(?:static\s+)?const\s+IdlStepDecl\s+([A-Za-z_][A-Za-z_0-9]*)\s*=\s*\{([\s\S]*?)\}\s*;/g)) {
    const first = args(m[2])[0];
    if (first) stepFn.set(m[1], first.replace(/^&/, "").trim());
  }

  const listOf = (expr, enumMembers) => {
    const inline = expr.match(/\(\s*const\s+IdlArgType\s*\[\s*\]\s*\)\s*\{([\s\S]*)\}/);
    if (inline) return args(inline[1]).map((t) => t.split(/\s+/)[0]);
    const name = expr.trim().replace(/^&/, "");
    /* `NULL` IS A RESOLVED LIST AND NOT AN UNREADABLE ONE — it is how a member with no arguments spells its
       type list, which is most of the platform's zero-arity surface. Returned as null it was the single
       largest reason a consumer could not judge a member at all: 78 of them, every one a member whose ARITY is
       still perfectly comparable against its IDL. An empty list says "this declares no positions", which is a
       fact, where null says "this reader could not tell", which was not true. */
    if (name === "NULL" || name === "0") return [];
    return arrays.get(name) || (enumMembers.has(name) ? [name] : null);
  };

  return { arrays, stepFn, listOf,
    /* `C` is the contract() this consumer already read — passed in rather than re-read, so one file reads the
       header and both audits see the same enum and the same modifier set. */
    read(C) {
      const enumMembers = C.all;
      const MOD_RE = new RegExp(`\\bidl_(${[...C.modifiers].join("|")})\\s*\\(`, "g");
      const out = [];
      DECL_RE.lastIndex = 0;
      let m;
      while ((m = DECL_RE.exec(src))) {
        const kind = m[1];
        const open = m.index + m[0].length - 1;
        const { text, end } = callText(src, open);
        const a = args(text);
        /* The assignment this declaration's step id lands in — `g_id_x = idl_method_id(…)`, sometimes an array
           element for a magic-indexed family. It is the ONLY link between a declaration and the install that
           names the member, so it is captured as written and resolved by whoever needs it. */
        const before = src.slice(Math.max(0, m.index - 120), m.index);
        const asg = before.match(/([A-Za-z_][A-Za-z_0-9]*(?:\s*\[[^\]]*\])?)\s*=\s*$/);
        let types = null, nargs = null, body = null, variadic = false, stepDeclName = null;
        if (kind === "method_id")        { types = listOf(a[1] || "", enumMembers); nargs = a[2]; body = a[3]; }
        else if (kind === "method_id_ext")  { types = listOf(a[1] || "", enumMembers); nargs = a[2];
                                              variadic = /^\s*true\s*$/.test(a[3] || ""); body = a[5]; }
        else if (kind === "method_id_dict") { types = listOf(a[1] || "", enumMembers); nargs = a[2]; body = a[5]; }
        else if (kind === "method_id_step") { types = listOf(a[1] || "", enumMembers); nargs = a[2];
                                              stepDeclName = (a[5] || "").replace(/^&/, "").trim();
                                              body = stepFn.get(stepDeclName); }
        else if (kind === "setter_id")      { types = listOf(a[1] || "", enumMembers); nargs = "1"; body = a[3]; }
        else if (kind === "setter_id_step") { types = listOf(a[1] || "", enumMembers); nargs = "1";
                                              stepDeclName = (a[3] || "").replace(/^&/, "").trim();
                                              body = stepFn.get(stepDeclName); }
        out.push({
          kind, types, body: body ? body.trim() : null, stepDeclName, variadic,
          nargs: nargs != null && /^\d+$/.test(String(nargs).trim()) ? Number(String(nargs).trim()) : null,
          nargsExpr: nargs == null ? null : String(nargs).trim(),
          lhs: asg ? asg[1].replace(/\s+/g, "") : null,
          at: m.index, end, mods: new Map(),
          optionalFrom: null, optionalFromExpr: null, splitOptionalFrom: null, brand: null, defaults: [],
          argIfaces: [],
        });
      }

      /* Attach each post-declaration modifier to the nearest preceding declaration. */
      const orphans = [];
      let k;
      while ((k = MOD_RE.exec(src))) {
        const open = k.index + k[0].length - 1;
        const { text } = callText(src, open);
        const a = args(text);
        /* The SAME call, read out of the raw source — same offsets, because `strip` preserves length. This is
           the only place a modifier's string literal can come from, and it is split by the same splitter so
           the two argument lists index alike. */
        const rawArgs = args(rawSrc.slice(open + 1, open + 1 + text.length));
        let owner = null;
        for (const d of out) if (d.end < k.index && (!owner || d.end > owner.end)) owner = d;
        if (!owner) { orphans.push({ mod: k[1], at: k.index }); continue; }
        if (!owner.mods.has(k[1])) owner.mods.set(k[1], []);
        owner.mods.get(k[1]).push(a);
        const num = (x) => (x != null && /^\d+$/.test(String(x).trim()) ? Number(String(x).trim()) : null);
        if (k[1] === "optional_from") { owner.optionalFrom = num(a[0]); owner.optionalFromExpr = (a[0] || "").trim(); }
        else if (k[1] === "overload_split_optional_from") owner.splitOptionalFrom = num(a[0]);
        else if (k[1] === "variadic") owner.variadic = true;
        else if (k[1] === "iface_brand") owner.brand = (a[0] || "").trim();
        /* §3.2.15's `I` at ONE position, and the IDL IDENTIFIER it names — which is the half a consumer can
           check, because the spec states the interface at that position and the declaration states it again.
           The string literal is blanked by `strip`, so what survives is its quotes; the identifier is read
           back out of them rather than out of the raw source, since a consumer reading raw text here would be
           reading a different program from the one every other field came from. */
        else if (k[1] === "arg_iface") owner.argIfaces.push({ index: num(a[0]), pred: (a[1] || "").trim(),
                                                              iface: rawArgs[2] === undefined ? null
                                                                   : (rawArgs[2].match(/"([^"]*)"/) || [])[1] ?? null });
        else if (k[1] === "arg_default") owner.defaults.push({ index: num(a[0]), kind: (a[1] || "").trim(),
                                                              str: (a[2] || "").trim() });
      }
      return { decls: out, orphans };
    } };
}
