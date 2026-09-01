/* THE OBSERVABLE OPERATORS — the Observable standard §2.3.2 (Observable-returning) and §2.3.3
 * (promise-returning), plus §3's EventTarget.when().
 *
 * WHAT AN OPERATOR IS, EXACTLY. Every one of them is the same three-part shape and the standard writes it out
 * eighteen times: build an Observable whose subscribe callback (a) makes a per-subscription STATE record, (b)
 * builds an INTERNAL OBSERVER whose three algorithms read and write that record, and (c) SUBSCRIBES to the
 * source with the SUBSCRIBER'S OWN subscription controller's signal as the options signal. That last clause is
 * the whole unsubscription story: when the downstream consumer goes away, §2.1's close-a-subscription signals
 * that controller, the upstream subscription's step-9.2 abort algorithm runs, and the chain unwinds itself
 * with nothing here to arrange it.
 *
 * WHY THE STATE IS A JS RECORD AND NOT C. The standard says "references to all of the following: queue,
 * activeInnerSubscription, outerSubscriptionHasCompleted, and idx" — mutable state shared between a subscribe
 * callback and the algorithms it created, living exactly as long as the subscription. §State-isolation says
 * such a thing is a JS value: its writes are property writes the per-flow COW delta already captures, so one
 * arm's flatMap queue is invisible to its sibling and both park to the cold tier for free. A malloc'd C struct
 * captured by pointer would revert the POINTER on a context switch and leave the record reachable from nothing.
 *
 * WHY EVERY ALGORITHM IS A STEP CLOSURE OVER (OA_ id, record). The standard's internal observers are structs
 * of three ALGORITHMS. §2.2.1's push walk calls them, and it must not care whether a given one came from the
 * page's `{next}` object or from spec prose — that is what makes the walk ONE walk. So a native algorithm is a
 * callable of the same kind as a page callback: a JS_NewStepClosure over the id of which algorithm it is and
 * the record it operates on. Nothing here calls anything from C.
 *
 * AND WHY THE FILE IS SEPARATE BUT THE MACHINE IS NOT. See observable_impl.h: §2.3's algorithms subscribe,
 * emit, convert and close, so they are stages of §2.1/§2.2's machine rather than a machine beside it. This
 * file owns those stages; observable.c owns the rest and routes to `obs_ops_stage` for anything it does not
 * have. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/dom/abort.h"
#include "core/events/event_target.h"
#include "core/events/report_exception.h"
#include "core/dom/document.h"
#include "core/dom/observable.h"
#include "core/dom/observable_impl.h"

enum { OBS_STAGES(JS_STEP_STAGE_ENUM) };

/* ---- §3's `dictionary ObservableEventListenerOptions { boolean capture = false; boolean passive; };` -------
 *
 * DECLARED, BECAUSE THE READ IS §3.2.17'S AND NOT THIS FILE'S. `when()` walked it with its own table of
 * names, its own JS_NewAtom per read and its own ToBoolean — the second copy of Web IDL §3.2.17 Dictionary
 * types core/idl_args.h's header forbids, and the reason no IdlDictMember declaration in this engine named
 * `passive`. It had drifted from the one machine in the way that copy always drifts: the two booleans ran
 * JS_ToBool from plain C, which for unknown external input answers ECMAScript §7.1.2 ToBoolean's last step
 * ("Return true") and deletes the false world — idl_dict_bool is where that is NAMED as a member owed a fork.
 *
 * THE TWO MEMBERS ARE DECLARED DIFFERENTLY AND THE IDL IS WHY. `capture` writes `= false`, so it is an
 * IDL_BOOLEAN carrying §3.2.17 step 4.1.5's default and an absent one EXISTS holding false. `passive` writes
 * no default at all, so it is IDL_BOOLEAN_NO_DEFAULT and an absent one does not exist — which is precisely
 * what §3's listener construction reads ("options's passive if this member exists; null otherwise") and what
 * DOM §2.7's default passive value then fills. IDL_BOOLEAN there would fold the two states into one and make
 * `{}` and `{passive:false}` the same registration.
 *
 * The order is §3.2.17's LEXICOGRAPHIC one — capture, then passive — which idl_dict_declare asserts over this
 * array rather than leaving to be remembered. */
static const IdlDictMember WHEN_OPTIONS[] = {
    { "capture", IDL_BOOLEAN,           false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
    { "passive", IDL_BOOLEAN_NO_DEFAULT },
};
static const IdlDictDecl WHEN_OPTIONS_DECL = {
    "ObservableEventListenerOptions", WHEN_OPTIONS,
    (int)(sizeof(WHEN_OPTIONS) / sizeof(WHEN_OPTIONS[0]))
};
/* Its member names, interned once per runtime — they must be live at both halves of a member read, so they
   cannot be made per read. Borrowed from the IDL pool. */
static const JSAtom *g_when_options_atoms;

/* ---- small list helpers ------------------------------------------------------------------------------------
 * The lists here are the ENGINE's — §2.3.2's flatMap queue, §2.3.3's toArray values — so they are walked with
 * own indices and a `length` write rather than through `push`/`shift`, which are page-visible methods on
 * Array.prototype that a page may replace. */

static uint32_t arr_len(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = 0;
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void arr_append(JSContext *ctx, JSValueConst arr, JSValue v)
{
    JS_SetPropertyUint32(ctx, (JSValue)arr, arr_len(ctx, arr), v);
}

/* "Let nextValue be the first item in queue; remove this item from queue." OWNED, or undefined when empty. */
static JSValue arr_shift(JSContext *ctx, JSValueConst arr)
{
    uint32_t i, n = arr_len(ctx, arr);
    JSValue first;

    if (n == 0)
        return JS_UNDEFINED;
    first = JS_GetPropertyUint32(ctx, arr, 0);
    for (i = 1; i < n; i++)
        JS_SetPropertyUint32(ctx, (JSValue)arr, i - 1, JS_GetPropertyUint32(ctx, arr, i));
    JS_SetPropertyStr(ctx, (JSValue)arr, "length", JS_NewUint32(ctx, n - 1));
    return first;
}

/* ---- the state record --------------------------------------------------------------------------------------
 *
 * ONE RECORD SHAPE FOR EVERY OPERATOR, and the fields an operator does not use are simply absent. Two entries
 * are on every one of them:
 *   "sub" — the Subscriber this subscription is for. Every algorithm runs one of ITS members.
 *   "arg" — what the operator was constructed with: the Mapper, the Predicate, the notifier Observable, the
 *           amount, inspect's five handlers, catch's CatchCallback. Immutable for the operator's whole life,
 *           which is why it is copied into each subscription's record rather than read back off the closure. */

static JSValue rec_new(JSContext *ctx, JSValueConst sub, JSValueConst arg)
{
    JSValue rec = obs_record_new(ctx);

    obs_rec_set(ctx, rec, "sub", JS_DupValue(ctx, sub));
    obs_rec_set(ctx, rec, "arg", JS_DupValue(ctx, arg));
    return rec;
}

/* §2.3's ubiquitous "Let options be a new SubscribeOptions whose signal is subscriber's subscription
   controller's signal" — the one line that makes an operator chain unsubscribe itself. OWNED. */
static JSValue rec_sub_signal(JSContext *ctx, JSValueConst rec)
{
    JSValue sub = obs_rec_get(ctx, rec, "sub"), sig;

    DCHECK(obs_is_subscriber(sub), "an operator state record holds something that is not a Subscriber");
    sig = obs_subscriber_signal(ctx, sub);
    JS_FreeValue(ctx, sub);
    return sig;
}

/* A pass-through internal observer: "run subscriber's next()/error()/complete() with what was passed in". Four
   of §2.3.2's operators use exactly this as their source observer and three more as their INNER one. */
static JSValue observer_passthrough(JSContext *ctx, JSValueConst rec)
{
    JSValue n = obs_algo_new(ctx, OA_PASS_NEXT, rec);
    JSValue e = obs_algo_new(ctx, OA_PASS_ERROR, rec);
    JSValue c = obs_algo_new(ctx, OA_PASS_COMPLETE, rec);
    JSValue io = obs_internal_observer_new(ctx, n, e, c);

    JS_FreeValue(ctx, n);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, c);
    return io;
}

/* An observer whose `next` (and sometimes `complete`) is the operator's own and whose other algorithms are the
   pass-throughs. `next_alg`/`complete_alg` of -1 mean "the pass-through". */
static JSValue observer_of(JSContext *ctx, JSValueConst rec, int next_alg, int error_alg, int complete_alg)
{
    JSValue n = obs_algo_new(ctx, next_alg < 0 ? OA_PASS_NEXT : next_alg, rec);
    JSValue e = obs_algo_new(ctx, error_alg < 0 ? OA_PASS_ERROR : error_alg, rec);
    JSValue c = obs_algo_new(ctx, complete_alg < 0 ? OA_PASS_COMPLETE : complete_alg, rec);
    JSValue io = obs_internal_observer_new(ctx, n, e, c);

    JS_FreeValue(ctx, n);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, c);
    return io;
}

/* ---- entering the shared stages ---------------------------------------------------------------------------- */

/* §2.3.3's "signal abort <controller> [with E]" — DOM §3.2's whole operation, as a request. The record is reset
   at every entry rather than once per machine: one algorithm can abort its controller after having already
   closed something else through the same record. */
static void ops_abort_enter(JSContext *ctx, JSObsState *s, JSValueConst sig, int has_reason, JSValue reason,
                            int ret)
{
    JSValue keep = JS_DupValue(ctx, sig);   /* dup before free: the tail aborts the signal held in `op3` */

    JS_FreeValue(ctx, s->op3);
    s->op3 = keep;
    JS_FreeValue(ctx, s->creason);
    s->creason = reason;
    s->has_reason = (uint8_t)has_reason;
    abort_signal_work_release(ctx, &s->aw);
    abort_signal_work_start(&s->aw);
    s->snext = (uint8_t)ret;
    obs_goto(s, S_OP_ABORT);
}

