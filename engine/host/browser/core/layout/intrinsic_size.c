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
#include "core/layout/flex_intrinsic_size.h"
#include "core/layout/flex_item.h"
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
    CssPx sum, pad;

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
    } else if (margin.kind == CSS_LENGTH_ABSOLUTE) {
        /* AN ABSOLUTE LENGTH IS ALREADY §5.2.1's ANSWER AND HAS NO BASIS IN IT. §5.2.1's rule is stated of "a
           cyclic percentage", so a value carrying none is not a thing the section resolves — the computed
           length IS the contribution, at this basis and at every other. It is a separate arm and not a zero
           handed to the resolution entry because `css_length_resolve_pct` is css-values-4 §10.11 "Computed
           Value"'s USED-VALUE-TIME SIMPLIFICATION, whose subject is the two kinds that carry a percentage:
           routing an absolute length through it asks a percentage question of a value that has none, which is
           the dispatch its own assert refuses. core/layout/used_value.c and core/layout/table_width.c split
           the identical three-shape grammar the same way at every one of their sites. */
        sum = margin.px;
    } else {
        DCHECKF(margin.kind == CSS_LENGTH_PERCENTAGE || margin.kind == CSS_LENGTH_CALCULATED,
                "`%s` computed to none of the three shapes CSS 2.1 §8.3 \"Margin properties\" admits",
                side[IS_EDGE_MARGIN]);
        /* §5.2.1 resolves the PERCENTAGE against zero and not the whole value, which is the contrast the same
           section draws one paragraph up: a cyclic max or preferred size on a non-replaced box is "treated …
           as that property's initial value", while a margin's cyclic percentage "is resolved against zero". So
           `margin-left: calc(10px + 50%)` contributes 10px, and ONE BASIS COVERS THE TWO KINDS THAT CARRY A
           PERCENTAGE — not the three the grammar admits — because css-values-4 §10.11 "Computed Value" left
           the percentage inside the math function and §5.6 "Mixing Percentages and Dimensions" adds the pair's
           two terms in one step. */
        sum = css_length_resolve_pct(margin, css_px(0.0));
    }
    /* §8.4's <padding-width> has no `auto` and no keyword at all, so the split is two-way rather than three. */
    if (padding.kind == CSS_LENGTH_ABSOLUTE) {
        pad = padding.px;
    } else {
        DCHECKF(padding.kind == CSS_LENGTH_PERCENTAGE || padding.kind == CSS_LENGTH_CALCULATED,
                "`%s` computed to a keyword. CSS 2.1 §8.4 \"Padding properties\"' <padding-width> grammar is a "
                "length or a percentage and nothing else",
                side[IS_EDGE_PADDING]);
        pad = css_length_resolve_pct(padding, css_px(0.0));
    }
    /* THE FLOOR IS THE PROPERTY'S RANGE AND IT IS REACHED ONLY THROUGH A MATH FUNCTION. CSS 2.1 §8.4 says
       outright that "Unlike margin properties, values for padding values cannot be negative", and §5.1's range
       restriction drops a negative LITERAL — but css-values-4 §9.1 "Numeric Functions" exempts a math function
       from that check and moves it to the result: "numeric functions returning out-of-range values never cause
       a declaration to become invalid", and instead "the value of a numeric function is clamped to the range
       allowed in the context it is used at computed value time if possible, and at used value time otherwise".
       §5.2.1's ZERO BASIS is precisely what makes `calc(50% - 10px)` land out of range HERE and in range at a
       real width, so this clamp belongs at this resolution and could not have run at the cascade. IT IS OVER
       BOTH ARMS AND NOT OVER THE RESOLUTION ALONE: a math function whose Sum kept no percentage term
       (`calc(10px - 20px)`) is an ABSOLUTE computed value that §9.1 exempted from §5.1's parse-time check just
       the same, so the arm that never sees a basis is out of range for the same reason and by the same
       sentence. `css_px_max` and not an `if`, so the clamped-away operand's environment facts stay in the
       domain — core/layout/used_value.c's own padding arm states that reason in full, splits its two arms the
       same way, and clamps their one result exactly here. A MARGIN IS NOT CLAMPED, which is §8.3's "negative
       values for margin properties are allowed". */
    sum = css_px_add(sum, css_px_max(pad, css_px(0.0)));
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
/* THE ORIGIN OF THE CHILD USED TO BE A PARAMETER, and it is gone with the second crash it existed to name.
   It distinguished a child of the MEASURED box — whose whole list CSS 2.2 §9.2.1 "Block-level elements and
   block boxes"' dispatch had classified — from a child of a `display: inline` box this walk descended into,
   about which that dispatch said nothing, so that a BLOCK-LEVEL child could be refused as §9.2.1.1's
   block-in-inline in the second case and as a came-apart classification in the first. THAT ASYMMETRY IS
   RETIRED: core/layout/block_flow.h enumerates §9.2.1.1's box list in CONTENT order, so a block-level box
   reached through an inline box is a box on §9.4.1's stack exactly like a direct child, and the run this walk
   measures ENDS at it either way. A block-level box is therefore unreachable from both origins for the same
   reason, and one assertion states it once. */

