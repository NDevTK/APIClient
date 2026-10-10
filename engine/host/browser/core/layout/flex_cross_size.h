/* css-flexbox-1 §9.4 "Cross Size Determination"' steps 7, 8 and 11 and §9.6 "Cross-Axis Alignment"'s container
 * cross-size step: a flex container's content-based cross size, and a flex item's used cross size. CSS 2.1
 * §10.6.3 stacks children; items on one line do not stack, and the line is as tall as the largest of them.
 *
 * §9.6's step reads §9.4's: "Determine the flex container's used cross size using the rules of the formatting
 * context in which it participates. If a content-based cross size is needed, use the sum of the flex lines'
 * cross sizes." For a single-line container the sum has one term, step 8's line over step 7's hypothetical
 * sizes, so it is one call with no intermediate object. Step 11 is the second entry because it shares the item
 * walk, cross edges, §8.3 alignment and line size. The cross axis is whichever one §5.1 makes it; the physical
 * axis comes from `flex_container_axis_is_vertical` (core/layout/flex_item.h).
 * §9.4's step 9 (`align-content: stretch` on a "definite cross size") is unreachable here, since a
 * content-based size is asked exactly when there is none. §9 numbers steps globally, so §9.6 holds steps 13 to
 * 16; 13, 14 and 16 place items and are not here. Nothing is stored (per-flow state, solver/dom_cow.h). */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_CROSS_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_CROSS_SIZE_H

/* Cost: `flex_line_used_main_size` resolves the whole line per ask. An anonymous item asks it directly; an
   element item reaches it inside `used_value_block_level_content_px` (its `auto` height lays out line boxes at
   its used width, which routes through `uv_flex_item_main_size`). So N items cost N line resolutions plus
   this walk's own, and publishing a line from core/layout/flex_line.h would not retire the element items'
   asks; a stated main size on `used_value_block_level_content_px` would.
   Release arms: with the DFAILs compiled out, a multi-line container answers from one line holding every item
   and a `column` container answers a cross size where a main size was asked. Both are defined numbers, never
   a negative extent or one on the wrong axis. */

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* css-flexbox-1 §9.6 "Cross-Axis Alignment"' "If a content-based cross size is needed, use the sum of the flex
   lines' cross sizes" for `container`, as a content-box extent in CSS pixels on its cross axis. `container`
   must be a flex container (`flex_item_display_is_flex_container`), single-line and `horizontal-tb`, and its
   cross axis must be its block axis (`row` or `row-reverse`); asserted here, since §5, §8 and §9's properties
   read off another box answer their initial values.
   The block-axis precondition is about who asks: a `column` container's cross size is a width whose formatting
   context rule, CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal flow", needs no content; a
   shrink-to-fit `column` container asks §9.9.2 "Flex Container Intrinsic Cross Sizes" instead
   (core/layout/flex_intrinsic_size.c).
   The answer is a content box (a flex line sits inside the content box); css-sizing-3 §3.3's conversion is the
   exposing caller's. Whether one is needed is the caller's reading of §9.6's conditional, not a test of the
   declaration: css-sizing-3 §3.2's `min-content`/`max-content` block size is "equivalent to its automatic
   size", so `min-height: max-content` on a container with a declared `height` asks for this number with a
   definite cross size in hand, and is right to. */
CssPx flex_cross_size_content_based(lxb_dom_element_t *container);

/* css-flexbox-1 §9.4 "Cross Size Determination"' step 11: one flex item's used cross size, as a content-box
   extent in CSS pixels on `container`'s cross axis, for an item whose cross size property computes to `auto` or
   behaves as if it did. `item` is an element: the only caller, core/layout/used_value.c, is asked for
   elements; §4's anonymous item needs a used cross size only when §9.6 places it, which is unbuilt.
   A declared cross size takes step 11's "Otherwise, the used cross size is the item's hypothetical cross size",
   which is step 7 laying out "as if it were an in-flow block-level box": CSS 2.1 §10.5 "Content height: the
   'height' property" and §10.2 "Content width: the 'width' property" make the declaration the size (§10.6.3 and
   §10.3.3 solve only for `auto`). used_value.c's declared arm computes that and does not ask here.
   No clamp here: §8.3's "while still respecting the constraints imposed by min-height/min-width/max-height/
   max-width" is CSS 2.1 §10.7, run by the caller (`uv_sized` re-runs the pass with the limit); step 11, unlike
   §9.7, states no clamp of its own.
   `container` must be `item`'s flex container in the box tree, single-line and `horizontal-tb`. Either axis
   may be the cross one; `horizontal-tb` keeps the one `vertical` bit naming both the physical edge pair
   (css-writing-modes-4 §7.2 "Dimensional Mapping") and the sizing dimension (CSS 2.1 §10.6 versus css-sizing-3
   §5.1 "Intrinsic Sizes"). */
CssPx flex_cross_size_used_item_cross(lxb_dom_element_t *container, lxb_dom_element_t *item);

#endif
