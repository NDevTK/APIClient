/* SELECTION — Selection API §3 "Selection interface", §4.1 "Extensions to Document interface" and §4.2
 * "Extensions to Window interface" (W3C, 11 June 2026; the editor's draft at w3c.github.io/selection-api
 * carries the identical interface).
 *
 * IT IS A COMPONENT OVER §5.5's LIVE RANGE AND HOLDS NO BOUNDARY POINTS OF ITS OWN. §2 gives a selection
 * exactly two pieces of state — "Each selection can be associated with a single range" and "Each selection has
 * a direction" — and every one of §3's twenty-two members is stated over those two. So this file is the
 * selection's state plus §3's algorithms; the tree work is core/dom/range.c's and is reached, never copied.
 *
 * WHY §5 "RESPONDING TO DOM MUTATIONS" NEEDS NO CODE HERE, AND WHY THAT IS A DESIGN CHOICE RATHER THAN AN
 * OMISSION. Its four clauses all say the same thing: on a replace-data, a split, a normalize and an
 * insert-or-remove, "the user agent must update the range associated with selection … as if it's a live
 * range". The range a selection holds IS one — every member below that mints a range mints it through
 * range_new_bp, which registers it with §5.5's live-range set, and `addRange` takes the page's own Range
 * object by reference. A selection holding its own detached pair of boundary points would need all four
 * clauses re-implemented here, and they would be a second answer to what a live range does.
 *
 * THE SELECTION IS PER DOCUMENT AND ITS STATE IS PER FLOW, and those are two different mechanisms.
 *   PER DOCUMENT: §2's "Every document with a browsing context has a unique selection associated with it …
 *   This one selection must be shared by all the content of the document (though not by nested documents)".
 *   The object is built WITH the document (document.c's record, beside §4.5's `[SameObject] implementation`)
 *   so it belongs to the pre-boot BASELINE — one built lazily on the first `getSelection()` would belong to
 *   whichever flow happened to read first. Its PROTOTYPE is this realm's, out of quickjs's per-context class
 *   proto slot, because a member's `ctx` is the realm that DEFINED it: a prototype installed once would answer
 *   every document's `getSelection()` out of the first realm that built one.
 *   PER FLOW: the record is C behind a class opaque, so its writes are invisible to every property hook — the
 *   shape §COW's host-record rule is about. `sel_of` captures it into the running flow's delta, so a flow that
 *   calls `selectAllChildren(x)` selects nothing in a sibling and a parked flow resumes with its own
 *   selection. The capture is in the ACCESSOR because a record a flow has REACHED is one it may write, which
 *   is what leaves no write site to miss.
 *
 * WHAT IS DELIBERATELY NOT HERE. §3's `modify()` — see the idl_members_excluded declaration at the bottom, and
 * the reason there. §6's `selectstart`/`selectionchange` events and §2's "has scheduled selectionchange event"
 * are not this interface's members at all: §4.3 puts them on GlobalEventHandlers, and every one of §6.2.1's
 * scheduling conditions fires on a change this engine has no producer for yet. */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/dom/abstract_range.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/range.h"
#include "core/dom/selection.h"
#include "core/dom/shadow_root.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"

/* §2's DIRECTION. Three states, and the standard names all three. */
enum { SEL_DIRECTIONLESS = 0, SEL_FORWARDS = 1, SEL_BACKWARDS = 2 };

typedef struct Selection {
    /* §2's "associated with a document" — the Document's own WRAPPER rather than the Lexbor pointer. It is a
       counted reference, so a page that keeps `getSelection()` past its document keeps the wrapper that names
       the tree alive with it; a raw pointer here would be §4.5.1's DOMImplementation problem, which needs a
       detach at the record's release to stay honest. OWNED. */
    JSValue doc_obj;
    /* §2's ASSOCIATED RANGE: a §5.5 Range object, or JS_NULL when the selection is EMPTY. Held BY REFERENCE
       and never copied, which §3's addRange states outright ("Set this's range to range by a strong reference
       (not by making a copy)") and its note turns into an observable: `getSelection().getRangeAt(0) ===
       getSelection().getRangeAt(0)`, and a later `r.selectNode(b)` changing what the selection contains.
       OWNED. */
    JSValue range;
    int direction;
} Selection;

/* THE ONE LIST OF WHAT THIS RECORD OWNS — the per-flow COW capture, the finalizer and the gc_mark all iterate
   IT and none of them restates it. A field on one list and not another is either a leak the runtime's own walk
   reports with no owner named, or an over-subtracted refcount and a use-after-free; there is no third outcome,
   and the two are one edit apart. */
#define SO(f) (uint16_t)offsetof(Selection, f)
static const uint16_t SEL_VALS[] = { SO(doc_obj), SO(range) };
static const CowRecord SEL_REC = { sizeof(Selection), SEL_VALS, (int)(sizeof(SEL_VALS) / sizeof(SEL_VALS[0])) };

static JSClassID g_sel_class;

static Selection *sel_of(JSValueConst v)
{
    Selection *s = JS_GetOpaque(v, g_sel_class);
    /* THE RECORD TIME-TRAVELS — see the header paragraph. `direction` rides the same capture because it is in
       the same struct; the memcpy takes it and the two owned values are duplicated by offset. */
    if (s) cow_capture_host_record(v, s, &SEL_REC);
    return s;
}

/* Web IDL §3.7.6 Attributes and §3.7.7 Operations — this entry serves both, and each states the same step:
   a member reached with a receiver that does not implement the interface is a TypeError thrown
   at the call. NOT a DCHECK — a page (and the corpus) reaches for one deliberately. */
static Selection *sel_here(JSContext *ctx, JSValueConst v)
{
    Selection *s = sel_of(v);
    if (!s) { JS_ThrowTypeError(ctx, "this is not a Selection"); return NULL; }
    return s;
}

/* THE COLLECTOR'S TWO ENTRIES REACH THE RECORD FROM THE OBJECT AND READ NO STATIC THIS COMPONENT'S RELEASE
 * RESETS — core/agent_state.h's rule, and this component was written breaking it.
 *
 * `selection_free` is a row reached from document_agent_free, which is on core/platform.h's release column, and
 * every host's teardown is `platform_agent_free()` … `JS_RunGC` … `JS_FreeRuntime` in that order. So a Selection
 * still live at teardown — and there is one per document, minted by document_install whether or not a page ever
 * calls getSelection() — is finalized in a collection that runs AFTER `g_sel_class` is back at 0, and
 * `JS_GetOpaque(val, 0)` answers NULL for every one of them.
 *
 * THE MARK IS THE HALF THAT TOOK THE RUNTIME DOWN, exactly as agent_state.h records for dom_rect.c: an unmarked
 * child keeps the internal reference gc_decref exists to subtract, so gc_scan reads the Document WRAPPER this
 * record holds as rooted from OUTSIDE the heap, and the realm behind it is never collected — which is the
 * @WHY JS_FreeRuntime raises about a realm that survived the collection. The finalizer half is the quieter
 * one: it would leak the record and never subtract its two references.
 *
 * JS_GetAnyOpaque, because the collector dispatched here THROUGH the class — the id is a fact it already has
 * and must not look up. It is NOT compared against `g_sel_class` either: that is remote_object.c's failure in
 * the same note, a guaranteed false @WHY for any live object. `sel_of` keeps the class test, because that one
 * is Web IDL §3.7.6 Attributes' and §3.7.7 Operations' BRAND and runs while the agent is live. */
