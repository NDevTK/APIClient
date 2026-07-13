/* THE ONE WFQ DISPATCH LOOP — the scheduler core. Picks the highest-value flow (non-FIFO), runs it as a
   preemptible heap-frame flow (per-opcode value-yield + cooperative quantum), re-queues it if suspended,
   parks the cold tail under RAM pressure. This is the forced-exec event loop; the flow TYPES it dispatches
   (orphan/boot/candidate/session/async) live in their own components and are reached through the headers
   below. The engine entry (main.c) drives it via scheduler_run / seed_orphans and reads its bookkeeping
   through scheduler.h. See scheduler.h. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "solver/scheduler.h"
#include "solver/solve.h"           /* solve_is_verified — skip a redundant candidate flow */
#include "solver/boot_flow.h"       /* boot_replay/_candidate + resolve_replayed_handler + reg_add_boot + g_boot_delta */
#include "solver/async_flow.h"      /* flow_free_async_refs at flow teardown */
#include "solver/boot_scripts.h"
#include "core/dom/handler_registry.h"   /* is_msg_handler — drive a 'message' listener with the {pm} event */
#include "core/dom/events/event.h"       /* js_event_ctor — a driven non-message handler gets a real DOM Event */
#include "core/html/html_script_element.h"   /* script_load_gated — a <script> load handler is not eligible until its chunk provides */
#include <emscripten.h>              /* emscripten_get_now — the cooperative-quantum wall clock */
#include "solver/wfq.h"             /* wfq_weight — the ONE WFQ priority policy */
#include "solver/dom_cow.h"         /* dom_buf_load/free — per-flow DOM COW delta swap on context switch */
#include "solver/heap_cow.h"      /* per-flow HEAP COW delta (verb-API): apply/unapply/revert/fork/buf/base/boot */
#include "solver/constraints.h"     /* cons_reset — clear the per-flow value domain at flow start */
#include "solver/why.h"             /* why_add — a runtime-reasoned @WHY */
#include "quickjs-libc.h"           /* js_std_dump_error — dump an uncaught exception during a flow */

extern JSRuntime *g_rt;             /* the JS runtime (main.c) — per-fn microtask drain */
extern void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s);   /* push a string into a JS array (main.c util) */

/* ── the scheduler's OWN state: the registry + WFQ/dispatch bookkeeping + the running-flow decision vector.
   Defined here (the entry reads some via scheduler.h extern); the flow components never touch g_reg. */
int g_in_session = 0;
Flow *g_reg = NULL; int g_reg_n = 0, g_reg_cap = 0;
int g_running = 0; double g_cur_val = 0; Flow *g_cur_flow = NULL;
int g_cur_orphan_idx = -1;
int g_park_requested = 0; long g_work = 0; double g_yield_floor = -1e300; int g_made_progress = 0;
double g_quantum_start = 0.0; unsigned g_quantum_sample = 0; long g_switches = 0;
double g_max_parked = -1e300; int g_resume_mode = 0;
int g_initial_boot = 0;   /* the page's FIRST boot runs as a forking boot flow (all-false PRIMARY -> the canonical logged-out g_boot_delta; each opaque gate forks a TRUE sibling boot flow) — the ONE boot system, replacing the deleted monolithic non-forking pass + its separate reg_add_boot re-run. */
JSValue g_cur_fn = JS_UNDEFINED; signed char *g_dec = NULL; int g_dec_cap = 0, g_dec_n = 0, g_c = 0;

static int quantum_expired(void) { return g_made_progress && (emscripten_get_now() - g_quantum_start) > QUANTUM_MS; }

static int reg_readd(JSContext *ctx, Flow f)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        CHECK(nr, "reg_oom: flow-registry realloc failed — OOM is a physical floor, never continue with a dropped flow");
        g_reg = nr; g_reg_cap = nc;
    }
    g_reg[g_reg_n++] = f; return 1;
}

