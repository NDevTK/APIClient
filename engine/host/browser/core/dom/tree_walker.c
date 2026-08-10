/* TREEWALKER — DOM §6.2.
 *
 * EVERY ONE OF ITS SEVEN MEMBERS IS A STEP MACHINE, and none of them could be a C loop. Each is a walk over the
 * page's tree that calls the page's own `acceptNode` at every node it considers (§6.4, in node_filter.c), so a
 * C `for` around a `JS_Call` would be the drive-to-completion this engine aborts on — the filter may loop, may
 * await, and may fork a concolic branch, and the walk has to park in the middle of it and resume at the exact
 * node it was on. That is also why the stage lists below are as long as they are: a stage is a REST POINT, and
 * §6.2's algorithms have between one and three places where the page runs.
 *
 * THE SEVEN SHARE ONE STATE AND ONE OWNERSHIP DECLARATION, five step bodies between them, because two of the
 * standard's algorithms are each written once with a `type` operand: `firstChild`/`lastChild` are "traverse
 * children" and `nextSibling`/`previousSibling` are "traverse siblings". Splitting those into four bodies would
 * be four chances for the two directions to disagree, which is exactly the divergence one algorithm with an
 * operand cannot have.
 *
 * THE ALGORITHM'S OWN LOCALS ARE RAW NODE POINTERS AND ITS `current` IS A WRAPPER, and the split is the
 * standard's. `current` is a member the page reads, writes and compares by identity, so it is the JS object;
 * `node`, `sibling` and `temporary` are the algorithm's own variables, which never escape it. A removal
 * performed BY the filter (which the corpus does, deliberately) unparents a node without destroying it, so a
 * held pointer stays valid and the walk continues from where the standard says it does.
 *
 * WHAT THIS FILE READS FRESH, EVERY TIME, IS `walker's current` AND `walker's root`. The filter is the page's
 * code and it may assign `walker.currentNode` — "traverse children" step 3.4.4 compares against walker's
 * current, and taking a copy at step 1 would compare against a node the walker no longer stands on. */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/node.h"
#include "core/dom/node_filter.h"
#include "core/dom/tree_walker.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"

/* §6.2's own state: §6's shared traverser plus "an associated current (a node)". */
typedef struct WalkerData {
    Traverser t;         /* FIRST — TRAVERSER_VALS names its slots at this struct's offsets */
    JSValue   current;   /* §6.2 current (a node wrapper; OWNED) */
} WalkerData;

static JSClassID g_walker_class;
static JSRuntime *g_walker_rt;

/* THE RECORD TIME-TRAVELS. `current` is what every member of this interface writes, and it is written where no
   property hook can see — a forked arm that called nextNode() would otherwise have advanced the walker for its
   sibling. The capture is in the ACCESSOR: a record a flow has REACHED is one it may write, so there is no
   write site left to miss. The offset list is the same list the finalizer frees. */
static const uint16_t WALKER_VALS[] = { TRAVERSER_VALS(WalkerData), (uint16_t)offsetof(WalkerData, current) };
static const CowRecord WALKER_REC = { sizeof(WalkerData), WALKER_VALS, 3 };

static WalkerData *walker_of(JSValueConst v)
{
    WalkerData *w = JS_GetOpaque(v, g_walker_class);
    if (w) cow_capture_host_record(v, w, &WALKER_REC);
    return w;
}

/* The receiver, brand-checked. Every member of §6.2 is declared on the prototype, so a page can apply one to
   anything at all and the answer is a TypeError rather than a walk of nothing. */
static WalkerData *walker_here(JSContext *ctx, JSValueConst v)
{
    WalkerData *w = walker_of(v);
    if (!w) {
        JS_ThrowTypeError(ctx, "not a TreeWalker");
        return NULL;
    }
    return w;
}

static void walker_finalizer(JSRuntime *rt, JSValue val)
{
    WalkerData *w = JS_GetOpaque(val, g_walker_class);
    if (!w) return;
    traverser_release(rt, &w->t);
    JS_FreeValueRT(rt, w->current);
    free(w);
}

