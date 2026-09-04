/* NODEITERATOR — DOM §6.1.
 *
 * ONE MEMBER PAIR IS A MACHINE AND THE REST ARE NOT, and the split is provable rather than chosen. `nextNode`
 * and `previousNode` are both "to traverse", which filters every candidate through the page's `acceptNode`
 * (§6.4) — so they suspend, at the exact candidate they were on. The five attributes and `detach` read one slot
 * each and reach nothing of the page's, so a machine there would be ceremony.
 *
 * THE CANDIDATE REFERENCE IS THE STANDARD'S OWN RESUME POINT, and it is why §6.1 has two node pointers rather
 * than one. "To traverse" advances a CANDIDATE, filters it, and only promotes it to the reference on ACCEPT —
 * so a filter that throws, or a removal that happens WHILE the filter is running, has something well-defined to
 * act on. That is exactly a step machine's state, written into the standard before this engine existed.
 *
 * THE LIVE-ITERATOR REGISTRY IS A LIST OF BORROWED OBJECTS WHOSE FINALIZER REMOVES ITS OWN ENTRY. §4.2.3's
 * remove runs §6.1's pre-remove steps "for each NodeIterator whose root's node document is node's node
 * document", so the agent has to be able to enumerate them — and enumerating them out of a JS Array would make
 * every iterator a page ever created immortal for the life of the document, which is a ceiling with no bound on
 * it. A borrowed pointer plus a finalizer that unregisters is Blink's own design (Document::AttachNodeIterator
 * and the NodeIterator destructor), and it is sound HERE for a reason worth stating: the registry is not
 * per-flow data. What a pre-remove WRITES is the iterator's record, and that record is captured into the
 * running flow's COW delta by iter_of — so a flow that removes a node adjusts every iterator's pointers on its
 * OWN timeline and a sibling flow's iterator is untouched. A registry entry that outlives its flow therefore
 * costs a visit, never a wrong answer, and the entry itself dies with the object. */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/node.h"
#include "core/dom/node_filter.h"
#include "core/dom/node_iterator.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"

/* §6.1's own state: §6's shared traverser, the REFERENCE node pointer, and the CANDIDATE one. A node pointer is
   "a node and a `pointer before` boolean"; the candidate is nullable, which `JS_UNDEFINED` in its node slot is
   (a node pointer's node is never undefined). */
typedef struct IterData {
    Traverser t;          /* FIRST — TRAVERSER_VALS names its slots at this struct's offsets */
    JSValue   ref_node;   /* §6.1 reference's node (a wrapper; OWNED) */
    JSValue   cand_node;  /* §6.1 candidate reference's node, or JS_UNDEFINED for null (OWNED) */
    uint8_t   ref_before;
    uint8_t   cand_before;
} IterData;

static JSClassID g_iter_class;

/* THE RECORD TIME-TRAVELS — see tree_walker.c for why the capture belongs in the accessor. */
static const uint16_t ITER_VALS[] = { TRAVERSER_VALS(IterData), (uint16_t)offsetof(IterData, ref_node),
                                      (uint16_t)offsetof(IterData, cand_node) };
static const CowRecord ITER_REC = { sizeof(IterData), ITER_VALS, 4 };

/* THE AGENT'S LIVE ITERATORS — see the file header. Entries are BORROWED: the finalizer removes its own. */
static JSValueConst *g_live;
static int g_live_n, g_live_cap, g_live_closed;

static void iter_register(JSValueConst obj)
{
    if (g_live_n == g_live_cap) {
        int want = g_live_cap ? g_live_cap * 2 : 8;
        JSValueConst *a = realloc(g_live, sizeof(*a) * (size_t)want);
        CHECK(a != NULL, "the live-NodeIterator list could not grow — dropping an entry means §4.2.3's remove "
                         "silently stops adjusting that iterator");
        g_live = a;
        g_live_cap = want;
    }
    g_live[g_live_n++] = obj;
}

