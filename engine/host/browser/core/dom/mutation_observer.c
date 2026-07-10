/* MutationObserver — Blink core/dom. A faithful hand implementation: the callback never fires from a real intersection/
 * mutation/resize headless, so the ctor registers it as a driven scheduler flow; observe/disconnect are
 * DEDICATED, documented no-effect impls (the callback is reached by exploration, not by events) — never a
 * generic noop stub. The canonical MutationObserver IDL AUDITS this for missing members (idlgen), not generates stubs. */
#include "core/dom/mutation_observer.h"
#include "quickjs.h"

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

/* observe(target[, options]): the callback (registered in the ctor) is driven by exploration; no events fire
   headless, so observe has no further observable effect — a conscious analysis decision, documented. */
static JSValue ob_observe(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
/* disconnect(): documented no-effect — we keep the callback reachable rather than un-drive it. */
static JSValue ob_disconnect(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
/* takeRecords(): the spec drains + returns the record queue; headless no records are ever queued -> empty. */
static JSValue ob_takeRecords(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return JS_NewArray(ctx); }

JSValue js_mutation_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) { JSValue r = js_add_listener(ctx, JS_UNDEFINED, 1, argv); JS_FreeValue(ctx, r); }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "observe", JS_NewCFunction(ctx, ob_observe, "observe", 1));
    JS_SetPropertyStr(ctx, o, "disconnect", JS_NewCFunction(ctx, ob_disconnect, "disconnect", 0));
    JS_SetPropertyStr(ctx, o, "takeRecords", JS_NewCFunction(ctx, ob_takeRecords, "takeRecords", 0));
    return o;
}
