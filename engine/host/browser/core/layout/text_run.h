/* THE INLINE-AXIS MEASUREMENT OF A TEXT RUN — css-text-3 §4.1 "The White Space Processing Rules" for which
 * characters are there to measure, css-text-3 §5 "Line Breaking and Word Boundaries" (over core/layout/
 * line_break.h, which is [UAX14]'s algorithm) for where the run may break, and css-values-4 §6.1.1's ADVANCE
 * MEASURE for how wide each surviving one is.
 *
 * IT ANSWERS TWO NUMBERS AND THEY ARE THE TWO css-sizing-3 §2.1 "Auto Box Sizes" NAMES, which is why this
 * component exists at all rather than a width being summed wherever one was first needed. §2.1 states both
 * over the SAME content and separates them by exactly one thing — which soft wrap opportunities are taken:
 *   max-content inline size — "the narrowest inline size it could take while fitting around its contents IF
 *     NONE OF THE SOFT WRAP OPPORTUNITIES within the box were taken". §2.1's own note: "this is called the
 *     'preferred width' in CSS2.1§10.3.5".
 *   min-content inline size — "the inline size that would fit around its contents IF ALL SOFT WRAP
 *     OPPORTUNITIES within the box were taken". §2.1's note: "this is called the 'preferred minimum width' in
 *     CSS2.1§10.3.5".
 * So one walk over the characters produces both, and a component that produced only one would be walked twice
 * with two chances for the two answers to be measurements of different text.
 *
 * IT IS AN ACCUMULATOR AND NOT A FUNCTION OF ONE TEXT NODE, because css-text-3 §4.1.1 "Phase I: Collapsing and
 * Transformation" is stated over the FORMATTING CONTEXT and not over a node: a collapsible space is collapsed
 * against another "even one OUTSIDE THE BOUNDARY OF THE INLINE containing that space, provided both spaces are
 * within the same inline formatting context". An unbreakable segment crosses the same boundaries — `foo` and
 * `bar` in two adjacent text nodes are ONE word, and a per-node entry would report the wider of two halves as
 * the min-content size of a run that cannot break at all. The state that has to survive between nodes is
 * therefore the interface, not an implementation detail hidden behind one.
 *
 * IT COLLECTS THE CHARACTERS AND MEASURES AT `text_run_measure_finish`, WHICH THE RULES FORCE. [UAX14]'s rules
 * read FORWARD past the boundary they decide — LB25's `PO × OP IS NU` by three characters, LB15c's
 * `SP ÷ IS NU` by two, and LB9 allows an unbounded run of combining marks between the boundary and the
 * character that settles it — so no amount of per-character state can decide a boundary as the character
 * arrives. core/layout/line_break.h states that in full. Blink resolves it the same way, by collecting an
 * inline formatting context's text content into one buffer and running the break iterator over that. The
 * collection and that one [UAX14] pass are RELEASED BY `text_run_measure_release` and by nothing else, and the
 * span between `finish` and `release` is the whole life of the measurement: nothing is cached across it, for
 * core/layout/used_value.h's reason — a measurement is per-flow state, so a stored run width is shared state
 * solver/dom_cow.h does not swap and a stale one is another flow's text.
 *
 * THREE PARTITIONS OF ONE RUN, OVER ONE [UAX14] PASS — which is why the pass survives `finish` at all. The two
 * sizes above are css-sizing-3 §2.1's DEFINITIONS: they take NO opportunities and ALL of them, and neither
 * mentions a width. CSS 2.2 §9.4.2 "Inline formatting contexts" asks a third and different question — "when
 * several inline-level boxes cannot fit horizontally within a single line box, they are distributed among two
 * or more vertically-stacked line boxes" — which is a GREEDY FILL against the available width of each line, and
 * it is `text_run_measure_fill`. It is a third partition and NOT a generalisation of the other two: a fill at an
 * unbounded width and a fill at zero would reproduce their line sets, but by a chain of floating-point
 * comparisons where §2.1 needs none, so deriving a definition from a layout would make the definition depend on
 * arithmetic it is not stated over. All three read the SAME `actions` array, which is what makes it impossible
 * for them to disagree about where this run may break.
 *
 * §9.4.2's OVERFLOW SENTENCE IS WHY THE FILL NEEDS NO BOUND AND WHY `text_run_measure_splits` EXISTS. "If an
 * inline box cannot be split (e.g., if the inline box contains a single character, or language specific word
 * breaking rules disallow a break within the inline box, or if the inline box is affected by a white-space
 * value of nowrap or pre), then the inline box overflows the line box." So a segment always goes onto the
 * current line, however narrow that line is — the fill never has to invent a break — and a run with no break
 * position INSIDE it is ONE line box at EVERY available width. That second half is a theorem and not an
 * optimisation, and it is exported because it tells a caller whether it has to derive an available width at
 * all: a caller that would have to run CSS 2.1 §10.3 to answer a question whose answer cannot depend on it is
 * running a layout to discard it. The fill asserts the theorem rather than trusting the predicate.
 *
 * WHAT PHASE I LEAVES, FOR THE ONE `white-space` GROUP THIS COMPONENT MEASURES, is one sentence and it is
 * derived rather than assumed. Under `normal` or `nowrap` every space, tab and segment break is COLLAPSIBLE,
 * and §4.1.1's four steps compose to leave a maximal run of them as exactly ONE space:
 *   step 1 removes "any sequence of collapsible spaces and tabs immediately preceding or following a segment
 *     break", so a run containing a break keeps only the break;
 *   §4.1.3 "Segment Break Transformation Rules" then removes "any collapsible segment break immediately
 *     following another collapsible segment break" and transforms the one that is left "into a space (U+0020)
 *     or removed depending on the context before and after the break" — see below for why this component's
 *     content makes that choice determinate rather than UA-defined;
 *   step 3 converts "every collapsible tab" to a collapsible space;
 *   step 4 gives "any collapsible space immediately following another collapsible space" ZERO ADVANCE WIDTH —
 *     "(it is invisible, but retains its soft wrap opportunity, if any)".
 * Every path through those four leaves ONE space of nonzero advance per run, which is why the collection holds
 * a single U+0020 per run of white space rather than the characters it stood for.
 *
 * PHASE II IS APPLIED AT THE LINE AND NOT AT THE RUN, which is the whole reason the two answers differ in more
 * than one place. §4.1.2 "Phase II: Trimming and Positioning" removes "a sequence of collapsible spaces at the
 * beginning of a line" and again "at the end of a line" — a LINE, so which spaces disappear is a function of
 * WHERE THE BREAKS ARE, and the two answers take different breaks. The measurement therefore states one rule
 * and applies it twice: A LINE'S INLINE SIZE IS THE SUM OF ITS CHARACTERS' ADVANCES LESS ITS LEADING AND
 * TRAILING COLLAPSIBLE SPACES. For the max-content answer the lines are delimited by FORCED breaks only (CSS
 * 2.2 §10.3.5's own "formatting the content without breaking lines other than where explicit line breaks
 * occur"); for the min-content answer they are delimited by every break there is.
 *
 * A FORCED LINE BREAK IS THEREFORE MEASURED AND NOT REFUSED. css-text-3 §5.5 "Line Breaking Details":
 * "preserved segment breaks, and—regardless of the white-space value—any Unicode character with the BK and NL
 * line breaking class, must be treated as forced line breaks." U+000C FORM FEED and U+000B LINE TABULATION are
 * BK and U+0085 NEXT LINE is NL, and none of the three is collapsible under `normal`, so a `white-space:
 * normal` run can contain one and its max-content size is then the widest of the pieces rather than the whole.
 * That is a different statement from `white-space: pre`, which is refused: see text_run.c's `tr_wraps`, whose
 * crash names the three things preserved white space needs and which this does not supply any of.
 *
 * AND §4.1.3's UA-DEFINED CHOICE IS DETERMINATE HERE rather than being resolved by preference. The section
 * leaves "transformed into a space (U+0020) or removed" to the UA and says why the choice exists: "in
 * languages that use word separators, such as English and Korean, 'unbreaking' a line requires joining the two
 * lines with a space", while "in languages that have no word separators, such as Chinese, 'unbreaking' a line
 * requires eliminating any intervening white space". THE FIRST BRANCH IS TAKEN UNCONDITIONALLY AND THAT IS A
 * TAILORING THIS ENGINE HAS NOT BUILT: the antecedent is a fact about the CONTENT LANGUAGE, which css-text-3
 * §5's own list of controls (`word-break`, `line-break`, `overflow-wrap`, `hyphens`) is how a document states,
 * and core/css/css_computed_value.c derives a computed value for none of them. text_run.c asserts that rather
 * than recording it, at the same place it asserts the other three properties that would change this sum.
 *
 * WHAT IT DOES NOT MEASURE, AND WHY THAT IS AN ASSERT RATHER THAN A CAVEAT. css-text-3 §7.1 "Word Spacing: the
 * word-spacing property" and css-text-3 §7.2 "Tracking: the letter-spacing property" ADD to the advance of
 * exactly what this component sums, and css-text-3 §2.1 "Case Transforms: the text-transform property" changes WHICH characters it
 * sums. None of the three is a property core/css/css_computed_value.c derives a computed value for — so this
 * engine's cascade produces no answer for them anywhere, a declaration of one reaches no consumer, and there is
 * nothing here to read. That is a fact about the engine and not a decision this component made, which is why it
 * is asserted at the measurement: the day a row is added for any of the three, this crashes and names the sum
 * it has to enter. A comment saying "remember to ask" would be the same statement with nothing to enforce it.
 * KERNING AND LIGATURES ARE ABSENT FOR A DIFFERENT REASON and it is a property of the FACE: core/fonts/
 * default_font_data.c ships no 'GSUB' and no 'GPOS', so a run's advance IS the sum of its glyphs' advances,
 * with no pair adjustment and no substitution to make. That is the same statement core/layout/line_box.c makes
 * about the height, and it changes the day a face with those tables is loaded through the fetch chokepoint. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TEXT_RUN_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TEXT_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/line_break.h"

/* THE RUN IS A SEQUENCE OF ITEMS AND NOT OF CHARACTERS, because four different things occupy positions in it
 * and only one of them is text. css-sizing-3 §2.2 "Intrinsic Contributions" puts an inline box's own horizontal
 * margins, borders and padding into what it contributes ("based on the OUTER SIZE of the box"), and css-text-3
 * §5.5 "Line Breaking Details" places them at the box's TWO BOUNDARIES rather than at every break inside it —
 * "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the flow". So an
 * edge is A WIDTH AT A POSITION, and there is no code point to hang it on.
 * IT CANNOT BE A CHARACTER, and that is §5.5's sentence rather than a preference: [UAX14] decides breaks from
 * the code points it is given, so an edge smuggled in as one would create or forbid a break at the boundary
 * §5.5 says a box boundary must leave alone. It cannot be a total carried beside the collection either — an
 * inline box whose text spans three lines puts its left edge on the first and its right edge on the third, so
 * a sum added to the whole run would report a min-content size no line of that run ever has.
 * THE EDGE IS THEREFORE INVISIBLE TO THE BREAK PASS AND VISIBLE TO THE PER-LINE SUM, which is exactly the split
 * this type encodes: `line_break_actions` is handed the code points of the items that HAVE one, and the
 * per-line measurement walks ITEMS. The two index spaces that creates are reconciled at ONE place
 * (`text_run.c`'s `tr_item_boundary_of_cp`), because a break position expressed in the wrong one is a fragment
 * of an inline box placed on the wrong line and nothing downstream can contradict it.
 *
 * THE FORCED BREAK IS THE THIRD THING, AND IT IS THE EXACT COMPLEMENT OF THE EDGE: no width, and a break the
 * run's own text does not contain. HTML §15.3.4 "Phrasing content" declares `br { display-outside: newline; }`
 * and no CSS module defines that keyword (core/layout/phrasing_break.h states that in full), so the element
 * arrives as a plain `display: inline` box carrying a fact the cascade cannot answer for.
 * WHAT IT IS NOT is an EDGE with a flag, and the two differ in every field that matters: an edge contributes an
 * inline size and no break, this contributes a break and no size. It is not a CHARACTER either, and that is the
 * distinction the kind exists to keep — css-values-4 §6.1.1's advance measure would be summed for it, and CSS
 * 2.2 §9.4.2's zero-height rule asks "line boxes that contain no TEXT" separately from "and do not end with a
 * preserved newline", so an item answering the first question would answer the wrong one of the two.
 * WHAT IT DOES CONTRIBUTE IS A CODE POINT, AND [UAX14] DECIDES ITS TWO BOUNDARIES rather than this component
 * overriding them. §15.3.4's keyword is `newline`, and css-text-3 §4 "White Space Processing & Control
 * Characters" says which code point HTML's newline is: "in the case of HTML, newlines are normalized to line
 * feed characters (U+000A) for representation in the DOM, so when an HTML document is represented as a DOM tree
 * each line feed (U+000A) is treated as a segment break" — and §5.5 makes a PRESERVED segment break a forced
 * line break. So the declaration supplies the U+000A the document does not contain, and [UAX14] LB6
 * `× ( BK | CR | LF | NL )` then forbids a break before it while LB5 `LF !` makes the break after it mandatory.
 * That is why there is ONE [UAX14] pass and no second mechanism deciding where a `br` breaks: an action written
 * here beside actions the annex computed would be two rule sets over one run.
 *
 * THE ATOMIC INLINE IS THE FOURTH THING AND IT IS THE ONE THAT HAS BOTH — a WIDTH like an edge and a code point
 * like a forced break — which is exactly why it is not either of them wearing an extra field. css-text-3 §5.5
 * "Line Breaking Details": "for Web-compatibility there is a SOFT WRAP OPPORTUNITY BEFORE AND AFTER each
 * REPLACED ELEMENT OR OTHER ATOMIC INLINE, even when adjacent to a character that would normally suppress them,
 * including U+00A0 NO-BREAK SPACE." So it introduces the two breaks §5.5 says an inline box boundary must not
 * (which is what stops it being an EDGE), and CSS 2.2 §9.4.2 puts its whole box on the line — "horizontal
 * margins, borders, and padding are respected between these boxes" — so it occupies an inline size a forced
 * break does not have. CSS 2.2 §9.2.2 "Inline-level elements and inline boxes" says why one item is the whole
 * of it: atomic inline-level boxes are so called "because they participate in their inline formatting context
 * as a single opaque box", so one is never split across lines and there is nothing inside it for the run to hold.
 * ONE KIND SERVES A REPLACED ELEMENT AND AN `inline-block` ALIKE, and that is §5.5's own sentence rather than a
 * generalisation: it names "each replaced element OR OTHER atomic inline" as one category. What differs between
 * them is only where the WIDTH comes from (CSS 2.2 §10.3.2 "Inline, replaced elements" for the first, §10.3.9
 * "'Inline-block', non-replaced elements in normal flow" for the second), which is the CALLER's derivation and
 * not a fact this run records.
 * ITS CODE POINT IS U+FFFC OBJECT REPLACEMENT CHARACTER, whose [UAX14] Line_Break class is CB — and that is a
 * derivation of §5.5's two sentences rather than a convenient stand-in. LB20 `÷ CB`, `CB ÷` ("break before and
 * after unresolved CB") is the opportunity on each side that §5.5's first sentence requires; and because LB8a
 * `ZWJ ×`, LB11 `× WJ`, `WJ ×`, LB12 `GL ×` and LB12a `[^SP BA HY] × GL` are all EARLIER rules than LB20, the
 * annex already prohibits exactly what §5.5's second sentence prohibits — "with the exception of U+00A0
 * NO-BREAK SPACE, there must be no soft wrap opportunity between atomic inlines and adjacent characters
 * belonging to the Unicode GL, WJ, or ZWJ line breaking classes". The NAMED EXCEPTION is the one place the two
 * disagree, since U+00A0 is itself GL, and text_run.c applies it as a stated §5.5 tailoring of the one pass —
 * see `tr_atomic_nbsp_tailoring` there for why that is one rule set and not two. */
