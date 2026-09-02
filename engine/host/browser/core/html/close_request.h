/* CLOSE REQUESTS — HTML §6.10.1 "Close requests"' CLOSE REQUEST STEPS. See close_request.c.
 *
 * WHY THIS IS A COMPONENT OF ITS OWN AND NOT A FUNCTION IN EITHER OF THE TWO IT CALLS. §6.10.1's nine steps
 * are the JOIN between two components that each name the join from their own side and neither of which may
 * hold it. Step 1 is "If document's fullscreen element is not null:", which is the Fullscreen API Standard §2
 * "Model"'s concept and core/fullscreen/fullscreen.h's `fullscreen_element`. Step 7 is "Let closedSomething be
 * the result of processing close watchers on document's relevant global object.", which is HTML §6.10.2 "Close
 * watcher infrastructure"'s algorithm and core/html/close_watcher.h's `close_watcher_process_run`. Put the
 * nine steps in either callee and the other's question becomes a private copy living in a file that does not
 * own it — which is the thing both of those headers already say in as many words from opposite directions.
 *
 * THE TWO CALLEES DO NOT SHARE A STACK, AND THAT IS THE WHOLE REASON FULLSCREEN REGISTERS NO CLOSE WATCHER.
 * Step 1's two sub-steps are "Fully exit fullscreen given document's node navigable's top-level traversable's
 * active document." and "Return." — a bare RETURN, so a document with a fullscreen element short-circuits the
 * algorithm and never reaches step 7 at all. Fullscreen is not a watcher competing inside §6.10.2's group
 * algebra; it is a HIGHER-PRIORITY ARM of these nine steps, checked before the manager is asked anything. A
 * design that gave fullscreen a close watcher would put it in a group, where the allowance arithmetic and the
 * join-the-last-group branch would decide it, and one close request would then be able to close a popover and
 * leave the document fullscreen.
 *
 * ── WHO CALLS THIS, AND WHY THE ANSWER IS A MODELLED GESTURE RATHER THAN A USER ──────────────────────────────
 *
 * §6.10.1's preamble is the producer: "Whenever the user agent receives a potential close request targeted at
 * a Document document, it must queue a global task on the user interaction task source given document's
 * relevant global object to perform the following close request steps:". A potential close request is a USER
 * GESTURE — §6.10.1's own four examples are the Esc key, an Android back button or gesture, an assistive
 * technology's dismiss gesture, and a game controller's back button — and this agent has no user, so none of
 * them arrives on its own.
 *   IT IS MODELLED INSTEAD, WHICH IS A DIFFERENT THING FROM BEING ABSENT AND FROM BEING FABRICATED. HTML
 * §6.10.1 "Close requests" spells out two conforming platforms and one of them needs nothing this agent
 * lacks, in HTML §6.10.1's own words: "On platforms where a back button is a potential close request, no
 * event is involved, so when the back button is pressed, the user agent proceeds directly to process close
 * watchers." So the whole producer for that platform is an ARRIVAL
 * plus the queue, which is `close_request_flow` below and one arm of the solver's scheduler. Firing it is a
 * FORCED EXPLORATION ACT — the same kind of act as driving a function the page never called — so everything a
 * flow computes past one is graded FORCED, and that grading is the solver's (see close_request_flow).
 *   AND IT IS NOT AN ACTIVATION. HTML §6.4.2 "Processing model" defines an activation triggering input event
 * as "any event whose isTrusted attribute is true and whose type is one of" five types, the first being
 * "keydown", provided the key is neither the Esc key nor a shortcut key reserved by the user agent. The Esc
 * press §6.10.1's OTHER platform fires is therefore exactly the event §6.4.2 excludes, and a back button fires
 * no event at all — so a modelled close request performs NO activation notification, and a build that made it
 * do so would report an interaction the standard says did not happen. §6.4.1's timestamps have their own live
 * producer and never needed this one: §6.4.2's notification steps are performed by algorithms that fire no
 * input event at all, and core/file/file_picker.c performs them for File System Access §3.3 "The
 * showOpenFilePicker() method".
 *
 * ── THE SHAPE: A REQUEST A CALLING MACHINE DRIVES, BECAUSE STEP 7 IS ONE ─────────────────────────────────────
 *
 * Step 7 runs §6.10.2's process close watchers, which runs each watcher's cancel and close ACTIONS — the
 * page's own `cancel` and `close` handlers — and asks §6.4.1's history-action activation, which
 * core/html/user_activation.h answers as a FORK because the timestamp behind it is unknown external state. So
 * step 7 parks, forks and yields, and these nine steps cannot be a C function that returns an answer: a C
 * activation hosting a page's `cancel` handler is the drive-to-completion this engine aborts on, and a second
 * driver beside the one BFS is the cardinal violation CLAUDE.md names.
 *   IT IS THEREFORE A SUB-SEQUENCE AND NOT A STEP MACHINE OF ITS OWN, the shape core/html/close_watcher.h's
 * three action-running algorithms and core/html/form_entry_list.h's §4.10.22.4 already have: the CALLER holds
 * the state, names it in its own `visit`, and discharges it with its own teardown. That is what makes the
 * whole of §6.10.1 available to whichever machine eventually queues the task, rather than fixing now which
 * one it is.
 *
 * ── WHAT EACH STEP COSTS THIS AGENT, IN THE STANDARD'S OWN WORDS ─────────────────────────────────────────────
 *
 * STEP 2 IS DECLINED, AND DECLINING IT IS AN ANSWER RATHER THAN AN OMISSION. "Optionally, skip to the step
 * labeled alternative processing." — its example is "if the user agent detects user frustration at repeated
 * close request interception by the current web page". This agent detects no user frustration, because it has
 * no user; taking the option would silently make every close request a no-op, which is the same wrong answer
 * a broken manager would give and would be indistinguishable from it.
 *
 * STEPS 3 AND 4 FIRE NOTHING, AND THAT IS A PLATFORM THE STANDARD DESCRIBES RATHER THAN A CAPABILITY THIS
 * ENGINE IS MISSING. Step 3 is "Fire any relevant events, per UI Events or other relevant specifications." and
 * step 4 is "Let event be null if no such events are fired, or the Event object representing one of the fired
 * events otherwise." Which events are relevant is a property of the GESTURE, and §6.10.1 spells out both of
 * its own platforms: "On platforms where pressing the Esc key is interpreted as a close request, the user
 * agent must interpret the key being pressed down as the close request, instead of the key being released.
 * Thus, in the above algorithm, the "relevant events" that are fired must be the single keydown event." and,
 * for the other, "On platforms where a back button is a potential close request, no event is involved, so when
 * the back button is pressed, the user agent proceeds directly to process close watchers." Firing no relevant
 * event is therefore a CONFORMING platform, written into §6.10.1 as one of its two worked cases — so steps 3
 * and 4 here are complete for a gesture that is not a key press, and NARROWER than §6.10.1 for one that is.
 * That is the difference between a residual and a stub, and it is why neither step crashes.
 *
 * STEP 5 IS LIVE CODE OVER A VALUE THAT IS NULL TODAY. "If event is not null, and its canceled flag is set,
 * then return." A page cancelling the keydown is how a close request is suppressed on a keyboard platform, and
 * the test is written over the run's own `event` rather than elided, because eliding it is what makes a step
 * disappear from the file that owns the algorithm.
 *
 * STEP 6 IS A REAL QUESTION WITH A REAL ANSWER TODAY. "If document is not fully active, then return." — the
 * term is HTML §7.3.3 Fully active documents' walk, which core/dom/document.h exports and which core/frame's
 * navigable teardown can already make false. §6.10.1's own note says why the step exists: "This step is
 * necessary because, if event is not null, then an event listener might have caused document to no longer be
 * fully active." That note is about the keydown listener, but the step is not: step 7's own `cancel` handlers run AFTER it, so the
 * check is asked once here exactly as the algorithm orders it and is not re-asked below.
 *
 * ── WHAT THIS ANSWERS ITS CALLER, WHICH IS STEP 9 AND NOT A BOOLEAN ABOUT CLOSING ────────────────────────────
 *
 * Step 9 is "Alternative processing: Otherwise, there was nothing watching for a close request. The user agent
 * may instead interpret this interaction as some other action, instead of interpreting it as a close request."
 * — so the ONE fact the caller needs back is whether the algorithm fell through to step 9, which is when the
 * gesture is still the caller's to reinterpret. §6.10.1's own example of doing so is the back button: "If
 * there is not, then the user agent can interpret the back button press in another way, for example as a
 * request to traverse the history by a delta of −1." Every other exit — step 1's fullscreen return, step 5's
 * cancelled event, step 6's detached document, step 8's `closedSomething` — is an exit the caller must NOT
 * reinterpret, and one boolean separates those two sets exactly. `closedSomething` itself is step 7's local
 * and stays here: a caller given it would have to re-derive step 8 to learn what it meant.
 *
 * ── NAMED RESIDUAL: THE KEYBOARD PLATFORM — A TRUSTED `keydown` ──────────────────────────────────────────────
 *   The producer this residual used to name is BUILT (close_request_flow below, queued by the solver), so what
 *   is left of it is the ONE of §6.10.1's two platforms that needs an event.
 *   — WHAT IS NOT COVERED: step 3's keyboard-platform arm — HTML §6.10.1 "Close requests" says of it that
 *     "the "relevant events" that are fired must be the single keydown event" — and therefore step 4's
 *     non-null `event` and step 5's cancelled-event return, which is unsatisfiable while step 4's `event` is
 *     JS_NULL on every path this build takes. This agent
 *     dispatches no trusted input event, which core/html/user_activation.h states from its own side. The
 *     back-button platform is complete and is not part of this: it fires no event, by §6.10.1's own text.
 *   — WHAT THE NEXT DIFF BUILDS: a trusted `keydown` this engine MINTS and DISPATCHES at a target — an
 *     `isTrusted` KeyboardEvent whose listeners run as page code and therefore park the flow — so that step 4
 *     holds it and step 5 can read its canceled flag. It is strictly larger than the arrival built here: an
 *     arrival is a work item, a dispatch is UI Events' whole event path plus a key, and the two share no code.
 *     It must NOT notify user activation: §6.4.2 "Processing model" excludes the Esc key from its activation
 *     triggering input events BY NAME, so one trusted-keydown MECHANISM serves both sections while a trusted
 *     Esc keydown discharges these nine steps and leaves every timestamp in §6.4.1 UNTOUCHED.
 *   — HOW ITS ABSENCE WOULD SHOW: a page that calls `preventDefault()` on a `keydown` to suppress a close
 *     request cannot suppress one here — its `cancel`/`close` handlers fire for a modelled back gesture that a
 *     real Esc press would never have reached them with, so the suppression a browser honours is missing and
 *     nothing in the run says which platform it was modelling. In the corpus, the subtree that measures it is
 *     WPT `close-watcher/esc-key/`, whose files are named for the three key events and for the two questions
 *     this residual is about — `keydown.html`, `keypress.html`, `keyup.html`, `not-user-activation.html` and
 *     `synthetic-keyboard-event.html` — and `close-watcher` is a top-level entry of engine/wpt.mjs's
 *     collection list, which is where that membership is stated and checked. WHETHER THOSE BYTES ARE PRESENT
 *     IS NOT A FACT ABOUT THIS CHECKOUT and the clause that stood here made it one: the corpus is materialized
 *     under a gitignored working directory, so a claim that its files are not on disk is true of a tree the
 *     gate has not fetched and false of one it has, and either way it dates the moment it is written (that
 *     claim is this header's own retired prose, paraphrased rather than quoted, because a quoted run beside a
 *     citation is read as the STANDARD's words). What is durable is
 *     which subtree asks the question; what it SCORES is a measurement, and a measurement belongs beside the
 *     revision it was taken at rather than in a header.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CLOSE_REQUEST_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CLOSE_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/html/close_watcher.h"   /* CloseWatcherRun — step 7's own request, held by this one */

