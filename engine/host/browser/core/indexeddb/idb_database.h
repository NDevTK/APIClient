/* INDEXED DATABASE §2.1's DATABASE, §2.2's OBJECT STORE, and §6.1/§6.2/§6.4/§6.5/§6.6 over them.
   See idb_database.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_DATABASE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_DATABASE_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/indexeddb/idb_key_array.h"
#include "core/indexeddb/idb_key_path.h"

/* Declared once per AGENT: §2.1's "each storage key has an associated set of databases", built EMPTY at the
   pre-boot COW baseline. It is agent state and not realm state for the reason core/file/file_system.c's two
   roots are: §Security makes an instance an origin-keyed agent cluster and storage is keyed by origin, so a
   same-origin child navigable reaches THIS set of databases and not a second one. The release takes the
   runtime, which is what core/platform.h's third column is. */
void idb_database_init(JSContext *ctx);
void idb_database_free(JSRuntime *rt);

/* §2.1's SET OF DATABASES, asked and added to. `idb_database_find` answers JS_NULL for a name this storage key
   has no database for; `idb_database_create` makes one whose version is 0 ("When a database is first created,
   its version is 0") holding no object stores ("When a new database is created it doesn't contain any object
   stores"). Both answers are OWNED. Creating one that already exists is a should-never-happen: §5.1's open runs
   the find first, and two databases under one name in one storage key is not a state §2.1 has. */
JSValue idb_database_find(JSContext *ctx, const char *name);
JSValue idb_database_create(JSContext *ctx, const char *name);

/* §5.3 step 11's "DELETE db" — the database LEAVES §2.1's set, so a later open of that name reaches step 6 and
   creates a fresh one at version 0. It is a removal and not a teardown because §5.3 step 9 has already waited
   until every connection associated with it is closed, and a closed connection has left the set §2.1.1 keeps;
   both of those are asserted here, where the record is dropped, rather than trusted from the caller. */
void idb_database_destroy(JSContext *ctx, JSValueConst db);

/* §2.1's SET, ENUMERATED — "let databases be the set of databases in storageKey", which is §4.3's `databases()`
   and nothing else. An OWNED Array of the database records in the set's own order; the caller reads each one's
   name and version through the two accessors below. §4.3 step 4.3.1 is the caller's ("if db's version is 0,
   then continue"), not this one's: the set genuinely holds a version-0 database whenever §5.1 step 6 created
   one for an open that step 7 then refused, and that is the state the clause exists for. */
JSValue idb_database_list(JSContext *ctx);

/* §2.1's VERSION. There is no setter, and its absence is the standard's own sentence rather than a gap: "The
   only way to change the version is using an upgrade transaction", and there is no transaction yet — so the
   version a database has is the 0 it was created with, and the member that changes it arrives with §5.1's
   upgrade-a-database, which is the only algorithm allowed to. A setter standing here now would be an export
   with no caller: code nothing exercises, in a component whose whole claim is that it is exercised. */
double idb_database_version(JSContext *ctx, JSValueConst db);

/* §2.1's NAME, which §4.4's `name` getter answers with — "the name attribute returns this name even if this's
   close pending flag is true", so it is the DATABASE's and not the connection's. OWNED. */
JSValue idb_database_name(JSContext *ctx, JSValueConst db);

/* §2.1's VERSION, WRITTEN. §5.7 step 8 is the only caller and the standard says so — "the only way to change
   the version is using an upgrade transaction" — which is why this arrived with §5.7 and not before it. The
   sentence continues "this change is considered part of the transaction, and so if the transaction is aborted,
   this change is reverted", which is why the transaction is an operand: the version the database held is
   recorded on it, and §5.5 step 2 writes that back. */
void idb_database_set_version(JSContext *ctx, JSValueConst tx, JSValueConst db, double version);

/* §2.1's "a database has at most one associated UPGRADE TRANSACTION, which is either null or an upgrade
   transaction, and is initially null". It is what §4.4's `createObjectStore` and `deleteObjectStore` read to
   decide their "InvalidStateError", what §4.4's `transaction()` refuses on, and what §5.4 step 2.5.1 and §5.5
   step 7.1 set back to null. The read is OWNED and answers JS_NULL when there is none; the write CONSUMES. */
JSValue idb_database_upgrade_transaction(JSContext *ctx, JSValueConst db);
void    idb_database_set_upgrade_transaction(JSContext *ctx, JSValueConst db, JSValue tx);

