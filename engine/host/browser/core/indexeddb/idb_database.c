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
 * at all: a change whose transaction is unknown is a change nothing can undo. So each of the six mutations
 * below takes the transaction ahead of the state it changes, asserts what §2.7 says that transaction may do,
 * and records the INVERSE of what it is about to write. The sixth is §2.11's key generator, whose algorithms
 * live in core/indexeddb/idb_key_generator.c and reach this list through the one recorder exported for them.
 * The revert is at the bottom of this file, beside the writes it runs backwards. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_index.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_generator.h"
#include "core/indexeddb/idb_key_path.h"
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
/* §2.2's OPTIONAL KEY GENERATOR, which is §2.11's record (core/indexeddb/idb_key_generator.h) or JS_NULL. It
   holds the GENERATOR and not a flag saying there is one, because the two would be two answers to one question:
   a true with no current number, or a number on a store §4.5's `autoIncrement` reports false for, are both
   states this makes unrepresentable. "Uses a key generator" is then the same fact as "this field is not null". */
#define IDB_STORE_KEY_GENERATOR "keyGenerator"
#define IDB_STORE_RECORDS       "records"       /* §2.2's "list of records ... sorted according to key" */
#define IDB_STORE_DELETED       "deleted"       /* §4.4's "Destroy store" — see idb_object_store_destroy */
/* §2.6's SET OF INDEXES that reference this store, as the LIST core/indexeddb/idb_index.h keeps it. The field
   is the store record's and every write to it is that component's; this is the one spelling of its name. */
#define IDB_STORE_INDEXES       "indexes"

#define IDB_RECORD_KEY          "key"
#define IDB_RECORD_VALUE        "value"

/* §5.5 step 2's ONE CHANGE, and the six kinds this engine can make. Each names the state it is about to write
   and what that state held BEFORE — which is the whole of the change, because the revert is the same write with
   the remembered operand. A kind is added when a mutation is, and the revert's last arm ASSERTS which kind it
   is rather than accepting whatever is left: a kind nobody wrote a revert for has to abort at the revert, not
   quietly leave that change standing in a database an abort has told the page is untouched. */
#define IDB_CHANGE_KIND         "kind"
#define IDB_CHANGE_DB           "db"
#define IDB_CHANGE_STORE        "store"
#define IDB_CHANGE_INDEX        "index"    /* §2.6's index the change was made to or in */
#define IDB_CHANGE_KEY          "key"
#define IDB_CHANGE_NAME         "name"     /* the name the store or index was filed under before */
#define IDB_CHANGE_VERSION      "version"  /* the version the database held before */
#define IDB_CHANGE_REMOVED      "removed"  /* the records the store held before, in key order */
#define IDB_CHANGE_RECORDS      "records"  /* the index records written or removed, in (key, value) order */
/* §2.11's current number the store's key generator held before, as a BigInt — the exact integer type, for the
   reason idb_key_generator.c is built around: 2^53+1 is a current number and is not an ECMAScript Number. */
#define IDB_CHANGE_NUMBER       "number"

/* §6.4's delete and §6.6's clear share ONE KIND because they make ONE change: a set of records left a store,
   and the inverse is filing those same records back. A second kind would be a second name for one revert, and
   the revert is what a kind is for. */
enum { IDB_CHANGE_RECORD_STORED, IDB_CHANGE_RECORDS_REMOVED, IDB_CHANGE_STORE_CREATED,
       IDB_CHANGE_STORE_DESTROYED, IDB_CHANGE_STORE_RENAMED, IDB_CHANGE_VERSION_SET,
       IDB_CHANGE_KEY_GENERATOR, IDB_CHANGE_INDEX_CREATED, IDB_CHANGE_INDEX_DESTROYED,
       IDB_CHANGE_INDEX_RENAMED, IDB_CHANGE_INDEX_RECORDS_STORED, IDB_CHANGE_INDEX_RECORDS_REMOVED };

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

/* THE SAME COMPARISON BETWEEN TWO POSITIONS of one list — the shape §2.2's ordering invariant takes where a
   write JOINS two records that were not neighbours, which is what a removal does and what §6.1's write (whose
   invariant is stated against the key it just filed) does not. */
static int idb_record_pos_compare(JSContext *ctx, JSValueConst records, uint32_t i, uint32_t j)
{
    JSValue rec = JS_GetPropertyUint32(ctx, records, j);
    JSValue key = JS_GetPropertyStr(ctx, rec, IDB_RECORD_KEY);
    int c;

    DCHECK(JS_IsObject(key), "a record in an object store carried no key");
    c = idb_record_key_compare(ctx, records, i, key);
    JS_FreeValue(ctx, key);
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
    JSValue store, records, indexes, change;

    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "an object store was created by a transaction that is not an upgrade transaction — §2.7: \"object "
           "stores and indexes can't be added or removed\" by a readwrite transaction, and §4.4's "
           "createObjectStore reports that as an InvalidStateError before it reaches this");
    store = idl_slots_new(ctx);
    CHECK(!JS_IsException(store), "IndexedDB: §2.2's object store record could not be allocated");
    records = JS_NewArray(ctx);
    CHECK(!JS_IsException(records), "IndexedDB: an object store's list of records could not be allocated");
    /* §2.6's set of indexes that reference this store, EMPTY: a store is created with none, and §4.5's
       createIndex is the only algorithm that adds one. It is built here rather than on first use, so the
       ABSENCE of the field is never a state — the same statement the key path's JS_NULL is. */
    indexes = JS_NewArray(ctx);
    CHECK(!JS_IsException(indexes), "IndexedDB: an object store's set of indexes could not be allocated");
    JS_SetPropertyStr(ctx, store, IDB_STORE_INDEXES, indexes);
    JS_SetPropertyStr(ctx, store, IDB_STORE_KEY_PATH, key_path);
    /* §2.11: "when a object store is created it can be specified to use a key generator", and the generator's
       current number is "set when the associated object store is created" — so the record is built HERE and
       there is no later moment at which a store acquires one. */
    JS_SetPropertyStr(ctx, store, IDB_STORE_KEY_GENERATOR,
                      key_generator ? idb_key_generator_new(ctx) : JS_NULL);
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
    bool has;

    DCHECK(JS_IsNull(v) || JS_IsObject(v),
           "an object store carried no key-generator field — §2.2 gives every store one, JS_NULL for a store "
           "that has no generator, and the ABSENCE of the field is a different statement from the null it "
           "should hold");
    has = !JS_IsNull(v);
    JS_FreeValue(ctx, v);
    return has;
}