/* §6.10.1's nine steps, as ONE request whose state the calling machine holds. It nests exactly one level:
   step 7 IS §6.10.2's process close watchers, so this record carries that algorithm's whole request rather
   than a second copy of its cursors — the composition core/html/close_watcher_interface.c makes at the IDL
   surface, made one section up. */
typedef struct {
    uint8_t         phase;   /* these nine steps' own cursor — close_request.c's CRQ_* */
    JSValue         ev;      /* step 4's `event`: JS_NULL when step 3 fired nothing, which is every path in
                                this build — see the header note and the named residual (owned) */
    CloseWatcherRun cw;      /* step 7's request, whose cursors, event and cancel-action flag it owns */
} CloseRequestRun;

/* Place the run's owned fields before anything can fail — a step state arrives js_mallocz'd, which is safe to
   RELEASE but is not JS_UNDEFINED, so a machine calls this at the stage that starts the algorithm. */
void close_request_run_init(CloseRequestRun *r);
/* WHAT THIS RUN OWNS — the calling machine's `visit` forwards to it, so a fork inside step 7 gives each arm
   its own event rather than two flows one. It forwards to step 7's own request for the same reason. */
void close_request_run_visit(JSContext *ctx, CloseRequestRun *r, JSStepVisit *v);
/* GIVE BACK whatever step 7 is still holding on an ABANDONED flow — §6.10.2's request to close step 7 sets a
   watcher's "is running cancel action" and its step 9 clears it, so a flow dropped inside the page's own
   `cancel` handler would otherwise leave that watcher refusing every later request for the rest of the
   session. THIS is what a holding machine's `release` calls; it reads owned state, so it must run before the
   declaration is discharged. Idempotent, and safe on a run that never reached step 7. */
