/* INDEXED DATABASE §2.7's TRANSACTION — the state machine, and §4.10's IDBTransaction over it.
 *
 * WHY THIS IS A STATE MACHINE AND NOT A RECORD WITH FLAGS. Every sentence §2.7.1 writes about a transaction is
 * about WHICH OF FOUR STATES it is in: `commit()` is an "InvalidStateError" unless it is ACTIVE, `abort()` is
 * one if it is COMMITTING or FINISHED, a request may be placed only while it is ACTIVE, §5.9's success event
 * ACTIVATES it for the length of one dispatch and DEACTIVATES it afterwards, and §2.7.1's cleanup deactivates
 * it when control returns to the event loop. The four names here are the standard's own — active, inactive,
 * committing, finished — and every transition asserts the lifetime §2.7.1 states, so a caller cannot invent a
 * fifth or walk backwards out of finished.
 *
 * THE DEACTIVATION IS A SCHEDULER FACT, WHICH IS WHY IT IS NOT A BRACKET. §2.7.1's "cleanup Indexed Database
 * transactions" is invoked by HTML at the END of a microtask checkpoint (HTML §8.1.7.3's perform a microtask
 * checkpoint, the step between notifying about rejected promises and ClearKeptObjects), and that is a moment
 * only the scheduler knows: the flow has run its unit of work and holds no microtask. So this component
 * REGISTERS the steps with the one frontier (engine_set_checkpoint_hook) exactly as core/timing/timer.c
 * registers the timer step, rather than wrapping a call site in a save/restore pair that would only be right
 * for whichever caller wrote it. §2.7.1's own note — "the steps are run at most once for each transaction" —
 * is what makes the hook cheap to ask at every step: the list empties on the first run and the second answers
 * false.
 *
 * WHAT A FLOW OWNS HERE. The state is internal slots under a private Symbol on the IDBTransaction object, so
 * every write this file performs is an ordinary property write the per-flow COW delta already captures: flow A
 * can be mid-transaction with two records written while its sibling has none, and a flow parked between a
 * request's success event and the commit resumes with the identical state, request list and error. Both lists
 * this file keeps for the AGENT — the live transactions §2.7.2 asks about and §2.7.1's cleanup set — are JS
 * Arrays for the same reason core/events/message_port.c's queue is one: a malloc'd list captured as head/tail
 * POINTERS reverts the pointers on a context switch and leaves the nodes reachable from nothing.
 *
 * WHAT IS ABSENT AND WHY, stated rather than stubbed. `db` needs §4.4's IDBDatabase (the interface over §2.1.1's
 * connection this transaction holds), `objectStoreNames` needs a DOMStringList and `objectStore()` needs §4.6's
 * object store handle — all three arrive with §4.4-§4.6, and until then they are honestly ABSENT: the IDL gap
 * auditor lists them, and a page reaching one gets the TypeError a missing member gets rather than a shape-only
 * object that would report a store nothing wrote to. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"
#include "solver/engine.h"

/* §2.7's own fields, spelled once so the writers and the readers cannot disagree. */
#define TX_CONNECTION  "connection"   /* §2.7's connection — "all transactions are created through a connection" */
#define TX_SCOPE       "scope"        /* §2.7's scope: an Array of §2.2 object store records */
#define TX_MODE        "mode"
#define TX_DURABILITY  "durability"
#define TX_STATE       "state"        /* §2.7.1's state, one of the four constants */
#define TX_ERROR       "error"        /* §2.7's error, JS_NULL when none */
#define TX_REQUESTS    "requests"     /* §2.7's request list */
#define TX_HEAD        "head"         /* the index of the first request still in that list — see the header */

static JSValue g_key;          /* the private Symbol the slot record hangs off */
static int     g_ready;
static JSClassID g_tx_class;
static JSRuntime *g_tx_rt;

/* §2.7.2 ASKS ABOUT EVERY TRANSACTION THAT IS LIVE, so the set has to exist for it to ask: "a transaction is
   said to be live from when it is created until its state is set to finished". Append-ordered, which is what
   makes "were created before tx" the members already in it. */
static JSValue g_live = JS_UNDEFINED;
/* §2.7.1's CLEANUP SET — the transactions with a cleanup event loop matching this one. One event loop per
   agent here, so membership IS "has a cleanup event loop", and clearing it is removal. */
static JSValue g_cleanup = JS_UNDEFINED;

static int g_id_commit = -1, g_id_abort = -1;
static int g_complete_stepid = -1, g_abort_stepid = -1;

/* ---- the record ---------------------------------------------------------------------------------------- */

static uint32_t tx_array_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    int r;

    DCHECK(!JS_IsException(len), "reading the length of one of this component's own Arrays threw — they are "
                                 "engine-built and have no getters to run");
    r = JS_ToUint32(ctx, &n, len);
    DCHECK(r >= 0, "one of this component's own Arrays had a length that is not a number");
    (void)r;
    JS_FreeValue(ctx, len);
    return n;
}