JSValue idb_object_store_key_generator(JSContext *ctx, JSValueConst store)
{
    JSValue gen = JS_GetPropertyStr(ctx, store, IDB_STORE_KEY_GENERATOR);

    DCHECK(JS_IsObject(gen), "§2.11's key generator was asked of a store that has none. §2.2 makes it optional "
                             "and idb_object_store_uses_key_generator is how that is asked — every caller of "
                             "this asks it first, because §6.1's whole first step is under that condition");
    return gen;
}

JSValue idb_object_store_indexes(JSContext *ctx, JSValueConst store)
{
    JSValue v = JS_GetPropertyStr(ctx, store, IDB_STORE_INDEXES);

    DCHECK(JS_IsArray(v), "an object store carried no SET OF INDEXES — §2.2 gives every store one and "
                          "idb_object_store_create builds it empty, so a store without one was not built by "
                          "this file");
    return v;
}

/* §2.6's FIVE CHANGES. Each names the state it is about to write and what that state held before, exactly as
   the store's six do; the inverse is idb_index.c's, because the write is. The three METADATA changes assert
   §2.7's "object stores and indexes can't be added or removed" by anything but an upgrade transaction — the
   two RECORD changes do not, because §6.1 step 5 and §6.4 step 2 run in a readwrite transaction, and the
   read-only refusal they do owe is asserted once for every change by idb_transaction_record_change. */
static void idb_index_change(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index, int kind,
                             const char *field, JSValue operand)
{
    JSValue change = idb_change_new(ctx, kind);

    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "an index was added to, removed from or renamed within an object store by a transaction that is "
           "not an upgrade transaction — §2.7 gives only \"versionchange\" that power, and §4.5's createIndex "
           "and deleteIndex each report it as an InvalidStateError before they reach this");
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_STORE, JS_DupValue(ctx, store));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_INDEX, JS_DupValue(ctx, index));
    if (field != NULL)
        JS_SetPropertyStr(ctx, change, field, operand);
    idb_transaction_record_change(ctx, tx, change);
}

void idb_database_record_index_created(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index)
{
    idb_index_change(ctx, tx, store, index, IDB_CHANGE_INDEX_CREATED, NULL, JS_UNDEFINED);
}

void idb_database_record_index_destroyed(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index,
                                         JSValue name)
{
    idb_index_change(ctx, tx, store, index, IDB_CHANGE_INDEX_DESTROYED, IDB_CHANGE_NAME, name);
}

void idb_database_record_index_renamed(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index,
                                       JSValue name)
{
    idb_index_change(ctx, tx, store, index, IDB_CHANGE_INDEX_RENAMED, IDB_CHANGE_NAME, name);
}

/* THE INDEX AND NOT ITS STORE, because these two changes are undone inside the index's own list of records and
   the store plays no part in that — unlike the three above, whose inverse is a write to the store's set. */
static void idb_index_records_change(JSContext *ctx, JSValueConst tx, JSValueConst index, int kind,
                                     JSValue records)
{
    JSValue change = idb_change_new(ctx, kind);

    DCHECK(JS_IsArray(records), "an index's record change carried no list of the records it wrote or removed");
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_INDEX, JS_DupValue(ctx, index));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_RECORDS, records);
    idb_transaction_record_change(ctx, tx, change);
}

void idb_database_record_index_records_stored(JSContext *ctx, JSValueConst tx, JSValueConst index,
                                              JSValue records)
{
    idb_index_records_change(ctx, tx, index, IDB_CHANGE_INDEX_RECORDS_STORED, records);
}

void idb_database_record_index_records_removed(JSContext *ctx, JSValueConst tx, JSValueConst index,
                                               JSValue records)
{
    idb_index_records_change(ctx, tx, index, IDB_CHANGE_INDEX_RECORDS_REMOVED, records);
}

void idb_object_store_record_generator_change(JSContext *ctx, JSValueConst tx, JSValueConst store, int64_t prior)
{
    JSValue change = idb_change_new(ctx, IDB_CHANGE_KEY_GENERATOR);

    /* THE STORE AND NOT THE GENERATOR, for the reason every other change names the state it changed rather than
       a pointer into it: the revert reaches the generator the same way §2.11's algorithms do, through the store
       that owns it, so a store whose generator record were ever replaced could not leave the two disagreeing. */
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_STORE, JS_DupValue(ctx, store));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_NUMBER, JS_NewBigInt64(ctx, prior));
    idb_transaction_record_change(ctx, tx, change);
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

/* THE BLOCK'S OWN STAGE NUMBERS, from the SAME declaration the caller expands — so a stage added to the
   algorithm cannot renumber the arms below without renumbering the caller's list with it. */
enum { IDB_STORE_RECORD_ALGO_STAGES(JS_STEP_STAGE_ENUM, IDB_SR, "") IDB_SR_N };

#define IDB_SR_GOTO(hdr, to) STEP_GOTO((hdr)->stage, (to), &(hdr)->get_phase, &(hdr)->desc_phase, NULL)

void idb_store_record_walk_visit(JSContext *ctx, IdbStoreWalk *sw, JSStepVisit *v)
{
    idb_key_walk_visit(ctx, &sw->w, v);
    v->val(ctx, &sw->tx);
    v->val(ctx, &sw->store);
    v->val(ctx, &sw->value);
    v->val(ctx, &sw->key);
    v->val(ctx, &sw->generated);
    v->val(ctx, &sw->indexes);
    v->val(ctx, &sw->index_key);
}

