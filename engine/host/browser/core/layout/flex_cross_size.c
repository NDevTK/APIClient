/* css-flexbox-1 §9.4 "Cross Size Determination"' steps 7, 8 and 11 and §9.6 "Cross-Axis Alignment"'s container
   cross size. See flex_cross_size.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/line_box.h"
#include "core/layout/used_value.h"

/* Keyword-valued properties only. Length-valued ones (a cross size, a cross margin) go through
   `css_computed_length_is` (core/css/css_computed_value.h), whose `CssPx` keeps the environment fact a `50vh`
   size carries; the text entry refuses them. */
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

/* css-writing-modes-4 §7.2 "Dimensional Mapping"'s physical names of the three cross-axis properties read as
   computed values, for a vertical (`vertical`) or horizontal cross axis; the mirror of core/layout/
   flex_line.c's `fl_main_props`. §6.4 decides which physical axis (via `flex_container_axis_is_vertical`), and
   §7.2 which property states a size on it: "The height properties (height, min-height, and max-height) refer
   to the physical height, and the width properties (width, min-width, and max-width) refer to the physical
   width." The size's limits are absent because §8.3's stretched test names only the size and the two margins,
   and the §10.7 clamp is the caller's (flex_cross_size.h). */
typedef struct {
    const char *size;          /* `height`        or `width`        */
    const char *margin_start;  /* `margin-top`    or `margin-left`  */
    const char *margin_end;    /* `margin-bottom` or `margin-right` */
} FxCrossProps;

static FxCrossProps fx_cross_props(bool vertical)
{
    FxCrossProps p;

    p.size         = vertical ? "height"        : "width";
    p.margin_start = vertical ? "margin-top"    : "margin-left";
    p.margin_end   = vertical ? "margin-bottom" : "margin-right";
    return p;
}

/* The item's cross-axis border and padding, as used values, on the pair the axis names (css-writing-modes-4
   §7.2: CSS 2.1 §10.3's rules apply "to the inline size … and to the inline-start and inline-end margins,
   padding, and border", §10.6's likewise to the block size). `used_value_border_widths_px` writes CSS 2.1
   §8.5.1's top/right/bottom/left order, so the vertical pair is entries 0 and 2, the horizontal 1 and 3. */
static CssPx fx_cross_border_padding(lxb_dom_element_t *el, bool vertical)
{
    CssPx b[4];

    used_value_border_widths_px(el, b);
    if (vertical)
        return css_px_add(css_px_add(b[0], b[2]),
                          css_px_add(used_value_px(el, "padding-top"), used_value_px(el, "padding-bottom")));
    return css_px_add(css_px_add(b[1], b[3]),
                      css_px_add(used_value_px(el, "padding-left"), used_value_px(el, "padding-right")));
}

/* The item's cross-axis margins. An `auto` one is not answered: css-flexbox-1 §9.6 "Cross-Axis Alignment"'s
   first step gives it "the difference" between the item's outer cross size and its line's ("distribute the
   difference in those sizes equally to the auto margins"), which does not exist at step 8.
   core/layout/used_value.c refuses it by name on whichever physical axis is the cross one, naming §9.6. */
static CssPx fx_cross_margins(lxb_dom_element_t *el, bool vertical)
{
    FxCrossProps prop = fx_cross_props(vertical);

    return css_px_add(used_value_px(el, prop.margin_start), used_value_px(el, prop.margin_end));
}

/* The same pair for CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal flow"' stretch-fit, where an
   `auto` margin is zero: "If 'width' is set to 'auto', any other 'auto' values become '0' and 'width' follows
   from the resulting equality." css-flexbox-1 §9.4 "Cross Size Determination"' step 7 lays the item out as an
   in-flow block-level box with an `auto` cross size, so that antecedent holds.
   A second function, not a flag: §8.3 "Cross-axis Alignment: the align-items and align-self properties"'
   stretched value requires neither margin to be `auto`, so `fx_cross_margins` can never meet one there, while
   step 7 meets them routinely. A bool would let either caller take the other's rule. */
