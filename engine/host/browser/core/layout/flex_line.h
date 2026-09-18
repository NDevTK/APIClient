/* css-flexbox-1 §9.3 "Main Size Determination" and §9.7 "Resolving Flexible Lengths" — A FLEX ITEM'S USED MAIN
 * SIZE, which is the question core/layout/used_value.h answers for every other box and cannot answer for this
 * one: CSS 2.1 §10.3.3's constraint equation divides ONE box's slack between its own margins, and a flex item's
 * main size is divided between it and its SIBLINGS out of a free space none of them owns alone.
 *
 * IT IS THE MAIN AXIS AND NOTHING ELSE, and that is §5.1's dispatch rather than a narrowing. §9.3's two steps
 * are the last two of the three §9.2 "Line Length Determination" opens, and every one of them is stated in the
 * container's MAIN axis; a flex item's CROSS size is §9.4 "Cross Size Determination"'s step 11 and shares no
 * step with any of this. So a caller holding a PHYSICAL axis asks core/layout/flex_item.h's
 * `flex_container_main_axis` first and reaches this component only for the axis that mapping names.
 *
 * WHY §9.7 NEEDS §9.3 AND WHY THAT IS NOT A SECOND LINE-BREAKING ALGORITHM. §9.7's own first words are "To
 * resolve the flexible lengths of the items within a flex line", so the LINE is its operand and not a detail
 * of its caller — every sum in it is over "all items on the line" and the free space it distributes is the
 * container's inner main size less those items. §9.3's first step gives a SINGLE-LINE container that line in
 * one sentence — "If the flex container is single-line, collect all the flex items into a single flex line" —
 * so `flex-wrap: nowrap` needs no breaking at all, which is why the two steps are one component here and why a
 * multi-line container CRASHES rather than being served by a line this file invented.
 *
 * NOTHING IS STORED, for core/layout/intrinsic_size.h's reason: a layout is per-flow state, so a cached used
 * main size is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. One
 * consequence is stated here because a reader will meet it before they meet the reason: asking this entry for
 * N items of one container resolves that container N times. §9.7's loop is over the items of ONE line and its
 * answer for any of them is a function of ALL of them, so there is no smaller unit to ask for — and a cache
 * keyed on the container would be exactly the per-flow state that is banned.
 *
 * WHAT THIS COMPONENT IS NOT. It does not place anything: §9.5 "Main-Axis Alignment" distributes the REMAINING
 * free space to margins and to `justify-content`, and this file stops at the sentence §9.7 ends on — "Set each
 * item's used main size to its target main size." A caller wanting a position asks core/layout/flow_position.h,
 * which has no arm for a flex line yet and says so. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_LINE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_LINE_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* §9.7's last step for ONE item — "Set each item's used main size to its target main size" — as a CONTENT-box
   extent in CSS pixels, in the MAIN axis of `container`.
   `item` must be one of `container`'s flex items (css-flexbox-1 §4 "Flex Items", core/layout/flex_item.h's
   `flex_item_child_kind` answering `FLEX_ITEM_CHILD_ELEMENT` for it), and `container`'s main axis must be its
   INLINE axis — a `row` or `row-reverse` container, by §5.1 "Flex Flow Direction: the flex-direction
   property"' mapping. Both are asserted here rather than at the call, for core/layout/flex_intrinsic_size.h's
   reason: this component reads §5, §7 and §9's properties, whose `Applies to:` lines are flex containers and
   flex items, and reading one off any other box answers the cascade's initial keyword.
   IT IS A CONTENT BOX AND §9.7's OWN ARITHMETIC IS TOO, which is worth stating because the used value
   core/layout/used_value.h exposes is not: css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"
   makes the used value "as exposed for instance through getComputedStyle()" the border box's under
   `border-box`, while §9.7 floors "its content-box size at zero" in its own words and §7.2.3 "The flex-basis
   property" says "flex-basis determines the size of the content box, unless otherwise specified, such as by
   box-sizing". So the conversion belongs to the CALLER that exposes a used value, and every number inside this
   component is an inner one. */
CssPx flex_line_used_main_size(lxb_dom_element_t *container, lxb_dom_element_t *item);

#endif