void idb_store_record_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbStoreWalk *sw, JSValueConst tx,
                                 JSValueConst store, JSValueConst value, JSValueConst key, bool no_overwrite,
                                 int base, int after)
{
    DCHECK(idb_transaction_is(tx), "§6.1 was begun with something that is not a §2.7 transaction — every change "
                                   "it makes names the transaction making it");
    DCHECK(JS_IsObject(store), "§6.1 was begun over something that is not a §2.2 object store");
    /* A ZEROED JSValue IS THE INTEGER 0 AND NOT JS_UNDEFINED, so every slot is PLACED here. */
    sw->kpres = IDB_KEY_PATH_KEY;
    sw->tx = JS_DupValue(ctx, tx);
    sw->store = JS_DupValue(ctx, store);
    sw->value = JS_DupValue(ctx, value);
    sw->key = JS_DupValue(ctx, key);
    sw->generated = JS_UNDEFINED;
    sw->indexes = JS_UNDEFINED;
    sw->index_key = JS_UNDEFINED;
    sw->idx = 0;
    sw->no_overwrite = no_overwrite ? 1 : 0;
    sw->generator_raised = 0;
    sw->after = after;
    hdr->stage = (uint16_t)(base + IDB_SR_GENERATE);
}

JSValue idb_store_record_walk_take(JSContext *ctx, IdbStoreWalk *sw)
{
    DCHECK(JS_IsObject(sw->key), "§6.1's last step, \"Return key\", has no key to return");
    return JS_DupValue(ctx, sw->key);   /* STEP 6 */
}

/* §6.1 STEP 5's CURRENT INDEX. The set is the store's LIVE record and not a copy of it, which is exact rather
   than convenient: an operation is one task, and §4.5's createIndex and deleteIndex are synchronous members of
   a task of their own, so nothing can change this set between two stages of one operation. That is asserted
   here — at the index rather than at the set, because what a change would produce is precisely an entry that is
   deleted or that references another store. */
static JSValue sr_index_at(JSContext *ctx, IdbStoreWalk *sw)
{
    JSValue index = JS_GetPropertyUint32(ctx, sw->indexes, sw->idx), referenced;

    DCHECK(JS_IsObject(index), "§6.1 step 5's set of indexes had a hole in it");
    DCHECK(!idb_index_is_deleted(ctx, index),
           "§6.1 step 5 reached an index that has been DESTROYED. The set it walks is the store's own record "
           "and an operation is one task, so nothing may add to or remove from it while this algorithm runs");
    referenced = idb_index_store(ctx, index);
    DCHECK(JS_VALUE_GET_PTR(referenced) == JS_VALUE_GET_PTR(sw->store),
           "§6.1 step 5 reached an index that does not REFERENCE the store being written — the step is stated "
           "over \"each index which references store\" and the set is that store's own");
    JS_FreeValue(ctx, referenced);
    return index;
}

/* §6.1 STEP 1, the whole of it: the two arms under "if store uses a key generator", and nothing else. A store
   with in-line keys and no generator has no first step at all, because §4.5's `put` has already run §7.1's
   extract-a-key and handed the result in as `key`. */
static int sr_generate(JSContext *ctx, JSStepHdr *hdr, IdbStoreWalk *sw, int base)
{
    if (idb_object_store_uses_key_generator(ctx, sw->store)) {                    /* STEP 1 */
        if (JS_IsUndefined(sw->key)) {                                           /* STEP 1.1 */
            JSValue key_path;

            /* "Let key be the result of generating a key for store. If key is FAILURE, then this operation
               failed with a ConstraintError DOMException." The generator is spent at 2^53+1 and §2.11 keeps it
               spent; §5.6 step 5.2 takes the exception as the request's error. */
            if (!idb_key_generate(ctx, sw->tx, sw->store, &sw->generated)) {     /* STEP 1.1.1 */
                JS_ThrowDOMException(ctx, "ConstraintError",                     /* STEP 1.1.2 */
                                     "the object store's key generator can generate no more keys");
                return JS_STEP_ABRUPT;
            }
            JS_FreeValue(ctx, sw->key);
            sw->key = JS_DupValue(ctx, sw->generated);
            sw->generator_raised = 1;
            /* "If store ALSO uses in-line keys, then run inject a key into a value using a key path with
               value, key and store's key path." The value is §4.5 step 10's clone, and §4.5 step 11.4.2 has
               already run §7.2's other algorithm over this same pair — which is why §7.2's steps here are
               assertions rather than refusals. */
            key_path = idb_object_store_key_path(ctx, sw->store);
            if (!JS_IsNull(key_path))                                            /* STEP 1.1.3 */
                idb_key_path_inject(ctx, sw->value, sw->generated, key_path);
            JS_FreeValue(ctx, key_path);
        } else {
            /* "Otherwise, run possibly update the key generator for store with key." */
            sw->generator_raised =                                               /* STEP 1.2 */
                idb_key_generator_possibly_update(ctx, sw->tx, sw->store, sw->key) ? 1 : 0;
        }
    }
    DCHECK(JS_IsObject(sw->key), "§6.1 has no key to file its record under after its first step. The key is "
                                 "§7.4's key RECORD and not the page's value, and there are three ways one "
                                 "arrives: §4.5's `put` converts an out-of-line store's key argument (and "
                                 "reports an absent one as a DataError), it extracts an in-line store's key "
                                 "from the clone with §7.1 (whose `invalid` and `failure` are that member's "
                                 "other two DataErrors), and step 1.1.1 above generates one");
    IDB_SR_GOTO(hdr, base + IDB_SR_NO_OVERWRITE);
    return JS_STEP_YIELD;
}

/* "IS THERE A RECORD IN store WITH ITS KEY EQUAL TO key" — steps 2 and 3 ask it and step 4 asserts its answer,
   through the one binary search over a list §2.2 keeps in §2.4's key order. */
static bool sr_key_exists(JSContext *ctx, IdbStoreWalk *sw)
{
    JSValue records = idb_store_records(ctx, sw->store);
    bool exists;

    idb_record_pos(ctx, records, sw->key, &exists);
    JS_FreeValue(ctx, records);
    return exists;
}

