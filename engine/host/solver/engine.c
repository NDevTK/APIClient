/* The dispatch loop — see engine.h. */
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"   /* the DOM half of time-travel — swapped per-flow alongside the heap COW delta */
#include "check.h"
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* FETCH-AWAIT parking: a host fetch that is not synchronously available (a live network GET) returns a PENDING
   promise and registers its resolve capability + the value it will deliver on THE RUNNING FLOW. A flow that awaits
   it suspends its async body; when the flow's scripts + microtasks are drained but a live fetch is still pending,
   flow_step resolves the flow's OWN pending fetches (the network completing) — each awaiting async body's reaction
   enqueues as a job in that flow's queue — and resumes. Per-flow (not global) so one flow's drain never resolves
   another flow's fetch (which would route the reaction to the wrong flow's COW — a leak + contamination). */
void engine_pending_fetch(JSContext *ctx, JSValueConst resolve, JSValueConst value) {
    Flow *f = flow_running();
    /* A live fetch is ALWAYS issued from a running flow — both explore and @S verify are the ONE scheduler now
       (run_scheduler), so flow_running() is set; the flow's stall drains it (flow_step). */
    DCHECK(f != NULL, "engine_pending_fetch: a live fetch issued outside a running flow");
    if (f->npend >= f->pendcap) {
        f->pendcap = f->pendcap ? f->pendcap * 2 : 8;
        f->pending = realloc(f->pending, (size_t)f->pendcap * sizeof(FlowPending));
        CHECK(f->pending, "engine: OOM growing the flow's pending-fetch list");
    }
    f->pending[f->npend].resolve = JS_DupValue(ctx, resolve);
    f->pending[f->npend].value = JS_DupValue(ctx, value);
    f->npend++;
}
/* Resolve every pending fetch this flow issued (the network completed). Returns how many were drained. */
static int flow_drain_pending(JSContext *ctx, Flow *f) {
    int n = f->npend;
    for (int i = 0; i < n; i++) {
        JSValue r = JS_Call(ctx, f->pending[i].resolve, JS_UNDEFINED, 1, (JSValueConst *)&f->pending[i].value);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, f->pending[i].resolve);
        JS_FreeValue(ctx, f->pending[i].value);
    }
    f->npend = 0;
    return n;
}

/* Snapshot-fork handoff: solver_decide stashes the sibling's hot decision + pins here at a forking branch;
   the interpreter then clones the frame and calls engine_fork_finalize, which assembles the sibling flow. */
static void *g_fork_dec = NULL, *g_fork_pins = NULL;
void engine_prepare_fork(void *dec_blob, void *pin_blob) { g_fork_dec = dec_blob; g_fork_pins = pin_blob; }

/* GENERATOR-STATE fork stash: clone_deep_flow fires gen_fork for each generator body frame it clones (during
   JS_FlowClone), BEFORE engine_fork_finalize exists to hold the sibling's delta. Append here; the finalize
   drains all onto the just-created sibling delta and resets. Filled-then-fully-drained within one fork. */
typedef struct { JSValueConst genobj; void *g0, *g1; } GenForkRec;
static GenForkRec *g_genforks = NULL; static int g_genfork_n = 0, g_genfork_cap = 0;
void engine_gen_fork(JSContext *ctx, JSValueConst genobj, void *base_gd, void *cur_gd) {
    (void)ctx;
    if (g_genfork_n >= g_genfork_cap) {
        g_genfork_cap = g_genfork_cap ? g_genfork_cap * 2 : 8;
        g_genforks = realloc(g_genforks, (size_t)g_genfork_cap * sizeof(GenForkRec));
        CHECK(g_genforks, "engine: OOM generator-fork stash");
    }
    g_genforks[g_genfork_n].genobj = genobj; g_genforks[g_genfork_n].g0 = base_gd; g_genforks[g_genfork_n].g1 = cur_gd;
    g_genfork_n++;
}