typedef enum {
    TEXT_RUN_ITEM_CHAR = 0,   /* one code point surviving css-text-3 §4.1.1's Phase I */
    TEXT_RUN_ITEM_EDGE,       /* an inline box boundary: an inline size, and no code point at all */
    TEXT_RUN_ITEM_FORCED_BREAK, /* HTML §15.3.4's `display-outside: newline`: a code point, and no size */
    TEXT_RUN_ITEM_ATOMIC,     /* css-text-3 §5.5's replaced element or other atomic inline: a size AND a code
                                 point — CSS 2.2 §9.4.2's margin box on the line, and the U+FFFC whose [UAX14]
                                 class CB puts an opportunity on each side of it */
} TextRunItemKind;

/* ONE ITEM OF THE COLLECTED RUN. The inline it belongs to travels with it because both of the properties this
   measurement reads are per-element and the run crosses elements: css-values-4 §6.1.1's advance measure is
   stated over "the element on which it is used", and css-text-3 §5.5 "Line Breaking Details" says which
   element's `white-space` governs a break — "for soft wrap opportunities created by characters that disappear
   at the line break (e.g. U+0020 SPACE), properties on the box DIRECTLY CONTAINING THAT CHARACTER control the
   line breaking at that opportunity", and otherwise "the white-space property on the NEAREST COMMON ANCESTOR
   of the two characters controls breaking".
   THE FIELDS OF THE OTHER KIND ARE NEVER READ, AND THAT IS ASSERTED RATHER THAN ARRANGED: text_run.c reaches
   `cp`, `wraps` and `collapsible_space` only through accessors that DCHECK the kind, so a walk that forgot an
   item is an edge crashes at the read instead of measuring a zero-width U+0000 that no document contains. */
