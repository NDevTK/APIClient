/* TIMERS — HTML 8.6, the timer task source. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_TIMER_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_TIMER_H
#include "quickjs.h"

/* Installs setTimeout/clearTimeout/setInterval/clearInterval and queueMicrotask on the global. */
/* THE AGENT'S HALF: §8.6's four members, DECLARED once. A declaration builds one pool entry per member; the
   per-realm install puts that same member on each realm's global. */
void timer_init(JSContext *ctx);

void timer_install(JSContext *ctx, JSValueConst global);
/* Drop every scheduled timer (document teardown). */
void timer_reset(JSContext *ctx);

/* The VIRTUAL clock, in ms since the document started — the same clock the timer task source orders by. HTML
   has no wall clock to offer a headless run, and a second time source would order events differently from the
   queue that ran them. */
double timer_now(void);

/* THE CLOCK IS THE EVENT LOOP'S, NOT THE TIMER SOURCE'S — and it stopped being only the timer source's the
   moment a SECOND source became due at moments on it. §8.1.7.3's in-parallel half queues a rendering task at
   the next rendering opportunity, so "what happens next" is the earlier of that moment and the earliest timer
   expiry, and both halves of that comparison have to be askable from one place. They live here because this is
   where the clock is: a second clock would order events differently from the queue that ran them, which is the
   one thing the virtual clock exists to keep consistent.

   `timer_next_due` answers the earliest expiry, or -1 when no timer is set. `timer_advance_to` moves the clock
   to a moment ANOTHER task source becomes due — never past a timer that expires first, which is asserted here
   rather than trusted to the caller. */
double timer_next_due(void);
void   timer_advance_to(double when);

/* HTML 8.6: a STRING handler is EVALUATED when the timer fires. Running it is the HOST's — the extension's host
   queues it onto the flow that scheduled it, which is what keeps a `setTimeout("...")` payload explorable — and
   naming that register here would make the browser half depend on the scheduler, and through it on the whole
   solver, exactly as fetch.h says of its own provider. A host that registers none has not built the capability,
   and a page that uses one crashes naming it rather than silently dropping the handler. */
void timer_set_script_sink(void (*queue)(const char *src));

/* FIRE THE EARLIEST DUE TIMER — the event loop's step, asked by whoever DRIVES the loop and only when it has
   nothing else to run. Nothing is queued when a timer is set: §8.1.7 runs a task from a source that has one
   DUE, and the timer source's task is due at its expiry. Asking here, at the one moment the driver knows the
   queues are empty and the host owes nothing, is what keeps virtual time from stepping over work that is
   already due — a long timeout must not land in the middle of the work it was set to outlast.
   Returns 1 when a timer fired, so the driver knows it has work again; 0 when there is none. */
int timer_run_due(JSContext *ctx);

/* HTML §8.6's RUN STEPS AFTER A TIMEOUT, for an ENGINE algorithm rather than a page callback — what
   `AbortSignal.timeout()` step 3 performs, and what every other specification that says "run steps after a
   timeout" needs. `steps` is a CALLABLE the caller minted (a step-machine function object is the usual one),
   because §8.6 performs the steps at the expiry and an engine algorithm that runs page code — signalling
   abort runs the page's abort algorithms and fires an event — has to be a flow when it does.
   It is the SAME timer source the page's timers are on, so the engine's own scheduled work is ordered against
   them by the one clock. Answers §8.6's timerKey, which timer_cancel takes. */
int  timer_after(JSContext *ctx, double ms, JSValueConst steps);
void timer_cancel(JSContext *ctx, int key);

#endif