/* ONE RUN BEING MEASURED, WHICH IS MORE THAN THE ACCUMULATOR: the run's END is a node the walk may meet at ANY
   DEPTH, because §9.2.1.1 breaks an inline box around a block-level box inside it, so `end` cannot be tested
   by the child loop that started the walk. `past_end` is what carries that answer back OUT of the descent —
   and it is read as well as written, because §9.2.1.1's next sentence makes the difference observable: "if a
   border had been set on the P element in the above example, the border would be drawn around C1 (open at the
   end of the line) and C2 (open at the start of the line)". An inline box the run ends INSIDE is open at the
   end, so its TRAILING edge is not emitted — which is exactly the `past_end` test after the descent. */
typedef struct {
    TextRunMeasure *m;
    lxb_dom_node_t *end;   /* EXCLUSIVE, and reachable at any depth; NULL runs to the end of the content */
    bool past_end;
} IsRun;

static void is_child(IsRun *r, lxb_dom_element_t *parent, lxb_dom_node_t *n);

static void is_walk(IsRun *r, lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *c;

    /* THE ONLY CALLER IS THE INLINE-BOX DESCENT BELOW, so every child this reaches is inside one. */
    for (c = n->first_child; c != NULL && !r->past_end; c = c->next) is_child(r, el, c);
}

