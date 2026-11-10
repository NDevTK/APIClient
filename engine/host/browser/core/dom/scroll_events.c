/* CSSOM VIEW §13.2 "Scrolling" — see scroll_events.h for why the queue and its drain are one file, why the
   list is a JS Array on a per-realm record, and why a viewport's target is its Document. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/css/css_computed_value.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/scroll_events.h"
#include "core/events/event.h"
#include "core/realm.h"

/* The record's two fields: the pending scroll events, and the list the running scroll steps took out of it
   (scroll_events.h). */
#define SE_PENDING "pending"
#define SE_FIRING  "firing"
/* A pair is stored as the two-element Array §13.2 describes, so the pair stays atomic across a park: an
   (EventTarget, DOMString) split over two parallel lists is two facts that can be appended to unequally. */
#define SE_PAIR_TARGET 0
#define SE_PAIR_TYPE   1

static JSClassID g_slot = JS_INVALID_CLASS_ID;

static uint32_t se_len(JSContext *ctx, JSValueConst arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* OWNED — the caller frees. */
static JSValue se_list(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);
    JSValue list = JS_GetPropertyStr(ctx, rec, SE_PENDING);

    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsArray(list),
           "§13.2's pending scroll events is not an Array — the record is built with the realm holding one, "
           "and only this file writes it");
    return list;
}

/* §13.2's own idempotence test, stated by three of its algorithms in the same words: "If (doc, \"scroll\") is "
   already in doc's pending scroll events, abort these steps." The pair is compared on BOTH halves, because a
   Document with a queued `scroll` may legitimately also want a `scrollend`. */
static bool se_holds(JSContext *ctx, JSValueConst list, JSValueConst target, const char *type)
{
    uint32_t n = se_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, list, i);
        JSValue t = JS_GetPropertyUint32(ctx, pair, SE_PAIR_TARGET);
        JSValue ty = JS_GetPropertyUint32(ctx, pair, SE_PAIR_TYPE);
        const char *s = JS_ToCString(ctx, ty);
        bool same;

        CHECK(s != NULL, "§13.2's pending scroll events holds a type that could not be read as a string");
        same = JS_IsSameValue(ctx, t, target) && strcmp(s, type) == 0;
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, t);
        JS_FreeValue(ctx, ty);
        JS_FreeValue(ctx, pair);
        if (same) return true;
    }
    return false;
}

/* The APPEND the three gets-scrolled algorithms and the scroll steps' step 1 all end in, with §13.2's own
   preceding abort-if-already-there. Every mutation here is a property write, which is what the per-flow COW
   delta captures. */
static void se_append(JSContext *ctx, JSValueConst target, const char *type)
{
    JSValue list = se_list(ctx), pair;

    if (se_holds(ctx, list, target, type)) { JS_FreeValue(ctx, list); return; }
    pair = JS_NewArray(ctx);
    CHECK(!JS_IsException(pair), "§13.2: OOM building a pending scroll event pair");
    JS_SetPropertyUint32(ctx, pair, SE_PAIR_TARGET, JS_DupValue(ctx, target));
    JS_SetPropertyUint32(ctx, pair, SE_PAIR_TYPE, JS_NewString(ctx, type));
    JS_SetPropertyUint32(ctx, list, se_len(ctx, list), pair);
    JS_FreeValue(ctx, list);
}

/* §13.2's SNAP-CONTAINER STEPS, asserted unreachable at each of the places it states one — see
   scroll_events.h. It is one assert with one message because it is one absent property; `where` is what tells
   the reader which of §13.2's steps they are standing at, so the crash names an action with an object. */
static void se_no_snap_container(const char *where)
{
    DCHECKF(!css_computed_models("scroll-snap-type"),
            "css-scroll-snap-1 §4.1 \"Scroll Snapping Rules: the scroll-snap-type property\" is in this "
            "cascade now, so a box in this document can be a SNAP CONTAINER and CSSOM VIEW §13.2 "
            "\"Scrolling\" has a step this engine does not run, at %s. BUILD css-scroll-snap-1's snap "
            "positions, then the eventual-snap-target and scrollsnapchange/scrollsnapchanging targets §13.2 "
            "reads off them — and note that §13.2's step 2 branches for those two fire a `SnapEvent` and not "
            "an `Event`, so the interface is part of the same build", where);
}

