/* css-sizing-3 §5.1's intrinsic inline sizes over a box's own content. See intrinsic_size.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/box_subject.h"
#include "core/layout/containment.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_intrinsic_size.h"
#include "core/layout/grid_track.h"
#include "core/layout/flex_item.h"
#include "core/layout/inline_run_walk.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/layout_job.h"   /* the intrinsic size record of the job holding the thread */
#include "core/layout/phrasing_break.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_box.h"
#include "core/layout/table_grid.h"
#include "core/layout/table_width.h"
#include "core/layout/text_run.h"
#include "core/layout/used_value.h"

static char *is_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

static bool is_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = is_computed(el, name);
    bool same = strcmp(v, kw) == 0;

    free(v);
    return same;
}

/* ---- One side of an inline box: one property table, two questions ----------------------------------------
   The three properties of one side are written once (CSS 2 §8.1 "Box dimensions": padding, border, margin),
   for both css-sizing-3 §2.2 "Intrinsic Size Contributions"' outer size ("auto margins are treated as zero")
   and CSS 2.2 §9.4.2 "Inline formatting contexts"' line ("horizontal margins, borders, and padding are
   respected between these boxes").
   The questions differ on a percentage, which css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized
   Boxes" answers both ways: in an intrinsic contribution the containing block's width is the output ("a cyclic
   percentage is resolved against zero for determining intrinsic size contributions"); for a line box, filled
   at a determined width, "Otherwise, the percentage is resolved against the containing block's size". Its note
   says these rules "specify the previously-undefined behavior of this cyclic case" CSS 2.1 §8.4 leaves
   undefined.
   Neither entry asks whether the percentage is cyclic: every box this walk reaches has the measured box as its
   containing block (`is_child` returns before an out-of-flow child), and §5.2.1's other "otherwise" bullet
   ("introduced due to a block-axis size other than a minimum size on the containing block") cannot apply to
   these width-relative properties. */
/* The four sides, indexed by physical axis (`IntrinsicAxis`, the caller's answer; see intrinsic_size.h) and
   then by `trailing` (bottom on the vertical axis). Consumers sum each pair, so which flow-relative start maps
   to which side does not matter here. The vertical row reuses the same arithmetic: every rule below (§8.3's
   `auto` margin, §5.2.1's zero basis, §8.4's padding clamp, §3.3's `border-box` subtraction) is stated of the
   property, not the axis. */
static const char *const IS_EDGE[2][2][3] = {
    { { "margin-left",  "padding-left",  "border-left-width"   },
      { "margin-right", "padding-right", "border-right-width"  } },
    { { "margin-top",    "padding-top",    "border-top-width"    },
      { "margin-bottom", "padding-bottom", "border-bottom-width" } },
};
enum { IS_EDGE_MARGIN, IS_EDGE_PADDING, IS_EDGE_BORDER };

/* The border width is the same on both sides of the split: css-backgrounds-3 §3.3 "Line Thickness: the
   border-width properties"' computed value is "absolute length, snapped as a border width", with no percentage
   or `auto`. */
static CssPx is_edge_border_px(lxb_dom_element_t *el, const char *name)
{
    CssLength len = css_computed_length(el, name);

    DCHECKF(len.kind == CSS_LENGTH_ABSOLUTE,
            "`%s` computed to something that is not an absolute length. css-backgrounds-3 §3.3 \"Line "
            "Thickness: the border-width properties\" gives every arm of that derivation an absolute result — "
            "0 for a `none`/`hidden` style, one of the three keywords' thicknesses, the absolutized length "
            "otherwise — so a percentage or a keyword here is a rule that did not run. core/layout/"
            "used_value.c's own surround asserts the same thing about the same six properties",
            name);
    return len.px;
}

/* css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"'s surround at one side: padding and border
   width at §5.2.1's zero basis, without the margin. Shared by §2.2's outer size (which adds the margin) and
   §3.3's `border-box` conversion (which subtracts this: "the padding and border of the box, but not its
   margins"). The subtraction is cited, not quoted, as in core/layout/table_column_width.c: the editions word
   the floor differently. */
static CssPx is_intrinsic_pad_border_px(lxb_dom_element_t *el, IntrinsicAxis axis, bool trailing)
{
    const char *const *side = IS_EDGE[axis][trailing ? 1 : 0];
    CssLength padding = css_computed_length(el, side[IS_EDGE_PADDING]);
    CssPx pad;

    /* §8.4's <padding-width> has no keyword, so the split is two-way. */
    if (padding.kind == CSS_LENGTH_ABSOLUTE) {
        pad = padding.px;
    } else {
        DCHECKF(padding.kind == CSS_LENGTH_PERCENTAGE || padding.kind == CSS_LENGTH_CALCULATED,
                "`%s` computed to a keyword. CSS 2.1 §8.4 \"Padding properties\"' <padding-width> grammar is a "
                "length or a percentage and nothing else",
                side[IS_EDGE_PADDING]);
        pad = css_length_resolve_pct(padding, css_px(0.0));
    }
    /* The floor is the property's range ("values for padding values cannot be negative"), reachable only
       through a math function: css-values-4 §9.1 "Numeric Functions" says "numeric functions returning
       out-of-range values never cause a declaration to become invalid" and clamps "at used value time
       otherwise". §5.2.1's zero basis is what makes `calc(50% - 10px)` negative here, and `calc(10px - 20px)`
       arrives absolute, so the clamp covers both arms. `css_px_max` keeps the environment facts, as in
       core/layout/used_value.c's padding arm. Margins are not clamped ("negative values for margin properties
       are allowed"). */
    return css_px_add(css_px_max(pad, css_px(0.0)), is_edge_border_px(el, side[IS_EDGE_BORDER]));
}

/* Question one: css-sizing-3 §5.2.1's intrinsic contribution at one side, where a percentage is cyclic and
   resolves against zero. `static`, so no used-value site can reach its zero. */
static CssPx is_intrinsic_edge_px(lxb_dom_element_t *el, IntrinsicAxis axis, bool trailing)
{
    const char *const *side = IS_EDGE[axis][trailing ? 1 : 0];
    CssLength margin = css_computed_length(el, side[IS_EDGE_MARGIN]);
    CssPx sum;

    /* §2.2's `auto` margin as zero, as CSS 2.1 §10.3.1 "Inline, non-replaced elements" does for the used value
       ("A computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of '0'"); the only
       keyword §8.3's <margin-width> admits. */
    if (margin.kind == CSS_LENGTH_KEYWORD) {
        DCHECKF(margin.keyword == CSS_KEYWORD_AUTO,
                "`%s` computed to the keyword `%s`. CSS 2.1 §8.3 \"Margin properties\"' <margin-width> grammar "
                "admits a length, a percentage and `auto` and nothing else, so a fourth form is a declaration "
                "lexbor's own validation should have dropped",
                side[IS_EDGE_MARGIN], css_keyword_name(margin.keyword));
        sum = css_px(0.0);
    } else if (margin.kind == CSS_LENGTH_ABSOLUTE) {
        /* An absolute length carries no percentage, so it is the contribution as is. It does not go through
           `css_length_resolve_pct`, css-values-4 §10.11 "Computed Value"'s used-time simplification for the
           two percentage-carrying kinds, which refuses an absolute length (used_value.c and table_width.c
           split the same way). */
        sum = margin.px;
    } else {
        DCHECKF(margin.kind == CSS_LENGTH_PERCENTAGE || margin.kind == CSS_LENGTH_CALCULATED,
                "`%s` computed to none of the three shapes CSS 2.1 §8.3 \"Margin properties\" admits",
                side[IS_EDGE_MARGIN]);
        /* §5.2.1 resolves the percentage against zero, not the whole value (a cyclic preferred size is instead
           "treated … as that property's initial value"), so `margin-left: calc(10px + 50%)` contributes 10px.
           One basis serves both percentage-carrying kinds (css-values-4 §10.11; §5.6 "Mixing Percentages and
           Dimensions"). */
        sum = css_length_resolve_pct(margin, css_px(0.0));
    }
    return css_px_add(sum, is_intrinsic_pad_border_px(el, axis, trailing));
}

/* Question two: §5.2.1's "Otherwise, the percentage is resolved against the containing block's size", which
   is CSS 2.1 §8.3's and §8.4's ordinary basis and so core/layout/used_value.h's `used_value_px` (§10.3's
   per-box margin rules, §8.4's basis and clamp, §10.1's chain). The basis exists at the call: used values are
   derived per read from the flow's own tree. */