/* §6.1 STEP 4's WRITE, and step 5's list taken immediately after it. */
static int sr_store(JSContext *ctx, JSStepHdr *hdr, IdbStoreWalk *sw, int base)
{
    JSValue records = idb_store_records(ctx, sw->store), clone, rec, change;
    uint32_t pos, n, i;
    bool exists;

    /* §6.1 STEP 4's "! StructuredSerializeForStorage(value)", held as the live copy it produces. This is the
       step that makes §2.3's "later changes to a value have no effect on the record stored in the database"
       true of this component rather than of its caller — and it is where a concolic survives, because the
       clone seeds §2.7.1's `memory` with one rather than refusing it. */
    clone = structured_clone(ctx, sw->value);
    if (JS_IsException(clone)) {
        JS_FreeValue(ctx, records);
        return JS_STEP_ABRUPT;
    }
    rec = idl_slots_new(ctx);
    CHECK(!JS_IsException(rec), "IndexedDB: §2.2's record could not be allocated");
    JS_SetPropertyStr(ctx, rec, IDB_RECORD_KEY, JS_DupValue(ctx, sw->key));
    JS_SetPropertyStr(ctx, rec, IDB_RECORD_VALUE, clone);

    n = idb_list_len(ctx, records);
    pos = idb_record_pos(ctx, records, sw->key, &exists);
    DCHECK(!exists, "§6.1 step 4 found a record already under the key it is about to write. Step 3 removes it "
                    "— through §6.4, so that the index records referencing it go with it — and step 3 is the "
                    "stage immediately before this one");

    /* §5.5 step 2's half of this write, recorded BEFORE the list changes. The change carries only the key,
       because after step 3 there is never a record to displace: a put that overwrote is TWO changes, the §6.4
       removal step 3 made and this addition, and the revert runs them backwards. */
    change = idb_change_new(ctx, IDB_CHANGE_RECORD_STORED);
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_STORE, JS_DupValue(ctx, sw->store));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_KEY, JS_DupValue(ctx, sw->key));
    idb_transaction_record_change(ctx, sw->tx, change);

    /* THE LIST IS WRITTEN WITH [[DefineOwnProperty]] AND NOT [[Set]], which is core/idl_slots.h's sibling rule
       one level out: an assignment consults the prototype chain, so a page that put an index accessor on
       Array.prototype would swallow a record write and leave no own property behind at all. */
    for (i = n; i > pos; i--) {
        JSValue moved = JS_GetPropertyUint32(ctx, records, i - 1);

        DCHECK(JS_IsObject(moved), "an object store's list of records had a hole in it");
        JS_DefinePropertyValueUint32(ctx, records, i, moved, JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueUint32(ctx, records, pos, rec, JS_PROP_C_W_E);
    /* §2.2's TWO INVARIANTS, asserted against the neighbours of the one position that changed — "the list is
       sorted according to key in ascending order" and "there can never be multiple records in a given object
       store with the same key". */
    DCHECK(pos == 0 || idb_record_key_compare(ctx, records, pos - 1, sw->key) < 0,
           "an object store's list of records is no longer sorted: the record before the one just written has "
           "a key that is not less than it");
    DCHECK(pos + 1 >= idb_list_len(ctx, records) || idb_record_key_compare(ctx, records, pos + 1, sw->key) > 0,
           "an object store's list of records is no longer sorted, or holds two records under one key: the "
           "record after the one just written has a key that is not greater than it");
    JS_FreeValue(ctx, records);

    sw->indexes = idb_store_index_set(ctx, sw->store);   /* STEP 5's "each index which references store" */
    sw->idx = 0;
    IDB_SR_GOTO(hdr, base + IDB_SR_INDEX);
    return JS_STEP_YIELD;
}

/* §6.1 STEPS 5.3-5.4 — one condition in two arms: "if index's multiEntry flag is false, or if index key is not
   an array key, and if index already contains a record with key equal to index key, and index's unique flag is
   true", and the same sentence over "any of the SUBKEYS of index key" on the other arm. */
static bool sr_unique_clash(JSContext *ctx, IdbStoreWalk *sw, JSValueConst index)
{
    bool clash = false;

    if (!idb_index_unique(ctx, index))
        return false;
    if (!idb_index_key_is_per_subkey(ctx, index, sw->index_key))
        return idb_index_contains_key(ctx, index, sw->index_key);
    {
        JSValue subkeys = idb_key_subkeys(ctx, sw->index_key);
        uint32_t i, n = idb_list_len(ctx, subkeys);

        for (i = 0; i < n && !clash; i++) {
            JSValue sub = JS_GetPropertyUint32(ctx, subkeys, i);

            DCHECK(JS_IsObject(sub), "an array key's list of subkeys had a hole in it");
            clash = idb_index_contains_key(ctx, index, sub);
            JS_FreeValue(ctx, sub);
        }
        JS_FreeValue(ctx, subkeys);
    }
    return clash;
}

int idb_store_record_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbStoreWalk *sw, JSValue in, int base,
                              JSValue **out_cb, int *out_argc)
{
    int phase = hdr->stage - base;

    DCHECK(phase >= 0 && phase < IDB_SR_N,
           "§6.1 was resumed at a stage outside the block its caller declared for it");

    /* §7.1's own rest points — step 5.1's extraction, which is §7.4's walk over the index's key path. */
    if (phase <= IDB_SR_LEAVE) {
        int r = idb_key_walk_run(ctx, hdr, &sw->w, in, base, out_cb, out_argc);

        if (r != JS_STEP_ABRUPT)
            return r;
        /* §6.1 STEP 5.2's first arm: "If index key is an EXCEPTION ... take no further actions for index, and
           continue these steps for the next index", with the standard's own note beside it — "an exception
           thrown in this step is NOT RETHROWN". So the completion is taken here and discarded, which is this
           step's stated result replacing the way the conversion reports it, not a swallowed error. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        sw->idx++;
        IDB_SR_GOTO(hdr, base + IDB_SR_INDEX);
        return JS_STEP_YIELD;
    }
    JS_FreeValue(ctx, in);

    if (phase == IDB_SR_GENERATE)
        return sr_generate(ctx, hdr, sw, base);

    if (phase == IDB_SR_NO_OVERWRITE) {
        bool exists = sr_key_exists(ctx, sw);

        /* §2.11: "IF AN INSERTION FAILS due to constraint violations or IO error, THE KEY GENERATOR IS NOT
           UPDATED." Write c for the current number step 1 found. Every numeric key this store already holds
           went through step 1 itself, and each left c EITHER strictly above itself OR at the ceiling+1, from
           which no key generates again. So on the arm that RAISED c the key about to be filed is one no record
           holds, and this state cannot arise. The OTHER failure §2.11 names — step 5's unique-index
           ConstraintError — is a failure AFTER step 1 raised it, and that one is undone by §5.6 step 5.4. */
        DCHECK(!sw->generator_raised || !exists,
               "§6.1's step 1 raised the key generator's current number and step 2 then found a record ALREADY "
               "under that key. §2.11 forbids both halves of what that means: for `add` the operation is about "
               "to fail leaving the generator raised, and for `put` a number the generator produced or accepted "
               "is one a record already holds (\"the same key is never generated twice for the same object "
               "store\")");
        /* "If the no-overwrite flag was given to these steps and is true, and a record already exists in store
           with its key equal to key, then this operation failed with a ConstraintError." That is what tells
           `add` from `put`, and it is reported BEFORE the value is copied. */
        if (sw->no_overwrite && exists) {                                        /* STEP 2 */
            JS_ThrowDOMException(ctx, "ConstraintError",
                                 "a record with the given key is already in the object store");
            return JS_STEP_ABRUPT;
        }
        IDB_SR_GOTO(hdr, base + IDB_SR_REMOVE);
        return JS_STEP_YIELD;
    }

    if (phase == IDB_SR_REMOVE) {
        /* "If a record already exists in store with its key equal to key, then remove the record from store
           USING DELETE RECORDS FROM AN OBJECT STORE." It is §6.4 and not a splice, and the difference is the
           whole of finding #4: §6.4's step 2 removes the index records whose VALUE is this key, so an
           overwrite drops the old value's index entries before step 5 writes the new value's. */
        if (sr_key_exists(ctx, sw)) {                                            /* STEP 3 */
            JSValue only = idb_key_range_only_key(ctx, sw->key);

            idb_delete_records(ctx, sw->tx, sw->store, only);
            JS_FreeValue(ctx, only);
        }
        IDB_SR_GOTO(hdr, base + IDB_SR_STORE);
        return JS_STEP_YIELD;
    }

    if (phase == IDB_SR_STORE)
        return sr_store(ctx, hdr, sw, base);

    if (phase == IDB_SR_INDEX) {
        JSValue index, key_path;

        JS_FreeValue(ctx, sw->index_key);
        sw->index_key = JS_UNDEFINED;
        if (sw->idx >= idb_list_len(ctx, sw->indexes)) {
            IDB_SR_GOTO(hdr, sw->after);                                         /* STEP 6 */
            return JS_STEP_YIELD;
        }
        index = sr_index_at(ctx, sw);
        /* "Let index key be the result of extracting a key from a value using a key path with VALUE, index's
           key path, and index's MULTIENTRY FLAG." The value is the one §6.1 was given — §4.5 step 10's clone,
           which is why §7.1's own precondition holds and why nothing here runs the page's code. */
        key_path = idb_index_key_path(ctx, index);
        idb_key_path_walk_start(ctx, hdr, &sw->w, &sw->kpres, sw->value, key_path,   /* STEP 5.1 */
                                idb_index_multi_entry(ctx, index), base, base + IDB_SR_INDEX_TOOK);
        JS_FreeValue(ctx, key_path);
        JS_FreeValue(ctx, index);
        return JS_STEP_YIELD;
    }

    if (phase == IDB_SR_INDEX_TOOK) {
        JSValue kpk;

        /* §6.1 STEP 5.2's other two arms: "invalid, or failure — take no further actions for index, and
           continue these steps for the next index." §7.1's three answers map onto exactly that one line. */
        if (idb_key_path_walk_take(ctx, &sw->w, sw->kpres, &kpk) != IDB_KEY_PATH_KEY) {
            JS_FreeValue(ctx, kpk);
            sw->idx++;
            IDB_SR_GOTO(hdr, base + IDB_SR_INDEX);
            return JS_STEP_YIELD;
        }
        sw->index_key = kpk;
        IDB_SR_GOTO(hdr, base + IDB_SR_INDEX_UNIQUE);
        return JS_STEP_YIELD;
    }

    if (phase == IDB_SR_INDEX_UNIQUE) {
        JSValue index = sr_index_at(ctx, sw);
        bool clash = sr_unique_clash(ctx, sw, index);                            /* STEPS 5.3-5.4 */

        JS_FreeValue(ctx, index);
        if (clash) {
            /* "then this operation failed with a ConstraintError DOMException. Abort this algorithm without
               taking any further steps." §5.6 step 5.4 is what puts back what steps 3, 4 and the earlier
               indexes already wrote — this algorithm reports the failure and undoes nothing itself. */
            JS_ThrowDOMException(ctx, "ConstraintError",
                                 "the value's key at a unique index's key path is already in that index");
            return JS_STEP_ABRUPT;
        }
        IDB_SR_GOTO(hdr, base + IDB_SR_INDEX_WRITE);
        return JS_STEP_YIELD;
    }

    DCHECK(phase == IDB_SR_INDEX_WRITE, "§6.1 was re-entered at a phase it never rests at");
    {
        JSValue index = sr_index_at(ctx, sw);

        idb_index_store_records(ctx, sw->tx, index, sw->index_key, sw->key);     /* STEPS 5.5-5.6 */
        JS_FreeValue(ctx, index);
        sw->idx++;
        IDB_SR_GOTO(hdr, base + IDB_SR_INDEX);
        return JS_STEP_YIELD;
    }
}

