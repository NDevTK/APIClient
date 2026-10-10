/* css-flexbox-1 §9.3 "Main Size Determination" and §9.7 "Resolving Flexible Lengths" over one flex line. See
   flex_line.h. */
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
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/intrinsic_block_size.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/replaced_element.h"
#include "core/layout/used_value.h"

/* One member of §6 "Flex Lines"' line, with every operand §9.7 reads and the two values it writes.
   `el` is NULL for §4's anonymous child text sequence: that box is unstyleable, so every property answers its
   `Initial:` line (`flex-grow` 0, `flex-shrink` 1, `flex-basis` `auto` over `width: auto`, `min-width` `auto`,
   `max-width` `none`, CSS 2.1 §8's edges zero); the NULL arms below are that.
   Every size is a content box: §9.7 floors "its content-box size at zero" and §7.2.3 "The flex-basis property"
   sizes "the content box, unless otherwise specified, such as by box-sizing", so css-sizing-3 §3.3 "Box Edges
   for Sizing: the box-sizing property" converts each operand once on read. `surround` is css-sizing-3 §2.2
   "Intrinsic Size Contributions"' outer step, kept apart because §9.7 asks for outer sizes in three places
   (step 1, initial and remaining free space). */
typedef struct {
    lxb_dom_element_t *el;
    /* The item's identity: the element's node, or the first text node of an anonymous item's sequence. A
       node the child list holds, so no two items on the line can share one. */
    lxb_dom_node_t    *first;
    CssPx  base;          /* §9.2 "Line Length Determination"'s flex base size */
    CssPx  hypothetical;  /* §9.2's hypothetical main size */
    CssPx  surround;      /* the item's main-axis margin + border + padding */
    CssPx  min_main;      /* the used min main size, css-flexbox-1 §4.5's where `min-width` is `auto` */
    CssPx  max_main;      /* read only where `has_max` */
    bool   has_max;
    double grow;
    double shrink;
    CssPx  target;        /* §9.7's target main size */
    double violation;     /* §9.7's step 4d adjustment, `clamped size - unclamped size`; its sign is the kind */
    bool   frozen;
} FlItem;

/* The magnitude of one of §9.7's signed free-space values: "If the magnitude of this value is less than the
   magnitude of the remaining free space, use this as the remaining free space" compares magnitudes but keeps
   the selected value's sign, which an `fabs` at the assignment would drop. */
static double fl_magnitude(double v)
{
    return v < 0.0 ? -v : v;
}

/* Keyword-valued properties only. Length-valued ones (sizes, margins, `flex-basis`) go through
   `css_computed_length_is` (core/css/css_computed_value.h), whose `CssPx` keeps a `50vw` value's environment
   fact; the text entry refuses them. */
static bool fl_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;
    char nbuf[160];

    DCHECKF(v != NULL,
            "%s, property `%s`: the cascade produced no computed value for a property this engine models — "
            "every one of them is in lexbor's registry with an initial value, so the last layer always answers",
            box_subject(el, nbuf, sizeof nbuf), name);
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* css-writing-modes-4 §7.2 "Dimensional Mapping"'s physical names of the four main-axis properties read here,
   for a vertical (`vertical`) or horizontal main axis. css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings"
   decides which physical axis (`flex_container_axis_is_vertical`, core/layout/flex_item.h); §7.2 says which
   property states a size on it: "The height properties (height, min-height, and max-height) refer to the
   physical height, and the width properties (width, min-width, and max-width) refer to the physical width."
   `overflow` is here because css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items" asks it of the main axis
   ("a flex item whose computed overflow value is non-scrollable"); css-overflow-3 §3.1 "Managing Overflow: the
   overflow-x, overflow-y, and overflow properties" names its longhands physically. `flex-basis` is absent: it
   is stated over the main axis itself (css-flexbox-1 §7.2.3 "The flex-basis property"). */
typedef struct {
    const char *size;      /* `width`      or `height`     */
    const char *min_size;  /* `min-width`  or `min-height` */
    const char *max_size;  /* `max-width`  or `max-height` */
    const char *overflow;  /* `overflow-x` or `overflow-y` */
} FlMainProps;

static FlMainProps fl_main_props(bool vertical)
{
    FlMainProps p;

    p.size     = vertical ? "height"     : "width";
    p.min_size = vertical ? "min-height" : "min-width";
    p.max_size = vertical ? "max-height" : "max-width";
    p.overflow = vertical ? "overflow-y" : "overflow-x";
    return p;
}

/* css-sizing-3 §5.1 "Intrinsic Sizes"' two sizes in this container's main axis. Not core/layout/
   intrinsic_size.h's `IntrinsicInlineSizes`, which is the inline axis; the two coincide only for a `row`
   container in `horizontal-tb`.
   On the block axis it is one number twice: css-sizing-3 §3.2 says at both keywords "for a box's block size,
   unless otherwise specified, this is equivalent to its automatic size", and css-writing-modes-4 §7.2
   "Dimensional Mapping" sends that to CSS 2.1 §10.6 ("the calculation rules in CSS2.1 Section 10.6 are used in
   the block dimension"), i.e. §10.6.3's content height for a blockified item: `block_flow_auto_height`, which
   ignores the declared height. Not `used_value_block_level_content_px`, which answers a declaration:
   css-sizing-3 §5.1 assumes "an auto preferred size in that axis", and §9.2's step 3 substitutes the flex basis
   "in place of its main size". Reading the declaration would make §4.5's specified size suggestion cap itself
   and break `flex-basis: content`. */
typedef struct {
    CssPx min_content;
    CssPx max_content;
} FlMainSizes;