/* §2.3.3's "resolve p with v" / "reject p with e" — a CALL of the capability's resolving function, because
   RESOLVING is where 27.5.1.3 step 2.f reads `then` off whatever it was handed, and that is the page's code
   whenever a page object reaches it. `value` is CONSUMED. */
static void ops_settle_enter(JSContext *ctx, JSObsState *s, JSValueConst rec, int reject, JSValue value,
                             int ret)
{
    JS_FreeValue(ctx, s->op1);
    s->op1 = obs_rec_get(ctx, rec, reject ? "reject" : "resolve");
    DCHECK(JS_IsFunction(ctx, s->op1),
           "an operator settled a promise it never made — §2.3.3's step 1 is \"let p be a new promise\"");
    JS_FreeValue(ctx, s->op2);
    s->op2 = value;
    s->snext = (uint8_t)ret;
    obs_goto(s, S_OP_SETTLE);
}

/* An engine-built exception OBJECT, for the two places §2.3.3 names one: first()/last()'s RangeError on an
   empty source and reduce()'s TypeError on one with no initial value. Built by throwing and taking it back,
   because that is the engine's only constructor for the interface and it runs none of the page's code. */
static JSValue ops_range_error(JSContext *ctx, const char *msg)
{
    JS_ThrowRangeError(ctx, "%s", msg);
    return JS_GetException(ctx);
}

static JSValue ops_type_error(JSContext *ctx, const char *msg)
{
    JS_ThrowTypeError(ctx, "%s", msg);
    return JS_GetException(ctx);
}

/* ---- §2.3.2's METHODS THAT RUN NONE OF THE PAGE'S CODE ------------------------------------------------------
 *
 * map, filter, flatMap, switchMap, catch and finally convert ONE argument each, and every one of those
 * conversions is a Web IDL CALLBACK FUNCTION TYPE — which is a brand check and nothing else. There is no
 * [[Get]], no coercion and no user algorithm between the method's entry and its return, so a step machine here
 * would be ceremony: it would declare a suspension point the standard does not have. The five methods that DO
 * reach the page's code (takeUntil converts, take/drop coerce, inspect reads five dictionary members) are
 * machines, and they are in obs_ops_entry below. */

static JSValue ops_plain(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue arg;

    if (!obs_is_observable(this_val))
        return JS_ThrowTypeError(ctx, "an Observable operator was called on something that is not an "
                                      "Observable");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "1 argument required, but only 0 present");
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "the operator's callback must be a function");
    arg = obs_operator_observable(ctx, magic, this_val, argv[0]);
    return arg;
}

void obs_ops_init(JSContext *ctx)
{
    /* §3's dictionary has no ARGUMENT POSITION to be declared at — `when` registers its own step definition
       like every operation in this component — which is the entry idl_dict_declare is public for. It also
       runs §3.2.17's read-order check over the array above, which is why the walk goes through it rather than
       reaching for JS_NewAtom. Idempotent per runtime. */
    g_when_options_atoms = idl_dict_declare(ctx, &WHEN_OPTIONS_DECL);
}

void obs_ops_free(void)
{
    /* The atoms belong to the IDL pool, which gives them back with the runtime; the HANDLE is this file's,
       and one left pointing into a released pool is a stale slot the next agent would read. */
    g_when_options_atoms = NULL;
}

void obs_ops_install(JSContext *ctx, JSValueConst proto)
{
    static const struct { const char *name; int kind; } PLAIN[] = {
        { "map",       K_MAP },
        { "filter",    K_FILTER },
        { "flatMap",   K_FLATMAP },
        { "switchMap", K_SWITCHMAP },
        { "catch",     K_CATCH },
        { "finally",   K_FINALLY },
    };
    unsigned k;

    for (k = 0; k < sizeof(PLAIN) / sizeof(PLAIN[0]); k++)
        JS_SetPropertyStr(ctx, (JSValue)proto, PLAIN[k].name,
                          JS_NewCFunctionMagic(ctx, ops_plain, PLAIN[k].name, 1, JS_CFUNC_generic_magic,
                                               PLAIN[k].kind));

    idl_install_step_method(ctx, proto, "takeUntil", 1, obs_stepid(OP_TAKE_UNTIL));
    idl_install_step_method(ctx, proto, "take",      1, obs_stepid(OP_TAKE));
    idl_install_step_method(ctx, proto, "drop",      1, obs_stepid(OP_DROP));
    idl_install_step_method(ctx, proto, "inspect",   0, obs_stepid(OP_INSPECT));

    idl_install_step_method(ctx, proto, "toArray",   0, obs_stepid(OP_TOARRAY));
    idl_install_step_method(ctx, proto, "forEach",   1, obs_stepid(OP_FOREACH));
    idl_install_step_method(ctx, proto, "every",     1, obs_stepid(OP_EVERY));
    idl_install_step_method(ctx, proto, "first",     0, obs_stepid(OP_FIRST));
    idl_install_step_method(ctx, proto, "last",      0, obs_stepid(OP_LAST));
    idl_install_step_method(ctx, proto, "find",      1, obs_stepid(OP_FIND));
    idl_install_step_method(ctx, proto, "some",      1, obs_stepid(OP_SOME));
    idl_install_step_method(ctx, proto, "reduce",    1, obs_stepid(OP_REDUCE));
}

/* ---- §2.3.3's shared prologue -------------------------------------------------------------------------------
 *
 * All eight promise-returning operators open with the same six steps, and the ONE thing that differs between
 * them is whether they own an AbortController of their own. toArray and last do not — they never terminate the
 * subscription early, so their internal options signal IS the caller's. The other six do, because their
 * observer decides to stop (every()'s false, first()'s first value, a predicate that threw), and §3.2's
 * "create a dependent abort signal" is what lets one subscription answer to both. */

static int ops_wants_controller(int op)
{
    return op != OP_TOARRAY && op != OP_LAST;
}

/* Which argument position `options` is in, per the IDL. */
static int ops_options_index(int op)
{
    switch (op) {
    case OP_TOARRAY: case OP_FIRST: case OP_LAST: return 0;
    case OP_REDUCE:                               return 2;
    default:                                      return 1;   /* forEach, every, find, some */
    }
}

/* §2.3.3's callback argument, and whether the operator has one at all. */
static const char *ops_callback_name(int op)
{
    switch (op) {
    case OP_FOREACH: return "callback";
    case OP_EVERY: case OP_FIND: case OP_SOME: return "predicate";
    case OP_REDUCE: return "reducer";
    default: return NULL;
    }
}

/* ---- the entry: a fresh invocation whose operation is an operator's ----------------------------------------- */

int obs_ops_entry(JSContext *ctx, JSObsState *s, int op, int *pr)
{
    switch (op) {
    case OP_OP_SUBSCRIBE:
        /* §2.3's "an algorithm that takes a Subscriber subscriber" — the closure captured «kind, source, arg»
           when the operator method built its Observable. */
        s->kind = (uint8_t)JS_VALUE_GET_INT(JS_StepClosureData(&s->hdr, 0));
        s->src  = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
        s->sub  = JS_DupValue(ctx, step_arg(&s->hdr, 0));
        DCHECK(obs_is_subscriber(s->sub),
               "§2.3's subscribe callback was invoked with something that is not a Subscriber");
        s->st = rec_new(ctx, s->sub, JS_StepClosureData(&s->hdr, 2));
        obs_goto(s, S_OP_ENTER);
        return 0;

    case OP_OP_ALGO:
        /* One internal-observer algorithm, or an operator's abort algorithm. */
        s->alg = (uint8_t)JS_VALUE_GET_INT(JS_StepClosureData(&s->hdr, 0));
        s->st  = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
        s->sub = obs_rec_get(ctx, s->st, "sub");
        s->value = JS_DupValue(ctx, step_arg(&s->hdr, 0));
        obs_goto(s, S_OP_ENTER);
        return 0;

    case OP_TAKE_UNTIL:
        /* §2.3.2 takeUntil steps 1-2. Step 2 CONVERTS, and its throw is the method's — takeUntil states no
           catch, so a value that is not convertible is a TypeError out of takeUntil() itself. */
        if (!obs_is_observable(s->hdr.this_val)) {
            JS_ThrowTypeError(ctx, "takeUntil called on something that is not an Observable");
            *pr = JS_STEP_ABRUPT;
            return 1;
        }
        s->src = JS_DupValue(ctx, s->hdr.this_val);
        obs_convert_enter(ctx, s, step_arg(&s->hdr, 0), S_OP_AFTER);
        return 0;

    case OP_WHEN:
        /* §3 when(type, options) step 1: "If this's relevant global object is a Window whose associated
           Document is not fully active, then return" — and it returns UNDEFINED, not an Observable, which is
           the one place in this standard where a member's declared return type is not what a detached document
           gets. The receiver is any EventTarget, so there is no brand test beyond the object check the
           dispatch itself will make. */
        if (!JS_IsObject(s->hdr.this_val)) {
            JS_ThrowTypeError(ctx, "when called on something that is not an EventTarget");
            *pr = JS_STEP_ABRUPT;
            return 1;
        }
        if (!document_fully_active(ctx)) {
            obs_goto(s, S_DONE);
            return 0;
        }
        s->src = JS_DupValue(ctx, s->hdr.this_val);
        obs_goto(s, S_OP_ENTER);
        return 0;

    case OP_TAKE: case OP_DROP: case OP_INSPECT:
    case OP_TOARRAY: case OP_FOREACH: case OP_EVERY: case OP_FIRST:
    case OP_LAST: case OP_FIND: case OP_SOME: case OP_REDUCE:
        if (!obs_is_observable(s->hdr.this_val)) {
            JS_ThrowTypeError(ctx, "an Observable operator was called on something that is not an Observable");
            *pr = JS_STEP_ABRUPT;
            return 1;
        }
        s->src = JS_DupValue(ctx, s->hdr.this_val);
        {
            const char *cbname = ops_callback_name(op);
            if (cbname != NULL && !JS_IsFunction(ctx, step_arg(&s->hdr, 0))) {
                /* Web IDL: a required callback-function argument that is absent or not callable is a
                   TypeError before step 1, so no promise is created and nothing is subscribed. */
                JS_ThrowTypeError(ctx, "%s: the %s must be a function", "an Observable operator", cbname);
                *pr = JS_STEP_ABRUPT;
                return 1;
            }
        }
        obs_goto(s, S_OP_ENTER);
        return 0;

    default:
        DFAIL("observable_ops was entered with an operation it does not own");
        *pr = JS_STEP_ABRUPT;
        return 1;
    }
}

