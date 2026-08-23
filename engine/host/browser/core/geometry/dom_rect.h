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
   IT TAKES DOUBLES because the value it is for is a DETERMINED one — CSSOM VIEW §6's own zero rectangle for an
   element that generates no box, which every user agent computes identically. Unknown external input never
   arrives here (that is the page's own constructor, and dom_rect.c's record is built for it); a viewport-
   DERIVED number does, and it takes the entry below instead. */
JSValue dom_rect_new(JSContext *ctx, double x, double y, double width, double height);

/* §3's SAME CONSTRUCTOR STEPS with the four members ALREADY MINTED — the entry a browser algorithm uses when
   its numbers are a function of an ENVIRONMENT FACT rather than determined. A box sized by CSS 2 §10.3.3's
   constraint equation is derived from the initial containing block, whose dimensions core/frame/viewport.h
   models as a PICKED choice, so `rect.width < 768` is the same responsive gate `innerWidth < 768` is — and a
   `double` could carry the example and not the domain, deleting the mobile arm with nothing to say so. The
   caller mints each value through viewport.h's one seam, which is the only place a used length crosses to a
   page. CONSUMES the four values; each must be a Number or unknown external input. */
JSValue dom_rect_new_values(JSContext *ctx, JSValue x, JSValue y, JSValue width, JSValue height);

/* THE SAME STEPS PRODUCING A `DOMRectReadOnly` — the BASE interface, which is a different interface and not a
   flag on this one: it has no setters at all, so a rectangle handed out as one cannot be written back by the
   page. WHICH of the two an algorithm returns is stated by the IDL of the member returning it, and the two
   entries above answer `DOMRect` because their callers' IDLs say so (CSSOM VIEW §6's `getBoundingClientRect`
   and `getClientRects`). Intersection Observer §2.3 declares all three of an entry's rectangles
   `DOMRectReadOnly`, which is what this entry is for — a page holding an entry must not be able to rewrite the
   geometry the engine reported, and returning the mutable interface would also answer
   `entry.boundingClientRect instanceof DOMRect` true where every browser answers false.
   CONSUMES the four values on the same terms as the entry above. */
JSValue dom_rect_readonly_new_values(JSContext *ctx, JSValue x, JSValue y, JSValue width, JSValue height);

/* Is `v` a DOMRect or a DOMRectReadOnly — §4's DOMRectList asserts what it was handed, since its indexed getter
   and its `item` both declare `DOMRect?` as the type they answer. */
bool dom_rect_is(JSValueConst v);

#endif
