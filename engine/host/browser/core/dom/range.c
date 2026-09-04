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
 * EVERY MEMBER §5.5 DECLARES IS HERE, the five that MOVE CONTENT included. This paragraph used to say those
 * five were absent because §4.10 "Interface CharacterData"'s "replace data" and §4.11 "Interface Text"'s
 * "split a Text node" did not exist; both exist, all five are
 * installed below, and the prose outlived the absence it described — which is worse than no prose, because it
 * reads as authoritative and sends the next reader to build what is already built.
 *
 * TWO OF THE MACHINES ARE EXPORTED (range.h): Selection API §3 defines its stringifier and its
 * `deleteFromDocument()` as these very algorithms performed on the range a selection is associated with, so
 * the walk is shared and only the receiver differs. Nothing here knows what a Selection is. */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/dom/abstract_range.h"
#include "core/dom/document.h"
#include "core/dom/element_view.h"
#include "core/dom/node.h"
#include "core/dom/range.h"
#include "core/geometry/dom_rect.h"
#include "core/geometry/dom_rect_list.h"
#include "core/html/fragment_parser.h"   /* HTML §13.4's parse — §8.5.7 is a sixth declaration over it */
#include "core/html/trusted_types.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"
#include "solver/solve.h"

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

/* THE LAST FINALIZER OF A RELEASED AGENT WINDS THE LIST ALL THE WAY BACK, `g_live_closed` INCLUDED. The flag
   says "no more registrations are coming"; once the array is gone there is nothing left for it to be true
   ABOUT, and leaving it set is a slot no release resets — the second agent would open with its live list
   already closed and free the array under every range it built. It cannot be given back on the release column
   (that is the one instant it must be 1), so it is not declared to core/agent_state.h at all; range_init
   asserts its pre-init value instead, which is the moment it is true. */
static void range_live_drop(void)
{
    if (g_live_n || !g_live_closed) return;
    free(g_live);
    g_live = NULL;
    g_live_cap = 0;
    g_live_closed = 0;
}

/* THE COLLECTOR'S TWO ENTRIES READ NO STATIC THIS COMPONENT'S RELEASE RESETS — core/agent_state.h's rule, and
 * both were written breaking it. `range_free` is reached from element_free, which is on core/platform.h's
 * release column, and every host's teardown is `platform_agent_free()` … `JS_RunGC` … `JS_FreeRuntime` in that
 * order — so a Range still live at teardown (every `document.createRange()`, and every Selection's own range)
 * is finalized in a collection that runs AFTER `g_range_class` is back at 0, where `JS_GetOpaque(val, 0)`
 * answers NULL for every one of them. The finalizer would then leak the record and, worse, never REGISTER its
 * departure — so `range_live_drop` would be waiting on a count that no longer has anything to decrement it, and
 * the borrowed-pointer array would outlive the runtime. The mark is the half that takes the runtime down, as
 * agent_state.h records for dom_rect.c: an unmarked child keeps the internal reference gc_decref exists to
 * subtract, so gc_scan reads the two node wrappers a RangeBounds holds as rooted from OUTSIDE the heap and the
 * realm behind them is never collected at all.
 *
 * JS_GetAnyOpaque, because the collector dispatched here THROUGH the class — the id is a fact it already has
 * and must not look up. It is NOT compared against `g_range_class` either: that is the guaranteed-false `@WHY`
 * agent_state.h records for remote_object.c. `range_here` keeps the class test, because that one is Web IDL
 * §3.7.6 Attributes' and §3.7.7 Operations' BRAND — §5.5 declares both member kinds — and runs while the
 * agent is live. */
