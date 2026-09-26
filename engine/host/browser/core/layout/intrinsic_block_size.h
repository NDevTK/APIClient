/* css-sizing-3 §3.2 "Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch, min-content,
 * max-content, and fit-content values" — A BOX'S INTRINSIC SIZE IN ITS BLOCK AXIS, which is the question
 * core/layout/intrinsic_size.h answers for the INLINE axis and cannot answer for this one: that component's
 * walk lays line boxes out along a line and sums or maximizes across them, and a block size is what
 * CSS 2.1 §10.6 stacks.
 *
 * IT IS ONE NUMBER AND NOT A PAIR, AND THAT IS THE SECTION RATHER THAN A SIMPLIFICATION. §3.2 says the same
 * sentence at BOTH intrinsic keywords: `min-content` is "Use the min-content size in the relevant axis; for a
 * box's block size, unless otherwise specified, this is equivalent to its automatic size", and `max-content` is
 * "Use the max-content size in the relevant axis; for a box's block size, unless otherwise specified, this is
 * equivalent to its automatic size". So the block axis has ONE intrinsic size, and a caller that holds it holds
 * both of css-sizing-3 §5.1 "Intrinsic Sizes"' terms — which is why this returns a `CssPx` rather than the pair
 * `IntrinsicInlineSizes` carries. A pair here would be one measurement stored twice, free to be read as two.
 *
 * THE AUTOMATIC SIZE IS §3.2's OWN `auto` ENTRY — "For width/height, specifies an automatic size (automatic
 * block size / automatic inline size). See the relevant layout module for how to calculate this" — and
 * css-writing-modes-4 §7.2 "Dimensional Mapping" names the module for the block dimension: "the calculation
 * rules in CSS2.1 Section 10.6 are used in the block dimension". For a non-replaced block-level box that is
 * CSS 2.1 §10.6.3 "Block-level non-replaced elements in normal flow when 'overflow' computes to 'visible'"'
 * content-based height, which core/layout/block_flow.h's `block_flow_auto_height` is.
 *
 * IT IS `block_flow_auto_height` AND NOT `used_value_block_level_content_px`, WHICH IS THE ENTRY A READER
 * REACHES FOR FIRST AND IS THE WRONG ONE. That entry runs CSS 2.1 §10.6.3 only where the height BEHAVES AS AUTO
 * and otherwise answers the DECLARATION, and css-sizing-3 §5.1 defines an intrinsic size "given an auto
 * preferred size in that axis and no minimum or maximum size in that axis" — so a declared height is removed
 * from this number BY DEFINITION and reading it would make every consumer's own clamp a no-op cap on itself.
 * `block_flow_auto_height`'s own header states that it walks the children and returns their distance whatever
 * `height` says, reading that property only for CSS 2.1 §8.3.1 "Collapsing margins"' conjuncts.
 *
 * WHY IT IS A COMPONENT AND NOT A FUNCTION INSIDE EITHER CONSUMER. Two sections need this number over the same
 * boxes and neither owns it: css-flexbox-1 §9.2 "Line Length Determination"' step 3 needs it as a `column`
 * container's item's flex base size input (core/layout/flex_line.c, the USED pass), and css-flexbox-1 §9.9.3
 * "Flex Item Intrinsic Size Contributions" needs it as the same item's outer max-content size
 * (core/layout/flex_intrinsic_size.c, the INTRINSIC pass). The two passes diverge over the DECLARATION and over
 * the PERCENTAGE — which is why they have two flex-base-size functions and two clamps — and they cannot
 * diverge over this, because it is the measurement both of them substitute a declaration away in favour of. A
 * copy in each would be two answers to one question about how tall a box's content is, and the refusal below
 * would be written twice. THAT PHRASE IS INDIRECT SPEECH ON PURPOSE: this tree's own prose inside double
 * quotation marks is judged against the nearest preceding citation's section, so a quoted explanatory run here
 * reads as a fabricated quotation of css-flexbox-1 — and backticking it does not help, because the code-span
 * pattern that would mask it crosses at most ONE newline and this sentence spans more.
 *
 * NOTHING IS STORED, for core/layout/intrinsic_size.h's reason: a layout is per-flow state, so a cached
 * intrinsic size is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_BLOCK_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_BLOCK_SIZE_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/block_flow.h"

/* §3.2's INTRINSIC BLOCK SIZE of `el`'s box, as a CONTENT-box extent in CSS pixels.
   `el`'s OWN BLOCK AXIS MUST BE THE VERTICAL ONE — a computed `writing-mode` of `horizontal-tb`, asserted
   inside rather than at the call. It is this component that makes the block→physical substitution, by reaching
   for a walk that stacks children DOWNWARD, so it is this component that owes the precondition: a caller
   holding the flow-relative question has no reason to know which physical walk answers it. A consumer that
   reads PHYSICAL property names of the same element for its own arithmetic asserts the same thing again for
   its own reads, which is two components each guarding what it does rather than one guarding the other.
   A REPLACED BOX IS REFUSED BY NAME, and that refusal lives here for the reason this component exists: both
   consumers reach it through the same measurement, so the absence is stated once. */
CssPx intrinsic_block_size(lxb_dom_element_t *el);

/* THE SAME NUMBER FOR AN ANONYMOUS BLOCK CONTAINER over a RUN of `style`'s content — CSS 2.2 §9.2.1.1
   "Anonymous block boxes"' box and css-flexbox-1 §4 "Flex Items"' anonymous block container flex item, which
   are two sections generating one shape and are `intrinsic_inline_run_sizes`'s two callers on the other axis.
   IT IS CSS 2.1 §10.6.3's FIRST BULLET RATHER THAN ITS STACK, and the difference is what is inside the box:
   such a box contains only inline-level content, so by CSS 2.2 §9.2.1 "Block-level elements and block boxes" it
   "establishes an inline formatting context and thus contains only inline-level boxes" — and §10.6.3's list is
   then answered by its first item, "the bottom edge of the last line box", which `line_box_content_height` is.
   `block_flow_auto_height` cannot be asked instead, for the reason its own contract gives: it is stated over an
   ELEMENT and this box has none.
   `style` IS THE ELEMENT THE RUN'S CONTENT IS STYLED BY — the enclosing non-anonymous box, which is what both
   sections say an anonymous box inherits from (§9.2.1.1: "the properties of anonymous boxes are inherited from
   the enclosing non-anonymous box"; §4: "the anonymous item's box is unstyleable"). Its `writing-mode` is
   asserted for the same reason and about the same box as above, the anonymous one having no cascade of its own.
   THE RUN'S OWN EDGES ARE NOT ADDED, which is a derivation rather than an omission and is the identical
   sentence `intrinsic_inline_run_sizes` carries: both sections give such a box its non-inherited properties'
   initial values ("the margins will be 0"), so css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer size of
   it is its inner size unchanged. */
CssPx intrinsic_block_run_size(lxb_dom_element_t *style, BlockFlowRun run);

#endif
