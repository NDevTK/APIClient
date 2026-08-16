/* INDEXED DATABASE §2.1's DATABASE, §2.2's OBJECT STORE, and §6.1/§6.2 — the put/get round trip, as real state.
 *
 * WHY THIS IS SUBPROBLEM THREE. §2.4's key came first because §2.2's list of records is "sorted according to key
 * in ascending order" and a store written before the ordering existed would be a list with no order; §2.9's key
 * range came second because §6.2 retrieves "the first record in store's list of records whose key is IN RANGE".
 * Both of those are now here to be written against, and the two algorithms below are the whole of what an object
 * store DOES: a record goes in under a key, and a record comes back out by range. Everything above them — §2.7's
 * transaction, §2.8's request, §4.4-§4.8's interfaces, §5.1's open — is the machinery that DECIDES WHEN these
 * run, and none of it can be written before the operation it schedules exists.
 *
 * WHAT A STORED VALUE IS, WHICH IS THE DECISION THIS FILE IS BUILT AROUND.
 *
 * §2.3 says "Record values are Records output by the StructuredSerializeForStorage operation", and the standard
 * composes FOUR serialization halves for one put/get: §5.11 clones the page's value (serialize, deserialize) at
 * put time, §6.1 serializes that clone for storage, and §6.2 deserializes it for the reader. The middle pair is
 * the standard's own optimisation to give away — §6's NOTE says its deserializations "can be asserted not to
 * throw ... because they operate only on previous output of StructuredSerializeForStorage" — and nothing can
 * observe it, because the clone §5.11 built is reachable by no page: it is created inside the algorithm and
 * handed straight to §6.1. So the record's value here is a LIVE COPY and §6.2 copies it again on the way out.
 *
 * TWO INDEPENDENT REASONS SAY IT MUST BE, AND THE FIRST HAS NOTHING TO DO WITH THE SOLVER. A byte buffer on a
 * record is malloc'd C reached through a pointer, and §State-isolation names that failure exactly: the per-flow
 * COW delta captures the POINTER, a context switch reverts it, and the bytes are left reachable from nothing —
 * a leak the runtime's own GC walk cannot see. A record's key is already a JS value for that reason
 * (idb_key.c's own header states it), and a record whose key time-travels and whose value does not is half a
 * record. The value is a JS value, and the only JS value that IS §6.1's serialization is the copy itself.
 *
 * THE SECOND REASON IS THAT A STORED VALUE IS AN ATTACKER SOURCE AND MUST COME BACK STILL TAINTED. A page keeps
 * its session in here — `store.put(location.hash)`, `store.get('user')` into a sink — and §Attacker-sources
 * names IndexedDB as a source whose intrinsic constraints have to be modelled. The engine's serializer refuses a
 * concolic, because a concolic is a host-class object, so a store that went through bytes would answer
 * `store.put(location.hash)` with a "DataCloneError" where a browser stores a string: the whole surface out of
 * reach, and every login-gated path behind it unexplored. A store that de-tainted on the way in would be worse
 * than that — it would return a plausible datum, which is the one failure §Offensive-programming says nothing
 * can distinguish from a measurement, and the fork the read should have produced would be silently gone.
 *
 * SO THE CONCOLIC ARM IS IN THE CLONE AND NOT HERE. core/structured_clone.c seeds §2.7.1's `memory` with the
 * concolic itself: a concolic stands for a PRIMITIVE (§2.7.3's first arm) and a primitive is its own copy, so
 * what comes back is the SAME symbol rather than a second one about which the flow's narrowing says nothing.
 * This file then needs no arm of its own — it stores what the clone gives it, and the taint rides the value
 * through §6.1 and back out of §6.2 because it rides the value everywhere else.
 *
 * AND IT ALL TIME-TRAVELS FOR FREE. The set of databases, a database's set of object stores, a store's list of
 * records and each record's two fields are internal-slot records and one Array (core/idl_slots.h), so every
 * write this file performs is a PROPERTY WRITE the per-flow COW delta already captures: flow A's `put` is
 * invisible to its sibling, and a flow parked mid-transaction resumes seeing exactly the records it wrote.
 * There is no host record behind a class opaque here and therefore nothing for cow_capture_host_record to
 * capture — that mechanism is for state a component keeps in C, and this component keeps none.
 *
 * EVERY CHANGE NAMES THE TRANSACTION MAKING IT, which is §2.7's first sentence ("whenever data is read or
 * written to the database it is done by using a transaction") and is what makes §5.5 step 2's revert possible
 * at all: a change whose transaction is unknown is a change nothing can undo. So each of the five mutations
 * below takes the transaction ahead of the state it changes, asserts what §2.7 says that transaction may do,
 * and records the INVERSE of what it is about to write. The revert is at the bottom of this file, beside the
 * writes it runs backwards. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/structured_clone.h"

/* §2.1's, §2.2's and the record's OWN FIELDS, spelled once so the writers and the readers cannot disagree. */
#define IDB_DB_NAME             "name"
#define IDB_DB_VERSION          "version"
#define IDB_DB_STORES           "stores"        /* §2.1's "set of object stores", keyed by name */
#define IDB_DB_UPGRADE          "upgradeTransaction"  /* §2.1's upgrade transaction, JS_NULL when there is none */
#define IDB_DB_CONNECTIONS      "connections"   /* §2.1.1's open connections to this database, in join order */

#define IDB_STORE_NAME          "name"
#define IDB_STORE_KEY_PATH      "keyPath"       /* §2.2's optional key path — JS_NULL for out-of-line keys */
#define IDB_STORE_KEY_GENERATOR "keyGenerator"  /* §2.2's optional key generator */
#define IDB_STORE_RECORDS       "records"       /* §2.2's "list of records ... sorted according to key" */
#define IDB_STORE_DELETED       "deleted"       /* §4.4's "Destroy store" — see idb_object_store_destroy */

#define IDB_RECORD_KEY          "key"
#define IDB_RECORD_VALUE        "value"

/* §5.5 step 2's ONE CHANGE, and the five kinds this engine can make. Each names the state it is about to write
   and what that state held BEFORE — which is the whole of the change, because the revert is the same write with
   the remembered operand. A kind is added when a mutation is, and the revert's last arm ASSERTS which kind it
   is rather than accepting whatever is left: a kind nobody wrote a revert for has to abort at the revert, not
   quietly leave that change standing in a database an abort has told the page is untouched. */
