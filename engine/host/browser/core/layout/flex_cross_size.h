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
 * the mirror of it. WHICH PHYSICAL AXIS THAT IS, IS ASKED RATHER THAN ASSUMED: a `row` container's cross
 * axis is its BLOCK axis and a `column` container's is its INLINE one, and
 * `flex_container_axis_is_vertical` (core/layout/flex_item.h) composes §5.1's mapping with
 * css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings" to answer it. A caller holding a PHYSICAL axis
 * asks that entry rather than deciding for itself.
 * THE SENTENCE HERE USED TO SAY THIS COMPONENT REFUSED A `column` CONTAINER BY NAME, because §9.2 "Line
 * Length Determination"' last step and not §9.6 determines that container's BLOCK size and the two share no
 * step. Both halves of that reason stay exactly true and the CONCLUSION was wrong: §9.2 owns that
 * container's block size because the block size is its MAIN size, and what §9.4 owes it is its INLINE size —
 * which §9.4's steps 7 and 11 answer in the same words they answer a `row` container's height in. The
 * refusal was this component reading `margin-top`, `padding-top` and the top border width as the cross pair
 * unconditionally; those come from css-writing-modes-4 §7.2 "Dimensional Mapping" now, so the two entries
 * below differ in which AXIS they accept and the difference is about their CALLERS rather than about §9.4.
 *
 * WHAT IT DOES NOT DO, AND WHY EACH IS A DIFFERENT SECTION RATHER THAN A CASE. §9.4's step 9 distributes
 * spare cross space to the lines under `align-content: stretch` and its condition is "if the flex container
 * has a DEFINITE cross size" — which is exactly what this entry is called to produce and therefore exactly
 * what it does not have, so that step is unreachable from here rather than skipped. §9.6's steps 13, 14 and
 * 16 PLACE the items and its step 15 is the one this entry answers, which is not a size distinction but a
 * numbering one and is corrected here rather than left: the sentence read "steps 12, 13 and 15", and step 12
 * is §9.5 "Main-Axis Alignment"' while step 15 is the very step named at the top of this header as §9.6's
 * LAST-BUT-ONE. §9 "Flex Layout Algorithm" numbers its steps GLOBALLY — one `<ol>` runs from §9.1 "Initial
 * Setup"' step 1 to §9.6's step 16 — so §9.6 holds steps 13 through 16 and nothing earlier.
 * §9.4's STEP 11 IS THIS COMPONENT'S SECOND ENTRY AND NOT A THIRD STEP OF THIS ONE, which is worth a line
 * because the sentence here used to say step 11 was not this file's at all and that core/layout/used_value.c
 * refused an item's used cross size at both of its arms. It does not any more: `flex_cross_size_used_item_
 * cross` below answers it, and it lives here because its operands are this entry's — the item walk, the cross
 * edges, §8.3's resolved alignment and the line cross size — rather than because §9.4 is one section.
 *
 * NOTHING IS STORED, for core/layout/flex_line.h's reason: a layout is per-flow state, so a cached line cross
 * size is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. The cost is
 * stated here because a reader meets it before they meet the reason — `flex_line_used_main_size` resolves the
 * WHOLE line every time it is asked, and this walk reaches it ONCE PER ITEM, so a container of N items costs
 * N line resolutions and this walk adds a second N on top of them.
 * THE ROUTE IS DIRECT FOR ONE KIND OF ITEM AND TRANSITIVE FOR THE OTHER, WHICH IS WHAT DECIDES WHAT WOULD
 * RETIRE THAT COST — and the sentence here used to say only "asks", which reads as one direct call and sent a
 * next-diff clause the wrong way. For §4 "Flex Items"' ANONYMOUS item the walk asks that entry itself, with
 * the text node naming the item. For an ELEMENT item it does not ask at all: it asks
 * `used_value_block_level_content_px` for the item's CROSS size, whose `auto` arm is CSS 2.1 §10.6.3's line
 * boxes, whose available width is that item's own used WIDTH — and THAT read is what routes through
 * `uv_flex_item_main_size` into the line. So a LINE published from core/layout/flex_line.h would give this
 * walk the anonymous item's number and would NOT retire the N resolutions, because the element items' asks
 * are made inside a call this walk cannot hand a precomputed width to. What would retire them is the same
 * argument one level further out — a stated main size on `used_value_block_level_content_px`, so §9.4's step
 * 7 lays an element item out at the size §9.7 already gave it rather than re-deriving it — and that is a
 * different diff from publishing a line.
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
   `display`) and its CROSS axis must be its BLOCK axis — a `row` or `row-reverse` container by §5.1 "Flex Flow
   Direction: the flex-direction property"' mapping. Both are asserted here rather than at the call, for
   core/layout/flex_line.h's reason: this component reads §5, §8 and §9's properties, whose `Applies to:` lines
   are flex containers and flex items, and reading one off any other box answers the cascade's initial keyword.
   THE AXIS PRECONDITION IS THIS ENTRY'S AND NOT §9.4's, WHICH IS A STATEMENT ABOUT WHO ASKS: §9.6's step
   reaches here only where a CONTENT-BASED cross size is NEEDED, and a `column` container's cross size is a
   WIDTH whose "rules of the formatting context in which it participates" are CSS 2.1 §10.3.3 "Block-level,
   non-replaced elements in normal flow"' constraint equation — a number that needs no content at all. So the
   inline reading of step 8's walk would be a measurement with no consumer, and the section that a
   SHRINK-TO-FIT `column` container really asks for is §9.9.2 "Flex Container Intrinsic Cross Sizes". The
   refusal says so at the site.
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

/* css-flexbox-1 §9.4 "Cross Size Determination"' STEP 11 — THE USED CROSS SIZE OF ONE FLEX ITEM, as a
   CONTENT-box extent in CSS pixels on `container`'s CROSS axis, for an item whose cross size property
   COMPUTES TO `auto` or behaves as though it did. `item` is an ELEMENT and not a node, which is the whole of
   what separates this entry's subject from core/layout/flex_line.h's: the only caller is
   core/layout/used_value.c, which is asked a used value for an ELEMENT and is never asked one for §4 "Flex
   Items"' anonymous flex item, so a node-keyed signature here would be a second spelling with no caller —
   §4's box needs its USED cross size only when §9.6 "Cross-Axis Alignment" places it, which is unbuilt.
   THE `auto` PRECONDITION IS WHAT MAKES THIS ONE ARM RATHER THAN THE WHOLE STEP, and it is a statement about
   the CALLER rather than a narrowing of §9.4. Step 11's other arm — "Otherwise, the used cross size is the
   item's hypothetical cross size" — is, for an item with a DECLARED cross size, its own step 7 run over that
   declaration: "performing layout as if it were an in-flow block-level box", and for a block-level box with a
   DECLARED size that layout computes nothing ON EITHER AXIS. In the BLOCK dimension every rule CSS 2.1
   §10.6.3 "Block-level, non-replaced elements in normal flow when 'overflow' computes to 'visible'" states is
   conditioned on the property being `auto` ("If 'height' is 'auto', the height depends on whether the element
   has any block-level children"), so what is left is CSS 2.1 §10.5 "Content height: the 'height' property"
   making the declaration the height: "This property specifies the content height of boxes." In the INLINE
   dimension CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal flow"' equation solves for `width`
   only where `width` is `auto` — "If 'width' is not 'auto' and 'border-left-width' + … is larger than the
   width of the containing block, then any 'auto' values for 'margin-left' or 'margin-right' are … treated as
   zero", the over-constrained case, which adjusts a MARGIN and not the width — so §10.2 "Content width: the
   'width' property" makes the declaration the width. That is the
   number core/layout/used_value.c's declared arm already computes for every other box, so routing it here
   would be a second copy of css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"' conversion and
   of §10.2's percentage resolution. The declared arm therefore FALLS THROUGH there and this entry is not
   asked; what reaches here is the arm where the declaration decides nothing.
   IT IS THE CONTENT BOX, matching `used_value_block_level_content_px`'s convention and for its reason: §3.3's
   conversion belongs to the boundary that exposes a used value, and this entry's caller is that boundary.
   THE §10.7 CLAMP IS THE CALLER'S AND IS DELIBERATELY NOT HERE, which is worth a sentence because §8.3
   "Cross-axis Alignment: the align-items and align-self properties" states one and a reader will look for it.
   §8.3's stretch arm ends "while still respecting the constraints imposed by
   min-height/min-width/max-height/max-width", and CSS 2.1 §10.7's three steps are exactly that clamp: the
   caller returns this number as its tentative used value and `uv_sized` re-runs the pass with the limit
   substituted, which takes the DECLARED arm above and answers the limit. Writing the clamp here as well would
   be the second copy, and it would be the WRONG second copy — §10.7 re-runs the whole pass, so a limit that
   is itself a percentage is resolved against §10.1's basis rather than clamped as a raw number.
   THIS IS THEREFORE NOT THE EARLY RETURN core/layout/flex_line.h's main-axis twin takes, AND THE DIFFERENCE
   IS THE SECTION RATHER THAN A PREFERENCE: §9.7 "Resolving Flexible Lengths" applies its own clamp in its own
   words ("Clamp each non-frozen item's target main size by its used min and max main sizes"), so §10.4 must
   not run again over the main size; §9.4's step 11 states NO clamp of its own and §8.3 hands its one to
   CSS 2.1's properties by name, so §10.7 is where it belongs.
   `container` MUST BE `item`'s FLEX CONTAINER with the box tree agreeing, SINGLE-LINE and `horizontal-tb` —
   three of the four preconditions `flex_cross_size_content_based` states, asserted here for that entry's
   reason and because this one reaches it. THE FOURTH IS NOT SHARED: this entry answers WHICHEVER axis §5.1
   makes the cross one, so a `column` container's item asked for its WIDTH is answered here and a `row`
   container's item asked for its HEIGHT is too. `horizontal-tb` is still required, and for a reason that is
   not the same as the removed one: it is what makes the ONE bit this component threads name both a physical
   EDGE pair (css-writing-modes-4 §7.2 "Dimensional Mapping") and a sizing DIMENSION — step 7's block arm is
   CSS 2.1 §10.6 "Calculating heights and margins"' and its inline arm is css-sizing-3 §5.1 "Intrinsic
   Sizes"' pair, and in a vertical writing mode those two facts come apart. */
CssPx flex_cross_size_used_item_cross(lxb_dom_element_t *container, lxb_dom_element_t *item);

#endif
