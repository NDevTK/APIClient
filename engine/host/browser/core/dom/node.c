/* Node — see node.h. */
#include "core/dom/node.h"
#include "core/dom/event_target.h"   /* Node.prototype chains to EventTarget.prototype (a Node IS an EventTarget) */

static JSValue g_node_proto = JS_UNDEFINED;   /* the ONE Node.prototype (spine middle) */

static JSValue node_illegal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");   /* Blink: `new Node()` throws */
}

void node_init(JSContext *ctx, JSValue global) {
    JSValue proto = JS_NewObject(ctx);
    JS_SetPrototype(ctx, proto, event_target_proto(ctx));   /* EventTarget <- Node */
    /* Node's shared tree members are added here as they are built (nodeType/appendChild/childNodes/...); the
       structural layer exists now so Document/Element inherit through it and `instanceof Node` holds. */
    JSValue ctor = JS_NewCFunction2(ctx, node_illegal_ctor, "Node", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);   /* ctor.prototype = proto, proto.constructor = ctor */
    JS_SetPropertyStr(ctx, global, "Node", ctor);
    g_node_proto = proto;
}
void node_free(JSContext *ctx) { JS_FreeValue(ctx, g_node_proto); g_node_proto = JS_UNDEFINED; }
JSValueConst node_proto(JSContext *ctx) { (void)ctx; return g_node_proto; }
