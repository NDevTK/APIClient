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

/* §4.10 "replace data" STEPS 8-11 — the live-range half of a CharacterData splice, run by the splice itself
   once the data has moved. `offset` and `count` are the CODE-UNIT operands the algorithm was given AFTER its
   own step 3 clamp, and `data_units` is the replacement's length in code units. §4.10 owns the bytes; this
   owns what the boundary points do about them, which is why it is a call and not a second copy. */
void range_replace_data_steps(JSContext *ctx, lxb_dom_node_t *node, uint32_t offset, uint32_t count,
                              uint32_t data_units);

/* §4.4 `normalize()` STEPS 6.1-6.4 — run once per contiguous exclusive Text node `sib` that `node` absorbs,
   BEFORE `sib` is removed and with `length` being `node`'s length BEFORE the absorption. The removal that
   follows runs §5.5's pre-remove steps, which is right: 6.1-6.4 have already taken every boundary point off
   `sib`, so what is left for the removal to do is the sibling-index shift it does for any other node. */
void range_normalize_absorb_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *sib, uint32_t length);

/* §4.10 "split a Text node" STEPS 7.2-7.5 — run by the split once `new_node` is in the tree beside `node`.
   `offset` is the split point in code units. */
void range_split_text_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *new_node, uint32_t offset);

#endif
