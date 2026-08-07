/* FLOW + WFQ — the scheduler's unit of work, rebuilt clean and FRAME-AGNOSTIC.
 *
 * A FLOW is a code-flow through the program: a DECISION VECTOR over the shared pre-boot baseline. Its state is
 * replay(baseline, decision vector). The engine explores by re-running flows and forking at each concolic
 * branch — and CRUCIALLY, a fork is expressed as "append an arm to a decision vector," which is identical
 * whether the branch was a bytecode OP_if or a native builtin loop-back. That is the whole point of the
 * rebuild: the old design coupled forking to a bytecode OP_if rewind, so native frames could not fork; here a
 * flow is just (fn, decision vector), frame-agnostic by construction.
 *
 * The WFQ orders flows by an anytime-bandit priority: accumulated emitted VALUE + a UCB optimism bonus
 * (∝ 1/(1 + service) so a never-run flow is never starved) − CPU aging (a monopolizer that burns CPU without
 * emitting sinks below productive+unrun flows). ORDER-only: it never drops a work item. */
#ifndef ENGINE_HOST_SOLVER_FLOW_H
#define ENGINE_HOST_SOLVER_FLOW_H

#include "quickjs.h"

/* One queued microtask/reaction job owned by a flow (routed here by the job-enqueue hook, not a global list):
   the quickjs job function + its dup'd arguments, run under the flow's COW after its scripts. */
/* A queued job of this flow's, and WHICH of HTML 8.1.7's two queues it came from. `task` is not a label: the
   event loop performs a microtask checkpoint between one task and the next, so a task may not run while this
   flow still holds a microtask. One array keeps the two in a single arrival order — which is what a task source
   needs among its own tasks — and the pick applies the checkpoint rule. */
typedef struct { JSJobFunc *fn; int argc; JSValue *argv; int task; } FlowJob;

/* FETCH-AWAIT: a live (async) fetch this flow issued — a PENDING promise whose resolve capability + the value the
   "network" will deliver are held until the flow stalls, then resolved (the fetch completing) so the awaiting
   async body resumes. Per-flow (not global) so one flow's drain never resolves another flow's fetch. */
/* ONE live fetch this flow issued. `url` is what the trusted host must fetch (the sandbox cannot), and `value`
   is what the continuation resumes with once it has. A NULL url is an engine-supplied reply — a fixture or a
   modelled body — which needs no host round trip and drains immediately. */
/* WHAT THIS FLOW OWES ITSELF once the host supplies `url`. A fetch RESOLVES its promise with the reply; an
   injected <script src> has no promise at all — its reply is more of this flow's PROGRAM, queued as another
   script the one BFS runs. The kind is on the entry because it is the entry's business: the register, the
   dedup and the stall accounting are identical either way, and only the delivery differs. */
#define FLOW_PENDING_RESOLVE 0   /* fetch(): call `resolve` with the reply */
#define FLOW_PENDING_SCRIPT  1   /* injected <script src>: queue the reply as this flow's next program */
#define FLOW_PENDING_DOCSCRIPT 2 /* the document's OWN <script src>: the reply fills script slot `script_i` */
/* A REQUEST THIS FLOW IS OWED AN ANSWER TO. `method`, `hdrs`/`nhdr` and `body` are the rest of what the page
   asked — not decoration: SECURITY.md puts all network behind the trusted zone's safeFetch, and safeFetch is
   what decides SOP, CORS, method and credentials. It cannot decide about a method it was never told.
   EVERY OWNED FIELD HERE IS AN OBLIGATION AT THREE SITES: the push that fills it, the FORK that inherits it,
   and the free that releases it. The fork copied field by field into a malloc'd slot, so it already missed
   `script_i` before this record grew anything — and a missed field there is not a leak, it is uninitialised
   memory that the free path then walks. It copies the whole struct first now, so a field added later is
   inherited by default and only a POINTER needs a line of its own. */
