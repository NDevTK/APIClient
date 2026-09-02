/* USER ACTIVATION — HTML §6.4. See user_activation.h for why this is state and not a constant. */
#include <math.h>
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/frame/window_proxy.h"
#include "core/html/close_watcher.h"
#include "core/html/html_iframe.h"
#include "core/html/user_activation.h"
#include "core/timing/hr_time.h"
#include "solver/concolic.h"

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

/* THE UNKNOWN, AND THE TWO PREDICATES OVER IT — see the header for why the initial timestamp is a source and
   not a constant. The shape is what an @H record would display it as; the source identity is what the flow's
   path constraint is keyed by, so every Window of this agent shares ONE fact ("did the user interact with this
   page") rather than one per frame, which is also what a real interaction does: §6.4.2's notification activates
   an ancestor chain and a same-origin subtree in one go.
   The OPERATION strings are the other half of the key (decide.c keys an outcome fork by source-then-operation),
   so the two questions stay independent facts about the one source. */
#define UA_SHAPE       "{user has interacted}"
#define UA_SRC         "window.userActivation"
#define UA_OP_STICKY    "HTML §6.4.1 sticky activation"
#define UA_OP_TRANSIENT "HTML §6.4.1 transient activation"
#define UA_OP_HISTORY   "HTML §6.4.1 history-action activation"

/* THE TWO QUESTIONS user_activation_transient_run ASKS IN SEQUENCE, as the caller's phase byte spells them.
   Zero is "ask the first", which is what a zeroed byte already reads as and what every answered question
   leaves behind. */
enum { UA_PH_STICKY = 0, UA_PH_RECENT };

static int g_slot = -1;

/* THIS REALM'S §6.4.1 RECORD. Owned — the caller frees. */
static JSValue ua_record(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);

    DCHECK(JS_IsObject(rec), "a realm answered for its Window's §6.4.1 user activation state with no record");
    return rec;
}

/* ONE OF THE TWO TIMESTAMPS, OWNED. Either a real number — a DOMHighResTimeStamp this engine's own §6.4.2
   notification wrote, or one of the two infinities — or the CONCOLIC the record was born holding, which is
   "the user may or may not have interacted and this engine has no way to see which". */
static JSValue ua_ts(JSContext *ctx, const char *field)
{
    JSValue rec = ua_record(ctx), v = JS_GetPropertyStr(ctx, rec, field);

    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "a §6.4.1 activation timestamp is neither a number nor the unknown it was born as — the record holds "
           "two DOMHighResTimeStamps, the two infinities and the source the initial value is, and nothing else "
           "ever writes it");
    return v;
}

/* A TIMESTAMP THIS ENGINE KNOWS THE VALUE OF. Every caller has already established that the value is not the
   unknown — by testing concolic_is, or by having written it itself — so a non-number here is a caller that
   skipped that test and would otherwise coerce a source object to NaN and compare against it. */
static double ua_number(JSContext *ctx, JSValueConst v)
{
    double d = 0;

    DCHECK(!concolic_is(v), "a §6.4.1 activation timestamp that is UNKNOWN was read as a number — the "
                            "comparison would be against NaN, which answers every one of §6.4.1's three "
                            "questions false without asking the solver any of them");
    JS_ToFloat64(ctx, &d, v);
    return d;
}

/* WRITTEN INTO THE REALM WHOSE WINDOW IT IS, which is why every writer below takes a realm rather than acting
   on the asking one: §6.4.2's notification and consumptions each set the timestamps of a SET of Windows, and
   every one of them is a different realm in this agent. The write is an ordinary property write, so the heap
   COW captures it into the running flow's delta — one arm consuming an activation leaves its sibling's.
   `v` is CONSUMED. */
static void ua_set(JSContext *rctx, const char *field, JSValue v)
{
    JSValue rec = ua_record(rctx);

    JS_SetPropertyStr(rctx, rec, field, v);
    JS_FreeValue(rctx, rec);
}

