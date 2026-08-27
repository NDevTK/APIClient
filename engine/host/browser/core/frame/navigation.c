/* THE Navigation INTERFACE AND ITS ENTRY LIST — HTML §7.2.6.2, §7.2.6.3, §7.2.6.4 and §7.2.6.6. See
   navigation.h for what this component is and what of §7.2.6 is deliberately still absent. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/navigate_event.h"
#include "core/events/navigation_current_entry_change_event.h"
#include "core/frame/navigate_event_fire.h"
#include "core/frame/navigation.h"
#include "core/frame/navigation_history_entry.h"
#include "core/frame/session_history.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/structured_clone.h"

static JSValue   g_key;         /* the private Symbol the Navigation's own slots hang off */
static JSClassID g_nav_class;
static int       g_obj_slot = -1;
static int       g_id_entries = -1, g_id_update_current_entry = -1;

/* §7.2.6.3's TWO PIECES OF STATE, and TWO of §7.2.6.8's.
 *
 * "Each Navigation has an associated ENTRY LIST, a list of NavigationHistoryEntry objects, initially empty" and
 * "an associated CURRENT ENTRY INDEX, an integer, initially −1". They are an Array and a number on the
 * Navigation's own slot record rather than C fields, for the reason navigation.h gives: a property write is
 * what the per-flow COW delta captures, so two forked routers hold two entry lists with no new delta kind.
 *
 * §7.2.6.8's list of ongoing-navigation state has five entries and this build holds the two whose WRITERS are
 * built. FOCUS CHANGED is set by HTML §6.6.4's focus update steps, cleared by §8.1.7.3's update-the-rendering,
 * and cleared again by §7.2.6.8's abort the ongoing navigation step 3 (core/frame/navigation_abort.c). THE
 * ONGOING NAVIGATE EVENT is set by §7.2.6.10.4's inner algorithm, nulled by its commit handler success steps
 * (core/frame/navigate_event_fire.c), and nulled by abort a NavigateEvent step 2. The other three — the
 * suppress normal scroll restoration flag, the ongoing API method tracker and the map of upcoming traverse
 * trackers — belong to `intercept()` and to §7.2.6.7's methods, and a field with a writer and no reader is the
 * fabricated datum §Consumer-defaults describes, so each arrives with the algorithm that READS it. */
#define NAV_ENTRIES       "entryList"
#define NAV_CURRENT_INDEX "currentEntryIndex"
#define NAV_FOCUS_CHANGED "focusChanged"
/* §7.2.6.8's ONGOING NAVIGATE EVENT — the second of that list this build holds, and it is held for the same
   reason focus-changed is: every one of its writers is built. §7.2.6.10.4's inner algorithm sets it to the
   event it dispatches; its commit handler success steps null it out when the navigation succeeded, and
   §7.2.6.8's abort a NavigateEvent nulls it when the navigation ended any other way. */
#define NAV_ONGOING_EVENT "ongoingNavigateEvent"

/* ---- the object, and its slot record -------------------------------------------------------------------- */

JSValue navigation_object(JSContext *ctx)
{
    JSValue nav;

    DCHECK(g_obj_slot >= 0, "a Navigation was asked for before navigation_init declared the slot");
    nav = realm_value_get(ctx, g_obj_slot);
    DCHECK(JS_GetClassID(nav) == g_nav_class,
           "a realm answered for its §7.2.6.2 navigation API with something that is not a Navigation — the "
           "object is built with the realm by navigation_install_realm, before any page script runs");
    return nav;
}

/* The slot record of a Navigation. OWNED. */
static JSValue nav_slots(JSContext *ctx, JSValueConst nav)
{
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    JSValue slots;

    CHECK(k != JS_ATOM_NULL, "Navigation: the slot key could not be resolved to an atom");
    if (JS_GetOwnSlot(ctx, &slots, nav, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsObject(slots), "a Navigation carried no slot record — navigation_install_realm places one on "
                               "the object it builds, and nothing else mints a Navigation");
    return slots;
}

/* THIS REALM's entry list. OWNED. */
static JSValue nav_entry_list(JSContext *ctx)
{
    JSValue nav = navigation_object(ctx), slots = nav_slots(ctx, nav), list;

    JS_FreeValue(ctx, nav);
    list = JS_GetPropertyStr(ctx, slots, NAV_ENTRIES);
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsArray(list), "§7.2.6.3's entry list held something that is not a list");
    return list;
}

static uint32_t list_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static int64_t nav_current_index(JSContext *ctx)
{
    JSValue nav = navigation_object(ctx), slots = nav_slots(ctx, nav), v;
    int64_t i = 0;

    JS_FreeValue(ctx, nav);
    v = JS_GetPropertyStr(ctx, slots, NAV_CURRENT_INDEX);
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsNumber(v), "§7.2.6.3's current entry index held something that is not an integer — every "
                           "writer of the field is in this file and every one of them writes a number");
    JS_ToInt64(ctx, &i, v);
    JS_FreeValue(ctx, v);
    return i;
}

