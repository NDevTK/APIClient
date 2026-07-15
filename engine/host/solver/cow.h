/* Per-flow COW isolation — the swappable delta that makes flows INTERLEAVE like a browser.
 *
 * Each flow owns a CowDelta: the baseline (obj,prop) slots IT wrote, layered over the shared baseline. The
 * scheduler does NOT revert-to-baseline-then-rerun (a serial undo-log that forces a writer to run to
 * completion); it SWAPS deltas on every context switch — cow_unapply the outgoing flow (restore baseline,
 * SAVING the flow's current value so it can return) then cow_apply the incoming flow (replay ITS writes). So
 * two flows can each mutate the same shared `state.x` and each sees only its own value across any number of
 * mid-execution preemptions. A flow-local object (created during the run) is never captured, so a delta is
 * O(shared baseline state touched), never O(the run's transients).
 *
 * cow_capture_hook (the JSCowHook) records a baseline slot's pre-write value into the CURRENT flow's delta
 * (set by cow_set_current before each resume). unapply/apply are inverse and idempotent-in-pairs. */
#ifndef ENGINE_HOST_SOLVER_COW_H
#define ENGINE_HOST_SOLVER_COW_H

#include "quickjs.h"

typedef struct CowDelta CowDelta;

CowDelta *cow_delta_new(void);
void      cow_delta_free(JSContext *ctx, CowDelta *d);

/* Route captures to `d` (the flow about to run). NULL = captures are dropped (baseline setup). */
void      cow_set_current(CowDelta *d);

/* Install with JS_SetCowHook: called before a write to a baseline object; appends to the current delta. */
void      cow_capture_hook(JSContext *ctx, JSValueConst obj, JSAtom prop);

/* Context-switch AWAY from `d`: save each slot's current (flow) value, then restore the baseline value. After
   this the shared baseline is pristine for the next flow. */
void      cow_unapply(JSContext *ctx, CowDelta *d);

/* Context-switch INTO `d`: replay the flow's saved writes over the pristine baseline (noop until the flow has
   run once). After this the heap shows exactly what this flow last saw. */
void      cow_apply(JSContext *ctx, CowDelta *d);

void      cow_free(JSContext *ctx);

#endif