static void range_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    RangeBounds *b = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!b) return;`. range_new builds the record and JS_SetOpaques it with nothing between that can
       throw, so there is no window in which an object of this class exists without one. */
    DCHECK(b != NULL, "a Range was finalized with no bounds record — range_new mints the object and sets its "
                      "record with nothing in between, so an object of §5.5's class without one was built "
                      "somewhere that is not this component's one mint");
    range_unregister(val);
    range_live_drop();
    range_bounds_release(rt, b);
    free(b);
}

static void range_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    RangeBounds *b = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(b != NULL, "a Range was marked with no bounds record — its two node wrappers are counted references "
                      "and an unmarked child is read by gc_scan as rooted from outside the heap");
    range_bounds_mark(rt, b, mark_func);
}

/* DOES THIS VALUE IMPLEMENT `Range` — Web IDL §3.7 Interfaces' implementation-check an object step 3, "If
   object does not implement interface, then throw a TypeError.", as the PREDICATE core/idl_args' idl_this_iface
   takes. §3.7.7 Operations' create an operation function asks it at step 2.1.2.3, BEFORE step 2.1.4 computes
   the effective overload set, so a member that states it at its DECLARATION refuses a foreign receiver before
   §3.6 Overload resolution algorithm converts an argument. That order is OBSERVABLE here and not a nicety:
   `setStart`'s second position is IDL_UNSIGNED_LONG and `createContextualFragment`'s only one is IDL_DOMSTRING,
   so a body test lets `Range.prototype.setStart.call({}, node, {valueOf(){ … }})` run the PAGE'S OWN CODE and
   throw afterwards, where a browser throws with none of it having run.

   BRAND-CHECKED AGAINST §5.5's OWN CLASS — NOT against AbstractRange's list, because a StaticRange has none of
   these members and `Range.prototype.setStart.call(staticRange)` must be a TypeError rather than a live edit of
   an object the spec says never moves.

   AND IT ANSWERS `b != NULL` FOR FREE, which is what lets a converted body ASSERT where it used to BRANCH: an
   object of this class always carries its record (range_finalizer says why — one mint, nothing between that can
   throw), so this is exactly the test range_here already made, and a receiver it admits has a record in EVERY
   build. The declaration is therefore the guard that survives release; the DCHECK below it asserts only that
   the declaration was made, so nothing here promotes a dev-only check into a release dereference. */
static bool range_is(JSValueConst v)
{
    return JS_GetOpaque(v, g_range_class) != NULL;
}

/* THE RECORD FOR A RECEIVER §3.7 HAS ALREADY ADMITTED — every §5.5 OPERATION, whose declaration states the
   interface above. Reaching a body means idl_implementation_check ran and passed, so the only condition left
   for this to fire on is a member installed WITHOUT its brand, which is this engine's own routing being wrong
   and not a fact about page input. */
static RangeBounds *range_receiver(JSValueConst v)
{
    RangeBounds *b = JS_GetOpaque(v, g_range_class);

    DCHECK(b != NULL, "a §5.5 member reached its body on a receiver that is not a Range — its declaration "
                      "states Web IDL §3.7 Interfaces' implementation check, so reaching the body means "
                      "idl_implementation_check did not run for it");
    cow_capture_host_record(v, b, &RANGE_BOUNDS_REC);
    return b;
}

/* THE SAME QUESTION FOR THE ONE MEMBER THAT CANNOT STATE IT. `commonAncestorContainer` is minted by
   idl_install_accessor as a plain JS_CFUNC_getter_magic with no pool entry, so it converges on nothing that
   could ask §3.7 for it — the residual core/idl_args.c names at the site it would reach. ONE ANSWER TO ONE
   QUESTION: this routes to the predicate above, so the two ways into a §5.5 member cannot drift. When a plain
   getter gains a pool entry, this function goes with it. */
static RangeBounds *range_here(JSContext *ctx, JSValueConst v)
{
    if (!range_is(v)) {
        JS_ThrowTypeError(ctx, "not a Range");
        return NULL;
    }
    return range_receiver(v);
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

/* §4.10 "replace data" STEPS 8-11. The splice itself is §4.10's — what the boundary points do about it is
   §5.5's, which is why this is a call from there rather than a second copy of the registry walk there.
   STEPS 8-9 AND 10-11 CANNOT INTERFERE, and that is why they read as four independent passes in one loop: 8
   only ever writes `offset`, which is never greater than `offset + count`, so a boundary point 8 moved can
   never satisfy 10's test. Writing them as an if/else would be a different algorithm. */
void range_replace_data_steps(JSContext *ctx, lxb_dom_node_t *node, uint32_t offset, uint32_t count,
                              uint32_t data_units)
{
    uint32_t end = offset + count;
    int i;

    (void)ctx;
    DCHECK(node != NULL, "§4.10's live-range steps ran for no node");
    for (i = 0; i < g_live_n; i++) {
        /* abstract_range_of, not JS_GetOpaque: reaching the record is what puts it in the running flow's
           delta, which is what makes one flow's adjustment invisible to a sibling. */
        RangeBounds *b = abstract_range_of(g_live[i]);

        DCHECK(b != NULL, "the live-range list holds something that is not a range");
        if (bounds_start(b) == node) {
            if (b->start_off > offset && b->start_off <= end) b->start_off = offset;       /* STEP 8  */
            else if (b->start_off > end)                      b->start_off += data_units - count;  /* 10 */
        }
        if (bounds_end(b) == node) {
            if (b->end_off > offset && b->end_off <= end)     b->end_off = offset;         /* STEP 9  */
            else if (b->end_off > end)                        b->end_off += data_units - count;    /* 11 */
        }
    }
}

/* §4.11 "Interface Text"'s "split a Text node" STEPS 7.2-7.5. Step 7.1's insert has already run, and its OWN
   live-range step
   (§4.2.3 insert step 6) has already moved every boundary point in the parent whose offset is GREATER than the
   new node's index; 7.4 and 7.5 are what handles the one that is EQUAL to it. The two halves together are
   "every offset at or after the split point moves along by one", which is why neither is redundant. */
void range_split_text_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *new_node, uint32_t offset)
{
    lxb_dom_node_t *parent;
    JSValue nw;
    uint32_t idx;
    int i;

    DCHECK(node != NULL && new_node != NULL, "§4.11's split live-range steps ran for no node");
    parent = node->parent;
    DCHECK(parent != NULL, "§4.11 split step 7 runs only for a Text node that HAS a parent — step 8 is the "
                           "parentless case and has no live-range steps at all");
    if (!g_live_n) return;
    idx = node_index(node);
    nw = node_wrap(ctx, new_node);
    for (i = 0; i < g_live_n; i++) {
        RangeBounds *b = abstract_range_of(g_live[i]);

        DCHECK(b != NULL, "the live-range list holds something that is not a range");
        /* STEPS 7.2-7.3: a boundary point PAST the split point belongs to the new node now. */
        if (bounds_start(b) == node && b->start_off > offset)
            bounds_set_start(ctx, b, nw, b->start_off - offset);
        if (bounds_end(b) == node && b->end_off > offset)
            bounds_set_end(ctx, b, nw, b->end_off - offset);
        /* STEPS 7.4-7.5. Re-read: 7.2 may have just moved the boundary point onto `new_node`, which is not
           `parent`, so the two tests are disjoint — but reading a stale pointer here is the bug that shape
           invites, and the standard runs the four in this order for exactly that reason. */
        if (bounds_start(b) == parent && b->start_off == idx + 1) b->start_off++;
        if (bounds_end(b) == parent && b->end_off == idx + 1)     b->end_off++;
    }
    JS_FreeValue(ctx, nw);
}

/* §4.4 `normalize()` STEPS 6.1-6.4. The standard concatenates every contiguous Text node's data first and
   then walks `currentNode` forward adjusting the ranges; this engine's normalize absorbs ONE sibling per step
   (it is a machine, and the run is the page's), so the walk arrives here one `currentNode` at a time with the
   `length` the standard's accumulator would hold at that iteration. Same algorithm, same order, one sibling
   of it per call. */
void range_normalize_absorb_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *sib, uint32_t length)
{
    lxb_dom_node_t *parent;
    JSValue nw;
    uint32_t idx;
    int i;

    DCHECK(node != NULL && sib != NULL, "§4.4's normalize live-range steps ran for no node");
    DCHECK(sib->parent != NULL && sib->parent == node->parent,
           "§4.4's normalize absorbs a CONTIGUOUS sibling, so the two share a parent");
    if (!g_live_n) return;
    parent = sib->parent;
    idx = node_index(sib);
    nw = node_wrap(ctx, node);
    for (i = 0; i < g_live_n; i++) {
        RangeBounds *b = abstract_range_of(g_live[i]);

        DCHECK(b != NULL, "the live-range list holds something that is not a range");
        /* STEPS 6.1-6.2: a boundary point INSIDE the absorbed sibling lands at the same character of the
           node that absorbed it. */
        if (bounds_start(b) == sib) bounds_set_start(ctx, b, nw, b->start_off + length);
        if (bounds_end(b) == sib)   bounds_set_end(ctx, b, nw, b->end_off + length);
        /* STEPS 6.3-6.4: a boundary point in the PARENT naming the sibling's own position lands at the seam.
           Re-read for the reason §5.5's LIVE RANGE pre-remove steps re-read: 6.1 may have just moved it. The
           standard's term is "live range pre-remove steps" and §5.5 is where it defines them; the unqualified
           "pre-remove" is §4.2.3 "Mutation algorithms"' OWN operation, which is a different algorithm. */
        if (bounds_start(b) == parent && b->start_off == idx) bounds_set_start(ctx, b, nw, length);
        if (bounds_end(b) == parent && b->end_off == idx)     bounds_set_end(ctx, b, nw, length);
    }
    JS_FreeValue(ctx, nw);
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

JSValue range_new_bp(JSContext *ctx, JSValueConst snode, uint32_t soff, JSValueConst enode, uint32_t eoff)
{
    lxb_dom_node_t *sn = node_of(snode), *en = node_of(enode);

    DCHECK(sn != NULL && en != NULL, "a live range was asked for at a boundary point that is not a node");
    DCHECK(node_root(sn) == node_root(en),
           "a live range was asked for across two trees — §5.2's position is undefined between them, so every "
           "caller that mints a range decides which point is first and cannot have done so here");
    DCHECK(boundary_position(sn, soff, en, eoff) != BP_AFTER,
           "a live range was asked for with its start AFTER its end — §5.5's invariant is that it never is, "
           "and every §5.5 setter maintains it by moving the other point; a caller stating two points states "
           "them in order");
    return range_new(ctx, snode, soff, enode, eoff);
}

JSClassID range_class_id(void) { return g_range_class; }

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

/* §5.5 "insert a node into a live range" — declared here because `insertNode` is one of the members below and
   `surroundContents`' step 5 is the same algorithm; both reach the one implementation further down. */
static int range_insert_node(JSContext *ctx, RangeBounds *b, lxb_dom_node_t *node);

/* §5.5's SIXTEEN PLAIN OPERATIONS, EACH PAIRED WITH THE SENTENCE ITS OWN POOL ENTRY IS REPORTED BY. The enum
   below and the declaration loop in range_init are two readings of this ONE list, so a member §5.5 gains cannot
   acquire a pool entry whose forgotten release is then named after a neighbour — which is core/agent_state.h's
   whole complaint about one shared `what` for many slots. ORDER IS LOAD-BEARING and is §5.5's own IDL order:
   the four relative setters are reached by js_range_member and declared by range_init as the one contiguous
   run R_SET_START_BEFORE..R_SET_END_AFTER, so a member inserted into the middle of it would silently widen
   both. */
#define RANGE_OPERATIONS(X)                                                                          \
    X(R_SET_START,        "DOM §5.5's setStart declaration")                                          \
    X(R_SET_END,          "DOM §5.5's setEnd declaration")                                            \
    X(R_SET_START_BEFORE, "DOM §5.5's setStartBefore declaration")                                    \
    X(R_SET_START_AFTER,  "DOM §5.5's setStartAfter declaration")                                     \
    X(R_SET_END_BEFORE,   "DOM §5.5's setEndBefore declaration")                                      \
    X(R_SET_END_AFTER,    "DOM §5.5's setEndAfter declaration")                                       \
    X(R_COLLAPSE,         "DOM §5.5's collapse declaration")                                          \
    X(R_SELECT_NODE,      "DOM §5.5's selectNode declaration")                                        \
    X(R_SELECT_CONTENTS,  "DOM §5.5's selectNodeContents declaration")                                \
    X(R_CLONE,            "DOM §5.5's cloneRange declaration")                                        \
    X(R_DETACH,           "DOM §5.5's detach declaration")                                            \
    X(R_COMPARE_BP,       "DOM §5.5's compareBoundaryPoints declaration")                             \
    X(R_IS_POINT_IN,      "DOM §5.5's isPointInRange declaration")                                    \
    X(R_COMPARE_POINT,    "DOM §5.5's comparePoint declaration")                                      \
    X(R_INTERSECTS,       "DOM §5.5's intersectsNode declaration")                                    \
    X(R_INSERT_NODE,      "DOM §5.5's insertNode declaration")
enum {
#define X(m, w) m,
    RANGE_OPERATIONS(X)
#undef X
    R_MEMBER_N
};
static const char *const R_WHAT[R_MEMBER_N] = {
#define X(m, w) w,
    RANGE_OPERATIONS(X)
#undef X
};

static JSValue js_range_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    RangeBounds *b = range_receiver(this_val);
    JSValueConst nodev = argc > 0 ? argv[0] : JS_UNDEFINED;
    lxb_dom_node_t *n;
    uint32_t off = 0;

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

    case R_INSERT_NODE:
        /* §5.5 `[CEReactions] undefined insertNode(Node node)` — the wrapper's reactions are the IDL machine's
           and run at this member's boundary like every other declared member's. */
        n = node_of(nodev);
        DCHECK(n != NULL, "insertNode reached the body with something that is not a node");
        return range_insert_node(ctx, b, n) < 0 ? JS_EXCEPTION : JS_UNDEFINED;

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

/* §5.5 "get the common ancestor of a live range" — the concept, over a pair of boundary points rather than
   over a Range object, because every content-moving member below asks it of a SUBRANGE that is not one. */
static lxb_dom_node_t *common_ancestor(lxb_dom_node_t *start, lxb_dom_node_t *end)
{
    lxb_dom_node_t *container = start;

    DCHECK(start != NULL && end != NULL, "a live range's boundary point is not a node");
    while (!node_is_inclusive_ancestor(container, end)) {
        container = container->parent;
        DCHECK(container != NULL, "§5.5's common ancestor walked past the root — the two boundary points would "
                                  "have to be in different trees, which every setter prevents");
    }
    return container;
}

/* §5.5's commonAncestorContainer. No page code, one walk up. */
static JSValue js_range_common_ancestor(JSContext *ctx, JSValueConst this_val, int magic)
{
    RangeBounds *b = range_here(ctx, this_val);

    (void)magic;
    if (!b) return JS_EXCEPTION;
    return node_wrap(ctx, common_ancestor(bounds_start(b), bounds_end(b)));
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
const char *const RANGE_STR_STEPS[] = { RSTR_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* WHAT THIS MACHINE OWNS: one plain buffer holding no references. The node pointers are the document's. */
void range_str_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    RangeStrState *s = st;
    v->buf(ctx, (void **)&s->buf, s->cap);
}

/* THE RUNTIME'S ALLOCATOR, BECAUSE THE DECLARATION'S IS. `v->buf` hands the sibling a copy made with
   js_malloc and the teardown discharges it with js_free, so a buffer grown with the C library's realloc is one
   the fork frees through the wrong allocator — the runtime's accounting goes backwards by a block it never
   issued. It is invisible today only because nothing has forked mid-stringification. */
static void rstr_append(JSContext *ctx, RangeStrState *s, const char *p, size_t n)
{
    if (s->len + n > s->cap) {
        size_t want = s->cap ? s->cap * 2 : 64;
        char *nb;
        while (want < s->len + n) want *= 2;
        nb = js_realloc(ctx, s->buf, want);
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

/* THE BYTES OF A CHARACTER-DATA SLICE NAMED IN CODE UNITS — [from, to) of §4.4's length, which is what every
   boundary point in this file holds. A slice taken by indexing the byte store with a code-unit offset is the
   defect node_length carried: it lands mid-sequence on any non-ASCII text and cuts a code point in half. */
static void text_slice(const lxb_dom_node_t *n, uint32_t from, uint32_t to, const char **p, size_t *len)
{
    size_t a = node_cd_byte_of(n, from), b = node_cd_byte_of(n, to);
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;

    DCHECK(a <= b, "a §5 slice was asked for a range whose end precedes its start");
    *p = (const char *)cd->data.data + a;
    *len = b - a;
}

/* §5.5's "contained in": the node's root is the range's, (node, 0) is after the start, and (node, length) is
   before the end. Stated over the four raw values rather than over a Range, because §5.5's extract asks it of
   a SUBRANGE that is never a Range object. */
static bool bp_contains(lxb_dom_node_t *sn, uint32_t so, lxb_dom_node_t *en, uint32_t eo, lxb_dom_node_t *n)
{
    DCHECK(node_root(n) == node_root(sn), "§5.5's `contained in` was asked about a node in another tree — every "
                                          "caller reaches it through the common ancestor, which is in this one");
    return boundary_position(n, 0, sn, so) == BP_AFTER &&
           boundary_position(n, node_length(n), en, eo) == BP_BEFORE;
}

/* §5.5's "partially contained in": an inclusive ancestor of the range's start node but NOT of its end node, or
   the other way round. */
static bool bp_partially_contains(lxb_dom_node_t *sn, lxb_dom_node_t *en, lxb_dom_node_t *n)
{
    return node_is_inclusive_ancestor(n, sn) != node_is_inclusive_ancestor(n, en);
}

static bool range_contains(const RangeBounds *b, lxb_dom_node_t *n)
{
    return bp_contains(bounds_start(b), b->start_off, bounds_end(b), b->end_off, n);
}

int range_str_step(JSContext *ctx, JSStepHdr *hdr, RangeStrState *s, RangeBounds *b, JSValue *presult)
{
    lxb_dom_node_t *sn, *en;
    const char *p;
    size_t len;

    DCHECK(b != NULL, "§5.5's stringification stepped with no bounds — the caller resolves the subject before "
                      "every step and an unresolvable one is its own abrupt, not this machine's");
    sn = bounds_start(b);
    en = bounds_end(b);
    DCHECK(sn != NULL && en != NULL, "a live range's boundary point is not a node");

    if (hdr->stage == RSTR_HEAD) {
        if (sn == en && node_is_text(sn)) {                                  /* STEP 2 */
            DCHECK(b->start_off <= node_length(sn) && b->end_off <= node_length(sn),
                   "a Range's offsets are past the Text node they name — every setter checks the length");
            text_slice(sn, b->start_off, b->end_off > b->start_off ? b->end_off : b->start_off, &p, &len);
            *presult = JS_NewStringLen(ctx, p, len);
            return JS_STEP_DONE;
        }
        if (node_is_text(sn)) {                                              /* STEP 3 */
            text_slice(sn, b->start_off, node_length(sn), &p, &len);
            rstr_append(ctx, s, p, len);
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
            rstr_append(ctx, s, p, len);
        }
        s->cursor = node_next_in(s->cursor, s->root);
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == RSTR_TAIL, "the Range stringifier resumed into a stage §5.5 does not have");
    if (node_is_text(en)) {                                                  /* STEP 5 */
        text_slice(en, 0, b->end_off, &p, &len);
        rstr_append(ctx, s, p, len);
    }
    *presult = JS_NewStringLen(ctx, s->buf ? s->buf : "", s->len);           /* STEP 6 */
    return JS_STEP_DONE;
}

/* §5.5's OWN member: the subject is the receiver. */
static int js_range_to_string(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    RangeBounds *b = range_receiver(hdr->this_val);

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    return range_str_step(ctx, hdr, st, b, presult);
}

/* No release: the accumulator is range_str_visit's and the teardown discharges that one list. */
static const IdlStepDecl RANGE_TO_STRING = { js_range_to_string, sizeof(RangeStrState), range_str_visit, NULL,
                                             "DOM §5.5 Range stringification behavior", RANGE_STR_STEPS };

/* ---- §5.5's CONTENT-MOVING MEMBERS ----------------------------------------------------------------------
 *
 * `deleteContents`, `extractContents`, `cloneContents`, `insertNode` and `surroundContents`. They were ABSENT,
 * and the reason recorded for that — that §4.10's "replace data" and §4.11's "split" did not exist — is no
 * longer true: both are built, and this file now runs their live-range steps.
 *
 * THEY ARE MACHINES BECAUSE THEY WALK THE PAGE'S TREE. `deleteContents` collects every contained node in tree
 * order and `extractContents` descends the whole spine between each boundary point and the common ancestor;
 * both are O(document) and neither may hold the scheduler for it. They rest one node per step.
 *
 * EXTRACT AND CLONE ARE ONE MACHINE, and that is not a merge of two algorithms — it is a refusal to write the
 * partial-containment rule twice. The standard states them as two lists that differ in exactly three places
 * (extract repositions the range, extract MOVES a contained child where clone deep-copies it, extract empties
 * the CharacterData it took bytes from), and every other line is word-for-word the same. `move` names those
 * three places; two bodies would be two places for "which child is partially contained" to drift.
 *
 * THE SPEC'S RECURSION IS AN EXPLICIT FRAME STACK. Extracting a subrange recurses once per level between the
 * boundary node and the common ancestor, and that depth is the PAGE's — a C recursion there is an unbounded C
 * stack in an engine whose whole point is that the C stack is flat. */

/* §5.5's "creating a document fragment given range's start node's node document". Flow-private until the page
   is handed it, exactly like a clone. */
static lxb_dom_node_t *range_new_fragment(lxb_dom_node_t *of)
{
    lxb_dom_document_fragment_t *f;

    DCHECK(of != NULL, "§5.5 asked for a fragment on no document");
    f = lxb_dom_document_fragment_interface_create(of->owner_document);
    CHECK(f != NULL, "§5.5: the Lexbor fragment allocation failed — handing the page a null it cannot tell "
                     "from a fragment it never asked for is not an option");
    dom_cow_note_created(lxb_dom_interface_node(f));
    return lxb_dom_interface_node(f);
}

/* "Set clone's data to the result of substringing data of `src` with [from, to)". A brand-new clone has no live
   range pointing into it, so this is the raw write §4.10's `data` field is, not a "replace data". */
static void range_clone_data(lxb_dom_node_t *clone, lxb_dom_node_t *src, uint32_t from, uint32_t to)
{
    const char *p;
    size_t len;

    text_slice(src, from, to, &p, &len);
    dom_cow_set_text(clone, p, len);
}

/* §4.10 "replace data of `n` with (offset, count, "")" — the emptying half of extract and deleteContents. */
static void range_delete_data(JSContext *ctx, lxb_dom_node_t *n, uint32_t offset, uint32_t count)
{
    JSValue r = node_cd_replace_data(ctx, n, offset, count, "", 0);

    DCHECK(!JS_IsException(r), "§5.5 handed §4.10 an offset past the node it had just measured");
    JS_FreeValue(ctx, r);
}

static bool node_is_chardata_kind(const lxb_dom_node_t *n)
{
    return n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT ||
           n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
           n->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION;
}

/* §5.5 extract steps 12-14 / deleteContents steps 5-7 — WHERE THE RANGE LANDS. Written once because the two
   algorithms state it identically, down to the note explaining why referenceNode's parent cannot be null. */
static void range_collapse_point(lxb_dom_node_t *sn, lxb_dom_node_t *en, uint32_t so,
                                 lxb_dom_node_t **new_node, uint32_t *new_off)
{
    if (node_is_inclusive_ancestor(sn, en)) {
        *new_node = sn;
        *new_off = so;
        return;
    }
    {
        lxb_dom_node_t *ref = sn;
        while (ref->parent && !node_is_inclusive_ancestor(ref->parent, en))
            ref = ref->parent;
        DCHECK(ref->parent != NULL, "§5.5's collapse point walked to a parentless node — it would be the "
                                    "range's root, and a root is an inclusive ancestor of the end node, so "
                                    "the loop above cannot have reached it");
        *new_node = ref->parent;
        *new_off = node_index(ref) + 1;
    }
}

/* ---- extract / clone the contents ----------------------------------------------------------------------- */

/* EVERY "LET CLONE BE A CLONE OF …" IS §4.4's `clone a node`, PERFORMED — there are six of them here (steps 4,
   16, 17, 19 and 20's shallow clones and step 14's "with subtree set to true"), and this file used to copy the
   node itself instead. That was not one algorithm written twice, it was a DIFFERENT algorithm: §4.4's step 3
   (HTML's cloning steps for `input`, `textarea`, `script` and `template`) and its step 6 (a clonable shadow
   root, cloned even when subtree is false) were missing from every one of the six, silently and per node. So
   the algorithm's own stage block is declared here, inside this member's, and node.c's body runs in it — the
   same delegation surroundContents' step 3 makes for `extract`, and for the same reason. */
#define RX_STAGES(X) \
    X(RX_ENTER,     "DOM §5.5 extract steps 1-4.1 / clone the contents steps 1-4.1 (the fragment, the collapsed " \
                    "range, and — both ends in one CharacterData node — a clone of the start node)") \
    X(RX_ENTER_CLONED, "DOM §5.5 extract steps 4.2-4.5 / clone the contents steps 4.2-4.4 (the clone's data, " \
                    "appending it to the fragment, and — extracting — emptying what it took)") \
    X(RX_LOCATE,    "DOM §5.5 extract steps 5-15 / clone the contents steps 5-11 (the common ancestor, the two " \
                    "partially contained children, the contained children, the doctype check — and, extracting " \
                    "only, where the range lands)") \
    X(RX_FIRST,     "DOM §5.5 extract steps 16.1 and 17.1 / clone the contents steps 12.1 and 13.1 (a clone of " \
                    "the first partially contained child)") \
    X(RX_FIRST_CLONED, "DOM §5.5 extract steps 16.2-16.4 and 17.2-17.3 / clone the contents steps 12.2-12.3 " \
                    "and 13.2-13.3 (the clone's data, or the subrange whose fragment it receives)") \
    X(RX_CONTAINED, "DOM §5.5 extract step 18 / clone the contents step 14.1 (one contained child per step: " \
                    "extract APPENDS it, clone the contents clones it with subtree set to true)") \
    X(RX_CONTAINED_CLONED, "DOM §5.5 clone the contents step 14.2 (append the cloned contained child to " \
                    "fragment)") \
    X(RX_LAST,      "DOM §5.5 extract steps 19.1 and 20.1 / clone the contents steps 15.1 and 16.1 (a clone of " \
                    "the last partially contained child)") \
    X(RX_LAST_CLONED, "DOM §5.5 extract steps 19.2-19.4 and 20.2-20.3 / clone the contents steps 15.2-15.3 " \
                    "and 16.2-16.3 (the clone's data, or the subrange whose fragment it receives)") \
    X(RX_LEAVE,     "DOM §5.5 extract step 21 / clone the contents step 17 (return fragment — a subrange's " \
                    "fragment is appended to its clone on the way out)") \
    NODE_CLONE_ALGO_STAGES(X, RX_CLONE, "DOM §5.5 extract / clone the contents: `a clone of` a boundary node " \
                    "or a contained child")
/* SURROUNDCONTENTS' STEP 3 IS `extract this`, SO ITS STAGES ARE THE EXTRACTION'S — declared here, in ITS
   numbering, and NOT held in a private byte of its state. A machine rests on `hdr->stage` and the driver
   asserts that the stage it holds is a step its declaration names; a delegated algorithm keeping its own
   counter is a resume point nothing can check, which is the one thing a step machine may not have. The
   extraction body is therefore written against a BASE: `hdr->stage - base` is its phase, and the two callers
   pass the base of the block they declared. */
#define RS_STAGES(X) \
    X(RS_CHECK,   "DOM §5.5 surroundContents steps 1-2 (the partially contained non-Text node, and what " \
                  "newParent may be)") \
    X(RS_X_ENTER,     "DOM §5.5 surroundContents step 3 → extract steps 1-4.1") \
    X(RS_X_ENTER_CLONED, "DOM §5.5 surroundContents step 3 → extract steps 4.2-4.5") \
    X(RS_X_LOCATE,    "DOM §5.5 surroundContents step 3 → extract steps 5-15") \
    X(RS_X_FIRST,     "DOM §5.5 surroundContents step 3 → extract steps 16.1 and 17.1") \
    X(RS_X_FIRST_CLONED, "DOM §5.5 surroundContents step 3 → extract steps 16.2-16.4 and 17.2-17.3") \
    X(RS_X_CONTAINED, "DOM §5.5 surroundContents step 3 → extract step 18, one contained child per step") \
    X(RS_X_CONTAINED_CLONED, "DOM §5.5 surroundContents step 3 → extract step 18 (an extraction MOVES a " \
                  "contained child, so this stage is the one its clone-the-contents twin rests at)") \
    X(RS_X_LAST,      "DOM §5.5 surroundContents step 3 → extract steps 19.1 and 20.1") \
    X(RS_X_LAST_CLONED, "DOM §5.5 surroundContents step 3 → extract steps 19.2-19.4 and 20.2-20.3") \
    X(RS_X_LEAVE,     "DOM §5.5 surroundContents step 3 → extract step 21") \
    NODE_CLONE_ALGO_STAGES(X, RS_X_CLONE, "DOM §5.5 surroundContents step 3 → extract: `a clone of` a " \
                  "partially contained boundary node") \
    X(RS_WRAP,    "DOM §5.5 surroundContents steps 4-7 (empty newParent, insert it, append the fragment, and " \
                  "select it)")
/* TWO ENUMS, EACH BASED AT IDL_STEP_FIRST, because each is a DECLARATION's own stage block and `steps[0]` is
   the stage a member is entered at. Numbering surroundContents' block after the extraction's would leave its
   first stage naming the extraction's first step, which is a label that lies about where a parked flow is. */
enum { IDL_STEP_STAGE_BASE(RX_STAGES) RX_STAGES(JS_STEP_STAGE_ENUM) };
enum { IDL_STEP_STAGE_BASE(RS_STAGES) RS_STAGES(JS_STEP_STAGE_ENUM) };
/* THE TWO BLOCKS ARE ONE SHAPE, and the body is what depends on it: it writes `base + (STAGE - RX_ENTER)` and
   reads `hdr->stage - base + RX_ENTER`, so a stage present in one caller's copy and not the other's would
   silently point a resume at its neighbour. Stated as a compile-time assertion rather than trusted, because
   the two lists are edited one at a time and this is the moment the drift becomes invisible. */
_Static_assert(RS_X_CLONE_LEAVE - RS_X_ENTER == RX_CLONE_LEAVE - RX_ENTER,
               "surroundContents' copy of the extraction's stage block is not the same length as the "
               "extraction's own — one of the two lists gained a stage the other did not");
static const char *const RX_STEPS[] = { RX_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* WHICH OF THE TWO ALGORITHMS ONE BODY IS RUNNING. Spelled in full because §4.4's own algorithm now
   declares stages in this member's block: `RX_CLONE_ROOT` is a step of `clone a node` and this is a
   member of §5.5, and one prefix for both would read as if the two were related. */
enum { RX_EXTRACT = 0, RX_CLONE_CONTENTS };

/* ONE LEVEL OF THE SPEC'S RECURSION. Its boundary points are the four values step 3 snapshots, not a live
   Range: the standard says "a new live range" and the difference is unobservable, because a subrange is
   created and consumed with no page code in between and nothing reads it after its own step 15. What IS
   load-bearing is that the OUTER range is the page's real one — step 15 moves it, and the removals that follow
   move it again through the pre-remove steps.
   Every pointer here is the DOCUMENT'S. A node this walk puts in a fragment is still a node; nothing frees one
   under a parked flow. */
typedef struct RxFrame {
    lxb_dom_node_t  *sn, *en;       /* step 3's originalStartNode / originalEndNode */
    uint32_t         so, eo;        /* step 3's originalStartOffset / originalEndOffset */
    lxb_dom_node_t  *frag;          /* step 1's fragment */
    lxb_dom_node_t  *into;          /* the clone this frame's fragment is appended to; NULL at the top */
    lxb_dom_node_t **contained;     /* step 10's containedChildren, SNAPSHOTTED — the walk removes siblings */
    int              nc, ccap, ci;
    lxb_dom_node_t  *first_pcc, *last_pcc;
    uint8_t          after;         /* the stage the PARENT resumes at when this frame leaves */
} RxFrame;

typedef struct RxState {
    RxFrame *f;
    int      sp, cap;
    /* §4.4's `clone a node`, PERFORMED — one walk at a time, which is what the algorithm's own start asserts:
       every site here starts a clone and resumes at the stage that consumes it, so a second can only begin
       once the first has handed its copy back. */
    NodeCloneState nc;
} RxState;

static void rx_frame_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    RxFrame *f = elem;
    v->buf(ctx, (void **)&f->contained, sizeof(lxb_dom_node_t *) * (size_t)f->ccap);
}

static void rx_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    RxState *s = st;
    v->array(ctx, (void **)&s->f, sizeof(RxFrame), s->sp, s->cap, rx_frame_visit);
    node_clone_visit_state(ctx, &s->nc, v);   /* the delegated algorithm's own allocation is its to declare */
}

/* THE RUNTIME'S ALLOCATOR, BECAUSE THE DECLARATION'S IS — see rstr_append. `v->array` copies this stack with
   js_malloc for the sibling and frees it with js_free, and each frame's `contained` goes the same way through
   rx_frame_visit, so the growth has to come from the same allocator the fork and the teardown use. */
static RxFrame *rx_push(JSContext *ctx, RxState *s)
{
    RxFrame *f;

    if (s->sp == s->cap) {
        int want = s->cap ? s->cap * 2 : 4;
        RxFrame *a = js_realloc(ctx, s->f, sizeof(RxFrame) * (size_t)want);
        CHECK(a != NULL, "§5.5's extract could not grow its frame stack — the spine between a boundary point "
                         "and the common ancestor is the page's depth, and dropping a level would silently "
                         "extract part of a subtree");
        s->f = a;
        s->cap = want;
    }
    f = &s->f[s->sp++];
    memset(f, 0, sizeof(*f));
    return f;
}

static void rx_contained_add(JSContext *ctx, RxFrame *f, lxb_dom_node_t *n)
{
    if (f->nc == f->ccap) {
        int want = f->ccap ? f->ccap * 2 : 8;
        lxb_dom_node_t **a = js_realloc(ctx, f->contained, sizeof(*a) * (size_t)want);
        CHECK(a != NULL, "§5.5's extract could not grow its contained-children list");
        f->contained = a;
        f->ccap = want;
    }
    f->contained[f->nc++] = n;
}

/* THE EXTRACTION, over a stage BLOCK that starts at `base`. `move` is what separates extract from clone: it
   repositions the range, it MOVES a contained child rather than copying it, and it empties the CharacterData
   it took bytes from. Everything else is one algorithm. */
/* `base` is where this caller's copy of the stage block starts; the phase is the offset into it. */
#define RXS(x) (base + ((x) - RX_ENTER))
static int rx_run(JSContext *ctx, JSStepHdr *hdr, RxState *s, int move, int base, JSValue *presult)
{
    RxFrame *f;

    if (!s->sp) {
        /* THE TOP FRAME IS THE RECEIVER'S OWN RANGE — read here and nowhere else, because everything below is
           stated over the four values step 3 snapshots. */
        RangeBounds *b = range_receiver(hdr->this_val);
        f = rx_push(ctx, s);
        f->sn = bounds_start(b);
        f->so = b->start_off;
        f->en = bounds_end(b);
        f->eo = b->end_off;
        DCHECK(f->sn != NULL && f->en != NULL, "a live range's boundary point is not a node");
        hdr->stage = RXS(RX_ENTER);
    }
    /* `A CLONE OF` — §4.4's algorithm, in the block this member declared for it. It is routed BEFORE the switch
       because its stages are this member's too: the switch below maps a stage onto this file's own steps, and
       the clone's are node.c's. The algorithm points the stage back at the site that started it. */
    if (hdr->stage >= RXS(RX_CLONE_ROOT)) {
        DCHECK(hdr->stage <= RXS(RX_CLONE_LEAVE),
               "§5.5 resumed past the stage block it declared for `clone a node` — the algorithm's six stages "
               "are the LAST of this member's, so there is nothing above them to resume into");
        return node_clone_run(ctx, hdr, &s->nc, RXS(RX_CLONE_ROOT));
    }

    f = &s->f[s->sp - 1];

    switch (hdr->stage - base + RX_ENTER) {
    case RX_ENTER:
        f->frag = range_new_fragment(f->sn);                                  /* STEP 1 */
        if (f->sn == f->en && f->so == f->eo) { hdr->stage = RXS(RX_LEAVE); return JS_STEP_YIELD; }   /* STEP 2 */
        if (f->sn == f->en && node_is_chardata_kind(f->sn)) {                 /* STEP 4.1 */
            node_clone_start(hdr, &s->nc, f->sn, false, RXS(RX_CLONE_ROOT), RXS(RX_ENTER_CLONED));
            return JS_STEP_YIELD;
        }
        hdr->stage = RXS(RX_LOCATE);
        return JS_STEP_YIELD;

    case RX_ENTER_CLONED:                                                     /* STEPS 4.2-4.4 */
        range_clone_data(s->nc.copy, f->sn, f->so, f->eo);
        node_insert_at(f->frag, s->nc.copy, NULL);
        /* Extract's step 4.4 only: the bytes the fragment now holds leave the original. Cloning the contents
           has no such step, which is the whole of the difference `move` names. */
        if (move) range_delete_data(ctx, f->sn, f->so, f->eo - f->so);
        hdr->stage = RXS(RX_LEAVE);                                           /* STEP 4.5 */
        return JS_STEP_YIELD;

    case RX_LOCATE: {
        lxb_dom_node_t *ca = common_ancestor(f->sn, f->en), *c;         /* STEP 5 */
        int i;

        if (!node_is_inclusive_ancestor(f->sn, f->en))                  /* STEPS 6-7 */
            for (c = ca->first_child; c; c = c->next)
                if (bp_partially_contains(f->sn, f->en, c)) { f->first_pcc = c; break; }
        if (!node_is_inclusive_ancestor(f->en, f->sn))                  /* STEPS 8-9 */
            for (c = ca->first_child; c; c = c->next)
                if (bp_partially_contains(f->sn, f->en, c)) f->last_pcc = c;
        for (c = ca->first_child; c; c = c->next)                       /* STEP 10 */
            if (bp_contains(f->sn, f->so, f->en, f->eo, c)) rx_contained_add(ctx, f, c);
        for (i = 0; i < f->nc; i++)                                     /* STEP 11 */
            if (f->contained[i]->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
                return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                            "a doctype is contained in the range"), JS_STEP_ABRUPT;
        if (move && s->sp == 1) {
            /* STEPS 12-15, AND ONLY FOR THE PAGE'S OWN RANGE. A subrange is not a live range in this engine
               (see RxFrame) and nothing reads its position after this point, so moving it would be writing a
               value with no reader — the observable half of these steps is the receiver's. */
            RangeBounds *b = range_receiver(hdr->this_val);
            lxb_dom_node_t *nn;
            uint32_t no;
            range_collapse_point(f->sn, f->en, f->so, &nn, &no);
            {
                JSValue w = node_wrap(ctx, nn);
                bounds_set_start(ctx, b, w, no);
                bounds_set_end(ctx, b, w, no);
                JS_FreeValue(ctx, w);
            }
        }
        hdr->stage = RXS(RX_FIRST);
        return JS_STEP_YIELD;
    }

    case RX_FIRST:
        if (f->first_pcc) {                                        /* STEPS 16.1 and 17.1: `a clone of` it */
            node_clone_start(hdr, &s->nc, f->first_pcc, false, RXS(RX_CLONE_ROOT), RXS(RX_FIRST_CLONED));
            return JS_STEP_YIELD;
        }
        hdr->stage = RXS(RX_CONTAINED);
        return JS_STEP_YIELD;

    case RX_FIRST_CLONED:
        hdr->stage = RXS(RX_CONTAINED);
        if (node_is_chardata_kind(f->first_pcc)) {                            /* STEPS 16.2-16.4 */
            uint32_t len = node_length(f->sn);
            DCHECK(f->first_pcc == f->sn, "§5.5's note says a CharacterData first partially contained child IS "
                                          "the original start node, and this one is not");
            range_clone_data(s->nc.copy, f->sn, f->so, len);
            node_insert_at(f->frag, s->nc.copy, NULL);
            if (move) range_delete_data(ctx, f->sn, f->so, len - f->so);      /* extract's step 16.4 only */
            return JS_STEP_YIELD;
        }
        {                                                                     /* STEPS 17.2-17.3 */
            RxFrame *sub;
            node_insert_at(f->frag, s->nc.copy, NULL);
            sub = rx_push(ctx, s);
            f = &s->f[s->sp - 2];   /* rx_push may have moved the array */
            sub->sn = f->sn;
            sub->so = f->so;
            sub->en = f->first_pcc;
            sub->eo = node_length(f->first_pcc);
            sub->into = s->nc.copy;
            sub->after = (uint8_t)RXS(RX_CONTAINED);
            hdr->stage = RXS(RX_ENTER);
        }
        return JS_STEP_YIELD;

    case RX_CONTAINED:                                                        /* STEP 18 / STEP 14.1 */
        if (f->ci == f->nc) { hdr->stage = RXS(RX_LAST); return JS_STEP_YIELD; }
        {
            lxb_dom_node_t *c = f->contained[f->ci++];
            /* MOVING one is `append`, which pre-inserts and therefore REMOVES it from the tree first — and
               that removal runs the live-range pre-remove steps, which is how the range the caller still holds
               keeps up. */
            if (move) { node_insert_at(f->frag, c, NULL); return JS_STEP_YIELD; }
            /* CLONING THE CONTENTS instead says "a clone of contained child with subtree set to true", which is
               §4.4's algorithm and nothing this file may re-derive: a copy made here would carry neither the
               cloning steps HTML defines for the elements in that subtree nor their clonable shadow roots. */
            node_clone_start(hdr, &s->nc, c, true, RXS(RX_CLONE_ROOT), RXS(RX_CONTAINED_CLONED));
        }
        return JS_STEP_YIELD;

    case RX_CONTAINED_CLONED:                                                 /* STEP 14.2 */
        DCHECK(!move, "extract's step 18 APPENDS a contained child and clones nothing — an extraction resumed "
                      "at the clone-the-contents step means the two algorithms' one body took the wrong arm");
        node_insert_at(f->frag, s->nc.copy, NULL);
        hdr->stage = RXS(RX_CONTAINED);
        return JS_STEP_YIELD;

    case RX_LAST:
        if (f->last_pcc) {                                         /* STEPS 19.1 and 20.1: `a clone of` it */
            node_clone_start(hdr, &s->nc, f->last_pcc, false, RXS(RX_CLONE_ROOT), RXS(RX_LAST_CLONED));
            return JS_STEP_YIELD;
        }
        hdr->stage = RXS(RX_LEAVE);
        return JS_STEP_YIELD;

    case RX_LAST_CLONED:
        hdr->stage = RXS(RX_LEAVE);
        if (node_is_chardata_kind(f->last_pcc)) {                             /* STEPS 19.2-19.4 */
            DCHECK(f->last_pcc == f->en, "§5.5's note says a CharacterData last partially contained child IS "
                                         "the original end node, and this one is not");
            range_clone_data(s->nc.copy, f->en, 0, f->eo);
            node_insert_at(f->frag, s->nc.copy, NULL);
            if (move) range_delete_data(ctx, f->en, 0, f->eo);                /* extract's step 19.4 only */
            return JS_STEP_YIELD;
        }
        {                                                                     /* STEPS 20.2-20.3 */
            RxFrame *sub;
            node_insert_at(f->frag, s->nc.copy, NULL);
            sub = rx_push(ctx, s);
            f = &s->f[s->sp - 2];
            sub->sn = f->last_pcc;
            sub->so = 0;
            sub->en = f->en;
            sub->eo = f->eo;
            sub->into = s->nc.copy;
            sub->after = (uint8_t)RXS(RX_LEAVE);
            hdr->stage = RXS(RX_ENTER);
        }
        return JS_STEP_YIELD;

    default:
        DCHECK(hdr->stage == RXS(RX_LEAVE), "§5.5's extract resumed into a stage it does not have");
        if (s->sp == 1) {                                                     /* STEP 21 */
            *presult = node_wrap(ctx, f->frag);
            return JS_STEP_DONE;
        }
        /* "Append subfragment to clone" — the caller's line, run here because this is where the subfragment
           is finished. The fragment itself is left empty and unreferenced, which is what appending one does. */
        node_insert_at(f->into, f->frag, NULL);
        hdr->stage = f->after;
        js_free(ctx, f->contained);   /* the allocator rx_contained_add grew it with, and the one v->buf uses */
        f->contained = NULL;
        f->nc = f->ccap = 0;
        s->sp--;
        return JS_STEP_YIELD;
    }
#undef RXS
}

/* `extractContents` and `cloneContents` — one declaration, two magics, and the block starts where every
   declared member's own stages start. */
static int rx_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    return rx_run(ctx, hdr, st, idl_step_magic(hdr) == RX_EXTRACT, RX_ENTER, presult);
}

/* No release: the frame stack, each frame's contained-children list and the clone's own level stack are all
   rx_visit's, and the teardown discharges that one list. */
static const IdlStepDecl RANGE_EXTRACT = { rx_step, sizeof(RxState), rx_visit, NULL,
                                           "DOM §5.5 extract a live range / clone the contents of a live range",
                                           RX_STEPS };

/* ---- deleteContents ------------------------------------------------------------------------------------- */

/* IT IS ITS OWN ALGORITHM AND NOT `extract` WITH THE FRAGMENT DROPPED. The standard states it over a flat list
   of contained nodes "omitting any node whose parent is also contained", which has no recursion in it at all —
   the spine extract descends is exactly what that omission rule replaces. Writing it as extract-and-discard
   would build a fragment the page never sees and clone nodes nobody reads. */
#define RD_STAGES(X) \
    X(RD_ENTER,   "DOM §5.5 deleteContents steps 1-3 (the collapsed range and the both-ends-in-one-" \
                  "CharacterData-node case)") \
    X(RD_COLLECT, "DOM §5.5 deleteContents step 4 (the nodes contained in the range whose parent is not, one " \
                  "node per step)") \
    X(RD_PLACE,   "DOM §5.5 deleteContents steps 5-9 (where the range lands, and the start node's tail)") \
    X(RD_REMOVE,  "DOM §5.5 deleteContents step 10 (remove each collected node, one per step)") \
    X(RD_TAIL,    "DOM §5.5 deleteContents step 11 (the end node's head)")
enum { IDL_STEP_STAGE_BASE(RD_STAGES) RD_STAGES(JS_STEP_STAGE_ENUM) };
const char *const RANGE_DEL_STEPS[] = { RD_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct RangeDelState RdState;

void range_del_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    RdState *s = st;
    v->buf(ctx, (void **)&s->list, sizeof(lxb_dom_node_t *) * (size_t)s->cap);
}

int range_del_step(JSContext *ctx, JSStepHdr *hdr, RangeDelState *s, RangeBounds *b, JSValue *presult)
{
    DCHECK(b != NULL, "§5.5's deleteContents stepped with no bounds — the caller resolves the subject before "
                      "every step and an unresolvable one is its own abrupt, not this machine's");

    switch (hdr->stage) {
    case RD_ENTER:
        s->sn = bounds_start(b);
        s->so = b->start_off;
        s->en = bounds_end(b);
        s->eo = b->end_off;
        DCHECK(s->sn != NULL && s->en != NULL, "a live range's boundary point is not a node");
        if (s->sn == s->en && s->so == s->eo) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }   /* STEP 1 */
        if (s->sn == s->en && node_is_chardata_kind(s->sn)) {                                     /* STEP 3 */
            range_delete_data(ctx, s->sn, s->so, s->eo - s->so);
            *presult = JS_UNDEFINED;
            return JS_STEP_DONE;
        }
        /* STEP 4's walk is bounded by the common ancestor: every contained node is a descendant of it. */
        s->root = common_ancestor(s->sn, s->en);
        s->cursor = s->root;
        hdr->stage = RD_COLLECT;
        return JS_STEP_YIELD;

    case RD_COLLECT:                                                                              /* STEP 4 */
        if (!s->cursor) { hdr->stage = RD_PLACE; return JS_STEP_YIELD; }
        if (bp_contains(s->sn, s->so, s->en, s->eo, s->cursor) &&
            !(s->cursor->parent && bp_contains(s->sn, s->so, s->en, s->eo, s->cursor->parent))) {
            if (s->n == s->cap) {
                int want = s->cap ? s->cap * 2 : 8;
                lxb_dom_node_t **a = js_realloc(ctx, s->list, sizeof(*a) * (size_t)want);
                CHECK(a != NULL, "§5.5's deleteContents could not grow its removal list — a dropped entry is a "
                                 "node the page asked to delete and still has");
                s->list = a;
                s->cap = want;
            }
            s->list[s->n++] = s->cursor;
            /* A CONTAINED node's descendants are contained and their parent is too, so step 4 omits every one
               of them: skip the subtree rather than walking it to discard it. */
            while (s->cursor->last_child) s->cursor = s->cursor->last_child;
        }
        s->cursor = node_next_in(s->cursor, s->root);
        return JS_STEP_YIELD;

    case RD_PLACE: {
        lxb_dom_node_t *nn;
        uint32_t no;
        range_collapse_point(s->sn, s->en, s->so, &nn, &no);                                 /* STEPS 5-7 */
        {
            JSValue w = node_wrap(ctx, nn);
            bounds_set_start(ctx, b, w, no);                                                 /* STEP 8 */
            bounds_set_end(ctx, b, w, no);
            JS_FreeValue(ctx, w);
        }
        if (node_is_chardata_kind(s->sn))                                                    /* STEP 9 */
            range_delete_data(ctx, s->sn, s->so, node_length(s->sn) - s->so);
        hdr->stage = RD_REMOVE;
        return JS_STEP_YIELD;
    }

    case RD_REMOVE:                                                                          /* STEP 10 */
        if (s->i == s->n) { hdr->stage = RD_TAIL; return JS_STEP_YIELD; }
        dom_cow_remove_child(s->list[s->i++]);
        return JS_STEP_YIELD;

    default:
        DCHECK(hdr->stage == RD_TAIL, "§5.5's deleteContents resumed into a stage it does not have");
        if (node_is_chardata_kind(s->en))                                                    /* STEP 11 */
            range_delete_data(ctx, s->en, 0, s->eo);
        *presult = JS_UNDEFINED;
        return JS_STEP_DONE;
    }
}

/* §5.5's OWN member: the subject is the receiver. */
static int rd_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    RangeBounds *b;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    b = range_receiver(hdr->this_val);
    return range_del_step(ctx, hdr, st, b, presult);
}

/* No release: the removal list is range_del_visit's, discharged with the rest. */
static const IdlStepDecl RANGE_DELETE = { rd_step, sizeof(RdState), range_del_visit, NULL,
                                          "DOM §5.5 Range.deleteContents()", RANGE_DEL_STEPS };

/* ---- insertNode ----------------------------------------------------------------------------------------- */

/* §5.5 "insert a node into a live range". No page code and no walk of the page's tree — a fixed number of
   pointer moves — so it is an ordinary declared member rather than a machine. `node` arrives brand-checked. */
static int range_insert_node(JSContext *ctx, RangeBounds *b, lxb_dom_node_t *node)
{
    lxb_dom_node_t *sn = bounds_start(b), *ref, *parent;
    uint32_t new_off;

    DCHECK(sn != NULL, "a live range's start is not a node");
    if (sn->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION ||                               /* STEP 1 */
        sn->type == LXB_DOM_NODE_TYPE_COMMENT ||
        (sn->type == LXB_DOM_NODE_TYPE_TEXT && !sn->parent) || sn == node)
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "a node cannot be inserted at this range's start"), -1;
    ref = NULL;                                                                               /* STEP 2 */
    if (sn->type == LXB_DOM_NODE_TYPE_TEXT) {                                                 /* STEP 3 */
        ref = sn;
    } else {                                                                                  /* STEP 4 */
        uint32_t k = 0;
        for (ref = sn->first_child; ref && k < b->start_off; ref = ref->next) k++;
    }
    parent = ref ? ref->parent : sn;                                                           /* STEP 5 */
    /* STEP 6 names §4.2.3's "ensure pre-insert validity" with an empty childrenToExclude, and it is the ONE
       entry in node.h — not a second transcription here. The copy that stood in this file had drifted from
       the algorithm in three places (a plain ancestor test where §4.2.2 says host-including, and steps 6
       and 8 blind to CDATASection, which §4.12 Interface CDATASection declares `: Text`), which is what two
       copies do. */
    if (!node_ensure_pre_insert_valid(ctx, node, parent, ref, NULL)) return -1;                /* STEP 6 */
    if (sn->type == LXB_DOM_NODE_TYPE_TEXT) {                                                  /* STEP 7 */
        ref = node_split_text(ctx, sn, b->start_off);
        if (!ref) return -1;
    }
    if (node == ref) ref = ref->next;                                                          /* STEP 8 */
    if (node->parent) dom_cow_remove_child(node);                                              /* STEP 9 */
    new_off = ref ? node_index(ref) : node_length(parent);                                     /* STEP 10 */
    new_off += node_is_document_fragment(node) ? node_length(node) : 1u;                       /* STEP 11 */
    node_insert_at(parent, node, ref);                                                         /* STEP 12 */
    if (node_of(b->start_node) == node_of(b->end_node) && b->start_off == b->end_off) {        /* STEP 13 */
        JSValue w = node_wrap(ctx, parent);
        bounds_set_end(ctx, b, w, new_off);
        JS_FreeValue(ctx, w);
    }
    return 0;
}

/* ---- surroundContents ----------------------------------------------------------------------------------- */

/* IT IS A MACHINE BECAUSE ITS STEP 3 IS `extract`, which is one. The two checks before it are O(depth) walks
   and the three steps after it are pointer moves; what makes this suspend is the extraction it PERFORMS —
   the same body, over the stage block RS_STAGES declares in THIS member's own numbering, so every resume
   point is a step this declaration names rather than a private byte nothing can check. */
static const char *const RS_STEPS[] = { RS_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* The extraction's own state, EMBEDDED: step 3 is `extract this`, so this machine performs that algorithm
   rather than calling a second implementation of it. */
typedef struct RsState {
    RxState rx;
    lxb_dom_node_t *frag;
} RsState;

static void rs_visit(JSContext *ctx, void *st, JSStepVisit *v) { rx_visit(ctx, &((RsState *)st)->rx, v); }

static int rs_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    RsState *s = st;
    RangeBounds *b = range_receiver(hdr->this_val);
    lxb_dom_node_t *np = argc > 0 ? node_of(argv[0]) : NULL;

    (void)out_cb; (void)out_argc;
    DCHECK(np != NULL, "surroundContents reached the body with something that is not a node");

    if (hdr->stage == RS_CHECK) {
        lxb_dom_node_t *sn = bounds_start(b), *en = bounds_end(b), *n, *ca;

        JS_FreeValue(ctx, cb_result);
        /* STEP 1. The partially contained nodes are exactly the inclusive ancestors of one boundary node up to
           (but not including) the common ancestor — walking those two chains is O(depth), where walking the
           whole subtree asking "is this partially contained" is O(document). */
        ca = common_ancestor(sn, en);
        for (n = sn; n && n != ca; n = n->parent)
            if (n->type != LXB_DOM_NODE_TYPE_TEXT && bp_partially_contains(sn, en, n))
                return JS_ThrowDOMException(ctx, "InvalidStateError",
                                            "a non-Text node is partially contained in the range"),
                       JS_STEP_ABRUPT;
        for (n = en; n && n != ca; n = n->parent)
            if (n->type != LXB_DOM_NODE_TYPE_TEXT && bp_partially_contains(sn, en, n))
                return JS_ThrowDOMException(ctx, "InvalidStateError",
                                            "a non-Text node is partially contained in the range"),
                       JS_STEP_ABRUPT;
        if (np->type == LXB_DOM_NODE_TYPE_DOCUMENT || np->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE ||
            node_is_document_fragment(np))                                                    /* STEP 2 */
            return JS_ThrowDOMException(ctx, "InvalidNodeTypeError",
                                        "newParent cannot be a Document, DocumentType or DocumentFragment"),
                   JS_STEP_ABRUPT;
        hdr->stage = RS_X_ENTER;
        return JS_STEP_YIELD;
    }

    JS_FreeValue(ctx, cb_result);
    /* STEP 3, and the block it spans is every stage the extraction declared here — including the six §4.4's
       `clone a node` rests at, which are the LAST of them because the algorithm is entered from four of the
       extraction's own steps and hands control back to each. */
    if (hdr->stage >= RS_X_ENTER && hdr->stage <= RS_X_CLONE_LEAVE) {
        /* THE EXTRACTION, IN THIS MEMBER'S OWN STAGE BLOCK. `extract` always MOVES, which is why the `move`
           operand is 1 here and is not read off a magic: surroundContents has no cloning form. */
        JSValue sub = JS_UNDEFINED;
        int r = rx_run(ctx, hdr, &s->rx, 1, RS_X_ENTER, &sub);
        if (r != JS_STEP_DONE) return r;
        s->frag = node_of(sub);
        DCHECK(s->frag != NULL, "§5.5's extraction produced something that is not a fragment");
        JS_FreeValue(ctx, sub);
        hdr->stage = RS_WRAP;
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == RS_WRAP, "§5.5's surroundContents resumed into a stage it does not have");
    while (np->first_child) dom_cow_remove_child(np->first_child);                             /* STEP 4 */
    if (range_insert_node(ctx, b, np) < 0) return JS_STEP_ABRUPT;                              /* STEP 5 */
    node_insert_at(np, s->frag, NULL);                                                         /* STEP 6 */
    {                                                                                          /* STEP 7 */
        JSValue pw;
        uint32_t i = node_index(np);
        DCHECK(np->parent != NULL, "§5.5 step 5 has just inserted newParent, so it has a parent");
        pw = node_wrap(ctx, np->parent);
        bounds_set_start(ctx, b, pw, i);
        bounds_set_end(ctx, b, pw, i + 1);
        JS_FreeValue(ctx, pw);
    }
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl RANGE_SURROUND = { rs_step, sizeof(RsState), rs_visit, NULL,
                                            "DOM §5.5 Range.surroundContents(newParent)", RS_STEPS };

/* ---- HTML §8.5.7 "The createContextualFragment() method" -------------------------------------------------
 *
 * `partial interface Range { [CEReactions, NewObject] DocumentFragment
 *  createContextualFragment((TrustedHTML or DOMString) string); };` — HTML §8.5.7, NOT "DOM Parsing and
 * Serialization". That specification has been merged into HTML and survives only as a stub with an issue
 * tracker, which §8.5.7's own first paragraph is the link to; a reader holding it is holding a document that
 * no longer defines this member.
 *
 * WHAT MAKES IT "CONTEXTUAL" is steps 2-6, and they are the whole of what this file adds to the shared §13.4
 * machine: the markup is parsed in the tree-building context of the RANGE'S START NODE, so `<tr>` inside a
 * `<table>` context survives and the same string parsed bare does not. Dropping the context — parsing against
 * `body` always, or against the document element — is the one implementation mistake that leaves every other
 * assertion in the WPT file passing, so the context is resolved here and handed to fragment_parse_begin as
 * `context`, which is the same argument §8.5.4's innerHTML setter hands it.
 *
 * ITS SCRIPTS RUN, AND THAT IS THE MEMBER'S DEFINING PROPERTY rather than a detail. §13.2.4.5 "Other parsing
 * state flags" names this member BY NAME as the user of the FRAGMENT parser scripting mode — "Scripts are
 * executed as soon as they are inserted into the document as part of a the HTML fragment parsing algorithm,
 * ignoring async and defer attributes. This mode is used by createContextualFragment()." — and step 7 passes
 * `Fragment` outright. So a `<script>` in the markup is NOT marked already started, does not run while the
 * fragment is detached (§4.12.1 step 7, "If el is not connected, then return"), and runs the moment the page
 * appends the fragment to a document. That is a SOLVER-VISIBLE difference and not only a fidelity one: a
 * bundle that builds DOM this way ships code whose execution the engine would otherwise never reach.
 *
 * THE SINK IS REAL AND IS REPORTED AS ONE. §8.5.7's own note is "This method performs no sanitization to
 * remove potentially-dangerous elements and attributes like script or event handler content attributes", so
 * this joins innerHTML and insertAdjacentHTML on solve_html_sink — and it is the STRONGEST of the three,
 * because the breakout does not need an `onerror` to fire: a `<script>` in the markup executes on insertion.
 *
 * IT PLACES INTO A DocumentFragment INSTEAD OF A TREE, which is the only structural difference from the five
 * members that came before it, and it needs no new placement kind: §13.4 step 15 creates the fragment the
 * algorithm returns, and FRAG_INTO_CHILDREN with that fragment as the anchor reaches the identical loop.
 * Nothing in it is connected, so §4.2.3's insertion steps reach no `<script>` preparation — which is exactly
 * what "not yet added to document, should not have run" means. */
static int js_range_contextual_fragment(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FragmentParse *s = st;
    int r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;

    if (hdr->stage == FRAG_TRUSTED) {
        /* STEP 1 — "Let compliantString be the result of invoking the get trusted type compliant string
           algorithm with TrustedHTML, this's relevant global object, string, "Range createContextualFragment",
           and "script"." The sink NAME is the standard's own and is what a Trusted Types violation report
           carries, which is what keeps this member distinguishable from `Element innerHTML` in one report. */
        s->compliant = trusted_types_compliant_string(ctx, TRUSTED_TYPE_HTML, argc > 0 ? argv[0] : JS_UNDEFINED,
                                                      "Range createContextualFragment");
        if (JS_IsException(s->compliant)) { s->compliant = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        hdr->stage = FRAG_START;
    }
    if (hdr->stage == FRAG_START) {
        RangeBounds *b = range_receiver(hdr->this_val);
        lxb_dom_element_t *el = NULL;
        lxb_dom_document_fragment_t *out;
        lxb_dom_document_t *doc;
        lxb_dom_node_t *node;
        const char *html;

        node = bounds_start(b);                                         /* STEP 2 */
        DCHECK(node != NULL,
               "§8.5.7 step 2 read a Range's start node and found none — §5.5 gives every live range two "
               "boundary points at construction and every setter replaces rather than clears one, so a null "
               "here is a range this engine built and DOM §5.5 cannot describe");
        doc = node->owner_document;
        /* STEPS 3, 4 AND 5. `element` starts null; an Element start node IS the context, and a Text or a
           Comment contributes its PARENT ELEMENT — DOM §4.4's "parent element" is the parent if it is an
           element and null otherwise, which is why a Comment sitting in a DocumentFragment falls through to
           step 6 rather than taking the fragment as a context.
           CDATASection IS a Text — DOM §4.11 declares `interface CDATASection : Text` — so step 4's "node
           implements Text" is true of one, and a node-type test that listed only LXB_DOM_NODE_TYPE_TEXT would
           answer a narrower question than the step asks. */
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
            el = lxb_dom_interface_element(node);
        else if (node->type == LXB_DOM_NODE_TYPE_TEXT || node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
                 node->type == LXB_DOM_NODE_TYPE_COMMENT)
            el = (node->parent != NULL && node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT)
               ? lxb_dom_interface_element(node->parent) : NULL;
        /* STEP 6 — "If element is null or all of the following are true: element's node document is an HTML
           document; element's local name is "html"; and element's namespace is the HTML namespace, then set
           element to the result of creating an element given this's node document, "body", and the HTML
           namespace."
           ALL THREE CONJUNCTS, and the namespace one is not a formality: `createElementNS("http://fake-ns",
           "html")` has the local name and not the namespace, so it stays its own context and the markup parses
           against it exactly as it would against a `<div>`. DOM §4.5's "A document is said to be an XML
           document if its type is `xml`; otherwise an HTML document" is what the first conjunct asks, which is
           why it is the NEGATION of document_is_xml_of and not a content-type string.
           WHY THE STANDARD DOES THIS AT ALL: parsing against `<html>` would put the markup through
           §13.2.6.4.3 'The "before head" insertion mode' and materialise a `<head>` and a `<body>` around it,
           which is the compat bug the WPT file cites (Mozilla 585819) — so `<span>Hello</span>` selected over
           `document.documentElement` must come back as one `<span>` and not as a document skeleton.
           The `body` is in NO TREE and nothing else will ever free it, so the machine owns it: it goes on
           `own_context`, which fragment_parse_release destroys on the completed path and the throw path
           alike — the same field, and the same reason, as §8.5.5 step 5's. */
        if (el == NULL ||
            (!document_is_xml_of(lxb_dom_interface_node(el)->owner_document) &&
             lxb_dom_interface_node(el)->ns == LXB_NS_HTML &&
             lxb_html_tree_node_is(lxb_dom_interface_node(el), LXB_TAG_HTML))) {
            /* "…AND THE HTML NAMESPACE" is step 6's own last clause, so it is NAMED here. It was being taken
               from `lxb_dom_document_t::type`, which nothing in this engine writes — see document.h's
               document_create_element_html — and a Range whose start node is in an XML document therefore got
               a `body` in NO namespace, which §13.4 step 2 reads to pick the tokenizer state. */
            s->own_context = document_create_element_html(doc, "body", 4);
            el = s->own_context;
        }
        DCHECK(lxb_dom_interface_node(el)->owner_document == doc,
               "§8.5.7's context element belongs to a document other than the range's — steps 4 and 5 take the "
               "start node or its parent and step 6 creates in this's node document, so all three are one "
               "document, and §13.4 step 15 creates the returned fragment in it");
        /* HTML §13.4 "Parsing HTML fragments" STEP 15 — "Let fragment be the result of creating a document
           fragment given target's node document" — hoisted ahead of the parse because it is what this member
           RETURNS and because the placement needs it as its anchor. THIS FLOW MADE IT, so the delta owns it
           and destroys it if the flow is discarded: a fragment comes out of the DOCUMENT's lexbor arena and is
           reachable from no tree, so without the creation entry every one ever built would sit in that arena
           with no owner and invisible to the runtime's gc_obj_list walk, which only sees GC objects. */
        out = lxb_dom_document_fragment_interface_create(doc);
        CHECK(out != NULL, "§13.4 step 15's DocumentFragment could not be created — handing back a null the "
                           "page cannot tell from a fragment it never asked for is not an option");
        dom_cow_note_created(lxb_dom_interface_node(out));
        /* THE SINK, BEFORE THE PARSE and whatever the value turns out to be — see the header above. */
        solve_html_sink(ctx, s->compliant);
        if (concolic_is(s->compliant)) {
            /* A concolic value has no bytes to parse and the sink report IS what this call means. The member
               still RETURNS a DocumentFragment, because its IDL return type is one and a page that reads
               `.childNodes` off it must find an empty list rather than a TypeError on undefined. */
            *presult = node_wrap(ctx, lxb_dom_interface_node(out));
            return JS_STEP_DONE;
        }
        DCHECK(JS_IsString(s->compliant),
               "§8.5.7 reached its body with an unconverted argument — the IDL declaration is what converts "
               "it, and running the page's toString from here is the drive-to-completion the flow machinery "
               "exists to avoid");
        html = JS_ToCString(ctx, s->compliant);
        if (!html) return JS_STEP_ABRUPT;
        /* STEP 7 — "Return the result of invoking the fragment parsing algorithm steps with element,
           compliantString, and Fragment." THREE arguments, and the third is the one this member exists to
           pass. `allowDeclarativeShadowRoots` is not among them, so HTML §8.5.4's fragment parsing algorithm
           steps hand §13.4 the literal `false`: a `<template shadowrootmode>` in this markup stays a template,
           exactly as it does through innerHTML. */
        fragment_parse_begin(ctx, s, el, lxb_dom_interface_node(out), FRAG_INTO_CHILDREN, html,
                             /*clear_first*/ false, /*allow_declarative*/ false, FRAG_SCRIPTING_FRAGMENT);
        JS_FreeCString(ctx, html);
        hdr->stage = FRAG_FEED;
        return JS_STEP_YIELD;
    }
    r = fragment_parse_step(ctx, hdr, s);
    /* STEP 7's RETURN, taken at the machine's terminal: `anchor` is §13.4 step 15's fragment and the placement
       has just emptied §13.4 step 12's `root` into it. It is read off the state rather than remembered in a
       local because every stage between here and FRAG_START is a rest point a park can happen at. */
    if (r == 0) *presult = node_wrap(ctx, s->anchor);
    return r;
}

static const char *const RANGE_CONTEXTUAL_FRAGMENT_STEPS[] = { FRAG_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* FOUR STAGES: this member never replaces a target's children, so FRAG_CLEAR is past the end of what it
   declares and the driver says so if the shared machine ever reaches it from here. It never filters either,
   so §8.6.4's sanitizer stages are past the end as well. */
static const IdlStepDecl RANGE_CONTEXTUAL_FRAGMENT = {
    js_range_contextual_fragment, sizeof(FragmentParse), fragment_parse_visit, fragment_parse_release,
    "HTML §8.5.7 Range.createContextualFragment(string)", RANGE_CONTEXTUAL_FRAGMENT_STEPS,
    .unforkable = fragment_parse_unforkable
};

/* ---- CSSOM VIEW §9 "Extensions to the Range Interface" ---------------------------------------------------
 * `partial interface Range { DOMRectList getClientRects(); [NewObject] DOMRect getBoundingClientRect(); }`.
 *
 * IT IS A SELECTION ALGORITHM OVER §6's, NOT A SECOND GEOMETRY. §9 states its list as two inclusion rules and
 * computes no rectangle of its own for an element: "for each element selected by the range, whose parent is
 * NOT selected by the range, include the border areas returned by invoking getClientRects() on the element."
 * That invocation is §2's INTERNAL algorithm (core/dom/element_view.h), so a page that overwrites
 * `Element.prototype.getClientRects` cannot change what a Range measures — and every box this engine cannot
 * place crashes THERE, in the one component that owns a border area, rather than a second time here.
 *
 * THE PARENT CLAUSE IS WHAT MAKES THE LIST A COVER RATHER THAN A PILE: an element whose parent is also
 * selected is already inside its parent's border area, so including it again would double-count it. "Selected
 * by the range" is DOM §5.5's CONTAINED IN, which `range_contains` above already decides.
 *
 * THE TEXT RULE IS THE ONE THIS ENGINE CANNOT RUN, and §9 says exactly why in its own words: the bounds "are
 * computed using FONT METRICS; thus, for horizontal writing, the vertical dimension of each box is determined
 * by the font ascent and descent, and the horizontal dimension by the text advance width", over whole
 * TYPOGRAPHIC CHARACTER UNITS. That is the same capability CSS 2 §9.4.2's line boxes need, so it is named as
 * one thing in both places rather than as two gaps.
 *
 * THE COORDINATE SPACE IS THE ELEMENT MEMBER'S, because the rectangles ARE the element member's. */

/* §9's "the range is not in the document" — DOM §5.5 makes a live range's root "the root of its start node",
   so a range whose nodes hang off a DocumentFragment or a detached subtree has a root that is not a Document
   and every user agent answers it with an empty list. The per-ELEMENT questions (is it being rendered, does it
   generate a box) are §6 step 1's and are asked there, once per element. */
static bool range_in_document(const RangeBounds *b)
{
    return range_root(b)->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* §9's TEXT rule's own condition — "for each Text node SELECTED OR PARTIALLY SELECTED by the range (INCLUDING
   WHEN THE BOUNDARY-POINTS ARE IDENTICAL)". The parenthesis is a third case and not a restatement: a collapsed
   range inside a Text node contains it under neither of DOM §5.5's two predicates (a node is not contained in a
   range whose boundary points are both inside it, and it is not partially contained either, since it is an
   inclusive ancestor of BOTH boundary nodes), and §9 still says a rectangle is included for it — which is what
   makes a collapsed range's `getBoundingClientRect` a caret position in every user agent. */
static bool range_selects_text(const RangeBounds *b, lxb_dom_node_t *n)
{
    lxb_dom_node_t *sn = bounds_start(b), *en = bounds_end(b);

    if (!node_is_text(n)) return false;
    return n == sn || n == en || range_contains(b, n) || bp_partially_contains(sn, en, n);
}

/* §9's getClientRects(), in the spec's own order — ONE tree walk in CONTENT ORDER, which is what the standard
   asks the merged list to be in ("a list of DOMRect objects IN CONTENT ORDER that matches the following
   constraints"). Two separate passes would produce the two constraints' lists concatenated and not
   interleaved, which is a different list whenever a range covers both. */
static JSValue range_client_rects(JSContext *ctx, const RangeBounds *b)
{
    lxb_dom_node_t *root, *n;
    JSValue out = JS_NewArray(ctx);
    uint32_t k = 0;

    CHECK(!JS_IsException(out), "the range client-rect list could not be allocated");
    if (!range_in_document(b)) return dom_rect_list_new(ctx, out);
    /* THE WALK IS OVER THE COMMON ANCESTOR AND NOT THE DOCUMENT, and that is a statement about the range and
       not a saving: DOM §5.5's "contained in" places both of a contained node's boundary points inside the
       range, so no node outside `commonAncestorContainer` can be contained or partially contained, and a walk
       from the document root would visit them only to answer no. Starting here is the same list. */
    root = common_ancestor(bounds_start(b), bounds_end(b));
    for (n = root; n != NULL; n = node_next_in(n, root)) {
        if (range_selects_text(b, n))
            DFAIL("CSSOM VIEW §9's getClientRects() includes a rectangle for every TEXT NODE the range selects "
                  "or partially selects, and states what computes it: 'the bounds of these DOMRect objects are "
                  "computed using FONT METRICS; thus, for horizontal writing, the vertical dimension of each "
                  "box is determined by the font ascent and descent, and the horizontal dimension by the text "
                  "advance width', with a partially covered TYPOGRAPHIC CHARACTER UNIT (half a surrogate pair, "
                  "part of a grapheme cluster) rounded out to the whole unit. THE VERTICAL DIMENSION IS NO "
                  "LONGER THE GAP: the ascent and descent this rule names are CSS 2 §10.8.1 'Leading and "
                  "half-leading''s `A` and `D`, and core/css/font_metrics.h holds both for the first available "
                  "font. WHAT IS ABSENT IS THE ADVANCE WIDTH — font_metrics.h measures exactly two glyphs "
                  "(css-values-4 §6.1.1's assumed '0' for `ch` and '水' for `ic`) and no run of text — which "
                  "is the SAME missing capability CSS 2 §9.4.2's line boxes need for an inline box's fragments "
                  "and §10.6.3's line-box arm needs for a content-based height. BUILD the per-glyph advance "
                  "beside `A` and `D`, and css-text-3's typographic character unit segmentation; this rule and "
                  "those two are then one component's three consumers");
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && range_contains(b, n) &&
            !(n->parent != NULL && range_contains(b, n->parent))) {
            JSValue list = element_view_client_rects(lxb_dom_interface_element(n)), len;
            uint32_t i, m = 0;

            DCHECK(JS_IsObject(list), "§9 invoked §6's getClientRects() on a selected element and got no list "
                                      "back — that algorithm answers a DOMRectList on every path it has");
            len = JS_GetPropertyStr(ctx, list, "length");
            JS_ToUint32(ctx, &m, len);
            JS_FreeValue(ctx, len);
            for (i = 0; i < m; i++)
                JS_SetPropertyUint32(ctx, out, k++, JS_GetPropertyUint32(ctx, list, i));
            JS_FreeValue(ctx, list);
        }
    }
    return dom_rect_list_new(ctx, out);
}

/* §9's getBoundingClientRect(), which is its OWN four steps and not §6's get-the-bounding-box — the standard
   writes them out again for the Range, so they are written out again here. Steps 3 and 4 reduce a list of one
   to that one rectangle under either, for the reason ev_bounding_rect states in element_view.c, and the choice
   between them for a longer list is the same unwritten comparison over a possibly-concolic number. */
static JSValue range_bounding_rect(JSContext *ctx, const RangeBounds *b)
{
    JSValue list = range_client_rects(ctx, b), len;
    uint32_t n = 0;

    len = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    /* step 2 */
    if (n == 0) {
        JS_FreeValue(ctx, list);
        return dom_rect_new(ctx, 0.0, 0.0, 0.0, 0.0);
    }
    /* steps 3 and 4, which agree for a one-rectangle list */
    if (n == 1) {
        JSValue only = JS_GetPropertyUint32(ctx, list, 0);

        DCHECK(dom_rect_is(only), "§9's getBoundingClientRect read a list whose one member is not a DOMRect");
        JS_FreeValue(ctx, list);
        return only;
    }
    JS_FreeValue(ctx, list);
    DFAIL("CSSOM VIEW §9's getBoundingClientRect() steps 3 and 4 must CHOOSE for a list of more than one "
          "rectangle — the first one when every rectangle has a zero width or height, the smallest enclosing "
          "rectangle otherwise — and both are COMPARISONS over numbers a border area derived from the initial "
          "containing block carries a viewport domain on. core/dom/element_view.c's own reduction names the "
          "primitive to write them over (Geometry Interfaces §3's NaN-safe edges, which already state the rule "
          "for an unknown operand: yield the concolic, run the derivation on the examples, never fork). WRITE "
          "BOTH there and reach them from here, so the two members share one reduction rather than two");
    return dom_rect_new(ctx, 0.0, 0.0, 0.0, 0.0);
}

enum { R_CLIENT_RECTS = 0, R_BOUNDING_RECT };

static JSValue js_range_rects(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    RangeBounds *b = range_receiver(this_val);

    (void)argc; (void)argv;
    if (magic == R_CLIENT_RECTS) return range_client_rects(ctx, b);
    DCHECK(magic == R_BOUNDING_RECT, "a CSSOM VIEW §9 Range member was declared with a magic neither of the two "
                                     "members the partial interface declares carries");
    return range_bounding_rect(ctx, b);
}

/* ---- DECLARATION AND INSTALL ---------------------------------------------------------------------------- */

/* DOM §5.5 Interface Range's four comparison constants, on both the interface object and the prototype —
   which is where Web IDL §3.7.5 Constants puts an interface's constants, and the two targets are why one table
   is installed twice below. Web IDL §3.7.4 Named properties object is the number that stood here, and it
   resolves, so nothing reported it: a correct number under no title is the one shape an audit cannot see.
   The descriptor is IDL_CONSTANT_PROP_FLAGS, stated once in idl_args.h with the sentence it derives from. */
static const JSCFunctionListEntry js_range_consts[] = {
    JS_PROP_INT32_DEF("START_TO_START", 0, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("START_TO_END",   1, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("END_TO_END",     2, IDL_CONSTANT_PROP_FLAGS),
    JS_PROP_INT32_DEF("END_TO_START",   3, IDL_CONSTANT_PROP_FLAGS),
};

static int g_id[R_MEMBER_N], g_id_to_string = -1, g_id_delete = -1, g_id_extract = -1,
           g_id_clone_contents = -1, g_id_surround = -1, g_id_client_rects = -1, g_id_bounding_rect = -1,
           g_id_contextual_fragment = -1;

void range_init(JSContext *ctx)
{
    JSClassDef d = { "Range", range_finalizer, range_gc_mark };
    static const IdlArgType NODE_OFFSET[2] = { IDL_INTERFACE, IDL_UNSIGNED_LONG };
    static const IdlArgType ONE_NODE[1] = { IDL_INTERFACE };
    static const IdlArgType ONE_BOOL[1] = { IDL_BOOLEAN };
    static const IdlArgType HOW_RANGE[2] = { IDL_UNSIGNED_SHORT, IDL_INTERFACE };
    static const IdlArgType ONE_HTML[1] = { IDL_DOMSTRING };
    int k;

    if (g_range_class) return;   /* one AGENT, one class and one set of pool entries */
    /* THE LIVE LIST'S OWN PRE-INIT STATE, ASSERTED HERE BECAUSE HERE IS WHERE IT IS TRUE. These four are
       agent state that core/agent_state.h cannot hold: the array LEGITIMATELY outlives the release column (the
       objects whose finalizers unregister are still alive when a component's `_free` runs, so the last
       finalizer frees it), and `g_live_closed` is legitimately 1 there. A slot in that position is asserted at
       the next `_init` instead of being declared — which is exactly what would have caught the release that
       set the flag and never put it back. */
    DCHECK(g_live == NULL && g_live_n == 0 && g_live_cap == 0 && !g_live_closed,
           "§5.5's live-range list is not where a fresh process would have left it — a previous agent in this "
           "process released this component and its last Range's finalizer never wound the list back, so this "
           "agent opens with a list that is already closed and an array it does not own");
    abstract_range_init(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_range_class);
    JS_NewClass(JS_GetRuntime(ctx), g_range_class, &d);
    abstract_range_claim_class(g_range_class);

    for (k = 0; k < R_MEMBER_N; k++) g_id[k] = -1;
    g_id[R_SET_START] = idl_method_id(ctx, NODE_OFFSET, 2, js_range_member, R_SET_START);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    g_id[R_SET_END] = idl_method_id(ctx, NODE_OFFSET, 2, js_range_member, R_SET_END);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    for (k = R_SET_START_BEFORE; k <= R_SET_END_AFTER; k++) {
        g_id[k] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, k);
        idl_this_iface(range_is, "Range");
        idl_iface_brand(node_class_id());
    }
    g_id[R_COLLAPSE] = idl_method_id(ctx, ONE_BOOL, 1, js_range_member, R_COLLAPSE);
    idl_this_iface(range_is, "Range");
    idl_optional_from(0);
    g_id[R_SELECT_NODE] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, R_SELECT_NODE);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    g_id[R_SELECT_CONTENTS] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, R_SELECT_CONTENTS);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    g_id[R_CLONE] = idl_method_id(ctx, NULL, 0, js_range_member, R_CLONE);
    idl_this_iface(range_is, "Range");
    g_id[R_DETACH] = idl_method_id(ctx, NULL, 0, js_range_member, R_DETACH);
    idl_this_iface(range_is, "Range");
    /* `short compareBoundaryPoints(unsigned short how, Range sourceRange)` — the interface arm brands against
       THIS class, so a StaticRange or a plain object is a TypeError before step 1. */
    g_id[R_COMPARE_BP] = idl_method_id(ctx, HOW_RANGE, 2, js_range_member, R_COMPARE_BP);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(g_range_class);
    g_id[R_IS_POINT_IN] = idl_method_id(ctx, NODE_OFFSET, 2, js_range_member, R_IS_POINT_IN);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    g_id[R_COMPARE_POINT] = idl_method_id(ctx, NODE_OFFSET, 2, js_range_member, R_COMPARE_POINT);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    g_id[R_INTERSECTS] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, R_INTERSECTS);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    g_id[R_INSERT_NODE] = idl_method_id(ctx, ONE_NODE, 1, js_range_member, R_INSERT_NODE);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    g_id_to_string = idl_method_id_step(ctx, NULL, 0, NULL, 0, &RANGE_TO_STRING, 0);
    idl_this_iface(range_is, "Range");
    /* §5.5's five content-moving members. The three that take no argument declare none; surroundContents takes
       a `Node newParent`, whose interface arm brands it before step 1 runs. */
    g_id_delete = idl_method_id_step(ctx, NULL, 0, NULL, 0, &RANGE_DELETE, 0);
    idl_this_iface(range_is, "Range");
    g_id_extract = idl_method_id_step(ctx, NULL, 0, NULL, 0, &RANGE_EXTRACT, RX_EXTRACT);
    idl_this_iface(range_is, "Range");
    g_id_clone_contents = idl_method_id_step(ctx, NULL, 0, NULL, 0, &RANGE_EXTRACT, RX_CLONE_CONTENTS);
    idl_this_iface(range_is, "Range");
    g_id_surround = idl_method_id_step(ctx, ONE_NODE, 1, NULL, 0, &RANGE_SURROUND, 0);
    idl_this_iface(range_is, "Range");
    idl_iface_brand(node_class_id());
    /* HTML §8.5.7's `partial interface Range` — `[CEReactions, NewObject] DocumentFragment
       createContextualFragment((TrustedHTML or DOMString) string)`. ONE REQUIRED argument, so no
       idl_optional_from: Web IDL §3.6 throws a TypeError before any conversion when a call passes fewer
       arguments than a member has required ones, which is what makes `range.createContextualFragment()` a
       TypeError rather than a parse of nothing.
       IDL_DOMSTRING FOR THE UNION, because TrustedHTML is Trusted Types §2's interface and this platform has
       none — so every value takes the DOMString arm, and there is NO [LegacyNullToEmptyString] on it (unlike
       §8.5.4 innerHTML's), which is why `createContextualFragment(null)` parses the four characters `null`
       and `(undefined)` the nine characters `undefined`.
       `[CEReactions]` needs no statement here: core/idl_args.c wraps EVERY member in HTML §4.13.6's element
       queue, so a custom element the placement upgrades runs its reactions at this member's epilogue like
       any other. */
    g_id_contextual_fragment = idl_method_id_step(ctx, ONE_HTML, 1, NULL, 0, &RANGE_CONTEXTUAL_FRAGMENT, 0);
    idl_this_iface(range_is, "Range");
    /* CSSOM VIEW §9's `partial interface Range` — neither member takes an argument. */
    g_id_client_rects = idl_method_id(ctx, NULL, 0, js_range_rects, R_CLIENT_RECTS);
    idl_this_iface(range_is, "Range");
    g_id_bounding_rect = idl_method_id(ctx, NULL, 0, js_range_rects, R_BOUNDING_RECT);
    idl_this_iface(range_is, "Range");

    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. Every one of these twenty-four
       slots was held by a release that gave NONE of them back: `range_free` closed the live list and returned,
       so the class this agent registered, its twenty-three pool entries and the class it claimed in §5.3's
       walk all survived the runtime they belong to. Nothing could report it — a pool entry is an int and a
       class id is an int, so neither of JS_FreeRuntime's two censuses has anything to say about them, and the
       only reader of a stale one is the NEXT agent's `range_init`, which consults `g_range_class` precisely to
       decide it need not run.
       DECLARED UNDER `element`, BECAUSE THE NAME IS THE ROW'S AND NOT THE FILE'S. core/platform.c's list has
       no `range` row and must not grow one: nothing releases this component but element_free, which is that
       row's release column, so `element` is the name whose pairing this declaration is the inverse of. Beside
       core/dom/selection.c's seventeen, which name `document` for the same reason.
       AND THE STANDARD IS NAMED IN EACH `what`, because these are read out of a report headed `element`, where
       a bare §5.5 would be no standard at all. */
    agent_state_class("element", &g_range_class,
                      "DOM §5.5's Range class, and this component's declaration latch");
    for (k = 0; k < R_MEMBER_N; k++) agent_state_id("element", &g_id[k], R_WHAT[k]);
    agent_state_id("element", &g_id_to_string, "DOM §5.5's stringifier step-machine declaration");
    agent_state_id("element", &g_id_delete, "DOM §5.5's deleteContents step-machine declaration");
    agent_state_id("element", &g_id_extract, "DOM §5.5's extractContents step-machine declaration");
    agent_state_id("element", &g_id_clone_contents,
                   "DOM §5.5's cloneContents step-machine declaration");
    agent_state_id("element", &g_id_surround, "DOM §5.5's surroundContents step-machine declaration");
    agent_state_id("element", &g_id_client_rects,
                   "CSSOM VIEW §9 Extensions to the Range Interface's getClientRects declaration");
    agent_state_id("element", &g_id_bounding_rect,
                   "CSSOM VIEW §9 Extensions to the Range Interface's getBoundingClientRect declaration");
    agent_state_id("element", &g_id_contextual_fragment,
                   "HTML §8.5.7 The createContextualFragment() method's step-machine declaration");
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
    idl_install_method(ctx, proto, "setStart", g_id[R_SET_START]);
    idl_install_method(ctx, proto, "setEnd", g_id[R_SET_END]);
    idl_install_method(ctx, proto, "setStartBefore", g_id[R_SET_START_BEFORE]);
    idl_install_method(ctx, proto, "setStartAfter", g_id[R_SET_START_AFTER]);
    idl_install_method(ctx, proto, "setEndBefore", g_id[R_SET_END_BEFORE]);
    idl_install_method(ctx, proto, "setEndAfter", g_id[R_SET_END_AFTER]);
    idl_install_method(ctx, proto, "collapse", g_id[R_COLLAPSE]);
    idl_install_method(ctx, proto, "selectNode", g_id[R_SELECT_NODE]);
    idl_install_method(ctx, proto, "selectNodeContents", g_id[R_SELECT_CONTENTS]);
    idl_install_method(ctx, proto, "compareBoundaryPoints", g_id[R_COMPARE_BP]);
    idl_install_method(ctx, proto, "cloneRange", g_id[R_CLONE]);
    idl_install_method(ctx, proto, "detach", g_id[R_DETACH]);
    idl_install_method(ctx, proto, "isPointInRange", g_id[R_IS_POINT_IN]);
    idl_install_method(ctx, proto, "comparePoint", g_id[R_COMPARE_POINT]);
    idl_install_method(ctx, proto, "intersectsNode", g_id[R_INTERSECTS]);
    idl_install_method(ctx, proto, "deleteContents", g_id_delete);
    idl_install_method(ctx, proto, "extractContents", g_id_extract);
    idl_install_method(ctx, proto, "cloneContents", g_id_clone_contents);
    idl_install_method(ctx, proto, "insertNode", g_id[R_INSERT_NODE]);
    idl_install_method(ctx, proto, "surroundContents", g_id_surround);
    idl_install_method(ctx, proto, "toString", g_id_to_string);
    idl_install_method(ctx, proto, "createContextualFragment", g_id_contextual_fragment);
    idl_install_method(ctx, proto, "getClientRects", g_id_client_rects);
    idl_install_method(ctx, proto, "getBoundingClientRect", g_id_bounding_rect);
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
    idl_define_global_property_reference(ctx, global, "Range", ctor);
}


/* THE LIST OUTLIVES THE COMPONENT'S TEARDOWN, and it has to. A component's `_free` runs BEFORE the runtime's
   final sweep, so the objects whose finalizers unregister are still alive when it does — freeing the array
   there is a use-after-free at the first finalizer, and asserting the list is empty there asserts an order the
   runtime does not have. So teardown only says "no more registrations are coming", and the LAST finalizer
   frees the array. A list that is already empty is freed at once, which is the ordinary case. */
void range_free(JSRuntime *rt)
{
    int k;

    DCHECK(g_range_class != 0, "§5.5's Range was released in an agent that never declared it");
    g_live_closed = 1;
    range_live_drop();
    /* AND THE TWENTY-FOUR SLOTS, GIVEN BACK — the half this release did not have. The CLASS goes back to 0
       because a class is registered in a RUNTIME (core/agent_state.h states the one policy): a carried id
       would name a class in a runtime that is gone AND, being the latch above, would make the next agent's
       `range_init` return before re-registering it. The pool entries go back to -1 because the pool they
       index is the agent's too, and an entry the next agent's `range_install_proto` reads is an index into a
       pool that no longer exists. */
    for (k = 0; k < R_MEMBER_N; k++) g_id[k] = -1;
    g_id_to_string = g_id_delete = g_id_extract = g_id_clone_contents = g_id_surround = -1;
    g_id_client_rects = g_id_bounding_rect = g_id_contextual_fragment = -1;
    g_range_class = 0;
    abstract_range_free(rt);
}
