/* APIClient v2 host entry — the ONE scheduler.
 *
 * DESIGN (the ONE invariant): ONE persistent runtime, ONE top-level scheduler loop,
 * EVERYTHING is a flow the loop schedules. No phases, no separate grind, no second loop.
 *
 * This milestone adds PREEMPTION on top of the value-ordered registry: a flow parks
 * mid-execution and RESUMES, so the scheduler interleaves flows by value-of-information
 * rather than running each to completion. Mechanism = cooperative yield on quickjs-ng's
 * OWN async-frame suspend (NOT JSPI — no C-stack save/teardown desync; NOT a trampoline
 * yet). A flow is an async function; `await __yield()` parks it (quickjs suspends the async
 * frame); the scheduler resumes it by resolving that promise, choosing WHICH parked flow
 * resumes next by value (non-FIFO). Per-opcode preemption of a deep SYNC loop is a later,
 * separately-scoped capability (the heap-stack trampoline) — real orphan-driving is shallow.
 *
 * Flow registry lives in SCHEDULER-OWNED C memory (categorically separate from any flow's
 * JS heap — the old design's corruption came from putting scheduler state in the flow heap).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "quickjs-libc.h"

/* ---- the ONE flow registry (scheduler-owned memory) --------------------------------
   A flow is either a STARTER (an un-run function to invoke) or a RESUMER (the resolve
   function of a parked flow's __yield promise — calling it wakes the flow). Both carry a
   value-of-information; the loop always advances the highest first. */
#define FLOW_MAX 8192
typedef struct { JSValue handle; double val; int is_resume; } Flow;
static Flow    g_reg[FLOW_MAX];
static int     g_reg_n = 0;
static int     g_running = 0;         /* 1 while a flow's code is executing */
static double  g_cur_val = 0;         /* the running flow's value; travels into its next __yield resumer */
static int     g_emit_total = 0;      /* total @H emitted this session (the WFQ progress signal) */
static JSRuntime *g_rt = NULL;

static int reg_add(JSContext *ctx, JSValue handle, double val, int is_resume)
{
    if (g_reg_n >= FLOW_MAX) { printf("@WHY {\"phase\":\"reg_full\"}\n"); JS_FreeValue(ctx, handle); return 0; }
    g_reg[g_reg_n].handle = handle; g_reg[g_reg_n].val = val; g_reg[g_reg_n].is_resume = is_resume;
    g_reg_n++; return 1;
}

/* __emit(tag): the ONLY progress signal — a real host edge (@H) surfaced. Raises the running
   flow's value so its continuation/children order ahead of quiet flows. */
static JSValue js_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    printf("@H %s\n", s ? s : "?"); fflush(stdout);
    if (s) JS_FreeCString(ctx, s);
    g_emit_total++;
    if (g_running) g_cur_val += 1.0;
    return JS_UNDEFINED;
}

/* __fork(fn, hint?): add a flow to the ONE registry (a STARTER). Boot, branch-arms,
   orphan-invokes are all just this. Not run now; the loop picks it by value. */
static JSValue js_fork(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "__fork(fn)");
    double hint = 0; if (argc > 1) JS_ToFloat64(ctx, &hint, argv[1]);
    reg_add(ctx, JS_DupValue(ctx, argv[0]), hint, 0);
    return JS_UNDEFINED;
}

/* __yield(): PARK the running flow. Returns a promise the flow awaits (quickjs suspends its
   async frame); its resolve function becomes a RESUMER in the registry carrying the flow's
   current value. The scheduler decides WHICH resumer fires next (non-FIFO), so a low-value
   flow's next quantum starves behind a high-value flow's — the WFQ, per-quantum. */
static JSValue js_yield(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    reg_add(ctx, resolving[0], g_cur_val, 1);   /* keep resolve as the resumer */
    JS_FreeValue(ctx, resolving[1]);            /* reject unused */
    return promise;
}

/* The ONE scheduler loop: pick the highest-value flow (NON-FIFO), advance it ONE quantum
   (start it, or resume it to its next __yield / completion), repeat until the registry
   drains. A STARTER async fn runs synchronously to its first await inside JS_Call. A RESUMER
   is woken by resolving its promise, then its continuation job is drained to the next yield. */
static void scheduler_run(JSContext *ctx)
{
    while (g_reg_n > 0) {
        int best = 0;
        for (int i = 1; i < g_reg_n; i++) if (g_reg[i].val > g_reg[best].val) best = i;
        Flow f = g_reg[best];
        g_reg_n--; g_reg[best] = g_reg[g_reg_n];   /* swap-remove */
        printf("@RUN val=%.0f resume=%d n=%d\n", f.val, f.is_resume, g_reg_n); fflush(stdout);
        g_running = 1; g_cur_val = f.val;
        if (f.is_resume) {
            JSValue u = JS_UNDEFINED;
            JSValue r = JS_Call(ctx, f.handle, JS_UNDEFINED, 1, &u);   /* resolve -> enqueue continuation */
            if (JS_IsException(r)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, r);
            JSContext *c1; int jr;
            while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { /* run the woken continuation to next yield */ }
            if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
        } else {
            JSValue r = JS_Call(ctx, f.handle, JS_UNDEFINED, 0, NULL); /* start: runs to first await/return */
            if (JS_IsException(r)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, r);
        }
        g_running = 0;
        JS_FreeValue(ctx, f.handle);
    }
}

int main(int argc, char **argv)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "@E {\"phase\":\"newruntime\"}\n"); return 1; }
    g_rt = rt;
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JS_UpdateStackTop(rt);
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "@E {\"phase\":\"newcontext\"}\n"); JS_FreeRuntime(rt); return 1; }
    js_std_add_helpers(ctx, argc - 1, argv + 1);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__emit", JS_NewCFunction(ctx, js_emit, "__emit", 1));
    JS_SetPropertyStr(ctx, g, "__fork", JS_NewCFunction(ctx, js_fork, "__fork", 2));
    JS_SetPropertyStr(ctx, g, "__yield", JS_NewCFunction(ctx, js_yield, "__yield", 0));
    JS_FreeValue(ctx, g);

    int rc = 0;
    if (argc > 1) {
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