typedef struct { char *name, *value; } FlowHeader;
typedef struct {
    JSValue resolve; JSValue value; char *url; int have_value; int kind; int script_i;
    char       *method;
    FlowHeader *hdrs;
    int         nhdr;
    char       *body;
    size_t      body_len;
} FlowPending;

/* WHAT ONE STEP OF A FLOW ANSWERED. OWED is not a third kind of flow — it is the same flow reporting that the
   work it has left belongs to the host, so the scheduler can tell an exhausted frontier from a waiting one
   without any member leaving the queue. */
#define FLOW_STEP_MORE  0
#define FLOW_STEP_DONE  1
#define FLOW_STEP_OWED  2

typedef struct Flow {
    JSValue fn;            /* the function this flow re-drives (JS_UNDEFINED for a boot/session flow) */
    signed char *dec;      /* the DECISION VECTOR — the arm (0/1) this flow takes at each branch, in order */
    int dec_n;             /* length of dec */
    double val;            /* accumulated emitted VALUE (new @H + @S) — the WFQ's reward term */
    long cpu;              /* CPU units consumed since last emit — the WFQ's aging term */

    /* INTERLEAVING STATE — persisted while this flow is PAUSED so the scheduler can run another flow and come
       back. A flow is preempted mid-execution (cooperative quantum) and resumed byte-identically; its COW
       delta, decision cursor, and pins all swap with it (see engine.c). Zero-initialized by flow_add. */
    /* A CANDIDATE SESSION. This flow re-runs the page with one attacker payload substituted for one source, to
       see whether it FIRES at the sink. It is not a different KIND of flow — same scripts, same scheduler, same
       preemption — it just carries the substitution, which is why the candidate lives here rather than in a
       driver that runs the program start-to-finish beside the BFS. NULL for an ordinary flow. */
    char *cand_src;        /* the source identity the payload replaces (owned) */
    char *cand_payload;    /* the breakout to try (owned) */
    const char *cand_sink; /* the sink name to record if it fires (static) */
    /* DID THIS FLOW'S PoC FIRE, and is its substitution live? Both were globals in solve.c, which is only
       correct while one candidate runs start-to-finish with nothing else scheduled — the shape the standalone
       verify driver has and the BFS does not. As a flow among flows a candidate is preempted, parked and
       resumed with ordinary flows in between, so a global `fired` records another flow's marker and a global
       `verifying` leaves the substitution live for whoever runs next. They belong to the flow, and they swap
       with it. */
    int cand_fired;        /* this flow's X9 marker executed */
    int cand_verifying;    /* this flow is a candidate run: the sink takes the concrete arg */

    int   started;         /* decide_enter has run (fresh) — else resume from the blobs below */
    void *frame;           /* the current script's live preemptible frame (JS_FlowNew handle), NULL between scripts */
    /* THE DOCUMENT'S LOAD STAGE, in THIS flow's world: 0 = still running scripts, 1 = DOMContentLoaded fired,
       2 = load fired. Per-flow because the listeners are (they are ordinary properties on the target, so the COW
       delta isolates registration), and because a forked arm reaches the end of the document in its own time. */
    int   dom_stage;
    int   script_i;        /* position in the script sequence: static [0,n), then this flow's dyn chunks [n, n+dyn_n) */
    /* THE HIGHEST SCRIPT INDEX THIS FLOW HAS COMPILED, so that compiling one twice can be caught. A flow runs
       each program in its sequence ONCE; a preempted flow RESUMES its suspended frame and never re-enters
       JS_FlowNew for a program it already started. Re-compiling is a REPLAY, which this engine does not do — a
       replay re-executes side effects the flow already performed against a delta that already holds them. */
    int   last_compiled;   /* -1 until the flow compiles its first program */
    char **dyn; int dyn_n, dyn_cap;   /* this flow's OWN lazily-loaded chunk bodies (per-flow, not global) */
    /* WHICH OF THOSE ARE @S CANDIDATES rather than the page's own script. A page script that does not compile is
       a real problem and asserts; a CANDIDATE that does not compile is the ordinary case — most breakouts do not
       fit most sink contexts, which is why the solver tries several and keeps the one that FIRES. CLAUDE.md
       names it: an unsolved @S candidate is a parked search, never a @WHY. Kept as a parallel array so the
       page-script assert stays fully armed inside a candidate flow, which still loads real chunks. */
    unsigned char *dyn_cand;
    void *delta;           /* this flow's isolated HEAP COW delta (CowDelta*), applied while running */
    void *dom; int dom_n, dom_cap;   /* this flow's isolated DOM COW delta HEAD buffer (dom_cow), swapped with the
                                        heap delta on every context-switch so the DOCUMENT is a per-flow time-travel
                                        entity: two flows see different trees/attributes, a rewind restores the
                                        exact document the flow saw. Detached via dom_buf_take while parked. */
    void *dom_base;        /* the shared IMMUTABLE base-segment chain below the head (dom_cow_fork): a snapshot-
                              forked sibling references the parent's O(N) DOM delta in O(1). NULL until a fork. */
    void *dec_blob;        /* suspended decision state while paused (decide_suspend) */
    void *pin_blob;        /* suspended pin state while paused (concolic_pins_suspend) */
    FlowJob *jobs; int njob, jobcap;   /* ASYNC-AS-FLOW: this flow's OWN queued microtasks AND tasks, drained
                                          after its scripts under its live COW (correct ordering, per-flow isolated) */
    FlowPending *pending; int npend, pendcap;   /* FETCH-AWAIT: this flow's OWN live (pending) fetches, resolved when
                                                   the flow's scripts+microtasks stall (the network completing). */
    /* THE PARKED CONTINUATION, swapped with everything else on a context switch. A forced preempt inside
       job-driven code parks an async activation in the RUNTIME's one slot; that activation belongs to THIS
       flow's timeline and resumes under THIS flow's delta, so leaving it in the runtime while a sibling runs
       would either resume it against the wrong heap or drop it outright. Empty (park_fn NULL) for a flow with
       nothing parked, which is every flow that has not preempted inside a reaction. */
    void *park_ctx; void *park_fn; void *park_opaque;
} Flow;

