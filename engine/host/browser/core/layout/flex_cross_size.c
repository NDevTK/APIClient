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
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/line_box.h"
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

/* §8.3's RESOLVED CROSS-AXIS ALIGNMENT FOR ONE ITEM, AS THE KEYWORD ITSELF — ONE FACT, WITH THE QUESTIONS
   ASKED OF IT BELOW. It was computed inline inside the baseline refusal while that was the only question this
   component asked of it; §9.4's step 11 asks a DIFFERENT one of the same value ("is this item STRETCHED"), and
   two resolutions of one keyword are two answers that are free to disagree about `align-self: auto`. So the
   resolution is the fact and each caller is a predicate over it, which is also why this returns the KEYWORD
   rather than a bool: a second bool would be a second resolution wearing a narrower name.
   §4's ANONYMOUS FLEX ITEM IS `item == NULL` AND IS NOT EXEMPT, which is §8.3's own sentence and the reason
   the question is asked of it at all: "align-items sets the default alignment for all of the flex container's
   items, INCLUDING ANONYMOUS FLEX ITEMS." §4 makes that box unstyleable, so it declares no `align-self` and
   takes the same arm an element item taking `auto` takes.
   §8.3's `Initial: auto` on `align-self` is what makes the container's `align-items` the value that decides an
   item that declares nothing, which is the whole of that keyword's meaning.
   THE CALLER FREES THE RESULT. */
static char *fx_resolved_align(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    char *items = css_computed_value(container, "align-items");
    char *self = NULL;

    DCHECK(items != NULL,
           "the cascade produced no computed value for `align-items` — it is in lexbor's registry with an "
           "initial value, so the last layer always answers");
    if (item != NULL) {
        self = css_computed_value(item, "align-self");
        DCHECK(self != NULL,
               "the cascade produced no computed value for `align-self` — it is in lexbor's registry with an "
               "initial value, so the last layer always answers");
    }
    if (self == NULL || strcmp(self, "auto") == 0) {
        free(self);
        return items;
    }
    free(items);
    return self;
}

/* §8.3's `stretch` VALUE'S OWN ANTECEDENT, WHICH IS THREE CONDITIONS AND NOT ONE: "If the cross size property
   of the flex item computes to `auto`, and neither of the cross-axis margins are `auto`, the flex item is
   STRETCHED." The keyword alone is not the test and reading it as one is the whole of what this predicate
   exists to prevent — a `stretch`-aligned item with a DECLARED cross size is not stretched, and neither is one
   with an `auto` cross margin, which §9.6 "Cross-Axis Alignment"' first step gives the spare cross space to
   instead.
   THE CROSS SIZE PROPERTY IS READ AS COMPUTED AND NOT AS THE PASS'S VALUE, which matters because the two come
   apart exactly once: CSS 2.1 §10.7's re-run substitutes a LIMIT for the size and runs the pass again, and
   that pass takes the DECLARED arm in core/layout/used_value.c rather than reaching this component at all. So
   `auto` here is §8.3's own word and never a restatement of which arm the caller is in — and a PERCENTAGE
   cross size that css-sizing-3 §3.2.1 "“Behaving as auto”" makes behave as auto is correctly NOT stretched,
   because §8.3 says COMPUTES TO `auto` and a percentage does not.
   THE MARGINS ARE READ AS COMPUTED FOR THE SAME REASON AND A SECOND ONE: core/layout/used_value.c REFUSES a
   vertical `auto` margin on a flex item by name, naming §9.6, so asking for its used value here would crash on
   the very page this arm exists to answer NO for. */
