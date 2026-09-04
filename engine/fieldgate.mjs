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
 *                    because it is what will hide the writer's removal tomorrow. It is the FALLBACK half of
 *                    that concealment; the other half is COERCED, one entry down, and every test in this
 *                    category looks for an operand the other one does not have.
 *                      AND THERE IS A THIRD HALF THIS GATE DOES NOT HAVE AND, ON THE EVIDENCE BELOW, SHOULD
 *                    NOT: the same collapse written as a POSITIVE TEST. `Array.isArray(f.children) &&
 *                    f.children.length` has no fallback operand and no conversion, so neither category above
 *                    can see it — and it folds a TWO-STATE field onto one answer exactly as `f.children || []`
 *                    does, because lib/field-def.js states `children: null` = NOT a message and `[]` = a
 *                    message with no fields. Two encoders wrote an empty JSON object under the field's own key
 *                    for both, which is the fabrication the DEFAULTED category exists to stop, spelled where
 *                    nothing here looks. It was found by reading, not by this gate, and fixing it moved no
 *                    count in any column.
 *                      A RULE WAS DESIGNED FOR IT AND REFUSED ON A MEASUREMENT, at 7f18a00c plus that fix.
 *                    The accusation would be "a positive-length test over a field whose producer distinguishes
 *                    `null` from `[]`", and the two-state fact is stated ONLY IN PROSE, in FIELD_DEF_ABSENT's
 *                    comments — so a rule needs either a list of two-state names typed in here, which is the
 *                    restated second copy this auditor exists to catch, or dataflow this scan does not do.
 *                    Taking the four fields whose comments DO say it (`children`, `_excludedValues`,
 *                    `_predicates`, `_looselyEquals`) and accusing every such test over them yields SIX live
 *                    sites, of which ONE is a defect: the other five are folds that are CORRECT — a badge and a
 *                    placeholder that render nothing for either state, and three OpenAPI exports that omit the
 *                    key for either. Five sixths noise, and a gate at that rate gets muted whole.
 *                      WHAT SEPARATES THE ONE FROM THE FIVE IS NOT THE TEST — it is whether the `null` arm
 *                    WRITES A VALUE or does nothing, which is semantic and is not a thing a scan of this shape
 *                    can ask. It is not unanswerable, though; it is answered in the RECORD instead. A two-state
 *                    field with a NAMED READER beside its record (lib/field-def.js's `fdChildren`, and
 *                    `fdHasChildren` beside it for the fold that IS correct) forces every site to spell which
 *                    of the three arms it means, and the arms are then visible to whoever reads it. So the
 *                    absence of a rule here is a decision and not a gap, and the place to look when this
 *                    defect next appears is a two-state field with no named reader.
 *   COERCED          the same concealment with nothing in the line to find. A read standing under an
 *                    ECMAScript conversion does not have its absence SUBSTITUTED, it has one MINTED:
 *                    §7.1.19 ToString ( arg ) answers the String `undefined` for undefined and `null` for
 *                    null, and both of those are property keys a JSON object can carry and path segments a
 *                    server can spell. Measured: four `String(f.id)` reads composed a wire address off a
 *                    record that does not carry `id`, the token matched the FIRST entry of the map being
 *                    searched, and what came out was a field identity, a type badge and a rename button all
 *                    describing a field nothing described — invisible to every category above, because the
 *                    DEFAULTED test looks for a `||` and there was none. ONE of its three bands accuses and
 *                    two are printed or counted; §the COERCION states the axis and §THE THREE BANDS states
 *                    what was measured and what was refused.
 *   OUTSIDE THE RETURN DOMAIN
 *                    the same defect where the plausible datum is a VALUE and not a name: a host branching on
 *                    a code its producer cannot answer. Every identifier on both sides is spelled correctly,
 *                    so the three categories above see nothing — see §the RETURN-DOMAIN namespace below.
 *                    Beside it, the value a producer DOES answer that an exhaustive construct omits, and the
 *                    producer that never declared what it can answer at all.
 *   DECIDED PLATFORM a receiver whose OWN DECLARATION names a Web IDL type — `new URL(x)`, `await fetch(u)`,
 *                    an unshadowed `document`, the `Event` a callback the platform calls is handed, the
 *                    binding a `for … of` over a platform object declares. The platform is its producer and
 *                    idlgen.mjs is its auditor, so it is out of THIS gate's subject. Printed in full with the
 *                    interface that decided it: a decided negative nobody can see is the concealment this file
 *                    exists to report, performed on its own output.
 *                      AND THE BAND ASKS ONE QUESTION, NOT TWO — see §the TWO QUESTIONS A BASE ANSWERS. WHOSE
 *                    contract a receiver is on is decided by the construct alone; WHICH interface it is needs
 *                    the read set as well, and a read set that fits eleven of a subtree's interfaces equally
 *                    answers the second and not the first. A row whose narrowing stayed open is still decided
 *                    here and says so on its own line.
 *   DECIDED FOREIGN  a receiver a module OUTSIDE this corpus hands to a callback — the callee is a binding this
 *                    file imported from a BARE specifier, so the party that supplies the record is a package or
 *                    a `node:` builtin. Same sentence as DECIDED PLATFORM with the module system as the
 *                    authority instead of the IDL: it says WHERE the producer is, which is all a specifier can
 *                    say, and it claims no interface. Without it those receivers reach the SHAPE anchor and are
 *                    decided by name collision — Node's `IncomingMessage` reads `url`, `headers` and `method`.
 *   DECIDED REMOTE-STATED
 *                    the same scoping answer for a producer that is NOT a module and has no specifier at all: a
 *                    body read off a `Response`, which is bytes a party outside this process stated. It is its
 *                    own band because the two differ on the question the VERDICT asks — a module's record may be
 *                    DCHECKed and a stranger's may never be, so one band could not say which remedy a row is
 *                    owed. §REMOTE-STATED at the mint, §THE-TWO-QUESTIONS-A-DECIDED-NEGATIVE-ANSWERS at the
 *                    print.
 *   OFF-INTERFACE    that same object, read for a name the spec does not declare on it AND that no producer in
 *                    the corpus writes either. The interface's member list is the whole member list, so this
 *                    is READ-NO-WRITER landing on a platform object — the one defect the decided negative
 *                    could otherwise have swallowed. Where the narrowing stayed open the accusation is made
 *                    against EVERY candidate in the subtree at once — a name no candidate declares is off the
 *                    object whichever candidate it turns out to be — and never against the base alone.
 *   AMBIGUOUS        a receiver whose record identity this cannot decide. Counted, never resolved either way.
 *   REFUSED          a construct this scan cannot read. Counted and named with file:line, never guessed past.
 *
 * WHY REFUSING IS THE WHOLE DISCIPLINE. idl_installed.mjs records the same primitive lying four separate ways
 * because it read TEXT where it needed a STRUCTURE — a member credited because the word appeared in the file,
 * a `lastIndexOf` matching inside a longer identifier, an indirect call matching no pattern. A scan that
 * guesses hands the analysis a plausible answer and the refusal machinery never runs. So every name here is
 * taken OUT OF A CONSTRUCT: a JSON key position inside a resolved emission format, an object-literal key, a
 * member expression whose receiver this can normalize, and a COMPUTED key resolved through the same declaration
 * this file reads to type a receiver. An unresolvable format argument, a receiver that is not a normalizable
 * chain, a computed key naming an identifier NO scope here binds (whose constant can be declared in another
 * file), a file whose brackets do not balance — each is a REFUSAL with its file and line, and zero refusals is
 * the armed state. WHAT IS NOT A REFUSAL IS A DECIDED NEGATIVE, and the line between them is whether a name
 * could still be there: a key the program COMPUTES — a call, a member, a template, or an identifier a scope
 * binds without a declaration this can read — names no string in this file, and `[expr]:` is the syntax that
 * says so. Counting that as unreadable put three constructs in the refusal list that were the language.
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
 * The single-field case is NOT a pass — it is the AMBIGUOUS category, named with its place, because whether
 * a receiver is an endpoint or an AST node is a question this cannot answer and answering it either way is the
 * guess.
 * AND THE SHAPE IS ASKED OF BOTH ZONES, BECAUSE A NAME COLLIDES ACROSS THEM IN BOTH DIRECTIONS AT ONCE. One C
 * function body is one record and ONE JS OBJECT LITERAL IS ONE RECORD, off the same construct the whole-record
 * default already reads. Without the second half the anchor had only ONE authority it could be decided by, so
 * two generic names were enough to hand a record built entirely in JavaScript to a C emission — and then the
 * WRITE question, asked of the CORPUS rather than of the anchored record, let any unrelated party holding that
 * spelling stand in as its producer. Those are one defect seen from its two ends: identity by NAME ALONE. A JS
 * literal declaring nothing the engine does not declare is a RESTATEMENT of the emission and not a rival to it,
 * or a driver's own expectation would dispossess the record it was written to check.
 * AND A JS RECORD IS NOT ALWAYS FINISHED WHERE IT IS OPENED, WHICH IS THE HALF THAT BIT HARDEST — because the
 * fields a record adds by MEMBER ASSIGNMENT are exactly the ones it copies off an emission, so the literal was
 * left holding only the record's OWN names, scored one against the emission's several, and lost an anchor it
 * should have taken. The two halves are unioned under §the BINDING, which is a construct and not a proximity
 * rule: the literal must be the operand of an `=` whose left side is a bare identifier, and that binding's
 * DECLARATION — the same one the platform arm types a receiver off — must be exactly one write whose text is
 * that literal, so a name re-assigned to a second object cannot merge two records. A record assembled entirely
 * out of an emission's own field names is still a RESTATEMENT and is still dropped: the fold widens what a
 * rival can be, never what counts as one.
 * A BODY THAT DOES NOT OWN ITS BUFFER IS NOT A RECORD EITHER — see §the FRAGMENT, which is what keeps a record
 * emitted by a composer plus a helper from being read as two records that each lack the other's fields.
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
  /* the template/`${}` stack: each entry is the brace depth a `${` was opened at, AND the offset it opened
     at. The second is read by §the COERCION, and it is recorded here rather than recovered later because this
     pass is the only one that knows which `${` belongs to a template and which `{` opens a block — asking that
     of the masked text afterwards would be a guess about two characters. */
  const tmpl = [];
  const subst = [];   // [interior start, closing-brace offset) for each `${ … }`
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
          tmpl.push({ depth: braceDepth, open: j + 2 });
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
      if (tmpl.length && braceDepth === tmpl[tmpl.length - 1].depth) {
        /* back into the template that opened this `${` */
        subst.push([tmpl.pop().open, i]);
        let j = i + 1;
        for (;;) {
          if (j >= n) return { fault: { off: i, why: "an unterminated template literal" } };
          if (src[j] === "\\") { j += 2; continue; }
          if (src[j] === "`") { blank(struct, i + 1, j); i = j + 1; break; }
          if (src[j] === "$" && src[j + 1] === "{") { blank(struct, i + 1, j); tmpl.push({ depth: braceDepth, open: j + 2 }); braceDepth++; i = j + 2; break; }
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
  return { code: code.join(""), struct: struct.join(""), subst };
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

/* ONE EXPRESSION OR TWO — ASKED OF THE STRUCTURE, NEVER OF THE TEXT. Once a span's whitespace is collapsed a
   `;` is the only character left that can say it crossed a statement boundary, and three sites here asked that
   of `code`, where a string, template or regexp literal's own characters are intact. `maskJS` blanks those
   contents in `struct` for exactly this reason: a `;` inside a literal is a DATUM and cannot terminate
   anything, so reading one as a boundary refuses a span that is one expression by construction. It refused
   `first._park.join(";")` — a real destructuring source, in the corpus, over the ONE array whose absence this
   project treats as a dropped frontier. Every caller already has the struct offsets, because the span was
   DELIMITED on `struct` in the first place; only its TEXT comes from `code`. */
const crossesStatement = (structSpan) => structSpan.includes(";");

/* ---- what stands between a read and the `||` after it ------------------------------------------------------ */

/* A `||` OR `??` FOLLOWING A READ IS NOT NECESSARILY THAT READ'S DEFAULT, AND THE TWO ARE OPPOSITE FACTS ABOUT
 * THE SAME LINE. `x.f || d` SUBSTITUTES `d` for `x.f`, which is the concealment this category is named after —
 * a field that was never written arrives at the consumer as a plausible datum. `!x.f || …` and
 * `a.f !== x.f || …` are the OTHER thing: the field's value is consumed by an operator whose result is a
 * BOOLEAN, and the `||` then joins booleans. Nothing is substituted for the field, because a boolean is not a
 * value anyone mistook for one — the absence DECIDES a branch instead of hiding inside a datum. Reported as
 * defaults, those lines say the exact reverse of what they do: `if (!r.headers || typeof r.headers !== 'object')
 * throw` is the DCHECK this gate's own verdict tells a reader to write, printed as the defect it is the cure for.
 *
 * SO THE QUESTION IS STRUCTURAL AND IT IS ASKED TO THE LEFT, NOT TO THE RIGHT. Precedence puts every comparison
 * and every unary `!` INSIDE an operand of `||`, so a read that ends just before a `||` is the whole left operand
 * only when nothing to its left is already consuming it. That is one token: the significant text immediately
 * before the read expression begins.
 *
 * ONLY BOOLEAN-PRODUCING OPERATORS COUNT, and the narrowness is the point rather than caution. `(a.b + c.d) || e`
 * is still reported, because `undefined + 1` is `NaN`, `NaN` is falsy, and `e` then stands in for a number the
 * producer never wrote — a fabricated datum by the same route as `x.f || 0`. `a && b.c || d` is still reported
 * for the same reason. Widening this test to "anything that is not the whole operand" would take both of those
 * out, and they belong in. The list is therefore the comparisons and the logical NOT, and nothing else.
 *
 * `=>` IS NOT `>`. The arrow's second character is the one character that makes this test answer backwards —
 * `xs.map((x) => x.f || 0)` would read its `>` as a relational operator and decide away a real default at the
 * most ordinary callback shape there is — so every relational form states which characters may NOT precede it. */
const BOOL_OPS = [
  { op: "===", before: "" }, { op: "!==", before: "" }, { op: "==", before: "" }, { op: "!=", before: "" },
  { op: ">=", before: ">" }, { op: "<=", before: "<" }, { op: ">", before: "=>" }, { op: "<", before: "<" },
];
function boolConsumer(struct, code, start) {
  let i = start;
  while (i > 0 && /\s/.test(struct[i - 1])) i--;
  if (i === 0) return null;
  if (struct[i - 1] === "!") return "!";
  for (const { op, before } of BOOL_OPS) {
    if (struct.slice(i - op.length, i) !== op) continue;
    /* `before` is the characters that, standing immediately in FRONT of this text, make it the tail of a
       different operator: `>>=` in front of `>=`, `<<` in front of `<`, and `=>`/`>>` in front of `>`. Spelled
       out beside the operator so the guard cannot drift away from the thing it guards. */
    if (before.includes(struct[i - op.length - 1])) continue;
    return op;
  }
  /* ECMAScript §13.10 Relational Operators' WORD-spelled member. `x instanceof C || …` is a comparison whose
     text is an identifier, so the character test above cannot see it at all.
     `in` IS THE OTHER ONE AND IT IS DELIBERATELY ABSENT, because §14.7.5 The for-in, for-of, and for-await-of
     Statements spells its head with the SAME WORD and the two are not the same operator. `for (var p in
     schema.properties || {})` — which is in this corpus — is a `|| {}` defaulting a record field, and reading
     its `in` as a comparison would decide away the exact defect this category exists for. Telling the two
     apart means looking left for a `for (`, and the value of getting that right is one construct nobody
     writes: a relational `in` immediately in front of a `||`. So the trap is removed rather than guarded, and
     an `x in o.f || y` is REPORTED — the under-crediting direction this file chooses everywhere else. */
  const w = /([A-Za-z_$][\w$]*)\s*$/.exec(code.slice(Math.max(0, i - 24), i));
  return w && w[1] === "instanceof" ? w[1] : null;
}


/* ---- §the COERCION: the absence that arrives as a value with no default standing beside it ----------------- */

/* A DEFAULT SUBSTITUTES A VALUE FOR AN ABSENCE; A COERCION MINTS ONE OUT OF IT, AND THE MINTED ONE IS THE
 * BETTER CONCEALMENT. §the CATEGORIES states DEFAULTED as "`||`, `??`, `?.`, or inside a swallowing
 * try/catch" — every one of those an OPERAND standing in the line beside the read, which is the whole reason
 * that category is findable. ECMAScript §7.1.19 ToString ( arg ) needs no operand: its own steps answer the
 * String `undefined` for undefined and the String `null` for null. Both of those are property keys a JSON
 * object can carry and path segments a server can spell, so the absence does not merely survive
 * the read — it arrives at the consumer WEARING THE SHAPE OF THE THING BEING LOOKED FOR. Measured in this
 * corpus: a lookup composed a wire address with `String()` off a record that does not carry the field, and the
 * token it minted matched the FIRST entry of the map it was searching; what came out was a field identity, a
 * type badge and a rename button all describing a field nothing described. Every DEFAULTED test in this file
 * looks for a fallback, and in that line there was none to find.
 *
 * THE AXIS IS THE ABSTRACT OPERATION, NEVER A LIST OF COERCING FUNCTIONS. Such a list is the second copy this
 * auditor exists to catch: right on the day it is written, silently short on the day the corpus grows a
 * spelling. What is asked instead is which of ECMAScript's own conversions the read's SYNTAX places it under.
 *   §7.1.19 ToString ( arg )  undefined => `undefined`, null => `null`. Both are plausible data — a key, a
 *                             path segment, an attribute value, a query parameter. This is the sharp half.
 *   §7.1.4 ToNumber ( arg )   undefined => NaN, null => +0. An absent field reaching a count, an index or an
 *                             id as `0` is the same plausible datum by the other route; NaN is the loud half
 *                             and travels with it, because WHICH of the two an absent field is here is
 *                             precisely the question nobody at the read can answer.
 *   §7.1.2 ToBoolean ( arg )  DELIBERATELY OUT, for the reason §boolConsumer argues one screen up: a boolean
 *                             is not a value anyone mistook for a datum — the absence DECIDES A BRANCH rather
 *                             than hiding inside one. Reporting it would report every `if (o.f)` in the
 *                             corpus, which is reporting the language.
 *
 * AND THE SYNTAX IS THE SPEC'S OWN PRODUCTIONS, each a construct rather than a spelling:
 *   §13.2.8.5 Runtime Semantics: SubstitutionEvaluation — a read inside a `${ }`, whose spans the masker
 *             records because it is the pass that knows which `${` is a template's.
 *   §13.15.3 ApplyStringOrNumericBinaryOperator — with opText `+`, a String on EITHER side makes the operator
 *             take ToString of both, so a `+` chain holding a string literal coerces every operand in it;
 *             with any other opText both sides go through ToNumeric, which is §7.1.4 for a non-BigInt.
 *   §13.5.4 Unary + Operator — ToNumber of its one operand.
 *   §19.2 Function Properties of the Global Object, and the two constructors called as functions beside them:
 *             §22.1.1.1 String ( value ), §21.1.1.1 Number ( value ), §19.2.5 parseInt ( string, radix ) and
 *             §19.2.4 parseFloat ( string ) each STATE the conversion of their first argument in their own
 *             steps. A name THIS CORPUS BINDS is not one of them, which is asked of `declaredHere` rather than
 *             assumed — the same question the platform arm asks of an unshadowed `document`.
 *
 * WHAT IS LEFT OUT, SAID PLAINLY, BECAUSE A SILENT OMISSION IS THIS FILE'S OWN DEFECT CLASS PERFORMED ON ITS
 * OWN OUTPUT.
 *   A WEB IDL ARGUMENT typed DOMString or USVString coerces exactly as `String()` does — `el.setAttribute(k,
 *     v.f)` mints the token `undefined` through the platform rather than through a global — and it is
 *     DERIVABLE here, off the same @webref/idl the platform arm already loads. It is not asked, because typing the callee's
 *     RECEIVER is a question the later anchor pass answers while this one runs during the scan. THE NEXT DIFF
 *     asks it where `ifaceOfExpr` already stands and joins `argTypeName` to it, so that a coerced argument is
 *     reported at the platform call. ITS ABSENCE SHOWS as a row landing on a `String(...)` one line above a
 *     `setAttribute` and no row at all on the `setAttribute` that has no `String()` in front of it.
 *   `x.f.toString()` AND `x.f.join()` are not this defect: they THROW on an absence, which is the loud
 *     outcome this whole file exists to ask for, and a member immediately called is already skipped as an
 *     operation rather than a datum.
 *   `JSON.stringify(x.f)` yields the ABSENT value rather than a token, so it conceals nothing a reader could
 *     mistake for a datum.
 *   A TAGGED TEMPLATE hands its substitutions to its tag function UNCONVERTED (§13.3.11 Tagged Templates), so
 *     a read inside one is not under §13.2.8.5 at all and this OVER-reports it. The masker records a `${ }`
 *     span without asking what stands in front of the backtick, so telling the two apart is one character the
 *     next diff reads there; until it does, the over-report lands in the KEYED/A VALUE bands, which are listed
 *     and counted rather than accused, and it SHOWS as a template-substitution row whose backtick is preceded
 *     by an identifier.
 *
 * AND THE IDENTITY QUESTION IS ASKED OF ONE CONSTRUCT OF THE FOUR, WHICH IS A NARROWING AND IS NAMED AS ONE.
 * §THE THREE BANDS accuses a coerced read whose token is an EQUALITY OPERAND, and only the global-conversion
 * arm knows its own outer extent — a call runs from its callee to its closing paren, which is where the `===`
 * is looked for. A template substitution and a `+` chain are coerced expressions too, and their extents are a
 * backtick pair and an operand run this walk does not currently carry out of `coercionOf`; so `${x.f} === k`
 * and `"a" + x.f === k` are recorded as A VALUE. THE NEXT DIFF returns the coerced expression's span from
 * `coercionOf` beside its conversion, so `isEqualityOperand` is asked of all four the same way. ITS ABSENCE
 * SHOWS as a template or `+` row in the A VALUE band with a `===` on the same line. */

/* WHERE a coerced read stands, which is what decides whether the token it mints is a datum or a diagnostic.
   Three constructs, and each is already read in this file for another question: the argument list of a
   should-never-happen (the absence ABORTS there, so the conversion is printing a message beside a crash, not
   feeding a consumer), the argument list of a console emission (the token is being shown to a person, who is
   the one reader who can tell "undefined" from a field value), and everywhere else — which is the seam. */
function readPlace(assertSpans, emitSpans, off) {
  if (assertSpans.some(([a, b]) => off >= a && off < b)) return "assert";
  if (emitSpans.some(([a, b]) => off >= a && off < b)) return "log";
  return null;
}

/* One past the opener of the innermost `(` enclosing `off` with no balanced pair between them, or -1 when the
   innermost enclosing bracket is not a `(` at all. A `[` or `{` in that position ends the walk rather than
   being stepped over: an argument list is what the conversion functions below are read out of, and a read
   inside an array or an object literal inside a call is not in the call's argument position. */
function enclosingCallParen(struct, off) {
  let d = 0;
  for (let i = off - 1; i >= 0; i--) {
    const c = struct[i];
    if (c === ")" || c === "]" || c === "}") d++;
    else if (c === "(" || c === "[" || c === "{") {
      if (d === 0) return c === "(" ? i : -1;
      d--;
    }
  }
  return -1;
}

/* The span [a,b) with its surrounding whitespace removed. */
function spansOf(struct, a, b) {
  while (a < b && /\s/.test(struct[a])) a++;
  while (b > a && /\s/.test(struct[b - 1])) b--;
  return [a, b];
}

/* The `[` of the innermost MEMBER ACCESSOR enclosing `off`, or -1. An array literal's `[` is not one, and the
   character in front of the bracket is what says which it is: a member accessor's base ends in an identifier
   character or a closing bracket, an array literal's `[` follows an operator, a comma or nothing. */
function enclosingBracketKey(struct, off) {
  let d = 0;
  for (let i = off - 1; i >= 0; i--) {
    const c = struct[i];
    if (c === ")" || c === "]" || c === "}") d++;
    else if (c === "(" || c === "[" || c === "{") {
      if (d) { d--; continue; }
      if (c !== "[") return -1;
      let b = i;
      while (b > 0 && /\s/.test(struct[b - 1])) b--;
      return b > 0 && /[\w$)\]]/.test(struct[b - 1]) ? i : -1;
    }
  }
  return -1;
}

/* IS THE COERCED EXPRESSION AN EQUALITY OPERAND — which is the question that separates a token that EXISTS
   from a token that MATCHES. `String(x.f) === node.field` is the measured incident exactly: the absence became
   the token `undefined`, a JSON body's own key can BE that token, and the comparison then succeeded on the first entry
   of the map. Asked of the WHOLE coerced expression, never of the read inside it. */
const EQ_OPS = ["===", "!==", "==", "!="];
function isEqualityOperand(struct, a, b) {
  let r = b;
  while (r < struct.length && /\s/.test(struct[r])) r++;
  for (const op of EQ_OPS) if (struct.slice(r, r + op.length) === op) return true;
  let l = a;
  while (l > 0 && /\s/.test(struct[l - 1])) l--;
  for (const op of EQ_OPS) if (struct.slice(l - op.length, l) === op) return true;
  return false;
}

/* One operand of a `+` chain, stepped over from its first character. Identifier characters, `.`, `?.` and
   balanced groups are the operand; anything else ends it. -1 says this walk cannot tell where the operand
   ends, which ends the chain — the under-crediting direction this file takes everywhere. */
function skipOperandFwd(struct, i) {
  const n = struct.length;
  let started = false;
  while (i < n) {
    const c = struct[i];
    if (/[\w$.]/.test(c)) { i++; started = true; continue; }
    if (c === "?" && struct[i + 1] === ".") { i += 2; continue; }
    if (c === "(" || c === "[") { const e = matchAt(struct, i); if (e < 0) return -1; i = e; started = true; continue; }
    break;
  }
  return started ? i : -1;
}
function skipOperandBack(struct, i) {
  let started = false;
  while (i > 0) {
    const c = struct[i - 1];
    if (/[\w$.]/.test(c)) { i--; started = true; continue; }
    if (c === ")" || c === "]") {
      let d = 0, j = i - 1;
      for (; j >= 0; j--) {
        const q = struct[j];
        if (q === ")" || q === "]" || q === "}") d++;
        else if (q === "(" || q === "[" || q === "{") { d--; if (!d) break; }
      }
      if (j < 0) return -1;
      i = j; started = true; continue;
    }
    break;
  }
  return started ? i : -1;
}

/* Is the read at [from,to) an operand of a `+` chain one of whose operands is a STRING or TEMPLATE literal?
   §13.15.3's `+` takes ToString of BOTH operands the moment either side is a String, and `+` is
   left-associative, so one string literal anywhere in the run is enough to say a ToString happened to at
   least the sub-expression this read is in. The quote characters survive in `struct` — only their CONTENTS
   are blanked — so whether an operand is a string literal is a structural question here. */
function plusChainString(struct, from, to) {
  let i = to;
  for (;;) {
    while (i < struct.length && /\s/.test(struct[i])) i++;
    if (struct[i] !== "+" || struct[i + 1] === "+" || struct[i + 1] === "=") break;
    i++;
    while (i < struct.length && /\s/.test(struct[i])) i++;
    if (struct[i] === '"' || struct[i] === "'" || struct[i] === "`") return true;
    const e = skipOperandFwd(struct, i);
    if (e < 0) break;
    i = e;
  }
  let j = from;
  for (;;) {
    while (j > 0 && /\s/.test(struct[j - 1])) j--;
    if (struct[j - 1] !== "+" || struct[j - 2] === "+" || struct[j - 2] === "=" || struct[j - 2] === "!"
        || struct[j - 2] === "<" || struct[j - 2] === ">") break;
    j--;
    while (j > 0 && /\s/.test(struct[j - 1])) j--;
    if (struct[j - 1] === '"' || struct[j - 1] === "'" || struct[j - 1] === "`") return true;
    const e = skipOperandBack(struct, j);
    if (e < 0) break;
    j = e;
  }
  return false;
}

/* The arithmetic operators §13.15.3 sends through ToNumeric, plus `+`, whose own conversion is decided by the
   VALUES and not by the syntax — either operand being a String makes it ToString of both, and nothing else
   makes it ToNumber of both. Where a string LITERAL is in the chain the answer is known and `plusChainString`
   has already given it; where it is not, the read is still under a conversion and WHICH one is undecidable
   here, which the row says rather than picking.
   `/` IS DELIBERATELY ABSENT. The masker decides division from regexp by what precedes the slash; a `/` after
   a read is a division and readable, but a `/` BEFORE one can be the closing slash of a regexp whose interior
   this view has blanked, and telling those apart at this end is the guess this file refuses everywhere. The
   cost is that `a / o.f` is not reported while `o.f * 2` is — the under-crediting direction. */
