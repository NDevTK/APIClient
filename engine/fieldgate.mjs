/* THE RECORD-FIELD CONTRACT AUDITOR — the gate for the defect class §Architecture names and nothing else has.
 *
 * "A FIELD A CONSUMER DEFAULTS IS A FIELD NOBODY WILL NOTICE IS NEVER WRITTEN, and the default is not the
 * symptom — it is the concealment." Every instance of that defect is a live product bug that is INVISIBLE as a
 * crash: a popup badging every finding HIGH because it read `cspBlocked` where the engine emits `cspBlocks`; a
 * whole reply path unreachable because a reader branched on a value the producer never returns; a diagnostic
 * reading zero for every session there has ever been because its three field names are emitted by nothing.
 * They were each found by a person chasing a wrong number, hours or days later. The rule §Architecture gives is
 * mechanical — "a name that is READ somewhere and WRITTEN nowhere is a broken contract, and a default is what
 * stops it being a crash" — and a mechanical rule is a GATE, which is what this is.
 *
 * IT IS MODELLED ON idlgen.mjs, WHICH AUDITS THE SAME SHAPE ONE LAYER UP. There, the spec's member list is
 * diffed against what the components install, an install construct whose name cannot be resolved statically is
 * reported UNRESOLVED with its file and line, and the exit code is the verdict: no baseline, no allowlist, no
 * threshold, no --warn-only. Here the producer's emitted field list is diffed against what the consumers read.
 * There is nothing to update when a record gains a field — the gate fails only when the two sides disagree.
 *
 * THE CATEGORIES, AND WHY THE DEFAULTED ONE IS THE ONE THAT MATTERS.
 *   READ-NO-WRITER   a consumer names a field of a record no producer emits. The wrong number itself. A rename
 *                    prints on BOTH sides — the new spelling READ with nothing writing it, the old spelling
 *                    WRITTEN with nothing reading it — which is why a rename is the loudest thing here.
 *   WRITE-NO-READER  a producer emits a field nothing reads: a measurement that has never once been looked at,
 *                    or the surviving half of a rename.
 *   DEFAULTED        a read of a record field through `||`, `??`, `?.`, or inside a swallowing try/catch, and
 *                    the coarser DEFAULTED WHOLE RECORD — a `|| { … }` substituting an entire emitted shape.
 *                    This is the CONCEALMENT: it is what turns the categories above from a crash into a
 *                    plausible datum, and it is a defect on its own terms even where the writer exists today,
 *                    because it is what will hide the writer's removal tomorrow.
 *   AMBIGUOUS        a receiver whose record identity this cannot decide. Counted, never resolved either way.
 *   REFUSED          a construct this scan cannot read. Counted and named with file:line, never guessed past.
 *
 * WHY REFUSING IS THE WHOLE DISCIPLINE. idl_installed.mjs records the same primitive lying four separate ways
 * because it read TEXT where it needed a STRUCTURE — a member credited because the word appeared in the file,
 * a `lastIndexOf` matching inside a longer identifier, an indirect call matching no pattern. A scan that
 * guesses hands the analysis a plausible answer and the refusal machinery never runs. So every name here is
 * taken OUT OF A CONSTRUCT: a JSON key position inside a resolved emission format, an object-literal key, a
 * member expression whose receiver this can normalize. A computed key, an unresolvable format argument, a
 * receiver that is not a normalizable chain, a file whose brackets do not balance — each is a REFUSAL with its
 * file and line, and zero refusals is the armed state.
 *
 * WHAT ANCHORS A READ TO A RECORD, which is the question that decides whether this report is signal or noise.
 * Every `.length` and `.textContent` in the corpus is a field read, and a gate that lists them lists nothing.
 * The anchor is the PRODUCER'S OWN DECLARATION, exactly as idlgen's anchor is the .idl — and it is a SHAPE,
 * not a name. One C function's emissions are one record (endpoint_json_array's method/url/params/name/location,
 * result_json's counters, solve_json_array's sink/source/poc/firesOn), and a receiver is that record when it is
 * READ LIKE IT: TWO of its fields, never one. One is not enough because `name`, `url`, `type` and `source` are
 * both emitted here and what every other object in a JS corpus calls its fields — anchoring on one of those
 * anchors an AST node as an endpoint and a DOM node as a finding, and the report becomes a listing of the
 * language. `f.sink` and `f.poc` are two of solve.c's, so `f` is a finding and `f.cspBlocked` is a read of a
 * field nothing writes. A member IMMEDIATELY CALLED is skipped as an operation rather than a datum, which is
 * sound and not merely convenient: a record on this seam is JSON text, and JSON carries no functions.
 * The single-field case is NOT a pass — it is the AMBIGUOUS category, named with its place, because "is this
 * an endpoint or an AST node" is a question this cannot answer and answering it either way is the guess.
 *
 * WHAT IS AUDITED AND WHAT IS DELIBERATELY NOT.
 *   THE SERIALIZED SEAM. A record is JSON text the engine PRINTS and the host PARSES, plus the `@TAG` stream
 *   markers the engine prints and the host matches. That is the seam where a name has two independent
 *   spellings and nothing checks them against each other.
 *   NOT the in-heap Web IDL surface. `JS_SetPropertyStr(ctx, obj, "status", v)` installs a member on an object
 *   a page reaches, and WHICH members those must be is decided by the IDL — idlgen.mjs already audits it, off
 *   the spec rather than off the consumers. Counting those here would drown this report in the other gate's
 *   subject and would answer its question worse.
 *   NOT the qjs_* ABI. Its C entries are `QJS_EXPORT` declarations and its JS counterparts are named through
 *   `engine/build.mjs`'s QJS_ABI table and `extension/mojom.js`'s interface records, so resolving that
 *   namespace means reading build.mjs — which is the follow-up named at the bottom of this file, not a
 *   namespace to half-resolve here. A namespace this cannot read soundly is one it does not declare.
 *
 * THE CORPUS IS A RULE, NOT A LIST, AND IT IS PRINTED. Producer and consumer both: engine/host/** (the C that
 * emits and reads back), engine/*.mjs (the drivers), extension/**\/*.js minus the generated wasm glue, and the
 * top level of testing/ minus its one-off `debug-*` probes. Fixture pages under testing/fixtures and
 * testing/corpus are the SUBJECT of a run, not a party to the seam: crediting a fixture's own object literal
 * as the producer of an engine field name is the false COMPLETE idl_installed.mjs was rewritten to remove.
 */
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, extname, relative, basename, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..");

/* ---- corpus ---------------------------------------------------------------------------------------------- */

function walk(dir, out = []) {
  let ents;
  try { ents = readdirSync(dir); } catch { return out; }
  for (const e of ents) {
    const p = join(dir, e);
    let st;
    try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) walk(p, out);
    else out.push(p);
  }
  return out;
}

/* The corpus rule, applied per path. A file is in because of WHERE it is and WHAT it is, never because it was
   named — a list of seam files is a list a reader has to keep true, and the first file that joins the seam
   without joining the list goes unaudited exactly as an uncollected WPT test does. */
function corpus() {
  const files = [];
  for (const p of walk(join(ROOT, "engine", "host")))
    if (extname(p) === ".c" || extname(p) === ".h") files.push({ path: p, lang: "c", area: "engine/host" });
  /* …minus this file. An instrument is not a party to the seam it measures, and including it is not neutral:
     the report's OWN vocabulary — `file`, `line`, `name`, `shape`, `keys` — becomes a producer, and every one
     of those names is then immune to the read-with-no-writer diff. Reconstructing `canVerify`'s missing
     `shape` field found exactly that: the defect went unreported because this file writes `shape:` in its own
     result rows. */
  for (const p of walk(join(ROOT, "engine")))
    if (extname(p) === ".mjs" && dirname(p) === join(ROOT, "engine") && p !== fileURLToPath(import.meta.url))
      files.push({ path: p, lang: "js", area: "engine drivers" });
  for (const p of walk(join(ROOT, "extension")))
    if (extname(p) === ".js" && !p.includes(join("lib", "qjs")))
      files.push({ path: p, lang: "js", area: "extension" });
  for (const p of walk(join(ROOT, "testing")))
    if (extname(p) === ".js" && dirname(p) === join(ROOT, "testing") && !basename(p).startsWith("debug-"))
      files.push({ path: p, lang: "js", area: "testing drivers" });
  return files;
}

