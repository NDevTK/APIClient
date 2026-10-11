/* css-flexbox-1 §9.9 "Intrinsic Sizes" over a flex container's own items. See flex_intrinsic_size.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_intrinsic_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/intrinsic_block_size.h"
#include "core/layout/intrinsic_size.h"

/* css-writing-modes-4 §7.3 "Orthogonal Flows"' perpendicular case, refused ("When a box has a writing mode that
   is perpendicular to its containing block it is said to be in, or establish, an orthogonal flow."). §9.9 sums
   along the container's axes, and a perpendicular item's own inline size lies along the container's block axis,
   so it would be a measurement of the other dimension.
   Equal computed values is a sufficient test, not §7.3's: `vertical-rl` against `vertical-lr` is parallel and
   still refused, the safe direction. Widening it to §6.2 "Flow-relative Directions"' parallelism belongs with
   §9.2 "Line Length Determination"'s orthogonal arm ("an English document (horizontal writing mode) containing
   a column flex container containing a vertical Japanese (vertical writing mode) flex item"). */
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

/* One item's css-sizing-3 §5.2 "Intrinsic Contributions" outer contribution in the container's inline axis,
   which is the cross axis here. Not §9.9.3 "Flex Item Intrinsic Size Contributions", whose every sentence is
   about the main axis ("The main-size min-content contribution of a flex item"); §9.9.2 "Flex Container
   Intrinsic Cross Sizes" uses the items' plain "min-content contribution/max-content contribution", the entry
   every block container's child is measured by. */
static IntrinsicInlineSizes fis_item_cross_contribution(lxb_dom_element_t *container, lxb_dom_node_t *child,
                                                        FlexItemChildKind kind, lxb_dom_node_t **next)
{
    /* The kind is the caller's and is not re-asked: classifying a text node walks its whole sequence, so a
       second ask is a second reading of one child list. */
    switch (kind) {
    case FLEX_ITEM_CHILD_TEXT: {
        /* §4's anonymous block container flex item: one CSS 2.2 §9.4.2 "Inline formatting contexts" over the
           sequence (core/layout/intrinsic_size.h's run entry), with zero edges because it is unstyleable (the
           NULL that entry reads as an anonymous box). */
        lxb_dom_node_t *end = flex_item_text_sequence_end(container, child);
        /* §4's sequence as a box-tree sibling range: `BlockFlowRun` reads `after`'s next box-tree sibling, so
           `{ box_tree_prev_sibling(container, child), end }` is `[child, end)` (a NULL `after` meaning the
           container's start). Not `child->prev`, which css-display-3 §2.5 "Box Generation: the none and
           contents keywords"' splice makes NULL inside a `contents` element. The run cannot begin inside an
           inline box: §4 blockifies every item ("if the computed display value of an element's nearest
           ancestor element (skipping display:contents ancestors) is flex or inline-flex, the element's own
           display value is blockified"), which is why CSS 2.2 §9.2.1.1's runs and these share one entry. */
        BlockFlowRun seq;
        IntrinsicInlineSizes run;

        seq.after = box_tree_child_node(box_tree_prev_sibling(container, box_tree_child_of_node(child)));
        seq.end = end;
        run = intrinsic_inline_run_sizes(container, seq);
        *next = end;
        return intrinsic_outer_contribution(NULL, run);
    }
    case FLEX_ITEM_CHILD_ELEMENT: {
        lxb_dom_element_t *item = lxb_dom_interface_element(child);

        fis_require_parallel(container, item);
        *next = box_tree_child_node(box_tree_next_sibling(container, box_tree_child_of_node(child)));
        return intrinsic_outer_contribution(item, intrinsic_inline_sizes(item));
    }
    case FLEX_ITEM_CHILD_NONE:
        break;
    /* No `default` arm: `-Wswitch` makes a new `FlexItemChildKind` a compile failure at every caller, since
       §9.9's arms are over a list and an unclassified member would be dropped silently. */
    }
    {
        IntrinsicInlineSizes none;

        DFAIL("css-flexbox-1 §4's classification answered FLEX_ITEM_CHILD_NONE for a child this walk had "
              "already decided to measure. The caller skips that value before it asks for a contribution, so "
              "two readings of one node disagreed — which means the child list changed under the walk");
        /* The release fall-through contributes nothing: §9.9.2's arms are maxima, so a zero pair invents no
           width. */
        none.min_content = css_px(0.0);
        none.max_content = css_px(0.0);
        *next = box_tree_child_node(box_tree_next_sibling(container, box_tree_child_of_node(child)));
        return none;
    }
}