CssPx used_inline_box_edge_px(lxb_dom_element_t *el, bool trailing)
{
    /* The horizontal row, named: this is a line box's question, and a line runs along the inline axis, horizontal
       in the one writing mode laid out here. */
    const char *const *side = IS_EDGE[INTRINSIC_AXIS_HORIZONTAL][trailing ? 1 : 0];
    CssPx sum;

    DCHECK(el != NULL, "CSS 2.2 §9.4.2's line-box edge was asked for with no element");
    sum = used_value_px(el, side[IS_EDGE_MARGIN]);
    sum = css_px_add(sum, used_value_px(el, side[IS_EDGE_PADDING]));
    return css_px_add(sum, is_edge_border_px(el, side[IS_EDGE_BORDER]));
}

/* css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes"' cyclic percentage on one property of
   a replaced child, refused: CSS 2.1 §10.2 "Content width: the 'width' property", §10.4 "Minimum and maximum
   widths: 'min-width' and 'max-width'" and §8.4 resolve it against the containing block's width, which is this
   walk's output. The property name is passed in so the message, written once and reached from a loop, names
   it (as used_value.c's crashes name the `display`). */
static void is_require_acyclic(lxb_dom_element_t *el, const char *name)
{
    CssLength len = css_computed_length(el, name);

    if (len.kind != CSS_LENGTH_PERCENTAGE && len.kind != CSS_LENGTH_CALCULATED) return;
    DFAILF("css-sizing-3 §5.2.1 \"Intrinsic Contributions of Percentage-Sized Boxes\": this REPLACED child's "
           "`%s` is a PERCENTAGE or a calculation carrying one, and it resolves against the containing block's "
           "width — which for the box this walk is measuring is the number this walk is being run to produce. "
           "CSS 2.1 §10.2, §10.4 and §8.4 each state that case and each leaves it \"undefined in CSS 2.1\"; "
           "§5.2.1 is what defines it, so this is a SPECIFIED answer this engine has not built and not an "
           "undefined one. WHAT §5.2.1 SAYS SPLITS THE FIVE PROPERTIES AND THAT SPLIT IS THE WHOLE OF THE "
           "WORK. For a `min-width`, a `padding-top` or a `padding-bottom` it is one number — its summary "
           "table gives a cyclic percentage on a min size property, a margin or a padding the value zero for "
           "BOTH contributions — so those three are blocked only by WHERE the substitution may live: "
           "`used_value_content_px` is core/layout/used_value.h's shared entry, and core/layout/line_box.c, "
           "core/html/html_image.c and core/frame/viewport.c all read it from OUTSIDE any intrinsic pass, "
           "where the same percentage is not cyclic — so a zero pushed down there would answer a question "
           "those three did not ask. For a "
           "`width` or a `max-width` on a REPLACED box the two contributions genuinely DIFFER — the "
           "max-content contribution treats the whole value as that property's INITIAL value, the min-content "
           "contribution resolves it against zero. "
           "THE ITEM CARRIES A PAIR NOW AND THAT RETIRES NONE OF THE FIVE, WHICH IS WHAT THIS MESSAGE USED TO "
           "SAY IT WOULD. It said `TEXT_RUN_ITEM_ATOMIC` holds ONE `size` and instructed its reader to BUILD "
           "THE PAIR ON THE ITEM FIRST, after which \"this arm supplies §5.2.1's two numbers and the refusal "
           "goes\". The first half was exactly right about the tree and the SECOND HALF WAS WRONG ABOUT THE "
           "WORK, in the direction that would have had a reader delete this crash with nothing behind it. The "
           "item was a BLOCKER for two of the five properties and was never the missing CAPABILITY for any of "
           "them: for `min-width`, `padding-top` and `padding-bottom` it was not even the blocker, since "
           "§5.2.1 gives those ONE number for both contributions and what stops them is WHERE that zero may "
           "live, exactly as the sentence above this one says. "
           "WHAT THE NEXT DIFF BUILDS, and it is the same thing for all five: an entry that runs CSS 2.1 "
           "§10.3.2 \"Inline, replaced elements\" over a CALLER-SUPPLIED substitution rather than over the "
           "element's own computed values. `used_value_content_px` cannot be it — its three non-intrinsic "
           "callers named above ask a question in which these percentages are not cyclic at all — and "
           "`is_replaced_sizes` in this file is the shape that can be: it already composes §10.3.2 through "
           "`used_value_replaced_auto_width_px` for the case where the property states nothing. GREP BOTH "
           "BEFORE BUILDING: neither takes a substituted value today. Then this arm calls it TWICE, once per "
           "contribution, and hands `is_atomic_replaced`'s producer the two different numbers the item can now "
           "hold. Until that entry exists an arm here could only pick one of §5.2.1's answers and report it as "
           "both, which is a wrong shrink-to-fit width rather than an approximate one",
           name);
}

/* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes"' atomic inline-level box, as one item of this run:
   such boxes "participate in their inline formatting context as a single opaque box", with "a soft wrap
   opportunity before and after each replaced element or other atomic inline" (css-text-3 §5.5 "Line Breaking
   Details").
   The size is the margin box, composed here rather than read whole: core/layout/line_box.c's walk uses
   `used_value_margin_edge_px`, whose percentage basis is cyclic here. So it is `is_intrinsic_edge_px` for the
   horizontal edges (§2.2 outer size at §5.2.1's zero basis) plus `used_value_content_px` for CSS 2.1 §10.3.2
   "Inline, replaced elements"' content width, guarded by the refusals below. The edges are computed first:
   `used_value_content_px` reads `padding-left`/`padding-right` itself, and the edge calls have already
   checked they are absolute. */
static void is_atomic_replaced(TextRunMeasure *m, lxb_dom_element_t *el)
{
    /* The five properties §10.3.2's derivation reads that resolve against the containing block's width. The
       horizontal edges are the edge entry's; `margin-top`/`margin-bottom` are width-relative too but read by
       no arm of §10.3.2 or §10.4. `padding-top`/`padding-bottom` are here because the intrinsic-ratio arm
       reads the used height and its vertical surround. */
    static const char *const CYCLIC[] = { "width", "min-width", "max-width", "padding-top", "padding-bottom" };
    CssPx lead, trail, outer;
    size_t i;

    for (i = 0; i < sizeof CYCLIC / sizeof CYCLIC[0]; i++) is_require_acyclic(el, CYCLIC[i]);
    lead = is_intrinsic_edge_px(el, INTRINSIC_AXIS_HORIZONTAL, false);
    trail = is_intrinsic_edge_px(el, INTRINSIC_AXIS_HORIZONTAL, true);
    outer = css_px_add(css_px_add(lead, used_value_content_px(el, false)), trail);
    /* One number as both of css-sizing-3 §2.1's contributions: §5.1 separates them only by a zero or
       infinite containing block in that axis, and no arm of CSS 2.2 §10.3.2 "Inline, replaced elements" reads
       the containing block's inline size (see `is_replaced_sizes`). The edges resolve cyclic percentages to
       zero, one answer for both ("For the min size properties, as well as for margins and paddings (and
       gutters), a cyclic percentage is resolved against zero for determining intrinsic size contributions").
       §5.2.1's replaced rule for a cyclic `width`/`max-width` would differ, and is refused above. */
    text_run_measure_add_atomic(m, el, outer, outer);
}

/* What `is_child` left for its caller, as core/layout/line_box.c's `lb_child` answers it: nothing more, a text
   node whose characters are the next items, an inline box whose opening edge it emitted and whose box-tree
   children are next, or "not yet" for an atomic inline whose own sizes are not ready, with nothing added. Both
   classify the same inline formatting context and must agree on which children are in it; a block-level child
   is unreachable from either, because core/layout/block_flow.h's run ends before one at any depth. */
typedef enum { IS_CHILD_PLACED = 0, IS_CHILD_TEXT, IS_CHILD_INLINE_BOX, IS_CHILD_NOT_YET } IsChildNext;

/* css-sizing-3 §5.2's declared-size step, defined with §9.4.1's stack: §9.4.2's walk applies the same steps
   to an atomic inline-level box, and one definition keeps them agreeing. */
static IntrinsicInlineSizes is_declared_inline_sizes(lxb_dom_element_t *ch, IntrinsicInlineSizes measured);

