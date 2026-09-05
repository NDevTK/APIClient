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
#include "core/dom/scroll_events.h"
#include "core/events/event.h"
#include "core/realm.h"

/* The record's two fields ARE §13.2's two collections — see scroll_events.h for why they are two. */
#define SE_PENDING  "pending"
#define SE_SCROLLED "scrolled"
/* A pair is stored as the two-element Array §13.2 describes, so the pair stays atomic across a park: an
   (EventTarget, DOMString) split over two parallel lists is two facts that can be appended to unequally. */
#define SE_PAIR_TARGET 0
#define SE_PAIR_TYPE   1

static int g_slot = -1;

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

void scroll_events_viewport_scrollend(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);

    /* NOT AN APPEND. §3.1 step 7.1 makes the box one that WAS SCROLLED; §13.2's scroll steps step 1 is what
       turns that into a pair, on the next run of those steps. See scroll_events.h for why the two are not
       collapsed even though an instant scroll makes them one frame apart. */
    JS_SetPropertyStr(ctx, rec, SE_SCROLLED, JS_TRUE);
    JS_FreeValue(ctx, rec);
}

bool scroll_events_pending(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);
    JSValue list = JS_GetPropertyStr(ctx, rec, SE_PENDING);
    JSValue scrolled = JS_GetPropertyStr(ctx, rec, SE_SCROLLED);
    bool any;

    DCHECK(JS_IsBool(scrolled),
           "§13.2's was-scrolled field is not a boolean — §3.1's step 7.1 and the scroll steps' step 1 are its "
           "only writers and both write a boolean");
    any = se_len(ctx, list) > 0 || JS_ToBool(ctx, scrolled);
    JS_FreeValue(ctx, scrolled);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, rec);
    return any;
}

uint32_t scroll_events_scroll_steps_begin(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);
    JSValue scrolled = JS_GetPropertyStr(ctx, rec, SE_SCROLLED);
    JSValue list;
    uint32_t n;

    DCHECK(JS_IsBool(scrolled), "§13.2's was-scrolled field is not a boolean at the scroll steps' step 1");
    /* STEP 1 — "For each scrolling box box that was scrolled". In this build that set is the viewport or
       nothing (scroll_events.h), so the walk is this test; its step 1.1's target is the Document, its step 1.2
       is the snap-container step, and its steps 1.3 and 1.4 are the append's own two halves. */
    if (JS_ToBool(ctx, scrolled)) {
        se_no_snap_container("the scroll steps' step 1.2");
        se_append(ctx, document_object(ctx), "scrollend");
        /* THE SET IS EMPTIED HERE AND §13.2 DOES NOT SAY SO, which is a gap in the standard rather than a
           choice made here: step 3 empties the pending scroll events and names no other collection, so a set
           that survived would append a `scrollend` at every rendering opportunity for ever after one scroll.
           A box "that was scrolled" is one scrolled SINCE THESE STEPS LAST RAN, by the same reading §13.1's
           resize steps state outright for their own condition. */
        JS_SetPropertyStr(ctx, rec, SE_SCROLLED, JS_FALSE);
    }
    JS_FreeValue(ctx, scrolled);
    /* STEP 2's EXTENT, taken now — see scroll_events.h for why an index below it names one pair for the whole
       walk even though a listener may append during it. */
    list = JS_GetPropertyStr(ctx, rec, SE_PENDING);
    n = se_len(ctx, list);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, rec);
    return n;
}

