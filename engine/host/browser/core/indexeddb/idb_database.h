/* INDEXED DATABASE §2.1's DATABASE, §2.2's OBJECT STORE, and §6.1/§6.2/§6.4/§6.5/§6.6 over them.
   See idb_database.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_DATABASE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_DATABASE_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

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

/* EVERY CHANGE TO §2.1's AND §2.2's STATE NAMES THE TRANSACTION MAKING IT — the five declarations below and no
   others, because those five are every write this component performs on a database. §2.7's first sentence is
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
 * The record's value is a COPY THIS ALGORITHM MAKES, so §2.3's "later changes to a value have no effect on the
 * record stored in the database" is a fact about this component rather than a precondition on its caller.
 *
 * Returns 0 with *pkey the key the record was filed under (OWNED, a key record — §6.1's "Return key"), or -1
 * with the DOMException the algorithm names live ("ConstraintError"). */
int idb_store_record(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst value, JSValueConst key,
                     bool no_overwrite, JSValue *pkey);

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
 * NEITHER CAN FAIL, so neither has §6.1's -1 arm and both meet §5.6 step 5.4's atomicity by having nothing to
 * report: the key conversion §4.5's member owes is already done, there is no value to clone, and §2.2 states
 * no constraint a removal can violate. What leaves is RECORDED against the transaction like every other change
 * this file makes — holding the record objects themselves, so §5.5 step 2 files those same records back rather
 * than rebuilding them.
 *
 * They are two entries because the standard states two algorithms; the one write they are both made of is in
 * the .c, and it is where §6.4 step 2 and §6.6 step 2 (the index halves) are asserted absent. */
void idb_delete_records(JSContext *ctx, JSValueConst tx, JSValueConst store, JSValueConst range);
void idb_clear_store(JSContext *ctx, JSValueConst tx, JSValueConst store);

/* §6.5's RECORD COUNTING OPERATION — "let count be the number of records, if any, in source's list of records
   with key in range". A READ: it records no change and therefore takes no transaction, which is the same
   reason §6.2's two retrievals do not take one. §6.5 says SOURCE and not store because §2.6's index is one
   too, and an index has a list of records of its own; this engine has no index (§4.5's `createIndex` is not
   installed and no store record carries an index set), so the store is the only source there is. */
uint32_t idb_count_records(JSContext *ctx, JSValueConst store, JSValueConst range);

/* §2.2's THREE FIELDS a store handle reports and §4.5's members branch on. The name is the STORE's, which
   §4.5's `name` getter note distinguishes from the HANDLE's copy of it; the key path is JS_NULL for a store
   with out-of-line keys; the key generator is §4.5's `autoIncrement`. The first two are OWNED. */
JSValue idb_object_store_name(JSContext *ctx, JSValueConst store);
JSValue idb_object_store_key_path(JSContext *ctx, JSValueConst store);
bool    idb_object_store_uses_key_generator(JSContext *ctx, JSValueConst store);

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

/* §5.5 step 2's "any object stores ... WHICH WERE CREATED DURING THE TRANSACTION", asked of one store — which
   is the question §5.8 step 5.1 branches on ("if handle's object store was not newly created during
   transaction"). It is answered out of the SAME list the revert runs, because that list is the record of what
   this transaction did and a second flag on the store would be a second answer to it. Asked only BETWEEN §5.5's
   step 2 and its step 4: the list is emptied when the transaction reaches FINISHED, which this asserts rather
   than reading an empty list as "no". */
bool idb_database_store_was_created_by(JSContext *ctx, JSValueConst tx, JSValueConst store);

#endif