static IsChildNext is_child(TextRunMeasure *m, lxb_dom_element_t *parent, BoxTreeChild c,
                            lxb_dom_node_t **node)
{
    lxb_dom_node_t *n;
    lxb_dom_element_t *el;
    CssDisplay d;
    bool inline_box;

    /* css-lists-3 §3.5's outside marker box "is placed outside the principal block box", so it contributes
       nothing to the content's css-sizing-3 §5.1 "Intrinsic Sizes", as core/layout/line_box.c's fill skips
       it. */
    if (box_tree_child_is_pseudo(c)) {
        DCHECK(block_flow_child_kind(parent, c) == BLOCK_FLOW_CHILD_OUTSIDE_MARKER,
               "a pseudo-element member of a list item's inline formatting context was classified as something "
               "other than css-lists-3 §3.5's outside marker, which is the one arm core/layout/block_flow.h "
               "answers for it");
        return IS_CHILD_PLACED;
    }
    n = c.node;
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_TEXT:
        /* CSS 2.2 §9.2.2.1 "Anonymous inline boxes": collapsible white space "does not generate any anonymous
           inline boxes". core/layout/block_flow.h answers that for every walk. */
        if (!block_flow_text_child_generates_box(n)) return IS_CHILD_PLACED;
        *node = n;
        return IS_CHILD_TEXT;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* CSS 2.2 §9.2 generates boxes for elements and text only. */
        return IS_CHILD_PLACED;
    case LXB_DOM_NODE_TYPE_ELEMENT:
        break;
    default:
        DFAIL("a node type CSS 2.2 §9.2's box generation does not describe is inside a block container being "
              "measured — the tree this walk iterates holds elements, text, comments, processing instructions "
              "and a doctype, and a CDATA section, a document or a fragment is not a child any parser this "
              "engine runs produces there. Find the writer that inserted it");
        return IS_CHILD_PLACED;
    }
    el = lxb_dom_interface_element(n);
    /* css-display-3 §2.5 "Box Generation: the none and contents keywords": with `display: none`, "The element
       and its descendants generate no boxes or text sequences." */
    if (css_computed_display_is(el, CSS_KEYWORD_NONE)) return IS_CHILD_PLACED;
    /* Out of flow contributes nothing and introduces no break ("out-of-flow boxes and inline box boundaries do
       not introduce a forced line break or soft wrap opportunity in the flow"): CSS 2.2 §9.3.1 takes it out of
       normal flow, as core/layout/line_box.c also skips it. */
    if (css_computed_keyword_is(el, "position", CSS_KEYWORD_ABSOLUTE) ||
        css_computed_keyword_is(el, "position", CSS_KEYWORD_FIXED))
        return IS_CHILD_PLACED;
    if (block_flow_is_floated(el))
        DFAIL("CSS 2.2 §9.5 \"Floats\" takes this child out of the line, and css-sizing-3 §5.2 \"Intrinsic "
              "Contributions\" still counts it: CSS 2.2 §10.3.5 \"Floating, non-replaced elements\" computes "
              "the preferred width \"by formatting the content without breaking lines other than where "
              "explicit line breaks occur\", and a float shortens the line boxes beside it rather than "
              "leaving them alone — §9.4.2's own \"line boxes may vary in "
              "width if available horizontal space is reduced due to floats\". So the contribution is neither "
              "this walk's sum along the line nor a maximum over it, and there is no arm here that is right by "
              "default. BUILD the float's contribution beside the line's content, over "
              "core/layout/float_placement.h's placement");
    /* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes" makes a replaced element an atomic inline-level box
       whatever its `display` ("a non-replaced element with a `display` value of `inline` generates an inline
       box"), so a replaced `inline-block` is answered here too, once. CSS 2.1 §10.3.10 "'Inline-block', replaced
       elements in normal flow" is "Exactly as inline replaced elements.", so one derivation serves both.
       It is asked after the out-of-flow and float tests, CSS 2.2 §9.7 "Relationships between 'display',
       'position', and 'float'"' order as in core/layout/block_flow.c, so a floated `img` is not put in the
       run. */
    if (replaced_element_of(el).replaced) { is_atomic_replaced(m, el); return IS_CHILD_PLACED; }
    d = css_computed_display(el);
    inline_box = css_display_spells(d, CSS_KEYWORD_INLINE);
    /* Freed before either crash: a `DFAIL` compiles out in release and control falls through, so a `free` on
       each side would double free (see `tr_wraps`). */
    if (inline_box) {
        /* HTML §15.3.4 "Phrasing content" first: a `br` changes the partition, not a term. The max-content size
           formats content "without breaking lines other than where explicit line breaks occur" (CSS 2.2
           §10.3.5 "Floating, non-replaced elements"), so a missed `br` reports a paragraph's width as its
           longest line. core/layout/line_box.c asks the same component. */
        switch (phrasing_break_of(el)) {
        case PHRASING_BREAK_FORCED:
            text_run_measure_add_forced_break(m, el);
            return IS_CHILD_PLACED;
        case PHRASING_BREAK_OPPORTUNITY:
            DFAIL("HTML §15.3.4 \"Phrasing content\" gives `wbr` the UA declaration `display-outside: "
                  "break-opportunity`, which puts a SOFT WRAP OPPORTUNITY inside this run — and css-sizing-3 "
                  "§2.1 defines the MIN-CONTENT inline size as the size \"if ALL soft wrap opportunities "
                  "within the box were taken\", so an opportunity this walk cannot see reports a `<wbr>`-split "
                  "word as one unbreakable segment and gives CSS 2.2 §10.3.5's shrink-to-fit a preferred "
                  "minimum width no line of this run has. It leaves the max-content answer alone, since §2.1 "
                  "takes NONE of them — which is why this is one absent capability with two different wrong "
                  "answers rather than one. THE ITEM KIND IS NO LONGER WHAT IS MISSING: text_run.h carries "
                  "`br`'s FORCED break, collected as the U+000A css-text-3 §4 names so [UAX14] LB5 and LB6 "
                  "decide its boundaries. An opportunity needs a DIFFERENT code point and a different pair of "
                  "rules — U+200B ZERO WIDTH SPACE, class ZW, under LB7 `× ZW` and LB8 `ZW SP* ÷` — and it "
                  "needs §5.5's enabling question answered from the `wbr`'s OWN computed `white-space`, which "
                  "is neither of the two cases `tr_opportunity_enabled` implements. BUILD both in "
                  "core/layout/text_run.c, where core/layout/line_box.c's own `wbr` crash names the same two");
            return IS_CHILD_PLACED;
        case PHRASING_BREAK_NONE:
            break;
        }
        /* Replaced elements left above, so this is CSS 2.2 §9.2.2's "non-replaced element with a `display` value of
           `inline`", an inline box whose content is the descent below (an atomic inline has no text of this run
           inside it, so that is settled before the edges). */
        /* §5.5: "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
           flow", and css-text-3 §4.1.1's collapsing crosses them ("even one outside the boundary of the inline
           containing that space, provided both spaces are within the same inline formatting context"). So the
           inline box's text is this run, added in document order, each character styled by its own element. */
        /* The two edges bracket the descent in call order, and the run's break mapping puts each on its
           fragment's line. They are emitted even when zero: an edge's position says which line an empty
           inline box is on. */
        text_run_measure_add_box_edge(m, el, is_intrinsic_edge_px(el, INTRINSIC_AXIS_HORIZONTAL, false));
        *node = n;
        return IS_CHILD_INLINE_BOX;
    }
    /* Every child reaching here is inline-level. For a direct child, CSS 2.2 §9.2.1 "Block-level elements and
       block boxes"' dispatch ("A block container box either contains only block-level boxes or establishes an
       inline formatting context and thus contains only inline-level boxes") plus §9.2.1.1's run delimitation
       guarantee it. For a child inside a `display: inline` box, §9.2.1.1's second paragraph makes an in-flow
       block-level box "a sibling of those anonymous boxes" on the container's stack, enumerated by
       `block_flow_next_block_box`, so the run ends before it. Non-generating nodes, out-of-flow boxes and
       floats returned or crashed above; the assert keeps those tests honest. */
    DCHECK(block_flow_child_kind(parent, c) == BLOCK_FLOW_CHILD_INLINE,
           "a child of a box whose inline formatting context is being measured is not INLINE-LEVEL after this "
           "walk's own tests for a non-generating node, for §9.3.1's out-of-flow box and for §9.5's float have "
           "all let it through — so this run holds a box core/layout/block_flow.h says is not in it, and the "
           "two classifications have come apart. A BLOCK-LEVEL answer here is the sharpest form of that: "
           "CSS 2.2 §9.2.1.1 \"Anonymous block boxes\" puts a block-level box on the CONTAINER's stack, as a "
           "sibling of the anonymous boxes, whether it is a direct child or is reached through an inline box "
           "it breaks — so no run this walk is ever handed may contain one, and whichever of "
           "`bf_content_is_inline` and `block_flow_next_block_box` stopped agreeing with the other is the fix");
    /* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes"' atomic inline-level box that is not replaced, one
       item of this run ("as a single opaque box"; css-text-3 §5.5 "Line Breaking Details": "a soft wrap
       opportunity before and after each replaced element or other atomic inline").
       What reaches here is inline-level, non-replaced and not `display: inline`, which leaves css-display-3
       §2.6 "Precomposed Inline-level Display Values" (`inline-block`, `inline-table`, `inline-flex`,
       `inline-grid`; core/layout/block_flow.c's `bf_element_child` tests those plus `inline`). Each is sized by
       its own module through `intrinsic_inline_sizes` (§9.4, CSS 2.1 §17.5.2.2 Automatic table layout,
       css-flexbox-1 §9.9 "Intrinsic Sizes", or a crash naming css-grid-2 §5.2 "Sizing Grid Containers").
       The steps are css-sizing-3 §5.2 "Intrinsic Contributions"', as §9.4.1's stack applies them: §5.1's
       measurement, the box's `width`/`min-width`/`max-width` (`is_declared_inline_sizes`), then §2.2's outer
       size, regardless of outer display type. The item carries both sizes: the box's own wrap opportunities are
       its answer, not this run's.
       No re-entry: `intrinsic_inline_sizes` reads no real containing block and descends a cycle-free tree, and
       an inline-block table reaches `table_intrinsic_inline_sizes`, never `table_widths` (table_width.h).
       Cyclic percentages are already substituted, so `is_require_acyclic` is not owed. */
    if (!intrinsic_inline_sizes_ask(el)) return IS_CHILD_NOT_YET;
    {
        IntrinsicInlineSizes outer = intrinsic_inline_contributions(el);

        text_run_measure_add_atomic(m, el, outer.min_content, outer.max_content);
    }
    return IS_CHILD_PLACED;
}

