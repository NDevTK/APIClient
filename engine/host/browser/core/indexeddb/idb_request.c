/* INDEXED DATABASE §2.8's REQUEST — the state machine that reaches `done` and fires at the page, and §4.1's
 * IDBRequest over it.
 *
 * WHY THIS IS A DECLARED STEP MACHINE AND NOT A C FUNCTION. §2.8 is two flags and two values — processed,
 * done, result, error — and the ONLY interesting thing about them is WHEN they change: "when a request is made,
 * a new request is returned with its done flag set to false ... if a request completes successfully, its done
 * flag is set to true, its result is set to the result of the request, and an event with type success is fired
 * at the request". That fire is the PAGE'S code, running with the transaction ACTIVE, free to place further
 * requests against it, to throw, to loop, to await. A C body that dispatched from a `JS_Call` would be the
 * drive-to-completion this engine aborts on; a `while` over the listener list would be a second event loop. So
 * the whole of §5.6, §5.9 and §5.10 is ONE machine that RESTS at the dispatch, and the page's listeners run as
 * ordinary preemptible flows while siblings overtake it.
 *
 * IT IS ALSO WHY THE STAGES ARE THE SPEC'S STEPS AND NOT A COUNTER. A parked flow can be resumed by a
 * cold-tier read in another session, and what it holds is the LABEL — "§5.9 step 8" — so a request suspended
 * inside its own success dispatch last week resumes at the dispatch and not one step either side of it.
 *
 * WHAT A FLOW OWNS HERE. The four fields live in internal slots under a private Symbol on the IDBRequest
 * object, so every write is an ordinary property write the per-flow COW delta captures: flow A's request can
 * be `done` with a result while its sibling's is still `pending`, and neither can see the other's. The result
 * itself is whatever §6.2 answered with — including a CONCOLIC, which is the point of storing values as live
 * JS values in the first place (core/indexeddb/idb_database.c): a page that put `location.hash` in a store and
 * reads it back through `request.result` gets the tainted value, and the gate it feeds forks.
 *
 * §2.8.1's OPEN REQUEST IS HERE TOO, and it is the same RECORD wearing a second INTERFACE. "An open request is
 * a special type of request" — the six fields are identical, and what differs is entirely above them: two more
 * events may be fired at it (`blocked`, `upgradeneeded`), its source is always null, its get-the-parent returns
 * null instead of its transaction, and it is never placed against a transaction by §5.6 — so §5.1, §5.7 and
 * §4.3's completion task write its fields themselves, through the entries this file exports for them. The
 * second CLASS exists because a class is what carries the per-realm prototype slot, and one class for both
 * would put `onupgradeneeded` on every `store.get()` request. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"

/* §2.8's own fields, spelled once. */
#define RQ_SOURCE       "source"        /* §2.8's source object, JS_NULL for an open request */
#define RQ_TRANSACTION  "transaction"   /* §2.8's transaction, JS_NULL until it is placed against one */
#define RQ_RESULT       "result"
#define RQ_ERROR        "error"         /* a DOMException, or JS_NULL for "no error occurred" */
#define RQ_PROCESSED    "processed"     /* §2.8's processed flag */
#define RQ_DONE         "done"          /* §2.8's done flag */

static JSValue g_key;
static int     g_ready;
static JSClassID g_req_class;
/* §2.8.1's OPEN REQUEST wears its own class, because §4.1 gives it its own INTERFACE — and the class is what
   carries the per-realm prototype slot, so one class for both would put `onupgradeneeded` on every `store.get`
   request. The RECORD is identical (the same six fields under the same private Symbol), which is §2.8.1's own
   sentence: an open request is a request, with two more events fired at it. */
static JSClassID g_open_class;
static JSRuntime *g_req_rt;
static int g_exec_stepid = -1, g_abort_stepid = -1;

/* ---- the record ---------------------------------------------------------------------------------------- */

/* The request's internal slots, or UNDEFINED for a value that is not one. An OWN SLOT read — never a lookup,
   which would mint a concolic for an internal slot at the solver's absent-state seam. OWNED. */
static JSValue rq_slots(JSContext *ctx, JSValueConst req)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an IDBRequest slot was asked for before idb_request_init made the key");
    if (!JS_IsObject(req))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBRequest slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &st, req, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

bool idb_request_is(JSValueConst v)
{
    JSClassID id = JS_GetClassID(v);

    return (g_req_class != 0 && id == g_req_class) || (g_open_class != 0 && id == g_open_class);
}

