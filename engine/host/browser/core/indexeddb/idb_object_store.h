/* INDEXED DATABASE §2.2.1's OBJECT STORE HANDLE and §4.5's IDBObjectStore. See idb_object_store.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_OBJECT_STORE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_OBJECT_STORE_H

#include <stdbool.h>

#include "quickjs.h"

void idb_object_store_init(JSContext *ctx);
void idb_object_store_free(JSRuntime *rt);

/* §2.2.1's "an object store handle has an associated object store and an associated transaction", built for
 * §4.4's `createObjectStore` step 10 and §4.10's `objectStore()` last step ("return an object store handle
 * associated with store and this").
 *
 * IT IS NOT A CACHE AND THE CALLER MUST NOT MAKE IT ONE. §2.2.1: "there must be only one object store handle
 * associated with a particular object store within a transaction", which §4.10's note restates as "each call
 * to this method on the same IDBTransaction instance with the same name returns the same IDBObjectStore
 * instance" — a page compares them. The SET of handles belongs to the TRANSACTION (idb_transaction.h), so this
 * entry MINTS one and the caller files it; a second cache here would be a second answer to one question.
 *
 * "A name, which is initialized to the name of the associated object store when the object store handle is
 * created" — the handle's own copy, which is what §5.8 reverts and what makes `store.name` keep answering after
 * an aborted rename. Both operands are BORROWED. OWNED. */
JSValue idb_object_store_handle(JSContext *ctx, JSValueConst store, JSValueConst transaction);

/* Web IDL §3.7.5's brand, asked of a value that arrived from another component. */
bool idb_object_store_is(JSValueConst v);

#endif
