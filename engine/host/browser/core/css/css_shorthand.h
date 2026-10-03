/* THE GRAMMAR OF A DECLARATION LEXBOR'S REGISTRY DOES NOT TYPE — which is CSS Cascade §Shorthand Properties
 * for a shorthand, and the property's own value grammar for a longhand.
 *
 * WHY LEXBOR DOES NOT DO IT. Lexbor's property registry carries `overflow-x` and `overflow-y` and does NOT
 * carry `overflow`, so `overflow: hidden` reaches the cascade as an UNKNOWN declaration — which lexbor still
 * parses, still names and still serializes (it becomes a `LXB_CSS_PROPERTY__CUSTOM` holding the name and the
 * raw value tokens), so nothing is lost, it is simply not expanded. Expanding it HERE rather than adding a
 * property to lexbor's GENERATED registry keeps the vendored parser at its pinned tag, which is the whole
 * reason this engine binds to it.
 *
 * THE CASCADE IS OVER LONGHANDS ONLY, which is why the first half is a component and not a line inside the
 * cascade: a declaration sets a longhand either by BEING it or by being a shorthand of it, and the second way
 * is a per-shorthand GRAMMAR (`overflow: <'overflow-block'>{1,2}` is not `margin`'s four-side rotation and is
 * not `border`'s any-order triple). Each is its own small parse with its own invalid case, and an invalid
 * shorthand value is a DROPPED declaration rather than a partial one — which is exactly the contract a caller
 * wants back: a value, or nothing.
 *
 * AND THE FIRST WAY NEEDS THE SAME GRAMMAR, which is why both halves are one component rather than two. For a
 * longhand lexbor DOES carry, its own parser has already validated the declaration and serialized it back
 * canonically, so the value is taken verbatim. For one it does not — the four `border-*-width` and the four
 * `border-*-style`, which are `__CUSTOM` like every shorthand here — NOTHING has applied the property's
 * grammar, so `border-top-style: bogus` would win the cascade and be reported as a computed value no grammar
 * admits (css_style_declaration.c records `display: bogus` doing exactly that through lexbor's `__UNDEF`), and
 * `border-top-style: SOLID` would fail to compare equal to the keyword it is. Those are the same two jobs the
 * `border-style` shorthand's expansion does to each of its components, off the same list — so a longhand
 * declaration goes through this file too, and one grammar answers both spellings. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SHORTHAND_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SHORTHAND_H
#include <stdbool.h>
#include <stddef.h>

/* The SPECIFIED value that the declaration `shorthand: value` gives to `longhand`. NULL when `shorthand` is not
   one this component expands, when it does not set `longhand`, or when `value` does not match the shorthand's
   grammar — the last is an INVALID declaration, which the cascade drops, and returning NULL is how it is
   dropped. OWNED: the caller frees. */
char *css_shorthand_component(const char *shorthand, const char *value, const char *longhand);

/* Does this component own `longhand`'s OWN value grammar — is it one lexbor's property registry does not carry
   and nothing else has validated? The cascade asks before it takes a declaration's value verbatim, because the
   answer is what decides whether that value has been through a grammar at all. FALSE for every property lexbor
   types (its parser is the grammar) and for a CSS custom property (`--brand`, whose value is by definition
   whatever the author wrote). */
bool css_shorthand_validates_longhand(const char *longhand);

/* The SPECIFIED value the declaration `longhand: value` gives `longhand` itself, put through that grammar:
   canonicalized where the grammar is a keyword, verbatim where it is a length. NULL when the value does not
   match — an INVALID declaration, which the cascade drops. Only ever called for a name the predicate above
   answers TRUE for, and it asserts that. OWNED: the caller frees. */
char *css_shorthand_longhand_value(const char *longhand, const char *value);

