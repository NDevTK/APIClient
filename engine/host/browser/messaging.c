/* Channel messaging — see messaging.h. Extracted from main.c. A MessagePort/BroadcastChannel is a faithful-shape
 * object (postMessage/close/addEventListener); the onmessage handler registered via addEventListener becomes a
 * driven scheduler flow (js_add_listener), so a page's channel message handler is explored. */
#include "messaging.h"
#include "opaque.h"   /* js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* onmessage handler -> driven flow */

static JSValue js_msgport_new(JSContext *ctx) {
    JSValue p = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, p, "postMessage", JS_NewCFunction(ctx, js_noop, "postMessage", 1));
    JS_SetPropertyStr(ctx, p, "start", JS_NewCFunction(ctx, js_noop, "start", 0));
    JS_SetPropertyStr(ctx, p, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, p, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, p, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    return p;
}
JSValue js_msg_channel_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "port1", js_msgport_new(ctx));
    JS_SetPropertyStr(ctx, o, "port2", js_msgport_new(ctx));
    return o;
}
JSValue js_broadcast_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = js_msgport_new(ctx);   /* same shape (postMessage/close/addEventListener) */
    JS_SetPropertyStr(ctx, o, "name", (argc >= 1) ? JS_ToString(ctx, argv[0]) : JS_NewString(ctx, ""));
    return o;
}
