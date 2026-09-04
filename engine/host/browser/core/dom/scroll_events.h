/* CSSOM VIEW §13.2 "Scrolling" — a Document's PENDING SCROLL EVENTS, the appends that fill the list, and the
 * SCROLL STEPS that drain it.
 *
 * WHY ONE FILE. §13.2 is a queue and its drain, and the two are one contract: a list with only a writer is a
 * value nothing reads and a list with only a reader is a value nobody writes, and each of those is a defect
 * this codebase has met by name. So the appends and the drain live together, and the only thing outside is the
 * FIRING — which runs the page's listeners and therefore parks, so it belongs to the step machine that owns
 * HTML §8.1.7.3 "Processing model"'s update-the-rendering (core/rendering/rendering.c, its step 9).
 *
 * THE LIST IS A JS ARRAY ON A PER-REALM RECORD, and that is the whole of how it time-travels. §13.2 gives each
 * `Document` "an associated list of pending scroll events, which stores pairs of (`EventTarget`, `DOMString`),
 * initially empty" — platform data a FLOW queues, so it must fork per flow and park with the flow that queued
 * it. Every mutation below is an ordinary property write, which the per-flow COW delta already captures; a
 * malloc'd list captured as head/tail pointers would revert the POINTERS on a context switch and leave the
 * nodes reachable from nothing.
 *
 * AND IT COSTS NOTHING PER FLOW UNTIL A FLOW QUEUES SOMETHING. The record is built WITH the realm, before any
 * page script runs, so it is part of the COW BASELINE that every flow shares rather than one object per member
 * of the frontier. What that makes load-bearing is the WRITE side: these steps run for every document of every
 * rendering opportunity, so a write made unconditionally would put an entry in EVERY live flow's delta once
 * per frame — which is why step 3's empty is conditional and why §3.1's step 7.1 mark is a write only when the
 * position actually changed. core/frame/viewport.c's §13.1 resize latch states the same rule about itself.
 *
 * TWO COLLECTIONS AND NOT ONE, because §13.2 names two. The LIST holds the pairs that will be fired. The set
 * of "each scrolling box box that was scrolled" is separate: §3.1 "Scrolling"'s perform a scroll step 7.1
 * marks a box there when the position changed, and the scroll steps' own step 1 is what turns that set into
 * (target, `scrollend`) pairs — one frame later than the `scroll` pair, which the gets-scrolled steps append
 * as the scroll happens. Collapsing the two would put `scrollend` in the list at the moment of the scroll,
 * which is the same order for an INSTANT scroll and the wrong one for a smooth scroll that is still running.
 * IN THIS BUILD THAT SET HAS AT MOST ONE MEMBER — the viewport — and is a boolean for that reason rather than
 * as a narrowing: an ELEMENT cannot be in it, because an element that reaches a real perform-a-scroll aborts
 * for want of a place to put its position (core/dom/perform_scroll.c). The diff that gives an element a scroll
 * position is the diff that makes this a set.
 *
 * A VIEWPORT'S TARGET IS ITS DOCUMENT, AND THAT IS READ OUT OF §13.2's OWN STEP 2 RATHER THAN CHOSEN. Its step
 * 1.1 says to let target be "the viewport", and a viewport is not an object any Web IDL interface exposes —
 * §4's members are on `Window` and §12's on `VisualViewport`, and neither IS the viewport. Step 2's first
 * branch settles it: "If target is a `Document`, and type is `scroll` or `scrollend`, fire an event named type
 * that bubbles at target." A `scrollend` reaches that branch ONLY if some target that produces one is a
 * Document — an ELEMENT's scrollend takes step 2's last branch and fires at the element — so the standard
 * naming `scrollend` beside `scroll` there is the statement that a viewport's target is its Document. The
 * gets-scrolled steps for a viewport already append the DOCUMENT by name, so this makes the two halves agree.
 *
 * SNAP CONTAINERS ARE ASSERTED UNREACHABLE, not skipped: css-scroll-snap-1 declares `scroll-snap-type` and
 * this cascade models no such property, so `scroll-snap-type` computes to its initial `none` for every element
 * in every document this engine parses and no box is a snap container. Three of §13.2's steps are about one
 * (the two update-scrollsnapchanging calls and step 2's two SnapEvent branches); each is asserted at its
 * place, so the day the cascade models the property they fire where the step to build is. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_SCROLL_EVENTS_H
#define ENGINE_HOST_BROWSER_CORE_DOM_SCROLL_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* Declared once per AGENT: the realm-value slot the record lives in. The record itself is built WITH each
   realm, so no reader has to mint one inside whichever flow reached it first. */
void scroll_events_init(JSContext *ctx);
void scroll_events_free(void);

/* §13.2's "Whenever a viewport gets scrolled (whether in response to user interaction or by an API), the user
   agent must run these steps" — all four of them, over the viewport of `ctx`'s active document. Run by §3.1's
   perform a scroll at the moment the position changes. */
void scroll_events_viewport_scrolled(JSContext *ctx);

/* §3.1's step 7.1, "If the scroll position changed as a result of this call, emit the scrollend event", for a
   VIEWPORT — which is to make it a member of the set §13.2's scroll steps step 1 iterates. It does NOT append
   to the list; step 1 is what does that, on the next run of the scroll steps. */
void scroll_events_viewport_scrollend(JSContext *ctx);

/* Does this document give update-the-rendering anything to do on §13.2's account? HTML §8.1.7.3 step 4 removes
   a doc "for which the user agent believes updating the rendering would have no visible effect and whose map
   of animation frame callbacks is empty", and a document holding a queued `scroll` has a visible effect
   pending — without this term the frame is never queued, step 9 is written and UNREACHABLE, and a page's
   `scroll` listener never runs. */
bool scroll_events_pending(JSContext *ctx);

/* §13.2's scroll steps STEP 1 — "For each scrolling box box that was scrolled" — followed by the COUNT step 2
   will iterate. The count is taken here, at the one moment both steps' state is settled: a listener that
   scrolls again during the drain APPENDS, and an append cannot move an item already in the list, so an index
   below this count names the same pair for the whole of the walk. */
uint32_t scroll_events_scroll_steps_begin(JSContext *ctx);

/* §13.2's scroll steps STEP 2, for one item: places that item's TARGET and the EVENT step 2 says to fire at
   it, which is where §13.2's four branches over the pair are decided. Both are OWNED — the caller frees. */
void scroll_events_item(JSContext *ctx, uint32_t i, JSValue *target, JSValue *ev);

/* §13.2's scroll steps STEP 3 — "Empty doc's pending scroll events." */
void scroll_events_scroll_steps_end(JSContext *ctx);

#endif
