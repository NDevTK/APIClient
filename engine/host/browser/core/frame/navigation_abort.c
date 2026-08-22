/* ABORTING AN ONGOING NAVIGATION — HTML §7.2.6.8. See navigation_abort.h for what this component is, why it is
   a request, and what of §7.2.6.8 is deliberately still absent.

   THE STEP NUMBERS BELOW ARE THE STANDARD'S OWN LISTS, COUNTED. §7.2.6.8 writes ABORT THE ONGOING NAVIGATION as
   seven steps, ABORT A NavigateEvent as ten, and INFORM THE NAVIGATION API ABOUT ABORTING NAVIGATION as three
   whose last one is the while; each label and each comment quotes the step's text as well as its number, so a
   reader can check the number against the words rather than against a count. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/abort.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/navigate_event.h"
#include "core/events/report_exception.h"
#include "core/frame/navigation.h"
#include "core/frame/navigation_abort.h"
#include "core/realm.h"

/* WHERE A PARKED ABORT IS, AS A STEP OF §7.2.6.8.
 *
 * THREE REST POINTS AND EVERY ONE OF THEM IS THE PAGE'S CODE — which is the whole reason this is a machine. The
 * first stage is the two algorithms' O(1) heads run as one: reading a slot, clearing a flag, minting a
 * DOMException, testing and setting a flag, and nulling a slot. Nothing in that range can grow with anything the
 * page controls and the algorithm does not stop inside it, so it is one label that says the range in those
 * terms; the stage ENDS at the first step that runs code. */
#define NAB_STAGES(X)                                                                                        \
    X(NAB_ABORT,   "HTML §7.2.6.8 inform the navigation API about aborting navigation step 3's while "        \
                   "condition, abort the ongoing navigation steps 1-7 and abort a NavigateEvent steps 1-2 "   \
                   "(the ongoing navigate event, clearing focus-changed, the AbortError DOMException, the "   \
                   "canceled flag, and setting the ongoing navigate event to null — a range of ONE O(1) "     \
                   "engine action per step: one slot read, two flag writes, one exception mint, one slot "    \
                   "write, none of them over anything that can grow)")                                        \
    X(NAB_SIGNAL,  "HTML §7.2.6.8 abort a NavigateEvent step 3 (signal abort on event's abort controller "    \
                   "given reason)")                                                                          \
    X(NAB_EXTRACT, "HTML §7.2.6.8 abort a NavigateEvent step 4 (errorInfo is the result of extracting error " \
                   "information from reason)")                                                               \
    X(NAB_FIRE,    "HTML §7.2.6.8 abort a NavigateEvent step 6 (fire an event named navigateerror at "        \
                   "navigation using ErrorEvent, with additional attributes initialized according to "        \
                   "errorInfo)")
