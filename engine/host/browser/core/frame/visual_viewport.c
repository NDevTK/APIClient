/* THE VISUAL VIEWPORT — CSSOM VIEW §12. See visual_viewport.h for what this component owns (one fact: the
   scale factor), why every other member is a derivation over viewport.c's layout viewport, and why it is per
   realm. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/cow.h"        /* the instance's record is a component's own C state — it time-travels */
#include "core/agent_state.h"
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

/* THE RECORD — the document §12's members are about, carried by the VisualViewport rather than looked up from
 * the realm the getter was DEFINED in.
 *
 * WHY. §12's members report "this's associated document"'s visual viewport. A C member runs in the realm that
 * DEFINED it (js_call_c_function sets `ctx = p->u.cfunc.realm`), so a getter reading the geometry off `ctx`
 * answered for whichever realm's prototype the call went through — and the geometry genuinely differs: a child
 * navigable's viewport is 300 CSS pixels wide and the top-level traversable's is 1280.
 *
 * AND THE DAMAGE IS NOT ONLY A WRONG NUMBER. core/frame/viewport.h's `viewport_env_value` states that THE
 * DOCUMENT IS PART OF THE KEY, "so `innerWidth` is a different question in each and one key would let a branch
 * taken in the parent decide the iframe's" — so a cross-realm read minted the iframe's value under the PARENT's
 * key, and a flow that pinned one would prune an arm of the other that nothing had contradicted. That is the
 * wrong-narrowing failure CLAUDE.md §Solver-half forbids, reached through a member rather than through a gate.
 * (core/frame/screen.c is deliberately the opposite and both are right: a UA presents every document of a page
 * on ONE screen, so a Screen member IS its own source and must NOT be keyed per document.)
 *
 * THE ASSERT THAT STOOD HERE WAS A PAGE-HELD ABORT SWITCH: it compared the receiver against this realm's own
 * VisualViewport and DCHECKed them equal, and a receiver is PAGE-SUPPLIED INPUT, which a DCHECK may never
 * stand on. `Object.getOwnPropertyDescriptor(VisualViewport.prototype, "width").get
 * .call(otherFrame.visualViewport)` is two lines of ordinary JavaScript and ended the process. `vv_brand`
 * answers Web IDL §3.7.6's question one line above every caller and is the whole of what §3.7.6 asks.
 *
 * `global` IS WHAT MAKES `realm` SAFE, and it is a declared JSValue edge rather than a `JS_DupContext`: a
 * context reference hung off an opaque is invisible to gc_decref and would make the realm permanently
 * uncollectable, while a live global is a live realm. core/timing/performance.c states that reasoning at its
 * own record. It matters here more than most: §4 hands a page `null` once the document stops being fully
 * active, but a reference TAKEN EARLIER stays live, so this object outliving its presentation is the ordinary
 * case rather than the exotic one. */
typedef struct {
    JSContext *realm;    /* the document §12's members report. NOT a counted reference */
    JSValue    global;   /* "this's relevant global object" — OWNED, and what holds `realm` up */
} VisualViewportRec;

/* THE ONE STATEMENT OF WHAT THE RECORD OWNS — the same list the finalizer frees and the gc_mark walks. */
static const uint16_t VV_VAL_OFF[] = { (uint16_t)offsetof(VisualViewportRec, global) };
static const CowRecord VV_REC = { sizeof(VisualViewportRec), VV_VAL_OFF, 1 };
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

/* WEB IDL §3.7.6 Attributes' BRAND CHECK — `VisualViewport.prototype.width` read off a plain object is a
   TypeError, and a
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

/* THE ACCESSOR EVERY MEMBER REACHES THE RECORD THROUGH, and the capture is IN it for solver/cow.h's reason: a
   record a flow has REACHED is one it may write, the delta dedups to one entry per (flow, object), and there
   is then no write site left to miss. Bounded by §2's own shape — one VisualViewport per document.
   NOT vv_brand: a brand check is a QUESTION, asked of values that are not VisualViewports at all, and a
   question must not capture. */
static VisualViewportRec *vv_rec(JSValueConst v)
{
    VisualViewportRec *r = g_vv_class ? JS_GetOpaque(v, g_vv_class) : NULL;

    if (r) cow_capture_host_record(v, r, &VV_REC);
    return r;
}

/* JS_GetAnyOpaque and not JS_GetOpaque in BOTH — core/agent_state.h's rule, and this component's release
   column sets `g_vv_class` back to 0, so reading the static would make a finalizer running after it answer
   NULL for a record that is there. */
static void vv_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    VisualViewportRec *r = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(r != NULL, "a VisualViewport was finalized with no record — §2's object has exactly one mint and it "
                      "attaches the record with nothing in between that could collect");
    JS_FreeValueRT(rt, r->global);
    free(r);
}