#define IDB_CHANGE_KIND         "kind"
#define IDB_CHANGE_DB           "db"
#define IDB_CHANGE_STORE        "store"
#define IDB_CHANGE_KEY          "key"
#define IDB_CHANGE_PRIOR        "prior"    /* the record that key held before, or JS_NULL for none */
#define IDB_CHANGE_NAME         "name"     /* the name the store was filed under before */
#define IDB_CHANGE_VERSION      "version"  /* the version the database held before */

enum { IDB_CHANGE_RECORD_STORED, IDB_CHANGE_STORE_CREATED, IDB_CHANGE_STORE_DESTROYED,
       IDB_CHANGE_STORE_RENAMED, IDB_CHANGE_VERSION_SET };

/* §2.1's SET OF DATABASES for this agent's storage key. See the header for why it is the AGENT's. */
static JSValue g_databases = JS_UNDEFINED;

/* ---- the records, as this file reads them ------------------------------------------------------------------ */

/* HOW LONG ONE OF THIS FILE'S OWN LISTS IS. The list is engine-built and holds engine-built records, so a
   malformed one is the component disagreeing with itself and crashes rather than reporting. */
static uint32_t idb_list_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;
    int r;

    DCHECK(!JS_IsException(len), "reading the length of an object store's list of records threw — it is this "
                                 "component's own Array and has no getters to run");
    r = JS_ToUint32(ctx, &n, len);
    DCHECK(r >= 0, "an object store's list of records had a length that is not a number");
    (void)r;
    JS_FreeValue(ctx, len);
    return n;
}

/* REMOVE a name from one of this file's own keyed records. A slot record has a NULL prototype and holds only
   what this component wrote (core/idl_slots.h), so the atom round-trip is the whole of the operation and a
   failure to intern one is an allocation failure rather than anything the page can cause. */
static void idb_slots_delete(JSContext *ctx, JSValueConst rec, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);

    CHECK(a != JS_ATOM_NULL, "IndexedDB: a store name could not be interned to be removed");
    JS_DeleteProperty(ctx, rec, a, 0);
    JS_FreeAtom(ctx, a);
}

static JSValue idb_store_records(JSContext *ctx, JSValueConst store)
{
    JSValue records = JS_GetPropertyStr(ctx, store, IDB_STORE_RECORDS);

    DCHECK(JS_IsArray(records), "an object store carried no list of records — every store is built by "
                                "idb_object_store_create, which gives it one, so anything else is not a store");
    return records;
}

/* §2.4's COMPARE against the key of the record at `i`. The list is sorted by exactly this, so both the search
   below and the invariant it asserts afterwards are stated in the one comparison rather than in two. */
static int idb_record_key_compare(JSContext *ctx, JSValueConst records, uint32_t i, JSValueConst key)
{
    JSValue rec = JS_GetPropertyUint32(ctx, records, i);
    JSValue rkey = JS_GetPropertyStr(ctx, rec, IDB_RECORD_KEY);
    int c;

    DCHECK(JS_IsObject(rkey), "a record in an object store carried no key — a record is a key AND a value "
                              "(§2.2), and this list holds only records idb_store_record built");
    c = idb_key_compare(ctx, rkey, key);
    JS_FreeValue(ctx, rkey);
    JS_FreeValue(ctx, rec);
    return c;
}

/* WHERE `key` BELONGS in a store's sorted list of records, and whether the list already holds one under it. The
   search is BINARY because §2.2 keeps the list "sorted according to key in ascending order" — that ordering is
   not decoration, it is what makes a store an index rather than a bag, and a walk here would quietly make every
   put O(n). `lo` lands on the first record whose key is not less than `key`, which is both the insertion point
   and — when it compares equal — §6.1's "a record already exists in store with its key equal to key". It is one
   function because §6.1 and §5.5 step 2's revert of a §6.1 must land on the SAME position: a revert that found
   its record by a second search written differently would put a record back where the put had not been. */