bool idb_request_is_open(JSValueConst v)
{
    return g_open_class != 0 && JS_GetClassID(v) == g_open_class;
}

static bool rq_brand(JSContext *ctx, JSValueConst this_val)
{
    if (idb_request_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "an IDBRequest member was reached on something that is not an IDBRequest");
    return false;
}

static JSValue rq_get(JSContext *ctx, JSValueConst req, const char *field)
{
    JSValue slots = rq_slots(ctx, req), v;

    DCHECK(JS_IsObject(slots), "an IDBRequest field was read off a value carrying no slot record");
    v = JS_GetPropertyStr(ctx, slots, field);
    JS_FreeValue(ctx, slots);
    return v;
}

/* `v` is CONSUMED. */
static void rq_set(JSContext *ctx, JSValueConst req, const char *field, JSValue v)
{
    JSValue slots = rq_slots(ctx, req);

    DCHECK(JS_IsObject(slots), "an IDBRequest field was written on a value carrying no slot record");
    JS_SetPropertyStr(ctx, slots, field, v);
    JS_FreeValue(ctx, slots);
}

static bool rq_flag(JSContext *ctx, JSValueConst req, const char *field)
{
    JSValue v = rq_get(ctx, req, field);
    bool b = JS_ToBool(ctx, v);

    JS_FreeValue(ctx, v);
    return b;
}

/* §2.8's "a new request with source as source", built with both flags false and neither value accessible.
   `source` is BORROWED. `cls` is which of §4.1's two interfaces it wears — the RECORD is the same either way,
   which is §2.8.1's own sentence. OWNED. */
static JSValue rq_new(JSContext *ctx, JSValueConst source, JSClassID cls)
{
    JSValue req, st, proto;
    JSAtom k;

    DCHECK(g_ready, "an IDBRequest was created before idb_request_init declared the interface");
    DCHECK(cls == g_req_class || cls == g_open_class,
           "a request was created wearing a class that is not one of §4.1's two interfaces");
    proto = JS_GetClassProto(ctx, cls);
    DCHECK(!JS_IsNull(proto), "an IDBRequest prototype was asked for in a realm that never ran its install");
    req = JS_NewObjectProtoClass(ctx, proto, cls);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(req), "IndexedDB: the IDBRequest allocation failed");
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "IndexedDB: the IDBRequest slot record allocation failed");
    JS_SetPropertyStr(ctx, st, RQ_SOURCE, JS_DupValue(ctx, source));
    /* "A request has a transaction which is initially null. This will be set when a request is placed against
       a transaction using the steps to asynchronously execute a request." */
    JS_SetPropertyStr(ctx, st, RQ_TRANSACTION, JS_NULL);
    JS_SetPropertyStr(ctx, st, RQ_RESULT, JS_UNDEFINED);
    JS_SetPropertyStr(ctx, st, RQ_ERROR, JS_NULL);
    JS_SetPropertyStr(ctx, st, RQ_PROCESSED, JS_FALSE);
    JS_SetPropertyStr(ctx, st, RQ_DONE, JS_FALSE);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBRequest slot key could not be interned");
    JS_SetProperty(ctx, req, k, st);
    JS_FreeAtom(ctx, k);
    return req;
}

/* §2.8.1's OPEN REQUEST. "The source of an open request is always null", which is the argument this call
   passes and the whole of what makes the source parameter absent here. */
JSValue idb_request_new_open(JSContext *ctx)
{
    return rq_new(ctx, JS_NULL, g_open_class);
}

/* ---- §2.8's four fields, for the algorithms outside this component that write them -------------------------
 *
 * WHY THE WRITES ARE HERE AND NOT AT THEIR CALLERS. §5.1, §5.7, §5.5 step 7.3 and §4.3's completion task each
 * write a request's result, error, transaction and two flags directly, because an open request is never placed
 * against a transaction and so never reaches §5.6's task, which is where every other request's fields are
 * written. Four files reaching into a slot record under this component's private Symbol would be four copies of
 * this file's field names; asked of the component, there is one, and each write asserts what §2.8 says the
 * field is. */

void idb_request_set_result(JSContext *ctx, JSValueConst req, JSValue result)
{
    DCHECK(idb_request_is(req), "a request result was written on something that is not a request");
    rq_set(ctx, req, RQ_RESULT, result);
}

void idb_request_set_error(JSContext *ctx, JSValueConst req, JSValue error)
{
    DCHECK(idb_request_is(req), "a request error was written on something that is not a request");
    DCHECK(JS_IsUndefined(error) || JS_IsObject(error),
           "§2.8's error is a DOMException or nothing — `DOMException? error` is what §4.1's getter reports, "
           "and a non-object here would be a value that getter has no answer for");
    rq_set(ctx, req, RQ_ERROR, error);
}