int seed_orphans(JSContext *ctx)
{
    int cnt = JS_CountOrphans(ctx);   /* EXACT uncalled-function count -> size the staging buffer to it (no fixed cap) */
    if (cnt == 0) return 0;
    JSValue *buf = (JSValue *)malloc((size_t)cnt * sizeof(JSValue));
    CHECK(buf, "orphan_oom: orphan-collection staging alloc failed — OOM is a physical floor, never truncate the frontier");
    int n = JS_CollectOrphans(ctx, buf, cnt), seeded = 0;
    for (int i = 0; i < n; i++) {
        /* A <script>'s 'load' handler is a LOAD-GATED continuation: skip it until its chunk provides (else it
           drives on the pre-load baseline where the chunk's globals are absent -> a phantom endpoint). Released
           by script_load_release on chunk-provide, then this continuous collection picks it up. */
        if (script_load_gated(JS_VALUE_GET_PTR(buf[i]))) { JS_FreeValue(ctx, buf[i]); continue; }
        int dup = 0;
        for (int j = 0; j < g_reg_n; j++)
            if (JS_VALUE_GET_PTR(g_reg[j].handle) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        /* also skip one already recorded in the stable buffer (queued/parked, not yet run) */
        if (!dup) for (int j = 0; j < g_orphan_n; j++)
            if (JS_VALUE_GET_PTR(g_orphan_buf[j]) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        if (dup) { JS_FreeValue(ctx, buf[i]); continue; }
        if (g_orphan_n >= g_orphan_cap) {   /* grow the locator set — every orphan gets a stable index (parkable/resumable), no 4096 cutoff */
            int nc = g_orphan_cap ? g_orphan_cap * 2 : 256;
            JSValue *nb = (JSValue *)realloc(g_orphan_buf, (size_t)nc * sizeof(JSValue));
            CHECK(nb, "orphan_oom: locator-buffer realloc failed — OOM is a physical floor, never drop a locator");
            g_orphan_buf = nb; g_orphan_cap = nc;
        }
        int idx = g_orphan_n; g_orphan_buf[g_orphan_n++] = JS_DupValue(ctx, buf[i]);   /* buffer owns a ref (stable locator) */
        if (g_resume_mode) { JS_FreeValue(ctx, buf[i]); continue; }   /* resume: build locators only; recipes are seeded explicitly */
        reg_add(ctx, buf[i], 1.0, NULL, 0)->orphan_idx = idx;
        seeded++;
    }
    free(buf);
    return seeded;   /* count surfaces in @RESULT._orphans (via g_orphan_n) — no dead @ORPHANS line */
}

/* Per-flow isolation is the engine COW (JS_CowSetActive/heap_cow_revert): shared-state writes (var-refs =
   globals/lexicals/closures; baseline-object property mutations) are captured while a flow explores, and
   reverted to the post-boot BASELINE before the next STARTER runs — so an independent flow never sees
   another's writes, yet a flow sees its OWN writes within its run. */

/* Thin Flow->scalars adapter over the ONE WFQ policy (solver/wfq.c); the order-key formula lives THERE
   (auditable + isolation-testable), this only projects a Flow's accounting into it. */
static double flow_weight(const Flow *f)
{
    return wfq_weight(f->val, f->visits, f->cpu);
}

/* The value-driven yield decision, called by the engine at each loop back-edge of the running TOP flow
   frame. Ticks CPU (accounting, NOT a cap — the flow is resumable, never truncated), then yields iff a
   PARKED flow now outranks the running one. The JS heap AND Lexbor DOM are per-flow COW deltas that swap on
   context-switch (heap_cow_buf_take/Load + dom delta), so a flow is preemptible mid-write to EITHER — no writer
   runs to completion, no empty-delta guard. */
static int wfq_yield(void)
{
    if (!g_cur_flow) return 0;
    g_cur_flow->cpu += 1.0;
    /* Both the JS heap AND the Lexbor DOM are per-flow COW deltas that swap on context-switch, so a flow is
       preemptible mid-write to either — no writer runs to completion. */
    if (g_max_parked > flow_weight(g_cur_flow)) return 1;   /* VALUE yield: a parked flow outranks the running one (g_max_parked maintained at dispatch + reg_add) */
    /* COOPERATIVE-QUANTUM yield (orthogonal, thread-sharing): held the worker thread a full wall-clock slice
       -> suspend + return to host, resume identical frontier. Clock sampled every QUANTUM_SAMPLE back-edges. */
    if (++g_quantum_sample >= QUANTUM_SAMPLE) { g_quantum_sample = 0; return quantum_expired(); }
    return 0;
}

/* Emit the remaining frontier as compact REPLAY recipes (orphan_idx + decision-vector) and clear it.
   A parked flow is reconstructed next session by re-running boot + replaying its decisions — never by
   serializing a live continuation. The host persists these lines to IDB; @PARK/@PARKED are read back. */
void park_frontier(JSContext *ctx)
{
    int parked = 0, unrecipe = 0;
    for (int i = 0; i < g_reg_n; i++) {
        Flow *f = &g_reg[i];
        /* A flow is COLD-PARKABLE iff it self-relocates by FUNCTION SOURCE on the next session. TWO kinds:
           (1) an ORPHAN-derived flow — located among the re-collected orphans by JS_OrphanHash;
           (2) an ASYNC-call flow — its OWN async function re-fires when phase-1 boot re-runs, so the live flow
               is re-created and phase-2 resume re-attaches this decvec by hash (async marker 'a'). The
               unification (await + branch decisions in ONE g_dec) is exactly what makes an async decvec a
               replayable recipe: re-driving the re-fired async call with it reconstructs its await/branch path.
           @S candidate flows are transient (never parked); a boot-triggered async flow persists, a
           handler-triggered one re-explores when its handler re-drives (still never lost, just not skipped-to). */
        uint32_t oh = 0; int async_rec = 0;
        if (!f->candidate && f->orphan_idx >= 0 && f->orphan_idx < g_orphan_n) {
            oh = JS_OrphanHash(ctx, g_orphan_buf[f->orphan_idx]);   /* stable identity, not the positional index */
        } else if (!f->candidate && f->is_async && JS_IsFunction(ctx, f->handle)) {
            oh = JS_OrphanHash(ctx, f->handle);   /* async flow: locate by its OWN function's source */
            async_rec = 1;
        } else if (!f->candidate && JS_IsFunction(ctx, f->handle)) {
            /* CONTINUATION flow (a deferred timer/promise-reaction/forEach-element callback registered with
               orphan_idx=-1): it still has a FUNCTION HANDLE, so locate it by SOURCE next session and drive it
               with its decision vector — never a silent drop. HONEST LIMIT: its inherited COW delta (the handler
               writes JS_CowBufSnapshot captured) is NOT in the recipe, so a cold resume RE-DERIVES the flow from
               scratch (re-run boot + replay decisions) rather than restoring the inherited state — sound (the
               decisions replay over a re-established baseline), only the hot inherited delta is not persisted. */
            oh = JS_OrphanHash(ctx, f->handle);
        }
        if (oh) {
            /* recipe = "[a]hash,decbits,valcenti,visits" in a DYNAMIC buffer. The decision vector is UNBOUNDED:
               a fixed buffer would TRUNCATE a deep flow's decisions -> a wrong (shorter) replay path, a
               hidden depth bound (the cardinal violation). valcenti = accumulated value*100 (the flow's
               emitted-value score) and visits = times scheduled: TOGETHER they are the cold-tier
               frontierWeight estimator (emit-per-VISIT, the guarded rate CLAUDE.md ranks rehydration by),
               and on reseed they restore the UCB explore term so a heavily-explored low-yield recipe
               correctly defers to unproven ones instead of resuming as if brand-new. A leading 'a' marks an
               async recipe (re-attach to a re-fired async call), else an orphan recipe (drive the orphan). */
            size_t cap = 17 + (size_t)(f->dec_n > 0 ? f->dec_n : 0) + 40;
            char *rec = (char *)malloc(cap);
            if (rec) {
                int o = 0;
                if (async_rec) rec[o++] = 'a';
                o += snprintf(rec + o, cap - (size_t)o, "%u,", oh);
                for (int j = 0; j < f->dec_n; j++) rec[o++] = f->dec[j] ? '1' : '0';
                snprintf(rec + o, cap - (size_t)o, ",%d,%d", (int)(f->val * 100.0), f->visits);
                arr_push_str(ctx, g_park, rec);   /* "[a]hash,decbits,valcenti,visits" replay recipe -> @RESULT._park */
                free(rec);
                parked++;
            }
        } else if (!f->candidate && !f->vtarget) {
            /* No source recipe (a session flow drives __driveSession, not a locatable page function; it re-explores
               when its handlers re-fire next session — never SKIPPED-to, but not resumed either). Count it so
               @PARKED is HONEST rather than claiming a clean park while this tail is re-derived from scratch. */
            unrecipe++;
        }
        if (f->fs) JS_FlowFree(g_rt, f->fs);
        if (f->cow) heap_cow_buf_free(ctx, f->cow, f->cow_n);   /* free the parked flow's stashed heap COW delta */
        heap_cow_base_free(ctx, f->cow_base);   /* drop this flow's reference to the shared base chain */
        if (f->dom) dom_buf_free(f->dom, f->dom_n);   /* and its DOM delta */
        dom_base_free(f->dom_base);                   /* drop this flow's reference to the shared DOM base chain */
        JS_FreeValue(ctx, f->handle);
        free(f->dec); free(f->candidate); free(f->vtarget); free(f->drive_src); flow_free_async_refs(ctx, f);    }
    g_reg_n = 0;
    /* HONEST observability: `parked` = flows with a source-identity replay recipe (resumed next session);
       `unrecipe` = the recipe-less tail (session flows) that re-explore from scratch when their handlers re-fire.
       Reporting both is the truth — the old comment claimed "never a silent drop" while dropping this tail. */
    printf("@PARKED %d %d\n", parked, unrecipe); fflush(stdout);
}

/* A BOOT FLOW is pending (reply-triggered forking re-run or a boot-fork sibling) — a RAM-pressure park must
   not drop it: it delivers an already-fetched reply's gated surface and has no replay recipe if dropped. */
static int reg_has_boot(void) {
    for (int i = 0; i < g_reg_n; i++) if (g_reg[i].is_boot) return 1;
    return 0;
}
/* The ONE scheduler loop: pick the highest-WEIGHT flow (NON-FIFO), run it as a preemptible heap-frame
   flow (per-opcode yield, no slice count), re-queue it if it suspended (interleave), repeat. BFS by value-of-information: shallow
   high-emit flows finish ahead of the deep residue, which is starved to ~0 CPU (resumable). */
void scheduler_run(JSContext *ctx)
{
    g_quantum_start = emscripten_get_now();   /* fresh cooperative slice per scheduler_run (per qjs_step) */
    for (;;) {
        seed_orphans(ctx);          /* CONTINUOUS: pick up functions a prior flow defined dynamically (chunks) */
        if (g_reg_n == 0) break;
        /* COOPERATIVE-QUANTUM RETURN (§NO BOUNDS: a thread-yield, NOT a cap): held the worker thread a full
           wall-clock slice -> return to qjs_step (which reports HOT work remains) so the host loop pumps the
           worker's message queue / interleaves other engines / streams findings, then re-enters and RESUMES the
           byte-identical frontier. Distinct from the value yield-floor (which flow) and RAM park (evict tail):
           this is purely WHEN to let the one thread breathe. wfq_yield already suspended the running flow on the
           same signal, so here we just stop re-dispatching and hand control back. */
        if (quantum_expired()) break;
        /* COLD-TIER PARK (cross-page/cross-session fairness) = RESOURCE PRESSURE on ALL work, not just
           starters: on RAM PRESSURE (host-driven, RESOURCE-based — NEVER a dispatch count; a per-N slice is
           the banned step-cap the value yield-floor below already avoids), PARK the rest of the frontier as replay
           recipes (orphan_idx + decision-vector) to the IDB cold tier and stop — resumable next burst/session.
           The host sets g_park_requested when resident RAM crosses the working-set floor. With RAM headroom (a
           lone engine, a small page) it NEVER fires, so the page runs to COMPLETION in one visit — findings are
           never lost to a clock. Not a bound: parked flows re-drive by re-running boot + decisions, and BFS has
           already extracted the productive breadth, so pressure only ever touches the least-valuable starved tail.
           EXCEPTION — never park while a BOOT FLOW is pending: a reply-triggered forking boot (reg_add_boot on
           a NEW reply) is the DELIVERY of an already-fetched reply — the moat's headline gated/logged-in surface
           — and it carries NO orphan_idx, so park_frontier would DROP it with no recipe (permanently lost, not
           deferred). The WFQ, not a clock, orders the boot-fork tree; it drains (finite reply-gated branches). */
        if (g_park_requested && !reg_has_boot()) { park_frontier(ctx); break; }
        int best = -1;
        for (int i = 0; i < g_reg_n; i++) {
            /* an async-call flow still PARKED on a pending await is not runnable: skip it until its promise
               settles (a runnable flow — the callee it awaits — completes + resolves it). Its RESOLVED value
               is delivered on the next dispatch (JS_FlowResumeInject). */
            if (!JS_IsUndefined(g_reg[i].await_promise) && JS_PromiseState(ctx, g_reg[i].await_promise) == JS_PROMISE_PENDING) continue;
            if (best < 0 || flow_weight(&g_reg[i]) > flow_weight(&g_reg[best])) best = i;
        }
        if (best < 0) break;   /* every remaining flow is parked on a pending await (a genuine cycle) -> nothing runnable */
        /* HOST-LEVEL VALUE YIELD (cross-document fairness): yield HOT to the host WFQ the moment this engine's
           BEST flow no longer outranks the runner-up ENGINE (weight set by the host in g_yield_floor) — the
           frontier stays in g_reg, the host re-ranks all live engines + the top cold recipe and resumes the
           winner. VALUE-driven, NOT a dispatch count (a per-N slice is a banned step-cap). g_made_progress
           guards against a zero-work ping-pong at the boundary. */
        if (g_made_progress && flow_weight(&g_reg[best]) <= g_yield_floor) break;
        Flow f = g_reg[best];                       /* f is a STABLE COPY: reg_add during the run may realloc g_reg */
        g_reg_n--; g_reg[best] = g_reg[g_reg_n];    /* swap-remove */
        /* REDUNDANT @S candidate: another candidate already broke out this sink (in g_verified) -> skip it,
           saving a full boot-replay/bundle re-run. Not a bound: the sink IS solved; the work is duplicate. */
        if (f.vtarget && solve_is_verified(ctx, f.vtarget)) { JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); free(f.drive_src); continue; }
        f.visits++;
        g_work++;                                   /* one flow got CPU (starter OR resume) — diagnostic tally */
        g_made_progress = 1;                        /* dispatched a flow this visit (progress guard, not a cap) */
        g_cur_orphan_idx = f.orphan_idx;            /* siblings forked during this flow inherit its locator */
        g_running = 1; g_cur_val = f.val; g_cur_flow = &f;
        /* recompute g_max_parked over the PARKED flows (g_reg now excludes the running flow, swap-removed above);
           reg_add keeps it current as this flow forks. O(N) once per dispatch (the pick already scanned O(N)),
           so wfq_yield stays O(1) per opcode. */
        g_max_parked = -1e300;
        for (int i = 0; i < g_reg_n; i++) { double w = flow_weight(&g_reg[i]); if (w > g_max_parked) g_max_parked = w; }
        g_candidate = f.candidate;   /* @S replay flow: source getters return this concrete candidate (else NULL=opaque) */
        g_in_session = f.session || f.sess_ctx;   /* session RUN-MODE, or an inherited session sink-CONTEXT (a reaction spawned mid-session): either way a sink here enqueues a candidate SESSION */

        if (f.session) {
            /* ATTACKER SESSION as a PREEMPTIBLE FLOW: __driveSession (bytecode) fires all handlers in seed
               order over ONE accumulating COW delta (cross-handler shared state). Run as a heap-frame flow so
               it YIELDS per-opcode like any other — run-to-completion is GONE, candidate sessions included: a
               candidate's boot-undo now lives IN the flow delta (heap_cow_seed_boot_inverse), so a suspend's
               heap_cow_unapply restores the post-boot baseline with no host-side bracket. On suspend the
               accumulated delta unapplies/reapplies exactly like a sync flow's, so handler A's tainted write
               still reaches B after an interleave. */
            g_cur_fn = JS_UNDEFINED;
            int sess_starter = (f.fs == NULL);
            if (sess_starter) {
                g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);                 /* replay this session's decisions; new opaque branches fork siblings */
                for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
                g_c = 0; cons_reset();
                if (f.candidate) boot_replay_candidate(ctx);
                JSValue gg = JS_GetGlobalObject(ctx);
                JSValue ds = JS_GetPropertyStr(ctx, gg, "__driveSession");
                f.fs = JS_FlowNew(ctx, ds, JS_UNDEFINED, 0, NULL);        /* preemptible frame for the self-hosted session loop */
                JS_FreeValue(ctx, ds); JS_FreeValue(ctx, gg);
                replay_handlers_clear(ctx);                              /* candidate boot-replay's transient listeners done (session fires g_handlers) */
                if (!f.fs) why_add(ctx, "session", "__driveSession not flow-able (alloc failure) — FATAL");   /* @WHY = crash: __driveSession is guaranteed bytecode, so NULL is a real (OOM) error to surface, never skip */
            } else {                                                     /* RESUME: reload decisions + swap in this session's OWN accumulated delta */
                g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
                for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
                g_c = f.saved_c; cons_reset();
                heap_cow_base_load(f.cow_base); f.cow_base = NULL; heap_cow_buf_load(f.cow, f.cow_n, f.cow_cap); heap_cow_apply(ctx); f.cow = NULL; f.cow_n = f.cow_cap = 0;
                dom_base_load(f.dom_base); f.dom_base = NULL; dom_buf_load(f.dom, f.dom_n, f.dom_cap); dom_apply(); f.dom = NULL; f.dom_n = f.dom_cap = 0;
            }
            JS_SetFlowYieldHook(wfq_yield);
            JSValue out = JS_UNDEFINED;
            int st = JS_FlowResume(ctx, f.fs, &out);
            JS_SetFlowYieldHook(NULL);
            JSContext *c1; int jr; while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
            if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
            g_running = 0; g_cur_fn = JS_UNDEFINED;
            if (st == 1) {                                               /* SUSPENDED: stash accumulated delta + re-queue (interleave with other flows) */
                g_switches++;
                heap_cow_unapply(ctx); f.cow = heap_cow_buf_take(&f.cow_n, &f.cow_cap); f.cow_base = heap_cow_base_take();
                dom_unapply(); f.dom = dom_buf_take(&f.dom_n, &f.dom_cap); f.dom_base = dom_base_take();
                f.saved_c = g_c; f.val = g_cur_val;
                if (g_dec_n > f.dec_n) {
                    signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1));
                    if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; }
                } else {
                    for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i];
                }
                f.dec_n = g_dec_n;
                g_cur_flow = NULL;
                reg_readd(ctx, f);
            } else {                                                     /* COMPLETED: revert this session's delta (boot-inverse included -> post-boot baseline) */
                heap_cow_revert(ctx); { int cn, cc; void *cb = heap_cow_buf_take(&cn, &cc); heap_cow_buf_free(ctx, cb, cn); }
                dom_revert(); { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free(db, dn); }
                if (JS_IsException(out)) js_std_dump_error(ctx);
                JS_FreeValue(ctx, out);
                g_cur_flow = NULL;
                JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); free(f.drive_src);            }
            g_candidate = NULL;
            continue;
        }

        if (f.is_boot) {
            /* BOOT FLOW: re-run boot from the PRISTINE pre-boot baseline as a FORKING starter. Cached replies
               resolve synchronously (make_response injects __body), so a reply-consuming continuation runs
               IN-LINE and its gated branches FORK with the concolic example (unreachable via the non-forking
               promise-resume). The boot-undo lives IN the flow delta (heap_cow_seed_boot_inverse — the SAME primitive
               candidate flows use), so heap_cow_revert restores the post-boot baseline with NO host-side bracket:
               one uniform COW-delta mechanism, and g_boot_delta stays the canonical baseline (only READ). */
            g_cur_fn = JS_UNDEFINED;
            g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
            for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
            g_c = 0; cons_reset();
            JS_SetFlowYieldHook(NULL);
            if (g_boot_delta) heap_cow_seed_boot_inverse(ctx, g_boot_delta, g_boot_delta_n);   /* seed flow delta with boot-inverse; heap -> pre-boot (globals incl let/const deleted), RECORDED */
            g_in_boot_flow = 1;
            boot_scripts_run(ctx);                                        /* re-run boot, FORKING; cached replies resolve sync */
            { JSContext *cb; int jr; while ((jr = JS_ExecutePendingJob(g_rt, &cb)) > 0) { } if (jr < 0) js_std_dump_error(cb ? cb : ctx); }   /* drain continuations (they fork too) */
            g_in_boot_flow = 0;
            heap_cow_revert(ctx); { int cn, cc; void *cb = heap_cow_buf_take(&cn, &cc); heap_cow_buf_free(ctx, cb, cn); }   /* revert boot-inverse + re-run writes -> post-boot baseline */
            dom_revert(); { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free(db, dn); }
            g_running = 0; g_cur_fn = JS_UNDEFINED; g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); free(f.drive_src);            continue;
        }

        /* SYNC flow: load its per-flow scheduler state (decision vector + branch cursor). __branch
           consumes/extends g_dec + forks siblings; force-invoke with OPAQUE this+args (external input
           the tool must not concretely decide) so gates fork and computed URLs are shaped. */
        g_cur_fn = f.handle;
        g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
        for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
        g_c = f.saved_c;
        cons_reset();   /* rebuild the value domain as this flow re-sees its branch conditions (starter = full; resume = from saved_c) */

        int is_starter = (f.fs == NULL);
        if (is_starter) {                           /* STARTER: fresh heap frame + empty heap/DOM deltas (both left NULL by the previous flow's exit) */
            /* @S CROSS-FLOW: a candidate flow re-runs boot with the concrete candidate pinned, so shared
               state a handler reads (window.x = location.hash set at boot) holds the candidate, not the
               baseline opaque. boot_replay_candidate SEEDS this flow's delta with the boot-inverse (heap ->
               pre-boot so a guarded init re-fires) then replays boot under the candidate — all in the flow's
               OWN delta, so a suspend/revert restores the post-boot baseline with no host-side bracket. */
            if (f.candidate) boot_replay_candidate(ctx);
            /* CONTINUATION starter: a callback deferred from a handler carries an INHERITED delta — a shared
               immutable base SEGMENT (heap_cow_fork/dom_cow_fork at defer time) for BOTH heap and DOM — so it sees
               the handler's writes. Load+apply it. Mutually exclusive with a candidate flow (own boot-inverse). */
            else if (f.cow || f.cow_base || f.dom || f.dom_base) {
                if (f.cow || f.cow_base) { heap_cow_base_load(f.cow_base); f.cow_base = NULL; heap_cow_buf_load(f.cow, f.cow_n, f.cow_cap); heap_cow_apply(ctx); f.cow = NULL; f.cow_n = f.cow_cap = 0; }
                if (f.dom || f.dom_base) { dom_base_load(f.dom_base); f.dom_base = NULL; dom_buf_load(f.dom, f.dom_n, f.dom_cap); dom_apply(); f.dom = NULL; f.dom_n = f.dom_cap = 0; }
            }
            /* CLOSURE cross-flow: for a candidate flow, drive the handler boot_replay RE-CREATED (candidate
               closure), located by source identity — else the ORIGINAL f.handle (baseline closure) is driven
               and the candidate never reaches a closure-captured source. Non-candidate flows drive f.handle. */
            JSValue drive = f.handle, resolved = JS_UNDEFINED;
            if (f.candidate) { resolved = resolve_replayed_handler(ctx, f.handle); if (!JS_IsUndefined(resolved)) drive = resolved; }
            /* Each orphan ARGUMENT is DISTINCT external input, so it gets its OWN source identity ({arg0}..{arg7})
               — NOT the shared source-less g_concolic. Sharing one source-less value across args aliased them in the
               per-flow constraint tracker: `function(a,b){ if(a=='x' && b=='y') sink }` recorded ==x and ==y on the
               SAME (empty) source, a false contradiction that PRUNED the sink arm. Distinct sources let each arg's
               gate tokens accumulate + reverse independently (the per-flow constraints a PoC is built from). Owned
               -> freed after JS_FlowNew dups them into the frame. */
            JSValue oargs[8]; for (int i = 0; i < 8; i++) { char s[16]; snprintf(s, sizeof s, "{arg%d}", i); oargs[i] = JS_NewConcolicSourced(ctx, s, s); }
            /* OPAQUE-COLLECTION element: an items.forEach(cb) callback's element carries the collection's
               provenance ({reply}), not a bare {arg0} — so f.key keeps the reply taint. */
            JSValue elem = JS_UNDEFINED, ev = JS_UNDEFINED;
            if (f.drive_src) { elem = JS_NewConcolicSourced(ctx, f.drive_src, f.drive_src); JS_FreeValue(ctx, oargs[0]); oargs[0] = elem; }
            /* A 'message' listener's first arg is a MessageEvent whose .data is attacker-controlled
               (postMessage): drive it with the {pm} source-tagged event so a sink reaching e.data reports
               {pm} and the PoC assembler builds a postMessage-delivered PoC. (g_msg_event is BORROWED.) */
            if (!JS_IsUndefined(g_msg_event) && is_msg_handler(drive)) { JS_FreeValue(ctx, oargs[0]); oargs[0] = g_msg_event; }
            else if (!f.drive_src && is_handler(ctx, drive)) { ev = js_event_ctor(ctx, JS_UNDEFINED, 0, NULL); JS_FreeValue(ctx, oargs[0]); oargs[0] = ev; }   /* a non-message handler gets a REAL Event (type/target/preventDefault…), not opaque -> no phantom shape-check arm */
            /* Drive an orphan METHOD with its REAL receiver instance if one exists (this.field -> concrete
               boot value, a real example) else a SOURCED {this} — external input, NOT the shared source-less
               g_concolic, so a `this=='x'` gate token is attributed + reversible (source-at-creation, like args). */
            JSValue recv = JS_FindReceiver(ctx, drive);
            JSValue this_own = JS_IsUndefined(recv) ? JS_NewConcolicSourced(ctx, "{this}", "{this}") : JS_UNDEFINED;
            JSValue this_val = JS_IsUndefined(recv) ? this_own : recv;
            f.fs = JS_FlowNew(ctx, drive, this_val, 8, oargs);   /* async funcs included now — JS_FlowNew accepts them */
            JS_FreeValue(ctx, this_own);   /* JS_FlowNew dup'd it; UNDEFINED (a real receiver) frees to noop */
            JS_FreeValue(ctx, recv);
            for (int i = 0; i < 8; i++) if (JS_VALUE_GET_PTR(oargs[i]) != JS_VALUE_GET_PTR(g_msg_event)) JS_FreeValue(ctx, oargs[i]);   /* free the owned {argN}/elem/ev; skip the borrowed shared g_msg_event */
            JS_FreeValue(ctx, resolved); replay_handlers_clear(ctx);   /* JS_FlowNew dup'd the drive handle; drop our resolved ref + transient replay handlers */
        }
        /* RESUME: swap in the parked flow's OWN heap COW delta (unapplied while it slept) so it sees its own
           shared-state writes again, not another flow's. Starters begin with an empty delta (globals NULL). */
        if (!is_starter) {
            heap_cow_base_load(f.cow_base); f.cow_base = NULL; heap_cow_buf_load(f.cow, f.cow_n, f.cow_cap); heap_cow_apply(ctx); f.cow = NULL; f.cow_n = f.cow_cap = 0;
            dom_base_load(f.dom_base); f.dom_base = NULL; dom_buf_load(f.dom, f.dom_n, f.dom_cap); dom_apply(); f.dom = NULL; f.dom_n = f.dom_cap = 0;
        }

        /* EVERY sync flow is preemptible mid heap-write (per-flow COW delta), CANDIDATE flows included: the
           candidate boot-undo lives in the flow delta (heap_cow_seed_boot_inverse), so a suspend's heap_cow_unapply
           restores the post-boot baseline — no host-side bracket to straddle. */
        JS_SetFlowYieldHook(wfq_yield);
        JSValue out = JS_UNDEFINED;
        int st;
        if (!JS_IsUndefined(f.await_promise)) {
            /* Resume an async-call flow whose awaited promise has now SETTLED (the pick loop only selects a
               runnable/settled flow): inject the settled value (or throw the rejection) and drive. */
            JSValue ap = f.await_promise; f.await_promise = JS_UNDEFINED;
            int rej = (JS_PromiseState(ctx, ap) == JS_PROMISE_REJECTED);
            JSValue av = JS_PromiseResult(ctx, ap);
            JS_FreeValue(ctx, ap);
            st = JS_FlowResumeInject(ctx, f.fs, av, rej, &out);
            JS_FreeValue(ctx, av);
        } else {
            st = JS_FlowResume(ctx, f.fs, &out);
        }
        JS_SetFlowYieldHook(NULL);
        JSContext *c1; int jr;
        while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
        if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
        g_running = 0; g_cur_fn = JS_UNDEFINED;

        if (st == 1) {
            /* SUSPENDED: UNAPPLY this flow's heap writes (baseline restored for the next flow) and STASH its
               delta buffer; re-queue. On resume it re-applies. Interleaving of heap-writers is now sound. */
            g_switches++;   /* one flow was preempted mid-run -> a real context switch (interleave), MEASURED */
            heap_cow_unapply(ctx);
            f.cow = heap_cow_buf_take(&f.cow_n, &f.cow_cap); f.cow_base = heap_cow_base_take();
            dom_unapply();
            f.dom = dom_buf_take(&f.dom_n, &f.dom_cap); f.dom_base = dom_base_take();
            f.saved_c = g_c;
            f.val = g_cur_val;
            if (g_dec_n > f.dec_n) {                /* grew (new branch decisions taken this flow-run) */
                signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1));
                if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; }
            } else {
                for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i];
            }
            f.dec_n = g_dec_n;
            g_cur_flow = NULL;
            reg_readd(ctx, f);
        } else if (st == 2) {
            /* PENDING await (an async-call flow awaited a still-unsettled promise `out`): STASH its delta like a
               suspend + record the promise; the pick loop deprioritizes it until settled, then it resumes via
               JS_FlowResumeInject. (An unawaited or already-resolved await never parks — settled inline.) */
            g_switches++;
            heap_cow_unapply(ctx); f.cow = heap_cow_buf_take(&f.cow_n, &f.cow_cap); f.cow_base = heap_cow_base_take();
            dom_unapply(); f.dom = dom_buf_take(&f.dom_n, &f.dom_cap); f.dom_base = dom_base_take();
            f.saved_c = g_c; f.val = g_cur_val;
            if (g_dec_n > f.dec_n) { signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1)); if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; } }
            else { for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i]; }
            f.dec_n = g_dec_n;
            f.await_promise = out;   /* the pending promise (ownership transferred; out NOT freed here) */
            g_cur_flow = NULL;
            reg_readd(ctx, f);
        } else {
            /* COMPLETED/error: discard this flow's heap writes (restore baseline) + free its delta buffer. */
            heap_cow_revert(ctx);
            { int cn, cc; void *cb = heap_cow_buf_take(&cn, &cc); heap_cow_buf_free(ctx, cb, cn); }
            dom_revert();
            { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free(db, dn); }
            if (!JS_IsUndefined(f.aresolve)) {
                /* async-call flow: SETTLE its result promise so an awaiting caller-flow resumes — REJECT on a
                   thrown completion (st==3, out=exception value) so the awaiter re-throws (try/catch/.catch runs),
                   else RESOLVE with the return value (st==0). */
                JSValue fn = (st == 3) ? f.areject : f.aresolve;
                if (!JS_IsUndefined(fn)) { JSValue rr = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&out); if (JS_IsException(rr)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); } JS_FreeValue(ctx, rr); }
            } else if (JS_IsException(out)) {
                js_std_dump_error(ctx);   /* a non-async (sync orphan/boot) flow threw on opaque input — the exploration surface */
            }
            JS_FreeValue(ctx, out);
            g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); free(f.drive_src);   /* candidate owned by a completed replay flow */
            flow_free_async_refs(ctx, &f);
        }
        g_candidate = NULL;   /* clear the replay candidate; a suspended flow re-sets it from f.candidate on resume */
    }
    JS_SetFlowYieldHook(NULL);
}

