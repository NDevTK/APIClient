/* A Document's animation state and Web Animations Level 1 §4.4 "Animation Frames"'s update animations and send
 * events, which HTML §8.1.7.3 "update the rendering" step 11 runs for each document.
 *
 * The record is per realm (a realm is one Document here, document.open() included, which §4.3.1.1 says keeps its
 * default document timeline) and is built at realm install, so it is baseline and every flow's writes to it are
 * that flow's own delta. It holds the `now` timestamp the procedure was last given, the document's timelines in
 * creation order, the pending animation event queue, and the copy step 4 takes for steps 6-7 to dispatch. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_DOCUMENT_ANIMATIONS_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_DOCUMENT_ANIMATIONS_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

void document_animations_init(JSContext *ctx);
void document_animations_free(JSRuntime *rt);

/* This realm's record. OWNED. */
JSValue document_animations_record(JSContext *ctx);
/* The `now` the record's procedure was last run with, as a time value: null before the first run. OWNED. */
JSValue document_animations_now(JSContext *ctx, JSValueConst record);
/* Associate a document timeline with the record's Document (§4.3 "A timeline can be associated with a document"). */
void document_animations_add_timeline(JSContext *ctx, JSValueConst record, JSValueConst timeline);
/* Append `ev` (consumed) to the record's pending animation event queue with its target and scheduled event time. */
void document_animations_append_event(JSContext *ctx, JSValueConst record, JSValue ev, JSValueConst target,
                                      JSValueConst scheduled);

/* Web Animations §4.5's global animation list, appended to: the position (from 1) the appended animation holds. The
   list's order is all §5.4.2 reads of it, so a per-realm count in creation order is the list. */
uint32_t document_animations_list_append(JSContext *ctx);

/* Every animation associated with one of this realm's Document's timelines, timeline by timeline. OWNED. */
JSValue document_animations_all(JSContext *ctx);

/* HTML §8.1.7.3 step 4's "no visible effect" term for this document: an animation of it owes a pending task,
   a finish its timeline has not reached, or an event the queue holds. */
bool document_animations_pending(JSContext *docctx);

/* §4.4 steps 1-5 for the Document of `docctx` at `now` (borrowed): update every timeline and the animations
   associated with them, remove replaced animations, the microtask checkpoint, and the copy, clear and stable sort
   of the pending animation event queue. Answers how many events steps 6-7 dispatch. */
uint32_t document_animations_update(JSContext *docctx, JSValueConst now);
/* Step 7's `i`th event and its target, both OWNED. */
void document_animations_event_at(JSContext *docctx, uint32_t i, JSValue *target, JSValue *ev);
/* Step 7 is done: the copy is released. */
void document_animations_dispatch_end(JSContext *docctx);

#endif
