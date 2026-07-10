/* EventTarget — see event_target.h. */
#include "core/dom/event_target.h"

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* register a handler -> a driven scheduler flow */

static JSValue g_event_target_proto = JS_UNDEFINED;   /* the ONE EventTarget.prototype (spine root) */

/* removeEventListener / dispatchEvent are DEDICATED documented no-effect: every registered handler is kept
   REACHABLE for orphan-driving (we never honour removal), and handlers are driven by exploration rather than a
   synthetic dispatch — a conscious analysis decision, not a stub. */
static JSValue et_remove(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }
static JSValue et_dispatch(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_TRUE; }
static JSValue et_illegal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");   /* `new EventTarget()` is legal in the spec, but headless we never construct a bare one — a page's own throw is the forcing function if it does */
}

void event_target_init(JSContext *ctx, JSValue global) {
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, proto, "removeEventListener", JS_NewCFunction(ctx, et_remove, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, proto, "dispatchEvent", JS_NewCFunction(ctx, et_dispatch, "dispatchEvent", 1));
    JSValue ctor = JS_NewCFunction2(ctx, et_illegal_ctor, "EventTarget", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);   /* ctor.prototype = proto, proto.constructor = ctor */
    JS_SetPropertyStr(ctx, global, "EventTarget", ctor);
    g_event_target_proto = proto;   /* keep the singleton (owns a ref) — chained onto by Node/Document/Element */
}
void event_target_free(JSContext *ctx) { JS_FreeValue(ctx, g_event_target_proto); g_event_target_proto = JS_UNDEFINED; }
JSValueConst event_target_proto(JSContext *ctx) { (void)ctx; return g_event_target_proto; }
