/* WHAT A PLAIN PARSE OF A REAL BUNDLE RECOVERS, SO THAT WHAT EXECUTION ADDS CAN BE PRICED.
 *
 * THE QUESTION THIS EXISTS TO ANSWER IS STRATEGIC AND WAS ASKED BY THE PROJECT OWNER: does forced execution
 * still learn anything on a large real web app that a plain Babel parse of the same bundle would not? Most of
 * this engine is a browser, and a browser is expensive; if a parser recovers the same surface, the cost buys
 * nothing on the @H half and has to be justified on VALUES and on the @S half or not at all.
 *
 * IT IS A CONTROL AND NOT A TARGET, AND THAT DISTINCTION IS LOAD-BEARING RATHER THAN A DISCLAIMER. CLAUDE.md
 * §What-the-tool-produces says `netdiff --unused` is "a DIAGNOSTIC that the solver dominates the live page,
 * NOT the optimization target"; the same sentence governs this file. A number here going UP is not progress
 * and a number here going DOWN is not progress — the only thing either says is how much of the surface a
 * cheaper instrument already reaches. Optimising the engine toward this baseline would be optimising toward
 * a parser, which is the one thing the engine is not.
 *
 * WHAT IT DOES NOT MEASURE, STATED FIRST BECAUSE A COVERAGE FIGURE THAT DOES NOT NAME ITS DENOMINATOR IS THE
 * DEFECT CLAUDE.md §a-coverage-figure-states-what-it-is-a-fraction-of IS ABOUT. This reads the JS DOOR only:
 * the addresses a bundle's own CODE names. The engine has a second door — markup — and `git grep -c
 * endpoint_record` puts EIGHT of its recording sites in `html_link.c`, three in `html_image.c`, one each in
 * `html_script.c` and `html_form.c`, against one each in `fetch.c`, `xml_http_request.c`,
 * `navigator_beacon.c`, `multipart_batch.c`, `reply_decode.c` and `engine.c`. A `<script src>` and a `<link
 * href>` are recovered COMPLETELY by any HTML parser and by the engine alike, so counting them would add one
 * number to both sides of the comparison and settle nothing. They are excluded, they are excluded on
 * purpose, and the excluded population is reported beside the included one rather than left to be inferred.
 *
 * THE POPULATION COMES FROM `engine/corpus_programs.mjs` AND NOT FROM A FILENAME, for the reason that file's
 * own header gives at length: a fetcher folded a URL's query into the saved name, so three genuine shipped
 * bundles had extensions no list would carry, and a corpus that is quietly smaller reports a smaller number
 * in the flattering direction. That module joins bytes to the server's own `Content-Type` by sha256 and
 * THROWS on any file it cannot type. THIS FILE REPRODUCES ITS PUBLISHED TOTALS BEFORE PRINTING ANY BREAKDOWN
 * OF THEM, which is the calibration CLAUDE.md §AND-WHERE-THE-SUBJECT-ALREADY-PUBLISHES-A-TOTAL prescribes:
 * a probe that is a second implementation of somebody's selector can walk what the tree DECLARES where the
 * instrument walks what it INSTALLS, and the disagreement is invisible in its output.
 *
 * THE DOORS ARE READ OUT OF THE ENGINE AND NOT INVENTED HERE. A list of "things that look like requests"
 * would be a second copy of a fact `endpoint_record`'s callers already state, and the copy anyone writes
 * first is the one that drops a door. Each entry in `DOORS` below names the engine file whose
 * `endpoint_record` call it mirrors, so the two can be diffed by anyone; a door with no engine site is
 * marked as such and is a FLOOR-WIDENING rather than a comparison.
 *
 * IT IS A PARSE AND NEVER A REGEX OVER SOURCE TEXT. CLAUDE.md §RUN-DON'T-MATCH bans the second, and it bans
 * it in BOTH directions here: a regex baseline would be artificially weak, which would make the engine look
 * artificially good, which is the one result this file must not manufacture. `@babel/parser` is the parser
 * the question was asked about by name. MEASURED at the corpus this ran against: it parses 721 of 724
 * non-`index` files and the three refusals are HTML documents, so the parse is not the limiting factor on
 * the static side and cannot be offered as an excuse for a low number.
 *
 * THE KIND PARTITION IS THE WHOLE ANSWER AND A SINGLE COUNT WOULD DECIDE THE QUESTION WRONGLY. A parser and
 * an interpreter differ on exactly one axis — whether the URL's value is in the TEXT or only in a RUN — so
 * the four kinds below are not a presentation choice:
 *   LITERAL  the argument is one string literal. A parse has it; so does the engine; execution adds nothing.
 *   FOLDED   the argument is a computation over bindings this file could resolve WITHOUT RUNNING ANYTHING.
 *            A parse has it too, which is the half that makes the baseline fair rather than a strawman.
 *   SHAPE    it resolves to literal text plus at least one hole — `"/api/" + region`. A parse gets the
 *            SHAPE and can never get the VALUE; the engine can, because it ran the concatenation on a real
 *            operand. THIS ROW IS THE ENTIRE ARGUMENT FOR EXECUTION ON THE @H HALF.
 *   OPAQUE   it resolves to no literal text at all — a bare identifier, a member, a call. A parse knows
 *            only that a request happens here. Same reading as SHAPE and stronger.
 * A high LITERAL+FOLDED share is a finding AGAINST the browser on paths and must be reported as one.
 *
 * FOLDING IS DELIBERATELY CONSERVATIVE AND THE NUMBER IS THEREFORE A FLOOR FOR THE PARSE, WHICH IS THE
 * DIRECTION THAT COSTS THIS PROJECT RATHER THAN FLATTERS IT. An identifier is folded only where its name is
 * bound EXACTLY ONCE in the whole file and never assigned again, so no shadowing can make a fold wrong. A
 * real commercial extractor does interprocedural constant propagation and would fold MORE. Reporting a floor
 * for the side whose strength is inconvenient is the only honest direction: CLAUDE.md
 * §A-SWEEP-IS-TRUSTED-BY-ITS-METHOD says a static derivation over text is a lower bound wearing a total's
 * clothes, and here the lower bound belongs to the baseline rather than to the subject.
 *
 * A ZERO IN ANY ROW IS READ AGAINST THE BASE RATE PRINTED BESIDE IT. `pathish` counts distinct string
 * literals in the same programs that LOOK like addresses and are attached to no door — the population a
 * naive "grep the bundle for /api/" tool reports. It is NOT an endpoint count and must never be quoted as
 * one; it is there so that "the parse found N request sites" can be read against "and M address-shaped
 * strings it could not attach to any request", which is what finding something would have looked like.
 *
 * THE CORPUS IS NOT TRACKED AND THE DRIVER IS, which is `testing/corpus/README.md`'s split and not this
 * file's choice: this repository carries no copy of anybody else's site. So every figure printed here is a
 * fact about ONE FETCH, at the instant that fetch's own manifest names, and the instant is printed with the
 * numbers. Without a corpus this file THROWS and the throw carries the command that makes one — a throw that
 * names a hazard and offers no exit is the shape CLAUDE.md
 * §A-CONTRACT-THAT-NAMES-A-HAZARD-AND-OFFERS-NO-EXIT forbids.
 *
 * THE DECLARED BLIND SPOTS CARRY A SIZE AND NOT A SENTENCE, because a floor that names what it excludes
 * without measuring it is read as a total anyway:
 *   - `el.src = url` / `el.href = url` — the door `html_script.c` and `html_link.c` DO record and no door
 *     above reads, because `.src` is a property of many things that are not elements and admitting it to
 *     the door set would buy recall with precision this comparison cannot afford. THAT EXCLUSION STANDS
 *     AND ITS SIZE IS NOW COUNTED ON EVERY RUN, per site, in the same four kinds, printed as THE DOOR
 *     SET'S OWN BLIND SPOT and summed into no door total. The count is an OVER-count of elements by
 *     construction — every `.src`/`.href` assignment in the file, element or not — which is the safe
 *     direction for the size of a blind spot: a blind spot stated too large certifies nothing, while one
 *     stated too small is read as a clean bill.
 *     THIS PARAGRAPH USED TO CARRY THE SIZE AS FOUR FROZEN NUMBERS — "217 `.src =` and 212 `.href =`
 *     assignments in the corpus and 21 and 8 of them have a single string literal on the right — so 400
 *     of 429 are computed" — and it is rewritten rather than deleted because the ARGUMENT is right and a
 *     reader who re-derives it will re-add the figures. A count over a corpus this repository does not
 *     carry is unreproducible BY CONSTRUCTION, so it cannot be checked and cannot go loudly wrong: a
 *     re-derivation at a later fetch answered 218 and 215 against its 217 and 212. The claim that a reader
 *     widening this file "should expect to add mostly OPAQUE rows" is the part that HELD and is what the
 *     band now measures rather than asserts.
 *     THE REASON THE BAND EXISTS RATHER THAN A WIDER DOOR SET IS A MEASUREMENT AND NOT A PREFERENCE. The
 *     PROGRAM door is BIMODAL BY BUNDLER: a bundle that ships native `import()` scores in the door, and
 *     one whose bundler compiled `import()` away into a chunk-id map plus a `<script>` injection scores
 *     ZERO there. The address is then composed through a CALL (`script.src = R.tu(R.p + R.u(id))`), so a
 *     `.src` door would add one OPAQUE row per runtime and recover NO address — recall bought for nothing,
 *     and precision spent. What would recover those addresses is interprocedural folding through the
 *     chunk-URL function, which is a different subproblem and is named as one below.
 *   - a library wrapper (`axios.get`, `$.ajax`, an SDK `request()`), for the reason the DOORS table gives.
 *   - anything a bundle reaches through a member call this file cannot name, which is unbounded and is why
 *     the site count here is stated as a floor everywhere it is stated at all.
 *
 * NAMED RESIDUAL — THE CHUNK MANIFEST. WHAT IS NOT COVERED: a program address a bundler emits as a MAP
 * (`R.u = id => 9016===id ? "static/chunks/a.js" : ...`, or `"p/"+({id:"hash"})[id]+".js"`) composed with a
 * public-path literal and delivered to `script.src`. A parse has every one of those addresses in plain
 * literal text and this file recovers none of them, because the fold is per-argument and the composition
 * crosses a function boundary. WHAT THE NEXT DIFF BUILDS: a fold that resolves a PROPERTY assigned exactly
 * once in the file (the same bound-once-never-reassigned discipline `collectBinds` already applies to
 * names, which is what makes it sound) and that inlines a single-parameter function whose body folds,
 * ENUMERATING a ternary chain or a computed member on an object literal into the set of addresses it can
 * return — a 1-site-to-N-addresses relation this file's row shape does not yet have. It may NOT be keyed
 * on a bundler: CLAUDE.md §RUN-DON'T-MATCH bans bundler recognition by name, so the rule has to be about
 * the EXPRESSION and never about whose runtime emitted it. HOW ITS ABSENCE WOULD SHOW: a site whose
 * PROGRAM-door row reads 0 while the blind-spot row beside it reads a nonzero OPAQUE count — read those
 * two columns together in the per-site PROGRAM block, which is the reason that block prints them adjacent.
 *
 * WHAT COMPLETES THE COMPARISON, NAMED SO IT CAN BE RUN RATHER THAN RE-DERIVED. This file is one half. The
 * other half is not "the engine's endpoint count", which answers a different question: `solver/result.c`
 * publishes `epEmitted` and `epPreProgram`, and its own comment says `epEmitted - epPreProgram` is "the most
 * addresses forced execution can have contributed to this document's surface, so a run reading them EQUAL
 * learned nothing the markup did not already state". THAT DIFFERENCE IS THE ENGINE SIDE OF THIS COMPARISON.
 * The measurement that closes it: drive a real app page through the WASM artifact with
 * `testing/harness.js restart` at a revision that publishes both rows, and read the difference.
 * IT CANNOT BE TAKEN FROM THE ARCHIVE TODAY, and the reason is a partition rather than an absence — the
 * archived rows that publish BOTH are ten, and they are two disjoint populations: the real-SPA rows are the
 * NATIVE `--abi` arm, where the reply door reads 43 asked / 1 answered and the surface is 43 = 43 (execution
 * contributed nothing), and the rows where execution contributed everything (516 = 516 - 0) are the build's
 * own SYNTHETIC fixture, whose `epPreProgram` is 0 because it has no markup door at all. No archived row is
 * a REAL page under the WASM artifact publishing `epPreProgram`. Under that artifact the same page's reply
 * door reads 43 asked / 43 answered with `rowsAwaitingBytes` 0 — so the bytes DO arrive there — and its
 * fork count is 3 against the native arm's 6242, which is why the missing row is worth taking rather than
 * predicted: one arm fetches and does not explore, the other explores and does not fetch.
 *
 * THIS FILE'S OWN NUMBERS ARE LOAD-INDEPENDENT AND THAT IS WHY THEY MAY BE QUOTED AT ALL. CLAUDE.md §Testing
 * forbids quoting a rate taken while a build loads the box, and everything here is a COUNT over bytes that
 * do not move: the same corpus gives the same answer on a busy machine and an idle one. The only figure that
 * is not is the parse time, which is printed as a fact about the run and is in no conclusion.
 *
 *   NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs   # make a corpus
 *   node testing/static_surface.mjs                                     # read one
 *   node testing/static_surface.mjs --json > out.json                   # ... as data
 *   node testing/static_surface.mjs --site excalidraw --examples 20     # ... and look at the rows
 */

