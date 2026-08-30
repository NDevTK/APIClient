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
 * §2.7's THIRD list is the one this file gained for §2.7.2 Transaction scheduling — the request tasks held for a
 * transaction that has not started yet — and it is PER TRANSACTION rather than per agent, on the slot record
 * beside the request list, because whose requests they are is the transaction's fact and because it then rides
 * the flow's COW delta like everything else here: one arm of a fork can be holding two puts behind an earlier
 * read/write transaction while its sibling, whose earlier transaction aborted, has already released them.
 *
 * WHAT IS ABSENT AND WHY, stated rather than stubbed. `objectStoreNames` is the one member of §4.10 this file
 * does not build, and it is blocked on a whole interface rather than on anything here: §4.10's getter ends in
 * "return the result (a DOMStringList) of creating a sorted name list with names", and DOMStringList does not
 * exist in this engine. It is honestly ABSENT — the IDL gap auditor lists it, and a page reaching it gets the
 * TypeError a missing member gets rather than an Array pretending to be a DOMStringList, which would answer
 * `contains()` with undefined. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_name_list.h"
#include "core/indexeddb/idb_object_store.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/indexeddb/idb_upgrade_abort.h"
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
#define TX_REQUEST     "request"      /* §2.7.3's open request, for an upgrade transaction; JS_NULL otherwise */
#define TX_HANDLES     "handles"      /* §2.2.1's one-handle-per-store set, keyed by the STORE — see below */
#define TX_CHANGES     "changes"      /* §5.5 step 2's changes made to the database — see the header */
#define TX_START       "start"        /* §2.7.1 Transaction lifecycle's `started`, as the three moments below */
#define TX_HELD        "held"         /* §2.7 Transactions' requests kept track of until this one starts */
#define TX_COMMIT      "commit"       /* §5.4 Committing a transaction step 2's in-parallel block — see below */

/* §2.7.1 TRANSACTION LIFECYCLE's START, as the THREE moments its own sentence has: a transaction is created
   UNSTARTED; "when an implementation is able to enforce the constraints for the transaction's scope and mode,
   defined below, the implementation must queue a database task to start the transaction asynchronously" — that
   queued task is the middle moment, and it exists because a second finish must not queue a second start; and
   "once the transaction has been started the implementation can begin executing the requests placed against the
   transaction". §5.7 Upgrading a database's step 6 starts its upgrade transaction DIRECTLY and never passes
   through the middle one. */
enum { TX_START_NO = 0, TX_START_QUEUED, TX_START_YES };

/* §5.4 COMMITTING A TRANSACTION step 2's IN-PARALLEL BLOCK, as the two moments it has here. Its FIRST step is a
   WAIT — step 2.1, "wait until every item in transaction's request list is processed" — and its LAST is step
   2.5's "queue a database task". Between them this engine has nothing to do (see the task's own comment on step
   2.3), so the block IS that wait followed by that queueing, and the two moments are "the wait is outstanding"
   and "the task has been queued".
   THE WAIT CANNOT BE DISCHARGED BY QUEUE ORDER, WHICH IS WHAT THIS ENUM EXISTS TO SAY. That was the standing
   design and it holds for §5.9 step 8.3 and §5.10 step 8.4, which run commit a transaction only with the request
   list ALREADY EMPTY — but §4.10's `commit()` is stated over an ACTIVE transaction and says nothing about the
   request list, so its whole purpose is to commit one with requests outstanding. Queueing the completion task
   there put it AHEAD of them: against a transaction that has not yet started, §2.7's held request tasks are not
   in the queue at all (idb_transaction_start appends them later), so the completion task overtook every one —
   §5.4 step 2.5.2 set the state to FINISHED and step 2.5.3 fired `complete` while the requests were still in the
   list, and each request's own §5.6 step 5.6 task then delivered its `success` AFTER `complete` and removed
   itself from a transaction that had already finished. In dev that was the commit task's step 2.1 assert; in
   release it is a page seeing its events in an order no browser produces.
   SO THE WAIT IS THE STATE, AND IT IS DISCHARGED WHERE THE REQUEST SET CHANGES — idb_transaction_remove_request
   is the ONE place the list can shrink (§5.6 step 5.6.1 is its only caller), so that is where step 2.1 is asked
   again and where the task is queued once the answer is yes. */
enum { TX_COMMIT_NO = 0, TX_COMMIT_QUEUED };

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

static int g_id_commit = -1, g_id_abort = -1, g_id_object_store = -1;
static int g_complete_stepid = -1, g_abort_stepid = -1, g_start_stepid = -1;

/* §2.7.2's consequence of a transaction leaving the live set, defined in the scheduling section below and
   called from the one line that performs that removal. */
static void tx_release_blocked(JSContext *ctx);
/* §5.4 step 2's in-parallel block, defined with the rest of §5.4 below and called from the one line that can
   change its step 2.1's answer — the removal that shrinks the request list. */
static void tx_commit_step2(JSContext *ctx, JSValueConst tx);
/* §5.7 step 10's rendezvous — see the header. NULL until §5.1's component registers, which it does in its own
   declaration, so an upgrade transaction cannot finish before there is something to tell. */
static void (*g_upgrade_finished)(JSContext *ctx, JSValueConst tx, bool aborted);

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

int idb_transaction_mode(JSContext *ctx, JSValueConst tx)
{
    int m = tx_int(ctx, tx, TX_MODE);

    DCHECK(m >= IDB_TX_READONLY && m <= IDB_TX_VERSIONCHANGE,
           "a transaction carried a mode IDBTransactionMode does not name");
    return m;
}

static JSValue tx_field(JSContext *ctx, JSValueConst tx, const char *field)
{
    JSValue slots = tx_slots(ctx, tx), v;

    DCHECK(JS_IsObject(slots), "an IDBTransaction field was read off a value carrying no slot record");
    v = JS_GetPropertyStr(ctx, slots, field);
    JS_FreeValue(ctx, slots);
    return v;
}

JSValue idb_transaction_connection(JSContext *ctx, JSValueConst tx)
{
    JSValue conn = tx_field(ctx, tx, TX_CONNECTION);

    DCHECK(JS_IsObject(conn), "a transaction carried no connection — §2.7 says all transactions are created "
                              "through one, and idb_transaction_new refuses a creation without it");
    return conn;
}

void idb_transaction_set_request(JSContext *ctx, JSValueConst tx, JSValueConst request)
{
    JSValue slots = tx_slots(ctx, tx);

    DCHECK(JS_IsObject(slots), "an open request was recorded on a value that is not a transaction");
    DCHECK(tx_int(ctx, tx, TX_MODE) == IDB_TX_VERSIONCHANGE,
           "an open request was recorded on a transaction that is not an upgrade transaction — §2.7.3's "
           "\"the request associated with transaction\" is a fact about an upgrade transaction alone, and a "
           "readonly transaction carrying one would make §5.4 step 2.5.4 reach into somebody else\u2019s request");
    JS_SetPropertyStr(ctx, slots, TX_REQUEST, JS_DupValue(ctx, request));
    JS_FreeValue(ctx, slots);
}