static void engine_fork_finalize(JSContext *ctx, JSValue *clone) {
    Flow *parent = flow_running();
    DCHECK(parent != NULL && g_fork_dec != NULL, "engine_fork_finalize: fork without a running flow / prepared state");
    Flow *sib = flow_add(ctx, parent->fn, NULL, 0);
    sib->started = 1;                 /* HOT: resume from the cloned frame + blobs, never a fresh re-run */
    sib->frame = clone;               /* the frame snapshot taken AT the branch */
    sib->script_i = parent->script_i; /* same position in the script sequence */
    sib->delta = cow_delta_fork(ctx, (CowDelta *)parent->delta);   /* O(1) shared base segment, then diverges */
    /* GENERATOR-STATE swaps built by clone_deep_flow for this fork: record each on the sibling's delta so the
       shared generator object resolves to the sibling's own cloned execution state while it runs. */
    for (int i = 0; i < g_genfork_n; i++)
        cow_delta_add_gendata(ctx, (CowDelta *)sib->delta, g_genforks[i].genobj, g_genforks[i].g0, g_genforks[i].g1);
    g_genfork_n = 0;
    /* DOM analog of the heap clone: freeze the parent's live DOM head into a SHARED refcounted base segment
       (refcount 2 — parent keeps a fresh empty head over it, sibling references it too), so the sibling INHERITS
       the parent's PRE-FORK document writes in O(1) instead of a copy, then each diverges on its own head. */
    sib->dom_base = dom_cow_fork();
    sib->dec_blob = g_fork_dec; g_fork_dec = NULL;
    sib->pin_blob = g_fork_pins; g_fork_pins = NULL;
    if (parent->dyn_n) {              /* inherit the lazy chunks loaded up to the branch */
        sib->dyn = malloc((size_t)parent->dyn_n * sizeof(char *)); CHECK(sib->dyn, "engine: OOM fork dyn");
        for (int i = 0; i < parent->dyn_n; i++) { sib->dyn[i] = strdup(parent->dyn[i]); CHECK(sib->dyn[i], "engine: OOM fork dyn body"); }
        sib->dyn_n = sib->dyn_cap = parent->dyn_n;
    }
}

/* The frame-agnostic REPLAY fork is DELETED: re-running a nested/deep flow from its start is BANNED (not
   byte-identical — shared state can differ between the run and the re-run). A concolic branch inside an async
   body on the tramp chain now DFAILs in the engine (see branch_arm_fork) until the sound async-frame snapshot
   is built; there is no re-run fallback to hide that gap. */

void engine_queue_script(const char *body) {
    Flow *f = flow_running();   /* the running flow owns the lazy chunk it loads */
    if (!body || !f) return;
    if (f->dyn_n >= f->dyn_cap) { f->dyn_cap = f->dyn_cap ? f->dyn_cap * 2 : 8; f->dyn = realloc(f->dyn, (size_t)f->dyn_cap * sizeof(char *)); CHECK(f->dyn, "engine: OOM dynamic-script queue"); }
    f->dyn[f->dyn_n] = strdup(body); CHECK(f->dyn[f->dyn_n], "engine: OOM dynamic-script body"); f->dyn_n++;
}

/* Preempt hook, two orthogonal yield decisions at the one per-back-edge check:
   (1) VALUE yield — suspend the running flow the MOMENT a parked flow outranks it (the WFQ, not a clock,
       decides which flow runs). The rival is recomputed only when the frontier membership changes (a fork
       adds a flow) or the running flow switches — cached by (gen, cur) so this is O(1) per back-edge, never
       an O(flows) scan per opcode.
   (2) COOPERATIVE-QUANTUM yield — a thread-sharing floor: even a top-ranked flow breathes every Q back-edges
       so the host loop can interleave / pump / snapshot. NOT a step cap: it drops/reorders no flow and the
       flow resumes byte-identically. */
static unsigned g_qtick = 0;
#define FLOW_QUANTUM 64
static unsigned g_seen_gen = 0; static Flow *g_seen_cur = NULL; static int g_outranked = 0;
static int preempt_hook(void) {
    Flow *cur = flow_running();
    if (flow_frontier_gen() != g_seen_gen || cur != g_seen_cur) {   /* (1) recompute rival only on change */
        g_seen_gen = flow_frontier_gen(); g_seen_cur = cur;
        Flow *rival = cur ? flow_best_other(cur) : NULL;
        g_outranked = (rival && cur && flow_weight(rival) > flow_weight(cur));
    }
    if (g_outranked) return 1;                        /* value yield */
    return (++g_qtick % FLOW_QUANTUM) == 0;           /* (2) quantum floor */
}

