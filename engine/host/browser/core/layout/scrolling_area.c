/* CSSOM VIEW §2 "Terminology"'s SCROLLING AREA, for an element. See scrolling_area.h for why the term is a
   component, for the shape §2's four-row table shares, and for why an extent cannot stand in for it. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/element_view.h"
#include "core/layout/block_flow.h"
#include "core/layout/flow_position.h"
#include "core/layout/scrolling_area.h"
#include "core/layout/used_value.h"

static bool sa_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* §2's OVERFLOW DIRECTIONS, reduced to the one bit each axis needs: does the ENDING edge of the scrolling area
   sit at the larger coordinate on this axis. "A scrolling box of a viewport or element has two overflow
   directions, which are the block-end and inline-end directions for that viewport or element", and
   css-writing-modes-4 §6.2 "Flow-relative Directions" derives those: block-end is "the side opposite
   block-start", which in `horizontal-tb` mode is the physical bottom, and inline-end is "the side opposite
   start" — the line-right side for a used `direction` of `ltr` and the line-left side for `rtl`.
   ONE PROPERTY DECIDES EACH AXIS HERE, and that is a fact about `horizontal-tb` rather than a general rule:
   the block axis is the vertical one and the inline axis the horizontal one only in a horizontal writing mode,
   which is why the assertion below is what lets this function take the axis rather than the abstract
   dimension. §6.2 states the split in so many words — "while determining the block-start and block-end sides
   of a box depends only on the writing-mode property, determining the inline-start and inline-end sides of a
   box depends not only on the writing-mode property but also the direction property". */
static bool sa_ending_edge_at_higher_coordinate(lxb_dom_element_t *el, bool vertical)
{
    bool horizontal_tb = sa_computed_is(el, "writing-mode", "horizontal-tb");

    DCHECK(horizontal_tb,
           "CSSOM VIEW §2's overflow directions were mapped onto the physical axes for a box whose computed "
           "`writing-mode` is not `horizontal-tb`, where the block axis is not the vertical one. The element's "
           "own box was placed by core/layout/flow_position.c, which crashes for exactly that value before any "
           "coordinate exists to measure — so this element has a position and a writing mode that disagree, and "
           "the two tests have come apart");
    if (vertical) return true;   /* block-end is downward in every horizontal writing mode */
    /* inline-end is the line-right side for a used `direction` of `ltr` and the line-left side for `rtl`, and
       the property admits no third value — asserted here rather than read as "not rtl", so a spelling the
       cascade should have refused crashes instead of quietly meaning `ltr`. */
    DCHECK(sa_computed_is(el, "direction", "ltr") || sa_computed_is(el, "direction", "rtl"),
           "a computed `direction` is neither `ltr` nor `rtl`. css-writing-modes-4 §2.1 \"Specifying "
           "Directionality: the direction property\" gives the property the `Value:` line `ltr | rtl` and "
           "nothing else");
    return !sa_computed_is(el, "direction", "rtl");
}

/* CSS 2 §8.1 "Box dimensions"'s LEADING BORDER on one axis — the one edge between a box's border box, which is
   what core/layout/flow_position.h places, and its PADDING box, which is what §2's table is stated over. */
static CssPx sa_border_before(lxb_dom_element_t *el, bool vertical)
{
    CssLength b = css_computed_length(el, vertical ? "border-top-width" : "border-left-width");

    DCHECK(b.kind == CSS_LENGTH_ABSOLUTE,
           "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 §3.3's "
           "`Computed value:` line is `absolute length, snapped as a border width`, so every arm of that "
           "derivation produces one and a percentage or a keyword here is a rule that did not run");
    return b.px;
}

/* §2's "excluding boxes that have an ancestor of the element as their containing block". A box laid out
   against a rectangle that belongs to an ANCESTOR of this element does not move when this element scrolls, so
   it is not in this element's scrolling area however deep in the tree it sits — an absolutely positioned
   descendant whose nearest positioned ancestor is above `el` is the case the sentence is written for. CSS 2.1
   §10.1's four cases are core/layout/used_value.h's and are asked, never re-derived here. */
