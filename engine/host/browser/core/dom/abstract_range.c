/* DOM §5.2 boundary points, §5.3 AbstractRange, §5.4 StaticRange. See abstract_range.h. */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/abstract_range.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"

static const uint16_t RANGE_BOUNDS_OFF[] = { (uint16_t)offsetof(RangeBounds, start_node),
                                             (uint16_t)offsetof(RangeBounds, end_node) };
const CowRecord RANGE_BOUNDS_REC = { sizeof(RangeBounds), RANGE_BOUNDS_OFF, 2 };

void range_bounds_release(JSRuntime *rt, RangeBounds *b)
{
    JS_FreeValueRT(rt, b->start_node);
    JS_FreeValueRT(rt, b->end_node);
    b->start_node = JS_UNDEFINED;
    b->end_node = JS_UNDEFINED;
}

void range_bounds_mark(JSRuntime *rt, RangeBounds *b, JS_MarkFunc *mark_func)
{
    JS_MarkValue(rt, b->start_node, mark_func);
    JS_MarkValue(rt, b->end_node, mark_func);
}

/* ---- §5.2 --------------------------------------------------------------------------------------------- */

/* TREE ORDER between two nodes of one tree: −1 if `a` comes first, 0 if they are the same node, +1 otherwise.
   §4.2's "preceding"/"following" are defined by exactly this, and §5.2 step 3 is the only caller that needs it
   — but writing it inline there would be a second tree-order rule beside compareDocumentPosition's.
   IT WALKS UP, NOT ACROSS: the ancestor chains meet at the common ancestor and the answer is the order of the
   two children of it that the chains passed through, which is O(depth + siblings) with no allocation. A walk
   forward from one node is O(tree) and is what a naive "is it following" costs. */
static int tree_order(lxb_dom_node_t *a, lxb_dom_node_t *b)
{
    lxb_dom_node_t *x, *y, *n;
    int da = 0, db = 0;

    if (a == b) return 0;
    for (x = a; x; x = x->parent) da++;
    for (y = b; y; y = y->parent) db++;
    x = a; y = b;
    /* Lift the deeper chain to the other's depth. If the lift LANDS on the other node, that node is an ancestor
       — and a descendant follows its ancestor in tree order. */
    while (da > db) { if (x->parent == b) return BP_AFTER;  x = x->parent; da--; }
    while (db > da) { if (y->parent == a) return BP_BEFORE; y = y->parent; db--; }
    if (x == b) return BP_AFTER;
    if (y == a) return BP_BEFORE;
    while (x->parent != y->parent) {
        x = x->parent;
        y = y->parent;
        DCHECK(x != NULL && y != NULL, "§5.2 compared two nodes that are not in the same tree — the standard "
                                       "asserts they share a root and every caller checks it first");
    }
    /* Same parent now: whichever of the two the sibling list reaches first is the earlier node. */
    for (n = x->prev; n; n = n->prev)
        if (n == y) return BP_AFTER;
    return BP_BEFORE;
}

int boundary_position(lxb_dom_node_t *a, uint32_t ao, lxb_dom_node_t *b, uint32_t bo)
{
    lxb_dom_node_t *child;

    DCHECK(a != NULL && b != NULL, "§5.2 was asked for the position of a boundary point with no node");
    /* STEP 1's assertion. */
    DCHECK(node_root(a) == node_root(b), "§5.2's boundary points are in different trees — every caller checks "
                                         "the roots first, so reaching here means one of them stopped");
    /* STEP 2. */
    if (a == b) return ao == bo ? BP_EQUAL : (ao < bo ? BP_BEFORE : BP_AFTER);
    /* STEP 3 — inverted rather than recursed: the standard states it as a recursion into the swapped pair, and
       the only thing that recursion can answer is the mirror of steps 4-5. */
    if (tree_order(a, b) == BP_AFTER) {
        int r = boundary_position(b, bo, a, ao);
        return r == BP_BEFORE ? BP_AFTER : (r == BP_AFTER ? BP_BEFORE : BP_EQUAL);
    }
    /* STEP 4 — a is an ancestor of b (strictly: step 2 handled equality). */
    if (node_is_inclusive_ancestor(a, b)) {
        child = b;
        while (child->parent != a) {
            child = child->parent;
            DCHECK(child != NULL, "§5.2 step 4 walked past the ancestor it was told it had");
        }
        if (node_index(child) < ao) return BP_AFTER;
    }
    /* STEP 5. */
    return BP_BEFORE;
}

/* ---- §5.3's GETTERS, OVER EITHER INTERFACE -------------------------------------------------------------- */