static void nav_set_current_index(JSContext *ctx, int64_t i)
{
    JSValue nav = navigation_object(ctx), slots = nav_slots(ctx, nav);

    JS_FreeValue(ctx, nav);
    JS_SetPropertyStr(ctx, slots, NAV_CURRENT_INDEX, JS_NewInt64(ctx, i));
    JS_FreeValue(ctx, slots);
}

/* ---- §7.2.6.3's "has entries and events disabled" ---------------------------------------------------------
 *
 * Verbatim, and every clause of it is about THIS realm's Document rather than about the Navigation: not fully
 * active, or the initial about:blank, or an OPAQUE ORIGIN. The three together are the states in which a
 * Document has no observable session history of its own — an entry list would be a list of entries nothing may
 * traverse to — so the standard answers `entries()` with the empty list and `currentEntry` with null rather
 * than building wrappers nobody can use.
 *
 * THE OPAQUE-ORIGIN CLAUSE ASKS WHETHER THE ORIGIN IS OPAQUE, which is a different question from §7.2.1's
 * same-origin check and used to be routed through it: while an origin was a serialization, "is this navigable
 * same origin with itself" was false in exactly the case where the origin was opaque, so the wrong predicate
 * gave the right answer. §7.1.1 step 1 says an origin IS same origin with itself, opaque included, so that
 * route now answers true and the clause asks the question it actually has (core/url/origin.h). */
bool navigation_entries_and_events_disabled(JSContext *ctx)
{
    if (!document_fully_active(ctx)) return true;
    if (session_history_is_initial_about_blank(ctx)) return true;
    if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx)))) return true;
    return false;
}

/* ---- §7.2.6.3's "current entry" and "get the navigation API entry index" ---------------------------------- */

/* §7.2.6.3's CURRENT ENTRY of this realm's Navigation: null when entries and events are disabled, otherwise
   entry list[current entry index]. OWNED — JS_NULL for the standard's null. */
static JSValue nav_current_entry(JSContext *ctx)
{
    JSValue list, e;
    int64_t i;

    if (navigation_entries_and_events_disabled(ctx)) return JS_NULL;
    i = nav_current_index(ctx);
    DCHECK(i != -1, "§7.2.6.3's current entry asserts the current entry index is not −1, and this Navigation's "
                    "is — the index is set away from −1 by initialize-the-navigation-API-entries-for-a-new-"
                    "document, which core/frame/session_history.c runs at the document's install, and entries "
                    "and events are disabled for every Document that has not reached it");
    list = nav_entry_list(ctx);
    DCHECK(i >= 0 && (uint32_t)i < list_len(ctx, list),
           "§7.2.6.3's current entry index names no entry — §7.2.6.4 is the only writer of both the index and "
           "the list, and it moves them together");
    e = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
    JS_FreeValue(ctx, list);
    return e;
}

int64_t navigation_entry_index_of(JSContext *ctx, JSValueConst she)
{
    JSValue list = nav_entry_list(ctx);
    uint32_t n = list_len(ctx, list), i;
    int64_t found = -1;

    DCHECK(JS_IsObject(she), "§7.2.6.3's get-the-navigation-API-entry-index was given something that is not a "
                             "§7.4.1.1 session history entry");
    for (i = 0; i < n; i++) {
        JSValue nhe = JS_GetPropertyUint32(ctx, list, i);
        JSValue e = navigation_history_entry_she(ctx, nhe);
        bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(she);

        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, nhe);
        if (same) { found = (int64_t)i; break; }
    }
    JS_FreeValue(ctx, list);
    return found;
}

/* ---- §7.2.6.4's INITIALIZE THE NAVIGATION API ENTRIES FOR A NEW DOCUMENT ---------------------------------- */

void navigation_initialize_entries(JSContext *ctx, JSValueConst new_shes, JSValueConst initial_she)
{
    JSValue list;
    uint32_t n, i;
    int64_t index;

    /* STEPS 1 AND 2 — the standard's own asserts, and they are what makes this the ONE moment in a Document's
       life the algorithm may run: an entry list that is already populated is a second initialization, which
       §7.2.6.4's other two entry points (update-for-reactivation and update-for-a-same-document-navigation)
       exist precisely to avoid. */
    list = nav_entry_list(ctx);
    DCHECK(list_len(ctx, list) == 0,
           "§7.2.6.4's initialize-the-navigation-API-entries-for-a-new-document asserts the entry list is "
           "empty and this one is not — a Document reaching it twice is a second install, and a Document "
           "coming BACK (out of bfcache) is §7.2.6.4's UPDATE THE NAVIGATION API ENTRIES FOR REACTIVATION "
           "instead, which matches the surviving wrappers against the new entries and fires `dispose` at the "
           "ones the history threw away while the Document was parked");
    JS_FreeValue(ctx, list);
    DCHECK(nav_current_index(ctx) == -1,
           "§7.2.6.4's initialize-the-navigation-API-entries-for-a-new-document asserts the current entry "
           "index is −1, and this Navigation's has already been set");
    /* STEP 3. A Document with entries and events disabled gets NO wrappers at all, which is what makes
       `navigation.entries()` the empty list on the initial about:blank and in an opaque-origin document
       forever after — the list is never built rather than built and hidden. */
    if (navigation_entries_and_events_disabled(ctx)) return;
    /* STEP 4: "For each newSHE of newSHEs: let newNHE be a new NavigationHistoryEntry created in the relevant
       realm of navigation; set newNHE's session history entry to newSHE; append newNHE to navigation's entry
       list." The realm is THIS one, which is why the wrapper is minted here and not by the caller. */
    list = nav_entry_list(ctx);
    n = list_len(ctx, new_shes);
    for (i = 0; i < n; i++) {
        JSValue she = JS_GetPropertyUint32(ctx, new_shes, i);
        JSValue nhe = navigation_history_entry_new(ctx, she);

        JS_FreeValue(ctx, she);
        JS_SetPropertyUint32(ctx, list, i, nhe);
    }
    JS_FreeValue(ctx, list);
    /* STEP 5. */
    index = navigation_entry_index_of(ctx, initial_she);
    DCHECK(index >= 0,
           "§7.2.6.4's initialize-the-navigation-API-entries-for-a-new-document set the current entry index "
           "from an entry the list it just built does not contain — newSHEs comes from §7.4.1.4's "
           "get-session-history-entries-for-the-navigation-API, whose first act is to append the entry at the "
           "starting index, so initialSHE is always one of them");
    nav_set_current_index(ctx, index);
}