/* ---- §6.2's OBJECT STORE RETRIEVAL OPERATIONS --------------------------------------------------------------- */

/* §2.9's "a key is IN a key range", asked of ONE POSITION of a store's list. The four algorithms over a range
   — §6.2's two retrievals, §6.4's delete and §6.5's count — each ask it and each asked it differently while it
   was written out at the call site, which is four chances to read the record's key from somewhere else. */
static bool idb_record_in_range(JSContext *ctx, JSValueConst records, uint32_t i, JSValueConst range)
{
    JSValue rec = JS_GetPropertyUint32(ctx, records, i);
    JSValue key = JS_GetPropertyStr(ctx, rec, IDB_RECORD_KEY);
    bool in;

    DCHECK(JS_IsObject(key), "a record in an object store carried no key");
    in = idb_key_range_contains(ctx, range, key);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, rec);
    return in;
}

/* "Let record be the FIRST record in store's list of records whose key is IN RANGE, if any." First in the
   list's own order, which §2.2 makes key order — so this walk answers the smallest in-range key and a store
   whose list had gone out of order would answer with the wrong record rather than with none. -1 for "record
   was not found". */
static int idb_first_in_range(JSContext *ctx, JSValueConst records, JSValueConst range)
{
    uint32_t n = idb_list_len(ctx, records), i;

    for (i = 0; i < n; i++)
        if (idb_record_in_range(ctx, records, i, range))
            return (int)i;
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

/* ---- §6.4's DELETE RECORDS FROM AN OBJECT STORE and §6.6's OBJECT STORE CLEAR OPERATION ---------------------- */

/* THE ONE WRITE BOTH REMOVALS ARE MADE OF: `count` records leave the list at `from`, and what left is recorded
 * so §5.5 step 2 files it back.
 *
 * A SPAN AND NOT A SET OF POSITIONS, because §2.2 keeps the list in §2.4's key order and §2.9's range is an
 * INTERVAL in that order — so the records a range selects are contiguous, which is what idb_range_span asserts
 * where it computes them and what makes one shift of the tail the whole of the removal. §6.6 hands the whole
 * list, which is the same shape with no range to be an interval of. */
static void idb_records_remove_span(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst records,
                                    uint32_t from, uint32_t count)
{
    JSValue change, removed;
    uint32_t n = idb_list_len(ctx, records), i;

    DCHECK(from <= n && count <= n - from,
           "a span that is not inside an object store's list of records was removed from it");
    /* §5.5 step 2 records "THE CHANGES MADE to the database by the transaction", and a removal that removed
       nothing made none. That is not a decision about what the abort may see — it is the same sentence §6.1
       relies on when it records only after its two refusals — and it is what keeps a `delete` over keys the
       store does not hold from filing one empty change per call for the transaction's whole lifetime. */
    if (count == 0)
        return;
    removed = JS_NewArray(ctx);
    CHECK(!JS_IsException(removed), "IndexedDB: §5.5 step 2's record of the removed records could not be "
                                    "allocated");
    /* THE RECORDS THEMSELVES, HELD AND NOT COPIED — the same decision §6.1 makes for the record a put
       displaces, and for the same reason: a record is written once and replaced whole, so the object the list
       holds IS the state that key had, and the revert files that same object back. In key order, because that
       is the order the list holds them in. */
    for (i = 0; i < count; i++)
        JS_DefinePropertyValueUint32(ctx, removed, i, JS_GetPropertyUint32(ctx, records, from + i),
                                     JS_PROP_C_W_E);
    change = idb_change_new(ctx, IDB_CHANGE_RECORDS_REMOVED);
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_STORE, JS_DupValue(ctx, store));
    JS_SetPropertyStr(ctx, change, IDB_CHANGE_REMOVED, removed);
    idb_transaction_record_change(ctx, tx, change);

    /* DEFINE and not assign, for the reason §6.1's write states: an index accessor a page put on
       Array.prototype would otherwise swallow the shift and leave the list holding records that are gone. */
    for (i = from + count; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, records, i - count, JS_GetPropertyUint32(ctx, records, i),
                                     JS_PROP_C_W_E);
    JS_SetPropertyStr(ctx, records, "length", JS_NewUint32(ctx, n - count));
    /* §2.2's ordering survives the removal of a CONTIGUOUS span by construction, and the one join the removal
       creates is between the record before the span and the record now after it — which is the whole of what
       this write can break, the same way §6.1 states its two invariants against the one position it wrote. */
    DCHECK(from == 0 || from >= idb_list_len(ctx, records) ||
           idb_record_pos_compare(ctx, records, from - 1, from) < 0,
           "removing a span from an object store's list of records left it out of order: the record before "
           "the removed span has a key that is not less than the record now after it");
}

