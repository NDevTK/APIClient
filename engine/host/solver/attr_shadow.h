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
/* THE KEY IS THE ATTRIBUTE'S OWN IDENTITY: (owner, slot, NAMESPACE, local name). §4.9 keys an attribute on
   (namespace, local name), so a shadow keyed on the name alone gives `xlink:href` and `href` one taint entry
   between them — one write clobbering the other's provenance, in whichever direction ran last. `ns` is the
   namespace URI, NULL for the null namespace, and is always NULL for ATTR_SLOT_PROPERTY: a DOM property has no
   namespace, which is a fact about the slot rather than a case to special-case.
   `owner` IS THE ELEMENT — except for an attribute whose element is NULL, where it is the ATTR NODE ITSELF.
   §9.4.7's removed attribute keeps its value and `createAttribute` returns one that never had an element, so
   `const a = doc.createAttribute("x"); a.value = location.hash; el.setAttributeNode(a)` writes a source into
   the DOM through an object that has no element at the moment the write happens. Keyed on the element alone
   there is nowhere to record that, and the provenance is gone by the time the attribute is attached. It is an
   opaque KEY and never dereferenced, which is why one field carries both. */
int attr_shadow_find(const void *owner, int kind, const char *ns, const char *name);   /* index, or -1 */
void attr_shadow_set(JSContext *ctx, const void *owner, int kind, const char *ns, const char *name,
                     JSValueConst opaque);   /* JS_UNDEFINED clears */
JSValue attr_shadow_opaque(int i);   /* the shadow opaque at index i (BORROWED — dup it to keep) */
void attr_shadow_free(JSContext *ctx);

#endif
