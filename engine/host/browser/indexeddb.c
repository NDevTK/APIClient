/* IndexedDB shape stub — see indexeddb.h. Extracted from main.c. Reads return OPAQUE; the request/db/tx/store
 * chain exists so bundle code that opens IDB, reads a value, and flows it onward is explored. addEventListener
 * on the request/db/tx registers the handler as a scheduler FLOW (js_add_listener, borrowed from the host). */
#include "indexeddb.h"
#include "opaque.h"   /* g_opaque, js_noop, js_opaque_stub */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* onsuccess/onerror handler -> driven flow */

static JSValue js_idb_request(JSContext *ctx, JSValue result) {
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "result", result);
    JS_SetPropertyStr(ctx, r, "error", JS_NULL);
    JS_SetPropertyStr(ctx, r, "readyState", JS_NewString(ctx, "done"));
    JS_SetPropertyStr(ctx, r, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, r, "onsuccess", JS_NULL);
    JS_SetPropertyStr(ctx, r, "onerror", JS_NULL);
    JS_SetPropertyStr(ctx, r, "onupgradeneeded", JS_NULL);
    return r;
}
static JSValue js_idb_opaque_req(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)   /* get/getAll/... -> request, result opaque */
{ return js_idb_request(ctx, JS_DupValue(ctx, g_opaque)); }
static JSValue js_idb_store(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);
static JSValue js_idb_store_self(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)   /* index()/objectStore() -> a store */
{ return js_idb_store(ctx, t, c, v); }
static JSValue js_idb_store(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue s = JS_NewObject(ctx);
    const char *reqm[] = { "get", "getAll", "getAllKeys", "getKey", "count", "add", "put", "delete", "clear", "openCursor", "openKeyCursor" };
    for (size_t i = 0; i < sizeof reqm / sizeof reqm[0]; i++) JS_SetPropertyStr(ctx, s, reqm[i], JS_NewCFunction(ctx, js_idb_opaque_req, reqm[i], 1));
    JS_SetPropertyStr(ctx, s, "index", JS_NewCFunction(ctx, js_idb_store_self, "index", 1));
    JS_SetPropertyStr(ctx, s, "createIndex", JS_NewCFunction(ctx, js_idb_store_self, "createIndex", 1));
    return s;
}
static JSValue js_idb_tx(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue tx = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tx, "objectStore", JS_NewCFunction(ctx, js_idb_store_self, "objectStore", 1));
    JS_SetPropertyStr(ctx, tx, "abort", JS_NewCFunction(ctx, js_noop, "abort", 0));
    JS_SetPropertyStr(ctx, tx, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    return tx;
}
static JSValue js_idb_db(JSContext *ctx) {
    JSValue db = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, db, "transaction", JS_NewCFunction(ctx, js_idb_tx, "transaction", 2));
    JS_SetPropertyStr(ctx, db, "createObjectStore", JS_NewCFunction(ctx, js_idb_store_self, "createObjectStore", 1));
    JS_SetPropertyStr(ctx, db, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, db, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JSValue names = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, names, "contains", JS_NewCFunction(ctx, js_opaque_stub, "contains", 1));
    JS_SetPropertyStr(ctx, db, "objectStoreNames", names);
    return db;
}
JSValue js_idb_open(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{ return js_idb_request(ctx, js_idb_db(ctx)); }
