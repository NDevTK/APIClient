/* IDLE CALLBACKS — Cooperative Scheduling of Background Tasks §4 Window interface extensions and §5 Processing.
 *
 * THE STANDARD IS ITS OWN DOCUMENT AND NOT A PART OF HTML — "Cooperative Scheduling of Background Tasks",
 * whose Editor's Draft the W3C Web Performance group maintains at https://w3c.github.io/requestidlecallback/.
 * NOTHING IN THIS TREE INDEXES IT: engine/specindex/ has no row for it and engine/citegen.mjs's registry names
 * no anchor that resolves to it, so every §-number in this file and in idle_deadline.c is COUNTED by that
 * auditor and CHECKED by nothing. That is a silent zero rather than a clean bill, and it is stated here rather
 * than left to be discovered: the fix is one registry row (key "requestidlecallback", kind respec, base
 * https://w3c.github.io/requestidlecallback/, anchors "requestidlecallback" / "cooperative scheduling of
 * background tasks") plus a `--regen` of that key, and until it lands a reader who wants a number verified
 * fetches the document.
 *
 * §4 GIVES EVERY Window THREE ASSOCIATED CONCEPTS and this component holds all three, per REALM, in one
 * object built with the realm:
 *   - "A list of idle request callbacks. The list MUST be initially empty and each entry in this list is
 *     identified by a number, which MUST be unique within the list for the lifetime of the Window object."
 *   - "A list of runnable idle callbacks", with the same sentence about its entries.
 *   - "An idle callback identifier, which is a number which MUST initially be zero."
 *
 * THE LISTS ARE HEAP OBJECTS AND THAT IS LOAD-BEARING, for core/rendering/animation_frame.h's reason: a
 * registered callback is per-FLOW state. One arm of a fork may request an idle callback its sibling never
 * requested, and a parked flow must resume owed exactly the callbacks it was owed. A C-side list would be one
 * list answering for every flow and the COW delta would have nothing to capture; an object built at realm
 * install sits in the pre-boot baseline and every registration is an ordinary property write the delta already
 * captures.
 *
 * §5 IS NOT A SECOND SCHEDULER. §5.1's own note says the algorithm is "called by the event loop processing
 * model when it determines that the event loop is otherwise idle", so it is a RUNG of the one loop and not a
 * loop of its own — idle_callback_run is that rung, registered through solver/engine.h's hook seam beside the
 * timer and rendering steps and asked only where the running flow has nothing else to do. The callback it
 * reaches is handed to JS_EnqueueCallTask, which is the same door every timer expiry goes through, so an idle
 * callback is a first-class flow in the one WFQ: preemptible per opcode, forkable, and parkable to the cold
 * tier at any depth. There is no idle queue, no drain and no budget. */
#ifndef ENGINE_HOST_BROWSER_CORE_SCHEDULING_IDLE_CALLBACK_H
#define ENGINE_HOST_BROWSER_CORE_SCHEDULING_IDLE_CALLBACK_H

#include "quickjs.h"

/* THE AGENT'S HALF — §4's two members declared once, the per-realm slot the three concepts live in, and the
   §5 rung registered on the event loop. It declares §4.3's interface too (idle_deadline_init): that interface
   has no producer but §5.2 step 3.2, so a second core/platform.c row for it would be a second thing to
   remember with nothing else to decide. */
void idle_callback_init(JSContext *ctx);

/* THE REALM'S HALF, and it is TWO calls for core/rendering/animation_frame.h's reason: the three concepts are a
   per-realm intrinsic every realm gets through core/realm.h's one declared list, while the MEMBERS go on the
   Window, which is the host's per-document install. */
void idle_callback_install_store(JSContext *ctx);
void idle_callback_install(JSContext *ctx, JSValueConst global);

void idle_callback_free(JSRuntime *rt);

/* §5.1 START AN IDLE PERIOD and §5.2 INVOKE IDLE CALLBACKS, as the event loop's idle rung — ONE step per call,
   over every fully active document of this agent. Answers 1 when it did something and 0 when no Window of this
   agent has an idle callback to start or to run, which is what lets the rung below it be reached. */
int idle_callback_run(JSContext *ctx);

#endif
