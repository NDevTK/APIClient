/* GEOMETRY INTERFACES §4 — `DOMRectList`, the list CSSOM VIEW §6's `getClientRects()` answers with. See
 * dom_rect_list.c.
 *
 * IT IS A SEPARATE COMPONENT FROM §3's RECTANGLE because it is a separate PROBLEM: §3 is four numbers and four
 * derivations over them, §4 is an indexed-property interface over a sequence of them, and the standard already
 * says the two have nothing to do with each other — "DOMRectList only exists for compatibility with legacy Web
 * content. When specifying a new API, DOMRectList must not be used. Use sequence<DOMRect> instead." A legacy
 * container inside the interface every new API is told to return directly would be the wrong file forever. */
#ifndef ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_RECT_LIST_H
#define ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_RECT_LIST_H

#include "quickjs.h"

/* Declared once per AGENT: the class and `item`'s pool entry. It REGISTERS the per-realm prototype install. */
void dom_rect_list_init(JSContext *ctx);
void dom_rect_list_install_proto(JSContext *ctx);
void dom_rect_list_install(JSContext *ctx, JSValueConst global);
void dom_rect_list_free(JSContext *ctx);

/* §4's list over `rects`, an Array of DOMRect in the order the caller's algorithm produced them — CSSOM VIEW
 * §6's is "in content order, one for each box fragment". CONSUMES `rects`.
 *
 * AN EMPTY LIST IS NOT A SEPARATE ENTRY POINT: getClientRects step 1's "return an empty DOMRectList" is the
 * CALLER stating that the element generates no fragments, and it says so by passing an empty Array. A component
 * with an `_empty()` of its own would be a second construction whose emptiness nothing downstream could tell
 * apart from a list that happened to come out empty.
 *
 * THE RECTANGLES ARE HELD AS A JS ARRAY, never as a malloc'd C vector: a list a flow holds must fork per flow
 * and park to the cold tier with it, and an Array's mutations are property writes the COW delta already
 * captures — CLAUDE.md's rule for platform data a flow queues. */
JSValue dom_rect_list_new(JSContext *ctx, JSValue rects);

#endif
