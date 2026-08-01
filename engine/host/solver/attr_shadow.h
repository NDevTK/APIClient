/* DOM named-slot taint SHADOW — an (element, slot)->opaque side map.
 *
 * When a SOURCE (location.hash, a reply field, ...) is stashed into a DOM string slot — an ATTRIBUTE
 * (el.setAttribute('x', src)) or a text PROPERTY (el.textContent = src) — Lexbor stores only the ToString'd
 * concrete string, losing the opacity/taint. This side map preserves it: the WRITE records the opaque here and
 * the READ recovers it (in baseline/opaque flows), so a source stashed in the DOM by one handler reaches a sink
 * in another. The two SLOT KINDS share one map because they are one concept — a named string slot on an element
 * — but they do not share a NAMESPACE: `el.setAttribute("textContent", x)` and `el.textContent = y` are
 * different slots, so the kind is part of the key rather than a naming convention on top of it.
 * Self-contained: owns the map; the struct is private (callers reach the stored opaque via attr_shadow_opaque). */
#ifndef ENGINE_HOST_ATTR_SHADOW_H
#define ENGINE_HOST_ATTR_SHADOW_H

#include <lexbor/dom/dom.h>
#include "quickjs.h"

enum { ATTR_SLOT_ATTRIBUTE = 0, ATTR_SLOT_PROPERTY = 1 };   /* setAttribute(name) vs a DOM property (textContent) */
int attr_shadow_find(lxb_dom_element_t *el, int kind, const char *name);   /* index of the (el,kind,name) entry, or -1 */
void attr_shadow_set(JSContext *ctx, lxb_dom_element_t *el, int kind, const char *name, JSValueConst opaque);   /* JS_UNDEFINED clears */
JSValue attr_shadow_opaque(int i);   /* the shadow opaque at index i (BORROWED — dup it to keep) */
void attr_shadow_free(JSContext *ctx);

#endif