/* The transaction's internal slots, or UNDEFINED for a value that is not one. Read as an OWN SLOT, never a
   lookup: a miss on a lookup is the solver's absent-state seam and would mint a concolic for an internal slot.
   OWNED. */
static JSValue tx_slots(JSContext *ctx, JSValueConst tx)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an IDBTransaction slot was asked for before idb_transaction_init made the key");
    if (!JS_IsObject(tx))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBTransaction slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &st, tx, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

bool idb_transaction_is(JSValueConst v)
{
    return g_tx_class != 0 && JS_GetClassID(v) == g_tx_class;
}

/* WEB IDL §3.7.5's BRAND CHECK for a member — `IDBTransaction.prototype.abort.call({})` is a TypeError, and a
   page tells that apart from the "InvalidStateError" the algorithm reports. */
static bool tx_brand(JSContext *ctx, JSValueConst this_val)
{
    if (idb_transaction_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "an IDBTransaction member was reached on something that is not an IDBTransaction");
    return false;
}

static int32_t tx_int(JSContext *ctx, JSValueConst tx, const char *field)
{
    JSValue slots = tx_slots(ctx, tx), v;
    int32_t out = -1;
    int r;

    DCHECK(JS_IsObject(slots), "an IDBTransaction field was read off a value carrying no slot record");
    v = JS_GetPropertyStr(ctx, slots, field);
    r = JS_ToInt32(ctx, &out, v);
    DCHECK(r >= 0, "an IDBTransaction's own numeric field is not a number — only this file writes them");
    (void)r;
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, slots);
    return out;
}

static void tx_set_int(JSContext *ctx, JSValueConst tx, const char *field, int32_t v)
{
    JSValue slots = tx_slots(ctx, tx);

    DCHECK(JS_IsObject(slots), "an IDBTransaction field was written on a value carrying no slot record");
    JS_SetPropertyStr(ctx, slots, field, JS_NewInt32(ctx, v));
    JS_FreeValue(ctx, slots);
}

int idb_transaction_state(JSContext *ctx, JSValueConst tx)
{
    return tx_int(ctx, tx, TX_STATE);
}

/* §2.7.1's LIFETIME, as the only door onto the state. Every transition the standard describes is here and
   nothing else is: the assert is what stops a caller inventing one, and it fires at the caller that wrote the
   wrong transition rather than three algorithms later at whoever read the impossible state. */
void idb_transaction_set_state(JSContext *ctx, JSValueConst tx, int state)
{
    int now = idb_transaction_state(ctx, tx);

    DCHECK(state >= IDB_TX_ACTIVE && state <= IDB_TX_FINISHED,
           "an IDBTransaction was moved to a state §2.7.1 does not name — the four are active, inactive, "
           "committing and finished");
    DCHECK(now != IDB_TX_FINISHED,
           "an IDBTransaction that is FINISHED was moved to another state. §2.7.1: \"Once a transaction has "
           "committed or aborted, it enters this state\" — it is the last one, §5.5's abort returns "
           "immediately for a transaction already in it, and every member that could move it refuses first");
    DCHECK(!(state == IDB_TX_ACTIVE && now == IDB_TX_COMMITTING),
           "an IDBTransaction that is COMMITTING was made ACTIVE again. §5.9 step 7 activates only an INACTIVE "
           "transaction, which is the whole of why a success event fired while a commit is in flight cannot "
           "let the page place another request against it");
    tx_set_int(ctx, tx, TX_STATE, state);
    if (state == IDB_TX_FINISHED) {
        /* "A transaction is said to be live from when it is created until its state is set to finished." The
           removal is HERE rather than at the two callers so a transaction cannot be finished by one path and
           stay live because the other path was the one that remembered. */
        uint32_t i, n = tx_array_len(ctx, g_live);

        for (i = 0; i < n; i++) {
            JSValue m = JS_GetPropertyUint32(ctx, g_live, i);
            bool same = JS_VALUE_GET_PTR(m) == JS_VALUE_GET_PTR(tx);

            JS_FreeValue(ctx, m);
            if (!same) continue;
            for (; i + 1 < n; i++) {
                JSValue next = JS_GetPropertyUint32(ctx, g_live, i + 1);
                JS_DefinePropertyValueUint32(ctx, g_live, i, next, JS_PROP_C_W_E);
            }
            JS_SetPropertyStr(ctx, g_live, "length", JS_NewUint32(ctx, n - 1));
            return;
        }
        DFAIL("a transaction reached the FINISHED state without being live — every transaction is added to "
              "the live set by idb_transaction_new and removed only here, so this one was either finished "
              "twice or created by something other than §2.7's creation");
    }
}

/* ---- §2.7's REQUEST LIST -------------------------------------------------------------------------------- */