typedef struct {
    TextRunItemKind kind;
    lxb_dom_element_t *style;
    /* THE CODE POINT [UAX14] DECIDES THIS ITEM'S BOUNDARIES FROM — TEXT_RUN_ITEM_CHAR,
       TEXT_RUN_ITEM_FORCED_BREAK and TEXT_RUN_ITEM_ATOMIC, which are exactly the kinds that have one. For a
       character it is the character; for a forced break it is the U+000A css-text-3 §4 names as HTML's newline,
       supplied by HTML §15.3.4's declaration rather than by the document; for an atomic inline it is the U+FFFC
       whose class CB is what css-text-3 §5.5's "soft wrap opportunity before and after" is expressed as. An
       EDGE has none and its slot is never read. */
    uint32_t cp;
    /* css-text-3 §3's `nowrap` "suppresses line breaks (text wrapping) within the source" while leaving white
       space collapsible, so whether an opportunity EXISTS is a second fact about the inline and not derivable
       from the first. Recorded per character because §5.5 makes it a per-element question. */
    bool wraps;
    /* Whether this character is one of §4.1.1's COLLAPSIBLE SPACES — the one U+0020 a run of white space
       collapsed to. §4.1.2 trims exactly these at a line's two edges, and a U+0020 that is content under some
       other `white-space` value would not be trimmable, so the flag is carried rather than re-derived from the
       code point. */
    bool collapsible_space;
    /* THE INLINE SIZE THIS ITEM OCCUPIES ON ITS LINE, for the two kinds that have one and NOT the same quantity
       for both. TEXT_RUN_ITEM_EDGE holds css-sizing-3 §2.2's outer-size contribution at ONE boundary of an
       inline box, so the box's two edges carry its two halves; TEXT_RUN_ITEM_ATOMIC holds the WHOLE margin box
       (CSS 2 §8.1's outer edge) of a box CSS 2.2 §9.2.2 makes "a single opaque box", so there is no second item
       for the other half. They share a slot because the per-line sum does the same thing with both — add it at
       the position the item sits at — and they are told apart by the KIND, which is what every accessor below
       asserts on. A CHARACTER's contribution is css-values-4 §6.1.1's advance measure of its own code point and
       is not stored, and a FORCED BREAK contributes none at all. */
    CssPx size;
} TextRunItem;

