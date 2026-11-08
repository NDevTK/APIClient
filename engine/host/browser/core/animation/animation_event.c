/* CSS Animations' AnimationEvent. See animation_event.h. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/animation/animation_event.h"
#include "core/animation/css_animations.h"
#include "core/events/event.h"
#include "core/idl_args.h"
#include "core/realm.h"

static JSRuntime *g_rt;
static JSClassID g_class;
static JSValue g_key = JS_UNDEFINED;   /* the private Symbol the four attributes live under */
static int g_id_ctor = -1;

enum { AE_ANIMATION_NAME, AE_ELAPSED_TIME, AE_PSEUDO_ELEMENT, AE_ANIMATION };
static const char *const AE_SLOT[] = { "animationName", "elapsedTime", "pseudoElement", "animation" };

static JSValue ae_proto(JSContext *ctx)
{
    JSValue p = JS_GetClassProto(ctx, g_class);

    DCHECK(!JS_IsNull(p), "AnimationEvent.prototype was asked for in a realm that never installed it");
    return p;
}

/* Hang the four attributes on `ev`. Each is borrowed and duplicated. */
static JSValue ae_attach(JSContext *ctx, JSValue ev, JSValueConst name, JSValueConst elapsed, JSValueConst pseudo,
                         JSValueConst animation)
{
    JSValue slots;
    JSAtom k;

    if (JS_IsException(ev)) return ev;
    DCHECK(JS_IsString(name) && JS_IsNumber(elapsed) && JS_IsString(pseudo) &&
               (JS_IsNull(animation) || css_animations_is(ctx, animation)),
           "an AnimationEvent's attributes reached it unconverted — the constructor's dictionary and the user "
           "agent's producer both state a CSSOMString, a double, a CSSOMString and a CSSAnimation?");
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "AnimationEvent: OOM allocating its slots");
    JS_SetPropertyStr(ctx, slots, "animationName", JS_DupValue(ctx, name));
    JS_SetPropertyStr(ctx, slots, "elapsedTime", JS_DupValue(ctx, elapsed));
    JS_SetPropertyStr(ctx, slots, "pseudoElement", JS_DupValue(ctx, pseudo));
    JS_SetPropertyStr(ctx, slots, "animation", JS_DupValue(ctx, animation));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the AnimationEvent slot key could not be reached");
    JS_DefinePropertyValue(ctx, ev, k, slots, 0);
    JS_FreeAtom(ctx, k);
    return ev;
}

JSValue animation_event_new(JSContext *ctx, const char *type, JSValueConst animation, JSValueConst name,
                            double elapsed_s)
{
    JSValue t = JS_NewString(ctx, type), pseudo = JS_NewString(ctx, ""), elapsed = JS_NewFloat64(ctx, elapsed_s), ev;

    DCHECK(g_rt != NULL, "an AnimationEvent was created before CSS Animations §5.1 was declared");
    ev = event_new_derived(ctx, ae_proto(ctx), t, true, false, false, true);
    JS_FreeValue(ctx, t);
    CHECK(!JS_IsException(ev), "an AnimationEvent could not be allocated");
    ev = ae_attach(ctx, ev, name, elapsed, pseudo, animation);
    JS_FreeValue(ctx, pseudo);
    return ev;
}

static JSValue js_ae_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    JSValue slots = JS_UNDEFINED, v;

    CHECK(k != JS_ATOM_NULL, "the AnimationEvent slot key could not be reached");
    if (!JS_IsObject(this_val) || JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "the receiver is not an AnimationEvent");
    }
    v = JS_GetPropertyStr(ctx, slots, AE_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* Level 1 §5.1: "AnimationEvent(type, animationEventInitDict) is an event constructor" — DOM §2.5's steps, with
   the dictionary's own members. */
static JSValue js_ae_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev, v[4], r;
    int i;

    (void)magic;
    if (JS_IsUndefined(this_val)) return JS_ThrowTypeError(ctx, "constructor AnimationEvent requires 'new'");
    DCHECK(argc >= 1, "AnimationEvent's constructor reached its body with no type — §3.6 step 5's TypeError is the "
                      "declaration's");
    ev = event_new_derived(ctx, ae_proto(ctx), argv[0], idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"), idl_dict_bool(ctx, init, "composed"), false);
    for (i = 0; i < 4; i++) v[i] = idl_dict_get(ctx, init, AE_SLOT[i]);
    r = ae_attach(ctx, ev, v[0], v[1], v[2], v[3]);
    for (i = 0; i < 4; i++) JS_FreeValue(ctx, v[i]);
    return r;
}

static bool ae_css_animation_is(JSContext *ctx, JSValueConst v) { return css_animations_is(ctx, v); }

static void animation_event_install_realm(JSContext *ctx)
{
    JSValue base = event_proto(ctx), proto, prev;
    int i;

    DCHECK(g_rt != NULL, "a realm asked for AnimationEvent.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "animation_event_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "AnimationEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "AnimationEvent");
    for (i = 0; i < 4; i++) idl_install_accessor(ctx, proto, AE_SLOT[i], js_ae_get, i, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

void animation_event_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor = idl_step_constructor(ctx, "AnimationEvent", g_id_ctor), proto;

    DCHECK(g_rt != NULL, "CSS Animations §5.1 was installed before it was declared");
    CHECK(!JS_IsException(ctor), "the AnimationEvent interface object could not be allocated");
    proto = ae_proto(ctx);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "AnimationEvent", ctor);
}

void animation_event_init(JSContext *ctx)
{
    static const IdlArgType CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    /* EventInit's members, then AnimationEventInit's with Level 2's partial dictionary merged in, sorted. */
    static const IdlDictMember INIT[] = {
        { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
        { "animation", IDL_INTERFACE_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL,
          .iface_is = ae_css_animation_is, .iface_name = "CSSAnimation" },
        { "animationName", IDL_DOMSTRING, false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
        { "elapsedTime", IDL_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
        { "pseudoElement", IDL_DOMSTRING, false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_rt == NULL, "animation_event_init ran twice — AnimationEvent is declared once per agent");
    g_rt = rt;
    {
        JSClassDef def = { "AnimationEvent" };

        JS_NewClassID(rt, &g_class);
        CHECK(JS_NewClass(rt, g_class, &def) == 0, "AnimationEvent's prototype slot could not be declared");
    }
    g_key = JS_NewSymbol(ctx, "animationEventSlots", false);
    CHECK(!JS_IsException(g_key), "the AnimationEvent slot key could not be allocated");
    g_id_ctor = idl_method_id_dict(ctx, CTOR_ARGS, 2, INIT, (int)(sizeof INIT / sizeof INIT[0]), js_ae_ctor, 0);
    idl_optional_from(1);   /* `optional AnimationEventInit animationEventInitDict = {}` */
    agent_state_ptr("animation", &g_rt, "the runtime CSS Animations §5.1 AnimationEvent was declared in");
    agent_state_class("animation", &g_class, "CSS Animations §5.1 AnimationEvent's prototype slot");
    agent_state_value("animation", &g_key, "the private key AnimationEvent's four attributes live under");
    agent_state_id("animation", &g_id_ctor, "CSS Animations §5.1 AnimationEvent's constructor declaration");
    realm_declare_intrinsic(animation_event_install_realm);
}

void animation_event_free(JSRuntime *rt)
{
    DCHECK(g_rt == rt, "AnimationEvent was released against a runtime other than the one it was declared in");
    JS_FreeValueRT(rt, g_key);
    agent_state_reached("animation");   /* animation_free undoes the row's slots, these included */
}