static void sel_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    Selection *s = JS_GetAnyOpaque(val, &id);
    size_t i;

    (void)id;
    /* NOT `if (!s) return;`. selection_new is the one mint and it builds the record BEFORE the object, with
       nothing between the mint and the JS_SetOpaque, so there is no window in which a Selection exists without
       one and nothing for either entry to meet. */
    DCHECK(s != NULL, "a Selection was finalized with no record — selection_new builds the record before it "
                      "mints the object, so an object of this class without one was built somewhere that is "
                      "not this file");
    for (i = 0; i < sizeof(SEL_VALS) / sizeof(SEL_VALS[0]); i++)
        JS_FreeValueRT(rt, *(JSValue *)((char *)s + SEL_VALS[i]));
    free(s);
}

static void sel_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    Selection *s = JS_GetAnyOpaque(val, &id);
    size_t i;

    /* THE DOCUMENT HOLDS THE SELECTION AND THE SELECTION HOLDS THE DOCUMENT, which is a cycle the collector can
       only break if it is told about both edges. document.c's realm mark walks the record's side (the
       `selection` entry of doc_rec_refs); this is the other half. JS_GetAnyOpaque and never sel_of, for the
       reason every other component's mark reaches past its accessor: a COW capture during a collection would
       dup values on an object being torn down. */
    (void)id;
    DCHECK(s != NULL, "a Selection was marked with no record — selection_new builds the record before it "
                      "mints the object, so one cannot reach a collection without it");
    for (i = 0; i < sizeof(SEL_VALS) / sizeof(SEL_VALS[0]); i++)
        JS_MarkValue(rt, *(JSValue *)((char *)s + SEL_VALS[i]), mark_func);
}

/* ---- §2's STATE, READ ------------------------------------------------------------------------------------ */

static lxb_dom_node_t *sel_doc(const Selection *s)
{
    lxb_dom_node_t *d = node_of(s->doc_obj);
    DCHECK(d != NULL && d->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "a Selection's associated document is not a Document node — §2 associates a selection with the "
           "document it was built for and nothing rewrites the field");
    return d;
}

/* §2: "When there is no range associated with the selection, the selection is empty." */
static bool sel_empty(const Selection *s) { return JS_IsNull(s->range); }

/* The bounds of the range this selection is associated with, or NULL when it is empty. Going through
   abstract_range_of is what captures the RANGE's own record into this flow's delta, which matters because
   §3's members read the range's boundary points and `addRange` hands the page a range it can still move. */
static RangeBounds *sel_bounds(const Selection *s)
{
    RangeBounds *b;
    if (sel_empty(s)) return NULL;
    b = abstract_range_of(s->range);
    DCHECK(b != NULL, "a Selection's associated range is not a §5 range object — §3's only writers are "
                      "addRange, whose argument the IDL brands against Range, and this file's own mints");
    return b;
}

/* §2's ANCHOR and FOCUS, as ONE question: "If the selection's range is not null and its direction is forwards,
   its anchor is the range's start, and its focus is the end. Otherwise, its focus is the start and its anchor
   is the end." One conditional rather than two, because backwards and directionless take the same arm.
   `want_anchor` picks which of the pair; the caller has already established the selection is not empty. */
static void sel_point(const Selection *s, const RangeBounds *b, bool want_anchor,
                      JSValueConst *pnode, uint32_t *poff)
{
    bool at_start = (s->direction == SEL_FORWARDS) == want_anchor;

    *pnode = at_start ? b->start_node : b->end_node;
    *poff  = at_start ? b->start_off  : b->end_off;
}

/* §3's "in the document tree" — the node's ROOT is the document this selection is associated with. §2's own
   note is why this is a real condition and not a tautology: "anchor and focus of selection need not to be in
   the document tree. It could be in a shadow tree of the same document." A boundary point inside a shadow tree
   has a SHADOW ROOT for a root, so every getter guarded by this clause answers null/0/"None"/0 for it. */
static bool sel_in_document_tree(const Selection *s, JSValueConst node)
{
    lxb_dom_node_t *n = node_of(node);
    return n != NULL && node_root(n) == sel_doc(s);
}

/* The clause §3 repeats on rangeCount, type and getRangeAt: "this is empty or either focus or anchor is not in
   the document tree". Both points are tested, not just one, which is the standard's own "either". */
static bool sel_reportable(const Selection *s)
{
    RangeBounds *b = sel_bounds(s);
    if (!b) return false;
    return sel_in_document_tree(s, b->start_node) && sel_in_document_tree(s, b->end_node);
}

/* ---- §2's STATE, WRITTEN --------------------------------------------------------------------------------- */

/* CONSUMES `r` — JS_NULL to make the selection empty. */
static void sel_set_range(JSContext *ctx, Selection *s, JSValue r)
{
    JS_FreeValue(ctx, s->range);
    s->range = r;
}

/* §3's five members that "let newRange be a new range" and then set this's range to it. It is a LIVE range
   (range_new_bp), which is what makes Selection API §5 a statement about this engine rather than a to-do. The
   two points must already be in §5.2 order, which every caller establishes; range_new_bp asserts it.
   THE NEW RANGE IS BUILT BEFORE THE OLD ONE IS RELEASED, and that is load-bearing rather than incidental:
   collapseToStart and extend name boundary points that live in the OLD range's record, so a release first
   would hand this function two freed wrappers. */
static void sel_set_new_range(JSContext *ctx, Selection *s, JSValueConst sn, uint32_t so,
                              JSValueConst en, uint32_t eo)
{
    sel_set_range(ctx, s, range_new_bp(ctx, sn, so, en, eo));
}

/* ---- §3's ATTRIBUTES -------------------------------------------------------------------------------------- */

enum { SG_ANCHOR_NODE = 0, SG_ANCHOR_OFFSET, SG_FOCUS_NODE, SG_FOCUS_OFFSET, SG_IS_COLLAPSED,
       SG_RANGE_COUNT, SG_TYPE, SG_DIRECTION };

static JSValue js_sel_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    Selection *s = sel_here(ctx, this_val);
    RangeBounds *b;
    JSValueConst node;
    uint32_t off;

    if (!s) return JS_EXCEPTION;
    b = sel_bounds(s);

    switch (magic) {
    case SG_ANCHOR_NODE:
    case SG_ANCHOR_OFFSET:
    case SG_FOCUS_NODE:
    case SG_FOCUS_OFFSET:
        /* "…or null if the anchor is null or anchor is not in the document tree", and 0 for the offsets. */
        if (!b) return (magic == SG_ANCHOR_NODE || magic == SG_FOCUS_NODE) ? JS_NULL : JS_NewUint32(ctx, 0);
        sel_point(s, b, magic == SG_ANCHOR_NODE || magic == SG_ANCHOR_OFFSET, &node, &off);
        if (!sel_in_document_tree(s, node))
            return (magic == SG_ANCHOR_NODE || magic == SG_FOCUS_NODE) ? JS_NULL : JS_NewUint32(ctx, 0);
        return (magic == SG_ANCHOR_NODE || magic == SG_FOCUS_NODE) ? JS_DupValue(ctx, node)
                                                                   : JS_NewUint32(ctx, off);

    case SG_IS_COLLAPSED:
        /* "true if and only if the anchor and focus are the same (including if both are null)". It is stated
           over §2's anchor and focus THEMSELVES and not over the getters above, so the document-tree clause
           does not enter: an empty selection's two nulls are the same, and a non-empty one's two points are
           the same exactly when its range is collapsed. */
        if (!b) return JS_TRUE;
        return JS_NewBool(ctx, node_of(b->start_node) == node_of(b->end_node) && b->start_off == b->end_off);

    case SG_RANGE_COUNT:
        return JS_NewUint32(ctx, sel_reportable(s) ? 1u : 0u);

    case SG_TYPE:
        if (!sel_reportable(s)) return JS_NewString(ctx, "None");
        return JS_NewString(ctx, node_of(b->start_node) == node_of(b->end_node) &&
                                 b->start_off == b->end_off ? "Caret" : "Range");

    default:
        DCHECK(magic == SG_DIRECTION, "a Selection attribute ran with a magic §3 does not declare");
        /* "…'none' if this is empty or this selection is directionless." The empty test is FIRST and is not
           the same test: a selection can be empty with a direction still recorded from before. */
        if (sel_empty(s) || s->direction == SEL_DIRECTIONLESS) return JS_NewString(ctx, "none");
        return JS_NewString(ctx, s->direction == SEL_FORWARDS ? "forward" : "backward");
    }
}

