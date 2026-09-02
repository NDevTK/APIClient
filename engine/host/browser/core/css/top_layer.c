/* THE TOP LAYER — CSS Positioned Layout Level 4 §3 "Top Layer", §3.3 "Top Layer Manipulation". See top_layer.h
 * for why there are two sets, why they are JS Arrays, and why they hang off the Document.
 *
 * WHAT §3.3 SAYS ABOUT STYLE, AND WHAT THIS BUILD CAN ANSWER ABOUT IT. Two of the three manipulation algorithms
 * end in a cascade edit — add says "at the UA !important cascade origin, add a rule targeting el containing an
 * `overlay: auto` declaration", request-removal says "remove the UA !important `overlay: auto` rule targeting
 * el" — and PROCESS TOP LAYER REMOVALS is the one step that READS the result: "if el's computed value of
 * overlay is none, or el is not rendered, remove from the top layer immediately el".
 *
 * The pair is a LATCH and not a style: the rule is added by exactly one algorithm and removed by exactly one
 * other, and a UA `!important` declaration wins the cascade over author `!important` (CSS Cascading and
 * Inheritance Level 5 §6.2 Cascade Sorting Order), so while it stands the computed value is `auto` whatever the
 * page writes. An element that has reached the pending set has therefore had the rule removed, and its computed
 * `overlay` is then whatever the AUTHOR says — which in this engine is nothing at all, because this cascade has
 * no `overlay` property to declare. That is not believed, it is ASKED, of the parser that owns the answer, at
 * the step that reads it: `lxb_css_property_by_name("overlay")` is NULL, and the day it is not, the DCHECK
 * fires AT the disjunct that must then consult the computed value instead of standing on this argument.
 *
 * SO THE LATCH IS NOT MODELLED AS A SEPARATE FLAG, and that is deliberate rather than a shortcut. A boolean
 * beside the two sets would be a THIRD piece of state saying exactly what membership of the two sets already
 * says — the rule is present iff the element is in the top layer and not pending removal, which is §3.3's own
 * "is in the top layer" — and a second copy of one fact is what drifts. When this engine grows the `overlay`
 * property the rule becomes a real UA cascade entry and this component adds and removes it there; the reading
 * side is the assert below, which is what will say so. */
#include <string.h>

#include <lexbor/css/css.h>
#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/top_layer.h"
#include "core/dom/node.h"

/* The two sets' slot keys — Symbols, so neither set is a string property of this engine's invention sitting on
   every Document where a page can see it, and interned atoms because that is what a slot read takes. */
static JSValue g_layer_key = JS_UNDEFINED;
static JSAtom  g_atom_layer = JS_ATOM_NULL;
static JSValue g_pending_key = JS_UNDEFINED;
static JSAtom  g_atom_pending = JS_ATOM_NULL;

void top_layer_declare(JSContext *ctx)
{
    DCHECK(JS_IsUndefined(g_layer_key), "top_layer_declare ran twice — one instance is one agent, and the two "
                                        "slot keys identify the same two sets in every realm");
    g_layer_key = JS_NewSymbol(ctx, "documentTopLayer", false);
    CHECK(!JS_IsException(g_layer_key), "the top-layer slot key allocation failed");
    g_atom_layer = JS_ValueToAtom(ctx, g_layer_key);
    CHECK(g_atom_layer != JS_ATOM_NULL, "the top-layer slot key could not be interned");
    g_pending_key = JS_NewSymbol(ctx, "documentPendingTopLayerRemovals", false);
    CHECK(!JS_IsException(g_pending_key), "the pending-top-layer-removals slot key allocation failed");
    g_atom_pending = JS_ValueToAtom(ctx, g_pending_key);
    CHECK(g_atom_pending != JS_ATOM_NULL, "the pending-top-layer-removals slot key could not be interned");
}

void top_layer_free(JSRuntime *rt)
{
    /* The slot keys are the AGENT's — a Symbol nobody frees is a live GC object the runtime's own walk counts
       as a leak. The SETS are the documents' and are released with their wrappers. */
    JS_FreeAtomRT(rt, g_atom_layer);
    g_atom_layer = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_layer_key);
    g_layer_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_pending);
    g_atom_pending = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_pending_key);
    g_pending_key = JS_UNDEFINED;
}

/* "Let doc be el's node document" — as the Document's WRAPPER, which is what the two sets hang off. OWNED.
   JS_NULL when the element has no owner document, which no element in a live tree is. */
static JSValue tl_document_of(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "a §3.3 Top Layer Manipulation algorithm was given something that is not an element — every caller "
           "reaches it holding an Element, which is the type the standard declares for `el`");
    if (!n || !n->owner_document) return JS_NULL;
    return node_wrap(ctx, lxb_dom_interface_node(n->owner_document));
}

