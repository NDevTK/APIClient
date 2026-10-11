/* css-sizing-3 §5.1's intrinsic inline sizes over a box's own content. See intrinsic_size.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_intrinsic_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/intrinsic_size.h"
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

/* One child node of the box being measured. The shape mirrors core/layout/line_box.c's child walk: both
   iterate the same inline formatting context and must agree on which children are in it. */
/* A block-level child is unreachable from either origin (a direct child of the measured box, or reached through
   a `display: inline` box): core/layout/block_flow.h enumerates CSS 2.2 §9.2.1.1's box list in content order,
   so a run ends at a block-level box at any depth. One assertion states it in `is_child`. */

/* One run being measured. Its `end` can be met at any depth (§9.2.1.1 breaks an inline box around a block-level
   box inside it), so `past_end` carries that out of the descent. It also suppresses the trailing edge of an
   inline box the run ends inside, per §9.2.1.1: "the border would be drawn around C1 (open at the end of the
   line) and C2 (open at the start of the line)". */
typedef struct {
    TextRunMeasure *m;
    lxb_dom_node_t *end;   /* EXCLUSIVE, and reachable at any depth; NULL runs to the end of the content */
    bool past_end;
} IsRun;

static void is_child(IsRun *r, lxb_dom_element_t *parent, BoxTreeChild c);
/* css-sizing-3 §5.2's declared-size step, defined with §9.4.1's stack: §9.4.2's walk applies the same steps
   to an atomic inline-level box, and one definition keeps them agreeing. */
static IntrinsicInlineSizes is_declared_inline_sizes(lxb_dom_element_t *ch, IntrinsicInlineSizes measured);

static void is_walk(IsRun *r, lxb_dom_element_t *el)
{
    BoxTreeChild c;

    /* Called only for the inline-box descent below. The sequence is core/layout/box_tree.h's, which applies
       css-display-3 §2.5 "Box Generation: the none and contents keywords"' splice and never yields a
       `contents` element. */
    for (c = box_tree_first_child(el); box_tree_child_exists(c) && !r->past_end;
         c = box_tree_next_sibling(el, c))
        is_child(r, el, c);
}

