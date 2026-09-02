/* TIMERS — HTML §8.7 "Timers", the timer task source. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_TIMER_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_TIMER_H
#include "quickjs.h"

/* Installs setTimeout/clearTimeout/setInterval/clearInterval and queueMicrotask on the global. */
/* THE AGENT'S HALF: §8.7 Timers's four members, DECLARED once, and the per-realm MAP OF ACTIVE TIMERS this registers
   as a realm intrinsic. A declaration builds one pool entry per member; the per-realm install puts that same
   member on each realm's global and gives that global its own map. */
void timer_init(JSContext *ctx);

void timer_install(JSContext *ctx, JSValueConst global);
/* Agent teardown — core/platform.h's release column: the interned keys, the realm slot, the host edge and
   §8.1.7's timer step this component claimed on the ONE frontier. Each global's MAP goes with its realm, and
   each flow's entries go with the COW delta that holds them — there is no agent-wide list left to drop, which
   is why it takes the RUNTIME. */
void timer_free(JSRuntime *rt);

/* HTML §7.5.9 step 18's "clear window's map of active timers" — the unloading document's global, and only
   that one. It is a per-global map (§8.7 Timers), so clearing one document's takes nothing from a same-origin popup's.
   §8.7 Timers's timer identifier is deliberately NOT reset: the global is being destroyed, so nothing will ask it. */
void timer_clear_map(JSContext *ctx);

/* IS THE TIMER SOURCE'S NEXT TASK DUE BEFORE `moment`? — across every fully active document of the agent,
   because §8.7 Timers gives every global its own map and §8.1.7 runs them all on ONE task source. 0 when no
   timer is set at all, which is the same answer as "not before" and is right for both.
   It answers for the RUNNING FLOW: the maps are per-flow COW state, so what is in the heap when this is asked
   is exactly the flow's own timers. The moments live on core/timing/event_loop.h's clock, which is also where
   §8.1.7.3's rendering task source becomes due, so the two are compared rather than raced.
   IT IS THE COMPARISON AND NOT THE MOMENT, because an expiry is not always a number. §8.7's `timeout` can be
   unknown external input (a page's `setTimeout(f, someUnknown)`), and an expiry derived from one has no double
   to hand out — while the ORDER it takes is a question with two real answers, so asked here it FORKS and both
   orders run. Handing out a moment forced this component to pick one of them inside an accessor.
   AND `moment` IS A MOMENT AND NOT A `double` FOR THE SAME REASON READ FROM THE OTHER END: the frame the
   rendering loop is about to take is `last render opportunity time + the frame interval`, and once the clock
   itself can be unknown so is that sum. The comparison is core/timing/event_loop.h's one moment order. */
int timer_due_before(JSContext *ctx, JSValueConst moment);

/* THERE IS NO STRING-HANDLER HOST EDGE, AND ITS ABSENCE IS THE STATEMENT. HTML §8.7 "Timers" evaluates a
   DOMString handler at the EXPIRY, inside step 9's task — substep 9.8.7 creates a classic script from it and
   9.8.8 runs it — so the program is created and run by the task machine in timer.c, on the firing flow's own
   trampoline chain, and never queued elsewhere at the set. What used to stand here was a registration
   (`timer_set_script_sink`) each host had to make and one of them did not, so `setTimeout("…")` aborted the
   whole document under the WPT gate while the two hosts that registered it ran the same code correctly. The
   edge is not a parameter of this component any more; there is nothing for a host to forget.
   WHICH DOCUMENT'S PROGRAM IT IS was that edge's one real argument and it is answered where it belongs: §8.7
   states the compile against GLOBAL's relevant settings object (step 9's task substep 4, then 9.8.7), `global`
   is the timer initialization steps' first argument, which the members give as `this`, and the task is minted
   in the realm whose map held the entry — which js_timer_task_step asserts against step 1's `thisArg`. So
   `frames[0].setTimeout("x = 1")` defines `x` on the CHILD's Window, and so does
   `setTimeout.call(frames[0], "x = 1")`. */

/* FIRE THE EARLIEST DUE TIMER — the event loop's step, asked by whoever DRIVES the loop and only when it has
   nothing else to run. Nothing is queued when a timer is set: §8.1.7 runs a task from a source that has one
   DUE, and the timer source's task is due at its expiry. Asking here, at the one moment the driver knows the
   queues are empty, is what keeps virtual time from stepping over work that is already due — a long timeout
   must not land in the middle of the work it was set to outlast.
   "THE QUEUES ARE EMPTY" IS THE WHOLE CONDITION, AND "THE HOST OWES NOTHING" USED TO BE HALF OF IT. A reply
   still in flight is on no task queue — §8.1.7.3 "Processing model" step 2 asks for "a task queue with at
   least one runnable task" and a `fetch()` runs in parallel until it completes — so it is not work this
   source could step over, and waiting for it starved every due timer of a flow with one request outstanding.
   IT FIRES THE RUNNING FLOW'S TIMER, because it is asked from inside that flow's step and the maps it walks
   are that flow's COW state. A flow whose timer is due while another is being stepped fires it when the WFQ
   next schedules it — no timer wheel beside the frontier, and no cross-flow wakeup.
   Returns 1 when a timer fired, so the driver knows it has work again; 0 when there is none. */
int timer_run_due(JSContext *ctx);

/* HTML §8.7 Timers's RUN STEPS AFTER A TIMEOUT, for an ENGINE algorithm rather than a page callback — what
   `AbortSignal.timeout()` step 3 performs, and what every other specification that says "run steps after a
   timeout" needs. `steps` is a CALLABLE the caller minted (a step-machine function object is the usual one),
   because §8.7 Timers performs the steps at the expiry and an engine algorithm that runs page code — signalling
   abort runs the page's abort algorithms and fires an event — has to be a flow when it does.
   It is the SAME timer source the page's timers are on, so the engine's own scheduled work is ordered against
   them by the one clock, and it goes in the map of the global `ctx` names. Answers §8.7 Timers's timerKey, which
   timer_cancel takes — in that same global. */
int  timer_after(JSContext *ctx, double ms, JSValueConst steps);
void timer_cancel(JSContext *ctx, int key);

#endif
