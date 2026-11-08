/* CSS Animations: the CSSAnimation objects an element's computed `animation-*` values generate, and CSS Animations
 * Level 2 §6.1 "The CSSAnimation interface".
 *
 * CSS Animations Level 1 §2 "CSS Animations Model" says an animation applies to an element when its name is in the
 * computed `animation-name` and a `@keyframes` rule of that name exists, and how a change to `animation-name`
 * matches existing animations; Level 2 §2.1 "Owning element" gives each one an owning element and §2.2 "Animation
 * composite order" its order. An element's animations are held on its wrapper, in `animation-name` order, so every
 * update is the running flow's own write.
 *
 * The update runs at the style change events getAnimations() triggers and at update the rendering step 16.2.1,
 * the latter whenever a cascade input was written since the Document's last one (document_animations_style_stale).
 * Named residual: an environment change that moves computed values with no cascade input write (a viewport resize
 * flipping a media query) does not mark the Document stale. The next diff raises the cascade input generation at
 * the environment facts a media query reads. Its absence shows as an animation a resize turns on or off appearing
 * only after the page next writes a cascade input. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_CSS_ANIMATIONS_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_CSS_ANIMATIONS_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared by core/animation/animation.c's init, as part of its row. */
void css_animations_init(JSContext *ctx);
void css_animations_install(JSContext *ctx, JSValueConst global);
void css_animations_free(JSRuntime *rt);

/* Update `el`'s CSS animations from its computed values, as a style change event does. */
void css_animations_update(lxb_dom_element_t *el);
/* `el`'s CSS animations, in `animation-name` order, as an array. OWNED. */
JSValue css_animations_of(JSContext *ctx, lxb_dom_element_t *el);

/* Does `v` implement CSSAnimation? */
bool css_animations_is(JSContext *ctx, JSValueConst v);
/* CSS Animations Level 2 §5.1's event dispatch for one animation as a new animation frame is established: the
   phase and current iteration compared with the previous frame's, and the events the table names queued on the
   timeline's Document with the owning element as target. Nothing for an animation with no owning element. */
void css_animations_frame_events(JSContext *ctx, JSValueConst animation);

/* CSS Animations Level 2 §2.2: is `animation` ordered as a CSS animation with an owning element (or, canceled
   and not since played, by the owning element it was canceled off)? */
bool css_animations_owned(JSContext *ctx, JSValueConst animation);
/* CSS Animations Level 2 §2.2's order between two such animations: tree order of the owning elements, then position in `animation-name`.
   Negative when `a` sorts first. */
int css_animations_order(JSContext *ctx, JSValueConst a, JSValueConst b);

#endif
