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
 * WHAT WAS ABSENT AND WHY IT NO LONGER IS. `objectStoreNames` needed a DOMStringList, which the engine now has
 * as HTML §2.6.5's own component, and an ORDER, which §2's create a sorted name list owns for all three of its
 * consumers. Its set is THIS FILE's — §2.1.1's object store set — which is why the member lives here and not
 * beside §2.1's storage model. `transaction()` used to stand beside it, and for a
 * different reason: its `storeNames` is Web IDL §3.2.25's `(DOMString or sequence<DOMString>)`, whose arm is
 * chosen by ? GetMethod(V, %Symbol.iterator%) — a property read of the PAGE'S value, which a body cannot
 * perform because a C activation hosting the page's getter is the drive-to-completion this engine aborts on.
 * That union is a declared type now (core/idl_args.h's IDL_DOMSTRING_OR_SEQUENCE), so the member is here and
 * the body is handed either a string or the engine's own array of them. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "core/indexeddb/indexed_db.h"
#include "solver/concolic.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_key_path.h"
#include "core/indexeddb/idb_name_list.h"
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
static int g_id_close = -1, g_id_create_store = -1, g_id_delete_store = -1, g_id_transaction = -1;
/* §5.1 step 10.5's and §5.3 step 9's rendezvous — see the header. */
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
           "§5.1 step 10.5's rendezvous was registered twice — one component performs §5.1 and §5.3, and a "
           "second registration would silently replace the algorithm that is waiting on a connection to "
           "close");
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
    /* Step 1: "Set connection's close pending flag to true." §5.2's own note is what makes this the whole of
       the member's synchronous effect: "Once a connection's close pending flag has been set to true, no new
       transactions can be created using the connection." */
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

/* ---- §4.4's ENUMERATIONS ----------------------------------------------------------------------------------
 *
 * The value lists ARE the types (Web IDL §3.2.18 Enumeration types), so each is declared beside the POSITION
 * that takes it and the conversion
 * refuses anything else before a body runs. The ORDER is not free: the index into each list is the enumerator
 * idb_transaction.h declares, which is what lets one lookup serve both the declaration and the body. */
IDL_ENUM_VALUES(TX_MODES, "readonly", "readwrite", "versionchange");
IDL_ENUM_VALUES(TX_DURABILITY, "default", "strict", "relaxed");

/* WHICH VALUE OF THE ENUMERATION THIS IS. It runs none of the page's code — §3.2.18's membership check is part
   of the TYPE and has already run, so what arrives is one of the strings this file wrote. */
