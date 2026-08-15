/* THE VISUAL VIEWPORT — CSSOM VIEW §12. See visual_viewport.h for what this component owns (one fact: the
   scale factor), why every other member is a derivation over viewport.c's layout viewport, and why it is per
   realm. */
#include "check.h"
#include "quickjs.h"
#include "core/events/event_target.h"
#include "core/frame/viewport.h"
#include "core/frame/visual_viewport.h"
#include "core/idl_args.h"
#include "core/realm.h"

/* §2.2's SCALE FACTOR — the magnitude of the transform the visual viewport applies to the layout viewport, the
   thing usually called pinch-zoom. It is 1 because it is changed by a user gesture, or by the user agent
   magnifying a focused input element to make it legible, and this engine has modelled neither. That is a state,
   not an absence: the moment a gesture exists, this is what it writes, and every member below moves with it. */
#define VISUAL_VIEWPORT_SCALE 1.0

static JSClassID g_vv_class;
static int g_obj_slot = -1;      /* §2's "the VisualViewport object associated with the document" */
static int g_resize_slot = -1;   /* §13.1 step 2's "since the last time these steps were run" */

/* ---- §12's attributes ------------------------------------------------------------------------------------ */

typedef enum {
    VV_OFFSET_LEFT, VV_OFFSET_TOP, VV_PAGE_LEFT, VV_PAGE_TOP, VV_WIDTH, VV_HEIGHT, VV_SCALE
} VisualViewportMember;

/* The names are string LITERALS so engine/idlgen.mjs's install audit can see them. */
#define VISUAL_VIEWPORT_MEMBERS(X)  \
    X("offsetLeft", VV_OFFSET_LEFT) \
    X("offsetTop",  VV_OFFSET_TOP)  \
    X("pageLeft",   VV_PAGE_LEFT)   \
    X("pageTop",    VV_PAGE_TOP)    \
    X("width",      VV_WIDTH)       \
    X("height",     VV_HEIGHT)      \
    X("scale",      VV_SCALE)

static const char *const VV_NAME[] = {
#define X(n, m) n,
    VISUAL_VIEWPORT_MEMBERS(X)
#undef X
};
static const int VV_MAGIC[] = {
#define X(n, m) (int)(m),
    VISUAL_VIEWPORT_MEMBERS(X)
#undef X
};
#define VV_NAMES ((int)(sizeof(VV_NAME) / sizeof(VV_NAME[0])))

/* WEB IDL §3.7.5's BRAND CHECK — `VisualViewport.prototype.width` read off a plain object is a TypeError, and a
   page tells that apart from `undefined`: a feature detector that pulls the descriptor and applies the getter
   reads the throw as "this is a real interface". A real throw, not an assert, for exactly that reason. */
static bool vv_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_vv_class != 0, "a VisualViewport member ran before visual_viewport_init declared the class — the "
                            "member is only reachable through a prototype the per-realm install builds");
    if (JS_GetClassID(this_val) == g_vv_class) return true;
    JS_ThrowTypeError(ctx, "a VisualViewport member was reached on something that is not a VisualViewport");
    return false;
}

/* THE HALF OF "THIS's ASSOCIATED DOCUMENT" THIS ENGINE CAN ANSWER, asserted rather than assumed — the same
   shape and the same reason as navigator.c's. A C member runs in the realm that DEFINED it, so an ordinary
   `visualViewport.width` arrives with the ctx of the document whose prototype it went through, which is the
   right document. What does NOT arrive right is one realm's getter applied to ANOTHER realm's VisualViewport:
   the geometry would come out of the getter's realm, so an iframe's object read through the top-level realm's
   prototype would report 1280 where §12 says 300. */
static void vv_assert_this_realm(JSContext *ctx, JSValueConst this_val)
{
    JSValue own = realm_value_get(ctx, g_obj_slot);
    bool same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, own);
    DCHECK(same, "a VisualViewport member was reached through ONE realm's VisualViewport.prototype on ANOTHER "
                 "realm's VisualViewport — the geometry would be answered for the member's own document. "
                 "BUILD the VisualViewport that carries its own realm: give the instance a record as its class "
                 "opaque (with the finalizer, gc_mark and cow_capture_host_record contract that entails) so "
                 "the member reads its document off THIS, and delete the object slot below");
}

/* §12's `offsetLeft`/`offsetTop`: "the offset of the left edge of the visual viewport from the left edge of the
   LAYOUT viewport". At scale 1 the visual viewport covers the layout viewport exactly, so there is no offset to
   have — which is a fact about the scale factor, and the assert says so at the one place that would go wrong. */