static CssPx fx_cross_margins_stretch_fit(lxb_dom_element_t *el, bool vertical)
{
    FxCrossProps prop = fx_cross_props(vertical);
    CssPx m = css_px(0.0);

    if (!css_computed_length_is(el, prop.margin_start, CSS_KEYWORD_AUTO)) m = used_value_px(el, prop.margin_start);
    if (!css_computed_length_is(el, prop.margin_end, CSS_KEYWORD_AUTO))
        m = css_px_add(m, used_value_px(el, prop.margin_end));
    return m;
}

/* §8.3's resolved cross-axis alignment for one item, as the keyword: one resolution that the baseline refusal
   and step 11's stretched test both read, so they cannot disagree about `align-self: auto`.
   §4's anonymous item is `item == NULL` and is included: "align-items sets the default alignment for all of the
   flex container's items, including anonymous flex items"; unstyleable, it takes `auto`'s arm. `align-self`'s
   `Initial: auto` is what defers an undeclared item to the container. The caller frees the result. */
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

/* §8.3's `stretch` antecedent, three conditions: "If the cross size property of the flex item computes to
   `auto`, and neither of the cross-axis margins are `auto`, the flex item is stretched." A `stretch` item with a
   declared size, or with an `auto` cross margin (which §9.6's first step serves instead), is not stretched.
   The size is read as computed: a percentage that css-sizing-3 §3.2.1 "“Behaving as auto”" makes behave as
   auto does not compute to `auto`, so it is not stretched, and §10.7's re-run takes used_value.c's declared arm
   and never reaches here. The margins are read as computed because used_value.c refuses an `auto` cross
   margin. All three names come from the cross axis: `width`/`margin-left`/`margin-right` for a `column`
   container, `height`/`margin-top`/`margin-bottom` for a `row` one. */
static bool fx_is_stretched(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical)
{
    FxCrossProps prop = fx_cross_props(vertical);
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
    if (!css_computed_length_is(item, prop.size, CSS_KEYWORD_AUTO)) return false;
    return !css_computed_length_is(item, prop.margin_start, CSS_KEYWORD_AUTO) &&
           !css_computed_length_is(item, prop.margin_end, CSS_KEYWORD_AUTO);
}

/* css-flexbox-1 §9.4's step 8.1, the baseline group, which is not computed and so must not be dropped from
   step 8.3's maximum. §8.3 "Cross-axis Alignment: the align-items and align-self properties" declares
   `flex-start | flex-end | center | baseline | stretch` (`align-self` adds `auto`), with "align-items sets the
   default alignment for all of the flex container's items, including anonymous flex items. align-self allows
   this default alignment to be overridden for individual flex items."
   The test is a substring on purpose: css-align-3's `first baseline` and `last baseline` are baseline
   alignments too, and over-refusing is a crash rather than a wrong number. */
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

/* css-flexbox-1 §9.4 "Cross Size Determination"' step 8 for a single-line container's line (multi-line:
 * `fx_line_cross_sizes`). Its two arms coincide here, so there is one call and no dispatch on definiteness:
 *   - Definite: "If the flex container is single-line and has a definite cross size, the cross size of the flex
 *     line is the flex container's inner cross size", which `used_value_content_px` on the cross axis is.
 *   - Indefinite: step 8.3's maximum, which §9.6 makes the container's auto cross size ("the sum of the flex
 *     lines' cross sizes", one term), reached through `used_value_content_px` -> `block_flow_auto_height` ->
 *     `flex_cross_size_content_based`. On an inline cross axis §9.6's formatting-context rule (CSS 2.1 §10.3.3's
 *     equation) answers first, so the same call is right on both axes.
 *   - Step 8.3's single-line clamp to "the container's computed min and max cross sizes" is §10.7, already run
 *     in a used value ("if CSS 2.1's definition of min/max-width/height applied more generally, this behavior
 *     would fall out automatically"). It sits in the `Otherwise` arm only, not after both.
 * A stretched container is definite (§9.8 "Definite and Indefinite Sizes": "is considered definite").
 * Termination: step 7 measures items as block-level boxes (§10.6.3, never step 11 again), so recursion only
 * climbs the ancestor chain to the first non-item; the inline arm asks the container's §10.3.3 width (further
 * up) and the item's descendants' intrinsic sizes, neither reaching the item asked about. */