static bool sa_excluded(lxb_dom_element_t *el, lxb_dom_element_t *descendant)
{
    lxb_dom_element_t *cb = used_value_containing_block(descendant);
    const lxb_dom_node_t *cbn, *a;

    DCHECK(cb != NULL,
           "§10.1's FIRST case — the initial containing block — was answered for a strict DESCENDANT of an "
           "element, and that case is the ROOT ELEMENT's alone. A descendant of any element has a root element "
           "above it, so this is the root test and the tree's own shape having come apart");
    cbn = lxb_dom_interface_node(cb);
    /* An ANCESTOR of the element, which is strictly above it: a box whose containing block is the ELEMENT
       ITSELF is in the scrolling area, so the walk starts at the element's parent. */
    for (a = lxb_dom_interface_node(el)->parent; a != NULL; a = a->parent)
        if (a == cbn) return true;
    return false;
}

/* ONE DESCENDANT'S CONTRIBUTION — the margin edge §2's table names on the ENDING side of this axis. A margin
   edge is CSS 2 §8.1's outermost edge, so it is the placed BORDER box's origin moved outward by the margin on
   that side. Where the ENDING edge is at the higher coordinate that is the far border edge plus the
   trailing margin, and where it is at the lower one it is the origin less the leading margin. */
static CssPx sa_descendant_edge(lxb_dom_element_t *d, bool vertical, bool ending_at_hi)
{
    FlowPoint o = flow_border_box_origin(d);
    CssPx origin = vertical ? o.y : o.x;

    if (!ending_at_hi)
        return css_px_sub(origin, used_value_px(d, vertical ? "margin-top" : "margin-left"));
    return css_px_add(css_px_add(origin, used_value_border_edge_px(d, vertical)),
                      used_value_px(d, vertical ? "margin-bottom" : "margin-right"));
}

/* THE EXTREME OVER EVERY DESCENDANT'S BOX, in tree order. The walk descends through an element that generates
   NO box of its own, deliberately: css-display §3.1's `display: contents` gives such an element no box while
   "its children and pseudo-elements still generate boxes and text runs as normal", and those children are
   descendants of `el` exactly as any other box is. core/dom/element_view.h's one predicate is what decides
   which nodes have a box, so a `display: none` subtree contributes nothing without this walk carrying a second
   copy of that rule.
   A TEXT RUN IS A DESCENDANT BOX TOO, and it is the one this walk could silently miss. §2 says "all of the
   element's descendants' boxes" and an anonymous inline box holding a text run is one of them, so a
   `<div style="width:50px">verylongword</div>` has a scrolling area WIDER than its padding box in every user
   agent — and a walk over ELEMENTS alone would report the padding box and be wrong in the one direction that
   matters, because `scrollWidth > clientWidth` is precisely what a page asks this to decide. The walk
   therefore asks core/layout/block_flow.h's §9.2.2.1 rule about every text node it passes: collapsible white
   space contributes nothing, and a real run CRASHES BELOW naming the inline-axis measurement it would need.
   THIS IS NOT THE HEIGHT WALK ASKED TWICE — that walk never sees the contents of a box with a declared height
   (core/layout/block_flow.c's `bf_height_needs_content`), and a text run overflowing a declared-height box is
   exactly the case this member exists for. */