/* §6.4.1's INITIAL LAST ACTIVATION TIMESTAMP — positive infinity, "indicating that W has never been
   activated", carried as the EXAMPLE of the source it actually is. concolic_source_wrap is the seam: a host
   with no source overlay (a conformance run) gets the bare positive infinity back and every question below is
   plain arithmetic, so test262 sees the standard's own answers and forks nothing. */
static JSValue ua_never_activated(JSContext *ctx)
{
    return concolic_source_wrap(ctx, UA_SHAPE, UA_SRC, JS_NewFloat64(ctx, INFINITY));
}

/* ONE QUESTION OVER THE UNKNOWN, through the seam a step machine forks at. The operand is BORROWED for the
   length of the request and the REALM'S RECORD is what owns it — the driver reads h->fork_over on the line
   after this machine returns, with nothing running in between, and the record the value came from holds its own
   reference for the life of the realm. So the local reference is released here on both paths.
   OUTCOME 0 IS THE EXAMPLE'S ANSWER (no activation), which is what a run with no forking policy — the @S
   candidate re-fire — must take. */
static int ua_ask(JSContext *ctx, JSStepHdr *h, JSValue last, const char *op, bool *out)
{
    int arm = 0, rc;

    DCHECK(concolic_is(last), "§6.4.1's fork seam was asked about a timestamp this engine knows the value of — "
                              "a known state has one feasible answer and forking it would park a sibling flow "
                              "exploring a world that cannot happen");
    rc = step_fork_run(ctx, h, last, op, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
    JS_FreeValue(ctx, last);
    if (rc) return rc;
    *out = (arm == 1);
    return 0;
}

/* §6.4.1's "current high resolution time given W" — HR-TIME §4's operation, over the event loop's one virtual
   clock (core/timing/hr_time.h). It used to read that clock directly, which answered a DIFFERENT question:
   §6.4.1 measures from W's own environment's TIME ORIGIN, and the raw clock is the unsafe shared current time,
   so the two agree only while every environment's origin is zero. `ctx` IS the W the standard names — the
   three questions below are asked of a Window and answered against a timestamp stored in that same Window's
   record, so both ends of every comparison are measured from one origin. */
/* AND IT IS A MOMENT AND NOT A `double`, for the same reason `last` is a value: the event loop's clock is a
   moment, and once a timer set with an unknown `timeout` has fired the clock — and every duration measured
   from it — is unknown external input (core/timing/event_loop.h). Which makes the three questions below
   questions over TWO possibly-unknown operands rather than one, and each arm of each of them is a real
   program: a page that believes it has a transient activation and one that does not. OWNED. */
static JSValue ua_now(JSContext *ctx)
{
    return hr_time_current(ctx);
}

int user_activation_sticky_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, bool *out)
{
    /* "When the current high resolution time given W is greater than or equal to the last activation timestamp
       in W, W is said to have sticky activation." */
    JSValue last = ua_ts(ctx, UA_LAST);

    DCHECK(*phase == UA_PH_STICKY,
           "§6.4.1's sticky-activation question was asked on a phase byte the transient chain left mid-question "
           "— the two questions would then answer each other, since the second is only ever asked inside the "
           "first's true arm");
    if (!concolic_is(last)) {
        JSValue now = ua_now(ctx);

        if (!concolic_is(now)) {
            *out = ua_number(ctx, now) >= ua_number(ctx, last);
            JS_FreeValue(ctx, now);
            JS_FreeValue(ctx, last);
            return 0;
        }
        /* THE UNKNOWN OPERAND IS THE CURRENT TIME rather than the stored timestamp, and the fork is the same
           fork: `ua_ask` keys it by (this value's identity, this clause), so the question a replay re-asks is
           "is the clock past this Window's last activation" and not "which of the two operands was unknown". */
        JS_FreeValue(ctx, last);
        return ua_ask(ctx, h, now, UA_OP_STICKY, out);
    }
    return ua_ask(ctx, h, last, UA_OP_STICKY, out);
}

