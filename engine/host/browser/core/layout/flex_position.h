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

/* The offset of element flex item `item`'s border-box top-left corner from `container`'s content-box top-left
   corner, physical, in CSS pixels. `container` must be `item`'s flex container in the box tree (asserted). */
void flex_position_item_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y);

/* The used value of element flex item `item`'s margin `name` (`margin-left`, `-right`, `-top` or `-bottom`):
   §9.5 step 1's share of the line's free space for an `auto` main-axis margin and §9.6 step 13's for a cross-axis
   one, as the same placement above uses them. core/layout/used_value.h answers a flex item's `auto` margin
   through this. */
CssPx flex_position_item_margin(lxb_dom_element_t *container, lxb_dom_element_t *item, const char *name);

/* css-flexbox-1 §4's anonymous flex item of `container` (the "anonymous block container flex item" wrapping a
   child text sequence): its border box's top-left corner from `container`'s content-box top-left corner and its
   width and height, physical, in CSS pixels. Its margins, border and padding are zero, so this is also its
   margin and content box. The item is named by `first`, the first text node of its sequence, which is the
   identity core/layout/flex_line.h's members and used main size use; a later node of the same sequence names
   no item and is refused (asserted), so no caller reads the item's box as if it were a later run's. Built for a
   `row` or `row-reverse` container in `horizontal-tb` without `wrap-reverse`; the others are refused by name. */
void flex_position_anonymous_box(lxb_dom_element_t *container, const lxb_dom_node_t *first,
                                 CssPx *x, CssPx *y, CssPx *w, CssPx *h);

#endif
