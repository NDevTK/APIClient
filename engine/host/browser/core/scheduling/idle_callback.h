/* IDLE CALLBACKS — Cooperative Scheduling of Background Tasks §4 Window interface extensions and §5 Processing.
 *
 * THE STANDARD IS ITS OWN DOCUMENT AND NOT A PART OF HTML — "Cooperative Scheduling of Background Tasks",
 * whose Editor's Draft the W3C Web Performance group maintains at https://w3c.github.io/requestidlecallback/.
 * THIS STANDARD HAS NO TEXT CORPUS AND CANNOT GET ONE, AND ITS CITATIONS ARE NAMED RATHER THAN SILENT.
 * engine/specindex/ holds no row for it, and engine/citegen.mjs carries a FOREIGN entry `background tasks`
 * which is what makes that visible: a citation naming this standard is gated OUT of judging and appears, with
 * its tally, on the auditor's `standards seen but not indexed` census line. Run
 * `node engine/citegen.mjs engine/host/browser/core/scheduling/idle_callback.c` for today's split; MEASURED at
 * 6cc8439a that file read 82 citations, 4 of them counted under `background tasks` and 8 resolved on their own
 * evidence with 2 step references compared. So the honest statement is a COUNTED zero over THIS STANDARD and
 * not a silence over these files, and a reader who wants one of its numbers verified fetches the document.
 *   THREE SENTENCES STOOD HERE AND ALL THREE WERE WRONG, IN THE ONE DIRECTION A COVERAGE NOTE CANNOT AFFORD.
 *   They are recorded rather than deleted because each is what a reader re-derives from the missing row, and
 *   because the only reader of an absence claim is somebody deciding whether to BUILD an instrument for the
 *   axis — so a stale one argues for a second auditor standing beside a working one and nothing mechanical
 *   ever reports that it rotted.
 *   (1) `engine/citegen.mjs's registry names no anchor that resolves to it`. It does: the FOREIGN entry above,
 *   which is the difference between a citation this tree cannot see and one a file vote hands to whichever
 *   INDEXED standard happens to own a section by that number. That second outcome is the accusing one, and it
 *   is what the entry exists to stop.
 *   (2) `every §-number in this file and in idle_deadline.c is COUNTED by that auditor and CHECKED by nothing`.
 *   The FILE is the one thing a coverage note may not name, because a file cites more than one standard: at
 *   6cc8439a idle_deadline.c had 4 citations resolved on their own evidence — its Web IDL ones — and
 *   idle_callback.c 8, with 2 step references compared. Saying the auditor checks nothing here tells a reader
 *   to discount findings a live channel is already able to make.
 *   (3) `the fix is one registry row (key "requestidlecallback", kind respec, base
 *   https://w3c.github.io/requestidlecallback/ ...) plus a --regen`. That row cannot work, and this is the
 *   half that would have been EXECUTED rather than merely believed. MEASURED by fetching both, with the
 *   `permissions` row as the positive control because citegen already carries it as a working `kind: respec`:
 *   the requestidlecallback base answers 28,765 bytes carrying a `respecConfig`, ZERO `secno` headings and no
 *   `dt-published`, because ReSpec numbers a document IN THE BROWSER at render time — the bytes hold no
 *   section numbers at all — while the permissions base answers 336,720 bytes with NO `respecConfig`, 100
 *   `secno` headings and a `dt-published`. THE PRESENCE OF `respecConfig` IS THE TELL THAT A PAGE IS THE
 *   SOURCE, and it reads exactly like evidence that the respec reader is the right one; it is the opposite.
 *   engine/citegen.mjs's own `background tasks` entry records the same measurement and why the remaining
 *   answers are no better: the /TR/ rendering is a path the edition assertion refuses for a maintained row,
 *   and numbering the source by document order would be this codebase restating ReSpec's own algorithm.
 *   RETIREMENT: this record goes when a coverage note in this tree cannot name a FILE as the thing an
 *   unindexed standard leaves unchecked, and cannot name a base URL as indexable without the fetched heading
 *   count beside it.
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
