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
 *   - AbortSignal.any(signals) IS a machine, and the most of one: `sequence<AbortSignal>` is Web IDL §3.2.21.1's
 *     iterator protocol, which is the page's code at the @@iterator read, the call, every `next()` and every
 *     `done`/`value` read off its result.
 *   - AbortController.abort() is NOT, and the reason is a spec correction rather than a concession: 3.2 uses
 *     `this.[[Signal]]`, an INTERNAL SLOT, not Get(this, "signal"). Reading the public property (which is what
 *     this file did) both ran a page getter from C and let a page that overrides `signal` redirect abort().
 *   - throwIfAborted() and the `aborted`/`reason` getters touch OWN SLOTS ONLY, read with JS_GetOwnSlot, which
 *     is by definition not a lookup and cannot reach an accessor or a proxy trap. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "core/idl_iter.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/timing/timer.h"
#include "core/realm.h"
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
/* PER REALM — §3.7, and here it decides ANSWERS: a C member runs in the realm that DEFINED it. Held in
   quickjs's per-context class-proto slots. */
static JSClassID g_sig_class, g_ctrl_class;
/* The id JS_RegisterStepDef handed this runtime for AbortSignal.timeout's machine. One WASM instance is one
   document is one runtime, which is what the install DCHECK holds it to. */
static int g_timeout_stepid = -1;
static JSRuntime *g_abort_rt;

/* The agent's registrations — the step ids, the two class ids, and the realm-registry declaration. They belong
   to abort_init because the DECLARATION has to happen before the agent's own first realm runs the list. */
static void abort_build_agent(JSContext *ctx);

void abort_init(JSContext *ctx)
{
    DCHECK(!g_ready, "abort_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "abortState", false);
    CHECK(!JS_IsException(g_key), "the AbortSignal slot key allocation failed");
    g_ready = 1;
    abort_build_agent(ctx);
}

void abort_free(JSContext *ctx)
{
    if (!g_ready)
        return;
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;   /* the prototypes are the REALMS' — released with their contexts */
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

/* IS THIS SIGNAL ABORTED, ASKED THE ONLY WAY A C BUILTIN MAY ASK. solver_decide answers with the arm this flow
   takes for a concolic flag and parks the other arm as its own flow; -1 means the flag is an ordinary boolean
   and the real ToBool is the answer. A bare `if` here would pick one arm of an unknown and delete the other's
   code.
   THE ARM IS READ THROUGH SOLVER_ARM, and that is not decoration. The result carries SOLVER_FORKED_BIT when a
   sibling was prepared, so a first-time fork onto the true arm returns 257; this compared the raw value against
   1, took the FALSE arm, and left the flow disagreeing with its own decision vector for the rest of the run.
   The header documented the return as "the arm (0/1)", which is why the mistake was available to make. */
static int signal_is_aborted(JSContext *ctx, JSValueConst slots)
{
    JSValue flag = JS_GetPropertyStr(ctx, slots, "aborted");
    int d = solver_decide(ctx, flag);
    int r;

    if (d < 0) {
        r = JS_ToBool(ctx, flag);
    } else {
        DCHECK(SOLVER_ARM(d) == 0 || SOLVER_ARM(d) == 1,
               "a two-armed decision answered with an arm that is neither of them");
        r = SOLVER_ARM(d) == 1;
    }
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

    {
        /* abort_signal_proto ASSERTS that this realm ran its install — the members live on that prototype, so
           a signal minted before it exists would have none of them.
           IT WEARS THE CLASS, and that is what makes `AbortSignal` a DECLARABLE type. This was a plain
           JS_NewObjectProto, so the only brand an AbortSignal had was its private slot record — which a body
           can test and a DECLARATION cannot, since Web IDL's §3.2.15 conversion in core/idl_args.c compares
           class ids. HTML §7.2.6.10.1's `required AbortSignal signal` is a declared dictionary member, so
           without this the type would have had to be re-stated as a hand-written check in NavigateEvent's
           constructor — the exact duplication a declared type exists to remove. The class already existed for
           its per-realm prototype slot; giving it to the instances too is what every other interface in this
           engine does (core/frame/navigation_history_entry.c mints through the same pair). */
        JSValue sp = abort_signal_proto(ctx);
        sig = JS_NewObjectProtoClass(ctx, sp, g_sig_class);
        JS_FreeValue(ctx, sp);
    }
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

/* ---- §3.2's DEPENDENT SIGNALS ------------------------------------------------------------------------------
 *
 * Three more items on a signal's slot record: `dependent` (a boolean), `sources` and `deps` (§3.2's SOURCE
 * SIGNALS and DEPENDENT SIGNALS). They are JS Arrays on the record for the reason the algorithm list is one —
 * a write to them is an ordinary property write the per-flow COW delta captures, so one arm's dependent
 * subscription is invisible to its sibling and both park to the cold tier for free.
 *
 * THE SPEC CALLS BOTH SETS WEAK AND THESE ARE STRONG, which is over-RETENTION and never a wrong answer: the
 * sets are only ever read to propagate an abort, and quickjs collects the source↔dependent cycle the moment
 * both ends are unreachable. §3.2.1's GC requirement is the OPPOSITE direction — a dependent must not be
 * collected while its sources live — and a strong edge from source to dependent satisfies it outright. */

static JSValue signal_list(JSContext *ctx, JSValueConst sig, const char *name, int create)
{
    JSValue slots = signal_slots(ctx, sig), arr;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, slots, name);
    if (!JS_IsArray(arr) && create) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        CHECK(!JS_IsException(arr), "a signal's dependency list could not be allocated");
        JS_SetPropertyStr(ctx, slots, name, JS_DupValue(ctx, arr));
    }
    JS_FreeValue(ctx, slots);
    return arr;
}

static uint32_t array_len(JSContext *ctx, JSValueConst arr);

static void signal_list_append(JSContext *ctx, JSValueConst sig, const char *name, JSValueConst v)
{
    JSValue arr = signal_list(ctx, sig, name, 1);

    /* The list is created on demand and the allocation is a CHECK, so the only way this is not an array is a
       caller that handed a non-signal — which is a bug in the caller and not a case to skip quietly. */
    DCHECK(JS_IsArray(arr), "a signal list was appended to on something that is not an AbortSignal");
    JS_SetPropertyUint32(ctx, arr, array_len(ctx, arr), JS_DupValue(ctx, v));
    JS_FreeValue(ctx, arr);
}

/* §3.2's `dependent` boolean. */
static bool signal_dependent(JSContext *ctx, JSValueConst sig)
{
    JSValue slots = signal_slots(ctx, sig), v;
    bool b;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return false; }
    v = JS_GetPropertyStr(ctx, slots, "dependent");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, slots);
    return b;
}

