/* RESIZE OBSERVER — §2.1's interface and the whole of §3's processing model. See resize_observer.h for why the
 * state is JS values, why §3.4.5 is split at the callback, and why this is the mirror image of the observer
 * next door. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/element_view.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/events/error_event.h"
#include "core/events/event_target.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/geometry/dom_rect.h"
#include "core/idl_args.h"
#include "core/layout/box_subject.h"
#include "core/layout/used_value.h"
#include "core/realm.h"
#include "core/resize_observer/resize_observer.h"
#include "core/resize_observer/resize_observer_entry.h"
#include "core/resize_observer/resize_observer_size.h"

/* ---- the shapes -------------------------------------------------------------------------------------------
 *
 * §3.2.2's FOUR INTERNAL SLOTS, as one Array in an own slot on the observer. `RO_S_WINDOW` is not one of
 * §3.2.2's and is not state either: it is the observer's own REALM, recorded as the WindowProxy the realm is
 * reachable through, so that §3.4.4's [NewObject]s are minted where the page that will read them lives. §2.1's
 * constructor adds `this` to the CONSTRUCTING document's [[resizeObservers]], so the walking realm and this one
 * are the same realm — which is a DERIVATION and is asserted at the walk rather than assumed, because it is
 * exactly the thing that is not true of the observer next door (an implicit-root IntersectionObserver
 * constructed in an iframe lands on the TOP-LEVEL document's list).
 *
 * §3.1's ResizeObservation is an Array of four, held in the observer's own [[observationTargets]].
 * `ROB_LAST_INLINE` / `ROB_LAST_BLOCK` are §3.1's `lastReportedSizes`, and they are NUMBERS rather than
 * ResizeObserverSize objects for one reason: `isActive()` COMPARES them, in C, and must not fork. Every length
 * this component computes is a `CssPx` — an example plus the environment facts it is a joint function of
 * (core/css/css_length.h) — and it crosses core/frame/viewport.h's ONE seam into a page-visible value exactly
 * where §3.4.8 mints a ResizeObserverSize. Storing the crossed value here and reading it back would mean
 * comparing two values that may be unknown-derived, which is a fork this algorithm has no seam to ask at; the
 * EXAMPLE is what `isActive()` is a predicate over, and §3.4.5 step 2.3.3 writes the same two numbers §3.4.8
 * computed for the entry, out of the one call that computed them. */
enum { RO_S_CALLBACK = 0, RO_S_TARGETS, RO_S_ACTIVE, RO_S_SKIPPED, RO_S_WINDOW, RO_S_COUNT };
enum { ROB_TARGET = 0, ROB_BOX, ROB_LAST_INLINE, ROB_LAST_BLOCK, ROB_COUNT };

/* §2.1's `enum ResizeObserverBoxOptions`, IN THE IDL'S OWN ORDER — the array index IS RoBoxOption, which is
   what lets the entry's matching-sizes table (resize_observer_entry.c) be written over the same numbering. */
IDL_ENUM_VALUES(RO_BOX_VALUES, "border-box", "content-box", "device-pixel-content-box");

static JSClassID g_class;
static JSValue   g_state_key = JS_UNDEFINED;    /* the observer's own state slot */
static JSAtom    g_atom_state = JS_ATOM_NULL;
static JSAtom    g_atom_depth = JS_ATOM_NULL;   /* §3.4.5 step 1's shallowestTargetDepth, on the realm's list */
static int       g_docobs_slot = -1;            /* this realm's §3.2.1 [[resizeObservers]] */
static int       g_id_ctor = -1, g_id_observe = -1, g_id_unobserve = -1, g_id_disconnect = -1;
static int       g_ready;

/* ---- small list helpers ----------------------------------------------------------------------------------- */