static CssPx fx_line_cross_size(lxb_dom_element_t *container, bool vertical)
{
    return used_value_content_px(container, vertical);
}

/* css-flexbox-1 §9.4's step 7: one item's hypothetical cross size, as a content-box extent on `container`'s
   cross axis. "Determine the hypothetical cross size of each item by performing layout as if it were an
   in-flow block-level box with the used main size and the given available space, treating auto as
   fit-content." css-sizing-3 §3.2's fit-content is "min(max-content, max(min-content, stretch))":
     - Block arm: both intrinsic block sizes are "equivalent to its automatic size", so the formula is that one
       number A, `used_value_block_level_content_px` (CSS 2.1 §10.6 and §10.7 over a §10.3.3 box), which this
       file must not copy. The available space does not enter.
     - Inline arm: the formula over css-sizing-3 §5.1's pair (core/layout/intrinsic_size.h) and the stretch-fit
       size (§2.1: "for the inline axis, this is called the 'available width' in CSS2.1§10.3.5 and computed by
       the rules in CSS2.1§10.3.3"), i.e. the container's inner cross size less the item's cross edges
       (`avail`). Written in CSS 2.2 §10.3.5's order like used_value.c's `uv_shrink_to_fit_width`.
   `avail` uses §10.3.3's auto-as-zero margins; step 11's stretched arm uses `fx_cross_margins`, which refuses
   `auto`, so they share no expression. It is the container's inner cross size (step 11: "rather than the flex
   container’s"), one number with the line's here. A replaced item needs no arm: intrinsic_size.c returns its
   §10.3.2 size as both terms; the block arm refuses it in `used_value_block_level_content_px`. */
static CssPx fx_hypothetical_cross(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical)
{
    CssPx inner;

    if (vertical) {
        inner = used_value_block_level_content_px(item, true);
    } else {
        IntrinsicInlineSizes in = intrinsic_inline_sizes(item);
        CssPx surround = css_px_add(fx_cross_border_padding(item, false),
                                    fx_cross_margins_stretch_fit(item, false));
        CssPx avail = css_px_max(css_px_sub(fx_line_cross_size(container, false), surround), css_px(0.0));

        inner = css_px_min(css_px_max(in.min_content, avail), in.max_content);
    }
    DCHECK(inner.px >= 0.0,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 7 produced a NEGATIVE hypothetical cross "
           "size. Every arm it can take floors at zero — css-sizing-3 §3.3 \"Box Edges for Sizing: the "
           "box-sizing property\" says of the `border-box` conversion that \"the content box width and height "
           "are calculated by subtracting the border and padding in the corresponding axis from the specified "
           "<length-percentage>, and flooring the result at zero (as the inner size of a box cannot be "
           "negative)\", and CSS 2.1 §10.6.3's walk is a running sum of used heights — so a negative here is "
           "a derivation that lost an operand rather than a page. "
           "THE INLINE ARM NEEDS NO FLOOR OF ITS OWN AND THAT IS THE FORMULA'S ARITHMETIC RATHER THAN A "
           "CLAMP LEFT OUT, the same argument core/layout/used_value.c's `uv_shrink_to_fit_width` makes: "
           "`max(min-content, available)` is at least the min-content size, which core/layout/intrinsic_size.c "
           "asserts non-negative, and the `min` with the max-content size cannot take it below that same "
           "floor while core/layout/intrinsic_size.c's own `min-content <= max-content` holds. So a negative "
           "on THIS arm is one of those two asserts having been bypassed rather than a narrow container. "
           "THE WORDING IS THE STANDARD'S CURRENT ONE, WHICH IS WORTH A CLAUSE BECAUSE THIS TREE SPENT EIGHT "
           "SITES ON A SUPERSEDED ONE: they rendered the same floor in css-ui-3 §3.1 \"Changing the Box "
           "Model: the box-sizing property\"'s words under a css-sizing-3 §3.3 citation. THAT IS A MIS-AIMED "
           "QUOTATION AND NOT A FABRICATION — css-ui-3 defined `box-sizing` before css-sizing-3 took it "
           "over, css-ui-4 records the handover in its own prose, and css-sizing-3 REWROTE the sentence, so "
           "the words were a real standard's and were owed to no citation here. All eight are repaired and "
           "core/layout/used_value.h holds the argument; what a reader re-deriving this needs is that §3.3's "
           "floor is a step of the `border-box` conversion while §3.1's is unconditional, so a floor that "
           "converts nothing cites §3.1");
    return inner;
}

