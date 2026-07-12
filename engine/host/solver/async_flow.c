/* ASYNC-AS-FLOW — see async_flow.h. A native async CALL, a settled-promise REACTION, and each fulfilled AWAIT
   are first-class BFS flows in the ONE scheduler; this owns the hooks + the cross-session async-recipe map. */
#include <stdlib.h>
#include <string.h>
#include "solver/async_flow.h"
#include "solver/scheduler.h"   /* reg_add/spawn_async_sibling + Flow + g_ctx/g_running/g_cur_val/g_cur_flow/g_dec/g_dec_n/g_c/g_in_session/g_dec_ensure */

/* Release an async-call flow's owned refs (result-promise resolve fn + a parked await promise). No-op for a
   non-async-call flow (both UNDEFINED), so safe at every Flow free site. */
void flow_free_async_refs(JSContext *ctx, Flow *f) {
    JS_FreeValue(ctx, f->aresolve); f->aresolve = JS_UNDEFINED;
    JS_FreeValue(ctx, f->areject); f->areject = JS_UNDEFINED;
    JS_FreeValue(ctx, f->await_promise); f->await_promise = JS_UNDEFINED;
    JS_FreeValue(ctx, f->rthis); f->rthis = JS_UNDEFINED;
    if (f->rargs) { for (int i = 0; i < f->rargc; i++) JS_FreeValue(ctx, f->rargs[i]); free(f->rargs); f->rargs = NULL; f->rargc = 0; }
}
/* PENDING ASYNC-RECIPE MAP — cross-session resume for async-call flows. A parked async flow persists as
   ("a" + source-hash + decvec); on resume qjs_begin loads each into this map, and it is attached to the
   re-fired async call by SOURCE HASH at TWO consult sites of ONE map: (1) a post-load SWEEP over flows the
   phase-1 boot already re-created (those calls fired before this map existed, so they cannot consult it at
   call time), and (2) reg_add_async_call for a HANDLER-triggered call that fires in phase 3, after the map
   is live. Both replay the flow's parked await/branch path instead of re-forking from scratch. Unconsumed
   entries (a handler that never re-fires this session) are freed at teardown — never a leak, never lost
   (a never-fired handler's recipe simply waits for the session where it does fire). */
static AsyncRecipe *g_arec = NULL; static int g_arec_n = 0, g_arec_cap = 0;
/* Attach the first UNUSED recipe whose source hash matches this fresh async flow's function. Ownership of
   `dec` transfers to the flow (freed at flow teardown); the map slot is marked used. Returns 1 if attached. */
int arec_attach(JSContext *ctx, Flow *f) {
    if (!f->is_async || f->dec || !JS_IsFunction(ctx, f->handle)) return 0;
    uint32_t h = JS_OrphanHash(ctx, f->handle);
    for (int i = 0; i < g_arec_n; i++) {
        if (!g_arec[i].used && g_arec[i].hash == h) {   /* an EMPTY decvec (dec=NULL) still attaches: it restores prior val/visits (UCB), exactly as an orphan recipe does */
            f->dec = g_arec[i].dec; f->dec_n = g_arec[i].dec_n; f->val = g_arec[i].val; f->visits = g_arec[i].visits;
            g_arec[i].dec = NULL; g_arec[i].used = 1;
            return 1;
        }
    }
    return 0;
}
void arec_free(void) {
    for (int i = 0; i < g_arec_n; i++) free(g_arec[i].dec);
    free(g_arec); g_arec = NULL; g_arec_n = g_arec_cap = 0;
}

/* JSAsyncCallHook target: the bundle CALLED a native async function. `fs` is its pre-created live
   JSAsyncFunctionState (real args captured); `resolve` is the result-promise's resolve fn (settled on
   COMPLETION so an `await asyncFn()` caller-flow resumes). Register it as a flow driven from its START via
   JS_FlowResume (f.fs pre-set => dispatched as a resume with an empty delta). So a fire-and-forget async
   recursion (loadPage(d.next)) is a TREE of preemptible/parkable flows, each its own bounded COW delta —
   NOT a non-preemptible promise-reaction drain whose single delta cow-oom-aborts. */
