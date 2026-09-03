/* css-sizing-3 §5.1's intrinsic inline sizes over a box's own content. See intrinsic_size.h for why §5.1 is a
   definition and CSS 2.2 §10.3.5 is the operation, for the split with core/layout/text_run.h, and for which box
   this walk is the algorithm of. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/phrasing_break.h"
#include "core/layout/replaced_element.h"
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

/* ---- ONE SIDE OF AN INLINE BOX: ONE FACT, TWO QUESTIONS ---------------------------------------------------
   THE FACT IS WHICH THREE PROPERTIES ONE SIDE IS, and it is written once. CSS 2 §8.1 "Box dimensions" nests
   them around the content — padding, then border, then margin — and both of the sections that put an inline
   box's own edge somewhere are stated over the same three: css-sizing-3 §2.2 "Intrinsic Size Contributions"'
   outer size ("based on the outer size of the box; for this purpose auto margins are treated as zero") and CSS
   2.2 §9.4.2 "Inline formatting contexts"' line ("horizontal margins, borders, and padding are respected
   between these boxes"). A second copy of these six names is the one way the line's idea of which lengths
   bracket a box could drift from the contribution's, so there is one table and the two entries below are
   QUESTIONS asked of it rather than two derivations standing beside each other.
   THE TWO QUESTIONS DIFFER ON EXACTLY ONE INPUT — A PERCENTAGE — AND css-sizing-3 §5.2.1 "Intrinsic
   Contributions of Percentage-Sized Boxes" ANSWERS BOTH, in that one section. A percentage on any of the six
   resolves against the containing block's width (CSS 2.1 §8.3 "Margin properties" and §8.4 "Padding
   properties", which resolve against that width on BOTH axes). While an INTRINSIC contribution is being
   measured that width is the number the measurement produces, which is §5.2.1's own opening — "creating a
   cyclic dependency" — and its rule for these properties is one sentence: "For the min size properties, as
   well as for margins and paddings (and gutters), a cyclic percentage is resolved against zero for determining
   intrinsic size contributions." Its NEXT paragraph is the other question, "when calculating the used sizes
   and positions of the containing block's contents" — "Otherwise, the percentage is resolved against the
   containing block's size" — which is core/layout/line_box.c's fill, because a line box is filled at a width
   its containing block has already determined and the percentage is not cyclic there at all. §5.2.1's own note
   says these two rules are what "specify the previously-undefined behavior of this cyclic case" that CSS 2.1
   §8.4 leaves "undefined in CSS 2.1".
   WHAT STOOD HERE WAS ONE ENTRY REFUSING BOTH, and it refused the line's question with the contribution's
   reason — a crash whose stated cause (the width is this walk's own output) is FALSE of the caller that meets
   it most often. Its remedy clause was wrong as well as its scope: it said this function had FOUR call sites,
   two of them line_box.c's, and it had SIX — the two edges bracketing an inline box's descent AND the two
   inside `is_atomic_replaced`, both of which are this file's and neither of which the clause counted. Building
   §5.2.1's zero into the one shared entry, as that clause instructed, would have mis-laid-out every
   percentage-margined inline box on a real line, silently.
   NEITHER ENTRY ASKS WHETHER THE PERCENTAGE IS CYCLIC, and that is a derivation rather than an omission. Every
   box this walk reaches has the box being measured as its containing block — §9.4.2's inline formatting
   context is established by that block container, `is_child` returns before an out-of-flow child (whose
   containing block is another box) is ever measured, and the walk is run only to produce that box's own
   intrinsic inline size — so a percentage here is cyclic by construction. §5.2.1's OTHER "otherwise" bullet is
   unreachable through either entry for the same kind of reason: it is conditioned on a cyclic dependency
   "introduced due to a block-axis size other than a minimum size on the containing block", and all six of
   these resolve against the containing block's WIDTH, which is the INLINE axis. */
static const char *const IS_EDGE[2][3] = {
    { "margin-left",  "padding-left",  "border-left-width"  },
    { "margin-right", "padding-right", "border-right-width" },
};
enum { IS_EDGE_MARGIN, IS_EDGE_PADDING, IS_EDGE_BORDER };

/* THE BORDER WIDTH IS THE TERM WITH NO QUESTION IN IT, so it is one function both entries call rather than an
   arm of each: css-backgrounds-3 §3.3 "Line Thickness: the border-width properties"' `Computed value:` line is
   "absolute length, snapped as a border width", so there is no percentage for §5.2.1 to resolve and no `auto`
   for §2.2 to zero — the computed value IS the used one on both sides of the split. */
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

/* QUESTION ONE — css-sizing-3 §5.2.1's INTRINSIC CONTRIBUTION at one side, where a percentage is CYCLIC and
   resolves against ZERO. It is `static` on purpose and that is the structural half of this split: it has no
   caller outside this file, and being unreachable from anywhere else is what stops it being asked at a
   used-value site, where its zero would be a wrong answer no assert could see. */
