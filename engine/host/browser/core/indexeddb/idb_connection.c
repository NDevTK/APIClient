/* INDEXED DATABASE §2.1.1's DATABASE CONNECTION, §4.4's IDBDatabase over it, and §5.2's close.
 *
 * WHY THE CONNECTION IS A COMPONENT AND NOT A FIELD OF THE DATABASE. §2.1.1: "script does not interact with
 * databases directly. Instead, script has indirect access via a connection ... it is also the only way to
 * obtain a transaction for that database." Everything a page can do to a database it does through one, and the
 * connection is what carries the three pieces of state that make those operations answerable: the VERSION it
 * was opened at (which stops answering the database's the moment an upgrade aborts), the OBJECT STORE SET it
 * sees, and the CLOSE PENDING flag that every member checks before it creates anything.
 *
 * THE CONNECTION IS THE IDBDatabase OBJECT. §5.1 answers with a connection, §4.3's completion task writes that
 * into `request.result`, and the page reads an IDBDatabase — one object is what makes those the same sentence,
 * the same identity idb_request.c gives §2.8's request and idb_transaction.c gives §2.7's transaction. Its
 * state lives in an internal-slot record under a private Symbol, so every write is an ordinary property write
 * the per-flow COW delta captures: one flow's `close()` is invisible to its sibling, and a flow parked between
 * `upgradeneeded` and `success` resumes holding the connection it had.
 *
 * THE OBJECT STORE SET IS THE DATABASE'S OWN RECORD AND NOT A COPY. "A connection has an object store set,
 * which is initialized to the set of object stores in the associated database when the connection is created.
 * The contents of the set will remain constant except when an upgrade transaction is live" — and while one IS
 * live, this connection is the only one there can be (§5.1's connection queue guarantees it), so the two can
 * never diverge for THIS connection. What the sentence is about is the OTHER connections, which §5.1 requires
 * to be closed before an upgrade runs. A copy taken here would be a second answer to one question, and
 * `createObjectStore` would then have to write both.
 *
 * WHAT IS ABSENT AND WHY. `objectStoreNames` needs a DOMStringList, which this engine does not have.
 * `transaction()` needs Web IDL §3.2.25's `(DOMString or sequence<DOMString>)` union, whose resolution is
 * "Type(V) is Object and ? Get(V, @@iterator) is not undefined" — a PROPERTY READ of the page's value, so it
 * belongs in the argument-conversion machine as a request and not in a body that would run it from C. Both are
 * honestly ABSENT and the IDL gap auditor lists them; until `transaction()` lands, a page reaches §4.5's
 * members through the handle §4.4's `createObjectStore` returns and through §4.10's `objectStore()` on the
 * upgrade transaction, which is where every migration already works. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_object_store.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/realm.h"

/* §2.1.1's own fields, spelled once. */
#define CONN_DATABASE "database"
#define CONN_VERSION  "version"        /* §2.1.1's version, "set when the connection is created" */
#define CONN_STORES   "stores"         /* §2.1.1's object store set — the database's own record */
#define CONN_PENDING  "closePending"   /* §2.1.1's close pending flag, "initially false" */
#define CONN_CLOSED   "closed"         /* §5.2 step 3's "once they are complete, connection is closed" */

static JSValue   g_key;
static JSClassID g_conn_class;
static int       g_ready;
static JSRuntime *g_conn_rt;
static int g_id_close = -1, g_id_create_store = -1, g_id_delete_store = -1;
/* §5.1 step 10.6's rendezvous — see the header. */
static void (*g_closed_hook)(JSContext *ctx, JSValueConst connection);

/* ---- the record ---------------------------------------------------------------------------------------- */

static JSValue conn_slots(JSContext *ctx, JSValueConst c)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "an IDBDatabase slot was asked for before its interface was declared");
    if (!JS_IsObject(c))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBDatabase slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &st, c, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

bool idb_connection_is(JSValueConst v)
{
    return g_conn_class != 0 && JS_GetClassID(v) == g_conn_class;
}

static bool conn_brand(JSContext *ctx, JSValueConst this_val)
{
    if (idb_connection_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "an IDBDatabase member was reached on something that is not an IDBDatabase");
    return false;
}

