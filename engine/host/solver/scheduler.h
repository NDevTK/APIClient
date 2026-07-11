/* The ONE cross-session BFS scheduler — per-flow state types. A FLOW is the unit the WFQ orders: a forced
 * force-invoke STARTER (a function + a decision vector for its branch choices) that may be SUSPENDED mid-run
 * (fs = live heap frame, JS_FlowResume) and re-queued, carrying its own heap+DOM COW delta, value-of-
 * information, and (for @S) a concrete candidate. The registry + WFQ + dispatch live in the scheduler TU; this
 * header exposes ONLY the per-flow record so a coupled solver component (solve.c) can name it without pulling
 * in the scheduler internals — the same header discipline every browser component uses.
 *
 * NO BOUNDS: the registry and the decision vector grow dynamically (until RAM/disk, the platform floor); no
 * FLOW_MAX / DEC_MAX cap that would truncate distinct work the scheduler would otherwise reach. */
#ifndef ENGINE_HOST_SOLVER_SCHEDULER_H
#define ENGINE_HOST_SOLVER_SCHEDULER_H
#include <stdint.h>
#include "quickjs.h"

typedef struct {
    JSValue handle;      /* the function (starter/suspended) */
    double val;          /* accumulated value-of-information (emits raise it) */
    signed char *dec; int dec_n;  /* per-flow decision vector (branch-arm BFS) */
    void *fs;            /* live heap frame if SUSPENDED mid-run (JS_FlowResume), else NULL */
    void *cow; int cow_n, cow_cap;  /* this flow's HEAP COW DELTA, stashed while parked (unapplied); swapped in on resume */
    void *dom; int dom_n, dom_cap;  /* this flow's DOM COW DELTA, same swap discipline */
    int saved_c;         /* per-flow branch cursor (g_c) snapshot, restored on resume */
    double cpu;          /* back-edge CPU ticks since last emit (WFQ decay; reset to 0 on emit) */
    int visits;          /* times scheduled (UCB/fairness explore term) */
    int orphan_idx;      /* cross-session locator: index in deterministic orphan collection (-1 = boot/yield, not park-replayable) */
    char *candidate;     /* @S REPLAY flow: a concrete breakout payload the source getters return (instead of opaque),
                            so this flow re-runs the orphan through the REAL code+branches with the candidate; the sink
                            then sees a CONCRETE value and checks breakout. NULL = a normal opaque exploration flow. */
    int session;         /* ATTACKER SESSION flow: fire ALL registered handlers in seed order over ONE accumulating
                            COW delta (handler A's tainted write to shared state is visible to handler B), modeling an
                            attacker firing a sequence of events — the sound way to reach cross-handler sinks. */
    char *vtarget;       /* @S candidate flow: the "sink|ctx" it verifies. Once that's in g_verified, this flow is
                            REDUNDANT (another candidate already broke out) -> skip it, saving a full bundle re-run. */
    char *drive_src;     /* OPAQUE-COLLECTION callback flow (items.forEach(cb), items = a reply/injected opaque): the
                            element the callback is driven with must carry the COLLECTION's provenance, not a bare
                            {} — this is the collection's shape so the starter drives arg0 as {reply} (f.key reads
                            {reply}, keeping the reply taint) instead of losing it to g_concolic. NULL = default
                            g_concolic args (a genuine orphan handler whose args are external input). */
    int is_boot;         /* BOOT FLOW: re-run the page's boot (inline scripts) from the PRISTINE pre-boot baseline as a
                            FORKING starter, so an async reply (now cached, resolves synchronously on re-run) drives its
                            continuation's gated branches WITH the concolic example — the faithful boot-as-flow. */
    JSValue aresolve;    /* ASYNC-CALL flow: resolve fn of the invocation's result promise. On COMPLETION the scheduler
                            calls it with the return value, so an `await asyncFn()` caller-flow's promise settles and it
                            resumes. JS_UNDEFINED for a non-async-call flow. f.fs holds the pre-created async state. */
    JSValue areject;     /* ASYNC-CALL flow: reject fn of the result promise. On a THROWN completion the scheduler calls
                            it with the exception, so the awaiting caller's await re-throws (try/catch/.catch runs — a
                            throw path can itself reach a sink), never a silent resolve-with-undefined. */
    JSValue await_promise;   /* ASYNC-CALL flow PARKED on a still-pending await promise (JS_FlowResume returned 2): the
                                scheduler polls its state + resumes (JS_FlowResumeInject) once it settles. UNDEFINED = runnable. */
    JSValue rthis; JSValue *rargs; int rargc;   /* async-call RE-RUN RECIPE (func = handle) captured pristine at claim, so a
                                                   reject-replay sibling can JS_FlowNew the same call from scratch. */
    int is_async;   /* async-call flow (or a sibling of one): forks/replays await outcomes into the ONE decision vector
                       (g_dec), and re-runs via the recipe (func+args) so the await sequence is reproduced. */
    int sess_ctx;   /* the sink-CONTEXT is a session, DECOUPLED from the session RUN-MODE (`session`). A .then/await
                       continuation spawned while a session fired a handler inherits this: it RESUMES its own frame
                       (not the re-fire-all-handlers run-mode), yet a sink it reaches is treated as in-session so the
                       @S candidate replay spawns a candidate SESSION (re-fires the handler WITH the candidate,
                       delivering it through the .then chain the async source rode). Without it a handler-time async
                       sink (addEventListener('message', e=>P.resolve(e.data).then(t=>innerHTML=t))) never verifies —
                       the reaction runs OUTSIDE the session that fired it. */
} Flow;

