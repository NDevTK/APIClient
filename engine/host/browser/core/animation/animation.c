/* Web Animations Level 1 §4.5 "Animations" and §6.4 "The Animation interface". See animation.h. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/animation/animatable.h"
#include "core/animation/animation.h"
#include "core/animation/animation_event.h"
#include "core/animation/animation_playback_event.h"
#include "core/animation/animation_time.h"
#include "core/animation/animation_timeline.h"
#include "core/animation/css_animations.h"
#include "core/animation/document_animations.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/timing/task_source.h"

static JSRuntime *g_rt;
static JSClassID g_class;
static JSClassID g_notify_slot = JS_INVALID_CLASS_ID;   /* this realm's finish notification steps function */
static JSValue g_key = JS_UNDEFINED;                    /* the private Symbol an animation's slots live under */
static int g_id_ctor = -1, g_id_set_id = -1, g_id_set_timeline = -1, g_id_set_start = -1, g_id_set_current = -1,
           g_id_set_rate = -1, g_id_cancel = -1, g_id_finish = -1, g_id_play = -1, g_id_pause = -1,
           g_id_update_rate = -1, g_id_reverse = -1, g_id_persist = -1;

/* The pending task an animation holds (§4.5.8 step 15, §4.5.9's pending pause task). */
enum { AN_TASK_NONE = 0, AN_TASK_PLAY = 1, AN_TASK_PAUSE = 2 };
/* §5.5.1's replace state. */
enum { AN_REPLACE_ACTIVE = 0, AN_REPLACE_REMOVED = 1, AN_REPLACE_PERSISTED = 2 };
/* §4.5.17's play states. */
typedef enum { AN_IDLE, AN_RUNNING, AN_PAUSED, AN_FINISHED } AnPlayState;
static const char *const PLAY_STATE[] = { "idle", "running", "paused", "finished" };
static const char *const REPLACE_STATE[] = { "active", "removed", "persisted" };

/* ---- internal slots --------------------------------------------------------------------------------------- */

static bool an_is_value(JSValueConst v)
{
    return JS_IsObject(v) && JS_GetClassID(v) == g_class;
}

bool animation_is(JSContext *ctx, JSValueConst v)
{
    (void)ctx;
    DCHECK(g_rt != NULL, "Animation's brand was asked before its class was declared");
    /* An interface inheriting Animation (CSSAnimation) shares the class and differs in its prototype. */
    return an_is_value(v);
}

JSValue animation_slots(JSContext *ctx, JSValueConst animation)
{
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    JSValue slots = JS_UNDEFINED;

    CHECK(k != JS_ATOM_NULL, "the Animation slot key could not be reached");
    if (JS_GetOwnSlot(ctx, &slots, animation, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsObject(slots), "an Animation has no internal slots — animation_create attaches them at creation");
    return slots;
}

static JSValue sget(JSContext *ctx, JSValueConst slots, const char *k) { return JS_GetPropertyStr(ctx, slots, k); }
static void sset(JSContext *ctx, JSValueConst slots, const char *k, JSValue v)
{
    CHECK(JS_SetPropertyStr(ctx, slots, k, v) >= 0, "an Animation's internal slot could not be written");
}
static int sint(JSContext *ctx, JSValueConst slots, const char *k)
{
    JSValue v = sget(ctx, slots, k);
    int32_t n = 0;

    DCHECK(JS_IsNumber(v), "an Animation's integer slot holds no number");
    JS_ToInt32(ctx, &n, v);
    return n;
}

/* Is a time-valued slot resolved? */
static bool sresolved(JSContext *ctx, JSValueConst slots, const char *k)
{
    JSValue v = sget(ctx, slots, k);
    bool r = animation_time_resolved(v);

    JS_FreeValue(ctx, v);
    return r;
}

static JSValue zero(JSContext *ctx) { return JS_NewFloat64(ctx, 0); }

/* A rate or time compared against zero. */
static bool tv_lt0(JSContext *ctx, JSValueConst v) { JSValue z = JS_NewInt32(ctx, 0); return animation_time_lt(ctx, v, z); }
static bool tv_gt0(JSContext *ctx, JSValueConst v) { JSValue z = JS_NewInt32(ctx, 0); return animation_time_lt(ctx, z, v); }
static bool tv_is0(JSContext *ctx, JSValueConst v) { JSValue z = JS_NewInt32(ctx, 0); return animation_time_eq(ctx, v, z); }

/* ---- promises ----------------------------------------------------------------------------------------------- */

/* A new pending promise of this realm in slots `<name>`, with its resolving functions beside it. */
static void an_new_promise(JSContext *ctx, JSValueConst slots, const char *name)
{
    JSValue funcs[2], p = JS_NewPromiseCapability(ctx, funcs);
    char k[32];

    CHECK(!JS_IsException(p), "an Animation's promise could not be allocated");
    sset(ctx, slots, name, p);
    snprintf(k, sizeof k, "%sResolve", name);
    sset(ctx, slots, k, funcs[0]);
    snprintf(k, sizeof k, "%sReject", name);
    sset(ctx, slots, k, funcs[1]);
    snprintf(k, sizeof k, "%sSettled", name);
    sset(ctx, slots, k, JS_FALSE);
}

/* Settle slots `<name>` with `value`: a resolving function runs on a flow base, so it is called as one. */
static void an_settle(JSContext *ctx, JSValueConst slots, const char *name, bool fulfil, JSValueConst value)
{
    char k[32];
    JSValue fn;
    int r;

    snprintf(k, sizeof k, "%s%s", name, fulfil ? "Resolve" : "Reject");
    fn = sget(ctx, slots, k);
    DCHECK(JS_IsFunction(ctx, fn), "an Animation's promise has no resolving function — an_new_promise stores both");
    r = JS_CallAsFlow(ctx, fn, value);
    JS_FreeValue(ctx, fn);
    DCHECK(r >= 0, "an Animation's promise resolving function completed abruptly — ECMAScript §27.2.1.3.2's "
                   "resolve turns a throwing `then` getter into a rejection, so the function itself never throws");
    snprintf(k, sizeof k, "%sSettled", name);
    sset(ctx, slots, k, JS_TRUE);
}

static bool an_settled(JSContext *ctx, JSValueConst slots, const char *name)
{
    char k[32];
    JSValue v;
    bool s;

    snprintf(k, sizeof k, "%sSettled", name);
    v = sget(ctx, slots, k);
    s = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return s;
}

/* A DOMException named `name`, as a value. */
static JSValue an_dom_exception(JSContext *ctx, const char *name, const char *msg)
{
    JS_ThrowDOMException(ctx, name, "%s", msg);
    return JS_GetException(ctx);
}

/* Reject slots `<name>` with an "AbortError" and set its [[PromiseIsHandled]] (§4.5.14's two steps). */
static void an_abort(JSContext *ctx, JSValueConst slots, const char *name)
{
    JSValue err = an_dom_exception(ctx, "AbortError", "The animation was aborted"), p = sget(ctx, slots, name);

    an_settle(ctx, slots, name, false, err);
    JS_MarkPromiseHandled(ctx, p);
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, err);
}

/* ---- §4.5 and §4.6 derived quantities ---------------------------------------------------------------------- */

static JSValue an_timeline(JSContext *ctx, JSValueConst slots) { return sget(ctx, slots, "timeline"); }

/* The current time of the associated timeline, null for no timeline or an inactive one. */
static JSValue an_timeline_time(JSContext *ctx, JSValueConst slots)
{
    JSValue tl = an_timeline(ctx, slots), t;

    if (JS_IsNull(tl)) return JS_NULL;
    t = animation_timeline_current_time(ctx, tl);
    JS_FreeValue(ctx, tl);
    return t;
}

static bool an_has_pending_rate(JSContext *ctx, JSValueConst slots)
{
    JSValue v = sget(ctx, slots, "pendingRate");
    bool has = !JS_IsUndefined(v);

    JS_FreeValue(ctx, v);
    return has;
}

/* §4.5.15.2's effective playback rate. OWNED. */
static JSValue an_effective_rate(JSContext *ctx, JSValueConst slots)
{
    return sget(ctx, slots, an_has_pending_rate(ctx, slots) ? "pendingRate" : "rate");
}

/* §4.5.15.2 apply any pending playback rate. */
static void an_apply_pending_rate(JSContext *ctx, JSValueConst slots)
{
    if (!an_has_pending_rate(ctx, slots)) return;
    sset(ctx, slots, "rate", sget(ctx, slots, "pendingRate"));
    sset(ctx, slots, "pendingRate", JS_UNDEFINED);
}

