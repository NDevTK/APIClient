/* COOKIE STORE API §7.4 "Process changes" — see process_changes.h for which standard this is, why §7.4 is its
 * own component and which of its two arms is here. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/cookie_store/cookie_store.h"
#include "core/cookie_store/process_changes.h"
#include "core/events/cookie_change_event.h"
#include "core/events/event_target.h"
#include "core/frame/navigable.h"
#include "core/frame/window_proxy.h"
#include "core/loader/cookie_jar.h"
#include "core/timing/task_source.h"
#include "core/url/url.h"

static int g_claimed;

/* The length of a JS Array — the navigable walk's list, or one of §7.4's two change sets. */
static uint32_t pc_len(JSContext *ctx, JSValueConst a)
{
    JSValue len = JS_GetPropertyStr(ctx, a, "length");
    uint32_t n = 0;

    CHECK(JS_ToUint32(ctx, &n, len) >= 0, "a §7.4 list has no length");
    JS_FreeValue(ctx, len);
    return n;
}

/* §7.4's `To prepare lists from changes`, whole. EACH STEP IS QUOTED ON ITS OWN AND THE NESTING IS NOT
 * FLATTENED, because this algorithm's steps 3.2 and 3.3 are an if/otherwise pair and step 3.3 has two sub-steps
 * of its own — a run welding them into one sentence is a quotation of no section, and it is exactly the
 * mis-transcription CLAUDE.md rates as the cheapest tell that the reasoning beside it flattened the same
 * structure:
 *   1. "Let changedList be « »."
 *   2. "Let deletedList be « »."
 *   3. "For each change in changes, run these steps:"
 *   3.1. "Let item be the result of running create a CookieListItem from change's cookie."
 *   3.2. "If change's type is changed, then append item to changedList."
 *   3.3. "Otherwise, run these steps:"
 *   3.3.1. "Set item["value"] to undefined."
 *   3.3.2. "Append item to deletedList."
 *   4. "Return changedList and deletedList."
 * `changes` is core/loader/cookie_jar.h's « name, value, type » list, already filtered to ONE Window's
 * observable changes. `*out_changed` and `*out_deleted` are OWNED on return.
 *
 * STEP 3.3.1 SETS THE PROPERTY, IT DOES NOT REMOVE IT. "Set item["value"] to undefined" writes an entry into the
 * ordered map §7.1 "Query cookies"' create-a-CookieListItem returned, so the member is PRESENT and its value is
 * undefined — `"value" in item` stays true and `item.value` reads undefined. Deleting it instead would make a
 * deleted item and a `getAll()` item differ in their SHAPE rather than in one value, which is a distinction the
 * standard draws nowhere and which a page tests with `in`.
 * AND IT IS THE REASON A DELETED ITEM CARRIES NO CONCOLIC VALUE WITHOUT THAT BEING A LOSS: the value this write
 * replaces is external input, and what replaces it is the standard's own undefined rather than a placeholder
 * this engine chose — so there is no unknown here to lose. The NAME is untouched on both arms. */
static void pc_prepare_lists(JSContext *ctx, JSValueConst changes, JSValue *out_changed, JSValue *out_deleted)
{
    uint32_t i, n = pc_len(ctx, changes);
    uint32_t nchanged = 0, ndeleted = 0;

    *out_changed = JS_NewArray(ctx);          /* STEP 1 */
    *out_deleted = JS_NewArray(ctx);          /* STEP 2 */
    CHECK(!JS_IsException(*out_changed) && !JS_IsException(*out_deleted),
          "OOM building §7.4's changedList and deletedList");
    for (i = 0; i < n; i++) {                 /* STEP 3 */
        JSValue change = JS_GetPropertyUint32(ctx, changes, i);
        JSValue tv, item;
        int32_t type = 0;

        CHECK(JS_IsArray(change), "a member of §7.4's observable changes is not a « name, value, type » record");
        tv = JS_GetPropertyUint32(ctx, change, 2);
        CHECK(JS_ToInt32(ctx, &type, tv) >= 0, "a §7.4 observable change carries no type");
        JS_FreeValue(ctx, tv);
        /* STEP 3.1: "let item be the result of running create a CookieListItem from change's cookie" — §7.1's
           algorithm, which this component reaches rather than restates (cookie_store.h). */
        item = cookie_store_list_item(ctx, change);
        JS_FreeValue(ctx, change);
        CHECK(!JS_IsException(item), "OOM building a §7.4 CookieListItem");
        if (type == COOKIE_CHANGE_CHANGED) {  /* STEP 3.2 */
            CHECK(JS_DefinePropertyValueUint32(ctx, *out_changed, nchanged++, item, JS_PROP_C_W_E) >= 0,
                  "§7.4's changedList refused an item");
        } else {                              /* STEP 3.3 */
            DCHECK(type == COOKIE_CHANGE_DELETED,
                   "a §7.4 observable change is neither of that section's two types — the enumeration is "
                   "core/loader/cookie_jar.h's and its only writer is that store");
            CHECK(JS_SetPropertyStr(ctx, item, "value", JS_UNDEFINED) >= 0,   /* STEP 3.3.1 */
                  "a deleted §7.4 CookieListItem refused to have its value unset");
            CHECK(JS_DefinePropertyValueUint32(ctx, *out_deleted, ndeleted++, item, JS_PROP_C_W_E) >= 0,
                  "§7.4's deletedList refused an item");
        }
    }
}