static void is_child(IsRun *r, lxb_dom_element_t *parent, BoxTreeChild c)
{
    lxb_dom_node_t *n;
    lxb_dom_element_t *el;
    char *d;
    bool inline_box;

    /* The run's end, tested first at every depth: it is §9.2.1.1's block-level box that "becomes a sibling of
       those anonymous boxes", and everything after it belongs to a later box on §9.4.1's stack. */
    if (r->past_end) return;
    if (box_tree_child_is_node(c, r->end)) { r->past_end = true; return; }
    /* css-lists-3 §3.5's outside marker box "is placed outside the principal block box", so it contributes
       nothing to the content's css-sizing-3 §5.1 "Intrinsic Sizes", as core/layout/line_box.c's fill skips
       it. */
    if (box_tree_child_is_pseudo(c)) {
        DCHECK(block_flow_child_kind(parent, c) == BLOCK_FLOW_CHILD_OUTSIDE_MARKER,
               "a pseudo-element member of a list item's inline formatting context was classified as something "
               "other than css-lists-3 §3.5's outside marker, which is the one arm core/layout/block_flow.h "
               "answers for it");
        return;
    }
    n = c.node;
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_TEXT:
        /* CSS 2.2 §9.2.2.1 "Anonymous inline boxes": collapsible white space "does not generate any anonymous
           inline boxes". core/layout/block_flow.h answers that for every walk. */
        if (block_flow_text_child_generates_box(n)) text_run_measure_add_text(r->m, parent, n);
        return;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* CSS 2.2 §9.2 generates boxes for elements and text only. */
        return;
    case LXB_DOM_NODE_TYPE_ELEMENT:
        break;
    default:
        DFAIL("a node type CSS 2.2 §9.2's box generation does not describe is inside a block container being "
              "measured — the tree this walk iterates holds elements, text, comments, processing instructions "
              "and a doctype, and a CDATA section, a document or a fragment is not a child any parser this "
              "engine runs produces there. Find the writer that inserted it");
        return;
    }
    el = lxb_dom_interface_element(n);
    /* css-display-3 §2.5 "Box Generation: the none and contents keywords": with `display: none`, "The element
       and its descendants generate no boxes or text sequences." */
    if (is_computed_is(el, "display", "none")) return;
    /* Out of flow contributes nothing and introduces no break ("out-of-flow boxes and inline box boundaries do
       not introduce a forced line break or soft wrap opportunity in the flow"): CSS 2.2 §9.3.1 takes it out of
       normal flow, as core/layout/line_box.c also skips it. */
    if (css_computed_keyword_is(el, "position", CSS_KEYWORD_ABSOLUTE) || css_computed_keyword_is(el, "position", CSS_KEYWORD_FIXED)) return;
    if (block_flow_is_floated(el))
        DFAIL("CSS 2.2 §9.5 \"Floats\" takes this child out of the line, and css-sizing-3 §5.2 \"Intrinsic "
              "Contributions\" still counts it: CSS 2.2 §10.3.5 \"Floating, non-replaced elements\" computes "
              "the preferred width \"by formatting the content without breaking lines other than where "
              "explicit line breaks occur\", and a float shortens the line boxes beside it rather than "
              "leaving them alone — §9.4.2's own \"line boxes may vary in "
              "width if available horizontal space is reduced due to floats\". So the contribution is neither "
              "this walk's sum along the line nor a maximum over it, and there is no arm here that is right by "
              "default. BUILD §9.5.1's float placement, which core/layout/line_box.c and "
              "core/layout/flow_position.c both name as the same absent capability");
    /* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes" makes a replaced element an atomic inline-level box
       whatever its `display` ("a non-replaced element with a `display` value of `inline` generates an inline
       box"), so a replaced `inline-block` is answered here too, once. CSS 2.1 §10.3.10 "'Inline-block', replaced
       elements in normal flow" is "Exactly as inline replaced elements.", so one derivation serves both.
       It is asked after the out-of-flow and float tests, CSS 2.2 §9.7 "Relationships between 'display',
       'position', and 'float'"' order as in core/layout/block_flow.c, so a floated `img` is not put in the
       run. */
    if (replaced_element_of(el).replaced) { is_atomic_replaced(r->m, el); return; }
    d = is_computed(el, "display");
    inline_box = strcmp(d, "inline") == 0;
    /* Freed before either crash: a `DFAIL` compiles out in release and control falls through, so a `free` on
       each side would double free (see `tr_wraps`). */
    free(d);
    if (inline_box) {
        /* HTML §15.3.4 "Phrasing content" first: a `br` changes the partition, not a term. The max-content size
           formats content "without breaking lines other than where explicit line breaks occur" (CSS 2.2
           §10.3.5 "Floating, non-replaced elements"), so a missed `br` reports a paragraph's width as its
           longest line. core/layout/line_box.c asks the same component. */
        switch (phrasing_break_of(el)) {
        case PHRASING_BREAK_FORCED:
            text_run_measure_add_forced_break(r->m, el);
            return;
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
            return;
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
        text_run_measure_add_box_edge(r->m, el, is_intrinsic_edge_px(el, INTRINSIC_AXIS_HORIZONTAL, false));
        is_walk(r, el);
        /* §9.2.1.1's "open at the end of the line": the run ended inside this box, so no closing edge here and
           no opening edge on the next fragment. A run resumed mid-tree never calls this arm for an already-open
           ancestor, so its leading edge is withheld too. */
        if (r->past_end) return;
        text_run_measure_add_box_edge(r->m, el, is_intrinsic_edge_px(el, INTRINSIC_AXIS_HORIZONTAL, true));
        return;
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
           "`bf_content_kind` and `block_flow_next_block_box` stopped agreeing with the other is the fix");
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
    {
        IntrinsicInlineSizes inner = is_declared_inline_sizes(el, intrinsic_inline_sizes(el));
        IntrinsicInlineSizes outer = intrinsic_outer_contribution(el, inner);

        text_run_measure_add_atomic(r->m, el, outer.min_content, outer.max_content);
    }
}

/* CSS 2.2 §9.4.2's inline formatting context, measured over one run of `el`'s content, given as the inline box
   open where it starts, the first node to measure, and the node it ends before. The entry below composes these
   from core/layout/block_flow.h's `BlockFlowRun`; private because the open box and first node derive from one
   break.
   Open ancestors are closed on the way out, per §9.2.1.1 ("the border would be drawn around C1 (open at the end
   of the line) and C2 (open at the start of the line)"): a fragment the run starts inside gets no leading edge
   (this loop never calls `is_child` on it) and its trailing edge here; one it ends inside gets `past_end`.
   The step out goes to the ancestor's next sibling, never back to the ancestor, which would re-walk the closed
   fragment. `at` may be absent: an ancestor with nothing after it still gets its closing edge. */
