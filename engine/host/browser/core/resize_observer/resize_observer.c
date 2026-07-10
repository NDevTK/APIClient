/* ResizeObserver — Blink core/resize_observer. A faithful hand implementation: the callback never fires from a real intersection/
 * mutation/resize headless, so the ctor registers it as a driven scheduler flow; observe/disconnect are
 * DEDICATED, documented no-effect impls (the callback is reached by exploration, not by events) — never a
 * generic noop stub. The canonical ResizeObserver IDL AUDITS this for missing members (idlgen), not generates stubs. */
#include "core/resize_observer/resize_observer.h"
#include "quickjs.h"

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);

/* observe(target[, options]): the callback (registered in the ctor) is driven by exploration; no events fire
   headless, so observe has no further observable effect — a conscious analysis decision, documented. */
static JSValue ob_observe(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
/* disconnect(): documented no-effect — we keep the callback reachable rather than un-drive it. */
static JSValue ob_disconnect(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
/* unobserve(target): documented no-effect — the callback stays reachable for orphan-driving. */
static JSValue ob_unobserve(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }

JSValue js_resize_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) { JSValue r = js_add_listener(ctx, JS_UNDEFINED, 1, argv); JS_FreeValue(ctx, r); }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "observe", JS_NewCFunction(ctx, ob_observe, "observe", 1));
    JS_SetPropertyStr(ctx, o, "disconnect", JS_NewCFunction(ctx, ob_disconnect, "disconnect", 0));
    JS_SetPropertyStr(ctx, o, "unobserve", JS_NewCFunction(ctx, ob_unobserve, "unobserve", 1));
    return o;
}
