/* css-flexbox-1 §4 "Flex Items" — WHICH BOXES A FLEX CONTAINER'S CHILD LIST IS — together with §5.1 "Flex Flow
 * Direction: the flex-direction property"'s MAIN AXIS and §5.2 "Flex Line Wrapping: the flex-wrap property"'s
 * LINE COUNT. Every algorithm in §9 "Flex Layout Algorithm" dispatches on those two before it reads anything
 * else, and every one of them enumerates this same list, so the three questions are one component.
 *
 * WHAT MAKES THIS A DIFFERENT ENUMERATION FROM core/layout/block_flow.h's, rather than a second copy of it.
 * §9.4.1 "Block formatting contexts"' stack and §9.4.2 "Inline formatting contexts"' line are two lists over
 * one child list, told apart by each child's LEVEL, and §9.2.1.1 "Anonymous block boxes" wraps a run of
 * inline-level children so the two never mix. A flex container has ONE list and no levels in it: §4's own
 * sentence is "Each in-flow child of a flex container becomes a flex item", with no reading of the child's
 * `display` beyond whether it generates a box at all — css-display-3 §2.7 "Automatic Box Type Transformations"
 * has already blockified it (core/css/css_computed_value.c), and §4 says the rest outright: "flex items
 * themselves are flex-level boxes, not block-level boxes". A FLOAT IS A FLEX ITEM, which is the sharpest
 * difference and the one a reader arriving from block_flow.h will get wrong: §4's own example marks
 * `<div id="item2" style="float: left;">float</div>` as a flex item and comments "floating is ignored", so
 * CSS 2.2 §9.5 "Floats"' shortened line box — the reason block_flow.h reports a float as its own kind — has no
 * counterpart here and nothing downstream of this list has to say anything about one.
 * THE ANONYMOUS BOX IS ALSO A DIFFERENT RULE OVER A DIFFERENT RUN. §9.2.1.1's run is a maximal sequence of
 * INLINE-LEVEL children and ends at a block-level one; §4's is a CHILD TEXT SEQUENCE and ends at any element
 * that generates a box, which is why "flex items do not split around blocks" in §4's example — a `<span>`
 * holding a `display: block` child is ONE flex item containing three blocks, where §9.2.1.1 would have split
 * the run in two. So the two delimiters are two sentences and a shared entry would answer one of them wrong.
 *
 * WHAT IS SHARED IS THE CHARACTERS, and it is shared rather than copied. §4's white-space rule is "if the
 * entire text sequences contains only document white space characters (i.e. characters that can be affected by
 * the white-space property) it is instead not rendered (just as if its text nodes were display:none)", and
 * which characters those are is the question core/layout/block_flow.h answers for CSS 2.2 §9.2.2.1 "Anonymous
 * inline boxes" over the same DOM — one derivation from css-text-3 §4 "White Space Processing Rules", with
 * U+000C FORM FEED deliberately outside it. Two copies of that set is one document with two ideas of what its
 * white space is.
 * §4's RULE IS UNCONDITIONAL AND §9.2.2.1's IS NOT, which is why only the character set is shared. §9.2.2.1
 * removes a white-space run that "would subsequently be collapsed away according to the 'white-space'
 * property", so a `white-space: pre` container keeps it; §4 states no such condition — the sentence is about
 * the CONTENT of the sequence and reads no property at all — so this component asks the characters and never
 * the declaration.
 *
 * NOTHING IS STORED, for core/layout/intrinsic_size.h's reason: a box tree is per-flow state, so a cached item
 * list is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_ITEM_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_ITEM_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display values"' TWO SPELLINGS, decided from a
   computed `display` and from nothing else — the same shape as core/layout/block_flow.h's block-container
   test and asked for the same reason: a consumer has a `display` in hand and needs to know which module owns
   the box before it can ask this component anything. §3's own sentences give both and say what differs — "A
   flex container establishes a new flex formatting context for its contents" and "Flex containers are not
   block containers" — so the pair is the OUTER display type and the inner one is the same for both.
   THE SECOND HALF USED TO READ "A flex container is not a block container", WHICH IS IN NO EDITION OF THIS
   STANDARD, and it is written out rather than silently corrected because the CLAIM it made was true and only
   its transcription was not: measured with a positive control ("flex formatting context", 4 occurrences) and
   a negative one, the singular spelling occurs ZERO times in the Editor's Draft AND zero times in the /TR
   snapshot, while §3's own plural sentence occurs in both. A quotation that is right about the fact and wrong
   about the words is the one shape no reader re-checks, because the sentence beneath it is correct. */