void idb_transaction_add_request(JSContext *ctx, JSValueConst tx, JSValueConst request)
{
    JSValue slots = tx_slots(ctx, tx), list;

    DCHECK(JS_IsObject(slots), "a request was added to a value that is not a transaction");
    list = JS_GetPropertyStr(ctx, slots, TX_REQUESTS);
    DCHECK(JS_IsArray(list), "a transaction carried no request list — §2.7 gives every one a list and only "
                             "idb_transaction_new builds them");
    /* §5.6 step 4: "Add request to the end of transaction's request list." A DEFINE and not an assignment, for
       core/idl_slots.h's sibling rule: a page that put an index accessor on Array.prototype would otherwise
       swallow the write and leave no own property behind. */
    JS_DefinePropertyValueUint32(ctx, list, tx_array_len(ctx, list), JS_DupValue(ctx, request), JS_PROP_C_W_E);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, slots);
}

void idb_transaction_remove_request(JSContext *ctx, JSValueConst tx, JSValueConst request)
{
    JSValue slots = tx_slots(ctx, tx), list, head_at;
    uint32_t head;

    DCHECK(JS_IsObject(slots), "a request was removed from a value that is not a transaction");
    head = (uint32_t)tx_int(ctx, tx, TX_HEAD);
    list = JS_GetPropertyStr(ctx, slots, TX_REQUESTS);
    DCHECK(JS_IsArray(list), "a transaction carried no request list");
    DCHECK(head < tx_array_len(ctx, list),
           "a request was removed from a transaction whose request list is already empty — §5.6 step 5.7.1 "
           "removes each request exactly once, from the task that completes it");
    head_at = JS_GetPropertyUint32(ctx, list, head);
    DCHECK(JS_VALUE_GET_PTR(head_at) == JS_VALUE_GET_PTR(request),
           "a request was removed from a transaction that is not the FIRST one still in its list. §5.6 step "
           "5.1 makes each request wait until it is the first item that is not processed, so removals are in "
           "the order the additions were — a removal from the middle means two requests of one transaction "
           "completed out of order, which is the ordering §2.7.1 exists to guarantee");
    JS_FreeValue(ctx, head_at);
    /* THE SLOT IS CLEARED AS WELL AS SKIPPED. The cursor alone would leave the request reachable from the
       Array for the life of the transaction, and a request holds its result — which for a `get` is the whole
       value the store answered with. */
    JS_DefinePropertyValueUint32(ctx, list, head, JS_UNDEFINED, JS_PROP_C_W_E);
    tx_set_int(ctx, tx, TX_HEAD, (int32_t)(head + 1));
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, slots);
}

bool idb_transaction_requests_empty(JSContext *ctx, JSValueConst tx)
{
    JSValue slots = tx_slots(ctx, tx), list;
    uint32_t head = (uint32_t)tx_int(ctx, tx, TX_HEAD);
    bool empty;

    DCHECK(JS_IsObject(slots), "a request list was asked of a value that is not a transaction");
    list = JS_GetPropertyStr(ctx, slots, TX_REQUESTS);
    DCHECK(JS_IsArray(list), "a transaction carried no request list");
    empty = head >= tx_array_len(ctx, list);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, slots);
    return empty;
}

/* ---- §5.4's COMMIT: the database task that finishes the transaction and fires `complete` ------------------ */

/* The state both of this file's task machines carry: the event in flight and §2.9's dispatch request buffer.
   Nothing else — the target is the closure's captured transaction, read off the header. */
typedef struct {
    JSStepHdr   hdr;      /* FIRST: the driver writes the def and the operand bounds through it */
    uint8_t     started;  /* see tx_fire_start */
    uint8_t     phase;    /* the dispatch request's own cursor */
    JSValue     ev;       /* the event being dispatched (owned) */
    EventFireCb cb;       /* the dispatch request's operand buffer */
} JSIdbTxFireState;

static void js_idb_tx_fire_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdbTxFireState *s = st;
    int i;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* A STEP STATE IS js_mallocz'd, AND A ZEROED JSValue IS THE INTEGER 0 — not JS_UNDEFINED, because
   JS_TAG_INT is 0. So every value slot is placed here before anything can read one, and `started` is the byte
   that says whether it has been: quickjs-step.h's own note is that a machine can never answer "have I
   started?" from its stage, since the first stage's constant is also the entry stage — and the abort task
   below has exactly ONE stage, so its entry and its resume are the same arm. It runs ABOVE the dispatch, which
   is where code that legitimately runs on every entry belongs. */
static void tx_fire_start(JSIdbTxFireState *s)
{
    int i;

    if (s->started)
        return;
    s->started = 1;
    s->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
}

/* The transaction this task was queued for — captured by the closure, because a task's operands travel WITH
   it: §scheduler's rule that an operation which becomes a work item takes its inputs with it, and a
   transaction read back off anything else at task time would be whichever one happened to be current. */
#define IDB_TXTASK_TX 0

static JSValueConst tx_fire_target(const JSStepHdr *hdr)
{
    JSValueConst tx = JS_StepClosureData(hdr, IDB_TXTASK_TX);

    DCHECK(idb_transaction_is(tx), "an Indexed Database transaction task was minted over something that is "
                                   "not a transaction");
    return tx;
}

