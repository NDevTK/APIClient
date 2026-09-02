/* THE `IdlDictMember` / `IdlDictDecl` READER — one read of one construct, for every audit that asks about a
 * dictionary.
 *
 * WHY IT IS A MODULE AND NOT A BLOCK INSIDE ONE GATE. It was written inside idlgen.mjs, which asks about
 * dictionary member NAMES, `required` and §3.2.17's read order. dicttypegate.mjs asks about the same rows'
 * TYPES. Those are two questions over ONE declaration, and CLAUDE.md's standing rule for the instruments here
 * is that an auditor derives what it checks from the code that owns the fact rather than restating it — a
 * second reader of `IdlDictMember` would be a second answer to "what does this engine declare about this
 * dictionary", and the one that drifts is the copy whose consumer runs less often. So the READ lives here once
 * and the QUESTIONS live in the gates.
 *
 * WHAT IT DOES NOT DO, deliberately: it does not know what a dictionary IS. Whether a named declaration's
 * identifier is a dictionary any spec defines, which dictionary a bare array is a declaration of, and whether
 * a declared type is the one the IDL states are all IDL-side facts, and a reader that answered them would be
 * holding a fact it has no way to check. It reports what the C says and its own reach; the caller judges.
 *
 * A LIST IS READ OR REFUSED — never partially read. A member list this cannot read is not a list with fewer
 * members, it is a list of unknown length, and crediting the entries it did manage is a false COMPLETE minted
 * by the reader rather than by the engine.
 */

/* The 1-based line an offset falls on, so a finding names a site rather than a file — the same address rule
   §Offensive-programming states for an assert, applied to a report. */
export const lineAt = (src, off) => {
  let n = 1;
  for (let i = 0; i < off && i < src.length; i++) if (src[i] === "\n") n++;
  return n;
};
/* The `}` closing the `{` at `i`, or -1. Over MASKED source, so a brace inside a comment or a string cannot
   move the count — which is why this reads env.sources' masked text and never the file. */
const closeBrace = (s, i) => {
  let d = 0;
  for (let j = i; j < s.length; j++) {
    const c = s[j];
    if (c === "{") d++;
    else if (c === "}" && !--d) return j;
  }
  return -1;
};
/* Top-level commas of one initialiser body. */
const splitTopLevel = (s) => {
  const out = [];
  let d = 0, st = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (c === "(" || c === "[" || c === "{") d++;
    else if (c === ")" || c === "]" || c === "}") d--;
    else if (c === "," && !d) { out.push(s.slice(st, i)); st = i + 1; }
  }
  out.push(s.slice(st));
  return out;
};
const STRING_LIT = /^"((?:[^"\\]|\\.)*)"$/;

/* ONE `IdlDictMember` INITIALISER LIST. An entry that is a bare identifier is resolved through the macros
   idl_installed.mjs already collected — core/events/ui_event.h states the seven members UIEvent contributes as
   one object-like macro and four components expand it, so refusing that construct would put four event
   dictionaries beyond every audit for a spelling. */
