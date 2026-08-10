/* THE OBSERVABLE MACHINE'S PRIVATE SURFACE — shared by observable.c (§2.1/§2.2/§2.3.1) and observable_ops.c
 * (§2.3.2's Observable-returning operators and §2.3.3's promise-returning ones).
 *
 * WHY THERE IS A HEADER HERE AT ALL, AND WHY IT IS NOT A SECOND MACHINE. §2.3's operators are not a layer above
 * §2.2 — they are stages of the SAME algorithm graph. `map`'s next steps run the subscriber's next() METHOD;
 * `catch`'s error steps CONVERT a value to an Observable and SUBSCRIBE to it; `finally`'s subscribe callback
 * calls addTeardown. Written as their own machine each of those edges would be a C call into a second machine,
 * which is the re-entry the trampoline exists to remove. So there is ONE state, ONE stage enum and ONE step
 * function, and this header is what lets the operators' half of it live in its own file.
 *
 * WHAT EACH FILE OWNS. observable.c owns the state, the machine loop, and every stage of §2.1/§2.2/§2.3.1.
 * observable_ops.c owns the §2.3.2/§2.3.3 METHOD entries and the stages their algorithms rest at, reached
 * through exactly two functions — `obs_ops_entry` (a fresh invocation whose operation is an operator's) and
 * `obs_ops_stage` (a stage the core loop does not have). A stage neither file claims is a DFAIL in each. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_OBSERVABLE_IMPL_H
#define ENGINE_HOST_BROWSER_CORE_DOM_OBSERVABLE_IMPL_H
#include <stdbool.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/abort.h"
#include "core/streams/stream_work.h"
#include "core/events/report_exception.h"

/* WHICH OPERATION AN INVOCATION IS. §2.1's and §2.2's members, §2.3's operators, plus the algorithms that are
   reached as CALLABLES rather than as members: the abort algorithm a SubscribeOptions signal carries, the
   arms of §2.3.1's convert, and every operator's subscribe callback and internal-observer algorithm. */
enum {
    OP_CTOR = 0,       /* §2.2 new Observable(callback) */
    OP_SUBSCRIBE,      /* §2.2 subscribe(observer, options) */
    OP_NEXT,           /* §2.1 next(value) */
    OP_ERROR,          /* §2.1 error(error) */
    OP_COMPLETE,       /* §2.1 complete() */
    OP_ADD_TEARDOWN,   /* §2.1 addTeardown(teardown) */
    OP_UNSUB_ALGO,     /* §2.2.1 "subscribe to an Observable" step 9.2's abort algorithm */
    OP_FROM,           /* §2.3.1 Observable.from(value) */
    OP_FROM_ITER,      /* §2.3.1 "From iterable" — the returned Observable's subscribe callback */
    OP_FROM_ASYNC,     /* §2.3.1 "From async iterable" — likewise */
    OP_FROM_PROMISE,   /* §2.3.1 "From Promise" — likewise */
    OP_ITER_CLOSE,     /* §2.3.1's abort algorithm: IteratorClose / AsyncIteratorClose */
    OP_ASYNC_OK,       /* §2.3.1 nextAlgorithm's fulfilled reaction */
    OP_ASYNC_ERR,      /* §2.3.1 nextAlgorithm's rejected reaction */
    OP_PROMISE_OK,     /* §2.3.1 "From Promise" — the fulfilled reaction */
    OP_PROMISE_ERR,    /* §2.3.1 "From Promise" — the rejected reaction */
    /* §2.2.1 "SUBSCRIBE TO AN OBSERVABLE" REACHED FROM SPEC PROSE. Every §2.3 operator subscribes with an
       INTERNAL OBSERVER it built itself and a signal it chose, never with an ObserverUnion the page passed —
       and the standard splits the algorithm out of the `subscribe()` member for exactly that reason. It is a
       function object of the realm so a nested subscription is a CALL on the trampoline. */
    OP_SUBSCRIBE_NATIVE,
    OP_OP_SUBSCRIBE,   /* §2.3.2/§2.3.3: an operator's returned Observable's subscribe callback */
    OP_OP_ALGO,        /* §2.3.2/§2.3.3: one internal-observer algorithm, or an operator's abort algorithm */
    /* THE METHODS THAT ARE MACHINES. A method whose Web IDL prologue or whose own steps can reach the page's
       code is one; the rest (map, filter, flatMap, switchMap, catch, finally) convert only callback-function
       types, which is a brand check, and are plain C with an assert saying so. */
    OP_TAKE_UNTIL,     /* §2.3.2 takeUntil(any value) — step 2 CONVERTS, which reads the page's @@iterator */
    OP_TAKE,           /* §2.3.2 take(unsigned long long amount) — the coercion is the page's valueOf */
    OP_DROP,           /* §2.3.2 drop(unsigned long long amount) — likewise */
    OP_INSPECT,        /* §2.3.2 inspect(ObservableInspectorUnion) — five dictionary [[Get]]s */
    OP_TOARRAY,        /* §2.3.3 toArray(options) */
    OP_FOREACH,        /* §2.3.3 forEach(callback, options) */
    OP_EVERY,          /* §2.3.3 every(predicate, options) */
    OP_FIRST,          /* §2.3.3 first(options) */
    OP_LAST,           /* §2.3.3 last(options) */
    OP_FIND,           /* §2.3.3 find(predicate, options) */
    OP_SOME,           /* §2.3.3 some(predicate, options) */
    OP_REDUCE,         /* §2.3.3 reduce(reducer, initialValue, options) */
    OP_WHEN,           /* §3 EventTarget.when(type, options) — the two dictionary members are [[Get]]s */
    OP_N
};