/* Step 7 plus css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer step, which step 8.2 maximises: "Among
   all the items not collected by the previous step, find the largest outer hypothetical cross size." Separate
   because step 11's second arm wants the inner size. */
static CssPx fx_outer_hypothetical_cross(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical)
{
    return css_px_add(fx_hypothetical_cross(container, item, vertical),
                      css_px_add(fx_cross_border_padding(item, vertical), fx_cross_margins(item, vertical)));
}

/* §4 "Flex Items"' anonymous flex item's outer hypothetical cross size: step 7 over the one non-element box.
     - Inner: an "anonymous block container" holding a child text sequence and no block-level box, so CSS 2.2
       §9.4.2 "Inline formatting contexts" applies and CSS 2.1 §10.6.3's first bullet, "the bottom edge of the
       last line box", is the height (core/layout/line_box.h). The width is stated explicitly (`used_main`):
       `style` is the container, whose content box is the whole line, not this item's share.
     - Outer: zero. Anonymous, it has initial edges ("non-inherited properties have their initial value", CSS 2.2
       §9.2.1.1 "Anonymous block boxes"); reading them off the container would be the mistake core/layout/
       flex_line.c avoids in the main axis by answering zero for a NULL element. */
static CssPx fx_anonymous_outer_hypothetical_cross(lxb_dom_element_t *container, lxb_dom_node_t *first,
                                                   lxb_dom_node_t *end, CssPx used_main)
{
    bool any_line_box;
    CssPx first_baseline, last_baseline, inner;
    BlockFlowRun seq;
    char nbuf[160];

    /* §4's child text sequence as core/layout/block_flow.h's half-open run: `after` is the box-tree member
       before the sequence (NULL at the container's start) and `end` one past its last text node, as
       core/layout/flex_line.c hands it to the intrinsic pass. Not `first->prev`: under css-display-3 §2.5's
       splice that is NULL at the start of a `contents` element's children, which would start the run at the
       container's own start. */
    seq.after = box_tree_child_node(box_tree_prev_sibling(container, box_tree_child_of_node(first)));
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
    /* §4's box has no margin, border or padding to add, so the inner size is the outer one. */
    return inner;
}

/* The two preconditions both entries state, in one place: the box is a flex container and its block axis is
   vertical (`horizontal-tb`). They are one fact about the container shared by §9.4's steps 8 and 11. Either
   physical axis may be the cross one (edges come from `fx_cross_props`); the entry-specific block-axis
   condition is `flex_cross_size_content_based`'s. Messages name the section, not the caller. */
