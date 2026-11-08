/* CSSOM VIEW §6 "Extensions to the Element Interface"'s element scroll position: the scrolling area coordinates
 * aligned with the top-left of an element's padding edge, which the scrollTop and scrollLeft getters' last step
 * returns ("Return the y-coordinate of the scrolling area at the alignment point with the top of the padding edge
 * of the element") and CSSOM VIEW §3.1 "Scrolling"'s perform a scroll writes for an element's scrolling box.
 *
 * Held per element as two fields of the node's internal-slot record (core/dom/node.h's node_internal_slots), an
 * object no page code can reach, so reflection, a Proxy, a clone or a freeze of the element neither shows nor
 * refuses it. Every write is a property write the per-flow COW delta captures, so a fork's scroll lives in its
 * own world. An element never scrolled has no fields, and its position is zero on both axes, which is where
 * §6.1's clamp starts every box.
 *
 * The same positions move what a scroll container contains: CSS Overflow 3 §2.3 "Scrolling Overflow" scrolls the
 * scrollable overflow area within the scrollport, so a box is displaced by the position of every scroll container
 * its CSS 2.1 §10.1 containing block chain passes through. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_SCROLL_POSITION_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_SCROLL_POSITION_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared once per agent: the two field names. */
void element_scroll_position_init(JSContext *ctx);
void element_scroll_position_free(JSRuntime *rt);

/* `el`'s scroll position on one axis (`vertical` selects y), zero for an element never scrolled. */
double element_scroll_position(lxb_dom_element_t *el, bool vertical);

/* §3.1's instant scroll of `el`'s scrolling box to (`x`, `y`), already clamped by §6.1's scroll an element. */
void element_scroll_position_set(JSContext *ctx, lxb_dom_element_t *el, double x, double y);

/* The displacement of `el`'s own box by the scroll containers above it: the sum of the positions of each scroll
   container its containing block chain passes through, which a client coordinate subtracts. A `position: fixed`
   box's chain ends at the viewport, so nothing above it displaces it. */
void element_scroll_position_displacement(lxb_dom_element_t *el, double *dx, double *dy);

#endif
