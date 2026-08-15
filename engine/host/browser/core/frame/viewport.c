/* THE VIEWPORT — CSS 2.1 §9.1.1 / §10.1, and CSSOM VIEW §4's Window extensions over it. See viewport.h for why
   this is a modelled UA choice, why it is a component of its own, why it is answered per realm, and why §4's
   members are installed here rather than in window.c. */
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/frame/screen.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/realm.h"

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

/* THE CLIENT WINDOW — the browser window the top-level traversable is presented in, which is what CSSOM VIEW
   §4's `outerWidth`, `outerHeight`, `screenX` and `screenY` are all about. It is NOT the viewport: the viewport
   is the content area INSIDE it, and the difference is the UA's own chrome (a tab strip and a toolbar). A
   bundle that computes `outerHeight - innerHeight` is measuring exactly that, so the two must be able to
   differ; a client window equal to its viewport is the shrug this component exists not to make.
   THE SIZE IS DERIVED FROM THE VIEWPORT, in that direction, because the viewport is the load-bearing fact this
   file already states. There is no horizontal chrome on a desktop browser window, so only the height grows.
   IT IS THE SAME WINDOW FOR EVERY REALM OF THE PAGE. A child navigable has its own viewport and does NOT have
   its own client window — `iframe.contentWindow.screenX` is the position of the browser window, which is why
   these four take a realm only to be asked whether the document is being presented at all. */
#define VIEWPORT_CHROME_HEIGHT 74.0

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