static JSValue conn_get(JSContext *ctx, JSValueConst c, const char *field)
{
    JSValue slots = conn_slots(ctx, c), v;

    DCHECK(JS_IsObject(slots), "an IDBDatabase field was read off a value carrying no slot record");
    v = JS_GetPropertyStr(ctx, slots, field);
    JS_FreeValue(ctx, slots);
    return v;
}

/* `v` is CONSUMED. */
static void conn_set(JSContext *ctx, JSValueConst c, const char *field, JSValue v)
{
    JSValue slots = conn_slots(ctx, c);

    DCHECK(JS_IsObject(slots), "an IDBDatabase field was written on a value carrying no slot record");
    JS_SetPropertyStr(ctx, slots, field, v);
    JS_FreeValue(ctx, slots);
}

static bool conn_flag(JSContext *ctx, JSValueConst c, const char *field)
{
    JSValue v = conn_get(ctx, c, field);
    bool b;

    DCHECK(JS_IsBool(v), "a connection's flag is not a boolean — only this file writes them");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

JSValue idb_connection_open(JSContext *ctx, JSValueConst db)
{
    JSValue conn, st, proto;
    JSAtom k;

    DCHECK(g_ready, "a connection was opened before idb_connection_init declared the interface");
    DCHECK(JS_IsObject(db), "a connection was opened to something that is not a §2.1 database");
    proto = JS_GetClassProto(ctx, g_conn_class);
    DCHECK(!JS_IsNull(proto), "IDBDatabase.prototype was asked for in a realm that never ran its install");
    conn = JS_NewObjectProtoClass(ctx, proto, g_conn_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(conn), "IndexedDB: the IDBDatabase allocation failed");
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "IndexedDB: the IDBDatabase slot record allocation failed");
    JS_SetPropertyStr(ctx, st, CONN_DATABASE, JS_DupValue(ctx, db));
    /* "A connection has a version, which is set when the connection is created." §5.1 step 9 overwrites it
       with the version being opened; this is the value it has until then. */
    JS_SetPropertyStr(ctx, st, CONN_VERSION, JS_NewFloat64(ctx, idb_database_version(ctx, db)));
    JS_SetPropertyStr(ctx, st, CONN_STORES, idb_database_store_set(ctx, db));
    JS_SetPropertyStr(ctx, st, CONN_PENDING, JS_FALSE);
    JS_SetPropertyStr(ctx, st, CONN_CLOSED, JS_FALSE);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the IDBDatabase slot key could not be interned");
    JS_SetProperty(ctx, conn, k, st);
    JS_FreeAtom(ctx, k);
    /* "There may be multiple connections to a given database at any given time" — the set §5.1 step 10.1 asks
       for, joined HERE so the two edges of a connection's membership are its creation and its close. */
    idb_database_add_connection(ctx, db, conn);
    return conn;
}

JSValue idb_connection_database(JSContext *ctx, JSValueConst connection)
{
    JSValue db = conn_get(ctx, connection, CONN_DATABASE);

    DCHECK(JS_IsObject(db), "a connection carried no database — every connection is built by "
                            "idb_connection_open, which gives it one");
    return db;
}

double idb_connection_version(JSContext *ctx, JSValueConst connection)
{
    JSValue v = conn_get(ctx, connection, CONN_VERSION);
    double out = 0;
    int r = JS_ToFloat64(ctx, &out, v);

    DCHECK(r >= 0, "a connection's VERSION is not a number — §2.1.1 makes it one and only this file writes it");
    (void)r;
    JS_FreeValue(ctx, v);
    return out;
}

void idb_connection_set_version(JSContext *ctx, JSValueConst connection, double version)
{
    conn_set(ctx, connection, CONN_VERSION, JS_NewFloat64(ctx, version));
}

JSValue idb_connection_store_set(JSContext *ctx, JSValueConst connection)
{
    JSValue stores = conn_get(ctx, connection, CONN_STORES);

    DCHECK(JS_IsObject(stores), "a connection carried no object store set");
    return stores;
}

bool idb_connection_close_pending(JSContext *ctx, JSValueConst connection)
{
    return conn_flag(ctx, connection, CONN_PENDING);
}