static int conn_enum_index(JSContext *ctx, JSValueConst v, const char *const *values)
{
    const char *str = JS_ToCString(ctx, v);
    int i = 0;

    CHECK(str != NULL, "IndexedDB: an enumeration value could not be read back as a string");
    while (values[i] != NULL && strcmp(str, values[i]) != 0)
        i++;
    /* ALWAYS FATAL, because what this decides is whether the database may be WRITTEN. §3.2.18's check runs in
       the conversion against the list declared beside the member, so a value arriving here that this list does
       not name means the declaration and this list have drifted apart — and the index would then be read past
       the end of the list and handed to §2.7 as a mode. */
    CHECK(values[i] != NULL,
          "IndexedDB: a transaction enumeration held a value its own IDL does not list — the declaration and "
          "this list name different value sets");
    JS_FreeCString(ctx, str);
    return i;
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

/* How many object stores the connection's §2.1.1 object store set holds. */
static uint32_t conn_store_count(JSContext *ctx, JSValueConst connection)
{
    JSValue set = idb_connection_store_set(ctx, connection);
    JSPropertyEnum *keys = NULL;
    uint32_t count = 0, i;

    CHECK(JS_GetOwnPropertyNames(ctx, &keys, &count, set, JS_GPN_STRING_MASK) == 0,
          "IndexedDB: a connection's object store set could not be enumerated");
    for (i = 0; i < count; i++)
        JS_FreeAtom(ctx, keys[i].atom);
    js_free(ctx, keys);
    JS_FreeValue(ctx, set);
    return count;
}

/* "The version getter steps are to return this's version." THE CONNECTION'S, not the database's, which is the
   distinction the standard's own question-box draws: "once the connection has closed, this attribute will not
   reflect changes made with a later upgrade transaction". */
static JSValue js_conn_get_version(JSContext *ctx, JSValueConst this_val, int magic)
{
    double version;

    (void)magic;
    if (!conn_brand(ctx, this_val)) return JS_EXCEPTION;
    version = idb_connection_version(ctx, this_val);
    /* Only §5.8 step 3's newly-created arm leaves a connection at 0, and §5.5 step 2 has then emptied its set. */
    DCHECK(version >= 1 || conn_store_count(ctx, this_val) == 0,
           "a connection at version 0 holds object stores — stores exist only inside an upgrade that set the "
           "version to at least 1, so this world was composed rather than reached");
    return JS_NewFloat64(ctx, version);
}

/* "The objectStoreNames getter steps are: let names be a list of the names of the object stores in this's
   OBJECT STORE SET. Return the result (a DOMStringList) of creating a sorted name list with names."
   IT IS THE CONNECTION'S SET AND NOT THE DATABASE'S — §2.1.1's, "initialized to the set of object stores in
   the associated database when the connection is created" — which is the same distinction §4.5's `indexNames`
   draws for the handle's index set, and it is why this member is here and not in the storage model.
   THE SET IS KEYED BY NAME (a store is filed under its own), so the names ARE its own property keys and the
   stores are never read. The ORDER is not this member's: §2's create a sorted name list owns it, because the
   set's own order is an implementation detail and a wrong sort passes every length and containment check a
   test writes. A NEW LIST PER READ, which that algorithm's "return a NEW DOMStringList" is: a list a page took
   before `createObjectStore` ran keeps the contents it had. */
static JSValue js_conn_get_store_names(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue set, names;
    JSPropertyEnum *keys = NULL;
    uint32_t count = 0, i;

    (void)magic;
    if (!conn_brand(ctx, this_val)) return JS_EXCEPTION;
    set = idb_connection_store_set(ctx, this_val);
    names = JS_NewArray(ctx);
    CHECK(!JS_IsException(names), "IndexedDB: §4.4's list of object store names could not be allocated");
    CHECK(JS_GetOwnPropertyNames(ctx, &keys, &count, set, JS_GPN_STRING_MASK) == 0,
          "IndexedDB: a connection's object store set could not be enumerated");
    for (i = 0; i < count; i++)
        JS_DefinePropertyValueUint32(ctx, names, i, JS_AtomToString(ctx, keys[i].atom), JS_PROP_C_W_E);
    for (i = 0; i < count; i++)
        JS_FreeAtom(ctx, keys[i].atom);
    js_free(ctx, keys);
    JS_FreeValue(ctx, set);
    return idb_sorted_name_list(ctx, names);
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
typedef struct {
    uint32_t kp_valid;   /* unknown key path strings this flow decided valid (idb_key_path_valid_run) */
} ConnCsState;

/* The state owns no value: every entry re-derives the database, the transaction and the key path. */
static void conn_cs_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;
}

#define CONNCS_STAGES(X) \
    X(CONNCS_KEYPATH, "Indexed Database §4.4 createObjectStore() step 5 (keyPath is null or a valid key path, or " \
                      "a SyntaxError) - each unknown string of it forks over being valid") \
    X(CONNCS_CREATE, "Indexed Database §4.4 createObjectStore() steps 6-10 (no store named name, autoIncrement's " \
                     "key path neither empty nor a sequence, the store and its handle) - an unknown key path " \
                     "under autoIncrement forks over being empty")
enum { IDL_STEP_STAGE_BASE(CONNCS_STAGES) CONNCS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CONNCS_STEPS[] = { CONNCS_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* Step 8's "keyPath is an empty string" over unknown input: outcome 0 is non-empty (the store is created),
   outcome 1 empty (the InvalidAccessError); `real` is the example's length. The key path is borrowed from the
   converted options the machine holds. Returns 0 with *pempty set, or a step code the caller returns. */
static int conn_key_path_empty_run(JSContext *ctx, JSStepHdr *hdr, JSValueConst key_path, bool *pempty)
{
    JSValue ex = concolic_example_string(ctx, key_path);
    int real = JS_OUTCOME_REAL_UNSTATED, arm = 0, r;

    if (JS_IsString(ex)) {
        size_t len = 0;
        const char *es = JS_ToCStringLen(ctx, &len, ex);

        CHECK(es != NULL, "IndexedDB: OOM reading an unknown key path's example");
        real = len == 0 ? 1 : 0;
        JS_FreeCString(ctx, es);
    }
    JS_FreeValue(ctx, ex);
    r = step_fork_run(ctx, hdr, key_path, "Indexed Database §4.4 createObjectStore() step 8 (keyPath is the "
                                          "empty string)", 2, real, &arm);
    if (r) return r;
    *pempty = arm == 1;
    return 0;
}

/* "The createObjectStore(name, options) method steps are: ... let keyPath be options's keyPath member if it is
   not undefined or null, or null otherwise. If keyPath is not null and is not a valid key path, throw a
   SyntaxError. If an object store named name already exists in database throw a ConstraintError. Let
   autoIncrement be options's autoIncrement member. If autoIncrement is true and keyPath is an empty string or
   any sequence, throw an InvalidAccessError. Let store be a new object store in database ... Return a new
   object store handle associated with store and transaction." Steps 1-4 run on every entry: they read state
   this engine holds and the options the conversion built, and the forks below re-enter. */
static int conn_cs_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ConnCsState *s = st;
    /* `optional IDBObjectStoreParameters options = {}` means the dictionary exists whether or not the page
       wrote one, carrying every member's declared default, so it is read out of the vector, not `argc`. */
    JSValueConst this_val = hdr->this_val, options = argv[1];
    JSValue db = JS_UNDEFINED, tx = JS_UNDEFINED, key_path, existing, store, handle;
    const char *name = NULL;
    bool auto_increment;
    int r = 0;

    (void)argc; (void)out_cb; (void)out_argc;
    *presult = JS_UNDEFINED;
    JS_FreeValue(ctx, cb_result);   /* a fork's filler: no stage here waits on a request */
    DCHECK(JS_IsObject(options), "§4.4's createObjectStore was handed no options dictionary — the IDL writes "
                                 "`optional IDBObjectStoreParameters options = {}`, so the conversion builds "
                                 "one with every member at its default even for `createObjectStore(name)`");
    if (!conn_brand(ctx, this_val)) return JS_STEP_ABRUPT;
    if (conn_upgrade_transaction(ctx, this_val, &db, &tx) < 0) return JS_STEP_ABRUPT;
    key_path = idl_dict_get(ctx, options, "keyPath");
    DCHECK(!JS_IsUndefined(key_path), "IDBObjectStoreParameters' `keyPath` is declared with the IDL's own "
                                      "`= null`, so the conversion places it whether or not the page wrote "
                                      "one — an undefined here is the declaration having lost its default");
    if (hdr->stage == CONNCS_KEYPATH) {
        /* "If keyPath is not null and is not a VALID KEY PATH, throw a SyntaxError DOMException." §2.5's two
           arms over the value §3.2.25's union answered with — the IDL null, a string, or the ENGINE'S own
           array of strings — each of which may be unknown input. A list is valid when it is non-empty and
           every one of its strings is, which is why `[]` is a SyntaxError and `['']` is not. */
        if (!JS_IsNull(key_path)) {
            bool valid = false;

            r = idb_key_path_valid_run(ctx, hdr, key_path, &s->kp_valid, &valid);
            if (r) goto out;
            if (!valid) {
                JS_ThrowDOMException(ctx, "SyntaxError", "the key path is not a valid key path");
                goto fail;
            }
        }
        hdr->stage = CONNCS_CREATE;
    }
    DCHECK(hdr->stage == CONNCS_CREATE, "createObjectStore resumed at a stage it does not have");
    name = idb_name_cstr(ctx, argv[0]);
    if (name == NULL) goto fail;
    existing = idb_object_store_find(ctx, db, name);
    if (!JS_IsNull(existing)) {
        JS_FreeValue(ctx, existing);
        JS_ThrowDOMException(ctx, "ConstraintError",
                             "an object store with that name already exists in the database");
        goto fail;
    }
    JS_FreeValue(ctx, existing);

    auto_increment = idl_dict_bool(ctx, options, "autoIncrement");
    /* "If autoIncrement is true and keyPath is an EMPTY STRING or ANY SEQUENCE (empty or otherwise), throw an
       InvalidAccessError." Both arms name a key path §7.2's inject-a-key-into-a-value has nowhere to write a
       generated key: the empty key path names the VALUE ITSELF, and a list names several places at once. The
       standard's "empty or otherwise" covers a list §2.5's validity has already refused above with a
       SyntaxError, so what reaches here is a non-empty one. */
    if (auto_increment && !JS_IsNull(key_path)) {
        bool uninjectable = JS_IsArray(key_path);

        if (!uninjectable && concolic_is(key_path)) {
            r = conn_key_path_empty_run(ctx, hdr, key_path, &uninjectable);
            if (r) goto out;
        } else if (!uninjectable) {
            size_t len = 0;
            const char *ks = JS_ToCStringLen(ctx, &len, key_path);

            CHECK(ks != NULL, "IndexedDB: §4.4's key path could not be read back as a string");
            uninjectable = len == 0;
            JS_FreeCString(ctx, ks);
        }
        if (uninjectable) {
            JS_ThrowDOMException(ctx, "InvalidAccessError",
                                 "a key generator cannot be used with an empty key path or a sequence");
            goto fail;
        }
    }

    /* §4.4's "If keyPath is not null, set the created object store's key path to keyPath." — the value ITSELF,
       string or list, and not a re-derived copy of it. It is engine-owned (the union built it), so the page
       holds no reference to what the record now names — the other half of §4.5's note, whose first sentence is
       that the value its getter answers with "is not the same instance that was used when the object store
       was created". CONSUMED here. */
    store = idb_object_store_create(ctx, tx, db, name, key_path, auto_increment);
    key_path = JS_UNDEFINED;
    /* §5.7 step 3 set this transaction's scope to the connection's object store set, and this is the write
       that keeps that true — §4.10's note: "subsequent calls to [objectStoreNames] during an upgrade
       transaction can return lists with different contents as object stores are created and deleted". */
    idb_transaction_scope_add(ctx, tx, store);
    handle = idb_object_store_handle(ctx, store, tx);
    /* §2.2.1's one-handle-per-store: the handle this member returns is the one `objectStore()` must answer
       with afterwards, so it is filed rather than left for a second mint to duplicate. It is filed under the
       STORE and not under `name`, which is the key §2.2.1's sentence scopes uniqueness to and the only key a
       rename cannot invalidate — the store this member just created is one nothing else can yet hold a handle
       for, which is what the add asserts. */
    idb_transaction_handle_add(ctx, tx, handle);
    JS_FreeValue(ctx, store);
    *presult = handle;
    r = JS_STEP_DONE;
    goto out;

fail:
    r = JS_STEP_ABRUPT;
out:
    /* Every exit — done, a refusal, a fork — leaves through here. The key path is JS_UNDEFINED once the store
       record has taken it, so one free stands for every path. */
    if (name) JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, key_path);
    JS_FreeValue(ctx, db);
    JS_FreeValue(ctx, tx);
    return r;
}

static const IdlStepDecl CONNCS_DECL = {
    conn_cs_step, sizeof(ConnCsState), conn_cs_visit, NULL,
    "Indexed Database §4.4 IDBDatabase createObjectStore(name, options)", CONNCS_STEPS
};

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
    name = idb_name_cstr(ctx, argv[0]);
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
       connection's object store set IS the database's record — see the file header. */
    idb_transaction_scope_remove(ctx, tx, store);
    /* "If there is an object store handle associated with store and transaction, REMOVE ALL ENTRIES FROM ITS
       INDEX SET." The store's own set of indexes is left alone — this is the handle's §2.2.1 copy, and
       emptying it is what makes `handle.indexNames` empty for a store the page has just deleted. */
    {
        JSValue handle = idb_transaction_handle_find(ctx, tx, store);

        if (!JS_IsNull(handle))
            idb_object_store_handle_clear_index_set(ctx, handle);
        JS_FreeValue(ctx, handle);
    }
    idb_object_store_destroy(ctx, tx, db, store);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, db);
    JS_FreeValue(ctx, tx);
    return JS_UNDEFINED;
}