bool flex_item_display_is_flex_container(const char *display);

/* §5.1's MAIN AXIS, named by WHICH OF THE TWO WRITING-MODE AXES it is rather than by the keyword that chose
   it. §5.1 states the mapping and not a direction — "The flex container's main axis has the same orientation
   as the inline axis of the current writing mode" for `row`, and the same sentence with "the block axis of the
   current writing mode" for `column` — and `row-reverse` and `column-reverse` are each "Same as" their
   unreversed value "except the main-start and main-end directions are swapped", which is a fact about ORDER
   and never about which axis. So a consumer asking which axis this is must not be able to see the reversal:
   §9.9 "Intrinsic Sizes" would give two identical answers for it, and a caller that pattern-matched the
   keyword would eventually give two different ones. */
typedef enum {
    FLEX_MAIN_AXIS_INLINE = 0,  /* §5.1's `row` and `row-reverse` */
    FLEX_MAIN_AXIS_BLOCK        /* §5.1's `column` and `column-reverse` */
} FlexMainAxis;

FlexMainAxis flex_container_main_axis(lxb_dom_element_t *el);

/* §5.2's TWO ARMS, which §6 "Flex Lines" states in the vocabulary every consumer uses: "A single-line flex
   container (i.e. one with flex-wrap: nowrap) lays out all of its children in a single line, even if that
   would cause its contents to overflow", against a multi-line one, "one with flex-wrap: wrap or flex-wrap:
   wrap-reverse". TRUE is MULTI-LINE. `wrap-reverse` is "Same as wrap" for everything but the cross-start
   direction, which is the same kind of fact `row-reverse` carries and is likewise invisible here. */
bool flex_container_is_multi_line(lxb_dom_element_t *el);

/* §4's CLASSIFICATION OF ONE CHILD NODE. The three values are exhaustive over what a flex container's child
   list can hold, and this entry ANSWERS rather than refusing, for core/layout/block_flow.h's reason: what a
   caller DOES with a fact is the caller's own section, and a refusal written inside a shared classification
   reports ITS line and ITS remedy for callers it has never heard of. */
typedef enum {
    FLEX_ITEM_CHILD_NONE = 0,  /* generates no box, or §4.1 "Absolutely-Positioned Flex Children" takes it out
                                  of flow, or §4's white-space rule leaves the text sequence unrendered */
    FLEX_ITEM_CHILD_ELEMENT,   /* this element child IS one flex item */
    FLEX_ITEM_CHILD_TEXT       /* this text node is inside a child text sequence that §4 wraps in ONE anonymous
                                  block container flex item; `flex_item_text_sequence_end` delimits it */
} FlexItemChildKind;

FlexItemChildKind flex_item_child_kind(lxb_dom_element_t *container, lxb_dom_node_t *child);