/* css-values-4 §5.3 "Real Numbers: the <number> type"'s PRODUCTION over one component value's span — TRUE when
   the whole span is a literal number, with `*out` receiving its value so each grammar's own range restriction
   (`<number [0,∞]>` for a flex factor, `<number [1,1000]>` for a font weight) is checked by the caller that
   states it rather than here.
   IT IS ONE PRODUCTION AND NOT ONE PER SHORTHAND, which is why it is declared rather than repeated: §5.3's
   sentence is the same one for every property that admits a bare number, and two spellings of it are two ideas
   of what `0x10` is. THE SPAN IS FILTERED BEFORE `strtod` SEES IT, because `strtod` is a C production and not a
   CSS one — it accepts `0x10`, `inf` and `nan`, none of which §5.3 admits: "When written literally, a number is
   either an integer, or zero or more decimal digits followed by a dot (.) followed by one or more decimal
   digits; optionally, it can be concluded by the letter “e” or “E” followed by an integer indicating the
   base-ten exponent in scientific notation" — the spec's own curly marks kept, because the straight ones would
   close this quotation three words early. §5.3's next sentence adds the sign ("the first character of a number
   may be immediately preceded by - or + to indicate the number's sign"), which the filter admits too. */
bool css_shorthand_number(const char *w, size_t n, double *out);

/* THE COMPONENT VALUES of `v`, written into `w`/`len` as spans inside it. `max` is the grammar's own
   multiplier, so a value carrying more components than the grammar admits is INVALID rather than truncated —
   reported as -1, which every caller turns into the dropped declaration.
   IT IS CSS Syntax §4 "Tokenization"'s SPLIT AND NOT A SPLIT ON WHITESPACE, which is three sentences and not a
   nicety. A FUNCTION is one component value however many spaces its arguments carry, so `border: 1px solid
   rgb(1, 2, 3)` is THREE and a whitespace split reports five and drops it as over-long. A `<string>` is one
   however many spaces are INSIDE it (css-values-4 §4.4 "Quoted Strings: the <string> type": "they are
   delimited by double quotes or single quotes, and correspond to the <string-token> production"), so
   `list-style-type: "Note: "` is ONE and a whitespace split reports two. And a quote is only a delimiter when
   it is not ESCAPED — §4.4: "Double quotes cannot occur inside double quotes, unless escaped".
   EXPORTED for the reason §5.3's `<number>` above is: css-counter-styles-3 §4 "Defining Anonymous Counter
   Styles: the symbols() function"' `[ <string> | <image> ]+` is a component-value list inside ONE component
   value, so the consumer that owns that production asks here rather than writing a second split that would
   disagree about `symbols("a b" url(x.png))`. */
int css_shorthand_components(const char *v, const char **w, size_t *len, int max);

/* css-values-4 §4.4 "Quoted Strings: the <string> type"'s PRODUCTION over one component value's span — the
   `<string-token>` §4.4 defers to CSS Syntax for, asked as a validity test because the specified value is the
   author's own bytes (core/css/css_image.h states that argument for an `<image>` and §4.4's is the same one:
   a string's quotes are part of how CSSOM serializes it back, so choosing between them here would be this
   engine inventing a spelling).
   FALSE FOR A `<bad-string-token>`, which is the one arm a reader is likeliest to leave out. CSS Syntax §4.3.5
   "Consume a string token" ends a string at a NEWLINE with that token rather than with a string, and §4.4
   states the author-facing half — "It is possible to break strings over several lines ... but in such a case
   the newline itself has to be escaped with a backslash" — so a span carrying an unescaped newline, or whose
   closing quote is consumed by the escape in front of it, is a declaration CSS Syntax drops. */
bool css_shorthand_string(const char *w, size_t n);

