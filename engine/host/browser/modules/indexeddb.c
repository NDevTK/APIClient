/* IndexedDB concolic round-trip — see indexeddb.h. Extracted from main.c, then given REAL put->get logic
 * (mirroring storage.c): a stub that dropped stored values on write and returned bare opaque on read was
 * MISSING LOGIC; a bundle that stashes a token/URL in IDB and reads it back now recovers it as the @H example.
 * addEventListener on the request/db/tx registers the handler as a scheduler FLOW (js_add_listener). */
#include "modules/indexeddb.h"
#include "opaque.h"   /* g_opaque, js_noop, js_opaque_stub */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* onsuccess/onerror handler -> driven flow */
extern char *g_candidate;   /* @S replay: a tainted stored value is attacker-tamperable -> deliver the candidate on read */

/* key -> value the bundle put() this run (attacker-tamperable across sessions, like web storage). Keyed by the
 * explicit key (put(value,key)/get(key)); in-line keyPath stores are not yet resolved (fall through to opaque). */
static JSValue g_idb = JS_UNDEFINED;

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
static JSValue js_idb_opaque_req(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)   /* getAll/count/... -> request, result opaque */
{ return js_idb_request(ctx, js_concolic(ctx, "{idb}", JS_UNDEFINED)); }
/* put(value,key) / add(value,key): RECORD value keyed by the explicit key so a later get(key) recovers it. The
 * request result is the key (real IDB returns the stored key). No explicit key (in-line keyPath) -> opaque. */
static JSValue js_idb_put(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    if (c >= 2) {
        if (!JS_IsObject(g_idb)) { JS_FreeValue(ctx, g_idb); g_idb = JS_NewObject(ctx); }
        const char *k = JS_ToCString(ctx, v[1]);
        if (k) { JS_SetPropertyStr(ctx, g_idb, k, JS_DupValue(ctx, v[0])); JS_FreeCString(ctx, k); }
        return js_idb_request(ctx, JS_DupValue(ctx, v[1]));
    }
    return js_idb_request(ctx, js_concolic(ctx, "{idb}", JS_UNDEFINED));
}
/* get(key): recover the value put() this run as the @H EXAMPLE (opaque-for-control-flow so a gate still forks,
 * @S taint holds), deliver the @S candidate for a tainted stored value, or bare opaque when nothing was stored. */
static JSValue js_idb_get(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    if (c >= 1 && JS_IsObject(g_idb)) {
        const char *k = JS_ToCString(ctx, v[0]);
        if (k) {
            JSValue val = JS_GetPropertyStr(ctx, g_idb, k); JS_FreeCString(ctx, k);
            if (g_candidate && JS_IsOpaque(val)) { JS_FreeValue(ctx, val); return js_idb_request(ctx, JS_NewString(ctx, g_candidate)); }
            if (JS_IsOpaque(val)) return js_idb_request(ctx, val);                       /* stored an opaque: round-trip its taint */
            if (!JS_IsUndefined(val) && !JS_IsNull(val)) {                               /* stored a concrete value: opaque carrying it as the example */
                JSValue o = JS_NewOpaqueSourced(ctx, "{idb}", "{idb}");
                if (JS_IsOpaque(o)) { JS_SetOpaqueExample(ctx, o, val); return js_idb_request(ctx, o); }
                JS_FreeValue(ctx, o); return js_idb_request(ctx, val);
            }
            JS_FreeValue(ctx, val);
        }
    }
    return js_idb_request(ctx, js_concolic(ctx, "{idb}", JS_UNDEFINED));
}
static JSValue js_idb_store(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);
static JSValue js_idb_store_self(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)   /* index()/objectStore() -> a store */
{ return js_idb_store(ctx, t, c, v); }
static JSValue js_idb_store(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue s = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, s, "get", JS_NewCFunction(ctx, js_idb_get, "get", 1));           /* round-trip read */
    JS_SetPropertyStr(ctx, s, "getKey", JS_NewCFunction(ctx, js_idb_get, "getKey", 1));
    JS_SetPropertyStr(ctx, s, "add", JS_NewCFunction(ctx, js_idb_put, "add", 2));           /* round-trip write */
    JS_SetPropertyStr(ctx, s, "put", JS_NewCFunction(ctx, js_idb_put, "put", 2));
    const char *reqm[] = { "getAll", "getAllKeys", "count", "delete", "clear", "openCursor", "openKeyCursor" };   /* list/cursor results not yet modelled -> opaque */
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
void idb_free(JSContext *ctx) { JS_FreeValue(ctx, g_idb); g_idb = JS_UNDEFINED; }
