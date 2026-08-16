/* INDEXED DATABASE §2.7's TRANSACTION and §4.10's IDBTransaction. See idb_transaction.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_TRANSACTION_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_TRANSACTION_H

#include <stdbool.h>

#include "quickjs.h"

/* §2.7's MODE, in the order IDBTransactionMode lists it, and §2.7's DURABILITY HINT in the order
   IDBTransactionDurability lists it. Both are set when the transaction is created and "remain fixed for the
   life of the transaction", which is why neither has a setter. */
enum { IDB_TX_READONLY = 0, IDB_TX_READWRITE, IDB_TX_VERSIONCHANGE };
enum { IDB_DUR_DEFAULT = 0, IDB_DUR_STRICT, IDB_DUR_RELAXED };

/* §2.7.1's STATE — the standard's own four names, in the order its lifetime walks them. A transaction is
   created ACTIVE, goes INACTIVE when control returns to the event loop, is ACTIVE again for the duration of
   each request event dispatch, COMMITTING while it writes, and FINISHED once it has committed or aborted. */
enum { IDB_TX_ACTIVE = 0, IDB_TX_INACTIVE, IDB_TX_COMMITTING, IDB_TX_FINISHED };

void idb_transaction_init(JSContext *ctx);
void idb_transaction_free(JSRuntime *rt);

/* §2.7's "a newly created transaction with this connection, mode, durability and the set of object stores
   named in scope" — §4.9's step 7, reached from C because the member that runs it does not exist yet.
   `scope` is an Array of §2.2 object store records (core/indexeddb/idb_database.h) and is CONSUMED; the
   connection is BORROWED and dup'd onto the transaction. The answer is the IDBTransaction object, OWNED.
   IT IS CREATED ACTIVE and IT IS LIVE from here until its state is finished, which is what makes §2.7.2's
   scheduling constraint answerable — see the file for the one arm of that constraint this engine crashes on
   rather than queueing. */
JSValue idb_transaction_new(JSContext *ctx, JSValueConst connection, JSValue scope, int mode, int durability);

/* §4.9's step 8, "set transaction's cleanup event loop to the current event loop" — declared separately from
   the creation because §2.7 says a transaction only OPTIONALLY has one: §4.9's `transaction()` gives one (so a
   transaction a script created is deactivated once that script's task completes) and §5.7's upgrade
   transaction does NOT, because an upgrade transaction's lifetime is the `upgradeneeded` dispatch and the
   event loop must not deactivate it underneath. A creation that set it unconditionally would silently
   deactivate every upgrade transaction the day §5.7 lands. */
void idb_transaction_set_cleanup_loop(JSContext *ctx, JSValueConst tx);

/* §2.7.1's STATE, read and written. The write is the one every algorithm in §5.4, §5.5, §5.9 and §5.10
   performs, and it asserts the transitions §2.7.1's lifetime allows rather than leaving each caller to. */
int  idb_transaction_state(JSContext *ctx, JSValueConst tx);
void idb_transaction_set_state(JSContext *ctx, JSValueConst tx, int state);

/* §5.4's COMMIT A TRANSACTION and §5.5's ABORT A TRANSACTION. Each sets the state and queues the database
   task that fires the event, so neither runs the page's code and neither is a machine; the TASKS are.
   `error` is CONSUMED and may be JS_NULL — §5.5 is called with null by `abort()`, and §2.7's own note is that
   "the value null is considered an error, as it is set from abort()". */
void idb_transaction_commit(JSContext *ctx, JSValueConst tx);
void idb_transaction_abort(JSContext *ctx, JSValueConst tx, JSValue error);

/* §2.7's REQUEST LIST — "a request list of pending requests which have been made against the transaction",
   with §5.6's two operations over it (add to the end, remove) and §5.9/§5.10's one question about it.
   The removal is the HEAD and asserts that it is: §5.6 step 5.1 makes a request wait until it is the first
   item that is not processed, so requests are removed in the order they were added and a cursor is the whole
   of the list's shrinking end. That is why this is O(1) rather than a shift of a page-sized Array — a shift
   would be work of the page's size inside one stage, which the step contract makes a rest point rather than a
   statement. */