/* §4.5.1 calculating the current time; `use_hold` false is §4.5.12 step 1's substitution of an unresolved hold
   time. OWNED. */
static JSValue an_current_time_of(JSContext *ctx, JSValueConst slots, bool use_hold)
{
    JSValue hold = sget(ctx, slots, "hold"), start, tt, rate, d, t;

    if (use_hold && animation_time_resolved(hold)) return hold;
    JS_FreeValue(ctx, hold);
    tt = an_timeline_time(ctx, slots);
    start = sget(ctx, slots, "start");
    if (!animation_time_resolved(tt) || !animation_time_resolved(start)) {
        JS_FreeValue(ctx, tt);
        JS_FreeValue(ctx, start);
        return JS_NULL;
    }
    rate = sget(ctx, slots, "rate");
    d = animation_time_sub(ctx, tt, start);
    t = animation_time_mul(ctx, d, rate);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, rate);
    JS_FreeValue(ctx, start);
    JS_FreeValue(ctx, tt);
    return t;
}

static JSValue an_current_time(JSContext *ctx, JSValueConst slots) { return an_current_time_of(ctx, slots, true); }

/* A number field of the effect timing record. OWNED. */
static JSValue fx(JSContext *ctx, JSValueConst effect, const char *k)
{
    JSValue v = JS_GetPropertyStr(ctx, effect, k);

    DCHECK(JS_IsNumber(v), "an associated effect's timing record holds no number — its producer writes every field");
    return v;
}

/* §4.6.5.2's active duration: zero when either factor is. OWNED. */
static JSValue an_active_duration(JSContext *ctx, JSValueConst effect)
{
    JSValue dur = fx(ctx, effect, "duration"), it = fx(ctx, effect, "iterations"), r;

    r = (tv_is0(ctx, dur) || tv_is0(ctx, it)) ? zero(ctx) : animation_time_mul(ctx, dur, it);
    JS_FreeValue(ctx, dur);
    JS_FreeValue(ctx, it);
    return r;
}

/* §4.6.5.3's end time, max(start delay + active duration + end delay, 0). OWNED. */
static JSValue an_end_time(JSContext *ctx, JSValueConst effect)
{
    JSValue delay = fx(ctx, effect, "delay"), end_delay = fx(ctx, effect, "endDelay");
    JSValue active = an_active_duration(ctx, effect), a = animation_time_add(ctx, delay, active);
    JSValue b = animation_time_add(ctx, a, end_delay), z = zero(ctx), r = animation_time_max(ctx, b, z);

    JS_FreeValue(ctx, b);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, active);
    JS_FreeValue(ctx, end_delay);
    JS_FreeValue(ctx, delay);
    return r;
}

/* §4.5.12's associated effect end: the effect's end time, or zero with no effect. OWNED. */
static JSValue an_effect_end(JSContext *ctx, JSValueConst slots)
{
    JSValue effect = sget(ctx, slots, "effect"), r;

    r = JS_IsNull(effect) ? zero(ctx) : an_end_time(ctx, effect);
    JS_FreeValue(ctx, effect);
    return r;
}

static bool an_effect_end_infinite(JSContext *ctx, JSValueConst slots)
{
    JSValue end = an_effect_end(ctx, slots);
    double e = 0;
    bool inf;

    DCHECK(JS_IsNumber(end), "an associated effect end derived from unknown input — CSS supplies its timing");
    JS_ToFloat64(ctx, &e, end);
    inf = isinf(e) && e > 0;
    JS_FreeValue(ctx, end);
    return inf;
}

static int an_task(JSContext *ctx, JSValueConst slots) { return sint(ctx, slots, "task"); }

/* §4.5.17's play state. */
static AnPlayState an_play_state(JSContext *ctx, JSValueConst slots)
{
    JSValue cur = an_current_time(ctx, slots), rate, end;
    int task = an_task(ctx, slots);
    bool start = sresolved(ctx, slots, "start"), finished = false;

    if (!animation_time_resolved(cur) && !start && task == AN_TASK_NONE) return AN_IDLE;
    if (task == AN_TASK_PAUSE || (!start && task != AN_TASK_PLAY)) { JS_FreeValue(ctx, cur); return AN_PAUSED; }
    if (animation_time_resolved(cur)) {
        rate = an_effective_rate(ctx, slots);
        end = an_effect_end(ctx, slots);
        finished = (tv_gt0(ctx, rate) && animation_time_le(ctx, end, cur)) ||
                   (tv_lt0(ctx, rate) && !tv_gt0(ctx, cur));
        JS_FreeValue(ctx, end);
        JS_FreeValue(ctx, rate);
    }
    JS_FreeValue(ctx, cur);
    return finished ? AN_FINISHED : AN_RUNNING;
}

const char *animation_play_state(JSContext *ctx, JSValueConst animation)
{
    JSValue slots = animation_slots(ctx, animation);
    AnPlayState s = an_play_state(ctx, slots);

    JS_FreeValue(ctx, slots);
    return PLAY_STATE[s];
}

/* ---- §4.5.12 updating the finished state ---------------------------------------------------------------- */

static void an_finish_notify_queue(JSContext *ctx, JSValueConst animation, JSValueConst slots);
static void an_finish_notification_steps(JSContext *ctx, JSValueConst animation, JSValueConst slots);

static void an_update_finished_state(JSContext *ctx, JSValueConst animation, JSValueConst slots, bool did_seek,
                                     bool sync_notify)
{
    JSValue un = an_current_time_of(ctx, slots, did_seek), rate, end, prev, hold, cur;
    bool finished;

    /* Step 2. */
    if (animation_time_resolved(un) && sresolved(ctx, slots, "start") && an_task(ctx, slots) == AN_TASK_NONE) {
        rate = sget(ctx, slots, "rate");
        end = an_effect_end(ctx, slots);
        prev = sget(ctx, slots, "previous");
        if (tv_gt0(ctx, rate) && animation_time_le(ctx, end, un)) {
            if (did_seek) hold = JS_DupValue(ctx, un);
            else hold = animation_time_resolved(prev) ? animation_time_max(ctx, prev, end) : JS_DupValue(ctx, end);
            sset(ctx, slots, "hold", hold);
        } else if (tv_lt0(ctx, rate) && !tv_gt0(ctx, un)) {
            JSValue z = zero(ctx);

            if (did_seek) hold = JS_DupValue(ctx, un);
            else hold = animation_time_resolved(prev) ? animation_time_min(ctx, prev, z) : z;
            sset(ctx, slots, "hold", hold);
        } else if (!tv_is0(ctx, rate)) {
            JSValue tt = an_timeline_time(ctx, slots);

            if (animation_time_resolved(tt)) {
                hold = sget(ctx, slots, "hold");
                if (did_seek && animation_time_resolved(hold)) {
                    JSValue q = animation_time_div(ctx, hold, rate);

                    sset(ctx, slots, "start", animation_time_sub(ctx, tt, q));
                    JS_FreeValue(ctx, q);
                }
                JS_FreeValue(ctx, hold);
                sset(ctx, slots, "hold", JS_NULL);
            }
            JS_FreeValue(ctx, tt);
        }
        JS_FreeValue(ctx, prev);
        JS_FreeValue(ctx, end);
        JS_FreeValue(ctx, rate);
    }
    JS_FreeValue(ctx, un);
    /* Step 3. */
    cur = an_current_time(ctx, slots);
    sset(ctx, slots, "previous", cur);
    /* Steps 4-6. */
    finished = an_play_state(ctx, slots) == AN_FINISHED;
    if (finished && !an_settled(ctx, slots, "finished")) {
        if (sync_notify) {
            sset(ctx, slots, "notify", JS_NewInt32(ctx, 0));   /* cancels any queued microtask's run */
            an_finish_notification_steps(ctx, animation, slots);
        } else if (sint(ctx, slots, "notify") == 0) {
            an_finish_notify_queue(ctx, animation, slots);
        }
    }
    if (!finished && an_settled(ctx, slots, "finished")) an_new_promise(ctx, slots, "finished");
    /* CSS Animations Level 2 §2.2: a CSS animation disassociated from its owning element joins the global
       animation list "at the first moment when they transition out of the idle play state". */
    if (sint(ctx, slots, "listed") == 0 && !css_animations_owned(ctx, animation) &&
        an_play_state(ctx, slots) != AN_IDLE)
        sset(ctx, slots, "listed", JS_NewInt32(ctx, (int32_t)document_animations_list_append(ctx)));
}