/* Is the set of shorthands that can set `longhand` recorded here IN FULL, AND does the expansion above answer
   for every one of them? A consumer that derives a longhand's COMPUTED value asserts this before it trusts the
   cascade, because the failure mode of an unrecorded shorthand is silence: `margin: 0` would leave
   `margin-top` reading its initial value, with a real number to show for it and nothing to say the declaration
   was never looked at.
   THE TWO HALVES ARE ANSWERED BY TWO DIFFERENT MECHANISMS AND ONLY THE SECOND IS DERIVED. This sentence read
   "BOTH halves are DERIVED now — the first from the table below, the second from each shorthand's own `does
   css_shorthand_component answer for my longhands` flag — so the two lists that used to be maintained side by
   side cannot come apart", and it is rewritten rather than deleted because the reverse direction of the table
   IS derived by scanning the rows, so a reader who generalises from that will write it again.
   THE SECOND HALF IS DERIVED, AND NOT FROM A FLAG: the flag is DELETED — the row struct's `probe` comment in
   the implementation records that it went with the one row that answered FALSE for it — and what discharges
   that half now is css_shorthand_init's round trip, which runs EVERY row's fixture through the expansion and
   back and crashes on a row that does not answer for one of its own longhands. It is a property of the TABLE
   rather than of a longhand, which is stronger than the per-longhand read it replaced.
   THE FIRST HALF IS RECORDED BY HAND, AND IT IS NOT DERIVABLE FROM THIS TABLE AT ALL — not "not derived yet".
   IT QUANTIFIES OVER CSS AND THE TABLE KNOWS ONLY WHAT IS IN IT: `display` is complete because NO shorthand in
   CSS sets it, and no row mentions it, so a table-derivation cannot produce it; `font-variant-caps` is NOT
   complete because css-fonts-4 §6.11 "Overall shorthand for font rendering: the font-variant property" sets it
   and has no row here, and the `font` row DOES name it, so a table-derivation would answer TRUE where the
   correct answer is FALSE. The derivation and the recorded list would therefore disagree in BOTH directions,
   and the recorded list is the right one — which is why there is no agreement assert to be had between them.
   WHAT IS CLOSED BY CONSTRUCTION IS THE HAZARD THE RETIRED SENTENCE CREATED: a lane that adds a row, reads
   "both halves are DERIVED" and leaves the predicate alone used to get a silent FALSE for every longhand the
   new row names. css_shorthand_init now asserts the two lists PARTITION the table's own longhands — each is
   either recorded complete or recorded incomplete-by-design, never neither and never both — so a row whose
   longhands are in neither list CRASHES at init, naming the row and the longhand.
   RETIREMENT: this record goes when the first half is answered from the committed spec corpus's own property
   definitions — the properties whose `Value:` line names `<'longhand'>`, which engine/specindex/text carries —
   because the question is then derived from the STANDARDS rather than recorded from one reading of them, and
   the incomplete-by-design list is derived with it. */
bool css_shorthand_complete_for(const char *longhand);

/* THE TABLE'S OWN INVARIANTS, asserted once per agent. The reverse direction below and the forward direction
   above are two readings of ONE table, and the way that goes wrong is a longhand added to a shorthand's list
   with no branch in `css_shorthand_component` to expand it — silent, because the reverse reader never calls the
   forward one. So every expanding shorthand is EXERCISED here against its own fixture value and the round trip
   (expand each longhand, re-consolidate) must reproduce it. */
void css_shorthand_init(void);

/* ---- CSSOM §6.6's LONGHAND <-> SHORTHAND directions, which its declaration-block serialization walks ------- */

/* The widest longhand list in the table (css-fonts-4 §2.7's `font`: its seven Set Explicitly sub-properties
   and the twelve it Resets Implicitly — see core/css/css_font_shorthand.h), and the most shorthands any one
   longhand maps to (`border-top-width` maps to `border`, `border-width` and `border-top`). Both are ASSERTED
   against the table by css_shorthand_init, so a row that outgrows one crashes rather than writing past a
   caller's array. */
#define CSS_SHORTHAND_MAX_LONGHANDS 19
#define CSS_SHORTHAND_MAX_OF 3

