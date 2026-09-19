/* css-flexbox-1 §9.4 "Cross Size Determination"' steps 7 and 8 and §9.6 "Cross-Axis Alignment"' determination
   of the container's own used cross size. See flex_cross_size.h for why the three steps are one component and
   one call, and for why nothing here is stored. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/used_value.h"

static bool fx_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* THE ITEM'S CROSS-AXIS BORDER AND PADDING. It is the vertical pair because this component has already
   refused every container whose cross axis is not the vertical one — css-flexbox-1 §5.1 "Flex Flow Direction:
   the flex-direction property" makes a `row` container's cross axis its block axis and
   css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property" makes a `horizontal-tb` box's
   block axis the vertical one, and both are established at the entry below.
   `used_value_border_widths_px` WRITES CSS 2.1 §8.5's four sides in the top/right/bottom/left order every
   four-side rule in CSS states, so the cross pair is entries 0 and 2 — the mirror of
   core/layout/flex_line.c's `fl_main_border_padding`, which takes 1 and 3 for the same reason. */
static CssPx fx_cross_border_padding(lxb_dom_element_t *el)
{
    CssPx b[4];

    used_value_border_widths_px(el, b);
    return css_px_add(css_px_add(b[0], b[2]),
                      css_px_add(used_value_px(el, "padding-top"), used_value_px(el, "padding-bottom")));
}

/* THE ITEM'S CROSS-AXIS MARGINS. An `auto` one is NOT answered here and must not be: css-flexbox-1 §9.6
   "Cross-Axis Alignment"' first step gives it the difference between the item's outer cross size and the
   cross size of its flex line — "If its outer cross size (treating those auto margins as zero) is less than
   the cross size of its flex line, distribute the difference in those sizes equally to the auto margins" —
   which is a number the LINE's cross size is an operand of, so at step 8 it does not exist yet.
   core/layout/used_value.c already refuses that value by name at its vertical `auto`-margin arm, naming §9.6,
   so the refusal is ONE sentence in ONE place and this file does not restate it — the same arrangement
   core/layout/flex_line.c has for the main axis. */
static CssPx fx_cross_margins(lxb_dom_element_t *el)
{
    return css_px_add(used_value_px(el, "margin-top"), used_value_px(el, "margin-bottom"));
}

/* css-flexbox-1 §9.4's STEP 8.1 — the BASELINE GROUP — which this component does not compute and therefore
   must not silently leave out of step 8.3's maximum. §8.3 "Cross-axis Alignment: the align-items and
   align-self properties" declares both properties over the closed value list
   `flex-start | flex-end | center | baseline | stretch`, with `align-self` adding `auto` and an `Initial:` of
   `auto`, and its own prose gives the resolution: "align-items sets the default alignment for all of the flex
   container's items, including anonymous flex items. align-self allows this default alignment to be
   overridden for individual flex items."
   THE TEST IS A SUBSTRING AND THAT IS DELIBERATE RATHER THAN LOOSE. §8.3's Level 1 grammar admits the bare
   keyword only, and css-align-3 spells the same alignment `first baseline` and `last baseline`; every one of
   those is a BASELINE alignment and puts the item in §9.4's step 8.1 collection, so a test that admitted only
   the Level 1 spelling would answer NO for a page this engine's cascade may well accept. Refusing MORE is the
   safe direction here because the refusal is a crash and not a number. */
