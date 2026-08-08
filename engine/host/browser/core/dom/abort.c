/* ABORTCONTROLLER / ABORTSIGNAL — DOM §3.2, and the interface CLAUDE.md names when it says the concolic value
 * belongs only where the value is UNKNOWN.
 *
 * BOTH HALVES ARE HERE, AND THEY ARE DIFFERENT KINDS OF THING.
 *
 *   A CONTROLLER'S SIGNAL IS THE REAL STATE MACHINE. `new AbortController().signal.aborted` is false because
 *   this engine created the signal and knows nothing has aborted it; `controller.abort()` sets the flag, stores
 *   the reason and fires the `abort` event. There is no ignorance to model, so a concolic here would fork a
 *   branch whose sibling cannot happen.
 *
 *   A TIMEOUT SIGNAL IS UNKNOWN. Whether `AbortSignal.timeout(5000)` has fired by the time the code asks
 *   depends on wall-clock this engine does not model, and BOTH answers lead to code worth reaching — the
 *   request path and the retry/fallback path, and the fallback is routinely a different endpoint. So its
 *   `aborted` is concolic with the example a fast machine gives (false), and every branch on it forks.
 *
 * WHICH IS WHY THE C SIDE NEVER TESTS THE FLAG ITSELF. `throwIfAborted()` and the `reason` getter both have to
 * branch on a value that may be concolic, and a C `if` would silently pick one arm — the exact failure the
 * solver exists to prevent. They ask solver_decide, the same frame-agnostic hook an OP_if asks, so a builtin
 * forks by the identical call and the sibling arm is parked as its own flow.
 *
 * THE INTERNAL SLOTS ARE AN OWN PROPERTY UNDER A PRIVATE SYMBOL, for the reason EventTarget's listener map is:
 * a write to them is an ordinary property write, so the per-flow COW delta captures it with no new delta kind.
 * An abort in one arm of a fork is invisible to the sibling for free.
 *
 * WHICH MEMBERS ARE STEP MACHINES, AND WHY THE REST ARE PROVABLY NOT. A member that can reach the page's code
 * is a machine; a member that cannot is a plain C function with an assert saying so, because a machine there
 * would be ceremony and a plain function anywhere else would be a hole in the flow machinery.
 *   - AbortSignal.timeout(ms) IS a machine: `[EnforceRange] unsigned long long` is ToNumber on whatever the
 *     page passed, so `AbortSignal.timeout({valueOf(){ for(;;){} }})` is the page's loop and has to suspend.
 *   - AbortController.abort() is NOT, and the reason is a spec correction rather than a concession: 3.2 uses
 *     `this.[[Signal]]`, an INTERNAL SLOT, not Get(this, "signal"). Reading the public property (which is what
 *     this file did) both ran a page getter from C and let a page that overrides `signal` redirect abort().
 *   - throwIfAborted() and the `aborted`/`reason` getters touch OWN SLOTS ONLY, read with JS_GetOwnSlot, which
 *     is by definition not a lookup and cannot reach an accessor or a proxy trap. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/dom/abort.h"

/* The private key the signal's internal slots hang off — a Symbol, so a page enumerating its own objects
   cannot see it and cannot collide with it. `g_ready` rather than testing g_key, because a static JSValue is
   zero-initialised and zero is not JS_UNDEFINED. */
static JSValue g_key;
static int g_ready;
/* THE INTERFACE PROTOTYPE OBJECTS — Web IDL §3.7. Every member of these two interfaces is declared on
   `AbortSignal.prototype` / `AbortController.prototype`, not on the instance, and that is not decoration: it is
   what makes `signal instanceof AbortSignal` true, what a page's `AbortSignal.prototype.throwIfAborted.call(x)`
   reaches, and what `Object.getOwnPropertyNames(signal)` correctly reports as EMPTY. Building the members onto
   each instance instead left the interface object with no `.prototype` at all, so `instanceof` threw
   "operand 'prototype' property is not an object" — the page could not even ASK what a signal was. */
