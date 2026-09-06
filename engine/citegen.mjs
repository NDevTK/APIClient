/* Spec-citation gap AUDITOR — the same job engine/idlgen.mjs does for Web IDL, done for the section numbers
 * this tree cites. It reads the REAL spec text, builds an index of what each standard actually numbers and
 * names, and DIFFS every citation in the tree against it. It PRINTS what disagrees; it does not rewrite a
 * comment and it does not generate one.
 *
 *   node engine/citegen.mjs [path …]      audit (default: engine/host + the fork's own quickjs.c/.h)
 *   node engine/citegen.mjs --all         print every finding rather than the first 120 of each kind
 *   node engine/citegen.mjs --titles      EVERY row of the NUMBER-ONLY band, which the default run heads at 24 —
 *                                         each distinct § cited with no title beside it, and what the corpus says that § IS
 *   node engine/citegen.mjs --unanchored  the citations naming no standard that only a file vote placed — the
 *                                         ones inside a DCHECK/DFAIL/CHECK message first, since a crash prints them
 *   node engine/citegen.mjs --agree       the passages this tree quotes SEVERAL WAYS where the difference is NOT one
 *                                         inserted clause — counted on every run, listed only here, never accused
 *   node engine/citegen.mjs --gaps        the quotation check's verified/not-found split by distance from the §
 *   node engine/citegen.mjs --steps       the step numbers the step checks counted and REFUSED to accuse — those
 *                                         whose cited section holds no list reaching them, which is not the same
 *                                         claim as the section being wrong, and those whose claim about a step's
 *                                         CONTENT stands over several lists at once, where the number names
 *                                         several different steps and no reading can be accused
 *   node engine/citegen.mjs --regen [key] fetch the standard(s), rewrite engine/specindex/<key>.json
 *
 * WHY THIS EXISTS. CLAUDE.md §Browser half: a named spec with no number cannot be looked up, so it cannot
 * be checked, so it is indistinguishable from a recollection — and a WRONG number is worse than none,
 * because it reads as authoritative and sends the next reader to a section that does not say what the code
 * claims. That failure mode has NO SYMPTOM: nothing crashes, no gate goes red, and the citation looks exactly
 * like a correct one. The only thing that can catch it is the spec text itself, so this reads the spec text.
 *
 * AND THE SAME RULE RUNS BACKWARDS THROUGH THIS TOOL: a wrong FINDING is worse than none, for the identical
 * reason. It reads as authoritative, it stands at a site nobody can repair, and it teaches the reader to skim
 * the category it sits in — which is every real finding beside it. MENTION-NOT-CLAIM is where that is paid:
 * a citation the prose is TALKING ABOUT (a retired number kept beside its replacement, a spelling displayed
 * as a specimen) is not a citation the prose is MAKING, and accusing one is this file committing its own
 * subject. See mentionNotClaim. Nothing is suppressed — the category prints every site with the verdict the
 * checker withheld, and a retirement note that names a title its number does not carry becomes a finding of
 * its own (RETIREMENT-NOTE-WRONG) rather than an exemption.
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
 *     their guessed standard has, which is everything that check needs. It produced nothing the day it was
 *     gated, and "it has not fired yet" is not a property of a mechanism — which the tree has since settled
 *     the other way: the run prints a NON-ZERO count of title checks refused on that ground, so the gate is
 *     now suppressing claims a vote would have made rather than standing idle. Read the number in the
 *     INFERRED line; do not read one from here, because a count in a header is a fact about a revision.
 *     A MISATTRIBUTED cannot arise from a vote at all, and
 *     that is structural rather than lucky: the vote is only reached when the file's anchor is NOT among the
 *     standards the citation's own term evidence names, so `owned` below is false by construction.
 * WHAT THAT LEAVES IS A COUNT, AND A COUNT WITH NO LIST BEHIND IT IS THE SILENT-ZERO SHAPE THIS FILE ALREADY
 * NAMES ELSEWHERE. 8299 cannot be printed — a category that size is read once and never again, which is the
 * muting this whole file is written against. So it is printed the way the NUMBER-ONLY band prints its own
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
 * AND THE READING THAT COSTS THIS CHECK ITS CREDIT IS `VERIFIED, THEREFORE THE COMMENT IS RIGHT` — MEASURED,
 * ON A SITE REPAIRED FOR THE OPPOSITE REASON. A comment argued a length component's invariant from
 * css-values-4 §10.10.1 "Simplification" and quoted it; the repair that followed called the quotation a
 * PARAPHRASE IN QUOTATION MARKS whose words "occur nowhere in that section", and called this tool blind to it
 * because the bare §10.10.1 sat in the file-voted NOT-CHECKED bucket. BOTH HALVES WERE FALSE AND EACH WAS ONE
 * COMMAND FROM BEING CHECKED. The words are §10.10.1's own Note verbatim — "…they can only be combined with
 * other values that have identical units" — the stated title RESOLVED the citation (a title is evidence and
 * outranks the vote, which is why that rule exists), and this check COMPARED it and answered VERIFIED. The
 * mistake was real and it was the AIM: §10.10.1 governs a math function's residue, and the kind the assert
 * below it is about cannot reach that section at all.
 * SO THE ONE THING THIS CHECK CANNOT SEE IS THE ONE THING THAT WAS WRONG, and it says so two paragraphs up —
 * "it says NOTHING about whether the sentence is TRUE of the tree or whether the claim it supports is right".
 * A VERIFIED is a fact about SEVEN WORDS and never about the paragraph they were put in. The reflex worth
 * naming, because it arrived from a careful reader: a comment found to be arguing the wrong thing FEELS like a
 * comment whose quotation must also be invented, and the two are INDEPENDENT — a mis-aimed citation under a
 * perfectly pasted sentence is the commonest shape there is, and calling it a fabrication publishes a claim
 * about a standard that the standard refutes. Read the section before writing that a sentence is not in it.
 *
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
 * AND THE QUOTATIONS IT REFUSES ARE A CATEGORY WITH SITES IN IT, NOT A CLAUSE IN A CENSUS SENTENCE. The check
 * above judges a quotation only where the citation over it resolved on its OWN evidence, so a quotation under a
 * BARE §N — the largest citation shape in this tree — is counted and never asked about. Counted-and-unchecked is
 * the silent zero CLAUDE.md names in the same breath as an unindexed standard, and one level down it was true
 * here: the census said HOW MANY and never WHICH, so a reader standing in the file could not learn that a
 * quotation two lines away was unaskable. UNJUDGEABLE QUOTATION names them, one line each, on every run.
 *   — IT IS NOT A FINDING, and its first sentence says so, because the failure of getting that wrong is every
 *     correct site in it reported as a defect: this file's own subject, committed by this file. A site there may
 *     be quoting its standard perfectly. UNVERIFIABLE and WRONG are two states.
 *   — THE PROBE IS EVIDENCE AND NOT A RESOLUTION. A bare §N is genuinely ambiguous and only its AUTHOR can say
 *     what it meant, so the tool states a fact and adopts nothing: whether ANY indexed standard numbers that
 *     section AND holds those words there. Adopting the answer instead would let a quotation VERIFY ITSELF — the
 *     standard would have been chosen BECAUSE it holds the words — and would silently absorb a citation whose
 *     real standard this tool does not index, which is the confirmation channel the paragraph above refuses
 *     arriving from the other direction. So CORROBORATED-ELSEWHERE names the standard for the author to write,
 *     and UNCORROBORATED says no indexed document has these words at that number, which is where a fabrication
 *     under a bare number would sit and is therefore the head of the queue.
 * WHAT DRAINING IT DOES TO THE NUMBERS IS THE REASON THE DENOMINATOR IS PRINTED BESIDE IT: writing a standard at
 * one of these sites moves the quotation into the COMPARED population, where it is judged for the first time, so
 * a repair can RAISE the finding total. A finding count is only ever read against the population it was drawn
 * from, which is the rule the closing census already states for the tree and this states for one channel.
 *
 * AND A STEP IS A NUMBER NEITHER ARTIFACT ABOVE CAN SEE, WHICH IS WHY IT IS A THIRD. The section index holds
 * HEADINGS and the text corpus holds WORDS with the markup flattened out of them by design — that flattening is
 * what makes a quotation comparable — so a step, which is a POSITION IN A LIST, is invisible to both BY
 * CONSTRUCTION rather than by omission. CLAUDE.md names the shape and the cost: one file carried 110 citations,
 * produced zero findings, and had a whole cluster off by one; a nested list promotes its sub-items to peers so an
 * algorithm of true length 36 counts as 39; and the drift starts one step LATER than anybody spot-checks, so the
 * first number a reader tests is always the one that is still right. So --regen writes a THIRD artifact,
 * engine/specindex/steps/<key>.json, holding each section's list structure — see collectCorpus, which fills it
 * from the SAME boundary walk as the text so a step can never be filed under a section its own words are not.
 * It is a small fraction of the text corpus because it stores COUNTS AND OFFSETS and never prose: each item's
 * extent is two integers INTO the text corpus, so the standard's sentences exist in exactly one place and the
 * two artifacts cannot come to disagree about what a step says.
 *
 * IT ANSWERS TWO QUESTIONS AND THEY ARE NOT THE SAME QUESTION. The first is whether the number CAN EXIST, which
 * is the drift caught after it runs off the end of a list. The second is whether the step it names SAYS what the
 * citation says it says, which is that same drift caught while it is still IN RANGE — `step 16's string` where
 * the string arm is step 15 and step 16 is another clause of the one ladder, a citation nothing else here can
 * fault: the number resolves, the section is right, no quotation is being made. What still separates 6.4 from
 * 6.5 where neither carries the claim's own words is MEANING, and a reader must do that.
 *
 * TWO BANDS, AND ONLY ONE OF THEM IS A CLAIM ABOUT THE CITATION — the same asymmetry check (1) draws when it
 * refuses to ask whether a term-resolved standard HAS the cited number.
 *   — CORROBORATED: the path's own PREFIX exists in the cited section, so that section demonstrably holds the
 *     list the citation indexes into and the number that overflows it is wrong about that list. This is the
 *     finding, STEP-OUT-OF-RANGE.
 *   — NOT-IN-THIS-SECTION: no list under the section reaches the step at all. That is TRUE and it is NOT
 *     evidence the citation is wrong, because it is equally consistent with the section being right for the
 *     TERM while the step belongs to an algorithm the prose names IN WORDS one heading away — which is how this
 *     tree's comments are written. It is counted in the census and listed under --steps, never accused.
 * BOTH BANDS WERE READ AGAINST FETCHED TEXT BEFORE THE SPLIT WAS CHOSEN, which is the only reason it is a rule
 * rather than a threshold. At the revision this was measured the uncorroborated band was noise — `range.c`
 * numbering the EXTRACT algorithm's steps under a citation of `clone a node` that is exactly right, `node.c`'s
 * bare `STEP 13` markers governed by whatever § came last, `navigation.h` saying outright "the inner algorithm's
 * step 23" — while 26 of the corroborated band were read and 24 were real: Web IDL §3.12's `step 10.2/10.4/10.5`
 * where `call a user object operation` is §3.11 and its step 10 has exactly those five sub-steps; eight
 * IndexedDB §5.7 sites whose 9.x are the 10.x of `upgrade a database`; DOM §2.9's dispatch cluster, where 6.9.7
 * is a leaf and `set target to parent` is 6.9.8.1; `initialize a response`'s six steps under Fetch §5.5's `step
 * 8.1`; §4.10.21.1 "Definitions", which holds no algorithm while §4.10.21.2's fires the `invalid` event; and
 * three sites citing HTML §7.4.2.1 for `allowed by sandboxing to navigate`, which is §7.4.2.4's.
 * THE TWO THAT WERE WRONG SHARE ONE MECHANISM AND IT IS NAMED HERE RATHER THAN LEFT TO BE REDISCOVERED: a step
 * reference carries no § of its own, so it is attributed to the nearest citation BEFORE it — PASS 4's rule,
 * reused rather than restated — and a comment that cites HTML for one thing and then discusses a DOM step is
 * charged with the wrong section. A number some OTHER citation in the same prose unit admits is confirmed and
 * counted apart, which is what keeps that mechanism to the residue it is; what escapes is the case where the
 * owning algorithm is named in ENGLISH and nowhere in a number.
 *
 * AND THE CONTENT QUESTION SPLITS THE SAME WAY, ON EVIDENCE READ THE SAME WAY. A claim is only ever a
 * POSSESSIVE — `step N's X`, the one form that attaches a thing to a list position in one breath — and only
 * where the author BOUNDED the phrase: a bare word, or a run the standard itself defines as a term. Then:
 *   — ONE READING: the cited section holds exactly one list reaching that step, so the number names ONE step,
 *     and a rival step that DOES carry the word stands in that same list. This is the finding,
 *     STEP-SAYS-OTHERWISE.
 *   — SEVERAL READINGS: the number names as many steps as there are lists, which is CLAUDE.md's ambiguous
 *     sub-number — every candidate reading confirms it — and the rival would be picked from whichever list
 *     happened to hold the word, which can be another algorithm entirely. Counted, listed under --steps, never
 *     accused.
 * BOTH BANDS WERE READ, at the revision this landed, against the fetched standards rather than against the
 * corpus that produced them. Of the several-reading band three of ten were real; of the one-reading band the
 * only row that was not real was a retirement note quoting a step claim in order to RETIRE it — which is the
 * use-versus-mention defect this file already names, and it is now asked at the step and not only at the
 * citation. What is left in the reported band is what the standard's own words falsify.
 * WHAT IS NOT COVERED, NAMED: the EXISTENCE check does not ask the use-versus-mention question — only the
 * content check does, because that is where a disclaimed row was measured. A retirement note quoting an
 * out-of-range number would therefore still be accused. The next diff asks mentionOf at the same point in the
 * existence check, AFTER its band has been read the way this one's was; its absence shows as a
 * STEP-OUT-OF-RANGE row whose surrounding prose says the number STOOD as something.
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
/* AND THE STEP CORPUS IS A THIRD ARTIFACT FOR THE SAME REASON THE SECOND IS ONE: a standard can have a section
 * index, or that plus its words, or that plus the SHAPE of its lists, and the difference has to be
 * REPRESENTABLE so that "this standard's steps are not checked" is a state the filesystem says out loud rather
 * than a silence indistinguishable from a clean run. It is written by the same --regen walk as the text — see
 * collectCorpus — so the two can never be keyed by different section boundaries. */
const STEPS_DIR = join(INDEX_DIR, "steps");
const stepsFileOf = (key) => join(STEPS_DIR, key + ".json");

/* A section that LINKS a term this many times is a section the term is ABOUT. One passing reference is not a
 * subject, and treating it as one would confirm every citation of every chapter that mentions anything. */
const USE_FLOOR = 3;

/* THE REGISTRY. Everything standard-specific is here; the audit below reads only {sections, dfns, uses}.
 * `anchors` are the names this tree writes in front of a §, lowercased. A standard NOT listed here is not
 * audited — its citations are counted and named in the report so the blind spot is printed rather than
 * assumed to be zero. */
/* `edition` IS THE ANSWER TO "WHICH OF THIS STANDARD'S DOCUMENTS DOES A CITATION MEAN", AND IT IS DECLARED
 * RATHER THAN INFERRED BECAUSE A URL DOES NOT STATE IT. A spec with an Editor's Draft and a /TR/ snapshot is
 * TWO DOCUMENTS: different section numbers, different wording, sometimes a rule the working group has since
 * replaced. A tree that resolves that per author, in whichever direction whoever was standing there happened
 * to fetch, ends up citing both with nothing recording which — so the row answers it once, here.
 *   "maintained" — the document this standard's own EDITORS WRITE IN. For a CSSWG module that is the Editor's
 *     Draft; for a living standard it is the only document there is.
 *   "final"      — the editors have STOPPED, so the published Recommendation is not a snapshot OF anything
 *     that moves, it IS the standard. CSS 2.1, CSS 2.2 and XML 1.0 are here, and they are instances of the
 *     rule rather than exceptions to it.
 * WHY MAINTAINED AND NOT THE SNAPSHOT, WHICH IS THE ANSWER THAT LOOKS SAFER: a citation's job is to be
 * CHECKABLE, and stability is a different property that is easily mistaken for it. A snapshot never moves, so
 * nothing ever reports that the implementation target moved AWAY from it — a citation of one goes on reading
 * authoritative while the component beneath it implements a rule its own standard replaced, which is the
 * stale-claim failure mode with no instrument that can see it. A maintained document moves, and every
 * mechanism for THAT already runs on every audit: the title beside the number survives a renumber, --regen
 * prints each moved section, and the loader refuses a corpus whose edition disagrees with the numbers it is
 * keyed by. So the residual cost of this answer is a renumber landing between two regens, which is loud and
 * one command from being found, against the other answer's cost of being quietly wrong for years.
 * THE DECLARATION IS CHECKED TWO WAYS, because a rule that only reads well is a rule that drifts. A row
 * calling itself "maintained" may not point at a /TR/ snapshot (see below), and no committed corpus is
 * consulted until its recorded `base` is confirmed to be the document THIS row names (see audit) — otherwise
 * retargeting a standard at another edition would silently re-point every quotation at a corpus of the other
 * document, which is the one failure this field exists to make impossible. */
const SPECS = [
  { key: "html", label: "HTML Living Standard", kind: "whatwg-multipage",
    base: "https://html.spec.whatwg.org/multipage/", edition: "maintained", anchors: ["html", "htmls"] },
  { key: "ecmascript", label: "ECMAScript Language Specification", kind: "tc39-multipage",
    base: "https://tc39.es/ecma262/multipage/", edition: "maintained",
    anchors: ["ecmascript", "ecma", "ecma262", "ecma-262", "es", "tc39", "js"] },
  { key: "dom", label: "DOM Standard", kind: "bikeshed",
    base: "https://dom.spec.whatwg.org/", edition: "maintained", anchors: ["dom"] },
  { key: "url", label: "URL Standard", kind: "bikeshed",
    base: "https://url.spec.whatwg.org/", edition: "maintained", anchors: ["url"] },
  { key: "fetch", label: "Fetch Standard", kind: "bikeshed",
    base: "https://fetch.spec.whatwg.org/", edition: "maintained", anchors: ["fetch"] },
  /* STREAMS EARNED ITS ROW BY BEING THE BLIND SPOT THAT COST A HAND-AUDIT. `core/streams/pipe.c` cited
     §4.2.4 for ReadableStreamPipeTo at twenty-three sites — every stage label, every DCHECK and the
     `algorithm` string — and the operation is defined at §4.9.1 "Working with readable streams"; §4.2.4
     "Constructor, methods, and properties" is where `pipeTo` and `pipeThrough` are and it only CALLS it. The
     audit reported NOTHING, because an unindexed standard's citations are counted under OTHER_SPECS and never
     checked, and 226 of them were. That is the coverage loss this file's own header says is printed rather
     than assumed to be zero — and printing it is what made someone read the number. */
  { key: "streams", label: "Streams Standard", kind: "bikeshed",
    base: "https://streams.spec.whatwg.org/", edition: "maintained", anchors: ["streams"] },
  /* OBSERVABLE, AND IT IS AN INDEX RATHER THAN A FOREIGN ROW BECAUSE A REFUSAL WOULD HAVE BOUGHT SILENCE ON A
     WHOLE COMPONENT. The two answers are not equally honest here, and the choice is decided by what this tree
     already writes: `core/dom/observable.c`, `observable_ops.c` and `observable_impl.h` carry three hundred-odd
     citations between them and the standard numbers exactly the sections they name: §2.1 The Subscriber
     interface, §2.2 The Observable interface, §2.2.1 Supporting concepts, §2.3.1 from(), §2.3.2 and §2.3.3,
     §3 EventTarget integration.
     WHAT THE ABSENCE OF THIS ROW WAS DOING WAS NOT COUNTING THEM — IT WAS JUDGING THEM AGAINST ANOTHER
     DOCUMENT, which is the `file system access` failure and not the Streams one. An unanchored §2.1 in an
     IDL-and-HTML-dense file falls to the file vote, and the vote had somewhere wrong to go: the quotation of
     the fully-active guard the four Subscriber members open with was compared against HTML §2.1 and reported
     as not found, and `fully active` — an HTML term the Observable prose uses — resolved that §2.1 to HTML on
     its own evidence and raised a misattribution. Both are false and neither is an edit any author could make
     at the site. A foreign row would have turned those two into a refusal, which is honest and permanent; an
     index turns them into a VERIFIED, because the sentence is Observable §2.1's own. MEASURED on one frozen
     tree, this row alone: 166 citations moved OUT of the unjudged bands and into the judged population, +10
     VERIFIED, three false agreement sites cleared, and nine misattributions of one Observable section for
     another newly SEEN. The cost of the index is the one every row here carries — a renumbering between
     two regens, which `--regen` prints — against a refusal's cost of never checking a component this engine
     implements in full. */
  { key: "observable", label: "Observable", kind: "bikeshed",
    base: "https://wicg.github.io/observable/", edition: "maintained",
    anchors: ["observable", "observable standard"] },
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
    base: "https://webidl.spec.whatwg.org/", edition: "maintained", anchors: ["idl", "webidl", "web idl"] },
  /* `database` alone is an English word this tree writes in prose, so this standard is anchored ONLY by the
     two-word name it is actually cited under. Every one of its citations spells it that way. */
  { key: "database", label: "Indexed Database API", kind: "bikeshed",
    base: "https://w3c.github.io/IndexedDB/", edition: "maintained", anchors: ["indexed database", "indexeddb"] },
  { key: "cssomview", label: "CSSOM View Module", kind: "bikeshed",
    base: "https://drafts.csswg.org/cssom-view/", edition: "maintained", anchors: ["cssom view", "cssom-view"] },
  { key: "cssom", label: "CSS Object Model (CSSOM)", kind: "bikeshed",
    base: "https://drafts.csswg.org/cssom/", edition: "maintained", anchors: ["cssom"] },
  { key: "csp", label: "Content Security Policy Level 3", kind: "bikeshed",
    base: "https://w3c.github.io/webappsec-csp/", edition: "maintained", anchors: ["csp"] },
  { key: "xhr", label: "XMLHttpRequest Standard", kind: "bikeshed",
    base: "https://xhr.spec.whatwg.org/", edition: "maintained", anchors: ["xhr", "xmlhttprequest"] },
  { key: "fileapi", label: "File API", kind: "bikeshed",
    base: "https://w3c.github.io/FileAPI/", edition: "maintained", anchors: ["file api", "fileapi"] },
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
    base: "https://fs.spec.whatwg.org/", edition: "maintained", anchors: ["file system", "fs"] },
  /* THE ENCODING STANDARD, PROMOTED OUT OF OTHER_SPECS, AND IT IS THE SHARPEST CASE IN THIS TABLE FOR WHY A
     FOREIGN ROW IS A SILENT ZERO RATHER THAN A CLEAN BILL — because the blind spot was a WHOLE COMPONENT and
     the component is a table of section numbers. `core/encoding/encoding.c` is eleven decoders, and each one
     is introduced by the number of the section that defines it; the audit before this row raised ZERO
     misattributions over that whole directory and the file's numbering was SHIFTED throughout. Nor was the
     silence only a coverage gap — with no row here the file vote read this component as WEB IDL, which is the
     wrong-answer direction the hrtime row below describes: 72 of its citations were placed on a standard the
     component never names. THE DEFECT SHAPE, which is what generalises:
     this standard numbers its legacy families as siblings of the utf-8 one — §8 "The encoding", §9 "Legacy
     single-byte encodings", §10 and §11 "Legacy multi-byte Chinese (simplified)/(traditional) encodings",
     §12 "Legacy multi-byte Japanese encodings", §13 "Legacy multi-byte Korean encodings" and §14 "Legacy
     miscellaneous encodings" — so a reader who counts the families and forgets that utf-8 is one of them is off by
     exactly one for every family after it, and every number they write is a REAL section of this standard
     naming a DIFFERENT decoder. That is the failure this whole file exists for: not a number the standard
     lacks, which a reader notices, but a number it has.
     ITS ANCHOR IS THE ONE-WORD NAME, AND THAT IS THE `database` HAZARD — a common noun this tree writes in
     prose — SO IT WAS CHECKED RATHER THAN ASSUMED, TWO WAYS. Every `encoding §` in the audited tree is
     written with a capital E and is a citation of this standard; the lowercase form a `character encoding`
     or `an output encoding` sentence would produce appears NOWHERE in front of a §, and no C string literal
     ends on the word with the next literal opening on the §, which is the split-literal shape that makes a
     grep answer a false zero. anchorTokens strips a trailing `Standard`, so `Encoding Standard §7.2` and
     `Encoding §7.2` are one spelling by the time classifyAnchor sees them. */
  { key: "encoding", label: "Encoding Standard", kind: "bikeshed",
    base: "https://encoding.spec.whatwg.org/", edition: "maintained", anchors: ["encoding"] },
  /* THE MIME SNIFFING STANDARD, PROMOTED OUT OF OTHER_SPECS ONE COMMIT AFTER THE FOREIGN ROW THAT STOPPED THE
     GUESS — which is the order the `file system` row argues for and the reason both diffs exist. A foreign row
     REFUSES; only an index ANSWERS, and here the thing to answer is a whole component. `core/mime` is the
     MIME Sniffing Standard written out: `mime_type.c` is `§4.4` parse and `§4.5` serialize, `mime_type.h`
     declares the `§4.6` groups, and `mime_sniff.c` opens by naming `§5` "Handling a resource", `§6` "Matching
     a MIME type pattern" and `§7` "Determining the computed MIME type of a resource" — every one of those an
     exact heading of this document. Not one of them was checked against it. The file vote read the whole
     directory as FETCH, which is not a near miss: this standard's `§4.6` is "MIME type groups" and Fetch's is
     "HTTP-network-or-cache fetch", so the declaration that owns the groups table was REPORTED as misattributed
     to a document `mime_type.h` never names. That is the accusation direction, manufactured by a vote, out of
     a citation that was right.
     THE NUMBERS COLLIDE WITH FETCH'S ALL THE WAY DOWN, which is why the vote was so confident and so wrong:
     this document's `§4` is "MIME types" where Fetch's is "Fetching", `§5` is "Handling a resource" where
     Fetch's is "Fetch API", `§6` is "Matching a MIME type pattern" where Fetch's is "data: URLs". Real
     numbers, all of them, none of them the right document — and `core/mime` is not the only reader: `§5.2`
     "Reading the resource header" and the `§7`/`§8` sniffing tables are cited from core/image, core/loader,
     core/html and core/xhr, all of which vote for something else again.
     ITS ANCHORS ARE THE NAME AND THE SHORTNAME AND NOT THE LAST WORD. `mimesniff` is the spelling that was on
     the foreign list and is moved here rather than left behind, because an anchor on two lists is a second
     copy of one fact and the copy that drifts is the one nobody runs. `sniffing` alone is deliberately NOT an
     anchor: it is the word `safe-fetch.js` and SECURITY.md write for content-type sniffing as an activity, and
     a one-word anchor there is the `database` hazard with a whole trusted-zone file behind it. anchorTokens
     flattens a line break inside the name, so the split literal `"… MIME "` / `"Sniffing §5"` that
     `mime_sniff.c` opens with presents as one spelling by the time classifyAnchor is asked. */
  { key: "mimesniff", label: "MIME Sniffing Standard", kind: "bikeshed",
    base: "https://mimesniff.spec.whatwg.org/", edition: "maintained", anchors: ["mime sniffing", "mimesniff"] },
  /* THE COOKIE STORE API, AND ITS BASE IS THE ONE THING ABOUT IT A READER MUST NOT RECALL. This standard
     MOVED ORGANISATION: the two addresses a search returns for it — `wicg.github.io/cookie-store/` and
     `w3c.github.io/webappsec-cookie-store/` — both answer 404 today, and the document lives at
     `cookiestore.spec.whatwg.org` as a WHATWG Living Standard whose own boilerplate records the move from
     the W3C WICG. A row carrying either dead address would not fail loudly at the site of the mistake; it
     would fail at the next `--regen`, which is the one command nobody runs on the day they add a row.
     WHAT ITS ABSENCE WAS DOING WAS THE SILENT ZERO. Thirteen `Cookie Store API §N` sites stand in five files
     — `cookie_store.c`/`.h`, `loader/cookie_jar.c`/`.h` and `url/registrable_domain.h` — and every one of
     them named a standard nothing here indexed, so not one was checked. The numbers they name are real and
     this standard has all of them: §3 "The CookieStore interface" with §3.1 get() and §3.2 getAll() and
     §3.4 delete(), §6.1 "The Window interface", §7.1 "Query cookies" and §7.2 "Set a cookie".
     ITS ANCHOR IS THE THREE-WORD NAME AND MUST BE, for the reason `state token api` gives on the foreign
     list: anchorTokens reads AT MOST THREE trailing words, so `cookie store api` is the longest tail the
     tokenizer can ever produce for this standard and a longer spelling would be an entry with no reader.
     The one site that writes `Store API` alone is a C string literal split mid-name (`"… Cookie "` then
     `"Store API §3.1 …"`), and anchorTokens already flattens the join, so it presents the same three words.
     `cookie store` alone is NOT listed: nothing in this tree writes it in front of a §, and an anchor no
     citation produces is a claim about a spelling rather than about a document. */
  { key: "cookiestore", label: "Cookie Store API", kind: "bikeshed",
    base: "https://cookiestore.spec.whatwg.org/", edition: "maintained", anchors: ["cookie store api"] },
  { key: "permissions", label: "Permissions", kind: "respec",
    base: "https://w3c.github.io/permissions/", edition: "maintained", anchors: ["permissions"] },
  /* FULLSCREEN, AND IT IS THE MANUFACTURED-FINDING SHAPE THIS TABLE'S hrtime ROW DESCRIBES RATHER THAN A SILENT
     ZERO. Its section numbers are SMALL — §2 "Model", §3 "API", §7 "Permissions Policy Integration" — which is
     exactly the range every indexed standard also numbers, so with no row here a `§3` of this standard is
     placed by a file vote onto whichever standard the file otherwise cites and then JUDGED there: the
     quotation check measured Fullscreen §3's `fullscreenEnabled` sentence against Web IDL §3 "JavaScript
     binding" and reported a divergence, and `§3 "Top Layer"` was reported as a title Web IDL's §3 does not
     carry. Both citations were correct. A one-line row is the whole cost of not doing that.
     ITS ANCHORS ARE THE ONE-WORD NAME AND THE TWO-WORD ONE, and the one-word anchor is safe HERE where
     `permissions` is not: anchorTokens matches WHOLE words separated by whitespace, so `allowfullscreen` and
     `onfullscreenchange` cannot present a bare `fullscreen`, and no other row in this table is a tail of it. */
  { key: "fullscreen", label: "Fullscreen API Standard", kind: "bikeshed",
    base: "https://fullscreen.spec.whatwg.org/", edition: "maintained", anchors: ["fullscreen api", "fullscreen"] },
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
    base: "https://w3c.github.io/webappsec-permissions-policy/", edition: "maintained", anchors: ["permissions policy"] },
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
    base: "https://w3c.github.io/hr-time/", edition: "maintained", anchors: ["hr-time", "hrtime", "high resolution time"] },
  /* THE THREE STANDARDS THAT SIT ON TOP OF THE ROW ABOVE, and they are here for the reason that row's own
     comment gives rather than for their site count: the vocabulary they own COLLIDES with vocabulary an
     indexed standard also defines, so the resolver has somewhere wrong to go. `performance` is on OTHER_SPECS
     and `timeline`, `timing` and `entry` are words this tree writes constantly — and classifyAnchor asks the
     LISTED names longest-first, so a two-word anchor is what keeps `PERFORMANCE TIMELINE §3` from resolving to
     the foreign `performance` and `USER TIMING §2.1.1` from falling to a file vote that would judge it against
     HIGH RESOLUTION TIME, whose §2 is a different document's section entirely.
     ALL THREE ARE BIKESHED and all three carry a `dt-updated`, so none needs a reader of its own.
     NAVIGATION TIMING IS INDEXED FOR A SECTION IN ITS `Obsolete` CHAPTER — §8.1 The PerformanceTiming
     interface, whose twenty-one read-only attribute NAMES User Timing §2.2.1 step 1 refuses as mark names.
     That is a fact about the standard rather than about what this engine exposes, which is exactly the kind of
     claim that must be checkable: an obsolete section is the one most likely to be renumbered out from under
     a citation, and the title beside the number is what would survive it. */
  { key: "usertiming", label: "User Timing", kind: "bikeshed",
    base: "https://w3c.github.io/user-timing/", edition: "maintained", anchors: ["user timing", "user-timing"] },
  { key: "perftimeline", label: "Performance Timeline", kind: "bikeshed",
    base: "https://w3c.github.io/performance-timeline/", edition: "maintained",
    anchors: ["performance timeline", "performance-timeline"] },
  { key: "navtiming", label: "Navigation Timing", kind: "bikeshed",
    base: "https://w3c.github.io/navigation-timing/", edition: "maintained",
    anchors: ["navigation timing", "navigation-timing"] },
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
    base: "https://drafts.csswg.org/css-images-3/", edition: "maintained", anchors: ["css-images-3"] },
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
    base: "https://drafts.csswg.org/css-values-4/", edition: "maintained", anchors: ["css-values-4"] },
  { key: "csssizing3", label: "CSS Box Sizing Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-sizing-3/", edition: "maintained", anchors: ["css-sizing-3"] },
  { key: "csstext3", label: "CSS Text Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-text-3/", edition: "maintained", anchors: ["css-text-3"] },
  { key: "csstext4", label: "CSS Text Module Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-text-4/", edition: "maintained", anchors: ["css-text-4"] },
  { key: "csswritingmodes4", label: "CSS Writing Modes Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-writing-modes-4/", edition: "maintained", anchors: ["css-writing-modes-4"] },
  { key: "cssinline3", label: "CSS Inline Layout Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-inline-3/", edition: "maintained", anchors: ["css-inline-3"] },
  { key: "cssoverflow3", label: "CSS Overflow Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-overflow-3/", edition: "maintained", anchors: ["css-overflow-3"] },
  { key: "cssdisplay3", label: "CSS Display Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-display-3/", edition: "maintained", anchors: ["css-display-3"] },
  { key: "cssflexbox1", label: "CSS Flexible Box Layout Module Level 1", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-flexbox-1/", edition: "maintained", anchors: ["css-flexbox-1"] },
  { key: "cssgrid2", label: "CSS Grid Layout Module Level 2", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-grid-2/", edition: "maintained", anchors: ["css-grid-2"] },
  { key: "cssposition3", label: "CSS Positioned Layout Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-position-3/", edition: "maintained", anchors: ["css-position-3"] },
  /* LEVEL 4 IS A SEPARATE ROW FOR THE REASON THE css-images-3 ROW STATES: two levels of one module are two
     documents with two numberings, and the TOP LAYER is Level 4's — §3 "Top Layer" and §3.3 "Top Layer
     Manipulation" exist in css-position-4 and nowhere in css-position-3, whose §3 is a different heading
     entirely. Anchored only by the hyphenated levelled shortname, which is the one spelling that names a
     document; `CSS Positioned Layout §3` names neither and is normalized at the citation. */
  { key: "cssposition4", label: "CSS Positioned Layout Module Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-position-4/", edition: "maintained", anchors: ["css-position-4"] },
  { key: "cssbackgrounds3", label: "CSS Backgrounds and Borders Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-backgrounds-3/", edition: "maintained", anchors: ["css-backgrounds-3"] },
  { key: "csstransforms1", label: "CSS Transforms Module Level 1", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-transforms-1/", edition: "maintained", anchors: ["css-transforms-1"] },
  { key: "csscascade5", label: "CSS Cascading and Inheritance Level 5", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-cascade-5/", edition: "maintained", anchors: ["css-cascade-5"] },
  { key: "csssyntax3", label: "CSS Syntax Module Level 3", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-syntax-3/", edition: "maintained", anchors: ["css-syntax-3"] },
  { key: "cssfonts4", label: "CSS Fonts Module Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-fonts-4/", edition: "maintained", anchors: ["css-fonts-4"] },
  { key: "csscolor4", label: "CSS Color Module Level 4", kind: "bikeshed",
    base: "https://drafts.csswg.org/css-color-4/", edition: "maintained", anchors: ["css-color-4"] },
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
    base: "https://www.w3.org/TR/CSS2/", edition: "final", anchors: ["css-2-1"] },
  { key: "css22", label: "Cascading Style Sheets Level 2 Revision 2 (CSS 2.2)", kind: "w3c-chapters",
    base: "https://www.w3.org/TR/CSS22/", edition: "final", anchors: ["css-2-2"] },
  { key: "xml", label: "Extensible Markup Language (XML) 1.0 (Fifth Edition)", kind: "xmlspec",
    base: "https://www.w3.org/TR/xml/", edition: "final", anchors: ["xml"] },
];
const SPEC_BY_KEY = new Map(SPECS.map((s) => [s.key, s]));
const indexFileOf = (key) => join(INDEX_DIR, key + ".json");

/* THE EDITION DECLARATION IS ENFORCED AT THE ROW, because the failure it guards against is a row EDIT and not
 * a citation. Retargeting a standard's base is one keystroke and looks like a URL correction; what it actually
 * does is re-point every quotation of that standard at a different document. A missing declaration takes the
 * same arm as a wrong one — CLAUDE.md's rule for a value a consumer must not default — so an author adding a
 * standard has to ANSWER the question rather than inherit whichever answer the neighbouring row happened to
 * carry.
 * THE ONE MECHANICAL HALF OF THE RULE IS THE ONE THAT IS CHECKED: a /TR/ path is W3C's PUBLICATION space, so a
 * row claiming its editors still write there is claiming something the URL contradicts. The converse is NOT
 * asserted — a standard whose editors have stopped need not be a W3C publication at all, and asserting that
 * would be encoding today's three rows as though they were the rule. */
for (const s of SPECS) {
  if (s.edition !== "maintained" && s.edition !== "final")
    throw new Error(`${s.key}: no edition declared — a row states "maintained" (the document its editors write in: an Editor's Draft, or a living standard) or "final" (its editors have stopped, so the published Recommendation IS the standard). See the registry header for why this is declared and not inferred.`);
  if (s.edition === "maintained" && /^https?:\/\/(?:www\.)?w3\.org\/TR\//i.test(s.base))
    throw new Error(`${s.key}: declared "maintained" but its base ${s.base} is a /TR/ snapshot, which is a published copy rather than the document its editors write in. Either point it at the Editor's Draft, or declare it "final" because its editors have stopped.`);
}

/* ---- shared text normalization -------------------------------------------------------------------------- */

const ENTITIES = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " ", "#39": "'", "#x27": "'" };

function decodeEntities(s) {
  return s.replace(/&(#x?[0-9a-fA-F]+|[a-zA-Z]+);/g, (m, e) => {
    const k = e.toLowerCase();
    if (ENTITIES[k] !== undefined) return ENTITIES[k];
    /* A NUMERIC REFERENCE OUTSIDE UNICODE IS NOT A CHARACTER, AND `String.fromCodePoint` THROWS ON ONE — so
     * this line was a crash reachable from any prose this tool normalizes. It is not hypothetical text: XML
     * §4.1 "Character and Entity References" lets production [66] carry arbitrarily many digits, this tree
     * argues that in `xml_ref.c`'s own header, and a quotation carrying `&#99999999999999;` would have taken
     * the whole audit down with a RangeError naming nothing. The standard already answers it: HTML §13.2.5.84
     * "Numeric character reference end state" — a code point above 0x10FFFF is a
     * character-reference-outside-unicode-range parse error whose character reference code becomes U+FFFD.
     * `quoteTokens` then folds U+FFFD to a space like every other non-alphanumeric, so the replacement costs
     * the comparison nothing and the crash is gone at its root rather than guarded at one caller. */
    if (k[0] === "#") {
      const cp = parseInt(k[1] === "x" ? k.slice(2) : k.slice(1), k[1] === "x" ? 16 : 10);
      return Number.isFinite(cp) && cp >= 0 && cp <= 0x10ffff ? String.fromCodePoint(cp) : "\ufffd";
    }
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
 * boundary, and only the ones the reader numbered start a section.
 *
 * BUT AN UNNUMBERED SUB-HEADING IS NOT BACK MATTER, AND READING IT AS ONE THREW AWAY HALF OF A STANDARD.
 * The paragraph above is right about a heading that FOLLOWS the last numbered section and wrong about one that
 * stands INSIDE a numbered section — and a standard writes both. XML 1.0 titles every grammar production with
 * an unnumbered <h5> (`Document`, `Character Range`, thirty-five of them), and tc39's multipage renders
 * `Syntax` and each internal method as an unnumbered <h2> inside a numbered <h1> clause. With every heading a
 * boundary, a section's stored text STOPS at its first such sub-heading and everything after it is filed under
 * NO number and dropped from the corpus outright. MEASURED at the revision this landed, against the live
 * documents: the XML corpus held 54.6% of the standard's own words and ECMAScript's held 91.4%.
 * THE FAILURE IS THE ONE THIS CHECKER MUST NOT HAVE, which is why it is a boundary rule and not a tolerance:
 * fifteen quotations pasted correctly out of those documents were reported as words the standard does not
 * contain — the corpus manufacturing the finding, the same defect STEP_MARKER and stripMarkup each record one
 * layer down. It is also SILENT IN A SECOND DIRECTION, and that half is worse: the `foreign` probe names the
 * standard a quotation really belongs to, so a truncated corpus does not merely mis-report its OWN citations,
 * it suppresses the WRONG-STANDARD verdict on everyone else's. `xml_tag.h` quotes XML §3.1's Unique Att Spec
 * verbatim under a bare §3.1 the resolver placed on HTML, and the finding could not say so because XML's own
 * §3.1 had been cut to 127 characters.
 * THE RULE IS THE DOCUMENT'S OUTLINE, NOT A LIST OF SPELLINGS: a heading closes the open section when it is
 * numbered, or when its LEVEL is at or above the level of the numbered heading that opened it. A deeper
 * unnumbered heading is a sub-heading of the section it stands in and is passed through. Back matter is
 * unaffected — an Index or a References heading sits at or above the level of the sections it follows, which
 * is what made it a boundary in the first place — and a <footer> carries no level, so it always closes. */
function withBoundaries(body, marks) {
  const at = new Set(marks.map((m) => m.at));
  /* Every mark producer records the offset of the heading's own `<`, so the level is the third byte. A stop
   * that is not a heading (a <footer>) reads 0 and can never be deeper than an open section. */
  const level = (i) => { const c = body.charCodeAt(i + 2); return c >= 0x31 && c <= 0x36 ? c - 0x30 : 0; };
  const stops = [];
  const re = /<(?:h[1-6]|footer)\b/gi;
  for (let m; (m = re.exec(body)); ) if (!at.has(m.index)) stops.push({ at: m.index, no: null });
  const all = [...marks, ...stops].sort((a, b) => a.at - b.at);
  const out = [];
  let open = 0;                          /* level of the numbered heading currently open; 0 = none */
  for (const n of all) {
    if (n.no) { open = level(n.at); out.push(n); continue; }
    const l = level(n.at);
    if (!open || !l || l <= open) { out.push(n); open = 0; }
  }
  return out;
}

/* THE LIST STRUCTURE OF A SECTION, WHICH IS THE ONE THING tokenText DESTROYS AND THE ONLY THING THAT CAN
 * ANSWER A STEP NUMBER. `tokenText` flattens markup to words on purpose — that is what makes a quotation
 * comparable — and a step is not a word, it is a POSITION IN A LIST. So the same slice is read twice, once for
 * what it SAYS and once for how it is SHAPED, and the two readings share one boundary walk (see collectCorpus)
 * so a step can never be filed under a section its own text is not.
 *
 * WHAT A STEP IS, READ OFF THE MARKUP RATHER THAN RESTATED: a standard's steps are the TOP-LEVEL <li>s of one
 * <ol>, and CLAUDE.md's worked example is a step holding a nested list whose sub-items a flat count silently
 * promotes to peers — an algorithm of true length 36 counted as 39. That defect is impossible here by
 * construction rather than by care: an <li> belongs to the INNERMOST open list, so a nested list's items are
 * counted into that list and never into its parent. Only an <ol> is NUMBERED, so only an <ol> becomes a node;
 * a <ul> or a <dl> is a container the walk passes through, and an <ol> inside one attaches to the enclosing
 * <li> of the nearest ol ANCESTOR. That last rule is the deliberate over-approximation: a sub-list reached
 * through a <dl> switch renders restarting at 1 and a reader may well call its items `5.1`, so admitting them
 * can only ever make a path EXIST, and this whole channel accuses only where no path exists.
 *
 * SEVERAL SIBLING LISTS IN ONE STEP ARE KEPT AS SEVERAL, which is the shape CLAUDE.md calls worse than a wrong
 * number because every candidate reading confirms it. A step written as a catching list, then a
 * regardless-list, then a finally-list is ONE step holding THREE <ol>s, and a bare `10.2` names three different
 * things. Storing them merged would pick one; storing them as a list of lists lets the audit ask "does ANY
 * reading admit this path", which is the only question about them that has an answer. */
const LIST_TAG = /<\/?(ol|ul|dl|li|dd)\b[^>]*>/gi;

/* A COMMENT IS BLANKED IN PLACE RATHER THAN REMOVED, because two readings of one slice now have to agree on
 * where things ARE and not only on what they say. This walk records each item's POSITION, and those positions
 * are handed to the token mapping below as offsets into this same string — so a replacement that SHORTENS the
 * slice would slide every position after it by the length of a comment nobody can see, and the step whose
 * words the audit then reads would be the wrong one by however much markup the standard hides in its source.
 * Equal-length filler keeps one coordinate system for both readings, and whitespace is what the tokenizer
 * already turns these bytes into, so the two agree on the words as well as on the offsets. */
const blankRun = (s) => " ".repeat(s.length);
function listSource(slice) {
  return slice.replace(/<!--[\s\S]*?-->/g, blankRun)
    .replace(/<(script|style)\b[^>]*>[\s\S]*?<\/\1\s*>/gi, blankRun);
}

/* AN ITEM'S EXTENT IN TOKENS OF THE VERY TEXT THE QUOTATION CHECK READS, so the step corpus stores no prose of
 * its own: a step's words ARE the section's words, sliced, and two artifacts that cannot hold different bytes
 * cannot drift apart. The mapping is a COUNT and not a second tokenization, and it is EXACT for a reason the
 * markup guarantees rather than one this function is careful about: every cut here falls at a list tag;
 * `stripMarkup` turns a tag run that ENDS a fragment into a space whatever else the run holds; and `<li>`,
 * `<ol>`, `<ul>`, `<dl>` and `<dd>` are all in SPLIT_TAG, so a run containing one is a space in the joined
 * reading too. No token can straddle a cut in either reading, so the pieces' counts add up to the whole's.
 * `base` is where this slice's tokens begin in the section's stream — a section whose boundaries appear twice
 * has its text CONCATENATED, and an item of the second appearance is that far along. */
function toTokenSpans(clean, roots, base) {
  const cuts = new Set([0, clean.length]);
  const each = (l, f) => { f(l); for (const a of l.ch.values()) for (const x of a) each(x, f); };
  for (const r of roots) each(r, (l) => { for (const it of l.sp) { cuts.add(it.from); cuts.add(it.to); } });
  const ord = [...cuts].sort((a, b) => a - b);
  const at = new Map();
  let acc = base;
  for (let i = 0; i < ord.length; i++) {
    at.set(ord[i], acc);
    if (i + 1 < ord.length) { const t = tokenText(clean.slice(ord[i], ord[i + 1])); if (t) acc += t.split(" ").length; }
  }
  for (const r of roots) each(r, (l) => { for (const it of l.sp) { it.from = at.get(it.from); it.to = at.get(it.to); } });
}

function scanLists(slice, base) {
  const clean = listSource(slice);
  const roots = [];
  const st = [];                                   /* the open list elements, innermost last */
  LIST_TAG.lastIndex = 0;
  for (let m; (m = LIST_TAG.exec(clean)); ) {
    const name = m[1].toLowerCase(), close = m[0][1] === "/";
    if (name === "li" || name === "dd") {
      /* A WHATWG page omits `</li>`, so an item is COUNTED AT ITS OPEN TAG and closed by whatever follows.
         Counting closes instead would count nothing at all on the standard this tool cites most.
         AND AN ITEM'S NUMBER IS THE DOCUMENT'S, NOT THE COUNT — `start` and `value` are how a standard resumes
         a numbering across a note or an example, so a list of five items can legitimately end at step 20, and
         a checker that counted would report every one of those five as out of range. Reading the attributes
         costs two lines; discovering the hazard later costs a category of false accusations. */
      if (close || !st.length) continue;
      const f = st[st.length - 1];
      const v = name === "li" ? Number(attrOf(m[0], "value")) : NaN;
      f.cur = Number.isInteger(v) && v > 0 ? v : f.next;
      f.next = f.cur + 1;
      if (f.cur > f.n) f.n = f.cur;
      /* AN ITEM RUNS TO WHATEVER ENDS IT, WHICH IS NEVER ITS OWN CLOSE TAG on the standard this tool cites
         most — the same omitted `</li>` the count above is written around. So the item OPEN that follows it in
         the same list closes it, and a list close closes the last item of every list it pops; anything left
         open at the end of the slice runs to the end. Its extent therefore INCLUDES its sub-steps, which is
         what a reader means by "what step 11 says". */
      if (f.open) f.open.to = m.index;
      f.open = { i: f.cur, from: m.index + m[0].length, to: clean.length };
      f.sp.push(f.open);
      continue;
    }
    if (close) {
      for (let i = st.length - 1; i >= 0; i--) if (st[i].kind === name) {
        for (let j = st.length - 1; j >= i; j--) if (st[j].open) { st[j].open.to = m.index; st[j].open = null; }
        st.length = i; break;
      }
      continue;
    }
    const start = name === "ol" ? Number(attrOf(m[0], "start")) : NaN;
    const f = { kind: name, n: 0, cur: 0, next: Number.isInteger(start) && start > 0 ? start : 1, ch: new Map(),
                sp: [], open: null };
    /* ALL THREE LIST KINDS ARE NODES, AND A <ul> UNDER A STEP IS WHY. The corpus records what the markup IS;
       which of those a citation may be NAMING is a question about convention and belongs to the audit, not
       here — see how the audit flattens this tree. Recording only <ol> would decide that question in the
       corpus, where it cannot be argued or changed. Measured, and it is the case that forced the split:
       XHR's open() method step 11 is "Set variables associated with the object as follows:" over a BULLET
       list, and this tree cites the third bullet as `step 11.3` — the only spelling available, since the
       standard gives those items no number at all. A corpus holding <ol> alone reports five such sites as
       steps that cannot exist, and the repair it implies does not exist either. */
    const host = st.length ? st[st.length - 1] : null;
    if (host && host.cur) { const a = host.ch.get(host.cur) || []; a.push(f); host.ch.set(host.cur, a); }
    else roots.push(f);
    st.push(f);
  }
  toTokenSpans(clean, roots, base);
  return roots;
}

/* A LIST IS ITS COUNT, ITS SUB-LISTS, AND WHERE EACH OF ITS ITEMS STANDS IN THE SECTION'S WORDS — three facts
 * and a fixed shape, `[n, kids, spans]`, with `kids` empty where nothing under it is numbered. THE COUNT ALONE
 * USED TO BE THE WHOLE RECORD and the paragraph that stood here defended that: the audit's only question was
 * whether a path can EXIST, so an item with no numbered children was indistinguishable from its own index. The
 * audit now asks a second question — does the step the citation names SAY what the citation says it says — and
 * that question is about an item and not about a list, so an item has to be addressable. The positions are two
 * integers, never prose: the words stay in the text corpus, which is the only place any check reads words
 * from, so there is exactly one copy of the standard's sentences and this artifact indexes into it.
 * READERS TOLERATE THE OLD SHAPE ON PURPOSE — `listN` and `listKids` answer for a bare count, and the span
 * reader answers `null` — so a corpus written before this is UNCHECKED for the second question rather than a
 * crash or, worse, a silent zero. The `positions` stamp is what says which of those two a file is. */
function encList(l) {
  const ch = {};
  for (const [i, a] of [...l.ch].sort((x, y) => x - y)) ch[i] = a.map(encList);
  const sp = {};
  /* TWO ITEMS CAN CARRY ONE NUMBER — `value=` lets a standard resume a numbering, and nothing stops it landing
     on an index already used. The span is WIDENED to cover both rather than replaced, because every use of it
     is a question of the form "does this step mention X" and a wider step can only ever answer YES more
     often, which is the direction that withholds an accusation. */
  for (const it of l.sp) {
    const e = sp[it.i];
    sp[it.i] = e ? [Math.min(e[0], it.from), Math.max(e[1], it.to)] : [it.from, it.to];
  }
  return [l.n, ch, sp];
}

/* A SECTION CONTAINS ITS SUBSECTIONS, and this stores each section's OWN slice so that containment is a JOIN
 * the audit performs rather than a duplication the corpus carries. The slices of a section and its descendants
 * are contiguous in the document, so concatenating them in numeric order reproduces the original stream — no
 * artificial adjacency is created at a boundary, and no byte is stored twice.
 * THE TWO CORPORA ARE FILLED BY ONE WALK AND TRAVEL IN ONE OBJECT, so a reader cannot collect one and forget
 * the other, and the section boundaries a step is keyed by are BY CONSTRUCTION the ones its text is keyed by.
 * Two walks would be two copies of the one rule this file's own header says must not be copied. */
function collectCorpus(body, marks, into) {
  const b = withBoundaries(body, marks);
  for (let i = 0; i < b.length; i++) {
    if (!b[i].no) continue;
    const end = i + 1 < b.length ? b[i + 1].at : body.length;
    const slice = body.slice(b[i].at, end);
    const t = tokenText(slice);
    /* WHERE THIS SLICE'S WORDS BEGIN, read BEFORE the append that makes it untrue. A section whose boundaries
       appear twice has its text joined with a single space, so the second appearance's items are offset by the
       first's word count — and a step span is only ever read back through this same stream. */
    const prior = into.texts.get(b[i].no);
    if (t) into.texts.set(b[i].no, prior ? prior + " " + t : t);
    const enc = scanLists(slice, prior ? prior.split(" ").length : 0).filter((r) => r.n > 0).map(encList);
    if (enc.length) into.steps.set(b[i].no, (into.steps.get(b[i].no) || []).concat(enc));
  }
}

/* The corpora record the SAME two staleness facts the resolver index does, and the audit refuses one whose
 * pair disagrees with its index's — a corpus regenerated at a different edition from the section numbers it is
 * keyed by would answer questions about a document no single fetch ever saw. */
function writeCorpus(spec, sections, corpus, specUpdated) {
  const { texts, steps } = corpus;
  const stamp = { spec: spec.label, key: spec.key, base: spec.base, specUpdated: (specUpdated || "").trim(),
    fetched: new Date().toISOString().slice(0, 10) };
  mkdirSync(TEXT_DIR, { recursive: true });
  writeFileSync(textFileOf(spec.key), JSON.stringify(
    { ...stamp, sections: Object.fromEntries([...texts].sort((a, b) => cmpNo(a[0], b[0]))) }, null, 1) + "\n");
  let words = 0, missing = 0;
  for (const t of texts.values()) words += t.split(" ").length;
  for (const no of sections.keys()) if (!texts.has(no)) missing++;
  console.log(`  ${spec.key} text: ${texts.size} sections, ${words} words` +
    (missing ? `, ${missing} numbered section(s) with no text of their own` : ""));
  /* WRITTEN COMPACT, because this artifact is read by a machine and never by a person: it is counts and
     indices, so an indented spelling would triple a file whose whole content is punctuation. */
  mkdirSync(STEPS_DIR, { recursive: true });
  /* `positions` IS A STATEMENT ABOUT THIS FILE'S SHAPE, and it is written rather than sniffed for the reason
     the edition stamps beside it are: a reader that inferred the answer from the first list it happened to
     open would answer for the file from one sample, and the state it would get wrong — a corpus that carries
     the counts and not the offsets — is exactly the one whose second channel must report NOT CHECKED. */
  writeFileSync(stepsFileOf(spec.key), JSON.stringify(
    { ...stamp, positions: true, sections: Object.fromEntries([...steps].sort((a, b) => cmpNo(a[0], b[0]))) }) + "\n");
  let lists = 0, deepest = 0, items = 0, placed = 0;
  const depth = (l, d) => {
    deepest = Math.max(deepest, d);
    if (!Array.isArray(l)) return;
    items += l[0];
    placed += Object.keys(l[2] || {}).length;
    for (const a of Object.values(l[1])) for (const x of a) depth(x, d + 1);
  };
  for (const ls of steps.values()) for (const l of ls) { lists++; depth(l, 1); }
  /* THE TWO NUMBERS ARE PRINTED APART BECAUSE THEY CAN DISAGREE AND THE GAP IS THE ONE THING WORTH SEEING: an
     item's INDEX comes from the document's own `value=`, so a list can legitimately end at a number larger
     than the count of items it holds, and every such gap is a step the second channel cannot read. */
  console.log(`  ${spec.key} steps: ${steps.size} sections holding a list, ${lists} outermost lists, deepest nesting ${deepest}` +
    `, ${placed} item(s) positioned in the section's own words across a numbering that runs to ${items}`);
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

  const defs = new Map(), uses = new Map(), idToTerm = new Map(), bodies = [], corpus = { texts: new Map(), steps: new Map() };
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
  for (const b of bodies) { scanUses(b.body, b.marks, idToTerm, uses); collectCorpus(b.body, b.marks, corpus); }
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
  writeCorpus(spec, sections, corpus, updated);
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

  const defs = new Map(), uses = new Map(), idToTerm = new Map(), bodies = [], corpus = { texts: new Map(), steps: new Map() };
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
  for (const b of bodies) { scanUses(b.body, b.marks, idToTerm, uses); collectCorpus(b.body, b.marks, corpus); }
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
  writeCorpus(spec, sections, corpus, updated);
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
  const corpus = { texts: new Map(), steps: new Map() }; collectCorpus(body, marks, corpus);
  console.error(`  ${spec.key}: ${marks.length} headings on one page, ${ops.size} declared abstract operations`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated, ops));
  writeCorpus(spec, sections, corpus, updated);
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
  const corpus = { texts: new Map(), steps: new Map() }; collectCorpus(body, marks, corpus);
  console.error(`  ${spec.key}: ${marks.length} numbered headings on one page, ${ops.size} declared abstract operations`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated, ops));
  writeCorpus(spec, sections, corpus, updated);
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
  const corpus = { texts: new Map(), steps: new Map() }; collectCorpus(body, marks, corpus);
  console.error(`  ${spec.key}: ${marks.length} numbered headings, ${defs.size} definitions`);
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
  writeCorpus(spec, sections, corpus, updated);
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
  const corpus = { texts: new Map(), steps: new Map() };
  for (const b of bodies) { scanUses(b.body, b.marks, idToTerm, uses); collectCorpus(b.body, b.marks, corpus); }
  writeIndex(spec, finish(spec, sections, defs, uses, updated));
  writeCorpus(spec, sections, corpus, updated);
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
  "namespaces", "infra", "storage",
  "webcrypto", "svg", "mathml", "wasm", "uievents", "console", "performance",
  "workers", "websockets", "rfc", "unicode", "utf", "trusted", "clipboard",
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
  /* SUBRESOURCE INTEGRITY, AND ITS ABBREVIATION, WHICH IS THE ONE THIS TREE ACTUALLY WRITES. It arrived with
     CSP §6.7.2.4, whose step 4 hands a request's integrity metadata to SRI §3.3.2 "Parse metadata" and cites
     [SRI] for it — so a component reading that grammar cites a standard this audit does not index, and an
     unanchored `§3.3.2` in a CSP-dominant file is judged out of CSP, whose §3.3.2 does not exist. That is the
     `file system access` failure exactly: a real number, the wrong document. A foreign row REFUSES to decide
     rather than deciding wrongly, which is the honest state for a standard whose text this audit does not
     hold; an index row would be better and is a fetch away. */
  "sri", "subresource integrity",
  /* THE COMPATIBILITY STANDARD, AND IT IS HERE FOR THE FSA REASON WITH THE VOTE POINTING SOMEWHERE ELSE. This
     tree writes `CSS Compatibility §3.1` — the number WITH its standard, which is what CLAUDE.md asks for —
     and the tail regex ends on a word neither this list nor ANCHOR_TO_KEY held, so every one of those sites
     fell to its file's dominant anchor. `core/css/css_rule.c` and `test_forced.c` are HTML-dominant, so §3.1
     was judged out of HTML, whose §3.1 is "Documents"; the Compatibility Standard's §3.1 is "CSS At-rules"
     and holds the `-webkit-` at-rule alias table those sites are about. The cost of the wrong answer was
     measurable: `must be supported as aliases of the corresponding unprefixed at-rules` is that section's own
     sentence, pasted from its text, and stood in QUOTE-NOT-FOUND against HTML. Not indexed, so the sites are
     counted and never checked — which is the honest state for a standard whose text this audit does not
     hold. */
  "compatibility",
  /* THE TWO STANDARDS THAT EXTEND Fetch's `RequestInit` BY A PARTIAL DICTIONARY, and they are here for the
     reason the whole list exists: a partial's member is declared beside Fetch's own, in a Fetch-dominant
     file, so a citation of the OTHER standard's numbering sits surrounded by evidence for Fetch. Their
     numbers collide with Fetch's head-on: LNA §2.1 is "IP Address Space" where Fetch §2.1 is "URL", and
     LNA §3.1.2 is "Fetch API" where Fetch §3.1.2 is "`Set-Cookie` header" — two real numbers, neither the
     right document, which is the wrong-answer shape rather than a coverage gap. (Private State Token §6.1
     "Definitions" would land differently — Fetch §6 "data: URLs" has no subsections at all, so a vote makes
     it an UNKNOWN-SECTION — and it is listed for the same reason: the standard is the citation's claim, not
     the vote's guess.) Neither is indexed, so both are counted and never checked, which is the honest state
     for a WICG draft this tree cites for one enumeration and one dictionary shape.
     AND THE SECOND ENTRY IS THE TAIL AND NOT THE NAME, for the reason `cryptography api` sits beside `web
     cryptography api` above: anchorTokens' own regex reads AT MOST THREE trailing words, so the standard's
     full name — `Private State Token API`, four words — is a string this file can never be asked about, and
     listing it would be an entry with no reader. `state token api` is the longest tail the tokenizer can
     actually produce, and it is what the citation is recognized by. */
  "local network access", "state token api",
  /* RESIZE OBSERVER, and it is the `compatibility` case above with a WORSE vote behind it. This tree spells
     its citations `RESIZE OBSERVER §3.4.5`, and the tail word `observer` is on no list — so every one of them
     fell to its file's dominant anchor, and core/resize_observer/ is a component that legitimately cites HTML,
     Web IDL, CSS 2.2, css-sizing and css-writing-modes beside its own standard. The vote read them as DOM,
     whose §2.1 is "Introduction to \"DOM Events\"" and whose §4.3.1 is "Interface MutationObserver" — real
     numbers of a document that observer never names, and the second one plausible enough that a reader would
     have believed it. Not indexed, so the sites are counted and never checked, which is the honest state for
     a standard whose text this audit does not hold; an index row would be better and is a fetch away
     (drafts.csswg.org/resize-observer-1/, a bikeshed document). */
  "resize observer",
  /* GEOMETRY INTERFACES AND INTERSECTION OBSERVER — the two standards `resize observer` above left behind, and
     they are here because a correct citation of one was reported as a WRONG one. This tree spells them
     `GEOMETRY INTERFACES §3` and `Intersection Observer §2.3` — the number WITH its standard, which is what
     CLAUDE.md asks for — and neither tail word was on any list, so every one of them fell to its file's
     dominant anchor. The vote is not a coverage gap here, it is the wrong-answer shape: `core/platform.c`'s
     geometry rows sit two hundred lines from its HR-TIME row, so `§4 "The DOMRectList interface"` was judged
     out of High Resolution Time, whose §4 is "Time Origin"; the same sentence in `test_forced.c` was judged
     out of Web IDL, whose §4 is "Common definitions". Both are real numbers of documents Geometry never
     names, and because the citation STATES ITS TITLE the title check then reported the correct citation as a
     mismatch — an ACCUSATION manufactured by a vote, which is the one direction this audit must not fail in.
     A bare `§4` in the same place is the same mis-vote with nothing printed.
     THE TITLE IS WHAT MADE IT VISIBLE AND THE MIS-VOTE PREDATED IT: `geometry/dom_rect_list.h` has opened with
     `GEOMETRY INTERFACES §4` for as long as it has existed, silently filed under whatever its neighbours cite.
     Not indexed, so both are counted and never checked, which is the honest state for standards whose text
     this audit does not hold; index rows would be better and are a fetch away — Geometry Interfaces is a
     bikeshed document that MOVED, `drafts.fxtf.org/geometry/` now answering a redirect stub to
     `drafts.csswg.org/geometry/`, and Intersection Observer is `w3c.github.io/IntersectionObserver/`, 372 KB
     of rendered output with numbered `secno` headings and no `respecConfig`. */
  "geometry interfaces", "intersection observer",
  /* COOPERATIVE SCHEDULING OF BACKGROUND TASKS (requestIdleCallback), AND IT IS THE FIRST ENTRY ON THIS LIST
     WHOSE INDEX ROW IS NOT "A FETCH AWAY" — which is the sentence three neighbours above this one carry, so
     it is worth stating why it is false here rather than leaving the next reader to re-derive it.
     THE MAINTAINED DOCUMENT CANNOT BE READ BY ANY READER IN THIS FILE, and that is a fact about what the
     server sends rather than about this tool. `w3c.github.io/requestidlecallback/` answers 200 with 28 KB of
     UNRENDERED ReSpec SOURCE: it carries a `respecConfig`, no `<time class="dt-published">`, and ZERO
     numbered headings, because ReSpec numbers a document IN THE BROWSER at render time. The bytes therefore
     contain no section numbers at all. Contrast the `permissions` row, which is `kind: "respec"` and works:
     that URL serves 337 KB of RENDERED output with no `respecConfig`, a dt-published and 49 `<bdi
     class="secno">` headings. THE PRESENCE OF `respecConfig` IS THE TELL THAT A PAGE IS THE SOURCE, and it
     reads exactly like evidence that the respec reader is the right one — it is the opposite.
     AND NEITHER REMAINING ANSWER IS HONEST. `www.w3.org/TR/requestidlecallback/` IS rendered (17 numbered
     headings, dt-published), so it would parse — but it is a /TR/ path, which the edition assertion refuses
     for a "maintained" row and refuses correctly, and "final" would be a claim that the editors have STOPPED
     when the document is a W3C Working Draft with a live Editor's Draft. `labs.w3.org/spec-generator` answers
     503, and `index.html` is byte-identical to the directory, so there is no rendered form of the maintained
     document to fetch. Writing a reader that numbers the ReSpec SOURCE by document order would be this
     codebase restating ReSpec's own numbering algorithm — the second copy CLAUDE.md refuses, and the one
     that manufactures wrong answers rather than losing coverage.
     SO A FOREIGN ROW IS THE HONEST STATE, and it is `background tasks` because that is what the tokenizer can
     produce: every one of the eleven sites spells the standard `Cooperative Scheduling of Background Tasks
     §N` — `platform.c` in capitals, which classifyAnchor lowercases — five words, and anchorTokens reads at
     most three, so it offers `of background tasks` and then `background tasks`. The sites stand in
     `core/scheduling`, in `core/platform.c` and in `solver/engine.c`/`.h`, and an `other:` anchor is never judged,
     which is what stops a future file vote placing this standard's §4 and §5 on whichever document a
     scheduling file otherwise cites. They remain COUNTED AND NEVER CHECKED, and that is a silence this list
     prints rather than a clean bill. */
  "background tasks",
  /* THE SPELLING THIS TREE WRITES IS NOT ALWAYS THE SPELLING SOME LIST ALREADY HOLDS, AND A NEAR MISS IS A
     WRONG ANSWER RATHER THAN A COVERAGE GAP. Every name in this first group has a foreign row above it under
     ANOTHER spelling — `webcrypto`, `uievents`, `mimesniff`, `trusted`, `mediaqueries`/`media`, `referrer`,
     `unicode` — so the standard had ALREADY been decided to be one this audit does not index, and the only
     thing missing was the TAIL the citation actually ends on. anchorTokens reads at most three trailing words
     and classifyAnchor stops at the last of them, so `Web Cryptography §12` never reaches `webcrypto` and
     `MIME Sniffing §4.6` never reaches `mimesniff`: each came back with NO ANCHOR AT ALL and fell to its
     file's dominant vote, which is the `file system access` failure with the near answer sitting in the same
     table, two screens up, spelled by somebody who wrote the abbreviation and not the name.
     MEASURED, AND IN THE ACCUSATION DIRECTION RATHER THAN THE SILENT ONE, which is the half this audit must
     not fail in. `core/mime` votes `fetch` for every unanchored citation in it, and `mime_type.h` opens a
     declaration with `§4.6's MIME TYPE GROUPS` — which is MIME Sniffing's own, in the component that
     implements MIME Sniffing. That line was REPORTED as misattributed against `fetch §4.6` "HTTP-network-or-
     cache fetch", a real section of a document that file never names. In `embedder_policy.c` a bare `§3.4.1`
     inside a crash message is Reporting's "Generate report of type with data", and the same vote placed it at
     HTML.
     `web cryptography api` and `cryptography api` are ALREADY on this list and STAY: classifyAnchor takes the
     longest tail that classifies, so the three-word spelling is asked before the two-word one and the API
     form cannot be shadowed by the form without it. Both refuse either way, which is the point — this group
     adds no resolution, it removes a guess. */
  "web cryptography", "ui events", "trusted types", "tt",
  "media queries", "mq4", "referrer policy", "uax14",
  /* AND THE STANDARDS THIS TREE NAMES THAT NO LIST HELD UNDER ANY SPELLING. Each is a document with a real
     numbering that this audit holds no text for, cited with its name in front of the number exactly as CLAUDE.md
     asks — so every one of them was UNANCHORED and every one was decided by whichever standard its file
     otherwise cites. They are counted and never checked, which is the honest state; three of them are worth
     the sentence that says why an index row would NOT be the better answer today:
     `css scoping` — the document MOVED AND WAS RENAMED. `drafts.csswg.org/css-scoping-1/` answers 200 with
     315 bytes: a redirect stub titled "Moved to CSS Shadow Module Level 1" whose body sets `location.href` to
     `drafts.csswg.org/css-shadow-1/`. A `CSS Scoping §4.1` is therefore not the same section of the document
     that replaced it unless somebody checks, and indexing the successor under the predecessor's name would be
     this table asserting a renumbering nobody verified. All six sites write `CSS Scoping §4.1` with the title
     "Flattening the DOM into an Element Tree" and are the shadow-tree flat-tree walk in core/html.
     `positioned layout` — the citation names the LEVEL and the tokenizer throws it away. This tree writes
     `CSS Positioned Layout Level 4 §3 "Top Layer"`, and anchorTokens strips a trailing `Level N` before
     classifyAnchor is asked, so the tail is `CSS Positioned Layout` — a name that answers for cssposition3
     AND cssposition4, which are two documents with two numberings and both already indexed. An anchor here
     would be the `css-images` hazard the SPECS table refuses by name: one index answering two levels
     manufactures wrong answers rather than losing coverage. A foreign row refuses instead, and the ordered
     repair is to make the level SURVIVE the tail rather than to guess which document it belonged to.
     `css conditional` — CSS Conditional Rules is LEVELLED, and the population SPLITS on whether the citation
     says so. THE SENTENCE THAT STOOD HERE SAID THE SITES "write no level at all", AND IT WAS WRONG ABOUT MOST
     OF THEM, WHICH IS RECORDED RATHER THAN DELETED because a reader who re-derives a retired reason will
     re-introduce it: 35 sites write `CSS Conditional 5 §5.4`, one writes `CSS Conditional 3 §2.1`, and 28
     write the bare name. This entry ARMS the level join for the first two — they classify apart as
     `css-conditional-5` and `css-conditional-3`, on their own evidence — so what the row actually refuses is
     the 28 that carry NO level, which are the only ones with nothing in the citation to say which document
     numbers the section they mean. The levelled 35 are an index row away and that is the next diff for this
     standard, not a refusal. The claim was one command from being checked when it was written
     (`grep -c 'CSS Conditional [0-9]* §'`), and it was not.
     AND `typed om` IS THE ENTRY THAT PROVES A CAPITALISED-TOKEN SCAN CANNOT DERIVE THIS POPULATION, which is
     worth more than the entry. The list above was derived from the audit's own "capitalised tokens in front of
     a section sign that no list knows" line and from a grep for a name in front of one, and both read the
     LAST WORD before the
     section sign — so a standard this tree spells with a bare level, `CSS Typed OM 1 §6.4`, ends on a DIGIT and
     is invisible to both. Three sites were predicted for it. The measured answer is NINETY-THREE, because
     `typed om` on this list also arms anchorTokens' level JOIN — the same `(OTHER_SPECS.includes(bw[n-1]) &&
     …)` gate that turns `CSS Color 4 §16.2` into `css-color-4` — so the whole of core/css's Typed OM
     numbering stopped being decided by whichever standard those files otherwise cite. A derivation that reads
     the last token undercounts exactly the names the SPECS table's own `css-color-4` comment is about.
     MEASURED, both tool versions run back to back over ONE FROZEN SNAPSHOT at 391d3aa4 (submodule f2927c0a,
     56 entries) so the corpus is identical and only this list differs: file-vote guesses 9677 -> 8798, the
     judged population 34300 -> 34230, findings 1008 -> 1004. The judged count FALLS because that is what a
     refusal does — an explicit `other:` anchor beats a term coincidence, by design, since the citation's own
     claim about which standard it means outranks a phrase two documents share. An index row RAISES it; that is
     the next diff and not this one. */
  "parsing and serialization", "css conditional", "css scoping", "secure contexts", "reporting",
  "positioned layout", "css nesting", "css viewport", "typed om", "har",
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

/* WHICH FILES THIS AUDIT IS FOR, STATED ONCE. The two paragraphs inside `walk` argue each extension into this
 * set; the set itself has a second reader in `--since`, which built its own list as a git pathspec and was
 * therefore free to disagree — and did, for as long as `.js` and `.mjs` were audited by the walk and invisible
 * to the delta. A lane editing the trusted zone got `0 introduced` from a mode that had judged none of it.
 * A REGEXP AND A PATHSPEC ARE TWO ALPHABETS FOR ONE FACT, so the pathspec is DERIVED from this rather than
 * written beside it: add an extension here and both readers gain it in the same edit. */
const AUDITED_EXT = /\.(c|h|md|js|mjs)$/;
const AUDITED_GLOBS = ["*.c", "*.h", "*.md", "*.js", "*.mjs"];

function walk(dir, out = []) {
  /* THE CHECKOUT IS SHARED AND EDITED UNDER THIS WALK. An editor's temporary file appears between the readdir
   * and the stat and is gone before it, so a scan that trusts a name it just read crashes on another lane's
   * save. A vanished entry is not a finding and not an error — it is a file that was never in the tree
   * this run is measuring. */
  let names;
  try { names = readdirSync(dir); } catch { return out; }
  for (const e of names) {
    /* `out` and `.work` join the upstream skips for the upstream reason: both hold GENERATED bytes. Until
     * `.js` was admitted below neither could be reached, and the first thing the widening found was
     * `engine/host/out/qjs.js` — emscripten's glue, untracked, nobody's prose. A citation nobody wrote is not
     * a citation this tree can be held to, which is the same line `lexbor` and `qjs` are already drawn on. */
    if (e === "node_modules" || e === ".git" || e === "lexbor" || e === "qjs" || e === "out" || e === ".work" ||
        e.includes(".tmp.")) continue;
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
    /* AND `.js` IS PROSE THIS PROJECT WROTE, SO A NUMBER IN IT IS A NUMBER THIS AUDIT IS FOR. The extractor
     * below already reads JS — `//`, `/* *\/` and string literals are the same two places a C file can hold a
     * citation — so the only thing that kept the whole trusted zone out of every run was this character class.
     * CLAUDE.md §Architecture makes the bridge a first-class half of the product, and §Security puts the CORB
     * gate, the SOP/CORS decision and the destructive-path deny list in `safe-fetch.js` BY NAME: a wrong Fetch
     * number there governs a security decision and misleads exactly as `core/fetch/fetch.c` would. */
    else if (AUDITED_EXT.test(e)) out.push(p);
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
  /* A TEMPLATE LITERAL IS A STRING LITERAL, AND IT IS THE ONE KIND OF PROSE THIS SCANNER COULD NOT SEE. The
   * branches below are the two places a C file can hold a citation; JavaScript has a THIRD, which this walk
   * started reading the day `.js` and `.mjs` were admitted and which this scanner was never told about.
   * WHAT THAT COST WAS BOTH FAILURES AT ONCE, WHICH IS WHY IT IS ONE BRANCH AND NOT A PATCH ON EITHER.
   *   A template holding no `/*` was READ BY NOTHING: not counted short, not counted unresolved, not counted
   *     anywhere — the silent zero `quotedRuns` names one screen down, in a file kind the walk believes it is
   *     auditing. Every generator in `engine/` writes its C output through one, so the prose that TEACHES the
   *     conventions of the emitted header sat outside every channel here.
   *   A template holding one was read as a COMMENT, because `/*` inside it opens the comment branch and `*\/`
   *     ends it — an extent that starts and stops at two marks the author wrote as OUTPUT, and swallows the
   *     program text between two literals on the way. MEASURED, and it is the one false positive the
   *     agreement channel carried at the revision this landed: `encgen.mjs` splits an Encoding §4.2 sentence
   *     across a concatenation, and the joint between the two chunks reached the quotation check INSIDE the quoted
   *     text, so a correct quotation of a real standard could not match it at any authoring — a finding no
   *     edit at the site could clear, charged against prose that was right.
   * THE FIX FOR THE SECOND IS NOT A JOINT RULE HERE. Once a template is a LITERAL, its chunks are adjacent
   * literals of one expression, and what joins two of those is `LITERAL_JOINT`'s to say — one constant, read
   * by every walk of that adjacency. This branch owns only what prose IS. */
  const js = !!path && /\.(js|mjs)$/i.test(path);
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
    } else if (js && c === "`") {
      i = scanTemplate(src, i, spans);
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c) { if (src[j] === "\\") j++; if (src[j] === "\n") break; j++; }
      spans.push([i + 1, Math.min(j, n), "s"]); i = j + 1;
    } else i++;
  }
  /* EVERY READER OF A SPAN LIST BISECTS IT, so a scanner that emits one out of order or overlapping does not
   * report a wrong span — it makes `spanAt` and `inSpans` answer about a DIFFERENT span, quietly, for the rest
   * of the run. The template scanner is the first one here that recurses, and a recursion is exactly where an
   * offset gets emitted twice; this states the property those readers already assume rather than trusting the
   * scanner to have it. */
  for (let i = 1; i < spans.length; i++)
    if (spans[i][0] < spans[i - 1][1])
      throw new Error(`${path}: prose spans out of order at ${spans[i - 1]} then ${spans[i]} — every reader of ` +
                      `this list bisects it, so an overlap answers about the wrong span rather than failing`);
  return spans;
}

/* ONE TEMPLATE LITERAL, SCANNED AND NOT MATCHED, for the reason `quotedRuns` gives about the other marks: a
 * pattern with a minimum length silently re-pairs every delimiter after a short one, and a template's
 * delimiter cannot also be an apostrophe, so a scan that walks it cannot mis-pair. Returns the index just past
 * the closing backtick; pushes one span per CHUNK, because a substitution is program text and not prose.
 * THE CHUNK SPLIT IS THE WHOLE POINT OF SCANNING RATHER THAN SLICING. `${e.name}` and `${JSON.stringify(x)}`
 * are the program, and this tree writes both inside sentences: read as prose they hand the citation scan a
 * dotted number that is a property access and the quotation check a run of identifiers no standard has. Two
 * chunks around a substitution are two spans, so nothing merges them and neither half claims the other's
 * words — which is also the honest reading of a quotation with a computed value spliced into it: it is not a
 * verbatim quotation of anything. */
function scanTemplate(src, i, spans) {
  const n = src.length;
  let j = i + 1, start = j;
  while (j < n && src[j] !== "`") {
    if (src[j] === "\\") { j += 2; continue; }
    if (src[j] === "$" && src[j + 1] === "{") {
      if (j > start) spans.push([start, j, "s"]);
      j = scanSubst(src, j + 2, spans);
      start = j;
      continue;
    }
    j++;
  }
  if (Math.min(j, n) > start) spans.push([start, Math.min(j, n), "s"]);
  return Math.min(j + 1, n);
}

/* THE PROGRAM TEXT INSIDE A SUBSTITUTION, walked only far enough to find where it ENDS. A `}` inside a string
 * or a nested template is not the closing brace, so the walk skips those the same way the top-level scan does
 * — and a nested template is itself prose, so its chunks are pushed here rather than discarded: this tree
 * composes a message out of nested templates and the sentence in the inner one is a sentence somebody wrote.
 * The pushes stay ASCENDING because each is emitted at the offset the walk is standing on, which is what the
 * ordering assertion above is written over. */
function scanSubst(src, i, spans) {
  const n = src.length;
  let depth = 1, j = i;
  while (j < n && depth > 0) {
    const c = src[j];
    if (c === "{") { depth++; j++; }
    else if (c === "}") { depth--; j++; }
    else if (c === "`") j = scanTemplate(src, j, spans);
    else if (c === '"' || c === "'") {
      let k = j + 1;
      while (k < n && src[k] !== c) { if (src[k] === "\\") k++; if (src[k] === "\n") break; k++; }
      if (Math.min(k, n) > j + 1) spans.push([j + 1, Math.min(k, n), "s"]);
      j = Math.min(k + 1, n);
    } else j++;
  }
  return j;
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

/* THE JOINT BETWEEN TWO ADJACENT LITERALS OF ONE MESSAGE, WHICH IS ONE RULE AND HAS ONE OWNER. The paragraph
 * above is written about `proseUnit`'s three readers and it understates the population: `quotedSrcRuns` walks
 * the SAME adjacency to build a run, so the rule had TWO copies and a paraphrase rather than one copy and two
 * copies — and the copy that drifts is the one nobody runs against reality. It is a constant here so that a
 * fifth reader cannot spell it a fourth way.
 * WHAT A JOINT IS: the closing delimiter, the concatenation the language spells between them if it spells one,
 * and the opening delimiter. C joins adjacent literals with NOTHING, so this read `"` and whitespace and was a
 * rule about one language inside a scan that walks two. JavaScript writes the same adjacency `"…" + "…"` and
 * two backticked chunks joined by one `+`, and a quotation split across one arrived with the joint standing
 * INSIDE it — measured on `encgen.mjs`, where a correct quotation of Encoding §4.2 could not be matched at ANY
 * authoring, because nothing an author writes at the site removes a joint the language requires. That is a
 * finding no edit can clear, charged against prose that is right, which this file rates worse than a miss.
 * AT MOST ONE OPERATOR, and that is a derivation and not a threshold: one closing delimiter, one
 * concatenation, one opening delimiter. Two operators in a row is an expression rather than a joint, and
 * admitting it would let a unit reach across program text this scan never read. */
const LITERAL_JOINT = /^["'`\s]*\+?["'`\s]*$/;

/* THE PROSE UNIT A CITATION STANDS IN, AND THE ONE PLACE THIS FILE STATES WHERE A MESSAGE BEGINS AND ENDS.
 * The unit is the span the citation sits in, WIDENED in both directions to the maximal run of adjacent string
 * literals whenever it sits in one — the paragraph above says why a run and not a span, and this is that
 * sentence made executable. Three readers need it and they must not disagree: `governedProse` walks the
 * adjacency forward, `precedingProse` walks it backward, and the OK-NEARBY channels ask which citations SHARE
 * a unit. The third reader is the one that got it wrong. It keyed "shares a prose unit with this one" on the
 * SPAN INDEX while the other two walked the run, so a citation in a DIFFERENT LITERAL OF THE SAME `DFAIL` was
 * invisible to it and the quotation that citation explains was accused of belonging to a standard the very
 * same message cites correctly, three lines up. That is this auditor committing its own subject in the DENY
 * direction it has no licence to err in: a wrong finding stands at a site nobody can repair and teaches the
 * reader to skim the category it sits in. The rule now has ONE owner rather than two copies and a paraphrase,
 * which is the only arrangement in which a fourth reader cannot re-introduce the same gap. */
function proseUnit(src, spans, at) {
  const i = spanIdxAt(spans, at);
  if (i < 0) return null;
  let lo = i, hi = i;
  if (spans[i][2] === "s") {
    while (hi + 1 < spans.length && spans[hi + 1][2] === "s" &&
           LITERAL_JOINT.test(src.slice(spans[hi][1], spans[hi + 1][0]))) hi++;
    while (lo > 0 && spans[lo - 1][2] === "s" &&
           LITERAL_JOINT.test(src.slice(spans[lo - 1][1], spans[lo][0]))) lo--;
  }
  return { i, lo, hi, kind: spans[i][2] };
}

/* THE UNIT'S IDENTITY IS ITS FIRST SPAN, and every reader that groups citations by unit keys on this — never
 * on `spanIdxAt`, which names a LITERAL and not a message. */
function proseUnitKey(src, spans, at) {
  const u = proseUnit(src, spans, at);
  return u ? u.lo : -1;
}

/* A UNIT'S OWN TEXT, TAKEN FROM ITS SPANS AND NEVER FROM A SLICE OF THE FILE. Both readers below used to cut
 * the source between two offsets and then DELETE the joint with a pattern — `(?<!\\)"\s*"` — which is a THIRD
 * spelling of `LITERAL_JOINT` written in the wrong alphabet: it says what a joint looks like as TEXT while the
 * constant says what it looks like as an OFFSET RANGE, and the two must agree for a message's words to come
 * out as the author wrote them. They did not, the moment a second language's joint existed. This reads the
 * spans `proseUnit` already returned, so the joint is not deleted at all — it is never picked up, because the
 * program's own characters lie BETWEEN the spans and a walk over spans cannot reach them.
 * JOINED WITH NOTHING, which is the C semantics being preserved rather than a choice: adjacent literals
 * concatenate with no separator, so a word split across two of them is one word, and inserting a space here
 * would split it in every token stream downstream. */
function unitProse(src, spans, u, from, to) {
  let out = "";
  for (let k = u.lo; k <= u.hi; k++) {
    const a = Math.max(spans[k][0], from), b = Math.min(spans[k][1], to);
    if (b > a) out += src.slice(a, b);
  }
  return u.kind === "s" ? unescapeC(out) : out.replace(/\n\s*\*?\s*/g, " ");
}

/* The prose a citation governs, flattened into ONE line the way a reader reads it. A comment's line wrap and
 * its `*` gutter are not part of any sentence; a literal run's joints are not either, and the escape a C
 * literal spells `\"` is the quotation mark the author wrote. Unescaping is what makes a quotation inside a
 * crash message the same text as the same quotation inside a comment. */
function governedProse(src, spans, at, len, stopAt) {
  const u = proseUnit(src, spans, at);
  if (!u) return "";
  let end = spans[u.hi][1];
  if (stopAt !== null && stopAt < end) end = stopAt;
  return unitProse(src, spans, u, at + len, Math.max(at + len, end));
}

/* The same prose on the other side of the citation — everything from the start of the unit up to the number.
 * `governedProse` reads forward because a claim is normally made AFTER a number; a DISCLAIMER is made BEFORE
 * one, and nothing until now could see it. */
function precedingProse(src, spans, at) {
  const u = proseUnit(src, spans, at);
  if (!u) return "";
  return unitProse(src, spans, u, spans[u.lo][0], at);
}

/* ---- the STEP axis: a number no section index can see ---------------------------------------------------- */

/* WHY THIS EXISTS AND WHAT IT CAN AND CANNOT SAY. Every other check here judges a SECTION — does the standard
 * have it, does the algorithm live there, does the title match, are these its words. A STEP is none of those:
 * it is a position in a list, and the resolver index holds section headings while the text corpus holds words
 * with the markup flattened out of them, so a wrong step number is invisible to both BY CONSTRUCTION. CLAUDE.md
 * names the shape and the cost — one file carried 110 citations, produced zero findings, and had a whole cluster
 * off by one — and three separate clusters were found BY HAND in one session, each because somebody happened to
 * open the standard.
 *
 * WHAT THIS CHECK ACCUSES IS ONLY THE NUMBER THAT CANNOT EXIST, and that limit is the whole of its precision
 * argument. What it can state is that 6.8 is not a step of anything in that section — which is precisely the
 * shape the drift produces once it runs off the end, and it is what the one hand-found cluster of the session
 * that built it would have tripped. A step that exists and means something ELSE is a DIFFERENT question and it
 * has its own check below, over the item positions the corpus carries: see STEP_CLAIM. Where neither the number
 * nor the words falsify anything — 6.4 against 6.5 when the claim carries none of the standard's own vocabulary
 * — only a reader comparing MEANING can, and both checks say so rather than guessing.
 *
 * EVERY REFUSAL IS AN OVER-APPROXIMATION, DELIBERATELY, because the accusation is "no reading admits this":
 *   — a section is joined with its SUBSECTIONS, so an algorithm one heading down still answers;
 *   — every numbered list in that region is a candidate, not only the algorithm the citation names, because
 *     nothing at the site says WHICH algorithm and guessing one would be the invented-citation defect this file
 *     already refuses twice;
 *   — several sibling lists under one step are all admitted, which is CLAUDE.md's ambiguous sub-number: every
 *     candidate reading confirms it, so no reading can be accused;
 *   — a list reached through a <ul> or a <dl> is admitted as sub-steps of the enclosing <li>.
 * Each widens what EXISTS, so each can only remove a finding. */
/* A COMPONENT OF A STEP PATH IS NOT ALWAYS A NUMBER, AND THIS PATTERN SAID IT WAS. Digits and dots only means
 * `step 2.i` MATCHES ON ITS NUMERIC PREFIX and is then checked as `step 2` — so a lettered sub-step citation
 * was validated against the TOP-LEVEL count and reported clean whatever it named. Measured on the compiled
 * pattern: `step 2.i` captured `2`, `step 2.z` captured `2`, `step 13.a.iv` captured `13`. `step 2.z` of a
 * section whose step 2 holds thirteen sub-items passed, and so did every letter after `m`. That is the
 * SILENTLY-PERMISSIVE direction CLAUDE.md rates worse than no checker at all: the tool did not report that it
 * could not read the number, it reported that the citation was fine.
 *
 * WHICH GLYPH A STANDARD PRINTS IS THE STANDARD’S DECISION AND NOTHING HERE HOLDS A TABLE OF IT. ECMAScript’s
 * emu-alg stylesheet cycles decimal / lower-alpha / lower-roman by depth — and SHIFTS that cycle with its
 * `nested-once`…`nested-lots` classes — so §27.5.1.3’s step 2 prints its thirteen sub-items `a`…`m`; the
 * WHATWG standards override no list-style at all, so every depth prints decimal and their sub-steps are cited
 * `2.3.2`; a CSSWG draft writes `type="i"` on the one list it wants roman. The step corpus records ITEM
 * POSITIONS and never markers, so nothing committed here can say which convention a given section renders
 * under — and a per-standard table of it would be the hand-kept second copy §AN-AUDITOR-DERIVES-THE-RULE-IT-
 * CHECKS-FROM-THE-CODE-THAT-OWNS-IT refuses, derived from one spec and applied to all.
 *
 * SO THE GLYPH IS READ AND THE CONVENTION IS NOT. A component names a POSITION, and a written component
 * admits every position its own spelling can denote: a digit run denotes one, `z` denotes the 26th under the
 * alphabetic reading and nothing under the roman one, and `i` denotes the 9th OR the 1st because it is a
 * letter and a numeral both. Every admitted reading is carried and a path fails only where NO reading of it
 * reaches an item — the same over-approximation the paragraph above builds the rest of this check out of, so
 * it too can only remove a finding. The two populations that buys are COUNTED AND PRINTED rather than passed
 * off as verified: a path whose readings DISAGREE is NOT DECIDED (`letterSplit`), and a component whose
 * spelling denotes no position under either reading is UNCHECKED (`unreadable`).
 *
 * THE LEADING COMPONENT STAYS NUMERIC, which is a decision about ENGLISH and not about any standard: `steps?
 * \s+` followed by a bare letter matches `step a`, `step or`, `step is`, and the population that would enter
 * this channel is prose rather than citations. Every citation form this tree writes numbers its top level.
 * A LETTER ARM ALSO MAY NOT BE FOLLOWED BY A WORD CHARACTER, or the alternation truncates exactly as the old
 * pattern did: without it `xiii` matches the two-letter arm as `xi` and leaves `ii` behind — the same silent
 * prefix read, one alternative down. The numeric arm carries no such lookahead, so a numeric path is matched
 * byte-identically to before. */
const STEP_COMP = "(?:[1-9][0-9]*|(?:[ivxlcdm]{3,8}|[a-z]{1,2})(?![0-9a-z]))";
const STEP_NO = `[1-9][0-9]*(?:\\.${STEP_COMP})*`;
/* A STEP IS WRITTEN `step N` AND A SECTION IS WRITTEN `§N` — the convention CLAUDE.md fixes and the one the
 * bare-number reader already leans on from the other side, where a number the word `step` introduces is
 * excluded from being read as a section. This reads the same population that exclusion creates. The trailing
 * group takes a JOINED RUN (`steps 6.1-6.7`, `steps 3 and 4`, `steps 1, 2, 5`) because each operand is a step
 * the author is claiming exists, which is the one place this differs from the section reader: there the right
 * operand of a range is undecidable between a section and a step, and here the `step` lead-in has already
 * decided it. */
const STEP_REF = new RegExp(
  `(?:^|[^\\w])steps?\\s+(${STEP_NO})((?:\\s*(?:,|&|and|or|to|through|[-‐‑‒–—―])\\s*${STEP_NO})*)`, "gi");
/* CASE-INSENSITIVE LIKE THE READER IT RE-SCANS: STEP_REF matches under `i`, so a joined run it captured may
 * hold `2.J` and a case-sensitive re-scan of that run would find the `2` and drop the component. */
const STEP_MORE = new RegExp(STEP_NO, "gi");
/* See stepRefs: the run operand that is a SECTION because the word `step` follows it. */
const SEC_TRAIL = /^(?:['’]s)?\s*steps?\b/i;

/* THE OWNER OF A STEP NUMBER IS A FACT A SITE CAN STATE, AND NEAREST-PRECEDING IS WHAT YOU FALL BACK ON WHEN
 * IT DOES NOT. The fallback is wrong in exactly one shape and it produced BOTH of this channel's accusations at
 * once: a comment credits a section for a TERM it is borrowing — "get the parent says so", "the template
 * cloning steps return early" — and a step number written later in the same sentence lands on that credit
 * instead of on the algorithm the comment is actually walking. Two wrong answers were cancelling at one of
 * them, so a false accusation was suppressing a false step claim; the two were separated by the lane that
 * measured this, and the count went up rather than being kept flat.
 *
 * WHAT THIS MATCHES IS A CITATION MAKING A STEP CLAIM OF ITS OWN — the number inside the citation's own noun
 * phrase, across the spellings this tree writes: `§N step K`, `§N's step K`, `§N "Title" step K`, `§N (note)
 * step K`, `§N — step K`. That is a DECLARATION: the author has said, in one breath, which list K indexes. A
 * step number three clauses later has said no such thing, and the difference between those two is the whole of
 * what this can read off a site.
 *
 * MEASURED, AND EVERY CHEAPER RULE WAS REFUSED ON THE NUMBERS rather than on taste — see the header of the
 * declaration map in PASS 5 for what each one cost. */
const ADJ_STEP = new RegExp(
  `^(?:['’]s|s['’])?\\s*(?:"[^"]{0,80}"\\s*)?(?:\\([^)]{0,80}\\)\\s*)?[-,:–—]?\\s*(?:the\\s+)?steps?\\s+(${STEP_NO})`, "i");

/* THE OWNER WRITTEN WITH NO §, WHICH IS THE ONE SHAPE ADJ_STEP CANNOT REACH. ADJ_STEP reads the step out of a
 * CITATION’s own noun phrase and so is anchored at a citation; this reads the same noun phrase from the other
 * end — a dotted number immediately before `step K`, whether or not the bare-number reader admitted it. The
 * spellings are ADJ_STEP’s, so the two agree about what a noun phrase is; only the anchor differs. */
const OWN_LEAD = new RegExp(
  `(?<![\\w.§])([1-9][0-9]?(?:\\.[1-9][0-9]*){1,4})(?:['’]s)?\\s*(?:"[^"]{0,80}"\\s*)?` +
  `(?:\\([^)]{0,80}\\)\\s*)?[-,:–—]?\\s*(?:the\\s+)?steps?\\s+$`, "i");

/* THE SECOND QUESTION ABOUT A STEP, AND THE ONE THE PARAGRAPH ABOVE SAYS IT CANNOT ANSWER: not whether the
 * number can exist, but whether the step it names SAYS what the citation says it says. A number that exists
 * and is in range can still be the wrong step for the claim made about it — a comment reading `step 16's
 * string` where the string arm is step 15 and step 16 is a different clause of the same ladder. Nothing here
 * could see that: the existence channel confirms the number, the quotation channel is not looking at a
 * quotation, and the citation is well-formed. It is the drift CLAUDE.md describes caught one index BEFORE it
 * runs off the end, which is where all of it that stays in range lives.
 *
 * WHAT COUNTS AS A CLAIM IS THE WHOLE OF THE DESIGN, AND IT IS ONE GRAMMATICAL FORM: the POSSESSIVE. `step
 * N's X` is an author saying, in one breath, that step N HAS an X — a statement about that step's content,
 * made about that exact list position, with nothing between the number and the claim for an attribution rule
 * to get wrong. Every looser reading was refused for a reason this file has already paid for once: a step
 * number and a noun three clauses apart are not a claim about each other, and a checker that reads them as one
 * manufactures findings out of ordinary prose. This form is also common enough to be worth checking — the
 * tree writes it thousands of times — so the narrow rule costs recall it can afford.
 *
 * THE ORACLE IS THE STANDARD'S OWN WORDS, sliced by the corpus's item positions, exactly as the quotation
 * channel compares against the standard's own sentences. Nothing here holds an opinion about what any step
 * says, and no table anywhere records it; the audit asks the corpus, and the corpus is the document.
 *
 * WHERE THE PHRASE ENDS IS NOT RECOVERABLE, SO ONLY TWO SHAPES ARE READ — the ones in which the author has
 * bounded it themselves. `step 5.4's max is the member's own maxarg` runs straight into a verb; `step 3's
 * second constraint` puts a modifier where the claim's noun should be; `step 1's other arm` and `step 4's two
 * spellings` do the same. A reader that takes the run to the next comma is reading a SENTENCE and calling it
 * a claim, and that is measurable rather than arguable: it is where every false accusation in this channel's
 * first measurement came from — a step charged with not saying `a`, `was` or `two` because some sibling
 * happened to. What is left is:
 *   BARE — `step N's W` and then a full stop, a comma, a dash, the end of the prose. One word, no modifier,
 *     nothing to mis-attach: the author has named a thing and stopped.
 *   TERM — the phrase's leading run IS a term the standard DEFINES, so its extent is the STANDARD'S statement
 *     and not this reader's guess, and it is checked as a whole phrase rather than as a word. `step 8's
 *     removing steps` is a claim about a named algorithm; two words matching in one item is evidence a single
 *     word can never be.
 * Everything else is COUNTED and never judged. The recall that costs is real and is the right price. */
const STEP_CLAIM = /^['’]s\s+([^.,;:()"\n—–]{1,48})/;

/* HOW MANY STEPS MAY HOLD A WORD BEFORE NAMING THEM STOPS BEING A REPAIR. The accusation this channel makes is
 * not "the step does not say X" on its own — that is true of most steps and most words, and a reader cannot
 * act on it. It is "the step does not say X and THESE do", so the tool must be able to NAME where the word is,
 * and a word standing in a dozen steps names nothing. */
const NAME_CAP = 3;

/* HOW FAR A DEFINED TERM MAY RUN. The longest key in any of these indexes is a handful of words; the bound is
 * on the SEARCH and not on the vocabulary, so a longer term simply is not matched from its head. */
const TERM_WORDS = 8;

/* AND A BARE WORD MUST BE ONE THE STANDARD USES AS VOCABULARY, WHICH IS A QUESTION THE STANDARD ANSWERS ITSELF.
 * This is the gate that decides whether this channel is worth having, and the first version of it did not
 * exist: rarity WITHIN a list was taken for informativeness, and it is not — a short ladder in which `a`
 * happens to fall in two items makes `a` rare, and the tool then charged a step with not saying `a` and named
 * a rival on that basis. Eighty-eight findings, and reading them is what produced the two gates here.
 *   — THE VOCABULARY is the dfn and operation index this file already builds and every other check already
 *     trusts. A word inside some term the cited standard DEFINES is a word that standard uses to name things;
 *     `second`, `three`, `rounding` and `answer` are not, and neither is anything this tree invented. It is
 *     derived from the standard's own markup, so it needs no list to be kept and cannot drift from what the
 *     document says.
 *   — THE FREQUENCY is what the vocabulary alone cannot see. A standard's terms are noun PHRASES, so `a`, `to`
 *     and `is` are inside dozens of them and pass the first gate; what they cannot pass is standing in most of
 *     the standard's sections. A word that common carries no information about WHICH step, which is the only
 *     question here.
 * Both err toward silence, and the population they refuse is COUNTED so the silence is visible. */
const DF_CAP = 0.4;

/* A DECLARATION IS ONLY WORTH JOINING ON WHEN THE PATH IDENTIFIES A LIST POSITION, AND A BARE ONE-COMPONENT
 * PATH DOES NOT. Measured on 6581 checked step references: joining on any declared path silences 18 of the 69
 * NOT-IN-THIS-SECTION entries, and almost every one of those goes through a bare `step 5`/`step 6`/`step 8`
 * that some unrelated algorithm in the same file happens to declare — a file-header citation confirming a step
 * eight hundred lines away, which is the container-agrees-with-anything shape this tool has already refused
 * once in its term channel. Requiring two components drops that to ONE, and reading it confirms it: a comment
 * about enqueueing a custom element reaction says "the one step 14.4 COLLECTED into this element's
 * definition", and the same file writes that exact path beside the section that defines it, eight times.
 * A HEADING IS EXEMPT because a heading is not a passing remark — see headingDecl. */
const DECLARED_DEPTH = 2;

/* WHAT A MARKDOWN HEADING DECLARES, AND WHY IT IS NOT THE SAME KIND OF EVIDENCE AS A SENTENCE. A paragraph
 * cites in order to borrow; a heading names what everything under it is about, so a heading that writes a
 * section, a title and a step in one line has stated the owner of every step number in its block. This tree's
 * design notes are written that way and the numbers come from them. The scope is the BLOCK — from the heading
 * to the next heading at the same or a shallower level — because one document holds many algorithms and a
 * file-wide reading of one heading would claim every step in it. */
function mdHeadingLevel(text) {
  const m = /^(#{1,6})\s/.exec(text.replace(/^\s+/, ""));
  return m ? m[1].length : 0;
}

/* A WRITTEN COMPONENT’S POSITIONS — see STEP_NO for why there can be more than one and why no table decides
 * between them. The roman reading is CANONICAL-ONLY: `iiii` is not four in any renderer’s output, so admitting
 * it would widen the candidate set with a position nothing prints, which is the one direction this widening
 * may not go. A component that denotes NOTHING under either reading returns the empty set, and the pass
 * refuses to judge it rather than reading past it. */
const ROMAN_OK = /^m{0,3}(?:cm|cd|d?c{0,3})(?:xc|xl|l?x{0,3})(?:ix|iv|v?i{0,3})$/;
const ROMAN_V = { i: 1, v: 5, x: 10, l: 50, c: 100, d: 500, m: 1000 };
function romanValue(s) {
  if (!s || !ROMAN_OK.test(s)) return 0;
  let v = 0;
  for (let i = 0; i < s.length; i++) {
    const a = ROMAN_V[s[i]], b = ROMAN_V[s[i + 1]];
    v += b > a ? -a : a;
  }
  return v;
}
/* `a`=1 … `z`=26, `aa`=27 — the bijective base-26 a `lower-alpha` list emits past its alphabet. */
function alphaValue(s) {
  let v = 0;
  for (let i = 0; i < s.length; i++) v = v * 26 + (s.charCodeAt(i) - 96);
  return v;
}
function stepPositions(part) {
  if (/^[0-9]+$/.test(part)) return [Number(part)];
  const out = [];
  if (/^[a-z]{1,2}$/.test(part)) out.push(alphaValue(part));
  const r = romanValue(part);
  if (r && !out.includes(r)) out.push(r);
  return out;
}

/* A REFERENCE CARRIES BOTH ITS SPELLING AND ITS READINGS, and the two are used for different questions. `parts`
 * is what the author WROTE, so it is what a declaration elsewhere in the file is matched against and what a
 * finding names back to them; `path` is the positions each component may denote, which is the only thing the
 * corpus can be asked about. Writing one and deriving the other at each use would be the second copy. */
function stepRefs(prose) {
  const out = [];
  const mk = (raw, at) => {
    const parts = raw.toLowerCase().split(".");
    return { parts, path: parts.map(stepPositions), raw, at };
  };
  STEP_REF.lastIndex = 0;
  for (let m; (m = STEP_REF.exec(prose)); ) {
    const at = m.index + m[0].indexOf(m[1]);
    out.push(mk(m[1], at));
    if (!m[2]) continue;
    STEP_MORE.lastIndex = 0;
    for (let t; (t = STEP_MORE.exec(m[2])); ) {
      const tAt = at + m[1].length + t.index;
      /* A NUMBER THE WORD `step` FOLLOWS IS A SECTION AND NOT ANOTHER OPERAND OF THE RUN — the mirror of
       * STEP_LEAD, which keeps a number the word `step` PRECEDES from being read as a section, and it is the
       * same convention read from the other end. It costs nothing while sub-components are digits, because a
       * `.` then stops the head operand and the run never reaches the next clause; the moment a LETTERED
       * component is readable the head runs on and the `and` joining two CLAUSES reads as the `and` joining
       * two STEPS. Measured, on the first run after that widening: `10.1.9.2 step 2.a and 10.1.7 step 3` put
       * `10.1.7` in front of the checker as a step of §10.1.9.2, which has eight. It landed in the band this
       * channel lists and never accuses, so it cost no red — and the same shape over a section whose step N
       * exists would have been an accusation, which is the reason it is closed here rather than noted. */
      if (SEC_TRAIL.test(prose.slice(tAt + t[0].length, tAt + t[0].length + 12))) continue;
      out.push(mk(t[0], tAt));
    }
  }
  return out;
}

/* A list is its ITEM COUNT when nothing under it is numbered, and `[count, {index: [sub-list …]}]` when
 * something is — see encList for why the leaf form carries no per-item record. */
const listN = (l) => (Array.isArray(l) ? l[0] : l);
const listKids = (l, i) => (Array.isArray(l) && Object.hasOwn(l[1], i) ? l[1][i] : null);

/* WHERE A PATH LEAVES THE STANDARD, or null when some reading admits it whole. This is ONE function rather than
 * a predicate plus a diagnostic because two would be two statements of the same rule, and the one that drifts is
 * the one the findings are not decided by: the frontier it descends is EXACTLY the set of lists a true prefix
 * reaches, so "returns null" and "the path exists" are the same sentence rather than two that agree today. */
/* A COMPONENT IS A SET OF POSITIONS AND THE DESCENT CARRIES ALL OF THEM — the same widening the header
 * defends for LISTS, applied to READINGS. `2.i` descends into the 9th sub-item and into the 1st, and the path
 * fails only where no reading of a component reaches any list at that depth. A purely numeric path is a chain
 * of one-element sets, so it walks the identical frontier this walked before. */
function stepFail(lists, path) {
  let cur = lists;
  for (let k = 0; k < path.length; k++) {
    let best = 0;
    for (const l of cur) if (listN(l) > best) best = listN(l);
    const fit = path[k].filter((v) => v <= best);
    if (!fit.length) return { depth: k, largest: best, sub: false };
    if (k === path.length - 1) return null;
    const next = [];
    for (const l of cur) for (const v of fit) if (v <= listN(l)) { const ch = listKids(l, v); if (ch) next.push(...ch); }
    if (!next.length) return { depth: k, largest: best, sub: true };
    cur = next;
  }
  return null;
}

/* EVERY SINGLE-READING PATH A SPELLING ADMITS, which is what separates a step this tool has DECIDED from one
 * it has merely failed to refute. `stepFail` answers about the union; where the union admits a path and some
 * individual reading does not, the alphabetic/roman convention would have to be settled before anyone could
 * say which step the author named — and the corpus does not record it. That population is NOT DECIDED and is
 * printed as such rather than counted with the paths every reading confirms. The product is bounded by the
 * spelling: only a one-or-two letter run that is also a canonical numeral has two readings at all. */
function stepReadings(path) {
  let acc = [[]];
  for (const cands of path) {
    if (acc.length * cands.length > 16) return null;
    acc = acc.flatMap((a) => cands.map((v) => [...a, [v]]));
  }
  return acc;
}

/* THE TWO FAILURES ARE ONE INDEX APART AND SAYING SO WRONG SENDS THE READER TO THE WRONG STEP. `sub` means the
 * path reached `depth` and the step it landed on carries nothing numbered, so the LAST EXISTING step is
 * path[0..depth] and the missing one is path[0..depth+1]; the other means the index AT `depth` overflowed, so
 * the last existing step is path[0..depth-1]. Written as one expression they differ by exactly one slice
 * boundary, which is why they are written apart. */
function stepMsg(f, parts, no, spec) {
  /* THE SPELLING THE AUTHOR WROTE, never the position it resolved to — a reader sent to `step 2.9` for a
   * comment that says `step 2.i` has been sent to a step the comment does not name. */
  const held = parts.slice(0, f.depth + (f.sub ? 1 : 0)).join(".");
  const missing = parts.slice(0, f.depth + (f.sub ? 2 : 1)).join(".");
  if (f.sub) return `${spec} §${no} has a step ${held} and no list under it, so there is no step ${missing}`;
  return f.depth === 0
    ? `no list anywhere under ${spec} §${no} reaches step ${parts[0]} — the longest has ${f.largest} item(s)`
    : `${spec} §${no}'s step ${held} holds at most ${f.largest} sub-step(s), so there is no step ${missing}`;
}

/* USE VERSUS MENTION — A CITATION THE PROSE IS TALKING ABOUT IS NOT A CITATION THE PROSE IS MAKING, AND
 * REPORTING ONE AS A FABRICATION IS A RED THE TOOL CANNOT BE RIGHT ABOUT.
 *
 * This tree's own rules produce the shape. CLAUDE.md requires a retired number to stay written beside the
 * number that replaced it, so a correct repair READS as a fresh misattribution: `HTML §8.1.3.2 "Environment
 * settings objects" API base URL (§4.4 stood here and is "Grouping content")` cites §4.4 in order to retire
 * it, and every check in this file said the author had put the API base URL in Grouping content. The auditor
 * accusing the note that records the fix is worse than a missed finding by a wide margin: a missed finding
 * costs one wrong number, and a permanent red at a site nobody can repair teaches the reader to skim the
 * category — which is every other finding in it.
 *
 * IT IS MEASURABLE IN THE AUDITOR ITSELF, WHICH IS ALSO WHERE THE CONVENTION HAD TO BE OBEYED RATHER THAN
 * ONLY DESCRIBED. Pointing this audit at the tree's own gates returned worked examples of wrongness among
 * its findings: a sentence reading `a file writing 7.4.9 IteratorClose six times and 7.4.9 IteratorStepValue
 * four times has the numbering of an older edition` is about a SPELLING, and reading it as a claim is reading
 * the quotation marks off. THE DELIMITER MUST BE THE BACKTICK AND NOT THE DOUBLE QUOTE, and that asymmetry is
 * forced rather than chosen: `§8.1.3.2 "Environment settings objects"` is the CORRECT citation form this whole
 * tree writes, so admitting double quotes as a specimen delimiter would disclaim nearly every well-formed
 * citation in the corpus. So a displayed citation is written in backticks HERE too — three of this file's own
 * sentences were rewritten to obey the convention they document, which is cheaper and more honest than
 * widening the detector until it swallowed the thing it protects.
 *
 * TWO DETECTORS, AND EACH IS DEFENSIBLE ON ITS OWN.
 *   SPECIMEN — the number and the phrase sit inside ONE backtick run. Backticks in this tree quote a spelling:
 *     a name, a line of code, a citation being displayed. A citation being DISPLAYED makes no claim about
 *     where a term lives, exactly as `"a header list contains a name"` in quotation marks is not this file
 *     asserting it. A legitimate citation puts the backticks around the TERM alone, never around the section
 *     sign and the term together, so the shape is narrow rather than common. PARITY IS THE TEST AND IT IS THE
 *     WEAK HALF: one unbalanced backtick earlier in a long comment flips every citation below it, so the run
 *     must also close within the prose the citation governs. That error runs in the direction that costs a
 *     finding rather than the one that plants a red, and it is the reason this arm is stated as the shakier
 *     of the two rather than trusted equally.
 *   RETIREMENT — a closed set of phrases that state the number is HISTORICAL: it stood here, it used to be
 *     written, this file cited it, it was cited as. Each says "this number WAS the claim" and none of them
 *     can be read as making it.
 *
 * BARE NEGATION IS DELIBERATELY NOT IN THAT SET, and that is the line the whole mechanism turns on. A comment
 * opening "NOT §4.6.2's a attribute" is CONTRASTIVE: it asserts that §4.6.2 HAS an `a` attribute and that this
 * is a different one, so the number-and-term pair is a live claim and belongs in front of the checker. Reading
 * `not` as a disclaimer would silence a large and legitimate population — this tree writes that construction
 * constantly — for the sake of a handful of notes that already have unambiguous markers of their own. A
 * retirement note says the number WAS here; a contrast says the number IS something else. Only the first
 * disclaims.
 *
 * AND THIS IS NOT A SUPPRESSION LIST, on two counts that must both hold. It is a rule about the GRAMMAR of
 * the sentence, never a file or a line, so it cannot rot into a register of sites somebody decided to ignore.
 * And nothing is dropped: a disclaimed citation is REPORTED in its own counted category with the verdict the
 * checker would have given it, so a reader can see every one and disagree. CLAUDE.md permits a match in the
 * DENY direction for exactly this asymmetry — a wrong deny costs one finding a human can still find, a wrong
 * assert plants a red nobody can clear. */
const MENTION_RETIRED = /\b(?:stood(?: here)?|used to (?:be|say|read|stand|cite|carry)|previously (?:cited|read|said|stood)|formerly|this file cited|(?:was|were) (?:cited (?:as|at)|written here)|cited here as|mis-?cited|retired)\b[^.;]{0,100}$/i;
const MENTION_RETIRED_AFTER = /^[^.;]{0,40}?\b(?:stood here|used to stand|stood at this site|was written here)\b/i;
/* THE RULE ITSELF TAKES THE TWO SIDES OF THE SITE AND NOTHING ELSE, so a second reader that has prose rather
 * than an offset asks the SAME question rather than a copy of it. The step channel is that reader: a step
 * number carries no § of its own, so it is not a citation and has no `at` in a citation's span, and its
 * disclaimer sits in exactly the same place — measured, at a site whose parenthetical says the numbers STOOD
 * as something, quoting a step claim in order to retire it. Restating the two detectors there would be the
 * second copy this file's own header says drifts. */
function mentionNotClaim(src, spans, at, after) {
  return mentionOf(precedingProse(src, spans, at), after);
}
function mentionOf(pre, after) {
  /* An ODD number of backticks between the start of the prose unit and the number means the number stands
   * inside an open run — and PARITY ALONE IS NOT ENOUGH, because one unbalanced backtick anywhere earlier in
   * a long comment flips every citation below it into a mention and the findings vanish with nothing to say
   * so. That is the tolerable direction of this rule's error and it is still not a direction to be careless
   * in, so the run must also CLOSE: a backtick within the prose the citation governs. A stray opener has no
   * closer near each later citation; a real specimen has one a few words along. */
  if ((pre.match(/`/g) || []).length % 2 === 1 && after.includes("`"))
    return "displayed inside a backtick run — a spelling being shown, not a section being cited";
  const m = MENTION_RETIRED.exec(pre.slice(-140));
  if (m) return `the prose before it says "${m[0].trim().slice(0, 60)}"`;
  const a = MENTION_RETIRED_AFTER.exec(after || "");
  if (a) return `the prose after it says "${a[0].trim().slice(0, 60)}"`;
  return null;
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
    /* A MALFORMED ESCAPE IS TEXT, NOT A CODE POINT — and the length test is what tells them apart, because the
       alternation above cannot. Its `u`/`U`/`x` arms demand their exact digit counts, so a `\u` NOT followed by
       four hex digits does not match them; it falls to the final `[\s\S]` arm, which captures the bare letter
       and arrives here with `c === "u"` and NOTHING to parse. `parseInt("", 16)` is NaN and
       `String.fromCodePoint(NaN)` THROWS, so one such sequence anywhere in the audited corpus aborted the whole
       run — every file, for every lane, with a stack rather than a finding.
       THE INPUT IS LEGITIMATE AND THAT IS WHY THIS MAY NOT CRASH. This scanner reads PROSE as well as string
       literals, and prose is entitled to write `\u{1F308}` (ECMAScript's spelling, which C does not have) while
       discussing escapes — which is exactly what an instrument's comment about astral characters did. It is
       not a broken invariant of this codebase, it is text this normalizer does not decode, and the honest
       answer is to leave it as it stands rather than to invent a character for it or to stop.
       So the numeric arms require a payload; anything else yields the character itself, which is what the final
       arm already does for every other undecodable escape. */
    if ((c === "u" || c === "U") && e.length > 1) return String.fromCodePoint(parseInt(e.slice(1), 16));
    if (c === "x" && e.length > 1) return String.fromCharCode(parseInt(e.slice(1), 16));
    if (c >= "0" && c <= "7") return String.fromCharCode(parseInt(e, 8));
    return c === "n" || c === "t" || c === "r" || c === "f" || c === "v" ? " " : c;
  });
}

/* A QUOTATION MARK INSIDE A BACKTICK CODE SPAN IS A CHARACTER BEING SHOWN, NOT A DELIMITER — and this tree's
 * own convention already says so. `singleQuotedRuns` below refuses an opener that FOLLOWS a closing specimen
 * mark for exactly this reason, and its closing paragraph tells an author that "a specimen goes in backticks,
 * which both this scanner and `mentionNotClaim`'s rule already read as a spelling being shown rather than a
 * claim being made". That sentence was FALSE of the double-quote scanner, which read no backtick at all — and
 * the cost of the gap was not a wrong finding, it was a SILENT ZERO, which is the failure this file is written
 * against.
 *
 * WHAT THE GAP ACTUALLY DID, MEASURED RATHER THAN REASONED. An odd `"` leaves the scan with an opener it
 * cannot pair, and the scan then ABANDONED THE WHOLE PROSE BLOCK — `if (j < 0) break`. On the frozen tree that
 * happened 518 times across 95 files and left 37358 characters of prose unscanned: not judged, not counted
 * short, not counted anywhere, so a file carrying one simply read as a file with fewer quotations in it. 34 of
 * those 518 stood at a mark inside a code span, and the two largest of THOSE were `solve_filter.h:91` (2897
 * characters) and `wpt.mjs:209` (2085).
 *
 * AND ONE FILE HAD ALREADY PAID FOR IT IN PROSE, WHICH IS WHAT MAKES THIS A ROOT AND NOT A TIDY-UP.
 * `solve_filter.h` carries a standing instruction to its own authors that neither of two paragraphs "may gain
 * a double quote", because the fragment-set listing above them spells U+0022 inside a code span. That is
 * CLAUDE.md's contract-that-names-a-hazard-and-offers-no-exit: a warning a writer cannot act on except by not
 * writing, standing over a tool defect, for as long as the defect stands. Its account of the mechanism is also
 * wrong in the direction that matters — it says the next quotation is "swallowed as a several-hundred-word
 * continuation", and with no later mark in the block the scan does not swallow, it STOPS. Both readings are
 * symptoms of one cause and the cause is here.
 *
 * THE SPAN CROSSES AT MOST ONE NEWLINE, AND THE ARGUMENT THAT USED TO BOUND IT TO ONE LINE IS KEPT RATHER
 * THAN DELETED, because a reader who re-derives it will re-introduce it. A stray backtick that could open a
 * span running to the end of a comment would mask real quotation marks by the paragraph, and a scanner that
 * masks a paragraph is a way to HIDE a fabrication rather than a way to read a code span. That argument is
 * right. The BOUND it was spent on was wrong, and the two failed together.
 *
 * WHERE THE BOUND ACTUALLY BIT, WHICH IS NOT WHERE IT LOOKS LIKE IT DOES. The quotation window contains no
 * newline at all: `unitProse` replaces a comment's wrap and its gutter with a space before `governedProse`
 * returns it, so for the CHECK a wrapped specimen is already one line and always has been — which means the
 * convention this file prescribes for a wrongly-accused specimen, put it in backticks, has been followable at
 * a wrapped site the whole time, and a report that it was not is a report about a different reader. The two
 * readers whose text is RAW are the ones this bound governed: `treeProse`, which blanks quoted runs out of the
 * corpus of what this tree itself wrote, and `quotedSrcRuns`, whose mask is the file at file length so the
 * citation scan can tell a number in prose from a number inside a quotation. In both, a quotation mark inside
 * a WRAPPED code span was a DELIMITER. `SPEC_STEPS.md` writes `{ns: null, prefix: null, local:` and
 * `"xlink:href"}` as one span across a line break, 41 spans of that shape stand above its line 3457 carrying 8
 * such marks, and each is free to open a run that swallows the paragraphs after it — which is this function's
 * own IT ERRS TOWARD KEEPING THE FINDING clause firing in the accusing direction, since prose removed from the
 * corpus is a site left accused.
 *
 * MEASURED AT 8c03eadd OVER THE DEFAULT TARGET SET, one newline against none, one frozen snapshot, every other
 * input shared. VERIFIED 5249 both ways over 6369 quotations compared both ways: NOTHING LEFT THE COMPARISON,
 * which is the hazard above and it did not materialise. Citations READ 58206 to 58220, with numbers standing
 * inside a quotation 251 to 237 — `core/xml/xml_literal.h` writes `SystemLiteral ::=` and the grammar's own
 * quote characters as one span across a line break, and its header's citations came back at six real lines
 * and numbers instead of four collapsed onto line 4. Findings 1016 to 1013, and those three are
 * `core/dom/range.c` quotations moving out of QUOTE-NOT-FOUND into OWN-PROSE with `SPEC_STEPS.md` named as the
 * file whose own prose holds their words — a reclassification a reader can check in one command, not a
 * suppression, and the site is still judged by every other channel.
 *
 * ONE NEWLINE AND NOT TWO, AND THE COUNTEREXAMPLE FOR THE WIDER RULE IS IN THE TREE. The spans that appear
 * only at two or more newlines are stray-backtick artifacts, and they hold REAL spec quotations that would
 * stop being read: `core/html/fragment_parser.c` carries HTML's `If el is not connected, then return` four
 * newlines from one stray opener and its Fragment scripting mode note six from another. So the tree holds the
 * counterexample for the wider rule and the counterexample for the narrower one, and one newline is the line
 * between them. It is not a granularity anybody picked: a comment WRAPS, so a specimen that reaches the margin
 * is written across two lines, and a specimen written across three is a block rather than an inline span.
 *
 * A WRAPPED SPECIMEN WAS ALREADY READ ONE CHANNEL OVER, which is what makes this a disagreement rather than a
 * preference. `mentionOf` decides a displayed citation by backtick PARITY over the whole prose unit and has
 * never cared about a newline, so a deliberately-wrapped specimen was a spelling being SHOWN to that reader
 * and a claim being MADE to this one. Two answers to one question is the shape that drifts.
 *
 * The double form is tried FIRST because it is how this tree writes a code span whose CONTENT is a backtick
 * (``` `` ` `` ```), and reading that as two single spans would put the mask in the wrong place. IT STAYS
 * LINE-BOUNDED, and that is a residual with its three clauses rather than an oversight. NOT COVERED: a
 * double-form specimen that wraps. WHAT THE NEXT DIFF BUILDS: the same one-newline allowance on the double
 * alternative, bounded the same way. HOW ITS ABSENCE SHOWS: that specimen's own quotation marks arrive at
 * `quotedRuns` as delimiters, exactly as the single form's did above. It is left alone because the double
 * form's content class admits backticks, so a newline there lets one stray opener pair with a closer two lines
 * down, and the whole population it would serve is the 6 double-form spans this corpus holds. */
const CODE_SPAN = /``[^\n]*?``|`[^`\n]*(?:\n[^`\n]*)?`/g;
function codeSpanMask(prose) {
  const mask = new Uint8Array(prose.length);
  if (prose.indexOf("`") < 0) return mask;
  CODE_SPAN.lastIndex = 0;
  let m;
  while ((m = CODE_SPAN.exec(prose))) mask.fill(1, m.index, m.index + m[0].length);
  return mask;
}

/* THE DOUBLE-QUOTED RUNS, WHICH ARE MOST OF THE QUOTATIONS AND NOT ALL OF THEM. This paragraph said for a
 * long time that a quotation was a double-quoted run AND NOTHING ELSE, on the ground that this tree writes
 * code in backticks and a term in single quotes. The first half of that is right and the conclusion did not
 * follow: a term is SHORT and the word floor already declines it, so the mark was never what separated a term
 * from a quotation — length was. What the sentence actually bought was a REFUSAL TO SCAN, and a refusal to
 * scan is not a refusal to judge: a run this function never returns is not counted as too short, not counted
 * as unresolved, not counted anywhere at all, so a file carrying one simply reads as a file with fewer
 * quotations in it. That is the silent zero this tool exists to end, sitting inside the check its own header
 * calls the one a reader trusts most and verifies least — and a fabricated sentence written with the other
 * mark survived in it. `singleQuotedRuns` below reads that mark and states what it costs.
 * THIS SCANNER STAYS THE ONLY READER THE CITATION SCAN USES, and that is a decision rather than an oversight:
 * `quotedSrcRuns` exists to keep a number inside quoted text from being read as a citation, and the mark this
 * function reads cannot also be an apostrophe, so a stray one ends a scan instead of swallowing a paragraph.
 * The other mark can, which is the whole of why the two scanners are not one.
 * THE PAIRING IS SCANNED AND NOT MATCHED, and the difference is not style — a regex with a MINIMUM LENGTH
 * silently re-pairs every quote after a short one. `serialize as \"{\"` is a one-character quotation, so a
 * pattern demanding two characters skips its opening mark, pairs its CLOSING mark with the next opening one,
 * and hands the checker a fragment that begins mid-sentence and belongs to no quotation at all. Measured: it
 * manufactured findings whose quoted text started with a comma. A scanner has no minimum, so a short
 * quotation is READ and then declined by the word floor, where the report can count it. */
function quotedRuns(prose) {
  const out = [];
  const code = codeSpanMask(prose);
  for (let i = 0; i < prose.length; i++) {
    if (code[i]) continue;
    const ch = prose[i];
    /* A QUOTE THE AUTHOR ESCAPED IS NOT A DELIMITER. Both zones write a nested quotation as `\"`: a C message
     * because the literal needs it, a comment because it is quoting C. Reading one as a close pairs the
     * opening mark with a mark INSIDE the quotation and hands the checker a fragment that stops mid-sentence —
     * measured, on a DFAIL quoting `window.open`'s method steps, whose quotation was cut at "throw an". */
    if (prose[i - 1] === "\\") continue;
    if (ch !== '"' && ch !== "\u201c") continue;
    const close = ch === '"' ? '"' : "\u201d";
    const nextMark = (from) => {
      let k = prose.indexOf(close, from);
      while (k > 0 && (prose[k - 1] === "\\" || code[k])) k = prose.indexOf(close, k + 1);
      return k;
    };
    let j = nextMark(i + 1);
    /* A QUOTATION MAY CONTAIN A QUOTATION, AND THE STANDARDS' OWN SENTENCES DO. The paragraph above closes the
     * ESCAPED nested mark; this closes the UNESCAPED one, which is the same defect from the other direction and
     * the one a comment writes without noticing. HTML \u00a77.1.4's obtain reads `parsedItem[1]["report-to"]` and
     * Fetch \u00a73.5's legacy extract reads `mimeType["charset"]`, so a comment quoting either has a bare `"` in
     * the middle of its quotation \u2014 the scan then pairs the opening mark with THAT one, and re-pairs every
     * mark after it, so the tail of the quotation is handed to the checker as a fragment that starts on a `]`
     * and belongs to no quotation at all. Measured: `] exists, then set policy's endpoint to parsedItem[1][`
     * was reported as a finding against HTML, and the real quotation it came out of \u2014 which IS wrong, and
     * whose wrongness the file had reasoned from \u2014 was invisible because no run of it was ever compared.
     * THE DISCRIMINATOR IS THE CHARACTER AFTER THE MARK, and it is a fact about English rather than a guess: a
     * closing quotation mark is followed by whitespace or punctuation and never by a letter or a digit. So a
     * mark that is followed by a word character is a nested OPENER, and the scan resumes after ITS partner.
     * A nested run with no partner falls through to the old reading rather than swallowing the rest of the
     * comment, which is the direction that costs a finding instead of planting one. Straight marks only:
     * the curly pair is already asymmetric and cannot mis-pair this way. */
    if (ch === '"') {
      while (j >= 0 && /[A-Za-z0-9]/.test(prose[j + 1] || " ")) {
        const nestedClose = nextMark(j + 1);
        if (nestedClose < 0) break;
        const resumed = nextMark(nestedClose + 1);
        if (resumed < 0) break;
        j = resumed;
      }
    }
    /* AN OPENER WITH NO PARTNER IS PUNCTUATION, AND IT MAY NOT END THE SCAN. `break` here abandoned the whole
     * remaining prose block, so everything after an odd mark was not judged, not counted short, and not counted
     * anywhere — the silent zero this function's own paragraph names, arriving through this function. Measured
     * on the frozen tree WITH the code-span mask already in place: 483 blocks in 85 files still reach this line
     * and 14808 characters of prose stood behind it. Skipping the mark instead cannot mis-pair anything: every
     * pairing this scan makes is decided by `nextMark` from an opener, and an opener that answered -1 paired
     * with nothing to begin with. It is the same direction the nested-opener loop above already takes — a run
     * with no partner falls through rather than swallowing the rest of the comment. */
    if (j < 0) continue;
    out.push({ text: prose.slice(i + 1, j), at: i, mark: '"' });
    i = j;
  }
  return out;
}

/* THE OTHER MARK, WHOSE WHOLE DIFFICULTY IS THAT IT IS ALSO THE APOSTROPHE. Nothing about the claim a
 * quotation makes depends on which mark carries it, so refusing to read one mark refuses to CHECK a claim
 * that was made — but the two marks are not alike to scan, and treating them as alike is how a widening turns
 * into a fabricator. MEASURED on a frozen tree, twice, and the first number was the scanner's own defect
 * rather than the tree's population: a rule that took every mark preceded by a non-word character found 638
 * runs of six-plus compared words, and a rule that then excluded the apostrophe shapes below found 49. The
 * 589 that vanished were not quotations anybody wrote — they were one apostrophe pairing with another across
 * whole sentences, which is exactly the failure the double-quote scanner cannot have. So the discriminators
 * are not taste; each was read off a population it removed, and each errs toward reading a mark as punctuation
 * rather than as a delimiter, which costs a check and never plants one.
 *   AN OPENER MAY NOT FOLLOW A WORD CHARACTER — the possessive and the contraction, and by far the commonest
 *     shape of both.
 *   AN OPENER MAY NOT FOLLOW A CLOSING SPECIMEN MARK either. This is the one a word-character test misses and
 *     it dominated the 589: this tree writes a possessive on a backticked or bracketed identifier constantly,
 *     and the mark that follows one is an apostrophe standing after punctuation. Reading it as an opener paired
 *     it with the next term the sentence quoted, and handed the checker a run that began mid-clause.
 *   AN OPENER MAY NOT BE A CLITIC — the same possessive surviving the two rules above by having had its
 *     identifier stripped, and the plural forms with it.
 *   AN OPENER MAY NOT STAND AT THE WINDOW'S FIRST CHARACTER, because the character before it is then outside
 *     the window and no rule above can be asked. The window opens immediately after a citation's own number,
 *     so a real quotation cannot start there; what does start there is a possessive on the number itself.
 *   A CLOSER MAY NOT BE FOLLOWED BY A WORD CHARACTER, nor PRECEDED by whitespace — the same reading in the
 *     other direction, which is what stops a run ending on the opener of the next term the sentence quotes.
 * WHAT THIS ADMITS THAT IS NOT A SPEC QUOTATION, stated because a widening's cost is the part a reader cannot
 * measure from the count: a SPECIMEN this tree wrote with the wrong mark — a URL, the assertion strings of a
 * test file — is now compared against a standard and cannot be found in one. Those are reported rather than
 * suppressed, and the repair is at the site and is the tree's own convention: a specimen goes in backticks,
 * which both this scanner and `mentionNotClaim`'s rule already read as a spelling being shown rather than a
 * claim being made. A suppression rule here would be a second copy of that convention and the copy that
 * drifts is the one nobody runs. */
const SINGLE_CLITIC = /^(?:s|t|re|ll|ve|d|m)(?![A-Za-z0-9-])/i;
const SINGLE_NOT_OPEN = "`\"'’”)]}>";
function singleQuotedRuns(prose) {
  const out = [];
  const word = (c) => /[A-Za-z0-9]/.test(c || "");
  for (let i = 1; i < prose.length; i++) {
    if (prose[i] !== "'") continue;
    if (word(prose[i - 1]) || SINGLE_NOT_OPEN.includes(prose[i - 1])) continue;
    if (!word(prose[i + 1])) continue;
    if (SINGLE_CLITIC.test(prose.slice(i + 1, i + 5))) continue;
    let j = -1;
    for (let k = i + 1; k < prose.length; k++)
      if (prose[k] === "'" && !word(prose[k + 1] || " ") && !/\s/.test(prose[k - 1] || " ")) { j = k; break; }
    if (j < 0) break;
    out.push({ text: prose.slice(i + 1, j), at: i, mark: "'" });
    i = j;
  }
  return out;
}

/* THE SAME RUNS, ADDRESSED IN SOURCE OFFSETS — asked by the CITATION SCAN, which reads `src`, where the
 * quotation check reads the flattened prose. It is ONE reader asked twice and not two readers: `quotedRuns`
 * above is called verbatim, on a MASK of the file that has the SAME LENGTH as `src` and every quotation mark
 * at the SAME OFFSET, so a run it finds is already a source range. Restating what a quotation is here would be
 * a second copy of the most load-bearing rule in this file, and the copy that drifts is the one nobody runs.
 * EVERY PART OF THE MASK IS INDEX-PRESERVING, which is the whole of its correctness. Outside a prose span each
 * character becomes a space: that deletes the program's own text and — the part that matters — the C literal's
 * OWN delimiting quotes and the `" "` joints between the adjacent literals of one message, none of which any
 * author wrote as a quotation mark. Inside a literal a C escape's backslash becomes a space and its payload
 * becomes one too, EXCEPT `\"`, whose `"` is kept exactly where it stands, because that escape IS how this tree
 * writes a quotation mark inside a crash message and it is the only one that must survive. Nothing is inserted
 * and nothing is removed, so an offset in the mask is that offset in `src`. */
function quotedSrcRuns(src, spans) {
  const mask = new Array(src.length).fill(" ");
  for (const [s, e, kind] of spans) {
    for (let i = s; i < e; i++) mask[i] = src[i];
    if (kind !== "s") continue;
    for (let i = s; i < e; i++) {
      if (src[i] !== "\\" || i + 1 >= e) continue;
      mask[i] = " ";
      if (src[i + 1] !== '"') mask[i + 1] = " ";
      i++;
    }
  }
  const m = mask.join("");
  /* A MESSAGE IS SEVERAL ADJACENT LITERALS AND A QUOTATION CROSSES THEM — the same adjacency `governedProse`
   * and `precedingProse` walk, read here as MAXIMAL runs because a quotation that opens before a number and
   * closes after it must be ONE unit or the number stands inside neither half of it. What a joint IS is
   * `LITERAL_JOINT`'s to say and not this function's — see it at `proseUnit`, which walks the same adjacency
   * for three other readers. This used to spell the rule again here, and the second spelling is what a joint
   * in a language the first one had never met went wrong in. */
  const runs = [];
  for (let i = 0; i < spans.length; i++) {
    const start = spans[i][0];
    let end = spans[i][1], j = i;
    if (spans[i][2] === "s")
      while (j + 1 < spans.length && spans[j + 1][2] === "s" &&
             LITERAL_JOINT.test(src.slice(spans[j][1], spans[j + 1][0]))) { j++; end = spans[j][1]; }
    for (const q of quotedRuns(m.slice(start, end)))
      runs.push([start + q.at, start + q.at + q.text.length + 1]);
    i = j;
  }
  return runs;
}

/* STRICTLY INSIDE a quoted run — the marks themselves are the author's punctuation and not quoted text, so a
 * number standing ON one is outside every run and is read like any other. */
function inQuotedRun(runs, at) {
  let lo = 0, hi = runs.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (runs[mid][1] <= at) lo = mid + 1;
    else if (runs[mid][0] >= at) hi = mid - 1;
    else return true;
  }
  return false;
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

/* SQUARE BRACKETS INSIDE A QUOTATION ARE THE AUTHOR SAYING THESE ARE NOT THE STANDARD'S EXACT LETTERS, and a
 * checker that punishes the notation punishes the more honest transcription. `participate[s] in [its] inline
 * formatting context` is CSS 2.2 §9.2.2's sentence carrying the inflection the surrounding English needs,
 * marked so a reader can see the join; an unmarked `never causes` for the document's `never cause` claims more
 * than it can support and looks CLEANER to this tool. The tokenizer already deletes the brackets themselves —
 * every non-alphanumeric run becomes a space on both sides — so `[[Set]]` and `[EnforceRange]`, which are the
 * standards' OWN markup, compare correctly as typed. What it cannot absorb is a bracket INSIDE a word:
 * `participate[s]` becomes two tokens where the document has one, and a correctly-pasted quotation is then
 * reported as diverging at its first word.
 * SO THE QUOTATION IS OFFERED IN MORE FORMS, WHICH IS THE MECHANISM STEP_MARKER ALREADY ESTABLISHED and not a
 * second one: the brackets removed with their contents KEPT (an adapted inflection — `participate[s]` for the
 * document's `participates`), and removed WITH their contents (a word inserted for the sentence that the
 * standard does not have). Adding a form can only ever VERIFY something that was reported, never accuse
 * something that was not, so this cannot manufacture a finding — measured at the revision it landed, it retired
 * 14 QUOTE-NOT-FOUND and turned one more into the WRONG-SECTION it had been all along. */
const BRACKETED_ALTERATION = /\[([^\][]*)\]/g;
function fragmentsOf(quote) {
  const parts = String(quote).split(ELLIDED);
  const all = parts.map((p) => quoteTokens(p, false)).filter(Boolean);
  const cut = parts.map((p) => quoteTokens(p, true)).filter(Boolean);
  const big = all.filter((f) => f.split(" ").length >= MIN_FRAGMENT_WORDS);
  const bigCut = cut.filter((f) => f.split(" ").length >= MIN_FRAGMENT_WORDS);
  const words = all.reduce((n, f) => n + f.split(" ").length, 0);
  const compared = big.reduce((n, f) => n + f.split(" ").length, 0);
  const forms = [], seenForm = new Set();
  const addForm = (frags) => {
    const k = frags.join("\u0000");
    if (frags.length && !seenForm.has(k)) { seenForm.add(k); forms.push(frags); }
  };
  addForm(big);
  addForm(bigCut);
  if (/\[[^\][]*\]/.test(String(quote)))
    for (const dropContents of [false, true])
      for (const dropMarkers of [false, true])
        addForm(String(quote).replace(BRACKETED_ALTERATION, dropContents ? "" : "$1")
          .split(ELLIDED).map((p) => quoteTokens(p, dropMarkers)).filter(Boolean)
          .filter((f) => f.split(" ").length >= MIN_FRAGMENT_WORDS));
  return { all, big, bigCut, forms: forms.length ? forms : [big],
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

/* A STANDARD WRITES ONE CONCEPT IN TWO REGISTERS, AND THE CORPUS'S OWN NORMALIZER IS WHAT SEPARATES THEM —
 * SO ASKING ONLY THE PROSE REGISTER IS HALF A QUESTION. Every spec prints a compound concept as spaced words
 * in running text (`page rule`, `remove unsafe`, `toggle popover`) and as ONE IDENTIFIER in its IDL, its
 * algorithm names and its at-rule spellings (`CSSPageRule`, `removeUnsafe`, `togglePopover`). Both are that
 * SECTION'S OWN WORDS; nothing is being transformed into anything. What makes them two questions instead of
 * one is `tokenText`: it lowercases, so the case that separated the identifier's parts is gone, and the only
 * residue of the identifier register is that its words carry NO SEPARATOR. `containsFragments` pads both
 * sides with spaces — which is exactly right for the prose register and structurally blind to the other one.
 *
 * A ONE-WORD PHRASE HAS NO SEPARATOR TO BE MISSING, SO THIS PROBE ASKS IT NOTHING — IT DEGENERATES INTO A
 * BARE SUBSTRING TEST OVER ENGLISH WORDS, AND THAT WAS MEASURED RATHER THAN ARGUED. The evidence this check
 * rests on is the ABSENCE of the separator the prose register would have; below two words there is no such
 * absence to observe, and `includes` is then asking whether one word happens to sit inside another. Built
 * without the floor and run on the whole tree, it confirmed three one-word sites: two are `getiterator` at
 * ECMAScript §7.4.2 "GetIteratorDirect ( obj )" and §7.4.3 "GetIteratorFromMethod ( obj, method )", which
 * are real misattributions of an older edition's numbering (GetIterator is §7.4.4 "GetIterator ( obj, kind )")
 * that this audit is RIGHT about, and the third confirmed `[[Get]]` at HTML §7.2.3 "The WindowProxy exotic
 * object" on the strength of the token `target` — a word with nothing whatever to do with [[Get]]. That third
 * one is the case that decides the floor: the citation is correct, and the check certified it BY LUCK, which
 * MIN_FRAGMENT_WORDS' own paragraph calls worse than no check at all. The floor is not a coincidence-RATE
 * threshold and must not be read as one — the random rate at one word (0.25%) is LOWER than at two (0.43%).
 *
 * AND THE DISQUALIFIER IS A SECOND GATE, ASKING THE STANDARD'S OWN DEFINITION INDEX RATHER THAN A SPELLING
 * RULE, BECAUSE THE FLOOR ALONE ONLY FITS TODAY'S POPULATION. A substring of a longer identifier is usually a
 * DIFFERENT concept, and the two `getiterator` sites above are that shape at one word; the same shape exists
 * at two, and what separates `getiterator` inside `getiteratordirect` from `page rule` inside `csspagerule` is
 * neither prefix-versus-suffix nor word count — it is that `GetIteratorDirect` is a term ECMAScript DEFINES
 * and `CSSPageRule` is not a term CSSOM defines. So the containing token is asked of that standard's own
 * `dfns`/`ops`, and a token it names as a concept of its own DISQUALIFIES the hit: the section is then talking
 * about that other definition and the overlap is an accident of spelling. It refuses nothing in today's
 * accused population, which is exactly why it is written down rather than dropped — counted EXHAUSTIVELY over
 * every committed corpus, 578 of the 2958 (2+-word term × unrelated section) pairs whose join lands inside a
 * token have every containing token defined by that standard, 528 of them in ECMAScript and 47 in Streams, the
 * two that compose operation names out of other operation names. The population that reaches this check has
 * none today; the corpora say that is the accident, not the rule.
 *
 * HOW CONSERVATIVE THIS IS, MEASURED THE WAY MIN_FRAGMENT_WORDS WAS: over 73125 (random multi-word term ×
 * unrelated section) draws across every committed corpus, the join lands inside a token 0.14% of the time
 * against 2.92% for the spaced matcher this stands beside — so it is twenty times MORE conservative than the
 * probe already in use, and it is not a loosening of it. */
const MIN_JOIN_WORDS = 2;

function joinedInToken(hay, phrase, isTerm) {
  const w = phrase.split(" ").filter(Boolean);
  if (w.length < MIN_JOIN_WORDS) return false;
  const j = w.join("");
  for (const tok of hay.split(" ")) if (tok.includes(j) && !isTerm(tok)) return true;
  return false;
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

/* ---- the AGREEMENT axis: when N sites quote ONE sentence, does this tree say the same thing twice? -------- */

/* EVERY CHECK ABOVE JUDGES ONE QUOTATION AGAINST A STANDARD, INDEPENDENTLY, AND NONE OF THEM ASKS WHETHER THIS
 * TREE AGREES WITH ITSELF. CLAUDE.md names the gap and calls the check free: "a spec sentence that GAINS A
 * CLAUSE produces a state the instruments cannot report — the tree DISAGREEING WITH ITSELF, one site right and
 * the rest carrying a retired version — and it survives precisely because each site is judged alone." Its
 * measured instance is Fetch §4.1 Main fetch step 7, whose fourth disjunct four copies here had never
 * transcribed while a fifth had; nobody found it by auditing, and it surfaced only when a diff RELOCATED a
 * quotation to a fresh site and the per-site checker judged it there.
 *
 * SO THE EVIDENCE IS ENTIRELY INTERNAL, AND THAT IS THE WHOLE ARGUMENT FOR THIS CHANNEL EXISTING BESIDE THE
 * OTHERS RATHER THAN INSIDE THEM. It needs no corpus, no section index and no resolution, so it is asked of
 * every quotation the check SAW — including every one the resolver refused (FOREIGN, UNRESOLVED, VOTED,
 * NO-CORPUS, NO-SECTION), where no other channel here can speak at all. Measured on the pair that seeded this:
 * of the three sites quoting CSSOM View §6's get-the-bounding-box answer, one was VERIFIED, one was reported
 * MIS-TRANSCRIBED, and the third sat in UNJUDGEABLE / UNCORROBORATED because nothing placed its bare §3.1 —
 * so the per-site channel saw two thirds of a three-site disagreement and could name none of it as one.
 *
 * IT NEVER RANKS THE SITES BY HOW MANY THERE ARE, which is CLAUDE.md's own instruction: "a site that differs
 * from its siblings is either the only correct one or the only wrong one and BOTH are findings." Adopting the
 * MAJORITY spelling would be exactly the confirmation-by-inference this file refuses one channel over — a
 * spelling chosen BECAUSE it is common is not a spelling anybody checked, and the measured shape of getting
 * that backwards is recorded here already: a section retitled between two regens leaves the tree with the
 * DILIGENT author in the minority. What decides a row is the COMMITTED CORPUS and never a count of sites, and
 * the whole group is printed either way so a reader can see what was compared with what.
 *
 * GROUPING IS BY QUOTATION AND NEVER BY CITATION, AND THAT IS LOAD-BEARING RATHER THAN CONVENIENT. Two sites
 * quoting one sentence may cite different numbers, different standards, or no standard at all — one of them
 * possibly wrong, which is the very case a disagreement is most likely to accompany — so a grouping keyed on
 * the section would miss exactly the population it exists for. Nothing in here reads `spec` or `no`.
 *
 * AND IT IS DERIVED FROM THE QUOTATIONS THE TOOL ALREADY EXTRACTS: there is no list of sentences to watch, no
 * table of phrases, and nothing to keep in step with the tree. The passage relation is computed out of the same
 * `fragmentsOf` token stream every other quotation check compares, so a sentence this tool cannot read is a
 * sentence this channel does not pretend to have grouped.
 *
 * WHAT MAKES TWO QUOTATIONS ONE PASSAGE — two rules, both the file's own floors rather than new thresholds.
 *   (a) A SHARED CONTIGUOUS RUN OF `MIN_COMPARED_WORDS` TOKENS. That is the floor this file already sets for a
 *       quoted run being a QUOTATION rather than a collocation, and the measurement behind it is the one in
 *       fragmentsOf's paragraph: at six words, 6 of 400 phrases from HTML and 32 of 400 from DOM occur in some
 *       other standard at all.
 *   (b) THE TWO SPELLINGS DIFFER BY EXACTLY ONE CONTIGUOUS RUN THAT ONE OF THEM DOES NOT HAVE. That is the
 *       defect's own shape rather than a similarity threshold, and getting there took two wrong rules whose
 *       corrections are the argument for this one.
 *       THE FIRST WAS A SHARED RUN THAT IS A MAJORITY OF THE SHORTER, and it grouped two different sentences
 *       of one standard through a STOCK CLAUSE the standard repeats: CSSOM View ends both getClientRects'
 *       step 3 and clientTop's step 2 on the same nine words about transforms applying to an element, so an
 *       11-token quotation of the first and a 42-token quotation of the second shared nine tokens — a majority
 *       of the shorter, a fifth of the longer — and were reported as one passage quoted two ways.
 *       THE SECOND WAS A MAJORITY OF BOTH, and it survived that and not the shape underneath it. A standard
 *       writes PARALLEL SENTENCES — one per attribute, one per row-and-column, one per arm of a union
 *       conversion — and two correct quotations of two of them are near-identical by construction. Measured
 *       tree-wide on the first honest run: 215 passages, and the head of it was CSS 2.1's row-group sentence
 *       against its column-group one — identical but for `row`/`column group box` and `rows`/`columns` — the
 *       must-return-the-value-it-was-initialized-to sentence written once per ATTRIBUTE NAME, and six arms of
 *       Web IDL §3.2.25's union conversion. (Spelled out rather than quoted, for the reason the residual at the
 *       end of this block records: a worked example between double quotes is a QUOTATION to PASS 4.) Every one of
 *       them is two RIGHT quotations of two DIFFERENT sentences, and a category that size made of those is
 *       furniture inside a day — which costs every real finding beside it.
 *       WHAT SEPARATES THE TWO POPULATIONS IS SUBSTITUTION VERSUS INSERTION. A parallel sentence differs by a
 *       word SWAPPED (`row`/`column`, `detail`/`persisted`, `string`/`numeric`); the defect CLAUDE.md names is
 *       a sentence that GAINS A CLAUSE — Fetch §4.1 Main fetch step 7's fourth disjunct, absent from four
 *       copies and present in a fifth; CSSOM View's `a DOMRect object` against `a DOMRect`. So a DISAGREEMENT
 *       is a pair with a common PREFIX and a common SUFFIX, both non-empty, together at least
 *       `MIN_COMPARED_WORDS`, where ONE side has nothing between them at all — one spelling is exactly the
 *       other with a run inserted in the middle. Both ends non-empty is what makes it a middle rather than a
 *       CUT, and a cut is caught by containment below and is not a finding.
 *   (c) AN ELIDED QUOTATION IS ALWAYS THE CUT SIDE AND NEVER THE DISAGREEING ONE. A run carrying an ellipsis —
 *       the shape ELLIDED reads, a phrase, a `…`, another phrase — has a hole its AUTHOR MARKED, so its
 *       fragments joined look exactly like the short side of an insertion, and reporting the author's own
 *       ellipsis as a disagreement would be this file manufacturing a finding out of its own normalizer, which the
 *       STEP_MARKER paragraph already calls this checker's one unacceptable failure. A multi-fragment spelling
 *       may be CONTAINED in another and may never disagree with one.
 * WHAT MAKES ONE PASSAGE A DISAGREEMENT is that neither spelling CONTAINS the other, asked with the same
 * `containsFragments` word-boundary matcher the verification uses, and that the pair meets (b). A quotation
 * that is a CUT of a longer one — the common and legitimate case, an author quoting the clause they need — is
 * a member of the passage where it is at least half of it, and is printed inside the group so a reader can see
 * what was compared with what.
 * AND WHAT MAKES A DISAGREEMENT A FINDING IS THE COMMITTED CORPUS, ASKED BY THE CALLER RATHER THAN HERE. This
 * function is pure text and knows nothing about any standard, which is what keeps the grouping honest; the
 * split that follows is in `audit`, beside the corpora it needs — see heldBy. A pair that is one clause apart
 * is one of two things, and only the corpus can say which: BOTH spellings are real sentences a standard writes
 * in parallel, in which case nothing is wrong; or exactly one of them is any indexed standard's words, in
 * which case the other is the mis-transcription its own sibling names the repair for. Only the second is
 * accused.
 * WHAT THAT COSTS, STATED RATHER THAN DISCOVERED — three silences, none of them a wrong answer, and the first
 * is the one a later reader will want to close. A SUBSTITUTION is not judged — Web IDL's shortest-argument-list
 * sentence, standing at one site with `in the entries` and at another with `of the entries`, is one of these and
 * one of the two is the standard's; this channel cannot tell it from the parallel-sentence population it is
 * buried in. THE QUOTATION MARKS ARE DELIBERATELY ABSENT FROM THAT EXAMPLE, and the reason is this channel's own
 * subject: a worked example written between DOUBLE QUOTES is a QUOTATION to PASS 4, charged to the nearest
 * citation BEFORE it — a Fetch number, four lines up — so writing it out planted a QUOTE-WRONG-SECTION in the
 * auditor itself. Measured on the first frozen run of this diff, and repaired at the prose rather than by
 * narrowing the checker, which is what this file does with every finding it makes about itself. It is COUNTED and listed under
 * --agree rather than accused, which is the same split every other band here draws between what the tool can
 * demonstrate and what it can only notice. A SHORT quotation whose spellings diverge EARLY shares too few
 * tokens to reach the floor. And a disagreement one of whose sites quotes a MUCH LONGER passage around the
 * disputed sentence fails the majority the grouping needs. */
const AGREE_RUN = MIN_COMPARED_WORDS;

/* The longest run of tokens the two quotations share, contiguously, WITHIN one fragment of each. An elision is
 * a hole the author cut, so a run that spans one is a run neither author typed — the same reason
 * `containsFragments` matches fragment by fragment rather than joining them. */
function longestSharedRun(a, b) {
  let best = 0;
  for (const fa of a) {
    const x = fa.split(" ");
    for (const fb of b) {
      const y = fb.split(" ");
      let prev = new Int32Array(y.length + 1);
      for (let i = 1; i <= x.length; i++) {
        const cur = new Int32Array(y.length + 1);
        for (let j = 1; j <= y.length; j++)
          if (x[i - 1] === y[j - 1]) { cur[j] = prev[j - 1] + 1; if (cur[j] > best) best = cur[j]; }
        prev = cur;
      }
    }
  }
  return best;
}

/* ONE SPELLING IS THE OTHER WITH A RUN INSERTED IN THE MIDDLE — the shape of a sentence that gained a clause,
 * and the only shape this channel accuses. Both shared ends must be non-empty (an empty end is a CUT, which the
 * caller has already excluded by containment and which is not a defect), they must together reach the floor
 * this file sets for a run being a quotation at all, and ONE side must have nothing whatever between them. A
 * spelling the author ELIDED is refused outright: its own ellipsis is an insertion the author declared. */
function clauseEdit(A, B) {
  if (A.cut || B.cut) return null;
  const x = A.hay.split(" "), y = B.hay.split(" ");
  const n = Math.min(x.length, y.length);
  let p = 0; while (p < n && x[p] === y[p]) p++;
  let q = 0; while (q < n - p && x[x.length - 1 - q] === y[y.length - 1 - q]) q++;
  const ra = x.length - p - q, rb = y.length - p - q;
  if (!p || !q || p + q < MIN_COMPARED_WORDS) return null;
  if (Math.min(ra, rb) !== 0 || Math.max(ra, rb) === 0) return null;
  return { prefix: p, suffix: q,
           inserted: (ra ? x : y).slice(p, p + Math.max(ra, rb)).join(" ") };
}

/* THE SPELLINGS, THE PASSAGES THEY FALL INTO, AND WHICH PASSAGES DISAGREE. Candidate pairs come from a shingle
 * index rather than from every pair of spellings: a pair whose longest shared run reaches AGREE_RUN must share
 * at least one run of exactly AGREE_RUN tokens, so the index is EXACT and not a heuristic filter — it excludes
 * only pairs the relation would have refused anyway. */
function agreementPassages(records) {
  const spellings = new Map();
  for (const r of records) {
    const frags = r.frags.bigCut.length ? r.frags.bigCut : r.frags.big;
    if (!frags.length) continue;
    const key = frags.join(" … ");
    let sp = spellings.get(key);
    if (!sp) spellings.set(key, sp = { key, frags, hay: frags.join(" "), cut: false,
                                       n: frags.reduce((t, f) => t + f.split(" ").length, 0), sites: [] });
    if (r.frags.all.length > 1) sp.cut = true;   /* the author elided: never the disagreeing side */
    sp.sites.push(r);
  }
  const all = [...spellings.values()];

  const shingle = new Map();
  all.forEach((sp, i) => {
    const seen = new Set();
    for (const f of sp.frags) {
      const w = f.split(" ");
      for (let k = 0; k + AGREE_RUN <= w.length; k++) {
        const sh = w.slice(k, k + AGREE_RUN).join(" ");
        if (seen.has(sh)) continue;
        seen.add(sh);
        if (!shingle.has(sh)) shingle.set(sh, []);
        shingle.get(sh).push(i);
      }
    }
  });

  const parent = all.map((_, i) => i);
  const find = (i) => { while (parent[i] !== i) i = parent[i] = parent[parent[i]]; return i; };
  const union = (i, j) => { const a = find(i), b = find(j); if (a !== b) parent[a] = b; };
  const disagree = new Set();               /* the i<j pairs that differ by ONE inserted run — the findings */
  const differ = new Set();                 /* one passage, neither contains the other, some OTHER difference */
  const asked = new Set();
  for (const list of shingle.values()) {
    if (list.length < 2) continue;
    for (let a = 0; a < list.length; a++) for (let b = a + 1; b < list.length; b++) {
      const i = Math.min(list[a], list[b]), j = Math.max(list[a], list[b]);
      const pk = i * all.length + j;
      if (asked.has(pk)) continue;
      asked.add(pk);
      const A = all[i], B = all[j];
      /* A CUT FIRST, because a quotation that is another one shortened is the legitimate case and must never
         reach the shape test — where the shortening is at an END it looks exactly like an insertion. */
      if (containsFragments(A.hay, B.frags) || containsFragments(B.hay, A.frags)) {
        if (2 * Math.min(A.n, B.n) > Math.max(A.n, B.n)) union(i, j);
        continue;
      }
      const e = clauseEdit(A, B);
      if (!e) {
        /* not a single inserted clause — a passage only if the two are near-identical anyway, and then a
           DIFFERENCE this channel counts and does not accuse. */
        const run = longestSharedRun(A.frags, B.frags);
        if (run >= AGREE_RUN && 2 * run > Math.max(A.n, B.n)) { union(i, j); differ.add(pk); }
        continue;
      }
      union(i, j);
      disagree.add(pk);
    }
  }

  const byRoot = new Map();
  all.forEach((sp, i) => {
    const r = find(i);
    if (!byRoot.has(r)) byRoot.set(r, []);
    byRoot.get(r).push(i);
  });
  const passages = [], noticed = [];
  for (const members of byRoot.values()) {
    if (members.length < 2) continue;
    let bad = false, odd = false;
    for (let a = 0; a < members.length; a++)
      for (let b = a + 1; b < members.length; b++) {
        const pk = Math.min(members[a], members[b]) * all.length + Math.max(members[a], members[b]);
        if (disagree.has(pk)) bad = true;
        else if (differ.has(pk)) odd = true;
      }
    const g = members.map((i) => all[i]).sort((x, y) => y.sites.length - x.sites.length || (x.key < y.key ? -1 : 1));
    if (bad) passages.push(g); else if (odd) noticed.push(g);
  }
  /* A PASSAGE QUOTED AT SEVERAL SITES IN ONE SPELLING IS THE HEALTHY CASE AND IS COUNTED, because a channel
   * that prints only its findings says nothing about the population they were drawn from — which is the one
   * line CLAUDE.md requires on every run including the clean day. `multiSite` is every passage this tree
   * quotes more than once, whether the copies agree or not, and it is the denominator the disagreements are a
   * fraction OF: a repair moves a passage from `passages` into the agreeing remainder and moves neither out of
   * `multiSite`, so the two numbers cannot drift apart the way a bare finding count does. */
  const multiSite = [...byRoot.values()]
    .filter((m) => m.reduce((t, i) => t + all[i].sites.length, 0) > 1).length;
  const sitesIn = (gs) => gs.reduce((t, g) => t + g.reduce((n, sp) => n + sp.sites.length, 0), 0);
  noticed.sort((x, y) => sitesIn([y]) - sitesIn([x]));
  passages.sort((x, y) => sitesIn([y]) - sitesIn([x]));
  return { spellings: all.length, candidates: passages, noticed, multiSite,
           agreed: multiSite - passages.length - noticed.length, noticedSites: sitesIn(noticed) };
}

/* WHERE A QUOTE-NOT-FOUND SITS ON THE ONE AXIS THAT SAYS WHETHER IT IS SPEC TEXT AT ALL, and it has ONE owner
 * because THREE readers now ask it. A quotation that matched MIN_FRAGMENT_WORDS of the cited standard and then
 * stopped is a real sentence that went wrong partway — the standard's own text names the repair. One that left
 * within those words carries no evidence of being the standard's at all, and that is the band the census has
 * always called indistinguishable from this tree's own prose in quotation marks. The report bands on it, and
 * `treeAuthored` below is asked ONLY of the second — a distinction that is a threshold in one place and a
 * copy in two is the copy that drifts. */
const divergedLate = (q) => !!(q.div && q.div.matched >= MIN_FRAGMENT_WORDS);

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

/* THE DEFAULT POPULATION, LIFTED OUT OF `audit` BECAUSE A SECOND READER NEEDS THE SAME LIST AND MUST NOT
 * RESTATE IT. `treeAuthored` below asks whether a run of words is THIS TREE'S OWN PROSE, and that is a claim
 * about the TREE — so it may not be answered out of whatever paths the caller happened to name. A corpus
 * built from `--since`'s handful of changed files would answer NO for a sentence CLAUDE.md has carried for
 * months, and the same site would then be a finding in the delta run and not a finding in the full one:
 * a verdict that depends on the reader's argv, which is the population-dependent answer this file refuses
 * everywhere else. So the corpus is ALWAYS this list, and a run over one path still judges against the tree.
 * `notify` is how the absent-submodule notice reaches a reader from the audit path and stays silent on the
 * corpus path, where printing it a second time would say the same absence twice about one run. */
/* WHAT THIS TREE ITSELF WROTE, AS THE SAME TOKEN STREAM A QUOTATION IS COMPARED AGAINST — the evidence that
 * separates a quotation this check has JUDGED AND REJECTED from one it CANNOT JUDGE AT ALL.
 *
 * WHY THE SEPARATION IS OWED. The census has said for as long as this check has existed that a fabricated
 * sentence and a run of this tree's own prose in quotation marks both land in QUOTE-NOT-FOUND and "nothing
 * mechanical separates them". That sentence was true and it is the shape CLAUDE.md rates worst in an
 * instrument: two populations that take opposite work, summed behind one verdict, so a reader cannot tell a
 * defect from a thing the tool cannot see. It also costs the channel its credit in the direction nobody
 * checks — a finding total that is a fifth noise teaches a reader to skim the category, and every real
 * fabrication beside it is skimmed with it.
 *
 * THE DISCRIMINATOR IS DERIVED AND NEVER LISTED, which is the whole of why it may be trusted. A list of
 * exempt sentences would be a second copy of prose the tree already holds, and the copy that drifts is the
 * one nobody runs. What is asked instead is a question about the corpus this walk already reads: DO THESE
 * WORDS OCCUR IN THIS TREE'S OWN AUTHORED PROSE, somewhere other than inside quotation marks. `CLAUDE.md` is
 * a `.md` inside the audited tree and is read here like any other file, so its headings are in this corpus
 * because they are in the tree — not because anybody wrote them down twice.
 *
 * AUTHORED means OUTSIDE A QUOTED RUN, and that clause is what stops the mechanism verifying itself. Leave
 * the quoted runs in and a fabrication quoted at ONE site finds itself; quoted at TWO sites it finds its
 * sibling. So every run `quotedRuns` and `singleQuotedRuns` return is blanked before the tokens are taken —
 * the SAME two readers PASS 4 uses, asked of the whole file rather than of one citation's window, so what
 * counts as a quotation cannot come to mean two things. What is left is the sentences somebody in this
 * project wrote as their own, and a quotation matching one of those is a quotation OF THIS TREE.
 *
 * IT ERRS TOWARD KEEPING THE FINDING, IN BOTH OF ITS TWO FAILURE DIRECTIONS. An unbalanced mark makes
 * `quotedRuns` blank more than it should, which REMOVES prose from this corpus and leaves a site accused; a
 * sentence nobody wrote twice is never matched, and stays accused. The direction that would be fatal — a
 * fabrication laundered — needs somebody to have written that same fabricated sentence into this tree
 * unquoted, and then the site's evidence names the file that did, which is a claim a reader can check in one
 * command. Nothing is suppressed either way: the band below prints every site with the file that authored
 * the words, so the queue stays drainable and the verdict stays falsifiable. */
let TREE_PROSE = null;
function treeProse() {
  if (TREE_PROSE) return TREE_PROSE;
  TREE_PROSE = [];
  for (const f of defaultTargets()) {
    let src;
    try { src = readFileSync(f, "utf8"); } catch { continue; }
    let prose = "";
    for (const sp of proseSpans(src, f)) prose += src.slice(sp[0], sp[1]) + "\n \n";
    /* BOTH MARKS AND IN ONE ORDER, for PASS 4's reason: the blanking walks the runs left to right and a run
     * arriving out of order would rewind `cur` and re-admit text a previous run had removed. */
    const runs = [...quotedRuns(prose), ...singleQuotedRuns(prose)].sort((a, b) => a.at - b.at);
    let kept = "", cur = 0;
    for (const r of runs) {
      const stop = r.at + r.text.length + 2;
      if (r.at > cur) kept += prose.slice(cur, r.at);
      kept += "\n \n";
      if (stop > cur) cur = stop;
    }
    kept += prose.slice(cur);
    TREE_PROSE.push({ file: relative(ROOT, f), text: quoteTokens(kept, false) });
  }
  return TREE_PROSE;
}

/* WHICH FILE OF THIS TREE AUTHORED EACH CANDIDATE'S WORDS, or none. Sets `authoredAt` on the record.
 *
 * ONE PASS OVER THE CORPUS AND NOT ONE PASS PER CANDIDATE. The corpus is about twenty megabytes of tokens and
 * the candidates are several hundred, so asking each candidate of each file is twelve gigabytes of scanning —
 * measured at 24.6 s, which is a third again on top of the whole audit. The needles are known before the scan
 * starts, so the scan is turned inside out: a MIN_FRAGMENT_WORDS-word rolling window over the corpus, looked
 * up in a map keyed by each candidate's own first fragment. That is a PREFILTER and never the verdict —
 * `containsAnyForm` is what decides, on the candidate's every authoring and every fragment in order, exactly
 * as the verification and the WRONG-SECTION probe decide. The same two-step is what `alt` already does one
 * channel over, for the same reason and at the same floor. */
function treeAuthored(cands) {
  if (!cands.length) return;
  const want = new Map();
  for (const c of cands) {
    for (const frags of c.frags.forms) {
      if (!frags.length) continue;
      const w = frags[0].split(" ");
      if (w.length < MIN_FRAGMENT_WORDS) continue;
      const k = w.slice(0, MIN_FRAGMENT_WORDS).join(" ");
      if (!want.has(k)) want.set(k, new Set());
      want.get(k).add(c);
    }
  }
  if (!want.size) return;
  for (const { file, text } of treeProse()) {
    if (!text) continue;
    const w = text.split(" ");
    const hit = new Set();
    for (let i = 0; i + MIN_FRAGMENT_WORDS <= w.length; i++) {
      /* A MANUAL JOIN AND NOT slice().join(): this runs once per WORD of the whole corpus, which is millions
       * of times, and the floor is a compile-time constant so the window can be spelled out. */
      const cs = want.get(w[i] + " " + w[i + 1] + " " + w[i + 2] + " " + w[i + 3]);
      if (cs) for (const c of cs) if (!c.q.authoredAt) hit.add(c);
    }
    for (const c of hit) if (!c.q.authoredAt && containsAnyForm(text, c.frags)) c.q.authoredAt = file;
  }
}

function defaultTargets(notify = () => {}) {
  const out = [];
  /* The default is what this project WROTE: the host, plus the fork's own two quickjs translation units.
   * The rest of engine/qjs is upstream and its citations are not this tree's to answer for — but quickjs.c
   * carries more ECMAScript citations than the whole of engine/host, and a gate nobody points at a file is
   * a gate that does not run on it. */
  out.push(...walk(join(HERE, "host")));
  for (const extra of ["qjs/quickjs.c", "qjs/quickjs.h"]) {
    const p = join(HERE, extra);
    /* AND AN ABSENT ONE IS ANNOUNCED, BECAUSE THIS GUARD SILENTLY DROPPED THE LARGEST BODY OF CITATIONS
     * THIS GATE HAS. `engine/qjs` is a SUBMODULE, and a submodule is TRACKED BUT NOT POPULATED: a
     * `git clone --shared` — which is the frozen-snapshot procedure CLAUDE.md prescribes for every
     * instrument that reads this tree — leaves that directory EMPTY unless the snapshot's author copies it
     * in. `existsSync` then answers false, the file is skipped, and the run prints a resolved-of-total that
     * is a fraction of a population missing the file whose own comment above says it carries more
     * ECMAScript citations than the whole of engine/host.
     * MEASURED: a lane froze a snapshot exactly as prescribed, audited it, and reported the fork's
     * §27.5.1.3 cluster as "outside the auditor entirely, before and after" — it was outside THAT RUN, and
     * the reason was an empty directory rather than a policy. Nothing in the output said so.
     * ABSENT AND ZERO ARE DIFFERENT FACTS. A skipped file is not a clean file, so it is named here rather
     * than banded with the findings: a reader who sees no line assumes the default set was read, and this
     * is the one line that can tell them the snapshot they measured is not the tree they think it is. */
    if (existsSync(p)) out.push(p);
    else notify(`[citegen] NOT READ — ${extra} is absent from this checkout, so every citation it ` +
                     `carries is UNAUDITED and the totals below are a fraction of a population without ` +
                     `it. engine/qjs is a submodule: a plain clone does not populate it. This is an ` +
                     `ABSENCE, not a clean result.`);
  }
  /* AND THE TWO DOCUMENTS THE TREE DEFERS TO. CLAUDE.md states the rule this closes — a `.md` a C file cites
   * by name is CODE, because a claim about this tree travels by reference and nothing mechanical reports
   * that it has gone stale — and this file's own header used to name `.md` as its blind spot. These two are
   * cited by name from C hundreds of times between them, and they carry real spec citations with quoted
   * titles and quoted sentences, so leaving them out is the silent zero rather than a clean bill. */
  for (const doc of ["CLAUDE.md", "SECURITY.md"]) {
    const p = join(ROOT, doc);
    if (existsSync(p)) out.push(p);
  }
  /* AND THE TRUSTED ZONE, WHICH IS THE OTHER HALF OF THE PRODUCT AND WAS IN NO RUN THIS AUDITOR HAS EVER
   * MADE. A file the gate does not COLLECT is an excluded file, and the total LOOKS complete — §Testing's
   * rule, arriving here rather than in a test runner. `extension/` holds thirty-five files carrying a `§`,
   * and pointing this audit at them for the first time returned 18 MISATTRIBUTED and 18 undecided sites.
   *
   * THE DIAGNOSIS THAT SENT ME HERE WAS WRONG IN A WAY WORTH RECORDING, because the wrong one is the one
   * that gets repeated. A lane retired a number across the tree, then found more copies by grepping for the
   * RETIRED NUMBER rather than for the files this tool had listed, and concluded that an auditor "sees
   * misplaced terms it indexes, not the same wrong number in prose it cannot parse". That is a claim about
   * PARSING and it is false. The copy it found last was `§4.4 API base URL` in `bridge.js`, which is exactly
   * the form this file indexes — number, then a phrase HTML defines elsewhere — and it was invisible for one
   * reason only: nothing had ever pointed the audit at a `.js` file.
   *
   * AND THE GREP THAT REPLACED THE TOOL DID NOT FINISH THE JOB EITHER. `bridge.js` still carried that number
   * under a commit whose own message states that `git grep` returns no instance of it — `so the engine's
   * §4.4` ends one line and `API base URL was the bare origin` begins the next, so an exact-string grep
   * cannot see it and a prose scan that reads a comment as one unit can. That is the argument for the tool
   * over the grep, made by the case that had been offered as the argument against it. */
  for (const dir of ["extension"]) {
    const p = join(ROOT, dir);
    if (existsSync(p)) out.push(...walk(p));
  }
  /* AND THE GATES, which were named as a stated limit one commit ago and are collected one commit later,
   * because the reason they were outside has been built. A gate's prose is where this project reasons about
   * the standards out loud — idlgen argues Web IDL's §3.7.3 for five paragraphs, trusted.mjs argues Fetch's
   * network error — so a wrong number there misleads a reader exactly as one in a C file does, and it does
   * it in the document that TEACHES the convention. The blocker was real and specific: these files quote
   * wrong numbers on purpose as worked examples, and until MENTION-NOT-CLAIM existed collecting them would
   * have planted permanent false reds in the auditor itself. It exists, so they are collected. `readdir`
   * rather than `walk`, because engine/ contains host/ and qjs/ and this wants only the gates beside them.
   * The 15 real findings this returns are a queue, not a reason to have left it shut. */
  for (const e of readdirSync(HERE)) if (/\.mjs$/.test(e)) out.push(join(HERE, e));
  return out;
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
    for (const need of ["sections", "dfns", "ops", "uses", "specUpdated", "fetched", "base"]) {
      if (!ix[need]) throw new Error(`${relative(ROOT, f)} has no "${need}" — it was written by an older reader; re-run: node engine/citegen.mjs --regen ${s.key}`);
    }
    /* THE CORPUS RECORDS WHICH DOCUMENT IT WAS FETCHED FROM, AND UNTIL THIS LINE NOTHING HAD EVER READ IT —
     * a field written by every regen and consulted by no run, which is the mirror of the defect CLAUDE.md
     * names for a reader with no writer and is quieter, because a stamp nobody reads cannot go wrong loudly.
     * What it costs is the whole of the edition guarantee: an ED and a /TR/ snapshot of one standard are two
     * documents with two numberings, so retargeting a row's base without regenerating leaves every quotation
     * of that standard measured against the words of the document the registry no longer names — and each
     * verdict is individually plausible, because the corpus is internally consistent and merely about
     * something else. `specUpdated` cannot catch it: two editions of one standard can carry the SAME date,
     * and the pair below only asks whether the text and the section numbers came from one fetch, never which
     * document that fetch was of. So the identity of the document is asserted, not the freshness of it. */
    if (ix.base !== s.base)
      throw new Error(`${relative(ROOT, f)} was fetched from ${ix.base} and this registry now names ${s.base} — two editions of one standard are two documents with two numberings, so every citation would be judged against words the registry does not claim. Re-run: node engine/citegen.mjs --regen ${s.key}`);
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
    for (const need of ["sections", "specUpdated", "fetched", "base"])
      if (!tx[need]) throw new Error(`${relative(ROOT, f)} has no "${need}" — re-run: node engine/citegen.mjs --regen ${key}`);
    /* THE DOCUMENT'S IDENTITY IS ASSERTED HERE TOO AND NOT INHERITED FROM THE SECTION INDEX, because THIS is
     * the artifact a quotation is actually compared against — the index decides which numbers exist and the
     * text decides which words do, and it is the words an edition changes. The pair below asks whether these
     * two were fetched from one EDITION; this asks whether they were fetched from the same DOCUMENT, which
     * is a different question wherever two editions share a date. */
    if (tx.base !== SPEC_BY_KEY.get(key).base)
      throw new Error(`${relative(ROOT, f)} holds the words of ${tx.base} and this registry names ${SPEC_BY_KEY.get(key).base} — every quotation of this standard would be judged against another document's sentences. Re-run: node engine/citegen.mjs --regen ${key}`);
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
  /* THE STEP CORPUS IS LOADED ON THE SAME TERMS AS THE TEXT — present or absent, and refused outright where its
   * edition disagrees with the section numbers it is keyed by. The staleness rule bites harder here than it does
   * for text: a quotation checked against a slightly older edition usually still matches, while a step COUNT is
   * exactly the thing an editorial insertion changes, so an edition mismatch here is not a degraded answer, it is
   * a wrong one. */
  const stp = new Map(), stpStale = new Map(), stpNoPos = new Set();
  for (const [key, ix] of idx) {
    const f = stepsFileOf(key);
    if (!existsSync(f)) continue;
    const sp = JSON.parse(readFileSync(f, "utf8"));
    for (const need of ["sections", "specUpdated", "fetched"])
      if (!sp[need]) throw new Error(`${relative(ROOT, f)} has no "${need}" — re-run: node engine/citegen.mjs --regen ${key}`);
    if (sp.specUpdated !== ix.specUpdated) {
      stpStale.set(key, `the corpus is edition "${sp.specUpdated}" and the section index is "${ix.specUpdated}"`);
      continue;
    }
    /* A CORPUS WRITTEN BEFORE THE ITEM POSITIONS EXISTED IS NOT REFUSED — it still answers the question it was
     * built for, and the question it CANNOT answer is reported as unchecked rather than as clean. Absent is a
     * missing FIELD and not a missing FILE, so it is read once here and counted once below: the alternative is
     * a check that silently declines per site, which is the shape this whole file is written against. */
    if (!sp.positions) stpNoPos.add(key);
    Object.setPrototypeOf(sp.sections, null);
    stp.set(key, sp);
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
  /* WHETHER THE FILE SET WAS NAMED OR WALKED, which is what decides whether a HEAD is a truncation or the
   * whole answer. A run over one path — or over what `--since` says changed — has a population bounded by
   * that path, so a band capping its listing there would truncate a list the reader could have had in full,
   * and the reader standing IN the file is exactly the one the NUMBER-ONLY band exists for. */
  const explicitFiles = !!opts.files || targets.length > 0;
  let files;
  if (opts.files) files = opts.files;
  else if (targets.length) files = targets.map((t) => (statSync(t).isDirectory() ? walk(t) : [t])).flat();
  else files = defaultTargets((m) => console.log(m));

  const findings = [];
  const undecided = [];
  const mentions = [];
  const quotes = [];
  /* THE QUOTE-NOT-FOUND RECORDS WHOSE WORDS CARRY NO EVIDENCE OF BEING ANY STANDARD'S, HELD FOR ONE PASS OVER
   * THE TREE'S OWN PROSE AFTER THE WALK. Asked per file it would be one corpus scan per file; asked once it
   * is one scan for the run — see `treeAuthored`. */
  const ownCands = [];
  /* THE QUOTATIONS THIS CHECK REFUSED TO ASK ABOUT, HELD AS SITES AND NOT ONLY AS A NUMBER. A count with no
   * list behind it is the silent-zero shape CLAUDE.md names for exactly this state — "a standard with no
   * committed index is COUNTED and never CHECKED, which is a silent zero rather than a clean bill" — and this
   * channel had it in the one population a SITE EDIT drains: a quotation whose citation names no standard.
   * The census sentence below said how MANY and never WHICH, so nobody could drain one while they were already
   * standing in the file. Only the two site-drainable states are collected; FOREIGN is not, because no edit at
   * that site can fix it, and NO-SECTION is a citation this run already reports as wrong somewhere else. */
  const unjudgedQuotes = [];
  /* EVERY QUOTATION THIS RUN SAW, FOR THE AGREEMENT CHANNEL — see agreementPassages. It is filled BEFORE the
   * five refusals below and never after, because this channel's evidence is other SITES rather than a
   * standard: a quotation whose citation nothing placed is one this tree may still be quoting twice, and it is
   * the population no other check here can speak about at all. A record here is not a finding and not a
   * refusal; it is a member of the denominator, which is `qstat.seen` by construction. */
  const agreeSeen = [];
  /* Every refusal field below carries a `<name>Crash` twin, and `refuse` in PASS 4 throws if a name arrives
   * without one — so a state added here cannot silently lose the who-pays axis that orders the queue. */
  /* `single*` are not a refusal state and take no `Crash` twin from `refuse`: they say which MARK carried a
   * run this check saw, across every verdict and every refusal, so the channel that used to be unscanned can
   * be told apart from the one that always was. A widening whose size nobody can read is a widening nobody
   * can disbelieve, and `singleTooShort` is the half that says how much of it the word floor absorbed. */
  const qstat = { okNearbyCrossLiteral: 0, seen: 0, checked: 0, verified: 0, okNearby: 0, wrongSection: 0, wrongSectionAncestor: 0, wrongStandard: 0, notFound: 0, notFoundNothing: 0,
                  noCorpus: 0, noCorpusCrash: 0, noSection: 0, noSectionCrash: 0, tooShort: 0,
                  single: 0, singleCrash: 0, singleTooShort: 0,
                  voted: 0, votedCrash: 0, foreign: 0, foreignCrash: 0, unresolved: 0, unresolvedCrash: 0,
                  ownProse: 0, ownProseCrash: 0 };
  const noCorpusBy = new Map();
  const gapHist = [];
  const stepsOut = [], stepsAway = [], stepsSays = [], stepsSaysAmbig = [];
  /* THE STEP CENSUS PARTITIONS THE WHOLE POPULATION, INCLUDING EVERY REFUSAL, because CLAUDE.md's recurring
   * defect is several states behind one answer: a step this channel cannot judge because the standard has no
   * corpus, one it cannot judge because only a file vote placed the standard, and one it judged and found
   * present are three different facts, and a single "checked N" would make the first two read like the third.
   * `depth` counts the sub-numbered references apart from the plain ones because they are the population
   * CLAUDE.md says the drift lives in — a promoted nested list renumbers sub-steps and leaves top-level ones
   * untouched. */
  /* THE LETTERED POPULATION IS COUNTED APART BECAUSE IT IS THE ONE THIS CHECK USED TO READ PAST — see
     STEP_NO. `lettered` is how much of the corpus writes a glyph rather than a digit; `letterSplit` is the
     part of it whose readings disagree, which this channel DOES NOT DECIDE and must never be read as
     confirmed; `unreadable` is a spelling that denotes no position at all and is not judged either. */
  const sstat = { seen: 0, sub: 0, lettered: 0, letterSplit: 0, unreadable: 0,
                  checked: 0, exists: 0, okLead: 0, okNearby: 0, okDeclared: 0, reworded: 0, out: 0, away: 0,
                  noCorpus: 0, staleCorpus: 0, noSection: 0, noList: 0, voted: 0, foreign: 0, unresolved: 0, citeFlagged: 0,
                  /* The content channel's own partition, kept apart from the existence channel's for the reason
                     the existence census gives: a step whose corpus carries no positions, one whose phrase is
                     this tree's vocabulary rather than the standard's, and one whose claim was CONFIRMED are
                     three facts, and one "checked" would make the first two read like the third. */
                  claimNone: 0, claimSeen: 0, claimNoPos: 0, claimNotTerm: 0, claimMention: 0, claimOk: 0,
                  claimUnseen: 0, claimAmbig: 0, claimOut: 0 };
  const stepNoCorpusBy = new Map(), stepForeignBy = new Map();
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
  const stat = { total: 0, bare: 0, anchored: 0, byTerm: 0, byFile: 0, other: 0, skipped: 0, quotedNumber: 0,
                 confirmed: 0, confirmedByUse: 0, confirmedByContainment: 0, confirmedByRun: 0,
                 confirmedByText: 0, confirmedByIdent: 0, textRefused: 0,
                 unverified: 0, multiSpec: 0,
                 foreignTerm: 0, titleRefused: 0, byTitle: 0, numberRefused: 0,
                 titled: 0, titledQuoted: 0, titledEv: 0, titledOK: 0, titledMis: 0, titledMisInTitle: 0,
                 titledTailEv: 0, titledTailMis: 0, titleDeclined: 0,
                 noTitle: 0, noTitleDerivable: 0, noTitleNoSection: 0, noTitleCrash: 0,
                 titleBy: { notWords: 0, term: 0, possessive: 0, quotation: 0, oneWord: 0,
                            nearTitle: 0, voted: 0, noCorpus: 0, inSectionText: 0, unplaced: 0 } };
  /* The residue of check (5) — a stated title no channel could place. Listed, never counted as a finding. */
  const unplacedTitles = [];
  const byKey = new Map();
  /* Per standard, how many of its audited citations were placed there by the file vote rather than by their
   * own anchor or their own term — and the sites themselves, so the count has a list behind it. */
  const byKeyVoted = new Map();
  const voted = [];
  const byOther = new Map();
  /* THE NUMBER-ONLY POPULATION — a citation that states a § and no title beside it. It is a BAND and never a
   * finding: nothing here is accused, and the header's rule about the exit code is unchanged. Keyed by
   * `spec §no` because a TITLE IS A FACT ABOUT THE NUMBER and not about the site, so N sites citing one
   * number are one row carrying one answer. `sites` holds the first few coordinates for a reader who wants
   * to stand at one, and `n`/`blind`/`crash` are the counts the census reports. */
  const numberOnly = new Map();
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

  /* A SECTION AND ITS DESCENDANTS, AS ONE RULE WITH ONE OWNER. The quotation check reads a cited §N as the
   * text of that section PLUS everything numbered under it — the join `contains` above exists for — and that
   * rule was written out at three sites: the verification, the same-comment confirmation, and (below) the
   * probe that says whether ANY indexed standard holds a quotation at the number a bare citation wrote. Three
   * copies of one rule is the shape CLAUDE.md refuses in an auditor: the copy that drifts is the one nobody
   * runs against reality, and here the drifting copy would be the one deciding whether a site is CORROBORATED.
   * The PREFIX set is what makes the probe affordable — it answers "could this standard have that number at
   * all" in one lookup, so the forty-standard sweep touches only the standards that actually number it. */
  const secOrder = new Map(), secPrefix = new Map(), ownCache = new Map();
  const ownSections = (key, no) => {
    if (!secOrder.has(key)) {
      const ks = Object.keys(txt.get(key).sections).sort(cmpNo);
      secOrder.set(key, ks);
      const pre = new Set();
      for (const n of ks) { pre.add(n); let i = n.lastIndexOf("."); while (i > 0) { pre.add(n.slice(0, i)); i = n.lastIndexOf(".", i - 1); } }
      secPrefix.set(key, pre);
    }
    if (!secPrefix.get(key).has(no)) return [];
    const ck = key + "\u0000" + no;
    let v = ownCache.get(ck);
    if (v === undefined) { v = secOrder.get(key).filter((n) => n === no || contains(no, n)); ownCache.set(ck, v); }
    return v;
  };
  /* AND THE JOIN THAT FOLLOWS IT, CACHED FOR THE SAME REASON THE DIVERGENCE PROBE CACHES `wholeOf`: the
   * probe below asks the same (standard, number) pair once per quotation that cites it, and rebuilding a
   * section subtree's text per question makes a sweep quadratic in its own population. */
  const ownTextCache = new Map();
  const ownText = (key, no) => {
    const ck = key + "\u0000" + no;
    let v = ownTextCache.get(ck);
    if (v === undefined) {
      const sx = txt.get(key).sections;
      v = ownSections(key, no).map((n) => sx[n]).join(" ");
      ownTextCache.set(ck, v);
    }
    return v;
  };

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
   *
   * AND THE POSSESSIVE STANDING BEFORE THE PHRASE IS REFUSED FOR A REASON THAT IS NOT A THRESHOLD AT ALL, SO
   * IT IS WRITTEN DOWN HERE RATHER THAN LEFT AS AN ABSENCE FOR SOMEONE TO "FIX". `§N's X` DOES NOT CLAIM THAT
   * §N IS TITLED X — it claims X is IN §N, which is the opposite relation and is check (3)'s question, not
   * this one's. The two shapes differ only in which side of the phrase the apostrophe stands on and they mean
   * opposite things: a possessive AFTER the phrase is ADMITTED above (`§4.8.5 The iframe element's …` — the
   * name is complete and the sentence then talks about the thing), which is why `['’]s\b` sits in the
   * terminator set; a possessive BEFORE it makes the phrase the section's PROPERTY. This scan reads the RAW
   * prose while `c.words` was built from prose normTerm had already deleted the possessive out of, so the two
   * readings disagree at that one character — and that disagreement is CORRECT here, not a bug to reconcile.
   *
   * BOTH REFUSALS WERE RE-MEASURED AS A WIDENING AND THE WIDENING IS WRONG IN KIND RATHER THAN MERELY NOISY.
   * Check (2)'s note used to end by naming "make `delimited` see a colon and a possessive" as the thing a
   * future single-reader diff builds; that instruction is RETIRED, and it is retired HERE rather than
   * deleted, because a reader who re-derives it will re-attempt it. Built as one variant — a leading
   * possessive skipped before the scan, `:` added to the terminator set — and measured as two runs on one
   * frozen tree at `b973d98b`, only this file swapped between them: findings 495 -> 502, and EVERY OTHER
   * COLUMN BYTE-IDENTICAL (MISATTRIBUTED 490, UNDECIDED 2625, UNKNOWN-SECTION 0, RETIREMENT-NOTE-WRONG 0,
   * QUOTE-WRONG-SECTION 54, QUOTE-WRONG-STANDARD 24, MENTION-NOT-CLAIM 230), so the whole of it lands in
   * TITLE-MISMATCH, 5 -> 12. ALL SEVEN NEW FINDINGS WERE READ AND FIVE ARE FALSE ACCUSATIONS — a 71% false
   * rate in the one category this file calls the worst finding it can emit. The five: `§8.5.1's` and
   * `§9.3.2's` Computed-value and Applies-to LINES, which are rows of the property definition table those
   * sections carry and which happen to title CSS 2.1's conventions clauses; `§10's used values`, naming what
   * §10 computes; and twice the IDL specimen `§4.4 interface Document : Node` the sentence above already
   * predicted the colon would catch. Every one is exactly right as written and the finding names a
   * replacement number that would edit it into a wrong one.
   *
   * WHAT THE REFUSAL COSTS, MEASURED RATHER THAN ASSERTED: the possessive shape is NOT unjudged, because
   * normTerm deletes the `'s` and hands X to the term scan, which is what caught the pair check (2)'s note is
   * about — `§7.4.1's session history entries` is a MISATTRIBUTED from check (3), never a TITLE-MISMATCH from
   * here. What escapes both is only the case where X is a heading and NO index knows it as a term, and that
   * was two sites in the whole tree, both real: `engine/wpt.mjs`'s animation-frames rows and
   * `engine/fieldgate.mjs`'s callback-functions comment. Two real findings do not buy five wrong ones, and
   * the two were repaired by hand in the diff after this one.
   * WHAT A FUTURE ATTEMPT MUST BUILD FIRST, since the terminator set is not the obstacle: a rule separating a
   * phrase the author DISPLAYED from one they CLAIMED. Three of the five false accusations put the matched
   * words inside a BACKTICK RUN — `mentionNotClaim`'s specimen rule, which today asks about the CITATION,
   * asked about the PHRASE instead. It is not built here because with the terminator set unwidened no
   * backticked phrase can reach this check at all, so it would guard a path that does not exist. HOW ITS
   * ABSENCE WOULD SHOW: it cannot show today; it shows the instant someone widens the set above without it,
   * as those three sites going red.
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
    /* A NUMBER INSIDE A QUOTATION IS THE QUOTED DOCUMENT'S OWN WORDS AND NOT A CITATION ANYBODY HERE WROTE,
     * and the reason this is an EXCLUSION rather than a nuance is that admitting one does not merely add a
     * number to a count — IT DESTROYS THE QUOTATION IT STANDS IN. A citation ends the region the one before it
     * governs, so a number the author never wrote cuts that region at a point INSIDE a quoted run; the
     * quotation check then reads a fragment with an opening mark and no close, `quotedRuns` declines it, and
     * the quotation is not verified, not accused, and NOT COUNTED ANYWHERE. Both halves of a real sentence
     * vanish that way — the half before the cut has no closing mark and the half after it opens on the real
     * closing one — so the site reads exactly like a file with fewer quotations in it. That is the silent zero
     * this file refuses everywhere else, arriving through the one check CLAUDE.md calls the one a reader
     * trusts most and verifies least. MEASURED at the revision this landed, by corrupting one word inside four
     * quotations of one §17.5.3 crash message: the two whose text contains `CSS 2.1` did not move
     * MIS-TRANSCRIBED at all (4 -> 4) while the two without a number in them moved it per-quote (4 -> 5). The
     * standard's NAME has nothing to do with it: injecting a bare `§10.3` into one of the two that DID move
     * silenced it in the same way, which is what says the cause is the citation scan reading a number out of
     * quoted text rather than anything about which document is quoted.
     * IT IS THE SAME DEFECT RANGE_OPERAND AND STEP_LEAD BELOW ALREADY EXCLUDE — the tool inventing the citation
     * it then judges — and it is excluded at the same place for the same reason: a `§`-written number is not
     * exempt, because a standard's own prose cross-references its own sections and a comment quoting that
     * sentence is displaying the reference, not making it. `mentionNotClaim` states this for a specimen inside
     * a BACKTICK run and cannot help here: it withholds a VERDICT, long after the number has already ended a
     * region and taken a quotation with it.
     * WHICH DIRECTION ITS OWN ERROR RUNS: `quotedRuns` is a scanner with no minimum, so an unmatched opening
     * mark ends its scan rather than swallowing the rest of the unit — the failure it can still have is an
     * EARLY stray mark re-pairing the real quotations after it, which would hide a citation instead of
     * inventing one. That is the direction CLAUDE.md permits a deny to be wrong in, and it is measured rather
     * than assumed: the citation total this exclusion removes is reported beside the findings it changes. */
    const qruns = quotedSrcRuns(src, spans);
    CITE.lastIndex = 0;
    for (let m; (m = CITE.exec(src)); ) {
      if (inQuotedRun(qruns, m.index)) { stat.quotedNumber++; continue; }
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
      if (inQuotedRun(qruns, m.index)) { stat.quotedNumber++; continue; }
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
      const qm = /^['"’“]?s?['"’“]?\s*:?\s*["“]([^"”]{2,90})["”]/.exec(after);
      c.quoted = qm ? qm[1] : null;
      /* AND WHETHER A POSSESSIVE ATTACHED IT, RECORDED WHERE THE POSSESSIVE IS ADMITTED — this regex is the
       * one place that can still tell `§N's "X"` from `§N "X"`, and they are two different claims. `§N's X`
       * says X is IN §N, which is check (3)'s question; `§N "X"` says §N is TITLED X, which is check (2)'s and
       * check (4)'s. Check (2) reads both and does so on purpose: a CONFIRMATION that quantifies over a claim
       * the author did not make costs nothing, because the site is already right either way. An ACCUSATION
       * cannot, and the census at check (5) carries the measurement that settles it. */
      c.quotedPossessive = !!qm && /^['’]s/.test(qm[0]);
      c.words = normTerm(after.replace(/^'s\b/, " ")).split(" ").filter(Boolean);
      c.after = after;                       /* the raw prose, for the delimiter test in check (4) */
      /* AND THE WORDS A QUOTED TITLE LEAVES BEHIND, kept for the CENSUS and for nothing else. The probe below
       * reads the QUOTED run, so a citation written `Fetch §N "Fetch methods"' network error` puts its term
       * evidence on `fetch methods` and `network error` is never looked up at all — while the same claim
       * written bare, as `§N's network error`, goes through `lookup` and is reported. That is not an asymmetry
       * to close here: check (2) records the refinement that would close it as built, measured and refused. It
       * is the DENOMINATOR the reported counts are a fraction of, and this file's own rule — stated at check
       * (1) and again at check (4) — is that a refusal nobody can see is the same silent zero as a list nobody
       * can see. One regex, matched once: a second copy of this spelling is a second thing to get wrong.
       *
       * AND THE NUMBER IS WRITTEN `§N` ON PURPOSE, WHICH IS A RULE FOR EVERY WORKED EXAMPLE IN THIS FILE AND
       * WAS LEARNED BY BREAKING IT. This file is AUDITED BY ITSELF, so an example carrying a REAL number is a
       * REAL citation to PASS 3: it joins that number's group for this file and its own term evidence enters
       * `g.keys`, which is what decides how every OTHER citation of that number here resolves. Measured while
       * landing this paragraph — spelling the Fetch number out put `fetch` into the group that the IndexedDB
       * step-number example further up owns alone, the group went from one key to two, the file's dominant
       * anchor broke the tie the other way, and a comment that is exactly right was charged with a
       * QUOTE-WRONG-STANDARD. MENTION-NOT-CLAIM does not save it: that withholds a VERDICT on the mention
       * itself, and runs long after PASS 3 has already counted the mention's evidence for its neighbours. */
      c.tail = null;
      if (qm) {
        const tw = normTerm(after.slice(qm[0].length).replace(/^'s\b/, " ")).split(" ").filter(Boolean);
        if (tw.length) c.tail = tw;
      }
      /* THE FIRST TOKEN AS THE AUTHOR SPELLED IT, kept because normTerm has already destroyed the one signal
       * that separates an operation name from an English word — see addOp. It is accepted only where it
       * normalizes to exactly the first word the lookup will ask for, so a token this scan reads differently
       * from normTerm can never license a lookup normTerm did not produce. */
      const rawHead = (/^\s*(?:['’]s\b)?[\s"“'’(:,—–-]*(\[\[[A-Za-z]+\]\]|%[A-Za-z.]+%|[A-Za-z][A-Za-z0-9_$]*(?:\.[A-Za-z0-9_$]+)*)/
        .exec(after) || [])[1] || "";
      c.opHead = !!rawHead && c.words.length > 0 && nameIsIdentifier(rawHead) && normTerm(rawHead) === c.words[0];
      const only = c.anchor && !c.anchor.startsWith("other:") ? c.anchor : null;
      c.only = only;                         /* the census below re-asks `lookup` with the citation's own scope */
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
        let titledBy = null, titledByQuote = false;
        for (const k of titleCands) {
          const sx = idx.get(k).sections[no];
          if (!sx) continue;
          const wt = normTerm(sx.title);
          if (!wt) continue;
          /* THE LONGEST LEADING PHRASE WINS HERE TOO, WHICH IS THE ONE RULE CHECK (4) HAS THAT THIS LACKED.
           * What stood here was `c.words.slice(0, wt.split(" ").length).join(" ") === wt` — a BARE PREFIX
           * with no longest-match rule at all, so a SHORT title confirmed a citation whose author had
           * written a LONGER one. Measured: HTML §7.4.1 is "Session history" and §7.4.1.1 is "Session
           * history entries", so `§7.4.1's session history entries` was confirmed against §7.4.1 because
           * that section's two-word title is a prefix of the three-word title the author actually stated.
           * An OK-TITLED ENDS THE SITE, so this was not a visible wrong answer — it SUPPRESSED checks (3)
           * and (4) on a citation neither had been allowed to read, which is the silent zero this file
           * names everywhere else. Check (4) states the rule and the reason, and both apply unchanged: a
           * prefix match inside a longer phrase reads a claim out of a sentence that merely contains one.
           *
           * AND ONLY THAT RULE IS IMPORTED — `claimIn`'s OTHER TWO FLOORS ARE DELIBERATELY NOT, WHICH IS A
           * DECISION AND NOT AN OVERSIGHT, SO HERE IS THE REASON BEFORE SOMEONE "FINISHES" THE JOB. Reading
           * this whole check through `claimIn` is the obvious next step; it was BUILT, MEASURED AND REFUSED,
           * and the measurement is the argument. On the tree at the revision it was tried it turned 1746
           * confirmations into 48 fresh MISATTRIBUTED claims, and the ones read were all CORRECT citations:
           * `HTML §15.3.3 Flow content:` (check (2)'s OWN motivating example, three paragraphs up),
           * `§4.10.19.6 Form submission attributes replaces`, `CSSOM §6.4 CSS Rules`, `Web IDL §3.9 legacy
           * platform objects`, and — the case that settles it — `§7.4.1.1's session history entries`, the
           * RIGHT half of the very pair this fix exists for. Two of `claimIn`'s rules are blind in ways that
           * do not matter where it is used and matter enormously here: `delimited` requires the phrase to
           * END A NAME and its terminator set holds no COLON and cannot see a POSSESSIVE before the words
           * (`§N's Title` never matches), and the two-word floor drops one-word titles.
           * THE REASON THAT IS FATAL HERE AND HARMLESS THERE IS THE DIRECTION EACH CHECK FAILS IN, and the
           * two are opposite. Check (4) ACCUSES, so a phrase it fails to read is a finding not raised — it
           * costs recall, which this file has repeatedly chosen to pay. Check (2) CONFIRMS, so a phrase it
           * fails to read is a correct citation handed to the term check and reported as misattributed —
           * a FALSE POSITIVE, which check (4)'s own comment calls the worst finding this tool can emit,
           * because obeying it edits a correct citation into a wrong one. One reading cannot serve both
           * questions: it would be decided by the stricter one and the cost would land, silently, on the
           * looser. So the shared rule is the one that is direction-NEUTRAL — a longer title beats a
           * shorter one whichever way the check points — and the floors stay where their direction is safe.
           * AND THE CLAUSE THAT USED TO END THIS PARAGRAPH IS RETIRED — it said a future attempt must first
           * make `delimited` see a colon and a possessive, and that widening has since been built and
           * measured: it is wrong in KIND, not merely noisy, because `§N's X` claims X is IN §N rather than
           * that §N is TITLED X, which is check (3)'s question and is where that shape is already answered.
           * The measurement, the seven findings it raised and the five that were false are recorded at
           * `delimited`, together with what a single-reader attempt would have to build before the terminator
           * set is worth touching. Until then, widening this is a regression with a tidier shape. */
          let confirms;
          if (c.quoted) {
            confirms = normTerm(c.quoted) === wt;
          } else {
            /* the longest leading phrase that titles ANY section of THIS standard — `wt` is one of them by
             * construction, so this can only refuse when a strictly longer title also starts here. */
            const ttk = titleToNo.get(k);
            let longest = null;
            for (let n = Math.min(maxTitleWords, c.words.length); n >= 1; n--) {
              const p = c.words.slice(0, n).join(" ");
              if (ttk.has(p)) { longest = p; break; }
            }
            confirms = longest !== null && longest === wt;
          }
          if (confirms) {
            verdict = { kind: "OK-TITLED" }; titledBy = wt;
            titledByQuote = !!(c.quoted && normTerm(c.quoted) === wt);
            break;
          }
        }

        /* (3) TERM ATTRIBUTION across every indexed standard. The finding is raised only when NO candidate
         * standard defines the phrase at this number, none defines it UNDER it, and none is prominently about
         * it there — so the claim the report makes is the one it can prove. */
        /* AND IT IS A FUNCTION BECAUSE THE CENSUS ASKS IT TOO. Check (2) above ENDS the site, so every one of
         * these questions goes unasked there — and this file's rule, stated at check (1) and again at check
         * (4), is that a refusal nobody can see is the same silent zero as a list nobody can see. The census
         * below reports what the title consumed, and it reports it by RUNNING THIS, never by restating it:
         * CLAUDE.md's rule for an auditor is that it derives the rule it checks from the code that owns it,
         * because a second copy written for a counter drifts from the checker the day the checker changes and
         * then reports a tool this file no longer has. `stat.foreignTerm` is incremented by the CALLER rather
         * than in here, so asking the question for the census does not move a number about findings. */
        const termCheck = (ev) => {
          const ok = ev.hits.find((h) => h.defAt);
          const under = ev.hits.find((h) => h.underAt);
          const used = ev.hits.find((h) => h.useAt);
          /* AND THE SECTION'S OWN WORDS ANSWER THE SAME QUESTION THE LINK COUNT DOES, WHICH IS WHY THE THIRD
           * CLAUSE OF THIS FINDING WAS FALSE FOR NEARLY HALF OF WHAT IT ACCUSED. The claim a MISATTRIBUTED
           * makes has three parts — not defined here, not defined under here, and no section here is ABOUT it
           * — and until this line the third part was decided ENTIRELY by `scanUses`, which counts `href`
           * ATTRIBUTES pointing at the dfn and which `finish` then discards below two. So the whole question
           * "is this section about the term" was being asked of the editors' CROSS-REFERENCE MARKUP and never
           * of the standard's PROSE, and a section that states the term in its own sentence — without linking
           * it, or linking it once — was reported as having nothing to do with it.
           *
           * THE CORPUS THAT ANSWERS IT IS ALREADY COMMITTED AND ALREADY LOAD-BEARING, which is what makes this
           * a derivation rather than a new rule: `txt` is the per-section word stream the quotation channel
           * compares against, check (5) already asks it the identical question about a QUOTED TITLE, and
           * `containsFragments` is the same word-boundary matcher both use. Nothing here restates a rule the
           * corpus owns — the phrase is whatever `lookup` resolved, so the one-word floor and the
           * identifier gate that admit a bare operation name stay where they are and are not copied.
           *
           * MEASURED, by reading EVERY distinct claim rather than a sample: on the revision this landed
           * against, the term check produced 510 MISATTRIBUTED verdicts, 231 of which stood at a section whose
           * own text uses the phrase. Those 231 collapse to 110 distinct (standard, section, phrase) claims
           * and ALL 110 are correct citations — an abstract operation named by the clause that CALLS it
           * (ECMAScript is where this is thickest, because its clauses are written as sequences of other
           * operations), a CSS term stated by the section that fixes its value for one type, a DOM or HTML
           * concept invoked by the algorithm the comment is about. Four of them QUOTE the cited section's own
           * sentence back, which is the shape CLAUDE.md asks authors for, and the audit was charging them for
           * it. The strongest single case: css-values-4 defines `canonical unit` in its Compatible Units
           * section, and each of its four unit sections says in its own words that all units of that type are
           * compatible and names the canonical one — so four citations that quote those sentences verbatim
           * were four accusations.
           *
           * THE TEST IS THE SECTION'S OWN TEXT AND DELIBERATELY NOT ITS SUBTREE, AND THAT WAS MEASURED TOO,
           * BECAUSE THE SUBTREE VARIANT IS THE ONE THAT LOOKS MORE GENEROUS AND DESTROYS REAL FINDINGS.
           * Widening the haystack to the cited number's descendants confirms 61 more, and among them are the
           * TransformStreamDefaultController sites this file's own PASS-5 paragraph records as REAL: the
           * controller CLASS's subsections invoke the abstract operations, so their words contain the names
           * while the STEPS the comments cite live in another clause entirely. A container chapter's
           * descendants can be made to agree with almost any term of that standard, so a subtree hit is a
           * claim about a subsection the citation did not name. Every one of those 61 stays accused.
           *
           * IT IS ASKED BEFORE THE FOREIGN-TERM REFUSAL BELOW, WHICH TURNS 28 DECLINES INTO ANSWERS AND TAKES
           * NO ACCUSATION AWAY, and those 28 were read too. A site whose phrase only ANOTHER standard defines
           * is refused down there because a finding may only assert what it can prove — but the cited
           * section's OWN WORDS are positive evidence about the standard the citation actually named, so a
           * refusal there is a silent zero where a confirmation is available. The 28 are 20 distinct claims
           * and every one is right: `[[Delete]]` cited at the Web IDL clause of that name, the img element's
           * legacy factory function — which is the very example the refusal's own paragraph gives for
           * vocabulary a correct comment borrows — the iframe element's removing and post-connection steps,
           * and Streams' asynchronous-iteration algorithms cited at the section that states them. This does
           * not weaken that paragraph's argument; it answers the sites the argument had nothing to say about.
           *
           * AND ONE SUPPRESSED STEP FINDING COMES BACK, WHICH IS NOT A WIN AND IS RECORDED SO IT IS NOT READ
           * AS ONE. PASS 5 declines to re-report a step whose SECTION is already flagged, so a false
           * misattribution was hiding a step claim on the same citation — and that step claim is itself
           * false, by a different rule: the step belongs to the dispatch algorithm the surrounding comment
           * cites twice by number, and the step channel attributes a step to the NEAREST preceding citation,
           * which in that sentence is a different section. Two wrong answers were cancelling. The right
           * response is not to keep the false accusation that hid it; it is to name what the step channel
           * still cannot do — decide which of a sentence's several citations a step number belongs to — and
           * leave that to a diff that changes the step channel rather than this one.
           * The refusal is COUNTED rather than silent: a standard whose corpus is absent or stale is asked
           * nothing here, so the finding CARRIES that fact (`noText`) and the caller counts it — zero on the
           * revision this landed against, since every accused citation's standard carries a corpus, and the
           * counter is what makes that a measured zero instead of an assumed one. IT IS COUNTED BY THE CALLER
           * FOR THE REASON `stat.foreignTerm` IS: this function is run a second time by the census below, on
           * sites a title already confirmed, and a counter incremented in here would charge a number about
           * FINDINGS with questions asked for a CENSUS. */
          const tsec = txt.has(spec) ? txt.get(spec).sections[no] : undefined;
          const inText = tsec !== undefined && containsFragments(tsec, [quoteTokens(ev.phrase, false)]);
          /* THE SAME QUESTION, ASKED OF THE OTHER REGISTER — see joinedInToken for why that is two questions
           * and for the guard that keeps the second one honest. It is asked HERE, immediately after the prose
           * register and still ABOVE the foreign-term refusal, because the two are one claim about the cited
           * section's own words and splitting them across that refusal would leave the identifier register
           * answering only for standards that happen to own the term — the exact silent zero the line below
           * this pair was moved above the refusal to end. Nothing existing is reordered.
           * The `noText` refusal already carried by the finding covers this probe too: both read `tsec`, so a
           * standard with no committed corpus is unasked by both and counted once. */
          const identOwn = idx.get(spec);
          const inIdent = tsec !== undefined && !inText
            && joinedInToken(tsec, quoteTokens(ev.phrase, false),
                             (t) => identOwn !== undefined && (identOwn.dfns[t] !== undefined || identOwn.ops[t] !== undefined));
          /* AND THE OTHER MEMBERS OF THIS CITATION'S OWN RUN COUNT AS CITED, because the author wrote them.
           * The claim a MISATTRIBUTED makes — "you cited §N and the thing is numbered somewhere else" — is
           * simply FALSE when "somewhere else" is a number standing three characters to the left under the
           * same `'s`. This is the same asymmetry as the paragraph below: a confirmation may quantify over
           * anything the citation actually says, and a finding may only assert what it can prove. */
          const inRun = c.run
            ? ev.hits.find((h) => h.where.some((d) => c.run.some((r) => d === r || contains(r, d))))
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
          const owned = ev.hits.some((h) => h.key === spec);
          if (ok) return { kind: "OK-TERM" };
          if (under) return { kind: "OK-CONTAINS" };
          if (used) return { kind: "OK-USE" };
          if (inText) return { kind: "OK-TEXT" };
          if (inIdent) return { kind: "OK-IDENT" };
          if (inRun) return { kind: "OK-RUN" };
          if (!owned) return { kind: "FOREIGN-TERM" };
          const where = ev.hits.map((h) => {
            const hx = idx.get(h.key);
            return `${h.key} ${h.where.map((n) => `§${n} "${hx.sections[n] ? hx.sections[n].title : "?"}"`).join(" / ")}`;
          }).join("; ");
          const men = Math.max(...ev.hits.map((h) => h.mentions));
          const one = ev.hits.length === 1 && ev.hits[0].where.length === 1
            ? `${ev.hits[0].key} §${ev.hits[0].where[0]}` : null;
          return { kind: "MISATTRIBUTED", target: one, noText: tsec === undefined,
            /* THE THIRD CLAUSE NAMES BOTH INSTRUMENTS THAT ANSWERED IT, because a reader triaging this has to
             * know whether the words were read or only the links were counted — an unasked question and a
             * negative answer are the two facts this file refuses to average anywhere else. */
            msg: `"${ev.phrase}" is defined in ${where} — no indexed standard defines it at §${no}, nor under it, ` +
                 (tsec === undefined
                   ? `and no committed corpus could be asked whether §${no}'s own words use it`
                   : `nor is any §${no} about it: §${no}'s own text carries the phrase neither as spaced words nor inside an identifier of its own`) +
                 (sections[no] ? ` (${spec} §${no} is "${sections[no].title}"` : " (") +
                 (men ? `, which links the term ${men}×)` : ")") };
        };

        /* WHAT THE TITLE CONSUMED, COUNTED WHERE IT WAS CONSUMED. Two populations, and they are two because
         * they are consumed at two different places — pooling them would report a partition as a total, which
         * is the shape this report already refuses at MENTION-NOT-CLAIM and at the file-vote counters.
         *   (A) the SAME evidence check (3) would have read, which the `break` above ends the site before.
         *   (B) the prose after a QUOTED title, which never reaches a lookup at all: `c.ev` is the probe of the
         *       quoted run, so `Fetch §N "Fetch methods"' network error` puts its evidence on `fetch methods`
         *       while the bare `§N's network error` is looked up and reported. Same claim, two spellings, two
         *       verdicts — and it is (B), not (A), that the asymmetry a reader notices lives in. (`§N`, not the
         *       real number: see the paragraph at `c.tail` for why a worked example in THIS file must not carry
         *       one.)
         * NEITHER IS A FINDING AND NEITHER BECOMES ONE HERE. The refinement that would judge (B) was built,
         * measured and refused three paragraphs up; what is added is the number, because a category whose count
         * excludes a population without saying so is not a coverage figure. And (A) carries the evidence for the
         * refusal beside it: `titledMisInTitle` counts the sites whose term phrase sits INSIDE the very title
         * that confirmed them — the rendering-chapter `Flow content` heading against the content-category dfn
         * of the same name, which is the worked example three paragraphs up — and that is exactly the false
         * positive the title-first order exists to suppress, so a reader can SEE that the consumed population
         * is mostly that rather than take this file's word for it. */
        if (verdict && verdict.kind === "OK-TITLED") {
          stat.titled++;
          if (titledByQuote) stat.titledQuoted++;
          if (c.ev) {
            stat.titledEv++;
            const wouldBe = termCheck(c.ev);
            if (wouldBe.kind === "MISATTRIBUTED") {
              stat.titledMis++;
              const ph = c.ev.phrase;
              if (titledBy && (ph === titledBy || titledBy.startsWith(ph + " ") || titledBy.includes(" " + ph)))
                stat.titledMisInTitle++;
            } else if (wouldBe.kind !== "FOREIGN-TERM") stat.titledOK++;
          }
          if (titledByQuote && c.tail) {
            const tev = lookup(c.tail, no, c.only, false);
            if (tev) {
              stat.titledTailEv++;
              if (termCheck(tev).kind === "MISATTRIBUTED") stat.titledTailMis++;
            }
          }
        }

        if (!verdict && c.ev) {
          const t = termCheck(c.ev);
          if (t.kind === "FOREIGN-TERM") stat.foreignTerm++;
          else verdict = t;
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
        let stated = null;
        if (!verdict) {
          const tt = titleToNo.get(spec);
          let claim = stated = claimIn(tt, c);
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

        /* (5) THE STATED TITLE NOTHING ABOVE COULD PLACE — A CENSUS, NOT A CHECK, AND THE REASON IT IS A CENSUS
         * IS MEASURED RATHER THAN CAUTIOUS. Check (2) CONFIRMS a quoted phrase that IS the cited section's
         * title; check (4) ACCUSES one that titles a DIFFERENT section of the same standard. Between them sits
         * a phrase that titles NEITHER — and that is where a FABRICATED title lands, because a title somebody
         * invented or misremembered heads nothing anywhere, while a title gone stale through a renumbering
         * still heads the section it used to. So the gap is real and it is exactly the shape of the fabricated
         * QUOTATION this file's header records: a claim nobody could falsify because no channel asked.
         *
         * IT IS ASKED NOW AND IT DOES NOT PRODUCE A FINDING, AND THE MEASUREMENT IS WHY. Reported as a finding
         * — quoted run, two words or more, no term any index knows, below the quotation channel's floor, heading
         * no section, and absent from the cited section's own text — it raises 31 claims tree-wide. THIRTY of
         * them are the possessive `§N's "X"`, which asserts X is IN §N and says nothing whatever about §N's
         * title; check (2)'s own paragraph already calls that shape wrong in KIND rather than merely noisy. Of
         * the three that survive dropping the possessive, all three are still TERM claims written without one
         * (a `"close a subscription" steps 1-2`, an `"attribute changed":`, and a banner reading
         * `"CSS At-rules"` — the run itself lists them with their coordinates, which is why no number is spelled
         * out here: a worked example carrying a REAL number is a REAL citation to PASS 3, and its term evidence
         * would then help decide how every OTHER citation of that number in this file resolves — see the
         * paragraph at `c.tail`). Not one of the 31 is a fabricated title. Precision on the axis the channel
         * would claim to measure is zero. The corroboration check (4) requires — the phrase demonstrably heads
         * SOME section — is not a hedge on that channel, it is the ONLY evidence available that a quoted run
         * beside a number is a title claim at all, and removing it removes the subject of the sentence.
         *
         * SO WHAT IS BUILT IS THE THING THAT WAS ACTUALLY MISSING, WHICH IS THE REFUSAL'S VISIBILITY. This
         * file's rule, stated at check (1) and again at check (4), is that a refusal nobody can see is the same
         * silent zero as a list nobody can see — and this population was in no count under any name. An author
         * who wrote the form CLAUDE.md mandates, and got it wrong, read "0 findings" and could not tell that
         * their quoted run had been read and declined. Each bucket below is a DIFFERENT fact about why nothing
         * was said, and the last one — `unplaced` — is the residue a fabricated title would sit in, so it is
         * LISTED for a human exactly as UNDECIDED-ON-A-DIAGNOSED-NUMBER is. Three sites, which a person reads.
         *
         * EVERY BUCKET IS DECIDED BY THE CODE THAT OWNS THE QUESTION, never by a second copy of its rule:
         * `c.ev` is the probe's own answer, `c.quotedPossessive` is set by the regex that admits the possessive,
         * the quotation floor is `fragmentsOf` and `MIN_COMPARED_WORDS` called by name, `stated` is check (4)'s
         * own `claimIn`, and the text test is `containsFragments` over the corpus the quotation check reads. */
        if (!verdict && c.quoted) {
          const q = normTerm(c.quoted);
          const tx = txt.has(spec) ? txt.get(spec).sections : null;
          c.titleBucket =
            !q ? "notWords"
            : c.ev ? "term"
            : c.quotedPossessive ? "possessive"
            : fragmentsOf(c.quoted).compared >= MIN_COMPARED_WORDS ? "quotation"
            : q.split(" ").length < 2 ? "oneWord"
            : stated ? "nearTitle"
            : how === "file" ? "voted"
            : !tx ? "noCorpus"
            /* AND THIS LAST TEST'S JUSTIFICATION HAS BEEN MEASURED FALSE ONCE, WHICH IS RECORDED HERE RATHER
               THAN LEFT FOR THE NEXT READER TO REDISCOVER. The bucket reads a stated title whose WORDS occur
               in the cited section's text as "the author quoted the section, not its heading", and the
               haystack is the cited section JOINED WITH ITS SUBSECTIONS — whose own numbers and TITLES are
               the first tokens of each slice, because that is how the corpus stores a section. So a title
               that heads NOTHING can be absorbed here by a SUBSECTION'S HEADING, which is this channel's own
               question one level down and the opposite of what the bucket claims.
               MEASURED, with a positive control, after another lane reported a wrong title going unreported:
               `css-values-4 §6.1.2 'Viewport-relative Lengths'` — §6.1.2 is "Viewport-percentage Lengths: the
               *vw…" and `Viewport-relative` is §6.1.2.2's word, in "The Various Viewport-relative Units".
               Two clean single-line controls, one single-quoted and one double-quoted, both left
               TITLE-MISMATCH at 0. §6.1.2's OWN text does not carry `viewport relative` at all; §6.1.2.2's
               does, in its heading, and the join is what put the control here.
               WHY IT IS NOT REPAIRED IN THE DIFF THAT FOUND IT: check (5)'s paragraph below records the
               measurement that made this whole band a census rather than a finding, and moving one bucket out
               of it is a decision about that band, taken against a reading of the band, not a bug fix. What
               is wrong TODAY is the SENTENCE, so the sentence is what changes; the next reader who wants the
               bucket narrowed should ask the phrase whether it titles a section CONTAINED BY the cited one
               before asking whether its words appear under it, and read the whole band before landing it. */
            : containsFragments(Object.keys(tx).filter((n) => n === no || contains(no, n))
                                  .map((n) => tx[n]).join(" "), [quoteTokens(c.quoted, false)]) ? "inSectionText"
            : "unplaced";
        }
      }

      const rec = { file: relative(ROOT, file), line: lineOf(c.at), no, spec, how, bare: c.bare,
                    text: src.slice(c.at, c.at + 100).split("\n")[0] };

      /* THE PROSE MAY DISCLAIM ITS OWN CITATION — see mentionNotClaim. Asked of EVERY resolved site, decided
       * or not, because the suspect channel below turns an undecided site on a diagnosed number into a thing a
       * human must read, and a retirement note is precisely a site standing on a number that IS diagnosed
       * elsewhere in its file: the note gets swept in by the same rule that finds the real ones.
       *
       * AND A RETIREMENT NOTE IS CHECKED RATHER THAN EXCUSED, which is the difference between completing the
       * primitive and adding a hole. `(§4.4 stood here and is "Grouping content")` makes a SECOND, falsifiable
       * claim beside the one it withdraws: that §4.4 is titled "Grouping content". This audit holds every
       * title of every indexed standard, so it grades it. A note whose title the index confirms is VERIFIED —
       * the strongest state any citation in this tree reaches, because it has been checked in both directions.
       * A note naming a title the cited number does not carry is a NEW finding and a worse one than the
       * misattribution it was mistaken for: a wrong retirement note tells the next reader that a number they
       * are about to correct is already understood. */
      const after120 = governedProse(src, spans, c.at, c.len, null).slice(0, 120);
      const disclaim = mentionNotClaim(src, spans, c.at, after120);
      if (disclaim) {
        /* THE NOTE'S TITLE CLAIM IS THE FIRST QUOTED RUN AFTER THE NUMBER AND NOTHING FURTHER OUT, and getting
         * that wrong is this checker committing the defect it is here to end. The first attempt scanned 120
         * characters for ANY quoted run that titled some other section, and its first output was a false red
         * on this file's own header — the sentence recording that determine the origin was cited as §7.3.1
         * Navigables across five files while the standard defines it one heading over — where Navigables IS
         * §7.3.1's title and the sentence is exactly right. It matched the OTHER title in it, which
         * belongs to the OTHER number in the same sentence. So the window stops at the next citation: a title
         * that follows another number is that number's, and reaching past one to accuse the first is the same
         * misattribution in the auditor that the auditor exists to report. */
        const claimWin = after120.split(/§|(?<![\d.])\d+\.\d/)[0];
        const tt = titleToNo.get(spec), real = sections[no] ? normTerm(sections[no].title) : null;
        let note = null;
        for (const q of quotedRuns(claimWin)) {
          const n = normTerm(q.text);
          if (!n) continue;
          /* A QUOTED TITLE NEAR A RETIRED NUMBER IS NOT AUTOMATICALLY A CLAIM ABOUT THAT NUMBER, and the two
           * shapes read almost identically. `§4.4 stood here and is "Grouping content"` asserts the title;
           * `§13.2.5.41 was written here FOR "comment start state"` names the term the number was wrongly used
           * for, and asserts nothing about §13.2.5.41's own title. The second is CLAUDE.md's own worked example
           * of verifying a number before writing it, and the first version of this arm reported that sentence
           * as a wrong retirement note — a permanent false red in the one document every lane reads, produced
           * by the checker built to end permanent false reds.
           *
           * SO CONFIRMING AND ACCUSING ARE ASKED DIFFERENTLY, ON PURPOSE. A confirmation is additive: the site
           * is already reclassified either way, so matching the number's real title anywhere in the window only
           * adds a fact. An ACCUSATION plants a red, so it requires the sentence to actually make the claim —
           * a copula binding the number to the title, with nothing but the note's own words between them. Same
           * asymmetry CLAUDE.md draws between a deny and an assert, at the resolution of one verb. */
          if (n === real) { note = { ok: true, title: q.text }; break; }
          if (tt && tt.has(n) && /\b(?:is|was|are|were|reads|remains)\s*$/i.test(claimWin.slice(0, q.at)))
            note = { ok: false, title: q.text, at: tt.get(n) };
          break;
        }
        if (note && !note.ok) {
          findings.push({ ...rec, kind: "RETIREMENT-NOTE-WRONG", target: `${spec} §${note.at.join(", §")}`,
            msg: `the note retiring §${no} says it is "${note.title}" — that titles ${spec} §${note.at.join(", §")}; §${no} is "${sections[no].title}"` });
          continue;
        }
        mentions.push({ ...rec, why: disclaim, note,
          would: verdict && !verdict.kind.startsWith("OK") ? `${verdict.kind}: ${verdict.msg}` : null });
        continue;
      }

      /* THE NUMBER-ONLY BAND, RAISED HERE FOR THE REASON THE COMMENT DIRECTLY BELOW GIVES: a MENTION is not a
       * claim, so it must be in neither the count nor the list, and everything above this line has already
       * `continue`d the mentions out. A citation that RESOLVED and states no title reaches exactly here.
       *
       * WHY THIS IS A BAND AND NOT A CHECK. CLAUDE.md §Browser half requires the section's TITLE beside the
       * number, and gives the reason: "the title survives an edition the number does not, and a mismatch
       * between the two is then visible instead of silent." Every channel above can falsify a citation that
       * states a title — check (2) confirms one, check (4) accuses one, check (5) censuses one it cannot
       * place. A citation stating a NUMBER ALONE is outside all three BY CONSTRUCTION: there is no claim
       * beside the number for any of them to read, so the site is ACCEPTED and no line anywhere says it was
       * never examined. That is this file's own silent zero, arriving at the one shape CLAUDE.md asks
       * authors to avoid, and it is the shape a renumbering hides in — a wrong number sends a reader to the
       * wrong place, where they find out, and a number-only citation that RESOLVES tells them they arrived.
       *
       * TWO INCIDENTS, HOURS APART, IN DIFFERENT FILES, NEITHER LANE KNOWING OF THE OTHER, AND BOTH WERE
       * INVISIBLE TO THIS TOOL FOR THE SAME REASON. THE NUMBERS ARE WRITTEN `§N` HERE FOR THE REASON THE
       * `c.tail` PARAGRAPH GIVES — this file is audited by itself, so a worked example carrying a REAL
       * number is a REAL citation to PASS 3 and its term evidence would then help decide how every OTHER
       * citation of that number here resolves. Spelling these two out moved five citations and one
       * quotation verdict when this band was landed, measured on a frozen tree; the TITLES below are what
       * make the incidents legible and they are not numbers.
       * `quickjs.c` cited two ECMAScript Annex B §Ns for `Object.prototype.__lookupGetter__` and
       * `__lookupSetter__`; in the edition the editors maintain, those two numbers are
       * `String.prototype.blink ( )` and `String.prototype.bold ( )` — so the citation resolved, read as
       * authoritative, and sent the reader to a clause about markup. `core/dom/range.c` cited a Web IDL §N
       * for where an interface's CONSTANTS go; that §N is `Named properties object`, and `Constants` is the
       * section AFTER it. Both numbers EXIST, so check (1) had nothing to say; neither site stated a title,
       * so checks (2), (4) and (5) were never asked; and `constant` is not a term the Web IDL corpus
       * indexes, so check (3) had no phrase to look up. Every channel was silent and every silence was
       * correct. The population was in no count under any name.
       *
       * SO WHAT IS BUILT IS THE REFUSAL'S VISIBILITY AND THE ONE ANSWER THE CORPUS CAN ALREADY GIVE, which
       * is check (5)'s own settlement applied one axis over: the corpus holds the TITLE of every section its
       * index has, so for a citation that states only a number this tool can say WHAT THAT NUMBER IS. That
       * is not an accusation — a bare citation is very often exactly right — it is the two facts a reader
       * needs on one line. A row reading `idl §N` followed by the heading `Named properties object`,
       * standing beside a sentence about an interface's constants, is a defect a human sees at a glance and
       * no channel here can see at all.
       *
       * IT IS DELIBERATELY NOT A FINDING, and the reason is landed law rather than caution. CLAUDE.md: an
       * instrument that cannot see something has not FOUND anything, and a gate states its findings and its
       * blind spots as SEPARATE verdicts. The worked example is a record-field gate that read FAILED on
       * every build of an entire session with every real-defect category at zero, purely because its
       * blindness was summed into its verdict — chronic red and informative red are indistinguishable in the
       * one line a reader actually sees. This population is five figures; reported as findings it would be
       * furniture inside a day and would take the four real categories down with it. */
      {
        const titledOk = !!verdict && verdict.kind === "OK-TITLED";
        if (!titledOk) {
          const crash = inCrashMessage(src, spans, c.at);
          stat.noTitle++;
          if (crash) stat.noTitleCrash++;
          /* THREE STATES AND NOT TWO, kept apart for the reason this file keeps every other triple apart: a
           * number the corpus can title, a number the standard does not have at all (already reported or
           * refused above — the corpus has no title to offer and saying "no title" of it would read as the
           * same fact), and a site where nothing else corroborated the number either. Summing them would
           * make the band report on a mixture, which is the shape a reader cannot act on. */
          if (sections[no]) stat.noTitleDerivable++; else stat.noTitleNoSection++;
          /* `verdict` here is a CONFIRMATION from the term channel or an ACCUSATION from it. Only the sites
           * nothing corroborated at all enter the listing: where check (3) confirmed the number by the term
           * standing beside it, the tool HAS evidence and the missing title costs a reader convenience
           * rather than a check. Counted either way, LISTED only where the tool is blind — which is the same
           * line the census draws, so the listing's population is one the report names rather than one a
           * reader has to infer from its length.
           * A NUMBER THE STANDARD DOES NOT HAVE STAYS IN THE LISTING and says so in its row. It is the one
           * member of this population the tool holds a real opinion about, and for a TERM-resolved site that
           * opinion is printed NOWHERE ELSE: check (1) refuses to fire unless the standard is the citation's
           * own evidence, so `(no such section)` here is the only line in the whole report that says it. */
          /* NO COUNTER OF ITS OWN: `!verdict` here is the SAME predicate `stat.unverified` is raised on a few
           * lines below, over the same population at the same point in the loop, so a second tally would be
           * two right answers to one question — the shape CLAUDE.md says drifts, and the one this file
           * already routes rather than re-spells. The band reports `stat.unverified` and the identity
           * between it and this map's own site count is ASSERTED at the report, where both are in one hand. */
          if (!verdict) {
            const uk = `${spec} §${no}`;
            let g = numberOnly.get(uk);
            if (!g) numberOnly.set(uk, g = { n: 0, crash: 0, voted: 0,
                                             title: sections[no] ? sections[no].title : null, sites: [] });
            g.n++;
            if (crash) g.crash++;
            if (how === "file") g.voted++;
            if (g.sites.length < 4) g.sites.push(`${relative(ROOT, file)}:${lineOf(c.at)}`);
          }
        }
      }

      /* COUNTED HERE AND NOT AT THE CHECK, because a MENTION is not a claim and must be in neither the count
       * nor the list — the same reason the disclaim block above withholds a verdict rather than a report. */
      if (c.titleBucket) {
        stat.titleDeclined++;
        stat.titleBy[c.titleBucket]++;
        if (c.titleBucket === "unplaced") unplacedTitles.push({ ...rec, title: c.quoted, real: sections[no].title });
      }

      if (!verdict) {
        stat.unverified++;
        rec.groupNo = c.no;
        undecided.push(rec);
        continue;
      }
      const COUNTED_OK = { "OK-USE": "confirmedByUse", "OK-CONTAINS": "confirmedByContainment", "OK-RUN": "confirmedByRun",
                           "OK-TEXT": "confirmedByText", "OK-IDENT": "confirmedByIdent" };
      if (COUNTED_OK[verdict.kind]) { stat.confirmed++; stat[COUNTED_OK[verdict.kind]]++; continue; }
      if (verdict.kind.startsWith("OK")) { stat.confirmed++; continue; }
      /* THE SITE IS NOW A FINDING, AND PASS 5 MUST NOT REPORT IT TWICE. A step number under a citation whose
       * SECTION is already reported wrong is not a second defect — it is the same one seen from further down,
       * and the section finding states it better: measured, six `Streams §6.3 …Enqueue step 5` sites, each
       * already carrying "…Enqueue is defined in streams §6.4.2, and §6.3 is The TransformStreamDefaultController
       * class", would have been repeated as "no list under §6.3 reaches step 5" — true, weaker, and inflating a
       * count by restating a claim. Marked here rather than re-derived there, so the two can never disagree. */
      c.flagged = verdict.kind;
      if (verdict.noText) stat.textRefused++;
      findings.push({ ...rec, ...verdict });
    }

    /* PASS 4 — THE QUOTATIONS EACH CITATION GOVERNS. It runs after resolution because it USES the resolution:
     * this check does not decide which standard a comment is about, it asks whether the words the comment
     * attributes to a section are that section's words. Deriving the standard again here would be a second
     * copy of the most argued-over rule in this file, and the copy that drifts is the one nobody runs. */
    {
      const admitted = cites.filter((c) => c.admitted);
      /* WHICH CITATIONS SHARE A PROSE UNIT WITH THIS ONE — see the OK-NEARBY channel below. A UNIT AND NOT A
       * SPAN: see `proseUnit`, which owns that distinction for every reader in this file. */
      const byUnit = new Map();
      for (const c of admitted) {
        const k = proseUnitKey(src, spans, c.at);
        if (k < 0) continue;
        if (!byUnit.has(k)) byUnit.set(k, []);
        byUnit.get(k).push(c);
      }
      for (let i = 0; i < admitted.length; i++) {
        const c = admitted[i];
        const stop = i + 1 < admitted.length ? admitted[i + 1].at : null;
        const prose = governedProse(src, spans, c.at, c.len, stop);
        /* BOTH MARKS, IN ONE ORDER, because everything downstream of here reads a run's POSITION — the
         * nearest-preceding rule, the gap histogram, the window a confirmation searches. Two scans appended
         * one after the other would hand this loop a run at offset 400 before a run at offset 12 and make
         * those readers answer about the wrong neighbour. The two scanners stay separate for the reason
         * `singleQuotedRuns` gives; their OUTPUT is one stream. */
        const runs = [...quotedRuns(prose), ...singleQuotedRuns(prose)].sort((a, b) => a.at - b.at);
        for (const q of runs) {
          const f = fragmentsOf(q.text);
          if (!f.all.length) continue;
          if (f.compared < MIN_COMPARED_WORDS) {
            qstat.tooShort++;
            if (q.mark === "'") qstat.singleTooShort++;
            continue;
          }
          qstat.seen++;
          const rec = { file: relative(ROOT, file), line: lineOf(c.at), no: c.no, spec: c.spec,
                        how: c.how, quote: q.text.trim(), words: f.words, elided: f.elided, gap: q.at,
                        mark: q.mark, crash: inCrashMessage(src, spans, c.at) };
          if (q.mark === "'") { qstat.single++; if (rec.crash) qstat.singleCrash++; }
          /* BEFORE EVERY REFUSAL BELOW — the agreement channel is asked of the whole `qstat.seen` population,
             and half its value is the sites the resolver could not place. */
          agreeSeen.push({ ...rec, frags: f });
          /* THE FIVE STATES ARE KEPT APART, because CLAUDE.md's recurring defect is several states behind one
           * answer and a search cannot be directed toward a gap it cannot see. Each refusal below is a
           * DIFFERENT fact about why this quotation was not judged, and each is counted under its own name.
           *
           * THAT SENTENCE STOOD ABOVE TWO BRANCHES THAT SHARED ONE COUNTER, which is the removal-announced-
           * but-not-made shape CLAUDE.md records against THIS FILE by name: a comment certifying a separation
           * the line beneath it does not make, so an auditor reads the claim and never runs the grep. The
           * merged name was `voted`, and the census printed the whole of it under the file-vote wording —
           * false of the half that reached it through `!c.spec`. It is not a cosmetic
           * mislabel: this population is a WORK QUEUE, and the three states have three different repairs.
           *   FOREIGN — the citation NAMES a standard, and this tool indexes no corpus for it. Writing a
           *     standard at the site fixes nothing; the repair is an INDEX, and it belongs to whoever adds
           *     one. It is counted apart so it stops inflating a queue no edit at these sites can drain.
           *   UNRESOLVED — no anchor, no stated title, no term any index knows, and no file anchor to vote
           *     with. Nothing placed it, so there is no guess to refuse.
           *   VOTED — the file vote placed a standard and THE FILE VOTE MAY NOT JUDGE, this file's own
           *     division of labour. A quotation finding is a statement about a STANDARD, so a citation whose
           *     standard is an inference from its neighbours has nothing here that can be demonstrated.
           * WHAT SEPARATES THE LAST TWO IS WHETHER THIS TOOL HOLDS AN OPINION, NOT WHETHER A SITE EDIT HELPS,
           * and the difference is worth stating because the obvious reading is wrong in a way that was
           * MEASURED here rather than reasoned about. Both are drained by writing the standard's name in
           * front of the number: a bare section number bearing a quotation, in a file carrying no anchor to
           * vote with, lands in UNRESOLVED and leaves `checked` exactly where it was; the same site with the
           * standard's name written in front of that number is COMPARED, one more quotation judged and the
           * refusal gone. That was run both ways on a frozen tree. So the actionable per-site queue is VOTED plus
           * UNRESOLVED, and only FOREIGN is outside it. VOTED is nonetheless the head of that queue, because
           * it is where the audit prints a standard in parentheses it has no evidence for — a reader skimming
           * --unanchored can take that inference for a fact, which is the one way this tool commits the
           * failure it exists to catch. UNRESOLVED is silent instead of wrong, and silence is the cheaper bug.
           * The step channel next door has partitioned exactly this way all along; the divergence was here.
           *
           * AND EACH REFUSAL IS COUNTED TWICE — TOTAL, AND THE SUBSET A CRASH PRINTS. `rec.crash` is already
           * computed above for every quotation and was read only by the finding kinds, so for the whole
           * unjudged population it was a computed value consumed by nothing: CLAUDE.md's mirror of the
           * read-with-no-writer defect, and the harder one to see, because such a value is real and asserted
           * and simply read by nobody. It is the one axis that orders the queue by who pays — a quotation
           * in a comment is read with the file open, and a quotation in a DFAIL message is read by whoever is
           * standing at the abort with nothing around it, which is the same reason --unanchored ranks on it. */
          const refuse = (why) => {
            /* A TYPO'D NAME WOULD MAKE A COUNTER `NaN` AND PRINT IT, which is the plausible-datum shape this
             * census exists to end, so the field must already be declared in qstat. */
            if (!(why in qstat) || !(why + "Crash" in qstat)) throw new Error(`citegen: undeclared refusal counter ${why}`);
            qstat[why]++;
            if (rec.crash) qstat[why + "Crash"]++;
          };
          if (c.foreign) { refuse("foreign"); continue; }
          if (!c.spec) { refuse("unresolved"); unjudgedQuotes.push({ ...rec, why: "unresolved", frags: f }); continue; }
          if (c.how === "file") { refuse("voted"); unjudgedQuotes.push({ ...rec, why: "voted", frags: f }); continue; }
          if (!txt.has(c.spec)) { refuse("noCorpus"); noCorpusBy.set(c.spec, (noCorpusBy.get(c.spec) || 0) + 1); continue; }
          const tx = txt.get(c.spec).sections;
          /* A SECTION CONTAINS ITS SUBSECTIONS — the same rule check (3) applies to a term, applied to text,
           * and it is the join that keeps the corpus free of duplication. The slices are contiguous in the
           * document, so numeric order reproduces the original stream. */
          const own = ownSections(c.spec, c.no);
          if (!own.length) { refuse("noSection"); continue; }
          qstat.checked++;
          if (containsAnyForm(ownText(c.spec, c.no), f)) { qstat.verified++; gapHist.push([q.at, 1]); continue; }
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
          const near = (byUnit.get(proseUnitKey(src, spans, c.at)) || []).find((o) => {
            if (o === c || !o.spec || o.how === "file" || !txt.has(o.spec)) return false;
            return ownSections(o.spec, o.no).length && containsAnyForm(ownText(o.spec, o.no), f);
          });
          /* HOW MUCH OF THIS CHANNEL THE WIDENING IS CARRYING, reported rather than known, because a
           * confirmation channel that silently absorbs findings is the silent zero this file refuses
           * everywhere else — and because the widening is the one thing here that can only ever REMOVE a
           * finding, so its size is the size of the trust a reader is being asked to extend. */
          if (near && spanIdxAt(spans, near.at) !== spanIdxAt(spans, c.at)) qstat.okNearbyCrossLiteral++;
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
          /* AND THE ONE BAND THAT MAY BE ASKED WHETHER THIS TREE WROTE THE SENTENCE, WHICH IS A MEASUREMENT
           * AND NOT A PREFERENCE. The question is asked ONLY where the quotation left the standard inside
           * MIN_FRAGMENT_WORDS — the band this file's own census calls indistinguishable from this tree's
           * prose — and never where some standard's words are positive evidence for the other reading.
           * MEASURED at the revision this landed, on the frozen tree, by asking it of every quotation finding
           * instead: 8 of 55 QUOTE-WRONG-SECTION, 5 of 6 QUOTE-WRONG-STANDARD and 10 of 182
           * QUOTE-NOT-FOUND/MIS-TRANSCRIBED also occur in this tree's own prose, and reading them says why
           * that is not an exemption — `xml_tag.c` quotes XML §3's own sentence and the same file states it
           * again unquoted two paragraphs down; `element.c:1628` mis-transcribes a real HTML sentence its own
           * neighbour paraphrases. Every one of those is a REAL finding about a REAL spec quotation, and a
           * rule that cleared them would be the exemption swallowing the defect. Where the standard has none
           * of the words, it is not evidence about a standard at all. */
          const nf = { ...rec, kind: "QUOTE-NOT-FOUND", div: d, alt };
          quotes.push(nf);
          if (!divergedLate(nf)) ownCands.push({ q: nf, frags: f });
        }
      }
    }

    /* PASS 5 — THE STEP NUMBERS EACH CITATION GOVERNS. It reuses PASS 4's attribution rule rather than
     * restating it: a step reference belongs to the nearest citation BEFORE it, and the region a citation
     * governs ends at the next citation or at the end of its prose unit. Deriving a second rule for which
     * citation owns which number would be the copy this file's own header says drifts.
     *
     * NEAREST-PRECEDING IS A FALLBACK, NOT THE RULE, AND THE TWO DECLARATION MAPS BELOW ARE THE RULE. Where a
     * site STATES which list a step path indexes, that statement decides; proximity is what answers when
     * nothing states it. The distinction is not cosmetic — it is the difference between a finding a reader can
     * act on and one that sends them to the wrong section, which is the same defect as an assert naming a
     * remedy and no site. */
    {
      const admitted = cites.filter((c) => c.admitted);
      /* A UNIT AND NOT A SPAN — `proseUnit` owns the rule; this channel says "the same prose unit" in its own
       * words below and must therefore ask the same reader the other three ask. */
      const byUnit = new Map();
      for (const c of admitted) {
        const k = proseUnitKey(src, spans, c.at);
        if (k < 0) continue;
        if (!byUnit.has(k)) byUnit.set(k, []);
        byUnit.get(k).push(c);
      }
      /* WHAT THE FILE ITSELF SAYS A STEP PATH BELONGS TO. A citation with a step number in its OWN noun phrase
       * (see ADJ_STEP) has declared an owner for that path, and this tree writes the same path in both
       * spellings all the time — a comment opens with the bare label `step 6.9.1:` and the crash message it
       * introduces, five lines down, writes that same label QUALIFIED beside the dispatch section. One of those
       * two spellings carries the section; joining on the PATH is how the unqualified one gets it. This is a
       * join on an identifier, not a distance: nothing here reads how far apart the two spellings are, so it
       * cannot degrade into the proximity guess it replaces.
       *
       * IT CONFIRMS AND NEVER ACCUSES, so widening it can only remove a finding — the discipline the whole
       * channel is built on. What it must not do is confirm VACUOUSLY, and that is what DECLARED_DEPTH is
       * measured against.
       *
       * FOUR OTHER RULES WERE BUILT AND REFUSED ON MEASUREMENT, recorded here because the next person to look
       * at this will reach for them in this order. (1) Confirming against every citation in the enclosing
       * top-level definition silences 50 of 69 NOT-IN-THIS-SECTION entries and confirms a step off a
       * file-header citation hundreds of lines away. (2) Confirming against the adjacent prose units silences
       * 23, and nothing picks one unit over two. (3) Refusing the citation whenever the prose unit numbered a
       * step before it fires on 444 EXISTS rows, and reading them shows it wrong: a comment writing a
       * standard's operation name, its step count, and then that operation's steps one by one is a CORRECT
       * attribution the rule would throw away. (4) Refusing a citation that makes no step claim in its own
       * noun phrase fires on 276 EXISTS rows, and those read as correct too — an owning algorithm is routinely
       * named in English and nowhere in a number, which is exactly the population that escapes confirmation.
       * Every one of the four buys the two true corrections at a cost of hundreds of wrong ones. */
      const declared = new Map();
      for (let i = 0; i < admitted.length; i++) {
        const o = admitted[i];
        if (!o.spec || o.how === "file" || o.foreign) continue;
        const m = ADJ_STEP.exec(governedProse(src, spans, o.at, o.len,
                                              i + 1 < admitted.length ? admitted[i + 1].at : null));
        /* THE KEY IS THE SPELLING AND NOT THE POSITION, folded to one case. A declaration is the author
         * saying which list THIS path indexes into, so `2.i` is declared by `2.i` and not by `2.9` — two
         * spellings that may denote one item still say different things at a site. */
        if (!m) continue;
        const dk = m[1].toLowerCase();
        if (dk.split(".").length < DECLARED_DEPTH) continue;
        if (!declared.has(dk)) declared.set(dk, []);
        declared.get(dk).push(o);
      }
      /* THE HEADING A STEP REFERENCE STANDS UNDER, for a design note — see mdHeadingLevel. Built once per file
       * as [blockStart, blockEnd, citation, declaredPath], so the lookup below is a scan of the few headings
       * that declare a step rather than a re-parse per reference. */
      const headingDecl = [];
      if (/\.md$/.test(file)) {
        const heads = [];
        for (const sp of spans) {
          const lvl = mdHeadingLevel(src.slice(sp[0], sp[1]));
          if (lvl) heads.push({ at: sp[0], end: sp[1], lvl });
        }
        for (let i = 0; i < heads.length; i++) {
          const h = heads[i];
          let stop = src.length;
          for (let j = i + 1; j < heads.length; j++) if (heads[j].lvl <= h.lvl) { stop = heads[j].at; break; }
          for (let k = 0; k < admitted.length; k++) {
            const o = admitted[k];
            if (o.at < h.at || o.at >= h.end) continue;
            if (!o.spec || o.how === "file" || o.foreign) continue;
            const m = ADJ_STEP.exec(governedProse(src, spans, o.at, o.len,
                                                  k + 1 < admitted.length ? admitted[k + 1].at : null));
            if (!m) continue;
            headingDecl.push({ from: h.at, to: stop, cite: o, parts: m[1].toLowerCase().split(".") });
            break;
          }
        }
      }
      const headingOwnerOf = (at, parts) => headingDecl.find((h) =>
        at >= h.from && at < h.to && h.parts.length <= parts.length &&
        h.parts.every((v, i) => v === parts[i]))?.cite || null;
      /* The lists a citation's own section and its subsections hold, joined — the same containment JOIN the
       * quotation check performs on text, for the same reason: the corpus stores each section's OWN slice. */
      /* EVERY LIST IN THE REGION IS A CANDIDATE ROOT, WHICH IS THE CONVENTION HALF OF THIS CHECK AND THE HALF
       * THAT WAS MEASURED WRONG FIRST. The corpus stores a faithful tree; what a reader may be NAMING is a
       * separate question, and the answer this tree demonstrates is that an author cites an algorithm's steps
       * RELATIVE TO THAT ALGORITHM, wherever it sits in the markup. HTML's window event loop runs three steps
       * in parallel whose third queues a task with twenty-three steps in it, and every comment in this tree —
       * correctly, and as Chromium's do — calls the focus fixup `step 17` rather than `step 3.17`. Reading only
       * the section's outermost lists reported five of those as a step §8.1.7.3 does not reach, and the
       * "repair" would have been to invent a prefix the standard never prints beside the number.
       * SO A PATH IS SOUGHT FROM EVERY LIST, and what survives is the claim that no list anywhere under the
       * cited section admits it — which is what this channel says out loud and all it ever says. */
      const flatten = (l, out) => {
        out.push(l);
        if (Array.isArray(l)) for (const a of Object.values(l[1])) for (const x of a) flatten(x, out);
      };
      const listsFor = (spec, no) => {
        const sx = stp.get(spec).sections;
        const out = [];
        for (const n of Object.keys(sx)) if (n === no || contains(no, n)) for (const l of sx[n]) flatten(l, out);
        return out;
      };
      /* THE SAME FLATTEN, CARRYING THE SECTION EACH LIST CAME FROM. A span is an offset into ONE section's own
       * token stream — the corpus stores each section's slice and the audit JOINS containment — so a list
       * reached through a subsection has to remember whose words its numbers index into. Dropping that and
       * reading the spans against the joined text would slice the wrong region of the wrong section and
       * produce a step whose words belong to neither, which is the manufactured finding this file rates as
       * its one unacceptable failure. */
      const rootsFor = (spec, no) => {
        const sx = stp.get(spec).sections;
        const out = [];
        const fl = (l, sec) => {
          out.push({ sec, l });
          if (Array.isArray(l)) for (const a of Object.values(l[1])) for (const x of a) fl(x, sec);
        };
        for (const n of Object.keys(sx)) if (n === no || contains(no, n)) for (const l of sx[n]) fl(l, n);
        return out;
      };
      const itemSpan = (l, i) => (Array.isArray(l) && l.length > 2 && l[2] && Object.hasOwn(l[2], i) ? l[2][i] : null);
      /* THE STANDARD'S OWN VOCABULARY, ONE SET PER STANDARD, built from the same dfn and operation index the
       * term channel resolves on — so a word this gate admits is a word some check here already treats as
       * that standard's. */
      const vocab = new Map();
      const vocabOf = (spec) => {
        if (!vocab.has(spec)) {
          const v = new Set(), ix = idx.get(spec);
          for (const k of [...Object.keys(ix.dfns), ...Object.keys(ix.ops || {})])
            for (const w of k.split(/[^a-z0-9]+/)) if (w) v.add(w);
          vocab.set(spec, v);
        }
        return vocab.get(spec);
      };
      /* IS THIS RUN OF WORDS A TERM THIS STANDARD DEFINES — asked of the same two indexes, so a phrase this
       * admits is a phrase some other check here already resolves citations by. */
      const termOf = (spec, run) => {
        const ix = idx.get(spec);
        return Object.hasOwn(ix.dfns, run) || (ix.ops && Object.hasOwn(ix.ops, run));
      };
      /* HOW MUCH OF A STANDARD A WORD STANDS IN, counted over the same section texts every other word question
       * here is answered from, and cached per word because the gate asks it once per site. */
      const dfCache = new Map();
      const dfOf = (spec, word) => {
        const k = spec + " " + word;
        if (!dfCache.has(k)) {
          const sx = txt.get(spec).sections, tok = " " + word + " ";
          let n = 0, all = 0;
          for (const no of Object.keys(sx)) { all++; if ((" " + sx[no] + " ").includes(tok)) n++; }
          dfCache.set(k, all ? n / all : 1);
        }
        return dfCache.get(k);
      };
      const secWords = new Map();
      const wordsOf = (spec, sec) => {
        const k = spec + " " + sec;
        if (!secWords.has(k)) {
          const s = txt.has(spec) ? txt.get(spec).sections[sec] : null;
          secWords.set(k, s ? s.split(" ") : null);
        }
        return secWords.get(k);
      };
      /* EVERY LIST UNDER THE SECTION THAT HAS AN ITEM AT THIS PATH — one entry per READING the corpus admits,
       * for the same reason the existence check seeks a path from every list: nothing at the site says WHICH
       * algorithm, and picking one would be the invented-citation defect. The consequences of that are
       * asymmetric here and both are wanted — ONE reading that admits the claim silences it, and a finding
       * needs one reading that demonstrates it. */
      /* A READING IS PART OF WHAT A SITE IS — see STEP_NO. `2.i` resolves to a DIFFERENT item under the
       * alphabetic and the roman reading, so a site carries the position it reached; without that the content
       * check would test one list against the other reading’s item. */
      const stepSites = (spec, no, path) => {
        const sites = [];
        const last = path[path.length - 1];
        for (const { sec, l } of rootsFor(spec, no)) {
          let cur = [l];
          for (let k = 0; k < path.length - 1 && cur.length; k++) {
            const nx = [];
            for (const c of cur) for (const v of path[k]) { const kids = listKids(c, v); if (kids) nx.push(...kids); }
            cur = nx;
          }
          for (const c of cur) for (const v of last) if (itemSpan(c, v)) sites.push({ sec, list: c, item: v });
        }
        return sites;
      };
      for (let i = 0; i < admitted.length; i++) {
        const c = admitted[i];
        const stop = i + 1 < admitted.length ? admitted[i + 1].at : null;
        const prose = governedProse(src, spans, c.at, c.len, stop);
        for (const s of stepRefs(prose)) {
          sstat.seen++;
          if (s.path.length > 1) sstat.sub++;
          if (s.parts.some((t) => !/^[0-9]+$/.test(t))) sstat.lettered++;
          /* A COMPONENT THAT DENOTES NO POSITION IS NOT A STEP THIS CHECK MAY ACCUSE OR CONFIRM. It is asked
           * FIRST, ahead of every corpus question, because it is a fact about the SPELLING and attributing it
           * to a missing index would file it under somebody else’s gap. */
          if (s.path.some((cands) => !cands.length)) { sstat.unreadable++; continue; }
          /* THREE SILENCES, KEPT APART, because merging them is the defect CLAUDE.md names and because two of
           * them are not the same KIND of silence at all. A citation that NAMES a standard nothing here indexes
           * is unchecked and stays unchecked — but it is also the population that has been measured being
           * MISJUDGED by the checks that do run, when a file vote hands it to a standard that happens to own a
           * section by that number and the words are then compared against a document the author never cited.
           * A citation naming NO standard is a different fact, and one whose standard only a file vote placed is
           * a third. This channel refuses the vote outright — THE FILE VOTE MAY RESOLVE AND MAY NOT JUDGE, this
           * file's own division of labour, applied a third time — so a mis-vote cannot become a step finding
           * here; the count is printed so that refusal is visible rather than assumed. */
          /* A CITATION ALREADY REPORTED WRONG IS NOT ASKED ABOUT ITS STEPS — see where `flagged` is set. */
          if (c.flagged) { sstat.citeFlagged++; continue; }
          if (c.foreign) { sstat.foreign++; stepForeignBy.set(c.anchor.slice(6), (stepForeignBy.get(c.anchor.slice(6)) || 0) + 1); continue; }
          if (!c.spec) { sstat.unresolved++; continue; }
          if (c.how === "file") { sstat.voted++; continue; }
          if (!stp.has(c.spec)) {
            if (stpStale.has(c.spec)) sstat.staleCorpus++;
            else { sstat.noCorpus++; stepNoCorpusBy.set(c.spec, (stepNoCorpusBy.get(c.spec) || 0) + 1); }
            continue;
          }
          const lists = listsFor(c.spec, c.no);
          if (!idx.get(c.spec).sections[c.no]) { sstat.noSection++; continue; }
          /* A SECTION WITH NO NUMBERED LIST AT ALL IS NOT A SECTION THIS CHECK MAY ACCUSE. It is the shape a
           * citation takes when the SECTION is right and the ALGORITHM the step belongs to is written one
           * heading away — prose naming an interface, then a step of the operation that interface calls — and
           * accusing it would be this channel asserting something it cannot demonstrate. Counted, never
           * judged. */
          if (!lists.length) { sstat.noList++; continue; }
          sstat.checked++;
          const fail = stepFail(lists, s.path);
          if (!fail) {
            sstat.exists++;
            /* ADMITTED BY THE UNION IS NOT ADMITTED BY EVERY READING — see stepReadings. Where the two
               readings of a letter disagree, the citation stands on a convention the corpus does not record,
               and saying so is the whole difference between an absent check and a clean one. */
            if (s.path.some((cands) => cands.length > 1)) {
              const rd = stepReadings(s.path);
              if (!rd || rd.some((r) => stepFail(lists, r))) sstat.letterSplit++;
            }
            /* THE STEP EXISTS — SO ASK THE SECOND QUESTION. See STEP_CLAIM: only a POSSESSIVE is read as a
             * claim about the step's content, and only the standard's own words answer it.
             *
             * FOUR THINGS DECIDE IT, AND EVERY ONE ERRS TOWARD SILENCE.
             *   (1) THE PHRASE MUST BE ONE THE AUTHOR BOUNDED — a bare word, or a run the standard itself
             *       defines as a term. See STEP_CLAIM for why anything else is a sentence and not a claim.
             *   (2) A BARE WORD MUST BE THE STANDARD'S OWN VOCABULARY AND MUST NOT BE UBIQUITOUS IN IT — see
             *       DF_CAP. Anything else is this tree's word for something, and a word the standard never
             *       uses to name anything cannot say which step it belongs to. A DEFINED TERM needs neither
             *       gate: the standard has already said it names something, and a multi-word phrase is not
             *       the shape a common word takes.
             *   (3) ONE ADMITTING READING SILENCES THE SITE. Where the cited step uses the word under ANY of
             *       the readings the corpus admits, the claim checks out and nothing is reported — nothing at
             *       the site says which list the author meant, so no reading may be preferred.
             *   (4) AN ACCUSATION MUST NAME WHERE THE WORD IS. The finding is `step N does not say X and step
             *       M does`, which is a repair; `step N does not say X` alone is true of almost everything and
             *       actionable for nothing.
             * THE ATTRIBUTION RESIDUAL, NAMED because it is the one direction this can be wrong in: the
             * section is the nearest-preceding citation's, as everywhere in this pass, so a comment crediting
             * one section while stepping through another's algorithm could be accused. It is a far smaller
             * exposure here than in the existence check, because the word has to be that standard's vocabulary
             * AND stand in the cited section's own list before a finding is possible at all. What would show
             * it is a finding whose named rival step is unrelated to the comment's subject. */
            const cm = STEP_CLAIM.exec(prose.slice(s.at + s.raw.length));
            const pt = cm ? tokenText(cm[1]).split(" ").filter(Boolean) : [];
            if (!pt.length) { sstat.claimNone++; continue; }
            sstat.claimSeen++;
            /* USE VERSUS MENTION, ASKED AT THE STEP AND NOT AT THE CITATION — the same rule mentionNotClaim
             * states, reached through its own function so there is one copy of it. A step claim is disclaimed
             * exactly as a section citation is, and this tree writes the shape: a parenthetical recording that
             * two numbers STOOD as something, one of them a possessive step claim, is a retirement note doing
             * what CLAUDE.md requires and not a claim anybody is making. Accusing it would be this channel
             * committing the defect the whole file exists to find. */
            const mention = mentionOf(precedingProse(src, spans, c.at) + prose.slice(0, s.at), prose.slice(s.at));
            if (mention) { sstat.claimMention++; continue; }
            if (stpNoPos.has(c.spec) || !txt.has(c.spec)) { sstat.claimNoPos++; continue; }
            let ev = "";
            for (let k = Math.min(TERM_WORDS, pt.length); k >= 2; k--) {
              const run = pt.slice(0, k).join(" ");
              if (termOf(c.spec, run)) { ev = run; break; }
            }
            if (!ev && pt.length === 1 && vocabOf(c.spec).has(pt[0]) && dfOf(c.spec, pt[0]) < DF_CAP) ev = pt[0];
            if (!ev) { sstat.claimNotTerm++; continue; }
            const tok = " " + ev + " ";
            let best = null, admitted = false, readable = 0;
            for (const site of stepSites(c.spec, c.no, s.path)) {
              const w = wordsOf(c.spec, site.sec);
              if (!w) continue;
              const items = Object.keys(site.list[2]).map(Number);
              if (!items.length) continue;
              readable++;
              const textAt = (i) => { const sp = itemSpan(site.list, i); return sp ? " " + w.slice(sp[0], sp[1]).join(" ") + " " : ""; };
              const hits = items.filter((i) => textAt(i).includes(tok));
              if (hits.includes(site.item)) { admitted = true; break; }
              if (hits.length && hits.length <= NAME_CAP && (!best || hits.length < best.hits.length))
                best = { hits, sec: site.sec };
            }
            if (admitted) { sstat.claimOk++; continue; }
            if (!readable) { sstat.claimNoPos++; continue; }
            if (!best) { sstat.claimUnseen++; continue; }
            /* TWO BANDS, AND ONLY ONE OF THEM IS A CLAIM ABOUT THE CITATION — the same asymmetry the existence
             * check draws, drawn on the same evidence and for the same reason.
             *   ONE READING: the section holds exactly one list that reaches this step, so `step K` names ONE
             *     step, the tool can say what that step says, and the rival it names stands in the same list —
             *     which is what makes the finding a repair rather than an observation.
             *   SEVERAL: `step K` names as many different steps as there are lists, and CLAUDE.md's own rule
             *     for that is that every candidate reading confirms it and none can be accused. It is not only
             *     unfalsifiable, it MIS-ADDRESSES: the rival is picked from whichever list happens to hold the
             *     term, which can be a different algorithm entirely.
             * MEASURED BEFORE THE SPLIT WAS CHOSEN, by reading every row of both bands against the fetched
             * standards. In the several-reading band three of ten were real; in the one-reading band the only
             * row that was not real was a retirement note, which is now disclaimed one gate up. The band that
             * is refused is COUNTED and LISTED under --steps, never accused — and it holds real defects a
             * human can act on, which is exactly why it is printed rather than dropped. */
            const where = best.hits.length === 1
              ? `step ${best.hits[0]} of` : `steps ${best.hits.join(" and ")} of`;
            const verb = best.hits.length === 1 ? "does" : "do";
            const rec = {
              file: relative(ROOT, file), line: lineOf(c.at), no: c.no, spec: c.spec, how: c.how,
              step: s.raw, depth: s.path.length, word: ev, crash: inCrashMessage(src, spans, c.at),
              msg: `${c.spec} §${c.no} step ${s.raw} does not use \`${ev}\` — ${where} ` +
                   /* A MULTIPLICITY IS NOT ALWAYS SEVERAL LISTS — since a lettered component is read under
                      every position its spelling denotes (see STEP_NO), one list can reach `2.i` at two items,
                      and calling that "2 lists" would be the tool describing a state it is not in. */
                   (readable === 1 ? `the one step under it that \`${s.raw}\` can name ${verb}`
                                   : `one of the ${readable} steps under it that \`${s.raw}\` can name ${verb}, and which of them the citation means is not stated`) +
                   (best.sec === c.no ? "" : ` (that list stands in §${best.sec})`),
              text: prose.slice(Math.max(0, s.at - 60), s.at + 60).trim() };
            if (readable === 1) { sstat.claimOut++; stepsSays.push(rec); }
            else { sstat.claimAmbig++; stepsSaysAmbig.push(rec); }
            continue;
          }
          /* A NUMBER THE AUTHOR WROTE IN THIS SAME COMMENT IS A NUMBER THE AUTHOR CITED — the rule PASS 4
           * already states, and it matters more here, because a step is routinely written BEFORE the § it
           * belongs to (`step 3 of §4.9`) and nearest-preceding then charges the wrong section. A path some
           * other citation in the same prose unit admits is CONFIRMED, counted apart from a direct one. */
          /* A CONFIRMATION MAY QUANTIFY OVER A RESOLUTION A FINDING MAY NOT REST ON, and the two are not in
           * tension — they are the same asymmetry this file applies everywhere: a vote may not ACCUSE, and
           * using one to WITHHOLD an accusation errs in the safe direction. The quotation check's own
           * OK-NEARBY excludes a voted neighbour and that exclusion cost real silence here: `navigation.h`
           * names §7.2.6.10.4 and §7.2.6.8 in one comment, says in words that the number belongs to "the
           * inner algorithm", and §7.2.6.10.4 — resolved by the file's vote — carries the 33-step list that
           * admits every one of steps 23, 28, 30 and 31.
           * AND WHERE THE NEIGHBOUR'S OWN RESOLUTION IS PLAINLY BROKEN THE NUMBER IS STILL EVIDENCE. A vote
           * that hands `§4.13.4` to a standard with no such section has not told us which standard the author
           * meant; it has told us the vote failed. Asking every indexed standard that HAS that number is what
           * a confirmation is allowed to do, and it is the difference between suppressing one measured false
           * accusation and printing it: `custom_elements.c` writes `§4.13.4 step 14's lifecycleCallbacks` and
           * `step 14.4` four lines apart, and HTML §4.13.4's step 14 carries the thirteen sub-steps that read
           * the prototype. */
          const admitsPath = (key, no) =>
            stp.has(key) && idx.get(key).sections[no] &&
            (() => { const ol = listsFor(key, no); return ol.length && !stepFail(ol, s.path); })();
          /* AND THE NUMBER STANDING IMMEDIATELY BEFORE THE STEP IS THE OWNER THE AUTHOR NAMED, admitted as a
           * citation or not. OK-NEARBY below quantifies over the CITATIONS in this prose unit, so it cannot see
           * a bare number PASS 3 declined for want of group evidence — and `27.5.1.3 step 2.f`, written with no
           * §, is exactly that number. Reading it as the owner is the same asymmetry stated at OK-NEARBY: a
           * resolution a finding may not REST on may still WITHHOLD one, and the evidence here is stronger than
           * a neighbourhood, because the author put the number and the step in one noun phrase.
           * THIS WAS MEASURED AS A FALSE-ACCUSATION SOURCE THE MOMENT LETTERED COMPONENTS BECAME READABLE: three
           * sites write `27.5.1.3 step 2.f` for the `then` read, and nearest-preceding charged that step to
           * whatever § the paragraph had cited first (§27.5.4.7, Web IDL §3.7.10.2, Streams §4.9.5) — not one of
           * which has a list reaching that sub-step, so each accusation was sound about the section it named
           * and wrong about whose step it was. (That clause is written without the step's own spelling on
           * purpose: this channel has no use-versus-mention gate, so a worked example spelled out after a §
           * becomes a finding against the file that documents it — the convention this tool already states, and
           * obeying it here is cheaper than widening a detector until it swallows what it protects.) It cannot silence a citation about its OWN number: `lead !== c.no` is what keeps
           * `§N step K` in front of the checker. */
          const lead = OWN_LEAD.exec(prose.slice(0, s.at));
          if (lead && lead[1] !== c.no && [...idx.keys()].some((k) => admitsPath(k, lead[1]))) { sstat.okLead++; continue; }
          const near = (byUnit.get(proseUnitKey(src, spans, c.at)) || []).find((o) => {
            if (o === c) return false;
            if (o.spec && idx.has(o.spec) && idx.get(o.spec).sections[o.no]) return admitsPath(o.spec, o.no);
            return [...idx.keys()].some((k) => admitsPath(k, o.no));
          });
          if (near) { sstat.okNearby++; continue; }
          /* AND THE SAME PATH WRITTEN QUALIFIED ELSEWHERE IN THIS FILE IS THE OWNER THE SITE STATED, which is
           * a stronger fact than any neighbourhood: OK-NEARBY quantifies over what happens to be standing
           * nearby, and this quantifies over what the author WROTE about this exact list position. It is what
           * separates the two accusations this channel used to make. Measured, it moves two rows and no
           * others: a dispatch walk whose comment credits the get-the-parent section for a term while the
           * number it writes belongs to the algorithm named two lines up, and a custom-element reaction
           * comment naming a sub-step the same file declares eight times beside the section that defines it. */
          const declOwner = [s.parts.join("."), s.parts.slice(0, -1).join(".")]
            .filter((k) => k && k.split(".").length >= DECLARED_DEPTH)
            .flatMap((k) => declared.get(k) || [])
            .find((o) => !(o.spec === c.spec && o.no === c.no) && admitsPath(o.spec, o.no));
          if (declOwner) { sstat.okDeclared++; continue; }
          /* A HEADING DECIDES BOTH WAYS, AND THE SECOND WAY IS WHY THIS IS AN ATTRIBUTION FIX RATHER THAN
           * ANOTHER CONFIRMATION CHANNEL. Where a design note's heading declares the owner of this path, that
           * owner is what the finding is asked about — confirmed if it admits the path, and NAMED if it does
           * not. The finding that made this necessary read "html §4.12.3 has a step 6 and no list under it",
           * which is true and useless: the paragraph borrows that section for the template cloning steps while
           * the number belongs to the clone algorithm its own heading names, section, title and step. The step
           * is out of range under BOTH, so nothing about the verdict changes and everything about where the
           * reader is sent does. Measured over every step reference in every design note: one row is reworded,
           * none change band, and no row that existed becomes a finding. */
          const hOwner = headingOwnerOf(c.at, s.parts);
          let owner = c, ofail = fail;
          if (hOwner && !(hOwner.spec === c.spec && hOwner.no === c.no) && stp.has(hOwner.spec) &&
              idx.get(hOwner.spec) && idx.get(hOwner.spec).sections[hOwner.no]) {
            const hl = listsFor(hOwner.spec, hOwner.no);
            if (hl.length) {
              const hf = stepFail(hl, s.path);
              if (!hf) { sstat.okDeclared++; continue; }
              owner = hOwner; ofail = hf; sstat.reworded++;
            }
          }
          /* TWO FAILURES, AND ONLY ONE OF THEM IS A CLAIM ABOUT THE CITATION. This is the same asymmetry
           * check (1) draws when it refuses to ask whether a term-resolved standard HAS the cited number: a
           * finding may assert only what it can demonstrate, and "no list under §N reaches step K" does not
           * demonstrate that §N is the wrong section — it is equally consistent with the citation being RIGHT
           * about the section while the step belongs to an algorithm the prose names IN WORDS one heading
           * away, which is how this tree's comments are written. Measured, by reading them: of the
           * uncorroborated band, `range.c`'s `§4.4's clone a node, PERFORMED — there are six of them here
           * (steps 4, 16, 17, 19 and 20…)` numbers the EXTRACT algorithm's steps under a citation of `clone a
           * node` that is exactly right; `node.c` writes bare `STEP 13`/`STEPS 19-20` markers governed by
           * whatever § came last; `navigation.h` says outright "the inner algorithm's step 23"; `idb_get_all.h`
           * says "step 9 is two conversions in sequence" about getAll while citing §2.9 for one of them. Every
           * one is correct as written and a finding against it would name no repair.
           * A CORROBORATED FAILURE IS A DIFFERENT CLAIM: the path's own prefix EXISTS in the cited section, so
           * that section demonstrably holds the list the citation indexes into, and the number that runs off
           * its end is wrong about that list. That band was read too — Web IDL §3.12's `step 10.2/10.4/10.5`,
           * where `call a user object operation` is §3.11 and its step 10 has exactly the five sub-steps; the
           * eight IndexedDB §5.7 sites whose 9.x are the 10.x of `upgrade a database`; DOM §2.9's dispatch
           * cluster, where 6.9.7 is a leaf and the walk's `set target to parent` is 6.9.8.1; `initialize a
           * response`'s six steps under Fetch §5.5's `step 8.1`; and §4.10.21.1 "Definitions", which holds no
           * algorithm at all while the `invalid` event is fired by §4.10.21.2's.
           * SO THE UNCORROBORATED BAND IS COUNTED AND LISTED AND NEVER ACCUSED — visible where it used to be
           * silent, which is what this file does with every population it cannot judge. */
          /* THE RECORD IS ABOUT THE OWNER, NOT ABOUT WHATEVER STOOD NEAREST — `owner` is the citation when
           * nothing declared otherwise, so the unreworded case is byte-identical to what this printed before. */
          const rec = { file: relative(ROOT, file), line: lineOf(c.at), no: owner.no, spec: owner.spec, how: owner.how,
                        step: s.raw, depth: s.path.length, gap: s.at,
                        crash: inCrashMessage(src, spans, c.at),
                        msg: stepMsg(ofail, s.parts, owner.no, owner.spec) +
                             (owner === c ? "" : ` (the paragraph's own citation is ${c.spec} §${c.no}; the block's heading declares the owner)`),
                        text: prose.slice(Math.max(0, s.at - 60), s.at + 60).trim() };
          if (ofail.depth > 0 || ofail.sub) { sstat.out++; stepsOut.push(rec); }
          else { sstat.away++; stepsAway.push(rec); }
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
  /* AND THE QUOTATIONS THIS CHECK CANNOT JUDGE ARE TAKEN OUT OF THE FINDINGS HERE, WHERE THE WALK IS OVER AND
   * THE WHOLE CANDIDATE SET IS IN ONE HAND. They leave `quotes`, so they leave `quoteFindings`, so they leave
   * --since and the total — a site whose words this tree wrote is not a defect anybody can repair at that
   * site, and leaving it in the count is what teaches a reader to skim the band it sits in.
   * THE TOTAL FALLS AND THE DENOMINATOR DOES NOT, WHICH IS THE ONE DIRECTION CLAUDE.md SAYS INVITES NO
   * SCRUTINY, so the census prints this population's size beside the NOT-FOUND figure it came out of and the
   * band below prints every one of its sites. `qstat.checked`, `qstat.verified` and `qstat.notFound` are the
   * PASS's own counts and are deliberately not adjusted: they state what was compared and what the comparison
   * answered, and this is a later question asked of a subset of one of them. */
  const ownProse = [];
  treeAuthored(ownCands);
  {
    const kept = quotes.filter((q) => (q.authoredAt ? (ownProse.push(q), false) : true));
    quotes.length = 0;
    for (const q of kept) quotes.push(q);
    qstat.ownProse = ownProse.length;
    qstat.ownProseCrash = ownProse.filter((q) => q.crash).length;
  }
  /* A QUOTATION FINDING IS A FINDING, SO --since MUST SEE IT — and this line is what makes "run it on what you
   * write" reach the axis a lane is most likely to get wrong. A fabricated sentence is written by the person
   * writing the comment, in the diff they are landing, which is exactly the population --since reports; leaving
   * quotations out of the returned set would have left the routine channel blind to the one check CLAUDE.md
   * calls the one a reader trusts most and verifies least. `qtext` joins the key because two different
   * quotations under one citation can diverge identically, and a delta that could not tell them apart would
   * report a repaired one as still standing. */
  const quoteFindings = quotes.map((q) => ({ ...q, msg: quoteMsg(q), text: `"${q.quote}"`, qtext: q.quote }));
  /* AND THE AGREEMENT CHANNEL, WHOSE FINDING IS A GROUP AND WHOSE FINDINGS ARE ITS SITES. A disagreement is
   * repaired at ONE site and the other members of the group go with it, so a per-group count would understate
   * what a reader has to read and a per-site count states exactly the places a reader must look. Each row
   * carries the group's other spellings in its own message, because a finding that says "this disagrees with
   * something" and does not say WITH WHAT is one the reader has to re-derive.
   *
   * AND --since SEES IT, WITH A LIMIT THAT MUST BE STATED RATHER THAN DISCOVERED. The delta mode audits only
   * the files a diff touched, and this channel's evidence is OTHER SITES — so a disagreement whose sibling
   * stands in an untouched file forms in neither the base run nor the tip run and is reported by neither. That
   * is a COVERAGE hole and not a wrong answer: both runs read the same file set, so nothing can be reported as
   * introduced that was not, and what --since does catch is the shape that put this channel here — one diff
   * relocating or re-typing a sentence it already quotes elsewhere in the same diff. The full run is where the
   * whole-tree question is asked, and the report says so in the channel's own header. */
  const agreement = agreementPassages(agreeSeen);
  const clipQ = (t) => (t.length > 110 ? t.slice(0, 110) + "…" : t);
  /* WHICH STANDARD, IF ANY, CARRIES THIS SPELLING ANYWHERE IN ITS OWN TEXT — the evidence that separates the two
   * populations the text relation alone cannot, and the reason this channel is small enough to read.
   *
   * A NEAR-IDENTICAL PAIR IS ONE OF TWO THINGS AND THE CORPUS KNOWS WHICH. Either both spellings are REAL
   * sentences a standard writes in parallel — one per attribute, one per union arm, `preferred width` beside
   * `preferred minimum width` — in which case nothing is wrong and accusing them is this file committing its own
   * subject; or exactly ONE of them is any indexed standard's words, and the other is the mis-transcription its
   * own sibling names the repair for. That is a fact with its own proof, which is the form this file's header
   * endorses over an inference, and it is asked over the WHOLE of each standard rather than the cited section,
   * because a disagreement is a claim about the WORDS and says nothing about anybody's number.
   * IT IS ASKED ONLY OF THE CANDIDATE PASSAGES, not of all 7000-odd spellings: a whole-corpus scan is a
   * multi-megabyte indexOf and the population that reaches here is two orders of magnitude smaller.
   * AND IT IS GENEROUS ON PURPOSE — every site's own fragment forms are tried, and the first corpus that holds
   * any of them answers — because ABSENCE is the half that accuses, and this file pays its errors in recall. */
  const holdsCache = new Map();
  const heldBy = (sp) => {
    if (holdsCache.has(sp.key)) return holdsCache.get(sp.key);
    let at = null;
    for (const [k] of txt) {
      if (sp.sites.some((site) => containsAnyForm(wholeOf(k), site.frags))) { at = k; break; }
    }
    holdsCache.set(sp.key, at);
    return at;
  };
  const agreeGroups = [], agreeBoth = [], agreeNeither = [];
  for (const g of agreement.candidates) {
    const ev = g.map((sp) => ({ sp, at: heldBy(sp) }));
    const present = ev.filter((e) => e.at), absent = ev.filter((e) => !e.at);
    if (present.length && absent.length) agreeGroups.push({ g, ev, present, absent });
    else if (present.length) agreeBoth.push(g);
    else agreeNeither.push(g);
  }
  /* THE FINDING IS THE SITE THAT HAS TO BE EDITED, AND THE GROUP IS PRINTED WHOLE SO A READER CAN SEE WHY.
   * CLAUDE.md's instruction is to report a disagreement as a disagreement rather than as a verdict, and what is
   * reported here IS one — the row's claim is that this spelling occurs in NO indexed standard while a SIBLING
   * SITE quoting the same passage carries one that does, which is a statement about two sites and a corpus and
   * not a ruling. The two ways it can still be wrong are named in the row rather than left to be discovered:
   * the site's real standard may be one this tool indexes no text for, and the committed corpus can be OLDER
   * than the document its editors are writing — the fourth reading of a failed quotation, where the diligent
   * author who pasted current bytes is the one who reads as wrong. */
  const agreeFindings = [];
  for (const { g, present, absent } of agreeGroups) {
    const total = g.reduce((n, sp) => n + sp.sites.length, 0);
    for (const { sp } of absent) {
      for (const site of sp.sites) {
        const { frags, ...row } = site;
        agreeFindings.push({ ...row, kind: "QUOTE-DISAGREEMENT",
          msg: `this tree quotes this passage ${g.length} ways at ${total} site(s) and this spelling is in NO indexed` +
               ` standard, while ${present.map((e) => `${e.at} has "${clipQ(e.sp.key)}"`).join("; ")}` +
               ` — the sibling names the repair; a standard this tool indexes no text for, or a corpus older than the` +
               ` live edition, are the two ways this row can still be wrong`,
          text: `"${site.quote}"`, qtext: site.quote });
      }
    }
  }
  /* A STEP FINDING IS A FINDING, SO --since MUST SEE IT, for the reason the line above gives about quotations:
   * the number a lane gets wrong is the number that lane is writing right now. It needs no extra key field the
   * way a quotation does: one citation can carry several step references, and each one's `msg` names its own
   * step, so the delta key already tells them apart. The REFUSED band is deliberately not returned — it is not a finding, and
   * handing it to a delta channel would turn every one of those sites into a red in the first diff to touch
   * the file. */
  /* AND THE CONTENT FINDING TRAVELS WITH IT, for the same reason and one more: a wrong step is written by the
   * lane writing the comment, in the diff it is landing, and this is the axis that lane has no other way to
   * check — the number resolves, the section is right, and only the standard's own words disagree. */
  const stepFindings = [...stepsOut.map((v) => ({ ...v, kind: "STEP-OUT-OF-RANGE" })),
                        ...stepsSays.map((v) => ({ ...v, kind: "STEP-SAYS-OTHERWISE" }))];
  if (opts.quiet) return [...findings, ...quoteFindings, ...agreeFindings, ...stepFindings];
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
  /* THE EXCLUSION IS REPORTED WITH A NUMBER BECAUSE ITS OWN ERROR RUNS IN THE DENY DIRECTION — see the
   * quoted-number paragraph in PASS 1. A number this reader declines is a number no channel below judges, so
   * a silent exclusion would be the same invisibility it exists to end, one door over. */
  console.log(`  ${stat.quotedNumber} further number(s) stand INSIDE a quotation and are not citations this tree wrote — the quoted document's own cross-references. Admitting one ends the region the real citation governs at a point inside a quoted run, and the quotation then reaches the quotation check as a fragment with no closing mark and is judged by nothing`);
  /* AND WHAT THE DEFAULT DOES NOT COLLECT IS PRINTED BESIDE WHAT IT DID, because a scope a reader cannot see
   * is the same silent zero as a list a reader cannot see, one level out: a file nobody points the audit at
   * produces no finding, and a total over the files it DID read looks complete. `extension/` was outside every
   * run this tool has ever made until the line that adds it, and the gates are outside deliberately — with a
   * measured reason and a named blocker, which is the difference between a stated limit and an omission. */
  if (!opts.files && !targets.length)
    console.log(`  NOT COLLECTED by this default run: engine/qjs beyond quickjs.c/.h and engine/lexbor — upstream, not this tree's to answer for; ` +
      `and generated bytes under out/ and .work/, which nobody wrote. Everything this project's own hands typed is collected, gates included.`);
  console.log(`  resolved: ${stat.anchored} by their own anchor, ${stat.byTerm} by the term they name, ` +
    `${stat.byTitle} by a section TITLE they state that only one standard uses AND that that standard numbers beside the cited § ` +
    `(the neighbourhood half is not caution — without it a title whose owner this audit does not index resolves to whichever indexed standard happens to reuse the heading)`);
  console.log(`  INFERRED: ${stat.byFile} name no standard, no term and no title, and were placed by their file's dominant anchor — a guess, ` +
    `so nothing below judges them (${stat.titleRefused} title check(s) and ${stat.numberRefused} number-exists check(s) refused on that ground); ` +
    `--unanchored lists them`);
  console.log(`  ${stat.other} belong to a standard this audit does not index; ${stat.skipped} name no standard and no term it knows`);
  console.log(`  audited by standard (in parentheses, how many of them only a file vote placed there): ` +
    `${[...byKey].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}(${byKeyVoted.get(k) || 0})`).join(" ")}`);
  console.log(`  ${stat.confirmed} confirmed (${stat.confirmedByContainment} by a subsection of the cited number, ${stat.confirmedByUse} by a prominent use rather than the definition site, ` +
    `${stat.confirmedByText} by the cited section's OWN WORDS using the term where its markup links it fewer than the floor, ` +
    `${stat.confirmedByIdent} by the cited section spelling the same concept as ONE IDENTIFIER rather than as spaced words, ` +
    `${stat.confirmedByRun} by another number in the same citation's own run), ` +
    `${stat.unverified} carry no title and no term any index knows, ${stat.multiSpec} name a term more than one standard defines`);
  console.log(`  ${stat.foreignTerm} name a term only ANOTHER standard defines, so the standard they cite numbers nothing this audit could hold them to`);

  /* THE TITLE CHANNEL STATES WHAT IT CONSUMED, AND THE REASON IT MUST IS THIS FILE'S OWN RULE RATHER THAN A
   * NEW ONE. Check (1) counts the number-exists checks a file vote refused, check (4) counts the title checks
   * it refused, and the sentence at each says why: a check that silently declines to run is the silent zero
   * this whole file is written against. The LARGEST refusal this audit makes had no counter at all — a stated
   * title ENDS the site, so `stat.confirmed` pooled it with a term confirmation and nothing anywhere said how
   * much of the corpus the term check was never asked about. That is CLAUDE.md's coverage rule exactly: a
   * figure states what it is a fraction of, or it is not a coverage figure.
   * AND THE INCENTIVE IS WHY IT IS PRINTED RATHER THAN MERELY KNOWN. Writing a correct title beside a number is
   * what CLAUDE.md asks authors to do, and doing it MOVES a citation out of the reported population without
   * changing anything about whether its attribution is loose — so a tree that adds titles gets a quieter report
   * for it. Silence there would make this audit reward a cosmetic edit; the numbers below make the trade
   * visible instead, which is the only honest form the trade has. */
  console.log(`  ${stat.titled} were confirmed by the section TITLE they state, and a stated title ENDS the site — the term check below is not asked of them, deliberately (see check (2)).`);
  console.log(`    THE COST, so this report is not a fraction of a population it does not name: ${stat.titledEv} of those name a term this audit knows, ` +
    `and on that same evidence the term check would have CONFIRMED ${stat.titledOK}, REPORTED ${stat.titledMis}, and refused ${stat.titledEv - stat.titledOK - stat.titledMis} as another standard's vocabulary — ` +
    `${stat.titledMisInTitle} of which name a phrase sitting INSIDE the very title that confirmed them, which is the false positive the title-first order exists to suppress`);
  console.log(`    AND ${stat.titledQuoted} state that title IN QUOTES, which aims the term probe at the TITLE itself, so the prose AFTER the quote reaches no lookup at all: ` +
    `${stat.titledTailEv} of those trailing phrases are terms this audit knows and ${stat.titledTailMis} of them stand at a number no standard places them at. ` +
    `That is where one claim in two spellings gets two verdicts: a citation written \`Fetch §N "Fetch methods"' network error\` is confirmed by its title, ` +
    `and the same claim written \`§N's network error\` is REPORTED above — so writing a correct title down moves a site out of that count without changing what it claims`);

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

  /* AN ELISION IS DISCLOSED IN THE AXIS ITS ITEMS ARE KEYED BY, NEVER ONLY AS A TOTAL — and the line this
   * replaces obeyed the second half of that sentence and not the first. `… 348 more (--all)` is a TRUE
   * statement about a POPULATION, and the question every reader of this tool actually asks is about a FILE:
   * does the file I am editing carry a finding? A total cannot answer that, so the answer a grep of the
   * default run returns is ZERO — the silent-zero shape this file names everywhere else, arriving in the
   * auditor itself, where it is worse than anywhere: a zero from the thing whose job is to disagree reads as
   * agreement. It has already been paid for. A lane read a default run, found no `host/main.c` line in it,
   * and reported that queue as finished; `node engine/citegen.mjs engine/host/main.c` prints three findings
   * and fourteen undecided in 0.7 seconds, and the default run mentions that file nowhere.
   *
   * MEASURED at the revision this was written: 319 files carry at least one finding and 111 of them appear in
   * the default output — 208 files answer a grep of it with a zero that is a property of the instrument. And
   * the elision is not a sample. The head is in file-walk order, so it is an ALPHABETICAL CUT: of 468
   * MISATTRIBUTED, the printed 120 are core/dom, core/css and core/events, while the whole of core/html (45),
   * core/frame (73), core/streams (35), core/layout (29) and the fork's own quickjs.c (82) are absent from
   * the default report of that population — not one line. A reader of the default run would conclude this
   * tree's citation problems live in three directories.
   *
   * SO THE BODIES STAY CAPPED AND THE KEYS DO NOT. The readability argument the header makes above is an
   * argument about BODIES — two lines per finding, one of them a hundred characters of source text, and a
   * category that size is read once and never again. That argument is sound and it was silently extended to
   * the KEYS, which cost one short token apiece: the roll below is ~90 names where the bodies it stands for
   * are ~700 lines. The roll is UNCAPPED, because a capped roll is this same defect one level in.
   *
   * AND IT NAMES THE CHEAP EXACT ANSWER. A path argument audits that path alone, prints all of it, and costs
   * under a second against the full run's forty-four — the mode a reader standing in one file should use, and
   * the one thing this report has never said out loud. */
  const named = new Set();
  const head = (all, cap) => { const s = all.slice(0, cap); for (const f of s) named.add(f.file); return s; };
  /* THE ROLL IS BOUNDED WHERE THE RESIDUE IS BIG ENOUGH THAT ITS TAIL ANSWERS NOTHING. The roll exists so a
   * reader can see WHERE a residue lives; four hundred entries of `=1` on one line do not answer that, they
   * bury the report — the muting this file is written against, arriving through the line that reports the
   * elision. `rollCap` is opt-in and unset everywhere it was unset before, so every channel that had a
   * complete roll still has one, and only a band big enough to need the cap carries it. */
  const elided = (all, cap, what, rollCap = Infinity) => {
    const rest = all.slice(cap);
    if (!rest.length) return;
    const by = new Map();
    for (const f of rest) by.set(f.file, (by.get(f.file) || 0) + 1);
    const roll = [...by].sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : 1));
    console.log(`  … ${rest.length} more ${what} NOT PRINTED — the head above is in file order, not a sample. ` +
      `They stand in ${by.size} file(s)${roll.length > rollCap ? `, the ${rollCap} carrying most of them` : ``}: ` +
      roll.slice(0, rollCap).map(([k, v]) => `${k}=${v}`).join(" ") +
      (roll.length > rollCap ? ` … and ${roll.length - rollCap} more file(s) carrying ${roll.slice(rollCap).reduce((a, r) => a + r[1], 0)} between them` : ``));
    console.log(`      (--all prints them; \`node engine/citegen.mjs <path>\` audits one path and prints every finding in it)`);
  };

  /* AND THIS LINE PRINTS ON THE CLEAN DAY TOO, WHICH IS THE WHOLE OF WHY IT IS AN `else` AND NOT AN `if`.
     It is the REFUSAL band — the standards this run counted and never checked — and it used to appear only
     when it was non-empty, which is the shape CLAUDE.md names as furniture: a line nobody learns to look for,
     because it is absent on every run where it says nothing. That matters in one direction specifically, and
     it is the direction a reader is least suspicious of. A finding total falls when defects are FIXED and it
     falls when the instrument STOPS LOOKING, and moving a standard OUT of the judged population and into this
     band does the second while reading as the first. A reader who cannot see the band at zero has no way to
     tell which happened. So the zero is stated, in the same words as the non-zero, on every run. */
  if (byOther.size) {
    const all = [...byOther].sort((a, b) => b[1] - a[1]), shown = full ? all : all.slice(0, 14);
    console.log(`  standards seen but not indexed: ${shown.map(([k, v]) => `${k}=${v}`).join(" ")}${tail(all, shown)}`);
  } else {
    console.log(`  standards seen but not indexed: none — every standard a citation in this run NAMED has a row in SPECS, ` +
      `so nothing here was counted and left unchecked on that ground. A standard MOVING into this line is a coverage loss ` +
      `however small the finding total gets.`);
  }
  const gaps = [...unknownTok].filter(([, v]) => v >= 8).sort((a, b) => b[1] - a[1]);
  if (gaps.length) {
    const shown = full ? gaps : gaps.slice(0, 20);
    console.log(`  capitalised tokens in front of a § that no list knows (a standard among these is coverage this audit is not getting): ` +
      `${shown.map(([k, v]) => `${k}=${v}`).join(" ")}${tail(gaps, shown)}` +
      `; tokens seen fewer than 8 times are not listed`);
  }

  /* ===== NUMBER-ONLY CITATIONS — A BAND, NOT A FINDING LIST ==================================================
   * PRINTED ON EVERY RUN, INCLUDING THE CLEAN DAY, AND THAT IS THE HALF THAT WAS MISSING RATHER THAN THE
   * COUNT. This listing existed behind `--titles`, over a narrower population and capped at forty, and a line
   * reached by REMEMBERING a flag catches what it catches by luck — which is the same argument the header
   * already makes for the quotation check running unconditionally, and the same incident: the CSSOM View
   * fabrication was found only because one lane happened to pass a flag that day, and the run that found it
   * turned up an older site making the identical false claim that nobody had passed it for. So the band is
   * unconditional and `--titles` now widens it rather than enabling it.
   * WHAT IT IS A FRACTION OF IS STATED IN THE SAME LINE, because CLAUDE.md's rule is that a coverage figure
   * that does not name its denominator is not a coverage figure — and this one has three populations behind
   * it that take different work: a number the corpus can title, a number the standard does not have, and a
   * site the term channel corroborated anyway.
   * IT DOES NOT MOVE THE EXIT CODE, and neither does anything else here — this auditor reports by design. The
   * rule it is obeying is the sharper one: an instrument that cannot see something has not FOUND anything, so
   * the blind spot is a SEPARATE verdict from the findings and is never summed into them. */
  console.log(`\nNUMBER-ONLY CITATIONS — a § with no title beside it, which is the one shape no check above can falsify`);
  console.log(`  (CLAUDE.md §Browser half requires the title beside the number, because the title survives an edition the`);
  console.log(`   number does not and the mismatch is then visible instead of silent. A citation stating a number ALONE`);
  console.log(`   is outside checks (2), (4) and (5) BY CONSTRUCTION — there is no claim beside the number for any of`);
  console.log(`   them to read — so it is ACCEPTED. A wrong number sends a reader to the wrong place, where they find`);
  console.log(`   out; a number-only citation that RESOLVES tells them they have arrived.`);
  console.log(`   NOTHING BELOW IS ACCUSED. This is a blind spot, reported apart from the findings and never summed into them.)`);
  console.log(`  ${stat.noTitle} of the ${stat.total - stat.other - stat.skipped} resolved citation(s) state no title this audit could confirm` +
    ` (${stat.titled} do state one and were confirmed by it); ${stat.noTitleCrash} of them are printed by a CRASH, which is read with no file open`);
  console.log(`    ${stat.noTitleDerivable} cite a § the committed corpus HOLDS, so this tool can say what that number is — the rows below do` +
    `; ${stat.noTitleNoSection} cite a § the standard does not have, and the corpus has no title to offer for those`);
  /* THE IDENTITY THAT DEFINES THE LISTING, ASSERTED WHERE BOTH SIDES ARE IN ONE HAND. CLAUDE.md: a counter
   * states its kind at the point it is emitted and the conservation identity that defines it is emitted beside
   * it, because that identity is the one property of a counter a reader can actually check. The map is built
   * per NUMBER and the census counter is raised per SITE, by two different lines, so their agreement is a real
   * claim rather than a tautology — and if a later edit moves either predicate, this fires instead of printing
   * a band whose listing is a different population from the one its own sentence names. */
  const numberOnlySites = [...numberOnly.values()].reduce((a, g) => a + g.n, 0);
  if (numberOnlySites !== stat.unverified)
    throw new Error(`NUMBER-ONLY band: the listing holds ${numberOnlySites} site(s) and the census counted ${stat.unverified} — ` +
      `these are raised on the same predicate over the same population and must agree; one of the two moved`);
  console.log(`    ${stat.noTitle - stat.unverified} were corroborated anyway by the TERM standing beside the number (check (3) confirmed or accused them), so the missing title` +
    ` costs a reader convenience rather than a check; the remaining ${stat.unverified} are what nothing in this run examined at all, and they are the listing`);
  {
    /* PER STANDARD, because the band's whole content is which coverage this tool is not getting, and one
     * five-figure total moves for reasons no reader can attribute. A standard's row falls when its citations
     * gain titles and rises when it gains citations, and those are different facts about different work. */
    const by = new Map();
    for (const [k, g] of numberOnly) {
      const key = k.slice(0, k.indexOf(" §"));
      const r = by.get(key) || { n: 0, nos: 0 };
      r.n += g.n; r.nos++; by.set(key, r);
    }
    const all = [...by].sort((a, b) => b[1].n - a[1].n), shown = full ? all : all.slice(0, 14);
    console.log(`  unexamined by standard (sites/distinct numbers): ${shown.map(([k, v]) => `${k}=${v.n}/${v.nos}`).join(" ")}` +
      `${all.length > shown.length ? `; ${all.length - shown.length} more standard(s) not listed` : ""}`);
  }
  {
    /* THE CHEAP CLOSE, AND THE UNIT IS THE NUMBER RATHER THAN THE SITE BECAUSE A TITLE IS A FACT ABOUT THE
     * NUMBER. N sites citing one § are one row carrying one answer, which is what makes a five-figure
     * population fit on a screen at all — and it is also the honest unit, since the repair at each of those
     * sites is the same words.
     * THE ORDER IS FREQUENCY, AND THIS LINE SAYS WHAT THAT ORDERING ANSWERS AND WHAT IT DOES NOT. It answers
     * "where would writing a title buy the most", which is a real question: a number cited two hundred times
     * bare is one edit per site away from being checkable and a number cited once is not worth an afternoon.
     * It does NOT answer "which of these is wrong", and no ordering available here does — the tool has no
     * evidence about any of them, which is the definition of the band. The Web IDL §N of the incident above
     * sat six sites deep in this population while nine OTHER sites in this tree stated and confirmed that
     * number's real title; frequency would not have surfaced it and nothing else here would either. What surfaces a row like that is a
     * HUMAN reading the number against the sentence beside it, which is exactly what printing the title
     * makes possible and what no count ever will.
     * SO THE HEAD IS NOT THE POPULATION, and the line below says so rather than letting a truncated list read
     * as a whole one: `--titles` prints every row, and `node engine/citegen.mjs <path>` prints every row in
     * one path, which is the workflow that catches one — a reader already standing in the file. */
    const all = [...numberOnly].sort((a, b) => b[1].n - a[1].n);
    const cap = argv.includes("--titles") || full || explicitFiles ? Infinity : 24;
    const shown = all.slice(0, cap);
    if (shown.length) {
      console.log(`  what those numbers ARE, most-cited first — ${all.length} distinct number(s), ${shown.length} printed` +
        `${all.length > shown.length ? " (--titles prints them all; naming a path prints every row in it)" : ""}.` +
        ` A row is NOT a defect: read the title against the sentence at the site, which is the check this tool cannot make`);
      for (const [k, g] of shown) {
        console.log(`  ${String(g.n).padStart(4)}x  ${k}  ${g.title === null ? "(NO SUCH SECTION — the corpus has no §" + k.slice(k.indexOf("§") + 1) + " for this standard)" : `"${g.title}"`}` +
          (g.crash ? `  [${g.crash} in a crash message]` : "") +
          (g.voted ? `  — ${g.voted} of them name no standard either, so the "${k.slice(0, k.indexOf(" §"))}" half of this key is this audit's guess and a title written under it would confirm the inference rather than test it` : "") +
          `\n        ${g.sites.join("  ")}${g.n > g.sites.length ? `  … and ${g.n - g.sites.length} more` : ""}`);
      }
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
    for (const v of head([...solid, ...thin], cap)) {
      console.log(`  ${v.file}:${v.line}  §${v.no} (${v.spec}${v.has ? "" : ", which has no such section"})`);
      console.log(`      ${v.text.trim()}`);
    }
    elided([...solid, ...thin], cap, "file-voted crash citation(s)");
  }

  /* THE QUOTATION REPORT NAMES ITS AXIS AND ITS DENOMINATOR IN THE SAME LINE. CLAUDE.md: a coverage figure
   * states what it is a fraction of, or it is not a coverage figure — and every auditor here partitions, so
   * saying which question this one asked is what keeps a zero from reading as a clean bill on the others. */
  {
    console.log(`\nQUOTATION CHECK — do the words a citation puts in quotes occur in the section it names?`);
    console.log(`  (this asks about TEXT. It says nothing about whether the number exists, whether the algorithm lives there,`);
    console.log(`   or whether the claim the sentence makes is true — those are the checks above, and they are different axes.)`);
    console.log(`  ${qstat.seen} quotation(s) of ${MIN_COMPARED_WORDS}+ words stand in prose a citation governs; ${qstat.checked} were compared against a section's own words`);
    /* WHICH MARK CARRIED THEM, printed because this channel was UNSCANNED and a population that was invisible
     * has to be visible as a NUMBER before anybody can argue about its size. The too-short figure is beside it
     * for the same reason: it is where this tree's terms and property names land, and reading the two together
     * is what says the word floor — never the mark — is what separates a term from a quotation. */
    console.log(`    by MARK: ${qstat.seen - qstat.single} double-quoted, ${qstat.single} single-quoted (${qstat.singleCrash} of those in a crash message; a further ${qstat.singleTooShort} single-quoted run(s) were declined by the word floor, which is where this tree's own terms and property names land)`);
    console.log(`    VERIFIED ${qstat.verified}  CONFIRMED-BY-A-NUMBER-THE-SAME-COMMENT-CITES ${qstat.okNearby} (${qstat.okNearbyCrossLiteral} of them by a number in a DIFFERENT LITERAL of the same crash message — the population a span-keyed lookup cannot see, so this figure is what the prose-unit rule is carrying)  WRONG-SECTION ${qstat.wrongSection} (${qstat.wrongSectionAncestor} of them at a section that CONTAINS the cited one)  WRONG-STANDARD ${qstat.wrongStandard}  NOT-FOUND ${qstat.notFound}` +
      ` (of which ${qstat.notFoundNothing} leave the standard within their first ${MIN_FRAGMENT_WORDS} words, and ${qstat.ownProse} of THOSE are a run of this tree's OWN authored prose in quotation marks — separated by evidence, named below under OWN-PROSE QUOTATION, and NOT counted as findings)`);
    /* EACH REFUSAL NAMES ITS OWN STATE AND THE SUBSET A CRASH PRINTS, because these are a WORK QUEUE and the
     * three former `voted` states do not have the same repair: VOTED and UNRESOLVED are drained by writing
     * the standard's name at the site (measured, not assumed — see PASS 4), and FOREIGN is not, because it
     * already names one. The crash figure is what orders what remains: that text is read by whoever is
     * standing at an abort, with no file open. */
    const cr = (n) => (n ? `, ${n} of them in a crash message` : "");
    console.log(`  NOT CHECKED, and why: ${qstat.noCorpus} cite a standard with no committed text corpus${cr(qstat.noCorpusCrash)}` +
      (noCorpusBy.size ? ` (${[...noCorpusBy].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join(" ")})` : "") +
      `; ${qstat.voted} sit under a citation naming no standard, whose standard only a file vote placed${cr(qstat.votedCrash)} — the head of the site-drainable queue, because this is where the audit prints a standard it has no evidence for` +
      `; ${qstat.unresolved} carry no anchor, no stated title and no term any index knows, so nothing placed them at all${cr(qstat.unresolvedCrash)} — also drained by naming the standard, silent rather than wrong until it is` +
      `; ${qstat.foreign} name a standard this tool indexes no text for${cr(qstat.foreignCrash)}, which no edit at the site can fix — that repair is an index` +
      `; ${qstat.noSection} cite a §N the standard does not have${cr(qstat.noSectionCrash)} (the corpus holds text for every section its index has, so this is the UNKNOWN-SECTION population above)` +
      `; a further ${qstat.tooShort} quoted run(s) are shorter than ${MIN_COMPARED_WORDS} words or carry no fragment of ${MIN_FRAGMENT_WORDS} and are not quotations this check can falsify`);
    /* AND THE TWO SITE-DRAINABLE STATES IN THAT SENTENCE NOW HAVE A LIST BEHIND THEM. A count with no list is a
     * population nobody can act on one member at a time, which is the only way a queue this size is ever
     * drained — and this file already says so about --unanchored's population one channel over. */
    console.log(`    the ${qstat.voted} + ${qstat.unresolved} site-drainable refusals are NAMED below under UNJUDGEABLE QUOTATION, one line each, so one can be drained by whoever is already editing that file`);
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
    /* A CRASH PRINTS ITS MESSAGE TO SOMEONE WHO HAS NO FILE OPEN, so a quotation inside one is listed first —
     * the same ordering, and the same reason, as --unanchored's. */
    const rank = (q) => (q.crash ? 0 : 1);
    /* AND QUOTE-NOT-FOUND IS TWO QUESTIONS UNDER ONE NAME, WHICH ONE HEAD CANNOT SERVE. The summary line above
     * already states the split and then the list ignored it: the old ordering put the EARLY-DIVERGENCE band
     * first, and that band is the one the same sentence calls indistinguishable from this tree's own prose in
     * quotation marks. It is also the large one — 566 of 781 at the revision this was written — so a 60-item
     * head ranked that way was 60 items a reader cannot act on, with every falsifiable one behind them.
     *   MIS-TRANSCRIBED is a real sentence that went wrong partway: the words matched, then stopped matching,
     *     so the standard's own text names the repair. `run consume body` for `to return the result of running
     *     consume body`, `a header list contains a name` for `a header list list contains a header name name`.
     *   UNSEPARABLE leaves the standard immediately. A fabrication lands there and so does a correctly-quoted
     *     piece of this tree's own prose, and no rule here can tell them apart — which is a fact to PRINT, not
     *     a reason to lead with it or to hide it.
     * Both are printed, each with its own count, its own head and its own file roll. Not a reordering of one
     * list: a reordering would still bury one question under the other's cap, and the count is the point. */
    const bands = [];
    for (const kind of ["QUOTE-NOT-FOUND", "QUOTE-WRONG-STANDARD", "QUOTE-WRONG-SECTION"]) {
      const g = qg.get(kind) || [];
      if (kind !== "QUOTE-NOT-FOUND") { bands.push([kind, g, ""]); continue; }
      const late = divergedLate;
      bands.push([`${kind} / MIS-TRANSCRIBED`, g.filter(late),
        ` — matched ${MIN_FRAGMENT_WORDS}+ words and then diverged, so the standard's own text names the exact word to repair.` +
        ` An ELIDED PARENTHETICAL lands here too and is not a defect: IndexedDB writes "less than or equal to 2^53 (9007199254740992) + 1"` +
        ` and a comment quoting it without the figure diverges truthfully. The divergence point is printed so a reader can tell those apart in one glance`]);
      bands.push([`${kind} / UNSEPARABLE`, g.filter((q) => !late(q)),
        ` — left the standard within ${MIN_FRAGMENT_WORDS} words. A fabricated sentence and this tree's own prose in quotation marks both land here and nothing mechanical separates them; each needs a human`]);
    }
    for (const [label, g, why] of bands) {
      console.log(`\n${label}: ${g.length}${why}`);
      const ord = [...g].sort((a, b) => rank(a) - rank(b));
      for (const q of head(ord, qlimit)) {
        console.log(`  ${q.file}:${q.line}  ${quoteMsg(q)}`);
        console.log(`      "${q.quote.length > 150 ? q.quote.slice(0, 150) + "…" : q.quote}"`);
      }
      elided(ord, qlimit, label);
    }

    /* THE QUOTATIONS THIS CHECK CANNOT JUDGE BECAUSE THIS TREE WROTE THEM — a separate VERDICT and not a
     * quieter finding. CLAUDE.md: an instrument that cannot see something has not found anything, and a gate
     * that sums the two states leaves a reader unable to name a single thing it currently says. So the count
     * stands on its own line with the population it was drawn from beside it, on the clean day as well as the
     * bad one, and every site prints with the file whose own prose holds the words. */
    {
      console.log(`\nOWN-PROSE QUOTATION: ${ownProse.length} of the ${qstat.checked} quotation(s) compared` +
        ` (${qstat.ownProseCrash} in a crash message) — NOT findings, and NOT a suppression: the words occur in THIS TREE'S own` +
        ` authored prose, outside any quotation mark, at the file named on each row. That is a positive fact a` +
        ` reader can check in one command, and it says only what it says — this is not a quotation of a standard,` +
        ` so the question "are these the standard's words" has no answer here rather than a NO. Everything else` +
        ` about the site stands: the citation over it is still resolved and still checked by every other channel.`);
      const ord = [...ownProse].sort((a, b) => rank(a) - rank(b));
      for (const q of head(ord, qlimit)) {
        console.log(`  ${q.file}:${q.line}  ${q.spec} §${q.no}${q.crash ? "  [in a crash message]" : ""} — this tree's own prose, authored at ${q.authoredAt}`);
        console.log(`      "${q.quote.length > 150 ? q.quote.slice(0, 150) + "…" : q.quote}"`);
      }
      elided(ord, qlimit, "OWN-PROSE QUOTATION");
    }

    /* THE QUOTATIONS THIS CHECK REFUSED, AS A CATEGORY WITH SITES IN IT RATHER THAN A CLAUSE IN A SENTENCE.
     * The census line above has counted this population all along, and counting is what CLAUDE.md calls the
     * silent zero: "a standard with no committed index is COUNTED and never CHECKED, which is a silent zero
     * rather than a clean bill". The same sentence is true one level down, of a quotation whose CITATION names
     * no standard — and it was true here, in the one state a SITE EDIT drains. A reader standing in the file
     * could not find out that a quotation two lines away was unaskable, because no line named it.
     *
     * IT IS NOT A FINDING AND THE HEADER SAYS SO IN ITS FIRST SENTENCE, because the failure mode of getting
     * that wrong is 3538 correct sites reported as defects — this file's own subject, committed by this file.
     * A site here may be quoting its standard perfectly; nothing has read it. UNVERIFIABLE and WRONG are two
     * states, and a category that blurs them teaches a reader to skim it, which costs every real finding
     * beside it.
     *
     * THE PROBE IS EVIDENCE AND IT IS NOT A RESOLUTION, and that line is the whole design. A bare §N is
     * genuinely ambiguous — nothing at the site says which document it means — so the tool may not adopt an
     * answer for it: adopting one would make a quotation VERIFY ITSELF, since the standard would have been
     * chosen BECAUSE it holds these words, and a citation whose true standard this tool does not index would
     * be silently absorbed by whichever indexed standard happened to number that section. That is the
     * confirmation channel this file's header already refused once, arriving from a different direction. What
     * it may do instead is state a fact that carries its own proof, the form the header endorses ("no indexed
     * standard defines `about base url` at §7.4"):
     *   — CORROBORATED-ELSEWHERE: some indexed standard numbers §N and its own words hold this quotation. The
     *     audit NAMES it and does not adopt it. Writing that name at the site is one edit, and the NEXT run
     *     judges the quotation for real, against a standard an author asserted rather than one a tool guessed.
     *   — UNCORROBORATED: NO indexed standard holds these words at that number. That is a true statement about
     *     every document this tool has, and it is where a fabricated sentence under a bare number would sit —
     *     beside a perfectly good citation of a standard nothing here indexes, which is why it is still not a
     *     finding. It is the head of the queue because it is the band where reading one costs something.
     * The two bands are counted apart for the reason every band here is: a search cannot be directed toward a
     * gap it reports with the same number as another. */
    {
      const probe = (u) => {
        const hits = [];
        for (const [k] of txt) if (ownSections(k, u.no).length && containsAnyForm(ownText(k, u.no), u.frags)) hits.push(k);
        return hits;
      };
      for (const u of unjudgedQuotes) u.where = probe(u);
      const un = unjudgedQuotes.filter((u) => !u.where.length), corr = unjudgedQuotes.filter((u) => u.where.length);
      const nCrash = (g) => g.filter((u) => u.crash).length;
      const byWhy = (g, w) => g.filter((u) => u.why === w).length;
      console.log(`\nUNJUDGEABLE QUOTATION: ${unjudgedQuotes.length} of the ${qstat.seen} quotation(s) this check saw` +
        ` — NOT findings. The citation over each names no standard, so there is no document to compare it against;` +
        ` a site here may be quoting its standard perfectly and nothing here has read it.`);
      console.log(`  ${byWhy(unjudgedQuotes, "voted")} sit under a citation a FILE VOTE placed (the audit's inference, never the citation's claim) and` +
        ` ${byWhy(unjudgedQuotes, "unresolved")} under one nothing placed at all; ${nCrash(unjudgedQuotes)} are printed by a CRASH, which is read with no file open.`);
      console.log(`  THE REPAIR IS ONE EDIT AT THE SITE: write the standard's name in front of the number. That moves the quotation into` +
        ` the ${qstat.checked} compared above, where the next run judges it — so draining this queue can RAISE the finding count, and that is the` +
        ` repair working. The ${qstat.foreign} FOREIGN and ${qstat.noSection} NO-SECTION refusals in the census line are NOT here: no edit at those sites drains them.`);
      const ucap = argv.includes("--all") ? Infinity : 25;
      for (const [label, g, why] of [
        [`UNJUDGEABLE / UNCORROBORATED`, un,
          ` — no indexed standard numbers §N AND holds these words there. That is true of every document this tool holds, and it is` +
          ` consistent with THREE different things: a fabricated sentence, a correct citation of a standard nothing here indexes, and a run of` +
          ` THIS TREE'S OWN prose in quotation marks (measured — \`"OWNED: the caller frees. Never NULL"\` sits here). Nothing mechanical` +
          ` separates those, which is why this is a queue and not a finding; reading one is what separates them`],
        [`UNJUDGEABLE / CORROBORATED-ELSEWHERE`, corr,
          ` — some indexed standard numbers §N and its own words hold this quotation. The audit names it and does NOT adopt it: a standard` +
          ` the citation never named cannot become the standard it is judged against. Writing that name at the site is the whole repair`]]) {
        console.log(`\n${label}: ${g.length} (${nCrash(g)} in a crash message)${why}`);
        const ord = [...g].sort((a, b) => rank(a) - rank(b));
        for (const u of head(ord, ucap)) {
          console.log(`  ${u.file}:${u.line}  §${u.no}${u.crash ? "  [in a crash message]" : ""}` +
            (u.why === "voted" ? `  — placed by a FILE VOTE at ${u.spec}, which is this audit's inference and not the citation's claim` : `  — nothing placed this citation`) +
            (u.where.length ? `; these words ARE ${u.where.slice(0, 3).join(", ")} §${u.no}'s` : ``));
          console.log(`      "${u.quote.length > 150 ? u.quote.slice(0, 150) + "…" : u.quote}"`);
        }
        elided(ord, ucap, label, 30);
      }
    }
  }

  /* THE AGREEMENT REPORT. ITS DENOMINATOR IS PRINTED ON EVERY RUN INCLUDING THE CLEAN ONE, for the reason
   * CLAUDE.md gives about the direction nobody checks: a rising count invites scrutiny and a FALLING one does
   * not, so a channel that lost half its population reads as a channel that got the tree repaired. Here the two
   * numbers cannot come apart — a passage quoted at several sites stays in `multiSite` whether its copies agree
   * or not — which is exactly what makes them worth printing together.
   *
   * WHERE THIS LANDS IN THE VERDICT, DECIDED RATHER THAN INHERITED. It is a FINDING CHANNEL and not a census
   * band, and the discriminator is the one CLAUDE.md draws between a finding and a blind spot: a blind spot is
   * a construct the instrument CANNOT SEE, and this instrument sees everything it needs — a disagreement is
   * demonstrated entirely out of the tree's own bytes, needs no corpus, no resolution and no inference, and
   * cannot be true of a tree that is right. It is a fourth channel rather than a widening of the quotation
   * channel because the two have different DENOMINATORS: that one is a fraction of the quotations COMPARED
   * against a corpus, this one of every quotation SEEN, and summing two populations is the shape this file
   * refuses everywhere else.
   * A SITE MAY THEREFORE BE COUNTED IN BOTH, and that is two different claims about one line rather than one
   * claim counted twice: the quotation channel says the words are not the cited section's, and this says the
   * tree does not agree with itself about them. Measured on the pair that seeded this channel, one of the three
   * sites is in both and one is in this channel alone. */
  {
    console.log(`\nQUOTATION AGREEMENT — when this tree quotes one passage at several sites, do the sites AGREE?`);
    console.log(`  (this asks about THE TREE. It is asked of EVERY quotation this check saw — including every one the resolver`);
    console.log(`   refused, where no other channel here can speak at all — because a disagreement is a claim about the WORDS`);
    console.log(`   and needs nobody's section number. It does NOT rank the sites: what a row asserts is that ONE spelling is`);
    console.log(`   in no indexed standard while a SIBLING SITE's is, which is a fact about two sites and a corpus.)`);
    console.log(`  ${qstat.seen} quotation(s) seen in ${agreement.spellings} distinct spelling(s); ${agreement.multiSite} passage(s) are quoted at more than one site.` +
      ` ${agreement.agreed} of those AGREE — one spelling, or one spelling and a CUT of it.`);
    console.log(`  THE REST DIFFER, AND THE THREE WAYS THEY DIFFER TAKE THREE DIFFERENT ACTIONS, so they are counted apart:` +
      ` ${agreeGroups.length} passage(s) where the spellings are ONE INSERTED CLAUSE apart and the corpus holds SOME of them and not others` +
      ` — ${agreeFindings.length} site(s), the findings below;` +
      ` ${agreeBoth.length} where EVERY spelling is some standard's own words, which is a standard writing PARALLEL SENTENCES` +
      ` (one per attribute, one per union arm, \`preferred width\` beside \`preferred minimum width\`) and is not a defect at all;` +
      ` ${agreeNeither.length} where NO spelling is any indexed standard's, which is where this tree quoting its OWN prose lands and` +
      ` where a citation of an unindexed standard lands beside it;` +
      ` and ${agreement.noticed.length} (${agreement.noticedSites} site(s)) that differ some OTHER way than one inserted clause.` +
      ` The last three are NOT findings and are listed under --agree.`);
    console.log(`  HOW A PASSAGE IS BUILT, and it never reads a section number: two quotations are ONE PASSAGE when one CONTAINS` +
      ` the other and is at least half of it (a CUT — the legitimate case, an author quoting the clause they need), or when they` +
      ` share a contiguous run of ${AGREE_RUN}+ tokens that is a strict majority of BOTH. A passage is ACCUSED only where two of its` +
      ` spellings have a common non-empty PREFIX and SUFFIX totalling ${MIN_COMPARED_WORDS}+ tokens with ONE side holding nothing between them` +
      ` — a sentence that GAINED A CLAUSE, which is the defect's own shape, not a similarity score.`);
    console.log(`  BLIND SPOTS, STATED — all silences, none a wrong answer. A SUBSTITUTED word ("…IN the entries in S" against` +
      ` "…OF the entries in S") is not one inserted clause and is counted, never accused. An early divergence in a SHORT quotation` +
      ` shares too few tokens to reach the floor. A site whose quotation wraps a MUCH LONGER passage around the disputed sentence` +
      ` fails the majority. An ELIDED quotation is always the CUT side and never the disagreeing one, because its own ellipsis` +
      ` looks exactly like an insertion. And this channel is WHOLE-TREE: a path-scoped or --since run compares only the sites it` +
      ` read, so a disagreement whose sibling stands outside that set forms in neither run and is SILENT rather than clean.`);

    const gcap = argv.includes("--all") ? Infinity : 25;
    console.log(`\nQUOTE-DISAGREEMENT: ${agreeFindings.length} site(s) in ${agreeGroups.length} passage(s)`);
    let shown = 0;
    for (const { g, ev } of agreeGroups) {
      if (shown >= gcap) break;
      shown++;
      console.log(`  passage ${shown}: ${g.length} spellings at ${g.reduce((n, sp) => n + sp.sites.length, 0)} site(s)`);
      for (const { sp, at } of ev) {
        console.log(`    [${sp.sites.length} site(s)] ${at ? `IS ${at}'s own words` : `in NO indexed standard`}: "${clipQ(sp.sites[0].quote.trim())}"`);
        for (const site of sp.sites) {
          named.add(site.file);
          console.log(`        ${site.file}:${site.line}  ${site.spec ? `${site.spec} §${site.no}` : `§${site.no} (nothing placed this citation)`}` +
            (site.how === "file" ? " [placed by a FILE VOTE]" : "") + (site.crash ? "  [in a crash message]" : ""));
        }
      }
    }
    if (agreeGroups.length > shown)
      console.log(`  … ${agreeGroups.length - shown} more passage(s) NOT PRINTED — the head above is in site-count order.` +
        ` \`--all\` prints every one; a path argument prints every one inside that path.`);

    /* THE THREE BANDS THIS CHANNEL NOTICES AND DOES NOT ACCUSE, behind a flag for the reason every large band here
     * is: printed by default they are furniture inside a day and would take the accused band down with them. Their
     * COUNTS are on the census line above on EVERY run including the clean one, which is the half a reader must
     * not be able to miss — a channel whose findings fall while its population falls with them has lost coverage
     * and not gained repairs, and only the denominator says which. */
    if (argv.includes("--agree")) {
      for (const [label, gs, why] of [
        ["EVERY SPELLING IS A STANDARD'S OWN WORDS", agreeBoth,
         ` — two RIGHT quotations of two DIFFERENT sentences. This is what a standard's PARALLEL prose looks like from here` +
         ` and it is not a defect; the band exists so its size is readable rather than inferred from the accused band's smallness`],
        ["NO SPELLING IS ANY INDEXED STANDARD'S", agreeNeither,
         ` — this tree's own prose in quotation marks, a standard nothing here indexes, or two fabrications. Nothing mechanical` +
         ` separates those, which is why it is a queue and not a finding`],
        ["DIFFERS SOME OTHER WAY THAN ONE INSERTED CLAUSE", agreement.noticed,
         ` — a word SUBSTITUTED, a reordering, two edits. A mis-transcribed word is in here and so is every parallel sentence` +
         ` that differs by a noun; the corpus is not asked, because the shape that makes the question answerable is absent`]]) {
        console.log(`\nQUOTATION AGREEMENT / ${label}: ` +
          `${gs.reduce((t, g) => t + g.reduce((n, sp) => n + sp.sites.length, 0), 0)} site(s) in ${gs.length} passage(s) — NOT findings${why}`);
        for (const g of gs) {
          console.log(`  ${g.length} spellings at ${g.reduce((n, sp) => n + sp.sites.length, 0)} site(s):`);
          for (const sp of g) {
            console.log(`    [${sp.sites.length}] "${clipQ(sp.sites[0].quote.trim())}"`);
            for (const site of sp.sites) console.log(`        ${site.file}:${site.line}  ${site.spec ? `${site.spec} §${site.no}` : `§${site.no}`}`);
          }
        }
      }
    }
  }

  /* THE STEP REPORT STATES ITS DENOMINATOR IN THE SAME LINE AS ITS NUMBER, and it has more denominators than
   * any other check here because more things can stop it: a step reference is judged only where the citation
   * over it resolved on its own evidence, the standard has a committed step corpus at the same edition, the
   * section exists, and something under it is numbered. Each of those is a different reason for silence and
   * each is counted, because a search cannot be directed toward a gap it reports with the same number as two
   * others. */
  {
    console.log(`\nSTEP CHECK — can the step number a citation writes exist in the section it names?`);
    console.log(`  (this asks about LIST STRUCTURE. It cannot tell a step that exists from the RIGHT step: where an`);
    console.log(`   algorithm's step 6 holds seven sub-steps, 6.4 and 6.5 are both admitted and only MEANING separates`);
    console.log(`   them. What it can falsify is a number no reading of the section admits — which is what the drift`);
    console.log(`   CLAUDE.md describes produces once it runs past the end of a list.)`);
    console.log(`  ${sstat.seen} step reference(s) stand in prose a citation governs (${sstat.sub} of them sub-numbered); ${sstat.checked} were compared against a section's own lists`);
    console.log(`    ${sstat.lettered} write a LETTERED component (\`2.i\`, \`13.a.iv\`), read as every position that spelling can denote and never as a convention this file holds a table of — see STEP_NO` +
      `; of those, ${sstat.letterSplit} EXIST under one reading and not another, so which step the author named turns on the alphabetic/roman convention the corpus does not record: NOT DECIDED, counted here and never reported as confirmed`);
    console.log(`    EXISTS ${sstat.exists}  CONFIRMED-BY-A-NUMBER-THE-SAME-COMMENT-CITES ${sstat.okNearby}` +
      `  CONFIRMED-BY-THE-NUMBER-WRITTEN-BESIDE-IT ${sstat.okLead}` +
      `  CONFIRMED-BY-THE-OWNER-THE-FILE-DECLARES ${sstat.okDeclared}  OUT-OF-RANGE ${sstat.out}` +
      `  NOT-IN-THIS-SECTION ${sstat.away} (listed, never accused — see --steps)`);
    /* WHAT THE LAST NUMBER IS A FRACTION OF, said in the same line as the number: a reworded finding is one
     * the nearest-preceding citation would have charged to the wrong section. It is not an extra finding and
     * not a suppressed one — the verdict is the same and the address is different. */
    console.log(`    ${sstat.reworded} of those finding(s) name the section a design note's own HEADING declares` +
      ` rather than the citation standing nearest, and say so in the message`);
    console.log(`  NOT CHECKED, and why: ${sstat.unreadable} write a component whose spelling denotes no position under either the alphabetic or the roman reading` +
      `; ${sstat.noCorpus} cite a standard with no committed step corpus` +
      (stepNoCorpusBy.size ? ` (${[...stepNoCorpusBy].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join(" ")})` : "") +
      `; ${sstat.staleCorpus} cite one whose corpus is a different edition from its section index` +
      `; ${sstat.foreign} name a standard this tool does not index` +
      (stepForeignBy.size ? ` (${[...stepForeignBy].sort((a, b) => b[1] - a[1]).slice(0, 8).map(([k, v]) => `${k}=${v}`).join(" ")})` : "") +
      ` — those are NOT merely unchecked here: a citation naming an unindexed standard is what a file vote hands to whichever indexed standard owns a section by that number, and the other checks have been measured judging one against a document its author never cited` +
      `; ${sstat.unresolved} resolved to no standard at all` +
      `; ${sstat.citeFlagged} stand under a citation this run already reports as wrong, where the SECTION is the defect and the step number is a consequence of it` +
      `; ${sstat.voted} stand under a citation whose standard only a file vote placed, which this channel refuses to judge on` +
      `; ${sstat.noSection} cite a §N the standard does not have (the UNKNOWN-SECTION population above)` +
      `; ${sstat.noList} cite a section holding no list at all — the shape a RIGHT section takes when the algorithm the step belongs to is written one heading away, which this channel counts and never judges`);
    if (stpStale.size) for (const [k, why] of stpStale)
      console.log(`  ${k}: step corpus REFUSED — ${why}; re-run: node engine/citegen.mjs --regen ${k}`);
    /* THE BAND THIS CHANNEL WILL NOT ACCUSE IS PRINTED THE WAY THE NUMBER-ONLY BAND PRINTS ITS OWN POPULATION:
     * count and head in the census, the whole list behind a flag. A refusal nobody can see is the same silent zero as a list
     * nobody can see, and these sites were invisible to every check in this file before it existed — so the
     * count says how many there are and the flag says which. Read one and you are reading a citation that may
     * be perfectly right, which is exactly why it is not in the findings. */
    if (argv.includes("--steps")) {
      console.log(`\nSTEP-NOT-IN-THIS-SECTION: ${stepsAway.length} — the cited section holds no list reaching this step, which does NOT`);
      console.log(`  establish the citation is wrong: the section can be right for the TERM while the step belongs to an algorithm`);
      console.log(`  the prose names in words one heading away. Listed so a human can read them; never counted as a finding.`);
      const acap = argv.includes("--all") ? Infinity : 60;
      const aord = [...stepsAway].sort((a, b) => (a.crash ? 0 : 1) - (b.crash ? 0 : 1) || b.depth - a.depth);
      for (const v of head(aord, acap)) {
        console.log(`  ${v.file}:${v.line}  step ${v.step} — ${v.msg}`);
        console.log(`      …${v.text}…`);
      }
      elided(aord, acap, "STEP-NOT-IN-THIS-SECTION");
      /* THE CONTENT CHANNEL'S REFUSED BAND, PRINTED BESIDE THE EXISTENCE CHANNEL'S AND FOR THE SAME REASON.
       * These are claims whose section holds SEVERAL lists reaching the step, so the number names several
       * different steps and no reading may be accused — but reading them was how the split was measured, and
       * three of the ten read were real defects. A band nobody can see is the silent zero this file is written
       * against; a band nobody may be accused from is the discipline. Both, at once, is what this prints. */
      console.log(`\nSTEP-CONTENT-SEVERAL-READINGS: ${stepsSaysAmbig.length} — the cited section holds more than one list reaching this`);
      console.log(`  step, so the number names more than one step and none of them can be accused. Listed so a human can read`);
      console.log(`  them; never counted as a finding. Some are real, and the one that decides which is the reader.`);
      const cacap = argv.includes("--all") ? Infinity : 60;
      const caord = [...stepsSaysAmbig].sort((a, b) => (a.crash ? 0 : 1) - (b.crash ? 0 : 1) || b.depth - a.depth);
      for (const v of head(caord, cacap)) {
        console.log(`  ${v.file}:${v.line}  step ${v.step} — ${v.msg}`);
        console.log(`      …${v.text}…`);
      }
      elided(caord, cacap, "STEP-CONTENT-SEVERAL-READINGS");
    }
    const slimit = argv.includes("--all") ? Infinity : 60;
    /* A CRASH PRINTS ITS MESSAGE TO SOMEONE WITH NO FILE OPEN, so a step number inside one is listed first —
     * the same ordering and the same reason as --unanchored's and the quotation head's. */
    const sord = [...stepsOut].sort((a, b) => (a.crash ? 0 : 1) - (b.crash ? 0 : 1) || b.depth - a.depth);
    console.log(`\nSTEP-OUT-OF-RANGE: ${stepsOut.length}`);
    for (const v of head(sord, slimit)) {
      console.log(`  ${v.file}:${v.line}  step ${v.step} — ${v.msg}`);
      console.log(`      …${v.text}…`);
    }
    elided(sord, slimit, "STEP-OUT-OF-RANGE");

    /* THE CONTENT CHANNEL REPORTS ITS OWN DENOMINATOR IN ITS OWN LINE, because it is a fraction of a fraction:
     * only a step that EXISTS is asked, only a possessive is read as a claim, and only a standard whose corpus
     * carries item positions can be asked at all. A count printed without those three would be read as a
     * statement about every step reference in the tree, and it is a statement about the few that are all of
     * those things at once. */
    console.log(`\nSTEP CONTENT — does the step a citation names SAY what the citation says it says?`);
    console.log(`  (only \`step N's X\` is read as a claim; only the standard's own words answer it; and an accusation`);
    console.log(`   must be able to NAME the step that does use the word, or it is not a repair and is not made.)`);
    console.log(`  ${sstat.claimSeen} of the ${sstat.exists} step reference(s) that EXIST make a possessive claim` +
      ` (${sstat.claimNone} do not, and are outside this channel entirely)`);
    console.log(`    CONFIRMED ${sstat.claimOk} — the cited step uses the word the claim attaches to it` +
      `  REPORTED ${sstat.claimOut}` +
      `  SEVERAL-READINGS ${sstat.claimAmbig} (listed, never accused — see --steps)` +
      `  NOT CHECKED ${sstat.claimNoPos + sstat.claimNotTerm + sstat.claimMention + sstat.claimUnseen}: ${sstat.claimNoPos} cite a standard whose step corpus carries` +
      ` no item positions${stpNoPos.size ? ` (${[...stpNoPos].sort().join(" ")}` +
        ` — re-run: node engine/citegen.mjs --regen)` : ""}` +
      `, ${sstat.claimNotTerm} attach a phrase that is neither a term the standard defines nor one bare word of its own vocabulary standing in under ${Math.round(DF_CAP * 100)}% of its sections` +
      ` — this tree's word for something, a modifier, or a run of prose with no end this reader can find` +
      `, ${sstat.claimMention} stand in prose that DISCLAIMS them (a retirement note, a displayed spelling)` +
      `, ${sstat.claimUnseen} attach a term the cited section's own lists never use at all`);
    const cord = [...stepsSays].sort((a, b) => (a.crash ? 0 : 1) - (b.crash ? 0 : 1) || b.depth - a.depth);
    console.log(`\nSTEP-SAYS-OTHERWISE: ${stepsSays.length}`);
    for (const v of head(cord, slimit)) {
      console.log(`  ${v.file}:${v.line}  step ${v.step} — ${v.msg}`);
      console.log(`      …${v.text}…`);
    }
    elided(cord, slimit, "STEP-SAYS-OTHERWISE");
  }

  const groups = new Map();
  for (const f of findings) { if (!groups.has(f.kind)) groups.set(f.kind, []); groups.get(f.kind).push(f); }
  const limit = argv.includes("--all") ? Infinity : 120;
  console.log("");
  for (const kind of ["UNKNOWN-SECTION", "MISATTRIBUTED", "TITLE-MISMATCH", "RETIREMENT-NOTE-WRONG"]) {
    const g = groups.get(kind) || [];
    console.log(`${kind}: ${g.length}`);
    /* THE DENOMINATOR TRAVELS WITH THE HEADLINE NUMBER, not only with the census forty lines up. This count is
     * over the citations the term check was ASKED of, and a reader triaging it needs to know which population
     * that is without reconstructing it — the same reason the quotation check names its axis in its own
     * banner rather than leaving a zero to read as a clean bill. */
    if (kind === "MISATTRIBUTED")
      /* AND THE FIRST CLAUSE IS WRITTEN SO IT CANNOT BECOME AN OVER-CLAIM THE DAY A CORPUS GOES STALE — the
       * refused sites are subtracted from it IN THE SENTENCE rather than named afterwards, because a reader
       * who takes "every one of these" at face value and then meets a count of ones that were never asked has
       * been handed two statements that cannot both be true. */
      console.log(`  (${g.length - stat.textRefused} of these stand at a section whose own committed text carries the phrase neither as spaced words nor inside an identifier of its own, ` +
        `and the other ${stat.textRefused} could not be asked that question at all because their standard carries no usable corpus; ` +
        `${stat.confirmedByText} citation(s) whose cited section DOES state it in its own words, and ${stat.confirmedByIdent} whose cited section spells it as one identifier, are confirmed above rather than accused here. ` +
        `A count over the citations the term check was ASKED of. ${stat.titled} more were confirmed by a stated TITLE and never asked; ` +
        `on their own evidence the term check would have added ${stat.titledMis} claims here, ${stat.titledMisInTitle} of them naming a phrase inside that same title. ` +
        `Adding a correct title to a citation MOVES it out of this number — see the title-channel lines in the census above for both halves.)`);
    for (const f of head(g, limit)) {
      console.log(`  ${f.file}:${f.line}  ${f.msg}`);
      console.log(`      ${f.text.trim()}`);
    }
    elided(g, limit, kind);
  }
  console.log(`\nUNDECIDED-ON-A-DIAGNOSED-NUMBER: ${suspects.length}`);
  console.log(`  (these name no term, so the tool cannot decide them; they cite a number whose OTHER sites in the same file are misattributed above. A human must read each one — a guess here is the defect this file exists to find.)`);
  for (const f of head(suspects, limit)) {
    console.log(`  ${f.file}:${f.line}  §${f.no} — the decided sites on this number in this file point to ${tallyOf(f)}`);
    console.log(`      ${f.text.trim()}`);
  }
  elided(suspects, limit, "undecided site(s)");

  /* THE TITLE CHANNEL'S OWN REFUSALS, PARTITIONED BY WHY — see check (5). A citation that states a phrase in
   * quotes right after the number has written it where CLAUDE.md's mandated form puts the TITLE, so when
   * neither the confirmation nor the mismatch channel can place it, the report owes a reader the reason. The
   * buckets are not degrees of the same answer: a possessive is a claim about a section's CONTENTS, a long run
   * is a QUOTATION another channel already judged, a phrase in the section's own text is the section's WORDS,
   * and only the last bucket is a stated title this tool read and could say nothing at all about. */
  {
    const b = stat.titleBy;
    console.log(`\nTITLE-STATED-AND-UNPLACED: ${stat.titleDeclined} — a quoted phrase in title position that neither the`);
    console.log(`  confirmation channel (check 2) nor the mismatch channel (check 4) could place. This counts only citations`);
    console.log(`  resolved to one of the ${idx.size} INDEXED standards: the ${stat.other} that name a standard this audit does not index are`);
    console.log(`  outside every check here, and the "standards seen but not indexed" line in the census above names them.`);
    console.log(`    ${b.quotation} are long enough that the QUOTATION CHECK judged them (${MIN_COMPARED_WORDS}+ compared words) — not silent, reported there`);
    console.log(`    ${b.possessive} are the possessive \`§N's "X"\`, which claims X is IN §N and asserts nothing about §N's title — check (3)'s question, and it found no term`);
    console.log(`    ${b.term} carry a phrase some OTHER standard defines, already counted as another standard's vocabulary`);
    console.log(`    ${b.inSectionText} quote words that ARE in the text of the cited section OR OF A SUBSECTION OF IT — usually the author quoting the section rather than its heading,` +
      ` but a corpus slice begins with its own number and TITLE, so a title that heads nothing can be absorbed here by a SUBSECTION'S heading. Measured with a positive control: see the bucket`);
    console.log(`    ${b.nearTitle} state a title the cited standard carries at a section CONTAINING or CONTAINED BY the cited one — less precise, not wrong`);
    console.log(`    ${b.voted} stand under a standard only a file vote placed, so "no section of X is titled this" would be a claim about a document the citation never named`);
    console.log(`    ${b.noCorpus} cite a standard with a section index but no committed text corpus; ${b.oneWord} state a one-word title and ${b.notWords} normalize to no words at all`);
    console.log(`  ${b.unplaced} REMAIN — not the section's title, not any section's title, not a term any index knows, and not the section's own words.`);
    console.log(`  A FABRICATED TITLE LANDS HERE, so each is listed. This is not a finding: the tool cannot tell a fabricated title`);
    console.log(`  from a term claim written without a possessive, and check (5) records the measurement that says so.`);
    for (const f of head(unplacedTitles, limit)) {
      console.log(`  ${f.file}:${f.line}  ${f.spec} §${f.no} states "${f.title}"; §${f.no} is "${f.real}"`);
      console.log(`      ${f.text.trim()}`);
    }
    elided(unplacedTitles, limit, "unplaced title claim(s)");
  }

  /* PRINTED, NOT SUPPRESSED — the whole difference between a rule and a mute button. Every site the grammar
   * disclaimed is listed with the verdict the checker WOULD have given it, so a reader who thinks the tool got
   * the grammar wrong can see the finding it withheld and say so. A category that hides its own contents is
   * the silent zero this file is written against, and it would be no better for being a category of refusals.
   * The verified split is stated first because it is the part that is a CHECK rather than a refusal. */
  const noteOK = mentions.filter((m) => m.note && m.note.ok);
  console.log(`\nMENTION-NOT-CLAIM: ${mentions.length} — the prose is TALKING ABOUT this citation, not making it`);
  console.log(`  (${noteOK.length} of them are retirement notes whose OWN title claim this audit CONFIRMED against the index — checked in both directions, ` +
    `the strongest state a citation here reaches. A note naming a title its number does not carry is reported as RETIREMENT-NOTE-WRONG above, not here.)`);
  console.log(`  (a citation displayed inside a backtick run, or governed by "stood here"/"used to be"/"this file cited", is a MENTION. Bare "not §N" is NOT in that set — it is contrastive and stays a claim.)`);
  for (const m of head(mentions, limit)) {
    console.log(`  ${m.file}:${m.line}  §${m.no} (${m.spec}) — ${m.why}` +
      (m.note ? `; its title claim "${m.note.title}" is CONFIRMED` : "") +
      (m.would ? `; withheld ${m.would}` : "; the checker had no verdict on it either way"));
    console.log(`      ${m.text.trim()}`);
  }
  elided(mentions, limit, "mention(s)");

  /* AND THE LAST LINE CARRIES THE SAME FACT ON THE SAME AXIS, because it is the line a reader keeps. A count
   * of findings says nothing about whether this output mentioned the file they came to ask about, and this is
   * the one place a per-file coverage figure can be stated once for every list above it.
   *
   * AND IT WAS A FRACTION OF THE POPULATION IT DID NOT NAME, WHICH IS THIS FILE COMMITTING ITS OWN SUBJECT.
   * CLAUDE.md §Testing: a coverage figure states what it is a fraction of, in the same line, or it is not a
   * coverage figure. The line above printed `findings.length` under the bare word "finding(s)", and
   * `findings` is ONE of the THREE arrays this same function calls findings — the quotation channel and the
   * step channel are the other two, and the comments beside them say so in capitals ("A QUOTATION FINDING IS
   * A FINDING, SO --since MUST SEE IT"; "A STEP FINDING IS A FINDING"). So the disagreement was never about
   * what ought to count: the --since return two dozen lines up already unions all three, and only the report
   * printed a third of them under the whole name. MEASURED at the revision this was written: 504 printed,
   * 871 quotation and 20 step findings NOT printed — the headline was 36% of its own population, and it is
   * the number this project quotes at each other, so before/after deltas were being taken against a third of
   * the tree's citation defects. The repair is not a new rule about what a finding is; it is the report using
   * the set the delta channel next to it already uses, with each channel NAMED so nobody has to sum four
   * category headers by hand to learn what the total covers.
   *
   * THE SECOND HALF IS THE FILE FIGURE, AND IT WAS A RATIO OF TWO DIFFERENT POPULATIONS. `allFiles` was built
   * from findings+suspects+quotations — the step channel was in NO denominator anywhere, so 3 files carrying
   * only a step finding appeared in no count on this line. `named` is filled by every printed head, INCLUDING
   * the four lists that are deliberately not findings (MENTION-NOT-CLAIM, TITLE-STATED-AND-UNPLACED,
   * STEP-NOT-IN-THIS-SECTION, UNJUDGEABLE QUOTATION), so "standing in N file(s) of which M are named above" asserted M ⊆ N while 13
   * of the 164 were in no part of the 345. A containment claim whose two sides are gathered from different
   * sets is the same defect as the headline, one clause along: both sides are real counts and the sentence
   * joining them is not true of anything. So the denominator is the union of the three FINDING channels, and
   * the numerator is intersected with it — the files named above that carry a finding — while the files named
   * only by a non-finding list are stated apart rather than folded in.
   *
   * AND THE DENOMINATOR OF THE FINDINGS THEMSELVES IS PRINTED, BECAUSE WITHOUT IT A REPAIR READS AS A
   * REGRESSION. Every check here is asked only of a citation resolved on its OWN evidence; the INFERRED
   * population is placed by a file vote and the census line above says outright that nothing below judges it.
   * So writing a standard's name at an unanchored site MOVES that citation out of the unjudged population and
   * into the judged one, where it is checked for the first time — and the finding count can RISE because the
   * tool now sees a site it was previously blind to. That happened: a lane repaired 18 quotation findings and
   * watched a count go UP, which is correct behaviour reported by a number with no denominator on it. Stating
   * both sides makes the movement legible in the one line a reader keeps: the judged population is what a
   * repair grows, and a finding count is only comparable against the population it was drawn from. */
  /* EACH CHANNEL CARRIES ITS OWN DENOMINATOR, BECAUSE THE ONE PRINTED BELOW IS NOT THE ONE THAT MOVES. The
   * citation-resolution denominator is a fact about the TREE, and a run whose corpus changed leaves it
   * IDENTICAL — 26630 of 48994 in both halves of the incident that produced this line. What moved was each
   * channel's OWN population: quotations COMPARED fell 5156 → 2962 when two standards' text corpora were
   * refused, and the finding total fell with it, so a report that lost half the quotation channel read as a
   * 978 → 688 improvement. The reader who ran it said outright that on the total alone they would have
   * committed it as a win. CLAUDE.md's rule is that a coverage figure states what it is a fraction of IN THE
   * SAME LINE; this is that rule applied to the one line two runs are actually compared on. */
  const findingChannels = [["section/term/title", findings, () => judged, "resolved citations"],
                           ["quotation", quoteFindings, () => qstat.checked, "compared"],
                           ["quotation agreement", agreeFindings, () => qstat.seen, "quotations grouped"],
                           ["step", stepFindings, () => sstat.checked, "compared"]];
  const allFindings = findingChannels.flatMap(([, g]) => g);
  const allFiles = new Set(allFindings.map((f) => f.file));
  const namedWithFinding = [...named].filter((f) => allFiles.has(f));
  const judged = stat.anchored + stat.byTerm + stat.byTitle;
  const unjudged = stat.byFile + stat.other + stat.skipped;
  /* THE PARTITION IS ASSERTED RATHER THAN TRUSTED. These six counters are incremented at six different sites
   * and the sentence below claims they tile `stat.total` exactly; if a later resolver adds a seventh outcome,
   * the line would go on printing two numbers that no longer account for the corpus — a coverage figure whose
   * denominator has quietly stopped being the population. That is the defect this whole block exists to end,
   * so it crashes here instead of printing. */
  if (judged + unjudged !== stat.total)
    throw new Error(`citegen: the resolution census does not tile the corpus — judged ${judged} + unjudged ${unjudged} != ${stat.total} citations. ` +
      `A resolution outcome was added without a counter, and the coverage line below would understate its own denominator.`);
  console.log(`\n${allFindings.length} finding(s) = ${findingChannels.map(([n, g, d, lbl]) => `${g.length} ${n} of ${d()} ${lbl}`).join(" + ")}` +
    ` — the total over EVERY channel this run judges, which is the set --since compares. The four category headers above` +
    ` (UNKNOWN-SECTION, MISATTRIBUTED, TITLE-MISMATCH, RETIREMENT-NOTE-WRONG) sum to ${findings.length} and are one of the four.`);
  /* AND THE CORPUS STATE IS PRINTED HERE AND NOT ONLY MID-REPORT, because it is the one condition under which
   * a FALLING finding total means LOST COVERAGE rather than repair, and it is invisible in every other number
   * on this line. It arises from a PARTIAL corpus: `--regen` writes a standard's section index, its text and
   * its steps from ONE fetch, so it cannot produce this state — but copying `text/` and `steps/` into a tree
   * whose index is older can, and did. The editions then disagree, the staleness pair refuses those standards
   * outright, and the standards that vanish are whichever ones the WHATWG happened to edit that week: the
   * measured instance lost html and fetch, which between them carried 2205 of the run's quotations.
   * IT IS SAID WHEN THERE IS NOTHING TO SAY, TOO. A line that appears only on the bad day is a line nobody
   * learns to look for, and its absence then reads as the clean case rather than as a line that was never
   * printed — the silent zero this file is written against, in the summary itself. */
  {
    const ref = [...new Set([...txtStale.keys(), ...stpStale.keys()])].sort();
    const lost = qstat.noCorpus + sstat.noCorpus + sstat.staleCorpus;
    console.log(`  CORPUS: ${txt.size} standard(s) with a text corpus, ${stp.size} with steps` +
      (stpNoPos.size ? ` (${stpNoPos.size} of those carrying no item positions, so the step CONTENT check is not asked of them)` : "") +
      (ref.length
        ? `. REFUSED: ${ref.join(" ")} — the corpus is a different EDITION from the section index it is keyed by, which is what a`
          + ` partial regen leaves behind (text or steps copied in without the index beside them). Those standards are removed from the`
          + ` judged population WHOLE, so a finding total that falls here has lost coverage rather than gained repairs.`
          + ` Re-run: node engine/citegen.mjs --regen ${ref.join(" ")}.`
        : `. None refused: every committed corpus agrees in edition with the section index it is keyed by.`) +
      ` ${lost} quotation(s) and step reference(s) went unjudged this run for want of a corpus.`);
  }
  console.log(`  drawn from ${judged} citation(s) resolved on their own evidence, of ${stat.total} read` +
    ` — the other ${unjudged} (${stat.byFile} placed only by a file vote, ${stat.other} naming an unindexed standard, ${stat.skipped} naming no standard and no term)` +
    ` are outside every check here. Writing a standard, term or title at one of those MOVES it into the judged population,` +
    ` so a repair can RAISE this count: compare a finding total only against the judged number printed beside it.`);
  console.log(`  ${suspects.length} undecided beside them (not findings — see UNDECIDED-ON-A-DIAGNOSED-NUMBER).` +
    ` The findings stand in ${allFiles.size} file(s), ${namedWithFinding.length} of them named above; the rest are rolled up by the "NOT PRINTED" lines.` +
    ` A further ${named.size - namedWithFinding.length} file(s) are named above by a list that is NOT a finding channel.`);
  /* AND THE HEADS ARE HEADS, WHICH MAKES A CROSS-RUN SET-DIFF OF PRINTED LINES INVALID. Every list above is
   * capped and ordered by file, so a finding entering the head EVICTS another, and the evicted one reads
   * exactly like a repair while the entrant reads exactly like a regression. That is not a caveat about
   * precision, it is a wrong answer in both directions at once, and it lands on precisely the rolled-up
   * categories a reader most wants to compare. The three untruncated forms are named here because a reader
   * comparing two runs has no other way to know the printed lines are not the population. */
  console.log(`  The lists above are HEADS, in file order, not samples: an entry entering one EVICTS another, so diffing two runs'` +
    ` printed lines reports repairs and regressions that did not happen. Compare populations, not heads —` +
    ` \`--all\`, \`node engine/citegen.mjs <path>\` and \`--since <ref>\` each print or compare an untruncated set.`);
  console.log(`  This auditor REPORTS; it exits 0 by design — see the header.`);
}

/* ---- --since: what THIS diff introduced ------------------------------------------------------------------ */

/* A DELTA IS THE RIGHT MEASUREMENT AND A DELTA GATE IS STILL THE WRONG MECHANISM, and the two halves of that
 * are worth stating apart because the first is what this builds and the second is what it refuses to build.
 *
 * THE MEASUREMENT. A four-figure standing population is a number nobody reads, so "run it on what you write" —
 * which CLAUDE.md §Browser half now requires — is an instruction that costs a lane more attention than it has.
 * That figure USED TO BE WRITTEN HERE AS "five hundred", and it was the report's headline rather than the
 * population: the headline counted the section/term/title channel alone while this function's own return
 * unions three, so the sentence justifying THIS mode was quoting a third of the set THIS mode compares. The
 * count is deliberately not restated as a digit now — see the roll-up at the end of the report, which prints
 * the three channels by name at the revision it ran.
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
  const git = (args) => execFileSync("git", args, { cwd: ROOT, encoding: "utf8" }).split("\n").filter(Boolean);
  const changed = git(["diff", "--name-only", ref, "--", ...AUDITED_GLOBS]);
  /* AND THE FILE THAT IS NOT IN GIT YET, WHICH THIS MODE REPORTED AS `0 introduced` WHILE READING NONE OF IT.
     `git diff` compares two trees and an UNTRACKED file is in neither, so it is not omitted with a message —
     it is absent from the list, and the delta prints a clean zero for a diff whose whole content is a new
     component. That is the excluded-test shape this file names elsewhere: the total LOOKS complete. It is not
     a `git show` failure and needs no error handling — the `catch` below already reads an absent-at-ref file
     as empty, which is exactly the right answer for one: every finding in a new file is that diff's own. */
  const untracked = git(["ls-files", "--others", "--exclude-standard", "--", ...AUDITED_GLOBS]);
  const files = [...new Set([...changed, ...untracked])].map((r) => join(ROOT, r)).filter((p) => existsSync(p));
  if (!files.length) { console.log(`no audited file (${AUDITED_GLOBS.join(" ")}) differs from ${ref} or stands untracked — nothing for this mode to compare`); return; }

  const baseSrc = new Map();
  for (const p of files) {
    const rel = relative(ROOT, p);
    /* `stderr: "ignore"` because a file absent at <ref> is the EXPECTED case here and git says so loudly:
       `fatal: path '<p>' exists on disk, but not in '<ref>'` reaches the terminal on the child's stderr while
       the `catch` below handles it perfectly. A handled condition that prints a fatal is a false alarm at the
       top of a report, and a reader who has just been told this mode reads untracked files has every reason
       to read it as the mode failing on exactly those. Nothing is hidden: the header line states how many of
       the files read were untracked. */
    try { baseSrc.set(p, execFileSync("git", ["show", `${ref}:${rel}`],
      { cwd: ROOT, encoding: "utf8", maxBuffer: 64 * 1024 * 1024, stdio: ["ignore", "pipe", "ignore"] })); }
    catch { baseSrc.set(p, ""); }        /* absent at ref — a new file owns every finding in it */
  }
  const key = (f) => `${f.file}\u0000${f.kind}\u0000${f.no}\u0000${f.msg}\u0000${f.qtext || ""}`.replace(/:\d+/g, "");
  const tip = audit(argv, { files, quiet: true });
  const base = audit(argv, { files, quiet: true, srcOf: (p) => baseSrc.get(p) });
  const had = new Set(base.map(key));
  const added = tip.filter((f) => !had.has(key(f)));
  const gone = base.filter((f) => !new Set(tip.map(key)).has(key(f)));

  console.log(`spec-citation delta against ${ref}: ${files.length} audited file(s) read (${changed.length} ` +
    `differ from ${ref}, ${untracked.length} untracked), ${base.length} finding(s) before, ${tip.length} after`);
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