/* §3.2 step 4.1.1 of signal abort, and step 2 of "create a dependent abort signal": take a reason that has
   ALREADY been defaulted, with none of "signal abort"'s other steps. `reason` is CONSUMED. */
static void signal_adopt_reason(JSContext *ctx, JSValueConst sig, JSValue reason)
{
    JSValue slots = signal_slots(ctx, sig);

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); JS_FreeValue(ctx, reason); return; }
    JS_SetPropertyStr(ctx, slots, "aborted", JS_TRUE);
    JS_SetPropertyStr(ctx, slots, "reason", reason);
    JS_FreeValue(ctx, slots);
}

/* §3.2 "create a dependent abort signal", over a list held AS A JS ARRAY.
 *
 * THE LIST IS A JS VALUE AND NOT A C VECTOR, and that is the algorithm's shape rather than a convenience. Its
 * one script-visible caller is `AbortSignal.any(sequence<AbortSignal>)`, whose Web IDL conversion SUSPENDS at
 * every element — each `next()` and each `value` read is the page's code — so the half-built list has to be
 * something the flow's snapshot carries and its per-flow COW delta captures. That is exactly the "platform data
 * a flow queues is a JS value, never malloc'd C" rule: an Array's growth is a property write the delta already
 * captures, and a malloc'd vector parked across a suspension is a leak no GC walk can see.
 *
 * THE REALM is `ctx`'s — the algorithm's third parameter — because signal_new mints on that realm's
 * AbortSignal.prototype, which for a step machine is the realm that DEFINED the member. */
static JSValue dependent_signal_new(JSContext *ctx, JSValueConst signals)
{
    JSValue result = signal_new(ctx, JS_FALSE, JS_UNDEFINED);   /* Step 1 */
    uint32_t i, n = array_len(ctx, signals);

    CHECK(!JS_IsException(result), "a dependent AbortSignal could not be allocated");
    /* Step 2: an already-aborted input decides the answer OUTRIGHT — the result is born aborted with that
       signal's reason and registers no dependency at all, which is why the operators' "if internal options's
       signal is aborted, reject and return" test answers correctly on the very first line. */
    for (i = 0; i < n; i++) {
        JSValue sig = JS_GetPropertyUint32(ctx, signals, i);
        bool aborted = abort_signal_aborted(ctx, sig);
        if (aborted)
            signal_adopt_reason(ctx, result, abort_signal_reason(ctx, sig));
        JS_FreeValue(ctx, sig);
        if (aborted)
            return result;
    }
    {   /* Step 3 */
        JSValue slots = signal_slots(ctx, result);
        DCHECK(JS_IsObject(slots), "a signal this component just minted has no slot record");
        JS_SetPropertyStr(ctx, slots, "dependent", JS_TRUE);
        JS_FreeValue(ctx, slots);
    }
    for (i = 0; i < n; i++) {                                   /* Step 4 */
        JSValue sig = JS_GetPropertyUint32(ctx, signals, i);
        if (!signal_dependent(ctx, sig)) {
            signal_list_append(ctx, result, "sources", sig);
            signal_list_append(ctx, sig, "deps", result);
        } else {
            /* Step 4.2: FLATTEN — a dependent input contributes its own SOURCES, never itself, so the graph
               this builds is always exactly one hop deep. */
            JSValue src = signal_list(ctx, sig, "sources", 0);
            uint32_t k, m = JS_IsArray(src) ? array_len(ctx, src) : 0;
            for (k = 0; k < m; k++) {
                JSValue s = JS_GetPropertyUint32(ctx, src, k);
                DCHECK(!abort_signal_aborted(ctx, s) && !signal_dependent(ctx, s),
                       "§3.2 step 4.2.1: a source signal of a dependent signal was aborted or itself dependent "
                       "— the flattening this algorithm performs is what makes that impossible");
                signal_list_append(ctx, result, "sources", s);
                signal_list_append(ctx, s, "deps", result);
                JS_FreeValue(ctx, s);
            }
            JS_FreeValue(ctx, src);
        }
        JS_FreeValue(ctx, sig);
    }
    return result;                                              /* Step 5 */
}