/* This engine's value-of-information = its best flow's weight (0 if idle/done). Keeps flow_weight + g_reg
   private to the scheduler; the host Level-1 ranker (qjs_top_weight) reads it to order live engines. */
double scheduler_top_weight(void) {
    double best = 0; int seen = 0;
    for (int i = 0; i < g_reg_n; i++) { double w = flow_weight(&g_reg[i]); if (!seen || w > best) { best = w; seen = 1; } }
    return seen ? best : 0.0;
}

/* ── flow-creation edges (moved from the entry file) ─────────────────────────────── */
int g_dec_ensure(int n) {              /* grow g_dec to hold >= n decisions */
    if (n <= g_dec_cap) return 1;
    int nc = g_dec_cap ? g_dec_cap * 2 : 64; while (nc < n) nc *= 2;
    signed char *nd = (signed char *)realloc(g_dec, (size_t)nc);
    /* OOM here is the PHYSICAL floor, always fatal (CHECK, not a swallowed 0): a flow whose decision can't be
       recorded runs with an unrecorded arm -> its decision vector diverges from its execution and replay is
       corrupt; the unchecked callers (dispatch's g_dec_ensure(g_dec_n)) would instead overflow g_dec on the
       following writes. RAM PRESSURE is handled earlier by park-to-disk; this is the hard realloc wall. */
    CHECK(nd, "dec_oom: decision-vector realloc failed — the physical floor; a flow whose decision can't be recorded corrupts its replay");
    g_dec = nd; g_dec_cap = nc; return 1;
}