/* §2.1's members, as this machine indexes them — and `addTeardown` is one of them because §2.3.2's finally()
   is specified as "run subscriber's addTeardown() METHOD with callback", which is the same kind of step as
   "run subscriber's next() method" and must reach the same function object the page can see. */
enum { EM_NEXT = 0, EM_ERROR, EM_COMPLETE, EM_TEARDOWN, EM_N };

/* WHICH OPERATOR a subscribe callback belongs to — the closure data of an OP_OP_SUBSCRIBE step closure. */
enum {
    K_TAKEUNTIL = 0, K_MAP, K_FILTER, K_TAKE, K_DROP, K_FLATMAP, K_SWITCHMAP, K_INSPECT, K_CATCH, K_FINALLY,
    K_WHEN,      /* §3 EventTarget.when() — the returned Observable's subscribe callback ADDS AN EVENT
                    LISTENER whose signal is the subscription controller's, which is what unregisters it */
    K_N
};

/* WHICH INTERNAL-OBSERVER ALGORITHM an OP_OP_ALGO closure is. The standard writes each operator's observer out
   in full, but most of the individual algorithms are literally the same three words ("run subscriber's error()
   method, given the passed in error"), so the ones that are identical share one entry here. An algorithm that
   differs by so much as a bookkeeping line gets its own — collapsing those is how an operator ends up with
   another's semantics. */