static void iter_unregister(JSValueConst obj)
{
    int i;
    for (i = 0; i < g_live_n; i++)
        if (JS_VALUE_GET_PTR(g_live[i]) == JS_VALUE_GET_PTR(obj)) {
            g_live[i] = g_live[--g_live_n];
            return;
        }
    DFAIL("a NodeIterator was finalized without being in the live list — the registry and the objects' "
          "lifetimes have come apart, so a later removal would walk a freed entry");
}

static void iter_live_drop(void)
{
    if (g_live_n || !g_live_closed) return;
    free(g_live);
    g_live = NULL;
    g_live_cap = 0;
}

static IterData *iter_of(JSValueConst v)
{
    IterData *it = JS_GetOpaque(v, g_iter_class);
    if (it) cow_capture_host_record(v, it, &ITER_REC);
    return it;
}

/* WRITE ONE OF THE TWO NODE POINTERS, and never `JS_FreeValue(ctx, it->cand_node); it->cand_node = <build
   one>;` — see cow.h for the order and the defect. BOTH sides of it are real here. The REFILL allocates:
   node_wrap mints a wrapper, and an allocation IS a collection (js_trigger_gc has exactly one caller,
   JS_NewObjectFromShape), so a slot left naming freed storage across the build is walked by iter_gc_mark and
   decrefs a JSObject already back on the allocator's free list. The RELEASE is the same hazard from the other
   side: giving a node pointer back can drop the last reference to a wrapper, and a wrapper's finalizer is the
   page's platform code, which may allocate — which is why even step 4's `= JS_UNDEFINED` comes here.
   The record and its layout are bound HERE rather than at each call, so no site can pass a slot from another
   record with this layout — and that is also what makes §6.1's "adjust a node pointer" checkable at all: it
   writes THROUGH a pointer to whichever of the two pointers §6.1 handed it, and the layout assert is the only
   thing that can say the pointer named one of them.
   node_iterator_new's mint does not come here, and that is the one honest exception: before JS_SetOpaque the
   record is unreachable by the collector and its calloc'd slots hold no value to release. */
/* THE ADDRESS PASSES THROUGH: the asserts inside are about the SLOT, so they must name the WRITE and not this
   line — see cow.h's THE SITE TRAVELS WITH THE OPERATION. */
static void ni_set_at(JSContext *ctx, IterData *it, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, it, &ITER_REC, slot, v, file, line);
}
#define ni_set(ctx_, it_, slot_, v_) ni_set_at((ctx_), (it_), (slot_), (v_), __FILE__, __LINE__)

/* DOES THIS VALUE IMPLEMENT `NodeIterator` — Web IDL §3.7 Interfaces' implementation-check an object step 3,
   "If object does not implement interface, then throw a TypeError.", as the PREDICATE core/idl_args'
   idl_this_iface takes. §3.7.7 Operations' create an operation function asks it at step 2.1.2.3, BEFORE step
   2.1.4 computes the effective overload set, so a member stating it at its DECLARATION refuses a foreign
   receiver ahead of §3.6 Overload resolution algorithm's conversions — an order a body cannot reproduce.
   §6.1's three operations take no argument, so nothing of the page's runs between the two orders TODAY; the
   declaration is still where the brand belongs, because what keeps that true is the ARGUMENT LIST, which is
   the one thing a later partial interface can change without touching this file.
   PURE, DELIBERATELY: it is called by the brand check and must not capture the record into a flow's delta for
   a receiver that is about to be refused. iter_of's capture belongs to the members that ACCEPT one. */
static bool iter_is(JSValueConst v)
{
    return JS_GetOpaque(v, g_iter_class) != NULL;
}

/* THE RECORD FOR A RECEIVER §3.7 HAS ALREADY ADMITTED — §6.1's three operations, whose declarations state the
   interface above. Reaching a body means idl_implementation_check ran and passed, so the only condition left
   for this to fire on is a member installed WITHOUT its brand: this engine's own routing, never page input. */
