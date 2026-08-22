/* INDEXED DATABASE §2.6.1's INDEX HANDLE and §4.6's IDBIndex over it — the members a page reaches §6.3's two
 * retrievals and §6.5's count over an index through.
 *
 * WHY A HANDLE EXISTS AT ALL is §2.2.1's answer one level down: "script does not interact with indexes directly.
 * Instead, within a transaction, script has indirect access via an index handle." Every member below reads the
 * TRANSACTION first, because that is where §4.6 puts every one of its refusals — and the transaction is the
 * OBJECT STORE HANDLE's, which §2.6.1 states outright ("the transaction of an index handle is the transaction of
 * its associated object store handle"). So this file stores no transaction: it asks the store handle, and one
 * answer cannot disagree with itself.
 *
 * NOTHING OF §4.6 IS ABSENT ANY MORE, and this paragraph used to be the list of what was. It named two
 * constructs and both have since been built — §2.10 Cursor with §6.7 Cursor iteration for
 * `openCursor`/`openKeyCursor`, and §5.12 Creating a request to retrieve multiple items with §6.3's
 * retrieve-multiple-items-from-an-index and §4.8's IDBRecord for `getAll`/`getAllKeys`/`getAllRecords` — so
 * every member the interface declares is installed and answers with a computed value. The list is kept as
 * this sentence rather than deleted, because the next member added to this IDL needs somewhere to say what it
 * is waiting on, and because a paragraph naming absences that no longer exist sends its reader to build
 * something twice.
 *
 * THE MEMBERS THAT ARE HERE ARE MACHINES for §4.5's reason: `index.get([1, 2])` converts an Array exotic object
 * through §2.9's convert-a-value-to-a-key-range, whose step 3 is §7.4's array arm — the page's own code. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_cursor.h"
#include "core/indexeddb/idb_get_all.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_index.h"
#include "core/indexeddb/idb_index_handle.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/indexeddb/idb_object_store.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/realm.h"

/* §2.6.1's fields, spelled once. */
#define IX_INDEX   "index"          /* the §2.6 index this handle is a view of */
#define IX_STORE   "storeHandle"    /* §2.6.1's associated object store handle — and its transaction */
#define IX_NAME    "name"           /* §2.6.1's own name, "initialized to the name of the associated index" */
/* §4.6's keyPath getter answers with ONE object per handle — the same note §4.5's carries — so the converted
   value is remembered on its FIRST inspection. JS_UNDEFINED is the positive statement "not inspected yet". */
#define IX_KEY_PATH "keyPathValue"

static JSValue   g_key;
static JSClassID g_ix_class;
static int       g_ready;
static JSRuntime *g_ix_rt;
static int g_id_get = -1, g_id_get_key = -1, g_id_count = -1, g_setter_name = -1;
static int g_id_open_cursor = -1, g_id_open_key_cursor = -1;
static int g_id_get_all = -1, g_id_get_all_keys = -1, g_id_get_all_records = -1;
/* §4.6's `openCursor` and `openKeyCursor` are one algorithm differing in the KEY ONLY FLAG the cursor is
   created with — the same magic §4.5's pair carries, for the same reason. */
enum { IX_WITH_VALUE = 0, IX_KEY_ONLY = 1 };

/* §6.3's two retrievals, which §4.6 states as two members over one shape. */
enum { IX_GET_REFERENCED = 0, IX_GET_KEY = 1 };

/* ---- the record ---------------------------------------------------------------------------------------- */

static JSValue ix_slots(JSContext *ctx, JSValueConst h)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an IDBIndex slot was asked for before its interface was declared");
    if (!JS_IsObject(h))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBIndex slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &st, h, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

bool idb_index_handle_is(JSValueConst v)
{
    return g_ix_class != 0 && JS_GetClassID(v) == g_ix_class;
}

static bool ix_brand(JSContext *ctx, JSValueConst this_val)
{
    if (idb_index_handle_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "an IDBIndex member was reached on something that is not an IDBIndex");
    return false;
}

static JSValue ix_get(JSContext *ctx, JSValueConst h, const char *field)
{
    JSValue slots = ix_slots(ctx, h), v;

    DCHECK(JS_IsObject(slots), "an IDBIndex field was read off a value carrying no slot record");
    v = JS_GetPropertyStr(ctx, slots, field);
    JS_FreeValue(ctx, slots);
    return v;
}

JSValue idb_index_handle(JSContext *ctx, JSValueConst index, JSValueConst store_handle)
{
    JSValue h, st, proto;
    JSAtom k;

    DCHECK(g_ready, "an index handle was created before idb_index_handle_init declared the interface");
    DCHECK(JS_IsObject(index), "an index handle was created over something that is not a §2.6 index");
    DCHECK(idb_object_store_is(store_handle),
           "an index handle was created with no OBJECT STORE HANDLE — §2.6.1 gives every handle one, and it is "
           "what §4.6's `objectStore` answers with and what every member asks its transaction of");
    proto = JS_GetClassProto(ctx, g_ix_class);
    DCHECK(!JS_IsNull(proto), "IDBIndex.prototype was asked for in a realm that never ran its install");
    h = JS_NewObjectProtoClass(ctx, proto, g_ix_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(h), "IndexedDB: the IDBIndex allocation failed");
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "IndexedDB: the IDBIndex slot record allocation failed");
    JS_SetPropertyStr(ctx, st, IX_INDEX, JS_DupValue(ctx, index));
    JS_SetPropertyStr(ctx, st, IX_STORE, JS_DupValue(ctx, store_handle));
    /* "A name, which is initialized to the name of the associated index when the index handle is created." The
       handle's OWN copy — which is what §5.8 step 6 reverts, and what makes `index.name` go on answering the
       old name after an aborted rename. */
    JS_SetPropertyStr(ctx, st, IX_NAME, idb_index_name(ctx, index));
    JS_SetPropertyStr(ctx, st, IX_KEY_PATH, JS_UNDEFINED);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBIndex slot key could not be interned");
    JS_SetProperty(ctx, h, k, st);
    JS_FreeAtom(ctx, k);
    return h;
}