static JSValue g_sig_proto, g_ctrl_proto;
/* The id JS_RegisterStepDef handed this runtime for AbortSignal.timeout's machine. One WASM instance is one
   document is one runtime, which is what the install DCHECK holds it to. */
static int g_timeout_stepid = -1;
static JSRuntime *g_abort_rt;

void abort_init(JSContext *ctx)
{
    DCHECK(!g_ready, "abort_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "abortState", false);
    CHECK(!JS_IsException(g_key), "the AbortSignal slot key allocation failed");
    g_sig_proto = g_ctrl_proto = JS_UNDEFINED;
    g_ready = 1;
}

void abort_free(JSContext *ctx)
{
    if (!g_ready)
        return;
    JS_FreeValue(ctx, g_key);
    JS_FreeValue(ctx, g_sig_proto);
    JS_FreeValue(ctx, g_ctrl_proto);
    g_key = g_sig_proto = g_ctrl_proto = JS_UNDEFINED;
    g_ready = 0;
}

/* The internal-slot record on `o` — `{ aborted, reason }` for a signal, `{ signal }` for a controller — or
   UNDEFINED when `o` has none. Read as an OWN SLOT,
   never a lookup: a miss on a property lookup is the solver's absent-state seam and would mint a concolic for
   an internal slot, which is right for the page's own reads and wrong here. */
static JSValue signal_slots(JSContext *ctx, JSValueConst sig)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an AbortSignal slot was asked for before the key existed");
    if (!JS_IsObject(sig))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &st, sig, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

/* IS THIS SIGNAL ABORTED, ASKED THE ONLY WAY A C BUILTIN MAY ASK. solver_decide answers 0/1 for a concolic
   flag and parks the other arm as its own flow; -1 means the flag is an ordinary boolean and the real ToBool
   is the answer. A bare `if` here would pick one arm of an unknown and delete the other's code. */
static int signal_is_aborted(JSContext *ctx, JSValueConst slots)
{
    JSValue flag = JS_GetPropertyStr(ctx, slots, "aborted");
    int arm = solver_decide(ctx, flag);
    int r = (arm < 0) ? JS_ToBool(ctx, flag) : (arm == 1);
    JS_FreeValue(ctx, flag);
    return r;
}

/* §3.2's "abort reason": the DOMException the spec names when the caller supplied none. Built by throwing one
   and taking it back, because that is the engine's only constructor for the interface and it runs none of the
   page's code — reading `DOMException` off the global would, since a page may replace it. */
static JSValue abort_reason_default(JSContext *ctx, const char *name, const char *msg)
{
    JS_ThrowDOMException(ctx, name, "%s", msg);
    return JS_GetException(ctx);
}

/* §3.2 "signal abort" step 2: an UNDEFINED reason becomes a new "AbortError" DOMException. It lives here, in
   the one operation, rather than at each caller counting its own arguments — `controller.abort()`, Streams
   §5.2's WritableStreamAbort and `AbortSignal.abort()` all reach the same step, and a caller that forgot it
   handed the page a signal whose `reason` was undefined AFTER it had aborted, which no real signal can be.
   `reason` is CONSUMED. */
static JSValue abort_reason_or_default(JSContext *ctx, JSValue reason)
{
    if (!JS_IsUndefined(reason))
        return reason;
    JS_FreeValue(ctx, reason);
    return abort_reason_default(ctx, "AbortError", "signal is aborted without reason");
}

static JSValue js_sig_get_aborted(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue slots = signal_slots(ctx, this_val), v;
    (void)argc; (void)argv;
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "aborted called on something that is not an AbortSignal");
    }
    v = JS_GetPropertyStr(ctx, slots, "aborted");
    JS_FreeValue(ctx, slots);
    return v;
}

/* §3.2: `reason` is the abort reason when aborted and undefined otherwise — so it branches on the flag, and
   branches on it the same way everything else does. */
static JSValue js_sig_get_reason(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue slots = signal_slots(ctx, this_val), v;
    (void)argc; (void)argv;
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "reason called on something that is not an AbortSignal");
    }
    v = signal_is_aborted(ctx, slots) ? JS_GetPropertyStr(ctx, slots, "reason") : JS_UNDEFINED;
    JS_FreeValue(ctx, slots);
    return v;
}

