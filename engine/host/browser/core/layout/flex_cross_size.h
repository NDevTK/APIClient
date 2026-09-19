/* css-flexbox-1 §9.4 "Cross Size Determination"' STEPS 7 AND 8 AND §9.6 "Cross-Axis Alignment"' LAST-BUT-ONE
 * STEP — A FLEX CONTAINER'S CONTENT-BASED CROSS SIZE, which is the question core/layout/block_flow.h answers
 * for every other box with CSS 2.1 §10.6.3's stack of block-level children and cannot answer for this one:
 * §10.6.3 adds each child's margin box to a running column with §8.3.1's margins collapsing between them, and
 * two flex items on one line do not stack at all — the container's cross size is the LARGEST of them.
 *
 * WHY THREE STEPS OF TWO SECTIONS ARE ONE COMPONENT AND ONE CALL. §9.6's step is the READER and §9.4's steps
 * 7 and 8 are what it reads: "Determine the flex container's used cross size using the rules of the formatting
 * context in which it participates. If a content-based cross size is needed, use the sum of the flex lines'
 * cross sizes." For a SINGLE-LINE container that sum has one term, so §9.6's step is §9.4's step 8 for the one
 * line §9.3 "Main Size Determination"' step 5 collected — there is no intermediate object for a caller to hold
 * and no second entry for it to ask. §9.4's step 8 in turn is stated over the items' HYPOTHETICAL CROSS SIZES,
 * which is its own step 7, and nothing between the two is observable from outside. Splitting them would
 * publish two numbers whose only consumer is each other.
 *
 * IT IS THE CROSS AXIS AND NOTHING ELSE, which is §5.1 "Flex Flow Direction: the flex-direction property"'
 * dispatch rather than a narrowing — the same sentence core/layout/flex_line.h states for the main axis and
 * the mirror of it. A `row` container's cross axis is its BLOCK axis, so this answers a HEIGHT; a `column`
 * container's cross axis is its inline axis and this entry refuses it by name, because §9.2 "Line Length
 * Determination"' last step and not §9.6 is what determines that container's block size and the two share no
 * step. A caller holding a PHYSICAL axis asks core/layout/flex_item.h's `flex_container_main_axis` first.
 *
 * WHAT IT DOES NOT DO, AND WHY EACH IS A DIFFERENT SECTION RATHER THAN A CASE. §9.4's step 9 distributes
 * spare cross space to the lines under `align-content: stretch` and its condition is "if the flex container
 * has a DEFINITE cross size" — which is exactly what this entry is called to produce and therefore exactly
 * what it does not have, so that step is unreachable from here rather than skipped. §9.4's step 11 gives each
 * ITEM its used cross size from the line's, and §9.6's steps 12, 13 and 15 place them; neither is a size this
 * entry is asked for, and core/layout/used_value.c refuses an item's used cross size by name at both of its
 * arms for that reason.
 *
 * NOTHING IS STORED, for core/layout/flex_line.h's reason: a layout is per-flow state, so a cached line cross
 * size is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. The cost is
 * stated here because a reader meets it before they meet the reason — this entry asks
 * `flex_line_used_main_size` once per item and that entry resolves the whole line each time, so a container of
 * N items costs N line resolutions and this walk adds a second N on top of them.
 *
 * THE RELEASE ARM OF EVERY `DFAIL` BELOW RETURNS A NUMBER, and that is stated rather than left to be
 * discovered: with the asserts compiled out each refusal falls through into the walk beneath it, so a
 * multi-line container answers from one line holding every item and a `column` container answers a cross size
 * where a main size was asked for. Both are DEFINED wrong numbers of the same kind CSS 2.1 §10.6.3's stack
 * already was for this box, which is what §Offensive-programming's release exemption leaves behind and is not
 * this component's to choose. What it does NOT leave behind is a state a later component has no step for: the
 * answer is always a non-negative extent on the axis asked for. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_CROSS_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_CROSS_SIZE_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* css-flexbox-1 §9.6 "Cross-Axis Alignment"' "If a content-based cross size is needed, use the sum of the flex
   lines' cross sizes" for `container`, as a CONTENT-box extent in CSS pixels on the container's CROSS axis.
   `container` must BE a flex container (css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display
   values", core/layout/flex_item.h's `flex_item_display_is_flex_container` answering true for its computed
   `display`) and its main axis must be its INLINE axis — a `row` or `row-reverse` container by §5.1 "Flex Flow
   Direction: the flex-direction property"' mapping. Both are asserted here rather than at the call, for
   core/layout/flex_line.h's reason: this component reads §5, §8 and §9's properties, whose `Applies to:` lines
   are flex containers and flex items, and reading one off any other box answers the cascade's initial keyword.
   IT IS THE CONTENT BOX AND §9.4's ARITHMETIC IS TOO. §9.6's own sentence makes the answer the sum of the flex
   lines' cross sizes, and a flex line is inside the container's content box with nothing between them — so
   there is no border and no padding in this number, and css-sizing-3 §3.3 "Box Edges for Sizing: the
   box-sizing property"' conversion belongs to whichever caller exposes a used value. CSS 2.1 §10.6.3's walk
   answers a content extent for the same reason and its caller converts, which is why this can stand in for it.
   IT IS A CONTENT-BASED SIZE AND THEREFORE ONLY EVER THE ANSWER WHERE ONE IS NEEDED, WHICH IS §9.6's OWN
   CONDITIONAL AND NOT A FACT ABOUT THE DECLARATION. §9.4's step 8 has an arm for the other case — "If the
   flex container is single-line and has a definite cross size, the cross size of the flex line is the flex
   container's inner cross size" — and it is tempting to read `used_value_height_behaves_as_auto`
   (css-sizing-3 §3.2.1 "“Behaving as auto”") as the predicate that keeps this entry out of it. IT IS NOT, and
   the difference is a page rather than a nicety: css-sizing-3 §3.2 "Sizing Values: the
   <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content values" makes a
   `min-content` or `max-content` BLOCK size "equivalent to its automatic size", so `min-height: max-content`
   on a flex container with a DECLARED `height` asks for exactly this number with a definite cross size in
   hand — and is right to, because the AUTOMATIC size is the one computed with the cross axis treated as
   indefinite. So the caller reads §9.6's condition; this entry asserts nothing about the declaration. */
CssPx flex_cross_size_content_based(lxb_dom_element_t *container);

#endif
