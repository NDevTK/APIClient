/* RANGE — DOM §5.5, the LIVE range. See range.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_RANGE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_RANGE_H

#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/abstract_range.h"

JSClassID range_class_id(void);

void range_init(JSContext *ctx);
/* §5.5's INTERFACE PROTOTYPE OBJECT for one realm — declared into core/realm.h's list. */
void range_install_proto(JSContext *ctx);
void range_install(JSContext *ctx, JSValueConst global);
void range_free(JSRuntime *rt);

/* §4.5 createRange(): a new live range whose start and end are both (node, 0). `node` is the document's
   wrapper and is BORROWED. */
JSValue range_new_at(JSContext *ctx, JSValueConst node);

/* §5.5's "a new range" AT TWO GIVEN BOUNDARY POINTS — a LIVE one, registered with the live-range set below so
   that §4.2.3's insert and remove and §4.10's character-data splices move it. Selection API §2's "Each
   selection can be associated with a single range" is associated with one of THESE, and that is the whole of
   why Selection API §5 "Responding to DOM Mutations" needs no second mechanism: it says the selection's range
   is updated "as if it's a live range", and it IS one. `snode`/`enode` are node wrappers and are BORROWED; the
   caller states them in the order §5.2 requires, which every Selection API §3 member that mints a range does
   by comparing its two points first. OWNED. */
JSValue range_new_bp(JSContext *ctx, JSValueConst snode, uint32_t soff, JSValueConst enode, uint32_t eoff);

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
   follows runs §5.5's LIVE RANGE pre-remove steps (the standard's own term, defined at §5.5 — the unqualified
   "pre-remove" is §4.2.3's own operation), which is right: 6.1-6.4 have already taken every boundary point off
   `sib`, so what is left for the removal to do is the sibling-index shift it does for any other node. */
void range_normalize_absorb_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *sib, uint32_t length);

/* §4.11 "Interface Text"'s "split a Text node" STEPS 7.2-7.5 — run by the split once `new_node` is in the
   tree beside `node`.
   `offset` is the split point in code units. */
void range_split_text_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *new_node, uint32_t offset);

/* ---- §5.5's TWO MACHINES, AS MACHINES ANOTHER INTERFACE'S MEMBER MAY DECLARE ------------------------------
 *
 * Selection API §3 states its stringifier and its `deleteFromDocument()` as §5.5's OWN algorithms performed on
 * a different receiver — "the concatenation of the rendered text" of the selection's range, and "must invoke
 * deleteContents() on this's range". A second walk in that component would be a second answer to what a
 * range's text is and to which nodes a range's deletion removes, so the WALK is exported and only the RECEIVER
 * differs. The subject arrives as a RangeBounds the caller resolved, which is what keeps the dependency one
 * way: this file knows nothing about who is associated with a range.
 *
 * THE STATE IS PUBLIC BECAUSE AN IdlStepDecl'S SIZE IS A COMPILE-TIME CONSTANT. A member declaring one of these
 * embeds the state as its own first field and hands `&st->…` down, exactly as §5.5's own surroundContents
 * embeds the extract machine's. The `visit` and the STEPS array are the same objects both members name — the
 * stage numbering is shared, so a flow parked in one of these says the same thing whichever member it entered
 * through. */

typedef struct RangeStrState {
    char *buf;
    size_t len, cap;
    lxb_dom_node_t *cursor, *root;
} RangeStrState;

typedef struct RangeDelState {
    lxb_dom_node_t  *sn, *en;
    uint32_t         so, eo;
    lxb_dom_node_t  *cursor, *root;
    lxb_dom_node_t **list;
    int              n, cap, i;
} RangeDelState;

extern const char *const RANGE_STR_STEPS[];
extern const char *const RANGE_DEL_STEPS[];

void range_str_visit(JSContext *ctx, void *st, JSStepVisit *v);
void range_del_visit(JSContext *ctx, void *st, JSStepVisit *v);

/* ONE step of §5.5's stringification / of §5.5's deleteContents, over `b`. `b` is re-resolved by the CALLER at
   every entry rather than being remembered on the state, because the record time-travels: a context switch
   between two steps swaps the delta the bounds live in. Returns the JS_STEP_* the member returns. */
int range_str_step(JSContext *ctx, JSStepHdr *hdr, RangeStrState *s, RangeBounds *b, JSValue *presult);
int range_del_step(JSContext *ctx, JSStepHdr *hdr, RangeDelState *s, RangeBounds *b, JSValue *presult);

#endif