static bool fx_is_stretched(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    char *align;
    bool stretch;

    DCHECK(item != NULL,
           "css-flexbox-1 §8.3 \"Cross-axis Alignment: the align-items and align-self properties\"' STRETCHED "
           "test was asked with no item element. §4 \"Flex Items\"' anonymous flex item has no used cross size "
           "any caller asks for yet — §9.6 \"Cross-Axis Alignment\" is what would place it — so a NULL here is "
           "a caller that reached this predicate through a walk that should have stopped at step 8");
    align = fx_resolved_align(container, item);
    stretch = strcmp(align, "stretch") == 0;
    free(align);
    if (!stretch) return false;
    if (!fx_computed_is(item, "height", "auto")) return false;
    return !fx_computed_is(item, "margin-top", "auto") && !fx_computed_is(item, "margin-bottom", "auto");
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
static void fx_require_no_baseline_alignment(lxb_dom_element_t *container, lxb_dom_element_t *item,
                                             lxb_dom_node_t *subject)
{
    char *align = fx_resolved_align(container, item);
    bool baseline;
    char nbuf[160];

    baseline = strstr(align, "baseline") != NULL;
    free(align);
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
           "FOR §4 \"Flex Items\"' ANONYMOUS FLEX ITEM THE KEYWORD CAME FROM THE CONTAINER, which §8.3 "
           "states outright — \"align-items sets the default alignment for all of the flex container's items, "
           "including anonymous flex items\" — so that box is in §9.4 \"Cross Size Determination\"' "
           "step 8.1's collection exactly as an element "
           "item declaring `align-self: auto` under the same container is. "
           "WHAT IT NEEDS IS A BASELINE PER ITEM, MEASURED IN THE SAME FRAME AS THE OUTER CROSS EDGES, and "
           "core/layout/block_flow.h has both halves: `block_flow_first_line_box_baseline` and "
           "`block_flow_last_line_box_baseline` each report a distance from the box's TOP CONTENT EDGE and "
           "each answer FALSE for a box with no in-flow line box, which is css-flexbox-1 §8.5 \"Flex "
           "Container Baselines\"' own fallback condition. So what is missing is §8.5's choice between them "
           "and the two distances' conversion from the content edge to the item's OUTER cross edges, not a "
           "measurement",
           box_subject_node(subject, nbuf, sizeof nbuf));
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
           "THE WORDING IS THE STANDARD'S CURRENT ONE, WHICH IS WORTH A CLAUSE BECAUSE THIS TREE SPENT EIGHT "
           "SITES ON A SUPERSEDED ONE: they rendered the same floor in css-ui-3 §3.1 \"Changing the Box "
           "Model: the box-sizing property\"'s words under a css-sizing-3 §3.3 citation. THAT IS A MIS-AIMED "
           "QUOTATION AND NOT A FABRICATION — css-ui-3 defined `box-sizing` before css-sizing-3 took it "
           "over, css-ui-4 records the handover in its own prose, and css-sizing-3 REWROTE the sentence, so "
           "the words were a real standard's and were owed to no citation here. All eight are repaired and "
           "core/layout/used_value.h holds the argument; what a reader re-deriving this needs is that §3.3's "
           "floor is a step of the `border-box` conversion while §3.1's is unconditional, so a floor that "
           "converts nothing cites §3.1");
    return css_px_add(inner, css_px_add(fx_cross_border_padding(item), fx_cross_margins(item)));
}