enum {
    OA_PASS_NEXT = 0,  /* "Run subscriber's next() method, given the passed in value" */
    OA_PASS_ERROR,     /* "Run subscriber's error() method, given the passed in error" */
    OA_PASS_COMPLETE,  /* "Run subscriber's complete() method" */
    OA_TU_STOP,        /* §2.3.2 takeUntil: the notifier's next AND error steps — both run complete() */
    OA_MAP_NEXT,       /* §2.3.2 map */
    OA_FILTER_NEXT,    /* §2.3.2 filter */
    OA_TAKE_NEXT,      /* §2.3.2 take */
    OA_DROP_NEXT,      /* §2.3.2 drop */
    OA_FM_NEXT,        /* §2.3.2 flatMap's outer next */
    OA_FM_COMPLETE,    /* §2.3.2 flatMap's outer complete */
    OA_FM_INNER_DONE,  /* §2.3.2 flatMap's inner complete */
    OA_SM_NEXT,        /* §2.3.2 switchMap's outer next */
    OA_SM_COMPLETE,    /* §2.3.2 switchMap's outer complete */
    OA_SM_INNER_DONE,  /* §2.3.2 switchMap's inner complete */
    OA_INSP_NEXT,      /* §2.3.2 inspect */
    OA_INSP_ERROR,
    OA_INSP_COMPLETE,
    OA_INSP_ABORT,     /* §2.3.2 inspect's abort algorithm on the subscription controller's signal */
    OA_CATCH_ERROR,    /* §2.3.2 catch */
    OA_P_REJECT,       /* §2.3.3: the abort algorithm that rejects p with the signal's abort reason */
    OA_P_ERROR,        /* §2.3.3: "Reject p with the passed in error" — every one of the eight */
    OA_TOARRAY_NEXT, OA_TOARRAY_DONE,
    OA_FOREACH_NEXT,
    OA_EVERY_NEXT, OA_EVERY_DONE,
    OA_FIRST_NEXT, OA_FIRST_DONE,
    OA_LAST_NEXT, OA_LAST_DONE,
    OA_FIND_NEXT, OA_FIND_DONE,
    OA_SOME_NEXT, OA_SOME_DONE,
    OA_REDUCE_NEXT, OA_REDUCE_DONE,
    OA_WHEN_INVOKE,    /* §3 "the observable event listener invoke algorithm" — run subscriber's next(event) */
    OA_N
};

/* WHERE THIS MACHINE RESTS, AS THE STANDARD NUMBERS IT. One machine walks §2.1's four members, §2.2's
   constructor and subscribe, §2.3.1's convert and its arms, and §2.3's operators — so a stage names the
   OPERATION it is inside rather than a private count. */