/* ---- §2.3.2's subscribe callbacks --------------------------------------------------------------------------- */

/* Returns 1 when the machine must return *pr. */
static int ops_subscribe_callback(JSContext *ctx, JSObsState *s, int *pr)
{
    JSValue io, sig;

    (void)pr;
    switch (s->kind) {
    case K_TAKEUNTIL: {
        /* §2.3.2 takeUntil's subscribe callback, steps 1-4: the notifier is subscribed to FIRST, with the
           subscriber's own controller's signal, so a notifier that emits synchronously closes this
           subscription before the source is ever reached. */
        JSValue n = obs_algo_new(ctx, OA_TU_STOP, s->st);
        JSValue arg = obs_rec_get(ctx, s->st, "arg");
        /* NO COMPLETE STEPS. The standard is explicit: a notifier that completes without emitting must NOT
           complete this subscription, so the third algorithm is absent rather than a pass-through. And the
           ERROR steps are PRESENT, which is what keeps §2.2.1's default error algorithm — a report — from
           running instead. */
        io = obs_internal_observer_new(ctx, n, n, JS_UNDEFINED);
        sig = rec_sub_signal(ctx, s->st);
        obs_subscribe_enter(ctx, s, arg, io, sig, S_OP_TAIL);
        JS_FreeValue(ctx, n);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, sig);
        return 0;
    }

    case K_MAP: case K_FILTER: case K_DROP: {
        int alg = s->kind == K_MAP ? OA_MAP_NEXT : s->kind == K_FILTER ? OA_FILTER_NEXT : OA_DROP_NEXT;
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, 0));
        if (s->kind == K_DROP)
            obs_rec_set(ctx, s->st, "remaining", obs_rec_get(ctx, s->st, "arg"));
        io = observer_of(ctx, s->st, alg, -1, -1);
        sig = rec_sub_signal(ctx, s->st);
        obs_subscribe_enter(ctx, s, s->src, io, sig, S_DONE);
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, sig);
        return 0;
    }

    case K_TAKE: {
        /* Step 2: "If remaining is 0, then run subscriber's complete() method and abort these steps" — the
           source is never subscribed to at all. */
        JSValue amount = obs_rec_get(ctx, s->st, "arg");
        int64_t n = 0;
        JS_ToInt64(ctx, &n, amount);
        JS_FreeValue(ctx, amount);
        obs_rec_set(ctx, s->st, "remaining", JS_NewInt64(ctx, n));
        if (n == 0) {
            obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE);
            return 0;
        }
        io = observer_of(ctx, s->st, OA_TAKE_NEXT, -1, -1);
        sig = rec_sub_signal(ctx, s->st);
        obs_subscribe_enter(ctx, s, s->src, io, sig, S_DONE);
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, sig);
        return 0;
    }

    case K_FLATMAP:
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, 0));
        obs_rec_set(ctx, s->st, "outerDone", JS_FALSE);
        obs_rec_set(ctx, s->st, "active", JS_FALSE);
        obs_rec_set(ctx, s->st, "queue", JS_NewArray(ctx));
        io = observer_of(ctx, s->st, OA_FM_NEXT, -1, OA_FM_COMPLETE);
        sig = rec_sub_signal(ctx, s->st);
        obs_subscribe_enter(ctx, s, s->src, io, sig, S_DONE);
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, sig);
        return 0;

    case K_SWITCHMAP:
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, 0));
        obs_rec_set(ctx, s->st, "outerDone", JS_FALSE);
        obs_rec_set(ctx, s->st, "innerSig", JS_NULL);
        io = observer_of(ctx, s->st, OA_SM_NEXT, -1, OA_SM_COMPLETE);
        sig = rec_sub_signal(ctx, s->st);
        obs_subscribe_enter(ctx, s, s->src, io, sig, S_DONE);
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, sig);
        return 0;

    case K_CATCH:
        io = observer_of(ctx, s->st, -1, OA_CATCH_ERROR, -1);
        sig = rec_sub_signal(ctx, s->st);
        obs_subscribe_enter(ctx, s, s->src, io, sig, S_DONE);
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, sig);
        return 0;

    case K_FINALLY: {
        /* Step 1: "Run subscriber's addTeardown() method with callback" — the MEMBER, so a teardown added to
           an already-closed subscription takes §2.1's step-3 branch and fires immediately, exactly as it
           would for a page that called addTeardown itself. */
        JSValue cb = obs_rec_get(ctx, s->st, "arg");
        obs_emit_enter(ctx, s, EM_TEARDOWN, cb, S_OP_TAIL);
        return 0;
    }

    case K_INSPECT: {
        /* Step 1 of the subscribe callback: "If subscribe callback is not null, then invoke it with «» and
           rethrow" — and its throw means the source is NEVER subscribed to. */
        JSValue cfg = obs_rec_get(ctx, s->st, "arg");
        JSValue on_sub = JS_GetPropertyStr(ctx, cfg, "subscribe");
        JS_FreeValue(ctx, cfg);
        if (JS_IsFunction(ctx, on_sub)) {
            JS_FreeValue(ctx, s->op1);
            s->op1 = on_sub;
            obs_goto(s, S_OP_CALLBACK);
            return 0;
        }
        JS_FreeValue(ctx, on_sub);
        obs_goto(s, S_OP_AFTER);
        return 0;
    }

    case K_WHEN: {
        /* §3's subscribe callback. It subscribes to NOTHING — it adds an EVENT LISTENER whose callback runs
           the observable event listener invoke algorithm, and whose SIGNAL is the subscription controller's.
           That signal is the entire unsubscription mechanism: §2.1's close-a-subscription signals it, §2.7's
           step-6 abort steps remove the listener, and nothing here has to remember it was registered. */
        JSValue cfg = obs_rec_get(ctx, s->st, "arg");
        JSValue type = JS_GetPropertyStr(ctx, cfg, "type");
        const char *tp;
        /* THE TWO FLAGS WERE DECIDED AT THE MEMBER, where §3.2.17 converted them — this reads the ANSWERS.
           They were `JS_ToBool` calls here, which is the coercion this component must not perform: it runs in
           a LATER invocation than the one that held the page's value, so a `capture` that arrived unknown was
           pinned to true here with nothing left to fork against.
           BOTH PRESENCES ARE ASSERTED AND NEITHER IS DEFAULTED. `obs_rec_bool` answers false for a name that
           is not there and `obs_rec_int` answers 0, so a record built without them would register a bubbling
           NON-passive listener and read exactly like one the page asked for. */
        JSValue capv = obs_rec_get(ctx, cfg, "capture"), pasv = obs_rec_get(ctx, cfg, "passive");
        bool capture;
        int passive;

        DCHECK(JS_IsBool(capv) && JS_IsNumber(pasv),
               "§3's subscribe callback was handed a record without the two flags its OP_WHEN entry decides — "
               "that entry writes `capture` as a boolean and `passive` as the three-state -1/0/1 off the "
               "CONVERTED ObservableEventListenerOptions, so a record missing either was built elsewhere and "
               "would register a bubbling non-passive listener indistinguishable from one a page asked for");
        capture = obs_rec_bool(ctx, cfg, "capture");
        /* §3's tristate: -1 is "this member does not exist", which "add an event listener" step 4 fills from
           DOM §2.7's default passive value rather than treating as false. */
        passive = (int)obs_rec_int(ctx, cfg, "passive");
        JS_FreeValue(ctx, capv);
        JS_FreeValue(ctx, pasv);
        DCHECK(passive >= -1 && passive <= 1,
               "§3's `passive` reached the subscribe callback as something other than the three states the "
               "member's own conversion writes — the OP_WHEN entry decides it from the converted dictionary "
               "and puts -1, 0 or 1 on this record, so anything else is a second writer");
        sig = rec_sub_signal(ctx, s->st);
        /* Step 2: "If subscriber's subscription controller's signal is aborted, abort these steps." */
        if (abort_signal_aborted(ctx, sig)) {
            JS_FreeValue(ctx, cfg); JS_FreeValue(ctx, type); JS_FreeValue(ctx, sig);
            obs_goto(s, S_DONE);
            return 0;
        }
        io = obs_algo_new(ctx, OA_WHEN_INVOKE, s->st);
        tp = JS_ToCString(ctx, type);
        CHECK(tp != NULL, "§3 when(): the event type could not be read back as a string");
        event_target_add_listener(ctx, s->src, tp, io, capture, /*once*/ false, passive, sig);
        JS_FreeCString(ctx, tp);
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, cfg);
        JS_FreeValue(ctx, type);
        JS_FreeValue(ctx, sig);
        obs_goto(s, S_DONE);
        return 0;
    }

    default:
        DFAIL("an operator subscribe callback ran for a kind §2.3 does not define");
        *pr = JS_STEP_ABRUPT;
        return 1;
    }
}

