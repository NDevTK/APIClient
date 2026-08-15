/* UNHANDLED PROMISE REJECTIONS — HTML §8.1.4.7 "Unhandled promise rejections".
 *
 * The section number was §8.1.7.5 throughout this file, which is a REAL section — "Dealing with the event loop
 * from other specifications" — and a different algorithm. A citation that resolves to the wrong text is worse
 * than none, because it reads as checked.
 *
 * WHAT WAS MISSING, AND WHY IT MATTERS HERE MORE THAN IN A BROWSER: a promise that rejects with nobody to catch
 * it was SILENT. Not softened, not swallowed by a fallback — never observed at all. A page error is this
 * engine's report of a capability the page needed and the engine does not have, and an async bundle delivers
 * most of its errors as rejections: an `await` of a missing global, a `.then` whose body touches an unbuilt
 * API. Every one of those looked exactly like a flow that ran and did nothing, which is the worst possible
 * failure mode for a tool whose whole job is to notice what code CAN do.
 *
 * THE TWO LISTS ARE THE SPEC, AND THEY ARE WHY THIS IS NOT JUST A PRINT AT THE THROW SITE. §8.1.4.7 keeps an
 * "about-to-be-notified" list and an "outstanding" list precisely because attaching a handler LATER is normal,
 * correct code — `var p = f(); p.catch(h)` rejects before the catch is attached. Reporting at rejection time
 * would call every one of those an error. The runtime's tracker reports both edges (rejected-with-no-handler,
 * and handled-after-the-fact), so an entry that gets handled is removed and never notified about.
 *
 * THE LIST IS A HEAP OBJECT, for the same reason the custom-element registry is: it is per-flow state. Flow A
 * rejecting a promise is not flow B's rejection, a parked flow must resume owed exactly what it was owed, and
 * a C-global list would report one flow's rejection against another's world. A baseline Array carries all of
 * that through the heap COW with no new primitive.
 *
 * THE CHECKPOINT IS THE FLOW'S END, not HTML's per-microtask-checkpoint. HTML notifies after every microtask
 * checkpoint and then fires `rejectionhandled` to RETRACT a report whose promise was handled afterwards; this
 * engine has no drain to hang a checkpoint on (the scheduler IS the job pump), and reporting once the flow has
 * no work left is strictly the same set minus the retractions. That is why there is no rejectionhandled here:
 * there is no early report for it to retract.
 *
 * THE EVENT IS THE PAGE'S CHANCE TO ANSWER, and it is CANCELABLE, so the report is not this component's to
 * make: §8.1.4.7 fires `unhandledrejection` at the global and reports only if nothing called preventDefault.
 * A page that ships its own error reporter cancels it — and that reporter is code with a fetch in it, which is
 * exactly the surface this engine exists to reach, so firing the event is worth more than the report is.
 * Firing is a REQUEST (dispatch is synchronous and its listeners are the page's code), so each notification is
 * a step machine on the flow's queue rather than a call from C.
 *
 * WHAT IS HONESTLY ABSENT: `rejectionhandled`. It exists to RETRACT a report made at a per-microtask
 * checkpoint whose promise was handled afterwards, and this engine notifies at the flow's end instead, where
 * there is no early report to retract. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/unhandled_rejection.h"

/* §8.1.4.7's list, as a baseline heap object so it time-travels with the flow that filled it. Each live entry
   is a two-element array [promise, reason]; a handled entry becomes undefined in place, because the identity
   of a slot is what the handled edge finds it by and compacting would move the ones behind it. */
static JSValue g_list;
static int     g_ready;
/* PromiseRejectionEvent: its prototype, and the private Symbol its two slots live under. A page cannot forge a
   slot bag it cannot name, which is the same shape Event uses for its own. */
/* PER REALM, for the reason event.c states — a C member runs in the realm that DEFINED it. Held in quickjs's
   own per-context class-proto slot. */
static JSClassID g_pre_class;
static int g_id_pre_ctor;
static JSValue g_pre_key = JS_UNDEFINED;
/* THE NOTIFICATION DRIVER IS PER REALM, and that is §8.1.4.7's own requirement rather than tidiness: the event
   is fired at "the promise's relevant global object", and this driver reads its global off the ctx it runs
   under — which, for one function object held for the agent, is whichever realm minted it. Every child
   document's unhandled rejection fired at the ROOT window, which is the same defect a shared
   EventTarget.prototype has one link up. A rejection RECORDS the driver of the realm it rejected in, so the
   job carries its realm with it rather than the notify pass guessing one. */
