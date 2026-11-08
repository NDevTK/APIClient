/* Web Animations Level 1 §6.12 "The AnimationPlaybackEvent interface". See animation_playback_event.h. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/animation/animation_playback_event.h"
#include "core/animation/animation_time.h"
#include "core/events/event.h"
#include "core/idl_args.h"
#include "core/realm.h"

static JSRuntime *g_rt;
static JSClassID g_class;
static JSValue g_key = JS_UNDEFINED;   /* the private Symbol the two attributes live under */
static int g_id_ctor = -1;

enum { APE_CURRENT_TIME, APE_TIMELINE_TIME };

static JSValue ape_proto(JSContext *ctx)
{
    JSValue p = JS_GetClassProto(ctx, g_class);

    DCHECK(!JS_IsNull(p), "AnimationPlaybackEvent.prototype was asked for in a realm that never installed it");
    return p;
}

/* Hang the two attributes on `ev` (consumed on failure only through its own exception). */
static JSValue ape_attach(JSContext *ctx, JSValue ev, JSValueConst current_time, JSValueConst timeline_time)
{
    JSValue slots;
    JSAtom k;

    if (JS_IsException(ev)) return ev;
    (void)animation_time_resolved(current_time);
    (void)animation_time_resolved(timeline_time);
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "AnimationPlaybackEvent: OOM allocating its slots");
    JS_SetPropertyStr(ctx, slots, "currentTime", JS_DupValue(ctx, current_time));
    JS_SetPropertyStr(ctx, slots, "timelineTime", JS_DupValue(ctx, timeline_time));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the AnimationPlaybackEvent slot key could not be reached");
    JS_DefinePropertyValue(ctx, ev, k, slots, 0);
    JS_FreeAtom(ctx, k);
    return ev;
}

JSValue animation_playback_event_new(JSContext *ctx, const char *type, JSValueConst current_time,
                                     JSValueConst timeline_time)
{
    JSValue t = JS_NewString(ctx, type), ev;

    DCHECK(g_rt != NULL, "an AnimationPlaybackEvent was created before §6.12 was declared");
    ev = event_new_derived(ctx, ape_proto(ctx), t, false, false, false, true);
    JS_FreeValue(ctx, t);
    CHECK(!JS_IsException(ev), "an AnimationPlaybackEvent could not be allocated");
    return ape_attach(ctx, ev, current_time, timeline_time);
}

static JSValue js_ape_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots = JS_UNDEFINED, v;

    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the AnimationPlaybackEvent slot key could not be reached");
    if (!JS_IsObject(this_val) || JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "the receiver is not an AnimationPlaybackEvent");
    }
    v = JS_GetPropertyStr(ctx, slots, magic == APE_CURRENT_TIME ? "currentTime" : "timelineTime");
    JS_FreeValue(ctx, slots);
    return v;
}

/* §6.12's constructor: "Constructs a new AnimationPlaybackEvent object using the procedure defined for
   constructing events" — DOM §2.5's inner event creation steps, with the init dictionary's own two members. */
static JSValue js_ape_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev, cur, tl, r;

    (void)magic;
    if (JS_IsUndefined(this_val)) return JS_ThrowTypeError(ctx, "constructor AnimationPlaybackEvent requires 'new'");
    DCHECK(argc >= 1, "AnimationPlaybackEvent's constructor reached its body with no type — §3.6 step 5's TypeError "
                      "is the declaration's");
    ev = event_new_derived(ctx, ape_proto(ctx), argv[0], idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"), idl_dict_bool(ctx, init, "composed"), false);
    cur = idl_dict_get(ctx, init, "currentTime");
    tl = idl_dict_get(ctx, init, "timelineTime");
    r = ape_attach(ctx, ev, cur, tl);
    JS_FreeValue(ctx, cur);
    JS_FreeValue(ctx, tl);
    return r;
}

static void animation_playback_event_install_realm(JSContext *ctx)
{
    JSValue base = event_proto(ctx), proto, prev;

    DCHECK(g_rt != NULL, "a realm asked for AnimationPlaybackEvent.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "animation_playback_event_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "AnimationPlaybackEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "AnimationPlaybackEvent");
    idl_install_accessor(ctx, proto, "currentTime", js_ape_get, APE_CURRENT_TIME, -1);
    idl_install_accessor(ctx, proto, "timelineTime", js_ape_get, APE_TIMELINE_TIME, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

void animation_playback_event_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor = idl_step_constructor(ctx, "AnimationPlaybackEvent", g_id_ctor), proto;

    DCHECK(g_rt != NULL, "Web Animations §6.12 was installed before it was declared");
    CHECK(!JS_IsException(ctor), "the AnimationPlaybackEvent interface object could not be allocated");
    proto = ape_proto(ctx);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "AnimationPlaybackEvent", ctor);
}

void animation_playback_event_init(JSContext *ctx)
{
    static const IdlArgType CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    /* EventInit's members first, then AnimationPlaybackEventInit's own, each dictionary's in sorted order. */
    static const IdlDictMember INIT[] = {
        { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
        { "currentTime", IDL_DOUBLE_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL },
        { "timelineTime", IDL_DOUBLE_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL },
    };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_rt == NULL, "animation_playback_event_init ran twice — §6.12 is declared once per agent");
    g_rt = rt;
    {
        JSClassDef def = { "AnimationPlaybackEvent" };

        JS_NewClassID(rt, &g_class);
        CHECK(JS_NewClass(rt, g_class, &def) == 0, "AnimationPlaybackEvent's prototype slot could not be declared");
    }
    g_key = JS_NewSymbol(ctx, "animationPlaybackEventSlots", false);
    CHECK(!JS_IsException(g_key), "the AnimationPlaybackEvent slot key could not be allocated");
    g_id_ctor = idl_method_id_dict(ctx, CTOR_ARGS, 2, INIT, (int)(sizeof INIT / sizeof INIT[0]), js_ape_ctor, 0);
    idl_optional_from(1);   /* `optional AnimationPlaybackEventInit eventInitDict = {}` */
    agent_state_ptr("animation", &g_rt, "the runtime Web Animations §6.12 was declared in");
    agent_state_class("animation", &g_class, "§6.12 AnimationPlaybackEvent's prototype slot");
    agent_state_value("animation", &g_key, "the private key §6.12's two attributes live under");
    agent_state_id("animation", &g_id_ctor, "§6.12 AnimationPlaybackEvent's constructor declaration");
    realm_declare_intrinsic(animation_playback_event_install_realm);
}

void animation_playback_event_free(JSRuntime *rt)
{
    DCHECK(g_rt != NULL, "Web Animations §6.12 was released in an agent that never declared it");
    DCHECK(g_rt == rt, "Web Animations §6.12 was released against a runtime other than the one it was declared in");
    JS_FreeValueRT(rt, g_key);
    agent_state_reached("animation");   /* animation_free undoes the row's slots, these included */
}