/* The classes whose instances carry a RangeBounds. Two: §5.4's and §5.5's. */
#define ABSTRACT_RANGE_CLASSES 2
static JSClassID g_bounds_classes[ABSTRACT_RANGE_CLASSES];
static int g_bounds_class_n;

void abstract_range_claim_class(JSClassID cls)
{
    DCHECK(cls != 0, "a §5 interface claimed class 0");
    CHECK(g_bounds_class_n < ABSTRACT_RANGE_CLASSES,
          "a third §5 range interface claimed a class — AbstractRange's getters walk this list, so one it does "
          "not hold answers `startContainer` for an object that has one");
    g_bounds_classes[g_bounds_class_n++] = cls;
}

RangeBounds *abstract_range_of(JSValueConst v)
{
    int i;
    for (i = 0; i < g_bounds_class_n; i++) {
        RangeBounds *b = JS_GetOpaque(v, g_bounds_classes[i]);
        if (b) {
            /* THE RECORD TIME-TRAVELS. A flow that moves a range's start must not move it for a sibling, and
               the capture belongs where the flow REACHES the record — see tree_walker.c. */
            cow_capture_host_record(v, b, &RANGE_BOUNDS_REC);
            return b;
        }
    }
    return NULL;
}

/* magic 0 = startContainer, 1 = startOffset, 2 = endContainer, 3 = endOffset, 4 = collapsed. */
static JSValue js_abstract_range_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    RangeBounds *b = abstract_range_of(this_val);

    if (!b) return JS_ThrowTypeError(ctx, "not an AbstractRange");
    switch (magic) {
    case 0: return JS_DupValue(ctx, b->start_node);
    case 1: return JS_NewUint32(ctx, b->start_off);
    case 2: return JS_DupValue(ctx, b->end_node);
    case 3: return JS_NewUint32(ctx, b->end_off);
    default:
        DCHECK(magic == 4, "an AbstractRange attribute ran with a magic §5.3 does not declare");
        /* §5.3: "collapsed if its start node is its end node and its start offset is its end offset". */
        return JS_NewBool(ctx, node_of(b->start_node) == node_of(b->end_node) && b->start_off == b->end_off);
    }
}

/* ---- §5.4 StaticRange ----------------------------------------------------------------------------------- */

static JSClassID g_abstract_class, g_static_class;
static int g_id_static_ctor = -1;

static void static_range_finalizer(JSRuntime *rt, JSValue val)
{
    RangeBounds *b = JS_GetOpaque(val, g_static_class);
    if (!b) return;
    range_bounds_release(rt, b);
    free(b);
}

static void static_range_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    RangeBounds *b = JS_GetOpaque(val, g_static_class);
    if (b) range_bounds_mark(rt, b, mark_func);
}

/* `new StaticRange(init)` — §5.4. The dictionary arrived converted, so its four members are a Node, a number,
   a Node and a number and nothing here runs the page's code. */
