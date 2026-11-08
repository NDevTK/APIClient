/* DOM §4.2.3 "Mutation algorithms"' insert, in the standard's step order, resting at every node of each walk.
 *
 * A DocumentFragment's children are taken into nodes one per step, each removed through the remove machine with
 * suppressObservers (step 4.1), and step 7 then runs once per node of nodes.
 *
 * Steps 7.6 and 7.7 and the name index's upkeep of steps 7.2-7.3 are walks of the inserted subtree or its root,
 * which is the page's size, so each takes one node per entry and charges it as THREAD_WORK_DOM_WALK. The link
 * walks no index and the upkeep runs after it, one element per step (solver/dom_cow.h's primitives), so every
 * captured entry is replayed by a context switch over the tree it was recorded against, and a flow switched in
 * mid-insert sees its own tree and its own index. Nothing here runs the page's code: step 7.7.3 enqueues its
 * reactions, and steps 11-12 are the member's drain, over the batch step 8's stage records. */
#include "core/dom/node_insert.h"

#include <string.h>

#include "check.h"
#include "solver/dom_cow.h"
#include "core/dom/document.h"
#include "core/dom/document_name_index.h"
#include "core/dom/element.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/mutation_record.h"
#include "core/dom/node.h"
#include "core/dom/range.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/html/custom_elements.h"
#include "core/html/html_base_element.h"
#include "core/timing/thread_work.h"

void node_insert_start(JSStepHdr *hdr, NodeInsertState *s, lxb_dom_node_t *node, lxb_dom_node_t *parent,
                       lxb_dom_node_t *child, bool suppress_observers, int base, int after)
{
    DCHECK(node != NULL && parent != NULL, "§4.2.3 insert was started with no node or no parent");
    DCHECK(child == NULL || (child->parent == parent && child != node),
           "§4.2.3 insert was started before a child of another parent, or before node itself — pre-insert's "
           "steps 2-3 are what make the reference child node's next sibling");
    DCHECK(s->parent == NULL || (s->done && s->nodes == NULL && !JS_IsObject(s->nodes_js)),
           "a second §4.2.3 insert was started while one was still walking, or before its kept nodes were taken");
    memset(s, 0, sizeof *s);
    s->nodes_js = JS_UNDEFINED;
    s->parent = parent;
    s->child = child;
    s->after = after;
    s->suppress_observers = suppress_observers;
    s->observed = mutation_observer_any();
    if (node_is_document_fragment(node)) {                                     /* STEP 1 */
        s->frag = node;
        hdr->stage = base + NODE_INSERT_PHASE_TAKE;
        return;
    }
    s->node = node;
    s->count = 1;                                                              /* STEP 2 */
    hdr->stage = base + NODE_INSERT_PHASE_RANGES;
}

bool node_pre_insert_start(JSContext *ctx, JSStepHdr *hdr, NodeInsertState *s, lxb_dom_node_t *node,
                           lxb_dom_node_t *parent, lxb_dom_node_t *child, int base, int after)
{
    if (!node_ensure_pre_insert_valid(ctx, node, parent, child, NULL)) return false;   /* STEP 1 */
    if (child == node) child = node->next;                                              /* STEPS 2-3 */
    node_insert_start(hdr, s, node, parent, child, false, base, after);                 /* STEP 4 */
    return true;
}

void node_insert_keep_nodes(NodeInsertState *s)
{
    DCHECK(s->parent != NULL && !s->done, "an insert's nodes were kept for one that is not running");
    s->keep_nodes = 1;
}

JSValue node_insert_take_nodes(NodeInsertState *s)
{
    JSValue v = s->nodes_js;

    DCHECK(s->done && s->keep_nodes, "an insert's nodes were taken before it finished, or were not kept");
    s->nodes_js = JS_UNDEFINED;
    return v;
}

void node_insert_visit(JSContext *ctx, NodeInsertState *s, JSStepVisit *v)
{
    v->buf(ctx, (void **)&s->nodes, sizeof(*s->nodes) * s->cap);
    v->val(ctx, &s->nodes_js);
}

/* Step 1's nodes for a fragment, appended as step 4.1 takes each child out. */
static void node_insert_take(JSContext *ctx, NodeInsertState *s, lxb_dom_node_t *c)
{
    if (s->count == s->cap) {
        uint32_t want = s->cap ? s->cap * 2 : 8;
        lxb_dom_node_t **n = js_realloc(ctx, s->nodes, sizeof(*n) * want);

        CHECK(n != NULL, "§4.2.3 insert could not grow its list of a fragment's children");
        s->nodes = n;
        s->cap = want;
    }
    s->nodes[s->count] = c;
    if (s->observed) {
        if (JS_IsUndefined(s->nodes_js)) {
            s->nodes_js = JS_NewArray(ctx);
            CHECK(!JS_IsException(s->nodes_js), "§4.2.3 insert's node list could not be allocated");
        }
        JS_SetPropertyUint32(ctx, s->nodes_js, s->count, node_wrap(ctx, c));
    }
    s->count++;
}

