/* Web storage (localStorage / sessionStorage) as a concolic round-trip — see storage.h. */
#include "modules/storage.h"

/* key -> value the bundle setItem'd THIS run. getItem returns it as the @H EXAMPLE while staying
   opaque-for-control-flow (web storage is attacker-tamperable across sessions/tabs, so a gate on it still
   FORKS and @S taint holds) — recovers values the bundle round-trips through localStorage/sessionStorage
   instead of degrading them to a {} shape. */
static JSValue g_storage = JS_UNDEFINED;
extern char *g_candidate;   /* @S replay: the concrete candidate a source getter returns */

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
            /* @S CANDIDATE flow reading a TAINTED key: web storage is attacker-tamperable across tabs/sessions,
               so a value the app round-tripped from an attacker source is itself an attacker-delivered value.
               DELIVER THE CANDIDATE here (exactly like a source getter), so a STORED/second-order sink
               (handler A plants localStorage, handler B reads+sinks) breaks out WITHOUT needing to re-run the
               planter — the sink handler alone reads the candidate. Only tainted keys (stored opaque): a
               concrete app value is not made attacker-controlled. */
            if (g_candidate && JS_IsConcolic(v)) { JS_FreeValue(ctx, v); return JS_NewString(ctx, g_candidate); }
            if (JS_IsConcolic(v)) return v;                                  /* stored an opaque (e.g. setItem of location.hash): round-trip its taint */
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {                     /* stored a concrete value: opaque-for-control-flow carrying it as the example */
                JSValue o = JS_NewConcolicSourced(ctx, "{ls}", "{ls}");
                if (JS_IsConcolic(o)) { JS_SetConcolicExample(ctx, o, v); return o; }   /* consumes v */
                JS_FreeValue(ctx, o); return v;
            }
            JS_FreeValue(ctx, v);
        }
    }
    /* AMBIENT key (never set this run): the PURE SECOND-ORDER surface — an attacker who can write this origin's
       storage (a prior XSS, a sibling app on the origin) plants the value, and a later load reads+sinks it. It
       is genuinely attacker-plantable, so deliver the @S replay candidate here too (the sink handler alone reads
       it — no planter re-run needed); the finding is tagged second-order (needs a plant) at the report. */
    if (g_candidate) return JS_NewString(ctx, g_candidate);
    return js_concolic(ctx, "{ls}", JS_UNDEFINED);
}
void storage_free(JSContext *ctx) { JS_FreeValue(ctx, g_storage); g_storage = JS_UNDEFINED; }