static uint32_t ro_len(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = 0;
    JSValue v = JS_GetPropertyStr(ctx, (JSValue)arr, "length");

    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void ro_push(JSContext *ctx, JSValueConst arr, JSValue v)
{
    JS_SetPropertyUint32(ctx, (JSValue)arr, ro_len(ctx, arr), v);
}

static void ro_set_len(JSContext *ctx, JSValueConst arr, uint32_t n)
{
    JS_SetPropertyStr(ctx, (JSValue)arr, "length", JS_NewUint32(ctx, n));
}

static double ro_num_at(JSContext *ctx, JSValueConst arr, uint32_t i)
{
    JSValue v = JS_GetPropertyUint32(ctx, (JSValue)arr, i);
    double d = 0.0;

    DCHECK(JS_IsNumber(v),
           "an engine-built §3.1 ResizeObservation held something that is not a Number where one belongs — "
           "every writer of these two slots is in this file and every one of them writes an example length");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

/* THE OBSERVER'S OWN STATE. OWNED. */
static JSValue ro_state(JSContext *ctx, JSValueConst obj)
{
    JSValue s;

    if (JS_GetClassID(obj) != g_class)
        return JS_ThrowTypeError(ctx, "not a ResizeObserver");
    if (JS_GetOwnSlot(ctx, &s, obj, g_atom_state) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsObject(s),
           "a ResizeObserver carries no state — §2.1's constructor writes every slot before the object exists, "
           "so one without them was made somewhere else");
    return s;
}

/* THE OBSERVER'S OWN REALM — see the shapes comment. */
static JSContext *ro_realm(JSContext *ctx, JSValueConst state)
{
    JSValue proxy = JS_GetPropertyUint32(ctx, (JSValue)state, RO_S_WINDOW);
    JSContext *rctx;

    DCHECK(window_proxy_is(proxy),
           "a ResizeObserver's recorded global is not a WindowProxy — §2.1's constructor records the "
           "constructing realm's and nothing else writes the slot");
    rctx = window_proxy_realm(ctx, proxy);
    JS_FreeValue(ctx, proxy);
    DCHECK(rctx != NULL,
           "a ResizeObserver's realm is gone while the observer is still on a document's [[resizeObservers]] "
           "list — §3.5 keeps an observer alive while it is observing, and a released realm has no targets "
           "left to observe");
    return rctx;
}

/* ---- §3.2.1: the document's [[resizeObservers]] ------------------------------------------------------------ */

/* THIS REALM'S OBSERVER LIST. OWNED. §3.2.1: "Document has a [[resizeObservers]] slot that is a list of
   ResizeObservers in this document. It is initialized to empty." It is per REALM because a Document is, and
   because a member installed once would otherwise answer every document's question out of the realm that
   defined it (CLAUDE.md §A-PER-REALM-FACT-IS-ANSWERED-PER-REALM). */
static JSValue ro_doc_list(JSContext *ctx)
{
    JSValue arr = realm_value_get(ctx, g_docobs_slot);

    DCHECK(JS_IsArray(arr),
           "a realm was asked for its §3.2.1 [[resizeObservers]] before resize_observer_install_proto built "
           "one — the list is a per-realm intrinsic and every realm goes through that one install");
    return arr;
}

/* ---- §3.4.7 "Calculate depth for node" --------------------------------------------------------------------- */

/* "Let p be the parent-traversal path from node to a root Element of this element’s flattened DOM tree.
 * Return number of nodes in p."
 *
 * THE PATH IS OVER THE FLATTENED TREE AND NOT OVER THE NODE TREE, which is the whole reason this is its own
 * function. DOM §4.2.2 "Slots" makes a slotted child's flat-tree parent its ASSIGNED SLOT and a shadow root's
 * flat-tree parent its HOST, so an element distributed into a component's shadow tree is DEEPER than its
 * light-tree position says — and step 16's loop uses this number to decide which observations may broadcast on
 * this turn, so counting the wrong tree would let a component's own observation and its slotted content's
 * broadcast in the same turn and re-enter layout with a stale gather.
 *
 * THE ROOT ELEMENT COUNTS ZERO. The path from a node to a root element is the run of nodes ABOVE it, so the
 * root element's own path is empty; every browser's implementation of this step counts ancestors for the same
 * reading, and the number is only ever compared against another number produced here. */
static int32_t ro_depth(JSContext *ctx, lxb_dom_node_t *n)
{
    int32_t depth = 0;

    for (;;) {
        lxb_dom_node_t *slot = slot_assigned_slot(ctx, n), *p;

        if (slot != NULL) {
            n = slot;
        } else {
            p = n->parent;
            if (p != NULL && shadow_root_is(p))
                p = lxb_dom_interface_node(shadow_root_host(p));
            if (p == NULL || p->type != LXB_DOM_NODE_TYPE_ELEMENT)
                return depth;
            n = p;
        }
        depth++;
        DCHECK(depth >= 0, "§3.4.7's parent-traversal path over the flattened tree overflowed a signed count — "
                           "the flattened tree is a TREE, so the walk cannot revisit a node and a path longer "
                           "than two billion nodes is a cycle rather than a document");
    }
}

/* ---- §3.4.8 "Calculate box size, given target and observed box" -------------------------------------------- */

/* §3.4.8's INLINE AND BLOCK AXES, MAPPED ONTO THE PHYSICAL ONES. `vertical` false is the horizontal axis.
 *
 * ONE PROPERTY DECIDES EACH AXIS HERE, and that is a fact about `horizontal-tb` rather than a general rule —
 * the same conjunction core/layout/scrolling_area.c asserts at the same mapping, and for the same reason:
 * css-writing-modes-4 §6.2 "Flow-relative Directions" states that determining a box's block-start and
 * block-end sides depends only on `writing-mode`, while its inline sides depend on `direction` as well, and
 * neither axis is the one this maps it to in a vertical mode. The element's own box was placed by
 * core/layout/flow_position.c, which crashes for exactly that value before any extent exists to measure. */
static bool ro_axis_is_vertical(lxb_dom_element_t *el, bool block)
{
    char *wm = css_computed_value(el, "writing-mode");
    bool horizontal_tb;
    char nbuf[160], vbuf[64];

    DCHECK(wm != NULL, "the cascade produced no computed `writing-mode` for an element — every property this "
                       "engine asks for has an initial value in the UA sheet");
    horizontal_tb = strcmp(wm, "horizontal-tb") == 0;
    free(wm);
    DCHECKF(horizontal_tb,
            "%s, computed `writing-mode` `%s`: "
            "RESIZE OBSERVER §3.4.8's inline and block lengths were mapped onto the physical axes for a box "
            "whose computed `writing-mode` is not `horizontal-tb`, where the block axis is not the vertical "
            "one — css-writing-modes-4 §3.2 \"Block Flow Direction: the writing-mode property\" is what "
            "decides it. core/layout/flow_position.c crashes for that value before this box has a position at "
            "all, so an element that has one and this writing mode means the two tests have come apart",
            box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "writing-mode", vbuf, sizeof vbuf));
    return block;
}

/* §3.4.8's ONE LENGTH on one axis, for one of §2.1's three boxes.
 *
 * THE SVG ARM IS NOT REACHABLE IN THIS BUILD AND IS NOT SILENTLY FOLDED INTO THE OTHER ONE. §3.4.8's first arm
 * is "If target is an SVGGraphicsElement that does not have an associated CSS layout box", and this engine
 * declares no SVG interface at all — `SVGGraphicsElement` is on no realm's global — so every target that can
 * reach here is §3.4.8's "Otherwise", which is the arm written below. The day an SVG element exists, the box
 * this reads is the wrong one for it, and the assert is what says so at the line rather than in prose.
 *
 * A TARGET WITH NO BOX HAS AN EMPTY SIZE, AND THAT IS §3.3.1's OWN ANSWER RATHER THAN A DEFAULT. That section
 * lists the consequences of watching a content rect and two of them are exactly this case — "observation will
 * fire when watched Element is inserted/removed from DOM" and "observation will fire when watched Element
 * display gets set to none" — which are observable only because the size a box-less target reports is zero
 * rather than nothing. The third, "non-replaced inline Elements will always have an empty content rect", is
 * the second arm: core/dom/element_view.h's fragment kind is where this engine answers CSS 2.2 §9.2.2's
 * non-replaced-inline conjunction, so the two questions are one answer and not two. */
static CssPx ro_box_length(JSContext *rctx, lxb_dom_element_t *el, RoBoxOption box, bool block)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    bool vertical;
    CssPx len;

    if (!element_view_has_box(n) || element_view_fragment_kind(el) == ELEMENT_VIEW_FRAGMENTS_LINE_BOXES)
        return css_px(0.0);
    vertical = ro_axis_is_vertical(el, block);
    switch (box) {
    case RO_BOX_BORDER:
        /* The two steps this arm performs set the size's attributes from "target’s border area inline length"
           and its block length — CSS 2.2 §8.1 "Box dimensions"' border edge on that axis, which
           core/layout/used_value.h owns for every consumer. */
        return used_value_border_edge_px(el, vertical);
    case RO_BOX_CONTENT:
        /* …and this arm's from "target’s content area inline length" and its block length. §8.1's CONTENT box
           is NOT always what `used_value_px` answers — css-sizing §5 makes the used value refer to the border
           box under `box-sizing: border-box` — so used_value.h runs that conversion once, in the one place
           that owns its four terms, rather than each caller subtracting for itself. */
        return used_value_content_px(el, vertical);
    case RO_BOX_DEVICE_PIXEL_CONTENT:
        /* …and this arm's from the same content lengths "in integral device pixels". RESIZE OBSERVER §2.1
           says of this box that it "must contain integer values" and that "How a UA computes the device
           pixel box for an element is implementation-dependent", offering as one possible implementation "to
           multiply the box size and position by the device pixel ratio, then round both the resulting
           floating-point size and position of the box to integer values". That is what this does.
           THE RATIO IS AN ENVIRONMENT FACT AND THE PRODUCT CARRIES IT, which is why this is `css_px_mul` of
           two lengths and not `css_px_scale` by a bare double: that entry's own contract is that `k` is "a
           pure ratio — a percentage divided by 100 — so it changes no fact", and the device pixel ratio is
           the opposite of that. It is core/css/css_length.h's `CSS_ENV_DEVICE_PIXEL_RATIO`, one of the facts
           core/frame/viewport.h has decided is PICKED rather than derived, so a page's `devicePixelRatio > 1`
           retina gate and this number are one question and both arms of it stay reachable; scaling by a bare
           double would have deleted the arm the other display takes.
           THE ROUNDING MOVES THE EXAMPLE AND NOT THE PROVENANCE, deliberately: the rounded number is still a
           joint function of the content extent and the ratio, so the fact set is what it was and only the
           example is snapped to the integer RESIZE OBSERVER §2.1 requires. */
        len = css_px_mul(used_value_content_px(el, vertical),
                         css_px_env(CSS_ENV_DEVICE_PIXEL_RATIO, rctx, viewport_device_pixel_ratio(rctx)));
        len.px = floor(len.px + 0.5);
        return len;
    case RO_BOX_COUNT:
        break;
    }
    DFAIL("§3.4.8 was asked for a box §2.1's `enum ResizeObserverBoxOptions` does not declare — an "
          "observation's observed box is written by observe(), whose IDL enumeration check refuses every "
          "other keyword, and by §3.4.4, which passes the three the IDL lists");
    return css_px(0.0);
}