static int g_notify_slot = -1, g_notify_stepid = -1;
static void  (*g_report)(JSContext *ctx, JSValueConst reason);

void unhandled_rejection_set_report_hook(void (*fn)(JSContext *ctx, JSValueConst reason)) { g_report = fn; }

static uint32_t list_len(JSContext *ctx)
{
    JSValue v = JS_GetPropertyStr(ctx, g_list, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                              bool is_handled, void *opaque)
{
    uint32_t n, i;

    (void)opaque;
    DCHECK(g_ready, "the rejection tracker fired after its list was freed");
    n = list_len(ctx);
    if (!is_handled) {
        /* "add promise to the about-to-be-notified rejected promises list" */
        JSValue e = JS_NewArray(ctx);
        CHECK(!JS_IsException(e), "unhandled rejections: OOM recording a rejection — a dropped one is an error "
                                  "the page reported and this engine never saw");
        JS_SetPropertyUint32(ctx, e, 0, JS_DupValue(ctx, promise));
        JS_SetPropertyUint32(ctx, e, 1, JS_DupValue(ctx, reason));
        /* THE REJECTING REALM'S driver — quickjs hands this hook the ctx of the realm the promise belongs to,
           which is the one place that fact is known. */
        JS_SetPropertyUint32(ctx, e, 2, realm_value_get(ctx, g_notify_slot));
        JS_SetPropertyUint32(ctx, g_list, n, e);
        return;
    }
    /* "remove promise from the about-to-be-notified rejected promises list" — a handler attached after the
       rejection, which is ordinary code, not an error. */
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, g_list, i), p;
        bool same;
        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
        p = JS_GetPropertyUint32(ctx, e, 0);
        same = JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(promise);
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, e);
        if (same) { JS_SetPropertyUint32(ctx, g_list, i, JS_UNDEFINED); return; }
    }
    /* Not in the list is the ordinary case: the runtime reports the handled edge for every rejected promise,
       including ones handled in the same turn they rejected, which never reached the list at all. */
}

/* ---- PromiseRejectionEvent — §8.1.4.7's own interface ------------------------------------------------------ */
/* The two slots. Own properties under a private Symbol, read back by the getters — `promise` and `reason` are
   readonly attributes on the interface, never properties the page can assign. */
static JSValue pre_slot(JSContext *ctx, JSValueConst ev, const char *name)
{
    JSAtom k;
    JSValue slots, v;

    if (!JS_IsObject(ev)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_pre_key);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP: a lookup walks the prototype chain and reaches the solver's
       absent-state seam, which would mint a concolic for a name nobody defined. */
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    v = JS_GetPropertyStr(ctx, slots, name);
    JS_FreeValue(ctx, slots);
    return v;
}

static JSValue js_pre_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    return pre_slot(ctx, this_val, magic ? "reason" : "promise");
}

/* Mint one. `ev` is an Event this component then RE-POINTS at PromiseRejectionEvent.prototype, because the
   derived interface is a prototype chain and not a second kind of event object. */
static JSValue pre_new(JSContext *ctx, JSValue ev, JSValueConst promise, JSValueConst reason)
{
    JSValue slots;

    if (JS_IsException(ev)) return ev;
    {
        JSValue pre_proto = unhandled_rejection_proto(ctx);
        JS_SetPrototype(ctx, ev, pre_proto);
        JS_FreeValue(ctx, pre_proto);
    }
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "PromiseRejectionEvent: OOM allocating its slots");
    JS_SetPropertyStr(ctx, slots, "promise", JS_DupValue(ctx, promise));
    JS_SetPropertyStr(ctx, slots, "reason", JS_DupValue(ctx, reason));
    {
        JSAtom k = JS_ValueToAtom(ctx, g_pre_key);
        CHECK(k != JS_ATOM_NULL, "the PromiseRejectionEvent slot key could not be reached");
        JS_DefinePropertyValue(ctx, ev, k, slots, 0);   /* not enumerable, not writable: an internal slot */
        JS_FreeAtom(ctx, k);
    }
    return ev;
}

/* `new PromiseRejectionEvent(type, init)`. Every conversion is DECLARED — the type is a DOMString, the init is
   a dictionary whose `promise` member the IDL marks `required` — so by the time this runs there is no page
   code left to reach and it reads the engine-built dictionary with an ordinary get. */
static const IdlArgType PRE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember PRE_INIT[] = {   /* PromiseRejectionEventInit, in IDL declaration order */
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "promise", IDL_ANY, true }, { "reason", IDL_ANY },
};

