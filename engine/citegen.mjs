/* Spec-citation gap AUDITOR — the same job engine/idlgen.mjs does for Web IDL, done for the section numbers
 * this tree cites. It reads the REAL spec text, builds an index of what each standard actually numbers and
 * names, and DIFFS every citation in the tree against it. It PRINTS what disagrees; it does not rewrite a
 * comment and it does not generate one.
 *
 *   node engine/citegen.mjs [path …]      audit (default: engine/host + the fork's own quickjs.c/.h)
 *   node engine/citegen.mjs --all         print every finding rather than the first 120 of each kind
 *   node engine/citegen.mjs --titles      the numbers cited most often that carry no title and no known term
 *   node engine/citegen.mjs --unanchored  the citations naming no standard that only a file vote placed — the
 *                                         ones inside a DCHECK/DFAIL/CHECK message first, since a crash prints them
 *   node engine/citegen.mjs --gaps        the quotation check's verified/not-found split by distance from the §
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
 * SO A CITATION IS RESOLVED BY ITS OWN EVIDENCE, ACROSS EVERY INDEXED STANDARD, AND THE FILE VOTE NO LONGER
 * DECIDES WHAT THE TOOL ASSERTS.
 * The trailing phrase is looked up in EVERY index at once and the longest match wins; the standards that
 * define that phrase are the candidates, and the citation is a finding only when NO candidate places the term
 * at the cited number. The claim the tool then makes is strictly true and carries its own proof — "no indexed
 * standard defines `about base url` at §7.4" — where the old claim ("HTML does not") was true of the index and
 * silent about the world. An unanchored citation whose phrase no standard defines is UNDECIDED, counted and
 * never asserted about. The cost is recall, and it is the right trade: a checker that cries wolf gets muted,
 * and a muted checker is worse than none.
 *
 * BUT THE VOTE IS NOT GONE — IT IS DEMOTED, AND THE SENTENCE ABOVE USED TO SAY "GONE", WHICH IS WHY THIS
 * PARAGRAPH EXISTS. A citation that names a number and no standard is 32614 of this tree's 39071 — 83%, at the
 * revision this was measured — and the vote still picks a standard for 8299 of them, because nothing else can. That is not a defect on its own;
 * a bucket has to be chosen before an undecided site can be filed under one. The defect is a HEADER that told
 * the next reader the channel had been removed, so an auditor of THIS FILE would have gone looking for the
 * guess and concluded there was none. The rule that replaces the false claim is a division of labour, and it
 * is the one asymmetry this resolver already had in one place and lacked in another:
 *   — THE FILE VOTE MAY RESOLVE. It decides which bucket an undecided citation is counted under, which is a
 *     statement about a POPULATION and is read as one.
 *   — THE FILE VOTE MAY NOT JUDGE. Every check whose truth depends on WHICH standard was picked is asked only
 *     of a citation whose standard is its own evidence. The number-does-not-exist check has always been gated
 *     that way and says so below; the TITLE-MISMATCH check was not, and it is the one live route by which an
 *     inference could have become a verdict — 398 file-voted sites carry a quoted phrase and stand on a number
 *     their guessed standard has, which is everything that check needs. It produced nothing today, and "it has
 *     not fired yet" is not a property of a mechanism. A MISATTRIBUTED cannot arise from a vote at all, and
 *     that is structural rather than lucky: the vote is only reached when the file's anchor is NOT among the
 *     standards the citation's own term evidence names, so `owned` below is false by construction.
 * WHAT THAT LEAVES IS A COUNT, AND A COUNT WITH NO LIST BEHIND IT IS THE SILENT-ZERO SHAPE THIS FILE ALREADY
 * NAMES ELSEWHERE. 8299 cannot be printed — a category that size is read once and never again, which is the
 * muting this whole file is written against. So it is printed the way --titles prints the unverified
 * population: the count in the summary, the ACTIONABLE HEAD behind a flag. The head is the citations sitting
 * inside a DCHECK/DFAIL/CHECK STRING LITERAL, because that text is what a crash prints and what a reader acts
 * on — a bare number in a comment costs one reading, and a bare number in an abort message sends whoever is
 * standing at the crash to the wrong document. --unanchored lists exactly those.
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
 * SIX MARKUP FAMILIES, ONE INDEX SHAPE. The standards this tree cites are generated by six different
 * pipelines and each needs its own reader, but all six produce the same {sections, dfns, uses} index, so the
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
 *   — w3c-chapters (CSS 2.1, CSS 2.2): the standard is ONE document served as twenty-six chapter files, in
 *     hand-written 1998 HTML whose tags are mixed-case. It is the only family here where a section's TEXT and
 *     a section's NUMBER come from different fetches, so it is the only one that can be half-parsed — see
 *     regenW3cChapters for the two checks that make that impossible and for the three decisions a
 *     multi-document standard forces (one index per standard, the chapter list READ from the standard's own
 *     TOC, and the per-chapter agreement between that TOC and the chapter's own headings).
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
 * AND THE NUMBER IS THE CHEAP HALF. Everything above judges a NUMBER — does the standard have it, does the
 * term live there, does the title match — and a citation can pass all three while the SENTENCE beside it was
 * never written by anybody. That is the axis the QUOTATION CHECK asks about, and it is the one a reader trusts
 * most and verifies least: a quoted sentence appears to remove the need to open the spec at all, so a wrong
 * number sends a reader to the wrong place where they find out, and a fabricated quotation tells them not to go.
 * IT RUNS ON EVERY RUN, AND IT USED TO BE A FLAG — WHICH IS THE WHOLE OF WHY THIS PARAGRAPH IS HERE. A check
 * reached only by remembering a flag catches what it catches BY LUCK, and that is not a figure of speech: the
 * defect that put this paragraph here was found because one lane happened to pass `--quotes` while writing a
 * CSSOM View comment, and the same run turned up a SECOND, older site making the same false claim that had been
 * standing unread. Nobody was going to pass that flag on the day the next fabrication landed. The two
 * objections to running it always are both answerable and both were measured rather than argued: it costs
 * 29.7 s -> 33.9 s on the default target, which is 14% for the only evidence that can answer this question;
 * and it needs no network, because the corpus is COMMITTED exactly as the section index is, so the default path
 * fetches nothing and an upstream edition still cannot redden the tree. The flag is DELETED rather than kept as
 * an alias — CLAUDE.md's rule for a superseded mechanism — so there is no spelling of this run that skips it.
 * Two were measured before this existed and both had been standing, trusted: a CSSOM §6.1.1 citation beside
 * "whether the style sheet is applied", which appears nowhere in CSSOM, and a Web IDL §3.6 step quoted as
 * "if the argument is optional and it has a default value, set the value to that default", which appears
 * nowhere in Web IDL. Every check above passed both: the numbers resolve, the sections are real, the terms
 * are the standards' own.
 * IT NEEDS THE SPEC'S TEXT AND THE INDEX ABOVE DOES NOT HOLD ANY, so --regen writes a SECOND artifact,
 * engine/specindex/text/<key>.json, holding each section's own words as the token stream the check compares —
 * see tokenText for why storing the comparison form rather than the prose is what makes the committed file
 * mean what the checker means. It costs about eight megabytes across the nineteen standards, which is the
 * honest price of the only evidence that can answer this question, and it is kept OUT of the resolver index so
 * that a standard with no corpus is a NOT-CHECKED state the filesystem can represent rather than a silence.
 * WHAT IT CANNOT SEE, stated because a checker trusted past its evidence is worse than none: it reads a
 * quotation the way this tree writes one, between double quotes, so a paraphrase with no quotation marks
 * makes no claim it can falsify; it cannot tell a quotation of a STANDARD from a quotation of this tree's own
 * prose, so a comment quoting a DFAIL message under a citation lands in the same bucket as a fabrication and
 * the divergence evidence is what a reader triages on; and it says NOTHING about whether the sentence is
 * TRUE of the tree or whether the claim it supports is right — a correctly-pasted sentence supporting a
 * spec-wrong conclusion is invisible to it, exactly as it is to every other check here.
 * AND ONE MORE, WHICH IS A RESIDUAL RATHER THAN A LIMIT OF THE EVIDENCE: a quotation that is REAL BUT CUT is
 * VERIFIED, because every fragment it carries does occur, contiguously, exactly where it says. That is the
 * right answer to the question this check asks and the wrong answer to the one a reader has, whenever the cut
 * falls where the sentence turns — DOM §2.2's set-the-canceled-flag ends "…then set event's canceled flag, and
 * do nothing otherwise", and the dropped half IS the passive-listener guarantee. This tree's two copies of it
 * were caught only because they also ADDED two words; a clean truncation would have passed in silence, and
 * that is how its absence shows: a quotation ending at a comma, VERIFIED, whose standard goes on to negate it.
 * WHAT THE NEXT DIFF BUILDS is a SENTENCE-BOUNDARY side-channel written by --regen beside each section's token
 * stream — the token offsets at which the standard's own sentences end — plus a verdict that fires only where
 * the QUOTATION ITSELF ENDS IN A FULL STOP, which is the author claiming a whole sentence, and the corpus has
 * no boundary at the point the match ended. It is a side-channel and not a marker inside `sections` for a
 * measured reason: a standard's algorithm STEPS are list items, so a quotation of two consecutive steps types
 * a period the document does not contain, and a boundary token inside the compared stream would report every
 * such quotation as diverging — the normalizer manufacturing the finding, which the STEP_MARKER paragraph
 * below already records as this checker's one unacceptable failure.
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
/* THE QUOTATION CORPUS IS A SECOND ARTIFACT, NOT A SECOND TOOL — and it lives beside the resolver index rather
 * than inside it because a standard can have one and not the other, and that difference has to be
 * REPRESENTABLE. Both are read by every run now that the quotation check is not a mode, so the split no longer
 * buys speed — it buys the NOT-CHECKED state: a standard with a section index and no text corpus is counted and
 * printed as unchecked, which is what CLAUDE.md demands in place of a silent zero. Merging them would make that
 * state unspellable, and a standard whose words nobody holds would then be indistinguishable from one whose
 * quotations all passed. */
