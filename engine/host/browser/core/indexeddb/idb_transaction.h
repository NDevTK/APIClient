/* INDEXED DATABASE §2.7's TRANSACTION and §4.10's IDBTransaction. See idb_transaction.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_TRANSACTION_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_TRANSACTION_H

#include <stdbool.h>
#include <stdint.h>

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
   IT IS CREATED ACTIVE and IT IS LIVE from here until its state is finished, which is what makes §2.7.2
   Transaction scheduling's constraints answerable. The creation also performs §2.7.1 Transaction lifecycle's
   "queue a database task to start the transaction asynchronously" when §2.7.2 permits it — for every mode
   except "versionchange", whose start is §5.7 Upgrading a database's step 6, the line below. */
JSValue idb_transaction_new(JSContext *ctx, JSValueConst connection, JSValue scope, int mode, int durability);

/* §2.7.1 TRANSACTION LIFECYCLE's START, and §2.7 Transactions' "until the transaction is started the
 * implementation must not execute these requests; however, the implementation must keep track of the requests
 * and their order."
 *
 * THE HOLD IS WHY THERE IS NO SECOND QUEUE AND NO GATE ASKED PER OPERATION. §5.6 Asynchronously executing a
 * request mints its task the moment the request is placed — that is what carries its operands with it — but a
 * transaction that has not started is not allowed to EXECUTE it, so the task is held on the transaction and
 * released, in placement order, into the ONE task queue when the start runs. Nothing polls: §2.7.2's
 * constraints are stated over the earlier transactions that "are not finished", so the only event that can
 * change the answer is a transaction LEAVING the live set, and the one line that performs that removal asks
 * again for every transaction still waiting. A gate the request task asked before performing its operation
 * would instead have to re-queue itself, which is a poll wearing the queue's clothes and would spin for as long
 * as the page holds the earlier transaction open.
 *
 * `idb_transaction_start` is §5.7 step 6's line, exported for the upgrade transaction alone — every other
 * transaction reaches it through the database task the creation queued. It asserts §2.7.2 rather than
 * believing it, which is where §5.1 Opening a database connection step 10's wait for every connection to close
 * gets checked. `idb_transaction_queue_request_task` is the one door §5.6's task goes through, so there is no
 * call site left that could queue one without asking; `task` is BORROWED. */
void idb_transaction_start(JSContext *ctx, JSValueConst tx);
bool idb_transaction_started(JSContext *ctx, JSValueConst tx);
void idb_transaction_queue_request_task(JSContext *ctx, JSValueConst tx, JSValueConst task);

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

/* §2.7's REQUEST LIST, WALKED — the entries from the head cursor to the end, which are exactly the requests
   that have not been removed yet. §4.5's `createIndex` is the caller and its question is not "is the list
   empty" but "is any request still to run placed against THIS store", which is a different and much narrower
   thing: a pending `put` into another object store cannot be observed by an index on this one. `*from` is the
   head cursor and `*count` the list's length, so the caller walks [from, count) without a second reader of
   either. The list is OWNED. */
JSValue idb_transaction_pending_requests(JSContext *ctx, JSValueConst tx, uint32_t *from, uint32_t *count);

/* §5.5 step 2's "ALL THE CHANGES MADE TO THE DATABASE BY THE TRANSACTION" — the list of them, in the order they
   were made. It is the TRANSACTION's state because that is whose changes they are, and it is a JS Array on the
   transaction's slot record for the reason every other list in this component is one (§State-isolation): a
   malloc'd C list captured as a head pointer reverts the POINTER on a context switch and leaves its nodes
   reachable from nothing, and this list has to fork with the flow, park to the cold tier and come back. Forking
   with the flow is not incidental here — it is what makes an abort correct in one arm of a fork while its
   sibling commits, because each arm appends to its own copy and reverts only what it wrote.
   The record of one change is core/indexeddb/idb_database.c's, because the inverse of a write is a write and
   only the component that owns the state can make it. This side only files them and hands them back in order.
   `change` is CONSUMED; the list is OWNED and is read backwards by §5.5 step 2. */
void    idb_transaction_record_change(JSContext *ctx, JSValueConst tx, JSValue change);
JSValue idb_transaction_changes(JSContext *ctx, JSValueConst tx);

/* HOW MANY CHANGES THE TRANSACTION HAS RECORDED SO FAR — §5.6 step 5.4's WATERMARK, read before an operation
   is performed so that "revert all changes made by OPERATION" names the tail the operation appended and not
   the transaction's whole list. It is a count and not a cursor object because the list only grows while an
   operation runs: §2.7.2 gives one store to one read/write transaction at a time, and an operation is one
   task, so nothing else appends to or removes from this list across one. */
uint32_t idb_transaction_change_count(JSContext *ctx, JSValueConst tx);

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
   tx.objectStore('s')`. The set is the TRANSACTION's because that is the scope the sentence names.
   IT IS KEYED BY THE STORE, WHICH IS THE ONLY KEY THAT SENTENCE ADMITS. A set keyed by NAME says something
   else, and the difference is reachable from a page in three lines: `s.name = 'b'` renames the store an
   upgrade transaction holds a handle for, and `tx.objectStore('b')` then finds no entry under 'b' and mints a
   SECOND handle for that one store — two handles, `tx.objectStore('b') !== s`, and §5.8 step 5 walking a set
   with one store in it twice. A name is the one thing about a store an upgrade transaction can change, so it
   is the one thing this set cannot be keyed by. The find answers JS_NULL for a store this transaction holds no
   handle for; both are OWNED, and the add asserts the uniqueness rather than trusting its callers.
   THE SET IS A JS ARRAY in creation order, for the reason every other list in this component is one
   (§State-isolation) and because §5.8 step 5 walks it: `count` is written with the length so a walk needs no
   second reader of `length` in a third file. */
JSValue idb_transaction_handle_find(JSContext *ctx, JSValueConst tx, JSValueConst store);
void    idb_transaction_handle_add(JSContext *ctx, JSValueConst tx, JSValueConst handle);
JSValue idb_transaction_handles(JSContext *ctx, JSValueConst tx, uint32_t *count);

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
