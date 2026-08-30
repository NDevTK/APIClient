/* Spec-citation gap AUDITOR — the same job engine/idlgen.mjs does for Web IDL, done for the section numbers
 * this tree cites. It reads the REAL spec text, builds an index of what each standard actually numbers and
 * names, and DIFFS every citation in the tree against it. It PRINTS what disagrees; it does not rewrite a
 * comment and it does not generate one.
 *
 *   node engine/citegen.mjs [path …]      audit (default: engine/host + the fork's own quickjs.c/.h)
 *   node engine/citegen.mjs --all         print every finding rather than the first 120 of each kind
 *   node engine/citegen.mjs --titles      the numbers cited most often that carry no title and no known term
 *   node engine/citegen.mjs --regen [key] fetch the standard(s), rewrite engine/specindex/<key>.json
 *
 * WHY THIS EXISTS. CLAUDE.md §Browser half: a named spec with no number cannot be looked up, so it cannot
 * be checked, so it is indistinguishable from a recollection — and a WRONG number is worse than none,
 * because it reads as authoritative and sends the next reader to a section that does not say what the code
 * claims. That failure mode has NO SYMPTOM: nothing crashes, no gate goes red, and the citation looks exactly
 * like a correct one. The only thing that can catch it is the spec text itself, so this reads the spec text.
 *
 * THE CHECK THAT MATTERS IS TERM ATTRIBUTION, NOT NUMBER VALIDITY. A wrong number is almost never a number the
 * standard does not have — it is a REAL section cited for an algorithm that lives in a DIFFERENT one, and
 * a checker that only asks "does §7.3.1 exist?" answers yes and reports nothing. `determine the origin`
 * was cited as §7.3.1 "Navigables" across five files; the standard defines it under §7.3.2.1
 * "Creating browsing contexts", one heading over, and both numbers are real. So each index is keyed by that
 * standard's own definitions — every term it defines, filed under the section whose heading it sits
 * beneath — and a citation that names an algorithm is checked against where that algorithm IS.
 *
 * A ONE-STANDARD CHECKER IS A FALSE-ATTRIBUTION ENGINE, AND THAT IS NOT A COVERAGE GAP — IT IS A WRONG ANSWER
 * IN THE EXACT SHAPE THIS FILE EXISTS TO END. The first version indexed HTML alone and resolved an unanchored
 * §N by the FILE'S DOMINANT ANCHOR: whichever standard the file names most often decides every citation in it
 * that names none. That rule is a guess wearing an answer's clothes, and it was measured wrong twice in one
 * day. `solver/dom_cow.c` is HTML-dominant and cites DOM constantly, so its five `§4.9 "get an attribute by
 * name"` citations — DOM §4.9 "Interface Element", correct as written — were reported as belonging to HTML
 * §4.9 "Tabular data". And `qjs/quickjs.c`, the most ECMAScript-dense file in the tree, anchors HTML 24 times
 * and ECMAScript 12, so the fallback declared the JavaScript engine an HTML file and judged 58 citations of
 * ECMAScript §7.4.9 against a number the HTML standard does not have.
 *
 * SO A CITATION IS RESOLVED BY ITS OWN EVIDENCE, ACROSS EVERY INDEXED STANDARD, AND THE FILE VOTE IS GONE.
 * The trailing phrase is looked up in EVERY index at once and the longest match wins; the standards that
 * define that phrase are the candidates, and the citation is a finding only when NO candidate places the term
 * at the cited number. The claim the tool then makes is strictly true and carries its own proof — "no indexed
 * standard defines `about base url` at §7.4" — where the old claim ("HTML does not") was true of the index and
 * silent about the world. An unanchored citation whose phrase no standard defines is UNDECIDED, counted and
 * never asserted about. The cost is recall, and it is the right trade: a checker that cries wolf gets muted,
 * and a muted checker is worse than none.
 *
 * A TERM IS DEFINED IN ONE SECTION AND USED IN ANOTHER, AND A READER MAY CITE EITHER. `is initial about:blank`
 * is DEFINED in HTML §3.1.1 "The Document object" and CLEARED in §7.4.4; a comment about the clearing that
 * cites §7.4.4 is right, and a definition-site-only index calls it wrong. So the index records USES as well as
 * definitions: a section that LINKS a term at least USE_FLOOR times is a section that is about that term, and
 * a citation landing there is confirmed rather than reported. Prominence is the gate — one passing mention is
 * not a subject — and the two facts stay separate in the index, so a finding can say which one it is missing.
 *
 * THAT USE SCAN IS INTRA-STANDARD ONLY, AND THE CROSS-STANDARD HALF OF IT IS REFUSED RATHER THAN DEFERRED. The
 * proposal that stood here was to store per index an `ids` map (dfn id -> term) and an `xuses` map filled by the
 * same href walk, then let `probe` return a hit for a standard that prominently USES a phrase as well as one
 * that defines it — so that a comment naming another standard's concept while citing its own section would be
 * confirmed instead of reported. Its worked example was thirteen event files reading `§2.2's constructor steps`,
 * called correct-as-written and owed an apology. THAT EXAMPLE WAS MEASURED AND BOTH HALVES OF IT WERE FALSE.
 *   — THE CITATIONS WERE WRONG. DOM §2.2 "Interface Event" carries the interface's IDL block and its attribute
 *     definitions and no steps at all; the algorithm those comments run is DOM §2.5 "Constructing events" —
 *     "When a constructor of the Event interface, or of an interface that inherits from the Event interface, is
 *     invoked, these steps must be run" — which is also where `create an event` and the `event constructing
 *     steps` hook are defined. The confirmation would have SILENCED SIXTEEN REAL MISATTRIBUTIONS: precisely the
 *     direction the proposal itself named as the one where a mistake costs a finding, arriving through its own
 *     motivating case, which is why a confirmation channel is argued from a READ population and never from a
 *     plausible one.
 *   — AND IT WOULD NOT HAVE FIRED ANYWAY. DOM writes `constructor steps` as PLAIN TEXT nine times and LINKS Web
 *     IDL's definition of it ZERO times, so an href walk records nothing for that phrase in any DOM section. A
 *     confirmation channel that cannot see its own motivating case is not a narrower version of the right
 *     mechanism, it is a different one — and its silences would have landed where nobody had looked.
 * WHAT IS REAL IS AN ASYMMETRY IN THE RESOLVER RATHER THAN A GAP IN THE INDEX, and it is where any later attempt
 * has to earn its bar. `!owned` — the filter below that refuses to accuse a standard the phrase's definers do
 * not include — is UNREACHABLE for a citation resolved BY ITS TERM, because that resolution picked the standard
 * precisely BECAUSE it defines the phrase, so `owned` is true by construction. An ANCHORED citation therefore
 * gets a soundness check that a TERM-RESOLVED one — which carries strictly less evidence about which standard it
 * is — does not, and term resolution is where the findings live: 472 of 534 at the revision this was measured.
 * The bar a cross-standard channel must clear is NOT "does some standard prominently use this phrase at this
 * number", because a number collision across fifteen standards is cheap; it is whether the tool can NAME the
 * section it believes is right. Here it could not have: DOM does not define `constructor steps` anywhere. The
 * PHRASE belongs to Web IDL and only the ALGORITHM belongs to DOM, and an index keyed by phrases cannot state
 * that. Its absence shows as a finding whose diagnosis names the phrase's owner while the number's owner — the
 * standard the comment is actually about — goes unnamed, which is a true report a reader must finish by hand.
 *
 * WHAT IS CHECKABLE OFFLINE. Every index is COMMITTED and the audit never touches the network, for idlgen.mjs's
 * reason: a build must work with no network, and fetching a hundred spec pages per run is a gate measuring the
 * WHATWG's uptime. --regen is the one command that fetches, and it curls rather than using node's
 * fetch, because curl is what carries this environment's proxy configuration and is what an agent
 * verifying a section number is already required to use. The cost of vendoring is STALENESS, and staleness is
 * then a CHECKABLE fact rather than an invisible one: each index records the standard's own "Last Updated"
 * line and the date it was fetched, the audit prints both in its header, and --regen prints every section
 * whose NUMBER MOVED since the committed index — which is exactly the renumbering hazard §Browser
 * half names as the reason a citation carries its title beside its number.
 *
 * REPORT, NOT FAIL — a departure from idlgen.mjs, argued rather than inherited. idlgen's finding is a
 * MISSING BROWSER CAPABILITY: red is correct, because the tree cannot be right until someone writes C. A
 * citation is prose. A build that fails on a comment is a build in which no lane can land a spelling fix, and
 * it turns every stale line into a stop-the-world event for whoever next touches that directory — which is
 * how a checker gets muted. So the exit code is 0 and the findings are the output. What keeps that honest is
 * §Testing's discipline for any gate: the count is reported beside the revision it was measured at.
 *
 * FIVE MARKUP FAMILIES, ONE INDEX SHAPE. The standards this tree cites are generated by five different
 * pipelines and each needs its own reader, but all five produce the same {sections, dfns, uses} index, so the
 * audit knows nothing about any of them. Adding a standard is a row in SPECS plus one curl — WHERE A READER
 * ALREADY EXISTS. Where one does not, the row is worth writing anyway rather than leaving the standard
 * counted-and-unchecked, because a generator is a bounded known-answer parse and a silent zero is not.
 *   — whatwg-multipage (HTML): §-numbered <h2>-<h6> across ~57 pages, terms as <dfn>.
 *   — bikeshed (DOM, URL, Fetch, Streams, Web IDL, IndexedDB, CSSOM, CSSOM View, CSP, XHR, File API): one
 *     page, `data-level` on the heading, terms as <dfn>. The W3C-hosted ones are the same generator, so they
 *     need no reader of their own — only their own row.
 *   — respec (Permissions): one page, the number in a <bdi class=secno> inside the heading, terms as <dfn>.
 *   — xmlspec (XML 1.0): a 2008 Recommendation, so nothing in it will ever renumber; the number is the
 *     heading's own leading token and a definition is a titled <a name=dt-…> rather than a <dfn>.
 *   — tc39-multipage (ECMAScript): §-numbered <h1> per emu-clause. This one is structurally different in a
 *     way that matters: ECMAScript defines almost nothing with <dfn> — abstract-operations.html carries FOUR
 *     on a page holding a hundred and fourteen clauses — because in ECMAScript the ALGORITHM IS THE CLAUSE.
 *     `IteratorClose` is not a dfn anywhere; it is the title of §7.4.11 and the `aoid` of its <emu-clause>.
 *     So this reader's terms come from clause TITLES and aoids, and it is the one index that may hold a
 *     SINGLE-WORD term — `IteratorClose`, `ToPrimitive`, `[[Get]]` — because an ECMAScript operation name is
 *     an identifier the standard owns, not a common noun. The identifier test is what separates those from
 *     `Scope`, `Conformance` and `Objects`, which are chapter headings and would match everything.
 *
 * THE THREE THINGS THAT MADE THIS REPORT NOISE, EACH FIXED AT ITS ROOT AND EACH WORTH KEEPING AS A SHAPE:
 *   — A dfn whose whole content is a link OUT to another standard is an IMPORT, not a definition.
 *     HTML §2.1.9 "Dependencies" re-exports several hundred CSS and DOM terms that way, and indexing them made
 *     one HTML section the answer to every CSS citation in the tree.
 *   — A number-does-not-exist check fires on ABSENCE, which is what a MIS-RESOLVED citation produces, so it
 *     is asked only of an explicitly anchored citation. Term attribution fires on a positive match against a
 *     phrase the standard defines, which is its own evidence about which standard the comment is discussing.
 *   — A parse is checked against the PAGE, never against a floor. The first version of the TOC parse
 *     dropped every section whose title wraps across a line — HTML §4.8.5 "The iframe element",
 *     §7.4, §4.13.2 — and sailed past a "did we get at least 500" guard while the audit reported
 *     "HTML has no §4.8.5" for 65 correct citations. A floor a broken parse passes is not a check. The same
 *     check caught the tc39 reader dropping §13.9.2 (a `>` inside a quoted title attribute ends an unquoted
 *     attribute scan) and every Annex (`<span class=secnum>Annex A <span class=annex-kind>…`).
 *
 * A CITATION IS NOT ALWAYS SPELLED WITH A §, AND THE ONE THAT IS NOT IS WHERE THE ERRORS WERE. quickjs.c
 * writes `7.4.9 IteratorClose`, never `§7.4.9` — 58 times, and the §-only reader saw NONE of them. A bare
 * dotted number cannot be admitted on sight (`0.0` is a double, `1.5` is a factor, `13.2` is a version), so it
 * is admitted BY GROUP EVIDENCE: a bare number is a citation when SOME OTHER occurrence of that same number
 * in that same file is followed by a phrase an indexed standard defines. `7.4.9` earns admission because 37
 * of its 58 sites name an ECMAScript operation; `1.5` earns nothing and is never looked at. Bare numbers are
 * read only out of PROSE — comment bodies and string literals, the two places a citation can live — so a
 * float literal and an array bound are not candidates in the first place.
 *
 * AND THE SITES THAT CARRY NO TERM ARE REPORTED AS WHAT THEY ARE: UNDECIDED, NOT WRONG. Of quickjs.c's 58,
 * twenty-one say only `7.4.9's close` or `7.4.9 step 2` — no operation name, nothing to attribute. The tool
 * cannot decide those and does not pretend to; it reports them as sharing a number whose other occurrences ARE
 * diagnosed, which is a fact about the file and not a guess about the line, and it is exactly what a human
 * needs to know to go read them. Inventing a target for those would be the wrong-citation defect committed by
 * the instrument built to find it. */
