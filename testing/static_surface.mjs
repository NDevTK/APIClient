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
 * AND A SECOND AXIS, BECAUSE THE FOUR KINDS ABOVE ARE ABOUT THE ADDRESS AND SAY NOTHING ABOUT WHETHER A RUN
 * EVER REACHES THE CALL. The kinds answer "could a parse have had this value"; the REACH band answers "what
 * has to be CALLED for this line to run", which is the only axis on which a door a run rings and a door it
 * does not come apart. A parse reads a call whether or not anything invokes its enclosing function, so this
 * band costs the parse nothing and is not a concession — it is what makes a door's ZERO IN A RUN readable,
 * which no column here could do before. Three arms, an exact partition of each class's own `sites`:
 *   TOP-LEVEL   function depth 0 — the module or script body performs the call, so evaluating the program
 *               reaches it and nothing else has to happen.
 *   ASYNC FN    the INNERMOST enclosing function is `async`. It runs only once something invokes that body,
 *               and in a real application the invoker is an effect flushed after a render commit, an event
 *               handler, a `setTimeout` or an idle callback — none of which is program evaluation.
 *   SYNC FN     the innermost enclosing function is an ordinary one. It also needs an invoker.
 * THE PARTITION IS ASSERTED against `sites` for both classes, so no row can fall out of all three and make a
 * class read as having fewer async sites than it has — the flattering direction for the DATA door, whose zero
 * in a run is the thing this band exists to make readable.
 * IT IS A FLOOR IN ONE DIRECTION ONLY AND THE ASYMMETRY IS THE WHOLE OF HOW TO READ IT. `innerSync`
 * OVER-states reachability, because a sync function nothing calls is exactly as unreached as an async one;
 * `innerAsync` cannot over-state it, because an async body needs an invoker by construction. So a high
 * `innerAsync` share IS evidence a class needs an invoker, and a high `innerSync` share is NOT evidence that
 * it does not. Reading the second as a clean bill is the one reading this band must not be used for.
 * WHAT IT DOES NOT ANSWER, STATED HERE BECAUSE THE BAND IS ONE HOP SHORT OF THE QUESTION A READER WANTS: it
 * says a call needs an invoker and never WHETHER THAT INVOKER IS ITSELF REACHED. A sync function called from
 * top level is reached and a sync function called only from an async one is not, and both land in `innerSync`.
 * The call graph is what separates them, this file builds none, and a bound-once fold is not one — so the
 * band is a NECESSARY-CONDITION reading and never a sufficient one. HOW ITS ABSENCE WOULD SHOW: a corpus
 * whose DATA door is almost all `innerSync` would read as needing no invoker while every one of those
 * functions sat behind an async caller. WHAT THE NEXT DIFF BUILDS: a reachability closure over the call graph
 * this file can already name — the bound-once function declarations it folds through — so `innerSync` splits
 * into "called from a body the program evaluates" and "called only from somewhere that itself needs an
 * invoker", which is the same fold already built for the chunk manifest pointed at callers instead of at
 * addresses.
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
 *     and precision spent. What recovers those addresses is interprocedural folding through the chunk-URL
 *     function, which is a different subproblem and is the CHUNK MANIFEST channel — built, and reported as
 *     its own band rather than as a door, for the same reason this exclusion stands.
 *   - a library wrapper (`axios.get`, `$.ajax`, an SDK `request()`), for the reason the DOORS table gives.
 *   - anything a bundle reaches through a member call this file cannot name, which is unbounded and is why
 *     the site count here is stated as a floor everywhere it is stated at all.
 *
 * THE CHUNK-MANIFEST RESIDUAL IS BUILT AND WHAT REPLACES IT IS NARROWER AND NAMES A DIFFERENT MECHANISM.
 * It asked for a fold that resolves a PROPERTY assigned exactly once in the file, inlines a single-parameter
 * function, and ENUMERATES a ternary chain or a computed member into the set of addresses it can return.
 * That is what the CHUNK MANIFEST channel is, it is keyed on the expression and on no runtime's name, and
 * the two forms it was written from both answer: a ternary chain over a public-path literal, and two object
 * literals joined at one key. It found a PRECISION half its own clause did not anticipate and that is worth
 * keeping, because the clause would otherwise be read as finished: the shape it describes is also the shape
 * of an i18n table, an enum and a label map, so before an address test was added the channel reported
 * `session`, `Users` and `0.001` as addresses and its figure was roughly double.
 * WHAT IS NOT COVERED NOW: a runtime whose chunk-URL FUNCTION or public-path OBJECT is named by an
 * identifier the FILE binds more than once. The bound-once discipline refuses those by design and is right
 * to, because without a scope graph a name bound twice could be folded across a shadow and a wrong fold
 * INVENTS an address. But the second binding is very often in a NESTED function that encloses neither the
 * write nor the read, so the refusal is stricter than the hazard: measured on one webpack-4 runtime, the
 * object and the chunk-URL function are each bound twice — once as a declaration at the runtime's own scope
 * and once as a `var` inside a nested function — and both uses sit at the outer scope, where a scope-correct
 * reader would settle them. WHAT THE NEXT DIFF BUILDS: one scope pre-pass producing the BINDER of every
 * Identifier by node identity, so the slot map and the function-declaration lookup key on (binder, name)
 * rather than on name. Node identity is what makes it cheap — the fold already receives the AST node, so
 * nothing has to be threaded through it — and it is strictly BOTH more precise and wider than the file-wide
 * count it replaces. HOW ITS ABSENCE WOULD SHOW: a site whose PROGRAM-door row reads 0, whose blind-spot
 * OPAQUE count beside it is nonzero, AND whose manifest column reads 0 — three columns the per-site PROGRAM
 * block prints adjacent for exactly this reading. Two sites on the corpus this ran against read that way
 * for the reason just measured; two more read that way and their cause is NOT established, which is stated
 * rather than guessed at.
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