/* Step 5's finish notification steps. */
static void an_finish_notification_steps(JSContext *ctx, JSValueConst animation, JSValueConst slots)
{
    JSValue cur, tt, tl, ev, record, end, when;

    if (an_play_state(ctx, slots) != AN_FINISHED) return;
    an_settle(ctx, slots, "finished", true, animation);
    cur = an_current_time(ctx, slots);
    tt = an_timeline_time(ctx, slots);
    ev = animation_playback_event_new(ctx, "finish", cur, tt);
    JS_FreeValue(ctx, tt);
    JS_FreeValue(ctx, cur);
    tl = an_timeline(ctx, slots);
    if (JS_IsNull(tl)) {
        /* No document for timing: "queue a task to dispatch finishEvent at animation" on the DOM manipulation
           task source. */
        event_target_fire(ctx, animation, ev, JS_UNDEFINED, TASK_SOURCE_DOM_MANIPULATION);
        return;
    }
    record = animation_timeline_document_record(ctx, tl);
    end = an_effect_end(ctx, slots);
    when = animation_timeline_to_origin_relative(ctx, tl, end);
    document_animations_append_event(ctx, record, ev, animation, when, animation);
    JS_FreeValue(ctx, when);
    JS_FreeValue(ctx, end);
    JS_FreeValue(ctx, record);
    JS_FreeValue(ctx, tl);
}

/* The microtask the steps run in: argv[0] is the animation, argv[1] the token it was queued under. A token the
   slot no longer holds is a run the synchronous arm cancelled. */
static JSValue js_finish_notify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue slots;
    int32_t token = 0;

    (void)this_val;
    DCHECK(argc == 2 && animation_is(ctx, argv[0]), "the finish notification microtask was queued without its animation");
    slots = animation_slots(ctx, argv[0]);
    JS_ToInt32(ctx, &token, argv[1]);
    if (token != 0 && sint(ctx, slots, "notify") == token) {
        sset(ctx, slots, "notify", JS_NewInt32(ctx, 0));
        an_finish_notification_steps(ctx, argv[0], slots);
    }
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* The record whose §4.4 step 1 is running, or JS_UNDEFINED: while it is set, a finish notification microtask is
   step 3's checkpoint work and is held on the record for animation_run_held_finish_notifications. It is scoped to
   one synchronous call that holds the thread. */
static JSValue g_step1_record = JS_UNDEFINED;

void animation_hold_finish_notifications(JSValueConst record)
{
    DCHECK(JS_IsUndefined(record) != JS_IsUndefined(g_step1_record),
           "§4.4 step 1's finish notification hold was opened twice or closed while not open");
    g_step1_record = (JSValue)record;
}

void animation_run_held_finish_notifications(JSContext *ctx, JSValueConst record)
{
    JSValue held = JS_GetPropertyStr(ctx, record, "heldNotify"), len;
    uint32_t i, n = 0;

    DCHECK(JS_IsArray(held), "a Document's record holds no list of held finish notifications");
    len = JS_GetPropertyStr(ctx, held, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_SetPropertyStr(ctx, record, "heldNotify", JS_NewArray(ctx));
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, held, i), args[2];

        args[0] = JS_GetPropertyUint32(ctx, e, 0);
        args[1] = JS_GetPropertyUint32(ctx, e, 1);
        JS_FreeValue(ctx, js_finish_notify(ctx, JS_UNDEFINED, 2, args));
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, held);
}

static void an_finish_notify_queue(JSContext *ctx, JSValueConst animation, JSValueConst slots)
{
    JSValue fn, args[2];
    int32_t token = sint(ctx, slots, "notifySeq") + 1;

    sset(ctx, slots, "notifySeq", JS_NewInt32(ctx, token));
    sset(ctx, slots, "notify", JS_NewInt32(ctx, token));
    if (!JS_IsUndefined(g_step1_record)) {
        JSValue held = JS_GetPropertyStr(ctx, g_step1_record, "heldNotify"), e = JS_NewArray(ctx), len;
        uint32_t n = 0;

        len = JS_GetPropertyStr(ctx, held, "length");
        JS_ToUint32(ctx, &n, len);
        JS_FreeValue(ctx, len);
        JS_SetPropertyUint32(ctx, e, 0, JS_DupValue(ctx, animation));
        JS_SetPropertyUint32(ctx, e, 1, JS_NewInt32(ctx, token));
        JS_SetPropertyUint32(ctx, held, n, e);
        JS_FreeValue(ctx, held);
        return;
    }
    fn = realm_value_get(ctx, g_notify_slot);
    DCHECK(JS_IsFunction(ctx, fn), "a realm has no finish notification function — animation_install_realm builds it");
    args[0] = (JSValue)animation;
    args[1] = JS_NewInt32(ctx, token);
    JS_EnqueueCallJob(ctx, fn, 2, args);
    JS_FreeValue(ctx, fn);
}

/* ---- §4.5 procedures --------------------------------------------------------------------------------------- */

/* §4.5.2 set the timeline of an animation. */
static void an_set_timeline(JSContext *ctx, JSValueConst animation, JSValueConst slots, JSValueConst timeline)
{
    JSValue old = an_timeline(ctx, slots);

    if (JS_VALUE_GET_PTR(old) == JS_VALUE_GET_PTR(timeline) && JS_IsNull(old) == JS_IsNull(timeline)) {
        JS_FreeValue(ctx, old);
        return;
    }
    if (!JS_IsNull(old)) animation_timeline_dissociate(ctx, old, animation);
    JS_FreeValue(ctx, old);
    sset(ctx, slots, "timeline", JS_DupValue(ctx, timeline));
    if (!JS_IsNull(timeline)) animation_timeline_associate(ctx, timeline, animation);
    if (sresolved(ctx, slots, "start")) sset(ctx, slots, "hold", JS_NULL);
    an_update_finished_state(ctx, animation, slots, false, false);
}

/* §4.5.4 silently set the current time. False with a TypeError pending where step 1.1 throws. */
static bool an_silently_set_current_time(JSContext *ctx, JSValueConst slots, JSValueConst seek)
{
    JSValue tt, rate;

    if (!animation_time_resolved(seek)) {
        JSValue cur = an_current_time(ctx, slots);
        bool resolved = animation_time_resolved(cur);

        JS_FreeValue(ctx, cur);
        if (resolved) { JS_ThrowTypeError(ctx, "Failed to set the 'currentTime' property: it cannot be null"); return false; }
        return true;
    }
    tt = an_timeline_time(ctx, slots);
    rate = sget(ctx, slots, "rate");
    if (sresolved(ctx, slots, "hold") || !sresolved(ctx, slots, "start") || !animation_time_resolved(tt) ||
        tv_is0(ctx, rate)) {
        sset(ctx, slots, "hold", JS_DupValue(ctx, seek));
    } else {
        JSValue q = animation_time_div(ctx, seek, rate);

        sset(ctx, slots, "start", animation_time_sub(ctx, tt, q));
        JS_FreeValue(ctx, q);
    }
    if (!animation_time_resolved(tt)) sset(ctx, slots, "start", JS_NULL);
    sset(ctx, slots, "previous", JS_NULL);
    JS_FreeValue(ctx, rate);
    JS_FreeValue(ctx, tt);
    return true;
}

/* §4.5.4 set the current time. */
static bool an_set_current_time(JSContext *ctx, JSValueConst animation, JSValueConst slots, JSValueConst seek)
{
    if (!an_silently_set_current_time(ctx, slots, seek)) return false;
    if (an_task(ctx, slots) == AN_TASK_PAUSE) {
        sset(ctx, slots, "hold", JS_DupValue(ctx, seek));
        an_apply_pending_rate(ctx, slots);
        sset(ctx, slots, "start", JS_NULL);
        sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
        an_settle(ctx, slots, "ready", true, animation);
    }
    an_update_finished_state(ctx, animation, slots, true, false);
    return true;
}

/* §4.5.5 set the start time. */
static void an_set_start_time(JSContext *ctx, JSValueConst animation, JSValueConst slots, JSValueConst start)
{
    JSValue tt = an_timeline_time(ctx, slots), prev, rate;

    if (!animation_time_resolved(tt) && animation_time_resolved(start)) sset(ctx, slots, "hold", JS_NULL);
    prev = an_current_time(ctx, slots);
    an_apply_pending_rate(ctx, slots);
    sset(ctx, slots, "start", JS_DupValue(ctx, start));
    if (animation_time_resolved(start)) {
        rate = sget(ctx, slots, "rate");
        if (!tv_is0(ctx, rate)) sset(ctx, slots, "hold", JS_NULL);
        JS_FreeValue(ctx, rate);
        JS_FreeValue(ctx, prev);
    } else {
        sset(ctx, slots, "hold", prev);
    }
    if (an_task(ctx, slots) != AN_TASK_NONE) {
        sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
        an_settle(ctx, slots, "ready", true, animation);
    }
    an_update_finished_state(ctx, animation, slots, true, false);
    JS_FreeValue(ctx, tt);
}