static JSValue js_sig_throw_if_aborted(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue slots = signal_slots(ctx, this_val);
    (void)argc; (void)argv;
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "throwIfAborted called on something that is not an AbortSignal");
    }
    if (signal_is_aborted(ctx, slots)) {
        JSValue reason = JS_GetPropertyStr(ctx, slots, "reason");
        JS_FreeValue(ctx, slots);
        return JS_Throw(ctx, reason);
    }
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* Create a signal. `aborted` and `reason` are CONSUMED. */
static JSValue signal_new(JSContext *ctx, JSValue aborted, JSValue reason)
{
    JSValue sig, st;
    JSAtom k;

    DCHECK(JS_IsObject(g_sig_proto),
           "an AbortSignal was minted before abort_install built AbortSignal.prototype — the members live "
           "there, so a signal made earlier would have none of them");
    sig = JS_NewObjectProto(ctx, g_sig_proto);
    CHECK(!JS_IsException(sig), "the AbortSignal allocation failed");
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "the AbortSignal slot record allocation failed");
    JS_SetPropertyStr(ctx, st, "aborted", aborted);
    JS_SetPropertyStr(ctx, st, "reason", reason);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the AbortSignal slot key could not be interned");
    JS_SetProperty(ctx, sig, k, st);
    JS_FreeAtom(ctx, k);
    /* THE SLOT RECORD IS ALL AN INSTANCE CARRIES. Every member is on the prototype above. */
    return sig;
}

/* §3.2 "signal abort", STEP 2 AND STEP 3: set the flag and the reason. Already-aborted answers false, which is
   what keeps a double abort() from running anything twice. `reason` is CONSUMED. */
static bool signal_abort_state(JSContext *ctx, JSValueConst sig, JSValue reason)
{
    JSValue slots = signal_slots(ctx, sig);

    if (!JS_IsObject(slots) || signal_is_aborted(ctx, slots)) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, reason);
        return false;
    }
    JS_SetPropertyStr(ctx, slots, "aborted", JS_TRUE);
    JS_SetPropertyStr(ctx, slots, "reason", abort_reason_or_default(ctx, reason));
    JS_FreeValue(ctx, slots);
    return true;
}

/* ---- §3.2's ABORT ALGORITHMS ------------------------------------------------------------------------------ */

/* The signal's algorithm list, an Array on its slot record — created on demand, so a signal nobody registers
   against carries nothing. An ALGORITHM IS NOT A LISTENER: it runs before the `abort` event, the page cannot
   see it or remove it, and the whole list is dropped once it has run. */
static JSValue signal_algos(JSContext *ctx, JSValueConst sig, int create)
{
    JSValue slots = signal_slots(ctx, sig), arr;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, slots, "algorithms");
    if (!JS_IsArray(arr) && create) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        if (!JS_IsException(arr))
            JS_SetPropertyStr(ctx, slots, "algorithms", JS_DupValue(ctx, arr));
    }
    JS_FreeValue(ctx, slots);
    return arr;
}

static uint32_t array_len(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = 0;
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

void abort_signal_add_algorithm(JSContext *ctx, JSValueConst sig, JSValueConst fn)
{
    JSValue arr;

    DCHECK(JS_IsFunction(ctx, fn), "an abort algorithm that is not callable was registered on a signal");
    /* §3.2: "if signal is aborted, then return". A signal that has already fired has already emptied its list,
       so registering into it would leave a value nothing will ever run or free. */
    if (abort_signal_aborted(ctx, sig))
        return;
    arr = signal_algos(ctx, sig, 1);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return; }
    JS_SetPropertyUint32(ctx, arr, array_len(ctx, arr), JS_DupValue(ctx, fn));
    JS_FreeValue(ctx, arr);
}