static uint32_t idb_record_pos(JSContext *ctx, JSValueConst records, JSValueConst key, bool *exists)
{
    uint32_t n = idb_list_len(ctx, records), lo = 0, hi = n, mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (idb_record_key_compare(ctx, records, mid, key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *exists = lo < n && idb_record_key_compare(ctx, records, lo, key) == 0;
    return lo;
}

/* ONE CHANGE, ADDRESSED TO THE TRANSACTION THAT IS MAKING IT. The record is built here and filed by
   idb_transaction_record_change, which is where §2.7's two facts about that transaction are asserted — this
   file states WHAT changed and the transaction states that it was allowed to. CONSUMED by the caller's file. */
static JSValue idb_change_new(JSContext *ctx, int kind)
{
    JSValue c = idl_slots_new(ctx);

    CHECK(!JS_IsException(c), "IndexedDB: §5.5 step 2's record of a database change could not be allocated");
    JS_SetPropertyStr(ctx, c, IDB_CHANGE_KIND, JS_NewInt32(ctx, kind));
    return c;
}

static int idb_change_kind(JSContext *ctx, JSValueConst change)
{
    JSValue v = JS_GetPropertyStr(ctx, change, IDB_CHANGE_KIND);
    int32_t k = -1;
    int r = JS_ToInt32(ctx, &k, v);

    DCHECK(r >= 0, "a recorded database change carried no kind — only idb_change_new builds them");
    (void)r;
    JS_FreeValue(ctx, v);
    return (int)k;
}

static JSValue idb_change_get(JSContext *ctx, JSValueConst change, const char *field)
{
    JSValue v = JS_GetPropertyStr(ctx, change, field);

    DCHECK(!JS_IsUndefined(v), "a recorded database change is missing an operand its kind is made of — the "
                               "kind and the fields are written together by the mutation that recorded it");
    return v;
}

/* ---- §2.1's SET OF OBJECT STORES, as the two writes every change to it is made of ----------------------------
 *
 * A store LEAVES the set, or JOINS it under a name. Everything §4.4 and §4.5 do to that set is one or both of
 * those, and so is everything §5.5 step 2 undoes — a destroy is a leave, its revert is a join; a rename is a
 * leave and a join, and so is its revert. They are factored because a second copy of either would be a second
 * place the set is written from, and this file's whole claim is that there is one. */

static void idb_store_set_remove(JSContext *ctx, JSValueConst db, JSValueConst store)
{
    JSValue stores = idb_database_store_set(ctx, db), name = idb_object_store_name(ctx, store), held;
    const char *cname = JS_ToCString(ctx, name);

    CHECK(cname != NULL, "IndexedDB: a store leaving its database's set could not report its own name");
    held = JS_GetPropertyStr(ctx, stores, cname);
    DCHECK(JS_VALUE_GET_PTR(held) == JS_VALUE_GET_PTR(store),
           "a store left its database's set of object stores under a name that set files a DIFFERENT store "
           "under — §2.2 makes the name unique within the database and this file is the only writer of both "
           "the name and the set, so the two disagreeing means one of them was written by something else");
    JS_FreeValue(ctx, held);
    idb_slots_delete(ctx, stores, cname);
    JS_FreeCString(ctx, cname);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, stores);
}

static void idb_store_set_file(JSContext *ctx, JSValueConst db, JSValueConst store, const char *name)
{
    JSValue stores = idb_database_store_set(ctx, db), existing;

    DCHECK(name != NULL, "a store was filed in its database's set under no name");
    existing = JS_GetPropertyStr(ctx, stores, name);
    DCHECK(JS_IsUndefined(existing), "a store was filed under a name its database already holds one for — §2.2: "
                                     "\"at any one time, the name is unique within the database to which it "
                                     "belongs\", which §4.4's createObjectStore and §4.5's name setter each "
                                     "report as a ConstraintError before they reach this");
    JS_FreeValue(ctx, existing);
    /* The store's OWN name and the key it is filed under are one fact written in one place, which is what makes
       idb_store_set_remove able to find a store by asking it its name. */
    JS_SetPropertyStr(ctx, (JSValue)store, IDB_STORE_NAME, JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, stores, name, JS_DupValue(ctx, store));
    JS_FreeValue(ctx, stores);
}

/* §4.4's "Destroy store", which §5.5 step 2 also performs on a store this transaction CREATED: "any object
   stores and indexes which were created during the transaction are now considered deleted for the purposes of
   other algorithms." THE RECORD IS MARKED AND NOT FREED, because a handle the page already holds still names it
   and §4.5's members each ask "has this store been deleted" as their first step. */
static void idb_store_destroy_raw(JSContext *ctx, JSValueConst db, JSValueConst store)
{
    idb_store_set_remove(ctx, db, store);
    JS_SetPropertyStr(ctx, (JSValue)store, IDB_STORE_DELETED, JS_TRUE);
}

/* ---- §2.1's database ---------------------------------------------------------------------------------------- */

void idb_database_init(JSContext *ctx)
{
    DCHECK(JS_IsUndefined(g_databases), "idb_database_init ran twice — §2.1's set of databases belongs to the "
                                        "AGENT, and a second one would give a same-origin child navigable a "
                                        "storage area of its own");
    g_databases = idl_slots_new(ctx);
    CHECK(!JS_IsException(g_databases), "IndexedDB: §2.1's set of databases could not be allocated");
    agent_state_value("idb_database", &g_databases,
                      "§2.1's set of databases for this storage key, and the declaration latch");
}

void idb_database_free(JSRuntime *rt)
{
    JS_FreeValueRT(rt, g_databases);
    g_databases = JS_UNDEFINED;
}

JSValue idb_database_find(JSContext *ctx, const char *name)
{
    JSValue db;

    DCHECK(!JS_IsUndefined(g_databases), "§2.1's set of databases was asked before idb_database_init built it");
    DCHECK(name != NULL, "a database was looked up under no name — §2.1's name identifies it within a storage "
                         "key, and the empty string is a name while a null pointer is not one");
    db = JS_GetPropertyStr(ctx, g_databases, name);
    CHECK(!JS_IsException(db), "IndexedDB: §2.1's set of databases could not be read");
    if (JS_IsUndefined(db))
        return JS_NULL;
    return db;
}

JSValue idb_database_create(JSContext *ctx, const char *name)
{
    JSValue existing = idb_database_find(ctx, name), db, stores, connections;

    DCHECK(JS_IsNull(existing), "a database was created under a name this storage key already holds one for — "
                                "§2.1 gives a storage key ONE database per name, and §5.1's open asks for the "
                                "existing one before it creates anything");
    JS_FreeValue(ctx, existing);
    db = idl_slots_new(ctx);
    CHECK(!JS_IsException(db), "IndexedDB: §2.1's database record could not be allocated");
    stores = idl_slots_new(ctx);
    CHECK(!JS_IsException(stores), "IndexedDB: a database's set of object stores could not be allocated");
    JS_SetPropertyStr(ctx, db, IDB_DB_NAME, JS_NewString(ctx, name));
    /* "When a database is first created, its version is 0 (zero)." The only thing that changes it is an
       upgrade transaction, which is §5.1's business and not this record's. */
    JS_SetPropertyStr(ctx, db, IDB_DB_VERSION, JS_NewFloat64(ctx, 0));
    /* "When a new database is created it doesn't contain any object stores." */
    JS_SetPropertyStr(ctx, db, IDB_DB_STORES, stores);
    /* "A database has at most one associated upgrade transaction, which is either null or an upgrade
       transaction, and is INITIALLY NULL." */
    JS_SetPropertyStr(ctx, db, IDB_DB_UPGRADE, JS_NULL);
    connections = JS_NewArray(ctx);
    CHECK(!JS_IsException(connections), "IndexedDB: a database's set of connections could not be allocated");
    JS_SetPropertyStr(ctx, db, IDB_DB_CONNECTIONS, connections);
    JS_SetPropertyStr(ctx, g_databases, name, JS_DupValue(ctx, db));
    return db;
}

JSValue idb_database_name(JSContext *ctx, JSValueConst db)
{
    JSValue v = JS_GetPropertyStr(ctx, db, IDB_DB_NAME);

    DCHECK(JS_IsString(v), "a database carried no NAME — §2.1 gives every database one and only "
                           "idb_database_create writes it");
    return v;
}

/* THE FIELD, which §5.7 step 8 moves forwards and §5.5 step 2 moves back. The revert is the only writer that
   may put a ZERO here — that is the version a database is created with — so the assert that no algorithm may
   write one belongs to the public setter and not to this. */
static void idb_database_write_version(JSContext *ctx, JSValueConst db, double version)
{
    DCHECK(JS_IsObject(db), "a version was written on something that is not a §2.1 database");
    DCHECK(version >= 0, "a database was given a NEGATIVE version — §2.1's version is \"a 64-bit integer\" "
                         "that §4.3's `open` refuses below 1 and that this file creates at 0");
    JS_SetPropertyStr(ctx, (JSValue)db, IDB_DB_VERSION, JS_NewFloat64(ctx, version));
}

void idb_database_set_version(JSContext *ctx, JSValueConst tx, JSValueConst db, double version)
{
    JSValue change;

    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "a database's VERSION was changed by a transaction that is not an upgrade transaction — §2.1: \"the "
           "only way to change the version is using an upgrade transaction\"");
    DCHECK(version >= 1, "§5.1 step 5 makes a version at least 1 — 0 is the version a database is CREATED "
                         "with and §4.3's `open` reports a zero version as a TypeError before §5.1 is reached, "
                         "so nothing may write one back");
    /* §5.7 step 8's own sentence: "this change is considered part of the transaction, and so if the transaction
       is aborted, this change is reverted." The old version is what that revert writes, so it is remembered
       here — §5.7 keeps its own copy for the `upgradeneeded` event's `oldVersion`, which is a different
       question asked at a different time and answered from the entry that fires it. */
    change = idb_change_new(ctx, IDB_CHANGE_VERSION_SET);
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_DB, JS_DupValue(ctx, db));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_VERSION, JS_NewFloat64(ctx, idb_database_version(ctx, db)));
    idb_transaction_record_change(ctx, tx, change);
    idb_database_write_version(ctx, db, version);
}