/* §7.4's `To fire a change event named type with changes at target` — steps 4 to 7. Steps 1 to 3 and steps 5
 * and 6 are the interface's, in core/events/cookie_change_event.c, which is where §5.1 "The CookieChangeEvent
 * interface"' contract about those two slots lives.
 *
 * STEP 7's "Dispatch event at target" IS QUEUED AND NOT PERFORMED, BECAUSE THE CALLER'S OWN STEP QUEUES IT.
 * §7.4 step 1.4 is "Queue a global task on the DOM manipulation task source given window to fire a change event
 * named "change" with changes at window's CookieStore", so the whole of fire-a-change-event is inside that task
 * — which is precisely the caller core/events/event_target.h's queued reach declares itself to be for, and for
 * no other: a caller whose own standard says to QUEUE a task in order to fire an event. That sentence is
 * REPORTED rather than quoted, for the reason cookie_store.h gives about un-numbered matter and CLAUDE.md gives
 * about this tree's own prose — a run in quotation marks beside a §7.4 citation is judged AGAINST §7.4, so
 * quoting another file here manufactures a fabricated-quotation finding at a comment that is right.
 * THE TASK IS ENQUEUED IN THE TARGET'S REALM, which is what HTML §8.1.7.2 "Queuing tasks"' queue-a-global-task
 * says: the task belongs to the receiving document, so HTML §7.5.10 "Destroying documents"' step 5 removal of a
 * destroyed document's tasks reaches it.
 * `changes` is BORROWED. */
static void pc_fire_change_event(JSContext *ctx, JSValueConst changes, JSValueConst target)
{
    JSValue changed, deleted, ev;

    pc_prepare_lists(ctx, changes, &changed, &deleted);           /* STEP 4 */
    ev = cookie_change_event_new_to_fire(ctx, changed, deleted);  /* STEPS 1-3, 5-6; CONSUMES both */
    CHECK(!JS_IsException(ev),
          "Cookie Store API §7.4's CookieChangeEvent could not be allocated — a dropped change event is a page "
          "that never learns a cookie it is watching moved");
    event_target_fire(ctx, target, ev, JS_UNDEFINED, TASK_SOURCE_DOM_MANIPULATION);   /* STEP 7; CONSUMES ev */
}

/* §7.4's `To process cookie changes`, step 1 — "For every Window window, run the following steps."
 *
 * THE WINDOWS ARE THIS AGENT'S NAVIGABLE TREE, which is the same set by construction rather than by a filter.
 * CLAUDE.md §Security keys one instance on an ORIGIN-KEYED AGENT CLUSTER, so every Window this engine holds is
 * one origin's; a different-origin document is a PEER instance whose cookie changes belong to that instance's
 * own store, and core/frame/navigable.h's walk already reports neither a peer's navigable nor one this instance
 * has not materialized. The jar asserts the consequence rather than trusting it: cookie_jar_observable_changes
 * routes to the same same-principal predicate every other public entry there does, so a Window from a second
 * principal reaching this walk aborts at the store naming the cross-instance read to build.
 *
 * STEP 2 IS NOT HERE. NAMED RESIDUAL. NOT COVERED: §7.4's step 2, "For every service worker registration
 * registration", which collects the changes matching that registration's cookie change subscription list and
 * fires a functional event named "cookiechange" using §5.2 "The ExtendableCookieChangeEvent interface". So a
 * service worker cannot be told a cookie changed, and neither can a Window through §4 "The CookieStoreManager
 * interface"' subscriptions. NEXT DIFF: `ServiceWorkerRegistration` — an interface this engine does not have at
 * all, whose name occurs in three declaration tables (browser/idl_exposure.h, browser/idl_inheritance.h,
 * browser/platform_names.h) and in no component — because the `cookies` attribute §4.4
 * "The ServiceWorkerRegistration interface" declares is the ONLY route to a CookieStoreManager from anywhere,
 * Window included. HOW ITS ABSENCE WOULD SHOW: a reader would observe that a cookie written while a
 * service worker holds a subscription dispatches nothing to it, and that `navigator.serviceWorker` offers no
 * registration to read `cookies` off in the first place.
 * WHY NOTHING OF THAT ARM MAY LAND FIRST, AND THE OBVIOUS REASON IS THE WRONG ONE. `CookieStoreManager` is NOT
 * service-worker-only — its IDL is `[Exposed=(ServiceWorker,Window), SecureContext]` and the `partial interface`
 * §4.4 declares carries `[Exposed=(ServiceWorker,Window)]` too, so a Window reaches one through a registration,
 * and an argument that no Window can is refuted by one read of the IDL. What is true is
 * §A-FIELD-A-CONSUMER-DEFAULTS':
 * §4.1 "The subscribe() method" writes a subscription list whose ONLY consumer is the step 2 above, and step 2's
 * event is an interface that does not exist — so subscriptions would be a producer with no reader and step 2
 * would be a reader with no event, and the two halves of that defect conceal each other. */
