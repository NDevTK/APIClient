/* Web Animations Level 1 §4.5 "Animations" and §6.4 "The Animation interface".
 *
 * An animation's §4.5 state lives in internal slots on a null-prototype object, so every write is the running
 * flow's own and a fork's sibling never sees it.
 *
 * A pending play or pause task runs "as soon as animation is ready" (§4.5.6): the user agent has rendered the
 * associated effect's first frame and the timeline is active. That is the next update animations and send events
 * for the timeline's Document (core/animation/document_animations.h), whose `now` is the ready time.
 *
 * The associated effect is held as its timing alone (§4.6's start delay, iteration duration, iteration count, end
 * delay, fill mode and direction): what §4.5's algorithms and §4.6.7's relevance read of it.
 * Named residual: no AnimationEffect or KeyframeEffect object exists, so §6.4's `effect` attribute and
 * `commitStyles()` are not installed and an effect's keyframes and target properties are not modelled. The next
 * diff builds §6.5 "The AnimationEffect interface" and §6.6 "The KeyframeEffect interface" over the timing record
 * this file holds. Its absence shows as `animation.effect` reading undefined. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_H

#include <stdbool.h>

#include "quickjs.h"

void animation_init(JSContext *ctx);
/* The Window half: the Animation interface object. */
void animation_install(JSContext *ctx, JSValueConst global);
void animation_free(JSRuntime *rt);

/* Does `v` implement Animation (CSSAnimation included)? */
bool animation_is(JSContext *ctx, JSValueConst v);
/* Animation.prototype of this realm, for an interface inheriting it. OWNED. */
JSValue animation_proto(JSContext *ctx);

/* A new animation with prototype `proto` (borrowed; Animation.prototype or an inheriting interface's): §6.4's
   constructor steps over `timeline` (null for none) and an associated effect given by its timing record `effect`
   (null for none). Not appended to the global animation list when `listed` is false (CSS Animations Level 2 §2.2
   keeps an animation with an owning element off it). OWNED. */
JSValue animation_create(JSContext *ctx, JSValueConst proto, JSValueConst timeline, JSValueConst effect, bool listed);
/* The animation's internal slots, for a component layering an interface over Animation. OWNED. */
JSValue animation_slots(JSContext *ctx, JSValueConst animation);

/* §4.5.8 play an animation (auto-rewind), §4.5.9 pause an animation and §4.5.14 cancel an animation, for a
   caller other than the page. Play and pause answer false with a DOMException pending where the procedure
   throws. */
bool animation_play(JSContext *ctx, JSValueConst animation, bool auto_rewind);
bool animation_pause(JSContext *ctx, JSValueConst animation);
void animation_cancel(JSContext *ctx, JSValueConst animation);
/* §4.5.17's play state, as AnimationPlayState's string. */
const char *animation_play_state(JSContext *ctx, JSValueConst animation);

/* §4.4 step 1 for one animation associated with a timeline just updated: run its pending task if it is now ready,
   then update its finished state. */
void animation_frame_service(JSContext *ctx, JSValueConst animation);
/* Does this animation owe its Document another update animations and send events: a pending task, or a running
   playback whose end the timeline has not reached? */
bool animation_needs_frame(JSContext *ctx, JSValueConst animation);
/* §5.5.2's replaceable. */
bool animation_replaceable(JSContext *ctx, JSValueConst animation);
/* §4.6.7: is this animation relevant (associated effect current or in effect, replace state not removed)? */
bool animation_relevant(JSContext *ctx, JSValueConst animation);
/* §5.4.2's composite order of the animations of two effects: negative when `a` sorts first, zero when equal. */
int animation_composite_order(JSContext *ctx, JSValueConst a, JSValueConst b);

#endif