/* ---- §7.2.6.4's UPDATE THE NAVIGATION API ENTRIES FOR A SAME-DOCUMENT NAVIGATION -------------------------- */

enum { NAV_UPD_BEGIN = 0, NAV_UPD_ENTRY_CHANGE, NAV_UPD_DISPOSE };

void navigation_update_work_start(NavigationUpdateWork *w)
{
    int k;

    /* THE SLOTS ARE UNDEFINED BEFORE THEY ARE ANYTHING ELSE — a zeroed JSValue is the INTEGER 0, not undefined
       (JS_TAG_INT is 0), so a slot read before it is written yields a real value the page can see. Same rule,
       same reason as core/streams/stream_work.h's. */
    w->stage = NAV_UPD_BEGIN;
    w->phase = 0;
    w->i = 0;
    w->disposed = w->ev = w->navigate_event = JS_UNDEFINED;
    STEP_CB_FOREACH(w->cb, k) w->cb[k] = JS_UNDEFINED;
}

void navigation_update_work_visit(JSContext *ctx, NavigationUpdateWork *w, JSStepVisit *v)
{
    int k;

    v->val(ctx, &w->disposed);
    v->val(ctx, &w->ev);
    v->val(ctx, &w->navigate_event);
    STEP_CB_FOREACH(w->cb, k) v->val(ctx, &w->cb[k]);
}

void navigation_update_work_release(JSContext *ctx, NavigationUpdateWork *w)
{
    int k;

    JS_FreeValue(ctx, w->disposed);
    JS_FreeValue(ctx, w->ev);
    JS_FreeValue(ctx, w->navigate_event);
    w->disposed = w->ev = w->navigate_event = JS_UNDEFINED;
    STEP_CB_FOREACH(w->cb, k) {
        JS_FreeValue(ctx, w->cb[k]);
        w->cb[k] = JS_UNDEFINED;
    }
}