/* §2.3.2 inspect: steps 2-4 of its subscribe callback, after the `subscribe` handler (if any) returned. */
static void ops_inspect_attach(JSContext *ctx, JSObsState *s)
{
    JSValue cfg = obs_rec_get(ctx, s->st, "arg");
    JSValue on_abort = JS_GetPropertyStr(ctx, cfg, "abort");
    JSValue io, sig = rec_sub_signal(ctx, s->st);

    if (JS_IsFunction(ctx, on_abort)) {
        /* Step 2: the abort algorithm. It is HELD on the record because three of the observer's algorithms
           REMOVE it — the standard's note says why: the handler is for consumer-initiated unsubscriptions
           only, so a producer that errors or completes must unregister it before closing. */
        JSValue algo = obs_algo_new(ctx, OA_INSP_ABORT, s->st);
        obs_rec_set(ctx, s->st, "abortAlgo", JS_DupValue(ctx, algo));
        abort_signal_add_algorithm(ctx, sig, algo);
        JS_FreeValue(ctx, algo);
    }
    JS_FreeValue(ctx, on_abort);
    JS_FreeValue(ctx, cfg);
    io = observer_of(ctx, s->st, OA_INSP_NEXT, OA_INSP_ERROR, OA_INSP_COMPLETE);
    obs_subscribe_enter(ctx, s, s->src, io, sig, S_DONE);
    JS_FreeValue(ctx, io);
    JS_FreeValue(ctx, sig);
}

/* §2.3.2 inspect: "Remove abort callback from subscriber's subscription controller's signal." */
static void ops_inspect_drop_abort(JSContext *ctx, JSObsState *s)
{
    JSValue algo = obs_rec_get(ctx, s->st, "abortAlgo");

    if (JS_IsFunction(ctx, algo)) {
        JSValue sig = rec_sub_signal(ctx, s->st);
        abort_signal_remove_algorithm(ctx, sig, algo);
        JS_FreeValue(ctx, sig);
        obs_rec_set(ctx, s->st, "abortAlgo", JS_UNDEFINED);
    }
    JS_FreeValue(ctx, algo);
}

/* ---- §2.3.3's method prologue ------------------------------------------------------------------------------- */

/* Everything after the `options.signal` read, up to the subscription. Returns 1 when *pr must be returned. */
static int ops_promise_prologue(JSContext *ctx, JSObsState *s, int op, JSValueConst opt_signal)
{
    JSValue rf[2], p, isig, io;
    int has_ctl = ops_wants_controller(op);

    p = JS_NewPromiseCapability(ctx, rf);
    CHECK(!JS_IsException(p), "§2.3.3: the operator's promise could not be created");
    s->st = obs_record_new(ctx);
    obs_rec_set(ctx, s->st, "resolve", rf[0]);
    obs_rec_set(ctx, s->st, "reject", rf[1]);
    if (ops_callback_name(op) != NULL)
        obs_rec_set(ctx, s->st, "arg", JS_DupValue(ctx, step_arg(&s->hdr, 0)));
    obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, 0));
    s->result = p;

    if (has_ctl) {
        /* "Let controller be a new AbortController" — this component holds it as the SIGNAL that controller
           owns, because §3.2's controller is reachable only through that signal and nothing here needs it for
           anything but signalling. */
        JSValue csig = abort_signal_new(ctx);
        JSValueConst pair[2];
        int n = 0;
        obs_rec_set(ctx, s->st, "csig", JS_DupValue(ctx, csig));
        pair[n++] = csig;
        if (abort_signal_is(ctx, opt_signal))
            pair[n++] = opt_signal;
        isig = abort_signal_dependent_new(ctx, pair, n);
        JS_FreeValue(ctx, csig);
    } else {
        /* toArray and last use the caller's signal DIRECTLY — they never end the subscription themselves, so
           there is no second source for a dependent signal to have. */
        isig = abort_signal_is(ctx, opt_signal) ? JS_DupValue(ctx, opt_signal) : JS_UNDEFINED;
    }
    obs_rec_set(ctx, s->st, "isig", JS_DupValue(ctx, isig));

    if (abort_signal_is(ctx, isig) && abort_signal_aborted(ctx, isig)) {
        /* "Reject p with the signal's abort reason. Return p." — and the source is never subscribed to. */
        JSValue reason = abort_signal_reason(ctx, isig);
        JS_FreeValue(ctx, isig);
        ops_settle_enter(ctx, s, s->st, /*reject*/ 1, reason, S_DONE);
        return 0;
    }
    if (abort_signal_is(ctx, isig)) {
        JSValue algo = obs_algo_new(ctx, OA_P_REJECT, s->st);
        abort_signal_add_algorithm(ctx, isig, algo);
        JS_FreeValue(ctx, algo);
    }

    switch (op) {
    case OP_TOARRAY:
        obs_rec_set(ctx, s->st, "values", JS_NewArray(ctx));
        io = observer_of(ctx, s->st, OA_TOARRAY_NEXT, OA_P_ERROR, OA_TOARRAY_DONE);
        break;
    case OP_FOREACH: io = observer_of(ctx, s->st, OA_FOREACH_NEXT, OA_P_ERROR, OA_FIND_DONE); break;
    case OP_EVERY:   io = observer_of(ctx, s->st, OA_EVERY_NEXT,   OA_P_ERROR, OA_EVERY_DONE); break;
    case OP_FIRST:   io = observer_of(ctx, s->st, OA_FIRST_NEXT,   OA_P_ERROR, OA_FIRST_DONE); break;
    case OP_LAST:
        obs_rec_set(ctx, s->st, "hasLast", JS_FALSE);
        io = observer_of(ctx, s->st, OA_LAST_NEXT, OA_P_ERROR, OA_LAST_DONE);
        break;
    case OP_FIND:    io = observer_of(ctx, s->st, OA_FIND_NEXT,    OA_P_ERROR, OA_FIND_DONE); break;
    case OP_SOME:    io = observer_of(ctx, s->st, OA_SOME_NEXT,    OA_P_ERROR, OA_SOME_DONE); break;
    default:
        DCHECK(op == OP_REDUCE, "§2.3.3's prologue ran for an operator it does not have");
        /* "Let accumulator be initialValue if it is given, and uninitialized otherwise" — GIVEN, not
           non-undefined: `reduce(f, undefined)` HAS an initial value, and the difference is whether the first
           emitted value is fed to the reducer or becomes the accumulator. */
        obs_rec_set(ctx, s->st, "hasAcc", JS_NewBool(ctx, s->hdr.argc > 1));
        if (s->hdr.argc > 1)
            obs_rec_set(ctx, s->st, "acc", JS_DupValue(ctx, step_arg(&s->hdr, 1)));
        io = observer_of(ctx, s->st, OA_REDUCE_NEXT, OA_P_ERROR, OA_REDUCE_DONE);
        break;
    }
    obs_subscribe_enter(ctx, s, s->src, io, isig, S_DONE);
    JS_FreeValue(ctx, io);
    JS_FreeValue(ctx, isig);
    return 0;
}

/* ---- the stages ---------------------------------------------------------------------------------------------
 *
 * Each returns 1 with *pr set when the machine must return, and 0 when the stage moved and the core loop
 * should keep going. */

static int ops_stage_enter(JSContext *ctx, JSObsState *s, JSValue *pcb, JSValue **out_cb, int *out_argc,
                           int *pr);
static int ops_stage_callback(JSContext *ctx, JSObsState *s, JSValue *pcb, JSValue **out_cb, int *out_argc,
                              int *pr);
static int ops_stage_after(JSContext *ctx, JSObsState *s, int *pr);
static int ops_stage_tail(JSContext *ctx, JSObsState *s, int *pr);

int obs_ops_stage(JSContext *ctx, JSObsState *s, JSValue *pcb, JSValue **out_cb, int *out_argc, int *pr)
{
    int r;

    switch (s->hdr.stage) {
    case S_OP_ENTER:    return ops_stage_enter(ctx, s, pcb, out_cb, out_argc, pr);
    case S_OP_CALLBACK: return ops_stage_callback(ctx, s, pcb, out_cb, out_argc, pr);
    case S_OP_AFTER:    return ops_stage_after(ctx, s, pr);
    case S_OP_TAIL:     return ops_stage_tail(ctx, s, pr);

    case S_OP_SETTLE: {
        /* §2.3.3: "resolve p with v" / "reject p with e", as a call of the capability's resolving function. */
        JSValueConst arg = s->op2;
        JSValue out;
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->op1, JS_UNDEFINED, 1, &arg, *pcb, &out,
                          out_cb, out_argc);
        if (r > 0) { *pr = r; return 1; }
        *pcb = JS_UNDEFINED;
        /* A capability's resolving functions never throw: 27.5.1.3's resolveSteps's own abrupt case is turned into a
           REJECTION of the same promise before it returns. */
        DCHECK(!JS_IsException(out), "a promise capability's resolving function raised");
        if (JS_IsException(out)) JS_FreeValue(ctx, JS_GetException(ctx));
        else JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, s->op1); s->op1 = JS_UNDEFINED;
        JS_FreeValue(ctx, s->op2); s->op2 = JS_UNDEFINED;
        obs_goto(s, s->snext);
        return 0;
    }

    case S_OP_ABORT:
        /* §2.3.3's "signal abort controller [with E]" — DOM §3.2's whole operation, which is what tears the
           subscription down: the subscriber's step-9.2 abort algorithm is registered on the very signal this
           one is a source of. */
        r = abort_signal_run(ctx, &s->aw, s->op3, s->has_reason ? (JSValueConst)s->creason : JS_UNDEFINED,
                             *pcb, out_cb, out_argc);
        if (r > 0) { *pr = r; return 1; }
        *pcb = JS_UNDEFINED;
        if (r < 0) { *pr = JS_STEP_ABRUPT; return 1; }
        JS_FreeValue(ctx, s->creason); s->creason = JS_UNDEFINED;
        s->has_reason = 0;
        JS_FreeValue(ctx, s->op3); s->op3 = JS_UNDEFINED;
        obs_goto(s, s->snext);
        return 0;

    default:
        DFAIL("an Observable machine resumed in a stage neither half of the component claims");
        *pr = JS_STEP_ABRUPT;
        return 1;
    }
}

