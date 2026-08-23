/* OBSERVABLE AND SUBSCRIBER — the Observable standard (WICG, being upstreamed into WHATWG DOM), §2.1 and §2.2.
 *
 * WHAT THIS IS. An Observable is a producer the page describes ONCE — a `SubscribeCallback` that is handed a
 * Subscriber — and that runs whenever something subscribes. Every value it pushes travels through the
 * subscriber's INTERNAL OBSERVERS to the page's `next`/`error`/`complete` callbacks, and every subscription
 * ends exactly once, through one algorithm ("close a subscription"), which signals the subscriber's own
 * AbortSignal and then runs the teardowns the producer registered — in REVERSE insertion order.
 *
 * WHY EVERY PIECE OF IT IS A STEP MACHINE. There is no step of this standard that does not run the page's code:
 * the subscribe callback is the page's, each internal observer's next/error/complete steps invoke the page's
 * callbacks, a teardown is the page's, an abort algorithm is the page's, and the dictionary members
 * (`{next, error, complete}`, `{signal}`) are read with [[Get]], which on a Proxy is the page's trap. A C body
 * calling any of them is the drive-to-completion this engine aborts on, so the whole component is ONE machine
 * whose stages rest at the standard's own steps and whose every callback is a `step_call_run` request. A
 * producer that loops forever suspends at its back-edge like any other flow, and a sibling flow overtakes it.
 *
 * ONE MACHINE, MANY OPERATIONS — the shape Streams §6 already has here. §2.1's four members, §2.2's constructor
 * and subscribe, and the ALGORITHMS that are not members at all (close a subscription, the abort algorithm a
 * SubscribeOptions signal carries) are stages of one state, because they call INTO each other: subscribe's
 * step 10 can throw and become `subscriber.error(E)`, error() closes the subscription, closing runs teardowns.
 * Written as separate machines those edges would each be a C call into a second machine — which is exactly the
 * re-entry the trampoline exists to remove.
 *
 * THE STATE IS JS VALUES ON A PRIVATE-SYMBOL SLOT RECORD, never malloc'd C. A subscriber's internal-observer
 * list and its teardown list are queues a FLOW builds, and §State-isolation says such a queue is a JS Array:
 * its mutations are property writes the per-flow COW delta already captures, so one flow's subscription is
 * invisible to its sibling and both park to the cold tier for free.
 *
 * AN INTERNAL OBSERVER IS A RECORD OF THREE CALLABLES. The standard's "internal observer" is a struct of three
 * ALGORITHMS, which for a script subscription wrap the page's callbacks and for a spec-prose subscription are
 * native steps. Both are JS callables here — a native one is a `JS_NewStepClosure` step machine — so the walk
 * that pushes a value has ONE shape, and a native observer suspends exactly where a script one does. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/dom/abort.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/streams/stream_work.h"
#include "core/events/report_exception.h"
#include "core/dom/observable.h"
#include "core/dom/observable_impl.h"

/* The private key the slot records hang off — a Symbol, so a page enumerating its own objects cannot see it
   and cannot collide with it. `g_ready` rather than testing g_key, because a static JSValue is zero-initialised
   and zero is not JS_UNDEFINED. */
static JSValue    g_key;
static JSAtom     g_katom;
static int        g_ready;
static JSClassID  g_obs_class, g_sub_class;
static JSRuntime *g_obs_rt;
static int        g_op_stepid[OP_N];

/* ---- the slot records ------------------------------------------------------------------------------------
 *
 * An Observable's is `{ callback, subscriber }` — §2.2's "subscribe callback" and its "weak subscriber".
 * A Subscriber's is `{ observers, teardowns, active, signal }` — §2.1's four internal items, with the
 * "subscription controller" represented by the AbortSignal it owns, because §3.2's controller is reachable only
 * through that signal and this component never needs the controller for anything else. */

static JSValue obs_slots(JSContext *ctx, JSValueConst o)
{
    JSValue st;

    DCHECK(g_ready, "an Observable slot record was asked for before the key existed");
    if (!JS_IsObject(o))
        return JS_UNDEFINED;
    /* AN OWN SLOT, never a lookup: a miss on a lookup is the solver's absent-state seam and would mint a
       concolic for an internal slot — right for the page's own reads, wrong here. */
    if (JS_GetOwnSlot(ctx, &st, o, g_katom) <= 0)
        st = JS_UNDEFINED;
    return st;
}

static JSValue slot_get(JSContext *ctx, JSValueConst o, const char *name)
{
    JSValue st = obs_slots(ctx, o), v;

    if (!JS_IsObject(st)) { JS_FreeValue(ctx, st); return JS_UNDEFINED; }
    v = JS_GetPropertyStr(ctx, st, name);
    JS_FreeValue(ctx, st);
    return v;
}

/* `v` is CONSUMED. */
static void slot_set(JSContext *ctx, JSValueConst o, const char *name, JSValue v)
{
    JSValue st = obs_slots(ctx, o);

    DCHECK(JS_IsObject(st), "a slot was written on something this component never gave a slot record");
    JS_SetPropertyStr(ctx, st, name, v);
    JS_FreeValue(ctx, st);
}

static bool observable_is(JSValueConst v) { return JS_GetClassID(v) == g_obs_class; }
static bool subscriber_is(JSValueConst v) { return JS_GetClassID(v) == g_sub_class; }

bool obs_is_observable(JSValueConst v) { return observable_is(v); }
bool obs_is_subscriber(JSValueConst v) { return subscriber_is(v); }
int  obs_stepid(int op)
{
    DCHECK(op >= 0 && op < OP_N, "an Observable operation was asked for by an id the component does not have");
    return g_op_stepid[op];
}

static uint32_t array_len(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = 0;
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* §2.1's `active` boolean. A plain engine-owned flag: nothing about a subscription is unknown to this engine,
   so a concolic here would fork an arm that cannot happen. */
static bool subscriber_active(JSContext *ctx, JSValueConst sub)
{
    JSValue v = slot_get(ctx, sub, "active");
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

/* ---- constructors for the two platform objects ------------------------------------------------------------ */

/* `cb` is CONSUMED. `proto` is BORROWED and may be anything; a non-object means this realm's. */
static JSValue observable_new(JSContext *ctx, JSValueConst proto, JSValue cb)
{
    JSValue o, st, p;

    p = JS_IsObject(proto) ? JS_DupValue(ctx, proto) : JS_GetClassProto(ctx, g_obs_class);
    DCHECK(!JS_IsNull(p), "an Observable was minted in a realm that never ran its install");
    o = JS_NewObjectProtoClass(ctx, p, g_obs_class);
    JS_FreeValue(ctx, p);
    if (JS_IsException(o)) { JS_FreeValue(ctx, cb); return o; }
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "the Observable slot record allocation failed");
    JS_SetPropertyStr(ctx, st, "callback", cb);
    JS_SetPropertyStr(ctx, st, "subscriber", JS_UNDEFINED);
    JS_SetProperty(ctx, o, g_katom, st);
    return o;
}

/* §2.1: a fresh Subscriber — empty observer and teardown lists, active, and its own subscription controller,
   which this component holds as the SIGNAL that controller owns. */
static JSValue subscriber_new(JSContext *ctx)
{
    JSValue o, st, p = JS_GetClassProto(ctx, g_sub_class);

    DCHECK(!JS_IsNull(p), "a Subscriber was minted in a realm that never ran its install");
    o = JS_NewObjectProtoClass(ctx, p, g_sub_class);
    JS_FreeValue(ctx, p);
    if (JS_IsException(o)) return o;
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "the Subscriber slot record allocation failed");
    JS_SetPropertyStr(ctx, st, "observers", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, st, "teardowns", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, st, "active", JS_TRUE);
    JS_SetPropertyStr(ctx, st, "signal", abort_signal_new(ctx));
    JS_SetProperty(ctx, o, g_katom, st);
    return o;
}

/* §2.2.1's "internal observer": three ALGORITHMS, each here a callable or undefined. A script subscription's
   are the page's callbacks; a spec-prose subscription's are step closures. Nothing else distinguishes them,
   which is what makes the push walk one walk. Each argument is BORROWED. */
static JSValue internal_observer_new(JSContext *ctx, JSValueConst n, JSValueConst e, JSValueConst c)
{
    JSValue io = idl_slots_new(ctx);

    CHECK(!JS_IsException(io), "an internal observer record could not be allocated");
    JS_SetPropertyStr(ctx, io, "next", JS_DupValue(ctx, n));
    JS_SetPropertyStr(ctx, io, "error", JS_DupValue(ctx, e));
    JS_SetPropertyStr(ctx, io, "complete", JS_DupValue(ctx, c));
    return io;
}

JSValue obs_internal_observer_new(JSContext *ctx, JSValueConst n, JSValueConst e, JSValueConst c)
{
    return internal_observer_new(ctx, n, e, c);
}