void idb_request_set_transaction(JSContext *ctx, JSValueConst req, JSValue tx)
{
    DCHECK(idb_request_is(req), "a request transaction was written on something that is not a request");
    DCHECK(JS_IsNull(tx) || idb_transaction_is(tx),
           "§2.8's transaction is a transaction or null — §5.4 step 2.5.4 and §5.5 step 7.3 each set it back "
           "to null once the upgrade transaction it named is gone");
    rq_set(ctx, req, RQ_TRANSACTION, tx);
}

void idb_request_set_done(JSContext *ctx, JSValueConst req, bool done)
{
    DCHECK(idb_request_is(req), "a request done flag was written on something that is not a request");
    rq_set(ctx, req, RQ_DONE, JS_NewBool(ctx, done));
}

void idb_request_set_processed(JSContext *ctx, JSValueConst req, bool processed)
{
    DCHECK(idb_request_is(req), "a request processed flag was written on something that is not a request");
    rq_set(ctx, req, RQ_PROCESSED, JS_NewBool(ctx, processed));
}

JSValue idb_request_error(JSContext *ctx, JSValueConst req)
{
    JSValue e;

    DCHECK(idb_request_is(req), "a request error was read off something that is not a request");
    e = rq_get(ctx, req, RQ_ERROR);
    /* The record spells "no error occurred" as UNDEFINED where §5.6 step 5.6.3.2 wrote one and as NULL where
       the request was built; §5.1 step 10.9 asks one question of both, so both answer it the same way. */
    if (JS_IsUndefined(e))
        return JS_NULL;
    return e;
}

JSValue idb_request_transaction(JSContext *ctx, JSValueConst req)
{
    DCHECK(idb_request_is(req), "a request transaction was read off something that is not a request");
    return rq_get(ctx, req, RQ_TRANSACTION);
}

/* A DOMException as a VALUE. Built by throwing one and taking it back, because that is the engine's only
   constructor for the interface and it runs none of the page's code — reading `DOMException` off the global
   would, since a page may replace it. (core/dom/abort.c derives the same value the same way.) */
static JSValue rq_dom_exception(JSContext *ctx, const char *name, const char *msg)
{
    JS_ThrowDOMException(ctx, name, "%s", msg);
    return JS_GetException(ctx);
}

/* ---- §5.6's ASYNCHRONOUSLY EXECUTE A REQUEST, and §5.9/§5.10's fire ------------------------------------- */

/* What the task this machine IS was minted over. Captured by the closure, because §scheduler's rule is that an
   operation which becomes a work item takes its inputs with it. */
#define RQ_CD_REQUEST    0
#define RQ_CD_OPERATION  1

typedef struct {
    JSStepHdr   hdr;          /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t     started;      /* see rq_state_start */
    uint8_t     phase;        /* the operation CALL's cursor, and later the dispatch request's */
    JSValue     result;       /* §5.6's `result`: the operation's answer, or the DOMException it threw (owned) */
    JSValue     ev;           /* the success/error event being dispatched (owned) */
    EventFireCb cb;           /* the request buffer — wide enough for §2.9's three-argument dispatch, and so
                                 for the zero-argument operation call that uses it in an earlier stage */
    uint8_t     is_error;     /* §5.6's "result is an error" */
    uint8_t     not_canceled; /* §5.10 step 9.3's "event's canceled flag", as the dispatch answers it */
} JSIdbReqState;

static void js_idb_req_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdbReqState *s = st;
    int i;

    v->val(ctx, &s->result);
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* A STEP STATE IS js_mallocz'd, AND A ZEROED JSValue IS THE INTEGER 0 — JS_TAG_INT is 0, so a zeroed slot is
   not JS_UNDEFINED and `JS_IsUndefined(s->ev)` would answer false for a slot nothing has written. Every value
   slot is therefore placed before anything can read one, and `started` is the byte that says whether it has
   been: quickjs-step.h's own note is that a machine can never answer "have I started?" from its stage, since
   the first stage's constant is also the entry stage. It runs ABOVE the dispatch, where code that legitimately
   runs on every entry belongs. */
static void rq_state_start(JSIdbReqState *s)
{
    int i;

    if (s->started)
        return;
    s->started = 1;
    s->result = JS_UNDEFINED;
    s->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
}

