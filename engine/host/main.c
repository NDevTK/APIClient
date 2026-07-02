/* APIClient v2 host entry — the ONE scheduler.
 *
 * DESIGN (the ONE invariant): ONE persistent runtime, ONE top-level scheduler loop, EVERYTHING is a
 * flow the loop schedules. No phases, no separate grind, no second loop.
 *
 * Capabilities so far, each verified on the proven loop:
 *  - value-ordered (NON-FIFO) flow registry in scheduler-owned C memory; everything-a-flow (__fork).
 *  - PREEMPTION: a flow parks (await __yield) + resumes WITH STATE via quickjs-ng async-frame suspend.
 *  - fetch(url) host edge -> @H (the COMPUTED endpoint), all flow code runs in the ONE loop.
 *  - ORPHAN-INVOKE: force-invoke never-executed functions (JS_CollectOrphans) -> the UNUSED endpoints.
 *  - FORCED BRANCH-ARMS (this milestone): __branch() explores BOTH arms of a gated branch by
 *    decision-vector BFS — a flow re-runs its function with a forced-choice table; a new decision
 *    returns true for this flow and FORKS a sibling that replays the prefix then takes false. This
 *    surfaces the branch-gated (login/flag-gated) endpoints. Value-ordered, so productive paths first.
 *    (Auto-forking at OP_if on OPAQUE external input — so real bundles need no __branch — is the next
 *    engine capability; the decision-vector scheduling is proven here.)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "quickjs-libc.h"

/* ---- the ONE flow registry (scheduler-owned memory) --------------------------------
   A flow is a STARTER (a function to force-invoke, with a decision vector for its branch choices) or
   a RESUMER (the resolve fn of a parked flow's __yield promise). Both carry value-of-information. */
/* NO BOUNDS: the registry and the decision vector grow dynamically (until RAM/disk, the platform floor
   — the design's UNBOUNDED). No FLOW_MAX / DEC_MAX cap that would truncate distinct work the scheduler
   would otherwise reach. (Eviction of the cold/low-value tail to IDB is the further step; growth removes
   the artificial ceiling first.) */
typedef struct { JSValue handle; double val; int is_resume; signed char *dec; int dec_n; } Flow;
static Flow   *g_reg = NULL;
static int     g_reg_n = 0, g_reg_cap = 0;
static int     g_running = 0;
static double  g_cur_val = 0;
static int     g_emit_total = 0;
static JSRuntime *g_rt = NULL;

/* decision-vector state for the RUNNING starter flow (branch-arm BFS) — grows unbounded */
static JSValue      g_cur_fn = JS_UNDEFINED;   /* the running starter's function (borrowed) so __branch can fork a sibling that re-runs it */
static signed char *g_dec = NULL;              /* working decision vector: forced prefix + this flow's chosen-true suffix */
static int          g_dec_cap = 0;
static int          g_dec_n = 0;               /* length of decisions made/forced so far */
static int          g_c = 0;                   /* cursor: next decision index __branch will consume */

static int g_dec_ensure(int n) {              /* grow g_dec to hold >= n decisions */
    if (n <= g_dec_cap) return 1;
    int nc = g_dec_cap ? g_dec_cap * 2 : 64; while (nc < n) nc *= 2;
    signed char *nd = (signed char *)realloc(g_dec, (size_t)nc);
    if (!nd) return 0;
    g_dec = nd; g_dec_cap = nc; return 1;
}

static int reg_add(JSContext *ctx, JSValue handle, double val, int is_resume, signed char *dec, int dec_n)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        if (!nr) { printf("@WHY {\"phase\":\"reg_oom\"}\n"); JS_FreeValue(ctx, handle); free(dec); return 0; }
        g_reg = nr; g_reg_cap = nc;
    }
    g_reg[g_reg_n].handle = handle; g_reg[g_reg_n].val = val; g_reg[g_reg_n].is_resume = is_resume;
    g_reg[g_reg_n].dec = dec; g_reg[g_reg_n].dec_n = dec_n;
    g_reg_n++; return 1;
}

