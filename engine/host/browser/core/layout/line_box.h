/* CSS 2.2 §9.4.2 "Inline formatting contexts" and §10.8 "Line height calculations: the 'line-height' and
 * 'vertical-align' properties" — THE LINE BOX, which is the other half of §10.6.3's content-based height and
 * the box core/layout/block_flow.h has been crashing for.
 *
 * WHY THIS IS A SEPARATE COMPONENT FROM block_flow.h AND NOT ANOTHER ARM OF ITS WALK. §9.4.2 opens by saying
 * which box establishes this formatting context — "an inline formatting context is established by a block
 * container box that CONTAINS NO BLOCK-LEVEL BOXES" — and §9.4.1 says the same of the other one. They are
 * ALTERNATIVES over one BOX, decided by its content and by nothing else — and CSS 2.2 §9.2.1.1 "Anonymous
 * block boxes" is what makes that sentence true of a container holding both, by putting a box there that the
 * element tree does not: "if a block container box (such as that generated for the DIV above) has a
 * block-level box inside it (such as the P above), then we force it to have only block-level boxes inside it",
 * each run of inline-level content wrapped in an anonymous block box. So this component is asked about a RUN
 * of a block container's children rather than about an element, and the ELEMENT it is handed alongside is the
 * one whose style the box has — §9.2.1.1: "the properties of anonymous boxes are inherited from the enclosing
 * non-anonymous box … non-inherited properties have their initial value." The two algorithms share no step:
 * §9.4.1 stacks border boxes down a column and reduces §8.3.1's adjoining margin runs, while §9.4.2 flows
 * boxes ALONG a line and then §10.8 takes two maxima across it. A single walk carrying both would be two
 * algorithms behind one `if`, and the `if` would be re-asked per child instead of once per box.
 *
 * THE TWO HALVES OF THE ANSWER ARE MEASURED BY DIFFERENT THINGS, and keeping them apart is what makes this
 * component readable. §10.8's three steps are arithmetic over `line-height`, `A` and `D`, all of which
 * core/css/css_computed_value.h answers per element — so the HEIGHT of one line box needs no font outlines at
 * all. HOW MANY line boxes there are is the other half and it needs everything: §9.4.2 distributes
 * inline-level boxes across "two or more vertically-stacked line boxes" when they "cannot fit horizontally
 * within a single line box", which is the run's own advances (core/css/font_metrics.h, per Unicode scalar
 * value off the first available face's 'cmap' and 'hmtx') and its soft wrap opportunities ([UAX14], through
 * core/layout/line_break.h) measured against the AVAILABLE WIDTH of each line. That distribution is
 * `text_run_measure_fill`, and it lives in core/layout/text_run.h beside css-sizing-3 §2.1's two intrinsic
 * partitions because all three are walks over ONE [UAX14] pass and a second pass is the one way they could
 * disagree about where this run may break.
 *
 * SO THIS COMPONENT COLLECTS, FILLS, AND THEN MEASURES EACH LINE, and the order is forced rather than chosen:
 * §10.8's step 3 is "the distance between the uppermost box top and the lowermost box bottom" of ONE line, so
 * which boxes those maxima are taken over is a question only the fill can answer. A walk over the ELEMENT TREE
 * cannot: an inline box whose text spans three lines is on all three and one whose text fits is on one, and
 * the tree records neither. That is the same fact CSSOM VIEW §6's `getClientRects()` step 3 reports as a
 * fragment count.
 *
 * THE AVAILABLE WIDTH IS ASKED FOR ONLY WHERE IT IS AN OPERAND, which is §9.4.2's own overflow sentence and
 * not an optimisation: "if an inline box cannot be split (e.g., if the inline box contains a single
 * character …), then the inline box OVERFLOWS the line box." A run with no break position inside it is
 * therefore ONE line box at every width — css-text-3 §5.5 "Line Breaking Details" is what makes an inline
 * formatting context of empty inline boxes such a run, since "out-of-flow boxes and inline box boundaries do
 * not introduce a forced line break or soft wrap opportunity in the flow" — and deriving a width for it would
 * run CSS 2.1 §10.3 over this box to discard the result. `text_run_measure_splits` is that question and the
 * fill ASSERTS the theorem rather than letting a caller trust it.
 *
 * §9.4.2's ZERO-HEIGHT LINE BOX IS A SECOND ANSWER AND NOT A ROUNDING OF THE FIRST, which is why the entry
 * reports it separately. "Line boxes that contain no text, no preserved white space, no inline elements with
 * non-zero margins, padding, or borders, and no other in-flow content … and do not end with a preserved
 * newline MUST BE TREATED AS ZERO-HEIGHT LINE BOXES for the purposes of determining the positions of any
 * elements inside of them, and MUST BE TREATED AS NOT EXISTING FOR ANY OTHER PURPOSE." A caller that saw only
 * a height of zero could not tell that case from a line box that measured zero, and §8.3.1 asks for exactly
 * that distinction twice: its adjoining test excepts them by name ("note that certain zero-height line boxes
 * (see 9.4.2) are ignored for this purpose") and its collapse-through note conditions on "it DOES NOT CONTAIN
 * A LINE BOX". So `<div><span></span></div>` is a box with an in-flow child, no line box, and margins that
 * collapse through it — three facts a single number cannot carry.
 *
 * WHAT IS ON THE LINE IS WHAT THE ELEMENT TREE HOLDS, and that is this engine's model rather than an omission
 * this component makes: it builds NO PSEUDO-ELEMENT BOXES anywhere, which core/css/css_style_declaration.c
 * enforces at the one surface a page can see it from — `getComputedStyle(el, "::before")` THROWS a
 * NotSupportedError rather than answering the originating element's values. So a `content` declaration puts no
 * box on this line for the same reason it puts none in §9.4.1's stack, and the day css-content-3's generated
 * content exists it becomes a box every walk over children sees at once. Reading a `content` declaration HERE
 * would be one component disagreeing with that model rather than fixing it.
 *
 * NOTHING IS STORED, for core/layout/used_value.h's reason: a layout is per-flow state, so a cached line box
 * is shared state solver/dom_cow.h does not swap and a stale one is another flow's geometry. Every answer is
 * derived per read from the running flow's own tree. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BOX_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LINE_BOX_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* CSS 2.2 §10.6.3's FIRST BULLET for the block container box that establishes ONE inline formatting context —
   "the distance from its top content edge to the bottom edge of the last line box" — in CSS pixels.
   THE FORMATTING CONTEXT IS THE HALF-OPEN RUN `[first, end)` OF `style`'s CHILDREN, AND `style` IS WHOSE
   PROPERTIES THE BOX HAS. Those are two arguments and not one because §9.2.1.1's anonymous block box has no
   element: a mixed container generates one box per run of inline-level children, and each of them "inherit[s]
   from the enclosing non-anonymous box" — the DIV, not itself — while the content it holds is only its own
   run. For a container with no block-level box at all the run IS the whole child list, so `first` is its first
   child and `end` is NULL; `end` is exclusive and NULL means "to the end of the list".
   `*any_line_box` RECEIVES §9.4.2's OTHER ANSWER: false when every line box in this formatting context is one
   the section says "must be treated as NOT EXISTING for any other purpose", which is a different fact from a
   height of zero and is what §8.3.1's collapse-through note asks for. It is written on every path, so a caller
   reading it after a crash-free return is reading a measurement rather than whatever it initialised.
   THE CALLER HAS ALREADY ESTABLISHED §9.4.2's OWN CONDITION over the run it passes — that this box contains no
   block-level boxes — because deciding it requires classifying every child, which core/layout/block_flow.c
   does once, both to choose between the two formatting contexts and to delimit §9.2.1.1's runs. A block-level
   box reaching this walk is those two classifications having come apart, and it crashes here saying so. */