JSValue idb_index_handle_index(JSContext *ctx, JSValueConst handle)
{
    JSValue index;

    DCHECK(idb_index_handle_is(handle), "the associated index of something that is not an index handle was "
                                        "asked for");
    index = ix_get(ctx, handle, IX_INDEX);
    DCHECK(JS_IsObject(index), "an index handle carried no index — idb_index_handle gives every handle one");
    return index;
}

JSValue idb_index_handle_name(JSContext *ctx, JSValueConst handle)
{
    JSValue name;

    DCHECK(idb_index_handle_is(handle), "the name of something that is not an index handle was asked for");
    name = ix_get(ctx, handle, IX_NAME);
    DCHECK(JS_IsString(name), "an index handle carried no name — §2.6.1 initialises one when the handle is "
                              "created, and only this file writes it");
    return name;
}

/* §2.6.1's "the transaction of an index handle is the transaction of its associated object store handle" —
   asked and not stored, so there is one answer. OWNED. */
static JSValue ix_transaction(JSContext *ctx, JSValueConst handle)
{
    JSValue sh = ix_get(ctx, handle, IX_STORE), tx;

    DCHECK(idb_object_store_is(sh), "an index handle carried no object store handle");
    tx = idb_object_store_handle_transaction(ctx, sh);
    JS_FreeValue(ctx, sh);
    return tx;
}

void idb_index_handle_restore_name(JSContext *ctx, JSValueConst handle)
{
    JSValue index = idb_index_handle_index(ctx, handle), slots = ix_slots(ctx, handle);

    DCHECK(JS_IsObject(slots), "an index handle's name was restored on a value carrying no slot record");
    JS_SetPropertyStr(ctx, slots, IX_NAME, idb_index_name(ctx, index));
    JS_FreeValue(ctx, slots);
    JS_FreeValue(ctx, index);
}

/* ---- the refusals every member of §4.6 begins with -------------------------------------------------------
 *
 * §4.6 states them once per member and states them IDENTICALLY, in this order: "if index or index's OBJECT
 * STORE has been deleted, throw an InvalidStateError"; "if transaction's state is not active, then throw a
 * TransactionInactiveError". THE OBJECT STORE'S HALF IS NOT REDUNDANT — §4.4's deleteObjectStore destroys the
 * store and leaves the indexes that reference it alone, so an index handle can outlive its store's deletion
 * with its own deleted flag still false. Returns -1 with the throw live; `*pindex` is OWNED on success. */
static int ix_check(JSContext *ctx, JSValueConst h, JSValue *pindex, JSValue *ptx)
{
    JSValue index = idb_index_handle_index(ctx, h), tx = ix_transaction(ctx, h), store;
    bool deleted;

    store = idb_index_store(ctx, index);
    deleted = idb_index_is_deleted(ctx, index) || idb_object_store_is_deleted(ctx, store);
    JS_FreeValue(ctx, store);
    if (deleted) {
        JS_FreeValue(ctx, index);
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "InvalidStateError", "the index, or the object store it references, has been "
                                                       "deleted");
        return -1;
    }
    if (idb_transaction_state(ctx, tx) != IDB_TX_ACTIVE) {
        JS_FreeValue(ctx, index);
        JS_FreeValue(ctx, tx);
        JS_ThrowDOMException(ctx, "TransactionInactiveError",
                             "the transaction the index belongs to is not active");
        return -1;
    }
    *pindex = index;
    *ptx = tx;
    return 0;
}

/* ---- §5.6's OPERATIONS ------------------------------------------------------------------------------------
 *
 * NONE of these runs the page's code: §6.3's deserialization is over a value this engine stored, and the count
 * is over this engine's own list — so they are C bodies over their captured operands, like §6.2's and §6.5's.
 * Each carries the INDEX and not the handle, for §scheduler's reason: a work item takes its inputs with it. */
#define OP_IX_INDEX 0
#define OP_IX_RANGE 1

static JSValue js_idb_index_retrieve_operation(JSContext *ctx, JSValueConst this_val, int argc,
                                               JSValueConst *argv, int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv;
    if (magic == IX_GET_KEY)
        return idb_index_retrieve_value(ctx, func_data[OP_IX_INDEX], func_data[OP_IX_RANGE]);
    return idb_index_retrieve_referenced_value(ctx, func_data[OP_IX_INDEX], func_data[OP_IX_RANGE]);
}

static JSValue js_idb_index_count_operation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                            int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_NewUint32(ctx, idb_index_count_records(ctx, func_data[OP_IX_INDEX], func_data[OP_IX_RANGE]));
}