int reg_add_async_call(JSContext *ctx, void *fs, JSValueConst func_obj, JSValueConst resolve, JSValueConst reject) {
    if (ctx != g_ctx) return 0;                 /* CLAIM only for the MAIN analysis ctx; the @S solve realm (g_solve_ctx)
                                                   runs async native — routing its call into the main g_reg is cross-ctx corruption */
    Flow *f = reg_add(ctx, JS_DupValue(ctx, func_obj), g_running ? g_cur_val : 1.0, NULL, 0);
    f->fs = fs;                                 /* the live async state — the flow owns + frees it (JS_FlowResume) */
    f->is_async = 1;
    f->aresolve = JS_DupValue(ctx, resolve);
    f->areject = JS_DupValue(ctx, reject);
    /* Capture the PRISTINE re-run recipe (this + args, before the call runs) so an await FORK can spawn a
       reject-replay sibling (re-run the same call, forcing one await to reject -> the inline try/catch arm). */
    JSValueConst rthis = JS_UNDEFINED, *rargs = NULL; int rargc = 0;
    JS_FlowRecipe(fs, NULL, &rthis, &rargc, &rargs);
    f->rthis = JS_DupValue(ctx, rthis);
    if (rargc > 0 && rargs) {
        f->rargs = (JSValue *)malloc((size_t)rargc * sizeof(JSValue));
        if (f->rargs) { f->rargc = rargc; for (int i = 0; i < rargc; i++) f->rargs[i] = JS_DupValue(ctx, rargs[i]); }
    }
    arec_attach(ctx, f);   /* resume: a handler-triggered async call (fired after qjs_begin) replays its parked path */
    return 1;
}
/* Spawn a re-run sibling of an async flow: a fresh call state from the RECIPE (func+args), the decision vector
   `dec` (branch + await decisions, ownership transferred to reg_add), is_async set, recipe carried so it can
   fork further. Used by BOTH await_decide (a new await -> reject sibling) and branch_decide (an async flow's
   branch fork) — one recipe re-run path, so branch and await decisions replay together over ONE vector. */
Flow *spawn_async_sibling(JSContext *ctx, Flow *pf, signed char *dec, int dec_n) {
    void *sfs = JS_FlowNew(ctx, pf->handle, pf->rthis, pf->rargc, (JSValueConst *)pf->rargs);
    if (!sfs) { free(dec); return NULL; }
    Flow *sib = reg_add(ctx, JS_DupValue(ctx, pf->handle), g_cur_val, dec, dec_n);
    sib->fs = sfs; sib->is_async = 1;
    sib->rthis = JS_DupValue(ctx, pf->rthis);
    if (pf->rargc > 0 && pf->rargs) {
        sib->rargs = (JSValue *)malloc((size_t)pf->rargc * sizeof(JSValue));
        if (sib->rargs) { sib->rargc = pf->rargc; for (int i = 0; i < pf->rargc; i++) sib->rargs[i] = JS_DupValue(ctx, pf->rargs[i]); }
    }
    return sib;
}
/* JSFlowAwaitHook: a fulfilled await is a GATE on the ONE decision vector (g_dec/g_c), exactly like branch_decide
   at OP_if. Replay the recorded arm, or fork a REJECT sibling (re-run via recipe, this await -> reject = the inline
   try/catch catch arm) and take FULFIL. Returns 1=reject, 0=fulfil. Non-async / non-forking context -> fulfil. */
