/* DOM §4.2.3 "Mutation algorithms"' replace and replace all, in the standard's step order. Every removal is the
 * remove machine's and every insert the insert machine's, so each rests per node of its walks; these add the
 * steps between them and the one record each algorithm queues. Nothing here runs the page's code. */
#include "core/dom/node_replace.h"

#include <string.h>

#include "check.h"
#include "core/dom/document.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/mutation_record.h"
#include "core/dom/node.h"

/* Parent's node document: a Document is its own. */
static lxb_dom_document_t *node_replace_document(lxb_dom_node_t *parent)
{
    return parent->type == LXB_DOM_NODE_TYPE_DOCUMENT ? lxb_dom_interface_document(parent)
                                                      : parent->owner_document;
}

/* A one-node list of wrappers, owned. */
static JSValue node_replace_list1(JSContext *ctx, lxb_dom_node_t *n)
{
    JSValue list = JS_NewArray(ctx);

    CHECK(!JS_IsException(list), "a tree mutation record's node list could not be allocated");
    JS_SetPropertyUint32(ctx, list, 0, node_wrap(ctx, n));
    return list;
}

/* Insert's nodes for a record: the kept fragment's, or « node ». Owned; JS_UNDEFINED when there are none. */
static JSValue node_replace_added(JSContext *ctx, NodeInsertState *in, lxb_dom_node_t *node)
{
    if (node_is_document_fragment(node)) return node_insert_take_nodes(in);
    return node_replace_list1(ctx, node);
}

void node_replace_start(JSStepHdr *hdr, NodeReplaceState *s, lxb_dom_node_t *node, lxb_dom_node_t *child,
                        lxb_dom_node_t *parent, int base, int after)
{
    DCHECK(node != NULL && child != NULL && child->parent == parent,
           "§4.2.3 replace was started with no node, or a child that is not parent's — step 1's validity throws "
           "NotFoundError for that, and the caller runs it");
    DCHECK(s->parent == NULL, "a second §4.2.3 replace was started on one state");
    memset(s, 0, sizeof *s);
    s->removed = JS_UNDEFINED;
    s->node = node;
    s->child = child;
    s->parent = parent;
    s->after = after;
    s->observed = mutation_observer_any();
    s->ref = child->next;                                                      /* STEP 2 */
    if (s->ref == node) s->ref = node->next;                                   /* STEP 3 */
    s->prev = child->prev;                                                     /* STEP 4 */
    if (node->parent) {                                                        /* STEP 6: adopt step 2 */
        node_remove_start(hdr, &s->rm, node, /*suppress_observers*/ false, base + NODE_REPLACE_PHASE_RM_RANGES,
                          base + NODE_REPLACE_PHASE_ADOPT);
        return;
    }
    hdr->stage = base + NODE_REPLACE_PHASE_ADOPT;
}

int node_replace_run(JSContext *ctx, JSStepHdr *hdr, NodeReplaceState *s, int base)
{
    int phase = hdr->stage - base;

    DCHECK(s->parent != NULL, "§4.2.3 replace was resumed before it was started");
    if (phase >= NODE_REPLACE_PHASE_RM_RANGES && phase < NODE_REPLACE_PHASE_RM_RANGES + NODE_REMOVE_PHASE_N)
        return node_remove_run(ctx, hdr, &s->rm, base + NODE_REPLACE_PHASE_RM_RANGES);
    if (phase >= NODE_REPLACE_PHASE_IN_TAKE && phase < NODE_REPLACE_PHASE_IN_TAKE + NODE_INSERT_PHASE_N)
        return node_insert_run(ctx, hdr, &s->in, base + NODE_REPLACE_PHASE_IN_TAKE);
    switch (phase) {
    case NODE_REPLACE_PHASE_ADOPT:
        /* Named residual. Not covered: adopt's step 3 walks node's shadow-including inclusive descendants inside
           this one step when node comes from another document. Next: that walk one node per step, held in this
           state, shared with the insert machine's step 7.1. Absence shows as a cross-document replaceChild of a
           large subtree whose step overruns a quantum while no DOM-walk work is charged for it. */
        node_adopt_steps(document_realm_of(s->parent), s->node, node_replace_document(s->parent));   /* STEP 6 */
        if (s->child->parent) {                                                /* STEP 7 */
            if (s->observed) s->removed = node_replace_list1(ctx, s->child);  /* STEP 7.1 */
            node_remove_start(hdr, &s->rm, s->child, /*suppress_observers*/ true,
                              base + NODE_REPLACE_PHASE_RM_RANGES, base + NODE_REPLACE_PHASE_INSERT);
            return JS_STEP_YIELD;
        }
        hdr->stage = base + NODE_REPLACE_PHASE_INSERT;
        return JS_STEP_YIELD;

    case NODE_REPLACE_PHASE_INSERT:                                            /* STEPS 8-9 */
        node_insert_start(hdr, &s->in, s->node, s->parent, s->ref, /*suppress_observers*/ true,
                          base + NODE_REPLACE_PHASE_IN_TAKE, base + NODE_REPLACE_PHASE_RECORD);
        node_insert_keep_nodes(&s->in);
        return JS_STEP_YIELD;

    case NODE_REPLACE_PHASE_RECORD:                                            /* STEP 10 */
        if (s->observed) {
            JSValue added = node_replace_added(ctx, &s->in, s->node);

            mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, s->parent, NULL, NULL, NULL, 0, added,
                                           s->removed, s->prev, s->ref);
            JS_FreeValue(ctx, added);
        }
        JS_FreeValue(ctx, s->removed);
        s->removed = JS_UNDEFINED;
        hdr->stage = s->after;
        return JS_STEP_YIELD;

    default:
        DFAILF("§4.2.3 replace was resumed at phase %d, outside the block its caller declared for it", phase);
        return JS_STEP_ABRUPT;
    }
}

