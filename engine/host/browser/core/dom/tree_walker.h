/* TREEWALKER — DOM §6.2. See tree_walker.c for why each of its seven members is a step machine. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_TREE_WALKER_H
#define ENGINE_HOST_BROWSER_CORE_DOM_TREE_WALKER_H

#include <lexbor/dom/dom.h>
#include "quickjs.h"

void tree_walker_init(JSContext *ctx);
/* §6.2's INTERFACE PROTOTYPE OBJECT for one realm — declared into core/realm.h's list. */
void tree_walker_install_proto(JSContext *ctx);
/* §6.2's interface OBJECT on the global. It declares no constructor, so it is not constructible. */
void tree_walker_install(JSContext *ctx, JSValueConst global);
void tree_walker_free(JSContext *ctx);

/* §4.5 createTreeWalker steps 1-4: a new TreeWalker whose root AND current are `root`. `root` is the node's
   wrapper and `filter` is JS_NULL or the callback object; both are BORROWED. */
JSValue tree_walker_new(JSContext *ctx, JSValueConst root, uint32_t what, JSValueConst filter);

#endif