static void fx_require_supported_container(lxb_dom_element_t *container)
{
    char nbuf[160], wbuf[64];
    CssDisplay display;
    bool is_container;

    DCHECK(container != NULL,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' cross sizing was asked for with no CONTAINER "
           "element");
    display = css_computed_display(container);
    is_container = flex_item_display_is_flex_container(display);
    DCHECK(is_container,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' cross sizing was asked of a box that is "
           "not a FLEX CONTAINER. §3 \"Flex Containers: the flex and inline-flex display values\" is the two "
           "spellings and core/layout/flex_item.h decides them; every property this component reads has a "
           "flex container or a flex item on its `Applies to:` line, so on any other box the walk below would "
           "be reading the cascade's initial keywords and reporting them as a layout");
    /* css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property": re-asked here as this
       component's own precondition. In `horizontal-tb` the one `vertical` bit names both a physical axis
       (css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings", core/css/css_logical.c) and a dimension
       (§7.2 "Dimensional Mapping"), letting `fx_hypothetical_cross` choose §10.6 or §10.3 from it. In a
       vertical mode they come apart; what is missing then is splitting that bit into the two and measuring
       the intrinsic pair in the dimension rather than the inline axis. */
    if (!fx_computed_is(container, "writing-mode", "horizontal-tb"))
        DFAILF("%s, computed `writing-mode` `%s`: this FLEX CONTAINER's BLOCK dimension is not the vertical "
               "axis, so the ONE bit this component threads cannot name both — css-writing-modes-4 §7.2 "
               "\"Dimensional Mapping\" reads it as `which physical pair of edges is the cross axis's` and "
               "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 7 reads it as `which of CSS 2.1's two "
               "sizing chapters measures this item`, and in a `horizontal-tb` mode those are the same bit. A `vertical-rl` `column` container has its CROSS axis vertical and "
               "its INLINE dimension vertical too, so step 7 owes it css-sizing-3 §5.1 \"Intrinsic Sizes\"' "
               "pair measured in the VERTICAL axis while the edges it adds are `margin-top` and "
               "`padding-top` — one arm of this component's own dispatch taken from each answer. "
               "TWO REMEDIES HAVE BEEN RETIRED HERE AND BOTH ARE WRITTEN OUT BECAUSE A READER RE-DERIVES "
               "THEM. The first said to BUILD css-writing-modes-4 §7.4 \"Flow-Relative Mappings\", and that "
               "citation was MIS-AIMED: §7.4 is the rule for WHOSE writing mode a flow-relative question is "
               "read against, and the TABLE is §6.4 \"Abstract-to-Physical Mappings\", which "
               "core/css/css_logical.c holds and whose dimension rows are exported as "
               "`css_logical_axis_is_vertical`. The second said to BUILD §9.4's STEPS 7, 8 AND 11 READ IN "
               "THE OTHER DIMENSION, naming css-sizing-3 §5.1's two intrinsic sizes as what CSS 2.1 §10.3 "
               "\"Calculating widths and margins\" is to §10.6 \"Calculating heights and margins\" — and "
               "step 7's INLINE arm and step 11 ARE BUILT, over exactly that pair, so a reader who follows "
               "it builds them a second time. WHAT IS ACTUALLY MISSING IS THAT `fx_cross_props` AND "
               "`fx_hypothetical_cross` TAKE ONE `vertical` WHERE THIS BOX NEEDS TWO: split it into the "
               "PHYSICAL axis §6.4 answers and the DIMENSION §7.2 answers, hand each to the half that reads "
               "it, and measure the intrinsic pair in the dimension rather than in the inline axis — "
               "core/layout/intrinsic_size.h is named for the INLINE one and says so",
               box_subject(container, nbuf, sizeof nbuf),
               box_subject_computed(container, "writing-mode", wbuf, sizeof wbuf));
}

/* §9.4's steps 8.2 and 8.1's refusal for one line member on a block cross axis: its outer hypothetical cross
   size. An element item is step 7 plus its edges; §4's anonymous item is line boxes at its used main size.
   The baseline refusal comes first because §9.4's step 8.1 collects those items instead. */
static CssPx fx_member_outer_hypothetical_cross(lxb_dom_element_t *container, const FlexLineMember *m)
{
    lxb_dom_element_t *item;

    DCHECK(m->first != NULL, "a flex line member carries no identity node");
    if (m->anonymous) {
        /* Step 8.1 applies to this box too: §8.3's `align-items` sets the default "for all of the flex
           container's items, including anonymous flex items". */
        fx_require_no_baseline_alignment(container, NULL, m->first);
        return fx_anonymous_outer_hypothetical_cross(container, m->first, m->end,
                                                     flex_line_used_main_size(container, m->first));
    }
    DCHECK(m->first->type == LXB_DOM_NODE_TYPE_ELEMENT && m->end == NULL,
           "a flex line member that is not §4's anonymous item is not an element, or carries a text-sequence end");
    item = lxb_dom_interface_element(m->first);
    fx_require_no_baseline_alignment(container, item, m->first);
    return fx_outer_hypothetical_cross(container, item, true);
}

