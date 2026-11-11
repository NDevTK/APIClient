/* DOM §4.2.3 "Mutation algorithms"' insert, as a step machine that rests per node — see node_insert.c.
 *
 * A member owes it a NodeInsertState in its state block, visited by node_insert_visit, NODE_INSERT_ALGO_STAGES in
 * its stage list (one block of NODE_INSERT_PHASE_N stages at `base`), and a stage `after` it resumes at once
 * steps 1-9 are done; steps 10-12 are the member's drain. Its stages must forbid script
 * (IdlStepDecl.script_forbidden, answered by node_insert_script_forbidden), so the drain holds the batch the
 * insert records and runs step 12 only once the member leaves them. The state owns `nodes` and `nodes_js`; the
 * nodes themselves are the documents', and the member's own header holds node's wrapper. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_INSERT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_INSERT_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/node_adopt.h"
#include "core/dom/node_remove.h"

#define NODE_INSERT_ALGO_STAGES(X, P, W) \
    X(P##_TAKE,        W " → DOM §4.2.3 insert steps 1-4 (a DocumentFragment's children into nodes, one per " \
                         "step, each removed with suppressObservers; the fragment's record)") \
    NODE_REMOVE_ALGO_STAGES(X, P##_RM, W " → DOM §4.2.3 insert step 4.1, or step 7.1 → DOM §4.5 adopt step 2") \
    X(P##_RANGES,      W " → DOM §4.2.3 insert step 5 (the live-range steps for child)") \
    X(P##_NODE,        W " → DOM §4.2.3 insert step 7 (the next node of nodes; step 7.1's adopt step 2)") \
    X(P##_ADOPT,       W " → DOM §4.2.3 insert step 7.1 (DOM §4.5 adopt step 1 and step 3's condition)") \
    X(P##_ADOPT_WALK,  W " → DOM §4.2.3 insert step 7.1 (DOM §4.5 adopt step 3, one shadow-including inclusive " \
                         "descendant per step)") \
    X(P##_LINK,        W " → DOM §4.2.3 insert steps 6 and 7.2-7.3 (previousSibling; node into parent's " \
                         "children)") \
    X(P##_INDEX,       W " → DOM §4.2.3 insert steps 7.2-7.3 (the name index's upkeep, one node of node's " \
                         "subtree per step)") \
    X(P##_ASSIGNED,    W " → DOM §4.2.3 insert steps 7.4-7.5 (assign a slot for node; parent's slot change)") \
    X(P##_SLOT_TREE,   W " → DOM §4.2.3 insert step 7.6 (assign slottables for a tree with node's root, one node " \
                         "of it per step)") \
    X(P##_DESCENDANTS, W " → DOM §4.2.3 insert step 7.7 (one shadow-including inclusive descendant per step: its " \
                         "insertion steps and connectedCallback; then the next node of nodes)") \
    X(P##_RECORD,      W " → DOM §4.2.3 insert step 8 (the tree mutation record)") \
    X(P##_CHANGED,     W " → DOM §4.2.3 insert step 9 (the children changed steps for parent)")

enum { NODE_INSERT_ALGO_STAGES(JS_STEP_STAGE_ENUM, NODE_INSERT_PHASE, "") NODE_INSERT_PHASE_N };

typedef struct NodeInsertState {
    lxb_dom_node_t *node, *parent, *child;  /* step 7's node of nodes, parent and child; child may be NULL */
    lxb_dom_node_t *frag;                   /* insert's node when it is a DocumentFragment, or NULL */
    lxb_dom_node_t **nodes;                 /* step 1's nodes for a fragment, `count` of `cap`; owned */
    JSValue nodes_js;                       /* ...as wrappers while an observer exists, for the two records; owned */
    uint32_t count, cap, at;                /* step 2's count; step 7's position in nodes */
    lxb_dom_node_t *prev;                   /* step 6's previousSibling */
    lxb_dom_node_t *cursor;                 /* the walk of the stage standing */
    lxb_dom_node_t *tree_root;              /* step 7.6's root, node's root */
    lxb_dom_document_t *index_doc;          /* the document whose tree node is in, or NULL */
    NodeRemoveState rm;                     /* step 4.1, and step 7.1's adopt step 2 when node has a parent */
    NodeAdoptState adopt;                   /* step 7.1's adopt steps 1 and 3 */
    int after;
    uint8_t connected;                      /* node is connected after steps 7.2-7.3 (step 7.7.2) */
    uint8_t realm_steps;                    /* ...in a document with a realm, so HTML's insertion steps run */
    uint8_t suppress_observers;             /* insert's suppressObservers: step 8 queues no record */
    uint8_t observed;                       /* a mutation observer exists, so the records have wrappers to name */
    uint8_t keep_nodes;                     /* nodes_js outlives the insert, for the caller's record */
    uint8_t done;                           /* step 9 is done */
} NodeInsertState;

/* Begin inserting `node` into `parent` before `child` (NULL: append), with insert's suppressObservers; the
   caller's next entry is at `base`. Asserts pre-insert's steps 2-3 ran: child is NULL or parent's child and is not
   node. `s` is zeroed or holds a finished insert. */
void node_insert_start(JSStepHdr *hdr, NodeInsertState *s, lxb_dom_node_t *node, lxb_dom_node_t *parent,
                       lxb_dom_node_t *child, bool suppress_observers, int base, int after);
/* One stage of the insert; JS_STEP_YIELD at every rest, with hdr->stage at `after` once step 9 is done. It makes
   no request and cannot throw. */
int node_insert_run(JSContext *ctx, JSStepHdr *hdr, NodeInsertState *s, int base);
/* DOM §4.2.3 pre-insert: steps 1-3 (ensure pre-insert validity, which throws; referenceChild), then step 4's
   insert started as node_insert_start does, without suppressObservers. Returns false having thrown, with nothing
   started. Step 5's return is the caller's. */
bool node_pre_insert_start(JSContext *ctx, JSStepHdr *hdr, NodeInsertState *s, lxb_dom_node_t *node,
                           lxb_dom_node_t *parent, lxb_dom_node_t *child, int base, int after);
/* Keep a fragment's nodes as wrappers past the insert, for a caller whose own record names them (replace step
   10, replace all step 7); called right after node_insert_start. */
void node_insert_keep_nodes(NodeInsertState *s);
/* The kept nodes of a finished insert of a DocumentFragment while an observer exists, owned by the caller, or
   JS_UNDEFINED. */
JSValue node_insert_take_nodes(NodeInsertState *s);
/* What the state owns, for the member's visit. */
void node_insert_visit(JSContext *ctx, NodeInsertState *s, JSStepVisit *v);
/* Why the page's code must not run at `stage`, or NULL: every stage of the block at `base`. */
const char *node_insert_script_forbidden(int stage, int base);

#endif
