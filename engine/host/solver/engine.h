/* The DISPATCH LOOP — drains the WFQ frontier. Each flow is the page's scripts run as ONE preemptible program
 * (JS_FlowNew), replaying the flow's decision vector; the first flow is the empty vector. A concolic branch
 * inside a run forks a sibling flow (decide.c); the loop keeps running the highest-value flow until the
 * frontier is empty. There is NO separate boot executor — the scripts ARE the first flow.
 *
 * A @S candidate re-fire is a FLOW seeded onto this same frontier (solve_seed_candidates), not a separate
 * executor: one scheduler runs exploration and verification alike. */
#ifndef ENGINE_HOST_SOLVER_ENGINE_H
#define ENGINE_HOST_SOLVER_ENGINE_H

#include "quickjs.h"

/* Run the page's scripts as one code flow: each script `bodies[i]` is its OWN program (JS_FlowNew — faithful
   per-<script> scope, NEVER concatenated), run in document order, sharing globals + the flow's COW delta. */

/* Queue a DYNAMICALLY-LOADED script body (a lazy chunk / injected <script> / import()) to run in the CURRENT
   flow after the current script, sharing its globals + COW delta. Called from the script-load host-edge when
   forced execution reaches a load. Because a load sits behind a branch, the ONE BFS discovers different lazy
   scripts on different arms — lazy loading is not a separate system, just more code the flow runs and forks
   through. The body is copied; the queue is per-run and drained by the flow that owns it. */
void engine_queue_script(const char *body);
/* Park the running flow on an injected <script src>: the host fetches it, and the reply becomes this flow's next
   program rather than a promise's value. */
void engine_pending_script_url(JSContext *ctx, const char *url);
/* Park the running flow on the document's OWN external <script src> at position `script_i`: classic scripts run
   in document order, so the flow waits there, and the reply fills the shared slot every flow reads. */
void engine_pending_docscript(JSContext *ctx, const char *url, int script_i);

/* solver_decide calls this at a forking branch to stash the sibling's hot decision + pins; the interpreter's
   fork hook (engine_fork_finalize) assembles the sibling from the frame clone + these. */
void engine_prepare_fork(void *dec_blob, void *pin_blob);

/* Run the scripts to frontier exhaustion: seed the first flow, bracket each run with the decision state +
   per-flow COW delta, and drain the frontier by WFQ order. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, int n);

/* THE SESSION — the same dispatch loop, stepped by its HOST instead of drained. The extension's host has other
   work between quanta (its message port, other documents' engines, streaming findings), and CLAUDE.md's
   cooperative-quantum yield says the scheduler RETURNS for exactly that and then resumes the byte-identical
   frontier. engine_run is a host with nothing else to do, so it is these two in a loop — one scheduler either
   way. ENGINE_STEP_YIELD leaves the session live and every flow where it was; ENGINE_STEP_DONE means the
   frontier is empty and the session's hooks are uninstalled. */
#define ENGINE_STEP_DONE   0
#define ENGINE_STEP_YIELD  2   /* the value the extension bridge's qjs_step already speaks */
/* STALLED: every flow has run as far as it can, but the frontier is not exhausted — one or more are parked on
   something only the HOST can supply (a reply the sandbox cannot fetch). The session stays LIVE and every
   parked flow keeps its snapshot; the host supplies what is owed and steps again. Without this the scheduler
   closes the session on an empty run-queue and those flows are never resumed, which is how a page whose config
   gates its later endpoints loses everything after the first request. */
#define ENGINE_STEP_STALLED 3
#define ENGINE_QUANTUM_MS  12  /* a thread-sharing floor, not a cap: nothing is dropped across it */
/* WHAT THE HOST IS OWED. The scheduler asks this ONE seam before it decides the frontier is exhausted; a
   non-zero answer means STALLED rather than DONE. It is a question, not policy: the scheduler holds no idea of
   what a reply is, and the host holds no idea of what a flow is. */
void engine_set_stall_hook(int (*owed)(void));

void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, int n, int forking);
int  engine_sched_step(void);

/* FETCH-AWAIT: a host fetch that returns a PENDING promise registers its resolve capability + the value it will
   deliver. A flow that awaits it parks; when the frontier stalls, engine_run resolves these and un-parks (the
   network completing). Called from a live-fetch host-edge that models an asynchronous GET. */
void engine_pending_fetch(JSContext *ctx, JSValueConst resolve, JSValueConst value);

/* The same park, with the URL only the TRUSTED HOST can fetch. The value arrives later through engine_provide;
   until it does the flow cannot finish, which is what keeps reply-gated code reachable. ONE register — the
   flow's own — because the reaction the resolve enqueues belongs to that flow and to its COW delta. */
void engine_pending_fetch_url(JSContext *ctx, JSValueConst resolve, JSValueConst value, const char *url);
/* THE FRONTIER'S BEST WEIGHT — what the host ranks this document's engine by against every other live one.
   Level-1 and level-2 are ONE policy (§scheduler): the host orders engines by their best flow exactly as the
   engine orders flows, so this is flow_weight of flow_best and nothing else. -inf when nothing is runnable, so
   an engine with no work never outranks one that has some. */
double engine_top_weight(void);

/* THE VALUE YIELD's floor: the weight of the best flow in the RUNNER-UP engine. The running flow hands the
   thread back the moment its own weight falls below it, because from there the other document's work is worth
   more — level-1 and level-2 are one policy. It is ORDER only and drops nothing: the flow keeps its snapshot
   and resumes exactly where it was, which is what separates a yield from a cap. -inf (the default) means the
   host has named no rival, so only the cooperative quantum yields. */
void engine_set_yield_floor(double w);

/* THE VALUE YIELD (§scheduler level-1). The host sets the RUNNER-UP ENGINE's best weight as this engine's
   floor; the moment this engine's own best flow no longer outranks that, it hands the thread back so the host
   can run the better document. It is not a slice and not a cap: nothing is dropped, reordered or forgotten —
   the frontier is exactly where it was and the next step resumes it. -inf (the default) means "run on". */
void engine_set_yield_floor(double floor);
const char *engine_pending_urls(void);                                  /* newline-joined, or "" */
int engine_provide(JSContext *ctx, const char *url, JSValueConst value); /* entries filled */

/* Install as JSTimeTravelHooks.gen_fork: a concolic branch inside a synchronously-driven generator body forked
   the flow, and clone_deep_flow built a per-flow gen_data clone. Stash the swap; engine_fork_finalize drains it
   onto the new sibling's COW delta (so the shared generator object resolves per-flow). */
void engine_gen_fork(JSContext *ctx, JSValueConst genobj, void *base_gd, void *cur_gd);

/* How many times the dispatch loop CONTEXT-SWITCHED between flows. The result document reports it because the
   findings cannot: an interleaving scheduler and a FIFO one agree on an easy page and disagree on every hard
   one, so the interleave has to be observable on its own. */
int  engine_switch_count(void);

#endif