/* ---- §3's OPERATIONS -------------------------------------------------------------------------------------- */

/* §3's OPERATIONS, EACH BESIDE THE SENTENCE A REPORT ABOUT THIS COMPONENT'S RELEASE PRINTS FOR IT. They are
   ONE list because they are read together: `g_id` below is twelve pool entries this agent holds, core/
   agent_state.h asserts each of them is back at -1 once `document`'s release column has run, and the `what` it
   names one by is read out of a report headed `document` — where an index into an enum would name nothing a
   reader can go and look at, and where a bare `§3` would be DOM's rather than this standard's. Two lists could
   drift by one line and the report would then describe the wrong member with nothing to say so. */
#define SELECTION_OPERATIONS(X)                                                                        \
    X(SM_GET_RANGE_AT,        "Selection API §3's getRangeAt declaration")                             \
    X(SM_ADD_RANGE,           "Selection API §3's addRange declaration")                               \
    X(SM_REMOVE_RANGE,        "Selection API §3's removeRange declaration")                            \
    X(SM_REMOVE_ALL,          "Selection API §3's removeAllRanges declaration, which `empty` aliases")  \
    X(SM_COLLAPSE,            "Selection API §3's collapse declaration, which `setPosition` aliases")   \
    X(SM_COLLAPSE_TO_START,   "Selection API §3's collapseToStart declaration")                        \
    X(SM_COLLAPSE_TO_END,     "Selection API §3's collapseToEnd declaration")                          \
    X(SM_EXTEND,              "Selection API §3's extend declaration")                                 \
    X(SM_SET_BASE_AND_EXTENT, "Selection API §3's setBaseAndExtent declaration")                       \
    X(SM_SELECT_ALL_CHILDREN, "Selection API §3's selectAllChildren declaration")                      \
    X(SM_CONTAINS_NODE,       "Selection API §3's containsNode declaration")                           \
    X(SM_COMPOSED_RANGES,     "Selection API §3's getComposedRanges declaration")
enum {
#define X(m, w) m,
    SELECTION_OPERATIONS(X)
#undef X
    SM_N
};

/* The condition §3's collapse (step 4), extend (step 1) and setBaseAndExtent (step 2) all open with:
   "If document associated with this is not a shadow-including inclusive ancestor of node, abort these steps."
   ABORT, not throw — every one of the three says so, and a page tells the difference. */
static bool sel_doc_reaches(const Selection *s, lxb_dom_node_t *n)
{
    return shadow_root_is_shadow_including_inclusive_ancestor(sel_doc(s), n);
}

/* §3's selectAllChildren step 3: "let … childCount be the number of children of node". IT IS NOT DOM §4.4's
   `length`, and the difference is a real answer and not a nicety: §4.4 gives a CharacterData node its data's
   length in code units, so `selectAllChildren(textNode)` read through `length` would select a span of
   characters inside a node that has no children at all. Counted here rather than reached for, because DOM
   exposes no "number of children" that is not §4.4's length. */
static uint32_t sel_child_count(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *c;
    uint32_t k = 0;
    for (c = n->first_child; c; c = c->next) k++;
    return k;
}

/* §5.2's position between two boundary points, when they may be in DIFFERENT trees. §3's extend and
   setBaseAndExtent both compare points that the step before them has NOT constrained to one root — extend's
   step 5 exists precisely for the different-root case — so the comparison has to be asked safely rather than
   through boundary_position, whose own first assertion is that the roots agree. */
static bool sel_bp_before_or_equal(lxb_dom_node_t *a, uint32_t ao, lxb_dom_node_t *b, uint32_t bo)
{
    DCHECK(node_root(a) == node_root(b),
           "§3 compared two boundary points across two trees — every caller establishes one root first, "
           "because §5.2's position has no answer between them");
    return boundary_position(a, ao, b, bo) != BP_AFTER;
}

