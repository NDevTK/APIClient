/* CSS 2 §9.4.1 "Block formatting contexts" — a box's position. See flow_position.h for the coordinate space,
   for why the root element is the one box that is answered, and for what each other box is waiting on. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/frame/viewport.h"
#include "core/layout/flow_position.h"
#include "core/layout/used_value.h"

static bool fp_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

static bool fp_length_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    CssLength len = css_computed_length(el, name);

    return len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, kw) == 0;
}

/* "The containing block in which the root element lives is a rectangle called the initial containing block"
   (§10.1) — the element whose parent is the Document itself, which is the same test core/layout/used_value.c
   makes for the base case of its own recursion. */
static bool fp_is_root(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* THE INITIAL CONTAINING BLOCK'S WIDTH, asked of the ELEMENT'S own document and never of a running realm — an
   iframe's ICB is 300 CSS pixels wide and its parent's is 1280. The caller has established that the element has
   a box, which is defined over the document being presented, so the viewport exists here rather than being
   checked for a second time. */
static CssPx fp_icb_width(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSContext *dctx;

    DCHECK(n->owner_document != NULL,
           "the initial containing block was asked for an element whose node has no owner document — every node "
           "this engine mints belongs to the document that created it");
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    DCHECK(dctx != NULL && viewport_exists(dctx),
           "CSS 2 §9.4.1's placement was asked for an element whose document is not being presented, so there "
           "is no viewport and §10.1's INITIAL CONTAINING BLOCK does not exist. The caller's own first step is "
           "the has-a-box predicate (core/dom/element_view.h), which is defined over exactly that question — so "
           "the two answers have come apart");
    return viewport_icb_width(dctx);
}

FlowPoint flow_border_box_origin(lxb_dom_element_t *el)
{
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    lxb_dom_node_t *n;
    CssPx ml, mr, outer;

    DCHECK(el != NULL, "a border box's position was asked for with no element");
    n = lxb_dom_interface_node(el);

    /* §9.4.1 PLACES BOXES IN NORMAL FLOW, so a box that is not in one leaves through its own section first —
       §9.3's positioning scheme is what decides which, and it decides it before any placement rule applies. */
    if (fp_computed_is(el, "position", "absolute") || fp_computed_is(el, "position", "fixed"))
        DFAIL("CSS 2 §9.3.1 takes an ABSOLUTELY POSITIONED box out of normal flow, so §9.4.1's placement does "
              "not apply to it at all: its position is §9.3.2's `top`/`right`/`bottom`/`left` resolved against "
              "the containing block §10.1's third and fourth cases give it, and §10.6.4 solves the vertical "
              "pair the same way §10.3.7 solves the horizontal one. Both sections fall back on the STATIC "
              "POSITION — 'where the box would have been in normal flow' — for their `auto` cases, so this is "
              "not an alternative to §9.4.1's flow layout but a consumer of it. BUILD §9.4.1's vertical "
              "stacking first, then §10.3.7 and §10.6.4 over it");
    if (!fp_computed_is(el, "float", "none"))
        DFAIL("CSS 2 §9.5 'Floats' positions a FLOATING box, and §9.4.1's rule does not: a float is shifted to "
              "the left or right edge of its containing block and then down past any earlier float it would "
              "overlap, under §9.5.1's nine constraints. Its own used width is a SHRINK-TO-FIT (§10.3.5), which "
              "core/layout/used_value.c crashes on for want of an intrinsic size, so the extent and the "
              "position are blocked on two different components. BUILD §9.5.1's float placement over the line "
              "boxes it interacts with");
    if (fp_computed_is(el, "display", "inline"))
        DFAIL("CSS 2 §9.4.2 'Inline formatting contexts' places an INLINE box, and it places it on a LINE BOX: "
              "'boxes are laid out horizontally, one after the other, beginning at the top of a containing "
              "block', and the line box that results is as tall as the boxes in it. So an inline box's position "
              "needs line breaking, which needs the text measured with a real font — and the same fact is why "
              "an inline element can be SEVERAL fragments and therefore several rectangles. BUILD the inline "
              "formatting context: the font metrics first, then §9.4.2's line boxes over them");

    /* §10.1's FIRST CASE, which is §9.4.1's base case as well: the root element's containing block is the ICB,
       "anchored at the canvas origin". Both of §9.4.1's rules then reduce to this box's own two margins.
       VERTICALLY, "boxes are laid out one after the other, vertically, beginning at the top of a containing
       block" — there is no preceding sibling, so the box begins at the ICB's top, and §8.3.1 'Collapsing
       margins' states that "margins of the root element's box do not collapse", so the distance from that top
       to the border edge is the root's OWN used `margin-top` and not a collapsed one.
       HORIZONTALLY, "each box's left outer edge touches the left edge of the containing block (for
       right-to-left formatting, right edges touch)" — two answers, and §10.1 says which one applies: "the
       direction property of the initial containing block is the same as for the root element". `direction` is
       not among the properties core/css/css_computed_value.h models, so the two are told apart only where they
       AGREE, which is exactly when the margin box fills the containing block. */
    if (!fp_is_root(n))
        DFAIL("CSS 2 §9.4.1 places a non-root block-level box in normal flow BELOW ITS PRECEDING SIBLINGS: "
              "'boxes are laid out one after the other, vertically, beginning at the top of a containing block. "
              "The vertical distance between two sibling boxes is determined by the margin properties.' So this "
              "box's y needs the used HEIGHT of every preceding in-flow sibling and §8.3.1's COLLAPSING of the "
              "margins between them — and a sibling whose `height` is `auto` is CSS 2 §10.6.3's content-based "
              "height, which core/layout/used_value.c crashes on for the same walk. ONE subproblem answers "
              "both, and it is the in-flow child walk §10.6.3's own crash asks for: 'the distance from the top "
              "content edge to the bottom margin edge of the last in-flow child', which needs no font at all "
              "when the children are block-level and none when there are none. BUILD that walk with §8.3.1's "
              "collapsing, and this box's y is the running offset it already computes. The x is not waiting on "
              "it: §10.1's second case makes the containing block the nearest block container ancestor's "
              "CONTENT EDGE, whose extent used_value.c computes and whose own POSITION is this same recursion");
    if (!fp_length_is(el, "width", "auto")) {
        /* A declared `width` leaves slack in the containing block, and §9.4.1's two touchings then disagree. */
        DFAIL("CSS 2 §9.4.1 says 'each box's left outer edge touches the left edge of the containing block (for "
              "right-to-left formatting, right edges touch)', and this root element declares a `width`, so its "
              "margin box does NOT fill the initial containing block and the two answers differ by the slack. "
              "§10.1 states which applies — 'the direction property of the initial containing block is the same "
              "as for the root element' — and `direction` is not among the properties css_computed_value.c "
              "models, so there is nothing to read it through. ONE ROW is missing and it is the same row CSS 2 "
              "§10.3.3's over-constrained margin case asks for in core/layout/used_value.c: css-writing-modes "
              "gives `direction` the `Computed value: specified keyword` line, so it is a row of "
              "css_computed_models' as-specified arm and a row of css_shorthand_complete_for. RECORD it, and "
              "this crash becomes a branch between `margin-left` and the containing block's width minus the "
              "margin box");
    }
    ml = used_value_px(el, "margin-left");
    mr = used_value_px(el, "margin-right");
    outer = css_px_add(css_px_add(ml, mr), used_value_border_edge_px(el, false));
    if (outer.px > fp_icb_width(el).px)
        DFAIL("CSS 2 §10.3.3's rule 5 solved this root element's `width` from the constraint equation, so its "
              "MARGIN BOX should exactly fill the initial containing block — and it is WIDER than the ICB, "
              "which happens only when css-sizing §5's floor of the content box at zero cut the solve short: "
              "the margins, borders and paddings alone exceed the containing block. The margin box then does "
              "not fill it, §9.4.1's left-touching and right-touching answers differ by the overflow, and the "
              "ICB's computed `direction` is what chooses between them — the same one row the declared-`width` "
              "crash above names in full");
    /* §9.4.1's horizontal rule, with the two formattings agreeing: the margin box fills the containing block,
       so its left outer edge is at the ICB's left edge under either. */
    p.x = ml;
    /* §9.4.1's vertical rule at the top of the containing block, with §8.3.1's root-element exception. */
    p.y = used_value_px(el, "margin-top");
    return p;
}
