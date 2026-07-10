/* IndexedDB (window.indexedDB.open) — Blink modules/indexeddb/. The UNTRUSTED sandbox can't touch real IDB
 * (SECURITY.md), so this is an in-memory CONCOLIC round-trip (mirroring storage.c for localStorage): an
 * objectStore.put/add(value,key) records the value, and get(key) recovers it as the @H example while staying
 * opaque-for-control-flow (IDB is attacker-tamperable across sessions/tabs, so a gate on it still FORKS and @S
 * taint holds). Reads with nothing stored, and getAll/count/cursor (list results — not yet modelled), stay
 * opaque. The request/db/transaction/objectStore chain exists so bundle code that stores then reads runs. */
#ifndef ENGINE_HOST_BROWSER_INDEXEDDB_H
#define ENGINE_HOST_BROWSER_INDEXEDDB_H
#include "quickjs.h"
JSValue js_idb_open(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);   /* indexedDB.open(name,ver) -> request */
void idb_free(JSContext *ctx);   /* teardown: drop this run's stored values */
#endif
