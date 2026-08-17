/* INDEXED DATABASE §2.6's INDEX and §6.3's Index retrieval operations. See idb_index.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_INDEX_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* §2.6's SET OF INDEXES on one object store, as the LIST every consumer of it wants: §6.1 step 5 walks it,
   §6.4 step 2 and §6.6 step 2 walk it, §5.8 step 5.2 copies it and §4.5's `indexNames` sorts it. Only the three
   members that name one index look it up by name, and a store has a handful — so this is an Array in creation
   order and NOT a record keyed by name, for the reason idb_transaction.h gives for the handle set: a rename is
   the one thing about an index an upgrade transaction can change, so it cannot be the key. OWNED. */
JSValue  idb_store_index_set(JSContext *ctx, JSValueConst store);
uint32_t idb_store_index_count(JSContext *ctx, JSValueConst store);

/* §6.1 STEP 5's OWN LIST — "each index which references store", MINUS the indexes whose population request has
   not run. A COPY and not the store's record, because that is what the filter makes it; the entries are the
   live indexes, so §6.1's asserts about them are unchanged. OWNED.
   §6.4 step 2 and §6.6 step 2 keep asking for the WHOLE set above: an unpopulated index holds no records, so a
   removal over it is a no-op either way, and filtering there would be a second statement of the same fact. */
JSValue idb_store_populated_index_set(JSContext *ctx, JSValueConst store);

/* §4.5's `createIndex` step 11, "let index be a new index in store", with the four fields step 11 sets. The
   store is the index's REFERENCED OBJECT STORE (§2.6) and the index joins that store's set here. `key_path` is
   CONSUMED; the answer is the §2.6 index record, OWNED. §2.6's "at any one time, the name is unique within
   index's referenced object store" is asserted rather than reported — createIndex's step 6 is the
   "ConstraintError" that makes it true. */
JSValue idb_index_create(JSContext *ctx, JSValueConst tx, JSValueConst store, const char *name,
                         JSValue key_path, bool unique, bool multi_entry);

/* The index named `name` in `store`, or JS_NULL. OWNED. §4.5's `createIndex` step 6, `deleteIndex` step 6 and
   §4.6's name setter step 9 are the callers.
   `idb_index_find_in` IS THE SAME QUESTION ASKED OF A SET, and it exists because §4.5's `index()` asks it of a
   DIFFERENT one: §2.2.1 gives the object store HANDLE an index set of its own, and §5.8 step 5.2 is the whole
   reason the two are not the same list. Conflating them is what that step exists to expose. */
JSValue idb_index_find(JSContext *ctx, JSValueConst store, const char *name);
JSValue idb_index_find_in(JSContext *ctx, JSValueConst set, const char *name);

/* §4.5's `deleteIndex` step 9, "Destroy index", and §4.6's name setter step 10. Both are MARKING and re-keying
   rather than freeing, for the reason idb_object_store_destroy is: a handle the page holds goes on naming the
   index and §4.6's members each ask "has this index been deleted" first. */
void idb_index_destroy(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index);
void idb_index_rename(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index, const char *name);
bool idb_index_is_deleted(JSContext *ctx, JSValueConst index);

/* THE STATE §4.5's NOTE REQUIRES: "although this method does not return an IDBRequest object, the INDEX
   CREATION ITSELF IS PROCESSED AS AN ASYNCHRONOUS REQUEST within the upgrade transaction." §4.5's steps create
   the index synchronously, so an index exists and references the store before that request has walked the
   store's records into it — and while it does, §6.1 step 5 must not see it. That is exactly §4.5's worked
   example (two `put`s queued, then a unique index): with the index visible, the second put fails with a
   ConstraintError, and the standard says the TRANSACTION aborts when the index creation's constraint fails.
   The flag is cleared by the population operation and by nothing else (core/indexeddb/idb_index_populate.h). */
bool idb_index_is_unpopulated(JSContext *ctx, JSValueConst index);
void idb_index_set_populated(JSContext *ctx, JSValueConst index);