import { readFileSync, statSync } from "node:fs";
import { createHash } from "node:crypto";
import { relative, resolve } from "node:path";
import { parse } from "@babel/parser";
import { VISITOR_KEYS } from "@babel/types";
import { corpusPrograms, PROGRAM, DOCUMENT, essenceOf } from "../engine/corpus_programs.mjs";

const TAG = "static_surface";
const die = (s) => { throw new Error(`[${TAG}] ${s}`); };

/* ── THE DOORS ────────────────────────────────────────────────────────────────────────────────────────────
   Each row mirrors a call site that reaches `endpoint_record` in the engine, and names it, so that a reader
   can diff this table against `git grep -n 'endpoint_record(' engine/host` rather than trust it. `engine`
   is the file that records it; a row whose `engine` is null is a door this file reads and the engine's @H
   surface does NOT record as an endpoint, and it is counted apart for exactly that reason — mixing them
   would put addresses on the static side that the engine was never asked for, which is a comparison between
   two different questions.
   THE MATCH IS ON THE PLATFORM NAME AND NEVER ON A RECEIVER. `engine/js_code_refs.mjs` records that a
   receiver's spelling carries no information about the thing being asked, and a minifier renames every local
   while leaving `fetch`, `XMLHttpRequest`, `open` and `sendBeacon` alone because they are the platform's.
   WHAT IS NOT HERE AND WHY, because a floor that does not say what it excludes is read as a total: a library
   wrapper (`axios.get`, `$.ajax`, an SDK's `request()`) is NOT matched. `.get(` and `.post(` are ordinary
   method names on Map, URLSearchParams, Headers and every model object in a bundle, so keying on them would
   report a number dominated by things that are not requests — the precision failure that would make this
   baseline useless in the other direction. Every request such a wrapper ultimately issues passes through
   `fetch` or `XMLHttpRequest` INSIDE the library, so the address is seen there as OPAQUE (the wrapper's own
   variable) rather than missed entirely: the effect is to move rows from SHAPE/LITERAL into OPAQUE, which
   UNDERSTATES what a parse recovers. That is the safe direction for this file and it is still a floor. */
/* EVERY DOOR CARRIES ITS DESTINATION CLASS AND THE TOTAL IS NEVER PRINTED WITHOUT IT, because a single
   count over both classes decides this question wrongly and decides it in the flattering direction.
   Fetch §2.2.5 "Requests"' DESTINATION is the concept and CLAUDE.md states it in the engine's own words —
   "a reply that becomes a PROGRAM against one that becomes a VALUE". A dynamic `import()` of a bundler chunk
   is a PROGRAM load: the page loading itself, an address a browser computes from a manifest the bundler
   emitted, and NOT an API this product exists to surface. A `fetch` is a VALUE load and is.
   MEASURED on the corpus this ran against, which is why this is a partition and not a note: 839 of 1071
   request sites were PROGRAM-door and 232 were DATA-door, so a headline over the union would be 78% a
   statement about chunk loading. Worse, the two classes have OPPOSITE kind profiles — a bundler emits its
   chunk addresses as literals or as a folded table by construction, so the PROGRAM class is where a parse
   looks strongest and it is the class the @H product cares least about. Summing them would let the easy
   population answer for the hard one. */