Flow *reg_add(JSContext *ctx, JSValue handle, double val, signed char *dec, int dec_n)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        CHECK(nr, "reg_oom: flow-registry realloc failed — OOM is a physical floor, never continue with a dropped flow");
        g_reg = nr; g_reg_cap = nc;
    }
    Flow *f = &g_reg[g_reg_n];
    f->handle = handle; f->val = val;
    f->dec = dec; f->dec_n = dec_n;
    f->fs = NULL; f->saved_c = 0; f->cpu = 0; f->visits = 0;
    f->cow = NULL; f->cow_n = 0; f->cow_cap = 0; f->cow_base = NULL;
    f->dom = NULL; f->dom_n = 0; f->dom_cap = 0; f->dom_base = NULL;
    f->orphan_idx = -1; f->candidate = NULL; f->session = 0; f->vtarget = NULL;
    f->drive_src = NULL;
    f->is_boot = 0;
    f->aresolve = JS_UNDEFINED; f->areject = JS_UNDEFINED; f->await_promise = JS_UNDEFINED;
    f->rthis = JS_UNDEFINED; f->rargs = NULL; f->rargc = 0; f->is_async = 0;
    f->sess_ctx = 0;
    g_reg_n++;
    { double w = wfq_weight(val, 0, 0); if (w > g_max_parked) g_max_parked = w; }   /* fresh flow (visits=cpu=0): same ONE policy, no duplicated formula — keeps g_max_parked current for wfq_yield */
    return f;
}