bool viewport_exists(JSContext *ctx)
{
    return document_fully_active(ctx);
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

/* CSSOM VIEW §4: "the x-coordinate, RELATIVE TO THE INITIAL CONTAINING BLOCK ORIGIN, of the left of the
 * viewport". So the question is where the viewport sits over its own scrolling area, and this engine can
 * COMPUTE that rather than guess it.
 *
 * §2 defines a VIEWPORT's scrolling area as the initial containing block extended by the margin edges of all of
 * the viewport's DESCENDANTS' BOXES. This engine generates no boxes — there is no layout — so the extension is
 * empty and the scrolling area is exactly the ICB; the ICB is exactly the viewport; and a scrolling box whose
 * scrolling area is its own size has one valid scroll position, its origin. The left of the viewport therefore
 * IS the ICB origin, for every flow at every moment, and 0 is a derived value rather than an absent one.
 *
 * THE MOMENT THERE IS A LAYOUT this stops being a derivation: the scrolling area becomes the union above, the
 * position becomes real state, and this becomes the read of what §3's perform-a-scroll wrote. The two-sided
 * assertion for that already exists and is in the right place — update-the-rendering step 9 (CSSOM VIEW §13.2's
 * SCROLL STEPS) is asserted against `scrollTo`, the member whose arrival means a scrolling box can be moved at
 * all, so the step that would have to drain doc's pending scroll events names itself first. */
double viewport_scroll_x(JSContext *ctx)
{
    (void)ctx;
    return 0.0;
}

double viewport_scroll_y(JSContext *ctx)
{
    (void)ctx;
    return 0.0;
}

/* The client window's size, in CSS pixels, and its position relative to §2.3's Web-exposed screen area origin.
   The position is DERIVED and not a second UA choice: an ordinary desktop window sits in the middle of the
   space the operating system leaves for it, and screen.c already states what that space is. */
static double viewport_client_width(void)  { return VIEWPORT_TOP_WIDTH; }
static double viewport_client_height(void) { return VIEWPORT_TOP_HEIGHT + VIEWPORT_CHROME_HEIGHT; }

static double viewport_client_screen_x(void)
{
    DCHECK(viewport_client_width() <= screen_avail_width(),
           "the modelled client window is wider than the Web-exposed available screen area it is positioned "
           "inside — a window a UA could not open, and `screenX` would come out negative");
    return (double)(int)((screen_avail_width() - viewport_client_width()) / 2.0);
}

static double viewport_client_screen_y(void)
{
    DCHECK(viewport_client_height() <= screen_avail_height(),
           "the modelled client window is taller than the Web-exposed available screen area it is positioned "
           "inside — a window a UA could not open, and `screenY` would come out negative");
    return (double)(int)((screen_avail_height() - viewport_client_height()) / 2.0);
}

/* ---- CSSOM VIEW §4's Window extensions ------------------------------------------------------------------- */

/* THE MEMBERS, AS ONE LIST, and the aliases are IN it rather than being a second install: §4 states
   `pageXOffset` as "the value returned by the scrollX attribute" and `screenLeft` as `screenX`'s, so the two
   names share one magic and there is no second body that could ever answer differently. Every one of them is
   [Replaceable] readonly — assigning replaces the accessor with an ordinary data property, which is what
   idl_install_replaceable performs. */
typedef enum {
    VP_INNER_W, VP_INNER_H, VP_OUTER_W, VP_OUTER_H,
    VP_SCROLL_X, VP_SCROLL_Y, VP_SCREEN_X, VP_SCREEN_Y, VP_DPPX
} ViewportMember;

#define VIEWPORT_WINDOW_MEMBERS(X)   \
    X("innerWidth",       VP_INNER_W)  \
    X("innerHeight",      VP_INNER_H)  \
    X("outerWidth",       VP_OUTER_W)  \
    X("outerHeight",      VP_OUTER_H)  \
    X("scrollX",          VP_SCROLL_X) \
    X("pageXOffset",      VP_SCROLL_X) \
    X("scrollY",          VP_SCROLL_Y) \
    X("pageYOffset",      VP_SCROLL_Y) \
    X("screenX",          VP_SCREEN_X) \
    X("screenLeft",       VP_SCREEN_X) \
    X("screenY",          VP_SCREEN_Y) \
    X("screenTop",        VP_SCREEN_Y) \
    X("devicePixelRatio", VP_DPPX)

/* The names are string LITERALS so engine/idlgen.mjs's install audit can see them — a name built by
   concatenation reads as absent to the auditor, which is the audit lying by omission. */
static const char *const VP_NAME[] = {
#define X(n, m) n,
    VIEWPORT_WINDOW_MEMBERS(X)
#undef X
};
static const int VP_MAGIC[] = {
#define X(n, m) (int)(m),
    VIEWPORT_WINDOW_MEMBERS(X)
#undef X
};
#define VP_NAMES ((int)(sizeof(VP_NAME) / sizeof(VP_NAME[0])))

/* A member whose IDL type is `long`. Every length this component models is a whole number of CSS pixels, so a
   fraction here is a derivation that produced something the type cannot carry rather than a value to round. */
static JSValue vp_long(JSContext *ctx, double v)
{
    DCHECK(v == (double)(int32_t)v,
           "a CSSOM VIEW §4 Window member declared `long` computed a value that is not an integer");
    return JS_NewInt32(ctx, (int32_t)v);
}

static JSValue js_vp_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    /* §4 conditions six of these on there being a viewport or a client window, and the two absences are the
       same one in this engine: both are what a NAVIGABLE presents, and a document that is not fully active is
       presented by nothing. `devicePixelRatio` is NOT one of them — its algorithm asks about the OUTPUT DEVICE,
       which exists whether or not this document is on it. */
    bool presented = viewport_exists(ctx);

    (void)this_val;
    switch ((ViewportMember)magic) {
    /* "The innerWidth attribute must return the viewport width INCLUDING the size of a rendered scroll bar (if
       any), or zero if there is no viewport." Including the scrollbar is why no scrollbar question arises: the
       answer is the viewport, whether or not one is rendered. */
    case VP_INNER_W:  return vp_long(ctx, presented ? viewport_width(ctx) : 0.0);
    case VP_INNER_H:  return vp_long(ctx, presented ? viewport_height(ctx) : 0.0);
    /* "The outerWidth attribute must return the width of the client window. If there is no client window this
       attribute must return zero." */
    case VP_OUTER_W:  return vp_long(ctx, presented ? viewport_client_width() : 0.0);
    case VP_OUTER_H:  return vp_long(ctx, presented ? viewport_client_height() : 0.0);
    /* "The scrollX attribute must return the x-coordinate, relative to the initial containing block origin, of
       the left of the viewport, or zero if there is no viewport." An `unrestricted double`, not a `long`. */
    case VP_SCROLL_X: return JS_NewFloat64(ctx, presented ? viewport_scroll_x(ctx) : 0.0);
    case VP_SCROLL_Y: return JS_NewFloat64(ctx, presented ? viewport_scroll_y(ctx) : 0.0);
    /* "The screenX and screenLeft attributes must return the x-coordinate, relative to the origin of the
       Web-exposed screen area, of the left of the client window as number of CSS pixels, or zero if there is
       no such thing." */
    case VP_SCREEN_X: return vp_long(ctx, presented ? viewport_client_screen_x() : 0.0);
    case VP_SCREEN_Y: return vp_long(ctx, presented ? viewport_client_screen_y() : 0.0);
    /* §4's DETERMINE THE DEVICE PIXEL RATIO: with no output device return 1; otherwise the CSS pixel size
       divided by the device pixel size. This engine models an output device (screen.c), so it is the ratio. */
    case VP_DPPX:     return JS_NewFloat64(ctx, viewport_device_pixel_ratio(ctx));
    }
    DFAIL("a CSSOM VIEW §4 Window member was read with a magic no member of this file declares — the magic IS "
          "the member, so an unknown one means a name was installed without a case to answer it");
    return JS_UNDEFINED;
}