static IterData *iter_receiver(JSValueConst v)
{
    IterData *it = iter_of(v);

    DCHECK(it != NULL, "a §6.1 member reached its body on a receiver that is not a NodeIterator — its "
                       "declaration states Web IDL §3.7 Interfaces' implementation check, so reaching the body "
                       "means idl_implementation_check did not run for it");
    return it;
}

/* THE SAME QUESTION FOR THE MEMBERS THAT CANNOT STATE IT — §6.1's five ATTRIBUTE GETTERS, which
   idl_install_accessor mints as plain JS_CFUNC_getter_magic functions with no pool entry, so they converge on
   nothing that could ask §3.7 for them: the residual core/idl_args.c names at the site it would reach. ONE
   ANSWER TO ONE QUESTION: this routes to the predicate above, so the two ways into a §6.1 member cannot drift.
   When a plain getter gains a pool entry, this function goes with it. */
static IterData *iter_here(JSContext *ctx, JSValueConst v)
{
    if (!iter_is(v)) {
        JS_ThrowTypeError(ctx, "not a NodeIterator");
        return NULL;
    }
    return iter_receiver(v);
}

static void iter_finalizer(JSRuntime *rt, JSValue val)
{
    IterData *it = JS_GetOpaque(val, g_iter_class);
    if (!it) return;
    iter_unregister(val);
    iter_live_drop();
    traverser_release(rt, &it->t);
    JS_FreeValueRT(rt, it->ref_node);
    JS_FreeValueRT(rt, it->cand_node);
    free(it);
}

static void iter_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    IterData *it = JS_GetOpaque(val, g_iter_class);
    if (!it) return;
    traverser_mark(rt, &it->t, mark_func);
    JS_MarkValue(rt, it->ref_node, mark_func);
    JS_MarkValue(rt, it->cand_node, mark_func);
}