int branch_decide(JSContext *ctx, JSValueConst cond)
{
    if (g_boot_replay) return 1;                            /* boot-replay: fixed arm (re-establishing shared state for an @S candidate, no vector to replay) */
    if (!g_running || (JS_IsUndefined(g_cur_fn) && !g_in_boot_flow && !g_in_session)) return 0;   /* meaningful inside a starter flow, a boot flow, OR a session flow (all re-run without a single fn handle) */
    /* value-domain provenance of the condition: cond TRUE means `src <op> tok`; false arm holds the negation. */
    const char *src = NULL, *tok = NULL; int op = JS_ConcolicCmp(cond, &src, &tok);
    const char *jk = JS_ConcolicJKey(cond);   /* method-clean JSON field path of the compared value (for the @S envelope gate-merge) */
    int has = (op != OPCMP_NONE) && src;
    int true_op = op, false_op = opcmp_neg(op);

    if (g_c < g_dec_n) {                                    /* forced replay: take the recorded arm; RE-RECORD its constraint */
        int arm = g_dec[g_c] ? 1 : 0;
        cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? (arm ? true_op : false_op) : OPCMP_NONE, has ? jk : NULL);
        g_c++; return arm;
    }
    g_dec_ensure(g_c + 1);                                   /* only RAM/disk (the platform floor) bounds depth — g_dec_ensure CHECK-crashes at the hard wall, never fabricates an arm */

    /* A CANDIDATE session (verifying an @S breakout) takes a fixed arm for a NEW branch — it replays the
       detecting session's vector (above) then follows through with the candidate, it does not re-explore. */
    if (g_in_session && g_candidate) { cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE, has ? jk : NULL); g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++; return 1; }

    /* LOOP-BACK over an OPAQUE COLLECTION (for-of/for-in `done`, tagged "{@iterdone}"): iteration is
       UNBOUNDED-PARKABLE-PAGED, never run-to-completion in one flow. Take the EXIT arm (done=true) as the
       PRIMARY — this flow STOPS iterating (breadth first) — and PARK the CONTINUE arm (done=false) as its OWN
       sibling flow, so every additional iteration is a separate preemptible/pageable flow. A parked continue
       sibling runs at mark=1, so its per-iteration transients (Request/Promise/…) are flow_local-skipped and
       no single flow accumulates an unbounded delta. This is the ONE design replacing BOTH the drive-once
       terminating iterator (a banned bound that dropped shared-state-gated deep endpoints) and the
       loop-forever-in-one-flow cow-oom. The WFQ starves the identical-input continue tail (paged, resumable). */
    if (JS_IsConcolic(cond)) {
        const char *cs = JS_ConcolicSrcC(cond);
        if (cs && !strcmp(cs, "{@iterdone}")) {
            signed char *sib = (signed char *)malloc((size_t)(g_c + 1));
            CHECK(sib, "iterdone: cannot park the CONTINUE arm — dropping it truncates iteration (loses every deeper element/endpoint); OOM is a physical floor, never a silent drop");
            {
                for (int i = 0; i < g_c; i++) sib[i] = g_dec[i];
                sib[g_c] = 0;   /* CONTINUE (done=false): the parked per-iteration sibling does one more iteration */
                if (g_in_session) reg_add_session(ctx, sib, g_c + 1);
                else if (g_in_boot_flow) reg_add_boot(ctx, sib, g_c + 1);
                else if (g_cur_flow && g_cur_flow->is_async) spawn_async_sibling(ctx, g_cur_flow, sib, g_c + 1);
                else reg_add(ctx, JS_DupValue(ctx, g_cur_fn), g_cur_val, sib, g_c + 1)->orphan_idx = g_cur_orphan_idx;
            }
            cons_set(g_c, NULL, NULL, OPCMP_NONE, NULL);
            g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++;   /* EXIT (done=true): this flow stops iterating here */
            return 1;
        }
    }

    /* NEW decision: ask the substrate which arms the per-flow domain permits (pinned -> one arm via forced-exec
       predicate eval; unpinned -> prune only a provably-contradicted arm). The scheduler holds ZERO constraint
       logic — it only INTERPRETS the two booleans to take the one feasible arm, or fork when both are open. */
    int tf = 1, ff = 1;
    if (has) cons_arm_feasible(src, tok, true_op, false_op, g_c, &tf, &ff);
    if (has && !tf && ff) { cons_set(g_c, src, tok, false_op, jk); g_dec[g_c] = 0; g_dec_n = g_c + 1; g_c++; return 0; }   /* TRUE arm impossible */
    if (has && !ff && tf) { cons_set(g_c, src, tok, true_op, jk);  g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++; return 1; }   /* FALSE arm impossible */

    if (g_initial_boot) {
        /* INITIAL boot flow: KEEP the all-false PRIMARY (so g_boot_delta stays the logged-out baseline the whole
           frontier layers over) but FORK the TRUE arm as a sibling boot flow — one run explores boot's gates,
           replacing the monolithic (false-only, no exploration) pass AND its separate reg_add_boot re-run. */
        signed char *bsib = (signed char *)malloc((size_t)(g_c + 1));
        if (bsib) { for (int i = 0; i < g_c; i++) bsib[i] = g_dec[i]; bsib[g_c] = 1; reg_add_boot(ctx, bsib, g_c + 1); }
        cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? false_op : OPCMP_NONE, has ? jk : NULL);
        g_dec[g_c] = 0; g_dec_n = g_c + 1; g_c++;
        return 0;
    }
    signed char *sib = (signed char *)malloc((size_t)(g_c + 1));   /* both arms feasible: fork FALSE sibling, take TRUE */
    if (sib) {
        for (int i = 0; i < g_c; i++) sib[i] = g_dec[i];
        sib[g_c] = 0;
        if (g_in_session) reg_add_session(ctx, sib, g_c + 1);      /* an exploratory session forks ANOTHER session (re-fire handlers with the sibling vector) */
        else if (g_in_boot_flow) reg_add_boot(ctx, sib, g_c + 1);  /* a boot flow forks ANOTHER boot flow (re-run boot with the sibling vector) */
        else if (g_cur_flow && g_cur_flow->is_async)               /* an ASYNC flow: re-run via the recipe (func+args) so the awaits — recorded in this SAME g_dec vector — replay too (re-enters catches) */
            spawn_async_sibling(ctx, g_cur_flow, sib, g_c + 1);
        else reg_add(ctx, JS_DupValue(ctx, g_cur_fn), g_cur_val, sib, g_c + 1)->orphan_idx = g_cur_orphan_idx;   /* sibling = same function (same locator), different decisions */
    }
    cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE, has ? jk : NULL);
    g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++;
    return 1;
}

