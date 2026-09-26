/* css-sizing-3 §5.1 "Intrinsic Sizes" — A BOX'S MIN-CONTENT AND MAX-CONTENT INLINE SIZES, which are the two
 * terms CSS 2.2 §10.3.5 "Floating, non-replaced elements" calls the preferred minimum width and the preferred
 * width and sends its `auto` widths to.
 *
 * THE MODERN DEFINITION IS CIRCULAR BACK TO CSS 2 AND THAT IS THE FIRST THING TO KNOW ABOUT IT, because a
 * reader who takes css-sizing-3 for the algorithm finds none. §5.1 states both sizes in terms of the very
 * layout that consumes them — "the min-content size of a box in each axis is the size it would have IF IT WAS A
 * FLOAT given an auto preferred size in that axis … and if its containing block was ZERO-sized in that axis",
 * the max-content size the same sentence with "INFINITELY-sized" — and then says outright: "this specification
 * does not define how to determine the sizes of floats. Please refer to [CSS2]." So §5.1 is a DEFINITION and
 * CSS 2.2 §10.3.5 is the operation, in its own words: "calculate the preferred width by FORMATTING THE CONTENT
 * WITHOUT BREAKING LINES other than where explicit line breaks occur, and also calculate the preferred minimum
 * width, e.g., by TRYING ALL POSSIBLE LINE BREAKS."
 * css-sizing-3 §2.1 "Auto Box Sizes" is what pins the two vocabularies to each other, by name — "this is called
 * the 'preferred width' in CSS2.1§10.3.5" of the max-content inline size, "this is called the 'preferred
 * minimum width' in CSS2.1§10.3.5" of the min-content inline size — and it also states the operation in the
 * form this engine implements: max-content is the size "if NONE of the soft wrap opportunities within the box
 * were taken", min-content the size "if ALL soft wrap opportunities within the box were taken". Two spellings,
 * one walk, and citing only one of them would leave the other reader unable to check the code.
 *
 * WHAT THIS COMPONENT IS AND WHAT core/layout/text_run.h IS, because they are one measurement split at the
 * place the two problems actually part. text_run.h owns the CHARACTERS — css-text-3 §4.1's white space
 * processing and §5's soft wrap opportunities over [UAX14] — and knows nothing about boxes. This owns the BOX
 * TREE: which of a box's children contribute, and how css-sizing-3 §5.2 "Intrinsic Contributions" combines
 * them. The split is not tidiness: core/layout/line_box.c needs the first without the second (a line box's
 * height is a function of where the run breaks, not of an intrinsic size) and so does CSSOM VIEW §2's scrolling
 * area, so folding the characters in here would make those two consumers depend on a sizing algorithm neither
 * of them asks about.
 *
 * THE ANSWER IS THE CONTENT BOX'S INLINE SIZE, which is the box CSS 2.2 knows and the one §10.3.5's formula is
 * arithmetic in: every term of `min(max(preferred minimum width, available width), preferred width)` is a
 * content width, and the result is the used `width`. css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing
 * property"'s conversion to the border box is applied to the RESULT by the caller, exactly as core/layout/
 * used_value.c applies it to §10.3.3's constraint equation — so this component never asks about `box-sizing`
 * and a caller that forgot to is wrong in one place rather than in two.
 *
 * WHICH OF CSS 2.2 §9.4's TWO FORMATTING CONTEXTS THIS BOX ESTABLISHES IS ASKED ONCE, OVER THE WHOLE CHILD
 * LIST, BEFORE EITHER ALGORITHM RUNS. §9.2.1 "Block-level elements and block boxes" states the alternative —
 * "A block container box either contains only block-level boxes or establishes an inline formatting context
 * and thus contains only inline-level boxes" — and §9.4.2 states the condition, "a block container box that
 * contains no block-level boxes". The two algorithms share no step: §9.4.2's flows the children ALONG a line
 * and sums, §9.4.1's stacks them DOWN a column so css-sizing-3 §5.2's contribution is the MAXIMUM over them.
 * A walk that discovered the difference part-way through a measurement had already begun summing a run before
 * it met the child that made the sum the wrong operation, which is why the question is a dispatch and not a
 * case. It is core/layout/block_flow.h's exported predicate rather than a second copy of §9.4.2's condition,
 * because that component's own stack chooses between the same two sections over the same list.
 * BOTH ARMS ARE HERE. §9.4.2's is the whole of the run measurement below. §9.4.1's takes the MAXIMUM over the
 * box list §9.2.1.1 "Anonymous block boxes" forces the container to have — its block-level children plus one
 * anonymous block box per maximal run of inline-level children — with each operand css-sizing-3 §2.2
 * "Intrinsic Size Contributions"' OUTER size, which is where that phrase is defined rather than at §5.2. The
 * enumeration is core/layout/block_flow.h's, both halves of it: one classification and one run delimitation,
 * shared with the walk that PLACES the same boxes so that two answers to "is this child block-level" cannot
 * exist. `block_flow_anonymous_boxes` is deliberately NOT what it asks — that entry PLACES, and a placement
 * needs the container's used width, which for the only two boxes that ask for an intrinsic size is a
 * shrink-to-fit over this very entry.
 * IT APPLIES A CHILD'S OWN DECLARED INLINE SIZE, AND THE SENTENCE THAT STOOD HERE SAID IT DID NOT. That
 * sentence is rewritten rather than deleted because the reason it gave is still the reason the term exists:
 * §5.2's contribution is the size of a hypothetical float CONTAINING the child, so a child with `width: 500px`
 * contributes 500px and not what its text measures, and reporting the measured number would be a WRONG width
 * for a real document rather than a narrower one. What it named as still missing — CSS 2.1 §10.4 "Minimum and
 * maximum widths: 'min-width' and 'max-width'"'s clamp and css-sizing-3 §3.3 "Box Edges for Sizing: the
 * box-sizing property"'s conversion — is the whole of what `is_declared_inline_sizes` now is. The one thing
 * that has NOT changed is which values leave the measurement standing: §5.2.1 substitutes a cyclic PERCENTAGE
 * away, and the §5.1 pair this walk computes is what a substituted `auto` / `none` means.
 * §5.1 AND §5.2 ARE TWO QUESTIONS AND THIS COMPONENT ANSWERS BOTH, WHICH IS WHY THE TERM IS NOT INSIDE
 * `intrinsic_inline_sizes`: §5.1 defines a box's own intrinsic sizes "given an auto preferred size in that
 * axis and no minimum or maximum size in that axis", so the three properties are REMOVED from it by
 * definition, and §5.2's contribution is where they are applied. A caller asking this component for a BOX's
 * intrinsic size gets §5.1's answer; the child walk inside it asks for §5.2's.
 *
 * NOTHING IS STORED, for core/layout/used_value.h's reason: a layout is per-flow state, so a cached intrinsic
 * size is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_SIZE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/block_flow.h"

/* CSS 2.2 §9.4.2 "Inline formatting contexts"' OUTER SIZE at ONE SIDE of an INLINE BOX, as USED VALUES, in CSS
   pixels — the trailing side for `trailing` true. §9.4.2 is what puts it on the line ("horizontal margins,
   borders, and padding are respected between these boxes") and css-text-3 §5.5 "Line Breaking Details" is what
   says WHERE: "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
   flow", so the two edges sit at the box's boundaries and not at every break inside it. core/layout/line_box.c
   hands it to core/layout/text_run.h as an EDGE item at a POSITION, which is why it is never added to a total.
   IT IS THE USED-VALUE HALF OF ONE FACT WHOSE OTHER HALF IS THIS COMPONENT'S OWN, which is why a line box's
   operand is declared in the intrinsic-size header instead of in core/layout/used_value.h. The FACT is which
   three properties one side of an inline box is — a margin, a padding and a border width — and css-sizing-3
   §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes" asks TWO QUESTIONS of it that differ on exactly
   one input. A percentage on any of the six resolves against the containing block's width; while an INTRINSIC
   contribution is being measured that width is the number the measurement produces, and §5.2.1 states the
   resolution for that case — "For the min size properties, as well as for margins and paddings (and gutters),
   a cyclic percentage is resolved against zero for determining intrinsic size contributions." Its very next
   paragraph states the other, "when calculating the used sizes and positions of the containing block's
   contents": "Otherwise, the percentage is resolved against the containing block's size." That second one is a
   LINE's case, because a line box is filled at a width its containing block has already determined, so the
   percentage is not cyclic there at all and resolves normally.
   SO THERE ARE TWO ENTRIES OVER ONE PROPERTY TRIPLE, never two triples. One predicate answering both questions
   answered BOTH with the stricter one's refusal, and substituting §5.2.1's zero into that shared entry — which
   is what its own crash message instructed — would have silently mis-laid-out every percentage-margined inline
   box on a real line. THE INTRINSIC ANSWER IS NOT DECLARED HERE AND THAT IS THE STRUCTURAL HALF OF THE SPLIT:
   it is `static` in intrinsic_size.c because it has no caller outside that walk, and being unreachable is what
   stops it being asked at a used-value site, where its zero would be a wrong number no assert could see. */
