/* CSSOM VIEW §2 "Terminology"'s SCROLLING AREA, for an element. See scrolling_area.h for why the term is a
   component, for the shape §2's four-row table shares, and for why an extent cannot stand in for it. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/dom/element_view.h"
#include "core/layout/block_flow.h"
#include "core/layout/flow_position.h"
#include "core/layout/line_box.h"
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
bool scrolling_area_ending_edge_at_higher_coordinate(lxb_dom_element_t *el, bool vertical)
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

/* §2's "excluding boxes that have an ancestor of the element as their containing block". A box laid out
   against a rectangle that belongs to an ANCESTOR of this element does not move when this element scrolls, so
   it is not in this element's scrolling area however deep in the tree it sits — an absolutely positioned
   descendant whose nearest positioned ancestor is above `el` is the case the sentence is written for. CSS 2.1
   §10.1's four cases are core/layout/used_value.h's and are asked, never re-derived here.
   THIS IS THE ELEMENT COLUMN'S CLAUSE AND THE VIEWPORT COLUMN HAS NONE, which is why the walk below takes an
   exclusion ANCHOR and not a flag: the clause is stated over "an ancestor of THE ELEMENT", a viewport has no
   ancestor, and §2 writes no such sentence in the viewport half of any of its four rows. */
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

/* CSS 2 §8.1's CONTENT BOX ORIGIN on one axis — the placed padding box moved inward by the leading padding.
   It is the rectangle CSS 2.2 §9.4.2 gives a block container's LINE BOXES ("the width of a line box is
   determined by a containing block", and §10.1's second case makes that the CONTENT edge), so it is the origin
   core/layout/line_box.h's span is measured from and the one place the two components have to agree. */
static CssPx sa_content_origin(lxb_dom_element_t *b, bool vertical)
{
    FlowPoint o = flow_padding_box_origin(b);

    return css_px_add(vertical ? o.y : o.x, used_value_px(b, vertical ? "padding-top" : "padding-left"));
}

/* ONE BLOCK CONTAINER'S INLINE FORMATTING CONTEXT folded into the running extreme — the boxes CSS 2.2 §9.2.2.1
   "Anonymous inline boxes" generates for its text runs, which are §2's "descendants' boxes" with no element of
   their own to be placed through.
   IT IS ASKED OF THE ELEMENT THAT ESTABLISHES THE CONTEXT AND NOT OF EACH TEXT NODE, because §9.4.2's
   distribution is a fact about the WHOLE context — "when several inline-level boxes cannot fit horizontally
   within a single line box, they are distributed among two or more vertically-stacked line boxes" — so which
   line a run's fragments land on is a function of every character before them, and a per-node question could
   not be answered at all. core/layout/block_flow.h owns which elements those are.
   BOTH EDGES ARE ASKED FOR AND THE RIGHT ONE IS TAKEN, because the context's own `direction` decides which end
   of its lines the content grows from and `el`'s decides which end of the SCROLLING AREA is the ending one:
   a `rtl` block inside a `ltr` scroller overflows toward the LOWER coordinate, which is `el`'s BEGINNING edge.
   §2's two rows are then one expression rather than a case per combination. */
