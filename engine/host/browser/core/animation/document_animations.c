/* A Document's animation state and Web Animations Level 1 §4.4 "Animation Frames". See document_animations.h. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/animation/animation.h"
#include "core/animation/animation_time.h"
#include "core/animation/animation_timeline.h"
#include "core/animation/document_animations.h"
#include "core/realm.h"
#include "solver/flow.h"

static JSClassID g_slot = JS_INVALID_CLASS_ID;
static int g_ready;

/* An entry of the pending animation event queue: [event, target, scheduled event time]. */
enum { DA_EVENT = 0, DA_TARGET = 1, DA_SCHEDULED = 2 };

static uint32_t da_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static JSValue da_list(JSContext *ctx, JSValueConst record, const char *name)
{
    JSValue l = JS_GetPropertyStr(ctx, record, name);

    DCHECK(JS_IsArray(l), "a Document's animation record lost one of its lists — da_install_record builds all three");
    return l;
}

JSValue document_animations_record(JSContext *ctx)
{
    JSValue r;

    DCHECK(g_ready, "a Document's animation record was asked for before Web Animations was declared");
    r = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsObject(r), "a realm has no Document animation record — da_install_record builds one per realm");
    return r;
}

JSValue document_animations_now(JSContext *ctx, JSValueConst record)
{
    JSValue now = JS_GetPropertyStr(ctx, record, "now");

    (void)animation_time_resolved(now);
    return now;
}

void document_animations_add_timeline(JSContext *ctx, JSValueConst record, JSValueConst timeline)
{
    JSValue l = da_list(ctx, record, "timelines");

    JS_SetPropertyUint32(ctx, l, da_len(ctx, l), JS_DupValue(ctx, timeline));
    JS_FreeValue(ctx, l);
}

void document_animations_append_event(JSContext *ctx, JSValueConst record, JSValue ev, JSValueConst target,
                                      JSValueConst scheduled)
{
    JSValue q = da_list(ctx, record, "events"), e = JS_NewArray(ctx);

    CHECK(!JS_IsException(e), "a pending animation event could not be recorded");
    (void)animation_time_resolved(scheduled);
    JS_SetPropertyUint32(ctx, e, DA_EVENT, ev);
    JS_SetPropertyUint32(ctx, e, DA_TARGET, JS_DupValue(ctx, target));
    JS_SetPropertyUint32(ctx, e, DA_SCHEDULED, JS_DupValue(ctx, scheduled));
    JS_SetPropertyUint32(ctx, q, da_len(ctx, q), e);
    JS_FreeValue(ctx, q);
}

uint32_t document_animations_list_append(JSContext *ctx)
{
    JSValue record = document_animations_record(ctx), v = JS_GetPropertyStr(ctx, record, "listed");
    uint32_t n = 0;

    DCHECK(JS_IsNumber(v), "a Document's animation record lost its global animation list count");
    JS_ToUint32(ctx, &n, v);
    JS_SetPropertyStr(ctx, record, "listed", JS_NewUint32(ctx, n + 1));
    JS_FreeValue(ctx, record);
    return n + 1;
}

/* Every animation associated with one of the record's timelines, timeline by timeline, as one array. OWNED. */
static JSValue da_animations(JSContext *ctx, JSValueConst record)
{
    JSValue tls = da_list(ctx, record, "timelines"), out = JS_NewArray(ctx);
    uint32_t i, j, n = da_len(ctx, tls), m = 0;

    CHECK(!JS_IsException(out), "a Document's animations could not be gathered");
    for (i = 0; i < n; i++) {
        JSValue tl = JS_GetPropertyUint32(ctx, tls, i), as = animation_timeline_animations(ctx, tl);
        uint32_t k = da_len(ctx, as);

        for (j = 0; j < k; j++) JS_SetPropertyUint32(ctx, out, m++, JS_GetPropertyUint32(ctx, as, j));
        JS_FreeValue(ctx, as);
        JS_FreeValue(ctx, tl);
    }
    JS_FreeValue(ctx, tls);
    return out;
}

