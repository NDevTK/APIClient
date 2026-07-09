/* DOM Element JSClass + el_wrap — see dom_element.h.
 *
 * The wrapper is deliberately LOGIC-FREE: it only binds a real Lexbor node to a JS Element object whose
 * methods (still in main.c, migrating here) are DOM host-edges the ONE BFS scheduler drives — el_wrap adds
 * no control flow of its own, so no logic ever lives outside the scheduler. */
#include "dom_element.h"

JSClassID g_el_class_id;   /* the "Element" JSClass id (lexbor owns the nodes; no JS finalizer) */

JSValue el_wrap(JSContext *ctx, lxb_dom_element_t *el) {
    if (!el) return JS_NULL;
    JSValue o = JS_NewObjectClass(ctx, g_el_class_id);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, el);
    return o;
}

/* DOM TRAVERSAL: parentNode/children/firstElementChild/nextElementSibling were undefined -> `el.children
   .length` / `el.parentNode.x` THREW, killing DOM-walking bundles. Return REAL el_wrap'd element nodes from
   Lexbor so a tree walk that reaches a fetch/sink is explored. children is a REAL array (.length/.forEach/[i]
   all work). Only ELEMENT nodes (text nodes aren't wrapped — a walker keying on .children matches the browser). */
static int node_is_element(lxb_dom_node_t *n) { return n && n->type == LXB_DOM_NODE_TYPE_ELEMENT; }
JSValue js_el_parent(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    lxb_dom_node_t *p = lxb_dom_interface_node(el)->parent;
    return node_is_element(p) ? el_wrap(ctx, lxb_dom_interface_element(p)) : JS_NULL;
}
JSValue js_el_children(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    JSValue arr = JS_NewArray(ctx); if (!el) return arr;
    uint32_t idx = 0;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->first_child; n; n = n->next)
        if (node_is_element(n)) JS_SetPropertyUint32(ctx, arr, idx++, el_wrap(ctx, lxb_dom_interface_element(n)));
    return arr;
}
JSValue js_el_first_el_child(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->first_child; n; n = n->next)
        if (node_is_element(n)) return el_wrap(ctx, lxb_dom_interface_element(n));
    return JS_NULL;
}
JSValue js_el_next_el_sib(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->next; n; n = n->next)
        if (node_is_element(n)) return el_wrap(ctx, lxb_dom_interface_element(n));
    return JS_NULL;
}
