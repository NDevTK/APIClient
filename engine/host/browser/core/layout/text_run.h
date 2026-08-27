/* THE INLINE-AXIS MEASUREMENT OF A TEXT RUN — css-text-3 §4.1 "The White Space Processing Rules" for which
 * characters are there to measure, css-text-3 §5 "Line Breaking and Word Boundaries" for where the run may
 * break, and css-values-4 §6.1.1's ADVANCE MEASURE for how wide each surviving one is.
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
 * WHAT PHASE I AND PHASE II LEAVE, FOR THE ONE `white-space` GROUP THIS COMPONENT MEASURES, is one sentence and
 * it is derived rather than assumed. Under `normal` or `nowrap` every space, tab and segment break is
 * COLLAPSIBLE, and §4.1.1's four steps compose to leave a maximal run of them as exactly ONE space:
 *   step 1 removes "any sequence of collapsible spaces and tabs immediately preceding or following a segment
 *     break", so a run containing a break keeps only the break;
 *   §4.1.3 "Segment Break Transformation Rules" then removes "any collapsible segment break immediately
 *     following another collapsible segment break" and transforms the one that is left "into a space (U+0020)
 *     or removed depending on the context before and after the break" — see below for why this component's
 *     alphabet makes that choice determinate rather than UA-defined;
 *   step 3 converts "every collapsible tab" to a collapsible space;
 *   step 4 gives "any collapsible space immediately following another collapsible space" ZERO ADVANCE WIDTH —
 *     "(it is invisible, but retains its soft wrap opportunity, if any)".
 * Every path through those four leaves ONE space of nonzero advance per run, which is why this component
 * carries a single pending space rather than a count. §4.1.2 "Phase II: Trimming and Positioning" then removes
 * "a sequence of collapsible spaces at the beginning of a line" and again "at the end of a line" — so the run's
 * leading white space is dropped on both answers, its trailing white space is dropped on both, and an INTERIOR
 * run is dropped on the min-content answer alone, because that is the answer in which the break is taken there
 * and the space is consequently at the end of a line.
 *
 * WHERE THE RUN MAY BREAK IS UAX14's QUESTION AND THIS COMPONENT ANSWERS IT FOR A STATED ALPHABET AND CRASHES
 * FOR EVERY OTHER CHARACTER. css-text-3 §5 makes the break positions a property of the content — "valid soft
 * wrap opportunities depend on the content language and writing system", "in most writing systems, in the
 * absence of hyphenation a soft wrap opportunity occurs only at word boundaries" — and §5.5 "Line Breaking
 * Details" hands the rules themselves to [UAX14], whose Line_Break property is a table of every Unicode scalar
 * value. THIS ENGINE SHIPS NO SUCH TABLE, and the failure mode of guessing one is invisible: a break this
 * component does not know about makes the min-content size TOO LARGE, a shrink-to-fit box TOO WIDE, and nothing
 * anywhere disagrees. So the alphabet is enumerated, its members' Line_Break values are the ones
 * https://www.unicode.org/Public/UCD/latest/ucd/LineBreak.txt assigns them, and a character outside it CRASHES
 * naming the property table as the thing to build:
 *   SP — U+0020 SPACE, plus U+0009 TAB and U+000A LINE FEED and U+000D CARRIAGE RETURN, which Phase I above has
 *        already turned into one.
 *   AL — the ASCII letters and the ASCII symbols LineBreak.txt gives class AL: `#`, `&`, `*`, `<`, `=`, `>`,
 *        `@`, `^`, `_`, `` ` ``, `~`.
 *   NU — the ASCII digits.
 * OVER THAT ALPHABET UAX14's ORDERED RULES DECIDE EVERY ADJACENT PAIR AND ITS DEFAULT IS NEVER REACHED, which
 * is the property that makes the answer exact rather than approximate: LB7 "do not break before spaces or zero
 * width space" and LB18 "break after spaces" fix both sides of a space run, LB28 "do not break between
 * alphabetics" fixes AL×AL, LB23 "do not break between digits and letters" fixes AL×NU and NU×AL, and LB25's
 * last production `NU ( SY | IS )* × NU` fixes NU×NU. LB31 "break everywhere else" therefore applies to no pair
 * this component admits — asserted, because that is exactly the claim that would silently stop being true if
 * the alphabet were widened by hand.
 * THE SOFT WRAP OPPORTUNITY IS AFTER THE SPACE RUN AND NOT BEFORE IT, which LB7 and LB18 state as two rules and
 * which is why the space is billed to the line that PRECEDES the break and is then trimmed off it by §4.1.2.
 *
 * AND §4.1.3's UA-DEFINED CHOICE IS DETERMINATE OVER THIS ALPHABET rather than being resolved by preference.
 * The section leaves "transformed into a space (U+0020) or removed" to the UA and says why the choice exists:
 * "in languages that use word separators, such as English and Korean, 'unbreaking' a line requires joining the
 * two lines with a space", while "in languages that have no word separators, such as Chinese, 'unbreaking' a
 * line requires eliminating any intervening white space". Every character this component admits is ASCII AL or
 * NU — a word-separator writing system, and the only one it admits — so the first branch is the one whose
 * antecedent is true here. The day a character from the second is admitted, that is a rule to build alongside
 * its Line_Break value and not a default to inherit; the crash for it is already the one above.
 *
 * WHAT IT DOES NOT MEASURE, AND WHY THAT IS AN ASSERT RATHER THAN A CAVEAT. css-text-3 §7.1 "Word Spacing: the
 * word-spacing property" and §7.2 "Tracking: the letter-spacing property" ADD to the advance of exactly what
 * this component sums, and §2.1 "Case Transforms: the text-transform property" changes WHICH characters it
 * sums. None of the three is a property core/css/css_computed_value.c derives a computed value for — so this
 * engine's cascade produces no answer for them anywhere, a declaration of one reaches no consumer, and there is
 * nothing here to read. That is a fact about the engine and not a decision this component made, which is why it
 * is asserted at the measurement: the day a row is added for any of the three, this crashes and names the sum
 * it has to enter. A comment saying "remember to ask" would be the same statement with nothing to enforce it.
 * KERNING AND LIGATURES ARE ABSENT FOR A DIFFERENT REASON and it is a property of the FACE: core/fonts/
 * default_font_data.c ships no 'GSUB' and no 'GPOS', so a run's advance IS the sum of its glyphs' advances,
 * with no pair adjustment and no substitution to make. That is the same statement core/layout/line_box.c makes
 * about the height, and it changes the day a face with those tables is loaded through the fetch chokepoint.
 *
 * NOTHING IS STORED, for core/layout/used_value.h's reason: a measurement is per-flow state, so a cached run
 * width is shared state solver/dom_cow.h does not swap and a stale one is another flow's text. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TEXT_RUN_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TEXT_RUN_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* THE RUNNING STATE OF ONE INLINE FORMATTING CONTEXT'S MEASUREMENT. The fields are exposed because a caller
   that walks a box's children owns the walk and this owns the text; they are written ONLY by the entries
   below, and read only through them, which is what the two accessors' asserts are for. */