static JSValue js_pre_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue promise, reason, ev;
    const char *type;
    bool bubbles, cancelable;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor PromiseRejectionEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PromiseRejectionEvent constructor requires a type");
    bubbles = idl_dict_bool(ctx, init, "bubbles");
    cancelable = idl_dict_bool(ctx, init, "cancelable");
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type) return JS_EXCEPTION;
    ev = event_new_untrusted(ctx, type, bubbles, cancelable);   /* §2.2: what the PAGE constructs is untrusted */
    JS_FreeCString(ctx, type);
    promise = idl_dict_get(ctx, init, "promise");
    reason = idl_dict_get(ctx, init, "reason");
    ev = pre_new(ctx, ev, promise, reason);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, reason);
    return ev;
}

/* ---- the notification ------------------------------------------------------------------------------------- */
/* ONE rejection's notification: fire `unhandledrejection` at the global and, unless a listener cancelled it,
   hand the reason to whoever decides what an unreported rejection means. A machine because §2.9 dispatch is
   SYNCHRONOUS and its listeners are the page's code — the fire is a request, not a call from C. */
/* WHERE THIS MACHINE RESTS. §8.1.4.7's steps 1-3 clone and empty the list and step 4 QUEUES a global task; this
   machine is that task's body for ONE promise, which is step 4.1's loop. Its two halves are the two stages:
   4.1.2 fires the event (the page's listeners), 4.1.3 decides whether the rejection is reported. Minting the
   PromiseRejectionEvent is part of 4.1.2 and cannot suspend, so it shares that stage's entry. */
#define REJECT_NOTIFY_STAGES(X) \
    X(REJECT_EVENT, "HTML §8.1.4.7 step 4.1.2 (the PromiseRejectionEvent, cancelable, for this promise)") \
    X(REJECT_FIRE,  "HTML §8.1.4.7 steps 4.1.2-4.1.3 (fire unhandledrejection at the global; report unless it " \
                    "was canceled)")
enum { REJECT_NOTIFY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REJECT_NOTIFY_STEPS[] = { REJECT_NOTIFY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSRejectNotify {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   fphase;   /* the fire request's own phase */
    bool      not_canceled;
    JSValue   ev;       /* the event, minted once and re-read after the dispatch (owned) */
    EventFireCb   cb;    /* the fire request's buffer — event_target_fire_run needs four slots */
} JSRejectNotify;

static void js_reject_notify_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRejectNotify *s = st;
    int k;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

static int js_reject_notify_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRejectNotify *s = st;
    JSValueConst promise = step_arg(&s->hdr, 0), reason = step_arg(&s->hdr, 1);
    JSValue global;
    int r, k;

    if (s->hdr.stage == REJECT_EVENT) {
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, REJECT_FIRE, &s->fphase, NULL);
        /* §8.1.4.7: `unhandledrejection` does not bubble and IS cancelable — the cancel is the whole point. */
        s->ev = pre_new(ctx, event_new(ctx, "unhandledrejection", false, true), promise, reason);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
    }
    DCHECK(s->hdr.stage == REJECT_FIRE, "the rejection notification resumed into a stage §8.1.4.7 does not have");
    global = JS_GetGlobalObject(ctx);
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), global, s->ev, JS_UNDEFINED, cb_result, &s->not_canceled,
                              out_cb, out_argc);
    JS_FreeValue(ctx, global);
    if (r > 0) return r;
    /* "If the event was not canceled, report the exception." A page that cancels is doing its own reporting,
       which is code with a fetch in it — the endpoint that reporter posts to is learned either way. */
    if (s->not_canceled && g_report)
        g_report(ctx, reason);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_reject_notify_def = {
    sizeof(JSRejectNotify), js_reject_notify_step, NULL, 0, .visit = js_reject_notify_visit,
    .algorithm = "HTML §8.1.4.7 notify about rejected promises — step 4's queued task, one promise",
    .steps = REJECT_NOTIFY_STEPS
};

int unhandled_rejection_notify(JSContext *ctx)
{
    uint32_t n, i;
    int queued = 0;

    DCHECK(g_ready, "an unhandled-rejection checkpoint ran before the tracker was installed");
    n = list_len(ctx);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, g_list, i);
        JSValueConst argv[2];
        JSValue promise, reason, notify;

        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
        promise = JS_GetPropertyUint32(ctx, e, 0);
        reason = JS_GetPropertyUint32(ctx, e, 1);
        notify = JS_GetPropertyUint32(ctx, e, 2);   /* the REJECTING realm's driver, recorded with the entry */
        DCHECK(JS_IsFunction(ctx, notify), "a recorded rejection lost the driver of the realm it rejected in");
        argv[0] = promise; argv[1] = reason;
        JS_EnqueueCallJob(ctx, notify, 2, argv);
        JS_FreeValue(ctx, notify);
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, reason);
        JS_FreeValue(ctx, e);
        queued++;
    }
    /* CLEARED HERE, not when the fires complete: the list is "about to be notified about", and these now are. */
    JS_SetPropertyStr(ctx, g_list, "length", JS_NewInt32(ctx, 0));
    return queued;
}

