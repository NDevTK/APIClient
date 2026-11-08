/* Web Animations Level 1 §4.3 "Timelines" and §4.3.1 "Document timelines", with §6.2 "The AnimationTimeline
 * interface", §6.3 "The DocumentTimeline interface" and §6.9 "Extensions to the Document interface".
 *
 * A document timeline's current time is "a fixed offset from the now timestamp provided each time the update
 * animations and send events procedure is run", so it is derived at every read from the `now` its Document's
 * record holds (core/animation/document_animations.h) minus the timeline's origin time. Before that procedure has
 * run for the Document there is no now timestamp, and the timeline is inactive. Every document timeline is
 * monotonically increasing (§4.3.1: "After a document timeline becomes active, it is monotonically increasing").
 *
 * Named residual: §4.3.1's "a document timeline that is associated with a Document that is not an active document
 * is also considered to be inactive" is not asked. The next diff asks the Document's active state, kept by
 * core/frame, at the current-time read. Its absence shows as a timeline of a document that has been navigated
 * away from answering the last time it was given instead of null. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_TIMELINE_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_TIMELINE_H

#include <stdbool.h>

#include "quickjs.h"

void animation_timeline_init(JSContext *ctx);
/* The Window half: the AnimationTimeline and DocumentTimeline interface objects. */
void animation_timeline_install(JSContext *ctx, JSValueConst global);
/* §6.9's `timeline` on Document.prototype, installed by the component that owns Document.prototype. */
void animation_timeline_install_document_members(JSContext *ctx, JSValueConst document_proto);
void animation_timeline_free(JSRuntime *rt);

/* Does `v` implement AnimationTimeline? */
bool animation_timeline_is(JSContext *ctx, JSValueConst v);
/* §4.3.1.1's default document timeline of this realm's Document. OWNED. */
JSValue animation_timeline_default(JSContext *ctx);
/* The timeline's current time, null while it is inactive. OWNED. */
JSValue animation_timeline_current_time(JSContext *ctx, JSValueConst timeline);
/* §4.3.1's convert a timeline time to an origin-relative time. OWNED. */
JSValue animation_timeline_to_origin_relative(JSContext *ctx, JSValueConst timeline, JSValueConst time);
/* The record of the Document the timeline is associated with (core/animation/document_animations.h). OWNED. */
JSValue animation_timeline_document_record(JSContext *ctx, JSValueConst timeline);

/* The animations associated with the timeline, in the order they were associated, as an array. OWNED. */
JSValue animation_timeline_animations(JSContext *ctx, JSValueConst timeline);
/* Associate `animation` with the timeline, or dissociate it; the Animation's own timeline slot is its caller's. */
void animation_timeline_associate(JSContext *ctx, JSValueConst timeline, JSValueConst animation);
void animation_timeline_dissociate(JSContext *ctx, JSValueConst timeline, JSValueConst animation);

#endif