/* §2.3'S PER-SUBSCRIPTION STATE. The standard's operators say "references to all of the following: queue,
   activeInnerSubscription, outerSubscriptionHasCompleted, and idx" — one mutable record shared by the
   subscribe callback and every one of its internal observer's algorithms. It is a JS object because
   §State-isolation says a queue a flow builds is one: its writes are property writes the per-flow COW delta
   already captures, and it parks to the cold tier with the snapshot for free. Null-prototyped and engine-owned,
   so reading it back from C runs none of the page's code. */
JSValue obs_record_new(JSContext *ctx)
{
    JSValue rec = idl_slots_new(ctx);
    CHECK(!JS_IsException(rec), "an operator's per-subscription state record could not be allocated");
    return rec;
}

JSValue obs_rec_get(JSContext *ctx, JSValueConst rec, const char *name)
{
    return JS_GetPropertyStr(ctx, rec, name);
}

void obs_rec_set(JSContext *ctx, JSValueConst rec, const char *name, JSValue v)
{
    DCHECK(JS_IsObject(rec), "an operator state field was written on something that is not a state record");
    JS_SetPropertyStr(ctx, (JSValue)rec, name, v);
}

bool obs_rec_bool(JSContext *ctx, JSValueConst rec, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

int64_t obs_rec_int(JSContext *ctx, JSValueConst rec, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, name);
    int64_t n = 0;
    JS_ToInt64(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

JSValue obs_subscriber_signal(JSContext *ctx, JSValueConst sub)
{
    JSValue sig = slot_get(ctx, sub, "signal");
    DCHECK(abort_signal_is(ctx, sig), "a Subscriber's subscription controller is not an AbortSignal");
    return sig;
}

/* Remove one internal observer from a subscriber's list, by IDENTITY. Compacted in place rather than spliced:
   `splice` is a page-visible method on Array.prototype and this list is the engine's. */
static void observers_remove(JSContext *ctx, JSValueConst sub, JSValueConst io)
{
    JSValue arr = slot_get(ctx, sub, "observers");
    uint32_t i, n, k = 0;

    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return; }
    n = array_len(ctx, arr);
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_VALUE_GET_TAG(v) == JS_VALUE_GET_TAG(io) && JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(io)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        JS_SetPropertyUint32(ctx, arr, k++, v);
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, k));
    JS_FreeValue(ctx, arr);
}

static void array_append(JSContext *ctx, JSValueConst arr, JSValue v)
{
    JS_SetPropertyUint32(ctx, (JSValue)arr, array_len(ctx, arr), v);
}

/* ---- the machine ------------------------------------------------------------------------------------------ */

enum { OBS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OBS_STEPS[] = { OBS_STAGES(JS_STEP_STAGE_LABEL) NULL };


static void js_obs_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSObsState *s = st;
    int k;

    v->val(ctx, &s->result);
    v->val(ctx, &s->obs);
    v->val(ctx, &s->sub);
    v->val(ctx, &s->io);
    v->val(ctx, &s->sig);
    v->val(ctx, &s->list);
    v->val(ctx, &s->value);
    v->val(ctx, &s->creason);
    for (k = 0; k < 3; k++) v->val(ctx, &s->iocb[k]);
    v->val(ctx, &s->src);
    v->val(ctx, &s->st);
    v->val(ctx, &s->op1);
    v->val(ctx, &s->op2);
    v->val(ctx, &s->op3);
    for (k = 0; k < 6; k++) v->val(ctx, &s->cb[k]);
    abort_signal_work_visit(ctx, &s->aw, v);
    stream_work_visit(ctx, &s->sw, v);
    report_exception_work_visit(ctx, &s->rw, v);
    v->val(ctx, &s->rerr);
}

/* DELETED: js_obs_release, which restated js_obs_visit field for field — twenty-two values, three sub-records
   and the same order. The declaration is the one list and the teardown discharges it. */
static JSValue js_obs_fini(JSContext *ctx, void *st, bool take_result)
{
    JSObsState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;

    if (take_result) s->result = JS_UNDEFINED;
    /* §8.1.4.6 step 5's FLAG — the one part of the report record that is not a reference, so no declaration
       names it and leaving it set would put this global in error reporting mode forever. */
    report_exception_work_unlock(ctx, &s->rw);
    return r;
}

/* LEAVING A STAGE WITH A CALL IN FLIGHT is the bug this assert exists for: a stage that holds a request must
   reach the same step_call_run to collect its result, and deciding differently on the way back in leaves the
   phase byte set, so the NEXT request reads that as a resume and answers without ever asking. */
void obs_goto(JSObsState *s, int stage)
{
    DCHECK(s->phase == 0, "an Observable stage was left with a call still in flight");
    s->hdr.stage = (uint16_t)stage;
}

/* Enter §2.1's "close a subscription", returning to `ret` when it finishes. `reason` is CONSUMED; a `has`
   of 0 means the algorithm was given none, which is the difference between `subscriber.complete()` and an
   unsubscribe — one aborts the controller with an AbortError, the other with the caller's reason. */
static void obs_close_enter(JSContext *ctx, JSObsState *s, int has, JSValue reason, int ret)
{
    JS_FreeValue(ctx, s->creason);
    s->creason = reason;
    /* THE SIGNAL-ABORT RECORD IS RESET AT EVERY ENTRY, never once per machine: one invocation can close two
       subscriptions (an unsubscribe whose teardown completes another), and a record left in its DONE stage
       would answer the second close without running anything. */
    abort_signal_work_release(ctx, &s->aw);
    abort_signal_work_start(&s->aw);
    s->has_reason = (uint8_t)has;
    s->next = (uint8_t)ret;
    obs_goto(s, S_CLOSE_ENTER);
}

/* §2.1's THREE MEMBERS AS FUNCTION OBJECTS OF THIS REALM, read off Subscriber.prototype at install time.
   §2.3's operators and §2.3.1's arms are all specified as "run subscriber's next() METHOD", so a native
   algorithm must reach the same function object the page sees rather than mint a second one — and taking the
   reference HERE is what keeps a page that later replaces the property from redirecting the standard's own
   algorithms. Indexed by EM_NEXT/EM_ERROR/EM_COMPLETE/EM_TEARDOWN. */
static int g_sub_fn_slot[EM_N];

/* Run one of those members on this machine's subscriber, returning to `ret`. `value` is CONSUMED. */
void obs_emit_enter(JSContext *ctx, JSObsState *s, int which, JSValue value, int ret)
{
    JS_FreeValue(ctx, s->value);
    s->value = value;
    s->emit = (uint8_t)which;
    s->next = (uint8_t)ret;
    obs_goto(s, S_EMIT_CALL);
}

/* HTML §8.1.4.6 "report an exception", returning to `ret`. `err` is CONSUMED. The record is reset at every
   entry rather than once per machine: one walk reports once per throwing callback. The ALGORITHM belongs to
   core/events/report_exception.c — DOM §2.9's throwing listener and HTML §8.12 Animation frames's animation-frame callback reach
   the same one, and this is its fourth caller. */
void obs_report_enter(JSContext *ctx, JSObsState *s, JSValue err, int ret)
{
    JS_FreeValue(ctx, s->rerr);
    s->rerr = err;
    report_exception_work_release(ctx, &s->rw);
    report_exception_work_start(&s->rw);
    s->rnext = (uint8_t)ret;
    obs_goto(s, S_REPORT);
}

/* The live exception becomes `subscriber.error(E)` — §2.3.1's answer at every one of its throwing steps, and
   §2.3.2/§2.3.3's at every "if an exception E was thrown, then run subscriber's error() method, given E". */
void obs_fail_to(JSContext *ctx, JSObsState *s, int ret)
{
    obs_emit_enter(ctx, s, EM_ERROR, JS_GetException(ctx), ret);
}

static void obs_fail(JSContext *ctx, JSObsState *s)
{
    obs_fail_to(ctx, s, S_DONE);
}

/* §2.2.1 "SUBSCRIBE TO AN OBSERVABLE" REACHED FROM SPEC PROSE, returning to `ret`. It is a CALL of the realm's
   own function object over §2.2.1's machine rather than a jump into S_ATTACH, and that is the whole point: an
   operator subscribes to its source from INSIDE one of its own algorithms, so the two subscriptions' states
   would otherwise be one state. As a call, the inner one is an ordinary machine on the trampoline. */
static int g_sub_native_slot;

void obs_subscribe_enter(JSContext *ctx, JSObsState *s, JSValueConst observable, JSValueConst io,
                         JSValueConst signal, int ret)
{
    /* DUP BEFORE FREE, at every one of the three. An operand slot's incoming value is routinely the SAME value
       the slot already holds — the flatMap/switchMap/catch tails hand `op1` straight back as the Observable
       they just converted — and freeing first would drop the last reference to the argument the very next line
       reads. */
    JSValue a = JS_DupValue(ctx, observable), b = JS_DupValue(ctx, io), c = JS_DupValue(ctx, signal);

    JS_FreeValue(ctx, s->op1); s->op1 = a;
    JS_FreeValue(ctx, s->op2); s->op2 = b;
    JS_FreeValue(ctx, s->op3); s->op3 = c;
    s->snext = (uint8_t)ret;
    obs_goto(s, S_SUB_NATIVE);
}

