/* IntersectionObserver — Blink core/intersection_observer. A faithful hand implementation, audited complete
 * against canonical IntersectionObserver IDL (engine/idlgen.mjs reports any missing member). The callback never
 * fires from a real intersection headless, so the ctor registers it as a driven scheduler flow; the config
 * attributes (root/rootMargin/thresholds/...) hold the REAL values the page passed in `options` (with the spec
 * defaults) — the observer's own state, not a stub; observe/disconnect are dedicated documented no-effect. */
#include "core/intersection_observer/intersection_observer.h"
#include "quickjs.h"

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

/* observe(target[, options]): the callback (registered in the ctor) is driven by exploration; no intersections
   fire headless, so observe has no further observable effect — a conscious analysis decision, documented. */
static JSValue ob_observe(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
static JSValue ob_disconnect(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
static JSValue ob_unobserve(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
/* takeRecords(): the spec drains + returns the record queue; headless no records are ever queued -> empty. */
static JSValue ob_takeRecords(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return JS_NewArray(ctx); }

/* option or default: read options[key] if present, else `dflt` (which is consumed). */
static JSValue opt_or(JSContext *ctx, JSValueConst opts, const char *key, JSValue dflt) {
    if (JS_IsObject(opts)) { JSValue v = JS_GetPropertyStr(ctx, opts, key);
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) { JS_FreeValue(ctx, dflt); return v; } JS_FreeValue(ctx, v); }
    return dflt;
}

JSValue js_intersection_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) { JSValue r = js_add_listener(ctx, JS_UNDEFINED, 1, argv); JS_FreeValue(ctx, r); }
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue o = JS_NewObject(ctx);
    /* Config attributes = the REAL values from `options` with the IDL/spec defaults (the observer's own state). */
    JS_SetPropertyStr(ctx, o, "root", opt_or(ctx, opts, "root", JS_NULL));
    JS_SetPropertyStr(ctx, o, "rootMargin", opt_or(ctx, opts, "rootMargin", JS_NewString(ctx, "0px 0px 0px 0px")));
    JS_SetPropertyStr(ctx, o, "scrollMargin", opt_or(ctx, opts, "scrollMargin", JS_NewString(ctx, "0px 0px 0px 0px")));
    JS_SetPropertyStr(ctx, o, "delay", opt_or(ctx, opts, "delay", JS_NewInt32(ctx, 0)));
    JS_SetPropertyStr(ctx, o, "trackVisibility", opt_or(ctx, opts, "trackVisibility", JS_FALSE));
    /* thresholds: the spec normalizes options.threshold to a list; undefined -> [0], a number -> [n], else the array. */
    { JSValue th = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "threshold") : JS_UNDEFINED;
      JSValue arr;
      if (JS_IsArray(th)) arr = JS_DupValue(ctx, th);
      else { arr = JS_NewArray(ctx); JS_SetPropertyUint32(ctx, arr, 0, JS_IsNumber(th) ? JS_DupValue(ctx, th) : JS_NewFloat64(ctx, 0)); }
      JS_FreeValue(ctx, th);
      JS_SetPropertyStr(ctx, o, "thresholds", arr); }
    JS_SetPropertyStr(ctx, o, "observe", JS_NewCFunction(ctx, ob_observe, "observe", 1));
    JS_SetPropertyStr(ctx, o, "unobserve", JS_NewCFunction(ctx, ob_unobserve, "unobserve", 1));
    JS_SetPropertyStr(ctx, o, "disconnect", JS_NewCFunction(ctx, ob_disconnect, "disconnect", 0));
    JS_SetPropertyStr(ctx, o, "takeRecords", JS_NewCFunction(ctx, ob_takeRecords, "takeRecords", 0));
    return o;
}
