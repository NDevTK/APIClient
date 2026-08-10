/* RANGE — DOM §5.5, the LIVE range.
 *
 * WHAT MAKES IT LIVE IS THAT THE TREE MOVES ITS BOUNDARY POINTS, and that is the whole difference between this
 * and §5.4's StaticRange: the two carry the identical pair of boundary points (RangeBounds, in
 * abstract_range.c) and the identical five getters, and a Range additionally REGISTERS itself so that §4.2.3's
 * insert and remove can adjust it. Those adjustments are the standard's own algorithms — the "live range
 * pre-remove steps" and insert's step 4 — and they run from the DOM's one mutation chokepoint, so a tree write
 * cannot reach the tree without them.
 *
 * THE REGISTRY IS BORROWED POINTERS WITH A FINALIZER THAT UNREGISTERS, exactly as node_iterator.c's is and for
 * exactly its reasons: a strong list would make every range a page ever built immortal, and the per-flow COW
 * capture in abstract_range_of is what makes a shared registry answer per flow — a removal performed by one
 * flow moves the boundary points on THAT flow's timeline and a sibling's range is untouched.
 *
 * WHAT IS HERE AND WHAT IS HONESTLY ABSENT. Everything §5.5 states over boundary points is here: the
 * constructor, the six setters, collapse, selectNode, selectNodeContents, commonAncestorContainer,
 * compareBoundaryPoints, isPointInRange, comparePoint, intersectsNode, cloneRange, detach and the stringifier.
 * The five members that MOVE CONTENT — deleteContents, extractContents, cloneContents, insertNode and
 * surroundContents — are ABSENT, and absent rather than stubbed because each of them is stated in terms of DOM
 * §4.10's "replace data" and "split", neither of which this engine has: CharacterData carries `data` and
 * `length` and no mutation members at all. A deleteContents that removed whole nodes and left the partially
 * contained Text alone would be a lie a page cannot detect. The forcing function is the page's own TypeError,
 * and the thing to build first is §4.10. */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/abstract_range.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/range.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"

static JSClassID g_range_class;

/* THE AGENT'S LIVE RANGES — borrowed, each removed by its own finalizer. */
static JSValueConst *g_live;
static int g_live_n, g_live_cap, g_live_closed;

static void range_register(JSValueConst obj)
{
    if (g_live_n == g_live_cap) {
        int want = g_live_cap ? g_live_cap * 2 : 8;
        JSValueConst *a = realloc(g_live, sizeof(*a) * (size_t)want);
        CHECK(a != NULL, "the live-range list could not grow — a dropped entry is a range §4.2.3 silently "
                         "stops keeping up to date, which is the one thing that makes it LIVE");
        g_live = a;
        g_live_cap = want;
    }
    g_live[g_live_n++] = obj;
}

static void range_unregister(JSValueConst obj)
{
    int i;
    for (i = 0; i < g_live_n; i++)
        if (JS_VALUE_GET_PTR(g_live[i]) == JS_VALUE_GET_PTR(obj)) {
            g_live[i] = g_live[--g_live_n];
            return;
        }
    DFAIL("a live range was finalized without being in the live list — the registry and the objects' lifetimes "
          "have come apart, so a later mutation would walk a freed entry");
}

static void range_live_drop(void)
{
    if (g_live_n || !g_live_closed) return;
    free(g_live);
    g_live = NULL;
    g_live_cap = 0;
}

static void range_finalizer(JSRuntime *rt, JSValue val)
{
    RangeBounds *b = JS_GetOpaque(val, g_range_class);
    if (!b) return;
    range_unregister(val);
    range_live_drop();
    range_bounds_release(rt, b);
    free(b);
}

static void range_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    RangeBounds *b = JS_GetOpaque(val, g_range_class);
    if (b) range_bounds_mark(rt, b, mark_func);
}

/* The receiver, brand-checked against §5.5's own class — NOT against AbstractRange's list, because a
   StaticRange has none of these members and `Range.prototype.setStart.call(staticRange)` must be a TypeError
   rather than a live edit of an object the spec says never moves. */