/* The pair for one element item.
   Named residual: §9.2's step 3 ends "If a cross size is needed to determine the main size (e.g. when the flex
   item's main size is in its block axis, or when it has a preferred aspect ratio) and the flex item's cross
   size is auto and not definite, in this calculation use fit-content as the flex item's cross size", and this
   walk states no cross size, so line boxes break at whatever the item's cross-size route answers. Next:
   `line_box_content_height`'s `avail` threaded through CSS 2.1 §10.6.3's walk so a caller can state the
   width, over css-flexbox-1 §9.8 "Definite and Indefinite Sizes"' cross-axis definiteness test. Absence would
   show as a `column` item with an auto, indefinite cross size whose used height changes with its cross-size
   route rather than its content. */
static FlMainSizes fl_measure_element(lxb_dom_element_t *el, bool vertical)
{
    FlMainSizes out;

    if (!vertical) {
        IntrinsicInlineSizes m = intrinsic_inline_sizes(el);

        out.min_content = m.min_content;
        out.max_content = m.max_content;
        return out;
    }
    /* The block arm is core/layout/intrinsic_block_size.h, which states the css-sizing-3 §3.2 derivation, the
       choice of `block_flow_auto_height`, the replaced-element refusal and the writing-mode precondition once
       for both this pass and core/layout/flex_intrinsic_size.c's §9.9.3 "Flex Item Intrinsic Size
       Contributions" (the two diverge over declarations and percentages, not this measurement). Both fields
       carry the one block size. */
    out.min_content = out.max_content = intrinsic_block_size(el);
    return out;
}

/* The pair for §4's anonymous child text sequence, which has no element. An anonymous block container of a
   text sequence "establishes an inline formatting context and thus contains only inline-level boxes" (CSS 2.2
   §9.2.1 "Block-level elements and block boxes"), so the block arm is CSS 2.1 §10.6.3's first item, "the bottom
   edge of the last line box", via core/layout/intrinsic_block_size.h's run entry. `style` is the container: the
   box inherits "from the enclosing non-anonymous box" (CSS 2.2 §9.2.1.1 "Anonymous block boxes") and "the
   anonymous item's box is unstyleable" (§4), as `intrinsic_inline_run_sizes` pairs it. */
static FlMainSizes fl_measure_run(lxb_dom_element_t *container, BlockFlowRun run, bool vertical)
{
    FlMainSizes out;

    if (!vertical) {
        IntrinsicInlineSizes m = intrinsic_inline_run_sizes(container, run);

        out.min_content = m.min_content;
        out.max_content = m.max_content;
        return out;
    }
    out.min_content = out.max_content = intrinsic_block_run_size(container, run);
    return out;
}

/* The item's main-axis border and padding, as used values: a percentage padding resolves against the flex
   container's content box, determined before §9.3 runs (core/layout/intrinsic_size.c's intrinsic pass
   resolves it against zero, css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes").
   The axis picks the pair (css-writing-modes-4 §7.2 "Dimensional Mapping": CSS 2.1 §10.3's rules "apply to the
   inline size … and to the inline-start and inline-end margins, padding, and border", §10.6's to the block
   ones). `used_value_border_widths_px` writes CSS 2.1 §8.5.1's top/right/bottom/left order, so the vertical
   pair is indices 0 and 2 and the horizontal 1 and 3. */
static CssPx fl_main_border_padding(lxb_dom_element_t *el, bool vertical)
{
    CssPx b[4];

    if (el == NULL) return css_px(0.0);
    used_value_border_widths_px(el, b);
    if (vertical)
        return css_px_add(css_px_add(b[0], b[2]),
                          css_px_add(used_value_px(el, "padding-top"), used_value_px(el, "padding-bottom")));
    return css_px_add(css_px_add(b[1], b[3]),
                      css_px_add(used_value_px(el, "padding-left"), used_value_px(el, "padding-right")));
}

/* The item's main-axis margins. An `auto` one is not answered: css-flexbox-1 §9.5 "Main-Axis Alignment"'s first
   step gives it the remaining free space ("If the remaining free space is positive and at least one main-axis
   margin on this line is auto, distribute the free space equally among these margins"), which §9.7 has not yet
   produced. core/layout/used_value.c refuses it by name on whichever physical axis is the main one. */
static CssPx fl_main_margins(lxb_dom_element_t *el, bool vertical)
{
    if (el == NULL) return css_px(0.0);
    if (vertical) return css_px_add(used_value_px(el, "margin-top"), used_value_px(el, "margin-bottom"));
    return css_px_add(used_value_px(el, "margin-left"), used_value_px(el, "margin-right"));
}

/* One declared main-axis size of `el` as a content-box extent: true when the property states a size, false
   when it states its initial keyword `initial` (the only keyword admitted). `name` is the caller's because
   `flex-basis` has one spelling on both axes (css-flexbox-1 §7.2.3 "The flex-basis property"); `fl_main_props`
   picks the others.
   Not core/layout/intrinsic_size.h's `intrinsic_declared_sizing_px`: that answers false for a percentage
   (css-sizing-3 §5.2.1 drops a cyclic one from a contribution), while here the basis `inner_main` is
   determined and the percentage is definite. §3.3's conversion uses the used edges, as
   `fl_main_border_padding` does. */
