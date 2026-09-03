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
 * A BOX ON A LINE IS PLACED BY A DIFFERENT SECTION, and it leaves through that section rather than through the
 * induction above. IT USED TO BE THE ONLY SUCH BOX AND IT IS NOT ANY MORE — CSS 2.1 §17.5 Visual layout of
 * table contents places a table ROW box, described in its own paragraph below — so the SHAPE is the rule and
 * the count was never part of it: a box whose own section positions it returns before the induction, and a
 * reader who takes "the one box" literally will fold the next such section back into §9.4.1's two rules.
 * §9.4's two normal-flow formatting contexts are ALTERNATIVES decided
 * by a box's own LEVEL and by nothing else: §9.4.1's two rules are written about a block-level box, and a box
 * on a line is §9.4.2's — "boxes are laid out horizontally, one after the other, beginning at the top of a
 * containing block". Its origin is its FIRST FRAGMENT's; core/layout/line_box.h computes the fragments against
 * the establishing block container's content box, and this file adds that box's own origin and CSS 2 §8.1's
 * leading border and padding — the same composition §10.1's second case makes for a block-level child.
 * THE LEVEL IS THE WHOLE TEST AND "REPLACED" IS NOT PART OF IT. §9.2.2 puts both kinds of inline-level box on
 * the line: a non-replaced `display: inline` element "generates an inline box" that §9.4.2 SPLITS across as
 * many line boxes as it spans, and an ATOMIC inline-level box — "a single opaque box" — is exactly ONE
 * fragment, whether it is a REPLACED element (HTML §15.4 "Replaced elements", whose computed `display` stays
 * `inline`) or an `inline-block` of either kind, which §9.2.2 names in the same list.
 * line_box.h answers both and this file composes the coordinate out of `*out[0]` either way; asking whether a
 * box is replaced HERE was one component answering a question about the fragment's SHAPE that belongs to the
 * component that delimits it.
 *
 * A TABLE ROW BOX IS PLACED BY CSS 2.1 §17.5 Visual layout of table contents AND NOT BY §9.4.1, and it is the
 * second box that leaves through its own section. CSS 2.1 §17.5's opening sentence is why §9.4.1 cannot be
 * stretched over it — "Internal table elements do not have margins" — so there is no margin edge for
 * §9.4.1's horizontal rule to touch and no margin for its vertical stacking to separate two rows by.
 * §17.5's rule 1 places the row
 * ("Each row box occupies one row of grid cells"), §17.5's own last paragraph puts its edges at the cells'
 * border edges in the separated model, and §17.6.1 The separated borders model's `border-spacing` is the gap
 * between them; the coordinate is that grid row turned into a distance from the TABLE BOX's content edge, and
 * the table box's own origin is an ordinary recursion into this same entry, because CSS 2.1 §17.4 Tables in the
 * visual formatting model puts the table WRAPPER on §9.4.1's stack and gives the table box inside it the
 * initial `margin-*`. The other seven table-internal boxes still crash, each naming what §17.5 leaves it
 * waiting on — and they are not one blocker: a cell has its rectangle and needs the other axis of the same sum
 * with rule 5's `rtl` interchange, a row group needs rule 2's run of grid rows, a column and a column group
 * need a mapping from an ELEMENT to a grid column that exists in neither direction, and a caption is not in the
 * grid at all.
 *
 * WHAT STILL CRASHES, each naming ITS OWN missing piece rather than one shared "there is no layout": a float
 * is §9.5's own positioning, an out-of-flow box is §9.3.2's offsets over a static position, an `inline-flex`
 * or `inline-grid` waits on the USED MAIN SIZE its own module owns (css-flexbox-1 §9.9.1 "Flex Container
 * Intrinsic Main Sizes", css-grid-1 §5.2 "Sizing Grid Containers") and on the baseline that falls out of that
 * module's layout, an `inline-table` waits on NEITHER OF THOSE AND THIS LIST USED TO GROUP IT WITH THEM — its
 * inline size is CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property's and is built
 * (core/layout/table_width.h, routed by core/layout/used_value.c), so what it waits on is the baseline CSS 2.2
 * §10.8.1 "Leading and half-leading" makes its first row's, which is CSS 2.1 §17.5.3 Table height algorithms'
 * and has no component, and a box whose
 * computed `writing-mode` is not `horizontal-tb` waits on
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

/* ONE BORDER AREA in the same space — a position and the two extents that go with it. */
typedef struct {
    CssPx x, y, width, height;
} FlowRect;

/* EVERY BORDER AREA OF A BOX ON A LINE, in content order, in the initial containing block's space. Answers the
 * count and stores a newly allocated array of that many at `*out`, WHICH THE CALLER OWNS AND MUST FREE; the
 * count is never zero, and for a REPLACED element it is exactly ONE (§9.2.2's "single opaque box").
 *
 * IT IS A SECOND ENTRY BECAUSE AN INLINE BOX HAS MORE THAN ONE RECTANGLE AND `flow_border_box_origin` HAS ONE
 * ANSWER. CSS 2 §9.4.2 "Inline formatting contexts": "when an inline box exceeds the width of a line box, it is
 * SPLIT into several boxes and these boxes are distributed across several line boxes" — and CSSOM VIEW §6
 * "Extensions to the Element Interface"'s getClientRects() step 3 returns "one for each box fragment", which is
 * exactly this list, while §7's `offsetLeft` and §6's getBoundingClientRect want the first one's corner. Two
 * members, one derivation: `flow_border_box_origin` answers an inline box out of `*out[0]`, so the corner a
 * page reads and the first rectangle it enumerates cannot disagree.
 *
 * IT ANSWERS THE FULL RECTANGLE AND NOT A POINT, which is the one place this component and core/layout/
 * used_value.h are not separable FOR A NON-REPLACED INLINE BOX. That box's border area is not "an extent at a
 * position": its inline extent is how far its own fragment runs ON THAT LINE, which is a different number per
 * fragment and is not §10.3.1's used `width` (the property "does not apply"), and its block extent is
 * §10.6.1's content area out of the first available font rather than a used `height`. Both come out of
 * core/layout/line_box.h's fragment, so splitting them across two entries would put half of one rectangle
 * behind a component that cannot answer for the other half.
 * A REPLACED ELEMENT'S EXTENTS ARE SEPARABLE AND ARE STILL ANSWERED HERE, which is not a redundancy: §10.3.2
 * and §10.6.2 give it a used width and height, so core/dom/element_view.c composes its ONE rectangle the
 * ordinary way (that extent at this file's origin) and never calls this entry. What this entry hands back for
 * it is the fragment the origin is READ OUT OF, and line_box.c asserts its inline extent against §10.3.2's
 * used border edge — one number derived two ways, which is what makes the two roads checkable against each
 * other rather than merely parallel.
 *
 * `el` MUST BE A BOX ON A LINE — a computed `display` of `inline`, in normal flow, with a `horizontal-tb`
 * writing mode, whether or not HTML §15.4 makes it replaced. Every other box crashes naming its own section,
 * in this file for the positioning scheme and in core/layout/line_box.c for what it contributes to a line. */
size_t flow_inline_fragment_rects(lxb_dom_element_t *el, FlowRect **out);

#endif
