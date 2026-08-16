/* INDEXED DATABASE §2.9's KEY RANGE and §4.7's IDBKeyRange interface. See idb_key_range.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_RANGE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_RANGE_H

#include "quickjs.h"

/* Declared ONCE PER AGENT: the class §4.7's brand check asks, the five member declarations, and the per-realm
   install that builds this realm's IDBKeyRange.prototype and interface object. The component holds no
   agent-lifetime JS value, so there is nothing to release — a range's own state is on the range. */
void idb_key_range_init(JSContext *ctx);

#endif