/* The main axis's three sizing property names and its physical axis, composed once: §9.9.3 and §9.2's step 3
   name no physical property, and css-writing-modes-4 §7.2 "Dimensional Mapping" pins each triple ("The height
   properties (height, min-height, and max-height) refer to the physical height, and the width properties
   (width, min-width, and max-width) refer to the physical width"). `vertical` is the caller's, from
   core/layout/flex_item.h's `flex_container_axis_is_vertical`. `flex-basis` is not on the table (one spelling
   on both axes, css-flexbox-1 §7.2.3 "The flex-basis property"). */
typedef struct {
    const char *size;       /* §9.9.3's "outer preferred size" — §7.1's "main size property" */
    const char *min_size;
    const char *max_size;
    IntrinsicAxis axis;     /* which pair of edges §2.2's outer size and §3.3's conversion read */
} FisMainProps;

static FisMainProps fis_main_props(bool vertical)
{
    FisMainProps p;

    p.size     = vertical ? "height"     : "width";
    p.min_size = vertical ? "min-height" : "min-width";
    p.max_size = vertical ? "max-height" : "max-width";
    p.axis     = vertical ? INTRINSIC_AXIS_VERTICAL : INTRINSIC_AXIS_HORIZONTAL;
    return p;
}

/* css-sizing-3 §5.1 "Intrinsic Sizes"' two sizes in the container's main axis, distinct from
   `IntrinsicInlineSizes` (the inline axis); they coincide only for a `row` container in `horizontal-tb`, as
   core/layout/flex_line.c's `FlMainSizes` states. The cross walk keeps `IntrinsicInlineSizes` correctly:
   §9.9.2 is reached only for a container whose cross axis is its inline axis. */
typedef struct {
    CssPx min_content;
    CssPx max_content;
} FisMainSizes;

/* One item's own css-sizing-3 §5.1 pair in the main axis, which §9.9.3's first step takes "the larger of"
   against a declared size. On the block axis it is one number twice (css-sizing-3 §3.2: "for a box's block
   size, unless otherwise specified, this is equivalent to its automatic size"), from
   core/layout/intrinsic_block_size.h, shared with §9.2's step 3 in the used pass. */
static FisMainSizes fis_measure_item(lxb_dom_element_t *item, bool vertical)
{
    FisMainSizes out;

    if (!vertical) {
        IntrinsicInlineSizes m = intrinsic_inline_sizes(item);

        out.min_content = m.min_content;
        out.max_content = m.max_content;
        return out;
    }
    out.min_content = out.max_content = intrinsic_block_size(item);
    return out;
}

/* The same pair for §4's anonymous child text sequence, which has no element; no edge is added (see
   `fis_child_main_contribution`). */
static FisMainSizes fis_measure_run(lxb_dom_element_t *container, BlockFlowRun run, bool vertical)
{
    FisMainSizes out;

    if (!vertical) {
        IntrinsicInlineSizes m = intrinsic_inline_run_sizes(container, run);

        out.min_content = m.min_content;
        out.max_content = m.max_content;
        return out;
    }
    out.min_content = out.max_content = intrinsic_block_run_size(container, run);
    return out;
}

/* css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer step in the main axis, through
   core/layout/intrinsic_size.h's two entries. The horizontal pair entry applies §2.2's floor ("if the ideal
   max-content contribution would be smaller than the min-content contribution (e.g. due to the use of negative
   margins), the effective max-content contribution is floored by the min-content contribution"); the block
   axis has one size, so its single-extent entry has nothing to floor. */