/* ---- refusals -------------------------------------------------------------------------------------------- */

/* A refusal is a PLACE and a REASON, because the only useful thing to do with one is open it. They are grouped
   by reason in the verdict so a class of unreadable construct is one line rather than four hundred. */
const refusals = [];
const refuse = (file, line, reason, text) =>
  refusals.push({ file, line, reason, text: (text || "").replace(/\s+/g, " ").slice(0, 90) });

/* ---- masking: comments out, string interiors out, length preserved ---------------------------------------- */

/* Two views of one file at IDENTICAL offsets. `code` has comments blanked and string literals INTACT — it is
   what the member names are read out of. `struct` additionally has every string/template/regex INTERIOR
   blanked — it is what structure is scanned in, so a `printf(` inside a comment or a `{` inside a string is
   not a construct. Every replacement is the same length as what it replaces, so an offset into either view is
   an offset into the original and a reported line number is the real one. */

const lineOf = (src, off) => {
  let line = 1;
  for (let i = 0; i < off && i < src.length; i++) if (src[i] === "\n") line++;
  return line;
};

const blank = (arr, from, to) => { for (let i = from; i < to && i < arr.length; i++) if (arr[i] !== "\n") arr[i] = " "; };

/* C. A block comment's newlines go too, because that is what the preprocessor does — line splicing is phase 2
   and comment removal is phase 3, so a multi-line comment inside a #define is whitespace and the macro
   continues past it. Line numbers are computed from the ORIGINAL source, so losing them here costs nothing. */
function maskC(src) {
  const code = src.split(""), struct = src.split("");
  const n = src.length;
  let i = 0;
  while (i < n) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "*") {
      let j = i + 2;
      while (j < n && !(src[j] === "*" && src[j + 1] === "/")) j++;
      for (let k = i; k < Math.min(j + 2, n); k++) { code[k] = " "; struct[k] = " "; }
      i = j + 2; continue;
    }
    if (c === "/" && src[i + 1] === "/") {
      let j = i;
      while (j < n && src[j] !== "\n") j++;
      for (let k = i; k < j; k++) { code[k] = " "; struct[k] = " "; }
      i = j; continue;
    }
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c) { if (src[j] === "\\") j++; j++; }
      blank(struct, i + 1, Math.min(j, n));
      i = j + 1; continue;
    }
    if (c === "\\" && src[i + 1] === "\n") { code[i] = code[i + 1] = " "; struct[i] = struct[i + 1] = " "; i += 2; continue; }
    i++;
  }
  return { code: code.join(""), struct: struct.join("") };
}

/* JS, and the one place a masker for this language can silently be wrong: `/` is division or the start of a
   regular expression depending on the PRECEDING token, and getting it backwards does not fail — it swallows
   the rest of the file into a literal that never closes, or turns a literal's contents into structure. So the
   decision is made from the previous significant token, `}` is resolved toward a regex (a block far more often
   ends there than an object literal is divided), and the answer is CHECKED: a file whose masked brackets do
   not balance is REFUSED whole rather than half-read, which is the one question whose failure invalidates
   every other answer about that file. */
const REGEX_OK_WORD = new Set(["return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "throw",
  "case", "do", "else", "yield", "await", "if", "while", "for", "switch"]);

function maskJS(src) {
  const code = src.split(""), struct = src.split("");
  const n = src.length;
  /* the template/`${}` stack: each entry is the brace depth at which a `${` was opened */
  const tmpl = [];
  let braceDepth = 0;
  let i = 0;
  /* A `#!` line is not JavaScript and its `/usr/bin` is not a regular expression — reading it as one swallows
     the file and this reported `engine/sync.mjs` as untokenizable for exactly that reason. */
  if (src[0] === "#" && src[1] === "!") { while (i < n && src[i] !== "\n") i++; blank(code, 0, i); blank(struct, 0, i); }
  let prevSig = "", prevWord = "";
  const note = (ch) => { if (ch && !/\s/.test(ch)) prevSig = ch; };
  while (i < n) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "*") {
      let j = i + 2;
      while (j < n && !(src[j] === "*" && src[j + 1] === "/")) j++;
      blank(code, i, Math.min(j + 2, n)); blank(struct, i, Math.min(j + 2, n));
      i = j + 2; continue;
    }
    if (c === "/" && src[i + 1] === "/") {
      let j = i;
      while (j < n && src[j] !== "\n") j++;
      blank(code, i, j); blank(struct, i, j);
      i = j; continue;
    }
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c) {
        if (src[j] === "\n") return { fault: { off: i, why: "an unterminated string literal" } };
        if (src[j] === "\\") j++;
        j++;
      }
      blank(struct, i + 1, Math.min(j, n));
      i = j + 1; prevSig = '"'; prevWord = ""; continue;
    }
    if (c === "`") {
      /* the literal chunks are blanked in `struct`; a `${` recurses into ordinary scanning so a member read
         inside an interpolation is still structure */
      let j = i + 1;
      for (;;) {
        if (j >= n) return { fault: { off: i, why: "an unterminated template literal" } };
        if (src[j] === "\\") { j += 2; continue; }
        if (src[j] === "`") { blank(struct, i + 1, j); i = j + 1; break; }
        if (src[j] === "$" && src[j + 1] === "{") {
          blank(struct, i + 1, j);
          tmpl.push(braceDepth);
          braceDepth++;
          i = j + 2;
          break;
        }
        j++;
      }
      prevSig = "`"; prevWord = ""; continue;
    }
    if (c === "/") {
      const wordIsKeyword = prevWord && REGEX_OK_WORD.has(prevWord);
      const divides = !wordIsKeyword && (/[A-Za-z0-9_$)\]]/.test(prevSig));
      if (!divides) {
        let j = i + 1, cls = false;
        for (;;) {
          if (j >= n || src[j] === "\n") return { fault: { off: i, why: "an unterminated regular expression" } };
          if (src[j] === "\\") { j += 2; continue; }
          if (src[j] === "[") cls = true;
          else if (src[j] === "]") cls = false;
          else if (src[j] === "/" && !cls) break;
          j++;
        }
        blank(struct, i + 1, j);
        i = j + 1;
        while (i < n && /[a-z]/.test(src[i])) i++;
        prevSig = "/"; prevWord = ""; continue;
      }
    }
    if (c === "{") braceDepth++;
    else if (c === "}") {
      braceDepth--;
      if (tmpl.length && braceDepth === tmpl[tmpl.length - 1]) {
        /* back into the template that opened this `${` */
        tmpl.pop();
        let j = i + 1;
        for (;;) {
          if (j >= n) return { fault: { off: i, why: "an unterminated template literal" } };
          if (src[j] === "\\") { j += 2; continue; }
          if (src[j] === "`") { blank(struct, i + 1, j); i = j + 1; break; }
          if (src[j] === "$" && src[j + 1] === "{") { blank(struct, i + 1, j); tmpl.push(braceDepth); braceDepth++; i = j + 2; break; }
          j++;
        }
        prevSig = "`"; prevWord = ""; continue;
      }
    }
    if (/[A-Za-z_$]/.test(c)) {
      let j = i;
      while (j < n && /[\w$]/.test(src[j])) j++;
      prevWord = src.slice(i, j);
      prevSig = src[j - 1];
      i = j; continue;
    }
    note(c);
    if (!/\s/.test(c)) prevWord = "";
    i++;
  }
  return { code: code.join(""), struct: struct.join("") };
}

/* ---- balanced scanning ----------------------------------------------------------------------------------- */

const PAIRS = { "(": ")", "[": "]", "{": "}" };

