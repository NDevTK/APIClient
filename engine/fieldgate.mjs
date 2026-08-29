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
 *   OUTSIDE THE RETURN DOMAIN
 *                    the same defect where the plausible datum is a VALUE and not a name: a host branching on
 *                    a code its producer cannot answer. Every identifier on both sides is spelled correctly,
 *                    so the three categories above see nothing — see §the RETURN-DOMAIN namespace below.
 *                    Beside it, the value a producer DOES answer that an exhaustive construct omits, and the
 *                    producer that never declared what it can answer at all.
 *   DECIDED PLATFORM a receiver whose OWN DECLARATION names a Web IDL type — `new URL(x)`, `await fetch(u)`,
 *                    an unshadowed `document`, the `Event` a callback the platform calls is handed. The
 *                    platform is its producer and idlgen.mjs is its auditor, so it is out of THIS gate's
 *                    subject. Printed in full with the interface that decided it: a decided negative nobody
 *                    can see is the concealment this file exists to report, performed on its own output.
 *   OFF-INTERFACE    that same object, read for a name the spec does not declare on it AND that no producer in
 *                    the corpus writes either. The interface's member list is the whole member list, so this
 *                    is READ-NO-WRITER landing on a platform object — the one defect the decided negative
 *                    could otherwise have swallowed.
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
 *   subject and would answer its question worse. THAT LINE IS NOW ENFORCED IN BOTH DIRECTIONS: this gate had
 *   no way to know what the platform PROVIDES, so a read of a browser object's own member arrived looking like
 *   a read of a record nobody writes, and the same Web IDL that keeps those installs out is what keeps those
 *   reads out — see §the PLATFORM receiver.
 *   AND THE qjs_* ABI, which is the same seam one layer down and is audited as its own namespace below: an
 *   entry is described by its `QJS_EXPORT` body, by the build's export list, by the renderer document's
 *   binding table and by the mojom method record, and until those four were read together nothing asked
 *   whether an exported entry had a caller at all. This paragraph used to say the ABI was a namespace this
 *   file would not read; it is read now, and the sentence goes with the absence it described.
 *
 * THE CORPUS IS A RULE, NOT A LIST, AND IT IS PRINTED. Producer and consumer both: engine/host/** (the C that
 * emits and reads back), engine/*.mjs (the drivers), extension/**\/*.js minus the generated wasm glue, the
 * inline `<script>` of every extension DOCUMENT (the renderer's ABI binding table is four hundred lines of
 * this seam that no namespace saw while the corpus rule was spelled `.js`), and the
 * top level of testing/ minus its one-off `debug-*` probes. Fixture pages under testing/fixtures and
 * testing/corpus are the SUBJECT of a run, not a party to the seam: crediting a fixture's own object literal
 * as the producer of an engine field name is the false COMPLETE idl_installed.mjs was rewritten to remove.
 */
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, extname, relative, basename, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { gateRevision, revisionLines, revisionMoved } from "./gate_revision.mjs";
import { loadIdl, windowGlobals } from "./idl_members.mjs";

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
/* AND THE WALK ANSWERS ITS OWN CONE, because the two must be one list. The cone is the set of paths this
   gate's number is a FACT ABOUT — what gate_revision asks git whether the checkout still agrees with — and a
   second hand-written copy of it is the same defect as a second copy of a build's source list: it is right on
   the day it is written and silently wrong on the day a root moves. So every walk below pushes the pathspec it
   is a walk of, and there is nothing to keep in step.
   THE CONE IS WIDER THAN THE WALK BY EXACTLY THIS FILE, and that is right rather than an oversight: the
   instrument is not a party to the seam it MEASURES (see below), but its own bytes decide every answer it
   gives, so a modified `fieldgate.mjs` is precisely a run whose number no revision describes. */