/* THE RUNNING STATE OF ONE INLINE FORMATTING CONTEXT'S MEASUREMENT. The fields are written ONLY by the entries
   below and read only through them, which is what their asserts are for. */
typedef struct {
    /* css-text-3 §4.1.1's Phase I output interleaved with the box edges §5.5 places between its characters, in
       document order. Owned; released by `finish`. */
    TextRunItem *items;
    size_t count, capacity;
    /* Whether the last character added was inside a run of collapsible white space, so the next one does not
       open a second surviving space for the same run — §4.1.1 step 4's "any collapsible space immediately
       following another collapsible space" taken as a property of the run. It survives between nodes, which is
       what makes a run split across two text nodes collapse as ONE. */
    bool in_white_space_run;
    /* [UAX14]'s ACTION AT EVERY POSITION of the run's own CODE POINTS — `actions[k]` is the action at the
       boundary immediately before code point `k`, and `item_of_cp[k]` is the ITEM that code point belongs to.
       The code points are the run's characters, its forced breaks AND its atomic inlines, which are the three
       kinds that have one; an inline box edge contributes none, per css-text-3 §5.5. Both arrays are written by
       `finish`, which also applies §5.5's ONE tailoring of the annex (the U+00A0 exception beside an atomic
       inline — see text_run.c's `tr_atomic_nbsp_tailoring`), so what they hold is [UAX14] AS CSS 2.2 REQUIRES
       IT and every partition below reads that one answer rather than re-deriving it. They are RETAINED until
       `release`, because all three partitions above are walks over this one pass: a second
       `line_break_actions` call for the fill would be the same rules run twice with two chances to disagree
       about where this run may break. Owned; NULL exactly while `ncps` is zero. */
    LineBreakAction *actions;
    size_t *item_of_cp;
    size_t ncps;
    /* css-sizing-3 §2.1's two answers, written by `finish` and undefined before it. */
    CssPx max_content;
    CssPx min_content;
    /* CSS 2.2 §9.4.2's own theorem, written by `finish`: is there a position STRICTLY INSIDE this run that is a
       forced line break or an ENABLED soft wrap opportunity. [UAX14] LB3 makes the position at end of text
       mandatory and that one is not a split — it closes the run rather than dividing it — so this is false for
       a run the section says "overflows the line box" rather than being distributed. */
    bool splits;
    bool finished;
    /* `release` has run. The two sizes and the fill are all derived from a collection it freed, so this is the
       end of the measurement and not a step in it — read through the same assert the answers go through. */
    bool released;
} TextRunMeasure;

