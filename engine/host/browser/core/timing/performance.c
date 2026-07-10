/* Performance — see performance.h. Extracted from main.c and made spec-complete: now() is elapsed time
 * (nondeterministic external -> opaque, so a branch on it forks); mark/measure/clearMarks/clearMeasures are
 * no-ops (no real timeline); the getEntries family returns an EMPTY ARRAY (not undefined — a page's
 * `performance.getEntriesByType('resource').forEach(...)` must not throw); timeOrigin is concrete. The old
 * `{now}`-only stub threw on mark/measure, killing perf-instrumented bundles. */
#include "core/timing/performance.h"
#include "opaque.h"   /* g_opaque, js_noop, js_opaque */

static JSValue js_perf_empty(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v; return JS_NewArray(ctx);   /* PerformanceEntryList -> empty (no timeline headless) */
}

JSValue js_performance_make(JSContext *ctx) {
    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf, "now", JS_NewCFunction(ctx, js_opaque, "now", 0));         /* elapsed ms -> opaque (nondeterministic) */
    JS_SetPropertyStr(ctx, perf, "timeOrigin", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, perf, "mark", JS_NewCFunction(ctx, js_noop, "mark", 1));
    JS_SetPropertyStr(ctx, perf, "measure", JS_NewCFunction(ctx, js_noop, "measure", 1));
    JS_SetPropertyStr(ctx, perf, "clearMarks", JS_NewCFunction(ctx, js_noop, "clearMarks", 0));
    JS_SetPropertyStr(ctx, perf, "clearMeasures", JS_NewCFunction(ctx, js_noop, "clearMeasures", 0));
    JS_SetPropertyStr(ctx, perf, "getEntries", JS_NewCFunction(ctx, js_perf_empty, "getEntries", 0));
    JS_SetPropertyStr(ctx, perf, "getEntriesByType", JS_NewCFunction(ctx, js_perf_empty, "getEntriesByType", 1));
    JS_SetPropertyStr(ctx, perf, "getEntriesByName", JS_NewCFunction(ctx, js_perf_empty, "getEntriesByName", 1));
    return perf;
}