/* ---- §4.4's TRANSACTION ------------------------------------------------------------------------------------
 *
 * "The transaction(storeNames, mode, options) method steps are: 1. If a live upgrade transaction is associated
 * with the connection, throw an InvalidStateError. 2. If this's close pending flag is true, then throw an
 * InvalidStateError. 3. Let scope be the set of unique strings in storeNames if it is a sequence, or a set
 * containing one string equal to storeNames otherwise. 4. If any string in scope is not the name of an object
 * store in the connected database, throw a NotFoundError. 5. If scope is empty, throw an InvalidAccessError.
 * 6. If mode is not readonly or readwrite, throw a TypeError. 7. Let transaction be a newly created transaction
 * with this connection, mode, options' durability member, and the set of object stores named in scope. 8. Set
 * transaction's cleanup event loop to the current event loop. 9. Return an IDBTransaction object."
 *
 * STEP 3 IS WHY THIS MEMBER WAS ABSENT. `storeNames` is Web IDL §3.2.25's `(DOMString or sequence<DOMString>)`,
 * whose arm is chosen by ? GetMethod(V, %Symbol.iterator%) — a property read of the page's value, and therefore
 * the page's code. Reading it from this body would be a C activation hosting a page getter, so the union is a
 * DECLARED TYPE in core/idl_args.c and this body is handed either a string or the engine's own array of them;
 * "if it is a sequence" is then the one JS_IsString below and nothing else.
 *
 * STEP 6 REFUSES A VALUE THE TYPE ADMITS, which is why it is a step and not part of the declaration.
 * IDBTransactionMode lists "versionchange", so §3.2.18 accepts it and this algorithm then throws a TypeError
 * for it — an upgrade transaction is created by §5.7 and never by a page. */

