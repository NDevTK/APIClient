/* The DISPATCH LOOP — drains the WFQ frontier. Each flow is the page's scripts run as ONE preemptible program
 * (JS_FlowNew), replaying the flow's decision vector; the first flow is the empty vector. A concolic branch
 * inside a run forks a sibling flow (decide.c); the loop keeps running the highest-value flow until the
 * frontier is empty. There is NO separate boot executor — the scripts ARE the first flow.
 *
 * `src`/`len` is the page's script source (boot_source). flow_exec_once compiles+runs it as a global-program
 * flow; the same primitive drives the scheduler AND @S candidate re-runs, so there is ONE executor. */
#ifndef ENGINE_HOST_SOLVER_ENGINE_H
#define ENGINE_HOST_SOLVER_ENGINE_H

#include "quickjs.h"
#include <stddef.h>

/* Run the page's scripts ONCE as a flow (global program, resumed to completion). The single executor. */
void flow_exec_once(JSContext *ctx, const char *src, size_t len);

/* Run the scripts to frontier exhaustion: seed the first flow, bracket each run with the decision state +
   per-flow COW delta, and drain the frontier by WFQ order. */
void engine_run(JSContext *ctx, const char *src, size_t len);

#endif