void unhandled_rejection_init(JSContext *ctx)
{
    DCHECK(!g_ready, "unhandled_rejection_init ran twice — one instance is one document");
    /* Built at init so it belongs to the pre-boot BASELINE: a write during a flow is captured by the heap COW.
       A list allocated lazily inside a flow would be that flow's private object and no sibling would see it. */
    g_list = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_list), "the rejected-promise list could not be allocated");
    g_pre_key = JS_NewSymbol(ctx, "promiseRejectionSlots", false);
    CHECK(!JS_IsException(g_pre_key), "the PromiseRejectionEvent slot key allocation failed");
    {
        JSClassDef d = { "PromiseRejectionEvent" };
        JS_NewClassID(JS_GetRuntime(ctx), &g_pre_class);
        JS_NewClass(JS_GetRuntime(ctx), g_pre_class, &d);
    }
    /* The notification driver is a step function nobody installs, so a page can neither see it nor replace it. */
    g_notify_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_reject_notify_def);
    CHECK(g_notify_stepid >= 0, "no step id for the rejection-notification driver");
    g_notify_slot = realm_value_declare(ctx, "§8.1.4.7 notifyRejected");
    g_ready = 1;
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), rejection_tracker, NULL);
    /* THE CONSTRUCTOR'S DECLARATION IS THE AGENT'S — one pool entry per member; every realm's interface object
       is built from that same one. */
    g_id_pre_ctor = idl_method_id_dict(ctx, PRE_CTOR_ARGS, 2, PRE_INIT,
                                       (int)(sizeof(PRE_INIT) / sizeof(PRE_INIT[0])), js_pre_ctor, 0);
    realm_declare_intrinsic(unhandled_rejection_install_proto);
}

/* §8.1.7.2's PROTOTYPE FOR ONE REALM. `interface PromiseRejectionEvent : Event` — a real chain, so
   `e instanceof Event` and every Event member hold on one of these, and it chains to THIS realm's
   Event.prototype because a chain to another document's is the same defect one link up. */
void unhandled_rejection_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base;

    DCHECK(g_ready, "a realm asked for PromiseRejectionEvent.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_pre_class);
    DCHECK(JS_IsNull(prev), "unhandled_rejection_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "PromiseRejectionEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PromiseRejectionEvent");
    idl_install_accessor(ctx, proto, "promise", js_pre_get, 0, -1);
    idl_install_accessor(ctx, proto, "reason", js_pre_get, 1, -1);
    JS_SetClassProto(ctx, g_pre_class, proto);
    {
        JSValue fn = JS_NewCFunction2(ctx, NULL, "notifyRejected", 2, JS_CFUNC_step, g_notify_stepid);
        CHECK(!JS_IsException(fn), "the rejection-notification driver could not be allocated");
        realm_value_set(ctx, g_notify_slot, fn);
    }
}

JSValue unhandled_rejection_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_pre_class);
    DCHECK(!JS_IsNull(proto),
           "PromiseRejectionEvent.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

void unhandled_rejection_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "PromiseRejectionEvent was installed before its prototype was built");
    ctor = idl_step_constructor(ctx, "PromiseRejectionEvent", 2, g_id_pre_ctor);
    CHECK(!JS_IsException(ctor), "the PromiseRejectionEvent interface object could not be allocated");
    {
        JSValue proto = unhandled_rejection_proto(ctx);
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "PromiseRejectionEvent", ctor);
}

void unhandled_rejection_free(JSContext *ctx)
{
    if (!g_ready) return;
    /* The tracker goes FIRST: teardown frees promises, and a rejected one still on the runtime's list would
       fire the callback into a list this call is about to release. */
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), NULL, NULL);
    g_ready = 0;
    JS_FreeValue(ctx, g_list);   /* the prototypes are the REALMS' — each is released with its context */
    JS_FreeValue(ctx, g_pre_key);
    g_list = g_pre_key = JS_UNDEFINED;   /* the per-realm driver is released with its context */
    g_report = NULL;
}