static RangeBounds *range_here(JSContext *ctx, JSValueConst v)
{
    RangeBounds *b = JS_GetOpaque(v, g_range_class);
    if (!b) {
        JS_ThrowTypeError(ctx, "not a Range");
        return NULL;
    }
    cow_capture_host_record(v, b, &RANGE_BOUNDS_REC);
    return b;
}

static lxb_dom_node_t *bounds_start(const RangeBounds *b) { return node_of(b->start_node); }
static lxb_dom_node_t *bounds_end(const RangeBounds *b)   { return node_of(b->end_node); }

/* §5.5: "The root of a live range is the root of its start node." */
static lxb_dom_node_t *range_root(const RangeBounds *b)
{
    lxb_dom_node_t *n = bounds_start(b);
    DCHECK(n != NULL, "a live range's start is not a node");
    return node_root(n);
}

static void bounds_set_start(JSContext *ctx, RangeBounds *b, JSValueConst node, uint32_t off)
{
    JS_FreeValue(ctx, b->start_node);
    b->start_node = JS_DupValue(ctx, node);
    b->start_off = off;
}

static void bounds_set_end(JSContext *ctx, RangeBounds *b, JSValueConst node, uint32_t off)
{
    JS_FreeValue(ctx, b->end_node);
    b->end_node = JS_DupValue(ctx, node);
    b->end_off = off;
}

/* ---- §5.5's LIVE-RANGE MUTATION ALGORITHMS -------------------------------------------------------------- */

void range_pre_remove(JSContext *ctx, lxb_dom_node_t *node)
{
    lxb_dom_node_t *parent;
    uint32_t index;
    JSValue pw;
    int i;

    if (!g_live_n || !node) return;
    parent = node->parent;
    if (!parent) return;   /* §5.5 asserts a parent; a removal of an already-detached node reaches nothing */
    index = node_index(node);
    pw = node_wrap(ctx, parent);
    for (i = 0; i < g_live_n; i++) {
        /* abstract_range_of, not JS_GetOpaque: reaching the record is what puts it in the running flow's
           delta, which is what makes one flow's adjustment invisible to a sibling. */
        RangeBounds *b = abstract_range_of(g_live[i]);
        lxb_dom_node_t *sn, *en;

        DCHECK(b != NULL, "the live-range list holds something that is not a range");
        sn = bounds_start(b);
        en = bounds_end(b);
        DCHECK(sn != NULL && en != NULL, "a live range's boundary point is not a node");
        /* STEPS 4-5: a boundary point INSIDE the subtree being removed collapses onto the removal site. */
        if (node_is_inclusive_ancestor(node, sn)) bounds_set_start(ctx, b, pw, index);
        if (node_is_inclusive_ancestor(node, en)) bounds_set_end(ctx, b, pw, index);
        /* STEPS 6-7: a boundary point in the PARENT past the removed child moves back by one. Re-read, because
           steps 4-5 may have just moved it here — and the standard runs the four in this order for that
           reason. `>` and not `>=`: an offset EQUAL to the index already points at the removal site. */
        sn = bounds_start(b);
        en = bounds_end(b);
        if (sn == parent && b->start_off > index) b->start_off--;
        if (en == parent && b->end_off > index)   b->end_off--;
    }
    JS_FreeValue(ctx, pw);
}

void range_did_insert(JSContext *ctx, lxb_dom_node_t *node)
{
    lxb_dom_node_t *parent;
    uint32_t index;
    int i;

    (void)ctx;
    if (!g_live_n || !node) return;
    parent = node->parent;
    if (!parent) return;
    index = node_index(node);
    for (i = 0; i < g_live_n; i++) {
        RangeBounds *b = abstract_range_of(g_live[i]);
        DCHECK(b != NULL, "the live-range list holds something that is not a range");
        /* §4.2.3 insert step 4, stated against the REFERENCE child's index — which, once the node is in, is the
           inserted node's own. An offset EQUAL to it stays: it names the point immediately before the new node,
           which is where the boundary was. */
        if (bounds_start(b) == parent && b->start_off > index) b->start_off++;
        if (bounds_end(b) == parent && b->end_off > index)     b->end_off++;
    }
}