/* §6.4's "all records ... with key in range", as the SPAN they occupy. `*pcount` is 0 when none is in range.
   The walk asserts the contiguity the span shape rests on: once a record is in range, every later in-range
   record must be the next one, because §2.9's range is an interval in the order §2.2 sorts the list by. */
static void idb_range_span(JSContext *ctx, JSValueConst records, JSValueConst range,
                           uint32_t *pfrom, uint32_t *pcount)
{
    uint32_t n = idb_list_len(ctx, records), i;
    bool found = false;

    *pfrom = 0;
    *pcount = 0;
    for (i = 0; i < n; i++) {
        if (!idb_record_in_range(ctx, records, i, range))
            continue;
        DCHECK(!found || i == *pfrom + *pcount,
               "an object store's list of records holds an in-range record separated from the span that same "
               "range selected — §2.9's key range is an interval in §2.4's key order and §2.2 keeps the list "
               "in that order, so a range selects a contiguous run or the list is no longer sorted");
        if (!found) {
            *pfrom = i;
            found = true;
        }
        *pcount = i + 1 - *pfrom;
    }
}

void idb_delete_records(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst range)
{
    JSValue records = idb_store_records(ctx, store);
    uint32_t from, count;

    /* "Remove all records, if any, from store's list of records with key in range." */
    idb_range_span(ctx, records, range, &from, &count);          /* STEP 1 */
    idb_records_remove_span(ctx, tx, store, records, from, count);
    JS_FreeValue(ctx, records);
    /* "For each index which references store, remove every record from index's list of records whose VALUE is
       in range, if any such records exist." THE STORE'S RANGE AND NOT A PER-RECORD LOOP: an index record's
       value is a key of the referenced object store, so the records to remove are named by the same range step
       1 removed the store's records under — which is also why this runs even when step 1 removed nothing, the
       standard putting no condition on it. */
    idb_index_remove_records_in_value_range(ctx, tx, store, range);   /* STEP 2 */
    /* "Return undefined" is the caller's — the operation closure this runs inside answers §5.6 with it. */
}

void idb_clear_store(JSContext *ctx, JSValueConst tx, JSValueConst store)
{
    JSValue records = idb_store_records(ctx, store);

    /* "Remove all records from store." The whole list is the span, so this is §6.4 with the range that has no
       bounds — except that §6.6 has no range at all, which is why it is stated as its own algorithm rather
       than reached through a range this file would have had to mint. */
    idb_records_remove_span(ctx, tx, store, records, 0, idb_list_len(ctx, records));   /* STEP 1 */
    JS_FreeValue(ctx, records);
    /* "In all indexes which reference store, remove ALL records." No range at all, which is the difference
       from §6.4 step 2 rather than an unbounded range this file would have had to mint. */
    idb_index_remove_all_records(ctx, tx, store);                                     /* STEP 2 */
}