JSValue idb_database_upgrade_transaction(JSContext *ctx, JSValueConst db)
{
    JSValue v = JS_GetPropertyStr(ctx, db, IDB_DB_UPGRADE);

    DCHECK(!JS_IsUndefined(v), "a database carried no upgrade-transaction field — §2.1 gives every database "
                               "one, initially null, and only idb_database_create builds them");
    return v;
}

void idb_database_set_upgrade_transaction(JSContext *ctx, JSValueConst db, JSValue tx)
{
    DCHECK(JS_IsObject(db), "an upgrade transaction was written on something that is not a §2.1 database");
    JS_SetPropertyStr(ctx, (JSValue)db, IDB_DB_UPGRADE, tx);
}

JSValue idb_database_connections(JSContext *ctx, JSValueConst db)
{
    JSValue v = JS_GetPropertyStr(ctx, db, IDB_DB_CONNECTIONS);

    DCHECK(JS_IsArray(v), "a database carried no set of connections");
    return v;
}

void idb_database_add_connection(JSContext *ctx, JSValueConst db, JSValueConst connection)
{
    JSValue list = idb_database_connections(ctx, db);

    JS_DefinePropertyValueUint32(ctx, list, idb_list_len(ctx, list), JS_DupValue(ctx, connection),
                                 JS_PROP_C_W_E);
    JS_FreeValue(ctx, list);
}

void idb_database_remove_connection(JSContext *ctx, JSValueConst db, JSValueConst connection)
{
    JSValue list = idb_database_connections(ctx, db);
    uint32_t i, n = idb_list_len(ctx, list);

    for (i = 0; i < n; i++) {
        JSValue m = JS_GetPropertyUint32(ctx, list, i);
        bool same = JS_VALUE_GET_PTR(m) == JS_VALUE_GET_PTR(connection);

        JS_FreeValue(ctx, m);
        if (!same) continue;
        for (; i + 1 < n; i++)
            JS_DefinePropertyValueUint32(ctx, list, i, JS_GetPropertyUint32(ctx, list, i + 1), JS_PROP_C_W_E);
        JS_SetPropertyStr(ctx, list, "length", JS_NewUint32(ctx, n - 1));
        JS_FreeValue(ctx, list);
        return;
    }
    JS_FreeValue(ctx, list);
    DFAIL("a connection left a database's connection set without being in it — every connection joins at "
          "idb_connection_open and leaves exactly once, when §5.2 finds it CLOSED, so this one was either "
          "closed twice or opened by something other than §5.1");
}

JSValue idb_database_store_set(JSContext *ctx, JSValueConst db)
{
    JSValue stores = JS_GetPropertyStr(ctx, db, IDB_DB_STORES);

    DCHECK(JS_IsObject(stores), "a database carried no set of object stores");
    return stores;
}

double idb_database_version(JSContext *ctx, JSValueConst db)
{
    JSValue v = JS_GetPropertyStr(ctx, db, IDB_DB_VERSION);
    double out = 0;
    int r;

    r = JS_ToFloat64(ctx, &out, v);
    DCHECK(r >= 0, "a database's VERSION is not a number — §2.1 makes it one and only this file writes it");
    (void)r;
    JS_FreeValue(ctx, v);
    return out;
}

/* ---- §2.2's object store ------------------------------------------------------------------------------------ */

JSValue idb_object_store_create(JSContext *ctx, JSValueConst tx, JSValueConst db, const char *name,
                                JSValue key_path, bool key_generator)
{
    JSValue store, records, change;

    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "an object store was created by a transaction that is not an upgrade transaction — §2.7: \"object "
           "stores and indexes can't be added or removed\" by a readwrite transaction, and §4.4's "
           "createObjectStore reports that as an InvalidStateError before it reaches this");
    store = idl_slots_new(ctx);
    CHECK(!JS_IsException(store), "IndexedDB: §2.2's object store record could not be allocated");
    records = JS_NewArray(ctx);
    CHECK(!JS_IsException(records), "IndexedDB: an object store's list of records could not be allocated");
    JS_SetPropertyStr(ctx, store, IDB_STORE_KEY_PATH, key_path);
    JS_SetPropertyStr(ctx, store, IDB_STORE_KEY_GENERATOR, JS_NewBool(ctx, key_generator));
    JS_SetPropertyStr(ctx, store, IDB_STORE_RECORDS, records);
    JS_SetPropertyStr(ctx, store, IDB_STORE_DELETED, JS_FALSE);
    /* The name and the set are one write — see idb_store_set_file, which is also what asserts §2.2's uniqueness
       that §4.4 reports as a ConstraintError before this runs. */
    idb_store_set_file(ctx, db, store, name);
    change = idb_change_new(ctx, IDB_CHANGE_STORE_CREATED);
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_DB, JS_DupValue(ctx, db));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_STORE, JS_DupValue(ctx, store));
    idb_transaction_record_change(ctx, tx, change);
    return store;
}

JSValue idb_object_store_name(JSContext *ctx, JSValueConst store)
{
    JSValue v = JS_GetPropertyStr(ctx, store, IDB_STORE_NAME);

    DCHECK(JS_IsString(v), "an object store carried no NAME — §2.2 gives every store one and only "
                           "idb_object_store_create writes it");
    return v;
}