/* BEGIN a formatting context's measurement. Every field is written, so a caller reading one back has a
   measurement rather than whatever its storage held. */
void text_run_measure_init(TextRunMeasure *m);

/* ADD ONE TEXT NODE's characters, in tree order, to the measurement. `style` is the element whose computed
   properties the characters have — the text node's own parent, which is the inline box they are in — and the
   caller must add the nodes of one formatting context in document order, because §4.1.1's collapsing and
   [UAX14]'s rules are both stated over ADJACENT characters.
   THE CALLER HAS ALREADY ESTABLISHED that this run generates a box: CSS 2.2 §9.2.2.1 "Anonymous inline boxes"
   removes a wholly-collapsible run between block-level boxes before it is anything to measure, and
   core/layout/block_flow.h answers that question once for every walk. A run this component is handed is
   measured as it stands. */
void text_run_measure_add_text(TextRunMeasure *m, lxb_dom_element_t *style, const lxb_dom_node_t *text);

/* ADD ONE INLINE BOX BOUNDARY at the current position of the run — css-sizing-3 §2.2's outer-size contribution
   for one side of `style`'s box, in CSS pixels. Called at the two boundaries and nowhere between them, which is
   css-text-3 §5.5's placement: the edges are at the box's boundaries and not at every break inside it, so a box
   whose text spans three lines puts its opening edge on the first line and its closing edge on the third.
   IT DOES NOT SAY WHICH SIDE IT IS, AND THAT IS THE POINT: what an edge does to the measurement is add its size
   to whichever line the item lands on, and which line that is falls out of the run's ORDER — the caller emits
   the opening edge before the box's content and the closing edge after it, and text_run.c's break-position
   mapping does the rest. A `side` argument would be a second statement of the same fact with its own way of
   disagreeing with the call order.
   `size` MAY BE ZERO and that is not a call to skip: a zero-width edge still occupies a POSITION, which is what
   decides the line an otherwise-empty inline box is on. */
