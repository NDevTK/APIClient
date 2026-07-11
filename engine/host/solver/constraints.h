/* Per-flow value-domain constraint tracker — the concolic path constraint. As a flow runs, branch_decide
 * records "src IS/ISN'T tok" per decision; cons_feasible PRUNES a provably-contradicted arm (sound-only, never
 * a false contradiction) so no phantom @H; cons_fixed_value returns the value an `==` gate PINNED (for @H
 * concretization). Isolation-testable: pure feasibility logic over the constraint list. Also holds the per-flow
 * @S-delivery state reset alongside the constraints (required-origin, the JSON-envelope sink field-path/root).
 * See constraints.c. */
#ifndef ENGINE_HOST_SOLVER_CONSTRAINTS_H
#define ENGINE_HOST_SOLVER_CONSTRAINTS_H
#include "quickjs.h"   /* OPCMP_* enum */

typedef struct { char *src; char *tok; int op; char *jkey; } Cons;   /* op = HOLDING comparison of src vs tok; jkey = method-CLEAN JSON field path (for the @S envelope gate-merge) */
extern Cons *g_cons; extern int g_cons_cap, g_cons_n;
extern char g_origin_req[256];   /* forgeable origin gate on this path -> the PoC's delivery origin */
extern char g_sink_jkey[128];    /* the sink value's JSON field path (""/".html") for the @S envelope */
extern char g_sink_root[64];     /* the sink source root token ("{hash}") for merging sibling gate fields */

int opcmp_neg(int op);
void cons_reset(void);
void cons_set(int i, const char *src, const char *tok, int op, const char *jkey);
void cons_arm_feasible(const char *src, const char *tok, int true_op, int false_op, int upto, int *tf, int *ff);   /* the ONE feasibility seam the scheduler calls */
const char *cons_fixed_value(const char *src);
void cons_free(void);   /* teardown: reset + free the backing array */

#endif