void idb_transaction_add_request(JSContext *ctx, JSValueConst tx, JSValueConst request);
void idb_transaction_remove_request(JSContext *ctx, JSValueConst tx, JSValueConst request);
bool idb_transaction_requests_empty(JSContext *ctx, JSValueConst tx);

/* §2.7's MODE and CONNECTION, read. The mode is what §4.5's members test for a "ReadOnlyError" and what §4.4's
   `createObjectStore` tests to know it is inside an upgrade transaction; the connection is what §4.10's `db`
   answers with and what §4.5's rename reaches the database through. The connection is OWNED. */
int     idb_transaction_mode(JSContext *ctx, JSValueConst tx);
JSValue idb_transaction_connection(JSContext *ctx, JSValueConst tx);

/* §5.7 step 3's "set transaction's scope to connection's object store set", KEPT TRUE as that set changes.
   §2.7's note is that "a transaction's scope remains fixed UNLESS the transaction is an upgrade transaction",
   and §4.10's own note says what a page sees: "subsequent calls to this attribute during an upgrade transaction
   can return lists with different contents as object stores are created and deleted". So §4.4's
   `createObjectStore` adds the store it created and `deleteObjectStore` removes the one it destroyed, and the
   assert is that only an upgrade transaction may. */
void idb_transaction_scope_add(JSContext *ctx, JSValueConst tx, JSValueConst store);
void idb_transaction_scope_remove(JSContext *ctx, JSValueConst tx, JSValueConst store);

/* §2.2.1's "there must be only ONE object store handle associated with a particular object store within a
   transaction", which §4.10's note states as what a page compares: `tx.objectStore('s') ===
   tx.objectStore('s')`. The set is the TRANSACTION's because that is the scope the sentence names. The find
   answers JS_NULL for a name this transaction has no handle for; both are keyed by the name the handle was
   created under, which is the name §4.10 and §4.4 reach the store by. OWNED. */
JSValue idb_transaction_handle_find(JSContext *ctx, JSValueConst tx, const char *name);
void    idb_transaction_handle_add(JSContext *ctx, JSValueConst tx, const char *name, JSValueConst handle);

/* §2.7.3's UPGRADE TRANSACTION and its OPEN REQUEST. "An upgrade transaction is automatically created when
   running the steps to upgrade a database", and §5.4 step 2.5.4 and §5.5 step 7.3 both reach the request FROM
   the transaction — so §5.7 records it here when it creates the transaction. JS_NULL for every other
   transaction, which is what tells the two apart wherever the mode alone would not. `request` is BORROWED. */
void    idb_transaction_set_request(JSContext *ctx, JSValueConst tx, JSValueConst request);
JSValue idb_transaction_request(JSContext *ctx, JSValueConst tx);

/* §5.7 step 10's "WAIT FOR TRANSACTION TO FINISH" — the one wait in §5.1 that neither the task queue nor a
   connection closing can discharge, because what satisfies it is the upgrade transaction reaching FINISHED
   inside §5.4's commit task or §5.5's abort task. §5.1's component registers here, and this file calls it for
   an upgrade transaction and for no other, so the narrowness is what keeps one hook from serving two questions.
   Registered once, by the component that performs §5.1. */
void idb_transaction_set_upgrade_finished_hook(void (*on_finished)(JSContext *ctx, JSValueConst tx));

/* §5.2 step 3's "wait for all transactions created using connection to complete". Asked of this component
   because "live" is §2.7's own word for the set this file keeps, and the answer is what makes a close-pending
   connection CLOSED. */
bool idb_transaction_any_live_for_connection(JSContext *ctx, JSValueConst connection);

/* Is this value an IDBTransaction — Web IDL §3.7.5's brand, asked of a value that arrived from another
   component (a request's `transaction`) rather than from the page. */
bool idb_transaction_is(JSValueConst v);

#endif
