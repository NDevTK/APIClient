/* USER ACTIVATION — HTML §6.4 "Tracking user activation". See user_activation.c.
 *
 * WHAT STOOD HERE BEFORE WAS A CONSTANT WITH AN EXCUSE ATTACHED. §7.4.2.4's unload-prompt conjunction asks
 * whether "document's relevant global object has sticky activation", and document_lifecycle.c answered
 * `return false;` under a comment that said this user agent has no input device. Half of that comment is
 * SPEC-TRUE and survives into this component: §6.4.2 activates only on an event whose `isTrusted` is true, and
 * §3.2.2's `element.click()` mints an UNTRUSTED event, so a page cannot activate itself. The other half was the
 * defect — "there is no source of activation" is a fact about this engine's INPUTS, and it was written down as
 * a fact about the STATE. Those differ everywhere it matters: the state has two timestamps with two different
 * lifetimes, an ordering between them, a consumption that makes one false while the other stays true, and a
 * per-Window identity that a notification propagates across a navigable subtree. A constant models none of it,
 * and every algorithm that reads it — the unload prompt, §7.3.1.7's popup rules, §7.4.2.1's source snapshot
 * params, §6.10's close watchers — would each have grown its own copy of the same excuse.
 *
 * SO THE STATE IS REAL AND IT STARTS INACTIVE. Every Window this agent builds carries §6.4.1's two timestamps
 * at their spec-initial values, the three boolean states are COMPUTED from them at the moment they are asked,
 * and §6.4.2's notification and both consumptions are here for the moment an activation exists to report. What
 * this engine does not yet have is the SOURCE: it dispatches no trusted `keydown`/`mousedown`/`pointerdown`/
 * `pointerup`/`touchend`, so nothing calls user_activation_notify and every read answers false — the same
 * answer the constant gave, computed rather than asserted, and it changes the moment the first trusted input
 * event exists.
 *
 * PER WINDOW MEANS PER REALM, AND THE RECORD TIME-TRAVELS. The timestamps are a Window's, so they live in this
 * realm's own baseline record (realm.h's per-realm value), the shape §6.6's visibility state and §8.9's
 * animation-frame map already use: the record is unreachable from the page so nothing but this component can
 * write it, and each field is an ordinary property write, so the heap COW captures it and one forked arm can
 * consume an activation without touching its sibling's.
 *
 * AND THE INITIAL LAST ACTIVATION TIMESTAMP IS CONCOLIC, WHICH IS THE OTHER HALF OF THE SAME SENTENCE. The
 * paragraph above ended "every read answers false ... and it changes the moment the first trusted input event
 * exists", and that was a CONSTANT with an excuse attached one level down: an engine that dispatches no trusted
 * input event has not OBSERVED "the user did not interact", it has observed NOTHING. Whether a user has
 * interacted with the page is unknown EXTERNAL state — the most general concolic value, with the example a
 * never-interacted page gives (positive infinity) and no constraint on it — and BOTH answers reach code worth
 * running: the picker/consume path of §4.10.5.4's showPicker and its NotAllowedError fallback, which in a real
 * bundle is routinely a different endpoint. A concrete `false` here deletes the fork and the whole activated
 * world with it, which is the defect CLAUDE.md names for a loaded `features.admin:false`.
 *   So the timestamp is a source (core/dom/abort.c's AbortSignal.timeout().aborted is the same distinction:
 * the engine's OWN controller knows, the outside world does not), and the three boolean states below are asked
 * THROUGH THE FORK SEAM — a C `if` on a value the solver has not decided silently picks one arm, which is
 * exactly what the solver exists to prevent. What is NOT concolic is anything this engine performed itself:
 * §6.4.2's notification writes a real timestamp and a consumption writes negative infinity, and both are then
 * ordinary arithmetic with nothing left to fork over.
 *   A HOST WITH NO SOURCE OVERLAY (a conformance run) gets the plain positive infinity — concolic_source_wrap
 * is the seam that decides, so test262 keeps the spec's own answers and forks nothing. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_USER_ACTIVATION_H
#define ENGINE_HOST_BROWSER_CORE_HTML_USER_ACTIVATION_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* Declared once per AGENT, from html_form_declare — the per-realm half declares ITSELF into realm.h's one
   list, so every realm this agent builds gets §6.4.1's record at its spec-initial values. */
void user_activation_init(JSContext *ctx);
void user_activation_free(void);