/* §2.1.1's "there may be multiple CONNECTIONS to a given database at any given time", as the set §5.1 step
   10.1 asks for ("the set of all connections, except connection, associated with db") and §5.3 asks for again.
   A connection joins when it is created and leaves when it is CLOSED — §5.2's "once they are complete,
   connection is closed" — which is exactly the pair of moments §5.1 step 10.6 waits between. The read is an
   OWNED Array in join order. */
void    idb_database_add_connection(JSContext *ctx, JSValueConst db, JSValueConst connection);
void    idb_database_remove_connection(JSContext *ctx, JSValueConst db, JSValueConst connection);
JSValue idb_database_connections(JSContext *ctx, JSValueConst db);

/* §2.1's SET OF OBJECT STORES, as the record §2.1.1's connection is initialised to — the SAME object, never a
   copy, for the reason idb_connection.c states. OWNED. */
JSValue idb_database_store_set(JSContext *ctx, JSValueConst db);

/* EVERY CHANGE TO §2.1's AND §2.2's STATE NAMES THE TRANSACTION MAKING IT — the six declarations below and no
   others, because those six are every write this component performs on a database. §2.7's first sentence is
   why ("whenever data is read or written to the database it is done by using a transaction"), and §5.5 step 2
   is what needs it: the transaction records the INVERSE of each change as it is made, so an abort has something
   to write back. A mutation that took no transaction would be a change nothing could undo, and the abort would
   be silently partial rather than crashing — which is the whole reason this is a parameter and not a lookup.
   Each asserts what §2.7 says its transaction may do (a read-only transaction may change nothing; only an
   upgrade transaction may add, remove or rename a store, or change the version). */

/* §2.2's SET OF OBJECT STORES. A store has a name unique within its database, OPTIONALLY a key path (with one
   it "uses in-line keys", without one "out-of-line keys") and OPTIONALLY a key generator — both are on the
   record because §2.2 puts them there, and both are what §6.1's first step branches on. `key_path` is JS_NULL
   for out-of-line keys, or the key path value (a string, or a list of strings). CONSUMED. */
JSValue idb_object_store_create(JSContext *ctx, JSValueConst tx, JSValueConst db, const char *name,
                                JSValue key_path, bool key_generator);
JSValue idb_object_store_find(JSContext *ctx, JSValueConst db, const char *name);   /* or JS_NULL. OWNED. */

/* §6.1's STORE A RECORD INTO AN OBJECT STORE, with store, value, an optional key, and a no-overwrite flag.
 *
 * `key` is a KEY RECORD (core/indexeddb/idb_key.h) — the caller has already run §7.4 on whatever the page
 * passed, because §4.5's `put` reports that conversion's "DataError" before it reaches this algorithm. It may
 * be JS_UNDEFINED only for a store with a key generator, which is §6.1's own first step.
 *
 * THAT FIRST STEP MUTATES `value`, and only there: a store with a key generator AND in-line keys has the
 * generated key INJECTED into the value at its key path (§6.1 step 1.1.3, §7.2), which is how a record put with
 * no key comes back out of §6.2 carrying one. The value is §4.5 step 10's clone, so the write is on a value no
 * page holds — which is also the precondition §7.2 asserts rather than tests.
 *
 * The record's value is a COPY THIS ALGORITHM MAKES, so §2.3's "later changes to a value have no effect on the
 * record stored in the database" is a fact about this component rather than a precondition on its caller.
 *
 * IT IS A DELEGATABLE ALGORITHM AND NOT A CALL, and §6.1 STEP 5 IS WHY. That step runs §7.1's extract-a-key
 * once per index which references the store, whose step 3 is §7.4 — and an index whose key path resolves to an
 * Array reaches §7.4's ARRAY ARM, which is exactly what §2.6's multiEntry flag exists for. There is one
 * implementation of that arm and it is the parkable walk (core/indexeddb/idb_key_array.h), so §6.1 has that
 * algorithm's rest points inside its own and the caller drives them: §4.5's add-or-put OPERATION declares this
 * block in its stage list, embeds an IdbStoreWalk, chains idb_store_record_walk_visit into its `visit` and
 * hands `run` the base of the block. There is no C entry, because the only caller is that operation and a
 * second, non-suspending copy of these steps is the thing this shape exists to prevent.
 *
 * §7.1's stage block leads the list so the walk's base IS this block's base; the six numbered steps follow. */
