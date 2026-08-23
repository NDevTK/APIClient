/* WHAT A COMPONENT ACTUALLY INSTALLS — the truthful half of the Web IDL gap audit.
 *
 * The audit in idlgen.mjs diffs a spec member list against the members a component installs. That diff is only
 * worth reading if the installed side is TRUE, and it was not: the installed side was a scan for ANY STRING
 * LITERAL in the component's .c. Both errors follow from that, and the dangerous one is silent.
 *
 *   FALSE COMPLETE. A file containing "required" for an unrelated reason — a content-attribute name, a
 *   DOMException name, an error message, an enum value — was credited with installing a member called
 *   `required`, and the audit then reported a real gap as filled. That is why html_form.c, input_value.c and
 *   constraint_validation.c could not be named in the interface map at all: those files are full of content
 *   attribute names ("required", "pattern", "min", "step", "size"), so naming them would have turned a false
 *   ABSENT into a false COMPLETE. An auditor that can be made to lie by adding a row is an auditor whose
 *   silence means nothing.
 *
 *   FALSE ABSENT. A member installed through a macro, a table or a shared install helper spells its name
 *   nowhere in the file that installs it, so it was reported missing while it was shipping. HTMLInputElement's
 *   `files`, `value`, `checked` and `validity` were all in that state.
 *
 * Both come from one root: installation was inferred from TEXT, when it is a STRUCTURED FACT. A member is
 * installed by a small, explicit vocabulary — an `idl_install_*` call, a `JSCFunctionListEntry` table handed to
 * JS_SetPropertyFunctionList, a JS_DefinePropertyGetSet, a reflection table handed to
 * element_declare_reflections, or a property write onto an install target — and this module reads THOSE, taking
 * the member name out of the install construct itself. A bare string is not a declaration of anything.
 *
 * IT FAILS LOUD RATHER THAN GUESSING. An install construct whose member name cannot be resolved statically is
 * reported as UNRESOLVED with its file and line. It is never silently dropped (which reads as a gap that is not
 * there) and never silently counted (which reads as a gap that is filled and is not). The UNRESOLVED list is
 * the audit's own gap report on itself, and it is the work queue for making the next name resolvable.
 *
 * WHAT COUNTS AS AN INSTALL TARGET — AND IT IS THE SAME QUESTION AS WHICH INTERFACE IT IS, ASKED ONCE.
 * `JS_SetPropertyStr` and `JS_DefinePropertyValueStr` are the two forms that are also how any ordinary object is
 * built, so resolving the NAME is not enough: the object has to be one a page can reach a member ON. fetch.c
 * writes "status", "statusText" and "body" onto a plain record and Window really does have a `status` member;
 * event_target.c writes "removed", "once" and "passive" onto a listener record; viewport.c writes §13.1's
 * "hasBeenRun" latch onto a per-realm record built with NO PROTOTYPE. Counting those is the false COMPLETE
 * above, and refusing to count `fetch`, `navigator`, `Node` and `Text` — installed on the global with the same
 * call — is the false ABSENT.
 *
 * There WAS a second solve for this — an object was a target if any unambiguous form named it, and the fact
 * flowed BIDIRECTIONALLY along assignments and along arguments into functions this corpus defines. Two-way
 * argument flow is what made it wrong: a shared helper's parameter is not one object, it is the union of every
 * caller's, so one real prototype handed to `realm_value_set` made EVERY other caller's record a target too, and
 * §13.1's resize latch, §6.4.1's activation timestamps, §6.6.7's autofocus list and a converted FocusEvent
 * dictionary were all read as objects with members installed on them. They then reported as UNATTRIBUTED — a
 * report whose entries are mostly noise, which is where the string-literal scan ended up too.
 *
 * That solve is DELETED, because the question it answered is already answered exactly by the one below: A PAGE
 * REACHES AN OBJECT BECAUSE THE CORPUS DECLARES AN INTERFACE FOR IT. Web IDL §3.7.3's `idl_interface_tag`,
 * §3.7.1's interface object, quickjs's per-realm class-prototype slot and §3.7.3's [Global] statement are the
 * four declarations, and an object none of them reaches is an INTERNAL RECORD: a write onto it installs nothing.
 * One graph decides both, so an install can no longer be "counted but unfilable", and the two failures stay
 * visible from BOTH sides rather than being allowlisted: an AMBIGUOUS write on an undeclared object is reported
 * by name (the caller prints it where the name is a member of the interface being audited, so a member really
 * installed as a plain own property cannot go silent), and an UNAMBIGUOUS install on an undeclared object stays
 * UNATTRIBUTED, named with file and line. The construction says the same thing from the other end and is
 * asserted: an object built with NO PROTOTYPE (`JS_NewObjectProto(ctx, JS_NULL)`, `idl_slots_new`) is reachable
 * from nothing the page holds, so an install form naming one is a CONTRADICTION — reported, never tolerated.
 *
 * WHICH INTERFACE THAT TARGET IS — the same solve, and the reason there is only one. Reading the install
 * construct fixed WHAT is installed and left WHERE untouched: the audit was FILE-granular, so a row credited
 * every member any of its files installed, and html_form.c's `value` — on HTMLTextAreaElement.prototype — counted
 * for HTMLInputElement. That is a false COMPLETE, which HIDES a gap rather than burying it in noise. So a
 * member is attributed to the interface its TARGET is, read out of Web IDL §3.7.3's @@toStringTag that the
 * component already installs on every interface prototype object (`idl_interface_tag`) and §3.7.3's [Global]
 * statement for the object a [Global] interface puts its members on directly (`idl_global_object`). A target
 * this cannot decide is UNATTRIBUTED — named with its file, line and member, never credited to its file, which
 * is the fallback the whole mechanism exists to remove.
 *
 * THE SUBSET A SHARED INSTALLER SELECTS IS READ, and it was the last place this guessed.
 * `event_target_install_handlers(ctx, target, EH_XHR)` walks HTML §8.1.7.2's ninety event handler IDL
 * attributes and installs the rows whose MIXIN bit the caller asked for; the guard is a `continue` at the top
 * of the loop rather than a condition over the site, so guardAt() reads nothing and all ninety were credited to
 * every prototype any caller ever handed it. That was defended here as honest — "a name over-credited to a
 * prototype is only a false COMPLETE when the IDL puts that name on that interface, and the masks and the
 * mixins agree today" — and both halves of that defence are wrong. It is exactly the case where the IDL DOES
 * put the name on the interface: XMLHttpRequestEventTarget declares seven of those names and Window declares
 * eighty-odd, so wherever two callers' interfaces share a name the mask IS the difference between installed and
 * credited. And "they agree today" is a fact about the corpus checked by nobody, in the ONE direction this
 * audit cannot catch from the other side — an over-credited member does not print as anything at all.
 * So the subset is resolved PER CALL SITE, where the caller supplies both the selector and the target: the name
 * column and the selector column are two columns of one X-list, the caller's argument is evaluated as a C
 * integer constant, and the rows whose bits intersect are the members that call installs on that target. A
 * selector this cannot evaluate — or a row filter in any other shape — is a REFUSAL with its file and line,
 * never a fallback to the union: a subset the audit cannot compute is a member list it does not know.
 */
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, extname } from "node:path";

/* ---- source masking ------------------------------------------------------------------------------------- */

/* Comments and line-continuations removed WITHOUT MOVING ANY BYTE: every replacement is the same length, so an
   offset into the masked text is an offset into the original and a reported line number is the real one. A
   continued line becomes one logical line (the newline turns into a space), which is what makes a multi-line
   #define and a multi-line call one thing to match. String literals are left exactly as they are — they carry
   the member names.
   A BLOCK COMMENT'S NEWLINES GO TOO, because that is what the C preprocessor does: line splicing is phase 2
   and comment removal is phase 3, so a multi-line comment sitting inside a #define is whitespace and the macro
   continues past it. event_target.c's ninety event-handler names are one such macro with a four-line comment in
   the middle of it, and stopping at that comment read nine of them. */
function mask(src) {
  const out = src.split("");
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "*") {
      let j = i + 2;
      while (j < n && !(src[j] === "*" && src[j + 1] === "/")) j++;
      for (let k = i; k < Math.min(j + 2, n); k++) out[k] = " ";
      i = j + 2;
      continue;
    }
    if (c === "/" && src[i + 1] === "/") {
      let j = i;
      while (j < n && src[j] !== "\n") j++;
      for (let k = i; k < j; k++) out[k] = " ";
      i = j;
      continue;
    }
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c) { if (src[j] === "\\") j++; j++; }
      i = j + 1;
      continue;
    }
    if (c === "\\" && src[i + 1] === "\n") { out[i] = " "; out[i + 1] = " "; i += 2; continue; }
    i++;
  }
  return out.join("");
}

const lineOf = (src, off) => {
  let line = 1;
  for (let i = 0; i < off && i < src.length; i++) if (src[i] === "\n") line++;
  return line;
};

/* ---- balanced scanning ---------------------------------------------------------------------------------- */

const PAIRS = { "(": ")", "[": "]", "{": "}" };

/* The index one past the closer that matches the opener at `open`. Strings are skipped, so a brace inside a
   literal never closes a block. Returns -1 when the text ends first, which is a source this cannot parse and
   therefore something to report rather than to assume about.
   THE CLOSER MUST MATCH THE OPENER'S KIND. Counting one depth across `(`, `[` and `{` let a `)` close a `[`,
   which does not return -1 — it returns a SPAN, and a plausible wrong span is the failure mode this whole
   file is built against, not a missing one. Every caller already treats -1 as "I could not read this", so
   refusing costs nothing and guessing cost everything. */
function matchAt(text, open) {
  if (!PAIRS[text[open]]) return -1;
  const stack = [];
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (c === '"' || c === "'") {
      i++;
      while (i < text.length && text[i] !== c) { if (text[i] === "\\") i++; i++; }
      continue;
    }
    if (PAIRS[c]) stack.push(c);
    else if (c === ")" || c === "]" || c === "}") {
      if (PAIRS[stack.pop()] !== c) return -1;
      if (!stack.length) return i + 1;
    }
  }
  return -1;
}

/* PARSE INTEGRITY, ASKED ONCE PER FILE AND ANSWERED WITH A PLACE. Every span primitive in this reader —
   matchAt, splitTop, callSites, functions, localDefs — assumes the masked text's brackets balance and nest by
   kind, and each of them fails DIFFERENTLY and silently when they do not: matchAt now refuses, but splitTop
   returns a depth that never comes back to zero, callSites drops the call, functions drops a whole body. There
   is no useful place to report that per primitive, because by then the location is a span inside a span. So it
   is asked ONCE of the file, where the answer is a line a person can open — and it is the one question whose
   failure invalidates every other answer this file gives about that file. */
function bracketFault(masked) {
  const stack = [];
  for (let i = 0; i < masked.length; i++) {
    const c = masked[i];
    if (c === '"' || c === "'") {
      i++;
      while (i < masked.length && masked[i] !== c) { if (masked[i] === "\\") i++; i++; }
      continue;
    }
    if (PAIRS[c]) stack.push({ c, at: i });
    else if (c === ")" || c === "]" || c === "}") {
      const open = stack.pop();
      if (!open) return { at: i, why: `a closing \`${c}\` with nothing open` };
      if (PAIRS[open.c] !== c) return { at: i, why: `a \`${open.c}\` closed by \`${c}\`` };
    }
  }
  const last = stack[stack.length - 1];
  return last ? { at: last.at, why: `an unclosed \`${last.c}\`` } : null;
}

/* Split at top-level `sep` only — a comma inside `(…)`, `[…]`, `{…}` or a string belongs to the thing it is
   inside. This is how a struct row's fields and a call's arguments are separated, and the two are the same
   operation. */
function splitTop(text, sep = ",", braces = true) {
  const parts = [];
  let depth = 0, start = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"' || c === "'") {
      i++;
      while (i < text.length && text[i] !== c) { if (text[i] === "\\") i++; i++; }
      continue;
    }
    if (c === "(" || c === "[" || (braces && c === "{")) depth++;
    else if (c === ")" || c === "]" || (braces && c === "}")) depth--;
    else if (c === sep && depth === 0) { parts.push(text.slice(start, i)); start = i + 1; }
  }
  parts.push(text.slice(start));
  return parts;
}

/* ---- the preprocessor this needs (and no more) ----------------------------------------------------------- */

/* THE MACRO IS PART OF THE DECLARATION, so the auditor follows it to where it is defined rather than giving up.
   §4.10.21's ten validity states are one X-macro expanded into the ValidityState member names; §4.13.7.4's 44
   ARIAMixin members are another; the element-interface table's reflection lists are a third. Reading only the
   text of the file that installs them sees none of the names.
   Object-like and function-like #defines, expanded recursively with a depth ceiling. The ceiling is not a
   coverage bound — an expansion that hits it yields nothing and the install that needed it is reported
   UNRESOLVED by name, which is the loud failure this module exists to produce. */
const DEFINE_RE = /^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\([^)]*\))?[ \t]*([^\n]*)/gm;

function parseDefines(masked, into) {
  DEFINE_RE.lastIndex = 0;
  let m;
  while ((m = DEFINE_RE.exec(masked))) {
    const [, name, params, body] = m;
    into.set(name, {
      params: params ? params.slice(1, -1).split(",").map((s) => s.trim()).filter(Boolean) : null,
      body: body.trim(),
    });
  }
}

/* An X-macro that a table `#undef`s after using it is SCOPED TO THAT TABLE — event_target.c defines `X` three
   times over the same handler list to build three parallel arrays. Kept in the file-wide table it would be one
   name with three meanings and whichever came last would expand every other file's `X`. So a macro the file
   undefines is not a file-wide macro; it is re-read from the initializer that defines it, where its one meaning
   is unambiguous. */
