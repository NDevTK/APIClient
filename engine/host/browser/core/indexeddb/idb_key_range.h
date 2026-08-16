/* INDEXED DATABASE §2.9's KEY RANGE and §4.7's IDBKeyRange interface. See idb_key_range.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_RANGE_H
#define ENGINE_HOST_BROWSER_CORE_INDEXEDDB_IDB_KEY_RANGE_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT: the class §4.7's brand check asks, the five member declarations, and the per-realm
   install that builds this realm's IDBKeyRange.prototype and interface object. The component holds no
   agent-lifetime JS value, so there is nothing to release — a range's own state is on the range. */
void idb_key_range_init(JSContext *ctx);

/* §2.9's CONVERT A VALUE TO A KEY RANGE, which is what every member that takes a `query` runs before it does
   anything else — §4.5's `get`, `getKey`, `delete` and `count`, and §5.12's retrieve-multiple. A value that IS
   a key range is itself; undefined or null is an unbounded key range, or a "DataError" where the null
   disallowed flag is set; anything else is §7.4's key, in a range containing only it.
   Returns 0 with *prange an OWNED key range, or -1 with the "DataError" DOMException live. */
int idb_key_range_from_value(JSContext *ctx, JSValueConst value, bool null_disallowed, JSValue *prange);

/* §2.9's "a key is IN a key range" — the membership test §6.2's retrievals, §6.4's delete, §6.5's count and
   §6.7's cursor iteration are each stated over. `range` is one of this component's own ranges, which is what
   the conversion above guarantees, so a value that is not one crashes rather than answering. */
bool idb_key_range_contains(JSContext *ctx, JSValueConst range, JSValueConst key);

#endif