/* ---- §5.5's MEMBERS ------------------------------------------------------------------------------------- */

static JSValue range_new(JSContext *ctx, JSValueConst snode, uint32_t soff, JSValueConst enode, uint32_t eoff)
{
    JSValue obj, proto = JS_GetClassProto(ctx, g_range_class);
    RangeBounds *b;

    DCHECK(!JS_IsNull(proto), "a Range was built in a realm with no Range.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_range_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    b = calloc(1, sizeof(*b));
    CHECK(b != NULL, "the Range record allocation failed");
    b->start_node = JS_DupValue(ctx, snode);
    b->end_node = JS_DupValue(ctx, enode);
    b->start_off = soff;
    b->end_off = eoff;
    JS_SetOpaque(obj, b);
    range_register(obj);
    return obj;
}

JSValue range_new_at(JSContext *ctx, JSValueConst node)
{
    DCHECK(node_of(node) != NULL, "a live range was asked for at something that is not a node");
    return range_new(ctx, node, 0, node, 0);
}

/* `new Range()` — §5.5: "set this's start and end to (current global object's associated Document, 0)". */
static JSValue js_range_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValueConst doc = document_object(ctx);

    (void)new_target; (void)argc; (void)argv;
    DCHECK(node_of(doc) != NULL, "new Range() ran in a realm whose Document is not a node");
    return range_new(ctx, doc, 0, doc, 0);
}

/* §5.5's "set the start or end of a range to a boundary point". magic 0 = start, 1 = end. */
static int range_set_bp(JSContext *ctx, RangeBounds *b, JSValueConst nodev, uint32_t off, int is_end)
{
    lxb_dom_node_t *n = node_of(nodev);

    DCHECK(n != NULL, "a Range boundary-point setter reached the body with something that is not a node");
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) {                        /* STEP 1 */
        JS_ThrowDOMException(ctx, "InvalidNodeTypeError", "a Range boundary point cannot be a DocumentType");
        return -1;
    }
    if (off > node_length(n)) {                                              /* STEP 2 */
        JS_ThrowDOMException(ctx, "IndexSizeError", "the offset is past the node's length");
        return -1;
    }
    /* STEP 4. The OTHER end moves when the two would otherwise be in different trees or out of order, which is
       what keeps a live range's start before its end without the page having to. */
    if (!is_end) {
        if (range_root(b) != node_root(n) ||
            boundary_position(n, off, bounds_end(b), b->end_off) == BP_AFTER)
            bounds_set_end(ctx, b, nodev, off);
        bounds_set_start(ctx, b, nodev, off);
    } else {
        if (range_root(b) != node_root(n) ||
            boundary_position(n, off, bounds_start(b), b->start_off) == BP_BEFORE)
            bounds_set_start(ctx, b, nodev, off);
        bounds_set_end(ctx, b, nodev, off);
    }
    return 0;
}

enum { R_SET_START = 0, R_SET_END, R_SET_START_BEFORE, R_SET_START_AFTER, R_SET_END_BEFORE, R_SET_END_AFTER,
       R_COLLAPSE, R_SELECT_NODE, R_SELECT_CONTENTS, R_CLONE, R_DETACH,
       R_COMPARE_BP, R_IS_POINT_IN, R_COMPARE_POINT, R_INTERSECTS };

