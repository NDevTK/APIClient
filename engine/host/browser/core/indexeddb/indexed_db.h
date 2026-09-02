/* INDEXED DATABASE §4.3's IDBFactory and the `indexedDB` attribute. See indexed_db.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_INDEXED_DB_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_INDEXED_DB_H

#include "quickjs.h"

/* Declared ONCE PER AGENT: the IDBFactory class §3.7.7 Operations' brand check asks, `cmp`'s declaration, and
   the
   per-realm install that builds this realm's IDBFactory.prototype, its interface object and the one
   [SameObject] IDBFactory `indexedDB` answers with. */
void indexed_db_init(JSContext *ctx);
/* The AGENT's half, undone — core/platform.h's release column. The sentence that stood here said this
   component "holds no agent-lifetime JS value, so there is nothing to release", and the second clause did not
   follow from the first: a class id, a realm-value slot and five declarations are all made in and against ONE
   runtime, and every one of them was carried across it. */
void indexed_db_free(void);

#endif
