/* css-flexbox-1 §9.5 "Main-Axis Alignment" and §9.6 "Cross-Axis Alignment": where a flex item's border box
 * sits inside its flex container's content box, over the used main sizes of core/layout/flex_line.h and the
 * used cross sizes core/layout/used_value.h answers through core/layout/flex_cross_size.h.
 *
 * Every `flex-direction` is placed: §5.1 "Flex Flow Direction: the flex-direction property" and §5.2's
 * cross-start are mapped to physical sides by core/layout/flex_item.h's `flex_container_start_margin`.
 * A multi-line container's items are placed per line, the lines by `align-content` over flex_cross_size.h's
 * per-line cross sizes. Refused at the entry, by name: `wrap-reverse`, a vertical writing mode, and a collapsed
 * item's strut. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_POSITION_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_POSITION_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/layout_question.h"

/* The offset of element flex item `item`'s border-box top-left corner from `container`'s content-box top-left
   corner, physical, in CSS pixels. `container` must be `item`'s flex container in the box tree (asserted), and
   its placement asked (`flex_position_operands_ask`) under the open drive, which asserts it served: a read of
   the memoized answer. */
void flex_position_item_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y);
/* The layout the entry above reads, asked under a step machine's resting drive (core/layout/layout_job.h):
   `container`'s content box on both axes, its §9.3 line membership, each element item's used sizes' operands
   on both axes and, for an item in a baseline-sharing group, its first and last line box baselines, and each
   anonymous item's line boxes, and then the placement of every element item over them. One layout job
   question per container (LAYOUT_Q_FLEX_PLACE), answered once per job however many of its items are read:
   true once memoized; false ("not yet") with it suspended for the caller's drive. */
bool flex_position_operands_ask(lxb_dom_element_t *container);
/* LAYOUT_Q_FLEX_PLACE's body, which only block_flow_drive_deferred calls: the asks above, each member's, then
   §9.5 once per line and §9.6 once per line and item. */
void flex_position_question_run(LayoutQuestion q, size_t at);

/* The used value of element flex item `item`'s margin `name` (`margin-left`, `-right`, `-top` or `-bottom`):
   §9.5 step 1's share of the line's free space for an `auto` main-axis margin and §9.6 step 13's for a cross-axis
   one, as the same placement above uses them. core/layout/used_value.h answers a flex item's `auto` margin
   through this; it is a synchronous used value, so it drives the container's members once. */
CssPx flex_position_item_margin(lxb_dom_element_t *container, lxb_dom_element_t *item, const char *name);

/* css-flexbox-1 §4's anonymous flex item of `container` (the "anonymous block container flex item" wrapping a
   child text sequence): its border box's top-left corner from `container`'s content-box top-left corner and its
   width and height, physical, in CSS pixels. Its margins, border and padding are zero, so this is also its
   margin and content box. The item is named by `first`, the first text node of its sequence, which is the
   identity core/layout/flex_line.h's members and used main size use; a later node of the same sequence names
   no item and is refused (asserted), so no caller reads the item's box as if it were a later run's. Built for a
   `row` or `row-reverse` container in `horizontal-tb` without `wrap-reverse`; the others are refused by name.
   Its operands are `flex_position_operands_ask`'s, asked under the open drive (asserted, as above). */
void flex_position_anonymous_box(lxb_dom_element_t *container, const lxb_dom_node_t *first,
                                 CssPx *x, CssPx *y, CssPx *w, CssPx *h);

#endif