static JSValue js_range_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    RangeBounds *b = range_here(ctx, this_val);
    JSValueConst nodev = argc > 0 ? argv[0] : JS_UNDEFINED;
    lxb_dom_node_t *n;
    uint32_t off = 0;

    if (!b) return JS_EXCEPTION;
    switch (magic) {
    case R_SET_START:
    case R_SET_END:
        if (argc > 1 && JS_ToUint32(ctx, &off, argv[1]) < 0) return JS_EXCEPTION;
        return range_set_bp(ctx, b, nodev, off, magic == R_SET_END) < 0 ? JS_EXCEPTION : JS_UNDEFINED;

    case R_SET_START_BEFORE:
    case R_SET_START_AFTER:
    case R_SET_END_BEFORE:
    case R_SET_END_AFTER: {
        JSValue pw;
        int r;
        n = node_of(nodev);
        DCHECK(n != NULL, "a Range setter reached the body with something that is not a node");
        if (!n->parent)
            return JS_ThrowDOMException(ctx, "InvalidNodeTypeError", "the node has no parent");
        off = node_index(n) + (magic == R_SET_START_AFTER || magic == R_SET_END_AFTER ? 1u : 0u);
        pw = node_wrap(ctx, n->parent);
        r = range_set_bp(ctx, b, pw, off, magic == R_SET_END_BEFORE || magic == R_SET_END_AFTER);
        JS_FreeValue(ctx, pw);
        return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }

    case R_COLLAPSE:
        /* §5.5: `collapse(optional boolean toStart = false)` — to the START means the END moves. */
        if (argc > 0 && JS_ToBool(ctx, argv[0])) bounds_set_end(ctx, b, b->start_node, b->start_off);
        else                                     bounds_set_start(ctx, b, b->end_node, b->end_off);
        return JS_UNDEFINED;

    case R_SELECT_NODE:
        /* §5.5 "select a node". It sets both boundary points DIRECTLY rather than through "set the start", so
           the doctype check is on the node's PARENT type and there is none. */
        n = node_of(nodev);
        DCHECK(n != NULL, "selectNode reached the body with something that is not a node");
        if (!n->parent)
            return JS_ThrowDOMException(ctx, "InvalidNodeTypeError", "the node has no parent");
        {
            JSValue pw = node_wrap(ctx, n->parent);
            uint32_t i = node_index(n);
            bounds_set_start(ctx, b, pw, i);
            bounds_set_end(ctx, b, pw, i + 1);
            JS_FreeValue(ctx, pw);
        }
        return JS_UNDEFINED;

    case R_SELECT_CONTENTS:
        n = node_of(nodev);
        DCHECK(n != NULL, "selectNodeContents reached the body with something that is not a node");
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
            return JS_ThrowDOMException(ctx, "InvalidNodeTypeError",
                                        "a Range cannot select a DocumentType's contents");
        bounds_set_start(ctx, b, nodev, 0);
        bounds_set_end(ctx, b, nodev, node_length(n));
        return JS_UNDEFINED;

    case R_CLONE:
        return range_new(ctx, b->start_node, b->start_off, b->end_node, b->end_off);

    case R_DETACH:
        /* §5.5: "The detach() method steps are to do nothing." The functionality was REMOVED from the standard
           and the member kept for compatibility — the spec's own text, not a stub, and the brand check above is
           what keeps it from being a no-op on anything at all. */
        return JS_UNDEFINED;

    case R_COMPARE_BP: {
        RangeBounds *src;
        uint32_t how = 0;
        lxb_dom_node_t *tn, *sn2;
        uint32_t to, so2;

        if (argc > 0 && JS_ToUint32(ctx, &how, argv[0]) < 0) return JS_EXCEPTION;
        if (how > 3)                                                          /* STEP 1 */
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "the comparison is not one of §5.5's four constants");
        src = JS_GetOpaque(argc > 1 ? argv[1] : JS_UNDEFINED, g_range_class);
        DCHECK(src != NULL, "compareBoundaryPoints reached the body with something that is not a Range");
        cow_capture_host_record(argv[1], src, &RANGE_BOUNDS_REC);
        if (range_root(b) != range_root(src))                                 /* STEP 2 */
            return JS_ThrowDOMException(ctx, "WrongDocumentError", "the two ranges are in different trees");
        /* STEP 4's table: START_TO_START(0) this-start/source-start, START_TO_END(1) this-END/source-start,
           END_TO_END(2) this-end/source-end, END_TO_START(3) this-START/source-end. */
        if (how == 1 || how == 2) { tn = bounds_end(b);   to = b->end_off; }
        else                      { tn = bounds_start(b); to = b->start_off; }
        if (how == 2 || how == 3) { sn2 = bounds_end(src);   so2 = src->end_off; }
        else                      { sn2 = bounds_start(src); so2 = src->start_off; }
        return JS_NewInt32(ctx, boundary_position(tn, to, sn2, so2));
    }

    case R_IS_POINT_IN:
    case R_COMPARE_POINT:
        n = node_of(nodev);
        DCHECK(n != NULL, "a Range point member reached the body with something that is not a node");
        if (argc > 1 && JS_ToUint32(ctx, &off, argv[1]) < 0) return JS_EXCEPTION;
        if (node_root(n) != range_root(b)) {
            /* THE TWO MEMBERS DIVERGE HERE AND NOWHERE ELSE: a point in another tree is simply not in the
               range, and is an error to COMPARE against it. */
            if (magic == R_IS_POINT_IN) return JS_FALSE;
            return JS_ThrowDOMException(ctx, "WrongDocumentError", "the point is in a different tree");
        }
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
            return JS_ThrowDOMException(ctx, "InvalidNodeTypeError", "a DocumentType is not a boundary point");
        if (off > node_length(n))
            return JS_ThrowDOMException(ctx, "IndexSizeError", "the offset is past the node's length");
        if (boundary_position(n, off, bounds_start(b), b->start_off) == BP_BEFORE)
            return magic == R_IS_POINT_IN ? JS_FALSE : JS_NewInt32(ctx, -1);
        if (boundary_position(n, off, bounds_end(b), b->end_off) == BP_AFTER)
            return magic == R_IS_POINT_IN ? JS_FALSE : JS_NewInt32(ctx, 1);
        return magic == R_IS_POINT_IN ? JS_TRUE : JS_NewInt32(ctx, 0);

    default:
        DCHECK(magic == R_INTERSECTS, "a Range member ran with a magic §5.5 does not declare");
        n = node_of(nodev);
        DCHECK(n != NULL, "intersectsNode reached the body with something that is not a node");
        if (node_root(n) != range_root(b)) return JS_FALSE;
        if (!n->parent) return JS_TRUE;   /* a root is intersected by every range of its tree */
        off = node_index(n);
        return JS_NewBool(ctx,
            boundary_position(n->parent, off, bounds_end(b), b->end_off) == BP_BEFORE &&
            boundary_position(n->parent, off + 1, bounds_start(b), b->start_off) == BP_AFTER);
    }
}

