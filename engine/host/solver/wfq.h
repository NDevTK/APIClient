/* The ONE WFQ priority policy — the anytime-bandit order key at the HEART of the BFS solver, and the ONLY
 * place the formula lives. Pure + isolation-testable (feed val/visits/cpu, assert the ordering) and auditable
 * (it holds no scheduler state — main.c's flow_weight is a thin Flow->scalars adapter, and the fresh-flow
 * g_max_parked seed calls it with visits=cpu=0). ORDER-only: it decides WHICH flow runs next, NEVER WHETHER a
 * flow continues and NEVER drops a work item. The host-level (Level-1) ranking shares this policy via each
 * engine's reported top-weight (qjs_top_weight), not a re-implementation. See wfq.c. */
#ifndef ENGINE_HOST_SOLVER_WFQ_H
#define ENGINE_HOST_SOLVER_WFQ_H

/* Priority = base + accumulated emitted VALUE-of-information (new @H/@S raise val) + a UCB OPTIMISM/explore
   bonus proportional to 1/(visits+1) so a never-run flow is never starved by a proven one - a CPU-since-emit
   AGING decay so a monopolizer (burns CPU, emits nothing) SINKS below productive + unrun flows and is starved
   (deprioritized to ~0 CPU, resumable, NEVER terminated). Additive, NOT a value/cpu ratio — the aging term
   already yields value-per-CPU behavior without the 0/0 degeneracy a ratio hits on an unrun flow. */
double wfq_weight(double val, int visits, double cpu);

#endif