static FisMainSizes fis_outer_main(lxb_dom_element_t *el, bool vertical, FisMainSizes inner)
{
    FisMainSizes out;

    if (!vertical) {
        IntrinsicInlineSizes p;

        p.min_content = inner.min_content;
        p.max_content = inner.max_content;
        p = intrinsic_outer_contribution(el, p);
        out.min_content = p.min_content;
        out.max_content = p.max_content;
        return out;
    }
    out.min_content = intrinsic_outer_block_contribution(el, inner.min_content);
    out.max_content = intrinsic_outer_block_contribution(el, inner.max_content);
    return out;
}

/* css-flexbox-1 §9.2 "Line Length Determination"'s flex base size of `item`, in the container's main axis
   (`vertical` says which physical axis). `measured` is the item's own §5.1 pair. Two arms are reachable, routed
   by §7.1 "The flex Shorthand", where `auto` is defined: "When specified on a flex item, the auto keyword
   retrieves the value of the main size property as the used flex-basis. If that value is itself auto, then the
   used value is content."
     - A definite used flex basis: "If the item has a definite used flex basis, that's the flex base size".
     - `content` is the max-content main size in both halves: "This is typically equivalent to the max-content
       size", and §9.2's last arm, "treating a value of content as max-content".
   §9.2's arm C ("the flex container is being sized under a min-content or max-content constraint", answering
   the item's size under that constraint) would give the min-content size under a min-content constraint. WPT's
   flex-container-min-content-001.html expects 3ch at its sixteenth row for a `flex: 1 0 auto` item measuring
   1ch/3ch, which only a max-content flex base size gives through §9.9.3's floor; the `row` halves of that file
   and its max-content sibling agree with this reading. The oracle outranks arm C until a case separates them
   the other way. */
static CssPx fis_flex_base_size(lxb_dom_element_t *item, bool vertical, FisMainSizes measured)
{
    FisMainProps prop = fis_main_props(vertical);
    CssLength basis = css_computed_length(item, "flex-basis");
    CssPx v;

    if (basis.kind == CSS_LENGTH_ABSOLUTE) {
        /* §3.3's conversion, as for a declared main size: "flex-basis determines the size of the content box,
           unless otherwise specified, such as by box-sizing". The property is `flex-basis` on either axis;
           `prop.axis` tells the entry which edges to subtract. Its initial keyword is `auto`. */
        if (intrinsic_declared_sizing_px(item, prop.axis, "flex-basis", CSS_KEYWORD_AUTO, &v)) return v;
        DFAIL("css-flexbox-1 §7.2.3 \"The flex-basis property\"' computed value was an absolute length and "
              "core/layout/intrinsic_size.h's §3.3 conversion declined it. The two read the same cascade entry "
              "one call apart, so they cannot disagree about its shape");
        return measured.max_content;
    }
    if (basis.kind == CSS_LENGTH_PERCENTAGE || basis.kind == CSS_LENGTH_CALCULATED) {
        /* §7.2.3: "percentage values of flex-basis are resolved against the flex item's containing block (i.e.
           its flex container) and if that containing block's size is indefinite, the used value for flex-basis
           is content." Here the container's main size is the number being produced, so not definite
           (css-sizing-3 §2 "Terminology": "a size that can be determined without performing layout"). */
        return measured.max_content;
    }
    DCHECK(basis.kind == CSS_LENGTH_KEYWORD,
           "`flex-basis` computed to none of the shapes css-flexbox-1 §7.2.3 \"The flex-basis property\"' "
           "`Value:` line of `content | <'width'>` admits");
    switch (basis.keyword) {
    case CSS_KEYWORD_CONTENT:
        return measured.max_content;
    case CSS_KEYWORD_AUTO:
        /* §7.1's `auto` reads "the value of the main size property", the one the axis names (`fis_main_props`),
           as in core/layout/flex_line.c. */
        if (intrinsic_declared_sizing_px(item, prop.axis, prop.size, CSS_KEYWORD_AUTO, &v)) return v;
        return measured.max_content;
    case CSS_KEYWORD_STRETCH:
    case CSS_KEYWORD_MIN_CONTENT:
    case CSS_KEYWORD_MAX_CONTENT:
    case CSS_KEYWORD_FIT_CONTENT:
        DFAILF("`flex-basis` computed to the keyword `%s`. css-flexbox-1 §7.2.3 \"The flex-basis property\" gives "
               "it `content | <'width'>`, and css-sizing-3 §3.2 \"Sizing Values: the <length-percentage>, auto | "
               "none, stretch, min-content, max-content, and fit-content values\" adds the rest by reference — "
               "\"The flex-basis property hereby also gains these new keywords, as its values are defined by "
               "reference to <'width'>\". BUILD §3.2's keyword arm where a `width` of the same shape is answered "
               "(core/layout/intrinsic_size.c refuses the identical keyword on the identical grammar) and this "
               "routes through it unchanged",
               css_keyword_name(basis.keyword));
        return measured.max_content;
    default:
        DFAILF("`flex-basis` computed to keyword %u, outside css-flexbox-1 §7.2.3's set", (unsigned)basis.keyword);
        return measured.max_content;
    }
}