import { readFileSync, statSync, readdirSync } from "node:fs";
import { createHash } from "node:crypto";
import { relative, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
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
   THE GLOBAL OBJECT IS NOT A RECEIVER IN THAT SENTENCE'S SENSE, AND READING IT AS ONE COST THIS FILE
   FOURTEEN SITES. THE MATCH IS ON THE PLATFORM NAME AND NEVER ON A RECEIVER is right and it is about a
   receiver that CARRIES INFORMATION —
   `api.fetch`, `this.fetch`, `mk().fetch`, where the name to the left is a minifier's local and says
   nothing about what is being asked. A reference to the GLOBAL OBJECT is the opposite of that: `fetch(u)`
   and `window.fetch(u)` are ONE platform name reached two ways, and what stands to the left is a spelling
   of the global scope rather than an object whose identity is in question. So a member call whose object is
   PROVABLY the global object is looked up in the `callee-global` and `new` rows exactly as if the object had
   not been written, and the same holds for `new self.Worker`.
   PROVABLY IS THE LOAD-BEARING WORD AND IT IS THE SAME DISCIPLINE `collectBinds` ALREADY APPLIES TO NAMES.
   A name bound ZERO times in the whole file and assigned never cannot be anything but the global; a file
   that BINDS it can mean something else by it, and the UMD wrapper `(function(window){ ... })(window)` and
   the transpiler idiom `var self = this` both do. Such a site is REFUSED and the refusal carries a size
   rather than a sentence, which is what `xhrOpenSkippedNonLiteralMethod` already does for the xhr.open
   method exclusion. The
   bound-once fold is sound because a name bound once cannot be shadowed; this is that argument one step
   weaker in its premise and therefore one step stronger in its conclusion.
   THE THREE NUMBERS THAT PRICE THIS ARE PRINTED ON EVERY RUN AND NONE OF THEM IS ASSERTED HERE, because a
   widening that buys recall is only honest beside what it declines, and a figure over a corpus this
   repository does not carry cannot be checked by a reader who re-derives it. `globalDoor` reports what was
   ADMITTED through the global spelling, what was REFUSED for a bound global name, and how many member calls
   and constructions naming a platform door were DECLINED for a receiver that is not the global object at
   all — which is the library-wrapper population WHAT IS NOT HERE AND WHY turns away, counted instead of
   described. A widening whose precision cost is unmeasured is a trade nobody made.
   THE DIRECTION THIS WIDENS IN IS THE ONE THAT COSTS THIS PROJECT, WHICH IS THE WHOLE REASON IT BELONGS IN
   A CONTROL. A recall hole here makes the BASELINE look weak, a weak baseline makes the engine look strong,
   and this file's own opening says that is the one result it must not manufacture. So a site the parse can
   reach and this file was missing is a defect in this file however small the count, and the count moving an
   existing total is a fact to report as MOVED rather than a reason to leave the hole open.
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
/* THE GLOBAL OBJECT'S OWN NAMES. `global` is node's and is here because a bundle ships one build for both. */
const GLOBAL_OBJECTS = new Set(["window", "self", "globalThis", "global"]);
/* WHERE THE ENGINE IS, so a name this file measures is read out of the declaration that owns it. */
const ENGINE_DIR = resolve(dirname(fileURLToPath(import.meta.url)), "..", "engine");

/* ── THE DECLARED ENTRY NAMES, AND WHY THIS FILE IS WHERE THEY GET PRICED ─────────────────────────────────
   Two components in the engine raise a census row when THE COMPILER resolves a free identifier against the
   global object, and both record the same named residual at their own declaration: the row sees ONE
   SPELLING, the bare identifier, so a program that reaches the same platform name through a property of the
   global object raises nothing. Both name the same next diff, a member-name channel at the field-get
   emitter, and both say the floor is in the direction that WITHHOLDS a finding.
   THE QUESTION THAT DIFF HAS TO BE PRICED AGAINST IS A PROPERTY OF REAL BUNDLES AND NOT OF THE ENGINE, which
   is why it is answered here and not there. Those rows are read as a BIT — zero against nonzero — and their
   own header says so in as many words, so the floor costs a READING only where a bundle spells a name
   EXCLUSIVELY as a property. A program that writes `window.requestAnimationFrame` once and the bare name
   anywhere else still raises the row, and for a bit that is the whole of what is asked of it. So the
   decisive column below is not how often the property spelling occurs; it is HOW MANY SITES SPELL A NAME
   ONLY THAT WAY, because that is the only population on which the landed denominator answers zero about a
   program that does hang work off the rung.
   THE NAMES ARE DERIVED FROM THE ENGINE AND NEVER TYPED HERE, for the reason the DOORS table gives at
   length: a list of platform names in this file would be a second copy of a fact the engine's own
   declarations already state, and the copy anyone writes first is the one that drops a name. A dropped name
   is measured as a smaller population, which is the flattering direction for the diff being priced. Each
   rung declares a NULL-terminated table at its per-realm install and each request edge declares its entry
   name as a string, so both are read out of the sources that own them and every unresolved element THROWS.
   THERE IS NO EXPECTED COUNT ASSERTED, because a count would be a bound that goes stale on the day a fourth
   rung lands: what is asserted instead is that every declaration site found resolved to at least one name,
   which grows with the tree and still fails loudly on a shape change. */
/* ONE SOURCE'S DECLARATIONS, AS A PURE FUNCTION OF ITS TEXT, SO EVERY REFUSAL BELOW CAN BE SHOWN FIRING.
   The walk that finds the files cannot be armed without a second engine tree to break; this can be armed with
   a string, and the refusals are the whole reason the population may be trusted — a shape change that went
   through quietly would report fewer declared names, and fewer names is a smaller population, which is the
   flattering direction for the diff this band exists to price. It THROWS rather than returning a short answer
   for exactly that reason. */
function entryNamesFromSource(src, where, add) {
  let rung = 0, edge = 0;
  for (const m of src.matchAll(/\brung_entry_declare\s*\(\s*([A-Z][A-Z_0-9]*)\s*,\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)/g)) {
    rung++;
    const unit = m[1], table = m[2];
    const t = new RegExp(`static\\s+const\\s+char\\s*\\*\\s*const\\s+${table}\\s*\\[\\s*\\]\\s*=\\s*\\{([^}]*)\\}`).exec(src);
    if (!t) die(`${table} is declared to a rung at ${where} and this pass cannot find its table, so the ` +
                `names that rung counts would go unmeasured and the population would read smaller than it is.`);
    let got = 0;
    for (const raw of t[1].split(",").map((s) => s.trim())) {
      if (!raw || raw === "NULL") continue;
      const q = /^"(.*)"$/.exec(raw);
      if (q) { add(q[1], unit); got++; continue; }
      const lit = new RegExp(`static\\s+const\\s+char\\s+${raw}\\s*\\[\\s*\\]\\s*=\\s*"([^"]*)"`).exec(src);
      if (!lit) die(`${table}'s element ${raw} at ${where} resolves to no string literal in its own file, so ` +
                    `one declared name would be silently missing from this channel.`);
      add(lit[1], unit); got++;
    }
    if (!got) die(`${table} at ${where} resolved to no names at all.`);
  }
  for (const m of src.matchAll(/\bendpoint_([a-z_]+)_edge_declare\s*\(\s*"([^"]+)"/g)) {
    edge++; add(m[2], "edge:" + m[1]);
  }
  return { rung, edge };
}
function declaredEntryNames() {
  const host = resolve(ENGINE_DIR, "host");
  const files = [];
  const walkDir = (d) => {
    let names;
    try { names = readdirSync(d); } catch { return; }
    for (const e of names) {
      const p = resolve(d, e);
      let st; try { st = statSync(p); } catch { continue; }
      if (st.isDirectory()) walkDir(p);
      else if (/\.c$/.test(e)) files.push(p);
    }
  };
  walkDir(host);
  if (!files.length) die(`no engine source under ${host}, so the declared entry names cannot be derived and ` +
                         `a hand-typed list is the one thing this channel may not fall back to.`);
  const out = new Map();          // name -> the declaration(s) that named it
  const add = (n, who) => { if (!out.has(n)) out.set(n, []); out.get(n).push(who); };
  let rungSites = 0, edgeSites = 0, resolvedRung = 0, resolvedEdge = 0;
  for (const f of files) {
    const n = entryNamesFromSource(readFileSync(f, "utf8"), relative(ENGINE_DIR, f), add);
    rungSites += n.rung; edgeSites += n.edge; resolvedRung += n.rung; resolvedEdge += n.edge;
  }
  if (!rungSites || !edgeSites)
    die(`the declared entry names were derived from ${rungSites} rung declaration(s) and ${edgeSites} edge ` +
        `declaration(s); a zero on either side means this pass stopped matching a shape the engine still ` +
        `uses, and a channel measuring none of a population reports the smallest possible floor.`);
  if (resolvedRung !== rungSites || resolvedEdge !== edgeSites)
    die(`only ${resolvedRung}/${rungSites} rung and ${resolvedEdge}/${edgeSites} edge declaration(s) resolved.`);
  return out;
}
const ENTRY_DECL = declaredEntryNames();
const ENTRY_NAMES = new Set(ENTRY_DECL.keys());
/* THE SPELLINGS, EACH A PARTITION MEMBER EXCEPT THE LAST. `bareFree` is the one the landed rows already see;
   `bareBoundName` is a bare reference in a file that also binds the name somewhere, which the engine's
   scope-correct resolver very probably DOES see and this file-wide pass cannot prove, so it is counted apart
   rather than folded into either answer. `typeofBare` is not a partition member and is not summed: it says
   which of the two reads the landed row would have recorded, and every one of them is already inside
   `bareFree` or `bareBoundName`. */
const SPELLINGS = ["bareFree", "bareBoundName", "qualified", "qualifiedBoundGlobal", "computedLiteral",
                   "instanceMember", "instanceMemberComputed", "destructuredFromGlobal"];
const spellTally = () => {
  const t = {};
  for (const k of SPELLINGS) t[k] = 0;
  t.typeofBare = 0;
  return t;
};
/* A PROPERTY NAME MAY NOT BE BOTH A `member-call` DOOR AND A GLOBAL-REACHED ONE, ASSERTED RATHER THAN
   BELIEVED. `window.fetch` is resolved by looking the PROPERTY up among the `callee-global` rows, so a door
   added later that names `fetch` or `importScripts` as a `member-call` would make one site match two rows
   and the order of two `if`s would decide which — the kind of silent double-count no total can reveal. The
   two families are disjoint today (`open`/`sendBeacon` against `fetch`/`importScripts`) and this is what
   keeps them so. */
for (const d of DOORS) {
  if (d.kind !== "member-call") continue;
  if (DOOR_BY_NAME.has("callee-global:" + d.name) || DOOR_BY_NAME.has("new:" + d.name))
    die(`DOORS names ${d.name} as a member-call AND as a global-reachable door, so a call through the ` +
        `global object would match two rows and be counted under whichever is tested first.`);
}

/* ── FOLDING ──────────────────────────────────────────────────────────────────────────────────────────────
   Returns { text, holes } where `text` is the literal bytes recovered with each unresolved subexpression
   rendered as `{n}` — the SAME rendering `solver/endpoint.c` uses for a concolic hole, so a static row and
   an engine row are comparable strings rather than two notations for one address — and `holes` counts them.
   `branches` collects the alternative arm of every conditional that folds both ways, which is a place a
   PARSE beats a RUN: execution takes one arm unless it forks, and the text carries both. */
const MAX_DEPTH = 24;

/* ── THE ENUMERATING FOLD: ONE FOLDER, A SECOND MODE, AND NO SECOND COPY ──────────────────────────────────
   `fold` takes an optional `env`. With `env` NULL it is the folder every existing row is classified by and
   not one of its answers moves — that is asserted rather than hoped for, by an A/B whose door totals and
   whose judged population are byte-identical across this diff. With an `env` it may additionally resolve a
   PARAMETER to a candidate value, decide an equality, index an object literal with a literal key, take the
   right arm of `||` past a missing key, and INLINE a single-parameter function. Two folders would have been
   free to disagree about what a `+` does; one folder with a mode cannot.
   WHY A MODE AND NOT A WIDER DEFAULT: an equality this file can decide is a fold the ordinary channel could
   legitimately have too, and enabling it there would MOVE numbers other lanes are pricing against. The mode
   keeps the chunk-manifest channel additive to every total that already exists, which is what makes its own
   number readable on the run it lands in.
   A MISS IS NOT AN EMPTY STRING AND THE DIFFERENCE IS THE WHOLE SOUNDNESS OF THE ENUMERATION. Indexing an
   object literal with a key it does not carry yields `undefined` in the language, and a bundler's chunk-URL
   function relies on exactly that — `({names}[id] || id)` falls through to the id. Folding a miss to `""`
   would silently INVENT an address with a segment deleted from it, so a miss carries a HOLE as well as its
   marker: read past `||` it disappears, and read anywhere else it drops the candidate. */
const MISS = () => ({ text: "{?}", holes: 1, miss: true });

/* ── WHAT AN ADDRESS LOOKS LIKE, IN ONE PLACE ─────────────────────────────────────────────────────────────
   The base rate and the chunk manifest both have to decide whether a recovered string is an address, and two
   copies of that decision would be free to drift into reporting one population under two definitions. The
   first two alternatives are the base rate's own, unchanged, so its number cannot move by this being
   factored out; the third is added for the manifest, because a bundler that emits `./chunk.HASH.js` has
   written an address and the base rate never had to read one.
   A FRAGMENT IS REFUSED AND THAT IS THE POINT RATHER THAN A LIMITATION. A composition that recovers a chunk
   NAME without the public path in front of it — `grafana.geomapPanel.HASH.css`, `chunk.123.js` — has
   recovered part of an address, and emitting a part as a whole would be this channel INVENTING one. Such a
   fold is counted as a FRAGMENT and reported apart: the count says a manifest is present and that the
   composition this file reached did not include its public path, which is a floor stated with a size.
   IT IS ALSO THE PRECISION HALF OF THE MANIFEST CHANNEL AND IT WAS MEASURED, NOT ASSUMED. Without it the
   channel reported `session`, `Users`, `usdc-usdt-n` and `0.001` as addresses — a one-parameter function
   indexing a string table is an i18n table, an enum or a label map at least as often as it is a chunk
   manifest, and nothing about the SHAPE of the code tells them apart. What tells them apart is what comes
   out, which is the only axis that does not require knowing whose runtime wrote it. Ground truth for the
   direction: of the addresses recovered at one site, fifteen name files the fetcher independently mirrored. */
const looksLikeAddress = (v) => /^https?:\/\/[^\s]+$/.test(v) ||
                                /^\/[A-Za-z0-9_][^\s"'<>]*$/.test(v) ||
                                /^\.{1,2}\/[^\s"'<>]+$/.test(v);

function fold(node, binds, depth, env) {
  if (node == null) return { text: "{?}", holes: 1 };
  if (depth > MAX_DEPTH) return { text: "{?}", holes: 1 };
  if (env) {
    const r = foldEnvOnly(node, binds, depth, env);
    if (r) return r;
  }
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
          const r = fold(node.expressions[i], binds, depth + 1, env);
          t += r.holes ? r.text : r.text;
          h += r.holes;
        }
      }
      return { text: t, holes: h };
    }
    case "BinaryExpression": {
      if (node.operator !== "+") return { text: "{?}", holes: 1 };
      const a = fold(node.left, binds, depth + 1, env), b = fold(node.right, binds, depth + 1, env);
      return { text: a.text + b.text, holes: a.holes + b.holes };
    }
    case "Identifier": {
      const b = binds.get(node.name);
      if (b && b.node) return fold(b.node, binds, depth + 1, env);
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
        if (k === node.property.name) return fold(p.value, binds, depth + 1, env);
      }
      return { text: "{?}", holes: 1 };
    }
    case "ConditionalExpression": {
      const a = fold(node.consequent, binds, depth + 1, env), b = fold(node.alternate, binds, depth + 1, env);
      if (a.holes === 0 && b.holes === 0) return { text: a.text, holes: 0, alt: b.text };
      return { text: "{?}", holes: 1 };
    }
    case "TSAsExpression":
    case "TSNonNullExpression":
    case "ParenthesizedExpression":
      return fold(node.expression, binds, depth + 1, env);
    default:
      return { text: "{?}", holes: 1 };
  }
}

/* `obj.prop` AS ONE KEY, so a write and a read of the same slot are the same string and cannot drift. */
function memberKey(node) {
  if (!node || (node.type !== "MemberExpression" && node.type !== "OptionalMemberExpression")) return null;
  if (node.computed || node.object.type !== "Identifier" || node.property.type !== "Identifier") return null;
  return node.object.name + "." + node.property.name;
}

/* AN OBJECT LITERAL, HOWEVER THE FILE SPELLS THE WAY TO IT: written inline, which is what a minified chunk
   table is; bound once to a name; or assigned once to a property. */
function objectLiteralOf(node, binds, env) {
  if (!node) return null;
  if (node.type === "ObjectExpression") return node;
  if (node.type === "Identifier") {
    const b = binds.get(node.name);
    return b && b.node && b.node.type === "ObjectExpression" ? b.node : null;
  }
  if (env) {
    const m = env.memberOnce.get(memberKey(node));
    if (m && m.type === "ObjectExpression") return m;
  }
  return null;
}

/* A SINGLE-PARAMETER FUNCTION WHOSE WHOLE BODY IS ONE RETURNED EXPRESSION, which is the only shape that can
   be inlined without reasoning about statements. A function with more than one statement is REFUSED rather
   than approximated by its last return: the statements before it may narrow the parameter, and a fold that
   ignored them would enumerate addresses the function cannot actually return. */
function singleParamFn(node, binds, env) {
  let fn = null;
  if (!node) return null;
  if (node.type === "Identifier") {
    const b = binds.get(node.name);
    if (b && b.node) fn = b.node;
    /* A FUNCTION DECLARATION IS ONE FUNCTION ONLY IF ITS NAME IS BOUND ONCE, and `binds` cannot answer for
       it: a declaration has no initializer, so `collectBinds` files it under the same "not foldable" set as
       a parameter. Minified code reuses one letter for a dozen declarations and a dozen parameters, so
       taking whichever declaration was recorded last would inline a DIFFERENT function and enumerate
       addresses no call site can produce. The count is the same test `binds` makes, asked separately. */
    else if (env && env.fnDecl.has(node.name) &&
             env.count.get(node.name) === 1 && !env.reassigned.has(node.name)) fn = env.fnDecl.get(node.name);
  } else if (env) {
    const m = env.memberOnce.get(memberKey(node));
    if (m) fn = m;
  }
  if (!fn) return null;
  if (fn.type !== "FunctionDeclaration" && fn.type !== "FunctionExpression" &&
      fn.type !== "ArrowFunctionExpression") return null;
  if (!fn.params || fn.params.length !== 1 || fn.params[0].type !== "Identifier") return null;
  if (fn.body.type === "BlockStatement") {
    if (fn.body.body.length !== 1) return null;
    const st = fn.body.body[0];
    if (st.type !== "ReturnStatement" || !st.argument) return null;
  }
  return fn;
}

/* EVERYTHING THE ORDINARY CHANNEL MAY NOT DO, IN ONE PLACE, REACHED ONLY WITH AN `env`. Returning null hands
   the node back to the folder's own switch, so a node this mode has nothing to say about is folded exactly as
   it is folded with no env at all. */