static JSValue js_sel_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    Selection *s = sel_here(ctx, this_val);
    JSValueConst nodev = argc > 0 ? argv[0] : JS_UNDEFINED;
    RangeBounds *b;
    lxb_dom_node_t *n;
    uint32_t off = 0;

    if (!s) return JS_EXCEPTION;
    b = sel_bounds(s);

    switch (magic) {
    case SM_GET_RANGE_AT: {
        /* "must throw an IndexSizeError exception if index is not 0, or if this is empty or either focus or
           anchor is not in the document tree. Otherwise, it must return a reference to (not a copy of) this's
           range." The reference is what makes the note's `getRangeAt(0) === getRangeAt(0)` hold. */
        uint32_t index = 0;
        if (JS_ToUint32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION;
        if (index != 0 || !sel_reportable(s))
            return JS_ThrowDOMException(ctx, "IndexSizeError",
                                        "a Selection has at most one range and only while it is in the "
                                        "document tree");
        return JS_DupValue(ctx, s->range);
    }

    case SM_ADD_RANGE: {
        RangeBounds *rb = abstract_range_of(argv[0]);
        DCHECK(rb != NULL, "addRange reached the body with something that is not a Range — the argument's "
                           "interface type is what brands it");
        /* STEP 1. "If the root of the range's boundary points are not the document associated with this,
           abort these steps." A live range's two points always share a root (§5.5 maintains it), so the
           root of "the range's boundary points" is one node. */
        DCHECK(node_root(node_of(rb->start_node)) == node_root(node_of(rb->end_node)),
               "a live range's two boundary points are in different trees — §5.5's setters move the other "
               "point precisely so that they cannot be");
        if (node_root(node_of(rb->start_node)) != sel_doc(s)) return JS_UNDEFINED;
        if (sel_reportable(s)) return JS_UNDEFINED;                                          /* STEP 2 */
        sel_set_range(ctx, s, JS_DupValue(ctx, argv[0]));                                    /* STEP 3 */
        return JS_UNDEFINED;
    }

    case SM_REMOVE_RANGE:
        /* "must make this empty by disassociating its range if this's range is range. Otherwise, it must throw
           a NotFoundError." The test is OBJECT IDENTITY, which is the whole point of holding the page's own
           Range by reference. */
        if (!sel_empty(s) && JS_VALUE_GET_PTR(s->range) == JS_VALUE_GET_PTR(argv[0])) {
            sel_set_range(ctx, s, JS_NULL);
            return JS_UNDEFINED;
        }
        return JS_ThrowDOMException(ctx, "NotFoundError", "that range is not the selection's range");

    case SM_REMOVE_ALL:
        /* removeAllRanges, and `empty()`, which "must be an alias, and behave identically" to it — so it is
           the same declaration installed under a second name rather than a second body. */
        sel_set_range(ctx, s, JS_NULL);
        return JS_UNDEFINED;

    case SM_COLLAPSE:
        /* collapse, and `setPosition`, which "must be an alias, and behave identically" to it. */
        if (JS_IsNull(nodev)) { sel_set_range(ctx, s, JS_NULL); return JS_UNDEFINED; }        /* STEP 1 */
        n = node_of(nodev);
        DCHECK(n != NULL, "collapse reached the body with something that is neither a Node nor null");
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)                                       /* STEP 2 */
            return JS_ThrowDOMException(ctx, "InvalidNodeTypeError",
                                        "a selection cannot be collapsed onto a DocumentType");
        if (argc > 1 && JS_ToUint32(ctx, &off, argv[1]) < 0) return JS_EXCEPTION;
        if (off > node_length(n))                                                             /* STEP 3 */
            return JS_ThrowDOMException(ctx, "IndexSizeError", "the offset is past the node's length");
        if (!sel_doc_reaches(s, n)) return JS_UNDEFINED;                                      /* STEP 4 */
        sel_set_new_range(ctx, s, nodev, off, nodev, off);                                 /* STEPS 5-7 */
        return JS_UNDEFINED;

    case SM_COLLAPSE_TO_START:
    case SM_COLLAPSE_TO_END:
        /* "must throw InvalidStateError exception if the this is empty. Otherwise, it must create a new range,
           set the start both its start and end to the start [end] of this's range, and then set this's range
           to the newly-created range." A NEW range, leaving the old object unchanged, which the note says is
           deliberate and which is why this is not a `collapse()` on the existing one. */
        if (!b)
            return JS_ThrowDOMException(ctx, "InvalidStateError", "the selection is empty");
        if (magic == SM_COLLAPSE_TO_START)
            sel_set_new_range(ctx, s, b->start_node, b->start_off, b->start_node, b->start_off);
        else
            sel_set_new_range(ctx, s, b->end_node, b->end_off, b->end_node, b->end_off);
        return JS_UNDEFINED;

    case SM_EXTEND: {
        JSValueConst old_anchor_node;
        uint32_t old_anchor_off;
        bool same_root, focus_first;

        n = node_of(nodev);
        DCHECK(n != NULL, "extend reached the body with something that is not a Node");
        if (!sel_doc_reaches(s, n)) return JS_UNDEFINED;                                      /* STEP 1 */
        if (!b)                                                                               /* STEP 2 */
            return JS_ThrowDOMException(ctx, "InvalidStateError", "the selection is empty");
        if (argc > 1 && JS_ToUint32(ctx, &off, argv[1]) < 0) return JS_EXCEPTION;
        sel_point(s, b, /*want_anchor*/true, &old_anchor_node, &old_anchor_off);              /* STEP 3 */
        /* STEP 5. "If node's root is not the same as the this's range's root" — the range's root is one node
           because §5.5 keeps its two points in one tree, which addRange's own assertion above states. */
        same_root = node_root(n) == node_root(node_of(b->start_node));
        if (!same_root) {
            sel_set_new_range(ctx, s, nodev, off, nodev, off);                              /* STEPS 4-5 */
            /* STEP 9 still runs, and its comparison is between two points that are now in different trees.
               §5.2 has no answer there, and the standard's own step 5 is the acknowledgement that the two are
               not comparable — so the direction that survives is FORWARDS, which is what step 9's "Otherwise"
               arm gives every case its `if` cannot decide. */
            s->direction = SEL_FORWARDS;
            return JS_UNDEFINED;
        }
        focus_first = !sel_bp_before_or_equal(node_of(old_anchor_node), old_anchor_off, n, off);
        if (!focus_first) sel_set_new_range(ctx, s, old_anchor_node, old_anchor_off, nodev, off);  /* STEP 6 */
        else              sel_set_new_range(ctx, s, nodev, off, old_anchor_node, old_anchor_off);  /* STEP 7 */
        s->direction = focus_first ? SEL_BACKWARDS : SEL_FORWARDS;                            /* STEP 9 */
        return JS_UNDEFINED;
    }

    case SM_SET_BASE_AND_EXTENT: {
        JSValueConst an = argv[0], fn = argv[2];
        uint32_t ao = 0, fo = 0;
        lxb_dom_node_t *anode = node_of(an), *fnode = node_of(fn);
        bool focus_first;

        DCHECK(anode != NULL && fnode != NULL,
               "setBaseAndExtent reached the body with something that is not a Node");
        if (JS_ToUint32(ctx, &ao, argv[1]) < 0 || JS_ToUint32(ctx, &fo, argv[3]) < 0) return JS_EXCEPTION;
        if (ao > node_length(anode) || fo > node_length(fnode))                                /* STEP 1 */
            return JS_ThrowDOMException(ctx, "IndexSizeError", "an offset is past its node's length");
        if (!sel_doc_reaches(s, anode) || !sel_doc_reaches(s, fnode)) return JS_UNDEFINED;     /* STEP 2 */
        /* STEPS 3-5. Both nodes are shadow-including inclusive descendants of one document by step 2, and
           §5.2's position is asked between them — which is well-defined only when their ROOTS agree. Two
           nodes of one document can sit in two different shadow trees and reach here, and §3 step 5 states
           the comparison with no case for it: the ordering that WOULD answer is over the composed tree, which
           DOM defines no position for. It is a CRASH and not an invented answer, because either arm of step 5
           would build a range whose start is in one tree and whose end is in another — a §5.5 invariant
           violation that would then be silently carried by every member reading it. */
        if (node_root(anode) != node_root(fnode))
            DFAIL("Selection API §3 setBaseAndExtent step 5 was reached with its anchor and focus in two "
                  "different roots of one document (two shadow trees, or a shadow tree and the document) — "
                  "what to build is a composed-tree boundary-point ordering, which DOM §5.2 does not define "
                  "and §3's getComposedRanges is the only member that names the composed tree at all");
        focus_first = !sel_bp_before_or_equal(anode, ao, fnode, fo);
        if (!focus_first) sel_set_new_range(ctx, s, an, ao, fn, fo);
        else              sel_set_new_range(ctx, s, fn, fo, an, ao);
        s->direction = focus_first ? SEL_BACKWARDS : SEL_FORWARDS;                             /* STEP 7 */
        return JS_UNDEFINED;
    }

    case SM_SELECT_ALL_CHILDREN:
        n = node_of(nodev);
        DCHECK(n != NULL, "selectAllChildren reached the body with something that is not a Node");
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)                                        /* STEP 1 */
            return JS_ThrowDOMException(ctx, "InvalidNodeTypeError",
                                        "a DocumentType has no children to select");
        /* STEP 2 is the node's ROOT and not the shadow-including reach steps 1/2 of the three members above
           use. That asymmetry is the standard's; implementing it as one shared test would silently make
           `selectAllChildren` accept a node inside a shadow tree, which this member alone refuses. */
        if (node_root(n) != sel_doc(s)) return JS_UNDEFINED;
        sel_set_new_range(ctx, s, nodev, 0, nodev, sel_child_count(n));                     /* STEPS 3-6 */
        s->direction = SEL_FORWARDS;                                                           /* STEP 7 */
        return JS_UNDEFINED;

    case SM_CONTAINS_NODE: {
        bool allow_partial = argc > 1 && JS_ToBool(ctx, argv[1]);
        lxb_dom_node_t *rs, *re;
        uint32_t last;

        n = node_of(nodev);
        DCHECK(n != NULL, "containsNode reached the body with something that is not a Node");
        /* "must return false if this is empty or if node's root is not the document associated with this." */
        if (!b || node_root(n) != sel_doc(s)) return JS_FALSE;
        rs = node_of(b->start_node);
        re = node_of(b->end_node);
        /* AND FALSE AGAIN WHEN THE RANGE IS NOT IN THAT TREE. §3 states the two comparisons below with §5.2's
           position, whose first step asserts one root — and the clause above constrains only the NODE's root,
           so a selection §2's own note allows (its points in a shadow tree of this document) reaches them
           across two roots. A node cannot be inside a range of another tree, which is the answer §5.2 would
           give if it had one, and it is stated here rather than left to the assertion inside it. */
        if (node_root(rs) != node_root(n)) return JS_FALSE;
        DCHECK(node_root(rs) == node_root(re),
               "a live range's two boundary points are in different trees — §5.5's setters move the other "
               "point precisely so that they cannot be");
        /* THE FIRST AND LAST BOUNDARY POINTS IN A NODE are (node, 0) and (node, node's length) — §5.2's own
           pair, which is what §5.5's "contained in" is stated over too.
           "BEFORE OR VISUALLY EQUIVALENT TO" IS §5.2's before-or-equal HERE, AND THAT IS NOT A SHORTCUT: the
           visual relaxation can only move a boundary point across content that RENDERS as nothing, and
           deciding that needs a layout this engine does not run. The DOM comparison is what every engine
           ships and is the strictly narrower of the two — it never reports a node contained that the
           relaxation would not.
           "AFTER OR VISUALLY EQUIVALENT TO x" IS "x BEFORE OR EQUAL TO IT" — the same one comparison read from
           the other end, so there is one predicate here and not two. */
        last = node_length(n);
        if (!allow_partial)
            return JS_NewBool(ctx, sel_bp_before_or_equal(rs, b->start_off, n, 0) &&
                                   sel_bp_before_or_equal(n, last, re, b->end_off));
        return JS_NewBool(ctx, sel_bp_before_or_equal(rs, b->start_off, n, last) &&
                               sel_bp_before_or_equal(n, 0, re, b->end_off));
    }

    default:
        DFAIL("a Selection operation ran with a magic §3 does not declare — getComposedRanges is the one "
              "member with a body of its own and never reaches this one");
        return JS_UNDEFINED;
    }
}

