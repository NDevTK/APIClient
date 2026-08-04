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
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "core/events/event_target.h"
#include "core/dom/abort.h"

/* The private key the signal's internal slots hang off — a Symbol, so a page enumerating its own objects
   cannot see it and cannot collide with it. `g_ready` rather than testing g_key, because a static JSValue is
   zero-initialised and zero is not JS_UNDEFINED. */
static JSValue g_key;
static int g_ready;
/* The id JS_RegisterStepDef handed this runtime for AbortSignal.timeout's machine. One WASM instance is one
   document is one runtime, which is what the install DCHECK holds it to. */
static int g_timeout_stepid = -1;
static JSRuntime *g_abort_rt;

void abort_init(JSContext *ctx)
{
    DCHECK(!g_ready, "abort_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "abortState", false);
    CHECK(!JS_IsException(g_key), "the AbortSignal slot key allocation failed");
    g_ready = 1;
}

void abort_free(JSContext *ctx)
{
    if (!g_ready)
        return;
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;
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

    sig = JS_NewObject(ctx);
    CHECK(!JS_IsException(sig), "the AbortSignal allocation failed");
    st = JS_NewObject(ctx);
    CHECK(!JS_IsException(st), "the AbortSignal slot record allocation failed");
    JS_SetPropertyStr(ctx, st, "aborted", aborted);
    JS_SetPropertyStr(ctx, st, "reason", reason);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the AbortSignal slot key could not be interned");
    JS_SetProperty(ctx, sig, k, st);
    JS_FreeAtom(ctx, k);

    /* §3.2: AbortSignal inherits EventTarget, and `abort` is dispatched at it. */
    event_target_install(ctx, sig);
    event_target_install_handlers(ctx, sig, EH_SIGNAL);   /* §3.2: `attribute EventHandler onabort` */

    JS_DefinePropertyGetSet(ctx, sig, JS_NewAtom(ctx, "aborted"),
                            JS_NewCFunction(ctx, js_sig_get_aborted, "get aborted", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyGetSet(ctx, sig, JS_NewAtom(ctx, "reason"),
                            JS_NewCFunction(ctx, js_sig_get_reason, "get reason", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_SetPropertyStr(ctx, sig, "throwIfAborted",
                      JS_NewCFunction(ctx, js_sig_throw_if_aborted, "throwIfAborted", 0));
    return sig;
}

/* §3.2 "signal abort": set the flag and the reason, then fire `abort` at the signal. Already-aborted is a
   no-op, which is what keeps a double abort() from firing twice. `reason` is CONSUMED. */
static void signal_abort(JSContext *ctx, JSValueConst sig, JSValue reason)
{
    JSValue slots = signal_slots(ctx, sig);

    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, reason);
        return;
    }
    if (signal_is_aborted(ctx, slots)) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, reason);
        return;
    }
    JS_SetPropertyStr(ctx, slots, "aborted", JS_TRUE);
    JS_SetPropertyStr(ctx, slots, "reason", reason);
    JS_FreeValue(ctx, slots);
    /* Each listener runs as its own task on the RUNNING flow — never a JS_Call from C, which is the
       drive-to-completion this engine aborts on. */
    /* §3.2: "fire an event named abort at signal" — it neither bubbles nor is cancelable. */
    event_target_fire(ctx, sig, "abort", /*bubbles*/ false, /*cancelable*/ false);
}

static JSValue js_ctrl_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    /* 3.2 step 1 is `this.[[Signal]]` — an INTERNAL SLOT. Read as an own slot, so no accessor and no proxy trap
       can sit on it: a page that assigns over the public `signal` property does not redirect abort(), and this
       function reaches none of the page's code and therefore needs no stage. */
    JSValue slots = signal_slots(ctx, this_val);
    JSValue sig, reason;

    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "abort called on something that is not an AbortController");
    }
    sig = JS_GetPropertyStr(ctx, slots, "signal");
    JS_FreeValue(ctx, slots);
    if (!JS_IsObject(sig)) {
        JS_FreeValue(ctx, sig);
        return JS_ThrowTypeError(ctx, "abort called on something that is not an AbortController");
    }
    /* §3.2 step 1: an absent reason becomes an "AbortError" DOMException. A PRESENT one is used verbatim,
       including undefined passed explicitly — the spec distinguishes them by argument count. */
    reason = (argc > 0) ? JS_DupValue(ctx, argv[0])
                        : abort_reason_default(ctx, "AbortError", "signal is aborted without reason");
    signal_abort(ctx, sig, reason);
    JS_FreeValue(ctx, sig);
    return JS_UNDEFINED;
}

static JSValue js_abort_controller_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValue obj, sig;
    (void)argc; (void)argv;

    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor AbortController requires 'new'");
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    sig = signal_new(ctx, JS_FALSE, JS_UNDEFINED);
    /* [[Signal]], the slot abort() reads — and separately the [SameObject] `signal` property the page reads.
       Two names for one object on purpose: the spec's algorithm uses the slot, the page uses the property, and
       overwriting the property must not change what abort() aborts. */
    {
        JSValue st = JS_NewObject(ctx);
        JSAtom k = JS_ValueToAtom(ctx, g_key);
        CHECK(!JS_IsException(st) && k != JS_ATOM_NULL, "the AbortController slot record allocation failed");
        JS_SetPropertyStr(ctx, st, "signal", JS_DupValue(ctx, sig));
        JS_SetProperty(ctx, obj, k, st);
        JS_FreeAtom(ctx, k);
    }
    JS_SetPropertyStr(ctx, obj, "signal", sig);
    JS_SetPropertyStr(ctx, obj, "abort", JS_NewCFunction(ctx, js_ctrl_abort, "abort", 0));
    return obj;
}

/* AbortSignal.abort(reason) — §3.2: a signal that is already aborted. */
static JSValue js_sig_static_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue reason;
    (void)this_val;
    reason = (argc > 0) ? JS_DupValue(ctx, argv[0])
                        : abort_reason_default(ctx, "AbortError", "signal is aborted without reason");
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

void abort_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctrl, sigctor;

    DCHECK(JS_IsObject(global), "abort_install was given something that is not the global object");
    DCHECK(g_ready, "abort_install ran before abort_init");
    DCHECK(g_abort_rt == NULL || g_abort_rt == JS_GetRuntime(ctx),
           "abort was installed into a second runtime — its step id belongs to the first, and one WASM instance "
           "is one document");
    if (g_timeout_stepid < 0) {
        g_abort_rt = JS_GetRuntime(ctx);
        g_timeout_stepid = JS_RegisterStepDef(g_abort_rt, &js_timeout_def);
    }

    ctrl = JS_NewCFunction2(ctx, js_abort_controller_ctor, "AbortController", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctrl), "the AbortController constructor allocation failed");
    JS_SetPropertyStr(ctx, (JSValue)global, "AbortController", ctrl);

    sigctor = JS_NewCFunction2(ctx, js_abort_signal_ctor, "AbortSignal", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(sigctor), "the AbortSignal interface object allocation failed");
    JS_SetPropertyStr(ctx, sigctor, "abort",
                      JS_NewCFunction(ctx, js_sig_static_abort, "abort", 0));
    JS_SetPropertyStr(ctx, sigctor, "timeout",
                      JS_NewCFunction2(ctx, NULL, "timeout", 1, JS_CFUNC_step, g_timeout_stepid));
    JS_SetPropertyStr(ctx, (JSValue)global, "AbortSignal", sigctor);
}
