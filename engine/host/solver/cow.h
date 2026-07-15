/* Per-flow COW isolation — see the design in decide.h/flow.h. In the re-run model each flow re-executes the
 * program from the baseline, so isolation = REVERT every write a run made to a baseline object before the next
 * run. cow_capture_hook (the JSCowHook) records a baseline object's pre-write state into the delta; cow_revert
 * restores it. A flow-local object (created during the run) is discarded with the run and never captured, so
 * the delta is O(baseline state touched), never O(the run's transients). */
#ifndef ENGINE_HOST_SOLVER_COW_H
#define ENGINE_HOST_SOLVER_COW_H

#include "quickjs.h"

/* Install with JS_SetCowHook: called before a write to a baseline object. */
void cow_capture_hook(JSContext *ctx, JSValueConst obj, JSAtom prop);

/* Restore all captured baseline writes (old value, or delete if the prop was added) + clear the delta. Called
   by the dispatch loop after each flow run so the next flow starts from the baseline. */
void cow_revert(JSContext *ctx);

void cow_free(JSContext *ctx);

#endif