int navigation_update_entries_run(JSContext *ctx, NavigationUpdateWork *w, JSValueConst destination_she,
                                  const char *navigation_type, JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValue nav, list = JS_UNDEFINED, old_current = JS_UNDEFINED;
    uint32_t n = 0;
    int r;

    DCHECK(navigation_type != NULL &&
           (!strcmp(navigation_type, "push") || !strcmp(navigation_type, "replace") ||
            !strcmp(navigation_type, "reload") || !strcmp(navigation_type, "traverse")),
           "§7.2.6.4's update-the-navigation-API-entries-for-a-same-document-navigation was given a "
           "NavigationType outside §7.2.6.3's enumeration");
    if (w->stage == NAV_UPD_ENTRY_CHANGE) goto fire_entry_change;
    if (w->stage == NAV_UPD_DISPOSE) goto fire_dispose;

    /* STEP 1. It is asked HERE and not by the caller because it is a fact about this Document that can change
       under a listener: a `dispose` handler that removes this frame makes the NEXT update a no-op, and the
       caller has no business knowing that. */
    if (navigation_entries_and_events_disabled(ctx)) {
        JS_FreeValue(ctx, in);
        return 0;
    }
    /* STEP 2 — read BEFORE the list moves, because §7.2.7.1's `from` is what was current and a page compares
       it against `navigation.currentEntry` afterwards. */
    old_current = nav_current_entry(ctx);
    DCHECK(JS_IsObject(old_current),
           "§7.2.6.4's oldCurrentNHE is null while entries and events are enabled — the two are the same "
           "condition, so a null here means the current entry index is −1 in a Document whose entries were "
           "initialized");
    /* STEP 3. */
    w->disposed = JS_NewArray(ctx);
    CHECK(!JS_IsException(w->disposed), "navigation: §7.2.6.4's disposedNHEs could not be allocated");
    list = nav_entry_list(ctx);
    n = list_len(ctx, list);

    if (!strcmp(navigation_type, "traverse")) {
        /* STEP 4. A traversal moves the INDEX and touches no wrapper: the entries it moves between are already
           in the list, which is exactly why the standard's own note says this algorithm is only ever called
           for SAME-DOCUMENT traversals — a cross-document one arrives through initialize-for-a-new-document or
           update-for-reactivation instead. */
        int64_t index = navigation_entry_index_of(ctx, destination_she);

        DCHECK(index != -1,
               "§7.2.6.4 step 4.2 asserts the current entry index is not −1 after a \"traverse\" — the entry "
               "the traversal arrived at is not in this Navigation's entry list, which means the list is out "
               "of step with §7.4.1's entries. §7.4.1.4's get-session-history-entries-for-the-navigation-API "
               "is what fills it, and only initialize-for-a-new-document and this algorithm may move it");
        nav_set_current_index(ctx, index);
    } else if (!strcmp(navigation_type, "push")) {
        /* STEP 5: the index moves forward one and EVERY entry from there to the end is disposed — which is
           §7.4.1.4's clear-the-forward-session-history seen from the navigation API's side. A page with an
           `ondispose` listener therefore runs code inside a `pushState` that followed a `back()`. */
        int64_t index = nav_current_index(ctx) + 1;
        uint32_t j, d = 0;

        nav_set_current_index(ctx, index);
        for (j = (uint32_t)index; j < n; j++, d++)
            JS_SetPropertyUint32(ctx, w->disposed, d, JS_GetPropertyUint32(ctx, list, j));
        /* "Remove all items in disposedNHEs from navigation's entry list" — over a contiguous tail, that is a
           truncation to the current entry index, and step 7 below then writes the new entry at exactly that
           position. */
        JS_SetPropertyStr(ctx, list, "length", JS_NewInt64(ctx, index));
    } else if (!strcmp(navigation_type, "replace")) {
        /* STEP 6. The entry that was current is disposed and step 7 puts its replacement at the same index —
           the wrapper is REPLACED, not mutated, because §7.4.4 built a whole new session history entry. */
        JS_SetPropertyUint32(ctx, w->disposed, 0, JS_DupValue(ctx, old_current));
    } else {
        /* A "reload": §7.2.6.4 falls through all three arms, and step 7's condition excludes it too, so the
           entry list and the index are untouched and the algorithm's whole effect is the events below. That
           is the standard's own shape and not an omission — §7.2.7.1's note says so ("If navigationType is
           null or 'reload', then this value will be the same as navigation.currentEntry"). */
        DCHECK(!strcmp(navigation_type, "reload"),
               "§7.2.6.4 reached its fall-through arm with a NavigationType that is not \"reload\"");
    }
    /* STEP 7. */
    if (!strcmp(navigation_type, "push") || !strcmp(navigation_type, "replace")) {
        JSValue nhe = navigation_history_entry_new(ctx, destination_she);
        int64_t index = nav_current_index(ctx);

        JS_SetPropertyUint32(ctx, list, (uint32_t)index, nhe);
    }
    JS_FreeValue(ctx, list);
    /* STEP 8 — "if navigation's ongoing API method tracker is non-null, then NOTIFY ABOUT THE COMMITTED-TO
       ENTRY" — is what carries the `committed` promise of a `navigation.navigate()` call, and §7.2.6.7's
       methods are the only producers of a tracker, so it is null and the step does nothing.
       STEP 10: "let navigateEvent be navigation's ONGOING NAVIGATE EVENT." READ HERE, USED AT STEP 14, and the
       standard's own note says why it is read this early: steps 12 and 13 fire `currententrychange` and then
       `dispose`, and "event handlers could start another navigation, or otherwise change the value of" what
       these steps are about. So the operation takes its input with it rather than reading it back four steps
       later. STEP 9's apiMethodTracker is the same read for the tracker, and there is none.
       STEP 11's PREPARE TO RUN SCRIPT has nothing to suppress in this engine, which is an answer about this
       agent rather than a shrug: its own note says it is there to stop the JavaScript execution context stack
       becoming empty and forcing a microtask checkpoint between the event handlers below and the promise
       handlers of §7.2.6.7's methods. This scheduler has no job-queue drain to trigger — every enqueued job is
       a flow in the one WFQ — so there is no checkpoint for an extra execution context to hold back. */
    w->navigate_event = navigation_ongoing_navigate_event(ctx);
    w->stage = NAV_UPD_ENTRY_CHANGE;
    /* STEP 12's event, minted before the dispatch and held across it. */
    w->ev = navigation_current_entry_change_event_new_to_fire(ctx, navigation_type, old_current);
    JS_FreeValue(ctx, old_current);
    old_current = JS_UNDEFINED;
    if (JS_IsException(w->ev)) {
        w->ev = JS_UNDEFINED;
        JS_FreeValue(ctx, in);
        return -1;
    }

fire_entry_change:
    nav = navigation_object(ctx);
    /* `currententrychange` is not cancelable, so §2.9's answer is discarded — the way DOM's fire-an-event
       discards it for every event no algorithm branches on. */
    r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), nav, w->ev, JS_UNDEFINED, in, NULL,
                              out_cb, out_argc);
    JS_FreeValue(ctx, nav);
    if (r > 0) return r;
    JS_FreeValue(ctx, w->ev);
    w->ev = JS_UNDEFINED;
    in = JS_UNDEFINED;
    w->stage = NAV_UPD_DISPOSE;