static double vv_offset(void)
{
    DCHECK(VISUAL_VIEWPORT_SCALE == 1.0,
           "the modelled visual viewport is scaled, so it no longer covers the layout viewport and its offsets "
           "from it are real numbers — BUILD them here from the scale factor and the pan position that changed "
           "it, rather than leaving this answering zero");
    return 0.0;
}

/* §12's value for one member, in CSS pixels. Every attribute opens with "if the visual viewport's associated
   document is not fully active, return 0", which viewport.c answers as "there is no viewport". */
static double vv_value(JSContext *ctx, VisualViewportMember m)
{
    if (!viewport_exists(ctx)) return 0.0;
    switch (m) {
    case VV_OFFSET_LEFT:
    case VV_OFFSET_TOP: return vv_offset();
    /* "the offset of the left edge of the visual viewport from the left edge of the INITIAL CONTAINING BLOCK of
       the layout viewport's document" — which is the offset above measured from the layout viewport's own
       position over the ICB, i.e. its scroll position. viewport.c owns that; it is not recomputed here. */
    case VV_PAGE_LEFT:  return vv_offset() + viewport_scroll_x(ctx);
    case VV_PAGE_TOP:   return vv_offset() + viewport_scroll_y(ctx);
    /* "the width of the visual viewport EXCLUDING the width of any rendered vertical classic scrollbar that is
       fixed to the visual viewport" — none is rendered, so it is the visual viewport's width, which at scale 1
       is the layout viewport's. Note that this is where it differs from `innerWidth`, which §4 defines as
       INCLUDING the scrollbar: the two are equal here because there is no scrollbar, not because they are the
       same quantity. */
    case VV_WIDTH:      return viewport_width(ctx) / VISUAL_VIEWPORT_SCALE;
    case VV_HEIGHT:     return viewport_height(ctx) / VISUAL_VIEWPORT_SCALE;
    /* "If there is no output device, return 1. Otherwise, return the visual viewport's scale factor." This
       engine models an output device — screen.c — so it is the factor. */
    case VV_SCALE:      return VISUAL_VIEWPORT_SCALE;
    }
    DFAIL("a VisualViewport member was read with a magic no member of this file declares — the magic IS the "
          "member, so an unknown one means a name was installed without a case to answer it");
    return 0.0;
}

static JSValue js_vv_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    if (!vv_brand(ctx, this_val)) return JS_EXCEPTION;
    vv_assert_this_realm(ctx, this_val);
    return JS_NewFloat64(ctx, vv_value(ctx, (VisualViewportMember)magic));
}

JSValue visual_viewport_object(JSContext *ctx)
{
    /* §4: "If the associated document is fully active, the visualViewport attribute must return the
       VisualViewport object ... Otherwise, it must return null." The note beside it is why this is a real
       branch and not a formality: a reference retained to a VisualViewport whose document is no longer being
       presented must reveal nothing about the browsing context, which is what every member's own
       not-fully-active zero already enforces — this stops the object being reachable at all. */
    if (!viewport_exists(ctx)) return JS_NULL;
    return realm_value_get(ctx, g_obj_slot);
}

static JSValue js_win_visual_viewport(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return visual_viewport_object(ctx);
}

/* ---- CSSOM VIEW §13.1 step 2 ------------------------------------------------------------------------------ */

/* The record's four fields ARE the spec sentence — the three properties §13.1 step 2 names, and whether the
   steps have run at all. See viewport.h for why the second cannot be replaced by seeding the first three. */
#define VV_RESIZE_RAN   "hasBeenRun"
#define VV_RESIZE_SCALE "scale"
#define VV_RESIZE_W     "width"
#define VV_RESIZE_H     "height"

static double vv_latched(JSContext *ctx, JSValueConst rec, const char *field)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, field);
    double d = 0.0;

    DCHECK(JS_IsNumber(v), "the §13.1 step 2 record holds a property that is not a number — a run that latched "
                           "one latched all three, so this is a write from outside the algorithm");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