typedef struct {
    uint8_t  started;
    uint32_t i;      /* the next name of storeNames steps 3-4 resolve */
    uint32_t n;      /* how many stores the scope holds */
    uint32_t cand;   /* which of the database's stores an unknown name's fork asks next */
    JSValue  scope;  /* steps 3-4's records, handed to step 7 (owned) */
    JSValue  pred;   /* that ask's IsStrictlyEqual, held across its fork (owned) */
} ConnTxState;

static void conn_tx_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    ConnTxState *s = st;

    if (!s->started) return;
    v->val(ctx, &s->scope);
    v->val(ctx, &s->pred);
}

/* The store an unknown name names among the connected database's, where no store is filed under the name its
   identity bought: which one depends on the unknown, so this forks over IsStrictlyEqual of the unknown and
   each store name in the set's order, each equal arm a predicate over the unknown's domain; the arm equal to
   none answers JS_NULL. 0 with the store in *pstore (owned), or a request code. */
static int conn_scope_fork(JSContext *ctx, JSStepHdr *hdr, ConnTxState *s, JSValueConst db, JSValueConst name_v,
                           JSValue *pstore)
{
    JSValue set = idb_database_store_set(ctx, db);
    JSPropertyEnum *keys = NULL;
    uint32_t count = 0, i;
    int r = 0, arm;

    *pstore = JS_NULL;
    CHECK(JS_GetOwnPropertyNames(ctx, &keys, &count, set, JS_GPN_STRING_MASK) == 0,
          "IndexedDB: the connected database's object store set could not be enumerated");
    for (; s->cand < count; s->cand++) {
        JSValue nm = JS_AtomToString(ctx, keys[s->cand].atom);

        if (JS_IsUninitialized(s->pred)) {
            JSValue sp[2];

            sp[0] = JS_DupValue(ctx, name_v);
            sp[1] = JS_DupValue(ctx, nm);
            CHECK(concolic_cmp_hook(ctx, sp + 2, 0, JS_CONCOLIC_EQ_STRICT),
                  "IndexedDB: the transaction scope's equality declined a name already established as unknown");
            s->pred = sp[0];
        }
        r = step_tobool_run(ctx, hdr, s->pred, "Indexed Database §4.4 transaction() step 4 (a name in "
                            "storeNames is the name of an object store in this's associated database)", &arm);
        if (r) { JS_FreeValue(ctx, nm); break; }
        JS_FreeValue(ctx, s->pred);
        s->pred = JS_UNINITIALIZED;
        if (arm) {
            const char *c = JS_ToCString(ctx, nm);

            CHECK(c != NULL, "IndexedDB: a store name of the connected database could not be read");
            *pstore = idb_object_store_find(ctx, db, c);
            JS_FreeCString(ctx, c);
            JS_FreeValue(ctx, nm);
            break;
        }
        JS_FreeValue(ctx, nm);
    }
    for (i = 0; i < count; i++)
        JS_FreeAtom(ctx, keys[i].atom);
    js_free(ctx, keys);
    JS_FreeValue(ctx, set);
    return r;
}

