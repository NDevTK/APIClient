/* css-flexbox-1 §9.9 "Intrinsic Sizes" over a flex container's own items. See flex_intrinsic_size.h for why
   §5.1 decides which of §9.9's two sections answers an INLINE size, and for why neither of the two arms
   reachable through this entry needs a flex line. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_intrinsic_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/intrinsic_size.h"

/* css-writing-modes-4 §7.3 "Orthogonal Flows"' PERPENDICULAR CASE, REFUSED. §7.3 states the alternative in two
   bullets — "The two writing modes are parallel to each other" against "The two writing modes are
   perpendicular to each other" — and names the second: "When a box has a writing mode that is perpendicular to
   its containing block it is said to be in, or establish, an orthogonal flow."
   IT IS THIS COMPONENT'S QUESTION BECAUSE EVERY OPERAND IT SUMS IS AN *INLINE* SIZE. §9.9's two sections are
   stated in the container's MAIN and CROSS axes, and this entry turns them into an inline size through §5.1's
   mapping — which is a fact about the CONTAINER's writing mode. An item's own intrinsic inline size is a fact
   about the ITEM's, so where the two modes are perpendicular the number core/layout/intrinsic_size.h returns
   for the item lies along the container's BLOCK axis and is the wrong operand entirely — not a narrower
   answer, a measurement of the other dimension.
   EQUAL COMPUTED VALUES IS A SUFFICIENT TEST AND NOT THE SECTION'S OWN, and that is deliberate: §7.3's
   "parallel" holds for `vertical-rl` against `vertical-lr` as well, so this refuses a pair that is in fact
   parallel. A refusal is the safe direction — it crashes naming what to build instead of measuring the wrong
   axis — and widening it to §6.2 "Flow-relative Directions"' real parallelism test is part of building
   §9.2 "Line Length Determination"'s own orthogonal arm, whose note names this exact document: "This case
   occurs, for example, in an English document (horizontal writing mode) containing a column flex container
   containing a vertical Japanese (vertical writing mode) flex item." */
static void fis_require_parallel(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    char *cw = css_computed_value(container, "writing-mode");
    char *iw = css_computed_value(item, "writing-mode");
    bool same;
    char nbuf[160], mbuf[160];

    DCHECK(cw != NULL && iw != NULL,
           "the cascade produced no computed `writing-mode` — css-writing-modes-4 §3.2 \"Block Flow Direction: "
           "the writing-mode property\" gives it an initial value of `horizontal-tb`, so the last layer of the "
           "cascade always answers");
    same = strcmp(cw, iw) == 0;
    if (!same)
        DFAILF("%s (computed `writing-mode` `%s`) inside %s (`%s`): this FLEX ITEM's writing mode differs from "
               "its flex container's, so css-writing-modes-4 §7.3 \"Orthogonal Flows\" is what decides whether "
               "the item's own intrinsic INLINE size is an operand of css-flexbox-1 §9.9 \"Intrinsic Sizes\" at "
               "all. Where the two are perpendicular it is not: §9.9's sums are along the container's main and "
               "cross axes, and a perpendicular item's inline size lies along the container's BLOCK axis, so "
               "summing it reports one dimension of a box as the other. WHAT TO BUILD IS §7.3's OWN "
               "CLASSIFICATION, not a case here — \"The two writing modes are parallel to each other\" against "
               "\"The two writing modes are perpendicular to each other\", over css-writing-modes-4 §6.2 "
               "\"Flow-relative Directions\"' axes rather than over the keyword — and then §9.2 \"Line Length "
               "Determination\"'s step 3 arm that is written for exactly this box, whose flex base size is "
               "\"the item's max-content main size\" after laying it out \"using the rules for a box in an "
               "orthogonal flow\". core/layout/flow_position.c refuses the same pair one question earlier, for "
               "a box's PLACEMENT rather than its size, and names the same mappings",
               box_subject(item, mbuf, sizeof mbuf), iw, box_subject(container, nbuf, sizeof nbuf), cw);
    free(cw);
    free(iw);
}

/* ONE ITEM'S css-sizing-3 §5.2 "Intrinsic Contributions" OUTER CONTRIBUTION IN THE CONTAINER'S INLINE AXIS —
   which is the CROSS axis here, and that is the whole reason this is not css-flexbox-1 §9.9.3 "Flex Item
   Intrinsic Size Contributions". §9.9.3's title names contributions in general and every sentence in it names
   the MAIN axis — "The main-size min-content contribution of a flex item", "the main-size max-content
   contribution" — and its cap, floor and clamp are all over the flex base size and the "min/max MAIN size". A
   cross-axis contribution has none of that: §9.9.2 "Flex Container Intrinsic Cross Sizes" states its operands
   as the items' plain "min-content contribution/max-content contribution", which is css-sizing-3 §5.2's term
   and is answered by the same entry every block-level child of a block container is answered by. */