JSValue node_iterator_new(JSContext *ctx, JSValueConst root, uint32_t what, JSValueConst filter)
{
    JSValue obj, proto;
    IterData *it;

    DCHECK(g_iter_class != 0, "a NodeIterator was built before node_iterator_init declared its class");
    proto = JS_GetClassProto(ctx, g_iter_class);
    DCHECK(!JS_IsNull(proto), "a NodeIterator was built in a realm with no NodeIterator.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_iter_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    it = calloc(1, sizeof(*it));
    CHECK(it != NULL, "the NodeIterator record allocation failed");
    /* §4.5 createNodeIterator steps 2-5. */
    it->t.root = JS_DupValue(ctx, root);
    it->t.filter = JS_DupValue(ctx, filter);
    it->t.what = what;
    it->ref_node = JS_DupValue(ctx, root);
    it->ref_before = 1;
    it->cand_node = JS_UNDEFINED;   /* the candidate reference is initially null */
    JS_SetOpaque(obj, it);
    iter_register(obj);
    return obj;
}

/* ---- §6.1's PRE-REMOVE STEPS ---------------------------------------------------------------------------- */

/* The last node of `n`'s subtree in tree order — its deepest last descendant, or `n` itself. */
static lxb_dom_node_t *iter_last_inclusive_descendant(lxb_dom_node_t *n)
{
    while (n->last_child) n = n->last_child;
    return n;
}

/* "toBeRemovedNode's first FOLLOWING node that is an inclusive descendant of root and is not an inclusive
   descendant of toBeRemovedNode" — the tree-order successor of the whole subtree, bounded by root. */
static lxb_dom_node_t *iter_following_outside(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    while (n && n != root) {
        if (n->next) return n->next;
        n = n->parent;
    }
    return NULL;
}

/* §6.1 "adjust a node pointer". `pnode`/`pbefore` are the pointer, updated in place. */
static void iter_adjust(JSContext *ctx, IterData *it, JSValue *pnode, uint8_t *pbefore,
                        lxb_dom_node_t *removed)
{
    lxb_dom_node_t *root = node_of(it->t.root), *n = node_of(*pnode), *fresh;

    DCHECK(root != NULL, "a NodeIterator's root is not a node");
    DCHECK(n != NULL, "a NodeIterator's node pointer holds something that is not a node");
    /* STEP 1. */
    if (!node_is_inclusive_ancestor(removed, n) || node_is_inclusive_ancestor(removed, root))
        return;
    /* STEP 2. */
    if (*pbefore) {
        fresh = iter_following_outside(removed, root);
        if (fresh) {
            ni_set(ctx, it, pnode, node_wrap(ctx, fresh));
            *pbefore = 1;
            return;
        }
    }
    /* STEPS 3-4. */
    fresh = removed->prev ? iter_last_inclusive_descendant(removed->prev) : removed->parent;
    DCHECK(fresh != NULL, "§6.1's adjust reached a node being removed from no parent");
    ni_set(ctx, it, pnode, node_wrap(ctx, fresh));
    *pbefore = 0;
}

void node_iterator_pre_remove(JSContext *ctx, lxb_dom_node_t *to_be_removed)
{
    int i;

    if (!g_live_n || !to_be_removed) return;
    for (i = 0; i < g_live_n; i++) {
        /* iter_of, not JS_GetOpaque: reaching the record is what puts it in the running flow's delta, which is
           what makes one flow's adjustment invisible to a sibling. */
        IterData *it = iter_of(g_live[i]);
        lxb_dom_node_t *root;

        DCHECK(it != NULL, "the live-NodeIterator list holds something that is not a NodeIterator");
        root = node_of(it->t.root);
        DCHECK(root != NULL, "a live NodeIterator's root is not a node");
        /* §4.2.3: only the iterators of the removed node's NODE DOCUMENT. */
        if (root->owner_document != to_be_removed->owner_document) continue;
        iter_adjust(ctx, it, &it->ref_node, &it->ref_before, to_be_removed);   /* STEP 1 */
        if (!JS_IsUndefined(it->cand_node))                                    /* STEP 2 */
            iter_adjust(ctx, it, &it->cand_node, &it->cand_before, to_be_removed);
    }
}

/* ---- §6.1 "to traverse" — nextNode() and previousNode() ------------------------------------------------- */

enum { NI_NEXT = 0, NI_PREVIOUS };

#define NI_STAGES(X) \
    X(NI_ENTER,  "DOM §6.1 traverse steps 1-2 (the candidate reference becomes the reference)") \
    X(NI_FILTER, "DOM §6.1 traverse step 3.4 (filtering the candidate — the page's acceptNode runs here)")
enum { IDL_STEP_STAGE_BASE(NI_STAGES) NI_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NI_STEPS[] = { NI_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct NodeIterState {
    NodeFilterCall f;
    JSValue wrap;      /* the candidate's wrapper, held ACROSS the filter call (OWNED) */
} NodeIterState;

static void ni_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    NodeIterState *s = st;
    node_filter_call_visit(ctx, &s->f, v);
    v->val(ctx, &s->wrap);
}

/* §6.4 step 7's flag alone — `wrap` and everything the filter call holds are references ni_visit names, and the
   teardown discharges that one declaration. */
static void ni_release(JSContext *ctx, void *st)
{
    (void)ctx;
    node_filter_call_unlock(&((NodeIterState *)st)->f);
}

/* Step 4's "set iterator's candidate reference to null", which is also step 3.4's abrupt exit. */
static void ni_drop_candidate(JSContext *ctx, IterData *it)
{
    ni_set(ctx, it, &it->cand_node, JS_UNDEFINED);
    it->cand_before = 0;
}

static int ni_traverse_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeIterState *s = st;
    IterData *it = iter_receiver(hdr->this_val);
    int type = idl_step_magic(hdr), result = 0, r;
    lxb_dom_node_t *root;

    (void)argc; (void)argv;
    root = node_of(it->t.root);
    DCHECK(root != NULL, "a NodeIterator's root is not a node");

    if (hdr->stage == NI_ENTER) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* STEP 1: the candidate starts as the reference. STEP 2's `result` is this body's `*presult`. */
        ni_set(ctx, it, &it->cand_node, JS_DupValue(ctx, it->ref_node));
        it->cand_before = it->ref_before;
        hdr->stage = NI_FILTER;
    }

    DCHECK(hdr->stage == NI_FILTER, "§6.1's traverse resumed into a stage it does not have");
    for (;;) {
        lxb_dom_node_t *cand;

        /* STEP 3.1/3.2 — the advance. Guarded, because it PRECEDES the filter: a resume must re-enter §6.4 on
           the candidate it parked in rather than advance past it. */
        if (!node_filter_in_flight(&s->f)) {
            cand = node_of(it->cand_node);
            DCHECK(cand != NULL, "a NodeIterator's candidate is not a node");
            if (type == NI_NEXT) {
                if (!it->cand_before) {
                    lxb_dom_node_t *following = node_next_in(cand, root);
                    if (!following) break;             /* STEP 3.1.1.1's "then break" */
                    ni_set(ctx, it, &it->cand_node, node_wrap(ctx, following));
                }
                it->cand_before = 0;
            } else {
                if (it->cand_before) {
                    lxb_dom_node_t *preceding = node_prev_in(cand, root);
                    if (!preceding) break;
                    ni_set(ctx, it, &it->cand_node, node_wrap(ctx, preceding));
                }
                it->cand_before = 1;
            }
            JS_FreeValue(ctx, s->wrap);
            s->wrap = JS_DupValue(ctx, it->cand_node);   /* STEP 3.3 */
        }

        /* STEP 3.4. On a throw the candidate is dropped, which is the standard's own sentence. */
        r = node_filter_run(ctx, hdr, hdr->this_val, &it->t, &s->f, s->wrap, cb_result, &result,
                            out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) { ni_drop_candidate(ctx, it); return JS_STEP_ABRUPT; }
        if (result == NODE_FILTER_ACCEPT) {           /* STEP 3.5 */
            ni_set(ctx, it, &it->ref_node, JS_DupValue(ctx, it->cand_node));
            it->ref_before = it->cand_before;
            /* STEP 3.5.2 RETURNS `node`, WHICH STEP 3.3 BOUND BEFORE THE FILTER RAN — not the candidate
               reference, which the filter may have moved. A filter that removes the very node it is filtering
               runs §6.1's pre-remove steps against this traversal, so the candidate is retargeted to a node
               still in the tree while the ANSWER is the node the page was asked about. Reading the candidate
               here returned the retargeted node and the corpus pins the difference. */
            *presult = JS_DupValue(ctx, s->wrap);
            ni_drop_candidate(ctx, it);               /* STEP 4 */
            return JS_STEP_DONE;
        }
    }
    ni_drop_candidate(ctx, it);                       /* STEP 4 */
    *presult = JS_NULL;                               /* STEP 5, with `result` still null */
    return JS_STEP_DONE;
}