/* THE LONGHANDS `shorthand` SETS, in the CANONICAL ORDER of its own property definition table. NULL, with
   `*pn` zero, for a name this component does not record as a shorthand.
   THE ORDER IS LOAD-BEARING wherever the grammar is positional — `margin`'s four sides, `overflow`'s two axes,
   `border-<side>`'s `<line-width> || <line-style> || <color>` — because css_shorthand_serialize_value reads
   the caller's parallel value array through it. BORROWED: the table's storage is static. */
const char *const *css_shorthand_longhands(const char *shorthand, unsigned *pn);

/* Is `name` recorded above as a shorthand? A REAL CSS shorthand this component does not record answers FALSE,
   and a caller then treats it as a property in its own right — which is what the block serialization does for
   css-fonts-4 §6.11 "Overall shorthand for font rendering: the font-variant property"'s `font-variant`, whose
   seven `font-variant-*` longhands this table has no row for. That is why the predicate is named for the TABLE
   and not for CSS: a reader who takes it as "is a CSS shorthand" gets a wrong answer for every shorthand
   outside it. */
bool css_shorthand_is_shorthand(const char *name);

/* THE SAME TABLE ASKED FOR A NAME RATHER THAN A YES, because CSSOM §2's "supported CSS property" is a SET whose
   members have a canonical spelling and a caller that matched one has to store it. It is the same row and the
   same lookup as the predicate above — a second table of the same names is the copy that drifts — and the
   pointer is the row's own static name, so it outlives every caller and needs no free.
   WHY A SHORTHAND THIS COMPONENT EXPANDS IS A SUPPORTED PROPERTY AT ALL: §2 defines the term as "a CSS
   property that the user agent implements", and the user agent is this ENGINE and not the vendored parser it
   embeds. A name in the table below is one whose grammar and whose expansion this file owns, which is what
   implementing a property means. */
/* THE SAME ROWS, ENUMERATED — the table walked by index, NULL past the last, so a caller that must build a SET
   out of them does not need a count beside the accessor (a count and a table are one fact, and the count is
   the copy that goes stale when a row is added). CSSOM §6.6.1's three per-property installers need this
   because they walk a property SPACE rather than asking about a name they already hold. */
const char *css_shorthand_name_at(unsigned i);

const char *css_shorthand_property_named(const char *name);

