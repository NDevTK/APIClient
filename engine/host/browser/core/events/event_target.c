/* EVENTTARGET — DOM §2.7, over the objects this engine already hands the page.
 *
 * WPT NAMED THIS ONE. Every DOM test loads testharness.js, which declares its tests and then waits for the LOAD
 * EVENT before reporting — so with no event loop, 175 of 175 dom/nodes files ran the harness perfectly and
 * produced nothing at all. It is also how ordinary pages are written: the half of a bundle that runs on
 * DOMContentLoaded is the half that touches the DOM and calls the API.
 *
 * WHERE THE LISTENERS LIVE. On the TARGET, as an ordinary own property under a private key the page cannot
 * reach. That is not a shortcut — it is what makes registration per-flow for free: a listener added in one arm
 * of a fork is a property write like any other, so the COW delta captures it and the sibling never sees it.
 * A side table keyed by object pointer would have needed its own delta kind and its own swap, for the same
 * result.
 *
 * HOW A LISTENER RUNS. Not by JS_Call from C — a listener body is the page's code and holds loops, awaits and
 * concolic branches, so calling it from a C activation is the drive-to-completion the engine aborts on. Each
 * listener is dispatched through the promise machinery, which is how every other page callback in this engine
 * reaches a flow: the reaction runs as a call-root flow, preemptible and forkable like anything else. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/events/event.h"

/* The two shapes every DOM member in this file has. Spelled once so a member declares its IDL, not a bitmask. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include "core/events/event_target.h"

/* The private key the listener map hangs off. A SYMBOL, so a page enumerating its own objects cannot see it and
   cannot collide with it — the same reason the platform uses internal slots. */
/* `g_ready` rather than testing g_key: a static JSValue is ZERO-initialised, and zero is not JS_UNDEFINED — the
   tag is part of the value. Asking JS_IsUndefined of it answers "no" before anything has run, which fired the
   ran-twice assert on the FIRST call. A JSValue's emptiness is not the allocator's default. */
static JSValue g_key;
static int g_ready;
/* The ids JS_RegisterStepDef handed this runtime for add/removeEventListener. `type` is a Web IDL DOMString,
   so it is ToString on whatever the page passed and cannot be a JS_ToCString from C. */
static int g_add_stepid = -1, g_remove_stepid = -1, g_dispatch_stepid = -1;
static JSValue idl_add_or_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);

void event_target_init(JSContext *ctx)
{
    DCHECK(!g_ready, "event_target_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "eventListeners", false);
    CHECK(!JS_IsException(g_key), "the event-listener key allocation failed");
    g_ready = 1;
    if (g_add_stepid < 0) {
        g_add_stepid    = idl_method_id(ctx, IDL_1STR, 1, idl_add_or_remove, 0);   /* (DOMString type, EventListener?) */
        g_remove_stepid = idl_method_id(ctx, IDL_1STR, 1, idl_add_or_remove, 1);
    }

}

void event_target_free(JSContext *ctx)
{
    if (!g_ready)
        return;
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;
    g_ready = 0;
}

/* The map of type -> listener array on `target`, created on first use. NULL only on allocation failure. */
static JSValue listener_map(JSContext *ctx, JSValueConst target, int create)
{
    JSAtom k;
    JSValue map;

    DCHECK(g_ready, "an event-listener map was asked for before the key existed");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP. `window` is the global object, and a miss on the global is the
       solver's absent-state seam: it mints a concolic for the name a page read and did not define. That is
       right for the page's own reads and wrong for an internal slot — the map came back as a concolic, every
       listener was stored on it, and window.addEventListener silently registered nothing. An internal slot is
       by definition an own slot, so it is read as one. */
    if (JS_GetOwnSlot(ctx, &map, target, k) <= 0)
        map = JS_UNDEFINED;
    if (!JS_IsObject(map) && create) {
        JS_FreeValue(ctx, map);
        map = JS_NewObject(ctx);
        if (!JS_IsException(map))
            JS_SetProperty(ctx, (JSValue)target, k, JS_DupValue(ctx, map));
    }
    JS_FreeAtom(ctx, k);
    return map;
}

/* The listener-list work, once `type` is a real string. Split from the coercion so the part that CAN reach the
   page's code is a request and the part that cannot is ordinary C. */
static JSValue add_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type)
{
    JSValue map, arr;
    uint32_t len = 0;
    JSValueConst argv[2];

    argv[0] = JS_UNDEFINED; argv[1] = cb;
    map = listener_map(ctx, this_val, 1);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr)) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, map, type, JS_DupValue(ctx, arr));
    }
    /* "If the event listener list already contains a listener with the same callback, do nothing." */
    JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        int same = JS_VALUE_GET_TAG(e) == JS_VALUE_GET_TAG(argv[1]) &&
                   JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(argv[1]);
        JS_FreeValue(ctx, e);
        if (same) { JS_FreeValue(ctx, arr); JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    }
    JS_SetPropertyUint32(ctx, arr, len, JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
   
    return JS_UNDEFINED;
}