JSValue idb_object_store_key_path(JSContext *ctx, JSValueConst store)
{
    JSValue v = JS_GetPropertyStr(ctx, store, IDB_STORE_KEY_PATH);

    DCHECK(!JS_IsUndefined(v), "an object store carried no key-path field — §2.2 gives every store one, JS_NULL "
                               "for a store with out-of-line keys, and the ABSENCE of the field is a different "
                               "statement from the null it should hold");
    return v;
}

JSValue idb_object_store_key_path_value(JSContext *ctx, JSValueConst store)
{
    JSValue path = idb_object_store_key_path(ctx, store), out;
    uint32_t i, n;

    /* "or null if none", and a string key path converts to ITSELF — §3.2.9's DOMString-to-JS conversion is the
       identity on a string this component already holds, so there is nothing to copy: a JS string is
       immutable, which is why the note's "changing the properties of the object" is about the Array alone. */
    if (!JS_IsArray(path))
        return path;
    n = idb_list_len(ctx, path);
    DCHECK(n > 0, "an object store's key path is an EMPTY list — §2.5's last bullet is \"a non-empty list\", "
                  "which §4.4's createObjectStore reports as a SyntaxError before a store is created");
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "IndexedDB: §4.5's keyPath could not allocate the Array Web IDL §3.2.24 makes");
    for (i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, out, i, JS_GetPropertyUint32(ctx, path, i), JS_PROP_C_W_E);
    JS_FreeValue(ctx, path);
    return out;
}

bool idb_object_store_uses_key_generator(JSContext *ctx, JSValueConst store)
{
    JSValue v = JS_GetPropertyStr(ctx, store, IDB_STORE_KEY_GENERATOR);
    bool b;

    DCHECK(JS_IsBool(v), "an object store carried no key-generator field");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

bool idb_object_store_is_deleted(JSContext *ctx, JSValueConst store)
{
    JSValue v = JS_GetPropertyStr(ctx, store, IDB_STORE_DELETED);
    bool b;

    DCHECK(JS_IsBool(v), "an object store carried no deleted flag");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

void idb_object_store_destroy(JSContext *ctx, JSValueConst tx, JSValueConst db, JSValueConst store)
{
    JSValue change;

    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "an object store was destroyed by a transaction that is not an upgrade transaction — §2.7 gives "
           "only \"versionchange\" the power to remove one, and §4.4's deleteObjectStore is a member of the "
           "connection rather than of a readwrite transaction");
    /* THE NAME IT WAS FILED UNDER travels with the change and is not read back off the store at revert time:
       §5.5 step 2 puts the store back in the SET, and the set is keyed by the name it left under. */
    change = idb_change_new(ctx, IDB_CHANGE_STORE_DESTROYED);
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_DB, JS_DupValue(ctx, db));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_STORE, JS_DupValue(ctx, store));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_NAME, idb_object_store_name(ctx, store));
    idb_transaction_record_change(ctx, tx, change);
    idb_store_destroy_raw(ctx, db, store);
}

void idb_object_store_rename(JSContext *ctx, JSValueConst tx, JSValueConst db, JSValueConst store,
                             const char *name)
{
    JSValue change;

    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "an object store was renamed by a transaction that is not an upgrade transaction — §4.5's name "
           "setter reports that as an InvalidStateError before this runs");
    DCHECK(name != NULL, "a store was renamed to no name");
    change = idb_change_new(ctx, IDB_CHANGE_STORE_RENAMED);
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_DB, JS_DupValue(ctx, db));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_STORE, JS_DupValue(ctx, store));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_NAME, idb_object_store_name(ctx, store));
    idb_transaction_record_change(ctx, tx, change);
    idb_store_set_remove(ctx, db, store);
    idb_store_set_file(ctx, db, store, name);
}

JSValue idb_object_store_find(JSContext *ctx, JSValueConst db, const char *name)
{
    JSValue stores = JS_GetPropertyStr(ctx, db, IDB_DB_STORES), store;

    DCHECK(JS_IsObject(stores), "a database carried no set of object stores");
    store = JS_GetPropertyStr(ctx, stores, name);
    CHECK(!JS_IsException(store), "IndexedDB: a database's set of object stores could not be read");
    JS_FreeValue(ctx, stores);
    if (JS_IsUndefined(store))
        return JS_NULL;
    return store;
}

/* ---- §6.1's STORE A RECORD INTO AN OBJECT STORE ------------------------------------------------------------- */