#define RQX_STAGES(X) \
    X(RQX_PERFORM,          "Indexed Database §5.6 step 5.2 (let result be the result of performing " \
                            "operation)") \
    X(RQX_ABORT_COMMITTING, "Indexed Database §5.6 step 5.3 (result is an error and the transaction is " \
                            "committing: abort the transaction with it)") \
    X(RQX_PROCESSED,        "Indexed Database §5.6 step 5.5 (set request's processed flag to true)") \
    X(RQX_DELIVER,          "Indexed Database §5.6 steps 5.6.1-5.6.3 — ONE O(1) engine action: the request " \
                            "leaves the transaction's list at its head, its done flag is set, and its result " \
                            "or its error is written") \
    X(RQX_ACTIVATE,         "Indexed Database §5.9 steps 1-7 / §5.10 steps 1-7 — ONE O(1) engine action: the " \
                            "event is created with its type and its two flags, and an INACTIVE transaction " \
                            "is made active for the dispatch") \
    X(RQX_DISPATCH,         "Indexed Database §5.9 step 8 / §5.10 step 8 (dispatch event at request with " \
                            "legacyOutputDidListenersThrowFlag)") \
    X(RQX_DEACTIVATE,       "Indexed Database §5.9 step 9 / §5.10 step 9 (set the transaction inactive, then " \
                            "abort it or commit it)")
enum { RQX_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RQX_STEPS[] = { RQX_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_req_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbReqState *s = st;
    JSValueConst req = JS_StepClosureData(&s->hdr, RQ_CD_REQUEST);
    JSValue tx = JS_UNDEFINED;
    int r = 0;

    DCHECK(idb_request_is(req), "an Indexed Database request task was minted over something that is not a "
                                "request");

    rq_state_start(s);
    STEP_DISPATCH(RQX_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(RQX_PERFORM);
        /* §5.6's preamble: "These steps can be aborted at any point if the transaction the created request
           belongs to is aborted using the steps to abort a transaction." §5.5 step 6 does exactly that — it
           sets the processed flag and queues the request's AbortError task — so a request already processed
           when this task runs is one the abort has claimed, and performing its operation now would write to a
           database whose transaction is finished. */
        if (rq_flag(ctx, req, RQ_PROCESSED)) {
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;
        }
        /* §5.6 step 5.1: "Wait until request is the first item in transaction's request list that is not
           processed." The wait is DISCHARGED BY THE QUEUE — a transaction's requests are queued as tasks in
           the order they were placed and each completes inside its own task — so this is an assertion rather
           than a loop, and it fires if two of one transaction's requests ever run out of order. */
        tx = rq_get(ctx, req, RQ_TRANSACTION);
        DCHECK(idb_transaction_is(tx), "a request task ran for a request that was never placed against a "
                                       "transaction — §5.6 step 4 adds it to one before this task exists");
        JS_FreeValue(ctx, tx);
        {
            JSValueConst op = JS_StepClosureData(&s->hdr, RQ_CD_OPERATION);

            DCHECK(JS_IsFunction(ctx, op), "a request was executed with an operation that is not callable — "
                                           "§5.6's operation is \"an operation to perform on a database\", "
                                           "and this engine states one as a step closure over its operands");
            /* THE OPERATION IS A CALL REQUEST, so an operation of the page's size (a getAll over a store, a
               cursor walk) parks like anything else. This machine declares catches_abrupt, so the DOMException
               the algorithm names arrives as a VALUE here rather than tearing the machine down — which is what
               §5.6's "if result is an error" is asking about. */
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), op, JS_UNDEFINED, 0, NULL, cb_result, &s->result,
                              out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
        }
        if (JS_IsException(s->result)) {
            s->result = JS_GetException(ctx);
            s->is_error = 1;
        }
        STEP_GOTO(s->hdr.stage, RQX_ABORT_COMMITTING, &s->phase, NULL);

    STEP_ARM(RQX_ABORT_COMMITTING);
        /* §5.6 step 5.3: "If result is an error and transaction's state is committing, then run abort a
           transaction with transaction and result, and terminate these steps." No event is fired for this
           request at all — the transaction's `abort` event is the whole report. */
        tx = rq_get(ctx, req, RQ_TRANSACTION);
        if (s->is_error && idb_transaction_state(ctx, tx) == IDB_TX_COMMITTING) {
            idb_transaction_abort(ctx, tx, s->result);
            s->result = JS_UNDEFINED;   /* consumed by the abort */
            JS_FreeValue(ctx, tx);
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;
        }
        JS_FreeValue(ctx, tx);
        STEP_GOTO(s->hdr.stage, RQX_PROCESSED, &s->phase, NULL);