#define OBS_STAGES(X) \
    X(S_ENTRY, "Observable §2.1/§2.2 (this invocation's entry: which member or algorithm it is, and the " \
               "Observable or Subscriber it acts on)") \
    X(S_CTOR_PROTO, "Web IDL §3.7.1 (Get(newTarget, \"prototype\") — what makes " \
                    "`class O extends Observable {}` produce an O)") \
    X(S_OBSERVER_READ, "Observable §2.2.1 subscribe step 3 (processing observer: the ObserverUnion's arm, and " \
                       "a SubscriptionObserver's members read in Web IDL §3.2.18's order)") \
    X(S_OPTIONS_READ, "Web IDL §3.2.18 (converting SubscribeOptions — the `signal` member's [[Get]])") \
    X(S_ATTACH, "Observable §2.2.1 subscribe steps 5-9 (the reused-or-new Subscriber, and what the " \
                "SubscribeOptions signal registers on it)") \
    X(S_INVOKE, "Observable §2.2.1 subscribe step 10 (invoking the subscribe callback with the Subscriber, " \
                "with \"rethrow\")") \
    X(S_EMIT_ENTER, "Observable §2.1 next/error/complete step 1 (the active check, and for error and complete " \
                    "the close that precedes the push)") \
    X(S_EMIT_WALK, "Observable §2.1 next step 4 / error step 5 / complete step 5 (running each internal " \
                   "observer's steps over a COPY of the list)") \
    X(S_CLOSE_ENTER, "Observable §2.1 close-a-subscription steps 1-2 (the re-entrancy guard, then active " \
                     "becomes false)") \
    X(S_CLOSE_ABORT, "Observable §2.1 close-a-subscription step 3 (signal abort the subscription controller: " \
                     "its abort algorithms, then the `abort` event)") \
    X(S_CLOSE_TEARDOWN, "Observable §2.1 close-a-subscription step 4 (each teardown, in REVERSE insertion " \
                        "order, invoked with \"report\")") \
    X(S_TEARDOWN_NOW, "Observable §2.1 addTeardown step 3 (the subscription is already inactive, so the " \
                      "teardown is invoked immediately with \"report\")") \
    X(S_REPORT, "HTML §8.1.4.6 report an exception (the `error` event fired at the global — the page's " \
                "onerror runs here), reached from §2.1's error(), its default error algorithm, and every " \
                "callback the standard invokes with \"report\"") \
    X(S_EMIT_CALL, "Observable §2.3 (running a Subscriber's next()/error()/complete() METHOD from a native " \
                   "subscribe callback — the operators and from() are all specified that way)") \
    X(S_FROM_PROBE_ASYNC, "Observable §2.3.1 convert-to-an-Observable step 3 " \
                          "(GetMethod(value, %Symbol.asyncIterator%))") \
    X(S_FROM_PROBE_SYNC, "Observable §2.3.1 convert-to-an-Observable \"From iterable\" step 1 " \
                         "(GetMethod(value, %Symbol.iterator%))") \
    X(S_ITER_METHOD, "Observable §2.3.1 (GetIterator step 1: re-reading the iterator method off the source, " \
                     "which the standard notes is deliberate)") \
    X(S_ITER_CALL, "Observable §2.3.1 (GetIterator step 2: calling the iterator method)") \
    X(S_ITER_NEXTFN, "Observable §2.3.1 (GetIterator step 5: Get(iterator, \"next\"))") \
    X(S_ITER_WRAP, "Observable §2.3.1 (GetIterator(value, async) step 1.b: CreateAsyncFromSyncIterator over " \
                   "a source with no %Symbol.asyncIterator%)") \
    X(S_ITER_STEP, "Observable §2.3.1 (IteratorNext: calling the iterator's `next` — and the yield point of " \
                   "an unbounded iteration)") \
    X(S_ITER_DONE, "Observable §2.3.1 (IteratorComplete: Get(iteratorResult, \"done\"))") \
    X(S_ITER_VALUE, "Observable §2.3.1 (IteratorValue: Get(iteratorResult, \"value\"))") \
    X(S_ITER_RETURN_FN, "Observable §2.3.1 (IteratorClose step 4: Get(iterator, \"return\"))") \
    X(S_ITER_RETURN_CALL, "Observable §2.3.1 (IteratorClose step 6: calling `return`)") \
    X(S_ASYNC_PROMISE, "Observable §2.3.1 nextAlgorithm step 5 (a promise resolved with what the async " \
                       "iterator's `next` returned — 27.2.1.3.2 step 8's `then` read is the page's)") \
    X(S_ASYNC_REACT, "Observable §2.3.1 nextAlgorithm step 6 (reacting to nextPromise)") \
    X(S_PROMISE_REACT, "Observable §2.3.1 \"From Promise\" (reacting to the value)") \
    X(S_PROMISE_DONE, "Observable §2.3.1 \"From Promise\" (the subscriber's complete(), after its next())") \
    X(S_SUB_NATIVE, "Observable §2.2.1 (a spec-prose \"Subscribe to an Observable\" — the operators' " \
                    "subscription, made with an internal observer and a signal of the standard's own choosing)") \
    X(S_OP_CONVERT, "Observable §2.3.2 (calling from() with a value an operator produced: takeUntil's " \
                    "notifier, and flatMap/switchMap/catch's innerObservable)") \
    X(S_OP_ENTER, "Observable §2.3.2/§2.3.3 (an operator's subscribe callback or internal-observer algorithm, " \
                  "at its first step — the bookkeeping the standard states before any of the page's code)") \
    X(S_OP_CALLBACK, "Observable §2.3.2/§2.3.3 (invoking the operator's Mapper/Predicate/Reducer/Visitor/" \
                     "CatchCallback/inspector handler, with \"rethrow\")") \
    X(S_OP_AFTER, "Observable §2.3.2/§2.3.3 (the operator's step after its callback returned — the emit, the " \
                  "settle, or the second subscription)") \
    X(S_OP_TAIL, "Observable §2.3.2/§2.3.3 (an operator's LAST step, reached after the emit its previous step " \
                 "made: take's complete() after its next(), flatMap's complete() after its inner one's)") \
    X(S_OP_SETTLE, "Observable §2.3.3 (resolving or rejecting the operator's promise — a call of the " \
                   "capability's resolving function, which is where 27.2.1.3.2 reads `then`)") \
    X(S_OP_ABORT, "Observable §2.3.3 (signal abort the operator's own controller, after its promise settled — " \
                  "every()'s false, first()'s value, find()'s hit, and every predicate that threw)") \
    X(S_DONE, "Observable §2.1/§2.2 (the operation is complete)")
