/* css-flexbox-1 §4 "Flex Items": which boxes a flex container's child list is, with §5.1 "Flex Flow Direction:
 * the flex-direction property"'s main axis and §5.2 "Flex Line Wrapping: the flex-wrap property"'s line count,
 * which every §9 "Flex Layout Algorithm" step dispatches on first.
 *
 * This is not core/layout/block_flow.h's enumeration. "Each in-flow child of a flex container becomes a flex
 * item", with no block/inline levels ("flex items themselves are flex-level boxes, not block-level boxes";
 * css-display-3 §2.7 "Automatic Box Type Transformations" has blockified them). A float is a flex item (§4's
 * example: "floating is ignored"). The anonymous item wraps a child text sequence, which ends at any
 * box-generating element, so "flex items do not split around blocks", unlike CSS 2.2 §9.2.1.1's run.
 *
 * §4's white-space rule ("if the entire text sequences contains only document white space characters (i.e.
 * characters that can be affected by the white-space property) it is instead not rendered (just as if its text
 * nodes were display:none)") shares only its character set with core/layout/block_flow.h's §9.2.2.1 test (one
 * css-text-3 §4 derivation, U+000C outside it). §4's condition reads no property, so `white-space: pre` does
 * not keep such a sequence. Nothing is stored: a box tree is per-flow state solver/dom_cow.h does not swap. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_ITEM_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_ITEM_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_display.h"

/* css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display values"' two values, decided from a
   computed `display` alone, so a consumer can learn which module owns a box first. "A flex container
   establishes a new flex formatting context for its contents" and "Flex containers are not block containers":
   the pair differs in outer display type only. */
bool flex_item_display_is_flex_container(CssDisplay display);

/* §5.1's main axis, named by which writing-mode axis it is. `row` "has the same orientation as the inline axis
   of the current writing mode", `column` the block axis, and each `-reverse` is "Same as" its base value except
   main-start and main-end "are swapped": an order, deliberately invisible here so callers cannot give two
   answers for one axis. */
typedef enum {
    FLEX_MAIN_AXIS_INLINE = 0,  /* §5.1's `row` and `row-reverse` */
    FLEX_MAIN_AXIS_BLOCK        /* §5.1's `column` and `column-reverse` */
} FlexMainAxis;

FlexMainAxis flex_container_main_axis(lxb_dom_element_t *el);

/* A flex container's two axes, css-flexbox-1 §2 "Flex Layout Box Model": "The main axis of a flex container is
   the primary axis along which flex items are laid out", and "The axis perpendicular to the main axis is called
   the cross axis." */
typedef enum {
    FLEX_AXIS_MAIN = 0,
    FLEX_AXIS_CROSS
} FlexAxis;

/* Whether one of `container`'s two axes is the vertical one: §5.1's mapping composed with css-writing-modes-4
 * §6.4 "Abstract-to-Physical Mappings" (core/css/css_logical.h), joined here once because layout operands are
 * physical.
 *
 * The writing mode is `container`'s: css-writing-modes-4 §7.4 "Flow-Relative Mappings" lays a box out in its
 * containing block's writing mode, and the flex container is its items' containing block. An item in a
 * perpendicular mode is a css-writing-modes-4 §7.3 "Orthogonal Flows" box (refused by
 * core/layout/flex_intrinsic_size.c), so this answer says nothing about an item's own axes.
 *
 * Asserts `container` is a flex container (§5.1 applies to flex containers only; elsewhere the read would be
 * the initial `row`). */
bool flex_container_axis_is_vertical(lxb_dom_element_t *container, FlexAxis axis);

/* §5.2's two arms, in §6 "Flex Lines"' words: "A single-line flex container (i.e. one with flex-wrap: nowrap)
   lays out all of its children in a single line, even if that would cause its contents to overflow", versus a
   multi-line one, "one with flex-wrap: wrap or flex-wrap: wrap-reverse". True is multi-line; `wrap-reverse`'s
   cross-start swap is invisible here. */
bool flex_container_is_multi_line(lxb_dom_element_t *el);

/* §4's classification of one child node, exhaustive over a flex container's child list. It answers rather
   than refuses: what a caller does with the fact is its own section's concern. */
typedef enum {
    FLEX_ITEM_CHILD_NONE = 0,  /* generates no box, or §4.1 "Absolutely-Positioned Flex Children" takes it out
                                  of flow, or §4's white-space rule leaves the text sequence unrendered */
    FLEX_ITEM_CHILD_ELEMENT,   /* this element child IS one flex item */
    FLEX_ITEM_CHILD_TEXT       /* this text node is inside a child text sequence that §4 wraps in one anonymous
                                  block container flex item; `flex_item_text_sequence_end` delimits it */
} FlexItemChildKind;

FlexItemChildKind flex_item_child_kind(lxb_dom_element_t *container, lxb_dom_node_t *child);

/* §4's child text sequence, delimited: one past the last text node of the maximal sequence containing `first`,
   in core/layout/line_box.h's (first, end) half-open form. `first` must answer `FLEX_ITEM_CHILD_TEXT`.
   Contiguity is css-display-3's: "each contiguous sequence of sibling text nodes generates a text sequence"
   (§1 "Introduction"), comments and the like "are ignored, as if they didn't exist", and §2.5 "Box Generation:
   the none and contents keywords"' `none` elements are ignored "as if they did not exist in the box tree".
   `contents` is the opposite case: the element is "replaced in the element tree by its contents", so sibling
   means a box-tree relation, and this and the classification above walk core/layout/box_tree.h's sequence. A
   `contents` element holding a box ends a sequence through that box; an empty one leaves neighbours contiguous.
   The difference is a number: §9.9.1.2 "Web-compatible Intrinsic Sizing Algorithm: Max-content Size and
   Min-content Single-line Size" sums item contributions while one item's min-content is a maximum over its
   segments, so splitting a sequence widens the container. */
lxb_dom_node_t *flex_item_text_sequence_end(lxb_dom_element_t *container, lxb_dom_node_t *first);

/* §4.4 "Collapsed Items"' collapsed flex item: `item`'s computed `visibility` is `collapse` ("Specifying
   visibility: collapse on a flex item causes it to become a collapsed flex item"; css-display-3 §4
   "Invisibility: the visibility property" declares the keyword).
   §9.9.1.2 and §9.9.1.3 state their operand as the "non-collapsed" flex items; §9.9.2 "Flex Container
   Intrinsic Cross Sizes" does not, because a collapsed item "leaves behind a strut that keeps the flex line's
   cross-size stable", so the cross walk does not ask this. An anonymous item is never collapsed (unstyleable,
   initial `visible`), which is why this takes an element. */
bool flex_item_is_collapsed(lxb_dom_element_t *item);

/* css-flexbox-1 §7.2.1 "The flex-grow property"' and §7.2.2 "The flex-shrink property"' flexibility factor of
   `item`. `name` is `flex-grow` or `flex-shrink`; the answer is the `<number [0,∞]>` the cascade holds (computed
   "specified number" / "specified value").
   Shared because §9.9.3 "Flex Item Intrinsic Size Contributions" reads its sign ("if the item is not growable",
   "not shrinkable") and §9.7 "Resolving Flexible Lengths" reads the number (the distribution ratio and its
   less-than-one sum test). Zero gets no share: §9.7's step 2 freezes such an item. */
double flex_item_flexibility_factor(lxb_dom_element_t *item, const char *name);


#endif