void node_replace_visit(JSContext *ctx, NodeReplaceState *s, JSStepVisit *v)
{
    v->val(ctx, &s->removed);
    node_insert_visit(ctx, &s->in, v);
}

const char *node_replace_script_forbidden(int stage, int base)
{
    if (stage < base || stage >= base + NODE_REPLACE_PHASE_N) return NULL;
    return "DOM §4.2.3 replace runs no page code between its steps 6 and 10: its insert's post-connection steps "
           "run only after the record is queued";
}

void node_replace_all_start(JSStepHdr *hdr, NodeReplaceAllState *s, lxb_dom_node_t *node, lxb_dom_node_t *parent,
                            int base, int after)
{
    DCHECK(parent != NULL, "§4.2.3 replace all was started with no parent");
    DCHECK(s->parent == NULL, "a second §4.2.3 replace all was started on one state");
    memset(s, 0, sizeof *s);
    s->removed = JS_UNDEFINED;
    s->node = node;
    s->parent = parent;
    s->after = after;
    s->observed = mutation_observer_any();
    hdr->stage = base + NODE_REPLACE_ALL_PHASE_NEXT;
}

int node_replace_all_run(JSContext *ctx, JSStepHdr *hdr, NodeReplaceAllState *s, int base)
{
    int phase = hdr->stage - base;

    DCHECK(s->parent != NULL, "§4.2.3 replace all was resumed before it was started");
    if (phase >= NODE_REPLACE_ALL_PHASE_RM_RANGES && phase < NODE_REPLACE_ALL_PHASE_RM_RANGES + NODE_REMOVE_PHASE_N)
        return node_remove_run(ctx, hdr, &s->rm, base + NODE_REPLACE_ALL_PHASE_RM_RANGES);
    if (phase >= NODE_REPLACE_ALL_PHASE_IN_TAKE && phase < NODE_REPLACE_ALL_PHASE_IN_TAKE + NODE_INSERT_PHASE_N)
        return node_insert_run(ctx, hdr, &s->in, base + NODE_REPLACE_ALL_PHASE_IN_TAKE);
    switch (phase) {
    case NODE_REPLACE_ALL_PHASE_NEXT: {
        lxb_dom_node_t *c = s->parent->first_child;

        if (c) {                                                               /* STEPS 1 and 5 */
            if (s->observed) {
                if (JS_IsUndefined(s->removed)) {
                    s->removed = JS_NewArray(ctx);
                    CHECK(!JS_IsException(s->removed), "replace all's removedNodes could not be allocated");
                }
                JS_SetPropertyUint32(ctx, s->removed, s->nremoved++, node_wrap(ctx, c));
            }
            node_remove_start(hdr, &s->rm, c, /*suppress_observers*/ true, base + NODE_REPLACE_ALL_PHASE_RM_RANGES,
                              base + NODE_REPLACE_ALL_PHASE_NEXT);
            return JS_STEP_YIELD;
        }
        if (s->node) {                                                         /* STEP 6 */
            node_insert_start(hdr, &s->in, s->node, s->parent, NULL, /*suppress_observers*/ true,
                              base + NODE_REPLACE_ALL_PHASE_IN_TAKE, base + NODE_REPLACE_ALL_PHASE_RECORD);
            node_insert_keep_nodes(&s->in);
            return JS_STEP_YIELD;
        }
        hdr->stage = base + NODE_REPLACE_ALL_PHASE_RECORD;
        return JS_STEP_YIELD;
    }

    case NODE_REPLACE_ALL_PHASE_RECORD: {
        /* Steps 2-4's addedNodes are the fragment's children as insert took them, which step 5 cannot change:
           parent is not node, since validity refuses a node into its own inclusive descendant. */
        JSValue added = s->node && s->observed ? node_replace_added(ctx, &s->in, s->node) : JS_UNDEFINED;

        if (JS_IsObject(added) || s->nremoved)                                 /* STEP 7 */
            mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, s->parent, NULL, NULL, NULL, 0, added,
                                           s->removed, NULL, NULL);
        JS_FreeValue(ctx, added);
        JS_FreeValue(ctx, s->removed);
        s->removed = JS_UNDEFINED;
        hdr->stage = s->after;
        return JS_STEP_YIELD;
    }

    default:
        DFAILF("§4.2.3 replace all was resumed at phase %d, outside the block its caller declared for it", phase);
        return JS_STEP_ABRUPT;
    }
}

void node_replace_all_visit(JSContext *ctx, NodeReplaceAllState *s, JSStepVisit *v)
{
    v->val(ctx, &s->removed);
    node_insert_visit(ctx, &s->in, v);
}

const char *node_replace_all_script_forbidden(int stage, int base)
{
    if (stage < base || stage >= base + NODE_REPLACE_ALL_PHASE_N) return NULL;
    return "DOM §4.2.3 replace all runs no page code between its steps 5 and 7: its insert's post-connection "
           "steps run only after the record is queued";
}
