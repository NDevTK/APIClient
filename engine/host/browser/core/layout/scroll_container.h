/* CSSOM View §2 "Terminology"'s scrolling box: whether an element establishes one. §6 and §6.1 ask it, and
 * CSSOM View never defines it; the definition is css-overflow-3 §3.1 "Managing Overflow: the overflow-x,
 * overflow-y, and overflow properties"' scroll container: "The scroll, auto, and hidden values are known as the
 * scrollable values of overflow. They cause the box to be a scroll container and the affected axis to be a
 * scrollable axis", and "If neither axis computes to a scrollable value, the box is not a scroll container."
 * The join is made once here, so `hidden` (scrollable, no scrollbar) and `clip` (not) are read one way.
 *
 * §2's note lets a non-overflowing `auto` box have no scrolling box ("might not have a scrolling box"). Taking
 * it changes no answer: §6's step 10 tests overflow separately, and a non-overflowing scroll container's
 * scrolling area is its padding box (core/layout/scrolling_area.h), so its only scroll position is the origin.
 *
 * Also asked here: §3.1's `Applies to:` line ("block containers, flex containers, grid containers, and table
 * grid boxes", via core/css/css_property_applies.h); css-overflow-3 §3.1.4 "Overflow Viewport Propagation",
 * whose propagating element "must then have a used overflow value of visible" (so `<html style="overflow:auto">`
 * is not one, the viewport is); and that a box exists (core/dom/element_view.h), for `display: none`. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_SCROLL_CONTAINER_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_SCROLL_CONTAINER_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* Whether `el` establishes a scrolling box: whether its principal box is a css-overflow-3 §3.1 scroll
   container. */
bool scroll_container_is(lxb_dom_element_t *el);

/* Whether `el` has a content clip: CSSOM View's "computed style has overflow properties that cause its content
 * to be clipped to the element's padding edge", which Intersection Observer §2.2 uses for an explicit element
 * root's intersection rectangle and §3.2.7 for whether a container clips the walk's rectangle.
 *
 * It differs from `scroll_container_is` in one value: `clip` is non-scrollable but clips. The box
 * precondition, the `Applies to:` line and §3.1.4's used value are shared. This answers whether the clip is the
 * padding box, never what it is (core/layout/flow_position.h's `flow_padding_box_origin` and
 * core/layout/used_value.h's `used_value_padding_edge_px`). */
bool scroll_container_content_clip_is(lxb_dom_element_t *el);

/* css-overflow-3 §3.1.4 "Overflow Viewport Propagation": is `el` the one element per document whose overflow
 * was given to the viewport, so that its own used overflow is `visible` whatever it computed to?
 *
 * The two predicates above ask it, and so does CSS 2 §9.4.1's parenthesis (overflow other than visible
 * establishes a block formatting context "except when that value has been propagated to the viewport"), in
 * core/layout/block_flow.h's margin-collapsing run. It is total: §3.1.4's own "display value is not none"
 * conditions on the root and the body are asked here.
 *
 * It must not be built into the cascade: CSSOM §9 "Resolved Values" resolves `overflow-x`/`overflow-y` to the
 * computed value, so `getComputedStyle(document.body).overflowX` on `body { overflow-x: hidden }` answers
 * `"hidden"`; folding propagation into core/css/css_computed_value.c's `computed_overflow` would answer
 * `"visible"`. */
bool scroll_container_propagates_overflow_to_viewport(lxb_dom_element_t *el);

#endif
