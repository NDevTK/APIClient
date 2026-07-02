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
typedef struct {
    JSValue handle;      /* the function (starter/suspended) or resolve fn (async __yield resume) */
    double val;          /* accumulated value-of-information (emits raise it) */
    int is_resume;       /* async __yield resume: JS_Call the resolve fn (not sync-preemptible) */
    signed char *dec; int dec_n;  /* per-flow decision vector (branch-arm BFS) */
    void *fs;            /* live heap frame if SUSPENDED mid-run (JS_FlowResume), else NULL */
    int saved_c;         /* per-flow branch cursor (g_c) snapshot, restored on resume */
    double cpu;          /* back-edge CPU ticks since last emit (WFQ decay; reset to 0 on emit) */
    int visits;          /* times scheduled (UCB/fairness explore term) */
} Flow;
static Flow   *g_reg = NULL;
static int     g_reg_n = 0, g_reg_cap = 0;
static int     g_running = 0;
static double  g_cur_val = 0;
static Flow   *g_cur_flow = NULL;   /* running flow (a stable local copy; its weight is read by the yield hook) */
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
    g_reg[g_reg_n].fs = NULL; g_reg[g_reg_n].saved_c = 0; g_reg[g_reg_n].cpu = 0; g_reg[g_reg_n].visits = 0;
    g_reg_n++; return 1;
}

/* Re-add a SUSPENDED flow (a full copy, fs retained) so it interleaves back into the ONE registry. */
static int reg_readd(JSContext *ctx, Flow f)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        if (!nr) { printf("@WHY {\"phase\":\"reg_oom\"}\n");
                   if (f.fs) JS_FlowFree(g_rt, f.fs); JS_FreeValue(ctx, f.handle); free(f.dec); return 0; }
        g_reg = nr; g_reg_cap = nc;
    }
    g_reg[g_reg_n++] = f; return 1;
}

/* __emit(tag): the ONLY progress signal — a real host edge (@H) surfaced. */
static JSValue js_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    printf("@H %s\n", s ? s : "?"); fflush(stdout);
    if (s) JS_FreeCString(ctx, s);
    g_emit_total++;
    if (g_running) { g_cur_val += 1.0; if (g_cur_flow) { g_cur_flow->val = g_cur_val; g_cur_flow->cpu = 0; } }
    return JS_UNDEFINED;
}

/* fetch(url): the moat's host edge. URL = whatever the bundle COMPUTED. Emit @H, raise value, return a
   resolved promise wrapping a minimal response so `await fetch(...)`/`.then` continue. */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    printf("@H %s\n", url ? url : "?"); fflush(stdout);
    g_emit_total++;
    if (g_running) { g_cur_val += 1.0; if (g_cur_flow) { g_cur_flow->val = g_cur_val; g_cur_flow->cpu = 0; } }
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
/* Minimal host-edge stubs for a browser bundle: a no-op (addEventListener etc — the handler stays a
   never-fired function, so orphan-invoke drives it), and an opaque-returning stub (DOM reads that are
   external input the tool must not concretely decide). A missing capability is a missing stub, never a
   parallel resolver — the real Lexbor DOM replaces these when the host is wired. */
static JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_UNDEFINED; }
static JSValue js_opaque_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_DupValue(ctx, g_opaque); }
/* addEventListener(type, handler): a registered handler that NEVER FIRES is exactly the unused surface —
   keep it reachable (in g_handlers) so orphan-invoke drives it and surfaces its gated endpoints. */
static JSValue g_handlers = JS_UNDEFINED;
static int g_handler_n = 0;
static JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    JSValueConst h = (argc >= 2) ? argv[1] : (argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(g_handlers))
        JS_SetPropertyUint32(ctx, g_handlers, (uint32_t)g_handler_n++, JS_DupValue(ctx, h));
    return JS_UNDEFINED;
}

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

/* WFQ order key (ORDER-only, never drops a work item): base + accumulated value-of-information
   (emits raise val) + an explore/fairness floor for rarely-scheduled flows - a CPU-since-emit decay
   so a monopolizer (burns CPU, emits nothing) SINKS below productive/unrun flows and gets starved.
   This is the ONE policy the host-level priority.js uses too; same formula, two levels. */
static double flow_weight(const Flow *f)
{
    return 1.0 + f->val + 0.5 / (double)(f->visits + 1) - 0.01 * f->cpu;
}

/* The value-driven yield decision, called by the engine at each loop back-edge of the running TOP
   flow frame. Ticks CPU (accounting, NOT a cap — the flow is resumable, never truncated), then yields
   iff a PARKED flow now outranks the running one. The empty-COW-delta guard keeps interleaving correct
   without per-flow deltas yet: a shared-state writer runs to completion (never suspended mid-write). */
static int wfq_yield(void)
{
    if (!g_cur_flow) return 0;
    g_cur_flow->cpu += 1.0;
    if (JS_CowDepth() > 0) return 0;                 /* correctness guard: don't preempt a shared-state writer */
    double rw = flow_weight(g_cur_flow);
    for (int i = 0; i < g_reg_n; i++)
        if (flow_weight(&g_reg[i]) > rw) return 1;   /* a parked flow now outranks the running quantum */
    return 0;
}

/* The ONE scheduler loop: pick the highest-WEIGHT flow (NON-FIFO), run it as a preemptible heap-frame
   quantum, re-queue it if it suspended (interleave), repeat. BFS by value-of-information: shallow
   high-emit flows finish ahead of the deep residue, which is starved to ~0 CPU (resumable). */