/* §4.4 step 3's "the set of UNIQUE strings", resolved to the §2.2 object store RECORDS step 7 names, with step
   4's "NotFoundError" thrown for the first name the connected database does not have. The uniqueness is by
   RECORD IDENTITY because idb_object_store_find answers the database's own record for a name rather than a copy
   of it, so `db.transaction(['s','s'])` names one store and its scope has one entry. An unknown name with no
   store under its identity's name forks (conn_scope_fork). 0, a request code, or -1. */
static int conn_scope_add(JSContext *ctx, JSStepHdr *hdr, ConnTxState *s, JSValueConst db, JSValueConst name_v)
{
    const char *name;
    JSValue store;
    uint32_t k;
    int r;

    DCHECK(JS_IsString(name_v) || concolic_is(name_v),
           "§4.4 step 3 was handed a scope entry that is neither a string nor unknown — the `(DOMString or "
           "sequence<DOMString>)` conversion produces strings, passing unknown input through as itself, and "
           "this engine builds the list they arrive in, so nothing else can be in it");
    name = idb_name_cstr(ctx, name_v);
    if (name == NULL) return -1;
    store = idb_object_store_find(ctx, db, name);
    JS_FreeCString(ctx, name);
    if (JS_IsNull(store) && concolic_is(name_v)) {
        r = conn_scope_fork(ctx, hdr, s, db, name_v, &store);
        if (r) return r;
    }
    if (JS_IsNull(store)) {
        JS_FreeValue(ctx, store);
        JS_ThrowDOMException(ctx, "NotFoundError",
                             "a name in the transaction's scope is not the name of an object store in the "
                             "connected database");
        return -1;
    }
    for (k = 0; k < s->n; k++) {
        JSValue have = JS_GetPropertyUint32(ctx, s->scope, k);
        bool dup = JS_VALUE_GET_PTR(have) == JS_VALUE_GET_PTR(store);

        JS_FreeValue(ctx, have);
        if (dup) {
            JS_FreeValue(ctx, store);
            return 0;
        }
    }
    JS_DefinePropertyValueUint32(ctx, s->scope, s->n++, store, JS_PROP_C_W_E);
    return 0;
}

#define CONNTX_STAGES(X) \
    X(CONNTX_SCOPE, "Indexed Database §4.4 transaction() steps 3-4 (the unique names of storeNames, each the name " \
                    "of an object store in the database or a NotFoundError) - an unknown name forks over the " \
                    "database's store names, one ask per store")
enum { IDL_STEP_STAGE_BASE(CONNTX_STAGES) CONNTX_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CONNTX_STEPS[] = { CONNTX_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int conn_tx_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ConnTxState *s = st;
    JSValueConst this_val = hdr->this_val, names = argv[0], options = argv[2];
    JSValue db = JS_UNDEFINED, upgrade, durability, tx;
    uint32_t count;
    int mode, durab, r;

    (void)argc; (void)out_cb; (void)out_argc;
    *presult = JS_UNDEFINED;
    JS_FreeValue(ctx, cb_result);
    if (s->started) goto scope;
    s->started = 1;
    s->scope = JS_UNDEFINED;
    s->pred = JS_UNINITIALIZED;
    s->i = s->n = s->cand = 0;
    if (!conn_brand(ctx, this_val)) return JS_STEP_ABRUPT;
    /* THE ONE VALUE THIS ASSERT DOES NOT SPEAK FOR IS THE ONE THE MEMBER IS EXPLORED WITH, AND THE DISJUNCT IS
       THE WHOLE OF THIS HUNK. Web IDL §3.2.18 Enumeration types is a MEMBERSHIP TEST, and the paragraph at
       step 6 below is this file's own statement that unknown external input therefore has no arm to cross as —
       but core/idl_args.h's idl_concolic_rule answers IDL_CONCOLIC_CROSSES for IDL_ENUM all the same, so
       `db.transaction(store, location.hash)` reaches THIS line still holding the unknown.
       WRITTEN WITHOUT THE DISJUNCT IT ABORTED THE WHOLE DOCUMENT HERE — seventy-odd lines ABOVE the
       `concolic_is(argv[1])` ask that is the correct crash for that value, names the fork this member owes and
       gives the arm set the enumeration itself supplies. A reader got an authoritative, reproducible and WRONG
       diagnosis ("this member's declaration is not the one that was invoked", which sends them to
       core/idl_args.c) in place of the right one further down the same body. That is the stale-`DFAIL` failure
       mode with the two crashes in ONE function: the assert was true of every value except the one that
       reaches it, and being nearly true is what made it worse than silence.
       WHAT IT STILL STATES is what the declaration actually guarantees — every value that is NOT unknown
       external input arrives converted — which is the half a body may rely on. */
    DCHECK(JS_IsString(argv[1]) || concolic_is(argv[1]),
           "§4.4's transaction was handed a mode that is neither a string nor unknown external input — "
           "`optional IDBTransactionMode mode = \"readonly\"` is an enumeration WITH a default, so §3.6 steps "
           "15.4.1 and 16.1 place one whether or not the page passed one, and Web IDL §3.2.18 Enumeration "
           "types converts every other value a page can write. An unknown crosses as itself and is answered "
           "at step 6 below, which is where this member's missing fork is named");
    DCHECK(JS_IsObject(options),
           "§4.4's transaction was handed no options dictionary — the IDL writes `optional "
           "IDBTransactionOptions options = {}`, so the conversion builds one carrying every declared default");