#define TXC_STAGES(X) \
    X(TXC_FINISH, "Indexed Database §5.4 step 2.5.2 (set transaction's state to finished)") \
    X(TXC_FIRE,   "Indexed Database §5.4 step 2.5.3 (fire an event named complete at transaction)")
enum { TXC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TXC_STEPS[] = { TXC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_tx_complete_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbTxFireState *s = st;
    JSValueConst tx = tx_fire_target(&s->hdr);
    int r;

    tx_fire_start(s);
    STEP_DISPATCH(TXC_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(TXC_FINISH);
        /* §5.4 step 2.2: "If transaction's state is no longer committing, then terminate these steps." An
           abort that arrived between the commit and this task is exactly that, and it has already fired its
           own `abort` event — so the commit says nothing. */
        if (idb_transaction_state(ctx, tx) != IDB_TX_COMMITTING) {
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;
        }
        /* §5.4 step 2.1 ("wait until every item in transaction's request list is processed") is DISCHARGED BY
           THE QUEUE rather than waited on: §5.6 performs its operation and sets the processed flag inside the
           task it was queued as, so every request queued before this task has been processed by the time this
           one runs. The assert is that statement, checked rather than believed. */
        DCHECK(idb_transaction_requests_empty(ctx, tx),
               "§5.4's commit task ran while the transaction still holds requests. Step 2.1 waits until every "
               "item in the request list is processed, and this engine discharges that wait through the task "
               "queue — so an outstanding request here means a request was queued AFTER the commit, which "
               "§2.7.1 forbids for a committing transaction");
        /* §5.4 step 2.3's "attempt to write any outstanding changes to the database, considering the
           durability hint" has nothing to do: the store IS the heap (core/indexeddb/idb_database.c), §6.1
           wrote the record when it ran, and there is no medium behind it for a durability hint to reach. That
           is a fact about this store rather than a step skipped — which is also why step 2.4's write-error arm
           is unreachable and is not written. */
        idb_transaction_set_state(ctx, tx, IDB_TX_FINISHED);
        STEP_GOTO(s->hdr.stage, TXC_FIRE, &s->phase, NULL);

    STEP_ARM(TXC_FIRE);
        /* "Fire an event named complete at transaction" — bubbles and cancelable both false, which is DOM
           §2.4's default and what §5.4 leaves unstated. A REQUEST, so the page's listeners run as ordinary
           preemptible code and this task resumes once every one of them has returned. */
        if (JS_IsUndefined(s->ev)) {
            s->ev = event_new(ctx, "complete", /*bubbles*/ false, /*cancelable*/ false);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
        }
        r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), tx, s->ev, JS_UNDEFINED, cb_result, NULL,
                                  out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_tx_complete_def = {
    sizeof(JSIdbTxFireState), js_idb_tx_complete_step, NULL, 0, .visit = js_idb_tx_fire_visit,
    .algorithm = "Indexed Database §5.4's commit-a-transaction database task", .steps = TXC_STEPS
};

void idb_transaction_commit(JSContext *ctx, JSValueConst tx)
{
    JSValue fn;

    /* §5.4 step 1: "Set transaction's state to committing." */
    idb_transaction_set_state(ctx, tx, IDB_TX_COMMITTING);
    /* §5.4 step 2's "run the following steps in parallel" ends in "queue a database task". This engine has no
       second thread and the write in between has nothing to do (see the task's own comment), so the parallel
       half IS the task — one queue hop rather than two, which preserves the one thing the pair decides: that
       the completion runs after every request task already queued against this transaction. */
    fn = JS_NewStepClosure(ctx, g_complete_stepid, 0, 1, &tx);
    CHECK(!JS_IsException(fn), "IndexedDB: §5.4's commit task could not be minted");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

/* ---- §5.5's ABORT: the database task that fires `abort` -------------------------------------------------- */

#define TXA_STAGES(X) \
    X(TXA_FIRE, "Indexed Database §5.5 step 7.2 (fire an event named abort at transaction)")
enum { TXA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TXA_STEPS[] = { TXA_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_tx_abort_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbTxFireState *s = st;
    JSValueConst tx = tx_fire_target(&s->hdr);
    int r;

    tx_fire_start(s);
    STEP_DISPATCH(TXA_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(TXA_FIRE);
        /* §5.5 step 7.1 and 7.3 are the upgrade-transaction arms, and §2.7.3's upgrade transaction arrives
           with §5.7's upgrade-a-database — there is no way to create one yet, which is why the arms are
           absent rather than written against a state nothing can produce.
           "with its bubbles attribute initialized to true": the event travels to the connection, which is what
           §2.7's get-the-parent algorithm answers with. */
        if (JS_IsUndefined(s->ev)) {
            s->ev = event_new(ctx, "abort", /*bubbles*/ true, /*cancelable*/ false);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
        }
        r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), tx, s->ev, JS_UNDEFINED, cb_result, NULL,
                                  out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_tx_abort_def = {
    sizeof(JSIdbTxFireState), js_idb_tx_abort_step, NULL, 0, .visit = js_idb_tx_fire_visit,
    .algorithm = "Indexed Database §5.5's abort-a-transaction database task", .steps = TXA_STEPS
};

void idb_transaction_abort(JSContext *ctx, JSValueConst tx, JSValue error)
{
    JSValue slots, list, fn;
    uint32_t i, head, n;

    /* §5.5 step 1: "If transaction's state is finished, abort these steps." */
    if (idb_transaction_state(ctx, tx) == IDB_TX_FINISHED) {
        JS_FreeValue(ctx, error);
        return;
    }
    /* §5.5 step 2: "All the changes made to the database by the transaction are reverted." A READ-ONLY
       transaction has made none, so there is nothing to revert and that is a fact about the mode rather than a
       step skipped. Any other mode needs the undo, and it does not exist. */
    if (tx_int(ctx, tx, TX_MODE) != IDB_TX_READONLY) {
        DFAIL("Indexed Database §5.5 step 2's REVERT is not built. A transaction that can write needs the "
              "changes it made to be undone when it aborts — for a \"readwrite\" transaction that is every "
              "record §6.1 stored and every record §6.4 deleted through it, and for an upgrade transaction it "
              "is additionally the object stores and indexes created or removed and the version change "
              "itself. The mechanism is the per-flow COW delta this engine already has (solver/cow.h): a "
              "transaction's writes are a SPAN of one flow's delta, so the undo is unapplying that span "
              "rather than a second journal — build it where the transaction records where its span began, "
              "and delete this crash");
    }
    /* §5.5 step 3 is the upgrade-transaction arm; §2.7.3's upgrade transaction arrives with §5.7. */
    /* §5.5 steps 4 and 5. */
    idb_transaction_set_state(ctx, tx, IDB_TX_FINISHED);
    slots = tx_slots(ctx, tx);
    DCHECK(JS_IsObject(slots), "a transaction was aborted that carries no slot record");
    JS_SetPropertyStr(ctx, slots, TX_ERROR, error);

    /* §5.5 step 6: "For each request of transaction's request list, abort the steps to asynchronously execute
       a request for request, set request's processed flag to true, and queue a database task to [fail it with
       an AbortError]." The whole of that is the request's own, so it is asked of the request rather than
       reached into from here. */
    list = JS_GetPropertyStr(ctx, slots, TX_REQUESTS);
    DCHECK(JS_IsArray(list), "a transaction carried no request list");
    head = (uint32_t)tx_int(ctx, tx, TX_HEAD);
    n = tx_array_len(ctx, list);
    for (i = head; i < n; i++) {
        JSValue req = JS_GetPropertyUint32(ctx, list, i);

        DCHECK(JS_IsObject(req), "a transaction's request list had a hole in it — the entries below the head "
                                 "are cleared and the ones above it are requests");
        idb_request_abort(ctx, req);
        JS_FreeValue(ctx, req);
    }
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, slots);

    /* §5.5 step 7's database task. */
    fn = JS_NewStepClosure(ctx, g_abort_stepid, 0, 1, &tx);
    CHECK(!JS_IsException(fn), "IndexedDB: §5.5's abort task could not be minted");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

/* ---- §2.7.1's CLEANUP, run at the end of a microtask checkpoint ------------------------------------------ */

void idb_transaction_set_cleanup_loop(JSContext *ctx, JSValueConst tx)
{
    DCHECK(idb_transaction_is(tx), "a cleanup event loop was given to something that is not a transaction");
    DCHECK(!JS_IsUndefined(g_cleanup), "§2.7.1's cleanup set was written before idb_transaction_init built it");
    JS_DefinePropertyValueUint32(ctx, g_cleanup, tx_array_len(ctx, g_cleanup), JS_DupValue(ctx, tx),
                                 JS_PROP_C_W_E);
}

/* "To cleanup Indexed Database transactions, run the following steps. They will return true if any
   transactions were cleaned up, or false otherwise."
   THERE IS ONE EVENT LOOP PER AGENT HERE, so "with cleanup event loop matching the current event loop" is
   membership of this set, and step 2.2's "clear transaction's cleanup event loop" is removal from it. The
   whole set is taken and emptied BEFORE the first deactivation, which is the same reason DOM §3.2's abort
   snapshots its algorithm list: a deactivation is an ordinary property write and nothing this file does can
   add to the set from inside the walk, but a walk over the live Array would be the shape that breaks the day
   something can. */
static void idb_transaction_cleanup(JSContext *ctx)
{
    JSValue taken;
    uint32_t i, n;

    DCHECK(!JS_IsUndefined(g_cleanup), "§2.7.1's cleanup ran before idb_transaction_init built its set");
    n = tx_array_len(ctx, g_cleanup);
    if (n == 0)
        return;   /* "If there are no transactions ... return false." */
    /* THE SET IS COPIED OUT AND THEN TRUNCATED IN PLACE — never swapped for a fresh Array. `g_cleanup` is a C
       STATIC, so assigning a new Array to it is a write no per-flow COW delta can see: this flow's siblings
       would go on holding entries in the Array the static no longer names, and their transactions would never
       be deactivated with nothing to say so. Emptying it is a `length` write, which is an ordinary property
       write the delta captures like any other. */
    taken = JS_NewArray(ctx);
    CHECK(!JS_IsException(taken), "IndexedDB: §2.7.1's cleanup walk could not take its snapshot");
    for (i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, taken, i, JS_GetPropertyUint32(ctx, g_cleanup, i), JS_PROP_C_W_E);
    JS_SetPropertyStr(ctx, g_cleanup, "length", JS_NewInt32(ctx, 0));
    for (i = 0; i < n; i++) {
        JSValue tx = JS_GetPropertyUint32(ctx, taken, i);

        DCHECK(idb_transaction_is(tx), "§2.7.1's cleanup set held something that is not a transaction");
        /* "Set transaction's state to inactive." A transaction that has already COMMITTED or ABORTED in this
           same turn is FINISHED, and §2.7.1's state is the last one — the deactivation is about a transaction
           the script left ACTIVE, which is the case the note describes ("transactions created by a script call
           to transaction() are deactivated once the task that invoked the script has completed"). */
        if (idb_transaction_state(ctx, tx) == IDB_TX_ACTIVE) {
            idb_transaction_set_state(ctx, tx, IDB_TX_INACTIVE);
            /* §2.7.1's lifetime: "The implementation must attempt to commit an inactive transaction when all
               requests placed against the transaction have completed and their returned results handled, no
               new requests have been placed against the transaction, and the transaction has not been
               aborted." A transaction deactivated with an EMPTY request list is exactly that state — nothing
               will ever run against it again — so this is where the automatic commit is due. A transaction
               with outstanding requests commits from §5.9 step 9.3 instead, once the last one's success event
               has been handled. */
            if (idb_transaction_requests_empty(ctx, tx))
                idb_transaction_commit(ctx, tx);
        }
        JS_FreeValue(ctx, tx);
    }
    JS_FreeValue(ctx, taken);
}

/* ---- §2.7.2's SCHEDULING CONSTRAINT ---------------------------------------------------------------------- */

/* "Two transactions have overlapping scope if any object store is in both transactions' scope." The stores are
   compared by IDENTITY because §2.2 gives a database ONE record per name (idb_database.c's set of object
   stores answers with it), so two scopes naming one store hold the same object. */
static bool tx_scopes_overlap(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    uint32_t i, j, na = tx_array_len(ctx, a), nb = tx_array_len(ctx, b);

    for (i = 0; i < na; i++) {
        JSValue sa = JS_GetPropertyUint32(ctx, a, i);

        for (j = 0; j < nb; j++) {
            JSValue sb = JS_GetPropertyUint32(ctx, b, j);
            bool same = JS_VALUE_GET_PTR(sa) == JS_VALUE_GET_PTR(sb);

            JS_FreeValue(ctx, sb);
            if (same) { JS_FreeValue(ctx, sa); return true; }
        }
        JS_FreeValue(ctx, sa);
    }
    return false;
}

/* §2.7.2's two constraints, asked of the transaction that has just been created. Anything already in the live
   set was created before it, so "were created before tx" is membership. */
static void tx_check_can_start(JSContext *ctx, JSValueConst scope, int mode)
{
    uint32_t i, n = tx_array_len(ctx, g_live);

    for (i = 0; i < n; i++) {
        JSValue other = JS_GetPropertyUint32(ctx, g_live, i);
        JSValue oslots = tx_slots(ctx, other);
        JSValue oscope;
        int omode;
        bool blocks;

        DCHECK(JS_IsObject(oslots), "§2.7.2's live set held something that is not a transaction");
        oscope = JS_GetPropertyStr(ctx, oslots, TX_SCOPE);
        omode = tx_int(ctx, other, TX_MODE);
        /* A read-only transaction is blocked only by an earlier READ/WRITE one; a read/write transaction is
           blocked by ANY earlier one. Both only when the scopes overlap. */
        blocks = (mode == IDB_TX_READONLY ? omode != IDB_TX_READONLY : true) &&
                 tx_scopes_overlap(ctx, scope, oscope);
        JS_FreeValue(ctx, oscope);
        JS_FreeValue(ctx, oslots);
        JS_FreeValue(ctx, other);
        if (!blocks)
            continue;
        DFAIL("Indexed Database §2.7.2's TRANSACTION START QUEUE is not built. A transaction was created that "
              "cannot start yet — an earlier live transaction has an overlapping scope and the modes forbid "
              "them running together — and §2.7.1 says the implementation must \"keep track of the requests "
              "and their order\" and not execute them until the transaction is started. So the missing "
              "mechanism is the DELAYED START: a created transaction is live and accepts requests "
              "immediately, and its queued operations may not run until every earlier overlapping "
              "transaction is finished. Build it as a per-transaction gate the request task asks before it "
              "performs its operation — never as a bound on how many transactions may exist");
    }
}

/* ---- creation ------------------------------------------------------------------------------------------- */

JSValue idb_transaction_new(JSContext *ctx, JSValueConst connection, JSValue scope, int mode, int durability)
{
    JSValue tx, st, proto, requests;
    JSAtom k;

    DCHECK(g_ready, "an IDBTransaction was created before idb_transaction_init declared the interface");
    DCHECK(JS_IsObject(connection), "a transaction was created with no connection. §2.7: \"All transactions "
                                    "are created through a connection, which is the transaction's "
                                    "connection\" — it is what §2.7's get-the-parent algorithm answers with, "
                                    "so a transaction without one has no event path above itself");
    DCHECK(JS_IsArray(scope), "a transaction was created with a scope that is not a list of object stores");
    DCHECK(mode >= IDB_TX_READONLY && mode <= IDB_TX_VERSIONCHANGE, "a transaction was created in a mode "
                                                                    "IDBTransactionMode does not name");
    DCHECK(durability >= IDB_DUR_DEFAULT && durability <= IDB_DUR_RELAXED,
           "a transaction was created with a durability hint IDBTransactionDurability does not name");
    DCHECK(tx_array_len(ctx, scope) > 0, "a transaction was created with an EMPTY scope — §4.9 step 5 reports "
                                         "that as an \"InvalidAccessError\" before this algorithm is reached");

    proto = JS_GetClassProto(ctx, g_tx_class);
    DCHECK(!JS_IsNull(proto), "IDBTransaction.prototype was asked for in a realm that never ran its install");
    tx = JS_NewObjectProtoClass(ctx, proto, g_tx_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(tx), "IndexedDB: the IDBTransaction allocation failed");
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "IndexedDB: the IDBTransaction slot record allocation failed");
    requests = JS_NewArray(ctx);
    CHECK(!JS_IsException(requests), "IndexedDB: a transaction's request list could not be allocated");
    JS_SetPropertyStr(ctx, st, TX_CONNECTION, JS_DupValue(ctx, connection));
    JS_SetPropertyStr(ctx, st, TX_SCOPE, JS_DupValue(ctx, scope));
    JS_SetPropertyStr(ctx, st, TX_MODE, JS_NewInt32(ctx, mode));
    JS_SetPropertyStr(ctx, st, TX_DURABILITY, JS_NewInt32(ctx, durability));
    /* §2.7.1: "When a transaction is created its state is initially active." */
    JS_SetPropertyStr(ctx, st, TX_STATE, JS_NewInt32(ctx, IDB_TX_ACTIVE));
    JS_SetPropertyStr(ctx, st, TX_ERROR, JS_NULL);
    JS_SetPropertyStr(ctx, st, TX_REQUESTS, requests);
    JS_SetPropertyStr(ctx, st, TX_HEAD, JS_NewInt32(ctx, 0));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBTransaction slot key could not be interned");
    JS_SetProperty(ctx, tx, k, st);
    JS_FreeAtom(ctx, k);

    tx_check_can_start(ctx, scope, mode);
    /* "A transaction is said to be live from when it is created": the set §2.7.2 asks about is joined here and
       left in idb_transaction_set_state, so the two edges of `live` are the two edges of the sentence. */
    JS_DefinePropertyValueUint32(ctx, g_live, tx_array_len(ctx, g_live), JS_DupValue(ctx, tx), JS_PROP_C_W_E);
    JS_FreeValue(ctx, scope);
    return tx;
}

/* ---- §4.10's members ------------------------------------------------------------------------------------ */

/* "The mode getter steps are to return this's mode." The strings are IDBTransactionMode's own identifiers, in
   the order the enum declares them, so the constant and the name cannot drift. */
static const char *const TX_MODE_NAME[] = { "readonly", "readwrite", "versionchange" };
static const char *const TX_DUR_NAME[]  = { "default", "strict", "relaxed" };

static JSValue js_tx_get_mode(JSContext *ctx, JSValueConst this_val, int magic)
{
    int m;

    (void)magic;
    if (!tx_brand(ctx, this_val)) return JS_EXCEPTION;
    m = tx_int(ctx, this_val, TX_MODE);
    DCHECK(m >= IDB_TX_READONLY && m <= IDB_TX_VERSIONCHANGE, "a transaction carried a mode not in §2.7's set");
    return JS_NewString(ctx, TX_MODE_NAME[m]);
}

/* "The durability getter steps are to return this's durability hint." */
static JSValue js_tx_get_durability(JSContext *ctx, JSValueConst this_val, int magic)
{
    int d;

    (void)magic;
    if (!tx_brand(ctx, this_val)) return JS_EXCEPTION;
    d = tx_int(ctx, this_val, TX_DURABILITY);
    DCHECK(d >= IDB_DUR_DEFAULT && d <= IDB_DUR_RELAXED, "a transaction carried a durability hint not in §2.7's set");
    return JS_NewString(ctx, TX_DUR_NAME[d]);
}

/* "The error getter steps are to return this's error, or null if none." */
static JSValue js_tx_get_error(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots, err;

    (void)magic;
    if (!tx_brand(ctx, this_val)) return JS_EXCEPTION;
    slots = tx_slots(ctx, this_val);
    DCHECK(JS_IsObject(slots), "an IDBTransaction carried no slot record");
    err = JS_GetPropertyStr(ctx, slots, TX_ERROR);
    JS_FreeValue(ctx, slots);
    return err;
}

/* "The commit() method steps are: 1. If this's state is not active, then throw an InvalidStateError
   DOMException. 2. Run commit a transaction with this." */
static JSValue js_tx_commit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    if (!tx_brand(ctx, this_val)) return JS_EXCEPTION;
    if (idb_transaction_state(ctx, this_val) != IDB_TX_ACTIVE)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "the transaction is not active, so it cannot be committed");
    idb_transaction_commit(ctx, this_val);
    return JS_UNDEFINED;
}

/* "The abort() method steps are: 1. If this's state is committing or finished, then throw an
   InvalidStateError DOMException. 2. Run abort a transaction with this and null."
   THE NULL IS THE ERROR, and §2.7's own note is why it cannot be an absent one: "implementors need to keep in
   mind that the value null is considered an error, as it is set from abort()". */
static JSValue js_tx_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    int state;