/* ---- §3's getComposedRanges() ----------------------------------------------------------------------------- */

/* Step 3's / step 5's inner condition: "startNode's root is not a shadow-including inclusive ancestor of any of
   options["shadowRoots"]". `roots` is the converted sequence — a JS Array of ShadowRoot wrappers, brought here
   by the DECLARATION, so nothing of the page's runs while it is walked. */
static bool sel_root_reaches_any(JSContext *ctx, lxb_dom_node_t *root, JSValueConst roots)
{
    uint32_t i, n = 0;
    JSValue lenv;

    /* §3 writes `sequence<ShadowRoot> shadowRoots = []`, and IdlDictDefault has no arm for an empty sequence,
       so an absent member arrives as `undefined`. THAT IS THE IDL'S OWN VALUE READ POSITIVELY and not a
       consumer-side fill: an empty sequence contains nothing, so "is this root a shadow-including inclusive
       ancestor of any of them" is FALSE — the same answer the walk below would reach with zero entries. */
    if (!JS_IsObject(roots)) return false;
    lenv = JS_GetPropertyStr(ctx, roots, "length");
    JS_ToUint32(ctx, &n, lenv);
    JS_FreeValue(ctx, lenv);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, roots, i);
        lxb_dom_node_t *en = node_of(e);

        JS_FreeValue(ctx, e);
        DCHECK(en != NULL && shadow_root_is(en),
               "§3's shadowRoots list holds something that is not a shadow root — the sequence's element type "
               "is what brands it, so a non-ShadowRoot here means the declaration lost its narrowing");
        if (shadow_root_is_shadow_including_inclusive_ancestor(root, en)) return true;
    }
    return false;
}

/* Steps 3 and 5 are one loop with one difference: the start point takes the host's index and the end point
   takes it PLUS ONE, which is what turns a point before the host into a point after it. */
static void sel_rescope(JSContext *ctx, JSValueConst roots, lxb_dom_node_t **pnode, uint32_t *poff, uint32_t add)
{
    while (*pnode) {
        lxb_dom_node_t *root = node_root(*pnode);
        lxb_dom_element_t *host;

        if (!shadow_root_is(root)) break;
        if (sel_root_reaches_any(ctx, root, roots)) break;
        host = shadow_root_host(root);
        DCHECK(host != NULL, "a shadow root with no host reached §3's getComposedRanges — §4.8's attach is "
                             "what sets the pair and neither half exists without the other");
        *poff = node_index(lxb_dom_interface_node(host)) + add;
        *pnode = lxb_dom_interface_node(host)->parent;
    }
}

static JSValue js_sel_composed_ranges(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    Selection *s = sel_here(ctx, this_val);
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    lxb_dom_node_t *sn, *en;
    uint32_t so, eo;
    JSValue out, roots, sw, ew, sr;

    (void)magic;
    if (!s) return JS_EXCEPTION;
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "§3's getComposedRanges could not allocate its result");
    if (sel_empty(s)) return out;                                                              /* STEP 1 */
    {
        RangeBounds *b = sel_bounds(s);
        sn = node_of(b->start_node);                                                           /* STEP 2 */
        so = b->start_off;
        en = node_of(b->end_node);                                                             /* STEP 4 */
        eo = b->end_off;
    }
    roots = idl_dict_get(ctx, options, "shadowRoots");
    sel_rescope(ctx, roots, &sn, &so, 0);                                                      /* STEP 3 */
    sel_rescope(ctx, roots, &en, &eo, 1);                                                      /* STEP 5 */
    JS_FreeValue(ctx, roots);
    /* STEP 6. Both walks stop at a node, never past one: a shadow root's host always has a parent while the
       host is in a tree, and a root that is not a shadow root ends the loop. */
    DCHECK(sn != NULL && en != NULL,
           "§3's getComposedRanges walked a boundary point out of every tree — the loop climbs from a shadow "
           "root to its host's parent, and a host with no parent is a shadow tree hanging off nothing");
    sw = node_wrap(ctx, sn);
    ew = node_wrap(ctx, en);
    sr = static_range_new(ctx, sw, so, ew, eo);
    JS_FreeValue(ctx, sw);
    JS_FreeValue(ctx, ew);
    JS_SetPropertyUint32(ctx, out, 0, sr);
    return out;
}

/* ---- §3's deleteFromDocument() and its stringifier -------------------------------------------------------- */

/* BOTH ARE §5.5's OWN MACHINES WITH A DIFFERENT RECEIVER, and that is the standard's own construction:
   deleteFromDocument "must invoke deleteContents() on this's range", and the stringifier returns the range's
   text. So the walk is core/dom/range.c's, exported for exactly these two members, and what lives here is the
   subject resolution — which has to run at EVERY step rather than once, because the record time-travels and a
   context switch between two steps swaps the delta the bounds live in. */

/* §3's deleteFromDocument tests its condition ONCE, before it invokes deleteContents — so the answer is
   LATCHED and not re-asked per step. Re-asking would be a condition evaluated in the middle of the algorithm
   it guards: the walk removes nodes and moves the range as it goes, and a step at which the answer flipped
   would abandon a half-finished deletion with no step of the standard to name. */