const DOORS = [
  { id: "fetch",         cls: "data",    engine: "browser/core/fetch/fetch.c",            kind: "callee-global",  name: "fetch",            urlArg: 0 },
  { id: "xhr.open",      cls: "data",    engine: "browser/core/xhr/xml_http_request.c",   kind: "member-call",    name: "open",             urlArg: 1, minArgs: 2, arg0Method: true },
  { id: "sendBeacon",    cls: "data",    engine: "browser/core/frame/navigator_beacon.c", kind: "member-call",    name: "sendBeacon",       urlArg: 0 },
  { id: "new WebSocket", cls: "data",    engine: null,                                    kind: "new",            name: "WebSocket",        urlArg: 0 },
  { id: "new EventSource", cls: "data",  engine: null,                                    kind: "new",            name: "EventSource",      urlArg: 0 },
  { id: "import()",      cls: "program", engine: "browser/core/html/html_script.c",       kind: "dynamic-import", name: "import",           urlArg: 0 },
  { id: "importScripts", cls: "program", engine: null,                                    kind: "callee-global",  name: "importScripts",    urlArg: 0 },
  { id: "new Worker",    cls: "program", engine: null,                                    kind: "new",            name: "Worker",           urlArg: 0 },
  { id: "new SharedWorker", cls: "program", engine: null,                                 kind: "new",            name: "SharedWorker",     urlArg: 0 },
];
const DOOR_BY_NAME = new Map();
for (const d of DOORS) {
  const k = d.kind + ":" + d.name;
  if (DOOR_BY_NAME.has(k)) die(`two DOORS rows share ${k}`);
  DOOR_BY_NAME.set(k, d);
}

/* ── FOLDING ──────────────────────────────────────────────────────────────────────────────────────────────
   Returns { text, holes } where `text` is the literal bytes recovered with each unresolved subexpression
   rendered as `{n}` — the SAME rendering `solver/endpoint.c` uses for a concolic hole, so a static row and
   an engine row are comparable strings rather than two notations for one address — and `holes` counts them.
   `branches` collects the alternative arm of every conditional that folds both ways, which is a place a
   PARSE beats a RUN: execution takes one arm unless it forks, and the text carries both. */
const MAX_DEPTH = 24;

function fold(node, binds, depth) {
  if (node == null) return { text: "{?}", holes: 1 };
  if (depth > MAX_DEPTH) return { text: "{?}", holes: 1 };
  switch (node.type) {
    case "StringLiteral":
      return { text: node.value, holes: 0 };
    case "NumericLiteral":
      return { text: String(node.value), holes: 0 };
    case "TemplateLiteral": {
      let t = "", h = 0;
      for (let i = 0; i < node.quasis.length; i++) {
        t += node.quasis[i].value.cooked ?? node.quasis[i].value.raw ?? "";
        if (i < node.expressions.length) {
          const r = fold(node.expressions[i], binds, depth + 1);
          t += r.holes ? r.text : r.text;
          h += r.holes;
        }
      }
      return { text: t, holes: h };
    }
    case "BinaryExpression": {
      if (node.operator !== "+") return { text: "{?}", holes: 1 };
      const a = fold(node.left, binds, depth + 1), b = fold(node.right, binds, depth + 1);
      return { text: a.text + b.text, holes: a.holes + b.holes };
    }
    case "Identifier": {
      const b = binds.get(node.name);
      if (b && b.node) return fold(b.node, binds, depth + 1);
      return { text: "{?}", holes: 1 };
    }
    case "MemberExpression": {
      /* An object bound once to an object literal with literal keys — `const R={u:"/x"}; fetch(R.u)`. */
      if (node.computed || node.object.type !== "Identifier" || node.property.type !== "Identifier")
        return { text: "{?}", holes: 1 };
      const b = binds.get(node.object.name);
      if (!b || !b.node || b.node.type !== "ObjectExpression") return { text: "{?}", holes: 1 };
      for (const p of b.node.properties) {
        if (p.type !== "ObjectProperty" || p.computed) continue;
        const k = p.key.type === "Identifier" ? p.key.name : (p.key.type === "StringLiteral" ? p.key.value : null);
        if (k === node.property.name) return fold(p.value, binds, depth + 1);
      }
      return { text: "{?}", holes: 1 };
    }
    case "ConditionalExpression": {
      const a = fold(node.consequent, binds, depth + 1), b = fold(node.alternate, binds, depth + 1);
      if (a.holes === 0 && b.holes === 0) return { text: a.text, holes: 0, alt: b.text };
      return { text: "{?}", holes: 1 };
    }
    case "TSAsExpression":
    case "TSNonNullExpression":
    case "ParenthesizedExpression":
      return fold(node.expression, binds, depth + 1);
    default:
      return { text: "{?}", holes: 1 };
  }
}

/* ── ONE FILE ─────────────────────────────────────────────────────────────────────────────────────────────
   Two passes over one AST. The first collects the bindings that are safe to fold and the guard nesting; the
   second reads the doors. They are two passes rather than one because a bundle names a constant AFTER using
   it as often as before, and a single forward pass would fold a declaration's own order into the answer. */

function walk(root, enter, leave) {
  const stack = [{ node: root, entered: false }];
  while (stack.length) {
    const fr = stack[stack.length - 1];
    if (!fr.entered) {
      fr.entered = true;
      enter(fr.node);
      const keys = VISITOR_KEYS[fr.node.type] || [];
      const kids = [];
      for (const k of keys) {
        const v = fr.node[k];
        if (Array.isArray(v)) { for (const c of v) if (c && typeof c.type === "string") kids.push(c); }
        else if (v && typeof v.type === "string") kids.push(v);
      }
      for (let i = kids.length - 1; i >= 0; i--) stack.push({ node: kids[i], entered: false });
    } else {
      stack.pop();
      if (leave) leave(fr.node);
    }
  }
}

/* WHICH NAMES MAY BE FOLDED. A name is foldable only if the WHOLE FILE binds it once and assigns it never.
   That is stronger than scope-correctness and is chosen for it: without a scope graph, a name bound twice
   could be folded across a shadow, and a fold that is wrong INVENTS an address — the one failure this file
   may not have, because an invented static row would be scored as "the parse found it" against an engine
   that did not. A name bound once cannot be shadowed by anything. */
function collectBinds(ast) {
  const count = new Map();   // name -> number of binding occurrences anywhere in the file
  const init = new Map();    // name -> initializer node of its (single) binding
  const assigned = new Set();

  const bind = (id, valueNode) => {
    if (!id || id.type !== "Identifier") return;
    count.set(id.name, (count.get(id.name) || 0) + 1);
    if (valueNode) init.set(id.name, valueNode); else assigned.add(id.name);
  };
  const bindPattern = (pat) => {
    if (!pat) return;
    if (pat.type === "Identifier") { bind(pat, null); return; }
    walk(pat, (n) => { if (n.type === "Identifier") bind(n, null); });
  };

  walk(ast, (n) => {
    switch (n.type) {
      case "VariableDeclarator":
        if (n.id.type === "Identifier") bind(n.id, n.init || null);
        else bindPattern(n.id);
        break;
      case "FunctionDeclaration":
      case "ClassDeclaration":
        if (n.id) bind(n.id, null);
        for (const p of n.params || []) bindPattern(p);
        break;
      case "FunctionExpression":
      case "ArrowFunctionExpression":
      case "ClassMethod":
      case "ObjectMethod":
        for (const p of n.params || []) bindPattern(p);
        break;
      case "CatchClause":
        bindPattern(n.param);
        break;
      case "ImportSpecifier":
      case "ImportDefaultSpecifier":
      case "ImportNamespaceSpecifier":
        bind(n.local, null);
        break;
      case "AssignmentExpression":
        if (n.left.type === "Identifier") assigned.add(n.left.name);
        break;
      case "UpdateExpression":
        if (n.argument.type === "Identifier") assigned.add(n.argument.name);
        break;
      default: break;
    }
  });

  const binds = new Map();
  for (const [name, c] of count)
    if (c === 1 && !assigned.has(name) && init.has(name)) binds.set(name, { node: init.get(name) });
  return { binds, count };
}