void scroll_events_viewport_scrolled(JSContext *ctx)
{
    /* Step 1 — "Let doc be the viewport's associated `Document`." — is the realm's own document, which is what
       a viewport belongs to; scroll_events.h states why the pair's TARGET is that document. */
    JSValueConst doc = document_object(ctx);

    DCHECK(JS_IsObject(doc),
           "§13.2's viewport-gets-scrolled steps ran in a realm with no Document — a viewport is the area a "
           "navigable presents a document IN, so §3.1's perform a scroll cannot have moved one here");
    /* Step 2 */
    se_no_snap_container("the viewport-gets-scrolled steps' step 2");
    /* Steps 3 and 4 — "If (doc, \"scroll\") is already in doc's pending scroll events, abort these steps." /
       "Append (doc, \"scroll\") to doc's pending scroll events." */
    se_append(ctx, doc, "scroll");
}

/* §3.1 step 7.1's "emit the scrollend event" for an instant scroll: the pair is appended at once, behind the
   target's `scroll`, where §13.2's scroll steps step 1 would append it behind every pending `scroll` (Chrome,
   scroll_events.h). Step 1.2's snap-container step and step 1.3's already-pending test come with it. */
static void se_scrollend(JSContext *ctx, JSValueConst target)
{
    se_no_snap_container("the scroll steps' step 1.2");
    se_append(ctx, target, "scrollend");
}

void scroll_events_viewport_scrollend(JSContext *ctx)
{
    se_scrollend(ctx, document_object(ctx));
}

void scroll_events_element_scrolled(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue target;

    /* Step 1: "Let doc be the element's node document", whose record this realm holds. */
    DCHECK(el != NULL && document_realm_of(lxb_dom_interface_node(el)) == ctx,
           "§13.2's element-gets-scrolled steps ran for an element whose node document is not this realm's; "
           "§3.1 is handed the realm of the box's own document");
    /* Step 2 */
    se_no_snap_container("the element-gets-scrolled steps' step 2");
    /* Steps 3 and 4: abort if (element, "scroll") is already pending, else append it. */
    target = node_wrap(ctx, lxb_dom_interface_node(el));
    CHECK(JS_IsObject(target), "an element that was scrolled could not be wrapped");
    se_append(ctx, target, "scroll");
    JS_FreeValue(ctx, target);
}

void scroll_events_element_scrollend(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue target = node_wrap(ctx, lxb_dom_interface_node(el));

    CHECK(JS_IsObject(target), "an element that was scrolled could not be wrapped");
    se_scrollend(ctx, target);
    JS_FreeValue(ctx, target);
}

bool scroll_events_pending(JSContext *ctx)
{
    JSValue list = se_list(ctx);
    bool any = se_len(ctx, list) > 0;

    JS_FreeValue(ctx, list);
    return any;
}

/* OWNED — the caller frees. */
static JSValue se_firing(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);
    JSValue firing = JS_GetPropertyStr(ctx, rec, SE_FIRING);

    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsArray(firing), "§13.2's firing list is not an Array; the record is built holding one and only "
                               "this file writes it");
    return firing;
}

uint32_t scroll_events_scroll_steps_begin(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);
    JSValue list = JS_GetPropertyStr(ctx, rec, SE_PENDING);
    JSValue firing = se_firing(ctx);
    uint32_t n = se_len(ctx, list);

    DCHECK(se_len(ctx, firing) == 0, "§13.2's scroll steps began while an earlier run's list was still being "
                                     "fired; update the rendering runs them once per document per frame");
    JS_FreeValue(ctx, firing);
    /* The pending list is taken whole and replaced, so a pair a listener appends is the next frame's and is
       tested for "already pending" against that frame's list only (Chrome, scroll_events.h). Written only when
       non-empty, so an idle frame adds nothing to a flow's delta. */
    if (n > 0) {
        JS_SetPropertyStr(ctx, rec, SE_FIRING, JS_DupValue(ctx, list));
        JS_SetPropertyStr(ctx, rec, SE_PENDING, JS_NewArray(ctx));
    }
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, rec);
    return n;
}