import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, mkdirSync, readdirSync, statSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, relative } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);
const INDEX_DIR = join(HERE, "specindex");

/* A section that LINKS a term this many times is a section the term is ABOUT. One passing reference is not a
 * subject, and treating it as one would confirm every citation of every chapter that mentions anything. */
const USE_FLOOR = 3;

/* THE REGISTRY. Everything standard-specific is here; the audit below reads only {sections, dfns, uses}.
 * `anchors` are the names this tree writes in front of a §, lowercased. A standard NOT listed here is not
 * audited — its citations are counted and named in the report so the blind spot is printed rather than
 * assumed to be zero. */
const SPECS = [
  { key: "html", label: "HTML Living Standard", kind: "whatwg-multipage",
    base: "https://html.spec.whatwg.org/multipage/", anchors: ["html", "htmls"] },
  { key: "ecmascript", label: "ECMAScript Language Specification", kind: "tc39-multipage",
    base: "https://tc39.es/ecma262/multipage/",
    anchors: ["ecmascript", "ecma", "ecma262", "ecma-262", "es", "tc39", "js"] },
  { key: "dom", label: "DOM Standard", kind: "bikeshed",
    base: "https://dom.spec.whatwg.org/", anchors: ["dom"] },
  { key: "url", label: "URL Standard", kind: "bikeshed",
    base: "https://url.spec.whatwg.org/", anchors: ["url"] },
  { key: "fetch", label: "Fetch Standard", kind: "bikeshed",
    base: "https://fetch.spec.whatwg.org/", anchors: ["fetch"] },
  /* STREAMS EARNED ITS ROW BY BEING THE BLIND SPOT THAT COST A HAND-AUDIT. `core/streams/pipe.c` cited
     §4.2.4 for ReadableStreamPipeTo at twenty-three sites — every stage label, every DCHECK and the
     `algorithm` string — and the operation is defined at §4.9.1 "Working with readable streams"; §4.2.4
     "Constructor, methods, and properties" is where `pipeTo` and `pipeThrough` are and it only CALLS it. The
     audit reported NOTHING, because an unindexed standard's citations are counted under OTHER_SPECS and never
     checked, and 226 of them were. That is the coverage loss this file's own header says is printed rather
     than assumed to be zero — and printing it is what made someone read the number. */
  { key: "streams", label: "Streams Standard", kind: "bikeshed",
    base: "https://streams.spec.whatwg.org/", anchors: ["streams"] },
  /* THE EIGHT BELOW WERE COUNTED AND NEVER CHECKED, WHICH READS EXACTLY LIKE A CLEAN BILL AND IS A SILENT ZERO.
     Streams proved the size of that: the run before its row reported 226 citations under OTHER_SPECS, the run
     after audited 928 and raised 26 misattributions that were not new — they were newly SEEN. Every standard
     here was in the same state, and the biggest of them is the one whose correctness matters most, since Web
     IDL is the spec of every member's argument handling in this engine.
     THE NAME A CITATION WRITES IS NOT ALWAYS THE NAME THE INDEX IS KEYED BY, so `anchors` carries the tree's
     own spellings and `key` stays a short file name. `cssomview` and `fileapi` are keyed apart from the
     one-word `view` and `api` deliberately — see anchorTokens for why the last word of a multi-word name is
     the part that collides. */
  { key: "idl", label: "Web IDL Standard", kind: "bikeshed",
    base: "https://webidl.spec.whatwg.org/", anchors: ["idl", "webidl", "web idl"] },
  /* `database` alone is an English word this tree writes in prose, so this standard is anchored ONLY by the
     two-word name it is actually cited under. Every one of its citations spells it that way. */
  { key: "database", label: "Indexed Database API", kind: "bikeshed",
    base: "https://w3c.github.io/IndexedDB/", anchors: ["indexed database", "indexeddb"] },
  { key: "cssomview", label: "CSSOM View Module", kind: "bikeshed",
    base: "https://drafts.csswg.org/cssom-view/", anchors: ["cssom view", "cssom-view"] },
  { key: "cssom", label: "CSS Object Model (CSSOM)", kind: "bikeshed",
    base: "https://drafts.csswg.org/cssom/", anchors: ["cssom"] },
  { key: "csp", label: "Content Security Policy Level 3", kind: "bikeshed",
    base: "https://w3c.github.io/webappsec-csp/", anchors: ["csp"] },
  { key: "xhr", label: "XMLHttpRequest Standard", kind: "bikeshed",
    base: "https://xhr.spec.whatwg.org/", anchors: ["xhr", "xmlhttprequest"] },
  { key: "fileapi", label: "File API", kind: "bikeshed",
    base: "https://w3c.github.io/FileAPI/", anchors: ["file api", "fileapi"] },
  { key: "permissions", label: "Permissions", kind: "respec",
    base: "https://w3c.github.io/permissions/", anchors: ["permissions"] },
  { key: "xml", label: "Extensible Markup Language (XML) 1.0 (Fifth Edition)", kind: "xmlspec",
    base: "https://www.w3.org/TR/xml/", anchors: ["xml"] },
];
const SPEC_BY_KEY = new Map(SPECS.map((s) => [s.key, s]));
const indexFileOf = (key) => join(INDEX_DIR, key + ".json");

/* ---- shared text normalization -------------------------------------------------------------------------- */

const ENTITIES = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ", "#39": "'", "#x27": "'" };

function decodeEntities(s) {
  return s.replace(/&(#x?[0-9a-fA-F]+|[a-zA-Z]+);/g, (m, e) => {
    const k = e.toLowerCase();
    if (ENTITIES[k] !== undefined) return ENTITIES[k];
    if (k[0] === "#") return String.fromCodePoint(parseInt(k[1] === "x" ? k.slice(2) : k.slice(1), k[1] === "x" ? 16 : 10));
    return m;
  });
}

function stripTags(s) {
  return decodeEntities(String(s).replace(/<[^>]*>/g, " ")).replace(/\s+/g, " ").trim();
}

/* The spec's markup, a citation's prose and a citation's quoted title must all reduce to ONE spelling or a
 * comparison between them means nothing. Curly quotes, hyphens standing in for spaces (`determine-the-origin`
 * for `determine the origin`), possessive `'s`, and the spec's own <code>/<var> wrappers all differ from the
 * plain phrase while naming the same thing. */
function normTerm(s) {
  return decodeEntities(String(s).replace(/<[^>]*>/g, " "))
    .replace(/[‘’“”]/g, "'")
    .replace(/[‐-―]/g, "-")
    .toLowerCase()
    .replace(/[-_/]+/g, " ")
    .replace(/'s\b/g, "")
    .replace(/[^a-z0-9 ()[\]@.]+/g, " ")
    /* A SENTENCE-FINAL PERIOD IS PUNCTUATION; A PERIOD BETWEEN TWO CHARACTERS IS PART OF A NAME. The dot has
     * to survive `Array.prototype.map`, so it cannot simply be stripped — and while it survived, the citation
     * `URL §5.2 application/x-www-form-urlencoded serializing.` did not match that section's own title,
     * because its last word was `serializing.` and the standard's was `serializing`. A correct, fully-titled
     * citation was reported as a misattribution over one character. The guard is what the two cases differ in:
     * a period after an alphanumeric and before whitespace ends a sentence; every other period is a name's. */
    .replace(/(?<=[a-z0-9])\.(?=\s|$)/g, "")
    .replace(/\s+/g, " ")
    .trim();
}

/* Section numbers sort component-wise, and ECMAScript's annexes make a component a LETTER (`B.3.2`), so the
 * comparison must not assume a number and silently order every annex as zero. */
function cmpNo(a, b) {
  const x = String(a).split("."), y = String(b).split(".");
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const p = x[i] === undefined ? "" : x[i], q = y[i] === undefined ? "" : y[i];
    const np = /^\d+$/.test(p), nq = /^\d+$/.test(q);
    if (np && nq) { const d = +p - +q; if (d) return d; }
    else if (p !== q) return p < q ? -1 : 1;
  }
  return 0;
}