static bool fl_declared_main_px(lxb_dom_element_t *el, CssPx inner_main, const char *name, const char *initial,
                                bool vertical, CssPx *out)
{
    CssLength len;
    CssPx declared;
    char nbuf[160];

    if (el == NULL) return false;
    len = css_computed_length(el, name);
    if (len.kind == CSS_LENGTH_KEYWORD) {
        DCHECKF(strcmp(len.keyword, initial) == 0,
                "%s: `%s` computed to the keyword `%s` rather than to its initial value `%s`. "
                "css-sizing-3 §3.2 \"Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch, "
                "min-content, max-content, and fit-content values\" adds keywords this engine records no "
                "computed-value "
                "rule for, and css-flexbox-1 §7.2.3 \"The flex-basis property\" gains every one of them by "
                "reference — \"The flex-basis property hereby also gains these new keywords, as its values are "
                "defined by reference to <'width'>\". BUILD "
                "css-sizing-3 §3.2's keyword arm where a `width` of the same "
                "shape is answered (core/layout/intrinsic_size.c and core/layout/used_value.c both refuse the "
                "identical keyword on the identical grammar) and this routes through it unchanged",
                box_subject(el, nbuf, sizeof nbuf), name, len.keyword, initial);
        return false;
    }
    /* An absolute length is the answer and is split off: `css_length_resolve_pct` serves the two kinds that
       carry a percentage (css-values-4 §10.11 "Computed Value": "with used value time information, a math
       function always simplifies down to a single numeric value") and refuses an absolute length at its
       entry, as core/layout/intrinsic_size.c's margin arm and every other caller split it. */
    if (len.kind == CSS_LENGTH_ABSOLUTE) {
        declared = len.px;
    } else {
        DCHECKF(len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED,
                "%s: `%s` computed to a kind outside the four core/css/css_length.h declares. The keyword arm "
                "above and the absolute arm beside this one take the other two, so this is a four-way "
                "dispatch's `default:` over an enum this engine OWNS — a guard, and not a shape "
                "css-sizing-3 §3.2 \"Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch, "
                "min-content, max-content, and fit-content values\" admits and this file has not built",
                box_subject(el, nbuf, sizeof nbuf), name);
        /* css-flexbox-1 §7.2.3: "percentage values of flex-basis are resolved against the flex item's containing
           block (i.e. its flex container)". */
        /* The basis is the container's inner size on the main axis: §7.2.3 says so for `flex-basis`, and CSS 2.1
           §10.2 "Content width: the 'width' property" and §10.5 "Content height: the 'height' property" resolve a
           size percentage against the containing block (the flex container) on its own axis. A `height`
           percentage over an indefinite height behaves as `auto` (css-sizing-3 §3.2.1 "“Behaving as auto”")
           and never reaches here. */
        declared = css_length_resolve_pct(len, inner_main);
    }
    if (fl_computed_is(el, "box-sizing", "border-box"))
        declared = css_px_max(css_px_sub(declared, fl_main_border_padding(el, vertical)), css_px(0.0));
    *out = css_px_max(declared, css_px(0.0));
    return true;
}

/* css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items"' used value of a main-axis automatic minimum size,
   which css-sizing-3 §3.2 hands to this module ("Unless otherwise defined by the relevant layout module,
   however, it resolves to a used value of 0"). §4.5: "the used value of a main axis automatic minimum size on a
   flex item whose computed overflow value is non-scrollable is its content-based minimum size; for main-axis
   scroll containers the automatic minimum size is zero, as usual." The gate is css-overflow-3 §3.1's
   membership ("The visible and clip values are known as the non-scrollable values") over the main axis's own
   longhand (`overflow-y` for a `column` container).
   The non-replaced arm is the one reached (the replaced one is refused in `fl_fill`): "Use the larger of the
   content size suggestion and the transferred size suggestion (if one exists), capped by the specified size
   suggestion (if one exists)", "clamped by the maximum main size if it's definite"; with no aspect ratio the
   transferred suggestion does not exist.
   §9.9.3 "Flex Item Intrinsic Size Contributions" must not read this: WPT's flex-container-min-content-001.html
   (rows with `flex: 0 1 0.2ch` over 1ch content, and rows 14 and 15) expects §9.9.3's clamp without a §4.5
   floor, which core/layout/flex_intrinsic_size.c records at its step four. */
static CssPx fl_automatic_minimum_main(lxb_dom_element_t *el, CssPx inner_main, FlMainSizes measured,
                                       bool vertical)
{
    FlMainProps prop = fl_main_props(vertical);
    CssPx out = measured.min_content, v;

    /* §4's anonymous item has `overflow: visible` (non-scrollable) and no declaration to cap or clamp. */
    if (el == NULL) return out;
    if (!fl_computed_is(el, prop.overflow, "visible") && !fl_computed_is(el, prop.overflow, "clip"))
        return css_px(0.0);
    /* "capped by the specified size suggestion (if one exists)", whose own definition is "If the item's
       preferred main size is definite and not automatic, then the specified size suggestion is that size." */
    if (fl_declared_main_px(el, inner_main, prop.size, "auto", vertical, &v)) out = css_px_min(out, v);
    /* "In either case, the size is clamped by the maximum main size if it's definite." */
    if (fl_declared_main_px(el, inner_main, prop.max_size, "none", vertical, &v)) out = css_px_min(out, v);
    return out;
}

/* The two arms of §9.2 "Line Length Determination"' step 3 this component reaches:
     - arm A, "If the item has a definite used flex basis, that's the flex base size" (shared with
       core/layout/flex_intrinsic_size.c);
     - arm E, "Otherwise, size the item into the available space using its used flex basis in place of its main
       size, treating a value of content as max-content", for a container with a determined main size.
   flex_intrinsic_size.c takes arm C instead ("the flex container is being sized under a min-content or
   max-content constraint"). They diverge only at percentages, indefinite there and definite here, so they must
   not share one function. `flex-basis: auto` is §7.1 "The flex Shorthand"'s: "When specified on a flex item,
   the auto keyword retrieves the value of the main size property as the used flex-basis. If that value is
   itself auto, then the used value is content." */
static CssPx fl_flex_base_size(lxb_dom_element_t *el, CssPx inner_main, FlMainSizes measured, bool vertical)
{
    FlMainProps prop = fl_main_props(vertical);
    CssLength basis;
    CssPx v;

    /* §4's anonymous item: `flex-basis: auto` over an `auto` main size is `content` (§7.1), which §9.2's
       last arm sizes at max-content. */
    if (el == NULL) return measured.max_content;
    basis = css_computed_length(el, "flex-basis");
    if (basis.kind == CSS_LENGTH_KEYWORD) {
        if (strcmp(basis.keyword, "content") == 0) return measured.max_content;
        /* §7.1's `auto` reads "the main size property", the one the axis names (`fl_main_props`). */
        if (strcmp(basis.keyword, "auto") == 0) {
            if (fl_declared_main_px(el, inner_main, prop.size, "auto", vertical, &v)) return v;
            return measured.max_content;
        }
    }
    if (fl_declared_main_px(el, inner_main, "flex-basis", "auto", vertical, &v)) return v;
    return measured.max_content;
}