/* Cross-session REPLAY RECIPE for an async-call flow: its source hash + decision vector + accumulated UCB
   state, so a rehydrated flow re-runs the same call and reproduces its await outcomes. */
typedef struct { uint32_t hash; signed char *dec; int dec_n; double val; int visits; int used; } AsyncRecipe;

/* ── SCHEDULER INTERFACE for the coupled @S solver TU (solver/solve.c) ──────────────────────────────────
   The scheduler OWNS the registry, the per-flow decision context, and the dispatch state; solve.c is the one
   solver component that must ENQUEUE candidate flows and read the running flow's context. It does so through
   this narrow interface instead of reaching into g_reg — the registry array stays encapsulated. */
Flow *reg_add(JSContext *ctx, JSValue handle, double val, signed char *dec, int dec_n);   /* append + return the new flow (never NULL; OOM aborts) */
Flow *spawn_async_sibling(JSContext *ctx, Flow *pf, signed char *dec, int dec_n);          /* re-run sibling of an async flow (NULL if JS_FlowNew fails) */

extern int g_in_session;        /* a session flow is running -> solve_add enqueues candidate SESSION flows */
extern int g_running;           /* a flow is currently dispatched (vs the monolithic boot) */
extern double g_cur_val;        /* the running flow's accumulated value (inherited by enqueued siblings) */
extern Flow *g_cur_flow;        /* the running flow (its is_async gates the async-sink candidate path) */
extern int g_cur_orphan_idx;    /* the running flow's orphan locator (inherited by candidate siblings) */
extern signed char *g_dec;      /* the running flow's decision vector (a candidate inherits it to replay the sink-reaching arms) */
extern int g_dec_n;             /* length of that vector */
extern int g_c;                 /* branch cursor / count of decisions taken this flow (solve_add's `gated` flag) */
extern char *g_candidate;       /* @S: the running REPLAY flow's concrete candidate (NULL in a normal flow) */
extern int g_emit_total;        /* the shared emit counter (a reached sink is progress like an @H) */
extern JSContext *g_ctx;        /* the MAIN analysis context — async hooks CLAIM a call only for it (never the @S solve realm) */
extern int g_in_boot_flow;      /* a BOOT flow is re-running boot -> suppress handler re-registration */
extern int g_boot_replay;       /* re-running boot for a candidate -> capture re-registered (closure) handlers, don't grow g_handlers */
extern JSValue g_orphan_buf[4096]; extern int g_orphan_n;   /* the deterministic orphan collection (functions to force-fire), a cross-session locator */
extern JSValue g_msg_event;     /* the synthetic {pm} attacker MessageEvent a 'message' listener is driven with */
int g_dec_ensure(int n);        /* grow the decision vector to hold >= n decisions (unbounded, RAM/disk floor) */

/* The registry array + WFQ/dispatch bookkeeping — the scheduler's own state (defined with the dispatch loop).
   The engine entry (qjs_step/begin/teardown) reads/writes some; the flow components never touch g_reg. */
extern Flow *g_reg; extern int g_reg_n, g_reg_cap;   /* the ONE flow registry */
extern int g_park_requested;    /* host RAM pressure -> park the cold tail to IDB */
extern long g_work;             /* flow dispatches this run (diagnostic) */
extern double g_yield_floor;    /* host cross-document value yield-floor (yield HOT when best flow <= this) */
extern int g_made_progress;     /* dispatched >=1 flow this qjs_step (zero-work ping-pong guard) */
extern double g_quantum_start;  /* wall-clock start of this cooperative quantum */
extern long g_switches;         /* flow suspend/re-queue events (interleave count) */
extern double g_max_parked;     /* highest parked-flow weight (O(1) wfq_yield preemption test) */
extern JSValue g_cur_fn;        /* the running starter's function (branch_decide forks a sibling that re-runs it) */
extern int g_dec_cap;           /* capacity of the decision vector */
extern JSValue g_park;          /* JS array of parked replay recipes (the IDB cold-tier frontier) */
#define QUANTUM_MS 12.0          /* cooperative wall-clock slice before yielding the worker thread to the host */
#define QUANTUM_SAMPLE 512u      /* read the wall clock once per this many back-edges (cheap quantum check) */
extern unsigned g_quantum_sample;  /* back-edge counter for the sampled quantum clock */
extern int g_resume_mode;          /* resuming a parked frontier: seed ONLY the recipes, not fresh orphans */
void scheduler_run(JSContext *ctx);   /* the ONE WFQ dispatch loop (qjs_step drives it) */
int  seed_orphans(JSContext *ctx);    /* collect + enqueue never-executed functions as orphan flows */
double scheduler_top_weight(void);   /* this engine's best-flow weight (host Level-1 ranking) */

#endif