void text_run_measure_add_box_edge(TextRunMeasure *m, lxb_dom_element_t *style, CssPx size);

/* ADD ONE FORCED LINE BREAK at the current position of the run — HTML §15.3.4 "Phrasing content"'s
   `br { display-outside: newline; }`, which core/layout/phrasing_break.h is what recognises. `style` is the
   element the break IS, and it is not a bookkeeping field: CSS 2.2 §10.8's step 1 is over every inline-level
   box on the line and this one is on it, so its own `line-height` and first available font are two of the
   operands of the line's height — `<br style="line-height:100px">` makes a line 100px tall in every user
   agent, and the element is where that number is read from.
   IT TAKES NO CODE POINT ARGUMENT, because the code point is not the caller's to choose: §15.3.4's keyword is
   `newline` and css-text-3 §4 says which code point HTML's newline is, so the U+000A is a fact about the
   declaration and belongs beside the [UAX14] pass that consumes it. A `cp` parameter would let two callers
   disagree about what a `br` is.
   IT CONTRIBUTES NO ADVANCE. The break renders nothing, and the U+000A above exists to be given to [UAX14] and
   to nothing else — measuring css-values-4 §6.1.1's advance for it would put the first available font's
   .notdef width on the line for a box that draws no glyph. */
void text_run_measure_add_forced_break(TextRunMeasure *m, lxb_dom_element_t *style);

/* ADD ONE ATOMIC INLINE at the current position of the run — css-text-3 §5.5 "Line Breaking Details"' "each
   replaced element or other atomic inline". `size` is the box's MARGIN BOX inline size (CSS 2 §8.1 "Box
   dimensions": "the four margin edges define the box's margin box"), which is what CSS 2.2 §9.4.2 puts on the
   line: "horizontal margins, borders, and padding are respected between these boxes".
   IT IS ONE CALL AND NOT A PAIR OF EDGES, which is CSS 2.2 §9.2.2 "Inline-level elements and inline boxes"
   rather than a shorthand: atomic inline-level boxes are so called "because they participate in their inline
   formatting context as a single opaque box", so one is never split across two line boxes and there is no
   inside of it for the run to hold. Bracketing it with two edges would additionally place two boundaries §5.5 says introduce no break
   around a box whose whole point is that it introduces two.
   THE SIZE IS THE CALLER'S DERIVATION AND THE SECTION DEPENDS ON THE BOX TYPE — CSS 2.2 §10.3.2 "Inline,
   replaced elements" for a replaced element, §10.3.9 "'Inline-block', non-replaced elements in normal flow" for
   an `inline-block`, and its own module's for an inline-flex or inline-grid — which is why this entry takes the
   number rather than deriving it: a component that chose the section would be classifying the box a second
   time, in a file that has no reason to know the difference.
   IT TAKES NO CODE POINT, for `text_run_measure_add_forced_break`'s reason: WHICH code point expresses §5.5's
   two opportunities is a fact about the section and not the caller's to vary, so two callers cannot disagree
   about what an atomic inline is to [UAX14].
   `size` MAY BE ZERO — HTML §15.4.2's fourth rule gives an `img` that represents nothing natural dimensions of
   0, and CSS 2.2 §10.3.2's first arm takes that width whole — and it may be NEGATIVE, because CSS 2.2 §8.3
   "Margin properties" allows negative margins and the margin box is the border box plus both of them. Neither
   is a call to skip: the item's POSITION is what puts §5.5's two opportunities on the line. */