static void scheduler_run(JSContext *ctx)
{
    while (g_reg_n > 0) {
        int best = 0;
        for (int i = 1; i < g_reg_n; i++)
            if (flow_weight(&g_reg[i]) > flow_weight(&g_reg[best])) best = i;
        Flow f = g_reg[best];                       /* f is a STABLE COPY: reg_add during the run may realloc g_reg */
        g_reg_n--; g_reg[best] = g_reg[g_reg_n];    /* swap-remove */
        f.visits++;
        g_running = 1; g_cur_val = f.val; g_cur_flow = &f;

        if (f.is_resume) {
            /* async __yield resume: drive the microtask to completion (a resolve fn, not sync-preemptible) */
            g_cur_fn = JS_UNDEFINED; g_dec_n = 0; g_c = 0;
            JS_SetFlowYieldHook(NULL);
            JSValue u = JS_UNDEFINED;
            JSValue r = JS_Call(ctx, f.handle, JS_UNDEFINED, 1, &u);
            if (JS_IsException(r)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, r);
            JSContext *c1; int jr;
            while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
            if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
            g_running = 0; g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec);
            continue;
        }

        /* SYNC flow: load its per-flow scheduler state (decision vector + branch cursor). __branch
           consumes/extends g_dec + forks siblings; force-invoke with OPAQUE this+args (external input
           the tool must not concretely decide) so gates fork and computed URLs are shaped. */
        g_cur_fn = f.handle;
        g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
        for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
        g_c = f.saved_c;

        if (f.fs == NULL) {                         /* STARTER: baseline + fresh heap frame */
            JS_CowRevert(ctx);                      /* revert shared-state to the post-boot baseline (safe: only empty-delta flows suspend) */
            JSValue oargs[8]; for (int i = 0; i < 8; i++) oargs[i] = g_opaque;
            f.fs = JS_FlowNew(ctx, f.handle, g_opaque, 8, oargs);
            if (!f.fs) { g_running = 0; g_cur_flow = NULL; JS_FreeValue(ctx, f.handle); free(f.dec); continue; }
        }
        /* else SUSPENDED: resume in place. Its COW delta was empty at suspend + starters revert to
           baseline, so shared state == baseline == what this flow expects (it wrote nothing shared). */

        JS_SetFlowYieldHook(wfq_yield);
        JSValue out = JS_UNDEFINED;
        int st = JS_FlowResume(ctx, f.fs, &out);
        JS_SetFlowYieldHook(NULL);
        JSContext *c1; int jr;
        while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
        if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
        g_running = 0; g_cur_fn = JS_UNDEFINED;

        if (st == 1) {
            /* SUSPENDED at a back-edge: snapshot per-flow scheduler state and RE-QUEUE (interleave). */
            f.saved_c = g_c;
            f.val = g_cur_val;
            if (g_dec_n > f.dec_n) {                /* grew (new branch decisions taken this quantum) */
                signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1));
                if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; }
            } else {
                for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i];
            }
            f.dec_n = g_dec_n;
            g_cur_flow = NULL;
            reg_readd(ctx, f);
        } else {
            /* COMPLETED/error: JS_FlowResume already freed the heap frame + its values. */
            if (JS_IsException(out)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, out);
            g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec);
        }
    }
    JS_SetFlowYieldHook(NULL);
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

    /* Minimal browser environment: window/self = globalThis; OPAQUE external-input sources (location,
       document.cookie, navigator, referrer) so a real bundle's gate on external input auto-forks without
       synthetic args; addEventListener = no-op (the handler is then a never-fired orphan, driven). This
       is the seam the real Lexbor DOM + real safe-fetch plug into during the host rewire. */
    g_handlers = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, g, "__handlers", JS_DupValue(ctx, g_handlers));   /* reachable so handlers survive to orphan-collect */
    JS_SetPropertyStr(ctx, g, "window", JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "self",   JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "globalThis", JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "location", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, g, "navigator", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, g, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, g, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    {
        JSValue doc = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, doc, "cookie", JS_DupValue(ctx, g_opaque));
        JS_SetPropertyStr(ctx, doc, "referrer", JS_DupValue(ctx, g_opaque));
        JS_SetPropertyStr(ctx, doc, "URL", JS_DupValue(ctx, g_opaque));
        JS_SetPropertyStr(ctx, doc, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
        JS_SetPropertyStr(ctx, doc, "querySelector", JS_NewCFunction(ctx, js_opaque_stub, "querySelector", 1));
        JS_SetPropertyStr(ctx, doc, "getElementById", JS_NewCFunction(ctx, js_opaque_stub, "getElementById", 1));
        JS_SetPropertyStr(ctx, doc, "createElement", JS_NewCFunction(ctx, js_opaque_stub, "createElement", 1));
        JS_SetPropertyStr(ctx, g, "document", doc);
    }
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
    /* Clean teardown (else JS_FreeRuntime asserts gc_obj_list non-empty): stop + revert the COW log so
       its held baseline values return to their slots, and drop the opaque marker. */
    JS_CowSetActive(0);
    JS_CowRevert(ctx);
    JS_SetOpaqueMarker(JS_UNDEFINED); JS_SetBranchHook(NULL);
    JS_FreeValue(ctx, g_opaque); g_opaque = JS_UNDEFINED;
    JS_FreeValue(ctx, g_handlers); g_handlers = JS_UNDEFINED;
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    fflush(stdout);
    return rc;
}