typedef struct { RangeDelState rd; uint8_t asked, run; } SelDelState;
typedef struct { RangeStrState rs; } SelStrState;

static void sel_del_visit(JSContext *ctx, void *st, JSStepVisit *v) { range_del_visit(ctx, &((SelDelState *)st)->rd, v); }
static void sel_str_visit(JSContext *ctx, void *st, JSStepVisit *v) { range_str_visit(ctx, &((SelStrState *)st)->rs, v); }

static int sel_del_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    Selection *s = sel_here(ctx, hdr->this_val);
    SelDelState *ss = st;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (!s) return JS_STEP_ABRUPT;
    /* "must invoke deleteContents() on this's range if this is not empty and both focus and anchor are in the
       document tree. Otherwise the method must do nothing." */
    if (!ss->asked) { ss->asked = 1; ss->run = sel_reportable(s) ? 1 : 0; }
    if (!ss->run) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
    /* The BOUNDS are re-resolved at every step even though the condition is not, and the two are different
       questions: the record time-travels, so a context switch between two steps swaps the delta this range's
       boundary points live in and a pointer held across one would name the wrong timeline's. */
    return range_del_step(ctx, hdr, &ss->rd, sel_bounds(s), presult);
}

static int sel_str_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    Selection *s = sel_here(ctx, hdr->this_val);
    RangeBounds *b;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (!s) return JS_STEP_ABRUPT;
    /* "the concatenation of the rendered text IF THERE IS A RANGE associated with this" — an empty selection
       stringifies to the empty string, which is the `if` clause and not a shrug. It needs no latch, unlike the
       deletion above: this walk runs none of the page's code and writes nothing, so the range it is over
       cannot go away under it — the bounds are re-resolved per step only because the RECORD time-travels. */
    b = sel_bounds(s);
    if (!b) { *presult = JS_NewString(ctx, ""); return JS_STEP_DONE; }
    return range_str_step(ctx, hdr, &((SelStrState *)st)->rs, b, presult);
}

/* No release for either: each machine's one buffer is its `visit`'s, discharged with the rest. */
static const IdlStepDecl SELECTION_DELETE = {
    sel_del_step, sizeof(SelDelState), sel_del_visit, NULL,
    "Selection API §3 deleteFromDocument(), over DOM §5.5's deleteContents steps", RANGE_DEL_STEPS };
static const IdlStepDecl SELECTION_STRINGIFIER = {
    sel_str_step, sizeof(SelStrState), sel_str_visit, NULL,
    "Selection API §3 stringifier, over DOM §5.5's stringification steps", RANGE_STR_STEPS };

/* ---- §4.1 and §4.2's ENTRY POINTS ------------------------------------------------------------------------- */

/* §4.1: "The method must return the selection associated with this if this has an associated browsing context,
   and it must return null otherwise." The RECEIVER decides which document, because the member is
   Document.prototype's and a realm can hold several documents — `implementation.createHTMLDocument("")` has no
   browsing context and is exactly the null case. */
static JSValue js_document_get_selection(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    (void)argc; (void)argv; (void)magic;
    /* §2's OTHER WRITER, WHICH THIS ENGINE HAS NO PRODUCER FOR: "A document's selection is a singleton object
       associated with that document, so it gets replaced with a new object when Document.open() is called."
       Nothing in this build has an `open()`, so the singleton is written exactly once — by document_install —
       and there is no replacement step to run. Asserted rather than written down, so that the day `open()`
       lands this fires HERE, at the member whose answer would otherwise be a Selection belonging to a document
       that no longer exists.
       IT IS ASKED AT THE MEMBER AND NOT AT THE INSTALL, and that is not a preference: `Document` reaches the
       global well after this component builds a realm's selection, so a probe at the install would resolve a
       path that is not there yet and would pass for ever — a check that can never fire is worse than none. */
    realm_awaits(ctx, "Document.prototype.open",
                 "Selection API §2's note — a document's selection \"gets replaced with a new object when "
                 "Document.open() is called\" — now has a producer, and nothing replaces it: HTML's document "
                 "open steps must build a fresh Selection on the Document's record beside the new tree");
    return document_selection(ctx, this_val);
}

/* §4.2: "The method must invoke and return the result of getSelection() on this's Window.document attribute."
   Stated over the document rather than over the realm, so it is that same call with the realm's own document
   as the receiver — one implementation, and the two members cannot disagree. */
static JSValue js_window_get_selection(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    JSValueConst doc = document_object(ctx);

    (void)this_val; (void)argc; (void)argv; (void)magic;
    DCHECK(node_of(doc) != NULL,
           "window.getSelection() ran in a realm with no Document — §4.2 forwards to `this's Window.document`, "
           "and a Window whose document is not a node has nothing to forward to");
    return document_selection(ctx, doc);
}

/* ---- declaration and installation ------------------------------------------------------------------------- */

