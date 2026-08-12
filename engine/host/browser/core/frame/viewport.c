/* THE VIEWPORT — CSS 2.1 §9.1.1 / §10.1. See viewport.h for why this is a modelled UA choice, why it is a
   component of its own, and why it is answered per realm. */
#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"

/* THE TOP-LEVEL TRAVERSABLE's viewport. A UA answering the question CSS hands it, exactly as rendering.c
   answers the refresh rate: 1280 x 720 is an ordinary desktop content area, and it is deliberately NOT
   screen.c's 1920 x 1080 — a window is not a screen, and a bundle that compares `innerWidth` against
   `screen.width` is asking a real question whose two sides must be able to differ. */
#define VIEWPORT_TOP_WIDTH   1280.0
#define VIEWPORT_TOP_HEIGHT   720.0

/* A CHILD NAVIGABLE's viewport is its container's CONTENT BOX, and for an `iframe` with no author size that
   box is CSS 2.1 §10.3.2's default for a replaced element with no intrinsic dimensions: 300 x 150. That is the
   spec's own number rather than a second UA choice, which is why it is stated here as a derivation and not as
   a preference.
   AN AUTHOR STYLE THAT RESIZES THE FRAME IS NOT MODELLED, and that is a layout gap rather than a viewport one:
   `iframe { width: 500px }` is a cascade this engine has no CSS parser for, so the used width it produces does
   not exist to be read. The moment there is a layout, this size comes from the container element's used
   content box and this constant goes with it. */
#define VIEWPORT_CHILD_WIDTH  300.0
#define VIEWPORT_CHILD_HEIGHT 150.0

/* A modelled non-HiDPI display. See viewport.h. */
#define VIEWPORT_DPPX           1.0

/* IS THIS REALM's document IN the top-level traversable? Asked of the NAVIGABLE tree rather than remembered at
   install, because a realm is built before it is attached to anything and the answer is a fact about where the
   document sits. `window_proxy_top_navigable` answers the top-level traversable's proxy in every case
   INCLUDING the caller's own (window_proxy.h's note: `top` would answer the Window there, and a walk handed
   that reaches something that is not a proxy). */
static bool viewport_is_top(JSContext *ctx)
{
    JSValueConst self = document_window_proxy(ctx);
    JSValue top;
    bool is_top;

    DCHECK(window_proxy_is(self),
           "the viewport was asked for a realm whose document has no WindowProxy — every Document this agent "
           "holds is the active document of a navigable, and the navigable is what a viewport belongs to");
    top = window_proxy_top_navigable(ctx, self);
    DCHECK(window_proxy_is(top),
           "a navigable answered its top-level traversable with something that is not a WindowProxy");
    is_top = window_proxy_doc(top) == window_proxy_doc(self);
    JS_FreeValue(ctx, top);
    return is_top;
}

double viewport_width(JSContext *ctx)
{
    return viewport_is_top(ctx) ? VIEWPORT_TOP_WIDTH : VIEWPORT_CHILD_WIDTH;
}

double viewport_height(JSContext *ctx)
{
    return viewport_is_top(ctx) ? VIEWPORT_TOP_HEIGHT : VIEWPORT_CHILD_HEIGHT;
}

double viewport_device_pixel_ratio(JSContext *ctx)
{
    (void)ctx;
    return VIEWPORT_DPPX;
}
