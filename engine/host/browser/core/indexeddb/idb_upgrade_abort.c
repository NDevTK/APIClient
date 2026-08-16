/* INDEXED DATABASE §5.8's ABORT AN UPGRADE TRANSACTION — what a PAGE can still see of a database an abort has
 * just put back.
 *
 * WHY IT IS A COMPONENT AND NOT A BLOCK INSIDE §5.5. Its five steps write three different components' state —
 * §2.1.1's connection version, §2.1.1's object store set, and §2.2.1's name on every handle the transaction
 * ever handed out — and each is reached through the accessor that owns it. So this file holds the ALGORITHM
 * and none of the state, which is also what lets every one of its assertions be about the algorithm: that its
 * caller asked the mode question, that it runs where §5.5 puts it, and that the revert ran first.
 *
 * ITS POSITION IS THE WHOLE OF ITS DESIGN, AND IT IS WHY THERE IS NO "DID THIS DATABASE EXIST BEFORE" FLAG.
 * §5.8 states steps 3 and 4 as a pair of arms — "database's version if database PREVIOUSLY EXISTED, or 0
 * (zero) if database was NEWLY CREATED", and "the set of object stores in database if database previously
 * existed, or the EMPTY SET if database was newly created" — and §5.5 has already run step 2's revert by the
 * time these steps run, which collapses each pair to ONE read:
 *
 *   the version. §2.1: "when a database is first created, its version is 0 (zero)", and the only algorithm
 *   that ever changes it is §5.7 step 8, which records the version it displaced as a change of this very
 *   transaction. So a database THIS open created is back at 0 after the revert, and a database that previously
 *   existed is back at the version it had — and "database's version" is the correct answer in both arms.
 *
 *   the object store set. Every store created during the transaction is a recorded change the revert has just
 *   destroyed, so a newly created database's set is back to the empty one §2.1 creates it holding. And in this
 *   engine the connection's set IS the database's record rather than a copy of it (idb_connection.c states
 *   why), so step 4 is already true and what is left of it is the IDENTITY — asserted here, because writing it
 *   twice would be the second answer to one question that file exists to avoid.
 *
 * A flag remembering which database this was would be a second answer to a question the revert has already
 * answered, and it would be the field nobody notices is never written on the day the revert changes.
 *
 * §2.6's INDEX DOES NOT EXIST, so step 5.2 — "set handle's index set to the set of indexes that reference its
 * object store" — has nothing to write. That is a fact about this engine's state and not a step skipped: a
 * handle has no index set to put back because nothing can create an index to put in one, which is the same
 * sentence §4.4's deleteObjectStore states about its own index-set step. `createIndex` is honestly ABSENT and
 * the IDL gap auditor lists it; the day it lands, this step lands with it. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_object_store.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/indexeddb/idb_upgrade_abort.h"

void idb_abort_upgrade_transaction(JSContext *ctx, JSValueConst tx)
{
    JSValue connection, database, conn_stores, db_stores, handles;
    double version;
    uint32_t i, n;

    DCHECK(idb_transaction_is(tx), "§5.8 was run over something that is not a §2.7 transaction");
    DCHECK(idb_transaction_mode(ctx, tx) == IDB_TX_VERSIONCHANGE,
           "§5.8 was run for a transaction that is NOT an upgrade transaction. §5.5 step 3 asks that question "
           "before it runs these steps, and every sentence below is about state only an upgrade transaction "
           "can have changed — a read/write transaction has no version to put back and no store set to");
    DCHECK(idb_transaction_state(ctx, tx) != IDB_TX_FINISHED,
           "§5.8 was run for a FINISHED transaction. These steps run between §5.5's step 2 and its step 4, and "
           "step 4 is what sets that state — so reaching here afterwards means the transaction's record of "
           "what it changed has already been emptied, and step 5's \"newly created during transaction\" would "
           "answer NO for every store");

    /* Step 1: "Let connection be transaction's connection." Step 2: "Let database be connection's database." */
    connection = idb_transaction_connection(ctx, tx);
    database = idb_connection_database(ctx, connection);

    /* Step 3: "Set connection's version to database's version if database previously existed, or 0 (zero) if
       database was newly created." ONE READ for both arms — see the file header — and §2.1.1's own sentence is
       what it satisfies: the connection's version "remains constant for the lifetime of the connection UNLESS
       AN UPGRADE IS ABORTED, in which case it is set to the previous version of the database". */
    version = idb_database_version(ctx, database);
    DCHECK(version < idb_connection_version(ctx, connection),
           "§5.8 step 3 would not LOWER the connection's version. §5.1 runs the upgrade only when the "
           "database's version is less than the version being opened — step 7 refuses the reverse outright, so "
           "there is no downgrade — and §5.1 step 9 set the connection to the version being opened. A database "
           "that is not below the connection here means §5.5 step 2 did not revert §5.7 step 8's version "
           "before these steps ran, and the number about to be written is the one the upgrade installed");
    idb_connection_set_version(ctx, connection, version);

    /* Step 4: "Set connection's object store set to the set of object stores in database if database
       previously existed, or the empty set if database was newly created." Both arms are the database's own
       set after the revert, and this connection's set IS that record — so the step is already performed and
       what remains is to assert it. */
    conn_stores = idb_connection_store_set(ctx, connection);
    db_stores = idb_database_store_set(ctx, database);
    DCHECK(JS_VALUE_GET_PTR(conn_stores) == JS_VALUE_GET_PTR(db_stores),
           "a connection's object store set is not its database's record. §2.1.1 initialises the set to the "
           "database's when the connection is created and idb_connection.c keeps that identity rather than "
           "copying — which is the whole of how §4.4's createObjectStore writes one set instead of two, and "
           "the whole of why §5.8 step 4 has nothing left to write");
    JS_FreeValue(ctx, db_stores);
    JS_FreeValue(ctx, conn_stores);

    /* Step 5: "For each object store handle handle associated with transaction, INCLUDING THOSE FOR OBJECT
       STORES THAT WERE CREATED OR DELETED DURING TRANSACTION." The set is the transaction's whole list and
       nothing is filtered out of it: a handle for a destroyed store is exactly the case §4.5's note is about
       ("although script cannot access an object store by using the objectStore() method after the transaction
       is aborted, it can still have references to IDBObjectStore instances"). */
    handles = idb_transaction_handles(ctx, tx, &n);
    for (i = 0; i < n; i++) {
        JSValue handle = JS_GetPropertyUint32(ctx, handles, i), store;

        DCHECK(idb_object_store_is(handle), "a transaction's set of object store handles held something that "
                                            "is not an object store handle");
        store = idb_object_store_handle_store(ctx, handle);
        /* Step 5.1: "If handle's object store was NOT NEWLY CREATED during transaction, set handle's name to
           its object store's name." The guard is the step: a store the transaction created has just been
           destroyed by the revert and there is no earlier name for its handle to go back to, so the handle
           keeps the last name it was given — while a store that outlived the transaction has had its own name
           put back by step 2, and this is the write that carries that out to the page's handle. */
        if (!idb_database_store_was_created_by(ctx, tx, store))
            idb_object_store_handle_restore_name(ctx, handle);
        /* Step 5.2 is the index set, which §2.6's index does not exist to fill — see the file header. */
        JS_FreeValue(ctx, store);
        JS_FreeValue(ctx, handle);
    }
    JS_FreeValue(ctx, handles);

    JS_FreeValue(ctx, database);
    JS_FreeValue(ctx, connection);
}
