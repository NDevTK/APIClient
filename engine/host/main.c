/* APIClient v2 host entry — the ONE scheduler.
 *
 * DESIGN (rebuild, the ONE invariant): ONE persistent runtime, ONE top-level
 * scheduler loop, and EVERYTHING is a flow the loop schedules. No --fe-boot /
 * --fe-drive / --fe-deep-grind phases, no separate grind runtime, no second loop.
 *
 * Skeleton (this milestone): the loop + the flow registry, in SCHEDULER-OWNED C
 * memory (categorically separate from any flow's JS heap — the old design's
 * corruption came from putting scheduler state in the flow-churn heap). Flows are
 * value-ordered (NON-FIFO): the scheduler runs the highest-value-of-information
 * flow first. Value = emitted output (@H) so far + the fork hint. Everything is a
 * flow: boot forks children via __fork; children fork grandchildren; ONE registry,
 * ONE loop, until the registry drains.
 *
 * NOT yet here (next layers, each added on this proven loop): per-opcode
 * preemption (a running flow yields mid-quantum + resumes — the heap-stack
 * trampoline), COW snapshot per flow, RAM-evict + cross-session to IDB, real host
 * edges (fetch), Lexbor DOM, Z3.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "quickjs-libc.h"

/* ---- the ONE flow registry (scheduler-owned memory, NOT any flow's heap) ---- */
#define FLOW_MAX 4096
static JSValue g_flow_fn[FLOW_MAX];   /* the function to invoke for this flow */
static double  g_flow_val[FLOW_MAX];  /* value-of-information: fork hint + emitted @H */
static int     g_flow_n = 0;          /* live flow count */
static int     g_running = 0;         /* 1 while a flow is executing */
static double  g_cur_val = 0;         /* the running flow's value (it is NOT in the registry while running;
                                         emits accumulate here and travel with it when it re-parks) */
static int     g_emit_total = 0;      /* total @H emitted this session (the WFQ progress signal) */

/* __emit(tag): the ONLY progress signal — a real host edge (@H) surfaced. Raises the
   running flow's value so its siblings/children order behind it, and the global count. */
static JSValue js_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    printf("@H %s\n", s ? s : "?"); fflush(stdout);
    if (s) JS_FreeCString(ctx, s);
    g_emit_total++;
    if (g_running) g_cur_val += 1.0;   /* accumulates on the running flow; travels with it if it re-parks */
    return JS_UNDEFINED;
}

/* __fork(fn, hint?): add a flow to the ONE registry. Boot, branch-arms, orphan-invokes
   are ALL just this — "add a flow", scheduled by the ONE loop. Not run now; the loop
   picks it by value. hint seeds its initial value-of-information (0 if omitted). */
static JSValue js_fork(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "__fork(fn)");
    if (g_flow_n >= FLOW_MAX) { printf("@WHY {\"phase\":\"fork_full\"}\n"); return JS_UNDEFINED; }
    double hint = 0;
    if (argc > 1) JS_ToFloat64(ctx, &hint, argv[1]);
    g_flow_fn[g_flow_n] = JS_DupValue(ctx, argv[0]);
    g_flow_val[g_flow_n] = hint;
    g_flow_n++;
    return JS_UNDEFINED;
}

/* The ONE scheduler loop: pick the highest-value flow (NON-FIFO), run it, drain. A
   flow that forks children + emits raises the frontier; the loop always advances the
   highest value-of-information first. Nothing "completes the phase" — the loop just
   schedules flows until the registry is empty (later: until the disk floor, with
   parked/evicted flows resumable). */
static void scheduler_run(JSContext *ctx)
{
    while (g_flow_n > 0) {
        int best = 0;
        for (int i = 1; i < g_flow_n; i++) if (g_flow_val[i] > g_flow_val[best]) best = i;
        JSValue fn = g_flow_fn[best];
        double val = g_flow_val[best];
        /* swap-remove the picked flow BEFORE running (so its own __fork children append cleanly) */
        g_flow_n--;
        g_flow_fn[best] = g_flow_fn[g_flow_n];
        g_flow_val[best] = g_flow_val[g_flow_n];
        printf("@RUN val=%.0f n=%d\n", val, g_flow_n); fflush(stdout);
        g_running = 1; g_cur_val = val;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        g_running = 0;   /* run-to-completion for now: g_cur_val is discarded; preemption (next layer) re-parks with it */
        if (JS_IsException(r)) js_std_dump_error(ctx);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, fn);
    }
}

int main(int argc, char **argv)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "@E {\"phase\":\"newruntime\"}\n"); return 1; }
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JS_UpdateStackTop(rt);
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "@E {\"phase\":\"newcontext\"}\n"); JS_FreeRuntime(rt); return 1; }
    js_std_add_helpers(ctx, argc - 1, argv + 1);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");

    /* Install the scheduler's host functions on the global. */
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__emit", JS_NewCFunction(ctx, js_emit, "__emit", 1));
    JS_SetPropertyStr(ctx, g, "__fork", JS_NewCFunction(ctx, js_fork, "__fork", 2));
    JS_FreeValue(ctx, g);

    int rc = 0;
    if (argc > 1) {
        /* Boot is FLOW 0: eval the bundle/script once — it seeds the registry via __fork
           (and may __emit directly). Then the ONE loop schedules everything it forked. */
        JSValue v = JS_Eval(ctx, argv[1], strlen(argv[1]), "<boot>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { js_std_dump_error(ctx); rc = 1; }
        JS_FreeValue(ctx, v);
        scheduler_run(ctx);
        js_std_loop(ctx);
    }

    printf("@DONE emit=%d\n", g_emit_total); fflush(stdout);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    fflush(stdout);
    return rc;
}