/* §9's algorithm is one ordered list continued from §9.1 to §9.6, so step numbers here are global (§9.1's
   list opens at 1 and later sections continue it; §9.8 "Definite and Indefinite Sizes" says "see step 11" for
   §9.4's fifth item). §9.2's items are steps 2, 3 and 4.
   §9.2's step 3 closes "The hypothetical main size is the item's flex base size clamped according to its used
   min and max main sizes (and flooring the content box size at zero)", applied in CSS 2.1 §10.4 "Minimum and
   maximum widths: 'min-width' and 'max-width'"' order (max first, then min), so a `min-width` above a
   `max-width` wins. The flex base size itself is not clamped ("When determining the flex base size, the item's
   min and max main sizes are ignored (no clamping occurs)"), so both are kept on `FlItem`. */
static CssPx fl_clamp_main(const FlItem *it, CssPx v)
{
    if (it->has_max) v = css_px_min(v, it->max_main);
    v = css_px_max(v, it->min_main);
    return css_px_max(v, css_px(0.0));
}

/* One item's operands, read once: §9.7 reads each several times, and nothing can change the document between
   its steps, so §9.2's work happens here and the loop reads only this struct. */
static void fl_fill(FlItem *it, CssPx inner_main, FlMainSizes measured, bool vertical)
{
    FlMainProps prop = fl_main_props(vertical);
    CssPx v;
    char nbuf[160];

    if (it->el != NULL) {
        ReplacedElement rep = replaced_element_of(it->el);

        /* css-flexbox-1 §9.2's arm B ("If the flex item has … a preferred aspect ratio, a used flex basis of
           content, and a definite cross size, then the flex base size is calculated from its used cross size
           and the flex item's aspect ratio") and §4.5's transferred size suggestion ("If the item has a
           preferred aspect ratio and its preferred cross size is definite, then the transferred size
           suggestion is that size … converted through the aspect ratio") both need a ratio and a definite
           cross size, and are refused together.
           With an `auto` cross size neither applies, and §4.5's replaced arm ("Use the smaller of the content
           size suggestion and the transferred size suggestion (if one exists)") equals the non-replaced arm,
           so such an item is not refused. The ratio exists (core/layout/replaced_element.h); what is missing is
           css-flexbox-1 §9.8 "Definite and Indefinite Sizes"' cross-axis definiteness test (not
           `flex_cross_size_used_item_cross`, which answers indefinite sizes too). */
        /* The declaration tested is on the cross axis: `width` for a `column` container, `height` for a `row`
           one. */
        if (rep.replaced && rep.has_ratio &&
            !css_computed_length_is(it->el, vertical ? "width" : "height", "auto"))
            DFAILF("%s: this REPLACED flex item has a preferred aspect ratio and a declared cross size, so "
                   "css-flexbox-1 §9.2 \"Line Length Determination\"' arm B is its flex base size and §4.5 "
                   "\"Automatic Minimum Size of Flex Items\"' TRANSFERRED SIZE SUGGESTION is part of its "
                   "automatic minimum — and neither is built. Both are stated over the same two facts: \"If "
                   "the flex item has … a preferred aspect ratio, a used flex basis of content, and a definite "
                   "cross size, then the flex base size is calculated from its used cross size and the flex "
                   "item's aspect ratio\", and \"If the item has a preferred aspect ratio and its preferred "
                   "cross size is definite, then the transferred size suggestion is that size (clamped by its "
                   "minimum and maximum cross sizes if they are definite), converted through the aspect "
                   "ratio\". THE RATIO IS NOT WHAT IS MISSING — core/layout/replaced_element.h answers "
                   "css-images-3 §4.1 \"Object-Sizing Terminology\"'s natural aspect ratio for this element "
                   "already. WHAT IS MISSING IS NOT THE USED CROSS SIZE EITHER, WHICH IS THE THIRD REMEDY "
                   "THIS ARM HAS HAD AND THE SECOND TO BE RETIRED — each was correct when written and each is "
                   "left here because a reader re-derives them in the same order. The first said to build "
                   "§9.4 whole; its steps 7 and 8 are built (core/layout/flex_cross_size.h) and "
                   "core/layout/block_flow.c CALLS them. The second said to build §9.4's STEP 11, and that is "
                   "built too (`flex_cross_size_used_item_cross`). WHAT BOTH SENTENCES ASKED FOR IS A USED "
                   "SIZE AND WHAT THESE TWO RULES ASK FOR IS A DEFINITE ONE, which is a different question "
                   "that css-flexbox-1 §9.8 \"Definite and Indefinite Sizes\" owns: step 11 answers for an "
                   "INDEFINITE cross size as readily as for a definite one — its second arm is the item's "
                   "hypothetical cross size — so calling it here would turn this arm's own `definite cross "
                   "size` condition into a test that is always true and would run §9.2's arm B on every "
                   "replaced item with a "
                   "ratio. BUILD §9.8's DEFINITENESS TEST FOR THE CROSS AXIS, whose own sentences are the "
                   "algorithm (\"If a single-line flex container has a definite cross size, the automatic "
                   "preferred outer cross size of any stretched flex items is the flex container's inner "
                   "cross size … and is considered definite\"), and then both rules become a call",
                   box_subject(it->el, nbuf, sizeof nbuf));
        it->grow = flex_item_flexibility_factor(it->el, "flex-grow");
        it->shrink = flex_item_flexibility_factor(it->el, "flex-shrink");
    } else {
        /* §4's anonymous item: css-flexbox-1 §7.2.1 "The flex-grow property" and §7.2.2 "The flex-shrink
           property" give `Initial:` values of `0` and `1`. */
        it->grow = 0.0;
        it->shrink = 1.0;
    }
    it->surround = css_px_add(fl_main_border_padding(it->el, vertical), fl_main_margins(it->el, vertical));
    it->base = fl_flex_base_size(it->el, inner_main, measured, vertical);
    it->has_max = fl_declared_main_px(it->el, inner_main, prop.max_size, "none", vertical, &v);
    it->max_main = it->has_max ? v : css_px(0.0);
    /* css-sizing-3 §3.1.2 "Minimum Size Properties: the min-width and min-height properties" gives both an
       initial `auto`, which css-sizing-3 §3.2 hands to css-flexbox-1 §4.5, so the false arm is that keyword. */
    if (!fl_declared_main_px(it->el, inner_main, prop.min_size, "auto", vertical, &v))
        v = fl_automatic_minimum_main(it->el, inner_main, measured, vertical);
    it->min_main = v;
    it->hypothetical = fl_clamp_main(it, it->base);
    it->target = it->base;
    it->frozen = false;
}