static const IdlStepDecl NI_TRAVERSE = { ni_traverse_step, sizeof(NodeIterState), ni_visit, ni_release,
                                         "DOM §6.1 traverse", NI_STEPS };

/* ---- §6.1's ATTRIBUTES AND detach() --------------------------------------------------------------------- */

/* magic 0 = root, 1 = referenceNode, 2 = pointerBeforeReferenceNode, 3 = whatToShow, 4 = filter. */
static JSValue js_iter_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    IterData *it = iter_here(ctx, this_val);

    if (!it) return JS_EXCEPTION;
    switch (magic) {
    case 0: return JS_DupValue(ctx, it->t.root);
    case 1: return JS_DupValue(ctx, it->ref_node);
    case 2: return JS_NewBool(ctx, it->ref_before != 0);
    case 3: return JS_NewUint32(ctx, it->t.what);
    default:
        DCHECK(magic == 4, "a NodeIterator attribute ran with a magic §6.1 does not declare");
        return JS_DupValue(ctx, it->t.filter);
    }
}

/* §6.1: "The detach() method steps are to do nothing." The functionality was REMOVED from the standard and the
   member kept for compatibility, so a no-effect body here is the spec's own text and not a stub — it is the one
   shape §NO STUBS exempts. What keeps it from being a no-op on ANYTHING AT ALL is its declaration's receiver
   interface: `NodeIterator.prototype.detach.call({})` is §3.7 step 3's TypeError, decided before this runs.
   THE CALL BELOW IS THE ASSERTION AND NOT THE CHECK, and this is the member that most needs one — a body that
   does nothing cannot LOOK wrong, so a detach whose brand was never declared would answer `undefined` for every
   receiver on earth and no test of its behaviour could tell. Asserting the routing here is the only thing that
   would say so. */
