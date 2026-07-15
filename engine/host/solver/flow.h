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

typedef struct Flow {
    JSValue fn;            /* the function this flow re-drives (JS_UNDEFINED for a boot/session flow) */
    signed char *dec;      /* the DECISION VECTOR — the arm (0/1) this flow takes at each branch, in order */
    int dec_n;             /* length of dec */
    double val;            /* accumulated emitted VALUE (new @H + @S) — the WFQ's reward term */
    long cpu;              /* CPU units consumed since last emit — the WFQ's aging term */
    int visits;            /* dispatch count — the WFQ's UCB optimism denominator */
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

/* Remove + free a flow (its emitted work is done, or it was evicted). */
void  flow_remove(JSContext *ctx, Flow *f);

int   flow_count(void);

#endif