/* The `horizontal-tb` requirement. Every edge and size read here is physical (css-writing-modes-4 §7.2
   "Dimensional Mapping": "The height properties … refer to the physical height, and the width properties …
   refer to the physical width"), and `fl_main_props` composes them with §5.1 over
   `flex_container_axis_is_vertical`. That settles names but not rules: §7.2 says "the calculation rules in
   CSS2.1 Section 10.3 are used for the inline dimension measurements" and "the calculation rules in CSS2.1
   Section 10.6 are used in the block dimension", and this component picks between §10.3's intrinsic pair and
   §10.6's `block_flow_auto_height` by whether the main axis is vertical. In `horizontal-tb` vertical is the
   block dimension; in a vertical mode they come apart, so this is where the second question is asked (as at
   core/layout/flex_cross_size.c's entry). What is missing then is a dimension-parameterised reading of
   intrinsic_size.c (lays line boxes out horizontally) and block_flow.c (stacks downward).
   It is asked of each item too: an item in a perpendicular mode is a css-writing-modes-4 §7.3 "Orthogonal
   Flows" box, for which §9.2's step 3 has its own arm ("lay the item out using the rules for a box in an
   orthogonal flow"), refused likewise in core/layout/flex_intrinsic_size.c. */
static void fl_require_horizontal_tb(lxb_dom_element_t *el, const char *what)
{
    char nbuf[160], wbuf[64];

    if (fl_computed_is(el, "writing-mode", "horizontal-tb")) return;
    DFAILF("%s, computed `writing-mode` `%s`: this %s's BLOCK dimension is not the vertical one, so "
           "css-writing-modes-4 §7.2 \"Dimensional Mapping\" does not put CSS 2.1 §10.6's rules on the "
           "vertical axis and §10.3's on the horizontal one — and this component chooses between exactly "
           "those two measurements by asking which physical axis css-flexbox-1 §5.1 \"Flex Flow Direction: "
           "the flex-direction property\" sent the MAIN axis to. In a vertical writing mode the two "
           "questions come apart: a `column` container's main axis lies along the HORIZONTAL while its block "
           "dimension is the vertical one, so `fl_measure_element`'s block arm would run CSS 2.1 §10.6.3's "
           "walk for a "
           "size CSS 2.1 §10.3 owns, and its inline arm would measure a `row` container's main size with "
           "the other "
           "dimension's rules. THE REMEDY THAT STOOD HERE SAID TO BUILD css-writing-modes-4 §7.4 "
           "\"Flow-Relative Mappings\" AND THE CITATION WAS MIS-AIMED: §7.4 is the rule for WHOSE writing "
           "mode a flow-relative question is read against, the TABLE is §6.4 \"Abstract-to-Physical "
           "Mappings\", and core/css/css_logical.c holds it with its dimension rows exported as "
           "`css_logical_axis_is_vertical`. A SECOND REMEDY IS RETIRED AND IS WHAT THIS ARM NOW STANDS ON: "
           "it said the BLOCK-DIMENSION reading of §9.2 \"Line Length Determination\"' step 3 and §4.5 "
           "\"Automatic Minimum Size of Flex Items\" was absent and that building it would make this an "
           "axis parameter. IT IS BUILT and this is that parameter's remaining precondition rather than the "
           "old refusal. WHAT IS ABSENT NOW IS THE DIMENSION-PARAMETERISED READING OF THE TWO WALKS "
           "THEMSELVES — core/layout/intrinsic_size.c measures an INLINE pair by laying out line boxes "
           "horizontally and core/layout/block_flow.c stacks children DOWNWARD, so each is one physical "
           "direction's algorithm wearing a logical name. BUILD either one over the other direction and this "
           "refusal becomes a third value of the same parameter",
           box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "writing-mode", wbuf, sizeof wbuf),
           what);
}

/* §9.3's step 5, "Collect flex items into flex lines", for the arm that needs no breaking: "If the flex
   container is single-line, collect all the flex items into a single flex line." §4's item list ("Each in-flow
   child of a flex container becomes a flex item", text sequences wrapped once each) is enumerated as
   core/layout/flex_intrinsic_size.c enumerates it. `out` may be NULL to count only; it is one walk run twice,
   because classifying a text node walks its whole sequence and a half-built list would have to carry that. */