/* ---- CSSOM VIEW §13.1's resize steps, this component's half ----------------------------------------------- */

/* The record's three fields ARE the spec sentence: what the viewport measured the last time the steps ran, and
   whether there HAS been a last time. See viewport.h for why the second one cannot be replaced by seeding the
   first at install. */
#define VP_RESIZE_RAN "hasBeenRun"
#define VP_RESIZE_W   "width"
#define VP_RESIZE_H   "height"

static int g_resize_slot = -1;

bool viewport_resize_changed(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_resize_slot);
    JSValue ran = JS_GetPropertyStr(ctx, rec, VP_RESIZE_RAN);
    double w = viewport_width(ctx), h = viewport_height(ctx);
    bool had_run, changed;

    DCHECK(JS_IsBool(ran),
           "the §13.1 resize-steps record's `has been run` field is not a boolean — nothing but these steps "
           "ever writes this record, and they write exactly the three fields the algorithm names");
    had_run = JS_ToBool(ctx, ran);
    JS_FreeValue(ctx, ran);

    changed = false;
    if (had_run) {
        JSValue lw = JS_GetPropertyStr(ctx, rec, VP_RESIZE_W);
        JSValue lh = JS_GetPropertyStr(ctx, rec, VP_RESIZE_H);
        double dw = 0.0, dh = 0.0;

        DCHECK(JS_IsNumber(lw) && JS_IsNumber(lh),
               "the §13.1 resize-steps record holds a dimension that is not a number — a run that latched one "
               "latched both, so this is a write from outside the algorithm");
        JS_ToFloat64(ctx, &dw, lw);
        JS_ToFloat64(ctx, &dh, lh);
        JS_FreeValue(ctx, lw);
        JS_FreeValue(ctx, lh);
        changed = dw != w || dh != h;
    }
    /* WRITTEN ONLY WHEN IT MOVES, because each write is captured into the running flow's COW delta and a frame
       that re-latches an unchanged pair would grow every flow's delta once per rendering opportunity for
       nothing. Re-writing the same numbers is not observable, so not writing them is the same algorithm. */
    if (changed || !had_run) {
        JS_SetPropertyStr(ctx, rec, VP_RESIZE_W, JS_NewFloat64(ctx, w));
        JS_SetPropertyStr(ctx, rec, VP_RESIZE_H, JS_NewFloat64(ctx, h));
        if (!had_run)
            JS_SetPropertyStr(ctx, rec, VP_RESIZE_RAN, JS_TRUE);
    }
    JS_FreeValue(ctx, rec);
    return changed;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

static void viewport_install(JSContext *ctx)
{
    JSValue global, rec;
    int i;

    /* §13.1's record, built WITH the realm — running twice in one realm is asserted by realm_value_set, which
       is where the first record is standing. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "viewport: OOM building a realm's CSSOM VIEW §13.1 resize-steps record");
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_RAN, JS_FALSE);
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_W, JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_H, JS_NewFloat64(ctx, 0.0));
    realm_value_set(ctx, g_resize_slot, rec);

    /* Web IDL §3.7.3: Window is [Global], so its members are own properties of the GLOBAL OBJECT rather than of
       Window.prototype — see window.c, which states the same rule for the browsing-context half. */
    global = JS_GetGlobalObject(ctx);
    for (i = 0; i < VP_NAMES; i++)
        idl_install_replaceable(ctx, global, VP_NAME[i], js_vp_get, VP_MAGIC[i]);
    JS_FreeValue(ctx, global);
}

void viewport_init(JSContext *ctx)
{
    DCHECK(g_resize_slot < 0, "viewport_init ran twice — the §13.1 record's slot is declared once per AGENT");
    g_resize_slot = realm_value_declare(ctx, "CSSOM VIEW §13.1 the viewport as the resize steps last saw it");
    realm_declare_intrinsic(viewport_install);
}

void viewport_free(void)
{
    /* The records are the REALMS' — each is released with its context. What the agent holds is the slot, and a
       slot id is a class id in a runtime that is going away with it. */
    g_resize_slot = -1;
}