function collectMacros(masked, into) {
  const local = new Map();
  parseDefines(masked, local);
  for (const [, name] of masked.matchAll(/^[ \t]*#[ \t]*undef[ \t]+([A-Za-z_]\w*)/gm)) local.delete(name);
  for (const [k, v] of local) into.set(k, v);
}

function expand(text, macros, depth = 0) {
  if (depth > 12) return text;
  let changed = false;
  let out = "";
  let i = 0;
  while (i < text.length) {
    const c = text[i];
    if (c === '"' || c === "'") {
      const s = i++;
      while (i < text.length && text[i] !== c) { if (text[i] === "\\") i++; i++; }
      out += text.slice(s, ++i);
      continue;
    }
    if (!/[A-Za-z_]/.test(c)) { out += c; i++; continue; }
    let j = i;
    while (j < text.length && /[\w]/.test(text[j])) j++;
    const word = text.slice(i, j);
    const def = macros.get(word);
    if (!def) { out += word; i = j; continue; }
    if (!def.params) { out += ` ${def.body} `; i = j; changed = true; continue; }
    let k = j;
    while (k < text.length && /\s/.test(text[k])) k++;
    if (text[k] !== "(") { out += word; i = j; continue; }
    const end = matchAt(text, k);
    if (end < 0) { out += word; i = j; continue; }
    const args = splitTop(text.slice(k + 1, end - 1)).map((s) => s.trim());
    let body = def.body;
    def.params.forEach((p, idx) => {
      body = body.replace(new RegExp(`\\b${p}\\b`, "g"), args[idx] === undefined ? "" : args[idx]);
    });
    out += ` ${body} `;
    i = end;
    changed = true;
  }
  return changed ? expand(out, macros, depth + 1) : out;
}

/* ---- the tables a member name can live in ---------------------------------------------------------------- */

/* `typedef struct { … } Name;` — the field ORDER is what turns `ARIA[i].member` and `HL_M[i].name` into a
   column of the table, so the names are read in declaration order and a positional row is read against them. */
function structFields(body) {
  const fields = [];
  for (const decl of splitTop(body, ";")) {
    const d = decl.trim();
    if (!d) continue;
    /* the declarator list past the type: `const char *member, *attr` → member, attr */
    for (const one of splitTop(d)) {
      const t = one.trim();
      /* A FUNCTION-POINTER field names itself inside `(*name)`, and reading only a trailing identifier found
         none — which does not drop that field, it SHIFTS every field after it. ByteReaderIface starts with two
         of them, so `readers` was read out of the `iface` column. */
      const fp = t.match(/\(\s*\*\s*([A-Za-z_]\w*)\s*\)/);
      const id = t.match(/([A-Za-z_]\w*)\s*(\[[^\]]*\])?$/);
      fields.push(fp ? fp[1] : id ? id[1] : null);
    }
  }
  return fields;
}

function collectTypedefs(masked, into) {
  const re = /typedef\s+struct\s*(?:[A-Za-z_]\w*\s*)?\{/g;
  let m;
  while ((m = re.exec(masked))) {
    const open = masked.indexOf("{", m.index);
    const end = matchAt(masked, open);
    if (end < 0) continue;
    const tail = masked.slice(end).match(/^\s*([A-Za-z_]\w*)\s*;/);
    if (!tail) continue;
    into.set(tail[1], structFields(masked.slice(open + 1, end - 1)));
  }
}

/* AN INITIALISED DEFINITION, wherever it is — file scope or inside the function that installs it (body.c builds
   its two-entry JSCFunctionListEntry on the stack, element.c's three plain reflections are a function-local
   static, and bar_prop.c's six BarProp names are another). An ARRAY's rows are its entries; a plain STRUCT is
   one row, which is what `static const ByteReaderIface BLOB_READER_IFACE = { … }` is and what lets the reader
   table it names be followed. The element TYPE decides how a row is read: a `char *` array's rows ARE the
   values, a struct array's rows are positional against the struct's fields.
   TABLES ARE KEYED PER FILE. Two files each declaring a function-local `NAMES[]` is not one table with two
   meanings — document.c's five element shortcuts and element_internals.c's three interface names both spell it
   that way, and one global map answered document.c's `NAMES[k]` with element_internals.c's list. */
function collectTables(masked, typedefs, into) {
  const re = /([A-Za-z_]\w*)\s*(\[[^\];{}]*\])?\s*=\s*\{/g;
  let m;
  while ((m = re.exec(masked))) {
    const name = m[1];
    const isArray = m[2] !== undefined;
    const open = masked.indexOf("{", m.index + name.length);
    const end = matchAt(masked, open);
    if (end < 0) continue;
    /* The type text: back to the previous statement boundary — stepping OVER a complete `{…}`, because an
       anonymous struct's own field declarations end in semicolons and stopping at one of them would throw away
       the very field list the rows are read against (`static const struct { const char *tag; … } HTML_IFACE[]`
       is the case that forces it). */
    let s = m.index;
    while (s > 0) {
      const prev = masked[s - 1];
      if (prev === "}") {
        let depth = 0, k = s - 1;
        for (; k >= 0; k--) {
          if (masked[k] === "}") depth++;
          else if (masked[k] === "{") { depth--; if (depth === 0) break; }
        }
        if (k < 0) break;
        s = k;
        continue;
      }
      if ("{;".includes(prev)) break;
      s--;
    }
    const typeText = masked.slice(s, m.index);
    let fields = null;
    /* `struct` AS A WORD. This was `lastIndexOf("struct")`, a SUBSTRING search, and platform.c contains
       `i_structured_clone(…) { (void)d; structured_clone_install(c, g); }` a few lines above its table — so the
       match landed inside an IDENTIFIER, the next `{` after it was a thunk's BODY, and PLATFORM's field list
       was read as ["d", null] out of `(void)d; page_reveal_install(c, g);`. Every column of that table then
       resolved to nothing, which is where the [Global] fact died: `PLATFORM[i].install(ctx, global, doc)` names
       the 72 functions every per-realm component is installed by, so Window's ninety handler attributes,
       `opener`, `closed`, `top`, `setTimeout`, `structuredClone` and `matchMedia` were all UNATTRIBUTED and all
       counted ABSENT. A table whose type is a real inline `struct { … }` still matches, because that is a
       keyword and this now asks for one. */
    let inlineStruct = -1;
    for (const sm of typeText.matchAll(/\bstruct\b/g)) inlineStruct = sm.index;
    if (inlineStruct >= 0 && typeText.indexOf("{", inlineStruct) >= 0) {
      const so = typeText.indexOf("{", inlineStruct);
      const se = matchAt(typeText, so);
      if (se > 0) fields = structFields(typeText.slice(so + 1, se - 1));
    } else {
      const words = typeText.replace(/[*]/g, " ").trim().split(/\s+/).filter(Boolean);
      for (let k = words.length - 1; k >= 0; k--) {
        if (typedefs.has(words[k])) { fields = typedefs.get(words[k]); break; }
      }
    }
    if (!into.has(name)) into.set(name, []);
    into.get(name).push({ fields, isArray, at: m.index, body: masked.slice(open + 1, end - 1) });
  }
}

/* The tables split into the scopes C gives them: one map per FUNCTION for the ones defined inside a body, one
   for the file's own. document.c declares `NAMES[]` twice — the document-readiness strings in one function and
   §3.1.5's five element shortcuts in another — and one map per file could only ever answer with one of them.
   Two definitions still in ONE scope after that split is a name this cannot decide, and it resolves to nothing
   so the install that needed it is reported UNRESOLVED rather than answered out of the wrong one. */
function scopeTables(masked, typedefs, fns) {
  const all = new Map();
  collectTables(masked, typedefs, all);
  const file = new Map(), perFn = new Map();
  const put = (map, name, t) => map.set(name, map.has(name) ? null : t);
  for (const [name, defs] of all)
    for (const t of defs) {
      const f = fns.find((x) => t.at >= x.start && t.at < x.end);
      if (!f) { put(file, name, t); continue; }
      if (!perFn.has(f.name)) perFn.set(f.name, new Map());
      put(perFn.get(f.name), name, t);
    }
  return { file, perFn };
}

/* ---- resolving an expression that stands in a member-name position --------------------------------------- */

const STRING_RE = /^\s*"((?:[^"\\]|\\.)*)"\s*$/;

/* A STRING CONSTANT IS A DECLARATION OF A NAME, and it is the third spelling of the one a macro and a table
   already are. §4.7's readable byte stream declares its queue-entry and pull-into-descriptor fields as
   `static const char *const Q_BUFFER = "buffer";` and writes them thirty times, and this read a #define and an
   initialised table and not that — so thirty constructs reported UNRESOLVED, which is the audit's own gap report
   naming the form it had not learned. Only `const char *` declarations are read: an initialised pointer to char
   IS a name and nothing else is one. Two declarations of the same identifier in one file resolve to nothing,
   which is the same answer a doubly-declared table gives, because neither can be decided. */
const CHAR_CONST_RE =
  /(?:^|[;{}])\s*(?:static\s+)?const\s+char\s*\*\s*(?:const\s+)?([A-Za-z_]\w*)\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);/gm;

function collectCharConsts(masked, into) {
  CHAR_CONST_RE.lastIndex = 0;
  let m;
  while ((m = CHAR_CONST_RE.exec(masked))) into.set(m[1], into.has(m[1]) ? null : m[2].trim());
}

/* ---- integer constants, for the SELECTOR a shared installer subsets its names by --------------------------- */

/* An INTEGER the C states as a constant. It is needed for exactly one question and it is not a small one: a
   shared installer that walks a table and installs the rows whose mask bit the CALLER asked for. Without the
   arithmetic there is no way to tell which rows a call site takes, and the only alternative to computing it is
   crediting every row to every caller — which is a false COMPLETE wherever the caller's interface really does
   declare a name the mask withholds, and a false COMPLETE is the one error this audit cannot catch from the
   other side, because the member it hides simply never prints. Only what a C constant expression holds is
   evaluated; anything else answers null and the site REFUSES rather than guessing. */
function evalInt(exprIn, consts, macros) {
  const text = macros ? expand(String(exprIn), macros) : String(exprIn);
  const toks = text.match(/0[xX][0-9a-fA-F]+|\d+[uUlL]*|[A-Za-z_]\w*|<<|>>|[()|&^~+]|\S/g);
  if (!toks) return null;
  let i = 0;
  const prim = () => {
    const t = toks[i];
    if (t === undefined) return null;
    if (t === "(") { i++; const v = or(); if (toks[i] !== ")") return null; i++; return v; }
    if (t === "~") { i++; const v = prim(); return v === null ? null : ~v; }
    if (t === "+") { i++; return prim(); }
    if (/^0[xX]/.test(t)) { i++; return parseInt(t, 16); }
    if (/^\d/.test(t)) { i++; return parseInt(t, 10); }
    if (/^[A-Za-z_]\w*$/.test(t)) { if (!consts.has(t) || consts.get(t) === null) return null; i++; return consts.get(t); }
    return null;
  };
  const bin = (next, ops) => () => {
    let v = next();
    while (v !== null && ops.includes(toks[i])) {
      const op = toks[i++], r = next();
      if (r === null) return null;
      v = op === "&" ? (v & r) : op === "^" ? (v ^ r) : op === "|" ? (v | r)
        : op === "<<" ? (v << r) : op === ">>" ? (v >> r) : (v + r);
    }
    return v;
  };
  const shift = bin(prim, ["<<", ">>"]);
  const add = bin(shift, ["+"]);
  const and = bin(add, ["&"]);
  const xor = bin(and, ["^"]);
  const or = bin(xor, ["|"]);
  const v = or();
  return v === null || i !== toks.length ? null : v;
}

/* THE ROW FILTER of a shared installer: `if (!(EH_MASK[i] & mask)) continue;` — a parallel column of the same
   list tested against one of the installer's own parameters. It is the ONE shape this can read, and that is
   stated rather than hidden: anything else with a `continue` in it REFUSES, because the alternative to reading
   the filter is crediting every row to every caller. */
const SELECT_RE = /\bif\s*\(\s*!\s*\(\s*([A-Za-z_]\w*)\s*\[\s*[A-Za-z_]\w*\s*\]\s*&\s*([A-Za-z_]\w*)\s*\)\s*\)\s*continue\s*;/;
function selectorOf(f, R) {
  const m = f.body.match(SELECT_RE);
  if (!m) return { why: "the names come from a table and the body filters rows with a `continue` this cannot " +
                        "read, so which rows a call installs is unknown" };
  const at = f.params.indexOf(m[2]);
  if (at < 0) return { why: `the row filter tests \`${m[2]}\`, which is not one of this installer's parameters` };
  const cells = R.column(m[1], null);
  if (!cells) return { why: `the selector column \`${m[1]}\` could not be read as a table` };
  return { cells, at, table: m[1] };
}

/* `enum { EH_GLOBAL = 1, EH_WINDOW = 2, … }` — a bare name is the previous value plus one, which is C's rule and
   is what makes a list nobody numbered still answerable. A name whose initializer this cannot evaluate is left
   UNKNOWN rather than given a plausible number, and every later bare name in that body is unknown with it. */
