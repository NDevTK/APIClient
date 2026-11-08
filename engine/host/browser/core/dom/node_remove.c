/* DOM §4.2.3 "Mutation algorithms"' remove, in the standard's step order, resting at every node of each walk.
 *
 * Steps 10 and 14 and the name index's upkeep of step 7 are walks of the removed subtree, which is the page's
 * size, so each takes one node per entry and charges it as THREAD_WORK_DOM_WALK. The index upkeep runs while the
 * subtree is still linked (solver/dom_cow.h's per-element primitive), so every captured entry is replayed by a
 * context switch over the tree it was recorded against, and a flow switched in mid-removal sees its own tree and
 * its own index. Nothing here runs the page's code. */
#include "core/dom/node_remove.h"

#include "check.h"
#include "solver/dom_cow.h"
#include "core/dom/document.h"
#include "core/dom/document_name_index.h"
#include "core/dom/element.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/node.h"
#include "core/dom/node_iterator.h"
#include "core/dom/range.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/html/custom_elements.h"
#include "core/html/html_base_element.h"
#include "core/timing/thread_work.h"

void node_remove_start(JSStepHdr *hdr, NodeRemoveState *s, lxb_dom_node_t *node, int base, int after)
{
    DCHECK(node != NULL && node->parent != NULL,
           "§4.2.3 remove was started on no node or a node with no parent — step 2 asserts the parent, and the "
           "caller's own steps are what establish it");
    DCHECK(s->node == NULL, "a second §4.2.3 remove was started while one was still walking");
    s->node = node;
    s->after = after;
    hdr->stage = base + NODE_REMOVE_PHASE_RANGES;
}

/* Steps 11 and 14.1's removing steps, and 13 and 14.2's disconnectedCallback, for one node. */
static void node_remove_node_steps(const NodeRemoveState *s, lxb_dom_node_t *n)
{
    JSContext *realm;

    if (!s->parent_connected) return;
    html_base_element_removing_steps(n);
    if (s->realm_steps) element_removing_steps(n);
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    realm = document_realm_of(n);
    if (realm) custom_elements_disconnected(realm, lxb_dom_interface_element(n));
}

int node_remove_run(JSContext *ctx, JSStepHdr *hdr, NodeRemoveState *s, int base)
{
    int phase = hdr->stage - base;

    DCHECK(s->node != NULL, "§4.2.3 remove was resumed before it was started");
    switch (phase) {
    case NODE_REMOVE_PHASE_RANGES:
        s->parent = s->node->parent;                                           /* STEP 1 */
        DCHECK(s->parent != NULL, "§4.2.3 remove step 2: node has no parent");  /* STEP 2 */
        range_pre_remove(ctx, s->node);                                        /* STEP 3 */
        hdr->stage = base + NODE_REMOVE_PHASE_ITERATORS;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_ITERATORS:
        node_iterator_pre_remove(ctx, s->node);                                /* STEP 4 */
        s->index_doc = document_name_index_tree_document(s->node);
        s->cursor = s->index_doc ? s->node : NULL;
        hdr->stage = base + NODE_REMOVE_PHASE_DETACH;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_DETACH:
        if (s->cursor) {
            DCHECK(s->node->parent == s->parent, "§4.2.3 remove's subtree left its parent before step 7");
            if (s->cursor->type == LXB_DOM_NODE_TYPE_ELEMENT)
                dom_cow_remove_index_element(s->index_doc, lxb_dom_interface_element(s->cursor));
            thread_work_charge(THREAD_WORK_DOM_WALK, 1);
            s->cursor = node_next_in(s->cursor, s->node);
            return JS_STEP_YIELD;
        }
        s->old_prev = s->node->prev;                                           /* STEP 5 */
        s->old_next = s->node->next;                                           /* STEP 6 */
        dom_cow_remove_detach(s->node);                                        /* STEP 7 */
        hdr->stage = base + NODE_REMOVE_PHASE_ASSIGNED;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_ASSIGNED:
        slot_removed_assigned_steps(ctx, s->node, s->parent);                  /* STEPS 8-9 */
        s->cursor = slot_removed_tree_steps_apply(s->parent) ? s->node : NULL;
        hdr->stage = base + NODE_REMOVE_PHASE_SLOT_FIND;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_SLOT_FIND:                                          /* STEP 10 */
        if (s->cursor && slot_is(s->cursor)) {
            s->cursor = NULL;
            hdr->stage = base + NODE_REMOVE_PHASE_SLOT_ASSIGN;
            return JS_STEP_YIELD;
        }
        if (s->cursor) {
            thread_work_charge(THREAD_WORK_DOM_WALK, 1);
            s->cursor = node_next_in(s->cursor, s->node);
            return JS_STEP_YIELD;
        }
        hdr->stage = base + NODE_REMOVE_PHASE_ROOT;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_SLOT_ASSIGN:
        slot_removed_tree_steps(ctx, s->node, s->parent);                      /* STEPS 10.1-10.2 */
        hdr->stage = base + NODE_REMOVE_PHASE_ROOT;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_ROOT:
        /* Step 12 is read before step 11 because the removing steps HTML defines are stated for a node leaving a
           connected tree, and none of them changes parent's connectedness. */
        s->parent_connected = node_is_connected(s->parent);                    /* STEP 12 */
        s->realm_steps = s->parent_connected && document_realm_of(s->parent) != NULL;
        node_remove_node_steps(s, s->node);                                    /* STEPS 11 and 13 */
        s->cursor = shadow_root_next_in_shadow_including(ctx, s->node, s->node);
        hdr->stage = base + NODE_REMOVE_PHASE_DESCENDANTS;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_DESCENDANTS:                                        /* STEP 14 */
        if (s->cursor) {
            node_remove_node_steps(s, s->cursor);                              /* STEPS 14.1-14.2 */
            thread_work_charge(THREAD_WORK_DOM_WALK, 1);
            s->cursor = shadow_root_next_in_shadow_including(ctx, s->cursor, s->node);
            return JS_STEP_YIELD;
        }
        hdr->stage = base + NODE_REMOVE_PHASE_RECORD;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_RECORD:
        mutation_observer_transient_for_removal(ctx, s->node, s->parent);     /* STEP 15 */
        mutation_observer_removal_record(ctx, s->node, s->parent, s->old_prev, s->old_next);   /* STEP 16 */
        hdr->stage = base + NODE_REMOVE_PHASE_CHANGED;
        return JS_STEP_YIELD;

    case NODE_REMOVE_PHASE_CHANGED:
        node_children_changed(ctx, s->parent);                                 /* STEP 17 */
        hdr->stage = s->after;
        return JS_STEP_YIELD;

    default:
        DFAILF("§4.2.3 remove was resumed at phase %d, outside the block its caller declared for it", phase);
        return JS_STEP_ABRUPT;
    }
}
