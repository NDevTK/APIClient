/* WHICH COLUMN A WEB IDL §3.8 INTERFACE-OBJECT PLACEMENT SITS IN, AND WHAT THAT COLUMN OWES.
 *
 *   node engine/placeaudit.mjs                 audit engine/host/browser
 *   node engine/placeaudit.mjs <path …>        audit those files/directories instead
 *   node engine/placeaudit.mjs --all           print every row rather than a sample of each bucket
 *   node engine/placeaudit.mjs --sites         print every placement site with its column, channel and names
 *   node engine/placeaudit.mjs --selftest      run the positive controls and print what each parser answered
 *
 * WHAT IT ASKS. Web IDL §3.8 "Platform objects implementing interfaces" is "To define the global property
 * references on target, given realm realm", and its step 1 is "Let interfaces be a list that contains every
 * interface that is exposed in realm" — the population is a REALM's, and the algorithm names no Document. This
 * engine has two columns to place an interface object from: core/platform.c's `declare` field, which runs a
 * component's `_init` once per agent and is where a component registers the per-realm intrinsic every realm
 * runs, and its `install` field, which runs once per DOCUMENT. A realm with no Document over it — a worker
 * global scope — reaches the second column never. So an interface whose §3.3.7 exposure set reaches any global
 * name other than `Window`, placed from the per-document column, is a name that realm is owed and does not get.
 *
 * That join is the whole of the report: which column each placement is in, crossed with what the corpus's own
 * exposure set says the placement owes. Neither half is stated here — the columns come out of platform.c's own
 * table and out of the registration every realm intrinsic makes, and the exposure sets come out of
 * browser/idl_exposure.h, which engine/idlgen.mjs generates from @webref/idl. A hand-typed list of either would
 * be the second copy that drifts, and the interface names in this file are exactly zero.
 *
 * IT IS NOT A GATE AND MUST NOT BECOME ONE. It exits 0 whatever it finds, it is not wired into engine/build.mjs
 * and it keeps no per-file ledger. Its value is that a number anyone quotes can be re-derived at a revision:
 * the figures this population produced have been quoted across briefs and into a commit message from a script
 * that lived in a scratch directory, which is the state CLAUDE.md names — "a measurement can outlive its
 * instrument, which makes the number quotable and unreproducible at the same time". It prints its own revision
 * for that reason, and it prints no expected total and no baseline: both rot, and a count of what is missing
 * shrinks as people do the work.
 *
 * THE TWO VERDICTS ARE SEPARATE, following engine/idlgen.mjs. A count of what this run FOUND and a count of
 * what this run COULD NOT READ are statements about different subjects, and summing them is the defect where
 * three states hide behind one answer. Every blind-spot row is the amount by which the findings above it are a
 * floor rather than a total.
 *
 * ITS FLOOR AND THE SPELLING IT SEARCHED, because a static sweep over source text reports a lower bound wearing
 * a total's clothes:
 *
 *   - IT COUNTS CALLS TO THE §3.8 DOORS, and it DERIVES which functions those are rather than naming them: the
 *     door is `idl_define_global_property_reference`, and any function in the door's own file that calls it is
 *     a router to it (§3.8's step 3.1.4 alias and the [Exposed]-gated install are the two). An interface object
 *     placed on a global by a bare `JS_SetPropertyStr` asks §3.8 nothing and is invisible to the door count —
 *     so the run also counts, over a BALANCED argument list and keyed on the IDENTIFIER rather than on the
 *     receiver, every `JS_SetPropertyStr` whose name argument is a key of IDL_EXPOSURE, and reports that as a
 *     blind-spot magnitude. Keying on the receiver has failed twice in this tree and cannot tell a defect from
 *     the idiom.
 *
 *   - IT READS TEXT, NOT A PARSE, so the identifier argument is resolved by three channels and no more: a
 *     string literal; an index into a table declared in the same file, whose rows it reads; and a parameter of
 *     a shared helper, resolved from the helper's callers. A site outside those three is reported UNRESOLVED
 *     with its file and line, never dropped and never counted.
 *
 *   - THE CALL GRAPH FOLLOWS CALLS AND TWO NAMED REGISTRATIONS. `realm_declare_intrinsic(F)` and
 *     core/platform.c's table are function-pointer handovers rather than calls, so they are edges this reads by
 *     name. Any OTHER function whose address is taken and which reaches a placement is reported as a site no
 *     root reaches — which is the shape a call-graph question gets WRONG, so it is a blind-spot row and never a
 *     zero.
 *
 * THE POSITIVE CONTROL IS `--selftest` AND IT IS NOT DECORATION. The derivation this replaces answered
 * UNREACHED for exactly the interesting names, because its scan skipped a one-line thunk that defines and calls
 * on the same line — a malformed question, not a finding. So the parsers here are exercised on constructed
 * input that includes that thunk, a table-driven placement, a helper resolved from its callers and a site that
 * must come back unresolved, and the selftest prints what each answered. An empty answer from a checker nobody
 * has watched classify anything has calibrated nothing.
 */

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..");
const DEFAULT_ROOT = path.join(REPO, "engine", "host", "browser");
const EXPOSURE_HEADER = path.join(REPO, "engine", "host", "browser", "idl_exposure.h");
const PLATFORM_C = path.join(REPO, "engine", "host", "browser", "core", "platform.c");

/* The one door, by name, because it is the only fact in this file that cannot be derived from something else —
   every other door is found by asking which functions in its own file route to it. */
const DOOR_SEED = "idl_define_global_property_reference";
/* The registration a component makes for the per-realm column (core/realm.h) — a function POINTER handover, so
   it is an edge this reads by name rather than a call the graph would find on its own. */
const REALM_REGISTRAR = "realm_declare_intrinsic";
/* The table core/platform.c states its two columns in. Its rows are `{ name, declare, install, release }`. */
const PLATFORM_TABLE = "PLATFORM";

/* ---- source scanning ------------------------------------------------------------------------------------ */

/* Comments out, string and character literals INTACT, offsets and line numbers preserved — a comment becomes
   the same number of spaces, so every offset into the result is an offset into the original. The identifier a
   placement names is a string literal, so a scanner that discarded literals could not answer anything. */
