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
 * consume an activation without touching its sibling's. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_USER_ACTIVATION_H
#define ENGINE_HOST_BROWSER_CORE_HTML_USER_ACTIVATION_H

#include <stdbool.h>

#include "quickjs.h"

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
                   time limit, which is exactly what makes it a separate question from TRANSIENT. */
bool user_activation_sticky(JSContext *ctx);
bool user_activation_transient(JSContext *ctx);
bool user_activation_history_action(JSContext *ctx);

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
                          brings it back. */
void user_activation_consume(JSContext *ctx);
void user_activation_consume_history_action(JSContext *ctx);

#endif