/* §4.5.8 play an animation. False with an "InvalidStateError" pending where step 4 throws. */
static bool an_play(JSContext *ctx, JSValueConst animation, JSValueConst slots, bool auto_rewind)
{
    bool aborted_pause = an_task(ctx, slots) == AN_TASK_PAUSE, pending_ready = false;
    JSValue seek = JS_NULL, cur = an_current_time(ctx, slots), rate = an_effective_rate(ctx, slots);
    JSValue end = an_effect_end(ctx, slots);

    if (auto_rewind) {
        bool resolved = animation_time_resolved(cur);

        if (!tv_lt0(ctx, rate) &&
            (!resolved || tv_lt0(ctx, cur) || animation_time_le(ctx, end, cur))) {
            seek = zero(ctx);
        } else if (tv_lt0(ctx, rate) && (!resolved || !tv_gt0(ctx, cur) || animation_time_lt(ctx, end, cur))) {
            if (an_effect_end_infinite(ctx, slots)) {
                JS_FreeValue(ctx, end); JS_FreeValue(ctx, rate); JS_FreeValue(ctx, cur);
                JS_ThrowDOMException(ctx, "InvalidStateError",
                                     "Cannot play a reversed animation with an infinite associated effect end");
                return false;
            }
            seek = JS_DupValue(ctx, end);
        }
    }
    if (!animation_time_resolved(seek) && !sresolved(ctx, slots, "start") && !animation_time_resolved(cur))
        seek = zero(ctx);
    /* Steps 6-7: a document timeline is monotonically increasing, so has finite timeline is false. */
    if (animation_time_resolved(seek)) sset(ctx, slots, "hold", JS_DupValue(ctx, seek));
    if (sresolved(ctx, slots, "hold")) sset(ctx, slots, "start", JS_NULL);
    if (an_task(ctx, slots) != AN_TASK_NONE) {
        sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
        pending_ready = true;
    }
    JS_FreeValue(ctx, end);
    JS_FreeValue(ctx, rate);
    JS_FreeValue(ctx, cur);
    if (!sresolved(ctx, slots, "hold") && !animation_time_resolved(seek) && !aborted_pause &&
        !an_has_pending_rate(ctx, slots))
        return true;
    JS_FreeValue(ctx, seek);
    if (!pending_ready) an_new_promise(ctx, slots, "ready");
    /* Step 13: the task runs when the animation is ready — animation_frame_service. */
    sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_PLAY));
    an_update_finished_state(ctx, animation, slots, false, false);
    return true;
}

/* §4.5.9 pause an animation. False with an "InvalidStateError" pending where step 5 throws. */
static bool an_pause(JSContext *ctx, JSValueConst animation, JSValueConst slots)
{
    JSValue cur, rate, seek = JS_NULL;
    bool pending_ready = false;

    if (an_task(ctx, slots) == AN_TASK_PAUSE || an_play_state(ctx, slots) == AN_PAUSED) return true;
    cur = an_current_time(ctx, slots);
    if (!animation_time_resolved(cur)) {
        rate = sget(ctx, slots, "rate");
        if (!tv_lt0(ctx, rate)) {
            seek = zero(ctx);
        } else if (an_effect_end_infinite(ctx, slots)) {
            JS_FreeValue(ctx, rate);
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "Cannot pause a reversed animation with an infinite associated effect end");
            return false;
        } else {
            seek = an_effect_end(ctx, slots);
        }
        JS_FreeValue(ctx, rate);
    }
    JS_FreeValue(ctx, cur);
    if (animation_time_resolved(seek)) sset(ctx, slots, "hold", seek);   /* has finite timeline is false */
    if (an_task(ctx, slots) == AN_TASK_PLAY) {
        sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
        pending_ready = true;
    }
    if (!pending_ready) an_new_promise(ctx, slots, "ready");
    sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_PAUSE));
    an_update_finished_state(ctx, animation, slots, false, false);
    return true;
}

/* §4.5.14 reset an animation's pending tasks. */
static void an_reset_pending_tasks(JSContext *ctx, JSValueConst animation, JSValueConst slots)
{
    if (an_task(ctx, slots) == AN_TASK_NONE) return;
    sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
    an_apply_pending_rate(ctx, slots);
    an_abort(ctx, slots, "ready");
    an_new_promise(ctx, slots, "ready");
    an_settle(ctx, slots, "ready", true, animation);
}

/* §4.5.14 cancel an animation. */
static void an_cancel(JSContext *ctx, JSValueConst animation, JSValueConst slots)
{
    if (an_play_state(ctx, slots) != AN_IDLE) {
        JSValue tl, tt = JS_NULL, ev, effect = sget(ctx, slots, "effect");
        double active = 0;

        /* The active time at the moment of canceling with a fill mode of both, which CSS Animations Level 2
           §5.1's `animationcancel` reports after the current time it derives from is gone. */
        if (JS_IsObject(effect) && animation_active_time(ctx, animation, true, &active))
            sset(ctx, slots, "cancelActiveTime", JS_NewFloat64(ctx, active));
        JS_FreeValue(ctx, effect);
        an_reset_pending_tasks(ctx, animation, slots);
        an_abort(ctx, slots, "finished");
        an_new_promise(ctx, slots, "finished");
        tl = an_timeline(ctx, slots);
        if (!JS_IsNull(tl)) tt = animation_timeline_current_time(ctx, tl);
        ev = animation_playback_event_new(ctx, "cancel", JS_NULL, tt);
        if (JS_IsNull(tl)) {
            event_target_fire(ctx, animation, ev, JS_UNDEFINED, TASK_SOURCE_DOM_MANIPULATION);
        } else {
            JSValue record = animation_timeline_document_record(ctx, tl);
            JSValue when = animation_time_resolved(tt) ? animation_timeline_to_origin_relative(ctx, tl, tt) : JS_NULL;

            document_animations_append_event(ctx, record, ev, animation, when, animation);
            JS_FreeValue(ctx, when);
            JS_FreeValue(ctx, record);
        }
        JS_FreeValue(ctx, tt);
        JS_FreeValue(ctx, tl);
    }
    sset(ctx, slots, "hold", JS_NULL);
    sset(ctx, slots, "start", JS_NULL);
}

/* §4.5.13 finish an animation. False with an "InvalidStateError" pending where step 1 throws. */
static bool an_finish(JSContext *ctx, JSValueConst animation, JSValueConst slots)
{
    JSValue rate = an_effective_rate(ctx, slots), limit, tt;
    bool refused = tv_is0(ctx, rate) || (tv_gt0(ctx, rate) && an_effect_end_infinite(ctx, slots));

    JS_FreeValue(ctx, rate);
    if (refused) {
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "Cannot finish an animation with a zero playback rate or an infinite associated effect end");
        return false;
    }
    an_apply_pending_rate(ctx, slots);
    rate = sget(ctx, slots, "rate");
    limit = tv_gt0(ctx, rate) ? an_effect_end(ctx, slots) : zero(ctx);
    CHECK(an_silently_set_current_time(ctx, slots, limit), "§4.5.13 seeks to a resolved limit, which cannot throw");
    tt = an_timeline_time(ctx, slots);
    if (!sresolved(ctx, slots, "start") && animation_time_resolved(tt)) {
        JSValue q = animation_time_div(ctx, limit, rate);

        sset(ctx, slots, "start", animation_time_sub(ctx, tt, q));
        JS_FreeValue(ctx, q);
    }
    if (an_task(ctx, slots) == AN_TASK_PAUSE && sresolved(ctx, slots, "start")) {
        sset(ctx, slots, "hold", JS_NULL);
        sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
        an_settle(ctx, slots, "ready", true, animation);
    }
    if (an_task(ctx, slots) == AN_TASK_PLAY && sresolved(ctx, slots, "start")) {
        sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
        an_settle(ctx, slots, "ready", true, animation);
    }
    an_update_finished_state(ctx, animation, slots, true, true);
    JS_FreeValue(ctx, tt);
    JS_FreeValue(ctx, limit);
    JS_FreeValue(ctx, rate);
    return true;
}