int ctx_forks(void) {
    if (g_boot_replay) return 1;                                                   /* fixed-arm replay (exit after 1) */
    if (!g_running) return 0;
    /* A running flow ALWAYS carries a fork context: g_cur_fn is a real fn (sync/async/orphan dispatch), or it
       is a boot flow (g_in_boot_flow — incl. the initial forking boot), or a session (g_in_session). The old
       "monolithic boot -> drive-once" state (running, no fn, not boot, not session) is GONE — boot is now the
       initial FORKING boot flow — so assert the invariant instead of silently returning drive-once past it. */
    DCHECK(!JS_IsUndefined(g_cur_fn) || g_in_boot_flow || g_in_session,
           "ctx_forks: running flow has no fn handle and is neither boot nor session — the deleted monolithic-boot state should be unreachable");
    return 1;
}

void flow_defer_callback(JSContext *ctx, JSValueConst cb) {
    Flow *f = reg_add(ctx, JS_DupValue(ctx, cb), g_running ? g_cur_val : 1.0, NULL, 0);
    /* CONTINUATION: a callback deferred from a running (revert-)flow inherits that flow's live PROPERTY delta,
       so it sees state the flow wrote before deferring (obj.x = tainted; setTimeout(()=>use(obj.x))) — a
       fresh-baseline flow reads it undefined. Closure-var state already rides the callback's own closure, so
       only property writes need this snapshot. During the INITIAL boot g_cur_flow is NULL (the forking boot
       flow has no reg-entry), so nothing is snapshotted here: boot's writes commit to the post-boot baseline,
       which a boot-deferred flow already sees. */
    if (g_running && g_cur_flow) {
        /* SHARE the delta, don't COPY it — the persistent-versioned-heap BRANCH: heap_cow_fork freezes the running
           flow's current heap delta into an immutable, refcounted base SEGMENT that this continuation references
           in O(1) (not the O(delta) JS_CowBufSnapshot copy it replaces). The parent continues over a fresh empty
           head; the callback sees the defer-point state (the shared base) plus its own later writes. Superior to
           the copy in two ways: O(1) instead of O(delta), and it carries the closure-var (slot) writes the copy
           dropped as pointer-fragile — so a callback reading a closure var the handler wrote before deferring
           now sees it. The parent keeps its own reference to the same segment (g_cow_base), released on its
           suspend/complete; refcount 2 balances parent + this sibling. */
        f->cow_base = heap_cow_fork(ctx); f->cow = NULL; f->cow_n = f->cow_cap = 0;
        f->dom_base = dom_cow_fork(); f->dom = NULL; f->dom_n = f->dom_cap = 0;   /* SHARE the DOM delta too (O(1) base segment), symmetric to the heap — replaces the O(delta) dom_buf_snapshot copy */
    }
}