/* One past the closer matching the opener at `open`, or -1 when the text ends first or a closer of the WRONG
   KIND arrives. Counting one depth across the three kinds does not return -1 on a mismatch, it returns a SPAN,
   and a plausible wrong span is the failure mode this whole file is built against. */
function matchAt(struct, open) {
  if (!PAIRS[struct[open]]) return -1;
  const stack = [];
  for (let i = open; i < struct.length; i++) {
    const c = struct[i];
    if (PAIRS[c]) stack.push(c);
    else if (c === ")" || c === "]" || c === "}") {
      if (PAIRS[stack.pop()] !== c) return -1;
      if (!stack.length) return i + 1;
    }
  }
  return -1;
}

/* Asked ONCE per file, where the answer is a line a person can open. Every span primitive below assumes the
   masked text's brackets balance and nest by kind, and each fails DIFFERENTLY and silently when they do not. */
function bracketFault(struct) {
  const stack = [];
  for (let i = 0; i < struct.length; i++) {
    const c = struct[i];
    if (PAIRS[c]) stack.push({ c, i });
    else if (c === ")" || c === "]" || c === "}") {
      const t = stack.pop();
      if (!t || PAIRS[t.c] !== c) return { off: i, why: `a '${c}' closing a '${t ? t.c : "nothing"}'` };
    }
  }
  return stack.length ? { off: stack[stack.length - 1].i, why: `an unclosed '${stack[stack.length - 1].c}'` } : null;
}

/* Top-level `,`-separated spans inside a bracket pair. */
function splitTop(struct, from, to) {
  const parts = [];
  let depth = 0, start = from;
  for (let i = from; i < to; i++) {
    const c = struct[i];
    if (PAIRS[c]) depth++;
    else if (c === ")" || c === "]" || c === "}") depth--;
    else if (c === "," && depth === 0) { parts.push([start, i]); start = i + 1; }
  }
  parts.push([start, to]);
  return parts;
}

/* Call sites of a named function, found in the STRUCT view so a name inside a string or a comment is not one.
   The name must not be preceded by an identifier character — the `lastIndexOf`-matching-inside-a-longer-name
   defect idl_installed.mjs records. */
function callSites(struct, name) {
  const out = [];
  const re = new RegExp(`\\b${name}\\s*\\(`, "g");
  let m;
  while ((m = re.exec(struct))) {
    const open = struct.indexOf("(", m.index);
    if (m.index > 0 && /[\w$]/.test(struct[m.index - 1])) continue;
    const close = matchAt(struct, open);
    if (close < 0) { out.push({ at: m.index, open, close: -1, args: null }); continue; }
    out.push({ at: m.index, open, close, args: splitTop(struct, open + 1, close - 1) });
    re.lastIndex = close;
  }
  return out;
}

const sig = (s, from, to) => { let i = from; while (i < to && /\s/.test(s[i])) i++; return i; };

/* ---- the record namespaces ------------------------------------------------------------------------------- */

/* A field name, with every place that WRITES it and every place that READS it. The diff between the two sides
   is the whole verdict; nothing else is stored, and there is no expected-shape file to keep true. */
/* THE SEAM'S PRODUCER SIDE, which is what anchors everything below — the field names the C engine actually
   SERIALIZES. It plays the part the .idl plays in idlgen: the authority a consumer is diffed against. A record
   name a page or a popup invents for its own view object is not part of this contract and does not anchor
   anything, for the same reason a fixture page's object literal must not credit an engine field. */
const cEmitted = new Set();

/* AND IT ANCHORS AS A SHAPE, NOT AS A NAME, because a name on its own cannot say which record it came from.
   `name`, `url`, `type` and `source` are emitted by this engine AND are what every other object in a JS corpus
   calls its fields, so a receiver anchored by one of them anchors everything — an AST node read as an endpoint
   record, a DOM node read as a finding. A SHAPE is the set of field names ONE producer emits together (one C
   function's emissions are one record: endpoint_json_array's method/url/params/name/location/validValues,
   result_json's twenty-four counters, solve_json_array's sink/source/poc/firesOn), and a receiver is that
   record when it is READ LIKE IT — two of its fields, not one. The single-name case is not dropped: it is the
   AMBIGUOUS category below, named with its place, because "is this an endpoint or an AST node" is a question
   this cannot answer and answering it either way is the guess the whole file is built against. */
const shapes = new Map();   // "file:offset" -> Set(name)
const shapeOf = (id) => { let s = shapes.get(id); if (!s) shapes.set(id, (s = new Set())); return s; };

const fields = new Map();   // name -> {writes:[site], reads:[site]}
const markers = new Map();  // "@TAG" -> {writes:[site], reads:[site]}
const rec = (map, name) => {
  let e = map.get(name);
  if (!e) map.set(name, (e = { writes: [], reads: [] }));
  return e;
};

const defaulted = [];   // {file,line,name,form,recv}

/* A JSON key POSITION, which is the construct — `"name"` followed by a colon. A bare `"name"` in an emission
   is a VALUE and declares nothing, which is the whole difference between reading a structure and scanning for
   a word. */
const JSON_KEY = /"([A-Za-z_$][\w$]*)"\s*:/g;
const keysIn = (text) => { const out = []; let m; JSON_KEY.lastIndex = 0; while ((m = JSON_KEY.exec(text))) out.push(m[1]); return out; };

/* A stream marker is `@TAG` at the very start of an emission — `printf("@COLD {...")`. Unlike a member name,
   `@COLD` in a string literal cannot mean anything else in this tree, so for THIS namespace a literal is the
   construct. The asymmetry is deliberate and is why the two namespaces are kept apart. */
const MARKER_ANY = /(^|[^A-Za-z0-9_@])(@[A-Z][A-Z0-9_]*)/g;
const markersIn = (text) => { const out = []; let m; MARKER_ANY.lastIndex = 0; while ((m = MARKER_ANY.exec(text))) out.push(m[2]); return out; };

/* ---- C side ---------------------------------------------------------------------------------------------- */

/* The emission vocabulary. `printf`/`fprintf`/`js_printf` write the seam directly; `json_buf_puts`/`_str` are
   how endpoint.c and solve.c build their arrays; `snprintf`/`sprintf` build a buffer whose fate is decided
   below. Anything else that could carry a JSON key is a refusal, not an assumption. */
/* `json_buf_str` is NOT here and that is the point: it serializes its argument AS a JSON string, escaping the
   quotes, so a key can never come out of it — endpoint.c writes `json_buf_puts(&b, "{\"method\":")` for the key
   and `json_buf_str(&b, e->method)` for the value. Listing it made twenty-nine value expressions into refusals
   about a field that could not have been there. */
const C_EMIT = { printf: 0, fprintf: 1, js_printf: 0, json_buf_puts: 1 };
const C_BUILD = { snprintf: 2, sprintf: 1 };
const C_MATCH = ["strstr", "strcasestr", "strncmp", "strcmp", "memmem", "strnstr"];

/* Adjacent string literals, concatenated and unescaped, or null when the span holds anything else. A macro, a
   variable, a ternary — each is a format this cannot resolve, and resolving it wrong invents a field. */
function cLiteral(code, struct, from, to) {
  let i = sig(struct, from, to), out = "", any = false;
  while (i < to) {
    if (code[i] !== '"') break;
    let j = i + 1, buf = "";
    while (j < to && code[j] !== '"') {
      if (code[j] === "\\") {
        const e = code[j + 1];
        buf += e === "n" ? "\n" : e === "t" ? "\t" : e === "r" ? "\r" : e === "0" ? "\0" : e;
        j += 2; continue;
      }
      buf += code[j]; j++;
    }
    if (j >= to) return null;
    out += buf; any = true;
    i = sig(struct, j + 1, to);
  }
  return any && i >= to ? out : null;
}