/* S_OP_ENTER — the first step of a subscribe callback, of an internal-observer algorithm, or of one of the
   methods whose Web IDL prologue reads the page's objects. */
static int ops_stage_enter(JSContext *ctx, JSObsState *s, JSValue *pcb, JSValue **out_cb, int *out_argc,
                           int *pr)
{
    int op = s->hdr.arg, r;

    if (op == OP_OP_SUBSCRIBE)
        return ops_subscribe_callback(ctx, s, pr);

    if (op == OP_TAKE || op == OP_DROP) {
        /* Web IDL `unsigned long long amount` — ToNumber on whatever the page passed, so its `valueOf` runs
           here. A NEGATIVE result is Web IDL's 2^64 + v, an amount no subscription can reach; carried as the
           signed value it counts down without ever meeting zero, which is the same observable behaviour and
           the only one an int64 can represent. */
        int64_t n = 0;
        r = step_toint64_run(ctx, &s->hdr, step_arg(&s->hdr, 0), *pcb, &n, out_cb, out_argc);
        if (r > 0) { *pr = r; return 1; }
        if (r < 0) { *pr = JS_STEP_ABRUPT; return 1; }
        *pcb = JS_UNDEFINED;
        {
            JSValue amount = JS_NewInt64(ctx, n);
            s->result = obs_operator_observable(ctx, op == OP_TAKE ? K_TAKE : K_DROP, s->src, amount);
            JS_FreeValue(ctx, amount);
        }
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; *pr = JS_STEP_ABRUPT; return 1; }
        obs_goto(s, S_DONE);
        return 0;
    }

    if (op == OP_WHEN) {
        /* `when(DOMString type, optional ObservableEventListenerOptions options = {})`.
           THE ARGUMENTS CONVERT LEFT TO RIGHT — Web IDL §3.6 Overload resolution algorithm: "the JavaScript
           values are converted from left to right" — so `type`'s ToString runs BEFORE any member of the
           dictionary is read. This block ran the dictionary FIRST, under a comment claiming that WAS the
           argument order, and a page sees the difference: `et.when({toString(){log("t")}},
           {get capture(){log("c")}})` logs t then c in a browser and logged c, then passive, then t here. */
        if (JS_IsUndefined(s->st))
            s->st = obs_record_new(ctx);
        /* ARGUMENT 0, `DOMString type` — ToString on whatever the page passed, so its `toString` is the page's
           code and this is a request like every other conversion here. The converted string ON THE RECORD is
           the latch: a resume that already has one does not re-run the page's toString. */
        {
            JSValue tv = obs_rec_get(ctx, s->st, "type");
            bool have = !JS_IsUndefined(tv);
            JS_FreeValue(ctx, tv);
            if (!have) {
                JSValue str;
                r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), *pcb, &str, out_cb, out_argc);
                if (r > 0) { *pr = r; return 1; }
                if (r < 0) { *pr = JS_STEP_ABRUPT; return 1; }
                *pcb = JS_UNDEFINED;
                obs_rec_set(ctx, s->st, "type", str);
            }
        }
        /* ARGUMENT 1, the dictionary — Web IDL §3.2.17 Dictionary types, run by the ONE walk
           core/idl_args.h owns. */
        {
            JSValueConst options = step_arg(&s->hdr, 1);
            JSValue in = *pcb, o, pv;

            *pcb = JS_UNDEFINED;
            if (!s->dw.started) {
                /* Web IDL §3.2.17 Dictionary types (ES-to-IDL list) step 1: "If jsDict is not an Object and
                   jsDict is neither undefined nor null, then throw a TypeError". It stays at the CALLER
                   because idl_dict_walk_start asserts that step rather than performing it — undefined and
                   null are legal there, and step 4.1.2 gives every member undefined. */
                if (!JS_IsObject(options) && !JS_IsUndefined(options) && !JS_IsNull(options)) {
                    JS_FreeValue(ctx, in);
                    JS_ThrowTypeError(ctx, "the ObservableEventListenerOptions argument is neither an object, "
                                           "undefined nor null");
                    *pr = JS_STEP_ABRUPT;
                    return 1;
                }
                /* NO INTERFACE AND NO FRAMES: both members are booleans, so nothing here brands and nothing
                   nests — and idl_dict_walk_start asserts the frame count against idl_members_depth over this
                   very list rather than taking that on trust. */
                if (idl_dict_walk_start(ctx, &s->dw, options, WHEN_OPTIONS, WHEN_OPTIONS_DECL.n,
                                        g_when_options_atoms, WHEN_OPTIONS_DECL.name, /*iface*/ 0,
                                        /*narrow*/ NULL, /*frames*/ NULL, /*frames_cap*/ 0) < 0) {
                    JS_FreeValue(ctx, in);
                    *pr = JS_STEP_ABRUPT;
                    return 1;
                }
            }
            r = idl_dict_walk_run(ctx, &s->hdr, &s->dw, /*frames*/ NULL, /*frames_cap*/ 0, in,
                                  out_cb, out_argc);
            if (r > 0) { *pr = r; return 1; }
            if (r < 0) { *pr = JS_STEP_ABRUPT; return 1; }
            o = idl_dict_walk_take(ctx, &s->dw);
            /* THE TWO FLAGS ARE DECIDED HERE, WHERE THE DICTIONARY IS, and the record carries the ANSWERS
               rather than the raw members — so the subscribe callback, which runs in another invocation and
               may resume from the cold tier, does no coercion of its own. A JS_ToBool down there is the same
               pin as one up here, one step further from the value that would have to fork. */
            obs_rec_set(ctx, s->st, "capture", JS_NewBool(ctx, idl_dict_bool(ctx, o, "capture")));
            /* §3's listener construction gives its `passive` as "options's passive if this member
               exists; null otherwise". `passive` declares no default, so §3.2.17 leaves an absent one absent
               and the THIRD state is real: -1 is that null,
               which "add an event listener" step 4 then fills from DOM §2.7's default passive value (TRUE for
               a wheel listener on a Window). Collapsing it to false would make `{}` and `{passive:false}` one
               registration, which is the pair that differs most. */
            pv = idl_dict_get(ctx, o, "passive");
            obs_rec_set(ctx, s->st, "passive",
                        JS_NewInt32(ctx, JS_IsUndefined(pv) ? -1
                                                            : (idl_dict_bool(ctx, o, "passive") ? 1 : 0)));
            JS_FreeValue(ctx, pv);
            JS_FreeValue(ctx, o);
        }
        s->result = obs_operator_observable(ctx, K_WHEN, s->src, s->st);
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; *pr = JS_STEP_ABRUPT; return 1; }
        obs_goto(s, S_DONE);
        return 0;
    }

    if (op == OP_INSPECT) {
        /* `optional ObservableInspectorUnion inspectorUnion = {}`: a callable IS the next handler; an object
           is an ObservableInspector whose five members Web IDL §3.2.17 reads in LEXICOGRAPHIC order. */
        static const char *const MEMBERS[5] = { "abort", "complete", "error", "next", "subscribe" };
        JSValueConst u = step_arg(&s->hdr, 0);

        if (JS_IsUndefined(s->st)) {
            s->st = obs_record_new(ctx);
            s->member = 0;
        }
        if (JS_IsFunction(ctx, u)) {
            obs_rec_set(ctx, s->st, "next", JS_DupValue(ctx, u));
        } else if (JS_IsObject(u)) {
            while (s->member < 5) {
                JSAtom a = JS_NewAtom(ctx, MEMBERS[s->member]);
                JSValue v;
                r = step_getprop_run(ctx, &s->hdr, u, a, *pcb, &v, out_cb, out_argc);
                JS_FreeAtom(ctx, a);
                if (r > 0) { *pr = r; return 1; }
                if (r < 0) { *pr = JS_STEP_ABRUPT; return 1; }
                *pcb = JS_UNDEFINED;
                if (!JS_IsUndefined(v) && !JS_IsFunction(ctx, v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "inspect: an ObservableInspector member must be a function");
                    *pr = JS_STEP_ABRUPT;
                    return 1;
                }
                obs_rec_set(ctx, s->st, MEMBERS[s->member], v);
                s->member++;
            }
        } else if (!JS_IsUndefined(u) && !JS_IsNull(u)) {
            JS_ThrowTypeError(ctx, "inspect: the inspector must be a function or an object");
            *pr = JS_STEP_ABRUPT;
            return 1;
        }
        s->member = 0;
        s->result = obs_operator_observable(ctx, K_INSPECT, s->src, s->st);
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; *pr = JS_STEP_ABRUPT; return 1; }
        obs_goto(s, S_DONE);
        return 0;
    }

    if (op == OP_TOARRAY || op == OP_FOREACH || op == OP_EVERY || op == OP_FIRST ||
        op == OP_LAST || op == OP_FIND || op == OP_SOME || op == OP_REDUCE) {
        /* `optional SubscribeOptions options = {}` — Web IDL §3.2.17 Dictionary types, run by the ONE walk
           observable.c declares. Web IDL runs this BEFORE the method steps, so a throwing getter throws out of
           the operator rather than producing a rejected promise.
           THIS BLOCK USED TO BE THE SECOND COPY of that section — its own `JS_NewAtom("signal")`, its own
           [[Get]], its own hand-written brand test and its own TypeError text, standing beside the identical
           four lines in observable.c's S_OPTIONS_READ. Eight of this dictionary's nine call sites are here, so
           the copy that drifted would have been the one seven operators went through. */
        r = obs_subscribe_options_run(ctx, s, step_arg(&s->hdr, ops_options_index(op)), pcb, out_cb, out_argc);
        if (r > 0) { *pr = r; return 1; }
        if (r < 0) { *pr = JS_STEP_ABRUPT; return 1; }
        /* §2.3.3's "options's signal" — BORROWED by the prologue, which dups whatever it keeps. An absent
           member is undefined here, which is the same "not given" the prologue's own arms already read. */
        return ops_promise_prologue(ctx, s, op, s->has_sig ? (JSValueConst)s->sig : JS_UNDEFINED);
    }

    DCHECK(op == OP_OP_ALGO, "S_OP_ENTER was reached by an operation that has no first step there");

    /* ---- the internal-observer algorithms, at their first step ---- */
    switch (s->alg) {
    case OA_PASS_NEXT:
        obs_emit_enter(ctx, s, EM_NEXT, JS_DupValue(ctx, s->value), S_DONE);
        return 0;
    case OA_PASS_ERROR:
        obs_emit_enter(ctx, s, EM_ERROR, JS_DupValue(ctx, s->value), S_DONE);
        return 0;
    case OA_PASS_COMPLETE:
    case OA_TU_STOP:
        obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE);
        return 0;

    case OA_WHEN_INVOKE:
        /* §3 "the observable event listener invoke algorithm": run subscriber's next() with the event. */
        obs_emit_enter(ctx, s, EM_NEXT, JS_DupValue(ctx, s->value), S_DONE);
        return 0;

    case OA_MAP_NEXT: case OA_FILTER_NEXT: case OA_FOREACH_NEXT:
    case OA_EVERY_NEXT: case OA_FIND_NEXT: case OA_SOME_NEXT:
    case OA_FM_NEXT: case OA_SM_NEXT: case OA_REDUCE_NEXT:
        break;   /* handled below — each needs its own prologue before the callback */

    case OA_TAKE_NEXT:
        /* "Run subscriber's next() with the value. Decrement remaining. If remaining is 0, complete()." */
        obs_emit_enter(ctx, s, EM_NEXT, JS_DupValue(ctx, s->value), S_OP_TAIL);
        return 0;

    case OA_DROP_NEXT: {
        /* "If remaining is > 0, then decrement remaining and abort these steps. Assert: remaining is 0."
           The spec's `remaining` is an UNSIGNED LONG LONG, so "> 0" means "not 0" — and this engine carries
           the amount as a SIGNED int64 because Web IDL's `drop(-1)` is 2^64-1, a count no subscription can
           reach and no double can hold. Carried signed it counts DOWN from -1 without ever meeting zero,
           which is the same observable behaviour; testing `> 0` instead of `!= 0` would emit on the very
           first value and turn "drop everything" into "drop nothing". */
        int64_t rem = obs_rec_int(ctx, s->st, "remaining");
        if (rem != 0) {
            obs_rec_set(ctx, s->st, "remaining", JS_NewInt64(ctx, rem - 1));
            obs_goto(s, S_DONE);
            return 0;
        }
        obs_emit_enter(ctx, s, EM_NEXT, JS_DupValue(ctx, s->value), S_DONE);
        return 0;
    }

    case OA_FM_COMPLETE: {
        JSValue q = obs_rec_get(ctx, s->st, "queue");
        bool idle = !obs_rec_bool(ctx, s->st, "active") && arr_len(ctx, q) == 0;
        JS_FreeValue(ctx, q);
        obs_rec_set(ctx, s->st, "outerDone", JS_TRUE);
        if (idle) { obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE); return 0; }
        obs_goto(s, S_DONE);
        return 0;
    }

    case OA_FM_INNER_DONE: {
        /* The inner Observable finished: the next queued value is processed, and only an EMPTY queue releases
           `activeInnerSubscription` — which is what lets the outer complete() finally complete this one. */
        JSValue q = obs_rec_get(ctx, s->st, "queue");
        if (arr_len(ctx, q) != 0) {
            JS_FreeValue(ctx, s->value);
            s->value = arr_shift(ctx, q);
            JS_FreeValue(ctx, q);
            break;                       /* fall into the process-next-value chain below */
        }
        JS_FreeValue(ctx, q);
        obs_rec_set(ctx, s->st, "active", JS_FALSE);
        if (obs_rec_bool(ctx, s->st, "outerDone")) {
            obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE);
            return 0;
        }
        obs_goto(s, S_DONE);
        return 0;
    }

    case OA_SM_COMPLETE: {
        JSValue inner = obs_rec_get(ctx, s->st, "innerSig");
        bool idle = !abort_signal_is(ctx, inner);
        JS_FreeValue(ctx, inner);
        obs_rec_set(ctx, s->st, "outerDone", JS_TRUE);
        if (idle) { obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE); return 0; }
        obs_goto(s, S_DONE);
        return 0;
    }

    case OA_SM_INNER_DONE:
        if (obs_rec_bool(ctx, s->st, "outerDone")) {
            obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE);
            return 0;
        }
        obs_rec_set(ctx, s->st, "innerSig", JS_NULL);
        obs_goto(s, S_DONE);
        return 0;

    case OA_INSP_NEXT: case OA_INSP_ERROR: case OA_INSP_COMPLETE: {
        static const char *const H[3] = { "next", "error", "complete" };
        int which = s->alg == OA_INSP_NEXT ? 0 : s->alg == OA_INSP_ERROR ? 1 : 2;
        JSValue cfg = obs_rec_get(ctx, s->st, "arg");
        JSValue h = JS_GetPropertyStr(ctx, cfg, H[which]);
        JS_FreeValue(ctx, cfg);
        /* The error and complete algorithms drop the abort handler FIRST: the standard's note is that it must
           fire only for consumer-initiated unsubscriptions, and a producer that is ending the subscription
           here is about to signal the very controller it is registered on. */
        if (which != 0)
            ops_inspect_drop_abort(ctx, s);
        if (JS_IsFunction(ctx, h)) {
            JS_FreeValue(ctx, s->op1);
            s->op1 = h;
            obs_goto(s, S_OP_CALLBACK);
            return 0;
        }
        JS_FreeValue(ctx, h);
        obs_goto(s, S_OP_AFTER);
        return 0;
    }

    case OA_INSP_ABORT: {
        /* §2.3.2 inspect step 2's abort algorithm: "Invoke abort callback with «the signal's abort reason» and
           REPORT" — so its throw is reported at the global, never propagated into §3.2's algorithm walk. */
        JSValue cfg = obs_rec_get(ctx, s->st, "arg");
        JSValue h = JS_GetPropertyStr(ctx, cfg, "abort");
        JSValue sig = rec_sub_signal(ctx, s->st);
        JS_FreeValue(ctx, cfg);
        JS_FreeValue(ctx, s->value);
        s->value = abort_signal_reason(ctx, sig);
        JS_FreeValue(ctx, sig);
        DCHECK(JS_IsFunction(ctx, h),
               "inspect registered an abort algorithm with no abort handler behind it");
        JS_FreeValue(ctx, s->op1);
        s->op1 = h;
        obs_goto(s, S_OP_CALLBACK);
        return 0;
    }

    case OA_CATCH_ERROR: {
        JSValue h = obs_rec_get(ctx, s->st, "arg");
        DCHECK(JS_IsFunction(ctx, h), "catch's CatchCallback is not callable");
        JS_FreeValue(ctx, s->op1);
        s->op1 = h;
        obs_goto(s, S_OP_CALLBACK);
        return 0;
    }

    case OA_P_REJECT: {
        /* §2.3.3's abort algorithm: "Reject p with internal options's signal's abort reason." */
        JSValue isig = obs_rec_get(ctx, s->st, "isig");
        JSValue reason = abort_signal_reason(ctx, isig);
        JS_FreeValue(ctx, isig);
        ops_settle_enter(ctx, s, s->st, /*reject*/ 1, reason, S_DONE);
        return 0;
    }

    case OA_P_ERROR:
        ops_settle_enter(ctx, s, s->st, /*reject*/ 1, JS_DupValue(ctx, s->value), S_DONE);
        return 0;

    case OA_TOARRAY_NEXT: {
        JSValue values = obs_rec_get(ctx, s->st, "values");
        arr_append(ctx, values, JS_DupValue(ctx, s->value));
        JS_FreeValue(ctx, values);
        obs_goto(s, S_DONE);
        return 0;
    }
    case OA_TOARRAY_DONE:
        ops_settle_enter(ctx, s, s->st, 0, obs_rec_get(ctx, s->st, "values"), S_DONE);
        return 0;

    case OA_EVERY_DONE:
        ops_settle_enter(ctx, s, s->st, 0, JS_TRUE, S_DONE);
        return 0;
    case OA_SOME_DONE:
        ops_settle_enter(ctx, s, s->st, 0, JS_FALSE, S_DONE);
        return 0;
    case OA_FIND_DONE:
        /* find() and forEach() both resolve with undefined when the source completes. */
        ops_settle_enter(ctx, s, s->st, 0, JS_UNDEFINED, S_DONE);
        return 0;
    case OA_FIRST_DONE:
        ops_settle_enter(ctx, s, s->st, 1,
                         ops_range_error(ctx, "first(): the Observable completed without emitting a value"),
                         S_DONE);
        return 0;

    case OA_FIRST_NEXT:
        /* "Resolve p with the value. Signal abort controller." — the settle FIRST, so a downstream handler
           sees the promise settled before the subscription's teardowns run. The controller is placed here
           because S_OP_TAIL is shared with the operators that abort WITH a reason. */
        JS_FreeValue(ctx, s->op3);
        s->op3 = obs_rec_get(ctx, s->st, "csig");
        s->has_reason = 0;
        ops_settle_enter(ctx, s, s->st, 0, JS_DupValue(ctx, s->value), S_OP_TAIL);
        return 0;

    case OA_LAST_NEXT:
        obs_rec_set(ctx, s->st, "hasLast", JS_TRUE);
        obs_rec_set(ctx, s->st, "last", JS_DupValue(ctx, s->value));
        obs_goto(s, S_DONE);
        return 0;
    case OA_LAST_DONE:
        if (obs_rec_bool(ctx, s->st, "hasLast"))
            ops_settle_enter(ctx, s, s->st, 0, obs_rec_get(ctx, s->st, "last"), S_DONE);
        else
            ops_settle_enter(ctx, s, s->st, 1,
                             ops_range_error(ctx, "last(): the Observable completed without emitting a value"),
                             S_DONE);
        return 0;

    case OA_REDUCE_DONE:
        if (obs_rec_bool(ctx, s->st, "hasAcc"))
            ops_settle_enter(ctx, s, s->st, 0, obs_rec_get(ctx, s->st, "acc"), S_DONE);
        else
            ops_settle_enter(ctx, s, s->st, 1,
                             ops_type_error(ctx, "reduce(): an empty Observable with no initial value"),
                             S_DONE);
        return 0;

    default:
        DFAIL("an operator internal-observer algorithm ran with an OA_ id §2.3 does not define");
        *pr = JS_STEP_ABRUPT;
        return 1;
    }

    /* ---- the algorithms that INVOKE the operator's callback, at «value, idx» ---- */
    if (s->alg == OA_REDUCE_NEXT && !obs_rec_bool(ctx, s->st, "hasAcc")) {
        /* "If accumulator is uninitialized, set accumulator to the value, set idx to idx + 1, and abort these
           steps" — the reducer is not called with the first value at all. */
        obs_rec_set(ctx, s->st, "hasAcc", JS_TRUE);
        obs_rec_set(ctx, s->st, "acc", JS_DupValue(ctx, s->value));
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx") + 1));
        obs_goto(s, S_DONE);
        return 0;
    }
    if (s->alg == OA_FM_NEXT && obs_rec_bool(ctx, s->st, "active")) {
        /* An inner subscription is still running, so this value waits its turn. */
        JSValue q = obs_rec_get(ctx, s->st, "queue");
        arr_append(ctx, q, JS_DupValue(ctx, s->value));
        JS_FreeValue(ctx, q);
        obs_goto(s, S_DONE);
        return 0;
    }
    if (s->alg == OA_FM_NEXT)
        obs_rec_set(ctx, s->st, "active", JS_TRUE);
    /* THE CALLBACK IS PLACED BEFORE ANYTHING THAT CAN SUSPEND. switchMap's next steps signal the previous
       inner controller first, and that is a full §3.2 abort — algorithms, then the `abort` event, both the
       page's code — so the machine parks between here and S_OP_CALLBACK. An operand fetched after the park
       would be fetched by a stage that never runs. */
    JS_FreeValue(ctx, s->op1);
    s->op1 = obs_rec_get(ctx, s->st, "arg");
    DCHECK(JS_IsFunction(ctx, s->op1), "an operator's callback is not callable at the moment it is invoked");
    if (s->alg == OA_SM_NEXT) {
        /* "If activeInnerAbortController is not null, then signal abort it" — the previous inner subscription
           is dropped before the new one is derived, which is the whole of what switchMap means. */
        JSValue inner = obs_rec_get(ctx, s->st, "innerSig");
        JSValue fresh = abort_signal_new(ctx);
        obs_rec_set(ctx, s->st, "innerSig", JS_DupValue(ctx, fresh));
        JS_FreeValue(ctx, fresh);
        if (abort_signal_is(ctx, inner)) {
            ops_abort_enter(ctx, s, inner, 0, JS_UNDEFINED, S_OP_CALLBACK);
            JS_FreeValue(ctx, inner);
            return 0;
        }
        JS_FreeValue(ctx, inner);
    }
    obs_goto(s, S_OP_CALLBACK);
    return 0;
}