/* §4's CHILD TEXT SEQUENCE, delimited: one past the LAST TEXT NODE of the maximal sequence containing `first`,
   in the (first, end) half-open form core/layout/line_box.h takes. `first` must itself answer
   FLEX_ITEM_CHILD_TEXT.
   WHAT COUNTS AS CONTIGUOUS IS css-display-3's AND NOT THIS FILE'S. §1 "Introduction" builds the sequence —
   "each contiguous sequence of sibling text nodes generates a text sequence containing their text contents" —
   and the same section is what makes a comment, a processing instruction and a doctype invisible between two
   of them: "for the purposes of CSS, all of these additional types of nodes are ignored, as if they didn't
   exist". §2.5 "Box Generation: the none and contents keywords" adds the other interruption that is not one,
   for `display: none`, in the words this delimiter needs: "anonymous box generation rules will ignore the
   elided elements entirely, as if they did not exist in the box tree".
   IT IS NOT COSMETIC AND THE TWO ANSWERS ARE DIFFERENT NUMBERS. §9.9.1.2 "Web-compatible Intrinsic Sizing
   Algorithm: Max-content Size and Min-content Single-line Size" SUMS the items' min-content contributions,
   while one item's own min-content size is the MAXIMUM over its segments — so splitting one sequence into two
   items turns a maximum into a sum and reports a container wider than it is. */
lxb_dom_node_t *flex_item_text_sequence_end(lxb_dom_element_t *container, lxb_dom_node_t *first);

/* §4.4 "Collapsed Items"' COLLAPSED FLEX ITEM — true where `item`'s computed `visibility` is `collapse`.
   §4.4's first sentence is the whole definition: "Specifying visibility: collapse on a flex item causes it to
   become a collapsed flex item", and css-display-3 §4 "Invisibility: the visibility property" is where the
   keyword is declared, in terms that name this section back — "collapse indicates that the box is collapsed,
   which can cause it to take up less space than otherwise in a formatting-context-specific way; see …
   collapsed flex items in flex layout".
   IT IS ASKED BY ONE OF §9.9's TWO SECTIONS AND MUST NOT BE ASKED BY THE OTHER, which is why the question is
   an entry here rather than a line at the walk that wants it. §9.9.1.2 and §9.9.1.3 both state their operand
   as the "non-collapsed" flex items; §9.9.2 "Flex Container Intrinsic Cross Sizes" states neither of its as
   that, because §4.4 gives a collapsed item a CROSS-axis effect the main axis does not have — "the collapsed
   flex item is removed from rendering entirely, but leaves behind a strut that keeps the flex line's
   cross-size stable". A reader who finds the cross walk not calling this has found that sentence and not an
   omission.
   AN ANONYMOUS ITEM IS NEVER ONE and cannot reach this entry: §4 makes that box unstyleable, so its
   `visibility` is the initial `visible`, and asking the CONTAINER — the only element in reach — would answer
   about the wrong box. That is why this takes an ELEMENT and not a node. */
bool flex_item_is_collapsed(lxb_dom_element_t *item);

/* css-flexbox-1 §7.2.1 "The flex-grow property"' AND §7.2.2 "The flex-shrink property"' FLEXIBILITY FACTOR of
   `item`. `name` is `flex-grow` or `flex-shrink`, and the answer is the `<number [0,∞]>` both `Value:` lines
   declare: their `Computed value:` lines are "specified number" and "specified value", so what the cascade
   holds is the number itself and this only reads it back.
   IT IS §4's QUESTION RATHER THAN ONE SECTION'S BECAUSE TWO ALGORITHMS ASK IT AND NEITHER OWNS IT.
   css-flexbox-1 §9.9.3 "Flex Item Intrinsic Size Contributions" reads only its SIGN, through two conditions
   stated of the ITEM and not of the factor — "if the item is not growable", "if the item is not shrinkable" —
   while §9.7 "Resolving Flexible Lengths" reads the NUMBER, both as the divisor of its distribution ratio and
   as the sum its less-than-one arm tests. A predicate named for one of those at this level would be one fact
   answering two questions, and a second reader of the same declaration would be the copy that drifts.
   ZERO IS THE DIVIDING VALUE AND IT IS §7.2.1's OWN: the property "sets the flex grow factor to the provided
   number", and §9.7 gives a zero factor no share of the free space at all — its step 2 freezes such an item
   outright. */
double flex_item_flexibility_factor(lxb_dom_element_t *item, const char *name);


#endif