/* css-flexbox-1 §9.9.3 "Flex Item Intrinsic Size Contributions" for one element flex item, as the outer pair
   §9.9.1's arms sum and maximise, in the container's main axis (`fis_main_props`). Its four steps, in order:
   "The main-size min-content contribution of a flex item is the larger of its outer min-content size and outer
   preferred size if that is not an automatic size", likewise max-content, then "each contribution is capped by
   the item's flex base size if the item is not growable, floored by the item's flex base size if the item is
   not shrinkable, and then further clamped by the item's min/max main size."
   The cap and floor are two `if`s: `flex: 0 0 <length>` takes both and is pinned to its base size. Every term is
   on the content box and §2.2's outer step runs last: both sides of each comparison belong to one item, so its
   edges cancel (§9.9.1.1 says "outer flex base size" where it means it; §9.9.3 does not). */
/* Named residual: §4.5 "Automatic Minimum Size of Flex Items" is not built here. Not covered: an automatic
   minimum main size (the main axis's min size property computing to `auto`, which css-sizing-3 §3.2 hands to
   §4.5: "the used value of a main axis automatic minimum size on a flex item whose computed overflow value is
   non-scrollable is its content-based minimum size"); the clamp reads it as zero. Next: §4.5's suggestions over
   this item (content size suggestion is the pair's min-content half; specified size suggestion is the main size
   read above; the transferred one needs core/layout/replaced_element.h's natural aspect ratio). Absence shows
   as a container whose intrinsic main size is below Chrome's for an item whose flex base size or max main size
   sits under its min-content size. WPT's flex-container-min-content-001.html expects the zero floor's 0.2ch
   for a `flex: 0 1 0.2ch` item of 1ch min-content. */
static FisMainSizes fis_item_main_contribution(lxb_dom_element_t *container, lxb_dom_element_t *item,
                                               bool vertical)
{
    FisMainProps prop = fis_main_props(vertical);
    FisMainSizes measured, out;
    CssPx base, v;

    fis_require_parallel(container, item);
    measured = fis_measure_item(item, vertical);
    out = measured;

    /* Step one: "the larger of its outer min-content size and outer preferred size if that is not an
       automatic size". The preferred size is the main size property (`fis_main_props`); css-sizing-3 §3.2
       "Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and
       fit-content values"' `auto` "specifies an automatic size", which is the false arm. */
    if (intrinsic_declared_sizing_px(item, prop.axis, prop.size, CSS_KEYWORD_AUTO, &v)) {
        out.min_content = css_px_max(out.min_content, v);
        out.max_content = css_px_max(out.max_content, v);
    }
    /* Steps two and three. */
    base = fis_flex_base_size(item, vertical, measured);
    if (flex_item_flexibility_factor(item, "flex-grow") <= 0.0) {
        out.min_content = css_px_min(out.min_content, base);
        out.max_content = css_px_min(out.max_content, base);
    }
    if (flex_item_flexibility_factor(item, "flex-shrink") <= 0.0) {
        out.min_content = css_px_max(out.min_content, base);
        out.max_content = css_px_max(out.max_content, base);
    }
    /* Step four: "and then further clamped by the item's min/max main size", max first and min last, so a
       `min-width` above a `max-width` wins: CSS 2.1 §10.4 "Minimum and maximum widths: 'min-width' and
       'max-width'" horizontally and §10.7 "Minimum and maximum heights: 'min-height' and 'max-height'"
       vertically, in the same order. The `auto` arm is the residual above. */
    if (intrinsic_declared_sizing_px(item, prop.axis, prop.max_size, CSS_KEYWORD_NONE, &v)) {
        out.min_content = css_px_min(out.min_content, v);
        out.max_content = css_px_min(out.max_content, v);
    }
    if (intrinsic_declared_sizing_px(item, prop.axis, prop.min_size, CSS_KEYWORD_AUTO, &v)) {
        out.min_content = css_px_max(out.min_content, v);
        out.max_content = css_px_max(out.max_content, v);
    }
    return fis_outer_main(item, vertical, out);
}