JSValue idb_transaction_request(JSContext *ctx, JSValueConst tx)
{
    return tx_field(ctx, tx, TX_REQUEST);
}

/* ---- §2.7's SCOPE, for the one transaction whose scope moves ---------------------------------------------- */

static JSValue tx_scope(JSContext *ctx, JSValueConst tx)
{
    JSValue scope = tx_field(ctx, tx, TX_SCOPE);

    DCHECK(JS_IsArray(scope), "a transaction carried no scope — §2.7 gives every transaction one and only "
                              "idb_transaction_new builds them");
    return scope;
}

void idb_transaction_scope_add(JSContext *ctx, JSValueConst tx, JSValueConst store)
{
    JSValue scope;

    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "a store was added to the scope of a transaction that is not an upgrade transaction — §2.7: \"a "
           "transaction\u2019s scope remains fixed unless the transaction is an upgrade transaction\"");
    scope = tx_scope(ctx, tx);
    JS_DefinePropertyValueUint32(ctx, scope, tx_array_len(ctx, scope), JS_DupValue(ctx, store), JS_PROP_C_W_E);
    JS_FreeValue(ctx, scope);
}

void idb_transaction_scope_remove(JSContext *ctx, JSValueConst tx, JSValueConst store)
{
    JSValue scope;
    uint32_t i, n;

    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "a store was removed from the scope of a transaction that is not an upgrade transaction");
    scope = tx_scope(ctx, tx);
    n = tx_array_len(ctx, scope);
    for (i = 0; i < n; i++) {
        JSValue m = JS_GetPropertyUint32(ctx, scope, i);
        bool same = JS_VALUE_GET_PTR(m) == JS_VALUE_GET_PTR(store);

        JS_FreeValue(ctx, m);
        if (!same) continue;
        for (; i + 1 < n; i++)
            JS_DefinePropertyValueUint32(ctx, scope, i, JS_GetPropertyUint32(ctx, scope, i + 1), JS_PROP_C_W_E);
        JS_SetPropertyStr(ctx, scope, "length", JS_NewUint32(ctx, n - 1));
        JS_FreeValue(ctx, scope);
        return;
    }
    JS_FreeValue(ctx, scope);
    DFAIL("§4.4's deleteObjectStore removed a store the upgrade transaction\u2019s scope does not hold — §5.7 "
          "step 3 sets that scope to the connection\u2019s object store set and §4.4\u2019s createObjectStore "
          "adds to both, so the two disagreeing means one of them was written by something else");
}

/* ---- §2.2.1's ONE HANDLE PER STORE, per transaction -------------------------------------------------------
 *
 * "There must be only one object store handle associated with a particular object store within a transaction."
 * THE SET IS KEYED BY THE STORE because that is the only key that sentence admits — see the header for the
 * three lines of page script that tell a store-keyed set from a name-keyed one, and for why a name is the one
 * thing about a store this set could not be keyed by. */

JSValue idb_transaction_handles(JSContext *ctx, JSValueConst tx, uint32_t *count)
{
    JSValue h = tx_field(ctx, tx, TX_HANDLES);

    DCHECK(JS_IsArray(h), "a transaction carried no object store handle set — every transaction is built by "
                          "idb_transaction_new, which gives it one");
    DCHECK(count != NULL, "a transaction's object store handles were asked for with nowhere to report how "
                          "many there are");
    *count = tx_array_len(ctx, h);
    return h;
}

JSValue idb_transaction_handle_find(JSContext *ctx, JSValueConst tx, JSValueConst store)
{
    uint32_t i, n;
    JSValue handles = idb_transaction_handles(ctx, tx, &n);

    DCHECK(JS_IsObject(store), "an object store handle was looked up for something that is not a §2.2 store");
    for (i = 0; i < n; i++) {
        JSValue h = JS_GetPropertyUint32(ctx, handles, i), held;
        bool same;

        DCHECK(idb_object_store_is(h), "a transaction's set of object store handles held something that is "
                                       "not an object store handle");
        held = idb_object_store_handle_store(ctx, h);
        same = JS_VALUE_GET_PTR(held) == JS_VALUE_GET_PTR(store);
        JS_FreeValue(ctx, held);
        if (same) {
            JS_FreeValue(ctx, handles);
            return h;
        }
        JS_FreeValue(ctx, h);
    }
    JS_FreeValue(ctx, handles);
    return JS_NULL;
}

void idb_transaction_handle_add(JSContext *ctx, JSValueConst tx, JSValueConst handle)
{
    JSValue handles, store, existing;
    uint32_t n;

    DCHECK(idb_object_store_is(handle),
           "something that is not an object store handle was filed in a transaction's handle set");
    store = idb_object_store_handle_store(ctx, handle);
    existing = idb_transaction_handle_find(ctx, tx, store);
    DCHECK(JS_IsNull(existing),
           "a SECOND object store handle was filed for one STORE in one transaction — §2.2.1: \"there must be "
           "only one object store handle associated with a particular object store within a transaction\", "
           "which §4.10's note states as what a page compares. §4.10's objectStore() resolves the store and "
           "asks for its existing handle before it mints another, and §4.4's createObjectStore files the one "
           "it has just made for a store nothing else can yet hold one for");
    JS_FreeValue(ctx, existing);
    JS_FreeValue(ctx, store);
    handles = idb_transaction_handles(ctx, tx, &n);
    /* A DEFINE and not an assignment, for core/idl_slots.h's sibling rule — the same reason the request list
       and the change list are written with one. */
    JS_DefinePropertyValueUint32(ctx, handles, n, JS_DupValue(ctx, handle), JS_PROP_C_W_E);
    JS_FreeValue(ctx, handles);
}

void idb_transaction_set_upgrade_finished_hook(void (*on_finished)(JSContext *ctx, JSValueConst tx,
                                                                   bool aborted))
{
    DCHECK(g_upgrade_finished == NULL || on_finished == NULL,
           "§5.7 step 10's rendezvous was registered twice — one component performs §5.1, and a second "
           "registration would silently replace the algorithm that is waiting");
    g_upgrade_finished = on_finished;
}