function readDictMembers(body, macros, depth = 0) {
  const out = [];
  for (const raw of splitTopLevel(body)) {
    const e = raw.trim();
    if (!e) continue;
    if (!e.startsWith("{")) {
      const def = /^[A-Za-z_]\w*$/.test(e) && macros.get(e);
      if (!def || def.params || depth > 4) return { why: e.replace(/\s+/g, " ").slice(0, 60) };
      const nested = readDictMembers(def.body, macros, depth + 1);
      if (nested.why) return nested;
      out.push(...nested.members);
      continue;
    }
    const fields = splitTopLevel(e.slice(1, e.lastIndexOf("}")));
    const lit = (fields[0] || "").trim().match(STRING_LIT);
    if (!lit) return { why: (fields[0] || "").trim().replace(/\s+/g, " ").slice(0, 60) };
    /* Field 2 is `required` and field 4 is §3.2.17's level; both are omitted by the short form, and C then
       zero-fills them — which is the IDL's own default for both (a member with no `required` written is
       optional, and a dictionary that inherits nothing has one level). A field that is not a literal is not
       assumed either way: it is left undeclared and the checks that need it skip that member and say so. */
    const req = (fields[2] || "").trim(), lvl = (fields[4] || "").trim();
    /* FIELD 1 IS THE MEMBER'S TYPE, and it is carried as BOTH the raw text and the enumerator that text is
       when it is one. The pair is not redundancy: `IdlArgType` is an enum, so a row whose type position holds
       anything else — a macro, a cast, a designated initialiser that skipped it — is a row this reader could
       not place, and reporting that as "no type" would be the consumer-side default §Architecture forbids.
       `null` here therefore means UNREAD and never IDL_NONE; the text says what was there instead. */
    const typeText = (fields[1] || "").trim().replace(/\s+/g, " ");
    out.push({ name: lit[1],
               type: /^IDL_[A-Z0-9_]+$/.test(typeText) ? typeText : null,
               typeText,
               required: req === "" ? false : req === "true" ? true : req === "false" ? false : null,
               level: lvl === "" ? 0 : /^\d+$/.test(lvl) ? Number(lvl) : null });
  }
  return { members: out };
}

const ARRAY_DECL = /\bIdlDictMember\s+([A-Za-z_]\w*)\s*\[[^\]]*\]\s*=\s*\{/g;
const NAMED_DECL = /\bIdlDictDecl\s+([A-Za-z_]\w*)\s*=\s*\{/g;

/* EVERY DICTIONARY DECLARATION THIS ENGINE MAKES, read out of `env` (idl_installed.mjs's loadEnvironment).
 *
 *   arrays      every `IdlDictMember` array read, with its file, line, C identifier and members
 *   named       every `IdlDictDecl` read, carrying the dictionary IDENTIFIER it states and the array it names
 *   unreadable  every declaration of either kind this reader refused, with why
 *
 * The two passes run in that order and `unreadable` preserves it, because a consumer prints the list and a
 * reordering would read as a change in the tree. */
export function readDictDecls(env) {
  const arrays = [], named = [], unreadable = [];
  const bySymbol = new Map();
  for (const [path, { masked, orig }] of env.sources) {
    ARRAY_DECL.lastIndex = 0;
    for (let m; (m = ARRAY_DECL.exec(masked)); ) {
      const open = m.index + m[0].length - 1, close = closeBrace(masked, open);
      const at = { file: path, line: lineAt(orig, m.index), sym: m[1] };
      if (close < 0) { unreadable.push({ ...at, why: "the initialiser's braces do not balance" }); continue; }
      const r = readDictMembers(masked.slice(open + 1, close), env.macros);
      if (r.why) { unreadable.push({ ...at, why: `an entry this reader cannot resolve: ${r.why}` }); continue; }
      const rec = { ...at, members: r.members };
      arrays.push(rec);
      bySymbol.set(`${path}\0${m[1]}`, rec);
      if (!bySymbol.has(m[1])) bySymbol.set(m[1], rec);
    }
  }
  for (const [path, { masked, orig }] of env.sources) {
    NAMED_DECL.lastIndex = 0;
    for (let m; (m = NAMED_DECL.exec(masked)); ) {
      const open = m.index + m[0].length - 1, close = closeBrace(masked, open);
      if (close < 0) continue;
      const fields = splitTopLevel(masked.slice(open + 1, close));
      const lit = (fields[0] || "").trim().match(STRING_LIT), sym = (fields[1] || "").trim();
      const line = lineAt(orig, m.index);
      if (!lit) { unreadable.push({ file: path, line, sym: m[1], why: "its dictionary identifier is not a string literal" }); continue; }
      const arr = bySymbol.get(`${path}\0${sym}`) || bySymbol.get(sym);
      if (!arr) { unreadable.push({ file: path, line, sym: m[1], why: `its member list \`${sym}\` was not read` }); continue; }
      named.push({ file: path, line, decl: m[1], name: lit[1], arr });
    }
  }
  return { arrays, named, unreadable };
}
