/* Web Animations Level 1 §6.8 "The Animatable interface mixin"'s getAnimations() on Element (§6.11 "Extensions to
 * the Element interface") and §6.10 "Extensions to the DocumentOrShadowRoot interface mixin"'s getAnimations() on
 * Document and ShadowRoot.
 *
 * Each triggers a style change event over the target (core/animation/css_animations.h's update, for the target
 * element and, for a subtree, every element below it), then answers §4.6.7's relevant animations in §5.4.2's
 * composite order. The event is core/animation/style_change_event.h's resting walk, so the member rests with it. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATABLE_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATABLE_H

#include "quickjs.h"

/* Declared by core/animation/animation.c's init, as part of its row. */
void animatable_init(JSContext *ctx);
void animatable_free(void);
/* `getAnimations` on Element.prototype, and on Document.prototype or ShadowRoot.prototype. */
void animatable_install_element_members(JSContext *ctx, JSValueConst element_proto);
void animatable_install_document_or_shadow_root_members(JSContext *ctx, JSValueConst proto);

#endif