/* One of the two ordered sets on `doc`, MINTED only where the caller is about to APPEND to it. OWNED, and
   JS_UNDEFINED for the document that has never had one — which every read below treats as the empty set,
   because an absent slot IS "initially empty" and tl_len answers 0 for anything that is not an Array.
   `mint` IS NOT AN OPTIMISATION AND THE DEFAULT IS `false`. A mint is a PROPERTY WRITE on the Document, so the
   heap COW delta captures it: a flow that merely ASKS a question about the top layer would otherwise leave a
   delta entry on shared baseline state it never changed, and `document.fullscreenElement` — a getter a page
   reads freely, on every document, with nothing in any top layer — is the reader that makes that a real cost
   rather than a tidiness. So the three §3.3 Top Layer Manipulation algorithms mint the ONE set each of them
   appends to, and every other reach here is a read. */
static JSValue tl_set(JSContext *ctx, JSValueConst doc, JSAtom key, bool mint)
{
    JSValue set;

    DCHECK(g_atom_layer != JS_ATOM_NULL, "a top-layer set was reached before top_layer_declare minted its keys");
    if (JS_IsNull(doc) || JS_IsUndefined(doc)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &set, doc, key) > 0) {
        DCHECK(JS_IsArray(set), "a top-layer slot held something that is not an Array — the only writer is the "
                                "mint below, and nothing else can name the Symbol it hangs off");
        return set;
    }
    if (!mint) return JS_UNDEFINED;
    set = JS_NewArray(ctx);
    CHECK(!JS_IsException(set), "a top-layer ordered set could not be allocated");
    /* CONFIGURABLE AND WRITABLE for the reason html_dialog.c's return-value slot is: a slot defined with no
       flags makes every later write a silent no-op, and the ARRAY OBJECT is what later reads must find. */
    JS_DefinePropertyValue(ctx, (JSValue)doc, key, JS_DupValue(ctx, set),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return set;
}

static uint32_t tl_len(JSContext *ctx, JSValueConst set)
{
    JSValue lv;
    uint32_t n = 0;

    if (!JS_IsArray(set)) return 0;
    lv = JS_GetPropertyStr(ctx, set, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* The INDEX of `el` in an ordered set, or -1. An ordered set holds each element at most once (§3.3's add
   removes before it appends), so the first hit is the only one. Identity, not equality: these are wrappers, and
   node.c guarantees one wrapper per node. */
static int64_t tl_index_of(JSContext *ctx, JSValueConst set, JSValueConst el)
{
    uint32_t i, n = tl_len(ctx, set);

    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, set, i);
        bool same = JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(el);

        JS_FreeValue(ctx, v);
        if (same) return (int64_t)i;
    }
    return -1;
}

/* REMOVE FROM AN ORDERED SET — the operation the whole component is shaped around, because it removes from the
   MIDDLE and the ORDER of what remains is what §3 renders and what §6.12's popover stack walks. Written as a
   shift plus a `length` write rather than through Array.prototype.splice: splice is the PAGE's property, which
   a page may replace, and reaching for it would put the page's code inside a C activation with no flow base. */
static void tl_remove(JSContext *ctx, JSValueConst set, JSValueConst el)
{
    int64_t at = tl_index_of(ctx, set, el);
    uint32_t i, n;

    if (at < 0) return;
    n = tl_len(ctx, set);
    for (i = (uint32_t)at; i + 1 < n; i++)
        JS_SetPropertyUint32(ctx, (JSValue)set, i, JS_GetPropertyUint32(ctx, set, i + 1));
    JS_SetPropertyStr(ctx, (JSValue)set, "length", JS_NewUint32(ctx, n - 1));
}

static void tl_append(JSContext *ctx, JSValueConst set, JSValueConst el)
{
    JS_SetPropertyUint32(ctx, (JSValue)set, tl_len(ctx, set), JS_DupValue(ctx, el));
}

/* §3.3: "To ADD AN ELEMENT TO THE TOP LAYER, given an Element el." */
void top_layer_add(JSContext *ctx, JSValueConst el)
{
    JSValue doc = tl_document_of(ctx, el);                                     /* step 1 */
    JSValue layer = tl_set(ctx, doc, g_atom_layer, true);
    JSValue pending = tl_set(ctx, doc, g_atom_pending, false);

    /* Step 2: "if el is already contained in doc's top layer" — with its own assert, which the standard writes
       as an assert and says why: "(Otherwise, this is a spec error.)" */
    if (tl_index_of(ctx, layer, el) >= 0) {
        DCHECK(tl_index_of(ctx, pending, el) >= 0,
               "CSS Positioned Layout Level 4 §3.3 Top Layer Manipulation's add-an-element-to-the-top-layer "
               "step 2.1 asserts that an element already in the top layer is also in pending top layer "
               "removals — a caller that adds an element which is in the layer and NOT pending removal is the "
               "spec error the standard names there, and appending it again would put one element in an "
               "ordered set twice");
        tl_remove(ctx, layer, el);                                             /* step 2.2 */
        tl_remove(ctx, pending, el);
    }
    tl_append(ctx, layer, el);                                                 /* step 3 */
    /* Step 4 is the UA !important `overlay: auto` rule. It is the latch this component's header describes: its
       one observable effect is on step 1 of process-top-layer-removals below, which asks it there of the
       parser that owns the answer rather than of a second copy of this membership. */
    JS_FreeValue(ctx, pending);
    JS_FreeValue(ctx, layer);
    JS_FreeValue(ctx, doc);
}