/* Advance flow `f` by up to one quantum. Returns 1 when the flow has FINISHED all its scripts + lazy chunks,
   0 when it yielded mid-execution (resume it later). Each <script>/chunk is its OWN program (JS_FlowNew) run
   in document order in the shared context, under f's COW delta (set by the caller). */
/* ASYNC-AS-FLOW job-enqueue hook (installed as JS_SetJobEnqueueHook): route a promise reaction / microtask to
   the ENQUEUING flow's own queue instead of the global list, so it runs later under that flow's live COW. */
static int engine_enqueue_job(JSContext *ctx, JSJobFunc *fn, int argc, JSValueConst *argv) {
    Flow *f = flow_running();
    if (!f) return 0;   /* enqueued outside a flow (baseline setup) -> let the fork use its default global list */
    if (f->njob >= f->jobcap) {
        f->jobcap = f->jobcap ? f->jobcap * 2 : 4;
        f->jobs = realloc(f->jobs, (size_t)f->jobcap * sizeof(FlowJob));
        CHECK(f->jobs, "engine: OOM flow job queue — a dropped reaction corrupts async exploration");
    }
    FlowJob *j = &f->jobs[f->njob++];
    j->fn = fn; j->argc = argc;
    j->argv = argc ? malloc((size_t)argc * sizeof(JSValue)) : NULL;
    if (argc) CHECK(j->argv, "engine: OOM job argv");
    for (int i = 0; i < argc; i++) j->argv[i] = JS_DupValue(ctx, argv[i]);
    return 1;   /* host owns it */
}

/* Run ONE of the flow's queued jobs (FIFO) under its currently-applied COW; free its args + result. */
static void flow_run_one_job(JSContext *ctx, Flow *f) {
    FlowJob j = f->jobs[0];
    memmove(f->jobs, f->jobs + 1, (size_t)(--f->njob) * sizeof(FlowJob));   /* FIFO pop */
    JSValue r = j.fn(ctx, j.argc, (JSValueConst *)j.argv);   /* the reaction runs in this flow's timeline */
    JS_FreeValue(ctx, r);
    for (int i = 0; i < j.argc; i++) JS_FreeValue(ctx, j.argv[i]);
    free(j.argv);
}

static int flow_step(JSContext *ctx, Flow *f, char *const *bodies, int n) {
    for (;;) {
        /* THE PARKED CONTINUATION OUTRANKS EVERYTHING ELSE THIS FLOW COULD DO — that is the park's whole
           contract: a forced preempt must be transparent to observable ordering, so the flow resumes BEFORE any
           job it has queued. Yielding after one resume keeps the scheduler in charge of fairness; the park (if
           it parks again immediately) rides the switch-out with the flow. Without this the solver host never
           pumped the slot at all: the continuation sat there until a second flow parked and asserted. */
        if (JS_ResumeParkedFlow(JS_GetRuntime(ctx))) return 0;
        if (!f->frame) {
            const char *body;
            if (f->script_i < n) body = bodies[f->script_i];
            else if (f->script_i - n < f->dyn_n) body = f->dyn[f->script_i - n];
            else if (f->njob > 0) { flow_run_one_job(ctx, f); return 0; }   /* scripts done -> drain a microtask, yield */
            else if (f->npend > 0) {
                /* FETCH-AWAIT: scripts + microtasks are drained, but a suspended async body is awaiting a LIVE
                   fetch (a pending promise). The network completes now: resolve THIS flow's pending fetches — each
                   awaiting async body's reaction is enqueued as a job in this flow's queue (we are switched in,
                   flow_running == f) — then loop to run those jobs and resume the continuations. */
                flow_drain_pending(ctx, f);
                continue;
            }
            else return 1;   /* all scripts + chunks + microtask jobs + live fetches done */
            /* NULL ScriptOrModule name: an inline page script's name is the DOCUMENT's URL, which this host does
               not model yet — nothing here has one to give. It is what a relative `import('./chunk.js')` resolves
               against, so the moat's lazy-chunk surface needs the document URL plumbed to this call. */
            f->frame = JS_FlowNew(ctx, body, strlen(body), NULL, 0);   /* page <script>/chunk: classic non-strict global */
            DCHECK(f->frame != NULL, "flow_step: a page <script>/chunk did not compile");
        }
        {
            /* A <script>'s completion value is not observable to the page (only an eval API surfaces one), so it is
               taken and released here — never DISCARDED by the engine, which would hide a live value from the host. */
            JSValue cv = JS_UNDEFINED;
            int suspended = JS_FlowResume(ctx, (JSValue *)f->frame, &cv);
            JS_FreeValue(ctx, cv);
            if (suspended) return 0;   /* quantum yield — more work, resume later */
        }
        JS_FlowFree(ctx, (JSValue *)f->frame); f->frame = NULL; f->script_i++;   /* this script done -> next */
    }
}

