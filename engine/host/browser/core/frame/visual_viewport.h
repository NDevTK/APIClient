/* THE VISUAL VIEWPORT — CSSOM VIEW §12, and its half of §13.1's resize steps.
 *
 * §2 defines it as "a kind of viewport whose scrolling area is ANOTHER viewport, called the LAYOUT VIEWPORT",
 * which may additionally apply a SCALE TRANSFORM to it. That is the whole of the interface's subject matter:
 * pinch-zoom, and where the zoomed-in window sits over the page. So this component owns exactly one fact of its
 * own — the SCALE FACTOR — and every other member is derived from it and from viewport.c's layout viewport.
 * A second copy of the geometry here would be the defect viewport.h names.
 *
 * IT IS NOT A STUB, and the distinction matters because a shape-only `visualViewport` bag is precisely what
 * §NO STUBS bans. Every one of §12's seven attributes has a REAL computed answer in this model: the scale
 * factor is 1 because a scale factor is changed by a user gesture (or by the UA magnifying a focused input) and
 * neither has happened; at scale 1 the visual viewport COVERS the layout viewport, so its offsets from it are
 * zero and its size IS the layout viewport's; and `pageLeft`/`pageTop` are those offsets plus the layout
 * viewport's own scroll position, which viewport.c derives. Change the scale factor and every one of them moves,
 * which is what tells a derivation from a placeholder.
 *
 * ITS EVENTS. §12 makes it an EventTarget with `onresize`, `onscroll` and `onscrollend`. The resize event has a
 * producer — §13.1 step 2, written in rendering.c, which asks `visual_viewport_resize_changed` — and the two
 * scroll events do not: §13.2 fires them when a visual viewport GETS SCROLLED, and nothing in this engine can
 * scroll one while the scrolling area is the same size as the viewport (viewport.c derives why). The handler
 * attributes exist because §12 declares them and a page reads their presence; they simply have nothing to fire
 * until there is a layout.
 *
 * PER REALM, for the reason everything in this directory is: `frames[0].visualViewport.width` is 300 and the
 * top-level one is 1280, and a member installed once would answer both out of whichever realm built it. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_VISUAL_VIEWPORT_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_VISUAL_VIEWPORT_H

#include <stdbool.h>
#include "quickjs.h"

/* Declared once per AGENT: the class that is both the per-realm prototype slot and the brand, the two per-realm
   value slots, and the per-realm install. */
void visual_viewport_init(JSContext *ctx);
void visual_viewport_free(void);

/* §4's `visualViewport` answer for THIS realm: "If the associated document is fully active, the VisualViewport
   object associated with the Window object's associated document. Otherwise, null."
   OWNED — the caller frees. It is the SAME object on every call, which is what [SameObject] states. */
JSValue visual_viewport_object(JSContext *ctx);

/* CSSOM VIEW §13.1 step 2: "If the VisualViewport associated with doc has had its SCALE, WIDTH or HEIGHT
   properties changed since the last time these steps were run, fire an event named resize at the
   VisualViewport." LATCHES what it saw, on the same per-realm-record-of-property-writes terms as
   viewport_resize_changed — see viewport.h for why the first run in a flow is never a change. */
bool visual_viewport_resize_changed(JSContext *ctx);

#endif
