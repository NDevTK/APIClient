/* Deferred / callback FLOWS — a callback becomes a first-class BFS flow (the sibling of async_flow.c: that turns
 * a promise/await into a flow, this turns a setTimeout/rAF/microtask or an opaque-collection callback into one).
 * Each defers a STARTER flow that INHERITS the deferring flow's live COW delta via an O(1) shared base segment,
 * so the callback sees state the flow wrote before deferring. Split out of the scheduler: creating a flow-TYPE
 * belongs with the other flow-type components (boot_flow, async_flow), not the WFQ dispatch loop. */
#ifndef ENGINE_HOST_SOLVER_DEFER_H
#define ENGINE_HOST_SOLVER_DEFER_H

#include "quickjs.h"

void flow_defer_callback(JSContext *ctx, JSValueConst cb);                   /* setTimeout/rAF/microtask callback -> a starter flow inheriting the current delta (timers.c edge) */
void drive_opaque_cb(JSContext *ctx, JSValueConst cb, JSValueConst coll);    /* a callback on OPAQUE input (items.forEach(cb)) -> a flow driving cb with the collection's provenance (JS_SetCbHook) */

#endif