/* ---- the record: CSS 2.2 §9.4.2's measurements and §9.4.1's stack walks, held by the layout job ----------- */

/* One run's measurement held between steps: core/layout/inline_run_walk.h's walk over the run and, inside a
   text node, its character position, then text_run.h's steps over the closed collection. */
typedef struct {
    TextRunMeasure m;
    InlineRunWalk walk;
    bool in_text;
    TextRunText text;
    bool collected;   /* the walk ended and the collection is closed */
} IsMeasure;

/* What an entry is about: one run's measurement, or the place of a walk over a container's members that
   asks each one's sizes ahead of the read (§9.4.1's stack, a flex container's items, a table's cells and
   captions). */
typedef enum { IS_ENTRY_RUN = 0, IS_ENTRY_STACK, IS_ENTRY_FLEX, IS_ENTRY_TABLE } IsKind;

/* What an entry holds, allocated apart so a pointer to it survives the table's growth under a nested ask. A
   run entry's measurement lives from its first ask until it is answered; a walk entry holds its place, and a
   table's its grid and captions until every member is answered. */
typedef struct {
    bool answered;
    bool measuring;              /* a run's `meas` is begun and not yet released */
    IsMeasure meas;
    IntrinsicInlineSizes sizes;  /* a run's answer, once `answered` */
    lxb_dom_node_t *prev;        /* a stack's last break whose run and child are answered */
    IntrinsicSizeWalk walk;      /* a flex container's place */
    bool gridded;                /* a table's `grid` and `captions` are built and not yet released */
    TableGrid grid;
    lxb_dom_element_t **captions;
    size_t ncaptions, at;        /* a table's next member: a cell index, then `ncells` plus a caption index */
} IsHeld;

typedef struct IsEntry {
    lxb_dom_element_t *el;          /* the container; NULL is empty, and nothing is removed */
    IsKind kind;
    lxb_dom_node_t *after, *end;    /* the run, for a run entry; NULL for a walk */
    IsHeld *held;                   /* owned */
} IsEntry;

static size_t is_key_slot(lxb_dom_element_t *el, IsKind kind, BlockFlowRun run, size_t cap)
{
    uint64_t h = (uint64_t)(uintptr_t)el * 0x9E3779B97F4A7C15ull;

    h ^= (uint64_t)(uintptr_t)run.after * 0xC2B2AE3D27D4EB4Full;
    h ^= (uint64_t)(uintptr_t)run.end * 0x165667B19E3779F9ull;
    h ^= ((uint64_t)kind + 1u) * 0x27D4EB2F165667C5ull;
    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 29;
    return (size_t)h & (cap - 1);
}

/* The slot the key occupies, or the empty slot it would be inserted at. */
static size_t is_key_probe(const IsEntry *tab, size_t cap, lxb_dom_element_t *el, IsKind kind, BlockFlowRun run)
{
    size_t i = is_key_slot(el, kind, run, cap);

    while (tab[i].el != NULL &&
           (tab[i].el != el || tab[i].kind != kind || tab[i].after != run.after || tab[i].end != run.end))
        i = (i + 1) & (cap - 1);
    return i;
}

static void is_record_grow(IntrinsicSizeRecord *r)
{
    size_t cap = r->cap != 0 ? r->cap * 2 : 64, i;
    IsEntry *tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "the layout job's intrinsic size record could not grow");
    for (i = 0; i < r->cap; i++) {
        BlockFlowRun run;

        if (r->tab[i].el == NULL) continue;
        run.after = r->tab[i].after;
        run.end = r->tab[i].end;
        tab[is_key_probe(tab, cap, r->tab[i].el, r->tab[i].kind, run)] = r->tab[i];
    }
    free(r->tab);
    r->tab = tab;
    r->cap = cap;
}

static void is_measure_release(IsMeasure *k)
{
    if (!k->collected) {
        /* An abandoned collection is closed so the measurement can be released. */
        inline_run_walk_release(&k->walk);
        text_run_measure_finish_begin(&k->m);
    }
    text_run_measure_release(&k->m);
}

static void is_table_release(IsHeld *h)
{
    table_grid_release(&h->grid);
    free(h->captions);
    h->captions = NULL;
    h->gridded = false;
}

void intrinsic_size_record_release(IntrinsicSizeRecord *r)
{
    size_t i;

    DCHECKF(r->asks == r->served + r->built + r->resumed,
            "a layout job's intrinsic size record does not close: %lld asks against %lld served, %lld begun and "
            "%lld continued; every ask has exactly one of the three outcomes, so a gap is a return between the "
            "ask and its outcome", r->asks, r->served, r->built, r->resumed);
    DCHECKF(r->built >= 0 && (size_t)r->built == r->used,
            "a layout job's intrinsic size record holds %zu entries after %lld were begun; each records exactly "
            "one new key", r->used, r->built);
    for (i = 0; i < r->cap; i++) {
        if (r->tab[i].el == NULL) continue;
        if (r->tab[i].held->measuring) is_measure_release(&r->tab[i].held->meas);
        if (r->tab[i].held->gridded) is_table_release(r->tab[i].held);
        free(r->tab[i].held);
    }
    free(r->tab);
    memset(r, 0, sizeof *r);
}

/* The entry for the key in the current job's record, created on its first ask. */
static IsHeld *is_held_of(lxb_dom_element_t *el, IsKind kind, BlockFlowRun run)
{
    IntrinsicSizeRecord *r = layout_job_intrinsic_sizes();
    IsHeld *h;
    size_t i;

    DCHECK(el != NULL, "css-sizing-3 §5.1's intrinsic inline sizes were asked for with no element");
    DCHECK(r != NULL,
           "css-sizing-3 §5.1's intrinsic inline sizes were asked with no layout job holding a record — no drive "
           "is open and no job is entered, and every ask and every reader is made under one "
           "(core/layout/layout_job.h)");
    r->asks++;
    if (r->cap != 0) {
        i = is_key_probe(r->tab, r->cap, el, kind, run);
        if (r->tab[i].el != NULL) {
            h = r->tab[i].held;
            if (h->answered) r->served++;
            else             r->resumed++;
            return h;
        }
    }
    h = calloc(1, sizeof *h);
    CHECK(h != NULL, "out of memory holding one intrinsic inline size measurement");
    if (r->used * 2 >= r->cap) is_record_grow(r);
    i = is_key_probe(r->tab, r->cap, el, kind, run);
    r->tab[i].el = el;
    r->tab[i].kind = kind;
    r->tab[i].after = run.after;
    r->tab[i].end = run.end;
    r->tab[i].held = h;
    r->used++;
    r->built++;
    return h;
}