static IntrinsicInlineSizes fis_item_cross_contribution(lxb_dom_element_t *container, lxb_dom_node_t *child,
                                                        FlexItemChildKind kind, lxb_dom_node_t **next)
{
    /* THE KIND IS THE CALLER'S AND IS NOT RE-ASKED, which is not a saving but a correctness property: §4's
       classification of a TEXT node walks the whole sequence it is in, so asking twice is two readings of one
       child list, and two readings can only ever agree or be a bug nothing reports. One question, one answer,
       carried. */
    switch (kind) {
    case FLEX_ITEM_CHILD_TEXT: {
        /* §4's ANONYMOUS BLOCK CONTAINER FLEX ITEM. Its content is one CSS 2.2 §9.4.2 "Inline formatting
           contexts" over the sequence, which is core/layout/intrinsic_size.h's run entry, and its edges are
           zero because §4 makes the box unstyleable — the NULL that entry reads as an anonymous box. */
        lxb_dom_node_t *end = flex_item_text_sequence_end(container, child);
        IntrinsicInlineSizes run = intrinsic_inline_run_sizes(container, child, end);

        *next = end;
        return intrinsic_outer_contribution(NULL, run);
    }
    case FLEX_ITEM_CHILD_ELEMENT: {
        lxb_dom_element_t *item = lxb_dom_interface_element(child);

        fis_require_parallel(container, item);
        *next = child->next;
        return intrinsic_outer_contribution(item, intrinsic_inline_sizes(item));
    }
    case FLEX_ITEM_CHILD_NONE:
        break;
    /* NO `default` ARM, DELIBERATELY: `-Wswitch` is the forcing function here and a default would switch it
       off. A fourth `FlexItemChildKind` is a fourth kind of member in every flex container's item list, and
       §9.9's arms are all over a LIST — a member this walk cannot classify is a member it would silently drop,
       so the day one is added the answer must be a COMPILE failure at every caller and not a crash at one. */
    }
    {
        IntrinsicInlineSizes none;

        DFAIL("css-flexbox-1 §4's classification answered FLEX_ITEM_CHILD_NONE for a child this walk had "
              "already decided to measure. The caller skips that value before it asks for a contribution, so "
              "two readings of one node disagreed — which means the child list changed under the walk");
        /* THE RELEASE FALL-THROUGH CONTRIBUTES NOTHING RATHER THAN MEASURING SOMETHING, which is what
           FLEX_ITEM_CHILD_NONE means: §9.9.2's arms are a MAXIMUM, so a zero pair leaves the answer exactly
           where the other items put it and cannot invent a width for a box no section generates. */
        none.min_content = css_px(0.0);
        none.max_content = css_px(0.0);
        *next = child->next;
        return none;
    }
}

/* §9.9.2 "Flex Container Intrinsic Cross Sizes", for the container whose CROSS axis is its inline axis — a
   `column` or `column-reverse` container, by §5.1's mapping.
   BOTH REACHABLE ARMS ARE A MAXIMUM OVER THE ITEMS, and the section states them as two sentences that agree
   on the min-content size and part on the max-content one. Single-line: "The min-content/max-content cross
   size of a single-line flex container is the largest min-content contribution/max-content contribution
   (respectively) of its flex items." Multi-line column: "The min-content cross size is the largest min-content
   contribution among all of its flex items", and the section's own note says why that is not the same shape as
   the max-content one — "This heuristic effectively assumes a single flex line, in order to guarantee that the
   min-content size is smaller than the max-content size."
   A COLLAPSED ITEM IS *NOT* SKIPPED HERE and that is the section rather than an oversight. §9.9.1.2 and
   §9.9.1.3 both state their operand as the "non-collapsed" flex items and §9.9.2 states neither of its as
   that, because §4.4 "Collapsed Items" gives a collapsed item a cross-axis effect the main axis does not have:
   "the collapsed flex item is removed from rendering entirely, but leaves behind a strut that keeps the flex
   line's cross-size stable". So this walk asks css-display-3 §4 "Invisibility: the visibility property"
   nothing, and a walk over §9.9.1 will have to. */