    STEP_ARM(RQX_PROCESSED);
        /* §5.6 step 5.4 ("if result is an error, then revert all changes made by operation") has nothing to
           undo: the operation's contract is that it is ATOMIC, and §6.1 — the only operation this engine has
           — reports every failure before it copies the value it was given. That is a property of the
           operations rather than a step skipped, and it is stated at the contract in idb_request.h so an
           operation that breaks it cannot be written by accident.
           §5.6 step 5.5. */
        rq_set(ctx, req, RQ_PROCESSED, JS_TRUE);
        STEP_GOTO(s->hdr.stage, RQX_DELIVER, &s->phase, NULL);

    STEP_ARM(RQX_DELIVER);
        /* §5.6 step 5.6's database task, steps 1-3. The three sub-steps are one stage because together they
           are one O(1) engine action: the removal is the head of the transaction's request list (the cursor
           idb_transaction.h explains), and the rest is two flag writes and a value. */
        tx = rq_get(ctx, req, RQ_TRANSACTION);
        idb_transaction_remove_request(ctx, tx, req);
        JS_FreeValue(ctx, tx);
        rq_set(ctx, req, RQ_DONE, JS_TRUE);
        if (s->is_error) {
            rq_set(ctx, req, RQ_RESULT, JS_UNDEFINED);
            rq_set(ctx, req, RQ_ERROR, JS_DupValue(ctx, s->result));
        } else {
            rq_set(ctx, req, RQ_RESULT, JS_DupValue(ctx, s->result));
            rq_set(ctx, req, RQ_ERROR, JS_UNDEFINED);
        }
        STEP_GOTO(s->hdr.stage, RQX_ACTIVATE, &s->phase, NULL);

    STEP_ARM(RQX_ACTIVATE);
        /* §5.9 steps 1-3 / §5.10 steps 1-3: an Event whose type is `success` with both flags FALSE, or
           `error` with both flags TRUE. The difference is the whole of what tells the two algorithms apart at
           the page: an error event BUBBLES to the transaction and the connection, and `preventDefault()` on it
           is what stops step 9.3 aborting the transaction.
           §5.9 steps 5-7 / §5.10 steps 5-7: the transaction is ACTIVE for the duration of the dispatch, which
           is what lets a success handler place the next request against it. */
        if (JS_IsUndefined(s->ev)) {
            s->ev = event_new(ctx, s->is_error ? "error" : "success", s->is_error != 0, s->is_error != 0);
            if (JS_IsException(s->ev)) {
                s->ev = JS_UNDEFINED;
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_ABRUPT;
            }
        }
        tx = rq_get(ctx, req, RQ_TRANSACTION);
        if (idb_transaction_state(ctx, tx) == IDB_TX_INACTIVE)
            idb_transaction_set_state(ctx, tx, IDB_TX_ACTIVE);
        JS_FreeValue(ctx, tx);
        STEP_GOTO(s->hdr.stage, RQX_DISPATCH, &s->phase, NULL);

    STEP_ARM(RQX_DISPATCH);
        {
            bool not_canceled = false;

            r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), req, s->ev, JS_UNDEFINED, cb_result,
                                      &not_canceled, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            if (r < 0) return JS_STEP_ABRUPT;
            s->not_canceled = not_canceled ? 1 : 0;
        }
        STEP_GOTO(s->hdr.stage, RQX_DEACTIVATE, &s->phase, NULL);

