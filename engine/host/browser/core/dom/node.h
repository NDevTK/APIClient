/* THE NODE INTERFACE — DOM §4.4, the base every tree object shares. One JS object per Lexbor node. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void node_init(JSContext *ctx);
/* §4.4's prototypes for ONE realm — Node, CharacterData, Text, Comment. Declared into core/realm.h's list. */
void node_install_protos(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue node_chardata_proto(JSContext *ctx);
void node_free(JSContext *ctx);

/* The wrapper for `n`, or JS_NULL. The SAME Lexbor node always yields the SAME JS object: a page compares nodes
   by identity constantly, and a fresh wrapper per lookup makes every such comparison silently false. The
   prototype is chosen by the node's TYPE, so a Text node is a Text and an Element is an Element. */
JSValue node_wrap(JSContext *ctx, lxb_dom_node_t *n);
/* The Lexbor node behind any wrapper, or NULL if `v` is not one. */
lxb_dom_node_t *node_of(JSValueConst v);
/* Node.prototype — the base a derived DOM interface inherits from, borrowed. */
JSValue node_proto(JSContext *ctx);
/* The interface a node TYPE wears, borrowed — what a component naming its own interface object reads. */
JSValue node_type_proto(JSContext *ctx, int node_type);
/* A derived interface claims the node type(s) it is the interface OF: element.c claims ELEMENT with the
   Element.prototype it built on top of node_proto(). `proto` is CONSUMED — the table owns what it holds, and
   claiming a type twice is a DCHECK because one of the two would silently lose. This is what keeps node_wrap
   the ONE place a wrapper is built: two builders is two identity tables, which is no identity at all. */
/* A DERIVED INTERFACE CLAIMS A NODE TYPE, once per AGENT: the table maps a type to the CLASS whose
   per-context proto slot holds that realm's prototype. The prototype itself is never registered here — it is
   per realm, and the claiming component installs it into its own slot. */
void node_claim_type(int node_type, JSClassID cls);
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
/* §4.2.7 ChildNode and §4.2.8 ParentNode — the convenience mixins, installed on the interfaces whose IDL
   INCLUDES them. Not on Node.prototype: `document.remove()` is not a member of anything. */
void node_install_child_mixin(JSContext *ctx, JSValueConst proto);
/* §4.2.4 NonElementParentNode — getElementById, on Document and DocumentFragment. One implementation over its
   RECEIVER, for the reason ParentNode's members are: a mixin is what the IDL says these are. */
void node_install_nonelement_parent_mixin(JSContext *ctx, JSValueConst proto);
void node_install_parent_mixin(JSContext *ctx, JSValueConst proto);
/* §4.2.3's insertion/removing steps, as the LIST the standard writes: a component REGISTERS one and the
   chokepoint runs them all, in registration order. `inserted` is 1 after a node entered the tree and 0 BEFORE
   one leaves it. */
typedef void (*NodeTreeHook)(JSContext *ctx, lxb_dom_node_t *n, int inserted);
void node_add_tree_hook(NodeTreeHook fn);
/* THE PRE-ORDER SUCCESSOR within `root`'s subtree, or NULL at the end — the one traversal primitive the spec's
   tree-order algorithms need. Exported for the same reason it exists: having it once is what keeps every
   algorithm that walks in tree order from growing its own walker that disagrees at the edges. */
lxb_dom_node_t *node_next_in(lxb_dom_node_t *n, lxb_dom_node_t *root);
/* Its inverse — the pre-order PREDECESSOR within `root`'s subtree, or NULL at the beginning. §6.1's backwards
   traversal is stated over exactly this, and a second one written inline gets the last-descendant descent
   wrong. */
lxb_dom_node_t *node_prev_in(lxb_dom_node_t *n, lxb_dom_node_t *root);
/* §4.4 isConnected — is this node's root a document. */
bool node_is_connected(const lxb_dom_node_t *n);
/* §4.4's "root": the topmost inclusive ancestor. §5's boundary points and §6's traversers both ask. */
lxb_dom_node_t *node_root(lxb_dom_node_t *n);
/* §4.2's "inclusive ancestor" — walked UP from the descendant, so it is O(depth) with no allocation. */
bool node_is_inclusive_ancestor(const lxb_dom_node_t *a, const lxb_dom_node_t *b);
/* §4.4's "length" — 0 for a doctype, the data length of a CharacterData node, the child count otherwise. */
uint32_t node_length(const lxb_dom_node_t *n);
/* §4.2's "index" — how many siblings precede this node. */
uint32_t node_index(const lxb_dom_node_t *n);
/* An ELEMENT's interface is decided by its TAG — HTML's element-interface table, which is the html layer's
   knowledge. It registers the answer here; node_wrap asks it and stays the one place a wrapper is built. */
void node_set_element_resolver(JSValue (*fn)(JSContext *ctx, lxb_dom_element_t *el));

/* THE NODE IS BEING DESTROYED — drop the wrapper the map holds for it. Called from the DOM's destroy
   chokepoint, which is the only place a node's lifetime ends, so the map stays bounded by the nodes that
   actually exist rather than by every node ever created. */
void node_wrap_forget(JSContext *ctx, lxb_dom_node_t *n);

/* The wrapper identity map's size: how many nodes it names and how many slots it has. Reported by the seam
   assertion because a table that has grown out of proportion to the live document is the shape of a leak. */
void node_wrap_stats(long *n, long *cap);

#endif