static CssPx sa_inline_context_extreme(lxb_dom_element_t *b, bool vertical, bool ending_at_hi, CssPx best)
{
    CssPx origin, lo, hi;

    if (!block_flow_establishes_inline_context(b)) return best;
    origin = sa_content_origin(b, vertical);
    line_box_content_span(b, lxb_dom_interface_node(b)->first_child, NULL, vertical, &lo, &hi);
    DCHECK(css_px_sub(hi, lo).px >= 0.0,
           "CSS 2.2 §9.4.2's line boxes reported a span whose ENDING edge is before its BEGINNING edge. The "
           "two are a maximum and a minimum over the same set of boxes seeded at the same corner, so an "
           "inverted pair is the two edges having been derived from different lines");
    if (ending_at_hi) return css_px_max(best, css_px_add(origin, hi));
    return css_px_min(best, css_px_add(origin, lo));
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
   matters, because `scrollWidth > clientWidth` is precisely what a page asks this to decide.
   THOSE BOXES ARE FOLDED PER FORMATTING CONTEXT AND NOT PER TEXT NODE, which CSS 2.2 §9.4.2 forces: its
   distribution is stated over the whole context, so which line box a run's fragments land on is a function of
   every character before them and a per-node question has no answer at all. The walk therefore asks
   core/layout/line_box.h once at each element that ESTABLISHES a context — `el` included, since the anonymous
   inline boxes in `el`'s own context hold `el`'s descendant TEXT NODES and are their boxes — and then asks
   §9.2.2.1's rule about each text node only to check that some context has already covered it.
   THIS IS NOT THE HEIGHT WALK ASKED TWICE — that walk never sees the contents of a box with a declared height
   (core/layout/block_flow.c's `bf_height_needs_content`), and a text run overflowing a declared-height box is
   exactly the case this member exists for.
   THE WALK ROOT AND THE EXCLUSION ANCHOR ARE TWO PARAMETERS BECAUSE §2's TWO COLUMNS SPLIT THEM. For an element
   they are the same element: the subtree walked is its own and the excluded boxes are those whose containing
   block is above it. For a VIEWPORT the subtree walked is the DOCUMENT ELEMENT's — a viewport's descendants are
   every box in the document — and `exclude_under` is NULL, because §2's viewport column carries no exclusion
   clause at all. A NULL anchor is the spec's own absence and not a mode switch: there is no ancestor of a
   viewport for a containing block to be, so there is nothing for the test to be asked about. */
static CssPx sa_descendants_extreme(lxb_dom_element_t *el, lxb_dom_element_t *exclude_under,
                                    bool vertical, bool ending_at_hi, CssPx seed)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(el), *n = root->first_child;
    /* `el`'s OWN formatting context first. `el` is not one of its own descendants and this is not a special
       case for it: the boxes folded here are the anonymous inline boxes around `el`'s child TEXT NODES, which
       are descendants, and the element establishing the context is simply where §9.4.2 can be asked about
       them. `el` has a box — the caller's own precondition — so there is no second existence test. */
    CssPx best = sa_inline_context_extreme(el, vertical, ending_at_hi, seed);

    while (n != NULL) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *d = lxb_dom_interface_element(n);

            if (element_view_has_box(n)) {
                /* THE EXCLUSION IS PER BOX AND IS NOT INHERITED BY THE SUBTREE, which is §2's own sentence:
                   it excludes "boxes THAT HAVE an ancestor of the element as their containing block", and the
                   containing block of a box inside an excluded one is that excluded box — not an ancestor of
                   `el`. So a descendant of an out-of-flow box is IN the scrolling area while the box itself is
                   out of it, and the inline formatting context an excluded box establishes is folded for the
                   same reason: the anonymous inline boxes on its lines have IT as their containing block. */
                if (exclude_under == NULL || !sa_excluded(exclude_under, d)) {
                    CssPx e = sa_descendant_edge(d, vertical, ending_at_hi);

                    best = ending_at_hi ? css_px_max(best, e) : css_px_min(best, e);
                }
                best = sa_inline_context_extreme(d, vertical, ending_at_hi, best);
            }
        } else if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_element_t *parent;

            DCHECK(n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
                   "a TEXT node inside an element's subtree has no element parent — the walk started at an "
                   "element and descends only through its children, so a text node here belongs to one");
            parent = lxb_dom_interface_element(n->parent);
            /* §9.2.2.1's rule answers whether the run is a box at all; collapsible white space is not and
               contributes nothing. A run that IS a box has already been folded above IF its parent is the
               element establishing the context it is on — which is every case except the one below. */
            if (block_flow_text_child_generates_box(parent, n) &&
                !block_flow_establishes_inline_context(parent))
                DFAIL("CSSOM VIEW §2's scrolling area takes the extreme over \"all of the element's "
                      "descendants' boxes\", and one of them is the ANONYMOUS INLINE BOX (CSS 2.2 §9.2.2.1) "
                      "holding this text run — but the element this run is a child of does NOT establish the "
                      "inline formatting context the run is flowed into, so there is no element to ask "
                      "core/layout/line_box.h about and the run has not been folded in. THE MEASUREMENT AND "
                      "THE PLACEMENT ARE BOTH BUILT and neither is what is missing: `line_box_content_span` "
                      "answers where a context's boxes reach on either axis, and this walk folds it at every "
                      "element that establishes one. WHAT IS MISSING IS THE BOX THAT ESTABLISHES *THIS* "
                      "CONTEXT, and CSS 2.2 §9.2.1.1 \"Anonymous block boxes\" says why it has no element: "
                      "\"if a block container box … has a block-level box inside it …, then we force it to "
                      "have only block-level boxes inside it\", each maximal run of inline-level children "
                      "wrapped in an ANONYMOUS BLOCK BOX. This run is inside one of those, so its context's "
                      "line boxes are `[first, end)` of the parent's children rather than the whole child "
                      "list — the run `block_flow.c`'s `bf_anon_run_end` delimits and hands to "
                      "`line_box_content_height` already — and its content box's origin is the anonymous "
                      "box's, which is a distance down §9.4.1's stack that the same walk computes and keeps "
                      "to itself. BUILD an entry beside `block_flow_establishes_inline_context` that reports "
                      "the anonymous block boxes of one container — each run's `[first, end)` and the offset "
                      "of its content box from the container's own — and fold each of them here exactly as "
                      "the whole-child-list case is folded. THE OTHER SHAPE THAT REACHES THIS LINE is a run "
                      "whose parent is an INLINE box (`<div>a<span>b</span></div>`), and that one is already "
                      "named one crash earlier: the `span`'s own box is placed by §9.4.2 and "
                      "core/layout/flow_position.c aborts for it before this walk descends into its text");
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
    /* THE ELEMENT'S OWN PADDING BOX FIRST, because its two edges are §2's table's own BEGINNING edges and
       because the placement inside it is where a box type this engine cannot lay out crashes by its own name.
       core/layout/flow_position.h owns the border-to-padding step, so the read of `border-top-width` that used
       to sit in this file is gone rather than duplicated — css-overflow-3 §2.3 "Scrolling Overflow"'s scrollport
       is the same rectangle and would have been the second copy. */
    origin = flow_padding_box_origin(el);
    ending_at_hi = scrolling_area_ending_edge_at_higher_coordinate(el, vertical);
    padding = used_value_padding_edge_px(el, vertical);
    /* §2's two BEGINNING edges for an element are "the element's top padding edge" and "the element's left
       padding edge", and its two ENDING edges are the extreme over that same padding edge and the descendants'
       margin edges — so the element's own padding box is in the extreme rather than beside it, which is what
       makes a scrolling area at least as large as the padding box for every element in every document. */
    lo = vertical ? origin.y : origin.x;
    hi = css_px_add(lo, padding);
    if (ending_at_hi) hi = sa_descendants_extreme(el, el, vertical, true, hi);
    else              lo = sa_descendants_extreme(el, el, vertical, false, lo);
    DCHECK(css_px_sub(hi, lo).px >= padding.px,
           "an element's scrolling area came out SMALLER than its own padding box. §2's table takes the ending "
           "edge as an extreme OVER that padding edge, so the padding box is one of the operands of the maximum "
           "and the result cannot be below it — a smaller answer is the beginning edge and the ending edge "
           "having been derived from different boxes");
    return css_px_sub(hi, lo);
}