/* Context switches performed by the dispatch loop, for the result document (result.h). Cumulative for the
   life of this engine — one wasm instance is one document, so that is the document's count. */
static int g_switches = 0;
int engine_switch_count(void) { return g_switches; }

static void flow_switch_out(JSContext *ctx, Flow *f) {   /* pause f: snapshot its solver state, restore baseline */
    /* the PARKED CONTINUATION travels with the flow, for the reason the delta does: it resumes a suspended
       async activation of THIS flow, under THIS flow's heap. Left in the runtime it would be resumed by
       whichever flow the scheduler picked next — against the wrong delta — or, if that flow parked too, hit
       JS_ParkFlow's one-slot assertion, which is exactly what the smoke test was aborting on. */
    { JSContext *pc; JSFlowParkFn *pf; void *po;
      if (JS_TakeParkedFlow(JS_GetRuntime(ctx), &pc, &pf, &po)) {
          f->park_ctx = pc; f->park_fn = (void *)pf; f->park_opaque = po;
      } }
    f->dec_blob = decide_suspend();
    f->pin_blob = concolic_pins_suspend();
    cow_unapply(ctx, (CowDelta *)f->delta);
    cow_set_current(NULL);
    dom_unapply();                                  /* DOM twin of cow_unapply: restore the baseline document */
    f->dom = dom_buf_take(&f->dom_n, &f->dom_cap);  /* detach this flow's DOM head so the global is empty for the next flow */
    f->dom_base = dom_base_take();                  /* ...and its shared base chain (NULL until a DOM fork) */
    flow_set_running(NULL);
}

static void flow_switch_in(JSContext *ctx, Flow *f) {   /* resume/start f: apply its delta + solver state */
    JS_PutParkedFlow(JS_GetRuntime(ctx), (JSContext *)f->park_ctx, (JSFlowParkFn *)f->park_fn, f->park_opaque);
    f->park_ctx = NULL; f->park_fn = NULL; f->park_opaque = NULL;
    if (!f->delta) f->delta = cow_delta_new();
    cow_set_current((CowDelta *)f->delta);
    cow_apply(ctx, (CowDelta *)f->delta);
    dom_buf_load(f->dom, f->dom_n, f->dom_cap);   /* attach this flow's DOM head (NULL/0 for a fresh flow = empty) */
    dom_base_load(f->dom_base);                   /* ...and its base chain, BEFORE dom_apply walks it */
    dom_apply();                                  /* DOM twin of cow_apply: replay this flow's document writes */
    if (!f->started) { f->started = 1; decide_enter(ctx, f); }   /* fresh flow: replay from cursor 0 */
    else {                                                        /* paused flow: restore where it left off */
        decide_resume(f->dec_blob, f->fn);   decide_blob_free(f->dec_blob); f->dec_blob = NULL;
        concolic_pins_resume(f->pin_blob);   concolic_pins_blob_free(f->pin_blob); f->pin_blob = NULL;
    }
    flow_set_running(f);
}