static IntrinsicInlineSizes fis_cross_sizes(lxb_dom_element_t *el, bool multi_line)
{
    lxb_dom_node_t *c = lxb_dom_interface_node(el)->first_child;
    IntrinsicInlineSizes out;
    bool any = false;
    char nbuf[160];

    /* THE EMPTY CONTAINER IS A REAL ANSWER AND NOT A FLOOR: a maximum over no items is zero, and §6 "Flex
       Lines" says that is the one container with no line at all — "Every line contains at least one flex item,
       unless the flex container itself is completely empty." */
    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    while (c != NULL) {
        lxb_dom_node_t *next = c->next;
        FlexItemChildKind kind = flex_item_child_kind(el, c);
        IntrinsicInlineSizes one;

        if (kind == FLEX_ITEM_CHILD_NONE) { c = next; continue; }
        one = fis_item_cross_contribution(el, c, kind, &next);
        DCHECK(next != c,
               "css-flexbox-1 §4's item walk did not advance past the child it had just measured, so this walk "
               "would measure the same flex item for ever. A text sequence always contains at least the node "
               "it was delimited from, and an element item always advances by one sibling");
        out.min_content = css_px_max(out.min_content, one.min_content);
        out.max_content = css_px_max(out.max_content, one.max_content);
        any = true;
        c = next;
    }
    /* §9.9.2's MULTI-LINE COLUMN MAX-CONTENT ARM IS THE ONE THING IN THIS COMPONENT THAT NEEDS FLEX LINES, and
       it is refused HERE rather than at the dispatch so the min-content half — which is a maximum and IS
       built — still runs and so the crash names the container it is about. An EMPTY multi-line container is
       not refused: the sum of no line cross sizes is zero, which is the number a maximum over no items already
       produced, so §6's completely-empty container has one answer and not two. */
    if (multi_line && any)
        DFAILF("%s: css-flexbox-1 §9.9.2 \"Flex Container Intrinsic Cross Sizes\" gives a MULTI-LINE COLUMN "
               "flex container a max-content cross size this component cannot compute, and it is the only arm "
               "of §9.9 reachable from an intrinsic INLINE size that is stated over flex lines rather than "
               "over items: \"The max-content cross size is the sum of the flex line cross sizes resulting "
               "from sizing the flex container under a cross-axis max-content constraint, using the largest "
               "max-content cross-size contribution among the flex items as the available space in the cross "
               "axis for each of the flex items during layout.\" THE MIN-CONTENT HALF IS ALREADY THE ANSWER "
               "ABOVE — §9.9.2 makes it \"the largest min-content contribution among all of its flex items\", "
               "a maximum over items with no line in it — so what is missing is exactly the LINES: §9.3 "
               "\"Main Size Determination\"'s step 5 collects items into flex lines, and §9.4 \"Cross Size "
               "Determination\" is what gives each line the cross size this sum is over. BUILD THOSE TWO; "
               "core/layout/block_flow.c names §9.4 as the same absent capability for a flex container's own "
               "auto cross size, so one component answers both",
               box_subject(el, nbuf, sizeof nbuf));
    return out;
}