static JSValue js_iter_detach(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)argc; (void)argv; (void)magic;
    iter_receiver(this_val);
    return JS_UNDEFINED;
}

static int g_id_next = -1, g_id_prev = -1, g_id_detach = -1;

void node_iterator_init(JSContext *ctx)
{
    JSClassDef d = { "NodeIterator", iter_finalizer, iter_gc_mark };

    if (g_iter_class) return;   /* one AGENT, one class and one set of pool entries */
    JS_NewClassID(JS_GetRuntime(ctx), &g_iter_class);
    JS_NewClass(JS_GetRuntime(ctx), g_iter_class, &d);
    node_filter_init(ctx);

    g_id_next   = idl_method_id_step(ctx, NULL, 0, NULL, 0, &NI_TRAVERSE, NI_NEXT);
    idl_this_iface(iter_is, "NodeIterator");
    g_id_prev   = idl_method_id_step(ctx, NULL, 0, NULL, 0, &NI_TRAVERSE, NI_PREVIOUS);
    idl_this_iface(iter_is, "NodeIterator");
    g_id_detach = idl_method_id(ctx, NULL, 0, js_iter_detach, 0);
    idl_this_iface(iter_is, "NodeIterator");

    realm_declare_intrinsic(node_iterator_install_proto);
}

void node_iterator_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_iter_class != 0, "a realm asked for NodeIterator.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_iter_class);
    DCHECK(JS_IsNull(prev), "node_iterator_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "NodeIterator.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "NodeIterator");
    idl_install_accessor(ctx, proto, "root", js_iter_get, 0, -1);
    idl_install_accessor(ctx, proto, "referenceNode", js_iter_get, 1, -1);
    idl_install_accessor(ctx, proto, "pointerBeforeReferenceNode", js_iter_get, 2, -1);
    idl_install_accessor(ctx, proto, "whatToShow", js_iter_get, 3, -1);
    idl_install_accessor(ctx, proto, "filter", js_iter_get, 4, -1);
    idl_install_method(ctx, proto, "nextNode", g_id_next);
    idl_install_method(ctx, proto, "previousNode", g_id_prev);
    idl_install_method(ctx, proto, "detach", g_id_detach);
    JS_SetClassProto(ctx, g_iter_class, proto);
}

void node_iterator_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_iter_class);

    DCHECK(!JS_IsNull(proto), "NodeIterator was installed in a realm that never ran its prototype install");
    JS_SetPropertyStr(ctx, (JSValue)global, "NodeIterator", idl_interface_object(ctx, "NodeIterator", proto));
    JS_FreeValue(ctx, proto);
}

void node_iterator_free(JSRuntime *rt)
{
    (void)rt;
    /* THE LIST OUTLIVES THE COMPONENT'S TEARDOWN, and it has to. A component's `_free` runs BEFORE the runtime's
       final sweep, so the objects whose finalizers unregister are still alive when it does — freeing the array
       there is a use-after-free at the first finalizer, and asserting the list is empty there asserts an order the
       runtime does not have. So teardown only says "no more registrations are coming", and the LAST finalizer
       frees the array. A list that is already empty is freed at once, which is the ordinary case. */
    g_live_closed = 1;
    iter_live_drop();
}