fire_dispose:
    /* STEP 13: "For each disposedNHE of disposedNHEs: fire an event named `dispose` at disposedNHE." ONE
       ENTRY PER TURN, because each dispatch runs the page's listeners: a walk that fired two of them between
       two rest points would be a stretch of this algorithm the scheduler cannot preempt inside, and a page
       that pushes over a long forward history disposes as many entries as it went back past. */
    for (;;) {
        uint32_t d = list_len(ctx, w->disposed);
        JSValue target;

        if (w->i >= d) break;
        if (JS_IsUndefined(w->ev)) {
            /* DOM's fire-an-event with neither of its two flags, because §7.2.6.4 sets none of them; TRUSTED,
               because the user agent fired it. There is no interface of its own: §7.2.6.5 declares `dispose`
               with no "using" clause, so it is a plain Event. */
            w->ev = event_new(ctx, "dispose", /*bubbles*/ false, /*cancelable*/ false);
            if (JS_IsException(w->ev)) { w->ev = JS_UNDEFINED; JS_FreeValue(ctx, in); return -1; }
        }
        target = JS_GetPropertyUint32(ctx, w->disposed, w->i);
        r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), target, w->ev, JS_UNDEFINED, in, NULL,
                                  out_cb, out_argc);
        JS_FreeValue(ctx, target);
        if (r > 0) return r;
        in = JS_UNDEFINED;
        JS_FreeValue(ctx, w->ev);
        w->ev = JS_UNDEFINED;
        w->i++;
    }
    /* STEP 14: "Run the NAVIGATE EVENT INTERCEPT COMMIT HANDLER STEPS given navigation, navigateEvent, and
       apiMethodTracker" — over the event step 10 took, not over whatever is ongoing now. They are what ENDS a
       navigate event: they null the Navigation's ongoing navigate event out and fire `navigatesuccess`, and
       they run in a microtask rather than here (core/frame/navigate_event_fire.c says why). The tracker is
       null, which those steps' step 6 is the only reader of.
       STEP 15's clean up after running script is step 11's other half. */
    /* THE FIELD IS A NavigateEvent OR NULL, so asking the BRAND asks both halves at once — and it is asked as
       an assertion rather than as an `if`, which would be this same invariant softened into a silent skip of
       the commit handler steps. */
    DCHECK(navigate_event_is(ctx, w->navigate_event),
           "§7.2.6.4 reached step 14 with NO ongoing navigate event — a same-document navigation got here "
           "without one being fired, and the only path in this build that can is a TRAVERSAL: "
           "core/frame/session_history.c's §7.4.6.1 step 5 still has to fire a TRAVERSE navigate event, and "
           "the assertion naming that work is at that site, which is reached first");
    navigate_event_intercept_commit(ctx, w->navigate_event);
    JS_FreeValue(ctx, in);
    return 0;
}

/* ---- §7.2.6.8's focus changed during ongoing navigation --------------------------------------------------- */

void navigation_set_focus_changed(JSContext *ctx, bool changed)
{
    JSValue nav = navigation_object(ctx), slots = nav_slots(ctx, nav);

    JS_FreeValue(ctx, nav);
    JS_SetPropertyStr(ctx, slots, NAV_FOCUS_CHANGED, JS_NewBool(ctx, changed));
    JS_FreeValue(ctx, slots);
}

/* ---- §7.2.6.8's ongoing navigate event ---------------------------------------------------------------------- */

JSValue navigation_ongoing_navigate_event(JSContext *ctx)
{
    JSValue nav = navigation_object(ctx), slots = nav_slots(ctx, nav), ev;

    JS_FreeValue(ctx, nav);
    ev = JS_GetPropertyStr(ctx, slots, NAV_ONGOING_EVENT);
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsNull(ev) || navigate_event_is(ctx, ev),
           "§7.2.6.8's ongoing navigate event held something that is neither a NavigateEvent nor null — the "
           "field starts at null with the Navigation and its only writers are §7.2.6.10.4's inner algorithm "
           "step 24, its commit handler success steps step 4, and §7.2.6.8's abort a NavigateEvent step 2");
    return ev;
}

void navigation_set_ongoing_navigate_event(JSContext *ctx, JSValueConst ev)
{
    JSValue nav = navigation_object(ctx), slots = nav_slots(ctx, nav);

    JS_FreeValue(ctx, nav);
    DCHECK(JS_IsNull(ev) || navigate_event_is(ctx, ev),
           "§7.2.6.8's ongoing navigate event was set to something that is neither a NavigateEvent nor null");
    JS_SetPropertyStr(ctx, slots, NAV_ONGOING_EVENT, JS_DupValue(ctx, ev));
    JS_FreeValue(ctx, slots);
}

