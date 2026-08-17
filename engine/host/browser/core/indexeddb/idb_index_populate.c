/* INDEXED DATABASE §4.5 The IDBObjectStore interface's createIndex NOTE, as the algorithm it is.
 *
 * WHAT THE STANDARD ACTUALLY SAYS, AND WHY IT NEEDS A COMPONENT. §4.5's createIndex steps create the index
 * SYNCHRONOUSLY — step 11 is "let index be a new index in store" — and then the prose adds two things those
 * steps do not: "the index creation itself is processed as an ASYNCHRONOUS REQUEST within the upgrade
 * transaction", and, for a store whose data violates the new index's constraints, "the implementation must
 * queue a database task to ABORT THE UPGRADE TRANSACTION which was used for the createIndex() call". §4.5's own
 * worked example is what pins both halves:
 *
 *     const request1 = objectStore.put({name: "betty"}, 1);
 *     const request2 = objectStore.put({name: "betty"}, 2);
 *     const index = objectStore.createIndex("by_name", "name", {unique: true});
 *
 * "Since the index creation is considered an asynchronous request, the index's uniqueness constraint does not
 * cause the second request to fail. Instead, the transaction will be aborted when the index is created and the
 * constraint fails."
 *
 * SO TWO THINGS ARE TRUE AT ONCE and the flag on §2.6's index is what holds them apart: the index EXISTS the
 * moment createIndex returns (`indexNames` reports it, `index()` finds it, §4.6's members answer off it), and
 * §6.1 step 5 must NOT see it until this operation has run. Without the flag the two `put`s file records in it
 * and the second one fails — a ConstraintError on the page's own request, where a browser aborts the whole
 * upgrade — which is a wrong answer rather than a missing one.
 *
 * AND THE ABORT NEEDS NO SPECIAL CASE, which is the reason this is a REQUEST rather than a call. The operation
 * raises §6.1 step 5's "ConstraintError"; §5.6 step 5.4 reverts the index records this operation had already
 * written; §5.10 Firing an error event fires `error` at a request the page holds no reference to, so its
 * canceled flag is false because nothing can call `preventDefault()` on it; and §5.10 step 9.3 runs abort a
 * transaction with the request's error. §5.5 Aborting a transaction step 2 then reverts the index's CREATION
 * along with everything else the upgrade did, and §5.8 puts the page's handles back.
 *
 * IT IS A STEP MACHINE FOR §6.1's REASON, ONE LEVEL DEEPER. It walks a list of the PAGE'S SIZE — every record
 * the store holds — and each record runs §6.1 step 5, whose step 5.1 is §7.1 and therefore §7.4's array arm.
 * So it rests once per record, and inside that at every rest point §7.4 has. There is no C entry: a second,
 * non-suspending copy of this walk is the thing this shape exists to prevent. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_index.h"
#include "core/indexeddb/idb_index_populate.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"

/* WHAT THE OPERATION WAS MINTED OVER. Captured by the closure, because §scheduler's rule is that an operation
   which becomes a work item takes its inputs WITH it: the store read back off the index at task time would be
   the same one today and is exactly the shape that breaks when it stops being. */
#define POP_CD_TX     0
#define POP_CD_STORE  1
#define POP_CD_INDEX  2

typedef struct {
    JSStepHdr    hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    IdbStoreWalk sw;       /* §6.1 step 5 in flight, for ONE record of the store */
    uint32_t     pos;      /* how many of the store's records have been begun */
    uint32_t     n;        /* how many it held when the walk started — see the assert in POP_RECORD */
} JSIdbPopulateState;

/* WHAT THIS MACHINE OWNS: §6.1's own record and nothing else. It has no answer of its own — §5.6 makes the
   request's result whatever the operation returned, and this one returns undefined on every path, which is why
   the definition declares no `fini`. */
static void js_idb_pop_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdbPopulateState *s = st;

    idb_store_record_walk_visit(ctx, &s->sw, v);
}

