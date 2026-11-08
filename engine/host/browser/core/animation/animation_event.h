/* CSS Animations Level 1 §5.1 "The AnimationEvent Interface", with CSS Animations Level 2 §5.2 "The
 * AnimationEvent Interface"'s `animation` member: the `animationstart`, `animationiteration`, `animationend` and
 * `animationcancel` events a CSS animation's owning element receives. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_EVENT_H

#include "quickjs.h"

/* Declared by core/animation/animation.c's init, as part of its row. */
void animation_event_init(JSContext *ctx);
void animation_event_install(JSContext *ctx, JSValueConst global);
void animation_event_free(JSRuntime *rt);

/* A trusted event of `type` the user agent queues: bubbling, not cancelable (Level 1 §5.2), for the CSSAnimation
   `animation` (borrowed), its `animationName` and `elapsedTime` in seconds, on no pseudo-element. OWNED. */
JSValue animation_event_new(JSContext *ctx, const char *type, JSValueConst animation, JSValueConst name,
                            double elapsed_s);

#endif