/* §2.3.1 "convert to an Observable", as a CALL, returning to `ret` with the answer in `s->op1` — or with the
   TypeError live and `s->op1` an exception. The standard's own note asks for exactly this ("we shouldn't
   invoke from() directly … we want to pipe the exceptions to subscriber"), and a call is what gives the
   caller the throw as a VALUE: this machine declares catches_abrupt, so an abrupt request result is an operand
   rather than an unwind. The function object is the realm's OWN reference to §2.3.1's machine, never
   `Observable.from` read off the constructor, which a page may replace. */
static int g_from_fn_slot;

void obs_convert_enter(JSContext *ctx, JSObsState *s, JSValueConst value, int ret)
{
    JSValue v = JS_DupValue(ctx, value);   /* dup before free — see obs_subscribe_enter */

    JS_FreeValue(ctx, s->op1); s->op1 = v;
    s->fnext = (uint8_t)ret;
    obs_goto(s, S_OP_CONVERT);
}

JSValue obs_algo_new(JSContext *ctx, int alg, JSValueConst rec)
{
    JSValueConst data[2];
    JSValue kv = JS_NewInt32(ctx, alg), fn;

    DCHECK(alg >= 0 && alg < OA_N, "an operator internal-observer algorithm was minted with no OA_ id");
    data[0] = kv;
    data[1] = rec;
    fn = JS_NewStepClosure(ctx, g_op_stepid[OP_OP_ALGO], 1, 2, data);
    JS_FreeValue(ctx, kv);
    CHECK(!JS_IsException(fn), "an operator's internal-observer algorithm could not be allocated");
    return fn;
}

JSValue obs_operator_observable(JSContext *ctx, int kind, JSValueConst source, JSValueConst arg)
{
    JSValueConst data[3];
    JSValue kv = JS_NewInt32(ctx, kind), cb;

    DCHECK(kind >= 0 && kind < K_N, "an operator Observable was minted with no K_ id");
    data[0] = kv;
    data[1] = source;
    data[2] = arg;
    cb = JS_NewStepClosure(ctx, g_op_stepid[OP_OP_SUBSCRIBE], 1, 3, data);
    JS_FreeValue(ctx, kv);
    if (JS_IsException(cb))
        return cb;
    return observable_new(ctx, JS_UNDEFINED, cb);
}

static bool sub_signal_aborted(JSContext *ctx, JSObsState *s)
{
    JSValue sig = slot_get(ctx, s->sub, "signal");
    bool b = abort_signal_aborted(ctx, sig);
    JS_FreeValue(ctx, sig);
    return b;
}

/* Register an abort algorithm on this machine's subscriber's subscription controller. `algo` is CONSUMED. */
static void sub_add_algo(JSContext *ctx, JSObsState *s, JSValue algo)
{
    JSValue sig = slot_get(ctx, s->sub, "signal");

    DCHECK(abort_signal_is(ctx, sig), "a Subscriber's subscription controller is not an AbortSignal");
    abort_signal_add_algorithm(ctx, sig, algo);
    JS_FreeValue(ctx, sig);
    JS_FreeValue(ctx, algo);
}

/* §2.3.1: "return a new Observable whose subscribe callback is an algorithm that takes a Subscriber". The
   algorithm is a step closure over the SOURCE, which is the whole of what each arm captures. */
static JSValue observable_from_arm(JSContext *ctx, int op, JSValueConst source)
{
    JSValue cb = JS_NewStepClosure(ctx, g_op_stepid[op], 1, 1, &source);

    if (JS_IsException(cb))
        return cb;
    return observable_new(ctx, JS_UNDEFINED, cb);
}

/* THE ONE STAGE LOOP. `op` is the operation for a fresh entry and is ignored on a resume, where the stage the
   state is parked in says everything. */