static void is_child(IsRun *r, lxb_dom_element_t *parent, lxb_dom_node_t *n)
{
    lxb_dom_element_t *el;
    char *d;
    bool inline_box;
    char nbuf[160];

    /* THE RUN'S END, TESTED AT EVERY DEPTH AND BEFORE ANYTHING ELSE. It is CSS 2.2 §9.2.1.1's block-level box
       — the one that "becomes a sibling of those anonymous boxes" — so it is not content of this run at any
       depth, and the walk stops rather than returning: every node after it in content order belongs to a
       LATER box on §9.4.1's stack. */
    if (r->past_end) return;
    if (n == r->end) { r->past_end = true; return; }
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_TEXT:
        /* CSS 2.2 §9.2.2.1 "Anonymous inline boxes" first: a run of white space this element's `white-space`
           collapses away "does not generate any anonymous inline boxes", so it is not content and contributes
           nothing. core/layout/block_flow.h answers that for every walk over a block container's children, and
           asking it here rather than re-deriving it is what keeps this walk and §9.4.1's agreeing about what a
           document's white space is. */
        if (block_flow_text_child_generates_box(parent, n)) text_run_measure_add_text(r->m, parent, n);
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
        /* CSS 2.2 §9.2.2 makes a REPLACED element an ATOMIC inline-level box wherever its `display` puts it,
           so the `inline` above does not settle which of the two this is. It is asked before the descent
           because an atomic inline has no text of this run inside it to descend to. */
        if (replaced_element_of(el).replaced) { is_atomic_replaced(r->m, el); return; }
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
        text_run_measure_add_box_edge(r->m, el, is_intrinsic_edge_px(el, false));
        is_walk(r, el);
        /* §9.2.1.1's "open at the end of the line": the run ended INSIDE this inline box, so this fragment
           has no closing edge and the next anonymous block box's fragment of the same box has no opening
           one. The leading edge above was emitted because the run did NOT start inside it — the entry that
           resumes a run mid-tree never calls this arm for an already-open ancestor. */
        if (r->past_end) return;
        text_run_measure_add_box_edge(r->m, el, is_intrinsic_edge_px(el, true));
        return;
    }
    /* THE LEVEL OF EVERY CHILD THIS WALK REACHES, ASSERTED ONCE FOR BOTH ORIGINS. Two different sections
       guarantee it and they now guarantee the same thing. For a child of the MEASURED box it is CSS 2.2 §9.2.1
       "Block-level elements and block boxes"' dispatch, asked over the whole list before this walk ran ("A
       block container box either contains only block-level boxes or establishes an inline formatting context
       and thus contains only inline-level boxes"), plus §9.2.1.1's run delimitation, which ends a run AT a
       block-level box rather than inside it. For a child of a `display: inline` box this walk descended into
       it is §9.2.1.1's SECOND paragraph, RUN: an inline box containing an in-flow block-level box is broken
       around it and the block-level box "becomes a sibling of those anonymous boxes", so a container holding
       one is not §9.4.2's context at all and core/layout/block_flow.c's classification sends it down §9.4.1's
       stack — where the run delimitation refuses the split it cannot yet express, one call before this walk
       would meet it.
       THE OTHER TWO KINDS CANNOT REACH THIS LINE EITHER, and by this function's own tests rather than by
       anyone else's: §9.2's non-generating nodes and §9.3.1's out-of-flow box each returned above, and §9.5's
       float crashed above under §9.4.2's own reason. The assert is what keeps all four derivations honest if
       any of those tests is ever moved.
       WHAT THIS LINE USED TO SAY, AND WHY THE CHANGE IS NOT A NARROWING: a `DFAILF` stood on the block-level
       arm telling its reader to build §9.2.1.1's breaking, and it named the right file and the right function
       for HALF the work — the classification, which descends now. Its second half said "this file's §9.4.1 arm
       below maximises over the three boxes it produced, with no arm needed here at all", and that is true only
       of a box list this component cannot yet be handed: §9.2.1.1 makes ONE child node yield THREE boxes, and
       `block_flow_anonymous_box_end` returns a SIBLING, so there is no run for `is_block_context` to maximise
       over until that boundary becomes a position in content order. The absence is named at that boundary now,
       once, instead of here and in core/layout/line_box.c under two different halves of one sentence. */
    DCHECK(block_flow_child_kind(parent, n) == BLOCK_FLOW_CHILD_INLINE,
           "a child of a box whose inline formatting context is being measured is not INLINE-LEVEL after this "
           "walk's own tests for a non-generating node, for §9.3.1's out-of-flow box and for §9.5's float have "
           "all let it through — so this run holds a box core/layout/block_flow.h says is not in it, and the "
           "two classifications have come apart. A BLOCK-LEVEL answer here is the sharpest form of that: "
           "CSS 2.2 §9.2.1.1 \"Anonymous block boxes\" puts a block-level box on the CONTAINER's stack, as a "
           "sibling of the anonymous boxes, whether it is a direct child or is reached through an inline box "
           "it breaks — so no run this walk is ever handed may contain one, and whichever of "
           "`bf_content_kind` and `block_flow_anonymous_box_end` stopped agreeing with the other is the fix");
    DFAILF("CSS 2.2 §9.2.2 \"Inline-level elements and inline boxes\"' ATOMIC INLINE-LEVEL BOX THAT IS NOT "
           "REPLACED — an `inline-block`, an `inline-flex`, an `inline-grid` or an `inline-table`, and the list "
           "is closed rather than illustrative. §9.2.1's dispatch has already established that the measured box "
           "\"either contains only block-level boxes or establishes an inline formatting context and thus "
           "contains only inline-level boxes\" and that it is the second, and the assert directly above "
           "establishes the same of a child reached THROUGH an inline box, so this child is inline-level "
           "either way. It failed the `display: inline` test above and `replaced_element_of` says it is not "
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

/* CSS 2.2 §9.4.2's INLINE FORMATTING CONTEXT, MEASURED, over ONE RUN of `el`'s CONTENT. The run is given as
   the inline box that is OPEN where it starts, the first node inside that box to measure, and the node it ends
   BEFORE — a shape the ENTRY below composes out of core/layout/block_flow.h's `BlockFlowRun`, which names a
   run by the two breaks that bracket it. It is private because those three are not independent: the open box
   and the first node are derived from one break, and a caller free to pair them itself could open a fragment
   the run does not start in.
   THE RUN CLOSES ITS OPEN ANCESTORS ON THE WAY OUT, WHICH IS §9.2.1.1's OWN SENTENCE: "if a border had been
   set on the P element in the above example, the border would be drawn around C1 (open at the end of the line)
   and C2 (open at the start of the line)". A fragment the run STARTS inside is open at the start — no leading
   edge, and this loop never calls `is_child` on the ancestor itself, which is what withholds it — and it is
   closed here when the walk leaves it. A fragment the run ENDS inside is open at the end, which `is_child`
   answers with `past_end` at its own site.
   THE STEP OUT IS TO THE ANCESTOR'S NEXT SIBLING AND NEVER TO THE ANCESTOR, which sounds like a detail and is
   the defect this loop was first written with: resuming at the ancestor re-enters the whole fragment that was
   just closed, so a container's LAST run walked its own content a second time and its maximum came back at
   nearly twice the width. `at` is allowed to be NULL — an ancestor with nothing after it inside its own parent
   still has a closing edge to emit, which is exactly the "even if either side is empty" half of the split. */
static IntrinsicInlineSizes is_run_sizes(lxb_dom_element_t *el, lxb_dom_element_t *open, lxb_dom_node_t *at,
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
        lxb_dom_node_t *c, *box;

        for (c = at; c != NULL && !r.past_end; c = c->next) is_child(&r, open, c);
        if (r.past_end) break;
        box = lxb_dom_interface_node(open);
        if (box == root) break;
        text_run_measure_add_box_edge(&m, open, is_intrinsic_edge_px(open, true));
        DCHECK(box->parent != NULL && box->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "CSS 2.2 §9.2.1.1's run was inside a box whose parent is not an element, so the walk cannot "
               "leave it — the ancestors of every position in a container's content are inline boxes up to "
               "the container itself, and this chain does not reach it");
        at = box->next;
        open = lxb_dom_interface_element(box->parent);
    }
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

IntrinsicInlineSizes intrinsic_inline_run_sizes(lxb_dom_element_t *el, BlockFlowRun run)
{
    DCHECK(el != NULL, "CSS 2.2 §9.4.2's context was measured with no block container to style it");
    if (run.after == NULL) return is_run_sizes(el, el, lxb_dom_interface_node(el)->first_child, run.end);
    DCHECK(run.after->parent != NULL && run.after->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "CSS 2.2 §9.2.1.1's run was started after a node with no element parent, so there is no box for it "
           "to be a fragment of — the node a run follows is a child of the container or of an inline box the "
           "break itself split, and either way its parent is an element whose style the fragment carries");
    return is_run_sizes(el, lxb_dom_interface_element(run.after->parent), run.after->next, run.end);
}

/* css-sizing-3 §5.2's CONTRIBUTION OF ONE BOX ON THE STACK, out of the box's own two INNER sizes and its two
   edges — §2.2 "Intrinsic Size Contributions": "Intrinsic size contributions are based on the outer size of the
   box; for this purpose, auto margins are treated as zero."
   §2.2's FLOOR IS THE SECOND SENTENCE OF THAT PARAGRAPH AND IT IS LIVE HERE, not a formality: "if the ideal
   max-content contribution would be smaller than the min-content contribution (e.g. due to the use of negative
   margins), the effective max-content contribution is floored by the min-content contribution." One box's two
   contributions share ONE edge sum, so a negative margin ON THIS BOX cannot invert them — what can is a
   negative margin on an inline box INSIDE it, because CSS 2.1 §8.3 "Margin properties" allows one ("negative
   values for margin properties are allowed") and §2.1's two readings of the run add it a different number of
   times: the max-content size sums the whole line once, the min-content size takes the widest SEGMENT, and a
   negative margin subtracts from the first without touching the second. So the floor is arithmetic that fires
   on a real document, and asserting the order instead of applying it would be a crash on a page CSS 2.1
   permits. */
static IntrinsicInlineSizes is_contribution(IntrinsicInlineSizes inner, CssPx edge)
{
    IntrinsicInlineSizes out;

    out.min_content = css_px_add(inner.min_content, edge);
    out.max_content = css_px_max(css_px_add(inner.max_content, edge), out.min_content);
    return out;
}

IntrinsicInlineSizes intrinsic_outer_contribution(lxb_dom_element_t *el, IntrinsicInlineSizes inner)
{
    /* THE INNER PAIR IS NOT ASSERTED IN EITHER DIRECTION, and both refusals were written here and deleted,
       which is worth keeping because each looked like an invariant this codebase owes. An ORDER between them
       is exactly what §2.2's own floor exists to repair — "if the ideal max-content contribution would be
       smaller than the min-content contribution (e.g. due to the use of negative margins), the effective
       max-content contribution is floored by the min-content contribution" — so asserting it would crash on a
       page CSS 2.1 §8.3 "Margin properties" permits ("negative values for margin properties are allowed"),
       which is the reason `is_contribution` states in full at its own site. NON-NEGATIVITY is the same
       sentence one term earlier: the operand that inverts the pair is a negative margin on a box INSIDE the
       run, and nothing stops it driving a max-content sum below zero before this floor sees it.
       WHAT IS ASSERTED IS THE SHAPE, at `intrinsic_inline_sizes`'s own boundary, where the pair leaves the
       walk that produced it — a value this codebase computed. Here the pair is the CALLER's arithmetic, and
       §2.2's floor is a rule about it rather than a check on it.
       NULL is CSS 2.2 §9.2.1.1's and css-flexbox-1 §4's ANONYMOUS box — see intrinsic_size.h — whose edge sum
       is zero because neither section gives it any declaration to compute one from. */
    if (el == NULL) return is_contribution(inner, css_px(0.0));
    return is_contribution(inner, css_px_add(is_intrinsic_edge_px(el, false), is_intrinsic_edge_px(el, true)));
}

/* css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes"' NON-REPLACED arm, for the two
   properties that would otherwise decide a stacked child's inline size instead of its content: "if the box is
   non-replaced, then the entire value of any max size property or preferred size property (width, max-width,
   height, max-height) specified as an expression containing a percentage … that is cyclic is treated, for the
   purpose of calculating the box's intrinsic size contributions only, as that property's initial value".
   SO A PERCENTAGE IS NOT A REFUSAL AND A LENGTH IS. Every box this walk reaches has the box being measured as
   its containing block, so a percentage here is cyclic BY CONSTRUCTION (the same derivation
   `is_intrinsic_edge_px` states in full) and §5.2.1 substitutes `auto` / `none` — which is the contribution
   this walk already
   computes, so the substitution is the code doing nothing rather than an arm to write. A LENGTH is not cyclic
   and not substituted: it is a real declared inline size this walk does not yet apply, and applying it is
   §10.4's clamp plus css-sizing-3 §3.3's `box-sizing` conversion, neither of which is here. */
static void is_require_intrinsic_inline_size(lxb_dom_element_t *ch, const char *name, const char *initial)
{
    CssLength len = css_computed_length(ch, name);
    char nbuf[160];

    if (len.kind == CSS_LENGTH_KEYWORD) {
        DCHECKF(strcmp(len.keyword, initial) == 0,
                "`%s` computed to the keyword `%s` rather than to its initial value `%s`. CSS 2.1 §10.2 "
                "\"Content width: the 'width' property\" and §10.4 \"Minimum and maximum widths: 'min-width' "
                "and 'max-width'\" admit no other keyword, and css-sizing-3 §3.2 \"Sizing Values: the "
                "<length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and "
                "fit-content values\"'s level-3 additions are ones this engine records no computed-value "
                "rule for — "
                "core/layout/used_value.c asserts the same thing about the same grammar. So this is a value "
                "the cascade produced and no section of it defines",
                name, len.keyword, initial);
        return;
    }
    /* §5.2.1's substitution, which is this walk's ordinary answer. */
    if (len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED) return;
    DFAILF("css-sizing-3 §5.2 \"Intrinsic Contributions\" is a maximum over this box's children's OUTER sizes, "
           "and this child declares its own inline size: `%s` is an absolute LENGTH, which css-sizing-3 §5.2.1 "
           "\"Intrinsic Contributions of Percentage-Sized Boxes\" does NOT substitute away — its substitution "
           "is for a value \"specified as an expression containing a percentage\", and a length is not cyclic "
           "and not one. So this child's contribution is its DECLARED size and not the intrinsic one measured "
           "below, and taking the measured one would report a `<div style=\"width:500px\">x</div>` inside a "
           "float as one glyph wide — a WRONG width for a real document rather than a narrower one, which is "
           "why it crashes here instead of being named as a residual. WHAT TO BUILD IS THE TERM AND NOT THE "
           "LIST: the enumeration below is complete. It is two steps and neither is in this file. CSS 2.1 "
           "§10.4 \"Minimum and maximum widths: 'min-width' and 'max-width'\" clamps the declared `width` "
           "between `min-width` and `max-width` — over the SAME §5.2.1 substitutions, so a percentage `%s` "
           "reaching that step is already `auto`/`none`/zero. css-sizing-3 §3.3 \"Box Edges for Sizing: the "
           "box-sizing property\" then says which box the declared number IS, and this component's own header "
           "records that it never asks that question of ITSELF because its caller applies §3.3 to the RESULT — "
           "for a CHILD this walk IS the caller, so the conversion belongs at this term. %s",
           name, name, box_subject(ch, nbuf, sizeof nbuf));
}

/* CSS 2.2 §9.4.1 "Block formatting contexts"' CONTEXT, whose intrinsic inline sizes are a MAXIMUM AND NOT A
   SUM, and the section says why in one sentence: "in a block formatting context, each box's left outer edge
   touches the left edge of the containing block". The children therefore OVERLAP in the inline axis instead of
   sharing it, so css-sizing-3 §5.2 "Intrinsic Contributions"' hypothetical float — "a box's min-content
   contribution / max-content contribution in each axis is the size of the content box of a hypothetical
   auto-sized float that contains only that box" — must be as wide as the WIDEST of them and no wider. That is
   the whole of the difference from §9.4.2's walk above, and it is why the two share no step.
   THE LIST IS NEITHER THE CHILD NODES NOR A PARTITION OF THEM, AND THAT IS §9.2.1.1's DOING TWICE OVER. Its
   first paragraph forces a mixed container "to have only block-level boxes inside it", so each maximal run of
   inline-level content becomes one ANONYMOUS BLOCK BOX; its second breaks an inline box around an in-flow
   block-level box inside it, so ONE child node can yield THREE boxes — "a block box representing the BODY,
   containing an anonymous block box around C1, the SPAN block box, and another anonymous block box around C2".
   BOTH are core/layout/block_flow.h's enumeration and neither is re-derived here, because two answers to "what
   boxes does this container have" is one document with two box trees.
   THE ENUMERATION IS A LOOP OVER BREAKS AND NOT A SWITCH OVER CHILDREN, and what that retires is worth
   naming: this walk used to classify each child itself and carry an arm for each `BlockFlowChildKind`. §9.5's
   FLOAT was one of those arms, and its refusal is not lost — a float is inside some run now, and the run walk
   crashes on it naming BOTH consequences in one message (the sum along a line and the maximum over the stack,
   since §9.4.2's "line boxes may vary in width if available horizontal space is reduced due to floats" is what
   makes a float change its NEIGHBOURS' operands rather than contribute one). The `-Wswitch` forcing function
   moved with the classification, into the enumeration behind `block_flow_next_block_box`, which is now the
   one place a fifth kind of box would have to be answered for.
   THE ANONYMOUS BOX'S EDGES ARE ZERO AND THAT IS A DERIVATION, not an omission. §9.2.1.1: "the properties of
   anonymous boxes are inherited from the enclosing non-anonymous box …. Non-inherited properties have their
   initial value … the margins will be 0", so its margin box, its border box and its content box are one
   rectangle and §2.2's outer size is its inner size unchanged. Its STYLE for the run inside it is `el`'s, by
   the same sentence.
   AN EMPTY RUN IS A REAL ANSWER AND NOT A CASE TO SKIP: a maximum over no boxes is zero, which is the same
   number §9.4.2's walk returns for the same document, so the range before the first block-level box and the
   one after the last are measured like any other and contribute nothing when they hold nothing. That is also
   why routing CSS 2.2 §9.2.1's third state — a block container with no in-flow child at all — to this arm
   rather than to that one costs nothing and keeps the dispatch a single question. An empty `<td>` is the
   common shape and core/layout/table_column_width.c asks for one on every table. */
static IntrinsicInlineSizes is_block_context(lxb_dom_element_t *el)
{
    lxb_dom_node_t *prev = NULL;
    IntrinsicInlineSizes out;

    out.min_content = css_px(0.0);
    out.max_content = css_px(0.0);
    for (;;) {
        lxb_dom_node_t *brk = block_flow_next_block_box(el, prev);
        IntrinsicInlineSizes one;

        /* §9.2.1.1's ANONYMOUS BLOCK BOX: everything strictly between the previous break and this one. */
        {
            BlockFlowRun run;

            run.after = prev;
            run.end = brk;
            /* §9.2.1.1's ANONYMOUS BLOCK BOX, WHERE THE SECTION GENERATES ONE. A run holding no inline-level
               content is not a box — "we assume that there is an anonymous block box around 'Some text'" — and
               `<div><p></p></div>` has ONE box on its stack, not three. A maximum would not notice the empty
               ones, which is exactly why the test is here: skipping them by the SAME predicate §9.4.1's
               placement skips them by is what keeps the two walking one box list rather than two that agree
               only by arithmetic. */
            if (!block_flow_run_generates_box(el, run)) one = out;
            else one = intrinsic_outer_contribution(NULL, intrinsic_inline_run_sizes(el, run));
        }
        out.min_content = css_px_max(out.min_content, one.min_content);
        out.max_content = css_px_max(out.max_content, one.max_content);
        if (brk == NULL) return out;
        {
            lxb_dom_element_t *ch = lxb_dom_interface_element(brk);

            /* THE CHILD'S OWN DECLARATIONS FIRST, because a `width` this walk cannot apply makes the number
               below the wrong operand rather than an imprecise one. */
            is_require_intrinsic_inline_size(ch, "width", "auto");
            is_require_intrinsic_inline_size(ch, "max-width", "none");
            is_require_intrinsic_inline_size(ch, "min-width", "auto");
            one = intrinsic_outer_contribution(ch, intrinsic_inline_sizes(ch));
        }
        out.min_content = css_px_max(out.min_content, one.min_content);
        out.max_content = css_px_max(out.max_content, one.max_content);
        /* STRICTLY FORWARD, which is what makes this terminate: the enumeration is seeded with the box it just
           returned and is exclusive of it, so no content position is visited twice and the whole loop costs
           one pass over the content however many boxes §9.2.1.1 produced. */
        prev = brk;
    }
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
        bool flex = flex_item_display_is_flex_container(d);

        free(d);
        /* css-flexbox-1 §3 "Flex Containers: the flex and inline-flex display values"' box, WHOSE MODULE
           OWNS IT — the same dispatch this crash below describes, taken rather than described for the one
           `display` that now has a component. §9.9's answer is the same PAIR in the same box as this
           function's, so a consumer never learns which module produced its number. */
        if (flex) return flex_intrinsic_inline_sizes(el);
        if (!container)
            DFAIL("css-sizing-3 §5.1's intrinsic inline sizes were asked of a box that is NOT A BLOCK CONTAINER, "
                  "so neither of CSS 2.2 §9.4's two formatting contexts is what lays its content out and "
                  "neither §9.4.2's line boxes nor §9.4.1's stack is the algorithm. Which module owns it is its "
                  "own `display`, and TWO OF THE THREE ARE NO LONGER AN ALGORITHM TO WRITE HERE. "
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
                  "THE TABLE ARM IS NOT ONE EITHER AND THIS LINE USED TO SAY IT WAS: "
                  "§17.2.1's structure is core/layout/table_box.h's, §17.5 Visual layout of table contents' "
                  "grid is core/layout/table_grid.h's, and §17.5.2 itself is core/layout/table_width.h's — a "
                  "reader following the old sentence would have built the whole of it a second time. WHAT IS "
                  "LEFT FOR THIS ENTRY IS AN EXPORT, NOT A LAYOUT: §17.5.2.2 Automatic table layout names the "
                  "pair this function returns in its own final rules — \"the minimum width required by all the "
                  "columns plus cell spacing or borders (MIN)\" and \"the maximum width required by the "
                  "columns plus cell spacing or borders (MAX)\" — and core/layout/table_width.c computes both "
                  "to reach a USED width without publishing either, over per-column minima and maxima "
                  "core/layout/table_column_width.h already exports. So MAKE §17.5.2.2's MIN and MAX a second "
                  "producer of `IntrinsicInlineSizes` for a table box. WHAT IS LEFT AS A LAYOUT IS THE GRID, "
                  "AND ITS SECTION NUMBER USED TO BE WRONG HERE — this line said css-grid-2 §11.5, which is "
                  "\"Aligning the Grid: the justify-content and align-content properties\" and decides nothing "
                  "about a size. The sentence that governs is §5.2 \"Sizing Grid Containers\": \"The "
                  "max-content size (min-content size) of a grid container is the sum of the grid container's "
                  "track sizes (including gutters) in the appropriate axis, when the grid is sized under a "
                  "max-content constraint (min-content constraint).\" So the operand is the TRACKS, which is "
                  "§12.5 \"Resolve Intrinsic Track Sizes\" inside §12.3 \"Track Sizing Algorithm\" — a whole "
                  "chapter away from where this crash pointed. That arm needs its own box tree and its own "
                  "track list first and is not this walk with a different accumulator");
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
       defect to work around: a `display: contents` child, a misparented table-internal box and an unmodelled
       `display` each abort inside that classification, naming block_flow.c's own reason — which is sharper
       than what this file said, because all three are the same BOX TREE this list is not yet, and building it
       there fixes every walk at once. A FLOAT IS NO LONGER ONE OF THEM AND USED TO BE: that classification
       answered it with §9.4.1's clearance, which is the placement walk's consequence and not an intrinsic
       size's, so a reader arriving from a shrink-to-fit width was handed the wrong half of one absence. The
       classification now reports the float as a FACT and each caller states its own section's consequence at
       its own line — this file's is in `is_block_context`, over §9.4.2's shortened line box. */
    if (block_flow_establishes_inline_context(el)) {
        BlockFlowRun whole;

        /* §9.4.2's context with an ELEMENT to name it is the WHOLE of `el`'s content, which is the run with no
           break on either side of it — the same shape `is_block_context` would reach for a container with no
           block-level box at all, stated here rather than discovered. */
        whole.after = NULL;
        whole.end = NULL;
        out = intrinsic_inline_run_sizes(el, whole);
    } else {
        out = is_block_context(el);
    }
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