static void walker_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    WalkerData *w = JS_GetOpaque(val, g_walker_class);
    if (!w) return;
    traverser_mark(rt, &w->t, mark_func);
    JS_MarkValue(rt, w->current, mark_func);
}

JSValue tree_walker_new(JSContext *ctx, JSValueConst root, uint32_t what, JSValueConst filter)
{
    JSValue obj, proto;
    WalkerData *w;

    DCHECK(g_walker_class != 0, "a TreeWalker was built before tree_walker_init declared its class");
    proto = JS_GetClassProto(ctx, g_walker_class);
    DCHECK(!JS_IsNull(proto), "a TreeWalker was built in a realm with no TreeWalker.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_walker_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    w = calloc(1, sizeof(*w));
    CHECK(w != NULL, "the TreeWalker record allocation failed");
    /* §4.5 createTreeWalker steps 2-4: root AND current are the given root. */
    w->t.root = JS_DupValue(ctx, root);
    w->t.filter = JS_DupValue(ctx, filter);
    w->t.what = what;
    w->current = JS_DupValue(ctx, root);
    JS_SetOpaque(obj, w);
    return obj;
}

/* ---- THE MACHINES' SHARED STATE ------------------------------------------------------------------------- */

/* WHAT A WALK IS PARKED ON. One struct for all five bodies, because they are five algorithms over the same
   three locals and one filter call — and because the ownership declaration is per STRUCT, so five structs
   would be five visits that could drift. */
typedef struct TreeWalkState {
    NodeFilterCall f;           /* §6.4's call, in flight */
    lxb_dom_node_t *node;       /* the algorithm's `node` */
    lxb_dom_node_t *sibling;    /* "traverse siblings"' and nextNode's `sibling` */
    lxb_dom_node_t *temp;       /* nextNode's `temporary` */
    JSValue wrap;               /* the wrapper for `node`, held ACROSS the filter call (OWNED) */
    int result;                 /* the filter's answer, which nextNode and previousNode carry between steps */
} TreeWalkState;

/* WHAT THIS MACHINE OWNS. The node pointers are not owned — the document owns every node — so the list is the
   filter call's slots and the wrapper the call is holding. */
static void tw_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    TreeWalkState *s = st;
    node_filter_call_visit(ctx, &s->f, v);
    v->val(ctx, &s->wrap);
}

static void tw_release(JSContext *ctx, void *st)
{
    TreeWalkState *s = st;
    node_filter_call_release(ctx, &s->f);
    JS_FreeValue(ctx, s->wrap);
    s->wrap = JS_UNDEFINED;
}

/* §6.4 over `s->node`. Returns >0 (park), 0 with s->result set, or -1. */
static int tw_filter(JSContext *ctx, JSStepHdr *hdr, WalkerData *w, TreeWalkState *s,
                     JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    DCHECK(s->node != NULL, "§6.2 asked to filter no node");
    /* THE WRAPPER IS TAKEN WHEN THE FILTER IS NOT ALREADY IN FLIGHT, and the condition is that rather than
       "the slot is empty" because a step state arrives ZEROED — and a zeroed JSValue is the NUMBER 0, not
       JS_UNDEFINED. Testing the slot therefore answered "already set" on the very first call and handed §6.4
       an integer, which its own assert caught. The filter's cursor is the real answer to "is this the same
       node I parked on". */
    if (!node_filter_in_flight(&s->f)) {
        JS_FreeValue(ctx, s->wrap);
        s->wrap = node_wrap(ctx, s->node);
        if (JS_IsException(s->wrap)) { s->wrap = JS_UNDEFINED; JS_FreeValue(ctx, in); return -1; }
    }
    r = node_filter_run(ctx, hdr, hdr->this_val, &w->t, &s->f, s->wrap, in, &s->result, out_cb, out_argc);
    if (r > 0) return r;
    JS_FreeValue(ctx, s->wrap);
    s->wrap = JS_UNDEFINED;
    return r;
}