#if APICLIENT_DEV
/* The entry for the key if the current job's record holds one, counting nothing, for asserts. */
static const IsHeld *is_held_peek(lxb_dom_element_t *el, IsKind kind, BlockFlowRun run)
{
    IntrinsicSizeRecord *r = layout_job_intrinsic_sizes();
    size_t i;

    if (r == NULL || r->cap == 0) return NULL;
    i = is_key_probe(r->tab, r->cap, el, kind, run);
    return r->tab[i].el != NULL ? r->tab[i].held : NULL;
}
#endif

IntrinsicSizeWalk *intrinsic_size_walk_of(lxb_dom_element_t *el)
{
    BlockFlowRun none;

    none.after = NULL;
    none.end = NULL;
    return &is_held_of(el, IS_ENTRY_FLEX, none)->walk;
}

/* One step of a run's collection: a character of the text node it is in, one child, or the end of a box. It
   answers the items it took, at least one, or zero for an atomic inline whose sizes are not ready, with the
   walk still naming it. */
static size_t is_collect_step(IsMeasure *k)
{
    TextRunMeasure *m = &k->m;
    size_t before = m->count;
    lxb_dom_element_t *parent;
    lxb_dom_node_t *node = NULL;
    BoxTreeChild c;

    if (k->in_text) {
        text_run_measure_text_step(m, &k->text);
        k->in_text = !text_run_measure_text_done(&k->text);
        return m->count > before ? m->count - before : 1;
    }
    switch (inline_run_walk_next(&k->walk, &parent, &c)) {
    case INLINE_RUN_END:
        return 1;
    case INLINE_RUN_CLOSE:
        /* The box is complete on this run, so its closing edge is here; a box the run ends inside gets none
           (§9.2.1.1's "open at the end of the line"). */
        text_run_measure_add_box_edge(m, parent, is_intrinsic_edge_px(parent, INTRINSIC_AXIS_HORIZONTAL, true));
        return 1;
    case INLINE_RUN_CHILD:
        break;
    }
    switch (is_child(m, parent, c, &node)) {
    case IS_CHILD_NOT_YET:
        return 0;
    case IS_CHILD_PLACED:
        inline_run_walk_take(&k->walk, NULL);
        break;
    case IS_CHILD_TEXT:
        inline_run_walk_take(&k->walk, NULL);
        text_run_measure_text_begin(m, parent, node, &k->text);
        k->in_text = !text_run_measure_text_done(&k->text);
        break;
    case IS_CHILD_INLINE_BOX:
        inline_run_walk_take(&k->walk, lxb_dom_interface_element(node));
        break;
    }
    return m->count > before ? m->count - before : 1;
}

bool intrinsic_inline_run_sizes_ask(lxb_dom_element_t *el, BlockFlowRun run)
{
    IsHeld *h = is_held_of(el, IS_ENTRY_RUN, run);
    IsMeasure *k = &h->meas;

    if (h->answered) return true;
    if (!h->measuring) {
        /* Collected before any of it is measured: [UAX14]'s rules read forward past the boundary they decide
           (LB25 by three characters, LB9 across any number of marks); see core/layout/text_run.h. */
        if (!layout_job_line_items_left()) return false;
        text_run_measure_init(&k->m);
        inline_run_walk_begin(&k->walk, el, run);
        k->in_text = false;
        k->collected = false;
        h->measuring = true;
    }
    while (!k->collected) {
        size_t n;

        if (!layout_job_line_items_left()) return false;
        if (k->walk.done && !k->in_text) {
            inline_run_walk_end(&k->walk);
            text_run_measure_finish_begin(&k->m);
            k->collected = true;
            break;
        }
        n = is_collect_step(k);
        if (n == 0) {
            DCHECK(!layout_job_line_items_left(),
                   "an atomic inline's intrinsic sizes answered \"not yet\" with the line box items unspent; the "
                   "ask defers nothing else, so its only \"not yet\" is the allowance");
            return false;
        }
        layout_job_line_items_spend(n);
    }
    while (!text_run_measure_finish_done(&k->m)) {
        if (!layout_job_line_items_left()) return false;
        layout_job_line_items_spend(text_run_measure_finish_step(&k->m));
    }
    h->sizes.min_content = text_run_measure_min_content(&k->m);
    h->sizes.max_content = text_run_measure_max_content(&k->m);
    /* §5.1's answer is two numbers, so the collection is freed once they are read. */
    text_run_measure_release(&k->m);
    h->measuring = false;
    h->answered = true;
    return true;
}

/* The reader is a drive over the ask, which answers at once where a body asked first. Under a resting drive
   it measures nothing: the body that reads asked first, so no activation measures what it did not ask for. */
IntrinsicInlineSizes intrinsic_inline_run_sizes(lxb_dom_element_t *el, BlockFlowRun run)
{
    LayoutJobDrive d;
    IntrinsicInlineSizes out;
    IsHeld *h;

    DCHECK(el != NULL, "CSS 2.2 §9.4.2's context was measured with no block container to style it");
#if APICLIENT_DEV
    if (layout_job_rests_innermost()) {
        const IsHeld *held = is_held_peek(el, IS_ENTRY_RUN, run);
        char nbuf[160];

        DCHECKF(held != NULL && held->answered,
                "an intrinsic inline size of %s was read under a resting drive with no answered ask before it, so "
                "its drive would measure the run inside this activation; the body whose read reaches here asks "
                "`intrinsic_inline_sizes_ask` first and propagates \"not yet\", as core/layout/line_box.c's "
                "atomic inline does", box_subject(el, nbuf, sizeof nbuf));
    }
#endif
    layout_job_drive_begin(&d);
    while (!intrinsic_inline_run_sizes_ask(el, run)) (void)block_flow_drive_deferred(&d);
    h = is_held_of(el, IS_ENTRY_RUN, run);
    DCHECK(h->answered, "an intrinsic inline size measurement answered its ask and holds no answer");
    out = h->sizes;
    layout_job_drive_end(&d);
    return out;
}

/* css-sizing-3 §5.2's contribution of one box from its two inner sizes and edges (§2.2 "Intrinsic Size
   Contributions": "based on the outer size of the box; for this purpose, auto margins are treated as zero").
   §2.2's floor ("if the ideal max-content contribution would be smaller than the min-content contribution (e.g.
   due to the use of negative margins), the effective max-content contribution is floored by the min-content
   contribution") is live: a negative margin on an inline box inside the run (allowed by CSS 2.1 §8.3 "Margin
   properties") lowers the max-content line sum but not the widest min-content segment. So it is applied, not
   asserted. */
static IntrinsicInlineSizes is_contribution(IntrinsicInlineSizes inner, CssPx edge)
{
    IntrinsicInlineSizes out;

    out.min_content = css_px_add(inner.min_content, edge);
    out.max_content = css_px_max(css_px_add(inner.max_content, edge), out.min_content);
    return out;
}

IntrinsicInlineSizes intrinsic_outer_contribution(lxb_dom_element_t *el, IntrinsicInlineSizes inner)
{
    /* The inner pair is not asserted: an inversion is what §2.2's floor repairs (see `is_contribution`), and a
       negative inline-box margin inside the run can make a max-content sum negative. The shape is asserted at
       `intrinsic_inline_sizes`' boundary, where this codebase computed the pair. NULL is an anonymous box with
       zero edges (see intrinsic_size.h). */
    if (el == NULL) return is_contribution(inner, css_px(0.0));
    return is_contribution(inner, css_px_add(is_intrinsic_edge_px(el, INTRINSIC_AXIS_HORIZONTAL, false),
                                            is_intrinsic_edge_px(el, INTRINSIC_AXIS_HORIZONTAL, true)));
}

/* §5.2's order: §5.1's measurement, then the box's `width`/`min-width`/`max-width`, then §2.2's outer size;
   a declaration applied after the outer step would clamp a margin box by a content-box property. */
IntrinsicInlineSizes intrinsic_inline_contributions(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "css-sizing-3 §5.2's contributions were asked of no element");
    return intrinsic_outer_contribution(el, is_declared_inline_sizes(el, intrinsic_inline_sizes(el)));
}

CssPx intrinsic_outer_block_contribution(lxb_dom_element_t *el, CssPx inner)
{
    /* §2.2's outer size over one number: a block size has one intrinsic size (css-sizing-3 §3.2: both keywords
       mean "its automatic size"), so there is no pair for §2.2's floor. The edge arithmetic is the shared table's
       vertical row. */
    if (el == NULL) return inner;
    return css_px_add(inner, css_px_add(is_intrinsic_edge_px(el, INTRINSIC_AXIS_VERTICAL, false),
                                       is_intrinsic_edge_px(el, INTRINSIC_AXIS_VERTICAL, true)));
}

