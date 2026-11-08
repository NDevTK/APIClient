/* Web Animations Level 1 §4.3 "Timelines", §6.2, §6.3 and §6.9. See animation_timeline.h. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/animation/animation_time.h"
#include "core/animation/animation_timeline.h"
#include "core/animation/document_animations.h"
#include "core/idl_args.h"
#include "core/realm.h"

static JSRuntime *g_rt;
static JSClassID g_tl_class, g_dtl_class;
static JSValue g_key = JS_UNDEFINED;   /* the private Symbol a timeline's internal slots live under */
static int g_id_ctor = -1;

/* A timeline's internal slots: `doc`, its Document's record; `origin`, §4.3.1's origin time; `animations`, the
   animations associated with it. A null-prototype object, so reading it runs none of the page's code. */
static JSValue tl_slots(JSContext *ctx, JSValueConst timeline)
{
    JSAtom k;
    JSValue slots = JS_UNDEFINED;

    DCHECK(g_rt != NULL, "a timeline's slots were read before §6.3's class was declared");
    DCHECK(JS_GetClassID(timeline) == g_dtl_class,
           "a timeline's slots were asked of an object that is not a DocumentTimeline — the callers brand first");
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the timeline slot key could not be reached");
    if (JS_GetOwnSlot(ctx, &slots, timeline, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsObject(slots), "a DocumentTimeline has no internal slots — tl_new attaches them at creation");
    return slots;
}

bool animation_timeline_is(JSContext *ctx, JSValueConst v)
{
    (void)ctx;
    DCHECK(g_rt != NULL, "AnimationTimeline's brand was asked before its class was declared");
    return JS_IsObject(v) && JS_GetClassID(v) == g_dtl_class;
}

/* A new DocumentTimeline of this realm's Document with origin time `origin`, associated with that Document. */
static JSValue tl_new(JSContext *ctx, JSValueConst origin)
{
    JSValue proto = JS_GetClassProto(ctx, g_dtl_class), obj, slots, record;
    JSAtom k;

    DCHECK(!JS_IsNull(proto), "a DocumentTimeline was created in a realm that never installed its prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_dtl_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DocumentTimeline could not be allocated");
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "a DocumentTimeline's slots could not be allocated");
    record = document_animations_record(ctx);
    JS_SetPropertyStr(ctx, slots, "doc", JS_DupValue(ctx, record));
    JS_SetPropertyStr(ctx, slots, "origin", JS_DupValue(ctx, origin));
    JS_SetPropertyStr(ctx, slots, "animations", JS_NewArray(ctx));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the timeline slot key could not be reached");
    JS_DefinePropertyValue(ctx, obj, k, slots, 0);
    JS_FreeAtom(ctx, k);
    document_animations_add_timeline(ctx, record, obj);
    JS_FreeValue(ctx, record);
    return obj;
}

JSValue animation_timeline_document_record(JSContext *ctx, JSValueConst timeline)
{
    JSValue slots = tl_slots(ctx, timeline), record = JS_GetPropertyStr(ctx, slots, "doc");

    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsObject(record), "a DocumentTimeline is associated with no Document — tl_new associates every one");
    return record;
}

JSValue animation_timeline_default(JSContext *ctx)
{
    JSValue record = document_animations_record(ctx), tl = JS_GetPropertyStr(ctx, record, "defaultTimeline");

    JS_FreeValue(ctx, record);
    DCHECK(animation_timeline_is(ctx, tl),
           "a Document has no default document timeline — §4.3.1.1 gives every Document one, built with its realm");
    return tl;
}

/* §4.3.1: the now timestamp minus the origin time; unresolved until the first now timestamp. */
JSValue animation_timeline_current_time(JSContext *ctx, JSValueConst timeline)
{
    JSValue slots = tl_slots(ctx, timeline), record = JS_GetPropertyStr(ctx, slots, "doc");
    JSValue origin = JS_GetPropertyStr(ctx, slots, "origin"), now = document_animations_now(ctx, record), t;

    t = animation_time_resolved(now) ? animation_time_sub(ctx, now, origin) : JS_NULL;
    JS_FreeValue(ctx, now);
    JS_FreeValue(ctx, origin);
    JS_FreeValue(ctx, record);
    JS_FreeValue(ctx, slots);
    return t;
}