/* ---- §4.6's GET, GETKEY and COUNT -------------------------------------------------------------------------
 *
 * THE THREE SHARE ONE STATE and one visit, because what each holds is the same three things: §2.9's conversion
 * in flight, and the index and transaction its refusals read. They do NOT share a stage list — a label names
 * the member's own step, which is the whole point of a label. */
typedef struct { IdbRangeWalk rw; JSValue index, tx; } IdbIndexQueryState;

static void js_ix_query_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbIndexQueryState *s = st;

    idb_key_range_walk_visit(ctx, &s->rw, v);
    v->val(ctx, &s->index);
    v->val(ctx, &s->tx);
}

#define IXG_STAGES(X) \
    X(IXG_BEGIN, "Indexed Database §4.6 get / getKey steps 1-4 — ONE O(1) engine action: transaction and index " \
                 "are this's, a deleted index or object store is an InvalidStateError and an inactive " \
                 "transaction is a TransactionInactiveError — and step 5's conversion of query begins") \
    IDB_KEY_RANGE_ALGO_STAGES(X, IXG_R, "Indexed Database §4.6 get / getKey step 5 (let range be the result of " \
                                        "converting a value to a key range with query and true)") \
    X(IXG_OPERATION, "Indexed Database §4.6 get / getKey steps 6-7, after the O(1) tail step 5's conversion " \
                     "ends in — ONE O(1) engine action: an invalid key is that conversion's DataError, the " \
                     "operation is an algorithm to run one of §6.3's two retrievals with index and range, and " \
                     "the request is the result of asynchronously executing it")
