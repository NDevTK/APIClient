/* THE NODE INTERFACE — DOM §4.4, the base every tree object shares. One JS object per Lexbor node. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void node_init(JSContext *ctx);
void node_free(JSContext *ctx);

/* The wrapper for `n`, or JS_NULL. The SAME Lexbor node always yields the SAME JS object: a page compares nodes
   by identity constantly, and a fresh wrapper per lookup makes every such comparison silently false. The
   prototype is chosen by the node's TYPE, so a Text node is a Text and an Element is an Element. */
JSValue node_wrap(JSContext *ctx, lxb_dom_node_t *n);
/* The Lexbor node behind any wrapper, or NULL if `v` is not one. */
lxb_dom_node_t *node_of(JSValueConst v);
/* The members every node kind carries (tree accessors, appendChild/removeChild, textContent), installed on a
   wrapper by whichever component builds it. */
void node_install_base(JSContext *ctx, JSValueConst obj);
/* The class id every node wrapper shares — components that need JS_NewObjectClass for one. */
JSClassID node_class_id(void);
/* element.c registers how an Element wrapper is furnished, and what to do with a node once it is inserted (a
   <script> is PREPARED per HTML 4.12.1). node_wrap stays the ONE place a wrapper is built — two builders is two
   identity tables, which is no identity at all. */
void node_set_element_installer(void (*fn)(JSContext *ctx, JSValueConst obj));
void node_set_inserted_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *n));

#endif