void  flow_registry_init(void);
void  flow_registry_free(JSContext *ctx);

/* Add a flow to the frontier. Takes ownership of `dec` (freed with the flow) and dups `fn`. `dec` may be NULL
   (dec_n 0) for a from-baseline flow. Returns the stored Flow* (stable until removed). Never fails (OOM aborts
   via CHECK — a dropped flow corrupts the frontier). */
Flow *flow_add(JSContext *ctx, JSValueConst fn, signed char *dec, int dec_n);
/* How many flows this document ever created — the other half of the switch count. A run whose cost jumped needs
   to say WHICH grew: the frontier, or the work per flow. */
long flow_created_count(void);

/* The WFQ priority of a flow (higher = run sooner). Pure function of the flow's reward/aging/visit state. */
double flow_weight(const Flow *f);

/* The highest-priority flow in the frontier, or NULL if empty. Does not remove it. */
Flow *flow_best(void);

/* The highest-priority flow OTHER than `exclude` — the running flow's rival for the value-driven yield. */
Flow *flow_best_other(const Flow *exclude);

/* A counter bumped on every frontier membership change (add/remove). The value-yield recomputes its rival
   only when this changes (or the running flow switches), never per-opcode. */
unsigned flow_frontier_gen(void);

/* The running flow (scheduler-set). Detectors credit emitted value to it; the scheduler ages it. */
void  flow_set_running(Flow *f);
Flow *flow_running(void);
void  flow_credit_emit(double v);   /* a NEW @H/@S from the running flow: raise reward, reset aging */
void  flow_age_running(long units); /* CPU burned this step without emitting */

/* Remove + free a flow (its emitted work is done, or it was evicted). */
void  flow_remove(JSContext *ctx, Flow *f);

int   flow_count(void);
/* The i'th flow in registry order, or NULL past the end — a WALK over the frontier's members, which is what a
   register living on the flows needs. flow_best answers which one to RUN; this answers who exists. */
Flow *flow_at(int i);

#endif