void scroll_events_item(JSContext *ctx, uint32_t i, JSValue *target, JSValue *ev)
{
    JSValue list = se_firing(ctx), pair, ty;
    const char *type;

    DCHECK(target != NULL && ev != NULL,
           "§13.2's scroll steps step 2 was asked for an item with nowhere to put it");
    DCHECK(i < se_len(ctx, list),
           "§13.2's scroll steps step 2 was asked for an item past the list the steps took; nothing appends to "
           "that list once it is taken");
    pair = JS_GetPropertyUint32(ctx, list, i);
    *target = JS_GetPropertyUint32(ctx, pair, SE_PAIR_TARGET);
    ty = JS_GetPropertyUint32(ctx, pair, SE_PAIR_TYPE);
    type = JS_ToCString(ctx, ty);
    CHECK(type != NULL, "§13.2's pending scroll events holds a type that could not be read as a string");
    /* Step 2: a Document's `scroll`/`scrollend` "bubbles"; the SnapEvent branches are the snap-container steps
       asserted above; "Otherwise, fire an event named type at target", which does not bubble. Fire leaves
       cancelable unset (DOM §2.10 "Firing events"). */
    DCHECK(strcmp(type, "scroll") == 0 || strcmp(type, "scrollend") == 0,
           "§13.2's scroll steps step 2 reached a pending scroll event whose type is neither `scroll` nor "
           "`scrollend` — a snap event's pair has arrived and step 2's SnapEvent branches must be written for it");
    if (JS_IsSameValue(ctx, *target, document_object(ctx))) {
        *ev = event_new(ctx, type, /*bubbles*/ true, /*cancelable*/ false);
    } else {
        DCHECK(node_of(*target) != NULL && node_of(*target)->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "§13.2's scroll steps step 2 reached a pending scroll event whose target is neither this Document "
               "nor an element — a VisualViewport's pair has arrived, which nothing in this engine scrolls");
        *ev = event_new(ctx, type, /*bubbles*/ false, /*cancelable*/ false);
    }
    CHECK(!JS_IsException(*ev), "§13.2's scroll steps could not allocate their event");
    JS_FreeCString(ctx, type);
    JS_FreeValue(ctx, ty);
    JS_FreeValue(ctx, pair);
    JS_FreeValue(ctx, list);
}

void scroll_events_scroll_steps_end(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);
    JSValue firing = se_firing(ctx);

    /* Step 3, "Empty doc's pending scroll events", over the list the steps took. Written only when there is
       something to empty, for the reason the take gives. */
    if (se_len(ctx, firing) > 0)
        JS_SetPropertyStr(ctx, rec, SE_FIRING, JS_NewArray(ctx));
    JS_FreeValue(ctx, firing);
    JS_FreeValue(ctx, rec);
}

static void scroll_events_install(JSContext *ctx)
{
    JSValue rec = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(rec), "scroll_events: OOM building a realm's CSSOM VIEW §13.2 record");
    JS_SetPropertyStr(ctx, rec, SE_PENDING, JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, rec, SE_FIRING, JS_NewArray(ctx));
    realm_value_set(ctx, g_slot, rec);
}

void scroll_events_init(JSContext *ctx)
{
    DCHECK(g_slot == JS_INVALID_CLASS_ID, "scroll_events_init ran twice — the §13.2 record's slot is declared once per AGENT");
    g_slot = realm_value_declare(ctx, "CSSOM VIEW §13.2 a Document's pending scroll events, and the list the "
                                     "running scroll steps took from it");
    agent_state_realm_slot("scroll_events", &g_slot,
                           "CSSOM VIEW §13.2 Scrolling's realm-value slot for a Document's pending scroll events and "
                           "the list being fired");
    realm_declare_intrinsic(scroll_events_install);
}

void scroll_events_free(void)
{
    /* The records are the REALMS' — each is released with its context. What the agent holds is the slot, and
       it is a slot and not a reference, so there is nothing to free above this and the undo is the whole
       release. EVERY HANDLE THIS ROW DECLARED, GIVEN BACK FROM THE ONE LIST THAT ALREADY NAMES THEM — see
       core/agent_state.h's agent_state_undo for why the assignment that stood here is a second copy of the
       declaration rather than its inverse, even where the copy is one line long.
       LAST, AND THAT ORDER IS THE CONTRACT — trivially so here, nothing above reads g_slot. It is called BY
       this component rather than from core/platform.c's release column deliberately: a column that undid every
       component automatically would leave agent_state_check_released nothing to catch. */
    agent_state_undo("scroll_events");
}