/* ---- §7.2.6.6's members ------------------------------------------------------------------------------------ */

/* WEB IDL §3.7.5's BRAND, plus the SAME-REALM check core/frame/history.c makes for the same reason: a C member
   runs in the realm that DEFINED it, so a member pulled off THIS realm's Navigation.prototype and applied to
   ANOTHER realm's Navigation would read this document's entry list while wearing that document's object. Two
   same-origin documents are one heap, so that is a reachable state and not a hypothetical. */
static bool nav_brand(JSContext *ctx, JSValueConst this_val)
{
    JSValue own;
    bool same;

    DCHECK(g_nav_class != 0, "a Navigation member ran before navigation_init declared the class");
    if (JS_GetClassID(this_val) != g_nav_class) {
        JS_ThrowTypeError(ctx, "a Navigation member was reached on something that is not a Navigation");
        return false;
    }
    own = realm_value_get(ctx, g_obj_slot);
    same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);
    JS_FreeValue(ctx, own);
    DCHECK(same, "a Navigation member was reached on ONE realm's Navigation through ANOTHER realm's function — "
                 "every member reads the entry list of ITS OWN realm, so the answer would be a different "
                 "navigable's history wearing this document's Navigation. BUILD the Navigation that carries "
                 "its own realm: give the instance its realm as its class opaque (with the finalizer, gc_mark "
                 "and cow_capture_host_record contract that entails) so the member reads it off THIS");
    return true;
}

enum { NAV_CURRENT_ENTRY = 0, NAV_CAN_GO_BACK, NAV_CAN_GO_FORWARD, NAV_GETTERS };
static const char *const NAV_GETTER_NAME[] = { "currentEntry", "canGoBack", "canGoForward" };

static JSValue js_nav_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    int64_t i;

    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    switch (magic) {
    /* "The currentEntry getter steps are to return the current entry of this." */
    case NAV_CURRENT_ENTRY: return nav_current_entry(ctx);
    /* §7.2.6.6's canGoBack and canGoForward, whose first step is the disabled test and whose second is the
       standard's assert — and whose real content is one comparison each. They are NOT "is there a previous
       session history entry": the entry list holds only the SAME-ORIGIN CONTIGUOUS run around the current
       entry, so an index of 0 already means the entry before this one is another origin's or does not exist,
       which is what §7.2.6.6's prose spells out. */
    case NAV_CAN_GO_BACK:
        if (navigation_entries_and_events_disabled(ctx)) return JS_FALSE;
        i = nav_current_index(ctx);
        DCHECK(i != -1, "§7.2.6.6's canGoBack asserts the current entry index is not −1 while entries and "
                        "events are enabled");
        return JS_NewBool(ctx, i != 0);
    case NAV_CAN_GO_FORWARD: {
        JSValue list;
        uint32_t n;

        if (navigation_entries_and_events_disabled(ctx)) return JS_FALSE;
        i = nav_current_index(ctx);
        DCHECK(i != -1, "§7.2.6.6's canGoForward asserts the current entry index is not −1 while entries and "
                        "events are enabled");
        list = nav_entry_list(ctx);
        n = list_len(ctx, list);
        JS_FreeValue(ctx, list);
        return JS_NewBool(ctx, i != (int64_t)n - 1);
    }
    default:
        DFAIL("a Navigation accessor was installed with a magic no member of this file declares");
        return JS_UNDEFINED;
    }
}

/* §7.2.6.6's `sequence<NavigationHistoryEntry> entries()`.
 *
 * A NEW ARRAY EVERY CALL, and that is the TYPE rather than this body's choice: Web IDL's sequence conversion
 * builds a fresh JavaScript array from the list, which the standard's own note pins as `navigation.entries()
 * !== navigation.entries()`. Handing back the live list would also hand a page the ability to splice this
 * Navigation's state. */
static JSValue js_nav_entries(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue out, list;
    uint32_t n, i;

    (void)argc; (void)argv; (void)magic;
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) return out;
    /* STEP 1: "If this has entries and events disabled, then return the empty list." */
    if (navigation_entries_and_events_disabled(ctx)) return out;
    list = nav_entry_list(ctx);
    n = list_len(ctx, list);
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, i, JS_GetPropertyUint32(ctx, list, i));
    JS_FreeValue(ctx, list);
    return out;
}

/* ---- §7.2.6.6's updateCurrentEntry(options) ----------------------------------------------------------------
 *
 * IT IS THE ONE WAY TO WRITE navigation API STATE WITHOUT NAVIGATING, and the standard says what it is for:
 * "This method is best used to capture updates to the page that have already happened, and need to be
 * reflected into the navigation API state." A router that keeps scroll offsets or a form's draft in the entry
 * uses it on every input event.
 *
 * IT IS A STEP MACHINE BECAUSE OF ITS LAST STEP. Step 5 fires `currententrychange` at the Navigation, which is
 * a §2.9 dispatch and therefore the page's own listeners — so the member parks on it and resumes with the
 * algorithm finished, exactly as every other member that dispatches does. */
