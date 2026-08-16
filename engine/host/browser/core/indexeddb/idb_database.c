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
 * capture — that mechanism is for state a component keeps in C, and this component keeps none. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/structured_clone.h"

/* §2.1's, §2.2's and the record's OWN FIELDS, spelled once so the writers and the readers cannot disagree. */
#define IDB_DB_NAME             "name"
#define IDB_DB_VERSION          "version"
#define IDB_DB_STORES           "stores"        /* §2.1's "set of object stores", keyed by name */

#define IDB_STORE_NAME          "name"
#define IDB_STORE_KEY_PATH      "keyPath"       /* §2.2's optional key path — JS_NULL for out-of-line keys */
#define IDB_STORE_KEY_GENERATOR "keyGenerator"  /* §2.2's optional key generator */
#define IDB_STORE_RECORDS       "records"       /* §2.2's "list of records ... sorted according to key" */

#define IDB_RECORD_KEY          "key"
#define IDB_RECORD_VALUE        "value"

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

/* ---- §2.1's database ---------------------------------------------------------------------------------------- */

void idb_database_init(JSContext *ctx)
{
    DCHECK(JS_IsUndefined(g_databases), "idb_database_init ran twice — §2.1's set of databases belongs to the "
                                        "AGENT, and a second one would give a same-origin child navigable a "
                                        "storage area of its own");
    g_databases = idl_slots_new(ctx);
    CHECK(!JS_IsException(g_databases), "IndexedDB: §2.1's set of databases could not be allocated");
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
    JSValue existing = idb_database_find(ctx, name), db, stores;

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
    JS_SetPropertyStr(ctx, g_databases, name, JS_DupValue(ctx, db));
    return db;
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

/* ---- §2.1.1's database connection ---------------------------------------------------------------------------- */

#define IDB_CONN_DATABASE "database"
#define IDB_CONN_VERSION  "version"        /* §2.1.1's version, "set when the connection is created" */
#define IDB_CONN_STORES   "stores"         /* §2.1.1's object store set */
#define IDB_CONN_CLOSING  "closePending"   /* §2.1.1's close pending flag, "initially false" */

JSValue idb_connection_open(JSContext *ctx, JSValueConst db)
{
    JSValue conn, stores;

    DCHECK(JS_IsObject(db), "a connection was opened to something that is not a §2.1 database");
    conn = idl_slots_new(ctx);
    CHECK(!JS_IsException(conn), "IndexedDB: §2.1.1's connection record could not be allocated");
    stores = JS_GetPropertyStr(ctx, db, IDB_DB_STORES);
    DCHECK(JS_IsObject(stores), "a database carried no set of object stores");
    JS_SetPropertyStr(ctx, conn, IDB_CONN_DATABASE, JS_DupValue(ctx, db));
    /* "A connection has a version, which is set when the connection is created." */
    JS_SetPropertyStr(ctx, conn, IDB_CONN_VERSION, JS_NewFloat64(ctx, idb_database_version(ctx, db)));
    /* "An object store set, which is initialized to the set of object stores in the associated database when
       the connection is created. The contents of the set will remain constant except when an upgrade
       transaction is live." The SAME record, not a copy: with no upgrade transaction in existence there is
       nothing that can make the two diverge, and a copy taken now would be a second answer to one question
       the day §5.7 lands — which is exactly the shape §Offensive-programming calls a plausible datum. */
    JS_SetPropertyStr(ctx, conn, IDB_CONN_STORES, stores);
    JS_SetPropertyStr(ctx, conn, IDB_CONN_CLOSING, JS_FALSE);
    return conn;
}

JSValue idb_connection_database(JSContext *ctx, JSValueConst connection)
{
    JSValue db = JS_GetPropertyStr(ctx, connection, IDB_CONN_DATABASE);

    DCHECK(JS_IsObject(db), "a connection carried no database — every connection is built by "
                            "idb_connection_open, which gives it one");
    return db;
}

/* ---- §2.2's object store ------------------------------------------------------------------------------------ */

JSValue idb_object_store_create(JSContext *ctx, JSValueConst db, const char *name, JSValue key_path,
                                bool key_generator)
{
    JSValue stores = JS_GetPropertyStr(ctx, db, IDB_DB_STORES), existing, store, records;

    DCHECK(JS_IsObject(stores), "a database carried no set of object stores — every database is built by "
                                "idb_database_create, which gives it one");
    existing = JS_GetPropertyStr(ctx, stores, name);
    DCHECK(JS_IsUndefined(existing), "two object stores were created under one name in one database — §2.2: "
                                     "\"At any one time, the name is unique within the database to which it "
                                     "belongs\", which §4.4's createObjectStore reports as a ConstraintError "
                                     "before it reaches this");
    JS_FreeValue(ctx, existing);
    store = idl_slots_new(ctx);
    CHECK(!JS_IsException(store), "IndexedDB: §2.2's object store record could not be allocated");
    records = JS_NewArray(ctx);
    CHECK(!JS_IsException(records), "IndexedDB: an object store's list of records could not be allocated");
    JS_SetPropertyStr(ctx, store, IDB_STORE_NAME, JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, store, IDB_STORE_KEY_PATH, key_path);
    JS_SetPropertyStr(ctx, store, IDB_STORE_KEY_GENERATOR, JS_NewBool(ctx, key_generator));
    JS_SetPropertyStr(ctx, store, IDB_STORE_RECORDS, records);
    JS_SetPropertyStr(ctx, stores, name, JS_DupValue(ctx, store));
    JS_FreeValue(ctx, stores);
    return store;
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

int idb_store_record(JSContext *ctx, JSValueConst store, JSValueConst value, JSValueConst key,
                     bool no_overwrite, JSValue *pkey)
{
    JSValue records, key_path, generator, clone, rec;
    uint32_t n, lo, hi, mid, pos, i;
    bool exists, derived;

    *pkey = JS_UNDEFINED;
    key_path = JS_GetPropertyStr(ctx, store, IDB_STORE_KEY_PATH);
    generator = JS_GetPropertyStr(ctx, store, IDB_STORE_KEY_GENERATOR);
    derived = JS_ToBool(ctx, generator) || !JS_IsNull(key_path);
    JS_FreeValue(ctx, key_path);
    JS_FreeValue(ctx, generator);
    if (derived) {
        /* §6.1's FIRST STEP, and §4.5's step before it, are not built — and neither can be reached today,
           because the only member that gives a store a key path or a key generator is §4.4's
           createObjectStore, which does not exist. It crashes HERE because this is where the absence would
           otherwise become a wrong record: a store with a key generator would file its record under a key
           nothing generated, and a store with in-line keys under a key nothing extracted. */
        DFAIL("Indexed Database §6.1's KEY-GENERATOR / IN-LINE-KEY step is not built. A store with a key "
              "generator needs §2.11's \"generate a key\" (a monotonically increasing number) and \"possibly "
              "update the key generator\"; where it ALSO uses in-line keys it needs §7.2's \"inject a key into "
              "a value using a key path\", which WRITES into the value being stored and therefore runs on the "
              "clone below and never on the page's own object. A store with a key path and no generator needs "
              "§7.1's \"extract a key from a value using a key path\", which §4.5's `put` runs BEFORE this "
              "algorithm — so that half lands with `put` and not here. Build them with createObjectStore, the "
              "only member that can give a store either one");
        /* A RELEASE BUILD FALLS THROUGH TO THE ALGORITHM'S OWN ANSWER for a key it could not derive — §6.1
           step 1.1.2: "If key is failure, then this operation failed with a ConstraintError DOMException." A
           bare -1 with nothing thrown would be a caller returning JS_EXCEPTION with no exception live. */
        JS_ThrowDOMException(ctx, "ConstraintError", "a key could not be generated for the object store");
        return -1;
    }
    DCHECK(JS_IsObject(key), "§6.1 was handed no key for a store that has no key generator. The key is §7.4's "
                             "key RECORD and not the page's value: §4.5's `put` converts it and reports that "
                             "conversion's DataError, and a store with out-of-line keys and no generator makes "
                             "an absent key a DataError there too");

    records = idb_store_records(ctx, store);
    n = idb_list_len(ctx, records);
    /* WHERE THE KEY BELONGS, by §2.4's compare over a list §2.2 keeps sorted. The search is a binary one
       BECAUSE the list is sorted — that ordering is not decoration, it is what makes a store an index rather
       than a bag, and a walk here would quietly make every put O(n). `lo` lands on the first record whose key
       is not less than `key`, which is both the insertion point and — when it compares equal — §6.1's "a
       record already exists in store with its key equal to key". */
    lo = 0;
    hi = n;
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (idb_record_key_compare(ctx, records, mid, key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    pos = lo;
    exists = pos < n && idb_record_key_compare(ctx, records, pos, key) == 0;

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