/* css-writing-modes-4 §8 "The Principal Writing Mode"' ELEMENT — the one whose used `writing-mode` and
 * `direction` the VIEWPORT's overflow directions are those of. §8: "The principal writing mode of the document
 * is determined by the used writing-mode, direction, and text-orientation values of the root element. This
 * writing mode is used, for example, to determine the direction of scrolling"; §8.1 "Propagation to the Initial
 * Containing Block": "The principal writing mode is propagated to the initial containing block and to the
 * viewport, thereby affecting … the scrolling direction of the viewport."
 * AND ITS HTML SPECIAL CASE IS THE WHOLE REASON THIS IS A FUNCTION RATHER THAN A READ OF THE ROOT. §8: "As a
 * special case for handling HTML documents, if the root element has a body child element whose display value is
 * not none, the used value of the of writing-mode and direction properties on root element are taken from the
 * computed writing-mode and direction of the first such child element instead of from the root element's own
 * values." So `<html><body dir=rtl>` gives the VIEWPORT a leftward inline-end overflow direction while the root
 * element's own computed `direction` is still `ltr` — the propagation is on USED values and §8 says in so many
 * words that it "does not affect the computed values … of the root element itself". Reading the root's computed
 * value would answer the wrong direction for the single most common way an author writes a right-to-left page.
 * IT LIVES IN THIS FILE BECAUSE §2's VIEWPORT ROW IS ONE OF ITS TWO READERS. It used to be a static in
 * core/dom/element_scrolling.c, whose comment gave as the reason that §2's overflow directions were all anyone
 * asked of it "so it is answered here rather than in core/frame/viewport.h, whose §2 column is the viewport's
 * SCROLLING AREA — a rectangle this file does not touch and which that component derives as the initial
 * containing block on both axes, WITH NO DIRECTION IN IT". That last clause was the argument, and building §2's
 * viewport row below retires it: the scrolling area now has a direction in it, so the fact has two readers and
 * belongs beside the table that is the reason both of them ask. */