/* THE ONE SEAM. A length crosses core/frame/viewport.h's boundary into a page-visible number exactly here and
   in §3.4.4's content rect, which is what carries the environment facts it is a joint function of into the
   value a bundle then branches on — `if (entry.contentRect.width < 768)` is the same question `innerWidth <
   768` is, and a bare `JS_NewFloat64` at either site would delete the arm the other viewport takes. */
static JSValue ro_length_value(JSContext *rctx, CssPx len)
{
    return viewport_env_derived(len, JS_NewFloat64(rctx, len.px));
}

/* §3.4.8's WHOLE RESULT as a page-visible ResizeObserverSize, minted in `rctx` — the observer's realm, because
   a [NewObject] belongs to the realm whose interface produced it. The two lengths cross core/frame/viewport.h's
   ONE seam here and nowhere else in this component, which is what carries each one's domain into the value the
   page reads and forks on. `out_*` receive the examples §3.1's `isActive()` compares, so the comparison and the
   value the page sees come out of ONE computation. */
static JSValue ro_box_size(JSContext *rctx, lxb_dom_element_t *el, RoBoxOption box,
                           CssPx *out_inline, CssPx *out_block)
{
    CssPx i = ro_box_length(rctx, el, box, /*block*/ false), b = ro_box_length(rctx, el, box, /*block*/ true);

    *out_inline = i;
    *out_block = b;
    return resize_observer_size_new(rctx, ro_length_value(rctx, i), ro_length_value(rctx, b));
}

/* ---- §3.1's `isActive()` ----------------------------------------------------------------------------------- */

/* "Set currentSize by calculate box size given target and observedBox. Return true if currentSize is not equal
 * to the first entry in this.lastReportedSizes. Return false."
 *
 * IT COMPARES EXAMPLES AND NOT THE CROSSED VALUES — see the shapes comment. The initial pair §3.1 writes is
 * (−1, −1), which no box extent can equal, so a target's FIRST gather always makes it active and the first
 * frame after `observe()` always delivers. That is the observable behaviour every bundle relies on: a
 * ResizeObserver fires once as soon as it starts observing, before anything has resized. */
static bool ro_observation_is_active(JSContext *ctx, JSValueConst obs)
{
    JSValue target = JS_GetPropertyUint32(ctx, (JSValue)obs, ROB_TARGET);
    lxb_dom_node_t *n = node_of(target);
    int32_t box;
    double now_i, now_b, was_i, was_b;

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "a §3.1 ResizeObservation's target is not an Element — §2.1's `observe(Element target)` is the only "
           "writer and Web IDL §3.2.15's brand plus the declaration's narrowing refuse everything else");
    box = (int32_t)ro_num_at(ctx, obs, ROB_BOX);
    DCHECK(box >= 0 && box < RO_BOX_COUNT, "a §3.1 ResizeObservation's observedBox is not one §2.1 declares");
    now_i = ro_box_length(ctx, lxb_dom_interface_element(n), (RoBoxOption)box, /*block*/ false).px;
    now_b = ro_box_length(ctx, lxb_dom_interface_element(n), (RoBoxOption)box, /*block*/ true).px;
    JS_FreeValue(ctx, target);
    was_i = ro_num_at(ctx, obs, ROB_LAST_INLINE);
    was_b = ro_num_at(ctx, obs, ROB_LAST_BLOCK);
    return now_i != was_i || now_b != was_b;
}

/* ---- §3.4.1, §3.4.2, §3.4.3 -------------------------------------------------------------------------------- */

void resize_observer_gather(JSContext *docctx, int32_t depth)
{
    JSValue list = ro_doc_list(docctx);
    uint32_t nobs = ro_len(docctx, list), i;

    for (i = 0; i < nobs; i++) {                                        /* step 2 */
        JSValue observer = JS_GetPropertyUint32(docctx, list, i);
        JSValue state = ro_state(docctx, observer);
        JSValue targets, active, skipped;
        uint32_t n, k;

        DCHECK(!JS_IsException(state), "a §3.2.1 [[resizeObservers]] list held something that is not a "
                                       "ResizeObserver — §2.1's constructor is its only writer");
        active = JS_NewArray(docctx);
        skipped = JS_NewArray(docctx);
        CHECK(!JS_IsException(active) && !JS_IsException(skipped),
              "a ResizeObserver's active or skipped target list could not be re-allocated");
        /* STEP 2.1: "Clear observer’s [[activeTargets]], and [[skippedTargets]]." A FRESH Array rather than a
           length reset, so a list the previous turn handed out (an entries walk holding one, a parked flow
           standing in one) is not emptied under its reader. */
        JS_SetPropertyUint32(docctx, state, RO_S_ACTIVE, JS_DupValue(docctx, active));
        JS_SetPropertyUint32(docctx, state, RO_S_SKIPPED, JS_DupValue(docctx, skipped));
        targets = JS_GetPropertyUint32(docctx, state, RO_S_TARGETS);
        n = ro_len(docctx, targets);
        for (k = 0; k < n; k++) {                                        /* step 2.2 */
            JSValue obs = JS_GetPropertyUint32(docctx, targets, k);

            if (ro_observation_is_active(docctx, obs)) {                 /* step 2.2.1 */
                JSValue tgt = JS_GetPropertyUint32(docctx, obs, ROB_TARGET);
                /* step 2.2.1.1 and 2.2.1.2: the target's depth against the loop's, which is what puts an
                   observation the caller has already broadcast past this turn into `skipped` instead of
                   activating it again. */
                int32_t td = ro_depth(docctx, node_of(tgt));

                JS_FreeValue(docctx, tgt);
                ro_push(docctx, td > depth ? active : skipped, JS_DupValue(docctx, obs));
            }
            JS_FreeValue(docctx, obs);
        }
        JS_FreeValue(docctx, targets);
        JS_FreeValue(docctx, active);
        JS_FreeValue(docctx, skipped);
        JS_FreeValue(docctx, state);
        JS_FreeValue(docctx, observer);
    }
    JS_FreeValue(docctx, list);
}

