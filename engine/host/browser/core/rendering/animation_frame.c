/* ANIMATION FRAMES — HTML §8.9. See animation_frame.h for why the map is a heap object and why the
   snapshot-then-recheck is the algorithm rather than an optimisation. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/rendering/animation_frame.h"

/* THE PER-REALM STORE. §8.9 gives every Window "a map of animation frame callbacks" and "an animation frame
   callback identifier", and both are per-Window rather than per-agent — a child navigable's rAF handles are
   its own, and a member installed once answers with the realm it was DEFINED in unless the state is reached
   through the realm. The slot holds ONE object for the realm's whole life and is never replaced: what
   time-travels is the object's PROPERTIES, which the heap COW captures, and replacing the slot would put one
   flow's map where every other flow looks. */
static int g_slot = -1;
static int g_id_request = -1, g_id_cancel = -1;
static JSAtom g_atom_queue = JS_ATOM_NULL, g_atom_next = JS_ATOM_NULL;
static int g_ready;

/* The store, OWNED. */
static JSValue af_store(JSContext *ctx)
{
    JSValue st;

    DCHECK(g_ready, "a Window's map of animation frame callbacks was reached before §8.9 was declared");
    st = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsObject(st),
           "a realm answered §8.9's map of animation frame callbacks with no map — every Window is given one "
           "at creation, so this realm never ran animation_frame_install and its `requestAnimationFrame` "
           "would register into nothing");
    return st;
}

/* The entry list, OWNED. Entry i is a two-element array [handle, callback], or `undefined` for an entry that
   has been taken or cancelled — a TOMBSTONE, because the snapshot is an index range and compacting under it
   would renumber the keys the run is walking. */
static JSValue af_queue(JSContext *ctx)
{
    JSValue st = af_store(ctx), q = JS_GetProperty(ctx, st, g_atom_queue);

    JS_FreeValue(ctx, st);
    DCHECK(JS_IsArray(q), "§8.9's map of animation frame callbacks lost its entry list");
    return q;
}

static uint32_t af_len(JSContext *ctx, JSValueConst q)
{
    JSValue len = JS_GetPropertyStr(ctx, q, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

bool animation_frame_pending(JSContext *ctx)
{
    JSValue q = af_queue(ctx);
    uint32_t i, n = af_len(ctx, q);
    bool any = false;

    for (i = 0; i < n && !any; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i);
        any = JS_IsObject(e);                     /* a tombstone is not a pending callback */
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, q);
    return any;
}

uint32_t animation_frame_snapshot(JSContext *ctx)
{
    JSValue q = af_queue(ctx);
    uint32_t n = af_len(ctx, q);

    JS_FreeValue(ctx, q);
    return n;
}

JSValue animation_frame_take(JSContext *ctx, uint32_t i)
{
    JSValue q = af_queue(ctx), e, cb;

    DCHECK(i < af_len(ctx, q),
           "§8.9's walk asked for an entry past the snapshot it took — the map only ever GROWS during a run, "
           "so an index inside the snapshot cannot fall off the end");
    e = JS_GetPropertyUint32(ctx, q, i);
    if (!JS_IsObject(e)) {   /* step 3's re-check: cancelled by an earlier callback of THIS frame */
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, q);
        return JS_UNDEFINED;
    }
    cb = JS_GetPropertyUint32(ctx, e, 1);
    DCHECK(JS_IsFunction(ctx, cb),
           "§8.9's map held an entry whose callback is not callable — the IDL's FrameRequestCallback brand "
           "check is what puts one there, so nothing else can have");
    /* "remove callbacks[handle]" — BEFORE the invoke, which is what makes a callback that re-registers itself
       get a NEW handle rather than resurrecting the one being run. */
    JS_SetPropertyUint32(ctx, q, i, JS_UNDEFINED);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, q);
    return cb;
}

void animation_frame_run_end(JSContext *ctx, uint32_t consumed)
{
    JSValue q = af_queue(ctx);
    uint32_t n = af_len(ctx, q), i;

    DCHECK(consumed <= n, "§8.9's walk consumed more entries than the map ever held");
    for (i = consumed; i < n; i++)
        JS_SetPropertyUint32(ctx, q, i - consumed, JS_GetPropertyUint32(ctx, q, i));
    JS_SetPropertyStr(ctx, q, "length", JS_NewUint32(ctx, n - consumed));
    JS_FreeValue(ctx, q);
}

/* HTML §8.9: `unsigned long requestAnimationFrame(FrameRequestCallback callback)`.
   1. Let target be this Window. 2. Let handle be target's animation frame callback identifier, incremented.
   3. Set callbacks[handle] to callback. 4. Return handle. No page code runs here: the callback's brand check
   is the declaration's (IDL_CALLBACK), so by the time this body runs there is nothing left to coerce. */