/* §9.4's step 8 for every line of `container` on a block cross axis, in the `Otherwise` arm: "for each flex
   line: … The used cross size of the flex line is the largest of the numbers found in the previous two steps
   and zero." Writes one size per line to a calloc'd `*sizes` the caller frees and returns the line count.
   The single-line clamp of step 8.3 is §10.7's, run in a used value (see `fx_line_cross_size`). */
static size_t fx_line_cross_sizes(lxb_dom_element_t *container, CssPx **sizes)
{
    FlexLineMember *m;
    size_t n, lines, i;

    n = flex_line_members(container, &m);
    *sizes = NULL;
    if (n == 0) return 0;
    lines = m[n - 1].line + 1;
    *sizes = (CssPx *)calloc(lines, sizeof **sizes);
    CHECK(*sizes != NULL, "allocating css-flexbox-1 §9.4 step 8's per-line cross sizes failed — one number per "
                          "flex line of one container, so this is the physical RAM floor");
    for (i = 0; i < n; i++) {
        DCHECK(m[i].line < lines && (i == 0 || m[i].line >= m[i - 1].line),
               "flex_line_members answered lines that are not contiguous runs in collection order");
        (*sizes)[m[i].line] = css_px_max((*sizes)[m[i].line], fx_member_outer_hypothetical_cross(container, &m[i]));
    }
    free(m);
    return lines;
}

CssPx flex_cross_size_content_based(lxb_dom_element_t *container)
{
    CssPx *sizes, sum = css_px(0.0);
    size_t lines, i;
    char nbuf[160];

    fx_require_supported_container(container);
    /* This entry's own precondition: the cross axis is the block one. §9.4's step 8 itself is stated over
       either axis; this is about who asks. §9.6 asks only where a content-based cross size is needed, and a
       `column` container's is a width under CSS 2.1 §10.3.3 "Block-level, non-replaced elements in normal
       flow"' content-free equation, so `block_flow_auto_height` and used_value.c never route one here. A
       shrink-to-fit `column` container is answered by §9.9.2 "Flex Container Intrinsic Cross Sizes" in
       core/layout/flex_intrinsic_size.c (via intrinsic_size.c), not by this walk. */
    if (!flex_container_axis_is_vertical(container, FLEX_AXIS_CROSS))
        DFAILF("%s: this FLEX CONTAINER's CROSS axis is its INLINE axis (css-flexbox-1 §5.1 \"Flex Flow "
               "Direction: the flex-direction property\": a `column` container's main axis \"has the same "
               "orientation as the block axis of the current writing mode\"), so a CONTENT-BASED cross size "
               "for it is a WIDTH — and §9.6 \"Cross-Axis Alignment\"' step asks for one only where the "
               "rules of the formatting context need it, which for a block-level container is CSS 2.1 "
               "§10.3.3 \"Block-level, non-replaced elements in normal flow\"' constraint equation and "
               "therefore never. BUILD css-flexbox-1 §9.9.2 \"Flex Container Intrinsic Cross Sizes\" — \"The "
               "min-content/max-content cross size of a single-line flex container is the largest min-content "
               "contribution/max-content contribution (respectively) of its flex items.\" — in "
               "core/layout/flex_intrinsic_size.c, because a SHRINK-TO-FIT `column` container is the one box "
               "that asks this question and it asks it as an intrinsic size rather than as §9.6's sum",
               box_subject(container, nbuf, sizeof nbuf));
    /* §9.6's "If a content-based cross size is needed, use the sum of the flex lines' cross sizes", over step
       8's `Otherwise` arm. Step 8's definite single-line arm and step 9's stretch are not read: a content-based
       size is the automatic size (css-sizing-3 §3.2 makes `min-content`/`max-content` block sizes "equivalent
       to its automatic size" too), and an automatic size is not a definite cross size. An empty container sums
       nothing (§6 "Flex Lines": "unless the flex container itself is completely empty"). */
    lines = fx_line_cross_sizes(container, &sizes);
    for (i = 0; i < lines; i++) sum = css_px_add(sum, sizes[i]);
    free(sizes);
    return sum;
}