/* §3.4.2 and §3.4.3 are ONE walk asked TWO questions — see resize_observer.h for why they are two entries. */
static bool ro_any_nonempty(JSContext *docctx, int slot)
{
    JSValue list = ro_doc_list(docctx);
    uint32_t nobs = ro_len(docctx, list), i;
    bool any = false;

    for (i = 0; i < nobs && !any; i++) {
        JSValue observer = JS_GetPropertyUint32(docctx, list, i);
        JSValue state = ro_state(docctx, observer);
        JSValue l = JS_GetPropertyUint32(docctx, state, (uint32_t)slot);

        any = ro_len(docctx, l) != 0;
        JS_FreeValue(docctx, l);
        JS_FreeValue(docctx, state);
        JS_FreeValue(docctx, observer);
    }
    JS_FreeValue(docctx, list);
    return any;
}

bool resize_observer_has_active(JSContext *docctx)  { return ro_any_nonempty(docctx, RO_S_ACTIVE); }
bool resize_observer_has_skipped(JSContext *docctx) { return ro_any_nonempty(docctx, RO_S_SKIPPED); }

/* ---- §3.4.4 "Create and populate a ResizeObserverEntry" ---------------------------------------------------- */

/* The seven steps, in order, for `target`. `observed` is the observation's own box, and `out_*` receive the
 * examples of THAT box's two lengths — §3.4.5 step 2.3.3's "matching entry sizes", taken out of the very call
 * that computed the member the entry carries rather than recomputed at the caller.
 *
 * STEP 6's contentRect IS THE CONTENT BOX MADE PHYSICAL AND THEN OFFSET: "Set this.contentRect to logical
 * this.contentBoxSize given target and observedBox of \"content-box\"", then steps 6.1 and 6.2 set its top and
 * left to the target's padding top and padding left. §3.3.1 "content rect" states the same rectangle
 * directly — width is content width, height is content height, top is padding top, left is padding left — and
 * says why that origin: "Absolute position coordinate space origin is topLeft of the padding rect."
 *
 * STEP 7's SVG ARM is unreachable in this build for `ro_box_length`'s reason, and its two effects (top and
 * left both zero) would be the same rectangle for a box-less element anyway. */
static JSValue ro_create_and_populate_entry(JSContext *rctx, JSValueConst target, RoBoxOption observed,
                                            double *out_inline, double *out_block)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(node_of(target));
    CssPx size_i[RO_BOX_COUNT], size_b[RO_BOX_COUNT], pad_top, pad_left;
    JSValue border, content, device, rect;

    DCHECK(el != NULL, "§3.4.4 was asked to populate an entry for something that is not an element");
    /* Steps 3, 4 and 5, in the standard's own order. */
    border = ro_box_size(rctx, el, RO_BOX_BORDER, &size_i[RO_BOX_BORDER], &size_b[RO_BOX_BORDER]);
    content = ro_box_size(rctx, el, RO_BOX_CONTENT, &size_i[RO_BOX_CONTENT], &size_b[RO_BOX_CONTENT]);
    device = ro_box_size(rctx, el, RO_BOX_DEVICE_PIXEL_CONTENT,
                         &size_i[RO_BOX_DEVICE_PIXEL_CONTENT], &size_b[RO_BOX_DEVICE_PIXEL_CONTENT]);
    /* §3.4.5 step 2.3.3's "matching entry sizes", taken out of the very calls that computed the members the
       entry carries — ONE derivation reached once, so the number a page reads and the number the next frame's
       `isActive()` compares against cannot come apart. Step 2.3.3's own three-row table ("matching sizes are
       entry.borderBoxSize if observation.observedBox is \"border-box\"", and so on for the other two) is the
       array index here rather than a switch, because RoBoxOption is declared in §2.1's own keyword order —
       which is the whole reason that enumerator is the index and not an opaque tag. */
    *out_inline = size_i[observed].px;
    *out_block = size_b[observed].px;
    /* Step 6, and steps 6.1/6.2's origin. A box-less target has no padding to read either — §3.3.1's own
       consequence — so the two edges are asked only where a box exists, out of the same predicate step 3 used. */
    if (element_view_has_box(lxb_dom_interface_node(el)) &&
        element_view_fragment_kind(el) != ELEMENT_VIEW_FRAGMENTS_LINE_BOXES) {
        pad_top = used_value_px(el, "padding-top");
        pad_left = used_value_px(el, "padding-left");
    } else {
        pad_top = css_px(0.0);
        pad_left = css_px(0.0);
    }
    rect = dom_rect_readonly_new_values(rctx, ro_length_value(rctx, pad_left), ro_length_value(rctx, pad_top),
                                        ro_length_value(rctx, size_i[RO_BOX_CONTENT]),
                                        ro_length_value(rctx, size_b[RO_BOX_CONTENT]));
    CHECK(!JS_IsException(rect), "§3.4.4 step 6's content rect could not be allocated");
    return resize_observer_entry_new(rctx, target, rect, border, content, device);
}

/* ---- §3.4.5 "Broadcast active resize observations" --------------------------------------------------------- */

uint32_t resize_observer_broadcast_begin(JSContext *docctx)
{
    JSValue list = ro_doc_list(docctx);
    uint32_t n;

    /* STEP 1: "Let shallowestTargetDepth be ∞". It rides the realm's own list Array, so it is a property write
       the COW delta captures and two forked arms hold two of them — a `double` in a static would be one
       document's answer for every flow. */
    JS_SetProperty(docctx, list, g_atom_depth, JS_NewFloat64(docctx, HUGE_VAL));
    n = ro_len(docctx, list);
    JS_FreeValue(docctx, list);
    return n;
}

double resize_observer_broadcast_depth(JSContext *docctx)
{
    JSValue list = ro_doc_list(docctx), v = JS_GetProperty(docctx, list, g_atom_depth);
    double d = HUGE_VAL;

    DCHECK(JS_IsNumber(v),
           "§3.4.5 step 3's shallowestTargetDepth was read before resize_observer_broadcast_begin wrote step "
           "1's ∞ — the two are one algorithm and the begin is what starts it");
    JS_ToFloat64(docctx, &d, v);
    JS_FreeValue(docctx, v);
    JS_FreeValue(docctx, list);
    return d;
}

/* Step 2's per-observer body, up to but not including step 2.4's invoke — see resize_observer.h for the split.
   Returns JS_UNDEFINED for step 2.1's "continue". */