/* __emit(tag): the ONLY progress signal — a real host edge (@H) surfaced. */
static JSValue js_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    printf("@H %s\n", s ? s : "?"); fflush(stdout);
    if (s) JS_FreeCString(ctx, s);
    g_emit_total++; if (g_running) g_cur_val += 1.0;
    return JS_UNDEFINED;
}

/* fetch(url): the moat's host edge. URL = whatever the bundle COMPUTED. Emit @H, raise value, return a
   resolved promise wrapping a minimal response so `await fetch(...)`/`.then` continue. */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    printf("@H %s\n", url ? url : "?"); fflush(stdout);
    g_emit_total++; if (g_running) g_cur_val += 1.0;
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url ? url : ""));
    JS_SetPropertyStr(ctx, resp, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, 200));
    if (url) JS_FreeCString(ctx, url);
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (!JS_IsException(promise)) {
        JSValue rr = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, &resp); JS_FreeValue(ctx, rr);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
    }
    JS_FreeValue(ctx, resp);
    return promise;
}

/* __branch(): a FORCED DECISION POINT (a gate on opaque external input). If the running flow's decision
   vector already fixes this point, replay it. Otherwise it's a NEW branch: FORK a sibling flow that
   re-runs the SAME function, replaying this flow's decisions so far then taking FALSE here; this flow
   takes TRUE (recorded so deeper new branches fork correctly). BFS over the decision tree -> both arms
   of every gate are explored, surfacing the branch-gated endpoints. */
/* branch_decide: the decision-vector fork logic (0/1). Called BOTH by __branch() (explicit) and by the
   engine's OP_if hook when a branch condition is OPAQUE (real bundles). Forced replay of this flow's
   decision prefix; a NEW decision forks the FALSE sibling (re-run the same function) and takes TRUE. */
static int branch_decide(JSContext *ctx)
{
    if (!g_running || JS_IsUndefined(g_cur_fn)) return 0;   /* only meaningful inside a starter flow */
    if (g_c < g_dec_n) return g_dec[g_c++] ? 1 : 0;         /* forced replay */
    if (!g_dec_ensure(g_c + 1)) return 1;                    /* only RAM/disk (the platform floor) bounds depth — not a cap */
    signed char *sib = (signed char *)malloc((size_t)(g_c + 1));
    if (sib) {
        for (int i = 0; i < g_c; i++) sib[i] = g_dec[i];
        sib[g_c] = 0;
        reg_add(ctx, JS_DupValue(ctx, g_cur_fn), g_cur_val, 0, sib, g_c + 1);
    }
    g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++;
    return 1;
}
static JSValue js_branch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return branch_decide(ctx) ? JS_TRUE : JS_FALSE; }

/* __opaque(): return the OPAQUE sentinel — external input the tool must not concretely decide. A branch
   on it (if(__opaque())) auto-forks BOTH arms via the engine OP_if hook, no explicit __branch needed. */
static JSValue g_opaque = JS_UNDEFINED;
static JSValue js_opaque(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_DupValue(ctx, g_opaque); }

/* __fork(fn, hint?): add a flow to the ONE registry (a STARTER, fresh decision vector). */
static JSValue js_fork(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "__fork(fn)");
    double hint = 0; if (argc > 1) JS_ToFloat64(ctx, &hint, argv[1]);
    reg_add(ctx, JS_DupValue(ctx, argv[0]), hint, 0, NULL, 0);
    return JS_UNDEFINED;
}

/* __yield(): PARK the running flow; its resolve fn becomes a RESUMER carrying the flow's value. */
static JSValue js_yield(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) return promise;
    reg_add(ctx, rf[0], g_cur_val, 1, NULL, 0);
    JS_FreeValue(ctx, rf[1]);
    return promise;
}