/* §5.5's commonAncestorContainer — "getting the common ancestor". No page code, one walk up. */
static JSValue js_range_common_ancestor(JSContext *ctx, JSValueConst this_val, int magic)
{
    RangeBounds *b = range_here(ctx, this_val);
    lxb_dom_node_t *container, *end;

    (void)magic;
    if (!b) return JS_EXCEPTION;
    container = bounds_start(b);
    end = bounds_end(b);
    DCHECK(container != NULL && end != NULL, "a live range's boundary point is not a node");
    while (!node_is_inclusive_ancestor(container, end)) {
        container = container->parent;
        DCHECK(container != NULL, "§5.5's common ancestor walked past the root — the two boundary points would "
                                  "have to be in different trees, which every setter prevents");
    }
    return node_wrap(ctx, container);
}

/* ---- §5.5's STRINGIFIER --------------------------------------------------------------------------------- */

/* IT IS A MACHINE BECAUSE IT WALKS THE PAGE'S TREE. No line of the page's code can run inside it, which is not
   what makes a C body safe to leave un-parkable — being O(1) is, and a range can span a document. It yields
   once per node, which is where a sibling flow overtakes it. */
#define RSTR_STAGES(X) \
    X(RSTR_HEAD, "DOM §5.5 stringification steps 1-3 (the empty string, the both-ends-in-one-Text answer, and " \
                 "the start Text node's tail)") \
    X(RSTR_WALK, "DOM §5.5 stringification step 4 (the data of every contained Text node, one node per step)") \
    X(RSTR_TAIL, "DOM §5.5 stringification steps 5-6 (the end Text node's head, and the result)")