JSValue document_animations_all(JSContext *ctx)
{
    JSValue record = document_animations_record(ctx), as = da_animations(ctx, record);

    JS_FreeValue(ctx, record);
    return as;
}

bool document_animations_pending(JSContext *docctx)
{
    JSValue record = document_animations_record(docctx), q = da_list(docctx, record, "events");
    JSValue as;
    bool any = da_len(docctx, q) > 0;
    uint32_t i, n;

    JS_FreeValue(docctx, q);
    as = da_animations(docctx, record);
    n = da_len(docctx, as);
    for (i = 0; i < n && !any; i++) {
        JSValue a = JS_GetPropertyUint32(docctx, as, i);

        any = animation_needs_frame(docctx, a);
        JS_FreeValue(docctx, a);
    }
    JS_FreeValue(docctx, as);
    JS_FreeValue(docctx, record);
    return any;
}

/* §4.4 step 5's order: by scheduled event time, unresolved first, then by the composite order of the targets. */
static bool da_before(JSContext *ctx, JSValueConst x, JSValueConst y)
{
    JSValue tx = JS_GetPropertyUint32(ctx, x, DA_SCHEDULED), ty = JS_GetPropertyUint32(ctx, y, DA_SCHEDULED);
    JSValue ax, ay;
    bool rx = animation_time_resolved(tx), ry = animation_time_resolved(ty), before;

    if (rx != ry || (rx && !animation_time_eq(ctx, tx, ty))) {
        before = !rx || (ry && animation_time_lt(ctx, tx, ty));
        JS_FreeValue(ctx, tx);
        JS_FreeValue(ctx, ty);
        return before;
    }
    JS_FreeValue(ctx, tx);
    JS_FreeValue(ctx, ty);
    ax = JS_GetPropertyUint32(ctx, x, DA_TARGET);
    ay = JS_GetPropertyUint32(ctx, y, DA_TARGET);
    DCHECK(animation_is(ctx, ax) && animation_is(ctx, ay),
           "a pending animation event's target is not an animation — every producer in core/animation/ appends "
           "an AnimationPlaybackEvent targeted at the animation it is about");
    before = animation_composite_order(ctx, ax, ay) < 0;
    JS_FreeValue(ctx, ax);
    JS_FreeValue(ctx, ay);
    return before;
}

uint32_t document_animations_update(JSContext *docctx, JSValueConst now)
{
    JSValue record = document_animations_record(docctx), as, q, copy;
    uint32_t i, j, n;
    int task0 = 0, micro0 = 0, task1 = 0, micro1 = 0;
    const Flow *f = flow_running();

    DCHECK(animation_time_resolved(now), "update animations and send events was given no now timestamp");
    if (f) flow_job_kinds(f, &task0, &micro0);
    /* Step 1: "Update the current time of all timelines associated with doc passing now as the timestamp" — each
       document timeline derives its time from this one value — then the animations associated with them. */
    JS_SetPropertyStr(docctx, record, "now", JS_DupValue(docctx, now));
    as = da_animations(docctx, record);
    n = da_len(docctx, as);
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(docctx, as, i);

        animation_frame_service(docctx, a);
        JS_FreeValue(docctx, a);
    }
    /* Step 2: remove replaced animations. §5.5.2's test asks for each target property of an effect, and no
       effect here models its target properties. */
    for (i = 0; i < n; i++) {
        JSValue a = JS_GetPropertyUint32(docctx, as, i);

        if (animation_replaceable(docctx, a))
            DFAIL("Web Animations §5.5.2 Removing replaced animations reached a replaceable animation — build "
                  "the target properties of a KeyframeEffect (§6.6) so the higher-composite-order test can be asked");
        JS_FreeValue(docctx, a);
    }
    JS_FreeValue(docctx, as);
    /* Step 3: perform a microtask checkpoint. Step 1 is the only producer of microtasks here, so the checkpoint
       has work exactly when step 1 queued one, and this machine cannot run a checkpoint between its steps. */
    if (f) flow_job_kinds(f, &task1, &micro1);
    if (micro1 > micro0)
        DFAIL("Web Animations §4.4 step 3 must perform a microtask checkpoint here, and step 1 queued a microtask "
              "(a settled promise's reaction or the finish notification steps) — build a checkpoint the "
              "rendering machine can rest on between steps 1 and 4");
    /* Steps 4-5: copy, clear and stable-sort the pending animation event queue. An insertion sort is stable. */
    q = da_list(docctx, record, "events");
    n = da_len(docctx, q);
    copy = JS_NewArray(docctx);
    CHECK(!JS_IsException(copy), "the animation events to dispatch could not be copied");
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(docctx, q, i);

        for (j = i; j > 0; j--) {
            JSValue prev = JS_GetPropertyUint32(docctx, copy, j - 1);

            if (!da_before(docctx, e, prev)) { JS_FreeValue(docctx, prev); break; }
            JS_SetPropertyUint32(docctx, copy, j, prev);
        }
        JS_SetPropertyUint32(docctx, copy, j, e);
    }
    JS_FreeValue(docctx, q);
    JS_SetPropertyStr(docctx, record, "events", JS_NewArray(docctx));
    JS_SetPropertyStr(docctx, record, "dispatching", copy);
    JS_FreeValue(docctx, record);
    return n;
}

