/* Cross-flow CONCOLIC-taint set — the HEAP taint-shadow, the exact twin of attr_shadow.c's DOM taint-shadow.
 *
 * Records the (object,property) slots that were ever written a CONCOLIC value (external/attacker input carrying
 * its provenance + example). Unlike the per-flow COW delta, this is GLOBAL and survives flow revert: a login
 * ACTION that writes `store.user` from a concolic reply must make EVERY later gate on `store.user` FORK, in ANY
 * flow, regardless of definition/execution order — the COW-isolated per-flow drive would otherwise never see
 * another flow's write. Monotonic (concrete->concolic only); the concrete/logged-out arm is still explored via
 * the fork.
 *
 * Keyed by object POINTER identity + atom (g_boot_delta stays applied between orphan flows, so the object
 * identity is stable and a pointer+atom key matches across flows). Self-contained: it owns the table and only
 * uses the PUBLIC quickjs value API. The interpreter reaches it through the engine's concolic-taint hooks — the
 * WRITE point records (JS_SetConcolicTaintAddHook), the READ point looks up (the component installs/clears the
 * get hook so a normal property read pays only a NULL-pointer check while the set is empty). */
#ifndef ENGINE_HOST_SOLVER_CONCOLIC_TAINT_H
#define ENGINE_HOST_SOLVER_CONCOLIC_TAINT_H

#include "quickjs.h"

void concolic_taint_init(JSContext *ctx);   /* install the record hook + remember ctx (once, at engine setup) */
void concolic_taint_reset(JSContext *ctx);  /* clear the set + disable the read hook (per-analysis: one wasm serves many pages) */

#endif