/* §4.5.15.1 set the playback rate. */
static void an_set_playback_rate(JSContext *ctx, JSValueConst animation, JSValueConst slots, JSValueConst rate)
{
    JSValue prev;

    sset(ctx, slots, "pendingRate", JS_UNDEFINED);
    prev = an_current_time(ctx, slots);
    sset(ctx, slots, "rate", JS_DupValue(ctx, rate));
    /* A document timeline is monotonically increasing, so the first arm is the one that can match. */
    {
        JSValue tl = an_timeline(ctx, slots);

        if (animation_time_resolved(prev) && !JS_IsNull(tl))
            CHECK(an_set_current_time(ctx, animation, slots, prev), "§4.5.15.1 seeks to a resolved time");
        JS_FreeValue(ctx, tl);
    }
    JS_FreeValue(ctx, prev);
    an_update_finished_state(ctx, animation, slots, true, false);
}

/* §4.5.15.2 seamlessly update the playback rate. */
static bool an_update_playback_rate(JSContext *ctx, JSValueConst animation, JSValueConst slots, JSValueConst rate)
{
    AnPlayState prev = an_play_state(ctx, slots);
    JSValue cur;
    bool unresolved;

    sset(ctx, slots, "pendingRate", JS_DupValue(ctx, rate));
    if (an_task(ctx, slots) != AN_TASK_NONE) return true;
    cur = an_current_time(ctx, slots);
    unresolved = !animation_time_resolved(cur);
    JS_FreeValue(ctx, cur);
    if (prev == AN_IDLE || prev == AN_PAUSED || unresolved) {
        an_apply_pending_rate(ctx, slots);
    } else if (prev == AN_FINISHED) {
        JSValue un = an_current_time_of(ctx, slots, false), tt = an_timeline_time(ctx, slots), start;

        if (tv_is0(ctx, rate)) {
            start = JS_DupValue(ctx, tt);
        } else {
            JSValue q = animation_time_div(ctx, un, rate);

            start = animation_time_sub(ctx, tt, q);
            JS_FreeValue(ctx, q);
        }
        sset(ctx, slots, "start", start);
        an_apply_pending_rate(ctx, slots);
        an_update_finished_state(ctx, animation, slots, false, false);
        JS_FreeValue(ctx, tt);
        JS_FreeValue(ctx, un);
    } else {
        return an_play(ctx, animation, slots, false);
    }
    return true;
}

/* §4.5.16 reverse an animation. */
static bool an_reverse(JSContext *ctx, JSValueConst animation, JSValueConst slots)
{
    JSValue tt = an_timeline_time(ctx, slots), original, rate;
    bool inactive = !animation_time_resolved(tt);

    JS_FreeValue(ctx, tt);
    if (inactive) {
        JS_ThrowDOMException(ctx, "InvalidStateError", "Cannot reverse an animation with no active timeline");
        return false;
    }
    original = sget(ctx, slots, "pendingRate");
    rate = an_effective_rate(ctx, slots);
    sset(ctx, slots, "pendingRate", animation_time_neg(ctx, rate));
    JS_FreeValue(ctx, rate);
    if (!an_play(ctx, animation, slots, true)) {
        sset(ctx, slots, "pendingRate", original);
        return false;
    }
    JS_FreeValue(ctx, original);
    return true;
}

/* ---- the update animations and send events half ----------------------------------------------------------- */

/* §4.5.8 step 13's task, at the moment the animation became ready. */
static void an_run_play_task(JSContext *ctx, JSValueConst animation, JSValueConst slots, JSValueConst ready)
{
    JSValue rate, start;

    DCHECK(sresolved(ctx, slots, "start") || sresolved(ctx, slots, "hold"),
           "§4.5.8 step 13.1: a pending play task ran with neither a start time nor a hold time resolved — step 7 "
           "sets one of the two before the task is scheduled");
    sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
    if (sresolved(ctx, slots, "hold")) {
        JSValue hold = sget(ctx, slots, "hold");

        an_apply_pending_rate(ctx, slots);
        rate = sget(ctx, slots, "rate");
        if (tv_is0(ctx, rate)) {
            start = JS_DupValue(ctx, ready);
        } else {
            JSValue q = animation_time_div(ctx, hold, rate);

            start = animation_time_sub(ctx, ready, q);
            JS_FreeValue(ctx, q);
        }
        sset(ctx, slots, "start", start);
        if (!tv_is0(ctx, rate)) sset(ctx, slots, "hold", JS_NULL);
        JS_FreeValue(ctx, rate);
        JS_FreeValue(ctx, hold);
    } else if (an_has_pending_rate(ctx, slots)) {
        JSValue s = sget(ctx, slots, "start"), d, match;

        rate = sget(ctx, slots, "rate");
        d = animation_time_sub(ctx, ready, s);
        match = animation_time_mul(ctx, d, rate);
        JS_FreeValue(ctx, d);
        JS_FreeValue(ctx, rate);
        an_apply_pending_rate(ctx, slots);
        rate = sget(ctx, slots, "rate");
        if (tv_is0(ctx, rate)) {
            sset(ctx, slots, "hold", JS_DupValue(ctx, match));
            start = JS_DupValue(ctx, ready);
        } else {
            JSValue q = animation_time_div(ctx, match, rate);

            start = animation_time_sub(ctx, ready, q);
            JS_FreeValue(ctx, q);
        }
        sset(ctx, slots, "start", start);
        JS_FreeValue(ctx, rate);
        JS_FreeValue(ctx, match);
        JS_FreeValue(ctx, s);
    }
    an_settle(ctx, slots, "ready", true, animation);
    an_update_finished_state(ctx, animation, slots, false, false);
}

/* §4.5.9's pending pause task, at the moment playback was suspended. */
static void an_run_pause_task(JSContext *ctx, JSValueConst animation, JSValueConst slots, JSValueConst ready)
{
    sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
    if (sresolved(ctx, slots, "start") && !sresolved(ctx, slots, "hold")) {
        JSValue s = sget(ctx, slots, "start"), rate = sget(ctx, slots, "rate"), d = animation_time_sub(ctx, ready, s);

        sset(ctx, slots, "hold", animation_time_mul(ctx, d, rate));
        JS_FreeValue(ctx, d);
        JS_FreeValue(ctx, rate);
        JS_FreeValue(ctx, s);
    }
    an_apply_pending_rate(ctx, slots);
    sset(ctx, slots, "start", JS_NULL);
    an_settle(ctx, slots, "ready", true, animation);
    an_update_finished_state(ctx, animation, slots, false, false);
}

void animation_frame_service(JSContext *ctx, JSValueConst animation)
{
    JSValue slots = animation_slots(ctx, animation), tt = an_timeline_time(ctx, slots);
    int task = an_task(ctx, slots);

    DCHECK(animation_time_resolved(tt), "an animation of a timeline just given a now timestamp reads no time");
    if (task == AN_TASK_PLAY) an_run_play_task(ctx, animation, slots, tt);
    else if (task == AN_TASK_PAUSE) an_run_pause_task(ctx, animation, slots, tt);
    else an_update_finished_state(ctx, animation, slots, false, false);
    JS_FreeValue(ctx, tt);
    JS_FreeValue(ctx, slots);
}

/* Named residual: a running animation whose end is infinite owes no frame, so CSS Animations' `animationiteration`
   for it fires only at frames something else asks for. The next diff makes an animation with an owning element
   whose iteration boundary lies ahead owe the frame that reaches it. Its absence shows as an infinite CSS
   animation's iteration events stopping on a document with no other rendering work. */
bool animation_needs_frame(JSContext *ctx, JSValueConst animation)
{
    JSValue slots = animation_slots(ctx, animation);
    bool needs = an_task(ctx, slots) != AN_TASK_NONE ||
                 (an_play_state(ctx, slots) == AN_RUNNING && !an_effect_end_infinite(ctx, slots));

    JS_FreeValue(ctx, slots);
    return needs;
}

/* ---- §4.6 phases, §4.6.7 relevance, §5.4.2 composite order, §5.5.2 replaceable ------------------------------ */

typedef enum { FX_NONE, FX_BEFORE, FX_ACTIVE, FX_AFTER } FxPhase;

