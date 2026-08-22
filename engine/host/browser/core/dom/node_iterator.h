/* NODEITERATOR — DOM §6.1. See node_iterator.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_ITERATOR_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_ITERATOR_H

#include <lexbor/dom/dom.h>
#include "quickjs.h"

void node_iterator_init(JSContext *ctx);
/* §6.1's INTERFACE PROTOTYPE OBJECT for one realm — declared into core/realm.h's list. */
void node_iterator_install_proto(JSContext *ctx);
/* §6.1's interface OBJECT on the global. It declares no constructor. */
void node_iterator_install(JSContext *ctx, JSValueConst global);
void node_iterator_free(JSRuntime *rt);

/* §4.5 createNodeIterator steps 1-5. `root` is the node's wrapper and `filter` is JS_NULL or the callback
   object; both are BORROWED. */
JSValue node_iterator_new(JSContext *ctx, JSValueConst root, uint32_t what, JSValueConst filter);

/* §6.1's PRE-REMOVE STEPS, run by §4.2.3's remove for every NodeIterator whose root's node document is the
   removed node's. Registered as a tree hook so the one place a node leaves the tree is the one place this
   happens. */
void node_iterator_pre_remove(JSContext *ctx, lxb_dom_node_t *to_be_removed);

#endif