export function blankComments(src) {
  /* SPLIT BY CODE UNIT, NOT BY CODE POINT. `Array.from` iterates CODE POINTS, so one astral character makes the
     array shorter than the string and every offset after it is wrong by the number of surrogate pairs seen so
     far — while `src[i]`, `src.indexOf` and every regex index are code-UNIT based. Three emoji in one comment
     of one component were seen to coincide with that file contributing ZERO placement sites to a run reporting
     no blind spot at all — an empty answer that is a fact about the parser, which is the shape this
     instrument's own controls exist to catch.
     THE CAUSAL STORY ABOVE IS NOT ESTABLISHED, AND SAYING SO IS THE POINT. Reverting this line to `Array.from`
     does NOT make `after_astral` fail, and it still does not with the fixture's astral run widened six-fold —
     checked on a copy, both arms, before this note was written. The reason is visible here: the astral
     character sits INSIDE the comment being blanked, so it is overwritten in either spelling, and a uniform
     shift of a string the parser thereafter reads on its own terms is not observable. So the emoji may have
     been a coincidence and the zero may have had another cause.
     THE LINE STAYS AS IT IS ON AN A-PRIORI ARGUMENT, WHICH IS THE HALF THAT DOES HOLD: `src[i]`, `src.indexOf`
     and every regex index above are code-UNIT based, so the array they write through must be too — that is
     true whether or not any file has ever tripped it. What is retired is the claim to have MEASURED the
     failure. CLAUDE.md rates a correct conclusion reached by a wrong argument as worse than an open question,
     because a reader who checks the argument finds it false and discards the conclusion with it.
     WHAT WOULD CLOSE THIS: a control whose astral character sits OUTSIDE any comment or string, where the two
     spellings genuinely disagree about what the blanked output contains. `after_astral` does not discriminate
     and is a coverage row rather than a control until then. HOW ITS ABSENCE SHOWS: this line can be reverted
     and every control still passes, which is what was observed. */
  const out = src.split("");
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "*") {
      const end = src.indexOf("*/", i + 2);
      const stop = end < 0 ? n : end + 2;
      for (let k = i; k < stop; k++) if (out[k] !== "\n") out[k] = " ";
      i = stop;
    } else if (c === "/" && src[i + 1] === "/") {
      let k = i;
      while (k < n && src[k] !== "\n") { out[k] = " "; k++; }
      i = k;
    } else if (c === '"' || c === "'") {
      const quote = c;
      let k = i + 1;
      while (k < n) {
        if (src[k] === "\\") { k += 2; continue; }
        if (src[k] === quote) { k++; break; }
        k++;
      }
      i = k;
    } else {
      i++;
    }
  }
  return out.join("");
}

function lineIndex(src) {
  const starts = [0];
  for (let i = 0; i < src.length; i++) if (src[i] === "\n") starts.push(i + 1);
  return starts;
}

function lineOf(starts, off) {
  let lo = 0, hi = starts.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (starts[mid] <= off) lo = mid; else hi = mid - 1;
  }
  return lo + 1;
}

const IDENT_CHAR = /[A-Za-z0-9_]/;

/* Skip backwards over whitespace in `code` from `i` (exclusive), returning the index of the last non-space. */
function backNonSpace(code, i) {
  let k = i - 1;
  while (k >= 0 && /\s/.test(code[k])) k--;
  return k;
}

/* The index of the bracket matching the one at `open`, or -1. Strings are already skipped by the caller having
   handed us comment-blanked code; quotes inside are handled so a literal `"("` cannot unbalance anything. */
function matchBracket(code, open) {
  const pairs = { "(": ")", "[": "]", "{": "}" };
  const close = pairs[code[open]];
  if (!close) return -1;
  let depth = 0;
  for (let i = open; i < code.length; i++) {
    const c = code[i];
    if (c === '"' || c === "'") {
      const q = c;
      i++;
      while (i < code.length) {
        if (code[i] === "\\") { i += 2; continue; }
        if (code[i] === q) break;
        i++;
      }
      continue;
    }
    if (c === code[open]) depth++;
    else if (c === close) { depth--; if (depth === 0) return i; }
  }
  return -1;
}

/* Top-level commas of the text BETWEEN a pair of brackets. */
export function splitTop(text) {
  const out = [];
  let depth = 0, start = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"' || c === "'") {
      const q = c;
      i++;
      while (i < text.length) {
        if (text[i] === "\\") { i += 2; continue; }
        if (text[i] === q) break;
        i++;
      }
      continue;
    }
    if (c === "(" || c === "[" || c === "{") depth++;
    else if (c === ")" || c === "]" || c === "}") depth--;
    else if (c === "," && depth === 0) { out.push(text.slice(start, i)); start = i + 1; }
  }
  if (text.slice(start).trim() !== "" || out.length) out.push(text.slice(start));
  return out.map((s) => s.trim());
}

/* EVERY TOP-LEVEL FUNCTION DEFINITION, by walking brace depth rather than by matching a line. The derivation
   this file replaces read function bodies with an line-oriented scan and answered UNREACHED for every component
   whose thunk is written `static void d_url(JSContext *c, ...) { ...; url_init(c); }` — one line, definition and
   call together. Depth-walking cannot make that mistake, and --selftest holds the thunk that proves it. */
export function parseFunctions(code) {
  const fns = [];
  let depth = 0;
  for (let i = 0; i < code.length; i++) {
    const c = code[i];
    if (c === '"' || c === "'") {
      const q = c;
      i++;
      while (i < code.length) {
        if (code[i] === "\\") { i += 2; continue; }
        if (code[i] === q) break;
        i++;
      }
      continue;
    }
    if (c === "}") { depth--; continue; }
    if (c !== "{") continue;
    if (depth++ > 0) continue;                       /* a nested brace, not a definition */
    const end = matchBracket(code, i);
    if (end < 0) break;
    /* A body's `{` is preceded by the `)` of a parameter list. `= {` (an initializer) and `struct X {` are not. */
    const p = backNonSpace(code, i);
    if (p < 0 || code[p] !== ")") { depth--; i = end; continue; }
    const openParen = (() => {
      let d = 0;
      for (let k = p; k >= 0; k--) {
        if (code[k] === ")") d++;
        else if (code[k] === "(") { d--; if (d === 0) return k; }
      }
      return -1;
    })();
    if (openParen < 0) { depth--; i = end; continue; }
    let e = backNonSpace(code, openParen);
    if (e < 0 || !IDENT_CHAR.test(code[e])) { depth--; i = end; continue; }
    let s = e;
    while (s >= 0 && IDENT_CHAR.test(code[s])) s--;
    const name = code.slice(s + 1, e + 1);
    if (/^\d/.test(name)) { depth--; i = end; continue; }
    fns.push({
      name,
      params: code.slice(openParen + 1, p),
      bodyStart: i,
      bodyEnd: end,
      defOffset: s + 1,
    });
    depth--;
    i = end;
  }
  return fns;
}