enum { IDL_STEP_STAGE_BASE(RSTR_STAGES) RSTR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RSTR_STEPS[] = { RSTR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct RangeStrState {
    char *buf;
    size_t len, cap;
    lxb_dom_node_t *cursor, *root;
} RangeStrState;

/* WHAT THIS MACHINE OWNS: one plain buffer holding no references. The node pointers are the document's. */
static void rstr_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    RangeStrState *s = st;
    v->buf(ctx, (void **)&s->buf, s->cap);
}

static void rstr_release(JSContext *ctx, void *st)
{
    RangeStrState *s = st;
    (void)ctx;
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
}

static void rstr_append(RangeStrState *s, const char *p, size_t n)
{
    if (s->len + n > s->cap) {
        size_t want = s->cap ? s->cap * 2 : 64;
        char *nb;
        while (want < s->len + n) want *= 2;
        nb = realloc(s->buf, want);
        CHECK(nb != NULL, "the Range stringifier's buffer could not grow");
        s->buf = nb;
        s->cap = want;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
}

static bool node_is_text(const lxb_dom_node_t *n) { return n->type == LXB_DOM_NODE_TYPE_TEXT; }

static void text_bytes(const lxb_dom_node_t *n, const char **p, size_t *len)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
    *p = (const char *)cd->data.data;
    *len = cd->data.length;
}

/* §5.5's "contained in": the node's root is the range's, (node, 0) is after the start, and (node, length) is
   before the end. */
static bool range_contains(const RangeBounds *b, lxb_dom_node_t *n)
{
    return boundary_position(n, 0, bounds_start(b), b->start_off) == BP_AFTER &&
           boundary_position(n, node_length(n), bounds_end(b), b->end_off) == BP_BEFORE;
}

static int js_range_to_string(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    RangeStrState *s = st;
    RangeBounds *b = range_here(ctx, hdr->this_val);
    lxb_dom_node_t *sn, *en;
    const char *p;
    size_t len;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (!b) return JS_STEP_ABRUPT;
    sn = bounds_start(b);
    en = bounds_end(b);
    DCHECK(sn != NULL && en != NULL, "a live range's boundary point is not a node");

    if (hdr->stage == RSTR_HEAD) {
        if (sn == en && node_is_text(sn)) {                                  /* STEP 2 */
            text_bytes(sn, &p, &len);
            DCHECK(b->start_off <= len && b->end_off <= len,
                   "a Range's offsets are past the Text node they name — every setter checks the length");
            *presult = JS_NewStringLen(ctx, p + b->start_off,
                                       b->end_off > b->start_off ? b->end_off - b->start_off : 0);
            return JS_STEP_DONE;
        }
        if (node_is_text(sn)) {                                              /* STEP 3 */
            text_bytes(sn, &p, &len);
            if (b->start_off < len) rstr_append(s, p + b->start_off, len - b->start_off);
        }
        /* THE WALK IS BOUNDED BY THE COMMON ANCESTOR, not by the document and not by the end node. Every
           CONTAINED node is a descendant of the common ancestor, so that subtree is exactly what has to be
           visited — and stopping at the end NODE would miss its own contained children, which is what a range
           whose end is (div, 2) has. */
        s->root = sn;
        while (!node_is_inclusive_ancestor(s->root, en)) {
            s->root = s->root->parent;
            DCHECK(s->root != NULL, "the stringifier's boundary points are in different trees");
        }
        s->cursor = s->root;
        hdr->stage = RSTR_WALK;
        return JS_STEP_YIELD;
    }

    if (hdr->stage == RSTR_WALK) {                                           /* STEP 4 */
        if (!s->cursor) { hdr->stage = RSTR_TAIL; return JS_STEP_YIELD; }
        if (node_is_text(s->cursor) && range_contains(b, s->cursor)) {
            text_bytes(s->cursor, &p, &len);
            rstr_append(s, p, len);
        }
        s->cursor = node_next_in(s->cursor, s->root);
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == RSTR_TAIL, "the Range stringifier resumed into a stage §5.5 does not have");
    if (node_is_text(en)) {                                                  /* STEP 5 */
        text_bytes(en, &p, &len);
        rstr_append(s, p, b->end_off < len ? b->end_off : len);
    }
    *presult = JS_NewStringLen(ctx, s->buf ? s->buf : "", s->len);           /* STEP 6 */
    return JS_STEP_DONE;
}

static const IdlStepDecl RANGE_TO_STRING = { js_range_to_string, sizeof(RangeStrState), rstr_visit, rstr_release,
                                             "DOM §5.5 Range stringification behavior", RSTR_STEPS };

/* ---- DECLARATION AND INSTALL ---------------------------------------------------------------------------- */

/* §5.5's four comparison constants, on both the interface object and the prototype — which is where Web IDL
   §3.7.4 puts an interface's constants. */
static const JSCFunctionListEntry js_range_consts[] = {
    JS_PROP_INT32_DEF("START_TO_START", 0, 0),
    JS_PROP_INT32_DEF("START_TO_END",   1, 0),
    JS_PROP_INT32_DEF("END_TO_END",     2, 0),
    JS_PROP_INT32_DEF("END_TO_START",   3, 0),
};

static int g_id[15], g_id_to_string = -1;

void range_init(JSContext *ctx)
{
    JSClassDef d = { "Range", range_finalizer, range_gc_mark };
    static const IdlArgType NODE_OFFSET[2] = { IDL_INTERFACE, IDL_UNSIGNED_LONG };
    static const IdlArgType ONE_NODE[1] = { IDL_INTERFACE };
    static const IdlArgType ONE_BOOL[1] = { IDL_BOOLEAN };
    static const IdlArgType HOW_RANGE[2] = { IDL_UNSIGNED_SHORT, IDL_INTERFACE };
    int k;

    if (g_range_class) return;   /* one AGENT, one class and one set of pool entries */
    abstract_range_init(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_range_class);
    JS_NewClass(JS_GetRuntime(ctx), g_range_class, &d);
    abstract_range_claim_class(g_range_class);

    for (k = 0; k < 15; k++) g_id[k] = -1;
    g_id[R_SET_START] = idl_method_id(ctx, NODE_OFFSET, 2, js_range_member, R_SET_START);
    idl_iface_brand(node_class_id());
    g_id[R_SET_END] = idl_method_id(ctx, NODE_OFFSET, 2, js_range_member, R_SET_END);
    idl_iface_brand(node_class_id());
    for (k = R_SET_START_BEFORE; k <= R_SET_END_AFTER; k++) {
        g_id[k] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, k);
        idl_iface_brand(node_class_id());
    }
    g_id[R_COLLAPSE] = idl_method_id(ctx, ONE_BOOL, 1, js_range_member, R_COLLAPSE);
    idl_optional_from(0);
    g_id[R_SELECT_NODE] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, R_SELECT_NODE);
    idl_iface_brand(node_class_id());
    g_id[R_SELECT_CONTENTS] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, R_SELECT_CONTENTS);
    idl_iface_brand(node_class_id());
    g_id[R_CLONE] = idl_method_id(ctx, NULL, 0, js_range_member, R_CLONE);
    g_id[R_DETACH] = idl_method_id(ctx, NULL, 0, js_range_member, R_DETACH);
    /* `short compareBoundaryPoints(unsigned short how, Range sourceRange)` — the interface arm brands against
       THIS class, so a StaticRange or a plain object is a TypeError before step 1. */
    g_id[R_COMPARE_BP] = idl_method_id(ctx, HOW_RANGE, 2, js_range_member, R_COMPARE_BP);
    idl_iface_brand(g_range_class);
    g_id[R_IS_POINT_IN] = idl_method_id(ctx, NODE_OFFSET, 2, js_range_member, R_IS_POINT_IN);
    idl_iface_brand(node_class_id());
    g_id[R_COMPARE_POINT] = idl_method_id(ctx, NODE_OFFSET, 2, js_range_member, R_COMPARE_POINT);
    idl_iface_brand(node_class_id());
    g_id[R_INTERSECTS] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, R_INTERSECTS);
    idl_iface_brand(node_class_id());
    g_id_to_string = idl_method_id_step(ctx, NULL, 0, NULL, 0, &RANGE_TO_STRING, 0);

    realm_declare_intrinsic(range_install_proto);
}