static size_t fl_collect(lxb_dom_element_t *container, CssPx inner_main, bool vertical, FlItem *out)
{
    /* §4's child list is css-display-3 §2.5 "Box Generation: the none and contents keywords"' spliced
       sequence ("the element must be treated as if it had been replaced in the element tree by its contents"),
       from core/layout/box_tree.h; `->first_child` would hand the classification a `contents` element and
       drop its children from every sum. Every member is a node (css-display-3 §2.3 "Generating Marker Boxes:
       the list-item keyword" pairs `list-item` with `flow` or `flow-root` only). */
    lxb_dom_node_t *c = box_tree_child_node(box_tree_first_child(container));
    size_t n = 0;
    char nbuf[160];

    while (c != NULL) {
        lxb_dom_node_t *next = box_tree_child_node(box_tree_next_sibling(container, box_tree_child_of_node(c)));
        FlexItemChildKind kind = flex_item_child_kind(container, c);
        FlItem it;

        if (kind == FLEX_ITEM_CHILD_NONE) { c = next; continue; }
        memset(&it, 0, sizeof it);
        switch (kind) {
        case FLEX_ITEM_CHILD_TEXT: {
            lxb_dom_node_t *end = flex_item_text_sequence_end(container, c);
            BlockFlowRun seq;

            /* §4's sequence as core/layout/block_flow.h's half-open run, whose `after` is a box-tree position;
               `->prev` is NULL at the start of a `contents` element's children and would start the run at the
               container's start. */
            seq.after = box_tree_child_node(box_tree_prev_sibling(container, box_tree_child_of_node(c)));
            seq.end = end;
            next = end;
            it.el = NULL;
            it.first = c;
            if (out != NULL) fl_fill(&it, inner_main, fl_measure_run(container, seq, vertical), vertical);
            break;
        }
        case FLEX_ITEM_CHILD_ELEMENT:
            next = box_tree_child_node(box_tree_next_sibling(container, box_tree_child_of_node(c)));
            it.el = lxb_dom_interface_element(c);
            it.first = c;
            /* css-flexbox-1 §4.4 "Collapsed Items" gives this item a second layout round and this component runs
               one. §9.4 "Cross Size Determination"' step 10: "If any flex items have visibility: collapse, note
               the cross size of the line they're in as the item's strut size, and restart layout from the
               beginning. In this second layout round, when collecting items into lines, treat the collapsed
               items as having zero main size." core/layout/flex_cross_size.c refuses the same item for the
               strut its step 8 owes; what is missing is step 10's two rounds. */
            if (flex_item_is_collapsed(it.el))
                DFAILF("%s: this flex item's computed `visibility` is `collapse`, so css-flexbox-1 §4.4 "
                       "\"Collapsed Items\" makes it a COLLAPSED FLEX ITEM and §9.4 \"Cross Size "
                       "Determination\"' step 10 restarts the whole of "
                       "css-flexbox-1 §9 \"Flex Layout Algorithm\" for its container with every "
                       "collapsed item at ZERO main size. This component runs §9.3 \"Main Size "
                       "Determination\" once, so the used main size it would answer is the FIRST round's — "
                       "the one a browser throws away — and every other item on the line would be flexed "
                       "against free space this item is still occupying. BUILD §9.4's STEP 10, WHICH OWNS "
                       "BOTH ROUNDS — its step 8 is built (core/layout/flex_cross_size.h) and supplies "
                       "the STRUT SIZE the second one carries, and refuses this same item at its own walk "
                       "for that reason, so what is absent is the two ROUNDS and neither measurement",
                       box_subject(it.el, nbuf, sizeof nbuf));
            if (out != NULL) {
                fl_require_horizontal_tb(it.el, "FLEX ITEM");
                fl_fill(&it, inner_main, fl_measure_element(it.el, vertical), vertical);
            }
            break;
        case FLEX_ITEM_CHILD_NONE:
            /* Unreachable by the guard above; asserted because the tail would count a zeroed record that §9.7
               would give a share of free space. The arm exists for `-Wswitch`. */
            DFAIL("css-flexbox-1 §4's classification answered FLEX_ITEM_CHILD_NONE for a child this walk had "
                  "already decided to collect. The loop skips that value before it reaches this switch, so "
                  "two readings of one child list disagreed — which means the child list changed under the "
                  "walk");
            break;
        /* No `default` arm: `-Wswitch` forces a new `FlexItemChildKind` to be handled, since §9.7's line sums
           must not silently drop a member. */
        }
        DCHECK(next != c,
               "css-flexbox-1 §4's item walk did not advance past the child it had just collected, so this "
               "walk would put the same flex item on the line for ever. A text sequence always contains at "
               "least the node it was delimited from, and an element item always advances by one sibling");
        if (out != NULL) out[n] = it;
        n++;
        c = next;
    }
    return n;
}

/* §9.7's step 3 and its "as for initial free space, above": "Sum the outer sizes of all items on the line,
   and subtract this from the flex container's inner main size. For frozen items, use their outer target main
   size; for other items, use their outer flex base size." One function for both, as the section states them. */
static CssPx fl_free_space(const FlItem *items, size_t n, CssPx inner_main)
{
    CssPx used = css_px(0.0);
    size_t i;

    for (i = 0; i < n; i++)
        used = css_px_add(used, css_px_add(items[i].frozen ? items[i].target : items[i].base,
                                           items[i].surround));
    return css_px_sub(inner_main, used);
}

/* css-flexbox-1 §9.7 "Resolving Flexible Lengths" over one line, ending "Set each item's used main size to its
   target main size" (`target` on return).
   The loop is bounded by the section, not a count: "This freezes at least one item, ensuring that the loop
   makes progress and eventually terminates" holds because the freeze step's arms are exhaustive over the
   total violation's sign, and the assert below makes that note falsifiable. An iteration cap would be a bound.
   The used flex factor is chosen once for the line (step 1: "If the sum is less than the flex container's inner
   main size, use the flex grow factor for the rest of this algorithm; otherwise, use the flex shrink factor"),
   so the direction cannot flip mid-line. */
