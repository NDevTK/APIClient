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
 * WHAT COUNTS AS AN INSTALL TARGET. `JS_SetPropertyStr` and `JS_DefinePropertyValueStr` are the two forms that
 * are also how any ordinary object is built, so the NAME resolving is not enough — the object has to be one a
 * member can be installed on. fetch.c writes "status", "statusText" and "body" onto a plain record and Window
 * really does have a `status` member; event_target.c writes "removed", "once" and "passive" onto a listener
 * record. Counting those is exactly the false COMPLETE above, and refusing to count `fetch`, `navigator`,
 * `Node` and `Text` — which are installed on the global with the same call — is the false ABSENT.
 *
 * So which objects are install targets is SOLVED rather than guessed, over the whole program at once. An object
 * is a target if an UNAMBIGUOUS form names it (`idl_install_*`, `JS_DefinePropertyGetSet`, `idl_interface_tag`,
 * `JS_SetPropertyFunctionList`), and the fact flows along the two edges that carry the object itself: an
 * assignment (`g = (JSValue)global`) and an argument passed to a function this corpus defines. Both edges are
 * bidirectional, because the object on each side is the same object. `window.c` installs `opener` on the global
 * with the IDL installer, which is what makes the global a target; the fact reaches `fetch_install`,
 * `document_install` and `node_install_interface_ctor` through the calls that hand the global to them, and it
 * reaches a listener record through nothing, because nothing ever installs a member on one.
 *
 * AND WHICH INTERFACE THAT TARGET IS, which is the same question one level down. Reading the install construct
 * fixed WHAT is installed and left WHERE untouched: the audit was FILE-granular, so a row credited every member
 * any of its files installed, and html_form.c's `value` — installed on HTMLTextAreaElement.prototype — counted
 * for HTMLInputElement. That is a false COMPLETE, which HIDES a gap rather than burying it in noise. So a
 * member is attributed to the interface its TARGET is, read out of Web IDL §3.7.3's @@toStringTag that the
 * component already installs on every interface prototype object (`idl_interface_tag`) and §3.7.3's [Global]
 * statement for the object a [Global] interface puts its members on directly (`idl_global_object`). A target
 * this cannot decide is UNATTRIBUTED — named with its file, line and member, never credited to its file, which
 * is the fallback the whole mechanism exists to remove.
 *
 * WHAT IS STILL COARSER THAN THE INTERFACE, so that nobody reads a silence as an answer: a shared installer
 * selecting a SUBSET of its names from a parameter. `event_target_install_handlers(ctx, target, EH_GLOBAL)`
 * installs the ninety handler attributes whose mask bit the caller asked for, and this reads the whole table
 * and attributes all ninety to every prototype any caller hands it — the same over-approximation the
 * file-granular audit made, no worse and no better, because the guard is a `continue` at the top of the loop
 * rather than a condition over the site, which is what guardAt() reads. It is honest on the corpus as it stands (a name
 * over-credited to a prototype is only a false COMPLETE when the IDL puts that name on that interface, and the
 * masks and the mixins agree today) and it is the next thing to make exact.
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

/* The index one past the closer that matches the opener at `open`. Strings are skipped, so a brace inside a
   literal never closes a block. Returns -1 when the text ends first, which is a source this cannot parse and
   therefore something to report rather than to assume about. */
function matchAt(text, open) {
  const PAIRS = { "(": ")", "[": "]", "{": "}" };
  const close = PAIRS[text[open]];
  if (!close) return -1;
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (c === '"' || c === "'") {
      i++;
      while (i < text.length && text[i] !== c) { if (text[i] === "\\") i++; i++; }
      continue;
    }
    if (c === "(" || c === "[" || c === "{") depth++;
    else if (c === ")" || c === "]" || c === "}") { depth--; if (depth === 0) return i + 1; }
  }
  return -1;
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
    const inlineStruct = typeText.lastIndexOf("struct");
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

class Resolver {
  constructor(macros, typedefs, tablesByFile, headerTables, file = null, fn = null) {
    this.macros = macros;
    this.typedefs = typedefs;
    this.tablesByFile = tablesByFile;
    this.headerTables = headerTables;
    this.file = file;
    this.fn = fn;
  }