void range_install_proto(JSContext *ctx)
{
    JSValue proto, base, prev;

    DCHECK(g_range_class != 0, "a realm asked for Range.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_range_class);
    DCHECK(JS_IsNull(prev), "range_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    base = abstract_range_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "Range.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Range");
    JS_SetPropertyFunctionList(ctx, proto, js_range_consts,
                               (int)(sizeof(js_range_consts) / sizeof(js_range_consts[0])));
    idl_install_accessor(ctx, proto, "commonAncestorContainer", js_range_common_ancestor, 0, -1);
    idl_install_method(ctx, proto, "setStart", 2, g_id[R_SET_START]);
    idl_install_method(ctx, proto, "setEnd", 2, g_id[R_SET_END]);
    idl_install_method(ctx, proto, "setStartBefore", 1, g_id[R_SET_START_BEFORE]);
    idl_install_method(ctx, proto, "setStartAfter", 1, g_id[R_SET_START_AFTER]);
    idl_install_method(ctx, proto, "setEndBefore", 1, g_id[R_SET_END_BEFORE]);
    idl_install_method(ctx, proto, "setEndAfter", 1, g_id[R_SET_END_AFTER]);
    idl_install_method(ctx, proto, "collapse", 0, g_id[R_COLLAPSE]);
    idl_install_method(ctx, proto, "selectNode", 1, g_id[R_SELECT_NODE]);
    idl_install_method(ctx, proto, "selectNodeContents", 1, g_id[R_SELECT_CONTENTS]);
    idl_install_method(ctx, proto, "compareBoundaryPoints", 2, g_id[R_COMPARE_BP]);
    idl_install_method(ctx, proto, "cloneRange", 0, g_id[R_CLONE]);
    idl_install_method(ctx, proto, "detach", 0, g_id[R_DETACH]);
    idl_install_method(ctx, proto, "isPointInRange", 2, g_id[R_IS_POINT_IN]);
    idl_install_method(ctx, proto, "comparePoint", 2, g_id[R_COMPARE_POINT]);
    idl_install_method(ctx, proto, "intersectsNode", 1, g_id[R_INTERSECTS]);
    idl_install_method(ctx, proto, "toString", 0, g_id_to_string);
    JS_SetClassProto(ctx, g_range_class, proto);
}

void range_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    abstract_range_install(ctx, global);
    proto = JS_GetClassProto(ctx, g_range_class);
    DCHECK(!JS_IsNull(proto), "Range was installed in a realm that never ran its prototype install");
    ctor = JS_NewCFunction2(ctx, (JSCFunction *)js_range_ctor, "Range", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the Range interface object could not be allocated");
    JS_SetPropertyFunctionList(ctx, ctor, js_range_consts,
                               (int)(sizeof(js_range_consts) / sizeof(js_range_consts[0])));
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "Range", ctor);
}


/* THE LIST OUTLIVES THE COMPONENT'S TEARDOWN, and it has to. A component's `_free` runs BEFORE the runtime's
   final sweep, so the objects whose finalizers unregister are still alive when it does — freeing the array
   there is a use-after-free at the first finalizer, and asserting the list is empty there asserts an order the
   runtime does not have. So teardown only says "no more registrations are coming", and the LAST finalizer
   frees the array. A list that is already empty is freed at once, which is the ordinary case. */
void range_free(JSContext *ctx)
{
    g_live_closed = 1;
    range_live_drop();
    abstract_range_free(ctx);
}