static JSValue remove_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type)
{
    JSValue map, arr, kept;
    uint32_t len = 0, k = 0;
    JSValueConst argv[2];

    argv[0] = JS_UNDEFINED; argv[1] = cb;
    map = listener_map(ctx, this_val, 0);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    kept = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        int same = JS_VALUE_GET_TAG(e) == JS_VALUE_GET_TAG(argv[1]) &&
                   JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(argv[1]);
        if (same) JS_FreeValue(ctx, e);
        else JS_SetPropertyUint32(ctx, kept, k++, e);
    }
    JS_SetPropertyStr(ctx, map, type, kept);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
   
    return JS_UNDEFINED;
}

/* add/removeEventListener's `type` is a Web IDL DOMString, so it is ToString on whatever the page passed and
   cannot be a JS_ToCString from C. They use the SHARED coerce-then-call machine rather than one of their own:
   what they have in common with getAttribute and createElement is exactly the thing that needs a machine, and a
   second copy is a second chance to get the resumption wrong.
   IT ALSO FIXES AN ORDERING MISTAKE I MADE. A bespoke version checked 2.7's "if callback is null, return"
   BEFORE the coercion, to avoid running a toString for a call that does nothing. That is backwards: Web IDL
   converts arguments in ORDER at call time, so `type` is converted first and the null-callback step is part of
   the algorithm that runs after. `addEventListener({toString(){ … }}, null)` DOES run that toString in a real
   browser, and now here. */
static JSValue idl_add_or_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *type;
    JSValue r;

    if (argc < 2 || (magic == 0 && !JS_IsFunction(ctx, argv[1])))
        return JS_UNDEFINED;   /* 2.7: a non-callable listener is ignored, not an error */
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type)
        return JS_EXCEPTION;
    r = (magic == 0) ? add_listener_with_type(ctx, this_val, argv[1], type)
                     : remove_listener_with_type(ctx, this_val, argv[1], type);
    JS_FreeCString(ctx, type);
    return r;
}

/* Schedule ONE listener as a JOB on the running flow. Never a JS_Call: the listener is the page's code and holds
   loops, awaits and concolic branches, so a C activation cannot host it. JS_EnqueueCallJob runs it as a
   call-root flow — the same base a promise reaction runs on, preemptible and forkable like any other program. */
static void schedule_listener(JSContext *ctx, JSValueConst fn, JSValueConst ev)
{
    JS_EnqueueCallJob(ctx, fn, 1, &ev);
}

/* §2.9 step 5: dispatch runs over a COPY of the listener list. That matters more here than in a browser — the
   walk suspends across every listener, so one that adds or removes a listener has arbitrarily long to do it.
   ONE list walk, so the engine's own firing and the page's dispatchEvent can never disagree about which
   listeners a target has; what differs between them is only how each listener is DELIVERED. */