JSValue resize_observer_broadcast_prepare(JSContext *docctx, uint32_t i)
{
    JSValue list = ro_doc_list(docctx);
    JSValue observer, state, active, entries, callback, out;
    JSContext *rctx;
    uint32_t n, k;
    double shallowest;

    DCHECK(i < ro_len(docctx, list),
           "§3.4.5 step 2 was asked for an observer past the end of the list resize_observer_broadcast_begin "
           "counted — the count and the walk are one snapshot");
    observer = JS_GetPropertyUint32(docctx, list, i);
    state = ro_state(docctx, observer);
    DCHECK(!JS_IsException(state),
           "a §3.2.1 [[resizeObservers]] list held something that is not a ResizeObserver");
    active = JS_GetPropertyUint32(docctx, state, RO_S_ACTIVE);
    n = ro_len(docctx, active);
    if (n == 0) {                                                        /* step 2.1: continue */
        JS_FreeValue(docctx, active);
        JS_FreeValue(docctx, state);
        JS_FreeValue(docctx, observer);
        JS_FreeValue(docctx, list);
        return JS_UNDEFINED;
    }
    /* THE ENTRIES ARE MINTED IN THE OBSERVER'S OWN REALM. §2.1's constructor put this observer on the
       CONSTRUCTING document's list, so that realm and `docctx` are the same one — a derivation, asserted here
       rather than assumed, because it is exactly what is NOT true of the observer next door. */
    rctx = ro_realm(docctx, state);
    DCHECK(rctx == docctx,
           "a ResizeObserver is on a document's [[resizeObservers]] list belonging to a realm that is not the "
           "one it was constructed in — §2.1's constructor step 4 adds `this` to the CONSTRUCTING Document's "
           "slot and nothing else writes either");
    entries = JS_NewArray(rctx);                                         /* step 2.2 */
    CHECK(!JS_IsException(entries), "§3.4.5 step 2.2's entries list could not be allocated");
    shallowest = resize_observer_broadcast_depth(docctx);
    for (k = 0; k < n; k++) {                                            /* step 2.3 */
        JSValue obs = JS_GetPropertyUint32(docctx, active, k);
        JSValue target = JS_GetPropertyUint32(docctx, obs, ROB_TARGET);
        int32_t box = (int32_t)ro_num_at(docctx, obs, ROB_BOX);
        double last_i = 0.0, last_b = 0.0;
        int32_t td;

        DCHECK(box >= 0 && box < RO_BOX_COUNT,
               "a §3.1 ResizeObservation's observedBox is not one §2.1's enumeration declares");
        /* steps 2.3.1 and 2.3.2 */
        ro_push(rctx, entries,
                ro_create_and_populate_entry(rctx, target, (RoBoxOption)box, &last_i, &last_b));
        /* STEP 2.3.3: "Set observation.lastReportedSizes to matching entry sizes." The two numbers come out of
           the very call that built the member the entry carries, so the value a page reads and the value the
           next frame's `isActive()` compares against are ONE computation and cannot come apart. */
        JS_SetPropertyUint32(docctx, obs, ROB_LAST_INLINE, JS_NewFloat64(docctx, last_i));
        JS_SetPropertyUint32(docctx, obs, ROB_LAST_BLOCK, JS_NewFloat64(docctx, last_b));
        td = ro_depth(docctx, node_of(target));                          /* step 2.3.4 */
        if ((double)td < shallowest) shallowest = (double)td;            /* step 2.3.5 */
        JS_FreeValue(docctx, target);
        JS_FreeValue(docctx, obs);
    }
    JS_SetProperty(docctx, list, g_atom_depth, JS_NewFloat64(docctx, shallowest));
    callback = JS_GetPropertyUint32(docctx, state, RO_S_CALLBACK);
    DCHECK(JS_IsFunction(docctx, callback),
           "a ResizeObserver's [[callback]] is not callable — §2.1's constructor takes a "
           "ResizeObserverCallback and Web IDL §3.2.19 Callback function types' brand test is what makes a "
           "non-callable a TypeError before the slot is written");
    /* STEP 2.4's three operands, in the order the standard names them: the callback, the `this` value (the
       observer) and the second argument (the observer again). The caller performs the invoke because it runs
       the page's code and this component holds no rest point. */
    out = JS_NewArray(docctx);
    CHECK(!JS_IsException(out), "§3.4.5 step 2.4's call operands could not be allocated");
    JS_SetPropertyUint32(docctx, out, 0, callback);
    JS_SetPropertyUint32(docctx, out, 1, JS_DupValue(docctx, observer));
    JS_SetPropertyUint32(docctx, out, 2, entries);
    JS_FreeValue(docctx, active);
    JS_FreeValue(docctx, state);
    JS_FreeValue(docctx, observer);
    JS_FreeValue(docctx, list);
    return out;
}

void resize_observer_broadcast_finish(JSContext *docctx, uint32_t i)
{
    JSValue list = ro_doc_list(docctx);
    JSValue observer = JS_GetPropertyUint32(docctx, list, i);
    JSValue state = ro_state(docctx, observer);
    JSValue fresh;

    DCHECK(!JS_IsException(state),
           "a §3.2.1 [[resizeObservers]] list held something that is not a ResizeObserver");
    /* STEP 2.5: "Clear observer.[[activeTargets]]." It runs AFTER the callback has returned, which is what
       makes an observation the callback itself re-activated — by resizing the very element it was told about —
       survive into step 16's next turn instead of being cleared out from under it.
       THE INDEX STILL NAMES THE SAME OBSERVER, and that is a derivation rather than an assumption: nothing in
       this standard REMOVES an observer from a Document's [[resizeObservers]] — §3.5 "ResizeObserver Lifetime"
       states the end of one as a garbage-collection condition ("there are no scripting references to the
       observer" and "the observer is not observing any targets") and no algorithm splices the list — so the
       only mutation a callback can make to it is §2.1's constructor APPENDING, which cannot move an index
       already in range. An index over a set the page can reorder would be the defect CLAUDE.md names at
       §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED; this set is append-only by construction. */
    fresh = JS_NewArray(docctx);
    CHECK(!JS_IsException(fresh), "§3.4.5 step 2.5's cleared active target list could not be allocated");
    JS_SetPropertyUint32(docctx, state, RO_S_ACTIVE, fresh);
    JS_FreeValue(docctx, state);
    JS_FreeValue(docctx, observer);
    JS_FreeValue(docctx, list);
}

/* ---- §3.4.6 "Deliver Resize Loop Error" -------------------------------------------------------------------- */

#define RO_ERR_STAGES(X)                                                                                       \
    X(RO_ERR_MINT,                                                                                             \
      "RESIZE OBSERVER §3.4.6 deliver resize loop error steps 1-2 (create a new ErrorEvent and initialize its " \
      "message slot to \"ResizeObserver loop completed with undelivered notifications.\")")                     \
    X(RO_ERR_FIRE,                                                                                             \
      "RESIZE OBSERVER §3.4.6 step 3 (report the exception event — HTML §8.1.4.6 step 6.2's fire of `error` "   \
      "at the global, using ErrorEvent, cancelable), parked on the page's `error` listeners")