/* Whether an identifier is handed to a matcher anywhere in the file — the one structural fact that separates a
   buffer BUILT to be emitted from a PATTERN built to be looked for. test_forced.c's
   `snprintf(pat, sizeof pat, "\"url\":\"%s\"", url)` then `strstr(hay, pat)` is a READ of the seam; result.c's
   `snprintf(out, n, "{\"fetchCallSites\":%s,...")` then `return out` is a WRITE of it, and nothing but the
   destination's later use tells them apart. A destination that is not a plain identifier is a refusal. */
function matcherOperands(struct, code) {
  const set = new Set();
  for (const fn of C_MATCH)
    for (const c of callSites(struct, fn)) {
      if (!c.args) continue;
      for (const [a, b] of c.args) {
        const t = code.slice(a, b).trim();
        if (/^[A-Za-z_]\w*$/.test(t)) set.add(t);
      }
    }
  return set;
}

function scanC(file, src) {
  const { code, struct } = maskC(src);
  const fault = bracketFault(struct);
  if (fault) { refuse(file, lineOf(src, fault.off), "a C file whose brackets do not balance — every span answer about it would be a guess", fault.why); return; }
  const patterns = matcherOperands(struct, code);
  const site = (off) => ({ file, line: lineOf(src, off) });

  /* ONE C FUNCTION'S EMISSIONS ARE ONE RECORD. endpoint_json_array writes its object one `json_buf_puts` at a
     time, so the call is not the record and the literal is not the shape — the enclosing body is. Top-level
     brace spans are what a function body is in a masked C file; a file-scope initializer is one too, and an
     initializer that emits JSON keys is a record by any reading, so nothing needs to tell them apart. */
  const bodies = [];
  {
    let depth = 0;
    for (let i = 0; i < struct.length; i++) {
      const c = struct[i];
      if (c === "{") { if (depth === 0) { const e = matchAt(struct, i); if (e > 0) bodies.push([i, e]); } depth++; }
      else if (c === "}") depth--;
    }
  }
  const bodyOf = (off) => { for (const [a, b] of bodies) if (off >= a && off < b) return `${file}:${lineOf(src, a)}`; return `${file}:file-scope`; };

  const emit = (text, off, isWrite) => {
    const ks = keysIn(text);
    for (const k of ks) {
      (isWrite ? rec(fields, k).writes : rec(fields, k).reads).push(site(off));
      if (isWrite) { cEmitted.add(k); shapeOf(bodyOf(off)).add(k); }
    }
    return ks.length;
  };

  /* THE MARKER NAMESPACE IS NOT THE FIELD NAMESPACE, AND ITS CONSTRUCT IS THE LITERAL. `@COLD` cannot mean
     anything but the stream marker in this tree, so unlike a member name it needs no surrounding structure to
     be a declaration — which is why the two are audited apart rather than under one rule. `@WHY` is emitted
     through check.h's APICLIENT_ASSERT_EMIT and `@WPTSTART` through a C string holding JS source, and a
     vocabulary of emission functions sees neither: both are the marker's own text, printed. The one thing
     that changes its role is the same fact that changes a built buffer's — a literal handed to a matcher is
     being LOOKED FOR, not printed. */
  {
    const matched = new Set();
    for (const fn of C_MATCH)
      for (const c of callSites(struct, fn)) {
        if (!c.args) continue;
        for (const [a, b] of c.args) {
          const lit = cLiteral(code, struct, a, b);
          if (lit) for (const t of markersIn(lit)) { matched.add(t); rec(markers, t).reads.push(site(a)); }
        }
      }
    const LIT = /"(?:[^"\\\n]|\\.)*"/g;
    let m;
    while ((m = LIT.exec(code))) {
      if (struct[m.index] !== '"') continue;
      for (const t of markersIn(m[0])) if (!matched.has(t)) rec(markers, t).writes.push(site(m.index));
    }
  }

  for (const [fn, fmtIdx] of Object.entries(C_EMIT))
    for (const c of callSites(struct, fn)) {
      if (!c.args) { refuse(file, lineOf(src, c.at), `an unbalanced ${fn}( — the emission's format argument cannot be delimited`); continue; }
      const a = c.args[fmtIdx];
      if (!a) { refuse(file, lineOf(src, c.at), `a ${fn}( with too few arguments to hold a format`); continue; }
      const lit = cLiteral(code, struct, a[0], a[1]);
      if (lit === null) {
        /* An unresolvable format is only a refusal where a key could be hiding: a call whose format is a
           variable in a file that emits no JSON at all is a log line, and counting it would bury the report
           in the noise idl_installed.mjs's UNATTRIBUTED list drowned in. The file-level test is stated so it
           can be argued with: this refuses in every file that emits at least one resolved key. */
        pendingUnresolved.push({ file, line: lineOf(src, c.at), fn, text: code.slice(a[0], Math.min(a[1], a[0] + 60)) });
        continue;
      }
      emit(lit, a[0], true);
    }

  for (const [fn, fmtIdx] of Object.entries(C_BUILD))
    for (const c of callSites(struct, fn)) {
      if (!c.args) { refuse(file, lineOf(src, c.at), `an unbalanced ${fn}( — the built text cannot be delimited`); continue; }
      const a = c.args[fmtIdx], d = c.args[0];
      if (!a || !d) { refuse(file, lineOf(src, c.at), `a ${fn}( with too few arguments to hold a destination and a format`); continue; }
      const lit = cLiteral(code, struct, a[0], a[1]);
      if (lit === null) { pendingUnresolved.push({ file, line: lineOf(src, c.at), fn, text: code.slice(a[0], Math.min(a[1], a[0] + 60)) }); continue; }
      if (!keysIn(lit).length) continue;
      const dst = code.slice(d[0], d[1]).trim();
      if (!/^[A-Za-z_]\w*$/.test(dst)) {
        refuse(file, lineOf(src, c.at), `a ${fn}( carrying a JSON key into a destination that is not a plain identifier — whether this text is emitted or matched is not decidable here`, dst);
        continue;
      }
      emit(lit, a[0], !patterns.has(dst));
    }

  /* The read side of the seam in C: a JSON key literal handed to a matcher. */
  for (const fn of C_MATCH)
    for (const c of callSites(struct, fn)) {
      if (!c.args) continue;
      for (const [a, b] of c.args) {
        const lit = cLiteral(code, struct, a, b);
        if (lit) emit(lit, a, false);
      }
    }

  /* A JSON key literal in a named string constant — `static const char PARKED[] = ",\"search\":\"parked\"..."`.
     Its role follows its identifier exactly as a snprintf destination's does. */
  const DECL = /\b(?:static\s+)?(?:const\s+)?char\s+(?:\*\s*)?([A-Za-z_]\w*)\s*(?:\[\s*\])?\s*=\s*(?=")/g;
  let m;
  while ((m = DECL.exec(struct))) {
    const at = m.index + m[0].length;
    let end = at;
    while (end < struct.length && struct[end] !== ";") end++;
    const lit = cLiteral(code, struct, at, end);
    if (lit === null) continue;
    if (!keysIn(lit).length) continue;
    emit(lit, at, !patterns.has(m[1]));
  }
}

/* Unresolvable formats are held until the whole C corpus is read, because whether one is a hiding place for a
   field depends on whether its FILE emits fields at all — and that is not known until the file is done. */
const pendingUnresolved = [];

/* ---- JS side --------------------------------------------------------------------------------------------- */

/* A receiver is normalized to its SOURCE TEXT, whitespace-collapsed. Two reads anchor to one record only when
   they are written identically — an aliasing question this deliberately does not answer, because answering it
   by flowing the fact along assignments and into parameters is what made idl_installed.mjs's second solve
   report a union of every caller's object. Identical text is a fact; a flowed identity is an inference. */