  /* The same resolver reading ONE scope: the function's own tables, then the file's, then a header's — which
     is what C scoping already says, and the only reason two files may each spell a table `NAMES[]`. */
  for(file, fn = null) {
    return new Resolver(this.macros, this.typedefs, this.tablesByFile, this.headerTables, file, fn);
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
    if (bare && locals && locals.has(e)) {
      const out = [];
      for (const assigned of locals.get(e)) {
        const v = this.strings(assigned, locals, depth + 1);
        if (!v) return null;
        out.push(...v);
      }
      return out.length ? out : null;
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
   forms that are also how any object is built, and which therefore require an install target (see the header).
   `fn` is the position of the body, read only to catch a `js_noop` member — the lazy stub this audit exists to
   expose, and a ban that has to be expressed against the install FORM rather than against the file's text. */
const CALL_FORMS = new Map(Object.entries({
  idl_install_accessor:          { target: 1, name: 2, fn: 3 },
  idl_install_accessor_step:     { target: 1, name: 2 },
  idl_install_method:            { target: 1, name: 2 },
  idl_install_step_method:       { target: 1, name: 2 },
  idl_install_replaceable:       { target: 1, name: 2, fn: 3 },
  idl_install_replaceable_value: { target: 1, name: 2 },
  JS_DefinePropertyGetSet:       { target: 1, name: 2, fn: 3 },
  JS_SetPropertyStr:             { target: 1, name: 2, fn: 3, ambiguous: true },
  JS_DefinePropertyValueStr:     { target: 1, name: 2, fn: 3, ambiguous: true },
}));

/* The forms that corroborate an object as an INSTALL TARGET without naming a member themselves. */
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
  { declare: "element_declare_reflections", install: "element_install_reflections", arg: 1, field: "idl",
    target: 1, handle: 2 },
  { declare: "byte_reader_declare", install: "byte_reader_install", arg: 1, via: "readers", field: "name",
    target: 1, handle: 2 },
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
];
const IFACE_OBJECT = { fn: "idl_interface_object", iface: 1, obj: 2 };
const CTOR_LINK = { fn: "JS_SetConstructor", ctor: 1, proto: 2 };

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
   an array initializer's `{` follows a `=` and is skipped. */
function functions(masked) {
  const out = [];
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
        else bodyStart = -1;
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
  for (const stmt of stmts) {
    /* The declarator list, split on commas — but NOT when the statement is an aggregate initialiser, whose
       commas separate ELEMENTS. Braces are not counted here either: a statement that opens a block carries an
       unmatched `{`, and counting it hid the second declarator of `JSAtom a = …, r = …` inside such a block. */
    const decls = /=\s*\{/.test(stmt) ? [stmt] : splitTop(stmt, ",", false);
    let declAt = stmtAt;
    for (const decl of decls) {
      const eq = decl.indexOf("=");
      if (eq >= 0 && !"=!<>+-*/&|^%".includes(decl[eq + 1]) && !"=!<>+-*/&|^%".includes(decl[eq - 1])) {
        const lhs = decl.slice(0, eq).trim().match(/([A-Za-z_]\w*)$/);
        if (lhs) {
          if (!map.has(lhs[1])) map.set(lhs[1], []);
          map.get(lhs[1]).push({ at: declAt + eq, rhs: decl.slice(eq + 1) });
        }
      }
      declAt += decl.length + 1;                  /* the comma splitTop removed */
    }
    stmtAt += stmt.length + 1;                    /* the semicolon splitTop removed */
  }
  for (const defs of map.values()) defs.sort((a, b) => a.at - b.at);
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
  const fnsOf = new Map();          /* path -> functions() */
  const byName = new Map();         /* function name -> its parameter list (for the argument edge) */
  for (const [path, { masked }] of sources) {
    const fs = functions(masked);
    fnsOf.set(path, fs);
    for (const f of fs) if (f.name && !byName.has(f.name)) byName.set(f.name, f.params);
  }
  const tablesByFile = new Map(), headerTables = new Map();
  for (const [p, { masked }] of sources) {
    const t = scopeTables(masked, typedefs, fnsOf.get(p));
    tablesByFile.set(p, t);
    if (extname(p) === ".h") for (const [k, v] of t.file) headerTables.set(k, headerTables.has(k) ? null : v);
  }
  const resolver = new Resolver(macros, typedefs, tablesByFile, headerTables);
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
        for (const [callee, form] of forms) {
          for (const site of callSites(fn.body, callee)) {
            const nameArg = stripCast(site.args[form.name === undefined ? form.tableArg : form.name] || "");
            const direct = fn.params.indexOf(nameArg);
            const col = nameArg.match(/^([A-Za-z_]\w*)\s*\[[^\]]*\]\s*\.\s*([A-Za-z_]\w*)$/);
            const via = col ? fn.params.indexOf(col[1]) : -1;
            if (direct < 0 && via < 0) continue;
            const tgt = stripCast(site.args[form.target] || "");
            const derived = { target: fn.params.indexOf(tgt), ambiguous: !!form.ambiguous };
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

  /* THE INSTALL TARGETS, solved over the whole program. See the header: an object is a target because an
     unambiguous form installs on it, and the fact travels with the object — along an assignment inside one
     function and along an argument into another. Both edges are bidirectional because both sides ARE the same
     object, and the graph is closed by BFS, so no propagation order has to be chosen. */
  const key = (path, fnName, v) => `${path}::${fnName}::${v}`;
  const edges = new Map(), seeds = [];
  const arrow = (a, b) => {
    if (!edges.has(a)) edges.set(a, []);
    edges.get(a).push(b);
  };
  const link = (a, b) => { arrow(a, b); arrow(b, a); };
  for (const [path, { masked }] of sources) {
    for (const f of fnsOf.get(path)) {
      const here = (v) => key(path, f.name, v);
      for (const [callee, form] of forms) {
        if (form.ambiguous) continue;
        for (const site of callSites(f.body, callee)) {
          const t = stripCast(site.args[form.target] || "");
          if (/^[A-Za-z_]\w*$/.test(t)) seeds.push(here(t));
        }
      }
      for (const [callee, pos] of TARGET_FORMS)
        for (const site of callSites(f.body, callee)) {
          const t = stripCast(site.args[pos] || "");
          if (/^[A-Za-z_]\w*$/.test(t)) seeds.push(here(t));
        }
      /* the assignment edge: `g = (JSValue)global`; and the CREATION seed. An object built with a host CLASS
         (JS_NewObjectClass / JS_NewObjectProtoClass) is a PLATFORM OBJECT — a MessageChannel, a Document
         wrapper — and a member written on one is a member of that interface, however §3.7-shaped the write is.
         A plain JS_NewObject or a null-prototyped JS_NewObjectProto is a RECORD: a listener registration, a
         response summary, a mutation record. Nothing else separates event_target.c's "removed" from
         message_port.c's "port1", and both are written with the same call. */
      for (const [lhs, rhss] of localAssignments(f.body))
        for (const rhs of rhss) {
          const r = stripCast(rhs);
          if (/^[A-Za-z_]\w*$/.test(r)) link(here(lhs), here(r));
          if (/^\s*(?:JS_NewObject(?:Proto)?Class|JS_GetGlobalObject)\s*\(/.test(rhs)) seeds.push(here(lhs));
          /* THE RETURN EDGE, and it is ONE-WAY: `doc = node_wrap(…)` is the object node_wrap built, so what the
             callee returns decides what the caller holds — never the other way round. Two-way, every caller of a
             shared constructor became one node: one target local anywhere made `idl_slots_new`'s result a
             target everywhere, and event_target.c's handler map — an internal slot bag with no prototype — was
             then an object this claimed members were installed on. */
          const call = rhs.trim().match(/^([A-Za-z_]\w*)\s*\(/);
          if (call && byName.has(call[1])) arrow(key("", call[1], "@return"), here(lhs));
        }
      for (const [, ret] of f.body.matchAll(/\breturn\s+([A-Za-z_]\w*)\s*;/g))
        arrow(here(ret), key("", f.name, "@return"));
      /* the argument edge: every call to a function this corpus defines. Found by walking the body's own call
         syntax once, rather than by looking for each of several thousand known names in it. */
      const CALL_RE = /\b([A-Za-z_]\w*)\s*\(/g;
      let cm;
      while ((cm = CALL_RE.exec(f.body))) {
        const callee = cm[1];
        const params = byName.get(callee);
        if (!params || forms.has(callee) || callee === f.name) continue;
        const open = f.body.indexOf("(", cm.index);
        const end = matchAt(f.body, open);
        if (end < 0) continue;
        splitTop(f.body.slice(open + 1, end - 1)).forEach((a, k) => {
          const v = stripCast(a);
          if (k < params.length && /^[A-Za-z_]\w*$/.test(v)) link(here(v), key("", callee, params[k]));
        });
      }
      /* a parameter is the same object inside its own function as at the call site */
      for (const p of f.params) link(here(p), key("", f.name, p));
    }
  }
  const targets = new Set();
  const queue = [...seeds];
  while (queue.length) {
    const v = queue.pop();
    if (targets.has(v)) continue;
    targets.add(v);
    for (const n of edges.get(v) || []) if (!targets.has(n)) queue.push(n);
  }
  /* An object this could NOT classify is not silently dropped — see installedMembers. A local built by a plain
     JS_NewObject / JS_NewObjectProto and never named by an install form is a record and provably not a target;
     anything else that a property write lands on is a question, and a question is reported. */
  const records = new Set();
  for (const [path, { masked }] of sources)
    for (const f of fnsOf.get(path))
      for (const [lhs, rhss] of localAssignments(f.body))
        for (const rhs of rhss)
          if (/^\s*JS_(?:NewObject(?:Proto)?|NewArray|NewObjectFromCtor)\s*\(/.test(rhs) &&
              !targets.has(key(path, f.name, lhs)))
            records.add(key(path, f.name, lhs));

  /* ---- WHICH INTERFACE EACH OBJECT IS, solved over the whole program ------------------------------------- */

  /* The same shape as the target solve above and a DIFFERENT graph, because the two facts travel differently.
     Targethood is symmetric — the object on each side of an edge is the same object, so if either is a target
     both are. An interface NAME is not: a shared installer's parameter is EVERY object its callers hand it, so
     the tags of those objects flow INTO it (and the members it installs land on all of them), but the union it
     accumulates must never flow back OUT to one particular caller's object, which would tell window.c's global
     that it is also an HTMLElement. So the argument edge is FORWARD ONLY, and a tag that crossed a CONDITIONAL
     call carries that with it.
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
  const tagIssues = [];           /* a tag whose interface name is not statically decidable */
  const ifaceObjects = [];        /* {node, ifaces, file, line} — checked back against the tag once solved */
  const interfaceTables = new Map();   /* table -> the field whose cells are interface identifiers */

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
        }
      /* §3.7.1's interface OBJECT — linked to its prototype, and its NAME kept as a check on the tag. */
      for (const site of callSites(f.body, IFACE_OBJECT.fn)) {
        const obj = stripDup(site.args[IFACE_OBJECT.obj] || "");
        const names = R.strings(site.args[IFACE_OBJECT.iface] || "", locals);
        if (named(obj) && names) ifaceObjects.push({ node: at(obj, site.at), ifaces: names, file: path, line: lineAt(site.at) });
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
      for (const site of callSites(f.body, "JS_SetClassProto")) {
        const c = (stripCast(site.args[1] || "").match(/^([A-Za-z_]\w*)/) || [])[1];
        const v = stripDup(site.args[2] || "");
        if (c && named(v)) tarrow(at(v, site.at), classKey(path, c), null);
      }
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
        const cp = e.match(/^JS_GetClassProto\s*\(\s*[^,]+,\s*([A-Za-z_]\w*)/);
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
      const CALL_RE = /\b([A-Za-z_]\w*)\s*\(/g;
      let cm;
      while ((cm = CALL_RE.exec(f.body))) {
        const callee = cm[1];
        const params = byName.get(callee);
        if (!params || forms.has(callee) || callee === f.name) continue;
        if (IFACE_SEEDS.some((s) => s.fn === callee) || callee === IFACE_OBJECT.fn) continue;
        const open = f.body.indexOf("(", cm.index);
        const end = matchAt(f.body, open);
        if (end < 0) continue;
        const args = splitTop(f.body.slice(open + 1, end - 1));
        /* A CONDITIONAL CALL CARRIES ITS CONDITION. The object a shared installer is handed under
           `if (!strcmp(HTML_IFACE[i].iface, "HTMLAnchorElement") || …)` is not every prototype the loop walks,
           it is those two — so hyperlink.c's twelve members reach HTMLAnchorElement and HTMLAreaElement and
           nothing else, which neither the old file-granular audit nor a plain "conditional, give up" could say. */
        const guard = guardAt(f.body, plainOf(f), cm.index);
        args.forEach((a, k) => {
          const v = stripDup(a);
          if (k < params.length && named(v))
            tarrow(at(v, cm.index), paramKey(callee, params[k]), guard ? { only: guard.only } : null);
        });
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

  return { macros, typedefs, sources, resolver, forms, fnsOf, targets, records, targetKey: key,
           tags, tagKey: (path, f, v, at) => tagKey(path, f, v, at), tagIssues, tagChecks, interfaceTables };
}

/* EVERY INSTALLED MEMBER, ATTRIBUTED TO THE INTERFACE ITS TARGET IS — one record per (member, site), carrying
   the interfaces it lands on or, when the target's interface cannot be decided, nothing and the reason. A
   record with no interface is NOT credited to the file it was written in: that fallback is exactly the false
   COMPLETE this attribution exists to remove, and the caller reports it as its own category instead.
   `paths` are absolute; pass the whole program, since which interface a member belongs to is a fact about the
   object it is installed on and not about which row named the file. */
export function installedMembers(paths, env) {
  const records = [], unresolved = [], offInstaller = [], excluded = [];
  const { forms } = env;

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
    const isTarget = (f, v) => env.targets.has(env.targetKey(path, f.name, v));
    /* Every name is resolved in the SCOPE the construct stands in — the enclosing function's tables, then the
       file's, then a header's. */
    const scoped = (f) => env.resolver.for(path, f ? f.name : null);

    const report = (off, form, expr) =>
      unresolved.push({ file: path, line: lineOf(orig, off), form, expr: expr.trim().replace(/\s+/g, " ") });

    /* WHICH INTERFACE THIS TARGET IS. `off` is a FILE offset; the tag graph is keyed by the assignment that
       produced the value, so the answer is the one that holds at this line and not at the end of the function. */
    const interfacesOf = (f, targetExpr, off) => {
      const v = stripDup(targetExpr || "");
      if (!/^[A-Za-z_]\w*$/.test(v))
        return { ifaces: [], candidates: [], why: `the install target \`${(targetExpr || "").trim()}\` is not a named object` };
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
    const emitWith = (names, stub, a, off, form) => {
      const line = lineOf(orig, off);
      for (const name of names)
        records.push({ name, stubbed: !!stub, file: path, line, form,
                       ifaces: a.ifaces, candidates: a.candidates, why: a.why });
    };
    const emit = (names, stub, f, targetExpr, off, form) =>
      emitWith(names, stub, interfacesOf(f, targetExpr, off), off, form);

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
        const raw = stripCast(site.args[pos] || "");
        const root = (raw.match(/^([A-Za-z_]\w*)/) || [])[1];
        if (forms.has(f.name) && root && f.params.includes(root)) continue;
        if (TABLE_FORMS.some((t) => t.install === f.name)) continue;
        const target = stripCast(site.args[form.target] || "");
        if (form.ambiguous && !isTarget(f, target)) {
          /* NOT an install target: nothing anywhere in the program installs a member on this object, so this is
             a record field — a listener registration, an event's internal slot bag, a response summary. Not
             counted, and not a gap either. It is recorded by NAME so the caller can say so when the name it
             writes happens to be a member of the interface being audited: `document.title` and
             `MessageChannel.port1` really are IDL members written this way, and calling either ABSENT would be
             as false as counting every "removed" flag as installed. */
          const names = scoped(f).strings(site.args[pos] || "", localsFor(f));
          for (const n of names || [])
            offInstaller.push({ name: n, file: path, line: lineOf(orig, site.at), target, form: callee });
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
        emit(names, noop, f, target, site.at, callee);
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
        emit(names, noop, f, stripCast(site.args[1] || ""), site.at, `JS_SetPropertyFunctionList/${tableExpr}`);
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
            emitWith(names, false, { ifaces: iname, candidates: [], why: null }, site.at, form.declare);
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
        const parts = chosen.map((s) => interfacesOf(fnAt(s.at), stripCast(s.args[form.target] || ""), s.at));
        const a = parts.length
          ? { ifaces: [...new Set(parts.flatMap((p) => p.ifaces))],
              candidates: [...new Set(parts.flatMap((p) => p.candidates))],
              why: parts.some((p) => !p.why) ? null : parts[0].why }
          : { ifaces: [], candidates: [],
              why: `nothing in this file installs the ${form.declare} registration` +
                   (lhs ? ` held in \`${lhs}\`` : " (it is not held in a named handle)") };
        emitWith(names, false, a, site.at, form.declare);
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
  return { records, unresolved, offInstaller, excluded };
}
