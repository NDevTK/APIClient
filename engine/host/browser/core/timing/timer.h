/* TIMERS — HTML 8.6, the timer task source. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_TIMER_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_TIMER_H
#include "quickjs.h"

/* Installs setTimeout/clearTimeout/setInterval/clearInterval and queueMicrotask on the global. */
void timer_install(JSContext *ctx, JSValueConst global);
/* Drop every scheduled timer (document teardown). */
void timer_reset(JSContext *ctx);

/* The VIRTUAL clock, in ms since the document started — the same clock the timer task source orders by. HTML
   has no wall clock to offer a headless run, and a second time source would order events differently from the
   queue that ran them. */
double timer_now(void);

/* HTML 8.6: a STRING handler is EVALUATED when the timer fires. Running it is the HOST's — the extension's host
   queues it onto the flow that scheduled it, which is what keeps a `setTimeout("...")` payload explorable — and
   naming that register here would make the browser half depend on the scheduler, and through it on the whole
   solver, exactly as fetch.h says of its own provider. A host that registers none has not built the capability,
   and a page that uses one crashes naming it rather than silently dropping the handler. */
void timer_set_script_sink(void (*queue)(const char *src));

#endif