const cone = [];
function corpus() {
  const files = [];
  cone.push("engine/host");
  for (const p of walk(join(ROOT, "engine", "host")))
    if (extname(p) === ".c" || extname(p) === ".h") files.push({ path: p, lang: "c", area: "engine/host" });
  /* …minus this file. An instrument is not a party to the seam it measures, and including it is not neutral:
     the report's OWN vocabulary — `file`, `line`, `name`, `shape`, `keys` — becomes a producer, and every one
     of those names is then immune to the read-with-no-writer diff. Reconstructing `canVerify`'s missing
     `shape` field found exactly that: the defect went unreported because this file writes `shape:` in its own
     result rows. */
  cone.push(":(glob)engine/*.mjs");
  for (const p of walk(join(ROOT, "engine")))
    if (extname(p) === ".mjs" && dirname(p) === join(ROOT, "engine") && p !== fileURLToPath(import.meta.url))
      files.push({ path: p, lang: "js", area: "engine drivers" });
  cone.push("extension");
  for (const p of walk(join(ROOT, "extension")))
    if (extname(p) === ".js" && !p.includes(join("lib", "qjs")))
      files.push({ path: p, lang: "js", area: "extension" });
  /* AND THE SHIPPED JAVASCRIPT THAT LIVES IN A DOCUMENT, which a corpus rule spelled `.js` cannot see. This
     is §Testing's uncollected-test defect in this gate's own corpus: `renderer.html` carries the ONE table
     binding each mojo method to its `qjs_*` entry, its return type and the field its reply answers under —
     four hundred lines of the seam this file exists to audit — and it was in no namespace at all because of
     its extension. A file is in for WHERE it is and WHAT it is; an inline module script in the extension is
     extension JavaScript however the document around it is named. */
  for (const p of walk(join(ROOT, "extension")))
    if (extname(p) === ".html") files.push({ path: p, lang: "html", area: "extension" });
  cone.push(":(glob)testing/*.js");
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

/* A DOCUMENT'S INLINE SCRIPTS AS ONE JS VIEW AT IDENTICAL OFFSETS — everything outside a `<script>` body
   blanked, newlines kept, so a line number reported against this view is the line number in the document. A
   `src=`-carrying `<script>` has no body to take and its empty span contributes nothing, so no case needs
   telling apart. HTML §13.2.5.4 Script data state ends the body at the first `</script`, which is what the
   parser does and therefore what a reader must do; a script containing that text in a string is a script the
   browser also cuts short, so the two agree by construction. */
/* A `<script>` INSIDE A COMMENT IS NOT A SCRIPT, and reading one as the start of a body swallowed thirty-five
   lines of English prose into the JavaScript view and reported the file as untokenizable — which is the
   plausible-wrong-answer failure this file is built against, arriving through the one construct nobody thinks
   of. HTML §13.2.5.43 Comment start state runs a comment to its `-->`, and a start tag inside one is text. */
function withoutHtmlComments(src) {
  const arr = src.split("");
  const RE = /<!--/g;
  let m;
  while ((m = RE.exec(src))) {
    const end = src.indexOf("-->", m.index + 4);
    const to = end < 0 ? src.length : end + 3;
    blank(arr, m.index, to);
    RE.lastIndex = to;
  }
  return arr.join("");
}

function htmlScriptView(raw) {
  const src = withoutHtmlComments(raw);
  const out = src.split("");
  blank(out, 0, src.length);
  const OPEN = /<script\b[^>]*>/gi;
  let m, any = false;
  while ((m = OPEN.exec(src))) {
    const from = m.index + m[0].length;
    const end = src.toLowerCase().indexOf("</script", from);
    const to = end < 0 ? src.length : end;
    for (let i = from; i < to; i++) out[i] = src[i];
    any = any || to > from;
    OPEN.lastIndex = to;
  }
  return any ? out.join("") : null;
}

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

/* THE KEYWORDS AFTER WHICH A BRACE IS A VALUE, not a body. `return { … }` was the only one read as a literal,
   so `yield { lo, hi, value }` — a generator handing back a record, which is the same construct with a
   different verb — was a brace this could not place. One list, used by both arms of the classifier, is why the
   two can no longer disagree: a word in it is never a block, and a word outside it never a literal. `do` and
   `else` are deliberately absent — their brace IS a body. */
const VALUE_POSITION = new Set(["return", "yield", "await", "typeof", "case", "in", "of", "void", "delete"]);

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
/* THE TOKEN RUNS TO THE END OF THE MARKER, HYPHENS INCLUDED, and stopping at the first `-` is not a smaller
   answer — it is a DIFFERENT NAME, which then has a writer and a reader that are two spellings of nothing.
   A marker whose two sides are `@BUDGET-NOT-INSTALLED` reads on both sides as `@BUDGET`, and a contract whose
   halves agree perfectly is reported as a half nobody writes: this file's own defect class, manufactured by
   its own tokenizer, and the loudest possible reminder that a scan reports the name it READ and not the name
   that is there. A hyphenated segment is part of the marker exactly as an underscored one is. */
const MARKER_ANY = /(^|[^A-Za-z0-9_@])(@[A-Z][A-Z0-9_]*(?:-[A-Z0-9_]+)*)/g;
const markersIn = (text) => {
  const out = [];
  let m;
  MARKER_ANY.lastIndex = 0;
  while ((m = MARKER_ANY.exec(text))) out.push({ name: m[2], at: m.index + m[1].length });
  return out;
};

/* `echo` IS THE SHELL'S EMISSION VERB, exactly as `console.log` is JavaScript's, and a string this corpus
   hands to `/bin/sh -c` is a PROGRAM rather than a datum. A marker printed by that program is written, and
   reading it as a reference instead put the printing half and the matching half of one contract on the same
   side — so the marker had two readers, no writer, and a category fired on a seam that is whole. The
   construct is the verb immediately in front of the marker inside the literal, with the shell quote that
   belongs to the shell rather than to JavaScript. */
const SHELL_ECHO = /\becho\s+(?:-[neE]+\s+)*(?:\\?["'])?$/;

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

/* A CONDITIONAL BETWEEN TWO LITERAL FORMATS RESOLVES TO BOTH, and refusing it hid field names that are as
   static as any other. `cond ? "\"minimum\":" : "\"exclusiveMinimum\":"` is a producer that emits one key or
   the other and never anything else, so both are declared — while a format that is a VARIABLE stays a refusal,
   because resolving that one means following an assignment and this file follows nothing. The arms are taken
   at depth zero so a `[j]` subscript or a nested `?:` inside an arm does not split the wrong operator. */
function cTernaryArms(code, struct, from, to) {
  let d = 0, q = -1;
  for (let i = from; i < to; i++) {
    const c = struct[i];
    if (PAIRS[c]) d++;
    else if (c === ")" || c === "]" || c === "}") d--;
    else if (!d && c === "?") { q = i; break; }
  }
  if (q < 0) return null;
  let d2 = 0, nested = 0, colon = -1;
  for (let i = q + 1; i < to; i++) {
    const c = struct[i];
    if (PAIRS[c]) d2++;
    else if (c === ")" || c === "]" || c === "}") d2--;
    else if (d2) continue;
    else if (c === "?") nested++;
    else if (c === ":") { if (!nested) { colon = i; break; } nested--; }
  }
  if (colon < 0) return null;
  const a = cLiteral(code, struct, q + 1, colon);
  const b = cLiteral(code, struct, colon + 1, to);
  return a !== null && b !== null ? [a, b] : null;
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
  /* THE BUFFERS THIS SCAN HAS ALREADY READ. `snprintf(t, sizeof t, ",\"searched\":%d", n)` then
     `json_buf_puts(&b, t)` is ONE emission written in two statements, and refusing over the second was this
     file counting its own resolved answer as a hole — the largest single group in the refusal list was a
     destination whose format had been read three lines above it. A `json_buf_puts` of a plain identifier that
     is a resolved build destination in this file therefore carries text already accounted for, which is a
     DECIDED negative on exactly the rule the build side already uses: nothing but the destination's other use
     in the file tells these apart, and that use is a construct rather than a dataflow. */
  const builtDests = new Set();
  for (const [fn, fmtIdx] of Object.entries(C_BUILD))
    for (const c of callSites(struct, fn)) {
      if (!c.args || !c.args[fmtIdx] || !c.args[0]) continue;
      const f = c.args[fmtIdx];
      if (cLiteral(code, struct, f[0], f[1]) === null && !cTernaryArms(code, struct, f[0], f[1])) continue;
      const dst = code.slice(c.args[0][0], c.args[0][1]).trim();
      if (/^[A-Za-z_]\w*$/.test(dst)) builtDests.add(dst);
    }
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
          if (lit) for (const t of markersIn(lit)) { matched.add(t.name); rec(markers, t.name).reads.push(site(a)); }
        }
      }
    const LIT = /"(?:[^"\\\n]|\\.)*"/g;
    let m;
    while ((m = LIT.exec(code))) {
      if (struct[m.index] !== '"') continue;
      for (const t of markersIn(m[0])) if (!matched.has(t.name)) rec(markers, t.name).writes.push(site(m.index));
    }
  }

  for (const [fn, fmtIdx] of Object.entries(C_EMIT))
    for (const c of callSites(struct, fn)) {
      if (!c.args) { refuse(file, lineOf(src, c.at), `an unbalanced ${fn}( — the emission's format argument cannot be delimited`); continue; }
      const a = c.args[fmtIdx];
      if (!a) { refuse(file, lineOf(src, c.at), `a ${fn}( with too few arguments to hold a format`); continue; }
      const lit = cLiteral(code, struct, a[0], a[1]);
      if (lit === null) {
        const arms = cTernaryArms(code, struct, a[0], a[1]);
        if (arms) { for (const t of arms) emit(t, a[0], true); continue; }
        if (builtDests.has(code.slice(a[0], a[1]).trim())) continue;
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
      let lit = cLiteral(code, struct, a[0], a[1]);
      if (lit === null) {
        const arms = cTernaryArms(code, struct, a[0], a[1]);
        if (!arms) { pendingUnresolved.push({ file, line: lineOf(src, c.at), fn, text: code.slice(a[0], Math.min(a[1], a[0] + 60)) }); continue; }
        lit = arms.join("");
      }
      if (!keysIn(lit).length) continue;
      const dst = code.slice(d[0], d[1]).trim();
      if (!/^[A-Za-z_]\w*$/.test(dst)) {
        refuse(file, lineOf(src, c.at), `a ${fn}( carrying a JSON key into a destination that is not a plain identifier — whether this text is emitted or matched is not decidable here`, dst);
        continue;
      }
      emit(lit, a[0], !patterns.has(dst));
    }

  /* The read side of the seam in C: a JSON key literal handed to a matcher. HELD, not credited here, because
     it must ANCHOR the same way a JS read does and the producer set is not complete until the corpus is. */
  for (const fn of C_MATCH)
    for (const c of callSites(struct, fn)) {
      if (!c.args) continue;
      for (const [a, b] of c.args) {
        const lit = cLiteral(code, struct, a, b);
        if (!lit) continue;
        const ks = keysIn(lit);
        if (ks.length) cMatched.push({ file, line: lineOf(src, a), keys: ks });
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

  collectDomainsC(file, src, code, struct, bodies);
  collectAbiC(file, src, code, struct);
}

/* Unresolvable formats are held until the whole C corpus is read, because whether one is a hiding place for a
   field depends on whether its FILE emits fields at all — and that is not known until the file is done. */
const pendingUnresolved = [];

/* THE C READ SIDE ANCHORS BY SHAPE TOO, and until it did it was the one namespace here allowed to name a
 * record out of a single word. `strstr(js, "\"authorization\":\"Bearer …\"")` is a matcher pattern whose ONE
 * key is a HEADER NAME — the engine serializes its headers as a record keyed by the header, `json_buf_str`
 * writing each key from the runtime string — so no static producer emits it and none ever can. Demanding one
 * is the demand-that-cannot-be-met this file already refuses to make of an IDL `magic`, and it fired here as a
 * finding rather than as a question.
 *
 * THE SPELLING IS WHAT MADE IT LOOK LIKE A CONTRACT, which is the whole argument for the shape rule. The same
 * pattern also matches `"x-api-version":` and `"content-type":`, and neither was reported — not because they
 * are different in kind but because a hyphen is not an identifier character. A category whose membership turns
 * on whether a header name happens to be spellable as a JS identifier is reporting the tokenizer.
 *
 * So a matcher literal is a read OF A RECORD when it names TWO of one producer's fields, or when the name it
 * carries is one a producer emits. One unwritten name alone is undecidable — a field of a record nobody writes,
 * or a key of a map whose keys are data — and it goes to AMBIGUOUS with its place, never to a finding. */
const cMatched = [];

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
  const raw = code.slice(i, end).trim();
  const t = raw.replace(/\s+/g, "");
  if (/^[A-Za-z_$]/.test(t)) return /^[^\s;]*$/.test(t) ? t : null;
  /* An ARRAY LITERAL head — `[...m.values()].filter(f).length` — is a computed collection and no record ever
     crossed a seam as one: a decided negative. */
  if (t[0] === "[") return "";
  if (t[0] !== "(") return null;
  /* A `new` HEAD IS DECIDED, not unreadable. `(new Error()).stack` CONSTRUCTS an object in this realm, and a
     record on this seam is JSON text that crossed one — so nothing is hiding here, exactly as nothing is
     hiding behind a string-literal head. */
  if (/^\(\s*new\s/.test(raw)) return "";
  /* A `|| <literal>` HEAD IS DECIDED BY ITS OWN ALTERNATIVE. `(resp.body || "").length` and
     `(m.match(re) || []).length` must accommodate the substituted string or array whatever the left side is,
     so the member read off them is that literal's member and not a field of any record. */
  if (/(\|\||\?\?)(\[\]|\{\}|""|''|``)\)$/.test(t)) return "";
  /* AN AWAITED CALL IS A RECEIVER WHOSE RECORD IDENTITY IS THE CALLED METHOD'S REPLY — the construct this
     file's closing paragraph named as the one thing still outside every namespace here, and it is where the
     shipped bridge lives: `(await eng.r.renderer.step()).code` reads the mojo boundary this gate already has
     BOTH descriptions of. The call NAMES the method and §the qjs_* ABI namespace has the method's own reply
     record, so the diff is construct against construct and nothing is followed through a promise. Only a
     no-argument call is read: an argument list is a span this normalizer would have to resolve to keep the
     receiver's text a fact, and the boundary's own methods take none. */
  return /^\((?:await)?[\w$.]*?\.[A-Za-z_$][\w$]*\(\)\)$/.test(t) ? t : null;
}

/* The method an awaited-call receiver names, or null — read back off the normalized text so the one place
   that decides the shape is the one place that reads it. */
const awaitedMethod = (recv) => {
  const m = /^\((?:await)?[\w$.]*?\.([A-Za-z_$][\w$]*)\(\)\)$/.exec(recv || "");
  return m ? m[1] : null;
};

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

/* ---- receiver scope: TWO BINDINGS OF ONE SPELLING ARE TWO RECEIVERS, NOT ONE ------------------------------- */

/* A RECEIVER IS ANCHORED BY ITS TEXT, AND TEXT IS ONLY A FACT INSIDE ONE SCOPE. Normalizing to source text is
 * what keeps this file off the flowed identity idl_installed.mjs was wrong for — but the text `r` in one
 * function body and the text `r` in another are two facts, and merging them is not conservative, it is the
 * INVENTION this whole file is built against, performed on the anchor itself. It produced exactly the defect
 * class the gate exists to report, in the gate's own output: a driver whose `r` holds a child-process result in
 * one function and its own reply record in another had the two unioned, so the reply record's fields ANCHORED
 * the union and the child-process fields came out as names no producer emits. Two confident false reds, on a
 * seam that is whole, pointing at platform members whose producer is the platform.
 *
 * SCOPE IS A CONSTRUCT AND NOT AN INFERENCE, which is why this is the fix rather than a list of platform names.
 * A `{` whose preceding significant text is a parameter list — or an arrow's `=>` — is a FUNCTION BODY, and the
 * names that body BINDS are its parameters and the declarations directly inside it. A read of `x` belongs to
 * the innermost enclosing body that binds `x`, and to the file when none does; a closure reading an outer name
 * therefore still anchors with its siblings, which a per-block split would have broken.
 *
 * WHICH DIRECTION THIS ERRS IN, STATED. Over-collecting a binder (an identifier inside a parameter default)
 * SPLITS reads that belonged together — the split lands in AMBIGUOUS, which is counted and named. Under-
 * collecting one MERGES two objects, which is the false red above. So every doubtful token is read as a
 * binder: this file's discipline is that the unanswerable question is reported, never resolved toward the
 * answer that produces a finding. */
const NOT_A_FUNCTION_HEAD = new Set(["if", "for", "while", "switch", "catch", "with", "do"]);

function functionScopes(struct, code) {
  const spans = [];
  /* the parameter span of the `(…)` whose `)` sits at `p`, or null */
  const paramsBefore = (p) => {
    let d = 0;
    for (let j = p; j >= 0; j--) {
      if (struct[j] === ")") d++;
      else if (struct[j] === "(") { d--; if (!d) return [j + 1, p]; }
    }
    return null;
  };
  for (let i = 0; i < struct.length; i++) {
    if (struct[i] !== "{") continue;
    let p = i - 1;
    while (p >= 0 && /\s/.test(struct[p])) p--;
    let params = null;
    if (struct[p] === ">" && struct[p - 1] === "=") {
      let q = p - 2;
      while (q >= 0 && /\s/.test(struct[q])) q--;
      if (struct[q] === ")") params = paramsBefore(q);
      else { const e = q + 1; while (q >= 0 && /[\w$]/.test(struct[q])) q--; if (e > q + 1) params = [q + 1, e]; }
    } else if (struct[p] === ")") {
      params = paramsBefore(p);
      if (!params) continue;
      let k = params[0] - 1;
      while (k > 0 && /\s/.test(struct[k - 1])) k--;
      const e = k;
      while (k > 0 && /[\w$]/.test(struct[k - 1])) k--;
      /* `if (…) {` and `for (…) {` carry a parenthesized head and bind nothing a receiver can be spelled as */
      if (NOT_A_FUNCTION_HEAD.has(code.slice(k, e))) continue;
    } else continue;
    const close = matchAt(struct, i);
    if (close < 0) continue;
    spans.push({ open: i, close, params, binds: new Set(), head: params ? params[0] : i });
  }
  /* AN EXPRESSION-BODIED ARROW IS A FUNCTION BODY TOO, and reading only the braced form made this walk answer
     one spelling of a construct and not its twin — the asymmetry that hides rather than errs. `arr.map(e =>
     e.name)` binds `e`, so a walk that does not see the binding calls `e` a FREE name, and a free name is the
     one thing a scope answer must never invent: a parameter spelled `location` or `event` would then be read as
     the global of that name, and every read on it credited to Location or Event. The body runs to the end of
     the expression — the first `,`, `;` or unmatched closer at depth zero — which is exactly what the arrow's
     body is. */
  for (let i = 0; i + 1 < struct.length; i++) {
    if (struct[i] !== "=" || struct[i + 1] !== ">") continue;
    let a2 = sig(struct, i + 2, struct.length);
    if (struct[a2] === "{") continue;                        /* the braced form, already taken above */
    let q = i - 1;
    while (q >= 0 && /\s/.test(struct[q])) q--;
    let params = null;
    if (struct[q] === ")") params = paramsBefore(q);
    else { const e = q + 1; while (q >= 0 && /[\w$]/.test(struct[q])) q--; if (e > q + 1) params = [q + 1, e]; }
    if (!params) continue;
    let e2 = a2, d = 0;
    for (; e2 < struct.length; e2++) {
      const ch = struct[e2];
      if (PAIRS[ch]) d++;
      else if (ch === ")" || ch === "]" || ch === "}") { if (!d) break; d--; }
      else if ((ch === "," || ch === ";") && !d) break;
    }
    spans.push({ open: i, close: e2, params, binds: new Set(), head: params[0] });
  }
  spans.sort((a, b) => a.open - b.open);

  /* THE CALL A FUNCTION LITERAL IS AN ARGUMENT OF, which is what lets Web IDL type a PARAMETER. `document
     .addEventListener("submit", function (e) { … })` states `e`'s type in the spec and nowhere in this file:
     DOM §2.7 declares `addEventListener`'s second argument an `EventListener?`, and that callback interface's
     one operation takes an `Event`. That is a chain of declarations, not a dataflow — nothing is followed
     through an assignment, a return or a promise; the call site IS the construct. The callee text and the
     argument index are all this records; resolving them is the IDL's job, one layer up. */
  for (const s of spans) {
    /* back to where the function LITERAL starts: past its parameter list, its optional name, `function`, `*`
       and `async`. Each step is taken only when the text is exactly that token, so a step not taken leaves the
       walk where it was and the argument test below simply fails. */
    let h = s.head, k = h - 1;
    const ws = () => { while (k >= 0 && /\s/.test(struct[k])) k--; };
    const word = () => { const w = k + 1; let q2 = k; while (q2 >= 0 && /[\w$]/.test(struct[q2])) q2--; return { text: code.slice(q2 + 1, w), at: q2 }; };
    ws();
    /* THE PARAMETER LIST'S OWN PAREN IS PART OF THE LITERAL, and leaving `h` inside it made an arrow its own
       enclosing call: the search below then found `(e)` rather than the `foo(` it sits in, and every arrow
       callback in the corpus fell out of this namespace while `function (e)` stayed in — one construct
       answered, its twin silently not. */
    if (struct[k] === "(") { h = k; k--; ws(); }
    let t2 = word();
    if (t2.text && t2.text !== "function" && t2.text !== "async") { k = t2.at; ws(); t2 = word(); }   /* a named function expression */
    if (t2.text === "function") {
      h = t2.at + 1; k = t2.at; ws();
      if (struct[k] === "*") { k--; ws(); }
      const a2 = word();
      if (a2.text === "async") h = a2.at + 1;
    } else if (t2.text === "async") h = t2.at + 1;                                                   /* an async arrow */
    let j = h - 1;
    while (j >= 0 && /\s/.test(struct[j])) j--;
    if (struct[j] !== "," && struct[j] !== "(") continue;
    let d = 0, open = -1;
    for (let x = j; x >= 0; x--) {
      const c = struct[x];
      if (c === ")" || c === "]" || c === "}") d++;
      else if (c === "(" || c === "[" || c === "{") { if (!d) { if (c !== "(") break; open = x; break; } d--; }
    }
    if (open < 0) continue;
    let e2 = open;
    while (e2 > 0 && /\s/.test(struct[e2 - 1])) e2--;
    if (!/[\w$)\]]/.test(struct[e2 - 1] || "")) continue;      /* not a call: a grouping or a head */
    const closeArgs = matchAt(struct, open);
    if (closeArgs < 0) continue;
    const parts = splitTop(struct, open + 1, closeArgs - 1);
    const index = parts.findIndex(([a, b]) => h >= a && h < b);
    if (index < 0) continue;
    const callee = receiverBefore(struct, code, e2);
    if (!callee) continue;
    s.callee = { text: callee, at: e2, index };
  }

  /* The innermost span containing `off`. Spans are sorted by their opening brace, so the candidates are the
     ones that START before it and the innermost is the LAST of those that also ends after it. */
  const starts = spans.map((s) => s.open);
  const innermost = (off) => {
    let lo = 0, hi = starts.length;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (starts[mid] < off) lo = mid + 1; else hi = mid; }
    for (let k = lo - 1; k >= 0; k--) if (spans[k].close > off) return spans[k];
    return null;
  };

  const ID = /[A-Za-z_$][\w$]*/g;
  for (const s of spans) {
    if (!s.params) continue;
    const t = code.slice(s.params[0], s.params[1]);
    let m;
    ID.lastIndex = 0;
    while ((m = ID.exec(t))) {
      if (t[m.index - 1] === ".") continue;                                  /* a member of a default's operand */
      if (/^\s*\(/.test(t.slice(m.index + m[0].length))) continue;           /* a call in a default value */
      s.binds.add(m[0]);
    }
    /* THE PARAMETERS IN ORDER, which is what an IDL argument list is indexed by. A pattern, a rest and a
       default all occupy a POSITION whatever they bind, so a slot this cannot name is held as null rather than
       skipped — dropping it would shift every parameter after it onto the wrong IDL type. */
    s.paramList = splitTop(struct, s.params[0], s.params[1])
      .map(([a, b]) => { const p2 = /^\s*([A-Za-z_$][\w$]*)\s*(?:=|$)/.exec(code.slice(a, b)); return p2 ? p2[1] : null; });
  }
  /* `const`/`let`/`var` declarators, and the name a `function`/`class` declaration introduces. Attributed to
     the innermost enclosing FUNCTION body rather than to the innermost block: a `const` in a loop body and a
     read of it after the loop are one binding by any reading, and splitting them would cost an anchor for
     nothing. Two same-named declarations inside ONE function body still merge, which is the residue. */
  const DECL = /\b(?:const|let|var)\s+([A-Za-z_$][\w$]*)|\bfunction\s*\*?\s*([A-Za-z_$][\w$]*)|\bclass\s+([A-Za-z_$][\w$]*)|\bcatch\s*\(\s*([A-Za-z_$][\w$]*)/g;
  let d;
  while ((d = DECL.exec(struct))) {
    const name = d[1] || d[2] || d[3] || d[4];
    const s = innermost(d.index);
    if (s) s.binds.add(name);
  }
  /* A destructuring declarator binds every name in its pattern — `const { ok, out } = f()` binds both, and
     missing them merges those receivers with any same-named binding elsewhere in the file. */
  const PAT = /\b(?:const|let|var)\s*(?=[[{])/g;
  while ((d = PAT.exec(struct))) {
    const open = sig(struct, d.index + d[0].length, struct.length);
    const close = matchAt(struct, open);
    if (close < 0) continue;
    const s = innermost(d.index);
    if (!s) continue;
    const t = code.slice(open + 1, close - 1);
    let m;
    ID.lastIndex = 0;
    while ((m = ID.exec(t))) if (t[m.index - 1] !== ".") s.binds.add(m[0]);
  }

  /* A BINDING'S OWN INITIALIZER, which is a DECLARATION and not a dataflow. `const url = new URL(x)` states
     what `url` is in the same construct that creates it, and reading that is the same kind of fact as reading a
     `#define` body — it is not following a value through a call, a parameter or a promise, which is the flowed
     identity this file refuses everywhere. It is recorded only where it is UNAMBIGUOUS: a name declared twice
     in one body, or assigned again after its declaration, has no single initializer and gets none, exactly as
     the return-domain namespace refuses a re-assigned binding. */
  const inits = new Map();     // `${scopeKey}\0${name}` -> initializer text, or null once it is doubtful
  const INIT = /\b(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=(?!=)/g;
  let q;
  while ((q = INIT.exec(struct))) {
    const s = innermost(q.index);
    const key = `${s ? s.open : "file"}\0${q[1]}`;
    if (inits.has(key)) { inits.set(key, null); continue; }   /* two declarations of one name — doubtful */
    let e = q.index + q[0].length, d = 0;
    for (; e < struct.length; e++) {
      const ch = struct[e];
      if (PAIRS[ch]) d++;
      else if (ch === ")" || ch === "]" || ch === "}") { if (!d) break; d--; }
      else if ((ch === ";" || ch === ",") && !d) break;
    }
    const from = q.index + q[0].length;
    const span = s ? [s.open, s.close] : [0, struct.length];
    /* One assignment is the declaration itself; a second is a value this cannot name at the read. */
    if (assignmentsTo(struct, q[1], span[0], span[1]) > 1) { inits.set(key, null); continue; }
    inits.set(key, code.slice(from, e).replace(/\s+/g, " ").trim());
  }

  /* The binder of `name` at `off`: the innermost enclosing body that binds it, or the file when none does. */
  const binderOf = (name, off) => {
    for (let s = innermost(off); s; s = innermost(s.open)) if (s.binds.has(name)) return String(s.open);
    return "file";
  };
  /* The IDL slot `name` occupies at `off`, when it is a PARAMETER of a function literal that is itself an
     argument of a call: the callee's text, that literal's argument index, and the parameter's own position. */
  const paramSlot = (name, off) => {
    for (let s = innermost(off); s; s = innermost(s.open)) {
      if (!s.binds.has(name)) continue;
      if (!s.callee || !s.paramList) return null;
      const at = s.paramList.indexOf(name);
      return at < 0 ? null : { callee: s.callee.text, calleeAt: s.callee.at, arg: s.callee.index, param: at };
    }
    return null;
  };
  return { binderOf, paramSlot, initOf: (name, off) => inits.get(`${binderOf(name, off)}\0${name}`) ?? null };
}

function scanJS(file, src) {
  const masked = maskJS(src);
  if (masked.fault) { refuse(file, lineOf(src, masked.fault.off), "a JS file this masker could not tokenize", masked.fault.why); return null; }
  const { code, struct } = masked;
  const fault = bracketFault(struct);
  if (fault) { refuse(file, lineOf(src, fault.off), "a JS file whose masked brackets do not balance — the mask is wrong somewhere above this and every span answer about the file would be a guess", fault.why); return null; }

  const site = (off) => ({ file, line: lineOf(src, off) });
  const swallow = swallowingTrySpans(struct);
  /* THE KEY A READ ANCHORS UNDER: the receiver's text, qualified by the scope its BASE NAME is bound in. The
     text stays the display, because that is what a reader opens the file to find; the key is what decides
     which reads are reads of one object. A receiver whose base is not a bare identifier (`a().b`) has no
     binder to ask about and keys under its text alone, exactly as before. */
  const { binderOf, initOf, paramSlot } = functionScopes(struct, code);
  const keyOf = (recv, off) => {
    const base = /^[A-Za-z_$][\w$]*/.exec(recv);
    return base ? `${recv}@${binderOf(base[0], off)}` : recv;
  };
  const localReads = [];   // {name, recv, key, off, form}
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
    /* An awaited mojo call has a BETTER anchor than the shape rule — the method's own declared reply — so it
       is routed to that diff instead of into the record namespace, where one field read off one call could
       only ever have been ambiguous. */
    const meth = awaitedMethod(recv);
    if (meth) { mojoReplyReads.push({ file, line: lineOf(src, m.index), method: meth, name: m[2] }); continue; }
    let form = null;
    if (optional) form = "?.";
    else if (/^\?\./.test(nxt)) form = "?. on the value";
    else if (/^\|\|/.test(nxt)) form = "|| default";
    else if (/^\?\?/.test(nxt)) form = "?? default";
    else if (inSpan(swallow, m.index)) form = "inside a swallowing try/catch";
    localReads.push({ name: m[2], recv, key: keyOf(recv, m.index), off: nameAt, form });
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
    localReads.push({ name, recv, key: keyOf(recv, at), off: at, form });
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
    else if (prevWord && !VALUE_POSITION.has(prevWord)) kind = "block";
    /* AN ARROW'S `=> {` IS ITS BODY, NOT A RECORD. Reading it as an object literal made every statement in
       every arrow function an entry with an unreadable key — two hundred and sixty-two refusals that were the
       language, not the corpus. The literal form is `=> ({…})`, whose preceding character is `(`. */
    else if (prevCh === ">" && struct[p - 1] === "=") kind = "block";
    /* A `case "x": {` and a `default: {` end in the same colon a ternary's else-branch does, and reading
       those blocks as object literals made every statement in every switch arm an entry with an unreadable
       key. The label is what tells them apart. */
    else if (prevCh === ":" && /\b(case|default)\b[^;{}]*$/.test(struct.slice(Math.max(0, p - 120), p))) kind = "block";
    else if ("=(,:[?|&".includes(prevCh) || VALUE_POSITION.has(prevWord)) kind = "literal";
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
      else localReads.push({ name, recv, key: keyOf(recv, i), off: a + t.indexOf(name), form: km[4] === "=" ? "= default in a destructuring pattern" : null });
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
    for (const t of markersIn(m[2])) {
      const printed = emitting(m.index) || SHELL_ECHO.test(m[2].slice(0, t.at));
      (printed ? rec(markers, t.name).writes : rec(markers, t.name).reads).push(site(m.index));
    }
  }
  /* A regex literal matching a marker is the same reference — `/^@RESUMED (\d+)$/` is how solvergate reads
     one. It is found in `code` at an offset the mask blanked, which is exactly how a regex is told from a
     division here: the masker already decided, and this only reads what it decided. */
  const RXLIT = /\/(?:[^/\\\n[]|\\.|\[(?:[^\]\\]|\\.)*\])+\/[a-z]*/g;
  while ((m = RXLIT.exec(code))) {
    /* THE FLAGS COME OFF BEFORE THE SLICE, NOT AFTER IT. Slicing to `length - 1` keeps the CLOSING SLASH,
       and a trailing `/` is not a lowercase letter — so stripping flags from that string stripped nothing,
       the interior test saw a non-blank character and every FLAGGED regex in the corpus was silently dropped.
       A marker matched by `/^@TAG (.*)$/m` therefore had no reader this could see, which put two whole and
       working stream contracts in the report as halves nobody reads: the same false red as a truncated marker
       token, from the same place — an off-by-one in the instrument rather than a gap in the tree. */
    const flags = /[a-z]*$/.exec(m[0])[0];
    if (!/^\s*$/.test(struct.slice(m.index + 1, m.index + m[0].length - 1 - flags.length))) continue;
    for (const t of markersIn(m[0])) rec(markers, t.name).reads.push(site(m.index));
  }

  collectDomainsJS(file, src, code, struct);
  collectAbiJS(file, src, code, struct);

  return { file, src, localReads, localWrites, wholeDefaults, site, initOf, binderOf, paramSlot };
}

/* ---- the RETURN-DOMAIN namespace: the one place a plausible datum is a VALUE rather than a NAME ------------ */

/* THE THREE NAMESPACES ABOVE DIFF NAMES, AND ONE HISTORICAL INSTANCE OF THIS DEFECT CARRIES NO NAME AT ALL.
 * `bridge.js` branched on `st === 1` for a NEED_FETCH code while the producer answers DONE(0), YIELD(2) and
 * STALLED(3) — every identifier on both sides was spelled correctly and every field existed. What did not exist
 * was the VALUE, so the branch was unreachable, the only caller of the reply service sat behind it, and the
 * whole `qjs_pending -> safeFetch -> qjs_provide` path had never once run in the shipped extension. A name diff
 * cannot see that, and the same run of it would miss it again.
 *
 * SO THE PRODUCER'S SIDE IS ITS RETURN DOMAIN — the set of integer constants a named function can answer — and
 * the consumer's side is the constants it compares that function's result against. It is read out of the same
 * kind of construct as everything else here, and the DECLARATION is taken in this order:
 *   1. A MEMBERSHIP ASSERTION over the value the function returns: `DCHECK(r == A || r == B || r == C, …)`.
 *      This is the engine's OWN idiom for stating an invariant at its origin (CLAUDE.md §Offensive
 *      programming), so where it exists it IS the producer's declaration and nothing needs to be inferred.
 *   2. Otherwise, every `return` in the body, when each yields a resolvable integer constant — a literal, a
 *      `#define`, or an enumerator (implicit counters included).
 *   3. Otherwise the function has NOT DECLARED what it can return, and that is reported rather than guessed.
 *
 * WHAT LINKS THE TWO SIDES IS A NAME AT A CALL, NEVER A DATAFLOW: the ABI's own `M.ccall("qjs_step", "number",
 * …)`, whose FIRST argument is a string literal naming the C entry and whose SECOND says the answer is a
 * number. The binding must be a `const`/`let`, so the block it lives in IS its scope — a fact rather than an
 * inference — and it must be assigned exactly once; a binding that is re-assigned, or bound by a form this
 * cannot scope, is a REFUSAL with its place. There is deliberately no attempt to follow a value through a
 * parameter or a returned object: that is the flowed identity idl_installed.mjs's second solve was wrong for,
 * and it would be wrong here for the same reason.
 *
 * AND THE SEAM IS WHAT MAKES IT A CONTRACT WITH TWO SIDES, which is why a C caller of a C function is NOT one.
 * The three namespaces above audit the serialized seam for the stated reason — that is where a name has two
 * independent spellings and nothing checks them against each other — and the ABI is the same seam for a value:
 * the codes live in `engine.h`, the host writes them as bare integers, and no compiler sees both. A C `switch`
 * on an internal dispatch index shares one header with its producer and is checked by the compiler that reads
 * them together, so auditing it would report the language's integer dispatch rather than the corpus's contract.
 * It was measured before it was excluded: sixty-eight C callers, of which the largest single group was an IDL
 * member's `magic` — a value whose domain is legitimately different for every interface that dispatches on it,
 * so a demand that its producer declare one is a demand that cannot be met and a red that could never go out.
 *
 * THE BOUNDARY THIS NAMESPACE DOES NOT CROSS, stated rather than left to be discovered. The shipped bridge
 * reaches the same entry through the mojo interface — `(await renderer.step()).code` — and half of that chain
 * is now resolvable: §the qjs_* ABI namespace below reads the renderer document's binding table, so `Step` is
 * known to be `qjs_step` and `code` is known to be the field its answer arrives under. What is still unlinked
 * is the READ: the receiver `(await eng.r.renderer.step())` is not a normalizable expression, and normalizing
 * it would mean following a value out of a promise and through a call, which is the flowed identity this file
 * refuses everywhere else. It is refused with its place, as it should be, and closing it means giving the
 * receiver walk a construct for an awaited call — not giving this one a guess.
 *
 * TWO CONSTANTS, NEVER ONE — the same discipline the shape anchor uses, in the value dimension. Where the
 * producer's domain RESOLVES, the link is by name and every compared constant is checkable however few there
 * are. Where it does NOT, demanding a declaration of every entry a host tests for zero would report the
 * language: a value compared against a single bare constant is a success test, and a value compared against
 * TWO OR MORE distinct constants is being treated as a member of a set. Only the second is a domain nobody
 * declared, and only that is reported.
 *
 * AND EXHAUSTIVENESS IS A CONSTRUCT TOO. `if (x === 0) … else …` handles every other value in its else and
 * claims nothing; a `switch` with no `default`, and a membership assertion, both claim to enumerate. Only those
 * two are read as a claim, so a value the producer can return and the claim omits is reported, and an ordinary
 * `if` is not turned into an obligation it never took on. */

const cConsts = new Map();          // NAME -> {file, line, text}  (#define bodies and enumerators, unresolved)
const cConstConflict = new Set();   // a NAME two translation units give different text — resolving it is a guess
const producers = new Map();        // fn -> {file, line, returns:[{text,line}], memberships:[…], dup:[file:line]}
const consumers = [];               // one binding of one call's result, with every constant compared against it
const jsConsts = new Map();         // file -> Map(NAME -> text)
let domainBindings = 0;             // every ccall result bound to a scopable name, branched on or not

/* An integer constant, or null. Names resolve through the map the caller hands in — the C corpus's own
   `#define`/enumerator table, or a JS file's own `const NAME = <int>` table — with a cycle guard, because a
   name that resolves to itself is not a value and answering one would be the invention this file is against. */
function intOf(text, consts, seen) {
  if (text == null) return null;
  let t = String(text).trim();
  for (;;) {
    if (/^\(.*\)$/.test(t) && matchAt(t, 0) === t.length) { t = t.slice(1, -1).trim(); continue; }
    break;
  }
  let sign = 1;
  while (/^[-+]/.test(t)) { if (t[0] === "-") sign = -sign; t = t.slice(1).trim(); }
  if (/^0[xX][0-9a-fA-F]+[uUlL]*$/.test(t)) return sign * parseInt(t, 16);
  if (/^\d+[uUlL]*$/.test(t)) return sign * parseInt(t, 10);
  if (/^[A-Za-z_$][\w$]*$/.test(t)) {
    const s = seen || new Set();
    if (s.has(t) || cConstConflict.has(t)) return null;
    const c = consts.get(t);
    if (c === undefined) return null;
    s.add(t);
    const v = intOf(typeof c === "string" ? c : c.text, consts, s);
    return v === null ? null : sign * v;
  }
  return null;
}

/* Top-level spans of one logical operator inside [from,to). Depth is counted across all three bracket kinds,
   which is sound here because the caller has already established the region's brackets balance. */
function splitLogical(struct, from, to, op) {
  const parts = [];
  let depth = 0, start = from;
  for (let i = from; i < to; i++) {
    const c = struct[i];
    if (PAIRS[c]) depth++;
    else if (c === ")" || c === "]" || c === "}") depth--;
    else if (!depth && c === op[0] && struct[i + 1] === op[1]) { parts.push([start, i]); start = i + 2; i++; }
  }
  parts.push([start, to]);
  return parts;
}

/* A MEMBERSHIP STATEMENT over ONE identifier, in either of its two spellings: `x == A || x == B` and its
   negation `x != A && x != B`. EVERY disjunct must compare the SAME identifier, or it is not a membership
   statement at all — `r != STALLED || pending` is a conditional about one member and reading it as a domain
   would invent two. */
function membershipOf(struct, code, from, to) {
  for (const [op, cmp] of [["||", /^\s*(?:===|==)\s*/], ["&&", /^\s*(?:!==|!=)\s*/]]) {
    const parts = splitLogical(struct, from, to, op);
    if (parts.length < 2) continue;
    let id = null;
    const consts = [];
    let ok = true;
    for (const [a, b] of parts) {
      const t = code.slice(a, b);
      const lhs = /^\s*([A-Za-z_$][\w$]*)\s*(?=[=!])/.exec(t);
      if (!lhs) { ok = false; break; }
      const rest = t.slice(lhs[0].length);
      const c = cmp.exec(rest);
      if (!c) { ok = false; break; }
      if (id === null) id = lhs[1];
      else if (id !== lhs[1]) { ok = false; break; }
      consts.push(rest.slice(c[0].length).trim());
    }
    if (ok && id && consts.length >= 2) return { id, consts, negated: op === "&&" };
  }
  return null;
}

/* Every comparison of `id` against a single operand token inside [from,to), in both operand orders. The
   OPERATOR must start exactly where the walk stands, so `<=`, `>=` and `=>` are not comparisons of membership
   and a plain `=` is not one either. */
function comparisonsOn(struct, code, id, from, to) {
  const out = [];
  const re = new RegExp(`(^|[^\\w$.])${id}(?![\\w$])`, "g");
  re.lastIndex = from;
  let m;
  const OPS = /^(===|!==|==|!=)/;
  const TOK = /^\s*([A-Za-z_$][\w$]*|0[xX][0-9a-fA-F]+|\d+)/;
  while ((m = re.exec(struct)) && m.index < to) {
    const at = m.index + m[1].length;
    let j = at + id.length;
    while (j < to && /\s/.test(struct[j])) j++;
    const right = OPS.exec(struct.slice(j, j + 3));
    if (right) {
      const t = TOK.exec(code.slice(j + right[0].length, Math.min(to, j + right[0].length + 40)));
      if (t) out.push({ text: t[1], off: at });
    }
    let k = m.index;
    while (k > from && /\s/.test(struct[k - 1])) k--;
    if (k >= from + 2) {
      const two = struct.slice(k - 3, k);
      const back = /(===|!==|==|!=)$/.exec(two) || /(==|!=)$/.exec(struct.slice(k - 2, k));
      if (back) {
        let e = k - back[1].length;
        while (e > from && /\s/.test(struct[e - 1])) e--;
        let s = e;
        while (s > from && /[\w$]/.test(struct[s - 1])) s--;
        const t = code.slice(s, e);
        if (/^([A-Za-z_$][\w$]*|0[xX][0-9a-fA-F]+|\d+)$/.test(t)) out.push({ text: t, off: at });
      }
    }
  }
  return out;
}

/* How many times `id` is ASSIGNED inside [from,to). One is what makes the binding's value the call's; two is a
   binding whose value at a comparison is not decidable here, and that is a refusal rather than a guess. */
function assignmentsTo(struct, id, from, to) {
  let n = 0;
  const re = new RegExp(`(^|[^\\w$.])${id}(?![\\w$])`, "g");
  re.lastIndex = from;
  let m;
  while ((m = re.exec(struct)) && m.index < to) {
    let j = m.index + m[1].length + id.length;
    while (j < to && /\s/.test(struct[j])) j++;
    if (struct[j] === "=" && struct[j + 1] !== "=" && struct[j + 1] !== ">") n++;
    else if (/^(\+\+|--|\+=|-=|\|=|&=|\^=|\*=|\/=|%=)/.test(struct.slice(j, j + 2))) n++;
  }
  return n;
}

/* A `switch (id) {` inside [from,to): its top-level case constants and whether it carries a `default`. A switch
   WITHOUT a default is the one branching construct that claims to enumerate, so it is the only one besides an
   assertion whose omissions are reported. */
function switchOn(struct, code, id, from, to) {
  const re = new RegExp(`\\bswitch\\s*\\(\\s*${id}\\s*\\)\\s*\\{`, "g");
  re.lastIndex = from;
  const m = re.exec(struct);
  if (!m || m.index >= to) return null;
  const open = struct.indexOf("{", m.index);
  const close = matchAt(struct, open);
  if (close < 0) return null;
  const cases = [];
  const CASE = /\bcase\s+([^:]+):/g;
  let c;
  CASE.lastIndex = open;
  while ((c = CASE.exec(struct)) && c.index < close) cases.push(code.slice(c.index + 5, c.index + c[0].length - 1).trim());
  return { cases, hasDefault: /\bdefault\s*:/.test(struct.slice(open, close)), off: m.index };
}

/* One consumer record: everything the diff needs about one binding, resolved later so a constant defined in a
   header a later file carries is not read as unresolvable. */
function pushConsumer(file, src, struct, code, fn, id, from, to, off) {
  const cmps = comparisonsOn(struct, code, id, from, to);
  /* A `case` LABEL IS A COMPARISON, and reading only `==` missed every switch outright — the construct that
     branches on a code without ever writing an operator. */
  const sw = switchOn(struct, code, id, from, to);
  if (sw) for (const c of sw.cases) cmps.push({ text: c, off: sw.off });
  if (!cmps.length) return;                      /* a result nothing branches on states nothing to check */
  let claim = null;
  if (sw && !sw.hasDefault) claim = { kind: "a switch with no default", consts: sw.cases, line: lineOf(src, sw.off) };
  if (!claim)
    for (const a of ["DCHECK", "CHECK", "assert", "DFAIL"])
      for (const cs of callSites(struct, a)) {
        if (!cs.args || cs.at < from || cs.at >= to) continue;
        const mem = membershipOf(struct, code, cs.args[0][0], cs.args[0][1]);
        if (mem && mem.id === id && !mem.negated)
          claim = { kind: `a ${a} membership assertion`, consts: mem.consts, line: lineOf(src, cs.at) };
      }
  consumers.push({ file, fn, id, line: lineOf(src, off),
                   compared: cmps.map((c) => ({ text: c.text, line: lineOf(src, c.off) })), claim });
}

function collectDomainsC(file, src, code, struct, bodies) {
  const line = (off) => lineOf(src, off);

  /* `#define NAME <text>` — object-like only. A function-like macro's name is followed by `(` with no space,
     which this pattern cannot match, and that is deliberate: its body is not a value. */
  const DEF = /(^|\n)[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+([^\n]*)/g;
  let m;
  while ((m = DEF.exec(code))) {
    const name = m[2], text = m[3].trim();
    const prev = cConsts.get(name);
    if (prev && prev.text !== text) cConstConflict.add(name);
    if (!prev) cConsts.set(name, { file, line: line(m.index + m[1].length), text });
  }

  /* Enumerators, implicit counters included — the other half of what §the task calls a `#define` set. */
  const EN = /\benum\b/g;
  while ((m = EN.exec(struct))) {
    let i = m.index + 4;
    while (i < struct.length && /[\s\w]/.test(struct[i])) i++;
    if (struct[i] !== "{") continue;
    const close = matchAt(struct, i);
    if (close < 0) continue;
    /* THE IMPLICIT COUNTER IS ONLY KNOWN WHILE THE EXPLICIT ONES ARE. An enumerator whose `= <expr>` this
       cannot resolve does not merely fail to answer for itself — it makes every enumerator AFTER it a value
       nobody knows, and continuing the count past it would hand the diff a plausible number. So the counter
       goes to null and the rest of the enum stays unresolved, which reads downstream as an OPEN domain and is
       reported as one. */
    let next = 0;
    for (const [a, b] of splitTop(struct, i + 1, close - 1)) {
      const t = code.slice(a, b).trim();
      if (!t) continue;
      const em = /^([A-Za-z_]\w*)\s*(?:=\s*([\s\S]+))?$/.exec(t);
      if (!em) continue;
      const explicit = em[2] !== undefined;
      if (!explicit && next === null) continue;
      const text = explicit ? em[2].trim() : String(next);
      const v = intOf(text, cConsts, null);
      next = v === null ? null : v + 1;
      const prev = cConsts.get(em[1]);
      if (prev && prev.text !== text) cConstConflict.add(em[1]);
      if (!prev) cConsts.set(em[1], { file, line: line(a), text });
    }
    EN.lastIndex = close;
  }

  /* THE ASSERTION SITES, READ ONCE PER FILE AND NOT ONCE PER BODY. Asking `callSites` inside the body loop is
     a scan of the whole file for every function in it, which is quadratic in the file's own size — measured at
     more than twice this gate's total runtime on `engine.c` alone. */
  const asserts = [];
  for (const a of ["DCHECK", "CHECK", "assert"])
    for (const cs of callSites(struct, a)) {
      if (!cs.args) continue;
      const mem = membershipOf(struct, code, cs.args[0][0], cs.args[0][1]);
      if (mem && !mem.negated) asserts.push({ at: cs.at, mem });
    }

  for (const [from, to] of bodies) {
    const fn = cFunctionNameBefore(struct, code, from);
    if (!fn) continue;

    /* THE PRODUCER'S DECLARATION. Both sources are collected here and the priority is applied at resolution,
       because a membership assertion may name a constant a later file defines. */
    const returns = [];
    const RET = /\breturn\b/g;
    RET.lastIndex = from;
    let r;
    while ((r = RET.exec(struct)) && r.index < to) {
      if (/[\w$]/.test(struct[r.index - 1] || "")) continue;
      let e = r.index + 6, d = 0;
      for (; e < to; e++) {
        const ch = struct[e];
        if (PAIRS[ch]) d++;
        else if (ch === ")" || ch === "]" || ch === "}") d--;
        else if (ch === ";" && !d) break;
      }
      returns.push({ text: code.slice(r.index + 6, e).trim(), line: line(r.index) });
    }
    const memberships = asserts.filter((a) => a.at >= from && a.at < to).map((a) => ({ ...a.mem, line: line(a.at) }));
    const prev = producers.get(fn);
    if (prev) prev.dup.push(`${file}:${line(from)}`);
    else producers.set(fn, { file, line: line(from), returns, memberships, dup: [] });
  }
}

/* The identifier immediately before a body's parameter list — a function DEFINITION's name. A brace whose
   preceding significant character is `=` is an initializer and answers null, which is what keeps a file-scope
   table out of the producer set. */
function cFunctionNameBefore(struct, code, braceAt) {
  let i = braceAt - 1;
  while (i >= 0 && /\s/.test(struct[i])) i--;
  if (struct[i] !== ")") return null;
  let depth = 0, j = i;
  for (; j >= 0; j--) {
    if (struct[j] === ")") depth++;
    else if (struct[j] === "(") { depth--; if (!depth) break; }
  }
  if (j < 0) return null;
  let k = j;
  while (k > 0 && /\s/.test(struct[k - 1])) k--;
  const e = k;
  while (k > 0 && /[\w$]/.test(struct[k - 1])) k--;
  const name = code.slice(k, e);
  return /^[A-Za-z_]\w*$/.test(name) ? name : null;
}

/* The innermost brace span containing `off`, which for a `const`/`let` IS its scope — a fact rather than an
   inference, and the reason only those two binding forms are read. */
function innermostBlock(struct, off) {
  const stack = [];
  for (let i = 0; i <= off && i < struct.length; i++) {
    if (struct[i] === "{") stack.push(i);
    else if (struct[i] === "}") stack.pop();
  }
  for (let s = stack.length - 1; s >= 0; s--) {
    const end = matchAt(struct, stack[s]);
    if (end > off) return [stack[s], end];
  }
  return [0, struct.length];
}

function collectDomainsJS(file, src, code, struct) {
  const line = (off) => lineOf(src, off);
  const mine = new Map();
  jsConsts.set(file, mine);
  /* `const A = 0, B = 2, C = 3;` is one declaration and three constants — reading only the first is how a
     domain comes out one member short. */
  const CD = /\bconst\s+/g;
  let m;
  while ((m = CD.exec(struct))) {
    let e = m.index + m[0].length, d = 0;
    for (; e < struct.length; e++) {
      const ch = struct[e];
      if (PAIRS[ch]) d++;
      else if (ch === ")" || ch === "]" || ch === "}") { if (!d) break; d--; }
      else if (ch === ";" && !d) break;
      else if (ch === "\n" && !d && /^\s*$/.test(struct.slice(e, struct.indexOf("\n", e + 1) + 1 || e))) break;
    }
    for (const [a, b] of splitTop(struct, m.index + m[0].length, e)) {
      const t = code.slice(a, b).trim();
      const dm = /^([A-Za-z_$][\w$]*)\s*=\s*([-+]?\s*(?:0[xX][0-9a-fA-F]+|\d+))$/.exec(t);
      if (dm) mine.set(dm[1], dm[2].replace(/\s+/g, ""));
    }
  }

  /* THE ABI CALL IS THE LINK, and its first argument is a string literal naming the C entry.
     THE ORDER OF THE THREE QUESTIONS IS THE WHOLE OF WHAT KEEPS THIS REPORT HONEST, and it was wrong once in
     each direction. A call is only a place a wrong branch can hide if SOMETHING BRANCHES ON ITS ANSWER, so
     "is the answer compared against a constant anywhere in this file" is asked BEFORE anything is refused —
     the renderer's own generic dispatcher (`M.ccall(e.fn, e.ret, …)`, one call standing for the whole table)
     places its answer in a reply record and compares it against nothing, and refusing over it manufactured a
     permanent unreadable-construct that no fix could ever retire. Only once the answer IS branched on does an
     unnameable entry, or a binding whose region this cannot state, become a place a defect can hide — and
     then it is refused with its place rather than searched for in a region that might not be its own. */
  for (const cs of callSites(struct, "ccall")) {
    if (!cs.args || cs.args.length < 2) continue;
    /* THE ANSWER'S TYPE IS THE CALL'S OWN SECOND ARGUMENT: a `'string'` or `'void'` entry has no integer
       domain at all, so it is a DECIDED negative rather than an unreadable construct. */
    const rt = /^(["'])(\w+)\1$/.exec(code.slice(cs.args[1][0], cs.args[1][1]).trim());
    if (rt && rt[2] !== "number") continue;
    const head = code.slice(Math.max(0, cs.at - 160), cs.at);
    const bind = /\b(const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*(?:await\s+)?[\w$.\s]*$/.exec(head) ||
                 /(^|[^\w$.=!<>])()([A-Za-z_$][\w$]*)\s*=\s*(?:await\s+)?[\w$.\s]*$/.exec(head);
    if (!bind) continue;                              /* nothing holds the answer — decided, not unreadable */
    const id = bind[2] || bind[3];
    if (!comparisonsOn(struct, code, id, 0, struct.length).length &&
        !switchOn(struct, code, id, 0, struct.length)) continue;   /* nothing branches on it — decided */
    const raw = code.slice(cs.args[0][0], cs.args[0][1]).trim();
    const lit = /^(["'])([A-Za-z_]\w*)\1$/.exec(raw);
    if (!lit) {
      refuse(file, line(cs.at), "an ABI call answering a number that is BRANCHED ON, whose entry name is not a " +
             "string literal — the producer whose return domain the branch is about cannot be named", raw);
      continue;
    }
    if (bind[1] !== "const" && bind[1] !== "let") {
      refuse(file, line(cs.at), "an ABI result that is branched on and bound by a form this cannot scope — only " +
             "a `const`/`let` declaration states the region its comparisons live in", head.slice(-60));
      continue;
    }
    const [from, to] = innermostBlock(struct, cs.at);
    if (assignmentsTo(struct, id, from, to) !== 1) {
      refuse(file, line(cs.at), "an ABI result whose binding is re-assigned in its own scope — which call a " +
             "comparison is about is not decidable here", id);
      continue;
    }
    domainBindings++;
    pushConsumer(file, src, struct, code, lit[2], id, from, to, cs.at);
  }
}

/* ---- the qjs_* ABI namespace: one entry, four descriptions of it, nothing checking them against each other -- */

/* THE ABI IS DESCRIBED IN FOUR PLACES AND ONLY TWO OF THEM WERE EVER DIFFED. `engine/build.mjs` already checks
 * its `QJS_ABI` export list against `main.c`'s `QJS_EXPORT` bodies in both directions, and that pair is where
 * this stops. What no gate asked is whether anything CALLS an entry — which is exactly the shape `qjs_result`
 * had: defined, exported, listed, and reached by nobody, so the zone that was supposed to read the result
 * document read `result || {}` and reported a clean bill for a run that learned nothing.
 *
 * THE FOUR DESCRIPTIONS, each read out of its own construct:
 *   1. `QJS_EXPORT <ret> qjs_x(` in the entry source — the entry, and the TYPE it answers with.
 *   2. `QJS_ABI = [ … ]` in the build — what is `--export=`'d, so what can be reached at all.
 *   3. `{ <method>: { fn: "qjs_x", ret: …, out: … } }` in the renderer document — the ONE binding of a mojo
 *      method to a C entry, the ccall return type it is read with, and the reply field it answers under. It is
 *      found by the `fn:` key naming a `qjs_`-prefixed literal, never by the table's variable name, so a table
 *      that moves or is renamed is still read.
 *   4. `{ ordinal, name: "Step", reply: [ { name: "code", … } ] }` in the mojom module — the TYPED, VALIDATED
 *      wire: the method the boundary declares and the field name its reply carries.
 * The mojom name and the binding's method name are related by `lowerFirst`, and that is not a convention this
 * assumes — it is the transformation `extension/mojo.js` PERFORMS on every method it installs, so the mapping
 * is read off the shipped code exactly as everything else here is.
 *
 * WHAT THE FOUR DISAGREEING MEANS, one defect per pair: an entry with no binding is a capability the extension
 * cannot use; a binding naming no entry is a call that cannot land; a mojom method with no binding is a
 * declared operation the renderer cannot perform, and a binding with no mojom method is an entry that crosses
 * the boundary without the validator that assumes the renderer is hostile ever seeing it; and an `out:` that
 * is not the reply field the mojom declares is the `cspBlocks`/`cspBlocked` defect ON THE WIRE, where the
 * reader gets `undefined` from a call that succeeded. */

const abiEntries = new Map();   // qjs_x -> {file, line, ret}    from QJS_EXPORT
const abiExported = new Map();  // qjs_x -> {file, line}         from the build's export list
const abiBindings = new Map();  // method -> {fn, ret, out, file, line}
const mojomMethods = new Map(); // "<iface>#Step" -> {file, line, iface, reply:[names], unresolved:[…]}
const servedInterfaces = [];    // {iface, file, line} — the interface a binding table's own document names
const abiCcalls = new Map();    // qjs_x -> [{file,line}]        a driver reaching the entry directly
const mojoReplyReads = [];      // {file,line,method,name}       `(await x.m()).f` — the CALLER'S read of a reply

/* The C side: the marker the entry source puts on every ABI body, with the type in front of the name. */
function collectAbiC(file, src, code, struct) {
  const RE = /\bQJS_EXPORT\s+([\w \t*]+?)\b(qjs_\w+)\s*\(/g;
  let m;
  while ((m = RE.exec(struct))) {
    if (abiEntries.has(m[2])) continue;
    abiEntries.set(m[2], { file, line: lineOf(src, m.index), ret: m[1].replace(/\s+/g, " ").trim() });
  }
}

/* An object-literal value on `key`, inside [from,to), when it is a string literal or `null` — the two forms
   the descriptions above use. Anything else answers undefined and is reported by its caller. */
function litProp(struct, code, key, from, to) {
  const re = new RegExp(`(^|[^\\w$.])${key}\\s*:\\s*`, "g");
  re.lastIndex = from;
  const m = re.exec(struct);
  if (!m || m.index >= to) return undefined;
  const at = m.index + m[0].length;
  const s = /^(["'])((?:[^"'\\]|\\.)*)\1/.exec(code.slice(at, Math.min(to, at + 120)));
  if (s) return s[2];
  if (/^null\b/.test(struct.slice(at, at + 5))) return null;
  return undefined;
}

/* The innermost `{` enclosing `off`, and its close. */
function enclosingBrace(struct, off) {
  const stack = [];
  for (let i = 0; i <= off && i < struct.length; i++) {
    if (struct[i] === "{") stack.push(i);
    else if (struct[i] === "}") stack.pop();
  }
  if (!stack.length) return null;
  const open = stack[stack.length - 1];
  const close = matchAt(struct, open);
  return close < 0 ? null : [open, close];
}

function collectAbiJS(file, src, code, struct) {
  const line = (off) => lineOf(src, off);
  const ifaceSpans = [];

  /* (2) the export list, read as an array of string literals bound to the name the linker is handed. */
  {
    const RE = /\bQJS_ABI\s*=\s*\[/g;
    let m;
    while ((m = RE.exec(struct))) {
      const open = struct.indexOf("[", m.index);
      const close = matchAt(struct, open);
      if (close < 0) { refuse(file, line(m.index), "an unbalanced ABI export list — its entries cannot be delimited"); continue; }
      for (const [a, b] of splitTop(struct, open + 1, close - 1)) {
        const t = code.slice(a, b).trim();
        if (!t) continue;
        const s = /^(["'])([A-Za-z_]\w*)\1$/.exec(t);
        if (!s) { refuse(file, line(a), "an ABI export list entry that is not a string literal — what is exported is not a static fact", t); continue; }
        if (!abiExported.has(s[2])) abiExported.set(s[2], { file, line: line(a) });
      }
    }
  }

  /* (3) the binding table, found by its `fn:` key rather than by the table's own name. */
  {
    /* MATCHED IN `code`, NOT IN `struct`, and this is the one place that distinction bites: the two views
       differ exactly by having string INTERIORS blanked, so a pattern whose payload is inside the quotes finds
       nothing at all in the structural view. It found nothing here, silently, and the diff below then reported
       ten entries as uncalled because the side that calls them had read zero rows — the same "a side answered
       nothing and the report read it as an answer" defect this whole file is against, produced inside it. The
       quote's POSITION is checked in `struct` so a match inside another literal is still not a construct. */
    const RE = /(^|[^\w$.])fn\s*:\s*(["'])(qjs_\w+)\2/g;
    let m;
    while ((m = RE.exec(code))) {
      const at = m.index + m[1].length;
      if (struct[m.index + m[0].length - 1] !== m[2]) continue;   /* the closing quote is real, so this is a literal */
      const span = enclosingBrace(struct, at);
      if (!span) { refuse(file, line(at), "an ABI binding whose enclosing record cannot be delimited", m[3]); continue; }
      const [open, close] = span;
      /* the METHOD is the key this record is the value of */
      let p = open - 1;
      while (p >= 0 && /\s/.test(struct[p])) p--;
      if (struct[p] !== ":") { refuse(file, line(at), "an ABI binding that is not the value of a named method key — the method it binds cannot be read", m[3]); continue; }
      p--;
      while (p >= 0 && /\s/.test(struct[p])) p--;
      let e = p + 1;
      while (p >= 0 && /[\w$]/.test(struct[p])) p--;
      const method = code.slice(p + 1, e);
      if (!/^[A-Za-z_$][\w$]*$/.test(method)) { refuse(file, line(at), "an ABI binding whose method key this cannot read", m[3]); continue; }
      const ret = litProp(struct, code, "ret", open, close);
      const out = litProp(struct, code, "out", open, close);
      if (ret === undefined) { refuse(file, line(at), "an ABI binding with no readable `ret` — the ccall type its answer is read with is not a static fact", method); continue; }
      if (out === undefined) { refuse(file, line(at), "an ABI binding with no readable `out` — the reply field its answer is placed under is not a static fact", method); continue; }
      abiBindings.set(method, { fn: m[3], ret, out, file, line: line(at) });
    }
  }

  /* WHICH INTERFACE A METHOD BELONGS TO, because a method list is per interface and a table binds ONE of them.
     Reading them as one pool reported `GetMojoStats` — a `content.mojom.ChildProcess` method the trusted zone
     implements — as an operation the RENDERER has no binding for, which is a confident false red produced by
     asking one interface's question of another's list. The owner is the interface record whose braces contain
     the method record; the interface a TABLE serves is named by that table's own document, through the call it
     makes to get the declaration, so neither half is inferred. */
  {
    const RE = /(^|[^\w$.])name\s*:\s*(["'])(\w+\.mojom\.\w+)\2/g;
    let m;
    while ((m = RE.exec(code))) {
      if (struct[m.index + m[0].length - 1] !== m[2]) continue;
      const span = enclosingBrace(struct, m.index + m[1].length);
      if (span) ifaceSpans.push({ iface: m[3], open: span[0], close: span[1] });
    }
  }
  /* `interfaceOf`, NOT `exposeInterface`, and the two are different questions rather than two spellings of
     one. `interfaceOf` asks for the DECLARATION a table is implemented against — the renderer walks its
     methods and looks each one up in the binding table — while `exposeInterface` PUBLISHES an implementation,
     and one document publishes several. Reading both made the served interface ambiguous and quietly took the
     whole declared-versus-bound direction out of the run. */
  for (const cs of callSites(struct, "interfaceOf")) {
    if (!cs.args) continue;
    const s2 = /^(["'])(\w+\.mojom\.\w+)\1$/.exec(code.slice(cs.args[0][0], cs.args[0][1]).trim());
    if (s2) servedInterfaces.push({ iface: s2[2], file, line: line(cs.at) });
  }

  /* (4) the mojom method records: an `ordinal` beside a `name` is the construct, and the reply's field names
     are what a consumer will read the answer under. An element of `reply` that is a bare identifier is
     resolved through the file's own `<ID> = { name: … }` declaration and REFUSED when there is none — never
     skipped, because a skipped element is a field name that silently has no counterpart. */
  {
    const RE = /(^|[^\w$.])ordinal\s*:\s*\d+\s*,\s*name\s*:\s*(["'])([A-Za-z_][\w$]*)\2/g;
    let m;
    while ((m = RE.exec(code))) {
      const at = m.index + m[1].length;
      if (struct[m.index + m[0].length - 1] !== m[2]) continue;
      const span = enclosingBrace(struct, at);
      if (!span) { refuse(file, line(at), "a mojom method record that cannot be delimited", m[3]); continue; }
      const [open, close] = span;
      const owner = ifaceSpans.filter((s2) => at > s2.open && at < s2.close)
                              .sort((a, b) => (b.open - a.open))[0];
      if (!owner) { refuse(file, line(at), "a mojom method record inside no interface declaration — which " +
                           "interface declares it cannot be read, and a method list is per interface", m[3]); continue; }
      const rec = { file, line: line(at), iface: owner.iface, name: m[3], reply: [], unresolved: [] };
      const RRE = /(^|[^\w$.])reply\s*:\s*\[/g;
      RRE.lastIndex = open;
      const r = RRE.exec(struct);
      if (r && r.index < close) {
        const ro = struct.indexOf("[", r.index), rc = matchAt(struct, ro);
        if (rc < 0) rec.unresolved.push("an unbalanced reply list");
        else for (const [a, b] of splitTop(struct, ro + 1, rc - 1)) {
          const t = code.slice(a, b).trim();
          if (!t) continue;
          if (t[0] === "{") {
            const n = litProp(struct, code, "name", a, b);
            if (typeof n === "string") rec.reply.push(n);
            else rec.unresolved.push(`a reply field at ${file}:${line(a)} whose name is not a literal`);
            continue;
          }
          if (/^[A-Za-z_$][\w$]*$/.test(t)) {
            /* a shared field record, named once and reused — resolved through its own declaration */
            const DRE = new RegExp(`(^|[^\\w$.])${t}\\s*=\\s*\\{`, "g");
            const d = DRE.exec(struct);
            if (d) {
              const dOpen = struct.indexOf("{", d.index), dClose = matchAt(struct, dOpen);
              const n = dClose > 0 ? litProp(struct, code, "name", dOpen, dClose) : undefined;
              if (typeof n === "string") { rec.reply.push(n); continue; }
            }
            rec.unresolved.push(`a reply element \`${t}\` at ${file}:${line(a)} this cannot resolve to a field record`);
            continue;
          }
          rec.unresolved.push(`a reply element at ${file}:${line(a)} this cannot read`);
        }
      }
      const key = owner.iface + "#" + m[3];
      if (!mojomMethods.has(key)) mojomMethods.set(key, rec);
    }
  }

  /* Every driver that reaches an entry directly, which is a caller the mojo boundary never sees. */
  for (const cs of callSites(struct, "ccall")) {
    if (!cs.args) continue;
    const s = /^(["'])(qjs_\w+)\1$/.exec(code.slice(cs.args[0][0], cs.args[0][1]).trim());
    if (!s) continue;
    if (!abiCcalls.has(s[2])) abiCcalls.set(s[2], []);
    abiCcalls.get(s[2]).push({ file, line: line(cs.at) });
  }
}

/* ---- the run --------------------------------------------------------------------------------------------- */

const files = corpus();

/* THE REVISION THIS SCAN'S NUMBER BELONGS TO, TAKEN BEFORE THE FIRST FILE IS READ. §Testing's rule is about
   gates in general and not about compilers in particular: "a gate runs from a FROZEN SNAPSHOT … and the commit
   it measured is REPORTED WITH THE RESULT". A reader is the same defect as a builder here — this walks about
   seven hundred files over several seconds out of a checkout several agents are editing, so a run of it in the
   shared working tree measures a program no revision contains, exactly as a build that read `idl_args.h` 33
   seconds apart did. The difference is only that a reader's wrong answer LOOKS like a finding rather than like
   a segfault. So the cone the walk itself declared is asked of git, printed at the start and again beside the
   verdict — the tail is what gets pasted — and asked AGAIN at the end, because a file edited under this scan
   is a file whose reported line number names something else by the time anyone opens it. */
const REV_AT_START = gateRevision(cone);
for (const l of revisionLines(REV_AT_START)) console.log(l);

const jsScans = [];
for (const f of files) {
  let src;
  try { src = readFileSync(f.path, "utf8"); } catch { continue; }
  const rel = relative(ROOT, f.path);
  if (f.lang === "c") { scanC(rel, src); continue; }
  if (f.lang === "html") {
    const view = htmlScriptView(src);
    if (!view) continue;    /* a document with no inline script carries no seam — decided, not unreadable */
    src = view;
  }
  const s = scanJS(rel, src);
  if (s) jsScans.push({ ...s, area: f.area });
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

/* ---- the PLATFORM receiver: a DECIDED negative, from the IDL rather than from a list ---------------------- */

/* THIS GATE HAD NO WAY TO KNOW WHAT THE PLATFORM PROVIDES, so every read of a browser object's own member
 * looked like a read of a record nobody writes. `resp.redirected` beside `resp.url` is Fetch §4.4's Response
 * read exactly as the spec defines it, and it arrived here as a question about the engine's own emissions;
 * `new URL(x).searchParams` is URL §6.1; `form.action` is HTML §4.10.3. The gate's subject is stated in its own
 * header — the SERIALIZED seam, NOT the in-heap Web IDL surface, which idlgen.mjs audits off the spec — so a
 * receiver that is a platform object is not a record this gate is the auditor of, and saying so is not a
 * softening. It is the subject line being enforced in the one direction it never was.
 *
 * THE AUTHORITY IS @webref/idl, WHICH IS WHY THIS IS NOT A NAME LIST. A hand-written set of platform names
 * would work on the day it was written and would rot exactly as CLAUDE.md says a per-file ledger rots: it is
 * unfalsifiable, it grows one entry per false red, and every entry is somebody's recollection of a spec. The
 * W3C-curated corpus is the same authority idlgen already diffs the components against, read through the same
 * flattening (inheritance, mixins, partials, §3.7.9's iteration members) — one source, two consumers.
 *
 * AND THE IDENTITY COMES FROM A CONSTRUCT, NEVER FROM THE NAME SET — WHICH IS THE WHOLE DIFFERENCE BETWEEN
 * THIS AND A COINCIDENCE, AND IT WAS MEASURED BEFORE IT WAS WRITTEN. The first form of this rule asked whether
 * the receiver's whole read set is a subset of exactly one interface, which sounds decisive and is not: the
 * platform declares some thirty thousand members over fifteen hundred interfaces, so two ordinary English words
 * land uniquely somewhere by accident. That run decided 58 receivers and SIX of them were wrong — a font-table
 * directory entry `{offset, length}` came out a GPUCompilationMessage, a request-log row `{id, timestamp}` a
 * Gamepad, a JSON-schema property `{type, format}` a Summarizer. None was a wrong ANSWER (none of those is a
 * record of this engine's either), and that is exactly what made it intolerable: a wrong REASON, printed with
 * an authority behind it, is CLAUDE.md's wrong section number — it reads as checked and sends the next reader
 * somewhere the spec does not go.
 *
 * SO THE RECEIVER'S TYPE IS READ OFF THE DECLARATION THAT CREATED IT, and Web IDL supplies the type at every
 * step: `new URL(x)` is §6.1's constructor; an unshadowed `document` is Window's `document` attribute, whose
 * IDL type is Document; `await fetch(u)` is WindowOrWorkerGlobalScope's `fetch`, whose IDL return type is
 * `Promise<Response>`; `resp.clone()` is Response's own operation, returning Response. A binding's `= <expr>`
 * is a DECLARATION in the same sense a `#define` body is, so reading it is not the flowed identity this file
 * refuses — and it is taken only where it is unambiguous, never for a name declared twice or assigned again.
 * A receiver whose declaration this cannot resolve is not a platform object as far as this gate is concerned,
 * and it goes back to the shape anchor and to AMBIGUOUS exactly as before.
 *
 * AN IDL RETURN TYPE IS AN UPPER BOUND, WHICH IS WHY THE INTERFACE IS THEN NARROWED. `getElementById` returns
 * `Element?` and the object is an HTMLSelectElement; a strict member diff against Element would report
 * `selectedIndex` as a member the platform does not have, which is a finding manufactured out of the spec being
 * read too literally. So the construct fixes a BASE and the read set picks among that base and its descendants
 * — exactly one, after collapsing a chain to its most general matching member. Here the name set is doing work
 * it is entitled to do, because the construct has already cut the fifteen hundred candidates down to one
 * inheritance subtree.
 *
 * AND A NAME NO CANDIDATE DECLARES IS THE OTHER HALF OF THE DIFF, not a decided negative: the construct says
 * the object is a platform object and the spec says the object has no such member. That is reported, with the
 * interface the construct named — the same shape as idlgen's gap report, from the same authority, in the
 * opposite direction. */
const idl = await loadIdl();
const IFACE_MEMBERS = new Map();   // an interface a receiver could BE -> its flattened member names
for (const [name, node] of idl.byName) {
  /* A mixin is not an object a receiver can be — Web IDL §2.3 gives an interface mixin no interface object and
     no prototype — so it is never an identity. Its members are folded into every interface that includes it,
     which is where they are reachable from. */
  if (node.type === "interface mixin") continue;
  IFACE_MEMBERS.set(name, new Set(idl.members(name)));
}
/* WINDOW'S MEMBER LIST IS NOT WINDOW'S MEMBERS. Web IDL §3.7 puts every `[Exposed=Window]` interface's own
   interface OBJECT on the global, so `window.WebSocket` and `window.EventSource` are properties of Window that
   the Window interface's member list does not mention — and a diff that reads only that list reports two of the
   most ordinary reads in a page as names the platform does not declare. It is the same fact absent.c is
   generated from, so it is the same function that answers it. */
IFACE_MEMBERS.set("Window", new Set([...IFACE_MEMBERS.get("Window"), ...windowGlobals(idl)]));

/* AN INTERFACE OBJECT IS NOT AN INSTANCE, and conflating them is a member list that is wrong in both
   directions. Web IDL §3.7.3 gives the interface object a `prototype` property and §3.7.5/§3.7.7 put the
   CONSTANTS and the STATIC operations on it — while `open`, `send` and every other prototype member are
   reachable only through `.prototype` or an instance. So a bare exposed name resolves to a distinct identity,
   spelled `ctor:X`, whose `.prototype` is X. */
const CTOR = "ctor:";
const ctorMembers = (iface) => {
  const out = new Set(["prototype", "name", "length"]);
  for (const m of idl.flatten(iface)) if (m.name && (m.type === "const" || (m.type === "operation" && m.special === "static") || m.static)) out.add(m.name);
  return out;
};
const identityMembers = (id) => (id.startsWith(CTOR) ? ctorMembers(id.slice(CTOR.length)) : IFACE_MEMBERS.get(id));
const ifaceLabel = (id) => (id.startsWith(CTOR) ? `the ${id.slice(CTOR.length)} interface object` : id);
/* An interface name beginning with an ACRONYM is read letter by letter, so the article follows the sound of the
   LETTER's name and not of the letter: "a URL" and "an XMLHttpRequest", never "an URL" and "a XMLHttpRequest". */
const anIface = (id) => id.startsWith(CTOR) ? ifaceLabel(id)
  : `${(/^[A-Z]{2}/.test(id) ? /^[AEFHILMNORSX]/ : /^[AEIOU]/).test(id) ? "an" : "a"} ${id}`;
const childrenOf = new Map();
for (const [n, base] of idl.inheritanceOf) { if (!childrenOf.has(base)) childrenOf.set(base, []); childrenOf.get(base).push(n); }
const subtreeCache = new Map();
const subtreeOf = (n) => {
  let hit = subtreeCache.get(n);
  if (hit) return hit;
  const out = [], seen = new Set();
  (function walk(x) { if (seen.has(x)) return; seen.add(x); out.push(x); for (const c of childrenOf.get(x) || []) walk(c); })(n);
  subtreeCache.set(n, out);
  return out;
};

/* WHAT A BARE GLOBAL NAME IS, taken from the spec's two declarations of that fact and from nowhere else: an
   interface `[Exposed=Window]` puts its own interface OBJECT on the global (§3.7), and an attribute of Window
   whose IDL type is an interface puts an instance there (`document`, `location`, `navigator`, `history`). Both
   are read out of the parsed corpus, so a global this engine has never heard of is still answered correctly. */
const GLOBAL_IFACE = new Map();
for (const n of idl.declarations) {
  if (!((n.type === "interface" || n.type === "namespace" || n.type === "callback interface") && n.name)) continue;
  const ex = (n.extAttrs || []).find((a) => a.name === "Exposed");
  const v = ex && ex.rhs ? ex.rhs.value : null;
  const onWindow = ex && ex.rhs && (ex.rhs.type === "*" || (typeof v === "string" ? v === "Window"
                    : Array.isArray(v) && v.some((x) => (x.value || x) === "Window")));
  if (onWindow && IFACE_MEMBERS.has(n.name)) GLOBAL_IFACE.set(n.name, CTOR + n.name);
}
/* An IDL type expression down to the interface it names, or null. `Promise<Response>` is a Response to every
   reader that awaits it; a union or a sequence names no single interface and is answered as none. */
function ifaceOfType(t) {
  if (!t) return null;
  if (Array.isArray(t)) return t.length === 1 ? ifaceOfType(t[0]) : null;
  if (t.union) return null;
  if (t.generic === "Promise") return ifaceOfType(t.idlType);
  if (t.generic) return null;
  const n = typeof t.idlType === "string" ? t.idlType : null;
  return n && IFACE_MEMBERS.has(n) ? n : null;
}
/* §2.4's callback FUNCTIONS, which are a declaration kind of their own and are not in the interface map. A
   callback INTERFACE is (EventListener lives in byName with its one `handleEvent` operation), so the two
   spellings of "a function the platform will call" are looked up in the two places the parse put them. */
const CALLBACKS = new Map();
for (const n of idl.declarations) if (n.type === "callback" && n.name) CALLBACKS.set(n.name, n);

/* The argument list a callback TYPE declares, or null. */
function callbackArgs(typeName) {
  const cb = CALLBACKS.get(typeName);
  if (cb) return cb.arguments || null;
  const node = idl.byName.get(typeName);
  if (!node || node.type !== "callback interface") return null;
  const op = (node.members || []).filter((m) => m.type === "operation");
  return op.length === 1 ? op[0].arguments || null : null;
}

/* The declared type NAME at argument `i` of the operation `name` on `iface` — the one step that turns
   `addEventListener`'s second argument into `EventListener`. A nullable or optional argument declares the same
   type; a union declares none. */
function argTypeName(iface, name, i) {
  for (const m of idl.flatten(iface)) {
    if (m.type !== "operation" || m.name !== name || !m.arguments) continue;
    const a = m.arguments[i];
    if (!a || !a.idlType || a.idlType.union || a.idlType.generic) continue;
    if (typeof a.idlType.idlType === "string") return a.idlType.idlType;
  }
  return null;
}

/* AN OPERATION IS NOT ITS RESULT. Web IDL §3.7.7 puts a Function on the prototype, so `document.getElementById`
   is a function object and `document.getElementById(x)` is an Element — reading one member kind for both makes
   a bare `window.fetch` a Response, which is the plausible wrong answer this file is built against. The KIND is
   therefore part of the question, and the call branch and the member branch ask different ones. */
const memberTypeCache = new Map();
function memberIface(iface, name, kind) {
  const k = `${iface}\0${name}\0${kind}`;
  if (memberTypeCache.has(k)) return memberTypeCache.get(k);
  let out = null;
  for (const m of idl.flatten(iface))
    if (m.type === kind && m.name === name) { out = ifaceOfType(m.idlType); if (out) break; }
  memberTypeCache.set(k, out);
  return out;
}
/* A BARE GLOBAL NAME IS A VALUE, so only Window's ATTRIBUTES put an object there — `document`, `location`,
   `navigator`, `history`. Window's OPERATIONS put function objects there, and `fetch` is not a Response. */
for (const m of idl.flatten("Window"))
  if (m.type === "attribute" && m.name && !GLOBAL_IFACE.has(m.name)) {
    const t = ifaceOfType(m.idlType);
    if (t) GLOBAL_IFACE.set(m.name, t);
  }
GLOBAL_IFACE.set("window", "Window");
GLOBAL_IFACE.set("globalThis", "Window");

const platformDecided = [];   // {file,line,recv,iface,names} — a receiver a construct says is a platform object
const offInterface = [];      // {file,line,recv,iface,missing} — that object, read for a member the spec denies

/* The interface a receiver EXPRESSION evaluates to, or null. Every arm is a construct that names a type; the
   `seen` set is a cycle guard for `const a = b, const b = a`, not a depth bound. */
function ifaceOfExpr(text, off, scan, seen) {
  let t = (text || "").trim();
  if (!t) return null;
  for (;;) {
    if (/^await\s/.test(t)) { t = t.slice(6).trim(); continue; }
    if (t[0] === "(" && matchAt(t, 0) === t.length) { t = t.slice(1, -1).trim(); continue; }
    break;
  }
  let m = /^new\s+([A-Za-z_$][\w$]*)\s*\(/.exec(t);
  if (m && matchAt(t, t.indexOf("(", m[0].length - 1)) === t.length) return IFACE_MEMBERS.has(m[1]) ? m[1] : null;
  if (/^[A-Za-z_$][\w$]*$/.test(t)) {
    const s = seen || new Set();
    const key = `${scan.file}\0${t}\0${scan.binderOf(t, off)}`;
    if (s.has(key)) return null;
    s.add(key);
    const init = scan.initOf(t, off);
    if (init !== null && init !== "") return ifaceOfExpr(init, off, scan, s);
    if (scan.binderOf(t, off) === "file") return GLOBAL_IFACE.get(t) || null;
    /* A PARAMETER OF A CALLBACK THE PLATFORM CALLS IS TYPED BY THE IDL, and by nothing else in this file. The
       chain is three declarations long and every link is spec text: the callee's operation declares its
       argument a callback type, that callback declares its own argument list, and the parameter's POSITION
       picks one. Nothing is followed through an assignment or a return — a call site is a construct, and this
       is the same kind of fact as a binding's initializer, read at the other end of the call. */
    const slot = scan.paramSlot(t, off);
    if (!slot) return null;
    const m2 = /^(.*)\.([A-Za-z_$][\w$]*)$/.exec(slot.callee);
    if (!m2) return null;
    const on = ifaceOfExpr(m2[1], slot.calleeAt, scan, s);
    if (!on || on.startsWith(CTOR)) return null;
    const cbType = argTypeName(on, m2[2], slot.arg);
    const args = cbType && callbackArgs(cbType);
    const a2 = args && args[slot.param];
    return a2 ? ifaceOfType(a2.idlType) : null;
  }
  /* A trailing `(…)` is a CALL and a trailing `.name` is a member; both are stripped right to left, so
     `document.getElementById(i)` is Document's `getElementById` and nothing has to be parsed forward. */
  if (t.endsWith(")")) {
    let d = 0, open = -1;
    for (let i = t.length - 1; i >= 0; i--) {
      const c = t[i];
      if (c === ")" || c === "]" || c === "}") d++;
      else if (c === "(" || c === "[" || c === "{") { d--; if (!d) { open = i; break; } }
    }
    if (open <= 0) return null;
    t = t.slice(0, open).trim();
    const mm = /^(.*)\.([A-Za-z_$][\w$]*)$/.exec(t);
    if (!mm) return /^[A-Za-z_$][\w$]*$/.test(t) && scan.binderOf(t, off) === "file"
      ? memberIface("Window", t, "operation") : null;
    const base = ifaceOfExpr(mm[1], off, scan, seen);
    return base && !base.startsWith(CTOR) ? memberIface(base, mm[2], "operation") : null;
  }
  m = /^(.*)\.([A-Za-z_$][\w$]*)$/.exec(t);
  if (m) {
    const base = ifaceOfExpr(m[1], off, scan, seen);
    if (!base) return null;
    /* Web IDL §3.7.3: an interface object's `prototype` is the interface PROTOTYPE OBJECT, whose members are
       the interface's — which is the one step from the constructor identity back to the instance one. */
    if (base.startsWith(CTOR)) return m[2] === "prototype" ? base.slice(CTOR.length) : null;
    return memberIface(base, m[2], "attribute");
  }
  return null;
}

/* WHICH INTERFACE IN `base`'s SUBTREE THE READ SET FITS. An IDL return type is an upper bound — `getElementById`
   answers `Element?` and the object is an HTMLSelectElement — so the construct fixes the subtree and the names
   pick within it: the candidate covering the MOST of them wins, an inheritance chain collapses to its most
   general matching member, and a tie between unrelated candidates is undecided like every other tie here. */
function narrowIdentity(base, names) {
  const cands = base.startsWith(CTOR) ? [base] : subtreeOf(base);
  let bestN = -1;
  for (const i of cands) {
    const ms = identityMembers(i);
    if (!ms) continue;
    let hit = 0;
    for (const n of names) if (ms.has(n)) hit++;
    if (hit > bestN) bestN = hit;
  }
  const hits = cands.filter((i) => { const ms = identityMembers(i); if (!ms) return false; let h = 0; for (const n of names) if (ms.has(n)) h++; return h === bestN; });
  const set = new Set(hits);
  const general = hits.filter((h) => { for (let x = idl.inheritanceOf.get(h); x; x = idl.inheritanceOf.get(x)) if (set.has(x)) return false; return true; });
  if (general.length !== 1) return null;
  const iface = general[0];
  const ms = identityMembers(iface);
  /* AN EXPANDO IS NOT AN OFF-INTERFACE READ, and telling them apart is the difference between this category and
     a listing of every property the extension installs on a page. `window.apiclientsink` is not in any IDL and
     never will be — it is the PoC's own fire marker, written by intercept.js on the line above — so the read
     has a producer and the seam it belongs to is the one this gate already audits. A name off the interface
     with NO producer anywhere in the corpus is the other thing entirely: nobody writes it and the spec denies
     it, which is this gate's own defect class landing on a platform object. */
  const outside = [...names].filter((n) => !ms.has(n));
  const unwritten = outside.filter((n) => !(fields.get(n)?.writes.length));
  return unwritten.length ? { iface, missing: unwritten } : { iface };
}

/* The C matcher literals, anchored now that both producer sides — the C emissions and the JS record
   constructions — are in. */
for (const cm of cMatched) {
  let bestN = 0;
  for (const [, shp] of shapes) { let h = 0; for (const k of cm.keys) if (shp.has(k)) h++; if (h > bestN) bestN = h; }
  const unwritten = cm.keys.filter((k) => !(fields.get(k)?.writes.length));
  if (bestN >= 2 || !unwritten.length) {
    for (const k of cm.keys) rec(fields, k).reads.push({ file: cm.file, line: cm.line });
    continue;
  }
  for (const k of cm.keys) if (!unwritten.includes(k)) rec(fields, k).reads.push({ file: cm.file, line: cm.line });
  ambiguous.push({ file: cm.file, line: cm.line, recv: "a matcher pattern", shared: cm.keys.find((k) => !unwritten.includes(k)),
                   unwritten });
}

for (const s of jsScans) {
  const byRecv = new Map();
  for (const r of s.localReads) {
    if (!byRecv.has(r.key)) byRecv.set(r.key, []);
    byRecv.get(r.key).push(r);
  }
  for (const [, rs] of byRecv) {
    const recv = rs[0].recv;
    const names = new Set(rs.map((r) => r.name));
    /* ASKED BEFORE THE SHAPE ANCHOR, because a platform object whose members happen to collide with two of an
       emitted record's names is exactly the receiver the shape rule anchors CONFIDENTLY and wrongly — and it
       then reports the rest of that object's own IDL members as fields nobody writes, and every default over
       one of them as a concealment. Deciding identity first is what makes the two categories disjoint. */
    const base = ifaceOfExpr(recv, rs[0].off, s, null);
    if (base) {
      const narrowed = narrowIdentity(base, names);
      if (narrowed) {
        const site = { ...s.site(rs[0].off), recv, iface: narrowed.iface, base, names: [...names] };
        if (narrowed.missing) offInterface.push({ ...site, missing: narrowed.missing });
        else platformDecided.push(site);
        continue;
      }
    }
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

/* ---- the return-domain diff ------------------------------------------------------------------------------- */

/* THE PRIORITY THE HEADER STATES, APPLIED ONCE PER PRODUCER. A membership assertion over a returned identifier
   is the declaration; a `return` of a resolvable constant contributes itself; anything else leaves the domain
   OPEN with the line that opened it, which is the answer rather than a hole. */
const domainCache = new Map();
function domainOf(fn) {
  if (domainCache.has(fn)) return domainCache.get(fn);
  let out;
  const p = producers.get(fn);
  if (!p) out = { open: "no definition of this name is in the corpus this gate reads" };
  else if (p.dup.length) out = { open: `defined in more than one translation unit (${[p.file + ":" + p.line, ...p.dup].join(", ")})` };
  else {
    const values = new Set();
    let open = null;
    for (const r of p.returns) {
      if (!r.text) continue;                        /* `return;` — a void arm carries no value */
      const v = intOf(r.text, cConsts, null);
      if (v !== null) { values.add(v); continue; }
      if (/^[A-Za-z_]\w*$/.test(r.text)) {
        const mem = p.memberships.find((m) => m.id === r.text);
        if (mem) {
          const vs = mem.consts.map((t) => intOf(t, cConsts, null));
          if (!vs.some((x) => x === null)) { for (const x of vs) values.add(x); continue; }
          open = open || `${p.file}:${mem.line} asserts \`${r.text}\`'s membership against a constant this cannot resolve`;
          continue;
        }
      }
      open = open || `${p.file}:${r.line} returns \`${r.text.replace(/\s+/g, " ").slice(0, 40)}\`, which is not a constant this can resolve and no assertion in the body states its membership`;
    }
    out = open ? { open } : values.size ? { values, at: `${p.file}:${p.line}` } : { open: `${p.file}:${p.line} returns no value` };
  }
  domainCache.set(fn, out);
  return out;
}

const outsideDomain = [];    // a constant a consumer branches on that the producer cannot answer — the defect
const unenumerated = [];     // a value the producer answers that an EXHAUSTIVE construct omits
const undeclaredDomain = []; // a producer enumerated by a consumer and declared by nobody

for (const c of consumers) {
  const map = jsConsts.get(c.file) || new Map();
  const seen = new Map();
  let unresolved = null;
  for (const cmp of c.compared) {
    const v = intOf(cmp.text, map, null);
    if (v === null) { unresolved = unresolved || cmp; continue; }
    if (!seen.has(v)) seen.set(v, cmp);
  }
  const dom = domainOf(c.fn);
  if (dom.open) {
    /* TWO, NEVER ONE — one bare constant is a success test and reporting it would report the language. */
    if (seen.size >= 2)
      undeclaredDomain.push({ file: c.file, line: c.line, fn: c.fn, id: c.id,
                              constants: [...seen.keys()].sort((a, b) => a - b), why: dom.open });
    continue;
  }
  if (unresolved)
    refuse(c.file, unresolved.line, "a constant compared against an ABI result that this cannot resolve to a " +
           "value — whether it is in the producer's return domain is not decidable here", unresolved.text);
  for (const [v, cmp] of seen)
    if (!dom.values.has(v))
      outsideDomain.push({ file: c.file, line: cmp.line, fn: c.fn, id: c.id, value: v,
                           text: cmp.text, domain: [...dom.values].sort((a, b) => a - b), at: dom.at });
  if (c.claim) {
    const claimed = new Set();
    let bad = false;
    for (const t of c.claim.consts) { const v = intOf(t, map, null); if (v === null) bad = true; else claimed.add(v); }
    const missing = [...dom.values].filter((v) => !claimed.has(v)).sort((a, b) => a - b);
    if (!bad && missing.length)
      unenumerated.push({ file: c.file, line: c.claim.line, fn: c.fn, id: c.id, kind: c.claim.kind,
                          missing, domain: [...dom.values].sort((a, b) => a - b), at: dom.at });
  }
}

/* ---- the ABI diff ------------------------------------------------------------------------------------------ */

/* The transformation `extension/mojo.js` performs on every method name it installs, read off that file rather
   than assumed — a mojom `Step` is the binding's `step`. If the shipped rule ever changes, the file it is read
   from is the one place that says so. */
const lowerFirst = (s) => s.charAt(0).toLowerCase() + s.slice(1);
const upperFirst = (s) => s.charAt(0).toUpperCase() + s.slice(1);

const abiDefects = [];   // {kind, name, text}
let abiServed = null;    // the interface the binding table's own document is implemented against
{
  const boundFns = new Set([...abiBindings.values()].map((b) => b.fn));
  /* THE INTERFACE THE BINDING TABLE SERVES, taken from that table's own document rather than guessed from an
     overlap. Two documents naming two interfaces is not a thing this can resolve, and it is refused rather
     than resolved toward the first one. */
  /* AND ONLY FROM A DOCUMENT THAT CARRIES BINDINGS. Another file asking for a declaration is not a table
     serving it, and folding those in is how one program's question gets asked of another's list. */
  const bindingFiles = new Set([...abiBindings.values()].map((b) => b.file));
  const namedHere = servedInterfaces.filter((s2) => bindingFiles.has(s2.file));
  const named = [...new Set(namedHere.map((s2) => s2.iface))];
  const served = abiServed = named.length === 1 ? named[0] : null;
  if (named.length > 1)
    for (const s2 of namedHere)
      refuse(s2.file, s2.line, "more than one mojo interface is named by the documents that carry ABI " +
             "bindings — which declaration a binding answers to cannot be decided here", s2.iface);
  for (const [fn, e] of abiEntries) {
    if (!abiExported.has(fn)) continue;   /* the build's own stage owns this pair and states it better */
    if (boundFns.has(fn) || abiCcalls.has(fn)) continue;
    abiDefects.push({ kind: "an EXPORTED entry nothing calls", name: fn, place: `${e.file}:${e.line}`,
                      text: "defined and exported, and no binding names it and no driver ccalls it — the shape " +
                            "`qjs_result` had while the zone that should have read the result document " +
                            "defaulted it away instead" });
  }
  for (const [method, b] of abiBindings) {
    if (!abiEntries.has(b.fn))
      abiDefects.push({ kind: "a BINDING naming no entry", name: `${method} -> ${b.fn}`, place: `${b.file}:${b.line}`,
                        text: "the renderer would ccall a symbol no QJS_EXPORT body defines" });
    else {
      /* THE TYPE THE ANSWER IS READ WITH. A `const char *` read as a number is an ADDRESS reported as a
         value, and an `int` read as a string is a pointer dereference into whatever that integer names —
         neither has a symptom the reader can see. */
      const ret = abiEntries.get(b.fn).ret;
      const want = /^void$/.test(ret) ? null : /\*/.test(ret) ? "string" : "number";
      if (b.ret !== want)
        abiDefects.push({ kind: "a BINDING whose ccall type is not the entry's", name: `${method} -> ${b.fn}`,
                          place: `${b.file}:${b.line}`,
                          text: `the entry answers \`${ret}\` and the binding reads it as ` +
                                `${b.ret === null ? "nothing" : "`" + b.ret + "`"}` });
    }
    const mm = served ? mojomMethods.get(served + "#" + upperFirst(method)) : null;
    if (served && !mm)
      abiDefects.push({ kind: "a BINDING the typed boundary does not declare", name: method, place: `${b.file}:${b.line}`,
                        text: `${served} carries no method record of this name, so the validator that assumes ` +
                              "the renderer is hostile has never seen this call" });
    else if (mm && b.out !== null && !mm.reply.includes(b.out))
      abiDefects.push({ kind: "a BINDING answering under a field the reply does not declare", name: method,
                        place: `${b.file}:${b.line}`,
                        text: `the renderer places its answer under \`${b.out}\` and the reply declares ` +
                              `{${mm.reply.join(", ")}} — a caller reading the declared name gets undefined ` +
                              `from a call that succeeded` });
  }
  for (const [, rec] of mojomMethods) {
    if (rec.unresolved.length)
      for (const u of rec.unresolved) refuse(rec.file, rec.line, "a mojom reply element this cannot read — a " +
                                             "field name may have no counterpart and this cannot say", u);
    /* ASKED ONLY OF THE INTERFACE THE TABLE SERVES. Another interface's methods are implemented in another
       zone entirely, and demanding an ABI binding for one is a question about the wrong program. */
    if (served && rec.iface === served && !abiBindings.has(lowerFirst(rec.name)))
      abiDefects.push({ kind: "a DECLARED method with no binding", name: rec.name, place: `${rec.file}:${rec.line}`,
                        text: `${served} declares an operation the renderer has no entry bound for` });
  }
}

/* ---- the caller's read of a mojo reply --------------------------------------------------------------------- */

/* THE HALF THAT WAS ONE-SIDED. The ABI diff above asks whether the RENDERER places its answer under the field
 * the boundary declares; this asks whether the CALLER takes it back out under that same field. They are the
 * two ends of one wire and only both together close it: an `out:` that agrees with the mojom and a bridge that
 * reads a third spelling is `cspBlocks`/`cspBlocked` with a validated boundary in between, where the caller
 * gets `undefined` from a call that succeeded and every downstream number is a plausible datum.
 *
 * A method the boundary does not declare stays a REFUSAL rather than becoming a finding: which record that
 * call answers with is then not something this can read, and guessing it is the invention this file is against.
 */
const replyFieldDefects = [];
for (const r of mojoReplyReads) {
  const mm = abiServed ? mojomMethods.get(abiServed + "#" + upperFirst(r.method)) : null;
  if (!mm) {
    refuse(r.file, r.line, "an awaited call whose method the typed boundary does not declare — the record its " +
           "answer arrives as cannot be named, so the field read off it cannot be diffed against anything",
           `${r.method}() -> .${r.name}`);
    continue;
  }
  if (mm.unresolved.length || !mm.reply.length) {
    refuse(r.file, r.line, "an awaited call on a method whose declared reply this could not read whole — a " +
           "field read off it cannot be said to be absent from a list that is itself incomplete",
           `${r.method}() -> .${r.name}`);
    continue;
  }
  if (!mm.reply.includes(r.name))
    replyFieldDefects.push({ file: r.file, line: r.line, method: r.method, name: r.name,
                             reply: mm.reply, at: `${mm.file}:${mm.line}` });
}

/* ---- verdict --------------------------------------------------------------------------------------------- */

const readNoWriter =[...fields].filter(([, e]) => e.reads.length && !e.writes.length);
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
    log(`  ${place(a)}  \`${a.recv}\` ${a.shared ? `shares \`${a.shared}\`, also names` : "names only"} ` +
        `${a.unwritten.slice(0, 4).map((n) => `\`${n}\``).join(", ")}${a.unwritten.length > 4 ? " …" : ""}`);
  if (ambiguous.length > 15) log(`  … and ${ambiguous.length - 15} more`);
}

/* PRINTED IN FULL, AND NOT A DEFECT — a decided negative nobody can see is the concealment this file reports.
   Every row is a claim about the tree made on @webref/idl's authority, so every row is somewhere a reader can
   disagree with it; the alternative is a count that shrank for reasons nobody can check. */
if (platformDecided.length) {
  const byIface = new Map();
  for (const p of platformDecided) byIface.set(p.iface, (byIface.get(p.iface) || 0) + 1);
  log(`── DECIDED PLATFORM — ${platformDecided.length} receiver(s) whose own declaration names a Web IDL type. ` +
      `The platform is their producer and idlgen.mjs is their auditor; this gate's subject is the serialized ` +
      `seam, so they are out of it — decided, not passed ──`);
  for (const p of platformDecided.slice(0, 20))
    log(`  ${place(p)}  \`${p.recv}\` is ${anIface(p.iface)}${p.base === p.iface ? "" : ` (${anIface(p.base)}, narrowed)`} — ` +
        `${p.names.slice(0, 6).map((n) => `\`${n}\``).join(", ")}${p.names.length > 6 ? " …" : ""}`);
  if (platformDecided.length > 20) log(`  … and ${platformDecided.length - 20} more`);
  log(`  by interface: ${[...byIface].sort((a, b) => b[1] - a[1]).map(([i, n]) => `${ifaceLabel(i)}×${n}`).join(", ")}`);
}

if (offInterface.length) {
  log(`── OFF-INTERFACE — ${offInterface.length} read(s) of a name the spec does not declare on the object the ` +
      `receiver's own declaration says it is. Web IDL is the whole member list, so an expando on a platform ` +
      `object is a name whose only producer is the line that wrote it ──`);
  for (const o of offInterface.slice(0, 20))
    log(`  ${place(o)}  \`${o.recv}\` is ${anIface(o.iface)}, and ${o.missing.map((n) => `\`${n}\``).join(", ")} ` +
        `${o.missing.length > 1 ? "are members it does not declare" : "is a member it does not declare"}`);
  if (offInterface.length > 20) log(`  … and ${offInterface.length - 20} more`);
}

/* THE NAMESPACE'S OWN COVERAGE, PRINTED WHETHER OR NOT IT FOUND ANYTHING. A namespace that resolved NO
   consumer and one that resolved a dozen and liked every one of them print the same empty sections, and
   "nothing found" against "nothing asked" is the pair this whole file exists to keep apart — the same reason
   §Testing gives for never averaging an absent count with a zero one. */
{
  const seen = new Map();
  for (const c of consumers) { if (!seen.has(c.fn)) seen.set(c.fn, 0); seen.set(c.fn, seen.get(c.fn) + 1); }
  const parts = [...seen].sort().map(([fn, n]) => {
    const d = domainOf(fn);
    return `${fn}${n > 1 ? `\u00d7${n}` : ""} ${d.open ? "OPEN" : `{${[...d.values].sort((a, b) => a - b).join(",")}}`}`;
  });
  log(`── return domains ── ${domainBindings} ABI result(s) bound to a scopable name, ${consumers.length} of them ` +
      `branched on, against ${producers.size} C function definition(s) read for a domain` +
      (parts.length ? `: ${parts.join("; ")}` : " — NOTHING WAS CHECKED, so the sections below are silent " +
       "because nothing was asked, not because nothing is wrong"));
}

/* THE HEADLINE OF THE FOURTH NAMESPACE: a branch on a value the producer cannot answer. It is unreachable
   code that looks like handled code, and every other category here would report it as clean. */
show(`OUTSIDE THE RETURN DOMAIN — ${outsideDomain.length} comparison(s) against a value the producer cannot answer`,
     outsideDomain,
     (d) => `${place(d)}  \`${d.id} == ${d.text}\` (${d.value}) on ${d.fn}(), whose domain is ` +
            `{${d.domain.join(", ")}} declared at ${d.at} — the branch is unreachable`);

show(`RETURN VALUE NOT ENUMERATED — ${unenumerated.length} exhaustive construct(s) omitting a value the producer answers`,
     unenumerated,
     (d) => `${place(d)}  ${d.kind} over \`${d.id}\` omits ${d.missing.join(", ")} of ${d.fn}()'s ` +
            `{${d.domain.join(", ")}} (${d.at}) — the value arrives at whichever branch is the fallback`);

show(`RETURN DOMAIN UNDECLARED — ${undeclaredDomain.length} producer(s) a consumer enumerates and nothing declares`,
     undeclaredDomain,
     (d) => `${place(d)}  \`${d.id}\` from ${d.fn}() is compared against {${d.constants.join(", ")}}; ${d.why}`);

/* THE ABI'S OWN COVERAGE, on the same rule as the domain namespace's: four descriptions that agree and four
   that were never read print the same silence. */
log(`── qjs_* ABI ── ${abiEntries.size} QJS_EXPORT entr(ies), ${abiExported.size} on the build's export list, ` +
    `${abiBindings.size} bound to a mojo method, ${mojomMethods.size} method(s) the typed boundary declares, ` +
    `${abiCcalls.size} reached directly by a driver` +
    (abiServed ? `; the table is implemented against ${abiServed}, so both directions were asked`
               : " — NO SERVED INTERFACE IS NAMED by a document carrying bindings, so the declared-versus-bound " +
                 "direction was NOT asked and its silence is not an answer") +
    (abiEntries.size && abiBindings.size && mojomMethods.size ? "" :
     " — A SIDE READ NOTHING, so the rows below are silent because nothing was asked"));

show(`ABI DESCRIPTIONS THAT DISAGREE — ${abiDefects.length}`, abiDefects,
     (d) => `${d.place}  ${d.kind}: ${d.name} — ${d.text}`, 30);

log(`── mojo reply reads ── ${mojoReplyReads.length} awaited call(s) whose answer a field is read off` +
    (mojoReplyReads.length ? "" : " — NOTHING WAS CHECKED, so the row below is silent because nothing was asked"));
show(`A REPLY FIELD THE BOUNDARY DOES NOT DECLARE — ${replyFieldDefects.length} caller read(s)`, replyFieldDefects,
     (d) => `${place(d)}  \`(await …${d.method}()).${d.name}\` — ${abiServed}'s reply declares ` +
            `{${d.reply.join(", ")}} at ${d.at}, so this read is undefined from a call that succeeded`);

/* PER AREA AS WELL AS IN TOTAL, for §Testing's reason: one number in which the widest surface answers most of
   the count makes every other component invisible. */
{
  const areaOf = new Map(files.map((f) => [relative(ROOT, f.path), f.area]));
  const tally = new Map();
  const bump = (f, k) => { const a = areaOf.get(f) || "?"; if (!tally.has(a)) tally.set(a, { rnw: 0, wnr: 0, def: 0, amb: 0, dom: 0, off: 0, ref: 0 }); tally.get(a)[k]++; };
  for (const d of outsideDomain) bump(d.file, "dom");
  for (const d of unenumerated) bump(d.file, "dom");
  for (const d of undeclaredDomain) bump(d.file, "dom");
  for (const [, e] of readNoWriter) for (const r of e.reads) bump(r.file, "rnw");
  for (const [, e] of writeNoReader) for (const w of e.writes) bump(w.file, "wnr");
  for (const d of defaulted) bump(d.file, "def");
  for (const o of offInterface) bump(o.file, "off");
  for (const a of ambiguous) bump(a.file, "amb");
  for (const r of refusals) bump(r.file, "ref");
  log("── per area ──");
  const w = Math.max(...[...tally.keys()].map((k) => k.length));
  for (const [a, t] of [...tally].sort((x, y) => (y[1].rnw + y[1].wnr + y[1].def) - (x[1].rnw + x[1].wnr + x[1].def)))
    log(`  ${a.padEnd(w)}  read-no-writer ${String(t.rnw).padStart(4)}   write-no-reader ${String(t.wnr).padStart(4)}` +
        `   defaulted ${String(t.def).padStart(4)}   domain ${String(t.dom).padStart(3)}` +
        `   off-iface ${String(t.off).padStart(3)}   ambiguous ${String(t.amb).padStart(3)}` +
        `   refused ${String(t.ref).padStart(4)}`);
}

const rByReason = new Map();
for (const r of refusals) rByReason.set(r.reason, (rByReason.get(r.reason) || 0) + 1);
if (refusals.length) {
  log(`── REFUSED — ${refusals.length} construct(s) this scan cannot read. Zero is the armed state: each one is a ` +
      `place a field name could be hiding, and a scan that guessed past it would report a plausible answer ──`);
  for (const [reason, n] of [...rByReason].sort((a, b) => b[1] - a[1])) {
    log(`  ${String(n).padStart(5)}  ${reason}`);
    /* EVERY PLACE, not the first four. The only useful thing to do with a refusal is OPEN it, and a listing
       that showed four of twenty-eight made the largest group in this report unactionable — a reader could
       not tell one unreadable construct repeated across a file from twenty-eight distinct ones, which is the
       difference between one piece of work and twenty-eight. A group that grows makes the report grow, which
       is the correct pressure; the cap that remains is only there so a masker regression cannot bury the
       verdict under its own output. */
    const rs = refusals.filter((x) => x.reason === reason);
    for (const r of rs.slice(0, 60)) log(`         ${place(r)}  ${r.text}`);
    if (rs.length > 60) log(`         … and ${rs.length - 60} more in this group`);
  }
}

for (const l of revisionLines(REV_AT_START)) console.log(l);
{
  const moved = revisionMoved(REV_AT_START);
  if (moved) console.error("[rev] THE TREE MOVED UNDER THIS SCAN — " + moved + ". Every file:line above names " +
                           "a position in the sources as they were READ, which no revision now describes.");
  else console.log("[rev] the tree did not move under this scan");
}

log("── verdict ──");
const cats = [
  ["record field names READ with no writer", readNoWriter.length],
  ["record field names WRITTEN with no reader", writeNoReader.length],
  ["stream markers NAMED with no writer", mReadNoWriter.length],
  ["stream markers WRITTEN with no reader", mWriteNoReader.length],
  ["reads of a record field DEFAULTED rather than asserted", defaulted.length],
  ["whole emitted records DEFAULTED away by a substitute literal", wholeDefaulted.length],
  ["comparisons OUTSIDE a producer's return domain — branches on a value it cannot answer", outsideDomain.length],
  ["values a producer returns that an exhaustive construct does NOT enumerate", unenumerated.length],
  ["producers whose RETURN DOMAIN is undeclared while a consumer enumerates constants", undeclaredDomain.length],
  ["qjs_* ABI descriptions that DISAGREE — an entry, its export, its binding, its declared method", abiDefects.length],
  ["mojo reply fields a CALLER reads that the boundary does not declare", replyFieldDefects.length],
  ["reads OFF the interface a platform receiver's own declaration names, that nothing else writes either",
   offInterface.length],
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

/* WHAT IS STILL ONE-SIDED, now that the ABI's four descriptions are read together: the CALLER'S READ of a mojo
 * reply. `(await eng.r.renderer.step()).code` names a field this gate knows the declaration of, and it reads it
 * off a receiver the normalizer refuses — so the shipped bridge, the zone the NEED_FETCH defect actually lived
 * in, still sits one construct outside every namespace here. What would close it is a receiver walk that can
 * read an AWAITED CALL as a receiver whose record identity is the called method's reply, which is a construct
 * rather than an inference: the call names the method and the method's own record names its fields. Until that
 * exists the refusal is the honest answer and its place is printed.
 */