/* Every `IDENT(` in a range, with the offset of the identifier and of its `(`. */
function callsIn(code, from, to) {
  const out = [];
  const re = /([A-Za-z_][A-Za-z0-9_]*)\s*\(/g;
  re.lastIndex = from;
  let m;
  while ((m = re.exec(code)) !== null) {
    if (m.index >= to) break;
    const before = backNonSpace(code, m.index);
    if (before >= 0 && IDENT_CHAR.test(code[before])) continue;   /* part of a longer name */
    if (before >= 0 && (code[before] === "." || code[before] === ">")) continue;  /* a member call */
    out.push({ name: m[1], at: m.index, paren: m.index + m[0].length - 1 });
    re.lastIndex = m.index + m[0].length - 1;
  }
  return out;
}

function argsOfCall(code, parenOffset) {
  const close = matchBracket(code, parenOffset);
  if (close < 0) return null;
  return splitTop(code.slice(parenOffset + 1, close));
}

/* ---- the corpus ----------------------------------------------------------------------------------------- */

function walk(p, acc) {
  const st = fs.statSync(p);
  if (st.isDirectory()) {
    for (const e of fs.readdirSync(p).sort()) walk(path.join(p, e), acc);
  } else if (/\.[ch]$/.test(p)) {
    acc.push(p);
  }
  return acc;
}

function loadCorpus(roots) {
  const files = [];
  for (const r of roots) walk(r, files);
  return files.map((file) => {
    const raw = fs.readFileSync(file, "utf8");
    const code = blankComments(raw);
    return { file, rel: path.relative(REPO, file), code, lines: lineIndex(code), fns: parseFunctions(code) };
  });
}

/* ---- the doors, derived --------------------------------------------------------------------------------- */

/* §3.8's doors are the seed and every function IN THE SEED'S OWN FILE that calls one. Nothing else can be a
   door: idl_args.h states the seed is "the ONE door an interface's name reaches the global through", so a
   router elsewhere in the tree would be a component placing through a component, which this reports as an
   ordinary site rather than as a door. */
function deriveDoors(corpus) {
  const seedUnit = corpus.find((u) => u.fns.some((f) => f.name === DOOR_SEED));
  if (!seedUnit) throw new Error(`no definition of ${DOOR_SEED} in the corpus — this audit has no subject`);
  const doors = new Map([[DOOR_SEED, seedUnit]]);
  let grew = true;
  while (grew) {
    grew = false;
    for (const f of seedUnit.fns) {
      if (doors.has(f.name)) continue;
      const calls = callsIn(seedUnit.code, f.bodyStart, f.bodyEnd);
      if (calls.some((c) => doors.has(c.name))) { doors.set(f.name, seedUnit); grew = true; }
    }
  }
  /* WHICH ARGUMENT IS THE IDENTIFIER is read off each door's own parameter list — the single `const char *`
     parameter — rather than stated here, so a door whose signature changes cannot silently be read at the
     wrong position. A door with none, or with more than one, is refused rather than guessed at. */
  const out = new Map();
  for (const [name] of doors) {
    const def = seedUnit.fns.find((f) => f.name === name);
    const params = splitTop(def.params);
    const idIdx = [];
    params.forEach((p, i) => { if (/\bconst\s+char\s*\*/.test(p) && !/\*\s*\*/.test(p)) idIdx.push(i); });
    if (idIdx.length !== 1) {
      out.set(name, { idArg: null, why: `${idIdx.length} \`const char *\` parameters, so no unique identifier argument` });
    } else {
      out.set(name, { idArg: idIdx[0], why: null });
    }
  }
  return { doors: out, doorFile: seedUnit };
}

/* ---- the two columns, derived --------------------------------------------------------------------------- */

function platformColumns(corpus) {
  const unit = corpus.find((u) => u.file === PLATFORM_C);
  if (!unit) throw new Error(`${path.relative(REPO, PLATFORM_C)} is not in the audited paths — the columns are stated there`);
  const m = new RegExp(`${PLATFORM_TABLE}\\s*\\[\\s*\\]\\s*=\\s*\\{`).exec(unit.code);
  if (!m) throw new Error(`no \`${PLATFORM_TABLE}[] = {\` in ${unit.rel} — the column list has moved`);
  const open = unit.code.indexOf("{", m.index + m[0].length - 1);
  const close = matchBracket(unit.code, open);
  const rows = splitTop(unit.code.slice(open + 1, close)).filter((r) => r.trim().startsWith("{"));
  const declare = new Set(), install = new Set();
  for (const row of rows) {
    const o = row.indexOf("{");
    const c = matchBracket(row, o);
    const f = splitTop(row.slice(o + 1, c));
    const take = (v) => (v && /^[A-Za-z_][A-Za-z0-9_]*$/.test(v) && v !== "NULL" ? v : null);
    const d = take(f[1]), i = take(f[2]);
    if (d) declare.add(d);
    if (i) install.add(i);
  }
  return { declare, install, rowCount: rows.length, unit };
}

function realmIntrinsics(corpus) {
  const out = new Set();
  for (const u of corpus) {
    for (const f of u.fns) {
      if (f.name === REALM_REGISTRAR) continue;      /* the registrar's own definition */
      for (const c of callsIn(u.code, f.bodyStart, f.bodyEnd)) {
        if (c.name !== REALM_REGISTRAR) continue;
        const a = argsOfCall(u.code, c.paren);
        if (a && a.length === 1 && /^[A-Za-z_][A-Za-z0-9_]*$/.test(a[0])) out.add(a[0]);
      }
    }
  }
  return out;
}

/* Call edges only. A function POINTER handed over is a registration, not a call, so the realm column is a
   separate root set rather than something reachable from the agent one. */
function callGraph(corpus) {
  const defs = new Map();
  for (const u of corpus) for (const f of u.fns) if (!defs.has(f.name)) defs.set(f.name, { u, f });
  const edges = new Map();
  for (const u of corpus) {
    for (const f of u.fns) {
      const set = edges.get(f.name) || new Set();
      for (const c of callsIn(u.code, f.bodyStart, f.bodyEnd)) if (defs.has(c.name)) set.add(c.name);
      edges.set(f.name, set);
    }
  }
  return { defs, edges };
}

function reachable(edges, roots) {
  const seen = new Set();
  const q = [...roots];
  while (q.length) {
    const n = q.pop();
    if (seen.has(n)) continue;
    seen.add(n);
    for (const c of edges.get(n) || []) if (!seen.has(c)) q.push(c);
  }
  return seen;
}

/* ---- resolving the identifier a site names -------------------------------------------------------------- */

const STRING_LIT = /^"((?:[^"\\]|\\.)*)"$/;

function literal(expr) {
  const m = STRING_LIT.exec(expr.trim());
  return m ? m[1] : null;
}

/* An array declared in this unit: `... NAME[DIM] = { ... };`. Returns the initializer text and, where the
   declaration is a struct, the ordered member names so a `.field` can be indexed. */