function collectEnums(masked, into, macros) {
  const re = /\benum\b\s*(?:[A-Za-z_]\w*\s*)?\{/g;
  let m;
  while ((m = re.exec(masked))) {
    const open = masked.indexOf("{", m.index);
    const end = matchAt(masked, open);
    if (end < 0) continue;
    let next = 0;
    for (const item of splitTop(masked.slice(open + 1, end - 1))) {
      const eq = item.indexOf("=");
      const name = (eq < 0 ? item : item.slice(0, eq)).trim();
      if (!/^[A-Za-z_]\w*$/.test(name)) continue;
      const v = eq < 0 ? next : evalInt(item.slice(eq + 1), into, macros);
      if (v === null) { into.set(name, null); next = null; continue; }
      into.set(name, into.has(name) && into.get(name) !== v ? null : v);
      next = v + 1;
    }
    re.lastIndex = end;
  }
}

class Resolver {
  constructor(macros, typedefs, tablesByFile, headerTables, constsByFile, headerConsts, file = null, fn = null) {
    this.macros = macros;
    this.typedefs = typedefs;
    this.tablesByFile = tablesByFile;
    this.headerTables = headerTables;
    this.constsByFile = constsByFile;
    this.headerConsts = headerConsts;
    this.file = file;
    this.fn = fn;
  }

  /* The same resolver reading ONE scope: the function's own tables, then the file's, then a header's — which
     is what C scoping already says, and the only reason two files may each spell a table `NAMES[]`. */
  for(file, fn = null) {
    return new Resolver(this.macros, this.typedefs, this.tablesByFile, this.headerTables, this.constsByFile,
                        this.headerConsts, file, fn);
  }

  charConst(name) {
    const scoped = this.file && this.constsByFile.get(this.file);
    if (scoped && scoped.has(name)) return scoped.get(name);
    return this.headerConsts.get(name) || null;
  }

  table(name) {
    const scoped = this.file && this.tablesByFile.get(this.file);
    if (scoped) {
      const inFn = this.fn && scoped.perFn.get(this.fn);
      if (inFn && inFn.has(name)) return inFn.get(name);
      if (scoped.file.has(name)) return scoped.file.get(name);
    }
    return this.headerTables.get(name) || null;
  }

  rows(tableName) {
    const t = this.table(tableName);
    if (!t) return null;
    /* A table that defines its own X-macro carries the meaning WITH it — `#define X(n, m) n,` immediately above
       `EVENT_HANDLERS(X)` is what makes that initializer a list of names rather than a list of pairs. Read it
       from the initializer, layered over the file-wide macros, so the same X-macro list expanded three ways
       gives three different tables and each is right. */
    const local = new Map(this.macros);
    parseDefines(t.body, local);
    const body = expand(t.body.replace(/^[ \t]*#[^\n]*/gm, ""), local);
    return { fields: t.fields, isArray: t.isArray, body,
             rows: splitTop(body).map((r) => r.trim()).filter((r) => r.length) };
  }

  /* One `{ a, b, c }` read against a field list. */
  readRow(row, fields, field) {
    const open = row.indexOf("{");
    const end = matchAt(row, open);
    if (end < 0 || field === null) return null;
    const cells = splitTop(row.slice(open + 1, end - 1)).map((s) => s.trim());
    const idx = fields ? fields.indexOf(field) : -1;
    return idx < 0 || idx >= cells.length ? null : [cells[idx]];
  }

  /* The values a table's COLUMN holds — `ARIA[i].member`, `HL_M[i].name` — or, with no field, the values of a
     flat array of strings: `VALIDITY_FLAG_NAMES[i]`, `DSD_ENUM_IDL[i]`, `EH_NAME[i]`. */
  column(tableName, field) {
    const t = this.rows(tableName);
    if (!t) return null;
    const out = [];
    /* A non-array definition is ONE row — `static const ByteReaderIface X = { … }` — so its fields are read
       straight off the initializer rather than off a row inside it. */
    if (t.isArray === false) return this.readRow(`{${t.body}}`, t.fields, field);
    for (const row of t.rows) {
      let cell = row;
      if (row.trim().startsWith("{")) {
        const one = this.readRow(row, t.fields, field);
        if (!one) return null;                        /* a struct row read as a bare value, or no such field */
        cell = one[0];
      } else if (field !== null) {
        return null;                                  /* a field asked of a flat array */
      }
      out.push(cell);
    }
    return out;
  }

  /* The member NAMES an expression can evaluate to, or null when it cannot be decided statically. `locals` maps
     an identifier to the expressions assigned to it in the enclosing function, which is what carries
     `a = JS_NewAtom(ctx, EH_NAME[i]); JS_DefinePropertyGetSet(ctx, target, a, …)`. */
  strings(exprIn, locals, depth = 0) {
    if (depth > 8) return null;
    let e = expand(exprIn, this.macros).trim();
    for (;;) {
      const before = e;
      e = e.trim();
      e = e.replace(/^\(\s*(?:JSValue|JSValueConst|const\s+char\s*\*|char\s*\*|void\s*\*)\s*\)\s*/, "");
      if (e.startsWith("(") && matchAt(e, 0) === e.length) e = e.slice(1, -1);
      if (e === before) break;
    }
    const lit = e.match(STRING_RE);
    if (lit) return [lit[1]];
    /* adjacent string-literal concatenation: "a" "b" */
    if (/^\s*"(?:[^"\\]|\\.)*"(\s*"(?:[^"\\]|\\.)*")+\s*$/.test(e))
      return [[...e.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((x) => x[1]).join("")];
    const wrap = e.match(/^(JS_NewAtom|JS_NewAtomLen|JS_ValueToAtom)\s*\(/);
    if (wrap) {
      const end = matchAt(e, e.indexOf("("));
      if (end < 0) return null;
      const args = splitTop(e.slice(e.indexOf("(") + 1, end - 1));
      return args.length >= 2 ? this.strings(args[1], locals, depth + 1) : null;
    }
    const cond = splitTop(e, "?");
    if (cond.length === 2) {
      const arms = splitTop(cond[1], ":");
      if (arms.length === 2) {
        const a = this.strings(arms[0], locals, depth + 1), b = this.strings(arms[1], locals, depth + 1);
        return a && b ? [...a, ...b] : null;
      }
    }
    const idx = e.match(/^([A-Za-z_]\w*)\s*\[[^\]]*\]\s*(?:\.\s*([A-Za-z_]\w*))?$/);
    if (idx) {
      const cells = this.column(idx[1], idx[2] === undefined ? null : idx[2]);
      if (!cells) return null;
      const out = [];
      for (const c of cells) {
        const v = this.strings(c, locals, depth + 1);
        if (!v) return null;
        out.push(...v);
      }
      return out;
    }
    const bare = e.match(/^[A-Za-z_]\w*$/);
    if (bare) {
      if (locals && locals.has(e)) {
        const out = [];
        for (const assigned of locals.get(e)) {
          const v = this.strings(assigned, locals, depth + 1);
          if (!v) return null;
          out.push(...v);
        }
        return out.length ? out : null;
      }
      /* the file's (or a header's) string constant — the third spelling of a declared name, after the macro
         and the table; a LOCAL of the same name is the nearer scope and answered above */
      const c = this.charConst(e);
      if (c) return this.strings(c, locals, depth + 1);
    }
    return null;
  }

  /* A TABLE handed to an install helper — `element_declare_reflections(ctx, R_INPUT, n)` names one directly,
     `element_declare_reflections(ctx, HTML_IFACE[i].refl, …)` names a COLUMN of tables, one per interface the
     element-interface table lists. Returns the table names, or null. */
  tableRefs(exprIn, depth = 0) {
    if (depth > 4) return null;
    let e = expand(exprIn, this.macros).trim();
    for (;;) {
      const before = e;
      e = e.trim();
      e = e.replace(/^\(\s*const\s+[A-Za-z_]\w*\s*\*\s*\)\s*/, "");
      if (e.startsWith("(") && matchAt(e, 0) === e.length) e = e.slice(1, -1);
      if (e === before) break;
    }
    if (/^&?\s*[A-Za-z_]\w*$/.test(e)) {
      const nm = e.replace(/^&\s*/, "");
      return this.table(nm) ? [nm] : null;
    }
    const idx = e.match(/^([A-Za-z_]\w*)\s*\[[^\]]*\]\s*\.\s*([A-Za-z_]\w*)$/);
    if (idx) {
      const cells = this.column(idx[1], idx[2]);
      if (!cells) return null;
      const out = [];
      for (const c of cells) {
        const t = c.trim();
        if (/^\s*(NULL|0)\s*$/.test(t)) continue;     /* a row that declares no reflections */
        const refs = this.tableRefs(t, depth + 1);
        if (!refs) return null;
        out.push(...refs);
      }
      return out;
    }
    return null;
  }
}

/* ---- the install vocabulary ------------------------------------------------------------------------------ */

/* THE FORMS THAT DECLARE AN INSTALLATION. `target` and `name` are argument positions. `ambiguous` marks the two
   forms that are also how any ordinary object is built, and which therefore count only where the object they
   write on is one the corpus declares an interface for (see the header).
   `fn` is the position of the body, read only to catch a `js_noop` member — the lazy stub this audit exists to
   expose, and a ban that has to be expressed against the install FORM rather than against the file's text. */
/* WHAT KIND OF PROPERTY A FORM DEFINES, which is a second thing the IDL states and the audit was not reading.
   Web IDL §3.7.6 Attributes defines EVERY attribute as `PropertyDescriptor{[[Getter]], [[Setter]],
   [[Enumerable]]: true, [[Configurable]]: configurable}` — an ACCESSOR, with [LegacyUnforgeable] deciding
   only the `configurable` — while §3.7.7's operation and §3.7.5's constant are DATA properties. A member
   installed as the wrong one is INSTALLED: it fills its ABSENT row and the interface reads COMPLETE, while
   `Object.getOwnPropertyDescriptor(w, "document").get` is undefined where every browser has a function and
   `w.document = x` writes where the spec ignores it.
   THAT IS THE DEFECT CLASS NO GAP COUNT CAN HOLD: not a missing member, a wrong one. `typeof NodeFilter`
   answered "object" where §3.11.1 constructs a function, and nothing in this audit could have counted it —
   it was found by reading what the spec builds rather than which names were absent. This is the same
   question asked where the audit can answer it mechanically, for every member of every audited interface.
   A form whose kind this cannot state is left UNDECLARED and judged by nobody — the refusal every other
   unread fact in this file gets — and the caller counts those, because a form nobody can judge is a member
   nobody checks. */
const CALL_FORMS = new Map(Object.entries({
  idl_install_accessor:          { target: 1, name: 2, fn: 3, kind: "accessor" },
  /* Web IDL §3.4.10's [LegacyUnforgeable]: the same attribute, defined on the object that IMPLEMENTS the
     interface rather than on its prototype and non-configurable. It is an install like any other — where it
     LANDS is what differs, and the attribution graph already follows the object. */
  idl_install_accessor_unforgeable: { target: 1, name: 2, fn: 3, kind: "accessor" },
  idl_install_accessor_step:     { target: 1, name: 2, kind: "accessor" },
  idl_install_method:            { target: 1, name: 2, kind: "data" },
  idl_install_step_method:       { target: 1, name: 2, kind: "data" },
  idl_install_replaceable:       { target: 1, name: 2, fn: 3, kind: "accessor" },
  idl_install_replaceable_value: { target: 1, name: 2, kind: "accessor" },
  JS_DefinePropertyGetSet:       { target: 1, name: 2, fn: 3, kind: "accessor" },
  /* Web IDL §3.7.1 Interface object — "the property has attributes { [[Writable]]: true, [[Enumerable]]:
     false, [[Configurable]]: true }", a DATA property on the global. These two are core/dom/node.c's shared
     install helpers for one, and they are HERE for the reason every other shared helper is: the helper's own
     `JS_SetPropertyStr(ctx, global, name, ctor)` takes the name from a PARAMETER, so it is the one line that
     cannot resolve by construction. Unregistered, that line was reported UNRESOLVED against every interface
     whose row names node.c — a defect count with no root fix behind it, since no change to the C could have
     made the forwarding line name a member — while the CALL SITES that do name one (`"HTMLElement"`,
     `"Element"`, `"Document"`, `"Node"` and thirty more literals) were read by nobody. Registered, the
     forwarding line is skipped by the shared-helper rule and each caller is audited where its literal is. */
  node_install_interface:        { target: 1, name: 2, kind: "data" },
  node_install_interface_ctor:   { target: 1, name: 2, kind: "data" },
  /* Both write a DATA property, which is what makes them the two forms that can be an §3.7.6 violation: an
     IDL attribute is an accessor and nothing else. */
  JS_SetPropertyStr:             { target: 1, name: 2, fn: 3, ambiguous: true, kind: "data" },
  JS_DefinePropertyValueStr:     { target: 1, name: 2, fn: 3, ambiguous: true, kind: "data" },
}));

/* The forms that state an object is one a page reaches members ON without naming a member themselves. Which
   interface it is comes from the tag; these are read for the CONTRADICTION check the header describes — an
   object the C builds with no prototype and one of these nonetheless names cannot be both. */
const TARGET_FORMS = new Map(Object.entries({
  idl_interface_tag:          1,
  JS_SetPropertyFunctionList: 1,
  event_target_install_handlers: 1,
  byte_reader_install:        1,
}));

/* A `JSCFunctionListEntry` row. The name is the first argument of the entry macro; a table is read only when
   something hands it to JS_SetPropertyFunctionList, so a table that is declared and never installed is not
   credited with anything. */
const ENTRY_RE = /\b(JS_C(?:FUNC|GETSET)\w*_DEF|JS_PROP_\w+_DEF|JS_ALIAS\w*_DEF|JS_OBJECT_DEF)\s*\(/g;
/* The same question of a JSCFunctionListEntry row, answered by the macro that builds it. `JS_ALIAS*_DEF` and
   `JS_OBJECT_DEF` are undeclared: an alias is a second name for a member defined elsewhere, and an object
   entry is a namespace rather than a member. */
const ENTRY_KIND = (macro) => macro.startsWith("JS_CGETSET") ? "accessor"
  : macro.startsWith("JS_CFUNC") || macro.startsWith("JS_PROP_") ? "data" : null;

/* THE TWO-HALVED INSTALL FORMS — a REGISTRY that is filled once per agent and installed once per realm. Both
   halves are named here because the names are only ever in the table the DECLARE half is handed; the install
   half carries a handle, which has nothing to read, and its own
   `idl_install_accessor(ctx, proto, g_reflect[base + i].idl, …)` is the forwarding line of a shared helper,
   audited at the declare site exactly like every other helper's forwarded parameter.
     - the REFLECTIONS: `element_declare_reflections(ctx, table, n)`, whose rows' `idl` field is literally the
       member's IDL name (every HTML content-attribute reflection this engine has).
     - the BYTE READERS: `byte_reader_declare(ctx, &iface)`, where the interface record's `readers` field names
       the table whose rows carry `text`, `json`, `arrayBuffer`, `blob` and `bytes` — the five members Blob,
       File, Request and Response all get from the one component. */
/* THE CONDITIONAL MEMBER — a member the flattened IDL has and the spec's PROSE makes conditional, under a
   condition this user agent does not meet. `idl_members_excluded(ctx, proto, "Iface", table, n, why)` is where
   the engine says so, and reading it is what keeps such a member out of the ABSENT list without the auditor
   holding any list of its own: the knowledge lives with the members, in the component, as code that asserts
   itself per realm. The caller checks it back against the corpus — a name declared here that the IDL no longer
   carries, or that the component nonetheless installs, is an error rather than a line nobody revisits. */
const EXCLUDED_FORM = { fn: "idl_members_excluded", iface: 2, table: 3, why: 5 };

const TABLE_FORMS = [
  /* a reflection IS §3.7.6's attribute — [Reflect] changes where the value comes from, not what kind of
     property the member is — and a byte reader is one of §4's five operations. */
  { declare: "element_declare_reflections", install: "element_install_reflections", arg: 1, field: "idl",
    target: 1, handle: 2, kind: "accessor" },
  { declare: "byte_reader_declare", install: "byte_reader_install", arg: 1, via: "readers", field: "name",
    target: 1, handle: 2, kind: "data" },
];

/* ---- which INTERFACE an object is ------------------------------------------------------------------------ */

/* THE MEMBER IS THE INTERFACE'S, NOT THE FILE'S. Reading installation out of the install construct fixed WHAT
   is installed and left WHERE untouched: the audit credited every member any of a row's files installed to that
   row's interface, so html_form.c's `value` — installed on HTMLTextAreaElement.prototype — counted for
   HTMLInputElement, and element_internals.c could not be named in any row at all because its `validity`,
   `labels` and `form` land on ElementInternals. A false ABSENT buries a real gap in noise; a false COMPLETE
   HIDES one, which is strictly worse.
   The interface-scoped fact is already in the components, because Web IDL §3.7.3 makes every interface
   prototype object carry an @@toStringTag whose value is the interface's identifier — `idl_interface_tag(ctx,
   proto, "Element")` is that member, installed because the spec says so and not for this auditor's benefit.
   §3.7.3's [Global] rule puts a [Global] interface's members on the global OBJECT instead, which is an instance
   and therefore carries no tag of its own — the statement for it is already in the code and needs no second
   spelling: `JS_SetGlobalClass(ctx, g_window_class)` says the global IS that class, and the class's prototype
   slot holds the object window.c tagged "Window". Without that edge every member of the largest interface in
   the engine — `fetch`, `location`, `setTimeout`, the ninety handler attributes — would be unattributed.
   Two more forms carry the fact without stating it: `idl_interface_object(ctx, "Response", proto)` builds
   §3.7.1's interface OBJECT — where the STATIC members go — for a named interface, and `JS_SetConstructor(ctx,
   ctor, proto)` says two objects are the two halves of one interface. Neither is read as a NAME: they LINK the
   two objects, so the ctor's statics are attributed through the prototype's tag, and the name
   idl_interface_object was given is then a CHECK against it (see `tagChecks`) rather than a second source of
   truth that could disagree in silence. */
const IFACE_SEEDS = [
  { fn: "idl_interface_tag",  obj: 1, iface: 2 },
  /* §3.13.1's CLASS STRING ON A NAMESPACE OBJECT is the same KIND of statement seen from the other side, which
     is why it seeds the same table. An interface's members are installed on the object the §3.7.3 tag names —
     its interface prototype object — and a NAMESPACE's members are installed on the object the §3.13.1 tag
     names, the namespace object itself; §3.13.1 steps 2-4 put the attributes, the operations and the constants
     directly on it. So "properties installed on the tagged object are this definition's members" is true of
     both, and the two differ only in what the tagged object IS — which is exactly why the C says it with two
     functions instead of one, so a reader of either side can tell which kind it is looking at. */
  { fn: "idl_namespace_tag",  obj: 1, iface: 2 },
];
const IFACE_OBJECT = { fn: "idl_interface_object", iface: 1, obj: 2 };
/* §3.11.1's LEGACY CALLBACK INTERFACE OBJECT, which is a SEED and not a link. A callback interface has no
   interface prototype object — Web IDL §3.7.3's tag is that object's, so there is none anywhere on this one —
   and the constants §3.7.5 defines on it are real members a page reads (`NodeFilter.SHOW_ELEMENT`). So the
   call that builds it is the only statement of which interface those members belong to, and the object it
   states about is the call's RESULT rather than one of its arguments. */
const CALLBACK_IFACE_OBJECT = { fn: "idl_callback_interface_object", iface: 1 };
const CTOR_LINK = { fn: "JS_SetConstructor", ctor: 1, proto: 2 };

/* AN OBJECT THE CORPUS DECLARES IS NOT AN INTERFACE PROTOTYPE OBJECT. Web IDL gives three page-reachable
   objects whose properties are NOBODY'S IDL members — §3.7.4's named properties object, §3.7.9.2's iterator
   prototype object and §3.7.10.2's asynchronous iterator prototype object — and a property installed on one is
   neither a gap to implement nor a member to credit. Left unstated it is indistinguishable from a target the
   attribution FAILED on, which is exactly where §3.7.10.2's `next` and `return` sat: two rows of "installed
   members whose target interface could not be decided", for an object whose interface was never the question.
   So the C says which kind it is at the object's own construction, the same way §3.7.3's tag says an interface
   prototype's identity, and the statement is read here rather than guessed from a name.
   IT IS TWO-SIDED, which is what keeps it from being an allowlist: each kind names the members Web IDL defines
   on it, and anything else installed there is an error rather than a line nobody revisits. */
const NON_INTERFACE_FORMS = new Map(Object.entries({
  idl_async_iterator_tag: { obj: 1, kind: "§3.7.10.2 asynchronous iterator prototype object",
                            members: ["next", "return"] },
}));

/* ---- the scan ------------------------------------------------------------------------------------------- */

function callSites(masked, name) {
  const out = [];
  const re = new RegExp(`\\b${name}\\s*\\(`, "g");
  let m;
  while ((m = re.exec(masked))) {
    const open = masked.indexOf("(", m.index);
    const end = matchAt(masked, open);
    if (end < 0) continue;
    out.push({ at: m.index, args: splitTop(masked.slice(open + 1, end - 1)), text: masked.slice(m.index, end) });
  }
  return out;
}

/* The top-level function bodies, with their parameter names. A function is a `{` whose header ends in `)`;
   an array initializer's `{` follows a `=` and is skipped.
   A `{` THAT IS NEITHER IS THE ONE THIS MUST NOT SWALLOW. Dropping it drops a WHOLE FUNCTION BODY, and every
   install written in it then does not exist as far as the audit is concerned — which already happened once,
   when a `#define` between the last `;` and a function's header made that header unreadable and took
   navigator.c's thirteen environment members and screen.c's nine with it, each reported ABSENT while it was
   shipping. The C facts that make a top-level `{` genuinely not a function are stated below and dropped
   silently because there is nothing there; anything else is UNCLASSIFIED and recorded.
   Unlike localDefs' refusal, this one is counted WHERE IT IS PARSED and not where an answer is depended on,
   and the difference is not an inconsistency: a name whose value could not be read still EXISTS, so a later
   query can be told it is standing on unread ground, but a function that was never seen leaves no member to
   ask about. There is no dependency point to attach to, because the consequence is an absence. */
const AGGREGATE_HEADER = /(?:=|\bstruct\b[\w \t]*|\bunion\b[\w \t]*|\benum\b[\w \t]*|\btypedef\b[\s\S]*)\s*$/;
function functions(masked) {
  const out = [];
  out.unclassified = [];
  let depth = 0, bodyStart = -1, prevBoundary = 0;
  for (let i = 0; i < masked.length; i++) {
    const c = masked[i];
    if (c === '"' || c === "'") {
      i++;
      while (i < masked.length && masked[i] !== c) { if (masked[i] === "\\") i++; i++; }
      continue;
    }
    /* A PREPROCESSOR LINE ENDS THE HEADER. `#define NAV_UA_REST "…"` sits between the last `;` and the next
       function, and treating it as part of that function's header threw the function away — navigator.c's
       thirteen environment members and screen.c's nine went with it, reported ABSENT while they were shipping. */
    if (c === "#" && depth === 0 && /(^|\n)[ \t]*$/.test(masked.slice(Math.max(0, i - 40), i))) {
      while (i < masked.length && masked[i] !== "\n") i++;
      prevBoundary = i + 1;
      continue;
    }
    if (c === "{") {
      if (depth === 0) {
        const header = masked.slice(prevBoundary, i);
        /* a function's header ends in its parameter list; a `#define F(a) {…}` ends the same way and is not one */
        if (/\)\s*$/.test(header)) bodyStart = i;
        else {
          bodyStart = -1;
          if (!AGGREGATE_HEADER.test(header)) out.unclassified.push({ at: i, header: header.trim().slice(-80) });
        }
      }
      depth++;
    } else if (c === "}") {
      depth--;
      if (depth === 0) {
        if (bodyStart >= 0) {
          const header = masked.slice(prevBoundary, bodyStart);
          const po = header.lastIndexOf("(");
          const params = po >= 0
            ? splitTop(header.slice(po + 1, header.lastIndexOf(")")))
                .map((p) => (p.trim().match(/([A-Za-z_]\w*)\s*(\[[^\]]*\])?$/) || [])[1])
                .filter(Boolean)
            : [];
          const nameM = header.slice(0, po < 0 ? header.length : po).match(/([A-Za-z_]\w*)\s*$/);
          out.push({ name: nameM ? nameM[1] : "", params, start: bodyStart, end: i + 1,
                     body: masked.slice(bodyStart, i + 1) });
        }
        prevBoundary = i + 1;
        bodyStart = -1;
      }
    } else if (depth === 0 && c === ";") prevBoundary = i + 1;
  }
  return out;
}

/* `ident = expr` inside one function body — the single hop that carries a name from where it is built to where
   it is installed (`a = JS_NewAtom(ctx, EH_NAME[i])`). Read by statement and then by DECLARATOR, because
   `JSAtom a = JS_NewAtom(ctx, "aborted"), r = JS_NewAtom(ctx, "reason");` is two initialisations and reading it
   as one gave `a` a value with `r`'s whole declarator glued to the end of it. Every assignment to an identifier
   is kept, so a name built two ways resolves to both or to nothing. */
/* EVERY ASSIGNMENT WITH ITS OFFSET, because a variable is not one object. `element_internals_install_protos`
   spells `proto` three times over — ElementInternals.prototype, then CustomStateSet.prototype, then
   ValidityState.prototype — and one node per NAME made those one object carrying three interface tags, which
   credits every member of each to all three. That is the file-granular lie one level further down, so a
   variable is keyed by the ASSIGNMENT that produced the value: a use at offset X belongs to the last definition
   at or before X (version -1 is the value the variable arrived with, which for a parameter is the caller's). */
function localDefs(body) {
  const map = new Map();
  /* Statements at ANY depth — a `{ … }` block inside a function is still this function's code, and counting
     braces hid html_element.c's `JSAtom a = JS_NewAtom(ctx, "content")` inside the block that installs it. */
  const stmts = splitTop(body, ";", false);
  let stmtAt = 0;
  /* WHAT THIS SAW AND COULD NOT ATTRIBUTE TO A NAME — the ledger the analysis above reads so that "there is no
     definition here" and "there is one I could not read" stop being the same answer. An LHS that is not a bare
     identifier is one of two things, and which is a C FACT rather than a judgement:
       - a path through `->`, or a leading `*`, writes the object a POINTER points at. The variable's own value
         is unchanged, so there is genuinely no definition here and nothing to record. 3645 of the corpus's
         4628 such assignments are this. (A leading `*` with a TYPE before it — `JSValue *p = &v` — is a
         declaration and keeps its definition; the dereference is the one that starts with the `*`.)
       - a path of `[…]`/`.` on a bare name changes what that name holds if it is an ARRAY or a STRUCT and does
         not if it is a POINTER. This parser does not read declarations, so it cannot say which: 983 of them,
         over 223 distinct names. That is a REFUSAL, recorded by NAME and OFFSET so a query that depends on the
         name at or after this point can be told it is standing on something unread rather than on nothing. */
  const unread = new Map();
  const unreadable = (lhsText, at) => {
    const s = lhsText.trim();
    if (s.includes("->") || s.startsWith("*")) return;
    const base = s.match(/^([A-Za-z_]\w*)\s*[[.]/);
    if (!base) return;
    if (!unread.has(base[1])) unread.set(base[1], []);
    unread.get(base[1]).push(at);
  };
  /* The declarator list, split on commas — but NOT when the statement is an aggregate initialiser, whose
     commas separate ELEMENTS. Braces are not counted here either: a statement that opens a block carries an
     unmatched `{`, and counting it hid the second declarator of `JSAtom a = …, r = …` inside such a block. */
  const take = (text, base) => {
    const decls = /=\s*\{/.test(text) ? [text] : splitTop(text, ",", false);
    let declAt = base;
    for (const decl of decls) {
      const eq = decl.indexOf("=");
      if (eq >= 0 && !"=!<>+-*/&|^%".includes(decl[eq + 1]) && !"=!<>+-*/&|^%".includes(decl[eq - 1])) {
        const lhsText = decl.slice(0, eq);
        const deref = lhsText.trim().startsWith("*");
        const lhs = deref ? null : lhsText.trim().match(/([A-Za-z_]\w*)$/);
        if (lhs) {
          if (!map.has(lhs[1])) map.set(lhs[1], []);
          map.get(lhs[1]).push({ at: declAt + eq, rhs: decl.slice(eq + 1) });
        } else if (!deref) unreadable(lhsText, declAt + eq);
      }
      declAt += decl.length + 1;                  /* the comma splitTop removed */
    }
  };
  /* A CONTROL-FLOW HEADER IS ITS OWN STATEMENTS, AND WHAT FOLLOWS IT IS ANOTHER. `for (i = 0; i < n; i++)`
     keeps its semicolons INSIDE parens, so splitting the body on top-level semicolons leaves the whole header
     glued to the first declaration in the loop — and reading that statement's FIRST `=` then answered with the
     loop counter and lost the declaration entirely. `JSValue proto = realm_value_get(ctx,
     g_proto_slot[IFACES[i].slot])` and `JSValue proto = JS_GetClassProto(ctx, classes[i])` are exactly those
     statements, which is why css_rule.c's fourteen §6.4 interface objects and element_internals.c's three
     §4.13.7 ones were built over an object no §3.7.3 tag reached: the object had no definition at all, so
     there was nothing for a tag to travel along. The header's own parts are read too — dropping them would
     trade one lost definition for another. */
  const HEADER_RE = /^[\s{}]*(?:else\s+)?\b(?:for|if|while|switch)\s*\(/;
  for (const stmt of stmts) {
    let head = 0;
    for (;;) {
      const m = HEADER_RE.exec(stmt.slice(head));
      if (!m) break;
      const open = head + m[0].length - 1;
      const end = matchAt(stmt, open);
      if (end < 0) break;
      let partAt = open + 1;
      for (const part of splitTop(stmt.slice(open + 1, end - 1), ";", false)) {
        take(part, stmtAt + partAt);
        partAt += part.length + 1;
      }
      head = end;
    }
    take(stmt.slice(head), stmtAt + head);
    stmtAt += stmt.length + 1;                    /* the semicolon splitTop removed */
  }
  for (const defs of map.values()) defs.sort((a, b) => a.at - b.at);
  for (const ats of unread.values()) ats.sort((a, b) => a - b);
  map.unread = unread;                            /* the refusals, read by the query that would depend on them */
  return map;
}

/* `ident = expr` inside one function body — the single hop that carries a name from where it is built to where
   it is installed (`a = JS_NewAtom(ctx, EH_NAME[i])`). Read by statement and then by DECLARATOR, because
   `JSAtom a = JS_NewAtom(ctx, "aborted"), r = JS_NewAtom(ctx, "reason");` is two initialisations and reading it
   as one gave `a` a value with `r`'s whole declarator glued to the end of it. Every assignment to an identifier
   is kept, so a name built two ways resolves to both or to nothing. */
function localAssignments(body) {
  const map = new Map();
  for (const [name, defs] of localDefs(body)) map.set(name, defs.map((d) => d.rhs));
  return map;
}

/* A NAME A FUNCTION WAS GIVEN AND MINTED AN ATOM OUT OF IS STILL THE NAME IT WAS GIVEN, and which parameter it
   came from is the question the derivation below asks. Reading only the identifier standing AT the install
   site answered it for `nav_env(ctx, nav, name, …)` and not for this engine's own accessor installers, which
   write `JSAtom a = JS_NewAtom(ctx, name); JS_DefinePropertyGetSet(ctx, target, a, …)` — so idl_define_accessor
   and idl_define_replaceable were not derived as install forms, and their forwarding lines then reported
   themselves as constructs whose member name is not statically resolvable. Two of the audit's own install
   helpers, named by the audit's own gap report, for the sake of one hop.
   THE HOP IS AN IDENTITY AND NOTHING ELSE: an atom mint over a string is that string (the same rule
   Resolver.strings already reads for JS_NewAtom in the other direction), and a plain assignment carries it.
   An expression that BUILDS a different string — an snprintf, a concatenation, a suffix — reaches no parameter
   and the site stays UNRESOLVED, which is the honest answer and not a silent credit. Two assignments naming
   two different parameters is not one parameter either, and refuses. */
const ATOM_MINT_RE = /^(JS_NewAtom|JS_NewAtomLen|JS_ValueToAtom)\s*\(/;
function paramBehind(fn, locals, expr, depth = 0) {
  if (depth > 4) return -1;
  const e = stripCast(String(expr || ""));
  const direct = fn.params.indexOf(e);
  if (direct >= 0) return direct;
  if (ATOM_MINT_RE.test(e)) {
    const open = e.indexOf("("), end = matchAt(e, open);
    if (end !== e.length) return -1;
    const args = splitTop(e.slice(open + 1, end - 1));
    return args.length >= 2 ? paramBehind(fn, locals, args[1], depth + 1) : -1;
  }
  if (/^[A-Za-z_]\w*$/.test(e) && locals.has(e)) {
    let one = -1;
    for (const rhs of locals.get(e)) {
      const p = paramBehind(fn, locals, rhs, depth + 1);
      if (p < 0 || (one >= 0 && p !== one)) return -1;
      one = p;
    }
    return one;
  }
  return -1;
}

/* THIS KNOWS TWO TYPE NAMES OUT OF THE NINE THE CORPUS CASTS TO, and that narrowness is recorded here rather
   than in a verdict because it is measured to cost nothing TODAY: the census is three install-form target
   arguments carrying a cast this cannot strip, all three `(JSClassID)` in realm.c, all three on a PARAMETER
   rather than a named slot, so all three resolve to nothing either way. It cannot be widened to "any
   parenthesised identifier" without eating `(f)(x)`, and there is no lexical way to know a type name from a
   value name — so the honest state is a narrow stripper whose blind spot is written down and whose cost is a
   number somebody re-measured, not a TODO. If a target ever arrives behind `(JSValueConst *)` or a typedef,
   the install lands as UNRESOLVED with its file and line, which is the loud failure and not a silent one. */
function stripCast(e) {
  let s = e.trim();
  for (;;) {
    const before = s;
    s = s.replace(/^\(\s*(?:JSValue|JSValueConst)\s*\)\s*/, "").trim();
    if (s.startsWith("(") && matchAt(s, 0) === s.length) s = s.slice(1, -1).trim();
    if (s === before) return s;
  }
}

/* `JS_DupValue(ctx, x)` IS x — a reference count is not a different object, and `JS_SetClassProto(ctx, cls,
   JS_DupValue(ctx, html_p))` is the realm taking its own reference to the prototype it was just handed. */
function stripDup(e) {
  const s = stripCast(e);
  const m = s.match(/^JS_DupValue\s*\(/);
  if (!m) return s;
  const end = matchAt(s, s.indexOf("("));
  if (end !== s.length) return s;
  const args = splitTop(s.slice(s.indexOf("(") + 1, end - 1));
  return args.length === 2 ? stripDup(args[1]) : s;
}

/* String and char literals blanked to spaces WITHOUT moving a byte, so a brace inside a message ("a <template>
   element {…}") cannot be mistaken for a block. Every offset into this is an offset into the original, which is
   what lets the guard's own literals be read back out of the unblanked text. */
function blankLiterals(text) {
  const out = text.split("");
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c !== '"' && c !== "'") continue;
    let j = i + 1;
    while (j < text.length && text[j] !== c) { if (text[j] === "\\") j++; j++; }
    for (let k = i + 1; k < Math.min(j, text.length); k++) out[k] = " ";
    i = j;
  }
  return out.join("");
}

/* WHAT GUARDS THIS SITE. A member installed UNCONDITIONALLY on a target that is several tagged prototypes (a
   shared installer's parameter, a loop variable over the element-interface table) really does land on all of
   them, so crediting each is the truth. Under a condition it lands on SOME of them — html_element.c installs
   `relList` on four of the sixty prototypes its loop walks and `sizes` on one — and crediting all sixty would
   put `sizes` on HTMLImageElement, which is the false COMPLETE this whole mechanism exists to remove.
   So the guard is READ rather than merely noticed, the same way an install construct is: a condition made
   entirely of POSITIVE string equalities (`!strcmp(HTML_IFACE[i].iface, "HTMLAnchorElement") ||
   !strcmp(…, "HTMLAreaElement")`) names exactly which of them, and those names are the answer. A NEGATED
   comparison means every interface EXCEPT the one it names, so narrowing to it would be precisely backwards —
   a guard with one of those in it yields no names and the site is reported UNATTRIBUTED instead of guessed.
   Literals that are not interface names (a tag name, an enum value) simply intersect with nothing, which is
   the same answer.
   Returns null when nothing guards the site, otherwise `{ only }` — the names, or null for a guard whose
   subject this cannot read. A guard the ENCLOSING LOOP applies with a `continue` (`if (!(EH_MASK[i] & mask))
   continue;`) is not one of these, and is the coarseness the module header names. */
const PLAIN = new WeakMap();
const plainOf = (f) => {
  if (!PLAIN.has(f)) PLAIN.set(f, blankLiterals(f.body));
  return PLAIN.get(f);
};

function guardAt(body, plain, at) {
  const heads = [];
  let i = at;
  for (;;) {
    let s = i;
    while (s > 0 && !";{}".includes(plain[s - 1])) s--;
    heads.push(body.slice(s, i));
    if (s === 0) break;
    /* out to the enclosing block: back over balanced braces to the `{` that opened it */
    i = s - 1;
    if (plain[i] !== "{") {
      let d = 0, k = i;
      for (; k >= 0; k--) {
        if (plain[k] === "}") d++;
        else if (plain[k] === "{") { if (d === 0) break; d--; }
      }
      if (k < 0) break;
      i = k;
    }
    if (heads.length > 8) break;
  }
  const guard = heads.find((h) => /\bif\s*\(/.test(h));
  if (guard === undefined) return null;
  const only = [];
  let usable = false;
  const RE = /(!\s*)?\bstrcmp\s*\(/g;
  let m;
  while ((m = RE.exec(guard))) {
    const open = m.index + m[0].length - 1;
    const end = matchAt(guard, open);
    if (end < 0) return { only: null };
    const args = splitTop(guard.slice(open + 1, end - 1));
    const after = guard.slice(end).match(/^\s*([=!]=)\s*0/);
    if (!m[1] && !(after && after[1] === "==")) return { only: null };   /* a NEGATED comparison */
    const lit = (args[1] || "").trim().match(STRING_RE);
    if (!lit) return { only: null };
    only.push(lit[1]);
    usable = true;
  }
  return { only: usable ? only : null };
}

/* ---- the public answer ------------------------------------------------------------------------------------ */

/* Every .c and .h under `root`, so a macro or a table can be followed to the header it is defined in — the
   §4.10.21 validity-state list lives in constraint_validation.h and the members it names are installed from
   element_internals.c. */
export function loadEnvironment(root) {
  const macros = new Map(), typedefs = new Map(), sources = new Map();
  const walk = (dir) => {
    for (const ent of readdirSync(dir)) {
      const p = join(dir, ent);
      if (statSync(p).isDirectory()) { walk(p); continue; }
      if (![".c", ".h"].includes(extname(p))) continue;
      const orig = readFileSync(p, "utf8");
      const masked = mask(orig);
      sources.set(p, { orig, masked });
      collectMacros(masked, macros);
      collectTypedefs(masked, typedefs);
    }
  };
  walk(root);
  /* AFTER the walk, because an enumerator's initializer may name a macro from any file. */
  const ints = new Map();
  for (const { masked } of sources.values()) collectEnums(masked, ints, macros);
  const fnsOf = new Map();          /* path -> functions() */
  const byName = new Map();         /* function name -> its parameter list (for the argument edge) */
  for (const [path, { masked }] of sources) {
    const fs = functions(masked);
    fnsOf.set(path, fs);
    for (const f of fs) if (f.name && !byName.has(f.name)) byName.set(f.name, f.params);
  }
  const tablesByFile = new Map(), headerTables = new Map();
  const constsByFile = new Map(), headerConsts = new Map();
  for (const [p, { masked }] of sources) {
    const t = scopeTables(masked, typedefs, fnsOf.get(p));
    tablesByFile.set(p, t);
    if (extname(p) === ".h") for (const [k, v] of t.file) headerTables.set(k, headerTables.has(k) ? null : v);
    const c = new Map();
    collectCharConsts(masked, c);
    constsByFile.set(p, c);
    if (extname(p) === ".h") for (const [k, v] of c) headerConsts.set(k, headerConsts.has(k) ? null : v);
  }
  const resolver = new Resolver(macros, typedefs, tablesByFile, headerTables, constsByFile, headerConsts);
  /* THE SHARED INSTALL HELPERS. A function that forwards one of its own PARAMETERS into a member-name position
     IS an install form, at that parameter's position — navigator.c's `nav_env(ctx, nav, "userAgent", …)`
     installs a member and spells the name only at the call site, and node.c's `mixin_install(ctx, proto,
     CHILD_NODE_MIXIN, n)` installs a whole mixin whose names are a column of the TABLE it is handed. Both
     shapes are the same fact — the name comes from the caller — so both are derived here rather than listed,
     and a wrapper around a wrapper is reached by iterating to a fixed point. */
  const forms = new Map(CALL_FORMS);
  for (let pass = 0; pass < 4; pass++) {
    let grew = false;
    for (const { masked } of sources.values()) {
      for (const fn of functions(masked)) {
        if (!fn.name || forms.has(fn.name)) continue;
        let fnLocals = null;
        const localsOf = () => (fnLocals || (fnLocals = localAssignments(fn.body)));
        for (const [callee, form] of forms) {
          for (const site of callSites(fn.body, callee)) {
            const nameArg = stripCast(site.args[form.name === undefined ? form.tableArg : form.name] || "");
            const col = nameArg.match(/^([A-Za-z_]\w*)\s*\[[^\]]*\]\s*\.\s*([A-Za-z_]\w*)$/);
            const via = col ? fn.params.indexOf(col[1]) : -1;
            const direct = via >= 0 ? -1 : paramBehind(fn, localsOf(), nameArg);
            if (direct < 0 && via < 0) continue;
            const tgt = stripCast(site.args[form.target] || "");
            /* A WRAPPER DEFINES WHAT ITS CALLEE DEFINES. The kind rides the derivation for the same reason
               the name position does: `nav_env` installs whatever idl_install_accessor installs. */
            const derived = { target: fn.params.indexOf(tgt), ambiguous: !!form.ambiguous, kind: form.kind };
            if (direct >= 0) derived.name = direct;
            else { derived.tableArg = via; derived.field = col[2]; }
            forms.set(fn.name, derived);
            grew = true;
            break;
          }
          if (forms.has(fn.name)) break;
        }
      }
    }
    if (!grew) break;
  }

  const key = (path, fnName, v) => `${path}::${fnName}::${v}`;

  /* ---- THE OBJECT WITH NO PROTOTYPE, and the assertion it makes ------------------------------------------ */

  /* WHICH objects a member can be installed on is decided by the interface solve below — an object a page can
     reach is an object the corpus DECLARES an interface for, and there is no second solve (see the header).
     The C states the same fact from the other end at the CONSTRUCTION, and that statement is worth asserting
     rather than believing: `JS_NewObjectProto(ctx, JS_NULL)` builds an object with NO PROTOTYPE, which is
     reachable from nothing the page holds — a field read off one cannot reach the page, which is exactly why
     the components build their per-realm state that way (viewport.c's §13.1 resize latch, user_activation.c's
     §6.4.1 timestamps, autofocus.c's §6.6.7 list, focus_event.c's converted dictionary, and `idl_slots_new`,
     which IS that call with a name on it).
     So an object built with no prototype can never be an interface prototype object, and an install form that
     names one is a CONTRADICTION — either the object should have been built as a prototype or the install is
     on the wrong object. It is reported with file, line and form and never tolerated. This is the second
     direction the classification is checked from: the declaration says an undeclared object is a record, and
     the construction says a no-prototype object is a record, and the two are read against each other. */
  const NULL_PROTO_RE = /^\s*JS_NewObjectProto\s*\(\s*[^,]+,\s*JS_NULL\s*\)\s*$/;
  const recordCtors = new Set();
  const isRecordExpr = (e) => {
    const s = stripCast(e);
    if (NULL_PROTO_RE.test(s)) return true;
    const call = s.match(/^([A-Za-z_]\w*)\s*\(/);
    return !!(call && recordCtors.has(call[1]) && matchAt(s, s.indexOf("(")) === s.length);
  };
  /* A function whose EVERY return is such a construction builds records and nothing else, which is what
     `idl_slots_new` is; a wrapper around one is reached by iterating to a fixed point. */
  for (let pass = 0; pass < 4; pass++) {
    let grew = false;
    for (const fs of fnsOf.values())
      for (const f of fs) {
        if (!f.name || recordCtors.has(f.name)) continue;
        const rets = [...f.body.matchAll(/\breturn\s+([^;]+);/g)].map((m) => m[1]);
        if (rets.length && rets.every((r) => isRecordExpr(r))) { recordCtors.add(f.name); grew = true; }
      }
    if (!grew) break;
  }
  /* A variable EVERY assignment of which is such a construction. Every, because a variable is not one object:
     a name reused for a record in one branch and a prototype in another is neither, and answering with the
     first would manufacture the contradiction this reports. */
  const noProtoVars = new Set();
  for (const [path] of sources)
    for (const f of fnsOf.get(path))
      for (const [lhs, rhss] of localAssignments(f.body))
        if (rhss.length && rhss.every((r) => isRecordExpr(r))) noProtoVars.add(key(path, f.name, lhs));

  const recordContradictions = [];
  for (const [path, { orig }] of sources)
    for (const f of fnsOf.get(path)) {
      const named = (t) => /^[A-Za-z_]\w*$/.test(t) && noProtoVars.has(key(path, f.name, t));
      const check = (callee, pos) => {
        for (const site of callSites(f.body, callee)) {
          const t = stripDup(site.args[pos] || "");
          if (named(t))
            recordContradictions.push({ file: path, line: lineOf(orig, f.start + site.at), form: callee, obj: t });
        }
      };
      for (const [callee, form] of forms)
        if (!form.ambiguous && form.target >= 0) check(callee, form.target);
      for (const [callee, pos] of TARGET_FORMS) check(callee, pos);
    }

  /* ---- WHICH INTERFACE EACH OBJECT IS, solved over the whole program ------------------------------------- */

  /* THE ONE SOLVE, and every question about an install target is asked of it: which interface a member lands
     on, and — because an object no declaration reaches is a record — whether it lands on an interface at all.
     A DELETED second solve carried "is this a target" as a SYMMETRIC fact, and symmetry is exactly what a
     shared installer's parameter cannot carry: that parameter is EVERY object its callers hand it, so the facts
     of those objects flow INTO it (and the members it installs land on all of them), but the union it
     accumulates must never flow back OUT to one particular caller's object — which is how one real prototype
     handed to `realm_value_set` told every per-realm record in the engine that it was a prototype too, and
     would tell window.c's global that it is also an HTMLElement. So the argument edge is FORWARD ONLY, and a
     tag that crossed a CONDITIONAL call carries that with it.
     A variable is keyed by the ASSIGNMENT that produced its value (see localDefs), so `proto` reused for three
     prototypes in one function is three objects and not one object with three interfaces. */
  const defsOf = new Map();                       /* path::fn -> Map(var -> [{at, rhs}]) */
  const defsFor = (path, f) => {
    const k = `${path}::${f.name}::${f.start}`;
    if (!defsOf.has(k)) defsOf.set(k, localDefs(f.body));
    return defsOf.get(k);
  };
  /* version -1 is the value the variable arrived with (a parameter's caller-supplied object) */
  const versionAt = (defs, v, at) => {
    const d = defs.get(v);
    if (!d) return -1;
    let i = -1;
    while (i + 1 < d.length && d[i + 1].at <= at) i++;
    return i;
  };
  /* ---- THE REFUSAL LEDGER ------------------------------------------------------------------------------- */

  /* A PRIMITIVE THAT CANNOT READ ITS INPUT MUST BE ABLE TO SAY SO, AND THE ANALYSIS ABOVE IT MUST NOT BE
     ABLE TO CONFUSE "there is nothing here" WITH "there is something I could not read". Every defect this
     reader has had is one instance of that confusion — a string scan crediting a member because the word
     appeared in the file, `lastIndexOf("struct")` matching inside a longer identifier, `PLATFORM[i].install`
     not matching an identifier-call pattern, a `for` header eaten by a top-level semicolon split. Each
     produced a PLAUSIBLE answer where a refusal was owed, and each was found by someone chasing a wrong
     number rather than by the gate. `matchAt` is the one primitive that already has the posture: it returns
     -1 and its header says why. This is that posture made general.
     A refusal is recorded WHERE THE ANSWER IS DEPENDED ON, never where it is parsed and never where it is
     merely keyed. The corpus has 983 assignments this parser cannot attribute; `tagKey` touches 195 of them
     because it keys EVERY named identifier an edge could run through, including `double rgb[3]` and `char
     out[N]` — asking about a name is not depending on the answer, and counting those would rebuild the noise
     this file deletes. The dependencies are the two questions whose answer is an INTERFACE: which interface a
     member's install target is (`interfacesOf`), and which interface a §3.7.1 interface object was built
     over (`ifaceObjects`). A refusal at either is a member or an identity decided on unread ground. */
  const refusals = [];
  const refusalSeen = new Set();
  const refuse = (primitive, path, off, fn, what, why) => {
    const k = `${primitive}|${path}|${fn}|${what}|${off}`;
    if (refusalSeen.has(k)) return;
    refusalSeen.add(k);
    refusals.push({ primitive, file: path, line: lineOf(sources.get(path).orig, Math.max(off, 0)), fn, what, why });
  };
  const refuseUnread = (path, f, v, at) => {
    const ats = defsFor(path, f).unread.get(v);
    if (!ats || ats[0] > at) return;
    refuse("localDefs", path, f.start + Math.max(at, 0), f.name, v,
           `which object \`${v}\` holds here is UNREAD — an earlier assignment writes it through an index or ` +
           `a member path, and whether that changes what \`${v}\` names depends on its declaration, which ` +
           `this parser does not read`);
  };
  /* A top-level `{` that is neither a function header nor a data aggregate — a whole body this reader may have
     dropped, counted at the parse because a member never seen leaves nothing to ask about. */
  for (const [path] of sources)
    for (const u of fnsOf.get(path).unclassified || [])
      refuse("functions", path, u.at, "(file scope)", u.header,
             `a top-level \`{\` whose header is neither a parameter list nor a data aggregate — if this opens ` +
             `a FUNCTION, its whole body and every install in it is invisible to this audit: \`…${u.header}\``);
  /* The file-level integrity question, asked before any answer about the file is believed. */
  for (const [path, { masked }] of sources) {
    const b = bracketFault(masked);
    if (b)
      refuse("brackets", path, b.at, "(file)", b.why,
             `this file's brackets do not nest — ${b.why}. Every span this reader takes from it (a call's ` +
             `arguments, a function's body, a statement) is measured from a delimiter that does not close ` +
             `where it appears to, so no answer about this file can be trusted, including a COMPLETE one`);
  }
  const tagKey = (path, f, v, at) => key(path, f.name, `${v}#${versionAt(defsFor(path, f), v, at)}`);
  const paramKey = (fnName, p) => key("", fnName, p);
  const classKey = (path, c) => key(path, "@class", c);
  const GLOBAL_OBJECT = key("", "@global", "object");

  const tedges = new Map();
  /* `guard` is null for an unconditional edge, `{only}` for a conditional one — `only` naming the interfaces
     the condition pins, or null when this could not read the condition's subject. */
  const tarrow = (a, b, guard) => {
    if (a === b) return;
    if (!tedges.has(a)) tedges.set(a, []);
    tedges.get(a).push({ to: b, guard: guard || null });
  };
  const tlink = (a, b) => { tarrow(a, b, null); tarrow(b, a, null); };
  const tagSeeds = [];            /* {node, ifaces} */
  /* WHICH INTERFACES A FILE DECLARES — not which members it installs. They are two different facts and the
     caller's row cross-check was reading the second where it meant the first: xml_http_request.c BUILDS and
     §3.7.3-TAGS XMLHttpRequestUpload.prototype and XMLHttpRequestEventTarget.prototype, and installs not one
     member on either — Upload declares no members of its own, and the seven handler attributes are
     event_target.c's shared installer's. So the file that IS that interface's component read as a stranger to
     it, and no change to the C could ever have made it stop: an interface with no members of its own has no
     member install to find. A component declares an interface by building its §3.7.3-tagged prototype or its
     §3.7.1 interface object, which is exactly the pair of statements already read below. */
  const declaresIface = new Map();
  const declares = (path, names) => {
    if (!declaresIface.has(path)) declaresIface.set(path, new Set());
    for (const n of names) declaresIface.get(path).add(n);
  };
  const tagIssues = [];           /* a tag whose interface name is not statically decidable */
  const nonIfaceNodes = new Map(); /* node -> the NON_INTERFACE_FORMS kind the C declares it to be */
  const ifaceObjects = [];        /* {node, ifaces, file, line} — checked back against the tag once solved */
  const interfaceTables = new Map();   /* table -> the field whose cells are interface identifiers */

  /* THE NAMES ONE SLOT GOES BY. A class id is a VALUE, so the name a prototype was written under is not always
     the name it is read back under: element_internals.c copies §4.13.7's three class ids into a local
     `JSClassID classes[3]` and reads all three prototypes through `JS_GetClassProto(ctx, classes[i])`. The slot
     node is keyed by the identifier, so that copy has to be an edge or the three §3.7.1 interface objects are
     built over an object no tag reaches. Only identifiers this corpus actually USES as a slot are collected,
     so an ordinary assignment between two JSValues cannot manufacture a slot that is not one. */
  const SLOT_ARG = ["JS_SetClassProto", "JS_GetClassProto", "realm_value_set", "realm_value_get"];
  const slotBase = (e) => (stripCast(e || "").match(/^([A-Za-z_]\w*)/) || [])[1];
  const slotNames = new Map();
  for (const [path] of sources) {
    const set = new Set();
    for (const f of fnsOf.get(path))
      for (const fn of SLOT_ARG)
        for (const site of callSites(f.body, fn)) {
          const c = slotBase(site.args[1]);
          if (c) set.add(c);
        }
    slotNames.set(path, set);
  }

  for (const [path, { orig }] of sources) {
    for (const f of fnsOf.get(path)) {
      const R = resolver.for(path, f.name);
      const defs = defsFor(path, f);
      const locals = localAssignments(f.body);
      const at = (v, off) => tagKey(path, f, v, off);
      const named = (e) => /^[A-Za-z_]\w*$/.test(e);
      const lineAt = (off) => lineOf(orig, f.start + off);

      /* THE STATEMENT ITSELF: this object is that interface. */
      for (const spec of IFACE_SEEDS)
        for (const site of callSites(f.body, spec.fn)) {
          const obj = stripDup(site.args[spec.obj] || "");
          const expr = site.args[spec.iface] || "";
          const names = R.strings(expr, locals);
          if (!names) { tagIssues.push({ file: path, line: lineAt(site.at), form: spec.fn, expr: expr.trim() }); continue; }
          /* A tag read out of a COLUMN says the table's rows ARE interfaces — html_element.c tags sixty
             prototypes from HTML_IFACE[i].iface — which is what lets a per-row reflection table be attributed
             to the interface of ITS row rather than to all sixty. */
          const col = stripCast(expr).match(/^([A-Za-z_]\w*)\s*\[[^\]]*\]\s*\.\s*([A-Za-z_]\w*)$/);
          if (col) interfaceTables.set(col[1], col[2]);
          if (!named(obj)) { tagIssues.push({ file: path, line: lineAt(site.at), form: spec.fn, expr: obj }); continue; }
          tagSeeds.push({ node: at(obj, site.at), ifaces: names });
          declares(path, names);
        }
      /* The objects the corpus declares are NOT interface prototype objects — see NON_INTERFACE_FORMS. The
         node is exact and local: an object handed somewhere else reaches no such declaration and stays
         UNATTRIBUTED, which is the honest answer rather than a widened claim. */
      for (const [fnName, spec] of NON_INTERFACE_FORMS)
        for (const site of callSites(f.body, fnName)) {
          const obj = stripDup(site.args[spec.obj] || "");
          if (named(obj)) nonIfaceNodes.set(at(obj, site.at), spec);
          else tagIssues.push({ file: path, line: lineAt(site.at), form: fnName, expr: obj });
        }
      /* §3.7.1's interface OBJECT — linked to its prototype, and its NAME kept as a check on the tag. */
      for (const site of callSites(f.body, IFACE_OBJECT.fn)) {
        const obj = stripDup(site.args[IFACE_OBJECT.obj] || "");
        const names = R.strings(site.args[IFACE_OBJECT.iface] || "", locals);
        if (named(obj) && names) {
          refuseUnread(path, f, obj, site.at);
          ifaceObjects.push({ node: at(obj, site.at), ifaces: names, file: path, line: lineAt(site.at) });
          declares(path, names);
        }
      }
      for (const site of callSites(f.body, CTOR_LINK.fn)) {
        const c = stripDup(site.args[CTOR_LINK.ctor] || ""), p = stripDup(site.args[CTOR_LINK.proto] || "");
        if (named(c) && named(p)) tlink(at(c, site.at), at(p, site.at));
      }
      /* quickjs's own per-realm class-prototype slot: what a component puts in it is what every later
         JS_GetClassProto of that class hands back, which is how element.c's `element_proto()` answers with the
         object element.c tagged. An INDEXED slot (`g_iface_class[i]`) is read as the whole array, so the slot
         carries every interface the loop put in it — which is exactly what the caller's own name then picks
         one of (see the literal-narrowed return edge below). */
      /* AND `realm_value_set` IS THAT SAME CALL. realm.c holds a per-realm value in exactly this store —
         `realm_value_set` is `JS_SetClassProto(ctx, (JSClassID)slot, v)` and `realm_value_get` is
         `JS_GetClassProto`, over a class declared for the SLOT that nothing is ever constructed with. So the
         engine has two spellings of one mechanism, and reading only quickjs's left css_rule.c's fourteen §6.4
         prototypes and css_style_sheet.c's §6.1.1 StyleSheet.prototype — every one of them §3.7.3-tagged where
         it is built — unreachable from the interface objects built over them. */
      for (const fn of ["JS_SetClassProto", "realm_value_set"])
        for (const site of callSites(f.body, fn)) {
          const c = slotBase(site.args[1]);
          const v = stripDup(site.args[2] || "");
          if (c && named(v)) tarrow(at(v, site.at), classKey(path, c), null);
        }
      /* A SLOT ID COPIED TO ANOTHER NAME IS THE SAME SLOT, and the edge runs FORWARD — from the name the
         prototype was written under to the name it is read back under — like every other edge in this graph. */
      for (const m of f.body.matchAll(/\b([A-Za-z_]\w*)\s*(?:\[[^\][;]*\])?\s*=\s*([A-Za-z_]\w*)\s*;/g))
        if (slotNames.get(path).has(m[1]) && slotNames.get(path).has(m[2]))
          tarrow(classKey(path, m[2]), classKey(path, m[1]), null);
      /* §3.7.3's [Global] rule, read off the statement the engine already makes: the global object is an
         INSTANCE of this class, so the interface whose prototype the class's slot holds is the interface whose
         members go directly on it. */
      for (const site of callSites(f.body, "JS_SetGlobalClass")) {
        const c = stripCast(site.args[1] || "");
        if (named(c)) tarrow(classKey(path, c), GLOBAL_OBJECT, null);
      }
      /* WHERE AN EXPRESSION'S OBJECT COMES FROM. Three sources answer with an object that is somewhere else's:
         a name, a class's per-realm prototype slot, and the one global object. */
      const sourceOf = (expr, off) => {
        const e = stripDup(expr);
        if (named(e)) return at(e, off);
        const cp = e.match(/^(?:JS_GetClassProto|realm_value_get)\s*\(\s*[^,]+,\s*([A-Za-z_]\w*)/);
        if (cp) return classKey(path, cp[1]);
        /* one realm, one global object — every JS_GetGlobalObject in the program answers with it, so the
           [Global] fact reaches the `global` every component installs on. */
        if (/^JS_GetGlobalObject\s*\(/.test(e)) return GLOBAL_OBJECT;
        return null;
      };
      for (const [lhs, ds] of defs)
        for (const d of ds) {
          const r = stripDup(d.rhs);
          /* the RHS is read at the value the variable had BEFORE this definition */
          const src = sourceOf(d.rhs, d.at - 1);
          if (src) { if (named(r)) tlink(at(lhs, d.at), src); else tarrow(src, at(lhs, d.at), null); }
          const io = r.match(/^idl_interface_object\s*\(\s*[^,]+,\s*([^,]+),\s*([^)]*)\)/);
          if (io && named(stripDup(io[2]))) tlink(at(lhs, d.at), tagKey(path, f, stripDup(io[2]), d.at - 1));
          /* §3.11.1's legacy callback interface object — the seed for an interface that has no prototype
             object to tag. Unreadable, it is reported like any other undecidable interface statement rather
             than dropped: the members installed on the object would otherwise be attributed to nothing. */
          if (r.match(/^([A-Za-z_]\w*)\s*\(/)?.[1] === CALLBACK_IFACE_OBJECT.fn) {
            const open = r.indexOf("("), end = matchAt(r, open);
            const cargs = end < 0 ? [] : splitTop(r.slice(open + 1, end - 1));
            const cnames = R.strings(cargs[CALLBACK_IFACE_OBJECT.iface] || "", locals);
            if (cnames) { tagSeeds.push({ node: at(lhs, d.at), ifaces: cnames }); declares(path, cnames); }
            else tagIssues.push({ file: path, line: lineAt(d.at), form: CALLBACK_IFACE_OBJECT.fn,
                                  expr: (cargs[CALLBACK_IFACE_OBJECT.iface] || "").trim() });
          }
          /* AN INSTANCE IS ITS PROTOTYPE'S INTERFACE. `JS_NewObjectProtoClass(ctx, proto, cls)` builds a
             PLATFORM OBJECT over an interface prototype object, so what it implements is whatever that
             prototype was tagged with — and TWO of Web IDL's rules put an interface's members on such an
             object rather than on the prototype: §3.4.10's [LegacyUnforgeable] (HTML §7.2.4's Location marks
             every member of the interface that way) and §3.7.3's [Global]. Without this edge those members are
             UNATTRIBUTED, which the audit reports as a gap that is not there — MessageChannel's `port1` and
             `port2` were in exactly that state, defined as [SameObject] own properties of the channel.
             ONE-WAY, for the reason the return edge is: the interface flows from the prototype to the instance
             built over it, and never back — an instance is one of many, so what is written on one says nothing
             about what its prototype carries. */
          const inst = r.match(/^JS_NewObjectProtoClass\s*\(\s*[^,]+,\s*([^,]+),/);
          if (inst && named(stripDup(inst[1])))
            tarrow(tagKey(path, f, stripDup(inst[1]), d.at - 1), at(lhs, d.at), null);
          const call = r.match(/^([A-Za-z_]\w*)\s*\(/);
          if (call && byName.has(call[1])) {
            /* A CALL THAT NAMES AN INTERFACE GETS THAT INTERFACE'S OBJECT. `html_iface_proto(ctx,
               "HTMLInputElement")` hands back one of the sixty prototypes its table holds, and which one is
               the argument — so the literal picks it out of what the callee's return carries. Without this the
               four prototypes html_element.c hands the forms component are one undecidable blur, and
               html_form.c's `value` on HTMLTextAreaElement.prototype is exactly the member this whole
               attribution exists to stop crediting to HTMLInputElement. The contract is asserted from the
               other side too: html_iface_proto DFAILs on a name its table does not list. */
            const open = r.indexOf("("), end = matchAt(r, open);
            const lits = end < 0 ? [] : splitTop(r.slice(open + 1, end - 1))
              .map((a) => (a.trim().match(STRING_RE) || [])[1]).filter(Boolean);
            tarrow(key("", call[1], "@return"), at(lhs, d.at), lits.length ? { only: lits } : null);
          }
        }
      for (const m of f.body.matchAll(/\breturn\s+([^;]+);/g)) {
        const src = sourceOf(m[1], m.index);
        if (src) tarrow(src, key("", f.name, "@return"), null);
      }
      for (const p of f.params) tarrow(paramKey(f.name, p), tagKey(path, f, p, -1), null);
      const argEdges = (callees, open, site) => {
        const end = matchAt(f.body, open);
        if (end < 0) return;
        const args = splitTop(f.body.slice(open + 1, end - 1));
        /* A CONDITIONAL CALL CARRIES ITS CONDITION. The object a shared installer is handed under
           `if (!strcmp(HTML_IFACE[i].iface, "HTMLAnchorElement") || …)` is not every prototype the loop walks,
           it is those two — so hyperlink.c's twelve members reach HTMLAnchorElement and HTMLAreaElement and
           nothing else, which neither the old file-granular audit nor a plain "conditional, give up" could say. */
        const guard = guardAt(f.body, plainOf(f), site);
        for (const callee of callees) {
          const params = byName.get(callee);
          if (!params) continue;
          args.forEach((a, k) => {
            const v = stripDup(a);
            if (k < params.length && named(v))
              tarrow(at(v, site), paramKey(callee, params[k]), guard ? { only: guard.only } : null);
          });
        }
      };
      const CALL_RE = /\b([A-Za-z_]\w*)\s*\(/g;
      let cm;
      while ((cm = CALL_RE.exec(f.body))) {
        const callee = cm[1];
        if (!byName.has(callee) || forms.has(callee) || callee === f.name) continue;
        if (IFACE_SEEDS.some((s) => s.fn === callee) || callee === IFACE_OBJECT.fn) continue;
        argEdges([callee], f.body.indexOf("(", cm.index), cm.index);
      }
      /* A CALL THROUGH A TABLE OF FUNCTION POINTERS IS A CALL TO EVERY FUNCTION THAT COLUMN HOLDS, and until
         this edge existed it was a call to NOTHING — the callee is not an identifier, so the direct edge above
         never matched and the arguments reached no parameter. That one gap is where the [Global] fact died:
         `PLATFORM[i].install(ctx, global, doc)` is how every per-realm component in this engine is installed,
         so `global` — which carries Window through §3.7.3's [Global] rule, `JS_SetGlobalClass` and the class's
         prototype slot — reached the `g` of `i_window`, `i_timer`, `i_bar_prop` and 119 others as an untagged
         object. Every member Window installs ON the global was therefore UNATTRIBUTED: the ninety event handler
         IDL attributes, `opener`, `closed`, `top`, `parent`, `setTimeout`, `structuredClone`, `matchMedia`,
         `getComputedStyle`, `requestAnimationFrame` — the largest interface in the engine, attributed to
         nothing, and its ABSENT list inflated by every one of them.
         The column is read the same way every other table in this file is, and it is the FORWARD direction
         only: the callee's parameter is the union of what its callers pass, which is what a function-pointer
         column means. A column this cannot resolve makes no edge, exactly as before. */
      const IND_CALL_RE = /\b([A-Za-z_]\w*)\s*\[[^\]]*\]\s*\.\s*([A-Za-z_]\w*)\s*\(/g;
      let im;
      while ((im = IND_CALL_RE.exec(f.body))) {
        const cells = R.column(im[1], im[2]);
        if (!cells) continue;
        const names = [...new Set(cells.map((c) => stripCast(c).trim())
          .filter((c) => /^[A-Za-z_]\w*$/.test(c) && byName.has(c) && c !== f.name))];
        if (names.length) argEdges(names, im.index + im[0].length - 1, im.index);
      }
    }
  }
  /* The closure. A tag that crossed a CONDITIONAL edge arrives UNCERTAIN — the object may be that interface,
     so the candidate is kept and named — EXCEPT where the condition NAMES it, which pins it as firmly as an
     unconditional edge would. */
  const tags = new Map();
  const put = (node, iface, certain) => {
    let m = tags.get(node);
    if (!m) tags.set(node, m = new Map());
    if (m.get(iface) === true || (m.has(iface) && !certain)) return false;
    m.set(iface, !!certain);
    return true;
  };
  const tqueue = [];
  for (const s of tagSeeds) for (const n of s.ifaces) if (put(s.node, n, true)) tqueue.push(s.node);
  while (tqueue.length) {
    const v = tqueue.pop();
    const outs = tedges.get(v) || [];
    if (!outs.length) continue;
    for (const [iface, certain] of [...tags.get(v)])
      for (const e of outs) {
        const through = !e.guard ? certain : !!(e.guard.only && e.guard.only.includes(iface));
        if (put(e.to, iface, through)) tqueue.push(e.to);
      }
  }

  /* THE CHECK BACK. §3.7.1's interface object was built for a NAMED interface; §3.7.3's tag says what the
     prototype behind it IS. They are two statements about one interface, made in two places, so either one can
     catch the other being wrong: a DISAGREEMENT means one of them names the wrong interface, which nothing else
     in the build would notice. A prototype this reaches no tag from is NOT an accusation that the tag is
     missing — it is this detector saying it could not follow the object, the same admission the UNRESOLVED list
     makes, and it is reported as such rather than as a gap in the component. */
  const tagChecks = [];
  for (const o of ifaceObjects) {
    const m = tags.get(o.node);
    const have = m ? [...m.keys()] : [];
    if (!have.length) tagChecks.push({ kind: "unreached", ifaces: o.ifaces, have, file: o.file, line: o.line });
    else if (!o.ifaces.some((n) => have.includes(n)))
      tagChecks.push({ kind: "contradicted", ifaces: o.ifaces, have, file: o.file, line: o.line });
  }

  return { macros, typedefs, sources, resolver, forms, fnsOf,
           tags, tagKey: (path, f, v, at) => tagKey(path, f, v, at), tagIssues, tagChecks, interfaceTables,
           nonIfaceNodes,
           recordContradictions, declaresIface, ints, refusals, refuseUnread };
}

/* EVERY INSTALLED MEMBER, ATTRIBUTED TO THE INTERFACE ITS TARGET IS — one record per (member, site), carrying
   the interfaces it lands on or, when the target's interface cannot be decided, nothing and the reason. A
   record with no interface is NOT credited to the file it was written in: that fallback is exactly the false
   COMPLETE this attribution exists to remove, and the caller reports it as its own category instead.
   `paths` are absolute; pass the whole program, since which interface a member belongs to is a fact about the
   object it is installed on and not about which row named the file. */
export function installedMembers(paths, env) {
  const records = [], unresolved = [], offInstaller = [], excluded = [], unselected = [];
  const { forms } = env;

  /* WHICH INTERFACE THIS TARGET IS — hoisted out of the per-file loop because a SELECTED installer's target is
     resolved in the CALLER's file and function, not in the one the install is written in. */
  const interfacesOf = (path, f, targetExpr, off) => {
    const v = stripDup(targetExpr || "");
    if (!/^[A-Za-z_]\w*$/.test(v))
      return { ifaces: [], candidates: [], why: `the install target \`${(targetExpr || "").trim()}\` is not a named object` };
    env.refuseUnread(path, f, v, off - f.start);
    /* AN OBJECT THE C DECLARES IS NOT AN INTERFACE PROTOTYPE OBJECT — asked FIRST, because "which interface
       is this" is not the question there and an unanswered question is what this whole file refuses to fake. */
    const kind = env.nonIfaceNodes.get(env.tagKey(path, f, v, off - f.start));
    if (kind) return { ifaces: [], candidates: [], nonInterface: kind, why: null };
    const m = env.tags.get(env.tagKey(path, f, v, off - f.start));
    if (!m || !m.size)
      return { ifaces: [], candidates: [], why: `no interface tag reaches \`${v}\`` };
    const certain = [...m].filter(([, c]) => c).map(([n]) => n);
    const all = [...m.keys()];
    /* ONE interface is one interface however the site was reached — a conditional call changes WHETHER the
       member is installed, which is not what this asks; it asks WHICH interface it lands on. */
    const one = certain.length ? certain : all;
    if (one.length === 1) return { ifaces: one, candidates: [], why: null };
    if (!certain.length)
      return { ifaces: [], candidates: all, why: `\`${v}\` is reached conditionally from ${all.length} tagged prototype(s)` };
    /* SEVERAL tagged prototypes and a conditional site: the member lands on some of them, not on all. The
       guard is read (see guardAt) — where it names them, those are the answer; where it does not, this is
       named UNATTRIBUTED rather than guessed, because crediting all of them is the false COMPLETE. */
    const guard = guardAt(f.body, plainOf(f), off - f.start);
    if (guard) {
      const named = guard.only ? certain.filter((n) => guard.only.includes(n)) : [];
      if (named.length) return { ifaces: named, candidates: [], why: null };
      return { ifaces: [], candidates: certain,
               why: `\`${v}\` is ${certain.length} tagged prototypes and this install is under a condition ` +
                    `that does not name which` };
    }
    return { ifaces: certain, candidates: [], why: null };
  };

  /* EVERY CALL OF A FUNCTION, corpus-wide, with the function and file it stands in — what a SELECTED
     installer's subset is resolved against. */
  const callersOf = (name) => {
    const out = [];
    for (const p of paths) {
      if (!env.sources.get(p)) continue;
      for (const cf of env.fnsOf.get(p) || []) {
        if (cf.name === name) continue;
        for (const site of callSites(cf.body, name)) out.push({ path: p, fn: cf, site });
      }
    }
    return out;
  };

  for (const path of paths) {
    const src = env.sources.get(path);
    if (!src) { unresolved.push({ file: path, line: 0, form: "(file)", expr: "not found" }); continue; }
    const { orig, masked } = src;
    const fns = env.fnsOf.get(path);
    const fnAt = (off) => fns.find((f) => off >= f.start && off < f.end);
    const localsCache = new Map();
    const localsFor = (f) => {
      if (!f) return null;
      if (!localsCache.has(f)) localsCache.set(f, localAssignments(f.body));
      return localsCache.get(f);
    };
    /* A NAME THIS FUNCTION WAS GIVEN, however many hops it took to reach the install. The forwarding line of a
       shared installer resolves to nothing by construction — the name is the CALLER's, and every caller is
       audited — but only `nav_env`-shaped helpers spell the parameter AT the install. `idl_install_accessor_step`
       writes `a = JS_NewAtom(ctx, name); JS_DefinePropertyGetSet(ctx, target, a, …)`, and a rule that read only
       the identifier at the site reported three of this engine's OWN install helpers as unresolvable constructs
       — the forwarding lines of the very forms the audit is built out of. Literals are blanked first, so a
       member whose name happens to spell a parameter is not mistaken for one. */
    const fromParam = (f, expr, depth = 0) => {
      if (!f || depth > 4) return false;
      const locals = localsFor(f) || new Map();
      for (const m of blankLiterals(String(expr)).matchAll(/[A-Za-z_]\w*/g)) {
        if (f.params.includes(m[0])) return true;
        if (locals.has(m[0]) && locals.get(m[0]).some((rhs) => fromParam(f, rhs, depth + 1))) return true;
      }
      return false;
    };
    /* Every name is resolved in the SCOPE the construct stands in — the enclosing function's tables, then the
       file's, then a header's. */
    const scoped = (f) => env.resolver.for(path, f ? f.name : null);

    const report = (off, form, expr) =>
      unresolved.push({ file: path, line: lineOf(orig, off), form, expr: expr.trim().replace(/\s+/g, " ") });

    const emitWith = (names, stub, a, off, form, where, kind) => {
      const at = where || { file: path, line: lineOf(orig, off) };
      for (const name of names)
        records.push({ name, stubbed: !!stub, file: at.file, line: at.line, form,
                       ifaces: a.ifaces, candidates: a.candidates, why: a.why,
                       nonInterface: a.nonInterface || null, kind: kind || null });
    };
    const emit = (names, stub, f, targetExpr, off, form, kind) =>
      emitWith(names, stub, interfacesOf(path, f, targetExpr, off), off, form, null, kind);

    /* The member names one DECLARATION of a two-halved registry carries — a table of rows whose `field` column
       is the member's IDL name, reached either directly or (the byte readers) through the interface record that
       names the table. */
    const membersOfDeclare = (R, form, expr) => {
      let refs = R.tableRefs(expr);
      if (refs && form.via) {
        const inner = [];
        for (const ref of refs) {
          const cells = R.column(ref, form.via);
          const got = cells && cells.map((c) => R.tableRefs(c));
          if (!got || got.some((g) => !g)) return null;
          for (const g of got) inner.push(...g);
        }
        refs = inner;
      }
      if (!refs) return null;
      const names = [];
      for (const ref of refs) {
        const cells = R.column(ref, form.field);
        if (!cells) return null;
        for (const c of cells) {
          const v = R.strings(c, null);
          if (!v) return null;
          names.push(...v);
        }
      }
      return names;
    };

    /* 1. the call forms */
    for (const [callee, form] of forms) {
      for (const site of callSites(masked, callee)) {
        const f = fnAt(site.at);
        /* A site outside every function body is a DECLARATION of the form, not a use of it. */
        if (!f) continue;
        /* A site inside the shared install helper that this form was DERIVED from is the forwarding itself —
           its name is the caller's, and every caller is audited. Reporting it would name the one line that
           cannot resolve by construction, and hide the call sites that can. */
        const pos = form.name === undefined ? form.tableArg : form.name;
        if (forms.has(f.name) && fromParam(f, site.args[pos] || "")) continue;
        if (TABLE_FORMS.some((t) => t.install === f.name)) continue;
        const target = stripCast(site.args[form.target] || "");
        const a = interfacesOf(path, f, target, site.at);
        if (form.ambiguous && !a.ifaces.length && !a.candidates.length) {
          /* AN OBJECT NO INTERFACE DECLARATION REACHES — see the header. §3.7.3's tag, §3.7.1's interface
             object, the per-realm class-prototype slot and [Global] are how the corpus says a page can reach an
             object, and this write lands on one none of them names: a listener registration, a response
             summary, an event's internal slot bag, a per-realm record built with no prototype at all. It
             installs nothing, so it is not counted and it is not a gap.
             It is recorded by NAME, because this is also where the classification can be WRONG in the other
             direction — a real IDL member written as a plain own property of an instance, which `document.title`
             is — and the caller prints it whenever the name is a member the interface being audited is
             otherwise missing. Neither answer is silent. */
          const names = scoped(f).strings(site.args[pos] || "", localsFor(f));
          for (const n of names || [])
            offInstaller.push({ name: n, file: path, line: lineOf(orig, site.at), target, form: callee,
                                why: a.why });
          continue;
        }
        if (site.args[pos] === undefined) { report(site.at, callee, "(no argument)"); continue; }
        const noop = form.fn !== undefined && /\bjs_noop\b/.test(site.args[form.fn] || "");
        const R = scoped(f);
        let names;
        if (form.name !== undefined) {
          names = R.strings(site.args[pos], localsFor(f));
        } else {
          const refs = R.tableRefs(site.args[pos]);
          names = refs ? [] : null;
          for (const ref of refs || []) {
            const cells = R.column(ref, form.field);
            if (!cells) { names = null; break; }
            for (const c of cells) {
              const v = R.strings(c, null);
              if (!v) { names = null; break; }
              names.push(...v);
            }
            if (!names) break;
          }
        }
        if (!names) { report(site.at, callee, site.args[pos]); continue; }
        /* A SHARED INSTALLER THAT SELECTS A SUBSET OF ITS NAMES BY A CALLER'S ARGUMENT installs a DIFFERENT set
           per call, and reading it as one set is the audit's only remaining false COMPLETE — the error it
           cannot catch from the other side, because the member it hides never prints as anything.
           `event_target_install_handlers(ctx, target, EH_XHR)` is the case: it walks HTML §8.1.7.2's ninety
           event handler IDL attributes and installs the rows whose MIXIN bit the caller asked for, and this
           credited all ninety to every prototype any caller ever handed it. XMLHttpRequestEventTarget declares
           seven of those names, Window declares eighty-odd, MessagePort two — so the mask is the whole
           difference between installed and credited wherever two callers' interfaces share a name.
           It is resolved PER CALL SITE, where both facts are: the caller supplies the selector AND the target,
           so the subset and the interface it lands on come from the same line. The name column and the
           selector column are two columns of ONE list (`EH_NAME[i]` and `EH_MASK[i]` are both `EVENT_HANDLERS`
           expanded), which is why they can be read positionally — and that is CHECKED rather than assumed: a
           length disagreement refuses the whole installer.
           A SELECTOR THIS CANNOT EVALUATE IS A REFUSAL, never a fallback to the union. It is named with its
           file and line in its own failing category, because a subset the audit cannot compute is a member
           list it does not know — and the one thing it must never do here is answer anyway. */
        const tgtIdx = f.params.indexOf(target);
        if (names.length > 1 && tgtIdx >= 0 && /\bcontinue\s*;/.test(f.body)) {
          const sel = selectorOf(f, R);
          const line = lineOf(orig, site.at);
          if (sel.why) { unselected.push({ file: path, line, fn: f.name, why: sel.why }); continue; }
          if (sel.cells.length !== names.length) {
            unselected.push({ file: path, line, fn: f.name,
                              why: `the selector column \`${sel.table}\` has ${sel.cells.length} rows and the ` +
                                   `name column has ${names.length} — they are not two columns of one list` });
            continue;
          }
          for (const c of callersOf(f.name)) {
            const off = c.fn.start + c.site.at;
            const cline = lineOf(env.sources.get(c.path).orig, off);
            const arg = c.site.args[sel.at];
            const mv = arg === undefined ? null : evalInt(arg, env.ints, env.macros);
            if (mv === null) {
              unselected.push({ file: c.path, line: cline, fn: f.name,
                                why: `the selector argument \`${String(arg).trim()}\` is not a constant this ` +
                                     `can evaluate, so which of ${names.length} names this call installs is unknown` });
              continue;
            }
            const chosen = [], bad = [];
            names.forEach((n, k) => {
              const bits = evalInt(sel.cells[k], env.ints, env.macros);
              if (bits === null) bad.push(n);
              else if (bits & mv) chosen.push(n);
            });
            if (bad.length) {
              unselected.push({ file: path, line, fn: f.name,
                                why: `${bad.length} row(s) of \`${sel.table}\` hold a selector this cannot ` +
                                     `evaluate — ${bad.slice(0, 4).join(", ")}` });
              continue;
            }
            emitWith(chosen, noop, interfacesOf(c.path, c.fn, stripCast(c.site.args[tgtIdx] || ""), off),
                     off, callee, { file: c.path, line: cline }, form.kind);
          }
          continue;
        }
        emitWith(names, noop, a, site.at, callee, null, form.kind);
      }
    }

    /* 2. the JSCFunctionListEntry tables, read only where one is installed */
    for (const site of callSites(masked, "JS_SetPropertyFunctionList")) {
      const f = fnAt(site.at);
      if (!f) continue;
      const R = scoped(f);
      const tableExpr = stripCast(site.args[2] || "");
      const t = /^[A-Za-z_]\w*$/.test(tableExpr) ? R.rows(tableExpr) : null;
      if (!t) { report(site.at, "JS_SetPropertyFunctionList", tableExpr); continue; }
      for (const row of t.rows) {
        ENTRY_RE.lastIndex = 0;
        const m = ENTRY_RE.exec(row);
        if (!m) { report(site.at, `JS_SetPropertyFunctionList/${tableExpr}`, row); continue; }
        const open = row.indexOf("(", m.index);
        const end = matchAt(row, open);
        const args = splitTop(row.slice(open + 1, end - 1));
        const names = R.strings(args[0] || "", null);
        if (!names) { report(site.at, `${m[1]} in ${tableExpr}`, args[0] || ""); continue; }
        const noop = /\bjs_noop\b/.test(row);
        emit(names, noop, f, stripCast(site.args[1] || ""), site.at, `JS_SetPropertyFunctionList/${tableExpr}`,
             ENTRY_KIND(m[1]));
      }
    }

    /* 3. THE TWO-HALVED REGISTRIES: reflections and byte readers. The DECLARE half carries the names and no
       object; the INSTALL half carries the object and a handle. So the interface comes from joining the halves,
       two ways, in this order:
         (a) THE TABLE'S OWN ROW. `element_declare_reflections(ctx, HTML_IFACE[i].refl, …)` declares one table
             per row of a table whose `iface` column is what html_element.c tags each prototype with — so row
             r's reflections are row r's interface, and the sixty element interfaces get exactly their own
             members instead of all sixty getting all of them.
         (b) THE HANDLE. `g_html_refl_base = element_declare_reflections(ctx, R_HTML, …)` and
             `element_install_reflections(ctx, html_p, g_html_refl_base, …)` are the same registration named by
             the same identifier, so the declaration's members belong to the object the install names. Where
             the handle is not an identifier (body.c keeps it in a per-interface record) a file with exactly
             ONE declaration of that registry has only one thing its installs can be installing.
       Anything left is UNATTRIBUTED, named, and not credited to the file. */
    for (const form of TABLE_FORMS) {
      const declares = callSites(masked, form.declare).filter((s) => fnAt(s.at));
      const installs = callSites(masked, form.install).filter((s) => fnAt(s.at));
      for (const site of declares) {
        const f = fnAt(site.at);
        const R = scoped(f);
        const arg = site.args[form.arg] || "";
        const col = stripCast(arg).match(/^([A-Za-z_]\w*)\s*\[[^\]]*\]\s*\.\s*([A-Za-z_]\w*)$/);
        const ifaceField = col ? env.interfaceTables.get(col[1]) : null;
        if (ifaceField) {
          const tables = R.column(col[1], col[2]), ifaces = R.column(col[1], ifaceField);
          if (!tables || !ifaces || tables.length !== ifaces.length) { report(site.at, form.declare, arg); continue; }
          for (let r = 0; r < tables.length; r++) {
            const cell = tables[r].trim();
            if (/^(NULL|0)$/.test(cell)) continue;      /* a row that declares no members */
            const names = membersOfDeclare(R, form, cell);
            const iname = R.strings(ifaces[r], null);
            if (!names || !iname) { report(site.at, `${form.declare}/${col[1]}[${r}]`, cell); continue; }
            emitWith(names, false, { ifaces: iname, candidates: [], why: null }, site.at, form.declare, null,
                     form.kind);
          }
          continue;
        }
        const names = membersOfDeclare(R, form, arg);
        if (!names) { report(site.at, form.declare, arg); continue; }
        /* the handle this declaration was stored in, if it was stored in a name at all */
        const stmt = masked.slice(Math.max(0, site.at - 200), site.at);
        const lhs = (stmt.match(/([A-Za-z_]\w*)\s*=\s*$/) || [])[1];
        let chosen = lhs ? installs.filter((s) => stripCast(s.args[form.handle] || "") === lhs) : [];
        if (!chosen.length && declares.length === 1) chosen = installs;
        const parts = chosen.map((s) => interfacesOf(path, fnAt(s.at), stripCast(s.args[form.target] || ""), s.at));
        const a = parts.length
          ? { ifaces: [...new Set(parts.flatMap((p) => p.ifaces))],
              candidates: [...new Set(parts.flatMap((p) => p.candidates))],
              why: parts.some((p) => !p.why) ? null : parts[0].why }
          : { ifaces: [], candidates: [],
              why: `nothing in this file installs the ${form.declare} registration` +
                   (lhs ? ` held in \`${lhs}\`` : " (it is not held in a named handle)") };
        emitWith(names, false, a, site.at, form.declare, null, form.kind);
      }
    }

    /* 4. the conditional members this user agent must NOT have */
    for (const site of callSites(masked, EXCLUDED_FORM.fn)) {
      const f = fnAt(site.at);
      if (!f) continue;
      const R = scoped(f);
      const iface = R.strings(site.args[EXCLUDED_FORM.iface] || "", null);
      const names = R.tableRefs(site.args[EXCLUDED_FORM.table] || "");
      const why = R.strings(site.args[EXCLUDED_FORM.why] || "", null);
      if (!iface || iface.length !== 1 || !names || !why) {
        report(site.at, EXCLUDED_FORM.fn, site.args[EXCLUDED_FORM.table] || "");
        continue;
      }
      for (const ref of names) {
        const cells = R.column(ref, null);
        if (!cells) { report(site.at, `${EXCLUDED_FORM.fn}/${ref}`, "(not a name table)"); continue; }
        for (const c of cells) {
          const one = R.strings(c, null);
          if (!one) { report(site.at, `${EXCLUDED_FORM.fn}/${ref}`, c); continue; }
          for (const n of one)
            excluded.push({ iface: iface[0], name: n, why: why[0], file: path, line: lineOf(orig, site.at) });
        }
      }
    }
  }
  return { records, unresolved, offInstaller, excluded, unselected };
}
