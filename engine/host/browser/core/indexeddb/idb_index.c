/* INDEXED DATABASE §2.6's INDEX — the second persistent key-value store this standard has, and §6.3 over it.
 *
 * WHY IT IS A COMPONENT OF ITS OWN AND NOT MORE OF idb_database.c. An index's list of records is sorted by a
 * DIFFERENT ordering from an object store's — "sorted primarily on the records keys, and SECONDARILY ON THE
 * RECORDS VALUES" — and it admits DUPLICATE KEYS where §2.2 forbids them ("there can never be multiple records
 * in a given object store with the same key"). Those are the two facts every write below rests on, so the two
 * lists cannot share one insertion point, one search or one invariant. What the two files do share is §5.5 step
 * 2's LEDGER, which stays idb_database.c's: the five changes this component makes are filed through that file's
 * five recorders and undone by the five inverses at the bottom of this one, exactly as §2.11's key generator is.
 *
 * WHAT AN INDEX RECORD IS. §2.6: "if a given record with key X in the object store referenced by the index has
 * the value A, and evaluating the index's key path on A yields the result Y, then the index will contain a
 * record with key Y and value X." So the record's KEY is the index key and its VALUE is a key of the referenced
 * object store — both §2.4 key records, so both time-travel with the flow for the reason idb_database.c's do,
 * and the REFERENCED VALUE (§6.3's first retrieval) is not stored at all: it is read back out of the store
 * through that primary key, which is what makes an index a view rather than a second copy of the data.
 *
 * THE REFERENCED OBJECT STORE IS ON THE RECORD because §2.6 puts it there, and it is what every assert below
 * about "which store is this index of" is stated against. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_index.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_range.h"

/* §2.6's FIELDS, spelled once so the writers and the readers cannot disagree. */
#define IDB_INDEX_NAME      "name"
#define IDB_INDEX_KEY_PATH  "keyPath"
#define IDB_INDEX_UNIQUE    "unique"       /* §2.6's unique flag */
#define IDB_INDEX_MULTI     "multiEntry"   /* §2.6's multiEntry flag */
#define IDB_INDEX_STORE     "store"        /* §2.6's referenced object store */
#define IDB_INDEX_RECORDS   "records"      /* §2.6's list of records */
#define IDB_INDEX_DELETED   "deleted"      /* §4.5's deleteIndex step 9, "Destroy index" */
/* THE ONE FIELD §2.6 DOES NOT NAME, and §4.5's note is what puts it here: "the index creation itself is
   processed as an ASYNCHRONOUS REQUEST within the upgrade transaction". §4.5's steps create the index
   synchronously, so between step 11 and the moment that request runs there is an index which references the
   store and which the store's records have not been walked into — and §6.1 step 5 must not see it, or the two
   `put`s §4.5's own worked example queues BEFORE the createIndex would file records in it and the SECOND put
   would fail with the ConstraintError the note says belongs to the index creation. */
#define IDB_INDEX_UNPOPULATED "unpopulated"

/* ONE INDEX RECORD: the index key it is filed under, and the referenced object store key it points at. */
#define IDB_IREC_KEY        "key"
#define IDB_IREC_VALUE      "value"

/* ---- the lists, as this file reads them ------------------------------------------------------------------- */

/* HOW LONG ONE OF THIS FILE'S OWN LISTS IS. Engine-built, holding engine-built records, so a malformed one is
   the component disagreeing with itself and crashes rather than reporting. */
static uint32_t idb_index_list_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;
    int r;

    DCHECK(!JS_IsException(len), "reading the length of an index's list threw — it is this component's own "
                                 "Array and has no getters to run");
    r = JS_ToUint32(ctx, &n, len);
    DCHECK(r >= 0, "an index's list had a length that is not a number");
    (void)r;
    JS_FreeValue(ctx, len);
    return n;
}

static JSValue idb_index_records(JSContext *ctx, JSValueConst index)
{
    JSValue records = JS_GetPropertyStr(ctx, index, IDB_INDEX_RECORDS);

    DCHECK(JS_IsArray(records), "an index carried no list of records — every index is built by "
                                "idb_index_create, which gives it one");
    return records;
}

static JSValue idb_irec_field(JSContext *ctx, JSValueConst records, uint32_t i, const char *field)
{
    JSValue rec = JS_GetPropertyUint32(ctx, records, i), v;

    DCHECK(JS_IsObject(rec), "an index's list of records had a hole in it");
    v = JS_GetPropertyStr(ctx, rec, field);
    DCHECK(JS_IsObject(v), "an index record carried no key or no value — §2.6 makes a record a key AND a "
                           "value, and this list holds only records idb_index_store_records built");
    JS_FreeValue(ctx, rec);
    return v;
}