/* THE SAME ALGORITHM REACHED FROM C, for a caller whose list is two locals rather than a converted sequence —
   §11's promise-returning operators, which pair their own controller's signal with the caller's. It builds the
   list and delegates; there is ONE implementation of the algorithm and this is not a second one. */
JSValue abort_signal_dependent_new(JSContext *ctx, JSValueConst *signals, int n)
{
    JSValue list = JS_NewArray(ctx), result;
    int i;

    CHECK(!JS_IsException(list), "the source list of a dependent AbortSignal could not be allocated");
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_DupValue(ctx, signals[i]));
    result = dependent_signal_new(ctx, list);
    JS_FreeValue(ctx, list);
    return result;
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
        CHECK(!JS_IsException(arr), "a signal's abort-algorithm list could not be allocated");
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
    DCHECK(JS_IsArray(arr), "an abort algorithm was registered on something that is not an AbortSignal — every "
                            "caller brands its signal first, so this is a lost registration and not a no-op");
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

JSClassID abort_signal_class(void)
{
    DCHECK(g_sig_class != 0, "AbortSignal's class was asked for before abort_init declared it — a DECLARATION "
                             "that brands against it (HTML §7.2.6.10.1's NavigateEventInit) is made at agent "
                             "init, so core/platform.c's order is what puts this component ahead of it");
    return g_sig_class;
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

enum { SA_START = 0, SA_TAKE, SA_ALGOS, SA_FIRE, SA_DONE };

void abort_signal_work_start(AbortSignalWork *w)
{
    int k;
    /* A step state arrives ZEROED, and a zeroed JSValue is the INTEGER 0 rather than undefined. */
    w->stage = SA_START;
    w->phase = 0;
    w->i = w->j = 0;
    w->targets = w->algos = w->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(w->cb, k) w->cb[k] = JS_UNDEFINED;
}

void abort_signal_work_visit(JSContext *ctx, AbortSignalWork *w, JSStepVisit *v)
{
    int k;
    v->val(ctx, &w->targets);
    v->val(ctx, &w->algos);
    v->val(ctx, &w->ev);
    STEP_CB_FOREACH(w->cb, k) v->val(ctx, &w->cb[k]);
}

void abort_signal_work_release(JSContext *ctx, AbortSignalWork *w)
{
    int k;
    JS_FreeValue(ctx, w->targets);
    JS_FreeValue(ctx, w->algos);
    JS_FreeValue(ctx, w->ev);
    w->targets = w->algos = w->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(w->cb, k) { JS_FreeValue(ctx, w->cb[k]); w->cb[k] = JS_UNDEFINED; }
}

int abort_signal_run(JSContext *ctx, AbortSignalWork *w, JSValueConst sig, JSValueConst reason,
                     JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (w->stage == SA_START) {
        if (!signal_abort_state(ctx, sig, JS_DupValue(ctx, reason))) {
            /* Already aborted: §3.2 step 1 returns, so nothing runs and nothing fires. */
            JS_FreeValue(ctx, in);
            STEP_GOTO(w->stage, SA_DONE, &w->phase, NULL);
            return 0;
        }
        /* STEPS 3-4, ENTIRELY BEFORE ANY OF THE PAGE'S CODE RUNS. Every non-aborted dependent takes THIS
           signal's (already-defaulted) reason now, so the source's own algorithms and `abort` listeners run in
           a world where each dependent already reads `aborted === true`. Deferring a dependent's state to its
           own turn of the walk below would be one turn late and page-visible. */
        w->targets = JS_NewArray(ctx);
        CHECK(!JS_IsException(w->targets), "signal abort: OOM collecting the signals whose abort steps run");
        JS_SetPropertyUint32(ctx, w->targets, 0, JS_DupValue(ctx, sig));
        {
            JSValue deps = signal_list(ctx, sig, "deps", 0);
            uint32_t k, n = JS_IsArray(deps) ? array_len(ctx, deps) : 0;
            for (k = 0; k < n; k++) {
                JSValue d = JS_GetPropertyUint32(ctx, deps, k);
                if (!abort_signal_aborted(ctx, d)) {
                    signal_adopt_reason(ctx, d, abort_signal_reason(ctx, sig));
                    JS_SetPropertyUint32(ctx, w->targets, array_len(ctx, w->targets), JS_DupValue(ctx, d));
                }
                JS_FreeValue(ctx, d);
            }
            JS_FreeValue(ctx, deps);
        }
        w->j = 0;
        STEP_GOTO(w->stage, SA_TAKE, &w->phase, NULL);
    }

    /* STEPS 5-6: "run the abort steps" for the signal, then for each dependent that took its reason. One walk,
       because the two steps ARE the same three sub-steps applied to different signals. */
    for (;;) {
        JSValue cur;

        if (w->stage == SA_DONE)
            break;
        DCHECK(JS_IsArray(w->targets), "a signal-abort request resumed with no target list");
        if (w->j >= array_len(ctx, w->targets)) { STEP_GOTO(w->stage, SA_DONE, &w->phase, NULL); break; }
        cur = JS_GetPropertyUint32(ctx, w->targets, w->j);

        if (w->stage == SA_TAKE) {
            /* THE LIST IS SNAPSHOT AND EMPTIED BEFORE THE FIRST ALGORITHM RUNS. §3.2 empties it as its own
               step, and an algorithm is free to abort another signal that shares this one's list-manipulating
               code; a walk over the live array would then run an entry that was added after the operation
               began, which the spec's "for each algorithm of signal's abort algorithms" over an emptied list
               cannot. */
            JS_FreeValue(ctx, w->algos);
            w->algos = signal_algos(ctx, cur, 0);
            {
                JSValue slots = signal_slots(ctx, cur);
                if (JS_IsObject(slots))
                    JS_SetPropertyStr(ctx, slots, "algorithms", JS_UNDEFINED);
                JS_FreeValue(ctx, slots);
            }
            w->i = 0;
            STEP_GOTO(w->stage, SA_ALGOS, &w->phase, NULL);
        }

        while (w->stage == SA_ALGOS) {
            JSValue out;
            if (!JS_IsArray(w->algos) || w->i >= array_len(ctx, w->algos)) {
                STEP_GOTO(w->stage, SA_FIRE, &w->phase, NULL);
                break;
            }
            {
                JSValue fn = JS_GetPropertyUint32(ctx, w->algos, w->i);
                r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), fn, JS_UNDEFINED, 0, NULL, in, &out,
                                  out_cb, out_argc);
                JS_FreeValue(ctx, fn);
            }
            if (r > 0) { JS_FreeValue(ctx, cur); return r; }
            in = JS_UNDEFINED;
            if (JS_IsException(out)) { JS_FreeValue(ctx, cur); return -1; }
            JS_FreeValue(ctx, out);
            w->i++;
        }

        if (w->stage == SA_FIRE) {
            /* §3.2 "fire an event named abort at signal" — the ONE §2.9 dispatch, as a REQUEST, so the
               listeners run as ordinary preemptible page code and the caller resumes after every one of them
               has returned. */
            if (JS_IsUndefined(w->ev))
                w->ev = event_new(ctx, "abort", /*bubbles*/ false, /*cancelable*/ false);
            r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), cur, w->ev, JS_UNDEFINED, in, NULL, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r > 0) { JS_FreeValue(ctx, cur); return r; }
            if (r < 0) { JS_FreeValue(ctx, cur); return -1; }
            /* The event belongs to the signal it was dispatched at: §2.9 leaves `target` and the dispatch
               flag on it, so the next target gets its own. */
            JS_FreeValue(ctx, w->ev);
            w->ev = JS_UNDEFINED;
            w->j++;
            STEP_GOTO(w->stage, SA_TAKE, &w->phase, NULL);
        }
        JS_FreeValue(ctx, cur);
    }
    JS_FreeValue(ctx, in);
    DCHECK(w->stage == SA_DONE, "a signal-abort request resumed in a stage it never parks in");
    return 0;
}

