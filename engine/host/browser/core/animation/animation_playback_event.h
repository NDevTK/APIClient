/* Web Animations Level 1 §6.12 "The AnimationPlaybackEvent interface": the `finish`, `cancel` and `remove`
 * events an Animation receives. Its two attributes are time values (null when unresolved) held in internal
 * slots on the event. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_PLAYBACK_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_PLAYBACK_EVENT_H

#include "quickjs.h"

void animation_playback_event_init(JSContext *ctx);
/* The Window half: the interface object. */
void animation_playback_event_install(JSContext *ctx, JSValueConst global);
void animation_playback_event_free(JSRuntime *rt);

/* A trusted event of `type` the user agent queues, neither bubbling nor cancelable, with its two attributes
   (borrowed time values). OWNED. */
JSValue animation_playback_event_new(JSContext *ctx, const char *type, JSValueConst current_time,
                                     JSValueConst timeline_time);

#endif
