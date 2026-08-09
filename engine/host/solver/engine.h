/* The DISPATCH LOOP — drains the WFQ frontier. Each flow is the page's scripts run as ONE preemptible program
 * (JS_FlowNew), replaying the flow's decision vector; the first flow is the empty vector. A concolic branch
 * inside a run forks a sibling flow (decide.c); the loop keeps running the highest-value flow until the
 * frontier is empty. There is NO separate boot executor — the scripts ARE the first flow.
 *
 * A @S candidate re-fire is a FLOW seeded onto this same frontier (solve_seed_candidates), not a separate
 * executor: one scheduler runs exploration and verification alike. */
#ifndef ENGINE_HOST_SOLVER_ENGINE_H
#define ENGINE_HOST_SOLVER_ENGINE_H

#include "core/fetch/fetch.h"
#include "quickjs.h"

/* Run the page's scripts as one code flow: each script `bodies[i]` is its OWN program (JS_FlowNew — faithful
   per-<script> scope, NEVER concatenated), run in document order, sharing globals + the flow's COW delta. */

/* Queue a DYNAMICALLY-LOADED script body (a lazy chunk / injected <script> / import()) to run in the CURRENT
   flow after the current script, sharing its globals + COW delta. Called from the script-load host-edge when
   forced execution reaches a load. Because a load sits behind a branch, the ONE BFS discovers different lazy
   scripts on different arms — lazy loading is not a separate system, just more code the flow runs and forks
   through. The body is copied; the queue is per-run and drained by the flow that owns it. */
void engine_queue_script(const char *body);
/* An @S CANDIDATE, queued as the program it would be if it fired. Same queue, one difference: it is ALLOWED not
   to compile, because most breakouts do not fit most sink contexts and a candidate that does not parse simply
   never fires. A page script that does not compile still asserts. */
void engine_queue_candidate(const char *body);
/* Park the running flow on an injected <script src>: the host fetches it, and the reply becomes this flow's next
   program rather than a promise's value. */
void engine_pending_script_url(JSContext *ctx, const char *url);
/* Park the running flow on the document's OWN external <script src> at position `script_i`: classic scripts run
   in document order, so the flow waits there, and the reply fills the shared slot every flow reads. */
void engine_pending_docscript(JSContext *ctx, const char *url, int script_i);

/* THE DOCUMENT'S LOAD LIFECYCLE, owned by the browser layer and asked by the scheduler. Called once per stage
   per flow when that flow has run everything the document gave it: stage 0 fires DOMContentLoaded, stage 1
   fires load. Returns how many listener tasks it scheduled. Registered by the host that owns a Document; a
   scheduler with no document (the solver fixture) simply never has one. */
/* The event loop's timer step (timer.h). Registered by the timer component; asked when a flow is idle. */
void engine_set_timer_hook(int (*fn)(JSContext *ctx));
void engine_set_document_done_hook(int (*fn)(JSContext *ctx, int stage));

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

/* HOW THE HOST PAYS WHAT IT OWES, for the driver that runs the frontier to completion in one call
   (engine_run). The stepping entry answers a stall by returning to its caller, which is what the extension's
   qjs_step does; a one-call driver has no such caller, so a stall with nobody to answer it ended the run — and
   a flow that had issued a request stopped at that request, its continuation never reaching the reply. The
   provider fills what engine_pending_urls names and returns how many entries it filled; 0 ends the run, which
   is the honest answer to "nobody can supply this". */
void engine_set_provider(int (*provide)(JSContext *ctx));

void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, int n, int forking);
int  engine_sched_step(void);

/* The same park, with the URL only the TRUSTED HOST can fetch. The value arrives later through engine_provide;
   until it does the flow cannot finish, which is what keeps reply-gated code reachable. ONE register — the
   flow's own — because the reaction the resolve enqueues belongs to that flow and to its COW delta. */
void engine_pending_fetch_url(JSContext *ctx, JSValueConst resolve, JSValueConst value,
                              const FetchRequest *req);
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
/* A SYNCHRONOUS REQUEST ONLY THE HOST CAN ANSWER — see engine.c. Issue it, return to the scheduler (a step
   machine returns JS_STEP_YIELD), and the flow SUSPENDS until the answer lands; siblings run meanwhile. The
   rendezvous is the returned id, never the request text: the answer is computed under the ASKING FLOW'S world,
   so two identical questions from two flows are two questions with two answers. */
uint32_t engine_host_request(JSContext *ctx, const char *op);
/* Has it been answered? BORROWED, so a machine re-entered before it is ready to consume may read it again. */
int      engine_host_answered(uint32_t req, JSValueConst *out);
/* Take the answer; the request leaves the register. OWNED by the caller. */
JSValue  engine_host_take(JSContext *ctx, uint32_t req);
/* The host delivers. Routed by id to ONE call site — never broadcast the way a fetched body is. Returns 0 when
   the asking flow is gone, which is not an error: nobody is waiting. */
int      engine_host_answer(JSContext *ctx, uint32_t req, JSValueConst value);
/* What the host still owes, as `id<TAB>op` lines. Pulled each step, and deliberately NOT deduped. */
const char *engine_host_requests(void);

/* AN EMISSION TO THE HOST — one way, never answered, and therefore never a suspend. "A new document exists,
   here is its name and what to load in it" is that shape: HTML §4.8.5 creates a child navigable inside the
   insertion steps, so it cannot ask anything, and it does not need to — the name is minted here (world.h) and
   the host is TOLD. It is an emission for the same reason a cross-document message is one: immutable once sent,
   so nothing has to un-send it when the sending flow parks or is outranked, and it needs no COW capture. */
void        engine_host_notify(JSContext *ctx, const char *op);
/* The notices posted since the last call, newline-joined, DRAINED by the call. "" when there are none. */
const char *engine_host_notices(void);

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
long engine_jobs_queued(void);
long engine_jobs_run(void);

/* WHO COUNTS THE DOM'S WRAPPERS. The scheduler's diagnostic line reports the identity map's size, and that map
   is the DOM's — so the DOM registers the counter rather than the solver naming node.h and dragging lexbor in
   behind it. */
void engine_set_wrap_stats(void (*fn)(long *n, long *cap));

#endif