function receiverBefore(struct, code, dotAt) {
  let i = dotAt;
  while (i > 0 && /\s/.test(struct[i - 1])) i--;
  if (i === 0) return null;
  const end = i;
  /* Walk LEFT over one primary expression: identifiers, `.`/`?.` links, and balanced `(…)`/`[…]` suffixes.
     A step this cannot take stops the walk with a refusal rather than with a shorter answer — a truncated
     receiver is a DIFFERENT receiver, and two reads anchoring to two spellings of one object is the silent
     half of this failure, not the loud one. */
  const link = () => {
    let j = i;
    while (j > 0 && /\s/.test(struct[j - 1])) j--;
    if (struct[j - 1] === "." && !(j >= 2 && struct[j - 2] === ".")) {
      i = struct[j - 2] === "?" ? j - 2 : j - 1;
      while (i > 0 && /\s/.test(struct[i - 1])) i--;
      return true;
    }
    return false;
  };
  for (;;) {
    while (i > 0 && /\s/.test(struct[i - 1])) i--;
    const c = struct[i - 1];
    if (c === ")" || c === "]") {
      let depth = 0, j = i - 1;
      for (; j >= 0; j--) {
        const d = struct[j];
        if (d === ")" || d === "]" || d === "}") depth++;
        else if (d === "(" || d === "[" || d === "{") { depth--; if (!depth) break; }
      }
      if (j < 0) return null;
      i = j;
      while (i > 0 && /\s/.test(struct[i - 1])) i--;
      while (i > 0 && /[\w$]/.test(struct[i - 1])) i--;
      if (link()) continue;
      break;
    }
    if (c !== undefined && /[\w$]/.test(c)) {
      while (i > 0 && /[\w$]/.test(struct[i - 1])) i--;
      if (link()) continue;
      break;
    }
    /* A chain whose left end is a STRING, a template or a regular expression is DECIDED, not unreadable:
       `"a,b".split(",").length` is an operation on a literal and no record ever crossed a seam as one. A
       decided negative is not a refusal — counting it as one would report the language again. */
    if (c === '"' || c === "'" || c === "`" || c === "/") return "";
    return null;
  }
  const t = code.slice(i, end).replace(/\s+/g, "");
  if (/^[A-Za-z_$]/.test(t)) return /^[^\s;]*$/.test(t) ? t : null;
  /* An ARRAY LITERAL head — `[...m.values()].filter(f).length` — is a computed collection and no record ever
     crossed a seam as one: a decided negative. A PARENTHESIZED head is not decided (`(await f()).field` could
     be anything), so it stays a refusal rather than a quiet pass. */
  return t[0] === "[" ? "" : null;
}

/* A swallowing try: its catch body neither rethrows nor asserts, so every read inside the try is a read whose
   absence the consumer has already decided to survive. §Architecture names `a catch {} around a read` in the
   same breath as `|| 0` for exactly this reason. */
