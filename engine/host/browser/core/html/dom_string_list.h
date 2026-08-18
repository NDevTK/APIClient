/* HTML §2.6.5 — `DOMStringList`, "a non-fashionable retro way of representing a list of strings". See
 * dom_string_list.c.
 *
 * IT IS AN HTML TYPE AND NOT AN INDEXED DATABASE ONE, which is why it lives here and not under
 * core/indexeddb/. Indexed Database §4.4, §4.5 and §4.10 are three of its consumers and HTML §7.2.5's
 * `ancestorOrigins` is a fourth; a list defined in the standard every one of them imports it from would be the
 * wrong file the moment the second consumer landed.
 *
 * THE STRINGS ARE HELD AS A JS ARRAY, never as a malloc'd C vector — the same rule core/geometry/
 * dom_rect_list.h states for the same reason: a list a flow holds must fork per flow and park to the cold tier
 * with it, and an Array's mutations are property writes the COW delta already captures.
 *
 * §2.6.5 GIVES THE LIST NO WRITE PATH. "Each DOMStringList object has an associated list" and the three
 * members only read it, so a list is built COMPLETE by its producer and never changes afterwards — which is
 * also what Indexed Database §5.12 means by "return a NEW DOMStringList associated with sorted" at each read
 * rather than one object kept in step. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DOM_STRING_LIST_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DOM_STRING_LIST_H

#include "quickjs.h"

/* Declared once per AGENT: the class, the slot key and the two operations' pool entries. It REGISTERS the
   per-realm prototype install. */
void dom_string_list_init(JSContext *ctx);
void dom_string_list_install_proto(JSContext *ctx);
void dom_string_list_install(JSContext *ctx, JSValueConst global);
void dom_string_list_free(JSRuntime *rt);

/* §2.6.5's list over `strings`, an Array of DOMString in the order the producer's algorithm put them in —
 * this interface defines no ordering of its own, so a producer that has one (Indexed Database §5.12's code-unit
 * sort) applies it before it gets here. CONSUMES `strings`.
 *
 * AN EMPTY LIST IS NOT A SEPARATE ENTRY POINT: a producer with nothing to list says so by passing an empty
 * Array, for the reason core/geometry/dom_rect_list.h gives. */
JSValue dom_string_list_new(JSContext *ctx, JSValue strings);

#endif