static JSValue js_request_animation_frame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                          int magic)
{
    JSValue st, q, entry, nv;
    uint32_t handle = 0;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1,
           "requestAnimationFrame reached its body with no callback — §3.6 step 5 throws for a call short of "
           "a member's REQUIRED arguments, and that throw is the declaration's");
    DCHECK(JS_IsFunction(ctx, argv[0]),
           "requestAnimationFrame's callback reached the body uncoerced — Web IDL §3.2.19's brand check belongs to "
           "the declaration (IDL_CALLBACK), and a body that re-tests it is a second answer to one question");
    st = af_store(ctx);
    nv = JS_GetProperty(ctx, st, g_atom_next);
    JS_ToUint32(ctx, &handle, nv);
    JS_FreeValue(ctx, nv);
    DCHECK(handle >= 1, "§8.9's animation frame callback identifier started below 1 — 0 is the handle a page "
                        "gets back for nothing, and `cancelAnimationFrame(0)` must name no entry");
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, handle + 1));
    JS_FreeValue(ctx, st);

    entry = JS_NewArray(ctx);
    CHECK(!JS_IsException(entry), "animation frames: OOM recording a requestAnimationFrame callback — a "
                                  "dropped one is a frame the page asked for and never gets");
    JS_SetPropertyUint32(ctx, entry, 0, JS_NewUint32(ctx, handle));
    JS_SetPropertyUint32(ctx, entry, 1, JS_DupValue(ctx, argv[0]));
    q = af_queue(ctx);
    JS_SetPropertyUint32(ctx, q, af_len(ctx, q), entry);
    JS_FreeValue(ctx, q);
    return JS_NewUint32(ctx, handle);
}

/* HTML §8.9: `undefined cancelAnimationFrame(unsigned long handle)` — "remove callbacks[handle]". A handle
   that names nothing does nothing, which is why there is no throw here and no report: a page cancelling a
   frame it already ran is ordinary code. */
static JSValue js_cancel_animation_frame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    JSValue q;
    uint32_t i, n, want = 0;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1,
           "cancelAnimationFrame reached its body with no handle — §3.6 step 5's TypeError is the "
           "declaration's, not this body's");
    DCHECK(JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(argv[0])),
           "cancelAnimationFrame's `handle` reached the body unconverted — the IDL declaration is what "
           "converts an `unsigned long`, and that conversion is the page's code");
    JS_ToUint32(ctx, &want, argv[0]);
    q = af_queue(ctx);
    n = af_len(ctx, q);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i), h;
        uint32_t got = 0;

        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
        h = JS_GetPropertyUint32(ctx, e, 0);
        JS_ToUint32(ctx, &got, h);
        JS_FreeValue(ctx, h);
        JS_FreeValue(ctx, e);
        if (got == want) { JS_SetPropertyUint32(ctx, q, i, JS_UNDEFINED); break; }
    }
    JS_FreeValue(ctx, q);
    return JS_UNDEFINED;
}

void animation_frame_init(JSContext *ctx)
{
    static const IdlArgType REQUEST_ARGS[1] = { IDL_CALLBACK };
    static const IdlArgType CANCEL_ARGS[1] = { IDL_UNSIGNED_LONG };

    DCHECK(!g_ready, "animation_frame_init ran twice — §8.9's members are declared once per agent");
    g_id_request = idl_method_id(ctx, REQUEST_ARGS, 1, js_request_animation_frame, 0);
    g_id_cancel = idl_method_id(ctx, CANCEL_ARGS, 1, js_cancel_animation_frame, 0);
    g_atom_queue = JS_NewAtom(ctx, "animationFrameCallbacks");
    g_atom_next = JS_NewAtom(ctx, "animationFrameCallbackIdentifier");
    CHECK(g_atom_queue != JS_ATOM_NULL && g_atom_next != JS_ATOM_NULL,
          "animation frames: the map's own keys could not be interned");
    g_slot = realm_value_declare(ctx, "§8.9 map of animation frame callbacks");
    g_ready = 1;
    realm_declare_intrinsic(animation_frame_install_map);
}

/* THE MAP IS BUILT AT REALM INSTALL, which puts it in the pre-boot BASELINE. Built lazily on the first
   `requestAnimationFrame` instead it would be whichever FLOW touched it first that owned it, and every
   sibling would then be registering into an object created inside another flow's delta. */
void animation_frame_install_map(JSContext *ctx)
{
    JSValue st;

    DCHECK(g_ready, "a realm asked for §8.9's map before the interface was declared");
    /* Running twice in one realm is asserted by realm_value_set, which is where the first value is standing. */
    st = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(st), "animation frames: this Window's map could not be allocated");
    {
        JSValue q = JS_NewArray(ctx);
        CHECK(!JS_IsException(q), "animation frames: this Window's entry list could not be allocated");
        JS_SetProperty(ctx, st, g_atom_queue, q);
    }
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, 1));   /* §8.9: handles start at 1 */
    realm_value_set(ctx, g_slot, st);
}

void animation_frame_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_ready, "§8.9's members were installed before they were declared");
    idl_install_method(ctx, (JSValue)global, "requestAnimationFrame", 1, g_id_request);
    idl_install_method(ctx, (JSValue)global, "cancelAnimationFrame", 1, g_id_cancel);
}

void animation_frame_free(JSContext *ctx)
{
    if (!g_ready) return;
    g_ready = 0;
    /* The MAPS are the realms' — each is released with its context, which is what the per-realm slot array
       is for. What this owns is the two interned keys. */
    JS_FreeAtom(ctx, g_atom_queue);
    JS_FreeAtom(ctx, g_atom_next);
    g_atom_queue = g_atom_next = JS_ATOM_NULL;
    g_slot = -1;
}
