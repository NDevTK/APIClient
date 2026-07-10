/* Timers — setTimeout / setInterval / requestAnimationFrame / requestIdleCallback / queueMicrotask. A deferred
 * callback is not a real wait — it is a first-class BFS FLOW (flow_defer_callback), so a bundle that defers
 * init in a timer is still explored, ordered + starved by the one WFQ. See timers.c. */
#ifndef ENGINE_HOST_BROWSER_TIMERS_H
#define ENGINE_HOST_BROWSER_TIMERS_H
#include "quickjs.h"
JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
#endif