/* §2.6's ORDERING, as ONE comparison: "sorted primarily on the records keys, and secondarily on the records
   values, in ascending order". Both halves are §2.4's compare, which is why this is one function and not a
   cascade written out at each of the four sites that needs it. */
static int idb_irec_compare(JSContext *ctx, JSValueConst records, uint32_t i, JSValueConst key,
                            JSValueConst value)
{
    JSValue rk = idb_irec_field(ctx, records, i, IDB_IREC_KEY), rv;
    int c = idb_key_compare(ctx, rk, key);

    JS_FreeValue(ctx, rk);
    if (c != 0)
        return c;
    rv = idb_irec_field(ctx, records, i, IDB_IREC_VALUE);
    c = idb_key_compare(ctx, rv, value);
    JS_FreeValue(ctx, rv);
    return c;
}

/* WHERE (key, value) BELONGS in the sorted list, and whether the list already holds that exact pair. BINARY,
   because §2.6 keeps the list sorted and a walk here would make every put O(n) per index. It is one function
   because a write and the revert of that write must land on the SAME position. */
static uint32_t idb_irec_pos(JSContext *ctx, JSValueConst records, JSValueConst key, JSValueConst value,
                             bool *exists)
{
    uint32_t n = idb_index_list_len(ctx, records), lo = 0, hi = n, mid;

    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        if (idb_irec_compare(ctx, records, mid, key, value) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *exists = lo < n && idb_irec_compare(ctx, records, lo, key, value) == 0;
    return lo;
}

/* THE ONE INSERT this file performs, with §2.6's ordering asserted against the one position it changed — the
   whole of what a single insert can break. DEFINE and not assign, for idb_database.c's reason: an index
   accessor a page put on Array.prototype would otherwise swallow the write. */
static void idb_irec_insert(JSContext *ctx, JSValueConst records, uint32_t pos, JSValue rec)
{
    uint32_t n = idb_index_list_len(ctx, records), i;
    JSValue key = JS_GetPropertyStr(ctx, rec, IDB_IREC_KEY);
    JSValue value = JS_GetPropertyStr(ctx, rec, IDB_IREC_VALUE);

    for (i = n; i > pos; i--)
        JS_DefinePropertyValueUint32(ctx, records, i, JS_GetPropertyUint32(ctx, records, i - 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, records, pos, rec, JS_PROP_C_W_E);
    DCHECK(pos == 0 || idb_irec_compare(ctx, records, pos - 1, key, value) < 0,
           "an index's list of records is no longer sorted: the record before the one just written is not "
           "strictly before it in (key, value) order");
    DCHECK(pos + 1 >= idb_index_list_len(ctx, records) ||
           idb_irec_compare(ctx, records, pos + 1, key, value) > 0,
           "an index's list of records is no longer sorted, or holds one (key, value) pair twice: the record "
           "after the one just written is not strictly after it");
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, key);
}

/* ONE POSITION REMOVED. The tail comes down, which is the whole of the removal because §2.6's ordering survives
   the removal of any single record by construction. */
static void idb_irec_remove_at(JSContext *ctx, JSValueConst records, uint32_t pos)
{
    uint32_t n = idb_index_list_len(ctx, records), i;

    DCHECK(pos < n, "a position outside an index's list of records was removed from it");
    for (i = pos; i + 1 < n; i++)
        JS_DefinePropertyValueUint32(ctx, records, i, JS_GetPropertyUint32(ctx, records, i + 1), JS_PROP_C_W_E);
    JS_SetPropertyStr(ctx, (JSValue)records, "length", JS_NewUint32(ctx, n - 1));
}

/* ---- §2.6's set of indexes on a store --------------------------------------------------------------------- */

JSValue idb_store_index_set(JSContext *ctx, JSValueConst store)
{
    JSValue set = idb_object_store_indexes(ctx, store);

    DCHECK(JS_IsArray(set), "an object store carried no set of indexes — idb_object_store_create gives every "
                            "store one, empty, and only this file adds to it");
    return set;
}

uint32_t idb_store_index_count(JSContext *ctx, JSValueConst store)
{
    JSValue set = idb_store_index_set(ctx, store);
    uint32_t n = idb_index_list_len(ctx, set);

    JS_FreeValue(ctx, set);
    return n;
}

/* THE INDEX AT ONE POSITION of a store's set. OWNED. */
static JSValue idb_store_index_at(JSContext *ctx, JSValueConst set, uint32_t i)
{
    JSValue index = JS_GetPropertyUint32(ctx, set, i);

    DCHECK(JS_IsObject(index), "an object store's set of indexes had a hole in it");
    return index;
}

JSValue idb_store_populated_index_set(JSContext *ctx, JSValueConst store)
{
    JSValue set = idb_store_index_set(ctx, store), out = JS_NewArray(ctx);
    uint32_t i, n = idb_index_list_len(ctx, set), k = 0;

    CHECK(!JS_IsException(out), "IndexedDB: §6.1 step 5's list of indexes could not be allocated");
    for (i = 0; i < n; i++) {
        JSValue index = idb_store_index_at(ctx, set, i);

        if (idb_index_is_unpopulated(ctx, index))
            JS_FreeValue(ctx, index);
        else
            JS_DefinePropertyValueUint32(ctx, out, k++, index, JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, set);
    return out;
}

/* A store's set JOINED and LEFT — the two writes every change to it is made of, factored for the reason
   idb_database.c factors the object store set's: a destroy is a leave, its revert is a join, and a second copy
   of either would be a second place the set is written from. */
static void idb_index_set_join(JSContext *ctx, JSValueConst store, JSValueConst index)
{
    JSValue set = idb_store_index_set(ctx, store);

    JS_DefinePropertyValueUint32(ctx, set, idb_index_list_len(ctx, set), JS_DupValue(ctx, index), JS_PROP_C_W_E);
    JS_FreeValue(ctx, set);
}

static void idb_index_set_leave(JSContext *ctx, JSValueConst store, JSValueConst index)
{
    JSValue set = idb_store_index_set(ctx, store);
    uint32_t i, n = idb_index_list_len(ctx, set);

    for (i = 0; i < n; i++) {
        JSValue member = idb_store_index_at(ctx, set, i);
        bool same = JS_VALUE_GET_PTR(member) == JS_VALUE_GET_PTR(index);

        JS_FreeValue(ctx, member);
        if (!same) continue;
        for (; i + 1 < n; i++)
            JS_DefinePropertyValueUint32(ctx, set, i, JS_GetPropertyUint32(ctx, set, i + 1), JS_PROP_C_W_E);
        JS_SetPropertyStr(ctx, set, "length", JS_NewUint32(ctx, n - 1));
        JS_FreeValue(ctx, set);
        return;
    }
    JS_FreeValue(ctx, set);
    DFAIL("an index left a store's set of indexes without being in it — an index joins at idb_index_create and "
          "leaves exactly once, at §4.5's deleteIndex or at §5.5 step 2's revert of its creation");
}

/* §2.6's "at any one time, the name is unique within index's referenced object store", asserted where a name is
   written. §4.5's createIndex step 6 and §4.6's name setter step 9 each report the "ConstraintError" that makes
   it true before either reaches this. */
static void idb_index_write_name(JSContext *ctx, JSValueConst store, JSValueConst index, const char *name)
{
    JSValue existing;

    DCHECK(name != NULL, "an index was given no name");
    existing = idb_index_find(ctx, store, name);
    DCHECK(JS_IsNull(existing) || JS_VALUE_GET_PTR(existing) == JS_VALUE_GET_PTR(index),
           "an index was named for a name its referenced object store already holds a DIFFERENT index under — "
           "§2.6: \"at any one time, the name is unique within index's referenced object store\"");
    JS_FreeValue(ctx, existing);
    JS_SetPropertyStr(ctx, (JSValue)index, IDB_INDEX_NAME, JS_NewString(ctx, name));
}

JSValue idb_index_create(JSContext *ctx, JSValueConst tx, JSValueConst store, const char *name,
                         JSValue key_path, bool unique, bool multi_entry)
{
    JSValue index, records;

    DCHECK(JS_IsObject(store), "an index was created over something that is not a §2.2 object store");
    DCHECK(!JS_IsNull(key_path) && !JS_IsUndefined(key_path),
           "an index was created with NO key path — §2.6 derives an index's keys from the referenced object "
           "store's values \"using a key path\", and §4.5's createIndex declares `keyPath` as a required "
           "non-nullable argument for exactly that reason");
    index = idl_slots_new(ctx);
    CHECK(!JS_IsException(index), "IndexedDB: §2.6's index record could not be allocated");
    records = JS_NewArray(ctx);
    CHECK(!JS_IsException(records), "IndexedDB: an index's list of records could not be allocated");
    JS_SetPropertyStr(ctx, index, IDB_INDEX_KEY_PATH, key_path);
    JS_SetPropertyStr(ctx, index, IDB_INDEX_UNIQUE, JS_NewBool(ctx, unique));
    JS_SetPropertyStr(ctx, index, IDB_INDEX_MULTI, JS_NewBool(ctx, multi_entry));
    JS_SetPropertyStr(ctx, index, IDB_INDEX_STORE, JS_DupValue(ctx, store));
    JS_SetPropertyStr(ctx, index, IDB_INDEX_RECORDS, records);
    JS_SetPropertyStr(ctx, index, IDB_INDEX_DELETED, JS_FALSE);
    /* UNPOPULATED until the request §4.5's note describes has walked the store's records into it. The flag
       needs no inverse in §5.5 step 2's ledger: the only thing that can clear it is that request, and an abort
       that reaches this index at all also reverts its CREATION (idb_index_revert_creation), which destroys it. */
    JS_SetPropertyStr(ctx, index, IDB_INDEX_UNPOPULATED, JS_TRUE);
    /* NAMED BEFORE IT JOINS, which is the order every other write here has: the uniqueness assert walks the
       set and asks each member its name, so an index in the set without one yet would be asked a question it
       has no answer to. */
    idb_index_write_name(ctx, store, index, name);
    idb_index_set_join(ctx, store, index);
    idb_database_record_index_created(ctx, tx, store, index);
    return index;
}

JSValue idb_index_find_in(JSContext *ctx, JSValueConst set, const char *name)
{
    JSValue found = JS_NULL;
    uint32_t i, n = idb_index_list_len(ctx, set);

    DCHECK(name != NULL, "an index was looked up under no name — §2.6's name identifies it within its "
                         "referenced object store, and the empty string is a name while a null pointer is not");
    for (i = 0; i < n && JS_IsNull(found); i++) {
        JSValue index = idb_store_index_at(ctx, set, i), iname = idb_index_name(ctx, index);
        const char *c = JS_ToCString(ctx, iname);

        CHECK(c != NULL, "IndexedDB: an index could not report its own name");
        if (!strcmp(c, name))
            found = JS_DupValue(ctx, index);
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, iname);
        JS_FreeValue(ctx, index);
    }
    return found;
}

JSValue idb_index_find(JSContext *ctx, JSValueConst store, const char *name)
{
    JSValue set = idb_store_index_set(ctx, store), found = idb_index_find_in(ctx, set, name);

    JS_FreeValue(ctx, set);
    return found;
}

void idb_index_destroy(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index)
{
    /* THE NAME IT WAS FILED UNDER travels with the change and is not read back at revert time, for the reason
       idb_object_store_destroy states: the revert puts it back into the set it left. */
    idb_database_record_index_destroyed(ctx, tx, store, index, idb_index_name(ctx, index));
    idb_index_set_leave(ctx, store, index);
    JS_SetPropertyStr(ctx, (JSValue)index, IDB_INDEX_DELETED, JS_TRUE);
}

void idb_index_rename(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index, const char *name)
{
    idb_database_record_index_renamed(ctx, tx, store, index, idb_index_name(ctx, index));
    idb_index_write_name(ctx, store, index, name);
}

bool idb_index_is_deleted(JSContext *ctx, JSValueConst index)
{
    JSValue v = JS_GetPropertyStr(ctx, index, IDB_INDEX_DELETED);
    bool b;

    DCHECK(JS_IsBool(v), "an index carried no deleted flag");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

bool idb_index_is_unpopulated(JSContext *ctx, JSValueConst index)
{
    JSValue v = JS_GetPropertyStr(ctx, index, IDB_INDEX_UNPOPULATED);
    bool b;

    DCHECK(JS_IsBool(v), "an index carried no unpopulated flag — idb_index_create gives every index one");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

void idb_index_set_populated(JSContext *ctx, JSValueConst index)
{
    DCHECK(idb_index_is_unpopulated(ctx, index),
           "an index was populated TWICE. §4.5's createIndex places exactly one population request per index "
           "and that request's operation is what clears this flag, so a second clearing means either two "
           "requests were placed for one index or one operation ran twice");
    JS_SetPropertyStr(ctx, (JSValue)index, IDB_INDEX_UNPOPULATED, JS_FALSE);
}

JSValue idb_index_name(JSContext *ctx, JSValueConst index)
{
    JSValue v = JS_GetPropertyStr(ctx, index, IDB_INDEX_NAME);

    DCHECK(JS_IsString(v), "an index carried no NAME — §2.6 gives every index one and only this file writes it");
    return v;
}

JSValue idb_index_key_path(JSContext *ctx, JSValueConst index)
{
    JSValue v = JS_GetPropertyStr(ctx, index, IDB_INDEX_KEY_PATH);

    DCHECK(JS_IsString(v) || JS_IsArray(v), "an index carried no key path — §2.6 gives every index one, and "
                                            "unlike §2.2's store it is never null");
    return v;
}

JSValue idb_index_key_path_value(JSContext *ctx, JSValueConst index)
{
    JSValue path = idb_index_key_path(ctx, index), out;
    uint32_t i, n;

    /* A string key path converts to ITSELF — Web IDL §3.2.9 is the identity on an immutable JS string — and a
       list becomes a PLAIN Array per §3.2.24, for the reason idb_database.h states for §4.5's keyPath. */
    if (!JS_IsArray(path))
        return path;
    n = idb_index_list_len(ctx, path);
    DCHECK(n > 0, "an index's key path is an EMPTY list — §2.5's last bullet is \"a non-empty list\", which "
                  "§4.5's createIndex reports as a SyntaxError before an index is created");
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "IndexedDB: §4.6's keyPath could not allocate the Array Web IDL §3.2.24 makes");
    for (i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, out, i, JS_GetPropertyUint32(ctx, path, i), JS_PROP_C_W_E);
    JS_FreeValue(ctx, path);
    return out;
}

static bool idb_index_flag(JSContext *ctx, JSValueConst index, const char *field)
{
    JSValue v = JS_GetPropertyStr(ctx, index, field);
    bool b;

    DCHECK(JS_IsBool(v), "an index carried no unique or multiEntry flag — §2.6 gives every index both and only "
                         "idb_index_create writes them");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

bool idb_index_unique(JSContext *ctx, JSValueConst index)      { return idb_index_flag(ctx, index, IDB_INDEX_UNIQUE); }
bool idb_index_multi_entry(JSContext *ctx, JSValueConst index) { return idb_index_flag(ctx, index, IDB_INDEX_MULTI); }

JSValue idb_index_store(JSContext *ctx, JSValueConst index)
{
    JSValue store = JS_GetPropertyStr(ctx, index, IDB_INDEX_STORE);

    DCHECK(JS_IsObject(store), "an index carried no REFERENCED OBJECT STORE — §2.6 gives every index one and "
                               "only idb_index_create writes it");
    return store;
}

/* ---- §6.1 step 5's halves ---------------------------------------------------------------------------------- */

bool idb_index_key_is_per_subkey(JSContext *ctx, JSValueConst index, JSValueConst index_key)
{
    return idb_index_multi_entry(ctx, index) && idb_key_is_array(ctx, index_key);
}

bool idb_index_contains_key(JSContext *ctx, JSValueConst index, JSValueConst key)
{
    JSValue records = idb_index_records(ctx, index);
    uint32_t i, n = idb_index_list_len(ctx, records);
    bool found = false;

    /* "index already contains a record with key EQUAL TO key" — about the key alone, so the secondary ordering
       on values plays no part and the search is over keys. The list is sorted, so the first record whose key is
       not less than `key` decides it. */
    for (i = 0; i < n && !found; i++) {
        JSValue rk = idb_irec_field(ctx, records, i, IDB_IREC_KEY);
        int c = idb_key_compare(ctx, rk, key);

        JS_FreeValue(ctx, rk);
        if (c > 0) break;
        found = c == 0;
    }
    JS_FreeValue(ctx, records);
    return found;
}

/* ONE RECORD FILED, and the object it returns is the one recorded for §5.5 step 2 — held and not copied, the
   same decision idb_database.c makes for the records a removal takes out. */
static JSValue idb_index_file_one(JSContext *ctx, JSValueConst records, JSValueConst key, JSValueConst value)
{
    JSValue rec = idl_slots_new(ctx);
    uint32_t pos;
    bool exists;

    CHECK(!JS_IsException(rec), "IndexedDB: §2.6's index record could not be allocated");
    JS_SetPropertyStr(ctx, rec, IDB_IREC_KEY, JS_DupValue(ctx, key));
    JS_SetPropertyStr(ctx, rec, IDB_IREC_VALUE, JS_DupValue(ctx, value));
    pos = idb_irec_pos(ctx, records, key, value, &exists);
    DCHECK(!exists, "an index already holds a record with this exact (key, value) pair. §6.1 step 3 removes "
                    "every index record whose value is the key being written before step 5 adds any, and "
                    "§7.4's multiEntry conversion drops duplicate subkeys, so no pair can be filed twice");
    idb_irec_insert(ctx, records, pos, JS_DupValue(ctx, rec));
    return rec;
}

void idb_index_store_records(JSContext *ctx, JSValueConst tx, JSValueConst index, JSValueConst index_key,
                             JSValueConst primary_key)
{
    JSValue records = idb_index_records(ctx, index), written = JS_NewArray(ctx);
    uint32_t n = 0;

    CHECK(!JS_IsException(written), "IndexedDB: §5.5 step 2's record of the index records written could not be "
                                    "allocated");
    if (!idb_index_key_is_per_subkey(ctx, index, index_key)) {
        /* §6.1 STEP 5.5: one record whose key is the index key. */
        JS_DefinePropertyValueUint32(ctx, written, n++, idb_index_file_one(ctx, records, index_key, primary_key),
                                     JS_PROP_C_W_E);
    } else {
        /* §6.1 STEP 5.6: "for each subkey of the subkeys of index key store a record in index containing subkey
           as its key and key as its value." §6.1's own note is that a subkey which is itself an array key is
           used DIRECTLY — nested array keys are not unpacked — which is why this loop is one level deep and
           does not recurse. The other note is that it is valid for there to be NO subkeys, in which case no
           record is added, which is what an empty list here means rather than a state to refuse. */
        JSValue subkeys = idb_key_subkeys(ctx, index_key);
        uint32_t i, sn = idb_index_list_len(ctx, subkeys);

        for (i = 0; i < sn; i++) {
            JSValue sub = JS_GetPropertyUint32(ctx, subkeys, i);

            DCHECK(JS_IsObject(sub), "an array key's list of subkeys had a hole in it");
            JS_DefinePropertyValueUint32(ctx, written, n++, idb_index_file_one(ctx, records, sub, primary_key),
                                         JS_PROP_C_W_E);
            JS_FreeValue(ctx, sub);
        }
        JS_FreeValue(ctx, subkeys);
    }
    JS_FreeValue(ctx, records);
    /* A write that wrote nothing made no change — the same sentence idb_database.c's removal states, and what
       keeps an empty multiEntry key from filing one empty change per put. */
    if (n == 0) {
        JS_FreeValue(ctx, written);
        return;
    }
    idb_database_record_index_records_stored(ctx, tx, index, written);
}

/* ---- §6.4 step 2 and §6.6 step 2 --------------------------------------------------------------------------- */

/* THE ONE REMOVAL BOTH ARE MADE OF: every record of `index` the caller's test selects leaves, and what left is
   recorded so §5.5 step 2 files it back. `range` is NULL for §6.6, which selects all — stated as the absence of
   a range rather than as an unbounded one this file would have had to mint, which is the same shape §6.6 itself
   has beside §6.4. A single walk, because the records a VALUE range selects are scattered through a list sorted
   by KEY: contiguity is a property of §6.4's store list and not of this one. */
static void idb_index_remove_matching(JSContext *ctx, JSValueConst tx, JSValueConst index,
                                      const JSValueConst *range)
{
    JSValue records = idb_index_records(ctx, index), removed = JS_NewArray(ctx);
    uint32_t i = 0, n = idb_index_list_len(ctx, records), gone = 0;

    CHECK(!JS_IsException(removed), "IndexedDB: §5.5 step 2's record of the removed index records could not be "
                                    "allocated");
    while (i < n) {
        bool take = true;

        if (range != NULL) {
            JSValue value = idb_irec_field(ctx, records, i, IDB_IREC_VALUE);

            take = idb_key_range_contains(ctx, *range, value);
            JS_FreeValue(ctx, value);
        }
        if (!take) { i++; continue; }
        /* THE RECORDS THEMSELVES, HELD AND NOT COPIED — the record object the list holds IS the state, so the
           revert files that same object back. In list order, which is (key, value) order. */
        JS_DefinePropertyValueUint32(ctx, removed, gone++, JS_GetPropertyUint32(ctx, records, i), JS_PROP_C_W_E);
        idb_irec_remove_at(ctx, records, i);
        n--;
    }
    JS_FreeValue(ctx, records);
    if (gone == 0) {
        JS_FreeValue(ctx, removed);
        return;
    }
    idb_database_record_index_records_removed(ctx, tx, index, removed);
}

/* "FOR EACH INDEX WHICH REFERENCES STORE" — the walk both steps begin with, over the store's own set, with
   §2.6's back-reference asserted at each: an index in this set that names another store would mean §6.1 step 5
   had populated it from records this removal is not touching. */
static void idb_index_each(JSContext *ctx, JSValueConst tx, JSValueConst store, const JSValueConst *range)
{
    JSValue set = idb_store_index_set(ctx, store);
    uint32_t i, n = idb_index_list_len(ctx, set);

    for (i = 0; i < n; i++) {
        JSValue index = idb_store_index_at(ctx, set, i), referenced = idb_index_store(ctx, index);

        DCHECK(JS_VALUE_GET_PTR(referenced) == JS_VALUE_GET_PTR(store),
               "an index in an object store's set of indexes does not REFERENCE that store — §2.6 makes the "
               "referenced object store the index's own field, and idb_index_create writes the two together");
        JS_FreeValue(ctx, referenced);
        idb_index_remove_matching(ctx, tx, index, range);
        JS_FreeValue(ctx, index);
    }
    JS_FreeValue(ctx, set);
}

void idb_index_remove_records_in_value_range(JSContext *ctx, JSValueConst tx, JSValueConst store,
                                             JSValueConst range)
{
    idb_index_each(ctx, tx, store, &range);
}

void idb_index_remove_all_records(JSContext *ctx, JSValueConst tx, JSValueConst store)
{
    idb_index_each(ctx, tx, store, NULL);
}

/* ---- §6.3's INDEX RETRIEVAL OPERATIONS --------------------------------------------------------------------- */

/* "Let record be the FIRST record in index's list of records whose key is in range, if any." First in the
   list's own order, which §2.6 makes (key, value) order — so this answers the smallest index key and, among
   records sharing it, the smallest referenced store key. -1 for "record was not found". */
static int idb_index_first_in_range(JSContext *ctx, JSValueConst records, JSValueConst range)
{
    uint32_t n = idb_index_list_len(ctx, records), i;

    for (i = 0; i < n; i++) {
        JSValue key = idb_irec_field(ctx, records, i, IDB_IREC_KEY);
        bool in = idb_key_range_contains(ctx, range, key);

        JS_FreeValue(ctx, key);
        if (in) return (int)i;
    }
    return -1;
}

/* EVERY READ OF AN INDEX RUNS AFTER ITS POPULATION, and the QUEUE is what makes that true rather than anything
   the three readers below could test: §4.5's createIndex places the population request at the line that creates
   the index, and no index handle exists before that line — so every §6.3 or §6.5 request over this index was
   placed later and §5.6 step 5.1 executes them in that order. An unpopulated index reached here would answer
   with an EMPTY list where the store holds matching records, which is a WRONG ANSWER rather than an error. */
static void idb_index_assert_populated(JSContext *ctx, JSValueConst index)
{
    DCHECK(!idb_index_is_unpopulated(ctx, index),
           "§6.3 or §6.5 read an index whose POPULATION REQUEST has not run. §4.5's note makes the index "
           "creation an asynchronous request within the upgrade transaction, and this read was placed against "
           "the same transaction after it — so §5.6 step 5.1's ordering has been broken, and the answer about "
           "to be given is an empty index rather than the store's records");
}

JSValue idb_index_retrieve_referenced_value(JSContext *ctx, JSValueConst index, JSValueConst range)
{
    JSValue records, primary, store, only, out;
    int i;

    idb_index_assert_populated(ctx, index);
    records = idb_index_records(ctx, index);
    i = idb_index_first_in_range(ctx, records, range);
    if (i < 0) {                                   /* "If record was not found, return undefined." */
        JS_FreeValue(ctx, records);
        return JS_UNDEFINED;
    }
    /* "Let serialized be record's REFERENCED VALUE" — §2.6's "the value of the record in the index's referenced
       object store which has a key equal to the index's record's value". That is §6.2's retrieve-a-value over
       the primary key, which is why it is reached through that one algorithm rather than through a second walk
       of the store's list written here. */
    primary = idb_irec_field(ctx, records, (uint32_t)i, IDB_IREC_VALUE);
    JS_FreeValue(ctx, records);
    store = idb_index_store(ctx, index);
    only = idb_key_range_only_key(ctx, primary);
    DCHECK(idb_count_records(ctx, store, only) == 1,
           "an index record's REFERENCED VALUE is not in the referenced object store. §2.6: \"each record in "
           "an index references one and only one record in the index's referenced object store\", and §6.4 "
           "step 2 is what keeps that true when a store record is removed — a missing one means that step did "
           "not run for this index");
    out = idb_retrieve_value(ctx, store, only);
    JS_FreeValue(ctx, only);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, primary);
    return out;
}

JSValue idb_index_retrieve_value(JSContext *ctx, JSValueConst index, JSValueConst range)
{
    JSValue records, value, out;
    int i;

    idb_index_assert_populated(ctx, index);
    records = idb_index_records(ctx, index);
    i = idb_index_first_in_range(ctx, records, range);
    if (i < 0) {
        JS_FreeValue(ctx, records);
        return JS_UNDEFINED;
    }
    /* "Return the result of converting a key to a value with record's VALUE" — §7.3 over the referenced object
       store's key, which is what this retrieval answers with rather than the index key. */
    value = idb_irec_field(ctx, records, (uint32_t)i, IDB_IREC_VALUE);
    JS_FreeValue(ctx, records);
    out = idb_key_to_value(ctx, value);
    JS_FreeValue(ctx, value);
    return out;
}

uint32_t idb_index_count_records(JSContext *ctx, JSValueConst index, JSValueConst range)
{
    JSValue records;
    uint32_t i, n, count = 0;

    idb_index_assert_populated(ctx, index);
    records = idb_index_records(ctx, index);
    n = idb_index_list_len(ctx, records);

    for (i = 0; i < n; i++) {
        JSValue key = idb_irec_field(ctx, records, i, IDB_IREC_KEY);

        if (idb_key_range_contains(ctx, range, key))
            count++;
        JS_FreeValue(ctx, key);
    }
    JS_FreeValue(ctx, records);
    return count;
}

/* ---- §5.5 step 2's INVERSES -------------------------------------------------------------------------------- */

void idb_index_revert_creation(JSContext *ctx, JSValueConst store, JSValueConst index)
{
    /* "Any object stores AND INDEXES which were created during the transaction are now considered deleted for
       the purposes of other algorithms" — the same marking §4.5's deleteIndex performs, because §4.6's members
       answer out of the handle the page still holds. */
    idb_index_set_leave(ctx, store, index);
    JS_SetPropertyStr(ctx, (JSValue)index, IDB_INDEX_DELETED, JS_TRUE);
}

void idb_index_revert_destruction(JSContext *ctx, JSValueConst store, JSValueConst index, const char *name)
{
    JS_SetPropertyStr(ctx, (JSValue)index, IDB_INDEX_DELETED, JS_FALSE);
    idb_index_write_name(ctx, store, index, name);
    idb_index_set_join(ctx, store, index);
}

void idb_index_revert_rename(JSContext *ctx, JSValueConst store, JSValueConst index, const char *name)
{
    idb_index_write_name(ctx, store, index, name);
}

void idb_index_revert_records_stored(JSContext *ctx, JSValueConst index, JSValueConst records)
{
    JSValue live = idb_index_records(ctx, index);
    uint32_t r, rn = idb_index_list_len(ctx, records);

    /* BACKWARDS over what was written, so each removal is of the record whose position the ones still to be
       removed do not depend on — and each is found by the SAME search the write used, so what is left is the
       list §2.6 describes rather than one this arm assembled out of remembered indices. */
    for (r = rn; r > 0; r--) {
        JSValue rec = JS_GetPropertyUint32(ctx, records, r - 1);
        JSValue key = JS_GetPropertyStr(ctx, rec, IDB_IREC_KEY);
        JSValue value = JS_GetPropertyStr(ctx, rec, IDB_IREC_VALUE);
        uint32_t pos;
        bool exists;

        pos = idb_irec_pos(ctx, live, key, value, &exists);
        DCHECK(exists, "an index record this transaction wrote is no longer in the index — §6.1 step 5 is the "
                       "only algorithm that writes this list, and §2.7.2 gives one store to one read/write "
                       "transaction at a time");
        idb_irec_remove_at(ctx, live, pos);
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, rec);
    }
    JS_FreeValue(ctx, live);
}

void idb_index_revert_records_removed(JSContext *ctx, JSValueConst index, JSValueConst records)
{
    JSValue live = idb_index_records(ctx, index);
    uint32_t r, rn = idb_index_list_len(ctx, records);

    for (r = 0; r < rn; r++) {
        JSValue rec = JS_GetPropertyUint32(ctx, records, r);
        JSValue key = JS_GetPropertyStr(ctx, rec, IDB_IREC_KEY);
        JSValue value = JS_GetPropertyStr(ctx, rec, IDB_IREC_VALUE);
        uint32_t pos;
        bool exists;

        pos = idb_irec_pos(ctx, live, key, value, &exists);
        DCHECK(!exists, "an index record this transaction REMOVED is in the index again under its own (key, "
                        "value) pair — the list of changes is run backwards so that every write made after the "
                        "removal has already been undone");
        idb_irec_insert(ctx, live, pos, rec);
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, key);
    }
    JS_FreeValue(ctx, live);
}
