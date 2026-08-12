/* USER ACTIVATION — HTML §6.4. See user_activation.h for why this is state and not a constant. */
#include <math.h>
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/frame/window_proxy.h"
#include "core/html/html_iframe.h"
#include "core/html/user_activation.h"
#include "core/timing/timer.h"

/* §6.4.1's TWO PER-WINDOW VALUES, and the initial value of each is the whole of what "never activated" means.
 *
 * A LAST ACTIVATION TIMESTAMP is "either a DOMHighResTimeStamp, positive infinity (indicating that W has never
 * been activated), or negative infinity (indicating that the activation has been consumed). Initially positive
 * infinity." Three states in ONE double, and the arithmetic is the reason: every question §6.4.1 asks is a
 * COMPARISON against it, and both infinities answer all three comparisons correctly with no case analysis at
 * all. Never-activated is a moment in the infinite future, so "now is at or past it" is false — not sticky, not
 * transient. Consumed is a moment in the infinite past, so "now is at or past it" is TRUE (sticky survives a
 * consumption, which is exactly what §6.4.1 says) while "now is before it plus the duration" is false, because
 * negative infinity plus anything finite is still negative infinity — not transient. A `bool consumed` field
 * beside a timestamp would have been the same three states with two of them able to disagree.
 *
 * A LAST HISTORY-ACTION ACTIVATION TIMESTAMP is "either a DOMHighResTimeStamp or positive infinity, initially
 * positive infinity", and it is never compared against the clock: §6.4.1's history-action activation is the
 * two timestamps being UNEQUAL. Both start at positive infinity, so a Window nothing has touched has no
 * history-action activation; a notification moves one and not the other, so it acquires one; the consumption
 * copies one onto the other, so it loses it again with no time limit anywhere in the question. */
#define UA_LAST    "lastActivation"
#define UA_HISTORY "lastHistoryActionActivation"

/* §6.4.1's TRANSIENT ACTIVATION DURATION — "a constant number indicating how long a user activation is
   available for certain user activation-gated APIs", which the standard leaves to the user agent and bounds
   only by intent: "expected to be at most a few seconds, so that the user can possibly perceive the link
   between an interaction with the page and the page calling the activation-gated API". This user agent's is
   five seconds. That is also what Chrome ships, which is confirmation and not the source — the source is the
   sentence above, and five seconds is the largest value that still reads as "a few". */
#define UA_TRANSIENT_ACTIVATION_DURATION_MS 5000.0

static int g_slot = -1;

/* THIS REALM'S §6.4.1 RECORD. Owned — the caller frees. */
static JSValue ua_record(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);

    DCHECK(JS_IsObject(rec), "a realm answered for its Window's §6.4.1 user activation state with no record");
    return rec;
}

static double ua_get(JSContext *ctx, const char *field)
{
    JSValue rec = ua_record(ctx), v = JS_GetPropertyStr(ctx, rec, field);
    double d = 0;

    DCHECK(JS_IsNumber(v), "a §6.4.1 activation timestamp is not a number — the record holds two "
                           "DOMHighResTimeStamps and the two infinities, and nothing else ever writes it");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    return d;
}

/* WRITTEN INTO THE REALM WHOSE WINDOW IT IS, which is why every writer below takes a realm rather than acting
   on the asking one: §6.4.2's notification and consumptions each set the timestamps of a SET of Windows, and
   every one of them is a different realm in this agent. The write is an ordinary property write, so the heap
   COW captures it into the running flow's delta — one arm consuming an activation leaves its sibling's. */
static void ua_set(JSContext *rctx, const char *field, double v)
{
    JSValue rec = ua_record(rctx);

    JS_SetPropertyStr(rctx, rec, field, JS_NewFloat64(rctx, v));
    JS_FreeValue(rctx, rec);
}

/* §6.4.1's "current high resolution time given W" — the event loop's one virtual clock (timer.h). A second
   time source would order an activation against the tasks that observe it differently from the queue that ran
   them, which is the one thing that clock exists to keep consistent. */
static double ua_now(void)
{
    return timer_now();
}

bool user_activation_sticky(JSContext *ctx)
{
    /* "When the current high resolution time given W is greater than or equal to the last activation timestamp
       in W, W is said to have sticky activation." */
    return ua_now() >= ua_get(ctx, UA_LAST);
}

bool user_activation_transient(JSContext *ctx)
{
    /* "... greater than or equal to the last activation timestamp in W, and less than the last activation
       timestamp in W plus the transient activation duration". */
    double last = ua_get(ctx, UA_LAST), now = ua_now();

    return now >= last && now < last + UA_TRANSIENT_ACTIVATION_DURATION_MS;
}

