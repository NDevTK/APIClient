/* The ONE WFQ priority policy — see wfq.h. Extracted from main.c so the solver's core order key is one
 * auditable, isolation-testable formula rather than an inline expression duplicated at the fresh-flow seed. */
#include "solver/wfq.h"
#include "check.h"   /* DCHECK — the policy REFUSES to rank on corrupt (negative) flow accounting */

double wfq_weight(double val, int visits, double cpu) {
    /* visits and cpu-since-emit are monotonic, non-negative accounting; a negative value means corrupt flow
       bookkeeping (an uninitialized or double-freed Flow) — an internal invariant violation to crash at the
       origin, never silently rank on. (val may be any sign: an emit raises it, no floor is assumed.) */
    DCHECK(visits >= 0, "wfq: negative visit count — corrupt flow accounting");
    DCHECK(cpu >= -1e-9, "wfq: negative cpu-since-emit — corrupt flow accounting");
    return 1.0 + val + 0.5 / (double)(visits + 1) - 0.01 * cpu;
}