static IntrinsicInlineSizes is_run_sizes(lxb_dom_element_t *el, lxb_dom_element_t *open, BoxTreeChild at,
                                         lxb_dom_node_t *end)
{
    TextRunMeasure m;
    IsRun r;
    IntrinsicInlineSizes out;
    lxb_dom_node_t *root = lxb_dom_interface_node(el);

    text_run_measure_init(&m);
    r.m = &m;
    r.end = end;
    r.past_end = false;
    for (;;) {
        BoxTreeChild c;
        lxb_dom_node_t *box;

        lxb_dom_element_t *up;

        for (c = at; box_tree_child_exists(c) && !r.past_end; c = block_flow_run_next(open, c))
            is_child(&r, open, c);
        if (r.past_end) break;
        box = lxb_dom_interface_node(open);
        if (box == root) break;
        text_run_measure_add_box_edge(&m, open, is_intrinsic_edge_px(open, INTRINSIC_AXIS_HORIZONTAL, true));
        /* The step out is over the box tree (box parent, then next box sibling): under css-display-3 §2.5's
           splice neither is a DOM pointer. */
        up = box_tree_parent(box_tree_child_of_node(box));
        DCHECK(up != NULL,
               "CSS 2.2 §9.2.1.1's run was inside a box with no BOX parent, so the walk cannot leave it — the "
               "box ancestors of every position in a container's content are inline boxes up to the container "
               "itself, and this chain does not reach it");
        at = box_tree_next_sibling(up, box_tree_child_of_node(box));
        open = up;
    }
    /* The measurement exists only after this: [UAX14]'s rules read forward past the boundary they decide (LB25 by
       three characters, LB9 across any number of marks); see core/layout/text_run.h. It retains the items, so
       this function releases them after reading the two numbers. */
    text_run_measure_finish(&m);
    out.min_content = text_run_measure_min_content(&m);
    out.max_content = text_run_measure_max_content(&m);
    /* §5.1's answer is two numbers, so the measurement ends here. */
    text_run_measure_release(&m);
    return out;
}