bool user_activation_history_action(JSContext *ctx)
{
    /* "When the last history-action activation timestamp of W is not equal to the last activation timestamp of
       W, then W is said to have history-action activation." */
    return ua_get(ctx, UA_HISTORY) != ua_get(ctx, UA_LAST);
}

/* ---- §6.4.2's PROCESSING MODEL ------------------------------------------------------------------------------
 *
 * THE TWO WALKS ARE DIFFERENT SETS ON PURPOSE, and the standard says so in a note: "an activation consumption
 * changes (to false) the transient activation states for all browsing contexts in the page, but an activation
 * notification changes (to true) the states for a subset of those browsing contexts. The exhaustive nature of
 * consumption here is deliberate: it prevents malicious sites from making multiple calls to an activation
 * consuming API from a single user activation (possibly by exploiting a deep hierarchy of iframes)." So the
 * notification walks ancestors plus SAME-ORIGIN descendants, and both consumptions walk the whole page from
 * the top-level traversable down, cross-origin frames included. Sharing one walk between them would have been
 * the security hole the note names. */

/* §6.4.2 STEP 5, FOR ONE WINDOW. Step 5.2 is "notify the close watcher manager about user activation given
   window", and §6.10.2's steps for that read and write two fields of the Window's CLOSE WATCHER MANAGER — its
   "next user interaction allows a new group" boolean and its "allowed number of groups" count. Both are read by
   exactly one thing, §6.10's CloseWatcher machinery, which this engine has no interface for: there is no
   manager here to notify, in the same sense §7.5.10's worker loops iterate a set that is empty by construction.
   The day CloseWatcher lands it brings its manager, and its manager brings this line. */
static void ua_activate(JSContext *rctx, double now)
{
    ua_set(rctx, UA_LAST, now);   /* step 5.1 */
}

/* THE ACTIVE DOCUMENT'S REALM OF A NAVIGABLE IN THE SET — with the two states that are not realms named at the
   point they are reached rather than skipped past.
   A navigable in ANOTHER INSTANCE holds its Window in the peer's heap; a timestamp written here would be
   written into a record no read of that Window will ever see.
   An UNMATERIALIZED navigable is the harder one: §7.4 already created the Window this step must write to and
   navigable.c DEFERS building its realm until something reaches it, so the Window the standard is talking
   about exists and its state has nowhere to live yet. */
static JSContext *ua_window_realm(JSContext *ctx, JSValueConst proxy)
{
    DCHECK(!window_proxy_is_remote(proxy),
           "§6.4.2's activation walk reached a navigable whose ACTIVE WINDOW is a PEER instance's — its "
           "§6.4.1 timestamps live in that instance's heap, so the write has to be POSTED to the instance "
           "holding that document (window_proxy_doc names which one) exactly as a cross-instance read is");
    DCHECK(window_proxy_materialized(proxy),
           "§6.4.2's activation walk reached a navigable whose realm is still DEFERRED — §7.4 created its "
           "Window and this step sets that Window's last activation timestamp, so BUILD the deferred Window's "
           "activation state: carry §6.4.1's two timestamps on the NAVIGABLE and hand them to the realm when "
           "it materializes, never materialize a realm from this walk (window_proxy.h: materializing every "
           "navigable a frontier ever created in order to look at it is the heap exhaustion the deferral "
           "exists to avoid)");
    return window_proxy_realm(ctx, proxy);
}

/* STEP 3 — "Extend windows with the active window of each of document's ancestor navigables". NO ORIGIN FILTER:
   a cross-origin ancestor is activated too, which is what makes a click inside an advertisement's frame count
   as an interaction with the page that embeds it. */
static void ua_notify_ancestors(JSContext *ctx, JSValueConst self, double now)
{
    JSValue p = window_proxy_parent_navigable(ctx, self);

    while (window_proxy_is(p)) {
        JSValue next;

        ua_activate(ua_window_realm(ctx, p), now);
        next = window_proxy_parent_navigable(ctx, p);
        JS_FreeValue(ctx, p);
        p = next;
    }
    JS_FreeValue(ctx, p);   /* JS_UNDEFINED at the top of the tree — window_proxy.h's engine-walk spelling */
}