int await_decide(JSContext *ctx) {
    if (ctx != g_ctx || !g_running || !g_cur_flow || !g_cur_flow->is_async) return 0;
    if (JS_IsUndefined(g_cur_flow->handle)) return 0;
    if (g_c < g_dec_n) { int arm = g_dec[g_c] ? 1 : 0; g_c++; return arm; }   /* replay this flow's recorded await/branch decisions */
    g_dec_ensure(g_c + 1);   /* CHECK-crashes at the hard OOM wall, never fabricates a fulfil arm */
    signed char *sib = (signed char *)malloc((size_t)(g_c + 1));   /* fork the REJECT sibling; this flow takes FULFIL */
    if (sib) { for (int i = 0; i < g_c; i++) sib[i] = g_dec[i]; sib[g_c] = 1; spawn_async_sibling(ctx, g_cur_flow, sib, g_c + 1); }
    g_dec[g_c] = 0; g_dec_n = g_c + 1; g_c++;
    return 0;
}
/* JSReactionHook target: a settled promise's REACTION (.then/.catch/.finally handler). Run handler(value) as a
   preemptible/parkable flow and SETTLE the chained promise on completion (resolve with the return value / reject
   on throw) — so a .then-based recursion is a TREE of flows, not a non-preemptible drain. A pass-through reaction
   (no handler) settles the chained promise directly. Non-bytecode handler / non-main-ctx -> native job (return 0). */
int reaction_flow(JSContext *ctx, JSValueConst handler, JSValueConst value, int is_reject, JSValueConst resolve, JSValueConst reject) {
    if (ctx != g_ctx) return 0;                 /* main analysis ctx only, never the @S solve realm */
    if (JS_IsUndefined(handler)) {              /* pass-through: fulfill->resolve, reject->reject the chained promise with value */
        JSValueConst fn = is_reject ? reject : resolve;
        if (!JS_IsUndefined(fn)) { JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&value); if (JS_IsException(r)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); } JS_FreeValue(ctx, r); }
        return 1;
    }
    void *fs = JS_FlowNew(ctx, handler, JS_UNDEFINED, 1, (JSValueConst *)&value);
    if (!fs) return 0;                          /* non-bytecode handler (C fn / bound) -> native job */
    Flow *f = reg_add(ctx, JS_DupValue(ctx, handler), g_running ? g_cur_val : 1.0, NULL, 0);
    f->fs = fs;
    /* HANDLER-TIME ASYNC @S: a reaction spawned while a session fired the handler inherits the session sink-CONTEXT
       (so a sink it reaches enqueues a candidate SESSION) and, if the parent is a candidate replay, the candidate
       (so when it resumes g_candidate is pinned and the sink takes solve_add's VERIFY branch — the awaited value
       IS the candidate). This is what makes addEventListener('message', e=>P.resolve(e.data).then(t=>sink)) solve. */
    f->sess_ctx = g_in_session || (g_cur_flow && g_cur_flow->sess_ctx);
    if (g_cur_flow && g_cur_flow->candidate) f->candidate = strdup(g_cur_flow->candidate);
    f->aresolve = JS_DupValue(ctx, resolve);
    f->areject = JS_DupValue(ctx, reject);
    return 1;
}

/* Park an async flow's REPLAY RECIPE (source-hash + decision vector + UCB state) into the pending map, so a
   later re-fire of that async call (arec_attach) replays its await/branch path instead of re-forking. Called
   by the engine's resume path (qjs_begin) when it loads a parked async recipe. Takes `dec` ownership. */
void arec_park(uint32_t hash, signed char *dec, int dec_n, double val, int visits) {
    if (g_arec_n >= g_arec_cap) { int nc = g_arec_cap ? g_arec_cap * 2 : 8;
        AsyncRecipe *na = (AsyncRecipe *)realloc(g_arec, (size_t)nc * sizeof(AsyncRecipe)); if (na) { g_arec = na; g_arec_cap = nc; } }
    if (g_arec_n < g_arec_cap) { g_arec[g_arec_n].hash = hash; g_arec[g_arec_n].dec = dec; g_arec[g_arec_n].dec_n = dec_n;
        g_arec[g_arec_n].val = val; g_arec[g_arec_n].visits = visits; g_arec[g_arec_n].used = 0; g_arec_n++; }
    else free(dec);
}