/* §4 "Flex Items"' ANONYMOUS FLEX ITEM'S OUTER HYPOTHETICAL CROSS SIZE — §9.4's step 7 for the one box a
   flex container's item list holds that is not an element, so `fx_outer_hypothetical_cross` cannot be asked
   for it and this is not a second copy of that function but the SAME step over a different box.
   THE TWO HALVES THE ELEMENT ARM GETS FROM core/layout/used_value.h ARE BOTH ANSWERED HERE, and each is a
   sentence rather than a simplification.
     - THE INNER HALF. Step 7's own words are "performing layout as if it were an in-flow block-level box WITH
       THE USED MAIN SIZE and the given available space, treating auto as fit-content" — and §4 makes this box
       an "anonymous block container", whose content is a CHILD TEXT SEQUENCE and therefore holds no
       block-level box at all. So CSS 2.2 §9.4.2 "Inline formatting contexts" is the formatting context by
       §9.4.1's own alternative, CSS 2.1 §10.6.3's first bullet is the whole height rule — "the distance
       from its top content edge to the first applicable of the following", whose first bullet is "the bottom
       edge of the last line box" — and core/layout/line_box.h answers it.
       THE WIDTH IS STATED AND THAT IS THE WHOLE POINT OF THE ARGUMENT: `style` is the CONTAINER, because §4's
       box is unstyleable and therefore has the container's properties, and the container's content box is the
       WHOLE LINE rather than this item's share of it. Handing the walk `style` alone would lay the run out at
       the container's inner main size, which equals this item's used main size only where §9.7 "Resolving
       Flexible Lengths" happened to flex it to the entire line.
     - THE OUTER HALF IS ZERO AND IS NOT READ. css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer step
       adds the box's margin, border and padding, and §4's box has none to add: it is anonymous, so
       CSS 2.2 §9.2.1.1 "Anonymous block boxes"' rule applies — "non-inherited properties have their initial
       value" — and every one of CSS 2.1 §8's four edges is initially zero on all four sides. Reading them off
       the CONTAINER would report the container's own padding as this item's, which is the same mistake in the
       cross axis that core/layout/flex_line.c refuses in the main one by answering zero for a NULL element. */
static CssPx fx_anonymous_outer_hypothetical_cross(lxb_dom_element_t *container, lxb_dom_node_t *first,
                                                   lxb_dom_node_t *end, CssPx used_main)
{
    bool any_line_box;
    CssPx first_baseline, last_baseline, inner;
    BlockFlowRun seq;
    char nbuf[160];

    /* §4's CHILD TEXT SEQUENCE AS core/layout/block_flow.h's HALF-OPEN RUN, built in ONE place and from the
       two nodes the caller's own walk already holds: `after` is the sibling BEFORE the sequence (NULL where it
       opens the container's content) and `end` is one past its last text node, which is exactly the form
       core/layout/flex_line.c hands the same sequence to the intrinsic pass. */
    seq.after = first->prev;
    seq.end = end;
    inner = line_box_content_height(container, seq, line_box_available_width_stated(used_main),
                                    &any_line_box, &first_baseline, &last_baseline);

    DCHECKF(any_line_box,
            "%s: css-flexbox-1 §4 \"Flex Items\"' ANONYMOUS FLEX ITEM was measured and CSS 2.2 §9.4.2 "
            "\"Inline formatting contexts\" reports that it contains NO LINE BOX. Two of this engine's own "
            "components have then disagreed about one child text sequence: §4's classification already "
            "answered FLEX_ITEM_CHILD_NONE for a sequence that \"contains only document white space "
            "characters\", so a sequence that reached this measurement holds at least one character that is "
            "not one — and CSS 2.2 §9.4.2's zero-height rule begins \"line boxes that contain NO TEXT\", "
            "which such a line does not satisfy. So this is core/layout/flex_item.c's white-space test and "
            "core/layout/line_box.c's existence test reading one run differently, not a page",
            box_subject_node(first, nbuf, sizeof nbuf));
    DCHECK(inner.px >= 0.0,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 7 produced a NEGATIVE hypothetical cross "
           "size for §4 \"Flex Items\"' anonymous flex item. CSS 2.1 §10.6.3's first bullet is a running sum "
           "of line box heights and CSS 2.2 §10.8's step 3 makes each of those \"the distance between the "
           "uppermost box top and the lowermost box bottom\" — which core/layout/line_box.c asserts "
           "non-negative at its own origin — so a negative here is a derivation that lost an operand rather "
           "than a page");
    /* §4's box has no margin, border or padding to add, so the INNER size IS the outer one — see the banner. */
    return inner;
}