enum { IDL_STEP_STAGE_BASE(IXG_STAGES) IXG_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IXG_STEPS[] = { IXG_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_ix_get(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                     JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbIndexQueryState *s = st;

    (void)argc;
    STEP_DISPATCH(IXG_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(IXG_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->index = JS_UNDEFINED;
        s->tx = JS_UNDEFINED;
        if (!ix_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        if (ix_check(ctx, hdr->this_val, &s->index, &s->tx) < 0)                     /* STEPS 1-4 */
            return JS_STEP_ABRUPT;
        /* "Let range be the result of converting a value to a key range with query AND TRUE" — the
           null-disallowed flag, so `index.get(null)` is a DataError rather than a read of the whole index. */
        return idb_key_range_walk_start(ctx, hdr, &s->rw, argv[0], /*null_disallowed*/ true,
                                        IXG_R_LENGTH, IXG_OPERATION);                /* STEP 5 */

    STEP_ARM(IXG_R_LENGTH);
    STEP_ARM(IXG_R_BEGIN);
    STEP_ARM(IXG_R_HOP);
    STEP_ARM(IXG_R_ENTRY);
    STEP_ARM(IXG_R_SUBKEY);
    STEP_ARM(IXG_R_LEAVE);
        return idb_key_range_walk_run(ctx, hdr, &s->rw, cb_result, IXG_R_LENGTH, out_cb, out_argc);

    STEP_ARM(IXG_OPERATION);
    {
        JSValueConst data[2];
        JSValue range, op;

        JS_FreeValue(ctx, cb_result);
        if (idb_key_range_walk_take(ctx, &s->rw, &range) < 0) return JS_STEP_ABRUPT;
        DCHECK(idb_transaction_state(ctx, s->tx) == IDB_TX_ACTIVE,
               "§4.6's member had its transaction deactivated between its own state check and the step that "
               "places the request — the member suspended in between (§7.4's array arm), and nothing may "
               "deactivate a transaction across that: §2.7.1's cleanup is a checkpoint of the event loop");
        data[OP_IX_INDEX] = s->index;
        data[OP_IX_RANGE] = range;
        op = JS_NewCFunctionData(ctx, js_idb_index_retrieve_operation, 0, idl_step_magic(hdr), 2, data);
        JS_FreeValue(ctx, range);
        CHECK(!JS_IsException(op), "IndexedDB: §6.3's operation could not be minted");
        /* "Return the result (an IDBRequest) of running asynchronously execute a request with THIS and
           operation." The source is the index handle, which is what `request.source` answers with. */
        *presult = idb_request_execute(ctx, hdr->this_val, s->tx, op);                /* STEP 7 */
        JS_FreeValue(ctx, op);
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl IX_GET_STEP = {
    js_ix_get, sizeof(IdbIndexQueryState), js_ix_query_visit, NULL,
    "Indexed Database §4.6 IDBIndex.get / .getKey", IXG_STEPS
};

/* ---- §4.6's GETALL, GETALLKEYS and GETALLRECORDS ------------------------------------------------------------
 *
 * "The getAll(queryOrOptions, count) method steps are: return the result of CREATING A REQUEST TO RETRIEVE
 * MULTIPLE ITEMS with the current Realm record, this, "value", queryOrOptions, and count if given. Rethrow any
 * exceptions." §4.6 states the three exactly as §4.5 does, so this is §4.5's body with two operands changed:
 * the refusals are `ix_check`'s (which ALSO refuses when the index's referenced object store was deleted —
 * §4.6 states that and §4.5 does not), and the source is an INDEX, which is what sends §5.12 step 10 to §6.3's
 * retrieve-multiple rather than §6.2's.
 *
 * The two files hold two bodies and not one shared one for the reason every other member pair here does: the
 * refusal set is the interface's, and a body that took a "which handle am I" flag would be a recognizer
 * standing where two interfaces' own steps belong. What IS shared is §5.12 itself, which is
 * core/indexeddb/idb_get_all.h's block — the algorithm the standard writes once. */
#define IXGA_STAGES(X) \
    X(IXGA_BEGIN, "Indexed Database §4.6 getAll / getAllKeys / getAllRecords, which is §5.12 steps 1-5 — ONE " \
                  "O(1) engine action: Web IDL §3.2.4.7's [EnforceRange] range test on the positional count " \
                  "(which precedes the member's own steps, because an argument is converted first), then " \
                  "source and transaction are this's, a deleted index or object store is an " \
                  "InvalidStateError and an inactive transaction is a TransactionInactiveError — and §5.12 " \
                  "steps 6-9 begin") \
    IDB_GET_ALL_ALGO_STAGES(X, IXGA_R, "Indexed Database §4.6 getAll / getAllKeys / getAllRecords") \
    X(IXGA_REQUEST, "Indexed Database §5.12 steps 10-11, after the O(1) tail steps 8-9's conversion ends in — " \
                    "ONE O(1) engine action: an invalid key is that conversion's DataError, the operation is " \
                    "an algorithm to run retrieve multiple items from an index, and the request is the result " \
                    "of asynchronously executing it")
enum { IDL_STEP_STAGE_BASE(IXGA_STAGES) IXGA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IXGA_STEPS[] = { IXGA_STAGES(JS_STEP_STAGE_LABEL) NULL };

static void js_ix_get_all_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    idb_get_all_walk_visit(ctx, st, v);
}

static int js_ix_get_all(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                         JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbGetAllWalk *w = st;

    STEP_DISPATCH(IXGA_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(IXGA_BEGIN);
    {
        JSValue index = JS_UNDEFINED, tx = JS_UNDEFINED;
        uint32_t count = 0;
        /* `optional [EnforceRange] unsigned long count` — absent when the page passed nothing or undefined,
           which is §5.12's "count IF GIVEN". `getAllRecords` declares no second argument at all. */
        bool has_count = argc > 1 && !JS_IsUndefined(argv[1]);
        int r;

        JS_FreeValue(ctx, cb_result);
        if (!ix_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        /* WEB IDL CONVERTS AN ARGUMENT BEFORE THE OPERATION'S OWN STEPS, so [EnforceRange]'s refusal precedes
           §5.12 step 2's: `deletedIndex.getAll(q, -1)` is a TypeError and not an "InvalidStateError". */
        if (has_count && idb_get_all_count_enforce_range(ctx, argv[1], &count) < 0)
            return JS_STEP_ABRUPT;
        if (ix_check(ctx, hdr->this_val, &index, &tx) < 0)                            /* §5.12 STEPS 1-5 */
            return JS_STEP_ABRUPT;
        r = idb_get_all_walk_start(ctx, hdr, w, index, tx, /*is_index*/ true, idl_step_magic(hdr),
                                   argv[0], idl_step_magic(hdr) == IDB_GET_ALL_RECORD,
                                   count, has_count, IXGA_R_LENGTH, IXGA_REQUEST);   /* §5.12 STEPS 6-9 */
        JS_FreeValue(ctx, index);
        JS_FreeValue(ctx, tx);
        return r;
    }

    STEP_ARM(IXGA_R_LENGTH);
    STEP_ARM(IXGA_R_BEGIN);
    STEP_ARM(IXGA_R_HOP);
    STEP_ARM(IXGA_R_ENTRY);
    STEP_ARM(IXGA_R_SUBKEY);
    STEP_ARM(IXGA_R_LEAVE);
        return idb_get_all_walk_run(ctx, hdr, w, cb_result, IXGA_R_LENGTH, out_cb, out_argc);

    STEP_ARM(IXGA_REQUEST);
        JS_FreeValue(ctx, cb_result);
        *presult = idb_get_all_walk_take(ctx, w, hdr->this_val);                     /* §5.12 STEPS 10-11 */
        return JS_IsException(*presult) ? JS_STEP_ABRUPT : JS_STEP_DONE;
}

static const IdlStepDecl IX_GET_ALL_STEP = {
    js_ix_get_all, sizeof(IdbGetAllWalk), js_ix_get_all_visit, NULL,
    "Indexed Database §4.6 IDBIndex.getAll / .getAllKeys / .getAllRecords", IXGA_STEPS
};

/* "The count(query) method steps are: ... let range be the result of converting a value to a key range with
   query. Rethrow any exceptions. Let operation be an algorithm to run COUNT THE RECORDS IN A RANGE with index
   and range."
   NO NULL-DISALLOWED FLAG, which is the standard's own difference from `get`: §4.6's note is "if null or not
   given, an unbounded key range is used", so the undefined below is that sentence about a member called with
   no argument rather than a default this consumer chose. */
#define IXC_STAGES(X) \
    X(IXC_BEGIN, "Indexed Database §4.6 count steps 1-4 — ONE O(1) engine action: transaction and index are " \
                 "this's, a deleted index or object store is an InvalidStateError and an inactive transaction " \
                 "is a TransactionInactiveError — and step 5's conversion of query begins") \
    IDB_KEY_RANGE_ALGO_STAGES(X, IXC_R, "Indexed Database §4.6 count step 5 (let range be the result of " \
                                        "converting a value to a key range with query)") \
    X(IXC_OPERATION, "Indexed Database §4.6 count steps 6-7, after the O(1) tail step 5's conversion ends in — " \
                     "ONE O(1) engine action: an invalid key is that conversion's DataError, the operation is " \
                     "an algorithm to run count the records in a range with index and range, and the request " \
                     "is the result of asynchronously executing it")
enum { IDL_STEP_STAGE_BASE(IXC_STAGES) IXC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IXC_STEPS[] = { IXC_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_ix_count(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbIndexQueryState *s = st;

    STEP_DISPATCH(IXC_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(IXC_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->index = JS_UNDEFINED;
        s->tx = JS_UNDEFINED;
        if (!ix_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        if (ix_check(ctx, hdr->this_val, &s->index, &s->tx) < 0)                     /* STEPS 1-4 */
            return JS_STEP_ABRUPT;
        return idb_key_range_walk_start(ctx, hdr, &s->rw, argc > 0 ? argv[0] : JS_UNDEFINED,
                                        /*null_disallowed*/ false, IXC_R_LENGTH, IXC_OPERATION);   /* STEP 5 */

    STEP_ARM(IXC_R_LENGTH);
    STEP_ARM(IXC_R_BEGIN);
    STEP_ARM(IXC_R_HOP);
    STEP_ARM(IXC_R_ENTRY);
    STEP_ARM(IXC_R_SUBKEY);
    STEP_ARM(IXC_R_LEAVE);
        return idb_key_range_walk_run(ctx, hdr, &s->rw, cb_result, IXC_R_LENGTH, out_cb, out_argc);

    STEP_ARM(IXC_OPERATION);
    {
        JSValueConst data[2];
        JSValue range, op;

        JS_FreeValue(ctx, cb_result);
        if (idb_key_range_walk_take(ctx, &s->rw, &range) < 0) return JS_STEP_ABRUPT;
        DCHECK(idb_transaction_state(ctx, s->tx) == IDB_TX_ACTIVE,
               "§4.6's `count` had its transaction deactivated between its own state check and the step that "
               "places the request");
        data[OP_IX_INDEX] = s->index;
        data[OP_IX_RANGE] = range;
        op = JS_NewCFunctionData(ctx, js_idb_index_count_operation, 0, 0, 2, data);   /* STEP 6 */
        JS_FreeValue(ctx, range);
        CHECK(!JS_IsException(op), "IndexedDB: §6.5's operation over an index could not be minted");
        *presult = idb_request_execute(ctx, hdr->this_val, s->tx, op);                /* STEP 7 */
        JS_FreeValue(ctx, op);
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl IX_COUNT_STEP = {
    js_ix_count, sizeof(IdbIndexQueryState), js_ix_query_visit, NULL,
    "Indexed Database §4.6 IDBIndex.count", IXC_STEPS
};

/* ---- §4.6's OPENCURSOR and OPENKEYCURSOR ------------------------------------------------------------------
 *
 * THE SAME SHAPE §4.5's PAIR HAS, over an INDEX source, and the difference is entirely inside §6.7: a cursor
 * whose source is an index walks a list sorted primarily on the index key and SECONDARILY on the record's
 * value, carries §2.10's OBJECT STORE POSITION beside its position, and answers `primaryKey` with that second
 * position rather than with its key. None of that is stated here — it is §2.10's and §6.7's, so this member is
 * the same ten steps with `this` being an index handle.
 *
 * `openCursor`'s cursor yields the REFERENCED VALUE (§6.7 step 13.1's "found record's referenced value"), which
 * is what makes an index cursor a view of the store rather than of the index; `openKeyCursor`'s key only flag
 * suppresses that lookup entirely, which is the whole cost difference between the two. */
#define IXCU_STAGES(X) \
    X(IXCU_BEGIN, "Indexed Database §4.6 openCursor / openKeyCursor steps 1-4 — ONE O(1) engine action: " \
                  "transaction and index are this's, a deleted index or object store is an InvalidStateError " \
                  "and an inactive transaction is a TransactionInactiveError — and step 5's conversion of " \
                  "query begins") \
    IDB_KEY_RANGE_ALGO_STAGES(X, IXCU_R, "Indexed Database §4.6 openCursor / openKeyCursor step 5 (let range " \
                                         "be the result of converting a value to a key range with query)") \
    X(IXCU_OPERATION, "Indexed Database §4.6 openCursor / openKeyCursor steps 6-10, after the O(1) tail step " \
                      "5's conversion ends in — ONE O(1) engine action: an invalid key is that conversion's " \
                      "DataError, a new cursor is created over range with this as its source handle, the " \
                      "operation is an algorithm to run iterate a cursor with it, the request is the result " \
                      "of asynchronously executing that operation, and the cursor's request is set to it")
enum { IDL_STEP_STAGE_BASE(IXCU_STAGES) IXCU_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IXCU_STEPS[] = { IXCU_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_ix_open_cursor(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbIndexQueryState *s = st;

    STEP_DISPATCH(IXCU_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(IXCU_BEGIN);
        JS_FreeValue(ctx, cb_result);
        s->index = JS_UNDEFINED;
        s->tx = JS_UNDEFINED;
        if (!ix_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        if (ix_check(ctx, hdr->this_val, &s->index, &s->tx) < 0)                      /* STEPS 1-4 */
            return JS_STEP_ABRUPT;
        return idb_key_range_walk_start(ctx, hdr, &s->rw, argc > 0 ? argv[0] : JS_UNDEFINED,
                                        /*null_disallowed*/ false, IXCU_R_LENGTH, IXCU_OPERATION); /* STEP 5 */

    STEP_ARM(IXCU_R_LENGTH);
    STEP_ARM(IXCU_R_BEGIN);
    STEP_ARM(IXCU_R_HOP);
    STEP_ARM(IXCU_R_ENTRY);
    STEP_ARM(IXCU_R_SUBKEY);
    STEP_ARM(IXCU_R_LEAVE);
        return idb_key_range_walk_run(ctx, hdr, &s->rw, cb_result, IXCU_R_LENGTH, out_cb, out_argc);

    STEP_ARM(IXCU_OPERATION);
    {
        JSValue range, cursor, op, req;
        const char *dir;

        JS_FreeValue(ctx, cb_result);
        if (idb_key_range_walk_take(ctx, &s->rw, &range) < 0) return JS_STEP_ABRUPT;
        DCHECK(idb_transaction_state(ctx, s->tx) == IDB_TX_ACTIVE,
               "§4.6's openCursor had its transaction deactivated between its own state check and the step "
               "that places the request");
        dir = JS_ToCString(ctx, argv[1]);
        CHECK(dir != NULL, "IndexedDB: §4.6's openCursor could not read the direction its IDL converted");
        cursor = idb_cursor_new(ctx, hdr->this_val, s->tx, dir, range,
                                idl_step_magic(hdr) == IX_KEY_ONLY);                  /* STEP 6 */
        JS_FreeCString(ctx, dir);
        JS_FreeValue(ctx, range);
        op = idb_cursor_iterate_operation(ctx, cursor);                               /* STEP 7 */
        req = idb_request_execute(ctx, hdr->this_val, s->tx, op);                     /* STEP 8 */
        JS_FreeValue(ctx, op);
        idb_cursor_set_request(ctx, cursor, req);                                     /* STEP 9 */
        JS_FreeValue(ctx, cursor);
        *presult = req;                                                               /* STEP 10 */
        return JS_STEP_DONE;
    }
}

static const IdlStepDecl IX_OPEN_CURSOR_STEP = {
    js_ix_open_cursor, sizeof(IdbIndexQueryState), js_ix_query_visit, NULL,
    "Indexed Database §4.6 IDBIndex.openCursor / .openKeyCursor", IXCU_STEPS
};

/* ---- §4.6's attributes ------------------------------------------------------------------------------------ */

/* "The name getter steps are to return this's name" — the HANDLE's. §4.6's own note is what that buys: "as long
   as the transaction has not finished, this is the same as the associated index's name. But once the
   transaction has finished, this attribute will not reflect changes made with a later upgrade transaction." */
static JSValue js_ix_get_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!ix_brand(ctx, this_val)) return JS_EXCEPTION;
    return idb_index_handle_name(ctx, this_val);
}

/* "The objectStore getter steps are to return this's object store handle." `[SameObject]`, which holds because
   the handle stores it rather than deriving it. */
static JSValue js_ix_get_object_store(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!ix_brand(ctx, this_val)) return JS_EXCEPTION;
    return ix_get(ctx, this_val, IX_STORE);
}

/* "The keyPath getter steps are to return this's index's key path", converted per Web IDL — and the note is
   §4.5's exactly: "it returns the same object instance every time it is inspected", which §3.2.24 alone does
   not give, since its own steps mint a new Array per conversion. So the first inspection converts and the
   HANDLE remembers; two handles for one index answer with two Arrays. */
static JSValue js_ix_get_key_path(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue index, kp, slots;

    (void)magic;
    if (!ix_brand(ctx, this_val)) return JS_EXCEPTION;
    slots = ix_slots(ctx, this_val);
    DCHECK(JS_IsObject(slots), "an IDBIndex carried no slot record");
    kp = JS_GetPropertyStr(ctx, slots, IX_KEY_PATH);
    if (!JS_IsUndefined(kp)) {   /* the positive statement "inspected before" — see IX_KEY_PATH */
        JS_FreeValue(ctx, slots);
        return kp;
    }
    JS_FreeValue(ctx, kp);
    index = idb_index_handle_index(ctx, this_val);
    kp = idb_index_key_path_value(ctx, index);
    JS_FreeValue(ctx, index);
    DCHECK(!JS_IsUndefined(kp), "§4.6's keyPath conversion answered undefined, which is the one value this "
                                "cache reads as \"not converted yet\" — §2.6 gives every index a key path and "
                                "it is never null, so the conversion answers a string or an Array");
    JS_SetPropertyStr(ctx, slots, IX_KEY_PATH, JS_DupValue(ctx, kp));
    JS_FreeValue(ctx, slots);
    return kp;
}

/* "The multiEntry getter steps are to return this's index's multiEntry flag" / "the unique getter steps are to
   return this's index's unique flag" — the INDEX's and not the handle's, which is why neither is cached. */
static JSValue js_ix_get_flag(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue index;
    bool b;

    if (!ix_brand(ctx, this_val)) return JS_EXCEPTION;
    index = idb_index_handle_index(ctx, this_val);
    b = magic ? idb_index_multi_entry(ctx, index) : idb_index_unique(ctx, index);
    JS_FreeValue(ctx, index);
    return JS_NewBool(ctx, b);
}

/* "The name setter steps are: ... if transaction is not an upgrade transaction, throw an InvalidStateError. If
   transaction's state is not active, throw a TransactionInactiveError. If index OR INDEX'S OBJECT STORE has
   been deleted, throw an InvalidStateError. If index's name is equal to name, terminate these steps. If an
   index named name already exists in index's object store, throw a ConstraintError. Set index's name to name.
   Set this's name to name."
   THE ORDER IS THE STANDARD'S and it is observable: the upgrade-transaction refusal comes BEFORE the deleted
   test, so `index.name = 'x'` on a deleted index inside a readonly transaction reports the transaction. */
static JSValue js_ix_set_name(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue index = JS_UNDEFINED, tx = JS_UNDEFINED, store = JS_UNDEFINED, old, existing;
    const char *name = NULL;
    int same;

    (void)magic;
    if (!ix_brand(ctx, this_val)) return JS_EXCEPTION;
    index = idb_index_handle_index(ctx, this_val);
    tx = ix_transaction(ctx, this_val);
    store = idb_index_store(ctx, index);
    if (idb_transaction_mode(ctx, tx) != IDB_TX_VERSIONCHANGE) {
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "an index can only be renamed within an upgrade transaction");
        goto fail;
    }
    if (idb_transaction_state(ctx, tx) != IDB_TX_ACTIVE) {
        JS_ThrowDOMException(ctx, "TransactionInactiveError", "the upgrade transaction is not active");
        goto fail;
    }
    if (idb_index_is_deleted(ctx, index) || idb_object_store_is_deleted(ctx, store)) {
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "the index, or the object store it references, has been deleted");
        goto fail;
    }
    name = JS_ToCString(ctx, val);
    if (name == NULL) goto fail;
    old = idb_index_name(ctx, index);
    {
        const char *cold = JS_ToCString(ctx, old);

        CHECK(cold != NULL, "IndexedDB: an index could not report its own name");
        same = strcmp(cold, name) == 0;
        JS_FreeCString(ctx, cold);
    }
    JS_FreeValue(ctx, old);
    if (same) {                       /* "If index's name is equal to name, terminate these steps." */
        JS_FreeCString(ctx, name);
        goto done;
    }
    existing = idb_index_find(ctx, store, name);
    if (!JS_IsNull(existing)) {
        JS_FreeValue(ctx, existing);
        JS_FreeCString(ctx, name);
        JS_ThrowDOMException(ctx, "ConstraintError",
                             "an index with that name already exists in the object store");
        goto fail;
    }
    JS_FreeValue(ctx, existing);
    /* BOTH NAMES ARE WRITTEN, and they are two different fields — the index's, which §2.6 makes unique within
       the store, and the handle's, which the getter answers and which §5.8 step 6 reverts. */
    idb_index_rename(ctx, tx, store, index, name);
    {
        JSValue slots = ix_slots(ctx, this_val);

        JS_SetPropertyStr(ctx, slots, IX_NAME, JS_NewString(ctx, name));
        JS_FreeValue(ctx, slots);
    }
    JS_FreeCString(ctx, name);

done:
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, index);
    JS_FreeValue(ctx, tx);
    return JS_UNDEFINED;

fail:
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, index);
    JS_FreeValue(ctx, tx);
    return JS_EXCEPTION;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static void idb_index_handle_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_ix_class != 0, "a realm asked for IDBIndex.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_ix_class);
    DCHECK(JS_IsNull(prev), "idb_index_handle_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBIndex.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBIndex");
    idl_install_accessor(ctx, proto, "name", js_ix_get_name, 0, g_setter_name);
    idl_install_accessor(ctx, proto, "objectStore", js_ix_get_object_store, 0, -1);
    idl_install_accessor(ctx, proto, "keyPath", js_ix_get_key_path, 0, -1);
    idl_install_accessor(ctx, proto, "multiEntry", js_ix_get_flag, 1, -1);
    idl_install_accessor(ctx, proto, "unique", js_ix_get_flag, 0, -1);
    /* IN §4.6'S OWN ORDER — all of it now. A member's `length` is Web IDL §3.7.7's, the number of REQUIRED
       arguments, which is 0 for every one whose sole argument is optional. */
    idl_install_method(ctx, proto, "get", 1, g_id_get);
    idl_install_method(ctx, proto, "getKey", 1, g_id_get_key);
    idl_install_method(ctx, proto, "getAll", 0, g_id_get_all);
    idl_install_method(ctx, proto, "getAllKeys", 0, g_id_get_all_keys);
    idl_install_method(ctx, proto, "getAllRecords", 0, g_id_get_all_records);
    idl_install_method(ctx, proto, "count", 0, g_id_count);
    idl_install_method(ctx, proto, "openCursor", 0, g_id_open_cursor);
    idl_install_method(ctx, proto, "openKeyCursor", 0, g_id_open_key_cursor);
    JS_SetClassProto(ctx, g_ix_class, JS_DupValue(ctx, proto));

    ctor = idl_interface_object(ctx, "IDBIndex", proto);
    CHECK(!JS_IsException(ctor), "the IDBIndex interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "IDBIndex", ctor);
    JS_FreeValue(ctx, global);
}

void idb_index_handle_init(JSContext *ctx)
{
    JSClassDef d = { "IDBIndex" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* `get(any query)`, `getKey(any query)` and `count(optional any query)` — every position is `any`, which is
       what makes §2.9's convert-a-value-to-a-key-range the members' OWN step. */
    static const IdlArgType QUERY_ARGS[1] = { IDL_ANY };
    /* `openCursor(optional any query, optional IDBCursorDirection direction = "next")` and its key-only twin —
       the same two positions §4.5's pair declares, and the same §3.2.18 enumeration in the second. */
    static const IdlArgType CURSOR_ARGS[2] = { IDL_ANY, IDL_ENUM };
    /* `getAll(optional any queryOrOptions, optional [EnforceRange] unsigned long count)` and
       `getAllRecords(optional IDBGetAllOptions options = {})` — §4.5's declarations exactly, for the same
       reasons: the first position is `any` so §5.12's own branch is the member's step, and the count is
       IDL_UNRESTRICTED_DOUBLE so the declaration runs §3.2.7's ToNumber and the body runs [EnforceRange]'s
       range. The dictionary's member list is core/indexeddb/idb_get_all.h's, declared once for both
       interfaces. */
    static const IdlArgType GET_ALL_ARGS[2] = { IDL_ANY, IDL_UNRESTRICTED_DOUBLE };
    static const IdlArgType GET_ALL_RECORDS_ARGS[1] = { IDL_DICT };

    DCHECK(!g_ready, "idb_index_handle_init ran twice — one instance is one document is one agent");
    g_key = JS_NewSymbol(ctx, "idbIndexState", false);
    CHECK(!JS_IsException(g_key), "the IDBIndex slot key allocation failed");
    g_ix_rt = rt;
    JS_NewClassID(rt, &g_ix_class);
    CHECK(JS_NewClass(rt, g_ix_class, &d) == 0,
          "IDBIndex: the per-realm prototype slot could not be declared");
    g_id_get = idl_method_id_step(ctx, QUERY_ARGS, 1, NULL, 0, &IX_GET_STEP, IX_GET_REFERENCED);
    g_id_get_key = idl_method_id_step(ctx, QUERY_ARGS, 1, NULL, 0, &IX_GET_STEP, IX_GET_KEY);
    /* ONE DECLARATION PER MEMBER over ONE body: §4.6 states the three as one algorithm differing in `kind`,
       so the kind is the declaration's MAGIC. */
    g_id_get_all = idl_method_id_step(ctx, GET_ALL_ARGS, 2, NULL, 0, &IX_GET_ALL_STEP, IDB_GET_ALL_VALUE);
    idl_optional_from(0);                        /* both positions are optional; the member's length is 0 */
    g_id_get_all_keys = idl_method_id_step(ctx, GET_ALL_ARGS, 2, NULL, 0, &IX_GET_ALL_STEP, IDB_GET_ALL_KEY);
    idl_optional_from(0);
    g_id_get_all_records = idl_method_id_step(ctx, GET_ALL_RECORDS_ARGS, 1, IDB_GET_ALL_OPTIONS,
                                              (int)(sizeof IDB_GET_ALL_OPTIONS /
                                                    sizeof IDB_GET_ALL_OPTIONS[0]),
                                              &IX_GET_ALL_STEP, IDB_GET_ALL_RECORD);
    idl_optional_from(0);                        /* `optional IDBGetAllOptions options = {}` */
    g_id_count = idl_method_id_step(ctx, QUERY_ARGS, 1, NULL, 0, &IX_COUNT_STEP, 0);
    idl_optional_from(0);                        /* `count(optional any query)` */
    g_id_open_cursor = idl_method_id_step(ctx, CURSOR_ARGS, 2, NULL, 0, &IX_OPEN_CURSOR_STEP, IX_WITH_VALUE);
    idl_optional_from(0);                        /* both positions are optional; the member's length is 0 */
    idl_enum_values(IDB_CURSOR_DIRECTIONS);      /* §3.2.18's value list for the `direction` position */
    idl_arg_default(1, IDL_DEFAULT_STRING, "next");   /* §3.6 step 14.2's `= "next"` */
    g_id_open_key_cursor = idl_method_id_step(ctx, CURSOR_ARGS, 2, NULL, 0, &IX_OPEN_CURSOR_STEP, IX_KEY_ONLY);
    idl_optional_from(0);
    idl_enum_values(IDB_CURSOR_DIRECTIONS);
    idl_arg_default(1, IDL_DEFAULT_STRING, "next");
    g_setter_name = idl_setter_id(ctx, IDL_DOMSTRING, /*null_to_empty*/ false, js_ix_set_name, 0);
    g_ready = 1;
    agent_state_flag("idb_index_handle", &g_ready, "the declaration latch");
    agent_state_ptr("idb_index_handle", &g_ix_rt, "the runtime §2.6.1's slot key was minted in");
    agent_state_value("idb_index_handle", &g_key, "§2.6.1's internal-slot key");
    realm_declare_intrinsic(idb_index_handle_install_realm);
}

void idb_index_handle_free(JSRuntime *rt)
{
    DCHECK(g_ready, "§2.6.1's index-handle machinery was released in an agent that never declared it");
    DCHECK(rt == g_ix_rt, "idb_index_handle_free was given a runtime that is not the one it declared into");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_ix_rt = NULL;
    g_ready = 0;
}