/* STEP 4 — "Extend windows with the active window of each of document's descendant navigables, filtered to
   include only those navigables whose active document's origin is same origin with document's origin".
   THE FILTER DOES NOT PRUNE THE SUBTREE, and that is why a cross-origin descendant is the cross-instance gap
   ua_window_realm names rather than a case to skip past: a SAME-ORIGIN GRANDCHILD under a cross-origin child is
   a descendant navigable whose active document is same origin, so it is in this set, and skipping the child
   would silently drop everything under it. Within one instance the filter is therefore always true — an
   instance is an origin-keyed agent cluster — which is asserted rather than assumed, because a filter that
   cannot fail and a filter that is not evaluated look identical afterwards.
   THE STACK IS A JS ARRAY, the shape navigable.c's tree walk already uses: no C recursion, so a document nested
   as deeply as a page cares to nest it costs heap and not stack. */
static void ua_notify_descendants(JSContext *ctx, JSContext *from, double now)
{
    JSValue stack = JS_NewArray(ctx);
    uint32_t ntop = 0;
    int i, n;

    CHECK(!JS_IsException(stack),
          "OOM walking §6.4.2 step 4's descendant navigables — a walk that loses half the subtree leaves "
          "documents in the page reporting no interaction while their siblings report one");
    n = iframe_child_navigable_count(from);
    for (i = n - 1; i >= 0; i--)
        JS_SetPropertyUint32(ctx, stack, ntop++, iframe_child_navigable(from, i));
    while (ntop > 0) {
        JSValue kid = JS_GetPropertyUint32(ctx, stack, --ntop);
        JSContext *kctx;
        bool same;

        JS_SetPropertyUint32(ctx, stack, ntop, JS_UNDEFINED);
        /* A DESTROYED NAVIGABLE IS NOT A DESCENDANT NAVIGABLE — §7.5.10 step 8 made its browsing context null,
           so it has no active document for the walk to reach and no active window to activate. */
        if (window_proxy_destroyed(kid)) {
            JS_FreeValue(ctx, kid);
            continue;
        }
        kctx = ua_window_realm(ctx, kid);
        same = window_proxy_same_origin_of(kid);
        DCHECK(same,
               "a navigable THIS agent holds answered cross-origin — an instance is an origin-keyed agent "
               "cluster, so every document in it shares one origin and ua_window_realm has already crashed on "
               "the ones that do not");
        if (same) ua_activate(kctx, now);
        n = iframe_child_navigable_count(kctx);
        for (i = n - 1; i >= 0; i--)
            JS_SetPropertyUint32(ctx, stack, ntop++, iframe_child_navigable(kctx, i));
        JS_FreeValue(ctx, kid);
    }
    JS_FreeValue(ctx, stack);
}

void user_activation_notify(JSContext *ctx)
{
    JSValueConst self = document_window_proxy(ctx);
    double now = ua_now();

    /* STEP 1. A document that is not fully active is not being interacted with: it is not the active document
       of a navigable in the tree, so there is no input path to it at all. */
    DCHECK(document_fully_active(ctx),
           "§6.4.2 step 1: an activation notification was performed for a Document that is not FULLY ACTIVE — "
           "a trusted input event is dispatched into the navigable's active document and nothing else");
    DCHECK(window_proxy_is(self), "§6.4.2's activation notification ran in a realm with no navigable");
    ua_activate(ctx, now);                        /* step 2: « document's relevant global object » */
    ua_notify_ancestors(ctx, self, now);          /* step 3 */
    ua_notify_descendants(ctx, ctx, now);         /* step 4 */
}

/* THE TWO CONSUMPTIONS' SHARED SET — steps 1-4 of both: "If W's navigable is null, then return"; the top-level
   traversable; "the inclusive descendant navigables of top's active document"; the active window of each.
   `history` picks which of the two fields step 5 writes, because the two algorithms differ in that one line
   and writing the walk twice would be two chances to get the SET wrong — and the set is the half of this that
   is a security property.
   AN UNMATERIALIZED NAVIGABLE IS A COMPUTED NO-OP HERE, not a skipped step, and that is a different answer
   from the notification's. Its Window has never been activated — the only writer of a last activation
   timestamp is the notification, which crashes rather than reach one — so its timestamp is positive infinity
   and step 5 does nothing to it either way: the consumption's own condition is "if the last activation
   timestamp is not positive infinity", and the history-action consumption's write copies positive infinity
   onto the positive infinity that is already there. */