/* §9.4's step 9 over a multi-line container's line sizes: "If the flex container has a definite cross size,
   align-content is stretch, and the sum of the flex lines' cross sizes is less than the flex container's inner
   cross size, increase the cross size of each flex line by equal amounts such that the sum of their cross
   sizes exactly equals the flex container's inner cross size." On a block cross axis a definite cross size is
   a `height` that does not behave as auto (css-sizing-3 §3.2.1 "“Behaving as auto”").
   Named residual. Not covered: §9.8 "Definite and Indefinite Sizes"' flex-specific rules that make a flex
   item's size definite without a declaration, so a multi-line container that is itself a stretched item of a
   definite single-line container counts as indefinite here. Next: a definiteness test built from §9.8's
   rules, which core/layout/flex_line.c's replaced-item refusal also names. Absence shows as such a container's lines not
   growing to fill the height its parent stretched it to. */
static void fx_stretch_lines(lxb_dom_element_t *container, CssPx *sizes, size_t lines)
{
    CssPx sum = css_px(0.0), inner, share;
    size_t i;

    if (used_value_height_behaves_as_auto(container)) return;
    if (!fx_computed_is(container, "align-content", "stretch")) return;
    for (i = 0; i < lines; i++) sum = css_px_add(sum, sizes[i]);
    inner = used_value_content_px(container, true);
    if (!(sum.px < inner.px)) return;
    share = css_px_scale(css_px_sub(inner, sum), 1.0 / (double)lines);
    for (i = 0; i < lines; i++) sizes[i] = css_px_add(sizes[i], share);
}

/* §9.4's steps 8 and 9 for the line holding `item` in a multi-line container on a block cross axis. */
static CssPx fx_multi_line_cross_size_of(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    FlexLineMember *m;
    CssPx *sizes, out;
    size_t n, lines, i, line = 0;
    bool found = false;
    char nbuf[160];

    n = flex_line_members(container, &m);
    for (i = 0; i < n; i++)
        if (m[i].first == lxb_dom_interface_node(item)) { line = m[i].line; found = true; break; }
    free(m);
    DCHECKF(found, "%s: css-flexbox-1 §9.3's step 5 did not collect this item into any line of its own "
                   "container, so the two readings of one child list disagree",
            box_subject(item, nbuf, sizeof nbuf));
    lines = fx_line_cross_sizes(container, &sizes);
    DCHECK(line < lines, "an item's §9.3 line is past the lines §9.4's step 8 measured over the same walk");
    fx_stretch_lines(container, sizes, lines);
    out = sizes[line];
    free(sizes);
    return out;
}