function foldEnvOnly(node, binds, depth, env) {
  switch (node.type) {
    case "Identifier":
      /* THE ENUMERATED PARAMETER. Its value is a candidate drawn from the function's OWN body, so the text
         this returns is a value the bundler really can be called with rather than one invented here. */
      if (env.vars.has(node.name)) return { text: env.vars.get(node.name), holes: 0 };
      return null;
    case "MemberExpression":
    case "OptionalMemberExpression": {
      /* `MAP[id]` WITH A LITERAL KEY. The object is either written inline — which is what a minified chunk
         table is — or is a name this file already trusts to be one thing. */
      if (!node.computed) {
        /* A PROPERTY ASSIGNED EXACTLY ONCE IN THE FILE, which is where a bundler keeps its public path and
           its chunk-URL function: `o.p="/assets/webpack/"`, `p.u=id=>...`. The once-ness is the same argument
           that makes a bound-once NAME foldable — a slot written in one place cannot be two things — and it
           additionally requires the OBJECT to be a name this file binds once, because a property of an
           object nobody can identify names nothing. */
        const m = env.memberOnce.get(memberKey(node));
        if (m) return fold(m, binds, depth + 1, env);
        return null;
      }
      const key = fold(node.property, binds, depth + 1, env);
      if (key.holes !== 0) return null;
      const obj = objectLiteralOf(node.object, binds, env);
      if (!obj) return null;
      for (const q of obj.properties) {
        if (q.type !== "ObjectProperty" || q.computed) continue;
        const k = q.key.type === "Identifier" ? q.key.name
                : q.key.type === "StringLiteral" ? q.key.value
                : q.key.type === "NumericLiteral" ? String(q.key.value) : null;
        if (k === key.text) return fold(q.value, binds, depth + 1, env);
      }
      return MISS();
    }
    case "LogicalExpression": {
      /* ONLY PAST A MISS, which is the one case a static reader can settle without knowing a runtime value:
         `undefined || x` IS `x` in every execution, so this decides nothing the program had a choice about. */
      if (node.operator !== "||") return null;
      const l = fold(node.left, binds, depth + 1, env);
      if (l.miss) return fold(node.right, binds, depth + 1, env);
      if (l.holes === 0 && l.text !== "") return l;
      return null;
    }
    case "BinaryExpression": {
      /* AN EQUALITY BETWEEN TWO SETTLED OPERANDS, so the ternary chain a bundler writes its manifest as can
         be DECIDED per candidate instead of collapsing to a hole. Compared as TEXT because that is what the
         fold produces, and `9016===e` with the candidate `9016` is the only shape this has to answer. */
      if (node.operator !== "===" && node.operator !== "==" &&
          node.operator !== "!==" && node.operator !== "!=") return null;
      const a = fold(node.left, binds, depth + 1, env), b = fold(node.right, binds, depth + 1, env);
      if (a.holes !== 0 || b.holes !== 0) return null;
      const eq = a.text === b.text;
      return { text: "", holes: 0, cmp: node.operator[0] === "!" ? !eq : eq };
    }
    case "ConditionalExpression": {
      const t = fold(node.test, binds, depth + 1, env);
      if (t.cmp === undefined) return null;
      return fold(t.cmp ? node.consequent : node.alternate, binds, depth + 1, env);
    }
    case "CallExpression":
    case "OptionalCallExpression": {
      /* INLINING THE ONE APPLICATION THIS ROW IS ABOUT. `env.app` is that call node and there is exactly one
         of it per row — a composition holding two applications is REFUSED by the scanner rather than guessed
         at, because two unknown parameters make the address set a product of two domains and nothing here
         has established the two are ever indexed together. */
      if (node !== env.app) return null;
      const fn = env.fn;
      const inner = new Map(env.vars);
      inner.set(fn.params[0].name, env.candidate);
      return fold(fn.body.type === "BlockStatement" ? fn.body.body[0].argument : fn.body,
                  binds, depth + 1, { ...env, vars: inner });
    }
    default:
      return null;
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
  const fnDecls = new Map(); // name -> its FunctionDeclaration node
  /* `assigned` IS NOT A SET OF ASSIGNMENTS AND THE NAME IS OLDER THAN THIS COMMENT. `bind` puts a name there
     whenever it has NO initializer, so every function declaration and every parameter is in it — which is
     right for `binds`, whose question is "may this name be folded", and WRONG for any question of the form
     "does this name still mean what it was bound to". The webpack runtime is a FunctionDeclaration with its
     public path assigned to a property of itself, so a slot test keyed on `assigned` refuses every real
     instance of the shape it exists to read. `reassigned` is the narrow set: written by an assignment or an
     update, and by nothing else. */
  const reassigned = new Set();
  const memberWrites = new Map(); // "obj.prop" -> { n, node }  every assignment to that slot
  const memberOther = new Set();  // "obj.prop" slots reached some way this pass cannot account for

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
        if (n.type === "FunctionDeclaration" && n.id) fnDecls.set(n.id.name, n);
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
      case "AssignmentExpression": {
        if (n.left.type === "Identifier") { assigned.add(n.left.name); reassigned.add(n.left.name); }
        /* EVERY WRITE TO AN `obj.prop` SLOT IS TALLIED, INCLUDING THE ONES THAT DISQUALIFY IT. A slot written
           by `+=`, or through a COMPUTED property (`obj[k]=v`, which may be this very slot and this pass
           cannot tell), is one whose single-assignment claim cannot be made — so it is poisoned by name
           rather than left looking clean, which is the direction that refuses a fold instead of inventing
           an address. */
        const k = memberKey(n.left);
        if (k) {
          if (n.operator !== "=") memberOther.add(k);
          else {
            const cur = memberWrites.get(k);
            if (cur) cur.n++; else memberWrites.set(k, { n: 1, node: n.right });
          }
        } else if (n.left.type === "MemberExpression" && n.left.computed &&
                   n.left.object.type === "Identifier") {
          memberOther.add(n.left.object.name + ".*");
        }
        break;
      }
      case "UpdateExpression":
        if (n.argument.type === "Identifier") { assigned.add(n.argument.name); reassigned.add(n.argument.name); }
        if (memberKey(n.argument)) memberOther.add(memberKey(n.argument));
        break;
      default: break;
    }
  });

  const binds = new Map();
  for (const [name, c] of count)
    if (c === 1 && !assigned.has(name) && init.has(name)) binds.set(name, { node: init.get(name) });
  /* THE SLOTS A SINGLE ASSIGNMENT SETTLES. Three conditions and each one refuses a real corpus shape: the
     slot is written exactly once with a plain `=`; the OBJECT is a name this file binds exactly once and
     never reassigns, so the slot belongs to one object; and nothing in the file writes that object through a
     COMPUTED property, which could be this slot under another spelling. */
  const memberOnce = new Map();
  for (const [k, v] of memberWrites) {
    if (v.n !== 1) continue;
    if (memberOther.has(k)) continue;
    const objName = k.slice(0, k.indexOf("."));
    if (count.get(objName) !== 1 || reassigned.has(objName)) continue;
    if (memberOther.has(objName + ".*")) continue;
    memberOnce.set(k, v.node);
  }
  /* `assigned` LEAVES WITH THE OTHER TWO because the global-object test needs it and cannot recompute it
     without walking the file again: a name this file never BINDS but somewhere ASSIGNS is a name whose value
     at the call site this pass has no claim on, global or not. */
  return { binds, count, assigned, reassigned, fnDecl: fnDecls, memberOnce };
}

/* ── THE CHUNK MANIFEST ───────────────────────────────────────────────────────────────────────────────────
   WHAT THIS ANSWERS AND WHY IT IS A BAND AND NOT A DOOR. A bundler that compiled `import()` away emits its
   chunk addresses as a MAP and a PUBLIC-PATH literal, composes them in a one-parameter function, and hands
   the result to an injected `<script>`. Every one of those addresses is in the file as plain literal text, so
   a PARSE has them and the site channel recovered NONE of them — the composition crosses a function boundary
   and the per-argument fold stops at the call. That was this file's own named residual and this is it built.
   IT IS NOT A REQUEST COUNT AND MUST NEVER BE QUOTED AS ONE. A manifest names every chunk the bundle COULD
   load; one run loads a few. So these rows are ADDRESSES RECOVERED FROM THE TEXT, reported beside the door
   totals and summed into none of them, in the same relationship the blind-spot band already has to a door.
   THE ENUMERATION IS OVER A DOMAIN THE FUNCTION ITSELF NAMES, WHICH IS WHAT KEEPS IT FROM INVENTING. The
   candidates are the KEYS of the object literals the body indexes with its own parameter and the LITERALS the
   body compares that parameter against — nothing else. For each candidate the whole composition is folded
   with the parameter bound to it, and a candidate whose fold leaves a hole is DROPPED. So every address
   emitted is one the function demonstrably returns for an input the function itself mentions; the set is the
   image of the enumerated domain and never a guess about the domain's extent.
   THE CORRELATION IS THE PART THAT WOULD BE WRONG IF IT WERE DONE THE OBVIOUS WAY. A chunk-URL function
   indexes TWO maps with ONE parameter — a name map and a hash map — so enumerating each map independently
   would emit the product of two domains and nearly every member of it would be an address that does not
   exist. Binding the parameter ONCE per candidate and folding the whole expression is what keeps the two
   lookups at the same point of the domain.
   IT IS KEYED ON THE EXPRESSION AND ON NO BUNDLER'S NAME, which CLAUDE.md's RUN-DON'T-MATCH rule requires:
   nothing here reads a runtime's identifier, a chunk-file naming convention or a public-path spelling. What
   it keys on is a single-parameter function applied to something, indexed by its own parameter — a shape,
   which is why it finds the webpack-4 and the webpack-5 form with one rule and would find a third. */
/* WHAT A COMPOSITION ROOT IS, AND WHY A CALL IS NOT ONE UNLESS IT IS THE APPLICATION ITSELF. A minified
   bundle is ONE top-level call — `!function(e){...}([...])` — so a rule that treated any CallExpression
   CONTAINING an application as a composition made the whole FILE the first candidate, found dozens of
   applications in it, refused it for holding two, and descended no further. Measured before the fix: 1383
   refusals and ZERO rows at every runtime this channel was built for. A string composition is a `+` or a
   template; a call is a root only when it IS the one application, which is the `el.src = t(id)` shape. */
const isStringComposer = (n) => (n.type === "BinaryExpression" && n.operator === "+") ||
                                n.type === "TemplateLiteral";

function applicationsIn(node, binds, env, out) {
  if (!node || typeof node.type !== "string") return;
  if ((node.type === "CallExpression" || node.type === "OptionalCallExpression") &&
      node.arguments.length === 1) {
    const fn = singleParamFn(node.callee, binds, env);
    if (fn) out.push({ app: node, fn });
  }
  for (const k of VISITOR_KEYS[node.type] || []) {
    const v = node[k];
    if (Array.isArray(v)) { for (const c of v) applicationsIn(c, binds, env, out); }
    else applicationsIn(v, binds, env, out);
  }
}

/* THE CANDIDATE DOMAIN, READ OUT OF THE FUNCTION'S OWN BODY AND NOWHERE ELSE. */
function candidateDomain(fn, binds, env) {
  const param = fn.params[0].name;
  const body = fn.body.type === "BlockStatement" ? fn.body.body[0].argument : fn.body;
  const out = new Set();
  const lit = (n) => n && (n.type === "StringLiteral" ? n.value
                         : n.type === "NumericLiteral" ? String(n.value) : null);
  const visit = (n) => {
    if (!n || typeof n.type !== "string") return;
    if ((n.type === "MemberExpression" || n.type === "OptionalMemberExpression") && n.computed &&
        n.property.type === "Identifier" && n.property.name === param) {
      const obj = objectLiteralOf(n.object, binds, env);
      if (obj) for (const q of obj.properties) {
        if (q.type !== "ObjectProperty" || q.computed) continue;
        const k = q.key.type === "Identifier" ? q.key.name
                : q.key.type === "StringLiteral" ? q.key.value
                : q.key.type === "NumericLiteral" ? String(q.key.value) : null;
        if (k !== null) out.add(k);
      }
    }
    if (n.type === "BinaryExpression" && (n.operator === "===" || n.operator === "==")) {
      if (n.left.type === "Identifier" && n.left.name === param) { const v = lit(n.right); if (v !== null) out.add(v); }
      if (n.right.type === "Identifier" && n.right.name === param) { const v = lit(n.left); if (v !== null) out.add(v); }
    }
    for (const k of VISITOR_KEYS[n.type] || []) {
      const v = n[k];
      if (Array.isArray(v)) { for (const c of v) visit(c); } else visit(v);
    }
  };
  visit(body);
  return { param, out };
}