    db = idb_connection_database(ctx, this_val);
    /* Step 1. The database's upgrade transaction is cleared when it finishes, so a non-null one is a LIVE one;
       "associated with THE CONNECTION" is the comparison below, because §5.1 lets a second connection exist
       while an upgrade runs and that connection's step 1 does not throw (its step 2 does). */
    upgrade = idb_database_upgrade_transaction(ctx, db);
    if (!JS_IsNull(upgrade)) {
        JSValue owner = idb_transaction_connection(ctx, upgrade);
        bool mine = JS_VALUE_GET_PTR(owner) == JS_VALUE_GET_PTR(this_val);

        JS_FreeValue(ctx, owner);
        JS_FreeValue(ctx, upgrade);
        if (mine) {
            JS_FreeValue(ctx, db);
            JS_ThrowDOMException(ctx, "InvalidStateError", "an upgrade transaction is live on this connection, so "
                                                           "no other transaction can be created with it");
            return JS_STEP_ABRUPT;
        }
    } else {
        JS_FreeValue(ctx, upgrade);
    }
    /* Step 2 — §5.2's note: "Once a connection's close pending flag has been set to true, no new
       transactions can be created using the connection." */
    if (idb_connection_close_pending(ctx, this_val)) {
        JS_FreeValue(ctx, db);
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "the connection is closing, so no new transaction can be created with it");
        return JS_STEP_ABRUPT;
    }
    JS_FreeValue(ctx, db);
    s->scope = JS_NewArray(ctx);
    CHECK(!JS_IsException(s->scope), "IndexedDB: the transaction's scope could not be allocated");
    hdr->stage = CONNTX_SCOPE;

scope:
    /* Steps 3 and 4, together, because step 4 asks its question of every string step 3 collected and the
       records are what step 7 wants anyway. A string, or an unknown on the string arm §3.2.25 forked, is one
       name; the sequence arm is the engine's own Array, whose elements may be unknown names. */
    db = idb_connection_database(ctx, this_val);
    if (JS_IsString(names) || concolic_is(names)) {
        count = 1;
    } else {
        JSValue lv;

        DCHECK(JS_IsArray(names),
               "§4.4 step 3 was handed a storeNames that is neither a string nor a sequence — §3.2.25's union "
               "answers with one or the other and this engine builds the sequence itself");
        lv = JS_GetPropertyStr(ctx, names, "length");
        CHECK(JS_ToUint32(ctx, &count, lv) == 0,
              "IndexedDB: the length of the transaction's scope sequence could not be read");
        JS_FreeValue(ctx, lv);
    }
    for (; s->i < count; s->i++) {
        JSValue e = count == 1 && !JS_IsArray(names) ? JS_DupValue(ctx, names) : JS_GetPropertyUint32(ctx, names, s->i);

        r = conn_scope_add(ctx, hdr, s, db, e);
        JS_FreeValue(ctx, e);
        if (r) {
            JS_FreeValue(ctx, db);
            return r < 0 ? JS_STEP_ABRUPT : r;
        }
        s->cand = 0;
    }
    /* Step 5. It is AFTER step 4 and the order is observable: `db.transaction([])` is an "InvalidAccessError"
       while `db.transaction(['nope'])` is a "NotFoundError". */
    if (s->n == 0) {
        JS_ThrowDOMException(ctx, "InvalidAccessError",
                             "a transaction cannot be created with an empty scope");
        goto fail;
    }
    /* Step 6 — see the header comment: the TYPE admits "versionchange" and this algorithm refuses it.
       BOTH ROADS INTO conn_enum_index CARRY AN UNKNOWN, and the ask is written at each of them rather than
       inside the helper, because what a reader needs is the ARM LIST and the two lists differ. Web IDL
       §3.2.18 Enumeration types is "Let S be the result of calling ? ToString(V)" and then "If S is not one
       of E's enumeration values, then throw a TypeError" — the type admits the listed strings and nothing
       else, so unknown external input has no arm to cross as here, where a DOMString member legitimately
       crosses. It reaches conn_enum_index's JS_ToCString as an unknown either way, which is a boundary that
       owes C real bytes and has none. */
    if (concolic_is(argv[1]))
        DFAIL("IDBDatabase.transaction's `mode` ARGUMENT is UNKNOWN EXTERNAL INPUT. Web IDL §3.2.18 "
              "Enumeration types refuses an unrecognised value with a TypeError, so it cannot cross as "
              "itself, and it must not be picked: `readonly` and `readwrite` are two worlds this engine "
              "observes differently — every mutating member opens with Indexed Database API §4.5 The "
              "IDBObjectStore interface's own step, \"If transaction is a read-only transaction, throw a "
              "\"ReadOnlyError\" DOMException.\" — so the arm decides whether a whole branch of the page's "
              "storage code runs at all — and `versionchange` is the third value the TYPE admits and step 6 "
              "directly below refuses. The arm "
              "set is GIVEN by the enumeration rather than chosen, which is what makes an N-way ask honest "
              "where a range's would not be: ask step_fork_run over TX_MODES plus the TypeError arm at the "
              "§3.2.18 conversion, outcome 0 the first listed value. Its subproblem — the two-armed §3.2.3 "
              "boolean, at the argument boundary AND at the dictionary member — is built, at the conversion "
              "and not in the bodies, which is the shape this ask takes too");
    mode = conn_enum_index(ctx, argv[1], TX_MODES);
    if (mode != IDB_TX_READONLY && mode != IDB_TX_READWRITE) {
        JS_ThrowTypeError(ctx, "a transaction cannot be created with the mode \"versionchange\" — an upgrade "
                               "transaction is created by the steps to upgrade a database");
        goto fail;
    }

