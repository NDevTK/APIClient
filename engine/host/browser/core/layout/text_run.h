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
 * collection is FREED by `finish`; nothing survives the call, for core/layout/used_value.h's reason — a
 * measurement is per-flow state, so a cached run width is shared state solver/dom_cow.h does not swap and a
 * stale one is another flow's text.
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
 * about the height, and it changes the day a face with those tables is loaded through the fetch chokepoint. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_TEXT_RUN_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_TEXT_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* ONE CHARACTER OF THE COLLECTED RUN, after css-text-3 §4.1.1's Phase I. The inline it belongs to travels with
   it because both of the properties this measurement reads are per-element and the run crosses elements:
   css-values-4 §6.1.1's advance measure is stated over "the element on which it is used", and css-text-3 §5.5
   "Line Breaking Details" says which element's `white-space` governs a break — "for soft wrap opportunities
   created by characters that disappear at the line break (e.g. U+0020 SPACE), properties on the box DIRECTLY
   CONTAINING THAT CHARACTER control the line breaking at that opportunity", and otherwise "the white-space
   property on the NEAREST COMMON ANCESTOR of the two characters controls breaking". */
typedef struct {
    uint32_t cp;
    lxb_dom_element_t *style;
    /* css-text-3 §3's `nowrap` "suppresses line breaks (text wrapping) within the source" while leaving white
       space collapsible, so whether an opportunity EXISTS is a second fact about the inline and not derivable
       from the first. Recorded per character because §5.5 makes it a per-element question. */
    bool wraps;
    /* Whether this character is one of §4.1.1's COLLAPSIBLE SPACES — the one U+0020 a run of white space
       collapsed to. §4.1.2 trims exactly these at a line's two edges, and a U+0020 that is content under some
       other `white-space` value would not be trimmable, so the flag is carried rather than re-derived from the
       code point. */
    bool collapsible_space;
} TextRunChar;

/* THE RUNNING STATE OF ONE INLINE FORMATTING CONTEXT'S MEASUREMENT. The fields are written ONLY by the entries
   below and read only through them, which is what their asserts are for. */
typedef struct {
    /* css-text-3 §4.1.1's Phase I output, in document order. Owned; released by `finish`. */
    TextRunChar *chars;
    size_t count, capacity;
    /* Whether the last character added was inside a run of collapsible white space, so the next one does not
       open a second surviving space for the same run — §4.1.1 step 4's "any collapsible space immediately
       following another collapsible space" taken as a property of the run. It survives between nodes, which is
       what makes a run split across two text nodes collapse as ONE. */
    bool in_white_space_run;
    /* css-sizing-3 §2.1's two answers, written by `finish` and undefined before it. */
    CssPx max_content;
    CssPx min_content;
    bool finished;
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

/* RUN [UAX14] OVER EVERYTHING COLLECTED and produce the two answers, then release the collection. Every caller
   must reach this: the measurement does not exist until it runs, and the memory the walk collected is this
   call's to free. Adding a node afterwards is a caller that ran two measurements through one accumulator. */
void text_run_measure_finish(TextRunMeasure *m);

/* css-sizing-3 §2.1's MAX-CONTENT and MIN-CONTENT INLINE SIZES, in CSS pixels — the two answers, read through
   entries rather than off the struct so that the one relation between them is asserted where it is read. */
CssPx text_run_measure_max_content(const TextRunMeasure *m);
CssPx text_run_measure_min_content(const TextRunMeasure *m);

#endif