CssPx flex_cross_size_used_item_cross(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    bool vertical;
    CssPx line, surround;
    char nbuf[160];

    fx_require_supported_container(container);
    /* Which physical axis the cross axis is, asked once of the container (css-writing-modes-4 §7.4
       "Flow-Relative Mappings": layout within the containing block uses its writing mode, and the flex
       container is its items' containing block). `fx_require_supported_container` already refused writing
       modes where this bit does not also name the dimension. */
    vertical = flex_container_axis_is_vertical(container, FLEX_AXIS_CROSS);
    DCHECK(item != NULL,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for with no item element");
    DCHECK(box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(item))) == container,
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for an item that is not a "
           "child of the container it was asked about. Step 11's operand is \"the flex line's cross size\", "
           "so a subject drawn from one container and a line drawn from another is an item sized against a "
           "line it is not on. The relation is the box tree's, since css-display-3 §2.5 \"Box Generation: the "
           "none and contents keywords\" puts a `contents` child's children on this line");
    /* css-sizing-3 §3.2.1 "“Behaving as auto”" names only "block percentage heights resolving against an
       indefinite size, see CSS2§10.5"; an inline percentage resolves against a definite width here, so on
       the inline axis behaving as auto is computing to `auto`. `used_value_height_behaves_as_auto` on both
       axes would read a `column` item's main size. */
    DCHECK(vertical ? used_value_height_behaves_as_auto(item)
                    : css_computed_length_is(item, fx_cross_props(false).size, CSS_KEYWORD_AUTO),
           "css-flexbox-1 §9.4 \"Cross Size Determination\"' step 11 was asked for an item whose cross size "
           "property DOES NOT behave as `auto`. This entry answers one arm of step 11 and its header says "
           "which: the other arm — \"Otherwise, the used cross size is the item's hypothetical cross size\" — "
           "is, for a declared cross size, step 7's own layout of that declaration \"as if it were an in-flow "
           "block-level box\", which CSS 2.1 §10.6.2 and §10.6.3 make the declared value itself in the BLOCK "
           "dimension and CSS 2.1 §10.3.3 \"Block-level, non-replaced elements in normal flow\" makes it in "
           "the INLINE one, every rule either of them states being conditioned on the property being `auto`. "
           "core/layout/used_value.c computes that for every other box already and FALLS THROUGH to it, so an "
           "item arriving here with a declaration is that fall-through having been lost and the number this "
           "entry would answer is the LINE's cross size reported as the item's");
    /* Step 11's first arm: "If a flex item's cross size depends on the available space in the cross axis,
       recalculate its cross size using the flex line's cross size (rather than the flex container's) as the
       available space." §8.3 makes a stretched item depend on it: "Its used value is the length necessary to
       make the cross size of the item's margin box as close to the same size as the line as possible." So the
       margins are subtracted with border and padding (10px margins under a 100px line give 80px). The zero
       floor is §8.3's "as close … as possible" and css-sizing-3 §3.1 "Sizing Properties"' "the inner size is
       always floored at zero" (not §3.3's `border-box` step: nothing is converted). §8.3's min/max clamp is the
       caller's §10.7. The operands are read inside this arm: `fx_cross_margins` refuses `auto`, one of the
       things §8.3 tests. */
    if (fx_is_stretched(container, item, vertical)) {
        if (!flex_container_is_multi_line(container)) {
            line = fx_line_cross_size(container, vertical);
        } else {
            /* A multi-line container's line is step 8's `Otherwise` arm plus step 9, whose definite cross
               size on an INLINE cross axis is a width; css-sizing-3 makes a stretch-fit width definite and a
               shrink-to-fit one not, and nothing here tells the two apart. */
            if (!vertical)
                DFAILF("%s: a stretched item of a MULTI-LINE `column` flex container needs its line's cross "
                       "size, and css-flexbox-1 §9.4 \"Cross Size Determination\"' step 9 asks whether the "
                       "container's WIDTH is definite: css-sizing-3 §2 \"Terminology\"' definite size admits "
                       "the stretch-fit sizing of non-replaced blocks and not a shrink-to-fit width. BUILD "
                       "that test over core/layout/used_value.c's width rules (§9.8 \"Definite and Indefinite "
                       "Sizes\" adds the flex cases), then run step 8's per-line walk on the inline axis with "
                       "§4's anonymous item measured at fit-content",
                       box_subject(item, nbuf, sizeof nbuf));
            line = fx_multi_line_cross_size_of(container, item);
        }
        surround = css_px_add(fx_cross_border_padding(item, vertical), fx_cross_margins(item, vertical));
        return css_px_max(css_px_sub(line, surround), css_px(0.0));
    }
    /* Step 11's second arm: "Otherwise, the used cross size is the item's hypothetical cross size", step 7's
       inner size (step 8.2 adds the outer step). The container is passed because step 7's available space is
       an operand of the inline arm's fit-content formula. */
    return fx_hypothetical_cross(container, item, vertical);
}
