/* The DISPATCH LOOP — drains the WFQ frontier, running each flow by re-executing the program while replaying
 * the flow's decision vector. Boot is the first flow (empty vector). A concolic branch inside a run forks a
 * sibling flow (decide.c); the loop keeps running the highest-value flow until the frontier is empty.
 *
 * The executor is injected: `run_program` executes the program ONCE (evals the page's scripts / drives boot).
 * The engine only owns SCHEDULING — decide_enter/leave brackets + WFQ order — so the same loop drives a
 * bytecode program and a native-builtin-heavy one identically (frame-agnostic all the way up). */
#ifndef ENGINE_HOST_SOLVER_ENGINE_H
#define ENGINE_HOST_SOLVER_ENGINE_H

#include "quickjs.h"

/* Run the program to frontier exhaustion. `run_program(ctx, ud)` executes it once (the caller supplies the
   real executor); the engine seeds boot, brackets each run with the decision state, and drains the frontier. */
void engine_run(JSContext *ctx, void (*run_program)(JSContext *ctx, void *ud), void *ud);

#endif