static void vv_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    VisualViewportRec *r = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(r != NULL, "a VisualViewport was marked with no record — its global is a counted reference and an "
                      "unmarked child keeps the internal count gc_decref subtracts, so gc_scan reads it as "
                      "rooted from OUTSIDE the heap and it is never collected at all");
    JS_MarkValue(rt, r->global, mark_func);
}

/* "THIS's ASSOCIATED DOCUMENT" — the environment §12's members report, ANSWERED off the receiver rather than
   asserted about. Every assert here stands on a value THIS component wrote at the mint, which is the only
   thing a DCHECK may stand on; the receiver's own brand is a TypeError one line above every caller. */
static JSContext *vv_environment(JSValueConst this_val)
{
    VisualViewportRec *r = vv_rec(this_val);

    DCHECK(r != NULL, "a VisualViewport reached a member with no record — the brand is the class and the mint "
                      "attaches the record before the object leaves it, so a branded object without one came "
                      "from a second mint that does not exist");
    DCHECK(r->realm != NULL, "a VisualViewport names no document — §2's mint is the one writer of this field "
                             "and it writes the realm it is installing into");
    return r->realm;
}

/* §12's `offsetLeft`/`offsetTop`: "the offset of the left edge of the visual viewport from the left edge of the
   LAYOUT viewport". At scale 1 the visual viewport covers the layout viewport exactly, so there is no offset to
   have — which is a fact about the scale factor, and the assert says so at the one place that would go wrong.
   THE ASSERT READS THE MODELLED SCALE, which is a compile-time constant and therefore the EXAMPLE the `scale`
   member's concolic carries. That is deliberate and it is also the reason the four positions stay concrete:
   they are not a free parameter of their own, they are what the pinned scale forces — see visual_viewport.h. */
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

/* WHICH OF §12's SEVEN REPORT THE ENVIRONMENT AND WHICH ARE DERIVED FROM IT — viewport.h's test, applied here.
   The WIDTH, the HEIGHT and the SCALE are points this model picked out of a range the environment leaves free:
   a UA presents a page at whatever scale a gesture (or a `<meta name=viewport content="initial-scale=2">`)
   left it at, and
   `visualViewport.scale !== 1` is the zoom gate a bundle puts zoom-aware layout behind. They are SEPARATE
   sources from `innerWidth`/`innerHeight` for screen.c's reason — `visualViewport.width < innerWidth` is the
   "is the user zoomed in" question, and one shared source would make that branch answer the size branch.
   THE FOUR POSITIONS ARE NOT FREE. A pan exists only inside a scale that is not 1 (at scale 1 the visual
   viewport covers the layout viewport, so there is nowhere to pan to), and `pageLeft`/`pageTop` add the layout
   viewport's scroll position, which viewport.h derives to a single point. Their domain here is that single
   point, and the gesture that would widen it WRITES state — so they are per-flow state in the COW delta the day
   that gesture exists, never an environment source. */
static bool vv_is_source(VisualViewportMember m)
{
    switch (m) {
    case VV_WIDTH:
    case VV_HEIGHT:
    case VV_SCALE:
        return true;
    case VV_OFFSET_LEFT:
    case VV_OFFSET_TOP:
    case VV_PAGE_LEFT:
    case VV_PAGE_TOP:
        return false;
    }
    DFAIL("a VisualViewport member was classified with a magic no member of this file declares — the magic IS "
          "the member, so an unknown one means a name was installed without a case to answer it");
    return false;
}