bool idb_connection_is_closed(JSContext *ctx, JSValueConst connection)
{
    return conn_flag(ctx, connection, CONN_CLOSED);
}

void idb_connection_set_closed_hook(void (*on_closed)(JSContext *ctx, JSValueConst connection))
{
    DCHECK(g_closed_hook == NULL || on_closed == NULL,
           "§5.1 step 10.6's rendezvous was registered twice — one component performs §5.1, and a second "
           "registration would silently replace the algorithm that is waiting on a connection to close");
    g_closed_hook = on_closed;
}

/* ---- §5.2's CLOSE A DATABASE CONNECTION -------------------------------------------------------------------
 *
 * Step 3 is the whole of the algorithm's shape: "wait for all transactions created using connection to
 * complete. Once they are complete, connection is closed." The wait cannot be discharged by the task queue —
 * a transaction completes when its own commit or abort task runs, which is arbitrarily far in the future — so
 * the transition is reached from BOTH ends: `close()` asks whether the wait is already over, and
 * idb_transaction.c reports every transaction that finishes. One transition, written once, so a connection
 * cannot become closed by one path and stay in the database's connection set because the other path was the
 * one that remembered. */

static void conn_become_closed(JSContext *ctx, JSValueConst connection)
{
    JSValue db;

    DCHECK(conn_flag(ctx, connection, CONN_PENDING),
           "a connection became closed without a close pending flag — §2.1.1: \"when the connection is closed "
           "its close pending flag is always set to true if it hasn't already been\"");
    if (conn_flag(ctx, connection, CONN_CLOSED))
        return;   /* §4.4's own note: "subsequent calls to close() will have no effect" */
    if (idb_transaction_any_live_for_connection(ctx, connection))
        return;   /* step 3's wait is not over; the last transaction to finish will bring us back here */
    conn_set(ctx, connection, CONN_CLOSED, JS_TRUE);
    db = idb_connection_database(ctx, connection);
    idb_database_remove_connection(ctx, db, connection);
    JS_FreeValue(ctx, db);
    /* §5.2's own note: "once the connection is closed, this can unblock the steps to upgrade a database, and
       the steps to delete a database, which both wait for connections to a given database to be closed". */
    if (g_closed_hook)
        g_closed_hook(ctx, connection);
}

void idb_connection_close(JSContext *ctx, JSValueConst connection)
{
    DCHECK(idb_connection_is(connection), "§5.2 was given something that is not a connection");
    /* Step 1: "Set connection's close pending flag to true." §2.1.1's note is what makes this the whole of the
       member's synchronous effect: "once a connection's close pending flag has been set to true, no new
       transactions can be created using the connection". */
    conn_set(ctx, connection, CONN_PENDING, JS_TRUE);
    conn_become_closed(ctx, connection);
}

void idb_connection_transaction_finished(JSContext *ctx, JSValueConst connection)
{
    DCHECK(idb_connection_is(connection), "a finished transaction named something that is not a connection");
    if (!conn_flag(ctx, connection, CONN_PENDING))
        return;   /* nothing is waiting on this connection: §5.2 has not been run for it */
    conn_become_closed(ctx, connection);
}

/* ---- §4.4's members ---------------------------------------------------------------------------------------- */

/* "The name getter steps are to return this's associated database's name." The DATABASE's, which the note is
   explicit about: "the name attribute returns this name even if this's close pending flag is true". */
static JSValue js_conn_get_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue db, name;

    (void)magic;
    if (!conn_brand(ctx, this_val)) return JS_EXCEPTION;
    db = idb_connection_database(ctx, this_val);
    name = idb_database_name(ctx, db);
    JS_FreeValue(ctx, db);
    return name;
}

/* "The version getter steps are to return this's version." THE CONNECTION'S, not the database's, which is the
   distinction the standard's own question-box draws: "once the connection has closed, this attribute will not
   reflect changes made with a later upgrade transaction". */
static JSValue js_conn_get_version(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!conn_brand(ctx, this_val)) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, idb_connection_version(ctx, this_val));
}

