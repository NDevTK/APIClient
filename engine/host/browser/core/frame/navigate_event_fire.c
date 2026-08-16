/* FIRING THE NAVIGATE EVENT — HTML §7.2.6.10.4. See navigate_event_fire.h for what this is and what of
   §7.2.6.10 is deliberately still absent.

   THE STEP NUMBERS BELOW ARE THE STANDARD'S OWN LIST, COUNTED. §7.2.6.10.4 writes the inner algorithm as an
   unnumbered ordered list of 33 steps and its wrappers as lists of 11 and 12; each label and each comment
   quotes the step's text as well as its number, so a reader can check the number against the words rather than
   against a count. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/abort.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/navigate_event.h"
#include "core/frame/history.h"
#include "core/frame/navigate_event_fire.h"
#include "core/frame/navigation.h"
#include "core/frame/navigation_abort.h"
#include "core/frame/navigation_destination.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/url/url.h"

/* ---- the inner navigate event firing algorithm ------------------------------------------------------------- */

/* FOUR REST POINTS, AND EVERY ONE OF THEM IS SOMEWHERE THE PAGE'S CODE RUNS OR SOMETHING OF THE PAGE'S SIZE IS
   WALKED. The wrapper's step 2 is its own stage because ABORTING a still-ongoing navigation fires `abort` at the
   old event's signal and `navigateerror` at the Navigation, and the standard makes it a LOOP over exactly that.
   NEF_PREPARE then builds the destination and the event out of what the operation was created with — a URL parse
   and a serialization, both over data the PAGE chose the size of, which is why that stage ends rather than
   running on into the dispatch. NEF_DISPATCH is §2.9's dispatch itself, where the page's `navigate` listeners
   run; the fire request's own two-phase cursor resumes inside that one stage. NEF_CANCELED is step 28's abort,
   for the same reason step 2's is a stage. Steps 29-33 need no stage of their own — they are O(1) engine
   actions performed once the dispatch has answered. */
#define NEF_STAGES(X)                                                                                        \
    X(NEF_ABORT,    "HTML §7.2.6.10.4 fire a push/replace/reload navigate event steps 1-2 (inform the "       \
                    "navigation API about aborting navigation in document's node navigable — §7.2.6.8's "     \
                    "loop, which aborts whatever navigation is still ongoing)")                               \
    X(NEF_PREPARE,  "HTML §7.2.6.10.4 fire a push/replace/reload navigate event steps 3-11 and the inner "    \
                    "navigate event firing algorithm steps 1-26 (the destination, the event with everything "  \
                    "§7.2.6.10.4 initializes on it, and setting navigation's ongoing navigate event)")        \
    X(NEF_DISPATCH, "HTML §7.2.6.10.4 inner navigate event firing algorithm step 27 (let dispatchResult be "  \
                    "the result of dispatching event at navigation)")                                         \
    X(NEF_CANCELED, "HTML §7.2.6.10.4 inner navigate event firing algorithm step 28.2 (if event's abort "     \
                    "controller's signal is not aborted, then abort the ongoing navigation given navigation)")