/* §4.6.6's phase of the associated effect at local time `t` (the animation's current time). */
static FxPhase an_phase(JSContext *ctx, JSValueConst slots, JSValueConst effect, JSValueConst t)
{
    JSValue delay, active, end, z = zero(ctx), m, bab, aab, sum, rate;
    bool backwards;
    FxPhase p;

    if (!animation_time_resolved(t)) return FX_NONE;
    delay = fx(ctx, effect, "delay");
    active = an_active_duration(ctx, effect);
    end = an_end_time(ctx, effect);
    m = animation_time_min(ctx, delay, end);
    bab = animation_time_max(ctx, m, z);
    JS_FreeValue(ctx, m);
    sum = animation_time_add(ctx, delay, active);
    m = animation_time_min(ctx, sum, end);
    aab = animation_time_max(ctx, m, z);
    JS_FreeValue(ctx, m);
    rate = sget(ctx, slots, "rate");
    backwards = tv_lt0(ctx, rate);
    if (animation_time_lt(ctx, t, bab) || (backwards && animation_time_eq(ctx, t, bab))) p = FX_BEFORE;
    else if (animation_time_lt(ctx, aab, t) || (!backwards && animation_time_eq(ctx, t, aab))) p = FX_AFTER;
    else p = FX_ACTIVE;
    JS_FreeValue(ctx, rate);
    JS_FreeValue(ctx, sum);
    JS_FreeValue(ctx, aab);
    JS_FreeValue(ctx, bab);
    JS_FreeValue(ctx, end);
    JS_FreeValue(ctx, active);
    JS_FreeValue(ctx, delay);
    return p;
}

static bool fx_fills(JSContext *ctx, JSValueConst effect, const char *mode)
{
    JSValue v = JS_GetPropertyStr(ctx, effect, "fill");
    const char *s = JS_ToCString(ctx, v);
    bool fills;

    DCHECK(s != NULL, "an associated effect's timing record holds no fill mode");
    fills = strcmp(s, mode) == 0 || strcmp(s, "both") == 0;
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return fills;
}

bool animation_relevant(JSContext *ctx, JSValueConst animation)
{
    JSValue slots = animation_slots(ctx, animation), effect = sget(ctx, slots, "effect"), t, rate;
    bool current = false, in_effect = false;
    FxPhase p;

    if (JS_IsNull(effect) || sint(ctx, slots, "replace") == AN_REPLACE_REMOVED) {
        JS_FreeValue(ctx, effect);
        JS_FreeValue(ctx, slots);
        return false;
    }
    t = an_current_time(ctx, slots);
    p = an_phase(ctx, slots, effect, t);
    rate = sget(ctx, slots, "rate");
    /* §4.6.6 current: in play, or before and playing forwards, or after and playing backwards. The fourth arm
       needs a timeline that is not monotonically increasing, and a document timeline always is. */
    current = (p == FX_ACTIVE && an_play_state(ctx, slots) != AN_FINISHED) ||
              (p == FX_BEFORE && tv_gt0(ctx, rate)) || (p == FX_AFTER && tv_lt0(ctx, rate));
    /* §4.7.2: the active time is resolved in the active phase, and in a filled before or after phase. */
    in_effect = p == FX_ACTIVE || (p == FX_BEFORE && fx_fills(ctx, effect, "backwards")) ||
                (p == FX_AFTER && fx_fills(ctx, effect, "forwards"));
    JS_FreeValue(ctx, rate);
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, effect);
    JS_FreeValue(ctx, slots);
    return current || in_effect;
}

/* A resolved time value as the number CSS Animations' event table computes over. */
static double an_number(JSContext *ctx, JSValueConst v)
{
    double d = 0;

    if (!JS_IsNumber(v))
        DFAIL("CSS Animations Level 2 §5.1's event table was asked over a time value that is unknown external "
              "input — build the table's comparisons as forks in update the rendering's step 11 machine");
    JS_ToFloat64(ctx, &d, v);
    return d;
}

double animation_effect_number(JSContext *ctx, JSValueConst animation, const char *name)
{
    JSValue slots = animation_slots(ctx, animation), effect = sget(ctx, slots, "effect"), v;
    double d;

    DCHECK(JS_IsObject(effect), "an effect timing number was asked of an animation with no associated effect");
    if (strcmp(name, "activeDuration") == 0) v = an_active_duration(ctx, effect);
    else if (strcmp(name, "endTime") == 0) v = an_end_time(ctx, effect);
    else v = fx(ctx, effect, name);
    d = an_number(ctx, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, effect);
    JS_FreeValue(ctx, slots);
    return d;
}

AnimationPhase animation_effect_phase(JSContext *ctx, JSValueConst animation)
{
    JSValue slots = animation_slots(ctx, animation), effect = sget(ctx, slots, "effect"), t;
    FxPhase p;

    DCHECK(JS_IsObject(effect), "an effect phase was asked of an animation with no associated effect");
    t = an_current_time(ctx, slots);
    p = an_phase(ctx, slots, effect, t);
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, effect);
    JS_FreeValue(ctx, slots);
    return p == FX_BEFORE ? ANIMATION_PHASE_BEFORE : p == FX_ACTIVE ? ANIMATION_PHASE_ACTIVE
         : p == FX_AFTER ? ANIMATION_PHASE_AFTER : ANIMATION_PHASE_IDLE;
}

bool animation_active_time(JSContext *ctx, JSValueConst animation, bool fill_both, double *out)
{
    JSValue slots = animation_slots(ctx, animation), effect = sget(ctx, slots, "effect"), t;
    AnimationPhase p = animation_effect_phase(ctx, animation);
    double local, delay, active;
    bool resolved;

    if (p == ANIMATION_PHASE_IDLE) {
        JS_FreeValue(ctx, effect);
        JS_FreeValue(ctx, slots);
        return false;
    }
    t = an_current_time(ctx, slots);
    local = an_number(ctx, t);
    delay = animation_effect_number(ctx, animation, "delay");
    active = animation_effect_number(ctx, animation, "activeDuration");
    /* §4.7.2's three arms. */
    if (p == ANIMATION_PHASE_BEFORE) {
        resolved = fill_both || fx_fills(ctx, effect, "backwards");
        *out = fmax(local - delay, 0);
    } else if (p == ANIMATION_PHASE_ACTIVE) {
        resolved = true;
        *out = local - delay;
    } else {
        resolved = fill_both || fx_fills(ctx, effect, "forwards");
        *out = fmax(fmin(local - delay, active), 0);
    }
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, effect);
    JS_FreeValue(ctx, slots);
    return resolved;
}

bool animation_current_iteration(JSContext *ctx, JSValueConst animation, double *out)
{
    AnimationPhase p = animation_effect_phase(ctx, animation);
    double active, duration, iterations, start, overall, simple;

    if (!animation_active_time(ctx, animation, false, &active)) return false;
    duration = animation_effect_number(ctx, animation, "duration");
    iterations = animation_effect_number(ctx, animation, "iterations");
    start = animation_effect_number(ctx, animation, "iterationStart");
    if (p == ANIMATION_PHASE_AFTER && isinf(iterations)) { *out = INFINITY; return true; }
    /* §4.7.3 overall progress. */
    overall = duration == 0 ? (p == ANIMATION_PHASE_BEFORE ? 0 : iterations) : active / duration;
    overall += start;
    /* §4.7.4 simple iteration progress. */
    simple = isinf(overall) ? fmod(start, 1.0) : fmod(overall, 1.0);
    if (simple == 0 && (p == ANIMATION_PHASE_ACTIVE || p == ANIMATION_PHASE_AFTER) &&
        active == animation_effect_number(ctx, animation, "activeDuration") && iterations != 0)
        simple = 1.0;
    /* §4.7.5. */
    *out = simple == 1.0 ? floor(overall) - 1 : floor(overall);
    return true;
}

bool animation_replaceable(JSContext *ctx, JSValueConst animation)
{
    JSValue slots = animation_slots(ctx, animation), effect = sget(ctx, slots, "effect"), tl = an_timeline(ctx, slots), target;
    double active;
    bool r;

    /* §5.5.2's conditions in its order. Every timeline here is a document timeline, which §4.3.1 makes
       monotonically increasing, so "associated with a monotonically increasing timeline" is having one. */
    DCHECK(JS_IsNull(tl) || animation_timeline_is(ctx, tl), "an animation's timeline slot holds no AnimationTimeline");
    r = !css_animations_has_owning_element(ctx, animation) && an_play_state(ctx, slots) == AN_FINISHED &&
        sint(ctx, slots, "replace") != AN_REPLACE_REMOVED && !JS_IsNull(tl) && !JS_IsNull(effect) &&
        animation_active_time(ctx, animation, false, &active);
    if (r) {
        target = JS_GetPropertyStr(ctx, effect, "target");
        r = JS_IsObject(target);
        JS_FreeValue(ctx, target);
    }
    JS_FreeValue(ctx, tl);
    JS_FreeValue(ctx, effect);
    JS_FreeValue(ctx, slots);
    return r;
}