/* "The close() method steps are to run close a database connection with this connection." */
static JSValue js_conn_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    if (!conn_brand(ctx, this_val)) return JS_EXCEPTION;
    idb_connection_close(ctx, this_val);
    return JS_UNDEFINED;
}

/* The FIRST TWO STEPS of `createObjectStore` and `deleteObjectStore`, which the standard writes identically:
   "let database be this's associated database. Let transaction be database's upgrade transaction if it is not
   null, or throw an InvalidStateError otherwise. If transaction's state is not active, then throw a
   TransactionInactiveError." Both are OWNED on success; -1 with the throw live. */
static int conn_upgrade_transaction(JSContext *ctx, JSValueConst c, JSValue *pdb, JSValue *ptx)
{
    JSValue db = idb_connection_database(ctx, c), tx = idb_database_upgrade_transaction(ctx, db);

    if (JS_IsNull(tx)) {
        JS_FreeValue(ctx, tx);
        JS_FreeValue(ctx, db);
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "the database is not inside an upgrade transaction, so its set of object stores "
                             "cannot be changed");
        return -1;
    }
    if (idb_transaction_state(ctx, tx) != IDB_TX_ACTIVE) {
        JS_FreeValue(ctx, tx);
        JS_FreeValue(ctx, db);
        JS_ThrowDOMException(ctx, "TransactionInactiveError", "the upgrade transaction is not active");
        return -1;
    }
    *pdb = db;
    *ptx = tx;
    return 0;
}

/* "The createObjectStore(name, options) method steps are: ... let keyPath be options's keyPath member if it is
   not undefined or null, or null otherwise. If keyPath is not null and is not a valid key path, throw a
   SyntaxError. If an object store named name already exists in database throw a ConstraintError. Let
   autoIncrement be options's autoIncrement member. If autoIncrement is true and keyPath is an empty string or
   any sequence, throw an InvalidAccessError. Let store be a new object store in database ... Return a new
   object store handle associated with store and transaction." */