/* §3.3: "To REQUEST AN ELEMENT TO BE REMOVED FROM THE TOP LAYER, given an Element el." */
void top_layer_request_removal(JSContext *ctx, JSValueConst el)
{
    JSValue doc = tl_document_of(ctx, el);                                     /* step 1 */
    JSValue layer = tl_set(ctx, doc, g_atom_layer, false);
    JSValue pending = tl_set(ctx, doc, g_atom_pending, true);

    /* Step 2 — the two ways this is a no-op, both stated over the sets. */
    if (tl_index_of(ctx, layer, el) >= 0 && tl_index_of(ctx, pending, el) < 0) {
        /* Step 3 removes the UA rule; step 4 appends. See step 4 of add above for where the rule is read. */
        tl_append(ctx, pending, el);                                           /* step 4 */
    }
    JS_FreeValue(ctx, pending);
    JS_FreeValue(ctx, layer);
    JS_FreeValue(ctx, doc);
}

/* §3.3: "To REMOVE AN ELEMENT FROM THE TOP LAYER IMMEDIATELY, given an Element el." Its note names the case it
   is for — "a modal dialog that is removed from the document" — and HTML §6.12's hide-a-popover takes it for
   every popover that was never in the auto or hint stack. */
void top_layer_remove_immediately(JSContext *ctx, JSValueConst el)
{
    JSValue doc = tl_document_of(ctx, el);                                     /* step 1 */
    JSValue layer = tl_set(ctx, doc, g_atom_layer, false);
    JSValue pending = tl_set(ctx, doc, g_atom_pending, false);

    tl_remove(ctx, layer, el);                                                 /* step 2 */
    tl_remove(ctx, pending, el);
    /* Step 3 removes the UA rule "if it exists" — the latch again, read at the one step that reads it. */
    JS_FreeValue(ctx, pending);
    JS_FreeValue(ctx, layer);
    JS_FreeValue(ctx, doc);
}

/* §3.3: "An element el is in the top layer if el is contained in its node document's top layer but not
   contained in its node document's pending top layer removals." BOTH sets are read, which is the whole of the
   term — see top_layer.h for why the raw one-set predicate that stood here was the wrong question for its one
   caller, and for the WPT document that settles it.
   NEITHER SET IS MINTED. This is a QUESTION about a document, so an absent layer answers false rather than
   leaving a property write on shared baseline state behind it — the same rule top_layer_collect states. */
bool top_layer_is_in(JSContext *ctx, JSValueConst el)
{
    JSValue doc = tl_document_of(ctx, el);
    JSValue layer = tl_set(ctx, doc, g_atom_layer, false);
    JSValue pending = tl_set(ctx, doc, g_atom_pending, false);
    bool in = tl_index_of(ctx, layer, el) >= 0 && tl_index_of(ctx, pending, el) < 0;

    JS_FreeValue(ctx, pending);
    JS_FreeValue(ctx, layer);
    JS_FreeValue(ctx, doc);
    return in;
}

/* §3's ORDER, READ — see top_layer.h for why the walk lives here, why "topmost" is the LAST member, why it
   reads the `top layer` rather than §3.3's "is in the top layer", and why it answers with the member and never
   with its rank. */