static int obs_run(JSContext *ctx, JSObsState *s, int op, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSValue out;
    int r;

    if (s->hdr.stage == S_ENTRY) {
        int k;
        /* A step state arrives ZEROED, and a zeroed JSValue is the INTEGER 0 rather than undefined. */
        s->result = s->obs = s->sub = s->io = s->sig = s->list = s->value = JS_UNDEFINED;
        s->creason = JS_UNDEFINED;
        for (k = 0; k < 3; k++) s->iocb[k] = JS_UNDEFINED;
        s->src = s->st = s->op1 = s->op2 = s->op3 = JS_UNDEFINED;
        for (k = 0; k < 6; k++) s->cb[k] = JS_UNDEFINED;
        abort_signal_work_start(&s->aw);
        stream_work_start(&s->sw);
        report_exception_work_start(&s->rw);
        s->rerr = JS_UNDEFINED;
        s->rnext = 0;
        s->i = 0;
        s->phase = s->next = s->emit = s->member = s->has_sig = s->has_reason = 0;
        s->async = 0;
        s->snext = s->fnext = S_DONE;
        s->kind = s->alg = 0;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;

        switch (op) {
        case OP_CTOR:
            /* §2.2's constructor: `constructor(SubscribeCallback callback)`. The receiver slot of a step
               constructor carries new.target, and undefined there is a plain call. */
            if (JS_IsUndefined(s->hdr.this_val)) {
                JS_ThrowTypeError(ctx, "constructor Observable requires 'new'");
                return JS_STEP_ABRUPT;
            }
            if (!JS_IsFunction(ctx, step_arg(&s->hdr, 0))) {
                JS_ThrowTypeError(ctx, "Observable: the subscribe callback must be a function");
                return JS_STEP_ABRUPT;
            }
            obs_goto(s, S_CTOR_PROTO);
            break;

        case OP_SUBSCRIBE:
            if (!observable_is(s->hdr.this_val)) {
                JS_ThrowTypeError(ctx, "subscribe called on something that is not an Observable");
                return JS_STEP_ABRUPT;
            }
            s->obs = JS_DupValue(ctx, s->hdr.this_val);
            /* §2.2.1 step 1: a detached document subscribes to nothing — the producer is never invoked and
               the observer's dictionary members are never even read. */
            if (!document_fully_active(ctx)) {
                obs_goto(s, S_DONE);
                break;
            }
            obs_goto(s, S_OBSERVER_READ);
            break;

        case OP_NEXT: case OP_ERROR: case OP_COMPLETE: case OP_ADD_TEARDOWN:
            if (!subscriber_is(s->hdr.this_val)) {
                JS_ThrowTypeError(ctx, "a Subscriber member was called on something that is not a Subscriber");
                return JS_STEP_ABRUPT;
            }
            s->sub = JS_DupValue(ctx, s->hdr.this_val);
            /* §2.1's FULLY-ACTIVE GUARD, which every one of these four members opens with: "If the relevant
               global object is a Window whose associated Document is not fully active, then return." It is
               after the brand test and after Web IDL's argument conversion, because those precede step 1 —
               `subscriber.next()` with no arguments is still a TypeError in a detached document. */
            if (!document_fully_active(ctx)) {
                obs_goto(s, S_DONE);
                break;
            }
            if (op == OP_ADD_TEARDOWN) {
                /* §2.1: `addTeardown(VoidFunction teardown)` — a callback function type, so a non-callable is
                   a TypeError before step 1. */
                if (!JS_IsFunction(ctx, step_arg(&s->hdr, 0))) {
                    JS_ThrowTypeError(ctx, "addTeardown: the teardown must be a function");
                    return JS_STEP_ABRUPT;
                }
                if (subscriber_active(ctx, s->sub)) {          /* step 2 */
                    JSValue td = slot_get(ctx, s->sub, "teardowns");
                    DCHECK(JS_IsArray(td), "a Subscriber's teardown list is not an Array");
                    array_append(ctx, td, JS_DupValue(ctx, step_arg(&s->hdr, 0)));
                    JS_FreeValue(ctx, td);
                    obs_goto(s, S_DONE);
                } else {
                    obs_goto(s, S_TEARDOWN_NOW);               /* step 3 */
                }
                break;
            }
            /* §2.1's `next(any value)` and `error(any error)` declare a REQUIRED argument, so a call with
               none is Web IDL's "1 argument required" TypeError before step 1 — not a push of undefined. */
            if (op != OP_COMPLETE && s->hdr.argc < 1) {
                JS_ThrowTypeError(ctx, "%s: 1 argument required, but only 0 present",
                                  op == OP_NEXT ? "next" : "error");
                return JS_STEP_ABRUPT;
            }
            s->emit = (uint8_t)(op == OP_NEXT ? EM_NEXT : op == OP_ERROR ? EM_ERROR : EM_COMPLETE);
            s->value = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            obs_goto(s, S_EMIT_ENTER);
            break;

        case OP_FROM:
            /* §2.3.1 "convert to an Observable" steps 1-2. Everything after them is a probe, and each probe
               is a [[Get]] of the page's. */
            s->obs = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            if (!JS_IsObject(s->obs)) {
                JS_ThrowTypeError(ctx, "Observable.from: a primitive is not convertible to an Observable");
                return JS_STEP_ABRUPT;
            }
            if (observable_is(s->obs)) {
                s->result = JS_DupValue(ctx, s->obs);
                obs_goto(s, S_DONE);
                break;
            }
            obs_goto(s, S_FROM_PROBE_ASYNC);
            break;

        case OP_FROM_ITER: case OP_FROM_ASYNC: case OP_FROM_PROMISE:
            s->obs = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));   /* the source it captured */
            s->sub = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            DCHECK(subscriber_is(s->sub),
                   "§2.3.1's subscribe callback was invoked with something that is not a Subscriber");
            if (op == OP_FROM_PROMISE) { obs_goto(s, S_PROMISE_REACT); break; }
            /* Both iterable arms begin the same way: an aborted subscription produces nothing at all. */
            if (sub_signal_aborted(ctx, s)) { obs_goto(s, S_DONE); break; }
            s->async = (uint8_t)(op == OP_FROM_ASYNC);
            obs_goto(s, S_ITER_METHOD);
            break;

        case OP_ITER_CLOSE:
            s->io = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            obs_goto(s, S_ITER_RETURN_FN);
            break;

        case OP_ASYNC_OK:
            s->sub     = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            s->io      = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
            s->iocb[0] = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 2));
            s->iocb[1] = JS_DupValue(ctx, step_arg(&s->hdr, 0));   /* the iterator result */
            s->async = 1;
            if (sub_signal_aborted(ctx, s)) { obs_goto(s, S_DONE); break; }
            if (!JS_IsObject(s->iocb[1])) {
                /* §2.3.1: "If Type(iteratorResult) is not Object, then run subscriber's error() method with
                   a TypeError" — which is also where a `next` that threw or answered a non-object lands,
                   because both were turned into this promise's settlement. */
                JS_ThrowTypeError(ctx, "an async iterator's next() did not answer an object");
                obs_fail(ctx, s);
                break;
            }
            obs_goto(s, S_ITER_DONE);
            break;

        case OP_ASYNC_ERR: case OP_PROMISE_ERR:
            s->sub = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            obs_emit_enter(ctx, s, EM_ERROR, JS_DupValue(ctx, step_arg(&s->hdr, 0)), S_DONE);
            break;

        case OP_PROMISE_OK:
            s->sub = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            obs_emit_enter(ctx, s, EM_NEXT, JS_DupValue(ctx, step_arg(&s->hdr, 0)), S_PROMISE_DONE);
            break;

        case OP_SUBSCRIBE_NATIVE: {
            /* §2.2.1 "subscribe to an Observable" reached from spec prose: the operands are the operator's
               own — the Observable, an INTERNAL OBSERVER it built, and the signal it chose — so steps 1-4 of
               the member (the ObserverUnion and the SubscribeOptions read) have no counterpart here and the
               algorithm begins at step 5. */
            JSValueConst signal = step_arg(&s->hdr, 2);
            s->obs = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            s->io  = JS_DupValue(ctx, step_arg(&s->hdr, 1));
            DCHECK(observable_is(s->obs),
                   "a spec-prose subscription named something that is not an Observable");
            DCHECK(JS_IsObject(s->io), "a spec-prose subscription carried no internal observer");
            if (abort_signal_is(ctx, signal)) {
                s->sig = JS_DupValue(ctx, signal);
                s->has_sig = 1;
            }
            obs_goto(s, S_ATTACH);
            break;
        }

        case OP_OP_SUBSCRIBE: case OP_OP_ALGO:
        case OP_TAKE_UNTIL: case OP_TAKE: case OP_DROP: case OP_INSPECT:
        case OP_TOARRAY: case OP_FOREACH: case OP_EVERY: case OP_FIRST:
        case OP_LAST: case OP_FIND: case OP_SOME: case OP_REDUCE:
        case OP_WHEN:
            if (obs_ops_entry(ctx, s, op, &r))
                return r;
            break;

        default:
            DCHECK(op == OP_UNSUB_ALGO, "an Observable machine was entered with an operation it does not have");
            /* §2.2.1 step 9.2's abort algorithm, which captured the subscriber, the internal observer it
               registered, and the signal whose reason it closes with. */
            s->sub = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            s->io  = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
            s->sig = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 2));
            DCHECK(subscriber_is(s->sub), "§2.2.1's abort algorithm captured something that is not a Subscriber");
            if (!subscriber_active(ctx, s->sub)) {             /* step 9.2.1 */
                obs_goto(s, S_DONE);
                break;
            }
            observers_remove(ctx, s->sub, s->io);              /* step 9.2.2 */
            {
                JSValue arr = slot_get(ctx, s->sub, "observers");
                uint32_t n = JS_IsArray(arr) ? array_len(ctx, arr) : 0;
                JS_FreeValue(ctx, arr);
                if (n != 0) { obs_goto(s, S_DONE); break; }
            }
            /* step 9.2.3: the last consumer left, so the subscription closes with THIS signal's reason. */
            obs_close_enter(ctx, s, 1, abort_signal_reason(ctx, s->sig), S_DONE);
            break;
        }
    }

    for (;;) {
        switch (s->hdr.stage) {

        case S_CTOR_PROTO: {
            /* Web IDL §3.7.1: the object's prototype is Get(newTarget, "prototype"), which on a subclass with
               a Proxy constructor is the page's trap. */
            JSAtom a = JS_NewAtom(ctx, "prototype");
            JSValue proto;
            r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, a, cb_result, &proto, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            s->result = observable_new(ctx, proto, JS_DupValue(ctx, step_arg(&s->hdr, 0)));
            JS_FreeValue(ctx, proto);
            if (JS_IsException(s->result)) return JS_STEP_ABRUPT;
            obs_goto(s, S_DONE);
            continue;
        }

        case S_OBSERVER_READ: {
            /* §2.2.1 step 3: the ObserverUnion. A callable IS the next steps; an object is a
               SubscriptionObserver, whose three members Web IDL §3.2.17 reads in LEXICOGRAPHIC order —
               complete, error, next — and each read is one [[Get]] of the page's. */
            static const char *const MEMBERS[3] = { "complete", "error", "next" };
            JSValueConst observer = step_arg(&s->hdr, 0);

            if (JS_IsFunction(ctx, observer)) {
                s->iocb[0] = JS_DupValue(ctx, observer);       /* next */
                obs_goto(s, S_OPTIONS_READ);
                continue;
            }
            if (JS_IsUndefined(observer) || JS_IsNull(observer)) {
                obs_goto(s, S_OPTIONS_READ);                   /* `optional ObserverUnion observer = {}` */
                continue;
            }
            if (!JS_IsObject(observer)) {
                JS_ThrowTypeError(ctx, "subscribe: the observer must be a function or an object");
                return JS_STEP_ABRUPT;
            }
            while (s->member < 3) {
                JSAtom a = JS_NewAtom(ctx, MEMBERS[s->member]);
                JSValue v;
                r = step_getprop_run(ctx, &s->hdr, observer, a, cb_result, &v, out_cb, out_argc);
                JS_FreeAtom(ctx, a);
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                cb_result = JS_UNDEFINED;
                /* Web IDL converts a member's TYPE the moment it has read it, so a non-callable `error` is a
                   TypeError before `next` is even asked for. */
                if (!JS_IsUndefined(v) && !JS_IsFunction(ctx, v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "subscribe: an observer member must be a function");
                    return JS_STEP_ABRUPT;
                }
                /* iocb is [next, error, complete]; the READ order is the other one. */
                s->iocb[s->member == 0 ? 2 : s->member == 1 ? 1 : 0] = v;
                s->member++;
            }
            s->member = 0;
            obs_goto(s, S_OPTIONS_READ);
            continue;
        }

        case S_OPTIONS_READ: {
            /* `optional SubscribeOptions options = {}` — one member, `AbortSignal signal`, whose read is the
               page's and whose type is a brand check. */
            JSValueConst options = step_arg(&s->hdr, 1);

            if (JS_IsObject(options)) {
                JSAtom a = JS_NewAtom(ctx, "signal");
                JSValue v;
                r = step_getprop_run(ctx, &s->hdr, options, a, cb_result, &v, out_cb, out_argc);
                JS_FreeAtom(ctx, a);
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                cb_result = JS_UNDEFINED;
                if (!JS_IsUndefined(v)) {
                    if (!abort_signal_is(ctx, v)) {
                        JS_FreeValue(ctx, v);
                        JS_ThrowTypeError(ctx, "subscribe: `signal` must be an AbortSignal");
                        return JS_STEP_ABRUPT;
                    }
                    s->sig = v;
                    s->has_sig = 1;
                } else {
                    JS_FreeValue(ctx, v);
                }
            } else if (!JS_IsUndefined(options) && !JS_IsNull(options)) {
                JS_ThrowTypeError(ctx, "subscribe: the options must be an object");
                return JS_STEP_ABRUPT;
            }
            s->io = internal_observer_new(ctx, s->iocb[0], s->iocb[1], s->iocb[2]);
            obs_goto(s, S_ATTACH);
            continue;
        }

        case S_ATTACH: {
            /* §2.2.1 steps 5-9. */
            JSValue held = slot_get(ctx, s->obs, "subscriber");
            bool reuse = subscriber_is(held) && subscriber_active(ctx, held);

            if (reuse) {
                s->sub = held;                                  /* step 5.1 */
            } else {
                JS_FreeValue(ctx, held);
                s->sub = subscriber_new(ctx);                   /* step 6 */
                if (JS_IsException(s->sub)) { s->sub = JS_UNDEFINED; return JS_STEP_ABRUPT; }
                slot_set(ctx, s->obs, "subscriber", JS_DupValue(ctx, s->sub));   /* step 8 */
            }
            {
                JSValue arr = slot_get(ctx, s->sub, "observers");   /* steps 5.2 and 7 */
                DCHECK(JS_IsArray(arr), "a Subscriber's internal observer list is not an Array");
                array_append(ctx, arr, JS_DupValue(ctx, s->io));
                JS_FreeValue(ctx, arr);
            }
            if (s->has_sig && abort_signal_aborted(ctx, s->sig)) {
                if (reuse) {
                    /* step 5.3.1: the consumer is gone before it arrived, so it simply never joins. */
                    observers_remove(ctx, s->sub, s->io);
                    obs_goto(s, S_DONE);
                    continue;
                }
                /* step 9.1: close the fresh subscriber with the signal's reason. The subscribe callback still
                   runs afterwards — step 10 is not conditional — which is what makes `addTeardown` inside an
                   already-aborted subscription invoke its teardown immediately. */
                obs_close_enter(ctx, s, 1, abort_signal_reason(ctx, s->sig), reuse ? S_DONE : S_INVOKE);
                continue;
            }
            if (s->has_sig) {
                /* steps 5.3.2 / 9.2: the abort algorithm that unregisters this one consumer and closes the
                   subscription when it was the last. */
                JSValueConst data[3];
                JSValue algo;
                data[0] = s->sub;
                data[1] = s->io;
                data[2] = s->sig;
                algo = JS_NewStepClosure(ctx, g_op_stepid[OP_UNSUB_ALGO], 0, 3, data);
                if (JS_IsException(algo)) return JS_STEP_ABRUPT;
                abort_signal_add_algorithm(ctx, s->sig, algo);
                JS_FreeValue(ctx, algo);
            }
            obs_goto(s, reuse ? S_DONE : S_INVOKE);              /* step 5.4 returns; step 10 invokes */
            continue;
        }

        case S_INVOKE: {
            /* §2.2.1 step 10: invoke the subscribe callback with «subscriber» and "rethrow"; an exception E
               becomes subscriber.error(E). This machine declares catches_abrupt, so the throw arrives as a
               VALUE here rather than tearing the machine down. */
            JSValue fn = slot_get(ctx, s->obs, "callback");
            JSValueConst arg = s->sub;
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, JS_UNDEFINED, 1, &arg, cb_result, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) {
                s->emit = EM_ERROR;
                JS_FreeValue(ctx, s->value);
                s->value = JS_GetException(ctx);
                obs_goto(s, S_EMIT_ENTER);
                continue;
            }
            JS_FreeValue(ctx, out);
            obs_goto(s, S_DONE);
            continue;
        }

        case S_EMIT_ENTER:
            /* §2.1's next/error/complete, step 1: an inactive subscriber pushes nothing. error() on one still
               REPORTS, because the standard will not let an error vanish. */
            if (!subscriber_active(ctx, s->sub)) {
                if (s->emit == EM_ERROR) {
                    JSValue e = s->value;
                    s->value = JS_UNDEFINED;
                    obs_report_enter(ctx, s, e, S_DONE);
                    continue;
                }
                obs_goto(s, S_DONE);
                continue;
            }
            if (s->emit == EM_NEXT) {
                obs_goto(s, S_EMIT_WALK);
                continue;
            }
            /* error and complete CLOSE first (step 3), and only then push over a copy of the list.
               §2.1's error() closes WITH the error: `subscriber.error(e)` leaves `subscriber.signal.reason`
               equal to `e`, which is what the standard's own test file asserts and what an upstream operator
               reads to learn why its downstream went away. The draft's prose for step 3 reads "Close this"
               with the operand omitted; the operand is what the tests and Chromium both have. */
            obs_close_enter(ctx, s, s->emit == EM_ERROR,
                            s->emit == EM_ERROR ? JS_DupValue(ctx, s->value) : JS_UNDEFINED, S_EMIT_WALK);
            continue;

        case S_EMIT_WALK: {
            static const char *const ALGO[3] = { "next", "error", "complete" };

            if (!JS_IsArray(s->list)) {
                /* THE LIST IS COPIED BEFORE THE FIRST OBSERVER RUNS. A `next` handler that subscribes again
                   appends to the LIVE list, and the standard's copy is what keeps that new consumer from
                   receiving the value currently in flight. */
                JSValue live = slot_get(ctx, s->sub, "observers");
                uint32_t k, n = JS_IsArray(live) ? array_len(ctx, live) : 0;
                s->list = JS_NewArray(ctx);
                if (JS_IsException(s->list)) { s->list = JS_UNDEFINED; JS_FreeValue(ctx, live); return JS_STEP_ABRUPT; }
                for (k = 0; k < n; k++)
                    JS_SetPropertyUint32(ctx, s->list, k, JS_GetPropertyUint32(ctx, live, k));
                JS_FreeValue(ctx, live);
                s->i = 0;
            }
            while (s->i < (int64_t)array_len(ctx, s->list)) {
                JSValue observer = JS_GetPropertyUint32(ctx, s->list, (uint32_t)s->i);
                JSValue fn = JS_GetPropertyStr(ctx, observer, ALGO[s->emit]);
                JS_FreeValue(ctx, observer);
                if (!JS_IsFunction(ctx, fn)) {
                    JS_FreeValue(ctx, fn);
                    /* §2.2.1: an internal observer with no `error` algorithm has the DEFAULT ERROR ALGORITHM,
                       which reports. `next` and `complete` default to doing nothing. THE CURSOR ADVANCES
                       FIRST, because the report is a dispatch this stage suspends on and resumes into. */
                    s->i++;
                    if (s->emit == EM_ERROR) {
                        obs_report_enter(ctx, s, JS_DupValue(ctx, s->value), S_EMIT_WALK);
                        break;
                    }
                    continue;
                }
                {
                    JSValueConst arg = s->value;
                    int argc = (s->emit == EM_COMPLETE) ? 0 : 1;
                    r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, JS_UNDEFINED, argc, &arg, cb_result,
                                      &out, out_cb, out_argc);
                }
                JS_FreeValue(ctx, fn);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
                /* Every one of these is invoked with "report": a throwing callback is reported and the walk
                   continues, which is why the standard can assert that no exception escapes this loop. */
                s->i++;
                if (JS_IsException(out)) {
                    obs_report_enter(ctx, s, JS_GetException(ctx), S_EMIT_WALK);
                    break;
                }
                JS_FreeValue(ctx, out);
            }
            if (s->hdr.stage == S_REPORT)
                continue;
            JS_FreeValue(ctx, s->list);
            s->list = JS_UNDEFINED;
            obs_goto(s, S_DONE);
            continue;
        }

        case S_CLOSE_ENTER:
            /* §2.1 "close a subscription" steps 1-2. The re-entrancy guard is the whole reason this is one
               algorithm: a teardown that aborts a downstream controller re-enters here while it is running. */
            if (!subscriber_active(ctx, s->sub)) {
                obs_goto(s, s->next);
                continue;
            }
            slot_set(ctx, s->sub, "active", JS_FALSE);
            obs_goto(s, S_CLOSE_ABORT);
            continue;

        case S_CLOSE_ABORT: {
            /* Step 3: signal abort the subscription controller — its abort algorithms run, then `abort` is
               fired at the signal, and both are the page's code. */
            JSValue sig = slot_get(ctx, s->sub, "signal");
            DCHECK(abort_signal_is(ctx, sig), "a Subscriber's subscription controller is not an AbortSignal");
            r = abort_signal_run(ctx, &s->aw, sig, s->has_reason ? (JSValueConst)s->creason : JS_UNDEFINED,
                                 cb_result, out_cb, out_argc);
            JS_FreeValue(ctx, sig);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (r < 0) return JS_STEP_ABRUPT;
            /* The reason has been handed over. */
            JS_FreeValue(ctx, s->creason);
            s->creason = JS_UNDEFINED;
            s->has_reason = 0;
            {
                JSValue td = slot_get(ctx, s->sub, "teardowns");
                JS_FreeValue(ctx, s->list);
                s->list = td;
                s->i = JS_IsArray(td) ? (int64_t)array_len(ctx, td) - 1 : -1;
            }
            obs_goto(s, S_CLOSE_TEARDOWN);
            continue;
        }

        case S_CLOSE_TEARDOWN:
            /* Step 4: each teardown, in REVERSE insertion order, invoked with "report". */
            while (s->i >= 0) {
                JSValue fn;
                /* Step 4.1, and the standard says it runs REPEATEDLY "because each teardown could result in
                   the above Document becoming inactive" — so it is asked inside the loop, per teardown, not
                   once before it. */
                if (!document_fully_active(ctx))
                    break;
                fn = JS_GetPropertyUint32(ctx, s->list, (uint32_t)s->i);
                r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, JS_UNDEFINED, 0, NULL, cb_result, &out,
                                  out_cb, out_argc);
                JS_FreeValue(ctx, fn);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
                s->i--;
                if (JS_IsException(out)) {
                    obs_report_enter(ctx, s, JS_GetException(ctx), S_CLOSE_TEARDOWN);
                    break;
                }
                JS_FreeValue(ctx, out);
            }
            if (s->hdr.stage == S_REPORT)
                continue;
            JS_FreeValue(ctx, s->list);
            s->list = JS_UNDEFINED;
            /* The list is emptied so a second close, or an addTeardown after one, cannot run them twice. */
            {
                JSValue td = slot_get(ctx, s->sub, "teardowns");
                if (JS_IsArray(td)) JS_SetPropertyStr(ctx, td, "length", JS_NewUint32(ctx, 0));
                JS_FreeValue(ctx, td);
            }
            obs_goto(s, s->next);
            continue;

        case S_TEARDOWN_NOW:
            /* §2.1 addTeardown step 3. */
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), step_arg(&s->hdr, 0), JS_UNDEFINED, 0, NULL,
                              cb_result, &out, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) {
                obs_report_enter(ctx, s, JS_GetException(ctx), S_DONE);
                continue;
            }
            JS_FreeValue(ctx, out);
            obs_goto(s, S_DONE);
            continue;

        case S_REPORT:
            /* HTML §8.1.4.6, as a request: the `error` event is dispatched at the global, so the page's
               listeners run here and this stage is where the machine resumes when they return. */
            r = report_exception_run(ctx, &s->rw, s->rerr, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (r < 0) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->rerr);
            s->rerr = JS_UNDEFINED;
            obs_goto(s, s->rnext);
            continue;

        case S_EMIT_CALL: {
            /* §2.3's "run subscriber's next()/error()/complete() METHOD". It is a CALL of this realm's own
               reference to that member, so the whole of §2.1 runs — including its close and its walk — as an
               ordinary machine on the trampoline rather than as C re-entry. */
            JSValue fn = realm_value_get(ctx, g_sub_fn_slot[s->emit]);
            JSValueConst arg = s->value;
            int n = (s->emit == EM_COMPLETE) ? 0 : 1;

            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, s->sub, n, &arg, cb_result, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            DCHECK(!JS_IsException(out),
                   "a Subscriber member threw — §2.1's members invoke every callback with \"report\", so "
                   "nothing they run can raise past them");
            if (JS_IsException(out)) JS_FreeValue(ctx, JS_GetException(ctx));
            else JS_FreeValue(ctx, out);
            obs_goto(s, s->next);
            continue;
        }

        case S_FROM_PROBE_ASYNC: case S_FROM_PROBE_SYNC: {
            /* §2.3.1 steps 3 and "From iterable" step 1. GetMethod, NOT GetIterator: a source with no
               iterator protocol must fall through rather than throw, and only a PRESENT non-callable is a
               TypeError. */
            bool is_async = (s->hdr.stage == S_FROM_PROBE_ASYNC);
            JSAtom a = JS_WellKnownSymbolAtom(is_async ? JS_WKS_ASYNC_ITERATOR : JS_WKS_ITERATOR);
            JSValue m;

            r = step_getprop_run(ctx, &s->hdr, s->obs, a, cb_result, &m, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            if (JS_IsUndefined(m) || JS_IsNull(m)) {
                JS_FreeValue(ctx, m);
                if (is_async) { obs_goto(s, S_FROM_PROBE_SYNC); continue; }
                /* "From Promise": the BRAND, not a `then` read — a thenable that is not a promise falls to
                   the TypeError below, which is what the standard's IsPromise says. */
                if (JS_IsPromise(s->obs)) {
                    s->result = observable_from_arm(ctx, OP_FROM_PROMISE, s->obs);
                    if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return JS_STEP_ABRUPT; }
                    obs_goto(s, S_DONE);
                    continue;
                }
                JS_ThrowTypeError(ctx, "Observable.from: the value is not an Observable, an async iterable, "
                                       "an iterable or a Promise");
                return JS_STEP_ABRUPT;
            }
            if (!JS_IsFunction(ctx, m)) {
                JS_FreeValue(ctx, m);
                JS_ThrowTypeError(ctx, "Observable.from: the iterator method is not callable");
                return JS_STEP_ABRUPT;
            }
            JS_FreeValue(ctx, m);
            s->result = observable_from_arm(ctx, is_async ? OP_FROM_ASYNC : OP_FROM_ITER, s->obs);
            if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            obs_goto(s, S_DONE);
            continue;
        }

        case S_ITER_METHOD: {
            /* GetIterator step 1. The method is read AGAIN here, at subscribe time — the standard's own note
               says so, and its test expectations pin it. */
            JSAtom a = JS_WellKnownSymbolAtom(s->async == 1 ? JS_WKS_ASYNC_ITERATOR : JS_WKS_ITERATOR);
            JSValue m;

            r = step_getprop_run(ctx, &s->hdr, s->obs, a, cb_result, &m, out_cb, out_argc);
            cb_result = JS_UNDEFINED;   /* consumed by the request, whichever way it completed */
            if (r > 0) return r;
            if (r < 0) { obs_fail(ctx, s); continue; }
            if (s->async == 1 && (JS_IsUndefined(m) || JS_IsNull(m))) {
                /* GetIterator(value, async) step 1.b: no %Symbol.asyncIterator% after all, so the SYNC
                   protocol is used and wrapped. */
                JS_FreeValue(ctx, m);
                s->async = 2;
                continue;
            }
            if (!JS_IsFunction(ctx, m)) {
                JS_FreeValue(ctx, m);
                JS_ThrowTypeError(ctx, "the iterator method is not callable");
                obs_fail(ctx, s);
                continue;
            }
            JS_FreeValue(ctx, s->iocb[0]);
            s->iocb[0] = m;
            obs_goto(s, S_ITER_CALL);
            continue;
        }

        case S_ITER_CALL:
            /* GetIterator steps 2-4. */
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->iocb[0], s->obs, 0, NULL, cb_result, &out,
                              out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) { obs_fail(ctx, s); continue; }
            if (!JS_IsObject(out)) {
                JS_FreeValue(ctx, out);
                JS_ThrowTypeError(ctx, "the iterator method did not answer an object");
                obs_fail(ctx, s);
                continue;
            }
            JS_FreeValue(ctx, s->io);
            s->io = out;
            obs_goto(s, S_ITER_NEXTFN);
            continue;

        case S_ITER_NEXTFN: {
            /* GetIterator step 5: Get(iterator, "next"), which is one more of the page's reads. */
            JSAtom a = JS_NewAtom(ctx, "next");
            JSValue m;

            r = step_getprop_run(ctx, &s->hdr, s->io, a, cb_result, &m, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            cb_result = JS_UNDEFINED;   /* consumed by the request, whichever way it completed */
            if (r > 0) return r;
            if (r < 0) { obs_fail(ctx, s); continue; }
            JS_FreeValue(ctx, s->iocb[0]);
            s->iocb[0] = m;
            if (s->async == 2) { obs_goto(s, S_ITER_WRAP); continue; }
            goto iter_registered;
        }

        case S_ITER_WRAP: {
            /* 27.1.4.1 CreateAsyncFromSyncIterator — the one step of GetIterator(obj, async) a host cannot
               perform itself, so the engine owns it. Both operands are CONSUMED. */
            JSValue nextfn = JS_UNDEFINED;
            JSValue wrapper = JS_NewAsyncFromSyncIterator(ctx, s->io, s->iocb[0], &nextfn);
            s->io = wrapper;
            s->iocb[0] = nextfn;
            if (JS_IsException(wrapper)) {
                s->io = JS_UNDEFINED;
                s->iocb[0] = JS_UNDEFINED;
                obs_fail(ctx, s);
                continue;
            }
            s->async = 1;
        }
        /* fall through */
        iter_registered:
            /* §2.3.1: an abort algorithm that closes the iterator, registered only once the record exists. */
            if (sub_signal_aborted(ctx, s)) { obs_goto(s, S_DONE); continue; }
            {
                JSValue algo = JS_NewStepClosure(ctx, g_op_stepid[OP_ITER_CLOSE], 0, 1,
                                                 (JSValueConst *)&s->io);
                if (JS_IsException(algo)) return JS_STEP_ABRUPT;
                sub_add_algo(ctx, s, algo);
            }
            obs_goto(s, S_ITER_STEP);
            continue;

        case S_ITER_STEP:
            /* THE UNBOUNDED WALK'S YIELD. An iterable of the page's size is exactly the shape §scheduler
               calls un-parkable when it runs to completion inside one opcode, so the machine offers the
               scheduler a switch once per iteration; with nobody waiting it is re-entered immediately.
               A REST POINT MAY NOT SIT BETWEEN A REQUEST AND ITS ANSWER, which is why the whole head of this
               stage is behind `phase == 0` — the same fact obs_goto asserts on when a stage is left. Without
               it the re-entry carrying `next()`'s RESULT took the yield instead of the answer and freed that
               result on the way out; the machine then re-entered, answered a call that had already been
               answered with undefined, and reported "an iterator's next() did not answer an object" for every
               iterable there is. An ABRUPT next() made it visible rather than merely wrong: the yield ran with
               the throw still live, which is the one thing a machine may not ask for, and the request check at
               the driver's convergence point named this stage. */
            if (s->phase == 0) {
                if (s->member == 0) { s->member = 1; JS_FreeValue(ctx, cb_result); return JS_STEP_YIELD; }
                s->member = 0;
                if (sub_signal_aborted(ctx, s)) { obs_goto(s, S_DONE); continue; }
            }
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->iocb[0], s->io, 0, NULL, cb_result, &out,
                              out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (s->async) {
                /* nextAlgorithm steps 3-5: a throw becomes a REJECTED nextPromise and a normal result a
                   RESOLVED one, so both settle asynchronously and the object check happens on the way out.
                   Turning either into a synchronous error() here would run the page's handler a microtask
                   too early. */
                s->reject = (uint8_t)(JS_IsException(out) ? 1 : 0);
                JS_FreeValue(ctx, s->sw.value);
                s->sw.value = JS_IsException(out) ? JS_GetException(ctx) : out;
                obs_goto(s, S_ASYNC_PROMISE);
                continue;
            }
            if (JS_IsException(out)) { obs_fail(ctx, s); continue; }
            if (!JS_IsObject(out)) {
                JS_FreeValue(ctx, out);
                JS_ThrowTypeError(ctx, "an iterator's next() did not answer an object");
                obs_fail(ctx, s);
                continue;
            }
            JS_FreeValue(ctx, s->iocb[1]);
            s->iocb[1] = out;
            obs_goto(s, S_ITER_DONE);
            continue;

        case S_ASYNC_PROMISE:
            /* 27.2.4.7 PromiseResolve over what `next` answered — a plain value, a page thenable or a real
               promise, all three covered by calling a capability's resolving function, which is where
               27.2.1.3.2 step 8 reads `then` off the page's object. */
            r = stream_promise_of_run(ctx, &s->sw, s->reject, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            obs_goto(s, S_ASYNC_REACT);
            continue;

        case S_ASYNC_REACT: {
            JSValueConst data[3];
            data[0] = s->sub;
            data[1] = s->io;
            data[2] = s->iocb[0];
            if (stream_react(ctx, s->sw.func, g_op_stepid[OP_ASYNC_OK], g_op_stepid[OP_ASYNC_ERR], data, 3) < 0)
                return JS_STEP_ABRUPT;
            obs_goto(s, S_DONE);
            continue;
        }

        case S_ITER_DONE: {
            /* IteratorComplete: Get(iteratorResult, "done"). */
            JSAtom a = JS_NewAtom(ctx, "done");
            JSValue v;

            r = step_getprop_run(ctx, &s->hdr, s->iocb[1], a, cb_result, &v, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            cb_result = JS_UNDEFINED;   /* consumed by the request, whichever way it completed */
            if (r > 0) return r;
            if (r < 0) { obs_fail(ctx, s); continue; }
            if (JS_ToBool(ctx, v)) {
                JS_FreeValue(ctx, v);
                obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE);
                continue;
            }
            JS_FreeValue(ctx, v);
            obs_goto(s, S_ITER_VALUE);
            continue;
        }

        case S_ITER_VALUE: {
            /* IteratorValue: Get(iteratorResult, "value"), then the subscriber's next() — after which the
               loop returns to its step, whose first act is the signal check the standard puts there. */
            JSAtom a = JS_NewAtom(ctx, "value");
            JSValue v;

            r = step_getprop_run(ctx, &s->hdr, s->iocb[1], a, cb_result, &v, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            cb_result = JS_UNDEFINED;   /* consumed by the request, whichever way it completed */
            if (r > 0) return r;
            if (r < 0) { obs_fail(ctx, s); continue; }
            obs_emit_enter(ctx, s, EM_NEXT, v, S_ITER_STEP);
            continue;
        }

        case S_ITER_RETURN_FN: {
            /* IteratorClose step 4: Get(iterator, "return"). This runs as an ABORT ALGORITHM, so there is
               nobody to hand a throw to — the standard's own IteratorClose(…, NormalCompletion(UNUSED))
               discards it. */
            JSAtom a = JS_NewAtom(ctx, "return");
            JSValue m;

            r = step_getprop_run(ctx, &s->hdr, s->io, a, cb_result, &m, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            cb_result = JS_UNDEFINED;   /* consumed by the request, whichever way it completed */
            if (r > 0) return r;
            if (r < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); obs_goto(s, S_DONE); continue; }
            if (!JS_IsFunction(ctx, m)) { JS_FreeValue(ctx, m); obs_goto(s, S_DONE); continue; }
            JS_FreeValue(ctx, s->iocb[0]);
            s->iocb[0] = m;
            obs_goto(s, S_ITER_RETURN_CALL);
            continue;
        }

        case S_ITER_RETURN_CALL:
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->iocb[0], s->io, 0, NULL, cb_result, &out,
                              out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (JS_IsException(out)) JS_FreeValue(ctx, JS_GetException(ctx));
            else JS_FreeValue(ctx, out);
            obs_goto(s, S_DONE);
            continue;

        case S_PROMISE_REACT: {
            JSValueConst data[1];
            data[0] = s->sub;
            if (stream_react(ctx, s->obs, g_op_stepid[OP_PROMISE_OK], g_op_stepid[OP_PROMISE_ERR], data, 1) < 0)
                return JS_STEP_ABRUPT;
            obs_goto(s, S_DONE);
            continue;
        }

        case S_PROMISE_DONE:
            obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE);
            continue;

        case S_SUB_NATIVE: {
            /* §2.2.1 from spec prose, AS A CALL. The three operands were placed by obs_subscribe_enter; the
               callee is this realm's own §2.2.1 machine, so the nested subscription — the producer it runs,
               the values it pushes back through this operator — is on the trampoline rather than C re-entry. */
            JSValue fn = realm_value_get(ctx, g_sub_native_slot);
            JSValueConst argv[3];

            argv[0] = s->op1;
            argv[1] = s->op2;
            argv[2] = s->op3;
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, JS_UNDEFINED, 3, argv, cb_result, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            /* §2.2.1 lets nothing escape: step 10 turns the producer's throw into subscriber.error(E), and
               every observer algorithm is invoked with "report". */
            DCHECK(!JS_IsException(out),
                   "a spec-prose subscription raised — §2.2.1 catches its producer's throw and reports every "
                   "observer's, so nothing it runs can unwind into the operator that subscribed");
            if (JS_IsException(out)) JS_FreeValue(ctx, JS_GetException(ctx));
            else JS_FreeValue(ctx, out);
            JS_FreeValue(ctx, s->op1); s->op1 = JS_UNDEFINED;
            JS_FreeValue(ctx, s->op2); s->op2 = JS_UNDEFINED;
            JS_FreeValue(ctx, s->op3); s->op3 = JS_UNDEFINED;
            obs_goto(s, s->snext);
            continue;
        }

        case S_OP_CONVERT: {
            /* §2.3.1's convert, AS A CALL — see obs_convert_enter. The answer, or the exception, lands in
               `op1`, which is where the operator stage that asked reads it. */
            JSValue fn = realm_value_get(ctx, g_from_fn_slot);
            JSValueConst arg = s->op1;

            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), fn, JS_UNDEFINED, 1, &arg, cb_result, &out,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            JS_FreeValue(ctx, s->op1);
            s->op1 = out;                      /* the Observable, or JS_EXCEPTION with the throw live */
            obs_goto(s, s->fnext);
            continue;
        }

        case S_DONE:
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;

        default:
            /* §2.3'S STAGES ARE observable_ops.c's. It answers the way this loop's own arms do, and DFAILs on
               a stage neither file claims — so a stage that exists in the enum and nowhere else aborts at the
               resume rather than silently doing nothing. */
            if (obs_ops_stage(ctx, s, &cb_result, out_cb, out_argc, &r))
                return r;
            continue;
        }
    }
}

