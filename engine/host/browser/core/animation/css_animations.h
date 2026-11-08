/* CSS Animations: the CSSAnimation objects an element's computed `animation-*` values generate, and CSS Animations
 * Level 2 §6.1 "The CSSAnimation interface".
 *
 * CSS Animations Level 1 §2 "CSS Animations Model" says an animation applies to an element when its name is in the
 * computed `animation-name` and a `@keyframes` rule of that name exists, and how a change to `animation-name`
 * matches existing animations; Level 2 §2.1 "Owning element" gives each one an owning element and §2.2 "Animation
 * composite order" its order. An element's animations are held on its wrapper, in `animation-name` order, so every
 * update is the running flow's own write.
 *
 * Named residual: the update runs at the style change event Web Animations §6.8's getAnimations() triggers, and at no
 * other. The next diff runs it at every style change event, update-the-rendering's style recalculation first. Its
 * absence shows as a CSSAnimation's start time dating from the first getAnimations() call rather than from the
 * first style resolution, and as no CSSAnimation existing for an element nobody asked about. */
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

/* CSS Animations Level 2 §2.2: is `animation` ordered as a CSS animation with an owning element (or, canceled
   and not since played, by the owning element it was canceled off)? */
bool css_animations_owned(JSContext *ctx, JSValueConst animation);
/* §2.2's order between two such animations: tree order of the owning elements, then position in `animation-name`.
   Negative when `a` sorts first. */
int css_animations_order(JSContext *ctx, JSValueConst a, JSValueConst b);

#endif