/* One declared sizing value of `ch` on `axis`, as a content-box extent, for css-sizing-3 §5.2 "Intrinsic
   Contributions": true when it states a size to apply. False has two sources:
     - the initial keyword, which css-sizing-3 §3.2 "Sizing Values: the <length-percentage [0,∞]>, auto | none,
       stretch, min-content, max-content, and fit-content values" gives no size (`auto` "specifies an automatic
       size", `none` is "no limit"; a min size's `auto` "resolves to a used value of 0", applied by the caller
       since css-flexbox-1 §4.5 "Automatic Minimum Size of Flex Items" overrides it for flex items);
     - a percentage, cyclic in an intrinsic pass. §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes"
       treats a cyclic preferred or max size "as that property's initial value", and resolves a min size's
       "against zero"; both mean no size to apply here, though `min-width`'s is a number (zero).
   §3.2's level-3 keywords have no computed-value rule here, so any other keyword is asserted away (as in
   core/layout/used_value.c). Exported for css-flexbox-1 §9.9.3 "Flex Item Intrinsic Size Contributions",
   whose item percentages are cyclic against the container's main size too; the composition is not shared
   (§5.2 replaces the measurement, §9.9.3 takes the larger). */
bool intrinsic_declared_sizing_px(lxb_dom_element_t *ch, IntrinsicAxis axis, const char *name,
                                 CssKeyword initial, CssPx *out)
{
    CssLength len = css_computed_length(ch, name);
    CssPx declared, surround;
    bool border_box;

    if (len.kind == CSS_LENGTH_KEYWORD) {
        DCHECKF(len.keyword == initial,
                "`%s` computed to the keyword `%s` rather than to its initial value `%s`. CSS 2.1 §10.2 "
                "\"Content width: the 'width' property\" and §10.4 \"Minimum and maximum widths: 'min-width' "
                "and 'max-width'\" admit no other keyword, and css-sizing-3 §3.2's level-3 additions are ones "
                "this engine records no computed-value rule for — core/layout/used_value.c asserts the same "
                "thing about the same grammar. So this is a value the cascade produced and no section of it "
                "defines",
                name, css_keyword_name(len.keyword), css_keyword_name(initial));
        return false;
    }
    if (len.kind != CSS_LENGTH_ABSOLUTE) {
        DCHECKF(len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED,
                "`%s` computed to none of the shapes css-sizing-3 §3.2 \"Sizing Values: the "
                "<length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content "
                "values\" admits",
                name);
        return false;
    }
    /* css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property"' conversion: under `border-box` the
       content box is the length less the two paddings and border widths `axis` names, floored at zero ("the
       specified padding and border widths of the box are subtracted from that size"; a `height` takes the top
       and bottom pair, css-writing-modes-4 §7.2 "Dimensional Mapping"). Cited, not quoted (editions differ; see
       core/layout/table_column_width.c). The surround is the intrinsic one (§5.2.1's zero basis), not
       used_value.h's, which answers the determined-width question. */
    declared = len.px;
    border_box = css_computed_keyword_is(ch, "box-sizing", CSS_KEYWORD_BORDER_BOX);
    if (border_box) {
        surround = css_px_add(is_intrinsic_pad_border_px(ch, axis, false),
                              is_intrinsic_pad_border_px(ch, axis, true));
        declared = css_px_max(css_px_sub(declared, surround), css_px(0.0));
    }
    *out = declared;
    return true;
}

/* css-sizing-3 §5.2 "Intrinsic Contributions"' contribution of a child that declares its own inline size, as
   content-box widths. "the size of the content box of a hypothetical auto-sized float that contains only that
   box": a float around a `width: 500px` box is 500px whatever its text measures, so a declaration replaces the
   measurement rather than being maxed with it. The measured pair is §5.1's ("given an auto preferred size in
   that axis and no minimum or maximum size in that axis"), the operand here.
   Both halves take the same declaration (one number under either containing block). The order is CSS 2.1
   §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'"'s, max then min, so `min-width: 300px;
   max-width: 100px` gives 300px. §10.4's second pass re-solves an `auto` margin, already zero under §2.2
   ("auto margins are treated as zero"), so a clamp is equivalent here; core/layout/used_value.c runs the real
   pass. */
static IntrinsicInlineSizes is_declared_inline_sizes(lxb_dom_element_t *ch, IntrinsicInlineSizes measured)
{
    IntrinsicInlineSizes out = measured;
    CssPx v;

    if (intrinsic_declared_sizing_px(ch, INTRINSIC_AXIS_HORIZONTAL, "width", CSS_KEYWORD_AUTO, &v)) {
        out.min_content = v;
        out.max_content = v;
    }
    if (intrinsic_declared_sizing_px(ch, INTRINSIC_AXIS_HORIZONTAL, "max-width", CSS_KEYWORD_NONE, &v)) {
        out.min_content = css_px_min(out.min_content, v);
        out.max_content = css_px_min(out.max_content, v);
    }
    /* §3.2's `auto` minimum is a used 0, and §5.2.1 resolves a cyclic min size against zero, so the floor runs
       on both arms. It is on the content box, before a possibly negative margin is added by the caller. */
    if (!intrinsic_declared_sizing_px(ch, INTRINSIC_AXIS_HORIZONTAL, "min-width", CSS_KEYWORD_AUTO, &v)) v = css_px(0.0);
    out.min_content = css_px_max(out.min_content, v);
    out.max_content = css_px_max(out.max_content, v);
    return out;
}

/* CSS 2.2 §9.4.1 "Block formatting contexts"' context, whose intrinsic inline sizes are a maximum: "each box's
   left outer edge touches the left edge of the containing block", so children overlap in the inline axis and
   css-sizing-3 §5.2's hypothetical float is as wide as the widest.
   The list is §9.2.1.1's boxes, from core/layout/block_flow.h: one anonymous block box per maximal inline run,
   and an inline box broken around a block-level box yields three ("a block box representing the BODY,
   containing an anonymous block box around C1, the SPAN block box, and another anonymous block box around
   C2"). The loop is over `block_flow_next_block_box`'s breaks, where `-Wswitch` over box kinds lives; a float
   sits inside a run and the run walk refuses it (§9.4.2: "line boxes may vary in width if available horizontal
   space is reduced due to floats").
   Anonymous boxes have zero edges ("Non-inherited properties have their initial value … the margins will be
   0") and `el`'s style. An empty range contributes nothing, so a container with no in-flow child (an empty
   `<td>`, which core/layout/table_column_width.c asks about constantly) takes this arm at no cost. */
static IntrinsicInlineSizes is_block_context(lxb_dom_element_t *el)
{
    lxb_dom_node_t *prev = NULL;
    IntrinsicInlineSizes out;

    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    for (;;) {
        lxb_dom_node_t *brk = block_flow_next_block_box(el, prev);
        IntrinsicInlineSizes one;

        /* §9.2.1.1's anonymous block box: everything strictly between the previous break and this one. */
        {
            BlockFlowRun run;

            run.after = prev;
            run.end = brk;
            /* Only where the section generates one: a run with no inline-level content is not a box ("we assume that
               there is an anonymous block box around 'Some text'"), so `<div><p></p></div>` has one box. The same
               predicate §9.4.1's placement uses keeps the two on one box list. */
            if (!block_flow_run_generates_box(el, run)) one = out;
            else one = intrinsic_outer_contribution(NULL, intrinsic_inline_run_sizes(el, run));
        }
        out.min_content = css_px_max(out.min_content, one.min_content);
        out.max_content = css_px_max(out.max_content, one.max_content);
        if (brk == NULL) return out;
        {
            lxb_dom_element_t *ch = lxb_dom_interface_element(brk);

            one = intrinsic_inline_contributions(ch);
        }
        out.min_content = css_px_max(out.min_content, one.min_content);
        out.max_content = css_px_max(out.max_content, one.max_content);
        /* Strictly forward: the enumeration is exclusive of the box it is seeded with, so the loop is one pass. */
        prev = brk;
    }
}

/* css-sizing-3 §5.1 "Intrinsic Sizes" for a replaced box. Which rule applies depends on natural dimensions: "a
   block-level or inline-level replaced element whose height or width behaves as auto is effectively defined to
   use its max-content size (CSS2§10.3.2)", while "the intrinsic sizes of replaced elements without natural
   sizes are defined below". So a box with a natural size takes CSS 2.2 §10.3.2 "Inline, replaced elements",
   via core/layout/used_value.h's `used_value_replaced_auto_width_px` (the same arms the used value runs; that
   header's CSS 2.1 §10.3.2 has the same number and title in both corpora), and one without takes §5.1's arms.
   Both sizes are one number: §5.1 varies only the containing block "in that axis", and no arm of §10.3.2 reads
   the containing block's inline size (four arms are a natural dimension or constant; the fifth multiplies the
   used height by the ratio). The box type is not asked: §5.1 sizes it "if it was a float", and CSS 2.2 §10.3.6
   "Floating, replaced elements" says "the used value of 'width' is determined as for inline replaced
   elements". */