function swallowingTrySpans(struct) {
  const spans = [];
  const re = /\btry\s*\{/g;
  let m;
  while ((m = re.exec(struct))) {
    const open = struct.indexOf("{", m.index);
    const close = matchAt(struct, open);
    if (close < 0) continue;
    const after = sig(struct, close, struct.length);
    if (!struct.startsWith("catch", after)) continue;
    let cb = after + 5;
    while (cb < struct.length && struct[cb] !== "{" && struct[cb] !== ";") cb++;
    if (struct[cb] !== "{") continue;
    const cbEnd = matchAt(struct, cb);
    if (cbEnd < 0) continue;
    const body = struct.slice(cb + 1, cbEnd - 1);
    /* `RETHROW_FATAL(e)` is this corpus's OWN non-swallowing construct — extension/check.js's one primitive C
       does not need, which re-throws an invariant abort while letting a page's real failure through. A catch
       that calls it is not swallowing, and missing that reported the most carefully asserted reads in the
       bridge (safeFetch's reply record, DCHECKed field by field) as concealed ones. */
    if (/\bthrow\b|\bDCHECK\b|\bCHECK\b|\bDFAIL\b|\bRETHROW_FATAL\b/.test(body)) continue;
    spans.push([open, close]);
    re.lastIndex = cbEnd;
  }
  return spans;
}

const inSpan = (spans, off) => spans.some(([a, b]) => off >= a && off < b);

function scanJS(file, src) {
  const masked = maskJS(src);
  if (masked.fault) { refuse(file, lineOf(src, masked.fault.off), "a JS file this masker could not tokenize", masked.fault.why); return null; }
  const { code, struct } = masked;
  const fault = bracketFault(struct);
  if (fault) { refuse(file, lineOf(src, fault.off), "a JS file whose masked brackets do not balance — the mask is wrong somewhere above this and every span answer about the file would be a guess", fault.why); return null; }

  const site = (off) => ({ file, line: lineOf(src, off) });
  const swallow = swallowingTrySpans(struct);
  const localReads = [];   // {name, recv, off, form}
  const localWrites = [];  // {name, off}
  const wholeDefaults = [];// {off, keys, left, op} — a `|| { … }` substituting an entire record

  /* --- member expressions -------------------------------------------------------------------------------- */
  const MEMBER = /(\?\.|\.)\s*([A-Za-z_$][\w$]*)/g;
  let m;
  while ((m = MEMBER.exec(struct))) {
    const optional = m[1] === "?.";
    const nameAt = m.index + m[0].length - m[2].length;
    if (struct[m.index - 1] === "." || struct[m.index - 1] === "?") continue;   /* `...spread`, `?.` already taken */
    const recv = receiverBefore(struct, code, m.index);
    const after = sig(struct, m.index + m[0].length, struct.length);
    const nxt = struct.slice(after, after + 3);
    const isWrite = /^=[^=>]/.test(nxt) || /^=$/.test(nxt);
    const isRMW = /^(\+\+|--|\+=|-=|\|\|=|\?\?=|&&=)/.test(nxt);
    /* A MEMBER IMMEDIATELY CALLED IS AN OPERATION, NOT A DATUM. `s.push(x)`, `JSON.stringify(o)` and
       `document.getElementById(i)` are the overwhelming majority of member expressions in any JS corpus, and
       counting them makes the report a listing of the language rather than of the seam. It is sound here
       rather than merely convenient: a record on this seam is JSON TEXT, and JSON carries no functions, so a
       member that is called is by construction not a field of a record that crossed it. */
    if (nxt[0] === "(" || /^\?\.\(/.test(nxt)) continue;
    if (recv === "") continue;   /* a chain bottoming out on a literal — decided, not unreadable */
    if (recv === null) {
      refuse(file, lineOf(src, m.index), "a member read whose receiver is not a normalizable expression — it cannot be anchored to a record", struct.slice(Math.max(0, m.index - 40), m.index + 20));
      continue;
    }
    if (isWrite) { localWrites.push({ name: m[2], off: nameAt }); continue; }
    let form = null;
    if (optional) form = "?.";
    else if (/^\?\./.test(nxt)) form = "?. on the value";
    else if (/^\|\|/.test(nxt)) form = "|| default";
    else if (/^\?\?/.test(nxt)) form = "?? default";
    else if (inSpan(swallow, m.index)) form = "inside a swallowing try/catch";
    localReads.push({ name: m[2], recv, off: nameAt, form });
    if (isRMW) localWrites.push({ name: m[2], off: nameAt });
  }

  /* --- bracketed member expressions with a literal name -------------------------------------------------- */
  const BRACKET = /(\?\.)?\[\s*(["'])((?:[^"'\\]|\\.)*)\2\s*\]/g;
  while ((m = BRACKET.exec(code))) {
    const at = m.index;
    if (struct[at] !== "[" && !(m[1] && struct[at] === "?")) continue;
    const name = m[3];
    if (!/^[A-Za-z_$][\w$]*$/.test(name)) continue;
    const recv = receiverBefore(struct, code, at);
    if (!recv) continue;   /* an index on a literal, or a receiver this cannot normalize */
    const after = sig(struct, at + m[0].length, struct.length);
    const nxt = struct.slice(after, after + 3);
    if (/^=[^=>]/.test(nxt) || /^=$/.test(nxt)) { localWrites.push({ name, off: at }); continue; }
    let form = null;
    if (m[1]) form = "?.[]";
    else if (/^\|\|/.test(nxt)) form = "|| default";
    else if (/^\?\?/.test(nxt)) form = "?? default";
    else if (inSpan(swallow, at)) form = "inside a swallowing try/catch";
    localReads.push({ name, recv, off: at, form });
  }

  /* --- braces: object literal (writes), destructuring pattern (reads), or a block ------------------------- */
  for (let i = 0; i < struct.length; i++) {
    if (struct[i] !== "{") continue;
    const close = matchAt(struct, i);
    if (close < 0) continue;
    let p = i - 1;
    while (p >= 0 && /\s/.test(struct[p])) p--;
    let prevWord = "";
    if (p >= 0 && /[\w$]/.test(struct[p])) { let q = p; while (q >= 0 && /[\w$]/.test(struct[q])) q--; prevWord = code.slice(q + 1, p + 1); }
    const prevCh = p >= 0 ? struct[p] : "";
    const afterOff = sig(struct, close, struct.length);
    const afterTxt = struct.slice(afterOff, afterOff + 3);
    const assignedTo = /^=[^=>]/.test(afterTxt) || /^=\s*$/.test(afterTxt);

    let kind = null;
    if (["const", "let", "var"].includes(prevWord)) kind = "pattern";
    else if (assignedTo) kind = "pattern";
    else if (prevWord && !["return", "typeof", "of", "in", "do", "else", "await", "yield", "case"].includes(prevWord)) kind = "block";
    /* AN ARROW'S `=> {` IS ITS BODY, NOT A RECORD. Reading it as an object literal made every statement in
       every arrow function an entry with an unreadable key — two hundred and sixty-two refusals that were the
       language, not the corpus. The literal form is `=> ({…})`, whose preceding character is `(`. */
    else if (prevCh === ">" && struct[p - 1] === "=") kind = "block";
    /* A `case "x": {` and a `default: {` end in the same colon a ternary's else-branch does, and reading
       those blocks as object literals made every statement in every switch arm an entry with an unreadable
       key. The label is what tells them apart. */
    else if (prevCh === ":" && /\b(case|default)\b[^;{}]*$/.test(struct.slice(Math.max(0, p - 120), p))) kind = "block";
    else if ("=(,:[?|&".includes(prevCh) || prevWord === "return") kind = "literal";
    else if (");}{".includes(prevCh) || prevCh === "") kind = "block";
    else kind = null;

    if (kind === "block") { continue; }
    const entries = splitTop(struct, i + 1, close - 1);
    const litKeys = [];
    if (kind === null) {
      /* Only braces that actually carry key-looking entries are worth refusing over; a `{` this cannot place
         whose contents are statements is a block by any reading, and refusing over it would report the
         language rather than the corpus. */
      const looksRecord = entries.some(([a, b]) => /^\s*(?:[A-Za-z_$][\w$]*|["'][^"']*["'])\s*:/.test(struct.slice(a, b)));
      if (looksRecord) refuse(file, lineOf(src, i), "a brace this cannot place as an object literal, a destructuring pattern or a block", struct.slice(Math.max(0, i - 40), i + 20));
      continue;
    }

    let recv = null;
    if (kind === "pattern") {
      if (assignedTo) {
        /* To the statement's `;` AT DEPTH ZERO, not to the next newline: `const { methodName } =
           calculateMethodMetadata(u,\n  url)` is one source expression and half of it is a different one. */
        let s = afterOff + 1, e = s, d = 0;
        for (; e < struct.length; e++) {
          const ch = struct[e];
          if (PAIRS[ch]) d++;
          else if (ch === ")" || ch === "]" || ch === "}") { if (!d) break; d--; }
          else if ((ch === ";" || ch === ",") && !d) break;
          else if (ch === "\n" && !d) { let k = e; while (k < struct.length && /\s/.test(struct[k])) k++; if (struct[k] !== "." && struct[k] !== "?") break; }
        }
        recv = code.slice(s, e).replace(/\s+/g, "");
        if (!/^[A-Za-z_$][^\s;]*$/.test(recv)) recv = null;
      }
      if (recv === null) {
        const kw = /^(of|in)\b/.exec(struct.slice(afterOff));
        if (kw) {
          let e2 = afterOff + kw[1].length;
          const stop = matchAt(struct, struct.lastIndexOf("(", i));
          let e3 = e2;
          while (e3 < struct.length && e3 < (stop > 0 ? stop - 1 : struct.length) && struct[e3] !== ";") e3++;
          const t2 = code.slice(e2, e3).replace(/\s+/g, "");
          if (/^[A-Za-z_$][^\s;]*$/.test(t2)) recv = t2;
        }
      }
      if (recv === null) {
        refuse(file, lineOf(src, i), "a destructuring pattern whose source expression is not normalizable — its fields cannot be anchored to a record", struct.slice(i, Math.min(close + 30, struct.length)));
        continue;
      }
    }

    for (const [a, b] of entries) {
      const t = struct.slice(a, b);
      if (!t.trim()) continue;
      if (/^\s*\.\.\./.test(t)) continue;
      /* A method shorthand — `foo() {}`, `async foo()`, `get foo()`, `*foo()` — is a key like any other, and
         reading it as unreadable is what put three hundred and sixty-nine entries in the refusal list that
         were not places anything could hide. */
      const km = /^\s*(?:(?:async|get|set)\s+)?\*?\s*(?:(["'])(.*?)\1|([A-Za-z_$][\w$]*))\s*(:|=|,|\(|$)/.exec(t);
      if (!km) {
        if (/^\s*\d/.test(t)) continue;   /* a numeric key is read, and is not a record field name */
        if (/^\s*\[/.test(t)) refuse(file, lineOf(src, a), `a computed ${kind === "literal" ? "object-literal key" : "destructuring key"} — the field name is not a static fact`, t);
        else if (kind === "literal") refuse(file, lineOf(src, a), "an object-literal entry whose key this cannot read", t);
        continue;
      }
      const name = km[2] !== undefined ? km[2] : km[3];
      if (!/^[A-Za-z_$][\w$]*$/.test(name)) continue;
      if (kind === "literal") { localWrites.push({ name, off: a + t.indexOf(name) }); litKeys.push(name); }
      else localReads.push({ name, recv, off: a + t.indexOf(name), form: km[4] === "=" ? "= default in a destructuring pattern" : null });
    }

    /* THE WHOLE RECORD DEFAULTED, which is the coarsest form of this defect and the one §Architecture opens
       with: `result || {}` turned a run that produced NO document into `{success:true}` with zero endpoints —
       a false clean bill. The literal on the right of a `||`/`??` is the shape being substituted, so the
       substitution is only reported when that shape IS one the engine emits: an `opts || {}` for a caller's
       options object is a default over something no producer was ever supposed to write. */
    if (kind === "literal" && (prevCh === "|" || prevCh === "?") && struct[p - 1] === prevCh) {
      let q = p - 1;
      while (q > 0 && /\s/.test(struct[q - 1])) q--;
      const left = receiverBefore(struct, code, q) || code.slice(Math.max(0, q - 24), q).replace(/\s+/g, " ").trim();
      wholeDefaults.push({ off: i, keys: litKeys.slice(), left, op: prevCh === "|" ? "||" : "??" });
    }
  }

  /* --- markers: a literal naming an @TAG is a reference to the stream contract ----------------------------- */
  /* A DRIVER PRINTS MARKERS TOO, and reading every JS literal as a MATCH made solvergate's own `@EMIT` and
     `@GATEFAIL` — which it writes with console.log and then greps out of a child's stdout — report as markers
     nothing prints. So the same discriminator the C side uses applies here: a literal inside an emission call
     is the marker being PRINTED; everywhere else it is the marker being LOOKED FOR. */
  const emitSpans = [];
  for (const fn of ["console.log", "console.error", "console.warn", "console.info", "process.stdout.write", "process.stderr.write"])
    for (const c of callSites(struct, fn.split(".").pop())) {
      const qual = fn.slice(0, fn.lastIndexOf(".")).replace(".", "\\s*\\.\\s*");
      if (!new RegExp(`(^|[^\\w$.])${qual}\\s*\\.\\s*$`).test(struct.slice(Math.max(0, c.at - 32), c.at))) continue;
      if (c.close > 0) emitSpans.push([c.open, c.close]);
    }
  const emitting = (off) => emitSpans.some(([a, b]) => off >= a && off < b);

  const STRLIT = /(["'`])((?:[^\\]|\\.)*?)\1/g;
  while ((m = STRLIT.exec(code))) {
    if (struct[m.index] !== m[1]) continue;
    for (const t of markersIn(m[2]))
      (emitting(m.index) ? rec(markers, t).writes : rec(markers, t).reads).push(site(m.index));
  }
  /* A regex literal matching a marker is the same reference — `/^@RESUMED (\d+)$/` is how solvergate reads
     one. It is found in `code` at an offset the mask blanked, which is exactly how a regex is told from a
     division here: the masker already decided, and this only reads what it decided. */
  const RXLIT = /\/(?:[^/\\\n[]|\\.|\[(?:[^\]\\]|\\.)*\])+\/[a-z]*/g;
  while ((m = RXLIT.exec(code))) {
    if (!/^\s*$/.test(struct.slice(m.index + 1, m.index + m[0].length - 1).replace(/[a-z]+$/, ""))) continue;
    for (const t of markersIn(m[0])) rec(markers, t).reads.push(site(m.index));
  }

  return { file, src, localReads, localWrites, wholeDefaults, site };
}

/* ---- the run --------------------------------------------------------------------------------------------- */

const files = corpus();
const jsScans = [];
for (const f of files) {
  let src;
  try { src = readFileSync(f.path, "utf8"); } catch { continue; }
  const rel = relative(ROOT, f.path);
  if (f.lang === "c") scanC(rel, src);
  else { const s = scanJS(rel, src); if (s) jsScans.push({ ...s, area: f.area }); }
}

/* An unresolvable emission format is a hiding place for a field only in a file that emits fields. Applied
   after the whole C corpus is read, because the file's own answer is what decides it. */
const emittingFiles = new Set();
for (const [, e] of fields) for (const w of e.writes) if (w.file.endsWith(".c") || w.file.endsWith(".h")) emittingFiles.add(w.file);
for (const u of pendingUnresolved)
  if (emittingFiles.has(u.file))
    refuse(u.file, u.line, `a ${u.fn}( in a seam-emitting file whose format argument is not a literal — a field name may be hiding in it`, u.text);

/* The JS writes go in first: a name written by a producer ANYWHERE — C emission or JS record construction —
   is a name the corpus produces, and that set is what anchors every read below. */
for (const s of jsScans) for (const w of s.localWrites) rec(fields, w.name).writes.push(s.site(w.off));

/* RECORD RECEIVERS, per file. A receiver is one when some field read on that exact text names a field the
   corpus produces. The reads are then replayed against that set, which is why this is two passes and not one:
   `f.cspBlocked` can only be judged after `f.sink` has said what `f` is. */
/* A `|| { … }` is only a defaulted RECORD where the shape it substitutes is one the engine emits — two of its
   keys, the same test a read receiver has to pass, so a caller's `opts || {}` is not reported and
   `result || { fetchCallSites: [], securitySinks: [], pageErrors: [], _park: [] }` is. */
const wholeDefaulted = [];
for (const s of jsScans)
  for (const w of s.wholeDefaults) {
    let bestN = 0, best = null;
    for (const [id, shp] of shapes) { let h = 0; for (const k of w.keys) if (shp.has(k)) h++; if (h > bestN) { bestN = h; best = id; } }
    if (bestN >= 2) wholeDefaulted.push({ ...s.site(w.off), left: w.left, op: w.op, shape: best, keys: w.keys });
  }

const ambiguous = [];   // a receiver that reads exactly ONE field of some emitted shape — undecidable, counted
for (const s of jsScans) {
  const byRecv = new Map();
  for (const r of s.localReads) {
    if (!byRecv.has(r.recv)) byRecv.set(r.recv, []);
    byRecv.get(r.recv).push(r);
  }
  for (const [recv, rs] of byRecv) {
    const names = new Set(rs.map((r) => r.name));
    let best = null, bestN = 0;
    for (const [id, shp] of shapes) {
      let hit = 0;
      for (const n of names) if (shp.has(n)) hit++;
      if (hit > bestN) { bestN = hit; best = id; }
    }
    if (bestN < 2) {
      /* ONE intersecting name is not an anchor and it is not a pass either. Reported only where the receiver
         also reads a name NO producer emits — which is the only configuration in which the undecided question
         could be hiding this defect, and reporting the rest would be reporting the language again. */
      if (bestN === 1 && [...names].some((n) => !(fields.get(n)?.writes.length)))
        ambiguous.push({ ...s.site(rs[0].off), recv, shared: [...names].filter((n) => cEmitted.has(n))[0],
                         unwritten: [...names].filter((n) => !(fields.get(n)?.writes.length)) });
      continue;
    }
    for (const r of rs) {
      const site = { ...s.site(r.off), recv, shape: best };
      rec(fields, r.name).reads.push(site);
      if (r.form) defaulted.push({ ...site, name: r.name, form: r.form });
    }
  }
}

/* ---- verdict --------------------------------------------------------------------------------------------- */

const readNoWriter = [...fields].filter(([, e]) => e.reads.length && !e.writes.length);
/* THE WRITE SIDE IS ASKED OF THE SEAM'S PRODUCER, not of every object literal in the tree. A key a JS module
   writes into its own record and nobody reads is dead JS; a field the ENGINE serializes into the result
   document and nobody reads is a measurement that has never once been looked at, which is the half of this
   defect class that reads zero forever without anyone noticing. */
/* AND A READER IS CREDITED FROM AN UNANCHORED READ TOO, which is the OPPOSITE direction from the category
   above and is deliberate. Anchoring decides whether a read belongs to THIS record; for "does anything read
   this name at all" the anchor is not needed, and demanding it manufactures alarms for every field whose only
   consumer this could not anchor. Over-crediting a reader means this category UNDER-reports — a field read on
   the wrong object is still a field nothing reads — and that residue is exactly what the AMBIGUOUS category
   below carries, rather than being averaged into a number. */
const readAnywhere = new Set();
for (const s of jsScans) for (const r of s.localReads) readAnywhere.add(r.name);
for (const [n, e] of fields) if (e.reads.some((r) => r.file.endsWith(".c") || r.file.endsWith(".h"))) readAnywhere.add(n);
const writeNoReader = [...fields].filter(([n, e]) => cEmitted.has(n) && e.writes.length && !readAnywhere.has(n));
const mReadNoWriter = [...markers].filter(([, e]) => e.reads.length && !e.writes.length);
const mWriteNoReader = [...markers].filter(([, e]) => e.writes.length && !e.reads.length);

const log = (s) => console.log(`[field-gate] ${s}`);
const byArea = new Map();
for (const f of files) byArea.set(f.area, (byArea.get(f.area) || 0) + 1);
log(`corpus — ${[...byArea].map(([a, n]) => `${a} ${n}`).join(", ")}; ` +
    `${fields.size} record field names and ${markers.size} stream markers carry a construct on at least one side`);

const place = (s) => `${s.file}:${s.line}`;
const show = (title, rows, render, cap = 25) => {
  if (!rows.length) return;
  log(`── ${title} ──`);
  for (const r of rows.slice(0, cap)) log(`  ${render(r)}`);
  if (rows.length > cap) log(`  … and ${rows.length - cap} more`);
};

show(`READ with no writer — ${readNoWriter.length} record field name(s) a consumer names off a record and no producer emits`,
     readNoWriter.sort((a, b) => b[1].reads.length - a[1].reads.length),
     ([n, e]) => `${n.padEnd(24)} read at ${e.reads.slice(0, 3).map(place).join(", ")}${e.reads.length > 3 ? ` (+${e.reads.length - 3})` : ""}` +
                 (e.reads[0].recv ? `  on \`${e.reads[0].recv}\`` : "  (matched in C)"));

/* GROUPED BY THE EMISSION, because one emission losing its whole reader is one fact and eighty-two names is a
   list. A record with every field unread is a document nobody opens; a record with ONE field unread is a
   rename that landed on one side, and the two want opposite fixes. */
if (writeNoReader.length) {
  const bySite = new Map();
  for (const [n, e] of writeNoReader) {
    const k = place(e.writes[0]);
    if (!bySite.has(k)) bySite.set(k, []);
    bySite.get(k).push(n);
  }
  log(`── WRITE with no reader — ${writeNoReader.length} record field name(s) a producer emits and nothing reads, ` +
      `from ${bySite.size} emission(s) ──`);
  for (const [k, ns] of [...bySite].sort((a, b) => b[1].length - a[1].length))
    log(`  ${k}  ${ns.length} of the shape: ${ns.sort().join(" ")}`);
}

/* NAMED, not "matched": a marker can be referred to by a diagnostic that tells the next reader to look for it,
   and a marker nothing prints is exactly as stale there as in a grep — the DFAIL failure mode one layer down. */
show(`NAMED marker with no writer — ${mReadNoWriter.length} stream marker(s) a consumer names and no emission prints`,
     mReadNoWriter, ([n, e]) => `${n.padEnd(16)} named at ${e.reads.slice(0, 3).map(place).join(", ")}`);
show(`WRITTEN marker with no reader — ${mWriteNoReader.length} stream marker(s) the engine prints and nothing matches`,
     mWriteNoReader, ([n, e]) => `${n.padEnd(16)} printed at ${e.writes.slice(0, 2).map(place).join(", ")}`);

const dByForm = new Map();
for (const d of defaulted) dByForm.set(d.form, (dByForm.get(d.form) || 0) + 1);
if (defaulted.length) {
  log(`── DEFAULTED reads — ${defaulted.length} read(s) of a record field through a form that survives its absence ──`);
  for (const [form, n] of [...dByForm].sort((a, b) => b[1] - a[1])) log(`  ${String(n).padStart(5)}  ${form}`);
  for (const d of defaulted.slice(0, 20)) log(`    ${place(d)}  ${d.recv}.${d.name}  (${d.form})`);
  if (defaulted.length > 20) log(`    … and ${defaulted.length - 20} more`);
}

if (wholeDefaulted.length) {
  log(`── DEFAULTED WHOLE RECORDS — ${wholeDefaulted.length} place(s) substituting an entire emitted record shape ──`);
  for (const d of wholeDefaulted)
    log(`  ${place(d)}  ${d.left} ${d.op} { ${d.keys.slice(0, 4).join(", ")}${d.keys.length > 4 ? ", …" : ""} }   [${d.shape}]`);
}

if (ambiguous.length) {
  log(`── AMBIGUOUS ANCHOR — ${ambiguous.length} receiver(s) reading exactly ONE field of an emitted record beside ` +
      `name(s) no producer writes. Whether the object is that record is not decidable here, so neither is whether ` +
      `those names are the defect; this is the report on itself, not a pass ──`);
  for (const a of ambiguous.slice(0, 15))
    log(`  ${place(a)}  \`${a.recv}\` shares \`${a.shared}\`, also reads ${a.unwritten.slice(0, 4).map((n) => `\`${n}\``).join(", ")}${a.unwritten.length > 4 ? " …" : ""}`);
  if (ambiguous.length > 15) log(`  … and ${ambiguous.length - 15} more`);
}

/* PER AREA AS WELL AS IN TOTAL, for §Testing's reason: one number in which the widest surface answers most of
   the count makes every other component invisible. */
{
  const areaOf = new Map(files.map((f) => [relative(ROOT, f.path), f.area]));
  const tally = new Map();
  const bump = (f, k) => { const a = areaOf.get(f) || "?"; if (!tally.has(a)) tally.set(a, { rnw: 0, wnr: 0, def: 0, amb: 0, ref: 0 }); tally.get(a)[k]++; };
  for (const [, e] of readNoWriter) for (const r of e.reads) bump(r.file, "rnw");
  for (const [, e] of writeNoReader) for (const w of e.writes) bump(w.file, "wnr");
  for (const d of defaulted) bump(d.file, "def");
  for (const a of ambiguous) bump(a.file, "amb");
  for (const r of refusals) bump(r.file, "ref");
  log("── per area ──");
  const w = Math.max(...[...tally.keys()].map((k) => k.length));
  for (const [a, t] of [...tally].sort((x, y) => (y[1].rnw + y[1].wnr + y[1].def) - (x[1].rnw + x[1].wnr + x[1].def)))
    log(`  ${a.padEnd(w)}  read-no-writer ${String(t.rnw).padStart(4)}   write-no-reader ${String(t.wnr).padStart(4)}` +
        `   defaulted ${String(t.def).padStart(4)}   ambiguous ${String(t.amb).padStart(3)}   refused ${String(t.ref).padStart(4)}`);
}

const rByReason = new Map();
for (const r of refusals) rByReason.set(r.reason, (rByReason.get(r.reason) || 0) + 1);
if (refusals.length) {
  log(`── REFUSED — ${refusals.length} construct(s) this scan cannot read. Zero is the armed state: each one is a ` +
      `place a field name could be hiding, and a scan that guessed past it would report a plausible answer ──`);
  for (const [reason, n] of [...rByReason].sort((a, b) => b[1] - a[1])) {
    log(`  ${String(n).padStart(5)}  ${reason}`);
    for (const r of refusals.filter((x) => x.reason === reason).slice(0, 4)) log(`         ${place(r)}  ${r.text}`);
  }
}

log("── verdict ──");
const cats = [
  ["record field names READ with no writer", readNoWriter.length],
  ["record field names WRITTEN with no reader", writeNoReader.length],
  ["stream markers NAMED with no writer", mReadNoWriter.length],
  ["stream markers WRITTEN with no reader", mWriteNoReader.length],
  ["reads of a record field DEFAULTED rather than asserted", defaulted.length],
  ["whole emitted records DEFAULTED away by a substitute literal", wholeDefaulted.length],
  ["receivers whose record identity is AMBIGUOUS — unanswerable, so unaudited", ambiguous.length],
  ["constructs REFUSED — unreadable, so unaudited", refusals.length],
].filter(([, n]) => n);
if (!cats.length) {
  log("  PASS — every record field a consumer names has a producer, every field a producer emits has a reader, " +
      "no consumer defaults a producer's field, and every construct resolved.");
  process.exit(0);
}
for (const [k, n] of cats) log(`  ${String(n).padStart(5)}  ${k}`);
console.error(`[field-gate] FAILED — ${cats.length} category(ies) above. A read with no writer is a wrong number ` +
              `already being reported; a write with no reader is a measurement nobody sees; a defaulted read is ` +
              `what stops either from being a crash. Fix at the ROOT: make the consumer DCHECK the field ` +
              `(extension/check.js mirrors check.h), or delete the half of the contract that has gone stale.`);
process.exit(1);

/* WHAT IS NOT WIRED, AND WHERE IT GOES. This runs standalone (`node engine/fieldgate.mjs`) and belongs as a
 * STAGE of engine/build.mjs beside the IDL audit, which is where a gate stops being something a person
 * remembers to run. Two things follow from that placement and neither is optional: the stage must run from the
 * FROZEN SNAPSHOT the build already measures (a scan of a shared working tree measures a program no revision
 * contains — the loaded-machine defect with a reader instead of a compiler), and its exit code must join the
 * build's verdict rather than be printed beside it.
 * THE THIRD NAMESPACE IS THE qjs_* ABI: `QJS_EXPORT` names one side, engine/build.mjs's QJS_ABI table and
 * extension/mojom.js's interface records name the other, and an export with no counterpart is the shape
 * `qjs_result`-with-no-caller had. It is not half-resolved here on purpose — a namespace read from one side
 * only would report every export as unreferenced, which is a plausible answer and therefore the worst kind.
 */
