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

#endif