/* THE FOUR PRECONDITIONS BOTH ENTRIES OF THIS COMPONENT STATE, ASKED IN ONE PLACE — that it IS a flex
   container, that its block axis is the vertical one, that its main axis is its INLINE axis, and that it is
   SINGLE-LINE. They were written inside `flex_cross_size_content_based` while that was the only entry, and
   they are moved here rather than copied because they are one FACT about the container and not one entry's
   question about it: css-flexbox-1 §9.4 "Cross Size Determination"' step 8 and its step 11 are two steps of
   ONE algorithm over ONE container, so a second copy is two lists that can come apart and the one that
   drifts is whichever entry a later reader does not open.
   THE MESSAGES NAME THE SECTION AND NOT THE CALLER, WHICH IS WHAT THE MOVE COSTS AND IS WORTH IT: a refusal
   reading "the number THIS ENTRY answers" was true while there was one entry, and a step 11 caller meeting
   it would read it as a claim about step 8. */
static void fx_require_supported_container(lxb_dom_element_t *container)
{
    char nbuf[160], wbuf[64];
    char *display;
    bool is_container;

    DCHECK(container != NULL,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' cross sizing was asked for with no CONTAINER "
           "element");
    display = css_computed_value(container, "display");
    DCHECK(display != NULL, "the cascade produced no computed `display` for a box a layout is walking");
    is_container = flex_item_display_is_flex_container(display);
    free(display);
    DCHECK(is_container,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' cross sizing was asked of a box that is "
           "not a FLEX CONTAINER. §3 \"Flex Containers: the flex and inline-flex display values\" is the two "
           "spellings and core/layout/flex_item.h decides them; every property this component reads has a "
           "flex container or a flex item on its `Applies to:` line, so on any other box the walk below would "
           "be reading the cascade's initial keywords and reporting them as a layout");
    /* css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property" is what makes the CROSS axis
       of a `row` container the VERTICAL one, and it is asked FIRST and refused rather than assumed. It is
       re-asked here rather than taken on trust from the caller because this header states it as this
       component's own precondition and every physical edge below depends on it.
       THE CLAUSE THAT STOOD HERE NAMED css-writing-modes-4 §7.4 "Flow-Relative Mappings" AS THE ONE ABSENT
       CAPABILITY THIS AND TWO OTHER COMPONENTS WERE WAITING ON, AND THAT CITATION WAS MIS-AIMED. §7.4 is not
       a mapping table: it is the rule deciding WHOSE writing mode a flow-relative question is read against —
       "Flow-relative directions are calculated with respect to the writing mode of the CONTAINING BLOCK of
       the box" for a box's layout within it, and the box's own for rules about its contents. The TABLE is
       §6.4 "Abstract-to-Physical Mappings", and core/css/css_logical.c has held it, transcribed cell by cell
       and asserted by `css_logical_init`, for longer than these refusals have stood; its DIMENSION rows are
       now exported as `css_logical_axis_is_vertical` and composed with §5.1 by
       `flex_container_axis_is_vertical` (core/layout/flex_item.h), which core/layout/used_value.c routes on.
       SO THE MAPPING IS NOT WHAT THIS COMPONENT IS WAITING FOR, and the two questions below are not one
       question that a mapping would collapse. They are two and they stay two: the FIRST is that the CROSS
       axis is the vertical one, which decides whether `margin-top` and `padding-top` are the edges this walk
       adds; the SECOND is that the vertical axis is the BLOCK dimension, which decides whether
       `used_value_block_level_content_px` — CSS 2.1 §10.6.3's stack of block-level children, which
       css-writing-modes-4 §7.2 "Dimensional Mapping" names as the BLOCK dimension's rule — is the right
       measurement at all. A `vertical-rl` `column` container satisfies the first and fails the second, so a
       single mapping question here would admit a box whose cross size §10.6 does not compute. */
    if (!fx_computed_is(container, "writing-mode", "horizontal-tb"))
        DFAILF("%s, computed `writing-mode` `%s`: this FLEX CONTAINER's block axis is not the vertical one, "
               "so css-flexbox-1 §5.1 \"Flex Flow Direction: the flex-direction property\"' mapping does not "
               "make `margin-top`, `padding-top` and the top border width the CROSS-axis edges this walk adds "
               "— and every operand of §9.4 \"Cross Size Determination\"' step 8 below is one of those. "
               "THE REMEDY THAT STOOD HERE SAID TO BUILD css-writing-modes-4 §7.4 \"Flow-Relative "
               "Mappings\", NAMING core/layout/block_flow.c AND core/layout/flex_line.c AS WAITING ON THE "
               "SAME THING, AND THE CITATION WAS MIS-AIMED. §7.4 is the rule for WHOSE writing mode a "
               "flow-relative question is read against; the TABLE is §6.4 \"Abstract-to-Physical "
               "Mappings\", which core/css/css_logical.c holds and whose dimension rows are exported as "
               "`css_logical_axis_is_vertical`. WHAT THIS COMPONENT NEEDS IS NOT THAT MAPPING BUT §9.4's "
               "STEPS 7, 8 AND 11 READ IN THE OTHER DIMENSION: its measurements are "
               "`used_value_block_level_content_px`, which is CSS 2.1 §10.6.3's stack of block-level "
               "children and which css-writing-modes-4 §7.2 \"Dimensional Mapping\" names as the BLOCK "
               "dimension's rule — so a container whose cross axis is the vertical one while its BLOCK "
               "dimension is the horizontal one has a cross size §10.6 does not compute. BUILD the inline "
               "reading (css-sizing-3 §5.1's two intrinsic sizes are what §10.3 is to §10.6), and then this "
               "component reads whichever dimension the mapping names",
               box_subject(container, nbuf, sizeof nbuf),
               box_subject_computed(container, "writing-mode", wbuf, sizeof wbuf));
    if (flex_container_main_axis(container) != FLEX_MAIN_AXIS_INLINE)
        DFAILF("%s: this FLEX CONTAINER's main axis is its BLOCK axis (css-flexbox-1 §5.1 \"Flex Flow "
               "Direction: the flex-direction property\": a `column` container's main axis \"has the same "
               "orientation as the block axis of the current writing mode\"), so its CROSS axis is the INLINE "
               "one, so every number this component answers on the CROSS axis is a WIDTH rather than the height "
               "it was reached for — step 8's line cross size and step 11's item cross size alike. "
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
               "a content-based cross size is asked for exactly because the container does not have",
               box_subject(container, nbuf, sizeof nbuf));
}