CssPx line_box_content_height(lxb_dom_element_t *style, lxb_dom_node_t *first, lxb_dom_node_t *end,
                              bool *any_line_box);

/* WHERE THE BOXES ON THIS FORMATTING CONTEXT'S LINE BOXES REACH on ONE PHYSICAL AXIS — `*lo` and `*hi` receive
 * the lowest and highest coordinates any of them occupies, as OFFSETS FROM THE ESTABLISHING BOX'S CONTENT BOX
 * ORIGIN on that axis (its left edge for the horizontal one, its top edge for the vertical one). The three
 * arguments before them are `line_box_content_height`'s three and mean exactly what they mean there.
 *
 * IT IS A SECOND REDUCTION OVER ONE FILL, NOT A SECOND LAYOUT. §9.4.2's distribution is run once by the same
 * static both entries go through, for the reason core/layout/text_run.h gives about its own three partitions:
 * two collections of one formatting context are two chances to disagree about where this run may break, and a
 * height and a span that disagreed about which line a box is on would be two documents.
 *
 * WHY IT EXISTS BESIDE THE HEIGHT, WHICH IS NOT THE SAME NUMBER TWICE. CSSOM VIEW §2 "Terminology"'s SCROLLING
 * AREA takes an extreme over "the … margin edge of all of the element's descendants' boxes", and the box
 * holding a text run is the ANONYMOUS INLINE BOX CSS 2.2 §9.2.2.1 "Anonymous inline boxes" generates — the one
 * box in the tree with no element to reach it through, so core/layout/flow_position.h cannot be asked where it
 * is. It is also the box a `scrollWidth` is usually ASKING about: a run wider than its container overflows on
 * the inline axis while contributing nothing to §10.6.3's height, and a container with a DECLARED height never
 * reaches the height walk at all (core/layout/block_flow.c's `bf_height_needs_content`) while its text still
 * overflows it. Neither fact is visible in a height.
 *
 * THE INLINE AXIS IS EXACT WITHOUT A PER-ITEM POSITION, AND THAT IS A THEOREM WITH TWO HALVES rather than an
 * approximation this entry settles for. §9.4.2 gives the LINE BOX the containing block's width — "in general,
 * the left edge of a line box touches the left edge of its containing block and the right edge touches the
 * right edge of its containing block" — and css-text-4 §7.1 "Text Alignment: the text-align shorthand" says
 * where the content sits inside it: "if (after justification, if any) the inline contents of a line box are
 * too long to fit within it, then the contents are START-ALIGNED: any content that doesn't fit overflows the
 * line box's end edge."
 *   A LINE THAT OVERFLOWS is therefore start-aligned by §7.1, so its boxes begin at the content box's own start
 *   edge and reach exactly `TextRunLine.size` from it. Nothing is read for this and nothing could be: §7.1
 *   leaves the alignment no say in it.
 *   A LINE THAT FITS is distributed by `text-align` (§9.4.2: "when the total width of the inline-level boxes on
 *   a line is LESS than the width of the line box containing them, their horizontal distribution within the
 *   line box is determined by the 'text-align' property"), and WHEREVER that puts it, it is inside the line
 *   box, which is inside the content box, which is inside the padding box. So the edge reported for it — the
 *   content box's own — is a coordinate the CALLER'S extreme absorbs, because CSSOM VIEW §2's other operand is
 *   that same element's padding edge. The number is therefore not a per-fragment position and MUST NOT BE READ
 *   AS ONE: this entry answers where the boxes reach OUTSIDE the content box, and inside it answers the content
 *   box. CSSOM VIEW §6's `getClientRects()` wants the POSITION and is still not this entry's caller — it is
 *   `line_box_inline_fragments`', below, which computes the per-item offset and the alignment this one is
 *   constructed to avoid needing. The two answers stay separate because the reason this one needs neither is a
 *   THEOREM about an extreme and not a gap: an entry that took the fragment positions and re-derived an extreme
 *   from them would read a used content width for every formatting context, including the ones §10.3 still
 *   crashes for, in order to reach a number this derivation already has.
 *
 * THE BLOCK AXIS IS A MAXIMUM OVER THE BOXES AND DELIBERATELY NOT THE STACK'S OWN BOTTOM, which is the one
 * place this entry and `line_box_content_height` are answering different questions about the same lines. CSS
 * 2.2 §10.8 makes each line box as tall as "the distance between the uppermost box top and the lowermost box
 * bottom" INCLUDING the STRUT — "exactly as if each line box starts with a zero-width inline box with the
 * element's font and line height properties. We call that imaginary box a 'strut'" — and an imaginary box is
 * not one of "the element's descendants' boxes". `<div style="line-height:100px"><span
 * style="line-height:10px">x</span></div>` has a line box far taller than anything in it, and reporting the
 * stack's bottom would be an overflow no box makes.
 *
 * IT REPORTS THE CONTENT BOX'S OWN BEGINNING CORNER FOR A CONTEXT WITH NOTHING ON ITS LINES, rather than
 * leaving either output unwritten. That corner is inside the padding box on both axes (CSS 2 §8.1 nests them
 * and a padding is non-negative), so it is invisible to CSSOM VIEW §2's extreme — the same property that makes
 * the fitting-line answer above harmless, stated once for the degenerate case too. */
