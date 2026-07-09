/* Timers — see timers.h. Extracted from main.c. setTimeout/setInterval/requestAnimationFrame/
 * requestIdleCallback/queueMicrotask all funnel here: a FUNCTION callback becomes a first-class BFS flow
 * (flow_defer_callback — the scheduler edge), so deferred init is explored like any other flow; a STRING
 * argument (setTimeout('code')) is an eval-class @S sink (solve_add). Returns an opaque timer id; clear/cancel
 * are no-ops (the WFQ starves an unproductive flow anyway). */
#include "timers.h"
#include "solve.h"    /* solve_add — setTimeout(string) EVALs -> js-context @S sink */
#include "opaque.h"   /* g_opaque */

extern void flow_defer_callback(JSContext *ctx, JSValueConst cb);   /* scheduler-side: register a callback as a BFS flow */

JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc >= 1 && JS_IsFunction(ctx, argv[0]))
        flow_defer_callback(ctx, argv[0]);
    else if (argc >= 1 && (JS_IsString(argv[0]) || JS_IsOpaque(argv[0])))
        solve_add(ctx, "setTimeout", "js", argv[0]);   /* setTimeout(STRING) evals -> js sink (candidate breakout + opaque detect) */
    return js_concolic(ctx, "{timerId}", JS_UNDEFINED);
}
