/* DOM §4.2.3 "Mutation algorithms"' remove, as a step machine that rests per node — see node_remove.c.
 *
 * A member owes it a NodeRemoveState in its state block, NODE_REMOVE_ALGO_STAGES in its stage list (one block of
 * NODE_REMOVE_PHASE_N stages at `base`), and a stage `after` it resumes at once the removal is done. Nothing the
 * state names is owned: the nodes are the document's, and the member's own header holds the wrappers. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_REMOVE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_REMOVE_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"

#define NODE_REMOVE_ALGO_STAGES(X, P, W) \
    X(P##_RANGES,      W " → DOM §4.2.3 remove steps 1-3 (parent; the live range pre-remove steps)") \
    X(P##_ITERATORS,   W " → DOM §4.2.3 remove step 4 (the NodeIterator pre-remove steps)") \
    X(P##_DETACH,      W " → DOM §4.2.3 remove steps 5-7 (oldPreviousSibling, oldNextSibling; remove node from " \
                         "its parent's children, whose name-index upkeep takes one node of node's subtree per " \
                         "step)") \
    X(P##_ASSIGNED,    W " → DOM §4.2.3 remove steps 8-9 (node's assigned slot; parent's slot change)") \
    X(P##_SLOT_FIND,   W " → DOM §4.2.3 remove step 10 (whether node has an inclusive descendant that is a slot, " \
                         "one node per step)") \
    X(P##_SLOT_PARENT_TREE, W " → DOM §4.2.3 remove step 10.1 (assign slottables for a tree with parent's " \
                              "root, one node of it per step)") \
    X(P##_SLOT_NODE_TREE, W " → DOM §4.2.3 remove step 10.2 (assign slottables for a tree with node, one node of " \
                            "it per step)") \
    X(P##_ROOT,        W " → DOM §4.2.3 remove steps 11-13 (the removing steps with node; isParentConnected; " \
                         "node's disconnectedCallback)") \
    X(P##_DESCENDANTS, W " → DOM §4.2.3 remove step 14 (one shadow-including descendant per step: its removing " \
                         "steps and disconnectedCallback)") \
    X(P##_RECORD,      W " → DOM §4.2.3 remove steps 15-16 (transient registered observers; the tree mutation " \
                         "record)") \
    X(P##_CHANGED,     W " → DOM §4.2.3 remove step 17 (the children changed steps for parent)")

enum { NODE_REMOVE_ALGO_STAGES(JS_STEP_STAGE_ENUM, NODE_REMOVE_PHASE, "") NODE_REMOVE_PHASE_N };

typedef struct NodeRemoveState {
    lxb_dom_node_t *node, *parent;
    lxb_dom_node_t *old_prev, *old_next;   /* steps 5-6 */
    lxb_dom_node_t *cursor;                /* the walk of the stage standing */
    lxb_dom_node_t *tree_root;             /* step 10.1's root, parent's root */
    lxb_dom_document_t *index_doc;         /* the document whose tree node is in, or NULL */
    int after;
    uint8_t parent_connected;              /* step 12's isParentConnected */
    uint8_t realm_steps;                   /* parent's document has a realm, so the removing steps HTML defines run */
} NodeRemoveState;

/* Begin removing `node` from its parent; the caller's next entry is at `base`. Asserts node has a parent:
   §4.2.3 remove step 2, which every caller's own steps establish. */
void node_remove_start(JSStepHdr *hdr, NodeRemoveState *s, lxb_dom_node_t *node, int base, int after);
/* One stage of the removal; JS_STEP_YIELD at every rest, with hdr->stage at `after` once it is done. It makes
   no request and cannot throw. */
int node_remove_run(JSContext *ctx, JSStepHdr *hdr, NodeRemoveState *s, int base);

#endif