    STEP_ARM(RQX_DEACTIVATE);
        JS_FreeValue(ctx, cb_result);
        tx = rq_get(ctx, req, RQ_TRANSACTION);
        /* §5.9 step 9 / §5.10 step 9: "If transaction's state is active, then:". A listener that committed or
           aborted the transaction has already moved it, and then none of this runs. */
        if (idb_transaction_state(ctx, tx) == IDB_TX_ACTIVE) {
            idb_transaction_set_state(ctx, tx, IDB_TX_INACTIVE);
            if (event_listeners_threw(ctx, s->ev)) {
                /* §5.9 step 9.2 / §5.10 step 9.2. §5.10 says "and terminate these steps ... This is done even
                   if event's canceled flag is false"; §5.9's wording omits the terminate, and THAT WORDING
                   CANNOT BE FOLLOWED — its step 9.3 would then commit a transaction this step has just made
                   FINISHED, which is not a transition §2.7.1 has and which idb_transaction_set_state refuses.
                   The two algorithms are the same shape and §5.10 states it; the omission is editorial. */
                idb_transaction_abort(ctx, tx, rq_dom_exception(ctx, "AbortError",
                                                               "a listener threw during the request's event"));
            } else if (s->is_error && s->not_canceled) {
                /* §5.10 step 9.3: an error event nobody called preventDefault() on ABORTS the transaction with
                   the request's own error. That is the sentence that makes an unhandled IndexedDB failure roll
                   the whole transaction back, and it is why the error event is cancelable at all. */
                idb_transaction_abort(ctx, tx, rq_get(ctx, req, RQ_ERROR));
            } else if (idb_transaction_requests_empty(ctx, tx)) {
                /* §5.9 step 9.3 / §5.10 step 9.4. */
                idb_transaction_commit(ctx, tx);
            }
        }
        JS_FreeValue(ctx, tx);
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_req_def = {
    sizeof(JSIdbReqState), js_idb_req_step, NULL, 0,
    /* catches_abrupt: §5.6 step 5.3's "if result is an error" is a test on a VALUE, so the DOMException the
       operation names has to arrive here rather than tearing the machine down and losing the request. */
    .catches_abrupt = 1, .visit = js_idb_req_visit,
    .algorithm = "Indexed Database §5.6's asynchronously execute a request, ending in §5.9's fire a success "
                 "event or §5.10's fire an error event",
    .steps = RQX_STEPS
};

JSValue idb_request_execute(JSContext *ctx, JSValueConst source, JSValueConst transaction,
                            JSValueConst operation)
{
    JSValueConst data[2];
    JSValue req, fn;

    /* §5.6 step 2: "Assert: transaction's state is active." A request may only be made against an active
       transaction, which is §2.7.1's whole reason for having the state: `store.get()` from a `setTimeout`
       after the creating task has returned is a "TransactionInactiveError" reported by §4.6's member, BEFORE
       this algorithm is reached. */
    DCHECK(idb_transaction_is(transaction), "a request was executed against something that is not a transaction");
    DCHECK(idb_transaction_state(ctx, transaction) == IDB_TX_ACTIVE,
           "§5.6 was reached with a transaction that is not ACTIVE. Its step 2 asserts this, because the "
           "member that placed the request is what reports a \"TransactionInactiveError\" for an inactive one "
           "— an assert firing here means that member did not");
    /* §5.6 step 3: "If request was not given, let request be a new request with source as source." The
       optional `request` operand belongs to §5.1's open and §4.8's cursor, which reuse ONE request object
       across several results; neither exists, so there is nothing yet that can pass one and the parameter is
       honestly absent rather than declared and always null. */
    req = rq_new(ctx, source, g_req_class);
    /* §5.6 step 4, and the write §2.8 describes as "this will be set when a request is placed against a
       transaction using the steps to asynchronously execute a request". */
    rq_set(ctx, req, RQ_TRANSACTION, JS_DupValue(ctx, transaction));
    idb_transaction_add_request(ctx, transaction, req);

    /* §5.6 step 5's "run these steps in parallel" ends in "queue a database task". This engine has no second
       thread, and the parallel half is the operation — which is itself preemptible, since it is a step
       closure driven by the same scheduler. So the two are ONE task, which preserves the property the pair
       decides: this request's completion runs after every task already queued against this transaction, so
       §5.6 step 5.1's wait and §5.4 step 2.1's are both discharged by the queue's order. */
    data[RQ_CD_REQUEST] = req;
    data[RQ_CD_OPERATION] = operation;
    fn = JS_NewStepClosure(ctx, g_exec_stepid, 0, 2, data);
    CHECK(!JS_IsException(fn), "IndexedDB: §5.6's request task could not be minted");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
    return req;   /* §5.6's last step: "Return request." */
}

/* ---- §5.5 step 6's per-request database task ------------------------------------------------------------ */

typedef struct {
    JSStepHdr   hdr;
    uint8_t     started;
    uint8_t     phase;
    JSValue     ev;
    EventFireCb cb;
} JSIdbReqAbortState;

static void js_idb_req_abort_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdbReqAbortState *s = st;
    int i;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* The same placement rq_state_start performs, for the same reason. */
static void rq_abort_state_start(JSIdbReqAbortState *s)
{
    int i;

    if (s->started)
        return;
    s->started = 1;
    s->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
}

#define RQA_STAGES(X) \
    X(RQA_SETTLE, "Indexed Database §5.5 steps 6.1-6.3 — ONE O(1) engine action: the request's done flag is " \
                  "set, its result is undefined, and its error is a newly created AbortError DOMException") \
    X(RQA_FIRE,   "Indexed Database §5.5 step 6.4 (fire an event named error at request with its bubbles and " \
                  "cancelable attributes initialized to true)")
enum { RQA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RQA_STEPS[] = { RQA_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_req_abort_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbReqAbortState *s = st;
    JSValueConst req = JS_StepClosureData(&s->hdr, RQ_CD_REQUEST);
    int r;

