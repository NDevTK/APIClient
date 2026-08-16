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

/* §2.2.1's "an object store handle has an ASSOCIATED OBJECT STORE", which is what the handle set is keyed by:
   the sentence above scopes uniqueness to the STORE and not to a name, and a name is the one thing about a
   store an upgrade transaction can change. OWNED. */
JSValue idb_object_store_handle_store(JSContext *ctx, JSValueConst handle);

/* §2.2.1's NAME — the HANDLE's own, which §4.5's `name` getter answers with and which §4.5's own note tells
   apart from the store's: "as long as the transaction has not finished, this is the same as the associated
   object store's name. But once the transaction has finished, this attribute will not reflect changes made
   with a later upgrade transaction." OWNED. */
JSValue idb_object_store_handle_name(JSContext *ctx, JSValueConst handle);

/* §5.8 step 5.1: "set handle's name to its OBJECT STORE's name" — the whole of that step, stated here because
   the field is this component's. It is what makes `store.name` answer the old name after an upgrade
   transaction renamed the store and then aborted: §5.5 step 2 put the STORE's name back, and this is the write
   that follows it out to the handle the page is still holding. §5.8 decides WHICH handles it runs for (a store
   newly created during the transaction is skipped); this performs it. */
void idb_object_store_handle_restore_name(JSContext *ctx, JSValueConst handle);

/* Web IDL §3.7.5's brand, asked of a value that arrived from another component. */
bool idb_object_store_is(JSValueConst v);

#endif
