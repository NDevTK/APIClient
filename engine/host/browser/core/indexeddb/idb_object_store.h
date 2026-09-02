/* INDEXED DATABASE §2.2.1's OBJECT STORE HANDLE and §4.5's IDBObjectStore. See idb_object_store.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_OBJECT_STORE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_OBJECT_STORE_H

#include <stdbool.h>
#include <stdint.h>

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

/* §2.2.1's ASSOCIATED TRANSACTION, which §4.5's `transaction` getter answers with and which §2.6.1 makes the
   INDEX HANDLE's too ("the transaction of an index handle is the transaction of its associated object store
   handle") — so an index handle asks this rather than keeping a second copy that could disagree. OWNED. */
JSValue idb_object_store_handle_transaction(JSContext *ctx, JSValueConst handle);

/* §2.2.1's INDEX SET — "an object store handle has an index set, which is initialized to the set of indexes
 * that reference the associated object store when the object store handle is created. The contents of the set
 * will remain constant EXCEPT WHEN AN UPGRADE TRANSACTION IS LIVE."
 *
 * IT IS NOT THE STORE'S SET, and §5.8 step 5.2 is the whole reason: an aborted upgrade transaction restores the
 * HANDLE's from the store's, which is a no-op only if the two were the same object. §4.5's `indexNames`,
 * `index()`, `createIndex` and `deleteIndex` all read and write THIS one ("add index to this's index set",
 * "remove index from this's index set", "the index named name in this's index set"), while §6.1 step 5 and
 * §6.4 step 2 walk the STORE's. An Array of §2.6 index records in creation order, OWNED. */
JSValue idb_object_store_handle_index_set(JSContext *ctx, JSValueConst handle);

/* §5.8 STEP 5.2: "set handle's index set to the set of indexes that reference its object store", and §4.4's
   deleteObjectStore step 6, "if there is an object store handle associated with store and transaction, remove
   all entries from its index set". Two writes because they are two algorithms with two answers — one copies
   the store's set, one empties. */
void idb_object_store_handle_reset_index_set(JSContext *ctx, JSValueConst handle);
void idb_object_store_handle_clear_index_set(JSContext *ctx, JSValueConst handle);

/* §2.6.1's "there must be only ONE INDEX HANDLE associated with a particular index within a transaction",
   which §4.5's `index()` note states as what a page compares: "each call to this method on the same
   IDBObjectStore instance with the same name returns the same IDBIndex instance". The set lives HERE and not
   on the transaction — unlike §2.2.1's own handle set — because there is exactly one object store handle per
   store per transaction, so this IS that scope, and because §5.8 step 6 reaches every index handle of a
   transaction through its object store handles. An Array in creation order, OWNED; `count` is written with
   its length so a walk needs no second reader of `length` in a third file. */
JSValue idb_object_store_handle_index_handles(JSContext *ctx, JSValueConst handle, uint32_t *count);

/* §6.1's and §6.4's OPERATIONS, MINTED — §4.5's `add or put` step 12 and `delete` step 7 state them, and
   §4.9's `update` step 10 and `delete` step 7 state the SAME two over a cursor's effective object store and
   effective key. Exported so there is one closure over §6.1's five operands rather than two that could come to
   disagree about what they are. `key` is a §2.4 key record, `range` a §2.9 key range; every operand is
   BORROWED and the callable is OWNED. */
JSValue idb_object_store_record_operation(JSContext *ctx, JSValueConst tx, JSValueConst store,
                                          JSValueConst value, JSValueConst key, bool no_overwrite);
JSValue idb_object_store_delete_operation(JSContext *ctx, JSValueConst tx, JSValueConst store,
                                          JSValueConst range);

/* Web IDL §3.8 Platform objects implementing interfaces' "value implements an interface interface", asked of a
   value that arrived from another component. */
bool idb_object_store_is(JSValueConst v);

#endif
