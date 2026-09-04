/* css-sizing-3 §5.1 "Intrinsic Sizes" — A BOX'S MIN-CONTENT AND MAX-CONTENT INLINE SIZES, which are the two
 * terms CSS 2.2 §10.3.5 "Floating, non-replaced elements" calls the preferred minimum width and the preferred
 * width and sends its `auto` widths to.
 *
 * THE MODERN DEFINITION IS CIRCULAR BACK TO CSS 2 AND THAT IS THE FIRST THING TO KNOW ABOUT IT, because a
 * reader who takes css-sizing-3 for the algorithm finds none. §5.1 states both sizes in terms of the very
 * layout that consumes them — "the min-content size of a box in each axis is the size it would have IF IT WAS A
 * FLOAT given an auto preferred size in that axis … and if its containing block was ZERO-sized in that axis",
 * the max-content size the same sentence with "INFINITELY-sized" — and then says outright: "this specification
 * does not define how to determine the sizes of floats. Please refer to [CSS2]." So §5.1 is a DEFINITION and
 * CSS 2.2 §10.3.5 is the operation, in its own words: "calculate the preferred width by FORMATTING THE CONTENT
 * WITHOUT BREAKING LINES other than where explicit line breaks occur, and also calculate the preferred minimum
 * width, e.g., by TRYING ALL POSSIBLE LINE BREAKS."
 * css-sizing-3 §2.1 "Auto Box Sizes" is what pins the two vocabularies to each other, by name — "this is called
 * the 'preferred width' in CSS2.1§10.3.5" of the max-content inline size, "this is called the 'preferred
 * minimum width' in CSS2.1§10.3.5" of the min-content inline size — and it also states the operation in the
 * form this engine implements: max-content is the size "if NONE of the soft wrap opportunities within the box
 * were taken", min-content the size "if ALL soft wrap opportunities within the box were taken". Two spellings,
 * one walk, and citing only one of them would leave the other reader unable to check the code.
 *
 * WHAT THIS COMPONENT IS AND WHAT core/layout/text_run.h IS, because they are one measurement split at the
 * place the two problems actually part. text_run.h owns the CHARACTERS — css-text-3 §4.1's white space
 * processing and §5's soft wrap opportunities over [UAX14] — and knows nothing about boxes. This owns the BOX
 * TREE: which of a box's children contribute, and how css-sizing-3 §5.2 "Intrinsic Contributions" combines
 * them. The split is not tidiness: core/layout/line_box.c needs the first without the second (a line box's
 * height is a function of where the run breaks, not of an intrinsic size) and so does CSSOM VIEW §2's scrolling
 * area, so folding the characters in here would make those two consumers depend on a sizing algorithm neither
 * of them asks about.
 *
 * THE ANSWER IS THE CONTENT BOX'S INLINE SIZE, which is the box CSS 2.2 knows and the one §10.3.5's formula is
 * arithmetic in: every term of `min(max(preferred minimum width, available width), preferred width)` is a
 * content width, and the result is the used `width`. css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing
 * property"'s conversion to the border box is applied to the RESULT by the caller, exactly as core/layout/
 * used_value.c applies it to §10.3.3's constraint equation — so this component never asks about `box-sizing`
 * and a caller that forgot to is wrong in one place rather than in two.
 *
 * WHICH OF CSS 2.2 §9.4's TWO FORMATTING CONTEXTS THIS BOX ESTABLISHES IS ASKED ONCE, OVER THE WHOLE CHILD
 * LIST, BEFORE EITHER ALGORITHM RUNS. §9.2.1 "Block-level elements and block boxes" states the alternative —
 * "A block container box either contains only block-level boxes or establishes an inline formatting context
 * and thus contains only inline-level boxes" — and §9.4.2 states the condition, "a block container box that
 * contains no block-level boxes". The two algorithms share no step: §9.4.2's flows the children ALONG a line
 * and sums, §9.4.1's stacks them DOWN a column so css-sizing-3 §5.2's contribution is the MAXIMUM over them.
 * A walk that discovered the difference part-way through a measurement had already begun summing a run before
 * it met the child that made the sum the wrong operation, which is why the question is a dispatch and not a
 * case. It is core/layout/block_flow.h's exported predicate rather than a second copy of §9.4.2's condition,
 * because that component's own stack chooses between the same two sections over the same list.
 * BOTH ARMS ARE HERE. §9.4.2's is the whole of the run measurement below. §9.4.1's takes the MAXIMUM over the
 * box list §9.2.1.1 "Anonymous block boxes" forces the container to have — its block-level children plus one
 * anonymous block box per maximal run of inline-level children — with each operand css-sizing-3 §2.2
 * "Intrinsic Size Contributions"' OUTER size, which is where that phrase is defined rather than at §5.2. The
 * enumeration is core/layout/block_flow.h's, both halves of it: one classification and one run delimitation,
 * shared with the walk that PLACES the same boxes so that two answers to "is this child block-level" cannot
 * exist. `block_flow_anonymous_boxes` is deliberately NOT what it asks — that entry PLACES, and a placement
 * needs the container's used width, which for the only two boxes that ask for an intrinsic size is a
 * shrink-to-fit over this very entry.
 * WHAT IT DOES NOT DO IS APPLY A CHILD'S OWN DECLARED INLINE SIZE. §5.2's contribution is the size of a
 * hypothetical float CONTAINING the child, so a child with `width: 500px` contributes 500px and not what its
 * text measures; css-sizing-3 §5.2.1 substitutes a cyclic PERCENTAGE away (to `auto` / `none`, which is what
 * this walk already computes) and leaves a LENGTH standing. That case crashes rather than answering, because
 * the measured number would be a WRONG width for a real document rather than a narrower one — CSS 2.1 §10.4's
 * clamp and css-sizing-3 §3.3's `box-sizing` conversion are what the term still needs.
 *
 * NOTHING IS STORED, for core/layout/used_value.h's reason: a layout is per-flow state, so a cached intrinsic
 * size is shared state solver/dom_cow.h does not swap and a stale one is another flow's document. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_SIZE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_INTRINSIC_SIZE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* CSS 2.2 §9.4.2 "Inline formatting contexts"' OUTER SIZE at ONE SIDE of an INLINE BOX, as USED VALUES, in CSS
   pixels — the trailing side for `trailing` true. §9.4.2 is what puts it on the line ("horizontal margins,
   borders, and padding are respected between these boxes") and css-text-3 §5.5 "Line Breaking Details" is what
   says WHERE: "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
   flow", so the two edges sit at the box's boundaries and not at every break inside it. core/layout/line_box.c
   hands it to core/layout/text_run.h as an EDGE item at a POSITION, which is why it is never added to a total.
   IT IS THE USED-VALUE HALF OF ONE FACT WHOSE OTHER HALF IS THIS COMPONENT'S OWN, which is why a line box's
   operand is declared in the intrinsic-size header instead of in core/layout/used_value.h. The FACT is which
   three properties one side of an inline box is — a margin, a padding and a border width — and css-sizing-3
   §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes" asks TWO QUESTIONS of it that differ on exactly
   one input. A percentage on any of the six resolves against the containing block's width; while an INTRINSIC
   contribution is being measured that width is the number the measurement produces, and §5.2.1 states the
   resolution for that case — "For the min size properties, as well as for margins and paddings (and gutters),
   a cyclic percentage is resolved against zero for determining intrinsic size contributions." Its very next
   paragraph states the other, "when calculating the used sizes and positions of the containing block's
   contents": "Otherwise, the percentage is resolved against the containing block's size." That second one is a
   LINE's case, because a line box is filled at a width its containing block has already determined, so the
   percentage is not cyclic there at all and resolves normally.
   SO THERE ARE TWO ENTRIES OVER ONE PROPERTY TRIPLE, never two triples. One predicate answering both questions
   answered BOTH with the stricter one's refusal, and substituting §5.2.1's zero into that shared entry — which
   is what its own crash message instructed — would have silently mis-laid-out every percentage-margined inline
   box on a real line. THE INTRINSIC ANSWER IS NOT DECLARED HERE AND THAT IS THE STRUCTURAL HALF OF THE SPLIT:
   it is `static` in intrinsic_size.c because it has no caller outside that walk, and being unreachable is what
   stops it being asked at a used-value site, where its zero would be a wrong number no assert could see. */