const ARITH_OPS = ["**", "*", "%", "-", "+"];
const ARITH_SEC = "ECMAScript §13.15.3 ApplyStringOrNumericBinaryOperator ( leftValue, opText, rightValue )";
const arithTo = (op) => (op === "+" ? "ToString or ToNumber — §13.15.3 decides on the VALUES" : "ToNumber");

/* The ECMAScript global functions whose own steps convert their FIRST argument, with the clause that says so.
   This is not a list of "functions that coerce" — it is the set of §19.2 Function Properties of the Global
   Object (plus the two constructors §22.1.1.1 and §21.1.1.1 called without `new`) whose defined steps begin by
   converting argument one, which is a fact about the STANDARD and not about this corpus. A name the corpus
   binds for itself is not the global one, and that is asked, never assumed. */
const GLOBAL_CONVERSION = new Map([
  ["String", { to: "ToString", sec: "ECMAScript §22.1.1.1 String ( value )" }],
  ["Number", { to: "ToNumber", sec: "ECMAScript §21.1.1.1 Number ( value )" }],
  ["parseInt", { to: "ToString", sec: "ECMAScript §19.2.5 parseInt ( string, radix )" }],
  ["parseFloat", { to: "ToString", sec: "ECMAScript §19.2.4 parseFloat ( string )" }],
]);

/* The conversion a read at [from,to) stands under, or null. `subst` is the masker's list of `${ }` interiors;
   `boundHere` is the file's own scope answer, so a corpus that declares its own `String` is not credited with
   the language's. */
function coercionOf(struct, code, from, to, subst, boundHere) {
  /* A CONVERSION APPLIES TO A WHOLE VALUE, AND A READ THAT IS ONLY PART OF ONE IS NOT UNDER IT. This is the
     same question §boolConsumer asks to the LEFT of a `||` — "is anything already consuming this read" — asked
     to the RIGHT, and it is what separates the axis from a listing of the language: in `${JSON.stringify(o.f)}`
     and `"x" + f(o.g)` the substitution and the `+` convert what the CALL returned, and `o.f`/`o.g` never meet
     ToString at all. Asking it first took this category from 536 rows to a population a person can read, and
     every row it removed was a read whose absence the conversion beside it does NOT make plausible. */
  let e = to;
  while (e < struct.length && /\s/.test(struct[e])) e++;
  if (struct[e] === "." || struct[e] === "[" || struct[e] === "(") return null;
  if (struct[e] === "?" && (struct[e + 1] === "." || struct[e + 1] === "[" || struct[e + 1] === "(")) return null;
  const spans = (a, b) => spansOf(struct, a, b);

  /* §13.3.3 EvaluatePropertyAccessWithExpressionKey, whose key goes through §7.1.21 ToPropertyKey ( arg ) and
     from there to §7.1.19 ToString. `o[x.f]` with `f` absent keys the object at `undefined`, and on the WRITE side
     `o[x.f] = v` mints that key ON the object — the token then exists, is enumerable, and is what a later
     lookup of a genuinely-absent field finds. The `[` must be a MEMBER accessor and not an array literal,
     which is what the character in front of it says. */
  const ob = enclosingBracketKey(struct, from);
  if (ob >= 0) {
    const cb = matchAt(struct, ob);
    if (cb > 0) {
      const [x, y] = spansOf(struct, ob + 1, cb - 1);
      if (x === from && y === to)
        return { to: "ToString", how: "a computed property key", use: "a property key",
                 sec: "ECMAScript §7.1.21 ToPropertyKey ( arg )" };
    }
  }

  /* §13.2.8.5 — the substitution's value is handed to ToString by the template's own evaluation, so the read
     must BE the substitution expression and not merely stand inside it. */
  for (const [a, b] of subst) {
    const [x, y] = spans(a, b);
    if (x === from && y === to)
      return { to: "ToString", how: "a `${ }` template substitution", use: "a value",
               sec: "ECMAScript §13.2.8.5 Runtime Semantics: SubstitutionEvaluation" };
  }

  /* §19.2 and the two constructors called as functions — the read must BE the FIRST argument, which is the one
     whose conversion those clauses state, and the callee must be the language's global rather than a member
     (`x.String(…)`) or a name this file binds for itself. */
  const open = enclosingCallParen(struct, from);
  if (open > 0) {
    const head = Math.max(0, open - 40);
    const w = /([A-Za-z_$][\w$]*)\s*$/.exec(code.slice(head, open));
    if (w && GLOBAL_CONVERSION.has(w[1]) && !boundHere(w[1])) {
      const before = struct[head + w.index - 1] || " ";
      const close = matchAt(struct, open);
      if (before !== "." && before !== "?" && close > 0) {
        const first = splitTop(struct, open + 1, close - 1)[0];
        if (first) {
          const [x, y] = spans(first[0], first[1]);
          if (x === from && y === to) {
            const g = GLOBAL_CONVERSION.get(w[1]);
            /* The coerced EXPRESSION is the whole call, so the identity question is asked of that span and not
               of the read — `String(x.f) === k` compares the token, and `x.f === k` compares the field. */
            const idAt = head + w.index;
            return { to: g.to, how: `the argument of \`${w[1]}()\``, sec: g.sec,
                     use: isEqualityOperand(struct, idAt, matchAt(struct, open)) ? "an equality operand" : "a value" };
          }
        }
      }
    }
  }

  /* §13.15.3 with a String on one side — ToString of BOTH, and the string literal in the chain is what says so. */
  if (plusChainString(struct, from, to))
    return { to: "ToString", how: "an operand of a `+` chain holding a string literal", sec: ARITH_SEC, use: "a value" };

  /* The arithmetic operators, asked to the right and then to the left. A compound assignment (`o.f *= 2`) is
     skipped: it is a read-modify-write of the record's OWN field, and the value it mints it also stores, which
     is a different sentence than the one this category makes. */
  let r = to;
  while (r < struct.length && /\s/.test(struct[r])) r++;
  for (const op of ARITH_OPS) {
    if (struct.slice(r, r + op.length) !== op) continue;
    if (struct[r + op.length] === "=") continue;                 /* `*=`, `+=`, `-=`, `%=`, `**=` */
    if (op.length === 1 && struct[r + 1] === op) continue;       /* `**`, `++`, `--` — a longer operator */
    return { to: arithTo(op), how: `the left operand of \`${op}\``, sec: ARITH_SEC, use: "a value" };
  }
  let l = from;
  while (l > 0 && /\s/.test(struct[l - 1])) l--;
  for (const op of ARITH_OPS) {
    if (struct.slice(l - op.length, l) !== op) continue;
    if (op.length === 1 && struct[l - op.length - 1] === op) continue;   /* the tail of `**`/`++`/`--` */
    /* UNARY OR BINARY IS A QUESTION ABOUT WHAT PRECEDES THE OPERATOR, and it changes which clause applies:
       §13.5.4 Unary + Operator converts one operand, §13.15.3 converts two. `x = -o.f` and `a - o.f` are the
       same character in two productions. */
    let b = l - op.length;
    while (b > 0 && /\s/.test(struct[b - 1])) b--;
    const binary = b > 0 && /[\w$)\]"'`]/.test(struct[b - 1]);
    if (!binary && op === "+")
      return { to: "ToNumber", how: "the operand of a unary `+`", sec: "ECMAScript §13.5.4 Unary + Operator", use: "a value" };
    if (!binary && op === "-")
      return { to: "ToNumber", how: "the operand of a unary `-`", sec: "ECMAScript §13.5.5 Unary - Operator", use: "a value" };
    if (!binary) continue;
    return { to: arithTo(op), how: `the right operand of \`${op}\``, sec: ARITH_SEC, use: "a value" };
  }
  return null;
}

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
   AMBIGUOUS category below, named with its place, because whether a receiver is an endpoint or an AST node is
   a question this cannot answer and answering it either way is the guess the whole file is built against. */
const shapes = new Map();   // "file:offset" -> Set(name)
const shapeOf = (id) => { let s = shapes.get(id); if (!s) shapes.set(id, (s = new Set())); return s; };

/* A BODY THAT EMITS KEYS INTO A BUFFER IT DID NOT DECLARE IS A FRAGMENT OF ITS CALLERS' RECORD, NOT A RECORD.
   "One C function's emissions are one record" is true of a body that OWNS the buffer, and json_buf.h states
   which one that is in its own first sentence — "Zero-initialise one (`JsonBuf b = { 0 };`)". A body holding no
   such declaration is writing into a `JsonBuf *` it was HANDED, so its keys are fields of whatever record the
   caller is composing, and reading it as a record of its own splits one emitted shape in two. Both halves then
   answer the anchor question wrongly and in opposite directions: a consumer of the whole record anchors to
   whichever half it reads more of and every field of the OTHER half becomes a name its record does not emit.
   Derived from the declaration rather than from a list of helper names, so a second fragment written tomorrow
   is a fragment on the day it is written. */
const cBodies = [];   // {id, file, name, root, calls:Set(name)}

const fields = new Map();   // name -> {writes:[site], reads:[site]}
const markers = new Map();  // "@TAG" -> {writes:[site], reads:[site]}
const rec = (map, name) => {
  let e = map.get(name);
  if (!e) map.set(name, (e = { writes: [], reads: [] }));
  return e;
};

const defaulted = [];   // {file,line,name,form,recv}
/* A read whose ABSENCE a conversion turns into a value — §the COERCION. Held apart from `defaulted` because
   the two are different facts about the line and the remedy differs: a default is deleted, a coercion is
   ASSERTED before it happens. Every row carries the conversion, the construct that applied it and the clause
   that defines it, so a reader can disagree with the classification without opening the standard. */
const coerced = [];     // {file,line,name,recv,shape,to,how,sec,where}
/* A read a `||`/`??` follows WITHOUT defaulting it, because a boolean-producing operator already consumed the
   value — see §boolConsumer. Printed in full for the same reason DECIDED PLATFORM is: a decided negative nobody
   can see is the concealment this file exists to report, performed on its own output. Every row names the
   operator that decided it, so every row is somewhere a reader can disagree. */
const decidedOperand = [];  // {file,line,name,recv,form,by}

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

/* The emission vocabulary LIBC SUPPLIES. `printf`/`fprintf`/`js_printf` write the seam directly;
   `snprintf`/`sprintf` build a buffer whose fate is decided below. These are compiled in because they cannot
   go missing; the names this FORK declares are DERIVED instead, below, off the attribute the compiler already
   reads — a hand-kept list cannot hold them and did not.
   THIS COMMENT USED TO END "anything else that could carry a JSON key is a refusal, not an assumption", AND
   THAT WAS THE WHOLE OF HOW THE DEFECT SURVIVED. It was not true and nothing made it true: a call to a
   format-taking function outside these two lists matches no loop here, so its keys were neither read NOR
   refused — they were dropped in silence, while the sentence promised a reader that the third case was
   impossible. A completeness claim a scan does not enforce is worse than no claim, because it is exactly the
   sentence that stops the next person looking. What holds now is narrower and checkable: a JSON key can reach
   this seam through libc's names, through the key entry, or through a declaration carrying `APICLIENT_PRINTF`,
   and the derivation of that third set prints itself beside the corpus so its collapse is legible. */
/* core/json_buf.h's writer is NOT in this list, and that is the fix rather than an omission. Its entries name
   their ROLE — `json_buf_key` a field name, `json_buf_raw` structure and formatted values, `json_buf_str` a
   string value — so which one a call is is a fact about the CALLEE and not about what its argument happens to
   hold. The key entry is a macro that concatenates its argument with the quotes and the colon, so a non-literal
   there does not compile; the other two cannot carry a name at all. Read below as its own construct. */
const C_EMIT = { printf: 0, fprintf: 1, js_printf: 0 };
/* The key entry's argument is a BARE NAME, not a JSON fragment — `json_buf_key(&b, "url")`. It is read as a
   declaration of exactly that field, which is the whole return on the split: one argument, one name, nothing to
   parse out of a string. A non-literal cannot reach here (it is a syntax error), so an argument this cannot
   resolve is a name hiding behind a MACRO, and that is refused with its place like any other unreadable
   construct rather than guessed at. */
const C_KEY = "json_buf_key";
/* And the other half of the same contract, checked rather than assumed: the raw entry MUST NOT carry a field
   name. The compiler forbids a computed key; nothing but this forbids a literal one written the old way, and
   without it the split is a convention that decays the first time somebody writes `json_buf_raw(&b, ",\"x\":")`
   because it is one call instead of two. */
const C_RAW = "json_buf_raw";
const rawKeys = [];   // {file,line,keys,text}
const C_BUILD = { snprintf: 2, sprintf: 1 };
const C_MATCH = ["strstr", "strcasestr", "strncmp", "strcmp", "memmem", "strnstr"];

/* AND THE FORMAT-TAKING FUNCTIONS THIS FORK DECLARES FOR ITSELF, WHICH A HAND-KEPT LIST IS STRUCTURALLY UNABLE
   TO HOLD. Every name above is libc's, and libc is not where this engine's documents are composed:
   solver/compose.h's `composef` sizes a document by what it writes and RETURNS it on the heap, and it carries
   every JSON key the result document and the quantum census emit. A vocabulary typed here could not know that,
   and did not — so a key composed through it was READ by a consumer and emitted, as far as this gate could
   tell, by nobody. That produced a whole category of confident false red: the extension's result-document
   reader asserts each of those names field for field, exactly as §Architecture demands, and was reported as
   the defect that rule exists to catch.
   THE QUIETER HALF IS WHY THIS IS A ROOT FIX AND NOT A SPELLING ONE. The keys of that SAME composition which
   did NOT appear in the report were not seen either; they were EXCUSED, each by an unrelated name that
   happened to collide with it — a `_cold:` on a bridge resolver record, a `securitySinks:` on a merge record,
   a `printf` in the test driver. A collision is not a producer, so this gate's silence about that seam was
   never a clean bill, and a rename on either side of it would have passed.
   SO THE VOCABULARY IS DERIVED FROM THE DECLARATION THE ENGINE ALREADY OBEYS, exactly as idlgen reads the real
   `.idl` rather than a table of member names somebody typed, and for the same reason: a restated rule is a
   second copy, and the copy that drifts is the one nobody runs against reality. check.h's
   `APICLIENT_PRINTF(f, a)` IS this tree's statement that a function takes a printf format at argument `f`, and
   compose.h says at its own declaration that the attribute is load-bearing rather than decoration. It answers
   WHERE THE FORMAT IS. It does not answer where the composed text GOES, so that is read off the declarator's
   TYPES, which do:
     a `char *` RETURN            the document IS the result. Its role then follows the identifier it is
                                  assigned to, exactly as a build destination's does; a composition that is
                                  RETURNED has no name in this file for a matcher to hold, so it is a write.
     a non-const `char *` at 0    an out-parameter, identical in every respect to `snprintf`, so it joins THAT
                                  vocabulary rather than getting a second one that could disagree with it.
     neither                      every parameter is unwritable and the result is not text, so the composed
                                  bytes have no destination the caller can name. That is DECIDED by the types
                                  and is not a refusal — check.h's release-mode format check exists precisely
                                  to compose nothing, and refusing it would report the language as a hole. */
const C_COMPOSE = {};   /* name -> format argument index; the destination is the RETURN value */

/* A KEYWORD BEFORE A PARENTHESIS IS THE LANGUAGE, NOT A DECLARATOR, and the distinction is load-bearing here
   rather than tidy: the declaration span reaches back to the end of the previous statement, so the `} while
   (0)` that closes the macro above check.h's own release-mode format check sits inside it. Read as a
   declarator that is a SECOND candidate, and a declaration this file can read perfectly becomes a refusal —
   the scan reporting the language as a place a field name might be hiding, which is the failure `idl_installed`
   records in the opposite direction. Every name here can be followed by `(` and none of them can be declared. */
const C_NOT_A_DECLARATOR = new Set(["while", "if", "for", "switch", "return", "sizeof", "do", "else",
                                    "case", "defined", "catch", "_Alignof", "_Static_assert"]);

/* WHERE A RETURNED DOCUMENT WENT, asked of the construct at the call and never of a dataflow. `out = composef(`
   names a destination whose role the file's own matcher operands decide, and `return composef(` hands the
   bytes out of this translation unit, where nothing here can match on them. Anything else — a composition
   passed straight into another call — is a place this cannot say whether the text is emitted or looked for,
   and it is refused with its line rather than guessed at. */
function composeDest(code, struct, at) {
  let i = at - 1;
  while (i >= 0 && /\s/.test(struct[i])) i--;
  if (i < 0) return null;
  if (struct[i] === "=" && (i === 0 || !"=!<>+-*/%&|^".includes(struct[i - 1]))) {
    let j = i - 1;
    while (j >= 0 && /\s/.test(struct[j])) j--;
    const end = j + 1;
    while (j >= 0 && /[\w$]/.test(struct[j])) j--;
    const name = code.slice(j + 1, end);
    return /^[A-Za-z_]\w*$/.test(name) ? { kind: "name", name } : null;
  }
  const end = i + 1;
  let j = i;
  while (j >= 0 && /[\w$]/.test(struct[j])) j--;
  return code.slice(j + 1, end) === "return" ? { kind: "return" } : null;
}

/* THE DECLARATION SPAN THE ATTRIBUTE ATTACHES TO. It is written BEFORE the declarator (check.h) or AFTER it
   (compose.h), so the span — bounded by the previous statement end or preprocessor line and by the `;` or `{`
   that ends this one — is what holds both spellings without this file guessing which one it is looking at.
   Inside it, exactly ONE parenthesised group other than the attribute's own may be preceded by an identifier:
   that identifier is the function and that group is its parameter list. Zero or several is a declaration this
   cannot read, and it is refused with its place — a format-taking function credited to the wrong name would
   attribute a whole document's keys to something that never emits one. */
