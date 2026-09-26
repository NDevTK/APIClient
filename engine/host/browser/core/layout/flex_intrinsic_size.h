/* css-flexbox-1 §9.9 "Intrinsic Sizes" — A FLEX CONTAINER'S MIN-CONTENT AND MAX-CONTENT INLINE SIZES, which is
 * the question core/layout/intrinsic_size.h answers for a CSS 2.2 §9.4 block container and cannot answer for
 * this box: neither of §9.4's two formatting contexts lays a flex container's content out, so neither §9.4.1
 * "Block formatting contexts"' stack nor §9.4.2 "Inline formatting contexts"' line is the algorithm.
 *
 * §9.9 IS TWO SECTIONS AND WHICH ONE ANSWERS AN *INLINE* SIZE IS §5.1's QUESTION, NOT A CASE INSIDE EITHER.
 * §9.9.1 "Flex Container Intrinsic Main Sizes" and §9.9.2 "Flex Container Intrinsic Cross Sizes" are stated in
 * FLOW-RELATIVE axes — main and cross — and a caller here asks for an INLINE size, so the dispatch is §5.1
 * "Flex Flow Direction: the flex-direction property"' mapping run backwards: a `row` container's main axis
 * "has the same orientation as the inline axis of the current writing mode", so §9.9.1 is what it is asked
 * for; a `column` container's main axis is the block axis, so its CROSS axis is the inline one and §9.9.2 is.
 * EXACTLY TWO OF THE FOUR ARMS ARE REACHABLE THROUGH THIS ENTRY and that is worth stating, because §9.9.2
 * spells out a `row` case and a reader who meets it here will look for it: §9.9.2's "row multi-line flex
 * container cross size" is a BLOCK size for a row container, so nothing that asks this component for an inline
 * size can reach it, and §9.9.1 is unreachable for a column container for the mirror reason.
 *
 * §9.9 NEEDS NO FLEX LINES FOR EITHER OF THOSE TWO, AND THAT IS THE FIRST THING TO KNOW ABOUT IT. §9.9.1's own
 * words are that "an implementation is conformant to CSS Flexible Box Layout if it conforms to either the
 * Ideal Algorithm or the Web-compatible Algorithm", and the second of those — §9.9.1.2 "Web-compatible
 * Intrinsic Sizing Algorithm: Max-content Size and Min-content Single-line Size" — is two sentences of
 * arithmetic over the items with no line in either: "take the sum of the max-content contributions of all the
 * non-collapsed flex items in the flex container", and the same sentence for a single-line container's
 * min-content size. §9.9.1.3 "Multi-line Min-content Algorithm" is the third case and is a MAXIMUM, again over
 * items rather than lines. §9.9.1.1 "Ideal Algorithm: Max-content Size and Min-content Single-line Size" is
 * the one that places items into lines, and §9.9.1.1's own note says why this engine does not run it: "because
 * it was not implemented correctly initially and existing content became dependent on the unfortunately
 * consistent incorrect implemented behavior it is not web compatible". So a flex container's intrinsic inline
 * size is NOT gated on §9.3 "Main Size Determination"'s line breaking, and treating it as though it were is
 * what would make this component wait for the whole of §9.
 * ONE ARM DOES NEED LINES AND IT IS NAMED WHERE IT IS REACHED, not here: §9.9.2's max-content cross size of a
 * MULTI-LINE COLUMN container "is the sum of the flex line cross sizes", which is §9.4 "Cross Size
 * Determination" over §9.3's lines and is the only thing in this component that is.
 *
 * NOTHING IS STORED, for core/layout/intrinsic_size.h's reason: a layout is per-flow state, so a cached
 * intrinsic size is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_INTRINSIC_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_INTRINSIC_SIZE_H

#include <lexbor/dom/dom.h>

#include "core/layout/intrinsic_size.h"

/* §9.9's INTRINSIC INLINE SIZES of the flex container `el`, as CONTENT-box widths in CSS pixels — the same
   pair and the same box core/layout/intrinsic_size.h answers for a block container, so a caller that has one
   of them has both and never has to know which module produced its number. `el`'s computed `display` must be
   `flex` or `inline-flex` (css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display values"),
   asserted here rather than at the call: this component reads §5's properties, whose `Applies to:` line is
   flex containers, and reading one off any other box answers the cascade's initial keyword. */
IntrinsicInlineSizes flex_intrinsic_inline_sizes(lxb_dom_element_t *el);

/* §9.9.1 "Flex Container Intrinsic Main Sizes"' MAX-CONTENT MAIN SIZE of the flex container `el`, as a
   CONTENT-box extent in CSS pixels, for the container whose MAIN axis is its BLOCK axis — a `column` or
   `column-reverse` container, by §5.1 "Flex Flow Direction: the flex-direction property"' mapping, asserted
   inside. `el`'s computed `display` must be `flex` or `inline-flex`, for the reason the entry above states.
   WHO ASKS, AND WHY THE ANSWER IS ONE NUMBER RATHER THAN A PAIR OR A GENERIC MAIN SIZE. css-flexbox-1 §9.2
   "Line Length Determination"' last step is the caller in one sentence — "Determine the main size of the flex
   container using the rules of the formatting context in which it participates. The automatic block size of a
   block-level flex container is its max-content size." So what a `column` container's auto HEIGHT needs is
   exactly §9.9.1's MAX-CONTENT size and nothing else, and core/layout/block_flow.c's content-based height is
   where that sentence is composed.
   THE MIN-CONTENT MAIN SIZE IS NOT HERE AND IS NOT DEFERRED WORK WITH A CRASH BEHIND IT — it is a question
   nothing in this engine can currently ask. The only route to it is css-sizing-3 §3.2 "Sizing Values: the
   <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content values"'
   `min-content` keyword on a `height` or `min-height`, and this engine records no computed-value rule for that
   keyword: every entry that reads the grammar refuses it BY NAME. Returning a pair would have been one half
   nothing exercises, which is the write-with-no-reader shape rather than a wider answer. The walk behind this
   entry computes both halves and the other one is dropped at this boundary, so building that reader later is a
   signature change and not an algorithm.
   IT NEEDS NO FLEX LINE AND NO MULTI-LINE ARM, which is the one thing about §9.9.1 most likely to be assumed
   the other way, since the cross-size walk behind the entry above REFUSES a multi-line container by name.
   §9.9.1 hands only a MULTI-LINE MIN-CONTENT size to §9.9.1.3 "Multi-line Min-content Algorithm", and
   §9.9.1.2's max-content sentence is stated over a flex container with no line-count condition on it — so
   §9.3 "Main Size Determination"'s line breaking is not on this path for either kind of container. */
CssPx flex_intrinsic_max_content_main_block_size(lxb_dom_element_t *el);

#endif
