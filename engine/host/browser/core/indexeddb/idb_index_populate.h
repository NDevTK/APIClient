/* INDEXED DATABASE §4.5 The IDBObjectStore interface's createIndex NOTE — "although this method does not return
 * an IDBRequest object, the INDEX CREATION ITSELF IS PROCESSED AS AN ASYNCHRONOUS REQUEST within the upgrade
 * transaction". See idb_index_populate.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_INDEX_POPULATE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_INDEX_POPULATE_H

#include "quickjs.h"

void idb_index_populate_init(JSContext *ctx);
void idb_index_populate_free(JSRuntime *rt);

/* PLACE THE POPULATION REQUEST — §4.5's createIndex, immediately after the index joins the store. The request
 * goes through §5.6 Asynchronously executing a request like every other, which is the whole of what makes the
 * note true: it queues BEHIND the requests already placed against the transaction (so §4.5's own worked example
 * — two `put`s, then a unique index — runs both puts first, and neither sees the index), and its operation is
 * what walks the store's existing records into the index and clears §2.6's unpopulated flag.
 *
 * A CONSTRAINT FAILURE THEN ABORTS THE TRANSACTION AND NOT ONE REQUEST, with no special-casing anywhere: the
 * operation raises §6.1 step 5's "ConstraintError", §5.6 step 5.4 reverts what it wrote, §5.10 Firing an error
 * event fires `error` at a request the page holds no reference to — so nothing can call `preventDefault()` on
 * it — and step 9.3 runs abort a transaction with that error. That is exactly the sentence §4.5 states for a
 * store whose data violates the new index's constraints.
 *
 * `handle` is §2.8's source object: the IDBObjectStore whose member placed the request. The IDBRequest §5.6
 * returns is dropped here, because §4.5 returns an IDBIndex and never that request. Every operand is BORROWED. */
void idb_index_populate_request(JSContext *ctx, JSValueConst handle, JSValueConst tx, JSValueConst store,
                                JSValueConst index);

#endif
