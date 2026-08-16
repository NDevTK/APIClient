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
 * WHAT IS ABSENT AND WHY. `indexNames`, `index()`, `createIndex()` and `deleteIndex()` are §2.6's INDEX, which
 * does not exist — there is nothing that can hold an index set. `delete()`, `clear()`, `count()`, the four
 * `getAll*` members and the two cursor openers are §6.3-§6.7 and §5.12, which do not exist either: the store
 * has §6.1's storage operation and §6.2's two retrievals and nothing else, so the four members here are exactly
 * the four algorithms there are. Each absent member is a TypeError naming itself, which the IDL gap auditor
 * lists — never a shape-only member that would report a deletion nothing performed. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_key.h"
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

static JSValue   g_key;
static JSClassID g_os_class;
static int       g_ready;
static JSRuntime *g_os_rt;
static int g_id_put = -1, g_id_add = -1, g_id_get = -1, g_id_get_key = -1, g_setter_name = -1;

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

/* ---- §4.5's ADD OR PUT ------------------------------------------------------------------------------------ */

static JSValue js_os_put(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst data[4];
    JSValue store = JS_UNDEFINED, tx = JS_UNDEFINED, key = JS_UNDEFINED, key_path, clone, op, req;
    bool key_given = argc > 1 && !JS_IsUndefined(argv[1]);
    bool in_line, generator;

    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    if (os_check(ctx, this_val, /*writes*/ true, &store, &tx) < 0) return JS_EXCEPTION;
    key_path = idb_object_store_key_path(ctx, store);
    in_line = !JS_IsNull(key_path);
    JS_FreeValue(ctx, key_path);
    generator = idb_object_store_uses_key_generator(ctx, store);

    /* "If store uses in-line keys and key was given, throw a DataError." */
    if (in_line && key_given) {
        JS_ThrowDOMException(ctx, "DataError",
                             "the object store uses in-line keys, so a key may not be given to put()");
        goto fail;
    }
    /* "If store uses out-of-line keys and has no key generator and key was not given, throw a DataError." */
    if (!in_line && !generator && !key_given) {
        JS_ThrowDOMException(ctx, "DataError",
                             "the object store uses out-of-line keys and has no key generator, so put() "
                             "requires a key");
        goto fail;
    }
    /* "If key was given, then: let r be the result of converting a value to a key with key. Rethrow any
       exceptions. If r is 'invalid value' or 'invalid type', throw a DataError. Let key be r." */
    if (key_given && idb_key_from_value(ctx, argv[1], &key) < 0)
        goto fail;

    /* "Let clone be a clone of value in targetRealm DURING TRANSACTION. Rethrow any exceptions."
       THE CLONE IS THE MEMBER'S AND NOT THE OPERATION'S, and that placement is observable: an unclonable value
       is a "DataCloneError" thrown by `put` ITSELF, synchronously, where the page's own try/catch sees it —
       not a request that later fires an `error` event. §6.1 serializes again on the way into the record, which
       is what makes §2.3's "later changes to a value have no effect" a property of the store rather than of
       whoever called it; that second copy is of engine-owned plain data and can refuse nothing. */
    clone = structured_clone(ctx, argv[0]);
    if (JS_IsException(clone))
        goto fail;

    /* §4.5's in-line-key steps (§7.1's extract-a-key, §7.2's check-that-a-key-could-be-injected) are §6.1's
       first step in this engine, which is where the crash naming them lives — see idb_database.c. A store
       reaching this line with in-line keys or a key generator carries no key, which is exactly what that
       algorithm's own first step branches on. */
    data[OP_STORE_TX] = tx;
    data[OP_STORE_STORE] = store;
    data[OP_STORE_VALUE] = clone;
    data[OP_STORE_KEY] = key;
    op = JS_NewCFunctionData(ctx, js_idb_store_operation, 0, magic, 4, data);
    JS_FreeValue(ctx, clone);
    CHECK(!JS_IsException(op), "IndexedDB: §6.1's operation could not be minted");
    /* "Return the result (an IDBRequest) of running asynchronously execute a request with THIS and operation."
       The source is the HANDLE, which is what `request.source` answers with. */
    req = idb_request_execute(ctx, this_val, tx, op);
    JS_FreeValue(ctx, op);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, tx);
    return req;