CssPx used_inline_box_edge_px(lxb_dom_element_t *el, bool trailing);

/* WHICH PHYSICAL AXIS A SIZING PROPERTY OR A BOX EDGE IS READ ON. It is PHYSICAL and not flow-relative, and
   that naming is a statement about this component rather than a convenience: css-writing-modes-4 §7.2
   "Dimensional Mapping" pins the six sizing properties to physical extents in its own words — "The height
   properties (height, min-height, and max-height) refer to the physical height, and the width properties
   (width, min-width, and max-width) refer to the physical width" — and every edge property this file reads is
   physical too (`margin-left`, `padding-top`, `border-right-width`). So a CALLER holding a flow-relative
   question composes css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings" first (core/css/css_logical.h's
   `css_logical_axis_is_vertical`, which core/layout/flex_item.h's `flex_container_axis_is_vertical` composes
   with css-flexbox-1 §5.1 "Flex Flow Direction: the flex-direction property") and hands the ANSWER here.
   Naming these two values `inline` and `block` would put that mapping inside this file, where it is not made.
   IT IS A REQUIRED PARAMETER ON `intrinsic_declared_sizing_px` AND NOT A DEFAULT, WHICH IS THE WHOLE REASON IT
   EXISTS. That entry used to be hardcoded to the HORIZONTAL axis, so
   `intrinsic_declared_sizing_px(el, "height", "auto", &v)` COMPILED and subtracted this box's LEFT and RIGHT
   padding and border widths from a declared HEIGHT — a wrong number for a real document with no assert that
   could see it, and the one shape css-flexbox-1 §9.9.1 "Flex Container Intrinsic Main Sizes" in the block axis
   was always going to reach for. An enum in the position a property NAME used to occupy makes that call fail
   to COMPILE, which is the state being made impossible rather than reported.
   §2.2's OUTER SIZE TAKES NO AXIS PARAMETER AND HAS TWO ENTRIES INSTEAD, and that asymmetry is deliberate: the
   pair entry applies §2.2's FLOOR, which is a rule about two numbers that can invert, and the block axis has
   one intrinsic size for the floor to have nothing to compare. Each of those entries states its axis in its own
   name; only the property-name entry, whose result is a single extent either way, is honestly one function.
   THE VERTICAL ARM IS NOT A SECOND MEASUREMENT AND THIS TYPE DOES NOT CLAIM ONE: what it parameterises is which
   pair of paddings and border widths §3.3's conversion subtracts, and `intrinsic_inline_sizes` below is still
   the HORIZONTAL walk alone (core/layout/intrinsic_block_size.h is the other one). A caller that reads a
   vertical declaration and then a horizontal measurement has composed two axes into one number, which is why
   the two measurements live in two headers rather than behind one parameter. */