int animation_composite_order(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    bool ca = css_animations_owned(ctx, a), cb = css_animations_owned(ctx, b);
    JSValue sa, sb;
    int pa, pb;

    /* §5.4.2's inter-class order: CSS Animations Level 2 §2.2 puts a CSS animation with an owning element
       before every animation without a specific class, and orders the class itself. */
    if (ca != cb) return ca ? -1 : 1;
    if (ca) return css_animations_order(ctx, a, b);
    sa = animation_slots(ctx, a);
    sb = animation_slots(ctx, b);
    pa = sint(ctx, sa, "listed");
    pb = sint(ctx, sb, "listed");
    DCHECK(pa > 0 && pb > 0, "§5.4.2 sorted an animation that has no position in the global animation list — CSS "
                             "Animations Level 2 §2.2 gives one to every animation not ordered by an owning element");
    JS_FreeValue(ctx, sa);
    JS_FreeValue(ctx, sb);
    return pa < pb ? -1 : pa > pb;
}

/* ---- creation ---------------------------------------------------------------------------------------------- */

JSValue animation_create(JSContext *ctx, JSValueConst proto, JSValueConst timeline, JSValueConst effect, bool listed)
{
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, g_class), slots;
    JSAtom k;

    CHECK(!JS_IsException(obj), "an Animation could not be allocated");
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "an Animation's internal slots could not be allocated");
    sset(ctx, slots, "id", JS_NewString(ctx, ""));
    sset(ctx, slots, "timeline", JS_NULL);
    sset(ctx, slots, "effect", JS_DupValue(ctx, effect));
    sset(ctx, slots, "start", JS_NULL);
    sset(ctx, slots, "hold", JS_NULL);
    sset(ctx, slots, "previous", JS_NULL);
    sset(ctx, slots, "rate", JS_NewInt32(ctx, 1));       /* §4.5.15: "The playback rate is initially 1." */
    sset(ctx, slots, "pendingRate", JS_UNDEFINED);
    sset(ctx, slots, "task", JS_NewInt32(ctx, AN_TASK_NONE));
    sset(ctx, slots, "notify", JS_NewInt32(ctx, 0));
    sset(ctx, slots, "notifySeq", JS_NewInt32(ctx, 0));
    sset(ctx, slots, "replace", JS_NewInt32(ctx, AN_REPLACE_ACTIVE));
    sset(ctx, slots, "listed", JS_NewInt32(ctx, listed ? (int32_t)document_animations_list_append(ctx) : 0));
    an_new_promise(ctx, slots, "finished");
    an_new_promise(ctx, slots, "ready");
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the Animation slot key could not be reached");
    JS_DefinePropertyValue(ctx, obj, k, JS_DupValue(ctx, slots), 0);
    JS_FreeAtom(ctx, k);
    /* §4.5.7: the current ready promise is initially resolved with the animation itself. */
    an_settle(ctx, slots, "ready", true, obj);
    an_set_timeline(ctx, obj, slots, timeline);
    JS_FreeValue(ctx, slots);
    return obj;
}

bool animation_play(JSContext *ctx, JSValueConst animation, bool auto_rewind)
{
    JSValue slots = animation_slots(ctx, animation);
    bool ok = an_play(ctx, animation, slots, auto_rewind);

    JS_FreeValue(ctx, slots);
    return ok;
}

bool animation_pause(JSContext *ctx, JSValueConst animation)
{
    JSValue slots = animation_slots(ctx, animation);
    bool ok = an_pause(ctx, animation, slots);

    JS_FreeValue(ctx, slots);
    return ok;
}

void animation_cancel(JSContext *ctx, JSValueConst animation)
{
    JSValue slots = animation_slots(ctx, animation);

    an_cancel(ctx, animation, slots);
    JS_FreeValue(ctx, slots);
}

/* ---- §6.4 members ----------------------------------------------------------------------------------------- */

static JSValue this_slots(JSContext *ctx, JSValueConst this_val, const char *member)
{
    if (!animation_is(ctx, this_val)) {
        JS_ThrowTypeError(ctx, "'%s' called on an object that is not an Animation", member);
        return JS_EXCEPTION;
    }
    return animation_slots(ctx, this_val);
}

enum { G_ID, G_TIMELINE, G_START, G_CURRENT, G_RATE, G_PLAY_STATE, G_REPLACE_STATE, G_PENDING, G_READY, G_FINISHED };
static const char *const GETTER_NAME[] = { "id", "timeline", "startTime", "currentTime", "playbackRate",
                                           "playState", "replaceState", "pending", "ready", "finished" };

static JSValue js_an_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = this_slots(ctx, this_val, GETTER_NAME[magic]), r;

    if (JS_IsException(slots)) return slots;
    switch (magic) {
    case G_ID:            r = sget(ctx, slots, "id"); break;
    case G_TIMELINE:      r = an_timeline(ctx, slots); break;
    case G_START:         r = sget(ctx, slots, "start"); break;
    case G_CURRENT:       r = an_current_time(ctx, slots); break;
    case G_RATE:          r = sget(ctx, slots, "rate"); break;
    case G_PLAY_STATE:    r = JS_NewString(ctx, PLAY_STATE[an_play_state(ctx, slots)]); break;
    case G_REPLACE_STATE: r = JS_NewString(ctx, REPLACE_STATE[sint(ctx, slots, "replace")]); break;
    case G_PENDING:       r = JS_NewBool(ctx, an_task(ctx, slots) != AN_TASK_NONE); break;
    case G_READY:         r = sget(ctx, slots, "ready"); break;
    case G_FINISHED:      r = sget(ctx, slots, "finished"); break;
    default:              DFAIL("an Animation getter was installed with a magic its switch does not have");
                          r = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, slots);
    return r;
}

enum { S_ID, S_TIMELINE, S_START, S_CURRENT, S_RATE };

static JSValue js_an_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue slots = animation_slots(ctx, this_val);
    bool ok = true;

    switch (magic) {
    case S_ID:       sset(ctx, slots, "id", JS_DupValue(ctx, val)); break;
    case S_TIMELINE: an_set_timeline(ctx, this_val, slots, JS_IsUndefined(val) ? JS_NULL : val); break;
    case S_START:    an_set_start_time(ctx, this_val, slots, val); break;
    case S_CURRENT:  ok = an_set_current_time(ctx, this_val, slots, val); break;
    case S_RATE:     an_set_playback_rate(ctx, this_val, slots, val); break;
    default:         DFAIL("an Animation setter was installed with a magic its switch does not have");
    }
    JS_FreeValue(ctx, slots);
    return ok ? JS_UNDEFINED : JS_EXCEPTION;
}

enum { M_CANCEL, M_FINISH, M_PLAY, M_PAUSE, M_UPDATE_RATE, M_REVERSE, M_PERSIST };

static JSValue js_an_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots = animation_slots(ctx, this_val);
    bool ok = true;

    switch (magic) {
    case M_CANCEL:  an_cancel(ctx, this_val, slots); break;
    case M_FINISH:  ok = an_finish(ctx, this_val, slots); break;
    case M_PLAY:    ok = an_play(ctx, this_val, slots, true); break;
    case M_PAUSE:   ok = an_pause(ctx, this_val, slots); break;
    case M_UPDATE_RATE:
        DCHECK(argc >= 1, "updatePlaybackRate reached its body with no rate — §3.6 step 5's TypeError is the declaration's");
        ok = an_update_playback_rate(ctx, this_val, slots, argv[0]);
        break;
    case M_REVERSE: ok = an_reverse(ctx, this_val, slots); break;
    case M_PERSIST: sset(ctx, slots, "replace", JS_NewInt32(ctx, AN_REPLACE_PERSISTED)); break;
    default:        DFAIL("an Animation method was installed with a magic its switch does not have");
    }
    JS_FreeValue(ctx, slots);
    return ok ? JS_UNDEFINED : JS_EXCEPTION;
}

/* §6.4's constructor: set the timeline (the default document timeline of the current global's Document when the
   argument is missing), then set the associated effect. No object implements AnimationEffect in this build, so
   Web IDL §3.2.15 step 1 refuses every value but null and undefined for `effect`. */
static JSValue js_an_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue proto, tl, obj;

    (void)magic;
    if (JS_IsUndefined(this_val)) return JS_ThrowTypeError(ctx, "constructor Animation requires 'new'");
    if (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "Failed to construct 'Animation': parameter 1 is not of type 'AnimationEffect'");
    tl = idl_arg_given(argc, argv, 1) ? JS_DupValue(ctx, argv[1]) : animation_timeline_default(ctx);
    proto = JS_GetClassProto(ctx, g_class);
    obj = animation_create(ctx, proto, tl, JS_NULL, true);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, tl);
    return obj;
}

