/* INDEXED DATABASE §2.1's DATABASE, §2.2's OBJECT STORE, and §6.1/§6.2 over them. See idb_database.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_DATABASE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_DATABASE_H

#include <stdbool.h>

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

/* §2.1.1's CONNECTION — "script does not interact with databases directly; instead, script has indirect access
   via a connection ... it is also the only way to obtain a transaction for that database".
 *
 * IT IS §2.7'S PLUMBING AND THAT IS WHY IT IS HERE RATHER THAN WITH §4.4's IDBDatabase. A transaction is
 * "created through a connection, which is the transaction's connection", so the connection is a SUBPROBLEM of
 * the transaction and not of the interface over it: IDBDatabase is §4.4's page-facing object, and this is the
 * §2.1.1 record it will be a view of. The record is an internal-slot object like §2.1's database and §2.2's
 * store, so it time-travels for free.
 *
 * "The act of opening a database creates a connection" — §5.1's open is the only algorithm that may, and it
 * does not exist; until then the caller is a host that stands in for it. A connection has "a version, which is
 * set when the connection is created", "a close pending flag which is initially false" and "an object store
 * set, which is initialized to the set of object stores in the associated database when the connection is
 * created". OWNED. */
JSValue idb_connection_open(JSContext *ctx, JSValueConst db);
/* The connection's associated database, which §4.10's `db` getter and §5.4's upgrade arm both reach through.
   OWNED. */
JSValue idb_connection_database(JSContext *ctx, JSValueConst connection);

/* §2.2's SET OF OBJECT STORES. A store has a name unique within its database, OPTIONALLY a key path (with one
   it "uses in-line keys", without one "out-of-line keys") and OPTIONALLY a key generator — both are on the
   record because §2.2 puts them there, and both are what §6.1's first step branches on. `key_path` is JS_NULL
   for out-of-line keys, or the key path value (a string, or a list of strings). CONSUMED. */
JSValue idb_object_store_create(JSContext *ctx, JSValueConst db, const char *name, JSValue key_path,
                                bool key_generator);
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
int idb_store_record(JSContext *ctx, JSValueConst store, JSValueConst value, JSValueConst key,
                     bool no_overwrite, JSValue *pkey);

/* §6.2's TWO RETRIEVALS, over `range` — a key range (core/indexeddb/idb_key_range.h). Each answers the FIRST
   record in the store's list of records whose key is in range, which is the first in KEY ORDER because §2.2
   says the list is sorted. JS_UNDEFINED when there is none, which is what both algorithms return.
   `idb_retrieve_value` answers a fresh copy of the record's value (§6.2's StructuredDeserialize); a page that
   mutates what it got has not touched the record. `idb_retrieve_key` answers §7.3's conversion of its key. */
JSValue idb_retrieve_value(JSContext *ctx, JSValueConst store, JSValueConst range);
JSValue idb_retrieve_key(JSContext *ctx, JSValueConst store, JSValueConst range);

#endif