typedef enum {
    INTRINSIC_AXIS_HORIZONTAL = 0,
    INTRINSIC_AXIS_VERTICAL   = 1
} IntrinsicAxis;

/* css-sizing-3 §5.1's PAIR. They are returned together and never separately because §2.1 defines them over the
   same content with only the soft wrap opportunities differing, so one walk produces both — and because the
   one relation between them (`min_content <= max_content`) is a statement about the pair that a caller holding
   one of them could not check. */
typedef struct {
    CssPx min_content;   /* §2.1's min-content inline size — CSS 2.2 §10.3.5's "preferred minimum width" */
    CssPx max_content;   /* §2.1's max-content inline size — CSS 2.2 §10.3.5's "preferred width" */
} IntrinsicInlineSizes;

/* THE INTRINSIC INLINE SIZES OF `el`'s BOX, as CONTENT-box widths in CSS pixels.
   THE CALLER HAS NOT ESTABLISHED ANYTHING and does not need to: every box type this walk cannot size crashes
   here naming its own section, which is what lets a consumer ask the question without first re-deriving the
   classification core/layout/block_flow.c owns. */
IntrinsicInlineSizes intrinsic_inline_sizes(lxb_dom_element_t *el);

/* ONE OF `el`'s THREE SIZING PROPERTIES ON `axis` as a CONTENT-box extent in CSS pixels, for an INTRINSIC
   pass — true when the property states a size the caller must apply, false when it states none. `name` is
   `width`, `min-width` or `max-width` on the horizontal axis and `height`, `min-height` or `max-height` on the
   vertical one, and `initial` is that property's initial value (`auto`, `auto`, `none`), which is the only
   keyword the assertion inside admits. `axis` IS NOT DERIVED FROM `name` and must not be: it governs which
   pair of paddings and border widths css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"
   subtracts, and deriving one from the other would be a second table of the same six property names free to
   disagree with the one the edges are read from.
   IT ANSWERS TWO SECTIONS AND COMPOSES NEITHER, and that is the point of exporting this half rather than the
   pair: css-sizing-3 §5.2 "Intrinsic Contributions" makes a declared size REPLACE the box's measured one,
   while css-flexbox-1 §9.9.3 "Flex Item Intrinsic Size Contributions" takes "the larger of its outer
   min-content size and outer preferred size", a MAXIMUM. One entry, two compositions, and a caller that took
   the wrong one would report one section's answer under the other's name.
   THE FALSE ARM IS A REAL ANSWER AND NOT A REFUSAL: css-sizing-3 §3.2 gives each initial keyword no size, and
   §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes" substitutes a CYCLIC percentage away — which a
   percentage reaching an intrinsic pass always is, because it resolves against the very size that pass is
   producing. A caller that wants §3.2's used 0 for a `min-width: auto` supplies it itself; this entry does not,
   because css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items" is what overrides that zero for a flex
   item and only the caller knows whether its box is one.
   css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"' conversion is applied here, over the
   INTRINSIC surround, so the result is a content-box width whichever `box-sizing` the box computed. */