/* orphan flow source (idempotent, main-driven after boot): never-executed fns become starter flows. */
static int g_orphans_seeded = 0;
static int seed_orphans(JSContext *ctx)
{
    if (g_orphans_seeded) return 0;
    g_orphans_seeded = 1;
    static JSValue buf[4096];
    int n = JS_CollectOrphans(ctx, buf, 4096), seeded = 0;
    for (int i = 0; i < n; i++) {
        /* Skip a function already queued as a flow (e.g. an explicitly __fork'd one not yet run) — else it
           would be force-invoked TWICE. Real bundles don't __fork, so this is usually a no-op. */
        int dup = 0;
        for (int j = 0; j < g_reg_n; j++)
            if (!g_reg[j].is_resume && JS_VALUE_GET_PTR(g_reg[j].handle) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        if (dup) { JS_FreeValue(ctx, buf[i]); continue; }
        reg_add(ctx, buf[i], 1.0, 0, NULL, 0);
        seeded++;
    }
    printf("@ORPHANS %d\n", seeded); fflush(stdout);
    return seeded;
}
static JSValue js_drive_orphans(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_NewInt32(ctx, seed_orphans(ctx)); }

/* Per-flow isolation is the engine COW (JS_CowSetActive/JS_CowRevert): shared-state writes (var-refs =
   globals/lexicals/closures; baseline-object property mutations) are captured while a flow explores, and
   reverted to the post-boot BASELINE before the next STARTER runs — so an independent flow never sees
   another's writes, yet a flow sees its OWN writes within its run. */

/* The ONE scheduler loop: pick the highest-value flow (NON-FIFO), advance it ONE quantum, repeat. */
static void scheduler_run(JSContext *ctx)
{
    while (g_reg_n > 0) {
        int best = 0;
        for (int i = 1; i < g_reg_n; i++) if (g_reg[i].val > g_reg[best].val) best = i;
        Flow f = g_reg[best];
        g_reg_n--; g_reg[best] = g_reg[g_reg_n];   /* swap-remove */
        g_running = 1; g_cur_val = f.val;
        JSValue r;
        if (f.is_resume) {
            JSValue u = JS_UNDEFINED;
            g_cur_fn = JS_UNDEFINED; g_dec_n = 0; g_c = 0;
            r = JS_Call(ctx, f.handle, JS_UNDEFINED, 1, &u);
        } else {
            /* STARTER: load its decision vector, run it. __branch consumes/extends g_dec + forks siblings.
               Force-invoke with OPAQUE this + args (external input the tool must not concretely decide):
               user.isAdmin / this.fooUrl become opaque (propagated), so gates fork and computed URLs are
               shaped. A flow that ignores args (boot forks) is unaffected. */
            g_cur_fn = f.handle;
            g_dec_n = f.dec_n;
            g_dec_ensure(g_dec_n);
            for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
            g_c = 0;
            JS_CowRevert(ctx);   /* per-flow isolation: revert shared-state to the post-boot baseline */
            JSValue oargs[8]; for (int i = 0; i < 8; i++) oargs[i] = g_opaque;
            r = JS_Call(ctx, f.handle, g_opaque, 8, oargs);
        }
        if (JS_IsException(r)) js_std_dump_error(ctx);
        JS_FreeValue(ctx, r);
        JSContext *c1; int jr;
        while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
        if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
        g_running = 0; g_cur_fn = JS_UNDEFINED;
        JS_FreeValue(ctx, f.handle);
        free(f.dec);
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
    JS_SetPropertyStr(ctx, g, "__branch", JS_NewCFunction(ctx, js_branch, "__branch", 0));
    JS_SetPropertyStr(ctx, g, "__opaque", JS_NewCFunction(ctx, js_opaque, "__opaque", 0));
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    /* Register the OPAQUE sentinel + the branch hook: a branch whose condition IS this object forks both
       arms via the decision-vector logic (real bundles: external input reaches OP_if as opaque). */
    g_opaque = JS_NewObject(ctx);   /* kept alive for the process; marker is pointer identity */
    JS_SetOpaqueMarker(g_opaque);
    JS_SetBranchHook(branch_decide);
    JS_SetPropertyStr(ctx, g, "__driveOrphans", JS_NewCFunction(ctx, js_drive_orphans, "__driveOrphans", 0));
    JS_FreeValue(ctx, g);

    int rc = 0;
    if (argc > 1) {
        JSValue v = JS_Eval(ctx, argv[1], strlen(argv[1]), "<boot>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { js_std_dump_error(ctx); rc = 1; }
        JS_FreeValue(ctx, v);
        seed_orphans(ctx);
        JS_CowSetActive(1);   /* baseline = post-boot state; capture shared-state writes during flow exploration */
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
