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

/* Run the page's scripts as one code flow: each script `bodies[i]` is its OWN program (JS_FlowNew — faithful
   per-<script> scope, NEVER concatenated), run in document order, sharing globals + the flow's COW delta. */
void flow_exec_once(JSContext *ctx, char *const *bodies, int n);

/* Queue a DYNAMICALLY-LOADED script body (a lazy chunk / injected <script> / import()) to run in the CURRENT
   flow after the current script, sharing its globals + COW delta. Called from the script-load host-edge when
   forced execution reaches a load. Because a load sits behind a branch, the ONE BFS discovers different lazy
   scripts on different arms — lazy loading is not a separate system, just more code the flow runs and forks
   through. The body is copied; the queue is per-run (rebuilt each replay) and drained by flow_exec_once. */
void engine_queue_script(const char *body);

/* solver_decide calls this at a forking branch to stash the sibling's hot decision + pins; the interpreter's
   fork hook (engine_fork_finalize) assembles the sibling from the frame clone + these. */
void engine_prepare_fork(void *dec_blob, void *pin_blob);

/* Run the scripts to frontier exhaustion: seed the first flow, bracket each run with the decision state +
   per-flow COW delta, and drain the frontier by WFQ order. */
void engine_run(JSContext *ctx, char *const *bodies, int n);

#endif