/* THE X-LIST IS SHARED; ITS EXPANSIONS ARE NOT. Each translation unit expands the constants for itself, which
   is what keeps `engine/check_step_visits.mjs` able to see, in the file that declares the definition, that the
   labels and the constants come from the ONE list. A header that expanded the enum instead would put the
   constants somewhere the gate does not read, which is the excluded-check shape. */

typedef struct JSObsState {
    JSStepHdr hdr;        /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   result;     /* what this entry answers (owned) */
    JSValue   obs;        /* the Observable being subscribed to (owned) */
    JSValue   sub;        /* the Subscriber (owned) */
    JSValue   io;         /* the internal observer being attached or walked (owned) */
    JSValue   sig;        /* the SubscribeOptions signal, or undefined (owned) */
    JSValue   list;       /* the snapshot being walked — observers, or teardowns (owned) */
    JSValue   value;      /* the value/error being pushed (owned) */
    /* THE CLOSE'S REASON, IN ITS OWN SLOT. §2.1's error() closes the subscription with the error AND THEN
       pushes that same error to every observer, so the two cannot share `value` — the close would free what
       the walk is about to hand over. */
    JSValue   creason;
    JSValue   iocb[3];    /* the internal observer's three algorithms as they are read; in §2.3.1's arms,
                             [0] is the iterator's `next` method and [1] the iterator result (owned) */
    /* §2.3'S OPERATOR SLOTS. `src` is the operator's source Observable and `st` its per-subscription state
       record; `op1`..`op3` are the operands a stage holds across its own request. They are separate from the
       core's because an operator stage can be reached WHILE the core's are live — catch's error steps run
       inside §2.1's walk, which owns `list`, `value` and `i`. */
    JSValue   src;
    JSValue   st;
    JSValue   op1, op2, op3;
    JSValue   cb[6];      /* the call request's buffer: [this, func, args...], and the widest call here is
                             §2.2.1's native subscribe (observable, internal observer, signal) and §2.3.3's
                             Reducer (accumulator, currentValue, index) — three arguments, so five slots */
    AbortSignalWork aw;   /* §3.2 "signal abort", as a request */
    StreamWork sw;        /* PromiseResolve, for §2.3.1's nextPromise */
    ReportExceptionWork rw;   /* HTML §8.1.4.6 "report an exception", as a request */
    JSValue   rerr;       /* what is being reported, held across that dispatch (owned) */
    int64_t   i;          /* the walk cursor */
    uint8_t   phase;      /* step_call_run's */
    uint8_t   next;       /* the stage the close sequence, or an emitted call, returns to */
    uint8_t   emit;       /* EM_NEXT / EM_ERROR / EM_COMPLETE */
    uint8_t   member;     /* which dictionary member a read is on; the iteration's yield latch */
    uint8_t   has_sig;    /* the SubscribeOptions declared a signal */
    uint8_t   has_reason; /* close was given a reason */
    /* §2.3.1's ARM: 0 = the sync iterable, 1 = the async one, 2 = the async one whose source turned out to
       have no %Symbol.asyncIterator% after all, so GetIterator(value, async) falls back to the sync protocol
       wrapped in a CreateAsyncFromSyncIterator. */
    uint8_t   async;
    uint8_t   reject;     /* the PromiseResolve in flight is a REJECTION (the async `next` threw) */
    /* THE REPORT'S OWN RETURN STAGE, and it may not be `next`. A report happens INSIDE a walk that already
       owns `next` — the teardown loop's continuation, the close sequence's — so sharing one byte would send
       the walk to the close's exit the first time a callback threw. */
    uint8_t   rnext;
    /* §2.3'S OWN RETURN STAGES, each its own byte for the same reason `rnext` is: a native subscription can be
       made from inside an algorithm that has already parked an emit on `next`, and takeUntil makes two of them
       in a row around one. */
    uint8_t   snext;      /* the stage a spec-prose "Subscribe to an Observable" returns to */
    uint8_t   fnext;      /* the stage "convert to an Observable" returns to */
    uint8_t   kind;       /* K_* — which operator this invocation belongs to */
    uint8_t   alg;        /* OA_* — which internal-observer algorithm it is */
} JSObsState;