static JSValue js_conn_create_object_store(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                           int magic)
{
    /* THE OPTIONS ARE READ OUT OF THE VECTOR AND NOT OUT OF `argc`. `optional IDBObjectStoreParameters
       options = {}` means the dictionary EXISTS whether or not the page wrote one, carrying every member's
       declared default — so a body that wrote `argc > 1 ? argv[1] : JS_UNDEFINED` would be filling a hole the
       conversion does not leave, which is the consumer-side default §Offensive-programming names. */
    JSValueConst options = argv[1];
    JSValue db = JS_UNDEFINED, tx = JS_UNDEFINED, key_path, existing, store, handle;
    const char *name = NULL, *path = NULL;
    size_t path_len = 0;
    bool auto_increment;

    (void)magic; (void)argc;
    DCHECK(JS_IsObject(options), "§4.4's createObjectStore was handed no options dictionary — the IDL writes "
                                 "`optional IDBObjectStoreParameters options = {}`, so the conversion builds "
                                 "one with every member at its default even for `createObjectStore(name)`");
    if (!conn_brand(ctx, this_val)) return JS_EXCEPTION;
    if (conn_upgrade_transaction(ctx, this_val, &db, &tx) < 0) return JS_EXCEPTION;
    name = JS_ToCString(ctx, argv[0]);
    if (name == NULL) goto fail;

    key_path = idl_dict_get(ctx, options, "keyPath");
    DCHECK(!JS_IsUndefined(key_path), "IDBObjectStoreParameters' `keyPath` is declared with the IDL's own "
                                      "`= null`, so the conversion places it whether or not the page wrote "
                                      "one — an undefined here is the declaration having lost its default");
    if (!JS_IsNull(key_path)) {
        /* §2.5's LIST ARM — `(DOMString or sequence<DOMString>)?` — is Web IDL §3.2.25's union, whose
           resolution is a property read of the page's value (`Get(V, @@iterator)`) and therefore belongs in
           the argument-conversion machine as a request rather than in this body. The member is declared
           `any` until it is there, so what arrives is whatever the page wrote and a non-string is this. */
        if (!JS_IsString(key_path)) {
            JS_FreeValue(ctx, key_path);
            DFAIL("Indexed Database §2.5's LIST key path is not built. `createObjectStore(name, {keyPath: "
                  "['a','b']})` declares a COMPOUND key path, whose value is §7.1's list of extracted keys "
                  "assembled into an array key — and reaching it needs Web IDL §3.2.25's `(DOMString or "
                  "sequence<DOMString>)` union, whose resolution reads @@iterator off the page's value and so "
                  "must be a REQUEST in core/idl_args.c rather than a read from C. Build the union type "
                  "there, declare this member's `keyPath` with it, and then §2.5's list arm and §7.1's "
                  "list arm here");
            /* A RELEASE BUILD FALLS THROUGH, and it must not return JS_EXCEPTION with nothing thrown — the
               capability is simply not supportable outside dev, which is what this says. */
            JS_ThrowTypeError(ctx, "a sequence key path is not supported");
            JS_FreeCString(ctx, name);
            goto fail;
        }
        path = JS_ToCStringLen(ctx, &path_len, key_path);
        JS_FreeValue(ctx, key_path);
        if (path == NULL) { JS_FreeCString(ctx, name); goto fail; }
        if (!idb_key_path_is_valid(path, path_len)) {
            JS_ThrowDOMException(ctx, "SyntaxError", "the key path is not a valid key path");
            goto fail_path;
        }
    } else {
        JS_FreeValue(ctx, key_path);
    }

    existing = idb_object_store_find(ctx, db, name);
    if (!JS_IsNull(existing)) {
        JS_FreeValue(ctx, existing);
        JS_ThrowDOMException(ctx, "ConstraintError",
                             "an object store with that name already exists in the database");
        goto fail_path;
    }
    JS_FreeValue(ctx, existing);

    auto_increment = idl_dict_bool(ctx, options, "autoIncrement");
    /* "If autoIncrement is true and keyPath is an EMPTY STRING or any sequence, throw an InvalidAccessError."
       The sequence arm is unreachable above; the empty string is not, and it is the arm that matters — an
       empty key path names the value ITSELF as the key, which a generated key could not be injected into. */
    if (auto_increment && path != NULL && path_len == 0) {
        JS_ThrowDOMException(ctx, "InvalidAccessError",
                             "a key generator cannot be used with an empty key path");
        goto fail_path;
    }

    store = idb_object_store_create(ctx, db, name, path != NULL ? JS_NewStringLen(ctx, path, path_len) : JS_NULL,
                                    auto_increment);
    /* §5.7 step 3 set this transaction's scope to the connection's object store set, and this is the write
       that keeps that true — §4.10's note: "subsequent calls to [objectStoreNames] during an upgrade
       transaction can return lists with different contents as object stores are created and deleted". */
    idb_transaction_scope_add(ctx, tx, store);
    handle = idb_object_store_handle(ctx, store, tx);
    /* §2.2.1's one-handle-per-store: the handle this member returns is the one `objectStore(name)` must answer
       with afterwards, so it is filed rather than left for a second mint to duplicate. */
    idb_transaction_handle_add(ctx, tx, name, handle);
    JS_FreeValue(ctx, store);
    if (path != NULL) JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, db);
    JS_FreeValue(ctx, tx);
    return handle;

fail_path:
    if (path != NULL) JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, name);
fail:
    JS_FreeValue(ctx, db);
    JS_FreeValue(ctx, tx);
    return JS_EXCEPTION;
}

/* "The deleteObjectStore(name) method steps are: ... let store be the object store named name in database, or
   throw a NotFoundError if none. Remove store from this's object store set. If there is an object store handle
   associated with store and transaction, remove all entries from its index set. Destroy store." */