/* §6.4.1's THREE BOOLEAN STATES, each asked of the Window whose realm is `ctx` and each COMPUTED from the two
   timestamps at the moment of the question — never stored, because "has the transient state expired" is a
   comparison against the current high resolution time and a stored boolean would answer with the time it was
   written at.
     STICKY      — the user has interacted with this Window at least once since it was created. Once true it
                   never goes back to false, INCLUDING across a consumption (§6.4.2's consume sets the
                   timestamp to negative infinity, which is still "not in the future").
     TRANSIENT   — the user has interacted RECENTLY: within the transient activation duration, and not since
                   consumed.
     HISTORY-ACTION — an interaction has happened that no history-action-consuming API has spent yet. It has no
                   time limit, which is exactly what makes it a separate question from TRANSIENT.

   EACH IS A REQUEST, NOT A `bool` FUNCTION, because the timestamp it compares against may be UNKNOWN (see the
   file header) and a question over an unknown FORKS: this flow takes one answer and a sibling flow is
   snapshotted holding the other. That is step_fork_run's contract, so the caller is a STEP MACHINE and these
   have its shape — return JS_STEP_FORK (the caller returns it and is re-entered here) or 0 with *out set.
   `phase` is a byte the CALLING machine owns on its own state, exactly as event_target_fire_run's is: TRANSIENT
   asks two questions in sequence (sticky, then "and recently"), so which of the two a parked flow is at cannot
   live in a C local. It starts at zero and is left at zero once an answer is delivered, so one byte serves any
   number of successive questions.
     THE TWO QUESTIONS ARE CHAINED RATHER THAN INDEPENDENT, and that is what keeps the worlds consistent:
   transient activation IMPLIES sticky activation (both are "now is at or past the last activation timestamp",
   and transient adds an upper bound), so asking "recently?" only inside the arm that already said "ever?"
   produces three worlds — never interacted, interacted long ago, interacted just now — and no fourth one that
   contradicts itself.
     A STAGE PER QUESTION IS THE CALLER'S OBLIGATION. `h`'s own fork phase remembers that ONE request is
   outstanding, so two of these in a single stage would re-ask the first every time the machine is re-entered
   and never converge; the phase byte separates the two questions INSIDE one of these, and the caller's stage
   separates successive ones. */
int user_activation_sticky_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, bool *out);
int user_activation_transient_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, bool *out);
int user_activation_history_action_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, bool *out);

/* §6.4.2's ACTIVATION NOTIFICATION STEPS, given the Document whose realm is `ctx` — what the user agent must
   perform BEFORE dispatching an activation triggering input event, which §6.4.2 defines as any event whose
   `isTrusted` is true and whose type is "keydown" (with the key neither Esc nor a reserved shortcut),
   "mousedown", "pointerdown" with pointerType "mouse", "pointerup" with pointerType not "mouse", or "touchend".
   It activates this Window, every ANCESTOR navigable's active window, and every SAME-ORIGIN descendant
   navigable's — the asymmetry with consumption below is the standard's and is deliberate. */
void user_activation_notify(JSContext *ctx);

/* §6.4.2's TWO CONSUMPTIONS, given the Window whose realm is `ctx`. Both walk the INCLUSIVE DESCENDANT
   NAVIGABLES of the top-level traversable's active document — every browsing context in the page, cross-origin
   ones included, which is what stops a deep iframe hierarchy from spending one interaction many times.
     consume            — what a TRANSIENT ACTIVATION-CONSUMING API performs (§7.3.1.7's popup rules when they
                          create a top-level traversable, §4.10.5.4's showPicker): the transient state becomes
                          false everywhere while the sticky state stays true.
     consume_history_action — what a HISTORY-ACTION ACTIVATION-CONSUMING API performs (§7.2.6.10.4's traverse
                          navigate event): the history-action state becomes false, and only another interaction
                          brings it back.

   ONLY THE FIRST IS A REQUEST, and the asymmetry is the two algorithms' own. §6.4.2's consume asks a question
   per Window — "if window's last activation timestamp is not positive infinity" — and over an unknown
   timestamp that question IS §6.4.1's sticky activation (a timestamp is never in the future, so "not positive
   infinity" and "now is at or past it" are the same test), so it is asked once through the same seam and every
   Window in the walk is written under the one answer. The history-action consumption asks NOTHING: it COPIES
   the last activation timestamp onto the last history-action activation timestamp, which is correct in every
   world without deciding which world this is. */
int  user_activation_consume_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase);
void user_activation_consume_history_action(JSContext *ctx);

/* §6.4.4's `UserActivation` OBJECT for this realm's Window — what §6.4.4's
   `partial interface Navigator { [SameObject] readonly attribute UserActivation userActivation; }` answers
   with, and the only thing navigator.c needs from this component. OWNED: the caller frees.

   "Each Window has an associated UserActivation ... Upon creation of the Window object, its associated
   UserActivation must be set to a new UserActivation object created in the Window object's relevant realm." So
   the object is minted with the realm beside the record above, and `[SameObject]` is a property of WHERE IT IS
   KEPT rather than of a cache the getter keeps: there is one object per realm to answer with, so
   `navigator.userActivation === navigator.userActivation` holds by construction and a flow cannot mint a
   baseline object by being the first to look. */
JSValue user_activation_object(JSContext *ctx);

#endif