static JSValue js_static_range_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic)
{
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue sc, ec, obj, proto;
    lxb_dom_node_t *sn, *en;
    RangeBounds *b;
    uint32_t so = 0, eo = 0;

    (void)this_val; (void)magic;
    sc = idl_dict_get(ctx, init, "startContainer");
    ec = idl_dict_get(ctx, init, "endContainer");
    sn = node_of(sc);
    en = node_of(ec);
    DCHECK(sn != NULL && en != NULL, "StaticRangeInit's containers reached the body unconverted — the "
                                     "dictionary's interface type is what makes them nodes");
    /* STEP 1. A doctype or an Attr cannot be a boundary point's node. */
    if (sn->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE || sn->type == LXB_DOM_NODE_TYPE_ATTRIBUTE ||
        en->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE || en->type == LXB_DOM_NODE_TYPE_ATTRIBUTE) {
        JS_FreeValue(ctx, sc);
        JS_FreeValue(ctx, ec);
        return JS_ThrowDOMException(ctx, "InvalidNodeTypeError",
                                    "a StaticRange boundary point cannot be a DocumentType or an Attr");
    }
    {
        JSValue v = idl_dict_get(ctx, init, "startOffset");
        JS_ToUint32(ctx, &so, v);
        JS_FreeValue(ctx, v);
        v = idl_dict_get(ctx, init, "endOffset");
        JS_ToUint32(ctx, &eo, v);
        JS_FreeValue(ctx, v);
    }
    proto = JS_GetClassProto(ctx, g_static_class);
    DCHECK(!JS_IsNull(proto), "a StaticRange was built in a realm with no StaticRange.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_static_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, sc); JS_FreeValue(ctx, ec); return obj; }
    b = calloc(1, sizeof(*b));
    CHECK(b != NULL, "the StaticRange record allocation failed");
    /* STEP 2. A static range is exactly what it was constructed with — it never tracks the tree, which is the
       whole of what makes it different from §5.5's. */
    b->start_node = sc;
    b->end_node = ec;
    b->start_off = so;
    b->end_off = eo;
    JS_SetOpaque(obj, b);
    return obj;
}

/* §5.4's `StaticRangeInit`, in LEXICOGRAPHIC order — §3.2.18 reads a dictionary's members that way and the page
   observes which getter runs first. All four are `required`, which is part of the type. */
static const IdlDictMember STATIC_RANGE_INIT[] = {
    { "endContainer",   IDL_INTERFACE,      true },
    { "endOffset",      IDL_UNSIGNED_LONG,  true },
    { "startContainer", IDL_INTERFACE,      true },
    { "startOffset",    IDL_UNSIGNED_LONG,  true },
};

void abstract_range_init(JSContext *ctx)
{
    JSClassDef ad = { "AbstractRange" };
    JSClassDef sd = { "StaticRange", static_range_finalizer, static_range_gc_mark };
    static const IdlArgType CTOR[1] = { IDL_DICT };

    if (g_static_class) return;   /* one AGENT, one class and one set of pool entries */
    JS_NewClassID(JS_GetRuntime(ctx), &g_abstract_class);
    JS_NewClass(JS_GetRuntime(ctx), g_abstract_class, &ad);
    JS_NewClassID(JS_GetRuntime(ctx), &g_static_class);
    JS_NewClass(JS_GetRuntime(ctx), g_static_class, &sd);
    abstract_range_claim_class(g_static_class);

    g_id_static_ctor = idl_method_id_dict(ctx, CTOR, 1, STATIC_RANGE_INIT,
                                          (int)(sizeof(STATIC_RANGE_INIT) / sizeof(STATIC_RANGE_INIT[0])),
                                          js_static_range_ctor, 0);
    idl_iface_brand(node_class_id());

    realm_declare_intrinsic(abstract_range_install_protos);
}

void abstract_range_install_protos(JSContext *ctx)
{
    JSValue ap, sp, prev;

    DCHECK(g_abstract_class != 0, "a realm asked for AbstractRange.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_abstract_class);
    DCHECK(JS_IsNull(prev), "abstract_range_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    ap = JS_NewObject(ctx);
    CHECK(!JS_IsException(ap), "AbstractRange.prototype could not be allocated");
    idl_interface_tag(ctx, ap, "AbstractRange");
    /* §5.3's five getters live HERE and nowhere else. Both derived interfaces inherit them, which is what the
       IDL's `StaticRange : AbstractRange` and `Range : AbstractRange` mean — installing them on each derived
       prototype instead would be two implementations of `collapsed`. */
    idl_install_accessor(ctx, ap, "startContainer", js_abstract_range_get, 0, -1);
    idl_install_accessor(ctx, ap, "startOffset",    js_abstract_range_get, 1, -1);
    idl_install_accessor(ctx, ap, "endContainer",   js_abstract_range_get, 2, -1);
    idl_install_accessor(ctx, ap, "endOffset",      js_abstract_range_get, 3, -1);
    idl_install_accessor(ctx, ap, "collapsed",      js_abstract_range_get, 4, -1);
    JS_SetClassProto(ctx, g_abstract_class, ap);

    sp = JS_NewObjectProto(ctx, ap);
    CHECK(!JS_IsException(sp), "StaticRange.prototype could not be allocated");
    idl_interface_tag(ctx, sp, "StaticRange");
    JS_SetClassProto(ctx, g_static_class, sp);
}

JSValue abstract_range_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_abstract_class);
    DCHECK(!JS_IsNull(proto), "AbstractRange.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

void abstract_range_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = abstract_range_proto(ctx), sctor;

    /* §5.3 declares no constructor: the interface object exists to be what `instanceof` names. */
    JS_SetPropertyStr(ctx, (JSValue)global, "AbstractRange",
                      idl_interface_object(ctx, "AbstractRange", proto));
    JS_FreeValue(ctx, proto);

    proto = JS_GetClassProto(ctx, g_static_class);
    DCHECK(!JS_IsNull(proto), "StaticRange was installed in a realm that never ran its prototype install");
    sctor = idl_step_constructor(ctx, "StaticRange", 1, g_id_static_ctor);
    JS_SetConstructor(ctx, sctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "StaticRange", sctor);
}

void abstract_range_free(JSRuntime *rt)
{
    (void)rt;   /* the prototypes are the REALMS' — released with their contexts */
}