/* S_OP_CALLBACK — invoking the operator's own callback with "rethrow". The argument list is the standard's:
   «value, idx» for a Mapper/Predicate/Visitor, «accumulator, currentValue, index» for a Reducer, «value» for a
   CatchCallback and for inspect's next/error, «» for inspect's subscribe/complete. */
static int ops_stage_callback(JSContext *ctx, JSObsState *s, JSValue *pcb, JSValue **out_cb, int *out_argc,
                              int *pr)
{
    JSValueConst argv[3];
    JSValue idxv = JS_UNDEFINED, accv = JS_UNDEFINED, out;
    int argc = 0, r;

    switch (s->alg) {
    case OA_REDUCE_NEXT:
        accv = obs_rec_get(ctx, s->st, "acc");
        idxv = JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx"));
        argv[0] = accv; argv[1] = s->value; argv[2] = idxv; argc = 3;
        break;
    case OA_MAP_NEXT: case OA_FILTER_NEXT: case OA_FOREACH_NEXT:
    case OA_EVERY_NEXT: case OA_FIND_NEXT: case OA_SOME_NEXT:
    case OA_FM_NEXT: case OA_FM_INNER_DONE: case OA_SM_NEXT:
        idxv = JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx"));
        argv[0] = s->value; argv[1] = idxv; argc = 2;
        break;
    case OA_CATCH_ERROR: case OA_INSP_NEXT: case OA_INSP_ERROR: case OA_INSP_ABORT:
        argv[0] = s->value; argc = 1;
        break;
    default:
        DCHECK(s->alg == OA_INSP_COMPLETE || s->hdr.arg == OP_OP_SUBSCRIBE,
               "S_OP_CALLBACK was reached by an algorithm with no callback to invoke");
        argc = 0;
        break;
    }
    r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->op1, JS_UNDEFINED, argc, argv, *pcb, &out,
                      out_cb, out_argc);
    JS_FreeValue(ctx, idxv);
    JS_FreeValue(ctx, accv);
    if (r > 0) { *pr = r; return 1; }
    *pcb = JS_UNDEFINED;
    JS_FreeValue(ctx, s->op1);
    s->op1 = out;                       /* the returned value, or JS_EXCEPTION with the throw live */
    obs_goto(s, S_OP_AFTER);
    return 0;
}