JSValue animation_proto(JSContext *ctx)
{
    JSValue p = JS_GetClassProto(ctx, g_class);

    DCHECK(!JS_IsNull(p), "Animation.prototype was asked for in a realm that never installed it");
    return p;
}

static void animation_install_realm(JSContext *ctx)
{
    JSValue proto, prev, fn;
    unsigned i;

    DCHECK(g_rt != NULL, "a realm asked for Animation.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "animation_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = event_target_derived_proto(ctx);   /* §6.4: `interface Animation : EventTarget` */
    idl_interface_tag(ctx, proto, "Animation");
    {
        static const int SETTER_OF[] = { S_ID, S_TIMELINE, S_START, S_CURRENT, S_RATE };
        const int setter_ids[] = { g_id_set_id, g_id_set_timeline, g_id_set_start, g_id_set_current, g_id_set_rate };

        for (i = 0; i < sizeof GETTER_NAME / sizeof GETTER_NAME[0]; i++)
            idl_install_accessor(ctx, proto, GETTER_NAME[i], js_an_get, (int)i,
                                 i < sizeof SETTER_OF / sizeof SETTER_OF[0] ? setter_ids[i] : -1);
    }
    event_target_install_handlers(ctx, proto, EH_ANIMATION);
    idl_install_method(ctx, proto, "cancel", g_id_cancel);
    idl_install_method(ctx, proto, "finish", g_id_finish);
    idl_install_method(ctx, proto, "play", g_id_play);
    idl_install_method(ctx, proto, "pause", g_id_pause);
    idl_install_method(ctx, proto, "updatePlaybackRate", g_id_update_rate);
    idl_install_method(ctx, proto, "reverse", g_id_reverse);
    idl_install_method(ctx, proto, "persist", g_id_persist);
    JS_SetClassProto(ctx, g_class, proto);
    fn = JS_NewCFunction(ctx, js_finish_notify, "finishNotificationSteps", 2);
    CHECK(!JS_IsException(fn), "the finish notification steps function could not be allocated");
    realm_value_set(ctx, g_notify_slot, fn);
}

void animation_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(g_rt != NULL, "Web Animations §6.4 was installed before it was declared");
    animation_timeline_install(ctx, global);
    ctor = idl_step_constructor(ctx, "Animation", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the Animation interface object could not be allocated");
    proto = animation_proto(ctx);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "Animation", ctor);
    animation_playback_event_install(ctx, global);
    css_animations_install(ctx, global);
    animation_event_install(ctx, global);
}

void animation_init(JSContext *ctx)
{
    static const IdlArgType CTOR_ARGS[2] = { IDL_ANY, IDL_INTERFACE_NULLABLE };
    static const IdlArgType RATE_ARGS[1] = { IDL_DOUBLE };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_rt == NULL, "animation_init ran twice — §6.4 is declared once per agent");
    /* A Document's record first (each timeline is associated with one), then §6.2-§6.3's timelines (the default
       one is built with the realm), this interface, and §6.12's event. All four declare under `animation`. */
    document_animations_init(ctx);
    animation_timeline_init(ctx);
    g_rt = rt;
    {
        JSClassDef def = { "Animation" };

        JS_NewClassID(rt, &g_class);
        CHECK(JS_NewClass(rt, g_class, &def) == 0, "Animation's prototype slot could not be declared");
    }
    g_key = JS_NewSymbol(ctx, "animationSlots", false);
    CHECK(!JS_IsException(g_key), "the Animation slot key could not be allocated");
    g_id_ctor = idl_method_id(ctx, CTOR_ARGS, 2, js_an_ctor, 0);
    idl_optional_from(0);   /* `optional AnimationEffect? effect = null, optional AnimationTimeline? timeline` */
    idl_arg_iface(1, animation_timeline_is, "AnimationTimeline");
    g_id_set_id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_an_set, S_ID);
    idl_this_iface(an_is_value, "Animation");
    g_id_set_timeline = idl_setter_id(ctx, IDL_INTERFACE_NULLABLE, false, js_an_set, S_TIMELINE);
    idl_arg_iface(0, animation_timeline_is, "AnimationTimeline");
    idl_this_iface(an_is_value, "Animation");
    g_id_set_start = idl_setter_id(ctx, IDL_DOUBLE_NULLABLE, false, js_an_set, S_START);
    idl_this_iface(an_is_value, "Animation");
    g_id_set_current = idl_setter_id(ctx, IDL_DOUBLE_NULLABLE, false, js_an_set, S_CURRENT);
    idl_this_iface(an_is_value, "Animation");
    g_id_set_rate = idl_setter_id(ctx, IDL_DOUBLE, false, js_an_set, S_RATE);
    idl_this_iface(an_is_value, "Animation");
    g_id_cancel = idl_method_id(ctx, NULL, 0, js_an_method, M_CANCEL);
    idl_this_iface(an_is_value, "Animation");
    g_id_finish = idl_method_id(ctx, NULL, 0, js_an_method, M_FINISH);
    idl_this_iface(an_is_value, "Animation");
    g_id_play = idl_method_id(ctx, NULL, 0, js_an_method, M_PLAY);
    idl_this_iface(an_is_value, "Animation");
    g_id_pause = idl_method_id(ctx, NULL, 0, js_an_method, M_PAUSE);
    idl_this_iface(an_is_value, "Animation");
    g_id_update_rate = idl_method_id(ctx, RATE_ARGS, 1, js_an_method, M_UPDATE_RATE);
    idl_this_iface(an_is_value, "Animation");
    g_id_reverse = idl_method_id(ctx, NULL, 0, js_an_method, M_REVERSE);
    idl_this_iface(an_is_value, "Animation");
    g_id_persist = idl_method_id(ctx, NULL, 0, js_an_method, M_PERSIST);
    idl_this_iface(an_is_value, "Animation");
    g_notify_slot = realm_value_declare(ctx, "Web Animations §4.5.12 this realm's finish notification steps");
    agent_state_ptr("animation", &g_rt, "the runtime Web Animations §6.4 was declared in");
    agent_state_class("animation", &g_class, "Web Animations §6.4 Animation's prototype slot and brand");
    agent_state_value("animation", &g_key, "the private key an Animation's internal slots live under");
    agent_state_realm_slot("animation", &g_notify_slot, "the per-realm slot §4.5.12's notification function is in");
    agent_state_id("animation", &g_id_ctor, "§6.4 Animation's constructor declaration");
    agent_state_id("animation", &g_id_set_id, "§6.4 Animation's `id` setter declaration");
    agent_state_id("animation", &g_id_set_timeline, "§6.4 Animation's `timeline` setter declaration");
    agent_state_id("animation", &g_id_set_start, "§6.4 Animation's `startTime` setter declaration");
    agent_state_id("animation", &g_id_set_current, "§6.4 Animation's `currentTime` setter declaration");
    agent_state_id("animation", &g_id_set_rate, "§6.4 Animation's `playbackRate` setter declaration");
    agent_state_id("animation", &g_id_cancel, "§6.4 Animation's `cancel()` declaration");
    agent_state_id("animation", &g_id_finish, "§6.4 Animation's `finish()` declaration");
    agent_state_id("animation", &g_id_play, "§6.4 Animation's `play()` declaration");
    agent_state_id("animation", &g_id_pause, "§6.4 Animation's `pause()` declaration");
    agent_state_id("animation", &g_id_update_rate, "§6.4 Animation's `updatePlaybackRate()` declaration");
    agent_state_id("animation", &g_id_reverse, "§6.4 Animation's `reverse()` declaration");
    agent_state_id("animation", &g_id_persist, "§6.4 Animation's `persist()` declaration");
    realm_declare_intrinsic(animation_install_realm);
    animation_playback_event_init(ctx);
    css_animations_init(ctx);
    animation_event_init(ctx);
    animatable_init(ctx);
}

void animation_free(JSRuntime *rt)
{
    DCHECK(g_rt != NULL, "Web Animations §6.4 was released in an agent that never declared it");
    DCHECK(g_rt == rt, "Web Animations §6.4 was released against a runtime other than the one it was declared in");
    animatable_free();
    animation_event_free(rt);
    css_animations_free(rt);
    animation_playback_event_free(rt);
    animation_timeline_free(rt);
    document_animations_free(rt);
    JS_FreeValueRT(rt, g_key);
    /* Last: the row's slots, the three sub-components' included, go back together. */
    agent_state_undo("animation");
}