bool visual_viewport_resize_changed(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_resize_slot);
    JSValue ran = JS_GetPropertyStr(ctx, rec, VV_RESIZE_RAN);
    double s = vv_value(ctx, VV_SCALE), w = vv_value(ctx, VV_WIDTH), h = vv_value(ctx, VV_HEIGHT);
    bool had_run, changed;

    DCHECK(JS_IsBool(ran),
           "the §13.1 step 2 record's `has been run` field is not a boolean — nothing but these steps ever "
           "writes this record, and they write exactly the four fields the algorithm names");
    had_run = JS_ToBool(ctx, ran);
    JS_FreeValue(ctx, ran);

    changed = had_run
        && (vv_latched(ctx, rec, VV_RESIZE_SCALE) != s
            || vv_latched(ctx, rec, VV_RESIZE_W) != w
            || vv_latched(ctx, rec, VV_RESIZE_H) != h);
    /* Written only when it moves — each write is captured into the running flow's COW delta, and re-latching an
       unchanged triple once per rendering opportunity would grow every flow's delta for nothing. */
    if (changed || !had_run) {
        JS_SetPropertyStr(ctx, rec, VV_RESIZE_SCALE, JS_NewFloat64(ctx, s));
        JS_SetPropertyStr(ctx, rec, VV_RESIZE_W, JS_NewFloat64(ctx, w));
        JS_SetPropertyStr(ctx, rec, VV_RESIZE_H, JS_NewFloat64(ctx, h));
        if (!had_run)
            JS_SetPropertyStr(ctx, rec, VV_RESIZE_RAN, JS_TRUE);
    }
    JS_FreeValue(ctx, rec);
    return changed;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

/* ONE PROTOTYPE, ONE INTERFACE OBJECT, ONE VisualViewport AND ONE §13.1 RECORD PER REALM, built WITH the realm.
   §2 gives every Window's document a VisualViewport and §3.7 gives every realm its own interface prototype
   object — and here that decides ANSWERS and not just identities, because the geometry a member reports comes
   out of the realm the member was DEFINED in. */
static void visual_viewport_install(JSContext *ctx)
{
    JSValue proto, prev, global, obj, rec;
    int i;

    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "visual viewport: OOM building a realm's §13.1 step 2 record");
    JS_SetPropertyStr(ctx, rec, VV_RESIZE_RAN, JS_FALSE);
    JS_SetPropertyStr(ctx, rec, VV_RESIZE_SCALE, JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, rec, VV_RESIZE_W, JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, rec, VV_RESIZE_H, JS_NewFloat64(ctx, 0.0));
    realm_value_set(ctx, g_resize_slot, rec);

    prev = JS_GetClassProto(ctx, g_vv_class);
    DCHECK(JS_IsNull(prev), "visual_viewport_install ran twice in one realm — everything already holding the "
                            "first VisualViewport.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "VisualViewport.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "VisualViewport");
    /* `interface VisualViewport : EventTarget` — a real prototype chain, so `addEventListener` on the object is
       §2.7's and not a second listener list. */
    event_target_chain(ctx, proto);
    for (i = 0; i < VV_NAMES; i++)
        idl_install_accessor(ctx, proto, VV_NAME[i], js_vv_get, VV_MAGIC[i], -1);
    /* §12's three event handler IDL attributes, declared ON this interface — the mixin bit is what says so. */
    event_target_install_handlers(ctx, proto, EH_VISUAL_VIEWPORT);
    JS_SetClassProto(ctx, g_vv_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. VisualViewport declares no constructor, so
       `new VisualViewport()` is a TypeError — and its PRESENCE is what a feature-detecting bundle reads before
       it touches `window.visualViewport` at all. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "VisualViewport", idl_interface_object(ctx, "VisualViewport", proto));

    obj = JS_NewObjectProtoClass(ctx, proto, g_vv_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the document's associated VisualViewport could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);

    /* §4: `[SameObject, Replaceable] readonly attribute VisualViewport? visualViewport`. */
    idl_install_replaceable(ctx, global, "visualViewport", js_win_visual_viewport, 0);
    JS_FreeValue(ctx, global);
}

void visual_viewport_init(JSContext *ctx)
{
    JSClassDef d = { "VisualViewport" };

    DCHECK(g_obj_slot < 0, "visual_viewport_init ran twice — the class and the slots are declared once per "
                           "AGENT");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the one object per realm WEARS it, so
       §3.7.5's check is a class-id comparison and a page cannot forge one. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_vv_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_vv_class, &d) == 0,
          "VisualViewport: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "CSSOM VIEW §2 the document's associated VisualViewport");
    g_resize_slot = realm_value_declare(ctx, "CSSOM VIEW §13.1 the VisualViewport as the resize steps last saw "
                                             "it");
    realm_declare_intrinsic(visual_viewport_install);
}

void visual_viewport_free(void)
{
    /* The prototypes, the interface objects, the VisualViewports and their records are the REALMS' — each is
       released with its context. What the agent holds is the two slots and a class id, and both are
       registrations in a runtime that is going away with them. */
    g_vv_class = 0;
    g_obj_slot = -1;
    g_resize_slot = -1;
}
