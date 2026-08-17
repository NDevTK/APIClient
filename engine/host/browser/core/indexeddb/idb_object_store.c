/* INDEXED DATABASE §2.2.1's OBJECT STORE HANDLE and §4.5's IDBObjectStore over it — the members a page reaches
 * §6.1's store-a-record and §6.2's retrievals through.
 *
 * WHY A HANDLE EXISTS AT ALL, WHICH IS THE WHOLE OF WHAT THIS FILE IS. §2.2.1: "script does not interact with
 * object stores directly. Instead, within a transaction, script has indirect access via an object store
 * handle." The handle is the pair (store, transaction), and every member below reads the TRANSACTION first —
 * whether it is still active, whether it is read-only — because that is where the standard puts every one of
 * its refusals. A component that gave the page the STORE would have nothing to ask those questions of, and
 * `store.get()` from a `setTimeout` after the creating task returned would silently read a database instead of
 * reporting the "TransactionInactiveError" every browser reports.
 *
 * THE HANDLE IS THE IDBObjectStore OBJECT, the same identity idb_request.c gives §2.8's request and
 * idb_connection.c gives §2.1.1's connection: §4.4's `createObjectStore` and §4.10's `objectStore()` each end
 * in "return an object store handle", and the page holds an IDBObjectStore. Its three fields live in an
 * internal-slot record under a private Symbol, so every write is an ordinary property write the per-flow COW
 * delta captures — flow A's rename is invisible to its sibling.
 *
 * THE UNIQUENESS IS THE TRANSACTION'S, NOT THIS FILE'S. §2.2.1: "there must be only one object store handle
 * associated with a particular object store within a transaction", and §4.10's note is what a page observes —
 * `tx.objectStore('s') === tx.objectStore('s')`. The SET lives on the transaction (idb_transaction.h), because
 * that is what the sentence says it is scoped to; a cache here would be a second answer to one question and
 * would have to be keyed by a pair this file has no identity for.
 *
 * WHAT IS ABSENT AND WHY — ONE NAMED CONSTRUCT PER MEMBER, so the next diff starts from building the construct
 * rather than from re-deriving which one is missing. Every number and title below is the W3C Recommendation's
 * own (§2.6 Index, §2.10 Cursor, §6.7 Cursor iteration operation, §6.3 Index retrieval operations, §5.12
 * Creating a request to retrieve multiple items, §4.8 The IDBRecord interface):
 *   - `indexNames`, `index()`, `createIndex()`, `deleteIndex()` wait on §2.6 INDEX. Nothing in this engine can
 *     hold §2.6's index set, and §2.6.1's index handle has no interface (§4.6 The IDBIndex interface is
 *     absent), which is the same reason §6.3 Index retrieval operations has no body.
 *   - `openCursor()`, `openKeyCursor()` wait on §2.10 CURSOR — the record with a position, a source handle and
 *     a direction — and on §6.7 Cursor iteration operation, which is what a `continue()` performs.
 *   - `getAll()`, `getAllKeys()`, `getAllRecords()` wait on §5.12 CREATING A REQUEST TO RETRIEVE MULTIPLE
 *     ITEMS, over the `IDBGetAllOptions` dictionary §4.5 declares beside this interface; `getAllRecords()`
 *     additionally waits on §4.8's IDBRecord.
 * Each absent member is a TypeError naming itself, which the IDL gap auditor lists — never a shape-only member
 * that would report a retrieval nothing performed.
 *
 * The members that ARE here are exactly the algorithms that are: §6.1's storage operation (`put`/`add`), §6.2's
 * two retrievals (`get`/`getKey`), §6.4's deletion (`delete`), §6.5's counting (`count`) and §6.6's clear. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_array.h"
#include "core/indexeddb/idb_key_path.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/indexeddb/idb_object_store.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/realm.h"
#include "core/structured_clone.h"

/* §2.2.1's three fields, spelled once. */
#define OS_STORE       "store"        /* the §2.2 object store record this handle is a view of */
#define OS_TRANSACTION "transaction"  /* §2.2.1's associated transaction */
#define OS_NAME        "name"         /* §2.2.1's own name, "initialized to the name of the associated store" */
/* §4.5's keyPath getter answers with ONE object per handle — "it returns the same object instance every time
   it is inspected" — so the converted value is remembered here on its FIRST inspection. JS_UNDEFINED is the
   positive statement "not inspected yet" and is written when the handle is created, never left absent: a
   consumer that read a missing field as "no cache" would be defaulting a field nobody writes. */
#define OS_KEY_PATH    "keyPathValue"

static JSValue   g_key;
static JSClassID g_os_class;
static int       g_ready;
static JSRuntime *g_os_rt;
static int g_id_put = -1, g_id_add = -1, g_id_get = -1, g_id_get_key = -1, g_setter_name = -1;
static int g_id_delete = -1, g_id_clear = -1, g_id_count = -1;

/* The magics that tell the two pairs of members apart. §4.5 states `put` and `add` as ONE algorithm differing
   only in the no-overwrite flag, and `get` and `getKey` as two retrievals differing only in which of §6.2's
   two operations they name — so each pair is one body, which is what the standard writes. */
enum { OS_PUT = 0, OS_ADD = 1 };
enum { OS_GET_VALUE = 0, OS_GET_KEY = 1 };

/* ---- the record ---------------------------------------------------------------------------------------- */

static JSValue os_slots(JSContext *ctx, JSValueConst h)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an IDBObjectStore slot was asked for before its interface was declared");
    if (!JS_IsObject(h))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBObjectStore slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &st, h, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

bool idb_object_store_is(JSValueConst v)
{
    return g_os_class != 0 && JS_GetClassID(v) == g_os_class;
}

static bool os_brand(JSContext *ctx, JSValueConst this_val)
{
    if (idb_object_store_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "an IDBObjectStore member was reached on something that is not an IDBObjectStore");
    return false;
}

static JSValue os_get(JSContext *ctx, JSValueConst h, const char *field)
{
    JSValue slots = os_slots(ctx, h), v;

    DCHECK(JS_IsObject(slots), "an IDBObjectStore field was read off a value carrying no slot record");
    v = JS_GetPropertyStr(ctx, slots, field);
    JS_FreeValue(ctx, slots);
    return v;
}