void abort_signal_remove_algorithm(JSContext *ctx, JSValueConst sig, JSValueConst fn)
{
    JSValue arr = signal_algos(ctx, sig, 0);
    uint32_t i, n, k = 0;

    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return; }
    n = array_len(ctx, arr);
    /* COMPACT IN PLACE rather than splice: `splice` is a page-visible method on Array.prototype and this list
       is the engine's, so it is walked with own indices like everything else here. */
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_VALUE_GET_TAG(v) == JS_VALUE_GET_TAG(fn) && JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(fn)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        JS_SetPropertyUint32(ctx, arr, k++, v);
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, k));
    JS_FreeValue(ctx, arr);
}

bool abort_signal_is(JSContext *ctx, JSValueConst v)
{
    /* THE SLOT RECORD IS THE BRAND. A signal is the only thing this component gives one of these to, and it is
       under a private Symbol the page cannot mint — so this is a real brand test and not a shape test. */
    JSValue slots = signal_slots(ctx, v), flag;
    bool ok;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return false; }
    /* A CONTROLLER has one of these too, holding `signal`; only a SIGNAL's has `aborted`. */
    flag = JS_GetPropertyStr(ctx, slots, "aborted");
    ok = !JS_IsUndefined(flag);
    JS_FreeValue(ctx, flag);
    JS_FreeValue(ctx, slots);
    return ok;
}

bool abort_signal_aborted(JSContext *ctx, JSValueConst sig)
{
    JSValue slots = signal_slots(ctx, sig);
    bool b = JS_IsObject(slots) && signal_is_aborted(ctx, slots);
    JS_FreeValue(ctx, slots);
    return b;
}

JSValue abort_signal_reason(JSContext *ctx, JSValueConst sig)
{
    JSValue slots = signal_slots(ctx, sig), v;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    v = signal_is_aborted(ctx, slots) ? JS_GetPropertyStr(ctx, slots, "reason") : JS_UNDEFINED;
    JS_FreeValue(ctx, slots);
    return v;
}

/* ---- §3.2 "signal abort", THE WHOLE OPERATION ------------------------------------------------------------- */

enum { SA_START = 0, SA_ALGOS, SA_FIRE, SA_DONE };

void abort_signal_work_start(AbortSignalWork *w)
{
    int k;
    /* A step state arrives ZEROED, and a zeroed JSValue is the INTEGER 0 rather than undefined. */
    w->stage = SA_START;
    w->phase = 0;
    w->i = 0;
    w->algos = w->ev = JS_UNDEFINED;
    for (k = 0; k < 4; k++) w->cb[k] = JS_UNDEFINED;
}

void abort_signal_work_visit(JSContext *ctx, AbortSignalWork *w, JSStepVisit *v)
{
    int k;
    v->val(ctx, &w->algos);
    v->val(ctx, &w->ev);
    for (k = 0; k < 4; k++) v->val(ctx, &w->cb[k]);
}

void abort_signal_work_release(JSContext *ctx, AbortSignalWork *w)
{
    int k;
    JS_FreeValue(ctx, w->algos);
    JS_FreeValue(ctx, w->ev);
    w->algos = w->ev = JS_UNDEFINED;
    for (k = 0; k < 4; k++) { JS_FreeValue(ctx, w->cb[k]); w->cb[k] = JS_UNDEFINED; }
}