void line_box_content_span(lxb_dom_element_t *style, lxb_dom_node_t *first, lxb_dom_node_t *end,
                           bool vertical, CssPx *lo, CssPx *hi);

/* ONE BOX FRAGMENT of an inline box — CSSOM VIEW §6 "Extensions to the Element Interface"'s getClientRects()
 * step 3's "one for each box fragment", which for an inline box is one per LINE BOX it spans. The four numbers
 * are the fragment's BORDER AREA as OFFSETS FROM THE CONTENT BOX ORIGIN of the block container that establishes
 * the formatting context — the same frame `line_box_content_span` reports in, and the frame
 * core/layout/flow_position.h turns into an initial-containing-block coordinate by adding that box's own origin
 * plus CSS 2 §8.1's leading border and padding.
 *
 * WHY THE BORDER AREA AND NOT THE CONTENT AREA. §6's step 3 says "describing its BORDER AREA", and for an
 * inline box that area is exactly what core/layout/text_run.h collects as the box's two EDGE items:
 * css-sizing-3 §2.2's outer size at each boundary, which CSS 2.2 §9.4.2 puts on the line ("horizontal margins,
 * borders, and padding are respected between these boxes"). An edge is the MARGIN box's contribution, so the
 * inline-axis border area is that span less the box's own two horizontal margins — the one subtraction this
 * component makes, and it is stated here because it is the whole difference between what the fill holds and
 * what §6 asks for.
 *
 * THE BLOCK AXIS IS THE BOX'S OWN CONTENT AREA AND EMPHATICALLY NOT THE LINE BOX'S, and CSS 2.2 §10.6.1
 * "Inline, non-replaced elements" is explicit about it in a sentence written to be misread the other way: "the
 * vertical padding, border and margin of an inline, non-replaced box start at the top and bottom of the CONTENT
 * AREA, and has NOTHING TO DO WITH the 'line-height'. But only the 'line-height' is used when calculating the
 * height of the LINE BOX." Two heights, and §6's step 3 wants the first. `<div style="line-height:100px"><span>
 * x</span></div>` renders a 100px line box around a span whose border area is one font tall, and reporting the
 * line box would be a rectangle no border is drawn on.
 * WHERE THE CONTENT AREA SITS IS THE BASELINE, which is the one number the line box does supply: §10.8's step 3
 * makes the line box "the distance between the uppermost box top and the lowermost box bottom", so its baseline
 * is the line's own maximum `A'` below its top and every box on it hangs from that one line. The content area
 * is then `A` above the baseline and `D` below it, and §8.1's padding and border nest outside it.
 * §10.6.1 LEAVES THE CONTENT AREA'S HEIGHT TO THE UA and this engine's choice is stated rather than assumed:
 * "the height of the content area should be based on the font, but this specification does not specify how. A
 * UA may, e.g., use the em-box or the MAXIMUM ASCENDER AND DESCENDER of the font." The second is taken, and it
 * is taken because it is the SAME `A` and `D` §10.8.1's strut already reads off the first available font — one
 * pair of numbers for both heights, so a fragment's rectangle and the line it sits on cannot come to describe
 * different fonts.
 * A LINE §9.4.2 says "must be treated as ZERO-HEIGHT line boxes for the purposes of determining the positions
 * of any elements INSIDE of them" still carries a fragment, at the position the stack has reached: §6's step 3
 * asks for zero-extent rectangles by name ("including those with a height or width of zero"), and §9.4.2's
 * "not existing for any other purpose" is about §8.3.1's margin collapsing and §10.6.3's height, both of which
 * this entry answers nothing for.
 *
 * §7.1's ALIGNMENT IS APPLIED HERE AND THE COORDINATE IS COMPLETE. css-text-4 §7.1 "Text Alignment: the
 * text-align shorthand" is a shorthand with a `Computed value:` line of "see individual properties", so what
 * this reads is §7.3 "Default Text Alignment: the text-align-all property" and, for the last line of the block
 * and for a line a forced break ends, §7.4 "Last Line Alignment: the text-align-last property". A caller
 * receives a distance from the content box and never an alignment to apply itself: two callers applying it
 * would be two answers to where one fragment is. */
