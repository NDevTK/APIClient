/* css-flexbox-1 §9.9 "Intrinsic Sizes": a flex container's min-content and max-content inline sizes, which
 * core/layout/intrinsic_size.h cannot answer because neither CSS 2.2 §9.4 formatting context lays out a flex
 * container's content.
 *
 * §9.9.1 "Flex Container Intrinsic Main Sizes" and §9.9.2 "Flex Container Intrinsic Cross Sizes" are stated in
 * main/cross axes, so the inline size is §9.9.1 for a `row` container and §9.9.2 for a `column` one, per §5.1
 * "Flex Flow Direction: the flex-direction property". §9.9.2's "row multi-line flex container cross size" is a
 * block size and unreachable from this entry.
 *
 * No flex lines are needed: this runs §9.9.1.2 "Web-compatible Intrinsic Sizing Algorithm: Max-content Size and
 * Min-content Single-line Size" (a sum of contributions) and §9.9.1.3 "Multi-line Min-content Algorithm" (a
 * maximum), not §9.9.1.1's Ideal Algorithm, which "is not web compatible". The one arm needing lines,
 * §9.9.2's multi-line column max-content cross size ("the sum of the flex line cross sizes"), crashes there.
 *
 * Nothing is stored: layout is per-flow state, which solver/dom_cow.h does not swap for a cache. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_INTRINSIC_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_INTRINSIC_SIZE_H

#include <lexbor/dom/dom.h>

#include "core/layout/intrinsic_size.h"

/* §9.9's intrinsic inline sizes of the flex container `el`, as content-box widths in CSS pixels: the same
   pair core/layout/intrinsic_size.h answers for a block container. `el`'s computed `display` must be `flex`
   or `inline-flex` (css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display values"), asserted
   here because §5's properties read off any other box answer their initial value. */
IntrinsicInlineSizes flex_intrinsic_inline_sizes(lxb_dom_element_t *el);

/* §9.9.1 "Flex Container Intrinsic Main Sizes"' max-content main size of the flex container `el`, as a
   content-box extent in CSS pixels, for a container whose main axis is its block axis (`column` or
   `column-reverse`, asserted). `el`'s computed `display` must be `flex` or `inline-flex`.
   Its caller is css-flexbox-1 §9.2 "Line Length Determination"'s last step, "The automatic block size of a
   block-level flex container is its max-content size", composed in core/layout/block_flow.c.
   The min-content main size is computed by the walk but dropped here: only css-sizing-3 §3.2's `min-content`
   keyword on a height would ask for it, and the cascade refuses that keyword by name.
   No flex line is needed for either container kind: only a multi-line min-content size goes to §9.9.1.3. */
CssPx flex_intrinsic_max_content_main_block_size(lxb_dom_element_t *el);

#endif