int abort_signal_run(JSContext *ctx, AbortSignalWork *w, JSValueConst sig, JSValueConst reason,
                     JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (w->stage == SA_START) {
        if (!signal_abort_state(ctx, sig, JS_DupValue(ctx, reason))) {
            /* Already aborted: §3.2 step 1 returns, so nothing runs and nothing fires. */
            JS_FreeValue(ctx, in);
            w->stage = SA_DONE;
            return 0;
        }
        /* THE LIST IS SNAPSHOT AND EMPTIED BEFORE THE FIRST ALGORITHM RUNS. §3.2 empties it as its own step,
           and an algorithm is free to abort another signal that shares this one's list-manipulating code; a
           walk over the live array would then run an entry that was added after the operation began, which
           the spec's "for each algorithm of signal's abort algorithms" over an emptied list cannot. */
        w->algos = signal_algos(ctx, sig, 0);
        {
            JSValue slots = signal_slots(ctx, sig);
            if (JS_IsObject(slots))
                JS_SetPropertyStr(ctx, slots, "algorithms", JS_UNDEFINED);
            JS_FreeValue(ctx, slots);
        }
        w->i = 0;
        w->stage = SA_ALGOS;
    }

    while (w->stage == SA_ALGOS) {
        JSValue out;
        if (!JS_IsArray(w->algos) || w->i >= array_len(ctx, w->algos)) { w->stage = SA_FIRE; break; }
        {
            JSValue fn = JS_GetPropertyUint32(ctx, w->algos, w->i);
            r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), fn, JS_UNDEFINED, 0, NULL, in, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
        }
        if (r > 0) return r;
        in = JS_UNDEFINED;
        if (JS_IsException(out)) return -1;
        JS_FreeValue(ctx, out);
        w->i++;
    }

    if (w->stage == SA_FIRE) {
        /* §3.2 "fire an event named abort at signal" — the ONE §2.9 dispatch, as a REQUEST, so the listeners
           run as ordinary preemptible page code and the caller resumes after every one of them has returned. */
        if (JS_IsUndefined(w->ev))
            w->ev = event_new(ctx, "abort", /*bubbles*/ false, /*cancelable*/ false);
        r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), sig, w->ev, in, NULL, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        w->stage = SA_DONE;
    }
    DCHECK(w->stage == SA_DONE, "a signal-abort request resumed in a stage it never parks in");
    return 0;
}

/* §3.2 abort() — A MACHINE, because the spec makes the dispatch SYNCHRONOUS: a page that calls ac.abort() and
   then reads a flag its listener set must see it already set, and a queued fire answers after abort() returned.
   Every step before the fire runs none of the page's code (the slot reads are own slots, the default reason is
   an engine-built DOMException), so the machine has exactly one suspension point: the listeners. */
typedef struct JSAbortState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   started;
    JSValue   sig;      /* the signal being aborted (owned) */
    AbortSignalWork w;  /* the shared "signal abort" request's own record */
} JSAbortState;

static void js_abort_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSAbortState *s = st;
    v->val(ctx, &s->sig);
    abort_signal_work_visit(ctx, &s->w, v);
}

static JSValue js_abort_fini(JSContext *ctx, void *st, bool take_result)
{
    JSAbortState *s = st;
    (void)take_result;
    JS_FreeValue(ctx, s->sig);
    s->sig = JS_UNDEFINED;
    abort_signal_work_release(ctx, &s->w);
    return JS_UNDEFINED;   /* §3.2 abort() returns undefined whatever the listeners did */
}

static int js_abort_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSAbortState *s = st;
    int r;

    if (!s->started) {
        /* §3.2 step 1 is `this.[[Signal]]` — an INTERNAL SLOT. Read as an own slot, so no accessor and no proxy
           trap can sit on it: a page that assigns over the public `signal` property does not redirect abort(). */
        JSValue slots = signal_slots(ctx, s->hdr.this_val);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->sig = JS_UNDEFINED;
        abort_signal_work_start(&s->w);
        s->started = 1;
        if (JS_IsObject(slots))
            s->sig = JS_GetPropertyStr(ctx, slots, "signal");
        JS_FreeValue(ctx, slots);
        if (!JS_IsObject(s->sig)) {
            JS_ThrowTypeError(ctx, "abort called on something that is not an AbortController");
            return JS_STEP_ABRUPT;
        }
    }
    /* THE WHOLE OF "signal abort", not a piece of it: the reason travels verbatim (the operation is what turns
       an undefined one into an AbortError), the signal's abort algorithms run, and then the `abort` event is
       fired at it. abort() has one thing to do and this is it. */
    r = abort_signal_run(ctx, &s->w, s->sig, step_arg(&s->hdr, 0), cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_abort_def = {
    sizeof(JSAbortState), js_abort_step, js_abort_fini, 0, .visit = js_abort_visit
};
static int g_abort_stepid = -1;


JSValue abort_signal_new(JSContext *ctx)
{
    return signal_new(ctx, JS_FALSE, JS_UNDEFINED);
}

/* §3.2's `[SameObject] readonly attribute AbortSignal signal` — an ACCESSOR on the prototype reading the
   [[Signal]] slot, which is where the IDL puts it. It was an own DATA property, and the difference is not
   cosmetic: a page could assign over `controller.signal` and hand the next reader a different object while
   abort() went on aborting the real one. [SameObject] is satisfied because the slot holds one signal for the
   controller's whole life. */
static JSValue js_ctrl_get_signal(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = signal_slots(ctx, this_val), sig;
    (void)magic;
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "not an AbortController");
    }
    sig = JS_GetPropertyStr(ctx, slots, "signal");
    JS_FreeValue(ctx, slots);
    return sig;
}

