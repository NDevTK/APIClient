/* DOM attribute taint SHADOW — an (element,name)->opaque side map.
 *
 * When a SOURCE (location.hash, a reply field, ...) is stashed into a DOM attribute (el.setAttribute('x', src)),
 * Lexbor stores only the ToString'd concrete string, losing the opacity/taint. This side map preserves it: the
 * attribute WRITE records the opaque here, and the attribute READ recovers it (in baseline/opaque flows), so a
 * source stashed in the DOM by one handler reaches a sink in another. Self-contained: owns the map; the struct
 * is private (callers reach the stored opaque via attr_shadow_opaque). */
#ifndef ENGINE_HOST_ATTR_SHADOW_H
#define ENGINE_HOST_ATTR_SHADOW_H

#include <lexbor/dom/dom.h>
#include "quickjs.h"

int attr_shadow_find(lxb_dom_element_t *el, const char *name);   /* index of the (el,name) entry, or -1 */
void attr_shadow_set(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst opaque);   /* JS_UNDEFINED clears */
JSValue attr_shadow_opaque(int i);   /* the shadow opaque at index i (BORROWED — dup it to keep) */
void attr_shadow_free(JSContext *ctx);

#endif