/* §3.2 abort() — A MACHINE, because the spec makes the dispatch SYNCHRONOUS: a page that calls ac.abort() and
   then reads a flag its listener set must see it already set, and a queued fire answers after abort() returned.
   Every step before the fire runs none of the page's code (the slot reads are own slots, the default reason is
   an engine-built DOMException), so the machine has exactly one suspension point: the listeners. */
typedef struct JSAbortState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   sig;      /* the signal being aborted (owned) */
    AbortSignalWork w;  /* the shared "signal abort" request's own record */
} JSAbortState;

static void js_abort_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSAbortState *s = st;
    v->val(ctx, &s->sig);
    abort_signal_work_visit(ctx, &s->w, v);
}

/* WHERE THIS MACHINE RESTS. §3.2's abort() is one step — "signal abort on this's signal with reason" — and
   that abstract operation is what runs the signal's abort algorithms and fires `abort` at it, which is the
   page's code. So the operand is one stage and the operation is the other; `started` was a private flag
   standing in for exactly that split, with no way to say which of the two a parked flow was at. */
#define ABORT_STAGES(X) \
    X(ABORT_SIGNAL_SLOT, "DOM §3.2 AbortController.abort() (this's [[Signal]], the operand of step 1)") \
    X(ABORT_SIGNAL_RUN,  "DOM §3.2 AbortController.abort() step 1 (signal abort on it: the abort algorithms, " \
                         "then the `abort` event)")
enum { ABORT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ABORT_STEPS[] = { ABORT_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_abort_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSAbortState *s = st;
    int r;