static int js_obs_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSObsState *s = st;
    return obs_run(ctx, s, s->hdr.arg, cb_result, out_cb, out_argc);
}

/* catches_abrupt: §2.2.1 step 10 invokes the subscribe callback with "rethrow" and CATCHES what it threw, and
   every other callback here is invoked with "report" — so an abrupt request result is this algorithm's VALUE
   at every one of its call sites, never a raise. */
#define OBS_DEF(i) { sizeof(JSObsState), js_obs_step, js_obs_fini, (i), .catches_abrupt = 1, \
                     .visit = js_obs_visit, \
                     .algorithm = "Observable §2.1/§2.2 (the shared machine over the Subscriber and " \
                                  "Observable operations)", \
                     .steps = OBS_STEPS }
#define OBS_DEF8(b) OBS_DEF((b)+0), OBS_DEF((b)+1), OBS_DEF((b)+2), OBS_DEF((b)+3), \
                    OBS_DEF((b)+4), OBS_DEF((b)+5), OBS_DEF((b)+6), OBS_DEF((b)+7)
static const JSTrampStepDef js_obs_defs[OP_N] = {
    OBS_DEF8(0), OBS_DEF8(8), OBS_DEF8(16), OBS_DEF8(24),
};
#undef OBS_DEF8
#undef OBS_DEF

