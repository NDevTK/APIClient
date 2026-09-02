/* CLOSE WATCHERS — HTML §6.10.2 "Close watcher infrastructure". See close_watcher.c.
 *
 * WHAT THIS IS INFRASTRUCTURE FOR. §6.10.1 "Close requests" defines what a user does — its own examples are
 * "The Esc key on desktop platforms", the back button or gesture on Android, "Any assistive technology's
 * dismiss gesture", a game controller's back button — and §6.10.2 defines the ONE per-Window structure every
 * dismissable thing in the platform registers with, so that one such request closes exactly one of them.
 * HTML §6.12 The popover attribute's show popover, §6.10.3 The CloseWatcher interface's `new CloseWatcher()`
 * and §4.11.4 The dialog element's modal `dialog` are three establishers of the SAME struct in the SAME list.
 * That is why this is its own component and not a
 * member of any of them: a second copy of the group algebra, per establisher, is three copies that can
 * disagree about whether one Esc closes one popover or all of them.
 *
 * THE MANAGER IS PER WINDOW, SO IT IS A PER-REALM RECORD. "Each Window has a close watcher manager" — not each
 * Document — and in this engine a Window is a realm, so the record lives in core/realm.h's per-realm value
 * beside the two timestamps HTML §6.4.1 Data model defines, which core/html/user_activation.c holds the same
 * way for the same sentence. It is built WITH the realm rather than on first touch, for the reason that file states: a
 * record minted on the first read is built inside whichever flow happened to ask first, and that flow's
 * baseline becomes every sibling's.
 *
 * ITS GROUPS ARE JS ARRAYS, AND THE STANDARD PICKED THE SHAPE THAT FORCES IT. The manager's groups is "a list
 * of lists of close watchers", and §6.10.2's destroy removes a watcher FROM THE MIDDLE of a group and then
 * removes any group left empty — which is exactly the structure §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE is
 * about. A malloc'd list captured as head/tail pointers reverts the POINTERS on a context switch and leaves
 * the nodes reachable from nothing, which the runtime's own GC walk cannot see and no gate reports. As Arrays
 * the mutations are property writes the heap COW delta already captures, so one forked arm that established a
 * watcher and one that did not each read back their own manager, and a parked flow resumes with the one it
 * had. core/css/top_layer.c holds §3's two ordered sets this way on the same argument.
 *
 * A WATCHER NAMES ITS THREE ALGORITHMS BY A KIND, NOT BY A CLOSURE. §6.10.2's struct holds a cancel action, a
 * close action and a get enabled state, each "a list of steps". All three terms are §6.10.2's own; what an
 * establisher does is SUPPLY them, and the one HTML §6.12 The popover attribute supplies as its close action
 * is hide a popover, which is a step machine that parks. So a JS function object cannot hold them and a C
 * function pointer cannot survive a park or a cross-session resume. A kind id out of a registry FIXED AT THIS
 * ENUM'S DEFINITION can: §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED permits a position to name a
 * thing exactly where the set is the machine's own and cannot change
 * under a parked flow, which a compile-time enum is and a page-mutated map is not. Blink spells the same fact
 * `CloseWatcher::Delegate`.
 *
 * WHAT THIS COMPONENT HOLDS, WHICH IS ALL OF §6.10.2. Four algorithms that touch ONLY the manager — notify the
 * close watcher manager about user activation (3 steps), establish a close watcher (7), destroy a close
 * watcher (3), and the "active" predicate — and three that RUN A WATCHER'S ACTIONS: request to close (12),
 * close (5) and process close watchers (4). §6.10.1's close request steps (9) are the only algorithm of §6.10
 * this component does not hold, and they are not its to hold — see the fullscreen paragraph at the end.
 *   THE LINE BETWEEN THOSE TWO SETS IS WORTH KEEPING WRITTEN DOWN, because it is what decides the SHAPE
 * of everything below. The four only READ AND WRITE THE MANAGER, so each is a plain C call. The three
 * here RUN THE WATCHER'S THREE ACTIONS, so each is a step machine twice over: it runs the page's code (a
 * `cancel` event, a `close` event, hide a popover) and it asks §6.4.1's history-action activation, which
 * core/html/user_activation.h answers as a REQUEST that forks because the timestamp behind it is unknown
 * external state. That is why they are shaped as a REQUEST a calling machine drives (CloseWatcherRun below)
 * and not as functions: a C activation hosting a `cancel` handler is the drive-to-completion this engine
 * aborts on.
 *
 * WHAT IS HONESTLY ABSENT NOW, AND WHY EACH IS A RESIDUAL RATHER THAN A CRASH.
 *   (a) §6.10.1's CLOSE REQUEST STEPS have no caller and no home. Nothing in this build dispatches a close
 * request at all — no trusted `keydown`, no Esc, no back gesture — so process close watchers below is
 * REACHABLE ONLY from the component that will own those 9 steps. It shows as a page whose `CloseWatcher` fires
 * `cancel`/`close` for `requestClose()`/`close()` and NEVER for a user's close request, and as an
 * `allowedNumberOfGroups` that only ever rises, since step 3's decrement is the only fall it has.
 *   (b) THE CLOSE ACTION §6.12 SUPPLIES — hide a popover — is not reachable from the dispatch below, because
 * core/html/popover.c exports show popover and hide a popover only as IDL members and its show popover step 15
 * (the one establisher of a POPOVER-kind watcher) DFAILs before it establishes anything. So that arm of the
 * close-action dispatch DFAILs naming the export to make, and it is unreachable in this build rather than
 * wrong: no watcher of that kind exists to reach it.
 *
 * WHERE FULLSCREEN MEETS THIS, AND WHERE IT DOES NOT. §6.10.1's close request steps are 9 steps whose step 1
 * is "If document's fullscreen element is not null", whose two sub-steps are "Fully exit fullscreen given
 * document's node navigable's top-level traversable's active document" and "Return" — so a fullscreen document
 * SHORT-CIRCUITS the whole algorithm and never reaches step 7's "Let closedSomething be the result of
 * processing close watchers". Fullscreen therefore does NOT register a close watcher and does not belong on
 * this stack; it is a higher-priority arm of the same 9 steps. What the two share is that ONE component owns
 * those 9 steps — its step 1 asking the fullscreen model and its step 7 asking this one — and neither half
 * should grow a private copy of them. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CLOSE_WATCHER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CLOSE_WATCHER_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request an ACTION parks on */