/* S_OP_AFTER — what the standard does with what the callback returned, or with what it threw. */
static int ops_stage_after(JSContext *ctx, JSObsState *s, int *pr)
{
    int threw = JS_IsException(s->op1);

    if (s->hdr.arg == OP_TAKE_UNTIL) {
        /* takeUntil step 2's convert: no catch, so the TypeError is the METHOD's. */
        if (threw) { s->op1 = JS_UNDEFINED; *pr = JS_STEP_ABRUPT; return 1; }
        s->result = obs_operator_observable(ctx, K_TAKEUNTIL, s->src, s->op1);
        JS_FreeValue(ctx, s->op1);
        s->op1 = JS_UNDEFINED;
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; *pr = JS_STEP_ABRUPT; return 1; }
        obs_goto(s, S_DONE);
        return 0;
    }

    if (s->hdr.arg == OP_OP_SUBSCRIBE) {
        /* inspect's subscribe handler returned. A throw means subscriber.error(E) and NO subscription to the
           source at all — the standard says so in a note, and it is the difference between inspect and a
           handler the page attached itself. */
        DCHECK(s->kind == K_INSPECT, "S_OP_AFTER was reached by a subscribe callback that has no callback");
        if (threw) {
            s->op1 = JS_UNDEFINED;
            obs_fail_to(ctx, s, S_DONE);
            return 0;
        }
        JS_FreeValue(ctx, s->op1);
        s->op1 = JS_UNDEFINED;
        ops_inspect_attach(ctx, s);
        return 0;
    }

    DCHECK(s->hdr.arg == OP_OP_ALGO, "S_OP_AFTER was reached by an operation with no callback in flight");

    switch (s->alg) {
    case OA_MAP_NEXT:
        if (threw) { s->op1 = JS_UNDEFINED; obs_fail_to(ctx, s, S_DONE); return 0; }
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx") + 1));
        obs_emit_enter(ctx, s, EM_NEXT, s->op1, S_DONE);
        s->op1 = JS_UNDEFINED;
        return 0;

    case OA_FILTER_NEXT: {
        bool matches;
        if (threw) { s->op1 = JS_UNDEFINED; obs_fail_to(ctx, s, S_DONE); return 0; }
        /* Web IDL `Predicate` returns `boolean`, so the return value is ToBoolean'd — a truthy object passes. */
        matches = JS_ToBool(ctx, s->op1);
        JS_FreeValue(ctx, s->op1);
        s->op1 = JS_UNDEFINED;
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx") + 1));
        if (matches) { obs_emit_enter(ctx, s, EM_NEXT, JS_DupValue(ctx, s->value), S_DONE); return 0; }
        obs_goto(s, S_DONE);
        return 0;
    }

    case OA_FOREACH_NEXT:
        if (threw) {
            /* "Reject p with E, and signal abort visitor callback controller with E." */
            JSValue e = JS_GetException(ctx);
            s->op1 = JS_UNDEFINED;
            JS_FreeValue(ctx, s->op2);
            s->op2 = JS_UNDEFINED;
            JS_FreeValue(ctx, s->op3);
            s->op3 = obs_rec_get(ctx, s->st, "csig");
            JS_FreeValue(ctx, s->creason);
            s->creason = e;
            s->has_reason = 1;
            ops_settle_enter(ctx, s, s->st, 1, JS_DupValue(ctx, s->creason), S_OP_TAIL);
            return 0;
        }
        JS_FreeValue(ctx, s->op1);
        s->op1 = JS_UNDEFINED;
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx") + 1));
        obs_goto(s, S_DONE);
        return 0;

    case OA_EVERY_NEXT: case OA_FIND_NEXT: case OA_SOME_NEXT: {
        bool passed;
        if (threw) {
            JSValue e = JS_GetException(ctx);
            s->op1 = JS_UNDEFINED;
            JS_FreeValue(ctx, s->op3);
            s->op3 = obs_rec_get(ctx, s->st, "csig");
            JS_FreeValue(ctx, s->creason);
            s->creason = e;
            s->has_reason = 1;
            ops_settle_enter(ctx, s, s->st, 1, JS_DupValue(ctx, s->creason), S_OP_TAIL);
            return 0;
        }
        passed = JS_ToBool(ctx, s->op1);
        JS_FreeValue(ctx, s->op1);
        s->op1 = JS_UNDEFINED;
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx") + 1));
        if ((s->alg == OA_EVERY_NEXT) ? !passed : passed) {
            /* every() settles FALSE on the first failure; find() settles the VALUE and some() TRUE on the
               first hit. All three then signal their own controller, with no reason — which is the abort the
               subscription's step-9.2 algorithm turns into a close. */
            JSValue settled = s->alg == OA_EVERY_NEXT ? JS_FALSE
                            : s->alg == OA_SOME_NEXT  ? JS_TRUE
                                                      : JS_DupValue(ctx, s->value);
            JS_FreeValue(ctx, s->op3);
            s->op3 = obs_rec_get(ctx, s->st, "csig");
            s->has_reason = 0;
            ops_settle_enter(ctx, s, s->st, 0, settled, S_OP_TAIL);
            return 0;
        }
        obs_goto(s, S_DONE);
        return 0;
    }

    case OA_REDUCE_NEXT:
        if (threw) {
            JSValue e = JS_GetException(ctx);
            s->op1 = JS_UNDEFINED;
            JS_FreeValue(ctx, s->op3);
            s->op3 = obs_rec_get(ctx, s->st, "csig");
            JS_FreeValue(ctx, s->creason);
            s->creason = e;
            s->has_reason = 1;
            ops_settle_enter(ctx, s, s->st, 1, JS_DupValue(ctx, s->creason), S_OP_TAIL);
            return 0;
        }
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx") + 1));
        obs_rec_set(ctx, s->st, "acc", s->op1);
        s->op1 = JS_UNDEFINED;
        obs_goto(s, S_DONE);
        return 0;

    case OA_FM_NEXT: case OA_FM_INNER_DONE: case OA_SM_NEXT:
        /* The flatmap/switchmap process-next-value steps, after the mapper: increment idx, then CONVERT what
           it returned, whose throw is also subscriber.error(E). */
        if (threw) { s->op1 = JS_UNDEFINED; obs_fail_to(ctx, s, S_DONE); return 0; }
        obs_rec_set(ctx, s->st, "idx", JS_NewInt64(ctx, obs_rec_int(ctx, s->st, "idx") + 1));
        {
            JSValue mapped = s->op1;
            s->op1 = JS_UNDEFINED;
            obs_convert_enter(ctx, s, mapped, S_OP_TAIL);
            JS_FreeValue(ctx, mapped);
        }
        return 0;

    case OA_CATCH_ERROR:
        if (threw) { s->op1 = JS_UNDEFINED; obs_fail_to(ctx, s, S_DONE); return 0; }
        {
            JSValue result = s->op1;
            s->op1 = JS_UNDEFINED;
            obs_convert_enter(ctx, s, result, S_OP_TAIL);
            JS_FreeValue(ctx, result);
        }
        return 0;

    case OA_INSP_NEXT:
        if (threw) {
            /* The handler threw: drop the abort algorithm (it is for consumer-initiated unsubscription only),
               then error the subscriber. The pass-through next() does NOT run. */
            s->op1 = JS_UNDEFINED;
            ops_inspect_drop_abort(ctx, s);
            obs_fail_to(ctx, s, S_DONE);
            return 0;
        }
        JS_FreeValue(ctx, s->op1);
        s->op1 = JS_UNDEFINED;
        obs_emit_enter(ctx, s, EM_NEXT, JS_DupValue(ctx, s->value), S_DONE);
        return 0;

    case OA_INSP_ERROR: case OA_INSP_COMPLETE: {
        int complete = s->alg == OA_INSP_COMPLETE;
        if (threw) { s->op1 = JS_UNDEFINED; obs_fail_to(ctx, s, S_DONE); return 0; }
        JS_FreeValue(ctx, s->op1);
        s->op1 = JS_UNDEFINED;
        obs_emit_enter(ctx, s, complete ? EM_COMPLETE : EM_ERROR,
                       complete ? JS_UNDEFINED : JS_DupValue(ctx, s->value), S_DONE);
        return 0;
    }

    case OA_INSP_ABORT:
        /* Invoked with "report": the throw is reported at the global and the algorithm walk continues. */
        if (threw) {
            s->op1 = JS_UNDEFINED;
            obs_report_enter(ctx, s, JS_GetException(ctx), S_DONE);
            return 0;
        }
        JS_FreeValue(ctx, s->op1);
        s->op1 = JS_UNDEFINED;
        obs_goto(s, S_DONE);
        return 0;

    default:
        DFAIL("S_OP_AFTER was reached by an algorithm that never invoked a callback");
        *pr = JS_STEP_ABRUPT;
        return 1;
    }
}