/* css-flexbox-1 §9.9.1 "Flex Container Intrinsic Main Sizes" in the container's main axis (`vertical`: a
   `row` container for an inline size, a `column` one for a block size), as `fis_main_sizes` below.
   "an implementation is conformant to CSS Flexible Box Layout if it conforms to either the Ideal Algorithm or
   the Web-compatible Algorithm"; this is §9.9.1.2 "Web-compatible Intrinsic Sizing Algorithm: Max-content Size
   and Min-content Single-line Size": "For the max-content size of a flex container, take the sum of the
   max-content contributions of all the non-collapsed flex items in the flex container", likewise single-line
   min-content. §9.9.1.1's Ideal Algorithm "is not web compatible". A multi-line min-content size is §9.9.1.3
   "Multi-line Min-content Algorithm", a maximum: "the largest min-content contribution of all the
   non-collapsed flex items". No flex line is needed.
   Collapsed items are skipped here, not in §9.9.2 (§4.4 "Collapsed Items": a collapsed item "leaves behind a
   strut that keeps the flex line's cross-size stable"). An anonymous item is never collapsed and its
   contribution is its measurement: unstyleable, it has `visibility: visible` and `flex: 0 1 auto` over
   `width: auto`, whose cap is itself. */
/* One child of the container as a main-axis contribution, with `*counts` false where "non-collapsed" excludes
   it. Excluded rather than zeroed: css-sizing-3 §2.2 permits a negative outer contribution ("negative values
   for margin properties are allowed", CSS 2.1 §8.3 "Margin properties"), and a zero would floor §9.9.1.3's
   maximum. */
static FisMainSizes fis_child_main_contribution(lxb_dom_element_t *container, lxb_dom_node_t *child,
                                                FlexItemChildKind kind, bool vertical,
                                                lxb_dom_node_t **next, bool *counts)
{
    /* The kind is the caller's and is not re-asked (see the cross walk). */
    *counts = true;
    switch (kind) {
    case FLEX_ITEM_CHILD_TEXT: {
        /* §4's anonymous block container flex item, whose §9.9.3 contribution is its measurement (see the
           §9.9.1 banner above) with zero edges (the NULL the entry reads as an anonymous box). */
        lxb_dom_node_t *end = flex_item_text_sequence_end(container, child);
        BlockFlowRun seq;

        /* The box-tree member before the sequence, as in the cross walk: a position in css-display-3 §2.5's
           spliced sequence, never a DOM sibling. */
        seq.after = box_tree_child_node(box_tree_prev_sibling(container, box_tree_child_of_node(child)));
        seq.end = end;
        *next = end;
        return fis_outer_main(NULL, vertical, fis_measure_run(container, seq, vertical));
    }
    case FLEX_ITEM_CHILD_ELEMENT: {
        lxb_dom_element_t *item = lxb_dom_interface_element(child);

        *next = box_tree_child_node(box_tree_next_sibling(container, box_tree_child_of_node(child)));
        if (flex_item_is_collapsed(item)) {
            FisMainSizes none;

            *counts = false;
            none.min_content = css_px(0.0);
            none.max_content = css_px(0.0);
            return none;
        }
        return fis_item_main_contribution(container, item, vertical);
    }
    case FLEX_ITEM_CHILD_NONE:
        break;
    /* No `default` arm, for the cross walk's reason. */
    }
    {
        FisMainSizes none;

        DFAIL("css-flexbox-1 §4's classification answered FLEX_ITEM_CHILD_NONE for a child this walk had "
              "already decided to measure. The caller skips that value before it asks for a contribution, so "
              "two readings of one child list disagreed — which means the child list changed under the walk");
        /* The release fall-through is excluded rather than zeroed, keeping §9.9.1.3's maximum honest. */
        *counts = false;
        none.min_content = css_px(0.0);
        none.max_content = css_px(0.0);
        *next = box_tree_child_node(box_tree_next_sibling(container, box_tree_child_of_node(child)));
        return none;
    }
}

