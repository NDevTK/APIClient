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
 * cow_capture_hook (installed as JSTimeTravelHooks.prop_write) records a baseline slot's pre-write value into
 * the CURRENT flow's delta (set by cow_set_current before each resume). unapply/apply are inverse and
 * idempotent-in-pairs. */
#ifndef ENGINE_HOST_SOLVER_COW_H
#define ENGINE_HOST_SOLVER_COW_H

#include "quickjs.h"

typedef struct CowDelta CowDelta;

CowDelta *cow_delta_new(void);
void      cow_delta_free(JSContext *ctx, CowDelta *d);

/* Fork a delta at a branch: freeze src's HEAD into a shared immutable base segment (refcount 2) that both src
   and the returned sibling reference, so the sibling inherits src's branch-point state in O(1) — NOT a copy —
   then each diverges on its own head. The DOM twin is dom_cow_fork. Pairs with JS_FlowClone. */
CowDelta *cow_delta_fork(JSContext *ctx, CowDelta *src);

/* Route captures to `d` (the flow about to run). NULL = captures are dropped (baseline setup). */
void      cow_set_current(CowDelta *d);

/* Install as JSTimeTravelHooks.prop_write: called before a write to a baseline object; appends to the current
   delta. */
void      cow_capture_hook(JSContext *ctx, JSValueConst obj, JSAtom prop);

/* Install as JSTimeTravelHooks.cell_write: called before a write to a shared CLOSURE CELL; captures it into the
   current delta so a snapshot-forked sibling that shares the cell stays isolated on write. */
void      cow_capture_varref(JSContext *ctx, void *vref);

/* Install as JSTimeTravelHooks.arr_append: O(1) capture of a KNOWN-NEW fast-array append slot (no dedup scan, no
   baseline lookup — an append is always a fresh existed=0 slot). The accumulator hot path. */
void      cow_capture_arr_append(JSContext *ctx, JSValueConst obj, JSAtom atom);

/* Record a per-flow GENERATOR-STATE swap into delta `d` (JSTimeTravelHooks.gen_fork): the shared generator
   object `genobj` must resolve to `cur_gd` (a per-flow clone `d` OWNS) while `d` runs and to `base_gd` (its
   object-owned original) otherwise. Applied/unapplied like any other slot on context-switch. Dedup-REPLACES an
   existing gendata entry for `genobj` (a re-fork of the same generator inside this flow), keeping the original
   base. `d` takes ownership of one ref on `cur_gd` (freed via JS_GenDataUnref on delta free/replace). */
void      cow_delta_add_gendata(JSContext *ctx, CowDelta *d, JSValueConst genobj, void *base_gd, void *cur_gd);

/* Context-switch AWAY from `d`: save each slot's current (flow) value, then restore the baseline value. After
   this the shared baseline is pristine for the next flow. */
void      cow_unapply(JSContext *ctx, CowDelta *d);

/* Context-switch INTO `d`: replay the flow's saved writes over the pristine baseline (noop until the flow has
   run once). After this the heap shows exactly what this flow last saw. */
void      cow_apply(JSContext *ctx, CowDelta *d);

void      cow_free(JSContext *ctx);

#endif