#define IDB_STORE_RECORD_ALGO_STAGES(X, P, W) \
    IDB_KEY_PATH_EXTRACT_ALGO_STAGES(X, P, W " → Indexed Database §6.1 step 5.1 (let index key be the result " \
                                             "of extracting a key from a value using a key path with value, " \
                                             "index's key path, and index's multiEntry flag)") \
    X(P##_GENERATE, W " → Indexed Database §6.1 step 1 (if store uses a key generator: generate a key for " \
                      "store and inject it into value, or possibly update the key generator with the key " \
                      "that was given)") \
    X(P##_NO_OVERWRITE, W " → Indexed Database §6.1 step 2 (the no-overwrite flag is true and a record " \
                          "already exists in store with its key equal to key: a ConstraintError)") \
    X(P##_REMOVE, W " → Indexed Database §6.1 step 3 (a record already exists in store with its key equal to " \
                    "key: remove it using delete records from an object store)") \
    X(P##_STORE, W " → Indexed Database §6.1 step 4 (store a record in store containing key as its key and " \
                   "the serialization of value as its value)") \
    X(P##_INDEX, W " → Indexed Database §6.1 step 5 (the next index which references store, or step 6 when " \
                   "there is none; step 5.1's extraction begins)") \
    X(P##_INDEX_TOOK, W " → Indexed Database §6.1 step 5.2 (an index key that is an exception, or invalid, or " \
                        "failure: take no further actions for this index and continue with the next)") \
    X(P##_INDEX_UNIQUE, W " → Indexed Database §6.1 steps 5.3-5.4 — ONE CONDITION the standard states in its " \
                          "two arms: index's unique flag is true and index already contains a record with a " \
                          "key equal to the index key (or to any of its subkeys), which is a ConstraintError") \
    X(P##_INDEX_WRITE, W " → Indexed Database §6.1 steps 5.5-5.6 — THE SAME CONDITION's two arms: one record " \
                         "whose key is the index key, or one record per subkey of it, each with key as its " \
                         "value")

/* §6.1 IN FLIGHT. `w` and `kpres` are §7.1's (its extra state is an enum, which is why the caller of THAT
   algorithm holds it rather than a record); everything else is §6.1's own — its four operands, the key
   step 1.1.1 generated, step 5's list of indexes and the cursor into it, and the index key step 5.1 answered. */
typedef struct IdbStoreWalk {
    IdbKeyWalk       w;
    IdbKeyPathResult kpres;
    JSValue  tx, store, value, key;   /* §6.1's operands; `key` is also its answer (owned) */
    JSValue  generated;               /* step 1.1.1's key, held until step 1.1.3 has injected it (owned) */
    JSValue  indexes;                 /* step 5's "each index which references store", as it stood (owned) */
    JSValue  index_key;               /* step 5.1's answer, between steps 5.2 and 5.6 (owned) */
    uint32_t idx;                     /* the cursor into `indexes` */
    uint8_t  no_overwrite;
    /* WHETHER STEP 1 RAISED §2.11's CURRENT NUMBER, which step 2 asserts against the key it found. It spans
       two stages now that §6.1 is one, so it is state rather than a C local. */
    uint8_t  generator_raised;
    int      after;                   /* the CALLER's stage this algorithm hands control back at */
} IdbStoreWalk;

/* BEGIN §6.1. Every operand is BORROWED and dup'd onto the record, because this algorithm is a work item and
   §scheduler's rule is that a work item takes its inputs with it. The caller returns JS_STEP_YIELD. */
void idb_store_record_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbStoreWalk *sw, JSValueConst tx,
                                 JSValueConst store, JSValueConst value, JSValueConst key, bool no_overwrite,
                                 int base, int after);

/* §6.1 STEP 5 ALONE, over ONE index and one record the store ALREADY HOLDS — the same block entered at step 5
 * instead of at step 1, which is what §4.5's population operation performs per record
 * (core/indexeddb/idb_index_populate.h). Steps 1-4 are skipped because there is nothing to generate, nothing to
 * refuse and nothing to write: the record is in the list, and what is missing is its entry in this one index.
 *
 * IT IS THE SAME IMPLEMENTATION AND NOT A SECOND ONE, which is the whole reason this is an entry point rather
 * than a walk in that file: step 5.1's extraction is §7.1 and therefore §7.4's array arm, step 5.2's three
 * discard arms, steps 5.3-5.4's unique refusal and steps 5.5-5.6's two write arms are exactly the sub-steps a
 * `put` runs, and a copy of them would be a second answer to §2.6's multiEntry flag. The abrupt it produces is
 * the same "ConstraintError", which is what §4.5's note turns into an aborted transaction.
 *
 * `value` is the record's STORED value, read in place: step 5 only EXTRACTS from it (step 1's inject is the
 * one step that writes, and this entry does not run it). Every operand is BORROWED. */