/* "set walker's current to node and return node" — the one sentence six of §6.2's algorithms end with. */
static JSValue tw_accept(JSContext *ctx, WalkerData *w, lxb_dom_node_t *n)
{
    JSValue v = node_wrap(ctx, n);
    JS_FreeValue(ctx, w->current);
    w->current = JS_DupValue(ctx, v);
    return v;
}

/* `walker's current`, READ FRESH — the filter may have assigned it. */
static lxb_dom_node_t *tw_current(WalkerData *w)
{
    lxb_dom_node_t *n = node_of(w->current);
    DCHECK(n != NULL, "a TreeWalker's current is not a node — the IDL's interface type is what keeps it one");
    return n;
}

static lxb_dom_node_t *tw_root(WalkerData *w)
{
    lxb_dom_node_t *n = node_of(w->t.root);
    DCHECK(n != NULL, "a TreeWalker's root is not a node");
    return n;
}

/* ---- §6.2 parentNode() ---------------------------------------------------------------------------------- */

#define TWP_STAGES(X) \
    X(TWP_ASCEND, "DOM §6.2 parentNode() steps 1-2.1 (this's current, then its parent)") \
    X(TWP_FILTER, "DOM §6.2 parentNode() step 2.2 (filtering that ancestor — the page's acceptNode runs here)")
enum { IDL_STEP_STAGE_BASE(TWP_STAGES) TWP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TWP_STEPS[] = { TWP_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int tw_parent_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    TreeWalkState *s = st;
    WalkerData *w = walker_here(ctx, hdr->this_val);
    int r;

    (void)argc; (void)argv;
    if (!w) { JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }

    if (hdr->stage == TWP_ASCEND) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* STEP 1, then step 2's guard and step 2.1. */
        s->node = tw_current(w);
        if (s->node == tw_root(w)) { *presult = JS_NULL; return JS_STEP_DONE; }
        s->node = s->node->parent;
        hdr->stage = TWP_FILTER;
    }

    DCHECK(hdr->stage == TWP_FILTER, "parentNode() resumed into a stage §6.2 does not have");
    for (;;) {
        if (!s->node) { *presult = JS_NULL; return JS_STEP_DONE; }
        /* STEP 2.2. */
        r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        if (s->result == NODE_FILTER_ACCEPT) { *presult = tw_accept(ctx, w, s->node); return JS_STEP_DONE; }
        /* Back to step 2: stop at the root, otherwise ascend and filter again. */
        if (s->node == tw_root(w)) { *presult = JS_NULL; return JS_STEP_DONE; }
        s->node = s->node->parent;
    }
}

static const IdlStepDecl TW_PARENT = { tw_parent_step, sizeof(TreeWalkState), tw_visit, tw_release,
                                       "DOM §6.2 TreeWalker.parentNode()", TWP_STEPS };

/* ---- §6.2 "traverse children" — firstChild() and lastChild() -------------------------------------------- */

enum { TWC_FIRST = 0, TWC_LAST };   /* the algorithm's `type` operand */

#define TWC_STAGES(X) \
    X(TWC_ENTER,  "DOM §6.2 traverse children steps 1-2 (walker's current, then its first/last child)") \
    X(TWC_FILTER, "DOM §6.2 traverse children step 3.1 (filtering node — the page's acceptNode runs here)")
enum { IDL_STEP_STAGE_BASE(TWC_STAGES) TWC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TWC_STEPS[] = { TWC_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* The `type` operand, as the two edge choices it decides. */
static lxb_dom_node_t *tw_edge_child(const lxb_dom_node_t *n, int type)
{
    return type == TWC_FIRST ? n->first_child : n->last_child;
}
static lxb_dom_node_t *tw_edge_sibling(const lxb_dom_node_t *n, int type)
{
    return type == TWC_FIRST ? n->next : n->prev;
}

static int tw_children_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    TreeWalkState *s = st;
    WalkerData *w = walker_here(ctx, hdr->this_val);
    int type = idl_step_magic(hdr), r;

    (void)argc; (void)argv;
    if (!w) { JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }

    if (hdr->stage == TWC_ENTER) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->node = tw_edge_child(tw_current(w), type);   /* STEPS 1-2 */
        hdr->stage = TWC_FILTER;
    }