enum { NAB_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NAB_STEPS[] = { NAB_STAGES(JS_STEP_STAGE_LABEL) NULL };
#define NAB_STAGE_COUNT ((int)(sizeof NAB_STEPS / sizeof *NAB_STEPS) - 1)
/* ONE NAME FOR THE ALGORITHMS THESE STAGES ARE STEPS OF — the dispatch's abort and the teardown's assertion
   share it, so neither can name a different algorithm from the other. */
#define NAB_ALGORITHM "HTML §7.2.6.8 aborting an ongoing navigation (the loop, the abort, and abort a "        \
                      "NavigateEvent)"

void navigation_abort_work_start(NavigationAbortWork *w)
{
    int k;

    /* THE SLOTS ARE UNDEFINED BEFORE THEY ARE ANYTHING ELSE — a zeroed JSValue is the INTEGER 0, not undefined
       (JS_TAG_INT is 0), so a slot read before it is written yields a real value the page can see. Same rule and
       same reason as core/frame/navigate_event_fire.c's work record. */
    w->stage = NAB_ABORT;
    w->phase = 0;
    w->event = w->reason = w->ev = JS_UNDEFINED;
    abort_signal_work_start(&w->sig);
    STEP_CB_FOREACH(w->cb, k) w->cb[k] = JS_UNDEFINED;
}

void navigation_abort_work_visit(JSContext *ctx, NavigationAbortWork *w, JSStepVisit *v)
{
    int k;

    v->val(ctx, &w->event);
    v->val(ctx, &w->reason);
    v->val(ctx, &w->ev);
    abort_signal_work_visit(ctx, &w->sig, v);
    STEP_CB_FOREACH(w->cb, k) v->val(ctx, &w->cb[k]);
}

void navigation_abort_work_release(JSContext *ctx, NavigationAbortWork *w)
{
    int k;

    DCHECK(w->stage < NAB_STAGE_COUNT,
           NAB_ALGORITHM " was abandoned at a cursor its stage declaration does not name — a record torn down "
           "at a stage nobody declares is one whose `navigateerror` nobody can say was fired");
    JS_FreeValue(ctx, w->event);
    JS_FreeValue(ctx, w->reason);
    JS_FreeValue(ctx, w->ev);
    w->event = w->reason = w->ev = JS_UNDEFINED;
    abort_signal_work_release(ctx, &w->sig);
    STEP_CB_FOREACH(w->cb, k) {
        JS_FreeValue(ctx, w->cb[k]);
        w->cb[k] = JS_UNDEFINED;
    }
    /* AND THE ALGORITHM IS BACK AT ITS FIRST STEP: a record a caller holds names the step it would be ENTERED
       at rather than the last one it rested at. A teardown is not a transition, which is why this is an
       assignment and not a STEP_GOTO — the record is abandoned wherever it stood, and its own sub-requests'
       cursors are legitimately mid-flight. */
    w->stage = NAB_ABORT;
    w->phase = 0;
}

/* THE ONE MACHINE BOTH ENTRY POINTS DRIVE. `loop` is the difference between the standard's two algorithms and
   nothing else is: with it, the machine's last stage jumps back to its first and the while condition at the top
   of that stage is what ends it; without it, the last stage answers. It is a PARAMETER rather than a field
   because a caller's identity does not change across a suspension — every re-entry comes from the same call
   site — and because a field would be a second statement of which algorithm is running. */
static int nab_run(JSContext *ctx, NavigationAbortWork *w, bool loop, JSValue in,
                   JSValue **out_cb, int *out_argc)
{
    JSValue nav, signal;
    int r;

    STEP_DISPATCH(NAB_STAGES, w->stage, NAB_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(NAB_ABORT);
    JS_FreeValue(ctx, in);   /* nothing has asked for anything yet, so this entry's answer belongs to nobody */
    /* THE PREVIOUS TURN'S SIGNAL-ABORT RECORD IS GIVEN BACK AND RE-STARTED HERE. §3.2's request is a one-shot:
       it ends at its own DONE stage still holding the list of signals whose abort steps it ran, and a record
       re-entered at DONE returns immediately having aborted nothing. So a second turn of the loop over the
       same record would signal no abort at all — silently, since its return value is the same 0. */
    abort_signal_work_release(ctx, &w->sig);
    abort_signal_work_start(&w->sig);
    /* ABORT THE ONGOING NAVIGATION STEP 1: "let event be navigation's ongoing navigate event" — which is also
       INFORM THE NAVIGATION API ABOUT ABORTING NAVIGATION step 3's WHILE CONDITION, read once because it is one
       read of one field. */
    JS_FreeValue(ctx, w->event);
    w->event = navigation_ongoing_navigate_event(ctx);
    /* STEP 2: "assert: event is not null." For the loop the assert IS the condition and a null is how the loop
       ends; for the single abort it is the standard's own assert about its caller. */
    DCHECK(loop || !JS_IsNull(w->event),
           "§7.2.6.8's ABORT THE ONGOING NAVIGATION step 2 asserts the Navigation has an ongoing navigate "
           "event, and this one has none — its caller here is §7.2.6.10.4's inner algorithm step 28.2, which "
           "has just dispatched the very event it is ending, and the only algorithms that clear the field are "
           "this one's own abort-a-NavigateEvent step 2 and the commit handler success steps' step 4");
    if (JS_IsNull(w->event)) {
        w->event = JS_UNDEFINED;   /* the standard's null is not a reference; the slot's rest state is undefined */
        return 0;
    }
    /* STEP 3: "set navigation's focus changed during ongoing navigation to false." */
    navigation_set_focus_changed(ctx, false);
    /* STEP 4's SUPPRESS NORMAL SCROLL RESTORATION DURING ONGOING NAVIGATION is not a field of this build's
       Navigation, and this step is the IDENTITY over it: the flag is false from the Navigation's creation, and
       the only writer that can make it true is commit-a-navigate-event's "traverse" arm, which is reached only
       when the interception state is not "none". The field, its two readers (§7.2.6.10.5's scroll behavior and
       that arm) and this write all arrive with `intercept()`, and the assertion for that day is the one
       core/frame/navigate_event_fire.c makes at inner step 29 — restated here it would be a second copy of a
       two-sided statement, and the copy that is not maintained is the one that goes on being silent. */
    /* STEP 5: "if error was not given, then let error be a new AbortError DOMException created in navigation's
       relevant realm." No caller gives one — see navigation_abort.h. It is built by THROWING one and taking it
       back, which is this engine's only constructor for the interface and which runs none of the page's code;
       reading `DOMException` off the global would, since a page may replace it. */
    JS_ThrowDOMException(ctx, "AbortError", "%s",
                         "the ongoing navigation was aborted");
    JS_FreeValue(ctx, w->reason);
    w->reason = JS_GetException(ctx);
    CHECK(!JS_IsUndefined(w->reason),
          "§7.2.6.8 step 5's \"AbortError\" DOMException was built and no exception was live");
    /* STEP 6: "if event's dispatch flag is set, then set event's canceled flag to true." The flag IS set when a
       second navigation starts from inside a `navigate` listener, because the event is still mid-dispatch —
       and that is exactly why this writes the FLAG rather than running §2.2's set-the-canceled-flag algorithm,
       which would refuse on a navigate event whose `cancelable` is false (core/events/event.h). */
    if (event_dispatch_flag(ctx, w->event))
        event_set_canceled(ctx, w->event, true);
    /* STEP 7 is ABORT A NavigateEvent given event and error. ITS STEPS 1 AND 2 are "let navigation be event's
       relevant global object's navigation API" — this realm's, which every call here reaches through `ctx` —
       and "set navigation's ongoing navigate event to null". Nulling it BEFORE anything runs the page's code is
       what makes the loop above terminate: everything from the next stage on is the page's, and the page's code
       is what puts a new event there. */
    navigation_set_ongoing_navigate_event(ctx, JS_NULL);
    STEP_GOTO(w->stage, NAB_SIGNAL, &w->phase, &w->sig.phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(NAB_SIGNAL);
    /* ABORT A NavigateEvent STEP 3: "SIGNAL ABORT on event's abort controller given reason." §3.2's whole
       operation as a REQUEST, because two of its steps run code — the signal's abort algorithms, and then the
       `abort` event fired at the signal. This is what tells a `navigate` listener that handed `event.signal` to
       a `fetch()` that the navigation it was serving is over. */
    signal = navigate_event_signal(ctx, w->event);
    r = abort_signal_run(ctx, &w->sig, signal, w->reason, in, out_cb, out_argc);
    JS_FreeValue(ctx, signal);
    if (r > 0) return r;
    if (r < 0) return -1;
    STEP_GOTO(w->stage, NAB_EXTRACT, &w->phase, &w->sig.phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(NAB_EXTRACT);
    JS_FreeValue(ctx, in);
    /* STEP 4: "let errorInfo be the result of EXTRACTING ERROR INFORMATION from reason", materialized as the
       ErrorEvent step 6 fires with it — core/events/report_exception.h states why those are one value here. The
       extraction renders the reason's BACKTRACE, which is the page's own stack depth, so the stage ends here
       and the scheduler is asked between that walk and the page's handlers. */
    JS_FreeValue(ctx, w->ev);
    w->ev = extract_error_information(ctx, w->reason, "navigateerror", /*cancelable*/ false);
    CHECK(!JS_IsException(w->ev),
          "§7.2.6.8's `navigateerror` ErrorEvent could not be allocated");
    /* STEP 5: "if navigation's ongoing API method tracker is non-null, then REJECT THE FINISHED PROMISE for
       apiMethodTracker with reason." It is null for every navigation this build can start, so the branch is not
       taken — asserted against the operation that makes a tracker rather than written down here. */
    realm_awaits(ctx, "Navigation.prototype.navigate",
                 "HTML §7.2.6.8's ABORT A NavigateEvent step 5 is reachable now that §7.2.6.7's `navigate` can "
                 "SET UP A NAVIGATE/RELOAD API METHOD TRACKER. Write it here: if navigation's ongoing API "
                 "method tracker is non-null, REJECT THE FINISHED PROMISE for it with `reason` — §7.2.6.8's own "
                 "three steps (reject the tracker's committed promise, reject its finished promise, then CLEAN "
                 "UP the tracker). Its five sibling algorithms land with it, and §7.2.6.10.4's inner algorithm "
                 "steps 2-4 and commit-a-navigate-event's steps 5 and 9 stop being the assertions they are now");
    STEP_GOTO(w->stage, NAB_FIRE, &w->phase, &w->sig.phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(NAB_FIRE);
    /* STEP 6: "FIRE AN EVENT NAMED navigateerror at navigation using ErrorEvent, with additional attributes
       initialized according to errorInfo." DOM's fire-an-event with neither of its two flags, because §7.2.6.8
       sets none of them, and TRUSTED because the user agent fired it. Nothing branches on the result, which is
       why it is the one fire in this component that asks for no answer. */
    nav = navigation_object(ctx);
    r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), nav, w->ev, JS_UNDEFINED, in, NULL,
                              out_cb, out_argc);
    JS_FreeValue(ctx, nav);
    if (r) return r;
    JS_FreeValue(ctx, w->ev);
    JS_FreeValue(ctx, w->event);
    JS_FreeValue(ctx, w->reason);
    w->ev = w->event = w->reason = JS_UNDEFINED;
    /* STEPS 7-10 ARE THE TRANSITION'S — "if navigation's transition is null, then return", then rejecting its
       committed and finished promises with reason and nulling it. It is null for every navigation this build
       fires, because a NavigationTransition exists only for one whose interception state left "none". */
    realm_awaits(ctx, "NavigateEvent.prototype.intercept",
                 "HTML §7.2.6.8's ABORT A NavigateEvent steps 7-10 are reachable now that `intercept()` can "
                 "move an event's INTERCEPTION STATE off \"none\", which is what makes §7.2.6.10.4's inner "
                 "algorithm step 29 set the Navigation's TRANSITION. Write them here: return if the transition "
                 "is null, otherwise REJECT its committed promise with `reason`, REJECT its finished promise "
                 "with `reason`, and set the transition to null. They land with the NavigationTransition "
                 "interface itself (§7.2.6.8's five attributes) and with `Navigation.transition`");
    /* THE LOOP'S BACK EDGE — INFORM THE NAVIGATION API ABOUT ABORTING NAVIGATION step 3's while, whose
       condition is re-read at the top of the first stage. It RETURNS rather than jumping, because a turn of
       this loop is unbounded page code and the engine must be free to park between turns: the standard's own
       note says the `navigateerror` handler can start a whole new navigation, and that navigation's flows are
       exactly what the scheduler may want to run before the next turn. A body that jumped back would drive the
       field to null inside one C call, which is the drive-to-completion this design has no place for. */
    STEP_GOTO(w->stage, NAB_ABORT, &w->phase, &w->sig.phase, NULL);
    return loop ? JS_STEP_YIELD : 0;
}

int navigation_abort_ongoing_run(JSContext *ctx, NavigationAbortWork *w, JSValue in,
                                 JSValue **out_cb, int *out_argc)
{
    return nab_run(ctx, w, /*loop*/ false, in, out_cb, out_argc);
}

int navigation_abort_inform_run(JSContext *ctx, NavigationAbortWork *w, JSValue in,
                                JSValue **out_cb, int *out_argc)
{
    return nab_run(ctx, w, /*loop*/ true, in, out_cb, out_argc);
}