int user_activation_transient_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, bool *out)
{
    /* "... greater than or equal to the last activation timestamp in W, and less than the last activation
       timestamp in W plus the transient activation duration" — a CONJUNCTION, and evaluating it left to right
       is what keeps the two worlds it opens consistent. The first conjunct is sticky activation exactly; the
       second is only asked where the first said yes, so no flow ever holds "recently but never". */
    JSValue last = ua_ts(ctx, UA_LAST);
    int rc;

    if (!concolic_is(last)) {
        JSValue nowv = ua_now(ctx);

        if (!concolic_is(nowv)) {
            double l = ua_number(ctx, last), now = ua_number(ctx, nowv);

            JS_FreeValue(ctx, nowv);
            JS_FreeValue(ctx, last);
            DCHECK(*phase == UA_PH_STICKY,
                   "§6.4.1's transient-activation question resumed mid-chain over a timestamp that is no "
                   "longer unknown — the second conjunct's answer is owed to a flow that is standing inside "
                   "the first's true arm, and arithmetic cannot deliver it");
            *out = now >= l && now < l + UA_TRANSIENT_ACTIVATION_DURATION_MS;
            return 0;
        }
        /* THE CONJUNCTION, WITH THE CLOCK AS THE UNKNOWN OPERAND — the same two ordered questions as below,
           over the same phase byte, so a flow parked between them resumes into the same place. Left to
           right, and the second is asked only inside the first's true arm, so no flow ever holds "recently
           but never". */
        JS_FreeValue(ctx, last);
        if (*phase == UA_PH_STICKY) {
            bool sticky = false;

            rc = ua_ask(ctx, h, JS_DupValue(ctx, nowv), UA_OP_STICKY, &sticky);
            if (rc) { JS_FreeValue(ctx, nowv); return rc; }
            if (!sticky) {
                JS_FreeValue(ctx, nowv);
                *out = false;
                return 0;
            }
            *phase = UA_PH_RECENT;
        }
        rc = ua_ask(ctx, h, nowv, UA_OP_TRANSIENT, out);
        if (rc) return rc;
        *phase = UA_PH_STICKY;
        return 0;
    }
    if (*phase == UA_PH_STICKY) {
        bool sticky = false;

        rc = ua_ask(ctx, h, JS_DupValue(ctx, last), UA_OP_STICKY, &sticky);
        if (rc) { JS_FreeValue(ctx, last); return rc; }
        if (!sticky) {                      /* the first conjunct is false, so the second is never asked */
            JS_FreeValue(ctx, last);
            *out = false;
            return 0;
        }
        *phase = UA_PH_RECENT;
    }
    DCHECK(*phase == UA_PH_RECENT, "§6.4.1's transient-activation question resumed in a phase it never parks in");
    rc = ua_ask(ctx, h, last, UA_OP_TRANSIENT, out);
    if (rc) return rc;
    *phase = UA_PH_STICKY;                  /* the chain is finished; the byte is ready for the next question */
    return 0;
}

