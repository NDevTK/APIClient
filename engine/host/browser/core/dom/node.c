/* Node — see node.h. */
#include "core/dom/node.h"
#include "core/dom/event_target.h"   /* Node.prototype chains to EventTarget.prototype (a Node IS an EventTarget) */

static JSValue g_node_proto = JS_UNDEFINED;   /* the ONE Node.prototype (spine middle) */

static JSValue node_illegal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");   /* Blink: `new Node()` throws */
}

/* The Node.nodeType CONSTANTS (spec): exposed on BOTH Node.prototype (so `node.ELEMENT_NODE`) and the Node
   interface object (so `Node.ELEMENT_NODE`) — bundles branch on `n.nodeType === Node.ELEMENT_NODE`. */
static const struct { const char *name; int val; } NODE_CONSTS[] = {
    { "ELEMENT_NODE", 1 }, { "ATTRIBUTE_NODE", 2 }, { "TEXT_NODE", 3 }, { "CDATA_SECTION_NODE", 4 },
    { "ENTITY_REFERENCE_NODE", 5 }, { "ENTITY_NODE", 6 }, { "PROCESSING_INSTRUCTION_NODE", 7 },
    { "COMMENT_NODE", 8 }, { "DOCUMENT_NODE", 9 }, { "DOCUMENT_TYPE_NODE", 10 }, { "DOCUMENT_FRAGMENT_NODE", 11 },
    { "NOTATION_NODE", 12 },
};
void node_init(JSContext *ctx, JSValue global) {
    JSValue proto = JS_NewObject(ctx);
    JS_SetPrototype(ctx, proto, event_target_proto(ctx));   /* EventTarget <- Node */
    /* Node's shared tree members are added here as they are built (appendChild/childNodes/...); the structural
       layer exists so Document/Element inherit through it and `instanceof Node` holds. */
    JSValue ctor = JS_NewCFunction2(ctx, node_illegal_ctor, "Node", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);   /* ctor.prototype = proto, proto.constructor = ctor */
    for (int i = 0; i < (int)(sizeof NODE_CONSTS / sizeof NODE_CONSTS[0]); i++) {
        JS_SetPropertyStr(ctx, proto, NODE_CONSTS[i].name, JS_NewInt32(ctx, NODE_CONSTS[i].val));
        JS_SetPropertyStr(ctx, ctor,  NODE_CONSTS[i].name, JS_NewInt32(ctx, NODE_CONSTS[i].val));
    }
    JS_SetPropertyStr(ctx, global, "Node", ctor);
    g_node_proto = proto;
}
void node_free(JSContext *ctx) { JS_FreeValue(ctx, g_node_proto); g_node_proto = JS_UNDEFINED; }
JSValueConst node_proto(JSContext *ctx) { (void)ctx; return g_node_proto; }