/* GUARD DEPTH IS PROVENANCE AND NOT DECORATION. CLAUDE.md §What-the-tool-produces' proposition is "what the
   bundle CAN do but didn't", and a parse reaches a gated call site whether the gate is taken or not — which
   is the one axis on which a parse is structurally STRONGER than a run, and it must be measured rather than
   conceded or assumed. The depth is the number of enclosing tests a runtime would have to satisfy: an `if`
   or `switch` body, a `?:` arm, the right-hand side of `&&`/`||`/`??`, and a `catch`. */
const GUARDS = new Set(["IfStatement", "ConditionalExpression", "SwitchCase", "CatchClause"]);

function readFile(src, filename) {
  let ast = null, err = null;
  for (const sourceType of ["module", "script"]) {
    try { ast = parse(src, { sourceType, errorRecovery: false, plugins: [] }); err = null; break; }
    catch (e) { err = e; }
  }
  if (!ast) return { parsed: false, error: String(err && err.message || err).slice(0, 160), sites: [], pathish: new Set(), blind: [], xhrOpenSkippedNonLiteralMethod: 0 };

  const { binds, count: bindCount } = collectBinds(ast);
  const sites = [];
  const pathish = new Set();
  const blind = [];
  let xhrOpenSkippedNonLiteralMethod = 0;
  const attached = new Set();          // node identity of URL args, so a door's own literal is not double-counted
  let guard = 0;
  const guardStack = [];

  walk(ast, (n) => {
    if (GUARDS.has(n.type)) { guardStack.push(n); guard++; }
    else if (n.type === "LogicalExpression") { guardStack.push(n); guard++; }

    let door = null, args = null;
    if (n.type === "CallExpression" || n.type === "OptionalCallExpression") {
      const c = n.callee;
      if (c && c.type === "Identifier")              door = DOOR_BY_NAME.get("callee-global:" + c.name);
      else if (c && c.type === "Import")             door = DOOR_BY_NAME.get("dynamic-import:import");
      else if (c && (c.type === "MemberExpression" || c.type === "OptionalMemberExpression") &&
               !c.computed && c.property.type === "Identifier")
                                                     door = DOOR_BY_NAME.get("member-call:" + c.property.name);
      args = n.arguments;
    } else if (n.type === "NewExpression" && n.callee && n.callee.type === "Identifier") {
      door = DOOR_BY_NAME.get("new:" + n.callee.name);
      args = n.arguments;
    }

    if (door && args) {
      if (door.minArgs && args.length < door.minArgs) door = null;
      /* `.open(` is XMLHttpRequest's only through a first argument that is an HTTP method. Without that
         test the row would be dominated by `window.open`, `db.open`, and every library's `open()` — which
         is the precision failure that makes a static number meaningless. A non-literal first argument is
         NOT admitted: the method would then be unknown too, and a row whose method and address are both
         unknown says only "a call happened", which no comparison can use. That exclusion is a FLOOR and is
         reported as `xhrOpenSkippedNonLiteralMethod`. */
      if (door && door.arg0Method) {
        const m = args[0];
        if (!m || m.type !== "StringLiteral" || !/^(GET|POST|PUT|DELETE|PATCH|HEAD|OPTIONS|TRACE)$/i.test(m.value)) {
          door = null; xhrOpenSkippedNonLiteralMethod++;
        }
      }
    }

    if (door && args) {
      const a = args[door.urlArg];
      const r = a ? fold(a, binds, 0) : { text: "{?}", holes: 1 };
      const literalChars = r.text.replace(/\{\?\}/g, "").length;
      let kind;
      if (a && a.type === "StringLiteral") kind = "literal";
      else if (r.holes === 0) kind = "folded";
      else if (literalChars > 0) kind = "shape";
      else kind = "opaque";
      if (a) attached.add(a);
      /* WHETHER A STRONGER PARSER COULD HAVE DONE BETTER IS MEASURED HERE AND NEVER CONCEDED OR ASSUMED.
         This file's fold is deliberately conservative, so `opaque` is a FLOOR for the parse and the obvious
         objection is that a real commercial extractor with interprocedural constant propagation would fold
         more. That objection is answerable with two numbers rather than an opinion, and both are carried on
         every row: the SHAPE of the argument, and — where it is a bare name — HOW MANY TIMES THAT NAME IS
         BOUND IN ITS OWN FILE. A name bound once is resolvable by any parser and this file already folds it;
         a name bound a hundred times is a minifier's reused register, and no name-based resolution can touch
         it without a full scope graph AND the caller graph behind it.
         A CEILING PROBE THAT IGNORED SHADOWING WAS BUILT FIRST AND IS RECORDED AS REFUTED, because the
         wrong method is what a later reader would otherwise repeat: it answered "115 of 132 resolvable" and
         its own samples bound the URL name to `"custom"`, `"$default"`, `"replace"` and `"="`. It had
         measured NAME COLLISION IN MINIFIED CODE and not resolvability at all. The binding COUNT is the
         sound form of the same question and needs no judgement to read. */
      const argShape = !a ? "absent"
        : a.type === "Identifier" ? "Identifier"
        : (a.type === "MemberExpression" && a.object && a.object.type === "ThisExpression") ? "this.member"
        : a.type;
      const argBinds = (a && a.type === "Identifier") ? (bindCount.get(a.name) || 0) : null;
      sites.push({
        argShape, argBinds, argNameLen: (a && a.type === "Identifier") ? a.name.length : null,
        door: door.id, cls: door.cls, engineDoor: door.engine !== null, kind, holes: r.holes,
        url: r.text, alt: r.alt || null, guard,
        method: door.arg0Method ? args[0].value.toUpperCase() : (door.id === "sendBeacon" ? "POST" : "GET"),
        line: n.loc ? n.loc.start.line : 0, file: filename,
      });
    }
  }, (n) => {
    if (GUARDS.has(n.type) || n.type === "LogicalExpression") { guardStack.pop(); guard--; }
  });

  /* THE BASE RATE. Every string literal in this program that looks like an address and is NOT the URL
     argument of a door — what a naive extractor would report and what this one deliberately does not. */
  walk(ast, (n) => {
    /* THE DECLARED BLIND SPOT, COUNTED RATHER THAN DESCRIBED. `el.src = url` / `el.href = url` is a door
       `html_script.c` and `html_link.c` DO record and the site channel above deliberately does not, because
       `.src` is a property of many things that are not elements. That exclusion is right and it was stated
       as a SENTENCE carrying two numbers frozen at a past corpus, which is the shape this file's own header
       forbids: a floor that names what it excludes without measuring it is read as a total. It is measured
       here, on every run, per site, in the same four kinds as a door — so a PROGRAM-door zero can be read
       against it. These rows are NOT sites and are summed into no door total; they are the size of what the
       door set cannot see, printed where a zero would otherwise be read as a clean bill. */
    if (n.type === "AssignmentExpression" && n.operator === "=") {
      const L = n.left;
      if (!L || L.type !== "MemberExpression" || L.computed || L.property.type !== "Identifier") return;
      if (L.property.name !== "src" && L.property.name !== "href") return;
      const r = fold(n.right, binds, 0);
      const literalChars = r.text.replace(/\{\?\}/g, "").length;
      const kind = n.right.type === "StringLiteral" ? "literal"
        : r.holes === 0 ? "folded" : literalChars > 0 ? "shape" : "opaque";
      blind.push({ prop: L.property.name, kind, url: r.text, file: filename,
                   line: n.loc ? n.loc.start.line : 0 });
      return;
    }
    if (n.type !== "StringLiteral" || attached.has(n)) return;
    const v = n.value;
    if (v.length < 2 || v.length > 512) return;
    if (/^https?:\/\/[^\s]+$/.test(v) || /^\/[A-Za-z0-9_][^\s"'<>]*$/.test(v)) pathish.add(v);
  });

  return { parsed: true, error: null, sites, pathish, blind, xhrOpenSkippedNonLiteralMethod };
}

/* ── THE ARMED CONTROL ────────────────────────────────────────────────────────────────────────────────────
   RUN ON EVERY INVOCATION, BEFORE ANY CORPUS IS READ, AND FATAL. CLAUDE.md §THE-ORDER-IS-FIXED-AND-IT-IS-TWO-
   RUNS: a probe whose expected output is silence, with no run in which the same probe shape SPOKE, has
   calibrated nothing — so each row below is an input this file must classify a stated way, and the NEGATIVE
   rows are inputs it must NOT see at all. A classifier that silently stopped matching `fetch` would report a
   smaller surface, and a smaller surface is the flattering direction here: it would read as "the parse
   recovers less", which is the answer that argues FOR the engine. This control is what stops that being
   indistinguishable from a true finding.
   EVERY `kind` AND EVERY `cls` APPEARS AT LEAST ONCE BELOW, asserted after the table runs, so a kind that
   became unreachable cannot go quiet — which is the defect a table of examples nobody counts always has. */
const SELFTEST = [
  // [ source, expected rows as `door|cls|kind|url` ... ]
  [`fetch("/api/users")`,                                   ["fetch|data|literal|/api/users"]],
  [`const B="/api/v2";fetch(B+"/users")`,                   ["fetch|data|folded|/api/v2/users"]],
  [`fetch("/api/"+region)`,                                 ["fetch|data|shape|/api/{?}"]],
  ["fetch(`/api/${r}/x`)",                                  ["fetch|data|shape|/api/{?}/x"]],
  [`fetch(u)`,                                              ["fetch|data|opaque|{?}"]],
  [`fetch(u.v)`,                                            ["fetch|data|opaque|{?}"]],
  [`const R={u:"/a/b"};fetch(R.u)`,                          ["fetch|data|folded|/a/b"]],
  [`const P="/p";fetch(c?P:"/q")`,                          ["fetch|data|folded|/p"]],
  [`x.open("GET","/t")`,                                    ["xhr.open|data|literal|/t"]],
  [`x.open("POST",u)`,                                      ["xhr.open|data|opaque|{?}"]],
  [`navigator.sendBeacon("/b",d)`,                          ["sendBeacon|data|literal|/b"]],
  [`import("./c.js")`,                                      ["import()|program|literal|./c.js"]],
  [`new Worker("/w.js")`,                                   ["new Worker|program|literal|/w.js"]],
  [`new WebSocket("wss://h/s")`,                            ["new WebSocket|data|literal|wss://h/s"]],
  [`if(a){fetch("/g")}`,                                    ["fetch|data|literal|/g"]],
  // NEGATIVES — a classifier that reports any of these is over-counting, which is the direction that would
  // make the parse look stronger than it is and the engine's contribution smaller than it is.
  [`window.open("/x","_blank")`,                            []],
  [`db.open("GET")`,                                        []],
  [`m.get("/api/x")`,                                       []],
  [`const s="/api/looks-like-an-endpoint"`,                 []],
  [`x.open(method,"/t")`,                                   []],
  // SHADOWING — the fold must REFUSE a name the file binds twice, because a wrong fold INVENTS an address.
  [`const B="/a";function f(){const B="/b";return fetch(B)}`, ["fetch|data|opaque|{?}"]],
];
const SELFTEST_GUARDED = new Set([`if(a){fetch("/g")}`]);

/* THE BLIND-SPOT CHANNEL IS ARMED SEPARATELY AND IN BOTH DIRECTIONS. Its whole job is to be the number a
   PROGRAM-door zero is read against, so a channel that silently stopped counting would make every such zero
   read as a clean bill — the one reading CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE exists to forbid. Each
   positive row must be classified the stated way AND must produce NO site row, because a blind-spot row
   that leaked into `sites` would move a number this file's conclusions are drawn from. */
const SELFTEST_BLIND = [
  [`s.src="/a.js"`,                 ["src|literal|/a.js"]],
  [`const B="/b/";s.src=B+"c.js"`,  ["src|folded|/b/c.js"]],
  [`s.src="/x/"+e`,                 ["src|shape|/x/{?}"]],
  [`s.src=P+u(e)`,                  ["src|opaque|{?}{?}"]],
  [`l.href="/s.css"`,               ["href|literal|/s.css"]],
  // NEGATIVES — none of these is an `.src`/`.href` assignment and counting one would inflate the size of
  // the blind spot, which is the direction that would make the door set look worse than it is.
  [`s.srcset="/a.js"`,              []],
  [`s[k]="/a.js"`,                  []],
  [`s.src+="/a.js"`,                []],
  [`fetch("/api/x")`,               []],
];

function selftest() {
  const seenKind = new Set(), seenCls = new Set();
  let spoke = 0;
  for (const [src, want] of SELFTEST) {
    const r = readFile(src, "<selftest>");
    if (!r.parsed) die(`SELF-TEST: the parser refused \`${src}\` — ${r.error}`);
    const got = r.sites.map((x) => `${x.door}|${x.cls}|${x.kind}|${x.url}`);
    if (got.join("\n") !== want.join("\n"))
      die(`SELF-TEST FAILED on \`${src}\`\n  want ${JSON.stringify(want)}\n  got  ${JSON.stringify(got)}\n` +
          `This file's classifier no longer does what its own numbers are read as meaning. Every figure ` +
          `below this point would be about a different question, so nothing is printed.`);
    for (const x of r.sites) { seenKind.add(x.kind); seenCls.add(x.cls); spoke++; }
    /* THE BRANCH COLUMN IS ARMED HERE AND NOWHERE ELSE. It reads 0 over the corpus, and a column that has
       never spoken cannot tell "the corpus has none" from "the mechanism is dead" — which is exactly the
       pair CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE exists to separate. */
    if (src.includes("c?P:")) {
      if (!(r.sites[0] && r.sites[0].alt === "/q"))
        die(`SELF-TEST FAILED: a conditional URL whose arms both fold did not record its second arm, so ` +
            `the branchAlt column is measuring nothing and its zero is unreadable.`);
    }
    if (SELFTEST_GUARDED.has(src) && !(r.sites[0] && r.sites[0].guard > 0))
      die(`SELF-TEST FAILED: a call under an \`if\` was recorded at guard depth 0, so the guard column is ` +
          `measuring nothing.`);
    if (src.startsWith(`const s=`) ) {
      /* THE BASE-RATE CHANNEL IS ARMED TOO. Its whole job is to be the thing a zero above is read against,
         so a base rate that silently stopped counting would leave every zero unreadable. */
      if (!r.pathish.has("/api/looks-like-an-endpoint"))
        die(`SELF-TEST FAILED: an address-shaped literal attached to no door was not counted as a base rate.`);
    }
  }
  const seenBlindKind = new Set(), seenBlindProp = new Set();
  let blindSpoke = 0;
  for (const [src, want] of SELFTEST_BLIND) {
    const r = readFile(src, "<selftest-blind>");
    if (!r.parsed) die(`SELF-TEST: the parser refused \`${src}\` — ${r.error}`);
    const got = r.blind.map((x) => `${x.prop}|${x.kind}|${x.url}`);
    if (got.join("\n") !== want.join("\n"))
      die(`SELF-TEST FAILED on the blind-spot channel for \`${src}\`\n  want ${JSON.stringify(want)}\n` +
          `  got  ${JSON.stringify(got)}\nThe number every PROGRAM-door zero is read against is measuring ` +
          `something other than what it is printed as meaning, so nothing is printed.`);
    if (want.length && r.sites.length)
      die(`SELF-TEST FAILED: \`${src}\` produced ${r.sites.length} SITE row(s). A blind-spot row must never ` +
          `enter the door totals — it is the size of what the doors cannot see, not a door.`);
    for (const x of r.blind) { seenBlindKind.add(x.kind); seenBlindProp.add(x.prop); blindSpoke++; }
  }
  for (const k of ["literal", "folded", "shape", "opaque"])
    if (!seenBlindKind.has(k))
      die(`SELF-TEST FAILED: no control exercises the ${k} kind of the blind-spot channel.`);
  for (const p of ["src", "href"])
    if (!seenBlindProp.has(p)) die(`SELF-TEST FAILED: no control exercises the ${p} blind-spot property.`);
  if (blindSpoke < 5) die(`SELF-TEST FAILED: only ${blindSpoke} blind-spot row(s) from the positive controls.`);
  /* THE xhr.open EXCLUSION IS A DECLARED FLOOR AND CARRIES A SIZE FOR THE SAME REASON. */
  if (readFile(`x.open(method,"/t")`, "<selftest>").xhrOpenSkippedNonLiteralMethod !== 1)
    die(`SELF-TEST FAILED: the xhr.open non-literal-method exclusion is not counted, so the floor this ` +
        `file's own DOORS comment says is "reported as xhrOpenSkippedNonLiteralMethod" is a sentence with ` +
        `no number behind it.`);
  for (const k of ["literal", "folded", "shape", "opaque"])
    if (!seenKind.has(k)) die(`SELF-TEST FAILED: no control exercises the ${k} kind, so its count is unarmed.`);
  for (const c of ["data", "program"])
    if (!seenCls.has(c)) die(`SELF-TEST FAILED: no control exercises the ${c} destination class.`);
  /* A CONTROL THAT NEVER SPOKE IS NOT A CONTROL. */
  if (spoke < 10) die(`SELF-TEST FAILED: only ${spoke} row(s) were produced by the positive controls.`);
  return { rows: SELFTEST.length, produced: spoke, blindRows: SELFTEST_BLIND.length, blindProduced: blindSpoke };
}

/* ── THE RUN ──────────────────────────────────────────────────────────────────────────────────────────── */

function main(argv) {
  const arg = (k, d) => { const i = argv.indexOf(k); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };
  const corpusDir = resolve(arg("--corpus", "engine/.work/sitecorpus/mirror"));
  const wantJson = argv.includes("--json");
  const onlySite = arg("--site", null);
  const nExamples = parseInt(arg("--examples", "0"), 10) || 0;

  const st = selftest();

  let stat = null;
  try { stat = statSync(corpusDir); } catch { /* handled below */ }
  if (!stat || !stat.isDirectory())
    die(`no corpus at ${corpusDir}. THIS REPOSITORY CARRIES NO COPY OF ANYBODY ELSE'S SITE — the driver is ` +
        `tracked and the corpus is not (testing/corpus/README.md), so there is nothing here to fall back ` +
        `on and this is not a defect. Make one, then re-run:\n` +
        `    NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs\n` +
        `    node testing/static_surface.mjs --corpus ${corpusDir}`);

  /* CALIBRATION FIRST. `corpusPrograms` publishes its own totals, so this probe reproduces them before any
     breakdown of them is believed — the disagreement is the finding rather than the breakdown. */
  const cp = corpusPrograms(corpusDir, TAG);

  /* SITE ATTRIBUTION BY CONTENT. The manifest's row carries the site id; the digest carries the file. A
     path rule here would be the second copy engine/corpus_programs.mjs refuses by name. */
  const manifestPath = resolve(corpusDir, "..", "provenance.json");
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
  const rows = Array.isArray(manifest) ? manifest : Object.values(manifest);
  const siteOf = new Map(), essOf = new Map();
  let fetchedFrom = null, fetchedTo = null;
  const note = (sha, id, ct, at) => {
    if (!sha) return;
    if (!siteOf.has(sha)) siteOf.set(sha, new Set());
    siteOf.get(sha).add(id);
    essOf.set(sha, essenceOf(ct));
    if (at) { if (!fetchedFrom || at < fetchedFrom) fetchedFrom = at; if (!fetchedTo || at > fetchedTo) fetchedTo = at; }
  };
  for (const r of rows) {
    note(r.sha256, r.id, r.contentType, r.fetchedAt);
    for (const s of r.resources || []) note(s.sha256, r.id, s.contentType, s.fetchedAt);
  }

  const sha = (b) => createHash("sha256").update(b).digest("hex");
  /* ONE TALLY SHAPE FOR BOTH CLASSES, so the two can only ever be printed the same way and a reader
     comparing them is comparing like with like. */
  const kindTally = () => ({ sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, guarded: 0, urls: new Set() });
  const perSite = new Map();
  const bucket = (id) => {
    if (!perSite.has(id)) perSite.set(id, {
      site: id, programs: 0, bytes: 0, parsed: 0, unparsed: 0, sites: 0,
      literal: 0, folded: 0, shape: 0, opaque: 0, guarded: 0, branchAlt: 0,
      engineDoorSites: 0, nonEngineDoorSites: 0, byDoor: {}, pathish: new Set(), urls: new Set(), rows: [],
      data: kindTally(), program: kindTally(),
      argShape: {}, bindBuckets: { "1": 0, "2-5": 0, "6-20": 0, "21-100": 0, "101+": 0, "0": 0 }, oneCharNames: 0,
      blind: { sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, src: 0, href: 0 }, blindUrls: new Set(), xhrOpenSkipped: 0,
    });
    return perSite.get(id);
  };

  let nProgramSeen = 0, nDocumentSeen = 0, ambiguous = 0, parseFail = [];
  const t0 = Date.now();

  for (const f of cp.files) {
    const buf = readFileSync(f);
    const d = sha(buf);
    const ess = essOf.get(d);
    if (DOCUMENT.has(ess)) { nDocumentSeen++; continue; }
    if (!PROGRAM.has(ess)) die(`${relative(corpusDir, f)} is neither program nor document by the manifest ` +
                               `(${JSON.stringify(ess)}) — corpusPrograms should have refused it first.`);
    nProgramSeen++;
    const ids = siteOf.get(d);
    if (!ids || ids.size === 0) die(`no manifest row names the site of ${relative(corpusDir, f)}`);
    if (ids.size > 1) ambiguous++;
    const id = [...ids].sort()[0];
    if (onlySite && id !== onlySite) continue;

    const b = bucket(id);
    b.programs++; b.bytes += buf.length;
    const r = readFile(buf.toString("utf8"), relative(corpusDir, f));
    if (!r.parsed) { b.unparsed++; parseFail.push(`${relative(corpusDir, f)}: ${r.error}`); continue; }
    b.parsed++;
    for (const v of r.pathish) b.pathish.add(v);
    b.xhrOpenSkipped += r.xhrOpenSkippedNonLiteralMethod;
    for (const s of r.blind) {
      b.blind.sites++; b.blind[s.kind]++; b.blind[s.prop]++;
      if (s.kind !== "opaque") b.blindUrls.add(s.url);
    }
    for (const s of r.sites) {
      b.sites++;
      b[s.kind]++;
      if (s.guard > 0) b.guarded++;
      if (s.alt) b.branchAlt++;
      if (s.engineDoor) b.engineDoorSites++; else b.nonEngineDoorSites++;
      b.byDoor[s.door] = (b.byDoor[s.door] || 0) + 1;
      const c = b[s.cls];
      c.sites++; c[s.kind]++; if (s.guard > 0) c.guarded++;
      /* THE CEILING COLUMNS ARE DATA-DOOR ONLY AND ONLY OVER ROWS THE FOLD DID NOT SETTLE, because that is
         the population the "a better parser would get these" objection is about; counting settled rows in
         it would answer a question nobody asked. */
      if (s.cls === "data" && s.kind !== "literal") {
        b.argShape[s.argShape] = (b.argShape[s.argShape] || 0) + 1;
        if (s.argBinds !== null) {
          const n = s.argBinds;
          const k = n === 0 ? "0" : n === 1 ? "1" : n <= 5 ? "2-5" : n <= 20 ? "6-20" : n <= 100 ? "21-100" : "101+";
          b.bindBuckets[k]++;
          if (s.argNameLen === 1) b.oneCharNames++;
        }
      }
      if (s.kind !== "opaque") { b.urls.add(s.method + " " + s.url); c.urls.add(s.method + " " + s.url); }
      if (nExamples) b.rows.push(s);
    }
  }
  const ms = Date.now() - t0;

  /* THE CALIBRATION IS ASSERTED AND NOT PRINTED-AND-HOPED-FOR. A probe that quietly walks a different
     population than the instrument it reproduces returns a plausible, larger, wrong number. */
  if (!onlySite && nProgramSeen !== cp.nProgram)
    die(`this probe typed ${nProgramSeen} file(s) as programs where corpusPrograms published ` +
        `${cp.nProgram}. The two selectors disagree about the population, which is the finding — do not ` +
        `read anything below this line.`);
  if (!onlySite && nDocumentSeen !== cp.nDocument)
    die(`this probe typed ${nDocumentSeen} document(s) where corpusPrograms published ${cp.nDocument}.`);

  const listedSites = new Set(rows.map((r) => r.id));
  const missingSites = [...listedSites].filter((id) => !perSite.has(id)).sort();

  const tot = {
    missingSites, corpusSites: listedSites.size,
    corpus: corpusDir, manifest: manifestPath,
    fetchedFrom, fetchedTo, readAt: new Date().toISOString(), ms,
    corpusProgramsSays: { onDisk: cp.onDisk, nProgram: cp.nProgram, nDocument: cp.nDocument, nExcluded: cp.nExcluded, bytes: cp.bytes },
    calibration: onlySite ? "SKIPPED (--site narrows the population)" : "REPRODUCED",
    siteCount: perSite.size, ambiguousBlobs: ambiguous,
    programs: 0, bytes: 0, parsed: 0, unparsed: 0,
    sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, guarded: 0, branchAlt: 0,
    engineDoorSites: 0, nonEngineDoorSites: 0, byDoor: {}, distinctUrls: 0, pathish: 0,
    argShape: {}, bindBuckets: { "1": 0, "2-5": 0, "6-20": 0, "21-100": 0, "101+": 0, "0": 0 }, oneCharNames: 0,
    blind: { sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, src: 0, href: 0 }, blindDistinctUrls: 0, xhrOpenSkipped: 0,
  };
  const allUrls = new Set(), allPathish = new Set(), allBlindUrls = new Set();
  const clsUrls = { data: new Set(), program: new Set() };
  tot.data = kindTally(); tot.program = kindTally();
  for (const b of perSite.values()) {
    for (const k of ["programs", "bytes", "parsed", "unparsed", "sites", "literal", "folded", "shape",
                     "opaque", "guarded", "branchAlt", "engineDoorSites", "nonEngineDoorSites"]) tot[k] += b[k];
    for (const [k, v] of Object.entries(b.byDoor)) tot.byDoor[k] = (tot.byDoor[k] || 0) + v;
    for (const [k, v] of Object.entries(b.argShape)) tot.argShape[k] = (tot.argShape[k] || 0) + v;
    for (const k of Object.keys(tot.bindBuckets)) tot.bindBuckets[k] += b.bindBuckets[k];
    tot.oneCharNames += b.oneCharNames;
    for (const cls of ["data", "program"]) {
      for (const k of ["sites", "literal", "folded", "shape", "opaque", "guarded"]) tot[cls][k] += b[cls][k];
      for (const u of b[cls].urls) clsUrls[cls].add(u);
    }
    for (const u of b.urls) allUrls.add(u);
    for (const p of b.pathish) allPathish.add(p);
    for (const k of Object.keys(tot.blind)) tot.blind[k] += b.blind[k];
    tot.xhrOpenSkipped += b.xhrOpenSkipped;
    for (const u of b.blindUrls) allBlindUrls.add(u);
  }
  tot.blindDistinctUrls = allBlindUrls.size;
  tot.distinctUrls = allUrls.size; tot.pathish = allPathish.size;
  tot.data.distinctUrls = clsUrls.data.size; tot.program.distinctUrls = clsUrls.program.size;
  delete tot.data.urls; delete tot.program.urls;

  /* THE PARTS SUM TO THE TOTAL, ASSERTED, because a count whose parts cannot be checked against it is a
     count a reader has to take on trust. */
  if (tot.literal + tot.folded + tot.shape + tot.opaque !== tot.sites)
    die(`the kind partition does not sum: ${tot.literal}+${tot.folded}+${tot.shape}+${tot.opaque} ` +
        `!= ${tot.sites}`);
  if (tot.engineDoorSites + tot.nonEngineDoorSites !== tot.sites)
    die(`the door partition does not sum against ${tot.sites}`);
  if (tot.data.sites + tot.program.sites !== tot.sites)
    die(`the destination partition does not sum: ${tot.data.sites}+${tot.program.sites} != ${tot.sites}`);
  for (const cls of ["data", "program"])
    if (tot[cls].literal + tot[cls].folded + tot[cls].shape + tot[cls].opaque !== tot[cls].sites)
      die(`the ${cls} kind partition does not sum against ${tot[cls].sites}`);
  if (tot.blind.literal + tot.blind.folded + tot.blind.shape + tot.blind.opaque !== tot.blind.sites)
    die(`the blind-spot kind partition does not sum against ${tot.blind.sites}`);
  if (tot.blind.src + tot.blind.href !== tot.blind.sites)
    die(`the blind-spot property partition does not sum against ${tot.blind.sites}`);

  const out = {
    total: tot,
    perSite: [...perSite.values()].sort((a, b) => b.data.sites - a.data.sites || b.sites - a.sites).map((b) => ({
      site: b.site, programs: b.programs, bytes: b.bytes, parsed: b.parsed, unparsed: b.unparsed,
      sites: b.sites, literal: b.literal, folded: b.folded, shape: b.shape, opaque: b.opaque,
      guarded: b.guarded, branchAlt: b.branchAlt, engineDoorSites: b.engineDoorSites,
      nonEngineDoorSites: b.nonEngineDoorSites, byDoor: b.byDoor,
      distinctUrls: b.urls.size, pathish: b.pathish.size,
      argShape: b.argShape, bindBuckets: b.bindBuckets, oneCharNames: b.oneCharNames,
      data: { ...b.data, urls: undefined, distinctUrls: b.data.urls.size },
      program: { ...b.program, urls: undefined, distinctUrls: b.program.urls.size },
      blind: { ...b.blind, distinctUrls: b.blindUrls.size }, xhrOpenSkipped: b.xhrOpenSkipped,
    })),
    parseFailures: parseFail,
    examples: nExamples ? [...perSite.values()].flatMap((b) => b.rows.slice(0, nExamples)) : undefined,
  };

  if (wantJson) { console.log(JSON.stringify(out, null, 1)); return; }

  const pct = (n, d) => (d ? (100 * n / d).toFixed(1) : "0.0") + "%";
  console.log(`# static_surface — WHAT A PARSE RECOVERS FROM THE JS DOOR. A CONTROL, NEVER A TARGET.`);
  console.log(`selftest ARMED: ${st.rows} controls produced ${st.produced} classified row(s); every kind and ` +
              `both destination classes exercised`);
  console.log(`         plus ${st.blindRows} blind-spot controls producing ${st.blindProduced} row(s), each ` +
              `asserted to enter NO door total`);
  console.log(`corpus   ${corpusDir}`);
  console.log(`fetched  ${fetchedFrom} .. ${fetchedTo}   read ${tot.readAt}   parse ${ms} ms`);
  console.log(`corpusPrograms: ${cp.onDisk} on disk = ${cp.nProgram} program + ${cp.nDocument} document + ` +
              `${cp.nExcluded} excluded, ${(cp.bytes / 1048576).toFixed(1)} MB  [calibration ${tot.calibration}]`);
  console.log(`parsed   ${tot.parsed}/${tot.programs} programs (${tot.unparsed} refused), ` +
              `${(tot.bytes / 1048576).toFixed(1)} MB over ${tot.siteCount} site(s)`);
  if (tot.missingSites.length)
    console.log(`no program in the corpus for ${tot.missingSites.length} of ${tot.corpusSites} listed site(s): ` +
                `${tot.missingSites.join(", ")} — a fact about the FETCH, not about the parse`);
  if (tot.ambiguousBlobs)
    console.log(`${tot.ambiguousBlobs} blob(s) are shared by more than one site and are attributed to one of them`);
  const block = (name, k, why) => {
    console.log(``);
    console.log(`${name} — ${why}`);
    console.log(`  sites ${k.sites}   distinct addresses ${k.distinctUrls}`);
    console.log(`  literal ${k.literal} (${pct(k.literal, k.sites)})   a string in the text; the parse and the engine both have it`);
    console.log(`  folded  ${k.folded} (${pct(k.folded, k.sites)})   resolved without running anything; the parse has it too`);
    console.log(`  shape   ${k.shape} (${pct(k.shape, k.sites)})   literal text + a hole; the parse has the SHAPE and never the VALUE`);
    console.log(`  opaque  ${k.opaque} (${pct(k.opaque, k.sites)})   no literal text at all; only "a request happens here"`);
    console.log(`  --> complete address from the text at ${pct(k.literal + k.folded, k.sites)}; ` +
                `${k.shape + k.opaque} site(s) (${pct(k.shape + k.opaque, k.sites)}) need a VALUE only a run has.`);
    console.log(`  guarded ${k.guarded} (${pct(k.guarded, k.sites)}) under >=1 test — read by the parse whether the gate is taken or not`);
  };
  block(`DATA DOOR (fetch / XMLHttpRequest / sendBeacon / WebSocket / EventSource)`, tot.data,
        `Fetch §2.2.5 destinations whose reply becomes a VALUE. THIS IS THE @H PRODUCT SURFACE.`);
  block(`PROGRAM DOOR (import() / Worker / SharedWorker / importScripts)`, tot.program,
        `replies that become a PROGRAM — the page loading itself. Reported apart and never summed in.`);
  console.log(``);
  console.log(`THE DOOR SET'S OWN BLIND SPOT, MEASURED — \`el.src =\` / \`el.href =\`, which html_script.c and`);
  console.log(`  html_link.c DO record and no door above reads. NOT sites and summed into no total; this is the`);
  console.log(`  number a PROGRAM-door or DATA-door ZERO has to be read against, because a bundler that loads`);
  console.log(`  its chunks by injecting a <script> passes through here and through no door at all.`);
  console.log(`  assignments ${tot.blind.sites}   (.src ${tot.blind.src}  .href ${tot.blind.href})   ` +
              `distinct addresses ${tot.blindDistinctUrls}`);
  console.log(`  literal ${tot.blind.literal} (${pct(tot.blind.literal, tot.blind.sites)})   ` +
              `folded ${tot.blind.folded} (${pct(tot.blind.folded, tot.blind.sites)})   ` +
              `shape ${tot.blind.shape} (${pct(tot.blind.shape, tot.blind.sites)})   ` +
              `opaque ${tot.blind.opaque} (${pct(tot.blind.opaque, tot.blind.sites)})`);
  console.log(`  ${tot.xhrOpenSkipped} further xhr.open call(s) were skipped for a non-literal first argument —`);
  console.log(`  the floor the DOORS comment names, carrying a size rather than a sentence.`);
  console.log(``);
  console.log(`BOTH CLASSES ${tot.sites} sites, ${tot.distinctUrls} distinct addresses — printed last and`);
  console.log(`  never first, because ${pct(tot.program.sites, tot.sites)} of it is chunk loading.`);
  console.log(`  literal ${tot.literal}  folded ${tot.folded}  shape ${tot.shape}  opaque ${tot.opaque}  guarded ${tot.guarded}`);
  console.log(`  branchAlt ${tot.branchAlt}  conditional URLs where the text carries BOTH arms and one run takes one`);
  console.log(`  doors the engine's endpoint_record records: ${tot.engineDoorSites}; doors it does not: ${tot.nonEngineDoorSites}`);
  console.log(`  by door: ${Object.entries(tot.byDoor).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join("  ") || "(none)"}`);
  console.log(``);
  const unsettled = tot.data.sites - tot.data.literal;
  const idents = Object.values(tot.bindBuckets).reduce((a, x) => a + x, 0);
  console.log(`COULD A STRONGER PARSER HAVE DONE BETTER? — asked of the ${unsettled} DATA-door site(s) whose`);
  console.log(`  URL is not one string literal. This file's fold is conservative ON PURPOSE, so its`);
  console.log(`  opaque count is a FLOOR for the parse; these two rows bound how much of a floor.`);
  console.log(`  argument shape: ${Object.entries(tot.argShape).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join("  ")}`);
  console.log(`  of the ${idents} whose URL is a bare NAME, times that name is bound in its own file:`);
  for (const [k, v] of Object.entries(tot.bindBuckets))
    if (v) console.log(`    ${String(k).padStart(7)} : ${String(v).padStart(4)}  ${k === "1" ? "<- any parser resolves this; THIS FILE ALREADY DOES" : k === "101+" ? "<- a minifier's reused register; no name-based resolution can touch it" : ""}`);
  console.log(`  ${tot.oneCharNames} of ${idents} of those names are ONE CHARACTER long.`);
  console.log(``);
  console.log(`BASE RATE (not endpoints, never quote as such): ${tot.pathish} distinct address-shaped string`);
  console.log(`  literals in the same programs that are attached to NO door. A naive extractor reports these;`);
  console.log(`  this one does not. A zero above would have to be read against this number.`);
  console.log(``);
  console.log(``);
  console.log(`PER SITE — the DATA door only; the program door is in --json.`);
  console.log(`site             programs   data  literal folded  shape opaque guarded   urls  pathish`);
  for (const s of out.perSite)
    console.log(`  ${s.site.padEnd(14)} ${String(s.programs).padStart(8)} ${String(s.data.sites).padStart(6)} ` +
                `${String(s.data.literal).padStart(8)} ${String(s.data.folded).padStart(6)} ${String(s.data.shape).padStart(6)} ` +
                `${String(s.data.opaque).padStart(6)} ${String(s.data.guarded).padStart(7)} ` +
                `${String(s.data.distinctUrls).padStart(6)} ${String(s.pathish).padStart(8)}`);
  console.log(``);
  /* PER SITE, THE PROGRAM DOOR BESIDE THE BLIND SPOT, WHICH IS THE ONLY PLACE THE TWO CAN BE READ TOGETHER.
     A site whose program door reads 0 has either shipped no chunk loader or loaded its chunks through the
     column to its right, and a table printing one without the other cannot tell those apart. MEASURED and
     the reason this block exists: the program door is BIMODAL BY BUNDLER — a bundle that emits native
     `import()` scores in the door, and one whose bundler compiled `import()` away into a chunk-id map plus
     a `<script>` injection scores ZERO there and scores here instead. Neither zero is a statement about how
     much the site loads. */
  console.log(`PER SITE — the PROGRAM door against the blind spot it can be lost in.`);
  console.log(`site             programs  prog  p.lit p.fold p.urls | blind  b.lit b.fold b.shape b.opaq  b.urls`);
  for (const s of [...out.perSite].sort((a, b) => b.blind.sites - a.blind.sites || b.program.sites - a.program.sites))
    console.log(`  ${s.site.padEnd(14)} ${String(s.programs).padStart(8)} ${String(s.program.sites).padStart(5)} ` +
                `${String(s.program.literal).padStart(6)} ${String(s.program.folded).padStart(6)} ` +
                `${String(s.program.distinctUrls).padStart(6)} | ${String(s.blind.sites).padStart(5)} ` +
                `${String(s.blind.literal).padStart(6)} ${String(s.blind.folded).padStart(6)} ` +
                `${String(s.blind.shape).padStart(7)} ${String(s.blind.opaque).padStart(6)} ` +
                `${String(s.blind.distinctUrls).padStart(7)}`);
  if (parseFail.length) { console.log(``); console.log(`PARSE REFUSED (${parseFail.length}):`); for (const p of parseFail.slice(0, 10)) console.log(`  ${p}`); }
  if (nExamples) {
    console.log(``); console.log(`EXAMPLES:`);
    for (const r of out.examples) console.log(`  [${r.kind}/${r.door}/g${r.guard}] ${r.method} ${r.url.slice(0, 120)}${r.alt ? `   (alt ${r.alt.slice(0, 60)})` : ""}   ${r.file}:${r.line}`);
  }
}

main(process.argv.slice(2));
