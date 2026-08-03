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
static int g_add_stepid = -1, g_remove_stepid = -1;
static const JSTrampStepDef js_add_listener_def, js_remove_listener_def;

void event_target_init(JSContext *ctx)
{
    DCHECK(!g_ready, "event_target_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "eventListeners", false);
    CHECK(!JS_IsException(g_key), "the event-listener key allocation failed");
    g_ready = 1;
    if (g_add_stepid < 0) {
        JSRuntime *rt = JS_GetRuntime(ctx);
        g_add_stepid    = JS_RegisterStepDef(rt, &js_add_listener_def);
        g_remove_stepid = JS_RegisterStepDef(rt, &js_remove_listener_def);
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

/* THE COERCION IS THE ONLY PART THAT REACHES THE PAGE. `type` is a Web IDL DOMString, so
   `el.addEventListener({toString(){ for(;;){} }}, f)` is the page's loop: it parks on step_tostring_run and
   resumes at the exact stage. Everything after it touches the component's own listener map and cannot.
   arg 0 of the def selects add (0) or remove (1) — one machine, because the two differ only in the call they
   finish with. */
typedef struct JSListenerState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   type;     /* the coerced DOMString (owned) */
} JSListenerState;

static void js_listener_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSListenerState *s = st;
    v->val(ctx, &s->type);
}

static JSValue js_listener_fini(JSContext *ctx, void *st, bool take_result)
{
    JSListenerState *s = st;
    (void)take_result;
    JS_FreeValue(ctx, s->type);
    s->type = JS_UNDEFINED;
    return JS_UNDEFINED;   /* both operations return undefined */
}

static int js_listener_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSListenerState *s = st;
    JSValueConst cb = step_arg(&s->hdr, 1);
    const char *type;
    int r;

    /* 2.7: a non-callable listener is ignored — and ignored BEFORE the coercion, which is what stops
       `addEventListener({toString(){ … }}, null)` from running that toString for nothing. */
    if (s->hdr.argc < 2 || !JS_IsFunction(ctx, cb)) {
        JS_FreeValue(ctx, cb_result);
        return JS_STEP_DONE;
    }
    r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->type, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;

    type = JS_ToCString(ctx, s->type);   /* a real string by now: this cannot reach the page */
    if (!type)
        return JS_STEP_ABRUPT;
    if (s->hdr.arg == 0)
        JS_FreeValue(ctx, add_listener_with_type(ctx, s->hdr.this_val, cb, type));
    else
        JS_FreeValue(ctx, remove_listener_with_type(ctx, s->hdr.this_val, cb, type));
    JS_FreeCString(ctx, type);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_add_listener_def = {
    sizeof(JSListenerState), js_listener_step, js_listener_fini, 0, .visit = js_listener_visit
};
static const JSTrampStepDef js_remove_listener_def = {
    sizeof(JSListenerState), js_listener_step, js_listener_fini, 1, .visit = js_listener_visit
};

/* The Event a listener receives. Enough of §2.2 to be USED rather than inspected for completeness: a page reads
   `type` and `target`, and calls the three no-op-in-a-headless-run methods. What is not here is absent. */
/* §2.2 the event's own FLAGS, set on the event object the listener was handed. These were one shared no-op —
   the lazy stub the IDL audit exists to expose — and a no-op preventDefault is not a small inaccuracy: whether
   the default action was cancelled is the ONE thing dispatchEvent reports, and a page that branches on
   `defaultPrevented` was reading a constant. Each now writes its flag, which is all the spec says they do.
   magic: 0 = preventDefault, 1 = stopPropagation, 2 = stopImmediatePropagation. */
static JSValue js_event_flag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const char *const FLAG[] = { "defaultPrevented", "cancelBubble", "cancelBubble" };
    (void)argc; (void)argv;
    /* 2.2: preventDefault does nothing unless the event is cancelable. */
    if (magic == 0) {
        JSValue c = JS_GetPropertyStr(ctx, this_val, "cancelable");
        int can = JS_ToBool(ctx, c);
        JS_FreeValue(ctx, c);
        if (!can) return JS_UNDEFINED;
    }
    if (magic == 2)
        JS_SetPropertyStr(ctx, (JSValue)this_val, "immediatePropagationStopped", JS_TRUE);
    JS_SetPropertyStr(ctx, (JSValue)this_val, FLAG[magic], JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue make_event(JSContext *ctx, const char *type, JSValueConst target)
{
    JSValue ev = JS_NewObject(ctx);
    if (JS_IsException(ev)) return ev;
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, ev, "currentTarget", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, ev, "bubbles", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, ev, "cancelable", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, ev, "cancelBubble", JS_FALSE);
    JS_SetPropertyStr(ctx, ev, "preventDefault",
                      JS_NewCFunctionMagic(ctx, js_event_flag, "preventDefault", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, ev, "stopPropagation",
                      JS_NewCFunctionMagic(ctx, js_event_flag, "stopPropagation", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, ev, "stopImmediatePropagation",
                      JS_NewCFunctionMagic(ctx, js_event_flag, "stopImmediatePropagation", 0, JS_CFUNC_generic_magic, 2));
    return ev;
}

/* Schedule ONE listener as a JOB on the running flow. Never a JS_Call: the listener is the page's code and holds
   loops, awaits and concolic branches, so a C activation cannot host it. JS_EnqueueCallJob runs it as a
   call-root flow — the same base a promise reaction runs on, preemptible and forkable like any other program. */
static void schedule_listener(JSContext *ctx, JSValueConst fn, JSValueConst ev)
{
    JS_EnqueueCallJob(ctx, fn, 1, &ev);
}

static int fire_at(JSContext *ctx, JSValueConst target, const char *type, JSValueConst ev)
{
    JSValue map, arr;
    uint32_t len = 0;
    int n = 0;

    map = listener_map(ctx, target, 0);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return 0; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (JS_IsArray(arr)) {
        JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
        for (uint32_t i = 0; i < len; i++) {
            JSValue fn = JS_GetPropertyUint32(ctx, arr, i);
            if (JS_IsFunction(ctx, fn)) { schedule_listener(ctx, fn, ev); n++; }
            JS_FreeValue(ctx, fn);
        }
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
    return n;
}

int event_target_fire(JSContext *ctx, JSValueConst target, const char *type, JSValueConst bubble_to)
{
    JSValue ev = make_event(ctx, type, target);
    int n;

    if (JS_IsException(ev)) return 0;
    n = fire_at(ctx, target, type, ev);
    if (JS_IsObject(bubble_to))
        n += fire_at(ctx, bubble_to, type, ev);   /* §2.9 bubble: a document event also reaches window */
    JS_FreeValue(ctx, ev);
    return n;
}

void event_target_install(JSContext *ctx, JSValueConst target)
{
    DCHECK(JS_IsObject(target), "event_target_install was handed something that is not an object");
    JS_SetPropertyStr(ctx, (JSValue)target, "addEventListener",
                      JS_NewCFunction2(ctx, NULL, "addEventListener", 2, JS_CFUNC_step, g_add_stepid));
    JS_SetPropertyStr(ctx, (JSValue)target, "removeEventListener",
                      JS_NewCFunction2(ctx, NULL, "removeEventListener", 2, JS_CFUNC_step, g_remove_stepid));
}