    (void)argc; (void)argv; (void)magic;
    if (!tx_brand(ctx, this_val)) return JS_EXCEPTION;
    state = idb_transaction_state(ctx, this_val);
    if (state == IDB_TX_COMMITTING || state == IDB_TX_FINISHED)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "the transaction has already been committed or aborted");
    idb_transaction_abort(ctx, this_val, JS_NULL);
    return JS_UNDEFINED;
}

/* §3.7's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM — per realm because a C member runs in the realm that
   DEFINED it, so one prototype shared by two documents would answer both from whichever built it. */
static void idb_transaction_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_tx_class != 0, "a realm asked for IDBTransaction.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_tx_class);
    DCHECK(JS_IsNull(prev), "idb_transaction_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBTransaction.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBTransaction");
    /* `interface IDBTransaction : EventTarget` — addEventListener and the three handler attributes are
       reached through the chain rather than copied onto each transaction. */
    event_target_chain(ctx, proto);
    event_target_install_handlers(ctx, proto, EH_IDB_TRANSACTION);
    idl_install_accessor(ctx, proto, "mode", js_tx_get_mode, 0, -1);
    idl_install_accessor(ctx, proto, "durability", js_tx_get_durability, 0, -1);
    idl_install_accessor(ctx, proto, "error", js_tx_get_error, 0, -1);
    idl_install_method(ctx, proto, "commit", 0, g_id_commit);
    idl_install_method(ctx, proto, "abort", 0, g_id_abort);
    JS_SetClassProto(ctx, g_tx_class, JS_DupValue(ctx, proto));

    ctor = idl_interface_object(ctx, "IDBTransaction", proto);
    CHECK(!JS_IsException(ctor), "the IDBTransaction interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "IDBTransaction", ctor);
    JS_FreeValue(ctx, global);
}