static FisMainSizes fis_main_sizes(lxb_dom_element_t *el, bool vertical, bool multi_line)
{
    /* §4's child list is css-display-3 §2.5 "Box Generation: the none and contents keywords"' spliced
       sequence, enumerated as core/layout/flex_line.c collects the line. Every member is a node
       (css-display-3 §2.3 "Generating Marker Boxes: the list-item keyword" pairs `list-item` with `flow` or
       `flow-root` only). */
    lxb_dom_node_t *c = box_tree_child_node(box_tree_first_child(el));
    FisMainSizes out;
    bool any = false;

    /* An empty container answers zero (§6 "Flex Lines": "unless the flex container itself is completely
       empty"). `any` exists because §9.9.1.3's maximum over negative contributions can be below zero, which a
       maximum seeded at zero would hide. */
    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    while (c != NULL) {
        lxb_dom_node_t *next = box_tree_child_node(box_tree_next_sibling(el, box_tree_child_of_node(c)));
        FlexItemChildKind kind = flex_item_child_kind(el, c);
        FisMainSizes one;
        bool counts;

        if (kind == FLEX_ITEM_CHILD_NONE) { c = next; continue; }
        one = fis_child_main_contribution(el, c, kind, vertical, &next, &counts);
        DCHECK(next != c,
               "css-flexbox-1 §4's item walk did not advance past the child it had just measured, so this walk "
               "would measure the same flex item for ever. A text sequence always contains at least the node "
               "it was delimited from, and an element item always advances by one sibling");
        if (counts) {
            out.max_content = css_px_add(out.max_content, one.max_content);
            if (!multi_line) out.min_content = css_px_add(out.min_content, one.min_content);
            else if (!any) out.min_content = one.min_content;
            else out.min_content = css_px_max(out.min_content, one.min_content);
            any = true;
        }
        c = next;
    }
    return out;
}

/* §9.9.2 "Flex Container Intrinsic Cross Sizes" for a container whose cross axis is its inline axis (`column`
   or `column-reverse`). Both reachable arms are maxima over items: "The min-content/max-content cross size of a
   single-line flex container is the largest min-content contribution/max-content contribution (respectively)
   of its flex items", and for a multi-line column "The min-content cross size is the largest min-content
   contribution among all of its flex items" ("This heuristic effectively assumes a single flex line, in order
   to guarantee that the min-content size is smaller than the max-content size.").
   Collapsed items are not skipped: §9.9.2 does not say "non-collapsed", and §4.4 gives a collapsed item a
   cross-axis strut. */