static void fl_resolve(FlItem *items, size_t n, CssPx inner_main)
{
    CssPx sum_hypothetical = css_px(0.0), initial_free;
    bool grow;
    size_t i;

    for (i = 0; i < n; i++)
        sum_hypothetical = css_px_add(sum_hypothetical,
                                      css_px_add(items[i].hypothetical, items[i].surround));
    grow = sum_hypothetical.px < inner_main.px;

    /* Step 2: "Each item in the flex line has a target main size, initially set to its flex base size. Each
       item is initially unfrozen" (in `fl_fill`), then "Freeze, setting its target main size to its
       hypothetical main size… any item that has a flex factor of zero; if using the flex grow factor: any
       item that has a flex base size greater than its hypothetical main size; if using the flex shrink factor:
       any item that has a flex base size smaller than its hypothetical main size." The zero test is on the
       used factor, so `flex: 0 1 auto` is inflexible only on a growing line. */
    for (i = 0; i < n; i++) {
        double factor = grow ? items[i].grow : items[i].shrink;
        bool inflexible = factor == 0.0 ||
                          (grow && items[i].base.px > items[i].hypothetical.px) ||
                          (!grow && items[i].base.px < items[i].hypothetical.px);

        if (inflexible) {
            items[i].target = items[i].hypothetical;
            items[i].frozen = true;
        }
    }

    /* Step 3, "Calculate initial free space.", computed once and kept: step 4b's less-than-one arm scales
       the initial free space, not the remaining one. */
    initial_free = fl_free_space(items, n, inner_main);

    for (;;) {
        CssPx remaining;
        double sum_factors = 0.0, denom = 0.0, total_violation = 0.0;
        size_t unfrozen = 0, froze = 0;

        /* Step 4a: "Check for flexible items. If all the flex items on the line are frozen, exit this loop." */
        for (i = 0; i < n; i++)
            if (!items[i].frozen) { unfrozen++; sum_factors += grow ? items[i].grow : items[i].shrink; }
        if (unfrozen == 0) break;

        /* Step 4b: "Calculate the remaining free space as for initial free space, above. If the sum of the
           unfrozen flex items' flex factors is less than one, multiply the initial free space by this sum. If
           the magnitude of this value is less than the magnitude of the remaining free space, use this as the
           remaining free space." A magnitude comparison keeping the scaled value's sign, not a clamp: WPT's
           max-width-violation.html expects a `flex: 0.8 0 0` column to take 520px of 800px (640px where an
           absolutely positioned sibling is not an item). */
        remaining = fl_free_space(items, n, inner_main);
        if (sum_factors < 1.0) {
            CssPx scaled = css_px_scale(initial_free, sum_factors);

            if (fl_magnitude(scaled.px) < fl_magnitude(remaining.px)) remaining = scaled;
        }

        /* Step 4c: "If the remaining free space is non-zero, distribute it proportional to the flex factors."
           The denominator is over unfrozen items, and in the shrink arm it is the scaled factor: "multiply its
           flex shrink factor by its inner flex base size". "Inner" is why `base` is a content box here. */
        if (remaining.px != 0.0) {
            for (i = 0; i < n; i++) {
                if (items[i].frozen) continue;
                denom += grow ? items[i].grow : (items[i].shrink * items[i].base.px);
            }
            if (denom != 0.0) {
                for (i = 0; i < n; i++) {
                    double share;

                    if (items[i].frozen) continue;
                    if (grow) {
                        share = items[i].grow / denom;
                        items[i].target = css_px_add(items[i].base, css_px_scale(remaining, share));
                    } else {
                        share = (items[i].shrink * items[i].base.px) / denom;
                        /* "Set the item's target main size to its flex base size minus a fraction of the absolute value
                           of the remaining free space proportional to the ratio. Note this may result in a negative
                           inner main size; it will be corrected in the next step." */
                        items[i].target = css_px_sub(items[i].base,
                                                     css_px_scale(remaining,
                                                                  remaining.px < 0.0 ? -share : share));
                    }
                }
            }
        }

        /* Step 4d: "Fix min/max violations. Clamp each non-frozen item's target main size by its used min and
           max main sizes and floor its content-box size at zero. If the item's target main size was made
           smaller by this, it's a max violation. If the item's target main size was made larger by this, it's
           a min violation." The clamped value is kept: WPT's total-min-max-violation-zero.html has two
           opposite violations summing to zero, freezing both at "250px and 50px", not 150px each. */
        for (i = 0; i < n; i++) {
            CssPx clamped;

            if (items[i].frozen) continue;
            clamped = fl_clamp_main(&items[i], items[i].target);
            items[i].violation = clamped.px - items[i].target.px;
            total_violation += items[i].violation;
            items[i].target = clamped;
        }

        /* Step 4e: "Freeze over-flexed items. The total violation is the sum of the adjustments from the
           previous step ∑(clamped size - unclamped size). If the total violation is: Zero — Freeze all items.
           Positive — Freeze all the items with min violations. Negative — Freeze all the items with max
           violations." The per-item adjustment from step 4d is both summed and read for its sign here, so it is
           carried rather than re-derived. */
        for (i = 0; i < n; i++) {
            bool freeze;

            if (items[i].frozen) continue;
            if (total_violation == 0.0) freeze = true;
            else if (total_violation > 0.0) freeze = items[i].violation > 0.0;   /* a MIN violation */
            else freeze = items[i].violation < 0.0;                              /* a MAX violation */
            if (freeze) { items[i].frozen = true; froze++; }
        }
        DCHECK(froze > 0,
               "css-flexbox-1 §9.7 \"Resolving Flexible Lengths\"' freeze step froze NO item, so this loop "
               "would run for ever over a line it can never finish. The section's own note is the invariant "
               "this asserts — \"This freezes at least one item, ensuring that the loop makes progress and "
               "eventually terminates\" — and it holds because its three arms are exhaustive over the sign of "
               "the total violation: zero freezes every item, a POSITIVE total is a sum of adjustments of "
               "which at least one must be positive (a min violation), and a NEGATIVE one the mirror. A pass "
               "that freezes nothing therefore means the violation this loop measured and the violation this "
               "step tested for were computed from different values");
        if (froze == 0) break;
    }
}