int user_activation_history_action_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, bool *out)
{
    /* "When the last history-action activation timestamp of W is not equal to the last activation timestamp of
       W, then W is said to have history-action activation." */
    JSValue last = ua_ts(ctx, UA_LAST), hist = ua_ts(ctx, UA_HISTORY);
    int rc;

    if (!concolic_is(last) && !concolic_is(hist)) {
        *out = ua_number(ctx, hist) != ua_number(ctx, last);
        JS_FreeValue(ctx, last);
        JS_FreeValue(ctx, hist);
        return 0;
    }
    /* THE HISTORY-ACTION CONSUMPTION COPIED ONE ONTO THE OTHER, so the two hold the IDENTICAL value and the
       question is answered by identity rather than by a fork: equal, so no history-action activation. This is
       the whole reason that consumption needs to decide nothing — the copy is correct in every world. */
    if (JS_VALUE_GET_TAG(last) == JS_VALUE_GET_TAG(hist) && JS_VALUE_GET_PTR(last) == JS_VALUE_GET_PTR(hist)) {
        JS_FreeValue(ctx, last);
        JS_FreeValue(ctx, hist);
        *out = false;
        return 0;
    }
    /* THE UNCONSUMED CASE, and it is not a new question. The only writer of the history-action timestamp is the
       consumption above, so a Window that has not had one still holds §6.4.1's initial positive infinity, and
       "the last activation timestamp is not positive infinity" is sticky activation said the other way round —
       a timestamp is never in the future, so "not positive infinity" and "now is at or past it" are one test.
       Asking it as sticky is what stops this from opening a fourth world contradicting the other three. */
    if (concolic_is(last) && !concolic_is(hist) && ua_number(ctx, hist) == INFINITY) {
        JS_FreeValue(ctx, last);
        JS_FreeValue(ctx, hist);
        return user_activation_sticky_run(ctx, h, phase, out);
    }
    /* A CONSUMED history-action timestamp against a LATER real activation: one of the two is the unknown and
       they are different values, so this is its own question over that unknown. */
    {
        JSValue unknown = concolic_is(last) ? last : hist;
        JSValue known = concolic_is(last) ? hist : last;

        JS_FreeValue(ctx, known);
        rc = ua_ask(ctx, h, unknown, UA_OP_HISTORY, out);   /* ua_ask consumes `unknown` */
    }
    return rc;
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

/* §6.4.2 STEP 5, FOR ONE WINDOW — both of its sub-steps, which are two different components'.
   THIS SITE ONCE RECORDED STEP 5.2 AS UNREACHABLE and the reason it gave has been retired, so it is written
   out here rather than deleted: a reader who re-derives it will re-introduce the gap. It said the manager's
   two fields were "read by exactly one thing, §6.10's CloseWatcher machinery, which this engine has no
   interface for", and concluded there was no manager to notify — the §7.5.10 shape, a walk over a set that is
   empty by construction. The conclusion did not follow from the premise even then: §6.10.2 gives the manager
   to the WINDOW, not to the `CloseWatcher` interface, and §6.12 The popover attribute and §4.11.4's modal
   `dialog` establish watchers in it without that interface existing at all. core/html/close_watcher.c now
   holds it, every realm is built with one at its spec-initial values, and step 5.2 is an ordinary call. */
static void ua_activate(JSContext *rctx)
{
    /* Step 5.1 — and this is the write that turns the unknown into a fact. Once a trusted input event has
       reached this Window, this engine has OBSERVED the activation and its timestamp is an ordinary number, so
       every one of §6.4.1's three questions is arithmetic from here and nothing forks over it again.
       THE MOMENT IS THIS WINDOW'S OWN, and that is why it is read here rather than passed in. §6.4.2
       Processing model's step 5.1 sets "window's last activation timestamp to THE CURRENT HIGH RESOLUTION
       TIME", which HR-TIME §4 defines only
       given a global object — and §6.4.1 then reads it back through `ua_now` given that same Window. Each
       Window in this walk has its own environment and therefore its own TIME ORIGIN, so one number computed in
       the initiating document and written into every frame of the page would be a duration measured from one
       origin and compared against another: an ancestor created before this document would report an activation
       in its own future, and §6.4.1's transient-activation window would be shifted by the difference.
       "AN ORDINARY NUMBER" IS NOW CONDITIONAL, AND THE FIRST PARAGRAPH SAID IT WAS NOT. The moment this
       stores is the current high resolution time, which is a MOMENT — unknown external input once a timer set
       with an unknown `timeout` has moved the event loop's clock there (core/timing/event_loop.h). So the
       observation is still real (a trusted input event DID reach this Window) while WHEN it happened may not
       be, and §6.4.1's three questions go on forking over the stored value exactly as they fork over the
       clock. Storing the value is what keeps those two facts apart; flattening it here would report an
       activation at a moment nothing computed. */
    ua_set(rctx, UA_LAST, ua_now(rctx));
    /* Step 5.2 — "Notify the close watcher manager about user activation given window." It is THIS window's
       manager, which is why it takes the same realm step 5.1 wrote: §6.4.2's step 5 walks a SET of Windows and
       every one of them is a different realm in this agent, so a manager notified out of the asking realm
       would raise one Window's allowance for another Window's interaction. */
    close_watcher_notify_user_activation(rctx);
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
static void ua_notify_ancestors(JSContext *ctx, JSValueConst self)
{
    JSValue p = window_proxy_parent_navigable(ctx, self);

    while (window_proxy_is(p)) {
        JSValue next;

        ua_activate(ua_window_realm(ctx, p));
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
static void ua_notify_descendants(JSContext *ctx, JSContext *from)
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
        if (same) ua_activate(kctx);
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

    /* STEP 1. A document that is not fully active is not being interacted with: it is not the active document
       of a navigable in the tree, so there is no input path to it at all. */
    DCHECK(document_fully_active(ctx),
           "§6.4.2 step 1: an activation notification was performed for a Document that is not FULLY ACTIVE — "
           "a trusted input event is dispatched into the navigable's active document and nothing else");
    DCHECK(window_proxy_is(self), "§6.4.2's activation notification ran in a realm with no navigable");
    ua_activate(ctx);                             /* step 2: « document's relevant global object » */
    ua_notify_ancestors(ctx, self);               /* step 3 */
    ua_notify_descendants(ctx, ctx);              /* step 4 */
}

/* THE TWO CONSUMPTIONS' SHARED SET — steps 1-4 of both: "If W's navigable is null, then return"; the top-level
   traversable; "the inclusive descendant navigables of top's active document"; the active window of each.
   `history` picks which of the two fields step 5 writes, because the two algorithms differ in that one line
   and writing the walk twice would be two chances to get the SET wrong — and the set is the half of this that
   is a security property.
   `sticky` IS THE ANSWER §6.4.1's STICKY-ACTIVATION QUESTION WAS GIVEN, asked ONCE by the caller before the
   walk begins. §6.4.2's step 5 condition — "if window's last activation timestamp is not positive infinity" —
   is that question said the other way round for a Window whose timestamp is UNKNOWN, and every Window in this
   agent shares the one source, so one answer serves the whole walk and the walk itself asks nothing and cannot
   suspend. A Window whose timestamp this engine WROTE is decided by the arithmetic instead, because there is
   nothing unknown left about it.
   `probe` reports whether any Window in the set still holds the unknown, WITHOUT writing anything — it is how
   the caller knows whether the question is worth asking at all, so a page every Window of which has a real
   timestamp forks nothing. */
static bool ua_consume_page(JSContext *ctx, bool history, bool sticky, bool probe)
{
    bool any_unknown = false;
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
        if (!window_proxy_materialized(nav)) {
            /* THIS USED TO BE A SKIP, AND ITS JUSTIFICATION DIED WITH THE CONSTANT. It read "step 5 is a no-op
               on it": the Window had never been activated, so its timestamp was positive infinity and neither
               write changed anything. That is no longer true — a Window whose realm has not been built yet has
               no record, and the record it will be BORN with holds the unknown, so a consumption that skips it
               leaves it answering §6.4.1's transient question out of the arm this very flow committed to when
               it decided to consume. That is the deep-iframe abuse §6.4.2's note names the exhaustive walk to
               prevent, arriving by the one door the walk does not cover. */
            DFAIL("§6.4.2's consumption reached a navigable whose realm is still DEFERRED, and its Window's "
                  "activation state has nowhere to be spent — BUILD what ua_window_realm's own assert names: "
                  "carry §6.4.1's two timestamps on the NAVIGABLE and hand them to the realm when it "
                  "materializes, so a consumption reaches a Window that has not run any code yet. Never "
                  "materialize a realm from this walk (window_proxy.h: materializing every navigable a frontier "
                  "ever created in order to look at it is the heap exhaustion the deferral exists to avoid)");
        }
        DCHECK(!window_proxy_is_remote(nav),
               "§6.4.2's consumption reached a navigable whose ACTIVE WINDOW is a PEER instance's — the walk "
               "is deliberately NOT origin-filtered, so a cross-origin frame is IN this set and skipping it "
               "would leave it holding an activation this page has already spent; POST the consumption to the "
               "instance holding that document exactly as a cross-instance read is posted");
        nctx = window_proxy_realm(ctx, nav);
        if (history) {
            /* "set window's last history-action activation timestamp to window's last activation timestamp" —
               a COPY, which is correct in every world at once: whatever the last activation timestamp turns
               out to have been, the two are afterwards the identical value and §6.4.1's history-action question
               is answered by that identity rather than by a decision this walk had to take. */
            if (!probe) ua_set(nctx, UA_HISTORY, ua_ts(nctx, UA_LAST));
        } else {
            /* "if window's last activation timestamp is not positive infinity, then set it to negative
               infinity" — a Window that was never activated STAYS never-activated, so a consumption cannot
               hand a page sticky activation it never earned. */
            JSValue last = ua_ts(nctx, UA_LAST);
            bool unknown = concolic_is(last);
            bool activated = unknown ? sticky : (ua_number(nctx, last) != INFINITY);

            JS_FreeValue(nctx, last);
            if (unknown) any_unknown = true;
            if (!probe && activated) ua_set(nctx, UA_LAST, JS_NewFloat64(nctx, -INFINITY));
        }
        n = iframe_child_navigable_count(nctx);
        for (i = n - 1; i >= 0; i--)
            JS_SetPropertyUint32(ctx, stack, ntop++, iframe_child_navigable(nctx, i));
        JS_FreeValue(ctx, nav);
    }
    JS_FreeValue(ctx, stack);
    return any_unknown;
}

int user_activation_consume_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase)
{
    bool sticky = false;

    /* THE PROBE FIRST, AND ONLY THEN THE QUESTION. A page whose every Window carries a timestamp this engine
       wrote has nothing unknown in it, and asking §6.4.1's sticky question there would park a sibling flow
       exploring a world the known state contradicts. The probe runs the same walk with the same set — one
       algorithm, not a second one that could disagree about which navigables are in it — and writes nothing. */
    if (ua_consume_page(ctx, false, false, /*probe*/ true)) {
        int rc = user_activation_sticky_run(ctx, h, phase, &sticky);

        if (rc) return rc;
    }
    ua_consume_page(ctx, false, sticky, /*probe*/ false);
    return 0;
}

void user_activation_consume_history_action(JSContext *ctx)
{
    ua_consume_page(ctx, true, /*sticky, unread by the copy*/ false, /*probe*/ false);
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

/* §6.4.4's TWO GETTERS, AND THEY ARE STEP MACHINES — which for a getter that takes no arguments and runs none
 * of the page's code needs saying, because the reason is neither of those. It is that the answer may be
 * UNKNOWN, and a question over an unknown FORKS: the getter has to be able to hand this flow one answer while
 * a sibling flow is snapshotted holding the other, and step_fork_run is the seam that does it. A plain C getter
 * cannot — it would take one arm silently, and `if (navigator.userActivation.isActive)` would then explore one
 * of its two branches and delete the other.
 *   IT IS ALSO WHAT KEEPS THE PAGE'S ANSWER AND THE ENGINE'S THE SAME ONE. `showPicker()` asks §6.4.1's
 * transient question through this same seam, so both are the one predicate in the flow's path constraint: a
 * page that reads `isActive` and then calls `showPicker()` cannot be told yes and then throw NotAllowedError.
 * Handing the page a concolic instead would have keyed its own `if` by the bare source, which is a DIFFERENT
 * predicate from the engine's, and the two would disagree in half the worlds.
 *
 *   §6.4.4 The UserActivation interface states both:
 *   "The hasBeenActive getter steps are to return true if this's relevant global object has sticky activation,
 *    and false otherwise."
 *   "The isActive getter steps are to return true if this's relevant global object has transient activation,
 *    and false otherwise." */
#define UA_GET_STAGES(X) \
    X(UA_GET_ASK, "HTML §6.4.4 the hasBeenActive / isActive getter steps (this's relevant global object's " \
                  "§6.4.1 sticky / transient activation state)")
enum { IDL_STEP_STAGE_BASE(UA_GET_STAGES) UA_GET_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UA_GET_STEPS[] = { UA_GET_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* WHICH OF THE TWO GETTERS, as the magic one declaration carries — they are one algorithm over §6.4.1's two
   boolean states, and a second machine would be a second copy of the brand check and the realm assert. */
enum { UA_GET_STICKY = 0, UA_GET_TRANSIENT };

/* THE ONLY STATE EITHER GETTER HOLDS: which of the transient chain's two questions is outstanding. It holds no
   JSValue at all, so there is nothing for the visit to trace and nothing for a teardown to release — declared
   rather than omitted, because a machine with no `visit` is refused at registration. */
typedef struct { uint8_t phase; } UaGetState;

static void ua_get_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int ua_get_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    UaGetState *s = st;
    int magic = idl_step_magic(hdr);
    bool state = false;
    int rc;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == UA_GET_ASK, "a UserActivation getter resumed into a stage §6.4.4 does not have");
    if (!ua_brand(ctx, hdr->this_val)) return -1;
    ua_assert_this_window(ctx, hdr->this_val);
    rc = (magic == UA_GET_TRANSIENT) ? user_activation_transient_run(ctx, hdr, &s->phase, &state)
                                     : user_activation_sticky_run(ctx, hdr, &s->phase, &state);
    if (rc) return rc;
    *presult = JS_NewBool(ctx, state);
    return JS_STEP_DONE;
}

static const IdlStepDecl UA_GET_DECL = { ua_get_step, sizeof(UaGetState), ua_get_visit, NULL,
                                         "HTML §6.4.4 UserActivation.hasBeenActive / .isActive",
                                         UA_GET_STEPS };
static int g_id_has_been_active = -1, g_id_is_active = -1;

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
    /* THE UNKNOWN IS BAKED INTO THE BASELINE, which is where it belongs: whether the user has interacted with
       this page is not a fact any flow established, so a value minted on the first READ would make whichever
       flow asked first the source of every sibling's answer. The history-action timestamp is NOT unknown —
       its only writer is §6.4.2's history-action consumption, so positive infinity here is a fact about what
       this engine has done rather than about what the user has. */
    JS_SetPropertyStr(ctx, rec, UA_LAST, ua_never_activated(ctx));
    JS_SetPropertyStr(ctx, rec, UA_HISTORY, JS_NewFloat64(ctx, INFINITY));
    realm_value_set(ctx, g_slot, rec);

    prev = JS_GetClassProto(ctx, g_ua_class);
    DCHECK(JS_IsNull(prev), "user_activation_install_realm ran twice in one realm — everything already holding "
                            "the first UserActivation.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "UserActivation.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "UserActivation");
    DCHECK(g_id_has_been_active >= 0 && g_id_is_active >= 0,
           "§6.4.4's getters were installed on a realm's prototype before user_activation_init declared them");
    idl_install_accessor_step(ctx, proto, "hasBeenActive", g_id_has_been_active, -1);
    idl_install_accessor_step(ctx, proto, "isActive", g_id_is_active, -1);
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
    /* THE TWO GETTERS ARE ONE DECLARATION WITH TWO MAGICS — declared once per AGENT, like every other member,
       because the id a declaration returns is the RUNTIME's and a per-realm declaration would mint the machine
       once per document. */
    g_id_has_been_active = idl_getter_id_step(ctx, &UA_GET_DECL, UA_GET_STICKY);
    g_id_is_active = idl_getter_id_step(ctx, &UA_GET_DECL, UA_GET_TRANSIENT);
    realm_declare_intrinsic(user_activation_install_realm);
}

void user_activation_free(void)
{
    /* The RECORDS, the prototypes, the interface objects and the Window-associated objects are the realms' —
       each is released with its context. What the agent holds is the two slots, and a slot id is a class id in
       a runtime that is going away with it. */
    g_slot = -1;
    g_obj_slot = -1;
    g_id_has_been_active = -1;
    g_id_is_active = -1;
}