#define UCE_STAGES(X)                                                                                     \
    X(UCE_STATE, "HTML §7.2.6.6 updateCurrentEntry(options) steps 1-4 (the current entry, "                \
                 "StructuredSerializeForStorage(options[\"state\"]), and setting it as that entry's "      \
                 "session history entry's navigation API state)")                                          \
    X(UCE_FIRE,  "HTML §7.2.6.6 updateCurrentEntry(options) step 5 (fire an event named currententrychange "\
                 "at this using NavigationCurrentEntryChangeEvent)")
enum { IDL_STEP_STAGE_BASE(UCE_STAGES) UCE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UCE_STEPS[] = { UCE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t     phase;
    JSValue     ev;    /* owned across the dispatch */
    EventFireCb cb;
} UceState;

static void uce_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    UceState *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static int js_nav_update_current_entry(JSContext *ctx, JSStepHdr *hdr, void *state, int argc,
                                       JSValueConst *argv, JSValue cb_result, JSValue *presult,
                                       JSValue **out_cb, int *out_argc)
{
    UceState *s = state;
    JSValue nav;
    int r;

    if (hdr->stage == UCE_FIRE) goto fire;

    if (!nav_brand(ctx, hdr->this_val)) { JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
    DCHECK(argc >= 1, "§7.2.6.6's updateCurrentEntry ran with no argument — `NavigationUpdateCurrentEntryOptions "
                      "options` is a REQUIRED dictionary argument, so §3.6's arity check answered that "
                      "before this body was entered");
    {
        JSValue current = nav_current_entry(ctx), she, st_v;
        StructuredData d;

        /* STEPS 1 AND 2: "If current is null, then throw an 'InvalidStateError' DOMException." A page reaches
           it deliberately — a detached iframe's `navigation.updateCurrentEntry({state:x})` — which is why it
           is a throw and not an assert. */
        if (JS_IsNull(current)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "this Navigation has no current entry to update");
            return JS_STEP_ABRUPT;
        }
        /* STEP 3: "Let serializedState be StructuredSerializeForStorage(options['state']), rethrowing any
           exceptions." §2.7's ForStorage variant differs from the plain one in refusing a SharedArrayBuffer,
           and this engine has no SharedArrayBuffer at all (core/structured_clone.c), so the two coincide.
           `required any state` — the member is required, so the declaration has already refused a dictionary
           without it and this read cannot be absent. */
        st_v = idl_dict_get(ctx, argv[0], "state");
        if (structured_serialize(ctx, st_v, &d) < 0) {
            JS_FreeValue(ctx, st_v);
            JS_FreeValue(ctx, current);
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_ABRUPT;
        }
        JS_FreeValue(ctx, st_v);
        /* STEP 4: "Set current's session history entry's navigation API state to serializedState." */
        she = navigation_history_entry_she(ctx, current);
        session_history_entry_set_nav_state(ctx, she, &d);
        JS_FreeValue(ctx, she);
        structured_data_free(ctx, &d);
        /* STEP 5's event: navigationType NULL — which is the whole signal a listener reads to tell this apart
           from a navigation — and `from` the entry that is STILL current, because this method moves nothing. */
        s->ev = navigation_current_entry_change_event_new_to_fire(ctx, NULL, current);
        JS_FreeValue(ctx, current);
        if (JS_IsException(s->ev)) {
            s->ev = JS_UNDEFINED;
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_ABRUPT;
        }
    }
    hdr->stage = UCE_FIRE;

fire:
    nav = navigation_object(ctx);
    r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), nav, s->ev, JS_UNDEFINED, cb_result, NULL,
                              out_cb, out_argc);
    JS_FreeValue(ctx, nav);
    if (r > 0) return r;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl UCE_DECL = {
    js_nav_update_current_entry, sizeof(UceState), uce_visit, NULL,
    "HTML §7.2.6.6 updateCurrentEntry(options)", UCE_STEPS
};

/* ---- declaration and install ------------------------------------------------------------------------------- */

/* §7.2.2's `[Replaceable] readonly attribute Navigation navigation` — an accessor until a page assigns to it
   and a plain data property afterwards, which is what Replaceable means. */
static JSValue js_win_navigation(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return navigation_object(ctx);
}

