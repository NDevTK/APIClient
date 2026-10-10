/* css-sizing-3 §3.2 "Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch, min-content,
 * max-content, and fit-content values": a box's intrinsic size in its block axis. core/layout/intrinsic_size.h
 * answers the inline axis by laying out line boxes; a block size is what CSS 2.1 §10.6 stacks.
 *
 * It is one number: for both `min-content` and `max-content`, "for a box's block size, unless otherwise
 * specified, this is equivalent to its automatic size". css-writing-modes-4 §7.2 "Dimensional Mapping" sends
 * that to CSS 2.1 §10.6, so for a non-replaced block-level box it is CSS 2.1 §10.6.3 "Block-level non-replaced
 * elements in normal flow when 'overflow' computes to 'visible'"' content height, `block_flow_auto_height`.
 * Not `used_value_block_level_content_px`, which answers a declared height: css-sizing-3 §5.1 "Intrinsic Sizes"
 * defines an intrinsic size "given an auto preferred size in that axis and no minimum or maximum size".
 *
 * Its two consumers are css-flexbox-1 §9.2 "Line Length Determination"' step 3 (core/layout/flex_line.c) and
 * §9.9.3 "Flex Item Intrinsic Size Contributions" (core/layout/flex_intrinsic_size.c); they differ over the
 * declaration and percentages but must agree on this measurement, so it is one component. Nothing is stored:
 * layout is per-flow state, which solver/dom_cow.h does not swap for a cache. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_BLOCK_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_BLOCK_SIZE_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/block_flow.h"

/* §3.2's intrinsic block size of `el`'s box, as a content-box extent in CSS pixels.
   `el`'s block axis must be vertical (computed `writing-mode` `horizontal-tb`), asserted inside: this
   component chooses the downward-stacking walk, so it owes the precondition. A replaced box is refused by
   name (CSS 2.1 §10.6.2 is not wired here). */
CssPx intrinsic_block_size(lxb_dom_element_t *el);

/* The same number for an anonymous block container over a run of `style`'s content: CSS 2.2 §9.2.1.1
   "Anonymous block boxes"' box and css-flexbox-1 §4 "Flex Items"' anonymous block container flex item.
   Such a box "establishes an inline formatting context and thus contains only inline-level boxes" (CSS 2.2
   §9.2.1 "Block-level elements and block boxes"), so the answer is §10.6.3's first item, "the bottom edge of
   the last line box" (`line_box_content_height`); `block_flow_auto_height` needs an element.
   `style` is the enclosing non-anonymous box the run inherits from ("the properties of anonymous boxes are
   inherited from the enclosing non-anonymous box"); its `writing-mode` is asserted as above. The run's own
   edges are not added: its non-inherited properties are initial ("the margins will be 0"). */
CssPx intrinsic_block_run_size(lxb_dom_element_t *style, BlockFlowRun run);

#endif