fail:
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, tx);
    return JS_EXCEPTION;
}

/* ---- §4.5's GET and GETKEY --------------------------------------------------------------------------------- */

static JSValue js_os_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst data[2];
    JSValue store = JS_UNDEFINED, tx = JS_UNDEFINED, range, op, req;

    (void)argc;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    /* NEITHER of these writes, so the read-only refusal does not apply — §4.5 puts them under a different
       heading for exactly that reason ("the following methods throw a TransactionInactiveError ..."). */
    if (os_check(ctx, this_val, /*writes*/ false, &store, &tx) < 0) return JS_EXCEPTION;
    /* "Let range be the result of converting a value to a key range with query AND TRUE." The `true` is the
       null-disallowed flag: `store.get(null)` is a DataError rather than a read of the whole store. */
    if (idb_key_range_from_value(ctx, argv[0], /*null_disallowed*/ true, &range) < 0) {
        JS_FreeValue(ctx, store);
        JS_FreeValue(ctx, tx);
        return JS_EXCEPTION;
    }
    data[OP_GET_STORE] = store;
    data[OP_GET_RANGE] = range;
    op = JS_NewCFunctionData(ctx, js_idb_retrieve_operation, 0, magic, 2, data);
    JS_FreeValue(ctx, range);
    CHECK(!JS_IsException(op), "IndexedDB: §6.2's operation could not be minted");
    req = idb_request_execute(ctx, this_val, tx, op);
    JS_FreeValue(ctx, op);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, tx);
    return req;
}

/* ---- §4.5's attributes ------------------------------------------------------------------------------------- */

/* "The name getter steps are to return this's name" — the HANDLE's, which is why it is not read off the store.
   The two agree until an upgrade transaction renames the store and aborts. */
static JSValue js_os_get_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    return idb_object_store_handle_name(ctx, this_val);
}

/* "The keyPath getter steps are to return this's object store's key path, or null if none." The note — "the
   returned value is not the same instance that was used when the object store was created" — is satisfied by
   the store holding the string §4.4 converted rather than the page's own value. */
static JSValue js_os_get_key_path(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue store, kp;

    (void)magic;
    if (!os_brand(ctx, this_val)) return JS_EXCEPTION;
    store = os_get(ctx, this_val, OS_STORE);
    kp = idb_object_store_key_path(ctx, store);
    JS_FreeValue(ctx, store);
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
    idl_install_method(ctx, proto, "put", 1, g_id_put);
    idl_install_method(ctx, proto, "add", 1, g_id_add);
    idl_install_method(ctx, proto, "get", 1, g_id_get);
    idl_install_method(ctx, proto, "getKey", 1, g_id_get_key);
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
    static const IdlArgType GET_ARGS[1] = { IDL_ANY };

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
    g_id_put = idl_method_id(ctx, PUT_ARGS, 2, js_os_put, OS_PUT);
    idl_optional_from(1);                        /* `optional any key` */
    g_id_add = idl_method_id(ctx, PUT_ARGS, 2, js_os_put, OS_ADD);
    idl_optional_from(1);
    g_id_get = idl_method_id(ctx, GET_ARGS, 1, js_os_get, OS_GET_VALUE);
    g_id_get_key = idl_method_id(ctx, GET_ARGS, 1, js_os_get, OS_GET_KEY);
    g_setter_name = idl_setter_id(ctx, IDL_DOMSTRING, /*null_to_empty*/ false, js_os_set_name, 0);
    g_ready = 1;
    realm_declare_intrinsic(idb_object_store_install_realm);
}

void idb_object_store_free(JSRuntime *rt)
{
    if (!g_ready)
        return;
    DCHECK(rt == g_os_rt, "idb_object_store_free was given a runtime that is not the one it declared into");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_ready = 0;
}