CssPx used_inline_box_edge_px(lxb_dom_element_t *el, bool trailing);

/* css-sizing-3 §5.1's PAIR. They are returned together and never separately because §2.1 defines them over the
   same content with only the soft wrap opportunities differing, so one walk produces both — and because the
   one relation between them (`min_content <= max_content`) is a statement about the pair that a caller holding
   one of them could not check. */
typedef struct {
    CssPx min_content;   /* §2.1's min-content inline size — CSS 2.2 §10.3.5's "preferred minimum width" */
    CssPx max_content;   /* §2.1's max-content inline size — CSS 2.2 §10.3.5's "preferred width" */
} IntrinsicInlineSizes;

/* THE INTRINSIC INLINE SIZES OF `el`'s BOX, as CONTENT-box widths in CSS pixels.
   THE CALLER HAS NOT ESTABLISHED ANYTHING and does not need to: every box type this walk cannot size crashes
   here naming its own section, which is what lets a consumer ask the question without first re-deriving the
   classification core/layout/block_flow.c owns. */
IntrinsicInlineSizes intrinsic_inline_sizes(lxb_dom_element_t *el);

/* CSS 2.2 §9.4.2 "Inline formatting contexts"' CONTEXT OVER ONE RUN of `el`'s children — [first, end), in the
   half-open form core/layout/line_box.h takes, with a NULL `end` running to the end of the child list — as
   CONTENT-box inline sizes in CSS pixels. The run's boxes are styled by `el`, which is what an ANONYMOUS box
   around them inherits from.
   IT IS EXPORTED BECAUSE TWO SECTIONS GENERATE AN ANONYMOUS BOX AROUND A RUN OF ONE BOX'S CHILDREN AND BOTH
   NEED THE SAME MEASUREMENT. CSS 2.2 §9.2.1.1 "Anonymous block boxes" wraps a maximal run of INLINE-LEVEL
   children and is this file's own §9.4.1 arm; css-flexbox-1 §4 "Flex Items" wraps a CHILD TEXT SEQUENCE in an
   anonymous block container flex item. The two RUNS are delimited by different sentences and the two
   delimiters are therefore two functions, but what is inside either of them is one inline formatting context
   and one walk — a second copy of it would be one document with two ideas of how wide its text is.
   THE RUN'S OWN EDGES ARE NOT ADDED, and for both callers that is a derivation rather than an omission: an
   anonymous box takes its non-inherited properties' initial values (§9.2.1.1: "the margins will be 0"), and
   css-flexbox-1 §4 says the same of its own ("the anonymous item's box is unstyleable"), so css-sizing-3 §2.2
   "Intrinsic Size Contributions"' outer size of such a box is its inner size unchanged. */