void idb_transaction_init(JSContext *ctx)
{
    JSClassDef d = { "IDBTransaction" };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(!g_ready, "idb_transaction_init ran twice — one instance is one document is one agent");
    g_key = JS_NewSymbol(ctx, "idbTransactionState", false);
    CHECK(!JS_IsException(g_key), "the IDBTransaction slot key allocation failed");
    g_tx_rt = rt;
    JS_NewClassID(rt, &g_tx_class);
    CHECK(JS_NewClass(rt, g_tx_class, &d) == 0,
          "IDBTransaction: the per-realm prototype slot could not be declared");
    g_live = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_live), "IndexedDB: §2.7.2's live transaction set could not be allocated");
    g_cleanup = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_cleanup), "IndexedDB: §2.7.1's cleanup set could not be allocated");
    g_id_commit = idl_method_id(ctx, NULL, 0, js_tx_commit, 0);
    g_id_abort = idl_method_id(ctx, NULL, 0, js_tx_abort, 0);
    g_complete_stepid = JS_RegisterStepDef(rt, &js_idb_tx_complete_def);
    g_abort_stepid = JS_RegisterStepDef(rt, &js_idb_tx_abort_def);
    /* §2.7.1's cleanup, registered with the ONE frontier — see the file comment for why it is a hook and not a
       bracket around whoever created the transaction. */
    engine_set_checkpoint_hook(idb_transaction_cleanup);
    g_ready = 1;
    realm_declare_intrinsic(idb_transaction_install_realm);
}

void idb_transaction_free(JSRuntime *rt)
{
    if (!g_ready)
        return;
    DCHECK(rt == g_tx_rt, "idb_transaction_free was given a runtime that is not the one it declared into");
    JS_FreeValueRT(rt, g_key);
    JS_FreeValueRT(rt, g_live);
    JS_FreeValueRT(rt, g_cleanup);
    g_key = JS_UNDEFINED;
    g_live = JS_UNDEFINED;
    g_cleanup = JS_UNDEFINED;
    g_ready = 0;
}