JSValue idb_object_store_handle(JSContext *ctx, JSValueConst store, JSValueConst transaction)
{
    JSValue h, st, proto;
    JSAtom k;

    DCHECK(g_ready, "an object store handle was created before idb_object_store_init declared the interface");
    DCHECK(JS_IsObject(store), "an object store handle was created over something that is not a §2.2 store");
    DCHECK(idb_transaction_is(transaction), "an object store handle was created with no transaction — §2.2.1 "
                                            "gives every handle one, and it is what every member of §4.5 asks "
                                            "its first question of");
    proto = JS_GetClassProto(ctx, g_os_class);
    DCHECK(!JS_IsNull(proto), "IDBObjectStore.prototype was asked for in a realm that never ran its install");
    h = JS_NewObjectProtoClass(ctx, proto, g_os_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(h), "IndexedDB: the IDBObjectStore allocation failed");
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "IndexedDB: the IDBObjectStore slot record allocation failed");
    JS_SetPropertyStr(ctx, st, OS_STORE, JS_DupValue(ctx, store));
    JS_SetPropertyStr(ctx, st, OS_TRANSACTION, JS_DupValue(ctx, transaction));
    /* "A name, which is initialized to the name of the associated object store when the object store handle is
       created." The handle's OWN copy — which is what §5.8 reverts and what makes `store.name` go on answering
       the old name after an aborted rename, the difference §4.5's getter note draws. */
    JS_SetPropertyStr(ctx, st, OS_NAME, idb_object_store_name(ctx, store));
    JS_SetPropertyStr(ctx, st, OS_KEY_PATH, JS_UNDEFINED);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBObjectStore slot key could not be interned");
    JS_SetProperty(ctx, h, k, st);
    JS_FreeAtom(ctx, k);
    return h;
}

JSValue idb_object_store_handle_store(JSContext *ctx, JSValueConst handle)
{
    JSValue store;

    DCHECK(idb_object_store_is(handle),
           "the associated object store of something that is not an object store handle was asked for");
    store = os_get(ctx, handle, OS_STORE);
    DCHECK(JS_IsObject(store), "an object store handle carried no store — idb_object_store_handle gives every "
                               "handle one and nothing else builds them");
    return store;
}

JSValue idb_object_store_handle_name(JSContext *ctx, JSValueConst handle)
{
    JSValue name;

    DCHECK(idb_object_store_is(handle), "the name of something that is not an object store handle was asked for");
    name = os_get(ctx, handle, OS_NAME);
    DCHECK(JS_IsString(name), "an object store handle carried no name — §2.2.1 initialises one when the handle "
                              "is created, and only this file writes it");
    return name;
}

void idb_object_store_handle_restore_name(JSContext *ctx, JSValueConst handle)
{
    JSValue store = idb_object_store_handle_store(ctx, handle), slots = os_slots(ctx, handle);

    DCHECK(JS_IsObject(slots), "an object store handle's name was restored on a value carrying no slot record");
    JS_SetPropertyStr(ctx, slots, OS_NAME, idb_object_store_name(ctx, store));
    JS_FreeValue(ctx, slots);
    JS_FreeValue(ctx, store);
}

/* ---- the refusals every member of §4.5 begins with -------------------------------------------------------
 *
 * §4.5 states them once per member and states them IDENTICALLY, in this order, which is why they are one
 * function: "if store has been deleted, throw an InvalidStateError"; "if transaction's state is not active,
 * then throw a TransactionInactiveError"; and — for the members that write — "if transaction is a read-only
 * transaction, throw a ReadOnlyError". Six copies of three sentences is six chances to write one of them as
 * something else. `writes` says whether the third applies. Returns -1 with the throw live. */
static int os_check(JSContext *ctx, JSValueConst h, bool writes, JSValue *pstore, JSValue *ptx)
{
    JSValue store = os_get(ctx, h, OS_STORE), tx = os_get(ctx, h, OS_TRANSACTION);

    DCHECK(JS_IsObject(store) && idb_transaction_is(tx),
           "an object store handle carried no store or no transaction — idb_object_store_handle gives every "
           "handle both");
    if (idb_object_store_is_deleted(ctx, store)) {
        JS_FreeValue(ctx, store);
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "InvalidStateError", "the object store has been deleted");
        return -1;
    }
    if (idb_transaction_state(ctx, tx) != IDB_TX_ACTIVE) {
        JS_FreeValue(ctx, store);
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "TransactionInactiveError",
                             "the transaction the object store belongs to is not active");
        return -1;
    }
    if (writes && idb_transaction_mode(ctx, tx) == IDB_TX_READONLY) {
        JS_FreeValue(ctx, store);
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "ReadOnlyError", "the transaction is a read-only transaction");
        return -1;
    }
    *pstore = store;
    *ptx = tx;
    return 0;
}

/* A MEMBER'S STEPS ARE ONE TASK, AND ITS SUSPENSIONS MAY NOT CHANGE THAT — asserted where the member depends on
 * it, which is the step that places the request.
 *
 * Every member below reads the transaction's state at its own step 3-4 and then SUSPENDS before its operation,
 * because §7.4's array arm runs the page's index accessors. In a browser that code runs inside the member's own
 * task, so §2.7.1's cleanup — which deactivates an active transaction and commits it when its request list is
 * empty — cannot run across it; this engine's forced preempt says the same thing about itself, that it must be
 * transparent to observable ordering. So a transaction that is no longer ACTIVE here is one that was deactivated
 * mid-member, and the abort belongs at that fact rather than at §5.6's own assert three files away, which reads
 * as "the member did not report a TransactionInactiveError" and would send its reader to this file. */
static void os_active_across_suspension(JSContext *ctx, JSValueConst tx)
{
    DCHECK(idb_transaction_state(ctx, tx) == IDB_TX_ACTIVE,
           "an IDBObjectStore member's transaction was deactivated BETWEEN the state check its own steps make "
           "and the step that places the request — the member suspended in between (§7.4's array arm is the "
           "page's own accessors), and nothing may deactivate a transaction across that: §2.7.1's cleanup is a "
           "checkpoint of the event loop, and a forced preempt is not one");
    (void)ctx; (void)tx;
}

/* ---- §5.6's OPERATIONS, as the callables this component states them with -----------------------------------
 *
 * §4.5 says "let operation be an algorithm to run store a record into an object store with store, clone, key
 * and no-overwrite flag" — an algorithm CLOSED OVER ITS OPERANDS, which is exactly what §scheduler's rule
 * demands of anything that becomes a work item: the operands travel WITH it, so the record is filed under the
 * key the member converted rather than under whatever the handle names by the time the task runs.
 *
 * Neither of these runs the page's code. §6.1's clone is over a value §4.5 has ALREADY cloned (see the member),
 * and §6.2's is over a value this component built, so both operate on engine-owned plain data — which is why
 * they are C bodies over their captured data rather than step machines. */
/* THE TRANSACTION IS ONE OF THE OPERANDS, for the same reason the store and the key are: this operation is a
   work item, and §5.5 step 2 has to know whose change the record it writes is. Read back off the handle at task
   time it would be whichever transaction that handle names BY THEN — the same one today, and exactly the shape
   §scheduler names as the defect that arrives with every conversion from a call to a job. */
#define OP_STORE_TX     0
#define OP_STORE_STORE  1
#define OP_STORE_VALUE  2
#define OP_STORE_KEY    3

