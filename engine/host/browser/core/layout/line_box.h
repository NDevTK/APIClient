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
   `*last_baseline` RECEIVES CSS 2.2 §10.8.1 "Leading and half-leading"'s OTHER DISTANCE DOWN THE SAME LINE
   BOXES — the offset from the same top content edge to the BASELINE of that same last line box, which is a
   position INSIDE the box the returned height ends at. It is an OUT-PARAMETER AND NOT A SECOND ENTRY for
   the reason the walk behind it states in full: §10.6.3's bottom edge and §10.8.1's baseline are one running
   position read at two points of one loop over one fill, and a second pass could put the baseline on a line
   the height had not counted. It is a distance ONLY WHEN `*any_line_box` IS TRUE; its zero otherwise is not a
   coordinate, exactly as the returned height's is not, and both are written on every path.
   `*first_baseline` RECEIVES THE SAME OFFSET TO THE FIRST EXISTING LINE BOX's BASELINE, and it is here rather
   than in a second entry for exactly the reason the last one is: it is the same running position read at a
   third point of the SAME loop, so it costs nothing and a second reduction over one fill is what this file
   refuses everywhere else. TWO SECTIONS ASK FOR IT BY NAME AND NEITHER IS §10.8.1's. css-inline-3 §4.2.1
   "Alignment Baseline Source: the baseline-source longhand"'s `first` arm is one — §4.2.1's own `auto` is
   "last-baseline alignment for inline-block, first-baseline alignment for everything else", so the alternative
   it names is this number and not §10.8.1's. CSS 2.1 §17.5.3 "Table height algorithms" is the other and is the
   one that made it load-bearing: "The baseline of a cell is the baseline of the first in-flow line box in the
   cell", which is what that section's row baseline — "the maximum distance between the top of the cell box and
   the baseline over all cells that have 'vertical-align: baseline'" — is the maximum of. For a box holding ONE
   line box the two baselines are the same number, and for a box holding several they are not; reading the last
   where §17.5.3 asks for the first makes a row too SHORT whenever a taller cell's first line sits above its
   last, which is a rectangle no reader can tell from a measured one.
   NONE OF THE FOUR IS OPTIONAL, which is why no out-parameter may be NULL: they are the four answers
   ONE reduction has, so a caller that wanted only some of them would still be paying for all four, and a
   nullable one is the shape that invites a second walk to be added for the answer it declined.
   THE CALLER HAS ALREADY ESTABLISHED §9.4.2's OWN CONDITION over the run it passes — that this box contains no
   block-level boxes — because deciding it requires classifying every child, which core/layout/block_flow.c
   does once, both to choose between the two formatting contexts and to delimit §9.2.1.1's runs. A block-level
   box reaching this walk is those two classifications having come apart, and it crashes here saying so. */
