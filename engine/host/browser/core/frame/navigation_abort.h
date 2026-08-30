/* ABORTING AN ONGOING NAVIGATION — HTML §7.2.6.8. See navigation_abort.c.
 *
 * WHAT IT IS. §7.2.6.10.4 sets the Navigation's ONGOING NAVIGATE EVENT when it dispatches `navigate` and only
 * one algorithm ever finishes that navigation successfully (the commit handler's success steps, which fire
 * `navigatesuccess`). Every other ending goes through here: a `navigate` listener that calls preventDefault(),
 * a second navigation started while the first is still ongoing, and — when §7.2.6.10.2 lands — a precommit
 * handler whose promise rejects. This component is the three algorithms §7.2.6.8 states for that ending.
 *
 * IT IS A REQUEST, AND ALL THREE OF ITS REST POINTS RUN THE PAGE'S CODE. Abort-a-NavigateEvent step 3 SIGNALS
 * ABORT on the event's abort controller — which runs the signal's abort algorithms and then fires `abort` at it,
 * so a listener that passed `event.signal` to a `fetch()` learns here that the navigation is over — and its step
 * 6 fires `navigateerror` at the Navigation. So the work record is the CALLING machine's, exactly as
 * core/dom/abort.h's AbortSignalWork and core/events/report_exception.h's record are: the caller visits it (a
 * fork mid-abort must not hand two arms one dispatch) and the caller releases it.
 *
 * AND IT IS A LOOP, WHICH IS WHY THE TWO ENTRIES BELOW ARE ONE MACHINE. §7.2.6.8's INFORM THE NAVIGATION API
 * ABOUT ABORTING NAVIGATION is "while navigation's ongoing navigate event is not null: abort the ongoing
 * navigation", and the standard's own note says why it is a while and not an if: aborting runs JavaScript (the
 * `navigateerror` handler), that JavaScript can start a new navigation, and the new one is superseded by the
 * completion of this one and so is itself signaled as aborted. Each turn of that loop is unbounded page code, so
 * the loop is the machine's own back edge — it returns JS_STEP_YIELD at every turn and the scheduler answers
 * from the frontier, rather than draining the field to null inside one C call.
 *
 * WHAT IS ABSENT AND WHERE IT SAYS SO.
 *   - Abort-the-ongoing-navigation's optional DOMException `error` is not a parameter: both callers this build
 *     has pass none, so step 5's "let error be a new AbortError DOMException" is the only arm there is, and a
 *     parameter whose every caller passes the default is a field with one writer. §7.4's window.stop() is what
 *     brings one.
 *   - Abort-a-NavigateEvent step 5's ONGOING API METHOD TRACKER has no producer (§7.2.6.7's `navigate`,
 *     `reload`, `traverseTo`, `back` and `forward` are the only ones), and its steps 7-10 are the TRANSITION's,
 *     which exists only for an intercepted navigation. Both are asserted against the operation that creates
 *     them rather than written down as a comment — see the body. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_ABORT_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_ABORT_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/abort.h"             /* AbortSignalWork — step 3's signal abort is a request of its own */
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request this work parks on */

typedef struct {
    uint8_t     stage;      /* NAB_STAGES in navigation_abort.c — which step of §7.2.6.8 this is at */
    uint8_t     phase;      /* event_target_fire_run's, for the `navigateerror` dispatch */
    /* THE EVENT THIS TURN OF THE LOOP IS ABORTING, taken at abort-the-ongoing-navigation step 1 and held to the
       end of that turn (owned; JS_UNDEFINED between turns). It is a field and not a re-read of the Navigation
       because abort-a-NavigateEvent step 2 NULLS the ongoing navigate event before its step 3 runs a line of
       the page's code — so from step 3 onwards the Navigation no longer names the event being aborted, and a
       `navigateerror` handler that starts a navigation has already put a DIFFERENT one there by step 6. */
    JSValue     event;
    JSValue     reason;     /* step 5's error: the "AbortError" DOMException this turn aborts with (owned) */
    JSValue     ev;         /* step 6's ErrorEvent, held across the dispatch (owned) */
    AbortSignalWork sig;    /* abort-a-NavigateEvent step 3's own request */
    EventFireCb cb;
} NavigationAbortWork;

void navigation_abort_work_start(NavigationAbortWork *w);
void navigation_abort_work_visit(JSContext *ctx, NavigationAbortWork *w, JSStepVisit *v);
/* Everything the visit names, released — for a holder that tears this record down itself rather than through a
   step definition's declaration. core/frame/navigate_event_fire.c's own release is that holder. */