static CssPx is_intrinsic_edge_px(lxb_dom_element_t *el, bool trailing)
{
    const char *const *side = IS_EDGE[trailing ? 1 : 0];
    CssLength margin = css_computed_length(el, side[IS_EDGE_MARGIN]);
    CssLength padding = css_computed_length(el, side[IS_EDGE_PADDING]);
    CssPx sum;

    /* §2.2's own sentence, which CSS 2.1 §10.3.1 "Inline, non-replaced elements" states again for the used
       value ("A computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of '0'"). It
       is the only keyword §8.3's <margin-width> grammar admits. */
    if (margin.kind == CSS_LENGTH_KEYWORD) {
        DCHECKF(strcmp(margin.keyword, "auto") == 0,
                "`%s` computed to the keyword `%s`. CSS 2.1 §8.3 \"Margin properties\"' <margin-width> grammar "
                "admits a length, a percentage and `auto` and nothing else, so a fourth form is a declaration "
                "lexbor's own validation should have dropped",
                side[IS_EDGE_MARGIN], margin.keyword);
        sum = css_px(0.0);
    } else {
        DCHECKF(margin.kind == CSS_LENGTH_ABSOLUTE || margin.kind == CSS_LENGTH_PERCENTAGE ||
                    margin.kind == CSS_LENGTH_CALCULATED,
                "`%s` computed to none of the three shapes CSS 2.1 §8.3 \"Margin properties\" admits",
                side[IS_EDGE_MARGIN]);
        /* §5.2.1 resolves the PERCENTAGE against zero and not the whole value, which is the contrast the same
           section draws one paragraph up: a cyclic max or preferred size on a non-replaced box is "treated …
           as that property's initial value", while a margin's cyclic percentage "is resolved against zero". So
           `margin-left: calc(10px + 50%)` contributes 10px, and one basis handles both shapes in one step
           because css-values-4 §10.11 "Computed Value" left the percentage inside the math function. */
        sum = css_length_resolve_pct(margin, css_px(0.0));
    }
    /* §8.4's <padding-width> has no `auto` and no keyword at all. */
    DCHECKF(padding.kind == CSS_LENGTH_ABSOLUTE || padding.kind == CSS_LENGTH_PERCENTAGE ||
                padding.kind == CSS_LENGTH_CALCULATED,
            "`%s` computed to a keyword. CSS 2.1 §8.4 \"Padding properties\"' <padding-width> grammar is a "
            "length or a percentage and nothing else",
            side[IS_EDGE_PADDING]);
    /* THE FLOOR IS THE PROPERTY'S RANGE AND IT IS REACHED ONLY THROUGH A MATH FUNCTION. CSS 2.1 §8.4 says
       outright that "Unlike margin properties, values for padding values cannot be negative", and §5.1's range
       restriction drops a negative LITERAL — but css-values-4 §9.1 "Numeric Functions" exempts a math function
       from that check and moves it to the result: "numeric functions returning out-of-range values never cause
       a declaration to become invalid", and instead "the value of a numeric function is clamped to the range
       allowed in the context it is used at computed value time if possible, and at used value time otherwise".
       §5.2.1's ZERO BASIS is precisely what makes `calc(50% - 10px)` land out of range HERE and in range at a
       real width, so this clamp belongs at this resolution and could not have run at the cascade. `css_px_max`
       and not an `if`, so the clamped-away operand's environment facts stay in the domain —
       core/layout/used_value.c's own padding arm states that reason in full, and this is the same arithmetic at
       the other basis. A MARGIN IS NOT CLAMPED, which is §8.3's "negative values for margin properties are
       allowed". */
    sum = css_px_add(sum, css_px_max(css_length_resolve_pct(padding, css_px(0.0)), css_px(0.0)));
    return css_px_add(sum, is_edge_border_px(el, side[IS_EDGE_BORDER]));
}

/* QUESTION TWO — §5.2.1's "Otherwise, the percentage is resolved against the containing block's size", which is
   CSS 2.1 §8.3's and §8.4's ordinary basis and therefore core/layout/used_value.h's answer rather than a
   second one written here. `used_value_px` owns §10.3's per-box-type rules for a margin (an inline box's
   `auto` is §10.3.1's 0), §8.4's percentage basis and its clamp for a padding, and §10.1's chain that produces
   the basis — every one of which this file would otherwise be re-deriving with nothing to keep the two
   agreeing. THE BASIS EXISTS AT THE MOMENT OF THE CALL because used_value.h derives every used value PER READ
   from the flow's own tree, so "the containing block has already determined its width" is a statement about
   derivability and not about an ordering this file has to arrange. */
CssPx used_inline_box_edge_px(lxb_dom_element_t *el, bool trailing)
{
    const char *const *side = IS_EDGE[trailing ? 1 : 0];
    CssPx sum;

    DCHECK(el != NULL, "CSS 2.2 §9.4.2's line-box edge was asked for with no element");
    sum = used_value_px(el, side[IS_EDGE_MARGIN]);
    sum = css_px_add(sum, used_value_px(el, side[IS_EDGE_PADDING]));
    return css_px_add(sum, is_edge_border_px(el, side[IS_EDGE_BORDER]));
}