IntrinsicInlineSizes intrinsic_inline_run_sizes(lxb_dom_element_t *el, BlockFlowRun run)
{
    BlockFlowRunStart start;

    DCHECK(el != NULL, "CSS 2.2 §9.4.2's context was measured with no block container to style it");
    /* Where the run begins is `block_flow_run_start`'s answer (core/layout/block_flow.h), shared with the fill so
       css-display-3 §2.5's splice is handled once. `open` is the element whose fragment the run continues. */
    start = block_flow_run_start(el, run);
    return is_run_sizes(el, start.open, start.at, run.end);
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
    char *box_sizing;
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
    box_sizing = css_computed_value(ch, "box-sizing");
    DCHECK(box_sizing != NULL,
           "the cascade produced no computed `box-sizing` — css-sizing-3 §3.3 \"Box Edges for Sizing: the "
           "box-sizing property\" gives it an initial value of `content-box`, so the cascade's last layer "
           "always answers");
    border_box = strcmp(box_sizing, "border-box") == 0;
    free(box_sizing);
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

            /* §5.2's order: §5.1's measurement, then the child's `width`/`min-width`/`max-width`, then §2.2's
               outer size; a declaration applied after the outer step would clamp a margin box by a content-box
               property. */
            one = intrinsic_outer_contribution(ch, is_declared_inline_sizes(ch, intrinsic_inline_sizes(ch)));
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
    /* §9.4.2's condition ("an inline formatting context is established by a block container box that contains
       no block-level boxes") applies only to block containers; other boxes are dispatched to their modules. */
    {
        char *d = is_computed(el, "display");
        bool container = block_flow_display_is_block_container(d);
        bool flex = flex_item_display_is_flex_container(d);
        bool table = table_box_kind_generates_table_box(table_box_kind(d));

        free(d);
        /* css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display values"' box: css-flexbox-1 §9.9's
           answer is the same pair in the same box. */
        if (flex) return flex_intrinsic_inline_sizes(el);
        /* CSS 2.1 §17.2 The CSS table model's `table` and `inline-table`: §17.5.2.2 Automatic table layout's pair
           in the same box; one arm because only §17.4's wrapper (the caller's question) tells them apart. */
        if (table) return is_table_sizes(el);
        if (!container)
            DFAIL("css-sizing-3 §5.1's intrinsic inline sizes were asked of a box that is NOT A BLOCK CONTAINER, "
                  "so neither of CSS 2.2 §9.4's two formatting contexts is what lays its content out and "
                  "neither §9.4.2's line boxes nor §9.4.1's stack is the algorithm. Which module owns it is its "
                  "own `display`, and TWO OF THE THREE HAVE LEFT THROUGH ARMS ABOVE THIS LINE, so what still "
                  "reaches this crash is the GRID and whatever `display` no module here models. "
                  "A TABLE-INTERNAL BOX IS NOT ONE OF THEM AND MUST NOT BE ANSWERED AS IF IT WERE: a "
                  "`table-cell` and a `table-caption` ARE block containers and took neither arm nor this "
                  "crash, and a row, a row group and a column box have no intrinsic inline size of their own "
                  "at all — CSS 2.1 §17.5 Visual layout of table contents' rules 1 and 2 give a row the whole "
                  "grid row, so its width is the TABLE's and the question belongs to the table box above it "
                  "(core/layout/table_box.h's `table_box_table_of`). Reaching this crash with one means "
                  "core/layout/block_flow.c's classification let a misparented box through, which is where "
                  "§17.2.1 Anonymous table objects is owed and not here. "
                  "A FLEX CONTAINER LEFT THROUGH THE LINE ABOVE AND THIS SENTENCE USED TO SEND ITS READER TO "
                  "BUILD FLEX LINE BREAKING FOR IT: it said css-flexbox-1 §9.9 \"Intrinsic Sizes\" derives a "
                  "flex container's intrinsic sizes from its flex lines, and §9.9 says the opposite for the "
                  "two arms an INLINE size can reach — §9.9.1 \"Flex Container Intrinsic Main Sizes\" states "
                  "that \"an implementation is conformant to CSS Flexible Box Layout if it conforms to either "
                  "the Ideal Algorithm or the Web-compatible Algorithm\", and the second of those is a sum "
                  "over ITEMS with no line in it. The reader who followed the old sentence would have built "
                  "the whole of §9.3 \"Main Size Determination\" before writing a single contribution. "
                  "core/layout/flex_intrinsic_size.h is the component, and what it still refuses it refuses by "
                  "name. "
                  "A TABLE LEFT THROUGH THE LINE ABOVE AND THIS SENTENCE USED TO SEND ITS READER TO BUILD "
                  "IT: it is core/layout/table_width.h's `table_intrinsic_inline_sizes`, and what the retired "
                  "clause got RIGHT is worth keeping because it is why that entry is an EXPORT and not a "
                  "layout — §17.5.2.2 Automatic table layout names the pair this function returns in its own "
                  "final rules, \"the minimum width required by all the columns plus cell spacing or borders "
                  "(MIN)\" and \"the maximum width required by the columns plus cell spacing or borders "
                  "(MAX)\", over per-column minima and maxima core/layout/table_column_width.h already "
                  "exports. IT WAS INCOMPLETE ON TWO POINTS AND BOTH CHANGED THE CODE, which is the record a "
                  "reader re-deriving this needs rather than the instruction. It named MIN and MAX and NOT "
                  "CAPMIN, and §17.5.2.2's rule this is the evaluation of carries all three — a producer "
                  "returning the bare column sums reports a table narrower than a caption its own §17.4 "
                  "wrapper has to hold. And it said EXPORT without saying WHAT MUST NOT BE EXPORTED: the "
                  "obvious reading is `table_widths`, whose answer is a USED width, and that reads the "
                  "CONTAINING BLOCK — which for a table inside a float or an inline-block returns through CSS "
                  "2.2 §10.3.5's shrink-to-fit into THIS function for the ancestor, so the naive export "
                  "recurses until the C stack ends on an ordinary document. The pair is containing-block-free "
                  "by css-sizing-3 §5.1's own definition, which removes the preferred size and thereby "
                  "§17.5.2.1 Fixed table layout with it. WHAT IS LEFT AS A LAYOUT IS THE GRID, "
                  "AND ITS SECTION NUMBER USED TO BE WRONG HERE — this line said css-grid-2 §11.5, which is "
                  "\"Aligning the Grid: the justify-content and align-content properties\" and decides nothing "
                  "about a size. The sentence that governs is css-grid-2 §5.2 \"Sizing Grid Containers\": \"The "
                  "max-content size (min-content size) of a grid container is the sum of the grid container's "
                  "track sizes (including gutters) in the appropriate axis, when the grid is sized under a "
                  "max-content constraint (min-content constraint).\" So the operand is the TRACKS, which is "
                  "css-grid-2 §12.5 \"Resolve Intrinsic Track Sizes\", which is a step of "
                  "css-grid-2 §12.3 \"Track Sizing Algorithm\" — a whole chapter away from where this "
                  "crash pointed. That arm needs its own box tree and its own track list first and is "
                  "not this walk with a different accumulator");
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