void text_run_measure_add_atomic(TextRunMeasure *m, lxb_dom_element_t *style, CssPx size);

/* RUN [UAX14] OVER EVERYTHING COLLECTED and produce css-sizing-3 §2.1's two answers. Every caller must reach
   this: the measurement does not exist until it runs. Adding a node afterwards is a caller that ran two
   measurements through one accumulator.
   IT NO LONGER FREES ANYTHING, and that is what makes CSS 2.2 §9.4.2's fill a walk over the same [UAX14] pass
   rather than a second one. `text_run_measure_release` is the call that ends the measurement, and every caller
   of this owes it exactly one. */
void text_run_measure_finish(TextRunMeasure *m);

/* END THE MEASUREMENT and free the collection and the [UAX14] pass. Nothing on the accumulator may be read
   afterwards — the two sizes are derived from bytes this call returns to the allocator, so a read after it is
   answered from freed memory whether or not the numbers happen to survive in the struct, and the accessors
   below crash rather than letting one through. */
void text_run_measure_release(TextRunMeasure *m);

/* ONE LINE BOX of CSS 2.2 §9.4.2's distribution — the half-open run `[from, to)` of COLLECTED ITEMS that landed
   on it, and the inline size those items occupy once css-text-3 §4.1.2 "Phase II: Trimming and Positioning" has
   removed the collapsible spaces at its two ends.
   THE RANGE IS OVER ITEMS AND NOT OVER CHARACTERS because that is what a consumer needs: CSS 2.2 §10.8's step 3
   is two maxima over the inline-level BOXES on the line, and an inline box is on a line exactly when one of its
   items is — which is the same reason `text_run_measure_add_box_edge` takes no side argument. */
typedef struct {
    size_t from, to;
    CssPx size;
} TextRunLine;

/* CSS 2.2 §9.4.2's DISTRIBUTION of this run's content across line boxes at one AVAILABLE WIDTH — "the width of
   a line box is determined by a containing block and the presence of floats", so `available` is the used
   content width of the box that establishes this formatting context, and a caller with a float in the context
   has a different width per line and is not this entry's caller yet.
   IT ANSWERS THE NUMBER OF LINE BOXES and stores a newly allocated array of that many `TextRunLine`s at
   `*lines`, WHICH THE CALLER OWNS AND MUST FREE. The lines PARTITION the collected items in order — every item
   is on exactly one line, including a box edge that trails the last character — because §9.4.2's own count is
   what CSSOM VIEW §6's `getClientRects()` step 3 reports as a fragment count, and a partition is the only shape
   in which "which line is this box on" has an answer for every box.
   IT IS GREEDY, WHICH IS §9.4.2 AND NOT A HEURISTIC: the section breaks only when boxes "cannot fit
   horizontally within a single line box" and its overflow sentence says what to do when even one cannot, so the
   line takes the widest prefix that fits and takes one segment regardless when nothing does. `available` is
   IGNORED — provably, and asserted — when `text_run_measure_splits` is false. */
size_t text_run_measure_fill(const TextRunMeasure *m, CssPx available, TextRunLine **lines);

/* CSS 2.2 §9.4.2's theorem: is there anywhere INSIDE this run for the fill to break. False means one line box
   at every available width, so a caller need not derive one. See the header above. */
bool text_run_measure_splits(const TextRunMeasure *m);

