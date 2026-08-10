/* RANGE — DOM §5.5, the LIVE range. See range.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_RANGE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_RANGE_H

#include <lexbor/dom/dom.h>
#include "quickjs.h"

void range_init(JSContext *ctx);
/* §5.5's INTERFACE PROTOTYPE OBJECT for one realm — declared into core/realm.h's list. */
void range_install_proto(JSContext *ctx);
void range_install(JSContext *ctx, JSValueConst global);
void range_free(JSContext *ctx);

/* §4.5 createRange(): a new live range whose start and end are both (node, 0). `node` is the document's
   wrapper and is BORROWED. */
JSValue range_new_at(JSContext *ctx, JSValueConst node);

/* §5.5's "live range PRE-REMOVE steps", run by §4.2.3's remove BEFORE the node leaves the tree. */
void range_pre_remove(JSContext *ctx, lxb_dom_node_t *node);
/* §4.2.3's insert step 4, run AFTER a node has entered the tree: every live range whose start or end names the
   new parent at an offset past the new node's index moves along by one. */
void range_did_insert(JSContext *ctx, lxb_dom_node_t *node);

#endif