JSValue selection_new(JSContext *ctx, JSValueConst doc)
{
    JSValue obj, proto = JS_GetClassProto(ctx, g_sel_class);
    Selection *s;

    DCHECK(!JS_IsNull(proto), "a Selection was built in a realm with no Selection.prototype");
    DCHECK(node_of(doc) != NULL && node_of(doc)->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "§2's selection was asked for something that is not a Document");
    /* THE RECORD IS BUILT FIRST, so there is NOTHING between the object's mint and its JS_SetOpaque — which is
       what makes the two collector entries' `DCHECK(s != NULL)` sound rather than hopeful. A calloc between
       them could not trigger a collection (it is not a JS allocation) but it is one more line for someone to
       put a failing call into, and the ordering costs nothing. */
    s = calloc(1, sizeof(*s));
    CHECK(s != NULL, "the Selection record allocation failed");
    s->doc_obj = JS_DupValue(ctx, doc);
    s->range = JS_NULL;   /* §2: "The selection must be initially empty." */
    /* §2 names a direction's INITIAL value only for a selection the USER created ("If the first indicated
       boundary point is before the second … forwards … Otherwise, it must be directionless"), and this engine
       has no pointing device, so every selection here is script-created and §2 is silent. FORWARDS is that
       silence resolved by §2's own rule rather than by a preference: `addRange` sets no direction, and a live
       range's start is never after its end, so the two indicated points of the only selection a script can
       make without saying a direction are in the forwards order. Directionless is left reachable because it
       is §2's state and the anchor/focus rule reads it; nothing here writes it. */
    s->direction = SEL_FORWARDS;
    obj = JS_NewObjectProtoClass(ctx, proto, g_sel_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the Selection object could not be allocated");
    JS_SetOpaque(obj, s);
    return obj;
}

static int g_id[SM_N], g_id_delete = -1, g_id_stringifier = -1, g_id_doc_get = -1, g_id_win_get = -1;

static const char *const SM_WHAT[] = {
#define X(m, w) w,
    SELECTION_OPERATIONS(X)
#undef X
};

/* §3's `dictionary GetComposedRangesOptions`. One member, `sequence<ShadowRoot> shadowRoots = []`, and it is
   the DECLARATION that runs Web IDL §3.2.21's iterator protocol over it — the page's iterator, its `next` and
   every `done`/`value` read park the machine on the element they are on, which a body walking the list
   afterwards could not reproduce. */
static const IdlDictMember GET_COMPOSED_RANGES_OPTIONS[] = {
    { "shadowRoots", IDL_SEQUENCE_INTERFACE, false, NULL, 0 },
};

/* §3's `modify()`. See the exclusion at install for why. */
static const char *const SELECTION_ABSENT[] = { "modify" };

void selection_init(JSContext *ctx)
{
    JSClassDef d = { "Selection", sel_finalizer, sel_gc_mark };
    static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };
    static const IdlArgType ONE_RANGE[1] = { IDL_INTERFACE };
    static const IdlArgType ONE_NODE[1] = { IDL_INTERFACE };
    static const IdlArgType NODE_OFFSET[2] = { IDL_INTERFACE, IDL_UNSIGNED_LONG };
    static const IdlArgType NULLNODE_OFFSET[2] = { IDL_INTERFACE_NULLABLE, IDL_UNSIGNED_LONG };
    static const IdlArgType NODE_BOOL[2] = { IDL_INTERFACE, IDL_BOOLEAN };
    static const IdlArgType FOUR_POINTS[4] = { IDL_INTERFACE, IDL_UNSIGNED_LONG,
                                              IDL_INTERFACE, IDL_UNSIGNED_LONG };
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };
    int k;

    DCHECK(g_sel_class == 0, "selection_init ran twice — §3's interface and its member declarations are made "
                             "once per AGENT, and a second declaration re-mints the class every Selection of "
                             "the first agent is already branded with");
    /* §3's `addRange(Range range)` and `removeRange(Range range)` brand against §5.5's CLASS, so that class has
       to exist by this line. It does, because core/platform.c's list declares `element` (whose init declares
       §5.5) before `document` (whose init reaches here) — asserted from this side rather than trusted, because
       a row moved in that list is a silent zero here and `idl_iface_brand(0)` is what would then be declared. */
    DCHECK(range_class_id() != 0,
           "§3's Selection was declared before DOM §5.5's Range — `addRange` and `removeRange` brand their "
           "argument against Range's class, and a brand against class 0 accepts nothing at all");
    JS_NewClassID(JS_GetRuntime(ctx), &g_sel_class);
    JS_NewClass(JS_GetRuntime(ctx), g_sel_class, &d);

    for (k = 0; k < SM_N; k++) g_id[k] = -1;
    g_id[SM_GET_RANGE_AT] = idl_method_id(ctx, ONE_ULONG, 1, js_sel_member, SM_GET_RANGE_AT);
    /* `undefined addRange(Range range)` / `undefined removeRange(Range range)` — the interface arm brands
       against §5.5's class, so a StaticRange or a plain object is a TypeError before step 1. */
    g_id[SM_ADD_RANGE] = idl_method_id(ctx, ONE_RANGE, 1, js_sel_member, SM_ADD_RANGE);
    idl_iface_brand(range_class_id());
    g_id[SM_REMOVE_RANGE] = idl_method_id(ctx, ONE_RANGE, 1, js_sel_member, SM_REMOVE_RANGE);
    idl_iface_brand(range_class_id());
    g_id[SM_REMOVE_ALL] = idl_method_id(ctx, NULL, 0, js_sel_member, SM_REMOVE_ALL);
    g_id[SM_COLLAPSE] = idl_method_id(ctx, NULLNODE_OFFSET, 2, js_sel_member, SM_COLLAPSE);
    idl_iface_brand(node_class_id());
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_ZERO, NULL);   /* §3.6 steps 15.4.1 and 16.1's `= 0` */
    g_id[SM_COLLAPSE_TO_START] = idl_method_id(ctx, NULL, 0, js_sel_member, SM_COLLAPSE_TO_START);
    g_id[SM_COLLAPSE_TO_END] = idl_method_id(ctx, NULL, 0, js_sel_member, SM_COLLAPSE_TO_END);
    g_id[SM_EXTEND] = idl_method_id(ctx, NODE_OFFSET, 2, js_sel_member, SM_EXTEND);
    idl_iface_brand(node_class_id());
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_ZERO, NULL);
    g_id[SM_SET_BASE_AND_EXTENT] = idl_method_id(ctx, FOUR_POINTS, 4, js_sel_member, SM_SET_BASE_AND_EXTENT);
    idl_iface_brand(node_class_id());
    g_id[SM_SELECT_ALL_CHILDREN] = idl_method_id(ctx, ONE_NODE, 1, js_sel_member, SM_SELECT_ALL_CHILDREN);
    idl_iface_brand(node_class_id());
    g_id[SM_CONTAINS_NODE] = idl_method_id(ctx, NODE_BOOL, 2, js_sel_member, SM_CONTAINS_NODE);
    idl_iface_brand(node_class_id());
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_FALSE, NULL);   /* §3.6 steps 15.4.1 and 16.1's `= false` */
    g_id[SM_COMPOSED_RANGES] = idl_method_id_dict(ctx, ONE_DICT, 1, GET_COMPOSED_RANGES_OPTIONS,
                                                  (int)(sizeof(GET_COMPOSED_RANGES_OPTIONS) /
                                                        sizeof(GET_COMPOSED_RANGES_OPTIONS[0])),
                                                  js_sel_composed_ranges, SM_COMPOSED_RANGES);
    idl_optional_from(0);   /* `getComposedRanges(optional GetComposedRangesOptions options = {})` */
    /* `sequence<ShadowRoot>`'s ELEMENT TYPE — every node wrapper is one class, so the class says "a Node" and
       the narrowing says which kind. */
    idl_iface_brand(node_class_id());
    idl_iface_narrow(shadow_root_is_value);

    g_id_delete = idl_method_id_step(ctx, NULL, 0, NULL, 0, &SELECTION_DELETE, 0);
    g_id_stringifier = idl_method_id_step(ctx, NULL, 0, NULL, 0, &SELECTION_STRINGIFIER, 0);
    g_id_doc_get = idl_method_id(ctx, NULL, 0, js_document_get_selection, 0);
    g_id_win_get = idl_method_id(ctx, NULL, 0, js_window_get_selection, 0);

    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The class is the declaration
       latch this init consults, so a release that kept it would hand a second agent a class registered in a
       runtime that no longer exists; declaring it here is what makes a forgotten release fire instead of going
       silent. The two entry-point ids are declared for the same reason.
       DECLARED UNDER `document`, BECAUSE THE NAME IS THE ROW'S AND NOT THE FILE'S. core/platform.c's list has
       no `selection` row and must not grow one: nothing declares this component but document_init (which is
       also what makes the Range assert above true) and nothing releases it but document_agent_free, so
       `document` is the row whose release column this declaration is the inverse of. Declaring it under its
       own file's name did not make a smaller check, it made an ABSENT one — the row pairing can only ask
       "does anybody release this?" about a name a row carries — and it made `document`'s side of that pairing
       able to say "declared no agent state" about a component that had declared five slots. Beside
       core/dom/document_current_script.c's one slot, which names this same row for this same reason.
       AND THE STANDARD IS NAMED IN EACH `what`, because these are read out of a report headed `document`,
       where a bare `§3` would be DOM's rather than this one's.
       AND §3's TWELVE OPERATIONS ARE DECLARED TOO, WHICH THEY WERE NOT — the loop below, which is the reason
       SELECTION_OPERATIONS pairs each one with its own sentence. `selection_free` already put every entry of
       `g_id` back at -1, and nothing asserted that it had: the release column was the inverse of five of this
       component's seventeen slots, so agent_state_check_released held nothing to check for the other twelve
       and an entry a future release forgot would be read, by the next agent's `selection_install_proto`, as a
       pool entry of a pool that no longer exists. A count that is short is not a weaker check of those slots;
       it is no check of them at all, wearing the same number a component that held nothing would produce. */
    agent_state_class("document", &g_sel_class,
                      "Selection API §3's Selection class, and this component's declaration latch");
    for (k = 0; k < SM_N; k++) agent_state_id("document", &g_id[k], SM_WHAT[k]);
    agent_state_id("document", &g_id_delete, "Selection API §3's deleteFromDocument step-machine declaration");
    agent_state_id("document", &g_id_stringifier, "Selection API §3's stringifier step-machine declaration");
    agent_state_id("document", &g_id_doc_get, "Selection API §4.1's getSelection declaration");
    agent_state_id("document", &g_id_win_get, "Selection API §4.2's getSelection declaration");
    realm_declare_intrinsic(selection_install_proto);
}