/* css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes"' CYCLIC PERCENTAGE SIZE on ONE
   property of the box being contributed, REFUSED. The section's own subject is a percentage that resolves
   against a size in the same axis as the contribution being calculated, which is exactly what every one of
   these resolves against: CSS 2.1 §10.2 "Content width: the 'width' property", §10.4 "Minimum and maximum
   widths: 'min-width' and 'max-width'" and §8.4 give all of them the containing block's WIDTH as their basis,
   and the containing block of a child measured by this walk is the box whose width this walk produces.
   THE PROPERTY NAME IS THE ADDRESS AND IT IS PASSED IN, because the crash below is written once and reached
   from a loop: a `DFAIL` stamps the line it is written at, so a message that named only the derivation would
   report one line for five different properties and ask its reader to route a read with no object. This is
   core/layout/used_value.c's own convention for the same reason — its box-type crashes name the computed
   `display` and nothing else. */
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
           "contribution resolves it against zero — and core/layout/text_run.h's atomic item cannot carry "
           "that: `TEXT_RUN_ITEM_ATOMIC` holds ONE `size`, and text_run.c sums that one field into both "
           "answers through `tr_line_size`. BUILD THE PAIR ON THE ITEM FIRST — a min-content and a "
           "max-content inline size on `TEXT_RUN_ITEM_ATOMIC` and on `text_run_measure_add_atomic`, with the "
           "two accumulators reading their own — and then this arm supplies §5.2.1's two numbers and the "
           "refusal goes. Until the item carries two, an arm here could only pick one of §5.2.1's answers and "
           "report it as both, which is a wrong shrink-to-fit width rather than an approximate one",
           name);
}

/* CSS 2.2 §9.2.2 "Inline-level elements and inline boxes"' ATOMIC INLINE-LEVEL BOX, as ONE ITEM of this run.
   The section is why there is one item and not a bracketed descent: an atomic inline-level box is so called
   "because they participate in their inline formatting context as a single opaque box", and css-text-3 §5.5
   "Line Breaking Details" is why the item is the run's business at all — it puts "a soft wrap opportunity
   before and after each replaced element or other atomic inline", which cuts the min-content segment there.
   THE SIZE IS THE MARGIN BOX'S, DERIVED HERE AND NOT READ WHOLE, and that is the one thing this arm may not
   share with core/layout/line_box.c's walk over the same children. That walk hands the item
   `used_value_margin_edge_px`, which resolves a PERCENTAGE margin or padding against the containing block's
   width — correct there, because a line box is filled at a width its containing block already determined, and
   self-referential here. So the same number is composed out of the three terms CSS 2 §8.1 "Box dimensions"
   nests it from, each taken from the entry that refuses the input that would make it cyclic:
     - `is_intrinsic_edge_px` for the two horizontal edges (margin, border and padding per side), which is
       css-sizing-3 §2.2's outer size under §5.2.1's cyclic-percentage resolution — the ZERO basis, which is
       exactly the half `used_inline_box_edge_px` answers differently for a real line;
     - `used_value_content_px` for CSS 2.1 §10.3.2 "Inline, replaced elements"' used CONTENT width over
       core/layout/replaced_element.h's natural dimensions, guarded by the five refusals above.
   THE EDGES ARE COMPUTED BEFORE THE CONTENT AND THAT IS AN ORDER AND NOT A STYLE: `used_value_content_px`
   reaches `padding-left` and `padding-right` itself (css-sizing-3 §3.3's conversion needs them under
   `box-sizing: border-box`, and the surround is computed either way), so it is the two edge calls that have
   already established those two are absolute lengths by the time it runs. */
static void is_atomic_replaced(TextRunMeasure *m, lxb_dom_element_t *el)
{
    /* THE FIVE PROPERTIES CSS 2.1 §10.3.2's derivation READS THAT RESOLVE AGAINST THE CONTAINING BLOCK'S
       WIDTH, and the list is over that derivation rather than over §8.3's whole width-relative set: the
       HORIZONTAL margins, paddings and border widths are the edge entry's six and are refused there, while
       `margin-top` and `margin-bottom` — which §8.3 resolves against the width too, and says so in its own
       sentence — are read by no arm of §10.3.2 or §10.4 at all, so refusing one would crash a document this
       arm measures correctly. `padding-top` and `padding-bottom` ARE here, because §10.3.2's intrinsic-ratio
       arm reads the used HEIGHT and the surround that read computes is the vertical pair. */
    static const char *const CYCLIC[] = { "width", "min-width", "max-width", "padding-top", "padding-bottom" };
    CssPx lead, trail;
    size_t i;

    for (i = 0; i < sizeof CYCLIC / sizeof CYCLIC[0]; i++) is_require_acyclic(el, CYCLIC[i]);
    lead = is_intrinsic_edge_px(el, false);
    trail = is_intrinsic_edge_px(el, true);
    text_run_measure_add_atomic(m, el,
                                css_px_add(css_px_add(lead, used_value_content_px(el, false)), trail));
}

/* ONE CHILD NODE of the box whose intrinsic sizes are being measured. The shape mirrors core/layout/
   line_box.c's own child walk deliberately: both are iterating the SAME inline formatting context, and the two
   answering differently about which children are in it would be one classification with two copies. */