    DCHECK(idb_request_is(req), "§5.5's per-request abort task was minted over something that is not a request");

    rq_abort_state_start(s);
    STEP_DISPATCH(RQA_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(RQA_SETTLE);
        rq_set(ctx, req, RQ_DONE, JS_TRUE);
        rq_set(ctx, req, RQ_RESULT, JS_UNDEFINED);
        rq_set(ctx, req, RQ_ERROR, rq_dom_exception(ctx, "AbortError",
                                                    "the transaction the request was made against was aborted"));
        STEP_GOTO(s->hdr.stage, RQA_FIRE, &s->phase, NULL);

    STEP_ARM(RQA_FIRE);
        /* THESE ARE NOT §5.10's STEPS, and the difference is the point: §5.10 activates the transaction for
           the dispatch and decides afterwards whether to abort or commit it. Here the transaction is already
           FINISHED — §5.5 step 4 set it before it queued this — so the fire is a plain §2.9 dispatch and
           nothing follows it. Following §5.10 instead would try to make a finished transaction active. */
        if (JS_IsUndefined(s->ev)) {
            s->ev = event_new(ctx, "error", /*bubbles*/ true, /*cancelable*/ true);
            if (JS_IsException(s->ev)) {
                s->ev = JS_UNDEFINED;
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_ABRUPT;
            }
        }
        r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), req, s->ev, JS_UNDEFINED, cb_result, NULL,
                                  out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_req_abort_def = {
    sizeof(JSIdbReqAbortState), js_idb_req_abort_step, NULL, 0, .visit = js_idb_req_abort_visit,
    .algorithm = "Indexed Database §5.5 step 6's per-request database task", .steps = RQA_STEPS
};

void idb_request_abort(JSContext *ctx, JSValueConst request)
{
    JSValue fn;

    DCHECK(idb_request_is(request), "§5.5's abort was given something that is not a request");
    /* "abort the steps to asynchronously execute a request for request, set request's processed flag to
       true": the processed flag IS the abandonment — §5.6's task reads it on entry and terminates, which is
       how "these steps can be aborted at any point" is expressed against a task that may not have run yet. */
    rq_set(ctx, request, RQ_PROCESSED, JS_TRUE);
    fn = JS_NewStepClosure(ctx, g_abort_stepid, 0, 1, &request);
    CHECK(!JS_IsException(fn), "IndexedDB: §5.5's per-request abort task could not be minted");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

/* ---- §4.1's members ------------------------------------------------------------------------------------- */

/* "The result getter steps are: 1. If this's done flag is false, then throw an InvalidStateError DOMException.
   2. Return this's result, or undefined if the request resulted in an error."
   THE THROW IS THE MEMBER'S WHOLE CONTENT and a page depends on it: reading `request.result` synchronously
   after `store.get()` is the mistake this reports, and answering `undefined` instead would be indistinguishable
   from a store that holds nothing. */
static JSValue js_rq_get_result(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!rq_brand(ctx, this_val)) return JS_EXCEPTION;
    if (!rq_flag(ctx, this_val, RQ_DONE))
        return JS_ThrowDOMException(ctx, "InvalidStateError", "the request is still pending");
    return rq_get(ctx, this_val, RQ_RESULT);
}

/* "The error getter steps are: 1. If this's done flag is false, then throw an InvalidStateError DOMException.
   2. Return this's error, or null if no error occurred." */
static JSValue js_rq_get_error(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue e;

    (void)magic;
    if (!rq_brand(ctx, this_val)) return JS_EXCEPTION;
    if (!rq_flag(ctx, this_val, RQ_DONE))
        return JS_ThrowDOMException(ctx, "InvalidStateError", "the request is still pending");
    e = rq_get(ctx, this_val, RQ_ERROR);
    /* `DOMException? error` — a request that succeeded holds `undefined` (§5.6 step 5.6.3.2 writes it), and
       the IDL's nullable type reports that as null. */
    if (JS_IsUndefined(e)) return JS_NULL;
    return e;
}

/* "The source getter steps are to return this's source, or null if no source is set." */
static JSValue js_rq_get_source(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!rq_brand(ctx, this_val)) return JS_EXCEPTION;
    return rq_get(ctx, this_val, RQ_SOURCE);
}

/* "The transaction getter steps are to return this's transaction." */
static JSValue js_rq_get_transaction(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!rq_brand(ctx, this_val)) return JS_EXCEPTION;
    return rq_get(ctx, this_val, RQ_TRANSACTION);
}

/* "The readyState getter steps are to return 'pending' if this's done flag is false, and 'done' otherwise." */
static JSValue js_rq_get_ready_state(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!rq_brand(ctx, this_val)) return JS_EXCEPTION;
    return JS_NewString(ctx, rq_flag(ctx, this_val, RQ_DONE) ? "done" : "pending");
}