const TEXT_DIR = join(INDEX_DIR, "text");
const textFileOf = (key) => join(TEXT_DIR, key + ".json");

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
  /* THE FILE SYSTEM STANDARD, PROMOTED OUT OF OTHER_SPECS, AND THE PROMOTION IS THE WHOLE POINT: a foreign row
     STOPS the resolver guessing and an indexed row ANSWERS. Both were needed and in that order. The foreign row
     ended a WRONG ANSWER — `file_system_writable.c` cites this standard's §2.5 `write a chunk` in its
     step-machine strings and the file vote, in a streams-dense file, read every one as Streams §2.5 "Internal
     queues and queuing strategies" — but it bought that with silence, and this standard is one whose citations
     are LABELS A CRASH PRINTS: a step machine's stage names ride a parked flow to the cold tier and come back
     in a `@WHY`. Silence there is the shape CLAUDE.md calls a silent zero rather than a clean bill.
     WHAT THE SILENCE WAS HIDING, and the reason this row is worth more than its citation count: THIS STANDARD
     ENDS AT §3. The twenty-seven headings this row commits are §1 Introduction, §2 Files and Directories with
     its interface subsections, and §3 Accessing the Bucket File System — so every `§4.x` written under its
     name is a number the standard does not have, and an anchored citation is exactly what the UNKNOWN-SECTION
     check is asked of. A foreign row cannot ask it; only an index can.
     ITS ANCHORS ARE THE TWO-WORD NAME AND THE ABBREVIATION THIS TREE ACTUALLY WRITES. anchorTokens strips a
     trailing `Standard`, so `File System Standard §2.4.1` and `File System §2.5` are one spelling by the time
     classifyAnchor sees them; `FS §2.2` is the other, and it is admitted because a tail is only ever a WHOLE
     word — the token scan requires whitespace between words, so `refs`/`prefs` can never present a bare `fs`.
     AND IT IS THE `database` HAZARD — a common noun this tree writes in prose — SO IT WAS CHECKED RATHER THAN
     ASSUMED: every `File System §` in engine/host is a citation of this standard, and the noun is written
     without a number. `file system access` and `fsa` stay on the foreign list one screen below, and must: that
     is a DIFFERENT document whose numbers collide with this one head-on. */
  { key: "fs", label: "File System Standard", kind: "bikeshed",
    base: "https://fs.spec.whatwg.org/", anchors: ["file system", "fs"] },
  { key: "permissions", label: "Permissions", kind: "respec",
    base: "https://w3c.github.io/permissions/", anchors: ["permissions"] },
  /* PERMISSIONS POLICY IS A DIFFERENT STANDARD FROM PERMISSIONS AND THE ROW ABOVE DOES NOT COVER IT — which is
     the silent-zero shape this table's own comment describes, arriving through a NEAR MISS rather than an
     absence. `engine/specindex/permissions.json` indexes w3c.github.io/permissions, whose deepest heading is
     §8 and which has no §9 at all, so every `Permissions Policy §9.x` in the tree resolved against nothing and
     was counted under OTHER_SPECS — including a DFAIL that instructed the next reader to build §9.6 over §9.1,
     §9.2 and §4.7. Seven numbers, all correct as it happens, and not one of them checkable.
     ITS ANCHOR IS THE TWO-WORD NAME AND MUST BE, because anchorTokens reads the longest tail first and
     `permissions` alone is the row above: a one-word anchor here would make the two standards indistinguishable
     in the direction that judges §9.6 against a standard that stops at §8. It is bikeshed like the other
     W3C-hosted rows, so it needs no reader of its own. */
  { key: "permissionspolicy", label: "Permissions Policy", kind: "bikeshed",
    base: "https://w3c.github.io/webappsec-permissions-policy/", anchors: ["permissions policy"] },
  /* AN UNINDEXED STANDARD DOES NOT ONLY LOSE COVERAGE — WHERE AN INDEXED ONE DEFINES AN ADJACENT TERM, IT
     MANUFACTURES FINDINGS AGAINST CORRECT CITATIONS, and that is the cry-wolf direction this file's header
     names as the one that gets a checker muted. Streams and Permissions Policy were silent zeros: their
     citations went to OTHER_SPECS and nothing was asserted about them. High Resolution Time was not silent.
     Its citations name `time origin`, `coarsen time` and `current high resolution time`, and HTML defines an
     environment settings object's `time origin` at §8.1.3.2 — so with no hr-time index to say that §4 is
     itself titled "Time Origin", every one of those sites resolved BY ITS TERM to HTML and was reported as a
     misattribution — eight of them, every one correct as written, plus the forty-four sites that shared their
     number and were reported as undecided against it. The shape to keep: a standard is a candidate for
     this table not when the tree cites it OFTEN but when the vocabulary it owns is vocabulary some indexed
     standard also defines, because that is when the resolver has somewhere wrong to go.
     ITS ANCHOR IS THE HYPHENATED SHORT NAME AND THE THREE-WORD FULL NAME. `time` alone is a word this tree
     writes in prose constantly and would swallow every unanchored citation near it; both listed spellings are
     the ones the tree actually writes in front of a §. It is bikeshed, so it needs no reader of its own. */
  { key: "hrtime", label: "High Resolution Time", kind: "bikeshed",
    base: "https://w3c.github.io/hr-time/", anchors: ["hr-time", "hrtime", "high resolution time"] },
  /* THE FIRST LEVELLED CSS MODULE IN THIS TABLE, AND THE LEVEL IS PART OF THE ANCHOR RATHER THAN NOISE ON IT.
     A CSS module's levels are different documents with different numbering that this tree cites SIDE BY SIDE:
     css-images-3 §2 "Image Values: the <image> type" is `<url> | <gradient>`, and css-images-4 §2 "2D Image
     Values: the <image> type" widens the same production — so an anchor of `css images` would answer BOTH with
     Level 3's numbers, which is a wrong answer rather than a coverage gap. That is why LEVELLED exists and why
     this row is anchored ONLY by the hyphenated levelled shortname: it is the one spelling that names a
     document. THE CONSEQUENCE IS THAT `CSS Images 3 §4.1` — a spelling this tree also writes — is NOT covered
     by this row, and must not be made so by trimming the trailing level, which would collapse it onto the same
     name Level 4 trims to. Those sites are normalized at the citation, not papered over here.
     WHAT THE SILENCE COST, since a levelled shortname is recognized by a PATTERN and so can reach a real site
     count with nobody having written its name down anywhere: a DFAIL naming the six-arm css-images-4 <image>
     production stood beside a component implementing the two-arm css-images-3 one, and the audit reported
     nothing about either — an unindexed standard's citations are counted and never checked. */
  { key: "cssimages3", label: "CSS Images Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-images-3/", anchors: ["css-images-3"] },
  /* THE REST OF THE CSS MODULES THIS TREE LAYS OUT WITH, AND THEY WERE THE LARGEST SILENT ZERO IN THIS TABLE.
     Every one of them is bikeshed on drafts.csswg.org, so each is a row and one curl and no reader — which is
     exactly why leaving them out was the shape CLAUDE.md refuses: the cost of coverage was a line of table and
     the cost of the silence was a whole directory. `core/layout` alone carried 1981 citations of which 339
     named a standard nothing here indexed, and the audit reported ZERO findings over it, which reads as a
     clean bill.
     AND THE SILENCE WAS NOT MERELY MISSING COVERAGE — IT WAS MANUFACTURING WRONG ANSWERS, the hrtime row's
     shape one screen up. `used_value.c` cites `§1.1 "Module interactions"` of css-sizing-3, unanchored; with
     css-sizing-3 unindexed the ONLY standard here whose §1.1 carries that title was CSS Images 3, so the title
     channel resolved it to a document the comment does not name and the quotation check then measured its
     css-sizing-3 sentence against CSS Images 3's words. A second standard holding the same title is what turns
     that confident wrong answer back into an honest undecided.
     ORDERED BY LEVEL WITHIN A MODULE, ANCHORED ONLY BY THE HYPHENATED LEVELLED SHORTNAME, for the reason the
     css-images-3 row states above: two levels of one module are two documents with two numberings, and an
     unlevelled `CSS Text §3.1` names neither. Those sites are normalized at the citation. */
  { key: "cssvalues4", label: "CSS Values and Units Module Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-values-4/", anchors: ["css-values-4"] },
  { key: "csssizing3", label: "CSS Box Sizing Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-sizing-3/", anchors: ["css-sizing-3"] },
  { key: "csstext3", label: "CSS Text Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-text-3/", anchors: ["css-text-3"] },
  { key: "csstext4", label: "CSS Text Module Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-text-4/", anchors: ["css-text-4"] },
  { key: "csswritingmodes4", label: "CSS Writing Modes Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-writing-modes-4/", anchors: ["css-writing-modes-4"] },
  { key: "cssinline3", label: "CSS Inline Layout Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-inline-3/", anchors: ["css-inline-3"] },
  { key: "cssoverflow3", label: "CSS Overflow Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-overflow-3/", anchors: ["css-overflow-3"] },
  { key: "cssdisplay3", label: "CSS Display Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-display-3/", anchors: ["css-display-3"] },
  { key: "cssflexbox1", label: "CSS Flexible Box Layout Module Level 1", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-flexbox-1/", anchors: ["css-flexbox-1"] },
  { key: "cssgrid2", label: "CSS Grid Layout Module Level 2", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-grid-2/", anchors: ["css-grid-2"] },
  { key: "cssposition3", label: "CSS Positioned Layout Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-position-3/", anchors: ["css-position-3"] },
  { key: "cssbackgrounds3", label: "CSS Backgrounds and Borders Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-backgrounds-3/", anchors: ["css-backgrounds-3"] },
  { key: "csstransforms1", label: "CSS Transforms Module Level 1", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-transforms-1/", anchors: ["css-transforms-1"] },
  { key: "csscascade5", label: "CSS Cascading and Inheritance Level 5", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-cascade-5/", anchors: ["css-cascade-5"] },
  { key: "csssyntax3", label: "CSS Syntax Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-syntax-3/", anchors: ["css-syntax-3"] },
  { key: "cssfonts4", label: "CSS Fonts Module Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-fonts-4/", anchors: ["css-fonts-4"] },
  { key: "csscolor4", label: "CSS Color Module Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-color-4/", anchors: ["css-color-4"] },
  /* THE TWO EDITIONS OF CSS 2, AND THEY ARE THE FIRST STANDARD HERE WHOSE DOCUMENT IS SPLIT ACROSS CHAPTERS AND
     WHOSE PIPELINE PREDATES EVERY GENERATOR ABOVE — see regenW3cChapters for how a multi-document standard is
     indexed and why the chapter list is READ rather than listed.
     THEY ARE TWO ROWS BECAUSE THEY ARE TWO DOCUMENTS THAT DISAGREE ABOUT THEIR OWN TEXT, which is the strongest
     form of the css-images-3/4 argument in this table and was measured rather than assumed: CSS 2.1 §10.3.2
     says "the used value of 'width' is undefined in CSS 2.1" and CSS 2.2 §10.3.2 says "undefined in CSS 2.2",
     one word apart in a sentence this tree quotes verbatim. A single `css2` row would VERIFY that quotation
     against whichever edition happened to be fetched and report the other as a fabrication.
     W3C SERVES CSS 2.1 UNDER A SHORTNAME THAT IS NOT THE ONE THIS TREE WRITES: `/TR/CSS21/` answers 301 to
     `/TR/CSS2/`, which is the Recommendation, so THAT is the base — a base recorded as the redirect's source
     would make every fetch depend on a redirect staying put. `/TR/CSS22/` is its own First Public Working
     Draft and is not redirected anywhere. */
  { key: "css21", label: "Cascading Style Sheets Level 2 Revision 1 (CSS 2.1)", kind: "w3c-chapters",
    base: "https://www.w3.org/TR/CSS2/", anchors: ["css-2-1"] },
  { key: "css22", label: "Cascading Style Sheets Level 2 Revision 2 (CSS 2.2)", kind: "w3c-chapters",
    base: "https://www.w3.org/TR/CSS22/", anchors: ["css-2-2"] },
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

/* AN ABSTRACT OPERATION'S DEFINITION CARRIES ITS SIGNATURE AND EVERY CITATION OF IT CARRIES ONLY ITS NAME, AND
 * THAT MISMATCH WAS A WHOLE STANDARD'S WORTH OF SILENCE. A bikeshed dfn is indexed by the text inside it, and
 * Streams writes each operation as `<dfn …>WritableStreamAbort(<var>stream</var>, <var>reason</var>)</dfn>` — so
 * the key is `writablestreamabort( stream reason )` while the phrase this tree writes, `§5.2 WritableStreamAbort
 * step 2`, matches NOTHING. 175 of Streams' 271 terms are keyed that way. Measured on the file that prompted
 * this: 28 wrong section numbers, hand-found, of which the audit reported ZERO — not because it judged them and
 * was wrong, but because no term was ever found to judge them against. Web IDL, IndexedDB, File API and the
 * File System Standard are keyed identically.
 *
 * SO THE NAME IS INDEXED BESIDE THE SIGNATURE — AT INDEX TIME, GATED BY THE STANDARD'S OWN DECLARATION, AND
 * THAT CHOICE IS THE WHOLE OF THE PRECISION. The obvious cheaper fix is to strip the argument list at MATCH
 * time, needing no refetch, gating the resulting one-word name on the tree's own CASING — an internal
 * lower-to-upper transition, which is what `nameIsIdentifier` already tests and why ECMAScript is allowed
 * one-word terms. IT WAS BUILT AND MEASURED AND IT IS WRONG, in precisely the shape ES_CALLABLE's own comment
 * records: stripping every parenthesised key admits IDL members and type names as operation names, and the
 * casing test cannot tell them apart because they are spelled identically. It raised `TypeError` and
 * `ReferenceError` against ECMAScript clauses that merely THROW them — the exact false attribution ES_CALLABLE
 * exists to prevent, re-manufactured one layer down — plus `attachShadow`, `appendChild`, `insertBefore`,
 * `postMessage`, `insertAdjacentHTML` and the `ReadableStream` interface itself, every one of them vocabulary a
 * correct comment names while citing a section that is about something else.
 * THE STANDARD ALREADY DRAWS THE LINE: bikeshed stamps `data-dfn-type` on every dfn, and Streams carries 163
 * `abstract-op` against 30 `method`, 20 `attribute`, 15 `interface`, 11 `dictionary` and 8 `constructor`. So
 * this reads the declaration rather than re-deriving it from a spelling — the same argument ES_CALLABLE makes,
 * applied to the readers that had no such gate. Every one of the false positives above is a `method` or an
 * `interface`; not one is an `abstract-op`.
 * THE CASING GATE STAYS, AT THE MATCH, AND IS NOT REDUNDANT WITH THIS ONE — the two guard different questions.
 * This gate asks whether the STANDARD calls the thing an operation. The casing gate asks whether the AUTHOR
 * wrote a name rather than an English word, and it is load-bearing the day a standard declares a lowercase
 * one-word abstract-op: the File System Standard's scoped `take` and `release` are exactly that shape, and
 * `release` in running prose must never resolve to one.
 * A BARE NAME IS ALSO WHAT THE USE SCAN COUNTS. `uses` is keyed by whatever spelling `idToTerm` maps the dfn's
 * id to, and an operation's uses filed under a signature nothing ever writes are uses nothing can read — so an
 * abstract-op's id maps to its BARE name, and the confirmation-by-prominent-use channel follows the citation. */
function addOp(ops, bare, sec) {
  if (!bare || bare.length > 90) return null;
  const list = ops.get(bare) || [];
  if (!list.includes(sec)) list.push(sec);
  ops.set(bare, list);
  return bare;
}

/* The name a dfn declares, with its argument list removed. Bikeshed writes the arguments as <var> children, so
 * the tag strip that normTerm already performs leaves them inside the parentheses; everything before the first
 * one is the name. A `data-lt` spelling is the same name written out (`[[CancelSteps]]`), so it is read too. */
function opNames(content, lt) {
  const out = [];
  for (const raw of [content, ...(lt ? decodeEntities(lt).split("|") : [])]) {
    const t = normTerm(String(raw).split("(")[0]);
    if (t && !out.includes(t)) out.push(t);
  }
  return out;
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

/* ---- the quotation corpus: a section's own words ---------------------------------------------------------- */

/* WHAT IS STORED IS WHAT IS COMPARED, AND THAT IDENTITY IS THE WHOLE RELIABILITY OF THE CHECK. A quotation in a
 * C comment and the same sentence in a standard differ in every way except their words: the comment wraps the
 * line and prefixes it with `*`, the standard splits the same phrase across <a>, <var> and <code> so a tag
 * strip leaves `sheet ’s media` where the comment wrote `sheet's media`, one side spells an apostrophe curly
 * and the other straight, and the standard's own generator emits &amp; where the author typed &. Every one of
 * those is punctuation or markup, and NONE of them is a word. So both sides reduce to the same thing — a
 * lowercase stream of alphanumeric runs, single-spaced — and the corpus stores that stream rather than the
 * prose it came from. A reader who greps the committed file is then reading EXACTLY the bytes the check reads,
 * with no second normalization standing between the artifact and the answer. */
/* A LIST MARKER IS NOT TEXT ON EITHER SIDE, AND THAT ASYMMETRY WAS MANUFACTURING FINDINGS BY THE DOZEN. A
 * standard numbers its algorithm steps with a CSS counter, so `1.` and `2.` exist in the RENDERING and not in
 * the document — while a comment quoting those steps types the numbers out, because that is what the reader
 * of the page saw. The quotation then carries tokens the corpus cannot have, and the comparison breaks at its
 * SECOND word every time: a fifty-seven-word quotation of the COOP matching algorithm, three of the permissions
 * policy's own numbered steps, `1. Let attribute be this's tabindex attribute` — every one of them pasted
 * faithfully and every one reported as diverging immediately.
 * IT IS STRIPPED FROM BOTH SIDES RATHER THAN FROM THE QUOTATION, and that is the whole of the correctness. A
 * marker looks exactly like the end of a sentence that happens to finish on a number (`must be 0. Return
 * true`), so a rule applied to one side only would delete a token the other side still has and turn a
 * matching pair into a miss — the normalizer inventing the defect from the other direction. Applied to both,
 * such a sentence loses the same token on both sides and still compares equal. */
const STEP_MARKER = /\b\d+(?:\.\d+)*\.\s+(?=[A-Z])/g;

/* A TAG INSIDE A WORD IS NOT A SPACE, AND READING IT AS ONE MADE THE CORPUS DISAGREE WITH THE DOCUMENT ITSELF.
 * The paragraph above rests on the corpus holding the standard's own WORDS, and a blanket tag-to-space rule
 * breaks exactly that where a standard marks up PART of a word: `<code><a>iframe</a></code>s` renders as
 * `iframes` and stored as `iframe s`, `<a>URL</a>s` as `url s`. The quotation side has no markup, so a comment
 * that pasted what the page displays carries `iframes` — one token where the corpus has two — and the
 * comparison breaks at that word. That is the normalizer manufacturing the finding, which the STEP_MARKER
 * paragraph above already names as the one failure mode this checker must not have. Measured across this
 * tree at the revision this landed: 11 quotations reported NOT-FOUND and 9 more left unverified, every one of
 * them pasted correctly.
 * THE RULE IS CSS's, NOT A LIST OF SPELLINGS: an INLINE box does not break a word and a BLOCK box does. So a
 * RUN of tags between two word characters strips to nothing unless some tag in the run SPLITS — a run rather
 * than a single tag because the artifact is usually two closing tags deep (`</a></code>s`), and the split list
 * is what keeps `word<br>word` and a table's `a</td><td>b` from being welded into one token. A run that is not
 * between two word characters is a space exactly as before.
 * `sup` AND `sub` ARE INLINE AND SPLIT ANYWAY, and they are the one place where the reader's eye and the token
 * stream must disagree: `2<sup>53</sup>` is a POWER, not the number 253, and every author transcribing it into
 * a comment types `2^53`, which normalizes to two tokens. Welding it produced exactly the false finding this
 * rule exists to remove — IndexedDB §2.11's key-generator sentence, quoted verbatim in three places, reported
 * as diverging at its own `2`. A rule about markup that ignores what the markup MEANS is a rule that trades one
 * manufactured finding for another. */
const SPLIT_TAG = /^<\/?(?:br|p|div|li|ul|ol|dl|dt|dd|tr|td|th|table|thead|tbody|tfoot|caption|col|colgroup|h[1-6]|pre|blockquote|section|article|aside|nav|figure|figcaption|hr|form|fieldset|legend|main|header|footer|address|details|summary|dialog|template|option|optgroup|body|html|head|sup|sub)\b/i;
const ONE_TAG = /<[^>]*>/g;
function stripMarkup(html) {
  return html.replace(/(?:<[^>]*>)+/g, (run, at) => {
    const before = html[at - 1], after = html[at + run.length];
    if (!before || !after || !/[A-Za-z0-9]/.test(before) || !/[A-Za-z0-9]/.test(after)) return " ";
    for (const t of run.match(ONE_TAG) || []) if (SPLIT_TAG.test(t)) return " ";
    return "";
  });
}

function tokenText(html) {
  return decodeEntities(
    stripMarkup(String(html)
      .replace(/<!--[\s\S]*?-->/g, " ")
      .replace(/<(script|style)\b[^>]*>[\s\S]*?<\/\1\s*>/gi, " ")))
    .replace(/\s+/g, " ")
    .toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();
}

/* A SECTION'S TEXT ENDS WHERE THE NEXT HEADING BEGINS, AND AN UNNUMBERED HEADING IS A HEADING. Without that
 * second half every bikeshed standard's LAST numbered section swallows the whole back matter — the Index, the
 * IDL Index, the References — which is thousands of words of the standard's own vocabulary, filed under one
 * number. A quotation checked against it would VERIFY against text that section does not contain, and a false
 * VERIFY is the silent direction: the tool would certify a fabrication. So every <hN> and every <footer> is a
 * boundary, and only the ones the reader numbered start a section. */
function withBoundaries(body, marks) {
  const at = new Set(marks.map((m) => m.at));
  const stops = [];
  const re = /<(?:h[1-6]|footer)\b/gi;
  for (let m; (m = re.exec(body)); ) if (!at.has(m.index)) stops.push({ at: m.index, no: null });
  return [...marks, ...stops].sort((a, b) => a.at - b.at);
}

/* A SECTION CONTAINS ITS SUBSECTIONS, and this stores each section's OWN slice so that containment is a JOIN
 * the audit performs rather than a duplication the corpus carries. The slices of a section and its descendants
 * are contiguous in the document, so concatenating them in numeric order reproduces the original stream — no
 * artificial adjacency is created at a boundary, and no byte is stored twice. */
function collectText(body, marks, into) {
  const b = withBoundaries(body, marks);
  for (let i = 0; i < b.length; i++) {
    if (!b[i].no) continue;
    const end = i + 1 < b.length ? b[i + 1].at : body.length;
    const t = tokenText(body.slice(b[i].at, end));
    if (!t) continue;
    into.set(b[i].no, into.has(b[i].no) ? into.get(b[i].no) + " " + t : t);
  }
}

/* The corpus records the SAME two staleness facts the resolver index does, and the audit refuses to use a
 * corpus whose pair disagrees with its index's — a text file regenerated at a different edition from the
 * section numbers it is keyed by would answer questions about a document no single fetch ever saw. */
function writeText(spec, sections, texts, specUpdated) {
  mkdirSync(TEXT_DIR, { recursive: true });
  const out = { spec: spec.label, key: spec.key, base: spec.base, specUpdated: (specUpdated || "").trim(),
    fetched: new Date().toISOString().slice(0, 10),
    sections: Object.fromEntries([...texts].sort((a, b) => cmpNo(a[0], b[0]))) };
  writeFileSync(textFileOf(spec.key), JSON.stringify(out, null, 1) + "\n");
  let words = 0, missing = 0;
  for (const t of texts.values()) words += t.split(" ").length;
  for (const no of sections.keys()) if (!texts.has(no)) missing++;
  console.log(`  ${spec.key} text: ${texts.size} sections, ${words} words` +
    (missing ? `, ${missing} numbered section(s) with no text of their own` : ""));
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
    `${Object.keys(out.ops).length} declared abstract operations, ` +
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

function finish(spec, sections, defs, uses, specUpdated, ops = new Map()) {
  /* The use map keeps the COUNT, not a boolean, because two different questions are asked of it and merging
   * them would answer both badly: USE_FLOOR links make a section the term's SUBJECT, so a citation landing
   * there is CONFIRMED; a section that links it merely twice is worth NAMING IN THE FINDING, so the reader
   * can see for themselves whether that mention is the one the comment meant. One mention is noise. */
  const kept = {};
  for (const [k, n] of uses) {
    if (n < 2) continue;
    const i = k.indexOf("\u0000");
    const term = k.slice(0, i), sec = k.slice(i + 1);
    const at = defs.get(term) || ops.get(term);
    if (!at) continue;                             /* a use of something this index does not define is noise */
    if (at.includes(sec)) continue;                /* the definition site is already the answer */
    (kept[term] = kept[term] || {})[sec] = n;
  }
  return {
    spec: spec.label, key: spec.key, base: spec.base, specUpdated: (specUpdated || "").trim(),
    fetched: new Date().toISOString().slice(0, 10),
    sections: Object.fromEntries([...sections].sort((a, b) => cmpNo(a[0], b[0]))),
    dfns: Object.fromEntries([...defs].sort((a, b) => (a[0] < b[0] ? -1 : 1))),
    /* THE OPERATION NAMES THE STANDARD ITSELF DECLARES — see addOp. A reader whose standard declares none
     * writes an empty object rather than omitting the field: an absent field would make an index written by an
     * older reader indistinguishable from a standard that has no abstract operations, and the audit asserts
     * this field's presence precisely so those two cannot be confused. */
    ops: Object.fromEntries([...ops].sort((a, b) => (a[0] < b[0] ? -1 : 1))),
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

  const defs = new Map(), uses = new Map(), idToTerm = new Map(), bodies = [], texts = new Map();
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
  for (const b of bodies) { scanUses(b.body, b.marks, idToTerm, uses); collectText(b.body, b.marks, texts); }
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
  writeText(spec, sections, texts, updated);
}

/* ---- reader: W3C chapter-split HTML (CSS 2.1, CSS 2.2) --------------------------------------------------- */

/* HOW A MULTI-DOCUMENT STANDARD IS INDEXED, STATED HERE RATHER THAN LEFT IMPLICIT IN THE DATA. Three decisions,
 * and each of them is a rule some other shape of this reader would have broken:
 *   — ONE STANDARD IS ONE INDEX, KEYED BY THE STANDARD. A citation writes `CSS 2.2 §10.3.3` and never names a
 *     chapter, because the chapter is an artefact of how W3C serves the document and not a thing the standard
 *     numbers. So the key is the standard and the chapter is recorded PER SECTION, in the `page` field the index
 *     shape already carries for the WHATWG multipage reader — which is what lets a finding name the file to
 *     open without the chapter ever entering the citation's vocabulary.
 *   — THE CHAPTER LIST IS READ FROM THE STANDARD'S OWN TABLE OF CONTENTS, NEVER LISTED HERE. A hand-kept list of
 *     twenty-six filenames is a second copy of a fact the document already states, and CLAUDE.md's rule for an
 *     auditor is that it DERIVES what it checks from the source of truth rather than restating it: the copy that
 *     drifts is the one nobody runs against reality, and a chapter dropped from such a list is a silent zero
 *     inside a standard that reports as indexed.
 *   — THE PARSE IS CHECKED AGAINST THE PAGE, TWICE, because a multi-document parse can fail per chapter and a
 *     whole-standard total would hide it. Every `tocxref` anchor must yield a section label, and then for EVERY
 *     chapter the set of sections the TOC files under it must equal the set of labelled headings the chapter
 *     itself carries. That second check is the one that earns its place: the TOC's fragment for CSS 2.2 §8.5.2
 *     is `#border-color-properties` and the chapter's anchor is `#border-color`, so a reader that had located
 *     each section by the TOC's own link would have silently dropped three sections of the box model — the
 *     standard's generated TOC disagreeing with the standard's text, which no floor and no total would show.
 * AND THE MARKUP IS 1998 HTML, SO EVERY TAG SCAN HERE IS CASE-INSENSITIVE. `box.html` writes `<H2>` and `<A
 * name=…>` where `visudet.html` writes `<h2>` and `<a name=…>` — in the same document, from the same generator —
 * and a case-sensitive anchor scan finds the first and not the second. Nothing above this line needs the rule
 * because bikeshed, respec and the WHATWG pipeline all emit lowercase; this reader is the one that meets a
 * document old enough to predate the convention. */
/* THE LABEL A SECTION OF THIS STANDARD CARRIES, and the letters are not decoration: CSS 2's appendices number
 * themselves `A.11.1`, `E.2`, `G.3`, and the citation reader's own SEC pattern already admits `[A-F]` followed
 * by a dotted part — so a reader that indexed only the digits would leave every appendix citation resolving
 * against a section its own standard's index does not hold. `Appendix G.` heads a chapter and is the same fact
 * spelled long. */
const SEC_LABEL = /^(?:Appendix\s+)?([0-9]+(?:\.[0-9]+)*|[A-Z](?:\.[0-9]+)*)\.?(?:\s+|$)/;

function regenW3cChapters(spec) {
  const toc = curl(spec.base);
  /* THE STANDARD'S OWN STATEMENT OF WHAT IT IS — `W3C Recommendation 07 June 2011, edited in place …` for
   * CSS 2.1, `W3C First Public Working Draft 12 April 2016` for CSS 2.2. It carries the maturity as well as the
   * date, which matters here more than anywhere else in this table: these two documents differ by an edition
   * and the staleness fact is what says which one a committed corpus was cut from. */
  const status = /<h2\b[^>]*\bid="W3C-doctype"[^>]*>([\s\S]*?)<\/h2\s*>/i.exec(toc);
  if (!status) throw new Error(`${spec.key}: no W3C-doctype heading — the index would record no staleness fact`);
  const updated = stripTags(status[1]);

  const sections = new Map();
  const pages = new Set();
  let parsed = 0;
  const tocRe = /<a href=("[^"]*"|'[^']*'|[^ >]+)[^>]*\bclass="tocxref"[^>]*>([\s\S]*?)<\/a\s*>/gi;
  for (let m; (m = tocRe.exec(toc)); ) {
    parsed++;
    const txt = stripTags(m[2]);
    const label = SEC_LABEL.exec(txt);
    /* EVERY TOC ENTRY OF THIS STANDARD CARRIES A LABEL — the appendices are lettered (`Appendix G.`, `G.3`) and
     * the chapters numbered — so an entry with none is a parse that has run off an anchor, not a document that
     * has an unlabelled section. It throws for the same reason the secno count above does. */
    if (!label) throw new Error(`${spec.key}: TOC entry "${txt.slice(0, 60)}" carries no section label — the parse is wrong`);
    const no = label[1], title = txt.slice(label[0].length).trim();
    const page = attrHref(m[1]).split("#")[0];
    const had = sections.get(no);
    /* A chapter is listed twice — once in the overview, once at the head of its own subtree — so a repeat is
     * expected and a repeat that DISAGREES about the title is a parse that has run off the end of an anchor. */
    if (had && had.title !== title) throw new Error(`${spec.key}: §${no} parsed as both "${had.title}" and "${title}" — the parse is wrong`);
    if (!had) sections.set(no, { title, page, id: "" });
    if (page) pages.add(page);
  }
  const anchors = (toc.match(/class="tocxref"/gi) || []).length;
  if (parsed !== anchors) throw new Error(`${spec.key}: the TOC has ${anchors} tocxref anchors and the parse read ${parsed} — the parse is wrong`);

  const defs = new Map(), uses = new Map(), idToTerm = new Map(), bodies = [], texts = new Map();
  for (const page of [...pages].sort()) {
    const body = curl(spec.base + page);
    const want = new Set([...sections].filter(([, s]) => s.page === page).map(([no]) => no));
    const marks = [];
    const seen = new Map();
    const headRe = /<h([1-6])\b[^>]*>([\s\S]*?)<\/h\1\s*>/gi;
    for (let m; (m = headRe.exec(body)); ) {
      const inner = m[2];
      const label = SEC_LABEL.exec(stripTags(inner));
      if (!label || seen.has(label[1])) continue;
      /* The section's own anchor, taken from the HEADING rather than from the TOC's link to it — see the
       * §8.5.2 case above. It is what the renumbering report keys on, so a fragment the chapter does not
       * contain would make every section look renumbered on the next fetch. */
      const id = (/<a\s[^>]*\bname=("[^"]*"|'[^']*'|[^ >]+)/i.exec(inner) || [])[1];
      seen.set(label[1], id ? attrHref(id) : "");
      marks.push({ at: m.index, no: label[1] });
    }
    const missing = [...want].filter((no) => !seen.has(no));
    const extra = [...seen.keys()].filter((no) => !want.has(no));
    if (missing.length || extra.length)
      throw new Error(`${spec.key}: ${page} — the TOC files ${want.size} section(s) here and the chapter's own ` +
        `headings are a different set (TOC-only: ${missing.join(", ") || "none"}; chapter-only: ${extra.join(", ") || "none"}) — the parse is wrong`);
    for (const [no, id] of seen) if (id) sections.get(no).id = id;
    /* A DEFINITION IN THIS STANDARD IS SOMETIMES ITS OWN ANCHOR AND SOMETIMES WRAPS ONE. `<dfn id="propdef-width">`
     * and `<dfn><a name="x0">…</a></dfn>` are both spellings this document uses, and a use site links whichever
     * one it was given — so both are recorded, or half the standard's own cross-references count as uses of
     * nothing. */
    const dfnRe = /<dfn\b([^>]*)>([\s\S]{0,400}?)<\/dfn\s*>/gi;
    for (let m; (m = dfnRe.exec(body)); ) {
      const sec = secAt(marks, m.index);
      if (!sec || IMPORTED.test(m[2])) continue;
      const term = normTerm(m[2]);
      if (!keepTerm(term)) continue;
      addDef(defs, term, sec);
      const own = attrOf(m[1], "id");
      if (own) idToTerm.set(own, term);
      const inner = /<a\s[^>]*\bname=("[^"]*"|'[^']*'|[^ >]+)/i.exec(m[2]);
      if (inner) idToTerm.set(attrHref(inner[1]), term);
    }
    bodies.push({ body, marks });
    process.stderr.write(`  ${page}: ${marks.length} headings\n`);
  }
  for (const b of bodies) { scanUses(b.body, b.marks, idToTerm, uses); collectText(b.body, b.marks, texts); }
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
  writeText(spec, sections, texts, updated);
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

  const defs = new Map(), uses = new Map(), idToTerm = new Map(), ops = new Map();
  const dfnRe = /<dfn\b([^>]*)>([\s\S]{0,400}?)<\/dfn>/g;
  for (let m; (m = dfnRe.exec(body)); ) {
    const sec = secAt(marks, m.index);
    if (!sec) continue;
    if (IMPORTED.test(m[2])) continue;
    /* Bikeshed carries a dfn's other spellings in data-lt, pipe-separated — `attribute list|attribute lists`.
     * A comment naming the plural is naming the same definition, so every spelling is indexed. */
    /* RESIDUAL — `data-dfn-for` IS READ BY NOTHING HERE, so a QUALIFIED one-word definition is dropped along
     * with the common nouns keepTerm exists to drop. Bikeshed writes a scoped concept as a bare word plus its
     * owner: the File System Standard defines its lock operations as `<dfn data-dfn-for="file entry/lock">take
     * </dfn>` and the matching `release`, while its prose reads "To release a lock on a given file entry" — so
     * the phrase a comment writes is not a spelling the index holds, and `take`/`release` alone are exactly the
     * coincidence generators keepTerm refuses. WHAT THE NEXT DIFF BUILDS: index a `data-dfn-for` dfn under the
     * composite its qualifier names (`file entry/lock` + `release` -> `release a lock`, `lock release`), which
     * is multi-word and therefore admissible, so the qualifier earns the term its scope already implies.
     * HOW ITS ABSENCE SHOWS: `core/file/file_system.c` cites `§2.1's release a lock` in two DCHECK messages,
     * correctly, and the only standard whose index holds that phrase is Streams — which defines `release a
     * lock` at §2.6 "Locking" — so both are reported as misattributed to a document they do not name. */
    const spellings = [normTerm(m[2])];
    const lt = attrOf(m[1], "data-lt");
    if (lt) for (const alt of lt.split("|")) spellings.push(normTerm(alt));
    let primary = null;
    for (const t of spellings) { if (!keepTerm(t)) continue; addDef(defs, t, sec); if (!primary) primary = t; }
    /* THE STANDARD'S OWN DECLARATION, READ RATHER THAN GUESSED — see addOp. Only `abstract-op` earns a bare
     * name; `method`, `attribute`, `interface`, `constructor` and `dict-member` are vocabulary a correct
     * comment names while citing a section about something else, and admitting them was measured wrong. */
    let op = null;
    if (attrOf(m[1], "data-dfn-type") === "abstract-op")
      for (const b of opNames(m[2], lt)) { const k = addOp(ops, b, sec); if (k && !op) op = k; }
    const id = attrOf(m[1], "id");
    if (id && (op || primary)) idToTerm.set(id, op || primary);
  }
  scanUses(body, marks, idToTerm, uses);
  const texts = new Map(); collectText(body, marks, texts);
  console.error(`  ${spec.key}: ${marks.length} headings on one page, ${ops.size} declared abstract operations`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated, ops));
  writeText(spec, sections, texts, updated);
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

  const defs = new Map(), uses = new Map(), idToTerm = new Map(), ops = new Map();
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
    /* ReSpec stamps the same `data-dfn-type` bikeshed does, so the same declaration answers here — see addOp. */
    let op = null;
    if (attrOf(m[1], "data-dfn-type") === "abstract-op")
      for (const b of opNames(m[2], attrOf(m[1], "data-lt"))) { const k = addOp(ops, b, sec); if (k && !op) op = k; }
    const id = attrOf(m[1], "id");
    if (id && (op || primary)) idToTerm.set(id, op || primary);
  }
  scanUses(body, marks, idToTerm, uses);
  const texts = new Map(); collectText(body, marks, texts);
  console.error(`  ${spec.key}: ${marks.length} numbered headings on one page, ${ops.size} declared abstract operations`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated, ops));
  writeText(spec, sections, texts, updated);
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
  const texts = new Map(); collectText(body, marks, texts);
  console.error(`  ${spec.key}: ${marks.length} numbered headings, ${defs.size} definitions`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
  writeText(spec, sections, texts, updated);
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
 * word — §7.3.2 is `Get ( O, P )` and `get` would match half the comments in this tree.
 *
 * THE SPELLING TEST BELOW IS SHARED WITH THE AUDIT AND IS NOT ECMAScript'S ALONE, which is why it is named for
 * what it asks rather than for the reader that first needed it. Every bikeshed standard writes an abstract
 * operation's dfn WITH its argument list, so `WritableStreamAbort` in a comment is not the index's
 * `writablestreamabort( stream reason )` and matches nothing — see `addOp` above, which indexes the bare name
 * the standard declares, and the audit's `probe`, where this predicate is asked of the TREE'S OWN spelling to
 * decide whether the author wrote a NAME or an English word. */
const ES_CALLABLE = /^(abstract operation|internal method|built-in function|numeric method|host-defined abstract operation|implementation-defined abstract operation|syntax-directed operation)$/;
function nameIsIdentifier(raw) {
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
        if (!t || !(keepTerm(t) || nameIsIdentifier(raw))) continue;
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
  const texts = new Map();
  for (const b of bodies) { scanUses(b.body, b.marks, idToTerm, uses); collectText(b.body, b.marks, texts); }
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
  writeText(spec, sections, texts, updated);
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
    else if (spec.kind === "w3c-chapters") regenW3cChapters(spec);
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
  /* FILE SYSTEM ACCESS, and it is here because the audit's own gap report asked for it: `Access=32` stood in
     the capitalised-tokens line — "a standard among these is coverage this audit is not getting" — while
     `core/file/file_picker.c`, whose banner reads "FILE SYSTEM ACCESS §3", had every one of its §3.x crash
     messages filed under WEB IDL by the file vote. That file cites Web IDL correctly three times (§3.2.21
     sequences, §3.2.17 dictionary types, §3.3.13 [SecureContext]) and those anchored sites are what made IDL
     its dominant standard, so the vote answered `process accept types` with Web IDL §3.2.1 "any" and
     `remember a picked directory` with §3.2.2 "undefined". Both real numbers, neither the right document. */
  "file system access",
  /* AND ITS ABBREVIATION, WHICH BECAME LOAD-BEARING THE MOMENT THE FILE SYSTEM STANDARD GAINED AN INDEX ROW.
     The two documents' numbers collide head-on: FS §2.2 is "The FileSystemHandle interface" and FSA §2.2 is
     "Permissions"; FS §2.3 is "The FileSystemFileHandle interface" and FSA §2.3 is "The FileSystemHandle
     interface". `core/file/file_system_handle.h` writes `FS §2.2`/`FS §2.3` and `FSA §2.2`/`FSA §2.3` in
     adjacent lines, correctly. While neither ABBREVIATION was known — the spelled-out `File System Access` was
     on this list and its four letters were on no list at all — both fell to the file vote together and were
     wrong together. Indexing FS alone would have been WORSE THAN THAT rather than better: `fs` becomes that
     file's dominant anchor, so the four-letter sites keep falling to a vote and the vote now confidently
     answers FSA's numbers out of the File System Standard — a wrong answer manufactured by adding a right one.
     A foreign row is what refuses that: an `other:` anchor is never judged at all. */
  "fsa",
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
  /* A LEVEL SUFFIX IS PART OF THE EDITION AND NOT PART OF THE NAME, AND LEAVING IT ON DOES NOT DEGRADE THE
   * ANSWER — IT ERASES THE QUESTION. The tail regex below requires its last token to START WITH A LETTER, so
   * `HIGH RESOLUTION TIME Level 3 §4` and `CSS Syntax Level 3 §5.4` end in a digit and match NOTHING: the
   * citation comes back with no anchor at all, and an unanchored citation is never asked the one check that
   * needs an anchor — whether the standard HAS the number. A spelled-out level is the same fact as the
   * levelled shortname LEVELLED already reads out of `css-syntax-3`, so it is trimmed for the same reason
   * `Standard` is: the words after the name say which document, and the name is what decides which index. */
  const tail = flat.replace(/[\s'"’(\[]+$/, "").replace(/\s+(?:Level|level)\s+[0-9]+$/, "")
    .replace(/\s+(?:Standard|standard|spec|Spec)$/, "");
  /* AND A LEVEL WRITTEN WITHOUT THE WORD "Level" IS THE SAME FACT AGAIN, IN THE SPELLING THIS TREE ACTUALLY
   * USES — but it is JOINED to the name rather than trimmed off it, and that difference is the whole of why
   * this is allowed where the SPECS table's own comment refuses a trim. That comment is right: `CSS Images 3`
   * and `CSS Images 4` are different documents with different numbering, so a rule that drops the level
   * answers both with one index and manufactures wrong answers. Joining loses nothing — `css-color-4` is
   * exactly the levelled shortname LEVELLED already reads, so Level 4 and Level 5 stay two names.
   * WHAT THIS FIXES IS A CITATION THAT IS ALREADY CORRECT. `css_color.c` writes `CSS Color 4 §16.2's
   * serialization` inside a DFAIL — the number with its standard, which is what CLAUDE.md asks for — and the
   * tail regex below cannot end on `4`, so the site came back UNANCHORED and the file vote filed it under
   * HTML, whose §16.2 is a different document's section entirely. Twenty-nine of that one file's crash
   * messages were in that state. Editing correct prose to satisfy a tokenizer would be the churn this audit
   * exists to avoid; the tokenizer reads the spelling.
   * IT IS GATED ON A NAME THIS FILE ALREADY KNOWS, AND THE GATE IS NOT CAUTION — IT IS THE DIFFERENCE BETWEEN
   * ADDING A RESOLUTION AND DESTROYING ONE. An anchor classified `other:` is FOREIGN and is never audited at
   * all, so joining a level onto an unknown name would take a citation that is currently resolved by the term
   * it names and drop it out of the audit. `ECMA 262` and `HTML 5` are exactly that shape. So the join is
   * emitted only where the joined form is an indexed standard's own anchor, or where the base name is already
   * on the foreign list — in both cases the classification it produces is one this file had already decided. */
  /* THE JOIN MUST NOT ABSORB THE SENTENCE THAT LEADS INTO THE NAME, and the first spelling of this did:
   * `no keyword in CSS Color 4 §10` produced `in-css-color-4`, which LEVELLED happily accepts as a standard,
   * so one file's citations of one document were counted under three different names. A tail is only a NAME
   * when its first word can start one — the CSS modules all start `css`, and a single word answers for
   * itself — so a lead-in word is refused rather than hyphenated into the answer. */
  const nameStart = (w) => ANCHOR_TO_KEY.has(w) || OTHER_SPECS.includes(w);
  /* A VERSION IS NOT ALWAYS ONE INTEGER, AND THE STANDARD THIS TREE CITES MOST OFTEN IN ITS LAYOUT CODE IS THE
   * ONE THAT PROVES IT. `CSS 2.2` and `CSS 2.1` are the names of two documents, and a version group of
   * `[0-9]+` cannot end on either — so both came back with NO ANCHOR AT ALL and fell to the file vote, which
   * is how a CSS 2.2 quotation of the strut got measured against CSSOM View's words and reported as not found.
   * The dotted part joins as a hyphen for the same reason the space does: what LEVELLED reads is a shortname,
   * and `css-2-2` is that shortname for a document whose own name is written with a dot. The join stays gated
   * on a name this file already knows, so widening the version cannot invent a standard — `at 8.4 §9.2` still
   * produces nothing, because `at` is on no list. */
  const lv = /^(.*?)((?:[A-Za-z][A-Za-z0-9+-]*[ \t]+){0,2}[A-Za-z][A-Za-z0-9+-]*)[ \t]+([0-9]+(?:\.[0-9]+)*)$/.exec(tail);
  const joined = [];
  if (lv) {
    const w2 = lv[2].split(/[ \t]+/);
    for (let n = w2.length; n >= 1; n--) {
      const bw = w2.slice(w2.length - n).map((x) => x.toLowerCase());
      const base = bw.join(" ");
      const j = base.replace(/\s+/g, "-") + "-" + lv[3].replace(/\./g, "-");
      if (ANCHOR_TO_KEY.has(j) || OTHER_SPECS.includes(base) ||
          (OTHER_SPECS.includes(bw[n - 1]) && nameStart(bw[0]))) joined.push(j);
    }
  }
  const m = /((?:[A-Za-z][A-Za-z0-9+-]*[ \t]+){0,2}[A-Za-z][A-Za-z0-9+-]*)$/.exec(tail);
  if (!m) return joined;
  const w = m[1].split(/[ \t]+/);
  const out = joined;
  for (let n = w.length; n >= 1; n--) out.push(w.slice(w.length - n).join(" "));
  return out;                                       /* longest tail first */
}

/* A NAME SOME LIST ACTUALLY HOLDS BEATS A NAME ONLY A PATTERN ACCEPTS, AND THAT IS TWO PASSES RATHER THAN ONE.
 * The token list arrives longest-tail-first because a longer NAME must beat the shorter one inside it —
 * `namespaces in xml` stops at itself rather than reaching the `xml` that would judge it against a standard it
 * is not. LEVELLED is not a name, it is a SHAPE, and it accepts any hyphenated word ending in a digit: so
 * `inline box CSS 2.2` joins to `inline-box-css-2-2`, which LEVELLED accepts, and a single scan hands a correct
 * CSS 2.2 citation to a foreign standard nobody has ever heard of. Measured on `core/layout` the day CSS 2 was
 * indexed: eight such names, each holding one or two citations of a document this table does index.
 * SO THE LISTED NAMES ARE ASKED FIRST, LONGEST FIRST, AND THE SHAPE IS THE FALLBACK. `namespaces in xml` still
 * wins over `xml` because both are listed and the longer is asked first; `css-2-2` now wins over
 * `inline-box-css-2-2` because only one of them is a name. */
function classifyAnchor(toks) {
  for (const t of toks) {
    const w = t.toLowerCase();
    if (ANCHOR_TO_KEY.has(w)) return ANCHOR_TO_KEY.get(w);
    if (OTHER_SPECS.includes(w)) return "other:" + w;
  }
  for (const t of toks) {
    const w = t.toLowerCase();
    if (LEVELLED.test(w)) return "other:" + w;
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
    /* A `.md` IN THE COMPILED CONE IS A DESIGN NOTE THE C FILES DEFER TO, AND CLAUDE.md'S RULE IS THAT SUCH A
     * DOCUMENT IS CODE — a claim about this tree travels by reference, and nothing mechanical reports that the
     * document a crash points at has gone stale. The two at the repository root are added by name below; this
     * is the half that had no name anywhere. `engine/host/SPEC_STEPS.md` is cited BY NAME from three C files as
     * the authority on which spec steps exist and where the page's own code can run, it carries 726 citations —
     * more than any C file in the tree — and until this line not one of them was ever read by this auditor. */
    else if (/\.(c|h|md)$/.test(e)) out.push(p);
  }
  return out;
}

/* PROSE — comment bodies and string literals, the two places in a C file where a citation can live. A bare
 * dotted number is read only out of these, so a float literal, an array bound and a version in a Makefile-ish
 * define are never candidates. String literals count because a DFAIL message is prose that a reader follows
 * exactly like a comment, and its number must be right for the same reason. */
/* THE TWO PLACES A CITATION CAN LIVE ARE NOT WORTH THE SAME, so the span carries WHICH ONE it is. A citation
 * in a comment is read by whoever opens the file; a citation in a string literal is read by whoever is
 * standing at an abort with the message in front of them, and that reader has no file open and no neighbouring
 * citations to compare it against. CLAUDE.md's own worked example of the damage — a DFAIL that instructed the
 * next person to build something the spec makes unreachable, "SPEC-WRONG and had been followed once" — is a
 * message, not a comment. Third element: "c" for comment, "s" for a string literal. `inSpans` reads [0] and
 * [1] only and is unaffected. */
function proseSpans(src, path) {
  /* A MARKDOWN FILE IS ALL PROSE, AND ITS UNIT IS THE PARAGRAPH. CLAUDE.md's own rule is that a `.md` a C file
   * cites by name is CODE for the purposes of a claim about this tree, and a design note is where a reader
   * goes to learn WHY — so a fabricated quotation there is the highest-leverage one in the repository. Read
   * as C it is nonsense: every apostrophe opens a string literal. Read as ONE span it is worse than nonsense,
   * because a citation would then govern every quotation to the end of the file. A blank line is what ends a
   * paragraph, and a paragraph is the prose unit a comment block is the C analogue of. */
  if (path && /\.md$/i.test(path)) {
    const out = [];
    for (let i = 0; i < src.length; ) {
      let e = src.indexOf("\n\n", i);
      if (e < 0) e = src.length;
      if (e > i) out.push([i, e, "c"]);
      i = e + 2;
    }
    return out;
  }
  const spans = [];
  const n = src.length;
  for (let i = 0; i < n; ) {
    const c = src[i];
    if (c === "/" && src[i + 1] === "*") {
      const e = src.indexOf("*/", i + 2);
      spans.push([i + 2, e < 0 ? n : e, "c"]); i = e < 0 ? n : e + 2;
    } else if (c === "/" && src[i + 1] === "/") {
      let e = src.indexOf("\n", i + 2); if (e < 0) e = n;
      spans.push([i + 2, e, "c"]); i = e;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c) { if (src[j] === "\\") j++; if (src[j] === "\n") break; j++; }
      spans.push([i + 1, Math.min(j, n), "s"]); i = j + 1;
    } else i++;
  }
  return spans;
}

/* WHICH SPAN AN OFFSET IS IN, or null. Same bisection as inSpans; kept separate because inSpans answers a
 * membership question on a hot path and this one answers a reporting question. */
function spanAt(spans, at) {
  let lo = 0, hi = spans.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (spans[mid][1] <= at) lo = mid + 1;
    else if (spans[mid][0] > at) hi = mid - 1;
    else return spans[mid];
  }
  return null;
}

/* IS THIS CITATION IN A CRASH MESSAGE — asked of a literal span, by walking back to the statement that opens
 * it. A message is built from several adjacent literals, so the scan starts at the FIRST literal of the run
 * and stops at the nearest statement boundary; anything else would miss every continuation line, which is
 * where a long DFAIL puts most of its words. A hit is the macro's own name immediately before an open paren.
 * THE WALK SKIPS PROSE RATHER THAN READING IT, because a message whose own text contains a `;` or a `}` — and
 * this tree's messages quote code constantly — would otherwise stop the scan at a statement boundary that is
 * a character inside a string. That is the same defect as a `sed` over a citation: prose and program are
 * different languages sharing one file, and a scan that forgets which one it is in gets the wrong answer. */
const CRASH_MACRO = /\b(DCHECK|DFAIL|CHECK|CHECK_FAIL)\s*\($/;
function inCrashMessage(src, spans, at) {
  const sp = spanAt(spans, at);
  if (!sp || sp[2] !== "s") return false;
  let i = sp[0] - 2;                       /* before this literal's own opening quote */
  const floor = Math.max(0, i - 4000);
  while (i > floor) {
    const s2 = spanAt(spans, i);
    if (s2) { i = s2[0] - (s2[2] === "s" ? 2 : 3); continue; }
    const ch = src[i];
    if (ch === ";" || ch === "{" || ch === "}") return false;
    if (ch === "(" && CRASH_MACRO.test(src.slice(Math.max(0, i - 24), i + 1))) return true;
    i--;
  }
  return false;
}

/* ---- the quotation check -------------------------------------------------------------------------------- */

/* WHY A QUOTATION NEEDS ITS OWN CHECK, AND WHY IT IS THE ONE AXIS NOTHING ELSE HERE CAN ASK. Everything above
 * judges a NUMBER: does the standard have it, does the term live there, does the title match. A quotation is a
 * different claim — that these WORDS occur in that section — and it is the claim a reader trusts most and
 * verifies least, because a quoted sentence appears to remove the need to open the spec at all. That is the
 * asymmetry: a wrong number sends a reader to the wrong place, where they find out; a fabricated quotation
 * tells them not to go. CLAUDE.md §Browser half states it and names a measured instance; a second was found
 * with a fabricated sentence attributed to a Web IDL step it had been sitting under, trusted, for as long as
 * anyone had read it. Neither is visible to any check above, because both citations carry a number that
 * RESOLVES, a title that MATCHES, and a term the standard really defines. Only the section's own text can say. */

/* WHICH QUOTATIONS ARE CHECKED: THE ONES A CITATION GOVERNS, because a finding must falsify a CLAIM and an
 * unattributed quotation makes none. This tree quotes constantly — its own prose, CLAUDE.md, a variable's
 * meaning, a page's minified JavaScript — and a checker that reached for all of them would be asking whether
 * arbitrary English appears in a standard, which is not a question about the tree. So a quotation belongs to
 * the nearest citation BEFORE it, and the region a citation governs ends at the next citation or at the end of
 * the prose it sits in, whichever comes first. */

/* A C MESSAGE IS SEVERAL ADJACENT LITERALS AND A QUOTATION CROSSES THEM, which is why the region is a RUN
 * rather than a span. A DFAIL that quotes a sentence puts the opening `\"` in one literal and the closing one
 * three lines down, so a per-literal scan finds an opening quote and no close and reads nothing at all —
 * silently, in exactly the place CLAUDE.md says a wrong citation costs the most, since a crash message is read
 * by someone standing at an abort with no file open. */
function spanIdxAt(spans, at) {
  let lo = 0, hi = spans.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (spans[mid][1] <= at) lo = mid + 1;
    else if (spans[mid][0] > at) hi = mid - 1;
    else return mid;
  }
  return -1;
}

/* The prose a citation governs, flattened into ONE line the way a reader reads it. A comment's line wrap and
 * its `*` gutter are not part of any sentence; a literal run's joints are not either, and the escape a C
 * literal spells `\"` is the quotation mark the author wrote. Unescaping is what makes a quotation inside a
 * crash message the same text as the same quotation inside a comment. */
function governedProse(src, spans, at, len, stopAt) {
  const i = spanIdxAt(spans, at);
  if (i < 0) return "";
  const kind = spans[i][2];
  let end = spans[i][1];
  if (kind === "s") {
    for (let k = i; k + 1 < spans.length && spans[k + 1][2] === "s"; k++) {
      /* adjacent literals of ONE message are separated by nothing but whitespace and their own two quotes */
      if (!/^["\s]*$/.test(src.slice(spans[k][1], spans[k + 1][0]))) break;
      end = spans[k + 1][1];
    }
  }
  if (stopAt !== null && stopAt < end) end = stopAt;
  const raw = src.slice(at + len, Math.max(at + len, end));
  return kind === "s"
    /* the joint between two literals is an unescaped `"` pair; a `\"` is content and must survive it */
    ? unescapeC(raw.replace(/(?<!\\)"\s*"/g, ""))
    : raw.replace(/\n\s*\*?\s*/g, " ");
}

/* EVERY C ESCAPE, NOT JUST THE BACKSLASH-PAIR — because the one this tree writes most in a spec quotation is
 * the one a naive `\\(.)` rule destroys. A standard's prose is full of curly apostrophes, so a message quoting
 * it writes `a transaction\u2019s scope`; dropping the backslash alone leaves the WORD `u2019` wedged into the
 * sentence, and the checker then reports a correctly-pasted quotation as diverging at its second word. That is
 * the normalizer manufacturing the finding, which is the one failure mode a checker must not have. */
const C_ESCAPE = /\\(u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|x[0-9a-fA-F]{1,2}|[0-7]{1,3}|[\s\S])/g;
function unescapeC(s) {
  return s.replace(C_ESCAPE, (m, e) => {
    const c = e[0];
    if (c === "u" || c === "U") return String.fromCodePoint(parseInt(e.slice(1), 16));
    if (c === "x") return String.fromCharCode(parseInt(e.slice(1), 16));
    if (c >= "0" && c <= "7") return String.fromCharCode(parseInt(e, 8));
    return c === "n" || c === "t" || c === "r" || c === "f" || c === "v" ? " " : c;
  });
}

/* A QUOTATION IS A DOUBLE-QUOTED RUN AND NOTHING ELSE. This tree writes code in backticks and a term in single
 * quotes; the double quote is what it uses to say "these are the standard's own words", which is the only
 * spelling that carries the claim this check falsifies.
 * THE PAIRING IS SCANNED AND NOT MATCHED, and the difference is not style — a regex with a MINIMUM LENGTH
 * silently re-pairs every quote after a short one. `serialize as \"{\"` is a one-character quotation, so a
 * pattern demanding two characters skips its opening mark, pairs its CLOSING mark with the next opening one,
 * and hands the checker a fragment that begins mid-sentence and belongs to no quotation at all. Measured: it
 * manufactured findings whose quoted text started with a comma. A scanner has no minimum, so a short
 * quotation is READ and then declined by the word floor, where the report can count it. */
function quotedRuns(prose) {
  const out = [];
  for (let i = 0; i < prose.length; i++) {
    const ch = prose[i];
    /* A QUOTE THE AUTHOR ESCAPED IS NOT A DELIMITER. Both zones write a nested quotation as `\"`: a C message
     * because the literal needs it, a comment because it is quoting C. Reading one as a close pairs the
     * opening mark with a mark INSIDE the quotation and hands the checker a fragment that stops mid-sentence —
     * measured, on a DFAIL quoting `window.open`'s method steps, whose quotation was cut at "throw an". */
    if (prose[i - 1] === "\\") continue;
    if (ch !== '"' && ch !== "\u201c") continue;
    const close = ch === '"' ? '"' : "\u201d";
    let j = prose.indexOf(close, i + 1);
    while (j > 0 && prose[j - 1] === "\\") j = prose.indexOf(close, j + 1);
    if (j < 0) break;
    out.push({ text: prose.slice(i + 1, j), at: i });
    i = j;
  }
  return out;
}

/* AN ELLIPSIS IS A CLAIM ABOUT BOTH HALVES, WHICH IS WHAT MAKES IT CHECKABLE RATHER THAN AN ESCAPE HATCH.
 * `"the disabled attribute … whether the style sheet is applied"` asserts that the first phrase appears, that
 * the second appears, and that the second appears AFTER the first — three facts a section's own words either
 * carry or do not. So the quotation is split on the elision mark and matched fragment by fragment, in order
 * and without overlap; nothing is loosened by the author having elided the middle. */
const ELLIDED = /\s*(?:\[\s*)?(?:…|\.\s*\.\s*\.+)(?:\s*\])?\s*/;

/* THE QUOTATION SIDE KEEPS ITS ANGLE BRACKETS AND THE SPEC SIDE DOES NOT, AND THAT ASYMMETRY IS DELIBERATE.
 * Markup exists only on the spec side, so `tokenText` strips tags there; a comment that writes `<iframe>` or
 * `<var>` in running prose is writing WORDS, and stripping them here would delete a token the standard really
 * has — a false finding manufactured by the normalizer. Everything else is shared: both sides end as a
 * lowercase stream of alphanumeric runs, so curly quotes, the possessive the spec splits across a tag
 * boundary, hyphenation and every difference of punctuation are gone from both at once. */
const quoteTokens = (s, dropMarkers) => {
  const t = decodeEntities(String(s)).replace(/\s+/g, " ");
  return (dropMarkers ? t.replace(STEP_MARKER, "") : t)
    .toLowerCase().replace(/[^a-z0-9]+/g, " ").trim();
};

/* THE THREE FLOORS BELOW ARE MEASURED, NOT CHOSEN, and the measurement is the reason each is where it is. A
 * short phrase is a COLLOCATION rather than a quotation, so a checker resting on one does not report a weaker
 * fact — it reports a coincidence, and in the VERIFY direction that is silent: it certifies a fabrication.
 * WHAT WAS MEASURED, on these committed corpora: 400 random n-word phrases drawn from one standard, counted
 * for occurrence in some OTHER standard. At n=3, 103 of 400 for HTML and 170 for DOM. At n=4, 33 and 84. At
 * n=6, 6 and 32. At n=8, 1 and 10. At n=10, 1 and 4. At n=12, 0 and 0. DOM is the noisiest because its
 * vocabulary is every other standard's; HTML is the largest and so the easiest to hit by chance. The curve is
 * what fixes the numbers:
 *   MIN_FRAGMENT_WORDS = 4 — below this a fragment is not evidence at all (a fifth of DOM's 4-word phrases
 *     occur elsewhere), so a shorter fragment is NOT COMPARED and the report says how many were skipped.
 *   MIN_COMPARED_WORDS = 6 — the floor is on the COMPARED words, not on the quotation's length, because an
 *     ellipsis can leave a long quotation resting on one short fragment. A quotation whose whole evidence is
 *     four words is declined and counted, never verified on 8-to-21% odds.
 *   ALT_FLOOR = 10 — a claim that ANOTHER standard owns these words is asserted only above 10, where the
 *     coincidence rate is at or under 1%. Below it the tool was naming `fetch §4.12.1` for `same origin with
 *     the top`, which is a stock phrase and not a passage. This floor is higher than the other two because it
 *     is the only one that names a document the citation never mentioned. */
const MIN_FRAGMENT_WORDS = 4;
const MIN_COMPARED_WORDS = 6;
const ALT_FLOOR = 10;

function fragmentsOf(quote) {
  const parts = String(quote).split(ELLIDED);
  const all = parts.map((p) => quoteTokens(p, false)).filter(Boolean);
  const cut = parts.map((p) => quoteTokens(p, true)).filter(Boolean);
  const big = all.filter((f) => f.split(" ").length >= MIN_FRAGMENT_WORDS);
  const bigCut = cut.filter((f) => f.split(" ").length >= MIN_FRAGMENT_WORDS);
  const words = all.reduce((n, f) => n + f.split(" ").length, 0);
  const compared = big.reduce((n, f) => n + f.split(" ").length, 0);
  return { all, big, forms: bigCut.join("\u0000") === big.join("\u0000") ? [big] : [big, bigCut],
           words, compared, elided: all.length - big.length };
}

/* IN ORDER AND WITHOUT OVERLAP, ON WHOLE WORDS. The haystack and the needle are both space-separated token
 * streams, so a bare indexOf would match `the disabled` inside `lathe disabled`; padding both with a space
 * makes every comparison a word comparison. */
/* A QUOTATION THAT ENDS ON A NUMBER IS THE ONE SPELLING BOTH-SIDED MARKER STRIPPING GETS WRONG, AND IT WAS 32
 * FALSE FINDINGS. STEP_MARKER's own paragraph argues that stripping a `12. ` marker from BOTH sides keeps a
 * sentence that merely FINISHES on a number (`must be 0. Return true`) comparing equal — and it does, while the
 * quotation carries the following capital that makes the pattern fire. It does not when the author STOPS at the
 * number: `"…then set timeout to 0"` keeps its `0` and the corpus, whose next sentence begins with a capital,
 * has lost one — so a correctly-pasted step diverged on its own last word. `timer.c` carried seven of those,
 * `history.c`'s `initially −1`, `file_system_writable.c`'s `an algorithm that returns 1`, `element.c`'s
 * `greater than 0`: every one right, every one reported.
 * SO THE CORPUS KEEPS EVERY TOKEN THE DOCUMENT HAS and the QUOTATION is offered in BOTH forms — as typed, and
 * with markers dropped. A standard's steps are numbered by a CSS counter, so the document holds no marker for
 * the corpus to lose, and the only side that can carry one is the side that typed it out. Both forms are asked
 * of every probe rather than only of the verification, because a finding that names WHERE the words live must
 * search the same way the confirmation did or it will name the wrong place for exactly this population. */
function containsAnyForm(hay, f) {
  for (const frags of f.forms) if (containsFragments(hay, frags)) return true;
  return false;
}

function containsFragments(hay, frags) {
  const h = " " + hay + " ";
  let pos = 0;
  for (const f of frags) {
    const i = h.indexOf(" " + f + " ", pos);
    if (i < 0) return false;
    pos = i + f.length + 1;
  }
  return true;
}

/* A BARE "NOT FOUND" IS THREE ANSWERS WEARING ONE NAME, AND WHICH ONE IT IS DECIDES WHAT THE READER DOES.
 * A quotation whose FIRST NINE WORDS are the standard's and whose tenth is not has been mis-transcribed: the
 * passage is real, the author dropped or reworded a clause, and the repair is to paste the sentence again. A
 * quotation of which NOT ONE PHRASE occurs anywhere in the standard is the fabrication CLAUDE.md names — a
 * sentence nobody in that working group ever wrote — and the repair is to delete it or to replace the claim.
 * A quotation that is the TREE'S OWN prose in quotation marks is the same shape as the second and is not a
 * defect at all. Reporting the three with one number would be the defect this file exists to find, so the
 * finding carries the longest prefix that IS the standard's: it is the evidence for the claim, it is computed
 * rather than asserted, and it is the discriminator a reader triages on. */
function longestPrefixWords(hay, words) {
  const h = " " + hay + " ";
  let lo = 0, hi = words.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (h.includes(" " + words.slice(0, mid).join(" ") + " ")) lo = mid; else hi = mid - 1;
  }
  return lo;
}

/* WHERE THE QUOTATION LEAVES THE STANDARD: the first fragment that is not there, and how much of it is. */
function divergence(hay, frags) {
  for (let i = 0; i < frags.length; i++) {
    const w = frags[i].split(" ");
    const n = longestPrefixWords(hay, w);
    if (n < w.length) return { frag: i, matched: n, of: w.length, head: w.slice(0, n).join(" "), next: w[n] };
  }
  return null;
}

/* ONE WORDING FOR A QUOTATION FINDING, READ BY BOTH ITS READERS. The full report prints it and --since prints
 * it, and the second reader is the reason this is a function rather than a template inside the report loop: a
 * finding a lane meets in its own delta and a finding it meets in the full audit must be the SAME sentence, or
 * the two channels are two auditors that happen to share a name. It is also what lets a quotation finding be an
 * ordinary member of `findings` — same {kind, msg, text} shape the number checks emit — so the delta needs to
 * know nothing about quotations at all. */
function quoteMsg(q) {
  return `${q.spec} §${q.no}${q.crash ? "  [in a crash message]" : ""}` +
    (q.kind === "QUOTE-WRONG-SECTION"
       ? ` — these words are that standard's, at §${q.at.slice(0, 4).join(", §")}` +
         (q.anc ? " — which CONTAINS the cited number, so this is an arm named for its algorithm rather than a wrong number" : "")
     : q.kind === "QUOTE-WRONG-STANDARD" ? ` — these words are ${q.where.slice(0, 3).join("; ")}`
     : q.div ? ` — diverges from ${q.spec} after ${q.div.matched} of ${q.div.of} words` +
               (q.div.frag ? ` (in elided fragment ${q.div.frag + 1})` : "") +
               (q.div.matched ? `: "${q.div.head}" is there, "${q.div.next}" is not`
                              : `: not even its FIRST WORD follows a boundary there`) +
               (q.alt ? ` — but ${q.alt.matched} of those words ARE ${q.alt.key} §${q.alt.at.slice(0, 3).join(", §")}'s` : "")
     : ` — ${q.words} words that appear in NO indexed standard`) +
    (q.elided ? ` (${q.elided} fragment(s) under ${MIN_FRAGMENT_WORDS} words not compared)` : "");
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
    for (const need of ["sections", "dfns", "ops", "uses", "specUpdated", "fetched"]) {
      if (!ix[need]) throw new Error(`${relative(ROOT, f)} has no "${need}" — it was written by an older reader; re-run: node engine/citegen.mjs --regen ${s.key}`);
    }
    /* A LOOKUP TABLE MUST NOT ANSWER FOR A KEY IT DOES NOT HOLD. JSON.parse hands back objects that inherit
     * Object.prototype, so `dfns["constructor"]` returns a FUNCTION and `dfns["to string"]`-shaped phrases
     * reach members nothing indexed — the table saying yes to a term the standard never defined. It crashed
     * here rather than answering wrongly only by luck (`.includes` is not a function on that value); the fix
     * is that the table has no prototype to inherit an answer from. */
    for (const t of ["sections", "dfns", "ops", "uses"]) Object.setPrototypeOf(ix[t], null);
    idx.set(s.key, ix);
  }
  /* THE QUOTATION CORPUS IS LOADED BY EVERY RUN — see the header's argument for why this stopped being a mode.
   * A standard with no corpus is NOT CHECKED — counted, named, and never mistaken for one that passed. The
   * staleness pair is ASSERTED rather
   * than defaulted past: a corpus fetched at a different edition from the section numbers it is keyed by would
   * answer questions about a document no single fetch ever saw, and CLAUDE.md's rule for a producer that does
   * not produce what a consumer reads is a crash, not a `|| {}`. */
  const txt = new Map(), txtStale = new Map();
  for (const [key, ix] of idx) {
    const f = textFileOf(key);
    if (!existsSync(f)) continue;
    const tx = JSON.parse(readFileSync(f, "utf8"));
    for (const need of ["sections", "specUpdated", "fetched"])
      if (!tx[need]) throw new Error(`${relative(ROOT, f)} has no "${need}" — re-run: node engine/citegen.mjs --regen ${key}`);
    /* THE EDITION IS THE STANDARD'S OWN STATEMENT AND THE FETCH DATE IS OURS, so the agreement that matters is
     * the EDITION. Two fetches on different days of an unmoved standard are the same document, and demanding
     * the same fetch date would refuse a corpus that is exactly right — a check spending its own coverage. A
     * corpus keyed by section numbers from a DIFFERENT edition is the real hazard, and specUpdated is what
     * names it. */
    if (tx.specUpdated !== ix.specUpdated) {
      txtStale.set(key, `the corpus is edition "${tx.specUpdated}" and the section index is "${ix.specUpdated}"`);
      continue;
    }
    Object.setPrototypeOf(tx.sections, null);
    txt.set(key, tx);
  }
  if (!idx.size) {
    console.error(`no committed index in ${relative(ROOT, INDEX_DIR)} — run: node engine/citegen.mjs --regen`);
    process.exit(2);
  }
  /* Longest-first matching needs to know how far a phrase can run before it stops being a term, and the
   * shortest a term can be, because ECMAScript's operation names are one word and nothing else's are. */
  let maxWords = 2, minWords = 2;
  /* The stripped operation names are part of the vocabulary the lookup walks, so they set these bounds too —
   * a one-word bare with minWords still 2 would be a key the loop below can never ask for. */
  let maxTitleWords = 2;
  const titleToNo = new Map();
  for (const [key, ix] of idx) {
    for (const t of [...Object.keys(ix.dfns), ...Object.keys(ix.ops)]) {
      const n = t.split(" ").length;
      if (n > maxWords) maxWords = n;
      if (n < minWords) minWords = n;
    }
    const tt = new Map();
    for (const [no, s] of Object.entries(ix.sections)) {
      const k = normTerm(s.title);
      if (!k) continue;
      const n = k.split(" ").length;
      if (n > maxTitleWords) maxTitleWords = n;
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
    /* AND THE TWO DOCUMENTS THE TREE DEFERS TO. CLAUDE.md states the rule this closes — a `.md` a C file cites
     * by name is CODE, because a claim about this tree travels by reference and nothing mechanical reports
     * that it has gone stale — and this file's own header used to name `.md` as its blind spot. These two are
     * cited by name from C hundreds of times between them, and they carry real spec citations with quoted
     * titles and quoted sentences, so leaving them out is the silent zero rather than a clean bill. */
    for (const doc of ["CLAUDE.md", "SECURITY.md"]) {
      const p = join(ROOT, doc);
      if (existsSync(p)) files.push(p);
    }
  }

  const findings = [];
  const undecided = [];
  const quotes = [];
  const qstat = { seen: 0, checked: 0, verified: 0, okNearby: 0, wrongSection: 0, wrongSectionAncestor: 0, wrongStandard: 0, notFound: 0, notFoundNothing: 0,
                  noCorpus: 0, noSection: 0, tooShort: 0, voted: 0 };
  const noCorpusBy = new Map();
  const gapHist = [];
  /* One standard's whole text, joined once. The divergence probe asks it repeatedly and rebuilding a
   * four-megabyte string per finding would make the mode's cost quadratic in its own findings. */
  const wholes = new Map();
  const wholeOf = (key) => {
    if (!wholes.has(key)) {
      const sx = txt.get(key).sections;
      wholes.set(key, Object.keys(sx).sort(cmpNo).map((n) => sx[n]).join(" "));
    }
    return wholes.get(key);
  };
  const stat = { total: 0, bare: 0, anchored: 0, byTerm: 0, byFile: 0, other: 0, skipped: 0,
                 confirmed: 0, confirmedByUse: 0, confirmedByContainment: 0, confirmedByRun: 0,
                 unverified: 0, multiSpec: 0,
                 foreignTerm: 0, titleRefused: 0, byTitle: 0, numberRefused: 0 };
  const byKey = new Map();
  /* Per standard, how many of its audited citations were placed there by the file vote rather than by their
   * own anchor or their own term — and the sites themselves, so the count has a list behind it. */
  const byKeyVoted = new Map();
  const voted = [];
  const byOther = new Map();
  const untitled = new Map();
  const untitledVoted = new Map();
  const unknownTok = new Map();
  const SEC = "[0-9]+(?:\\.[0-9]+)*|[A-F](?:\\.[0-9]+)+";
  /* A CITATION THE READER CANNOT SEE IS WORSE THAN ONE IT REPORTS WRONG, BECAUSE IT APPEARS IN NO TOTAL. This
   * pattern demanded the number touch the §, and `§ 3.2.26` — a spelling a human reads as identical — was
   * therefore in no count, no confirmed tally and no finding list: nineteen lines of `quickjs.c` hid
   * twenty-four citations that way, and the invisibility was not merely a silence, it MANUFACTURED a finding,
   * since a quotation belongs to the nearest citation BEFORE it and an unseen `§4.2` handed its sentence to the
   * `§3.2.26` above it. The sites were repaired by hand; this is the reader that cannot lose them again. Only
   * horizontal space is admitted: a `§` at the end of a line is a citation wrapped across a comment gutter,
   * whose next line begins with a `*` this scan would have to interpret, and no such site exists in the tree —
   * so admitting a newline here would be widening for a population of zero, which is how a reader starts
   * guessing. `seen` records where the NUMBER starts rather than where the § does, because that is the index
   * the bare-number pass finds and skipping the wrong one would read every spaced citation twice. */
  const CITE = new RegExp("§[ \t]*(" + SEC + ")", "g");
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
  /* AND NEITHER IS A NUMBER THE WORD `step` INTRODUCES — the same defect as the range operand, at SIXTY-ONE
   * PERCENT of this reader's candidates rather than five. CLAUDE.md fixes the convention this rests on: a
   * SECTION is written `§10.2` and a STEP is written `step 10.2`, so a bare number whose lead-in is the word
   * `step` or `steps` is not a section number anybody wrote, and admitting it is the tool inventing the
   * citation it then judges — the sentence the range-operand paragraph above already uses about itself.
   * IT IS NOT MERELY NOISE IN A COUNT, BECAUSE AN INVENTED CITATION GOVERNS THE PROSE AFTER IT. The quotation
   * check attributes a quotation to the nearest citation BEFORE it, so `§5.6 step 5.1's "wait until request is the
   * first item …"` charged Indexed Database §5.1 with §5.6's own sentence and reported a WRONG-SECTION against
   * a citation that is exactly right. Measured at the revision this landed: 2284 of 3759 bare candidates under
   * engine/host sit in this position, and the shape is every stage label and every step-quoting comment in the
   * tree (`HTML §4.10.5.1.14 step 4.2's rounding`, `CSP §6.7.3.3 step 5.2.2`, `console.c`'s `step 4.3`).
   * A `§`-WRITTEN NUMBER AFTER `step` IS STILL READ, and that asymmetry is the point: `step §7.4.5` is an
   * author naming a section, and the exclusion is only ever about a number with no § in front of it — which is
   * precisely the population this reader admits on a guess. */
  const STEP_LEAD = /(?:^|[^\w])steps?\s+$/i;

  /* Look a phrase up in every index at once. Returns the LONGEST phrase any standard knows, the standards
   * that know it, and — per standard — whether the cited number is its definition site or a prominent use. */
  /* A SECTION CONTAINS ITS OWN SUBSECTIONS, so a citation of §7.4 for a term the standard defines at §7.4.1.2
   * is not wrong — it is less precise, and precision is the author's call. Reporting it would be the tool
   * asserting an error it cannot demonstrate, which is exactly the failure it exists to catch. A SIBLING
   * (§7.4.5 for a term defined at §7.4.1.2) is a different matter and stays a finding. */
  const contains = (ancestor, sec) => sec.length > ancestor.length && sec.startsWith(ancestor + ".");

  /* WHAT MAKES AN UNQUOTED PHRASE A TITLE CLAIM RATHER THAN THE START OF A SENTENCE: THE AUTHOR ENDED IT THERE.
   * Quoting a title states its extent explicitly, and that is the whole reason check (4) could trust a quote
   * without further evidence. The unquoted form has to establish the same fact, and the only thing at the site
   * that can is where the phrase STOPS — so the matched title must run to the end of the prose or to a mark
   * that closes a phrase, never into another word.
   * IT WAS MEASURED THE OTHER WAY FIRST AND THE OTHER WAY IS UNUSABLE. Accepting any leading phrase that titles
   * a section raised twelve findings tree-wide of which nine were prose whose FIRST WORDS happen to title
   * something: `constraint-validation clause` (the title is `Constraint validation`, the author wrote an
   * adjective), `THE DOCUMENT'S ELEMENT SHORTCUTS` (`The document element`), `the window object's location
   * getter steps` (`The Window object`), `the navigate event intercept commit handler steps` (`The navigate
   * event`), `data model ----` as a banner, and two quoted SPEC SENTENCES whose opening words are a heading
   * somewhere. Every one names a specific replacement number for a citation that is right, which this file's
   * own comment calls the worst finding it can emit. The delimiter is what all nine lack and what the three
   * survivors have.
   * The scan runs on the RAW prose because normTerm has already turned every delimiter into a space; the words
   * are joined by "any run of non-alphanumerics" so a hyphenated or backticked spelling still matches, which is
   * the same normalization normTerm performs and must not be a second, disagreeing one.
   * WHAT COUNTS AS ENDING THE NAME WAS READ OFF THE TREE, NOT CHOSEN. The population whose recall this gate
   * spends is the CORRECT citations of this shape: the unquoted sites whose leading phrase EXACTLY titles the
   * section they cite, 1743 of them at the revision this was measured. What follows them, most often: a
   * possessive `'s` (`§4.8.5 The iframe element's …` — the name is complete and the sentence then talks about
   * the thing), then `step`/`steps` (`§7.4.5 Populating a session history entry step 3`), a comma, a string
   * literal's `\` continuation or its closing `"`, an em dash, a period, a comment close, an open paren. Each
   * is a way of ENDING A NAME, and admitting exactly those admits 873 of the 1743 — where punctuation alone
   * admits 251. What is refused is the shape all nine false positives had: ANOTHER WORD, which continues the
   * noun phrase and means the title was only its beginning. A colon is refused too, on the same evidence — it
   * delimits 50 correct sites and it is also what stands inside the quoted IDL `interface Document : Node`,
   * which produced two of the nine.
   * RESIDUAL — AN UNQUOTED TITLE RUNNING STRAIGHT INTO A VERB IS NOT JUDGED, and it is half of this shape:
   * 870 of the 1743 end in a word rather than a mark, so `§7.3.1 Creating browsing contexts is what this
   * claims` is declined while `§7.3.1 Creating browsing contexts, …` is caught. The code is right about what
   * it does and narrower than the promise. WHAT THE NEXT DIFF BUILDS: the thing the delimiter stands in for —
   * whether the words AFTER the title continue the noun phrase — asked of the index rather than of grammar, by
   * testing whether the LONGER phrase is itself something some standard names (a term, an operation, a title),
   * and declining only then; `the document element` + `shortcuts` and `the navigate event` + `intercept commit
   * handler steps` are exactly the cases a longer-phrase test can see and a delimiter cannot. HOW ITS ABSENCE
   * SHOWS: a citation with a wrong number and a right title followed by a verb is reported as carrying "no
   * title and no term any index knows" — filed as undecided, indistinguishable in the report from a citation
   * that states nothing at all, which is the very silence this check was widened to end. */
  const RE_ESC = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const ENDS_A_NAME = /^\s*(?:$|\*\/|['’]s\b|steps?\b|[.,;)\]("”»—–\\])/;
  const delimited = (raw, words) => {
    /* CASE-INSENSITIVE, because normTerm lowercased the words and the tree writes a banner in capitals — the
     * first spelling of this had no flag and matched NOTHING, which reads exactly like a clean result. */
    const m = new RegExp("^[^A-Za-z0-9]*" + words.map(RE_ESC).join("[^A-Za-z0-9]+"), "i").exec(raw);
    return !!m && ENDS_A_NAME.test(raw.slice(m[0].length));
  };

  /* THE TITLE A CITATION STATES, READ OUT OF ONE STANDARD'S HEADINGS — the longest leading phrase (or the
   * whole quoted phrase) that titles a section of `tt`. It is ONE function because it is asked TWICE about
   * two different questions — WHICH STANDARD, at the resolution below, and WHICH SECTION, at TITLE-MISMATCH —
   * and a second copy would be a second list of what counts as a stated title, drifting from the one that
   * decides findings. The two floors it carries are the file's own and are argued at TITLE-MISMATCH: a
   * one-word title is refused unless the author QUOTED it, and the longest leading phrase wins so a short
   * title nested inside a longer one cannot fire ahead of it. */
  const claimIn = (tt, c) => {
    if (c.quoted) { const q = normTerm(c.quoted); return tt.has(q) ? q : null; }
    for (let n = Math.min(maxTitleWords, c.words.length); n >= 2; n--) {
      const p = c.words.slice(0, n).join(" ");
      if (tt.has(p) && delimited(c.after, c.words.slice(0, n))) return p;
    }
    return null;
  };

  /* `opOK` says the phrase's RAW spelling at the citation is identifier-shaped, which is the one thing that
   * makes a one-word declared operation name safe to look up — see addOp. A dfn always wins over a stripped
   * name: the standard's own spelling of a term is better evidence than a signature with its arguments cut. */
  const probe = (phrase, no, only, opOK) => {
    const hits = [];
    for (const [key, ix] of idx) {
      if (only && key !== only) continue;
      let where = ix.dfns[phrase] || null;
      if (!where && (opOK || phrase.includes(" "))) where = ix.ops[phrase] || null;
      if (!where) continue;
      const u = ix.uses[phrase] || null;
      const n2 = u && Object.hasOwn(u, no) ? u[no] : 0;
      hits.push({ key, where,
        defAt: where.includes(no),
        underAt: where.some((d) => contains(no, d)),
        useAt: n2 >= USE_FLOOR, mentions: n2 });
    }
    return hits.length ? { phrase, hits } : null;
  };
  const lookup = (words, no, only, opHead) => {
    for (let n = Math.min(maxWords, words.length); n >= minWords; n--) {
      const r = probe(words.slice(0, n).join(" "), no, only, n === 1 && opHead);
      if (r) return r;
    }
    return null;
  };

  for (const file of files) {
    const src = opts.srcOf ? opts.srcOf(file) : readFileSync(file, "utf8");
    const lineOf = lineIndex(src);
    const spans = proseSpans(src, file);

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
      seen.add(m.index + m[0].length - m[1].length);
      if (a) votes.set(a, (votes.get(a) || 0) + 1);
      else if (tok && /^[A-Z]/.test(tok) && tok.length > 2) unknownTok.set(tok, (unknownTok.get(tok) || 0) + 1);
    }
    BARE.lastIndex = 0;
    for (let m; (m = BARE.exec(src)); ) {
      if (seen.has(m.index) || !inSpans(spans, m.index)) continue;
      if (RANGE_OPERAND.test(src.slice(Math.max(0, m.index - 24), m.index))) continue;
      if (STEP_LEAD.test(src.slice(Math.max(0, m.index - 24), m.index))) continue;
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
    /* A RUN OF ADJACENT NUMBERS IS ONE CITATION WITH SEVERAL TARGETS, AND ONLY ITS LAST MEMBER CARRIES THE
     * PROSE — which is exactly the shape that manufactures a finding against a citation that is right. A
     * distributive citation names the sections and the things in parallel: `§2.3/§2.4's create a new
     * FileSystemFileHandle / FileSystemDirectoryHandle`, `§9.1, §9.2 and §4.7's …`. The term scan reads the
     * WHOLE trailing phrase from each number, so the earlier members see prose beginning with a NUMBER and
     * decide nothing, while the LAST member is charged with the FIRST thing in the list — which the FIRST
     * number defines. Measured on the diff that indexed the File System Standard: four of its five findings
     * were one `§2.3/§2.4` pair written four times, and every one of them was correct as written.
     * THE RUN IS READ BACKWARD ONLY, because that asymmetry is a fact about the resolver rather than a
     * shortcut: a `§` is not a word character, so `§2.3/§2.4's create a new …` leaves the §2.3 site's prose
     * starting `2.4 create a new …`, which no lookup can match. Nothing forward needs collecting.
     * WHAT SEPARATES A RUN FROM TWO SENTENCES IS THE SEPARATOR AND NOTHING ELSE. A list separator, a
     * conjunction or a dash joins two numbers into one citation; a period, a word, or anything longer than a
     * few characters ends the first citation, and `§4.7's X. §4.8's Y` must stay two claims. */
    const RUN_JOIN = /^[ \t]*(?:[/,&+]|and|or|through|to|[-‐-―])?[ \t]*$/;
    for (let i = 0; i < cites.length; i++) {
      const c = cites[i];
      if (c.bare) continue;                 /* a bare number's admission is PASS 3's question, not this one */
      const run = [c.no];
      for (let j = i - 1; j >= 0; j--) {
        const prev = cites[j], next = cites[j + 1];
        if (prev.bare) break;
        const gapAt = prev.at + prev.len;
        if (next.at - gapAt > 8 || !RUN_JOIN.test(src.slice(gapAt, next.at))) break;
        run.push(prev.no);
      }
      if (run.length > 1) c.run = run;
    }
    for (const c of cites) {
      const after = src.slice(c.at + c.len, c.at + c.len + 220).replace(/\n\s*\*?\s*/g, " ");
      /* A QUOTE INTRODUCED BY A COLON IS STILL A QUOTE, and missing that made the tool judge a quoted SPEC
       * SENTENCE as if it were bare prose. `location.c` writes `§7.2.4: "The Window object's location getter
       * steps are to return this's Location object."` — the author quoted the standard verbatim, and with the
       * colon unrecognized the site fell to the running-prose path, whose leading three words happen to title
       * HTML §7.2.2 "The Window object". On the quoted path the WHOLE quotation is compared, matches no title,
       * and nothing is asserted — which is the correct answer and the one the quoting was meant to produce. */
      c.quoted = (/^['"’“]?s?['"’“]?\s*:?\s*["“]([^"”]{2,90})["”]/.exec(after) || [])[1] || null;
      c.words = normTerm(after.replace(/^'s\b/, " ")).split(" ").filter(Boolean);
      c.after = after;                       /* the raw prose, for the delimiter test in check (4) */
      /* THE FIRST TOKEN AS THE AUTHOR SPELLED IT, kept because normTerm has already destroyed the one signal
       * that separates an operation name from an English word — see addOp. It is accepted only where it
       * normalizes to exactly the first word the lookup will ask for, so a token this scan reads differently
       * from normTerm can never license a lookup normTerm did not produce. */
      const rawHead = (/^\s*(?:['’]s\b)?[\s"“'’(:,—–-]*(\[\[[A-Za-z]+\]\]|%[A-Za-z.]+%|[A-Za-z][A-Za-z0-9_$]*(?:\.[A-Za-z0-9_$]+)*)/
        .exec(after) || [])[1] || "";
      c.opHead = !!rawHead && c.words.length > 0 && nameIsIdentifier(rawHead) && normTerm(rawHead) === c.words[0];
      const only = c.anchor && !c.anchor.startsWith("other:") ? c.anchor : null;
      if (c.anchor && c.anchor.startsWith("other:")) { c.foreign = true; continue; }
      /* A QUOTE IS THE AUTHOR'S OWN STATEMENT OF WHAT THE CITATION IS ABOUT, so it is matched WHOLE and the
       * running prose is not consulted at all. Prefix-matching inside a quote reads a term out of a sentence
       * that merely contains one: `DOM §3.2 "fire an event named abort at signal"` cites Interface AbortSignal
       * correctly and quotes one of its STEPS, and a prefix match turned that into a misattribution of
       * §2.10 "Firing events". If the quoted phrase is not a term and not a title, there is nothing here to
       * check — which is UNDECIDED, not a finding. */
      c.ev = c.quoted
        ? probe(normTerm(c.quoted), c.no, only, nameIsIdentifier(c.quoted.trim()))
        : lookup(c.words, c.no, only, c.opHead);
      /* THE STANDARDS WHOSE HEADINGS THE CITATION'S OWN STATED TITLE NAMES, collected across EVERY index for
       * the same reason check (2) asks every candidate: a title is evidence about which STANDARD as much as
       * about which section, and asking only the one the resolver picked makes the answer depend on the guess
       * it is meant to replace. Until this existed the resolver could not act on that sentence even though
       * check (2) states it outright, so a citation whose ONLY evidence was the title CLAUDE.md asks authors
       * to write had no evidence at all as far as the resolver was concerned. */
      c.titleEv = [];
      for (const [k, tt] of titleToNo) { const q = claimIn(tt, c); if (q) c.titleEv.push({ key: k, claim: q }); }
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
        stat.total++; stat.other++; c.admitted = true;
        const k = c.anchor.slice(6);
        byOther.set(k, (byOther.get(k) || 0) + 1);
        continue;
      }
      const g = group.get(c.no);
      if (c.bare && !g.evidence) continue;      /* a number, not a citation */
      stat.total++;
      /* A CITATION ENDS THE REGION THE ONE BEFORE IT GOVERNS, whatever standard it turns out to name — so the
       * mark is set here, where a candidate becomes a citation, and not where it resolves. */
      c.admitted = true;
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
      /* A STATED TITLE RESOLVES THE STANDARD, AND IT SITS ABOVE THE FILE VOTE BECAUSE IT IS EVIDENCE WHERE THE
       * VOTE IS A GUESS. This does not REPLACE a resolution in the sense the paragraph below refuses — every
       * rule above it still wins — it replaces the GUESS, which is the one thing the header says may resolve
       * and may not judge. That distinction is the whole reason the number-does-not-exist check can be asked
       * here at all: `§3.2.4.6 unsigned long` names a Web IDL heading and nothing else in this tree's indexes,
       * so the standard is the citation's own evidence and a wrong number under it is demonstrable rather than
       * inferred.
       * IT MUST NAME EXACTLY ONE STANDARD. `Abstract operations` titles a section of nearly every bikeshed
       * spec and `Introduction` titles one of all of them; a title naming several is a coincidence generator
       * rather than evidence, which is the same floor keepTerm applies to a one-word term and the same one the
       * unquoted title check already applies here.
       * AND THE STANDARD MUST PLACE THAT TITLE NEAR THE CITED NUMBER — the same section, an ancestor, a
       * descendant, or a SIBLING under the same parent. THIS IS THE HALF THAT WAS MEASURED AND IS NOT
       * CAUTION: "only one INDEXED standard uses this title" is not "only one standard uses this title", and
       * the gap between those is every standard this audit does not hold — 1605 citations' worth. Without the
       * corroboration eight findings appeared and the ones read were all the same mis-resolution: `§10.13
       * "Serialization"` is CSS Typed OM, which nothing here indexes, and `serialization` titles css-images-3
       * §7, so the tool named a standard the comment never meant and then reported its number missing;
       * `POINTER LOCK 2.0 §6 "Extensions to the MouseEvent Interface"` went to CSSOM View §10 the same way.
       * A title sitting in the cited number's own neighbourhood cannot be that: a standard that numbers this
       * heading at §3.2.4.6 is a standard whose §3.2.4.x IS this subject, so a §3.2.4.12 written beside it is
       * demonstrably that standard with the NUMBER wrong. A title sitting nowhere near is the tell that the
       * STANDARD is wrong, which check (3)'s own `else` already refuses to judge past for the same reason.
       * What that refusal costs is disclosed rather than swallowed — see the number-exists counter below. */
      if (!spec && c.titleEv.length === 1 && idx.has(c.titleEv[0].key)) {
        const at = titleToNo.get(c.titleEv[0].key).get(c.titleEv[0].claim);
        const parent = (s) => (s.includes(".") ? s.slice(0, s.lastIndexOf(".")) : null);
        const near = (t) => t === c.no || contains(t, c.no) || contains(c.no, t) ||
                            (parent(t) !== null && parent(t) === parent(c.no));
        if (at.some(near)) { spec = c.titleEv[0].key; how = "title"; }
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
      c.spec = spec; c.how = how;
      stat[how === "anchored" ? "anchored" : how === "term" ? "byTerm"
           : how === "title" ? "byTitle" : "byFile"]++;
      byKey.set(spec, (byKey.get(spec) || 0) + 1);
      /* A GUESSED RESOLUTION IS COUNTED APART FROM A MATCHED ONE EVERYWHERE IT IS COUNTED AT ALL. Pooling them
       * makes "audited by standard: html=N" a number in which inference and evidence are indistinguishable —
       * which is the shape CLAUDE.md calls a plausible datum, performed on the audit's own census. */
      if (how === "file") {
        byKeyVoted.set(spec, (byKeyVoted.get(spec) || 0) + 1);
        voted.push({ file: relative(ROOT, file), line: lineOf(c.at), no: c.no, spec,
                     has: !!idx.get(spec).sections[c.no],
                     crash: inCrashMessage(src, spans, c.at),
                     text: src.slice(c.at, c.at + 110).split("\n")[0] });
      }

      const ix = idx.get(spec);
      const sections = ix.sections, no = c.no;
      let verdict = null;

      /* (1) The number the standard does not have — ASKED ONLY WHERE THE STANDARD IS THE CITATION'S OWN
       * EVIDENCE, which is an explicit anchor or a stated TITLE, and the asymmetry is the point rather than
       * caution. This check fires on ABSENCE, which is exactly what a
       * mis-resolved citation produces. */
      /* AND THE `else` IS A SOUNDNESS FILTER, NOT AN ACCIDENT OF NESTING — this was tried the other way and
       * measured. Ungating the term check on "does the resolved standard have this number" added 252 findings
       * and the sample was noise, every one of it the same shape: the number does not belong to the standard
       * the resolver picked, so the site is a MIS-RESOLUTION rather than a misattribution. `range.c`'s
       * `steps 16.1 and 17.1` is a step span whose right operand the bare-number reader admits, `text_stream.c`
       * cites Encoding §7.1 which nothing here indexes, `element.h` cites HTML §4.6.3 in a URL-flavoured
       * sentence. A number the resolved standard does not have is the tell that the resolution is wrong, and
       * judging past it is the tool asserting an error it cannot demonstrate. */
      /* A TERM-RESOLVED CITATION IS STILL NOT ASKED, AND THAT IS THE PARAGRAPH ABOVE RATHER THAN AN OVERSIGHT:
       * term evidence names a standard that DEFINES a phrase the comment uses, which is what prose in a spec
       * ecosystem looks like, so a number that standard lacks is the tell that the RESOLUTION is wrong. A
       * stated title is not that — it names a standard by quoting its own heading, so the standard is settled
       * and the number is the only thing left that can be wrong. THE COST OF THE OLD GATE WAS A SILENT ZERO:
       * a citation naming no standard and stating a title stood on ANY number, existent or not, and every
       * check that a guess disqualifies declined to look — so the site read exactly like a clean one. It is
       * counted now wherever it is still refused, because a refusal nobody can see is the same zero. */
      if (!sections[no]) {
        if (c.anchor || how === "title") verdict = { kind: "UNKNOWN-SECTION", msg: `${spec} has no §${no}` };
        else if (how === "file") stat.numberRefused++;
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
        /* THE TITLE CONSUMES THE WHOLE SITE, AND THE ONE REFINEMENT THAT LOOKS OBVIOUS HERE WAS BUILT, MEASURED
         * AND REFUSED — recorded because it will look obvious again. A citation states TWO claims when it names
         * an algorithm after the title (`§3 Tools for Specification Authors' coarsen time`): that the section
         * is titled that, and that the algorithm lives there. The title confirms the first and the second is
         * never asked, so a misattribution is invisible EXACTLY WHERE THE AUTHOR DID WHAT CLAUDE.md ASKS and
         * wrote the title down. Resuming the term scan on the words the title did NOT consume looks like the
         * cure and is not: it raised 74 findings tree-wide and the read sample was the header's own
         * prefix-matching defect at a new position — `§7.4.9 IteratorStep ( iteratorRecord )` followed by prose
         * that names IteratorClose, IteratorStepValue or GetIterator, every one of them an operation the cited
         * clause CALLS rather than one it should have cited. ECMAScript is where it is worst, because an
         * operation name is a one-word term and its clauses are written as sequences of other operations.
         * AND IT DID NOT CATCH ITS OWN MOTIVATING CASE, which is what settles it: hr-time §3 LINKS `coarsen
         * time` three times, so USE_FLOOR confirms that site anyway, exactly as the "defined in one section and
         * used in another" rule says it should. A channel that cannot see the case it was built for is a
         * different mechanism, not a narrower one — the same bar this file's header sets for the cross-standard
         * confirmation it also refused. What a future attempt needs is a way to tell a section's SUBJECT from
         * the operations it merely invokes; a positional scan is not that. */
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
          /* AND THE OTHER MEMBERS OF THIS CITATION'S OWN RUN COUNT AS CITED, because the author wrote them.
           * The claim a MISATTRIBUTED makes — "you cited §N and the thing is numbered somewhere else" — is
           * simply FALSE when "somewhere else" is a number standing three characters to the left under the
           * same `'s`. This is the same asymmetry as the paragraph below: a confirmation may quantify over
           * anything the citation actually says, and a finding may only assert what it can prove. */
          const inRun = c.run
            ? c.ev.hits.find((h) => h.where.some((d) => c.run.some((r) => d === r || contains(r, d))))
            : null;
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
          else if (inRun) verdict = { kind: "OK-RUN" };
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

        /* (4) A phrase that titles a DIFFERENT section of the same standard — the renumbering tell.
         * ASKED ONLY WHERE THE STANDARD IS THE CITATION'S OWN EVIDENCE, for the reason check (1) is: both
         * sides of the sentence this raises — "X titles <spec> §A; §B is Y" — are statements about the
         * RESOLVED standard, so on a file-voted resolution both are statements about a document the citation
         * never named. A false one of these is the worst finding this tool can emit: it reads as the
         * renumbering tell CLAUDE.md asks authors to write titles to catch, it names a specific replacement
         * number, and obeying it edits a CORRECT citation into a wrong one. The refusals are counted rather
         * than dropped — a check that silently declines to run is the silent zero again.
         *
         * AND THE PHRASE IS READ UNQUOTED AS WELL AS QUOTED, WHICH IS THE HOLE THIS CHECK HAD AND THE ONE THAT
         * MATTERED MOST, BECAUSE IT WAS EXACTLY THE MANDATED SPELLING THAT WENT UNJUDGED. CLAUDE.md §Browser
         * half writes its own worked example WITHOUT quotes — `HTML §13.2.5.43 comment start state` — and
         * promises, in capitals, that "a mismatch between the two is then visible instead of silent". This
         * check was gated on `c.quoted`, so it never asked. PROBED, not assumed: `HTML §7.3.1 "Creating
         * browsing contexts"` raised TITLE-MISMATCH and the identical citation without the quotes was reported
         * as carrying "no title and no term any index knows" — undecided, unjudged, indistinguishable in the
         * report from a citation that states nothing at all. Worse, check (2) above ALREADY reads the unquoted
         * form to CONFIRM a title, so the confirmation channel was generous exactly where the falsification
         * channel was narrow: the safe-looking construction was the unchecked one, and every author who wrote a
         * title on the strength of that promise got silence for it.
         *
         * IT STAYS ONE CATEGORY AND DOES NOT EARN A NEW NAME. Three states have to stay apart — a number the
         * standard does not have (UNKNOWN-SECTION), a title belonging to a different number (this), and a
         * citation the tool cannot judge (undecided) — and a quoted and an unquoted title are not a fourth
         * state: they are one claim in two spellings, both stating that the section is titled that. Splitting
         * them would make the CATEGORY report on PUNCTUATION rather than on the defect, and a reader triaging
         * "TITLE-MISMATCH-UNQUOTED" would have to learn that it means the same thing before acting on it.
         * The message names the phrase it read either way, so the site is checkable from the report alone.
         *
         * TWO THINGS KEEP THE UNQUOTED FORM FROM CRYING WOLF, and both are the file's own existing rules rather
         * than new thresholds. (a) A ONE-WORD TITLE IS REFUSED — `Introduction`, `Scope`, `Navigables`,
         * `Terminology` title a section of nearly every standard and appear in running prose constantly; that
         * is exactly the coincidence generator `keepTerm` refuses a one-word TERM for, and the same floor
         * applies for the same reason. A quoted one-word title is still read, because quoting it IS the
         * author's statement that it is a title. (b) THE LONGEST LEADING PHRASE WINS, so a short title nested
         * inside the prose of a longer one cannot fire ahead of it — the same longest-match rule `lookup` uses,
         * and the same reason: a prefix match inside a longer phrase reads a claim out of a sentence that
         * merely contains one. */
        /* THE REFUSAL IS COUNTED WHERE IT SUPPRESSES A FINDING, NOT WHERE IT SUPPRESSES A QUESTION, and that is
         * a change from the counter this replaces. It used to tick for every file-voted site carrying a quote,
         * whether or not the quote titled anything — a number that mixes "declined to look" with "declined to
         * report" and reads as the second. The claim is computed first and the vote is asked afterwards, so the
         * count is exactly the findings a guessed standard cost. */
        if (!verdict) {
          const tt = titleToNo.get(spec);
          let claim = claimIn(tt, c);
          /* A SECTION CONTAINS ITS OWN SUBSECTIONS, so citing §4.9.5 in prose that names §4.9's title is less
           * precise and not wrong — the identical rule check (3) applies to a term, applied to a title for the
           * identical reason. `readable_byte_stream.c` cites Streams §4.9.5 "Byte stream controllers" and says
           * `abstract operations`, which titles §4.9 above it. */
          if (claim && tt.get(claim).some((t) => contains(t, no) || contains(no, t))) claim = null;
          if (claim && how === "file") stat.titleRefused++;
          else if (claim) {
            verdict = { kind: "TITLE-MISMATCH",
              msg: `"${c.quoted || claim}" titles ${spec} §${tt.get(claim).join(", §")}; §${no} is "${sections[no].title}"` };
          }
        }
      }

      const rec = { file: relative(ROOT, file), line: lineOf(c.at), no, spec, how, bare: c.bare,
                    text: src.slice(c.at, c.at + 100).split("\n")[0] };
      if (!verdict) {
        stat.unverified++;
        const uk = `${spec} §${no}`;
        untitled.set(uk, (untitled.get(uk) || 0) + 1);
        /* THE KEY'S LEFT HALF CAN BE A GUESS, and --titles reads that key as an instruction: "a title here
         * makes the citation checkable". Under a file-voted standard the instruction is half wrong — what is
         * missing first is the STANDARD'S NAME, and a title written under the inferred one would confirm the
         * inference instead of testing it. So the row says how much of it is inference. */
        if (how === "file") untitledVoted.set(uk, (untitledVoted.get(uk) || 0) + 1);
        rec.groupNo = c.no;
        undecided.push(rec);
        continue;
      }
      const COUNTED_OK = { "OK-USE": "confirmedByUse", "OK-CONTAINS": "confirmedByContainment", "OK-RUN": "confirmedByRun" };
      if (COUNTED_OK[verdict.kind]) { stat.confirmed++; stat[COUNTED_OK[verdict.kind]]++; continue; }
      if (verdict.kind.startsWith("OK")) { stat.confirmed++; continue; }
      findings.push({ ...rec, ...verdict });
    }

    /* PASS 4 — THE QUOTATIONS EACH CITATION GOVERNS. It runs after resolution because it USES the resolution:
     * this check does not decide which standard a comment is about, it asks whether the words the comment
     * attributes to a section are that section's words. Deriving the standard again here would be a second
     * copy of the most argued-over rule in this file, and the copy that drifts is the one nobody runs. */
    {
      const admitted = cites.filter((c) => c.admitted);
      /* WHICH CITATIONS SHARE A PROSE UNIT WITH THIS ONE — see the OK-NEARBY channel below. */
      const bySpan = new Map();
      for (const c of admitted) {
        const k = spanIdxAt(spans, c.at);
        if (k < 0) continue;
        if (!bySpan.has(k)) bySpan.set(k, []);
        bySpan.get(k).push(c);
      }
      for (let i = 0; i < admitted.length; i++) {
        const c = admitted[i];
        const stop = i + 1 < admitted.length ? admitted[i + 1].at : null;
        const prose = governedProse(src, spans, c.at, c.len, stop);
        for (const q of quotedRuns(prose)) {
          const f = fragmentsOf(q.text);
          if (!f.all.length) continue;
          if (f.compared < MIN_COMPARED_WORDS) { qstat.tooShort++; continue; }
          qstat.seen++;
          const rec = { file: relative(ROOT, file), line: lineOf(c.at), no: c.no, spec: c.spec,
                        how: c.how, quote: q.text.trim(), words: f.words, elided: f.elided, gap: q.at,
                        crash: inCrashMessage(src, spans, c.at) };
          /* THE FIVE STATES ARE KEPT APART, because CLAUDE.md's recurring defect is several states behind one
           * answer and a search cannot be directed toward a gap it cannot see. Each refusal below is a
           * DIFFERENT fact about why this quotation was not judged, and each is counted under its own name. */
          if (!c.spec) { qstat.voted++; continue; }              /* foreign or unresolved — no claim to check */
          /* THE FILE VOTE MAY RESOLVE AND MAY NOT JUDGE — this file's own division of labour, applied. A
           * quotation finding is a statement about a STANDARD, so a citation whose standard is an inference
           * from its neighbours has nothing here that can be demonstrated. */
          if (c.how === "file") { qstat.voted++; continue; }
          if (!txt.has(c.spec)) { qstat.noCorpus++; noCorpusBy.set(c.spec, (noCorpusBy.get(c.spec) || 0) + 1); continue; }
          const tx = txt.get(c.spec).sections;
          /* A SECTION CONTAINS ITS SUBSECTIONS — the same rule check (3) applies to a term, applied to text,
           * and it is the join that keeps the corpus free of duplication. The slices are contiguous in the
           * document, so numeric order reproduces the original stream. */
          const own = Object.keys(tx).filter((n) => n === c.no || contains(c.no, n)).sort(cmpNo);
          if (!own.length) { qstat.noSection++; continue; }
          qstat.checked++;
          if (containsAnyForm(own.map((n) => tx[n]).join(" "), f)) { qstat.verified++; gapHist.push([q.at, 1]); continue; }
          gapHist.push([q.at, 0]);
          /* NOT IN THE CITED SECTION — and the next two questions are what separate a WRONG NUMBER from a
           * WRONG STANDARD from a sentence nobody wrote. Naming where the words DO live is the difference
           * between a finding a reader can act on and one they must re-derive. */
          /* A NUMBER THE AUTHOR WROTE IN THIS SAME COMMENT IS A NUMBER THE AUTHOR CITED, and this file already
           * says so about a citation's own run: a finding that claims "the thing is numbered somewhere else"
           * is simply FALSE when somewhere else is a section standing four lines up under the same `/*`. The
           * quotation-to-citation rule is NEAREST-PRECEDING, which is right for the common shape and wrong
           * whenever a second citation intervenes between the subject and its quotation — measured, on a
           * DFAIL that opens `§8.4.1's THREE-ARGUMENT open`, cites `Web IDL §3.6` for the overload rule, and
           * then quotes the ENTRY'S method steps, which are §8.4.1's. Nearest-preceding charges Web IDL with
           * a sentence the same message correctly attributes elsewhere. So a quotation confirmed by ANY
           * citation in its own prose unit is CONFIRMED — counted apart from a direct verification, because a
           * confirmation may quantify over everything the comment says while a finding may only assert what
           * it can prove. */
          const near = (bySpan.get(spanIdxAt(spans, c.at)) || []).find((o) => {
            if (o === c || !o.spec || o.how === "file" || !txt.has(o.spec)) return false;
            const ox = txt.get(o.spec).sections;
            const ext = Object.keys(ox).filter((n) => n === o.no || contains(o.no, n)).sort(cmpNo);
            return ext.length && containsAnyForm(ext.map((n) => ox[n]).join(" "), f);
          });
          if (near) { qstat.okNearby++; continue; }
          const elsewhere = Object.keys(tx).filter((n) => containsAnyForm(tx[n], f)).sort(cmpNo);
          if (elsewhere.length) {
            qstat.wrongSection++;
            /* AN ANCESTOR IS A DIFFERENT AND WEAKER CLAIM THAN A STRANGER, AND SAYING WHICH IS WHAT KEEPS THIS
             * CHANNEL FROM CRYING WOLF. `§2.2.6` for a sentence that lives at `§2.2.3` is a wrong number. But
             * a standard writes an algorithm in a section's PREAMBLE and then a subsection per case, so
             * `§6.4.8's serialization` for a sentence in §6.4 is an author naming the arm rather than the
             * algorithm — still a mismatch at the granularity this check asks about, and not the same defect.
             * Reporting both under one sentence is the collapse this file is written against, so the relation
             * is named and counted apart. */
            const anc = elsewhere.every((n) => contains(n, c.no));
            if (anc) qstat.wrongSectionAncestor++;
            quotes.push({ ...rec, kind: "QUOTE-WRONG-SECTION", at: elsewhere, anc });
            continue;
          }
          const foreign = [];
          for (const [k, t2] of txt) {
            if (k === c.spec) continue;
            const hit = Object.keys(t2.sections).filter((n) => containsAnyForm(t2.sections[n], f)).sort(cmpNo);
            if (hit.length) foreign.push(`${k} §${hit.slice(0, 3).join(", §")}`);
          }
          if (foreign.length) { qstat.wrongStandard++; quotes.push({ ...rec, kind: "QUOTE-WRONG-STANDARD", where: foreign }); continue; }
          qstat.notFound++;
          /* THE WHOLE STANDARD IS THE HAYSTACK FOR THE DIVERGENCE, not the cited section, because the question
           * this evidence answers is "are these the standard's words at all" — and a section boundary is
           * exactly what the reader is being told is NOT the problem. */
          /* THE FORM THAT GOT FURTHEST IS THE EVIDENCE, because a divergence is a claim about how much of the
           * quotation IS the standard's, and the other form's shorter prefix would understate it. */
          let d = null;
          for (const frags of f.forms) {
            const cand = divergence(wholeOf(c.spec), frags);
            if (!cand) { d = null; break; }
            if (!d || cand.matched > d.matched) d = cand;
          }
          if (d && d.matched < MIN_FRAGMENT_WORDS) qstat.notFoundNothing++;
          /* A ONE-WORD DRIFT DEFEATS AN EXACT SEARCH AND THEREBY HIDES A WRONG STANDARD, so where the
           * quotation is plausibly a real sentence the PREFIX is asked of every corpus, not only the cited
           * one. Measured, and it is why this exists: a DFAIL quoted `window.open`'s method steps and
           * attributed them to Web IDL §3.6; the standard's own wording carries a `then` the quotation drops,
           * so every exact channel came back empty and the finding said only "diverges after 4 words" — while
           * TWENTY of those twenty-four words are HTML's, one section away from where the same comment
           * correctly names HTML further down its own sentence. Naming the better match turns a vague finding
           * into the one a reader can act on, and it costs one probe per corpus over the population that has
           * already failed every exact test. */
          let alt = null;
          if (d && d.matched >= MIN_FRAGMENT_WORDS) {
            const w0 = f.big[0].split(" ");
            for (const [k] of txt) {
              if (k === c.spec) continue;
              const w = wholeOf(k);
              if (!(" " + w + " ").includes(" " + w0.slice(0, MIN_FRAGMENT_WORDS).join(" ") + " ")) continue;
              const n = longestPrefixWords(w, w0);
              if (n >= ALT_FLOOR && n > d.matched && (!alt || n > alt.matched)) alt = { key: k, matched: n };
            }
            if (alt) {
              const head = w0.slice(0, alt.matched).join(" ");
              alt.at = Object.keys(txt.get(alt.key).sections)
                .filter((n) => (" " + txt.get(alt.key).sections[n] + " ").includes(" " + head + " ")).sort(cmpNo);
            }
          }
          quotes.push({ ...rec, kind: "QUOTE-NOT-FOUND", div: d, alt });
        }
      }
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
  /* A QUOTATION FINDING IS A FINDING, SO --since MUST SEE IT — and this line is what makes "run it on what you
   * write" reach the axis a lane is most likely to get wrong. A fabricated sentence is written by the person
   * writing the comment, in the diff they are landing, which is exactly the population --since reports; leaving
   * quotations out of the returned set would have left the routine channel blind to the one check CLAUDE.md
   * calls the one a reader trusts most and verifies least. `qtext` joins the key because two different
   * quotations under one citation can diverge identically, and a delta that could not tell them apart would
   * report a repaired one as still standing. */
  const quoteFindings = quotes.map((q) => ({ ...q, msg: quoteMsg(q), text: `"${q.quote}"`, qtext: q.quote }));
  if (opts.quiet) return [...findings, ...quoteFindings];
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
  console.log(`  resolved: ${stat.anchored} by their own anchor, ${stat.byTerm} by the term they name, ` +
    `${stat.byTitle} by a section TITLE they state that only one standard uses`);
  console.log(`  INFERRED: ${stat.byFile} name no standard, no term and no title, and were placed by their file's dominant anchor — a guess, ` +
    `so nothing below judges them (${stat.titleRefused} title check(s) and ${stat.numberRefused} number-exists check(s) refused on that ground); ` +
    `--unanchored lists them`);
  console.log(`  ${stat.other} belong to a standard this audit does not index; ${stat.skipped} name no standard and no term it knows`);
  console.log(`  audited by standard (in parentheses, how many of them only a file vote placed there): ` +
    `${[...byKey].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}(${byKeyVoted.get(k) || 0})`).join(" ")}`);
  console.log(`  ${stat.confirmed} confirmed (${stat.confirmedByContainment} by a subsection of the cited number, ${stat.confirmedByUse} by a prominent use rather than the definition site, ` +
    `${stat.confirmedByRun} by another number in the same citation's own run), ` +
    `${stat.unverified} carry no title and no term any index knows, ${stat.multiSpec} name a term more than one standard defines`);
  console.log(`  ${stat.foreignTerm} name a term only ANOTHER standard defines, so the standard they cite numbers nothing this audit could hold them to`);

  /* A TRUNCATED LIST THAT DOES NOT SAY IT IS TRUNCATED IS READ AS THE WHOLE LIST, AND THESE TWO LINES ARE THE
   * ONLY PLACE THIS REPORT DISCLOSES WHAT IT DID NOT LOOK AT. CLAUDE.md calls an unindexed standard "COUNTED
   * and never CHECKED, which is a silent zero rather than a clean bill" — that silence is only broken if a
   * reader can tell "these are all of them" from "these are the loudest fourteen". `css-images-3` sat at
   * thirty-one unchecked sites while a DFAIL in that area named the WRONG LEVEL of its own standard, and a
   * levelled shortname is recognized by a PATTERN rather than by any list, so nothing anywhere had its name
   * written down. THE FIX IS LEGIBILITY, NEVER SEVERITY: this file reports and does not fail, because a
   * citation is prose and a build that cannot land a spelling fix stops every lane. So the tail is COUNTED
   * rather than printed — naming a hundred CSS modules would bury the fourteen that matter — and the count is
   * what tells a reader there is a tail at all. The `>= 8` floor below is disclosed for the same reason: a
   * threshold nobody can see is indistinguishable from an empty result. */
  const tail = (all, shown) => {
    const rest = all.length - shown.length;
    if (!rest) return "";
    return `; ${rest} more not listed, ${all.slice(shown.length).reduce((n, [, v]) => n + v, 0)} citations between them`;
  };
  /* AND `--all` IS WHAT SAYS "I AM HERE TO WORK THROUGH THE TAIL", so under it the tail is PRINTED rather than
   * counted. The truncation above is right for the default run and it is exactly wrong for the one reader who
   * has come to close a coverage gap: the next standard worth a row is by definition NOT in the loudest
   * fourteen, and reading it required patching a copy of this file — which is a checker whose own output cannot
   * answer the question it exists to raise. `--all` already means "print every finding rather than the first
   * 120 of each kind" and an unaudited standard is a finding of a kind, so this is the flag's existing promise
   * applied to the one list that was outside it. */
  const full = argv.includes("--all");
  if (byOther.size) {
    const all = [...byOther].sort((a, b) => b[1] - a[1]), shown = full ? all : all.slice(0, 14);
    console.log(`  standards seen but not indexed: ${shown.map(([k, v]) => `${k}=${v}`).join(" ")}${tail(all, shown)}`);
  }
  const gaps = [...unknownTok].filter(([, v]) => v >= 8).sort((a, b) => b[1] - a[1]);
  if (gaps.length) {
    const shown = full ? gaps : gaps.slice(0, 20);
    console.log(`  capitalised tokens in front of a § that no list knows (a standard among these is coverage this audit is not getting): ` +
      `${shown.map(([k, v]) => `${k}=${v}`).join(" ")}${tail(gaps, shown)}` +
      `; tokens seen fewer than 8 times are not listed`);
  }

  if (argv.includes("--titles")) {
    /* WHERE A TITLE WOULD BUY THE MOST. The unverified population is a COUNT, not a list of findings — but it
     * is not uniform: a number cited forty times with no title anywhere is one edit away from being checkable,
     * and a number cited once is not worth anyone's afternoon. */
    console.log(`\nnumbers cited most often with neither a title nor a term any index knows — a title here makes the citation checkable:`);
    for (const [k, v] of [...untitled].sort((a, b) => b[1] - a[1]).slice(0, 40)) {
      const [key, no] = k.split(" §");
      const s = idx.get(key).sections[no];
      const g = untitledVoted.get(k) || 0;
      console.log(`  ${String(v).padStart(4)}x  ${k}  ${s ? `"${s.title}"` : "(no such section)"}` +
        (g ? `  — ${g} of them name no standard either, so the "${key}" half of this key is this audit's guess` : ""));
    }
  }

  if (argv.includes("--unanchored")) {
    /* THE ACTIONABLE HEAD OF THE GUESS, AND IT IS ORDERED BY WHO PAYS FOR IT BEING WRONG. A citation in a
     * comment is read with the file open; a citation in a DCHECK/DFAIL message is read by whoever is standing
     * at the abort, with nothing around it — so a bare number there sends that reader to a section of a
     * document the author never named, and the tree has been burned by exactly that (a DFAIL whose
     * instruction was spec-wrong, followed once). Within the crash set, a number the GUESSED standard
     * actually has is listed first: that is where the tool holds a concrete opinion it has no evidence for,
     * and where a reader skimming the audit would take the parenthesised standard as a fact. */
    const crash = voted.filter((v) => v.crash);
    const solid = crash.filter((v) => v.has), thin = crash.filter((v) => !v.has);
    console.log(`\ncitations naming NO standard that only a file vote placed, and that a CRASH PRINTS: ${crash.length} of ${voted.length} file-voted sites`);
    console.log(`  (the standard in parentheses is this audit's INFERENCE from the file's other citations, never the citation's own claim.`);
    console.log(`   Writing the standard's name at the site is what turns each of these into something any later run can check.)`);
    const cap = argv.includes("--all") ? Infinity : 120;
    for (const v of [...solid, ...thin].slice(0, cap)) {
      console.log(`  ${v.file}:${v.line}  §${v.no} (${v.spec}${v.has ? "" : ", which has no such section"})`);
      console.log(`      ${v.text.trim()}`);
    }
    if (crash.length > cap) console.log(`  … ${crash.length - cap} more (--all)`);
  }

  /* THE QUOTATION REPORT NAMES ITS AXIS AND ITS DENOMINATOR IN THE SAME LINE. CLAUDE.md: a coverage figure
   * states what it is a fraction of, or it is not a coverage figure — and every auditor here partitions, so
   * saying which question this one asked is what keeps a zero from reading as a clean bill on the others. */
  {
    console.log(`\nQUOTATION CHECK — do the words a citation puts in quotes occur in the section it names?`);
    console.log(`  (this asks about TEXT. It says nothing about whether the number exists, whether the algorithm lives there,`);
    console.log(`   or whether the claim the sentence makes is true — those are the checks above, and they are different axes.)`);
    console.log(`  ${qstat.seen} quotation(s) of ${MIN_COMPARED_WORDS}+ words stand in prose a citation governs; ${qstat.checked} were compared against a section's own words`);
    console.log(`    VERIFIED ${qstat.verified}  CONFIRMED-BY-A-NUMBER-THE-SAME-COMMENT-CITES ${qstat.okNearby}  WRONG-SECTION ${qstat.wrongSection} (${qstat.wrongSectionAncestor} of them at a section that CONTAINS the cited one)  WRONG-STANDARD ${qstat.wrongStandard}  NOT-FOUND ${qstat.notFound}` +
      ` (of which ${qstat.notFoundNothing} leave the standard within their first ${MIN_FRAGMENT_WORDS} words — a fabricated sentence and a piece of this tree's own prose in quotation marks both land there, and nothing mechanical separates them)`);
    console.log(`  NOT CHECKED, and why: ${qstat.noCorpus} cite a standard with no committed text corpus` +
      (noCorpusBy.size ? ` (${[...noCorpusBy].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join(" ")})` : "") +
      `; ${qstat.voted} sit under a citation naming no standard, whose standard only a file vote placed` +
      `; ${qstat.noSection} cite a §N the standard does not have (the corpus holds text for every section its index has, so this is the UNKNOWN-SECTION population above)` +
      `; a further ${qstat.tooShort} quoted run(s) are shorter than ${MIN_COMPARED_WORDS} words or carry no fragment of ${MIN_FRAGMENT_WORDS} and are not quotations this check can falsify`);
    if (argv.includes("--gaps")) {
      const b = new Map();
      for (const [g, ok] of gapHist) { const k = Math.min(9, Math.floor(g / 60)); const r = b.get(k) || [0, 0]; r[ok]++; b.set(k, r); }
      for (const k of [...b.keys()].sort((a, z) => a - z))
        console.log(`  gap ${k * 60}-${k * 60 + 59}${k === 9 ? "+" : ""} chars after the §: verified ${b.get(k)[1]}, not found ${b.get(k)[0]}`);
    }
    if (txtStale.size) for (const [k, why] of txtStale)
      console.log(`  ${k}: text corpus REFUSED — ${why}; re-run: node engine/citegen.mjs --regen ${k}`);
    const qg = new Map();
    for (const q of quotes) { if (!qg.has(q.kind)) qg.set(q.kind, []); qg.get(q.kind).push(q); }
    const qlimit = argv.includes("--all") ? Infinity : 60;
    for (const kind of ["QUOTE-NOT-FOUND", "QUOTE-WRONG-STANDARD", "QUOTE-WRONG-SECTION"]) {
      const g = qg.get(kind) || [];
      console.log(`\n${kind}: ${g.length}`);
      /* A CRASH PRINTS ITS MESSAGE TO SOMEONE WHO HAS NO FILE OPEN, so a fabricated quotation inside one is
       * listed first — the same ordering, and the same reason, as --unanchored's. */
      const rank = (q) => (q.crash ? 0 : 2) + (q.div && q.div.matched < MIN_FRAGMENT_WORDS ? 0 : 1);
      const ord = [...g].sort((a, b) => rank(a) - rank(b));
      for (const q of ord.slice(0, qlimit)) {
        console.log(`  ${q.file}:${q.line}  ${quoteMsg(q)}`);
        console.log(`      "${q.quote.length > 150 ? q.quote.slice(0, 150) + "…" : q.quote}"`);
      }
      if (g.length > qlimit) console.log(`  … ${g.length - qlimit} more (--all)`);
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
  const changed = execFileSync("git", ["diff", "--name-only", ref, "--", "*.c", "*.h", "*.md"],
    { cwd: ROOT, encoding: "utf8" }).split("\n").filter(Boolean);
  const files = changed.map((r) => join(ROOT, r)).filter((p) => existsSync(p));
  if (!files.length) { console.log(`no .c/.h/.md file differs from ${ref} — nothing for this mode to compare`); return; }

  const baseSrc = new Map();
  for (const p of files) {
    const rel = relative(ROOT, p);
    try { baseSrc.set(p, execFileSync("git", ["show", `${ref}:${rel}`], { cwd: ROOT, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 })); }
    catch { baseSrc.set(p, ""); }        /* absent at ref — a new file owns every finding in it */
  }
  const key = (f) => `${f.file}\u0000${f.kind}\u0000${f.no}\u0000${f.msg}\u0000${f.qtext || ""}`.replace(/:\d+/g, "");
  const tip = audit(argv, { files, quiet: true });
  const base = audit(argv, { files, quiet: true, srcOf: (p) => baseSrc.get(p) });
  const had = new Set(base.map(key));
  const added = tip.filter((f) => !had.has(key(f)));
  const gone = base.filter((f) => !new Set(tip.map(key)).has(key(f)));

  console.log(`spec-citation delta against ${ref}: ${files.length} changed .c/.h/.md file(s), ` +
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