bool intrinsic_declared_sizing_px(lxb_dom_element_t *el, IntrinsicAxis axis, const char *name,
                                 const char *initial, CssPx *out);

/* CSS 2.2 §9.4.2 "Inline formatting contexts"' CONTEXT OVER ONE RUN of `el`'s CONTENT — core/layout/
   block_flow.h's `BlockFlowRun`, the range between two of §9.2.1.1's block-level boxes — as CONTENT-box inline
   sizes in CSS pixels. The run's boxes are styled by `el`, which is what an ANONYMOUS box around them inherits
   from.
   IT IS EXPORTED BECAUSE TWO SECTIONS GENERATE AN ANONYMOUS BOX AROUND A RUN OF ONE BOX'S CONTENT AND BOTH
   NEED THE SAME MEASUREMENT. CSS 2.2 §9.2.1.1 "Anonymous block boxes" wraps each maximal run of inline-level
   content and is this file's own §9.4.1 arm; css-flexbox-1 §4 "Flex Items" wraps a CHILD TEXT SEQUENCE in an
   anonymous block container flex item. The two RUNS are delimited by different sentences and the two
   delimiters are therefore two functions, but what is inside either of them is one inline formatting context
   and one walk — a second copy of it would be one document with two ideas of how wide its text is.
   THERE WAS BRIEFLY A SECOND ENTRY HERE, FOR A SIBLING RANGE, AND THE ARGUMENT FOR IT IS RETIRED. It said the
   two starts were not interchangeable because §9.2.1.1's run can begin inside an inline box and §4's cannot.
   The second half is true — §4 blockifies every flex item, so a flex container has no inline box among its
   children — and the CONCLUSION was wrong: a `BlockFlowRun` is read as `after`'s next sibling inside
   `after`'s parent, so `{ first->prev, end }` IS the sibling range `[first, end)` for any `first` that is a
   child, and §4's caller states its run that way. One entry, one walk, and a caller that hands over two bare
   node pointers no longer compiles.
   THE RUN'S OWN EDGES ARE NOT ADDED, and for both callers that is a derivation rather than an omission: an
   anonymous box takes its non-inherited properties' initial values (§9.2.1.1: "the margins will be 0"), and
   css-flexbox-1 §4 says the same of its own ("the anonymous item's box is unstyleable"), so css-sizing-3 §2.2
   "Intrinsic Size Contributions"' outer size of such a box is its inner size unchanged. */
IntrinsicInlineSizes intrinsic_inline_run_sizes(lxb_dom_element_t *el, BlockFlowRun run);