    if (s->hdr.stage == ABORT_SIGNAL_SLOT) {
        /* §3.2 step 1 is `this.[[Signal]]` — an INTERNAL SLOT. Read as an own slot, so no accessor and no proxy
           trap can sit on it: a page that assigns over the public `signal` property does not redirect abort(). */
        JSValue slots = signal_slots(ctx, s->hdr.this_val);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->sig = JS_UNDEFINED;
        abort_signal_work_start(&s->w);
        s->hdr.stage = ABORT_SIGNAL_RUN;
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
    DCHECK(s->hdr.stage == ABORT_SIGNAL_RUN, "abort() resumed into a stage §3.2 does not have");
    r = abort_signal_run(ctx, &s->w, s->sig, step_arg(&s->hdr, 0), cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_abort_def = {
    sizeof(JSAbortState), js_abort_step, NULL, 0,   /* §3.2 abort() returns undefined whatever the listeners did */
    .visit = js_abort_visit,
    .algorithm = "DOM §3.2 AbortController.abort(reason)", .steps = ABORT_STEPS
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
    {
        JSValue cp = JS_GetClassProto(ctx, g_ctrl_class);
        DCHECK(!JS_IsNull(cp), "an AbortController was built in a realm with no AbortController.prototype");
        obj = JS_NewObjectProto(ctx, cp);
        JS_FreeValue(ctx, cp);
    }
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
/* WHERE THIS MACHINE RESTS. The coercion is not part of §3.2's four steps — it is Web IDL's
   `[EnforceRange] unsigned long long milliseconds`, which precedes step 1 and is the page's `valueOf` — so it
   is its own stage. It was folded into the same stage as the build, which is a rest point inside the page's
   code sharing a number with one after it. */
#define TIMEOUT_STAGES(X) \
    X(TIMEOUT_COERCE, "Web IDL §3.2.4.8 [EnforceRange] unsigned long long (converting `milliseconds`, which runs " \
                      "the page's valueOf)") \
    X(TIMEOUT_BUILD,  "DOM §3.2 AbortSignal.timeout steps 1-4 (a new AbortSignal, its aborted state and its " \
                      "TimeoutError reason)")
enum { TIMEOUT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TIMEOUT_STEPS[] = { TIMEOUT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSTimeoutState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
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
    (void)ctx;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* §3.2 STEP 3's COMPLETION STEPS — "queue a global task on the timer task source given global to signal abort
 * given signal and a new TimeoutError DOMException". A machine, because signalling abort RUNS THE PAGE'S CODE:
 * the signal's abort algorithms and then its `abort` listeners, with every dependent signal taking the reason
 * first. HTML §8.6's timer_after performs it at the expiry, on the same task source the page's own timers are
 * on, so a timeout signal is ordered against them the way a browser orders it.
 *
 * THE SIGNAL IS CAPTURED, NOT PASSED. §8.6 performs the completion steps with no arguments — it has none to
 * give — so the one thing this needs travels as closure data, which is what JS_NewStepClosure is for. */
#define TIMEOUT_FIRE_STAGES(X) \
    X(TIMEOUT_FIRE_RUN, "DOM §3.2 AbortSignal.timeout step 3's queued task (signal abort on the timeout " \
                        "signal with a new TimeoutError DOMException: its abort algorithms, then `abort`)")
enum { TIMEOUT_FIRE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TIMEOUT_FIRE_STEPS[] = { TIMEOUT_FIRE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr      hdr;
    AbortSignalWork w;   /* the shared "signal abort" operation's record — one implementation, two callers */
    /* HAS THE WORK RECORD BEEN STARTED — a flag rather than a test on one of its fields, because a step state
       arrives ZEROED and a zeroed JSValue is the INTEGER 0 rather than undefined (abort_signal_work_start says
       so where it sets them). Zero is "not yet", which is the one thing readable before anything has written. */
    uint8_t        started;
} JSTimeoutFireState;

static void js_timeout_fire_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTimeoutFireState *s = st;
    if (s->started) abort_signal_work_visit(ctx, &s->w, v);
}

static int js_timeout_fire_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTimeoutFireState *s = st;
    JSValueConst sig = JS_StepClosureData(&s->hdr, 0);
    JSValue reason;
    int r;

    DCHECK(s->hdr.stage == TIMEOUT_FIRE_RUN,
           "the timeout signal's queued task resumed into a stage §3.2 does not have");
    if (!s->started) {
        abort_signal_work_start(&s->w);
        s->started = 1;
    }
    /* A NEW DOMException EACH TIME, which is the step's own wording: the signal was CREATED holding a default
       reason so `signal.reason` reads sensibly before the deadline, and the abort carries a fresh one. */
    reason = abort_reason_default(ctx, "TimeoutError", "signal timed out");
    r = abort_signal_run(ctx, &s->w, sig, reason, cb_result, out_cb, out_argc);
    JS_FreeValue(ctx, reason);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_timeout_fire_def = {
    sizeof(JSTimeoutFireState), js_timeout_fire_step, NULL, 0,
    .visit = js_timeout_fire_visit,
    .algorithm = "DOM §3.2 AbortSignal.timeout step 3's completion steps",
    .steps = TIMEOUT_FIRE_STEPS
};
static int g_timeout_fire_stepid = -1;

static int js_timeout_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTimeoutState *s = st;
    int64_t ms = 0;
    JSValue flag;
    int r;

    if (s->hdr.stage == TIMEOUT_COERCE) {
        s->result = JS_UNDEFINED;
        /* The coercion runs whatever the page put in `valueOf`, and its SIDE EFFECTS are observable — so it
           runs even though the duration itself does not change what this engine models. Dropping it would be a
           quieter spec bug than getting the number wrong. */
        r = step_toint64_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &ms, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->hdr.stage = TIMEOUT_BUILD;
    }
    DCHECK(s->hdr.stage == TIMEOUT_BUILD, "AbortSignal.timeout resumed into a stage §3.2 does not have");

    flag = concolic_new(ctx, "AbortSignal.timeout().aborted", "AbortSignal.timeout().aborted", JS_FALSE);
    CHECK(!JS_IsException(flag), "minting the timeout signal's aborted flag failed");
    s->result = signal_new(ctx, flag, abort_reason_default(ctx, "TimeoutError", "signal timed out"));
    /* §3.2 STEP 3: "run steps after a timeout given global, \"AbortSignal-timeout\", milliseconds, and the
       following step: queue a global task on the timer task source given global to signal abort given signal
       and a new TimeoutError DOMException."
       THE CONCOLIC FLAG ABOVE IS A DIFFERENT QUESTION and both are needed. It answers "has the deadline passed
       by the time the code asks", which is unknown and forks; this schedules the abort that actually happens,
       which RUNS THE PAGE'S CODE — the signal's abort algorithms and its `abort` listeners, with every
       dependent signal taking the reason first. Without it `AbortSignal.timeout(0).addEventListener('abort',f)`
       never ran f, a fetch's abort algorithm never shut the request down, and `AbortSignal.any([c.signal,
       timeout])` had one arm that could not fire.
       THE SIGNAL TRAVELS AS CLOSURE DATA because §8.6 performs the completion steps with no arguments. */
    if (g_timeout_fire_stepid < 0)
        g_timeout_fire_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_timeout_fire_def);
    {
        JSValueConst data = s->result;
        JSValue steps = JS_NewStepClosure(ctx, g_timeout_fire_stepid, 0, 1, &data);

        CHECK(!JS_IsException(steps),
              "AbortSignal.timeout: the completion steps' callee could not be allocated — a timeout signal "
              "whose abort was never scheduled reads as one that simply never fires");
        timer_after(ctx, (double)ms, steps);
        JS_FreeValue(ctx, steps);
    }
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_timeout_def = {
    sizeof(JSTimeoutState), js_timeout_step, js_timeout_fini, 0, .visit = js_timeout_visit,
    .algorithm = "DOM §3.2 AbortSignal.timeout(milliseconds)", .steps = TIMEOUT_STEPS
};

/* ---- §3.2's AbortSignal.any(signals) ------------------------------------------------------------------------
 *
 * `[NewObject] static AbortSignal any(sequence<AbortSignal> signals)`, and DOM states its steps as exactly one:
 * "return the result of creating a dependent abort signal from signals using AbortSignal and the current realm".
 * So the member IS the dependent-signal machinery above, which is why that had to exist first — an `any` written
 * as "add an algorithm to each input that aborts the result" would be a different algorithm wearing this one's
 * name, one turn late and page-visible (see abort.h).
 *
 * EVERYTHING BEFORE THAT ONE STEP IS WEB IDL §3.2.21's CONVERSION, AND IT IS THE PAGE'S CODE FROM END TO END —
 * the @@iterator read, the call that returns the iterator, the `next` read, every `next()` call and every
 * `done`/`value` read off its result. A C loop over it is the drive-to-completion this engine aborts on, so the
 * member is a machine driving core/idl_iter.h's shared cursor, exactly as URLSearchParams' sequence arm does:
 * `AbortSignal.any({ *[Symbol.iterator]() { while (true) yield c.signal; } })` suspends and resumes at whichever
 * `next()` it was inside, and a sibling flow runs meanwhile.
 *
 * THE ELEMENT TYPE IS CONVERTED AS EACH ELEMENT ARRIVES, NEVER AFTER THE WALK. §3.2.21.1 step 3.3 converts S_i
 * the moment the iterator yielded it, so `AbortSignal.any([sig, 0, sig2])` throws its TypeError with the third
 * element never asked for — collecting first and brand-checking after is one observable operation too many, and
 * the operation count is what a test pins. */
#define ANY_STAGES(X) \
    X(ANY_START,   "Web IDL §3.6 step 5 and §3.2.21 step 1 (`signals` is required, so a call with no argument " \
                   "empties the effective overload set; a non-Object is a TypeError before @@iterator is read)") \
    X(ANY_ELEMENT, "Web IDL §3.2.21.1 step 3 (the next value of `sequence<AbortSignal> signals`, then step 3.3's " \
                   "§3.2.15 interface-type conversion of what the iterator yielded)") \
    X(ANY_CREATE,  "DOM §3.2 AbortSignal.any step 1 (create a dependent abort signal from signals using " \
                   "AbortSignal and the current realm)")
enum { ANY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ANY_STEPS[] = { ANY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSAnyState {
    JSStepHdr  hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    IterCursor cur;      /* §3.2.21.1's protocol over the argument, one value per turn */
    JSValue    signals;  /* the sequence converted so far, an Array (owned) */
    JSValue    result;   /* the dependent signal, once created (owned) */
} JSAnyState;

/* WHAT THIS MACHINE OWNS: the cursor's five in-flight values and its call buffer (the cursor declares its own),
   the list being built, and the answer. A fork mid-conversion gives each arm its own list — two arms of a
   branch inside the page's iterator hand `any()` two different sequences, which is the whole point. */
static void js_any_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSAnyState *s = st;
    iter_cursor_visit(ctx, &s->cur, v);
    v->val(ctx, &s->signals);
    v->val(ctx, &s->result);
}

static JSValue js_any_fini(JSContext *ctx, void *st, bool take_result)
{
    JSAnyState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;

    (void)ctx;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static int js_any_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSAnyState *s = st;
    JSValue in = cb_result;
    int r;

    if (s->hdr.stage == ANY_START) {
        /* §3.6 step 5: `signals` is not optional, so a call with no argument removes the member's only entry
           from the effective overload set and throws — which a page must be able to tell apart from
           `AbortSignal.any([])`, a VALID call yielding a signal that nothing can ever abort.
           THE STATE IS COMPLETE BEFORE EITHER THROW, because the teardown runs through fini either way. */
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        iter_cursor_init(&s->cur);
        s->signals = s->result = JS_UNDEFINED;
        if (s->hdr.argc < 1) {
            JS_ThrowTypeError(ctx, "AbortSignal.any requires 1 argument, but only 0 were passed");
            return JS_STEP_ABRUPT;
        }
        /* §3.2.21 step 1: a non-Object is a TypeError HERE, before @@iterator is asked for. It is not a
           formality — `AbortSignal.any("")` must throw without reading String.prototype[@@iterator], and a
           conversion that starts at the read would iterate a string's characters and fail one step later with
           the wrong error after one operation too many. */
        if (!JS_IsObject(step_arg(&s->hdr, 0))) {
            JS_ThrowTypeError(ctx, "AbortSignal.any: the argument is not an object");
            return JS_STEP_ABRUPT;
        }
        s->signals = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->signals), "AbortSignal.any: the sequence being converted could not be allocated");
        s->hdr.stage = ANY_ELEMENT;
    }

    while (s->hdr.stage == ANY_ELEMENT) {
        r = iter_cursor_run(ctx, &s->hdr, &s->cur, step_arg(&s->hdr, 0), in, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        in = JS_UNDEFINED;
        if (s->cur.done) { s->hdr.stage = ANY_CREATE; break; }
        /* §3.2.15's interface-type conversion: a platform object implementing AbortSignal crosses as itself and
           anything else is a TypeError. The brand is the private slot record, which the page cannot forge. */
        if (!abort_signal_is(ctx, s->cur.value)) {
            JS_ThrowTypeError(ctx, "AbortSignal.any: an element of the sequence is not an AbortSignal");
            return JS_STEP_ABRUPT;
        }
        JS_SetPropertyUint32(ctx, s->signals, array_len(ctx, s->signals), JS_DupValue(ctx, s->cur.value));
    }

    /* Step 1, and the whole of it. It runs none of the page's code — every read it makes is an own slot and
       every list it touches is the engine's — so it is one stage and the machine has nothing left to rest on. */
    DCHECK(s->hdr.stage == ANY_CREATE, "AbortSignal.any resumed into a stage §3.2 does not have");
    s->result = dependent_signal_new(ctx, s->signals);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_any_def = {
    sizeof(JSAnyState), js_any_step, js_any_fini, 0, .visit = js_any_visit,
    .algorithm = "DOM §3.2 AbortSignal.any(signals)", .steps = ANY_STEPS
};
static int g_any_stepid = -1;

/* §3.2's IDL declares no constructor, so `new AbortSignal()` is a TypeError — and so is calling it. The
   interface object exists only to carry the statics and to be the thing `instanceof` names. */
static JSValue js_abort_signal_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* §3.2's STEP IDS AND CLASSES ARE THE AGENT'S; the PROTOTYPES are each realm's — see abort_install_protos.
   A step id is a runtime registration and a class id is one too, so both are minted once for the whole agent;
   the two prototype OBJECTS are Web IDL §3.7's per-realm ones, and the realm registry builds them. */
static void abort_build_agent(JSContext *ctx)
{
    JSClassDef sd = { "AbortSignal" }, cd = { "AbortController" };

    if (g_timeout_stepid < 0) {
        g_abort_rt = JS_GetRuntime(ctx);
        g_timeout_stepid = JS_RegisterStepDef(g_abort_rt, &js_timeout_def);
        g_abort_stepid = JS_RegisterStepDef(g_abort_rt, &js_abort_def);
        g_any_stepid = JS_RegisterStepDef(g_abort_rt, &js_any_def);
    }
    if (g_sig_class) return;
    JS_NewClassID(JS_GetRuntime(ctx), &g_sig_class);
    JS_NewClass(JS_GetRuntime(ctx), g_sig_class, &sd);
    JS_NewClassID(JS_GetRuntime(ctx), &g_ctrl_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ctrl_class, &cd);
    realm_declare_intrinsic(abort_install_protos);
}

/* §3.2's TWO INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM. */
void abort_install_protos(JSContext *ctx)
{
    JSValue sig_p, ctrl_p, prev;

    DCHECK(g_sig_class != 0, "a realm asked for AbortSignal.prototype before the interfaces were declared");
    prev = JS_GetClassProto(ctx, g_sig_class);
    DCHECK(JS_IsNull(prev), "abort_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* AbortSignal.prototype FIRST: the controller's prototype does not need it, but a signal minted by
       anything at all does, and `abort_signal_new` is reachable from §5.4 the moment this returns. */
    sig_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(sig_p), "the AbortSignal.prototype allocation failed");
    /* Web IDL §3.7.3: the interface prototype object carries the interface's identifier as its @@toStringTag,
       which is what makes `Object.prototype.toString.call(controller.signal)` answer "[object AbortSignal]" —
       the brand check a page performs without `instanceof`, and the one wpt asserts about every interface it
       touches. These two were the last interface prototypes in the engine without it. */
    idl_interface_tag(ctx, sig_p, "AbortSignal");
    /* §3.2: AbortSignal INHERITS EventTarget, so `addEventListener` and the `onabort` handler attribute are
       reached through the chain rather than copied onto each signal. */
    event_target_chain(ctx, sig_p);   /* §3.2: `AbortSignal : EventTarget` */
    event_target_install_handlers(ctx, sig_p, EH_SIGNAL);
    {
        JSAtom a = JS_NewAtom(ctx, "aborted"), r = JS_NewAtom(ctx, "reason");
        JS_DefinePropertyGetSet(ctx, sig_p, a,
                                JS_NewCFunction(ctx, js_sig_get_aborted, "get aborted", 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_DefinePropertyGetSet(ctx, sig_p, r,
                                JS_NewCFunction(ctx, js_sig_get_reason, "get reason", 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
        JS_FreeAtom(ctx, r);
    }
    JS_SetPropertyStr(ctx, sig_p, "throwIfAborted",
                      JS_NewCFunction(ctx, js_sig_throw_if_aborted, "throwIfAborted", 0));
    JS_SetClassProto(ctx, g_sig_class, sig_p);

    ctrl_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(ctrl_p), "the AbortController.prototype allocation failed");
    idl_interface_tag(ctx, ctrl_p, "AbortController");
    {
        JSAtom a = JS_NewAtom(ctx, "signal");
        JS_DefinePropertyGetSet(ctx, ctrl_p, a,
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_ctrl_get_signal, "get signal", 0,
                                                     JS_CFUNC_getter_magic, 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    JS_SetPropertyStr(ctx, ctrl_p, "abort",
                      JS_NewCFunction2(ctx, NULL, "abort", 0, JS_CFUNC_step, g_abort_stepid));
    JS_SetClassProto(ctx, g_ctrl_class, ctrl_p);
}

JSValue abort_signal_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_sig_class);
    DCHECK(!JS_IsNull(proto), "AbortSignal.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

void abort_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctrl, sigctor;

    DCHECK(JS_IsObject(global), "abort_install was given something that is not the global object");
    DCHECK(g_ready, "abort_install ran before abort_init");
    DCHECK(g_abort_rt == NULL || g_abort_rt == JS_GetRuntime(ctx),
           "abort was installed into a second runtime — its step id belongs to the first, and a runtime is an "
           "AGENT");

    ctrl =JS_NewCFunction2(ctx, js_abort_controller_ctor, "AbortController", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctrl), "the AbortController constructor allocation failed");
    {
        JSValue cp = JS_GetClassProto(ctx, g_ctrl_class);
        JS_SetConstructor(ctx, ctrl, cp);   /* .prototype and .constructor, both directions */
        JS_FreeValue(ctx, cp);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "AbortController", ctrl);

    sigctor = JS_NewCFunction2(ctx, js_abort_signal_ctor, "AbortSignal", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(sigctor), "the AbortSignal interface object allocation failed");
    JS_SetPropertyStr(ctx, sigctor, "abort",
                      JS_NewCFunction(ctx, js_sig_static_abort, "abort", 0));
    JS_SetPropertyStr(ctx, sigctor, "timeout",
                      JS_NewCFunction2(ctx, NULL, "timeout", 1, JS_CFUNC_step, g_timeout_stepid));
    /* §3.2's third static. Its ONE required argument is what `length` states, and the sequence conversion
       behind it is the page's code, which is why it is a machine and `abort` beside it is not. */
    JS_SetPropertyStr(ctx, sigctor, "any",
                      JS_NewCFunction2(ctx, NULL, "any", 1, JS_CFUNC_step, g_any_stepid));
    {
        JSValue sp = abort_signal_proto(ctx);
        JS_SetConstructor(ctx, sigctor, sp);
        JS_FreeValue(ctx, sp);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "AbortSignal", sigctor);
}