void close_request_run_unlock(JSContext *ctx, CloseRequestRun *r);

/* HTML §6.10.1 "Close requests"' CLOSE REQUEST STEPS, 9 steps, for the Document `document` the potential close
   request was targeted at.
   TWO REALMS, AND THEY ARE NOT INTERCHANGEABLE. `ctx` is the CALLING MACHINE's, which is what owns `in` and
   what a throw is raised in; the realm the STEPS run in is the DOCUMENT'S, because §6.10.1 queues the task
   "given document's relevant global object", and it is derived from the node here rather than taken from the
   caller — the same split, for the same sentence, that close_watcher_notify_user_activation's `wctx` is.
   `*palternative` takes the ONE fact step 9 leaves the caller: true when the algorithm fell through to
   alternative processing and the gesture is still the caller's to reinterpret, false for every other exit.
   Returns JS_STEP_CALL / JS_STEP_FORK / JS_STEP_YIELD (the caller returns it), -1 with a throw live, or 0 when
   it has answered. */
int close_request_run(JSContext *ctx, JSStepHdr *hdr, CloseRequestRun *r, lxb_dom_node_t *document,
                      JSValue in, bool *palternative, JSValue **out_cb, int *out_argc);

/* §6.10.1's PREAMBLE — "it must queue a global task on the user interaction task source given document's
   relevant global object to perform the following close request steps". This is that queue: it returns the
   FLOW BASE of a call of the machine that runs the nine steps, for the caller to install as a member's work.
   It is a task and therefore a FLOW, which §scheduler requires of every enqueued job and which step 7 makes
   unavoidable — the page's own `cancel` and `close` actions run inside it, and a C activation hosting one is
   the drive-to-completion this engine aborts on.
   THE COMPLETION VALUE IS STEP 9's ONE FACT as a boolean: true when the algorithm fell through to alternative
   processing, which is the standard's own statement that nothing in this timeline was watching. See the
   comment above the machine in close_request.c for why it is a value and not a comment.
   `ctx` is the CALLER's realm and decides nothing: the task's realm is the DOCUMENT's, derived from the node,
   and the callee is minted there so `js_call_c_function` runs these steps in the Window §6.10.1 names.
   WHO DECIDES THAT A POTENTIAL CLOSE REQUEST ARRIVED IS NOT THIS FILE. There is no user here, so the arrival
   is a MODELLED gesture — a forced exploration act — and both halves of that belong to the solver: WHEN to
   model one, and the fact that everything a flow computes past one is graded FORCED. A browser component that
   decided either would be a network/exploration policy inside the browser half. */
JSValue *close_request_flow(JSContext *ctx, lxb_dom_node_t *document);

#endif
