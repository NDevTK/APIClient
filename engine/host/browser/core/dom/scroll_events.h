/* CSSOM View §13.2 "Scrolling": a Document's pending scroll events, the appends that fill the list, and the
 * scroll steps that drain it. The firing runs the page's listeners and parks, so it belongs to update the
 * rendering's step machine (core/rendering/rendering.c).
 *
 * The list is a JS Array on a per-realm record built with the realm. It is therefore baseline, every mutation is
 * a property write the per-flow COW delta captures, and a flow carries nothing until it scrolls. The scroll steps
 * run for every document of every frame, so each write they make is made only when there is something to write.
 *
 * A viewport's pair names its Document: §13.2's step 1.1 says "the viewport", which no Web IDL interface
 * exposes, and step 2's first branch fires `scroll` and `scrollend` at a target that "is a Document".
 *
 * Snap containers are asserted unreachable: the cascade models no `scroll-snap-type`, so no box is one, and
 * each of §13.2's snap steps asserts at its place. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_SCROLL_EVENTS_H
#define ENGINE_HOST_BROWSER_CORE_DOM_SCROLL_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared once per AGENT: the realm-value slot the record lives in. The record itself is built WITH each
   realm, so no reader has to mint one inside whichever flow reached it first. */
void scroll_events_init(JSContext *ctx);
void scroll_events_free(void);

/* §13.2's "Whenever a viewport gets scrolled (whether in response to user interaction or by an API), the user
   agent must run these steps" — all four of them, over the viewport of `ctx`'s active document. Run by §3.1's
   perform a scroll at the moment the position changes. */
void scroll_events_viewport_scrolled(JSContext *ctx);

/* The order across targets and frames is Chrome's where it differs from §13.2's text (Chrome 153.0.8010.12;
   test_forced.c's scroll-events-element row holds the measured sequence):
   - §13.2's scroll steps step 1 turns "each scrolling box box that was scrolled" into (target, `scrollend`)
     pairs when the steps run, so every `scrollend` follows every pending `scroll`. Chrome fires a box's `scroll`
     then its `scrollend` before the next box's `scroll`. So an instant scroll appends both of its pairs when it
     happens, and there is no was-scrolled set.
   - §13.2's step 3 empties the pending scroll events after step 2's walk. Chrome fires a pair a listener queued
     during the walk in the next frame, even a `scroll` at the target whose `scroll` is being fired. So the steps
     take the list whole before the walk and leave a fresh one to append to. */

/* §3.1's step 7.1, "If the scroll position changed as a result of this call, emit the scrollend event", for a
   viewport: appends (document, `scrollend`) unless it is already pending. */
void scroll_events_viewport_scrollend(JSContext *ctx);

/* §13.2's "Whenever an element gets scrolled" steps, and §3.1's step 7.1 append, for an element of `ctx`'s
   document. The element's `scroll` and `scrollend` fire at it and do not bubble (scroll steps step 2). */
void scroll_events_element_scrolled(JSContext *ctx, lxb_dom_element_t *el);
void scroll_events_element_scrollend(JSContext *ctx, lxb_dom_element_t *el);

/* Does this document give update-the-rendering anything to do on §13.2's account? HTML §8.1.7.3 step 4 removes
   a doc "for which the user agent believes updating the rendering would have no visible effect and whose map
   of animation frame callbacks is empty", and a document holding a queued `scroll` has a visible effect
   pending — without this term the frame is never queued, step 9 is written and UNREACHABLE, and a page's
   `scroll` listener never runs. */
bool scroll_events_pending(JSContext *ctx);

/* Begins §13.2's scroll steps: takes the pending list for step 2 to walk, leaves an empty one for listeners to
   append to, and answers the taken list's length. An index below it names one pair for the whole walk. */
uint32_t scroll_events_scroll_steps_begin(JSContext *ctx);

/* §13.2's scroll steps STEP 2, for one item: places that item's TARGET and the EVENT step 2 says to fire at
   it, which is where §13.2's four branches over the pair are decided. Both are OWNED — the caller frees. */
void scroll_events_item(JSContext *ctx, uint32_t i, JSValue *target, JSValue *ev);

/* §13.2's scroll steps step 3, "Empty doc's pending scroll events", over the list the steps took. */
void scroll_events_scroll_steps_end(JSContext *ctx);

#endif