/* §2.6's FIELDS. The name and key path are OWNED; `idb_index_key_path_value` is §4.6's keyPath getter's Web IDL
   §3.2.24 conversion, minted per call for the reason idb_database.h states for the store's. */
JSValue idb_index_name(JSContext *ctx, JSValueConst index);
JSValue idb_index_key_path(JSContext *ctx, JSValueConst index);
JSValue idb_index_key_path_value(JSContext *ctx, JSValueConst index);
bool    idb_index_unique(JSContext *ctx, JSValueConst index);
bool    idb_index_multi_entry(JSContext *ctx, JSValueConst index);
JSValue idb_index_store(JSContext *ctx, JSValueConst index);   /* §2.6's referenced object store, OWNED */

/* §6.1 STEPS 5.3-5.6's ONE CONDITION, asked once so the two uniqueness steps and the two write steps cannot
   disagree about it: "if index's multiEntry flag is false, or if index key is not an array key" selects the
   single-record arm, and its negation selects the per-subkey arm. */
bool idb_index_key_is_per_subkey(JSContext *ctx, JSValueConst index, JSValueConst index_key);

/* §6.1 steps 5.3 and 5.4's "index already contains a record with key equal to `key`" — asked of the index key
   itself on the single-record arm, and of each subkey on the other. */
bool idb_index_contains_key(JSContext *ctx, JSValueConst index, JSValueConst key);

/* §6.1 steps 5.5 and 5.6's WRITE: one record per the arm above, each with `index_key` (or a subkey) as its key
   and `primary_key` as its value, filed so the list stays sorted primarily on keys and secondarily on values.
   The transaction is an operand because §5.5 step 2 has to know whose change these records are. */
void idb_index_store_records(JSContext *ctx, JSValueConst tx, JSValueConst index, JSValueConst index_key,
                             JSValueConst primary_key);

/* §6.4 STEP 2 and §6.6 STEP 2, over every index which references `store`: "remove every record from index's
   list of records whose VALUE is in range" and "remove all records". The range is the STORE's — an index
   record's value is a key of the referenced object store — so this is asked with the same range §6.4 step 1
   removed the store's records under, never per removed record. */
void idb_index_remove_records_in_value_range(JSContext *ctx, JSValueConst tx, JSValueConst store,
                                             JSValueConst range);
void idb_index_remove_all_records(JSContext *ctx, JSValueConst tx, JSValueConst store);

/* §6.3's TWO RETRIEVALS. Each answers the FIRST record in the index's list of records whose key is in range,
   which is the smallest index key because §2.6 keeps the list sorted, and JS_UNDEFINED when there is none.
   `retrieve_referenced_value` answers a fresh copy of that record's REFERENCED VALUE — the value of the record
   in the referenced object store whose key equals this record's value — and `retrieve_value` answers §7.3's
   conversion of the record's VALUE, which is that store key. */
JSValue idb_index_retrieve_referenced_value(JSContext *ctx, JSValueConst index, JSValueConst range);
JSValue idb_index_retrieve_value(JSContext *ctx, JSValueConst index, JSValueConst range);

/* §6.5's RECORD COUNTING OPERATION over an INDEX source — "the number of records, if any, in SOURCE's list of
   records with key in range". §6.5 says source and not store precisely because §2.6's index is one too. */
uint32_t idb_index_count_records(JSContext *ctx, JSValueConst index, JSValueConst range);

/* §5.5 step 2's INVERSES — one per change idb_database.c files for this component, applied by that file's
   revert because the ledger is its and the state is this one's (the shape idb_key_generator_revert has). */
void idb_index_revert_creation(JSContext *ctx, JSValueConst store, JSValueConst index);
void idb_index_revert_destruction(JSContext *ctx, JSValueConst store, JSValueConst index, const char *name);
void idb_index_revert_rename(JSContext *ctx, JSValueConst store, JSValueConst index, const char *name);
void idb_index_revert_records_stored(JSContext *ctx, JSValueConst index, JSValueConst records);
void idb_index_revert_records_removed(JSContext *ctx, JSValueConst index, JSValueConst records);

#endif