/* THE SHORTHANDS `property` IS A LONGHAND OF, written into `out` in CSSOM §6.6 "CSS Declaration Blocks"' own
   PREFERRED ORDER, which is FOUR STEPS and a DEFINITION rather than a UA preference. Returns how many. `max`
   must be at least CSS_SHORTHAND_MAX_OF. The names are BORROWED.
   THE STEPS ARE QUOTED ONE AT A TIME RATHER THAN CONDENSED, every run below from CSSOM §6.6: (1) order
   shorthands lexicographically. (2) move all items in shorthands that begin with U+002D, "last in the list,
   retaining their relative order". (3) the same for the items that begin with U+002D but do not begin with the
   `-webkit-` prefix. (4) "order shorthands by the number of longhand properties that map to it, with the
   greatest number first".
   THIS ENTRY USED TO CONDENSE ALL FOUR INTO ONE DOUBLE-QUOTED RUN, ONE LINE PER BACKTICKED PIECE BELOW SO THE
   MASK THAT KEEPS A SPELLING OUT OF THE QUOTATION CHANNEL IS NEVER ASKED TO CROSS A NEWLINE IT CANNOT CROSS:
   `order shorthands lexicographically; move all items that begin with - last; move all`
   `items that begin with - but not -webkit- last; order by the number of longhand`
   `properties that map to it, with the greatest number first`
   It is REWRITTEN RATHER THAN DELETED because the defect is the SHORTENING and not the words, and a reader who
   wants one compact sentence will write it again. It diverged from CSSOM at word six — `that` where the
   standard has `in` — and the clause it dropped TWICE is RETAINING THEIR RELATIVE ORDER, which is the
   STABILITY every later sentence in this component rests on: `css_sh_move_last` partitions in place and the
   final insertion sort compares strictly less-than, so each step is stable, and without that clause the
   quotation licensed none of it. A quotation CUT where the sentence turns is VERIFIED by any corpus that holds
   the sentence, so no channel here could ever have reported it.
   WHAT IS NOT COVERED: the live editor's draft has RESTRUCTURED this algorithm into FIVE steps and the
   committed corpus predates it, so the four above are the CORPUS'S and are what this component implements. The
   draft inserts a second step removing every item that is a LEGACY SHORTHAND and does not begin with U+002D,
   and re-spells the old second step over the `-webkit-` prefix rather than over U+002D. THE RE-SPELLING
   CHANGES NO OUTCOME — traced over a list of an unprefixed name, a `-moz-` one, a `-webkit-` one and a second
   unprefixed one, both arrangements end with the two unprefixed names, then the `-webkit-` one, then the
   `-moz-` one — so this component is already right about that half, and only the removal is outstanding.
   THE REMOVAL IS NOT BUILT AND ITS POPULATION OVER THE TABLE ABOVE IS EMPTY TODAY, which is a reading and not
   a design: css-cascade-5 §3.1 "Property Aliasing" makes a legacy shorthand an ALIAS declared by whichever
   specification creates the new property, its own worked example being the `page-break-*` properties, and no
   row of this table is an alias of anything — derive that rather than trust this sentence, by reading the row
   names out of SHORTHANDS and asking of each whether any specification declares it an alias. WHAT THE NEXT
   DIFF BUILDS: the corpus regeneration FIRST, because a verbatim quotation of the draft is reported as a
   fabrication against a corpus that predates it, and then a per-row FLAG rather than a spelling test, because
   the aliasing is a fact the DEFINING specification states and not one a name carries. HOW ITS ABSENCE WOULD
   SHOW: css-cascade-5 §3.1 says of a legacy shorthand that the CSSOM "will not use them when serializing
   declarations", so a block holding every longhand of an aliased shorthand would serialize under the
   DEPRECATED name wherever that name sorts ahead of the modern one — observable as a `cssText` naming a
   property the page never wrote.
   ZERO IS ONE FACT AND NOT A LIST OF SITUATIONS: NO ROW OF THE TABLE ABOVE NAMES `property` AMONG ITS
   LONGHANDS. THE OPERAND IS NAMED FOR WHAT CALLERS HAND IT RATHER THAN `longhand`, because a parameter that
   presupposes its operand's KIND is the whole of what makes a zero read as a claim about a longhand — and what
   a page writes is routinely a shorthand, a custom property, or a property this table has no row for.
   THIS ENTRY USED TO DESCRIBE THE RETURN AS
   `0 both for a longhand no recorded shorthand sets and for a name that IS one`
   (backticked: it is a spelling being shown, and in double quotes beside a § it would read as a quotation of
   CSSOM and be reported as one that is not there). It is REWRITTEN RATHER THAN DELETED because a reader who
   re-derives it from the fact that a shorthand answers 0 will write it again, and it is wrong twice over. It
   ENUMERATES INPUTS where the answer is a PROPERTY, so it is incomplete for every name it does not list and
   the two it does list partition nothing — a custom property answers 0 for the identical one reason. And read
   as a CONFESSION OF AMBIGUITY it invites a second out-parameter telling those two inputs apart, which no
   caller here would read.
   THE MERGE IS THE STANDARD'S OWN CONDITION AND NOT THIS COMPONENT'S CHOICE. CSSOM §6.6's step is "If property
   maps to one or more shorthand properties, let shorthands be an array of those shorthand properties, in
   preferred order", and a SHORTHAND maps to no shorthand exactly as an uncovered longhand does — so the
   condition is false, the loop does not run, and the declaration serializes under its own name, which is the
   correct answer for both. Every caller reads the count the way the algorithm does, as a LIST TO ITERATE and
   never as a flag:
   `cssd_serialize_decls` loops to it and emits the declaration itself when the loop does not run,
   `css_page_property_applies` loops to it and answers false, and `css_shorthand_init`'s round trip is the ONE
   site that asserts a nonzero — on an operand the `css_shorthand_is_shorthand` DCHECK two lines above it has
   already excluded, so the two readings are disjoint there rather than merged.
   THE SHORTHAND OPERAND IS REACHED RATHER THAN HYPOTHETICAL, which is why the merge has to be stated and not
   just tolerated: `css_page_property_applies` asks the page lists about the DECLARATION AS WRITTEN and before
   any expansion — `cssd_decls_collect_declaration` states why, and names a shorthand doing it — so every
   shorthand this table records that core/css/css_page.c's lists do not carry for the context asked arrives
   here, a `@page { list-style: none }` and a `@page { inset: 0 }` among them, and 0 is right for each because
   a name those lists do not carry does not apply however it expands.
   A CALLER THAT MUST TELL THE READINGS APART ASKS `css_shorthand_is_shorthand`, which is the SAME TABLE asked
   the other question — one fact, two predicates, with the mutual exclusion asserted in `css_shorthand_init`'s
   per-longhand loop and again inside `css_shorthand_complete_for`. WHAT IT MUST NOT DO IS READ 0 AS
   COMPLETENESS: `css_shorthand_complete_for` is not derivable from this entry and its own paragraph states why
   in both directions — `display` answers 0 here and IS complete, `font-variant-caps` answers 1 here and is NOT.
   RETIREMENT: this record goes when this entry delivers its names NULL-TERMINATED and returns nothing, as
   `css_shorthand_name_at` above already does and for the reason stated there, because a caller then holds no
   integer to read as a flag and the zero this paragraph is about does not exist to be interpreted. */