function findTable(unit, name) {
  const re = new RegExp(`\\b${name}\\s*\\[[^\\]]*\\]\\s*=\\s*\\{`, "g");
  const m = re.exec(unit.code);
  if (!m) return null;
  const open = unit.code.indexOf("{", m.index + m[0].length - 1);
  const close = matchBracket(unit.code, open);
  if (close < 0) return null;
  /* Member names, if the element type is a struct written inline or named just above. */
  let members = null;
  const before = unit.code.slice(0, m.index);
  const braceEnd = before.lastIndexOf("}");
  if (braceEnd >= 0 && /^\s*$/.test(before.slice(braceEnd + 1))) {
    let d = 0, s = -1;
    for (let k = braceEnd; k >= 0; k--) {
      if (unit.code[k] === "}") d++;
      else if (unit.code[k] === "{") { d--; if (d === 0) { s = k; break; } }
    }
    if (s >= 0 && /\bstruct\b[^{;]*$/.test(unit.code.slice(Math.max(0, s - 200), s))) {
      members = unit.code
        .slice(s + 1, braceEnd)
        .split(";")
        .map((decl) => {
          const mm = /([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*$/.exec(decl.trim());
          return mm ? mm[1] : null;
        })
        .filter(Boolean);
    }
  }
  return { init: unit.code.slice(open + 1, close), members };
}


/* A TABLE WHOSE ROWS ARE AN X-MACRO EXPANSION. `HTML_IFACE[]`'s initializer is three lines — a `#define X(...)`
   row template, one invocation of a generated list macro, and an `#undef` — so the ninety-odd interface names
   it places are in a DIFFERENT file, in a form no reading of the initializer can see. The list is generated
   (engine/elemgen.mjs writes it from the HTML Standard), so the names are a derived artifact exactly like the
   exposure sets, and reading them is reading the thing the engine obeys. This is the fourth channel and the
   last one: which macro PARAMETER lands in the field being indexed is read off the row template, so a template
   whose fields are reordered cannot silently be read at the wrong position. */
function expandXMacroTable(tab, field, corpus) {
  const rowTpl = /#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*\{([^}]*)\}/.exec(tab.init);
  if (!rowTpl) return null;
  const [, macroName, paramText, rowBody] = rowTpl;
  const invoke = new RegExp(`\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\(\\s*${macroName}\\s*\\)`).exec(tab.init);
  if (!invoke) return { unresolved: `\`${macroName}\` is defined as a row template and this run found no list macro invoking it` };
  const listName = invoke[1];
  const params = paramText.split(",").map((x) => x.trim());
  const fields = splitTop(rowBody);
  let fieldIdx = -1;
  if (field) {
    if (!tab.members) return { unresolved: `an X-macro table is indexed by \`.${field}\` and this run could not read the element struct's members` };
    fieldIdx = tab.members.indexOf(field);
    if (fieldIdx < 0) return { unresolved: `an X-macro table has no member \`${field}\` this run could see` };
  } else {
    fieldIdx = 0;
  }
  const paramIdx = params.indexOf((fields[fieldIdx] || "").trim());
  if (paramIdx < 0) return { unresolved: `the X-macro row template does not put a parameter of \`${macroName}\` in field ${fieldIdx}` };

  let listUnit = null, listDef = null;
  for (const u of corpus) {
    const d = new RegExp(`#\\s*define\\s+${listName}\\s*\\(`).exec(u.code);
    if (!d) continue;
    /* A multi-line macro runs to the first line not ending in a backslash. */
    let end = d.index;
    while (end < u.code.length) {
      const nl = u.code.indexOf("\n", end);
      if (nl < 0) { end = u.code.length; break; }
      const line = u.code.slice(end, nl);
      end = nl + 1;
      if (!/\\\s*$/.test(line)) break;
    }
    listUnit = u;
    listDef = u.code.slice(d.index, end);
    break;
  }
  if (!listDef) return { unresolved: `\`${listName}\` is invoked as an X-macro list and this run found no \`#define ${listName}(\` in the audited paths` };

  const names = [];
  const callRe = /\b([A-Za-z_][A-Za-z0-9_]*)\s*\(/g;
  let m;
  while ((m = callRe.exec(listDef)) !== null) {
    if (m[1] !== params[0] && m[1] !== "X") { /* the list's own formal parameter, whatever it is called */ }
    const open = m.index + m[0].length - 1;
    const close = matchBracket(listDef, open);
    if (close < 0) continue;
    const a = splitTop(listDef.slice(open + 1, close));
    if (a.length <= paramIdx) continue;
    const l = literal(a[paramIdx]);
    if (l !== null) names.push(l);
    callRe.lastIndex = close;
  }
  if (!names.length) return { unresolved: `\`${listName}\` expanded to no string literal in parameter ${paramIdx}` };
  return { names: [...new Set(names)], channel: `x-macro:${listName}` };
}

/* Four channels and no more. Each returns {names, channel} or {unresolved: why}. */
function resolveIdentifier(unit, fn, expr, corpus, seen) {
  seen = seen || new Set();
  const lit = literal(expr);
  if (lit !== null) return { names: [lit], channel: "literal" };

  /* TABLE[i] or TABLE[i].field — the rows of a table declared in this same unit. */
  const t = /^([A-Za-z_][A-Za-z0-9_]*)\s*\[[^\]]*\]\s*(?:\.\s*([A-Za-z_][A-Za-z0-9_]*))?$/.exec(expr.trim());
  if (t) {
    const tab = findTable(unit, t[1]);
    if (!tab) return { unresolved: `\`${t[1]}\` is indexed but no \`${t[1]}[...] = {\` declaration is in this file` };
    const xm = expandXMacroTable(tab, t[2], corpus);
    if (xm) return xm;
    const rows = splitTop(tab.init).filter((r) => r.trim() !== "");
    const names = [];
    for (const row of rows) {
      if (!t[2]) {
        const l = literal(row);
        if (l !== null) names.push(l);
        continue;
      }
      if (!tab.members) return { unresolved: `\`${t[1]}\` rows are indexed by \`.${t[2]}\` and this run could not read the element struct's members` };
      const idx = tab.members.indexOf(t[2]);
      if (idx < 0) return { unresolved: `\`${t[1]}\` has no member \`${t[2]}\` this run could see` };
      const o = row.indexOf("{");
      if (o < 0) return { unresolved: `a row of \`${t[1]}\` is not a braced initializer` };
      const f = splitTop(row.slice(o + 1, matchBracket(row, o)));
      const l = literal(f[idx] || "");
      if (l !== null) names.push(l);
    }
    if (!names.length) return { unresolved: `\`${expr.trim()}\` resolved to a table with no string literals in the indexed position` };
    return { names, channel: "table" };
  }

  /* A parameter of the enclosing function — the shared-helper shape. Resolve from the helper's callers. */
  const bare = /^[A-Za-z_][A-Za-z0-9_]*$/.exec(expr.trim());
  if (bare) {
    const params = splitTop(fn.params);
    const pIdx = params.findIndex((p) => new RegExp(`\\b${expr.trim()}\\s*$`).test(p.replace(/\[[^\]]*\]\s*$/, "")));
    if (pIdx < 0) return { unresolved: `\`${expr.trim()}\` is not a parameter of \`${fn.name}\` and is not a literal` };
    /* A HELPER'S CALLER MAY BE A HELPER. `node_install_interface` forwards its own `name` parameter to
       `node_install_interface_ctor`, and one of ITS callers indexes a table declared in a different file — so a
       resolution that stopped at one level answered UNRESOLVED for a site whose identifiers are all literals two
       hops away. Each hop resolves in the CALLER's unit, because that is where its table is declared. A cycle
       is refused rather than followed. */
    if (seen.has(fn.name)) return { unresolved: `\`${fn.name}\` forwards its identifier in a cycle this run refuses to follow` };
    seen.add(fn.name);
    const names = [];
    let sawCaller = false;
    for (const u of corpus) {
      for (const g of u.fns) {
        if (g.name === fn.name) continue;
        for (const c of callsIn(u.code, g.bodyStart, g.bodyEnd)) {
          if (c.name !== fn.name) continue;
          sawCaller = true;
          const a = argsOfCall(u.code, c.paren);
          if (!a || a[pIdx] === undefined) return { unresolved: `\`${fn.name}\` is called from ${u.rel}:${lineOf(u.lines, c.at)} with too few arguments for this run to read position ${pIdx}` };
          const r = resolveIdentifier(u, g, a[pIdx], corpus, seen);
          if (r.unresolved) return { unresolved: `via \`${fn.name}\` at ${u.rel}:${lineOf(u.lines, c.at)}: ${r.unresolved}` };
          names.push(...r.names);
        }
      }
    }
    if (!sawCaller) return { unresolved: `\`${fn.name}\` takes its identifier as a parameter and this run found no caller of it` };
    if (!names.length) return { unresolved: `\`${fn.name}\`'s callers named no literal identifier` };
    return { names: [...new Set(names)], channel: "helper-parameter" };
  }
  return { unresolved: `\`${expr.trim()}\` is not a literal, a table index, or a parameter of \`${fn.name}\`` };
}

/* ---- §3.3.7 exposure, out of the generated header ------------------------------------------------------- */

export function parseExposure(headerText) {
  const code = blankComments(headerText);
  const globals = new Map();
  const enumRe = /IDL_GLOBAL_([A-Z]+)\s*=\s*1u\s*<<\s*(\d+)/g;
  let m;
  while ((m = enumRe.exec(code)) !== null) globals.set(`IDL_GLOBAL_${m[1]}`, { name: m[1], bit: 1 << Number(m[2]) });
  if (!globals.size) throw new Error("no IDL_GLOBAL_* bits in the exposure header — its shape has changed");
  const rowsRe = /IDL_EXPOSURE\s*\[\s*\]\s*=\s*\{/.exec(code);
  if (!rowsRe) throw new Error("no IDL_EXPOSURE[] table in the exposure header — its shape has changed");
  const open = code.indexOf("{", rowsRe.index + rowsRe[0].length - 1);
  const close = matchBracket(code, open);
  const table = new Map();
  for (const row of splitTop(code.slice(open + 1, close))) {
    const o = row.indexOf("{");
    if (o < 0) continue;
    const f = splitTop(row.slice(o + 1, matchBracket(row, o)));
    const name = literal(f[0] || "");
    if (name === null) continue;
    const expr = (f[1] || "").trim();
    if (/\bIDL_EXPOSED_STAR\b/.test(expr)) { table.set(name, { star: true, bits: 0, names: [] }); continue; }
    const bits = [];
    for (const tok of expr.split("|").map((s) => s.trim())) {
      const g = globals.get(tok);
      if (!g) { bits.length = 0; bits.push(null); break; }
      bits.push(g);
    }
    if (bits.length === 1 && bits[0] === null) { table.set(name, { star: false, bits: null, names: null, raw: expr }); continue; }
    table.set(name, { star: false, bits: bits.reduce((a, g) => a | g.bit, 0), names: bits.map((g) => g.name) });
  }
  if (!table.size) throw new Error("the exposure table parsed to no rows — its shape has changed");
  return { globals, table };
}

/* ---- report --------------------------------------------------------------------------------------------- */

function revision() {
  try {
    const sha = execFileSync("git", ["-C", REPO, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
    /* NOT `.trim()` on the whole output: that eats the leading status column of the FIRST line only, so one
       path came out with its first character missing while every other one was right — a mangling that reads
       as a corrupt path rather than as an off-by-one, because it is invisible on every line but one. */
    const dirty = execFileSync("git", ["-C", REPO, "status", "--porcelain"], { encoding: "utf8" });
    const lines = dirty.split("\n").filter((l) => l.length > 3);
    return { sha, dirty: lines.map((l) => l.slice(3).trim()) };
  } catch {
    return { sha: "unknown", dirty: null };
  }
}

function bar(title) {
  return `\n${title}\n${"=".repeat(title.length)}`;
}

function run(argv) {
  const flags = new Set(argv.filter((a) => a.startsWith("--")));
  const paths = argv.filter((a) => !a.startsWith("--"));
  const roots = paths.length ? paths.map((p) => path.resolve(p)) : [DEFAULT_ROOT];
  const all = flags.has("--all");

  const corpus = loadCorpus(roots);
  const { doors, doorFile } = deriveDoors(corpus);
  const cols = platformColumns(corpus);
  const realmRoots = realmIntrinsics(corpus);
  const { defs, edges } = callGraph(corpus);

  const inRealm = reachable(edges, realmRoots);
  const inDoc = reachable(edges, cols.install);
  const inAgent = reachable(edges, cols.declare);

  const { table: exposure, globals } = parseExposure(fs.readFileSync(EXPOSURE_HEADER, "utf8"));
  const WINDOW = globals.get("IDL_GLOBAL_WINDOW");
  if (!WINDOW) throw new Error("the exposure header declares no IDL_GLOBAL_WINDOW bit");

  /* Every call to a door, from a function that is not itself a door. */
  const sites = [];
  for (const u of corpus) {
    for (const f of u.fns) {
      if (doors.has(f.name)) continue;
      for (const c of callsIn(u.code, f.bodyStart, f.bodyEnd)) {
        const door = doors.get(c.name);
        if (!door) continue;
        const line = lineOf(u.lines, c.at);
        if (door.idArg === null) {
          sites.push({ u, f, line, door: c.name, unresolved: door.why });
          continue;
        }
        const args = argsOfCall(u.code, c.paren);
        if (!args || args[door.idArg] === undefined) {
          sites.push({ u, f, line, door: c.name, unresolved: "this run could not split the call's argument list" });
          continue;
        }
        const r = resolveIdentifier(u, f, args[door.idArg], corpus);
        sites.push({ u, f, line, door: c.name, expr: args[door.idArg].trim(), ...r });
      }
    }
  }

  /* The column each site is in. */
  for (const s of sites) {
    const r = inRealm.has(s.f.name), d = inDoc.has(s.f.name), a = inAgent.has(s.f.name);
    s.col = r && d ? "both" : r ? "realm" : d ? "document" : a ? "agent-only" : "no-root";
  }

  const rev = revision();
  const L = [];
  L.push(bar("WEB IDL §3.8 INTERFACE-OBJECT PLACEMENT — WHICH COLUMN, AND WHAT THAT COLUMN OWES"));
  L.push(`revision            ${rev.sha}${rev.dirty === null ? " (not a git checkout)" : rev.dirty.length ? `  DIRTY: ${rev.dirty.length} path(s) differ from it` : ""}`);
  if (rev.dirty && rev.dirty.length) for (const p of rev.dirty.slice(0, 12)) L.push(`                      ${p}`);
  L.push(`audited             ${roots.map((r) => path.relative(REPO, r)).join(", ")} — ${corpus.length} translation unit(s), ${defs.size} function definition(s)`);
  L.push(`doors derived       ${[...doors.keys()].join(", ")}`);
  L.push(`                    (seeded on ${DOOR_SEED} in ${doorFile.rel}; a door is that function or a function in its own file that routes to one)`);
  L.push(`identifier argument ${[...doors].map(([n, d]) => `${n}=${d.idArg === null ? "REFUSED" : `arg ${d.idArg}`}`).join(", ")}  (read off each door's own \`const char *\` parameter)`);
  L.push(`roots derived       realm ${realmRoots.size} intrinsic(s) registered with ${REALM_REGISTRAR}; document ${cols.install.size} and agent ${cols.declare.size} field(s) of ${PLATFORM_TABLE}[] in ${cols.unit.rel} (${cols.rowCount} row(s))`);

  const byCol = (c) => sites.filter((s) => s.col === c);
  L.push(bar("FINDINGS — what this run read"));
  L.push(`${sites.length} §3.8 placement site(s), of ${sites.length} call(s) to a derived door from outside the doors' own file:`);
  for (const [name] of doors) {
    const n = sites.filter((s) => s.door === name).length;
    L.push(`  by door       ${String(n).padStart(4)} of ${sites.length} site(s) call ${name}`);
  }
  for (const c of ["realm", "document", "both", "agent-only", "no-root"]) {
    const n = byCol(c).length;
    if (n) L.push(`  by column     ${String(n).padStart(4)} of ${sites.length} site(s) are reached from the ${c} root set`);
  }

  if (flags.has("--sites")) {
    L.push("");
    L.push(`every site, in file order — ${sites.length} of ${sites.length}:`);
    for (const s of sites) {
      L.push(`  ${s.col.padEnd(10)} ${s.u.rel}:${s.line}  ${s.door}  ${s.unresolved ? `UNRESOLVED (${s.unresolved})` : `${s.names.length} name(s) [${s.channel}]: ${s.names.join(", ")}`}`);
    }
  }

  /* THE IDENTIFIERS ARE A DIFFERENT POPULATION FROM THE SITES AND STATE THEIR OWN DENOMINATOR. One site inside
     a shared helper places eighty-six names and one literal site places one, so a partition of SITES by
     exposure and a partition of NAMES by exposure are two different numbers and neither substitutes for the
     other. Both are printed, each against what it is a fraction of. */
  const placements = [];
  const unresolved = [], noRow = [];
  for (const s of sites) {
    if (s.unresolved) { unresolved.push(s); continue; }
    for (const n of s.names) {
      const row = exposure.get(n);
      if (!row || row.bits === null) { noRow.push({ s, n }); continue; }
      placements.push({ s, n, row });
    }
  }
  const bucketOf = (row) => (row.star ? "star" : (row.bits & ~WINDOW.bit) === 0 ? "window-only" : "beyond-window");

  const totalNames = sites.reduce((a, s) => a + (s.names ? s.names.length : 0), 0);
  L.push("");
  L.push(`${totalNames} identifier placement(s) across those ${sites.length} site(s) — ${new Set(sites.flatMap((s) => s.names || [])).size} distinct identifier(s); a site over a table or inside a shared helper places many, which is why this is not the site count`);
  L.push(`${placements.length} of ${totalNames} identifier placement(s) had a §3.3.7 row in ${path.relative(REPO, EXPOSURE_HEADER)} and could be judged`);

  for (const col of ["document", "realm", "both", "agent-only", "no-root"]) {
    const colSites = byCol(col);
    if (!colSites.length) continue;
    const colPl = placements.filter((x) => x.s.col === col);
    const b = { star: [], "window-only": [], "beyond-window": [] };
    for (const x of colPl) b[bucketOf(x.row)].push(x);
    L.push("");
    L.push(`the per-${col.toUpperCase()} column — ${colPl.length} judged identifier placement(s) across ${colSites.length} site(s):`);
    L.push(`  Window-only     ${String(b["window-only"].length).padStart(4)} of ${colPl.length} judged in this column — the exposure set names no global but Window`);
    L.push(`  [Exposed=*]     ${String(b.star.length).padStart(4)} of ${colPl.length} judged in this column — §3.3.7 step 1 returns before it looks at the realm, so EVERY realm is owed the name`);
    L.push(`  beyond Window   ${String(b["beyond-window"].length).padStart(4)} of ${colPl.length} judged in this column — the exposure set names a global other than Window`);
    const byGlobal = new Map();
    for (const x of b["beyond-window"]) for (const g of x.row.names) if (g !== "WINDOW") byGlobal.set(g, (byGlobal.get(g) || 0) + 1);
    for (const [g, n] of [...byGlobal].sort((a, c) => c[1] - a[1])) {
      L.push(`      ${g.padEnd(20)} ${String(n).padStart(4)} of ${b["beyond-window"].length} beyond-Window placement(s) in this column name this global`);
    }
    if (col !== "document") continue;
    /* THE DOCUMENT COLUMN IS THE ONE WHERE THE PARTITION IS A CLAIM ABOUT COVERAGE. A realm with no Document
       over it runs no install field of platform.c's table, so a name owed to such a realm and placed from here
       is a name that realm does not get. Listed by SITE, because a site is what a diff moves. */
    const owedElsewhere = [...b.star, ...b["beyond-window"]];
    const bySite = new Map();
    for (const x of owedElsewhere) {
      const k = `${x.s.u.rel}:${x.s.line}`;
      if (!bySite.has(k)) bySite.set(k, { s: x.s, names: [] });
      bySite.get(k).names.push(x.n);
    }
    const rows = [...bySite.values()];
    L.push("");
    L.push(`  ${owedElsewhere.length} of ${colPl.length} judged placement(s) in this column are owed to a realm that reaches no install field, across ${rows.length} of ${colSites.length} site(s)${all || rows.length <= 12 ? "" : ` (first 12; --all for every one)`}:`);
    for (const r of (all ? rows : rows.slice(0, 12))) {
      L.push(`    ${r.s.u.rel}:${r.s.line}  [${r.s.channel}, via ${r.s.door}]  ${r.names.length <= 6 ? r.names.join(", ") : `${r.names.slice(0, 6).join(", ")} … ${r.names.length} name(s)`}`);
    }
  }

  /* ---- the other ledger --------------------------------------------------------------------------------- */

  L.push(bar("BLIND SPOTS — what this run could not read (each row is the amount by which the findings above are a floor)"));
  L.push("Every channel is printed at zero as well, with what it is a count OF — a row that appears only on the bad day is a row nobody learns to look for, and a channel omitted when it finds nothing reads as a channel that does not exist.");
  const blind = [];

  blind.push({
    n: unresolved.length,
    of: `${sites.length} placement site(s)`,
    what: "the identifier argument was not resolvable by any of this run's four channels (a string literal, a table declared in the same file, a parameter resolved from the callers, an X-macro list)",
    rows: unresolved.map((s) => `${s.u.rel}:${s.line}  \`${s.expr ?? "?"}\` in ${s.f.name}() — ${s.unresolved}`),
  });
  blind.push({
    n: noRow.length,
    of: `${totalNames} identifier placement(s)`,
    what: `the identifier resolved and ${path.relative(REPO, EXPOSURE_HEADER)} states no §3.3.7 exposure set for it, so no column can be judged for it (that header's own rule is that a name with no row is EXPOSED, which is a statement about the property and not about the column)`,
    rows: noRow.map((x) => `${x.s.u.rel}:${x.s.line}  ${x.n}`),
  });
  const noRoot = byCol("no-root").concat(byCol("agent-only"));
  blind.push({
    n: noRoot.length,
    of: `${sites.length} placement site(s)`,
    what: `neither column's roots reach the enclosing function by a CALL edge — this run follows calls plus two named function-pointer handovers (${REALM_REGISTRAR}'s argument and ${PLATFORM_TABLE}[]'s fields), so a placement registered through any other indirection lands here rather than in a column`,
    rows: noRoot.map((s) => `${s.u.rel}:${s.line}  in ${s.f.name}() (${s.col})`),
  });

  /* THE DOOR COUNT IS ITSELF A FLOOR. An interface object put on a global with an ordinary property write asks
     §3.8 nothing, so it is not a door call and cannot be one of the sites above. Keyed on the IDENTIFIER — a
     name the corpus declares an exposure set for — and never on the receiver: a receiver-anchored search has
     failed twice in this tree, it cannot tell a defect from the idiom, and a `(JSValue)` cast between two
     arguments hides a site from it entirely. Matched over the BALANCED argument list for the same reason. */
  const bareWrites = [];
  for (const u of corpus) {
    for (const f of u.fns) {
      for (const c of callsIn(u.code, f.bodyStart, f.bodyEnd)) {
        if (!/^(JS_SetPropertyStr|JS_DefinePropertyValueStr|JS_DefinePropertyValueGetSet)$/.test(c.name)) continue;
        const a = argsOfCall(u.code, c.paren);
        if (!a) continue;
        const nameArg = a.map(literal).find((l) => l !== null && exposure.has(l));
        if (nameArg === undefined) continue;
        bareWrites.push(`${u.rel}:${lineOf(u.lines, c.at)}  "${nameArg}" in ${f.name}() via ${c.name}`);
      }
    }
  }
  blind.push({
    n: bareWrites.length,
    of: "the ordinary property writes in the audited paths, keyed on a name argument that is an identifier IDL_EXPOSURE declares",
    what: "not a call to a §3.8 door, so outside the site count entirely — this run cannot say whether such a write places an interface object or writes an ordinary member that shares the name",
    rows: bareWrites,
  });

  const doorRefused = [...doors].filter(([, d]) => d.idArg === null);
  blind.push({
    n: doorRefused.length,
    of: `${doors.size} derived door(s)`,
    what: "the identifier argument could not be read off the door's own parameter list, so every call to it is unresolved above",
    rows: doorRefused.map(([n, d]) => `${n} — ${d.why}`),
  });

  for (const b of blind) {
    L.push(`  ${String(b.n).padStart(4)} of ${b.of} — ${b.what}`);
    for (const r of (all || b.rows.length <= 8 ? b.rows : b.rows.slice(0, 8))) L.push(`         ${r}`);
    if (!all && b.rows.length > 8) L.push(`         … ${b.rows.length - 8} more; --all for every one`);
  }

  L.push("");
  L.push(`SPELLING SEARCHED: a call to one of the derived doors, matched over a BALANCED argument list, from a function this run found by walking brace depth. Every count above is a count OF THAT, and a construct this run cannot read is a row in the second ledger and never a zero in the first.`);
  L.push("");
  return L.join("\n");
}

/* ---- the positive control ------------------------------------------------------------------------------- */

/* CONSTRUCTED INPUT THIS RUN MUST CLASSIFY CORRECTLY, printed with what it answered. The derivation this file
   replaces answered UNREACHED for exactly the interesting names because its scan skipped a one-line thunk, and
   called that "a malformed question, not a finding" — so the first case here is that thunk. A checker nobody
   has watched classify anything has calibrated nothing, and an empty answer from one is not evidence. */
const SELFTEST_C = `
static void one_line_thunk(JSContext *c, const PlatformAgent *a) { (void)a; comp_init(c); }
void comp_init(JSContext *ctx) { realm_declare_intrinsic(comp_realm_install); }
static void comp_realm_install(JSContext *ctx) {
    idl_define_global_property_reference(ctx, global, "LiteralIface", ctor);
}
static const struct { const char *iface; int slot; } TBL[2] = {
    { "TableIfaceA", 0 },
    { "TableIfaceB", 1 },   /* a comment holding a "decoy string literal" */
};
static void table_install(JSContext *ctx, JSValueConst global) {
    int k;
    for (k = 0; k < 2; k++) idl_define_global_property_reference(ctx, global, TBL[k].iface, mint(ctx));
}
static const char *const FLAT[2] = { "FlatIfaceA", "FlatIfaceB" };
static void flat_install(JSContext *ctx, JSValueConst global) {
    idl_define_global_property_reference(ctx, global, FLAT[i], mint(ctx));
}
static void helper(JSContext *ctx, JSValueConst global, const char *name, JSValue ctor) {
    idl_define_global_property_reference(ctx, global, name, ctor);
}
static void helper_caller(JSContext *ctx, JSValueConst global) {
    helper(ctx, global, "HelperIface", mint(ctx));
}
static void opaque_install(JSContext *ctx, JSValueConst global) {
    idl_define_global_property_reference(ctx, global, pick_name(ctx), mint(ctx));
}
/* A table whose rows are an X-macro expansion, the shape core/html/html_element.c's interface table has. */
static const struct { const char *tag; const char *iface; int step; } XTBL[] = {
#define X(tag, iface, step) { tag, iface, step },
    SELFTEST_IFACES(X)
#undef X
};
static void xmacro_install(JSContext *ctx, JSValueConst global) {
    idl_define_global_property_reference(ctx, global, XTBL[i].iface, mint(ctx));
}
/* Reached from a realm intrinsic AND from a document install — the both column. */
static void shared_install(JSContext *ctx, JSValueConst global) {
    idl_define_global_property_reference(ctx, global, "SharedIface", mint(ctx));
}
static void comp_doc_install(JSContext *ctx, JSValueConst g, const PlatformDocument *d) { shared_install(ctx, g); }
static void comp_realm_two(JSContext *ctx) { shared_install(ctx, JS_GetGlobalObject(ctx)); }
void comp_two_init(JSContext *ctx) { realm_declare_intrinsic(comp_realm_two); }
/* An ordinary property write naming an interface — asks §3.8 nothing, so it is outside the site count and is
   a blind-spot magnitude instead. Written with a cast between the arguments, because that is the spelling a
   receiver-anchored search cannot see. */
static void bare_write(JSContext *ctx, JSValueConst global) {
    JS_SetPropertyStr(ctx, (JSValue)global, "WinIface", mint(ctx));
}
/* Reached from neither root — the no-root column, which must not read as an empty finding. */
static void orphan_install(JSContext *ctx, JSValueConst global) {
    idl_define_global_property_reference(ctx, global, "OrphanIface", mint(ctx));
}
/* An ASTRAL character in a comment — \u{1F308} — shifts every offset after it by one if the scanner splits by
   code point rather than by code unit, and the two functions below then fall out of the parse entirely. */
static void after_astral(JSContext *ctx, JSValueConst global) {
    idl_define_global_property_reference(ctx, global, "AfterAstralIface", mint(ctx));
}
`;

const SELFTEST_LIST_H = `
#define SELFTEST_IFACES(X) \\
    X("xa", "XMacroIfaceA", 1) \\
    X("xb", "XMacroIfaceB", 2) \\
    X("xc", "XMacroIfaceA", 3)
`;

function selftest() {
  const L = [];
  L.push(bar("POSITIVE CONTROLS — constructed input, and what each parser answered"));
  const code = blankComments(SELFTEST_C);
  const fns = parseFunctions(code);
  const unit = { file: "<selftest>", rel: "<selftest>", code, lines: lineIndex(code), fns };
  const listCode = blankComments(SELFTEST_LIST_H);
  const listUnit = { file: "<selftest-list>", rel: "<selftest-list>", code: listCode, lines: lineIndex(listCode), fns: parseFunctions(listCode) };
  const corpus = [unit, listUnit];

  const expectFns = [
    "one_line_thunk", "comp_init", "comp_realm_install", "table_install",
    "flat_install", "helper", "helper_caller", "opaque_install", "xmacro_install",
    "shared_install", "comp_doc_install", "comp_realm_two", "comp_two_init",
    "bare_write", "orphan_install", "after_astral",
  ];
  const got = fns.map((f) => f.name);
  let ok = true;
  const check = (label, expected, actual) => {
    const pass = JSON.stringify(expected) === JSON.stringify(actual);
    if (!pass) ok = false;
    L.push(`  ${pass ? "OK  " : "WRONG"}  ${label}`);
    L.push(`          expected  ${JSON.stringify(expected)}`);
    L.push(`          answered  ${JSON.stringify(actual)}`);
  };

  check("every function definition is found, the one-line thunk included", expectFns, got);
  check(
    "the one-line thunk's body is read — its call is seen, not skipped past",
    ["comp_init"],
    callsIn(code, fns[0].bodyStart, fns[0].bodyEnd).map((c) => c.name),
  );
  check("the realm registration is read as a root", ["comp_realm_install", "comp_realm_two"], [...realmIntrinsics(corpus)]);

  const site = (fnName) => {
    const f = fns.find((x) => x.name === fnName);
    const c = callsIn(code, f.bodyStart, f.bodyEnd).find((x) => x.name === DOOR_SEED || x.name === "helper");
    const a = argsOfCall(code, c.paren);
    const r = resolveIdentifier(unit, f, a[2], corpus);
    return r.unresolved ? { unresolved: true } : { names: r.names, channel: r.channel };
  };
  check("a string literal resolves", { names: ["LiteralIface"], channel: "literal" }, site("comp_realm_install"));
  check(
    "a `.field` of a struct table resolves to its rows, and a decoy literal inside a comment is not one of them",
    { names: ["TableIfaceA", "TableIfaceB"], channel: "table" },
    site("table_install"),
  );
  check("a flat string table resolves", { names: ["FlatIfaceA", "FlatIfaceB"], channel: "table" }, site("flat_install"));
  check(
    "a shared helper's parameter resolves from the helper's caller",
    { names: ["HelperIface"], channel: "helper-parameter" },
    site("helper"),
  );
  check("a computed identifier is REFUSED rather than guessed at", { unresolved: true }, site("opaque_install"));
  check(
    "a placement AFTER an astral character in a comment is still read — the scanner splits by code unit",
    { names: ["AfterAstralIface"], channel: "literal" },
    site("after_astral"),
  );

  check(
    "an X-macro table resolves through the list macro in another unit, deduplicated",
    { names: ["XMacroIfaceA", "XMacroIfaceB"], channel: "x-macro:SELFTEST_IFACES" },
    site("xmacro_install"),
  );

  /* THE COLUMN CLASSIFICATION HAS ITS OWN CONTROLS, because `both` and `no-root` both came back EMPTY on the
     real corpus and an empty answer from an unwatched checker is not evidence. Constructed here so that each
     one has a member. */
  const { edges } = callGraph(corpus);
  const inRealm = reachable(edges, realmIntrinsics(corpus));
  const inDoc = reachable(edges, new Set(["comp_doc_install"]));
  const col = (f) => (inRealm.has(f) && inDoc.has(f) ? "both" : inRealm.has(f) ? "realm" : inDoc.has(f) ? "document" : "no-root");
  check(
    "a placement reached from a realm intrinsic, from a document install, from both, and from neither",
    ["realm", "document", "both", "no-root"],
    [col("comp_realm_install"), col("comp_doc_install"), col("shared_install"), col("orphan_install")],
  );

  const exp = parseExposure(`
    enum { IDL_GLOBAL_WINDOW = 1u << 8, IDL_GLOBAL_WORKER = 1u << 9, };
    #define IDL_EXPOSED_STAR 0u
    static const IdlExposureRow IDL_EXPOSURE[] = {
      { "StarIface",   IDL_EXPOSED_STAR },
      { "WinIface",    IDL_GLOBAL_WINDOW },
      { "WorkerIface", IDL_GLOBAL_WINDOW | IDL_GLOBAL_WORKER },
    };`);
  check(
    "the exposure header's three shapes each parse",
    [true, ["WINDOW"], ["WINDOW", "WORKER"]],
    [exp.table.get("StarIface").star, exp.table.get("WinIface").names, exp.table.get("WorkerIface").names],
  );

  const bw = [];
  for (const f of fns) {
    for (const c of callsIn(code, f.bodyStart, f.bodyEnd)) {
      if (c.name !== "JS_SetPropertyStr") continue;
      const a = argsOfCall(code, c.paren);
      const nm = a.map(literal).find((l) => l !== null && exp.table.has(l));
      if (nm !== undefined) bw.push(`${f.name}:${nm}`);
    }
  }
  check(
    "an ordinary property write naming an interface is seen THROUGH a cast, keyed on the identifier and not the receiver",
    ["bare_write:WinIface"],
    bw,
  );
  check(
    "an identifier the exposure table does not declare has no row, so it is a blind spot and not a bucket",
    [true, true],
    [exp.table.get("NoSuchIface") === undefined, exp.table.has("WinIface")],
  );

  L.push("");
  L.push(ok ? "  every control classified as constructed." : "  A CONTROL DISAGREED — the answers above are about this parser, not about the tree.");
  L.push("");
  return L.join("\n");
}

const argv = process.argv.slice(2);
if (argv.includes("--selftest")) {
  const out = selftest();
  process.stdout.write(out);
  /* THE AUDIT EXITS 0 WHATEVER IT FINDS — it is not a gate, and a red exit code for a finding is an instruction
     the next reader obeys. A DISAGREEING CONTROL IS NOT A FINDING: it is a statement that this file's parsers
     do not do what this file says they do, so the numbers above it are about the parser rather than about the
     tree. That is the one condition worth a non-zero status, and it cannot become a tree gate because nothing
     runs --selftest as one. */
  process.exit(/A CONTROL DISAGREED/.test(out) ? 1 : 0);
}
process.stdout.write(run(argv));