static IntrinsicInlineSizes fis_cross_sizes(lxb_dom_element_t *el, bool multi_line)
{
    lxb_dom_node_t *c = box_tree_child_node(box_tree_first_child(el));
    IntrinsicInlineSizes out;
    bool any = false;
    char nbuf[160];

    /* An empty container's maximum over no items is zero (§6 "Flex Lines"). */
    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    while (c != NULL) {
        lxb_dom_node_t *next = box_tree_child_node(box_tree_next_sibling(el, box_tree_child_of_node(c)));
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
    /* §9.9.2's multi-line column max-content arm is the one part needing flex lines; refused after the walk
       so the min-content half runs and the message names the container. An empty multi-line container is
       zero either way, so it is not refused. */
    if (multi_line && any)
        DFAILF("%s: css-flexbox-1 §9.9.2 \"Flex Container Intrinsic Cross Sizes\" gives a MULTI-LINE COLUMN "
               "flex container a max-content cross size this component cannot compute, and it is the only arm "
               "of §9.9 reachable from an intrinsic INLINE size that is stated over flex lines rather than "
               "over items: \"The max-content cross size is the sum of the flex line cross sizes resulting "
               "from sizing the flex container under a cross-axis max-content constraint, using the largest "
               "max-content cross-size contribution among the flex items as the available space in the cross "
               "axis for each of the flex items during layout.\" THE MIN-CONTENT HALF IS ALREADY THE ANSWER "
               "ABOVE — §9.9.2 makes it \"the largest min-content contribution among all of its flex items\", "
               "a maximum over items with no line in it. §9.3 \"Main Size Determination\"'s step 5 breaks "
               "lines (core/layout/flex_line.h's `flex_line_members`) and §9.4 \"Cross Size Determination\"' "
               "step 8 sizes them on a BLOCK cross axis (core/layout/flex_cross_size.c). What is missing is "
               "step 8 on this container's INLINE cross axis, with §4's anonymous item measured at "
               "fit-content and each item's available space the largest max-content contribution, as the "
               "sentence above states",
               box_subject(el, nbuf, sizeof nbuf));
    return out;
}

IntrinsicInlineSizes flex_intrinsic_inline_sizes(lxb_dom_element_t *el)
{
    IntrinsicInlineSizes out;
    FisMainSizes main;

    DCHECK(el != NULL, "css-flexbox-1 §9.9's intrinsic sizes were asked for with no element");
    /* §5.1's mapping run backwards (see flex_intrinsic_size.h), asked first: §9.9.1 and §9.9.2 share no step. */
    if (flex_container_main_axis(el) == FLEX_MAIN_AXIS_BLOCK)
        return fis_cross_sizes(el, flex_container_is_multi_line(el));

    /* §9.9.1 on the horizontal axis, which for this container is both main and inline ("has the same
       orientation as the inline axis of the current writing mode"), so converting `FisMainSizes` to
       `IntrinsicInlineSizes` is sound; a `column` container returned above. */
    main = fis_main_sizes(el, false, flex_container_is_multi_line(el));
    out.min_content = main.min_content;
    out.max_content = main.max_content;
    return out;
}

CssPx flex_intrinsic_max_content_main_block_size(lxb_dom_element_t *el)
{
    char nbuf[160];

    DCHECK(el != NULL,
           "css-flexbox-1 §9.9.1's max-content main size was asked for with no element");
    /* §5.1's mapping asserted, not dispatched on: this entry answers the main size, so §5.1 only checks the
       caller came to the block axis; a `row` container's main size is `flex_intrinsic_inline_sizes`'. */
    DCHECKF(flex_container_main_axis(el) == FLEX_MAIN_AXIS_BLOCK,
            "%s: this FLEX CONTAINER's main axis is its INLINE axis (css-flexbox-1 §5.1 \"Flex Flow Direction: "
            "the flex-direction property\": a `row` container's main axis \"has the same orientation as the "
            "inline axis of the current writing mode\"), so the max-content MAIN size asked for here is not a "
            "block size and this entry would measure it along the wrong dimension. "
            "`flex_intrinsic_inline_sizes` is the entry for it, and it reaches the same §9.9.1 walk with its "
            "other axis",
            box_subject(el, nbuf, sizeof nbuf));
    /* No multi-line arm: §9.9.1 sends only a multi-line min-content size to §9.9.1.3 ("For the min-content
       size of a multi-line flex container, see §9.9.1.3"), and §9.9.1.2's max-content sentence has no line
       condition. The real `multi_line` is passed so the unread min-content half stays correct.
       The min-content half is not returned: only css-sizing-3 §3.2's `min-content` keyword on a `min-height`
       would ask, and the cascade has no computed-value rule for it. */
    return fis_main_sizes(el, true, flex_container_is_multi_line(el)).max_content;
}