static JSValue js_conn_delete_object_store(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                           int magic)
{
    JSValue db = JS_UNDEFINED, tx = JS_UNDEFINED, store;
    const char *name;

    (void)argc; (void)magic;
    if (!conn_brand(ctx, this_val)) return JS_EXCEPTION;
    if (conn_upgrade_transaction(ctx, this_val, &db, &tx) < 0) return JS_EXCEPTION;
    name = JS_ToCString(ctx, argv[0]);
    if (name == NULL) { JS_FreeValue(ctx, db); JS_FreeValue(ctx, tx); return JS_EXCEPTION; }
    store = idb_object_store_find(ctx, db, name);
    JS_FreeCString(ctx, name);
    if (JS_IsNull(store)) {
        JS_FreeValue(ctx, store);
        JS_FreeValue(ctx, db);
        JS_FreeValue(ctx, tx);
        return JS_ThrowDOMException(ctx, "NotFoundError",
                                    "no object store with that name exists in the database");
    }
    /* "Remove store from this's object store set" and "Destroy store" are ONE operation here, because this
       connection's object store set IS the database's record — see the file header. The index-set step has
       nothing to clear: §2.6's index does not exist, so no handle can hold one. */
    idb_transaction_scope_remove(ctx, tx, store);
    idb_object_store_destroy(ctx, db, store);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, db);
    JS_FreeValue(ctx, tx);
    return JS_UNDEFINED;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static void idb_connection_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_conn_class != 0, "a realm asked for IDBDatabase.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_conn_class);
    DCHECK(JS_IsNull(prev), "idb_connection_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBDatabase.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBDatabase");
    event_target_chain(ctx, proto);            /* `interface IDBDatabase : EventTarget` */
    event_target_install_handlers(ctx, proto, EH_IDB_DATABASE);
    idl_install_accessor(ctx, proto, "name", js_conn_get_name, 0, -1);
    idl_install_accessor(ctx, proto, "version", js_conn_get_version, 0, -1);
    idl_install_method(ctx, proto, "close", 0, g_id_close);
    idl_install_method(ctx, proto, "createObjectStore", 1, g_id_create_store);
    idl_install_method(ctx, proto, "deleteObjectStore", 1, g_id_delete_store);
    JS_SetClassProto(ctx, g_conn_class, JS_DupValue(ctx, proto));

    ctor = idl_interface_object(ctx, "IDBDatabase", proto);
    CHECK(!JS_IsException(ctor), "the IDBDatabase interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "IDBDatabase", ctor);
    JS_FreeValue(ctx, global);
}

void idb_connection_init(JSContext *ctx)
{
    JSClassDef d = { "IDBDatabase" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CREATE_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    static const IdlArgType DELETE_ARGS[1] = { IDL_DOMSTRING };
    /* `dictionary IDBObjectStoreParameters { (DOMString or sequence<DOMString>)? keyPath = null; boolean
       autoIncrement = false; }` — one level, so §3.2.17 reads them in the order they are written here, which
       a page pins by throwing from a member's getter. `keyPath` is declared `any` because its union's second
       arm is §3.2.25's sequence resolution, which is not built (see the member); the DEFAULT is declared, so
       the body reads a value that is there rather than inventing the null. */
    static const IdlDictMember CREATE_INIT[] = {
        { "autoIncrement", IDL_BOOLEAN },
        { "keyPath", IDL_ANY, false, NULL, 0, NULL, IDL_DEFAULT_NULL },
    };

    DCHECK(!g_ready, "idb_connection_init ran twice — one instance is one document is one agent");
    g_key = JS_NewSymbol(ctx, "idbDatabaseState", false);
    CHECK(!JS_IsException(g_key), "the IDBDatabase slot key allocation failed");
    g_conn_rt = rt;
    JS_NewClassID(rt, &g_conn_class);
    CHECK(JS_NewClass(rt, g_conn_class, &d) == 0,
          "IDBDatabase: the per-realm prototype slot could not be declared");
    g_id_close = idl_method_id(ctx, NULL, 0, js_conn_close, 0);
    g_id_create_store = idl_method_id_dict(ctx, CREATE_ARGS, 2, CREATE_INIT,
                                           (int)(sizeof CREATE_INIT / sizeof CREATE_INIT[0]),
                                           js_conn_create_object_store, 0);
    idl_optional_from(1);                      /* `optional IDBObjectStoreParameters options = {}` */
    g_id_delete_store = idl_method_id(ctx, DELETE_ARGS, 1, js_conn_delete_object_store, 0);
    g_ready = 1;
    realm_declare_intrinsic(idb_connection_install_realm);
}

void idb_connection_free(JSRuntime *rt)
{
    if (!g_ready)
        return;
    DCHECK(rt == g_conn_rt, "idb_connection_free was given a runtime that is not the one it declared into");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_ready = 0;
}