    /* Steps 7, 8 and 9. §4.9's creation takes the scope (CONSUMED), and the cleanup event loop is set HERE and
       not inside the creation because §5.7's upgrade transaction must not have one — see idb_transaction.h. */
    durability = idl_dict_get(ctx, options, "durability");
    /* THE MEMBER ROAD to the same helper, and the same missing fork. The declaration's `= "default"` is
       placed only when the page wrote NOTHING; `{durability: location.hash}` is a member the page wrote, so
       §3.2.17's loop crosses it and the assert below — which reads the absence of a string as the default
       having gone missing — reported a conversion failure for a value the conversion crossed on purpose. */
    if (concolic_is(durability))
        DFAIL("IDBTransactionOptions' `durability` MEMBER is UNKNOWN EXTERNAL INPUT. Web IDL §3.2.18 "
              "Enumeration types refuses an unrecognised value with a TypeError, so it cannot cross as "
              "itself, and picking one deletes the others: `enum IDBTransactionDurability { \"default\", "
              "\"strict\", \"relaxed\" }` is three values a page reads back off IDBTransaction's own "
              "`durability` getter and can branch on. The arm set is GIVEN by the enumeration, so ask "
              "step_fork_run over TX_DURABILITY plus the TypeError arm at the §3.2.18 conversion, outcome 0 "
              "the first listed value — the same one mechanism the `mode` argument above names, which is why "
              "it belongs at the conversion and not in either body");
    DCHECK(JS_IsString(durability),
           "IDBTransactionOptions' `durability` is declared with the IDL's own `= \"default\"`, so the "
           "conversion places it whether or not the page wrote one — an absent one here is the declaration "
           "having lost its default");
    durab = conn_enum_index(ctx, durability, TX_DURABILITY);
    JS_FreeValue(ctx, durability);
    tx = idb_transaction_new(ctx, this_val, s->scope, mode, durab);   /* takes the scope */
    s->scope = JS_UNDEFINED;
    idb_transaction_set_cleanup_loop(ctx, tx);
    JS_FreeValue(ctx, db);
    *presult = tx;
    return JS_STEP_DONE;

fail:
    JS_FreeValue(ctx, db);
    return JS_STEP_ABRUPT;
}

static const IdlStepDecl CONNTX_DECL = {
    conn_tx_step, sizeof(ConnTxState), conn_tx_visit, NULL,
    "Indexed Database §4.4 IDBDatabase transaction(storeNames, mode, options)", CONNTX_STEPS
};

/* ---- install ------------------------------------------------------------------------------------------------ */