/* The insert is over: what the state owns is released, and the member resumes at `after`. */
static int node_insert_finish(JSContext *ctx, JSStepHdr *hdr, NodeInsertState *s)
{
    js_free(ctx, s->nodes);
    s->nodes = NULL;
    s->cap = 0;
    if (!s->keep_nodes) {
        JS_FreeValue(ctx, s->nodes_js);
        s->nodes_js = JS_UNDEFINED;
    }
    s->done = 1;
    hdr->stage = s->after;
    return JS_STEP_YIELD;
}

/* Parent's node document: a Document is its own. */
static lxb_dom_document_t *node_insert_document(lxb_dom_node_t *parent)
{
    return parent->type == LXB_DOM_NODE_TYPE_DOCUMENT ? lxb_dom_interface_document(parent)
                                                      : parent->owner_document;
}

int node_insert_run(JSContext *ctx, JSStepHdr *hdr, NodeInsertState *s, int base)
{
    int phase = hdr->stage - base;
    JSContext *realm;

    DCHECK(s->parent != NULL, "§4.2.3 insert was resumed before it was started");
    if (phase >= NODE_INSERT_PHASE_RM_RANGES && phase < NODE_INSERT_PHASE_RM_RANGES + NODE_REMOVE_PHASE_N)
        return node_remove_run(ctx, hdr, &s->rm, base + NODE_INSERT_PHASE_RM_RANGES);
    switch (phase) {
    case NODE_INSERT_PHASE_TAKE: {
        lxb_dom_node_t *c = s->frag->first_child;

        if (c) {                                                               /* STEP 4.1 */
            node_insert_take(ctx, s, c);
            thread_work_charge(THREAD_WORK_DOM_WALK, 1);
            node_remove_start(hdr, &s->rm, c, /*suppress_observers*/ true, base + NODE_INSERT_PHASE_RM_RANGES,
                              base + NODE_INSERT_PHASE_TAKE);
            return JS_STEP_YIELD;
        }
        if (s->count == 0) return node_insert_finish(ctx, hdr, s);            /* STEP 3 */
        if (s->observed)                                                       /* STEP 4.2 */
            mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, s->frag, NULL, NULL, NULL, 0, JS_UNDEFINED,
                                           s->nodes_js, NULL, NULL);
        hdr->stage = base + NODE_INSERT_PHASE_RANGES;
        return JS_STEP_YIELD;
    }

    case NODE_INSERT_PHASE_RANGES:
        if (s->child) range_insert_steps(ctx, s->parent, s->child, s->count);  /* STEP 5 */
        /* Step 6 binds previousSibling before step 7.1's adopt removes node, so when node is child's previous
           sibling (parent's last child for a null child) step 8's record names node itself. Chrome 153 names
           the sibling left after the removal, the order the standard had while pre-insert adopted before
           insert; whatwg/dom#1488 restored that order for replace only (whatwg/dom#814). */
        s->prev = s->child ? s->child->prev : s->parent->last_child;           /* STEP 6 */
        s->at = 0;
        hdr->stage = base + NODE_INSERT_PHASE_NODE;
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_NODE:                                               /* STEP 7 */
        if (s->frag) s->node = s->nodes[s->at];
        if (s->node->parent) {                                                 /* STEP 7.1: adopt step 2 */
            node_remove_start(hdr, &s->rm, s->node, /*suppress_observers*/ false,
                              base + NODE_INSERT_PHASE_RM_RANGES, base + NODE_INSERT_PHASE_ADOPT);
            return JS_STEP_YIELD;
        }
        hdr->stage = base + NODE_INSERT_PHASE_ADOPT;
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_ADOPT:
        /* Named residual. Not covered: adopt's step 3 walks node's shadow-including inclusive descendants inside
           this one step when node comes from another document. Next: that walk one node per step over the
           adopt frames, held in this state. Absence shows as a cross-document appendChild of a large subtree
           whose step overruns a quantum while no DOM-walk work is charged for it. */
        node_adopt_steps(document_realm_of(s->parent), s->node, node_insert_document(s->parent));
        DCHECK(s->child == NULL || s->child->parent == s->parent,
               "§4.2.3 insert's reference child left parent during step 7.1 — adopt removes node, and pre-insert "
               "made child node's next sibling when it was node");
        hdr->stage = base + NODE_INSERT_PHASE_LINK;
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_LINK:
        dom_cow_insert_link(s->parent, s->node, s->child);                     /* STEPS 7.2-7.3 */
        s->index_doc = document_name_index_tree_document(s->node);
        s->cursor = s->index_doc ? s->node : NULL;
        s->connected = node_is_connected(s->node);
        s->realm_steps = s->connected && document_realm_of(s->node) != NULL;
        hdr->stage = base + NODE_INSERT_PHASE_INDEX;
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_INDEX:
        if (s->cursor) {
            DCHECK(s->node->parent == s->parent, "§4.2.3 insert's subtree left its parent during the index upkeep");
            if (s->cursor->type == LXB_DOM_NODE_TYPE_ELEMENT)
                dom_cow_insert_index_element(s->index_doc, lxb_dom_interface_element(s->cursor));
            thread_work_charge(THREAD_WORK_DOM_WALK, 1);
            s->cursor = node_next_in(s->cursor, s->node);
            return JS_STEP_YIELD;
        }
        hdr->stage = base + NODE_INSERT_PHASE_ASSIGNED;
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_ASSIGNED:
        slot_inserted_assigned_steps(ctx, s->node, s->parent);                  /* STEPS 7.4-7.5 */
        if (slot_inserted_tree_steps_apply(s->node)) {
            s->tree_root = node_root(s->node);
            s->cursor = s->tree_root;
            hdr->stage = base + NODE_INSERT_PHASE_SLOT_TREE;
            return JS_STEP_YIELD;
        }
        s->cursor = s->node;
        hdr->stage = base + NODE_INSERT_PHASE_DESCENDANTS;
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_SLOT_TREE:                                           /* STEP 7.6 */
        if (s->cursor) {
            slot_assign_tree_node(ctx, s->cursor);
            s->cursor = node_next_in(s->cursor, s->tree_root);
            return JS_STEP_YIELD;
        }
        s->cursor = s->node;
        hdr->stage = base + NODE_INSERT_PHASE_DESCENDANTS;
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_DESCENDANTS:                                         /* STEP 7.7 */
        if (s->cursor) {
            lxb_dom_node_t *n = s->cursor;

            realm = document_realm_of(n);
            if (s->connected) {
                html_base_element_insertion_steps(n);                           /* STEP 7.7.1 */
                if (s->realm_steps) element_insertion_steps(n);
                /* STEP 7.7.2 is s->connected: every node of the walk is a shadow-including inclusive descendant
                   of node, so it shares node's answer. */
                if (realm && n->type == LXB_DOM_NODE_TYPE_ELEMENT)
                    custom_elements_element_connected(realm, lxb_dom_interface_element(n));   /* STEP 7.7.3 */
            }
            thread_work_charge(THREAD_WORK_DOM_WALK, 1);
            s->cursor = shadow_root_next_in_shadow_including(realm ? realm : ctx, n, s->node);
            return JS_STEP_YIELD;
        }
        /* Steps 10-12 are the member's drain, which takes this node's entry at the step's end; the stages forbid
           script, so it collects staticNodeList in nodes' order and runs step 12 once the member leaves them. */
        element_tree_steps_record_insert(s->node);
        hdr->stage = base + (++s->at < s->count ? NODE_INSERT_PHASE_NODE : NODE_INSERT_PHASE_RECORD);
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_RECORD:
        if (!s->suppress_observers) {                                           /* STEP 8 */
            if (!s->frag)
                mutation_observer_insertion_record(ctx, s->node, s->parent, s->prev, s->child);
            else if (s->observed)
                mutation_observer_queue_record(ctx, MR_TYPE_CHILD_LIST, s->parent, NULL, NULL, NULL, 0,
                                               s->nodes_js, JS_UNDEFINED, s->prev, s->child);
        }
        hdr->stage = base + NODE_INSERT_PHASE_CHANGED;
        return JS_STEP_YIELD;

    case NODE_INSERT_PHASE_CHANGED:
        node_children_changed(ctx, s->parent);                                  /* STEP 9 */
        return node_insert_finish(ctx, hdr, s);

    default:
        DFAILF("§4.2.3 insert was resumed at phase %d, outside the block its caller declared for it", phase);
        return JS_STEP_ABRUPT;
    }
}

const char *node_insert_script_forbidden(int stage, int base)
{
    if (stage < base || stage >= base + NODE_INSERT_PHASE_N) return NULL;
    return "DOM §4.2.3 insert runs no page code between its steps 1 and 9: step 7.7.3 enqueues its custom element "
           "reactions for the member's [CEReactions] epilogue, and step 12's post-connection steps run only "
           "after every node is inserted";
}