bool idb_transaction_any_live_for_connection(JSContext *ctx, JSValueConst connection)
{
    uint32_t i, n = tx_array_len(ctx, g_live);

    for (i = 0; i < n; i++) {
        JSValue tx = JS_GetPropertyUint32(ctx, g_live, i), conn;
        bool same;

        DCHECK(idb_transaction_is(tx), "§2.7's live set held something that is not a transaction");
        conn = idb_transaction_connection(ctx, tx);
        same = JS_VALUE_GET_PTR(conn) == JS_VALUE_GET_PTR(connection);
        JS_FreeValue(ctx, conn);
        JS_FreeValue(ctx, tx);
        if (same)
            return true;
    }
    return false;
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
           "an IDBTransaction that is COMMITTING was made ACTIVE again. §5.9 step 6 activates only an INACTIVE "
           "transaction, which is the whole of why a success event fired while a commit is in flight cannot "
           "let the page place another request against it");
    tx_set_int(ctx, tx, TX_STATE, state);
    if (state == IDB_TX_FINISHED) {
        /* §5.5 step 2'S RECORD IS DROPPED HERE, at the ONE door both endings go through — §5.4 committed the
           changes and §5.5 has just written them back, and §5.5 step 1 abandons the abort of a transaction that
           is already finished, so nothing can ever read this list again. It is emptied rather than left because
           every entry holds a §2.2 RECORD the store no longer names — for a `get` that is the whole value the
           page stored — and a finished transaction the page still has a reference to would hold all of them. */
        JSValue changes = idb_transaction_changes(ctx, tx);
        uint32_t i, n;

        /* AND NOBODY ELSE MAY STILL BE HOLDING IT — the one moment that question is decidable, which is why
           the assert is here and not at the accessor. idb_transaction_changes hands out an OWNED reference,
           and both of its other callers run strictly BEFORE this door (§5.5's revert and §5.8's
           was-this-store-created each assert that the state is not yet finished), so at this line the list is
           held by exactly two things: the transaction's slot record, and this local. A third holder is a
           caller that took a reference and dropped it, and the consequence is out of all proportion to the
           mistake: the list is emptied on the next line, so what leaks is an EMPTY Array — which holds
           Array.prototype, which holds the realm's function objects, each of which holds the REALM. One
           dropped reference kept a whole browser alive (2612 Functions, 408 shapes, a JSContext at refcount
           3108) and the runtime's leak walk could name nothing but three anonymous Arrays, three sessions
           apart, before gdb found the allocation. This is the line that names it at the transaction instead. */
        DCHECK(JS_ValueRefCount(changes) == 2,
               "a finishing transaction's list of database changes is held by something other than the "
               "transaction and this reader — idb_transaction_changes hands out an OWNED reference and every "
               "borrower has given it back by this point, so the extra holder is a caller that dropped one; "
               "the list is emptied on the next line, so what it leaks is an empty Array whose only remaining "
               "edge is Array.prototype, and that edge makes the whole realm behind it immortal");
        JS_SetPropertyStr(ctx, changes, "length", JS_NewInt32(ctx, 0));
        JS_FreeValue(ctx, changes);
        /* AND SO IS §2.7'S LIST OF HELD REQUEST TASKS, at the same door and for the same reason. A transaction
           can be aborted "even if the transaction ... hasn't yet started", and one with no requests commits
           from §2.7.1's cleanup without ever starting — either way its held tasks will never be queued, and
           each one owns §5.6's operation closure and through it the value the page asked to store. §5.5 step 6
           has already queued every one of those requests its own AbortError task, so nothing here is lost. */
        {
            JSValue held = tx_field(ctx, tx, TX_HELD);

            DCHECK(JS_IsArray(held), "a transaction carried no list of held request tasks");
            JS_SetPropertyStr(ctx, held, "length", JS_NewInt32(ctx, 0));
            JS_FreeValue(ctx, held);
        }
        /* "A transaction is said to be live from when it is created until its state is set to finished." The
           removal is HERE rather than at the two callers so a transaction cannot be finished by one path and
           stay live because the other path was the one that remembered. */
        n = tx_array_len(ctx, g_live);
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
            /* §2.7.2's constraints are stated over the transactions that "are not finished", so the moment
               this one leaves that set is the moment the implementation may be able to enforce them for a
               transaction that was created after it. §2.7.1's answer to that moment is a queued start, which
               is what this does — adjacent to the removal, because it is that removal's whole consequence. */
            tx_release_blocked(ctx);
            /* §5.2 step 3: "wait for all transactions created using connection to complete. Once they are
               complete, connection is closed." The moment that becomes true is exactly this one, and it is
               reported rather than polled — the connection is what owns the answer and what §5.1 step 10.6 is
               waiting on. It is told for EVERY transaction, because a connection with no close pending simply
               answers that nothing changed. */
            {
                JSValue conn = idb_transaction_connection(ctx, tx);

                idb_connection_transaction_finished(ctx, conn);
                JS_FreeValue(ctx, conn);
            }
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
           "a request was removed from a transaction whose request list is already empty — §5.6 step 5.6.1 "
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
    /* THIS LINE IS THE ONLY THING THAT CAN CHANGE §5.4 step 2.1's ANSWER, which is why the block is asked here
       rather than polled: the list never grows once the transaction is committing (see tx_commit_step2), and
       this is its one shrink. A transaction that is not COMMITTING has no step 2 block to make progress in —
       §5.9 step 8.3 and §5.10 step 8.4 are what create one for a transaction whose last request just finished,
       and they run LATER in the same task, after the event this removal precedes has been dispatched. */
    if (idb_transaction_state(ctx, tx) == IDB_TX_COMMITTING)
        tx_commit_step2(ctx, tx);
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

/* ---- §5.5 step 2's CHANGES MADE TO THE DATABASE ---------------------------------------------------------- */

JSValue idb_transaction_changes(JSContext *ctx, JSValueConst tx)
{
    JSValue list = tx_field(ctx, tx, TX_CHANGES);

    DCHECK(JS_IsArray(list), "a transaction carried no list of database changes — every transaction is built by "
                             "idb_transaction_new, which gives it one");
    return list;
}

uint32_t idb_transaction_change_count(JSContext *ctx, JSValueConst tx)
{
    JSValue list = idb_transaction_changes(ctx, tx);
    uint32_t n = tx_array_len(ctx, list);

    JS_FreeValue(ctx, list);
    return n;
}

void idb_transaction_record_change(JSContext *ctx, JSValueConst tx, JSValue change)
{
    JSValue list;

    DCHECK(idb_transaction_is(tx), "a database change was recorded against something that is not a "
                                   "transaction — §2.7: \"whenever data is read or written to the database it "
                                   "is done by using a transaction\", so a change with no transaction is a "
                                   "change §5.5 step 2 could never undo");
    DCHECK(idb_transaction_mode(ctx, tx) != IDB_TX_READONLY,
           "a READ-ONLY transaction changed the database. §2.7: \"the transaction is only allowed to read "
           "data. No modifications can be done by this type of transaction\" — §4.5's members report that as "
           "a ReadOnlyError before the operation is even minted");
    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "a FINISHED transaction changed the database. §5.5 step 2 has already reverted this transaction's "
           "changes, or §5.4 has committed them, so this one belongs to neither and nothing would ever undo "
           "it — §5.6 step 5 is aborted for every request of an aborted transaction exactly so that no "
           "operation can run past this point");
    list = idb_transaction_changes(ctx, tx);
    /* A DEFINE and not an assignment, for core/idl_slots.h's sibling rule — the same reason the request list
       above is written with one. */
    JS_DefinePropertyValueUint32(ctx, list, tx_array_len(ctx, list), change, JS_PROP_C_W_E);
    JS_FreeValue(ctx, list);
}

/* ---- §2.7.3's TWO SHARED UPGRADE ARMS ----------------------------------------------------------------------
 *
 * §5.4's commit task and §5.5's abort task each carry the same two upgrade-transaction steps, phrased
 * identically ("if transaction is an upgrade transaction, then set transaction's connection's associated
 * database's upgrade transaction to null" / "... let request be the open request associated with transaction
 * and set request's transaction to null"), so they are written once and each task states its own step around
 * them. A transaction that is not an upgrade transaction has no request recorded, which is the ONE question
 * both of them ask. */

