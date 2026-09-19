/* CSSOM VIEW §2 "Terminology"'s SCROLLING BOX — the question "does this element establish one", which §6 and
 * §6.1 ask four times and which CSSOM VIEW never defines.
 *
 * IT IS AN UNDEFINED TERM IN ITS OWN SPEC, AND THAT IS WHY IT IS A COMPONENT. §2 uses "scrolling box" as a
 * primitive: it gives one "two overflow directions", it states the scrolling area's four edges "depending on
 * the viewport's or element's scrolling box's overflow directions", §6's `scrollTop`/`scrollLeft` setter and
 * §6's `scroll()`/`scrollTo()`/`scrollBy()` terminate when "the element has no associated scrolling box", and
 * §6.1's scroll a target into view walks "each ancestor element or viewport that establishes a scrolling box".
 * Nowhere does it say WHICH elements have one. The definition it is reaching for is css-overflow-3 §3.1
 * "Managing Overflow: the overflow-x, overflow-y, and overflow properties"' SCROLL CONTAINER, stated there in
 * one sentence: "The scroll, auto, and hidden values are known as the scrollable values of overflow. They cause
 * the box to be a scroll container and the affected axis to be a scrollable axis", and its complement — "If
 * neither axis computes to a scrollable value, the box is not a scroll container."
 * So the term is one fact with two names, and this component is where the name is joined to the definition
 * ONCE. A `strcmp` against `"auto"` written at each of the four sites would be four readings of a rule none of
 * them cites, free to disagree about `hidden` (which IS a scrollable value and does establish a scrolling box,
 * programmatically scrollable with no scrollbar) and about `clip` (which is NOT, and is the value css-overflow
 * added precisely so an author could clip without one).
 *
 * §2'S OWN NOTE IS THE ONE PLACE THE TWO TERMS COME APART, AND ITS LATITUDE IS UNOBSERVABLE HERE. The note
 * reads: "A body element that is potentially scrollable might not have a scrolling box. For instance, it could
 * have a used value of overflow being auto but not have its content overflowing its content area." That is a
 * MIGHT — permission for a user agent to say a non-overflowing `auto` box has no scrolling box — and taking it
 * would change no answer this engine can produce, which is why the definition above is taken literally instead
 * of being hedged. Every consumer of a scrolling box either asks a question that has the element's OVERFLOW as
 * a separate disjunct beside it (§6's step 10 terminates on "has no overflow" whether or not the box exists) or
 * computes a scroll POSITION inside it (§6.1), and a scroll container whose content does not overflow has a
 * scrolling area exactly equal to its padding box — core/layout/scrolling_area.h's own §2 derivation — so its
 * only valid scroll position is its origin, which is the position it already has. Both readings therefore
 * perform no scroll and settle the same promise. Stating that is a derivation; picking the note's arm "to be
 * safe" would be a second rule nothing could distinguish from this one.
 *
 * THE `Applies to:` LINE IS PART OF THE RULE AND IS ASKED, NOT ASSUMED. §3.1's line is "block containers, flex
 * containers, grid containers, and table grid boxes", so `overflow: auto` on a `display: inline` element does
 * not make it a scroll container at all — the declaration is there, the cascade computed it, and the property
 * does not apply. core/css/css_property_applies.h owns every such line and is asked here rather than a second
 * copy of this one being written beside the overflow read.
 *
 * AND ONE ELEMENT PER DOCUMENT IS EXEMPT BY A USED VALUE RATHER THAN A COMPUTED ONE. css-overflow-3 §3.1.4
 * "Overflow Viewport Propagation" gives the ROOT ELEMENT's overflow to the VIEWPORT (or, for an `html` root
 * whose overflow is `visible` in both axes, the BODY's), and ends: "the element from which the value is
 * propagated must then have a used overflow value of visible." So `<html style="overflow:auto">` is NOT a
 * scroll container — the viewport is — and a component that read only the computed value would report two
 * scrolling boxes where the document has one, which CSSOM VIEW §6.1's ancestor walk would then align the same
 * target against twice.
 *
 * AND A BOX IS THE PRECONDITION. A scroll container is a BOX, so an element that generates none establishes no
 * scrolling box however its `overflow` computed — core/dom/element_view.h's one predicate decides that, and it
 * is asked here rather than left to each caller, because a caller that forgot would read a `display: none`
 * element's `overflow: auto` and put it in §6.1's ancestor walk. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_SCROLL_CONTAINER_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_SCROLL_CONTAINER_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* Does `el` establish a SCROLLING BOX — is its principal box a css-overflow-3 §3.1 SCROLL CONTAINER. */