static void flow_finish(JSContext *ctx, Flow *f) {   /* f completed: tear down its interleaving state + remove */
    /* "all scripts, chunks, jobs and fetches are done" cannot be true with a continuation still parked — the
       loop above resumes one before it can answer that. Asserting it here is what keeps the park inside the
       no-work-item-is-ever-dropped rule rather than merely intending to. */
    DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && f->park_fn == NULL,
           "a flow finished with a continuation still parked — that flow's async activation is dropped");
    decide_leave(ctx);
    cow_unapply(ctx, (CowDelta *)f->delta); cow_set_current(NULL);
    cow_delta_free(ctx, (CowDelta *)f->delta); f->delta = NULL;
    /* f is CURRENT here (its head+base are loaded as the globals, and the head may have realloc'd during the run
       so f->dom is stale). dom_revert restores baseline + frees the head entries + unrefs the base chain; then
       free the now-empty global head ARRAY. Never touch the stale f->dom/f->dom_base — the live buffers are the
       globals. */
    dom_revert();
    { int dn, dc; free(dom_buf_take(&dn, &dc)); }
    f->dom = NULL; f->dom_n = f->dom_cap = 0; f->dom_base = NULL;
    for (int i = 0; i < f->dyn_n; i++) free(f->dyn[i]);
    free(f->dyn); f->dyn = NULL; f->dyn_n = f->dyn_cap = 0;
    /* flow_step returns 1 (finished) only with an empty job queue, but free any residual defensively. */
    for (int i = 0; i < f->njob; i++) { for (int k = 0; k < f->jobs[i].argc; k++) JS_FreeValue(ctx, f->jobs[i].argv[k]); free(f->jobs[i].argv); }
    free(f->jobs); f->jobs = NULL; f->njob = f->jobcap = 0;
    /* FETCH-AWAIT: flow_step drains pending before finishing, but free any residual (resolve capabilities + values). */
    for (int i = 0; i < f->npend; i++) { JS_FreeValue(ctx, f->pending[i].resolve); JS_FreeValue(ctx, f->pending[i].value); }
    free(f->pending); f->pending = NULL; f->npend = f->pendcap = 0;
    flow_set_running(NULL);
    flow_remove(ctx, f);
}

/* Run ONE flow start-to-finish (no interleaving) — the @S candidate re-run path, where a single candidate must
   execute fully to observe whether it fires. Uses the flow machinery + a transient COW delta. */
/* THE ONE BFS SCHEDULER — explore and @S candidate-verify are the SAME loop, differing ONLY in whether a concolic
   branch FORKS. A separate verify executor (a `while(JS_FlowResume){}` driving one candidate to completion with
   preemption off) is the cardinal violation twice over — a second scheduler beside the BFS AND a drive-to-
   completion (an unbounded candidate loop would hang, non-parkable). So verify is this same loop with forking off:
   ONE concrete path (no branch/fork hook), yet every candidate flow is preemptible + parkable like any other. */
static const JSFlowControlHooks FC_EXPLORE = { .branch = solver_decide, .fork = engine_fork_finalize, .preempt = preempt_hook };
static const JSFlowControlHooks FC_VERIFY  = { .preempt = preempt_hook };   /* candidate re-fire: no fork, still preemptible */
static const JSFlowControlHooks FC_OFF     = { 0 };

/* The quantum's wall clock. CLOCK_MONOTONIC, because the slice is about elapsed thread time and a wall-clock
   adjustment must not shorten or extend it. */
static int64_t engine_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* THE SESSION. The dispatch loop is not a function that drains — it is a state machine its HOST steps, because
   the cooperative-quantum yield in CLAUDE.md's §scheduler is exactly that: after a bounded wall-clock slice the
   scheduler RETURNS so the one thread pumps its message port, streams findings, interleaves other documents,
   and then resumes the byte-identical frontier. A `for(;;)` that runs to exhaustion cannot do any of it, and in
   the extension it freezes the worker outright. The state that used to be the loop's C locals lives here so a
   return between two iterations costs nothing to resume from. */
static JSContext *g_sess_ctx;
static char *const *g_sess_bodies;
static int g_sess_n;
static Flow *g_sess_cur;
static int g_sess_live;

void engine_sched_begin(JSContext *ctx, char *const *bodies, int n, int forking) {
    DCHECK(!g_sess_live, "engine_sched_begin while a session is already running — one scheduler, one session");
    g_sess_ctx = ctx; g_sess_bodies = bodies; g_sess_n = n; g_sess_cur = NULL; g_sess_live = 1;
    flow_add(ctx, JS_UNDEFINED, NULL, 0);   /* the first flow: the page's scripts, empty decision vector */
    JS_SetFlowLocalMark(1);                 /* objects created while a flow runs are flow-local (discarded) */
    dom_cow_set_ctx(ctx);                   /* the DOM delta needs ctx for the attribute taint-shadow dup/free */
    g_dom_capture = 1;                       /* record DOM writes into the running flow's delta (twin of FlowLocalMark) */
    JS_SetFlowControlHooks(forking ? &FC_EXPLORE : &FC_VERIFY);   /* preempt ALWAYS on; fork only when exploring */
    JS_SetJobEnqueueHook(engine_enqueue_job);   /* ASYNC-AS-FLOW: reactions route to the enqueuing flow's queue */
}