    DCHECK(hdr->stage == TWC_FILTER, "traverse children resumed into a stage §6.2 does not have");
    while (s->node) {
        r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc);   /* STEP 3.1 */
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        if (s->result == NODE_FILTER_ACCEPT) {                        /* STEP 3.2 */
            *presult = tw_accept(ctx, w, s->node);
            return JS_STEP_DONE;
        }
        if (s->result == NODE_FILTER_SKIP) {                          /* STEP 3.3 */
            lxb_dom_node_t *child = tw_edge_child(s->node, type);
            if (child) { s->node = child; continue; }
        }
        /* STEP 3.4 — up and along, stopping at the root or at the node the walk started from. */
        while (s->node) {
            lxb_dom_node_t *sib = tw_edge_sibling(s->node, type);
            if (sib) { s->node = sib; break; }
            {
                lxb_dom_node_t *parent = s->node->parent;
                if (!parent || parent == tw_root(w) || parent == tw_current(w)) {
                    *presult = JS_NULL;
                    return JS_STEP_DONE;
                }
                s->node = parent;
            }
        }
    }
    *presult = JS_NULL;   /* STEP 4 */
    return JS_STEP_DONE;
}

static const IdlStepDecl TW_CHILDREN = { tw_children_step, sizeof(TreeWalkState), tw_visit, tw_release,
                                         "DOM §6.2 traverse children", TWC_STEPS };

/* ---- §6.2 "traverse siblings" — nextSibling() and previousSibling() ------------------------------------- */

#define TWS_STAGES(X) \
    X(TWS_ENTER,  "DOM §6.2 traverse siblings steps 1-3.1 (walker's current, the root guard, and its " \
                  "next/previous sibling)") \
    X(TWS_FILTER, "DOM §6.2 traverse siblings step 3.2.2 (filtering the sibling — the page's acceptNode)") \
    X(TWS_PARENT, "DOM §6.2 traverse siblings step 3.5 (filtering the parent the walk rose to, whose ACCEPT " \
                  "ends the search)")
enum { IDL_STEP_STAGE_BASE(TWS_STAGES) TWS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TWS_STEPS[] = { TWS_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int tw_siblings_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    TreeWalkState *s = st;
    WalkerData *w = walker_here(ctx, hdr->this_val);
    int type = idl_step_magic(hdr), r;

    (void)argc; (void)argv;
    if (!w) { JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }

    if (hdr->stage == TWS_ENTER) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->node = tw_current(w);                       /* STEP 1 */
        if (s->node == tw_root(w)) { *presult = JS_NULL; return JS_STEP_DONE; }   /* STEP 2 */
        s->sibling = tw_edge_sibling(s->node, type);   /* STEP 3.1 */
        hdr->stage = TWS_FILTER;
    }

    for (;;) {
        if (hdr->stage == TWS_FILTER) {
            while (s->sibling) {
                s->node = s->sibling;                                     /* STEP 3.2.1 */
                r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc);   /* STEP 3.2.2 */
                cb_result = JS_UNDEFINED;
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                if (s->result == NODE_FILTER_ACCEPT) {                    /* STEP 3.2.3 */
                    *presult = tw_accept(ctx, w, s->node);
                    return JS_STEP_DONE;
                }
                s->sibling = tw_edge_child(s->node, type);                /* STEP 3.2.4 */
                if (s->result == NODE_FILTER_REJECT || !s->sibling)       /* STEP 3.2.5 */
                    s->sibling = tw_edge_sibling(s->node, type);
            }
            s->node = s->node->parent;                                    /* STEP 3.3 */
            if (!s->node || s->node == tw_root(w)) {                      /* STEP 3.4 */
                *presult = JS_NULL;
                return JS_STEP_DONE;
            }
            hdr->stage = TWS_PARENT;
        }

        DCHECK(hdr->stage == TWS_PARENT, "traverse siblings resumed into a stage §6.2 does not have");
        r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc);       /* STEP 3.5 */
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        if (s->result == NODE_FILTER_ACCEPT) { *presult = JS_NULL; return JS_STEP_DONE; }
        s->sibling = tw_edge_sibling(s->node, type);                      /* back to STEP 3.1 */
        hdr->stage = TWS_FILTER;
    }
}

