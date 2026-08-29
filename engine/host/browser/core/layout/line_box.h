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

#endif