function collectCFormatDecls(file, src) {
  const { code, struct } = maskC(src);
  if (bracketFault(struct)) return;   /* scanC refuses this file by name; one report of the same fault is enough */
  for (const c of callSites(struct, "APICLIENT_PRINTF")) {
    const at = c.at, lineStart = src.lastIndexOf("\n", at - 1) + 1;
    /* The attribute's own definition names its PARAMETERS where a use names positions — the same construct
       the field-name entry already skips, and for the same reason: reading it as a use makes the writer of
       the rule its own first violation. */
    if (/^\s*#\s*define\s+$/.test(code.slice(lineStart, at))) continue;
    if (!c.args || c.args.length !== 2 || c.close < 0) {
      refuse(file, lineOf(src, at), "an APICLIENT_PRINTF( whose two positions cannot be delimited — which argument carries the format is what decides every key the declaration beside it emits");
      continue;
    }
    const f = Number(code.slice(c.args[0][0], c.args[0][1]).trim());
    if (!Number.isInteger(f) || f < 1) {
      refuse(file, lineOf(src, at), "an APICLIENT_PRINTF( whose format position is not a literal integer", code.slice(c.args[0][0], c.args[0][1]));
      continue;
    }
    let lo = lineStart;
    for (let i = at - 1; i >= 0; i--) {
      const ch = struct[i];
      if (ch === ";" || ch === "}" || ch === "{") { lo = i + 1; break; }
      if (ch === "\n") {
        const ls = i + 1;
        let k = ls; while (k < at && /[ \t]/.test(struct[k])) k++;
        if (struct[k] === "#") { lo = i + 1; break; }
      }
      if (i === 0) lo = 0;
    }
    let hi = struct.length;
    for (let i = c.close; i < struct.length; i++)
      if (struct[i] === ";" || struct[i] === "{") { hi = i; break; }
    const groups = [];
    for (let i = lo; i < hi; i++) {
      if (struct[i] !== "(" || i === c.open) continue;
      const close = matchAt(struct, i);
      if (close < 0 || close > hi + 1) continue;
      let j = i - 1;
      while (j >= lo && /\s/.test(struct[j])) j--;
      const end = j + 1;
      while (j >= lo && /[\w$]/.test(struct[j])) j--;
      const name = code.slice(j + 1, end);
      if (/^[A-Za-z_]\w*$/.test(name) && !C_NOT_A_DECLARATOR.has(name)) groups.push({ name, nameAt: j + 1, open: i, close });
      i = close - 1;
    }
    if (groups.length !== 1) {
      refuse(file, lineOf(src, at), `an APICLIENT_PRINTF( beside ${groups.length === 0 ? "no" : groups.length} function declarator(s) — which function takes the format is not readable here`);
      continue;
    }
    const g = groups[0], fmtIdx = f - 1;
    const params = splitTop(struct, g.open + 1, g.close - 1);
    if (!params[fmtIdx]) {
      refuse(file, lineOf(src, at), `an APICLIENT_PRINTF( naming argument ${f} of a declarator that has fewer`);
      continue;
    }
    const before = code.slice(lo, g.nameAt).replace(/\s+/g, " ").trim();
    if (/\bchar\s*\*\s*$/.test(before)) { C_COMPOSE[g.name] = fmtIdx; continue; }
    let outIdx = -1;
    for (let p = 0; p < fmtIdx; p++) {
      const t = code.slice(params[p][0], params[p][1]);
      if (/\bchar\s*\*/.test(t) && !/\bconst\b/.test(t)) { outIdx = p; break; }
    }
    if (outIdx === 0) { C_BUILD[g.name] = fmtIdx; continue; }
    if (outIdx > 0) {
      refuse(file, lineOf(src, at), `an APICLIENT_PRINTF( on a composer whose writable destination is argument ${outIdx} — the build side reads a destination at argument 0`);
      continue;
    }
    /* Decided, not refused: nothing in this signature can carry the composed bytes anywhere. */
  }
}

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
  /* THE BUFFERS THIS SCAN HAS ALREADY READ. `snprintf(line, sizeof line, "{\"n\":%d}", n)` then `printf(line)`
     is ONE emission written in two statements, and refusing over the second would be this file counting its own
     resolved answer as a hole. An emission whose format is a plain identifier that is a resolved build
     destination in this file therefore carries text already accounted for, which is a DECIDED negative on
     exactly the rule the build side already uses: nothing but the destination's other use in the file tells
     these apart, and that use is a construct rather than a dataflow. */
  const builtDests = new Set();
  for (const [fn, fmtIdx] of Object.entries(C_BUILD))
    for (const c of callSites(struct, fn)) {
      if (!c.args || !c.args[fmtIdx] || !c.args[0]) continue;
      const f = c.args[fmtIdx];
      if (cLiteral(code, struct, f[0], f[1]) === null && !cTernaryArms(code, struct, f[0], f[1])) continue;
      const dst = code.slice(c.args[0][0], c.args[0][1]).trim();
      if (/^[A-Za-z_]\w*$/.test(dst)) builtDests.add(dst);
    }
  /* A RETURNING COMPOSER'S DESTINATION IS ITS ASSIGNMENT TARGET, and it belongs in the same set for the same
     reason: `out = composef(…)` then `printf(out)` is one emission written in two statements, and refusing
     over the second would be this file counting its own resolved answer as a hole. */
  for (const [fn, fmtIdx] of Object.entries(C_COMPOSE))
    for (const c of callSites(struct, fn)) {
      if (!c.args || !c.args[fmtIdx]) continue;
      const f = c.args[fmtIdx];
      if (cLiteral(code, struct, f[0], f[1]) === null && !cTernaryArms(code, struct, f[0], f[1])) continue;
      const d = composeDest(code, struct, c.at);
      if (d && d.kind === "name") builtDests.add(d.name);
    }
  const site = (off) => ({ file, line: lineOf(src, off) });

  /* ONE C FUNCTION'S EMISSIONS ARE ONE RECORD. endpoint_json_array writes its object one `json_buf_key` at a
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

  /* THE DECLARATOR IN FRONT OF THE BODY NAMES IT, and the body's own text says whether it owns its buffer. */
  for (const [a, b] of bodies) {
    const before = struct.slice(Math.max(0, a - 400), a);
    const open = before.lastIndexOf("(");
    const decl = open < 0 ? "" : before.slice(0, open);
    const nm = /([A-Za-z_]\w*)\s*$/.exec(decl);
    const body = struct.slice(a, b);
    const calls = new Set();
    let cm;
    const CALL = /([A-Za-z_]\w*)\s*\(/g;
    while ((cm = CALL.exec(body))) calls.add(cm[1]);
    cBodies.push({ id: `${file}:${lineOf(src, a)}`, file, name: nm ? nm[1] : null,
                   root: /\bJsonBuf\s+[A-Za-z_]\w*\s*(?:=|;)/.test(body), calls });
  }

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

  /* THE FIELD-NAME ENTRY. One argument, one name — the macro turns `json_buf_key(&b, "url")` into the quoted
     key, so there is no fragment to parse and no value it could have been instead. */
  for (const c of callSites(struct, C_KEY)) {
    /* THE ENTRY'S OWN DEFINITION IS NOT A USE OF IT. `#define json_buf_key(b, name) …` names its PARAMETER
       where a call names a field, and reading it as a call made the writer of the rule its own first violation
       — the scan reporting its own definition as the one place a name was hiding. A macro definition is the
       text from `#define` to the identifier, which is a construct and not a guess. */
    if (/^\s*#\s*define\s+$/.test(code.slice(src.lastIndexOf("\n", c.at) + 1, c.at))) continue;
    if (!c.args || !c.args[1]) { refuse(file, lineOf(src, c.at), `a ${C_KEY}( with no name argument to read`); continue; }
    const a = c.args[1];
    const name = cLiteral(code, struct, a[0], a[1]);
    if (name === null) {
      /* A non-literal is a SYNTAX ERROR at this entry, so what reaches here is a literal spelled through a
         macro — readable by a compiler and not by this. Refused with its place, exactly as an unresolvable
         format is: the point of the entry is that a name is readable off the source, and a name this cannot
         read is the one remaining hiding place rather than a name to guess at. */
      refuse(file, lineOf(src, c.at), `a ${C_KEY}( whose name is not a literal in this file — a field name behind a macro`,
             code.slice(a[0], Math.min(a[1], a[0] + 60)).trim());
      continue;
    }
    rec(fields, name).writes.push(site(a[0]));
    cEmitted.add(name);
    shapeOf(bodyOf(a[0])).add(name);
  }

  /* AND THE HALF THAT KEEPS THE OTHER ONE TRUE. The raw entry writes structure and formatted values; a field
     name in it is the pre-split spelling coming back, and it is a DEFECT rather than a refusal because it is
     perfectly readable — this is not a construct the scan cannot decide, it is one the producer must not
     write. Non-literals are ignored on purpose: raw bytes are the entry's job. */
  for (const c of callSites(struct, C_RAW)) {
    if (!c.args || !c.args[1]) continue;
    const a = c.args[1];
    const lit = cLiteral(code, struct, a[0], a[1]);
    if (lit === null) continue;
    const ks = keysIn(lit);
    if (ks.length) rawKeys.push({ file, line: lineOf(src, a[0]), keys: ks, text: lit.slice(0, 60) });
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

  /* THE RETURNING COMPOSER, read exactly like the build side above and differing in one place only: its
     destination is a construct at the CALL rather than an argument in it. A composition that is RETURNED is a
     write because nothing in this file holds a name for it; one assigned to an identifier is a write unless
     that identifier is a matcher operand, which is the same question the build side asks of `snprintf`. */
  for (const [fn, fmtIdx] of Object.entries(C_COMPOSE))
    for (const c of callSites(struct, fn)) {
      if (!c.args) { refuse(file, lineOf(src, c.at), `an unbalanced ${fn}( — the composed text cannot be delimited`); continue; }
      const a = c.args[fmtIdx];
      if (!a) { refuse(file, lineOf(src, c.at), `a ${fn}( with too few arguments to hold a format`); continue; }
      let lit = cLiteral(code, struct, a[0], a[1]);
      if (lit === null) {
        const arms = cTernaryArms(code, struct, a[0], a[1]);
        if (!arms) { pendingUnresolved.push({ file, line: lineOf(src, c.at), fn, text: code.slice(a[0], Math.min(a[1], a[0] + 60)) }); continue; }
        lit = arms.join("");
      }
      if (!keysIn(lit).length) continue;
      const d = composeDest(code, struct, c.at);
      if (!d) {
        refuse(file, lineOf(src, c.at), `a ${fn}( carrying a JSON key to a destination this cannot name — whether the document is emitted or matched is not decidable here`);
        continue;
      }
      emit(lit, a[0], d.kind === "return" || !patterns.has(d.name));
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

/* THE NORMALIZATION EVERY RECEIVER TEXT IS PRODUCED BY, AND THE ONE WHITESPACE RUN IT MUST NOT DELETE.
 * A receiver is anchored by its TEXT, so this function is what decides which two spans are one receiver, and
 * deleting every whitespace run makes that decision wrong in exactly one place. ECMAScript §12 ECMAScript
 * Language: Lexical Grammar: "The source text is scanned from left to right, repeatedly taking the longest
 * possible sequence of code points as the next input element." An IdentifierName (§12.7.1 Identifier Names) is
 * therefore a MAXIMAL run of identifier code points, so a whitespace run flanked by two of them is the only
 * thing holding two tokens apart — and deleting it does not collapse a spelling, it MINTS AN IDENTIFIER.
 * `await session(x)` becomes the text `awaitsession(x)`, which is the text a receiver actually spelled
 * `awaitsession(x)` produces; `new Set()` becomes `newSet()`, `typeof m` becomes `typeofm`, `mid in methods`
 * becomes `midinmethods`. Every consumer downstream then asks IDENTIFIER questions of that text — `keyOf`
 * qualifies it by the binder of its LEADING NAME, `originHead` and `ifaceOfExpr` strip a leading `await `,
 * `initOf` looks a name up — so a fused text is a name that identifies the wrong thing, which is the same
 * defect as a `;` inside a string literal read as a statement boundary, one token class over.
 *
 * SO A RUN IS KEPT AS ONE SPACE WHERE BOTH SIDES ARE IDENTIFIER CODE POINTS AND DELETED EVERYWHERE ELSE, and
 * that is decidable from the text with no parser, because it is a question about two characters. It cannot
 * mis-decide in either direction, and both halves are facts about the lexical grammar rather than heuristics:
 * KEEPING one never merges anything, since a space is not an identifier code point and the longest match ends
 * at it either way; DELETING one never merges two IDENTIFIERS, since a punctuator (§12.8 Punctuators) is built
 * of code points no IdentifierName may contain, so whatever stands beside it begins its own token.
 * WHAT THAT SECOND HALF DOES NOT SAY is that deleting is lossless for PUNCTUATORS — `a + + b` and `a++b` are
 * two token streams with one text, because a punctuator CAN be continued into a longer punctuator. That is
 * untouched here and unrealized: no whitespace run this corpus produces stands between two code points whose
 * concatenation is a punctuator at all.
 *
 * THE SIDE ASKED IS `struct`, NOT `code`, AND THAT IS THE WHOLE OF WHY THIS STAYS A NORMALIZATION. The claim
 * above is about the TOKEN STREAM, and `struct` is this file's token-stream view: a literal's interior is
 * blanked there, so whitespace inside one is not flanked by identifier code points and is collapsed away
 * exactly as it always was. Whitespace inside a literal is DATA and not a separator, which is a different
 * question with a different answer, and answering it here would answer it halfway — the space in `f("a b")`
 * would survive while the one in `f("a - b")` would not.
 * RESIDUAL, NOT COVERED: two DISTINCT string literals whose interiors differ only in whitespace still produce
 * one text, so `f("a b")` and `f("ab")` anchor as one receiver. The next diff makes a literal's interior
 * survive this normalization WHOLE rather than collapsed, which changes what a receiver's display text is and
 * not this rule. Its absence shows as one `byRecv` group whose rows quote two different literal arguments at
 * two lines. Unrealized at this revision: over every text all three producers below emit, no two spans that
 * are different token streams collapse onto one text. */
function normExpr(codeSpan, structSpan) {
  /* `blank` writes a space per masked code point, so the two views are the same length and an identifier code
     point in `struct` is that same code point in `code` at that same offset. A caller that sliced them at two
     different offsets would make every answer below a guess, which is the one thing worth crashing over. */
  if (codeSpan.length !== structSpan.length) throw new Error("normExpr: the code and struct spans are not the same span");
  let out = "";
  for (let k = 0; k < codeSpan.length; ) {
    if (!/\s/.test(codeSpan[k])) { out += codeSpan[k]; k++; continue; }
    let j = k;
    while (j < codeSpan.length && /\s/.test(codeSpan[j])) j++;
    if (k > 0 && j < codeSpan.length && /[\w$]/.test(structSpan[k - 1]) && /[\w$]/.test(structSpan[j])) out += " ";
    k = j;
  }
  return out;
}

/* A receiver is normalized to its SOURCE TEXT by §normExpr. Two reads anchor to one record only when
   they are written identically — an aliasing question this deliberately does not answer, because answering it
   by flowing the fact along assignments and into parameters is what made idl_installed.mjs's second solve
   report a union of every caller's object. Identical text is a fact; a flowed identity is an inference. */
/* `out`, when given, receives `.start` — the offset the receiver EXPRESSION begins at. A caller that has to ask
   what stands to the LEFT of the whole read needs that offset and cannot recover it from the text: the text is
   whitespace-collapsed and appears many times in a file, so searching for it would answer about some other
   occurrence. It is recorded only where a receiver was actually produced. */
/* AND `out.text` IS THE WALK'S OWN FACT, WHICH IS NOT THE SAME THING AS ITS ANSWER. This walk is asked by two
 * callers with two different questions, and for a long time it answered both with the stricter one's answer.
 * §the READ question is "does a RECORD cross a seam as this receiver", and for a `|| <literal>` head the answer
 * is no — the read is the substituted literal's member. §the CALLEE question is "what expression is being
 * CALLED here", asked where a function literal is an argument and the callee is what types its parameter, and
 * for that same head the answer is the text: `(n.members || []).some` calls Array.prototype.some, and the
 * elements it hands the callback are the elements of `n.members`, which §the ORIGIN of a value resolves through
 * the same `originHead` that strips a literal alternative for exactly this reason.
 *
 * The cost of collapsing them was silent in the direction §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS names: the
 * callee site got a DECIDED NEGATIVE (`""`, not null — the walk found the expression and decided it), read it
 * as falsy, and recorded no callee at all, so every callback passed to a method of a `|| []` head fell out of
 * `paramSlot` and its parameter had no producer to name. The refusal carried a reason about the OTHER question.
 *
 * So the WALK yields the fact — the primary expression's own normalized text — and each caller asks its own
 * question of it. `.text` is set wherever the walk completed a primary expression, decided negatives included,
 * and NOT where it stopped early (a string/template/regexp head, where nothing was normalized) or where the
 * span crossed a `;` IN THE STRUCTURE and is therefore not one expression — a `;` inside a literal is a datum
 * and terminates nothing, which is why that question is asked of `struct` and not of the text. */
function receiverBefore(struct, code, dotAt, out) {
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
  const t = normExpr(code.slice(i, end), struct.slice(i, end));
  /* The only whitespace `t` still carries is a run §normExpr kept because deleting it would have merged two
     tokens, so nothing in `t` can say this span is more than one STATEMENT — a `;` in the STRUCTURE is what
     says that, and see §crossesStatement for why the structure and not `t` is what is asked. */
  const split = crossesStatement(struct.slice(i, end));
  if (out) {
    let st = i;
    while (st < end && /\s/.test(code[st])) st++;
    out.start = st;
    if (!split) out.text = t;
  }
  if (/^[A-Za-z_$]/.test(t)) return split ? null : t;
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
     receiver's text a fact, and the boundary's own methods take none.
     THE SPACE AFTER `await` IS PART OF THE SHAPE, not decoration on it — §normExpr keeps exactly the run that
     separates the keyword from its operand, and matching `await` without it would match a receiver whose base
     is a BINDING NAMED `awaitx`, which is the collision that normalization exists to keep apart. */
  return /^\((?:await )?[\w$.]*?\.[A-Za-z_$][\w$]*\(\)\)$/.test(t) ? t : null;
}

/* The method an awaited-call receiver names, or null — read back off the normalized text so the one place
   that decides the shape is the one place that reads it. */
const awaitedMethod = (recv) => {
  const m = /^\((?:await )?[\w$.]*?\.([A-Za-z_$][\w$]*)\(\)\)$/.exec(recv || "");
  return m ? m[1] : null;
};

/* A FUNCTION WHOSE LAST STATEMENT IS `throw` DOES NOT RETURN, so a catch that calls one is a catch that
   rethrows — and the NAMES of those functions are COMPUTED here rather than listed, because a list is the one
   shape that goes short. This gate already knew it needed the concept: `RETHROW_FATAL` is named in the regex
   below with a paragraph explaining that missing it "reported the most carefully asserted reads in the bridge
   as concealed ones". The same omission was live for `extension/mojo.js`, whose EVERY catch is
   `catch (e) { …; this.conn._crash(e); }` and whose `_crash` ends in `throw e` — the transport's own comment
   says so in as many words ("Both then RETHROW: the assert is the mechanism and a transport that swallowed it
   would be the one place the ONE assertion mechanism is locally disabled"). Reported as swallowing, that was
   23 alarms on the most heavily asserted file in the corpus, and adding `_crash` to the regex would have
   bought the twenty-fourth spelling the same fate. So the question asked is the STRUCTURAL one — does the
   thing this catch calls always throw — and it is asked of the file's own definitions.
   ONLY THE SAME FILE IS CONSULTED, deliberately: a name resolved across files would need a module graph this
   scan does not have, and guessing one would put a plausible answer where §REFUSED demands an unreadable one.
   A cross-file rethrow helper therefore still reports, which is the direction that under-credits rather than
   over-credits — the same asymmetry the reader-crediting paragraph below chooses. */
function alwaysThrowingNames(struct) {
  const names = new Set();
  /* THE NAME IS THE LAST SEGMENT OF THE BINDING, which is what a call site spells: `Connection.prototype._crash`
     is called as `this.conn._crash(…)`, and `[\w$]` cannot cross the dot, so the capture is `_crash` already. */
  const heads = [
    /\bfunction\s+([A-Za-z_$][\w$]*)\s*\(/g,               // function f(…) {…}
    /([A-Za-z_$][\w$]*)\s*=\s*(?:async\s+)?function\b/g,   // …x.y.f = function (…) {…}
    /([A-Za-z_$][\w$]*)\s*:\s*(?:async\s+)?function\b/g,   // { f: function (…) {…} }
  ];
  for (const re of heads) {
    let m;
    while ((m = re.exec(struct))) {
      /* The body is the first brace after the parameter list, and the parameter list is the first paren after
         the head — taken by matching rather than by scanning, so a default value containing a brace cannot
         move the answer. */
      const paren = struct.indexOf("(", m.index + m[0].length - 1);
      if (paren < 0) continue;
      const afterParams = matchAt(struct, paren);
      if (afterParams < 0) continue;
      const open = sig(struct, afterParams, struct.length);
      if (struct[open] !== "{") continue;
      const close = matchAt(struct, open);
      if (close < 0) continue;
      if (endsInThrow(struct.slice(open + 1, close - 1))) names.add(m[1]);
    }
  }
  return names;
}

/* Does this body's LAST top-level statement throw? Scanned from the end so a `throw` in some earlier branch
   does not answer for the whole function — a helper that throws only on one arm still returns on the others,
   and crediting it would let a genuinely swallowing catch through. */
function endsInThrow(body) {
  let end = body.length;
  while (end > 0 && /[\s;]/.test(body[end - 1])) end--;
  let depth = 0, start = 0;
  for (let i = end - 1; i >= 0; i--) {
    const c = body[i];
    if (c === ")" || c === "]" || c === "}") depth++;
    else if (c === "(" || c === "[" || c === "{") { if (--depth < 0) { start = i + 1; break; } }
    else if (depth === 0 && c === ";") { start = i + 1; break; }
  }
  return /^\s*throw\b/.test(body.slice(start, end));
}

/* A swallowing try: its catch body neither rethrows nor asserts, so every read inside the try is a read whose
   absence the consumer has already decided to survive. §Architecture names `a catch {} around a read` in the
   same breath as `|| 0` for exactly this reason. */
function swallowingTrySpans(struct) {
  const rethrowers = alwaysThrowingNames(struct);
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
    /* …AND THE SAME QUESTION ASKED OF WHAT THE BODY CALLS — see `alwaysThrowingNames`. A catch whose whole
       job is to hand the failure to a helper that ends in `throw` re-raises exactly as a literal `throw`
       does, and the helper is what a corpus with a real transport in it will always have. */
    if ([...body.matchAll(/([A-Za-z_$][\w$]*)\s*\(/g)].some((c) => rethrowers.has(c[1]))) continue;
    spans.push([open, close]);
    re.lastIndex = cbEnd;
  }
  return spans;
}

const inSpan = (spans, off) => spans.some(([a, b]) => off >= a && off < b);

/* ---- the type an `instanceof` GUARD states, and the EXTENT over which it states it -------------------------- */

/* A DECLARATION IS NOT ALWAYS A DECLARATOR. Everything §the PLATFORM receiver reads is a construct that names a
 * type at the point the object is CREATED or HANDED OVER — `new URL(x)`, `await fetch(u)`, an IDL-typed callback
 * parameter, the arguments a local helper is called with — and a union-typed parameter has no such construct at
 * all. Fetch §5.6 "Fetch methods"' `fetch(input, init)` takes a `RequestInfo`, which §5.4 "Request class"
 * typedefs as `(Request or USVString)`, and the page's own wrapper of it then asks the question the spec's
 * union leaves open, in the way the language provides:
 * `input instanceof Request ? input.url : String(input)`. That test IS the declaration, and reading it is the
 * same kind of fact as reading a `#define` body — nothing is followed through an assignment, a return or a
 * promise; the guard and the read it guards are ONE expression.
 *
 * WITHOUT IT THE SHAPE ANCHOR ANSWERS, AND IT ANSWERS CONFIDENTLY AND WRONGLY. `input` reads `url` and `method`
 * and `init` reads `body` and `method`; those are names of the capture record `emit()` composes, so two of them
 * intersect an emitted shape and the ≥2 rule anchors a Fetch §5.4 Request to it. §the PLATFORM receiver's own
 * paragraph anticipated exactly that receiver — "a platform object whose members happen to collide with two of
 * an emitted record's names is exactly the receiver the shape rule anchors CONFIDENTLY and wrongly" — and asked
 * `ifaceOfExpr` first to prevent it; the prevention only works where `ifaceOfExpr` can answer, and for a
 * narrowed union it could not. A wrong anchor is worse than an undecided one in both directions at once: the
 * receiver's remaining IDL members are then scored against a record that never had them, and the record's own
 * audit budget is spent on an object that is not it.
 *
 * THE EXTENT IS THE WHOLE OF THE RIGOUR, because `input instanceof Request` says nothing about `input` anywhere
 * else in the body. So a guard carries a SPAN, taken from the construct that scopes it and from nothing else:
 *   - a ternary's CONSEQUENT — `x instanceof R ? x.f : …` — from the `?` to its matching `:`;
 *   - the right operand of `&&`, which is evaluated only where the left held, so the walk steps over `&&` and
 *     keeps looking rather than stopping;
 *   - an `if`'s CONSEQUENT, braced or a single statement, when the test's own paren is what the walk ran out to.
 * A read outside every such span is not narrowed, and a receiver read both inside and outside one is a union
 * this cannot collapse — which is AMBIGUOUS at the anchor, never an answer.
 *
 * NAMED RESIDUAL — THE NEGATED EARLY-RETURN GUARD. `if (!(x instanceof R)) return;` narrows `x` for the rest of
 * the body, and nothing here reads it: that is CONTROL FLOW rather than a construct, and every other rule in
 * this file is lexical. The next diff that wants it builds a "the statement list after a test whose every arm
 * leaves the block" primitive — `alwaysThrowingNames` is the same shape of question already asked once here.
 * Its absence shows as a receiver in AMBIGUOUS, or anchored by shape, whose reads are all members of one
 * interface and whose function opens with a negated `instanceof` test; it never shows as a wrong interface,
 * because a guard this does not see narrows nothing.
 *
 * NAMED RESIDUAL — THE DICTIONARY, WHICH IS THE OTHER HALF OF THE SAME RECEIVER AND IS NOT AN `instanceof`
 * QUESTION AT ALL. `window.fetch = async function (input, init)` binds two parameters, and the one this rule
 * fixes is `input`; `init` is the `RequestInit` declared in Fetch §5.4 Request class, which no test narrows
 * because a DICTIONARY is not a thing anything is an instance of — Web IDL §2.7 Dictionaries defines one as
 * "an ordered map data type with a fixed, ordered set of entries" and says an operation taking one "will
 * perform a one-time conversion from the given JavaScript value", so there is no interface object, no
 * prototype and nothing for `instanceof` to test; the type is stated ONLY by the operation that declares the
 * argument. TWO THINGS WERE MISSING AND THEY STACKED; THE FIRST IS BUILT AND ONLY THE SECOND IS STILL THIS
 * RESIDUAL. `idl_members.mjs` collects `dictionary` now — into its own `dictByName`/`dictInheritanceOf` pair,
 * partials merged as §2.7 requires, with `dictChain` giving that inheritance least-derived first, `dictMembers`
 * giving each member as `{name, required, hasDefault, level, declaredBy, idlType}` in Web IDL §3.2.17
 * "Dictionary types"' read order, and `dictionaryTypesIn` reaching a dictionary nested inside a union or a
 * sequence. So there IS a member list to diff `init`'s reads against, and this file already imports the module
 * that answers it.
 * THE RETIRED HALF IS WRITTEN DOWN RATHER THAN DELETED, BECAUSE ITS REASON STILL READS AS TRUE FROM HERE. This
 * clause used to say the next diff puts "dictionaries into the member map", and `byName`'s kind list really is
 * still `interface`, `interface mixin`, `callback interface` and `namespace` — so a reader re-deriving the
 * retired reason would go and fold them in, which is the one thing `idl_members.mjs` argues at its own site
 * must never happen: `byName.has(x)` is what idlgen asks to decide whether an interface tag names something
 * the corpus declares, and `members()` collects `attribute`/`operation`/`const` and never `field`, so a
 * dictionary sharing that map answers the EMPTY member list and mints a false COMPLETE. The two maps are
 * separate ON PURPOSE; what this residual wants is to READ the dictionary one.
 * WHAT THE NEXT DIFF BUILDS: the arm this file still has none of — one that types the parameters of a function
 * literal ASSIGNED TO a platform member, which is the mirror of §paramSlot's function literal PASSED TO one —
 * and then asks `dictMembers` of whichever declared argument type that arm lands `init` on. ITS ABSENCE SHOWS as
 * `init` reading `body` and `method` — two names of the capture record `emit()` composes — so the ≥2 shape rule
 * anchors it to that record and it appears NOWHERE in this report, because both names have producers; the tell
 * is that a `RequestInit` never reaches DECIDED PLATFORM while the `input` declared beside it now does. A relay
 * that named `input` and `init` as one finding is corrected here: they are two, and only one of them is this. */

/* The `:` matching a ternary `?` at `from`, or -1. Nested ternaries are counted; `?.` and `??` are neither. */
function ternaryColon(struct, from) {
  let d = 0, q = 0;
  for (let i = from; i < struct.length; i++) {
    const c = struct[i];
    if (PAIRS[c]) { d++; continue; }
    if (c === ")" || c === "]" || c === "}") { if (!d) return -1; d--; continue; }
    if (d) continue;
    if (c === "?") { if (struct[i + 1] === "?" || struct[i + 1] === ".") { i++; continue; } q++; continue; }
    if (c === ":") { if (!q) return i; q--; continue; }
    if (c === ";") return -1;
  }
  return -1;
}

/* The opener matching the closer at `close`, or -1. */
function openerOf(struct, close) {
  let d = 0;
  for (let j = close; j >= 0; j--) {
    const c = struct[j];
    if (c === ")" || c === "]" || c === "}") d++;
    else if (c === "(" || c === "[" || c === "{") { d--; if (!d) return j; }
  }
  return -1;
}

/* The span a test ending at `from` narrows, or null — the three constructs named above and nothing else. */
function guardedSpan(struct, code, from) {
  for (let i = from; ;) {
    i = sig(struct, i, struct.length);
    if (i >= struct.length) return null;
    const ch = struct[i];
    if (ch === "&" && struct[i + 1] === "&") { i += 2; continue; }
    if (ch === "?" && struct[i + 1] !== "?" && struct[i + 1] !== ".") {
      const c = ternaryColon(struct, i + 1);
      return c < 0 ? null : [i + 1, c];
    }
    if (ch !== ")") return null;
    const open = openerOf(struct, i);
    if (open < 0) return null;
    let k = open;
    while (k > 0 && /\s/.test(struct[k - 1])) k--;
    const e = k;
    while (k > 0 && /[\w$]/.test(struct[k - 1])) k--;
    if (code.slice(k, e) !== "if") return null;
    const b = sig(struct, i + 1, struct.length);
    if (struct[b] === "{") { const c = matchAt(struct, b); return c < 0 ? null : [b + 1, c - 1]; }
    let e2 = b, d = 0;
    for (; e2 < struct.length; e2++) {
      const c2 = struct[e2];
      if (PAIRS[c2]) d++;
      else if (c2 === ")" || c2 === "]" || c2 === "}") { if (!d) break; d--; }
      else if (c2 === ";" && !d) break;
    }
    return [b, e2];
  }
}

/* Every `X instanceof E` in a file, as {recv, rhs, rhsAt, from, to}. The receiver is normalized by the SAME walk
   a read's receiver is, so the two texts are comparable without a second normalizer; a receiver that walk
   DECIDES against (`""` — a literal head) or cannot read (`null`) narrows nothing, which is the under-crediting
   direction and leaves the read to every other rule here. The RHS is handed on as TEXT: what interface it names
   is a Web IDL question, and it is asked where every other one is — in `ifaceOfExpr`, after the IDL is loaded. */
function instanceofGuards(struct, code) {
  const out = [];
  const re = /\binstanceof\b/g;
  let m;
  while ((m = re.exec(struct))) {
    const recv = receiverBefore(struct, code, m.index);
    if (!recv) continue;
    const r0 = sig(struct, m.index + m[0].length, struct.length);
    let e = r0, d = 0;
    for (; e < struct.length; e++) {
      const c = struct[e];
      if (PAIRS[c]) { d++; continue; }
      if (c === ")" || c === "]" || c === "}") { if (!d) break; d--; continue; }
      if (d) continue;
      if (c === "?" && struct[e + 1] === ".") { e++; continue; }
      if (c === "?" || c === ":" || c === "," || c === ";") break;
      if ((c === "&" || c === "|") && struct[e + 1] === c) break;
    }
    const rhs = code.slice(r0, e).replace(/\s+/g, " ").trim();
    if (!rhs) continue;
    const span = guardedSpan(struct, code, e);
    if (!span) continue;
    /* A NAME REASSIGNED INSIDE ITS OWN GUARD IS NOT GUARDED, which is the SAME unambiguity rule §a binding's
       own initializer applies one construct over: a test states what the name is at the test, and a write
       between there and the read states that it is something else. Asked of every identifier in the chain, so
       `e = other` inside the block disqualifies `e.data`'s guard as well as `e`'s. A write this cannot see
       leaves the guard standing, which is the one direction that could decide wrongly — and it is why the
       question is asked of the file's own assignment primitive rather than of a second walk. */
    if (!(recv.match(/[A-Za-z_$][\w$]*/g) || []).some((n) => assignmentsTo(struct, n, span[0], span[1])))
      out.push({ recv, at: m.index, rhs, rhsAt: r0, from: span[0], to: span[1] });
  }
  return out;
}

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
    let params = null, fnName = null;
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
      /* THE NAME A CALL SITE SPELLS, and only where the word in front of it is `function`. A method shorthand
         `foo(a) { … }` reaches this same shape and is called as `o.foo(…)`, so crediting it would collect a
         DIFFERENT function's arguments under this one's name — the aliasing that makes a plausible answer. */
      const nm = code.slice(k, e);
      let kw = k;
      while (kw > 0 && /\s/.test(struct[kw - 1])) kw--;
      const kwEnd = kw;
      while (kw > 0 && /[\w$]/.test(struct[kw - 1])) kw--;
      if (code.slice(kw, kwEnd) === "function" && /^[A-Za-z_$][\w$]*$/.test(nm)) fnName = nm;
    } else continue;
    const close = matchAt(struct, i);
    if (close < 0) continue;
    spans.push({ open: i, close, params, binds: new Set(), head: params ? params[0] : i, fnName });
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
    /* THE WALK'S FACT, NOT ITS READ-SIDE ANSWER — see §`out.text`. What is wanted here is the text of the
       expression being CALLED, and a `(x || []).some` is being called whatever the read rule says about a
       member of it. Every consumer of `s.callee` asks its own question of the text and refuses what it cannot
       resolve: the IDL arm hands the base to `ifaceOfExpr`, the import arm demands the callee be a plain
       dotted path rooted at an imported binding, and §the ORIGIN of a value strips the alternative with the
       same `originHead` the value question already uses. */
    const out2 = {};
    receiverBefore(struct, code, e2, out2);
    if (!out2.text) continue;
    s.callee = { text: out2.text, at: e2, index };
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

  /* EVERY NAME THIS FILE DECLARES ANYWHERE, WHICH IS A DIFFERENT QUESTION FROM WHICH SCOPE BINDS IT AT A POINT
     — and reading one off the other is §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS. `binderOf` answers "file" both
     for a name declared at the top level and for a name this file never declares at all, and the second is the
     only one that can be an import whose constant is spelled somewhere this cannot look. Collapsing them made
     every top-level `for (let i = …)` index — declared twice in one file, so its declaration is doubtful and
     `initOf` answers null — refuse as if it were an import, which is 77 rows of the language rather than of the
     corpus. The bit is DECLARED-HERE and the two questions are asked of it separately. */
  const bound = new Set();
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
      bound.add(m[0]);
    }
    /* THE PARAMETERS IN ORDER, which is what an IDL argument list is indexed by. A pattern, a rest and a
       default all occupy a POSITION whatever they bind, so a slot this cannot name is held as null rather than
       skipped — dropping it would shift every parameter after it onto the wrong IDL type. */
    s.paramList = splitTop(struct, s.params[0], s.params[1])
      .map(([a, b]) => { const p2 = /^\s*([A-Za-z_$][\w$]*)\s*(?:=|$)/.exec(code.slice(a, b)); return p2 ? p2[1] : null; });
  }
  /* The end of the expression that STARTS at `from`: the first `;` or `,` at depth zero, or the closer that
     ends the construct the expression sits inside. The same walk `inits` already runs over a write's right-hand
     side, named once so the constructs below cannot spell it several ways. Defined HERE, above the declaration
     walk, because §the LEXICAL EXTENT needs it to find where a `for`-head's controlled statement ends. */
  const exprEnd = (from) => {
    let e = from, d = 0;
    for (; e < struct.length; e++) {
      const ch = struct[e];
      if (PAIRS[ch]) d++;
      else if (ch === ")" || ch === "]" || ch === "}") { if (!d) break; d--; }
      else if ((ch === ";" || ch === ",") && !d) break;
    }
    return e;
  };

  /* §the LEXICAL EXTENT — A `const`/`let` DECLARATION IS BOUND BY ITS BLOCK, NOT BY ITS FUNCTION, AND ATTRIBUTING
   * IT TO THE FUNCTION MERGES DISTINCT BINDINGS INTO ONE RECEIVER. This walk used to hand every `const`/`let` to
   * the innermost enclosing FUNCTION body and record the consequence as a residue — "two same-named declarations
   * inside ONE function body still merge". That residue is not a narrowing, it is a WRONG ANCHOR, and it accused
   * correct code: `engine/idlgen.mjs` binds `n` at module top level in `for (const n of idl.declarations)` and
   * again in `for (const n of dictNameds)`, so ONE receiver `n@file` carried six reads of a `webidl2` node and
   * nine of a record this corpus builds. §the ORIGIN of a value answered "the `webidl2` module" for the first
   * six and nothing for the other nine, `agreeOrigin` requires unanimity, and the merged receiver therefore
   * reached the SHAPE anchor — which read the UNION of two objects' field names and reported three `|| []` over
   * a foreign node as DEFAULTED. `engine/route.mjs` is the same defect one band over: `for (const r of
   * reads.values())` and a `const r = JSON.parse(…)` inside a later loop merged, the union anchored on the
   * engine result, and the map record's own five fields were reported OFF-RECORD — with this gate's own output
   * naming their writer, in this corpus, two lines away. A wrong anchor is worse than an undecided one, and an
   * auditor that accuses correct code is the one direction §Architecture rates as unrecoverable.
   *
   * SO THE BINDER IS THE DECLARATION'S OWN EXTENT, WHICH IS WHAT ECMAScript SAYS IT IS. §14.2.2 Runtime
   * Semantics: BlockDeclarationInstantiation ( code, env ) instantiates a block's lexical declarations in that
   * block's environment, and §14.7.5.7 ForIn/OfBodyEvaluation ( lhs, stmt, iteratorRecord, iterationKind,
   * lhsKind, labelSet [ , iteratorKind ] ) does "Let iterationEnv be NewDeclarativeEnvironment ( oldEnv )" per
   * iteration for a `for`-head declaration. `var` and a `function`/`class` declaration are NOT block-scoped and
   * keep the function span they always had. The extent is the innermost enclosing BRACKET: a `{` is the block
   * itself, and a `(` is a `for`/`catch` head whose binding also governs the statement it controls. A
   * declaration with no enclosing bracket is the module's own top level and still answers `"file"`, which is
   * what §the ORIGIN of a value and §GLOBAL_IFACE read as "no scope in this file binds this name". */
  const lexOf = (at) => {
    let d = 0, open = -1;
    for (let x = at - 1; x >= 0; x--) {
      const c = struct[x];
      if (c === ")" || c === "]" || c === "}") d++;
      else if (c === "(" || c === "[" || c === "{") { if (!d) { open = x; break; } d--; }
    }
    if (open < 0) return null;
    if (struct[open] === "{") { const close = matchAt(struct, open); return close < 0 ? null : [open, close]; }
    if (struct[open] !== "(") return null;                 /* a `[` — not a position a declaration can be in */
    const close = matchAt(struct, open);
    if (close < 0) return null;
    const b = sig(struct, close, struct.length);
    return [open, struct[b] === "{" ? matchAt(struct, b) : exprEnd(b)];
  };
  /* Every lexical binding this file introduces, by name, innermost-first — the list `binderOf` walks. */
  const lexBinds = new Map();
  const addLex = (name, at) => {
    const ext = lexOf(at);
    if (!ext || ext[1] < 0) return null;
    if (!lexBinds.has(name)) lexBinds.set(name, []);
    lexBinds.get(name).push({ open: ext[0], close: ext[1] });
    return ext;
  };
  /* A DECLARATION IS A LIST, AND READING ONLY ITS FIRST DECLARATOR LEFT EVERY LATER NAME FREE. `let out = "", i
     = 0, n = src.length` declares three bindings and a first-name-only walk saw one, so `i` and `n` were names
     NO scope in this file bound — which is the answer reserved for an IMPORT, and it is the one answer that
     must never be invented: §receiver scope's own worked example is a binding spelled `location` or `event`
     being read as the global of that name and every read on it credited to Location or Event. The list is
     split at its top-level commas, which is also what folds the destructuring arm in: a second regex for the
     pattern spelling only ever matched the FIRST declarator either, so `const a = 1, { ok } = f()` bound
     neither `ok` nor anything after it. */
  const declaratorsAt = (from) => {
    /* The list runs to the first `;` or unmatched closer at depth zero — `for (let i = 0; …)` ends at the
       semicolon and `for (const x of xs)` at the `)`, which is where each of those declarations ends. */
    let e = from, dd = 0;
    for (; e < struct.length; e++) {
      const ch = struct[e];
      if (PAIRS[ch]) dd++;
      else if (ch === ")" || ch === "]" || ch === "}") { if (!dd) break; dd--; }
      else if (ch === ";" && !dd) break;
    }
    const out = [];
    for (const [a, b] of splitTop(struct, from, e)) {
      const lead = /^\s*([A-Za-z_$][\w$]*)(?![\w$])/.exec(code.slice(a, b));
      if (lead) { out.push({ name: lead[1], at: a + lead[0].length - lead[1].length }); continue; }
      /* A destructuring declarator binds every name in its pattern — `const { ok, out } = f()` binds both, and
         missing them merges those receivers with any same-named binding elsewhere in the file. */
      const ob = sig(struct, a, b);
      if (struct[ob] !== "{" && struct[ob] !== "[") continue;
      const cb = matchAt(struct, ob);
      if (cb < 0 || cb - 1 > b) continue;
      const pt = code.slice(ob + 1, cb - 1);
      let m2;
      ID.lastIndex = 0;
      while ((m2 = ID.exec(pt))) if (pt[m2.index - 1] !== ".") out.push({ name: m2[0], at: ob + 1 + m2.index, pattern: true });
    }
    return out;
  };
  const VAR = /\b(const|let|var)\s+/g;
  const declSites = [];   // {name, at, kwAt, ext} — every binding a `const`/`let`/`var` in this file introduces
  let d;
  while ((d = VAR.exec(struct))) {
    const s = innermost(d.index);
    const lexical = d[1] !== "var";
    for (const n of declaratorsAt(d.index + d[0].length)) {
      /* A LEXICAL DECLARATION IS RECORDED AT ITS OWN EXTENT AND A `var` AT ITS FUNCTION, which is the one
         difference §the LEXICAL EXTENT is about. A lexical declaration with no enclosing bracket is the
         module's top level and has no extent, so it falls through to the span walk exactly as `var` does. */
      declSites.push({ ...n, kwAt: d.index, ext: lexical ? addLex(n.name, d.index) : null });
      bound.add(n.name);
      if (s) s.binds.add(n.name);
    }
  }
  /* The name a `function`/`class` declaration and a `catch` binding introduce, which are not lists. A `catch`
     parameter is lexical and its extent is its own head's paren, so it is asked from INSIDE that paren — asking
     at the `catch` keyword would find the enclosing block and give the binding a scope wider than the clause.
     A `function`/`class` declaration is not block-scoped the way a `const` is and keeps the span it had. */
  const NAMED = /\bfunction\s*\*?\s*([A-Za-z_$][\w$]*)|\bclass\s+([A-Za-z_$][\w$]*)|\bcatch\s*\(\s*([A-Za-z_$][\w$]*)/g;
  while ((d = NAMED.exec(struct))) {
    const name = d[1] || d[2] || d[3];
    const s = innermost(d.index);
    if (d[3]) addLex(name, d.index + d[0].length);
    bound.add(name);
    if (s) s.binds.add(name);
  }
  for (const [, list] of lexBinds) list.sort((a, b) => b.open - a.open);   /* innermost first */
  /* The key a binding is filed under. The two namespaces are kept apart by the `L` — a block whose brace is a
     function's own body brace would otherwise key its `const x` and that function's parameter `x` alike, and
     those are two bindings that cannot both exist but can both be asked. */
  const binderKey = (ext, s) => ext ? `L${ext[0]}` : s ? String(s.open) : "file";

  /* A BINDING'S DECLARATION IS EVERY WRITE THAT NAMES IT, AND A DECLARATOR'S INITIALIZER IS NOT A SPECIAL KIND
   * OF WRITE. `const url = new URL(x)` states what `url` is in the same construct that creates it, and reading
   * that is the same kind of fact as reading a `#define` body — it is not following a value through a call, a
   * parameter or a promise, which is the flowed identity this file refuses everywhere. `var parsed; try {
   * parsed = new URL(x) } catch { return … }` states it just as flatly; the only reason the initializer moved is
   * that the construction can THROW and the catch returns. So the two spellings are ONE construct read one way,
   * and the with-initializer and without-initializer forms no longer have a rule each to disagree with.
   *
   * AND SEVERAL WRITES ARE NOT DOUBT — THEY ARE SEVERAL DECLARATIONS OF ONE IDENTITY, WHICH IS THE STANDARD
   * §localParamSlot ALREADY APPLIES ONE CALL AWAY. That rule reads a parameter's type off EVERY argument passed
   * to it and requires all of them to agree; this read a binding's type off exactly ONE write and refused the
   * moment there were two. Those are two standards for one question, and the stricter one was arbitrary: a
   * binding written `= new URL(a)` here and `= new URL(b)` there has ONE identity and the writes agree about it.
   * Refusing it under-credits, so the receiver lands in AMBIGUOUS with nothing said about why — which is where a
   * `URL` this gate decides at two dozen constructors sat, undecided, because the binding handed to the deny-list
   * gate is assigned once at its declaration and once again after a redirect.
   *
   * A `null` OR `undefined` WRITE NAMES NO INTERFACE AND CONTRADICTS NONE. Web IDL §2.13.27 "Nullable types —
   * T?"'s nullable type is "an IDL type constructed from an existing type (called the inner type), which just
   * allows the additional value null to be a member of its set of values" — the same declaration with the
   * absent value added, and an absent object has no members for a read to anchor to —
   * so `var x = null; try { x = new URL(u) } catch { x = null }` declares a `URL?` and is read as a URL, exactly
   * as a nullable IDL argument is read as its type. It is skipped, never counted as a disagreement.
   *
   * WHAT IS STILL DOUBTFUL, and each is a construct rather than a caution: a name DECLARED TWICE in one scope
   * (which of the two a read means is not decidable here), a COMPOUND or INCREMENT write (`rhs < 0`, a value
   * derived from the old one, which declares nothing), and writes that DISAGREE about the interface they name. */
  const inits = new Map();     // `${scopeKey}\0${name}` -> [declaration text, …], or null once it is doubtful
  /* One regex for both spellings, because the initializer is just the first write and `assignmentSites` finds it
     with all the others. A declaration with no write at all — `for (const n of xs)`, whose binding is the loop's
     and not an assignment's — has no declaration text and is doubtful, which is what it was before this had a
     construct that could see it. */
  for (const q of declSites) {
    const s = innermost(q.kwAt);
    const key = `${binderKey(q.ext, s)}\0${q.name}`;
    if (inits.has(key)) { inits.set(key, null); continue; }   /* two declarations of one name — doubtful */
    /* A DESTRUCTURED NAME'S OWN INITIALIZER IS NOT READABLE HERE — `const { ok } = f()` declares `ok` out of a
       field of `f()`'s answer, which is not an expression this walk can hand back. Its LATER assignments are
       readable, and taking those alone would be a declaration set assembled out of the subset that happened to
       be easy: the false COMPLETE this file refuses everywhere. Doubtful, which is what it was before the walk
       could see the name at all. */
    if (q.pattern) { inits.set(key, null); continue; }
    /* THE WRITES SEARCHED ARE THE ONES INSIDE THIS BINDING'S OWN EXTENT. A `var` and a top-level declaration
       have the function span (or the file) for an extent, which is what every declaration had before. */
    const span = q.ext ? q.ext : s ? [s.open, s.close] : [0, struct.length];
    const writes = assignmentSites(struct, q.name, span[0], span[1]);
    if (!writes.length || writes.some((w) => w.rhs < 0)) { inits.set(key, null); continue; }
    inits.set(key, writes.map((w) => {
      let e = w.rhs, d = 0;
      for (; e < struct.length; e++) {
        const ch = struct[e];
        if (PAIRS[ch]) d++;
        else if (ch === ")" || ch === "]" || ch === "}") { if (!d) break; d--; }
        else if ((ch === ";" || ch === ",") && !d) break;
      }
      return code.slice(w.rhs, e).replace(/\s+/g, " ").trim();
    }));
  }

  /* The binder of `name` at `off`: the innermost LEXICAL EXTENT that declares it and contains `off`, else the
     innermost enclosing function body that binds it, else the file. */
  const binderOf = (name, off) => {
    for (const b of lexBinds.get(name) || []) if (off >= b.open && off < b.close) return `L${b.open}`;
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

  /* A PARAMETER OF A FUNCTION THIS FILE DECLARES IS TYPED BY THE ARGUMENTS THIS FILE PASSES IT, which is the
   * same kind of fact as `paramSlot` one line up and read at the same place — a CALL SITE — with the type
   * coming from the argument's own construct instead of from an IDL argument list. Nothing is followed through
   * an assignment, a return or a promise. It is the missing half of that rule rather than a new one: the IDL
   * types a parameter the PLATFORM hands a value to, and there was no way at all to type a parameter the FILE
   * hands a value to, so a `URL` and a `Response` this gate decides two dozen times at their constructors were
   * undecidable one call deep — a type-INFERENCE gap, not a gap in the authority.
   *
   * EVERY CALL SITE MUST ANSWER, AND ALL OF THEM MUST AGREE. A helper called with a URL at one site and with
   * something this cannot read at another has no single parameter type, and taking the sites that happen to
   * resolve would be reporting the ones that were easy — a plausible answer assembled out of a subset, which is
   * exactly the false COMPLETE idl_installed.mjs was rewritten to remove. One disagreement, one unreadable
   * argument or one call with too few arguments leaves the parameter undecided, which is AMBIGUOUS: the same
   * under-crediting direction the cross-file rethrow rule chooses.
   *
   * NOT COVERED: a function bound by `const f = (…) => …` or `const f = function (…) …`. Only the
   * `function NAME(` spelling carries its name where this walk already reads one; the bound spellings put the
   * name in a DECLARATOR whose declaration text is the literal, so the next diff joins `inits` — which holds
   * every write's text for a binding — to the span that literal opens and hands `fnName` over from there. ITS ABSENCE SHOWS as a receiver in AMBIGUOUS whose
   * reads are all members of one interface and whose enclosing function is arrow-bound — the same row this
   * rule removes for `function`-declared helpers, still standing beside them. */
  const localParamSlot = (name, off) => {
    for (let s = innermost(off); s; s = innermost(s.open)) {
      if (!s.binds.has(name)) continue;
      if (!s.fnName || !s.paramList) return null;
      const at = s.paramList.indexOf(name);
      /* The declaration's own parameter list is not a call of it — `function f(a, b)` matches `f\s*(` exactly
         as `f(x, y)` does, and reading it as one would resolve the parameter FROM ITSELF. */
      return at < 0 ? null : { fnName: s.fnName, param: at, declParen: s.params[0] - 1 };
    }
    return null;
  };
  const callArgsOf = (fnName, index, declParen) => {
    const out = [];
    for (const c of callSites(struct, fnName)) {
      if (c.open === declParen) continue;
      if (struct[c.at - 1] === ".") continue;      /* `o.fnName(…)` is some other object's member */
      if (!c.args) return null;                    /* an unbalanced call — the argument cannot be delimited */
      const a = c.args[index];
      if (!a || !code.slice(a[0], a[1]).trim()) return null;   /* called with fewer arguments than this slot */
      out.push({ text: code.slice(a[0], a[1]).replace(/\s+/g, " ").trim(), at: a[0] });
    }
    return out;
  };
  /* WHAT A `for … of` ITERATES — the one declaration `inits` cannot read, and it says so: "a declaration with
     no write at all — `for (const n of xs)`, whose binding is the loop's and not an assignment's — has no
     declaration text and is doubtful". That was true of a walk looking for ASSIGNMENTS, and the loop header is
     a construct of its own: `of` is a keyword in that position and the expression after it runs to the header's
     own `)`. It states what the binding IS everywhere in the loop, exactly as an initializer does, so it is
     read at the same place and held to the same standard — TWO loops binding one name in one scope is two
     declarations of one identity and is doubtful, the same answer `inits` gives that case.

     THE LOOP-EXTENT SEARCH THIS USED TO CARRY IS GONE, BECAUSE §the LEXICAL EXTENT MADE IT REDUNDANT. It walked
     every `for (const <name> of …)` head in the file and kept the ones whose BODY contained the read, precisely
     because the binder attributed both loops of a two-loop function to one scope and could not separate them —
     a workaround for the merge, written where the merge was visible. Now the head's own extent IS its binder,
     so `binderOf` at the head and `binderOf` at the read agree for exactly one loop and disagree for every
     other, and the filter one line down is the whole of the answer. Keeping the search beside the fix would be
     a superseded mechanism left as a fallback: it answered a read standing OUTSIDE every loop with the file's
     one remaining head — `engine/route.mjs`'s `const r = JSON.parse(e.str('qjs_result'))` was told it iterated
     `reads.values()` — which is a wrong answer where the binder now gives none. */
  const iterOf = (name, off) => {
    identOnly("iterOf", name);
    const RE = new RegExp(`\\bfor\\s*(?:await\\s+)?\\(\\s*(const|let|var)\\s+${name}\\s+of\\s+`, "g");
    const here = binderOf(name, off);
    const hits = new Set();
    let konst = true, at = -1;   /* the head's own offset — §pushArgsOf's rule, for the same reason */
    let m2;
    while ((m2 = RE.exec(struct))) {
      if (binderOf(name, m2.index + m2[0].length) !== here) continue;
      if (m2[1] !== "const") konst = false;
      const openParen = struct.indexOf("(", m2.index);
      const closeParen = matchAt(struct, openParen);
      if (closeParen < 0) continue;
      hits.add(code.slice(m2.index + m2[0].length, closeParen - 1).replace(/\s+/g, " ").trim());
      at = m2.index + m2[0].length;
    }
    /* SEVERAL LOOPS OVER ONE EXPRESSION ARE SEVERAL DECLARATIONS OF ONE IDENTITY, which is the standard `inits`
       states for a binding's writes — a file that walks `encodings` five times says the same thing five times.
       A `var` head is function-scoped, so two of those in one function still reach this together and two
       different expressions leave the binding undecided, exactly as two disagreeing writes do. */
    const text = hits.size === 1 ? [...hits][0] : null;
    return text === null ? null : { text, konst, at };
  };

  /* WHAT IS PUSHED INTO AN ARRAY — the other half of what an array binding's declaration says. `const decls =
     []` states the TYPE and nothing about the elements; `decls.push(…)` is where every element it will ever
     hold is named, and reading only the initializer answers "an empty array" about a binding whose whole
     purpose is what was put in it. A `...spread` argument contributes the ELEMENTS of its operand and a plain
     one contributes ITSELF, which is the distinction the syntax already draws. Every push must answer and all
     of them must agree, the standard §localParamSlot applies to a call's arguments.

     EACH ARGUMENT CARRIES ITS OWN OFFSET, which is the standard §returnsOf states one screen up and for the
     identical reason: "a caller resolving a name inside the returned expression must resolve it in the SCOPE
     THAT WROTE IT". A push is not where its consumer stands — `idl_members.mjs` pushes `...ast` from inside a
     loop body and the walk that asks about it arrives holding the offset of the `return` statement a hundred
     lines below. While a declaration was filed under its whole FUNCTION the two offsets answered alike and this
     could not be seen; under §the LEXICAL EXTENT the `const ast` inside that loop body is not in scope at the
     `return`, so resolving there answered NOTHING and the whole `webidl2` origin chain went undecided — eleven
     receivers losing a correct DECIDED FOREIGN and one of them landing in READ-WITH-NO-WRITER over a field
     `webidl2` emits. The offset is the fix; the extent only exposed which caller was already asking wrongly. */
  const pushArgsOf = (name, off) => {
    identOnly("pushArgsOf", name);
    const RE = new RegExp(`(^|[^\\w$.])${name}\\s*\\.\\s*push\\s*\\(`, "g");
    const here = binderOf(name, off);
    const out = [];
    let m2;
    while ((m2 = RE.exec(struct))) {
      if (binderOf(name, m2.index) !== here) continue;
      const open = m2.index + m2[0].length - 1;
      const close = matchAt(struct, open);
      if (close < 0) return null;
      for (const [a, b] of splitTop(struct, open + 1, close - 1)) {
        const t = code.slice(a, b).replace(/\s+/g, " ").trim();
        if (!t) return null;
        out.push(/^\.\.\./.test(t) ? { spread: true, text: t.slice(3).trim(), at: a } : { spread: false, text: t, at: a });
      }
    }
    return out.length ? out : null;
  };

  /* WHAT A FUNCTION THIS FILE DECLARES ANSWERS WITH. A `return` in the function's OWN body is a declaration of
     what the call evaluates to, read at the same place a declarator's initializer is; a `return` inside a
     nested function literal belongs to that literal and is skipped, which `innermost` decides rather than a
     brace count. Two functions of one name leave the answer undecided, as two declarations of one binding do.
     A bare `return;` states no value and is skipped — every OTHER return must still answer. */
  /* EACH ANSWER CARRIES ITS OWN OFFSET, and that is not a convenience. A caller resolving a name inside the
     returned expression must resolve it in the SCOPE THAT WROTE IT — §receiver scope's whole subject is that two
     bindings of one spelling are two receivers, and handing a cross-module answer back with the CALLER's offset
     would ask about a binding in the wrong file entirely. */
  const returnsOf = (fnName) => {
    const cand = spans.filter((x) => x.fnName === fnName);
    if (cand.length !== 1) return null;
    const s0 = cand[0];
    const out = [];
    const RET = /\breturn\b/g;
    RET.lastIndex = s0.open;
    let r;
    while ((r = RET.exec(struct)) && r.index < s0.close) {
      if (innermost(r.index) !== s0) continue;
      const a = sig(struct, r.index + 6, struct.length);
      if (struct[a] === ";" || struct[a] === "}") continue;
      out.push({ text: code.slice(a, exprEnd(a)).replace(/\s+/g, " ").trim(), at: a });
    }
    return out.length ? out : null;
  };

  return { binderOf, paramSlot, localParamSlot, callArgsOf, iterOf, pushArgsOf, returnsOf,
           declaredHere: (name) => bound.has(name),
           initOf: (name, off) => inits.get(`${binderOf(name, off)}\0${name}`) ?? null };
}

function scanJS(file, src) {
  const masked = maskJS(src);
  if (masked.fault) { refuse(file, lineOf(src, masked.fault.off), "a JS file this masker could not tokenize", masked.fault.why); return null; }
  const { code, struct, subst } = masked;
  const fault = bracketFault(struct);
  if (fault) { refuse(file, lineOf(src, fault.off), "a JS file whose masked brackets do not balance — the mask is wrong somewhere above this and every span answer about the file would be a guess", fault.why); return null; }

  const site = (off) => ({ file, line: lineOf(src, off) });
  const swallow = swallowingTrySpans(struct);
  /* --- the spans a should-never-happen ASSERTS over ------------------------------------------------------- */
  /* §Architecture's remedy for a field a consumer defaults is "it DCHECKs the field's presence and shape", and
     a consumer that has already done it has closed the thing this gate is looking for: a name the producer does
     not write is then an ABORT at the read rather than a plausible datum. That is a fact about a CONSTRUCT — the
     read sits inside the argument list of the assert — and it is read here so §the anchor loop can ask it. */
  const assertSpans = [];
  for (const fn of ["DCHECK", "DFAIL", "CHECK", "CHECK_FAIL", "DCHECKF"])
    for (const c of callSites(struct, fn))
      if (struct[c.at - 1] !== "." && c.close > 0) assertSpans.push([c.open, c.close]);
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
  /* THE KEY A READ ANCHORS UNDER: the receiver's text, qualified by the scope its BASE NAME is bound in. The
     text stays the display, because that is what a reader opens the file to find; the key is what decides
     which reads are reads of one object. A receiver whose base is not a bare identifier (`a().b`) has no
     binder to ask about and keys under its text alone, exactly as before. */
  const { binderOf, initOf, paramSlot, localParamSlot, callArgsOf, iterOf, pushArgsOf, returnsOf,
          declaredHere } = functionScopes(struct, code);
  const keyOf = (recv, off) => {
    const base = /^[A-Za-z_$][\w$]*/.exec(recv);
    return base ? `${recv}@${binderOf(base[0], off)}` : recv;
  };
  /* A GUARD IS KEYED LIKE A READ, not spelled like one. `keyOf` qualifies the receiver's text by the scope its
     base name is bound in, so a nested function that shadows `input` cannot borrow the outer `input`'s guard —
     the same fact §receiver scope records for reads, asked once here so the two cannot disagree. */
  const guards = instanceofGuards(struct, code).map((g) => ({ ...g, key: keyOf(g.recv, g.at) }));
  const guardedBy = (recv, off) => {
    const k = keyOf(recv, off);
    for (const g of guards) if (g.key === k && off >= g.from && off < g.to) return { text: g.rhs, at: g.rhsAt };
    return null;
  };
  /* WHAT A COMPUTED KEY NAMES — asked of the SAME declaration-reading construct that types a receiver, so a
   * `[k]` is resolved rather than refused over. `{ [ct]: … }` with `const ct = "application/json"` above it is a
   * key that is a STATIC FACT written in a movable way, and refusing it counted a decided thing as unreadable
   * while a resolved one may be the very writer a read was missing.
   *
   * THREE ANSWERS, AND THE THIRD IS THE ONLY REFUSAL. A literal, or an identifier whose every declaration in
   * this file is the SAME literal, NAMES that string. An expression the program COMPUTES — a call, a member, a
   * template, an operator, or an identifier a scope binds without a declaration this can read — names no string
   * in this file: its value is supplied at run time by a computation, and `[expr]` is the syntax that says so.
   * That is DECIDED, not unreadable, exactly as a receiver bottoming out on a literal is. What stays REFUSED is
   * an identifier NO scope here binds — an import or an ambient global — because the constant it names can be
   * declared in another file and the name would then be hiding in the one place this cannot look.
   *
   * A PARAMETER IS NOT A RUN-TIME VALUE WHEN THIS FILE IS THE ONE THAT SUPPLIES IT, and an earlier form of this
   * paragraph said it was — it listed "a parameter" among the things whose value "is supplied at run time by a
   * caller", which is true of the parameter and FALSE of the callers when every one of them is in this file and
   * passes a literal. §localParamSlot already reads exactly that construct one arm away, to type a receiver from
   * the arguments its own file hands it; the same reading answers a KEY, and refusing it here made the gate
   * blind to a reader in the one shape a census reader takes. Measured: engine/build.mjs's shared census-histogram
   * checker reads `b[name]` and is called once per histogram the C engine emits, each with a literal row name, so
   * every one of those histograms read as a field NOBODY CONSUMES while their reader stood in the corpus, asserted
   * their partition and threw on a short sum. (The checker has since gained a third caller, which is why the count
   * is not written down here: what the incident is ABOUT is the shape — one reader, a parameter for the field name,
   * every call site passing a literal — and a number beside it would go stale on the next histogram.) The gate's
   * own subject is a contract with two sides, and it was reporting a disagreement between one side and its own
   * blind spot.
   *
   * AND SEVERAL CALL SITES ARE NOT DOUBT HERE, WHICH IS THE ONE PLACE THIS DIVERGES FROM THE TYPE RULE BESIDE
   * IT — because a TYPE is one fact about one parameter, so two answers contradict, while a KEY is a VALUE and a
   * parameter legitimately takes a different one per call. Each site is a real call that really does read that
   * field off the receiver, so the answer is the SET and not a refusal. What still leaves it undecided is a site
   * passing something that is not a literal: the set is then INCOMPLETE, and an incomplete set of names would
   * credit some reads and silently drop the rest.
   *
   * THE UNDER-CREDITING DIRECTION, STATED SO IT CAN BE ARGUED WITH: deciding a computed key declares no field
   * loses a WRITE that only ever happens through one, whose name is also spelled statically at some read. That
   * read is then reported READ-NO-WRITER with its file and line — a louder row than a refusal and one a person
   * can open — where the refusal it replaces named a construct and no name at all. */
  const STRING_LIT = /^(["'])((?:[^\\]|\\.)*)\1$/;
  const litText = (t) => { const l = STRING_LIT.exec((t || "").trim()); return l ? l[2] : null; };
  const keyNamesOf = (expr, off) => {
    const t = (expr || "").trim();
    const lit = litText(t);
    if (lit !== null) return { names: [lit] };
    if (!/^[A-Za-z_$][\w$]*$/.test(t)) return { computed: true };
    const decls = initOf(t, off);
    if (decls) {
      let one = null;
      for (const d of decls) {
        const l = litText(d);
        /* A BINDING'S WRITES ARE ONE IDENTITY AND ARE HELD TO THE STRICTER STANDARD, unlike the call sites
           below: which of two assignments reaches this use is a fact about a PATH, and this file follows no
           paths. Two disagreeing writes are therefore undecided, exactly as two disagreeing declarations leave
           a receiver's type undecided. */
        if (l === null || (one !== null && one !== l)) return { computed: true };
        one = l;
      }
      return one === null ? { computed: true } : { names: [one] };
    }
    /* …and a parameter of a function THIS FILE declares is named by the arguments this file passes it. */
    const lp = localParamSlot(t, off);
    if (lp) {
      const passed = callArgsOf(lp.fnName, lp.param, lp.declParen);
      if (!passed || !passed.length) return { computed: true };
      const names = new Set();
      for (const a of passed) {
        const l = litText(a.text);
        if (l === null) return { computed: true };   /* one unreadable site, and the SET is incomplete */
        names.add(l);
      }
      return { names: [...names] };
    }
    /* NO SCOPE IN THIS FILE DECLARES THE NAME — an import or an ambient global, and the ONLY answer here that
       can be hiding a field name somewhere this cannot look. A name the file declares but whose declaration
       this cannot read to a single literal is DECIDED computed, exactly as an unreadable expression is. */
    return declaredHere(t) ? { computed: true } : null;
  };

  const localReads = [];   // {name, recv, key, off, form}
  const localWrites = [];  // {name, off}
  /* ONE OBJECT LITERAL IS ONE RECORD — the JS half of §the SHAPE anchor, off the same construct the whole-record
     default already reads. Without it this corpus's own records had no identity at all, so a JS receiver could
     only ever be decided by a C emission, and two generic names were enough to do it.
     AND A RECORD IS NOT ALWAYS FINISHED WHERE IT IS OPENED. `const analysis = { _run: outcome }` followed by
     `analysis.fetchCallSites = result.fetchCallSites` is ONE record written in two constructs, and a
     literal-only shape sees the smaller half of it — so the engine emission those assigned names were copied
     FROM out-scores the literal and wins an anchor the JS record should have taken. The two halves are unioned
     under §the BINDING below; what is collected here is the raw material for it, which is the receiver-and-key
     the write arm already computed and used to throw away. */
  const litShapes = [];    // {off, keys, binding} — `binding` is the scope-qualified key §the BINDING resolved
  const memberAssigns = []; // {key, name, off} — one `recv.name = …`, keyed like a read of the same receiver
  const wholeDefaults = [];// {off, keys, left, op} — a `|| { … }` substituting an entire record

  /* --- member expressions -------------------------------------------------------------------------------- */
  const MEMBER = /(\?\.|\.)\s*([A-Za-z_$][\w$]*)/g;
  let m;
  while ((m = MEMBER.exec(struct))) {
    const optional = m[1] === "?.";
    const nameAt = m.index + m[0].length - m[2].length;
    if (struct[m.index - 1] === "." || struct[m.index - 1] === "?") continue;   /* `...spread`, `?.` already taken */
    const at0 = {};
    const recv = receiverBefore(struct, code, m.index, at0);
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
    if (isWrite) {
      localWrites.push({ name: m[2], off: nameAt });
      memberAssigns.push({ key: keyOf(recv, m.index), name: m[2], off: nameAt });
      continue;
    }
    /* An awaited mojo call has a BETTER anchor than the shape rule — the method's own declared reply — so it
       is routed to that diff instead of into the record namespace, where one field read off one call could
       only ever have been ambiguous. */
    const meth = awaitedMethod(recv);
    if (meth) { mojoReplyReads.push({ file, line: lineOf(src, m.index), method: meth, name: m[2] }); continue; }
    let form = null, consumed = null;
    if (optional) form = "?.";
    else if (/^\?\./.test(nxt)) form = "?. on the value";
    else if (/^(\|\||\?\?)/.test(nxt)) {
      const op = nxt.slice(0, 2);
      consumed = at0.start === undefined ? null : boolConsumer(struct, code, at0.start);
      if (!consumed) form = `${op} default`;
      else consumed = { form: `${op} default`, by: consumed };
    }
    else if (inSpan(swallow, m.index)) form = "inside a swallowing try/catch";
    const co = form || consumed || at0.start === undefined ? null
      : coercionOf(struct, code, at0.start, nameAt + m[2].length, subst, declaredHere);
    localReads.push({ name: m[2], recv, key: keyOf(recv, m.index), off: nameAt, form, consumed,
                      coerced: co, coercedAt: co ? readPlace(assertSpans, emitSpans, m.index) : null });
    if (isRMW) localWrites.push({ name: m[2], off: nameAt });
  }

  /* --- bracketed member expressions with a literal name -------------------------------------------------- */
  const BRACKET = /(\?\.)?\[\s*(["'])((?:[^"'\\]|\\.)*)\2\s*\]/g;
  while ((m = BRACKET.exec(code))) {
    const at = m.index;
    if (struct[at] !== "[" && !(m[1] && struct[at] === "?")) continue;
    const name = m[3];
    if (!/^[A-Za-z_$][\w$]*$/.test(name)) continue;
    const at1 = {};
    const recv = receiverBefore(struct, code, at, at1);
    if (!recv) continue;   /* an index on a literal, or a receiver this cannot normalize */
    const after = sig(struct, at + m[0].length, struct.length);
    const nxt = struct.slice(after, after + 3);
    if (/^=[^=>]/.test(nxt) || /^=$/.test(nxt)) {
      localWrites.push({ name, off: at });
      memberAssigns.push({ key: keyOf(recv, at), name, off: at });
      continue;
    }
    let form = null, consumed = null;
    if (m[1]) form = "?.[]";
    else if (/^(\|\||\?\?)/.test(nxt)) {
      const op = nxt.slice(0, 2);
      consumed = at1.start === undefined ? null : boolConsumer(struct, code, at1.start);
      if (!consumed) form = `${op} default`;
      else consumed = { form: `${op} default`, by: consumed };
    }
    else if (inSpan(swallow, at)) form = "inside a swallowing try/catch";
    const co2 = form || consumed || at1.start === undefined ? null
      : coercionOf(struct, code, at1.start, at + m[0].length, subst, declaredHere);
    localReads.push({ name, recv, key: keyOf(recv, at), off: at, form, consumed,
                      coerced: co2, coercedAt: co2 ? readPlace(assertSpans, emitSpans, at) : null });
  }

  /* --- bracketed member expressions whose key is a NAME THIS FILE STATES ---------------------------------- */
  /* THE SAME CONSTRUCT AS THE BLOCK ABOVE, ONE DECLARATION FURTHER OUT. `o["f"]` and `o[F]` with `const F =
     "f"` are one fact written two ways, and so is `o[k]` inside a helper every caller in this file hands a
     literal — §keyNamesOf resolves all three and DECIDES the rest. Matched in `struct` so an identifier inside
     a string or a comment is not one, and disjoint from the literal block by construction: that regex requires
     a quote where this one requires a bare identifier. */
  const COMPUTED_KEY = /(\?\.)?\[\s*([A-Za-z_$][\w$]*)\s*\]/g;
  while ((m = COMPUTED_KEY.exec(struct))) {
    const at = m.index;
    const at2 = {};
    const recv = receiverBefore(struct, code, at, at2);
    if (!recv) continue;   /* an index on a literal, or a receiver this cannot normalize */
    const ans = keyNamesOf(m[2], at);
    if (!ans) {
      refuse(file, lineOf(src, at), "a computed member key naming an identifier no scope in this file binds — the constant it names can be declared in another one", struct.slice(Math.max(0, at - 40), at + m[0].length));
      continue;
    }
    if (ans.computed) continue;   /* the program computes this key, so the read names no field statically */
    const after = sig(struct, at + m[0].length, struct.length);
    const nxt = struct.slice(after, after + 3);
    /* A COMPUTED MEMBER IMMEDIATELY CALLED IS AN OPERATION, for §MEMBER's reason exactly: a record on this seam
       is JSON text and JSON carries no functions, so `o[m]()` is a dispatch and never a field of a record. */
    if (nxt[0] === "(") continue;
    const isWrite = /^=[^=>]/.test(nxt) || /^=$/.test(nxt);
    let form = null, consumed = null;
    if (!isWrite) {
      if (m[1]) form = "?.[]";
      else if (/^(\|\||\?\?)/.test(nxt)) {
        const op = nxt.slice(0, 2);
        consumed = at2.start === undefined ? null : boolConsumer(struct, code, at2.start);
        if (!consumed) form = `${op} default`;
        else consumed = { form: `${op} default`, by: consumed };
      } else if (inSpan(swallow, at)) form = "inside a swallowing try/catch";
    }
    for (const nm of ans.names) {
      if (!/^[A-Za-z_$][\w$]*$/.test(nm)) continue;
      if (isWrite) { localWrites.push({ name: nm, off: at }); memberAssigns.push({ key: keyOf(recv, at), name: nm, off: at }); }
      else {
        const co3 = form || consumed || at2.start === undefined ? null
          : coercionOf(struct, code, at2.start, at + m[0].length, subst, declaredHere);
        localReads.push({ name: nm, recv, key: keyOf(recv, at), off: at, form, consumed,
                          coerced: co3, coercedAt: co3 ? readPlace(assertSpans, emitSpans, at) : null });
      }
    }
  }

  /* --- §the BINDING: which record an object literal OPENS, so a later assignment can finish it -------------- */
  /* WHAT COUNTS AS "THE SAME RECORD" HAS TO BE SOMETHING THE CODE STATES, NEVER PROXIMITY. Member assignment
   * spreads across a function, a constructor, several files; a rule that folded whatever it found near a
   * literal would be the tokenizer deciding which writes belong to which object, which is the guess this file
   * refuses everywhere. What the code states is a BINDING, and this asks TWO constructs of it, both already
   * read here for other questions:
   *   1. THE LITERAL IS THE OPERAND OF AN `=` WHOSE LEFT SIDE IS A BARE IDENTIFIER. That is syntax — `analysis
   *      = {` — and it is what says which name this literal is the record of. A member path (`a.b = {`), a
   *      comparison and a compound write all fail it on the character before the `=`.
   *   2. THAT BINDING'S DECLARATION, ASKED OF §initOf, IS EXACTLY ONE WRITE AND ITS TEXT IS AN OBJECT LITERAL.
   *      This is the same construct the PLATFORM arm types a receiver off — "a binding's `= <expr>` is a
   *      DECLARATION in the same sense a `#define` body is" — and it is what makes the fold sound rather than
   *      merely local. `initOf` is already null for a name declared twice in one scope, for a compound write,
   *      and for a binding written more than once; so a `let o = {a:1}; … o = somethingElse; o.b = 2` cannot
   *      merge two objects under one name, because the second write is what makes the declaration doubtful.
   *      A `const` satisfies it by the language; a once-assigned `let` satisfies it by measurement, which is
   *      why the test is the declaration and not the keyword.
   * The key is `keyOf(name, …)` — the receiver text qualified by the scope that binds it, the SAME key a read
   * of that receiver is filed under — so an assignment naming a shadowing binding in a nested function keys
   * differently and cannot join this record. And a binding with assignments but NO readable literal declares
   * no record here: `const result = linesToAnalysis(…); result._fkey = …` names a record whose identity is the
   * answer of a CALL, and following a value out of a call is the flowed identity this file refuses. Those
   * assignments stay writes and anchor nothing, which is why the read of one still reports OFF-RECORD.
   * AND NEITHER OF THOSE IS A REFUSAL, which is the line §the REFUSAL discipline draws: a refusal is a place a
   * field NAME could be hiding, and no name hides here — every key of the literal and every assigned name is
   * already a write in the field namespace whatever this answers. What a doubtful binding costs is an IDENTITY,
   * and the file already has a category for an identity it cannot decide. Refusing over each of them would put
   * every object literal in the corpus in the refusal list, which is the language rather than the seam. */
  const bindingOfLiteral = (eq) => {
    if (struct[eq] !== "=") return null;
    if ("=!<>+-*/%&|^~".includes(struct[eq - 1])) return null;   /* a comparison or a compound write */
    let q = eq - 1;
    while (q >= 0 && /\s/.test(struct[q])) q--;
    if (q < 0 || !/[\w$]/.test(struct[q])) return null;
    const e = q;
    while (q >= 0 && /[\w$]/.test(struct[q])) q--;
    if (q >= 0 && (struct[q] === "." || struct[q] === "?")) return null;   /* `a.b = {` — a member, not a binding */
    const name = code.slice(q + 1, e + 1);
    if (!/^[A-Za-z_$][\w$]*$/.test(name)) return null;
    const decl = initOf(name, q + 1);
    if (!decl || decl.length !== 1 || decl[0][0] !== "{") return null;
    return keyOf(name, q + 1);
  };

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
        recv = normExpr(code.slice(s, e), struct.slice(s, e));
        /* §normExpr AND NOT A LOCAL STRIP, because this text and §receiverBefore's land in ONE `localReads`
           pool and are compared for equality there: two producers of one anchor that normalize differently are
           two spellings of one object, which is the merge §receiver scope is built against. It is also the
           producer where a KEYWORD can lead — `const { a } = await session(x)` is this arm, and the walk above
           can never yield a leading keyword because it stops at the operand's own first code point. */
        /* The `;` is asked of the STRUCTURE this span was delimited on, never of its text — §crossesStatement. */
        if (!/^[A-Za-z_$]/.test(recv) || crossesStatement(struct.slice(s, e))) recv = null;
      }
      if (recv === null) {
        const kw = /^(of|in)\b/.exec(struct.slice(afterOff));
        if (kw) {
          let e2 = afterOff + kw[1].length;
          const stop = matchAt(struct, struct.lastIndexOf("(", i));
          let e3 = e2;
          while (e3 < struct.length && e3 < (stop > 0 ? stop - 1 : struct.length) && struct[e3] !== ";") e3++;
          const t2 = normExpr(code.slice(e2, e3), struct.slice(e2, e3));
          if (/^[A-Za-z_$]/.test(t2) && !crossesStatement(struct.slice(e2, e3))) recv = t2;
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
        if (/^\s*\[/.test(t)) {
          const ob = a + t.indexOf("[");
          const cb = matchAt(struct, ob);
          const ans = cb < 0 ? null : keyNamesOf(code.slice(ob + 1, cb - 1), ob);
          if (!ans) {
            refuse(file, lineOf(src, a), `a computed ${kind === "literal" ? "object-literal key" : "destructuring key"} naming an identifier no scope in this file binds — the constant it names can be declared in another one`, t);
            continue;
          }
          if (ans.computed) continue;   /* the program computes this key, so the entry declares no field name */
          /* Resolved, and a resolved name that is not identifier-shaped declares no field — the same answer a
             quoted `"application/json":` key already gets one branch below, reached by one more construct. */
          for (const nm of ans.names) {
            if (!/^[A-Za-z_$][\w$]*$/.test(nm)) continue;
            if (kind === "literal") { localWrites.push({ name: nm, off: ob }); litKeys.push(nm); }
            else localReads.push({ name: nm, recv, key: keyOf(recv, i), off: ob, form: null });
          }
          continue;
        }
        if (kind === "literal") refuse(file, lineOf(src, a), "an object-literal entry whose key this cannot read", t);
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
    /* THE KEY COUNT IS ASKED OF THE WHOLE RECORD, WHICH IS WHY THE BINDING IS RESOLVED FIRST. `const analysis =
       { _run: outcome }` is a one-key literal and a six-field record, so a `>= 2` on the literal alone drops
       exactly the case §the BINDING exists for — the record whose literal half is the smaller one. A literal
       that opens a binding is kept whatever it declares; the two-name floor is then applied to the UNION, where
       it means what it was written to mean. */
    if (kind === "literal") {
      const binding = bindingOfLiteral(p);
      if (binding || litKeys.length >= 2) litShapes.push({ off: i, keys: litKeys.slice(), binding });
    }
    if (kind === "literal" && (prevCh === "|" || prevCh === "?") && struct[p - 1] === prevCh) {
      let q = p - 1;
      while (q > 0 && /\s/.test(struct[q - 1])) q--;
      const left = receiverBefore(struct, code, q) || code.slice(Math.max(0, q - 24), q).replace(/\s+/g, " ").trim();
      wholeDefaults.push({ off: i, keys: litKeys.slice(), left, op: prevCh === "|" ? "||" : "??" });
    }
  }

  /* --- the names this file imports from a module OUTSIDE the corpus --------------------------------------- */
  /* AN IMPORT SPECIFIER IS SYNTAX, AND A BARE ONE NAMES A MODULE THIS GATE DOES NOT READ. A relative specifier
     resolves inside the corpus and its records are on the seam like any other; a BARE one is a package or a
     `node:` builtin, whose records this corpus never produces and whose shape no authority here declares. That
     distinction is a construct — `./x.mjs` versus `node:http` is a character in the source, not an inference —
     and it is the same kind of fact the platform arm reads off a declaration. */
  /* THE RELATIVE SPECIFIER IS KEPT TOO, and it is not the same fact wearing the other sign. `foreignImports`
     answers "is this name's producer outside the corpus"; the MAP answers "which module declares this name",
     which is what lets §the ORIGIN walk follow a value one module further instead of stopping where the file
     ends. Both are the same character in the source, read once. */
  const foreignImports = new Set();
  const importOf = new Map();     // local binding name -> the specifier it was imported from
  const IMPORT = /\bimport\s+([^;]*?)\bfrom\s*(["'])([^"'\n]+)\2/g;
  while ((m = IMPORT.exec(code))) {
    if (struct[m.index + m[0].length - 1] !== m[2]) continue;
    const bare = !/^[./]/.test(m[3]);
    for (const part of m[1].replace(/[{}*]/g, " ").split(",")) {
      const as = /\bas\s+([A-Za-z_$][\w$]*)/.exec(part);
      const id = as || /([A-Za-z_$][\w$]*)\s*$/.exec(part.trim());
      if (!id || id[1] === "from") continue;
      importOf.set(id[1], m[3]);
      if (bare) foreignImports.add(id[1]);   /* a package or a `node:` builtin — never read by this gate */
    }
  }

  /* --- markers: a literal naming an @TAG is a reference to the stream contract ----------------------------- */

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

  /* `code` LEAVES WITH THE SCAN because §THE DERIVED READER asks a question about this file's own text that
     no per-read record can carry: which calls DELIMIT A REGION OF A PRODUCER. Handing out the masked text
     rather than re-reading the file is the same rule the revision guard states — a second read is a second
     instant, and a scan that answers about two of them answers about no revision. */
  return { file, src, code, localReads, localWrites, litShapes, memberAssigns, wholeDefaults, site, initOf, binderOf, paramSlot,
           localParamSlot, callArgsOf, guardedBy, foreignImports, importOf, iterOf, pushArgsOf, returnsOf,
           asserted: (off) => assertSpans.some(([a, b]) => off >= a && off < b) };
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
/* EVERY WRITE TO `id` INSIDE [from,to), each with the offset its right-hand side begins at — `-1` for a
   compound or increment form, which writes a value derived from the old one and therefore declares nothing. The
   COUNT is what tells a binding with one declaration from one this cannot name at the read; the POSITIONS are
   what let a binding declared empty and written once be read AT that write. Two consumers, one walk — a second
   copy of this regex is the hand-kept list this whole file argues against. */
function assignmentSites(struct, id, from, to) {
  const out = [];
  const re = new RegExp(`(^|[^\\w$.])${id}(?![\\w$])`, "g");
  re.lastIndex = from;
  let m;
  while ((m = re.exec(struct)) && m.index < to) {
    let j = m.index + m[1].length + id.length;
    while (j < to && /\s/.test(struct[j])) j++;
    if (struct[j] === "=" && struct[j + 1] !== "=" && struct[j + 1] !== ">") out.push({ at: m.index, rhs: j + 1 });
    else if (/^(\+\+|--|\+=|-=|\|=|&=|\^=|\*=|\/=|%=)/.test(struct.slice(j, j + 2))) out.push({ at: m.index, rhs: -1 });
  }
  return out;
}
function assignmentsTo(struct, id, from, to) { return assignmentSites(struct, id, from, to).length; }

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
/* THE PRODUCER'S OWN BYTES, KEPT FROM THE ONE READ THAT SCANNED THEM. §THE DERIVED READER resolves a region a
   consumer delimits by two literals of the producer's own text, and it must resolve it in the SAME bytes every
   file:line above was measured in — a re-read is a second instant, and this scan already prints whether the
   tree moved under it because an answer spanning two of them describes no revision. */
const cSrc = new Map();   // repo-relative path -> source text
/* The scans, keyed by their repo-relative path, so a relative import specifier resolves to the module that
   declares the name — which is how §the ORIGIN of a value follows a producer one module further. */
const scanByFile = new Map();
/* THE FORMAT-TAKING DECLARATIONS BEFORE ANY OF THEM IS READ. A composer is DECLARED in one header and CALLED
   in another file, so a vocabulary assembled while scanning would credit whichever file the walk reached
   first — the composed keys of every file ahead of the declaration would read as emitted by nobody, which is
   the exact defect this pass exists to end, reproduced by the order it was collected in. */
for (const f of files) {
  if (f.lang !== "c") continue;
  let src;
  try { src = readFileSync(f.path, "utf8"); } catch { continue; }
  collectCFormatDecls(relative(ROOT, f.path), src);
}
for (const f of files) {
  let src;
  try { src = readFileSync(f.path, "utf8"); } catch { continue; }
  const rel = relative(ROOT, f.path);
  if (f.lang === "c") { cSrc.set(rel, src); scanC(rel, src); continue; }
  if (f.lang === "html") {
    const view = htmlScriptView(src);
    if (!view) continue;    /* a document with no inline script carries no seam — decided, not unreadable */
    src = view;
  }
  const s = scanJS(rel, src);
  if (s) { const e = { ...s, area: f.area }; jsScans.push(e); scanByFile.set(rel, e); }
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

/* A FRAGMENT'S KEYS ARE ITS CALLERS' KEYS. Folded before anything anchors, so no receiver is ever compared
   against half of a record. A fragment nothing in its own file calls keeps its keys where they are rather
   than losing them — that is a fragment this scan could not place, and dropping its fields would be the
   silent third state §the REFUSAL discipline refuses. */
{
  const byName = new Map();
  for (const b of cBodies) if (b.name && !b.root && shapes.has(b.id)) byName.set(b.name, b);
  for (const [nm, frag] of byName) {
    let placed = false;
    for (const b of cBodies) {
      if (b === frag || !b.root || !b.calls.has(nm) || b.file !== frag.file) continue;
      for (const k of shapes.get(frag.id)) shapeOf(b.id).add(k);
      placed = true;
    }
    if (placed) shapes.delete(frag.id);
  }
}

/* THE CORPUS'S OWN JS RECORDS, and the ONE-DIRECTIONAL use this gate may make of them. They are not a second
   authority over the serialized seam — the C emissions are that, exactly as the .idl is idlgen's. What they
   are is the thing that can REFUTE a C anchor, and until they existed nothing could: a receiver reading `name`,
   `type` and `location` was anchored to endpoint.c's parameter record on three of the commonest field names in
   any JS corpus, and that is the wrong-anchor failure §the PLATFORM receiver already names, arriving through
   the one door neither the IDL arm nor the foreign arm covers — the corpus's own JavaScript.
   A LITERAL DECLARING NOTHING THE ENGINE DOES NOT DECLARE IS NOT A RIVAL. `{ _routedTasksFired: 0, … }` in a
   driver and `record(p, { wTop: 3, wMin: 1 })` in a fixture are the ENGINE's field names written down on the
   consumer's side — a restatement of the record, not a competing one. It makes no claim the emission does not
   already make, so it cannot be evidence against it; counting it would let a driver's own expectation
   dispossess the emission it was written to check. */
/* AND A RECORD ASSEMBLED BY ASSIGNMENT IS THE SAME RECORD AS THE LITERAL THAT OPENED IT — §the BINDING says
   which literal that is, and this is where the two halves become one shape. Until they did, a record built
   field-by-field was invisible to the identity half of this gate exactly where it mattered most: the assigned
   names are the ones a JS record COPIES OFF an engine emission, so the literal kept only the record's OWN
   fields, scored one against the emission's several, and lost the anchor — after which every one of those own
   fields printed as a read OFF the engine's record, which is a wrong reason with a producer's name behind it.
   THE RESTATEMENT TEST IS THEN ASKED OF THE UNION, WHICH IS ALSO WHAT ANSWERS THE COPY. A record that
   transcribes an emission field-by-field and adds NOTHING declares nothing the engine does not declare, so it
   is a restatement and is dropped here exactly as a copy written as one literal already was — the fold cannot
   turn a copy into a rival. What it can do is let a record that copies an emission AND states fields of its
   own be scored on all of them, which is what it is: `{sourceUrl, _run, resolverErrors}` plus the three
   document arrays is bridge.js's analysis, and the receiver reading four of those is reading an analysis. */
const jsShapes = new Map();   // "file:line" -> Set(name), rivals only
{
  const emitted = new Set();
  for (const [, shp] of shapes) for (const k of shp) emitted.add(k);
  for (const s of jsScans) {
    const assigned = new Map();   // binding key -> Set(name)
    for (const a of s.memberAssigns) {
      if (!assigned.has(a.key)) assigned.set(a.key, new Set());
      assigned.get(a.key).add(a.name);
    }
    for (const L of s.litShapes) {
      const keys = new Set(L.keys);
      /* ABSENCE IS A POSITIVE STATEMENT HERE, NOT A HOLE TO FILL: a binding no member assignment names is a
         record that is finished at its literal, which is the majority and is the case the fold must not
         change. Asked with `has`, never defaulted to an empty list — §A-FIELD-A-CONSUMER-DEFAULTS. */
      if (L.binding !== null && assigned.has(L.binding)) for (const n of assigned.get(L.binding)) keys.add(n);
      if (keys.size < 2) continue;   /* the two-name floor, applied to the whole record — see §the KEY COUNT */
      let own = false;
      for (const k of keys) if (!emitted.has(k)) { own = true; break; }
      if (!own) continue;            /* declares nothing the engine does not — a restatement, never a rival */
      jsShapes.set(`${s.file}:${s.site(L.off).line}`, keys);
    }
  }
}

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

/* A RECEIVER WHOSE RECORD IDENTITY THIS CANNOT DECIDE — counted, never resolved either way. Every row carries
   its own REASON, because the category now holds more than one question and a section header stating one of
   them says the wrong thing about the rest: a reader who cannot tell which question a row is an instance of
   cannot tell what would answer it, which is the same "three states behind one answer" this file refuses in the
   engine. Grouped by reason on the way out, exactly as REFUSED is. */
const ambiguous = [];   // {file, line, recv, reason, why}
const TWO_RECORDS = "a receiver that an ENGINE EMISSION and a record this corpus CONSTRUCTS explain exactly" +
                    " as well as each other — the identity is a disagreement this cannot collapse, and" +
                    " anchoring to either would be deciding it by which authority was asked first";
const ONE_FIELD = "a receiver reading exactly ONE field of an emitted record beside name(s) no producer writes" +
                  " — whether the object is that record is not decidable here, so neither is whether those" +
                  " names are the defect";
const oneFieldWhy = (shared, unwritten) =>
  `${shared ? `shares \`${shared}\`, also names` : "names only"} ` +
  `${unwritten.slice(0, 4).map((n) => `\`${n}\``).join(", ")}${unwritten.length > 4 ? " …" : ""}`;

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
 * refuses — and it is taken only where it is unambiguous. A binding written more than once is NOT thereby
 * doubtful: every write is a declaration of the same identity and they must AGREE, which is the standard the
 * parameter arm already applies to a call's arguments. What is doubtful is a name declared TWICE in one scope,
 * a compound or increment write (which derives a value from the old one and declares nothing), and writes that
 * name two different interfaces. A `null`/`undefined` write is Web IDL §2.13.27 Nullable types — T?'s absent
 * half of the same declaration — it names no interface and contradicts none, so it is skipped rather than
 * counted against, exactly as a nullable ARGUMENT is already read as the type it is nullable of.
 * A receiver whose declaration this cannot resolve is not a platform object as far as this gate is concerned,
 * and it goes back to the shape anchor and to AMBIGUOUS exactly as before.
 *
 * AN IDL RETURN TYPE IS AN UPPER BOUND, WHICH IS WHY THE INTERFACE IS THEN NARROWED. `getElementById` returns
 * `Element?` and the object is an HTMLSelectElement; a strict member diff against Element would report
 * `selectedIndex` as a member the platform does not have, which is a finding manufactured out of the spec being
 * read too literally. So the construct fixes a BASE and the read set picks among that base and its descendants,
 * collapsing an inheritance chain to its most general matching member. Here the name set is doing work it is
 * entitled to do, because the construct has already cut the fifteen hundred candidates down to one inheritance
 * subtree.
 *
 * §THE TWO QUESTIONS A BASE ANSWERS, AND WHY THE NARROWING MAY ONLY DECIDE ONE OF THEM. The pick above
 * frequently does NOT land on one candidate — `{value, classList}` fits eleven of Element's descendants
 * equally, because `classList` is Element's own and `value` is declared by every form control separately — and
 * the refusal to guess between them is right. What was wrong is what the refusal was allowed to take with it.
 * A base answers TWO questions: WHOSE contract this receiver is on, which the CONSTRUCT decides by itself
 * (`getElementById` is declared to return `Element?`, so whatever the object is, the platform made it), and
 * WHICH interface it is, which needs the read set too. That is CLAUDE.md's predicate-that-answers-two-questions
 * defect exactly — the stricter question decides and the cost lands silently on the other. Here the stricter
 * question was WHICH, one `null` carried both answers away, and the receiver fell through to an anchor decided
 * by NAME COLLISION — the failure the platform arm is asked FIRST to prevent, reached through the arm itself.
 * MEASURED AT cb454cff, before the split: of 193 receivers whose base a construct decided, 115 narrowed and
 * were printed and the other SEVENTY-EIGHT had that answer thrown away. SIX of the 78 reached AMBIGUOUS,
 * printed under a reason about an engine emission — a wrong statement of what was undecided, since which record
 * it is was never open. The other SEVENTY-TWO reached the shape anchor, scored under two of an emission's
 * names, and were printed in NO band at all: not audited, not decided, not counted — a removal nobody can see,
 * which is the concealment this file exists to report performed on its own output. NONE of the 78 scored TWO,
 * so the name-collision anchor is a HAZARD this closes and not a wrong answer it was measured making; what was
 * measured is the silence.
 * So the two questions are two predicates over the ONE fact: the base decides the band, and the narrowing
 * decides only what may be said about members — which keeps them from ever disagreeing, because there is still
 * exactly one bit underneath.
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
/* Web IDL §2.10 "Callback functions", which are a declaration kind of their own and are not in the interface
   map. A callback INTERFACE — §2.4 "Callback interfaces" — is (EventListener lives in byName with its one
   `handleEvent` operation), so the two spellings of "a function the platform will call" are looked up in the
   two places the parse put them. §2.4 stood at BOTH halves here, which is the one number the sentence is
   about telling apart. */
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
/* §valueIteratorType — WHAT A `for … of` OVER A PLATFORM OBJECT BINDS, read off the interface's OWN iteration
   declaration and off nothing else. Web IDL §2.5.9 Iterable declarations: "If a single type parameter is given,
   then the interface has a value iterator and provides values of the specified type. If two type parameters are
   given, then the interface has a pair iterator and provides value pairs with the given types." A pair iterator
   binds an ARRAY and names no interface, which is the answer given here — and the two set-ish declarations are
   decided by the same fact one section apart: §3.7.12.2 %Symbol.iterator% makes a setlike's default iterator
   "the function object that is the value of the values property", its one declared type, while §3.7.11.2
   %Symbol.iterator% makes a maplike's "the value of the entries property", which is pairs again.
   AN `async_iterable` IS REFUSED OUTRIGHT. §3.7.10 gives it %Symbol.asyncIterator% and no %Symbol.iterator%, so
   a plain `for … of` over one reaches nothing at all — and `iterOf` matches `for await (` and `for (` with one
   regex and hands back no way to tell them apart, so answering either way would be a guess about which
   construct was written. Under-crediting, which is the direction this file takes everywhere.
   READ THROUGH `flatten`, because the declaration is INHERITED: RadioNodeList declares no iteration of its own
   and iterating one yields NodeList's `Node`.
   THE ANSWER IS AN UPPER BOUND like every other IDL type here — `NodeList` is `iterable<Node>` and the element
   a popup reads `.value` off is an HTMLInputElement — so it is a BASE, narrowed by the read set at
   §narrowIdentity exactly as `getElementById`'s `Element?` is, and decided-platform either way. */
function valueIteratorType(iface) {
  for (const m of idl.flatten(iface)) {
    if (m.type === "maplike" || m.type === "async_iterable") return null;
    if (m.type === "iterable" || m.type === "setlike")
      return Array.isArray(m.idlType) && m.idlType.length === 1 ? ifaceOfType(m.idlType[0]) : null;
  }
  return null;
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

/* ---- the FOREIGN receiver: a decided negative from an IMPORT SPECIFIER rather than from an interface ------- */

/* THE PLATFORM ARM ANSWERS FOR WEB IDL AND THE CORPUS IS NOT ONLY A BROWSER. `@webref/idl` declares URL,
 * Response and Element; it declares nothing about Node's `http.IncomingMessage`, a `chrome.*` extension record,
 * a CDP `Profiler.CallFrame` or an npm package's own parse tree — so a receiver that is one of those reaches the
 * SHAPE anchor, and the shape anchor decides it by NAME COLLISION. That is the failure the platform arm's own
 * comment warns about ("a platform object whose members happen to collide with two of an emitted record's names
 * is exactly the receiver the shape rule anchors CONFIDENTLY and wrongly"), arriving through the door that
 * comment does not cover. Measured: `createServer((req, res) => …)` reads `req.url`, `req.headers` and
 * `req.method`, three names this engine also emits, so `String(req.url || '')` — correct Node code over an
 * object no producer in this corpus writes — was reported as a consumer DEFAULTING an engine field.
 *
 * WHAT DECIDES IT IS THE SPECIFIER, NOT THE NAMES. A callback parameter is typed by the call it is an argument
 * of; §paramSlot already reads that construct to let Web IDL type one. When the callee is a binding this file
 * imported from a BARE specifier, the party that calls the callback is a module outside the corpus, so the
 * record it hands over is not on the seam this gate audits — the same sentence DECIDED PLATFORM makes, with the
 * module system supplying the authority instead of the IDL. It states no interface and claims none: it says
 * where the producer is, which is the only thing an import specifier can say.
 *
 * THE UNDER-CREDITING DIRECTION, STATED: a bare-specifier helper that is handed a CORPUS record and calls back
 * with it (`someLib.each(records, (r) => r.url)`) loses that read. Its absence shows as the field's producer
 * appearing in WRITE-WITH-NO-READER with its file and line — a row a person can open — rather than as a silent
 * pass, which is the direction this file takes everywhere. */
const foreignDecided = [];    // {file,line,recv,names,why,by} — a value some module outside this corpus made
/* A receiver whose record identity is STILL open and whose every read is asserted at the read — see §the
   anchor loop's own paragraph for why that answers the half of the question this gate is the auditor of. */
const assertedDecided = []; // {file,line,recv,names}
const offInterface = [];      // {file,line,recv,iface,missing} — that object, read for a member the spec denies
/* A RECEIVER A RECORD THIS CORPUS CONSTRUCTS EXPLAINS BETTER THAN ANY ENGINE EMISSION DOES — its producer is
   JavaScript, so the contract it is on is not the serialized seam this gate is the auditor of. The same
   sentence as DECIDED PLATFORM with the corpus's own object literal as the authority instead of the IDL, and
   printed in full for the same reason both of those are: a decided negative nobody can see is the concealment
   this file exists to report, performed on its own output. Every row carries BOTH candidates and BOTH scores,
   so the margin that decided it is on the page. */
const corpusDecided = [];     // {file,line,recv,names,cShape,cN,jsShape,jsN}
/* A READ THE ANCHORED RECORD DOES NOT EMIT, where some OTHER record in the corpus writes that name. This is
   OFF-INTERFACE's sibling one layer down: there the authority is the IDL's member list, here it is the
   emission's own key list, and the defect is identical — a field read off a record that does not have it,
   excused until now by an unrelated party that happens to spell a field the same way. */
const offRecord = [];         // {file,line,recv,shape,name,writes}
/* A NAME THE CONSUMER ITSELF PUT ON THE RECORD, which is the one write OFF-RECORD's own sentence excludes and
   its test did not. That category says these reads were "excused by whichever UNRELATED party in the corpus
   happened to spell the field the same way", and a member assignment onto a receiver of THIS record in THIS
   file is not an unrelated party — it is this consumer, deriving a value the producer deliberately does not
   restate. Decided and printed with the assignment that made it, never accused: the remedy an accusation asks
   for is to make the producer emit the row, and that would give a quantity whose whole value is being DERIVED
   AS A CHECK a second writer, which is the assertion deleted rather than a contract closed. */
const consumerSynth = [];     // {file,line,recv,shape,name,at}

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
    /* THE BINDING FIRST, THE GUARD ONLY WHERE THE BINDING SAYS NOTHING — one identity, asked in one order, so
       there are never two answers to compare. Everything below states what the binding IS, everywhere in its
       scope; a guard states what it is HERE, and a name whose declaration already names an interface is
       narrowed by `narrowIdentity` within that interface's own subtree rather than by a second mechanism.
       So the guard is the LAST resort, which is exactly the position §the PLATFORM receiver leaves open: a
       receiver whose declaration this cannot resolve. The RHS of the test is an ordinary expression and is
       resolved by this same function, so `x instanceof Request` and `x instanceof window.Request` are one
       question — and Web IDL §3.7.3 makes the answer an interface OBJECT, whose instances are the interface,
       which is the one step `ctor:` already spells. A test against anything else names no interface and
       narrows nothing. */
    const bound = identityOfBinding(t, off, scan, s);
    if (bound) return bound;
    const g = scan.guardedBy(t, off);
    if (!g) return null;
    const ctor = ifaceOfExpr(g.text, g.at, scan, new Set(s));
    return ctor && ctor.startsWith(CTOR) ? ctor.slice(CTOR.length) : null;
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

/* §bindingDecl — WHICH CONSTRUCT DECLARES A BINDING, IN ONE ORDER, ASKED BY BOTH WALKS. Two walks here start
   from a bare name — `ifaceOfExpr` asking WHICH INTERFACE it is, `originOfExpr` asking WHOSE VALUE it is — and
   both must first answer the same prior question, which is which construct in the file states what the name
   holds. That question has ONE answer and it is ECMAScript's, so it is stated once here rather than twice:
   a second copy of an ordering rule is the shape §AN-AUDITOR-DERIVES-THE-RULE warns about, and the one that
   drifts is the copy nobody runs against reality.
   THE ORDER, AND WHY IT IS THIS ORDER. A `const` `for … of` head is taken FIRST because it is the only
   construct that declares its binding at all and because §14.7.5.7 ForIn/OfBodyEvaluation ( lhs, stmt,
   iteratorRecord, iterationKind, lhsKind, labelSet [ , iteratorKind ] ) makes a fresh declarative environment
   per iteration whose `const` binding §9.1.1.1.5 SetMutableBinding ( name, value, strict ) forbids assigning —
   so a `name = …` the assignment scan attributes to it CANNOT be a write to it and must be another binding's.
   The WRITES come next, every one of them a declaration of the one identity. A `let`/`var` head is not
   authoritative in that way and is reached only where the writes said nothing. */
function bindingDecl(scan, name, off) {
  const head = scan.iterOf(name, off);
  if (head && head.konst) return { iter: head };
  const writes = scan.initOf(name, off);
  if (writes) return { writes };
  return head ? { iter: head } : null;
}

/* WHAT A BARE NAME'S OWN BINDING SAYS IT IS — its declarations (every write that names it, which must agree, and
   the `for … of` head that is a declaration no write can be), the global, the IDL-typed callback parameter, the
   local parameter typed by its call sites. Every arm is a construct at the DECLARATION; none of them is a fact
   about the point of use, which is why the caller can fall through to a guard when they all answer nothing. The
   cycle guard is the caller's, because it is the caller that keys it. */
function identityOfBinding(t, off, scan, s) {
  const decl = bindingDecl(scan, t, off);
  /* A `for … of` HEAD DECLARES ITS BINDING AS AN ELEMENT OF WHAT THE HEAD ITERATES, and where that expression
     is a platform object the ELEMENT TYPE is declared too — by the interface's own iteration declaration, read
     at §valueIteratorType. `originOfExpr` has read this construct since it was written, to name a foreign
     array's producer; the interface question is the same construct asked for the other authority, and asking it
     here is why `for (const inp of document.querySelectorAll(…))` stopped being a binding no arm could see.
     The head's own offset is used, never the reader's: a name inside the header is bound in the scope that
     wrote the header. */
  if (decl && decl.iter) {
    const on = ifaceOfExpr(decl.iter.text, decl.iter.at, scan, new Set(s));
    return on && !on.startsWith(CTOR) ? valueIteratorType(on) : null;
  }
  /* EVERY DECLARATION OF THE BINDING MUST ANSWER, AND ALL OF THEM MUST AGREE — the standard the parameter arm
     below already applies to a call's arguments, asked here of a binding's writes. A `null`/`undefined` write is
     the nullable half of the same declaration and names no interface, so it is skipped rather than counted as a
     disagreement; one unreadable or one disagreeing write leaves the binding undecided, which is the
     under-crediting direction this file takes everywhere. EACH WRITE GETS ITS OWN COPY OF THE CYCLE GUARD, for
     the reason §localParamSlot records: two writes of the SAME identifier are not a cycle, and a shared set
     would read the second as one and answer the whole question `null`. */
  if (decl) {
    let one = null;
    for (const d of decl.writes) {
      if (!d || d === "null" || d === "undefined") continue;
      const got = ifaceOfExpr(d, off, scan, new Set(s));
      if (!got || (one !== null && one !== got)) return null;
      one = got;
    }
    return one;
  }
  if (scan.binderOf(t, off) === "file") return GLOBAL_IFACE.get(t) || null;
  /* A PARAMETER OF A CALLBACK THE PLATFORM CALLS IS TYPED BY THE IDL, and by nothing else in this file. The
     chain is three declarations long and every link is spec text: the callee's operation declares its
     argument a callback type, that callback declares its own argument list, and the parameter's POSITION
     picks one. Nothing is followed through an assignment or a return — a call site is a construct, and this
     is the same kind of fact as a binding's initializer, read at the other end of the call. */
  const slot = scan.paramSlot(t, off);
  if (slot) {
    const m2 = /^(.*)\.([A-Za-z_$][\w$]*)$/.exec(slot.callee);
    if (!m2) return null;
    const on = ifaceOfExpr(m2[1], slot.calleeAt, scan, s);
    if (!on || on.startsWith(CTOR)) return null;
    const cbType = argTypeName(on, m2[2], slot.arg);
    const args = cbType && callbackArgs(cbType);
    const a2 = args && args[slot.param];
    return a2 ? ifaceOfType(a2.idlType) : null;
  }
  /* …and a parameter of a function THIS FILE declares is typed by the arguments this file passes it — see
     §localParamSlot. The cycle guard is keyed on the FUNCTION AND SLOT rather than on the name, because a
     recursive helper reaches its own parameter through a different binding and the name key would not see it. */
  const lp = scan.localParamSlot(t, off);
  if (!lp) return null;
  const fnKey = `${scan.file}\0fn:${lp.fnName}#${lp.param}`;
  if (s.has(fnKey)) return null;
  s.add(fnKey);
  const passed = scan.callArgsOf(lp.fnName, lp.param, lp.declParen);
  if (!passed || !passed.length) return null;
  let one = null;
  for (const a of passed) {
    /* EACH CALL SITE GETS ITS OWN COPY OF THE GUARD, because two sites passing the SAME identifier are not a
       cycle and reading them as one answered the whole question `null`. THE MEASURED CASE was
       `_urlList(parsed.href, resp)` in `safe-fetch.js`, four times in one body: with a shared set the first
       `resp` resolved, added its key, and the second read its own answer as a loop — so a helper called
       consistently was decided only when its callers happened to spell the argument differently, which is the
       tokenizer deciding, not the constructs. That helper now takes `(requested, finalHref, redirected)` and
       the `resp` it re-read is gone (the chokepoint parses the landed URL once, above every gate that judges
       it), so the four repeated sites are `_urlList(parsed.href, _finalHref, resp.redirected)` and the rule
       they demonstrated is unchanged — a fixed illustration, not a fixed defect. */
    const got = ifaceOfExpr(a.text, a.at, scan, new Set(s));
    if (!got || (one !== null && one !== got)) return null;
    one = got;
  }
  return one;
}

/* ---- the ORIGIN of a value: the same specifier, followed further than one callback parameter --------------- */

/* THE FOREIGN ARM ABOVE ANSWERS FOR ONE CONSTRUCT AND THE QUESTION IS BIGGER THAN THAT CONSTRUCT. Its sentence
 * is right — "the specifier says where the producer is" — and the shape it reads it off is a callback PARAMETER,
 * which is one of the ways a value from outside this corpus reaches a receiver and not the only one. A value
 * that a package RETURNED (`parse(text)` from `webidl2`), that a `node:` builtin READ off the disk, or that this
 * corpus DECODED out of bytes it did not write (`JSON.parse` of a fetched body) is produced outside the corpus by
 * exactly the same reasoning, and it reached the shape anchor instead — where identity is decided by NAME
 * COLLISION, which is the failure the foreign arm exists to stop.
 *
 * WHAT THIS IS AND IS NOT. It is a walk over CONSTRUCTS, the same kind `ifaceOfExpr` runs and in the same order:
 * a declaration, a loop header, a call site, a spread. It is NOT a dataflow — nothing is followed through a
 * promise, a closure, a mutation or a path, and every arm that could answer twice must have its answers AGREE or
 * it answers nothing, which is the under-crediting direction this file takes everywhere.
 *
 * `JSON.parse` IS NOT A PRODUCER, IT IS A DECODER. The names in its result were written by whoever wrote the
 * bytes; asking who called `JSON.parse` answers the wrong question, exactly as asking who called `Response.text`
 * would. So the walk passes straight through it to its operand, and the operand is where the answer is.
 *
 * A FOREIGN ARRAY'S ELEMENTS ARE FOREIGN, which is why the element question is asked as the value question
 * first. Where that answers nothing there are element constructs of their own — what a `for … of` iterates, what
 * is pushed into an array, what an element-visiting callback is handed, and what `map`/`flatMap` build out of
 * their callback's own answer WHEN THAT ANSWER IS A PATH ROOTED AT THE CALLBACK'S OWN PARAMETER, which is the
 * only shape of it this walk may read — see §MAP_PATH for why the general shape is not readable here.
 *
 * THE UNDER-CREDITING DIRECTION, STATED: a corpus record handed to a package and returned by it — `sortBy(recs,
 * f)` — would be decided foreign, because the specifier is all an import can say and it says the same thing
 * either way. That is the FOREIGN arm's own residual, unchanged by widening it; its absence shows the same way,
 * as the field's producer standing in WRITE-WITH-NO-READER with its file and line. */

const SELECT_METHOD = new Set(["filter", "slice", "sort", "concat", "reverse", "flat", "toSorted", "toReversed"]);
const ELEMENT_CALLBACK = new Set(["map", "flatMap", "forEach", "filter", "some", "every", "find", "findIndex",
                                  "findLast", "findLastIndex", "sort", "flat"]);
const BODY_READER = new Set(["text", "json", "arrayBuffer", "blob", "formData", "bytes"]);
const MAP_METHOD = new Set(["map", "flatMap"]);

/* §MAP_PATH — `X.map(cb)`'s ELEMENTS ARE `cb`'s ANSWER, AND ONE SHAPE OF THAT ANSWER IS READABLE WITH NO SCOPE
 * LOOKUP AT ALL. The general shape is not: resolving a name inside `cb`'s body needs the body's own OFFSET, and
 * a declaration reaches this walk as TEXT — `initOf` hands back `code.slice(w.rhs, e)` whitespace-collapsed and
 * drops `w.rhs`, so every name inside it is resolved at the READER'S offset instead. That is harmless for a
 * declaration in an enclosing scope (the same binding is found) and WRONG inside a nested function literal,
 * where the spelling is bound by the literal and the reader's scope binds something else of the same name. An
 * arm that resolved a callback body that way would not under-credit, it would answer CONFIDENTLY about another
 * binding — the one outcome a decided category must never have.
 *
 * A PATH ROOTED AT THE CALLBACK'S OWN PARAMETER has no free name in it: `(g) => g.encodings` binds `g` in the
 * very text being read, so nothing has to be looked up anywhere. And the answer follows from a rule this walk
 * already makes one construct earlier — §the mem arm's "a member off a foreign base is foreign" — so the
 * elements of `X.map((p) => p.k)` are produced by whoever produced the elements of `X`, for `map` and for
 * `flatMap` alike (a foreign array's elements are foreign, so flattening one changes no answer). The path may
 * be empty (`(p) => p`), which is the identity map.
 *
 * IT CAN ONLY EVER AGREE WITH THE CONTAINER: where `X`'s elements are a record this corpus builds, this arm
 * answers nothing, exactly as the mem arm does. What it does NOT read is a body that computes — a call, an
 * object literal, a spread — and that is the residual: `registry.flatMap((g) => g.encodings.map((e) => ({ ...e,
 * … })))` builds its elements HERE out of foreign parts, and naming their producer needs both the offset plumbing
 * above and a rule for which of a spread literal's names came from the spread. WHAT THE NEXT DIFF BUILDS is the
 * offset half: `initOf` returning each declaration's own `w.rhs` beside its text, as `returnsOf` already returns
 * `{text, at}` — and ITS ABSENCE SHOWS as a receiver standing in AMBIGUOUS whose every read is of a field some
 * package or fetched body wrote, reached through a callback whose body is not a path. */
const pathOverParams = (cbText) => {
  const t = (cbText || "").trim();
  /* Only an ARROW, and only one whose body is an expression: a `function` literal's body is a block whose
     `return` is a statement this does not read, and a block-bodied arrow is the same construct. */
  const m = /^\(?\s*([^)=]*?)\s*\)?\s*=>\s*/.exec(t);
  if (!m || /^\s*async\b/.test(t)) return false;
  const params = m[1].split(",").map((p) => p.trim());
  if (!params.length || !params.every((p) => /^[A-Za-z_$][\w$]*$/.test(p))) return false;
  let body = t.slice(m[0].length).trim();
  while (body[0] === "(" && matchAt(body, 0) === body.length) body = body.slice(1, -1).trim();
  const path = /^([A-Za-z_$][\w$]*)(?:\s*\.\s*[A-Za-z_$][\w$]*)*$/.exec(body);
  return !!path && params.includes(path[1]);
};

const originName = (spec) => `the \`${spec}\` module`;

/* §REMOTE-STATED — THE ONE PRODUCER ON THIS WALK THAT IS NOT A MODULE, AND THE ONLY ONE WHOSE ROWS MAY NOT BE
 * TOLD TO ASSERT. Every other answer `originOfExpr` gives names a package, a `node:` builtin or a callback of
 * one: a producer that is IN THIS PROCESS, whose declaration a reader can open, and about which a wrong
 * assumption is this codebase's own to hold. A body read off a `Response` is none of those — nobody wrote the
 * producer, nobody can read it, and it can answer differently on the next request.
 *
 * IT IS A NAMED CONSTANT SO THE SPLIT AT THE PRINT IS DERIVED FROM THE SITE THAT MINTS IT rather than from a
 * second copy of the same rule spelled as a text match. §AN-AUDITOR-DERIVES-THE-RULE-IT-CHECKS: the day a
 * second remote producer is minted, it is minted through this constant and the band knows; a `/response/` test
 * at the print would have been the third copy of a fact whose second copy is what this gate exists to catch. */
const REMOTE_BYTES = "a fetched HTTP response body";
const isRemoteStated = (from) => from === REMOTE_BYTES;

/* Strip what does not change WHICH producer made the value: `await`, a grouping, and a `||`/`??` whose right
   operand is a LITERAL — the default is the concealment this gate reports elsewhere and never the producer. */
function originHead(t) {
  let s = (t || "").trim();
  for (;;) {
    if (/^await\s/.test(s)) { s = s.slice(6).trim(); continue; }
    if (s[0] === "(" && matchAt(s, 0) === s.length) { s = s.slice(1, -1).trim(); continue; }
    const alt = /(\|\||\?\?)\s*(\{\s*\}|\[\s*\]|(["'])(?:[^\\]|\\.)*?\3|-?\d[\w.]*|null|undefined|true|false)\s*$/.exec(s);
    if (alt && splitTopText(s, alt.index)) { s = s.slice(0, alt.index).trim(); continue; }
    break;
  }
  return s;
}
/* §identOnly — THE NAME THESE ASK ABOUT IS A BARE IDENTIFIER, AND UNTIL THERE WERE TWO CALLERS NOTHING SAID SO.
   `iterOf` and `pushArgsOf` INTERPOLATE the name into a RegExp, so a receiver text this file normalizes
   perfectly well — `ordSeries[i-1]` — turns `[i-1]` into a CHARACTER CLASS and `new RegExp` throws "Range out
   of order in character class" from inside the helper, naming neither the caller nor the contract. The call
   sites each guard with `/^[A-Za-z_$][\w$]*$/` and are correct; what was missing was the statement at the
   ORIGIN, which is what the next caller would otherwise discover the expensive way. It was discovered that way
   once already, while §bindingDecl was being measured: one probe asked these about a RECEIVER instead of a
   BINDING and the whole gate aborted three frames from the mistake with a message about regular expressions. */
function identOnly(fn, name) {
  if (!/^[A-Za-z_$][\w$]*$/.test(name))
    throw new Error(`${fn}: \`${name}\` is not a bare identifier. This builds a RegExp out of the name, so it ` +
                    `may only ever be asked about a BINDING — a receiver EXPRESSION reaching it is a caller ` +
                    `that skipped the identifier test its neighbours make.`);
}

/* Is `at` at bracket depth zero in `s`? A `||` inside a call's arguments is not this expression's alternative. */
function splitTopText(s, at) {
  let d = 0;
  for (let i = 0; i < at; i++) {
    const c = s[i];
    if (c === "(" || c === "[" || c === "{") d++;
    else if (c === ")" || c === "]" || c === "}") d--;
  }
  return d === 0;
}
/* `X.y` split at the LAST top-level dot, or null. */
function memberSplit(t) {
  let d = 0, dot = -1;
  for (let i = 0; i < t.length; i++) {
    const c = t[i];
    if (c === "(" || c === "[" || c === "{") d++;
    else if (c === ")" || c === "]" || c === "}") d--;
    else if (c === "." && !d) dot = i;
  }
  if (dot < 0) return null;
  const name = t.slice(dot + 1).trim();
  return /^[A-Za-z_$][\w$]*$/.test(name) ? { base: t.slice(0, dot).trim(), name } : null;
}
/* A trailing call `F(a, b)`: its callee text and its argument texts, or null. */
function callSplit(t) {
  if (!t.endsWith(")")) return null;
  let d = 0, open = -1;
  for (let i = t.length - 1; i >= 0; i--) {
    const c = t[i];
    if (c === ")" || c === "]" || c === "}") d++;
    else if (c === "(" || c === "[" || c === "{") { d--; if (!d) { open = i; break; } }
  }
  if (open <= 0) return null;
  const callee = t.slice(0, open).trim();
  if (!callee) return null;
  const args = splitTop(t, open + 1, t.length - 1).map(([a, b]) => t.slice(a, b).trim());
  return { callee, args };
}
/* The module a relative specifier names, as a repo-relative path this gate's own scan map is keyed by. */
function resolveModule(fromFile, spec) {
  if (!/^\./.test(spec)) return null;
  const dir = fromFile.includes("/") ? fromFile.slice(0, fromFile.lastIndexOf("/")) : "";
  const parts = (dir ? dir.split("/") : []).concat(spec.split("/"));
  const out = [];
  for (const p of parts) {
    if (p === "." || p === "") continue;
    if (p === "..") { out.pop(); continue; }
    out.push(p);
  }
  return out.join("/");
}

/* THE THREE QUESTIONS ARE NOT ONE. `mode` is "value" (who made the object this expression denotes), "elem" (who
 * made the elements of the array it denotes) or "bytes" (WHO WROTE THE TEXT it denotes) — and the third is the
 * one that keeps a decode honest. `JSON.parse(x)` does not produce a record, it DECODES one, so the question it
 * forwards is who wrote `x`; and a `readFileSync` answers that with a NODE OBJECT and not with a producer,
 * because THIS CORPUS WRITES FILES TOO. Collapsing the two made `JSON.parse(readFileSync(qjs.mjs.build.json))`
 * read as "produced by node:fs" when `build.mjs` wrote every name in it — a decided negative over a corpus seam,
 * which is the one direction a decided category must never be wrong in.
 *
 * `st` carries the cycle guard, and nothing else: it USED to carry the names the receiver reads, written by the
 * one caller and read by NO arm here — this gate's own WRITE-WITH-NO-READER, in this gate's own state object,
 * for as long as the walk has existed. Its verdict line for that case is "delete the half of the contract that
 * has gone stale", so it is deleted rather than left as an affordance somebody might one day read. */
function originOfExpr(t0, off, scan, st, mode) {
  const t = originHead(t0);
  if (!t || !scan) return null;
  const key = `${scan.file}\0${t}\0${mode}\0${off}`;
  if (st.seen.has(key)) return null;
  st.seen.add(key);
  const elem = mode === "elem", bytes = mode === "bytes";

  if (elem) {
    /* A FOREIGN CONTAINER'S ELEMENTS ARE FOREIGN — asked first, so the arms below are only the cases where the
       container itself is something this corpus built out of foreign parts. */
    const whole = originOfExpr(t, off, scan, st, "value");
    if (whole) return whole;
  }

  const call = callSplit(t);
  if (call) {
    /* `require("pkg")` IS AN IMPORT SPELT AS A CALL, and the specifier means what it means in either spelling.
       CommonJS is how the testing drivers are written and nothing above reads it. It names the producer of the
       MODULE OBJECT and says nothing about bytes some other party wrote. */
    const req = call.callee === "require" && call.args.length === 1 &&
                /^(["'])([^"'\n]+)\1$/.exec(call.args[0]);
    if (req && !/^[./]/.test(req[2])) return bytes ? null : originName(req[2]);
    if (call.callee === "JSON.parse" && call.args.length)
      return bytes ? null : originOfExpr(call.args[0], off, scan, st, "bytes");
    const cm = memberSplit(call.callee);
    if (cm) {
      if (SELECT_METHOD.has(cm.name) && !bytes) return originOfExpr(cm.base, off, scan, st, mode);
      /* §MAP_PATH: `X.map((p) => p.k)` builds its elements out of the callback's answer, and a path rooted at
         the callback's own parameter is that answer read with nothing looked up. Asked only of the ELEMENT
         question — who made `X.map(cb)` ITSELF is this corpus, which is the answer the `value` mode already
         gives by declining. */
      if (elem && MAP_METHOD.has(cm.name) && call.args.length === 1 && pathOverParams(call.args[0]))
        return originOfExpr(cm.base, off, scan, st, "elem");
      if (BODY_READER.has(cm.name) && ifaceOfExpr(cm.base, off, scan, null) === "Response")
        /* Fetch §5.3 Body mixin: these read THE BODY, and a body is bytes some SERVER wrote — an answer to the
           bytes question, which is exactly the one a filesystem read cannot answer. §REMOTE-STATED. */
        return bytes ? REMOTE_BYTES : null;
      /* A NODE API'S RETURN VALUE IS NODE'S RECORD and the bytes it carries are not. `readFileSync` answers a
         Buffer or a string whose CONTENT this corpus may well have written; the object is `node:fs`'s and the
         text is nobody-this-can-name's, which is why the two questions part company here. */
      if (bytes) return null;
    }
    /* A CALL OF A NAME THIS CORPUS DECLARES ANSWERS WITH ITS OWN `return`s — in this module, or in the one a
       relative specifier names. Every return must agree, as every declaration of a binding must, and each is
       resolved AT ITS OWN OFFSET so a name inside it is looked up in the scope that wrote it. */
    if (/^[A-Za-z_$][\w$]*$/.test(call.callee)) {
      const spec = scan.importOf.get(call.callee);
      if (spec && !/^[./]/.test(spec)) return bytes ? null : originName(spec);
      const home = spec ? scanByFile.get(resolveModule(scan.file, spec)) : scan;
      const rets = home && home.returnsOf(call.callee);
      if (rets) return agreeOrigin(rets.map((r) => originOfExpr(r.text, r.at, home, st, mode)));
    }
    return null;
  }

  const mem = memberSplit(t);
  if (mem) {
    const base = originOfExpr(mem.base, off, scan, st, "value");
    if (base) return bytes ? null : base;
    /* WHERE THE BASE IS A RECORD THIS CORPUS BUILT, the property is decided by the LITERAL that built it — the
       one construct that says what a returned object's field holds. `loadIdl()` answers `{ declarations, … }`
       and `idl.declarations` is that shorthand, resolved in the module that wrote it. */
    const lit = literalOf(mem.base, off, scan, st);
    if (lit) {
      const v = literalProp(lit.text, mem.name);
      if (v) return originOfExpr(v, lit.at, lit.scan, st, mode);
    }
    return null;
  }

  if (/^[A-Za-z_$][\w$]*$/.test(t)) {
    const spec = scan.importOf.get(t);
    if (spec && !/^[./]/.test(spec)) return bytes ? null : originName(spec);
    /* WHICH CONSTRUCT DECLARES THE BINDING IS §bindingDecl'S QUESTION, and the order it answers in — a `const`
       `for … of` head, then the writes, then a `let`/`var` head — is ECMAScript's rather than this walk's, which
       is why it is stated once there and read twice rather than spelled here and there.
       A `for … of` BINDING IS AN ELEMENT of what the header iterates, so the VALUE question about the binding is
       the ELEMENT question about the header, and there is no arm for the elements OF a loop variable. */
    const decl = bindingDecl(scan, t, off);
    if (decl && decl.iter)
      return elem || bytes ? null : originOfExpr(decl.iter.text, decl.iter.at, scan, st, "elem");
    if (decl) {
      const decls = decl.writes;
      const answers = decls.filter((d) => d && d !== "null" && d !== "undefined")
                           .map((d) => originOfExpr(d, off, scan, st, mode));
      const one = agreeOrigin(answers);
      if (one) return one;
      /* `const decls = []` states an EMPTY array and every element it holds is named by a push. Asked only
         where the declarations themselves answered nothing, so a binding that says what it is is not
         second-guessed by what was appended to it. */
      if (elem && decls.every((d) => /^\[\s*\]$/.test(d || ""))) {
        const pushed = scan.pushArgsOf(t, off);
        if (pushed) return agreeOrigin(pushed.map((p) => originOfExpr(p.text, p.at, scan, st,
                                                                     p.spread ? "elem" : "value")));
      }
      return null;
    }
    const ps = scan.paramSlot(t, off);
    if (ps && mode === "value" && ps.param === 0) {
      const cm2 = memberSplit(ps.callee);
      if (cm2 && ELEMENT_CALLBACK.has(cm2.name)) return originOfExpr(cm2.base, ps.calleeAt, scan, st, "elem");
    }
    return null;
  }
  return null;
}
/* Every answer must be the same answer, and every arm must have answered. */
function agreeOrigin(answers) {
  if (!answers.length) return null;
  let one = null;
  for (const a of answers) { if (!a || (one !== null && one !== a)) return null; one = a; }
  return one;
}
/* The object literal a name is declared to hold, with the OFFSET and the MODULE it was written at — both,
   because a name inside it is resolved in the scope that wrote it and nowhere else. */
function literalOf(t, at, scan, st) {
  const s = originHead(t);
  if (/^\{/.test(s) && matchAt(s, 0) === s.length) return { text: s, at, scan };
  const call = callSplit(s);
  if (call && /^[A-Za-z_$][\w$]*$/.test(call.callee)) {
    const spec = scan.importOf.get(call.callee);
    if (spec && !/^[./]/.test(spec)) return null;
    const home = spec ? scanByFile.get(resolveModule(scan.file, spec)) : scan;
    const rets = home && home.returnsOf(call.callee);
    if (rets && rets.length === 1) return literalOf(rets[0].text, rets[0].at, home, st);
    return null;
  }
  if (/^[A-Za-z_$][\w$]*$/.test(s)) {
    const decls = scan.initOf(s, at);
    if (decls && decls.length === 1) return literalOf(decls[0], at, scan, st);
  }
  return null;
}
/* The expression an object literal binds to `name` — `{ name: e }` or the `{ name }` shorthand — or null. The
   caller resolves it at the LITERAL's own offset rather than at the property's: a literal's text is whitespace-
   normalized, so an offset computed inside it is not an offset in the file, and every property of one literal is
   in one scope anyway. Naming the literal's start is exact; naming the property's would be arithmetic on a
   string this no longer has the original spacing of. */
function literalProp(lit, name) {
  for (const [a, b] of splitTop(lit, 1, lit.length - 1)) {
    const seg = lit.slice(a, b).trim();
    const kv = new RegExp(`^(?:["']?)${name}(?:["']?)\\s*:\\s*([\\s\\S]+)$`).exec(seg);
    if (kv) return kv[1].trim();
    if (seg === name) return name;
  }
  return null;
}

/* WHICH INTERFACE IN `base`'s SUBTREE THE READ SET FITS, AND WHAT IS STILL TRUE WHEN IT FITS SEVERAL. An IDL
   return type is an upper bound — `getElementById` answers `Element?` and the object is an HTMLSelectElement —
   so the construct fixes the subtree and the names pick within it: the candidate covering the MOST of them
   wins, and an inheritance chain collapses to its most general matching member.
   IT NEVER ANSWERS "NOTHING", because there are two questions here and only one of them can go unanswered —
   §THE TWO QUESTIONS A BASE ANSWERS. `narrowed` is false where the read set fits several unrelated candidates,
   and `iface` is then the BASE the construct named, which is the whole of the answer to WHOSE record this is.
   `fit` is how many candidates tied, so the row can say on its own line how open it stayed.
   AND THE MEMBER DIFF FOLLOWS THE SAME SPLIT. Narrowed, a name is off the interface when that one interface
   does not declare it. Open, it is off the object only when NO CANDIDATE IN THE SUBTREE declares it — which is
   sound whichever candidate the object turns out to be, and is the weakest test available, which is the
   direction this file takes everywhere. Accusing against the base alone would report `value` on every Element
   the popup reads, which is the finding manufactured out of the spec being read too literally that the
   paragraph above exists to stop. */
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
  const only = general.length === 1 ? general[0] : null;
  /* AN EXPANDO IS NOT AN OFF-INTERFACE READ, and telling them apart is the difference between this category and
     a listing of every property the extension installs on a page. `window.apiclientsink` is not in any IDL and
     never will be — it is the PoC's own fire marker, written by intercept.js on the line above — so the read
     has a producer and the seam it belongs to is the one this gate already audits. A name off the interface
     with NO producer anywhere in the corpus is the other thing entirely: nobody writes it and the spec denies
     it, which is this gate's own defect class landing on a platform object. */
  const declares = only ? (n) => identityMembers(only).has(n)
                        : (n) => cands.some((c) => identityMembers(c)?.has(n));
  const unwritten = [...names].filter((n) => !declares(n) && !(fields.get(n)?.writes.length));
  return { iface: only || base, narrowed: !!only, fit: general.length,
           ...(unwritten.length ? { missing: unwritten } : {}) };
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
  ambiguous.push({ file: cm.file, line: cm.line, recv: "a matcher pattern", reason: ONE_FIELD,
                   why: oneFieldWhy(cm.keys.find((k) => !unwritten.includes(k)), unwritten) });
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
    /* AND ASKED AT EVERY READ, NOT AT THE FIRST ONE, because ONE of the constructs that answers it is a fact
       about a POSITION rather than about the binding. Every other arm of `ifaceOfExpr` reads the declaration
       the group's own key is qualified by — the reads of one group share a binder by construction, so those
       arms cannot disagree across it and asking `rs[0]` was the same as asking all of them. An `instanceof`
       guard is not like that: it holds inside its own consequent and nowhere else, so a receiver read both
       inside and outside one is a union whose arms are two different objects. Deciding it from `rs[0]` would
       pick whichever arm the file happened to write first — the tokenizer deciding, which is the thing this
       file refuses everywhere — so a disagreement is AMBIGUOUS and is counted as one. */
    const bases = rs.map((r) => ifaceOfExpr(recv, r.off, s, null));
    const base = bases.every((b) => b === bases[0]) ? bases[0] : undefined;
    if (base === undefined) {
      const named = [...new Set(bases.filter(Boolean))];
      ambiguous.push({ ...s.site(rs[0].off), recv,
                       reason: "a receiver an `instanceof` guard narrows at SOME of its reads and not at others" +
                               " — the identity is a union this cannot collapse, and neither arm may anchor the other",
                       why: `is ${named.map(anIface).join(" or ")} only inside its guard, and reads ` +
                            `${[...names].map((n) => `\`${n}\``).join(", ")} across both` });
      continue;
    }
    /* THE BASE DECIDES THE BAND AND THE NARROWING DECIDES ONLY WHAT MAY BE SAID ABOUT MEMBERS — §THE TWO
       QUESTIONS A BASE ANSWERS. There is no `if (narrowed)` here any more, and its absence is the fix: a
       construct that names a Web IDL type has already answered the only question this band asks, and letting
       an open narrowing carry that answer away sent the receiver on to an anchor decided by name collision. */
    if (base) {
      const narrowed = narrowIdentity(base, names);
      const site = { ...s.site(rs[0].off), recv, iface: narrowed.iface, base, names: [...names],
                     open: narrowed.narrowed ? 0 : narrowed.fit };
      if (narrowed.missing) offInterface.push({ ...site, missing: narrowed.missing });
      else platformDecided.push(site);
      continue;
    }
    /* ASKED AFTER THE IDL AND BEFORE THE SHAPE, in that order for one reason each: a receiver Web IDL can name
       is named by it, and a receiver it cannot must not be handed to an anchor that decides by name collision. */
    {
      const ps = rs.map((r) => s.paramSlot(recv, r.off)).find(Boolean);
      /* THE CALLEE MUST BE THE IMPORTED BINDING ITSELF, or a member path rooted at one — never a name that
         merely BEGINS the callee text. `readdirSync(d, …).sort((a, b) => …)` is rooted at an import and the
         party calling that callback is `Array.prototype.sort`, so taking the leading identifier answered
         "foreign" for a receiver whose producer is decided by something else entirely. It happened to be right
         about those two (`Dirent` is `node:fs`'s) and right for a reason that is not a reason, which is the one
         outcome worse than no answer. A callee holding a `(` or a `[` is a call or an index, and the thing it
         returns is not the binding this file imported. */
      const callee = ps && /^([A-Za-z_$][\w$]*)(?:\.[A-Za-z_$][\w$]*)*$/.exec(ps.callee);
      if (callee && s.foreignImports.has(callee[1])) {
        foreignDecided.push({ ...s.site(rs[0].off), recv, names: [...names],
                             why: `is a callback argument of \`${ps.callee}\``, by: ps.callee });
        continue;
      }
      /* …AND THE SAME SPECIFIER, FOLLOWED FURTHER. A callback parameter is one way a value from outside this
         corpus reaches a receiver; a package's RETURN, a `node:` builtin's read of the disk and a `JSON.parse`
         of bytes this corpus did not write are others, and every one of them would otherwise be handed to an
         anchor that decides by name collision. See §the ORIGIN of a value. */
      /* ASKED AT EVERY READ, NOT AT THE FIRST ONE, for the reason the `instanceof` arm above states: one of the
         constructs that answers this is a fact about a POSITION — which loop body a read stands in — and a
         group's reads can stand in two. Deciding from `rs[0]` would be the tokenizer deciding. */
      const froms = rs.map((r) => originOfExpr(recv, r.off, s, { seen: new Set() }, "value"));
      const from = agreeOrigin(froms);
      if (from) {
        foreignDecided.push({ ...s.site(rs[0].off), recv, names: [...names],
                             why: `is produced by ${from}`, by: from });
        continue;
      }
    }
    let best = null, bestN = 0;
    for (const [id, shp] of shapes) {
      let hit = 0;
      for (const n of names) if (shp.has(n)) hit++;
      if (hit > bestN) { bestN = hit; best = id; }
    }
    /* ASKED AFTER THE C SHAPE AND BEFORE ANY READ IS RECORDED. A JS record only ever REMOVES a C anchor, never
       supplies one: where it explains strictly more of this receiver than the emission does, the receiver is
       that JS record and the serialized seam is not the contract it is on — a DECIDED negative in the same
       series as DECIDED PLATFORM and DECIDED FOREIGN, printed in full with both candidates and both scores so
       the decision is somewhere a reader can disagree with it. Where the two explain EXACTLY as much, the
       identity is a disagreement this cannot collapse, and the file's own answer to that is AMBIGUOUS — a
       wrong anchor is worse than an undecided one, and picking the emission because it is this gate's subject
       would be the tokenizer deciding. */
    if (bestN >= 2) {
      let jBest = null, jN = 0;
      for (const [id, shp] of jsShapes) {
        let hit = 0;
        for (const n of names) if (shp.has(n)) hit++;
        if (hit > jN) { jN = hit; jBest = id; }
      }
      if (jN >= bestN) {
        const row = { ...s.site(rs[0].off), recv, names: [...names], cShape: best, cN: bestN, jsShape: jBest, jsN: jN };
        if (jN > bestN) {
          corpusDecided.push(row);
          /* THE IDENTITY LEAVES THIS GATE'S SUBJECT; THE FORM DOES NOT. Being a corpus-local record answers
             "whose contract is this" and says NOTHING about `|| 0` — §the CATEGORIES calls a default a defect
             "on its own terms even where the writer exists today, because it is what will hide the writer's
             removal tomorrow", and that sentence is about the CONSUMER's form, not about which producer is on
             the other end. Dropping these rows with the anchor would have taken the DEFAULTED count from 32 to
             ZERO in the same diff that fixed the anchor — every row individually justified and the category
             emptied, which is the narrowing that makes an auditor read clean by asking less. So the reads are
             not recorded into `fields` (that namespace is the serialized seam's, and this record is not on it)
             and the defaulted form is reported exactly as before, against the record that actually owns it. */
          for (const r of rs) {
            const site = { ...s.site(r.off), recv, shape: jBest };
            if (r.form) defaulted.push({ ...site, name: r.name, form: r.form });
            else if (r.consumed) decidedOperand.push({ ...site, name: r.name, ...r.consumed });
            else if (r.coerced) coerced.push({ ...site, name: r.name, ...r.coerced, where: r.coercedAt, disp: "a record this corpus constructs" });
          }
        }
        else {
          for (const r of rs)
            if (r.coerced && !r.form && !r.consumed)
              coerced.push({ ...s.site(r.off), recv, name: r.name, ...r.coerced, where: r.coercedAt, disp: "AMBIGUOUS — two records explain it equally" });
          ambiguous.push({ ...s.site(rs[0].off), recv, reason: TWO_RECORDS,
                              why: `reads ${bestN} field(s) of the emission at ${best} and the same ${jN} of the ` +
                                   `record constructed at ${jBest} — \`${[...names].join("`, `")}\`` });
        }
        continue;
      }
    }
    if (bestN < 2) {
      for (const r of rs)
        if (r.coerced && !r.form && !r.consumed)
          coerced.push({ ...s.site(r.off), recv, name: r.name, ...r.coerced, where: r.coercedAt,
                         disp: bestN === 1 ? (shapes.get(best).has(r.name) ? "UNDECIDED — one emitted name, and this is it"
                                                                            : "UNDECIDED — one emitted name, and this is not it")
                                           : "UNDECIDED — no emitted name at all" });
      /* ONE intersecting name is not an anchor and it is not a pass either. Reported only where the receiver
         also reads a name NO producer emits — which is the only configuration in which the undecided question
         could be hiding this defect, and reporting the rest would be reporting the language again. */
      if (bestN === 1 && [...names].some((n) => !(fields.get(n)?.writes.length))) {
        /* AND THE CONSUMER'S OWN ASSERT ANSWERS THE HALF OF THIS QUESTION THAT MATTERS. The row above states two
           undecided things and they are not equally load-bearing: WHICH record this is, and whether the names no
           producer writes are the DEFECT. §Architecture's remedy for the second is verbatim "it DCHECKs the
           field's presence and shape", and a receiver every one of whose names is already asserted at a read has
           applied that remedy — whatever the record turns out to be, a name its producer does not write is an
           ABORT at the read and not a plausible datum, which is the entire thing this category is protecting.
           So it is DECIDED on a construct, not passed: the identity stays open and is printed saying so.
           ASKED ONLY HERE, inside the one-intersecting-name branch. Where the shape anchor DID decide the
           identity the question is a different one — that record's own contract — and an assert is no answer to
           it, so an arm placed above the anchor would remove audited receivers instead of undecided ones. */
        const unasserted = [...names].filter((n) => !rs.some((r) => r.name === n && s.asserted(r.off)));
        if (!unasserted.length) {
          assertedDecided.push({ ...s.site(rs[0].off), recv, names: [...names] });
          continue;
        }
        ambiguous.push({ ...s.site(rs[0].off), recv, reason: ONE_FIELD,
                         why: oneFieldWhy([...names].filter((n) => cEmitted.has(n))[0],
                                          [...names].filter((n) => !(fields.get(n)?.writes.length))) });
      }
      continue;
    }
    for (const r of rs) {
      const site = { ...s.site(r.off), recv, shape: best };
      /* THE WRITE QUESTION IS ASKED OF THE RECORD THIS READ ANCHORED TO, WHICH IS THE ONLY PARTY THAT CAN
         ANSWER IT. Asking it of the corpus — "does anything, anywhere, write this name" — lets ANY unrelated
         record excuse a field of THIS one, and a collision is not a producer: a `_cold:` on a bridge resolver
         literal, a `securitySinks:` on a merge record and a `printf` in a test driver were between them
         standing in for sixteen keys of one engine composer, so the gate's silence about that seam was never a
         clean bill and a rename on either side of it would have passed. A name the anchored record does not
         emit is reported HERE, with the places it IS written, which is what tells a reader in one line which of
         the two things they are looking at: a producer that renamed a field out from under a live consumer, or
         a receiver whose identity this file got wrong. The name written NOWHERE is not diverted into this
         category — it stays READ-NO-WRITER, which is louder and is already the right row for it. */
      if (!shapes.get(best).has(r.name)) {
        /* ASKED BEFORE THE ACCUSATION, AT THE SHAPE AND NOT AT THE FILE. The question is whether THIS consumer
           writes this name onto a record of the SAME shape this read anchored to, and the construct that answers
           it is a member ASSIGNMENT whose own receiver reads this shape's rows — the same two-name threshold the
           anchor above is decided by, so there is one rule here and not a second copy of it. A file-wide test
           would have excused a read off an unrelated receiver that merely shares the file; this excuses only a
           record the consumer demonstrably holds. */
        const syn = fields.get(r.name)?.writes.length ? (() => {
          for (const a of s.memberAssigns) {
            if (a.name !== r.name) continue;
            const rs2 = byRecv.get(a.key);
            if (!rs2) continue;
            const on = new Set();
            for (const q of rs2) if (shapes.get(best).has(q.name)) on.add(q.name);
            if (on.size >= 2) return { ...s.site(a.off), recv: rs2[0].recv, on: [...on] };
          }
          return null;
        })() : null;
        if (r.coerced && !r.form && !r.consumed)
          coerced.push({ ...site, name: r.name, ...r.coerced, where: r.coercedAt,
                         disp: syn ? "the emission it anchors to, onto which this consumer wrote this name itself"
                                   : fields.get(r.name)?.writes.length ? "the emission it anchors to, which does NOT carry this name"
                                                                                : "no producer anywhere" });
        if (syn) consumerSynth.push({ ...site, name: r.name, at: syn });
        else if (fields.get(r.name)?.writes.length)
          offRecord.push({ ...site, name: r.name, writes: fields.get(r.name).writes.slice(0, 4) });
        else rec(fields, r.name).reads.push(site);
        continue;
      }
      rec(fields, r.name).reads.push(site);
      if (r.form) defaulted.push({ ...site, name: r.name, form: r.form });
      else if (r.consumed) decidedOperand.push({ ...site, name: r.name, ...r.consumed });
      else if (r.coerced) coerced.push({ ...site, name: r.name, ...r.coerced, where: r.coercedAt, disp: "the emission it anchors to, which carries this name" });
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
/* ---- the DERIVED reader ------------------------------------------------------------------------------- */
/* A CONSUMER THAT TAKES ITS REQUIRED FIELD SET OUT OF THE PRODUCER HAS NO LITERAL NAME TO GREP FOR, AND THE
 * CATEGORY ABOVE IS A NAME GREP. `readAnywhere` asks "does any construct in this corpus spell this token", and
 * a reader built to §AN-AUDITOR-DERIVES-THE-RULE-IT-CHECKS-FROM-THE-CODE-THAT-OWNS-IT spells none of them: it
 * reads the row set off the composer's own format string and then requires every row of it, so the day a row is
 * added the reader knows. Both instruments obey the same rule and they answer OPPOSITE things about the same
 * emission, and the one that is wrong is the one asking about tokens.
 *   THE COST IS NOT A WASTED READING, WHICH IS WHY THIS IS A DEFECT IN THIS FILE AND NOT NOISE IN IT.
 * §Architecture's prescribed remedy for a write-with-no-reader is to ADD a reader, and adding a LITERAL one
 * here re-creates the hand-typed list the derivation replaced — a list this tree records as having been wrong
 * SEVEN times, three of those misses landing in the hour the derivation that ended it was being written. So the
 * finding does not merely mislead: it argues for undoing the fix. A count that mixes "I found a defect" with "I
 * cannot see this construct" is the three-states-behind-one-answer shape arriving in the instrument built to
 * end it, and the answer is a BAND, decided and printed, never a silent exemption.
 *   AND IT IS DERIVED, NOT LISTED. A hand-kept set of exempt field names would be the EIGHTH instance of the
 * thing above. What is read instead is the CALL: a consumer that delimits a region of a producer by two
 * literals of that producer's own text has named the region, and this resolves it in the producer's bytes —
 * the same discipline `idlgen` follows in reading the real `.idl`, and the same one the derivation being
 * credited follows in reading the real format string. Nothing here types a field name, so a row added over
 * there joins the credited set on the same run it joins the census.
 *   WHAT MAKES IT A ROUTE RATHER THAN A FALLBACK, on §the-test-for-a-predicate: delete the derived readers and
 * this predicate answers NO for every emission and the rows come back as findings — it selects against nothing
 * and there is no second implementation for it to hide. It CLASSIFIES, and the class it puts a row in is
 * printed with the call site that put it there, so a reader can disagree in one line.
 *   NAMED RESIDUAL — WHAT THIS DOES NOT ESTABLISH. It reads that the call DELIMITS the region; it does not read
 * what the callee then does with it, because that would mean interpreting the callee's extraction. So a call
 * that delimited a producer's region for some purpose OTHER than taking its field names out would credit that
 * region's rows with a reader they do not have, and this category would UNDER-report — the direction
 * `readAnywhere` already takes deliberately, one construct over. The next diff that needs it resolves the
 * callee's declaration in the same file and confirms its extraction yields the region's own key spellings; its
 * absence shows as a locator printed below whose credited names nobody can find a use for. EVERY locator this
 * finds is printed, with its region and its rows, so the whole population is one screen and not an assertion. */
const regionLocators = [];    // {file, line, callee, cFile, from, to, lineA, lineB, emits:[], only:[]}
const derivedRead = new Map();   // field name -> the locator that answers for it
{
  const LIT = String.raw`"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'`;
  /* MATCHED ON THE MASKED `code`, where a comment is blank and a string literal's own characters are intact —
     so a locator written inside a comment cannot be one, and the literal is read as the bytes the callee gets. */
  const CALL = new RegExp(String.raw`([A-Za-z_$][\w$]*)\s*\(\s*(${LIT})\s*,\s*(${LIT})\s*,\s*(${LIT})`, "g");
  /* A JS STRING LITERAL'S OWN VALUE, in both spellings, because the one that locates the @HWORK line is single
     quoted precisely so it can hold the double quote that opens the format. Unreadable is NOT-A-LOCATOR rather
     than a guess: a literal this cannot decode is one whose bytes it cannot look for in the producer either. */
  const unlit = (t) => {
    try { return t[0] === '"' ? JSON.parse(t)
                              : JSON.parse('"' + t.slice(1, -1).replace(/\\'/g, "'").replace(/"/g, '\\"') + '"'); }
    catch { return null; }
  };
  const cPaths = [...cSrc.keys()];
  for (const s of jsScans) {
    let m;
    CALL.lastIndex = 0;
    while ((m = CALL.exec(s.code))) {
      if (m.index > 0 && /[\w$]/.test(s.code[m.index - 1])) continue;   /* the tail of a longer identifier */
      const path = unlit(m[2]), from = unlit(m[3]), to = unlit(m[4]);
      if (path === null || from === null || to === null || !path) continue;
      /* THE PRODUCER IS NAMED RELATIVE TO A ROOT THIS FILE DOES NOT KNOW, so the path is resolved by suffix
         against the corpus rather than by joining a root guessed at here — and a path that names two producers
         is REFUSED with its place, never picked between. */
      const fit = cPaths.filter((k) => k === path || k.endsWith("/" + path)).filter((k) => {
        const t = cSrc.get(k), a = t.indexOf(from);
        return a >= 0 && t.indexOf(to, a) >= 0;
      });
      if (!fit.length) continue;
      if (fit.length > 1) {
        refuse(s.file, lineOf(s.src, m.index),
               "a call delimiting a region of a producer by that producer's own text, whose file argument names " +
               "more than one file in this corpus — which region a consumer derives its field set from cannot " +
               "be decided, and crediting either would answer for rows of the other",
               `${m[1]}(${JSON.stringify(path)}, …) — ${fit.join(", ")}`);
        continue;
      }
      const cFile = fit[0], t = cSrc.get(cFile);
      const a = t.indexOf(from), b = t.indexOf(to, a) + to.length;
      const loc = { file: s.file, line: lineOf(s.src, m.index), callee: m[1], cFile, from, to,
                    lineA: lineOf(t, a), lineB: lineOf(t, b - 1), emits: [], only: [] };
      regionLocators.push(loc);
    }
  }
  /* A ROW IS ANSWERED BY THE REGION THAT CONTAINS ITS EMISSION, which is the WRITE SITE and never the enclosing
     body: the @HWORK region is one printf inside a function whose body opens seventy lines above it, so a body
     span would have credited that whole function and a body-keyed test would have credited nothing. */
  for (const [n, e] of fields) {
    if (!cEmitted.has(n)) continue;
    for (const loc of regionLocators)
      if (e.writes.some((w) => w.file === loc.cFile && w.line >= loc.lineA && w.line <= loc.lineB)) {
        loc.emits.push(n);
        if (!derivedRead.has(n)) derivedRead.set(n, loc);
        if (!readAnywhere.has(n)) loc.only.push(n);
        break;
      }
  }
}

/* AND THE DERIVED READER IS SUBTRACTED HERE AND NOWHERE ELSE — not folded into `readAnywhere`, because the two
   are different answers and a reader who cannot tell them apart is back where this started: `readAnywhere` says
   a construct SPELLS the token, and `derivedRead` says a consumer requires the row without spelling it. Merged,
   the band below could not print which rows are answered by which, and a derivation that silently stopped
   covering a region would look exactly like a row that grew a literal reader. */
const writeNoReader = [...fields].filter(([n, e]) => cEmitted.has(n) && e.writes.length && !readAnywhere.has(n) && !derivedRead.has(n));
const mReadNoWriter = [...markers].filter(([, e]) => e.reads.length && !e.writes.length);
const mWriteNoReader = [...markers].filter(([, e]) => e.writes.length && !e.reads.length);

const log = (s) => console.log(`[field-gate] ${s}`);
const byArea = new Map();
for (const f of files) byArea.set(f.area, (byArea.get(f.area) || 0) + 1);
log(`corpus — ${[...byArea].map(([a, n]) => `${a} ${n}`).join(", ")}; ` +
    `${fields.size} record field names and ${markers.size} stream markers carry a construct on at least one side`);
/* AND WHICH FORMAT-TAKING FUNCTIONS THIS TREE DECLARED FOR ITSELF, printed because a DERIVED vocabulary can
   collapse to nothing in total silence. The libc names are compiled in and cannot go missing; these are read
   off `APICLIENT_PRINTF` at each declaration, so removing that attribute — or spelling the declarator in a way
   this cannot parse — does not fail anything: it quietly returns this gate to the state it was fixed out of,
   where every key composed through the missing name reads as emitted by nobody and the SAME keys that happen
   to collide with an unrelated record read as fine. A count nobody can see is the concealment this file exists
   to report, performed on its own output, so the derivation states itself and an empty list is legible. */
{
  const derived = [...Object.entries(C_COMPOSE).map(([n, i]) => `${n}(→return, fmt@${i})`),
                   ...Object.entries(C_BUILD).filter(([n]) => !["snprintf", "sprintf"].includes(n))
                                             .map(([n, i]) => `${n}(→arg0, fmt@${i})`)];
  log(`derived C composers — ${derived.length ? derived.join(", ") : "NONE; only libc's printf/snprintf are " +
      "read, so any document composed through this fork's own format-taking helper is invisible as a write"}`);
}

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
/* THE JUDGED POPULATION, ON THE LINE THE FINDING IS ON AND ON EVERY RUN. §Testing records an instrument whose
   findings fell 978 → 688 while its VERIFIED count fell 4164 → 2490 — coverage collapsing and reading as
   accuracy, because the number the findings were drawn FROM was on a line nobody went to. A count of defects
   falls both when defects are fixed and when the instrument stops looking, and only the denominator tells those
   apart. It prints on the clean day too, for the reason the corpus census does: a line that appears only on the
   bad day is one nobody learns to look for. */
/* THE TWO ANSWERS OVERLAP AND THE OVERLAP IS STATED, because three numbers that do not add up to the population
   are three numbers a reader has to guess the relationship of — and a partition nobody can check is exactly the
   shape this line was added to stop. spelled + derived − both = judged, on every run. */
{
  const judged = [...fields].filter(([n, e]) => cEmitted.has(n) && e.writes.length).map(([n]) => n);
  const spelled = judged.filter((n) => readAnywhere.has(n)).length;
  const derived = judged.filter((n) => derivedRead.has(n)).length;
  const both = judged.filter((n) => readAnywhere.has(n) && derivedRead.has(n)).length;
  log(`serialized field names JUDGED for a reader — ${judged.length}: ${spelled} named by a construct that ` +
      `SPELLS them, ${derived} required by a consumer that DERIVES its row set from the producer (${both} both), ` +
      `${writeNoReader.length} answered by neither`);
}

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

/* PRINTED IN FULL AND NOT A DEFECT, on the rule DECIDED PLATFORM is printed under: a decided negative nobody
   can see is the concealment this file exists to report, performed on its own output. Every locator is a claim
   this scan made about which rows a consumer requires, so every locator is somewhere a reader can disagree —
   and the rows it is the ONLY answer for are named apart, because those are exactly the rows the category above
   would otherwise be accusing. An empty list is legible for the same reason the derived-composer line is: this
   whole band collapses to nothing if a call's spelling stops parsing here, and that would return the gate to
   accusing correctly-written code with nothing to say it had. */
if (regionLocators.length) {
  log(`── DERIVED READER — ${regionLocators.length} consumer(s) that take their required field set out of the ` +
      `producer, answering for ${derivedRead.size} row(s) that no construct anywhere SPELLS. A name grep cannot ` +
      `see these; adding one would re-create the hand list the derivation replaced ──`);
  for (const l of regionLocators)
    log(`  ${l.file}:${l.line}  ${l.callee}(…) delimits ${l.cFile}:${l.lineA}..${l.lineB} by \`${l.from.replace(/\n/g, "\\n")}\`` +
        ` → ${l.emits.length} emitted row(s)` +
        (l.only.length ? `, ${l.only.length} of them answered by NOTHING ELSE: ${l.only.sort().join(" ")}`
                       : `, every one of which a construct also spells`));
} else {
  log(`── DERIVED READER — NONE. No consumer in this corpus delimits a region of a producer by that producer's ` +
      `own text, so every emitted row above is judged by whether some construct SPELLS its name. If that is a ` +
      `surprise, the locator parse is what collapsed, not the derivations ──`);
}

/* NAMED, not "matched": a marker can be referred to by a diagnostic that tells the next reader to look for it,
   and a marker nothing prints is exactly as stale there as in a grep — the DFAIL failure mode one layer down. */
show(`NAMED marker with no writer — ${mReadNoWriter.length} stream marker(s) a consumer names and no emission prints`,
     mReadNoWriter, ([n, e]) => `${n.padEnd(16)} named at ${e.reads.slice(0, 3).map(place).join(", ")}`);
show(`WRITTEN marker with no reader — ${mWriteNoReader.length} stream marker(s) the engine prints and nothing matches`,
     mWriteNoReader, ([n, e]) => `${n.padEnd(16)} printed at ${e.writes.slice(0, 2).map(place).join(", ")}`);

const dByForm = new Map();
for (const d of defaulted) dByForm.set(d.form, (dByForm.get(d.form) || 0) + 1);
/* THE THREE BANDS, AND WHY ONLY ONE OF THEM ACCUSES. Every row below is a read standing under a conversion,
   which is the same fact; what differs is what the corpus DOES with the token, and that is what decides whether
   an absence is a wrong answer or merely an unremarkable one.
     COMPARED   the minted token is an equality operand, so an absence MATCHES. That is a wrong answer with the
                same shape as a right one, it is the measured incident exactly, and its remedy is mechanical —
                assert the field, or compare before converting. This band is the verdict's.
     KEYED      the token becomes a property key, so `o[x.f] = v` MINTS "undefined" ON the object and a later
                read of a genuinely absent field finds it. Real, and LISTED rather than accused: whether the
                absence is possible is a fact about the RECORD, and at every disposition these rows land on the
                producer does write the name. Printed because a reader chasing an "undefined" key has nowhere
                else to look.
     A VALUE    the token is printed, concatenated or summed and nothing compares or keys it. NOT REPORTED, and
                the refusal is MEASURED rather than assumed: the rows were read, and they are census counters in
                a report line, tagged-union arms that have already branched on their own tag, and an
                instrument's own finding record — a listing of the language, which is what §the SHAPE anchor
                exists to keep out. The COUNT stays, per construct, because a decided negative nobody can see is
                the concealment this file exists to report; it is also the standing evidence for the refusal,
                which a comment could not be. */
const coercedCompared = coerced.filter((c) => c.use === "an equality operand" && !c.where);
const coercedKeyed = coerced.filter((c) => c.use === "a property key");
const coercedValue = coerced.filter((c) => c.use === "a value");
if (coerced.length) {
  log(`── COERCED — ${coerced.length} read(s) of a record field standing under a conversion that turns its ` +
      `absence into a value rather than into a crash ──`);
  log(`  ${String(coercedCompared.length).padStart(5)}  COMPARED — the token is an equality operand, so an absence MATCHES. This band is the verdict's`);
  for (const c of coercedCompared)
    log(`         ${place(c)}  ${c.recv}.${c.name}  ${c.to} via ${c.how}; the record is ${c.disp}  — ${c.sec}`);
  const inEq = coerced.filter((c) => c.use === "an equality operand" && c.where);
  for (const [w, n] of [...inEq.reduce((m, c) => m.set(c.where, (m.get(c.where) || 0) + 1), new Map())])
    log(`         (and ${n} more inside ${w === "assert" ? "a should-never-happen, where the absence ABORTS" : "a console emission, where a person reads the token"})`);
  log(`  ${String(coercedKeyed.length).padStart(5)}  KEYED — the token becomes a property key (§7.1.21 ToPropertyKey). Listed, not accused`);
  for (const c of coercedKeyed.slice(0, 20)) log(`         ${place(c)}  ${c.recv}.${c.name}  <${c.disp}${c.where ? "/" + c.where : ""}>`);
  if (coercedKeyed.length > 20) log(`         … and ${coercedKeyed.length - 20} more`);
  log(`  ${String(coercedValue.length).padStart(5)}  A VALUE — nothing compares or keys the token. Refused, by construct:`);
  const byHow = new Map();
  for (const c of coercedValue) byHow.set(c.how, (byHow.get(c.how) || 0) + 1);
  for (const [h, n] of [...byHow].sort((a, b) => b[1] - a[1])) log(`         ${String(n).padStart(5)}  ${h}`);
}
if (defaulted.length) {
  log(`── DEFAULTED reads — ${defaulted.length} read(s) of a record field through a form that survives its absence ──`);
  for (const [form, n] of [...dByForm].sort((a, b) => b[1] - a[1])) log(`  ${String(n).padStart(5)}  ${form}`);
  for (const d of defaulted.slice(0, 20)) log(`    ${place(d)}  ${d.recv}.${d.name}  (${d.form})`);
  if (defaulted.length > 20) log(`    … and ${defaulted.length - 20} more`);
}

if (decidedOperand.length) {
  const byOp = new Map();
  for (const d of decidedOperand) byOp.set(d.by, (byOp.get(d.by) || 0) + 1);
  log(`── DECIDED, NOT DEFAULTED — ${decidedOperand.length} read(s) a \`||\`/\`??\` FOLLOWS without defaulting: a ` +
      `boolean-producing operator consumed the value first, so the disjunction joins booleans and substitutes ` +
      `nothing for the field. \`if (!r.f || typeof r.f !== 'object') throw\` is the assertion this gate's verdict ` +
      `asks for, and reporting it as concealment would say the reverse of what it does — decided, not passed ──`);
  log(`  by the operator that consumed it: ${[...byOp].sort((a, b) => b[1] - a[1]).map(([o, n]) => `${o}×${n}`).join(", ")}`);
  for (const d of decidedOperand.slice(0, 20))
    log(`    ${place(d)}  ${d.recv}.${d.name}  (${d.form} — consumed by \`${d.by}\` first)`);
  if (decidedOperand.length > 20) log(`    … and ${decidedOperand.length - 20} more`);
}

if (wholeDefaulted.length) {
  log(`── DEFAULTED WHOLE RECORDS — ${wholeDefaulted.length} place(s) substituting an entire emitted record shape ──`);
  for (const d of wholeDefaulted)
    log(`  ${place(d)}  ${d.left} ${d.op} { ${d.keys.slice(0, 4).join(", ")}${d.keys.length > 4 ? ", …" : ""} }   [${d.shape}]`);
}

show(`FIELD NAME THROUGH THE RAW ENTRY — ${rawKeys.length} ${C_RAW}( literal(s) carrying a \`"name":\`. ` +
     `${C_KEY} is the entry that declares a field name and the only one a reader has to look at; a name written ` +
     `around it is a producer's contract hiding in bytes nothing audits`,
     rawKeys, (r) => `${place(r)}  ${r.keys.join(" ")}   in \`${r.text}\``);

if (ambiguous.length) {
  log(`── AMBIGUOUS ANCHOR — ${ambiguous.length} receiver(s) whose record identity this cannot decide. Counted, ` +
      `never resolved either way; this is the report on itself, not a pass. GROUPED BY THE QUESTION, because a ` +
      `count that merges two questions tells a reader neither what is undecided nor what would decide it ──`);
  const aByReason = new Map();
  for (const a of ambiguous) { if (!aByReason.has(a.reason)) aByReason.set(a.reason, []); aByReason.get(a.reason).push(a); }
  for (const [reason, as] of [...aByReason].sort((x, y) => y[1].length - x[1].length)) {
    log(`  ${String(as.length).padStart(5)}  ${reason}`);
    for (const a of as.slice(0, 20)) log(`         ${place(a)}  \`${a.recv}\` ${a.why}`);
    if (as.length > 20) log(`         … and ${as.length - 20} more in this group`);
  }
}

/* PRINTED IN FULL, AND NOT A DEFECT — a decided negative nobody can see is the concealment this file reports.
   Every row is a claim about the tree made on @webref/idl's authority, so every row is somewhere a reader can
   disagree with it; the alternative is a count that shrank for reasons nobody can check. */
/* §THE-TWO-QUESTIONS-A-DECIDED-NEGATIVE-ANSWERS. These rows used to print as ONE band, and one band is one bit
   answering two questions that a case has already arrived to separate — §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS,
   performed by this gate on its own output. The questions are:

     (1) IS THE PRODUCER OUTSIDE THIS CORPUS — the scoping question, and the only one the band was built for.
     (2) IS THIS CODEBASE ENTITLED TO ASSERT ON THE VALUE — §THE-DISCRIMINATOR-IS-WHOSE-BYTES-STATE-THE-VALUE.

   Both populations answer (1) the same way, so the merged bit kept whatever (1) needed and the cost landed
   silently on (2) — the shape that rule names, with the refusal's stated reason being about a different
   question than the row raises. The old banner made it literal: "The specifier says where the producer is" is
   TRUE of a package and a `node:` builtin, and of a body read off a `Response` it is not true in any part —
   there is no specifier, there is no module, and there is no producer anybody can open. Same conclusion,
   reached for a reason that does not hold, which this file's own §WHY REFUSING IS THE WHOLE DISCIPLINE rates
   as the outcome worse than no answer.

   AND THE COST IS THE VERDICT'S REMEDY, NOT THE ROW. The verdict below instructs the reader to DCHECK the
   field. For a module's record that is right — the producer is in this process and a wrong assumption about it
   is ours to hold. For remote-stated bytes it is the FORBIDDEN action: an assert on a value a stranger's server
   stated hands a remote party an abort switch for the zone that reads it, which is a measured incident and not
   a hypothetical. Nothing in one merged band could tell a reader which of those they were standing in, so the
   split is what makes the gate able to answer the question at all. It moves NO row out of decided and changes
   NO count: both bands are decided-not-passed exactly as the one band was. */
const remoteDecided = foreignDecided.filter((f) => isRemoteStated(f.by));
const moduleDecided = foreignDecided.filter((f) => !isRemoteStated(f.by));
if (moduleDecided.length) {
  const byCallee = new Map();
  for (const f of moduleDecided) byCallee.set(f.by, (byCallee.get(f.by) || 0) + 1);
  log(`── DECIDED FOREIGN — ${moduleDecided.length} receiver(s) a module OUTSIDE this corpus produced, either ` +
      `by handing it to a callback or by returning it. The specifier says where the producer is; this gate's ` +
      `subject is the serialized seam, so they are out of it — decided, not passed ──`);
  for (const f of moduleDecided) log(`  ${place(f)}  \`${f.recv}\` ${f.why} — ` +
                                     f.names.map((n) => `\`${n}\``).join(", "));
  log(`  by producer: ` + [...byCallee].sort((a, b) => b[1] - a[1]).map(([c, n]) => `${c}×${n}`).join(", "));
}
if (remoteDecided.length) {
  log(`── DECIDED REMOTE-STATED — ${remoteDecided.length} receiver(s) whose bytes a party outside this process ` +
      `stated. There is no specifier and no producer to open: the value is INPUT, so which names it carries is ` +
      `not a contract this corpus is one side of — decided, not passed. THE VERDICT'S REMEDY DOES NOT APPLY ` +
      `HERE: a DCHECK on a value a stranger stated hands that stranger an abort switch, so the answer to a ` +
      `malformed one is a REFUSAL yielding the record's declared absence — and equally never a default that ` +
      `turns the absence into a plausible datum ──`);
  for (const f of remoteDecided) log(`  ${place(f)}  \`${f.recv}\` ${f.why} — ` +
                                     f.names.map((n) => `\`${n}\``).join(", "));
}
if (assertedDecided.length) {
  log(`── DECIDED ASSERTED — ${assertedDecided.length} receiver(s) whose record identity is still OPEN and ` +
      `every one of whose reads is a should-never-happen ASSERT. A name the producer does not write aborts at ` +
      `the read, which is §Architecture's own remedy already applied — decided on the construct, not passed ──`);
  for (const a of assertedDecided)
    log(`  ${place(a)}  \`${a.recv}\` — ${a.names.map((n) => `\`${n}\``).join(", ")}, each DCHECKed at its read`);
}
if (platformDecided.length) {
  const byIface = new Map();
  for (const p of platformDecided) byIface.set(p.iface, (byIface.get(p.iface) || 0) + 1);
  const openN = platformDecided.filter((p) => p.open).length;
  log(`── DECIDED PLATFORM — ${platformDecided.length} receiver(s) whose own declaration names a Web IDL type. ` +
      `The platform is their producer and idlgen.mjs is their auditor; this gate's subject is the serialized ` +
      `seam, so they are out of it — decided, not passed. THE CONSTRUCT DECIDES THE BAND AND THE READ SET ` +
      `DECIDES ONLY THE INTERFACE: ${openN} of these rows read a name set that fits several of the base's ` +
      `subtree equally, so WHICH object it is stays open while WHOSE it is does not, and each such row says so ──`);
  for (const p of platformDecided.slice(0, 20))
    log(`  ${place(p)}  \`${p.recv}\` is ${anIface(p.iface)}${
        p.open ? `, UNNARROWED — ${p.open} interfaces in its subtree fit these names equally`
               : p.base === p.iface ? "" : ` (${anIface(p.base)}, narrowed)`} — ` +
        `${p.names.slice(0, 6).map((n) => `\`${n}\``).join(", ")}${p.names.length > 6 ? " …" : ""}`);
  if (platformDecided.length > 20) log(`  … and ${platformDecided.length - 20} more`);
  log(`  by interface: ${[...byIface].sort((a, b) => b[1] - a[1]).map(([i, n]) => `${ifaceLabel(i)}×${n}`).join(", ")}` +
      `${openN ? ` — the interface named for an UNNARROWED row is the BASE the construct gave, not a pick` : ""}`);
}

/* PRINTED IN FULL FOR THE REASON THE TWO ABOVE ARE, and with the margin that decided each row, because this
   is the arm that REMOVES receivers from the audit and a removal nobody can see is the concealment. */
if (corpusDecided.length) {
  log(`── DECIDED CORPUS-LOCAL — ${corpusDecided.length} receiver(s) a record THIS CORPUS CONSTRUCTS explains ` +
      `better than any engine emission does. The producer is JavaScript, so the contract is not the serialized ` +
      `seam — decided, not passed. Each row would otherwise have anchored to the emission beside it on the ` +
      `handful of field names every JS corpus shares (\`name\`, \`type\`, \`url\`, \`method\`, \`source\`) ──`);
  for (const c of corpusDecided.slice(0, 20))
    log(`  ${place(c)}  \`${c.recv}\` — ${c.jsN} field(s) of ${c.jsShape} against ${c.cN} of ${c.cShape}: ` +
        `${c.names.slice(0, 6).map((n) => `\`${n}\``).join(", ")}${c.names.length > 6 ? ` … (+${c.names.length - 6})` : ""}`);
  if (corpusDecided.length > 20) log(`  … and ${corpusDecided.length - 20} more`);
}

/* THE COLLISION THE SHAPE ANCHOR USED TO SWALLOW, now that the write question is asked of the anchored record.
   Every row prints where the name IS written, because that is the one thing that tells a reader which of the
   two possible stories they are looking at without opening a file: a producer that renamed a field out from
   under a live consumer, or a receiver whose identity this scan got wrong. */
if (offRecord.length) {
  const byRecv = new Map();
  for (const o of offRecord) {
    const k = `${o.file}\u0000${o.recv}\u0000${o.shape}`;
    if (!byRecv.has(k)) byRecv.set(k, []);
    byRecv.get(k).push(o);
  }
  log(`── OFF-RECORD — ${offRecord.length} read(s) of a name the record the receiver ANCHORS TO does not emit, ` +
      `from ${byRecv.size} receiver(s). Until the write question was asked of the anchored record these were ` +
      `excused by whichever unrelated party in the corpus happened to spell the field the same way ──`);
  for (const [, os] of byRecv) {
    log(`  ${os[0].file}  \`${os[0].recv}\` anchors to ${os[0].shape}`);
    const seen = new Set();
    for (const x of os) {
      if (seen.has(x.name)) continue;
      seen.add(x.name);
      log(`      .${x.name.padEnd(20)} read at ${os.filter((y) => y.name === x.name).map((y) => y.line).join(", ")}` +
          `; not emitted there; written at ${x.writes.map(place).join(", ")}`);
    }
  }
}

/* PRINTED IN FULL AND NOT A DEFECT, beside the category it was taken out of, so the two are one screen and the
   subtraction is visible rather than a number that got smaller. A row here is a claim that the consumer put the
   name on the record itself; the assignment is named, so a reader who thinks the receiver is a different record
   can say so in one line. */
if (consumerSynth.length) {
  log(`── DECIDED CONSUMER-SYNTHESISED — ${consumerSynth.length} read(s) of a name the anchored emission does not ` +
      `carry BECAUSE THIS CONSUMER WROTE IT ONTO THE RECORD, deriving it from rows the emission does carry. The ` +
      `producer's silence is deliberate where the value is a residue of its own published parts, and an ` +
      `accusation here asks for a SECOND WRITER of a quantity whose whole worth is being derived as a check ──`);
  for (const c of consumerSynth)
    log(`  ${place(c)}  \`${c.recv}.${c.name}\`  anchors to ${c.shape}; written at ${place(c.at)} onto ` +
        `\`${c.at.recv}\`, a receiver of that same emission (reads ${c.at.on.map((n) => `\`${n}\``).join(", ")})`);
}

if (offInterface.length) {
  log(`── OFF-INTERFACE — ${offInterface.length} read(s) of a name the spec does not declare on the object the ` +
      `receiver's own declaration says it is. Web IDL is the whole member list, so an expando on a platform ` +
      `object is a name whose only producer is the line that wrote it ──`);
  for (const o of offInterface.slice(0, 20))
    log(`  ${place(o)}  \`${o.recv}\` is ${anIface(o.iface)}${o.open ? ` or one of ${o.open - 1} other interfaces in its subtree` : ""}, ` +
        `and ${o.missing.map((n) => `\`${n}\``).join(", ")} ` +
        `${o.missing.length > 1 ? "are members" : "is a member"} ${o.open ? "no candidate among them declares" : "it does not declare"}`);
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
  const bump = (f, k) => { const a = areaOf.get(f) || "?"; if (!tally.has(a)) tally.set(a, { rnw: 0, wnr: 0, def: 0, amb: 0, dom: 0, off: 0, ref: 0, rk: 0 }); tally.get(a)[k]++; };
  for (const d of outsideDomain) bump(d.file, "dom");
  for (const d of unenumerated) bump(d.file, "dom");
  for (const d of undeclaredDomain) bump(d.file, "dom");
  for (const [, e] of readNoWriter) for (const r of e.reads) bump(r.file, "rnw");
  for (const [, e] of writeNoReader) for (const w of e.writes) bump(w.file, "wnr");
  for (const d of defaulted) bump(d.file, "def");
  for (const o of offInterface) bump(o.file, "off");
  for (const a of ambiguous) bump(a.file, "amb");
  for (const r of refusals) bump(r.file, "ref");
  for (const r of rawKeys) bump(r.file, "rk");
  /* WHAT THESE ZEROES ARE A FRACTION OF, printed WITH them, because §Testing's rule is that a coverage figure
   * states its denominator in the same line or it is not a coverage figure. The producer side of this contract
   * is `cEmitted` — the field names the C ENGINE SERIALIZES — and everything downstream is keyed to it: a
   * SHAPE is one C emission's own set of names, a receiver anchors only by matching two of one shape, and
   * WRITE-WITH-NO-READER is filtered by `cEmitted.has(name)` outright. So a record this tree builds and
   * consumes entirely in JavaScript declares NOTHING here, and a zero in these columns says nothing whatever
   * about it.
   *
   * MEASURED, AND THIS LINE EXISTS BECAUSE OF IT: `savedAt` was written into the cumulative store on every
   * save and read by nothing — one occurrence in the whole tree, the write — and this gate reported 0
   * write-no-reader for that area the whole time it stood. It was found by READING. The store crosses the
   * IndexedDB boundary and never the C seam, so no construct here could see it, and the honest report of that
   * is this sentence rather than a number that reads as a clean bill. Extending the corpus to the persistence
   * boundary — an object literal a `put`/`add` is handed, with the door that reads it back as the other side —
   * is a diff of its own; until it lands, the denominator is what is stated here. */
  log("── per area ── these columns are a fraction of ONE contract: the field names the C engine SERIALIZES " +
      "(shape-anchored consumers of them, and writes of them nobody reads). A record built, stored and read " +
      "back entirely in JavaScript — the cumulative store's IndexedDB round trip — declares no name in it, so " +
      "a zero below is silent about that boundary, not clean about it");
  const w = Math.max(...[...tally.keys()].map((k) => k.length));
  for (const [a, t] of [...tally].sort((x, y) => (y[1].rnw + y[1].wnr + y[1].def) - (x[1].rnw + x[1].wnr + x[1].def)))
    log(`  ${a.padEnd(w)}  read-no-writer ${String(t.rnw).padStart(4)}   write-no-reader ${String(t.wnr).padStart(4)}` +
        `   defaulted ${String(t.def).padStart(4)}   domain ${String(t.dom).padStart(3)}` +
        `   off-iface ${String(t.off).padStart(3)}   ambiguous ${String(t.amb).padStart(3)}` +
        `   refused ${String(t.ref).padStart(4)}   raw-key ${String(t.rk).padStart(3)}`);
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
  ["reads of a record field CONVERTED into a token something compares — an absence that MATCHES", coercedCompared.length],
  ["whole emitted records DEFAULTED away by a substitute literal", wholeDefaulted.length],
  ["comparisons OUTSIDE a producer's return domain — branches on a value it cannot answer", outsideDomain.length],
  ["values a producer returns that an exhaustive construct does NOT enumerate", unenumerated.length],
  ["producers whose RETURN DOMAIN is undeclared while a consumer enumerates constants", undeclaredDomain.length],
  ["qjs_* ABI descriptions that DISAGREE — an entry, its export, its binding, its declared method", abiDefects.length],
  ["mojo reply fields a CALLER reads that the boundary does not declare", replyFieldDefects.length],
  ["reads OFF the interface a platform receiver's own declaration names, that nothing else writes either",
   offInterface.length],
  ["reads OFF the record a receiver anchors to, that an UNRELATED record in the corpus writes", offRecord.length],
  [`field names emitted through ${C_RAW} — ${C_KEY} is the only entry that may declare one`, rawKeys.length],
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
              `what stops either from being a crash, and a CONVERTED one stops it from even looking empty. ` +
              `Fix at the ROOT, and WHOSE BYTES STATE THE VALUE decides which root: where this codebase ` +
              `computed it, make the consumer DCHECK the field (extension/check.js mirrors check.h) — its ` +
              `TYPE, never its content, since a path this corpus composes is legitimately empty; where a party ` +
              `outside this process stated it, an assert is the forbidden answer and a REFUSAL yielding the ` +
              `record's declared absence is the right one. Or delete the half of the contract that has gone ` +
              `stale. The DECIDED REMOTE-STATED band above is the population the first half must not be ` +
              `applied to.`);
process.exit(1);

/* WHAT IS STILL ONE-SIDED, now that the ABI's four descriptions are read together: the CALLER'S READ of a mojo
 * reply. `(await eng.r.renderer.step()).code` names a field this gate knows the declaration of, and it reads it
 * off a receiver the normalizer refuses — so the shipped bridge, the zone the NEED_FETCH defect actually lived
 * in, still sits one construct outside every namespace here. What would close it is a receiver walk that can
 * read an AWAITED CALL as a receiver whose record identity is the called method's reply, which is a construct
 * rather than an inference: the call names the method and the method's own record names its fields. Until that
 * exists the refusal is the honest answer and its place is printed.
 */