JSValue top_layer_topmost(JSContext *ctx, JSValueConst document, TopLayerPredicate pred, void *opaque)
{
    JSValue layer = tl_set(ctx, document, g_atom_layer, false);
    uint32_t n = tl_len(ctx, layer);
    JSValue found = JS_NULL;
    uint32_t i;

    DCHECK(pred != NULL,
           "CSS Positioned Layout Level 4 §3 Top Layer's ordered read was given no predicate — every caller is "
           "asking for the topmost element FOR WHICH SOMETHING HOLDS (Fullscreen §2 Model's fullscreen element "
           "asks whose fullscreen flag is set), and a null predicate would answer the last member of the layer "
           "to a caller that asked a different question");
    /* BACKWARDS, because §3's last member is the topmost one — so the first hit is the answer and the walk
       stops, rather than running the whole set to keep the highest index that matched. */
    for (i = n; i > 0; i--) {
        JSValue el = JS_GetPropertyUint32(ctx, layer, i - 1);

        if (pred(ctx, el, opaque)) {
            found = el;
            break;
        }
        JS_FreeValue(ctx, el);
    }
    DCHECK(tl_len(ctx, layer) == n,
           "a §3 Top Layer ordered-read predicate CHANGED THE SET IT WAS BEING WALKED OVER — the contract "
           "top_layer.h states is that a predicate is a C question about one element that runs no page code and "
           "reaches no §3.3 Top Layer Manipulation algorithm, and a walk whose set moves under it answers with "
           "an element at a position that no longer means what the walk read it for");
    JS_FreeValue(ctx, layer);
    return found;
}

/* §3's ORDER, READ AS A SEQUENCE — see top_layer.h for why this lives here rather than at the caller that
   builds one of the standard's derived lists out of it, why it runs FORWARDS where the walk above runs
   backwards, and why a snapshot the caller owns is not the rank that walk refuses to give. */
JSValue top_layer_collect(JSContext *ctx, JSValueConst document, TopLayerPredicate pred, void *opaque)
{
    JSValue layer = tl_set(ctx, document, g_atom_layer, false);
    uint32_t n = tl_len(ctx, layer);
    JSValue out = JS_NewArray(ctx);
    uint32_t i, k = 0;

    CHECK(!JS_IsException(out), "a §3 Top Layer ordered-read snapshot could not be allocated");
    DCHECK(pred != NULL,
           "CSS Positioned Layout Level 4 §3 Top Layer's ordered sequence read was given no predicate — every "
           "caller is building one of the standard's DERIVED ordered lists (HTML §6.12 The popover attribute's "
           "showing auto popover list appends only the members whose opened in popover mode is \"auto\"), and a "
           "null predicate would hand that caller the whole top layer under the name of its filtered list");
    /* FORWARDS: the list being built carries §3's order, so the appends happen in it. */
    for (i = 0; i < n; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, layer, i);

        if (pred(ctx, el, opaque))
            JS_SetPropertyUint32(ctx, out, k++, JS_DupValue(ctx, el));
        JS_FreeValue(ctx, el);
    }
    DCHECK(tl_len(ctx, layer) == n,
           "a §3 Top Layer ordered-read predicate CHANGED THE SET IT WAS BEING WALKED OVER — the contract "
           "top_layer.h states is that a predicate is a C question about one element that runs no page code and "
           "reaches no §3.3 Top Layer Manipulation algorithm, and a collect whose set moves under it answers "
           "with a list that is neither the set before nor the set after");
    JS_FreeValue(ctx, layer);
    return out;
}

/* §3.3: "To PROCESS TOP LAYER REMOVALS, given a Document doc." One step, and it is a filtered drain:
   "for each element el in doc's pending top layer removals: if el's computed value of overlay is none, or el is
   not rendered, remove from the top layer immediately el."
   THE WALK IS OVER A SNAPSHOT OF THE INDICES because the body mutates BOTH sets — remove-immediately takes el
   out of `pending` as well as out of the layer — so it runs backwards, which is the one direction in which a
   removal cannot move an element the walk has not reached yet. */
void top_layer_process_removals(JSContext *ctx, JSValueConst document)
{
    JSValue pending = tl_set(ctx, document, g_atom_pending, false);
    uint32_t n = tl_len(ctx, pending);
    uint32_t i;

    DCHECK(lxb_css_property_by_name((const lxb_char_t *)"overlay", 7) == NULL,
           "CSS Positioned Layout Level 4 §3.3 Top Layer Manipulation's process-top-layer-removals step 1 reads "
           "el's COMPUTED VALUE OF OVERLAY, and this build answered that disjunct from the UA !important latch "
           "alone because the cascade had no `overlay` property for an author to declare. It has one now: the "
           "computed value must be read for each pending element, and §3.4's `overlay` (an author transition "
           "can hold a removal open arbitrarily long) is what decides whether it leaves the layer this "
           "rendering update");
    for (i = n; i > 0; i--) {
        JSValue el = JS_GetPropertyUint32(ctx, pending, i - 1);

        /* The first disjunct, which the assert above is about: with no `overlay` property in this cascade the
           only writer of that computed value is the UA rule, and an element in `pending` has had it removed —
           so the computed value is the property's initial `none` and the disjunction short-circuits before the
           `is not rendered` half. That is the operator, not a skipped step. */
        top_layer_remove_immediately(ctx, el);
        JS_FreeValue(ctx, el);
    }
    JS_FreeValue(ctx, pending);
}