void selection_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_sel_class != 0, "a realm asked for Selection.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_sel_class);
    DCHECK(JS_IsNull(prev), "selection_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Selection.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Selection");
    idl_install_accessor(ctx, proto, "anchorNode",   js_sel_get, SG_ANCHOR_NODE, -1);
    idl_install_accessor(ctx, proto, "anchorOffset", js_sel_get, SG_ANCHOR_OFFSET, -1);
    idl_install_accessor(ctx, proto, "focusNode",    js_sel_get, SG_FOCUS_NODE, -1);
    idl_install_accessor(ctx, proto, "focusOffset",  js_sel_get, SG_FOCUS_OFFSET, -1);
    idl_install_accessor(ctx, proto, "isCollapsed",  js_sel_get, SG_IS_COLLAPSED, -1);
    idl_install_accessor(ctx, proto, "rangeCount",   js_sel_get, SG_RANGE_COUNT, -1);
    idl_install_accessor(ctx, proto, "type",         js_sel_get, SG_TYPE, -1);
    idl_install_accessor(ctx, proto, "direction",    js_sel_get, SG_DIRECTION, -1);
    idl_install_method(ctx, proto, "getRangeAt", g_id[SM_GET_RANGE_AT]);
    idl_install_method(ctx, proto, "addRange", g_id[SM_ADD_RANGE]);
    idl_install_method(ctx, proto, "removeRange", g_id[SM_REMOVE_RANGE]);
    idl_install_method(ctx, proto, "removeAllRanges", g_id[SM_REMOVE_ALL]);
    /* §3: "empty() … must be an alias, and behave identically, to removeAllRanges()", and the same sentence
       for setPosition/collapse. An alias is the SAME declaration under a second name — a second body would be
       a second place for the algorithm to drift. */
    idl_install_method(ctx, proto, "empty", g_id[SM_REMOVE_ALL]);
    idl_install_method(ctx, proto, "getComposedRanges", g_id[SM_COMPOSED_RANGES]);
    idl_install_method(ctx, proto, "collapse", g_id[SM_COLLAPSE]);
    idl_install_method(ctx, proto, "setPosition", g_id[SM_COLLAPSE]);
    idl_install_method(ctx, proto, "collapseToStart", g_id[SM_COLLAPSE_TO_START]);
    idl_install_method(ctx, proto, "collapseToEnd", g_id[SM_COLLAPSE_TO_END]);
    idl_install_method(ctx, proto, "extend", g_id[SM_EXTEND]);
    idl_install_method(ctx, proto, "setBaseAndExtent", g_id[SM_SET_BASE_AND_EXTENT]);
    idl_install_method(ctx, proto, "selectAllChildren", g_id[SM_SELECT_ALL_CHILDREN]);
    idl_install_method(ctx, proto, "deleteFromDocument", g_id_delete);
    idl_install_method(ctx, proto, "containsNode", g_id[SM_CONTAINS_NODE]);
    /* §3's `stringifier;` — Web IDL §3.7.7 names the operation `toString`. */
    idl_install_method(ctx, proto, "toString", g_id_stringifier);
    /* §3's `modify()`. Its steps 1-9 are string comparisons this file could run; steps 10 and 11 are not — they
       set the focus (and the anchor) to "the location as if the user had requested to extend [move] selection
       by granularity", where granularity is one of character, word, sentence, line, paragraph and their
       boundaries. Every one past "character" is a TEXT SEGMENTATION and a LAYOUT question: a line is decided by
       line breaking over a rendered box, a word by UAX #29, and step 8's "resolved text direction at this
       selection's focus" is UAX #9 BD3's embedding direction of the character at a position. This engine runs
       no layout, so a `modify` that moved by "line" would answer with a position it invented — the shape
       §NO STUBS refuses. ABSENT is the honest state and the page's own TypeError is the forcing function; what
       to build first is the UAX #29 segmenter that "word" and "sentence" need, since those two are decidable
       from the text alone and would leave only the box-shaped granularities on layout. */
    idl_members_excluded(ctx, proto, "Selection", SELECTION_ABSENT,
                         (int)(sizeof(SELECTION_ABSENT) / sizeof(SELECTION_ABSENT[0])),
                         "Selection API §3 modify() steps 10-11 set the selection's focus to \"the location as "
                         "if the user had requested to extend selection by granularity\", and every "
                         "granularity but \"character\" is decided by text segmentation and line layout this "
                         "engine does not run");
    JS_SetClassProto(ctx, g_sel_class, proto);
}

void selection_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_sel_class);

    DCHECK(!JS_IsNull(proto), "Selection was installed in a realm that never ran its prototype install");
    /* §3 declares no constructor: the interface object exists to be what `instanceof` names. */
    idl_define_global_property_reference(ctx, global, "Selection", idl_interface_object(ctx, "Selection", proto));
    JS_FreeValue(ctx, proto);
}

void selection_install_document_members(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_doc_get >= 0, "§4.1's getSelection was installed before selection_init ran");
    idl_install_method(ctx, proto, "getSelection", g_id_doc_get);
}

void selection_install_window_members(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_id_win_get >= 0, "§4.2's getSelection was installed before selection_init ran");
    idl_install_method(ctx, global, "getSelection", g_id_win_get);
}

/* The PROTOTYPES are the realms' — each is in its own class-proto slot and released with its context. What
   this component holds for the AGENT is the class and its member declarations, and both go: `selection_init`
   consults the class to decide whether it has anything to do, so leaving it set would hand a second agent a
   class registered in a runtime that no longer exists.
   AND THE COLLECTION THAT FINALIZES THE PAGE'S SELECTIONS RUNS AFTER THIS, which is why `sel_finalizer` and
   `sel_gc_mark` read neither the class nor any other static of this file — see the paragraph above them. */
void selection_free(void)
{
    DCHECK(g_sel_class != 0, "§3's Selection was released in an agent that never declared it");
    /* THE SLOTS ARE NOT ENUMERATED HERE ANY MORE — §3's class and its member and stringifier declarations,
       plus §4.1's and §4.2's two getSelection entries. They are declared under `document`, because this is a
       sub-component of that row and document_agent_free is the release that reaches it, and that release's
       last line undoes the whole row from the registry that already holds every slot's address and kind. See
       core/agent_state.h's agent_state_undo for why the list that stood here was a second copy of this file's
       own agent_state_class/_id calls rather than their inverse — this file's comment below already records
       what it cost when the two came apart.
       WHAT THE LIST ARGUED IS UNCHANGED, and only WHEN moves. The CLASS still goes back to 0 before the next
       agent — it is registered in a RUNTIME and it is this component's latch — and the pool entries still go
       back to -1. Nothing between this line and document_agent_free's last reads any of them: the release
       frees nothing, so no null here guards a free, and §5.3's abstract_range_of, which this file's
       sel_bounds reaches, is `element`'s state and is released by a row that runs AFTER this one. */
}