static JSValue js_idb_store_operation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic, JSValueConst *func_data)
{
    JSValue key = JS_UNDEFINED;

    (void)this_val; (void)argc; (void)argv;
    /* §6.1's own contract: 0 with the key it filed the record under, or -1 with the DOMException it names
       live — which §5.6 step 5.2 takes as "result is an error". */
    if (idb_store_record(ctx, func_data[OP_STORE_TX], func_data[OP_STORE_STORE], func_data[OP_STORE_VALUE],
                         func_data[OP_STORE_KEY], magic == OS_ADD, &key) < 0)
        return JS_EXCEPTION;
    return key;   /* "If successful, request's result will be the record's key." */
}

#define OP_GET_STORE 0
#define OP_GET_RANGE 1

static JSValue js_idb_retrieve_operation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv;
    if (magic == OS_GET_KEY)
        return idb_retrieve_key(ctx, func_data[OP_GET_STORE], func_data[OP_GET_RANGE]);
    return idb_retrieve_value(ctx, func_data[OP_GET_STORE], func_data[OP_GET_RANGE]);
}

/* §6.4's DELETE RECORDS FROM AN OBJECT STORE and §6.6's CLEAR, as two closures because they are two algorithms
   — §6.4 is closed over a range and §6.6 has none, and a shared body would have had to mint the unbounded range
   §6.6 does not state. Both carry the TRANSACTION for the reason §6.1's closure does: the change they record
   belongs to the transaction this request was placed against, not to whichever the handle names when the task
   runs. Both "return undefined". */
#define OP_DEL_TX     0
#define OP_DEL_STORE  1
#define OP_DEL_RANGE  2

static JSValue js_idb_delete_operation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    idb_delete_records(ctx, func_data[OP_DEL_TX], func_data[OP_DEL_STORE], func_data[OP_DEL_RANGE]);
    return JS_UNDEFINED;
}

#define OP_CLEAR_TX     0
#define OP_CLEAR_STORE  1

static JSValue js_idb_clear_operation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    idb_clear_store(ctx, func_data[OP_CLEAR_TX], func_data[OP_CLEAR_STORE]);
    return JS_UNDEFINED;
}

/* §6.5's COUNT THE RECORDS IN A RANGE. It carries NO transaction, because it changes nothing and therefore
   records nothing against one — the same operand list §6.2's retrievals have, and for the same reason. "Return
   count", which §5.6 makes the request's result. It is a COUNT OF THIS LIST, so it is a Web IDL §3.2.4.6
   `unsigned long` by construction, and JS_NewUint32 is that number exactly. */
#define OP_COUNT_STORE 0
#define OP_COUNT_RANGE 1

static JSValue js_idb_count_operation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_NewUint32(ctx, idb_count_records(ctx, func_data[OP_COUNT_STORE], func_data[OP_COUNT_RANGE]));
}

/* ---- §4.5's MEMBERS ARE MACHINES, BECAUSE §7.4 IS ONE ------------------------------------------------------
 *
 * `store.put(v, [1, 2])` and `store.get([1, 2])` convert an Array exotic object to a key, and THAT arm of §7.4
 * runs the page's own code — one `? HasOwnProperty` and one `? Get` per element, over a structure the page sized
 * and nested, either of which may be an accessor the page installed. A C body cannot perform it: that is the
 * drive-to-completion this engine aborts on, and C recursion over an element that is itself an array would be a
 * stack whose depth the page picks. So `add or put`, `get`, `getKey`, `delete` and `count` each declare the
 * conversion's stage block inside their own stage list, embed its record, chain its visit into their own, and
 * drive one walk — the same way §4.3's `cmp` and §4.7's five members do.
 *
 * THEY DELEGATE THROUGH TWO INTERMEDIATE ALGORITHMS, and each of those is a delegatable algorithm for the same
 * reason rather than a call that would hide the walk again: the four query members convert through §2.9's
 * convert-a-value-to-a-key-range (core/indexeddb/idb_key_range.h), and `add or put` additionally converts through
 * §7.1's extract-a-key (core/indexeddb/idb_key_path.h) for a store with in-line keys — where a LIST key path
 * assembles an Array and therefore ALWAYS reaches §7.4's array arm.
 *
 * A MEMBER'S REFUSALS AND ITS OPERANDS LIVE ON ITS STATE, because a member that suspends has no C locals left.
 * The state's `visit` is the one declaration of what it owns, so an abrupt exit at any stage frees exactly what
 * was reached — there is no `fail:` label restating the list, which is what a second list would be. */

/* ---- §4.5's ADD OR PUT ------------------------------------------------------------------------------------ */

#define OSP_STAGES(X) \
    X(OSP_BEGIN, "Indexed Database §4.5 add or put steps 1-7 — ONE O(1) engine action: transaction and store " \
                 "are the handle's, a deleted store is an InvalidStateError, an inactive transaction is a " \
                 "TransactionInactiveError, a read-only transaction is a ReadOnlyError, an in-line-key store " \
                 "given a key is a DataError, and an out-of-line-key store with no key generator and no key is " \
                 "a DataError — and step 8's conversion of the key argument begins") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, OSP_K, "Indexed Database §4.5 add or put step 8.1 (let r be the result of " \
                                        "converting a value to a key with key)") \
    X(OSP_TOOK_KEY, "Indexed Database §4.5 add or put steps 8.2-8.3 — ONE O(1) engine action: an r that is " \
                    "\"invalid value\" or \"invalid type\" is a DataError, and otherwise key is r (step 8.1's " \
                    "rethrow is the conversion's own abrupt completion and rests nowhere)") \
    X(OSP_CLONE, "Indexed Database §4.5 add or put step 10 (let clone be a clone of value in targetRealm during " \
                 "transaction; step 9's targetRealm is this member's own realm)") \
    IDB_KEY_PATH_EXTRACT_ALGO_STAGES(X, OSP_KP, "Indexed Database §4.5 add or put step 11.1 (let kpk be the " \
                                                "result of extracting a key from a value using a key path with " \
                                                "clone and store's key path)") \
    X(OSP_TOOK_KPK, "Indexed Database §4.5 add or put steps 11.2-11.4.1 — ONE O(1) engine action: an invalid " \
                    "kpk is a DataError, a kpk that is not failure becomes key, and a failure at a store with " \
                    "no key generator is a DataError") \
    X(OSP_OPERATION, "Indexed Database §4.5 add or put steps 12-13 — ONE O(1) engine action: the operation is " \
                     "an algorithm to run store a record into an object store with store, clone, key and the " \
                     "no-overwrite flag, and the request is the result of asynchronously executing it")
enum { IDL_STEP_STAGE_BASE(OSP_STAGES) OSP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OSP_STEPS[] = { OSP_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* ONE WALK RECORD FOR BOTH CONVERSIONS, and §4.5 is what makes that exact rather than merely economical: step 6
   refuses a key ARGUMENT to an in-line-key store and step 11 runs only FOR one, so the two conversions are
   mutually exclusive and the DCHECK at step 11.3 is that same fact asserted. The record would serve them in
   sequence anyway — §7.4 releases one already used at its own start, which is what §4.7's `bound` relies on.
   `kpres` is §7.1's extra state: an enum, so it rides the state's byte copy with nothing to declare. */
typedef struct {
    IdbKeyWalk       w;
    IdbKeyPathResult kpres;
    JSValue          store, tx, key, key_path, clone;
} IdbPutState;

static void js_os_put_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbPutState *s = st;