CssPx line_box_content_height(lxb_dom_element_t *style, lxb_dom_node_t *first, lxb_dom_node_t *end,
                              bool *any_line_box, CssPx *first_baseline, CssPx *last_baseline);

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
 * AN ATOMIC INLINE-LEVEL BOX HANGS FROM THAT SAME BASELINE BY A DIFFERENT SENTENCE, and the two derivations
 * are separated by whether the box HAS a baseline rather than by which kind of box it is. CSS 2.2 §10.8's
 * `vertical-align` definition states both halves: "for inline non-replaced elements, the box used for
 * alignment is the box whose height is the 'line-height' … FOR ALL OTHER ELEMENTS, THE BOX USED FOR ALIGNMENT
 * IS THE MARGIN BOX", and `baseline` itself — "align the baseline of the box with the baseline of the parent
 * box. IF THE BOX DOES NOT HAVE A BASELINE, ALIGN THE BOTTOM MARGIN EDGE with the parent's baseline." §10.8
 * gives a baseline to an `inline-table` ("The baseline of an 'inline-table' is the baseline of the first row
 * of the table") and to an `inline-block` ("The baseline of an 'inline-block' is the baseline of its last line
 * box in the normal flow, unless it has either no in-flow line boxes or if its 'overflow' property has a
 * computed value other than 'visible', in which case the baseline is the bottom margin edge") and to NOTHING
 * else. THAT SECOND SENTENCE IS ANSWERED FOR EVERY BLOCK CONTAINER §9.4.1's STACK CAN PLACE — the standard
 * writes it under §10.8.1 "Leading and half-leading", which is where this component cites it, and its main arm
 * reaches core/layout/block_flow.h's stack rather than this file's own reduction, so an `inline-block` holding
 * an in-flow BLOCK-LEVEL box is measured and not refused. What is still narrower than the sentence is a
 * block-level child whose own baseline another module owns — a `flex` or `grid` container on that stack —
 * which crashes there naming css-flexbox-1 §8.5 "Flex Container Baselines" and css-grid-1 §10.6 "Grid
 * Container Baselines", so it shows as an abort on an `inline-block` whose stack holds one and never as a
 * wrong number. Its three arms do not produce the same geometry:
 * an `inline-block` sent to its bottom margin edge by either disjunct hangs its whole margin box above the
 * line, while one measured by the MAIN arm has the line's baseline running THROUGH it, at the baseline of the
 * last line box of the formatting context inside it. So a REPLACED element's bottom
 * margin edge sits ON the line's baseline — which is why an image on a line of text leaves the font's
 * descender visible below it. Its border area is then that baseline less its own `margin-bottom`, extending
 * one used BORDER EDGE EXTENT upward (§10.6.2 "Inline replaced elements, block-level replaced elements in
 * normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements"). It is the
 * same pair `lb_atomic_extent` puts above the baseline for §10.8's step 1, read back through §8.1's nesting —
 * and it is that box's pair BECAUSE it has no baseline, never because every atomic inline's `below` is zero.
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
 * WHICH THE CALLER OWNS AND MUST FREE; the count is never zero, because a box that generates a box is on at
 * least one line ("line boxes are created as needed to hold inline-level content", and this box's items ARE
 * content the fill partitions).
 *
 * `el` MUST BE A BOX ON A LINE — a computed `display` of `inline` or `inline-block`, in flow, in a
 * `horizontal-tb` writing mode.
 * That is TWO SHAPES and the count is where they differ: a NON-REPLACED inline box is delimited by its two
 * EDGE items and CSS 2.2 §9.4.2 splits it across as many line boxes as it spans, while an ATOMIC inline-level
 * box is delimited by the ONE run item `lb_child` collects for it and is always exactly ONE fragment — CSS 2.2
 * §9.2.2 "Inline-level elements and
 * inline boxes" makes it a box that "participate[s] in [its] inline formatting context as
 * a SINGLE OPAQUE BOX", so it is never the box §9.4.2 "SPLIT[s] into several boxes". Both are asserted.
 * WHICH BOXES ARE ATOMIC HERE IS TWO INDEPENDENT FACTS AND NOT ONE `display`: a REPLACED element (HTML §15.4
 * "Replaced elements", whose computed `display` stays `inline`) and an `inline-block` of either kind, since
 * CSS 2.2 §10.3.10 "'Inline-block', replaced elements in normal flow" delegates the replaced half whole
 * ("Exactly as inline replaced elements.") rather than making it a different box. The block axis then reads
 * §10.8.1's SPLIT of the margin box at the box's own baseline (`lb_atomic_extent`) rather than assuming the
 * bottom margin edge, which is what admitting the `inline-block` required and is one arithmetic over both:
 * `below` is zero for every box the section gives no baseline.
 * AN `inline-table`, `inline-flex` OR `inline-grid` STILL CRASHES IN THE WALK, and for none of them is the
 * missing piece a placement — BUT THEY NO LONGER CRASH FOR ONE REASON, AND THIS PARAGRAPH USED TO SAY THEY
 * DID. An `inline-flex` or `inline-grid` still needs the USED MAIN SIZE its own module owns (css-flexbox-1
 * §9.9.1 "Flex Container Intrinsic Main Sizes", css-grid-1 §5.2 "Sizing Grid Containers") before §9.4.2 has
 * anything to put on a line, and the baseline each of them has falls out of that same module's layout.
 * AN `inline-table` HAS ITS INLINE SIZE: CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property
 * is built (core/layout/table_width.h) and core/layout/used_value.c routes a table box's width to it, so what
 * keeps this one out is the OTHER axis — CSS 2.2 §10.8.1 "Leading and half-leading" makes its baseline "the
 * baseline of the first row of the table", and a row's baseline is CSS 2.1 §17.5.3 Table height algorithms',
 * which has no component. The two absences are not one, and a reader taking them for one would build §17.5.2
 * a second time.
 *
 * IT FINDS THE FORMATTING CONTEXT ITSELF, and that is why it takes an element where the two entries above take
 * a run: the question "which inline formatting context is this box in" is answered by walking PAST every inline
 * ancestor to the nearest block container. WHAT STOOD HERE SAID THAT IS NOT §10.1's WALK BECAUSE "its own
 * exception makes an INLINE ancestor a containing block of a different shape", quoting §10.1's "the bounding box
 * around the padding boxes of the first and the last inline boxes" — and that exception is written in §10.1's
 * FOURTH case, about the nearest POSITIONED ancestor of a `position: absolute` box, not in its second. The
 * section's own worked example settles the second case in the opposite direction: for
 * `<P id="p2">... <EM id="em1"> ... <STRONG id="strong1">second</STRONG> ...</EM></P>` its table of containing
 * blocks gives BOTH `em1` and `strong1` the block established by `p2`, so §10.1's second case steps over an
 * inline ancestor exactly as this walk does, and core/layout/used_value.h now does too rather than crashing
 * there. THE TWO QUESTIONS STILL DIFFER AND THE DIFFERENCE IS NO LONGER THE INLINE STEP: §10.1's OTHER cases
 * answer something else entirely — the root's is the initial containing block (no element at all), a `fixed`
 * box's is the viewport, and an `absolute` box's is a positioned ancestor's PADDING edge, which is where that
 * inline exception actually lives — while "which context is this box on a line of" is the same block container
 * whatever this box's `position` is. So the walks agree on one case out of four and are not the same entry.
 * One walk, here, because both of §6's and §7's consumers would otherwise carry a copy.
 *
 * FINDING IT MEANS FINDING THE RUN AS WELL, because the container is only half an answer when it is MIXED. CSS
 * 2.2 §9.2.1.1 "Anonymous block boxes" — "if a block container box (such as that generated for the DIV above)
 * has a block-level box inside it (such as the P above), then we force it to have only block-level boxes inside
 * it" — puts this box's line boxes inside one of the ANONYMOUS BLOCK BOXES that forcing generates, one per
 * maximal run of inline-level children, and that box is not the container: filling the container's whole child
 * list would flow this box's items together with every other run's, which is a different partition on line
 * boxes that do not exist. So this entry asks core/layout/block_flow.h which of those boxes holds the child it
 * descended through, and fills THAT run. The runs and their positions come from block_flow.h's own §9.4.1 stack
 * rather than being delimited a second time here, because a run's boundaries and its box's position are two
 * halves of one derivation and two copies could disagree about where a margin collapsed.
 *
 * `*establishing` IS STILL THE CONTAINER AND THE FRAME IS STILL ITS CONTENT BOX, in both shapes, which is what
 * makes the paragraph above invisible to every caller. §9.2.1.1 gives the anonymous box no element and no
 * margin, border or padding, and its own origin inside the container is a number block_flow.h reports — so this
 * entry ADDS that origin to the coordinates it measures inside the box, and a mixed container's fragments come
 * out in the same frame an unmixed one's do. There is no second frame for a caller to know about, and no
 * element is reported that the element tree does not contain. */
size_t line_box_inline_fragments(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 LineBoxFragment **out);

/* WHERE A NON-REPLACED INLINE BOX'S OWN MARGIN EDGES REACH on ONE PHYSICAL AXIS — `*lo` and `*hi` receive the
 * extreme over ALL of its fragments, in the same frame `line_box_content_span` and `LineBoxFragment` report in
 * (offsets from `*establishing`'s content box origin on that axis), with `*establishing` receiving the block
 * container that frame belongs to.
 *
 * BOTH HALVES OF THE NAME ARE THE PRECONDITION AND THIS ENTRY ASSERTS THEM, which the entry above no longer
 * does for it: that one answers a REPLACED inline element now, and this one must not, because CSS 2.2 §8.3
 * "Margin properties"' exception is what its block arm rests on — "these properties apply to all elements, but
 * VERTICAL MARGINS WILL NOT HAVE ANY EFFECT ON NON-REPLACED INLINE ELEMENTS" — and a replaced element's
 * vertical margins DO have an effect. A replaced element also needs none of this: §10.3.2 and §10.6.2 give it
 * both extents and core/layout/flow_position.h gives it one origin, which is the ordinary composition.
 *
 * IT EXISTS BECAUSE AN INLINE BOX HAS NO `width` AND NO `height`, so the ONE-ORIGIN-PLUS-ONE-EXTENT shape every
 * other box's margin edge is composed from cannot describe it. CSS 2.2 §10.3.1 "Inline, non-replaced elements"
 * is one sentence long — "The 'width' property does not apply" — and §10.6.1 "Inline, non-replaced elements"
 * opens with "The 'height' property does not apply", so core/layout/used_value.h's border-edge EXTENT asserts
 * against exactly this box and a caller that composed one would abort in that file, naming CSSOM §9's
 * applicability contract, for a question that was asked here. §9.4.2 says what is there instead: "when an
 * inline box exceeds the width of a line box, it is SPLIT into several boxes and these boxes are distributed
 * across several line boxes", so the box is a SET of border areas and its margin edge is an extreme over them.
 *
 * ITS CALLER IS CSSOM VIEW §2 "Terminology"'s SCROLLING AREA — "the right margin edge of all of the element's
 * descendants' boxes" — and an inline box is one of those descendants whenever a page asks `scrollWidth` of a
 * container holding a `<span>`. CSSOM VIEW §6's own `scrollWidth` steps have no inline exclusion (its step 1
 * terminates only for "no associated box", where `clientWidth`'s terminates for "the box is inline"), so this
 * is not a corner: it is the shape of every paragraph.
 *
 * THE TWO MARGINS GO ON THE TWO FRAGMENTS THAT CARRY THE BOX'S OWN BOUNDARIES, WHICH ARE THE FIRST AND THE
 * LAST, and that is `line_box_inline_fragments`' own loop rather than a rule restated here. §9.4.2: "when an
 * inline box is split, margins, borders, and padding have NO VISUAL EFFECT where the split occurs (or at any
 * split, when there are several)" — so a middle fragment runs edge to edge with no margin on either side. That
 * entry emits one fragment per line its `[open, close]` item range intersects, in line order, and its lines
 * PARTITION the item collection: the first intersecting line is therefore the one holding `open` and the last
 * the one holding `close`, which is precisely where it took the two margins OFF to report §6's border area.
 * They go back on here and nowhere else.
 *
 * THE BLOCK AXIS TAKES NO MARGIN AT ALL, and that is CSS 2.2 §8.3 "Margin properties" in so many words: "These
 * properties apply to all elements, but VERTICAL MARGINS WILL NOT HAVE ANY EFFECT ON NON-REPLACED INLINE
 * ELEMENTS", restated in the `margin-top`/`margin-bottom` definition as "These properties have no effect on
 * non-replaced inline elements." So on that axis the margin edge IS the border area §10.6.1 nests around the
 * content area, and adding a vertical margin here would report an overflow no user agent draws.
 *
 * THE EXTREME IS OVER EVERY FRAGMENT AND NOT OVER THE TWO ENDS, and §8.3's NEGATIVE margin is what makes those
 * two different answers: "negative values for margin properties are allowed", so a `margin-left: -20px` puts
 * the FIRST fragment's margin edge 20px INSIDE its own border edge, and a middle fragment — whose split edge
 * carries no margin at all — is then the outermost thing the box has. Each fragment contributes the coordinate
 * the split sentence gives IT, and the extreme is over those; a fragment's bare border edge where a margin
 * belongs is not a coordinate any margin edge of this box occupies and is never an operand. */
void line_box_inline_margin_span(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 bool vertical, CssPx *lo, CssPx *hi);

#endif
