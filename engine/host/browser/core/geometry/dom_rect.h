/* GEOMETRY INTERFACES §3 — `DOMRectReadOnly` and `DOMRect`, the rectangle every geometric answer in the
 * platform is returned as. See dom_rect.c.
 *
 * IT IS A COMPONENT OF ITS OWN, IN A SPEC-NAMED DIRECTORY, because a rectangle belongs to NEITHER of the
 * standards that will read it: CSSOM VIEW §6 returns one from `getBoundingClientRect`, Intersection Observer
 * §3.1 puts three on every entry, Resize Observer's box sizes are built out of them, and Geometry Interfaces
 * is where all three go to find out what one IS. Writing it inside the first caller would put §3's own
 * algorithms — the NaN-safe edges, `fromRect`, the default `toJSON` — behind whichever component happened to
 * need a rectangle first.
 *
 * NOTHING HERE IS LAYOUT. §3 defines a rectangle as four numbers and four derivations over them; it says
 * nothing about where a box is, and it is complete without a layout engine. That is why this component has no
 * gap in it at all while core/dom/element_view.c — the caller that has to say WHICH four numbers an element's
 * box has — still crashes at the branch that needs one. */
#ifndef ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_RECT_H
#define ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_RECT_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared once per AGENT: the two classes, the constructors, the statics and the four setters. It REGISTERS
   the per-realm prototype install, so no host has a line to remember. */
void dom_rect_init(JSContext *ctx);
/* §3's two interface prototype objects for ONE realm, chained `DOMRect.prototype -> DOMRectReadOnly.prototype`
   the way the IDL's `interface DOMRect : DOMRectReadOnly` says. */
void dom_rect_install_protos(JSContext *ctx);
/* The two interface objects on one realm's global. */
void dom_rect_install(JSContext *ctx, JSValueConst global);
void dom_rect_free(void);

/* §3's DOMRect CONSTRUCTOR STEPS, reached from C — "let rect be a new DOMRect, set its four internal member
   variables, return rect". The rectangle is minted in `ctx`, which must be the RELEVANT REALM of whatever the
   member was invoked on: Web IDL creates a [NewObject] there, so `iframe.contentDocument.body`'s rectangle is
   an instance of the CHILD's DOMRect and not of the realm whose prototype the call was written through.
   IT TAKES DOUBLES because every value a browser algorithm puts in a rectangle is one it COMPUTED. A rectangle
   whose numbers came from the page can hold unknown external input and the record is built for that (see
   dom_rect.c), but that route is the page's own constructor and not this one — a browser algorithm handing a
   concolic here would be handing on an unknown it should have forked on. */
JSValue dom_rect_new(JSContext *ctx, double x, double y, double width, double height);

/* Is `v` a DOMRect or a DOMRectReadOnly — §4's DOMRectList asserts what it was handed, since its indexed getter
   and its `item` both declare `DOMRect?` as the type they answer. */
bool dom_rect_is(JSValueConst v);

#endif