static JSValue listener_snapshot(JSContext *ctx, JSValueConst target, const char *type)
{
    JSValue map, arr, copy = JS_NewArray(ctx);
    uint32_t len = 0, k = 0;

    map = listener_map(ctx, target, 0);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return copy; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (JS_IsArray(arr)) {
        JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
        for (uint32_t i = 0; i < len; i++) {
            JSValue fn = JS_GetPropertyUint32(ctx, arr, i);
            if (JS_IsFunction(ctx, fn)) JS_SetPropertyUint32(ctx, copy, k++, fn);
            else                        JS_FreeValue(ctx, fn);
        }
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
    return copy;
}

static int fire_at(JSContext *ctx, JSValueConst target, const char *type, JSValueConst ev)
{
    JSValue arr = listener_snapshot(ctx, target, type);
    uint32_t len = 0;

    JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    for (uint32_t i = 0; i < len; i++) {
        JSValue fn = JS_GetPropertyUint32(ctx, arr, i);
        schedule_listener(ctx, fn, ev);
        JS_FreeValue(ctx, fn);
    }
    JS_FreeValue(ctx, arr);
    return (int)len;
}

/* §2.9 DISPATCH, as a machine — and the reason dispatchEvent could not exist before.
 *
 * The spec makes dispatch SYNCHRONOUS and makes its return value depend on what the listeners did: it answers
 * `!canceled`, so `if (!el.dispatchEvent(ev)) { … }` is how a page asks whether anything called preventDefault.
 * Neither of the two obvious implementations can answer that. Calling the listeners from C is the
 * drive-to-completion this engine aborts on — a listener body holds loops, awaits and concolic branches.
 * Enqueueing them as jobs answers before any of them has run, so the answer would always be "not cancelled".
 *
 * So each listener is a CALL REQUEST: the machine parks on it, the listener runs as ordinary preemptible page
 * code at whatever depth it likes, and the machine resumes at the listener it was on with `i` as its cursor.
 * That is the same shape every continuation-holding builtin in the engine has, which is why this needs no
 * machinery of its own beyond the state.
 *
 * THE LIST IS SNAPSHOT FIRST, which the spec requires and which matters here more than in a browser: a listener
 * that runs mid-walk can add or remove listeners, and this walk is suspended across every one of them. */
typedef struct JSDispatchState {
    JSStepHdr hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;    /* the call request's own phase, so a stage can hold a call across a suspension */
    uint32_t  i, n;      /* THE RESUME POINT: the listener being called, and how many there are */
    JSValue   arr;       /* the listener list SNAPSHOT (owned) */
    JSValue   ev;        /* the event (owned) */
    JSValue   result;    /* !canceled (owned) */
    JSValue   cb[3];     /* the call request buffer: [this, listener, event] */
} JSDispatchState;

/* WHAT THIS MACHINE OWNS. The call buffer is in here because a fork mid-listener must not hand two arms one
   invocation — that is exactly what the visit contract is for. */
static void js_dispatch_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDispatchState *s = st;
    int k;
    v->val(ctx, &s->arr);
    v->val(ctx, &s->ev);
    v->val(ctx, &s->result);
    for (k = 0; k < 3; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_dispatch_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDispatchState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    int k;

    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->result);
    JS_FreeValue(ctx, s->arr);
    JS_FreeValue(ctx, s->ev);
    s->result = s->arr = s->ev = JS_UNDEFINED;
    for (k = 0; k < 3; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return r;
}

static int js_dispatch_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDispatchState *s = st;
    JSValue fn, ignored;
    int r;

    if (s->stage == 0) {
        JSValue tv;
        const char *type;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->arr = s->ev = s->result = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = JS_UNDEFINED;
        s->ev = JS_DupValue(ctx, step_arg(&s->hdr, 0));
        /* §2.9 step 1: the argument must BE an Event. The slot record is the brand — a page cannot forge the
           symbol it hangs off, so an object shaped like an event is still not one. */
        if (!event_is(ctx, s->ev)) {
            JS_ThrowTypeError(ctx, "dispatchEvent requires an Event");
            return JS_STEP_ABRUPT;
        }
        /* §2.9 step 2: an event already being dispatched cannot be dispatched again. */
        if (event_dispatch_flag(ctx, s->ev)) {
            JS_ThrowDOMException(ctx, "InvalidStateError", "the event is already being dispatched");
            return JS_STEP_ABRUPT;
        }
        event_set_dispatch_flag(ctx, s->ev, true);
        /* §2.9 step 3: isTrusted is false for an event the page dispatches, whatever it was when constructed. */
        event_set_trusted(ctx, s->ev, false);
        event_set_targets(ctx, s->ev, s->hdr.this_val, s->hdr.this_val);

        tv = event_type(ctx, s->ev);
        type = JS_IsString(tv) ? JS_ToCString(ctx, tv) : NULL;
        s->arr = type ? listener_snapshot(ctx, s->hdr.this_val, type) : JS_NewArray(ctx);
        if (type) JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, tv);
        s->n = 0;
        JS_ToUint32(ctx, &s->n, JS_GetPropertyStr(ctx, s->arr, "length"));
        s->i = 0;
        s->stage = 1;
    }

    while (s->i < s->n) {
        /* §2.9: stopImmediatePropagation ends the walk between listeners, which is the only place it can be
           observed — and the flag was set by a listener that has already returned. */
        if (s->cphase == 0 && event_stop_immediate(ctx, s->ev))
            break;
        fn = JS_GetPropertyUint32(ctx, s->arr, s->i);
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            s->i++;
            continue;
        }
        /* §2.9 "inner invoke": the listener is called with `this` = currentTarget and the event as its one
           argument. A CALL REQUEST, so the listener is ordinary preemptible page code and this machine parks. */
        r = step_call_run(ctx, &s->cphase, s->cb, fn, s->hdr.this_val, 1, (JSValueConst *)&s->ev,
                          cb_result, &ignored, out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;         /* parked ON THIS LISTENER; the resume comes back to it */
        JS_FreeValue(ctx, ignored);  /* §2.9: a listener's return value is discarded */
        s->i++;
    }
    JS_FreeValue(ctx, cb_result);

    /* §2.9 "clean up": the dispatch flag clears, the phase goes back to NONE and currentTarget to null. */
    event_set_dispatch_flag(ctx, s->ev, false);
    event_clear_current(ctx, s->ev);
    s->result = JS_NewBool(ctx, !event_canceled(ctx, s->ev));
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_dispatch_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, 0, .visit = js_dispatch_visit
};

