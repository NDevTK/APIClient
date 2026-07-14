/* Forced-execution FORK decisions — the exploration policy, split out of the WFQ scheduler (the loop DISPATCHES
 * flows; this decides how a flow FORKS at an opaque gate).
 *
 * branch_decide is the engine's OP_if / iterator branch hook: at a gate on opaque external input it explores
 * BOTH arms by decision-vector BFS. An ORPHAN fork at the flow base is a SNAPSHOT-FORK — both arms CONTINUE from
 * a frame snapshot + shared COW heap/DOM deltas, never a replay re-run, so the cross-flow shared state the path
 * depends on is preserved. session/boot/async forks re-execute in their own context (a re-fired handler
 * sequence / a re-run boot / replayed awaits). fork_spawn_sibling builds the snapshot sibling that the
 * scheduler's suspend handler requests once the flow has cleanly suspended at the gate. */
#ifndef ENGINE_HOST_SOLVER_FORK_H
#define ENGINE_HOST_SOLVER_FORK_H

#include "quickjs.h"
#include "solver/scheduler.h"   /* Flow — the sibling is a Flow the scheduler re-queues */

int  branch_decide(JSContext *ctx, JSValueConst cond);   /* JS_SetBranchHook: fork both arms of an opaque gate (decision-vector BFS) */
int  ctx_forks(void);                                    /* JS_SetForkableHook: an opaque-collection iterator forks where branch_decide decides */
void fork_spawn_sibling(JSContext *ctx, Flow *f);        /* on a pending SNAPSHOT-FORK: build the sibling from a frame snapshot + shared deltas (no-op if not pending) */

#endif