static void fx_require_no_baseline_alignment(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    char *self = css_computed_value(item, "align-self");
    char *items = css_computed_value(container, "align-items");
    bool baseline;
    char nbuf[160];

    DCHECK(self != NULL && items != NULL,
           "the cascade produced no computed value for `align-self` or `align-items` — both are in lexbor's "
           "registry with an initial value, so the last layer always answers");
    /* §8.3's `Initial: auto` on `align-self` is what makes the container's `align-items` the value that
       decides an item that declares nothing, which is the whole of that keyword's meaning. */
    baseline = strstr(self, "baseline") != NULL ||
               (strcmp(self, "auto") == 0 && strstr(items, "baseline") != NULL);
    free(self);
    free(items);
    if (!baseline) return;
    DFAILF("%s: this FLEX ITEM is BASELINE-ALIGNED in the cross axis (css-flexbox-1 §8.3 \"Cross-axis "
           "Alignment: the align-items and align-self properties\"), so css-flexbox-1 §9.4 \"Cross Size "
           "Determination\"' step 8 puts it in the FIRST of its two collections and not the second: "
           "\"Collect all the flex items whose inline-axis is parallel to the main-axis, whose align-self is "
           "baseline, and whose cross-axis margins are both non-auto. Find the largest of the distances "
           "between each item's baseline and its hypothetical outer cross-start edge, and the largest of the "
           "distances between each item's baseline and its hypothetical outer cross-end edge, and sum these "
           "two values.\" THAT SUM IS NOT THIS ITEM'S OUTER HYPOTHETICAL CROSS SIZE and it is not bounded by "
           "it either — two items whose baselines sit at different depths push the line TALLER than either of "
           "them is — so taking the walk below for it would report a line shorter than the page draws, which "
           "is the one answer a crash here is preferable to. "
           "WHAT IT NEEDS IS A BASELINE PER ITEM, MEASURED IN THE SAME FRAME AS THE OUTER CROSS EDGES, and "
           "core/layout/block_flow.h has both halves: `block_flow_first_line_box_baseline` and "
           "`block_flow_last_line_box_baseline` each report a distance from the box's TOP CONTENT EDGE and "
           "each answer FALSE for a box with no in-flow line box, which is css-flexbox-1 §8.5 \"Flex "
           "Container Baselines\"' own fallback condition. So what is missing is §8.5's choice between them "
           "and the two distances' conversion from the content edge to the item's OUTER cross edges, not a "
           "measurement",
           box_subject(item, nbuf, sizeof nbuf));
}

/* ONE ITEM'S OUTER HYPOTHETICAL CROSS SIZE — §9.4's step 7 plus css-sizing-3 §2.2 "Intrinsic Size
   Contributions"' outer step, which is what §9.4's step 8.2 takes the largest of: "Among all the items not
   collected by the previous step, find the largest outer hypothetical cross size."
   THE INNER HALF IS core/layout/used_value.h's AND NOT THIS FILE'S, for the reason that header states: step 7
   says "by performing layout as if it were an in-flow block-level box", so it is CSS 2.1 §10.6 and §10.7 run
   over a box read as §10.3.3's, and resolving the declaration here would be a second copy of both — including
   §10.7's percentage rule and its non-commutative min/max order. */
static CssPx fx_outer_hypothetical_cross(lxb_dom_element_t *item)
{
    CssPx inner = used_value_block_level_content_px(item, true);

    DCHECK(inner.px >= 0.0,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 7 produced a NEGATIVE hypothetical cross "
           "size. Every arm it can take floors at zero — css-sizing-3 §3.3 \"Box Edges for Sizing: the "
           "box-sizing property\" says of the `border-box` conversion that \"the content box width and height "
           "are calculated by subtracting the border and padding in the corresponding axis from the specified "
           "<length-percentage>, and flooring the result at zero (as the inner size of a box cannot be "
           "negative)\", and CSS 2.1 §10.6.3's walk is a running sum of used heights — so a negative here is "
           "a derivation that lost an operand rather than a page. "
           "THE WORDING IS THE STANDARD'S AND NOT THIS TREE'S, WHICH IS WORTH A CLAUSE BECAUSE THE TREE HAS "
           "ANOTHER: five sites in core/layout render this same rule as \"as the content width and height "
           "cannot be negative, this computation is floored at zero\", in quotation marks, and that sentence "
           "occurs in NO section of css-sizing-3 — not in the committed corpus and not in the live Editor's "
           "Draft. It is a paraphrase wearing a quotation's clothes, the five agree with each other and "
           "disagree with the document, and the citation auditor cannot report it because this tree's own "
           "prose holds the words. A reader copying a quotation from a sibling site here is copying that one");
    return css_px_add(inner, css_px_add(fx_cross_border_padding(item), fx_cross_margins(item)));
}