/* WHICH ALGORITHM TRIPLE A WATCHER CARRIES — see the header note above for why this is an id and not a
   closure. TWO ENTRIES, because two establishers are in this build:
     §6.12 The popover attribute's show popover step 15 establishes a watcher "given element's relevant global
   object, with: cancelAction being to return true. closeAction being to hide a popover given element, true,
   true, false, and null. getEnabledState being to return true."
     §6.10.3 The CloseWatcher interface's constructor step 2 establishes one "given this's relevant global
   object, with: cancelAction given canPreventClose being to return the result of firing an event named cancel
   at this, with the cancelable attribute initialized to canPreventClose. closeAction being to fire an event
   named close at this. getEnabledState being to return true."
     §4.11.4 The dialog element adds its own entry with its own three when it lands, and its three are NOT
   either of these — its getEnabledState is "to return true if dialog's enable close watcher for request close
   is true or dialog's computed closed-by state is not …", which is the concrete reason the dispatch in
   close_watcher.c asks the kind for the enabled state rather than answering `true` from one place.
   AN ENTRY IS APPENDED AND NEVER INSERTED. The number is stored on the watcher and a parked flow resumes
   holding it, so it is §AN-INDEX-NAMES-A-THING's permitted case only while the set is FIXED AT THIS
   DEFINITION and only grows at the end: renumbering POPOVER would give a resumed watcher a different triple.
   An entry added WITHOUT its three is what close_watcher_establish's range DCHECK and the action dispatch's
   own `default` are between them for. */
typedef enum {
    CLOSE_WATCHER_KIND_POPOVER = 0,     /* HTML §6.12 The popover attribute — subject is the popover Element */
    CLOSE_WATCHER_KIND_CLOSE_WATCHER,   /* HTML §6.10.3 The CloseWatcher interface — subject is the instance */
    CLOSE_WATCHER_KIND_COUNT
} CloseWatcherKind;