void navigation_abort_work_release(JSContext *ctx, NavigationAbortWork *w);

/* §7.2.6.8's ABORT THE ONGOING NAVIGATION given THIS realm's Navigation — ONE abort, whose step 2 asserts that
   there is an ongoing navigate event to abort. Its caller is §7.2.6.10.4's inner algorithm step 28.2, which has
   just dispatched the event it is ending.
     JS_STEP_CALL or JS_STEP_YIELD = return it (it has parked on the page's code, or at one of its own rest
   points), 0 = the abort has finished, -1 = an abort algorithm threw. */
int navigation_abort_ongoing_run(JSContext *ctx, NavigationAbortWork *w, JSValue in,
                                 JSValue **out_cb, int *out_argc);

/* §7.2.6.8's INFORM THE NAVIGATION API ABOUT ABORTING NAVIGATION in THIS realm's navigable — the LOOP over the
   algorithm above, and the same return contract. Its caller is §7.2.6.10.4's push/replace/reload wrapper step 2.
     ITS STEP 1 IS ALREADY TRUE HERE: "if this algorithm is running on navigable's active window's relevant
   agent's event loop, then continue on to the following steps; otherwise queue a global task ... " — every
   caller in this build is a step machine of this document's own flow, which IS that event loop, so the queued
   arm belongs to the day a cross-instance caller reaches it (SECURITY.md's origin-keyed agent cluster is where
   the other side of that boundary lives). */
int navigation_abort_inform_run(JSContext *ctx, NavigationAbortWork *w, JSValue in,
                                JSValue **out_cb, int *out_argc);

/* HTML §7.2.6.8 "Ongoing navigation tracking"'s INFORM THE NAVIGATION API ABOUT CHILD NAVIGABLE DESTRUCTION —
 * which is HTML §7.3.1.6 "Navigable destruction"'s DESTROY A CHILD NAVIGABLE STEP 4. It is a step of THAT
 * algorithm that THIS standard defines, which is why it lives beside the loop it opens with rather than beside
 * the destruction it is a step of.
 *
 * IT WAS THE ONE STEP OF THE NINE WITH NO IMPLEMENTATION AND NO CRASH, AND TWO SITES SAID IT WAS DONE. The
 * container's own removing steps and the destruction entry both read "destroy-a-child-navigable steps 4-5"
 * while performing only step 5, so a removed `<iframe>` whose Navigation had an ONGOING NAVIGATE EVENT kept it
 * for ever: no `abort` at the event's AbortSignal, so a `fetch(url, {signal: event.signal})` a `navigate`
 * listener started was never cancelled; no `navigateerror`; and then §7.5.10 step 7 dropped the queued tasks
 * and step 8 nulled the browsing context underneath it. Nothing crashed and nothing could — the step was
 * described as performed, which is the failure mode a missing step does not have.
 *
 * `cctx` IS THE DESTROYED NAVIGABLE'S OWN REALM AND NEVER THE CONTAINER'S. All four steps are written over
 * `navigable`: step 1 informs the navigation API about aborting navigation IN NAVIGABLE, and steps 2-4 read
 * "navigable's active window's navigation API". Handing this the realm that removed the `<iframe>` would abort
 * the PARENT's ongoing navigation and reject the parent's trackers — a wrong answer indistinguishable from a
 * right one, because the parent has a Navigation of its own and the algorithm would run to completion on it.
 *
 * THE CALLER DECIDES WHETHER THERE IS A REALM AT ALL, because that is a question about the NAVIGABLE and this
 * component holds none. An UNMATERIALIZED navigable answers "nothing to do" as a COMPUTED result rather than a
 * skip: script has never run in a realm that does not exist, so nothing can have started a navigation in it and
 * nothing can have called a traversal method on its Navigation. Asking for its realm anyway would BUILD one in
 * order to abort nothing. A navigable whose active document is a PEER INSTANCE's is that instance's to inform,
 * for the same reason its unload listeners are the peer's to fire.
 *
 * IT IS SYNCHRONOUS, WHICH IS THE STANDARD'S ORDER: step 4 runs between step 3's clearing of the container's
 * content navigable and step 5's queued destruction, so a page that removes an `<iframe>` has had this happen
 * by the time its next line runs. What that costs when the loop actually has a turn to take is stated where it
 * is paid — see the assert in the body. */
void navigation_abort_child_destroyed(JSContext *cctx);

#endif