static const IdlStepDecl TW_SIBLINGS = { tw_siblings_step, sizeof(TreeWalkState), tw_visit, tw_release,
                                         "DOM §6.2 traverse siblings", TWS_STEPS };

/* ---- §6.2 previousNode() -------------------------------------------------------------------------------- */

#define TWV_STAGES(X) \
    X(TWV_ENTER,   "DOM §6.2 previousNode() steps 1-2.1 (this's current, then its previous sibling)") \
    X(TWV_SIBLING, "DOM §6.2 previousNode() step 2.2.2 (filtering the previous sibling)") \
    X(TWV_DESCEND, "DOM §6.2 previousNode() step 2.2.3.2 (filtering each last child on the way down)") \
    X(TWV_PARENT,  "DOM §6.2 previousNode() step 2.5 (filtering the parent the walk rose to)")
enum { IDL_STEP_STAGE_BASE(TWV_STAGES) TWV_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TWV_STEPS[] = { TWV_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int tw_previous_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    TreeWalkState *s = st;
    WalkerData *w = walker_here(ctx, hdr->this_val);
    int r;

    (void)argc; (void)argv;
    if (!w) { JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }

    if (hdr->stage == TWV_ENTER) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->node = tw_current(w);                                  /* STEP 1 */
        if (s->node == tw_root(w)) { *presult = JS_NULL; return JS_STEP_DONE; }   /* STEP 2's condition */
        s->sibling = s->node->prev;                               /* STEP 2.1 */
        hdr->stage = TWV_SIBLING;
    }

    for (;;) {
        if (hdr->stage == TWV_SIBLING) {
            if (!s->sibling) {
                /* STEP 2.3 — the walk cannot rise past the root. */
                if (s->node == tw_root(w) || !s->node->parent) { *presult = JS_NULL; return JS_STEP_DONE; }
                s->node = s->node->parent;                                     /* STEP 2.4 */
                hdr->stage = TWV_PARENT;
                continue;
            }
            s->node = s->sibling;                                              /* STEP 2.2.1 */
            r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc);        /* STEP 2.2.2 */
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            hdr->stage = TWV_DESCEND;
        }

        if (hdr->stage == TWV_DESCEND) {
            /* STEP 2.2.3 — descend into the last child while the last answer was not REJECT, filtering each.
               The advance is guarded because it precedes the filter: a resume must re-enter §6.4 on the node it
               parked in, not descend one level further first. */
            while (s->result != NODE_FILTER_REJECT && (node_filter_in_flight(&s->f) || s->node->last_child)) {
                if (!node_filter_in_flight(&s->f)) s->node = s->node->last_child;            /* STEP 2.2.3.1 */
                r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc);    /* STEP 2.2.3.2 */
                cb_result = JS_UNDEFINED;
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
            }
            if (s->result == NODE_FILTER_ACCEPT) {                             /* STEP 2.2.4 */
                *presult = tw_accept(ctx, w, s->node);
                return JS_STEP_DONE;
            }
            s->sibling = s->node->prev;                                        /* STEP 2.2.5 */
            hdr->stage = TWV_SIBLING;
            continue;
        }

        DCHECK(hdr->stage == TWV_PARENT, "previousNode() resumed into a stage §6.2 does not have");
        r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc);            /* STEP 2.5 */
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        if (s->result == NODE_FILTER_ACCEPT) { *presult = tw_accept(ctx, w, s->node); return JS_STEP_DONE; }
        /* Back to STEP 2's condition, then STEP 2.1. */
        if (s->node == tw_root(w)) { *presult = JS_NULL; return JS_STEP_DONE; }
        s->sibling = s->node->prev;
        hdr->stage = TWV_SIBLING;
    }
}

static const IdlStepDecl TW_PREVIOUS = { tw_previous_step, sizeof(TreeWalkState), tw_visit, tw_release,
                                         "DOM §6.2 TreeWalker.previousNode()", TWV_STEPS };