void scroll_events_item(JSContext *ctx, uint32_t i, JSValue *target, JSValue *ev)
{
    JSValue list = se_list(ctx), pair, ty;
    const char *type;

    DCHECK(target != NULL && ev != NULL,
           "§13.2's scroll steps step 2 was asked for an item with nowhere to put it");
    DCHECK(i < se_len(ctx, list),
           "§13.2's scroll steps step 2 was asked for an item past the extent step 1 measured — the list only "
           "ever grows at its tail during the walk, so an index inside that extent cannot go out of range");
    pair = JS_GetPropertyUint32(ctx, list, i);
    *target = JS_GetPropertyUint32(ctx, pair, SE_PAIR_TARGET);
    ty = JS_GetPropertyUint32(ctx, pair, SE_PAIR_TYPE);
    type = JS_ToCString(ctx, ty);
    CHECK(type != NULL, "§13.2's pending scroll events holds a type that could not be read as a string");
    /* STEP 2's FOUR BRANCHES, and the first is the only one this build can reach. "If target is a `Document`,
       and type is `scroll` or `scrollend`, fire an event named type that BUBBLES at target." Its two
       SnapEvent branches are the snap-container steps asserted above; its last branch ("Otherwise, fire an
       event named type at target") is for an ELEMENT's own scroll, which needs an element that can hold a
       scroll position. So the assert below is two-sided against the arrival of either. */
    DCHECK(JS_IsSameValue(ctx, *target, document_object(ctx)) &&
           (strcmp(type, "scroll") == 0 || strcmp(type, "scrollend") == 0),
           "§13.2's scroll steps step 2 reached a pending scroll event whose target is not this Document or "
           "whose type is neither `scroll` nor `scrollend` — the three appends in this file are its only "
           "writers and each appends one of those two at the document, so an element's or a VisualViewport's "
           "pair has arrived and step 2's later branches must be written for it");
    /* CANCELABLE is false and BUBBLES is true: §13.2 says "fire an event named type that bubbles", and DOM
       §2.4's fire leaves every flag the caller does not name unset. TRUSTED, because the user agent fired it. */
    *ev = event_new(ctx, type, /*bubbles*/ true, /*cancelable*/ false);
    CHECK(!JS_IsException(*ev), "§13.2's scroll steps could not allocate their event");
    JS_FreeCString(ctx, type);
    JS_FreeValue(ctx, ty);
    JS_FreeValue(ctx, pair);
    JS_FreeValue(ctx, list);
}

void scroll_events_scroll_steps_end(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);
    JSValue list = JS_GetPropertyStr(ctx, rec, SE_PENDING);

    /* STEP 3 — "Empty doc's pending scroll events." — WRITTEN ONLY WHEN THERE IS SOMETHING TO EMPTY, and that
       is a load-bearing condition rather than a micro-optimisation. These steps run for EVERY document of
       EVERY rendering opportunity, because a document reaches step 9 for any of the reasons step 4 keeps it;
       an unconditional write here would therefore put one entry in EVERY live flow's COW delta once per frame
       per document, for a list that was already empty. Emptying an empty list is not observable, so not
       writing it is the same algorithm — which is the identical argument core/frame/viewport.c's §13.1 resize
       latch makes about re-latching an unchanged pair, and for the identical reason.
       A FRESH ARRAY rather than a length write, so a pair a listener appended DURING the walk goes with it:
       step 3 empties the list the standard says, and a pair that arrived after step 2 measured its extent is
       one step 2 did not iterate. */
    if (se_len(ctx, list) > 0)
        JS_SetPropertyStr(ctx, rec, SE_PENDING, JS_NewArray(ctx));
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, rec);
}

static void scroll_events_install(JSContext *ctx)
{
    JSValue rec = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(rec), "scroll_events: OOM building a realm's CSSOM VIEW §13.2 record");
    JS_SetPropertyStr(ctx, rec, SE_PENDING, JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, rec, SE_SCROLLED, JS_FALSE);
    realm_value_set(ctx, g_slot, rec);
}

void scroll_events_init(JSContext *ctx)
{
    DCHECK(g_slot < 0, "scroll_events_init ran twice — the §13.2 record's slot is declared once per AGENT");
    g_slot = realm_value_declare(ctx, "CSSOM VIEW §13.2 a Document's pending scroll events, and the set of "
                                     "scrolling boxes that were scrolled");
    agent_state_id("scroll_events", &g_slot,
                   "CSSOM VIEW §13.2 Scrolling's realm-value slot for a Document's pending scroll events and "
                   "its was-scrolled set");
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
