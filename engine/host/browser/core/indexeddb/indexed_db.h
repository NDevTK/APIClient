/* INDEXED DATABASE §4.3's IDBFactory and the `indexedDB` attribute. See indexed_db.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_INDEXED_DB_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_INDEXED_DB_H

#include "quickjs.h"

/* Declared ONCE PER AGENT: the IDBFactory class §3.7.5's brand check asks, `cmp`'s declaration, and the
   per-realm install that builds this realm's IDBFactory.prototype, its interface object and the one
   [SameObject] IDBFactory `indexedDB` answers with. Holds no agent-lifetime JS value, so there is nothing to
   release. */
void indexed_db_init(JSContext *ctx);

#endif