/* ---- §6.2 nextNode() ------------------------------------------------------------------------------------ */

#define TWN_STAGES(X) \
    X(TWN_ENTER,   "DOM §6.2 nextNode() steps 1-2 (this's current, and the initial FILTER_ACCEPT)") \
    X(TWN_CHILD,   "DOM §6.2 nextNode() step 3.1.2 (filtering the first child on the way down)") \
    X(TWN_SIBLING, "DOM §6.2 nextNode() step 3.7 (filtering the following sibling the walk rose to)")
enum { IDL_STEP_STAGE_BASE(TWN_STAGES) TWN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TWN_STEPS[] = { TWN_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int tw_next_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    TreeWalkState *s = st;
    WalkerData *w = walker_here(ctx, hdr->this_val);
    int r;

    (void)argc; (void)argv;
    if (!w) { JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }

    if (hdr->stage == TWN_ENTER) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->node = tw_current(w);            /* STEP 1 */
        s->result = NODE_FILTER_ACCEPT;     /* STEP 2 */
        hdr->stage = TWN_CHILD;
    }

    for (;;) {
        if (hdr->stage == TWN_CHILD) {
            /* STEP 3.1 — descend while the last answer was not REJECT. */
            while (s->result != NODE_FILTER_REJECT && (node_filter_in_flight(&s->f) || s->node->first_child)) {
                if (!node_filter_in_flight(&s->f)) s->node = s->node->first_child;        /* STEP 3.1.1 */
                r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc); /* STEP 3.1.2 */
                cb_result = JS_UNDEFINED;
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                if (s->result == NODE_FILTER_ACCEPT) {                      /* STEP 3.1.3 */
                    *presult = tw_accept(ctx, w, s->node);
                    return JS_STEP_DONE;
                }
            }
            /* STEPS 3.2-3.4 — rise until there is a following sibling, stopping AT the root. */
            s->sibling = NULL;
            s->temp = s->node;
            while (s->temp) {
                if (s->temp == tw_root(w)) { *presult = JS_NULL; return JS_STEP_DONE; }
                s->sibling = s->temp->next;
                if (s->sibling) break;
                s->temp = s->temp->parent;
            }
            /* STEP 3.5 — a walk that ran off a detached subtree's top has nowhere to go. */
            if (!s->sibling) { *presult = JS_NULL; return JS_STEP_DONE; }
            s->node = s->sibling;                                           /* STEP 3.6 */
            hdr->stage = TWN_SIBLING;
        }

        DCHECK(hdr->stage == TWN_SIBLING, "nextNode() resumed into a stage §6.2 does not have");
        r = tw_filter(ctx, hdr, w, s, cb_result, out_cb, out_argc);         /* STEP 3.7 */
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        if (s->result == NODE_FILTER_ACCEPT) {                              /* STEP 3.8 */
            *presult = tw_accept(ctx, w, s->node);
            return JS_STEP_DONE;
        }
        hdr->stage = TWN_CHILD;
    }
}

static const IdlStepDecl TW_NEXT = { tw_next_step, sizeof(TreeWalkState), tw_visit, tw_release,
                                     "DOM §6.2 TreeWalker.nextNode()", TWN_STEPS };

/* ---- §6.2's ATTRIBUTES ---------------------------------------------------------------------------------- */

/* magic 0 = root, 1 = whatToShow, 2 = filter, 3 = currentNode. None of them runs the page's code — each reads
   one slot of the record — so they are ordinary C getters. */
static JSValue js_walker_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    WalkerData *w = walker_here(ctx, this_val);

    if (!w) return JS_EXCEPTION;
    switch (magic) {
    case 0: return JS_DupValue(ctx, w->t.root);
    case 1: return JS_NewUint32(ctx, w->t.what);
    case 2: return JS_DupValue(ctx, w->t.filter);
    default:
        DCHECK(magic == 3, "a TreeWalker attribute ran with a magic §6.2 does not declare");
        return JS_DupValue(ctx, w->current);
    }
}

/* §6.2: "The currentNode setter steps are to set this's current to the given value." The value has already been
   brand-checked by the declaration's IDL_INTERFACE, so a non-Node threw before this ran. */