/* ---- what observable.c lends observable_ops.c ------------------------------------------------------------- */

/* The step id this runtime gave one of the operations above. */
int      obs_stepid(int op);
/* A per-subscription state RECORD: a null-prototyped engine-owned object, so reading it back runs none of the
   page's code, and a JS value, so its mutations ride the per-flow COW delta. */
JSValue  obs_record_new(JSContext *ctx);
JSValue  obs_rec_get(JSContext *ctx, JSValueConst rec, const char *name);
void     obs_rec_set(JSContext *ctx, JSValueConst rec, const char *name, JSValue v);   /* v CONSUMED */
bool     obs_rec_bool(JSContext *ctx, JSValueConst rec, const char *name);
int64_t  obs_rec_int(JSContext *ctx, JSValueConst rec, const char *name);
/* §2.2.1's "internal observer" — three algorithms, each a callable or undefined. Arguments BORROWED. */
JSValue  obs_internal_observer_new(JSContext *ctx, JSValueConst n, JSValueConst e, JSValueConst c);
/* An OP_OP_ALGO step closure over `alg` and the subscription's state record. */
JSValue  obs_algo_new(JSContext *ctx, int alg, JSValueConst rec);
/* A new Observable whose subscribe callback is an OP_OP_SUBSCRIBE closure over «kind, source, arg». `arg` may
   be undefined for the operators that take none. */
JSValue  obs_operator_observable(JSContext *ctx, int kind, JSValueConst source, JSValueConst arg);
bool     obs_is_observable(JSValueConst v);
bool     obs_is_subscriber(JSValueConst v);
/* A Subscriber's own subscription controller's signal. OWNED. */
JSValue  obs_subscriber_signal(JSContext *ctx, JSValueConst sub);
/* Move the machine to `stage`, asserting that no request is still in flight. */
void     obs_goto(JSObsState *s, int stage);
/* Run one of §2.1's three members on `s->sub`, returning to `ret`. `value` is CONSUMED. */
void     obs_emit_enter(JSContext *ctx, JSObsState *s, int which, JSValue value, int ret);
/* The live exception becomes `subscriber.error(E)`, returning to `ret`. */
void     obs_fail_to(JSContext *ctx, JSObsState *s, int ret);
/* HTML §8.1.4.6 "report an exception", returning to `ret` — what "invoke … with \"report\"" does with a
   throw. `err` is CONSUMED. */
void     obs_report_enter(JSContext *ctx, JSObsState *s, JSValue err, int ret);
/* §2.2.1 "Subscribe to an Observable" from spec prose, returning to `ret`. All three BORROWED. */
void     obs_subscribe_enter(JSContext *ctx, JSObsState *s, JSValueConst observable, JSValueConst io,
                             JSValueConst signal, int ret);
/* §2.3.1 "convert to an Observable", as a CALL so its TypeError arrives as a value. `value` BORROWED. */
void     obs_convert_enter(JSContext *ctx, JSObsState *s, JSValueConst value, int ret);

/* THE OPERATORS' HALF OF THE MACHINE. `obs_ops_entry` runs S_ENTRY for an operation observable_ops.c owns;
   `obs_ops_stage` runs a stage it owns. Both answer the way the core loop's arms do: 1 = "*pr is what step()
   must return", 0 = "the stage moved; keep looping". */
int obs_ops_entry(JSContext *ctx, JSObsState *s, int op, int *pr);
int obs_ops_stage(JSContext *ctx, JSObsState *s, JSValue *pcb_result, JSValue **out_cb, int *out_argc, int *pr);

/* §2.3's METHODS THAT RUN NONE OF THE PAGE'S CODE — map, filter, flatMap, switchMap, catch, finally. Their only
   Web IDL conversion is a callback-function type, which is a brand check, so they are plain C functions and
   this is where they are installed from. */
void obs_ops_install(JSContext *ctx, JSValueConst proto);

#endif