/* `in_inline_box` IS THE ORIGIN OF THE CHILD AND IT IS THREADED FROM THE CALLER, never re-derived here: FALSE
   is a child of the box being measured, whose whole child list CSS 2.2 §9.2.1 "Block-level elements and block
   boxes"' dispatch has already classified ("A block container box either contains only block-level boxes or
   establishes an inline formatting context and thus contains only inline-level boxes"), and TRUE is a child of
   a `display: inline` box this walk descended into, about which that dispatch said NOTHING — it was asked over
   the measured box's list, and an inline box's own children were not in it. The two crashes at the tail of
   this function are two DIFFERENT missing capabilities and the flag is the only thing that tells them apart,
   which is why it is a parameter and not a test. */
static void is_child(TextRunMeasure *m, lxb_dom_element_t *parent, lxb_dom_node_t *n, bool in_inline_box);

static void is_walk(TextRunMeasure *m, lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *c;

    /* THE ONLY CALLER IS THE INLINE-BOX DESCENT BELOW, so every child this reaches is inside one. */
    for (c = n->first_child; c != NULL; c = c->next) is_child(m, el, c, true);
}

static void is_child(TextRunMeasure *m, lxb_dom_element_t *parent, lxb_dom_node_t *n, bool in_inline_box)
{
    lxb_dom_element_t *el;
    char *d;
    bool inline_box;
    char nbuf[160];

    switch (n->type) {
    case LXB_DOM_NODE_TYPE_TEXT:
        /* CSS 2.2 §9.2.2.1 "Anonymous inline boxes" first: a run of white space this element's `white-space`
           collapses away "does not generate any anonymous inline boxes", so it is not content and contributes
           nothing. core/layout/block_flow.h answers that for every walk over a block container's children, and
           asking it here rather than re-deriving it is what keeps this walk and §9.4.1's agreeing about what a
           document's white space is. */
        if (block_flow_text_child_generates_box(parent, n)) text_run_measure_add_text(m, parent, n);
        return;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* CSS 2.2 §9.2 generates boxes for elements and for text, and for neither of these three. */
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
    /* css-display-3 §2.5 "Box Generation: the none and contents keywords" on `display: none`: "The element and
       its descendants generate no boxes or text sequences." So there is no box to contribute. This is the one
       element case that is nothing rather than something unbuilt, which is why it is not a crash.
       WHAT STOOD HERE cited §3 "Box Generation" and quoted `none` as turning "off the display of an element so
       that it has no effect on layout at all" — a sentence that appears NOWHERE in css-display-3, beside a
       number that is "Display Order: the order property". Box generation is §2.5 and always has been. */
    if (is_computed_is(el, "display", "none")) return;
    /* OUT OF FLOW CONTRIBUTES NOTHING, and css-text-3 §5.5 confirms it introduces no break either ("out-of-flow
       boxes and inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
       flow"). CSS 2.2 §9.3.1 takes an absolutely positioned box out of normal flow, so it is not on the line
       whose size is being measured and its own text is not in this run. core/layout/line_box.c returns at the
       same test for the same reason, which is why the two walks still agree about what is in the context. */
    if (is_computed_is(el, "position", "absolute") || is_computed_is(el, "position", "fixed")) return;
    if (!is_computed_is(el, "float", "none"))
        DFAIL("CSS 2.2 §9.5 \"Floats\" takes this child out of the line, and css-sizing-3 §5.2 \"Intrinsic "
              "Contributions\" still counts it: CSS 2.2 §10.3.5 \"Floating, non-replaced elements\" computes "
              "the preferred width \"by formatting the content without breaking lines other than where "
              "explicit line breaks occur\", and a float shortens the line boxes beside it rather than "
              "leaving them alone — §9.4.2's own \"line boxes may vary in "
              "width if available horizontal space is reduced due to floats\". So the contribution is neither "
              "this walk's sum along the line nor a maximum over it, and there is no arm here that is right by "
              "default. BUILD §9.5.1's float placement, which core/layout/line_box.c and "
              "core/layout/flow_position.c both name as the same absent capability");
    d = is_computed(el, "display");
    inline_box = strcmp(d, "inline") == 0;
    /* FREED BEFORE EITHER CRASH AND READ AGAIN BY NEITHER, for the reason `tr_wraps` states: a DFAIL is
       compiled out in release and control falls through it, so a `free` on each side of one is a double free
       that only the release build reaches. */
    free(d);
    if (inline_box) {
        /* HTML §15.3.4 "Phrasing content" FIRST, because what it declares changes the PARTITION and not a term
           in the sum. css-sizing-3 §2.1's max-content inline size is what CSS 2.2 §10.3.5 "Floating,
           non-replaced elements" computes "by formatting the content without breaking lines other than where
           explicit line breaks occur", so a `br` this walk did not see would not be a small error in the
           answer — it would report a whole paragraph's width as the width of
           its longest line. core/layout/line_box.c's walk over the same children asks the same component the
           same question, which is what keeps the two from disagreeing about how many lines a run has. */
        switch (phrasing_break_of(el)) {
        case PHRASING_BREAK_FORCED:
            text_run_measure_add_forced_break(m, el);
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
        /* CSS 2.2 §9.2.2 makes a REPLACED element an ATOMIC inline-level box wherever its `display` puts it,
           so the `inline` above does not settle which of the two this is. It is asked before the descent
           because an atomic inline has no text of this run inside it to descend to. */
        if (replaced_element_of(el).replaced) { is_atomic_replaced(m, el); return; }
        /* §5.5: "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
           flow", and css-text-3 §4.1.1's collapsing crosses the boundary too ("even one outside the boundary
           of the inline containing that space, provided both spaces are within the same inline formatting
           context"). So an inline box's text is not a second run beside this one — it IS this run, and the
           descent adds it in document order exactly as a text child of `parent` would be. Each character
           carries its OWN element as its style, which is what makes css-text-3 §5.5's per-element questions
           (the advance measure, and the `white-space` at a boundary) answerable per character afterwards. */
        /* THE TWO EDGES BRACKET THE DESCENT, which is the whole of §2.2's placement: emitted in call order, so
           the opening edge precedes the box's content in the run and the closing edge follows it, and the
           run's own break-position mapping is what then puts each on the line its fragment is on. They are
           emitted even when both are ZERO, because an edge occupies a POSITION and that position is what says
           which line an otherwise-empty inline box sits on. */
        text_run_measure_add_box_edge(m, el, is_intrinsic_edge_px(el, false));
        is_walk(m, el);
        text_run_measure_add_box_edge(m, el, is_intrinsic_edge_px(el, true));
        return;
    }
    if (in_inline_box)
        DFAILF("CSS 2.2 §9.2.1.1 \"Anonymous block boxes\"' BLOCK-IN-INLINE, or an atomic inline nested one box "
               "deeper — and THIS WALK MAY NOT GUESS WHICH. The child sits inside a `display: inline` box this "
               "walk descended into, so §9.2.1's dispatch has said nothing about its level: that question was "
               "asked over the MEASURED box's child list and an inline box's own children were not in it. If "
               "the child is BLOCK-LEVEL the algorithm is §9.2.1.1's breaking, which rebuilds the BOX TREE "
               "rather than adding a term to a sum — \"When an inline box contains an in-flow block-level box, "
               "the inline box (and its inline ancestors within the same line box) is broken around the "
               "block-level box\", and \"The line boxes before the break and after the break are enclosed in "
               "anonymous block boxes, and the block-level box becomes a sibling of those anonymous boxes\", so "
               "the measured box stops establishing ONE inline formatting context and becomes §9.4.1 \"Block "
               "formatting contexts\"' stack of three. If it is INLINE-LEVEL it is the atomic inline this same "
               "function names for a direct child, and the missing thing is the item pair rather than the box "
               "tree. THE LEVEL OF A CHILD IS core/layout/block_flow.c's `bf_child_kind` AND IT IS `static` "
               "THERE, so neither arm can be written until it is an exported entry of core/layout/block_flow.h "
               "over one child node — the same entry this file's own §9.4.1 arm waits on. BUILD IT, then split "
               "this crash on its answer. %s",
               box_subject_node(n, nbuf, sizeof nbuf));
    DFAILF("CSS 2.2 §9.2.2 \"Inline-level elements and inline boxes\"' ATOMIC INLINE-LEVEL BOX THAT IS NOT "
           "REPLACED — an `inline-block`, an `inline-flex`, an `inline-grid` or an `inline-table`, and the list "
           "is closed rather than illustrative. §9.2.1's dispatch has already established that the measured box "
           "\"either contains only block-level boxes or establishes an inline formatting context and thus "
           "contains only inline-level boxes\" and that it is the second, so a DIRECT child reaching here is "
           "inline-level; it failed the `display: inline` test above and `replaced_element_of` says it is not "
           "replaced, which leaves exactly those four. ITS ITEM IS THE ONE `is_atomic_replaced` ALREADY EMITS "
           "and its size is `intrinsic_inline_sizes` one level down under css-sizing-3 §2.2 \"Intrinsic Size "
           "Contributions\"' outer size (\"Intrinsic size contributions are based on the outer size of the box; "
           "for this purpose, auto margins are treated as zero\"), which `is_intrinsic_edge_px` answers on both "
           "sides. WHAT IS MISSING IS THAT THE ITEM CARRIES ONE NUMBER: core/layout/text_run.h's `TextRunItem` "
           "holds a single `CssPx size` and `text_run_measure_add_atomic` takes one, and a REPLACED box is the "
           "only atomic inline for which that is enough — its used width does not depend on where lines break, "
           "so css-sizing-3 §5.1 \"Intrinsic Sizes\" gives it two EQUAL numbers, while an `inline-block`'s own "
           "content wraps and gives it two different ones. BUILD THE PAIR ON THE ITEM — a min-content and a "
           "max-content size on `TEXT_RUN_ITEM_ATOMIC` and on `text_run_measure_add_atomic`, with text_run.c's "
           "two accumulators reading their own — which is the SAME missing thing `is_require_acyclic` above "
           "names for a percentage `width` on a replaced box, so one diff retires both. %s",
           box_subject_node(n, nbuf, sizeof nbuf));
}

/* CSS 2.2 §9.4.2's INLINE FORMATTING CONTEXT, MEASURED — the walk this component was written as, reached only
   through §9.2.1's dispatch below and therefore only for a box whose child list holds no block-level box. */
static IntrinsicInlineSizes is_inline_context(lxb_dom_element_t *el)
{
    TextRunMeasure m;
    IntrinsicInlineSizes out;
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *c;

    text_run_measure_init(&m);
    for (c = n->first_child; c != NULL; c = c->next) is_child(&m, el, c, false);
    /* THE MEASUREMENT DOES NOT EXIST UNTIL THIS RUNS, and that is [UAX14]'s doing rather than a lifecycle
       anybody chose: its rules read forward past the boundary they decide (LB25's `PO × OP IS NU` by three
       characters) and LB9 puts an unbounded run of combining marks between the two, so no per-character state
       can settle a break as the character arrives. core/layout/text_run.h states it in full. It RETAINS what
       the walk collected — CSS 2.2 §9.4.2's fill is a third partition over the same [UAX14] pass — so the
       release below is this function's to make and the two numbers are read between them. */
    text_run_measure_finish(&m);
    out.min_content = text_run_measure_min_content(&m);
    out.max_content = text_run_measure_max_content(&m);
    /* §5.1's answer is a PAIR OF NUMBERS and not a view onto the run, so the measurement ends here: this
       component asks §9.4.2 nothing, and a caller of it holds no items to be handed. */
    text_run_measure_release(&m);
    return out;
}

/* CSS 2.2 §9.4.1 "Block formatting contexts"' CONTEXT, whose intrinsic inline sizes are a MAXIMUM AND NOT A
   SUM, and the section says why in one sentence: "in a block formatting context, each box's left outer edge
   touches the left edge of the containing block". The children therefore OVERLAP in the inline axis instead of
   sharing it, so css-sizing-3 §5.2 "Intrinsic Contributions"' hypothetical float — "a box's min-content
   contribution / max-content contribution in each axis is the size of the content box of a hypothetical
   auto-sized float that contains only that box" — must be as wide as the WIDEST of them and no wider. That is
   the whole of the difference from §9.4.2's walk above, and it is why the two share no step.
   THE EMPTY CHILD LIST IS THIS ARM'S ONLY COMPLETE CASE AND IT IS A REAL ANSWER, not a floor: a maximum over
   no boxes is zero, which is the same number §9.4.2's walk returns for the same document, so routing CSS 2.2
   §9.2.1's third state — a block container with no in-flow child at all — to this arm rather than to that one
   costs nothing and keeps the dispatch a single question. An empty `<td>` is the common shape and
   core/layout/table_column_width.c asks for one on every table.
   WHAT IS NOT COVERED, AND IT IS THE LIST RATHER THAN THE TERM: the term is `is_intrinsic_edge_px` on both
   sides around `intrinsic_inline_sizes` ONE LEVEL DOWN, which is §2.2's outer size over a child whose own used
   inline size is itself an intrinsic one, and every piece of that is built. WHICH BOXES to take the maximum
   over is not, and that narrowing is a CRASH and not a named residual: an arm that guessed a child's level
   would answer a WRONG width for a real document rather than a narrower one, so there is nothing here that is
   right-but-incomplete to name — the crash below carries the whole of what is missing. */
static IntrinsicInlineSizes is_block_context(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *c;
    IntrinsicInlineSizes out;
    char nbuf[160];

    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    for (c = n->first_child; c != NULL; c = c->next) {
        lxb_dom_element_t *ch;

        /* THE NODES THAT GENERATE NO BOX, asked in the SAME ORDER and through the SAME components as `is_child`
           above, because the two walks are the two arms of one dispatch and a child either walk skipped and the
           other did not would be one document with two box lists. */
        switch (c->type) {
        case LXB_DOM_NODE_TYPE_TEXT:
            /* CSS 2.2 §9.2.2.1 "Anonymous inline boxes"' collapsed run generates no box; a run that survives is
               INLINE-LEVEL content beside a block-level box, which is §9.2.1.1's anonymous block box and falls
               to the crash below. */
            if (!block_flow_text_child_generates_box(el, c)) continue;
            break;
        case LXB_DOM_NODE_TYPE_COMMENT:
        case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
        case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
            continue;
        case LXB_DOM_NODE_TYPE_ELEMENT:
            ch = lxb_dom_interface_element(c);
            /* css-display-3 §2.5 "Box Generation: the none and contents keywords": "The element and its
               descendants generate no boxes or text sequences." */
            if (is_computed_is(ch, "display", "none")) continue;
            /* §9.3.1 takes an absolutely positioned box out of normal flow, so it is not one of the boxes
               §9.4.1's stack holds and §5.2's hypothetical float does not have to contain it. */
            if (is_computed_is(ch, "position", "absolute") || is_computed_is(ch, "position", "fixed")) continue;
            if (!is_computed_is(ch, "float", "none"))
                DFAILF("CSS 2.2 §9.5 \"Floats\" takes this child off §9.4.1's stack, and css-sizing-3 §5.2 "
                       "\"Intrinsic Contributions\" still counts it — §5.2's hypothetical float \"contains only "
                       "that box\", and a float INSIDE that hypothetical box sits BESIDE its in-flow siblings "
                       "rather than above them, so its contribution is neither one more operand of the maximum "
                       "below nor a term added to one and there is no arm here that is right by default. §9.4.1 "
                       "\"Block formatting contexts\" says the same thing from the other end: each box's left "
                       "outer edge touches the containing block's left edge \"even in the presence of floats, "
                       "although a box's line boxes may shrink due to the floats\". BUILD §9.5.1 \"Positioning "
                       "the float: the 'float' property\"'s placement, which core/layout/line_box.c and "
                       "core/layout/flow_position.c both name as the same absent capability. %s",
                       box_subject_node(c, nbuf, sizeof nbuf));
            break;
        default:
            DFAILF("a node type CSS 2.2 §9.2 \"Controlling box generation\" does not describe is inside a block "
                   "container being measured — the tree this walk iterates holds elements, text, comments, "
                   "processing instructions and a doctype, and a CDATA section, a document or a fragment is not "
                   "a child any parser this engine runs produces there. Find the writer that inserted it. %s",
                   box_subject_node(c, nbuf, sizeof nbuf));
        }
        DFAILF("css-sizing-3 §5.2 \"Intrinsic Contributions\" is a MAXIMUM over this box's in-flow children "
               "here, and WHICH BOXES those children are is the one input this walk does not have. THE TERM IS "
               "BUILT AND THE LIST IS NOT: §5.2 states each contribution over the child's OUTER size — "
               "css-sizing-3 §2.2 \"Intrinsic Size Contributions\": \"Intrinsic size contributions are based on "
               "the outer size of the box; for this purpose, auto margins are treated as zero\" — so one "
               "operand is `is_intrinsic_edge_px` on each side around `intrinsic_inline_sizes` ONE LEVEL DOWN, "
               "and all three of those exist. WHAT DOES NOT is the enumeration: CSS 2.2 §9.2.1.1 \"Anonymous "
               "block boxes\" says \"if a block container box (such as that generated for the DIV above) has a "
               "block-level box inside it (such as the P above), then we force it to have only block-level "
               "boxes inside it\", so the boxes to maximise over are the BLOCK-LEVEL children plus ONE "
               "anonymous block box per maximal run of inline-level children — and whether a given child is "
               "block-level or inline-level is core/layout/block_flow.c's `bf_child_kind`, which is `static` "
               "there. BUILD IT AS AN EXPORTED ENTRY OF core/layout/block_flow.h over ONE child node, answering "
               "§9.2.1 \"Block-level elements and block boxes\"' level and nothing else, and this walk then "
               "iterates the same list core/layout/block_flow.c's own stack does. "
               "`block_flow_anonymous_boxes` IS NOT THAT ENTRY AND MUST NOT BE REACHED FROM HERE, for a reason "
               "that is a cycle and not a preference: it answers each run's `content_y` and its `height`, which "
               "are §9.4.1's BLOCK-axis placement, and a run's height is core/layout/line_box.h's over its "
               "container's used CONTENT WIDTH — which for the only two boxes that ask this walk at all, CSS "
               "2.2 §10.3.5 \"Floating, non-replaced elements\"' float and §10.3.9's `inline-block`, IS "
               "core/layout/used_value.h's shrink-to-fit over the number this walk is being run to produce. So "
               "the exported entry must answer a LEVEL and place nothing. %s",
               box_subject_node(c, nbuf, sizeof nbuf));
    }
    return out;
}

IntrinsicInlineSizes intrinsic_inline_sizes(lxb_dom_element_t *el)
{
    IntrinsicInlineSizes out;

    DCHECK(el != NULL, "css-sizing-3 §5.1's intrinsic inline sizes were asked for with no element");
    /* THE REPLACED QUESTION IS ASKED FIRST, exactly as CSS 2.2 §10.3 asks it before the box type: a replaced
       element's intrinsic sizes come from its NATURAL DIMENSIONS and not from its children, which for most of
       them it has none of in the DOM at all. Answering it from this walk would report an `img` as being as wide
       as the text of its `alt` attribute is not. */
    if (replaced_element_of(el).replaced)
        DFAIL("css-sizing-3 §5.1 \"Intrinsic Sizes\" gives a REPLACED ELEMENT its own rules, and this walk over "
              "children is not them: the section defines the intrinsic sizes \"of replaced elements WITHOUT "
              "NATURAL SIZES\" from the preferred aspect ratio and the available space, and one WITH a natural "
              "size takes it — CSS 2.2 §10.3.2 \"Inline, replaced elements\"' first arm, \"if 'height' and "
              "'width' both have computed values of 'auto' and the element also has an intrinsic width, then "
              "that intrinsic width is the used value\". The natural dimensions are already answered "
              "(core/layout/replaced_element.h) and core/layout/used_value.c already runs §10.3.2 over them, so "
              "what is missing is only the wiring: make the min-content and max-content sizes of a replaced box "
              "§10.3.2's used width, with §5.1's zero-min-content arm for one that has a ratio and no natural "
              "size");
    /* §9.4.2's OWN CONDITION, asked before anything is measured: "an inline formatting context is established "
       by a block container box that CONTAINS NO BLOCK-LEVEL BOXES". A box that is not a block container at all
       does not establish either of CSS 2.2's two formatting contexts, so neither algorithm applies to it. */
    {
        char *d = is_computed(el, "display");
        bool container = block_flow_display_is_block_container(d);

        free(d);
        if (!container)
            DFAIL("css-sizing-3 §5.1's intrinsic inline sizes were asked of a box that is NOT A BLOCK CONTAINER, "
                  "so neither of CSS 2.2 §9.4's two formatting contexts is what lays its content out and "
                  "neither §9.4.2's line boxes nor §9.4.1's stack is the algorithm. Which module owns it is its "
                  "own `display`: css-flexbox-1 §9.9 \"Intrinsic Sizes\" derives a flex container's from its "
                  "flex lines, css-grid-2 §11.5 sizes the TRACKS and the container's intrinsic size follows "
                  "from them, and CSS 2.2 §17.5.2's automatic table layout derives a table's from its COLUMNS' "
                  "cell minima and maxima over the box structure §17.2.1 Anonymous table objects generates. "
                  "THE TABLE ARM IS NO LONGER AN ALGORITHM TO WRITE AND THIS LINE USED TO SAY IT WAS: "
                  "§17.2.1's structure is core/layout/table_box.h's, §17.5 Visual layout of table contents' "
                  "grid is core/layout/table_grid.h's, and §17.5.2 itself is core/layout/table_width.h's — a "
                  "reader following the old sentence would have built the whole of it a second time. WHAT IS "
                  "LEFT FOR THIS ENTRY IS AN EXPORT, NOT A LAYOUT: §17.5.2.2 Automatic table layout names the "
                  "pair this function returns in its own final rules — \"the minimum width required by all the "
                  "columns plus cell spacing or borders (MIN)\" and \"the maximum width required by the "
                  "columns plus cell spacing or borders (MAX)\" — and core/layout/table_width.c computes both "
                  "to reach a USED width without publishing either, over per-column minima and maxima "
                  "core/layout/table_column_width.h already exports. So MAKE §17.5.2.2's MIN and MAX a second "
                  "producer of `IntrinsicInlineSizes` for a table box; the flex and grid arms each need their "
                  "own box tree first and none of the three is this walk with a different accumulator");
    }
    /* CSS 2.2 §9.2.1 "Block-level elements and block boxes"' ALTERNATIVE, ASKED ONCE OVER THE WHOLE CHILD LIST
       AND BEFORE EITHER ALGORITHM RUNS: "A block container box either contains only block-level boxes or
       establishes an inline formatting context and thus contains only inline-level boxes." The two are
       different algorithms sharing no step — §9.4.2's sum along a line and §9.4.1's maximum down a stack — so
       which one this box gets is a fact about its CHILD LIST and never a case discovered part-way through a
       measurement. A walk that classified as it accumulated had already begun summing an inline run before it
       met the block-level child that made the sum the wrong operation.
       IT IS core/layout/block_flow.h's PREDICATE AND NOT A SECOND COPY OF §9.4.2's CONDITION, which is the
       reason that predicate is exported: core/layout/block_flow.c's own stack chooses between the same two
       sections over the same child list, and two answers to one question is one document with two box trees,
       free to disagree about whether a run of white space is content.
       ITS CLASSIFICATION REFUSES MORE THAN THIS FILE'S DOES, and that is a consequence to know rather than a
       defect to work around: a float, a `display: contents` child, a misparented table-internal box and an
       unmodelled `display` each abort inside that classification now, naming block_flow.c's own reason. For
       three of the four that reason is sharper than what this file said; for a FLOAT it names §9.4.1's
       clearance where §9.4.2's shortened line box is what a reader arriving from an intrinsic size wants, and
       both name §9.5.1's placement as the thing to build. */
    if (block_flow_establishes_inline_context(el)) out = is_inline_context(el);
    else out = is_block_context(el);
    /* THE TWO ARE NON-NEGATIVE because every advance summed into them is, and a negative intrinsic size would
       make CSS 2.2 §10.3.5's formula produce a negative used width for a box with content in it — which
       css-sizing-3 §3.3's "as the content width and height cannot be negative, this computation is floored at
       zero" would then hide rather than the derivation being fixed. Asserted here, at the boundary where the
       two numbers leave this component, so a caller never has to floor them. */
    DCHECK(out.min_content.px >= 0.0 && out.max_content.px >= 0.0,
           "css-sizing-3 §5.1's intrinsic inline sizes came out NEGATIVE. Each is a sum of advance measures, "
           "every one of which is a non-negative OpenType 'hmtx' advanceWidth times a non-negative computed "
           "`font-size`, so a negative here is arithmetic that lost a sign rather than a document");
    return out;
}