CssPx flex_line_used_main_size(lxb_dom_element_t *container, lxb_dom_node_t *item)
{
    FlItem *items;
    size_t n, i;
    CssPx inner_main, out;
    char nbuf[160];
    bool found = false, vertical;

    DCHECK(container != NULL && item != NULL,
           "css-flexbox-1 §9.3 \"Main Size Determination\"'s used main size was asked for with no container "
           "or no item");
    DCHECK(box_tree_parent(box_tree_child_of_node(item)) == container,
           "css-flexbox-1 §9.7 \"Resolving Flexible Lengths\"' used main size was asked for an item that is "
           "not a child of the container it was asked about. §9.7's every sum is over \"all items on the "
           "line\", so a subject drawn from one container and a free space drawn from another is an item "
           "flexed against a rectangle it is not in. "
           "THIS EQUALITY USED TO READ `item->parent == container` AND IS REWRITTEN RATHER THAN DELETED: "
           "css-display-3 §2.5 \"Box Generation: the none and contents keywords\" makes an item on this "
           "container's line need not be a DOM child of it, so the DOM reading refused exactly the items "
           "`fl_collect` now collects, and the box-tree one is the same precondition stated over the tree §4 "
           "is about");
    /* §5.1's mapping, asked first over the whole container: which physical axis is the main one is a fact of
       its `flex-direction` (§9.3 and §9.4 share no step). `fl_measure_element` and `fl_measure_run` measure
       either dimension. */
    vertical = flex_container_axis_is_vertical(container, FLEX_AXIS_MAIN);
    fl_require_horizontal_tb(container, "FLEX CONTAINER");
    /* §5.2 "Flex Line Wrapping: the flex-wrap property"'s multi-line arm. §9.3's step 5 then breaks lines
       ("collect consecutive items one by one until the first time that the next collected item would not fit
       into the flex container's inner main size") over the outer hypothetical main sizes `fl_fill` computes,
       plus §10 "Fragmenting Flex Layout"'s forced breaks; neither is built. */
    if (flex_container_is_multi_line(container))
        DFAILF("%s: this FLEX CONTAINER is MULTI-LINE (css-flexbox-1 §5.2 \"Flex Line Wrapping: the flex-wrap "
               "property\"; §6 \"Flex Lines\" makes `wrap` and `wrap-reverse` one case against `nowrap`), "
               "so css-flexbox-1 §9.3 \"Main Size Determination\"' step 5 must BREAK its items into lines "
               "before §9.7 \"Resolving Flexible Lengths\" can run over any of them — and every free space "
               "§9.7 distributes is per LINE, so answering from one line holding every item would flex each "
               "of them against space the other lines are using. §9.3's own sentence is the algorithm: "
               "\"Otherwise, starting from the first uncollected item, collect consecutive items one by one "
               "until the first time that the next collected item would not fit into the flex container's "
               "inner main size (or until a forced break is encountered, see § 10 Fragmenting Flex Layout). "
               "If the very first uncollected item wouldn't fit, collect just it into the line. For this "
               "step, the size of a flex item is its outer hypothetical main size.\" THE SIZE IT BREAKS ON "
               "IS ALREADY COMPUTED HERE — `fl_fill` produces exactly that outer hypothetical main size for "
               "every item — so what is missing is the BREAKING and §10's forced break, not the measurement",
               box_subject(container, nbuf, sizeof nbuf));

    /* §9.2 "Line Length Determination"'s step 2 for the main axis, first arm: "if that dimension of the flex
       container's content box is a definite size, use that". A `row` container's inline size is CSS 2.1
       §10.3.3's equation (core/layout/used_value.h). A `column` container's declared `height` is §10.6's used
       value; an auto one reaches core/layout/block_flow.c, which answers it with §9.2's last step ("The
       automatic block size of a block-level flex container is its max-content size") through
       `flex_intrinsic_max_content_main_block_size`. So no definiteness gate here.
       It is the content box (§9.2 says "content box"; `used_value_px` would be the border box under
       `border-box`), on the main axis, "that dimension". */
    inner_main = used_value_content_px(container, vertical);

    n = fl_collect(container, inner_main, vertical, NULL);
    DCHECK(n > 0,
           "css-flexbox-1 §9.7 \"Resolving Flexible Lengths\" was asked for an item of a container whose §4 "
           "\"Flex Items\" walk found NO item at all, and the subject of the ask is one of them. §6 \"Flex "
           "Lines\" is what makes that impossible — \"Every line contains at least one flex item, unless the "
           "flex container itself is completely empty\" — so two readings of one child list have disagreed, "
           "which means the list changed under the walk");
    /* The release arm of that assert: `calloc(0, …)` may return NULL and trip the allocation `CHECK` below
       for what is a walk disagreement. Zero is visibly not a used main size (as core/layout/
       flex_intrinsic_size.c chooses). */
    if (n == 0) return css_px(0.0);
    items = (FlItem *)calloc(n, sizeof *items);
    CHECK(items != NULL,
          "allocating css-flexbox-1 §6 \"Flex Lines\"' line for §9.7 \"Resolving Flexible Lengths\" failed. "
          "The line is one record per flex item of ONE container and it is freed before this entry returns, so "
          "this is the physical RAM floor and not a leak: §9.7's sums are over every item on the line at once "
          "and there is no smaller unit of it to resolve");
    (void)fl_collect(container, inner_main, vertical, items);
    fl_resolve(items, n, inner_main);

    out = css_px(0.0);
    for (i = 0; i < n; i++)
        if (items[i].first == item) { out = items[i].target; found = true; break; }
    free(items);
    DCHECKF(found,
            "%s: css-flexbox-1 §4 \"Flex Items\"' walk over this item's own flex container did not produce "
            "the item the used main size was asked for. Every caller establishes that it IS one before asking "
            "— core/layout/used_value.c classifies the box as a flex item from its box parent's `display`, and "
            "core/layout/flex_cross_size.c reaches §4's anonymous item through the same "
            "`flex_item_child_kind` walk this one runs — so the two readings of one child list have "
            "disagreed, and the number this entry would otherwise return is another item's. THE SUBJECT IS A "
            "NODE AND NOT AN ELEMENT for the reason §4 gives: its anonymous block container flex item is \"the "
            "one box a flex container's item list holds that is not an element\", so a TEXT node here is the "
            "FIRST node of a child text sequence and an ELEMENT node is an item in its own right",
            box_subject_node(item, nbuf, sizeof nbuf));
    return out;
}
