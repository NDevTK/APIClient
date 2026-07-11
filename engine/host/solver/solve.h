/* @S SOLVER — the sink entry + the solver's lifecycle/collection, its own component (solver/solve.c).
 *
 * solve_add is the destination every DOM/URL/JS sink host-edge calls when tainted input reaches it: it records
 * the reached sink and (for an opaque flow) spawns candidate-replay flows that drive concrete breakout payloads
 * through the REAL code to prove a working PoC. The solver is scheduler-COUPLED (it enqueues flows) but reaches
 * the scheduler only through solver/scheduler.h, so it is a component with a narrow contract. This header lets
 * the host-edges (dom_element.c, ...) call solve_add, and the engine drive the solver's lifecycle, without
 * seeing solve.c internals. `sctx` is the sink context: "html"/"htmls"/"url"/"js"/"scripturl". */
#ifndef ENGINE_HOST_SOLVE_H
#define ENGINE_HOST_SOLVE_H

#include "quickjs.h"

void solve_add(JSContext *ctx, const char *sink, const char *sctx, JSValueConst val);   /* a sink reached by tainted input */
void gate_collect(const char *token, const char *src);   /* JS_SetGateHook target: a concrete string the code tested tainted input against */
JSValue solve_all(JSContext *ctx);   /* pure COLLECTOR of the verified PoCs -> securitySinks[] (safe every snapshot) */
int solve_is_verified(JSContext *ctx, const char *vtarget);   /* scheduler: is this "sink|ctx" already solved? (skip a redundant candidate flow) */
void solve_init(JSContext *ctx);     /* create the @S accumulators (qjs_init) */
void solve_free(JSContext *ctx);     /* free the @S accumulators + gate tokens (qjs_teardown) */

extern JSContext *g_solve_ctx;   /* the clean candidate-eval realm — CREATED by the engine (needs the runtime), used by the solver */

#endif