static IntrinsicInlineSizes is_replaced_sizes(lxb_dom_element_t *el, const ReplacedElement *rep)
{
    IntrinsicInlineSizes out;
    CssPx size;

    DCHECK(rep->replaced,
           "css-sizing-3 §5.1's replaced-element rules were asked of a box HTML §15.4 \"Replaced elements\" "
           "does not classify as one — core/layout/replaced_element.h makes every other field of a "
           "non-replaced answer meaningless and zeroed, so reading a natural dimension off it would size a box "
           "CSS 2.2 §10.3.3 owns from dimensions it does not have");
    /* §5.1's own rules: "without natural sizes" is the absence of both (css-images-3 §4.1 "Object-Sizing
       Terminology": each "may or may not exist"); an object with only a natural height takes §10.3.2's
       second arm. */
    if (!rep->has_width && !rep->has_height) {
        /* §5.1's first arm, "if it has a preferred aspect ratio". css-sizing-3 §2.3 "Intrinsic Size Constraints":
           "unless otherwise specified, a box's preferred aspect ratio is its natural aspect ratio if it has
           one"; Lexbor's registry has no `aspect-ratio` property, so `rep->ratio` is the answer. The wording is
           the committed corpus's (`engine/specindex/csssizing3.json`); the live draft now says
           "non-degenerate preferred aspect ratio", the same in substance because core/layout/replaced_element.c
           never sets `has_ratio` for a degenerate ratio (css-images-3 §4.1 "Object-Sizing Terminology"). */
        if (rep->has_ratio)
            DFAIL("css-sizing-3 §5.1 \"Intrinsic Sizes\"' FIRST replaced arm — a replaced element with a "
                  "NON-DEGENERATE PREFERRED ASPECT RATIO and NEITHER natural size, which is the case CSS 2.2 "
                  "§10.3.2 \"Inline, replaced elements\" declines to define ("
                  "\"the used value of 'width' is undefined in CSS 2.2\") and css-sizing-3 fills in. "
                  "WHAT IS NOT COVERED: the min-content size, which css-sizing-3 §5.1 states outright as "
                  "\"for the min-content size, use zero\"; and the max-content size, whose three sub-arms are "
                  "the stretch fit into a DEFINITE available inline size, else a <length> "
                  "`min-width`/`min-height` with the other dimension from the ratio, else css-sizing-3 §5.1's "
                  "\"use an inline size matching the corresponding dimension of the initial containing "
                  "block\". "
                  "WHAT THE NEXT DIFF BUILDS: an AVAILABLE SPACE parameter on this component, because "
                  "`intrinsic_inline_sizes` takes an element and nothing else, so the first sub-arm's "
                  "antecedent — css-sizing-3 §5.1's \"if the available space is definite in the inline axis\" "
                  "— cannot even be ASKED from here and a walk that skipped to the second would answer a page "
                  "wrongly rather than narrowly. The other two sub-arms need no new input: "
                  "`intrinsic_declared_sizing_px` is already css-sizing-3 §5.1's `<length>` test (it answers "
                  "false for the percentage css-sizing-3 §5.2.1 \"Intrinsic Contributions of Percentage-Sized "
                  "Boxes\" substitutes away) and core/frame/viewport.h holds the initial containing block. "
                  "HOW ITS ABSENCE WOULD SHOW: a replaced box whose only stated dimension is a ratio is laid "
                  "out at CSS 2.2 §10.3.2's 300px default instead of at the ratio's own size, so a page that "
                  "sizes such a box by its height gets a width that ignores the height. "
                  "NOTHING IN THIS BUILD REACHES IT, AND THAT IS A FACT ABOUT ONE FILE RATHER THAN ABOUT THIS "
                  "ONE: core/layout/replaced_element.c sets `has_ratio` in exactly one constructor, the one "
                  "that also sets BOTH natural sizes, so a ratio without them cannot be constructed — the day "
                  "an SVG image's `viewBox` is sized (core/layout/used_value.c refuses the SAME object from "
                  "the used-value side and names the same subject), this arm is what that diff lands beside. "
                  "ITS RELEASE ARM AND THE SIBLING'S COMPOSE, which is asked here rather than left to compose "
                  "by luck: `DFAIL` is `((void)0)` at `-DAPICLIENT_DEV=0`, so this falls through to the "
                  "no-ratio arm below and answers 300px, and core/layout/used_value.c's refusal of the same "
                  "object falls through to the SAME 300px through CSS 2.2 §10.3.2's last arm. One defined "
                  "wrong answer, identical on both sides of the seam, so a release build lays such a box out "
                  "at the default replaced size rather than leaving the intrinsic pass and the used-value "
                  "pass disagreeing about one element");
        /* §5.1's second arm, "if it has no preferred aspect ratio": "for both the min-content size and max-content
           size: if the box has a <length> as its computed minimum size (min-width/min-height) in that
           dimension, use that size … otherwise, use 300px for the width and/or 150px for the height as needed."
           It consults `min-width` although §5.1 removes min sizes; its Note intends that ("this
           author-controllable behavior is made possible by the new auto value for the min size properties"),
           and §5.2's later floor by the same number is harmless.
           The fallback is `used_value_default_replaced_size`, not a literal 300: CSS 2.2 §10.3.2 qualifies it
           ("if 300px is too wide to fit the device, UAs should use the width of the largest rectangle that has
           a 2:1 ratio and fits the device instead"), so intrinsic and used widths agree. */
        if (!intrinsic_declared_sizing_px(el, INTRINSIC_AXIS_HORIZONTAL, "min-width", CSS_KEYWORD_AUTO, &size))
            size = used_value_default_replaced_size(false);
    } else {
        size = used_value_replaced_auto_width_px(el, rep);
    }
    DCHECK(size.px >= 0.0,
           "css-sizing-3 §5.1's intrinsic inline size of a REPLACED box came out negative. Every arm that "
           "produces it is a natural dimension (core/layout/replaced_element.c asserts those non-negative at "
           "their origin), a <length> min size floored at zero by css-sizing-3 §3.3's own conversion, a "
           "product of a non-negative extent with a positive ratio, or the 300px default — so a negative here "
           "is a derivation that lost an operand rather than a document");
    out.min_content = size;
    out.max_content = size;
    return out;
}

/* CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property's box, sized by its own module over the
   same grid core/layout/used_value.c's `uv_table_used_width` uses. The grid is built per call (per-flow
   state solver/dom_cow.h does not swap). */
static IntrinsicInlineSizes is_table_sizes(lxb_dom_element_t *el)
{
    TableGrid grid;
    IntrinsicInlineSizes out;

    table_grid_build(el, &grid);
    out = table_intrinsic_inline_sizes(el, &grid);
    table_grid_release(&grid);
    return out;
}

/* `is_block_context`'s walk asked ahead of its read, in the read's order: each anonymous block box's run, then
   each block-level child's own sizes, its place held from one ask to the next. */
static bool is_block_context_ask(lxb_dom_element_t *el)
{
    BlockFlowRun none;
    IsHeld *h;

    none.after = NULL;
    none.end = NULL;
    h = is_held_of(el, IS_ENTRY_STACK, none);
    while (!h->answered) {
        lxb_dom_node_t *brk;
        BlockFlowRun run;

        if (!layout_job_line_items_left()) return false;
        brk = block_flow_next_block_box(el, h->prev);
        run.after = h->prev;
        run.end = brk;
        if (block_flow_run_generates_box(el, run) && !intrinsic_inline_run_sizes_ask(el, run)) return false;
        if (brk == NULL) {
            h->answered = true;
            break;
        }
        if (!intrinsic_inline_sizes_ask(lxb_dom_interface_element(brk))) return false;
        h->prev = brk;
        layout_job_line_items_spend(1);
    }
    return true;
}

/* `is_table_sizes`' operands asked ahead of its read: CSS 2.1 §17.5.2.2's column widths read every cell's sizes
   (core/layout/table_column_width.c) and CAPMIN every caption's (core/layout/table_width.c). The grid and the
   caption list are held from the first ask until every member is answered.
   NAMED RESIDUAL, the grid's own walk. Not covered: `table_grid_build` and `table_box_captions` run in the one
   step that begins the ask, over every row of the table. Next: §17.5's grid built as a held walk over
   core/layout/table_box.h's rows. How its absence shows: a REST_UNIT_LAYOUT_DESCENDANTS overrun whose samples
   sit under `table_grid_build` reached from `is_table_ask`. */