static void pc_process_cookie_changes(JSContext *ctx)
{
    JSValue all = navigable_tree_order(ctx);
    uint32_t i, n = pc_len(ctx, all);

    for (i = 0; i < n; i++) {
        JSValue proxy = JS_GetPropertyUint32(ctx, all, i);
        JSContext *docctx = window_proxy_realm(ctx, proxy);
        UrlRecord uri;
        JSValue changes, target;

        DCHECK(docctx != NULL, "a navigable in this agent's tree order answered with no realm — the walk reports "
                               "only materialized ones, and a materialized navigable has a realm");
        /* STEP 1.1: "Let url be window's relevant settings object's creation URL." A Window with no usable
           request-uri has an empty observable change set rather than a filtered one — cookie_store.h states why
           that is a positive answer. */
        if (!cookie_store_request_uri(docctx, &uri)) {
            url_record_free(&uri);
            JS_FreeValue(ctx, proxy);
            continue;
        }
        /* STEP 1.2: "Let changes be the observable changes for url." */
        changes = cookie_jar_observable_changes(docctx, &uri);
        url_record_free(&uri);
        CHECK(!JS_IsException(changes), "OOM building §7.4's observable changes for a Window");
        /* STEP 1.3: "If changes is empty, then continue." */
        if (pc_len(docctx, changes) == 0) {
            JS_FreeValue(docctx, changes);
            JS_FreeValue(ctx, proxy);
            continue;
        }
        /* STEP 1.4, at "window's CookieStore". */
        target = cookie_store_of_realm(docctx);
        pc_fire_change_event(docctx, changes, target);
        JS_FreeValue(docctx, target);
        JS_FreeValue(docctx, changes);
        /* THE PROXY IS HELD FOR THE WHOLE ITERATION, on every path — core/timing/timer.c's walk over this same
           list does the same. `docctx` is the navigable's realm rather than a derivation of this reference, and
           releasing the reference first would be this walk relying on that. */
        JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, all);
}

void cookie_store_process_changes_init(void)
{
    DCHECK(!g_claimed, "cookie_store_process_changes_init ran twice — §2.2's process-cookie-changes steps are "
                       "claimed once per AGENT, from the one declaration pass");
    g_claimed = 1;
    cookie_jar_set_process_changes(pc_process_cookie_changes);
    /* core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for §7.4 is
       core/platform.c's `cookie_store` row — cookie_store_init calls this init and cookie_store_free calls this
       release. The latch is the only slot this component has; the CLAIM itself is the jar's field and is given
       back by the release below rather than by that row's undo, because only this component knows it made one. */
    agent_state_flag("cookie_store", &g_claimed,
                     "Cookie Store API §7.4 Process changes' claim on §2.2's process-cookie-changes steps");
}

void cookie_store_process_changes_free(void)
{
    /* NOT `if (!g_claimed) return;`. cookie_store.c's init calls this component's init on the ONE declaration
       pass and its free — which has already asserted its own latch — calls this release unconditionally, so the
       test could never be true and what it could do is hide a release that left the claim standing. */
    DCHECK(g_claimed, "Cookie Store API §7.4 was released in an agent that never claimed §2.2's steps");
    cookie_jar_set_process_changes(NULL);
    g_claimed = 0;
    /* AND THE LAST THING IS TO SAY THE CASCADE GOT HERE, which is what `cookie_store`'s undo asserts on and
       what this release did not do. core/agent_state.h states the rule at its own site: the undoing file is
       exempt because calling the undo IS its claim, so `every declaring file must have spoken` — and this file
       DECLARES under that row from the init above while cookie_store_free's agent_state_undo runs in a
       different translation unit, so the exemption is not this file's to take.
       IT IS AFTER THE HAND-BACK AND NOT BEFORE IT. The statement is that the release RAN, so a line that could
       still abort must stand in front of it; placed first it would mark the slot reached and then fail, leaving
       a run whose next agent reads a claim nothing gave back as one that was.
       THE ROW IS SPELLED AS A LITERAL HERE BECAUSE `CS_COMPONENT` IS A `#define` IN cookie_store.c RATHER THAN
       IN ANY HEADER, so it is not in scope in this file and the init eight lines up spells the row the same
       way. Two spellings of one row is a state the registry refuses at runtime in both directions rather than
       by construction, and agent_state.h argues at its own site why no spelling scheme can check the claim a
       sub-component's row name makes. */
    agent_state_reached("cookie_store");
}