static CssPx sa_descendants_extreme(lxb_dom_element_t *el, bool vertical, bool ending_at_hi, CssPx seed)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(el), *n = root->first_child;
    CssPx best = seed;

    while (n != NULL) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *d = lxb_dom_interface_element(n);

            if (element_view_has_box(n) && !sa_excluded(el, d)) {
                CssPx e = sa_descendant_edge(d, vertical, ending_at_hi);

                best = ending_at_hi ? css_px_max(best, e) : css_px_min(best, e);
            }
        } else if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
            DCHECK(n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
                   "a TEXT node inside an element's subtree has no element parent — the walk started at an "
                   "element and descends only through its children, so a text node here belongs to one");
            /* §9.2.2.1's rule answers whether the run is a box at all; collapsible white space is not and
               contributes nothing. A run that IS a box needs its anonymous inline box's MARGIN EDGE folded
               into `best`, and that edge is a position ALONG a line box — which is the crash below, stated
               here rather than borrowed from the predicate, because the predicate answers a classification
               and this member needs a coordinate. */
            if (block_flow_text_child_generates_box(lxb_dom_interface_element(n->parent), n))
                DFAIL("CSSOM VIEW §2's scrolling area takes the extreme over \"all of the element's "
                      "descendants' boxes\", and one of them is the ANONYMOUS INLINE BOX (CSS 2.2 §9.2.2.1) "
                      "holding this text run — so this walk needs the run's own INLINE-AXIS EXTENT, which is "
                      "the sum of its glyphs' advances, and its position on the line box §9.4.2 flowed it "
                      "into. THE SUM IS BUILT and so is the break search over it — core/layout/text_run.h "
                      "reports a run's whole advance and its widest unbreakable segment, over the per-glyph "
                      "advances core/css/font_metrics.h measures — so a zero here is no longer the only "
                      "available answer, and reporting one would still say that "
                      "`<div style=\"width:50px\">verylongword</div>` has no overflow at all, which is the "
                      "exact comparison `scrollWidth > clientWidth` is asked to decide. WHAT IS MISSING IS THE "
                      "PLACEMENT and not the measurement: this member needs the fragment's own MARGIN EDGE — a "
                      "coordinate — which is a function of which line box §9.4.2 flowed each fragment onto and "
                      "of where along that line it starts, and neither is a size. THE FIRST HALF IS BUILT: "
                      "`text_run_measure_fill` (core/layout/text_run.h) distributes the run across §9.4.2's "
                      "line boxes and reports the items on each, so WHICH line a fragment is on is answered. "
                      "The second half is a per-item running POSITION along the line, which the fill does not "
                      "produce and which §9.4.2 hands to `text-align` — the same two numbers "
                      "core/layout/flow_position.c crashes for when it asks where an INLINE BOX starts. BUILD "
                      "the per-item position beside the fill, then fold each fragment's margin edge into "
                      "`best` here");
        }
        if (n->first_child != NULL) { n = n->first_child; continue; }
        while (n != root && n->next == NULL) n = n->parent;
        if (n == root) break;
        n = n->next;
    }
    return best;
}

CssPx scrolling_area_extent_px(lxb_dom_element_t *el, bool vertical)
{
    CssPx padding, lo, hi;
    bool ending_at_hi;
    FlowPoint origin;

    DCHECK(el != NULL, "a scrolling area was asked for with no element");
    /* THE ELEMENT'S OWN BOX FIRST, because its position is the operand every other term is measured against
       and because the placement is where a box type this engine cannot lay out crashes by its own name. */
    origin = flow_border_box_origin(el);
    ending_at_hi = sa_ending_edge_at_higher_coordinate(el, vertical);
    padding = used_value_padding_edge_px(el, vertical);
    /* §2's two BEGINNING edges for an element are "the element's top padding edge" and "the element's left
       padding edge", and its two ENDING edges are the extreme over that same padding edge and the descendants'
       margin edges — so the element's own padding box is in the extreme rather than beside it, which is what
       makes a scrolling area at least as large as the padding box for every element in every document. */
    lo = css_px_add(vertical ? origin.y : origin.x, sa_border_before(el, vertical));
    hi = css_px_add(lo, padding);
    if (ending_at_hi) hi = sa_descendants_extreme(el, vertical, true, hi);
    else              lo = sa_descendants_extreme(el, vertical, false, lo);
    DCHECK(css_px_sub(hi, lo).px >= padding.px,
           "an element's scrolling area came out SMALLER than its own padding box. §2's table takes the ending "
           "edge as an extreme OVER that padding edge, so the padding box is one of the operands of the maximum "
           "and the result cannot be below it — a smaller answer is the beginning edge and the ending edge "
           "having been derived from different boxes");
    return css_px_sub(hi, lo);
}
