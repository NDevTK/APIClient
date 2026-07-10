/* Observers — IntersectionObserver / MutationObserver / ResizeObserver / PerformanceObserver (Blink
 * core/intersection_observer, core/dom (MutationObserver), core/resize_observer, core/timing). The observer
 * object is the OPAQUE concolic value (headless has no viewport/layout/compositor, so it never fires from
 * OBSERVATION and any property read is the honest unknown, not a fixed-shape stub); its callback IS page code
 * that runs on the observed event, so it is registered as a scheduler flow. See observer.c. */
#ifndef ENGINE_HOST_BROWSER_OBSERVER_H
#define ENGINE_HOST_BROWSER_OBSERVER_H
#include "quickjs.h"
JSValue js_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new IntersectionObserver(cb) / … */
#endif
