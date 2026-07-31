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

/* THE SESSION — the same dispatch loop, stepped by its HOST instead of drained. The extension's host has other
   work between quanta (its message port, other documents' engines, streaming findings), and CLAUDE.md's
   cooperative-quantum yield says the scheduler RETURNS for exactly that and then resumes the byte-identical
   frontier. engine_run is a host with nothing else to do, so it is these two in a loop — one scheduler either
   way. ENGINE_STEP_YIELD leaves the session live and every flow where it was; ENGINE_STEP_DONE means the
   frontier is empty and the session's hooks are uninstalled. */
#define ENGINE_STEP_DONE   0
#define ENGINE_STEP_YIELD  2   /* the value the extension bridge's qjs_step already speaks */
#define ENGINE_QUANTUM_MS  12  /* a thread-sharing floor, not a cap: nothing is dropped across it */
void engine_sched_begin(JSContext *ctx, char *const *bodies, int n, int forking);
int  engine_sched_step(void);

/* FETCH-AWAIT: a host fetch that returns a PENDING promise registers its resolve capability + the value it will
   deliver. A flow that awaits it parks; when the frontier stalls, engine_run resolves these and un-parks (the
   network completing). Called from a live-fetch host-edge that models an asynchronous GET. */
void engine_pending_fetch(JSContext *ctx, JSValueConst resolve, JSValueConst value);

/* Install as JSTimeTravelHooks.gen_fork: a concolic branch inside a synchronously-driven generator body forked
   the flow, and clone_deep_flow built a per-flow gen_data clone. Stash the swap; engine_fork_finalize drains it
   onto the new sibling's COW delta (so the shared generator object resolves per-flow). */
void engine_gen_fork(JSContext *ctx, JSValueConst genobj, void *base_gd, void *cur_gd);

/* How many times the dispatch loop CONTEXT-SWITCHED between flows. The result document reports it because the
   findings cannot: an interleaving scheduler and a FIFO one agree on an easy page and disagree on every hard
   one, so the interleave has to be observable on its own. */
int  engine_switch_count(void);

#endif