static JSValue js_abort_controller_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValue obj, sig;
    (void)argc; (void)argv;

    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor AbortController requires 'new'");
    DCHECK(JS_IsObject(g_ctrl_proto), "an AbortController was built before its prototype existed");
    obj = JS_NewObjectProto(ctx, g_ctrl_proto);
    if (JS_IsException(obj))
        return obj;
    sig = signal_new(ctx, JS_FALSE, JS_UNDEFINED);
    /* [[Signal]] — the ONE place the signal lives. Both abort() and the `signal` getter read it. */
    {
        JSValue st = idl_slots_new(ctx);
        JSAtom k = JS_ValueToAtom(ctx, g_key);
        CHECK(!JS_IsException(st) && k != JS_ATOM_NULL, "the AbortController slot record allocation failed");
        JS_SetPropertyStr(ctx, st, "signal", sig);
        JS_SetProperty(ctx, obj, k, st);
        JS_FreeAtom(ctx, k);
    }
    return obj;
}

/* AbortSignal.abort(reason) — §3.2: a signal that is already aborted. */
static JSValue js_sig_static_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue reason;
    (void)this_val;
    reason = abort_reason_or_default(ctx, (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED);
    return signal_new(ctx, JS_TRUE, reason);
}

/* AbortSignal.timeout(ms) — the UNKNOWN one, and the one member of this interface that reaches the page's code.
   The flag is concolic because whether the deadline has passed when the code asks depends on wall-clock this
   engine does not model, and both answers lead to code worth reaching. The MACHINE is because
   `[EnforceRange] unsigned long long milliseconds` is ToNumber on whatever was passed, so
   `AbortSignal.timeout({ valueOf() { for(;;){} } })` is the page's loop: it has to suspend and resume at the
   exact stage, which is what step_toint64_run parks on. */
typedef struct JSTimeoutState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    JSValue   result;   /* the signal, once built (owned) */
} JSTimeoutState;

/* WHAT THIS MACHINE OWNS: its answer. The coercion's own in-flight value lives on the header, which the shared
   teardown releases. */
static void js_timeout_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTimeoutState *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_timeout_fini(JSContext *ctx, void *st, bool take_result)
{
    JSTimeoutState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->result);
    return r;
}

static int js_timeout_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTimeoutState *s = st;
    int64_t ms = 0;
    JSValue flag;
    int r;

    if (s->stage == 0) {
        s->result = JS_UNDEFINED;
        s->stage = 1;
    }
    /* The coercion runs whatever the page put in `valueOf`, and its SIDE EFFECTS are observable — so it runs
       even though the duration itself does not change what this engine models. Dropping it would be a quieter
       spec bug than getting the number wrong. */
    r = step_toint64_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &ms, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;

    flag = concolic_new(ctx, "AbortSignal.timeout().aborted", "AbortSignal.timeout().aborted", JS_FALSE);
    CHECK(!JS_IsException(flag), "minting the timeout signal's aborted flag failed");
    s->result = signal_new(ctx, flag, abort_reason_default(ctx, "TimeoutError", "signal timed out"));
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_timeout_def = {
    sizeof(JSTimeoutState), js_timeout_step, js_timeout_fini, 0, .visit = js_timeout_visit
};

/* §3.2's IDL declares no constructor, so `new AbortSignal()` is a TypeError — and so is calling it. The
   interface object exists only to carry the statics and to be the thing `instanceof` names. */