int idb_store_record(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst value, JSValueConst key,
                     bool no_overwrite, JSValue *pkey)
{
    JSValue records, clone, rec, change;
    uint32_t n, pos, i;
    bool exists;

    *pkey = JS_UNDEFINED;
    /* §6.1's FIRST STEP branches on the KEY GENERATOR and on nothing else — "if store uses a key generator,
       then: if key is undefined ..." — and a store with in-line keys and no generator has no first step at all,
       because §4.5's `put` has already run §7.1's extract-a-key and handed the result in as `key`. That
       distinction is what this read is: the key path is not consulted here, and the arm below is the one arm
       §6.1 states rather than a "the key was derived somehow" test. */
    if (idb_object_store_uses_key_generator(ctx, store)) {
        /* IT CRASHES HERE because this is where the absence would otherwise become a wrong record: a store
           with a key generator would file its record under a key nothing generated. */
        DFAIL("Indexed Database §6.1's KEY-GENERATOR step is not built, and it is now the only step of this "
              "algorithm that is not — §7.1's extract-a-key landed with §4.5's `put`, so a store with in-line "
              "keys and no generator arrives here with its key already extracted. What is missing is §2.11's "
              "\"generate a key\" (a monotonically increasing number, failing above 2^53) and \"possibly "
              "update the key generator\" (a stored NUMBER key at or above the current number raises it) — "
              "and that generator's current number is state an aborted transaction reverts, so it is recorded "
              "as a change like every other write this file makes (see §5.5 step 2's revert at the bottom). "
              "Where the store ALSO uses in-line keys, step 1.3 needs §7.2's \"inject a key into a value using "
              "a key path\", which WRITES into the value being stored and therefore runs on the clone below "
              "and never on the page's own object — and §7.2's other algorithm, \"check that a key could be "
              "injected into a value\", is the step §4.5's `put` owes before this one, reported there as a "
              "DataError. Those three land together, in core/indexeddb/idb_key_path.c beside §7.1");
        /* A RELEASE BUILD FALLS THROUGH TO THE ALGORITHM'S OWN ANSWER for a key it could not derive — §6.1
           step 1.1.2: "If key is failure, then this operation failed with a ConstraintError DOMException." A
           bare -1 with nothing thrown would be a caller returning JS_EXCEPTION with no exception live. */
        JS_ThrowDOMException(ctx, "ConstraintError", "a key could not be generated for the object store");
        return -1;
    }
    DCHECK(JS_IsObject(key), "§6.1 was handed no key for a store that has no key generator. The key is §7.4's "
                             "key RECORD and not the page's value, and §4.5's `put` is where both of the ways "
                             "one arrives are performed: an out-of-line store's key argument is converted "
                             "there (and an absent one is a DataError there), and an in-line store's key is "
                             "§7.1's extract-a-key over the clone, whose `invalid` and whose `failure` are "
                             "that member's two DataErrors");

    records = idb_store_records(ctx, store);
    n = idb_list_len(ctx, records);
    /* WHERE THE KEY BELONGS, by §2.4's compare over a list §2.2 keeps sorted — see idb_record_pos, which is
       also where §5.5 step 2's revert of this write finds it again. */
    pos = idb_record_pos(ctx, records, key, &exists);

    /* §6.1 STEP 2: "If the no-overwrite flag was given to these steps and is true, and a record already exists
       in store with its key equal to key, then this operation failed with a ConstraintError." That is what
       tells `add` from `put`, and it is reported BEFORE the value is copied — a page that catches it has had
       nothing of its value read. */
    if (no_overwrite && exists) {
        JS_FreeValue(ctx, records);
        JS_ThrowDOMException(ctx, "ConstraintError",
                             "a record with the given key is already in the object store");
        return -1;
    }

    /* §6.1 STEP 4's "! StructuredSerializeForStorage(value)", held as the live copy it produces. This is the
       step that makes §2.3's "later changes to a value have no effect on the record stored in the database"
       true of this component rather than of its caller — and it is where a concolic survives, because the
       clone seeds §2.7.1's `memory` with one rather than refusing it. */
    clone = structured_clone(ctx, value);
    if (JS_IsException(clone)) {
        JS_FreeValue(ctx, records);
        return -1;
    }
    rec = idl_slots_new(ctx);
    CHECK(!JS_IsException(rec), "IndexedDB: §2.2's record could not be allocated");
    JS_SetPropertyStr(ctx, rec, IDB_RECORD_KEY, JS_DupValue(ctx, key));
    JS_SetPropertyStr(ctx, rec, IDB_RECORD_VALUE, clone);

    /* §5.5 step 2's half of this write, recorded AFTER the two arms that refuse and BEFORE the list changes.
       That placement is the same sentence §5.6 step 5.4 relies on and idb_request.h states — an operation is
       atomic, so an operation that failed has nothing to revert — and it is why a `put` that reported a
       ConstraintError leaves no change behind for the abort to undo.
       THE PRIOR RECORD IS HELD, NOT COPIED. A record is written once by this algorithm and replaced whole
       rather than mutated, so the record object the list currently holds IS the state that key had, and the
       revert files that same object back. JS_NULL says the store held no record under this key, which the
       revert reads as a positive statement (remove what this put added) rather than as a missing field. */
    change = idb_change_new(ctx, IDB_CHANGE_RECORD_STORED);
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_STORE, JS_DupValue(ctx, store));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_KEY, JS_DupValue(ctx, key));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_PRIOR,
                      exists ? JS_GetPropertyUint32(ctx, records, pos) : JS_NULL);
    idb_transaction_record_change(ctx, tx, change);

    /* THE LIST IS WRITTEN WITH [[DefineOwnProperty]] AND NOT [[Set]], which is core/idl_slots.h's sibling rule
       one level out: an assignment consults the prototype chain, so a page that put an index accessor on
       Array.prototype would swallow a record write and leave no own property behind at all — the store would
       report a put that never happened. A define creates the own property this list is made of and there is
       nothing for a page to intercept. */
    if (exists) {
        /* §6.1 STEP 3: "If a record already exists in store with its key equal to key, then remove the record
           from store using delete records from an object store." Removing the equal record and inserting the
           new one at the same key is one write at the same position — the list is sorted, so there is exactly
           one place that key can be, and shifting the tail down and back up again would be the same list. */
        JS_DefinePropertyValueUint32(ctx, records, pos, rec, JS_PROP_C_W_E);
    } else {
        for (i = n; i > pos; i--) {
            JSValue moved = JS_GetPropertyUint32(ctx, records, i - 1);

            DCHECK(JS_IsObject(moved), "an object store's list of records had a hole in it");
            JS_DefinePropertyValueUint32(ctx, records, i, moved, JS_PROP_C_W_E);
        }
        JS_DefinePropertyValueUint32(ctx, records, pos, rec, JS_PROP_C_W_E);
    }
    /* §2.2's TWO INVARIANTS, asserted where the list is written and nowhere else — "The list is sorted
       according to key in ascending order" and "There can never be multiple records in a given object store
       with the same key". Both are checked against the neighbours of the one position that changed, which is
       the whole of what this write can break. */
    DCHECK(pos == 0 || idb_record_key_compare(ctx, records, pos - 1, key) < 0,
           "an object store's list of records is no longer sorted: the record before the one just written has "
           "a key that is not less than it");
    DCHECK(pos + 1 >= idb_list_len(ctx, records) || idb_record_key_compare(ctx, records, pos + 1, key) > 0,
           "an object store's list of records is no longer sorted, or holds two records under one key: the "
           "record after the one just written has a key that is not greater than it");

    *pkey = JS_DupValue(ctx, key);   /* §6.1's last step: "Return key." */
    JS_FreeValue(ctx, records);
    return 0;
}

/* ---- §6.2's OBJECT STORE RETRIEVAL OPERATIONS --------------------------------------------------------------- */

/* "Let record be the FIRST record in store's list of records whose key is IN RANGE, if any." First in the
   list's own order, which §2.2 makes key order — so this walk answers the smallest in-range key and a store
   whose list had gone out of order would answer with the wrong record rather than with none. -1 for "record
   was not found". */