function scanManifest(ast, binds, base, filename) {
  const rows = [];
  let refusedTwoApplications = 0;
  const descend = (node) => {
    if (!node || typeof node.type !== "string") return;
    const bare = (node.type === "CallExpression" || node.type === "OptionalCallExpression") &&
                 node.arguments.length === 1 ? singleParamFn(node.callee, binds, base) : null;
    if (isStringComposer(node) || bare) {
      const apps = [];
      if (bare) apps.push({ app: node, fn: bare });
      else applicationsIn(node, binds, base, apps);
      if (apps.length === 1) {
        const { app, fn } = apps[0];
        const dom = candidateDomain(fn, binds, base);
        if (dom.out.size) {
          const addrs = new Set(), frags = new Set();
          let dropped = 0;
          for (const cand of dom.out) {
            const r = fold(node, binds, 0, { ...base, vars: new Map(), app, fn, candidate: cand });
            if (r.holes !== 0 || r.text === "") { dropped++; continue; }
            if (looksLikeAddress(r.text)) addrs.add(r.text); else frags.add(r.text);
          }
          if (addrs.size || frags.size) {
            rows.push({ file: filename, line: node.loc ? node.loc.start.line : 0,
                        candidates: dom.out.size, dropped, addresses: [...addrs], fragments: frags.size });
            return;   /* the OUTERMOST composition owns the row; an inner one would re-report it shorter */
          }
        }
      } else if (apps.length > 1) {
        /* AND THE REFUSAL DOES NOT DESCEND, which is the half a counter alone would have got wrong. Walking
           into a refused composition finds one of its applications on its own and enumerates THAT — yielding
           an address with the other application's whole contribution missing from it, which is not a shorter
           answer but a fabricated one. The control that removes this `return` reports such a row. */
        refusedTwoApplications++;
        return;
      }
    }
    for (const k of VISITOR_KEYS[node.type] || []) {
      const v = node[k];
      if (Array.isArray(v)) { for (const c of v) descend(c); } else descend(v);
    }
  };
  descend(ast);
  return { rows, refusedTwoApplications };
}

/* GUARD DEPTH IS PROVENANCE AND NOT DECORATION. CLAUDE.md §What-the-tool-produces' proposition is "what the
   bundle CAN do but didn't", and a parse reaches a gated call site whether the gate is taken or not — which
   is the one axis on which a parse is structurally STRONGER than a run, and it must be measured rather than
   conceded or assumed. The depth is the number of enclosing tests a runtime would have to satisfy: an `if`
   or `switch` body, a `?:` arm, the right-hand side of `&&`/`||`/`??`, and a `catch`. */
const GUARDS = new Set(["IfStatement", "ConditionalExpression", "SwitchCase", "CatchClause"]);
/* EVERY NODE THAT OPENS A FUNCTION SCOPE — the population `fnDepth` counts. A class STATIC BLOCK and a
   getter/setter are in it for the same reason an ordinary method is: each is a body something has to invoke.
   A `Program` is deliberately NOT in it, because depth 0 is exactly "the module body runs this". The list is
   asserted against @babel/types rather than trusted, one line down, so a Babel release that renames a node
   kind fails loudly instead of silently reporting every call in that shape at depth 0 — which is the
   flattering direction and the one that would make a door look reachable. */
const FN_SCOPES = new Set(["FunctionDeclaration", "FunctionExpression", "ArrowFunctionExpression",
                           "ObjectMethod", "ClassMethod", "ClassPrivateMethod", "StaticBlock"]);
for (const t of FN_SCOPES)
  if (!(t in VISITOR_KEYS))
    die(`FN_SCOPES names the node kind ${t}, which this @babel/types does not have — the reach column would ` +
        `report every call inside one at function depth 0, which reads as "the module body runs this" and is ` +
        `the direction that makes a door look reachable when it is not.`);

