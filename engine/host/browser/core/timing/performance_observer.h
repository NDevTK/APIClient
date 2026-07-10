#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_OBSERVER_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_OBSERVER_H
#include "quickjs.h"
/* PerformanceObserver — Blink core/timing. Its callback never fires headless, so the ctor registers it as a driven scheduler flow;
 * the instance SHAPE is generated from canonical PerformanceObserver IDL, observe/disconnect/... declared deliberate noops. */
JSValue js_performance_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);
#endif