void drive_opaque_cb(JSContext *ctx, JSValueConst cb, JSValueConst coll) {
    /* Register cb as a starter FLOW. NO seen-set: an unbounded recursion that calls `x.forEach(cb)` per level
       registers cb per level, but every level past the first drives cb with the SAME opaque args -> emits
       nothing new -> the WFQ STARVES those flows to ~0 CPU and RAM-pressure PARKS the tail to the IDB cold
       tier (unbounded-until-disk, the intended design). A dedup keyed by function identity was a BANNED
       seen-set (§NO BOUNDS: "only emitted output — never identity — proves a flow is done") masking the
       recursion as a hang; the cooperative quantum keeps the worker responsive while it starves + parks. */
    flow_defer_callback(ctx, cb);
    /* PROVENANCE: the element `cb` receives is an element OF `coll` (the reply/injected opaque the method was
       called on), so tag it with coll's shape — the starter drives arg0 as {reply} (not a bare {}), keeping
       the collection's taint through f.key etc. A non-opaque/untagged receiver leaves drive_src NULL (default
       g_concolic args). Registered flow is the last one appended by flow_defer_callback. */
    const char *cs = JS_IsConcolic(coll) ? JS_ConcolicShapeC(coll) : NULL;
    if (cs && cs[0] && strcmp(cs, "{}") != 0 && g_reg_n > 0) g_reg[g_reg_n - 1].drive_src = strdup(cs);
}