unsigned css_shorthand_shorthands_of(const char *property, const char **out, unsigned max);

/* @LOGICAL — THE LOGICAL PROPERTY GROUP ENTRY THAT STOOD HERE IS `css_logical_group_of` IN
   core/css/css_logical.h, and it moved rather than being wrapped: a forwarder here would be a second door on
   one question, and this component's §6.6 serialization is a CALLER of that question rather than its owner.
   ITS CITATION WAS ALSO MIS-AIMED and the repair is worth recording where the wrong number was read, because
   a reader who re-derives it from the sentence will reach for the same section: the "logical property group"
   definition and the sentence about cascading a pair together are css-logical-1 §4 "Flow-Relative Box Model
   Properties", not §2 — css-logical-1 §2 is "Flow-Relative Values: block-start, block-end, inline-start,
   inline-end", which is about the VALUES `caption-side`, `float`, `clear` and `text-align` take and names no
   property group at all. The quotation went with it: the sentence begins "Each set of parallel flow-relative
   properties and physical properties", and this header rendered it `any pair of flow-relative properties and
   physical properties`. */

/* CSSOM §6.7.2's SERIALIZE A CSS VALUE over a LIST of longhand declarations: the value a hypothetical
   `shorthand` declaration would carry, given `values[i]` as the serialized value of
   `css_shorthand_longhands(shorthand)[i]`. Every entry must be non-NULL — the caller has already established
   that the block declares all of them.
   NULL is the spec's "shorthand cannot exactly represent the values of all the properties in list", which
   §6.6's loop reads as the empty string and answers by moving on to the next shorthand: a `border` whose four
   widths disagree, a `border-top` whose three components are all initial (there would be nothing left to
   write), a set in which one longhand carries a CSS-wide keyword and another does not. OWNED. */
char *css_shorthand_serialize_value(const char *shorthand, const char *const *values);

#endif
