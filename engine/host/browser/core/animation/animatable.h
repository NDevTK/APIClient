/* Web Animations Level 1 §6.8 "The Animatable interface mixin"'s getAnimations() on Element (§6.11 "Extensions to
 * the Element interface") and §6.10 "Extensions to the DocumentOrShadowRoot interface mixin"'s getAnimations() on
 * Document and ShadowRoot.
 *
 * Each triggers a style change event over the target (core/animation/css_animations.h's update, for the target
 * element and, for a subtree, every element below it), then answers §4.6.7's relevant animations in §5.4.2's
 * composite order. The event's walk is a step machine on a layout job of its own: it updates elements under a
 * resting drive, charged as a DOM walk, and rests between them where the drive's allowance is spent. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATABLE_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATABLE_H

#include "quickjs.h"

/* Declared by core/animation/animation.c's init, as part of its row. */
void animatable_init(JSContext *ctx);
void animatable_free(void);
/* `getAnimations` on Element.prototype, and on Document.prototype or ShadowRoot.prototype. */
void animatable_install_element_members(JSContext *ctx, JSValueConst element_proto);
void animatable_install_document_or_shadow_root_members(JSContext *ctx, JSValueConst proto);

/* The process's census of the style change event's walk: the elements it visited, and the rests it took. */
unsigned long long animatable_style_change_elements_life(void);
unsigned long long animatable_style_change_rests_life(void);

#endif