static lxb_dom_element_t *sa_principal_writing_mode_element(lxb_dom_node_t *doc)
{
    lxb_dom_node_t *root = document_document_element_of(doc);
    lxb_dom_node_t *body = document_body_of(doc);

    DCHECK(root != NULL && root->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "css-writing-modes-4 §8's principal writing mode was asked of a document with no ROOT ELEMENT. §8 "
           "determines it from the root element's used values, and both readers reach here only for a document "
           "that HAS a viewport — CSSOM VIEW §2's viewport scrolling-area row below and CSSOM VIEW §6.1's "
           "ancestor walk — while a viewport exists only for a document a navigable is presenting, and such a "
           "document has a root element");
    /* "…a body child element whose display value is not none". `document_body_of` is HTML §3.1.7's body
       element, which is already the FIRST `body` or `frameset` child of the root; the display half is
       core/dom/element_view.h's one predicate, which reads the computed `display` of the element and of its
       ancestors and is the same question §8's clause asks. */
    if (body != NULL && element_view_has_box(body)) return lxb_dom_interface_element(body);
    return lxb_dom_interface_element(root);
}

bool scrolling_area_viewport_ending_edge_at_higher_coordinate(lxb_dom_node_t *doc, bool vertical)
{
    DCHECK(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "CSSOM VIEW §2's overflow directions for a VIEWPORT were asked of a node that is not a Document. The "
           "directions are css-writing-modes-4 §8's principal writing mode, which §8 states over THE DOCUMENT's "
           "root element, so there is no other node this question has an answer for");
    /* §2 gives "a scrolling box of a viewport or element" ONE definition of its two overflow directions, so
       once §8 has named the element whose used values the viewport's are, this is the element form and not a
       second derivation of the same bit. */
    return scrolling_area_ending_edge_at_higher_coordinate(sa_principal_writing_mode_element(doc), vertical);
}

