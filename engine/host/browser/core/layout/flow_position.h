/* CSS 2 §9.4.1 "Block formatting contexts" — WHERE A BOX IS, which is the other half of a box's geometry and
 * the half core/layout/used_value.h does not have.
 *
 * AN EXTENT IS NOT A POSITION AND THIS COMPONENT EXISTS TO KEEP THEM APART. used_value.h computes a distance
 * between two parallel edges of ONE box: §10.3.3's constraint equation over §10.1's containing block, and
 * §8.1's padding and border edges stated over it. Nothing in that chain says where the box SITS. A rectangle
 * needs both, so CSSOM VIEW §6's `getClientRects()` and §7's `offsetTop`/`offsetLeft` reach for this component
 * and not for that one, and an extent standing in for a coordinate is the one way a rectangle here could be
 * wrong while every number in it is real.
 *
 * THE COORDINATE SPACE IS THE INITIAL CONTAINING BLOCK'S, and it is the same one CSSOM VIEW measures in. §10.1
 * makes the ICB "a rectangle … anchored at the canvas origin", CSSOM VIEW §7 states `offsetTop` "relative to
 * the initial containing block origin", and §4's `scrollX` is "the x-coordinate, RELATIVE TO THE INITIAL
 * CONTAINING BLOCK ORIGIN, of the left of the viewport" — so the viewport moves inside this space rather than
 * the space moving with it. A CLIENT coordinate is this one minus the viewport's scroll position (§10's
 * `clientX` is "relative to the origin of the viewport"), and that subtraction belongs to the member that
 * reports a client rectangle, not here: this component answers in the space every CSSOM VIEW coordinate is
 * defined in and each caller states its own frame.
 *
 * WHAT IS ANSWERED, AND WHY EXACTLY THAT. §9.4.1's two sentences are the whole placement rule for a
 * block-level box: "boxes are laid out one after the other, vertically, beginning at the top of a containing
 * block. The vertical distance between two sibling boxes is determined by the margin properties", and "each
 * box's left outer edge touches the left edge of the containing block (for right-to-left formatting, right
 * edges touch)". For the ROOT ELEMENT both reduce to its own two margins: its containing block is §10.1's
 * first case, the ICB, whose origin IS the canvas origin; there is no preceding sibling for the vertical rule
 * to measure from; and §8.3.1 "Collapsing margins" states outright that "margins of the root element's box do
 * not collapse", so its top margin is its own used value and not a collapsed one. That is a derivation with no
 * term left over, and it is the base case every other box's position is stated against.
 *
 * EVERY OTHER IN-FLOW BLOCK-LEVEL BOX IS THAT BASE CASE PLUS §10.1's SECOND, AND THE INDUCTION IS THE WHOLE
 * COMPONENT. The containing block is "the CONTENT EDGE of the nearest block container ancestor box", so the
 * box's origin is that ancestor's origin — this same function, one level up — plus CSS 2 §8.1's leading border
 * and padding, plus what §9.4.1 puts between that content edge and this box. The horizontal term is the same
 * left-outer-edge touching the root arm states. The VERTICAL one is core/layout/block_flow.h: the used height
 * of every preceding in-flow sibling with §8.3.1's collapsing between them, which is the walk §10.6.3's
 * content-based height was blocked on as well — one subproblem, and building it built both.
 *
 * WHICH OF §9.4.1's TWO HORIZONTAL TOUCHINGS APPLIES is the CONTAINING BLOCK's computed `direction`, which
 * core/layout/used_value.h answers with §10.1's own first-case exception folded in. The two are not a slack
 * apart: they are the same distance measured from opposite edges, so an `rtl` box's border box begins at the
 * containing block's width less its own `margin-right` and less its border box's width, and an over-constrained
 * box differs between them exactly where §10.3.3 says it does.
 *
 * WHAT STILL CRASHES, each naming ITS OWN missing piece rather than one shared "there is no layout": a float
 * is §9.5's own positioning, an out-of-flow box is §9.3.2's offsets over a static position, an inline box is
 * §9.4.2's line boxes, and a box whose computed `writing-mode` is not `horizontal-tb` waits on
 * css-writing-modes-4 §7.4's flow-relative restatement of the two rules this file implements physically.
 *
 * THE ANSWER IS A `CssPx` PAIR FOR used_value.h's REASON. §10.1's base case is the viewport, so a coordinate
 * derived from it carries the ICB's environment fact and `rect.left < 768` is the same responsive gate
 * `innerWidth < 768` is. The example is the number this component's arithmetic runs on; the fact rides it, and
 * core/frame/viewport.h's one seam is where either crosses to a page. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_POSITION_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOW_POSITION_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* A POINT IN THE INITIAL CONTAINING BLOCK'S COORDINATE SPACE — see the header above for the space and for why
   each coordinate is a `CssPx` and not a `double`. */
typedef struct {
    CssPx x;
    CssPx y;
} FlowPoint;

/* THE TOP-LEFT CORNER OF `el`'s BORDER BOX, in that space. The caller has already established that the element
   HAS a box (core/dom/element_view.h's one predicate) — an element that generates none has no position at all
   and the caller's own step says so before reaching here. Every box CSS 2 §9.4 places and this component does
   not crashes naming its own section; there is no fallback coordinate. */
FlowPoint flow_border_box_origin(lxb_dom_element_t *el);

/* THE TOP-LEFT CORNER OF `el`'s PADDING BOX, in that same space — the border box origin moved inward by CSS 2
   §8.1 "Box dimensions"' leading BORDER on each axis, and nothing else.
   IT IS A SECOND ENTRY AND NOT A SECOND ANSWER: it is derived from `flow_border_box_origin`, so every box this
   component cannot place crashes there naming its own section before a padding edge is asked for. TWO
   ALGORITHMS NEED THIS EXACT RECTANGLE and each would otherwise carry its own copy of the border read: CSSOM
   VIEW §2 "Terminology" states an element's scrolling area over "the element's top padding edge" and "the
   element's left padding edge" (core/layout/scrolling_area.h), and css-overflow-3 §2.3 "Scrolling Overflow"
   makes a scroll container's SCROLLPORT — the visual viewport CSSOM VIEW §6.1's determine the scroll-into-view
   position aligns against — "coincide with its padding box". */
FlowPoint flow_padding_box_origin(lxb_dom_element_t *el);

#endif