static JSValue js_abort_signal_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* §3.2's PROTOTYPES AND STEP IDS ARE THE AGENT'S. They were built inside abort_install, which runs once per
   DOCUMENT — so a second same-origin realm overwrote both prototypes and every signal the first realm had
   minted lost the object its members came from, which JS_FreeRuntime's gc_obj_list walk reported as a leak of
   the whole graph. A step id is a runtime registration and a prototype is an object; Web IDL wants the
   prototype per realm, and that is the next conversion — but it is one object per AGENT here, never two. */
static void abort_build_agent(JSContext *ctx)
{
    if (g_timeout_stepid < 0) {
        g_abort_rt = JS_GetRuntime(ctx);
        g_timeout_stepid = JS_RegisterStepDef(g_abort_rt, &js_timeout_def);
        g_abort_stepid = JS_RegisterStepDef(g_abort_rt, &js_abort_def);
    }
    if (JS_IsObject(g_sig_proto)) return;

    /* AbortSignal.prototype FIRST: the controller's prototype does not need it, but a signal minted by
       anything at all does, and `abort_signal_new` is reachable from §5.4 the moment this returns. */
    g_sig_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_sig_proto), "the AbortSignal.prototype allocation failed");
    /* §3.2: AbortSignal INHERITS EventTarget, so `addEventListener` and the `onabort` handler attribute are
       reached through the chain rather than copied onto each signal. */
    JS_SetPrototype(ctx, g_sig_proto, event_target_proto());   /* §3.2: `AbortSignal : EventTarget` */
    event_target_install_handlers(ctx, g_sig_proto, EH_SIGNAL);
    JS_DefinePropertyGetSet(ctx, g_sig_proto, JS_NewAtom(ctx, "aborted"),
                            JS_NewCFunction(ctx, js_sig_get_aborted, "get aborted", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyGetSet(ctx, g_sig_proto, JS_NewAtom(ctx, "reason"),
                            JS_NewCFunction(ctx, js_sig_get_reason, "get reason", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_SetPropertyStr(ctx, g_sig_proto, "throwIfAborted",
                      JS_NewCFunction(ctx, js_sig_throw_if_aborted, "throwIfAborted", 0));

    g_ctrl_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_ctrl_proto), "the AbortController.prototype allocation failed");
    JS_DefinePropertyGetSet(ctx, g_ctrl_proto, JS_NewAtom(ctx, "signal"),
                            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_ctrl_get_signal, "get signal", 0,
                                                 JS_CFUNC_getter_magic, 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_SetPropertyStr(ctx, g_ctrl_proto, "abort",
                      JS_NewCFunction2(ctx, NULL, "abort", 0, JS_CFUNC_step, g_abort_stepid));

}

void abort_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctrl, sigctor;

    DCHECK(JS_IsObject(global), "abort_install was given something that is not the global object");
    DCHECK(g_ready, "abort_install ran before abort_init");
    DCHECK(g_abort_rt == NULL || g_abort_rt == JS_GetRuntime(ctx),
           "abort was installed into a second runtime — its step id belongs to the first, and a runtime is an "
           "AGENT");
    abort_build_agent(ctx);

    ctrl = JS_NewCFunction2(ctx, js_abort_controller_ctor, "AbortController", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctrl), "the AbortController constructor allocation failed");
    JS_SetConstructor(ctx, ctrl, g_ctrl_proto);   /* .prototype and .constructor, both directions */
    JS_SetPropertyStr(ctx, (JSValue)global, "AbortController", ctrl);

    sigctor = JS_NewCFunction2(ctx, js_abort_signal_ctor, "AbortSignal", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(sigctor), "the AbortSignal interface object allocation failed");
    JS_SetPropertyStr(ctx, sigctor, "abort",
                      JS_NewCFunction(ctx, js_sig_static_abort, "abort", 0));
    JS_SetPropertyStr(ctx, sigctor, "timeout",
                      JS_NewCFunction2(ctx, NULL, "timeout", 1, JS_CFUNC_step, g_timeout_stepid));
    JS_SetConstructor(ctx, sigctor, g_sig_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "AbortSignal", sigctor);
}