CssPx scrolling_area_viewport_extent_px(lxb_dom_node_t *doc, CssPx icb_extent, bool vertical)
{
    lxb_dom_node_t *rootn;
    bool ending_at_hi;
    CssPx lo, hi;

    DCHECK(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "CSSOM VIEW §2's scrolling area of a VIEWPORT was asked of a node that is not a Document — the row "
           "is stated over the initial containing block and \"all of the viewport's descendants' boxes\", and "
           "both of those are facts about the document a viewport is presenting");
    DCHECK(icb_extent.px >= 0.0,
           "CSS 2.2 §10.1 \"Definition of 'containing block'\" gives the initial containing block \"the "
           "dimensions of the viewport\", and a viewport has no negative dimension — so this is the ICB extent "
           "and its viewport having come apart, not a layout result");
    rootn = document_document_element_of(doc);
    DCHECK(rootn != NULL,
           "CSSOM VIEW §2's scrolling area of a VIEWPORT was asked of a document with no DOCUMENT ELEMENT. The "
           "extreme over \"all of the viewport's descendants' boxes\" is then over the EMPTY SET and the area "
           "is the initial containing block alone — a derivation the caller makes for itself, because it is "
           "the caller that knows a realm is presenting no document at all");
    ending_at_hi = scrolling_area_viewport_ending_edge_at_higher_coordinate(doc, vertical);
    /* THE INITIAL CONTAINING BLOCK'S TWO EDGES ON THIS AXIS, which are the coordinates 0 and `icb_extent` and
       are DERIVED rather than assumed: CSS 2.2 §10.1 anchors the ICB "at the canvas origin", and
       core/layout/flow_position.h places every box in that same space ("THE COORDINATE SPACE IS THE INITIAL
       CONTAINING BLOCK'S"), which is what lets a descendant's margin edge and the ICB's own edge be operands of
       one extreme at all. §2's viewport row takes ONE of them as the beginning edge outright — "the top edge of
       the initial containing block", "the left edge of the initial containing block" — and folds the OTHER into
       the extreme on the ending side, so both appear here and neither is beside the walk. */
    lo = css_px(0.0);
    hi = icb_extent;
    /* "ALL OF THE VIEWPORT'S DESCENDANTS' BOXES" — every box in the document, so the walk is the document
       element's subtree with the DOCUMENT ELEMENT'S OWN margin edge folded in beside it. The root is one of the
       viewport's descendants and it is the one box a walk over its own subtree cannot contribute for itself;
       for an element the same box is the caller, which is why the element row has no such term.
       A ROOT WITH NO BOX contributes nothing and neither does anything under it — core/dom/element_view.h's one
       predicate is the same one the walk applies per descendant — so the extreme is over the empty set and the
       area is the ICB. That is §2's answer for a `display: none` root, not a shrug at one. */
    if (element_view_has_box(rootn)) {
        lxb_dom_element_t *root = lxb_dom_interface_element(rootn);
        CssPx own = sa_descendant_edge(root, vertical, ending_at_hi);

        if (ending_at_hi) hi = sa_descendants_extreme(root, NULL, vertical, true,  css_px_max(hi, own));
        else              lo = sa_descendants_extreme(root, NULL, vertical, false, css_px_min(lo, own));
    }
    DCHECK(css_px_sub(hi, lo).px >= icb_extent.px,
           "a VIEWPORT's scrolling area came out SMALLER than its initial containing block. §2's viewport row "
           "takes the ending edge as an extreme OVER the ICB's own edge on that side — \"the bottom-most edge "
           "of the bottom edge of the initial containing block and the bottom margin edge of all of the "
           "viewport's descendants' boxes\" — so the ICB extent is one of the operands of the extreme and the "
           "result cannot be below it. This is the invariant CSSOM VIEW §6's `scrollWidth`/`scrollHeight` "
           "max(area, viewport) and §4's `scroll()` clamp both rest on, and a smaller answer is the beginning "
           "edge and the ending edge having been derived from different boxes");
    return css_px_sub(hi, lo);
}
