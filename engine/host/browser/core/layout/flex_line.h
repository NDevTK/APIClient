/* css-flexbox-1 §9.3 "Main Size Determination" and §9.7 "Resolving Flexible Lengths": a flex item's used main
 * size. core/layout/used_value.h cannot answer it, because CSS 2.1 §10.3.3 divides one box's slack between its
 * own margins while a flex item's main size comes from a free space shared with its siblings.
 *
 * Main axis only, per §5.1's mapping: a flex item's cross size is §9.4 "Cross Size Determination"'s step 11.
 * A caller holding a physical axis asks core/layout/flex_item.h's `flex_container_main_axis` first.
 *
 * §9.7 operates on a flex line ("To resolve the flexible lengths of the items within a flex line"), and §9.3's
 * first step gives a single-line container that line: "If the flex container is single-line, collect all the
 * flex items into a single flex line". A multi-line container crashes until line breaking is built.
 *
 * Nothing is stored: layout is per-flow state and a cache would be shared state solver/dom_cow.h does not
 * swap. Asking for N items of one container therefore resolves the line N times; each item's answer depends on
 * all of them. This stops at "Set each item's used main size to its target main size."; §9.5 "Main-Axis
 * Alignment" (positions) is not built here. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_LINE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_LINE_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* §9.7's last step for one item, as a content-box extent in CSS pixels in `container`'s main axis.
   `item` is the item's first node: the element for `FLEX_ITEM_CHILD_ELEMENT`, or the first text node of a
   child text sequence for `FLEX_ITEM_CHILD_TEXT` (core/layout/flex_item.h; `flex_item_text_sequence_end`
   delimits it), since css-flexbox-1 §4 "Flex Items" wraps each such sequence in an "anonymous block container
   flex item". A node the child list holds names every item uniquely without a sentinel element.
   `container`'s main axis must be its inline axis (`row` or `row-reverse`, §5.1 "Flex Flow Direction: the
   flex-direction property"); both preconditions are asserted here, since reading §5, §7 or §9's properties
   off a box outside their `Applies to:` line answers the initial value.
   The result is a content box, as §9.7's arithmetic is ("its content-box size", and §7.2.3 "The flex-basis
   property": "flex-basis determines the size of the content box"); converting to css-sizing-3 §3.3's
   `box-sizing` used value is the exposing caller's job. */
CssPx flex_line_used_main_size(lxb_dom_element_t *container, lxb_dom_node_t *item);

#endif