typedef struct {
    /* css-sizing-3 §2.1's max-content inline size of everything added so far: the whole run on ONE line, with
       §4.1.2's leading collapsible white space already dropped and its trailing run not yet placed. */
    CssPx max_content;
    /* §2.1's min-content inline size: the widest segment between soft wrap opportunities seen so far. */
    CssPx min_content;
    /* The segment currently open — the characters since the last soft wrap opportunity. */
    CssPx segment;
    /* §4.1.1's ONE surviving collapsible space of the run in progress, held until a character follows it: it is
       placed if one does (§4.1.2 trims only at a line's edges) and dropped if none does. Its advance is the
       first white-space character's own inline's, which is what §4.1.1 step 4 leaves and what §4.1.3's
       transformed break is. */
    CssPx pending_space;
    bool  has_pending_space;
    /* Whether a soft wrap opportunity exists after that space — css-text-3 §3's `nowrap` removes it while
       leaving the space collapsible, so the two facts are recorded together and not derived from each other. */
    bool  pending_space_wraps;
    /* Whether any character has been placed — §4.1.2's "beginning of a line", which is the whole difference
       between a leading collapsible run and an interior one. */
    bool  any_glyph;
} TextRunMeasure;

/* BEGIN a formatting context's measurement. Every field is written, so a caller reading one back has a
   measurement rather than whatever its storage held. */
void text_run_measure_init(TextRunMeasure *m);

/* ADD ONE TEXT NODE's characters, in tree order, to the measurement. `style` is the element whose computed
   properties the characters have — the text node's own parent, which is the inline box they are in — and the
   caller must add the nodes of one formatting context in document order, because §4.1.1's collapsing and
   UAX14's rules are both stated over ADJACENT characters.
   THE CALLER HAS ALREADY ESTABLISHED that this run generates a box: CSS 2.2 §9.2.2.1 "Anonymous inline boxes"
   removes a wholly-collapsible run between block-level boxes before it is anything to measure, and
   core/layout/block_flow.h answers that question once for every walk. A run this component is handed is
   measured as it stands. */
void text_run_measure_add_text(TextRunMeasure *m, lxb_dom_element_t *style, const lxb_dom_node_t *text);

/* css-sizing-3 §2.1's MAX-CONTENT and MIN-CONTENT INLINE SIZES of everything added so far, in CSS pixels — the
   two answers, read through entries rather than off the struct so that the one relation between them is
   asserted where it is read. */
CssPx text_run_measure_max_content(const TextRunMeasure *m);
CssPx text_run_measure_min_content(const TextRunMeasure *m);

#endif