function readFile(src, filename) {
  let ast = null, err = null;
  for (const sourceType of ["module", "script"]) {
    try { ast = parse(src, { sourceType, errorRecovery: false, plugins: [] }); err = null; break; }
    catch (e) { err = e; }
  }
  if (!ast) return { parsed: false, error: String(err && err.message || err).slice(0, 160), sites: [], pathish: new Set(), blind: [], xhrOpenSkippedNonLiteralMethod: 0,
                     globalDoor: { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 },
                     manifest: { rows: [], refusedTwoApplications: 0 },
                     spell: new Map(), spellOther: { globalComputedDynamic: 0 } };

  const { binds, count: bindCount, assigned: bindAssigned, reassigned, fnDecl, memberOnce } = collectBinds(ast);
  /* THE ENUMERATING FOLD'S FIXED HALF, built once per file: what a name and a slot resolve to. The per-row
     half — which application, which candidate — is added at the row. */
  const envBase = { memberOnce, fnDecl, count: bindCount, reassigned };
  const manifest = scanManifest(ast, binds, envBase, filename);
  const sites = [];
  const pathish = new Set();
  const blind = [];
  let xhrOpenSkippedNonLiteralMethod = 0;
  /* THE GLOBAL-REACHED DOOR'S OWN THREE NUMBERS, so the widening is read beside its price on every run. */
  const globalDoor = { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 };
  /* A NAME IS THE GLOBAL OBJECT ONLY IF THIS FILE NEVER BINDS IT AND NEVER ASSIGNS IT. Anything else and
     the object to the left is a value this pass cannot name, which is the case the DOORS comment refuses. */
  const provenGlobal = (o) => !!o && o.type === "Identifier" && GLOBAL_OBJECTS.has(o.name) &&
                              !(bindCount.get(o.name) > 0) && !bindAssigned.has(o.name);
  const attached = new Set();          // node identity of URL args, so a door's own literal is not double-counted
  let guard = 0;
  const guardStack = [];
  /* WHAT ENCLOSES THE CALL — the REACH axis, kept on its own stack for the reason the guard stack is: a
     depth read off the node afterwards would need a parent chain this walker deliberately does not carry.
     It answers a different question from `guard` and the two are not substitutes. `guard` says whether a
     TEST stands in front of the call, which a parse reads whether the gate is taken or not; this says what
     has to be CALLED for the call to happen at all, which is the only thing that separates a door the
     engine rings from one it does not. */
  const fnStack = [];

  walk(ast, (n) => {
    if (GUARDS.has(n.type)) { guardStack.push(n); guard++; }
    else if (n.type === "LogicalExpression") { guardStack.push(n); guard++; }
    if (FN_SCOPES.has(n.type)) fnStack.push(n);

    /* ONE HELPER FOR BOTH SHAPES, because `window.fetch(u)` and `new self.Worker(u)` pose the identical
       question — is the thing to the left the global object — and two copies of the answer would be free to
       disagree about it. `family` is the door family the PROPERTY is looked up in, which is why a
       `member-call` door can never arrive here: the assert under DOORS keeps the two families disjoint. */
    const globalReached = (obj, prop, family) => {
      if (!DOOR_BY_NAME.has(family + ":" + prop)) return null;
      if (provenGlobal(obj)) { globalDoor.admitted++; return DOOR_BY_NAME.get(family + ":" + prop); }
      if (obj && obj.type === "Identifier" && GLOBAL_OBJECTS.has(obj.name)) globalDoor.refusedBoundName++;
      else globalDoor.declinedNonGlobalReceiver++;
      return null;
    };

    let door = null, args = null;
    if (n.type === "CallExpression" || n.type === "OptionalCallExpression") {
      const c = n.callee;
      if (c && c.type === "Identifier")              door = DOOR_BY_NAME.get("callee-global:" + c.name);
      else if (c && c.type === "Import")             door = DOOR_BY_NAME.get("dynamic-import:import");
      else if (c && (c.type === "MemberExpression" || c.type === "OptionalMemberExpression") &&
               !c.computed && c.property.type === "Identifier")
                                                     door = DOOR_BY_NAME.get("member-call:" + c.property.name) ||
                                                            globalReached(c.object, c.property.name, "callee-global");
      args = n.arguments;
    } else if (n.type === "NewExpression" && n.callee) {
      if (n.callee.type === "Identifier") door = DOOR_BY_NAME.get("new:" + n.callee.name);
      else if ((n.callee.type === "MemberExpression" || n.callee.type === "OptionalMemberExpression") &&
               !n.callee.computed && n.callee.property.type === "Identifier")
        door = globalReached(n.callee.object, n.callee.property.name, "new");
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
        /* THE REACH PAIR. `fnDepth` 0 is a call the module body itself performs, so evaluating the program
           reaches it; anything above 0 needs its enclosing function CALLED. `innerAsync` is whether the
           INNERMOST enclosing function is an async one, which is the axis the two door classes come apart
           on: a call inside an `async` body runs only once something invokes that body, and in a real SPA
           the invoker is an effect flushed after a render commit, an event handler, a timer or an idle
           callback. Both are properties of the TEXT and neither is a claim about any engine. */
        fnDepth: fnStack.length,
        innerAsync: fnStack.length > 0 ? !!fnStack[fnStack.length - 1].async : false,
        method: door.arg0Method ? args[0].value.toUpperCase() : (door.id === "sendBeacon" ? "POST" : "GET"),
        line: n.loc ? n.loc.start.line : 0, file: filename,
      });
    }
  }, (n) => {
    if (GUARDS.has(n.type) || n.type === "LogicalExpression") { guardStack.pop(); guard--; }
    if (FN_SCOPES.has(n.type)) fnStack.pop();
  });


  /* ── HOW THIS PROGRAM SPELLS THE DECLARED ENTRY NAMES ───────────────────────────────────────────────────
     A SEPARATE WALK, DELIBERATELY, AND THE REASON IS CALIBRATION RATHER THAN CLARITY. Folding this into the
     door walk above would have put a new branch inside the classifier every existing number is produced by,
     and the whole worth of a figure added to this file is that the figures already here did not move. A
     third traversal costs a fraction of the parse it rides on and buys an A/B nobody has to argue about.
     WHAT IS NOT A REFERENCE IS MARKED BY ITS PARENT ON THE WAY DOWN, which needs no parent chain: this
     walker enters a node strictly before its children, so a property name, an object key, a declaration id,
     a parameter and an import local are all struck from the reference population by the node that owns them
     before the identifier itself is reached. Without that, an object literal carrying a `fetch` key would
     read as a program naming the platform's fetch, which is the precision failure that makes the bare count
     useless in the direction that hides the floor.
     AN ALIAS IS NOT A SPELLING OF ITS OWN AND IS DELIBERATELY NOT A COLUMN. `const f = fetch` reaches the
     name by the bare spelling and `const f = window.fetch` by the property spelling, so both are already
     attributed where they happen; a further column would double-count one occurrence under two headings.
     DESTRUCTURING IS THE ONE EXCEPTION, because there the name appears only as a PATTERN KEY and in no
     reference position at all, so nothing else would see it. */
  const spell = new Map();                                  // declared name -> its spelling tally
  const spellOther = { globalComputedDynamic: 0 };
  const spellHit = (nm, k) => {
    let t = spell.get(nm);
    if (!t) { t = spellTally(); spell.set(nm, t); }
    t[k]++;
  };
  const notRef = new Set();
  /* THIS BAND KEEPS ITS OWN BINDER SET AND DOES NOT REUSE `collectBinds`, AND THE REASON IS A DIRECTION RATHER
     THAN A PREFERENCE. `collectBinds` walks EVERY identifier under a pattern and binds it, which is right for
     its own question — may this name be folded — because over-binding there REFUSES a fold. Asked this
     question it over-binds in the direction that hides the floor: `function f(a = fetch())` binds `a` and
     REFERENCES fetch, and counting that reference as a bound name moves it out of the column the landed row
     already sees and into the column this pass cannot decide, which reports the property spelling as more
     necessary than it is. THE SELF-TEST ROW FOR THAT SHAPE IS WHAT CAUGHT IT and is why it is a control.
     A BINDING POSITION FEEDS BOTH SETS AND A PROPERTY NAME FEEDS ONLY ONE, which is the whole of why they are
     two sets: an object key and a member's property name are struck from the reference population and bind
     nothing, so folding them into a binder set would make every file that carries a `fetch` KEY read as a
     file that shadows fetch. */
  const spellBound = new Set();
  const bindName = (id) => { if (id && id.type === "Identifier") { notRef.add(id); spellBound.add(id.name); } };
  const markPattern = (p) => {
    if (!p) return;
    switch (p.type) {
      case "Identifier": bindName(p); return;
      case "AssignmentPattern": markPattern(p.left); return;
      case "RestElement": markPattern(p.argument); return;
      case "ArrayPattern": for (const e of p.elements || []) markPattern(e); return;
      case "ObjectPattern":
        for (const q of p.properties || []) {
          if (q.type === "ObjectProperty") { if (!q.computed && q.key) notRef.add(q.key); markPattern(q.value); }
          else if (q.type === "RestElement") markPattern(q.argument);
        }
        return;
      default: return;
    }
  };
  walk(ast, (n) => {
    switch (n.type) {
      case "MemberExpression":
      case "OptionalMemberExpression": {
        if (!n.computed && n.property && n.property.type === "Identifier") {
          notRef.add(n.property);
          const nm = n.property.name;
          if (ENTRY_NAMES.has(nm)) {
            if (provenGlobal(n.object)) spellHit(nm, "qualified");
            else if (n.object && n.object.type === "Identifier" && GLOBAL_OBJECTS.has(n.object.name))
              spellHit(nm, "qualifiedBoundGlobal");
            else spellHit(nm, "instanceMember");
          }
        } else if (n.computed && n.property) {
          const k = n.property;
          if (k.type === "StringLiteral" && ENTRY_NAMES.has(k.value))
            spellHit(k.value, provenGlobal(n.object) ? "computedLiteral" : "instanceMemberComputed");
          /* A COMPUTED KEY THIS PASS CANNOT READ BELONGS TO NO NAME AND IS COUNTED APART. `self[n]` may be
             any member of the global object, so attributing it to one would invent a population; leaving it
             out entirely would let a corpus that reaches everything dynamically read as reaching nothing. */
          else if (k.type !== "StringLiteral" && provenGlobal(n.object)) spellOther.globalComputedDynamic++;
        }
        break;
      }
      case "ObjectProperty":
      case "ObjectMethod":
      case "ClassMethod":
      case "ClassPrivateMethod":
      case "ClassProperty":
        if (!n.computed && n.key) notRef.add(n.key);
        break;
      case "VariableDeclarator": {
        if (n.id && n.id.type === "Identifier") bindName(n.id);
        else markPattern(n.id);
        /* THE ONE SHAPE NO REFERENCE POSITION WOULD SHOW. */
        if (n.id && n.id.type === "ObjectPattern" && provenGlobal(n.init)) {
          for (const p of n.id.properties || []) {
            if (p.type !== "ObjectProperty" || p.computed) continue;
            const k = p.key;
            const nm = k && (k.type === "Identifier" ? k.name : k.type === "StringLiteral" ? k.value : null);
            if (nm && ENTRY_NAMES.has(nm)) spellHit(nm, "destructuredFromGlobal");
          }
        }
        break;
      }
      case "FunctionDeclaration":
      case "FunctionExpression":
      case "ClassDeclaration":
      case "ClassExpression":
      case "ArrowFunctionExpression":
        if (n.id) bindName(n.id);
        for (const p of n.params || []) markPattern(p);
        break;
      case "CatchClause":
        markPattern(n.param);
        break;
      /* AN ASSIGNMENT IS NOT A BINDING AND IS STILL DISQUALIFYING, for `provenGlobal`'s own reason: a name the
         file WRITES may mean something other than the platform's by the time it is read. */
      case "AssignmentExpression":
        if (n.left && n.left.type === "Identifier") spellBound.add(n.left.name);
        break;
      case "UpdateExpression":
        if (n.argument && n.argument.type === "Identifier") spellBound.add(n.argument.name);
        break;
      case "ImportSpecifier":
      case "ImportDefaultSpecifier":
      case "ImportNamespaceSpecifier":
        if (n.local) bindName(n.local);
        if (n.imported) notRef.add(n.imported);
        break;
      case "ExportSpecifier":
        if (n.local) notRef.add(n.local);
        if (n.exported) notRef.add(n.exported);
        break;
      case "LabeledStatement":
      case "BreakStatement":
      case "ContinueStatement":
        if (n.label) notRef.add(n.label);
        break;
      case "UnaryExpression":
        /* WHICH OF THE TWO READS THE LANDED ROW WOULD HAVE RECORDED. The non-throwing form the unary parser
           patches in for `typeof` is the one a feature test uses, so a name reached only that way is a
           program PROBING for a capability rather than using it — a distinction the engine's own row carries
           in its second argument and this column exists to be read against. */
        if (n.operator === "typeof" && n.argument && n.argument.type === "Identifier" &&
            ENTRY_NAMES.has(n.argument.name)) spellHit(n.argument.name, "typeofBare");
        break;
      default: break;
    }
  });
  /* A SECOND PASS FOR THE BARE COLUMN, BECAUSE A REFERENCE MAY PRECEDE ITS OWN BINDING. A hoisted declaration
     and a function body that runs before the `var` below it both put the reference first in source order, so
     classifying a bare name during the marking pass would read the same program two ways depending on where
     its binding happened to be written. */
  walk(ast, (n) => {
    if (n.type !== "Identifier" || notRef.has(n) || !ENTRY_NAMES.has(n.name)) return;
    spellHit(n.name, spellBound.has(n.name) ? "bareBoundName" : "bareFree");
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
    /* THE BASE RATE KEEPS ITS OWN TWO ALTERNATIVES. `looksLikeAddress` adds a third for the manifest, and a
       relative `./x` counted here would move a number other lanes price against, so this asks for the two
       it always asked for. The shared helper is what stops the two channels disagreeing about the two. */
    if (/^https?:\/\/[^\s]+$/.test(v) || /^\/[A-Za-z0-9_][^\s"'<>]*$/.test(v)) pathish.add(v);
  });

  return { parsed: true, error: null, sites, pathish, blind, xhrOpenSkippedNonLiteralMethod, globalDoor,
           manifest, spell, spellOther };
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
  // THE GLOBAL OBJECT REACHING A PLATFORM DOOR. `fetch(u)` and `window.fetch(u)` are one platform name and
  // must classify identically; a row here that stopped matching would shrink the baseline, which is the
  // flattering direction and the one this control exists to make impossible to mistake for a finding.
  [`window.fetch("/api/a")`,                                ["fetch|data|literal|/api/a"]],
  [`self.fetch(u)`,                                         ["fetch|data|opaque|{?}"]],
  [`new self.Worker("/w.js")`,                              ["new Worker|program|literal|/w.js"]],
  [`new globalThis.WebSocket("wss://h/s")`,                 ["new WebSocket|data|literal|wss://h/s"]],
  [`globalThis.fetch("/api/"+r)`,                           ["fetch|data|shape|/api/{?}"]],
  // NEGATIVES — a classifier that reports any of these is over-counting, which is the direction that would
  // make the parse look stronger than it is and the engine's contribution smaller than it is.
  [`window.open("/x","_blank")`,                            []],
  [`db.open("GET")`,                                        []],
  [`m.get("/api/x")`,                                       []],
  [`const s="/api/looks-like-an-endpoint"`,                 []],
  [`x.open(method,"/t")`,                                   []],
  // A RECEIVER THAT CARRIES INFORMATION IS STILL REFUSED, which is the precision half of the widening above
  // and is the larger population by an order of magnitude: admitting any of these would put library-wrapper
  // method calls on the static side of a comparison the engine never answered for.
  [`api.fetch("/x")`,                                       []],
  [`this.fetch("/x")`,                                      []],
  [`mk().fetch("/x")`,                                      []],
  [`new p.Worker("/w.js")`,                                 []],
  // A GLOBAL NAME THIS FILE BINDS IS NOT THE GLOBAL OBJECT — the UMD wrapper, and every `var self=this`.
  [`function f(window){return window.fetch("/x")}`,         []],
  [`var self=this;self.fetch("/x")`,                        []],
  // SHADOWING — the fold must REFUSE a name the file binds twice, because a wrong fold INVENTS an address.
  [`const B="/a";function f(){const B="/b";return fetch(B)}`, ["fetch|data|opaque|{?}"]],
  // THE REACH BAND, ARMED IN ALL THREE ARMS. Each of these has to CLASSIFY as an ordinary row too, so they
  // sit in this table rather than in a set of their own: a reach control that stopped being a door row would
  // silently leave the band measuring a smaller population.
  [`async function f(){return fetch("/ra")}`,               ["fetch|data|literal|/ra"]],
  [`function f(){return fetch("/rs")}`,                     ["fetch|data|literal|/rs"]],
  [`const g=async()=>fetch("/rq")`,                         ["fetch|data|literal|/rq"]],
  [`async function o(){return function(){return fetch("/rn")}}`, ["fetch|data|literal|/rn"]],
];
const SELFTEST_GUARDED = new Set([`if(a){fetch("/g")}`]);
/* THE REACH BAND'S OWN CONTROLS — source to the pair the row must carry. It is armed in BOTH directions and
   at BOTH ends, which is the discipline CLAUDE.md §A-CONTROL-ARMS-ONLY-ON-A-SITE-THE-INSTRUMENT-CAN-JUDGE
   asks for: a band whose only control is a positive one cannot tell "this corpus has no async sites" from
   "the async test is stuck on". The fourth row is the one that matters most and is the easiest to get wrong
   — an async function enclosing a SYNC one — because `innerAsync` names the INNERMOST enclosing function and
   a walker that read the OUTERMOST would pass the first three and fail only here. */
const SELFTEST_REACH = new Map([
  [`fetch("/api/users")`,                                   { fnDepth: 0, innerAsync: false }],
  [`async function f(){return fetch("/ra")}`,                { fnDepth: 1, innerAsync: true }],
  [`function f(){return fetch("/rs")}`,                      { fnDepth: 1, innerAsync: false }],
  [`const g=async()=>fetch("/rq")`,                          { fnDepth: 1, innerAsync: true }],
  [`async function o(){return function(){return fetch("/rn")}}`, { fnDepth: 2, innerAsync: false }],
]);

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

/* THE CHUNK-MANIFEST CHANNEL IS ARMED ON BOTH SHAPES AND ON EVERY REFUSAL IT CLAIMS TO MAKE. Its number is
   an ADDRESS SET, so a row that silently stopped enumerating would report a smaller manifest — and a smaller
   manifest reads as "a parse cannot reach these after all", which is the flattering direction and exactly the
   answer this file must not manufacture. Each row states the addresses, how many candidates were drawn, and
   how many were DROPPED for a hole: the dropped figure is what arms the rule that a missing map key is not an
   empty string, and without it a candidate absent from the hash map would emit an address with a segment
   deleted from it. */
const SELFTEST_MANIFEST = [
  /* THE TERNARY-CHAIN FORM: a public path assigned to a slot, a chain assigned to another, composed. */
  [`var p={};p.u=e=>1===e?"a/1.js":2===e?"a/2.js":"a/x.js";p.p="/pub/";var b=p.p+p.u(e)`,
   { addresses: ["/pub/a/1.js", "/pub/a/2.js"], candidates: 2, dropped: 0 }],
  /* THE TWO-MAP FORM, WHICH IS THE ONE THE CORRELATION MATTERS FOR. `c` is in the name map and not the hash
     map, so it DROPS; `b` is in the hash map and not the name map, so `||` falls through to the key itself. */
  [`function t(e){return o.p+""+({a:"A",c:"C"}[e]||e)+"."+{a:"h",b:"g"}[e]+".chunk.js"}function o(){}o.p="/w/";s.src=t(e)`,
   { addresses: ["/w/A.h.chunk.js", "/w/b.g.chunk.js"], candidates: 3, dropped: 1 }],
  /* THE CORRELATION, ASSERTED AS AN EXACT LIST, WHICH IS THE ONLY CONTROL ON THE WHOLE DESIGN THAT MATTERS.
     Two maps are indexed by ONE parameter, so the answer is two addresses and not four: `na` pairs with `ha`
     and `nb` with `hb`, and the crossed pairs `./na.hb.js` and `./nb.ha.js` name nothing that exists. The
     `want` list is compared exactly, so a fold that enumerated the maps INDEPENDENTLY fails here — and it
     would fail loudly, because a cross-product grows as the square. Measured on one real runtime: 394
     addresses recovered where crossing the two maps would have emitted up to 155236. */
  [`function t(e){return "./"+{a:"na",b:"nb"}[e]+"."+{a:"ha",b:"hb"}[e]+".js"}var b=t(e)`,
   { addresses: ["./na.ha.js", "./nb.hb.js"], candidates: 2, dropped: 0, fragments: 0 }],
  /* A LABEL TABLE IS THE SAME SHAPE AS A CHUNK MANIFEST AND MUST RECOVER NO ADDRESS. This is the precision
     control and it is the one the channel was measured failing before the shape test existed. */
  [`function t(e){return {a:"session",b:"Users"}[e]}var b=t(e)`,
   { addresses: [], candidates: 2, dropped: 0, fragments: 2 }],
  /* A CHUNK NAME WITH NO PUBLIC PATH IS A FRAGMENT, not a shorter address. */
  [`function t(e){return {a:"app.HASH.css"}[e]}var b=t(e)`,
   { addresses: [], candidates: 1, dropped: 0, fragments: 1 }],
  /* A RELATIVE ADDRESS IS ONE, which is the third alternative the shape test adds for this channel. */
  [`function t(e){return "./chunks/"+{a:"a.HASH.js"}[e]}var b=t(e)`,
   { addresses: ["./chunks/a.HASH.js"], candidates: 1, dropped: 0, fragments: 0 }],
  /* NEGATIVES — each one a shape whose fold would INVENT an address, and each refused for a stated reason. */
  //  a slot written twice is not a slot this pass can read
  [`var p={};p.u=e=>1===e?"/a":"/b";p.u=e=>"/z";var b=p.p+p.u(e)`,                     null],
  //  a computed write to the object could be this very slot under another spelling
  [`var p={};p.u=e=>1===e?"/a/1.js":"/b";p[k]=1;var b=p.u(e)`,                         null],
  //  a body with a statement before its return may narrow the parameter
  [`function t(e){var z=1;return "/x/"+{a:"A"}[e]}var b=t(e)`,                         null],
  //  an object bound more than once is not one object
  [`var p={};var p={};p.u=e=>1===e?"/a/1.js":"/b";var b=p.u(e)`,                       null],
  //  a function whose body names no candidate for its parameter enumerates nothing
  [`var p={};p.u=e=>"/static/"+e;p.p="/x/";var b=p.p+p.u(e)`,                          null],
];
/* A COMPOSITION HOLDING TWO APPLICATIONS IS REFUSED AND THE REFUSAL IS COUNTED, asserted apart because the
   expected row list is empty either way and an uncounted refusal is indistinguishable from a shape the
   scanner never saw. */
const SELFTEST_MANIFEST_TWO = `var p={};p.u=e=>1===e?"/a/1.js":"/b";p.v=e=>2===e?"/c/2.js":"/d";var b=p.u(e)+p.v(e)`;

function selftest() {
  const seenKind = new Set(), seenCls = new Set();
  let spoke = 0, reachSpoke = 0;
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
    if (SELFTEST_REACH.has(src)) {
      const want = SELFTEST_REACH.get(src), got = r.sites[0];
      if (!got || got.fnDepth !== want.fnDepth || got.innerAsync !== want.innerAsync)
        die(`SELF-TEST FAILED on the REACH band for \`${src}\`\n  want ${JSON.stringify(want)}\n` +
            `  got  ${JSON.stringify(got && { fnDepth: got.fnDepth, innerAsync: got.innerAsync })}\n` +
            `The band that says whether a door's calls sit in a body something has to CALL is measuring ` +
            `something other than what it prints, so nothing below is printed.`);
      reachSpoke++;
    }
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
  /* THE MANIFEST CHANNEL, SHAPE BY SHAPE AND REFUSAL BY REFUSAL. */
  let manifestSpoke = 0, manifestAddrs = 0, manifestFrags = 0;
  for (const [src, want] of SELFTEST_MANIFEST) {
    const r = readFile(src, "<selftest-manifest>");
    if (!r.parsed) die(`SELF-TEST: the parser refused \`${src}\` — ${r.error}`);
    const rows = r.manifest.rows;
    if (!want) {
      if (rows.length)
        die(`SELF-TEST FAILED: \`${src}\` produced ${rows.length} manifest row(s) naming ` +
            `${JSON.stringify(rows[0].addresses)}. This shape cannot be folded soundly, so every address ` +
            `above is one this file INVENTED — the single failure the enumeration may not have.`);
      continue;
    }
    if (rows.length !== 1)
      die(`SELF-TEST FAILED: \`${src}\` produced ${rows.length} manifest row(s) and not 1.`);
    const got = { addresses: [...rows[0].addresses].sort(), candidates: rows[0].candidates,
                  dropped: rows[0].dropped, fragments: rows[0].fragments };
    const wantSorted = { ...want, addresses: [...want.addresses].sort(), fragments: want.fragments || 0 };
    if (JSON.stringify(got) !== JSON.stringify(wantSorted))
      die(`SELF-TEST FAILED on the manifest channel for \`${src}\`\n  want ${JSON.stringify(wantSorted)}\n` +
          `  got  ${JSON.stringify(got)}\nThe address set this file reports as what a PARSE recovers is not ` +
          `what its own controls say it is, so nothing is printed.`);
    if (r.sites.length)
      die(`SELF-TEST FAILED: \`${src}\` produced ${r.sites.length} SITE row(s). A manifest row must never ` +
          `enter the door totals — it is an ADDRESS a parse recovers, not a request site.`);
    if (rows[0].addresses.length) manifestSpoke++;
    manifestAddrs += rows[0].addresses.length;
    if (rows[0].fragments) manifestFrags += rows[0].fragments;
  }
  if (manifestSpoke < 2) die(`SELF-TEST FAILED: only ${manifestSpoke} manifest control(s) recovered an address.`);
  if (manifestFrags < 3)
    die(`SELF-TEST FAILED: the FRAGMENT channel counted ${manifestFrags}. It is what separates a chunk ` +
        `manifest from a label table, so a zero it has never been seen to move cannot be read at all.`);
  if (manifestAddrs < 4) die(`SELF-TEST FAILED: the manifest controls enumerated only ${manifestAddrs} address(es).`);
  {
    const r = readFile(SELFTEST_MANIFEST_TWO, "<selftest-manifest>");
    if (r.manifest.rows.length)
      die(`SELF-TEST FAILED: a composition holding TWO applications produced a manifest row. Two unknown ` +
          `parameters make the address set a product of two domains and most of that product does not exist.`);
    if (r.manifest.refusedTwoApplications < 1)
      die(`SELF-TEST FAILED: the two-application refusal is not counted, so its zero over the corpus cannot ` +
          `be told from a shape the scanner never met.`);
  }
  /* THE GLOBAL-REACHED DOOR'S THREE COUNTERS ARE ARMED ONE AT A TIME AND EACH IS SHOWN RISING, because a
     counter whose zero nobody has ever seen move cannot tell "the corpus has none of these" from "this
     channel stopped counting" — and the three are the whole price of the widening, so a dead one turns a
     measured trade back into an assertion. Each row also asserts the OTHER two stay at zero, which is what
     stops one input being read as evidence for a counter it never touched. */
  const gd = (src) => readFile(src, "<selftest>").globalDoor;
  const gdWant = [
    [`window.fetch("/api/a")`,                        { admitted: 1, refusedBoundName: 0, declinedNonGlobalReceiver: 0 }],
    [`new self.Worker("/w.js")`,                      { admitted: 1, refusedBoundName: 0, declinedNonGlobalReceiver: 0 }],
    [`function f(window){return window.fetch("/x")}`, { admitted: 0, refusedBoundName: 1, declinedNonGlobalReceiver: 0 }],
    [`api.fetch("/x")`,                               { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 1 }],
    [`new p.Worker("/w.js")`,                         { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 1 }],
    /* A PROPERTY NAMING NO DOOR MUST TOUCH NO COUNTER AT ALL, or the declined figure becomes a count of
       every member call in the corpus and the precision it is printed as pricing is unreadable. */
    [`api.load("/x")`,                                { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 }],
    [`window.open("/x","_blank")`,                    { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 }],
  ];
  for (const [src, want] of gdWant) {
    const got = gd(src);
    for (const k of Object.keys(want))
      if (got[k] !== want[k])
        die(`SELF-TEST FAILED: globalDoor.${k} is ${got[k]} and not ${want[k]} for \`${src}\`. The three ` +
            `numbers that price the global-reached door against what it declines are measuring something ` +
            `other than what they are printed as meaning.`);
  }
  /* THE SPELLING BAND IS ARMED ONE COLUMN AT A TIME, AND THE NEGATIVE ROWS ARE THE HALF THAT MATTERS. A
     probe whose expected output is silence, with no run in which the same probe shape SPOKE, has calibrated
     nothing — and here the silence has to be shown in BOTH directions, because the column that decides the
     verdict is a count of sites where a bare reference is ABSENT. A classifier that quietly counted an object
     key or a parameter as a bare reference would report the property spelling as harmless; one that quietly
     missed a bare reference would report it as critical. Each row therefore states the WHOLE tally, so a
     column rising that should not have is a failure exactly as a column staying flat is. */
  const sp = (src) => {
    const r = readFile(src, "<selftest>");
    const flat = spellTally();
    for (const t of r.spell.values()) for (const k of Object.keys(t)) flat[k] += t[k];
    flat.globalComputedDynamic = r.spellOther.globalComputedDynamic;
    return flat;
  };
  const spWant = [
    [`fetch("/a")`,                             { bareFree: 1 }],
    [`typeof requestAnimationFrame`,            { bareFree: 1, typeofBare: 1 }],
    [`setTimeout(f,0);setInterval(g,1)`,        { bareFree: 2 }],
    [`window.fetch("/a")`,                      { qualified: 1 }],
    [`self.requestIdleCallback(f)`,             { qualified: 1 }],
    [`window["fetch"]("/a")`,                   { computedLiteral: 1 }],
    [`var {fetch} = window`,                    { destructuredFromGlobal: 1 }],
    [`var fetch = 1; fetch("/a")`,              { bareBoundName: 1 }],
    [`function f(window){return window.fetch()}`, { qualifiedBoundGlobal: 1 }],
    [`api.fetch("/a")`,                         { instanceMember: 1 }],
    [`api["setTimeout"](f)`,                    { instanceMemberComputed: 1 }],
    [`self[k]()`,                               { globalComputedDynamic: 1 }],
    /* AND THE SILENCES. An object key, a shorthand property, a parameter, a declared function's own name and
       a default value's binding are not references to the platform, and a channel that counted one would
       report the bare spelling as commoner than it is — which reads as "the floor is harmless". */
    [`({fetch: 1, setTimeout: 2})`,             {}],
    [`function f(fetch, setInterval){}`,        {}],
    [`function setTimeout(){}`,                 {}],
    [`class C { fetch(){} }`,                   {}],
    [`x.notADeclaredName(1)`,                   {}],
    /* A DEFAULT VALUE IS AN ORDINARY EXPRESSION AND THE PARAMETER BESIDE IT IS A BINDING, which is the one
       pattern position a blanket mark would have struck out. */
    [`function f(a = fetch()){}`,               { bareFree: 1 }],
  ];
  let spSpoke = 0;
  for (const [src, want] of spWant) {
    const got = sp(src);
    for (const k of Object.keys(got)) {
      const w = want[k] || 0;
      if (got[k] !== w)
        die(`SELF-TEST FAILED: the spelling band's ${k} is ${got[k]} and not ${w} for \`${src}\`. The column ` +
            `that decides whether a member-name channel is on the critical path is a count of sites with NO ` +
            `bare reference, so a miscount in either direction inverts the verdict this band exists to give.`);
      if (got[k]) spSpoke++;
    }
  }
  if (spSpoke < SPELLINGS.length + 2)
    die(`SELF-TEST FAILED: only ${spSpoke} spelling column(s) were seen to rise; every one of the ` +
        `${SPELLINGS.length} spellings plus typeof and the unattributable computed read must be shown moving, ` +
        `or its zero over the corpus cannot be told from a column that stopped counting.`);
  for (const k of [...SPELLINGS, "typeofBare"]) {
    let rose = false;
    for (const [src] of spWant) if (sp(src)[k]) { rose = true; break; }
    if (!rose) die(`SELF-TEST FAILED: no control makes the spelling band's ${k} rise, so it is unarmed.`);
  }
  /* THE DERIVATION ITSELF IS ARMED, POSITIVE ROW FIRST AND THEN EVERY REFUSAL, because a name the engine
     declares and this pass failed to resolve would leave the population smaller than it is — the flattering
     direction for the diff being priced — and a refusal nobody has seen fire cannot be told from a shape the
     reader never met. The engine tree cannot be broken to test the walk, so the per-source reader is a pure
     function of its text and these are strings. */
  {
    const got = new Map();
    const n = entryNamesFromSource(
      `static const char A_NAME[] = "setTimeout";\n` +
      `static const char *const T_NAMES[] = { A_NAME, "requestIdleCallback", NULL };\n` +
      `rung_entry_declare(STEP_UNIT_TIMER, T_NAMES);\n` +
      `endpoint_fetch_edge_declare("fetch", steps, IDL_STEP_FIRST);\n`,
      "<selftest>", (nm, who) => got.set(nm, who));
    if (n.rung !== 1 || n.edge !== 1 || got.size !== 3 || !got.has("setTimeout") ||
        !got.has("requestIdleCallback") || !got.has("fetch"))
      die(`SELF-TEST FAILED: the entry-name derivation read ${got.size} name(s) from ${n.rung} rung and ` +
          `${n.edge} edge declaration(s) in a source carrying three, so the population this band measures is ` +
          `assembled by something other than what it is printed as reading.`);
    const refusals = [
      [`rung_entry_declare(STEP_UNIT_TIMER, NO_SUCH_TABLE);`, "a table with no declaration"],
      [`static const char *const T[] = { MISSING_ELEM, NULL };\nrung_entry_declare(STEP_UNIT_TIMER, T);`,
       "an element resolving to no literal"],
      [`static const char *const T[] = { NULL };\nrung_entry_declare(STEP_UNIT_TIMER, T);`, "an empty table"],
    ];
    for (const [bad, what] of refusals) {
      let threw = false;
      try { entryNamesFromSource(bad, "<selftest>", () => {}); } catch { threw = true; }
      if (!threw)
        die(`SELF-TEST FAILED: the entry-name derivation accepted ${what} without throwing. A shape change ` +
            `would then reduce the measured population silently, and a smaller population reads as a smaller ` +
            `floor under the very row this band is here to price.`);
    }
  }
  if (!ENTRY_NAMES.size) die(`SELF-TEST FAILED: no declared entry name was derived from the engine.`);
  for (const nm of ENTRY_NAMES)
    if (!sp(`${nm}(f)`).bareFree && !sp(`window.${nm}(f)`).qualified)
      die(`SELF-TEST FAILED: the declared name ${nm} is matched by neither spelling, so it is in the ` +
          `population and outside the channel.`);
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
  if (reachSpoke !== SELFTEST_REACH.size)
    die(`SELF-TEST FAILED: ${reachSpoke} of ${SELFTEST_REACH.size} REACH controls were judged — a control ` +
        `whose source stopped producing a door row is a control that certifies nothing.`);
  return { rows: SELFTEST.length, produced: spoke, blindRows: SELFTEST_BLIND.length, blindProduced: blindSpoke,
           manifestRows: SELFTEST_MANIFEST.length + 1, manifestProduced: manifestSpoke, manifestAddrs,
           reachRows: SELFTEST_REACH.size, spellRows: spWant.length, spellColumns: SPELLINGS.length + 2,
           spellNames: ENTRY_NAMES.size, derivationRefusals: 3 };
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
  const kindTally = () => ({ sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, guarded: 0,
                            /* THE REACH BAND, PER CLASS, so the two door classes are comparable on it —
                               which is the whole point: the question is not how deep a call sits but
                               whether the two classes sit in the SAME KIND of body. */
                            topLevel: 0, innerAsync: 0, innerSync: 0, urls: new Set() });
  const perSite = new Map();
  const bucket = (id) => {
    if (!perSite.has(id)) perSite.set(id, {
      site: id, programs: 0, bytes: 0, parsed: 0, unparsed: 0, sites: 0,
      literal: 0, folded: 0, shape: 0, opaque: 0, guarded: 0, branchAlt: 0,
      engineDoorSites: 0, nonEngineDoorSites: 0, byDoor: {}, pathish: new Set(), urls: new Set(), rows: [],
      data: kindTally(), program: kindTally(),
      argShape: {}, bindBuckets: { "1": 0, "2-5": 0, "6-20": 0, "21-100": 0, "101+": 0, "0": 0 }, oneCharNames: 0,
      blind: { sites: 0, literal: 0, folded: 0, shape: 0, opaque: 0, src: 0, href: 0 }, blindUrls: new Set(), xhrOpenSkipped: 0,
      globalDoor: { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 },
      manifest: { sites: 0, addressSites: 0, candidates: 0, dropped: 0, fragments: 0, refusedTwoApplications: 0, multi: 0 },
      manifestUrls: new Set(), manifestRows: [],
      spell: {}, spellOther: { globalComputedDynamic: 0 },
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
    for (const k of Object.keys(b.globalDoor)) b.globalDoor[k] += r.globalDoor[k];
    b.spellOther.globalComputedDynamic += r.spellOther.globalComputedDynamic;
    for (const [nm, t] of r.spell) {
      if (!b.spell[nm]) b.spell[nm] = spellTally();
      for (const k of Object.keys(t)) b.spell[nm][k] += t[k];
    }
    b.manifest.refusedTwoApplications += r.manifest.refusedTwoApplications;
    for (const m of r.manifest.rows) {
      b.manifest.sites++;
      if (m.addresses.length) b.manifest.addressSites++;
      b.manifest.candidates += m.candidates;
      b.manifest.dropped += m.dropped;
      b.manifest.fragments += m.fragments;
      if (m.addresses.length > 1) b.manifest.multi++;
      for (const u of m.addresses) b.manifestUrls.add(u);
      if (nExamples) b.manifestRows.push(m);
    }
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
      /* AN EXACT PARTITION OF `c.sites` AND NOT THREE INDEPENDENT COUNTS, asserted below at the one place
         all four are in one hand: a row is at depth 0 or it is not, and if it is not its innermost
         enclosing function is async or it is not. A reader differencing two of the three would otherwise be
         differencing quantities nothing holds together. */
      if (s.fnDepth === 0) c.topLevel++; else if (s.innerAsync) c.innerAsync++; else c.innerSync++;
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
    globalDoor: { admitted: 0, refusedBoundName: 0, declinedNonGlobalReceiver: 0 },
    manifest: { sites: 0, addressSites: 0, candidates: 0, dropped: 0, fragments: 0, refusedTwoApplications: 0, multi: 0 }, manifestDistinctUrls: 0,
    /* THE SPELLING BAND'S OWN TOTALS. `spellSites` is a presence count over SITES and never a sum of
       occurrences, because the landed rows it prices are read as a bit and a site is the unit at which one
       of them is zero. */
    spell: {}, spellOther: { globalComputedDynamic: 0 }, spellSites: {},
  };
  const allUrls = new Set(), allPathish = new Set(), allBlindUrls = new Set(), allManifestUrls = new Set();
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
      for (const k of ["sites", "literal", "folded", "shape", "opaque", "guarded",
                       "topLevel", "innerAsync", "innerSync"]) tot[cls][k] += b[cls][k];
      for (const u of b[cls].urls) clsUrls[cls].add(u);
    }
    for (const u of b.urls) allUrls.add(u);
    for (const p of b.pathish) allPathish.add(p);
    for (const k of Object.keys(tot.blind)) tot.blind[k] += b.blind[k];
    tot.xhrOpenSkipped += b.xhrOpenSkipped;
    for (const k of Object.keys(tot.globalDoor)) tot.globalDoor[k] += b.globalDoor[k];
    tot.spellOther.globalComputedDynamic += b.spellOther.globalComputedDynamic;
    for (const nm of ENTRY_NAMES) {
      const t = b.spell[nm];
      if (!tot.spell[nm]) tot.spell[nm] = spellTally();
      if (!tot.spellSites[nm])
        tot.spellSites[nm] = { any: 0, bareFree: 0, bareAny: 0, propertyAny: 0,
                               propertyOnlyTight: 0, propertyOnlyLoose: 0, wrapperOnly: 0 };
      if (!t) continue;
      for (const k of Object.keys(t)) tot.spell[nm][k] += t[k];
      /* THE PLATFORM-NAME POPULATION IS THE GLOBAL SPELLINGS AND EXCLUDES A WRAPPER'S OWN MEMBER. `api.fetch`
         names a receiver whose identity is in question, which the DOORS comment turns away and which no
         global-resolution channel of any spelling would ever raise — counting it here would inflate the
         population the member channel is being priced against with rows that channel cannot rescue. */
      const bareAny = t.bareFree + t.bareBoundName;
      const propAny = t.qualified + t.qualifiedBoundGlobal + t.computedLiteral + t.destructuredFromGlobal;
      const s = tot.spellSites[nm];
      if (bareAny || propAny) s.any++;
      if (bareAny) s.bareAny++;
      if (t.bareFree) s.bareFree++;
      if (propAny) s.propertyAny++;
      if (propAny && !bareAny) s.propertyOnlyTight++;
      if (propAny && !t.bareFree) s.propertyOnlyLoose++;
      if (!bareAny && !propAny && (t.instanceMember + t.instanceMemberComputed)) s.wrapperOnly++;
    }
    for (const k of Object.keys(tot.manifest)) tot.manifest[k] += b.manifest[k];
    for (const u of b.manifestUrls) allManifestUrls.add(u);
    for (const u of b.blindUrls) allBlindUrls.add(u);
  }
  tot.blindDistinctUrls = allBlindUrls.size;
  tot.manifestDistinctUrls = allManifestUrls.size;
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
  /* AND THE REACH BAND IS A PARTITION OF THE SAME `sites`, WHICH IS WHAT MAKES IT DIFFERENCEABLE. Without
     this a reader comparing `innerAsync` across the two classes would be comparing two numbers nothing
     holds to one denominator, and a row silently dropped out of all three would read as a class with
     fewer async sites — the flattering direction for the DATA door and the one that would make its zero in
     a run look explained. */
  for (const cls of ["data", "program"])
    if (tot[cls].topLevel + tot[cls].innerAsync + tot[cls].innerSync !== tot[cls].sites)
      die(`the ${cls} reach partition does not sum: ${tot[cls].topLevel}+${tot[cls].innerAsync}+` +
          `${tot[cls].innerSync} != ${tot[cls].sites}`);
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
      globalDoor: b.globalDoor,
      spell: b.spell, spellOther: b.spellOther,
      manifest: { ...b.manifest, distinctUrls: b.manifestUrls.size },
      manifestAddresses: nExamples ? [...b.manifestUrls].sort() : undefined,
    })),
    parseFailures: parseFail,
    examples: nExamples ? [...perSite.values()].flatMap((b) => b.rows.slice(0, nExamples)) : undefined,
    manifestExamples: nExamples ? [...perSite.values()].flatMap((b) => b.manifestRows.slice(0, 2).map((m) =>
      ({ ...m, addresses: m.addresses.slice(0, nExamples) }))) : undefined,
  };

  if (wantJson) { console.log(JSON.stringify(out, null, 1)); return; }

  const pct = (n, d) => (d ? (100 * n / d).toFixed(1) : "0.0") + "%";
  console.log(`# static_surface — WHAT A PARSE RECOVERS FROM THE JS DOOR. A CONTROL, NEVER A TARGET.`);
  console.log(`selftest ARMED: ${st.rows} controls produced ${st.produced} classified row(s); every kind and ` +
              `both destination classes exercised`);
  console.log(`         plus ${st.blindRows} blind-spot controls producing ${st.blindProduced} row(s), each ` +
              `asserted to enter NO door total`);
  console.log(`         plus ${st.manifestRows} chunk-manifest controls: ${st.manifestProduced} enumerated ` +
              `${st.manifestAddrs} address(es), the rest refused for a stated reason`);
  console.log(`         plus ${st.reachRows} REACH controls, each asserted for BOTH its function depth and ` +
              `whether its innermost enclosing function is async`);
  console.log(`         plus ${st.spellRows} SPELLING controls over ${st.spellNames} name(s) derived from the ` +
              `engine, every one of ${st.spellColumns} column(s) shown rising and every silence asserted`);
  console.log(`         plus the derivation itself: 1 positive row and ${st.derivationRefusals} refusal(s) ` +
              `shown THROWING, so a shape change cannot quietly report a smaller population`);
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
    console.log(`  reach: top-level ${k.topLevel} (${pct(k.topLevel, k.sites)})   ` +
                `inside an async fn ${k.innerAsync} (${pct(k.innerAsync, k.sites)})   ` +
                `inside a sync fn ${k.innerSync} (${pct(k.innerSync, k.sites)})`);
  };
  block(`DATA DOOR (fetch / XMLHttpRequest / sendBeacon / WebSocket / EventSource)`, tot.data,
        `Fetch §2.2.5 destinations whose reply becomes a VALUE. THIS IS THE @H PRODUCT SURFACE.`);
  block(`PROGRAM DOOR (import() / Worker / SharedWorker / importScripts)`, tot.program,
        `replies that become a PROGRAM — the page loading itself. Reported apart and never summed in.`);
  console.log(``);
  console.log(`WHAT HAS TO BE CALLED FOR A DOOR TO BE REACHED — the two classes compared on the ONE axis they`);
  console.log(`  differ on, which is NOT how deep they sit. A parse reads a call whether anything invokes its`);
  console.log(`  enclosing function or not; a RUN reaches it only if something does. So a door class whose`);
  console.log(`  calls sit at top level is reached by evaluating the program, and one whose calls sit inside an`);
  console.log(`  \`async\` body is reached only once something INVOKES that body — which in a real app is an`);
  console.log(`  effect flushed after a render commit, an event handler, a timer or an idle callback, and is a`);
  console.log(`  different question from whether the program ran at all. THE NUMBERS ARE IN THE TWO BLOCKS`);
  console.log(`  ABOVE, one \`reach:\` line each, so the comparison is read where each class's own denominator`);
  console.log(`  is. This paragraph states what the comparison MEANS and asserts nothing about any engine:`);
  console.log(`  both columns are properties of the TEXT and a run is what decides whether the invoker fires.`);
  console.log(`  IT IS A FLOOR IN ONE DIRECTION ONLY, AND THE DIRECTION IS STATED BECAUSE IT IS NOT SYMMETRIC:`);
  console.log(`  \`innerSync\` over-states reachability (a sync function nothing calls is as unreached as an`);
  console.log(`  async one) while \`innerAsync\` cannot — an async body needs an invoker by construction. So a`);
  console.log(`  high \`innerAsync\` share is evidence the class needs an invoker; a high \`innerSync\` share is`);
  console.log(`  NOT evidence that it does not, and reading it as one is the reading this note exists to stop.`);
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
  console.log(`THE GLOBAL-REACHED DOOR AND ITS PRICE — \`window.fetch(u)\` and \`new self.Worker(u)\` reach a`);
  console.log(`  platform name through the global object, and the door set reads them as the bare name because`);
  console.log(`  they ARE the bare name. These three are one trade and are printed together: a recall figure`);
  console.log(`  alone would be a widening whose precision cost nobody measured.`);
  console.log(`  admitted ${tot.globalDoor.admitted}   already counted inside the DATA and PROGRAM totals above, not added to them`);
  console.log(`  refused  ${tot.globalDoor.refusedBoundName}   the file BINDS or ASSIGNS that global name, so this pass cannot prove what it is`);
  console.log(`  declined ${tot.globalDoor.declinedNonGlobalReceiver}   a platform door name on a receiver that is not the global object — the library`);
  console.log(`           wrapper population the DOORS comment turns away, counted rather than described`);
  console.log(``);
  console.log(`HOW A REAL BUNDLE SPELLS THE NAMES THE ENGINE'S COMPILER-SIDE ROWS COUNT — the population a`);
  console.log(`  MEMBER-NAME CHANNEL would add, measured before it is built. Two engine components raise a row`);
  console.log(`  when the compiler resolves a FREE IDENTIFIER against the global object, and both record the same`);
  console.log(`  residual: the row sees that ONE spelling, so a name reached as a property of the global object`);
  console.log(`  raises nothing. Those rows are read as a BIT, so the floor costs a READING only at a site that`);
  console.log(`  spells a name ONLY as a property — which is the column to read and is the last one here.`);
  console.log(`  NOT sites, NOT endpoints, summed into no door total: these are NAME OCCURRENCES in the text.`);
  console.log(`  names derived from ${ENTRY_DECL.size} engine declaration(s), never typed here:`);
  for (const nm of [...ENTRY_NAMES].sort())
    console.log(`    ${nm.padEnd(22)} ${[...new Set(ENTRY_DECL.get(nm))].join(" ")}`);
  console.log(``);
  console.log(`  occurrences by spelling            what the landed row sees |  what a member channel would add  | wrapper`);
  console.log(`  name                     bareFree boundName typeof | qualif boundGl compLit destr | instMem instComp`);
  for (const nm of [...ENTRY_NAMES].sort()) {
    const t = tot.spell[nm] || spellTally();
    console.log(`  ${nm.padEnd(22)} ${String(t.bareFree).padStart(8)} ${String(t.bareBoundName).padStart(9)} ` +
                `${String(t.typeofBare).padStart(6)} | ${String(t.qualified).padStart(6)} ` +
                `${String(t.qualifiedBoundGlobal).padStart(7)} ${String(t.computedLiteral).padStart(7)} ` +
                `${String(t.destructuredFromGlobal).padStart(5)} | ${String(t.instanceMember).padStart(7)} ` +
                `${String(t.instanceMemberComputed).padStart(8)}`);
  }
  console.log(`  a bare reference in a file that also BINDS the name is boundName and is counted apart: the`);
  console.log(`  engine's resolver is scope-correct and very probably DOES see it, and this file-wide pass`);
  console.log(`  cannot prove which, so neither answer below is allowed to assume it. typeof is NOT a`);
  console.log(`  partition member and is not summed — every one of those is already inside one of the two bare`);
  console.log(`  columns, and it says the name was PROBED for rather than used. An alias is not a column at all:`);
  console.log(`  \`const f = fetch\` and \`const f = window.fetch\` are already counted at the spelling each used.`);
  console.log(``);
  console.log(`  AND THE SAME THING PER SITE, WHICH IS THE UNIT THE LANDED ROW IS ZERO AT. \`any\` is the sites`);
  console.log(`  whose text reaches the platform name by ANY global spelling; a wrapper's own member is excluded`);
  console.log(`  from it, because no global-resolution channel of any spelling would raise one.`);
  console.log(`  name                     sites:any  bareFree  bareAny  propAny  PROP-ONLY(tight)  (loose)  wrapperOnly`);
  for (const nm of [...ENTRY_NAMES].sort()) {
    const s = tot.spellSites[nm] || { any: 0, bareFree: 0, bareAny: 0, propertyAny: 0, propertyOnlyTight: 0, propertyOnlyLoose: 0, wrapperOnly: 0 };
    console.log(`  ${nm.padEnd(22)} ${String(s.any).padStart(9)} ${String(s.bareFree).padStart(9)} ` +
                `${String(s.bareAny).padStart(8)} ${String(s.propertyAny).padStart(8)} ` +
                `${String(s.propertyOnlyTight).padStart(17)} ${String(s.propertyOnlyLoose).padStart(8)} ` +
                `${String(s.wrapperOnly).padStart(12)}`);
  }
  console.log(`  PROP-ONLY(tight) is the population the member channel RESCUES: a site whose text spells the`);
  console.log(`  name as a property of the global object and never as a bare identifier at all, so the landed`);
  console.log(`  row reads 0 about a program that does name the entry. (loose) counts a site whose only bare`);
  console.log(`  reference sits in a file that binds the name, and is an OVER-count of the same thing — the two`);
  console.log(`  bracket it, and the spread between them is what this pass cannot decide without a scope graph.`);
  console.log(`  ${tot.spellOther.globalComputedDynamic} further read(s) of a COMPUTED member of the global object`);
  console.log(`  belong to no name at all (\`self[n]\`) and are attributed to none: a channel keyed on a property`);
  console.log(`  NAME cannot recover one either, so this is a floor under BOTH columns and not under one.`);
  {
    /* AND WHAT SUCH A CHANNEL WOULD COST, WHICH IS THE HALF A RECALL FIGURE ALONE WOULD NOT PRICE. A field-get
       emitter sees the PROPERTY NAME and, unless it also tests the receiver, cannot tell the global object's
       own member from a wrapper's or a bundler's re-export shim — and the two are not close in this corpus.
       THE ENGINE CAN MAKE THAT TEST AND THIS PASS CANNOT, which is the one place the comparison runs the other
       way: at a field get the engine holds the receiver OBJECT and can ask whether it is the realm's global,
       while a parse has only a name it must refuse to guess about. So this number is not an argument against
       the channel; it is the size of the thing the channel has to get right to be a refinement of the row
       rather than a louder and different one. */
    let g = 0, w = 0;
    for (const nm of ENTRY_NAMES) {
      const t = tot.spell[nm]; if (!t) continue;
      g += t.qualified + t.qualifiedBoundGlobal + t.computedLiteral + t.destructuredFromGlobal;
      w += t.instanceMember + t.instanceMemberComputed;
    }
    console.log(`  AND ITS PRICE: of ${g + w} property read(s) of a declared name, ${g} are on the global object`);
    console.log(`  and ${w} are on a receiver that is not it. A field-get channel that does not TEST the receiver`);
    console.log(`  raises both, so it would not refine the landed row — it would be a different and louder one.`);
    console.log(`  The engine can make that test where this pass cannot: at a field get it holds the receiver`);
    console.log(`  OBJECT and can ask whether it is the realm's global, and a parse holds only a name.`);
  }
  console.log(``);
  console.log(`THE CHUNK MANIFEST, RECOVERED — addresses a bundler emits as a MAP plus a public-path literal,`);
  console.log(`  composed in a one-parameter function and handed to an injected <script>. Every one is plain`);
  console.log(`  literal text in the file, so a PARSE has them; the door channel recovers none, because the`);
  console.log(`  composition crosses a function boundary. NOT request sites, summed into NO door total, and`);
  console.log(`  never quotable as endpoints: a manifest names every chunk the bundle COULD load and one run`);
  console.log(`  loads a few. This is the number the PROGRAM-door zeros in the table below are explained by.`);
  console.log(`  compositions ${tot.manifest.sites}, of which ${tot.manifest.addressSites} recovered an ADDRESS   ` +
              `distinct addresses ${tot.manifestDistinctUrls}   ${tot.manifest.multi} enumerate more than one`);
  console.log(`  ${tot.manifest.candidates} candidate key(s) were drawn from the functions' own bodies; ` +
              `${tot.manifest.dropped} dropped for a hole and`);
  console.log(`  ${tot.manifest.fragments} folded to a FRAGMENT — a chunk name with no public path in front of`);
  console.log(`  it, which is part of an address and is refused rather than emitted as a whole. This is also`);
  console.log(`  the precision half: without the shape test the channel called \`session\`, \`Users\` and`);
  console.log(`  \`0.001\` addresses, because indexing a string table is an i18n or enum lookup as often as`);
  console.log(`  it is a chunk manifest and nothing about the code's SHAPE tells the two apart.`);
  console.log(`  ${tot.manifest.refusedTwoApplications} composition(s) were REFUSED for holding two ` +
              `applications: two unknown parameters make the`);
  console.log(`  address set a product of two domains, and nothing here has established the two are indexed together.`);
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
  console.log(`PER SITE — the DATA door only; the program door is in --json. The last three columns are the`);
  console.log(`  REACH partition of \`data\` (top-level / inside an async fn / inside a sync fn), printed here`);
  console.log(`  because per-site is where it answers: a site's data-door count and how much of it needs an`);
  console.log(`  invoker are one reading and two numbers.`);
  console.log(`site             programs   data  literal folded  shape opaque guarded   urls  pathish   top  async   sync`);
  for (const s of out.perSite)
    console.log(`  ${s.site.padEnd(14)} ${String(s.programs).padStart(8)} ${String(s.data.sites).padStart(6)} ` +
                `${String(s.data.literal).padStart(8)} ${String(s.data.folded).padStart(6)} ${String(s.data.shape).padStart(6)} ` +
                `${String(s.data.opaque).padStart(6)} ${String(s.data.guarded).padStart(7)} ` +
                `${String(s.data.distinctUrls).padStart(6)} ${String(s.pathish).padStart(8)} ` +
                `${String(s.data.topLevel).padStart(5)} ${String(s.data.innerAsync).padStart(6)} ` +
                `${String(s.data.innerSync).padStart(6)}`);
  console.log(``);
  /* PER SITE, THE PROGRAM DOOR BESIDE THE BLIND SPOT, WHICH IS THE ONLY PLACE THE TWO CAN BE READ TOGETHER.
     A site whose program door reads 0 has either shipped no chunk loader or loaded its chunks through the
     column to its right, and a table printing one without the other cannot tell those apart. MEASURED and
     the reason this block exists: the program door is BIMODAL BY BUNDLER — a bundle that emits native
     `import()` scores in the door, and one whose bundler compiled `import()` away into a chunk-id map plus
     a `<script>` injection scores ZERO there and scores here instead. Neither zero is a statement about how
     much the site loads. */
  console.log(`PER SITE — the PROGRAM door against the blind spot it can be lost in, and the manifest it was`);
  console.log(`  lost INTO. A program-door 0 beside a nonzero b.opaq used to be the whole reading; m.urls is`);
  console.log(`  the addresses those opaque assignments were carrying, so the three columns answer together.`);
  console.log(`site             programs  prog  p.lit p.fold p.urls | blind  b.lit b.fold b.shape b.opaq  b.urls | m.sites m.urls`);
  for (const s of [...out.perSite].sort((a, b) => b.blind.sites - a.blind.sites || b.program.sites - a.program.sites))
    console.log(`  ${s.site.padEnd(14)} ${String(s.programs).padStart(8)} ${String(s.program.sites).padStart(5)} ` +
                `${String(s.program.literal).padStart(6)} ${String(s.program.folded).padStart(6)} ` +
                `${String(s.program.distinctUrls).padStart(6)} | ${String(s.blind.sites).padStart(5)} ` +
                `${String(s.blind.literal).padStart(6)} ${String(s.blind.folded).padStart(6)} ` +
                `${String(s.blind.shape).padStart(7)} ${String(s.blind.opaque).padStart(6)} ` +
                `${String(s.blind.distinctUrls).padStart(7)} | ${String(s.manifest.sites).padStart(7)} ` +
                `${String(s.manifest.distinctUrls).padStart(6)}`);
  if (parseFail.length) { console.log(``); console.log(`PARSE REFUSED (${parseFail.length}):`); for (const p of parseFail.slice(0, 10)) console.log(`  ${p}`); }
  if (nExamples) {
    console.log(``); console.log(`EXAMPLES:`);
    for (const r of out.examples) console.log(`  [${r.kind}/${r.door}/g${r.guard}] ${r.method} ${r.url.slice(0, 120)}${r.alt ? `   (alt ${r.alt.slice(0, 60)})` : ""}   ${r.file}:${r.line}`);
    console.log(``); console.log(`MANIFEST EXAMPLES (one composition, its first addresses):`);
    for (const m of out.manifestExamples) {
      console.log(`  ${m.file}:${m.line}  ${m.candidates} candidate(s), ${m.dropped} dropped, ${m.fragments} fragment(s)`);
      for (const a of m.addresses) console.log(`      ${a.slice(0, 140)}`);
    }
  }
}

main(process.argv.slice(2));