int event_target_fire(JSContext *ctx, JSValueConst target, const char *type, JSValueConst bubble_to)
{
    /* A real Event (§2.2), TRUSTED because the engine fired it — that flag is the whole difference from one the
       page constructs, and a page checks it. `load`/`abort`/`DOMContentLoaded` all bubble and none is
       cancelable, which is what the HTML and DOM specs say for each of them. */
    JSValue ev = event_new(ctx, type, /*bubbles*/ true, /*cancelable*/ false);
    int n;

    if (JS_IsException(ev)) return 0;
    event_set_targets(ctx, ev, target, target);
    n = fire_at(ctx, target, type, ev);
    if (JS_IsObject(bubble_to)) {
        event_set_targets(ctx, ev, target, bubble_to);   /* §2.9: currentTarget moves, target does not */
        n += fire_at(ctx, bubble_to, type, ev);
    }
    JS_FreeValue(ctx, ev);
    return n;
}

void event_target_install(JSContext *ctx, JSValueConst target)
{
    DCHECK(JS_IsObject(target), "event_target_install was handed something that is not an object");
    /* Declared HERE rather than in event_target_init because the machine is defined below it — and one
       declaration serves every target, which is what the id being cached says. */
    if (g_dispatch_stepid < 0)
        g_dispatch_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_dispatch_def);
    idl_install_method(ctx, target, "addEventListener", 2, g_add_stepid);
    idl_install_method(ctx, target, "removeEventListener", 2, g_remove_stepid);
    idl_install_method(ctx, target, "dispatchEvent", 1, g_dispatch_stepid);
}