bool scroll_container_is(lxb_dom_element_t *el);

/* Does `el` have a CONTENT CLIP — CSSOM VIEW's "computed style has overflow properties that cause its content
 * to be clipped to the element's padding edge", which INTERSECTION OBSERVER §2.2 uses to decide an explicit
 * element root's intersection rectangle and §3.2.7 to decide whether a container clips the walk's rectangle.
 *
 * IT IS A SECOND ENTRY AND NOT A SECOND ANSWER, and the reason is the one value the two questions disagree
 * about. §3.1 lists the SCROLLABLE values as `scroll`, `auto` and `hidden` and calls `visible` and `clip`
 * non-scrollable, so `overflow: clip` establishes NO scroll container — and it is the value css-overflow added
 * precisely so that an author could clip WITHOUT one, so it does have a content clip. Every other input to the
 * two questions is identical: the box precondition, §3.1's `Applies to:` line and §3.1.4's used value are all
 * read once, in this file, from the same computed keyword. A caller that asked `scroll_container_is` and meant
 * this would be wrong for `clip`; a caller that spelled `overflow != visible` itself would be wrong for the
 * other three, and this component exists because that is four readings of a rule none of them cites.
 *
 * A CALLER THAT ASKS THIS IS ASKING WHETHER THE CLIP RECTANGLE IS THE ELEMENT'S PADDING BOX, and the padding
 * box is core/layout/flow_position.h's `flow_padding_box_origin` with core/layout/used_value.h's
 * `used_value_padding_edge_px` on each axis — this entry answers WHETHER, never WHAT. */
bool scroll_container_content_clip_is(lxb_dom_element_t *el);

/* css-overflow-3 §3.1.4 "Overflow Viewport Propagation" — IS `el` THE ONE ELEMENT PER DOCUMENT WHOSE OVERFLOW
 * WAS GIVEN AWAY TO THE VIEWPORT, so that its own USED overflow is `visible` whatever it computed to?
 *
 * ONE FACT, THREE QUESTIONS, WHICH IS WHY IT IS AN ENTRY AND NOT A THIRD `strcmp`. The two predicates above
 * ask it to decide whether a box is a scroll container and whether it has a content clip. CSS 2 §9.4.1's own
 * parenthesis asks the SAME fact for a third: a block box with overflow other than visible establishes a block
 * formatting context "except when that value has been propagated to the viewport", which decides whether that
 * box's margins collapse through its edges — core/layout/block_flow.h's run. A caller that spelled §3.1.4's
 * root-and-body walk itself would be a second copy of a rule whose two arms are exactly the ones that drift.
 *
 * IT IS TOTAL, so there is no precondition a caller can forget. §3.1.4's own `display value is not none`
 * conditions — on the root in its first sentence, on the body in its second — are asked HERE, of both
 * elements. The two predicates above still ask their own box question first, and that is not a second reading
 * of this one: theirs are `a scroll container is a box` and `a content clip is a box's`, which are their rules
 * rather than §3.1.4's.
 *
 * THE ANSWER IS ABOUT THE USED VALUE AND NEVER THE COMPUTED ONE, SO IT MAY NOT BE BUILT INTO THE CASCADE.
 * §3.1.4 ends "The element from which the value is propagated must then have a used overflow value of
 * visible", and CSSOM §9 "Resolved Values" leaves `overflow-x` and `overflow-y` under "Any other property",
 * whose resolved value IS the computed value — so `getComputedStyle(document.body).overflowX` on
 * `body { overflow-x: hidden }` must still answer `"hidden"`. A propagation folded into
 * core/css/css_computed_value.c's `computed_overflow` would answer `"visible"` there, which is a
 * page-observable divergence from every browser rather than a fidelity gain. That is not a hypothetical: the
 * crash this entry replaced named that function as the place to build §3.1.4. RETIREMENT: this paragraph goes
 * when `computed_overflow` cannot read an element's ancestors at all, which makes the wrong layer unspellable
 * rather than merely argued against. */
bool scroll_container_propagates_overflow_to_viewport(lxb_dom_element_t *el);

#endif