/* ---- §6.5's RECORD COUNTING OPERATION ------------------------------------------------------------------------ */

uint32_t idb_store_record_count(JSContext *ctx, JSValueConst store)
{
    JSValue records = idb_store_records(ctx, store);
    uint32_t n = idb_list_len(ctx, records);

    JS_FreeValue(ctx, records);
    return n;
}

uint32_t idb_count_records(JSContext *ctx, JSValueConst store, JSValueConst range)
{
    JSValue records = idb_store_records(ctx, store);
    uint32_t n = idb_list_len(ctx, records), i, count = 0;

    /* "Let count be the number of records, if any, in source's list of records with key in range. Return
       count." Every record is asked. It is NOT idb_range_span's width: that entry's contiguity is an assertion
       the REMOVAL rests on, and a count that inherited it would report the assertion's answer instead of the
       standard's plain one. */
    for (i = 0; i < n; i++)
        if (idb_record_in_range(ctx, records, i, range))
            count++;
    JS_FreeValue(ctx, records);
    return count;
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
/* THE ONE BACKWARDS WALK both §5.5 step 2 and §5.6 step 5.4 are made of, over the half-open span [from, n) of
   the transaction's list of changes. The two entries below differ only in WHICH span they name and in what
   they do with the list afterwards. */
static void idb_revert_span(JSContext *ctx, JSValueConst changes, uint32_t from, uint32_t n)
{
    uint32_t i;

    for (i = n; i > from; i--) {
        JSValue change = JS_GetPropertyUint32(ctx, changes, i - 1);
        JSValue db, store, index, key, records, name;
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
            /* §6.1 step 4 ALWAYS ADDED, because its step 3 removed whatever was under this key first — so the
               inverse is a removal, and a put that overwrote is put back by the §6.4 removal step 3 recorded,
               which the backwards walk reaches next. */
            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            key = idb_change_get(ctx, change, IDB_CHANGE_KEY);
            records = idb_store_records(ctx, store);
            pos = idb_record_pos(ctx, records, key, &exists);
            DCHECK(exists, "a record this transaction stored is no longer in the store it was stored in — §6.1 "
                           "is the only algorithm that writes this list, and §2.7.2 gives one store to one "
                           "read/write transaction at a time, so nothing else can have removed it");
            len = idb_list_len(ctx, records);
            for (j = pos; j + 1 < len; j++)
                JS_DefinePropertyValueUint32(ctx, records, j, JS_GetPropertyUint32(ctx, records, j + 1),
                                             JS_PROP_C_W_E);
            JS_SetPropertyStr(ctx, records, "length", JS_NewUint32(ctx, len - 1));
            JS_FreeValue(ctx, records);
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, store);
            break;
        case IDB_CHANGE_RECORDS_REMOVED: {
            JSValue gone;
            uint32_t r, rn;

            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            gone = idb_change_get(ctx, change, IDB_CHANGE_REMOVED);
            records = idb_store_records(ctx, store);
            rn = idb_list_len(ctx, gone);
            /* Each record goes back at the ONE position its key can occupy in a sorted list, found by the same
               search §6.1's write and its revert use — so what this leaves is the list §2.2 describes rather
               than one this arm assembled out of a remembered index. FORWARDS over the removed list, because
               it is in key order and the insertion point of each is at or after the last. */
            for (r = 0; r < rn; r++) {
                JSValue rec = JS_GetPropertyUint32(ctx, gone, r);

                key = JS_GetPropertyStr(ctx, rec, IDB_RECORD_KEY);
                DCHECK(JS_IsObject(key), "a record removed from an object store carried no key");
                pos = idb_record_pos(ctx, records, key, &exists);
                DCHECK(!exists, "a record this transaction REMOVED is in the store again under its own key — "
                                "§2.7.2 gives one store to one read/write transaction at a time, so the only "
                                "writer between the removal and this revert is that transaction, and every "
                                "write it made after the removal has already been undone (the list of changes "
                                "is run backwards for exactly this)");
                len = idb_list_len(ctx, records);
                for (j = len; j > pos; j--)
                    JS_DefinePropertyValueUint32(ctx, records, j, JS_GetPropertyUint32(ctx, records, j - 1),
                                                 JS_PROP_C_W_E);
                JS_DefinePropertyValueUint32(ctx, records, pos, rec, JS_PROP_C_W_E);
                JS_FreeValue(ctx, key);
            }
            JS_FreeValue(ctx, records);
            JS_FreeValue(ctx, gone);
            JS_FreeValue(ctx, store);
            break;
        }
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
        case IDB_CHANGE_KEY_GENERATOR: {
            JSValue gen, was;
            int64_t number = 0;
            int r;

            /* §2.11: "aborting a transaction rolls back any increases to the key generator which happened
               during the transaction. This is to make all rollbacks consistent since rollbacks that happen due
               to crash never has a chance to commit the increased key generator value." Each increase recorded
               one change, and the list runs BACKWARDS, so the last write this arm performs is the earliest
               recorded number — "the value it had before the transaction was started". */
            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            was = idb_change_get(ctx, change, IDB_CHANGE_NUMBER);
            r = JS_ToBigInt64(ctx, &number, was);
            DCHECK(r >= 0, "a recorded key-generator change carried a current number that is not an exact "
                           "integer — idb_object_store_record_generator_change writes a BigInt and nothing "
                           "else writes this field");
            (void)r;
            gen = idb_object_store_key_generator(ctx, store);
            idb_key_generator_revert(ctx, gen, number);
            JS_FreeValue(ctx, gen);
            JS_FreeValue(ctx, was);
            JS_FreeValue(ctx, store);
            break;
        }
        case IDB_CHANGE_INDEX_CREATED:
            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            index = idb_change_get(ctx, change, IDB_CHANGE_INDEX);
            /* "For upgrade transactions this includes changes to the set of object stores AND INDEXES ... any
               object stores AND INDEXES which were created during the transaction are now considered deleted
               for the purposes of other algorithms." */
            idb_index_revert_creation(ctx, store, index);
            JS_FreeValue(ctx, index);
            JS_FreeValue(ctx, store);
            break;
        case IDB_CHANGE_INDEX_DESTROYED:
        case IDB_CHANGE_INDEX_RENAMED:
            store = idb_change_get(ctx, change, IDB_CHANGE_STORE);
            index = idb_change_get(ctx, change, IDB_CHANGE_INDEX);
            name = idb_change_get(ctx, change, IDB_CHANGE_NAME);
            cname = JS_ToCString(ctx, name);
            CHECK(cname != NULL, "IndexedDB: an index's former name could not be read back to restore it");
            /* THE TWO ARE ONE ARM because the state each undoes is the same pair — the name and the set — and
               a destroy differs only in that the index also has to rejoin the set it left. */
            if (kind == IDB_CHANGE_INDEX_DESTROYED)
                idb_index_revert_destruction(ctx, store, index, cname);
            else
                idb_index_revert_rename(ctx, store, index, cname);
            JS_FreeCString(ctx, cname);
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, index);
            JS_FreeValue(ctx, store);
            break;
        case IDB_CHANGE_INDEX_RECORDS_STORED:
        case IDB_CHANGE_INDEX_RECORDS_REMOVED:
            index = idb_change_get(ctx, change, IDB_CHANGE_INDEX);
            records = idb_change_get(ctx, change, IDB_CHANGE_RECORDS);
            if (kind == IDB_CHANGE_INDEX_RECORDS_STORED)
                idb_index_revert_records_stored(ctx, index, records);
            else
                idb_index_revert_records_removed(ctx, index, records);
            JS_FreeValue(ctx, records);
            JS_FreeValue(ctx, index);
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
}