/* One QUANTUM. Returns ENGINE_STEP_DONE when the frontier is empty (the session is closed and its hooks are
   uninstalled) and ENGINE_STEP_YIELD when the slice expired with the frontier intact. The slice is wall-clock
   and it is NOT a cap: nothing is dropped, starved, reordered or forgotten across it — the next call resumes the
   same top flow on the same frontier, which is the razor §scheduler states. */
int engine_sched_step(void) {
    JSContext *ctx = g_sess_ctx;
    char *const *bodies = g_sess_bodies;
    int n = g_sess_n;
    Flow *cur = g_sess_cur;
    int64_t deadline = engine_now_ms() + ENGINE_QUANTUM_MS;
    DCHECK(g_sess_live, "engine_sched_step with no live session");
    for (;;) {
        Flow *best = flow_best();           /* WFQ: highest value-of-information — a fresh fork (UCB) can preempt */
        if (!best) break;
        if (best != cur) {                  /* context switch: swap COW delta + decision + pins */
            if (cur) flow_switch_out(ctx, cur);
            flow_switch_in(ctx, best);
            best->visits++;
            cur = best;
            /* COUNTED, and it leaves in the result document. A scheduler that interleaves and one that runs
               its flows FIFO produce the same endpoints on an easy page and diverge on every hard one, so
               "does it actually switch" cannot be inferred from the findings — it has to be reported. The
               host's WFQ reads it for exactly that. Cumulative across steps: the host wants the document's
               total, not the last slice's. */
            g_switches++;
        }
        flow_age_running(1);   /* this step burned CPU; flow_credit_emit resets it when the flow emits value */
        if (flow_step(ctx, cur, bodies, n)) { flow_finish(ctx, cur); cur = NULL; }
        if (engine_now_ms() >= deadline) {   /* THREAD-SHARING, not value: hand the thread back, keep the frontier */
            g_sess_cur = cur;
            return ENGINE_STEP_YIELD;
        }
    }
    g_sess_cur = cur;
    /* ASYNC-AS-FLOW forcing function: every flow has run to completion, so NO microtask/promise reaction may
       still be queued. If one is, the scheduler DROPPED it — the not-yet-built async-as-flow capability (a
       reaction must become a first-class scheduler flow carrying the queuing flow's COW, which needs a fork
       job-enqueue hook). Crash LOUD here rather than silently drop it, so the gap cannot hide. */
    DCHECK(!JS_IsJobPending(JS_GetRuntime(ctx)),
           "async: a job reached the global list (enqueued outside a flow) but was never drained");
    JS_SetJobEnqueueHook(NULL);
    JS_SetFlowControlHooks(&FC_OFF);
    JS_SetFlowLocalMark(0);
    g_dom_capture = 0;
    g_sess_live = 0;
    return ENGINE_STEP_DONE;
}

/* A host that has nothing else to do between quanta — the node smoke test — drives the SAME steps in a loop.
   That is a HOST's loop over the one scheduler, not a second scheduler: the state machine is unchanged and a
   quantum boundary is invisible to it. */
static void run_scheduler(JSContext *ctx, char *const *bodies, int n, int forking) {
    engine_sched_begin(ctx, bodies, n, forking);
    while (engine_sched_step() != ENGINE_STEP_DONE)
        ;
}

/* EXPLORE: seed boot + drain the frontier, forking at every concolic branch. */
void engine_run(JSContext *ctx, char *const *bodies, int n) { run_scheduler(ctx, bodies, n, 1); }

/* @S CANDIDATE RE-FIRE: the SAME BFS loop with forking off — one concrete path (the injected candidate), still
   preemptible + parkable. No separate executor, no drive-to-completion. */
void flow_exec_once(JSContext *ctx, char *const *bodies, int n) { run_scheduler(ctx, bodies, n, 0); }