/* S_OP_TAIL — an operator's LAST step, always after something the previous stage suspended on. */
static int ops_stage_tail(JSContext *ctx, JSObsState *s, int *pr)
{
    if (s->hdr.arg == OP_OP_SUBSCRIBE) {
        switch (s->kind) {
        case K_TAKEUNTIL: {
            /* Step 5: "If subscriber's active is false, then return" — a notifier that emitted synchronously
               has already completed this subscription, and the source must not be subscribed to at all. */
            JSValue io, sig;
            JSValue active_sig = rec_sub_signal(ctx, s->st);
            bool dead = abort_signal_aborted(ctx, active_sig);
            JS_FreeValue(ctx, active_sig);
            if (dead) { obs_goto(s, S_DONE); return 0; }
            io = observer_passthrough(ctx, s->st);
            sig = rec_sub_signal(ctx, s->st);
            obs_subscribe_enter(ctx, s, s->src, io, sig, S_DONE);
            JS_FreeValue(ctx, io);
            JS_FreeValue(ctx, sig);
            return 0;
        }
        case K_FINALLY: {
            JSValue io = observer_passthrough(ctx, s->st);
            JSValue sig = rec_sub_signal(ctx, s->st);
            obs_subscribe_enter(ctx, s, s->src, io, sig, S_DONE);
            JS_FreeValue(ctx, io);
            JS_FreeValue(ctx, sig);
            return 0;
        }
        default:
            DFAIL("an operator subscribe callback reached its tail with nothing left to do");
            *pr = JS_STEP_ABRUPT;
            return 1;
        }
    }

    DCHECK(s->hdr.arg == OP_OP_ALGO, "S_OP_TAIL was reached by an operation that has no tail step");

    switch (s->alg) {
    case OA_TAKE_NEXT: {
        int64_t rem = obs_rec_int(ctx, s->st, "remaining") - 1;
        obs_rec_set(ctx, s->st, "remaining", JS_NewInt64(ctx, rem));
        if (rem == 0) { obs_emit_enter(ctx, s, EM_COMPLETE, JS_UNDEFINED, S_DONE); return 0; }
        obs_goto(s, S_DONE);
        return 0;
    }

    case OA_FM_NEXT: case OA_FM_INNER_DONE: case OA_SM_NEXT: {
        /* The convert returned. flatMap subscribes with the SUBSCRIBER's signal; switchMap with a DEPENDENT
           signal over «this inner controller's signal, the subscriber's», so the next outer value can drop
           this inner subscription without touching the outer one. */
        JSValue io, sig;
        if (JS_IsException(s->op1)) { s->op1 = JS_UNDEFINED; obs_fail_to(ctx, s, S_DONE); return 0; }
        if (s->alg == OA_SM_NEXT) {
            JSValueConst pair[2];
            JSValue inner = obs_rec_get(ctx, s->st, "innerSig");
            JSValue outer = rec_sub_signal(ctx, s->st);
            pair[0] = inner;
            pair[1] = outer;
            sig = abort_signal_dependent_new(ctx, pair, 2);
            JS_FreeValue(ctx, inner);
            JS_FreeValue(ctx, outer);
            io = observer_of(ctx, s->st, -1, -1, OA_SM_INNER_DONE);
        } else {
            sig = rec_sub_signal(ctx, s->st);
            io = observer_of(ctx, s->st, -1, -1, OA_FM_INNER_DONE);
        }
        {
            JSValue inner = s->op1;          /* TAKEN out of the slot, because the subscribe writes it back */
            s->op1 = JS_UNDEFINED;
            obs_subscribe_enter(ctx, s, inner, io, sig, S_DONE);
            JS_FreeValue(ctx, inner);
        }
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, sig);
        return 0;
    }

    case OA_CATCH_ERROR: {
        JSValue io, sig, inner;
        if (JS_IsException(s->op1)) { s->op1 = JS_UNDEFINED; obs_fail_to(ctx, s, S_DONE); return 0; }
        io = observer_passthrough(ctx, s->st);
        sig = rec_sub_signal(ctx, s->st);
        inner = s->op1;
        s->op1 = JS_UNDEFINED;
        obs_subscribe_enter(ctx, s, inner, io, sig, S_DONE);
        JS_FreeValue(ctx, inner);
        JS_FreeValue(ctx, io);
        JS_FreeValue(ctx, sig);
        return 0;
    }

    case OA_FIRST_NEXT: case OA_FOREACH_NEXT: case OA_EVERY_NEXT: case OA_FIND_NEXT:
    case OA_SOME_NEXT: case OA_REDUCE_NEXT:
        /* The promise settled; now signal the operator's own controller, which is what ends the subscription
           to `this`. `op3` and `creason` were placed by the stage that settled. */
        DCHECK(abort_signal_is(ctx, s->op3),
               "§2.3.3's operator has no controller to abort — its prologue is what builds one");
        {
            JSValue reason = s->creason;
            s->creason = JS_UNDEFINED;
            ops_abort_enter(ctx, s, s->op3, s->has_reason, reason, S_DONE);
        }
        return 0;

    default:
        DFAIL("S_OP_TAIL was reached by an algorithm that has no second step");
        *pr = JS_STEP_ABRUPT;
        return 1;
    }
}