/* ---- the plain accessors ----------------------------------------------------------------------------------
 * §2.1's `active` and `signal` read OWN SLOTS ONLY, which is by definition not a lookup and cannot reach an
 * accessor or a proxy trap — so neither can be the page's code and neither is a machine. */

enum { SUB_ACTIVE = 0, SUB_SIGNAL };

static JSValue js_sub_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    if (!subscriber_is(this_val))
        return JS_ThrowTypeError(ctx, "not a Subscriber");
    if (magic == SUB_ACTIVE)
        return JS_NewBool(ctx, subscriber_active(ctx, this_val));
    DCHECK(magic == SUB_SIGNAL, "a Subscriber attribute ran with a magic §2.1 does not declare");
    return slot_get(ctx, this_val, "signal");
}

static JSValue js_illegal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv)
{
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* ---- install ---------------------------------------------------------------------------------------------- */

void observable_init(JSContext *ctx)
{
    JSClassDef od = { "Observable" }, sd = { "Subscriber" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    int i;

    DCHECK(!g_ready, "observable_init ran twice — one instance is one document");
    DCHECK(g_obs_rt == NULL || g_obs_rt == rt,
           "Observable was declared into a second runtime — its step ids belong to the first, and a runtime "
           "is an AGENT");
    g_obs_rt = rt;
    g_key = JS_NewSymbol(ctx, "observableState", false);
    CHECK(!JS_IsException(g_key), "the Observable slot key allocation failed");
    g_katom = JS_ValueToAtom(ctx, g_key);
    CHECK(g_katom != JS_ATOM_NULL, "the Observable slot key could not be interned");
    g_ready = 1;

    JS_NewClassID(rt, &g_obs_class);
    JS_NewClass(rt, g_obs_class, &od);
    JS_NewClassID(rt, &g_sub_class);
    JS_NewClass(rt, g_sub_class, &sd);

    DCHECK(sizeof(js_obs_defs) / sizeof(js_obs_defs[0]) == OP_N,
           "an Observable operation was declared with no step definition beside it — the table is written in "
           "blocks and OP_N is what says how many of them there are");
    for (i = 0; i < OP_N; i++) {
        g_op_stepid[i] = JS_RegisterStepDef(rt, &js_obs_defs[i]);
        CHECK(g_op_stepid[i] >= 0, "observable: no step id for one of its operations");
    }
    {
        static const char *const FN[EM_N] = {
            "Subscriber.prototype.next", "Subscriber.prototype.error", "Subscriber.prototype.complete",
            "Subscriber.prototype.addTeardown"
        };
        for (i = 0; i < EM_N; i++)
            g_sub_fn_slot[i] = realm_value_declare(ctx, FN[i]);
    }
    g_sub_native_slot = realm_value_declare(ctx, "Observable §2.2.1 subscribe-to (spec prose)");
    g_from_fn_slot    = realm_value_declare(ctx, "Observable §2.3.1 convert-to-an-Observable");
    realm_declare_intrinsic(observable_install_protos);
}

void observable_install_protos(JSContext *ctx)
{
    JSValue obs_p, sub_p, prev;

    DCHECK(g_obs_class != 0, "a realm asked for Observable.prototype before the interfaces were declared");
    prev = JS_GetClassProto(ctx, g_obs_class);
    DCHECK(JS_IsNull(prev), "observable_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    sub_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(sub_p), "the Subscriber.prototype allocation failed");
    idl_interface_tag(ctx, sub_p, "Subscriber");
    idl_install_step_method(ctx, sub_p, "next", 1, g_op_stepid[OP_NEXT]);
    idl_install_step_method(ctx, sub_p, "error", 1, g_op_stepid[OP_ERROR]);
    idl_install_step_method(ctx, sub_p, "complete", 0, g_op_stepid[OP_COMPLETE]);
    idl_install_step_method(ctx, sub_p, "addTeardown", 1, g_op_stepid[OP_ADD_TEARDOWN]);
    idl_install_accessor(ctx, sub_p, "active", js_sub_get, SUB_ACTIVE, -1);
    idl_install_accessor(ctx, sub_p, "signal", js_sub_get, SUB_SIGNAL, -1);
    /* §2.3's ALGORITHMS REACH THESE THREE AS FUNCTION OBJECTS, and they are read OFF the prototype rather
       than minted a second time — a native `subscriber.next(v)` must be the same function the page can see,
       and the reference taken here is the one that keeps working when the page replaces the property. */
    {
        static const char *const N[EM_N] = { "next", "error", "complete", "addTeardown" };
        int k;
        for (k = 0; k < EM_N; k++) {
            JSValue fn = JS_GetPropertyStr(ctx, sub_p, N[k]);
            CHECK(JS_IsFunction(ctx, fn), "observable: a §2.1 member this component just installed is not a "
                                          "function");
            realm_value_set(ctx, g_sub_fn_slot[k], fn);
        }
    }
    JS_SetClassProto(ctx, g_sub_class, sub_p);

    obs_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(obs_p), "the Observable.prototype allocation failed");
    idl_interface_tag(ctx, obs_p, "Observable");
    idl_install_step_method(ctx, obs_p, "subscribe", 0, g_op_stepid[OP_SUBSCRIBE]);
    obs_ops_install(ctx, obs_p);
    JS_SetClassProto(ctx, g_obs_class, obs_p);

    /* §3's `partial interface EventTarget { Observable when(...) }`. A partial interface is the SAME interface,
       so the member goes on EventTarget.prototype rather than onto anything of this component's — which is
       what makes `document.when` and `element.when` and `signal.when` one declaration. */
    {
        JSValue etp = event_target_proto(ctx);
        DCHECK(JS_IsObject(etp), "§3's when() was installed into a realm with no EventTarget.prototype");
        idl_install_step_method(ctx, etp, "when", 1, g_op_stepid[OP_WHEN]);
        JS_FreeValue(ctx, etp);
    }

    /* THE TWO ALGORITHMS SPEC PROSE REACHES WITHOUT A MEMBER, as function objects OF THIS REALM. §2.2.1's
       "subscribe to an Observable" and §2.3.1's "convert to an Observable" are both stated as abstract
       operations precisely so the operators can use them without going through the Web IDL bindings — and a
       page that replaces `Observable.prototype.subscribe` or `Observable.from` must not redirect them. */
    {
        JSValue fn = JS_NewCFunction2(ctx, NULL, "", 3, JS_CFUNC_step, g_op_stepid[OP_SUBSCRIBE_NATIVE]);
        CHECK(JS_IsFunction(ctx, fn), "observable: §2.2.1's spec-prose subscribe could not be minted");
        realm_value_set(ctx, g_sub_native_slot, fn);
        fn = JS_NewCFunction2(ctx, NULL, "", 1, JS_CFUNC_step, g_op_stepid[OP_FROM]);
        CHECK(JS_IsFunction(ctx, fn), "observable: §2.3.1's convert could not be minted");
        realm_value_set(ctx, g_from_fn_slot, fn);
    }
}

void observable_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(JS_IsObject(global), "observable_install was given something that is not the global object");
    DCHECK(g_ready, "observable_install ran before observable_init");

    ctor = JS_NewCFunction2(ctx, NULL, "Observable", 1, JS_CFUNC_step_ctor, g_op_stepid[OP_CTOR]);
    CHECK(!JS_IsException(ctor), "the Observable interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_obs_class);
    DCHECK(!JS_IsNull(proto), "Observable was installed into a realm with no proto build");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    /* §2.3.1's `static Observable from(any value)` lives on the interface object. */
    JS_SetPropertyStr(ctx, ctor, "from",
                      JS_NewCFunction2(ctx, NULL, "from", 1, JS_CFUNC_step, g_op_stepid[OP_FROM]));
    JS_SetPropertyStr(ctx, (JSValue)global, "Observable", ctor);

    ctor = JS_NewCFunction2(ctx, js_illegal_ctor, "Subscriber", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the Subscriber interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_sub_class);
    DCHECK(!JS_IsNull(proto), "Subscriber was installed into a realm with no proto build");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "Subscriber", ctor);
}

void observable_free(JSContext *ctx)
{
    int i;

    if (!g_ready)
        return;
    JS_FreeAtom(ctx, g_katom);
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;      /* the prototypes are the REALMS' — released with their contexts */
    g_katom = JS_ATOM_NULL;
    g_ready = 0;
    g_obs_rt = NULL;
    for (i = 0; i < OP_N; i++) g_op_stepid[i] = -1;
}