/* Declared ONCE PER AGENT, from core/html's declaration point, and it must run BEFORE the first realm is
   built: a realm that missed the install has no manager, and §6.4.2 step 5.2 notifies one on every activation. */
void close_watcher_init(JSContext *ctx);
void close_watcher_free(JSRuntime *rt);

/* §6.10.2's "To NOTIFY THE CLOSE WATCHER MANAGER ABOUT USER ACTIVATION given a Window window", 3 steps —
   §6.4.2 Processing model's step 5.2, performed for each window of the activation notification's walk. `wctx`
   is the realm of the Window being notified, which is why it is a parameter and not the asking realm:
   §6.4.2's step 5 writes a SET of Windows and every one of them is a different realm in this agent. */
void close_watcher_notify_user_activation(JSContext *wctx);

/* §6.10.2's "To ESTABLISH A CLOSE WATCHER given a Window window, a list of steps cancelAction, a list of steps
   closeAction, and an algorithm that returns a boolean getEnabledState", 7 steps. `wctx` is the Window's
   realm; `subject` is the object the kind's three algorithms act on (HTML §6.12 passes the popover Element).
   OWNED — the caller frees, and the caller is also what holds the watcher for its lifetime (HTML §6.12 keeps it in
   the element's "popover close watcher"); the manager's own reference is dropped by destroy. */
JSValue close_watcher_establish(JSContext *wctx, CloseWatcherKind kind, JSValueConst subject);

/* §6.10.2's "To DESTROY A CLOSE WATCHER closeWatcher", 3 steps. Idempotent — destroying a watcher that is not
   in any group removes it from every group it is not in and compacts nothing, which is what the standard's
   own "remove closeWatcher from group" does for an absent item and what HTML §6.12's hide a popover relies on. */
void close_watcher_destroy(JSContext *wctx, JSValueConst watcher);

/* §6.10.2's "A close watcher closeWatcher is ACTIVE if closeWatcher's window's close watcher manager contains
   any list which contains closeWatcher." Read by request to close's step 1, by close's step 1, and as the
   two-sided assert on establish and destroy. */
bool close_watcher_is_active(JSContext *wctx, JSValueConst watcher);

/* ---- §6.10.2's THREE ACTION-RUNNING ALGORITHMS, as ONE request a calling machine drives -------------------
 *
 * THEY ARE A SUB-SEQUENCE AND NOT MEMBERS, exactly as core/html/form_entry_list.h's §4.10.22.4 is: each has
 * more than one caller in more than one section (`requestClose()` and process close watchers both request to
 * close; `close()` and request to close's step 11 both close), and every caller is a step machine already. So
 * the CALLER holds the state, names it in its own `visit`, and releases it with its own teardown.
 *
 * ONE RECORD FOR THREE ALGORITHMS, BECAUSE THE NESTING IS FIXED AND ACYCLIC — process close watchers step 2.2.2
 * requests to close, and request to close step 11 closes, and nothing goes back the other way. Each algorithm
 * therefore owns ONE cursor of its own and they cannot collide; what they SHARE is whichever action is in
 * flight, because at most one of the three is ever mid-dispatch. A record per level would be three types whose
 * only difference is which cursor is live.
 *
 * WHY THE ACTIONS NEED A CURSOR AT ALL. The cancel action §6.10.3 SUPPLIES fires a `cancel` event and the
 * close action it supplies fires `close`, so both are the page's own handlers — a loop, an `await`, a DOM
 * mutation — and the close action §6.12 supplies is hide a popover, which is a step machine that parks. And §6.4.1's history-action activation, which
 * request to close step 6 asks, is unknown external state, so it is answered by a FORK and not by a `bool`. */