void idb_store_record_walk_start_index(JSContext *ctx, JSStepHdr *hdr, IdbStoreWalk *sw, JSValueConst tx,
                                       JSValueConst store, JSValueConst index, JSValueConst value,
                                       JSValueConst key, int base, int after);

/* ONE STAGE of it. Returns a step code the caller must return, or JS_STEP_ABRUPT with the DOMException §6.1
   names live ("ConstraintError") — the only abrupt this algorithm produces, because step 5.2 catches §7.1's.
   `in` is CONSUMED. */
int  idb_store_record_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbStoreWalk *sw, JSValue in, int base,
                               JSValue **out_cb, int *out_argc);

void idb_store_record_walk_visit(JSContext *ctx, IdbStoreWalk *sw, JSStepVisit *v);

/* §6.1's LAST STEP, "Return key", at the caller's own stage. OWNED — a key record, which §7.3 converts on the
   way to `request.result`. */
JSValue idb_store_record_walk_take(JSContext *ctx, IdbStoreWalk *sw);

/* §6.2's TWO RETRIEVALS, over `range` — a key range (core/indexeddb/idb_key_range.h). Each answers the FIRST
   record in the store's list of records whose key is in range, which is the first in KEY ORDER because §2.2
   says the list is sorted. JS_UNDEFINED when there is none, which is what both algorithms return.
   `idb_retrieve_value` answers a fresh copy of the record's value (§6.2's StructuredDeserialize); a page that
   mutates what it got has not touched the record. `idb_retrieve_key` answers §7.3's conversion of its key. */
JSValue idb_retrieve_value(JSContext *ctx, JSValueConst store, JSValueConst range);
JSValue idb_retrieve_key(JSContext *ctx, JSValueConst store, JSValueConst range);

/* §6.4's DELETE RECORDS FROM AN OBJECT STORE and §6.6's OBJECT STORE CLEAR OPERATION — the two REMOVALS, which
 * differ only in which records leave: §6.4 takes a range ("remove all records, if any, from store's list of
 * records with key in range") and §6.6 takes none at all ("remove all records from store"). Each returns
 * undefined, which §5.6 makes the request's result.
 *
 * NEITHER CAN FAIL, so neither has an error arm: the key conversion §4.5's member owes is already done, there
 * is no value to clone, and §2.2 states no constraint a removal can violate. What leaves is RECORDED against
 * the transaction like every other change this file makes — holding the record objects themselves, so §5.5
 * step 2 files those same records back rather than rebuilding them.
 *
 * EACH ALSO PERFORMS ITS INDEX HALF: §6.4 step 2 removes "every record from index's list of records whose
 * VALUE is in range" — the STORE's range, once per index, never once per removed record — and §6.6 step 2
 * removes all of them. Both are core/indexeddb/idb_index.h's, beside the list they write.
 *
 * They are two entries because the standard states two algorithms; the one write they are both made of is in
 * the .c. */
void idb_delete_records(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst range);
void idb_clear_store(JSContext *ctx, JSValueConst tx, JSValueConst store);

/* §6.5's RECORD COUNTING OPERATION over an OBJECT STORE source — "let count be the number of records, if any,
   in source's list of records with key in range". A READ: it records no change and therefore takes no
   transaction, which is the same reason §6.2's two retrievals do not take one. §6.5 says SOURCE and not store
   because §2.6's index is one too; the index's half of the same algorithm is idb_index_count_records, beside
   the list it counts. */
uint32_t idb_count_records(JSContext *ctx, JSValueConst store, JSValueConst range);

/* HOW MANY RECORDS A STORE HOLDS AT ALL — not §6.5, which is stated over a range — and the RECORD at one
   position of §2.2's list, which §2.2 keeps in §2.4's key order. The two are what §4.5's population operation
   walks: it runs §6.1 step 5 for its one index over every record the store already holds, one record per
   stage. Positional rather than a handed-out list, because the list is this component's own record and a
   caller holding it would be a second writer of §2.2's invariants. Both answers are OWNED. */
uint32_t idb_store_record_count(JSContext *ctx, JSValueConst store);
void     idb_store_record_at(JSContext *ctx, JSValueConst store, uint32_t i, JSValue *key, JSValue *value);