static void ua_consume_page(JSContext *ctx, bool history)
{
    JSValueConst self = document_window_proxy(ctx);
    JSValue stack, top;
    uint32_t ntop = 0;

    DCHECK(window_proxy_is(self),
           "§6.4.2's consume user activation ran in a realm with no navigable — its step 1 returns for a "
           "Window whose navigable is null, and every Window this agent builds has one");
    stack = JS_NewArray(ctx);
    CHECK(!JS_IsException(stack),
          "OOM walking §6.4.2's inclusive descendant navigables — a consumption that misses a frame leaves it "
          "holding an activation the page has already spent, which is the abuse the exhaustive walk prevents");
    top = window_proxy_top_navigable(ctx, self);   /* step 2 */
    JS_SetPropertyUint32(ctx, stack, ntop++, top);
    while (ntop > 0) {                             /* steps 3-4, and step 5 as each item is reached */
        JSValue nav = JS_GetPropertyUint32(ctx, stack, --ntop);
        JSContext *nctx;
        int i, n;

        JS_SetPropertyUint32(ctx, stack, ntop, JS_UNDEFINED);
        if (window_proxy_destroyed(nav)) {
            JS_FreeValue(ctx, nav);
            continue;
        }
        if (!window_proxy_materialized(nav)) {   /* step 5 is a no-op on it, and it has no children — see above */
            JS_FreeValue(ctx, nav);
            continue;
        }
        DCHECK(!window_proxy_is_remote(nav),
               "§6.4.2's consumption reached a navigable whose ACTIVE WINDOW is a PEER instance's — the walk "
               "is deliberately NOT origin-filtered, so a cross-origin frame is IN this set and skipping it "
               "would leave it holding an activation this page has already spent; POST the consumption to the "
               "instance holding that document exactly as a cross-instance read is posted");
        nctx = window_proxy_realm(ctx, nav);
        if (history) {
            /* "set window's last history-action activation timestamp to window's last activation timestamp" */
            ua_set(nctx, UA_HISTORY, ua_get(nctx, UA_LAST));
        } else if (ua_get(nctx, UA_LAST) != INFINITY) {
            /* "if window's last activation timestamp is not positive infinity, then set it to negative
               infinity" — a Window that was never activated STAYS never-activated, so a consumption cannot
               hand a page sticky activation it never earned. */
            ua_set(nctx, UA_LAST, -INFINITY);
        }
        n = iframe_child_navigable_count(nctx);
        for (i = n - 1; i >= 0; i--)
            JS_SetPropertyUint32(ctx, stack, ntop++, iframe_child_navigable(nctx, i));
        JS_FreeValue(ctx, nav);
    }
    JS_FreeValue(ctx, stack);
}

void user_activation_consume(JSContext *ctx)
{
    ua_consume_page(ctx, false);
}

void user_activation_consume_history_action(JSContext *ctx)
{
    ua_consume_page(ctx, true);
}

/* ---- §6.4.4's UserActivation INTERFACE -----------------------------------------------------------------------
 *
 *   [Exposed=Window] interface UserActivation {
 *     readonly attribute boolean hasBeenActive;
 *     readonly attribute boolean isActive;
 *   };
 *
 * THE OBJECT CARRIES NO STATE OF ITS OWN, and that is the standard's shape rather than a stub: both getters
 * read "this's relevant global object", so the state they answer from is the record above and what the object
 * IS is the identity a page holds and brand-checks. It is minted with the realm below — §6.4.4 says "upon
 * creation of the Window object" — so the [SameObject] guarantee comes from the realm slot rather than from a
 * cache in navigator.c's getter, and no flow can make its own first read into every sibling's baseline. */
static JSClassID g_ua_class;
static int g_obj_slot = -1;

/* WEB IDL §3.7.5's BRAND CHECK. `UserActivation.prototype.isActive` read off a plain object is a TypeError, and
   a page tells that apart from `false` — which is the whole reason the members cannot be plain data properties
   on the one object. */
static bool ua_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_ua_class != 0, "a UserActivation getter ran before user_activation_init declared the class — the "
                            "getter is only reachable through a prototype the per-realm install builds, so "
                            "there is no route here that has not run the declaration first");
    if (JS_GetClassID(this_val) == g_ua_class) return true;
    JS_ThrowTypeError(ctx, "a UserActivation getter was read off something that is not a UserActivation");
    return false;
}

/* THE HALF OF "THIS'S RELEVANT GLOBAL OBJECT" THIS ENGINE CAN ANSWER, asserted rather than assumed. A C member
   runs in the realm that DEFINED it (js_call_c_function takes `ctx` from the function object), so an ordinary
   `navigator.userActivation.isActive` arrives with the ctx of the document whose prototype it went through —
   the right Window, because this realm's object is reached through this realm's prototype. What does NOT
   arrive right is one realm's getter applied to another's object; the two then name different Windows, and
   there is no third thing to consult, because the object holds nothing that says whose it is. */
