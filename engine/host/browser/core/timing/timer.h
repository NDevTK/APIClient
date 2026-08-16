/* TIMERS — HTML 8.6, the timer task source. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_TIMER_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_TIMER_H
#include "quickjs.h"

/* Installs setTimeout/clearTimeout/setInterval/clearInterval and queueMicrotask on the global. */
/* THE AGENT'S HALF: §8.6's four members, DECLARED once, and the per-realm MAP OF ACTIVE TIMERS this registers
   as a realm intrinsic. A declaration builds one pool entry per member; the per-realm install puts that same
   member on each realm's global and gives that global its own map. */
void timer_init(JSContext *ctx);

void timer_install(JSContext *ctx, JSValueConst global);
/* Agent teardown: the interned keys and the declaration. Each global's MAP goes with its realm, and each
   flow's entries go with the COW delta that holds them — there is no agent-wide list left to drop. */
void timer_free(JSContext *ctx);

/* HTML §7.5.9 step 18's "clear window's map of active timers" — the unloading document's global, and only
   that one. It is a per-global map (§8.6), so clearing one document's takes nothing from a same-origin popup's.
   §8.6's timer identifier is deliberately NOT reset: the global is being destroyed, so nothing will ask it. */
void timer_clear_map(JSContext *ctx);

/* THE EARLIEST EXPIRY ON THIS EVENT LOOP, or -1 when no timer is set — across every fully active document of
   the agent, because §8.6 gives every global its own map and §8.1.7 runs them all on ONE task source.
   It answers for the RUNNING FLOW: the maps are per-flow COW state, so what is in the heap when this is asked
   is exactly the flow's own timers. The clock the answer is a moment on lives in core/timing/event_loop.h —
   §8.1.7.3's rendering task source becomes due at moments on that same clock, so the two are compared rather
   than raced, and the comparison needs both halves askable. */
double timer_next_due(JSContext *ctx);

/* HTML 8.6: a STRING handler is EVALUATED when the timer fires. Running it is the HOST's — the extension's host
   queues it onto the flow that scheduled it, which is what keeps a `setTimeout("...")` payload explorable — and
   naming that register here would make the browser half depend on the scheduler, and through it on the whole
   solver, exactly as fetch.h says of its own provider. A host that registers none has not built the capability,
   and a page that uses one crashes naming it rather than silently dropping the handler.
   `doc` NAMES WHICH DOCUMENT'S PROGRAM IT IS — the realm the string is compiled in, which §8.6 says is the
   entry global object's: a `setTimeout("x = 1")` scheduled by an iframe's script defines `x` on THAT
   document's Window. The queue is the scheduler's and it keys the realm off the document (solver/flow.h), so
   the fact travels with the source instead of being re-derived from whichever realm the scheduler is rooted
   in — which is the parent's for every child navigable in the agent. */
void timer_set_script_sink(void (*queue)(uint32_t doc, const char *src));

/* FIRE THE EARLIEST DUE TIMER — the event loop's step, asked by whoever DRIVES the loop and only when it has
   nothing else to run. Nothing is queued when a timer is set: §8.1.7 runs a task from a source that has one
   DUE, and the timer source's task is due at its expiry. Asking here, at the one moment the driver knows the
   queues are empty and the host owes nothing, is what keeps virtual time from stepping over work that is
   already due — a long timeout must not land in the middle of the work it was set to outlast.
   IT FIRES THE RUNNING FLOW'S TIMER, because it is asked from inside that flow's step and the maps it walks
   are that flow's COW state. A flow whose timer is due while another is being stepped fires it when the WFQ
   next schedules it — no timer wheel beside the frontier, and no cross-flow wakeup.
   Returns 1 when a timer fired, so the driver knows it has work again; 0 when there is none. */
int timer_run_due(JSContext *ctx);

/* HTML §8.6's RUN STEPS AFTER A TIMEOUT, for an ENGINE algorithm rather than a page callback — what
   `AbortSignal.timeout()` step 3 performs, and what every other specification that says "run steps after a
   timeout" needs. `steps` is a CALLABLE the caller minted (a step-machine function object is the usual one),
   because §8.6 performs the steps at the expiry and an engine algorithm that runs page code — signalling
   abort runs the page's abort algorithms and fires an event — has to be a flow when it does.
   It is the SAME timer source the page's timers are on, so the engine's own scheduled work is ordered against
   them by the one clock, and it goes in the map of the global `ctx` names. Answers §8.6's timerKey, which
   timer_cancel takes — in that same global. */
int  timer_after(JSContext *ctx, double ms, JSValueConst steps);
void timer_cancel(JSContext *ctx, int key);

#endif
