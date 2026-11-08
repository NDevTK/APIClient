/* DOM §4.2.3 "Mutation algorithms"' insert of one node that is not a DocumentFragment, as a step machine that
 * rests per node — see node_insert.c.
 *
 * A member owes it a NodeInsertState in its state block, NODE_INSERT_ALGO_STAGES in its stage list (one block of
 * NODE_INSERT_PHASE_N stages at `base`), and a stage `after` it resumes at once steps 1-9 are done; steps 11-12
 * are the member's drain. Its stages must forbid script (IdlStepDecl.script_forbidden, answered by
 * node_insert_script_forbidden), so the drain holds the batch the insert records and runs step 12 only once the
 * member leaves them. Nothing the state names is owned: the nodes are the documents', and the member's own
 * header holds the wrappers. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_INSERT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_INSERT_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/node_remove.h"

#define NODE_INSERT_ALGO_STAGES(X, P, W) \
    X(P##_RANGES,      W " → DOM §4.2.3 insert steps 5-6 (the live-range steps for child; previousSibling)") \
    NODE_REMOVE_ALGO_STAGES(X, P##_RM, W " → DOM §4.2.3 insert step 7.1 → DOM §4.5 adopt step 2") \
    X(P##_ADOPT,       W " → DOM §4.2.3 insert step 7.1 (DOM §4.5 adopt steps 1 and 3)") \
    X(P##_LINK,        W " → DOM §4.2.3 insert steps 7.2-7.3 (node into parent's children)") \
    X(P##_INDEX,       W " → DOM §4.2.3 insert steps 7.2-7.3 (the name index's upkeep, one node of node's " \
                         "subtree per step)") \
    X(P##_ASSIGNED,    W " → DOM §4.2.3 insert steps 7.4-7.5 (assign a slot for node; parent's slot change)") \
    X(P##_SLOT_TREE,   W " → DOM §4.2.3 insert step 7.6 (assign slottables for a tree with node's root, one node " \
                         "of it per step)") \
    X(P##_DESCENDANTS, W " → DOM §4.2.3 insert step 7.7 (one shadow-including inclusive descendant per step: its " \
                         "insertion steps and connectedCallback)") \
    X(P##_RECORD,      W " → DOM §4.2.3 insert step 8 (the tree mutation record)") \
    X(P##_CHANGED,     W " → DOM §4.2.3 insert step 9 (the children changed steps for parent)")

enum { NODE_INSERT_ALGO_STAGES(JS_STEP_STAGE_ENUM, NODE_INSERT_PHASE, "") NODE_INSERT_PHASE_N };

typedef struct NodeInsertState {
    lxb_dom_node_t *node, *parent, *child;  /* insert's three operands; child may be NULL */
    lxb_dom_node_t *prev;                   /* step 6's previousSibling */
    lxb_dom_node_t *cursor;                 /* the walk of the stage standing */
    lxb_dom_node_t *tree_root;              /* step 7.6's root, node's root */
    lxb_dom_document_t *index_doc;          /* the document whose tree node is in, or NULL */
    NodeRemoveState rm;                     /* step 7.1's adopt step 2, when node has a parent */
    int after;
    uint8_t connected;                      /* node is connected after steps 7.2-7.3 (step 7.7.2) */
    uint8_t realm_steps;                    /* ...in a document with a realm, so HTML's insertion steps run */
} NodeInsertState;

/* Begin inserting `node` into `parent` before `child` (NULL: append); the caller's next entry is at `base`.
   Asserts pre-insert's steps 1-3 ran: node is not a DocumentFragment, child is NULL or parent's child and is not
   node. */
void node_insert_start(JSStepHdr *hdr, NodeInsertState *s, lxb_dom_node_t *node, lxb_dom_node_t *parent,
                       lxb_dom_node_t *child, int base, int after);
/* One stage of the insert; JS_STEP_YIELD at every rest, with hdr->stage at `after` once step 9 is done. It makes
   no request and cannot throw. */
int node_insert_run(JSContext *ctx, JSStepHdr *hdr, NodeInsertState *s, int base);
/* Why the page's code must not run at `stage`, or NULL: every stage of the block at `base`. */
const char *node_insert_script_forbidden(int stage, int base);

#endif