/* THE OUTER SIZE OF `el`'s BOX over INNER sizes the CALLER computed. css-sizing-3 §2 "Terminology" is where
   the term is defined — an outer size is "the margin-box size of a box", against an inner size, "the
   content-box size of a box" — and §2.2 "Intrinsic Size Contributions" is what makes it the operand of every
   contribution, with its own "for this purpose auto margins are treated as zero". css-sizing-3 §5.2.1
   "Intrinsic Contributions of Percentage-Sized Boxes"' cyclic percentage is resolved against zero on each of
   the six edge properties.
   THE INNER SIZES ARE AN ARGUMENT AND NOT `intrinsic_inline_sizes(el)`, which is the whole reason this entry
   exists rather than a one-argument one. css-flexbox-1 §9.9.3 "Flex Item Intrinsic Size Contributions" caps,
   floors and clamps a flex item's MAIN size before the outer conversion is applied, so the number that gets
   the edges added is not the one this component measured — and the two readings are the same number only
   because every operand of §9.9.3 is one item's size in one axis and every one of them takes the SAME edge
   sum, so a maximum, a cap, a floor and a clamp all commute with adding it.
   §2.2's FLOOR IS APPLIED HERE and is why this returns the PAIR rather than one number: "if the ideal
   max-content contribution would be smaller than the min-content contribution (e.g. due to the use of
   negative margins), the effective max-content contribution is floored by the min-content contribution." IT
   IS ALSO WHY NEITHER INNER SIZE IS ASSERTED — an inverted pair is the case that sentence is written for, so
   refusing one would crash on a page CSS 2.1 §8.3 "Margin properties" permits.
   `el` IS NULL FOR AN ANONYMOUS BOX, whose edge sum is ZERO — and that is a DERIVATION rather than a
   convenience, stated identically by the two sections that generate one: CSS 2.2 §9.2.1.1 "Anonymous block
   boxes" ("the properties of anonymous boxes are inherited from the enclosing non-anonymous box …
   Non-inherited properties have their initial value … the margins will be 0") and css-flexbox-1 §4 "Flex
   Items" ("the anonymous item's box is unstyleable, since there is no element to assign style rules to"). So
   its margin box, its border box and its content box are one rectangle. Passing NULL rather than reaching for
   an element that does not exist is what stops a caller substituting the CONTAINER's edges, which are a
   different box's and would be added twice.
   THE EDGE ITSELF IS NOT EXPORTED AND MUST NOT BE. §5.2.1 gives one property triple TWO answers — a cyclic
   percentage resolved against zero for a contribution, and against the containing block's size "when
   calculating the used sizes and positions of the containing block's contents" — and the zero one is a WRONG
   number at every used-value site with no assert that could see it. Being reachable only through a function
   that has already committed to being a contribution is what keeps the two apart. */
IntrinsicInlineSizes intrinsic_outer_contribution(lxb_dom_element_t *el, IntrinsicInlineSizes inner);

/* css-sizing-3 §2.2's OUTER SIZE of `el`'s BOX IN THE VERTICAL AXIS over an INNER size the CALLER computed —
   the top and bottom margin, border and padding, each at §5.2.1's zero basis exactly as the pair above takes
   the left and right ones.
   IT IS ONE NUMBER AND NOT A PAIR BECAUSE css-sizing-3 §3.2 GIVES A BOX'S BLOCK SIZE ONE INTRINSIC SIZE, in
   its own words at both intrinsic keywords ("for a box's block size, unless otherwise specified, this is
   equivalent to its automatic size"). core/layout/intrinsic_block_size.h is the measurement; this is §2.2's
   step over it.
   IT IS A SECOND ENTRY RATHER THAN AN AXIS PARAMETER ON THE PAIR ABOVE, AND THE DIFFERENCE IS ONE OPERATION:
   §2.2's floor of the max-content contribution by the min-content one is a rule about a pair that can invert,
   and there is no second member here for it to invert against. An axis parameter would have made that floor a
   comparison of a number with itself — a vacuous step wearing the syntax of a real one — and would have
   returned a type named for the other axis. What the two DO share is the edge arithmetic, which is one static
   derivation over `IS_EDGE`'s two rows.
   `el` IS NULL FOR AN ANONYMOUS BOX, whose edge sum is zero, for the derivation stated in full above. */
CssPx intrinsic_outer_block_contribution(lxb_dom_element_t *el, CssPx inner);

#endif
