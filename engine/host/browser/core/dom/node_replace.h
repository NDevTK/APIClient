/* DOM §4.2.3 "Mutation algorithms"' replace and replace all, as step machines over the remove and insert
 * machines — see node_replace.c.
 *
 * A member owes each a state in its state block, visited by its visit function, its stage block in its stage
 * list (NODE_REPLACE_PHASE_N or NODE_REPLACE_ALL_PHASE_N stages at `base`), and a stage `after` it resumes at once
 * the algorithm is done. Its stages must forbid script (node_replace_script_forbidden,
 * node_replace_all_script_forbidden), as the insert machine's do. The state owns its record's node lists; the
 * nodes are the documents'. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_NODE_REPLACE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_NODE_REPLACE_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/node_insert.h"
#include "core/dom/node_remove.h"

#define NODE_REPLACE_ALGO_STAGES(X, P, W) \
    NODE_REMOVE_ALGO_STAGES(X, P##_RM, W " → DOM §4.2.3 replace step 6 (DOM §4.5 adopt step 2), or step 7.2 " \
                                       "(remove child, with suppressObservers)") \
    X(P##_ADOPT,  W " → DOM §4.2.3 replace steps 6-7 (adopt steps 1 and 3; removedNodes)") \
    X(P##_INSERT, W " → DOM §4.2.3 replace steps 8-9 (insert node before referenceChild, with suppressObservers)") \
    NODE_INSERT_ALGO_STAGES(X, P##_IN, W " → DOM §4.2.3 replace step 9") \
    X(P##_RECORD, W " → DOM §4.2.3 replace step 10 (the tree mutation record)")

enum { NODE_REPLACE_ALGO_STAGES(JS_STEP_STAGE_ENUM, NODE_REPLACE_PHASE, "") NODE_REPLACE_PHASE_N };

typedef struct NodeReplaceState {
    lxb_dom_node_t *node, *child, *parent;
    lxb_dom_node_t *ref, *prev;    /* steps 2-3's referenceChild; step 4's previousSibling */
    NodeRemoveState rm;
    NodeInsertState in;
    JSValue removed;               /* step 7.1's removedNodes as wrappers while an observer exists; owned */
    int after;
    uint8_t observed;
} NodeReplaceState;

/* Begin replacing `child` with `node` within `parent`; the caller's next entry is at `base`. The caller ran step 1
   (ensure pre-insert validity, which throws); asserts child is parent's child. `s` is zeroed. */
void node_replace_start(JSStepHdr *hdr, NodeReplaceState *s, lxb_dom_node_t *node, lxb_dom_node_t *child,
                        lxb_dom_node_t *parent, int base, int after);
/* One stage; JS_STEP_YIELD at every rest, with hdr->stage at `after` once step 10 is done. Cannot throw. */
int node_replace_run(JSContext *ctx, JSStepHdr *hdr, NodeReplaceState *s, int base);
void node_replace_visit(JSContext *ctx, NodeReplaceState *s, JSStepVisit *v);
const char *node_replace_script_forbidden(int stage, int base);

#define NODE_REPLACE_ALL_ALGO_STAGES(X, P, W) \
    NODE_REMOVE_ALGO_STAGES(X, P##_RM, W " → DOM §4.2.3 replace all step 5 (remove a child, with " \
                                       "suppressObservers)") \
    X(P##_NEXT,   W " → DOM §4.2.3 replace all steps 1 and 5 (removedNodes; parent's next child to remove)") \
    NODE_INSERT_ALGO_STAGES(X, P##_IN, W " → DOM §4.2.3 replace all step 6 (insert node, with " \
                                       "suppressObservers)") \
    X(P##_RECORD, W " → DOM §4.2.3 replace all steps 2-4 and 7 (addedNodes; the tree mutation record)")

enum { NODE_REPLACE_ALL_ALGO_STAGES(JS_STEP_STAGE_ENUM, NODE_REPLACE_ALL_PHASE, "") NODE_REPLACE_ALL_PHASE_N };

typedef struct NodeReplaceAllState {
    lxb_dom_node_t *node, *parent;  /* node may be NULL */
    NodeRemoveState rm;
    NodeInsertState in;
    JSValue removed;                /* step 1's removedNodes as wrappers while an observer exists; owned */
    uint32_t nremoved;
    int after;
    uint8_t observed;
} NodeReplaceAllState;

/* Begin replacing all of `parent`'s children with `node` (NULL: none); the caller's next entry is at `base`. The
   caller ran whatever validity its own steps state. `s` is zeroed. */
void node_replace_all_start(JSStepHdr *hdr, NodeReplaceAllState *s, lxb_dom_node_t *node, lxb_dom_node_t *parent,
                            int base, int after);
/* One stage; JS_STEP_YIELD at every rest, with hdr->stage at `after` once step 7 is done. Cannot throw. */
int node_replace_all_run(JSContext *ctx, JSStepHdr *hdr, NodeReplaceAllState *s, int base);
void node_replace_all_visit(JSContext *ctx, NodeReplaceAllState *s, JSStepVisit *v);
const char *node_replace_all_script_forbidden(int stage, int base);

#endif