/* WHERE ITEM `i` BEGINS ALONG THE LINE BOX `line` — the inline-axis distance from the LINE BOX'S OWN START EDGE
 * to that item's start edge, in CSS pixels, with css-text-3 §4.1.2 "Phase II: Trimming and Positioning" applied
 * over the whole line exactly as `TextRunLine.size` has it applied.
 *
 * IT IS THE THIRD ANSWER OVER THE SAME FILL, AND IT IS A DISTANCE AND NOT A COORDINATE. The fill says WHICH
 * line each item is on and how wide each line's content is; §10.8 (core/layout/line_box.h) says how tall each
 * line is; this says where along its line an item sits. What none of the three says is where the line box's
 * start edge IS, and that is deliberate: CSS 2.2 §9.4.2 gives the line box the containing block's width ("in
 * general, the left edge of a line box touches the left edge of its containing block and the right edge touches
 * the right edge of its containing block") and then hands the content's position inside it to css-text-4 §7.3
 * "Default Text Alignment: the text-align-all property" — a computed value about the ESTABLISHING BOX, which
 * this component is not given and must not read, since it is handed a run rather than an element.
 *
 * `i == line.to` IS ADMITTED AND IS THE LINE'S OWN SIZE — where the last item ENDS. A fragment is a half-open
 * range of items exactly as a line is, so its two edges are this entry at its two bounds, and a caller that had
 * to special-case the far one would be re-deriving `TextRunLine.size` beside a number that already is it.
 *
 * IT SHARES ONE WALK WITH `TextRunLine.size` RATHER THAN REPRODUCING IT, which is the same rule text_run.h's
 * three partitions follow one level up and matters for the same reason: §4.1.2's trimming is a property of the
 * LINE, so a position derived from a prefix that re-trimmed would not add up to the size derived from the
 * whole, and the offsets of a line's items would not reach its own end. Two sums of one line is the one way
 * this component could hand a caller a fragment whose left edge and width describe different text. */
CssPx text_run_measure_line_offset(const TextRunMeasure *m, TextRunLine line, size_t i);

/* THE ELEMENT WHOSE COMPUTED PROPERTIES ITEM `i`'s BOX HAS — the inline box a character is in, or the box an
   edge belongs to. It is the one field asked WITHOUT a kind because both kinds carry it and for the same
   reason: css-values-4 §6.1.1's advance measure is stated over "the element on which it is used" and
   css-sizing-3 §2.2's outer size is a fact about one box, so an item with no element would be a measurement
   with no typography.
   THE RAW ITEM IS DELIBERATELY NOT EXPOSED. Every other field belongs to exactly one kind, and this file's own
   asserts exist because a walk that lost track of which kind it is standing on would measure an edge as a
   U+0000 or sum a character's uninitialised size — handing a consumer the struct would move that defect
   outside the component that can catch it. A consumer asks the questions it has, and there are two. */
lxb_dom_element_t *text_run_measure_item_style(const TextRunMeasure *m, size_t i);

/* IS ITEM `i` A CHARACTER. CSS 2.2 §9.4.2's zero-height rule opens with "line boxes that contain NO TEXT" and
   a consumer deciding whether one of the fill's lines exists is asking exactly this of each of its items. A
   forced break is NOT text for that list: it draws nothing, and the section asks about it in a separate
   conjunct with a separate answer — which is the next entry. */
bool text_run_measure_item_is_text(const TextRunMeasure *m, size_t i);

/* IS ITEM `i` A FORCED LINE BREAK. CSS 2.2 §9.4.2's zero-height rule ends with "and do not end with a preserved
   newline", which is the ONE conjunct of that list a line can satisfy while holding nothing that renders — so a
   consumer asking whether a line of the fill exists asks this of the line's last rendering item as well as
   asking the question above of all of them. The two are separate entries because the section states them as
   separate facts, and a consumer that folded them together would report `<div><br></div>` as a box containing
   no line box at all — which CSS 2.2 §8.3.1's collapse-through note reads as a box whose margins collapse
   through it, for a document that renders one line of text height. */
bool text_run_measure_item_is_forced_break(const TextRunMeasure *m, size_t i);

/* IS ITEM `i` AN ATOMIC INLINE. CSS 2.2 §9.4.2's zero-height rule ends its list with "and NO OTHER IN-FLOW
   CONTENT", which is the conjunct an atomic inline satisfies and neither of the other two entries answers: it
   is not TEXT (it draws no glyph and §9.4.2 asks about text separately) and it is not a preserved newline. A
   consumer deciding whether one of the fill's lines exists therefore asks all three, and a line holding an
   `<img>` exists however empty the rest of it is.
   IT IS ALSO WHAT SEPARATES THE TWO SIZED KINDS at CSS 2.2 §10.8's step 1, which gives them different answers
   from different sentences: "for REPLACED elements, inline-block elements, and inline-table elements, this is
   the height of their MARGIN BOX; for inline boxes, this is their 'line-height'." An EDGE belongs to a box on
   the second side of that semicolon and this item is the first, so a walk over a line's items cannot take one
   height for both. */
bool text_run_measure_item_is_atomic(const TextRunMeasure *m, size_t i);

/* css-sizing-3 §2.1's MAX-CONTENT and MIN-CONTENT INLINE SIZES, in CSS pixels — the two answers, read through
   entries rather than off the struct so that the one relation between them is asserted where it is read. */
CssPx text_run_measure_max_content(const TextRunMeasure *m);
CssPx text_run_measure_min_content(const TextRunMeasure *m);

#endif