/* ---- --regen: read the standards ------------------------------------------------------------------------ */

function curl(url) {
  return execFileSync("curl", ["-sSL", "--fail", "--max-time", "180", url], {
    encoding: "utf8", maxBuffer: 96 * 1024 * 1024,
  });
}

function attrHref(raw) {
  return String(raw || "").replace(/^["']|["']$/g, "");
}

function attrOf(attrs, name) {
  const m = new RegExp(name + "=(\"[^\"]*\"|'[^']*'|[^ >]+)").exec(attrs);
  return m ? attrHref(m[1]) : null;
}

/* Terms and uses are accumulated into these two maps by every reader, so the three readers differ only in how
 * they find a heading and a definition. `defs` is term -> [section …]; `uses` counts (term, section) pairs and
 * is filtered by USE_FLOOR at the end. */
function addDef(defs, term, sec) {
  const list = defs.get(term) || [];
  if (!list.includes(sec)) list.push(sec);
  defs.set(term, list);
}

function secAt(marks, at) {
  let lo = 0, hi = marks.length - 1, sec = null;
  while (lo <= hi) { const mid = (lo + hi) >> 1; if (marks[mid].at < at) { sec = marks[mid].no; lo = mid + 1; } else hi = mid - 1; }
  return sec;
}

/* A DEFINITION is a claim; a LINK to one is a use. Both standards' generators emit the link as an href ending
 * in the definition's own id, so one scan over hrefs, attributed to the nearest preceding heading, gives every
 * section that talks about a term. */
function scanUses(body, marks, idToTerm, uses) {
  const re = /href=("[^"]*"|'[^']*'|[^ >]+)/g;
  for (let m; (m = re.exec(body)); ) {
    const h = attrHref(m[1]);
    const hash = h.indexOf("#");
    if (hash < 0) continue;
    const term = idToTerm.get(h.slice(hash + 1));
    if (!term) continue;
    const sec = secAt(marks, m.index);
    if (!sec) continue;
    const k = term + "\u0000" + sec;
    uses.set(k, (uses.get(k) || 0) + 1);
  }
}

/* A dfn whose whole content is a link OUT to another standard is an IMPORT, not a definition. HTML §2.1.9
 * "Dependencies" re-exports several hundred terms that other standards own — `computed value`,
 * `containing block` — each as a <dfn> holding nothing but an absolute link. Indexing those filed half of CSS
 * under one HTML section and made it the answer to every CSS citation in the tree (measured: 472 findings). */
const IMPORTED = /^\s*<a [^>]*href=["']?https?:[\s\S]*<\/a>\s*$/;

/* A ONE-WORD term is not a citation check, it is a coincidence generator: `origin`, `document` and `container`
 * are defined by half the platform and appear in every other comment in this tree. Only ECMAScript overrides
 * this, and only for an operation NAME, which is an identifier rather than a common noun. */
function keepTerm(term) {
  return term.split(" ").length >= 2 && term.length <= 90;
}

function writeIndex(spec, out) {
  const prev = existsSync(indexFileOf(spec.key)) ? JSON.parse(readFileSync(indexFileOf(spec.key), "utf8")) : null;
  mkdirSync(INDEX_DIR, { recursive: true });
  writeFileSync(indexFileOf(spec.key), JSON.stringify(out, null, 1) + "\n");
  console.log(`\n${spec.key}: ${Object.keys(out.sections).length} sections, ${Object.keys(out.dfns).length} terms, ` +
    `${Object.keys(out.uses).length} terms with a prominent use site — updated ${out.specUpdated}`);
  if (!prev) return;
  /* THE RENUMBERING REPORT — the hazard §Browser half names, made visible. A section whose TITLE stayed and
   * whose NUMBER moved is exactly the citation that silently goes wrong, so it is named here rather than
   * discovered by a reader following it. */
  const byId = new Map();
  for (const [no, s] of Object.entries(prev.sections)) if (s.id) byId.set((s.page || "") + "#" + s.id, no);
  let moved = 0;
  for (const [no, s] of Object.entries(out.sections)) {
    const was = s.id ? byId.get((s.page || "") + "#" + s.id) : null;
    if (was && was !== no) { console.log(`  RENUMBERED  §${was} -> §${no}  "${s.title}"`); moved++; }
  }
  console.log(`  ${moved} section(s) renumbered since the committed index (fetched ${prev.fetched})`);
}

function finish(spec, sections, defs, uses, specUpdated) {
  /* The use map keeps the COUNT, not a boolean, because two different questions are asked of it and merging
   * them would answer both badly: USE_FLOOR links make a section the term's SUBJECT, so a citation landing
   * there is CONFIRMED; a section that links it merely twice is worth NAMING IN THE FINDING, so the reader
   * can see for themselves whether that mention is the one the comment meant. One mention is noise. */
  const kept = {};
  for (const [k, n] of uses) {
    if (n < 2) continue;
    const i = k.indexOf("\u0000");
    const term = k.slice(0, i), sec = k.slice(i + 1);
    if (!defs.has(term)) continue;                 /* a use of something this index does not define is noise */
    if (defs.get(term).includes(sec)) continue;    /* the definition site is already the answer */
    (kept[term] = kept[term] || {})[sec] = n;
  }
  return {
    spec: spec.label, key: spec.key, base: spec.base, specUpdated: (specUpdated || "").trim(),
    fetched: new Date().toISOString().slice(0, 10),
    sections: Object.fromEntries([...sections].sort((a, b) => cmpNo(a[0], b[0]))),
    dfns: Object.fromEntries([...defs].sort((a, b) => (a[0] < b[0] ? -1 : 1))),
    uses: Object.fromEntries(Object.entries(kept).sort((a, b) => (a[0] < b[0] ? -1 : 1))),
  };
}

/* ---- reader: WHATWG multipage (HTML) --------------------------------------------------------------------- */

function regenWhatwgMultipage(spec) {
  const toc = curl(spec.base);
  const updated = (/Last Updated\s*<span class=pubdate>([^<]*)</.exec(toc) || [])[1];
  if (!updated) throw new Error(`${spec.key}: the TOC's pubdate did not parse — the index would record no staleness fact`);

  const sections = new Map();
  const pages = new Set();
  const tocRe = /<a href=("[^"]*"|'[^']*'|[^ >]+)><span class=secno>([\d.]+)<\/span>([\s\S]*?)<\/a>/g;
  let parsed = 0;
  for (let m; (m = tocRe.exec(toc)); ) {
    parsed++;
    const href = attrHref(m[1]);
    const page = href.split("#")[0];
    const title = stripTags(m[3]);
    const had = sections.get(m[2]);
    /* Each top-level chapter is listed twice — once in the page's own nav, once in the tree — so a repeat is
     * expected. A repeat that DISAGREES about the title is a parse that has run off the end of an anchor. */
    if (had && had.title !== title) throw new Error(`§${m[2]} parsed as both "${had.title}" and "${title}" — the parse is wrong`);
    if (!had || (!had.page && page)) sections.set(m[2], { title, page, id: href.split("#")[1] || "" });
    if (page) pages.add(page);
  }
  /* THE PARSE IS CHECKED AGAINST THE PAGE, NOT AGAINST A FLOOR — see the header. Every secno span in the TOC
   * is one section; if the count differs, the parse is wrong and the index must not be written. */
  const secnos = (toc.match(/<span class=secno>/g) || []).length;
  if (parsed !== secnos) throw new Error(`${spec.key}: the TOC has ${secnos} secno spans and the parse read ${parsed} — the parse is wrong`);

  const defs = new Map(), uses = new Map(), idToTerm = new Map(), bodies = [];
  const sorted = [...pages].sort();
  for (const page of sorted) {
    const body = curl(spec.base + page);
    const marks = [];
    const headRe = /<h[2-6][^>]*>\s*<span class=secno>([\d.]+)<\/span>/g;
    for (let m; (m = headRe.exec(body)); ) marks.push({ at: m.index, no: m[1] });
    const dfnRe = /<dfn\b([^>]*)>([\s\S]{0,400}?)<\/dfn>/g;
    for (let m; (m = dfnRe.exec(body)); ) {
      const sec = secAt(marks, m.index);
      if (!sec) continue;
      if (IMPORTED.test(m[2])) continue;
      const term = normTerm(m[2]);
      if (!keepTerm(term)) continue;
      addDef(defs, term, sec);
      const id = attrOf(m[1], "id");
      if (id) idToTerm.set(id, term);
    }
    bodies.push({ page, body, marks });
    process.stderr.write(`  ${page}: ${marks.length} headings\n`);
  }
  for (const b of bodies) scanUses(b.body, b.marks, idToTerm, uses);
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
}

/* ---- reader: Bikeshed single page (DOM, URL, Fetch) ------------------------------------------------------ */

function regenBikeshed(spec) {
  const body = curl(spec.base);
  const updated = (/<time class="dt-updated"[^>]*>([^<]*)<\/time>/.exec(body) || [])[1];
  if (!updated) throw new Error(`${spec.key}: no dt-updated — the index would record no staleness fact`);

  const sections = new Map();
  const marks = [];
  const headRe = /<h([1-6])[^>]*\sdata-level="([\d.]+)"[^>]*>([\s\S]*?)<\/h\1>/g;
  let parsed = 0;
  for (let m; (m = headRe.exec(body)); ) {
    parsed++;
    const inner = m[3];
    const c = /<span class="content">([\s\S]*?)<\/span>/.exec(inner);
    const title = stripTags(c ? c[1] : inner.replace(/<span class="secno">[\s\S]*?<\/span>/, ""));
    const id = (/\sid="([^"]*)"/.exec(m[0].slice(0, m[0].indexOf(">"))) || [])[1] || "";
    const no = m[2];
    if (!sections.has(no)) sections.set(no, { title, page: "", id });
    marks.push({ at: m.index, no });
  }
  /* Same check, same reason: `data-level` appears on exactly the numbered headings, so a parse that reads
   * fewer of them than the page carries is a parse that must not be committed. */
  const levels = (body.match(/\sdata-level="[\d.]+"/g) || []).length;
  if (parsed !== levels) throw new Error(`${spec.key}: the page has ${levels} data-level headings and the parse read ${parsed} — the parse is wrong`);
  marks.sort((a, b) => a.at - b.at);

  const defs = new Map(), uses = new Map(), idToTerm = new Map();
  const dfnRe = /<dfn\b([^>]*)>([\s\S]{0,400}?)<\/dfn>/g;
  for (let m; (m = dfnRe.exec(body)); ) {
    const sec = secAt(marks, m.index);
    if (!sec) continue;
    if (IMPORTED.test(m[2])) continue;
    /* Bikeshed carries a dfn's other spellings in data-lt, pipe-separated — `attribute list|attribute lists`.
     * A comment naming the plural is naming the same definition, so every spelling is indexed. */
    const spellings = [normTerm(m[2])];
    const lt = attrOf(m[1], "data-lt");
    if (lt) for (const alt of lt.split("|")) spellings.push(normTerm(alt));
    let primary = null;
    for (const t of spellings) { if (!keepTerm(t)) continue; addDef(defs, t, sec); if (!primary) primary = t; }
    const id = attrOf(m[1], "id");
    if (id && primary) idToTerm.set(id, primary);
  }
  scanUses(body, marks, idToTerm, uses);
  console.error(`  ${spec.key}: ${marks.length} headings on one page`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
}

/* ---- reader: ReSpec single page (Permissions) ------------------------------------------------------------ */

/* ReSpec numbers a heading with a <bdi class=secno> INSIDE the heading and carries no `data-level` anywhere, so
 * the bikeshed reader reads zero sections from it — which is why this standard sat under OTHER_SPECS. The same
 * <bdi> also appears in every table-of-contents entry (100 of them against 49 real headings), so the scan is
 * anchored to an <hN> opening exactly as the tc39 reader is anchored to <h1>: the TOC's copies live in <a>. */
function regenRespec(spec) {
  const body = curl(spec.base);
  /* ReSpec states the draft's date as dt-PUBLISHED, not dt-updated — an editor's draft is republished rather
   * than amended in place, so that IS its staleness fact and there is no other. */
  const updated = (/<time class="dt-published"[^>]*>([^<]*)<\/time>/.exec(body) || [])[1];
  if (!updated) throw new Error(`${spec.key}: no dt-published — the index would record no staleness fact`);

  const sections = new Map(), marks = [];
  const headRe = /<h([1-6])([^>]*)>\s*<bdi class="secno">([^<]*)<\/bdi>([\s\S]*?)<\/h\1>/g;
  let parsed = 0;
  for (let m; (m = headRe.exec(body)); ) {
    parsed++;
    const no = stripTags(m[3]).replace(/\.$/, "").trim();
    const title = stripTags(m[4]);
    const had = sections.get(no);
    if (had && had.title !== title) throw new Error(`§${no} parsed as both "${had.title}" and "${title}" — the parse is wrong`);
    if (!had) sections.set(no, { title, page: "", id: attrOf(m[2], "id") || "" });
    marks.push({ at: m.index, no });
  }
  /* THE PAGE'S OWN COUNT OF HEADING OPENINGS, not a floor: a scan that runs off the end of one heading swallows
   * the next, so parsed < openings is exactly the failure this catches and the number is read off the page. */
  const opens = (body.match(/<h[1-6][^>]*>\s*<bdi class="secno">/g) || []).length;
  if (parsed !== opens) throw new Error(`${spec.key}: the page has ${opens} numbered heading openings and the parse read ${parsed} — the parse is wrong`);
  marks.sort((a, b) => a.at - b.at);

  const defs = new Map(), uses = new Map(), idToTerm = new Map();
  const dfnRe = /<dfn\b([^>]*)>([\s\S]{0,400}?)<\/dfn>/g;
  for (let m; (m = dfnRe.exec(body)); ) {
    const sec = secAt(marks, m.index);
    if (!sec || IMPORTED.test(m[2])) continue;
    /* ReSpec spells alternates in data-lt with the same pipe separator bikeshed uses, and adds data-local-lt
     * for the short form a section uses internally (`state` for `permission states`). Both name the same
     * definition, so both are indexed. */
    const spellings = [normTerm(m[2])];
    for (const a of ["data-lt", "data-local-lt", "data-plurals"]) {
      const v = attrOf(m[1], a);
      if (v) for (const alt of decodeEntities(v).split("|")) spellings.push(normTerm(alt));
    }
    let primary = null;
    for (const t of spellings) { if (!keepTerm(t)) continue; addDef(defs, t, sec); if (!primary) primary = t; }
    const id = attrOf(m[1], "id");
    if (id && primary) idToTerm.set(id, primary);
  }
  scanUses(body, marks, idToTerm, uses);
  console.error(`  ${spec.key}: ${marks.length} numbered headings on one page`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
}

/* ---- reader: xmlspec (XML 1.0) --------------------------------------------------------------------------- */

/* The oldest generator this tree cites and the simplest: a heading is `<h3><a name=… id=… />2.7 CDATA
 * Sections</h3>`, so the NUMBER IS PART OF THE HEADING TEXT and an unnumbered heading is a grammar production
 * (`Document`, `Character Range`) rather than a section. Those must not become marks — a production heading
 * inside §2.2 would otherwise shadow §2.2 for every definition below it. A DEFINITION is a titled anchor,
 * `<a name="dt-cdsection" id="dt-cdsection" title="CDATA Section">Definition</a>`, and every reference to it is
 * an href to that id, which is exactly the shape scanUses already reads. */
const XML_NUMBERED = /^([0-9]+(?:\.[0-9]+)*|[A-Z](?:\.[0-9]+)*)\s+(.+)$/;

function regenXmlspec(spec) {
  const body = curl(spec.base);
  const updated = (/<h2>[^<]*<a name="w3c-doctype"[^>]*\/>([^<]*)<\/h2>/.exec(body) || [])[1];
  if (!updated) throw new Error(`${spec.key}: no W3C doctype heading — the index would record no staleness fact`);

  const sections = new Map(), marks = [];
  const headRe = /<h([2-6])[^>]*>\s*<a name="([^"]*)"[^>]*\/>\s*([^<]*)<\/h\1>/g;
  let parsed = 0;
  for (let m; (m = headRe.exec(body)); ) {
    parsed++;
    const t = stripTags(m[3]);
    const s = XML_NUMBERED.exec(t);
    if (!s) continue;
    if (!sections.has(s[1])) sections.set(s[1], { title: s[2], page: "", id: m[2] });
    marks.push({ at: m.index, no: s[1] });
  }
  const opens = (body.match(/<h[2-6][^>]*>\s*<a name="/g) || []).length;
  if (parsed !== opens) throw new Error(`${spec.key}: the page has ${opens} heading openings and the parse read ${parsed} — the parse is wrong`);

  const defs = new Map(), uses = new Map(), idToTerm = new Map();
  const dfnRe = /<a name="(dt-[^"]*)"[^>]*\stitle="([^"]*)"[^>]*>/g;
  for (let m; (m = dfnRe.exec(body)); ) {
    const sec = secAt(marks, m.index);
    if (!sec) continue;
    const term = normTerm(m[2]);
    if (!keepTerm(term)) continue;      /* `Error`, `Application` — one word, a coincidence generator */
    addDef(defs, term, sec);
    idToTerm.set(m[1], term);
  }
  scanUses(body, marks, idToTerm, uses);
  console.error(`  ${spec.key}: ${marks.length} numbered headings, ${defs.size} definitions`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
}

/* ---- reader: tc39 multipage (ECMAScript) ----------------------------------------------------------------- */

/* `Annex A <span class=annex-kind>(informative)</span>` is how the standard numbers an annex, and the number
 * a citation writes is `A`. Everything else is already the number. */
function normSecnum(raw) {
  const t = stripTags(raw);
  const a = /^Annex\s+([A-Z])\b/.exec(t);
  return (a ? a[1] : t).replace(/\.$/, "").trim();
}

/* AN ECMAScript OPERATION NAME MAY STAND ALONE AS A TERM WHERE AN ENGLISH PHRASE MAY NOT, AND THE STANDARD
 * ITSELF SAYS WHICH NAMES THOSE ARE. A clause that defines callable behaviour declares it — `type="abstract
 * operation"`, `"internal method"`, `"built-in function"`, `"numeric method"`, `"host-defined abstract
 * operation"` — and a clause that describes a VALUE or a piece of vocabulary declares nothing. That
 * distinction is the whole precision of this index and guessing it from the spelling was measured wrong: a
 * CamelCase test admits `TypeError`, `ReferenceError` and `FinalizationRegistry`, which every algorithm in the
 * standard mentions, so `§9.4.2's ReferenceError` reported ResolveBinding as a misattribution of the
 * TypeError constructor's clause. An error-type name is vocabulary, not a citation; `IteratorClose` is a
 * citation. The spec draws that line already, so this reads it rather than re-deriving it.
 *
 * The spelling test still runs UNDER that gate, because a typed clause can still be titled with a common
 * word — §7.3.2 is `Get ( O, P )` and `get` would match half the comments in this tree. */
const ES_CALLABLE = /^(abstract operation|internal method|built-in function|numeric method|host-defined abstract operation|implementation-defined abstract operation|syntax-directed operation)$/;
function esNameIsIdentifier(raw) {
  if (/^\[\[/.test(raw) || raw.includes("%")) return true;
  if (/^[A-Za-z_$][A-Za-z0-9_$]*(\.[A-Za-z0-9_$]+)+$/.test(raw)) return true;
  return /^[A-Za-z][A-Za-z0-9]*$/.test(raw) && /[a-z][A-Z]/.test(raw);
}

/* THE STANDARD RECORDS ITS OWN RENAMES, so the alias is read rather than invented. ES2025 respelled every
 * well-known symbol from `@@replace` to `%Symbol.replace%` and left the old spelling behind in the clause's
 * `oldids` — and this tree, correctly, still writes `RegExp.prototype [ @@replace ]` because that is what the
 * comment's surrounding code is about. Without the alias the longest phrase the index knew was the one-word
 * `RegExp.prototype`, defined two clauses away, and a CORRECT citation was reported as misattributed. */
function esAliases(title) {
  return /%Symbol\.[a-zA-Z]+%/.test(title) ? [title.replace(/%Symbol\.([a-zA-Z]+)%/g, "@@$1")] : [];
}

function regenTc39(spec) {
  const toc = curl(spec.base);
  const updated = (/<h1 class=version>([^<]*)<\/h1>/.exec(toc) || [])[1];
  if (!updated) throw new Error(`${spec.key}: no version heading — the index would record no staleness fact`);

  const sections = new Map(), pages = new Set(), idToSec = new Map();
  /* The attribute scan must tolerate a `>` INSIDE a quoted attribute value: three clause titles are shift
   * operators (`The Signed Right Shift Operator ( >> )`) and an unquoted-attribute scan ends the tag on the
   * first `>` it meets, dropping those sections silently. The count check below is what caught it. */
  const A = /<a\s+((?:"[^"]*"|'[^']*'|[^>"'])*)>\s*<span class=secnum>((?:[^<]|<span[^>]*>[\s\S]*?<\/span>)*)<\/span>([\s\S]*?)<\/a>/g;
  let parsed = 0;
  for (let m; (m = A.exec(toc)); ) {
    parsed++;
    const href = attrOf(m[1], "href") || "";
    const page = href.split("#")[0], id = href.split("#")[1] || "";
    const titleAttr = attrOf(m[1], "title");
    const no = normSecnum(m[2]);
    const title = titleAttr ? decodeEntities(titleAttr).replace(/\s+/g, " ").trim() : stripTags(m[3]);
    if (!sections.has(no)) sections.set(no, { title, page, id });
    if (page) pages.add(page);
    if (id) idToSec.set(id, no);
  }
  const secnums = (toc.match(/<span class=secnum>/g) || []).length;
  if (parsed !== secnums) throw new Error(`${spec.key}: the TOC has ${secnums} secnum spans and the parse read ${parsed} — the parse is wrong`);

  const defs = new Map(), uses = new Map(), idToTerm = new Map();
  /* THE TERMS ARE THE CLAUSES — ECMAScript defines almost nothing with <dfn> because its algorithms ARE its
   * clauses. The TOC gives every clause's full signature (`IteratorClose ( iteratorRecord, completion )`);
   * the BARE name a comment actually writes is admitted below, under the standard's own type declaration. */
  for (const [no, s] of sections) {
    for (const t of [s.title, ...esAliases(s.title)]) {
      const full = normTerm(t);
      if (keepTerm(full)) addDef(defs, full, no);
      const nb = normTerm(t.split("(")[0].trim());
      if (nb && keepTerm(nb)) addDef(defs, nb, no);   /* a multi-word name needs no type gate */
    }
  }

  const sorted = [...pages].sort();
  const bodies = [];
  for (const page of sorted) {
    const body = curl(spec.base + page);
    /* The TOC is embedded in every page, and its secnums live in <a> elements; a content heading is an <h1>.
     * Anchoring the mark scan to <h1> is what keeps the two apart. */
    const marks = [];
    const headRe = /<h1[^>]*>\s*<span class=secnum>((?:[^<]|<span[^>]*>[\s\S]*?<\/span>)*)<\/span>/g;
    for (let m; (m = headRe.exec(body)); ) marks.push({ at: m.index, no: normSecnum(m[1]) });
    /* A SINGLE-WORD NAME IS ADMITTED ONLY WHERE THE CLAUSE DECLARES CALLABLE BEHAVIOUR — see ES_CALLABLE.
     * `aoid` is the standard's own canonical name for an abstract operation; for internal methods and
     * built-in functions the name is the clause title with its parameter list removed. */
    const clauseRe = /<emu-(?:clause|annex)\s+([^>]*)>/g;
    for (let m; (m = clauseRe.exec(body)); ) {
      const id = attrOf(m[1], "id");
      const sec = id ? idToSec.get(id) : null;
      if (!sec) continue;
      const type = (attrOf(m[1], "type") || "").toLowerCase();
      const aoid = attrOf(m[1], "aoid");
      if (!aoid && !ES_CALLABLE.test(type)) continue;
      const sect = sections.get(sec);
      const names = aoid ? [aoid] : [];
      if (sect) for (const t of [sect.title, ...esAliases(sect.title)]) names.push(t.split("(")[0].trim());
      for (const raw of names) {
        const t = normTerm(raw);
        if (!t || !(keepTerm(t) || esNameIsIdentifier(raw))) continue;
        addDef(defs, t, sec);
        if (!idToTerm.has(id)) idToTerm.set(id, t);
      }
    }
    const dfnRe = /<dfn\b([^>]*)>([\s\S]{0,400}?)<\/dfn>/g;
    for (let m; (m = dfnRe.exec(body)); ) {
      const sec = secAt(marks, m.index);
      if (!sec || IMPORTED.test(m[2])) continue;
      const term = normTerm(m[2]);
      if (!keepTerm(term)) continue;
      addDef(defs, term, sec);
      const id = attrOf(m[1], "id");
      if (id) idToTerm.set(id, term);
    }
    bodies.push({ body, marks });
    process.stderr.write(`  ${page}: ${marks.length} headings\n`);
  }
  for (const b of bodies) scanUses(b.body, b.marks, idToTerm, uses);
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
}

function regen(keys) {
  const wanted = keys.length ? keys : SPECS.map((s) => s.key);
  for (const k of wanted) {
    const spec = SPEC_BY_KEY.get(k);
    if (!spec) throw new Error(`no such standard "${k}" — known: ${SPECS.map((s) => s.key).join(", ")}`);
    console.error(`fetching ${spec.label} …`);
    if (spec.kind === "whatwg-multipage") regenWhatwgMultipage(spec);
    else if (spec.kind === "bikeshed") regenBikeshed(spec);
    else if (spec.kind === "respec") regenRespec(spec);
    else if (spec.kind === "xmlspec") regenXmlspec(spec);
    else if (spec.kind === "tc39-multipage") regenTc39(spec);
    else throw new Error(`unknown reader kind ${spec.kind}`);
  }
}

/* ---- the audit ------------------------------------------------------------------------------------------ */

/* A standard this tree cites that is NOT in SPECS is not audited, and that is a COVERAGE loss rather than a
 * wrong answer — the citation is counted under its own name and printed in the report. The list exists so an
 * unanchored citation in such a file is not mistaken for one of ours. */
const OTHER_SPECS = [
  "namespaces", "encoding", "infra", "storage",
  "webcrypto", "svg", "mathml", "wasm", "uievents", "console", "performance",
  "workers", "websockets", "mimesniff", "rfc", "unicode", "utf", "trusted", "clipboard",
  "notifications", "geolocation", "geometry", "fullscreen", "pointerevents", "webaudio", "webrtc",
  "beacon", "referrer", "mixed", "cors", "cookies",
  /* A MULTI-WORD NAME WHOSE LAST WORD IS AN INDEXED STANDARD'S ANCHOR MUST BE LISTED HERE OR IT IS AUDITED AS
     THAT STANDARD, which is a WRONG ANSWER rather than a coverage gap. "Namespaces in XML" numbers entirely
     different sections from XML 1.0, and "Selection API" and "Web Cryptography API" are not the File API.
     anchorTokens tries the longest tail first, so a listed multi-word name wins over its own last word. */
  "namespaces in xml", "selection api", "cryptography api", "web cryptography api",
  /* CSS modules, as this tree spells them when it does not use the levelled shortname */
  "css", "selectors", "cascade", "view", "values", "sizing", "fonts", "backgrounds", "text",
  "display", "position", "overflow", "images", "color", "transforms", "writing", "box", "inline",
  "contain", "align", "ui", "scroll", "logical", "variables", "syntax", "media", "mediaqueries",
  "highlight", "masking", "shapes", "multicol", "tables", "page", "flexbox", "grid", "counter", "lists",
  "break", "ruby", "pseudo", "speech", "transitions", "animations", "compositing", "filter", "srgb",
];
const ANCHOR_TO_KEY = new Map();
for (const s of SPECS) for (const a of s.anchors) ANCHOR_TO_KEY.set(a, s.key);
/* A levelled CSS shortname (`css-sizing-3`, `selectors-4`) is how this tree spells a CSS module most of the
 * time, and it must classify as ANOTHER standard rather than as no anchor at all. */
const LEVELLED = /^[a-z]+(-[a-z0-9]+)*-[0-9]+$/;

/* A STANDARD'S NAME IS NOT ALWAYS ONE WORD, AND THE LAST WORD OF A MULTI-WORD NAME IS THE PART THAT COLLIDES.
 * Reading only the last word is what left `File API §3.3.3` to be caught by hand while an auditor was already
 * running over that file: the token is `API`, and this tree also writes `Selection API` and `Web Cryptography
 * API`, so no one-word rule can tell the three apart and the honest one-word answer is to decide none of them.
 * The tail is therefore read as up to THREE words and classifyAnchor takes the LONGEST that classifies —
 * `w3c file api` falls through to `file api`, `and web idl` to `web idl`, and `namespaces in xml` stops at
 * itself rather than reaching the `xml` that would judge it against a standard it is not. */
/* AND THE NAME IS READ ACROSS THE LINE BREAK AND THE STRING JOIN, because a C file puts both INSIDE a spec's
 * name. `"… Namespaces "\n  "in XML §6.2"` is one sentence to a reader and three tokens to a scanner, and the
 * two-word tail `in xml` then falls through to XML 1.0 — which does not have a §6.2, so a correct citation of
 * Namespaces in XML was reported as a section the standard does not have. The AFTER text is already flattened
 * this way before its term is read; the BEFORE text was not, and that asymmetry was the bug. */
function anchorTokens(before) {
  const flat = before.replace(/[\n\r]+[ \t]*\*?[ \t]*/g, " ").replace(/["\\]+/g, " ");
  const tail = flat.replace(/[\s'"’(\[]+$/, "").replace(/\s+(?:Standard|standard|spec|Spec)$/, "");
  const m = /((?:[A-Za-z][A-Za-z0-9+-]*[ \t]+){0,2}[A-Za-z][A-Za-z0-9+-]*)$/.exec(tail);
  if (!m) return [];
  const w = m[1].split(/[ \t]+/);
  const out = [];
  for (let n = w.length; n >= 1; n--) out.push(w.slice(w.length - n).join(" "));
  return out;                                       /* longest tail first */
}

function classifyAnchor(toks) {
  for (const t of toks) {
    const w = t.toLowerCase();
    if (ANCHOR_TO_KEY.has(w)) return ANCHOR_TO_KEY.get(w);
    if (OTHER_SPECS.includes(w) || LEVELLED.test(w)) return "other:" + w;
  }
  return null;
}

function walk(dir, out = []) {
  /* THE CHECKOUT IS SHARED AND EDITED UNDER THIS WALK. An editor's temporary file appears between the readdir
   * and the stat and is gone before it, so a scan that trusts a name it just read crashes on another lane's
   * save. A vanished entry is not a finding and not an error — it is a file that was never in the tree
   * this run is measuring. */
  let names;
  try { names = readdirSync(dir); } catch { return out; }
  for (const e of names) {
    if (e === "node_modules" || e === ".git" || e === "lexbor" || e === "qjs" || e.includes(".tmp.")) continue;
    const p = join(dir, e);
    let st;
    try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) walk(p, out);
    else if (/\.(c|h)$/.test(e)) out.push(p);
  }
  return out;
}

/* PROSE — comment bodies and string literals, the two places in a C file where a citation can live. A bare
 * dotted number is read only out of these, so a float literal, an array bound and a version in a Makefile-ish
 * define are never candidates. String literals count because a DFAIL message is prose that a reader follows
 * exactly like a comment, and its number must be right for the same reason. */
function proseSpans(src) {
  const spans = [];
  const n = src.length;
  for (let i = 0; i < n; ) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "*") {
      const e = src.indexOf("*/", i + 2);
      spans.push([i + 2, e < 0 ? n : e]); i = e < 0 ? n : e + 2;
    } else if (c === "/" && src[i + 1] === "/") {
      let e = src.indexOf("\n", i + 2); if (e < 0) e = n;
      spans.push([i + 2, e]); i = e;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c) { if (src[j] === "\\") j++; if (src[j] === "\n") break; j++; }
      spans.push([i + 1, Math.min(j, n)]); i = j + 1;
    } else i++;
  }
  return spans;
}

function inSpans(spans, at) {
  let lo = 0, hi = spans.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (spans[mid][1] <= at) lo = mid + 1;
    else if (spans[mid][0] > at) hi = mid - 1;
    else return true;
  }
  return false;
}

function lineIndex(src) {
  const starts = [0];
  for (let i = 0; i < src.length; i++) if (src.charCodeAt(i) === 10) starts.push(i + 1);
  return (at) => {
    let lo = 0, hi = starts.length - 1, n = 1;
    while (lo <= hi) { const mid = (lo + hi) >> 1; if (starts[mid] <= at) { n = mid + 1; lo = mid + 1; } else hi = mid - 1; }
    return n;
  };
}

function audit(argv, opts = {}) {
  const idx = new Map();
  for (const s of SPECS) {
    const f = indexFileOf(s.key);
    if (!existsSync(f)) continue;
    const ix = JSON.parse(readFileSync(f, "utf8"));
    /* A COMMITTED INDEX WRITTEN BY AN OLDER READER IS A PRODUCER THAT DOES NOT PRODUCE A FIELD THIS CONSUMER
     * READS, and CLAUDE.md's rule for that is an assert rather than a default: `ix.uses || {}` would turn
     * "this index predates the use-site scan" into the plausible datum "no section is about any term", and
     * every citation confirmed by a use would silently become a finding. */
    for (const need of ["sections", "dfns", "uses", "specUpdated", "fetched"]) {
      if (!ix[need]) throw new Error(`${relative(ROOT, f)} has no "${need}" — it was written by an older reader; re-run: node engine/citegen.mjs --regen ${s.key}`);
    }
    /* A LOOKUP TABLE MUST NOT ANSWER FOR A KEY IT DOES NOT HOLD. JSON.parse hands back objects that inherit
     * Object.prototype, so `dfns["constructor"]` returns a FUNCTION and `dfns["to string"]`-shaped phrases
     * reach members nothing indexed — the table saying yes to a term the standard never defined. It crashed
     * here rather than answering wrongly only by luck (`.includes` is not a function on that value); the fix
     * is that the table has no prototype to inherit an answer from. */
    for (const t of ["sections", "dfns", "uses"]) Object.setPrototypeOf(ix[t], null);
    idx.set(s.key, ix);
  }
  if (!idx.size) {
    console.error(`no committed index in ${relative(ROOT, INDEX_DIR)} — run: node engine/citegen.mjs --regen`);
    process.exit(2);
  }
  /* Longest-first matching needs to know how far a phrase can run before it stops being a term, and the
   * shortest a term can be, because ECMAScript's operation names are one word and nothing else's are. */
  let maxWords = 2, minWords = 2;
  const titleToNo = new Map();
  for (const [key, ix] of idx) {
    for (const t of Object.keys(ix.dfns)) {
      const n = t.split(" ").length;
      if (n > maxWords) maxWords = n;
      if (n < minWords) minWords = n;
    }
    const tt = new Map();
    for (const [no, s] of Object.entries(ix.sections)) {
      const k = normTerm(s.title);
      if (!tt.has(k)) tt.set(k, []);
      tt.get(k).push(no);
    }
    titleToNo.set(key, tt);
  }

  const targets = argv.filter((a) => !a.startsWith("--"));
  let files;
  if (opts.files) files = opts.files;
  else if (targets.length) files = targets.map((t) => (statSync(t).isDirectory() ? walk(t) : [t])).flat();
  else {
    /* The default is what this project WROTE: the host, plus the fork's own two quickjs translation units.
     * The rest of engine/qjs is upstream and its citations are not this tree's to answer for — but quickjs.c
     * carries more ECMAScript citations than the whole of engine/host, and a gate nobody points at a file is
     * a gate that does not run on it. */
    files = walk(join(HERE, "host"));
    for (const extra of ["qjs/quickjs.c", "qjs/quickjs.h"]) {
      const p = join(HERE, extra);
      if (existsSync(p)) files.push(p);
    }
  }

  const findings = [];
  const undecided = [];
  const stat = { total: 0, bare: 0, anchored: 0, byTerm: 0, byFile: 0, other: 0, skipped: 0,
                 confirmed: 0, confirmedByUse: 0, confirmedByContainment: 0, unverified: 0, multiSpec: 0,
                 foreignTerm: 0 };
  const byKey = new Map();
  const byOther = new Map();
  const untitled = new Map();
  const unknownTok = new Map();
  const SEC = "[0-9]+(?:\\.[0-9]+)*|[A-F](?:\\.[0-9]+)+";
  const CITE = new RegExp("§(" + SEC + ")", "g");
  /* A bare number needs at least two components and no zero component: section numbers never carry a zero and
   * never a leading-zero part, which is what separates `7.4.9` from `0.0`, `1.0` and `0.5`. */
  /* The trailing guard must reject a LONGER number (`7.4.9.1`) without rejecting a citation that ends a
   * SENTENCE (`… is not 7.4.9.`) — a `.` alone is punctuation, a `.` before a digit is another component.
   * The first spelling of this lookahead dropped every sentence-final citation silently. */
  const BARE = /(?<![\w.§])([1-9][0-9]?(?:\.[1-9][0-9]*){1,4})(?!\w)(?!\.\d)/g;
  /* AND THE RIGHT OPERAND OF A RANGE IS NOT A CITATION CANDIDATE AT ALL, for the same reason a float literal is
   * not: it is not a number this tree is naming a section by. `steps 1-4.1` and `steps 4.2-4.4` are how every
   * stage label in this tree writes a span of STEPS, and the lookbehind above admits `4.1` and `4.4` out of
   * them because a `-` is not a word character. What follows such a match is the REST OF THE SENTENCE, so the
   * term scan reads the algorithm the label is about and reports it as misattributed to a section number that
   * was never written — the tool inventing the citation it then judges. Two of `dom/range.c`'s four findings
   * were exactly this, and 178 bare candidates tree-wide sit in this position.
   * IT IS EXCLUDED RATHER THAN RESOLVED BECAUSE IT IS GENUINELY UNDECIDABLE. `13.2-13.4` in prose is a range of
   * SECTIONS and `7.5-7.9` is a range of STEPS, and nothing at the site distinguishes them; this file's own
   * doctrine for a site it cannot decide is to refuse the guess, and the cost is one operand of a bare-written
   * span — whose left operand is still read, and whose §-written spelling is read by CITE regardless. */
  const RANGE_OPERAND = /[0-9](?:\.[0-9]+)*\s*[-‐-―]\s*$/;

  /* Look a phrase up in every index at once. Returns the LONGEST phrase any standard knows, the standards
   * that know it, and — per standard — whether the cited number is its definition site or a prominent use. */
  /* A SECTION CONTAINS ITS OWN SUBSECTIONS, so a citation of §7.4 for a term the standard defines at §7.4.1.2
   * is not wrong — it is less precise, and precision is the author's call. Reporting it would be the tool
   * asserting an error it cannot demonstrate, which is exactly the failure it exists to catch. A SIBLING
   * (§7.4.5 for a term defined at §7.4.1.2) is a different matter and stays a finding. */
  const contains = (ancestor, sec) => sec.length > ancestor.length && sec.startsWith(ancestor + ".");

  const probe = (phrase, no, only) => {
    const hits = [];
    for (const [key, ix] of idx) {
      if (only && key !== only) continue;
      if (!ix.dfns[phrase]) continue;
      const u = ix.uses[phrase] || null;
      const n2 = u && Object.hasOwn(u, no) ? u[no] : 0;
      hits.push({ key, where: ix.dfns[phrase],
        defAt: ix.dfns[phrase].includes(no),
        underAt: ix.dfns[phrase].some((d) => contains(no, d)),
        useAt: n2 >= USE_FLOOR, mentions: n2 });
    }
    return hits.length ? { phrase, hits } : null;
  };
  const lookup = (words, no, only) => {
    for (let n = Math.min(maxWords, words.length); n >= minWords; n--) {
      const r = probe(words.slice(0, n).join(" "), no, only);
      if (r) return r;
    }
    return null;
  };

  for (const file of files) {
    const src = opts.srcOf ? opts.srcOf(file) : readFileSync(file, "utf8");
    const lineOf = lineIndex(src);
    const spans = proseSpans(src);

    /* PASS 1 — collect candidates and this file's anchor votes. */
    const cites = [];
    const votes = new Map();
    const seen = new Set();
    CITE.lastIndex = 0;
    for (let m; (m = CITE.exec(src)); ) {
      const toks = anchorTokens(src.slice(Math.max(0, m.index - 40), m.index));
      const a = classifyAnchor(toks);
      const tok = toks.length ? toks[toks.length - 1] : null;   /* the one word, for the gap report below */
      cites.push({ at: m.index, len: m[0].length, no: m[1], anchor: a, bare: false });
      seen.add(m.index + 1);
      if (a) votes.set(a, (votes.get(a) || 0) + 1);
      else if (tok && /^[A-Z]/.test(tok) && tok.length > 2) unknownTok.set(tok, (unknownTok.get(tok) || 0) + 1);
    }
    BARE.lastIndex = 0;
    for (let m; (m = BARE.exec(src)); ) {
      if (seen.has(m.index) || !inSpans(spans, m.index)) continue;
      if (RANGE_OPERAND.test(src.slice(Math.max(0, m.index - 24), m.index))) continue;
      cites.push({ at: m.index, len: m[0].length, no: m[1], anchor: null, bare: true });
    }
    cites.sort((a, b) => a.at - b.at);

    let dominant = null, best = 0, second = 0;
    for (const [k, v] of votes) { if (v > best) { second = best; dominant = k; best = v; } else if (v > second) second = v; }
    /* The file vote SURVIVES ONLY AS A TIE-BREAK among standards that already define the term. It can no
     * longer decide a citation on its own — see the header: that is what produced the DOM-in-an-HTML-file and
     * the ECMAScript-in-quickjs.c false attributions. */
    const fallback = best >= 3 && best >= 2 * second ? dominant : null;

    /* PASS 2 — term evidence per citation, independent of any file-level guess. */
    for (const c of cites) {
      const after = src.slice(c.at + c.len, c.at + c.len + 220).replace(/\n\s*\*?\s*/g, " ");
      c.quoted = (/^['"’“]?s?['"’“]?\s*["“]([^"”]{2,90})["”]/.exec(after) || [])[1] || null;
      c.words = normTerm(after.replace(/^'s\b/, " ")).split(" ").filter(Boolean);
      const only = c.anchor && !c.anchor.startsWith("other:") ? c.anchor : null;
      if (c.anchor && c.anchor.startsWith("other:")) { c.foreign = true; continue; }
      /* A QUOTE IS THE AUTHOR'S OWN STATEMENT OF WHAT THE CITATION IS ABOUT, so it is matched WHOLE and the
       * running prose is not consulted at all. Prefix-matching inside a quote reads a term out of a sentence
       * that merely contains one: `DOM §3.2 "fire an event named abort at signal"` cites Interface AbortSignal
       * correctly and quotes one of its STEPS, and a prefix match turned that into a misattribution of
       * §2.10 "Firing events". If the quoted phrase is not a term and not a title, there is nothing here to
       * check — which is UNDECIDED, not a finding. */
      c.ev = c.quoted ? probe(normTerm(c.quoted), c.no, only) : lookup(c.words, c.no, only);
    }

    /* PASS 3 — GROUP EVIDENCE admits the bare numbers. A bare dotted number is a citation when some other
     * occurrence of that same number in this same file is followed by spec vocabulary; on its own it is a
     * float. The same grouping is what lets an undiagnosable site be reported as sharing a diagnosed number
     * instead of being silently dropped or, worse, guessed at. */
    const group = new Map();
    for (const c of cites) {
      if (c.foreign) continue;
      const g = group.get(c.no) || { evidence: false, keys: new Set(), members: [] };
      if (c.ev) { g.evidence = true; for (const h of c.ev.hits) g.keys.add(h.key); }
      g.members.push(c);
      group.set(c.no, g);
    }

    for (const c of cites) {
      if (c.foreign) {
        stat.total++; stat.other++;
        const k = c.anchor.slice(6);
        byOther.set(k, (byOther.get(k) || 0) + 1);
        continue;
      }
      const g = group.get(c.no);
      if (c.bare && !g.evidence) continue;      /* a number, not a citation */
      stat.total++;
      if (c.bare) stat.bare++;

      /* RESOLUTION, in order of how much the citation itself proves. */
      let spec = null, how = null;
      /* THE GROUP IS A UNION OVER ONE NUMBER IN ONE FILE, AND ITS ONE JOB IS ADMITTING A BARE NUMBER (PASS 3);
       * letting it also DECIDE which standard a citation belongs to means one neighbour can silently disqualify
       * every sibling. Measured the day Web IDL was indexed: `fetch/headers.c` cites §5.1 forty-odd times for
       * the Fetch Standard's Headers class, ELEVEN of them misattributions the audit had been reporting — and
       * one of those forty, at a different line, runs prose naming `interface prototype object`, which the new
       * index defines. The group became {fetch, idl}, the file's own anchor vote is 7 IDL against 6 Fetch and
       * so decides nothing, and all eleven findings stopped being reported. Nothing had confirmed them.
       * SO THE CITATION'S OWN EVIDENCE IS CONSULTED — but LAST, below the file fallback, for the reason spelled
       * out at that site: it must ADD a resolution rather than replace one. */
      const own = c.ev ? new Set(c.ev.hits.map((h) => h.key)) : null;
      if (c.anchor) { spec = c.anchor; how = "anchored"; }
      else if (g.keys.size === 1) { spec = [...g.keys][0]; how = "term"; }
      else if (g.keys.size > 1) {
        stat.multiSpec++;
        spec = g.keys.has(fallback) ? fallback : null;
        how = "term";
      }
      if (!spec && fallback && idx.has(fallback)) { spec = fallback; how = "file"; }
      /* LAST, AND ONLY WHERE EVERY OTHER RULE HAS GIVEN UP: the citation's OWN term evidence. It is placed here
       * rather than ahead of the group because anywhere earlier it does not ADD a resolution, it REPLACES one —
       * measured: put in the group's branch it preempted the file fallback and silently retired eighteen
       * findings whose file-anchored resolution was the better answer. Down here it can only turn a citation
       * that was about to be dropped unaudited into one that is judged. */
      if (!spec && own && own.size === 1 && idx.has([...own][0]) && idx.get([...own][0]).sections[c.no]) {
        spec = [...own][0]; how = "term";
      }
      if (!spec) { stat.skipped++; continue; }
      if (!idx.has(spec)) { stat.other++; byOther.set(spec.replace(/^other:/, ""), (byOther.get(spec.replace(/^other:/, "")) || 0) + 1); continue; }
      stat[how === "anchored" ? "anchored" : how === "term" ? "byTerm" : "byFile"]++;
      byKey.set(spec, (byKey.get(spec) || 0) + 1);

      const ix = idx.get(spec);
      const sections = ix.sections, no = c.no;
      let verdict = null;

      /* (1) The number the standard does not have — ASKED ONLY OF AN EXPLICITLY ANCHORED CITATION, and the
       * asymmetry is the point rather than caution. This check fires on ABSENCE, which is exactly what a
       * mis-resolved citation produces. */
      /* AND THE `else` IS A SOUNDNESS FILTER, NOT AN ACCIDENT OF NESTING — this was tried the other way and
       * measured. Ungating the term check on "does the resolved standard have this number" added 252 findings
       * and the sample was noise, every one of it the same shape: the number does not belong to the standard
       * the resolver picked, so the site is a MIS-RESOLUTION rather than a misattribution. `range.c`'s
       * `steps 16.1 and 17.1` is a step span whose right operand the bare-number reader admits, `text_stream.c`
       * cites Encoding §7.1 which nothing here indexes, `element.h` cites HTML §4.6.3 in a URL-flavoured
       * sentence. A number the resolved standard does not have is the tell that the resolution is wrong, and
       * judging past it is the tool asserting an error it cannot demonstrate. */
      if (!sections[no]) {
        if (c.anchor) verdict = { kind: "UNKNOWN-SECTION", msg: `${spec} has no §${no}` };
      } else {
        /* (2) THE SECTION'S OWN TITLE IS ASKED FIRST, AND THE ORDER IS THE WHOLE POINT. A citation written in
         * the form CLAUDE.md §Browser half mandates — the number with its title beside it — has already
         * proved itself, and nothing after it can overturn that. Asking term attribution first inverted it:
         * `HTML §15.3.3 Flow content` is exactly right, and because the standard ALSO defines `flow content`
         * as a content category over in §3.2.5.2.2, the term check fired and reported the correctly-titled
         * citation as misattributed. A title stated is a claim the standard can confirm outright. */
        /* AND IT IS ASKED OF EVERY CANDIDATE STANDARD, not of the one the resolver happened to pick — for the
         * same reason the term check is. `url.h`'s `§5.1 application/x-www-form-urlencoded parsing` states the
         * URL Standard's exact title for that number; the term it names is defined by BOTH HTML and URL, so
         * the multi-standard tie-break fell back to the file's anchor, judged it against HTML §5.1
         * "Introduction", and reported a citation that had already proved itself. A stated title is evidence
         * about WHICH standard as much as about which section. */
        const titleCands = c.anchor && idx.has(c.anchor) ? [c.anchor] : [...idx.keys()];
        for (const k of titleCands) {
          const sx = idx.get(k).sections[no];
          if (!sx) continue;
          const wt = normTerm(sx.title);
          if (!wt) continue;
          if ((c.quoted && normTerm(c.quoted) === wt) ||
              c.words.slice(0, wt.split(" ").length).join(" ") === wt) { verdict = { kind: "OK-TITLED" }; break; }
        }

        /* (3) TERM ATTRIBUTION across every indexed standard. The finding is raised only when NO candidate
         * standard defines the phrase at this number, none defines it UNDER it, and none is prominently about
         * it there — so the claim the report makes is the one it can prove. */
        if (!verdict && c.ev) {
          const ok = c.ev.hits.find((h) => h.defAt);
          const under = c.ev.hits.find((h) => h.underAt);
          const used = c.ev.hits.find((h) => h.useAt);
          /* CONFIRMATION QUANTIFIES OVER EVERY STANDARD; A FINDING DOES NOT — AND THAT ASYMMETRY IS THE WHOLE
           * DIFFERENCE BETWEEN A CHECKABLE CLAIM AND A COINCIDENCE. A confirmation says "some standard does
           * define this here", which is true or false on its own. A finding says "the standard you cited
           * numbers this thing somewhere else", and that sentence only means anything about a standard that
           * NUMBERS THE THING AT ALL. When the resolved standard is not among the phrase's definers, what the
           * comment did was USE another standard's vocabulary while citing its own section, which is what
           * prose in a spec ecosystem looks like — not a misattribution.
           * MEASURED, by reading every one of them: indexing Web IDL — the standard whose terms every other
           * standard is written in — turned that inference into 63 cross-standard findings, and the shape
           * repeats down the list. `html_iframe.c` cites HTML §4.8.5 "The iframe element" and says `insertion
           * steps`, which DOM §4.2.3 defines as the hook HTML §4.8.5 then fills in; `html_image.c` cites HTML
           * §4.8.3 "The img element" for the `Image()` LEGACY FACTORY FUNCTION, a Web IDL concept whose
           * instance lives exactly there; `event_target.c` cites DOM §2.7 "Interface EventTarget" and calls
           * its prototype the INTERFACE PROTOTYPE OBJECT, which is simply its name. Every motivating example
           * in this file's own header is same-standard — `determine the origin` at HTML §7.3.1 for HTML
           * §7.3.2.1, `pipeTo` at Streams §4.2.4 for Streams §4.9.1 — because that is the claim the index can
           * actually support. */
          const owned = c.ev.hits.some((h) => h.key === spec);
          if (ok) verdict = { kind: "OK-TERM" };
          else if (under) verdict = { kind: "OK-CONTAINS" };
          else if (used) verdict = { kind: "OK-USE" };
          else if (!owned) { stat.foreignTerm++; }
          else {
            const where = c.ev.hits.map((h) => {
              const hx = idx.get(h.key);
              return `${h.key} ${h.where.map((n) => `§${n} "${hx.sections[n] ? hx.sections[n].title : "?"}"`).join(" / ")}`;
            }).join("; ");
            const men = Math.max(...c.ev.hits.map((h) => h.mentions));
            const one = c.ev.hits.length === 1 && c.ev.hits[0].where.length === 1
              ? `${c.ev.hits[0].key} §${c.ev.hits[0].where[0]}` : null;
            verdict = { kind: "MISATTRIBUTED", target: one,
              msg: `"${c.ev.phrase}" is defined in ${where} — no indexed standard defines it at §${no}, nor under it, nor is any §${no} about it` +
                   (sections[no] ? ` (${spec} §${no} is "${sections[no].title}"` : " (") +
                   (men ? `, which links the term ${men}×)` : ")") };
          }
        }

        /* (4) A quoted phrase that titles a DIFFERENT section of the same standard — the renumbering tell. */
        if (!verdict && c.quoted) {
          const q = normTerm(c.quoted);
          if (titleToNo.get(spec).has(q)) {
            verdict = { kind: "TITLE-MISMATCH",
              msg: `"${c.quoted}" titles ${spec} §${titleToNo.get(spec).get(q).join(", §")}; §${no} is "${sections[no].title}"` };
          }
        }
      }

      const rec = { file: relative(ROOT, file), line: lineOf(c.at), no, spec, how, bare: c.bare,
                    text: src.slice(c.at, c.at + 100).split("\n")[0] };
      if (!verdict) {
        stat.unverified++;
        const uk = `${spec} §${no}`;
        untitled.set(uk, (untitled.get(uk) || 0) + 1);
        rec.groupNo = c.no;
        undecided.push(rec);
        continue;
      }
      if (verdict.kind === "OK-USE" || verdict.kind === "OK-CONTAINS") { stat.confirmed++; stat[verdict.kind === "OK-USE" ? "confirmedByUse" : "confirmedByContainment"]++; continue; }
      if (verdict.kind.startsWith("OK")) { stat.confirmed++; continue; }
      findings.push({ ...rec, ...verdict });
    }
  }

  /* A site the tool CANNOT decide, standing on a number whose other sites in the same file ARE diagnosed, is
   * a fact worth printing and a guess worth refusing. It is reported as its own category so nobody mistakes
   * it for a finding — see the header. */
  const wrong = new Map();
  for (const f of findings) {
    if (f.kind !== "MISATTRIBUTED") continue;
    const k = f.file + "\u0000" + f.no;
    if (!wrong.has(k)) wrong.set(k, new Map());
    const t = wrong.get(k), name = f.target || "(more than one candidate section)";
    t.set(name, (t.get(name) || 0) + 1);
  }
  const suspects = undecided.filter((u) => wrong.has(u.file + "\u0000" + u.groupNo));
  if (opts.quiet) return findings;
  /* WHAT THE OTHER SITES ON THIS NUMBER RESOLVED TO IS THE ONE THING THE READER NEEDS AND THE ONE THING THIS
   * CAN PROVE. A file writing `7.4.9 IteratorClose` six times and `7.4.9 IteratorStepValue` four times has
   * §the numbering of an older edition — and it is ALSO a file in which that one number means TWO different
   * operations, so the undecided sites on it cannot be swept to a single target. The tally goes beside them:
   * the evidence handed over, the guess refused. */
  const tallyOf = (f) => [...wrong.get(f.file + "\u0000" + f.no)].sort((a, b) => b[1] - a[1])
    .map(([t, n]) => `${t}×${n}`).join(", ");

  console.log("spec-citation audit");
  for (const [key, ix] of idx) {
    console.log(`  ${ix.spec}: ${Object.keys(ix.sections).length} sections, ${Object.keys(ix.dfns).length} terms, ` +
      `${Object.keys(ix.uses).length} with a prominent use site — index fetched ${ix.fetched}, standard updated ${ix.specUpdated}`);
  }
  console.log(`  ${stat.total} citations in ${files.length} files (${stat.bare} written without a §, admitted by group evidence)`);
  console.log(`  resolved: ${stat.anchored} by their own anchor, ${stat.byTerm} by the term they name, ${stat.byFile} by their file's dominant anchor`);
  console.log(`  ${stat.other} belong to a standard this audit does not index; ${stat.skipped} name no standard and no term it knows`);
  console.log(`  audited by standard: ${[...byKey].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join(" ")}`);
  console.log(`  ${stat.confirmed} confirmed (${stat.confirmedByContainment} by a subsection of the cited number, ${stat.confirmedByUse} by a prominent use rather than the definition site), ` +
    `${stat.unverified} carry no title and no term any index knows, ${stat.multiSpec} name a term more than one standard defines`);
  console.log(`  ${stat.foreignTerm} name a term only ANOTHER standard defines, so the standard they cite numbers nothing this audit could hold them to`);
  if (byOther.size) console.log(`  standards seen but not indexed: ${[...byOther].sort((a, b) => b[1] - a[1]).slice(0, 14).map(([k, v]) => `${k}=${v}`).join(" ")}`);
  const gaps = [...unknownTok].filter(([, v]) => v >= 8).sort((a, b) => b[1] - a[1]);
  if (gaps.length) console.log(`  capitalised tokens in front of a § that no list knows (a standard among these is coverage this audit is not getting): ${gaps.slice(0, 20).map(([k, v]) => `${k}=${v}`).join(" ")}`);

  if (argv.includes("--titles")) {
    /* WHERE A TITLE WOULD BUY THE MOST. The unverified population is a COUNT, not a list of findings — but it
     * is not uniform: a number cited forty times with no title anywhere is one edit away from being checkable,
     * and a number cited once is not worth anyone's afternoon. */
    console.log(`\nnumbers cited most often with neither a title nor a term any index knows — a title here makes the citation checkable:`);
    for (const [k, v] of [...untitled].sort((a, b) => b[1] - a[1]).slice(0, 40)) {
      const [key, no] = k.split(" §");
      const s = idx.get(key).sections[no];
      console.log(`  ${String(v).padStart(4)}x  ${k}  ${s ? `"${s.title}"` : "(no such section)"}`);
    }
  }

  const groups = new Map();
  for (const f of findings) { if (!groups.has(f.kind)) groups.set(f.kind, []); groups.get(f.kind).push(f); }
  const limit = argv.includes("--all") ? Infinity : 120;
  console.log("");
  for (const kind of ["UNKNOWN-SECTION", "MISATTRIBUTED", "TITLE-MISMATCH"]) {
    const g = groups.get(kind) || [];
    console.log(`${kind}: ${g.length}`);
    for (const f of g.slice(0, limit)) {
      console.log(`  ${f.file}:${f.line}  ${f.msg}`);
      console.log(`      ${f.text.trim()}`);
    }
    if (g.length > limit) console.log(`  … ${g.length - limit} more (--all)`);
  }
  console.log(`\nUNDECIDED-ON-A-DIAGNOSED-NUMBER: ${suspects.length}`);
  console.log(`  (these name no term, so the tool cannot decide them; they cite a number whose OTHER sites in the same file are misattributed above. A human must read each one — a guess here is the defect this file exists to find.)`);
  for (const f of suspects.slice(0, limit)) {
    console.log(`  ${f.file}:${f.line}  §${f.no} — the decided sites on this number in this file point to ${tallyOf(f)}`);
    console.log(`      ${f.text.trim()}`);
  }
  if (suspects.length > limit) console.log(`  … ${suspects.length - limit} more (--all)`);

  console.log(`\n${findings.length} finding(s), ${suspects.length} undecided beside them. This auditor REPORTS; it exits 0 by design — see the header.`);
}

/* ---- --since: what THIS diff introduced ------------------------------------------------------------------ */

/* A DELTA IS THE RIGHT MEASUREMENT AND A DELTA GATE IS STILL THE WRONG MECHANISM, and the two halves of that
 * are worth stating apart because the first is what this builds and the second is what it refuses to build.
 *
 * THE MEASUREMENT. Five hundred standing findings is a number nobody reads, so "run it on what you write" —
 * which CLAUDE.md §Browser half now requires — is an instruction that costs a lane more attention than it has.
 * What a lane actually needs is the handful its own diff ADDED, and that is computable exactly: audit the
 * files the diff touches with their WORKING-TREE bytes, audit the SAME files with the bytes at <ref>, and
 * report the difference. Both runs read the same committed indexes and the same resolver, so an upstream
 * edition cannot move the answer and neither can a peer's commit to a file this diff does not touch.
 *
 * IT IS KEYED BY (file, phrase, number), NEVER BY LINE, because a diff moves lines and a line-keyed delta
 * would report every citation below an inserted paragraph as introduced. A file that does not exist at <ref>
 * is read as empty, so a new file's findings are all its own.
 *
 * AND IT EXITS 0, LIKE EVERY OTHER MODE HERE. The delta form defeats the noise floor, which is the objection
 * it was proposed against, and it does NOT defeat the two that decide this: a citation is PROSE, so a build
 * that fails on one is a build in which no lane can land a spelling fix; and a finding appearing in a file
 * your diff touched is not evidence your diff caused it — this file's own history is the proof, since
 * indexing eight standards moved 35 findings to confirmations and revealed 84 more without a single C file
 * changing, and a resolver edit moved findings between files nobody had edited. A gate that fails a lane for
 * a finding it did not introduce gets muted exactly as fast as one that fails it for five hundred it did not
 * introduce. So this prints, and the human decides. */
function since(ref, argv) {
  const changed = execFileSync("git", ["diff", "--name-only", ref, "--", "*.c", "*.h"],
    { cwd: ROOT, encoding: "utf8" }).split("\n").filter(Boolean);
  const files = changed.map((r) => join(ROOT, r)).filter((p) => existsSync(p));
  if (!files.length) { console.log(`no .c/.h file differs from ${ref} — nothing for this mode to compare`); return; }

  const baseSrc = new Map();
  for (const p of files) {
    const rel = relative(ROOT, p);
    try { baseSrc.set(p, execFileSync("git", ["show", `${ref}:${rel}`], { cwd: ROOT, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 })); }
    catch { baseSrc.set(p, ""); }        /* absent at ref — a new file owns every finding in it */
  }
  const key = (f) => `${f.file} ${f.kind} ${f.no} ${f.msg}`.replace(/:\d+/g, "");
  const tip = audit(argv, { files, quiet: true });
  const base = audit(argv, { files, quiet: true, srcOf: (p) => baseSrc.get(p) });
  const had = new Set(base.map(key));
  const added = tip.filter((f) => !had.has(key(f)));
  const gone = base.filter((f) => !new Set(tip.map(key)).has(key(f)));

  console.log(`spec-citation delta against ${ref}: ${files.length} changed .c/.h file(s), ` +
    `${base.length} finding(s) before, ${tip.length} after`);
  console.log(`\nINTRODUCED BY THIS DIFF: ${added.length}`);
  for (const f of added) { console.log(`  ${f.file}:${f.line}  ${f.kind}  ${f.msg}`); console.log(`      ${f.text.trim()}`); }
  console.log(`\nRETIRED BY THIS DIFF: ${gone.length}`);
  for (const f of gone) console.log(`  ${f.file}  ${f.kind}  ${f.msg}`);
  console.log(`\nThis mode REPORTS; it exits 0 by design — see the comment above it.`);
}

const argv = process.argv.slice(2);
const sinceAt = argv.indexOf("--since");
if (sinceAt >= 0) {
  const ref = argv[sinceAt + 1] && !argv[sinceAt + 1].startsWith("--") ? argv[sinceAt + 1] : "origin/main";
  since(ref, argv.filter((a) => a !== "--since" && a !== ref));
}
else if (argv.includes("--regen")) regen(argv.filter((a) => !a.startsWith("--")));
else audit(argv);