IntrinsicInlineSizes flex_intrinsic_inline_sizes(lxb_dom_element_t *el)
{
    IntrinsicInlineSizes out;
    char nbuf[160];

    DCHECK(el != NULL, "css-flexbox-1 §9.9's intrinsic sizes were asked for with no element");
    /* §5.1's MAPPING RUN BACKWARDS — see flex_intrinsic_size.h. Asked first and over the whole container,
       because §9.9.1 and §9.9.2 share no step: one sums along an axis and the other takes a maximum across
       the other one, so which section this box gets is a fact about its `flex-direction` and never a case
       discovered part-way through an accumulation. */
    if (flex_container_main_axis(el) == FLEX_MAIN_AXIS_BLOCK)
        return fis_cross_sizes(el, flex_container_is_multi_line(el));

    /* §9.9.1 IS UNBUILT, and the zero below is what a RELEASE build answers rather than a second algorithm
       standing behind the crash. It is deliberately not §9.9.2's number: that walk is a MAXIMUM across the
       cross axis and this box's inline size is a SUM along its main one, so handing it over would be a
       plausible width from the wrong section — where zero is visibly not a width at all. */
    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    DFAILF("%s: css-flexbox-1 §9.9.1 \"Flex Container Intrinsic Main Sizes\" is what this container's INLINE "
           "size is — §5.1 \"Flex Flow Direction: the flex-direction property\" makes a `row` container's main "
           "axis \"the same orientation as the inline axis of the current writing mode\" — and it is NOT "
           "BUILT. "
           "THE COMBINATION IS NOT THE MISSING PART AND NEEDS NO FLEX LINE, which is the first thing to know "
           "because the shape of §9.9 suggests otherwise. §9.9.1 says outright that \"an implementation is "
           "conformant to CSS Flexible Box Layout if it conforms to either the Ideal Algorithm or the "
           "Web-compatible Algorithm\", and §9.9.1.2 \"Web-compatible Intrinsic Sizing Algorithm: Max-content "
           "Size and Min-content Single-line Size\" is the whole of the second: \"take the sum of the "
           "max-content contributions of all the non-collapsed flex items in the flex container\", and the "
           "same sentence with min-content for a single-line container. §9.9.1.3 \"Multi-line Min-content "
           "Algorithm\" is the remaining case and is a MAXIMUM: \"the min-content main size is simply the "
           "largest min-content contribution of all the non-collapsed flex items in the flex container\". "
           "core/layout/flex_item.h already answers which items those are and §9.9.2's walk in this file is "
           "the same loop with the operation changed. "
           "WHAT IS MISSING IS ONE ITEM'S CONTRIBUTION. §9.9.3 \"Flex Item Intrinsic Size Contributions\" "
           "states it as \"the larger of its outer min-content size and outer preferred size if that is not "
           "an automatic size\", then: \"each contribution is capped by the item's flex base size if the item "
           "is not growable, floored by the item's flex base size if the item is not shrinkable, and then "
           "further clamped by the item's min/max main size.\" "
           "THE PROPERTY LAYER THIS CRASH USED TO ASK FOR IS BUILT, and it is named so the next reader does "
           "not build it twice: §7.2.1 \"The flex-grow property\", §7.2.2 \"The flex-shrink property\" and "
           "§7.2.3 \"The flex-basis property\" have `Computed value:` rules in core/css/css_computed_value.c "
           "(the first two through `css_computed_value`, the third through `css_computed_length`), §7.1 \"The "
           "flex Shorthand\" has a row of its own kind in core/css/css_shorthand.c, and the css-display-3 §4 "
           "\"Invisibility: the visibility property\" that §9.9.1.2's \"non-collapsed\" and §4.4 \"Collapsed "
           "Items\" read is modelled beside them. `css_computed_models` and `css_shorthand_complete_for` are "
           "the two predicates that say so, and a reader who finds either answering FALSE for one of those "
           "five has found this paragraph gone stale rather than a layer to write. "
           "WHAT REMAINS IS TWO TERMS AND NEITHER IS A PROPERTY. "
           "(1) THE OUTER PREFERRED SIZE, which is the SAME missing term core/layout/intrinsic_size.c already "
           "crashes on for a block container's child that declares its own inline size — CSS 2.1 §10.4 "
           "\"Minimum and maximum widths: 'min-width' and 'max-width'\"'s clamp and css-sizing-3 §3.3 \"Box "
           "Edges for Sizing: the box-sizing property\"'s conversion, over a declared `width` — so ONE diff "
           "retires both crashes and neither is finished without the other. §9.9.3's own \"if that is not an "
           "automatic size\" is the arm that makes an `auto` width contribute nothing here. "
           "(2) THE FLEX BASE SIZE, which is §9.2 \"Line Length Determination\"'s STEP 3 and is what the cap "
           "and the floor are taken against. Its ARM C is the one every call reaching this line is inside — "
           "\"If the used flex basis is content or depends on its available space, and the flex container is "
           "being sized under a min-content or max-content constraint\" — whose answer is the next sentence, "
           "\"The flex base size is the item's resulting main size\", so under it the cap and the floor are "
           "arithmetic over the number this component already measures. ARM A is the other one a real page "
           "reaches, \"If the item has a definite used flex basis, that's the flex base size\", and §7.2.3's "
           "`auto` is what routes between them, in a sentence whose OPENING CONDITION is the half a trimmed "
           "quotation would drop: \"WHEN SPECIFIED ON A FLEX ITEM, the auto keyword retrieves the value of "
           "the main size property as the used flex-basis. If that value is itself auto, then the used value "
           "is content.\" "
           "§9.2's aspect-ratio arm B and its orthogonal-flow arm D are not on this path — the second because "
           "this file refuses a perpendicular item one question earlier, by name",
           box_subject(el, nbuf, sizeof nbuf));
    return out;
}
