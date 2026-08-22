/* INDEXED DATABASE §2.6.1's INDEX HANDLE and §4.6's IDBIndex over it. See idb_index_handle.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_INDEX_HANDLE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_INDEX_HANDLE_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared once per AGENT: §4.6's class and its members, plus the per-realm install. It holds one
   agent-lifetime value — the private Symbol §2.6.1's slots hang off — which is what the release takes back. */
void idb_index_handle_init(JSContext *ctx);
void idb_index_handle_free(JSRuntime *rt);

/* §4.5's `createIndex` step 13 and `index()` step 6, "return an index handle associated with index and this".
   §2.6.1: "an index handle has an associated index and an associated OBJECT STORE HANDLE. The transaction of
   an index handle is the transaction of its associated object store handle" — so the transaction is not stored
   here, it is asked of the store handle, and there is one answer rather than two that could disagree.
   The UNIQUENESS ("only one index handle associated with a particular index within a transaction") is the
   OBJECT STORE HANDLE's set, because there is exactly one of those per store per transaction — the same
   argument idb_transaction.h makes for its own. OWNED. */
JSValue idb_index_handle(JSContext *ctx, JSValueConst index, JSValueConst store_handle);
bool    idb_index_handle_is(JSValueConst v);

/* §2.6's index this handle is a view of, and §2.6.1's own name. Both OWNED. */
JSValue idb_index_handle_index(JSContext *ctx, JSValueConst handle);
JSValue idb_index_handle_name(JSContext *ctx, JSValueConst handle);

/* §5.8 STEP 6: "for each index handle handle associated with transaction, including those for indexes that
   were created or deleted during transaction: if handle's index was NOT NEWLY CREATED during transaction, set
   handle's name to its index's name." The guard is §5.5 step 2's list and therefore idb_database.h's; what is
   here is the write it decides. */
void idb_index_handle_restore_name(JSContext *ctx, JSValueConst handle);

#endif
