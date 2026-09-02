/* INDEXED DATABASE §2.1.1's CONNECTION, §4.4's IDBDatabase over it, and §5.2's close. See idb_connection.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_CONNECTION_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_CONNECTION_H

#include <stdbool.h>

#include "quickjs.h"

void idb_connection_init(JSContext *ctx);
void idb_connection_free(JSRuntime *rt);

/* §2.1.1's "the act of opening a database creates a connection", and §5.1 step 8's "let connection be a new
 * connection to db". §5.1 is the ONLY caller, which is what the standard means by "script has indirect access
 * via a connection": there is no other way to obtain one.
 *
 * THE CONNECTION IS THE IDBDatabase OBJECT, not a record something else is a view of — the same identity
 * idb_request.c gives §2.8's request and idb_transaction.c gives §2.7's transaction. §5.1 answers with a
 * connection and §4.3's completion task writes it into `request.result`, where the page reads an IDBDatabase;
 * one object is what makes those two sentences the same sentence, and it is why `request.result === request.
 * result` holds without a cache anywhere.
 *
 * "A connection has a version, which is set when the connection is created", "a close pending flag which is
 * initially false" and "an object store set, which is initialized to the set of object stores in the associated
 * database when the connection is created". OWNED. */
JSValue idb_connection_open(JSContext *ctx, JSValueConst db);

/* The connection's associated §2.1 DATABASE, which §4.4's `name`, §4.10's `db`, §5.1 and §5.7 all reach
   through. OWNED. */
JSValue idb_connection_database(JSContext *ctx, JSValueConst connection);

/* §2.1.1's VERSION — "it remains constant for the lifetime of the connection unless an upgrade is aborted, in
   which case it is set to the previous version of the database". §5.1 step 9 writes it and §5.8 writes it
   back, which is why there is a setter and why §2.1's database version still has none outside §5.7. */
double idb_connection_version(JSContext *ctx, JSValueConst connection);
void   idb_connection_set_version(JSContext *ctx, JSValueConst connection, double version);

/* §2.1.1's OBJECT STORE SET. It is the SAME record as the database's set of object stores — see the file for
   why a copy would be a second answer to one question. OWNED. */
JSValue idb_connection_store_set(JSContext *ctx, JSValueConst connection);

/* §2.1.1's CLOSE PENDING FLAG, which §5.1 step 10.2 skips a connection for and §4.4's `transaction()` and
   `createObjectStore` refuse on. */
bool idb_connection_close_pending(JSContext *ctx, JSValueConst connection);

/* §5.2's "once they are complete, connection is CLOSED" — the state §5.1 step 10.5 and §5.3 step 9 wait for,
   which is close pending AND no transaction created using this connection still live. */
bool idb_connection_is_closed(JSContext *ctx, JSValueConst connection);

/* §5.2's CLOSE A DATABASE CONNECTION, without the forced flag: §4.4's `close()` is its only caller, and the
   forced arm's caller is "a user agent in exceptional circumstances" — loss of the file system, a permission
   change, a storage key cleared — none of which this engine has, so the flag is honestly absent rather than a
   parameter every caller passes false for. */
void idb_connection_close(JSContext *ctx, JSValueConst connection);

/* WHO IS TOLD WHEN A CONNECTION BECOMES CLOSED. §5.1 step 10.5 and §5.3 step 9 — "wait until all connections
   in openConnections are closed", the same sentence in both — are the one wait in this standard that no queue
   can discharge: it is satisfied by the PAGE calling `close()`, or by the last transaction of an
   already-close-pending connection finishing, both of which happen in a later task. So the algorithm that is
   waiting registers here, and the two sites that can close a connection tell it. Registered once, by the
   component that performs both of those algorithms. */
void idb_connection_set_closed_hook(void (*on_closed)(JSContext *ctx, JSValueConst connection));

/* §5.2 step 3's other half: idb_transaction.c reports every transaction that reaches FINISHED, because "wait
   for all transactions created using connection to complete" is a fact about the TRANSACTIONS and the moment
   it becomes true is the moment one of them finishes. */
void idb_connection_transaction_finished(JSContext *ctx, JSValueConst connection);

/* Web IDL §3.8 Platform objects implementing interfaces' "value implements an interface interface", asked of a
   value that arrived from another component. */
bool idb_connection_is(JSValueConst v);

#endif