/* ── FRONTIER SEEDING (moved from main.c: it is pure frontier logic — orphan/session seeding + resume-recipe
   re-creation — using only scheduler state, so it belongs with the frontier it seeds, not the engine entry). */
/* Phase 2 (after qjs_init + the host's frontierGet by bundle-id): seed the frontier and fix the COW baseline.
   recipes NULL/"" -> a fresh visit (drive all orphans); non-empty -> RESUME by re-creating each parked flow,
   located by its function SOURCE hash (JS_OrphanHash), decisions replayed by the scheduler. */
/* Seed the frontier AFTER the initial boot has run (its functions must exist for orphan collection): fresh
   orphan/session flows, or a resume from recipes. Called from the first qjs_step, right after run_initial_boot. */
void seed_frontier(JSContext *ctx, const char *recipes)
{
    g_resume_mode = (recipes && recipes[0]) ? 1 : 0;
    seed_orphans(ctx);      /* normal: seed fresh orphan flows. resume: build g_orphan_buf locators only. */
    if (g_resume_mode) {
        /* RESUME: each recipe "hash,dec" re-creates a flow by LOCATING the orphan whose stable SOURCE identity
           (JS_OrphanHash) matches — robust to collection order/context. A recipe whose function is absent in
           THIS context simply doesn't match (skipped) — never drives the wrong one. */
        const char *p = recipes; int resumed = 0;
        while (*p) {
            int async_rec = 0;
            if (*p == 'a') { async_rec = 1; p++; }   /* async recipe: re-attach decvec to the re-fired async call, not drive an orphan */
            uint32_t want = (uint32_t)strtoul(p, NULL, 10);
            const char *comma = strchr(p, ','), *semi = strchr(p, ';');
            signed char *dec = NULL; int dec_n = 0; double rval = 1.0; int rvis = 0;
            if (comma && (!semi || comma < semi)) {
                const char *d = comma + 1, *end = semi ? semi : d + strlen(d);
                /* recipe = "hash,decbits[,valcenti[,visits]]": the decision vector ends at the SECOND comma
                   (the accumulated-value field) when present, else at the recipe end; visits follows a THIRD
                   comma. A legacy "hash,decbits" recipe (no value/visits) defaults rval=1.0/rvis=0 (unproven)
                   — backward compatible. */
                const char *comma2 = (const char *)memchr(d, ',', (size_t)(end - d));
                const char *decend = comma2 ? comma2 : end;
                dec_n = (int)(decend - d);
                if (dec_n > 0) { dec = (signed char *)malloc((size_t)dec_n); for (int i = 0; i < dec_n; i++) dec[i] = (d[i] == '1') ? 1 : 0; }
                if (comma2) {
                    double v = strtol(comma2 + 1, NULL, 10) / 100.0; if (v > 0) rval = v;   /* prior accumulated value */
                    const char *comma3 = (const char *)memchr(comma2 + 1, ',', (size_t)(end - (comma2 + 1)));
                    if (comma3) { long vs = strtol(comma3 + 1, NULL, 10); if (vs > 0 && vs < (1L << 30)) rvis = (int)vs; }   /* prior visit count (UCB explore term) */
                }
            }
            if (async_rec) {
                /* ASYNC recipe: park it in the pending map (g_arec). It attaches to the re-fired async call
                   by source hash at ONE of two consult sites of this one map — the post-loop SWEEP below (for
                   a boot-triggered call already re-created by phase-1 boot) or reg_add_async_call (for a
                   handler-triggered call that fires later in phase 3). Either way it REPLAYS its parked
                   await/branch path instead of re-forking from scratch. */
                arec_park(want, dec, dec_n, rval, rvis);   /* park the async recipe -> async_flow.c (takes dec ownership, frees on overflow) */
                if (!semi) break; p = semi + 1; continue;
            }
            int found = -1;
            for (int oi = 0; oi < g_orphan_n; oi++) { if (JS_OrphanHash(ctx, g_orphan_buf[oi]) == want) { found = oi; break; } }
            if (found >= 0) {
                { Flow *rf = reg_add(ctx, JS_DupValue(ctx, g_orphan_buf[found]), rval, dec, dec_n);   /* resume with the PRIOR accumulated value, not a flat 1.0 */
                  rf->orphan_idx = found; rf->visits = rvis; }   /* restore visits so UCB reflects prior exploration */
                resumed++;
            } else free(dec);
            if (!semi) break; p = semi + 1;
        }
        /* SWEEP: attach async recipes to the flows phase-1 boot already re-created (their calls fired before
           the map existed). Handler-triggered recipes stay pending for reg_add_async_call in phase 3. */
        for (int ri = 0; ri < g_reg_n; ri++) if (arec_attach(ctx, &g_reg[ri])) resumed++;
        printf("@RESUMED %d\n", resumed); fflush(stdout);
    }
    JS_CowSetActive(1);   /* baseline = post-boot state; capture shared-state writes during flow exploration */
    g_dom_capture = 1;    /* DOM baseline is now fixed too; capture flow DOM mutations for per-flow revert */
    /* Seed ONE attacker-SESSION flow when the page has >=2 handlers — it fires them in sequence over
       accumulating shared state, the sound way to reach cross-handler sinks (source stored by A, sunk by B). */
    /* Seed ONE exploratory session for ANY entry point (handler OR orphan): it fires them in sequence over an
       accumulating delta so a cross-handler/cross-action opaque write reaches a later gate (redux/vuex
       login-action -> thunk, or handler A -> handler B) AND a handler wired MID-drive (connectedCallback ->
       click) is fired over that delta with its producer's context. The old >=2 gate was a BOUND that truncated
       nested-handler discovery on a single-component page; the WFQ starves a session with no real cross-flow
       state, so >=1 costs ~nothing when unproductive. */
    if (!g_resume_mode && (g_handler_n + g_orphan_n) >= 1) reg_add(ctx, JS_UNDEFINED, 1.2, NULL, 0)->session = 1;
    /* BOOT-GATE EXPLORATION is INTRINSIC to the initial boot (run_initial_boot forks TRUE-arm siblings), so no
       separate boot re-run is enqueued here. A reply/chunk enqueues a delivery boot flow (qjs_provide). */
}