static JSValue js_vv_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    VisualViewportMember m = (VisualViewportMember)magic;
    char member[64];
    JSContext *env;
    JSValue v;

    if (!vv_brand(ctx, this_val)) return JS_EXCEPTION;
    /* THE DOCUMENT IS THE RECEIVER'S AND `ctx` IS THE CALL'S — §12's members report "this's associated
       document"'s visual viewport, and `js_call_c_function` sets `ctx = p->u.cfunc.realm`. Every read below
       takes `env`, including the MINT: core/frame/viewport.h keys a viewport source BY THE DOCUMENT, so
       minting through `ctx` would file an iframe's value under the parent's key and let a branch taken in one
       decide the other. That split is what the deleted assert was standing in for. */
    env = vv_environment(this_val);
    v = JS_NewFloat64(ctx, vv_value(env, m));
    /* The not-fully-active zero every §12 attribute opens with is the SPEC's answer rather than a geometry this
       UA chose, so it stays concrete for the same reason viewport.c's does. */
    if (!vv_is_source(m) || !viewport_exists(env)) return v;
    DCHECK(magic >= 0 && magic < VV_NAMES && VV_MAGIC[magic] == magic,
           "the VisualViewport member list is no longer in enum order, so a member's source identity would "
           "name a different member and two attributes would share one branch");
    DCHECK(strlen(VV_NAME[magic]) + 16 < sizeof member,
           "a VisualViewport member name longer than any in the IDL — a truncated one would key two members' "
           "branches together");
    snprintf(member, sizeof member, "visualViewport.%s", VV_NAME[magic]);
    return viewport_env_value(env, member, v);
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
   steps have run at all. See viewport.h for why the second cannot be replaced by seeding the first three.
   IT LATCHES `vv_value`, the modelled geometry, and not what a flow decided about the members that report it —
   for viewport.c's reason: a flow that branched on `visualViewport.scale` did not zoom anything. The
   `JS_IsNumber` assert below fires if a concolic ever reaches the latch. */
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
    VisualViewportRec *r;
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

    /* `interface VisualViewport : EventTarget` — a real prototype chain, so `addEventListener` on the object is
       §2.7's and not a second listener list. */
    proto = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, proto, "VisualViewport");
    for (i = 0; i < VV_NAMES; i++)
        idl_install_accessor(ctx, proto, VV_NAME[i], js_vv_get, VV_MAGIC[i], -1);
    /* §12's three event handler IDL attributes, declared ON this interface — the mixin bit is what says so. */
    event_target_install_handlers(ctx, proto, EH_VISUAL_VIEWPORT);
    JS_SetClassProto(ctx, g_vv_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. VisualViewport declares no constructor, so
       `new VisualViewport()` is a TypeError — and its PRESENCE is what a feature-detecting bundle reads before
       it touches `window.visualViewport` at all. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "VisualViewport",
                                         idl_interface_object(ctx, "VisualViewport", proto));

    obj = JS_NewObjectProtoClass(ctx, proto, g_vv_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the document's associated VisualViewport could not be allocated");
    /* THE RECORD, ATTACHED BEFORE THE OBJECT LEAVES THIS FUNCTION — which is what vv_environment's "a branded
       object without one came from a second mint that does not exist" rests on, and there is no second mint. */
    r = calloc(1, sizeof *r);
    CHECK(r != NULL, "this document's VisualViewport record could not be allocated");
    r->realm = ctx;
    r->global = JS_DupValue(ctx, global);   /* OWNED by the record from here */
    JS_SetOpaque(obj, r);
    realm_value_set(ctx, g_obj_slot, obj);

    /* §4: `[SameObject, Replaceable] readonly attribute VisualViewport? visualViewport`. */
    idl_install_replaceable(ctx, global, "visualViewport", js_win_visual_viewport, 0);
    JS_FreeValue(ctx, global);
}

void visual_viewport_init(JSContext *ctx)
{
    JSClassDef d = { "VisualViewport", .finalizer = vv_finalizer, .gc_mark = vv_gc_mark };

    DCHECK(g_obj_slot < 0, "visual_viewport_init ran twice — the class and the slots are declared once per "
                           "AGENT");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the one object per realm WEARS it, so
       §3.7.6 Attributes' check is a class-id comparison and a page cannot forge one. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_vv_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_vv_class, &d) == 0,
          "VisualViewport: the per-realm prototype slot could not be declared");
    /* THE NUMBER WAS WRONG AND IS CORRECTED HERE. This slot used to name "CSSOM VIEW §2 the document's
       associated VisualViewport", and §2 is Terminology — §2.2 Zooming is where that section says anything
       about a visual viewport at all, and what it says is about the SCALE FACTOR, which is the fact the
       constant at the top of this file cites it for. The object this slot holds is §4 Extensions to the Window
       Interface's `[SameObject, Replaceable] readonly attribute VisualViewport? visualViewport`, of the
       interface §12 VisualViewport declares. A wrong section number reads as authoritative and sends the next
       reader somewhere that does not say what the code claims, which is worse than no citation. */
    g_obj_slot = realm_value_declare(ctx, "CSSOM VIEW §4 Extensions to the Window Interface's visualViewport, "
                                          "of the interface §12 VisualViewport declares");
    g_resize_slot = realm_value_declare(ctx, "CSSOM VIEW §13.1 the VisualViewport as the resize steps last saw "
                                             "it");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The class is on the list because
       `visual_viewport_free` PUTS IT BACK AT 0, which is that header's pre-init value for a class id, so it is
       agent state in its sense and the release is now the inverse of a declaration rather than of nothing.
       RESETTING IT IS SAFE HERE FOR THE REASON THAT HEADER MAKES A RULE OUT OF: nothing this class dispatches
       runs after the release column. It has no finalizer and no gc_mark — the one object per realm wears it as
       §3.7.6 Attributes' BRAND and carries no opaque — so the collection that follows platform_agent_free
       reaches nothing that would read a class id this call has already zeroed. */
    agent_state_class("visual_viewport", &g_vv_class,
                      "CSSOM VIEW §12 VisualViewport's class — its per-realm prototype slot and Web IDL "
                      "§3.7.6 Attributes' brand");
    agent_state_realm_slot("visual_viewport", &g_obj_slot,
                           "CSSOM VIEW §4 Extensions to the Window Interface's realm-value slot for `visualViewport`, "
                           "and this component's declaration latch");
    agent_state_realm_slot("visual_viewport", &g_resize_slot,
                           "CSSOM VIEW §13.1 Resizing viewports' realm-value slot for the VisualViewport as the resize "
                           "steps last saw it");
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