static int idb_first_in_range(JSContext *ctx, JSValueConst records, JSValueConst range)
{
    uint32_t n = idb_list_len(ctx, records), i;

    for (i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, records, i);
        JSValue key = JS_GetPropertyStr(ctx, rec, IDB_RECORD_KEY);
        bool in;

        DCHECK(JS_IsObject(key), "a record in an object store carried no key");
        in = idb_key_range_contains(ctx, range, key);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, rec);
        if (in) return (int)i;
    }
    return -1;
}

JSValue idb_retrieve_value(JSContext *ctx, JSValueConst store, JSValueConst range)
{
    JSValue records = idb_store_records(ctx, store), rec, stored, out;
    int i = idb_first_in_range(ctx, records, range);

    /* "If record was not found, return undefined." */
    if (i < 0) {
        JS_FreeValue(ctx, records);
        return JS_UNDEFINED;
    }
    rec = JS_GetPropertyUint32(ctx, records, (uint32_t)i);
    stored = JS_GetPropertyStr(ctx, rec, IDB_RECORD_VALUE);
    /* "Return ! StructuredDeserialize(serialized, targetRealm)." A FRESH copy every time, which is what makes
       a page that mutates the value it got unable to reach the record — the other half of §2.3's
       by-value-not-by-reference, and the reason the copy cannot be skipped even though the stored value is
       already this component's own.
       THERE IS NO "NotReadableError" ARM: that answer exists for a read from underlying storage that fails,
       and this store IS the heap. A refusal here would be the clone disagreeing with the one §6.1 made. */
    out = structured_clone(ctx, stored);
    DCHECK(!JS_IsException(out), "§6.2's deserialization refused a value §6.1 had already copied — the two "
                                 "halves of one clone disagree, which no page input can cause");
    JS_FreeValue(ctx, stored);
    JS_FreeValue(ctx, rec);
    JS_FreeValue(ctx, records);
    return out;
}

JSValue idb_retrieve_key(JSContext *ctx, JSValueConst store, JSValueConst range)
{
    JSValue records = idb_store_records(ctx, store), rec, key, out;
    int i = idb_first_in_range(ctx, records, range);

    if (i < 0) {
        JS_FreeValue(ctx, records);
        return JS_UNDEFINED;
    }
    rec = JS_GetPropertyUint32(ctx, records, (uint32_t)i);
    key = JS_GetPropertyStr(ctx, rec, IDB_RECORD_KEY);
    /* "Return the result of converting a key to a value with record's key" — §7.3, which hands a concolic key
       back as the concolic rather than as a laundered copy of one. */
    out = idb_key_to_value(ctx, key);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, rec);
    JS_FreeValue(ctx, records);
    return out;
}

/* ---- §5.5 step 2's REVERT ----------------------------------------------------------------------------------
 *
 * "All the changes made to the database by the transaction are reverted. For upgrade transactions this includes
 * changes to the set of object stores and indexes, as well as the change to the version. Any object stores and
 * indexes which were created during the transaction are now considered deleted for the purposes of other
 * algorithms."
 *
 * WHY IT IS NOT A SPAN OF THE FLOW'S COW DELTA, which is what the crash this replaces asked for. The delta and
 * this algorithm are about different things and each of the three differences is reachable in this engine
 * today. (1) SCOPE: the delta's unit is "every write this FLOW made", and this algorithm's is "every write this
 * TRANSACTION made to THE DATABASE". A `success` handler runs inside the transaction's lifetime, so a `hits++`
 * in an `onsuccess` that then calls `abort()` lies inside any span of the flow's writes, and a browser keeps
 * that value — as it keeps the request's own state, the DOM, and every promise the handler settled. (2)
 * INTERLEAVING: §2.7.2 permits two live read/write transactions whose scopes are DISJOINT, and this engine
 * permits exactly that (idb_transaction.c crashes only on the overlapping case). Their request tasks land on
 * one flow's job queue, so the writes of one transaction are not a contiguous range of the other's. (3)
 * IDENTITY: the delta captures a slot ONCE, at this flow's first write to it, so a record an EARLIER
 * transaction on this flow already wrote holds its entry before any later mark and would not be reverted at
 * all; and a database this flow created after its own fork is flow-PRIVATE, which the delta deliberately does
 * not capture, so there would be nothing to unapply.
 *
 * SO THE CHANGES ARE THE TRANSACTION'S OWN STATE — which is §5.5's own word for them — AND THE REVERT IS
 * ORDINARY WRITES. The list is a JS Array on the transaction's slot record (§State-isolation's rule for
 * platform data a flow holds, which is also why it is not a malloc'd C list): an arm of a fork appends to it
 * through the property path the COW delta captures, so each arm reverts what IT recorded and one arm can abort
 * while its sibling goes on to commit. And the undo of a write goes back out through the SAME property path
 * the write came in through, so whatever isolation a database write has, its undo has exactly the same one —
 * which is the property that makes this correct under a fork without a second mechanism, and it is a property
 * of the write path rather than a claim made here.
 *
 * BACKWARDS, WHICH IS THE ONLY ORDER THAT COMPOSES. Two puts under one key leave two changes and only the
 * FIRST one's prior record is the state the transaction found; a store created and then renamed is put back by
 * undoing the rename and then the creation, which is also what lets the destroy assert that the set still holds
 * the store under the name it is about to remove. */