static void idb_request_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global, open_proto;

    DCHECK(g_req_class != 0, "a realm asked for IDBRequest.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_req_class);
    DCHECK(JS_IsNull(prev), "idb_request_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBRequest.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBRequest");
    event_target_chain(ctx, proto);   /* `interface IDBRequest : EventTarget` */
    event_target_install_handlers(ctx, proto, EH_IDB_REQUEST);
    idl_install_accessor(ctx, proto, "result", js_rq_get_result, 0, -1);
    idl_install_accessor(ctx, proto, "error", js_rq_get_error, 0, -1);
    idl_install_accessor(ctx, proto, "source", js_rq_get_source, 0, -1);
    idl_install_accessor(ctx, proto, "transaction", js_rq_get_transaction, 0, -1);
    idl_install_accessor(ctx, proto, "readyState", js_rq_get_ready_state, 0, -1);
    JS_SetClassProto(ctx, g_req_class, JS_DupValue(ctx, proto));

    ctor = idl_interface_object(ctx, "IDBRequest", proto);
    CHECK(!JS_IsException(ctor), "the IDBRequest interface object could not be allocated");
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "IDBRequest", ctor);

    /* §4.1's SECOND INTERFACE: `interface IDBOpenDBRequest : IDBRequest`. Its prototype chains to the one just
       built — so `request.result` and `request.readyState` are reached through it rather than copied — and it
       adds exactly the two event handler IDL attributes the standard declares on it and nothing else. */
    open_proto = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(open_proto), "IDBOpenDBRequest.prototype could not be allocated");
    idl_interface_tag(ctx, open_proto, "IDBOpenDBRequest");
    event_target_install_handlers(ctx, open_proto, EH_IDB_OPEN_REQUEST);
    JS_SetClassProto(ctx, g_open_class, JS_DupValue(ctx, open_proto));
    ctor = idl_interface_object(ctx, "IDBOpenDBRequest", open_proto);
    CHECK(!JS_IsException(ctor), "the IDBOpenDBRequest interface object could not be allocated");
    JS_FreeValue(ctx, open_proto);
    JS_SetPropertyStr(ctx, global, "IDBOpenDBRequest", ctor);
    JS_FreeValue(ctx, global);
}

void idb_request_init(JSContext *ctx)
{
    JSClassDef d = { "IDBRequest" }, od = { "IDBOpenDBRequest" };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(!g_ready, "idb_request_init ran twice — one instance is one document is one agent");
    g_key = JS_NewSymbol(ctx, "idbRequestState", false);
    CHECK(!JS_IsException(g_key), "the IDBRequest slot key allocation failed");
    g_req_rt = rt;
    JS_NewClassID(rt, &g_req_class);
    CHECK(JS_NewClass(rt, g_req_class, &d) == 0,
          "IDBRequest: the per-realm prototype slot could not be declared");
    JS_NewClassID(rt, &g_open_class);
    CHECK(JS_NewClass(rt, g_open_class, &od) == 0,
          "IDBOpenDBRequest: the per-realm prototype slot could not be declared");
    g_exec_stepid = JS_RegisterStepDef(rt, &js_idb_req_def);
    g_abort_stepid = JS_RegisterStepDef(rt, &js_idb_req_abort_def);
    g_ready = 1;
    agent_state_flag("idb_request", &g_ready, "the declaration latch");
    agent_state_ptr("idb_request", &g_req_rt, "the runtime §2.8's slot key was minted in");
    agent_state_value("idb_request", &g_key, "§2.8's internal-slot key");
    realm_declare_intrinsic(idb_request_install_realm);
}

void idb_request_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — see idb_transaction_free: the declare pass is unconditional, so an
       undeclared release is a teardown by something that is not core/platform.c's one list. */
    DCHECK(g_ready, "§2.8's request machinery was released in an agent that never declared it");
    DCHECK(rt == g_req_rt, "idb_request_free was given a runtime that is not the one it declared into");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_req_rt = NULL;
    g_ready = 0;
}