void navigation_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, nav, slots, entries;

    prev = JS_GetClassProto(ctx, g_nav_class);
    DCHECK(JS_IsNull(prev), "navigation_install_realm ran twice in one realm — everything already holding the "
                            "first Navigation would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Navigation.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Navigation");
    /* `interface Navigation : EventTarget` — a real prototype chain, so a page's
       `navigation.addEventListener('currententrychange', f)` is §2.7's registration. */
    event_target_chain(ctx, proto);
    idl_install_method(ctx, proto, "entries", 0, g_id_entries);
    idl_install_accessor(ctx, proto, NAV_GETTER_NAME[NAV_CURRENT_ENTRY], js_nav_get, NAV_CURRENT_ENTRY, -1);
    idl_install_method(ctx, proto, "updateCurrentEntry", 1, g_id_update_current_entry);
    idl_install_accessor(ctx, proto, NAV_GETTER_NAME[NAV_CAN_GO_BACK], js_nav_get, NAV_CAN_GO_BACK, -1);
    idl_install_accessor(ctx, proto, NAV_GETTER_NAME[NAV_CAN_GO_FORWARD], js_nav_get, NAV_CAN_GO_FORWARD, -1);
    /* §7.2.6.2's event handler IDL attributes, declared ON this interface. THREE of the four are installed:
       `oncurrententrychange`, and now `onnavigate` and `onnavigatesuccess` — core/frame/navigate_event_fire.c
       dispatches both of those events, and an event handler attribute is installed exactly when something
       fires the event it handles. `onnavigateerror` is still ABSENT: its event is fired by §7.2.6.8's ABORT A
       NavigateEvent, which is the algorithm the two DFAILs in that file name, and a handler attribute for an
       event nothing dispatches is the shape-only member NO STUBS forbids. */
    event_target_install_handlers(ctx, proto, EH_NAVIGATION);
    JS_SetClassProto(ctx, g_nav_class, JS_DupValue(ctx, proto));

    nav = JS_NewObjectProtoClass(ctx, proto, g_nav_class);
    CHECK(!JS_IsException(nav), "the Window's associated Navigation could not be allocated");
    slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(slots), "the Navigation's slot record could not be allocated");
    entries = JS_NewArray(ctx);
    CHECK(!JS_IsException(entries), "the Navigation's §7.2.6.3 entry list could not be allocated");
    JS_SetPropertyStr(ctx, slots, NAV_ENTRIES, entries);
    JS_SetPropertyStr(ctx, slots, NAV_CURRENT_INDEX, JS_NewInt64(ctx, -1));
    JS_SetPropertyStr(ctx, slots, NAV_FOCUS_CHANGED, JS_FALSE);
    /* §7.2.6.8: "ongoing navigate event, a NavigateEvent or NULL, INITIALLY NULL" — written here rather than
       left absent, because the standard's null is JS_NULL and an unwritten slot answers undefined, which is a
       third state neither the field nor its readers have. */
    JS_SetPropertyStr(ctx, slots, NAV_ONGOING_EVENT, JS_NULL);
    {
        JSAtom k = JS_ValueToAtom(ctx, g_key);

        CHECK(k != JS_ATOM_NULL, "Navigation: the slot key could not be resolved to an atom");
        JS_SetProperty(ctx, nav, k, slots);
        JS_FreeAtom(ctx, k);
    }
    realm_value_set(ctx, g_obj_slot, nav);

    global = JS_GetGlobalObject(ctx);
    /* §3.7.1's INTERFACE OBJECT. Navigation declares no constructor, so `new Navigation()` is a TypeError —
       and its PRESENCE is what a feature-detecting bundle reads before it touches `window.navigation`. */
    JS_SetPropertyStr(ctx, global, "Navigation", idl_interface_object(ctx, "Navigation", proto));
    idl_install_replaceable(ctx, global, "navigation", js_win_navigation, 0);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void navigation_init(JSContext *ctx)
{
    JSClassDef d = { "Navigation" };

    DCHECK(g_obj_slot < 0, "navigation_init ran twice — the class, the slot and the member declarations are "
                           "made once per AGENT");
    g_key = JS_NewSymbol(ctx, "navigationSlots", false);
    CHECK(!JS_IsException(g_key), "the Navigation slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_nav_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_nav_class, &d) == 0,
          "Navigation: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "HTML §7.2.6.2 the Window's associated navigation API");
    g_id_entries = idl_method_id(ctx, NULL, 0, js_nav_entries, 0);
    /* §7.2.6.6's `undefined updateCurrentEntry(NavigationUpdateCurrentEntryOptions options)` — the dictionary
       is NOT optional and its `state` member is REQUIRED, so `navigation.updateCurrentEntry()` and
       `navigation.updateCurrentEntry({})` are both TypeErrors from the declaration. */
    {
        static const IdlArgType ARGS[] = { IDL_DICT };
        static const IdlDictMember OPTIONS[] = { { "state", IDL_ANY, /*required*/ true } };

        g_id_update_current_entry = idl_method_id_step(ctx, ARGS, 1, OPTIONS,
                                                       (int)(sizeof(OPTIONS) / sizeof(OPTIONS[0])),
                                                       &UCE_DECL, 0);
    }
    realm_declare_intrinsic(navigation_install_realm);
}

void navigation_free(JSContext *ctx)
{
    /* The prototypes, the interface objects and the Navigation objects are the REALMS' — each is released with
       its context. The Symbol is the AGENT's and is a runtime-lifetime value this component minted, so it is
       released here: a component that mints one and does not free it leaks it from every instance, which is
       what JS_FreeRuntime's gc_obj_list walk reports and what core/events/event_target.c was caught by. */
    JS_FreeValue(ctx, g_key);
    g_key = JS_UNDEFINED;
    g_obj_slot = -1;
    g_id_entries = g_id_update_current_entry = -1;
}