CssPx flex_cross_size_content_based(lxb_dom_element_t *container)
{
    lxb_dom_node_t *c;
    CssPx largest = css_px(0.0);
    char nbuf[160];

    fx_require_supported_container(container);
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
        case FLEX_ITEM_CHILD_TEXT: {
            /* css-flexbox-1 §4 "Flex Items"' ANONYMOUS FLEX ITEM — "each child text sequence is wrapped in an
               anonymous block container flex item" — which §9.4's step 8 needs an OUTER HYPOTHETICAL CROSS
               SIZE for exactly like every other item on the line, and which used to crash here for two
               reasons that were BOTH about naming rather than about measuring.
               (1) ITS USED MAIN SIZE COULD NOT BE ASKED FOR. core/layout/flex_line.h had always COLLECTED
               this box and flexed it — §9.7 "Resolving Flexible Lengths" gives it a target main size like any
               other item — and only its LOOKUP was keyed on an element, so the number existed and had no
               name. It is now named by the item's FIRST NODE, which is this text node: §4's box is anonymous,
               so a DOM node the container's own child list holds is the only identity available that cannot
               collide with another item's.
               (2) ITS CROSS SIZE IS A RUN OF LINE BOXES AT THAT WIDTH, and core/layout/line_box.h derived the
               width from the element it was GIVEN — which here is the CONTAINER, whose content box is the
               whole line. That entry now takes §9.4.2's line box width as an argument, and this is the caller
               that states one.
               THE RUN IS §4's CHILD TEXT SEQUENCE AND IS DELIMITED THE SAME WAY core/layout/flex_line.c
               DELIMITS IT, through the same entry over the same child list, because it is the same sequence —
               and `next` IS that delimiter, so the node this walk advances to and the node the measurement
               stops at are ONE value rather than two reads of one question. */
            CssPx used_main;

            next = flex_item_text_sequence_end(container, c);
            /* §9.4's step 8.1 IS ASKED OF THIS BOX TOO — §8.3 "Cross-axis Alignment: the align-items and
               align-self properties" says `align-items` sets the default "for all of the flex container's
               items, including anonymous flex items", so the container's keyword reaches it and a baseline
               one puts it in the first collection rather than in the maximum below. */
            fx_require_no_baseline_alignment(container, NULL, c);
            used_main = flex_line_used_main_size(container, c);
            largest = css_px_max(largest,
                                 fx_anonymous_outer_hypothetical_cross(container, c, next, used_main));
            break;
        }
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
            fx_require_no_baseline_alignment(container, item, c);
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

/* css-flexbox-1 §9.4 "Cross Size Determination"' STEP 8's ANSWER FOR THE ONE LINE OF A SINGLE-LINE CONTAINER
 * — ITS TWO ARMS COINCIDE HERE AND THAT IS A DERIVATION RATHER THAN A SHORTCUT, so it is spelled out: the
 * obvious implementation is a dispatch on whether the container's cross size is definite, and a reader who
 * finds one call instead will otherwise write the dispatch back.
 *   - THE DEFINITE ARM IS THE CALL LITERALLY. "If the flex container is single-line and has a definite cross
 *     size, the cross size of the flex line is the flex container's inner cross size", and
 *     `used_value_content_px` on the cross axis IS the container's inner cross size.
 *   - THE INDEFINITE ARM REACHES THE SAME NUMBER THROUGH §9.6. Its "Otherwise" branch makes the line's cross
 *     size step 8.3's maximum, and §9.6 "Cross-Axis Alignment"' own step makes an auto-sized container's cross
 *     size "the sum of the flex lines' cross sizes" — one term for a single-line container. So the container's
 *     inner cross size IS that maximum, and core/layout/block_flow.c's `row` arm is the route that produces
 *     it: `used_value_content_px` reaches `block_flow_auto_height`, which reaches `flex_cross_size_content_
 *     based` above.
 *   - STEP 8.3's SINGLE-LINE CLAMP FALLS OUT AND IS NOT WRITTEN TWICE. "If the flex container is single-line,
 *     then clamp the line's cross-size to be within the container's computed min and max cross sizes" —
 *     which is CSS 2.1 §10.7's three steps over the CONTAINER, and `used_value_content_px` answers a USED
 *     value, so those steps have already run. The standard says as much in the note that follows: "Note that
 *     if CSS 2.1's definition of min/max-width/height applied more generally, this behavior would fall out
 *     automatically." THE CLAMP IS INSIDE THE `Otherwise` ARM AND NOT AFTER BOTH, which is a fact about the
 *     source and not about the prose: its `<p>` is nested in step 8.3's own `<li>`, inside the `<ol>` the
 *     "Otherwise, for each flex line:" paragraph opens. Reading it as applying to the definite arm too would
 *     clamp a number §10.7 had already clamped.
 * A STRETCHED CONTAINER IS COVERED BY THE FIRST ARM AND NOT BY AN EXCEPTION, which is the case that looks
 * like a counterexample: a flex container that is ITSELF a stretched flex item has no declaration of its own
 * and still has a definite cross size, because §9.8 "Definite and Indefinite Sizes" says so in its own words
 * — "If a single-line flex container has a definite cross size, the automatic preferred outer cross size of
 * any stretched flex items is the flex container's inner cross size (clamped to the flex item's min and max
 * cross size) and IS CONSIDERED DEFINITE." `used_value_content_px` answers that box's used cross size, which
 * is the stretched one.
 * IT TERMINATES, AND THE REASON IS STEP 7's OWN RECLASSIFICATION rather than anything this file does. The
 * only way back into this component from here is the indefinite arm's walk, and every per-item measurement
 * that walk makes is `used_value_block_level_content_px`, which reads the item AS A BLOCK-LEVEL BOX — so it
 * takes CSS 2.1 §10.6.3's stack and never re-enters §9.4's step 11. What DOES recur is the ancestor chain:
 * an item's container may itself be an item, and each level asks its own container, which terminates at the
 * first box that is not a flex item. */
static CssPx fx_line_cross_size(lxb_dom_element_t *container)
{
    return used_value_content_px(container, true);
}

CssPx flex_cross_size_used_item_cross(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    CssPx line, surround;

    fx_require_supported_container(container);
    DCHECK(item != NULL,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for with no item element");
    DCHECK(lxb_dom_interface_node(item)->parent == lxb_dom_interface_node(container),
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for an item that is not a "
           "child of the container it was asked about. Step 11's operand is \"the flex line's cross size\", "
           "so a subject drawn from one container and a line drawn from another is an item sized against a "
           "line it is not on — the same precondition core/layout/flex_line.h states for the main axis, and "
           "the one that makes `css-display-3 §2.5 \"Box Generation: the none and contents keywords\"' "
           "`contents` splice a case this component has not been handed rather than one it answers wrongly");
    DCHECK(used_value_height_behaves_as_auto(item),
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for an item whose cross size "
           "property DOES NOT behave as `auto`. This entry answers one arm of step 11 and its header says "
           "which: the other arm — \"Otherwise, the used cross size is the item's hypothetical cross size\" — "
           "is, for a declared cross size, step 7's own layout of that declaration \"as if it were an in-flow "
           "block-level box\", which CSS 2.1 §10.6.2 and §10.6.3 make the declared value itself. "
           "core/layout/used_value.c computes that for every other box already and FALLS THROUGH to it, so an "
           "item arriving here with a declaration is that fall-through having been lost and the number this "
           "entry would answer is the LINE's cross size reported as the item's");
    /* STEP 11's FIRST ARM — "If a flex item's cross size depends on the available space in the cross axis,
       recalculate its cross size using the flex line's cross size (rather than the flex container's) as the
       available space." §8.3 "Cross-axis Alignment: the align-items and align-self properties" is what makes
       an item's cross size depend on that space, and it names the state: "the flex item is STRETCHED. Its
       used value is the length necessary to make the cross size of the item's MARGIN BOX as close to the
       same size as the line as possible."
       SO THE SUBTRACTION IS THE MARGIN BOX'S AND NOT THE BORDER BOX'S, which is the whole of the arithmetic:
       the margins are subtracted alongside the border and padding because §8.3 equalises the MARGIN box, and
       an item with a 10px cross margin under a 100px line has an 80px content box rather than a 90px one.
       THE FLOOR IS §8.3's OWN "AS CLOSE … AS POSSIBLE" and not a defensive clamp: an item whose surround
       alone exceeds the line cannot reach it, and the nearest cross size it can take is zero — which
       css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property" states as a standing property of a
       content box in the same words ("as the inner size of a box cannot be negative").
       THE MIN/MAX CLAMP §8.3's NEXT CLAUSE NAMES IS NOT HERE AND THE HEADER SAYS WHY — CSS 2.1 §10.7's three
       steps are that clamp and they run over this number at the caller. */
    if (fx_is_stretched(container, item)) {
        line = fx_line_cross_size(container);
        surround = css_px_add(fx_cross_border_padding(item), fx_cross_margins(item));
        return css_px_max(css_px_sub(line, surround), css_px(0.0));
    }
    /* STEP 11's SECOND ARM — "Otherwise, the used cross size is the item's HYPOTHETICAL CROSS SIZE" — which is
       step 7, and step 7 is `used_value_block_level_content_px` and nothing else. This is the INNER half of
       what `fx_outer_hypothetical_cross` above returns rather than a second reading of it: that function adds
       css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer step because step 8.2 takes the largest OUTER
       hypothetical cross size, and step 11 gives the item its own cross size, which is the inner one. Two
       callers of one entry, each adding what its own step asks for. */
    return used_value_block_level_content_px(item, true);
}