typedef struct {
    uint8_t  rphase;    /* request to close's own cursor — close_watcher.c's CWR_* */
    uint8_t  cphase;    /* close's own cursor — CWC_* */
    uint8_t  pphase;    /* process close watchers' own cursor — CWP_* */
    uint8_t  fphase;    /* the fire request of whichever ACTION is in flight */
    uint8_t  ua_phase;  /* §6.4.1's history-action activation request's own phase */
    uint8_t  can_prevent;   /* request to close step 6's canPreventClose, held across steps 7-10 */
    uint8_t  processed;     /* process close watchers step 1's processedACloseWatcher, which step 2.2.1 raises */
    uint32_t i;         /* process close watchers step 2.2's reverse cursor: one PAST the member being run */
    JSValue  group;     /* process close watchers step 2.1's group, COPIED — see close_watcher.c (owned) */
    JSValue  cur;       /* the watcher step 2.2 is standing on (owned) */
    JSValue  ev;        /* the `cancel` or `close` event, held across its dispatch (owned) */
    /* THE WATCHER WHOSE "is running cancel action" THIS RUN SET AND STILL OWES A CLEAR (owned), or undefined.
       It is a FLAG and not a reference, which is why it is here rather than left to the declaration: a flow
       abandoned inside the page's `cancel` handler must not leave that watcher refusing every later request at
       step 3 for the rest of the session. Same shape and same reason as form_entry_list.h's `flag_set`. */
    JSValue  running;
    EventFireCb cb;     /* the fire request's buffer: [this, dispatch, target, event, targetOverride] */
} CloseWatcherRun;

/* Place the run's owned fields before anything can fail — a step state arrives js_mallocz'd, which is safe to
   RELEASE but is not JS_UNDEFINED, so a machine calls this at the stage that starts the algorithm. */
void close_watcher_run_init(CloseWatcherRun *r);
/* WHAT THIS RUN OWNS — the calling machine's `visit` forwards to it, so a fork mid-dispatch gives each arm its
   own event rather than two flows one. */
void close_watcher_run_visit(JSContext *ctx, CloseWatcherRun *r, JSStepVisit *v);
/* GIVE BACK step 7's flag that step 9 would otherwise have cleared. THIS is what a holding machine's `release`
   calls: the flag is not a reference, so no declaration can name it, and it READS the run's own `running`, so
   it must run before that declaration is discharged. Idempotent, and safe on a run that never took one. */
void close_watcher_run_unlock(JSContext *ctx, CloseWatcherRun *r);

/* §6.10.2's "To REQUEST TO CLOSE A CLOSE WATCHER closeWatcher with boolean requireHistoryActionActivation",
   12 steps. `*pproceed` takes the answer — false only through step 10, where the cancel action prevented the
   close. `require_history_action_activation` is passed on EVERY re-entry rather than stored, which is the same
   contract form_entry_list_run's `form` has: it is the caller's argument and not this run's state.
   Returns JS_STEP_CALL / JS_STEP_FORK (the caller returns it), -1 with a throw live, or 0 when it has answered. */
int close_watcher_request_to_close_run(JSContext *wctx, JSStepHdr *hdr, CloseWatcherRun *r,
                                       JSValueConst watcher, bool require_history_action_activation,
                                       JSValue in, bool *pproceed, JSValue **out_cb, int *out_argc);

/* §6.10.2's "To CLOSE A CLOSE WATCHER closeWatcher", 5 steps. It returns nothing: steps 1-3 are silent
   refusals the standard writes as a bare "return", which is why there is no out-parameter to distinguish them
   from a close that ran. Same return contract as above, minus JS_STEP_FORK — it takes no JSStepHdr because
   nothing in its 5 steps asks a question over unknown state; the day the close action §6.12 supplies is
   routed into it,
   that arm's own §6.6.6 fork is what adds one. */
int close_watcher_close_run(JSContext *wctx, CloseWatcherRun *r, JSValueConst watcher,
                            JSValue in, JSValue **out_cb, int *out_argc);

/* §6.10.2's "To PROCESS CLOSE WATCHERS given a Window window", 4 steps — `wctx` is that Window's realm.
   `*pprocessed` takes step 4's processedACloseWatcher, which §6.10.1's close request steps step 7 reads as
   `closedSomething` and nothing else does. Same return contract as above.
   IT HAS NO CALLER IN THIS BUILD and close_watcher.h's residual paragraph says so: §6.10.1's 9 steps are the
   one algorithm of §6.10 this component does not hold, because their step 1 asks the fullscreen model. */
int close_watcher_process_run(JSContext *wctx, JSStepHdr *hdr, CloseWatcherRun *r,
                              JSValue in, bool *pprocessed, JSValue **out_cb, int *out_argc);

#endif