CssPx flex_cross_size_content_based(lxb_dom_element_t *container)
{
    lxb_dom_node_t *c;
    CssPx largest = css_px(0.0);
    char nbuf[160], wbuf[64];
    char *display;
    bool is_container;

    DCHECK(container != NULL,
           "css-flexbox-1 §9.6 \"Cross-Axis Alignment\"' content-based cross size was asked for with no "
           "element");
    display = css_computed_value(container, "display");
    DCHECK(display != NULL, "the cascade produced no computed `display` for a box a layout is walking");
    is_container = flex_item_display_is_flex_container(display);
    free(display);
    DCHECK(is_container,
           "css-flexbox-1 §9.6 \"Cross-Axis Alignment\"' content-based cross size was asked of a box that is "
           "not a FLEX CONTAINER. §3 \"Flex Containers: the flex and inline-flex display values\" is the two "
           "spellings and core/layout/flex_item.h decides them; every property this component reads has a "
           "flex container or a flex item on its `Applies to:` line, so on any other box the walk below would "
           "be reading the cascade's initial keywords and reporting them as a layout");
    /* css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property" is what makes the CROSS axis
       of a `row` container the VERTICAL one, and it is asked FIRST and refused rather than assumed — the same
       question core/layout/block_flow.c asks one call up and core/layout/flex_line.c asks for the main axis,
       each naming css-writing-modes-4 §7.4 "Flow-Relative Mappings" as the one absent capability all three
       are waiting on. It is re-asked here rather than taken on trust from the caller because this header
       states it as this component's own precondition and every physical edge below depends on it. */
    if (!fx_computed_is(container, "writing-mode", "horizontal-tb"))
        DFAILF("%s, computed `writing-mode` `%s`: this FLEX CONTAINER's block axis is not the vertical one, "
               "so css-flexbox-1 §5.1 \"Flex Flow Direction: the flex-direction property\"' mapping does not "
               "make `margin-top`, `padding-top` and the top border width the CROSS-axis edges this walk adds "
               "— and every operand of §9.4 \"Cross Size Determination\"' step 8 below is one of those. BUILD "
               "css-writing-modes-4 §7.4 \"Flow-Relative Mappings\", which core/layout/block_flow.c and "
               "core/layout/flex_line.c name as the same absent capability, and then this component reads the "
               "flow-relative edges instead of assuming the physical ones",
               box_subject(container, nbuf, sizeof nbuf),
               box_subject_computed(container, "writing-mode", wbuf, sizeof wbuf));
    if (flex_container_main_axis(container) != FLEX_MAIN_AXIS_INLINE)
        DFAILF("%s: this FLEX CONTAINER's main axis is its BLOCK axis (css-flexbox-1 §5.1 \"Flex Flow "
               "Direction: the flex-direction property\": a `column` container's main axis \"has the same "
               "orientation as the block axis of the current writing mode\"), so its CROSS axis is the INLINE "
               "one and the number this entry answers is a WIDTH rather than the height it was reached for. "
               "§9.6 \"Cross-Axis Alignment\" is not what determines a `column` container's block size: §9.2 "
               "\"Line Length Determination\"' last step is, in one sentence — \"Determine the main size of "
               "the flex container using the rules of the formatting context in which it participates. The "
               "automatic block size of a block-level flex container is its max-content size.\" "
               "core/layout/block_flow.c holds that arm and names css-flexbox-1 §9.9.1 \"Flex Container "
               "Intrinsic Main Sizes\" IN THE BLOCK AXIS as what it needs, which shares no step with this "
               "component",
               box_subject(container, nbuf, sizeof nbuf));
    /* §5.2 "Flex Line Wrapping: the flex-wrap property"' other arm, refused for TWO reasons that would each
       be enough on its own — which is why it is refused here as well as at core/layout/flex_line.c, where
       only the first of them applies. */
    if (flex_container_is_multi_line(container))
        DFAILF("%s: this FLEX CONTAINER is MULTI-LINE (css-flexbox-1 §5.2 \"Flex Line Wrapping: the flex-wrap "
               "property\"; §6 \"Flex Lines\" makes `wrap` and `wrap-reverse` one case against `nowrap`), so "
               "§9.6 \"Cross-Axis Alignment\"' \"use the sum of the flex lines' cross sizes\" is a sum over "
               "SEVERAL lines and the walk below computes ONE. TWO THINGS ARE MISSING AND ONLY THE FIRST OF "
               "THEM IS core/layout/flex_line.c's: §9.3 \"Main Size Determination\"' step 5 must BREAK the "
               "items into lines before any of them has a used main size, and §9.4 \"Cross Size "
               "Determination\"' step 8 must then be run PER LINE, because its own step 8.3 takes the largest "
               "outer hypothetical cross size among the items OF ONE LINE — a single maximum over every item "
               "in the container would report the tallest item's height as the height of every line. §9.4's "
               "step 9 becomes reachable at the same moment and is unreachable now: \"If the flex container "
               "has a definite cross size, align-content is stretch, and the sum of the flex lines' cross "
               "sizes is less than the flex container's inner cross size, increase the cross size of each "
               "flex line by equal amounts\" — a condition on a DEFINITE cross size, which is the one thing "
               "this entry is called because the container does not have",
               box_subject(container, nbuf, sizeof nbuf));
    /* §9.4's step 8 HAS AN ARM FOR THE DEFINITE CASE — "If the flex container is single-line and has a
       definite cross size, the cross size of the flex line is the flex container's inner cross size" — AND
       THERE IS NO PRECONDITION HERE THAT RULES IT OUT, which is worth writing down because the obvious
       assert is wrong and was written and removed rather than merely not written. `used_value_height_behaves_
       as_auto` looks like the predicate that says this entry is the right one, and it is not: this entry's
       caller is core/layout/block_flow.h's AUTOMATIC block size, which core/layout/used_value.c asks for at
       TWO places, and only one of them is the `auto` arm. The other is css-sizing-3 §3.2 "Sizing Values: the
       <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content values"'
       `min-content`/`max-content` keyword on a `min-height` or a `max-height`, whose own sentence is "for a
       box's block size, unless otherwise specified, this is equivalent to its automatic size" — so
       `min-height: max-content` on a flex container with a DECLARED `height` reaches here with a definite
       cross size in hand and wants this walk anyway, because what it asked for is the AUTOMATIC size and the
       automatic size is the one computed with the cross axis treated as indefinite. An assert on the
       declaration would have fired on that page and named a defect that is not there.
       WHAT DOES DECIDE IT IS §9.6's OWN CONDITIONAL and it is the caller's to read: "If a content-based cross
       size is needed, use the sum of the flex lines' cross sizes." Both callers need one. */
    for (c = lxb_dom_interface_node(container)->first_child; c != NULL; ) {
        lxb_dom_node_t *next = c->next;
        FlexItemChildKind kind = flex_item_child_kind(container, c);

        if (kind == FLEX_ITEM_CHILD_NONE) { c = next; continue; }
        switch (kind) {
        case FLEX_ITEM_CHILD_TEXT:
            next = flex_item_text_sequence_end(container, c);
            DFAILF("%s: this is css-flexbox-1 §4 \"Flex Items\"' ANONYMOUS FLEX ITEM — the one box a flex "
                   "container's item list holds that is not an element — and §9.4 \"Cross Size "
                   "Determination\"' step 8 needs its OUTER HYPOTHETICAL CROSS SIZE like every other item's. "
                   "TWO DIFFERENT THINGS ARE MISSING AND NEITHER IS THE MEASUREMENT ITSELF. (1) ITS USED MAIN "
                   "SIZE CANNOT BE ASKED FOR: core/layout/flex_line.h's entry is keyed on an ELEMENT item and "
                   "asserts that item is a child of the container, and §4 makes this box unstyleable, so the "
                   "line it is on resolves it internally and publishes it for nobody. (2) ITS CROSS SIZE IS A "
                   "RUN OF LINE BOXES AT THAT WIDTH, and core/layout/line_box.h's `line_box_content_height` "
                   "takes the run but derives its available width from `used_value_content_px` OF THE ELEMENT "
                   "IT IS GIVEN — which here is the CONTAINER, so it would lay the run out at the container's "
                   "inner main size rather than at this item's used main size, and the two are equal only "
                   "when §9.7 \"Resolving Flexible Lengths\" happened to flex it to the full line. BUILD (1) "
                   "FIRST: an entry on core/layout/flex_line.h that reports the whole LINE — one used main "
                   "size per item in the order §9.3 \"Main Size Determination\"' step 5 collected them, the "
                   "anonymous ones included — which this walk then reads instead of asking per item, and "
                   "which also retires the N-line-resolutions-per-container cost flex_cross_size.h states. "
                   "THEN (2): an available width parameter on the line-box height, which is the same operand "
                   "§9.4's step 11 will need when it recalculates an item's cross size \"using the flex "
                   "line's cross size … as the available space\"",
                   box_subject_node(c, nbuf, sizeof nbuf));
            break;
        case FLEX_ITEM_CHILD_ELEMENT: {
            lxb_dom_element_t *item = lxb_dom_interface_element(c);

            next = c->next;
            /* §9.4's step 10 is this component's own and it is unbuilt, which is why the refusal is here and
               not a second copy of core/layout/flex_line.c's: that file refuses a collapsed item because the
               used main size it would answer is the FIRST round's, and this one refuses it because step 8
               owes the line a STRUT SIZE that only the second round produces. */
            if (flex_item_is_collapsed(item))
                DFAILF("%s: this flex item's computed `visibility` is `collapse`, so css-flexbox-1 §4.4 "
                       "\"Collapsed Items\" makes it a COLLAPSED FLEX ITEM and css-flexbox-1 §9.4 \"Cross "
                       "Size Determination\"' step 10 owns it — \"note the cross size of the line they're in "
                       "as the item's strut size, and restart layout from the beginning\". THE STRUT IS WHY "
                       "THIS IS STEP 8's PROBLEM AND NOT ONLY §9.3's: step 10's second round ignores the "
                       "collapsed items \"entirely (as if they were display:none) except that after "
                       "calculating the cross size of the lines, if any line's cross size is less than the "
                       "largest strut size among all the collapsed items in the line, set its cross size to "
                       "that strut size\" — so the line's cross size is a FLOOR taken from a number this "
                       "walk's own first round produces, and answering from one round drops it. BUILD STEP "
                       "10's TWO ROUNDS HERE: this walk is the first of them, what it owes is to record each "
                       "collapsed item's line cross size and run itself again with those items at zero main "
                       "size and out of the maximum below",
                       box_subject(item, nbuf, sizeof nbuf));
            fx_require_no_baseline_alignment(container, item);
            largest = css_px_max(largest, fx_outer_hypothetical_cross(item));
            break;
        }
        case FLEX_ITEM_CHILD_NONE:
            /* UNREACHABLE BY THE GUARD ABOVE, and asserted rather than left to fall through: the tail counts
               a member, and an item this walk cannot classify is one whose cross size would silently not be
               in the maximum — which is the line reported SHORTER than the page draws. The arm exists
               because `-Wswitch` is the forcing function this walk relies on, exactly as
               core/layout/flex_line.c's does. */
            DFAIL("css-flexbox-1 §4 \"Flex Items\"' classification answered FLEX_ITEM_CHILD_NONE for a child "
                  "this walk had already decided to measure. The loop skips that value before it reaches this "
                  "switch, so two readings of one child list disagreed — which means the child list changed "
                  "under the walk");
            break;
        /* NO `default` ARM, DELIBERATELY, for core/layout/flex_line.c's reason: `-Wswitch` is the forcing
           function, and a fourth `FlexItemChildKind` is a fourth kind of member of every flex container's
           item list — §9.4's step 8 is a MAXIMUM over a line, so a member this walk cannot classify is one it
           would silently leave out of it. */
        }
        DCHECK(next != c,
               "css-flexbox-1 §4 \"Flex Items\"' item walk did not advance past the child it had just "
               "measured, so this walk would add the same flex item to the maximum for ever. A text sequence "
               "always contains at least the node it was delimited from, and an element item always advances "
               "by one sibling");
        c = next;
    }
    /* §9.4's STEP 8.3 — "The used cross size of the flex line is the largest of the numbers found in the
       previous two steps and zero" — where the first of those two steps is the baseline collection the walk
       above refuses and the second is the maximum it accumulated. The zero is the section's own third
       operand and is therefore also the answer for a container with no items at all, which §6 "Flex Lines"
       admits by name: "Every line contains at least one flex item, unless the flex container itself is
       completely empty."
       §9.6's SUM IS THIS ONE NUMBER because §5.2's other arm is refused above, and it is stated rather than
       written as a loop of one so that a reader adding line breaking finds the sum missing rather than
       finding a loop that happens to run once.
       THERE IS NO ASSERT OVER THE RESULT AND THAT IS A DECISION. The obvious one — that the answer is
       non-negative — has no value of this program that could fail it: `largest` starts at §9.4's step 8.3 zero
       and the only thing done to it is a maximum WITH that zero, so the two sides of such a check cannot
       disagree and it would certify the walk without examining it. The invariant that is NOT vacuous is one
       step down and is asserted there, over the per-item hypothetical cross size this walk did not floor. */
    return largest;
}