    idb_key_walk_visit(ctx, &s->w, v);
    v->val(ctx, &s->store);
    v->val(ctx, &s->tx);
    v->val(ctx, &s->key);
    v->val(ctx, &s->key_path);
    v->val(ctx, &s->clone);
}

static int js_os_put(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                     JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbPutState *s = st;
    bool key_given = argc > 1 && !JS_IsUndefined(argv[1]);

    STEP_DISPATCH(OSP_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(OSP_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->store = JS_UNDEFINED;
        s->tx = JS_UNDEFINED;
        s->key = JS_UNDEFINED;
        s->key_path = JS_UNDEFINED;
        s->clone = JS_UNDEFINED;
        if (!os_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        if (os_check(ctx, hdr->this_val, /*writes*/ true, &s->store, &s->tx) < 0)   /* STEPS 1-5 */
            return JS_STEP_ABRUPT;
        /* §2.2's key path is HELD until the in-line-key step below has walked it — "store uses in-line keys" is
           the same fact as "its key path is not null", so the read that answers step 6 is the one step 11
           needs, and it is read HERE because that is where §4.5 asks it: a member that suspends and read it
           again at step 11 would be reading the store at a time the algorithm does not name. */
        s->key_path = idb_object_store_key_path(ctx, s->store);
        /* "If store uses in-line keys and key was given, throw a DataError." */
        if (!JS_IsNull(s->key_path) && key_given) {                                 /* STEP 6 */
            JS_ThrowDOMException(ctx, "DataError",
                                 "the object store uses in-line keys, so a key may not be given to put()");
            return JS_STEP_ABRUPT;
        }
        /* "If store uses out-of-line keys and has no key generator and key was not given, throw a DataError." */
        if (JS_IsNull(s->key_path) && !key_given &&                                 /* STEP 7 */
            !idb_object_store_uses_key_generator(ctx, s->store)) {
            JS_ThrowDOMException(ctx, "DataError",
                                 "the object store uses out-of-line keys and has no key generator, so put() "
                                 "requires a key");
            return JS_STEP_ABRUPT;
        }
        /* "If key was given, then: let r be the result of converting a value to a key with key." */
        if (!key_given) {
            STEP_GOTO(hdr->stage, OSP_CLONE, &hdr->get_phase, &hdr->desc_phase, NULL);
            return JS_STEP_YIELD;
        }
        idb_key_walk_start(ctx, hdr, &s->w, argv[1], OSP_K_LENGTH, OSP_TOOK_KEY);   /* STEP 8.1 */
        return JS_STEP_YIELD;

    /* STEP 8.1's conversion, whose six rest points are this member's. Named individually for the reason
       core/dom/node.c names `clone a node`'s: a stage added to the algorithm does not compile until it has an
       arm here, where a negation or a partial list would silently route it into a neighbour. */
    STEP_ARM(OSP_K_LENGTH);
    STEP_ARM(OSP_K_BEGIN);
    STEP_ARM(OSP_K_HOP);
    STEP_ARM(OSP_K_ENTRY);
    STEP_ARM(OSP_K_SUBKEY);
    STEP_ARM(OSP_K_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, OSP_K_LENGTH, out_cb, out_argc);

    STEP_ARM(OSP_TOOK_KEY);
        JS_FreeValue(ctx, cb_result);
        if (idb_key_walk_take(ctx, &s->w, &s->key) < 0) return JS_STEP_ABRUPT;   /* STEPS 8.2-8.3 */
        STEP_GOTO(hdr->stage, OSP_CLONE, &hdr->get_phase, &hdr->desc_phase, NULL);
        return JS_STEP_YIELD;

    /* "Let clone be a clone of value in targetRealm DURING TRANSACTION. Rethrow any exceptions."
       THE CLONE IS THE MEMBER'S AND NOT THE OPERATION'S, and that placement is observable: an unclonable value
       is a "DataCloneError" thrown by `put` ITSELF, synchronously, where the page's own try/catch sees it —
       not a request that later fires an `error` event. §6.1 serializes again on the way into the record, which
       is what makes §2.3's "later changes to a value have no effect" a property of the store rather than of
       whoever called it; that second copy is of engine-owned plain data and can refuse nothing. */
    STEP_ARM(OSP_CLONE);
        JS_FreeValue(ctx, cb_result);
        s->clone = structured_clone(ctx, argv[0]);                               /* STEP 10 */
        if (JS_IsException(s->clone)) {
            s->clone = JS_UNDEFINED;
            return JS_STEP_ABRUPT;
        }
        /* "If store uses IN-LINE KEYS, then: let kpk be the result of extracting a key from a value using a key
           path with CLONE and store's key path."
           THE CLONE AND NEVER THE PAGE'S OBJECT, which is what makes §7.1's own note true ("assertions can be
           made in the above steps because this algorithm is only applied to values that are the output of
           StructuredDeserialize") — a getter on the page's object would otherwise run inside a walk that
           asserts none can. Its §7.4 conversion is still the page's structure, which is why it is a walk. */
        if (JS_IsNull(s->key_path)) {                                            /* STEP 11's condition */
            STEP_GOTO(hdr->stage, OSP_OPERATION, &hdr->get_phase, &hdr->desc_phase, NULL);
            return JS_STEP_YIELD;
        }
        idb_key_path_walk_start(ctx, hdr, &s->w, &s->kpres, s->clone, s->key_path,
                                OSP_KP_LENGTH, OSP_TOOK_KPK);                    /* STEP 11.1 */
        return JS_STEP_YIELD;

    STEP_ARM(OSP_KP_LENGTH);
    STEP_ARM(OSP_KP_BEGIN);
    STEP_ARM(OSP_KP_HOP);
    STEP_ARM(OSP_KP_ENTRY);
    STEP_ARM(OSP_KP_SUBKEY);
    STEP_ARM(OSP_KP_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, OSP_KP_LENGTH, out_cb, out_argc);

    STEP_ARM(OSP_TOOK_KPK);
    {
        JSValue kpk;

        JS_FreeValue(ctx, cb_result);
        switch (idb_key_path_walk_take(ctx, &s->w, s->kpres, &kpk)) {
        /* "If kpk is not failure, let key be kpk." The key argument is absent on this arm — §4.5 refused a
           given one for an in-line store at step 6 — so there is nothing to free. */
        case IDB_KEY_PATH_KEY:                                                   /* STEP 11.3 */
            DCHECK(JS_IsUndefined(s->key), "§4.5's in-line-key step overwrote a key the page gave — \"if store "
                                           "uses in-line keys and key was given, throw a DataError\" is "
                                           "reported before the clone, so no key can be live here");
            s->key = kpk;
            break;
        /* "If kpk is INVALID, throw a DataError." The key path resolved to something §7.4 is not a key: the
           value has a field at that address and what is there cannot be one. */
        case IDB_KEY_PATH_INVALID:                                               /* STEP 11.2 */
            JS_ThrowDOMException(ctx, "DataError",
                                 "the value at the object store's key path is not a valid key");
            return JS_STEP_ABRUPT;
        /* "Otherwise (kpk is FAILURE): if store does not have a key generator, throw a DataError." The value
           has nothing at the key path at all, and without a generator there is no key for this record to be
           filed under. WITH one there is: §6.1 generates it and §7.2 injects it into the value at that same
           key path — which is the arm §4.5's step 11.4.2 ("if check that a key could be injected into a value
           with clone and store's key path returns false, throw a DataError") guards, and it lands with §2.11
           and §7.2 where idb_database.c's crash names them. Reaching that crash is what a store with a key
           generator does on every `put`, in-line or not. */
        case IDB_KEY_PATH_FAILURE:                                               /* STEP 11.4 */
            if (!idb_object_store_uses_key_generator(ctx, s->store)) {            /* STEP 11.4.1 */
                JS_ThrowDOMException(ctx, "DataError",
                                     "the value has no value at the object store's key path, and the store "
                                     "has no key generator");
                return JS_STEP_ABRUPT;
            }
            break;
        }
        STEP_GOTO(hdr->stage, OSP_OPERATION, &hdr->get_phase, &hdr->desc_phase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(OSP_OPERATION);
    {
        JSValueConst data[4];
        JSValue op;

        JS_FreeValue(ctx, cb_result);
        os_active_across_suspension(ctx, s->tx);
        data[OP_STORE_TX] = s->tx;
        data[OP_STORE_STORE] = s->store;
        data[OP_STORE_VALUE] = s->clone;
        data[OP_STORE_KEY] = s->key;
        op = JS_NewCFunctionData(ctx, js_idb_store_operation, 0, idl_step_magic(hdr), 4, data);   /* STEP 12 */
        CHECK(!JS_IsException(op), "IndexedDB: §6.1's operation could not be minted");
        /* "Return the result (an IDBRequest) of running asynchronously execute a request with HANDLE and
           operation." The source is the HANDLE, which is what `request.source` answers with. */
        *presult = idb_request_execute(ctx, hdr->this_val, s->tx, op);                            /* STEP 13 */
        JS_FreeValue(ctx, op);
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl PUT_STEP = {
    /* No release: the walk's level stack is idb_key_walk_visit's, and the teardown discharges that one list. */
    js_os_put, sizeof(IdbPutState), js_os_put_visit, NULL,
    "Indexed Database §4.5 add or put", OSP_STEPS
};

/* ---- §4.5's DELETE and CLEAR -------------------------------------------------------------------------------- */

/* THE THREE QUERY BODIES SHARE ONE STATE and one visit, because what each holds is the same three things: §2.9's
   conversion in flight, and the store and transaction its refusals read. They do NOT share a stage list — the
   step NUMBERS differ (`delete` converts at its step 6 where `get` and `count` convert at their step 5) and a
   label names the member's own step, which is the whole point of a label. */
typedef struct { IdbRangeWalk rw; JSValue store, tx; } IdbQueryState;

static void js_os_query_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbQueryState *s = st;

    idb_key_range_walk_visit(ctx, &s->rw, v);
    v->val(ctx, &s->store);
    v->val(ctx, &s->tx);
}

/* "The delete(query) method steps are: ... let range be the result of converting a value to a key range with
   query AND TRUE. Rethrow any exceptions. Let operation be an algorithm to run delete records from an object
   store with store and range. Return the result of running asynchronously execute a request with this and
   operation."
   THE `true` IS THE NULL-DISALLOWED FLAG and §4.5 says in its own note why this member has it where `count`
   does not: "unlike other methods which take keys or key ranges, this method does not allow null to be given as
   key. This is to reduce the risk that a small bug would clear a whole object store." */
#define OSD_STAGES(X) \
    X(OSD_BEGIN, "Indexed Database §4.5 delete steps 1-5 — ONE O(1) engine action: transaction and store are " \
                 "this's, a deleted store is an InvalidStateError, an inactive transaction is a " \
                 "TransactionInactiveError and a read-only transaction is a ReadOnlyError — and step 6's " \
                 "conversion of query begins") \
    IDB_KEY_RANGE_ALGO_STAGES(X, OSD_R, "Indexed Database §4.5 delete step 6 (let range be the result of " \
                                        "converting a value to a key range with query and true)") \
    X(OSD_OPERATION, "Indexed Database §4.5 delete steps 7-8, after the O(1) tail step 6's conversion ends in " \
                     "— ONE O(1) engine action: an invalid key is that conversion's DataError, the operation is " \
                     "an algorithm to run delete records from an object store with store and range, and the " \
                     "request is the result of asynchronously executing it")
enum { IDL_STEP_STAGE_BASE(OSD_STAGES) OSD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OSD_STEPS[] = { OSD_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_os_delete(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbQueryState *s = st;

    (void)argc;
    STEP_DISPATCH(OSD_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(OSD_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->store = JS_UNDEFINED;
        s->tx = JS_UNDEFINED;
        if (!os_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        if (os_check(ctx, hdr->this_val, /*writes*/ true, &s->store, &s->tx) < 0)   /* STEPS 1-5 */
            return JS_STEP_ABRUPT;
        return idb_key_range_walk_start(ctx, hdr, &s->rw, argv[0], /*null_disallowed*/ true,
                                        OSD_R_LENGTH, OSD_OPERATION);               /* STEP 6 */

    STEP_ARM(OSD_R_LENGTH);
    STEP_ARM(OSD_R_BEGIN);
    STEP_ARM(OSD_R_HOP);
    STEP_ARM(OSD_R_ENTRY);
    STEP_ARM(OSD_R_SUBKEY);
    STEP_ARM(OSD_R_LEAVE);
        return idb_key_range_walk_run(ctx, hdr, &s->rw, cb_result, OSD_R_LENGTH, out_cb, out_argc);

    STEP_ARM(OSD_OPERATION);
    {
        JSValueConst data[3];
        JSValue range, op;

        JS_FreeValue(ctx, cb_result);
        if (idb_key_range_walk_take(ctx, &s->rw, &range) < 0) return JS_STEP_ABRUPT;
        os_active_across_suspension(ctx, s->tx);
        data[OP_DEL_TX] = s->tx;
        data[OP_DEL_STORE] = s->store;
        data[OP_DEL_RANGE] = range;
        op = JS_NewCFunctionData(ctx, js_idb_delete_operation, 0, 0, 3, data);      /* STEP 7 */
        JS_FreeValue(ctx, range);
        CHECK(!JS_IsException(op), "IndexedDB: §6.4's operation could not be minted");
        *presult = idb_request_execute(ctx, hdr->this_val, s->tx, op);              /* STEP 8 */
        JS_FreeValue(ctx, op);
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl DELETE_STEP = {
    js_os_delete, sizeof(IdbQueryState), js_os_query_visit, NULL,
    "Indexed Database §4.5 IDBObjectStore.delete", OSD_STEPS
};

/* "The clear() method steps are: ... let operation be an algorithm to run clear an object store with store.
   Return the result of running asynchronously execute a request with this and operation." The member takes no
   argument at all, so os_check's three refusals ARE the whole of its steps before the operation. */
static JSValue js_os_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst data[2];
    JSValue store = JS_UNDEFINED, tx = JS_UNDEFINED, op, req;

    (void)argc; (void)argv; (void)magic;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    if (os_check(ctx, this_val, /*writes*/ true, &store, &tx) < 0) return JS_EXCEPTION;
    data[OP_CLEAR_TX] = tx;
    data[OP_CLEAR_STORE] = store;
    op = JS_NewCFunctionData(ctx, js_idb_clear_operation, 0, 0, 2, data);
    CHECK(!JS_IsException(op), "IndexedDB: §6.6's operation could not be minted");
    req = idb_request_execute(ctx, this_val, tx, op);
    JS_FreeValue(ctx, op);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, tx);
    return req;
}

/* ---- §4.5's GET and GETKEY --------------------------------------------------------------------------------- */

#define OSG_STAGES(X) \
    X(OSG_BEGIN, "Indexed Database §4.5 get / getKey steps 1-4 — ONE O(1) engine action: transaction and store " \
                 "are this's, a deleted store is an InvalidStateError and an inactive transaction is a " \
                 "TransactionInactiveError — and step 5's conversion of query begins") \
    IDB_KEY_RANGE_ALGO_STAGES(X, OSG_R, "Indexed Database §4.5 get / getKey step 5 (let range be the result of " \
                                        "converting a value to a key range with query and true)") \
    X(OSG_OPERATION, "Indexed Database §4.5 get / getKey steps 6-7, after the O(1) tail step 5's conversion ends " \
                     "in — ONE O(1) engine action: an invalid key is that conversion's DataError, the operation " \
                     "is an algorithm to run one of §6.2's two retrievals with store and range, and the request " \
                     "is the result of asynchronously executing it")
enum { IDL_STEP_STAGE_BASE(OSG_STAGES) OSG_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OSG_STEPS[] = { OSG_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_os_get(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                     JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbQueryState *s = st;

    (void)argc;
    STEP_DISPATCH(OSG_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(OSG_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->store = JS_UNDEFINED;
        s->tx = JS_UNDEFINED;
        if (!os_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        /* NEITHER of these writes, so the read-only refusal does not apply — §4.5 puts them under a different
           heading for exactly that reason ("the following methods throw a TransactionInactiveError ..."). */
        if (os_check(ctx, hdr->this_val, /*writes*/ false, &s->store, &s->tx) < 0)   /* STEPS 1-4 */
            return JS_STEP_ABRUPT;
        /* "Let range be the result of converting a value to a key range with query AND TRUE." The `true` is the
           null-disallowed flag: `store.get(null)` is a DataError rather than a read of the whole store. */
        return idb_key_range_walk_start(ctx, hdr, &s->rw, argv[0], /*null_disallowed*/ true,
                                        OSG_R_LENGTH, OSG_OPERATION);               /* STEP 5 */

    STEP_ARM(OSG_R_LENGTH);
    STEP_ARM(OSG_R_BEGIN);
    STEP_ARM(OSG_R_HOP);
    STEP_ARM(OSG_R_ENTRY);
    STEP_ARM(OSG_R_SUBKEY);
    STEP_ARM(OSG_R_LEAVE);
        return idb_key_range_walk_run(ctx, hdr, &s->rw, cb_result, OSG_R_LENGTH, out_cb, out_argc);

    STEP_ARM(OSG_OPERATION);
    {
        JSValueConst data[2];
        JSValue range, op;

        JS_FreeValue(ctx, cb_result);
        if (idb_key_range_walk_take(ctx, &s->rw, &range) < 0) return JS_STEP_ABRUPT;
        os_active_across_suspension(ctx, s->tx);
        data[OP_GET_STORE] = s->store;
        data[OP_GET_RANGE] = range;
        op = JS_NewCFunctionData(ctx, js_idb_retrieve_operation, 0, idl_step_magic(hdr), 2, data);   /* STEP 6 */
        JS_FreeValue(ctx, range);
        CHECK(!JS_IsException(op), "IndexedDB: §6.2's operation could not be minted");
        *presult = idb_request_execute(ctx, hdr->this_val, s->tx, op);                               /* STEP 7 */
        JS_FreeValue(ctx, op);
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl GET_STEP = {
    js_os_get, sizeof(IdbQueryState), js_os_query_visit, NULL,
    "Indexed Database §4.5 IDBObjectStore.get / .getKey", OSG_STEPS
};

/* ---- §4.5's COUNT ------------------------------------------------------------------------------------------- */

/* "The count(query) method steps are: ... if store has been deleted, throw an InvalidStateError. If
   transaction's state is not active, throw a TransactionInactiveError. Let range be the result of converting a
   value to a key range with query. Rethrow any exceptions. Let operation be an algorithm to run count the
   records in a range with store and range."
   NO READ-ONLY REFUSAL AND NO NULL-DISALLOWED FLAG, and both absences are the standard's: counting reads, and
   §2.9's conversion states undefined AND null as the one unbounded key range. AN ABSENT `query` IS THAT SAME
   ANSWER and §4.5's own note is where it is written — "if null or not given, the total number of records in
   the store is counted" — so the undefined below is the standard's statement about a member called with no
   argument, not a default this consumer chose for a value the machine did not deliver. */
#define OSC_STAGES(X) \
    X(OSC_BEGIN, "Indexed Database §4.5 count steps 1-4 — ONE O(1) engine action: transaction and store are " \
                 "this's, a deleted store is an InvalidStateError and an inactive transaction is a " \
                 "TransactionInactiveError — and step 5's conversion of query begins") \
    IDB_KEY_RANGE_ALGO_STAGES(X, OSC_R, "Indexed Database §4.5 count step 5 (let range be the result of " \
                                        "converting a value to a key range with query)") \
    X(OSC_OPERATION, "Indexed Database §4.5 count steps 6-7, after the O(1) tail step 5's conversion ends in — " \
                     "ONE O(1) engine action: an invalid key is that conversion's DataError, the operation is an " \
                     "algorithm to run count the records in a range with store and range, and the request is the " \
                     "result of asynchronously executing it")
enum { IDL_STEP_STAGE_BASE(OSC_STAGES) OSC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OSC_STEPS[] = { OSC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_os_count(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbQueryState *s = st;

    STEP_DISPATCH(OSC_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(OSC_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->store = JS_UNDEFINED;
        s->tx = JS_UNDEFINED;
        if (!os_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        if (os_check(ctx, hdr->this_val, /*writes*/ false, &s->store, &s->tx) < 0)   /* STEPS 1-4 */
            return JS_STEP_ABRUPT;
        return idb_key_range_walk_start(ctx, hdr, &s->rw, argc > 0 ? argv[0] : JS_UNDEFINED,
                                        /*null_disallowed*/ false, OSC_R_LENGTH, OSC_OPERATION);   /* STEP 5 */

    STEP_ARM(OSC_R_LENGTH);
    STEP_ARM(OSC_R_BEGIN);
    STEP_ARM(OSC_R_HOP);
    STEP_ARM(OSC_R_ENTRY);
    STEP_ARM(OSC_R_SUBKEY);
    STEP_ARM(OSC_R_LEAVE);
        return idb_key_range_walk_run(ctx, hdr, &s->rw, cb_result, OSC_R_LENGTH, out_cb, out_argc);

    STEP_ARM(OSC_OPERATION);
    {
        JSValueConst data[2];
        JSValue range, op;

        JS_FreeValue(ctx, cb_result);
        if (idb_key_range_walk_take(ctx, &s->rw, &range) < 0) return JS_STEP_ABRUPT;
        os_active_across_suspension(ctx, s->tx);
        data[OP_COUNT_STORE] = s->store;
        data[OP_COUNT_RANGE] = range;
        op = JS_NewCFunctionData(ctx, js_idb_count_operation, 0, 0, 2, data);        /* STEP 6 */
        JS_FreeValue(ctx, range);
        CHECK(!JS_IsException(op), "IndexedDB: §6.5's operation could not be minted");
        *presult = idb_request_execute(ctx, hdr->this_val, s->tx, op);               /* STEP 7 */
        JS_FreeValue(ctx, op);
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl COUNT_STEP = {
    js_os_count, sizeof(IdbQueryState), js_os_query_visit, NULL,
    "Indexed Database §4.5 IDBObjectStore.count", OSC_STEPS
};

/* ---- §4.5's attributes ------------------------------------------------------------------------------------- */

/* "The name getter steps are to return this's name" — the HANDLE's, which is why it is not read off the store.
   The two agree until an upgrade transaction renames the store and aborts. */
static JSValue js_os_get_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    return idb_object_store_handle_name(ctx, this_val);
}

/* "The keyPath getter steps are to return this's object store's key path, or null if none. The key path is
   converted as a DOMString (if a string) or a sequence<DOMString> (if a list of strings), per [WEBIDL]."
   THE CONVERSION IS THE STORE'S (idb_database.h states what Web IDL §3.2.24 makes of a list, and why it is a
   plain Array and not a frozen one); WHAT IS HERE IS THE IDENTITY THE NOTE REQUIRES. "The returned value is
   not the same instance that was used when the object store was created. However, if this attribute returns an
   object (specifically an Array), it returns the same object instance every time it is inspected" — a
   requirement §3.2.24 alone does not meet, since its own steps mint a new Array per conversion. So the first
   inspection converts and the handle remembers, which is also the half of the note about the STORE: two
   handles for one store answer with two Arrays, because the cache is the handle's. */
static JSValue js_os_get_key_path(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue store, kp, slots;

    (void)magic;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    slots = os_slots(ctx, this_val);
    DCHECK(JS_IsObject(slots), "an IDBObjectStore carried no slot record");
    kp = JS_GetPropertyStr(ctx, slots, OS_KEY_PATH);
    if (!JS_IsUndefined(kp)) {   /* the positive statement "inspected before" — see OS_KEY_PATH */
        JS_FreeValue(ctx, slots);
        return kp;
    }
    JS_FreeValue(ctx, kp);
    store = os_get(ctx, this_val, OS_STORE);
    kp = idb_object_store_key_path_value(ctx, store);
    JS_FreeValue(ctx, store);
    DCHECK(!JS_IsUndefined(kp), "§4.5's keyPath conversion answered undefined, which is the one value this "
                                "cache reads as \"not converted yet\" — the conversion answers null for a "
                                "store with out-of-line keys, a string, or an Array");
    JS_SetPropertyStr(ctx, slots, OS_KEY_PATH, JS_DupValue(ctx, kp));
    JS_FreeValue(ctx, slots);
    return kp;
}

/* "The transaction getter steps are to return this's transaction." `[SameObject]`, which holds because the
   handle stores the transaction rather than deriving it. */
static JSValue js_os_get_transaction(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    return os_get(ctx, this_val, OS_TRANSACTION);
}

/* "The autoIncrement getter steps are to return true if this's object store has a key generator, and false
   otherwise." */
static JSValue js_os_get_auto_increment(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue store;
    bool b;

    (void)magic;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    store = os_get(ctx, this_val, OS_STORE);
    b = idb_object_store_uses_key_generator(ctx, store);
    JS_FreeValue(ctx, store);
    return JS_NewBool(ctx, b);
}

/* "The name setter steps are: ... if store has been deleted, throw an InvalidStateError. If transaction is not
   an upgrade transaction, throw an InvalidStateError. If transaction's state is not active, throw a
   TransactionInactiveError. If store's name is equal to name, terminate these steps. If an object store named
   name already exists in store's database, throw a ConstraintError. Set store's name to name. Set this's name
   to name."
   BOTH NAMES ARE WRITTEN, and they are two different fields — the store's, which is where §2.2's set keys it,
   and the handle's, which is what the getter above answers and what §5.8 reverts. */
static JSValue js_os_set_name(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue store, tx, db, conn, old, existing, stores;
    const char *name;
    int same;

    (void)magic;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    store = os_get(ctx, this_val, OS_STORE);
    tx = os_get(ctx, this_val, OS_TRANSACTION);
    DCHECK(JS_IsObject(store) && idb_transaction_is(tx), "an object store handle carried no store or no "
                                                         "transaction");
    if (idb_object_store_is_deleted(ctx, store)) {
        JS_ThrowDOMException(ctx, "InvalidStateError", "the object store has been deleted");
        goto fail;
    }
    if (idb_transaction_mode(ctx, tx) != IDB_TX_VERSIONCHANGE) {
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "an object store can only be renamed within an upgrade transaction");
        goto fail;
    }
    if (idb_transaction_state(ctx, tx) != IDB_TX_ACTIVE) {
        JS_ThrowDOMException(ctx, "TransactionInactiveError", "the upgrade transaction is not active");
        goto fail;
    }
    name = JS_ToCString(ctx, val);
    if (name == NULL) goto fail;
    old = idb_object_store_name(ctx, store);
    {
        const char *cold = JS_ToCString(ctx, old);

        CHECK(cold != NULL, "IndexedDB: a store could not report its own name");
        same = strcmp(cold, name) == 0;
        JS_FreeCString(ctx, cold);
    }
    JS_FreeValue(ctx, old);
    if (same) {                       /* "If store's name is equal to name, terminate these steps." */
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, store);
        JS_FreeValue(ctx, tx);
        return JS_UNDEFINED;
    }
    conn = idb_transaction_connection(ctx, tx);
    db = idb_connection_database(ctx, conn);
    JS_FreeValue(ctx, conn);
    stores = idb_database_store_set(ctx, db);
    existing = JS_GetPropertyStr(ctx, stores, name);
    JS_FreeValue(ctx, stores);
    if (!JS_IsUndefined(existing)) {
        JS_FreeValue(ctx, existing);
        JS_FreeValue(ctx, db);
        JS_FreeCString(ctx, name);
        JS_ThrowDOMException(ctx, "ConstraintError",
                             "an object store with that name already exists in the database");
        goto fail;
    }
    JS_FreeValue(ctx, existing);
    idb_object_store_rename(ctx, tx, db, store, name);
    JS_FreeValue(ctx, db);
    {
        JSValue slots = os_slots(ctx, this_val);

        JS_SetPropertyStr(ctx, slots, OS_NAME, JS_NewString(ctx, name));
        JS_FreeValue(ctx, slots);
    }
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, tx);
    return JS_UNDEFINED;

fail:
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, tx);
    return JS_EXCEPTION;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static void idb_object_store_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_os_class != 0, "a realm asked for IDBObjectStore.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_os_class);
    DCHECK(JS_IsNull(prev), "idb_object_store_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBObjectStore.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBObjectStore");
    idl_install_accessor(ctx, proto, "name", js_os_get_name, 0, g_setter_name);
    idl_install_accessor(ctx, proto, "keyPath", js_os_get_key_path, 0, -1);
    idl_install_accessor(ctx, proto, "transaction", js_os_get_transaction, 0, -1);
    idl_install_accessor(ctx, proto, "autoIncrement", js_os_get_auto_increment, 0, -1);
    /* IN §4.5'S OWN ORDER, minus the members whose algorithms do not exist. The `length` of each is Web IDL
       §3.7.7 Operations' — "the length of the shortest argument list in the entries in S", which for a member
       with no overloads is the number of REQUIRED arguments — so `count`'s is 0 and `delete`'s is 1. */
    idl_install_method(ctx, proto, "put", 1, g_id_put);
    idl_install_method(ctx, proto, "add", 1, g_id_add);
    idl_install_method(ctx, proto, "delete", 1, g_id_delete);
    idl_install_method(ctx, proto, "clear", 0, g_id_clear);
    idl_install_method(ctx, proto, "get", 1, g_id_get);
    idl_install_method(ctx, proto, "getKey", 1, g_id_get_key);
    idl_install_method(ctx, proto, "count", 0, g_id_count);
    JS_SetClassProto(ctx, g_os_class, JS_DupValue(ctx, proto));

    ctor = idl_interface_object(ctx, "IDBObjectStore", proto);
    CHECK(!JS_IsException(ctor), "the IDBObjectStore interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "IDBObjectStore", ctor);
    JS_FreeValue(ctx, global);
}

void idb_object_store_init(JSContext *ctx)
{
    JSClassDef d = { "IDBObjectStore" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* `put(any value, optional any key)` and `get(any query)` — every position is `any`, which is what makes
       §7.4's convert-a-value-to-a-key and §2.9's convert-a-value-to-a-key-range the members' OWN steps: the
       IDL hands the value through unconverted and the algorithm decides what it is. */
    static const IdlArgType PUT_ARGS[2] = { IDL_ANY, IDL_ANY };
    /* `get(any query)`, `getKey(any query)`, `delete(any query)` and `count(optional any query)` are one
       declared type list; what tells them apart is which BODY runs and, for `count`, that its one position is
       optional. */
    static const IdlArgType QUERY_ARGS[1] = { IDL_ANY };

    DCHECK(!g_ready, "idb_object_store_init ran twice — one instance is one document is one agent");
    g_key = JS_NewSymbol(ctx, "idbObjectStoreState", false);
    CHECK(!JS_IsException(g_key), "the IDBObjectStore slot key allocation failed");
    g_os_rt = rt;
    JS_NewClassID(rt, &g_os_class);
    CHECK(JS_NewClass(rt, g_os_class, &d) == 0,
          "IDBObjectStore: the per-realm prototype slot could not be declared");
    /* ONE BODY per pair and one DECLARATION per member: §4.5 states `put` and `add` as one algorithm
       differing in a flag, and `get` and `getKey` as one shape over §6.2's two operations — so the flag is the
       declaration's MAGIC, which is also what the operation closure is minted with, and the body is written
       once. A declaration cannot be shared between the two members of a pair, because the magic IS the
       difference and it belongs to the declaration. */
    g_id_put = idl_method_id_step(ctx, PUT_ARGS, 2, NULL, 0, &PUT_STEP, OS_PUT);
    idl_optional_from(1);                        /* `optional any key` */
    g_id_add = idl_method_id_step(ctx, PUT_ARGS, 2, NULL, 0, &PUT_STEP, OS_ADD);
    idl_optional_from(1);
    g_id_get = idl_method_id_step(ctx, QUERY_ARGS, 1, NULL, 0, &GET_STEP, OS_GET_VALUE);
    g_id_get_key = idl_method_id_step(ctx, QUERY_ARGS, 1, NULL, 0, &GET_STEP, OS_GET_KEY);
    /* §6.4 and §6.6 are two algorithms and not one with a flag, so neither takes a magic — unlike the two
       pairs above, whose magic IS the difference the standard states. */
    g_id_delete = idl_method_id_step(ctx, QUERY_ARGS, 1, NULL, 0, &DELETE_STEP, 0);
    g_id_clear = idl_method_id(ctx, NULL, 0, js_os_clear, 0);
    g_id_count = idl_method_id_step(ctx, QUERY_ARGS, 1, NULL, 0, &COUNT_STEP, 0);
    idl_optional_from(0);                        /* `count(optional any query)` */
    g_setter_name = idl_setter_id(ctx, IDL_DOMSTRING, /*null_to_empty*/ false, js_os_set_name, 0);
    g_ready = 1;
    agent_state_flag("idb_object_store", &g_ready, "the declaration latch");
    agent_state_ptr("idb_object_store", &g_os_rt, "the runtime §2.2.1's slot key was minted in");
    agent_state_value("idb_object_store", &g_key, "§2.2.1's internal-slot key");
    realm_declare_intrinsic(idb_object_store_install_realm);
}

void idb_object_store_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — see idb_transaction_free. */
    DCHECK(g_ready, "§2.2.1's object-store machinery was released in an agent that never declared it");
    DCHECK(rt == g_os_rt, "idb_object_store_free was given a runtime that is not the one it declared into");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_os_rt = NULL;
    g_ready = 0;
}