enum { NEF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NEF_STEPS[] = { NEF_STAGES(JS_STEP_STAGE_LABEL) NULL };
#define NEF_STAGE_COUNT ((int)(sizeof NEF_STEPS / sizeof *NEF_STEPS) - 1)
/* ONE NAME FOR THE ALGORITHM THESE STAGES ARE STEPS OF — the dispatch's abort and the teardown's assertion
   share it, so neither can name a different algorithm from the other. */
#define NEF_ALGORITHM "HTML §7.2.6.10.4 the inner navigate event firing algorithm"

void navigate_event_fire_work_start(NavigateEventFireWork *w)
{
    int k;

    /* THE SLOTS ARE UNDEFINED BEFORE THEY ARE ANYTHING ELSE — a zeroed JSValue is the INTEGER 0, not undefined
       (JS_TAG_INT is 0), so a slot read before it is written yields a real value the page can see. Same rule and
       same reason as core/frame/navigation.c's update work. */
    w->stage = NEF_ABORT;
    w->phase = 0;
    w->navigation_type = NULL;
    w->same_document = false;
    w->url = w->classic = w->destination = w->event = JS_UNDEFINED;
    navigation_abort_work_start(&w->abort);
    STEP_CB_FOREACH(w->cb, k) w->cb[k] = JS_UNDEFINED;
}

void navigate_event_fire_work_visit(JSContext *ctx, NavigateEventFireWork *w, JSStepVisit *v)
{
    int k;

    v->val(ctx, &w->url);
    v->val(ctx, &w->classic);
    v->val(ctx, &w->destination);
    v->val(ctx, &w->event);
    navigation_abort_work_visit(ctx, &w->abort, v);
    STEP_CB_FOREACH(w->cb, k) v->val(ctx, &w->cb[k]);
}

void navigate_event_fire_work_release(JSContext *ctx, NavigateEventFireWork *w)
{
    int k;

    DCHECK(w->stage < NEF_STAGE_COUNT,
           NEF_ALGORITHM " was abandoned at a cursor its stage declaration does not name — a record torn down "
           "at a stage nobody declares is one whose event nobody can say was dispatched");
    JS_FreeValue(ctx, w->url);
    JS_FreeValue(ctx, w->classic);
    JS_FreeValue(ctx, w->destination);
    JS_FreeValue(ctx, w->event);
    w->url = w->classic = w->destination = w->event = JS_UNDEFINED;
    navigation_abort_work_release(ctx, &w->abort);
    STEP_CB_FOREACH(w->cb, k) {
        JS_FreeValue(ctx, w->cb[k]);
        w->cb[k] = JS_UNDEFINED;
    }
    /* AND THE ALGORITHM IS BACK AT ITS FIRST STEP: a record a caller holds names the step it would be ENTERED
       at rather than the last one it rested at, so a record reached again is asked from the wrapper's step 1.
       A teardown is not a transition, which is why this is an assignment and not a STEP_GOTO — the record is
       abandoned wherever it stood, and its fire request's cursor is legitimately mid-flight. */
    w->stage = NEF_ABORT;
    w->phase = 0;
}

void navigate_event_fire_push_replace_reload_begin(JSContext *ctx, NavigateEventFireWork *w,
                                                   const char *navigation_type, const char *destination_url,
                                                   bool is_same_document, const StructuredData *classic_state)
{
    DCHECK(w->stage == NEF_ABORT && JS_IsUndefined(w->url),
           "§7.2.6.10.4's fire-a-push/replace/reload-navigate-event was created over a work record that is "
           "already carrying one — a record is started, driven to its answer and only then re-used, because "
           "the second creation would discard the first navigation's event with the Navigation still holding "
           "it as its ongoing navigate event");
    DCHECK(navigation_type != NULL &&
           (!strcmp(navigation_type, "push") || !strcmp(navigation_type, "replace") ||
            !strcmp(navigation_type, "reload")),
           "§7.2.6.10.4's fire-a-push/replace/reload-navigate-event was given a NavigationType outside the "
           "three its name lists — a \"traverse\" arrives through fire-a-traverse-navigate-event, which takes "
           "a session history entry rather than a URL and which has its own call site");
    DCHECK(destination_url != NULL && *destination_url,
           "§7.2.6.10.4's fire-a-push/replace/reload-navigate-event was given no destinationURL — its caller "
           "holds one by the time it fires, because the navigate event is what asks the page's listeners about "
           "THAT address");
    w->navigation_type = navigation_type;
    w->same_document = is_same_document;
    w->url = JS_NewString(ctx, destination_url);
    CHECK(!JS_IsException(w->url), "navigate event: the destination URL could not be allocated");
    /* THE CLASSIC HISTORY API STATE IS COPIED HERE AND NOWHERE ELSE. Its producer is §7.2.5's `pushState`,
       which serialized it before this navigation existed and frees its bytes when its own stage ends; the
       navigate event is dispatched a rest point later, so a borrowed pointer would name freed memory by the
       time the event is built. An ArrayBuffer is what carries bytes across a park and a session. */
    if (classic_state) {
        DCHECK(classic_state->buf != NULL,
               "§7.2.6.10.4 was handed a classic history API state of no bytes — it is a SERIALIZED state or "
               "null, and StructuredSerializeForStorage of any value at all produces bytes, so an empty one is "
               "a caller that meant null");
        w->classic = JS_NewArrayBufferCopy(ctx, classic_state->buf, classic_state->len);
        CHECK(!JS_IsException(w->classic),
              "navigate event: the classic history API state could not be allocated");
    } else {
        w->classic = JS_NULL;
    }
}

/* §7.2.6.10.4's inner algorithm step 20's four-conjunct hashChange test, over the two URLs it names: step 19's
   currentURL, which is the Document's own address, and `target`, which is destination's URL serialized.
   THE FIRST CONJUNCT IS THE STANDARD'S OWN NOTE MADE MECHANICAL: "hashChange will be true for fragment
   navigations, but false for cases like history.pushState(undefined, '', '#fragment')" — which is why the
   classic history API state is asked about at all, and why it is asked FIRST. */
static bool nef_hash_change(JSContext *ctx, JSValueConst classic, const char *target, bool same_document)
{
    UrlRecord cur, dst;
    char *cur_bare, *dst_bare;
    bool same_but_fragment, fragment_differs;

    /* CONJUNCT 1: "event's classic history API state is null". */
    if (!JS_IsNull(classic)) return false;
    /* CONJUNCT 2: "destination's is same document is true". */
    if (!same_document) return false;
    url_record_init(&cur);
    url_record_init(&dst);
    CHECK(url_parse(&cur, document_base_url(ctx), strlen(document_base_url(ctx)), NULL),
          "this realm's document address is not a URL — the host captured something this engine cannot make a "
          "principal out of");
    CHECK(url_parse(&dst, target, strlen(target), NULL),
          "§7.2.6.10.4 was handed a destination URL that does not parse — its callers hold a URL record and "
          "serialize it, so a failure here is a serialization this engine cannot read back");
    /* CONJUNCT 3: "destination's URL equals currentURL with exclude fragments set to true" — URL §4.6's
       equality IS serialization equality, and the exclude-fragments form is the serializer's own flag. */
    cur_bare = url_serialize(&cur, true);
    dst_bare = url_serialize(&dst, true);
    CHECK(cur_bare != NULL && dst_bare != NULL, "navigate event: a URL could not be serialized");
    same_but_fragment = !strcmp(cur_bare, dst_bare);
    free(cur_bare);
    free(dst_bare);
    /* CONJUNCT 4: "destination's URL's fragment is not identical to currentURL's fragment". A NULL fragment is
       the spec's null, and null is not identical to the EMPTY string — `#` on the end of the address is a
       fragment of "" and reaching it from a URL with none is a fragment navigation. */
    if (!cur.fragment || !dst.fragment) fragment_differs = cur.fragment != dst.fragment;
    else                                fragment_differs = strcmp(cur.fragment, dst.fragment) != 0;
    url_record_free(&cur);
    url_record_free(&dst);
    return same_but_fragment && fragment_differs;
}

/* §7.2.6.10.4's COMMIT A NAVIGATE EVENT, given the event and (here always) a null apiMethodTracker.
 *
 * IT IS A TOP-LEVEL STEP OF THE INNER ALGORITHM AND NOT PART OF ITS INTERCEPTION BRANCH — step 30 reads "if
 * event's navigation precommit handler list is empty then commit event given apiMethodTracker", at the same
 * depth as step 29's `if`, so a navigation nobody intercepted commits too. What it DOES for such a navigation is
 * the interesting part: its step 7 switch is guarded by the interception state, so the URL and history update
 * steps it would otherwise run are the INTERCEPTED navigation's, and the caller (§7.2.5 step 10) runs its own. */
static void nef_commit_a_navigate_event(JSContext *ctx, JSValueConst event)
{
    JSValue signal;
    bool aborted;

    /* STEPS 1-2: "let navigation be event's target" and "let navigable be event's relevant global object's
       navigable" — this realm's Navigation and this realm's navigable, which is what every algorithm in this
       component means by both. */
    /* STEP 3: "if event's relevant global object's associated Document is not fully active, then return." A
       `navigate` listener that removes this frame reaches it, which is why it is a test and not an assert. */
    if (!document_fully_active(ctx)) return;
    /* STEP 4: "if event's abort controller's signal is aborted, then return." */
    signal = navigate_event_signal(ctx, event);
    aborted = abort_signal_aborted(ctx, signal);
    JS_FreeValue(ctx, signal);
    if (aborted) return;
    /* STEP 5's endResultIsSameDocument has exactly one reader — step 9, "if endResultIsSameDocument is false
       and apiMethodTracker is non-null, then clean up apiMethodTracker" — and the tracker is null for every
       navigation this build can start (§7.2.6.7's `navigate` and `reload` are the only producers of one). So
       the value is not computed here rather than computed and dropped: it comes back with the tracker.
       STEPS 6 AND 10's PREPARE / CLEAN UP AFTER RUNNING SCRIPT have nothing to suppress in this engine, for
       the reason core/frame/navigation.c gives at §7.2.6.4 step 11: their own note says they exist to stop the
       JavaScript execution context stack becoming empty and forcing a microtask checkpoint, and this scheduler
       has no job-queue drain to trigger — every enqueued job is a flow in the one WFQ.
       STEP 7's switch is reached only when the interception state is not "none", and STEP 8's transition is
       null for the same reason; both are asserted against `intercept()` at the call site rather than here, so
       the assertion fires once per firing instead of once per step. */
}

int navigate_event_fire_run(JSContext *ctx, NavigateEventFireWork *w, JSValue in, JSValue **out_cb,
                            int *out_argc, bool *pcontinue)
{
    JSValue nav, signal, state;
    StructuredData d, classic_bytes;
    const StructuredData *classic = NULL;
    const char *url;
    bool not_canceled = true, can_intercept, hash_change;
    int r;

    DCHECK(pcontinue != NULL, "§7.2.6.10.4's inner algorithm was driven with nowhere to put its answer — its "
                              "return value is what decides whether the navigation that asked proceeds");

    STEP_DISPATCH(NEF_STAGES, w->stage, NEF_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(NEF_ABORT);
    DCHECK(JS_IsString(w->url),
           "§7.2.6.10.4's wrapper was entered over a work record that was never created — "
           "navigate_event_fire_push_replace_reload_begin is what takes the destination URL, and an algorithm "
           "with no destination has nothing to ask the page about");
    /* THE WRAPPER'S STEP 1 is "let document be navigation's relevant global object's associated Document" —
       this realm's, which every call below reaches through `ctx`.
       THE WRAPPER'S STEP 2: "INFORM THE NAVIGATION API ABOUT ABORTING NAVIGATION in document's node navigable",
       whose whole content is "while navigation's ongoing navigate event is not null: ABORT THE ONGOING
       NAVIGATION" — core/frame/navigation_abort.h's loop. With none ongoing it does nothing and answers on this
       same entry; with one ongoing it aborts it, which fires `abort` at that event's signal and `navigateerror`
       at the Navigation, so this is a stage and not a line. TWO SYNCHRONOUS `history.pushState()` CALLS IN ONE
       TURN are the reachable case, and they are why inner step 23's assert below can be an assert. */
    r = navigation_abort_inform_run(ctx, &w->abort, in, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    /* AND IT RETURNS RATHER THAN RUNNING ON, for the reason the stage declaration gives: a body that sets its
       stage and falls into the next arm has crossed a boundary the driver never saw, so the label would claim a
       rest point the engine cannot park at. */
    STEP_GOTO(w->stage, NEF_PREPARE, &w->phase, &w->abort.phase, &w->abort.sig.phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(NEF_PREPARE);
    JS_FreeValue(ctx, in);   /* the abort has answered; this entry's answer belongs to nobody */
    /* THE WRAPPER'S STEP 3 is "if navigation has entries and events disabled, and apiMethodTracker is not
       null" — the tracker is null here (see navigate_event_fire.h), so the branch is not taken and its own
       note's reason for existing (a `navigateerror` handler that detached the Document) is what the step 4
       test below answers anyway.
       THE WRAPPER'S STEP 4: "if document is NOT FULLY ACTIVE, then return FALSE." */
    if (!document_fully_active(ctx)) {
        *pcontinue = false;
        return 0;
    }
    /* THE WRAPPER'S STEPS 5 AND 6 — "let event be the result of creating an event given NavigateEvent" and
       "set event's classic history API state to classicHistoryAPIState" — are performed at the inner
       algorithm's initializations below, as ONE construction. core/events/navigate_event.h states why: every
       one of §7.2.6.10.4's initializations happens before anything can observe the object, and an event
       visible between two of them would be an event whose `type` is not yet "navigate".
       THE WRAPPER'S STEPS 7-11 build the destination, and every one of the four fields is a value this
       operation was CREATED with — the URL and the same-document flag are the caller's, the entry is null
       (only fire-a-traverse-navigate-event sets one), and the state is StructuredSerializeForStorage(null)
       because the wrapper's navigationAPIState argument is at its default. */
    CHECK(structured_serialize(ctx, JS_NULL, &d) == 0,
          "navigate event: StructuredSerializeForStorage(null) failed — null is serializable by every clause "
          "of §2.7, so a refusal is the serializer being unable to allocate");
    state = JS_NewArrayBufferCopy(ctx, d.buf, d.len);
    CHECK(!JS_IsException(state), "navigate event: the destination's navigation API state could not be "
                                  "allocated");
    structured_data_free(ctx, &d);
    url = JS_ToCString(ctx, w->url);
    CHECK(url != NULL, "navigate event: the destination URL could not be read back");
    w->destination = navigation_destination_new(ctx, url, JS_NULL, state, w->same_document);
    CHECK(!JS_IsException(w->destination), "navigate event: the NavigationDestination could not be allocated");

    /* INNER STEP 1: "if navigation has entries and events disabled, then … return TRUE." Its three asserts are
       about state §7.2.6.7's methods are the only producers of, and it answers TRUE — a Document with no entry
       list has no navigate event to fire and the navigation proceeds unasked. */
    if (navigation_entries_and_events_disabled(ctx)) {
        JS_FreeCString(ctx, url);
        JS_FreeValue(ctx, w->destination);
        w->destination = JS_UNDEFINED;
        *pcontinue = true;
        return 0;
    }
    /* INNER STEPS 2, 3 AND 4 are the API METHOD TRACKER's, and there is none: step 2 asserts the ongoing one is
       null, step 3 looks the destination's entry's key up in the upcoming traverse trackers (and the entry is
       null here, which is the same fact one step round), and step 4 adopts whatever it found. §7.2.6.7's
       `navigate`, `reload`, `traverseTo`, `back` and `forward` are the only algorithms that make a tracker, and
       none of them is built (core/frame/navigation.h).
       INNER STEPS 5 AND 6 — the navigable and the document — are this realm's.
       INNER STEP 7: "if document can have its URL rewritten to destination's URL, AND either destination's is
       same document is true or navigationType is not 'traverse', then initialize event's canIntercept to true."
       The first conjunct is §7.2.5's own algorithm, asked of the component that owns it rather than derived a
       second time here — two derivations of one rule is where the second one goes wrong. */
    can_intercept = history_document_can_have_url_rewritten(ctx, url) &&
                    (w->same_document || strcmp(w->navigation_type, "traverse") != 0);
    /* INNER STEP 8's traverseCanBeCanceled has exactly one reader, step 9, and step 9 reads it only when
       navigationType IS "traverse" — its other disjunct is true for every other type. So the value is not
       computed here: its third conjunct is "navigation's relevant global object has HISTORY-ACTION ACTIVATION",
       which is HTML §6.6's user activation state, and this build has no producer for it. It arrives with
       fire-a-traverse-navigate-event, which is the only wrapper that can pass a "traverse".
       INNER STEP 9: "if either navigationType is not 'traverse' or traverseCanBeCanceled is true, then
       initialize event's cancelable to TRUE." */
    DCHECK(strcmp(w->navigation_type, "traverse") != 0,
           "§7.2.6.10.4's inner algorithm reached step 9 with a \"traverse\" — its cancelable is then step 8's "
           "traverseCanBeCanceled, whose third conjunct is history-action activation and which this build "
           "cannot compute. Build both with fire-a-traverse-navigate-event");
    /* INNER STEP 19: "let currentURL be document's URL", and INNER STEP 20's four-conjunct hashChange. */
    hash_change = nef_hash_change(ctx, w->classic, url, w->same_document);
    JS_FreeCString(ctx, url);
    /* INNER STEPS 17 AND 18: "set event's abort controller to a NEW AbortController created in navigation's
       relevant realm" and "initialize event's signal to event's abort controller's signal". The controller is
       held AS its signal — core/events/navigate_event.h states why: the only two things the standard ever does
       with it are signal abort ON the signal and read the signal out. */
    signal = abort_signal_new(ctx);
    CHECK(!JS_IsException(signal), "navigate event: the event's AbortController could not be allocated");
    /* INNER STEPS 10-16 AND 21-22, as the one construction the wrapper's step 5 collapses into:
         10 type "navigate", 11 navigationType, 12 destination, 13 downloadRequest (null — only
         fire-a-download-request-navigate-event passes a filename), 14 info (undefined — no tracker),
         15 hasUAVisualTransition (false — this user agent performs no visual transition),
         16 sourceElement (null — the wrapper's default), 21 userInitiated (false, because userInvolvement is
         "none": a script called this, not the browser UI), 22 formData (null — no form submission reaches
         this build). Each of the four defaults is a computed answer about THIS call site rather than a
         placeholder, and navigate_event_fire.h names the producer that turns each into a parameter. */
    /* THE CLASSIC HISTORY API STATE CROSSES AS THE BYTES IT IS. The record holds them in an ArrayBuffer
       because that is what parks; the event's constructor takes §2.7's serialized form, so this is a BORROWED
       view of the buffer's storage and must not be freed through structured_data_free — the buffer owns it. */
    if (!JS_IsNull(w->classic)) {
        classic_bytes.buf = JS_GetArrayBuffer(ctx, &classic_bytes.len, w->classic);
        DCHECK(classic_bytes.buf != NULL,
               "§7.2.6.10.4's work record held a classic history API state that is not the serialized bytes — "
               "navigate_event_fire_push_replace_reload_begin is its only writer and it copies them into an "
               "ArrayBuffer");
        classic = &classic_bytes;
    }
    w->event = navigate_event_new_to_fire(ctx, w->navigation_type, w->destination, can_intercept,
                                          /*cancelable*/ true, /*user_initiated*/ false, hash_change, signal,
                                          /*source_element*/ JS_NULL, classic);
    JS_FreeValue(ctx, signal);
    if (JS_IsException(w->event)) {
        w->event = JS_UNDEFINED;
        return JS_STEP_ABRUPT;
    }
    /* INNER STEPS 23 AND 24: "assert: navigation's ongoing navigate event is null" and "set navigation's
       ongoing navigate event to event". The event is what §7.2.6.4 step 14 comes back for, which is where a
       navigation's `navigatesuccess` and the clearing of this field live. */
    {
        JSValue ongoing = navigation_ongoing_navigate_event(ctx);
        bool none = JS_IsNull(ongoing);

        JS_FreeValue(ctx, ongoing);
        DCHECK(none, "§7.2.6.10.4's inner algorithm step 23 asserts the Navigation's ongoing navigate event is "
                     "null and this one's is not — the wrapper's step 2 is what makes that true, by running "
                     "§7.2.6.8's inform-the-navigation-API-about-aborting-navigation until the field IS null, "
                     "and it is the NEF_ABORT stage above. A non-null here means something set the field "
                     "BETWEEN that stage and this one, which nothing between them may do: every step from step "
                     "3 to this one is an engine action");
    }
    navigation_set_ongoing_navigate_event(ctx, w->event);
    /* INNER STEP 25: "set navigation's focus changed during ongoing navigation to false." */
    navigation_set_focus_changed(ctx, false);
    /* INNER STEP 26's SUPPRESS NORMAL SCROLL RESTORATION DURING ONGOING NAVIGATION is not a field of this
       build's Navigation, and adding one here would give it a writer and no reader: its two readers are the
       "traverse" arm of commit-a-navigate-event's switch and §7.2.6.10.5's scroll behavior, both of which are
       behind `intercept()`. The assertion for that is at step 29 below rather than restated here.
       AND THE STAGE ENDS. A body that sets its stage and runs on has crossed a boundary the driver never saw,
       so the label would claim a rest point the engine cannot park at; JS_STEP_YIELD asks the scheduler, which
       parks this navigation if a sibling flow outranks it and re-enters immediately if none does. */
    STEP_GOTO(w->stage, NEF_DISPATCH, &w->phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(NEF_DISPATCH);
    /* INNER STEP 27: "let dispatchResult be the result of DISPATCHING event at navigation." §2.9's dispatch,
       reached as a REQUEST because this algorithm can park — the page's `navigate` listeners run here, and one
       of them may `await` a fetch, loop, or start a navigation of its own. */
    nav = navigation_object(ctx);
    r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), nav, w->event, JS_UNDEFINED, in, &not_canceled,
                              out_cb, out_argc);
    JS_FreeValue(ctx, nav);
    if (r) return r;
    /* INNER STEP 28: "if dispatchResult is FALSE" — a listener called preventDefault() on a cancelable navigate
       event, which is a router refusing the navigation. Its first sub-step consumes history-action user
       activation for a "traverse" (there are none here, per step 9's assertion), and its second is the abort. */
    if (!not_canceled) {
        /* STEP 28.1: "if navigationType is 'traverse', then CONSUME HISTORY-ACTION USER ACTIVATION given
           navigation's relevant global object." There are none here, per step 9's assertion above, and it
           arrives with fire-a-traverse-navigate-event and HTML §6.6's user activation state together.
           STEP 28.2: "if event's abort controller's signal is NOT ABORTED, then abort the ongoing navigation
           given navigation." The signal IS already aborted when a `navigate` listener aborted this very
           navigation itself — a router that calls `preventDefault()` after something else already superseded
           the navigation — and the standard's test is what stops this aborting it twice. */
        bool aborted;

        signal = navigate_event_signal(ctx, w->event);
        aborted = abort_signal_aborted(ctx, signal);
        JS_FreeValue(ctx, signal);
        if (!aborted) {
            STEP_GOTO(w->stage, NEF_CANCELED, &w->phase, &w->abort.phase, &w->abort.sig.phase, NULL);
            return JS_STEP_YIELD;
        }
        /* STEP 28.3: "return false." */
        *pcontinue = false;
        return 0;
    }
    /* INNER STEPS 29 AND 31 ARE THE INTERCEPTION HALF, and the whole of it is unreachable while `intercept()`
       is absent: the interception state is "none" for every event this build fires, the navigation precommit
       handler list and the navigation handler list are both empty, and there is no NavigationTransition to
       build. That is asserted against the OPERATION rather than written down, so the day it lands this fires
       at the step whose other arm has to be written — which is what core/events/navigate_event.h says this
       algorithm would do. */
    realm_awaits(ctx, "NavigateEvent.prototype.intercept",
                 "HTML §7.2.6.10.4's inner navigate event firing algorithm steps 29-31 are reachable now that "
                 "`intercept()` can move an event's INTERCEPTION STATE off \"none\". Write them here: step 29 "
                 "takes the current entry as fromNHE, asserts it is not null, and sets navigation's TRANSITION "
                 "to a new NavigationTransition (§7.2.6.8's interface — navigationType, from entry, "
                 "destination, and a committed and a finished promise, both marked as handled); step 30 stays "
                 "as it is when the precommit handler list is empty; step 31 mints a "
                 "NavigationPrecommitController (§7.2.6.10.2), invokes each precommit handler with it, and "
                 "WAITS FOR ALL of the promises they return before committing. Step 32's answer then stops "
                 "being unconditional: it is TRUE only while the interception state is \"none\", and an "
                 "intercepted navigation answers FALSE because commit-a-navigate-event runs the URL and "
                 "history update steps itself. §7.2.6.10.5's finish, potentially-reset-the-focus and "
                 "potentially-process-scroll-behavior land with them, and so does §7.2.6.10.4 step 26's "
                 "SUPPRESS NORMAL SCROLL RESTORATION field, whose only readers they are");
    /* INNER STEP 30: "if event's navigation precommit handler list is EMPTY then COMMIT EVENT given
       apiMethodTracker" — it is empty (only `intercept()` appends to it), so every navigate event this build
       fires and nobody canceled commits here. */
    nef_commit_a_navigate_event(ctx, w->event);
    /* INNER STEP 32: "if event's interception state is 'none', then return TRUE." It is, so this is the answer;
       step 33's "return false" is the intercepted navigation's and arrives with the assertion above. */
    *pcontinue = true;
    return 0;

    STEP_ARM(NEF_CANCELED);
    /* INNER STEP 28.2, PERFORMED. §7.2.6.8's ABORT THE ONGOING NAVIGATION — the SINGLE abort rather than the
       loop, because the standard names the algorithm here and not the informing wrapper, and because the event
       it is about is the one this machine has just dispatched. It signals abort on that event's controller (so a
       listener that handed `event.signal` to a fetch is told to stop) and fires `navigateerror` at the
       Navigation, which is why this is a stage of its own. */
    r = navigation_abort_ongoing_run(ctx, &w->abort, in, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    /* STEP 28.3: "return false" — the navigation the page refused must not proceed. */
    *pcontinue = false;
    return 0;
}

/* ---- §7.2.6.10.4's NAVIGATE EVENT INTERCEPT COMMIT HANDLER STEPS -------------------------------------------
 *
 * WHAT ENDS A NAVIGATE EVENT. The inner algorithm sets the Navigation's ongoing navigate event and never clears
 * it; §7.2.6.4's update-the-navigation-API-entries step 14 runs these steps, and their success steps are what
 * null it out and fire `navigatesuccess`. A navigation whose entries never update therefore never finishes —
 * which is the standard's own shape, and why the wrapper's step 2 aborts one that is still ongoing.
 *
 * AND THEY RUN IN A MICROTASK, WHICH IS WHY THEY ARE A JOB. The standard reaches them through "WAIT FOR ALL of
 * promisesList", and with no handlers to invoke promisesList is « a promise resolved with undefined » — one
 * turn of the microtask queue, not the caller's turn. That ordering is observable and the standard's own note
 * says so: the sequence a page sees is `currententrychange` handlers, then `intercept()` handlers, then promise
 * handlers, and running these steps synchronously inside step 14 would fire `navigatesuccess` between the
 * `currententrychange` and `dispose` dispatches instead of after both. An enqueued job is one turn of that same
 * queue, and it is a first-class flow in the one WFQ. */
#define NEFC_STAGES(X)                                                                                       \
    X(NEFC_END,     "HTML §7.2.6.10.4 navigate event intercept commit handler steps, the wait-for-all success "\
                    "steps 1-6 (the fully-active and aborted tests, asserting the event is the Navigation's "  \
                    "ongoing navigate event, setting that to null, and finishing the event given true)")      \
    X(NEFC_SUCCESS, "HTML §7.2.6.10.4 navigate event intercept commit handler steps, the wait-for-all success "\
                    "step 7 (fire an event named navigatesuccess at navigation)")
enum { NEFC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NEFC_STEPS[] = { NEFC_STAGES(JS_STEP_STAGE_LABEL) NULL };
#define NEFC_ALGORITHM "HTML §7.2.6.10.4 the navigate event intercept commit handler steps"

typedef struct {
    JSStepHdr   hdr;    /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t     phase;
    JSValue     ev;     /* the `navigatesuccess` event, held across the dispatch (owned) */
    EventFireCb cb;
} NefCommitState;

static void js_nef_commit_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    NefCommitState *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static int js_nef_commit_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    NefCommitState *s = st;
    /* THE EVENT CAME WITH THE JOB. §7.2.6.4 reads the ongoing navigate event at ITS step 10 and hands it to
       step 14, so the event these steps are about is the one that was ongoing when the update began — and
       between those two steps the `currententrychange` and `dispose` listeners run, any of which can start a
       navigation and replace it. Reading it back off the Navigation here would end whatever navigation
       happened to be ongoing when the microtask ran, which is a different navigation with the same name. It is
       the job's ARGUMENT for that reason, and step 3's assert below is the standard checking the same thing. */
    JSValueConst event = step_arg(&s->hdr, 0);
    JSValue nav, signal, ongoing;
    bool aborted, same;
    int r;

    STEP_DISPATCH(NEFC_STAGES, s->hdr.stage, NEFC_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(NEFC_END);
    JS_FreeValue(ctx, cb_result);
    /* THE SLOTS ARE UNDEFINED BEFORE THEY ARE ANYTHING ELSE. A step state is js_mallocz'd, and a zeroed JSValue
       is the INTEGER 0 (JS_TAG_INT is 0) — so the `navigatesuccess` slot would read as a real value the stage
       below would try to dispatch, and the fork's visit would take a reference to an integer. This is the one
       entry that precedes every other. */
    {
        int k;

        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
    }
    DCHECK(navigate_event_is(event),
           "§7.2.6.10.4's commit handler job was enqueued with something that is not a NavigateEvent — "
           "navigate_event_intercept_commit is its only producer and §7.2.6.4 step 14 its only caller");
    /* SUCCESS STEP 1: "if event's relevant global object is not fully active, then ABORT THESE STEPS." A
       `dispose` listener that removed this frame reaches it. */
    if (!document_fully_active(ctx)) return JS_STEP_DONE;
    /* SUCCESS STEP 2: "if event's abort controller's signal is aborted, then abort these steps." */
    signal = navigate_event_signal(ctx, event);
    aborted = abort_signal_aborted(ctx, signal);
    JS_FreeValue(ctx, signal);
    if (aborted) return JS_STEP_DONE;
    /* SUCCESS STEP 3: "assert: event equals navigation's ongoing navigate event." */
    ongoing = navigation_ongoing_navigate_event(ctx);
    same = JS_VALUE_GET_PTR(ongoing) == JS_VALUE_GET_PTR(event);
    JS_FreeValue(ctx, ongoing);
    DCHECK(same, "§7.2.6.10.4's commit handler success steps assert that the event they are ending is the "
                 "Navigation's ongoing navigate event, and it is not — a navigation started and finished "
                 "between §7.2.6.4 step 10 reading it and this microtask running. §7.2.6.8's ABORT THE ONGOING "
                 "NAVIGATION is what the standard uses to make that impossible: it runs at the START of every "
                 "navigation (core/frame/navigation_abort.c, driven by this file's NEF_ABORT stage) and leaves "
                 "the field null, so a navigation that STARTED in between has aborted this one and success "
                 "step 2's aborted-signal test above should already have answered");
    /* SUCCESS STEP 4: "set navigation's ongoing navigate event to null." */
    navigation_set_ongoing_navigate_event(ctx, JS_NULL);
    /* SUCCESS STEP 5: "FINISH event given true" — §7.2.6.10.5's, whose step 3 is "if event's interception
       state is 'none', then return". It is, for every event this build fires, so the whole algorithm is that
       return; its other arms are the interception half and are asserted against the operation that reaches
       them. SUCCESS STEP 6's "resolve the finished promise for apiMethodTracker" needs a tracker, and there is
       none. */
    realm_awaits(ctx, "NavigateEvent.prototype.intercept",
                 "HTML §7.2.6.10.5's FINISH A NavigateEvent is reachable now that `intercept()` can move an "
                 "event's interception state off \"none\": write it as its own component beside this file — "
                 "the \"intercepted\" arm (assert didFulfill is false, assert the precommit handler list is "
                 "not empty, set the state to \"finished\"), then POTENTIALLY RESET THE FOCUS (the "
                 "Navigation's focus-changed flag, the focus reset behavior, the autofocus delegate or the "
                 "body element, and the focusing steps), then POTENTIALLY PROCESS SCROLL BEHAVIOR for a "
                 "didFulfill of true, then the state becomes \"finished\". Its other caller is "
                 "process-navigate-event-handler-failure, which lands with §7.2.6.8's abort");
    STEP_GOTO(s->hdr.stage, NEFC_SUCCESS, &s->phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(NEFC_SUCCESS);
    /* SUCCESS STEP 7: "FIRE AN EVENT NAMED navigatesuccess at navigation." DOM's fire-an-event with neither of
       its two flags, because §7.2.6.10.4 sets none of them; TRUSTED, because the user agent fired it. There is
       no interface of its own: §7.2.6.2 declares `navigatesuccess` with no "using" clause, so it is a plain
       Event — the same shape as the `dispose` core/frame/navigation.c fires. */
    if (JS_IsUndefined(s->ev)) {
        s->ev = event_new(ctx, "navigatesuccess", /*bubbles*/ false, /*cancelable*/ false);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
    }
    nav = navigation_object(ctx);
    r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), nav, s->ev, JS_UNDEFINED, cb_result, NULL,
                              out_cb, out_argc);
    JS_FreeValue(ctx, nav);
    if (r) return r;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    /* SUCCESS STEPS 8 AND 9 are the TRANSITION's — "if navigation's transition is not null, then resolve its
       finished promise with undefined" and "set navigation's transition to null" — and a transition exists only
       for an intercepted navigation, which is the assertion the stage before this one carries. */
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_nef_commit_def = {
    sizeof(NefCommitState), js_nef_commit_step, NULL, 0, .visit = js_nef_commit_visit,
    .algorithm = NEFC_ALGORITHM,
    .steps = NEFC_STEPS
};
static int g_commit_stepid = -1;

void navigate_event_fire_init(JSContext *ctx)
{
    DCHECK(g_commit_stepid < 0,
           "navigate_event_fire_init ran twice — the commit handler's step definition is registered once per "
           "AGENT, and a second id would leave two machines for one algorithm");
    /* REGISTERED HERE RATHER THAN AT THE FIRST COMMIT, because the id is agent state and this is where agent
       state is declared: a lazy registration is a second place the machine can come into existence, and it is
       the place a `release` cannot see. */
    g_commit_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_nef_commit_def);
    CHECK(g_commit_stepid >= 0,
          "the navigate event commit handler's step definition could not be registered");
}

void navigate_event_fire_free(void)
{
    /* What the agent holds is the registered id, and it names a machine in a runtime that is going away with
       it. Everything else this component makes belongs to the flow that made it. */
    g_commit_stepid = -1;
}

void navigate_event_intercept_commit(JSContext *ctx, JSValueConst event)
{
    JSValueConst argv[1];
    JSValue fn;

    DCHECK(navigate_event_is(event),
           "§7.2.6.4 step 14 ran the navigate event intercept commit handler steps over something that is not "
           "a NavigateEvent — the only value it may pass is the ongoing navigate event it read at its step 10");
    /* STEPS 1-3: "let promisesList be an empty list", "for each handler of event's NAVIGATION HANDLER LIST,
       append the result of invoking handler with an empty arguments list", and "if promisesList's size is 0,
       then set promisesList to « a promise resolved with undefined »". The handler list is empty for every
       event this build fires — `intercept()` is its only writer — so the third step is the one that runs, and
       the assertion for the day the other two do is at the inner algorithm's step 29.
       STEP 4: "WAIT FOR ALL of promisesList, with the following success steps" — over one already-resolved
       promise that is one turn of the microtask queue, which is what this job is. */
    DCHECK(g_commit_stepid >= 0,
           "§7.2.6.10.4's commit handler steps ran before navigate_event_fire_init registered their machine — "
           "core/platform.c declares this component with the agent, before any realm exists to navigate");
    /* THE CALLEE IS MINTED IN THIS REALM, for the reason core/frame/session_history.c gives its traversal job:
       a C function runs in the realm that DEFINED it, and every step of the algorithm reads THIS document's
       Navigation off `ctx`; one held in a static would end whichever document's navigation enqueued first. */
    fn = JS_NewCFunction2(ctx, NULL, "navigateEventCommit", 1, JS_CFUNC_step, g_commit_stepid);
    CHECK(!JS_IsException(fn), "the navigate event commit job's callee could not be allocated");
    argv[0] = event;
    JS_EnqueueCallJob(ctx, fn, 1, argv);
    JS_FreeValue(ctx, fn);
}