/* §2.2's THREE FIELDS a store handle reports and §4.5's members branch on. The name is the STORE's, which
   §4.5's `name` getter note distinguishes from the HANDLE's copy of it; the key path is JS_NULL for a store
   with out-of-line keys; the key generator is §4.5's `autoIncrement`. The first two are OWNED. */
JSValue idb_object_store_name(JSContext *ctx, JSValueConst store);
JSValue idb_object_store_key_path(JSContext *ctx, JSValueConst store);
bool    idb_object_store_uses_key_generator(JSContext *ctx, JSValueConst store);

/* §2.6's SET OF INDEXES that reference this store, as the field it is. The SET is
   core/indexeddb/idb_index.h's — every write to it and every question about it is that component's — and this
   is only where the store record keeps it, exported so that component can reach it without a second spelling
   of the field name. OWNED. */
JSValue idb_object_store_indexes(JSContext *ctx, JSValueConst store);

/* §2.11's KEY GENERATOR ITSELF, which is the record §2.11's two algorithms are stated over
   (core/indexeddb/idb_key_generator.h). §2.2 makes it OPTIONAL and the question above is how that is asked, so
   this asserts the store has one rather than answering null for a store that does not — a caller holding "the
   generator of a store with no generator" has nothing it could correctly do with it. OWNED. */
JSValue idb_object_store_key_generator(JSContext *ctx, JSValueConst store);

/* §2.11's CHANGE, recorded like the five above and by the algorithm that makes it. "Modifying a key generator's
   current number is considered part of a database operation ... likewise, if a transaction is aborted, the
   current number of the key generator for each object store in the transaction's scope is reverted to the value
   it had before the transaction was started."
   THE MUTATION IS §2.11's AND THE RECORD IS THIS COMPONENT'S, which is why this is the one write of the six that
   is exported: §5.5 step 2's list is this file's, and a second place that appended to it would be a second
   vocabulary of changes for one revert to understand. `prior` is the current number the generator held BEFORE
   the write about to be made, and it is the caller's obligation to record it first. */
void idb_object_store_record_generator_change(JSContext *ctx, JSValueConst tx, JSValueConst store, int64_t prior);

/* §2.6's FIVE CHANGES, filed here for the same reason §2.11's one is: §5.5 step 2's list is this file's, and a
   second place that appended to it would be a second vocabulary of changes for one revert to understand. The
   STATE is core/indexeddb/idb_index.h's, so the revert's five arms call that component's five inverses — the
   mutation is its and the ledger is this one's. Each asserts what §2.7 lets its transaction do: only an upgrade
   transaction may add, remove or rename an index, and a read-only transaction may write no record at all.
   `name` and `records` are CONSUMED; everything else is BORROWED. */
void idb_database_record_index_created(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index);
void idb_database_record_index_destroyed(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index,
                                         JSValue name);
void idb_database_record_index_renamed(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst index,
                                       JSValue name);
void idb_database_record_index_records_stored(JSContext *ctx, JSValueConst tx, JSValueConst index,
                                              JSValue records);
void idb_database_record_index_records_removed(JSContext *ctx, JSValueConst tx, JSValueConst index,
                                               JSValue records);

/* §4.5's `keyPath` GETTER'S CONVERSION: "return this's object store's key path, or null if none. The key path
 * is converted as a DOMString (if a string) or a sequence<DOMString> (if a list of strings), per [WEBIDL]."
 *
 * IT IS A COPY AND THAT IS THE POINT — the standard's own note is "the returned value is not the same instance
 * that was used when the object store was created ... changing the properties of the object has no effect on
 * the object store", and the record above is state a page must not be handed a reference to. Web IDL §3.2.24
 * is the conversion and it is exactly what a list gets here: "let A be a new Array object created as if by the
 * expression []", then CreateDataPropertyOrThrow for each entry. A PLAIN Array — extensible, writable,
 * configurable. It is NOT a frozen array: `FrozenArray<T>` is a different parameterized type (§3.2.27) that
 * neither this attribute's declaration (`readonly attribute any keyPath`) nor the prose above names, and
 * freezing what the spec says is a sequence would be a property no page can otherwise observe on an Array.
 * The identity the note DOES require ("it returns the same object instance every time it is inspected") is the
 * HANDLE's, not the store's — WPT's idbobjectstore_keyPath asserts both halves, that one handle answers with
 * one Array and that two handles for one store answer with two — so the cache belongs to §2.2.1's handle and
 * this entry mints. OWNED. */