void idb_database_revert_transaction(JSContext *ctx, JSValueConst tx)
{
    JSValue changes = idb_transaction_changes(ctx, tx);
    uint32_t i, n = idb_list_len(ctx, changes);

    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "the changes of a FINISHED transaction were reverted — §5.5 step 1 abandons the abort of a finished "
           "transaction, so reaching here twice means the changes are being undone against a database that has "
           "already had them undone once");
    for (i = n; i > 0; i--) {
        JSValue change = JS_GetPropertyUint32(ctx, changes, i - 1);
        JSValue db, store, key, prior, records, name;
        const char *cname;
        uint32_t pos, j, len;
        bool exists;
        int kind;

        DCHECK(JS_IsObject(change), "a transaction's list of database changes had a hole in it");
        /* READ ONCE: the kind is what the arm below is chosen by AND what the last arm asserts it is, and a
           DCHECK's condition may not be a second read of anything. */
        kind = idb_change_kind(ctx, change);
        switch (kind) {
        case IDB_CHANGE_RECORD_STORED:
            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            key = idb_change_get(ctx, change, IDB_CHANGE_KEY);
            prior = idb_change_get(ctx, change, IDB_CHANGE_PRIOR);
            records = idb_store_records(ctx, store);
            pos = idb_record_pos(ctx, records, key, &exists);
            DCHECK(exists, "a record this transaction stored is no longer in the store it was stored in — §6.1 "
                           "is the only algorithm that writes this list, and §2.7.2 gives one store to one "
                           "read/write transaction at a time, so nothing else can have removed it");
            if (JS_IsNull(prior)) {
                /* The put ADDED this key: the list loses it, and the tail comes down one place so the ordering
                   §2.2 states survives the removal. */
                len = idb_list_len(ctx, records);
                for (j = pos; j + 1 < len; j++)
                    JS_DefinePropertyValueUint32(ctx, records, j, JS_GetPropertyUint32(ctx, records, j + 1),
                                                 JS_PROP_C_W_E);
                JS_SetPropertyStr(ctx, records, "length", JS_NewUint32(ctx, len - 1));
            } else {
                /* The put OVERWROTE a record: the one it displaced goes back at the same position, which is the
                   only position its key can occupy in a sorted list. */
                JS_DefinePropertyValueUint32(ctx, records, pos, JS_DupValue(ctx, prior), JS_PROP_C_W_E);
            }
            JS_FreeValue(ctx, records);
            JS_FreeValue(ctx, prior);
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, store);
            break;
        case IDB_CHANGE_STORE_CREATED:
            db = idb_change_get(ctx, change, IDB_CHANGE_DB);
            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            /* "Any object stores ... which were created during the transaction are now considered DELETED for
               the purposes of other algorithms" — the same marking §4.4's deleteObjectStore performs, and for
               the same reason: §4.5's members answer out of the handle the page still holds. */
            idb_store_destroy_raw(ctx, db, store);
            JS_FreeValue(ctx, store);
            JS_FreeValue(ctx, db);
            break;
        case IDB_CHANGE_STORE_DESTROYED:
            db = idb_change_get(ctx, change, IDB_CHANGE_DB);
            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            name = idb_change_get(ctx, change, IDB_CHANGE_NAME);
            cname = JS_ToCString(ctx, name);
            CHECK(cname != NULL, "IndexedDB: a destroyed store's name could not be read back to restore it");
            JS_SetPropertyStr(ctx, store, IDB_STORE_DELETED, JS_FALSE);
            idb_store_set_file(ctx, db, store, cname);
            JS_FreeCString(ctx, cname);
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, store);
            JS_FreeValue(ctx, db);
            break;
        case IDB_CHANGE_STORE_RENAMED:
            db = idb_change_get(ctx, change, IDB_CHANGE_DB);
            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            name = idb_change_get(ctx, change, IDB_CHANGE_NAME);
            cname = JS_ToCString(ctx, name);
            CHECK(cname != NULL, "IndexedDB: a renamed store's former name could not be read back");
            idb_store_set_remove(ctx, db, store);
            idb_store_set_file(ctx, db, store, cname);
            JS_FreeCString(ctx, cname);
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, store);
            JS_FreeValue(ctx, db);
            break;
        default: {
            JSValue was;
            double version = 0;
            int r;

            DCHECK(kind == IDB_CHANGE_VERSION_SET,
                   "a recorded database change names a kind this revert has no arm for — a mutation that "
                   "records a change it cannot undo would leave that change standing after an abort");
            db = idb_change_get(ctx, change, IDB_CHANGE_DB);
            was = idb_change_get(ctx, change, IDB_CHANGE_VERSION);
            r = JS_ToFloat64(ctx, &version, was);
            DCHECK(r >= 0, "a recorded version change carried a version that is not a number");
            (void)r;
            /* §5.7 step 8: "this change is considered part of the transaction, and so if the transaction is
               aborted, this change is reverted." For a database this open CREATED, the version it goes back to
               is the 0 it was created with — which is also the answer §5.8 step 3 needs for a newly created
               database, without a second field remembering which it was. */
            idb_database_write_version(ctx, db, version);
            JS_FreeValue(ctx, was);
            JS_FreeValue(ctx, db);
            break;
        }
        }
        JS_FreeValue(ctx, change);
    }
    /* AND THE LIST ITSELF, which idb_transaction_changes hands over OWNED. Dropping it here was worth a whole
       browser: the list is emptied when the transaction reaches finished, so what survived was an EMPTY Array
       — and an Array holds Array.prototype, which holds the realm's function objects, each of which holds the
       REALM. Three reverts in one fixture therefore kept 2612 Functions, 408 shapes and a JSContext at
       refcount 3108 alive, reported by the runtime's leak walk as three anonymous `Array [ ] { length: 0 }`
       with nothing naming an owner. idb_transaction_set_state now asserts that nobody is still holding one at
       the moment the transaction finishes, which is where the next dropped reference will be named. */
    JS_FreeValue(ctx, changes);
}

/* "ANY OBJECT STORES ... WHICH WERE CREATED DURING THE TRANSACTION" — one store, asked of the list the revert
   above runs. The list is READ FORWARDS here because this is a membership question and not a composition: a
   store either appears as a creation of this transaction or it does not, and a store created and then
   destroyed inside one transaction was still created by it (§5.8 step 5.1 skips its handle either way). */
bool idb_database_store_was_created_by(JSContext *ctx, JSValueConst tx, JSValueConst store)
{
    JSValue changes = idb_transaction_changes(ctx, tx);
    uint32_t i, n = idb_list_len(ctx, changes);
    bool created = false;

    DCHECK(JS_IsObject(store), "\"was this store newly created during the transaction\" was asked of something "
                               "that is not a §2.2 object store");
    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "a FINISHED transaction was asked which object stores it created. Its list of changes is emptied "
           "when it reaches that state, so the answer would be NO for every store — and §5.8 step 5.1 would "
           "then rename the handle of a store this transaction created, which is the one case it must not");
    for (i = 0; i < n && !created; i++) {
        JSValue change = JS_GetPropertyUint32(ctx, changes, i);

        DCHECK(JS_IsObject(change), "a transaction's list of database changes had a hole in it");
        if (idb_change_kind(ctx, change) == IDB_CHANGE_STORE_CREATED) {
            JSValue made = idb_change_get(ctx, change, IDB_CHANGE_STORE);

            created = JS_VALUE_GET_PTR(made) == JS_VALUE_GET_PTR(store);
            JS_FreeValue(ctx, made);
        }
        JS_FreeValue(ctx, change);
    }
    JS_FreeValue(ctx, changes);
    return created;
}