static JSValue js_walker_set_current(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    WalkerData *w = walker_here(ctx, this_val);

    (void)magic;
    if (!w) return JS_EXCEPTION;
    JS_FreeValue(ctx, w->current);
    w->current = JS_DupValue(ctx, val);
    return JS_UNDEFINED;
}

static int g_id_parent = -1, g_id_first = -1, g_id_last = -1, g_id_prev_sib = -1, g_id_next_sib = -1,
           g_id_prev = -1, g_id_next = -1, g_id_set_current = -1;

void tree_walker_init(JSContext *ctx)
{
    JSClassDef d = { "TreeWalker", walker_finalizer, walker_gc_mark };

    if (g_walker_class) return;   /* one AGENT, one class and one set of pool entries */
    g_walker_rt = JS_GetRuntime(ctx);
    JS_NewClassID(g_walker_rt, &g_walker_class);
    JS_NewClass(g_walker_rt, g_walker_class, &d);
    node_filter_init(ctx);

    g_id_parent   = idl_method_id_step(ctx, NULL, 0, NULL, 0, &TW_PARENT, 0);
    g_id_first    = idl_method_id_step(ctx, NULL, 0, NULL, 0, &TW_CHILDREN, TWC_FIRST);
    g_id_last     = idl_method_id_step(ctx, NULL, 0, NULL, 0, &TW_CHILDREN, TWC_LAST);
    g_id_next_sib = idl_method_id_step(ctx, NULL, 0, NULL, 0, &TW_SIBLINGS, TWC_FIRST);
    g_id_prev_sib = idl_method_id_step(ctx, NULL, 0, NULL, 0, &TW_SIBLINGS, TWC_LAST);
    g_id_prev     = idl_method_id_step(ctx, NULL, 0, NULL, 0, &TW_PREVIOUS, 0);
    g_id_next     = idl_method_id_step(ctx, NULL, 0, NULL, 0, &TW_NEXT, 0);
    /* `attribute Node currentNode` — an INTERFACE type, so `walker.currentNode = null` is a TypeError from the
       declaration and never from a check this body would have to remember. */
    g_id_set_current = idl_setter_id(ctx, IDL_INTERFACE, false, js_walker_set_current, 0);
    idl_iface_brand(node_class_id());

    realm_declare_intrinsic(tree_walker_install_proto);
}

void tree_walker_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_walker_class != 0, "a realm asked for TreeWalker.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_walker_class);
    DCHECK(JS_IsNull(prev), "tree_walker_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "TreeWalker.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "TreeWalker");
    idl_install_accessor(ctx, proto, "root", js_walker_get, 0, -1);
    idl_install_accessor(ctx, proto, "whatToShow", js_walker_get, 1, -1);
    idl_install_accessor(ctx, proto, "filter", js_walker_get, 2, -1);
    idl_install_accessor(ctx, proto, "currentNode", js_walker_get, 3, g_id_set_current);
    idl_install_method(ctx, proto, "parentNode", 0, g_id_parent);
    idl_install_method(ctx, proto, "firstChild", 0, g_id_first);
    idl_install_method(ctx, proto, "lastChild", 0, g_id_last);
    idl_install_method(ctx, proto, "previousSibling", 0, g_id_prev_sib);
    idl_install_method(ctx, proto, "nextSibling", 0, g_id_next_sib);
    idl_install_method(ctx, proto, "previousNode", 0, g_id_prev);
    idl_install_method(ctx, proto, "nextNode", 0, g_id_next);
    JS_SetClassProto(ctx, g_walker_class, proto);
}

void tree_walker_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_walker_class);

    DCHECK(!JS_IsNull(proto), "TreeWalker was installed in a realm that never ran its prototype install");
    JS_SetPropertyStr(ctx, (JSValue)global, "TreeWalker", idl_interface_object(ctx, "TreeWalker", proto));
    JS_FreeValue(ctx, proto);
}

void tree_walker_free(JSContext *ctx)
{
    (void)ctx;   /* the prototype is the REALM's — released with its context */
}