static void ua_assert_this_window(JSContext *ctx, JSValueConst this_val)
{
    JSValue own = realm_value_get(ctx, g_obj_slot);
    bool same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, own);
    DCHECK(same, "§6.4.4's getter steps read THIS's relevant global object, and this UserActivation belongs to "
                 "a different realm of this agent than the prototype it was reached through — answering out of "
                 "the getter's own realm would report another document's activation. BUILD the object that "
                 "names its own Window: hand each realm's UserActivation its navigable's WindowProxy once §7.4 "
                 "has built one, resolve it with window_proxy_realm, and read the timestamps out of THAT realm");
}

/* "The hasBeenActive getter steps are to return true if this's relevant global object has sticky activation,
   and false otherwise." */
static JSValue js_ua_has_been_active(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!ua_brand(ctx, this_val)) return JS_EXCEPTION;
    ua_assert_this_window(ctx, this_val);
    return JS_NewBool(ctx, user_activation_sticky(ctx));
}

/* "The isActive getter steps are to return true if this's relevant global object has transient activation, and
   false otherwise." */
static JSValue js_ua_is_active(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!ua_brand(ctx, this_val)) return JS_EXCEPTION;
    ua_assert_this_window(ctx, this_val);
    return JS_NewBool(ctx, user_activation_transient(ctx));
}

JSValue user_activation_object(JSContext *ctx)
{
    return realm_value_get(ctx, g_obj_slot);   /* OWNED — realm_value_get asserts the realm ran its install */
}

/* ---- the declaration and the per-realm record ----------------------------------------------------------- */

/* ONE RECORD PER REALM, BUILT WITH THE REALM. §6.4.1's values belong to a Window and a Window is created with
   its realm, so the record is written here and not lazily on the first read: a record built on first touch
   would be built inside whichever flow happened to ask first, and that flow's baseline would become every
   other flow's. §6.4.4's prototype, interface object and Window-associated object are built here for the same
   reason and in the same breath — they are this Window's, and §3.7 gives every realm its own. */
static void user_activation_install_realm(JSContext *ctx)
{
    JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
    JSValue proto, prev, global, obj;

    CHECK(!JS_IsException(rec), "user activation: OOM building a realm's §6.4.1 record");
    JS_SetPropertyStr(ctx, rec, UA_LAST, JS_NewFloat64(ctx, INFINITY));
    JS_SetPropertyStr(ctx, rec, UA_HISTORY, JS_NewFloat64(ctx, INFINITY));
    realm_value_set(ctx, g_slot, rec);

    prev = JS_GetClassProto(ctx, g_ua_class);
    DCHECK(JS_IsNull(prev), "user_activation_install_realm ran twice in one realm — everything already holding "
                            "the first UserActivation.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "UserActivation.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "UserActivation");
    idl_install_accessor(ctx, proto, "hasBeenActive", js_ua_has_been_active, 0, -1);
    idl_install_accessor(ctx, proto, "isActive", js_ua_is_active, 0, -1);
    JS_SetClassProto(ctx, g_ua_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. §6.4.4 declares no constructor, so `new
       UserActivation()` is a TypeError — and its presence is what tells a feature-detecting bundle that the
       interface exists at all, which is exactly the gate this component was built to stop lying about. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "UserActivation", idl_interface_object(ctx, "UserActivation", proto));
    JS_FreeValue(ctx, global);

    /* "Upon creation of the Window object, its associated UserActivation must be set to a new UserActivation
       object created in the Window object's relevant realm." */
    obj = JS_NewObjectProtoClass(ctx, proto, g_ua_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the Window's associated UserActivation could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);
}

void user_activation_init(JSContext *ctx)
{
    JSClassDef d = { "UserActivation" };

    DCHECK(g_slot < 0, "user_activation_init ran twice — the record's slot is declared once per AGENT");
    g_slot = realm_value_declare(ctx, "HTML §6.4.1 the Window's user activation timestamps");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the one object per realm WEARS it, so
       §3.7.5's check is a class-id comparison and a page cannot forge one. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_ua_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_ua_class, &d) == 0,
          "UserActivation: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "HTML §6.4.4 the Window's associated UserActivation");
    realm_declare_intrinsic(user_activation_install_realm);
}

void user_activation_free(void)
{
    /* The RECORDS, the prototypes, the interface objects and the Window-associated objects are the realms' —
       each is released with its context. What the agent holds is the two slots, and a slot id is a class id in
       a runtime that is going away with it. */
    g_slot = -1;
    g_obj_slot = -1;
}
