/* IndexedDB (window.indexedDB.open) — Blink modules/indexeddb/. The UNTRUSTED sandbox can't touch real IDB
 * (SECURITY.md), so this is a faithful-SHAPE stub: open() -> a request whose db/transaction/objectStore chain
 * exists and whose reads (get/getAll/...) return OPAQUE, so bundle code that stores/reads IDB runs and its
 * downstream endpoints/sinks are still explored. See indexeddb.c. */
#ifndef ENGINE_HOST_BROWSER_INDEXEDDB_H
#define ENGINE_HOST_BROWSER_INDEXEDDB_H
#include "quickjs.h"
JSValue js_idb_open(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);   /* indexedDB.open(name,ver) -> request */
#endif