void idb_database_revert_transaction(JSContext *ctx, JSValueConst tx)
{
    JSValue changes = idb_transaction_changes(ctx, tx);

    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "the changes of a FINISHED transaction were reverted — §5.5 step 1 abandons the abort of a finished "
           "transaction, so reaching here twice means the changes are being undone against a database that has "
           "already had them undone once");
    /* THE LIST IS NOT TRUNCATED HERE, unlike §5.6 step 5.4's revert below. §5.8 step 5.1 asks this same list
       which object stores this transaction CREATED, and §5.5 runs §5.8 (its step 3) before it sets the state
       to finished (its step 4) — which is what empties the list. */
    idb_revert_span(ctx, changes, 0, idb_list_len(ctx, changes));
    /* AND THE LIST ITSELF, which idb_transaction_changes hands over OWNED. Dropping it here was worth a whole
       browser: the list is emptied when the transaction reaches finished, so what survived was an EMPTY Array
       — and an Array holds Array.prototype, which holds the realm's function objects, each of which holds the
       REALM. Three reverts in one fixture therefore kept 2612 Functions, 408 shapes and a JSContext at
       refcount 3108 alive, reported by the runtime's leak walk as three anonymous `Array [ ] { length: 0 }`
       with nothing naming an owner. idb_transaction_set_state now asserts that nobody is still holding one at
       the moment the transaction finishes, which is where the next dropped reference will be named. */
    JS_FreeValue(ctx, changes);
}

void idb_database_revert_operation(JSContext *ctx, JSValueConst tx, uint32_t from)
{
    JSValue changes = idb_transaction_changes(ctx, tx);
    uint32_t n = idb_list_len(ctx, changes);

    DCHECK(from <= n, "§5.6 step 5.4's watermark is beyond the transaction's list of changes. It is the length "
                      "that list had before the operation ran, and the list only grows while one runs — so a "
                      "watermark past its end means something REMOVED changes underneath the operation");
    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "an operation's changes were reverted against a FINISHED transaction — §5.6's task terminates for "
           "every request of an aborted transaction, exactly so that no operation runs past that point");
    idb_revert_span(ctx, changes, from, n);
    /* THE REVERTED CHANGES LEAVE THE LIST. They are no longer changes the transaction made — §5.6 step 5.4's
       own note is that this "only reverts the changes done by this request" — so a later abort must not undo
       them a second time, which would fire every one of the inverses' own asserts. */
    JS_SetPropertyStr(ctx, changes, "length", JS_NewUint32(ctx, from));
    JS_FreeValue(ctx, changes);
}

/* "ANY OBJECT STORES ... WHICH WERE CREATED DURING THE TRANSACTION" — one store, asked of the list the revert
   above runs. The list is READ FORWARDS here because this is a membership question and not a composition: a
   store either appears as a creation of this transaction or it does not, and a store created and then
   destroyed inside one transaction was still created by it (§5.8 step 5.1 skips its handle either way). */
static bool idb_created_by(JSContext *ctx, JSValueConst tx, JSValueConst thing, int kind, const char *field)
{
    JSValue changes = idb_transaction_changes(ctx, tx);
    uint32_t i, n = idb_list_len(ctx, changes);
    bool created = false;

    DCHECK(JS_IsObject(thing), "\"was this newly created during the transaction\" was asked of something that "
                               "is not a §2.2 object store or a §2.6 index");
    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "a FINISHED transaction was asked what it created. Its list of changes is emptied when it reaches "
           "that state, so the answer would be NO for everything — and §5.8 steps 5.1 and 6 would then rename "
           "the handle of something this transaction created, which is the one case they must not");
    for (i = 0; i < n && !created; i++) {
        JSValue change = JS_GetPropertyUint32(ctx, changes, i);

        DCHECK(JS_IsObject(change), "a transaction's list of database changes had a hole in it");
        if (idb_change_kind(ctx, change) == kind) {
            JSValue made = idb_change_get(ctx, change, field);

            created = JS_VALUE_GET_PTR(made) == JS_VALUE_GET_PTR(thing);
            JS_FreeValue(ctx, made);
        }
        JS_FreeValue(ctx, change);
    }
    JS_FreeValue(ctx, changes);
    return created;
}

bool idb_database_store_was_created_by(JSContext *ctx, JSValueConst tx, JSValueConst store)
{
    return idb_created_by(ctx, tx, store, IDB_CHANGE_STORE_CREATED, IDB_CHANGE_STORE);
}

bool idb_database_index_was_created_by(JSContext *ctx, JSValueConst tx, JSValueConst index)
{
    return idb_created_by(ctx, tx, index, IDB_CHANGE_INDEX_CREATED, IDB_CHANGE_INDEX);
}