/* §4.3.1: "return the sum of the timeline time and timeline's origin time. If timeline is inactive, return an
   unresolved time value." */
JSValue animation_timeline_to_origin_relative(JSContext *ctx, JSValueConst timeline, JSValueConst time)
{
    JSValue cur = animation_timeline_current_time(ctx, timeline), slots, origin, t;

    if (!animation_time_resolved(cur)) return cur;
    JS_FreeValue(ctx, cur);
    slots = tl_slots(ctx, timeline);
    origin = JS_GetPropertyStr(ctx, slots, "origin");
    t = animation_time_add(ctx, time, origin);
    JS_FreeValue(ctx, origin);
    JS_FreeValue(ctx, slots);
    return t;
}

JSValue animation_timeline_animations(JSContext *ctx, JSValueConst timeline)
{
    JSValue slots = tl_slots(ctx, timeline), list = JS_GetPropertyStr(ctx, slots, "animations");

    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsArray(list), "a DocumentTimeline lost its list of associated animations");
    return list;
}

static uint32_t tl_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

void animation_timeline_associate(JSContext *ctx, JSValueConst timeline, JSValueConst animation)
{
    JSValue list = animation_timeline_animations(ctx, timeline);

    JS_SetPropertyUint32(ctx, list, tl_len(ctx, list), JS_DupValue(ctx, animation));
    JS_FreeValue(ctx, list);
}

void animation_timeline_dissociate(JSContext *ctx, JSValueConst timeline, JSValueConst animation)
{
    JSValue list = animation_timeline_animations(ctx, timeline), out = JS_NewArray(ctx);
    JSValue slots = tl_slots(ctx, timeline);
    uint32_t i, n = tl_len(ctx, list), m = 0, found = 0;

    CHECK(!JS_IsException(out), "a timeline's list of associated animations could not be rebuilt");
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(ctx, list, i);

        if (JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(animation)) { found++; JS_FreeValue(ctx, a); continue; }
        JS_SetPropertyUint32(ctx, out, m++, a);
    }
    DCHECK(found == 1, "an animation was dissociated from a timeline it was not associated with exactly once — "
                       "§4.5.2 associates an animation with one timeline at a time");
    JS_SetPropertyStr(ctx, slots, "animations", out);
    JS_FreeValue(ctx, slots);
    JS_FreeValue(ctx, list);
}

/* §6.2's `readonly attribute double? currentTime`: "Returns the current time for this timeline or null if this
   timeline is inactive." */
static JSValue js_tl_current_time(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!animation_timeline_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "'currentTime' getter called on an object that is not an AnimationTimeline");
    return animation_timeline_current_time(ctx, this_val);
}

/* §6.3's `constructor(optional DocumentTimelineOptions options = {})`: "Creates a new DocumentTimeline. The
   Document with which the timeline is associated is the Document associated with the Window that is the current
   global object." `ctx` is that global's realm, the realm this constructor was defined in. */
static JSValue js_dtl_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue origin, tl;

    (void)magic;
    if (JS_IsUndefined(this_val)) return JS_ThrowTypeError(ctx, "constructor DocumentTimeline requires 'new'");
    origin = idl_dict_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "originTime");
    DCHECK(JS_IsNumber(origin) || JS_IsUndefined(origin) || animation_time_resolved(origin),
           "DocumentTimelineOptions' originTime crossed unconverted — §3.2.17 converts a declared member");
    if (JS_IsUndefined(origin)) origin = JS_NewFloat64(ctx, 0);
    tl = tl_new(ctx, origin);
    JS_FreeValue(ctx, origin);
    return tl;
}

/* §6.9's `readonly attribute DocumentTimeline timeline`: the default document timeline. The receiver is a
   Document of this realm, so its realm's record is the one. */
static JSValue js_doc_timeline(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val;
    (void)magic;
    return animation_timeline_default(ctx);
}