enum { RO_ERR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RO_ERR_STEPS[] = { RO_ERR_STAGES(JS_STEP_STAGE_LABEL) NULL };
#define RO_ERR_STAGE_COUNT ((int)(sizeof RO_ERR_STEPS / sizeof *RO_ERR_STEPS) - 1)
/* ONE NAME FOR THE ALGORITHM THESE STAGES ARE STEPS OF — the dispatch's abort and the entry assert share it,
   so neither can name a different algorithm from the other. */
#define RO_ERR_ALGORITHM "RESIZE OBSERVER §3.4.6 deliver resize loop error"

void resize_observer_loop_error_start(ResizeObserverLoopError *w)
{
    int i;

    w->stage = RO_ERR_MINT;
    w->phase = 0;
    w->ev = JS_UNDEFINED;
    for (i = 0; i < EVENT_FIRE_CB_SLOTS; i++)
        w->cb[i] = JS_UNDEFINED;
}

void resize_observer_loop_error_visit(JSContext *ctx, ResizeObserverLoopError *w, JSStepVisit *v)
{
    int i;

    v->val(ctx, &w->ev);
    for (i = 0; i < EVENT_FIRE_CB_SLOTS; i++)
        v->val(ctx, &w->cb[i]);
}

void resize_observer_loop_error_release(JSContext *ctx, ResizeObserverLoopError *w)
{
    resize_observer_loop_error_visit(ctx, w, JS_StepFreeVisitor());
}

int resize_observer_loop_error_run(JSContext *ctx, ResizeObserverLoopError *w, JSValue in,
                                   JSValue **out_cb, int *out_argc)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValueConst global = g;
    bool not_canceled = true;
    int r;

    JS_FreeValue(ctx, g);   /* a realm owns its global for the realm's whole life — this is a borrow */

    DCHECK((int)w->stage < RO_ERR_STAGE_COUNT,
           RO_ERR_ALGORITHM " was entered at a stage past the end of its own declaration — the record is the "
           "CALLER's, so a stage out of range means the caller is holding a record some other machine wrote");
    STEP_DISPATCH(RO_ERR_STAGES, w->stage, RO_ERR_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(RO_ERR_MINT);
    JS_FreeValue(ctx, in);   /* nothing has asked for anything yet, so this entry's answer belongs to nobody */
    /* STEPS 1 AND 2: "Create a new ErrorEvent. Initialize event's message slot to 'ResizeObserver loop
       completed with undelivered notifications.'." Every other member of the event is what the interface's own
       initialiser leaves: no error value, no filename, no position — §3.4.6 states the message and states
       nothing else, and inventing a filename here would report this engine's C as the script that failed.
       THE TYPE AND THE CANCELABILITY ARE THE FIRE'S, not the interface's (core/events/error_event.h), and they
       are HTML §8.1.4.6 step 6.2's: `error`, cancelable, so an `onerror` that returns true cancels this
       exactly as it cancels a script error. */
    w->ev = error_event_new(ctx, "error", /*cancelable*/ true,
                            JS_NewString(ctx, "ResizeObserver loop completed with undelivered notifications."),
                            JS_UNDEFINED, 0, 0, JS_NULL);
    CHECK(!JS_IsException(w->ev), "§3.4.6's ErrorEvent could not be allocated");
    STEP_GOTO(w->stage, RO_ERR_FIRE, &w->phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(RO_ERR_FIRE);
    /* STEP 3: report the exception event. It is the SAME §2.9 dispatch every other fire in this engine uses —
       reached as a REQUEST because the page's `error` listeners run from it and this caller can park. */
    r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), global, w->ev, JS_UNDEFINED, in,
                              &not_canceled, out_cb, out_argc);
    if (r)
        return r;
    /* THE DISPATCH'S ANSWER IS READ BY NO STEP OF §3.4.6, and that is the section's own shape rather than a
       value this file drops. HTML §8.1.4.6 has a step 7 that reads notHandled — it is what decides whether an
       exception a page CANCELLED still reaches a developer console — and §3.4.6 has three steps and no such
       step: there is nothing behind this fire to skip. The out-parameter is taken because the dispatch has one
       answer and every caller must give it somewhere to go; asserting on it would be asserting about the
       page's own listeners, which is input rather than an invariant of this engine's. */
    JS_FreeValue(ctx, w->ev);
    w->ev = JS_UNDEFINED;
    return 0;
}

/* ---- §2.1's observe / unobserve / disconnect ---------------------------------------------------------------- */

/* RESIZE OBSERVER §2.1's `unobserve(target)`, as the internal operation its own `observe` calls: "Let
   observation be ResizeObservation in [[observationTargets]] whose target slot is target. If observation is not
   found, return. Remove observation from [[observationTargets]]." Returns whether one was removed, which
   `observe`'s step 1 does not read and which the assert below does. */
static bool ro_unobserve(JSContext *ctx, JSValueConst state, JSValueConst target)
{
    JSValue targets = JS_GetPropertyUint32(ctx, (JSValue)state, RO_S_TARGETS);
    uint32_t n = ro_len(ctx, targets), i, k;
    bool removed = false;

    for (i = 0; i < n; i++) {
        JSValue obs = JS_GetPropertyUint32(ctx, targets, i);
        JSValue t = JS_GetPropertyUint32(ctx, obs, ROB_TARGET);
        bool hit = JS_VALUE_GET_PTR(t) == JS_VALUE_GET_PTR(target);

        JS_FreeValue(ctx, t);
        JS_FreeValue(ctx, obs);
        if (!hit) continue;
        for (k = i; k + 1 < n; k++)
            JS_SetPropertyUint32(ctx, targets, k, JS_GetPropertyUint32(ctx, targets, k + 1));
        ro_set_len(ctx, targets, n - 1);
        removed = true;
        break;
    }
    JS_FreeValue(ctx, targets);
    return removed;
}

/* RESIZE OBSERVER §2.1's `observe(target, options)`: "Adds target to the list of observed elements."
     1. If target is in [[observationTargets]] slot, call unobserve() with argument target.
     2. Let observedBox be the value of the box dictionary member of options.
     3. Let resizeObservation be new ResizeObservation(target, observedBox).
     4. Add the resizeObservation to the [[observationTargets]] slot.
   STEP 1 IS WHAT MAKES A SECOND `observe` OF ONE ELEMENT REPLACE THE FIRST rather than double it, which §2.1's
   own example says in so many words ("Observe just the border box. Replaces previous observation."). It also
   RESETS §3.1's lastReportedSizes to (−1, −1), because step 3 mints a new observation — so re-observing an
   element that has not changed size still delivers on the next frame. */
