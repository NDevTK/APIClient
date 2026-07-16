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
 * (∝ 1/(visits+1) so a never-run flow is never starved) − CPU aging (a monopolizer that burns CPU without
 * emitting sinks below productive+unrun flows). ORDER-only: it never drops a work item. */
#ifndef ENGINE_HOST_SOLVER_FLOW_H
#define ENGINE_HOST_SOLVER_FLOW_H

#include "quickjs.h"

/* One queued microtask/reaction job owned by a flow (routed here by the job-enqueue hook, not a global list):
   the quickjs job function + its dup'd arguments, run under the flow's COW after its scripts. */
typedef struct { JSJobFunc *fn; int argc; JSValue *argv; } FlowJob;

typedef struct Flow {
    JSValue fn;            /* the function this flow re-drives (JS_UNDEFINED for a boot/session flow) */
    signed char *dec;      /* the DECISION VECTOR — the arm (0/1) this flow takes at each branch, in order */
    int dec_n;             /* length of dec */
    double val;            /* accumulated emitted VALUE (new @H + @S) — the WFQ's reward term */
    long cpu;              /* CPU units consumed since last emit — the WFQ's aging term */
    int visits;            /* dispatch count — the WFQ's UCB optimism denominator */

    /* INTERLEAVING STATE — persisted while this flow is PAUSED so the scheduler can run another flow and come
       back. A flow is preempted mid-execution (cooperative quantum) and resumed byte-identically; its COW
       delta, decision cursor, and pins all swap with it (see engine.c). Zero-initialized by flow_add. */
    int   started;         /* decide_enter has run (fresh) — else resume from the blobs below */
    void *frame;           /* the current script's live preemptible frame (JS_FlowNew handle), NULL between scripts */
    int   script_i;        /* position in the script sequence: static [0,n), then this flow's dyn chunks [n, n+dyn_n) */
    char **dyn; int dyn_n, dyn_cap;   /* this flow's OWN lazily-loaded chunk bodies (per-flow, not global) */
    void *delta;           /* this flow's isolated HEAP COW delta (CowDelta*), applied while running */
    void *dom; int dom_n, dom_cap;   /* this flow's isolated DOM COW delta HEAD buffer (dom_cow), swapped with the
                                        heap delta on every context-switch so the DOCUMENT is a per-flow time-travel
                                        entity: two flows see different trees/attributes, a rewind restores the
                                        exact document the flow saw. Detached via dom_buf_take while parked. */
    void *dom_base;        /* the shared IMMUTABLE base-segment chain below the head (dom_cow_fork): a snapshot-
                              forked sibling references the parent's O(N) DOM delta in O(1). NULL until a fork. */
    void *dec_blob;        /* suspended decision state while paused (decide_suspend) */
    void *pin_blob;        /* suspended pin state while paused (concolic_pins_suspend) */
    FlowJob *jobs; int njob, jobcap;   /* ASYNC-AS-FLOW: this flow's OWN queued microtask/reaction jobs, drained
                                          after its scripts under its live COW (correct ordering, per-flow isolated) */
} Flow;

void  flow_registry_init(void);
void  flow_registry_free(JSContext *ctx);

/* Add a flow to the frontier. Takes ownership of `dec` (freed with the flow) and dups `fn`. `dec` may be NULL
   (dec_n 0) for a from-baseline flow. Returns the stored Flow* (stable until removed). Never fails (OOM aborts
   via CHECK — a dropped flow corrupts the frontier). */
Flow *flow_add(JSContext *ctx, JSValueConst fn, signed char *dec, int dec_n);

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

#endif