static void tx_clear_upgrade_transaction(JSContext *ctx, JSValueConst tx)
{
    JSValue req = idb_transaction_request(ctx, tx), conn, db, held;

    if (JS_IsNull(req)) {
        JS_FreeValue(ctx, req);
        return;
    }
    JS_FreeValue(ctx, req);
    conn = idb_transaction_connection(ctx, tx);
    db = idb_connection_database(ctx, conn);
    held = idb_database_upgrade_transaction(ctx, db);
    DCHECK(JS_VALUE_GET_PTR(held) == JS_VALUE_GET_PTR(tx),
           "a database's upgrade transaction is not the upgrade transaction that is finishing — §2.1 gives a "
           "database AT MOST ONE, and §5.1's connection queue is what guarantees a second upgrade cannot start "
           "while this one is live");
    JS_FreeValue(ctx, held);
    idb_database_set_upgrade_transaction(ctx, db, JS_NULL);
    JS_FreeValue(ctx, db);
    JS_FreeValue(ctx, conn);
}

/* §5.4 step 2.5.4 and §5.5 step 7.3, then §5.7 step 10's rendezvous. The ABORT arm puts three more fields back
   — §5.5 step 7.3 is the only place in this standard where a request's done flag walks backwards, which §2.8's
   own note names — and both arms end by telling the algorithm that is waiting for this transaction to finish. */