#define POP_STAGES(X) \
    X(POP_RECORD, "Indexed Database §4.5's createIndex note (the index creation is processed as an " \
                  "asynchronous request within the upgrade transaction) — the next record of the index's " \
                  "referenced object store, or the end of the population when there is none") \
    IDB_STORE_RECORD_ALGO_STAGES(X, POP, "Indexed Database §4.5's createIndex population request")
enum { POP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const POP_STEPS[] = { POP_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_populate_operation(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb,
                                     int *out_argc)
{
    JSIdbPopulateState *s = st;

    STEP_DISPATCH(POP_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(POP_RECORD);
    {
        JSValueConst tx = JS_StepClosureData(&s->hdr, POP_CD_TX);
        JSValueConst store = JS_StepClosureData(&s->hdr, POP_CD_STORE);
        JSValueConst index = JS_StepClosureData(&s->hdr, POP_CD_INDEX);
        JSValue key, value;

        JS_FreeValue(ctx, cb_result);
        DCHECK(idb_transaction_is(tx) && JS_IsObject(store) && JS_IsObject(index),
               "§4.5's population operation was minted over something other than a transaction, an object "
               "store and the index being filled");
        DCHECK(idb_index_is_unpopulated(ctx, index),
               "§4.5's population operation ran for an index that is already POPULATED. It is the only thing "
               "that clears that flag and it clears it once, at its own last stage — so this operation has "
               "been performed twice for one index, and the second run would file every record's index "
               "records a second time");
        /* THERE IS NOTHING TO CREATE IF THE INDEX IS GONE. §4.5's deleteIndex step 8 is "Destroy index", and
           `createIndex('i', …)` followed by `deleteIndex('i')` in one upgrade handler leaves this request
           already queued for an index that no longer references anything — as does deleting the object store
           itself (§4.4's deleteObjectStore, whose store keeps its records only so §5.5 step 2 can put it
           back). Neither is a gap: §6.1 step 5 is stated over "each index which references store", and a
           destroyed index is in no store's set. The flag is left SET, because nothing populated it and
           nothing can read it — §4.6's members and §4.5's `index()` each refuse a destroyed index first. */
        if (idb_index_is_deleted(ctx, index) || idb_object_store_is_deleted(ctx, store))
            return JS_STEP_DONE;
        if (s->pos == 0)
            s->n = idb_store_record_count(ctx, store);
        DCHECK(s->n == idb_store_record_count(ctx, store),
               "an object store's list of records CHANGED while §4.5's population operation walked it. An "
               "operation is one task and §2.7.2 gives one object store to one read/write transaction at a "
               "time, so nothing may add to or remove from this list across one — and a record added behind "
               "the cursor is a record the new index would never hold an entry for");
        /* THE END OF THE WALK IS WHERE THE FLAG CLEARS, and not the line that placed the request: every
           record has to be in the index before §6.1 step 5 may see it, or a `put` running between two of
           these stages would file its own record and leave the ones still ahead of the cursor to be filed
           again by this walk. */
        if (s->pos >= s->n) {
            idb_index_set_populated(ctx, index);
            return JS_STEP_DONE;
        }
        idb_store_record_at(ctx, store, s->pos, &key, &value);
        s->pos++;
        /* §6.1 STEP 5 FOR THIS RECORD AND THIS ONE INDEX — the same block a `put` runs, entered at step 5.
           The record's own key is what step 5.5's "key as its value" files, and its stored value is what step
           5.1 extracts the index key from. */
        idb_store_record_walk_start_index(ctx, &s->hdr, &s->sw, tx, store, index, value, key,
                                          POP_LENGTH, POP_RECORD);
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, key);
        return JS_STEP_YIELD;
    }

    /* §6.1 step 5's own rest points, which include §7.1's and therefore §7.4's. Named individually for the
       reason idb_object_store.c names the `put` operation's: a stage added to the algorithm does not compile
       until it has an arm here, where a partial list would silently route it into a neighbour. */
    STEP_ARM(POP_LENGTH);
    STEP_ARM(POP_BEGIN);
    STEP_ARM(POP_HOP);
    STEP_ARM(POP_ENTRY);
    STEP_ARM(POP_SUBKEY);
    STEP_ARM(POP_LEAVE);
    STEP_ARM(POP_INDEX);
    STEP_ARM(POP_INDEX_TOOK);
    STEP_ARM(POP_INDEX_UNIQUE);
    STEP_ARM(POP_INDEX_WRITE);
        return idb_store_record_walk_run(ctx, &s->hdr, &s->sw, cb_result, POP_LENGTH, out_cb, out_argc);

    /* §6.1's STEPS 1-4 ARE NOT THIS OPERATION'S, and their arms exist only so the dispatch is complete. The
       record is already in the store's list under its own key: there is no key to generate, no `add` to
       refuse, nothing to displace and nothing to write. `start_index` points the cursor at step 5 and the
       block only ever moves forward, so reaching one of these means the entry pointed somewhere else. */
    STEP_ARM(POP_GENERATE);
    STEP_ARM(POP_NO_OVERWRITE);
    STEP_ARM(POP_REMOVE);
    STEP_ARM(POP_STORE);
        JS_FreeValue(ctx, cb_result);
        DFAIL("§4.5's population operation was entered at one of §6.1's steps 1-4. It performs step 5 alone "
              "over records the object store already holds, and idb_store_record_walk_start_index is what "
              "puts its cursor there");
        return JS_STEP_ABRUPT;
}

static const JSTrampStepDef js_idb_populate_def = {
    sizeof(JSIdbPopulateState), js_idb_populate_operation, NULL, 0, .visit = js_idb_pop_visit,
    .algorithm = "Indexed Database §4.5's createIndex note — the index creation processed as an asynchronous "
                 "request, running §6.1 step 5 over every record the referenced object store holds",
    .steps = POP_STEPS
};
static int g_populate_stepid = -1;

void idb_index_populate_request(JSContext *ctx, JSValueConst handle, JSValueConst tx, JSValueConst store,
                                JSValueConst index)
{
    JSValueConst data[3];
    JSValue op, req;

    DCHECK(g_populate_stepid >= 0, "§4.5's population request was placed before idb_index_populate_init "
                                   "declared its machine");
    DCHECK(idb_index_is_unpopulated(ctx, index),
           "§4.5's createIndex placed a population request for an index that is already populated — the flag "
           "is set by idb_index_create and cleared only by the operation this places, so one index has been "
           "given two");
    data[POP_CD_TX] = tx;
    data[POP_CD_STORE] = store;
    data[POP_CD_INDEX] = index;
    op = JS_NewStepClosure(ctx, g_populate_stepid, 0, 3, data);
    CHECK(!JS_IsException(op), "IndexedDB: §4.5's index population operation could not be minted");
    req = idb_request_execute(ctx, handle, tx, op);
    JS_FreeValue(ctx, op);
    /* §5.6's "Return request" has no reader here: §4.5 returns an IDBIndex, and the page is never handed this
       request. Dropping it is what makes §5.10 step 9.3 unconditional — a request nobody holds is a request
       nobody can register an `error` listener on, so its canceled flag is false and the transaction aborts. */
    JS_FreeValue(ctx, req);
}

void idb_index_populate_init(JSContext *ctx)
{
    DCHECK(g_populate_stepid < 0, "idb_index_populate_init ran twice — one instance is one agent");
    g_populate_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_idb_populate_def);
    agent_state_id("idb_index_populate", &g_populate_stepid,
                   "§4.5's index population machine, and the declaration latch");
}

void idb_index_populate_free(JSRuntime *rt)
{
    (void)rt;
    DCHECK(g_populate_stepid >= 0, "§4.5's population machinery was released in an agent that never declared "
                                   "it");
    g_populate_stepid = -1;
}
