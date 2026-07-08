/* Web storage (localStorage / sessionStorage) as a concolic round-trip — see storage.h. */
#include "storage.h"

/* key -> value the bundle setItem'd THIS run. getItem returns it as the @H EXAMPLE while staying
   opaque-for-control-flow (web storage is attacker-tamperable across sessions/tabs, so a gate on it still
   FORKS and @S taint holds) — recovers values the bundle round-trips through localStorage/sessionStorage
   instead of degrading them to a {} shape. */
static JSValue g_storage = JS_UNDEFINED;

/* setItem(k,v): record v keyed by k so a later getItem(k) recovers it as the @H example. NOT a discovery
   driver itself, but the value it stores is (a URL/id round-tripped through storage). */
JSValue js_storage_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) {
        if (!JS_IsObject(g_storage)) { JS_FreeValue(ctx, g_storage); g_storage = JS_NewObject(ctx); }
        const char *k = JS_ToCString(ctx, argv[0]);
        if (k) { JS_SetPropertyStr(ctx, g_storage, k, JS_DupValue(ctx, argv[1])); JS_FreeCString(ctx, k); }
    }
    return JS_UNDEFINED;
}
/* getItem(k): stored data is EXTERNAL INPUT (attacker-tamperable across sessions/tabs) -> OPAQUE for
   control-flow (feeds auth headers/branches opaquely, forks gates, @S taint, replay-sound). BUT if the bundle
   setItem'd k THIS run, carry that value as the concrete @H EXAMPLE (concolic, like a config/reply): opaque
   for forking, real value for the endpoint key. Not set this run -> bare opaque. */
JSValue js_storage_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1 && JS_IsObject(g_storage)) {
        const char *k = JS_ToCString(ctx, argv[0]);
        if (k) {
            JSValue v = JS_GetPropertyStr(ctx, g_storage, k); JS_FreeCString(ctx, k);
            if (JS_IsOpaque(v)) return v;                                  /* stored an opaque (e.g. setItem of location.hash): round-trip its taint */
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {                     /* stored a concrete value: opaque-for-control-flow carrying it as the example */
                JSValue o = JS_NewOpaqueSourced(ctx, "{ls}", "ls");
                if (JS_IsOpaque(o)) { JS_SetOpaqueExample(ctx, o, v); return o; }   /* consumes v */
                JS_FreeValue(ctx, o); return v;
            }
            JS_FreeValue(ctx, v);
        }
    }
    return JS_DupValue(ctx, g_opaque);
}
void storage_free(JSContext *ctx) { JS_FreeValue(ctx, g_storage); g_storage = JS_UNDEFINED; }