/* Per realm: the two prototypes and the default document timeline, built with the realm so it is baseline. */
static void animation_timeline_install_realm(JSContext *ctx)
{
    JSValue base, proto, prev, record, tl;

    DCHECK(g_rt != NULL, "a realm asked for AnimationTimeline.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_tl_class);
    DCHECK(JS_IsNull(prev), "animation_timeline_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = JS_NewObject(ctx);
    CHECK(!JS_IsException(base), "AnimationTimeline.prototype could not be allocated");
    idl_interface_tag(ctx, base, "AnimationTimeline");
    idl_install_accessor(ctx, base, "currentTime", js_tl_current_time, 0, -1);
    proto = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(proto), "DocumentTimeline.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DocumentTimeline");
    JS_SetClassProto(ctx, g_tl_class, base);
    JS_SetClassProto(ctx, g_dtl_class, proto);
    /* §4.3.1.1: "The default document timeline has an origin time of zero." */
    tl = tl_new(ctx, JS_NewFloat64(ctx, 0));
    record = document_animations_record(ctx);
    JS_SetPropertyStr(ctx, record, "defaultTimeline", tl);
    JS_FreeValue(ctx, record);
}

void animation_timeline_install_document_members(JSContext *ctx, JSValueConst document_proto)
{
    DCHECK(g_rt != NULL, "§6.9's Document member was installed before Web Animations was declared");
    idl_install_accessor(ctx, document_proto, "timeline", js_doc_timeline, 0, -1);
}

void animation_timeline_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto, ctor;

    DCHECK(g_rt != NULL, "Web Animations §6.2 was installed before it was declared");
    proto = JS_GetClassProto(ctx, g_tl_class);
    DCHECK(!JS_IsNull(proto), "AnimationTimeline was installed in a realm with no prototype for it");
    ctor = idl_interface_object(ctx, "AnimationTimeline", proto);
    CHECK(!JS_IsException(ctor), "the AnimationTimeline interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "AnimationTimeline", ctor);
    ctor = idl_step_constructor(ctx, "DocumentTimeline", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the DocumentTimeline interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_dtl_class);
    DCHECK(!JS_IsNull(proto), "DocumentTimeline was installed in a realm with no prototype for it");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "DocumentTimeline", ctor);
}

void animation_timeline_init(JSContext *ctx)
{
    static const IdlArgType CTOR_ARGS[1] = { IDL_DICT };
    static const IdlDictMember OPTIONS[] = {
        { "originTime", IDL_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_ZERO },
    };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_rt == NULL, "animation_timeline_init ran twice — §6.2 and §6.3 are declared once per agent");
    g_rt = rt;
    {
        JSClassDef tl = { "AnimationTimeline" }, dtl = { "DocumentTimeline" };

        JS_NewClassID(rt, &g_tl_class);
        CHECK(JS_NewClass(rt, g_tl_class, &tl) == 0, "AnimationTimeline's prototype slot could not be declared");
        JS_NewClassID(rt, &g_dtl_class);
        CHECK(JS_NewClass(rt, g_dtl_class, &dtl) == 0, "DocumentTimeline's prototype slot could not be declared");
    }
    g_key = JS_NewSymbol(ctx, "documentTimelineSlots", false);
    CHECK(!JS_IsException(g_key), "the DocumentTimeline slot key could not be allocated");
    g_id_ctor = idl_method_id_dict(ctx, CTOR_ARGS, 1, OPTIONS, 1, js_dtl_ctor, 0);
    idl_optional_from(0);   /* `optional DocumentTimelineOptions options = {}` */
    agent_state_ptr("animation", &g_rt, "the runtime Web Animations §6.2 and §6.3 were declared in");
    agent_state_class("animation", &g_tl_class, "Web Animations §6.2 AnimationTimeline's prototype slot");
    agent_state_class("animation", &g_dtl_class,
                      "Web Animations §6.3 DocumentTimeline's prototype slot and the brand of every timeline");
    agent_state_value("animation", &g_key, "the private key a DocumentTimeline's internal slots live under");
    agent_state_id("animation", &g_id_ctor, "Web Animations §6.3 DocumentTimeline's constructor declaration");
    realm_declare_intrinsic(animation_timeline_install_realm);
}

void animation_timeline_free(JSRuntime *rt)
{
    DCHECK(g_rt != NULL, "Web Animations §6.2 was released in an agent that never declared it");
    DCHECK(g_rt == rt, "Web Animations §6.2 was released against a runtime other than the one it was declared in");
    JS_FreeValueRT(rt, g_key);
    agent_state_reached("animation");   /* animation_free undoes the row's slots, these included */
}