IntrinsicInlineSizes intrinsic_inline_run_sizes(lxb_dom_element_t *el, lxb_dom_node_t *first,
                                                lxb_dom_node_t *end);

/* THE OUTER SIZE OF `el`'s BOX over INNER sizes the CALLER computed. css-sizing-3 §2 "Terminology" is where
   the term is defined — an outer size is "the margin-box size of a box", against an inner size, "the
   content-box size of a box" — and §2.2 "Intrinsic Size Contributions" is what makes it the operand of every
   contribution, with its own "for this purpose auto margins are treated as zero". css-sizing-3 §5.2.1
   "Intrinsic Contributions of Percentage-Sized Boxes"' cyclic percentage is resolved against zero on each of
   the six edge properties.
   THE INNER SIZES ARE AN ARGUMENT AND NOT `intrinsic_inline_sizes(el)`, which is the whole reason this entry
   exists rather than a one-argument one. css-flexbox-1 §9.9.3 "Flex Item Intrinsic Size Contributions" caps,
   floors and clamps a flex item's MAIN size before the outer conversion is applied, so the number that gets
   the edges added is not the one this component measured — and the two readings are the same number only
   because every operand of §9.9.3 is one item's size in one axis and every one of them takes the SAME edge
   sum, so a maximum, a cap, a floor and a clamp all commute with adding it.
   §2.2's FLOOR IS APPLIED HERE and is why this returns the PAIR rather than one number: "if the ideal
   max-content contribution would be smaller than the min-content contribution (e.g. due to the use of
   negative margins), the effective max-content contribution is floored by the min-content contribution." IT
   IS ALSO WHY NEITHER INNER SIZE IS ASSERTED — an inverted pair is the case that sentence is written for, so
   refusing one would crash on a page CSS 2.1 §8.3 "Margin properties" permits.
   `el` IS NULL FOR AN ANONYMOUS BOX, whose edge sum is ZERO — and that is a DERIVATION rather than a
   convenience, stated identically by the two sections that generate one: CSS 2.2 §9.2.1.1 "Anonymous block
   boxes" ("the properties of anonymous boxes are inherited from the enclosing non-anonymous box …
   Non-inherited properties have their initial value … the margins will be 0") and css-flexbox-1 §4 "Flex
   Items" ("the anonymous item's box is unstyleable, since there is no element to assign style rules to"). So
   its margin box, its border box and its content box are one rectangle. Passing NULL rather than reaching for
   an element that does not exist is what stops a caller substituting the CONTAINER's edges, which are a
   different box's and would be added twice.
   THE EDGE ITSELF IS NOT EXPORTED AND MUST NOT BE. §5.2.1 gives one property triple TWO answers — a cyclic
   percentage resolved against zero for a contribution, and against the containing block's size "when
   calculating the used sizes and positions of the containing block's contents" — and the zero one is a WRONG
   number at every used-value site with no assert that could see it. Being reachable only through a function
   that has already committed to being a contribution is what keeps the two apart. */
IntrinsicInlineSizes intrinsic_outer_contribution(lxb_dom_element_t *el, IntrinsicInlineSizes inner);

#endif
