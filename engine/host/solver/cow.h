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

/* Install as JSTimeTravelHooks.map_add: O(1) capture of a KNOWN-NEW Set/Map record (Set.add / Map.set of a fresh
   key on a shared collection). unapply deletes the flow's added record (JS_MapDeleteRecord), apply re-adds it
   (JS_MapAddRecord) — the Set/Map accumulator analogue of cow_capture_arr_append. */
void      cow_capture_map_add(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst val);

/* Install as JSTimeTravelHooks.map_mutate: capture a reversible OVERWRITE / DELETE of an existing Set/Map record
   as an undo-log entry (apply replays it, unapply inverts it), completing cow_capture_map_add's add-only capture
   so ALL shared Set/Map mutations are per-flow isolated. op is JS_MAP_MUTATE_OVERWRITE / _DELETE. */
void      cow_capture_map_mutate(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst old_val, JSValueConst val, int op);
/* Install as JSTimeTravelHooks.async_state: capture a shared promise's settlement (state + result + pending
   reactions) or a resolving-function pair's already_resolved latch before this flow changes it, so each arm of
   a fork settles a pre-fork promise on its OWN timeline. */
void      cow_capture_async_state(JSContext *ctx, JSValueConst obj);

/* Install as JSTimeTravelHooks.module_eval: capture a MODULE record's evaluation state (status + capability +
   cycle fields) before this flow changes it. Its BINDINGS are closure cells cell_write already captures; this is
   the state that decides whether a flow evaluates the module at all, so without it the first flow to import a
   chunk left it EVALUATED for every sibling and the siblings read its exports as TDZ. */
void      cow_capture_module_eval(JSContext *ctx, void *mod);

/* Install as JSTimeTravelHooks.async_fork: record the per-flow swap of a shared suspended ASYNC activation. The
   engine cloned it because resuming CONSUMES it; this makes the clone what this flow's resolve/reject closure
   names, leaving the original as the baseline every other arm still finds. */
void      cow_capture_async_fork(JSContext *ctx, JSValueConst closure, void *base_data, void *cur_data);

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

/* INSTALL THE TIME-TRAVEL RECORD BOUNDARY — the per-flow COW capture set, declared once for the reason the
   concolic set is: two entries each spelled it out as a struct literal, which is a list that can drift, and one
   of them already had. `.gen_fork` is the scheduler's, which is why this lives with the capture hooks that make
   up the rest of it rather than at either entry. */
void cow_install_time_travel_hooks(void);

#endif