static void tx_release_open_request(JSContext *ctx, JSValueConst tx, bool aborted)
{
    JSValue req = idb_transaction_request(ctx, tx);

    if (JS_IsNull(req)) {
        JS_FreeValue(ctx, req);
        return;
    }
    idb_request_set_transaction(ctx, req, JS_NULL);
    if (aborted) {
        idb_request_set_result(ctx, req, JS_UNDEFINED);
        idb_request_set_processed(ctx, req, false);
        idb_request_set_done(ctx, req, false);
    }
    JS_FreeValue(ctx, req);
    DCHECK(g_upgrade_finished != NULL,
           "an upgrade transaction finished with nothing waiting for it. §5.7 step 10 is \"wait for "
           "transaction to finish\" and §5.1 continues there — so the component that performs §5.1 registers "
           "this rendezvous in its own declaration, and an upgrade transaction can only have been created by "
           "that same component");
    g_upgrade_finished(ctx, tx, aborted);
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
    X(TXC_FINISH, "Indexed Database §5.4 steps 2.5.1-2.5.2 — ONE O(1) engine action: an upgrade "\
                  "transaction's database forgets it, and the transaction's state is set to finished") \
    X(TXC_FIRE,   "Indexed Database §5.4 step 2.5.3 (fire an event named complete at transaction)") \
    X(TXC_RELEASE, "Indexed Database §5.4 step 2.5.4 (an upgrade transaction's open request has its "\
                   "transaction set to null), and §5.7 step 10's wait for it to finish")
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
        DCHECK(tx_int(ctx, tx, TX_COMMIT) == TX_COMMIT_QUEUED,
               "§5.4 step 2.5's database task ran for a transaction whose step 2 block had not queued one — "
               "tx_commit_step2 is the only writer of that moment and the only minter of this task, so a task "
               "arriving without it was minted by something that did not run step 2.1's wait");
        /* §5.4 step 2.2: "If transaction's state is no longer committing, then terminate these steps." An
           abort that arrived between the commit and this task is exactly that, and it has already fired its
           own `abort` event — so the commit says nothing. */
        if (idb_transaction_state(ctx, tx) != IDB_TX_COMMITTING) {
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;
        }
        /* §5.4 step 2.1 ("wait until every item in transaction's request list is processed") was WAITED ON, by
           the block that queued this task: tx_commit_step2 queues it only once the list is empty, and no
           request can join a committing transaction's list. The assert is that statement, checked rather than
           believed — and it is now about the block above rather than about queue order, which is the whole of
           what changed when §4.10's `commit()` turned out to be stated over a transaction with requests
           outstanding. */
        DCHECK(idb_transaction_requests_empty(ctx, tx),
               "§5.4's commit task ran while the transaction still holds requests. Step 2.1 waits until every "
               "item in the request list is processed, and tx_commit_step2 is what performs that wait — so an "
               "outstanding request here means a request joined the list after that block queued this task, "
               "which §5.6 step 2's assert and §2.7.1's state both forbid for a committing transaction");
        /* §5.4 step 2.3's "attempt to write any outstanding changes to the database, considering the
           durability hint" has nothing to do: the store IS the heap (core/indexeddb/idb_database.c), §6.1
           wrote the record when it ran, and there is no medium behind it for a durability hint to reach. That
           is a fact about this store rather than a step skipped — which is also why step 2.4's write-error arm
           is unreachable and is not written. */
        /* §5.4 step 2.5.1: "If transaction is an upgrade transaction, then set transaction's connection's
           associated database's upgrade transaction to null." It runs BEFORE the state change, which is the
           order that matters: a `complete` handler reading `db.createObjectStore` must already be refused. */
        tx_clear_upgrade_transaction(ctx, tx);
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
        STEP_GOTO(s->hdr.stage, TXC_RELEASE, &s->phase, NULL);

    STEP_ARM(TXC_RELEASE);
        JS_FreeValue(ctx, cb_result);
        /* §5.4 step 2.5.4: "If transaction is an upgrade transaction, then let request be the request
           associated with transaction and set request's transaction to null." The request is an OPEN request
           and §2.8.1 is what this restores: "the transaction of an open request is null unless an
           upgradeneeded event has been fired". */
        tx_release_open_request(ctx, tx, /*aborted*/ false);
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_tx_complete_def = {
    sizeof(JSIdbTxFireState), js_idb_tx_complete_step, NULL, 0, .visit = js_idb_tx_fire_visit,
    .algorithm = "Indexed Database §5.4's commit-a-transaction database task", .steps = TXC_STEPS
};

/* §5.4 step 2's IN-PARALLEL BLOCK, ASKED. Step 2.1 is a wait, so this is the block making its own progress: it
   answers step 2.1 and, when the answer is yes, performs step 2.5's "queue a database task" — the whole of the
   block, because steps 2.2 belong to the task and steps 2.3-2.4 have nothing to do here (see the task's comment).
   It is called at the two moments the answer can CHANGE and at no others: when the block is created (§5.4 step 1
   has just run) and when the request list shrinks. Nothing polls, for tx_release_blocked's reason — a request
   leaves the list in exactly one place. */
static void tx_commit_step2(JSContext *ctx, JSValueConst tx)
{
    JSValue fn;

    DCHECK(idb_transaction_state(ctx, tx) == IDB_TX_COMMITTING,
           "§5.4 step 2's in-parallel block was asked to make progress for a transaction that is not COMMITTING "
           "— step 1 is what creates the block and it is the state it sets, so a block over any other state is "
           "one nothing ran step 1 for");
    DCHECK(tx_int(ctx, tx, TX_COMMIT) == TX_COMMIT_NO,
           "§5.4 step 2's in-parallel block was asked to make progress after it had already queued step 2.5's "
           "database task. A block that has queued it is finished, and a second task would run §5.4's whole "
           "completion twice — firing `complete` at a transaction §5.4 step 2.5.2 has already made FINISHED");
    /* §5.4 step 2.1: "Wait until every item in transaction's request list is processed." A request is removed
       from the list by §5.6 step 5.6.1, which its own task performs AFTER step 5.5 has set its processed flag —
       so an EMPTY list is the strictly later of the two conditions and asking it here answers step 2.1 without
       reading a flag per request. While the answer is no this returns and the block stays parked: no request can
       be ADDED to a committing transaction (§5.6 step 2 asserts the state is active and §4.5's and §4.6's
       members report a "TransactionInactiveError" before reaching it), so the list only ever shrinks and the
       removal that empties it is the one that resumes this. */
    if (!idb_transaction_requests_empty(ctx, tx))
        return;
    /* §5.4 step 2.5: "Queue a database task to run these steps". */
    tx_set_int(ctx, tx, TX_COMMIT, TX_COMMIT_QUEUED);
    fn = JS_NewStepClosure(ctx, g_complete_stepid, 0, 1, &tx);
    CHECK(!JS_IsException(fn), "IndexedDB: §5.4's commit task could not be minted");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

void idb_transaction_commit(JSContext *ctx, JSValueConst tx)
{
    /* §5.4 step 1: "Set transaction's state to committing." */
    idb_transaction_set_state(ctx, tx, IDB_TX_COMMITTING);
    /* §5.4 step 2: "Run the following steps in parallel". This engine has no second thread, so the block is
       driven at the moments its first step's answer can change rather than by one — see tx_commit_step2. */
    tx_commit_step2(ctx, tx);
}

/* ---- §5.5's ABORT: the database task that fires `abort` -------------------------------------------------- */

#define TXA_STAGES(X) \
    X(TXA_CLEAR, "Indexed Database §5.5 step 7.1 (an upgrade transaction's database forgets it)") \
    X(TXA_FIRE,  "Indexed Database §5.5 step 7.2 (fire an event named abort at transaction)") \
    X(TXA_RELEASE, "Indexed Database §5.5 steps 7.3.1-7.3.4 — ONE O(1) engine action: an upgrade "\
                   "transaction's open request is returned to the state it was in before the "\
                   "upgradeneeded event, so §4.3's completion task can settle it — and §5.7 step 10's "\
                   "wait for the transaction to finish")
enum { TXA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TXA_STEPS[] = { TXA_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_tx_abort_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbTxFireState *s = st;
    JSValueConst tx = tx_fire_target(&s->hdr);
    int r;

    tx_fire_start(s);
    STEP_DISPATCH(TXA_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(TXA_CLEAR);
        /* §5.5 step 7.1: "If transaction is an upgrade transaction, then set transaction's connection's
           associated database's upgrade transaction to null." */
        tx_clear_upgrade_transaction(ctx, tx);
        STEP_GOTO(s->hdr.stage, TXA_FIRE, &s->phase, NULL);

    STEP_ARM(TXA_FIRE);
        /* "with its bubbles attribute initialized to true": the event travels to the connection, which is what
           §2.7's get-the-parent algorithm answers with. */
        if (JS_IsUndefined(s->ev)) {
            s->ev = event_new(ctx, "abort", /*bubbles*/ true, /*cancelable*/ false);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
        }
        r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), tx, s->ev, JS_UNDEFINED, cb_result, NULL,
                                  out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        STEP_GOTO(s->hdr.stage, TXA_RELEASE, &s->phase, NULL);

    STEP_ARM(TXA_RELEASE);
        JS_FreeValue(ctx, cb_result);
        /* §5.5 step 7.3: the open request's transaction, result, processed flag and done flag are all put
           back — "in some cases, the request's done flag will be set to false, then set to true again", which
           §2.8's own note names and which is why an open request is the one request that walks backwards. */
        tx_release_open_request(ctx, tx, /*aborted*/ true);
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
    /* §5.5 step 2: "All the changes made to the database by the transaction are reverted." Asked of the
       component that owns §2.1's and §2.2's state, which recorded the inverse of each change as it was made —
       see core/indexeddb/idb_database.c, which also states why this is not a span of the flow's COW delta. A
       READ-ONLY transaction has recorded nothing and reverts nothing, which is §2.7's statement about that
       mode rather than a branch here. */
    idb_database_revert_transaction(ctx, tx);

    /* §5.5 step 3: "If transaction is an upgrade transaction, run the steps to abort an upgrade transaction
       with transaction." Step 2 above put the DATABASE back; §5.8 is the other half of that sentence — what
       the page can still SEE of it, which §5.5's own note names ("this reverts changes to all connection,
       object store handle, and index handle instances associated with transaction"). It runs HERE, between
       the revert it reads and step 4's state change that empties the record of what this transaction did. */
    if (idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE)
        idb_abort_upgrade_transaction(ctx, tx);
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
               with outstanding requests commits from §5.9 step 8.3 instead, once the last one's success event
               has been handled. */
            if (idb_transaction_requests_empty(ctx, tx))
                idb_transaction_commit(ctx, tx);
        }
        JS_FreeValue(ctx, tx);
    }
    JS_FreeValue(ctx, taken);
}

/* ---- §2.7.2 TRANSACTION SCHEDULING, and the START QUEUE §2.7 Transactions requires of it ------------------
 *
 * §2.7.2 states the constraints and §2.7.1 Transaction lifecycle states what an implementation does about them:
 * "when an implementation is able to enforce the constraints ... queue a database task to start the transaction
 * asynchronously", and §2.7 states what happens meanwhile — "the implementation must allow requests to be
 * placed against the transaction whenever it is active. This is the case even if the transaction has not yet
 * been started. Until the transaction is started the implementation must not execute these requests; however,
 * the implementation must keep track of the requests and their order."
 *
 * SO THE QUEUE IS THE REQUESTS' TASKS, HELD ON THE TRANSACTION — not a queue of transactions and not a gate
 * asked per operation. §5.6 Asynchronously executing a request mints its task where the request is placed,
 * because that is what carries the right operands; the transaction then decides whether that task may be
 * EXECUTED, and releases every held one in placement order when it starts. Nothing polls, because §2.7.2's
 * constraints are stated over the earlier transactions that "are not finished" — so the only event that can
 * change the answer is a transaction LEAVING §2.7's live set, and that one line asks again for everything still
 * waiting. */

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

/* §2.7.2 TRANSACTION SCHEDULING's two constraints, asked of a LIVE transaction. "Were created before tx" is
   the PREFIX of the live set that ends at tx itself — the set is append-ordered by creation — and "are not
   finished" is membership of it, so both halves are read off one walk. NOTHING about `started` appears here:
   §2.7.2 tests the earlier transactions' FINISHED-ness, so a transaction that is itself still waiting to start
   goes on blocking the ones behind it, which is what makes a chain of overlapping read/write transactions run
   in creation order rather than in whatever order they became unblocked. */
static bool tx_can_start(JSContext *ctx, JSValueConst tx)
{
    JSValue scope = tx_scope(ctx, tx);
    int mode = idb_transaction_mode(ctx, tx);
    uint32_t i, n = tx_array_len(ctx, g_live);
    bool can = true, found = false;

    for (i = 0; i < n && can; i++) {
        JSValue other = JS_GetPropertyUint32(ctx, g_live, i), oscope;
        int omode;

        DCHECK(idb_transaction_is(other), "§2.7.2's live set held something that is not a transaction");
        if (JS_VALUE_GET_PTR(other) == JS_VALUE_GET_PTR(tx)) {
            JS_FreeValue(ctx, other);
            found = true;
            break;
        }
        oscope = tx_scope(ctx, other);
        omode = idb_transaction_mode(ctx, other);
        /* A read-only transaction is blocked only by an earlier READ/WRITE one; a read/write transaction is
           blocked by ANY earlier one. Both only when the scopes overlap. */
        if ((mode != IDB_TX_READONLY || omode != IDB_TX_READONLY) && tx_scopes_overlap(ctx, scope, oscope))
            can = false;
        JS_FreeValue(ctx, oscope);
        JS_FreeValue(ctx, other);
    }
    JS_FreeValue(ctx, scope);
    DCHECK(found || !can,
           "§2.7.2 was asked about a transaction that is not in §2.7's live set, so the walk read the whole "
           "set as \"created before\" it — every transaction joins that set in idb_transaction_new and leaves "
           "it only when it is finished, and a finished one can never start");
    return can;
}

/* §2.7.1 Transaction lifecycle's "queue a database task to start the transaction asynchronously". It has one
   stage and no request buffer — see the stage's own label for what that one stage is and why it is one. */
typedef struct {
    JSStepHdr hdr;   /* FIRST: the driver writes the def and the operand bounds through it */
} JSIdbTxStartState;

/* It owns NOTHING — the transaction is the closure's capture, read off the header — and it still declares that,
   because a def with no ownership declaration cannot be forked at all (JS_RegisterStepDef refuses one). */
static void js_idb_tx_start_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

/* THE STAGE IS ONE STEP AND IT IS NOT O(1), WHICH THE LABEL SAYS RATHER THAN CLAIMS OTHERWISE. It performs one
   queue append per request the page placed before this transaction started, so its length is the page's — and
   it is nonetheless INDIVISIBLE, for the reason the mechanism exists: a rest between two appends would let a
   success event of an already-released request run and place a NEW request, which would be queued directly and
   would then execute AHEAD of one the page made earlier. §2.7.1's "requests must be executed in the order in
   which they were made against the transaction" is exactly what the span protects. The work is also bounded by
   work the page has already caused — §5.6 minted one task per request at placement — so this moves N tasks it
   did not create. */
#define TXS_STAGES(X) \
    X(TXS_START, "Indexed Database §2.7.1 Transaction lifecycle's start the transaction, and with it §2.7 " \
                 "Transactions' \"the implementation must keep track of the requests and their order\" — " \
                 "every request task held for this transaction is appended to the one task queue in placement " \
                 "order, an indivisible span because a rest inside it would let a released request's success " \
                 "handler place a request that overtakes one the page made earlier")
enum { TXS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TXS_STEPS[] = { TXS_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_tx_start_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbTxStartState *s = st;
    JSValueConst tx = tx_fire_target(&s->hdr);

    (void)out_cb; (void)out_argc;
    STEP_DISPATCH(TXS_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(TXS_START);
        JS_FreeValue(ctx, cb_result);
        /* §2.7.1: "a transaction can be aborted at any time before it is finished, even if the transaction
           isn't currently active or hasn't yet started" — and a transaction with no requests commits from
           §2.7.1's cleanup without ever starting. Either way it is FINISHED here and there is nothing left to
           execute, which is the same shape as §5.4 step 2.2's "if transaction's state is no longer
           committing, then terminate these steps". */
        if (idb_transaction_state(ctx, tx) == IDB_TX_FINISHED)
            return JS_STEP_DONE;
        DCHECK(tx_int(ctx, tx, TX_START) == TX_START_QUEUED,
               "§2.7.1's start task ran for a transaction that had not been left waiting for it — the only "
               "writer of the queued moment is tx_schedule_start, and the only other way out of it is this "
               "task, so a transaction found unqueued here was started by §5.7 step 6 behind a task queued "
               "for it as well and its held requests are about to be executed twice");
        idb_transaction_start(ctx, tx);
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_tx_start_def = {
    sizeof(JSIdbTxStartState), js_idb_tx_start_step, NULL, 0, .visit = js_idb_tx_start_visit,
    .algorithm = "Indexed Database §2.7.1 Transaction lifecycle's start-the-transaction database task",
    .steps = TXS_STEPS
};

/* The middle moment of §2.7.1 Transaction lifecycle's sentence. A transaction whose constraints cannot yet be
   enforced is simply left alone: tx_release_blocked asks again every time the live set shrinks, which is the
   only event that can change the answer. */
static void tx_schedule_start(JSContext *ctx, JSValueConst tx)
{
    JSValue fn;

    DCHECK(tx_int(ctx, tx, TX_START) == TX_START_NO,
           "a start was queued for a transaction that has already started or already has one queued — §2.7.1 "
           "queues exactly one database task per transaction, and a second would assert its way through "
           "idb_transaction_start's own check one task later, where the caller that queued it is gone");
    DCHECK(tx_can_start(ctx, tx), "a start was queued for a transaction §2.7.2's constraints forbid starting");
    tx_set_int(ctx, tx, TX_START, TX_START_QUEUED);
    fn = JS_NewStepClosure(ctx, g_start_stepid, 0, 1, &tx);
    CHECK(!JS_IsException(fn), "IndexedDB: §2.7.1's start task could not be minted");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

static void tx_release_blocked(JSContext *ctx)
{
    uint32_t i, n = tx_array_len(ctx, g_live);

    for (i = 0; i < n; i++) {
        JSValue other = JS_GetPropertyUint32(ctx, g_live, i);

        DCHECK(idb_transaction_is(other), "§2.7's live set held something that is not a transaction");
        if (tx_int(ctx, other, TX_START) == TX_START_NO && tx_can_start(ctx, other))
            tx_schedule_start(ctx, other);
        JS_FreeValue(ctx, other);
    }
}

bool idb_transaction_started(JSContext *ctx, JSValueConst tx)
{
    DCHECK(idb_transaction_is(tx), "§2.7.1's started was asked of something that is not a transaction");
    return tx_int(ctx, tx, TX_START) == TX_START_YES;
}

void idb_transaction_start(JSContext *ctx, JSValueConst tx)
{
    JSValue held;
    uint32_t i, n;

    DCHECK(idb_transaction_is(tx), "§2.7.1's start was given something that is not a transaction");
    DCHECK(tx_int(ctx, tx, TX_START) != TX_START_YES,
           "a transaction was started twice — §2.7.1's start is the one edge that lets its requests execute, "
           "and running it again would queue every request task this transaction has held since");
    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "a FINISHED transaction was started — §2.7.1's own order is that a transaction is started and only "
           "then committed or aborted, and its held tasks have already been dropped at that door");
    DCHECK(tx_can_start(ctx, tx),
           "a transaction was started while §2.7.2's constraints forbid it: an earlier live transaction has an "
           "overlapping scope and the two modes may not run together. For §5.7 step 6's upgrade transaction "
           "this is §5.1 step 10's guarantee, checked rather than believed — no other connection to the "
           "database is open when the upgrade begins, so no other transaction against it can be live");
    tx_set_int(ctx, tx, TX_START, TX_START_YES);
    /* §2.7: "until the transaction is started the implementation must not execute these requests; however, the
       implementation must keep track of the requests and their order." This is the release of that order —
       every held task, oldest first, into the ONE queue, so §5.6 step 5.1's "wait until request is the first
       item in transaction's request list that is not processed" stays discharged by the queue. */
    held = tx_field(ctx, tx, TX_HELD);
    DCHECK(JS_IsArray(held), "a transaction carried no list of held request tasks");
    n = tx_array_len(ctx, held);
    for (i = 0; i < n; i++) {
        JSValue fn = JS_GetPropertyUint32(ctx, held, i);

        DCHECK(JS_IsFunction(ctx, fn), "§2.7's list of held request tasks held something that is not one");
        JS_EnqueueCallTask(ctx, fn, 0, NULL);
        JS_FreeValue(ctx, fn);
    }
    JS_SetPropertyStr(ctx, held, "length", JS_NewInt32(ctx, 0));
    JS_FreeValue(ctx, held);
}

void idb_transaction_queue_request_task(JSContext *ctx, JSValueConst tx, JSValueConst task)
{
    JSValue held;

    DCHECK(idb_transaction_is(tx), "a §5.6 request task was queued against something that is not a transaction");
    DCHECK(JS_IsFunction(ctx, task), "a §5.6 request task that is not callable was queued");
    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "a request was placed against a FINISHED transaction — §5.6 step 2 asserts the state is active, and "
           "§4.6's members report a \"TransactionInactiveError\" before this algorithm is reached");
    if (idb_transaction_started(ctx, tx)) {
        JS_EnqueueCallTask(ctx, task, 0, NULL);
        return;
    }
    held = tx_field(ctx, tx, TX_HELD);
    DCHECK(JS_IsArray(held), "a transaction carried no list of held request tasks");
    /* A DEFINE and not an assignment, for core/idl_slots.h's sibling rule — the same reason the request list
       and the change list are written with one. */
    JS_DefinePropertyValueUint32(ctx, held, tx_array_len(ctx, held), JS_DupValue(ctx, task), JS_PROP_C_W_E);
    JS_FreeValue(ctx, held);
}

/* ---- creation ------------------------------------------------------------------------------------------- */

JSValue idb_transaction_new(JSContext *ctx, JSValueConst connection, JSValue scope, int mode, int durability)
{
    JSValue tx, st, proto, requests, handles, changes, held;
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
    /* §4.4's `transaction()` step 5 reports an empty scope as an "InvalidAccessError" before this algorithm is
       reached — but §5.7 step 3 sets an UPGRADE transaction's scope to the connection's object store set, and
       a database being created by this very upgrade has none yet. So the emptiness is refused for the modes a
       page can ask for and is the ordinary state for the one it cannot. */
    DCHECK(mode == IDB_TX_VERSIONCHANGE || tx_array_len(ctx, scope) > 0,
           "a transaction was created with an EMPTY scope — §4.4's `transaction()` step 5 reports that as an "
           "\"InvalidAccessError\" before this algorithm is reached");

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
    /* §2.7.3's open request, which only §5.7 records and only for the transaction it creates. */
    JS_SetPropertyStr(ctx, st, TX_REQUEST, JS_NULL);
    handles = JS_NewArray(ctx);
    CHECK(!JS_IsException(handles), "IndexedDB: a transaction's object store handle set could not be allocated");
    JS_SetPropertyStr(ctx, st, TX_HANDLES, handles);
    /* §5.5 step 2's changes, empty because a transaction has made none when it is created. */
    changes = JS_NewArray(ctx);
    CHECK(!JS_IsException(changes), "IndexedDB: a transaction's list of database changes could not be allocated");
    JS_SetPropertyStr(ctx, st, TX_CHANGES, changes);
    /* §2.7.1: "when a transaction is created ... its state is initially active" and it has not started. §5.4
       step 2's block does not exist yet either — step 1 is what creates it, by moving the state to committing. */
    JS_SetPropertyStr(ctx, st, TX_START, JS_NewInt32(ctx, TX_START_NO));
    JS_SetPropertyStr(ctx, st, TX_COMMIT, JS_NewInt32(ctx, TX_COMMIT_NO));
    held = JS_NewArray(ctx);
    CHECK(!JS_IsException(held), "IndexedDB: a transaction's list of held request tasks could not be allocated");
    JS_SetPropertyStr(ctx, st, TX_HELD, held);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBTransaction slot key could not be interned");
    JS_SetProperty(ctx, tx, k, st);
    JS_FreeAtom(ctx, k);

    /* "A transaction is said to be live from when it is created": the set §2.7.2 asks about is joined here and
       left in idb_transaction_set_state, so the two edges of `live` are the two edges of the sentence. It is
       joined BEFORE the constraints are asked, because "were created before tx" is this set's prefix and tx is
       its own end of it. */
    JS_DefinePropertyValueUint32(ctx, g_live, tx_array_len(ctx, g_live), JS_DupValue(ctx, tx), JS_PROP_C_W_E);
    /* §2.7.1's start, for every transaction a page can create. §5.7's upgrade transaction is NOT started here:
       its step 6 says "start transaction" directly, between step 5's deactivation and step 7 — one moment
       earlier than a queued task, which matters because step 9's `upgradeneeded` handler places requests
       against it inside the same task. Queueing one here as well would start it twice. */
    if (mode != IDB_TX_VERSIONCHANGE && tx_can_start(ctx, tx))
        tx_schedule_start(ctx, tx);
    JS_FreeValue(ctx, scope);
    return tx;
}

/* ---- §4.10's members ------------------------------------------------------------------------------------ */

/* "The objectStoreNames getter steps are: let names be a list of the names of the object stores in this's
   SCOPE. Return the result of creating a sorted name list with names."
   IT READS THE STORES' OWN NAMES and not a copy taken when the transaction was made, which is what makes the
   member's note true — "the contents of each list returned by this attribute does not change, but subsequent
   calls to this attribute during an upgrade transaction can return lists with different contents as object
   stores are created and deleted" — since §5.7 keeps an upgrade transaction's scope equal to the connection's
   object store set as that set moves. */
static JSValue js_tx_get_object_store_names(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue scope, names;
    uint32_t i, n;

    (void)magic;
    if (!tx_brand(ctx, this_val)) return JS_EXCEPTION;
    scope = tx_scope(ctx, this_val);
    n = tx_array_len(ctx, scope);
    names = JS_NewArray(ctx);
    CHECK(!JS_IsException(names), "IndexedDB: §4.10's list of object store names could not be allocated");
    for (i = 0; i < n; i++) {
        JSValue store = JS_GetPropertyUint32(ctx, scope, i);

        JS_DefinePropertyValueUint32(ctx, names, i, idb_object_store_name(ctx, store), JS_PROP_C_W_E);
        JS_FreeValue(ctx, store);
    }
    JS_FreeValue(ctx, scope);
    return idb_sorted_name_list(ctx, names);
}

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

/* "The db getter steps are to return this's connection's associated database." The IDL's type is IDBDatabase,
   and in this engine §2.1.1's connection IS that object (idb_connection.h says why) — so the two halves of
   that sentence are one value and `[SameObject]` holds without a cache. */
static JSValue js_tx_get_db(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!tx_brand(ctx, this_val)) return JS_EXCEPTION;
    return idb_transaction_connection(ctx, this_val);
}

/* "The objectStore(name) method steps are: if this's state is finished, then throw an InvalidStateError. Let
   store be the object store named name in this's scope, or throw a NotFoundError if none. Return an object
   store handle associated with store and this."
   THE STORE IS RESOLVED FIRST AND THE HANDLE IS ASKED FOR BY THE STORE, which is the order the three steps
   above are written in and is not a stylistic choice: §2.2.1 scopes "only one object store handle" to the
   STORE, so a lookup by NAME answers a different question. `s.name = 'b'` inside an upgrade transaction leaves
   the handle set holding a store this member is about to be asked for under 'b' — a name-first lookup finds
   nothing, mints a second handle for that one store, and `tx.objectStore('b') !== s` where every browser
   answers true.
   The SCOPE is what is searched and not the database: a transaction may only reach the stores it named. */
static JSValue js_tx_object_store(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue scope, handle, store = JS_UNDEFINED;
    const char *name;
    uint32_t i, n;

    (void)argc; (void)magic;
    if (!tx_brand(ctx, this_val)) return JS_EXCEPTION;
    if (idb_transaction_state(ctx, this_val) == IDB_TX_FINISHED)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "the transaction has finished, so it has no object stores");
    name = JS_ToCString(ctx, argv[0]);
    if (name == NULL) return JS_EXCEPTION;
    scope = tx_scope(ctx, this_val);
    n = tx_array_len(ctx, scope);
    for (i = 0; i < n; i++) {
        JSValue candidate = JS_GetPropertyUint32(ctx, scope, i);
        JSValue cname = idb_object_store_name(ctx, candidate);
        const char *c = JS_ToCString(ctx, cname);
        bool same;

        CHECK(c != NULL, "IndexedDB: a store in a transaction's scope could not report its own name");
        same = strcmp(c, name) == 0;
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cname);
        if (same) { store = candidate; break; }
        JS_FreeValue(ctx, candidate);
    }
    JS_FreeValue(ctx, scope);
    JS_FreeCString(ctx, name);
    if (JS_IsUndefined(store))
        return JS_ThrowDOMException(ctx, "NotFoundError",
                                    "no object store with that name is in the transaction's scope");
    handle = idb_transaction_handle_find(ctx, this_val, store);
    if (JS_IsNull(handle)) {
        JS_FreeValue(ctx, handle);
        handle = idb_object_store_handle(ctx, store, this_val);
        idb_transaction_handle_add(ctx, this_val, handle);
    }
    JS_FreeValue(ctx, store);
    return handle;
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
    /* `interface IDBTransaction : EventTarget` — addEventListener and the three handler attributes are
       reached through the chain rather than copied onto each transaction. */
    proto = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, proto, "IDBTransaction");
    event_target_install_handlers(ctx, proto, EH_IDB_TRANSACTION);
    idl_install_accessor(ctx, proto, "objectStoreNames", js_tx_get_object_store_names, 0, -1);
    idl_install_accessor(ctx, proto, "mode", js_tx_get_mode, 0, -1);
    idl_install_accessor(ctx, proto, "durability", js_tx_get_durability, 0, -1);
    idl_install_accessor(ctx, proto, "error", js_tx_get_error, 0, -1);
    idl_install_accessor(ctx, proto, "db", js_tx_get_db, 0, -1);
    idl_install_method(ctx, proto, "objectStore", g_id_object_store);
    idl_install_method(ctx, proto, "commit", g_id_commit);
    idl_install_method(ctx, proto, "abort", g_id_abort);
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
    {
        static const IdlArgType OS_ARGS[1] = { IDL_DOMSTRING };

        g_id_object_store = idl_method_id(ctx, OS_ARGS, 1, js_tx_object_store, 0);
    }
    g_id_commit = idl_method_id(ctx, NULL, 0, js_tx_commit, 0);
    g_id_abort = idl_method_id(ctx, NULL, 0, js_tx_abort, 0);
    g_complete_stepid = JS_RegisterStepDef(rt, &js_idb_tx_complete_def);
    g_abort_stepid = JS_RegisterStepDef(rt, &js_idb_tx_abort_def);
    g_start_stepid = JS_RegisterStepDef(rt, &js_idb_tx_start_def);
    /* §2.7.1's cleanup, registered with the ONE frontier — see the file comment for why it is a hook and not a
       bracket around whoever created the transaction. */
    engine_set_checkpoint_hook(idb_transaction_cleanup);
    g_ready = 1;
    agent_state_flag("idb_transaction", &g_ready, "the declaration latch");
    agent_state_ptr("idb_transaction", &g_tx_rt, "the runtime §2.7's two sets were allocated in");
    agent_state_value("idb_transaction", &g_key, "§2.7's internal-slot key");
    agent_state_value("idb_transaction", &g_live, "§2.7.2's set of live transactions");
    agent_state_value("idb_transaction", &g_cleanup, "§2.7.1's cleanup set");
    realm_declare_intrinsic(idb_transaction_install_realm);
}

void idb_transaction_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional and whose table asserts that a release
       row has a declare — so a release reaching here undeclared is not a state to tolerate, it is a host that
       tore this component down with something that is not the platform's list. */
    DCHECK(g_ready, "§2.7's transaction machinery was released in an agent that never declared it");
    DCHECK(rt == g_tx_rt, "idb_transaction_free was given a runtime that is not the one it declared into");
    /* §2.7.1'S CLEANUP GOES FIRST, and for the reason unhandled_rejection.c gives for the rejection tracker:
       the hook is a callback INTO this component held by the ONE frontier (solver/engine.c), and this release
       is about to free both of the sets it walks. It was never cleared at all — solver_agent_free runs AFTER
       platform_agent_free, so between the two calls the scheduler held a checkpoint hook whose component had
       already given back §2.7.2's live set and §2.7.1's cleanup set. */
    engine_set_checkpoint_hook(NULL);
    JS_FreeValueRT(rt, g_key);
    JS_FreeValueRT(rt, g_live);
    JS_FreeValueRT(rt, g_cleanup);
    g_key = JS_UNDEFINED;
    g_live = JS_UNDEFINED;
    g_cleanup = JS_UNDEFINED;
    g_tx_rt = NULL;
    g_ready = 0;
}