static void idb_connection_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_conn_class != 0, "a realm asked for IDBDatabase.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_conn_class);
    DCHECK(JS_IsNull(prev), "idb_connection_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = event_target_derived_proto(ctx);   /* `interface IDBDatabase : EventTarget` */
    idl_interface_tag(ctx, proto, "IDBDatabase");
    event_target_install_handlers(ctx, proto, EH_IDB_DATABASE);
    idl_install_accessor(ctx, proto, "name", js_conn_get_name, 0, -1);
    idl_install_accessor(ctx, proto, "version", js_conn_get_version, 0, -1);
    idl_install_accessor(ctx, proto, "objectStoreNames", js_conn_get_store_names, 0, -1);
    idl_install_method(ctx, proto, "transaction", g_id_transaction);
    idl_install_method(ctx, proto, "close", g_id_close);
    idl_install_method(ctx, proto, "createObjectStore", g_id_create_store);
    idl_install_method(ctx, proto, "deleteObjectStore", g_id_delete_store);
    JS_SetClassProto(ctx, g_conn_class, JS_DupValue(ctx, proto));

    ctor = idl_interface_object(ctx, "IDBDatabase", proto);
    CHECK(!JS_IsException(ctor), "the IDBDatabase interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "IDBDatabase", ctor);
    JS_FreeValue(ctx, global);
}

void idb_connection_init(JSContext *ctx)
{
    JSClassDef d = { "IDBDatabase" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CREATE_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    static const IdlArgType DELETE_ARGS[1] = { IDL_DOMSTRING };
    /* `[NewObject] IDBTransaction transaction((DOMString or sequence<DOMString>) storeNames,
        optional IDBTransactionMode mode = "readonly", optional IDBTransactionOptions options = {});` — the
       union is what kept this member out, and it is a declared type because its arm is decided by a read of
       the page's value. `mode` carries §3.2.18's value list AND §3.6 steps 15.4.1 and 16.1's default, both
       stated below. */
    static const IdlArgType TX_ARGS[3] = { IDL_DOMSTRING_OR_SEQUENCE, IDL_ENUM, IDL_DICT };
    /* `dictionary IDBTransactionOptions { IDBTransactionDurability durability = "default"; };` — one member,
       whose value list IS its type and whose default its IDL writes, so the body reads a value that is there
       rather than inventing the "default". */
    static const IdlDictMember TX_OPTIONS[] = {
        { "durability", IDL_ENUM, false, TX_DURABILITY, 0, NULL, IDL_DEFAULT_STRING, "default" },
    };
    /* `dictionary IDBObjectStoreParameters { (DOMString or sequence<DOMString>)? keyPath = null; boolean
       autoIncrement = false; }` — one level, so §3.2.17 reads them in the order they are written here, which
       a page pins by throwing from a member's getter. `keyPath` STATES ITS OWN UNION, and the row it replaces
       was wrong about more than compound key paths: declared IDL_ANY, the page's value crossed unconverted, so
       `createObjectStore("s", {keyPath: {}})` reached the body as an object and hit the not-a-string arm —
       where §3.2.25 has no sequence to find, takes step 16, and makes it the string "[object Object]", which
       §2.5 then refuses with a "SyntaxError" exactly as a browser does. The DEFAULT is declared beside it, so
       the body reads a value that is there rather than inventing the null. */
    static const IdlDictMember CREATE_INIT[] = {
        { "autoIncrement", IDL_BOOLEAN },
        { "keyPath", IDL_DOMSTRING_OR_SEQUENCE_NULLABLE, false, NULL, 0, NULL, IDL_DEFAULT_NULL },
    };

    DCHECK(!g_ready, "idb_connection_init ran twice — one instance is one document is one agent");
    g_key = JS_NewSymbol(ctx, "idbDatabaseState", false);
    CHECK(!JS_IsException(g_key), "the IDBDatabase slot key allocation failed");
    g_conn_rt = rt;
    JS_NewClassID(rt, &g_conn_class);
    CHECK(JS_NewClass(rt, g_conn_class, &d) == 0,
          "IDBDatabase: the per-realm prototype slot could not be declared");
    g_id_close = idl_method_id(ctx, NULL, 0, js_conn_close, 0);
    g_id_transaction = idl_method_id_step(ctx, TX_ARGS, 3, TX_OPTIONS,
                                          (int)(sizeof TX_OPTIONS / sizeof TX_OPTIONS[0]), &CONNTX_DECL, 0);
    idl_optional_from(1);                                 /* `mode` and `options` are both optional */
    idl_arg_enum(1, TX_MODES);                            /* §3.2.18's value list for the `mode` position */
    idl_arg_default(1, IDL_DEFAULT_STRING, "readonly");   /* §3.6 steps 15.4.1 and 16.1's `= "readonly"` */
    g_id_create_store = idl_method_id_step(ctx, CREATE_ARGS, 2, CREATE_INIT,
                                           (int)(sizeof CREATE_INIT / sizeof CREATE_INIT[0]), &CONNCS_DECL, 0);
    idl_optional_from(1);                      /* `optional IDBObjectStoreParameters options = {}` */
    g_id_delete_store = idl_method_id(ctx, DELETE_ARGS, 1, js_conn_delete_object_store, 0);
    g_ready = 1;
    agent_state_flag("idb_connection", &g_ready, "the declaration latch");
    agent_state_ptr("idb_connection", &g_conn_rt, "the runtime §2.1.1's slot key was minted in");
    agent_state_value("idb_connection", &g_key, "§2.1.1's internal-slot key");
    /* AND THE CLASS ID — core/agent_state.h, which this component has told about every slot but
       this one. §4.4 "The IDBDatabase interface"'s class is registered in THIS runtime, so an id
       carried into the next agent names a class in a runtime that is gone, while that agent's
       allocator restarts at JS_CLASS_INIT_COUNT and hands the same number to somebody else — and
       because the id is also the handle every realm reaches the prototype through, nothing in this
       file would re-register it. It was minted inside the window core/platform.c's declare column
       brackets and named to no declaration, which is the direction that file's conservation
       identity aborts on. */
    agent_state_class("idb_connection", &g_conn_class, "§4.4's IDBDatabase class");
    realm_declare_intrinsic(idb_connection_install_realm);
}

void idb_connection_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — see idb_transaction_free. */
    DCHECK(g_ready, "§2.1.1's connection machinery was released in an agent that never declared it");
    DCHECK(rt == g_conn_rt, "idb_connection_free was given a runtime that is not the one it declared into");
    JS_FreeValueRT(rt, g_key);
    /* THE HAND-RESET LIST THAT STOOD HERE IS GONE RATHER THAN EXTENDED. agent_state_undo puts back
       every slot carrying this row's name out of the registry that already holds each one's address
       and its kind, so the list this function kept in step with idb_connection_init's
       declarations is COMPUTED rather than remembered — the class id joins it with no line here to
       forget, as will whatever that init declares next. The JS_FreeValueRT call stays: the undo
       resets HANDLES and never references, and freeing what a slot names is this component's own
       work. It is the LAST line, because the DCHECKs in this function ask about slots it nulls. */
    agent_state_undo("idb_connection");
}