typedef struct {
    CssPx inline_start, inline_end;   /* from the content box's LEFT edge (horizontal-tb, asserted) */
    CssPx block_start, block_end;     /* from the content box's TOP edge */
} LineBoxFragment;

/* `el`'s FRAGMENTS, in content order, with `*establishing` receiving the block container whose content box
 * origin they are measured from. Answers the count and stores a newly allocated array of that many at `*out`,
 * WHICH THE CALLER OWNS AND MUST FREE; the count is never zero, because an inline box that generates a box is
 * on at least one line ("line boxes are created as needed to hold inline-level content", and this box's two
 * edge items ARE content the fill partitions).
 *
 * `el` MUST BE A NON-REPLACED INLINE BOX — a computed `display` of `inline` that is in flow. An atomic
 * inline-level box (`inline-block` and the rest of css-display §2's inline-outer list) is ONE fragment and not
 * a span of them, and it reaches this component's own walk as the capability that walk crashes for; the caller
 * establishes which it has before asking, and this asserts it.
 *
 * IT FINDS THE FORMATTING CONTEXT ITSELF, and that is why it takes an element where the two entries above take
 * a run: the question "which inline formatting context is this box in" is answered by walking PAST every inline
 * ancestor to the nearest block container, which is NOT §10.1's containing block — §10.1's second case stops at
 * the nearest block container too but its own exception makes an INLINE ancestor a containing block of a
 * different shape ("the bounding box around the padding boxes of the first and the last inline boxes"), so
 * core/layout/used_value.h answers a different question and crashes there rather than stepping over it. One
 * walk, here, because both of §6's and §7's consumers would otherwise carry a copy. */
size_t line_box_inline_fragments(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 LineBoxFragment **out);

#endif
