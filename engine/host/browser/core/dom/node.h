/* Node — Blink core/dom, the middle of the DOM inheritance spine: EventTarget <- Node <- Document/Element. Node
 * owns the tree members every node shares (the structural interface); Document and Element chain their prototype
 * here, and Node.prototype chains to EventTarget.prototype, so `document instanceof Node`, `element instanceof
 * Node`, and `... instanceof EventTarget` all hold — the real IDL inheritance, not per-interface duplication. */
#ifndef ENGINE_HOST_BROWSER_NODE_H
#define ENGINE_HOST_BROWSER_NODE_H
#include "quickjs.h"
void node_init(JSContext *ctx, JSValue global);   /* create Node.prototype (chains to EventTarget) + window.Node */
void node_free(JSContext *ctx);                    /* drop the prototype singleton (teardown) */
JSValueConst node_proto(JSContext *ctx);           /* the shared Node.prototype (BORROWED) to chain Document/Element onto */
#endif