static bool is_table_ask(lxb_dom_element_t *el)
{
    BlockFlowRun none;
    IsHeld *h;

    none.after = NULL;
    none.end = NULL;
    h = is_held_of(el, IS_ENTRY_TABLE, none);
    if (h->answered) return true;
    if (!h->gridded) {
        if (!layout_job_line_items_left()) return false;
        table_grid_build(el, &h->grid);
        h->ncaptions = table_box_captions(el, &h->captions);
        DCHECK(h->captions != NULL || h->ncaptions == 0,
               "CSS 2.1 §17.4's caption boxes came back as a non-zero count with no array");
        h->gridded = true;
        h->at = 0;
        layout_job_line_items_spend(h->grid.ncells + h->ncaptions + 1);
    }
    while (h->at < h->grid.ncells + h->ncaptions) {
        lxb_dom_element_t *box;

        if (!layout_job_line_items_left()) return false;
        box = h->at < h->grid.ncells ? h->grid.cells[h->at].element : h->captions[h->at - h->grid.ncells];
        /* §17.2.1's anonymous cell has no element and holds nothing to measure; the read answers for it. */
        if (box != NULL && !intrinsic_inline_sizes_ask(box)) return false;
        h->at++;
        layout_job_line_items_spend(1);
    }
    is_table_release(h);
    h->answered = true;
    return true;
}

/* css-contain-2 §3.2 "Inline-Size Containment" (and §3.1, which implies it): "the inline-axis intrinsic sizes of
   the principal box are determined as if the element had no content". A block container or a flex container
   with no content has intrinsic inline sizes of zero; a replaced element's are its natural ones, which
   core/layout/replaced_element.h already answers under containment. True with `*out` written where this
   answers. */
static bool is_contained_as_empty(lxb_dom_element_t *el, IntrinsicInlineSizes *out)
{
    CssDisplay d;
    char nbuf[160];

    if (!containment_inline_size(el) || replaced_element_of(el).replaced) return false;
    d = css_computed_display(el);
    if (css_display_spells(d, CSS_KEYWORD_GRID) || css_display_spells(d, CSS_KEYWORD_INLINE_GRID))
        DFAILF("%s is a grid container under css-contain-2 §3.2 \"Inline-Size Containment\", sized \"as if the "
               "element had no content\". BUILD css-grid-1 §11's track sizing over no grid items, whose explicit "
               "tracks and gutters still have sizes", box_subject(el, nbuf, sizeof nbuf));
    if (!block_flow_display_is_block_container(d) && !flex_item_display_is_flex_container(d)) return false;
    out->min_content = css_px(0.0);
    out->max_content = css_px(0.0);
    return true;
}

bool intrinsic_inline_sizes_ask(lxb_dom_element_t *el)
{
    CssDisplay d;
    IntrinsicInlineSizes empty;

    DCHECK(el != NULL, "css-sizing-3 §5.1's intrinsic inline sizes were asked for with no element");
    /* The read's dispatch, in its order. A replaced element is sized by natural dimensions and measures no
       run; a box neither of §9.4's contexts nor a flex or table module lays out is refused by the read. */
    if (replaced_element_of(el).replaced) return true;
    if (is_contained_as_empty(el, &empty)) return true;
    d = css_computed_display(el);
    if (flex_item_display_is_flex_container(d)) return flex_intrinsic_inline_sizes_ask(el);
    if (table_box_kind_generates_table_box(table_box_kind(d))) return is_table_ask(el);
    if (css_display_spells(d, CSS_KEYWORD_GRID) || css_display_spells(d, CSS_KEYWORD_INLINE_GRID)) {
        CssPx min_content, max_content;

        return grid_container_inline_sizes_ask(el, &min_content, &max_content);
    }
    if (!block_flow_display_is_block_container(d)) return true;
    if (block_flow_establishes_inline_context(el)) {
        BlockFlowRun whole;

        whole.after = NULL;
        whole.end = NULL;
        return intrinsic_inline_run_sizes_ask(el, whole);
    }
    return is_block_context_ask(el);
}

IntrinsicInlineSizes intrinsic_inline_sizes(lxb_dom_element_t *el)
{
    IntrinsicInlineSizes out;

    DCHECK(el != NULL, "css-sizing-3 §5.1's intrinsic inline sizes were asked for with no element");
    /* The replaced question first, as CSS 2.2 §10.3 asks it before the box type: a replaced element is sized
       by natural dimensions, not children (an `img` is not as wide as its `alt`). */
    {
        ReplacedElement rep = replaced_element_of(el);

        /* Read once and handed over: HTML §15.4.2 "Images" decides from the image request, which changes as a
           reply lands, so a second `replaced_element_of` could answer about a different moment. */
        if (rep.replaced) return is_replaced_sizes(el, &rep);
    }
    if (is_contained_as_empty(el, &out)) return out;
    /* §9.4.2's condition ("an inline formatting context is established by a block container box that contains
       no block-level boxes") applies only to block containers; other boxes are dispatched to their modules. */
    {
        CssDisplay d = css_computed_display(el);
        bool container = block_flow_display_is_block_container(d);
        bool flex = flex_item_display_is_flex_container(d);
        bool table = table_box_kind_generates_table_box(table_box_kind(d));
        bool grid = css_display_spells(d, CSS_KEYWORD_GRID) || css_display_spells(d, CSS_KEYWORD_INLINE_GRID);
        char nbuf[160];

        /* css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display values"' box: css-flexbox-1 §9.9's
           answer is the same pair in the same box. */
        if (flex) return flex_intrinsic_inline_sizes(el);
        /* CSS 2.1 §17.2 The CSS table model's `table` and `inline-table`: §17.5.2.2 Automatic table layout's pair
           in the same box; one arm because only §17.4's wrapper (the caller's question) tells them apart. */
        if (table) return is_table_sizes(el);
        /* css-grid-1 §5.2 "Sizing Grid Containers": "The max-content size (min-content size) of a grid container
           is the sum of the grid container's track sizes (including gutters) in the appropriate axis, when the
           grid is sized under a max-content constraint (min-content constraint)." */
        if (grid) {
            LayoutJobDrive drive;

            layout_job_drive_begin(&drive);
            while (!grid_container_inline_sizes_ask(el, &out.min_content, &out.max_content))
                (void)block_flow_drive_deferred(&drive);
            layout_job_drive_end(&drive);
            return out;
        }
        if (!container)
            DFAILF("css-sizing-3 §5.1's intrinsic inline sizes were asked of %s, which is not a block container "
                   "and whose `display` no module here sizes (the flex, table and grid arms are above). BUILD that "
                   "module's intrinsic inline sizes", box_subject(el, nbuf, sizeof nbuf));
    }
    /* CSS 2.2 §9.2.1 "Block-level elements and block boxes"' alternative, asked once over the whole child list
       before either algorithm: "A block container box either contains only block-level boxes or establishes an
       inline formatting context and thus contains only inline-level boxes." It is core/layout/block_flow.h's
       predicate, shared with that file's stack.
       That classification refuses some boxes itself (a misparented table-internal box, an unmodelled
       `display`), naming block_flow.c's reason. A float is reported as a fact and refused by this file's run
       walk over §9.4.2's shortened line box. */
    if (block_flow_establishes_inline_context(el)) {
        BlockFlowRun whole;

        /* The whole of `el`'s content as one run, with no break on either side. */
        whole.after = NULL;
        whole.end = NULL;
        out = intrinsic_inline_run_sizes(el, whole);
    } else {
        out = is_block_context(el);
    }
    /* Asserted at the boundary where the pair leaves this component, so callers need not floor it: a negative
       size would give CSS 2.2 §10.3.5's formula a negative used width that css-sizing-3 §3.1 "Sizing
       Properties"' "the inner size is always floored at zero" would hide (§3.1, not §3.3, whose floor is a
       step of the `border-box` conversion). */
    DCHECK(out.min_content.px >= 0.0 && out.max_content.px >= 0.0,
           "css-sizing-3 §5.1's intrinsic inline sizes came out NEGATIVE. Both arms answer a maximum taken "
           "from zero (core/layout/text_run.c's lines, `is_block_context`'s child contributions), so a "
           "negative margin can lower a term but not the answer; this is an arm that stopped starting there");
    return out;
}