JSValue idb_object_store_key_path_value(JSContext *ctx, JSValueConst store);

/* §4.4's `deleteObjectStore` last step, "Destroy store", and the question every member of §4.5 asks first
   ("if store has been DELETED, throw an InvalidStateError"). A destroyed store leaves its database's set and
   is MARKED, because a handle the page already holds goes on naming it — §4.5's own note is that "although
   script cannot access an object store by using the objectStore() method after the transaction is aborted, it
   can still have references to IDBObjectStore instances", and those instances must report the state rather
   than answer out of a record nothing points at. */
void idb_object_store_destroy(JSContext *ctx, JSValueConst tx, JSValueConst db, JSValueConst store);
bool idb_object_store_is_deleted(JSContext *ctx, JSValueConst store);

/* §4.5's `name` SETTER, whose whole content is a RE-KEY: "set store's name to name" moves the store within
   §2.2's set, which is keyed by the name that identifies it there. The ConstraintError for a name the database
   already holds is the member's, reported before this runs. */
void idb_object_store_rename(JSContext *ctx, JSValueConst tx, JSValueConst db, JSValueConst store,
                             const char *name);

/* §5.5 step 2: "ALL THE CHANGES MADE TO THE DATABASE BY THE TRANSACTION ARE REVERTED. For upgrade transactions
   this includes changes to the set of object stores and indexes, as well as the change to the version. Any
   object stores and indexes which were created during the transaction are now considered deleted for the
   purposes of other algorithms."
   It is asked of this component and not of the transaction because the state being put back is this one's, and
   because the inverse of a write is a write — so it is made where the write is made, through the same two
   functions §2.2's set of object stores is changed by anywhere else. A read-only transaction has recorded
   nothing and this does nothing, which is a fact about what §2.7 lets that mode do rather than a mode test.
   See the file for why this is NOT a span of the flow's COW delta. */
void idb_database_revert_transaction(JSContext *ctx, JSValueConst tx);

/* §5.6 STEP 5.4: "If result is an error, then REVERT ALL CHANGES MADE BY OPERATION", with the standard's own
 * note beside it — "this only reverts the changes done by this request, not any other changes made by the
 * transaction".
 *
 * IT BECAME REQUIRED WITH §6.1 STEP 5, and idb_request.h used to state the opposite: that §6.1 "satisfies
 * atomicity structurally, reporting every failure before it copies the value". That was TRUE while step 2 was
 * §6.1's last refusal and is FALSE now — step 5's "ConstraintError" for a unique index happens AFTER step 3 has
 * removed the displaced record, step 4 has written the new one and earlier indexes have taken their records, so
 * an operation that fails has already changed the database. It is also §2.11's per-operation sentence ("if the
 * operation fails and the operation is reverted, the current number is reverted to the value it had before the
 * operation started"), which is why a key-generator change is one of the kinds this undoes.
 *
 * `from` is the length of the transaction's list of changes taken BEFORE the operation ran
 * (idb_transaction_change_count), so what this reverts is exactly the operation's own tail — backwards, the
 * order §5.5 step 2 uses and for the same reason. The reverted changes then LEAVE the list, because they are no
 * longer changes the transaction made and an abort must not undo them twice; §5.5 step 2's whole-transaction
 * revert does NOT truncate, because §5.8 step 5.1 asks that same list which stores this transaction created and
 * the list is emptied only when the transaction reaches FINISHED. */
void idb_database_revert_operation(JSContext *ctx, JSValueConst tx, uint32_t from);

/* §5.5 step 2's "any object stores ... WHICH WERE CREATED DURING THE TRANSACTION", asked of one store — which
   is the question §5.8 step 5.1 branches on ("if handle's object store was not newly created during
   transaction"). It is answered out of the SAME list the revert runs, because that list is the record of what
   this transaction did and a second flag on the store would be a second answer to it. Asked only BETWEEN §5.5's
   step 2 and its step 4: the list is emptied when the transaction reaches FINISHED, which this asserts rather
   than reading an empty list as "no". */
bool idb_database_store_was_created_by(JSContext *ctx, JSValueConst tx, JSValueConst store);

/* THE SAME QUESTION ABOUT AN INDEX — §5.8 step 6's "if handle's index was NOT NEWLY CREATED during
   transaction, set handle's name to its index's name". Answered out of the same list and with the same
   restriction on when it may be asked. */
bool idb_database_index_was_created_by(JSContext *ctx, JSValueConst tx, JSValueConst index);

#endif