void document_animations_event_at(JSContext *docctx, uint32_t i, JSValue *target, JSValue *ev)
{
    JSValue record = document_animations_record(docctx), d = da_list(docctx, record, "dispatching"), e;

    DCHECK(i < da_len(docctx, d), "step 7 asked for an animation event past the copy step 4 took");
    e = JS_GetPropertyUint32(docctx, d, i);
    *ev = JS_GetPropertyUint32(docctx, e, DA_EVENT);
    *target = JS_GetPropertyUint32(docctx, e, DA_TARGET);
    JS_FreeValue(docctx, e);
    JS_FreeValue(docctx, d);
    JS_FreeValue(docctx, record);
}

void document_animations_dispatch_end(JSContext *docctx)
{
    JSValue record = document_animations_record(docctx);

    JS_SetPropertyStr(docctx, record, "dispatching", JS_NewArray(docctx));
    JS_FreeValue(docctx, record);
}

/* Built with the realm, so the record is baseline. The default document timeline is added by
   core/animation/animation_timeline.c's install, which runs after this one. */
static void da_install_record(JSContext *ctx)
{
    JSValue r = JS_NewObjectProto(ctx, JS_NULL);

    DCHECK(g_ready, "a realm asked for its Document animation record before Web Animations was declared");
    CHECK(!JS_IsException(r), "a Document's animation record could not be allocated");
    JS_SetPropertyStr(ctx, r, "now", JS_NULL);
    JS_SetPropertyStr(ctx, r, "timelines", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, r, "events", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, r, "dispatching", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, r, "listed", JS_NewUint32(ctx, 0));
    realm_value_set(ctx, g_slot, r);
}

void document_animations_init(JSContext *ctx)
{
    DCHECK(!g_ready, "document_animations_init ran twice — a Document's animation record is declared once per agent");
    g_slot = realm_value_declare(ctx, "Web Animations Level 1 a Document's timelines and pending animation events");
    g_ready = 1;
    realm_declare_intrinsic(da_install_record);
    agent_state_flag("animation", &g_ready, "the declaration latch");
    agent_state_realm_slot("animation", &g_slot,
                           "the per-realm slot a Document's Web Animations record is held in");
}

void document_animations_free(JSRuntime *rt)
{
    (void)rt;
    DCHECK(g_ready, "a Document's animation record was released in an agent that never declared it");
    agent_state_reached("animation");   /* animation_free undoes the row's slots, these included */
}