static JSValue js_ro_observe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = ro_state(ctx, this_val), targets, obs, box;
    const char *kw;
    int i, observed = -1;

    (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(argc >= 1 && element_is(argv[0]),
           "observe reached its body without the Element its declaration requires — §2.1's "
           "`observe(Element target, …)` is a REQUIRED interface-typed argument, so Web IDL §3.2.15 Interface "
           "types' brand and the declaration's narrowing are what make everything else a TypeError before "
           "step 1");
    /* `optional ResizeObserverOptions options = {}`. A DECLARED DICTIONARY POSITION IS CONVERTED EVEN WHEN THE
       PAGE STOPPED SHORT OF IT (core/idl_args.c states the rule at the count it extends), so `observe(el)`
       arrives here with the object built and §2.1's `box = "content-box"` already placed on it. This is the
       ASSERTION of that and not a substitute for it: a body that built its own empty object, or that read the
       member as "content-box if absent", would be re-deriving the IDL's `= {}` and then the IDL's default —
       two answers to one question, free to disagree the day the dictionary gains a second member. */
    DCHECK(argc >= 2 && JS_IsObject(argv[1]),
           "observe's options reached the body without the object the dictionary conversion builds — §2.1 "
           "gives its one member a default and this body reads it rather than inventing it");
    ro_unobserve(ctx, state, argv[0]);                                   /* step 1 */
    box = JS_GetPropertyStr(ctx, (JSValue)argv[1], "box");               /* step 2 */
    IDL_DCHECK_MEMBER(JS_IsString(box), box, "box",
                      "`ResizeObserverBoxOptions` with a `= \"content-box\"` default by Resize Observer §2.1's "
                      "ResizeObserverOptions dictionary");
    kw = JS_ToCString(ctx, box);
    CHECK(kw != NULL, "§2.1's `box` member could not be read back as a string");
    for (i = 0; i < RO_BOX_COUNT; i++)
        if (strcmp(kw, RO_BOX_VALUES[i]) == 0) observed = i;
    JS_FreeCString(ctx, kw);
    JS_FreeValue(ctx, box);
    DCHECK(observed >= 0,
           "§2.1's `box` member reached the body carrying a keyword its enumeration does not list — Web IDL "
           "§3.2.18 Enumeration types' conversion is what throws a TypeError for every other string, so a "
           "value arriving here is one this declaration's value list and that check disagree about");
    /* STEP 3: "new ResizeObservation(target, observedBox)", whose own steps set the target, the observed box
       and `lastReportedSizes` to [(−1,−1)] — the pair no box extent can equal, which is why a fresh
       observation always delivers on the next frame. */
    obs = JS_NewArray(ctx);
    CHECK(!JS_IsException(obs), "a §3.1 ResizeObservation could not be allocated");
    JS_SetPropertyUint32(ctx, obs, ROB_TARGET, JS_DupValue(ctx, argv[0]));
    JS_SetPropertyUint32(ctx, obs, ROB_BOX, JS_NewInt32(ctx, observed));
    JS_SetPropertyUint32(ctx, obs, ROB_LAST_INLINE, JS_NewFloat64(ctx, -1.0));
    JS_SetPropertyUint32(ctx, obs, ROB_LAST_BLOCK, JS_NewFloat64(ctx, -1.0));
    targets = JS_GetPropertyUint32(ctx, state, RO_S_TARGETS);            /* step 4 */
    ro_push(ctx, targets, obs);
    JS_FreeValue(ctx, targets);
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

static JSValue js_ro_unobserve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = ro_state(ctx, this_val);

    (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(argc >= 1 && element_is(argv[0]),
           "unobserve reached its body without the Element its declaration requires — §2.1's "
           "`unobserve(Element target)` is a REQUIRED interface-typed argument");
    ro_unobserve(ctx, state, argv[0]);
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* RESIZE OBSERVER §2.1's `disconnect()`: "Clear the [[observationTargets]] list. Clear the [[activeTargets]] list."
   IT CLEARS THE TWO THE SECTION NAMES AND NOT THE THIRD, which is the standard's own text and is checkable:
   [[skippedTargets]] is written only by §3.4.1, which clears it at the top of every gather, so a stale entry
   there cannot survive one turn of step 16's loop and clearing it here would change nothing a page can see.
   Adding the third would be this engine answering a question §2.1 did not ask. */
static JSValue js_ro_disconnect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = ro_state(ctx, this_val);

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    JS_SetPropertyUint32(ctx, state, RO_S_TARGETS, JS_NewArray(ctx));
    JS_SetPropertyUint32(ctx, state, RO_S_ACTIVE, JS_NewArray(ctx));
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* ---- §2.1's CONSTRUCTOR ------------------------------------------------------------------------------------ */

typedef struct { uint8_t unused; } JSRoCtorState;
static void js_ro_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

#define RO_CTOR_STAGES(X)                                                                                      \
    X(RO_CTOR_BUILD = IDL_STEP_FIRST,                                                                          \
      "RESIZE OBSERVER §2.1 new ResizeObserver(callback) (set [[callback]], empty [[observationTargets]], and " \
      "add this to the Document's [[resizeObservers]] slot)")
enum { RO_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RO_CTOR_STEPS[] = { RO_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_ro_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj, state, proto, list;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == RO_CTOR_BUILD,
           "the ResizeObserver constructor resumed at a stage §2.1 does not have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor ResizeObserver requires 'new'"), -1;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ResizeObserver requires a callback"), -1;
    DCHECK(JS_IsFunction(ctx, argv[0]),
           "ResizeObserver's callback reached the body unconverted — §2.2 declares it a ResizeObserverCallback, "
           "and Web IDL §3.2.19 Callback function types' brand test is what makes a non-callable a TypeError "
           "before step 1");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "a ResizeObserver was constructed in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return -1;
    state = JS_NewArray(ctx);
    CHECK(!JS_IsException(state), "a ResizeObserver's state could not be allocated");
    JS_SetPropertyUint32(ctx, state, RO_S_CALLBACK, JS_DupValue(ctx, argv[0]));   /* step 2 */
    JS_SetPropertyUint32(ctx, state, RO_S_TARGETS, JS_NewArray(ctx));             /* step 3 */
    /* §3.2.2's other two lists are empty for the same reason: the section declares four slots and the
       constructor is what puts every one of them on the object, so no reader has an absence to default past. */
    JS_SetPropertyUint32(ctx, state, RO_S_ACTIVE, JS_NewArray(ctx));
    JS_SetPropertyUint32(ctx, state, RO_S_SKIPPED, JS_NewArray(ctx));
    /* THE CONSTRUCTING REALM, RECORDED — see the shapes comment for what reads it and why the read is asserted
       rather than assumed. */
    JS_SetPropertyUint32(ctx, state, RO_S_WINDOW, JS_DupValue(ctx, document_window_proxy(ctx)));
    JS_DefinePropertyValue(ctx, obj, g_atom_state, state, 0);
    /* STEP 4: "Add this to Document.[[resizeObservers]] slot." THE DOCUMENT IS THE CONSTRUCTING ONE, which is
       what makes this observer's list, its realm and its entries all the same realm's — the derivation
       §3.4.5's walk asserts. */
    list = ro_doc_list(ctx);
    ro_push(ctx, list, JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, list);
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_ro_ctor_decl = {
    js_ro_ctor_step, sizeof(JSRoCtorState), js_ro_ctor_visit, NULL,
    "RESIZE OBSERVER §2.1 new ResizeObserver(callback)", RO_CTOR_STEPS
};

/* ---- declaration and installation --------------------------------------------------------------------------- */

void resize_observer_init(JSContext *ctx)
{
    /* §2.1's `dictionary ResizeObserverOptions`, MEMBER FOR MEMBER AND DEFAULT FOR DEFAULT. Its one member
       carries the IDL's `= "content-box"`, so `observe(el)` and `observe(el, {})` read a value on every path
       and this file never invents one. */
    static const IdlDictMember OBSERVE_OPTIONS[] = {
        { "box", IDL_ENUM, false, RO_BOX_VALUES, 0, NULL, IDL_DEFAULT_STRING, "content-box" },
    };
    static const IdlArgType OBSERVE_ARGS[2] = { IDL_INTERFACE, IDL_DICT };
    static const IdlArgType UNOBSERVE_ARGS[1] = { IDL_INTERFACE };
    static const IdlArgType CTOR_ARGS[1] = { IDL_CALLBACK };
    JSClassDef d = { "ResizeObserver" };

    if (g_ready) return;   /* one AGENT, one class */
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);
    resize_observer_entry_init(ctx);

    g_state_key = JS_NewSymbol(ctx, "resizeObserverState", false);
    CHECK(!JS_IsException(g_state_key), "the ResizeObserver state slot key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    g_atom_depth = JS_NewAtom(ctx, "shallowestTargetDepth");
    CHECK(g_atom_state != JS_ATOM_NULL && g_atom_depth != JS_ATOM_NULL,
          "a Resize Observer slot key could not be interned");

    g_docobs_slot = realm_value_declare(ctx, "§3.2.1 the Document's [[resizeObservers]]");

    g_id_ctor = idl_method_id_step(ctx, CTOR_ARGS, 1, NULL, 0, &js_ro_ctor_decl, 0);
    g_id_observe = idl_method_id_dict(ctx, OBSERVE_ARGS, 2, OBSERVE_OPTIONS, (int)COUNTOF(OBSERVE_OPTIONS),
                                      js_ro_observe, 0);
    idl_iface_brand(node_class_id());
    idl_iface_narrow(element_is);        /* `observe(Element target)` */
    idl_optional_from(1);                /* `optional ResizeObserverOptions options = {}` */
    g_id_unobserve = idl_method_id(ctx, UNOBSERVE_ARGS, 1, js_ro_unobserve, 0);
    idl_iface_brand(node_class_id());
    idl_iface_narrow(element_is);        /* `unobserve(Element target)` */
    g_id_disconnect = idl_method_id(ctx, NULL, 0, js_ro_disconnect, 0);

    realm_declare_intrinsic(resize_observer_install_proto);
    g_ready = 1;

    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. §2.3's two record interfaces are
       declared under this same component name from their own `_init`s: one declare, one install and one
       release cover all three, which is what core/platform.c's witness table says by mapping every one of the
       three names to this component. `g_ready` is the latch this init consults first and is therefore the one
       that must not survive the release. */
    agent_state_flag("resize_observer", &g_ready, "the declaration latch");
    agent_state_class("resize_observer", &g_class, "§2.1's ResizeObserver class");
    agent_state_value("resize_observer", &g_state_key, "the observer's state-slot key");
    agent_state_atom("resize_observer", &g_atom_state, "the observer's state-slot key, interned");
    agent_state_atom("resize_observer", &g_atom_depth, "§3.4.5 step 1's shallowestTargetDepth field name");
    agent_state_id("resize_observer", &g_docobs_slot,
                   "the per-realm slot §3.2.1's [[resizeObservers]] is held in");
    agent_state_id("resize_observer", &g_id_ctor, "§2.1's constructor declaration");
    agent_state_id("resize_observer", &g_id_observe, "§2.1's observe declaration");
    agent_state_id("resize_observer", &g_id_unobserve, "§2.1's unobserve declaration");
    agent_state_id("resize_observer", &g_id_disconnect, "§2.1's disconnect declaration");
}

void resize_observer_install_proto(JSContext *ctx)
{
    JSValue proto, prev, list;

    DCHECK(g_class != 0, "a realm asked for ResizeObserver.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "resize_observer_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* §3.2.1's per-DOCUMENT state, built EAGERLY with the realm. Built lazily on first `new ResizeObserver` it
       would be created inside whichever flow happened to construct first, which puts a baseline object in one
       flow's COW delta and makes every sibling's observer list that flow's. */
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a realm's §3.2.1 [[resizeObservers]] could not be allocated");
    JS_SetProperty(ctx, list, g_atom_depth, JS_NewFloat64(ctx, HUGE_VAL));
    realm_value_set(ctx, g_docobs_slot, list);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "ResizeObserver.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ResizeObserver");
    idl_install_method(ctx, proto, "observe", g_id_observe);
    idl_install_method(ctx, proto, "unobserve", g_id_unobserve);
    idl_install_method(ctx, proto, "disconnect", g_id_disconnect);
    JS_SetClassProto(ctx, g_class, proto);
}

void resize_observer_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    resize_observer_entry_install(ctx, global);
    DCHECK(g_id_ctor >= 0, "ResizeObserver was installed before resize_observer_init declared it");
    ctor = idl_step_constructor(ctx, "ResizeObserver", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the ResizeObserver interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "ResizeObserver was installed in a realm that never ran its prototype install");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "ResizeObserver", ctor);
}

/* NONE OF THIS COMPONENT'S THREE CLASSES HAS A FINALIZER OR A gc_mark, which is why resetting the class ids is
 * safe — core/agent_state.h's cost, answered rather than assumed. All three `JSClassDef`s are a name and
 * nothing else, so `rt->class_array[id].finalizer` and `.gc_mark` are NULL and the collector has nothing of
 * this component's to dispatch to. Nothing is held where the collector cannot see it either: an observer's
 * five slots, an observation's four, an entry's five and a size's two are all OWN PROPERTIES, and the property
 * walk in mark_children and free_object is UNCONDITIONAL — it runs before the class hook is consulted and does
 * not depend on one existing. The per-realm list is the REALM's and goes with its context; what is agent state
 * about it is the SLOT NUMBER, declared and reset here. */
void resize_observer_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — this is a row on core/platform.h's release column and the declare pass
       that pairs with it is unconditional, so a release reached in an agent that never declared is the thing
       to CRASH on rather than the thing to skip. */
    DCHECK(g_ready, "Resize Observer was released in an agent that never declared it");
    resize_observer_entry_free(rt);
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_state);
    JS_FreeAtomRT(rt, g_atom_depth);
    g_atom_state = g_atom_depth = JS_ATOM_NULL;
    g_docobs_slot = -1;
    g_id_ctor = g_id_observe = g_id_unobserve = g_id_disconnect = -1;
    g_class = 0;
    g_ready = 0;
}
