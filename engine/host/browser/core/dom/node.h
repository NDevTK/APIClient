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
/* Node.prototype — the base a derived DOM interface inherits from, borrowed. */
JSValueConst node_proto(void);
/* The interface a node TYPE wears, borrowed — what a component naming its own interface object reads. */
JSValueConst node_type_proto(int node_type);
/* A derived interface claims the node type(s) it is the interface OF: element.c claims ELEMENT with the
   Element.prototype it built on top of node_proto(). `proto` is CONSUMED — the table owns what it holds, and
   claiming a type twice is a DCHECK because one of the two would silently lose. This is what keeps node_wrap
   the ONE place a wrapper is built: two builders is two identity tables, which is no identity at all. */
void node_set_proto(JSContext *ctx, int node_type, JSValue proto);
/* Install `Node`, `CharacterData`, `Text` and `Comment` as globals — the interface OBJECTS, carrying §4.4's
   constants and the prototypes an `instanceof` names. */
void node_install_interfaces(JSContext *ctx, JSValueConst global);
/* One interface OBJECT for a component that owns a derived interface — element.c installs `Element`,
   document.c installs `Document`. Not constructible (none of these declares a constructor), inheriting Node's
   interface object so §4.4's constants read off it. */
void node_install_interface(JSContext *ctx, JSValueConst global, const char *name, JSValueConst proto);
/* The class id every node wrapper shares — components that need JS_NewObjectClass for one. */
JSClassID node_class_id(void);
/* What to do with a node once it is inserted (a <script> is PREPARED per HTML 4.12.1) — element.c's rule, asked
   for here so the base does not have to know what a script is. */
/* §4.2.3's INSERTION and REMOVING STEPS. Registered by the element component, fired by the DOM-mutation
   chokepoint itself, so a tree write cannot reach the tree without them — the same structural guarantee the
   chokepoint already gives time-travel capture. `inserted` is 1 for a node that entered a document and 0 for
   one that is about to leave it; the callee walks the SUBTREE, because inserting a subtree connects every
   element in it. */
void node_set_tree_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *n, int inserted));
/* §4.4 isConnected — is this node's root a document. */
bool node_is_connected(const lxb_dom_node_t *n);
/* An ELEMENT's interface is decided by its TAG — HTML's element-interface table, which is the html layer's
   knowledge. It registers the answer here; node_wrap asks it and stays the one place a wrapper is built. */
void node_set_element_resolver(JSValueConst (*fn)(lxb_dom_element_t *el));

#endif
