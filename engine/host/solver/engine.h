/* The dispatch loop: drains the WFQ frontier. A flow runs the page's scripts in document order, each its own
 * preemptible program (JS_FlowNew, never concatenated) sharing globals and the flow's COW delta, and replays
 * the flow's decision vector; the first flow is the empty vector. A concolic branch forks a sibling flow
 * (decide.c), and the loop keeps running the highest-value flow until the frontier is empty. Boot is the first
 * flow, and an @S candidate re-fire is a flow seeded onto the same frontier (solve_seed_candidates), so one
 * scheduler runs exploration and verification alike. */
#ifndef ENGINE_HOST_SOLVER_ENGINE_H
#define ENGINE_HOST_SOLVER_ENGINE_H

#include <stddef.h>   /* size_t: every program crosses this header as (text, length) */
#include <stdint.h>   /* int64_t: EngineStepUnitRuns' `step_us` is a microsecond accumulator; included here so
                         its width does not depend on include order */

#include <lexbor/dom/dom.h>

#include "core/fetch/fetch.h"
#include "core/loader/script_type.h"   /* ScriptType: which HTML §8.1.4.4 run algorithm runs a row */
#include "core/timing/task_source.h"   /* TaskSource: the HTML §8.1.7.1 task source, if any, that queued a row */
#include "solver/step_unit.h"          /* the arms of flow_step; EngineStepUnitRuns counts per arm */
#include "quickjs.h"

/* Declared, not included: solver/flow.h owns the definition, and the entries here that name a flow only pass
   it through, so the engine's public surface does not pull in the scheduler's private record. */
struct Flow;

/* Where a queued program lands in its flow's program sequence. The caller states it because it is a spec fact
 * about the operation that caused the program, never a scheduling preference; there is no default.
 *   DYN_POS_APPEND is the tail. Every task takes it, since a task queue is FIFO: a `javascript:` navigation
 * (HTML §7.4.2.2 "Beginning navigation") and a lazy chunk's reply. A document's own script sequence also takes
 * it, though it is not a task, because HTML §4.12.1.1 fixes its order. Each entry below states its TaskSource.
 *   DYN_POS_IMMEDIATE runs a program the running program caused before anything else the sequence holds: HTML
 * §4.12.1.1 "Processing model"'s "immediately execute the script element el, even if other scripts are already
 * executing", and ECMAScript §19.2.1.1 PerformEval, which returns its completion into the call expression.
 * Defaulting such a program to the tail would make an @S proof already fired wait on the flow draining the
 * rest of an unbounded sequence, so a kill, park or eviction would lose it. */
typedef enum { DYN_POS_APPEND, DYN_POS_IMMEDIATE } DynPos;

/* Queue a program whose bytes came from a response (a lazy chunk's reply) to run in the running flow after the
   current program, sharing its globals and COW delta. Lazy loading is not a separate system: a load sits
   behind a branch, so different arms discover different chunks. The body is copied.
   `doc` names the document whose realm compiles the program (solver/flow.h's `dyn_doc`). An instance is an
   origin-keyed agent cluster, so it may be a child navigable's document; the caller always knows which.
   `url` is the HTML §8.1.4.1 "Scripts" base URL and is required: §8.1.4.2 "Fetching scripts" creates the
   script with the response's URL, which error reports and nested `import()` resolve against. A NULL address
   means "the document's" (an inline script's answer) and the compile then sets JS_EVAL_FLAG_INLINE_SCRIPT.
   `body_n` is the length: a decoded response may hold U+0000 (ECMAScript §11.1 "Source Text").
   The row is a classic script (no element behind it, HTML §8.1.4.4 "Calling scripts") on HTML §8.1.7.4's
   networking task source, so it takes DYN_POS_APPEND.
   HTML §8.7 "Timers" string handlers are not queued here: core/timing/timer.c compiles and runs them inside the
   timer task on the firing flow's trampoline, which keeps the timer task source on one queue. */
void engine_queue_fetched_script(uint32_t doc, const char *body, size_t body_n, const char *url);
/* Queue a `<script>` element's inline program at the tail of the running flow's sequence. HTML §4.12.1.1
   "Processing model"'s "execute the script element" switches on the element's type, so the row carries
   `stype` and flow_step routes it to §8.1.4.4's classic or module run. Callers are the three ways a document
   other than the session's gets inline programs: a child navigable (core/frame/navigable.c), a joined
   document (engine_join_document) and an element page code inserted (core/html/html_script.c).
   The tail is §4.12.1.1's position for parsed scripts and for every module or external destination; only an
   inline classic script a page inserted runs in place (engine_queue_script_immediate). The task source is
   TASK_SOURCE_NOT_A_TASK: the parse that reached the element runs it.
   `el` is the element. The classic arm sets the document's §3.1.7 `currentScript` to it for the whole run, and
   the run spans scheduler steps, so the row carries it (solver/flow.h's `dyn_el`).
   `body_n` is the length, not strlen: an inserted element's `.textContent` never passed HTML §13.2.5.4
   "Script data state", so it may hold a NUL; DOM §4.11 "Interface Text"'s child text content supplies it. */
void engine_queue_element_script(uint32_t doc, const char *body, size_t body_n, ScriptType stype,
                                 lxb_dom_element_t *el);
/* Queue an inline classic script a page inserted to run immediately: HTML §4.12.1.1 "Processing model" ends
   "prepare the script element" with "Otherwise, immediately execute the script element el, even if other
   scripts are already executing". It takes the slot after the inserting program (DYN_POS_IMMEDIATE) and
   nothing already in the sequence runs in between. It is a separate entry so callers of the tail entry cannot
   pick it by accident. The type is always classic: every module has gone to one of §4.12.1.1's lists before
   this step. `el` and `body_n` are as for engine_queue_element_script. The task source is
   TASK_SOURCE_NOT_A_TASK because no task runs it, and engine_queue_into asserts that a task never takes
   DYN_POS_IMMEDIATE. */
void engine_queue_script_immediate(uint32_t doc, const char *body, size_t body_n, lxb_dom_element_t *el);
/* Hold an external `<script src>`'s position in the running flow's sequence. HTML §4.12.1.1 fixes its order
   against its neighbours (a pending parsing-blocking script, §13.2.6.4.8; the list of scripts that execute
   when parsing finishes, run in order by §13.2.7), so the row holds only the URL, the flow waits at it, and
   the reply becomes the program in that slot. The request is issued by this call, not when the flow reaches
   the slot: §4.12.1.1 step 33 fetches at prepare time and step 35 only decides where the result runs. The
   as-soon-as-possible set has no position and uses engine_pending_script_url.
   `url` is already resolved against the element's document, which only the caller knows. `stype` and `el`
   survive the reply: §8.1.4.2 "Fetching scripts" decodes a module as UTF-8 and a classic script by its
   charset, and creates the script with the response's URL (the base for nested `import()` and a module's map
   key). `parser_inserted` is whether `el` has a non-null parser document; only the inserting party knows it,
   it cannot be re-derived at the park (core/html/html_script.h leaves it unstated for a loaded document's own
   markup), and it feeds the request's provenance (solver/pending.h's `pending_prov_compose`). The task source
   is TASK_SOURCE_NOT_A_TASK: the row is a position, and the reply's networking task is delivered separately. */
void engine_queue_docscript_url(uint32_t doc, const char *url, ScriptType stype, lxb_dom_element_t *el,
                                int parser_inserted);
/* Queue an @S candidate, in the session's document, as the program it would be if it fired. Unlike a page
   script it may fail to compile: most breakouts do not fit most sink contexts, and one that does not parse
   never fires.
   `pos` is the sink's own semantics: an eval sink is ECMAScript §19.2.1.1 PerformEval (DYN_POS_IMMEDIATE); a
   markup sink's auto-firing handler and a URL sink's `javascript:` navigation are tasks (DYN_POS_APPEND).
   `body_n` is the length: a candidate is built from attacker-shaped bytes that may hold a NUL, and truncating
   it would fire a program the search did not choose.
   The task source is TASK_SOURCE_SOLVER_CANDIDATE, which is not an HTML §8.1.7.1 source, so the candidate is
   never ordered against the page's real tasks by a fact nobody observed. */
void engine_queue_candidate(const char *body, size_t body_n, DynPos pos);
/* Queue a `javascript:` URL's script source as the program HTML §7.4.2.3.2's evaluate-a-javascript:-URL runs.
   It may fail to compile: "create a classic script" with a syntax error yields an abrupt completion and a
   navigation that does not happen. Its completion value decides a navigation (step 9: a String replaces the
   Document with an HTML parse of it), and the scheduler is the only place that value exists.
   `doc` is the target navigable's active document (step 5's settings object), not always the session's.
   `body_n` is step 3's percent-decoded length; URL §1.3 "Percent-encoded bytes" can yield U+0000.
   The task source is HTML §8.1.7.4's navigation and traversal source (§7.4.2.2 "Beginning navigation" step
   21), which also reaches `jobs` through JS_EnqueueCallTask (core/frame/navigable.c). One arrival clock
   across carriers (solver/flow.c's g_work_seq) orders flow_step's task ladder, which keeps §8.1.7.1's
   per-source order whichever carrier an item rides.
   Reply deliveries are stamped on the same clock (solver/pending.h's PEND_WORK_SEQ) but still precede every
   row of the sequence; that residual is stated at the delivery arm in solver/engine.c. */
void engine_queue_javascript_url(uint32_t doc, const char *body, size_t body_n);
/* Run a host instrument's program in every live timeline of the session's document and record its completion
 * value. A driver outside the engine (testing/render_diff.js's collector) needs a JS value computed in a
 * document's realm with unknowns on their concrete examples; the console printer cannot carry one, and the
 * fetch/@H surface is the product's finding surface.
 *   It is a queued row (TASK_SOURCE_NOT_A_TASK), not an evaluation, because the host calls it between steps
 * where preempt_hook asserts the policy may not be consulted; the scheduler runs it and engine_take_dumps
 * returns the answers. Every live timeline gets it because the DOM is per-flow, and each answer names its
 * world. Requires a live session. `program` is copied.
 *   The value is dumped (solver/value_dump.h); a throw reports as the document's page error and aborts the dev
 * build. The program runs in the analysed realm and can write there, so it is trusted-zone text only, and a
 * production host does not call this. */
void engine_request_dump(const char *program);
/* Drain the dumps recorded since the last call, newline-joined, or "" when there are none. The engine owns the
 * returned buffer until the next call. Each record is `<world><TAB><json>`: the world in `world_name`'s
 * spelling (`doc:session:serial`, the head of solver/world.h's grammar with no ancestry) and the JSON on one
 * line (value_dump.h), one record per timeline in completion order. It is not `world_serialize`'s spelling
 * because serializing marks a world as having crossed (see flow_emit_dump). */
const char *engine_take_dumps(void);
/* Photograph every world this run mints from now on (@PERWORLD). The solver holds no painter: this marks the
 * members standing now (paint_mark_standing_members, which reaches the boot flow) and makes flow_new mark every
 * later member (flow_paint_every_world). The scheduler discharges a mark by returning the thread with that
 * member switched in, and the host's own `qjs_paint` (main.c) renders it. Requires a live session with a
 * non-empty frontier.
 *   The return is taken at the closing edge of the member's turn, because only that edge holds what the member
 * did: engine.c's FLOW_STEP_DONE arm (flow_finish, which unapplies the deltas, is deferred across the return)
 * or a slice-end yield that leaves the member standing. An ask made before the first step is answered at the
 * end of that member's first turn.
 *   A mark changes no ranking: it is not in flow_weight, the eligible set or the pick's filter, and a switch-in
 * made for a picture would forge flow_switch_in's ranking record. A mark is one bit, so re-marking is a no-op,
 * except that a member whose finish is deferred for a picture is skipped and the deferred-finish path asserts
 * against it. It is one-way (see flow_paint_every_world) so an image directory is never a silent sample of a
 * run's worlds. Each member that ends costs one extra return and one painting-order walk. */
void engine_request_paint_every_world(void);
/* Park the running flow on a `<script src>` with no position to hold: an element a page inserted, or a member
   of HTML §4.12.1.1's set of scripts that will execute as soon as possible (§13.2.7 waits for that set only
   before the load event, so arrival order is a correct order). The host fetches it and the reply becomes this
   flow's next program. An element whose position §4.12.1.1 fixes uses engine_queue_docscript_url.
   `stype` travels with the park because "execute the script element" switches on it; `el` rides the register
   (solver/pending.h's `scriptEl`) onto the row the drain makes; `parser_inserted` is as for
   engine_queue_docscript_url, and is not the park's kind because a parser-inserted `<script async src>` also
   comes here. */
void engine_pending_script_url(JSContext *ctx, const char *url, ScriptType stype, lxb_dom_element_t *el,
                               int parser_inserted);
/* The event loop's timer step (timer.h), registered by the timer component and asked when a flow is idle. The
   browser components below register hooks for the same reason: the scheduler may not depend on the browser
   half. */
void engine_set_timer_hook(int (*fn)(JSContext *ctx));
/* HTML §8.1.7.3's in-parallel half, the rendering task source (rendering.h). Asked immediately before the timer
   step: both are due on the one virtual clock, and this one defers to a timer that expires first. */
void engine_set_rendering_hook(int (*fn)(JSContext *ctx));
/* The event loop's idle rung: Cooperative Scheduling of Background Tasks §5.1 Start an idle period algorithm
   and §5.2 Invoke idle callbacks algorithm (core/scheduling/idle_callback.h). Asked below rendering and timers,
   because §5.1's note runs it when the event loop is "otherwise idle". Answers 1 when it started a period or
   queued one callback. */
void engine_set_idle_hook(int (*fn)(JSContext *ctx));
/* The document load lifecycle (core/dom/document.c's document_lifecycle_step), asked when a flow has run
   everything its documents gave it. Each call advances one document's HTML §13.2.7 stage (DOMContentLoaded or
   load) and answers 1, or answers 0 when every document of the agent is complete. One claimant at a time
   (asserted); NULL releases the slot. A scheduler with no document (the solver fixture) has no hook. */
void engine_set_document_done_hook(int (*fn)(JSContext *ctx));
/* HTML §4.6.8.20 Link type "preload"'s browsing-context-connected time for the elements a parse produced
   (html_link.h). Asked ahead of everything else a flow could do: a browser connects them during tree
   construction, so their requests precede every script of the document. The baseline walk that finds them can
   only inventory them, because §4.2.4.3 "Fetching and processing a resource from a link element" ends in a
   fetch, and a fetch parks on a flow. */
void engine_set_link_connected_hook(int (*fn)(JSContext *ctx));
/* The end of a microtask checkpoint: HTML §8.1.7.3's "perform a microtask checkpoint" step after the microtask
   queue drains and before the flag clears, asked when the flow that just ran a unit of work holds no
   microtask. Registered by the component that owns the steps HTML runs there: Indexed Database §2.7.1
   "cleanup Indexed Database transactions", whose at-most-once-per-transaction note makes asking at every step
   free. */
void engine_set_checkpoint_hook(void (*fn)(JSContext *ctx));

/* Decide where a prepared fork's sibling comes back. decide.c calls this with the sibling's decision and pin
 * blobs already built, and exactly one consumer takes them:
 *   - An activation that will be cloned: the interpreter at an OP_if, or the step driver for a machine that
 *     yielded JS_STEP_FORK, asking through engine.c's flow-control hook wrappers. The blobs are stashed and
 *     engine_fork_finalize assembles the sibling from them plus the clone. Returns 1: the caller owes the
 *     snapshot.
 *   - The flow's own scheduler step, when `restartable` (solver_decide_restartable) says re-running the step
 *     reaches the asking code again. The sibling is assembled here with no frame and replays its recorded arm.
 *     Returns 0: nothing is owed and the FORKED bit is not raised.
 * A C body that is neither has no resume point, and this DFAILs naming `site`; the fix is to declare it with
 * JS_CFUNC_STEP_DEF, never to ask less. It does not stash unconditionally, because an unconsumed stash would
 * abort the next fork far from its cause. `asked` is the constraint key, or NULL. Requires a running flow in
 * a forking session (asserted). */
int engine_prepare_fork(JSContext *ctx, void *dec_blob, void *pin_blob, const char *asked, int restartable,
                        const char *site);

/* Whether this session forks at all (explore vs verify). The interpreter and step driver read the policy
 * through the flow-control hook table, whose absent `branch`/`outcome` already mean the ordinary arm (-1, or
 * outcome 0). A caller that asks the decision seam by symbol (a browser component with no OP_if and no
 * machine) consults neither, so decide.c asks this and takes the arm that site declared (solver/decide.h's
 * `nonforking`). It is asked rather than passed in so the bit has one copy, written where the session
 * declares its policy; a second copy could disagree with the hook table and mint frontier members inside a
 * verification. */
int engine_session_forks(void);

/* Run the scripts to frontier exhaustion in one call: seed the first flow, bracket each run with the decision
   state and per-flow COW delta, and drain the frontier by WFQ order. It is a forking session stepped to the
   end, so the arguments are engine_sched_begin's, including `recipes` (the parked residue, or NULL/"") and
   `els`. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                lxb_dom_element_t **els, int n, const char *recipes);

/* Install the Level-1 eviction seam, asked at each step boundary of the one-call driver. The host decides
   whether this engine should leave memory (only it sees the other engines and the summed working set); the
   engine decides when (the next boundary with no flow switched in, where every flow's state is in its own
   blob). A non-zero answer requests the park (engine_request_park), which writes every frontier member for the
   host to store, so it decides when the residue leaves memory, never how much of it survives. A host that has
   what it came for answers it too: there is no "stop driving" seam, because a driver that merely stopped would
   leave flows suspended mid-frame with replies outstanding, which flow_release asserts against. A host that
   never evicts installs nothing. */
void engine_set_park_hook(int (*want_park)(void));

/* Work this engine has performed: forks taken, flows created, jobs run and context switches, a lifetime sum.
   Exported so a host reporting on its run uses the same cadence quantity as the engine's own progress stream. */
long engine_work_done(void);

/* How many times the scheduler's preempt policy was asked: suspend points reached, which differs from preempts
   wanted (JS_FlowPreemptStats) and rescans performed (solver/flow.h's `scanRivalRuns`). A lifetime count,
   never reset, so a rate is a difference between two censuses. It is the denominator of the hook cache's miss
   rate: flow_rival_of's one caller is that policy's rescan branch, which runs after this count is raised, and
   solver/result.c asserts `scanRivalRuns` never exceeds it. No policy reads it. */
uint64_t engine_preempt_asks(void);

/* Which key of the preempt hook's rival cache had moved when it missed; a partition of `scanRivalRuns`. The
   cache is keyed on the frontier generation or the incumbent, and the two misses call for different work: a
   generation miss is the page branching, an incumbent (`cur`) miss is a rescan over an unmoved generation.
   A cached top-two fold does not answer a `cur` miss: the carry bit (solver/flow.c's flow_silence_carry)
   reorders members with no generation bump, and flow_pick_skipped drops the excluded member before weighing,
   so the old incumbent re-enters a population the pair never held. A sound fold retains the pair over the
   full eligible set and is used only when a recomputed weight strictly exceeds the retained third best, which
   bounds every other member because weights do not increase between generations. `both` counts intervals in
   which removing either invalidator alone would still have forced the walk.
   The raise sits in the rescan branch after the key compare, under the `cur != NULL` test that decides whether
   flow_rival_of runs, and solver/result.c asserts `gen + cur + both == scanRivalRuns` at the census. One
   struct and one call so the three are one sample. Lifetime counts, raised in every build like the total they
   partition; no policy reads them. */
typedef struct {
    uint64_t gen;    /* the frontier GENERATION had moved and the incumbent had not */
    uint64_t cur;    /* the INCUMBENT had changed and the generation had not */
    uint64_t both;   /* both had moved in one interval — neither invalidator alone explains this walk */
} EngineRivalMiss;
EngineRivalMiss engine_rival_miss(void);

/* The session: the same dispatch loop, stepped by its host instead of drained, so a host with other work
   between quanta (its message port, other engines, streamed findings) gets the cooperative-quantum return and
   then resumes the byte-identical frontier. engine_run is these entries in a loop. A step answers one of three
   values and every host keeps all three apart: a yield and a stall ask for opposite things, and only the
   engine knows which it meant.
   ENGINE_STEP_DONE: the frontier is empty and the session's hooks are uninstalled.
   ENGINE_STEP_YIELD: the session is live and every flow is where it was. */
#define ENGINE_STEP_DONE   0
#define ENGINE_STEP_YIELD  2   /* the value the extension bridge's qjs_step speaks */
/* ENGINE_STEP_STALLED: every flow has run as far as it can, but some are parked on something only the host can
   supply (a reply the sandbox cannot fetch). The session stays live and parked flows keep their snapshots; the
   host supplies what is owed (engine_host_owes) and steps again. A stall is a bill: stepping again without
   paying converts nothing into work, so a driver treats an unpaid stall as its terminator, never a step
   count. */
#define ENGINE_STEP_STALLED 3
#define ENGINE_QUANTUM_MS  12  /* a thread-sharing floor, not a cap: nothing is dropped across it. A budget of CPU
                                  consumed; solver/quantum.h owns the edge that expires it. */
/* How often the quantum budget is read, in interpreter dispatches. It reaches quickjs.c as
   JSFlowControlHooks.budget_period because quickjs.c includes no host header, so the scheduler owns the one
   value. It is an occasion to read the CPU clock, never a dispatch-denominated budget: a C activation that
   declares no step boundary retires no dispatches however long it runs (the aging charge in engine.c is
   denominated in thread time for that reason). The value bounds overshoot (at most one period past the budget,
   tens of microseconds) and amortises the clock read (a call into JS on some hosts); both stay under one
   percent of ENGINE_QUANTUM_MS from about 1024 to 16384, so re-derive it against the slice. The gate counts
   down, so it need not be a power of two. */
#define ENGINE_QUANTUM_ASK_EVERY 4096
/* Dev-build diagnostic margin, in work one step performs (forks + flows created + jobs run): a step that does
   this much without consulting the preempt hook has no suspend/resume seam on that path. It is work rather
   than wall time because a wall clock cannot decide this on a loaded machine (see engine_sched_step), and it
   sits far above an ordinary step's few forks between suspend points. It truncates and drops nothing. */
#define ENGINE_SEAMLESS_WORK 1000
/* The CPU margin for a seamless stretch the work count cannot see (a C `for(;;);` forks and queues nothing):
   400 quanta of consumed CPU without offering the scheduler a seam. Asked only where quantum_measure_is_cpu()
   says the reading is CPU, since consumed CPU is what a loaded machine cannot inflate. Dev-build only; it
   truncates and drops nothing. */
#define ENGINE_SEAMLESS_CPU_US ((int64_t)ENGINE_QUANTUM_MS * 1000 * 400)
/* Whether the host owes anything: non-zero if any frontier member waits on something only the host can supply,
   the union of engine_pending_fetches and engine_host_requests read from the same registers. It is the
   scheduler's last question before calling a frontier exhausted (STALLED rather than DONE), so a host does not
   report it; a host may still ask (the fixture's park hook does). */
int engine_host_owes(void);

/* Whether every frontier member is parked on a request the trusted zone refused: outstanding to its own flow
   (so the timeline is not torn down) and owed by nobody, because the joins skip a refused entry so it is not
   re-asked. engine_host_owes answers 0 for such a frontier; this says that 0 is final. The two cannot both
   hold, since one billable member falsifies this. A settled frontier ranks last (`-inf`), and
   engine_sched_slice's close writes its residue down before the session ends. */
int engine_frontier_settled(void);

/* Mark this instance's document as one another instance holds a reference into (a WindowProxy), so its
   timelines may not run out: a peer can ask it something at any time, and a fresh flow seeded from the
   baseline would answer about a document in which none of the page's scripts ran. The last timeline reports
   itself host-owed (flow.h) instead of finishing and the session stalls instead of closing; a host-owed flow
   is out of the pick, so nothing spins, and the arriving operation resumes it. Set by the host, because
   provisioning (another agent created this instance's navigable) is invisible to the engine; it belongs to
   the instance and survives session boundaries. */
void engine_set_referenced(int referenced);

/* Install how the host pays what it owes, for the one-call driver (engine_run), which has no caller to return a
   stall to. The provider fills what engine_pending_fetches and engine_host_requests name and answers how many
   entries it filled. run_scheduler calls it after every engine_sched_step and asserts nothing was left
   outstanding, so replies settle at the next quantum as the extension bridge settles them; a stall with
   `filled == 0` ends the run, since nothing this host holds will move it. */
void engine_set_provider(int (*provide)(JSContext *ctx));

/* Begin a stepped session over `n` scripts. `forking` is the explore/verify policy engine_session_forks
   answers. `recipes` is the parked residue the host stored for this bundle (';'-joined records, solver/cold.h),
   or NULL/"" for none; it seeds the frontier instead of the boot flow, never beside it, because a resumed flow
   already re-runs the document under its recorded arms.
   `types[i]` is entry i's HTML §4.12.1.1 script type: a classic script runs in a preemptible program frame
   (JS_FlowNew) and completes with a value, a module is linked and evaluated (JS_FlowEvalModule) and completes
   with a promise, and the body cannot say which (top-level `await`).
   `bodies[i]` and `srcs[i]` are core/loader/document_scripts.h's two independent items, source text and the
   HTML §8.1.4.1 "Scripts" base URL; an already-fetched external entry passes both, and an entry with neither
   is refused. `els[i]` is entry i's `script` element, which "execute the script element" switches on and whose
   classic arm sets §3.1.7 `currentScript`; NULL for a synthesized program list, where `currentScript` stays
   null. All four arrays are borrowed for the life of the session. One session at a time (asserted). */
void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                        lxb_dom_element_t **els, int n, int forking, const char *recipes);
int  engine_sched_step(void);

/* Emit the periodic census for a host whose output is a stream of lines: @SWAP, @COLD, @HEAP, @WFQ, @FORKAT, in
 * that order, from the composers that put the same bytes on the result document. Call it after each step; it
 * decides whether a sample is due (`ENGINE_PROGRESS_EVERY` units of engine_work_done() or a new @S candidate),
 * because a cadence in work performed belongs to the engine, and an elapsed-time cadence would describe the
 * machine. A host needs it because the result document is built only when the frontier drains or stalls, which
 * a real page may never do; rising or frozen counters tell exploration from one member holding the thread.
 * Callers are run_scheduler and test_forced.c's `--abi` arm. The wasm ABI hosts read the result document
 * instead, and engine_sched_step does not emit, so a console host is not handed a record stream. A sample is
 * O(frontier members) and weighs each member twice (solver/flow.h's FLOW_SCAN_CENSUS), visible as @WFQ's
 * `scanCensusWeights` against `scanNextWeights`.
 * Returns 1 when the lines went out and 0 when not due, so a line-stream host can emit its findings
 * (main.c's `qjs_emit_partial`) at the census's instant. Ignoring the return changes nothing. */
int engine_census_emit(void);

/* End the session when the host stops stepping. Call it unconditionally where the host leaves its loop, never
 * behind `if (r != ENGINE_STEP_DONE)`. Only DONE closes a session by itself; any other exit leaves the hooks
 * installed and one flow switched in (its heap delta applied, its DOM nodes in the document, its decision
 * state in decide.c's globals), and a teardown would then release state the scheduler still holds, which
 * flow_release asserts against. Ending performs the ordinary suspend on the running flow, as a park does, so
 * the frontier left behind is a set of snapshots and nothing is dropped. After DONE it does nothing. */
void engine_sched_end(void);

/* Join a second document of this agent (`qjs_join`): its scripts run on the frontier already running. An
 * instance is an origin-keyed agent cluster, and a host-handed document is built at the baseline before any of
 * its flows exist, unlike an HTML §7.3.1.3 "Child navigables" document, which is built inside its creating
 * flow and whose scripts that flow queues (core/frame/navigable.c). So this adds one flow, the joined
 * document's boot flow (an empty decision vector over the agent baseline, ranked, forkable and parkable like
 * the root's), and returns; nothing runs here, and no second session starts.
 * `cctx` is that document's realm, where its programs compile, and is asserted to be the realm `doc` names.
 * Requires a live session and a document other than the session's (asserted).
 * `bodies`/`srcs`/`types`/`n` are the document's §4.12.1 inventory (core/loader/document_scripts.h) in
 * document order, copied, because the host read them once and nothing owns them past this call. `els[i]` is
 * entry i's `script` element, borrowed, because it names a node of the handed-over tree, which outlives the
 * rows. */
void engine_join_document(JSContext *cctx, uint32_t doc, char **bodies, char **srcs,
                          const ScriptType *types, lxb_dom_element_t **els, int n);
/* Request the RAM-to-disk park. The host sees the pressure; the engine takes it at the next step boundary with
   no flow switched in, writes the park document (cold_park_json, published in the result) and answers
   ENGINE_STEP_DONE, and the flows' memory goes with the instance the host then tears down. Nothing is dropped:
   the residue comes back through the same admission step it left by. Requires a live session (asserted). */
void engine_request_park(void);
/* Whether this whole frontier was written out rather than drained. The instance teardown's asserts (main.c)
   read it: owed replies and outstanding synchronous requests are re-issued by each recipe's replay, so neither
   is a dropped work item. It is engine-wide; a single flow's release asks the flow's own `paged`
   (solver/flow.h), because a partial self-park writes a tail while the rest keeps running. */
int  engine_frontier_paged(void);

/* Take one reply that was owed to a flow this engine paged out, consuming it (a take, not a predicate). The
   partial self-park sells the cheapest member at the RAM floor, often a blocked flow whose recipe re-issues
   its request next session, so the reply the host was already fetching lands with nobody parked on it. That
   is a sale, not the mispairing the provide edge asserts against, and this tells the two apart. */
int  engine_take_paged_owed(void);

/* Park the running flow on a fetch only the trusted host can make. The reply arrives later through
   engine_provide, and until then the flow cannot finish, which keeps reply-gated code reachable. The park is
   on the flow's own register because the reaction the resolve enqueues belongs to that flow and its delta. */
void engine_pending_fetch_url(JSContext *ctx, JSValueConst resolve, JSValueConst value,
                              const FetchRequest *req);
/* The same park for a dynamic `import()`, which is owed source text: the drain settles `resolve` with the
   reply's body, not with the reply record `fetch()` builds a Response from. A failed load settles `reject`:
   HTML §8.1.6.7.3 "HostLoadImportedModule(referrer, moduleRequest, loadState, payload)" makes a null
   moduleScript "ThrowCompletion(a new TypeError)", and ECMAScript §13.3.10.3 "ContinueDynamicImport (
   promiseCapability, moduleCompletion )" calls "promiseCapability.[[Reject]]" with it. */
void engine_pending_module_url(JSContext *ctx, JSValueConst resolve, JSValueConst reject, const char *url);
/* The same park for a browser algorithm's own subresource fetch: HTML §4.2.4.3 "Fetching and processing a
   resource from a link element"'s built link types and HTML §4.8.4.3.5 "Updating the image data". The reply
   record goes to that algorithm's completion steps and nothing is queued as a program (solver/pending.h's
   FLOW_PENDING_RESOURCE): HTML §4.6.8.20 Link type "preload" fills the preload cache, HTML §4.6.8.12 Link type
   "modulepreload" fills the module map "already ready (but not evaluated)", and §4.8.4.3.5 decodes an image.
   `deliver` is called for a failure too, because HTML §4.6.8.12 step 14.1's `error` event and §4.6.8.20's
   network-error branch are the failure arm, and they live at the element. */
void engine_pending_resource_url(JSContext *ctx, JSValueConst deliver, const FetchRequest *req);
/* The frontier's best weight, by which the host ranks this engine against every other live one. Level 1 and
   level 2 are one policy, so this is flow_weight of the flow the scheduler would pick (flow_next_to_run with
   no incumbent). It is -inf when no member can be handed the thread, for an empty frontier and a fully
   host-owed one alike, since neither can convert a slice into work. It reads runnable members, not flow_best:
   host-owed marks last until the host does something that could answer them, and a stall code says "pay me",
   not what this engine is worth; ranking a blocked engine by an unrunnable flow's unaging weight would keep it
   in the hot pool forever. -inf is the value extension/mojo.js declares the Level-1 input carries, so a
   stalled engine sorts last through the existing order. */
double engine_top_weight(void);
/* The value yield's floor (level 1): the weight of the runner-up engine's best flow. The running flow hands the
   thread back as soon as this engine's best no longer outranks it, because the other document's work is then
   worth more. It orders only: nothing is dropped and the frontier resumes exactly where it was, which is what
   separates a yield from a cap. -inf (the default) means the host named no rival, so only the cooperative
   quantum yields. Declared twice below; both name the one definition. */
void engine_set_yield_floor(double w);

void engine_set_yield_floor(double floor);
/* Issue a synchronous request only the host can answer (see engine.c) and return its id; the asking step
   machine returns JS_STEP_YIELD and the flow suspends until the answer lands while siblings run. The id is the
   rendezvous, never the request text: the answer is computed under the asking flow's world, so two identical
   questions from two flows have two answers. */
uint32_t engine_host_request(JSContext *ctx, const char *op);
/* Whether request `req` has been answered. `*out` is borrowed, so a machine re-entered before it is ready to
   consume may read it again. It reports the answer's arrival, whatever its completion type. */
int      engine_host_answered(uint32_t req, JSValueConst *out);

/* An answer is a completion (ECMA-262 6.2.4), not a value: a peer resolves a cross-instance operation by
 * running a program, which may throw, and a channel without the type would deliver a throw as `undefined` and
 * skip the asker's `catch`. So every delivery and take states the type. A thrown Error crosses as a name
 * (remote_object.h) like any object. */
enum { ENGINE_COMPLETION_NORMAL, ENGINE_COMPLETION_THROW };

/* Take the answer, removing the request from the register. The caller owns the returned value, and
   `*pcompletion` (required) says whether it is a result or a thrown value to re-raise. */
JSValue  engine_host_take(JSContext *ctx, uint32_t req, int *pcompletion);
/* Take the answer as the completion it is, for a cross-instance step machine: a normal value goes to
   `*presult` and returns JS_STEP_DONE; a throw is re-raised in the asking flow at the parked call site, as a
   local operation would raise it, and returns JS_STEP_ABRUPT (quickjs-step.h). */
int      engine_host_take_completion(JSContext *ctx, uint32_t req, JSValue *presult);

/* Withdraw a rendezvous: Fetch §2 Infrastructure's "To terminate a fetch controller controller, set
 * controller's state to 'terminated'", for XHR §3.5.1 "The open() method" step 10 and XHR §3.2 "Garbage
 * collection". A flow with an unanswered synchronous request is blocked (pending_blocked) until a host event,
 * so a destroyed machine's entry would leave the flow blocked forever by nobody; this removes the entry and
 * runs flow_clear_host_owed, so the flow is askable on the next pick with its snapshot untouched.
 *   It is terminate, not abort: terminate carries no error, and at these callers nothing is left to catch one.
 * The zone is asked to stop the transfer with a `hostreq.terminate<TAB><id>` notice; a late answer to a
 * withdrawn id finds no register and engine_host_answer returns 0. It answers nothing, since no caller can yet
 * act on whether the id was live; withdrawals are counted (EngineFrontierCensus's host_terminated).
 *   Named residual: not covered is "abort a fetch controller", whose error is delivered (XHR §3.5.7 "The
 * abort() method" step 1; Fetch §5.6 "Fetch methods"); the next diff builds `engine_host_abort(ctx, req,
 * reason)` answering with ENGINE_COMPLETION_THROW, which needs the id reachable from the aborted object and a
 * `signal` on `fetch()`; its absence shows as `xhr.abort()` mid-flight firing abort/loadend twice, once more
 * when the unstopped reply lands. */
void     engine_host_terminate(JSContext *ctx, uint32_t req);

/* Who computed an answer; a parameter because the two have different multiplicities. A host answer is a value
 * the trusted zone computed itself (a load, an XHR fetch), so there is exactly one, and a second is the zone's
 * bug. A peer answer is a completion one of another instance's timelines produced by running a program, so one
 * question has N true answers, and the asking flow forks one arm per distinct answer (engine.c). The caller
 * states it because sniffing the op text would be a recognizer. */
enum { ENGINE_ANSWER_HOST, ENGINE_ANSWER_PEER };

/* Deliver an answer, routed by id to one call site (never broadcast like a fetched body). Returns 0 when the
   asking flow is gone, which means nobody is waiting. `world` names the timeline that computed the answer in
   world_serialize's grammar: required for ENGINE_ANSWER_PEER, so N true answers are told apart from one answer
   relayed twice, and NULL for ENGINE_ANSWER_HOST, which has no flow to name. */
int      engine_host_answer(JSContext *ctx, uint32_t req, const char *world, JSValueConst value, int completion,
                            int source);
/* What the host still owes, as `id<TAB>op` lines. Pulled each step and not deduped (see
   engine_retract_operations). */
const char *engine_host_requests(void);

/* Emit a one-way notice to the host: never answered, so never a suspend. HTML §4.8.5 creates a child navigable
   inside the insertion steps, which cannot ask anything, so its name is minted here (world.h) and the host is
   told. A notice is immutable once sent, so nothing un-sends it when the flow parks or is outranked, and it
   needs no COW capture. */
void        engine_host_notify(JSContext *ctx, const char *op);
/* Drain the notices posted since the last call, newline-joined; "" when there are none. */
const char *engine_host_notices(void);

/* Build a TAB-delimited notice record from `op` and its fields, sized exactly; the caller frees. It replaces a
   format string beside a hand-computed slack constant, which drift apart and let snprintf truncate silently.
   `nfields` is the caller's `sizeof a / sizeof a[0]`, never a NULL terminator, so a field added to the array
   is counted by the same edit (a variadic list missing its terminator is undefined behaviour the optimiser
   exploits). The last field is the remainder and the only one that may hold HTAB, since a raw CSP header may
   (RFC 9110 §5.5 "Field Values"); a tab in an earlier field would shift every later field past a reader's
   count, so that is asserted here, as is a newline in any field. */
char       *engine_notice_build(const char *op, const char *const *fields, size_t nfields);

/* Announce the death of worlds: one `world.gone<TAB><world name>` notice per name, the one writer of that
   record. Names come from world_flow_gone (a flow left the frontier) or world_session_gone (the whole frontier
   parked); both are lists because a death frees every ancestor whose last live descendant it was. It is a
   notice because nothing waits on it. The trusted zone broadcasts it to every other instance: the sender does
   not track which peers a flow reached, since releasing a world with no segment is a no-op (world.h). */
void        engine_notify_worlds_gone(JSContext *ctx, const char *const *names, int n);

/* Announce, at the park, each foreign world segment the residue carries: one `world.parked<TAB><world vector>`
 * notice per segment. A `world.gone` is broadcast only to live instances, so a death announced while this
 * document is cold would miss it and the resuming instance would hold a dead world's segment for the rest of
 * its process. The trusted zone, which knows the instance is parked, holds deaths for exactly the worlds this
 * record names. */
void        engine_notify_worlds_parked(JSContext *ctx, const char *const *vectors, int n);

/* Hand back every cross-agent operation this instance was asked: one `remoteop.retracted<TAB><token>` notice
 * per distinct token, taken at the park before the residue is written, so no member owes an answer when it
 * returns and the park's asserts stay at full strength. A started operation is handed back like a queued one:
 * its partial work is the parked flow's own COW delta, and what the peer is owed is a call (HTML §7.2.1.3.5
 * "CrossOriginGet ( O, P, Receiver )" ends "Return ? Call(getter, Receiver)"), which the re-ask makes once.
 *   The notice belongs to the last holder leaving: an operation is attached to every live timeline
 * (engine_perform), and a notice sent while another timeline holds it would make the zone forget a token that
 * timeline is about to answer under.
 *   It is returned rather than parked because a token is minted by the zone, names an entry in its in-memory
 * routing table and has no generation, so it does not outlive the zone session, while a residue does. Within
 * the session the asker is still suspended and the zone re-asks; after a restart the asker's own recipe
 * re-issues the request. The zone suppresses re-asking a carried request (an operation has the page's side
 * effects); this notice lifts that, and engine_host_requests, which does not dedupe, reports it again. */
void        engine_retract_operations(JSContext *ctx);

/* What the hand-back did, as lifetime counts taken at the retraction so they agree with the notices sent.
   `flows` is how many members held a question, `started` how many program rows were returned, `handed_back`
   how many notices left (distinct questions, not holders). `handed_back` equal to `flows` on a forked frontier
   means the last-holder rule is not being applied. */
void        engine_retract_census(long *flows, long *started, long *handed_back);

/* How many program rows on the frontier still carry a peer's rendezvous token (a gauge); engine_retract_census's
 * `started` is the same number after the fact. Zero means every question this instance was asked is still
 * queued on an arrival slot or answered. flow_perform appends the operation's program to a runnable row, so
 * the started state survives a slice only when the slice ends at the CPU quantum or the level-1 yield floor;
 * a single-document host whose slices end only at a stall or exhaustion always reads 0 between slices. */
long        engine_operations_started(void);

/* Inbound: a peer says one of its worlds is gone, so the segment this instance holds for it can go. It seeds
   no work. */
void engine_world_gone(JSContext *ctx, const char *world);
/* Inbound: a record another instance emitted as a notice, routed here by the trusted zone because this
   instance holds the document it names, with the sender's origin stamped by that zone (SECURITY.md: the
   untrusted engine may not compute it). It becomes a work item of every live timeline of the receiving
   document, and each flow delivers it in its own world when next run, because the page's `message` listener
   lives in the delta of the flow that registered it. The sender's world contributes its segment in this
   instance (engine.c states the conjunction and crashes on the part not yet built). The frontier is the only
   inbound queue. */
void engine_route(JSContext *ctx, const char *record, const char *sender_origin);

/* Put a routed record the cold tier rebuilt back on flow `f`'s delivery queue, registering its arrival on the
   ledger (engine_routed_census's `zero_delivery`, keyed on record text) in the same call. The pair is one entry
   so it cannot be separated: an unregistered push would fire `routed_rec_admitted`'s abort a session later. It
   registers before it attaches, as engine_route does, so a record whose attach aborts still counts. A rebuilt
   record is a new arrival: the ledger is process-lifetime and a park crosses a process, so a residue resumed
   and never delivered is a loss this census states. */
void engine_routed_rebuilt(JSContext *ctx, struct Flow *f, const char *record, const char *sender_origin);

/* Inbound: a cross-agent operation (core/frame/remote_op.h) another instance's flow is parked on, routed here
   because this instance holds the document it names. It is attached to every live timeline, because one
   question has an answer per timeline. Each flow performs it as its own next program (a peer answers by
   running one, never by reading a property from C) and emits the completion as a notice naming `token`, which
   the zone routes back to the asker. `token` is the zone's rendezvous, opaque here, since a request id is
   unique only inside the instance that minted it. Nothing runs inside this call. */
void engine_perform(JSContext *ctx, const char *token, const char *record);

/* What the ask side did, as lifetime counts. `asks` is how many cross-agent operations reached this instance,
 * raised before any check can refuse them; `attached` is how many (operation, timeline) pairs engine_perform
 * made. Together they split a zero `flows` in engine_retract_census three ways: nothing arrived; every live
 * timeline contradicted the addressee; or every holder left before the park. `flows <= attached` is asserted
 * in test_forced.c's park ladder. `attached` and `asks` have no order, since one arrival attaches to every
 * live timeline. */
void        engine_perform_census(long *asks, long *attached);

/* Which timeline of `doc_name` flow `f` has already committed to: the addressee a cross-agent operation
 * carries, read from the commitment record (flow.h). Heap; the caller frees. NULL means the flow addresses
 * nobody, which is common and positive. The asker states it because the receiver holds the flows but not the
 * commitment: once a flow takes a peer answer it is in that sending world (engine.c's answer_commit_taken), so
 * its next operation is a question of that timeline.
 *   Received rows for one document form a chain (flow.c's push aborts on a contradicting pair), and the
 * deepest row is used: a shallower one would admit both arms of a branch the flow already took, and the first
 * row would depend on push order. Independent rows (different generations, as a park replay produces) have no
 * meaningful order, so the answer is then NULL.
 *   Named residual: not covered is a target that is a same-origin child document of the peer agent, because
 * rows carry the agent's root document name (`mint` stamps `g_doc`), so such reads go unaddressed; the next
 * diff records the document a read was answered for beside the row's vector, a solver/cold.c 'r' grammar
 * change; its absence shows as the timelines answering one token equalling the peer's whole live frontier for
 * reads naming a non-root document but a strict subset for the root. An unmatched key only loses narrowing: a
 * wrong-agent addressee refuses nothing (world.h's world_vec_relate_held). */
char *engine_flow_addressee(JSContext *ctx, struct Flow *f, const char *doc_name);

/* The spelled absence of an addressee: "this flow addresses nobody". It cannot be read as a vector, because
   world.c writes every head as `<name>:<generation>:<serial>` and this has no colon. */
#define ENGINE_ADDRESSEE_NONE "-"

/* Inbound from the browser: document `doc` is no longer its navigable's active document, because the real
   browser navigated that navigable. This is HTML §7.4.6.1 "Updating the traversable"'s deactivate a document
   for a cross-document navigation, not §7.3.1.6 "Navigable destruction"; the two meet at §7.5.9/§7.5.10, so
   one machine serves both (core/frame/document_lifecycle.h). It is attached to every live timeline, because the
   `pagehide`/`unload` listeners, active timers and browsing context belong to the timeline that made them.
   Nothing runs here and no flow is dropped. The incoming document is not taken: §7.5.9 "Unloading documents"
   step 6 queues on the outgoing document's global, and `newDocument` only feeds unload timing info this user
   agent does not carry; a cross-origin incoming document is a peer instance's anyway. */
void engine_unload_document(uint32_t doc);

/* Who asked for a request, a fact about the park: HTML §4.12.1.1 "Processing model" gives a parser-inserted
 * script a parser document, so `parser` names bytes the zone itself fetched, and every other park is made by
 * running code. It is one of the two facts the provenance is composed from (solver/pending.h's PROV_*, with
 * solver/flow.h's `path_forced`). It is read off the record (pending.h's `parserIns`, stamped from what the
 * inserting component stated), never derived from the park's kind, which answers wrongly both ways: the
 * in-order list holds an element whose `async` setter cleared `force async`, and the as-soon-as-possible set
 * holds a parser-inserted `<script async src>`. Equal token length is not relied on: the join shifts the field
 * (engine_pending_fetches' join_set_tokens). */
#define PENDING_INITIATOR_PARSER "parser"   /* HTML §4.12.1.1's parser-inserted script of the loaded document */
#define PENDING_INITIATOR_SCRIPT "script"   /* a park made by running code: fetch(), import(), an injected src */

/* What a request is evidence of, spelled on the wire; the values and composition are solver/pending.h's PROV_*,
 * which the park stamps. The engine states it and the trusted zone (`safeFetch`) decides; a branch here that
 * refused to list a park would put network policy in the engine and leave the flow waiting silently. A
 * deduped set states the most observed of its members, since it is one request and any clean member makes it
 * real (argued at the join). */
#define PENDING_PROVENANCE_OBSERVED "observed" /* a real load of this document makes exactly this request */
#define PENDING_PROVENANCE_DERIVED  "derived"  /* the page's own code computed it from real inputs */
#define PENDING_PROVENANCE_FORCED   "forced"   /* a value in it exists only because a gate was forced */

/* Whether the address may rest on a witness this engine chose: a second field, not a fourth provenance word,
 * because provenance grades what a reply is worth while this decides whether the act may be spent, and a
 * fourth word would hide `forced` from every consumer that reads it. `pinned` means the parking flow, before
 * building the request, determined some source's value on an arm its concrete example contradicts
 * (solver/flow.h's `path_pinned`), after which concretize-on-pin answers reads with the chosen spelling.
 * `unpinned` means every byte came from the document, the server or the page's own text.
 *   It is a property of the path, a may-rest-on: a pinned read yields a bare primitive (`pin_mint`,
 * solver/concolic.c) and concolic_add_hook derives nothing from bare operands, so tracing bytes into the URL
 * would need a taint tracker, which the design bans. A deduped set states `pinned` if any member's path was,
 * the opposite direction to provenance, because under-claiming here fires an act. */
#define PENDING_PINNED_YES "pinned"     /* the path determined a witness before building this address */
#define PENDING_PINNED_NO  "unpinned"   /* it had not: every byte of the address came from outside this engine */

/* The provenance word for a request built by running the page's code rather than by a park: a navigation
 * (core/frame/navigable.c's §7.4.5 "Populating a session history entry" load and `navigable.create`) or a route
 * declaration (solver/route_seed.c). One composition in one place, read off the running flow.
 *   It answers `derived` or `forced`, never `observed`: `observed`'s first conjunct is HTML §4.12.1.1's parser
 * document, which only a park record carries, and no element is in scope where code runs. With no running flow
 * the answer is `derived`, which under-claims (costing a per-origin authorisation) rather than over-claims
 * into the observed pool; a caller for which a flow-less act is a broken invariant asserts it (route_seed.c
 * does). The engine states; `extension/lib/safe-fetch.js` and its callers decide.
 *   Named residual: not covered is a child navigable whose `<iframe src>` came from parsing bytes the zone
 * fetched, which should be `observed` and is answered `derived`; the next diff threads the parser-inserted bit
 * as a parameter from core/html/html_iframe.c's `iframe_document_parsed` through `iframe_create_navigable`,
 * `navigable_create` and `navigable_load_enqueue`; its absence shows when a host first treats `observed` and
 * `derived` navigations differently, as every parser-inserted child navigable landing on the wrong side. */
const char *engine_provenance_of_running_path(void);

/* The same answer as a number, for solver/endpoint.c, which stores and compares grades. The string form above
 * is engine_provenance_token of this, so the two cannot disagree. */
int engine_prov_of_running_path(void);

/* The provenance's wire spelling, shared by the pending line and the @H record so the three words are one
 * vocabulary; a second mapping could drift toward the stronger word. An unknown value is fatal (CHECK), since
 * a release fall-through would print garbage. */
const char *engine_provenance_token(int prov);

/* …AND THE SAME TWO WORDS FOR THE WITNESS MARK, FOR AN ACT THAT IS NOT A PARK. `PENDING_PINNED_*` above is
 * composed at a park off the parking flow's own `flow_path_pinned`; this is the SAME fact about the path that
 * is STANDING, for every request this engine builds by RUNNING THE PAGE'S CODE — core/xhr/xml_http_request.c's
 * §3.5.6 "The send() method" request is the caller it was written for, which had no witness mark at all while
 * the pending line beside it carried one, and the trusted zone's firing decision was told the fact does not
 * travel for a population of requests that is exactly what a forced arm produces.
 *
 * IT READS THE FLOW ITSELF rather than taking one, for `engine_prov_of_running_path`'s reason word for word: a
 * caller that passed a flow could pass a different one, and the fact wanted is about the path standing HERE.
 *
 * THE FLOW-LESS ANSWER IS `unpinned` AND IT IS NOT A CHOICE — IT IS FORCED BY THE NESTING, WHICH IS WORTH
 * STATING BECAUSE THE NEXT READER WILL REACH FOR THE OTHER WORD AND BE RIGHT TO. `pinned` is the SAFE word
 * everywhere else this field is composed: it is a MAY-REST-ON, and a deduped set states `pinned` if ANY
 * member's path did, because under-claiming HERE fires an act where under-claiming a provenance merely grades
 * a reply. Reasoning from that alone gives `pinned` for an act with no path standing, and that pair is
 * ILLEGAL: `engine_prov_of_running_path` answers `derived` on the same state, `path_pinned` is strictly
 * nested inside `path_forced`, and extension/lib/safe-fetch.js's `_firingRefusal` CHECKs exactly that pair —
 * fatal in RELEASE. So the two accessors' flow-less arms are one decision and not two, and the witness's is
 * decided by the provenance's. What makes that safe is that the state is a BROKEN INVARIANT at every caller
 * rather than a case: each one composes a request by running the page's code, the DCHECK below names it, and
 * `unpinned` alone fires nothing — safe-fetch.js's value arm additionally wants an actor of `page` and an
 * origin a person widened for that row.
 *
 * THE ENGINE STATES AND THE ZONE DECIDES, and the zone's THIRD word stays the zone's: `unstated` is what a
 * reader of a record carrying no mark composes for itself, which is how an artifact older than the key reads
 * as a fact about the BUILD instead of making a key absent. Nothing here ever spells it. */
int engine_pinned_of_running_path(void);

/* THE WITNESS MARK'S WIRE SPELLING — `engine_provenance_token`'s sibling, exported for the same reason and
 * added when the same premise died. This mapping stood as an inline ternary at the two places the pending
 * line's own join writes the field, on the argument that a static with one caller stays a static; a THIRD
 * site had to ask it the moment a request that is not a park carried the mark, and two files spelling one
 * two-word vocabulary is the copy that drifts in the direction that costs — a record this engine calls
 * `unpinned` and another file calls `pinned` is read as whichever the consumer saw first, and the consumer is
 * a firing decision. Fatal and never a DCHECK for `engine_provenance_token`'s reason exactly. */
const char *engine_pinned_token(int pinned);

/* WHAT THE BYTES ARE FOR, WHICH IS A DIFFERENT QUESTION FROM WHO ASKED — Fetch §2.2.5 "Requests"' DESTINATION,
 * stated verbatim off the request record the park carried (core/fetch/fetch.h) and never derived here.
 * THE TWO FIELDS ARE NOT TWO SPELLINGS OF ONE FACT, and reading them as one is what left a live hole. The
 * INITIATOR is HTML §4.12.1.1 "Processing model"'s `parser document` and says whether a REAL LOAD of this
 * document makes this request; the DESTINATION says whether the reply may be ingested as CODE. An injected
 * `<script src>`, a dynamic `import()` and a plain `fetch()` all report `script` as initiators — they are all
 * parks made by running code — and the first two are code loads while the third is not, so the initiator can
 * never answer the CORB question and a zone that asked it anyway got the answer right for one of the three.
 * ITS VOCABULARY IS THE SPEC'S AND NOT THIS ENGINE'S, which is the point: §2.2.5's destination type is one of
 * "", "audio", "audioworklet", "document", "embed", "font", "frame", "iframe", "image", "json", "manifest",
 * "object", "paintworklet", "report", "script", "serviceworker", "sharedworker", "style", "text", "track",
 * "video", "webidentity", "worker" or "xslt", and this seam carries whichever one the request has rather than
 * a two-valued summary of it — a `<link rel=preload as=font>` says `font` because that is what it is. The
 * EMPTY STRING is a value and not an omission: §2.2.5's "unless stated otherwise it is the empty string" is
 * what `fetch()` and XMLHttpRequest have, so an empty field on the line is the positive statement "data".
 * THE CONSUMER READS IT FOR §2.2.5's SCRIPT-LIKE PREDICATE — "audioworklet", "paintworklet", "script",
 * "serviceworker", "sharedworker" or "worker" — and that predicate is the CORB class. Anything else is data.
 * A THIRD PARK KIND, OR A NEW DESTINATION, THEREFORE COSTS NOTHING HERE AND CRASHES AT THE PRODUCER: the join
 * asserts the value is a destination type, so a park that states something outside the enumeration stops
 * rather than travelling to a zone that would read it as "not script-like" and ingest its reply as data. */
/* THE ONE TOKEN THIS FILE'S OWN PARKS EMIT, and the only one declared. §8.1.4.2 "Fetching scripts"' classic
   and module script fetches all create their request with `script`, and the three script parks below are in
   the solver, so they name it through this. Every OTHER destination is stated by the browser component whose
   own algorithm names it — `image` at HTML §4.8.4.3.5's potential-CORS request, `document` at a navigation,
   the EMPTY STRING at `fetch()` and XMLHttpRequest — as the literal that algorithm's step contains, which is
   where a citation can be checked against the text beside it. A macro for a value this file never writes would
   be a vocabulary entry with no producer here. */
#define PENDING_DESTINATION_SCRIPT "script"
/* THE ENUMERATION AND THE SCRIPT-LIKE PREDICATE ARE BOTH STATICS OF solver/engine.c AND NEITHER IS EXPORTED,
   for `method_is_token`'s reason: every use either has is inside that file. The enumeration answers two
   asserts (the join refuses to WRITE a value §2.2.5 does not define; the split refuses to BELIEVE one), and
   script-like answers the join's FOLD — a deduped set states the destination of its strictest member, because
   one reply satisfies every park in it. That fold is not a policy: it decides what the LINE says, never what
   is fetched. The CORB DECISION itself is the trusted zone's alone — the engine holds no network policy by
   construction — and asks the same §2.2.5 predicate once more, in `extension/lib/safe-fetch.js`, over the
   bytes it actually read. The two are the same question asked by the two parties that each have to answer it,
   which is not a duplicated table: neither party can take the other's answer, since the engine has no bytes
   and the zone has no register. */

/* WHAT THE HOST STILL OWES THE FRONTIER'S NETWORK PARKS — one
 * `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` line per outstanding
 * request, newline-terminated, "" for none, DEDUPED BY THE PAIR.
 * THIS SENTENCE NAMED FOUR FIELDS AFTER THE PROVENANCE BECAME THE FIFTH, which is the ordinary way a grammar
 * stated in prose beside the function that joins it goes wrong: every reader of the LINE was updated and the
 * one-line description of it was not. engine_pending_split below is the authority on the shape — it is what
 * every host takes the line apart with — and this is its restatement rather than a second grammar.
 *
 * THE METHOD IS PART OF THE REQUEST'S IDENTITY, and this seam used to answer an ADDRESS ALONE. The register
 * has carried the method since the day it carried the whole request (PEND_METHOD), and it was dropped at
 * exactly these two edges: the join listed URLs and engine_provide filled every entry naming the URL. So a page
 * that issues a GET and a POST to one address had them collect each other's bodies — not a missing feature, a
 * WRONG ANSWER, and every @H example value, every branch that reads that body and every @S verdict on that path
 * was derived from a response the page never received. It is the same defect the XHR path was corrected for
 * (SECURITY.md §Network: "a wrong answer, which is worse than an absent one"), one seam over.
 *
 * THE DESTINATION IS ON IT FOR THE SAME REASON THE METHOD IS, and it arrived by the same route: this seam
 * answered the CORB question out of a SIDE LIST that one producer filled — the module loader's chunk register,
 * which named dynamic `import()` targets and nothing else — so a document's own `<script src>` reached the
 * chokepoint with no load class at all and a cross-origin HTML or JSON body served for it was ingested as data
 * and then COMPILED. A list filled by one caller cannot answer for the others, and nothing about it could say
 * so; the destination is a property of the REQUEST (Fetch §2.2.5), every park states it, and the side list is
 * gone rather than kept beside this one.
 *
 * THE CREDENTIALS MODE IS ON IT FOR THE METHOD'S AND THE DESTINATION'S REASON EXACTLY, and it is the field
 * that says WHOSE SESSION PAYS. Fetch §2.2.5 "Requests" gives every request one and only the algorithm that
 * CREATED the request knows which — HTML §2.5.1 "Terminology"'s create a potential-CORS request for an
 * `<img>`, §2.5.4 "CORS settings attributes"' CORS settings attribute credentials mode for a `<script src>`,
 * XHR §3.5.6 "The send() method" from `withCredentials`, Fetch §5.4 "Request class" from `RequestInit` — and
 * those algorithms DISAGREE, so there is no value a consumer could supply that is right for more than one of
 * them. It crosses as §2.2.5's own THREE-VALUED token and never as a boolean, because `same-origin` is a
 * CONDITIONAL answer whose condition is the SOP question SECURITY.md gives to the trusted zone and to nothing
 * else: an engine that collapsed the three to "does this carry cookies" would have had to answer it in the
 * half that holds no network policy. The engine STATES; extension/lib/safe-fetch.js DECIDES, and derives its
 * own boolean there from this token and from its own willingness to spend the session.
 * ITS SPELLING IS core/fetch/fetch.h's `fetch_credentials_token` AND NOBODY ELSE'S — that file says so in as
 * many words, and it is why neither the join, the split nor this header restates the three words.
 *
 * WHY A TAB, AND WHY THAT IS NOT AN INVENTED DELIMITER. No field can contain one. A serialized URL cannot:
 * URL Standard §4.4 URL parsing removes all ASCII tab or newline from its input before anything else, so no
 * URL record can hold one and no serialization can produce one. A method cannot: Fetch §2.2.1 Methods says a
 * method "is a byte sequence that matches the method token production", and RFC 9110 §5.6.2 Tokens excludes
 * HTAB from tchar. A destination cannot: §2.2.5 ENUMERATES its values, and every one of them is ASCII
 * lowercase letters. The join ASSERTS all three rather than trusting them, and it is the same shape
 * engine_host_requests already answers in (`id<TAB>op`) — one seam, one grammar.
 *
 * The buffer is this function's and is valid until the next call. */
const char *engine_pending_fetches(void);
/* ONE LINE, SPLIT WHERE IT WAS JOINED — because three hosts each deriving the pair is three places to get it
   wrong, which is the hand-copy 59d0e42d abolished. `line` is the host's own mutable copy of one line (no
   newline); each TAB is overwritten with a NUL and the six fields are handed back pointing into it.
   THE DESTINATION, THE INITIATOR, THE PROVENANCE, THE PINNED MARK AND THE CREDENTIALS MODE ARE OUT-PARAMETERS
   AND NONE IS
   OPTIONAL, deliberately: a
   host that did not want one could pass NULL and would then be a host reading a request whose LOAD CLASS — or
   whose PROVENANCE — it never asked about, which is the defaulted-field defect wearing a convenience. Two of
   them are sharp in different ways: a host that skips the load class fetches a script as data, which is the
   state that field was added to end; a host that skips the provenance fires a request no client makes and
   carries its reply as an observation, which is what CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE forbids in as
   many words; and a host that skips the CREDENTIALS MODE decides whose session pays for the fetch by
   silence, which sends a `<link rel=preload>` that stated `include` out uncredentialed and learns a
   personalised body as the logged-out one — or, in the other direction, spends the person's cookies on a
   park whose own algorithm said `omit`. It costs a caller four locals and four membership asserts. */
void engine_pending_split(char *line, const char **method, const char **destination,
                          const char **initiator, const char **provenance, const char **pinned,
                          const char **credentials, const char **url);
/* DELIVER A BODY FOR ONE REQUEST — keyed on `(method, url)`, which is what the flow parked on. Returns how many
   entries it filled; 0 with nothing matched is the host's pairing being off (or a sale — engine_take_paged_owed),
   and it is the CALLER that tells those apart because the caller owns the credit. */
int engine_provide(JSContext *ctx, const char *method, const char *url, JSValueConst value);

/* REFUSE ONE REQUEST — the same `(method, url)` pair, because a refusal is an answer to the same question, and
 * it returns how many records it newly refused (0 with nothing matched is the host's pairing being off, or a
 * sale, and this function tells those two apart itself).
 *
 * IT IS NOT A NETWORK ERROR AND MUST NEVER BE SPELLED AS ONE. A refusal a REAL BROWSER also makes — a blocked
 * scheme (Fetch §4.3 "Scheme fetch" ends its switch "Return a network error"), a §4.10 "CORS check" failure,
 * a CORB-blocked body — comes through engine_provide as Fetch §5.6 "Fetch methods"' network error, which is
 * the FIDELITY. This entry is for the other kind: a refusal only THIS TOOL makes, where no browser refuses
 * anything and there is therefore no fact about the origin to relay. Handing the flow §5.6's error for one of
 * those tells it the server was unreachable for a request nobody sent, and every branch under the page's
 * `catch` is then explored on an observation that does not exist — and it destroys the property that makes
 * the per-origin widening mean anything, since a flow that has already run its failure path cannot fire the
 * day the origin is widened. The trusted zone grades its own refusal on exactly that axis and states which
 * one it is (extension/lib/safe-fetch.js); a host that re-derived the grade could only ever answer for the
 * rule its re-derivation happened to know about.
 *
 * WHAT IT COSTS AND WHAT PAYS FOR IT. A park alone explores NEITHER arm of `fetch(u).then(ok).catch(err)`,
 * and a declined request is precisely an unconstrained outcome — so this refusal makes the flow FORK: one arm
 * goes on waiting (the success arm, holding no invented reply), the other takes §5.6's network error and runs
 * the page's error path, with its own path marked FORCED so every value it learns carries the weakest grade
 * this vocabulary has. flow_decline_fork builds that pair; this only records the fact, because an arm minted
 * between scheduler steps would clone whichever flow the scheduler last ran.
 *
 * `reason` IS THE ZONE'S OWN WORDS and is copied. It is the only account anybody gets of a request this tool
 * chose not to make, and it is what tells a reader whether a widening would change the answer. */
int engine_decline(JSContext *ctx, const char *method, const char *url, const char *reason);

/* THE SAME REFUSAL FOR A SYNCHRONOUS REQUEST, KEYED ON THE RENDEZVOUS AND NOT ON AN ADDRESS — because the one
 * above provably cannot reach one, and the first thing that needs it is a §7.4 NAVIGATION.
 *
 * `engine_decline` finds its records through `pending_index_find(method, url)`, and `pending_push` deliberately
 * tracks every kind BUT `FLOW_PENDING_HOSTREQ` into that pair index. So a declined navigation matched NOTHING:
 * the refusal was recorded for nobody, the flow stayed parked for the session, and `flow_decline_fork` never
 * saw a record to fork. That is not a gap in the index — a synchronous request has no (method, url) to be
 * keyed by, since its whole identity is the rendezvous id the asking machine holds — so the door is keyed on
 * what the host already has: `qjs_host_requests` answers `id<TAB>op`, and the id is what comes back.
 *
 * IT IS A SECOND ENTRY AND NOT A THIRD `completion`, BY THE TEST `qjs_host_answer` ITSELF APPLIES. That entry
 * makes the completion a PARAMETER rather than a second entry point because a return and a throw are two
 * completions of ONE call — ECMA-262 6.2.4 has exactly those, which is what its own DCHECK says. A decline is
 * not a completion of the operation at all: the operation did not happen. Widening that enum would let a
 * refusal arrive at `engine_host_take` as a value the asking machine consumes, which is the state the assert
 * in `flow_decline_fork` exists to refuse.
 *
 * EXACTLY ONE FLOW'S REGISTER CAN NAME THE ID, which is what makes the walk stop at the first match: an
 * unanswered synchronous request is the one record a fork does NOT share (`engine_sibling_assemble` unshares it
 * and mints a fresh rendezvous, because its answer is computed under the ASKING flow's world). So there is no
 * shared-record hazard here of the kind the pair index's per-register `declineTaken` was written for.
 *
 * WHAT IT WRITES IS THE FACT AND NOTHING ELSE, exactly as the address-keyed one does: `flow_decline_fork` builds
 * the pair, so this may not settle the rendezvous — a machine whose request is DECLINED is not a machine with
 * an answer, and the arm that goes on waiting is the whole of what makes a per-origin widening mean anything.
 * Returns 1 if a record was marked, 0 if no flow is parked on that id. */
int engine_host_decline(JSContext *ctx, uint32_t req, const char *reason);

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
/* HOW MANY COMPLETED UNITS OF WORK this instance's flows have been credited — HTML §8.1.4.4 "Calling scripts"
   step 3 of clean up after running script, "if the JavaScript execution context stack is now empty". It is the
   PRECONDITION for running a queued job, so it is what makes `jobsRun` readable: without it, a run that queued
   thousands of reactions and ran none says nothing about whether the pump had nothing to do or was never
   eligible. See the declaration in engine.c for the measurement that made the pair necessary. */
long engine_units_done(void);

/* ---- THE LADDER'S OWN TRAFFIC — HOW MANY STEPS EACH ARM OF flow_step HAS RUN --------------------------------
 *
 * IT IS NOT THE `@COLD` HISTOGRAM AND THE TWO ARE NOT REFINEMENTS OF EACH OTHER. solver/cold.h's `step_units`
 * is a census of the MEMBERS STANDING at the instant it is taken — one bucket per arm, summing to the frontier
 * — so its `run-a-task: 0` says nobody is sitting in that arm right now. This is a count of STEPS over the
 * instance's life, so its `run-a-task: 0` says the ladder has never once reached that arm. Those two zeroes
 * are the OPPOSITE diagnoses of one symptom — an arm that is never entered against an arm that is entered
 * constantly and left again before any census — and they take opposite work. A gauge cannot answer the second
 * question and a lifetime total cannot answer the first, which is why both rows are emitted and neither is
 * derived from the other.
 *
 * IT IS A REPORT AND NEVER A BOUND (§NO BOUNDS). Nothing in the engine reads it to decide anything: no
 * fixpoint over an arm that stopped moving, no no-progress detector, no cap on how often an arm may run, no
 * seen-set over arms. The counters live in engine.c and say the same thing at the site; it is repeated here
 * because a header is where the next reader meets the numbers and a lifetime per-arm total is exactly the
 * shape someone reaches for to build a bound out of.
 *
 * A FILLED STRUCT for EngineFrontierCensus's reason and for one more: the array's extent is
 * solver/step_unit.h's own list, so a caller cannot size it from a second copy of that list and cannot get the
 * size wrong — there is no length argument to be right about. */
typedef struct {
    long steps;              /* scheduler steps: every entry into flow_step, counted at its own entry */
    long arms[STEP_UNIT_N];  /* …and how many of them ran each arm, in solver/step_unit.h's order. The two sides
                                are counted at DIFFERENT points on purpose (the entry, and the scheduler's
                                convergence point after the step returns), so `sum(arms) == steps` is an
                                assertion about routing rather than an arithmetic identity — engine.c asserts it
                                at the convergence point, where it is exact and where the offending step is
                                still in hand. */
    /* …AND HOW MANY OF THOSE STEPS DESCENDED THE LADDER AT ALL — the one number a reader of the orphan
       census's `asked` has never had, and the one that decides which of that zero's readings is available.
       IT IS NOT DERIVABLE FROM ANY ROW ON THIS DOCUMENT, which is the whole reason it is a field. flow_step's
       entire work ladder — the routed deliveries, the checkpoint, the reply, the program sequence, the task,
       the lifecycle, the orphan rungs, the clock-driven sources and every resting arm — sits inside one
       `if (!f->frame)`, so a step taken on a FRAMED member asks none of those conditions and is invisible in
       all of them. This counts the steps that entered that block, and there are three reasons a reader cannot
       compose it out of what is already published:
         · `unit_mid_program` below is the member's frame AFTER the step, read at the convergence point. A
           step that enters unframed and COMPILES leaves framed, and one that enters framed and ENDS its frame
           leaves unframed, so the two readings differ by exactly the arms that change framedness and neither
           bounds the other. They were 209 and 210 on one measured run and are free to differ by any amount.
         · Summing the `arms` that live under the `if` needs a per-arm branch label, and solver/step_unit.h
           declares none — solver/cold.h says so in its own words ("a reader told to read that arm's position
           in flow_step's chain against `engine_orphan_seed` finds it has no position in that chain at all").
           A list kept here would be a second copy of flow_step's structure and would drift from it.
         · `unframed_picks_lifetime` (solver/flow.h) is NOT this number and pairs with it by name, which is
           the trap. It is raised in flow_credit_pick, whose only caller is engine.c's `best != cur` block, so
           it counts SWITCH-INS that found an empty JavaScript execution context stack — a member switched in
           framed that unframes later is one descent this row sees and that one does not. Measured on one live
           page: 3 against 75.
       WHAT IT SEPARATES, AND THE TWO TAKE OPPOSITE WORK. `orphansAsked == 0` with this row 0 says the ladder
       was never descended at all, so the cause is UPSTREAM of every arm in it — members framed, or never
       handed the thread (solver/cold.h's `stepUnits` `none`) — and no ordering of the arms could have changed
       it. The same zero with this row LARGE says the ladder was descended and an arm ABOVE the orphan rung
       took every descent, which is a statement about those arms and is read off `stepUnitRuns` beside it.
       AND IT IS THE ROW `out_of_programs_at_the_ladder` CAN NO LONGER STAND IN FOR, which is why it is filed
       beside that family in the document rather than here at the top. That census selects on
       `script_i == dyn_n`, which WAS the rung's precondition and is no longer: the rung binds to
       `seq_compiles` — `a program of this flow's own sequence STARTS on this step` — so a member holding a row
       it cannot run descends the ladder and is counted in no `out_of_programs` row. On a document whose
       members always hold a row that family reads 0 for a reason that has nothing to do with the ladder.
       A COUNT OF DESCENTS AND NOT OF ENTRIES, so it is NOT comparable with `steps` above: flow_step's loop
       body iterates (the turn continuation at the reply delivery), and `steps` is raised once at the
       function's entry while this is raised on every pass that reaches the block. What it IS comparable with
       is `engine_orphan_census`'s `asked`, which is raised on the same per-pass basis inside this block — the
       containment is asserted at engine_step_unit_runs, where both are in one hand.
       AND THE TWO ARE PER-INSTANCE AND PER-SESSION RESPECTIVELY, WHICH IS STATED RATHER THAN LEFT TO A
       READER. solver/engine.c releases the orphan pair with the agent and never releases this one, so across
       a restart `asked` returns to 0 while this keeps climbing. The containment still holds in that direction;
       what does not hold is reading the pair as one span, exactly as engine/build.mjs already says of the
       orphan pair against @COLD.
       A REPORT AND NEVER A BOUND (§NO BOUNDS), for `arms`' reason exactly: nothing in the engine reads it, no
       arm of any verdict branches on it, and "how many steps got as far as the work ladder" is precisely the
       shape a no-progress detector would be built from.
       RETIREMENT: this row goes when solver/step_unit.h declares each arm's side of `if (!f->frame)`, because
       the descent count is then a sum over `arms` that cannot disagree with flow_step — which is also
       solver/cold.h's own stated retirement condition for the classes it carries in prose. */
    long unframed_steps;
    /* WHAT THE STEPS ABOVE COST, IN THE ONE MEASURE THE SLICE AND THE AGING CHARGE ARE ALREADY DENOMINATED IN
       — and it is in THIS struct rather than beside any other row because it is over exactly the population
       `steps` is: one charge per iteration of the scheduler loop that stepped a flow, taken at the line that
       already computes the delta for `flow_age_running`. A total whose denominator lives on another line, or
       in another census, is the lifetime-over-instant collapse this file's own rows keep having to correct.
       WHAT IT IS A TOTAL OF, EXACTLY, because "time per step" reads as if the step were the whole of it and it
       is not. The readings TELESCOPE — each charge is the clock at the end of this iteration minus the clock at
       the end of the previous one, or at slice entry for the first — so the quantity covers the PICK
       (solve_seed_candidates and flow_next_to_run), the CONTEXT SWITCH (both delta swaps) and the step itself,
       and it EXCLUDES the host's own time between slices, which each slice's fresh reading opens past. It is
       therefore what one turn of the dispatch loop costs, which is the quantity a reader wants when the
       question is why a run made so few choices.
       WHY THE RATIO AND NOT THE TOTAL IS THE READING. §Testing: two passes of one revision on one artifact are
       a 2x spread apart on this harness, so no count here may be quoted against another run. `step_us / steps`
       is two lifetime totals of ONE run that move together, so the spread divides out of it — and both sides
       are in the SLICE's own measure, which is what makes the quotient answerable without knowing whether that
       measure is CPU or wall (solver/quantum.h's `quantum_measure`, published as `@QUANTUM`). Any sentence
       that calls this CPU needs that line; the ratio against the slice does not, because the slice is armed on
       the same clock — which is also why a reader takes the slice off `@QUANTUM` rather than off
       ENGINE_QUANTUM_MS below: the run's own report belongs to the artifact that produced the total, and a
       header read afterwards belongs to whatever revision happens to be checked out.
       WHAT IT SEPARATES, which is the axis nothing in this census could reach. `steps` alone says how many
       choices a run made and cannot say why so few: a loop whose every turn consumes a whole slice makes about
       one choice per slice by construction — a granularity floor, not an ordering finding — while a loop whose
       turns are cheap made few choices because it was given little thread time at all, and those take opposite
       work. The @WFQ census answers the neighbouring half (what ASKING the order costs, in members walked per
       scan); this is what a turn costs in the currency the scheduler actually spends.
       A REPORT AND NEVER A BOUND (§NO BOUNDS), for `arms`' reason exactly: nothing in the engine reads it, no
       arm of any verdict branches on it, and a per-step time total is precisely the shape a watchdog or a
       step-cost cap would be built from. Its writer says the same thing at the site.
       AND IT IS `int64_t` BECAUSE `long` IS 32 BITS ON THE ONE HOST THIS ROW WAS BUILT TO BE READ ON. The two
       neighbours above are COUNTS of things the engine did and this is an accumulator of a CLOCK, which is a
       different quantity with a different horizon: the extension's engine is a wasm32 instance, where
       `__SIZEOF_LONG__` is 4, so a `long` of microseconds saturates at 2147483647 — 35.8 MINUTES of the
       measure the slice is denominated in. Past that the addition is signed overflow, which is undefined
       rather than merely wrapped, and the value a reader is handed is NEGATIVE.
       THE NEGATIVE IS WHY THIS IS A DEFECT AND NOT A LIMIT. `step_us / steps` is compared against the slice,
       and a negative numerator does not read as broken — it reads as a turn that cost far LESS than a slice,
       which is the arm that says the loop is not slice-bound and that a small step count is about thread time
       rather than granularity. So the one reading this row exists to make would silently INVERT on exactly the
       long runs it was written for, on the only host that ships. §Testing's rule that a measurement a loaded
       machine can falsify is no measurement is the same rule one layer down: a measurement its own arithmetic
       can falsify is no measurement either.
       THE WIDTH IS ASSERTED AT THE ACCUMULATOR (solver/engine.c's `_Static_assert` beside `g_step_us`, and a
       DCHECK before the one addition), because prose here cannot stop the next edit and a build failure can.
       result.c prints it through `(long long)`/`%lld` — the idiom the @WFQ census already uses for the notch
       rows, which are int64_t for this same reason. */
    int64_t step_us;
    /* …AND THE SPLIT OF IT THIS ROW'S OWN BANNER ASKS FOR AND COULD NOT MAKE. `step_us / steps` against the
     * slice separates a loop that is SLICE-BOUND from one that was given little thread time, which is the axis
     * above. It cannot separate the two slice-bound cases, and they take opposite work: a turn whose STEP
     * overruns the slice is the quantum with no asynchronous source to expire it — on the wasm instance that
     * ships, nothing can raise the yield bit mid-call, so a straight-line stretch never evaluates the budget —
     * while a turn whose PICK and SWAP dominate is the ordering and the COW delta costing more than the work
     * they order. One is solver/quantum.h's transport and the other is the frontier's own shape.
     * `sched_us` IS EVERYTHING IN THE TURN THAT IS NOT THE STEP, which is its name and not a shortfall: the
     * charge TELESCOPES, so it carries the previous iteration's tail — the microtask checkpoint, the finish —
     * with this one's pick and swap. That is the misattribution `step_us` above already declares, and the arm
     * is named for what it covers so it cannot be read as a pick cost.
     * TWO ROWS AND NOT A SUBTRACTION: a derived half cannot be checked, and `slice + sched == step` is what a
     * later edit adding a third phase to the turn breaks loudly instead of absorbing into the remainder. Both
     * are int64_t for `step_us`' reason exactly — a `long` of microseconds saturates in 35.8 minutes on wasm32
     * and INVERTS rather than going absent. */
    int64_t slice_us;
    int64_t sched_us;
    /* AND THE DENOMINATOR ALL THREE OF THE ROWS ABOVE HAVE NEVER HAD — the thread measure this instance
     * has consumed since its dispatch loop first ran, so `step_us / instance_us` is the share of the
     * engine's own thread that went into dispatch TURNS at all.
     * WHAT IT SEPARATES, AND IT IS TWO READINGS THAT TAKE OPPOSITE WORK. `step_us` and its two phases are
     * counts over the turns the loop took and say NOTHING about the turns it did not: a low `step_us / steps`
     * is equally a loop whose turns are cheap and a loop that was barely entered, and the second is not a
     * statement about the scheduler at all. Until this row existed the only way to reach it was to compare
     * `step_us` against a budget the census cannot see — a reader of a run had to know the rlimit its driver
     * was launched under, which is a fact about the HOST in a document about the engine, and a figure nobody
     * else can re-derive from the artifact.
     * BOTH SIDES ARE THE SAME CLOCK, WHICH IS THE ONLY PROPERTY THAT MAKES THE QUOTIENT MEAN ANYTHING. It is
     * `quantum_thread_us()` — CPU where the host has a CPU clock and wall where it does not — exactly as
     * `step_us` is, and `quantum_measure()` already names which on the @QUANTUM line. A ratio of two readings
     * of one clock survives a host that can only measure wall time; a ratio against an rlimit does not,
     * because an rlimit is PROCESS CPU and this is THREAD measure, and on a host with more than one thread
     * those are different quantities.
     * TAKEN IN THE SAME READING AS `steps` AND `step_us`, for the reason this struct exists: between two
     * accessor calls the loop can step, and a total read one call later than its denominator is the two-
     * instants collapse §Testing names. The containment `step_us <= instance_us` is asserted where both are
     * in one hand — every turn's charge is a sub-interval of the span this measures, so a violation is the
     * baseline having been taken after a turn, or the clock having stopped being monotone.
     * THE RESIDUAL THAT STOOD HERE IS DISCHARGED BY `loop_us`/`between_slices_us` BELOW, AND IT IS REWRITTEN
     * RATHER THAN DELETED BECAUSE ITS NEXT-DIFF CLAUSE NAMED A MECHANISM THIS FUNCTION DOES NOT HAVE AND THE
     * NEXT READER WOULD RE-DERIVE IT THE SAME WAY. It said: build "a second accumulator raised from the
     * readings `engine_sched_slice` already takes at its entry and at each of its returns". The SPLIT is
     * right and both halves of that sentence about the tree are wrong. `engine_sched_slice` takes ONE reading
     * at its entry and takes NO reading at ANY of its returns — the `now` a return leaves behind is the
     * last POST-STEP reading, so a span closed on it would silently exclude the slice's tail, which is part
     * of the very population the residual was about. And a charge written at each of those returns is the shape
     * `engine_sched_step`'s own banner refuses for `quantum_end()`, in as many words: "A `quantum_end()` call
     * before every return is the shape where one of them is eventually missing". The accumulators are
     * therefore in the WRAPPER, where the bracket already is and where the body's exits cannot reach them.
     * AND THE REMAINDER IS NOT THE HOST'S BY SUBTRACTION OF TWO PUBLISHED HALVES, WHICH IS THE SECOND THING
     * THAT CLAUSE GOT WRONG AND THE ONE THAT WOULD HAVE COST A READING. `instance_us - loop_us` is the host's
     * thread between slices PLUS the span from the last slice's return to the moment this census was
     * composed — and a census is composed on the host's own thread at whatever moment a driver asks for it,
     * so that trailing span can be the whole of a report build. It is charged into `between_slices_us` at the
     * accessor, from the same clock reading `instance_us` closes on, so the two rows are a PARTITION and the
     * reader adds rather than subtracts.
     * A REPORT AND NEVER A BOUND (§NO BOUNDS), for `step_us`' reason and with the same hazard — a measured
     * share of a thread is exactly what a throttle would be built from, and nothing reads this to decide
     * anything.
     * `int64_t` FOR `step_us`' REASON EXACTLY: a `long` of microseconds saturates in 35.8 minutes on wasm32
     * and INVERTS rather than going absent, and this one measures a span STRICTLY LONGER than that row. */
    int64_t instance_us;
    /* …AND THE PARTITION OF IT THE ROW ABOVE COULD NOT MAKE, WHICH IS THE WHOLE OF WHY A SMALL
     * `step_us / instance_us` HAS NEVER NAMED A COMPONENT. `loop_us` is the thread measure spent INSIDE
     * `engine_sched_step`'s bracket, summed over every slice; `between_slices_us` is the thread measure that
     * passed between one slice's return and the next one's entry, plus the span since the last return, which
     * is the host's. THE TWO SUM TO `instance_us` EXACTLY and that is asserted where all three are in one
     * hand, so a reader adds two published rows rather than subtracting one from a total and hoping the
     * remainder is what they think it is.
     * AND THAT THE OPEN TAIL IS THE HOST'S AT ALL IS A SEPARATE CLAIM AND IS SEPARATELY ASSERTED, because no
     * arithmetic here can reach it: the tail is closed from the last slice's RETURN, so a census composed
     * from INSIDE the dispatch loop would charge a running slice's own elapsed time to the host's half — and
     * the sum would still equal `instance_us`, since the two halves telescope whatever the tail belongs to.
     * engine_step_unit_runs asserts `!quantum_slice_open()` for that reason; solver/quantum.h's own invariant
     * ("the shipped ABI may never RETURN to the host holding one") is what makes it true today, and the
     * assert is what makes it stay true when a caller is added.
     * WHAT THEY SEPARATE, AND IT IS THE PAIR OF DIAGNOSES THE SHARE ABOVE SUMS. A run whose
     * `step_us / instance_us` is small is one of two things and they take OPPOSITE work. If `loop_us` is
     * small too, the engine was BARELY GIVEN THE THREAD: the remainder sits in `between_slices_us`, the
     * question is the DRIVER — how often it steps, what it does between steps, what the provider and the
     * parse cost — and no re-pricing of any weight term in this file reaches it. If `loop_us` is LARGE and
     * `step_us` is still small, the engine had the thread and spent it inside the dispatch loop OUTSIDE a
     * turn's own bracket, and the question is THIS SCHEDULER: the loop's entry and exit work, the park, the
     * session close, and every slice that ran no turn at all. §solver/flow.c names the first being dispatched
     * as the second; these two rows are what stops that being a matter of taste.
     * AND A SLICE THAT TOOK NO TURN IS THE POPULATION THAT MAKES THAT SECOND ARM REACHABLE AT ALL, which is
     * not obvious from `step_us`' own banner and is worth stating because it inverts what a reader expects.
     * The turn charge TELESCOPES from the slice's entry reading, so for a slice that takes at least one turn
     * the loop's entry work is already ON that first turn's bill and `loop_us - step_us` over it is only the
     * tail. A slice that takes NO turn charges `step_us` NOTHING and charges `loop_us` its whole duration. So
     * `loop_us - step_us` running large is, first of all, a statement about slices that answered without
     * dispatching anybody — which `slices` against `steps` is the reading for.
     * `slices` IS THE DENOMINATOR BOTH OF THEM WOULD OTHERWISE NOT HAVE, and it is the row `over_arms` names
     * below as the one nothing raises. A per-slice total with no count of slices is a total whose denominator
     * lives in another census or in nobody's hand, which is the collapse `step_us`' banner is about; with it,
     * `loop_us / slices` is what a slice costs and `between_slices_us / slices` is what the host takes
     * between two of them, both in the slice's own measure and both comparable against `ENGINE_QUANTUM_MS`.
     * IT IS A COUNT AND NOT A MEAN OF ANYTHING, so it survives the run-to-run spread the way `steps` does
     * not: quoted alone it is unquotable against another run, and as the denominator of a lifetime total of
     * ONE run it is what makes the quotient quotable at all.
     * RAISED IN `engine_sched_step` AND NOT IN `engine_sched_slice`, WHICH IS A CORRECTION TO `over_arms`'
     * OWN CLAUSE AND NOT A CHOICE. That clause says to raise it "beside `quantum_begin()` in
     * engine_sched_slice", and `quantum_begin()` is not in `engine_sched_slice` — it is in the WRAPPER, which
     * is the whole reason the wrapper exists. A reader who obeys the clause literally finds no such line.
     * A REPORT AND NEVER A BOUND (§NO BOUNDS), for `step_us`' reason and with a sharper hazard than any row
     * above: a count of slices beside what each one cost is exactly the pair a "the engine is not getting
     * enough thread, take more of it" policy would be built from. Nothing reads any of the three.
     * `int64_t` FOR `step_us`' REASON EXACTLY on the two microsecond rows — a `long` of microseconds
     * saturates in 35.8 minutes on wasm32 and INVERTS rather than going absent — and `long` on `slices`,
     * which is a count of dispatch slices and shares `steps`' horizon and `steps`' type. */
    int64_t loop_us;
    int64_t between_slices_us;
    long    slices;
    /* …AND THE PARTITION THE SPLIT ABOVE TURNED OUT TO NEED, WHICH IS THE READING AND NOT A SECOND OPINION.
     * `slice_us`' banner promises that `step_us / steps` against the slice answers whether the loop is
     * slice-bound. It does not, because that quotient is a LIFETIME MEAN over a turn population that is not
     * uniform: read as a SERIES rather than as a terminal value, the MARGINAL cost between consecutive
     * censuses of one run spans four orders of magnitude — 0.105 ms between two samples and 822 ms between
     * two others — so the mean is a figure no turn is near, and comparing it against the budget is the
     * bare-count-over-an-unpartitioned-population defect wearing a ratio.
     * AND THE DISTRIBUTION IS NOT MERELY WIDE, IT IS A HANDFUL OF OUTLIERS CARRYING ALMOST ALL THE TIME,
     * which is the shape that decides what this row is worth. Over one whole run — 47 censuses, 13043 turns,
     * revision 638eb345, one interleaving — FOUR of the 46 windows had a marginal turn at or past the slice
     * and the other FORTY-TWO ran at 0.1 to 1.5 ms, comfortably inside it. So the loop is not slice-bound in
     * the way a mean of 56 ms suggests: it is overwhelmingly NOT slice-bound, with a few turns of 155, 222,
     * 684 and 822 ms — the last being 68 slices in one turn — carrying the run. That is a small population
     * to go and look at, which a mean can never hand you.
     * A COROLLARY THAT CORRECTS THE PHASE SPLIT'S OWN HEADLINE: the STEP's share is not a constant. Over the
     * same run it drifts monotonically from 99.99% to 99.87%, so `sched`'s share GROWS sixteenfold as the run
     * proceeds — which is what a roughly fixed per-turn cost does once the cheap turns come to dominate the
     * denominator. The conclusion is unchanged and its stability was overstated: a single quoted percentage
     * of this pair is a reading of WHERE IN A RUN it was taken. `slice_overruns` is the count of
     * TURNS whose step alone met or exceeded the budget, so `slice_overruns / steps` is a proper fraction of
     * a denominator this struct already carries and needs no mean at all.
     * IT IS THE SAME INEQUALITY quantum_expired() ASKS ON ONE OF THE TWO BRANCHES, AND THE PARAGRAPH THAT SAID
     * IT WAS BOTH IS REWRITTEN RATHER THAN DELETED BECAUSE IT IS THE CLAIM A READER RE-DERIVES. It read:
     * "solver/quantum.c tests `quantum_thread_us() - <slice start> >= ENGINE_QUANTUM_MS * 1000` and this tests
     * the step's own two readings against that same product", and that is the GENERIC branch. The LINUX branch
     * — the one the native gate compiles, and so the one every number quoted off this row was measured on —
     * returns `g_fired != 0`, a flag a CLOCK_THREAD_CPUTIME_ID timer sets. The two agree in INTENT and are not
     * one test: this row compares two clock readings and that branch reads a signal flag, so a delayed or
     * coalesced delivery moves one and not the other. The conclusion the paragraph drew is unchanged — the row
     * is not a private opinion about the budget — but it rests on the two being written to the same MARGIN,
     * never on their being the same expression.
     * AND THE TAIL CLAUSE NAMED THE WRONG HOST, WHICH INVERTS WHAT A NATIVE OVERRUN MEANS. It called this
     * count "§scheduler's named transport gap made countable on a host where nothing can raise the yield bit
     * mid-call" — true of the wasm instance, which has no asynchronous edge, and FALSE of the native host,
     * whose timer raises the bit mid-call by construction. So a native overrun is not the transport gap: it is
     * a stretch that offered no raise point WHILE THE BIT WAS RAISABLE, which is the stronger reading and the
     * one the seam verdict exists for.
     * AND NEITHER SEAM VERDICT CAN JUDGE THE POPULATION THIS ROW COUNTS, WHICH IS WHY THE ROW KEEPS READING
     * HIGH BESIDE A SILENT ABORT. Both verdicts in engine_sched_step are ANDed with `g_preempt_asked == pa0`
     * — ZERO consultations across the WHOLE step — so they can only ever name a step that never offered a
     * point AND never ended by being preempted. A turn in this row ended at the slice boundary, so the hook
     * WAS consulted to end it, the conjunct is false, and both verdicts are disarmed AT ANY MARGIN. The CPU
     * margin is independently too coarse for it: 400 slices against a measured population near 291. MEASURED
     * on the native smoke at two adjacent revisions, 9e0f14dc and 3864b36a: `resume-program` overran 70 of
     * 161 and 70 of 152 runs, `start-a-classic-program` 3 of 7 and 3 of 8, total 73 both times over 1521 and
     * 945 steps, with ZERO @WHY in either log. The quantity the contract is about is the GAP between two
     * consecutive offers, which this file already computes (`g_max_gap`, closed off with the tail) and prints
     * without deciding on, because it is WALL and a wall gap cannot tell a seamless stretch from a descheduled
     * one. On a host quantum_measure_is_cpu() answers for, that objection does not apply and the gap can be
     * taken in the slice's own measure — which is the same move the CPU verdict already made for the TOTAL,
     * owed to the quantity the seam is actually about.
     * RETIREMENT: this record goes when a seam verdict decides on the GAP in the slice's own measure, so a
     * step that rests once and then runs seamlessly cannot be silent.
     * A COUNT AND NOT A MAXIMUM. A high-water mark of turn length would saturate early and then plateau, and
     * a plateau is indistinguishable from a ceiling on a short run; a count only rises with the population it
     * is drawn from, and the population is printed beside it. */
    int64_t slice_overruns;
    /* …AND WHICH ARM EACH OF THOSE TURNS WAS IN, which is the question the count above raises and cannot
     * answer. `arms` is the same list counting RUNS, so this is that histogram restricted to the turns that
     * overran, in solver/step_unit.h's order, and `sum(over_arms) == slice_overruns` is asserted where both
     * are in one hand exactly as `sum(arms) == steps` is. Two histograms over one list, and the pair is the
     * reading: an arm with many runs and no overruns is cheap however often it is taken, and an arm with
     * FOUR runs and FOUR overruns is a step that cannot be preempted, which is a different diff in a
     * different component from a hot arm.
     * WHY IT IS NOT PER-ARM TIME. A time accumulator per arm would answer "where did the run go", which is a
     * question about MASS; this answers "which arm cannot rest", which is a question about the TRANSPORT, and
     * only the second is what §NO BOUNDS' suspend-at-any-depth requirement is about. A count also needs no
     * second clock reading and partitions a total this struct already publishes, so it can be asserted
     * rather than believed.
     * ITS FIRST READING INVERTED THE OBVIOUS ONE, AND THAT IS WHAT THE PAIR ABOVE IS FOR. Ordered by COUNT the
     * answer was `resume-program`, 55 of the 65 overruns — and that arm runs 2085 times, so its rate is 2.6%
     * and it is the LEAST interesting of the three. Ordered by RATE the answer is `start-a-classic-program`:
     * 8 overruns out of TWENTY-FOUR RUNS, one start in three held the thread past the slice. A magnitude and a
     * rate over one population, naming different arms, which is why the reader that renders this says which
     * it sorted by.
     * AND THE SHAPE OF THE DISTRIBUTION IS A SEPARATE FACT FROM EITHER — the overruns are NOT spread with the
     * work. On that run `deliver-one-reply` took 36% of every turn and `run-a-task` 18.5%, and NEITHER OVERRAN
     * ONCE; every overrun was in one of three arms and two of those three are a program STARTING or RESUMING.
     * So the slice is held by particular UNITS OF WORK rather than by the amount of work. An arm's ABSENCE
     * from the overrun histogram is therefore evidence, and it is the half a reader who looks only at the
     * non-zero rows never sees.
     * AND THE MECHANISM THIS PARAGRAPH FIRST GAVE FOR THAT IS RETIRED, REFUTED BY THE SAME RUN'S OWN ROWS —
     * rewritten rather than deleted, because it is the explanation a reader re-derives in one step and it is
     * wrong. It said: the only raise sources are the interpreter's own (a back edge, a call, a fork), so a
     * unit running ENGINE C rather than page bytecode has nothing to raise the request and the budget cannot
     * be evaluated inside it however long it takes. The raise-kind half is TRUE and checkable — quickjs.h
     * declares exactly JS_PREEMPT_BACKEDGE, _FORK, _CALL and _HOST, and solver/engine.c's preempt_hook is the
     * only caller of quantum_expired on the flow path. The CONCLUSION does not follow, and this run refutes
     * it: a start COMPILES before it executes, and `start-ended-its-frame` — a start that compiled and ran to
     * completion inside the step — ran 153 TIMES AND OVERRAN NOT ONCE. `deliver-one-reply`, engine C at its
     * own door, ran 6990 times and overran not once either. The compile is not what holds the slice.
     * AND THE RATE WAS QUOTED AGAINST THE WRONG DENOMINATOR, WHICH IS WHAT MADE THE WRONG MECHANISM LOOK
     * NECESSARY. `start-a-classic-program` does not mean "a start"; step_unit.h's own split says it means a
     * start THAT RETURNED WITH ITS FRAME LIVE, its three frame-clearing outcomes being separate rows. So the
     * 24 is not the start population: all four start rows sum to 192, and 8 of 192 is 4.2%. "One start in
     * three" was a fraction of the wrong total, which is this project's own coverage-figure defect committed
     * at the row that exists to prevent it.
     * WHAT THE PARTITION ACTUALLY SAYS IS SHARPER THAN EITHER READING, because the two are not two
     * denominators for one question but TWO POPULATIONS: a start that FINISHED inside the step overran 0 of
     * 153 times, and a start that was STILL RUNNING when the step ended overran 8 of 24. The overruns sit
     * where page code was still executing, which is what a stretch between two of the page's OWN raise points
     * looks like — a long back-edge-free, call-free run — and not where this engine's C is. `resume-program`
     * at 55 of 2085 is the same population one step later. A unit is not uninterruptible because it is C; it
     * is uninterrupted because the BYTECODE it is running offered no raise point, and only the page decides
     * that.
     * AND IT IS NOT A COUNT OF THE TURNS THAT ENDED ON THE COOPERATIVE QUANTUM, WHICH IS THE READING A
     * READER HOLDING A FRAME-CLEARING RATE ARRIVES AT AND THE ONE THIS ROW MOST INVITES. preempt_hook's
     * third clause ends a turn when the quantum is SPENT, so it reads as though that population must be
     * exactly the turns counted here. The budget is the SLICE's and a slice holds MANY turns:
     * solver/quantum.c arms the edge once at engine_sched_slice's `quantum_begin()` and the dispatch loop
     * ends the slice on the same expiry, so at most ONE turn per slice can end on that clause and ITS OWN
     * delta is whatever was left of the budget when the edge fired — usually far under it, and therefore
     * not in this row at all. What this row counts is a turn that ALONE met the whole budget, which is the
     * transport question the paragraphs above are about and is a different population entirely.
     * WHAT ANSWERS THE OTHER QUESTION IS A COUNT OF SLICES, AND `slices` ABOVE IS IT — THE CLAUSE THAT SAID
     * NOTHING RAISES ONE IS RETIRED AND ITS COORDINATE WAS WRONG WHEN IT WAS WRITTEN, WHICH IS WORTH MORE
     * THAN THE RETIREMENT. It said to "raise a slice count beside `quantum_begin()` in engine_sched_slice",
     * and `quantum_begin()` has never been in `engine_sched_slice`: it is in `engine_sched_step`, whose own
     * banner says why the bracket is a wrapper at all. The SPLIT was right, the MECHANISM was right, and the
     * clause named a line that does not exist — which is what a next-diff clause is for and what it is worst
     * at, because the one reader of it has already decided to do the work.
     * WHAT THE COUNT BUYS, STATED AS THE BOUND IT IS AND NOT AS AN ANSWER: solver/quantum.c arms the edge
     * once per slice and the dispatch loop ends the slice on the same expiry, so the quantum-ended
     * population is bounded above by `slices`, and `steps - slices - <the blocked arms>` is a FLOOR on the
     * outranked one. STILL NOT COVERED, and it is the same sentence as before with the bound subtracted from
     * it: how a turn IN ANY GIVEN ARM came to end is still one of three clauses and is still unreadable at
     * every arm, so the pair above is a statement about the RUN and never about a row. The two that matter
     * take OPPOSITE work — a quantum-ended turn saying the slice is short for the spans being run (a policy
     * input this scheduler owns and may tune) and an OUTRANKED turn saying the order moved the thread to a
     * better-ranked member, which is the WFQ doing what it is for and is not a thing to repair.
     * HOW ITS ABSENCE SHOWS, unchanged because the per-arm half is unchanged: a reader meeting a low
     * frame-clearing rate reaches for this row, finds it small, and concludes the slice is not what ends
     * those turns — which this row cannot support in either direction, because the population it counts is
     * not the one that question is about. */
    long over_arms[STEP_UNIT_N];
    /* …AND WHETHER THE PAGE'S OWN CODE WAS EVEN RUNNING IN THOSE TURNS, WHICH IS THE ONE THING THE ARM
     * HISTOGRAM ABOVE CANNOT SAY AND THE THING ITS OWN CONCLUSION RESTS ON. `over_arms`' banner reaches a
     * verdict — "the overruns sit where page code was still executing, which is what a stretch between two of
     * the page's OWN raise points looks like" — and NOTHING in this struct measures that. It is an inference
     * from the ARM a turn declared, and an arm is where a step ENDED: `resume-program` and
     * `start-a-classic-program` both end inside JS_FlowResume whether the time went into the page's bytecode
     * or into ONE native call that never returned, and those take OPPOSITE work. One is the page choosing a
     * back-edge-free stretch, which no ordering reaches and which §NO BOUNDS forbids capping; the other is a
     * C activation that declares no step boundary, which is a step-machine conversion (§C-stack) in whichever
     * component owns that call.
     * AND THE EVIDENCE THAT CONCLUSION RESTS ON IS A FIXTURE'S, WHICH A REAL PAGE DISAGREES WITH — RELAYED
     * AND NOT RE-DERIVED HERE, SO IT IS A CLAIM TO CHECK AND NOT A ROW. `over_arms`' banner reaches its
     * verdict partly from `deliver-one-reply` running 6990 times on the native smoke and overrunning NOT
     * ONCE, i.e. engine C at its own door never holding the slice. A reading relayed from gitlab.com/explore
     * has that same arm overrunning 4 of 43 runs. If that holds, the fixture's zero is a statement about
     * REPLIES THE FIXTURE SERVES and not about engine C, which is exactly the shape CLAUDE.md's
     * fixture-workload rule names: a body whose length the page chose is not a quantity a fixture's
     * denominator contains. The rows below are what settles it either way, on either host, without anybody
     * having to believe the relay.
     * WHAT SEPARATES THEM IS ALREADY COMPUTED AND HAS NO READER FOR THIS POPULATION. solver/engine.c samples
     * `g_preempt_asked` at each turn's start, and the difference across the turn is how many suspend points
     * the path OFFERED — zero means the turn never reached ONE interpreter raise point. Both seam verdicts in
     * engine_sched_step are ANDed with `g_preempt_asked == pa0`, so they can only ever name a turn that
     * offered NO point, and a turn in `slice_overruns` ended at the slice boundary, where the hook WAS
     * consulted to end it. The CPU verdict additionally requires quantum_measure_is_cpu(), which is FALSE on
     * the host that ships. So on the shipped host the consultation count is written every turn and read by
     * nothing that can fire on an overrunning one: CLAUDE.md's computed-writer-with-no-reader defect, with
     * the value real and the only reader structurally disarmed.
     * TWO ROWS AND NOT ONE, because a SUM over the overrunning turns can be carried by one chatty turn while
     * every other one of them offered nothing. `slice_overrun_asks` is the total and
     * `slice_overrun_seamless` is HOW MANY of those turns offered zero — a count of turns, not of
     * consultations — so the pair partitions the population by the property that decides the diff rather than
     * averaging over it. THE TWO ARE EXACTLY EQUIVALENT AT THEIR ENDPOINTS and that is asserted where both
     * are in one hand: `slice_overrun_asks == 0` if and only if `slice_overrun_seamless == slice_overruns`,
     * because a turn contributes to the sum precisely when it is not seamless.
     * IN EVERY BUILD, unlike the seam verdict's own sampling, and the price is ONE READ OF A STATIC per turn
     * beside the two clock readings the turn already takes — the same argument solver/engine.c already makes
     * for `g_preempt_asked`'s increment being outside the dev guard, one indirection cheaper. The bracket is
     * the STEP's and not the turn's: it opens at the same `t_slice0` `slice_us` opens at, so the count is
     * over exactly the span the overrun test is about and not over the pick and the swap.
     * THEY DECIDE NOTHING AND BOUND NOTHING (§NO BOUNDS). Nothing reads either to refuse a step, shorten a
     * slice or demote a flow; a per-turn count of suspend points offered is precisely what a "this flow is
     * not yielding, take the thread" watchdog would be built from.
     * AND THE THIRD READING IS THE ONE THE PHASE SPLIT ABOVE CANNOT MAKE, WHICH IS WHY THIS PAIR IS NOT A
     * RESTATEMENT OF `sched_us`. `sched_us` bounds what the PICK cost — flow_next_to_run runs before the step
     * bracket opens — and the preempt hook's OWN rescan of the frontier does not land there: it is called
     * from the interpreter, so an O(members) walk through flow_rival_of is charged to `slice_us`, inside the
     * very turns this row counts. A reader who takes a small `sched_us` for "the ordering is not the cost"
     * has bounded the pick and said nothing about the hook. `slice_overrun_seamless == slice_overruns`
     * settles it outright and in the other direction: flow_rival_of's only caller is that hook, which raises
     * the consultation count before it rescans, so a turn that offered no consultation performed no rescan
     * and weighed no member. The ordering is then excluded from those turns by construction rather than by a
     * bound on a neighbouring row.
     * HOW THEIR ABSENCE WOULD SHOW, as an observation and not an instance: a reader holding a nonzero
     * `slice_overruns` reaches for `over_arms`, finds mass in a program arm, and states which of the two
     * spans held the thread — with no row anywhere in the artifact that could have contradicted them.
     * RETIREMENT: these two go when a seam verdict can judge a turn that ENDED at the slice boundary, i.e.
     * when it is no longer conjoined with `g_preempt_asked == pa0`, because the existing reader then names
     * the same population and these rows are a second copy of it. */
    uint64_t slice_overrun_asks;      /* suspend points OFFERED, summed over the turns that met the slice */
    long     slice_overrun_seamless;  /* …and how many of those turns offered NOT ONE */
    /* …AND IN WHICH ARM THOSE SEAMLESS TURNS WERE, WHICH IS THE JOIN THE TWO ROWS ABOVE AND `over_arms` CANNOT
     * MAKE AND THE ONE THING THE WHOLE PAIR'S CONCLUSION RESTS ON. `over_arms` is a histogram and
     * `slice_overrun_seamless` is a SCALAR, so a reader holding one arm at three quarters of all overruns and a
     * seamless count at three quarters of the same total cannot say whether those are the same turns — and the
     * two readings take OPPOSITE work, which is this pair's own argument: a seamless stretch is "a C activation
     * that declares no step boundary, which is a step-machine conversion (§C-stack) in whichever component owns
     * that call", and a stretch that offered points and ran anyway is "the page choosing a back-edge-free
     * stretch, which no ordering reaches and which §NO BOUNDS forbids capping". Two components, two diffs, and
     * until this row the evidence for either was two numbers of similar size.
     * MEASURED, WHICH IS WHY IT IS A ROW RATHER THAN A CAUTION: over three drives of one release artifact on one
     * real app, `seed-one-orphan-flow` overran 36 of 55, 100 of 121 and 122 of 140 of its OWN runs — 51%, 72%
     * and 76% of all overrunning turns — while the seamless scalar read 55%, 75% and 78% of that same total. The
     * two move together across three passes and nothing could join them.
     * IT IS A PARTITION OF `slice_overrun_seamless` EXACTLY AND A SUBSET OF `over_arms` PER ARM, and both are
     * asserted where all of them are in one hand — inside the overrun branch, one statement after the scalar,
     * from the same turn's arm and the same turn's consultation delta. Asserting at the accessor instead would
     * learn of a disagreement with the turn that caused it long gone.
     * LIFETIME COUNTS, never reset, raised in EVERY build — the scalar they partition is raised unconditionally
     * too, and a partition compiled out in release would print zeros beside a nonzero total and read as turns
     * that all offered a point rather than as a build that never classified them.
     * IT DECIDES NOTHING AND BOUNDS NOTHING (§NO BOUNDS), for the scan counters' reason exactly.
     * HOW ITS ABSENCE SHOWS, as an observation and not an instance: a reader holding a nonzero
     * `slice_overruns` finds one arm carrying most of them and the seamless scalar carrying a similar share,
     * and states which of the two spans held the thread — with no row in the artifact that could contradict
     * them in either direction.
     * RETIREMENT: this goes when a seam verdict can judge a turn that ENDED at the slice boundary, which is
     * the condition its own scalar carries, because the existing reader then names the same population per arm
     * and this is a second copy of it. */
    long     over_seamless_arms[STEP_UNIT_N];
    /* …AND HOW MANY POINTS THE NON-SEAMLESS TURNS OF EACH ARM OFFERED, WHICH IS THE ONE QUANTITY
     * `slice_overrun_asks` DESTROYS BY BEING A SUM. The scalar is a total over every overrunning turn that
     * offered at least one point, so a turn that asked TWICE and a turn that asked TWO THOUSAND TIMES are one
     * figure, and `asks / overruns` over the whole population averages across arms whose rates differ by an
     * order of magnitude. Those two per-turn densities are the two readings §`slice_overrun_asks` leaves open
     * and cannot separate: a turn that offered a handful of points and then ran for seconds is a long gap
     * between consultations — the population solver/quantum.h names as closed by a step-machine conversion and
     * by nothing in that file — while a turn that offered thousands and ran anyway is the page choosing a
     * stretch no ordering reaches, which §NO BOUNDS forbids capping. PER ARM is what makes the question
     * answerable at all, because the arms do not share a density: measured over seven real-site drives of two
     * documents, `deliver-one-reply` overran 4.6-11.3% of its own 401-622 runs while
     * `evaluate-a-module-program` and `microtask-checkpoint` overran 100% of their 3-6, so one ratio over the
     * sum is a mean across populations that answer differently.
     * RAISED IN THE SAME BRANCH AS THE SCALAR AND FROM THE SAME DELTA, one statement apart, so
     * `sum(over_ask_arms) == slice_overrun_asks` is exact THERE and is asserted there — the same argument
     * `over_seamless_arms` makes, and the reason neither is asked at the accessor, where a disagreement would
     * arrive with the turn that caused it long gone. It is the COMPLEMENT of `over_seamless_arms` by the order
     * of two statements rather than by agreement: a turn adds to one arm's ask sum precisely when it does not
     * add to that arm's seamless count.
     * A SUM OF CONSULTATIONS AND NOT A COUNT OF TURNS, which is why its denominator is `over_arms` minus
     * `over_seamless_arms` and never `over_arms` — a seamless turn contributes zero to this row by
     * construction, so dividing by every overrunning turn of the arm understates the density of the ones that
     * actually asked. That is the gauge-and-lifetime split arriving inside one pair, and stating it here is
     * what stops a reader composing the wrong quotient from two rows that sit side by side.
     * LIFETIME COUNTS, never reset, raised in EVERY build for `over_seamless_arms`' reason exactly.
     * A `long` WHERE THE SCALAR IT PARTITIONS IS A `uint64_t`, WHICH IS A NARROWING AND IS CHECKED RATHER THAN
     * HOPED. The width is not free to choose: solver/result.c composes every step-unit row through ONE
     * `cold_hist_json`, whose own banner exists to stop a second speller of the row format from drifting, and
     * that composer takes a `long` array and prints `%ld`. A `uint64_t` partition would therefore force a
     * SECOND composer for one row — the drift this project refuses — so the row is a `long` and the add
     * asserts its own headroom at the raise, in the form a DCHECK condition may take, exactly as the
     * microsecond accumulators do. On the host that ships a `long` is four bytes, so the horizon is about two
     * billion consultations IN ONE ARM across a frontier that is never reset; the scalar stays 64-bit because
     * it sums every arm and because narrowing it would be a change to a published row rather than to a new
     * one. The assert is what makes that sentence a checked claim instead of a remembered one.
     * IT DECIDES NOTHING AND BOUNDS NOTHING (§NO BOUNDS): a per-arm count of suspend points offered is
     * precisely what a "this arm is not yielding, take the thread" watchdog would be built from.
     * HOW ITS ABSENCE SHOWS, as an observation and not an instance: a reader holding an arm at 100% of its own
     * runs overrunning reaches for `slice_overrun_asks`, divides by `slice_overruns`, and states a per-turn
     * density that is a mean over every other arm's turns as well — with no row in the artifact that could
     * contradict them.
     * RETIREMENT: this goes when a turn's own maximum inter-consultation GAP is published per arm, because the
     * density this row gives is a proxy for that gap and the gap is what the two readings actually differ on —
     * which is the condition `slice_overrun_asks`' own banner carries, and the gap is computed in every DEV
     * build already and read only by a verdict conjoined with `asked == 0`. */
    long     over_ask_arms[STEP_UNIT_N];
    /* …AND THE LONGEST STRETCH INSIDE ONE OF THOSE TURNS DURING WHICH NO SUSPEND POINT WAS OFFERED, WHICH IS
     * THE ONE QUANTITY THE ROW ABOVE IS A PROXY FOR AND CANNOT BE — AND WHICH IS WORTH SOMETHING DIFFERENT
     * FROM WHAT THIS BANNER FIRST CLAIMED, BECAUSE THE INTUITIVE ARGUMENT FOR IT IS REFUTED BY THIS ENGINE.
     * IT READ: `over_ask_arms` divided by its own denominator is a DENSITY, and a density is a MEAN — a turn
     * that held the thread for seven seconds and consulted the policy nine times reads 9 whether those nine
     * were evenly spread (nine stretches of about 800 ms, which is NINE spans with no suspend point in them
     * and therefore nine step-machine conversions' worth of C) or clustered in the first millisecond before
     * one unbroken 7-second run, which is ONE; those take opposite work and the mean cannot separate them.
     * IT IS KEPT IN ITS OWN WORDS BECAUSE IT IS THE ARGUMENT A READER RE-DERIVES IN ONE STEP, and the EVENLY
     * SPREAD world it rests on CANNOT OCCUR HERE. Derived by reading, not relayed: `quantum_expired()` is
     * MONOTONE within one open slice on BOTH hosts — the generic branch tests a monotone clock against a
     * `g_slice_start_us` fixed at `quantum_begin`, and the linux branch reads a `g_fired` flag cleared ONLY in
     * `quantum_begin` and `quantum_end` — and solver/engine.c's preempt_hook has exactly three returns, of
     * which the only one that can answer FALSE is `return quantum_expired()`. Every consumer of a TRUE in
     * quickjs.c parks (`goto do_*_park`) or, off the flow base, `DFAIL`s, and a park ENDS THE TURN. So at most
     * ONE consultation per turn answers TRUE, every consultation that answered FALSE happened before the slice
     * reached ENGINE_QUANTUM_MS, and an overrunning turn lies inside a slice its own span already met —
     * therefore EVERY consultation of an overrunning turn falls inside one window of at most
     * ENGINE_QUANTUM_MS from that slice's opening.
     * WHICH MAKES THE GAP BOUNDED RATHER THAN FREE: it lies in [span − ENGINE_QUANTUM_MS, span] for every
     * non-seamless overrunning turn, so a long dark stretch is GUARANTEED on any such turn whose span exceeds
     * twice the budget and is not news. WHAT THE ROW IS THEREFORE WORTH IS THREE THINGS AND NOT THE ONE ABOVE.
     * FIRST, and this is what nothing else publishes: to within one budget it is the per-arm MAXIMUM TURN SPAN
     * of a non-seamless overrunning turn — `over_arms` is deliberately a COUNT and its own banner says WHY IT
     * IS NOT PER-ARM TIME, so no row anywhere says how long the worst turn of an arm actually was, and a
     * AND THAT FIRST USE IS NOT THIS BANNER'S ARGUMENT, IT IS A MEASUREMENT THIS TREE MADE WITHOUT IT, which
     * is better evidence than any reasoning here and is read from the commit rather than recalled: `6813a2a`
     * WITHDRAWS a per-arm turn span from testing/live-run.js's own banner because the row it was taken from
     * pools its denominator over every arm, and states what IS derivable per-arm from the rows that exist —
     * "lower 4 x 12 ms = 48 ms", "upper 64.875 s", "ratio 1352x". That bracket is the hole this row fills: it
     * answers the same question to within ONE BUDGET instead of within three orders of magnitude. The same
     * message records why pooling is not a small imprecision there — ask densities of 1.0, 19.3 and 6.0 on
     * one census line, a nineteenfold spread — which is `over_ask_arms`' own per-arm argument measured. READ
     * from `git log -1 --format=%B 6813a2a` and not re-derived here, so it is that commit's claim and not
     * this file's; what this file asserts is only that the row it adds is denominated in the same measure the
     * bracket is.
     * SECOND, the
     * RESIDUE `span − gap` is inside that one budget and says WHERE in the window the offers sat: at the
     * span the turn went dark immediately, a budget short of it the offers ran to the window's end. THIRD, a
     * value far above the budget IS the violation g_max_gap's own comment defines — "a step that runs five
     * seconds between two consecutive offers is the violation, whatever its total" — and a MAXIMUM
     * establishes it the moment it is OBSERVED, which is the one property a maximum has that a count does not.
     * THE ONE STATE THAT BREAKS THE DERIVATION IS ALREADY A REPORTED ONE, so this is a bound and not an
     * absolute: a consumer that DROPS a TRUE instead of acting on it leaves the turn running, after which a
     * second consultation can answer TRUE and the window argument fails. That is exactly the
     * `requested > fired` state solver/engine.c's seam message prints and names ("a point was reached, the
     * preempt was wanted, and it was DROPPED because no driver at that depth adopts the seam"), so a reader
     * who finds a gap far below `span − ENGINE_QUANTUM_MS` has found that, and not a narrow stretch.
     * MEASURED, AND IT IS WHY THE DENSITY ALONE IS NOT ENOUGH RATHER THAN WHY IT IS WRONG — RELAYED FROM FIVE
     * FRESH-BROWSER DRIVES OF ONE DEV ARTIFACT AND NOT RE-DERIVED HERE, SO IT IS A CLAIM TO CHECK. Two
     * documents sat in OPPOSITE regimes four orders of magnitude apart: three drives of one read 87, 70 and 74
     * consultations over 9, 9 and 11 overrunning turns with ZERO seamless ones — 6.7 to 9.7 each — while a
     * drive of the other read 1010142 over its 28 asked turns, about 36000 each. WHAT THAT BUYS AS A BOUND AND
     * NOT AS A STORY: a turn in this row met the budget, so it ran at least ENGINE_QUANTUM_MS, and nine offers
     * across it is at most one every 1.3 ms — while the turn's own LENGTH is unbounded above by anything on
     * this line, which is exactly why the gap is needed and is the half a density can never supply. BOTH
     * densities are correct and they
     * name opposite mechanisms, and in NEITHER case does the mean say where the time went: the low-ask regime
     * is exactly the one where a handful of consultations are consistent with one enormous gap, and it is the
     * regime the row above was built on.
     * THE UNIT IS THE SLICE'S OWN MEASURE AND THAT IS THE WHOLE OF THE ROW'S VALUE. A gap counted in
     * CONSULTATIONS answers nothing — the gap between two consecutive consultations is 1 by definition — and a
     * gap in WALL milliseconds cannot be compared with anything this scheduler decides on, which is why the
     * wall gap solver/engine.c already computes is PRINTED and never decided upon. `quantum_thread_us()` is the
     * currency the slice is denominated in, so a reading off this row is directly comparable with
     * ENGINE_QUANTUM_MS: "the longest stretch with no consultation was 6400000 us against a 12000 us budget"
     * is a sentence a reader can act on and "9.7 consultations each" is not.
     * IT COSTS ONE CLOCK READ PER CONSULTATION AND THAT IS A CHECKED CLAIM RATHER THAN A HOPE. On the generic
     * and emscripten branch solver/quantum.c's quantum_expired() ALREADY calls quantum_thread_us() — it is
     * `quantum_thread_us() - g_slice_start_us >= ENGINE_QUANTUM_MS * 1000` — and preempt_hook's last clause is
     * `return quantum_expired()`, so on the host that ships this is a SECOND read of a clock that hook already
     * reads per consultation and not a new class of cost. On the native branch quantum_expired() returns a
     * `volatile sig_atomic_t` flag and the read is genuinely new, in a hook that already performs two
     * flow_weight calls plus an O(members) rival rescan on a cache miss. Both brackets are free: the turn's two
     * ends are `t_slice0` and `now`, which engine_sched_step already reads in this same measure in EVERY build.
     * IN EVERY BUILD, for `slice_overrun_asks`' reason exactly and with a sharper consequence. A gap row
     * compiled out in release would read ZERO beside a nonzero ask sum, and a zero maximum gap is the
     * statement that every consultation of every overrunning turn was adjacent to the next — the FLATTERING
     * reading, and the one that retires the step-machine hypothesis this row exists to test. That is the
     * under-claim CLAUDE.md names as the direction nobody discovers by acting on, because acting on it means
     * not looking.
     * IT IS NOT A PARTITION AND SO ITS IDENTITY IS NOT A SUM, which is the one thing a reader of the three
     * rows above must not carry over. `sum(over_arms) == slice_overruns` and its two restrictions are
     * partitions; a sum of MAXIMA is a quantity no turn produced and no reader may compose. What holds is
     * `max over the arms == slice_overrun_gap_us` — the scalar is the same fold of the same per-turn number
     * over the same population — asserted inside the overrun branch where the arm, the turn's two clock
     * readings and the turn's consultation delta are all in one hand, exactly as the three rows above are and
     * never at the accessor. ONE ASSERT AND NOT TWO: the per-arm containment
     * `over_gap_arms[i] <= slice_overrun_gap_us` is ENTAILED by that maximum rather than independent of it, so
     * asserting it beside it would be one fact checked twice and would read as two. That is NOT true of the
     * three partitions above, where a sum identity holds with one arm's subset standing above its own
     * population and another's below it, which is exactly why each of those carries a second containment and
     * this one does not.
     * ITS DENOMINATOR IS THE SERIES LENGTH AND IT IS ALREADY PUBLISHED, which is what makes a ZERO readable.
     * A 0 in this row is two states — this arm had no non-seamless overrunning turn at all, or it had some and
     * every one of their gaps was under a microsecond — and `over_arms[i] - over_seamless_arms[i]` tells them
     * apart, which is the same denominator `over_ask_arms`' density is taken over. So the discriminator is a
     * row this struct already carries and this one needs no sentinel.
     * A MAXIMUM, WHICH `slice_overruns`' OWN BANNER ARGUES AGAINST FOR ITSELF, AND THE DIFFERENCE IS WHAT THE
     * READING IS COMPARED WITH. That banner says "A COUNT AND NOT A MAXIMUM. A high-water mark of turn length
     * would saturate early and then plateau, and a plateau is indistinguishable from a ceiling on a short
     * run", and it is right about a magnitude read against ANOTHER RUN'S magnitude. This one is read against a
     * COMPILE-TIME CONSTANT: the actionable question is whether any stretch of this arm ran far past the
     * budget, which is a FLOOR established the moment it is observed and does not get truer with a longer run.
     * What a reader may NOT do is read it as a ceiling — "gaps never exceed this" is exactly the misreading
     * CLAUDE.md names, and the series length beside it is what bounds how much evidence the figure is.
     * A SEAMLESS TURN IS EXCLUDED AND ITS GAP IS A DIFFERENT QUANTITY WEARING THIS NAME. A turn that offered
     * NOT ONE suspend point has no inter-consultation gap at all: its longest unbroken stretch is its own
     * whole duration, which is already published as the step phase of `slice_us` and is what
     * `over_seamless_arms` counts the turns of. Folding the two together would put a turn's LENGTH and a
     * turn's worst GAP into one row, after which an arm's reading could not be told from its mass.
     * IT IS NOT THE WALL GAP solver/engine.c ALREADY COMPUTES, AND THE TWO POPULATIONS ARE DISJOINT BY
     * CONSTRUCTION. That one (`g_max_gap`, `g_last_ask`) is WALL milliseconds, is DEV-only, and is read by
     * exactly one `DFAILF` whose condition requires the turn's consultation delta to be ZERO — so it reports
     * the whole-turn stretch of a SEAMLESS turn and is printed deliberately beside the work count, because a
     * wall quantity is what says whether the box was also loaded. This row is the slice's measure, is raised
     * in every build, and covers the turns that delta is NONZERO for. Two clocks, two populations, two
     * purposes, and neither is a second copy of the other.
     * A `long` WHERE THE SCALAR IS AN `int64_t`, FOR `over_ask_arms`' REASON EXACTLY AND WITH A DIFFERENT
     * HORIZON. solver/result.c composes every step-unit row through ONE `cold_hist_json`, which takes a `long`
     * array and prints `%ld`, so a 64-bit partition would force a second speller of that row format. This row
     * is a MAXIMUM rather than an accumulator, so a `long` of microseconds is not the 35.8-minute saturation
     * `step_us` is a sum against: the exposure is ONE TURN whose worst gap exceeds about 2147 seconds in the
     * slice's measure, which is an engine that has hung. The narrowing asserts its own headroom at the raise,
     * in the form a DCHECK condition may take, before the cast and never after it.
     * IT DECIDES NOTHING AND BOUNDS NOTHING (§NO BOUNDS), and the hazard is the sharpest of the four: a
     * per-arm worst gap against the budget is precisely the pair a "this arm has not yielded in N ms, take the
     * thread" watchdog would be built from. Nothing branches on either half.
     * HOW ITS ABSENCE SHOWS, as an observation and not an instance: a reader holding an arm whose overrunning
     * turns all offered points divides the ask sum by them, gets a density of single digits, and states that
     * the arm consulted the scheduler regularly and ran anyway — with no row in the artifact that could say
     * those single-digit consultations bracketed one unbroken multi-second stretch instead.
     * NAMED RESIDUAL — THE ROW IS CORRECT AND ITS ONLY READER IS THE FIXTURE-DRIVEN BUILD VERDICT, WHICH IS
     * NOT WHERE THE MEASUREMENT THAT MOTIVATED IT CAME FROM. WHAT IS NOT COVERED: the drives this row exists
     * for were REAL SITES, and `engine/build.mjs`'s `stepUnitOverrunReading` is the one consumer that takes
     * it — so a build's own smoke prints it and a real-site drive does not. That is NOT a correctness gap and
     * nothing refuses it: `testing/live-run.js` declares only `rungEntry`, `fetchEdge` and `xhrEdge` taken
     * WHOLE, so `cold` is a curated list by that driver's own statement and a row it does not carry is its
     * choice rather than a silent drop. It is a REACH gap, and it is the one CLAUDE.md names for a producer
     * whose consumer is not the instrument that measures the subject. WHAT THE NEXT DIFF BUILDS: both names
     * on that driver's cold row lists — `stepUnitOverrunGapArms` beside `stepUnitOverrunAskArms` on its
     * step-unit object list and `sliceOverrunGapUs` beside `sliceOverrunAsks` on its numeric one — which that
     * driver reads with a uniform `k in c`, so an artifact older than the row prints `-` under its own
     * absent-versus-zero rule rather than a 0. HOW ITS ABSENCE SHOWS, as an observation and not an instance:
     * a reader driving a real app meets an arm whose overrunning turns all offered points, divides the ask
     * sum by them, and states a per-turn density — with the row that would say whether those consultations
     * bracketed one unbroken multi-second stretch present in the artifact and absent from the only report
     * that drive produces. MEASURED ABSENT with the command, so this is a claim and not a recollection:
     * `grep -cE 'stepUnitOverrunGapArms|sliceOverrunGapUs' testing/live-run.js` answers 0, against
     * `grep -cE 'stepUnitOverrunAskArms|sliceOverrunAsks'` answering 6 as the armed control and an invented
     * `zzNoSuchRowEver` answering 0. It is not taken in the same diff because that file is one FOUR peer
     * commits landed in within the hour this row was written, and it is outside this change's scope.
     * AND THAT DRIVER IS THE ONE THAT WANTED THIS ROW, WHICH IS WHY THE RESIDUAL IS WORTH MORE THAN A NOTE:
     * the commit withdrawing a per-arm span for want of one (`6813a2a`) is a commit to THAT FILE, so the
     * consumer whose banner had to replace a figure with a 1352x bracket is the consumer not carrying the row
     * that would narrow it to one budget. A producer and a frustrated reader in one tree with nothing joining
     * them is the write-with-no-reader shape at the one place it costs a reading rather than a byte.
     * RETIREMENT: this goes when a seam verdict in engine_sched_step can judge a turn whose consultation
     * delta is NONZERO — i.e. when a verdict decides on THIS gap against the budget rather than on a turn
     * that offered nothing — because the existing reader then names the same population per arm and this row
     * is a second copy of it. MEASURED ABSENT with the command rather than asserted, so the condition is not
     * born met: `grep -c 'g_preempt_asked == pa0' engine/host/solver/engine.c` answers 2, which is BOTH
     * verdicts in that function carrying the conjunct, against `grep -c ENGINE_SEAMLESS_CPU_US` answering 1
     * as the armed control and an invented `seamGapVerdict` answering 0. */
    int64_t  slice_overrun_gap_us;    /* the worst no-suspend-point stretch of any non-seamless overrunning turn */
    long     over_gap_arms[STEP_UNIT_N];
    /* …AND THE ONE PHASE OF A START STEP THAT `slice_overruns` AND `over_arms` CAN LOCATE TO AN ARM AND
     * NEVER TO A PHASE. (This sentence said `the two rows above` until two rows were inserted between it and
     * them — a reference by POSITION resolves to whatever now occupies that position, which is why it
     * carries their NAMES now.) A start is a COMPILE and then an EXECUTION, and only the second runs
     * bytecode.
     * THIS PARAGRAPH'S HEADLINE READ `THAT CANNOT REST AT ANY INPUT SIZE`, and derived it: quickjs raises its
     * yield request from exactly four kinds of site of which three are the interpreter's own dispatch, so a
     * parse offered no raise point for its whole length, and its length is `body_n`, the page-chosen quantity
     * solver/rest_unit.h's bound (1) forbids in a step's cost. It is REWRITTEN RATHER THAN DELETED because
     * the derivation is still exactly right about the INTERPRETER and a reader will re-derive it. It is no
     * longer right about the PARSE: JS_FlowCompileStep polls the same hook from the parse's own production
     * dispatch and hands the parse back through `f->compile`, so a compile now RESTS.
     * AND THAT MOVED ONE ROW'S SUBJECT AND NOT THE OTHER'S, WHICH IS THE WHOLE OF WHAT A READER OF THIS PAIR
     * HAS TO KNOW. `classic_compiles` is ONE PER PROGRAM — raised at the stint that finishes a parse — and
     * `classic_compile_overruns` is ONE PER STINT whose own duration met the slice. They are two counters
     * raised at DIFFERENT EVENTS, so NEITHER IS A SUBSET OF THE OTHER and their quotient is not a rate: a
     * program parsed over several overrunning stints contributes several overruns and one compile. The
     * containment that does hold is against the stint population, which is DERIVED rather than counted
     * separately, because every stint ends in exactly one of two already-counted arms:
     *     compile STINTS == classic_compiles + arms[`compile-handed-the-thread-back`]
     * and `classic_compile_overruns <= that sum` is asserted at engine_step_unit_runs where all three are in
     * one hand. EVERY ROW HERE IS THE CLASSIC PHASE'S AND SAYS SO IN ITS NAME, which became load-bearing when
     * §8.1.4.4 "Calling scripts"' MODULE entry gained the same rest seam: a module stint parks into the same
     * `f->compile` and names `module-compile-handed-the-thread-back`, so it is in NONE of these three rows and
     * in none of that sum. That is what keeps the line above an EQUALITY — see solver/step_unit.h, where the
     * decision is recorded at the arm, and note that the module phase has no counters of its own BECAUSE its
     * entry compiles and evaluates in one call, so there is no span a bracket could time that is a parse. `classic_compile_overruns <= slice_overruns` also still holds by construction and is checked
     * there; that one additionally rests on a turn reaching the compile at most once, which its own comment
     * names.
     * READ AS A PAIR AND AGAINST A THIRD NUMBER, never alone: `classic_compile_overruns` says whether the rest
     * seam is firing often enough inside a parse for a stint to stay under the slice, and `classic_compile_again`
     * says whether the compile is REPEATED. Those are three different diffs — the bytecode between the page's
     * own raise points, a per-flow materialization ceiling, and the GRANULARITY of the parse's rest point — and
     * a reader holding either row by itself cannot tell them apart.
     * THE REPEAT CLAUSE READ `classic_compiles AGAINST THE PROGRAMS A DOCUMENT REACHED SAYS WHETHER THE COMPILE
     * IS REPEATED PER FLOW`, AND THAT IS A DIFFERENCE BETWEEN TWO POPULATIONS OF DIFFERENT WIDTH. It is kept in
     * its own words because it is the reading two counters printed side by side invite, and because a brief was
     * written out of it and a lane dispatched on it. `classic_compiles` counts every FLOW and every TIMELINE —
     * a fork parses every later program of its inherited sequence itself, and an @S candidate session and a
     * cold-resumed replay re-run the document from the baseline — AND every APPENDED row as well as every seeded
     * one, which for a modern bundle means dozens of lazy chunks of DISTINCT BYTES. The nearest published
     * denominator, `root_programs`, is narrower on both axes, so a figure many times it is what a healthy run of
     * such a page MUST read. The tree records the identical defect one row over for `progStarts` against
     * `rootPrograms` (solver/flow.c), where the two agreed by coincidence on one document.
     * WHAT REPLACES IT IS AN OBSERVATION WITH NO DENOMINATOR IN IT. `classic_compile_again` is the parses whose
     * BYTES some flow of this process had already parsed to completion — the identity being the BODY and not a
     * hash of the source text, which CLAUDE.md §ONE-global rules out because a minified bundle repeats one-line
     * bodies and a hash names a SET. It is a FLOOR and `classic_compile_own_decode` is its bound: a reply is
     * decoded PER DELIVERY, so two arms parked on one external row hold two buffers over one chunk and a repeat
     * between them is unobservable, which puts the true figure in [`again`, `again + own_decode`].
     * THE TWO SUBSETS ARE TWO PARTITIONS OF ONE POPULATION AND MAY NOT BE ADDED TO EACH OTHER — a parse can be
     * both a repeat and a per-flow decode — and each is contained in `classic_compiles`, asserted at
     * engine_step_unit_runs where all four are in one hand.
     * WHY A COUNT AND NOT A TIME, which `over_arms` above already argues for its own axis: a count answers
     * WHICH SPAN DID NOT REST, a time answers WHERE THE RUN WENT, and only the first is what §NO BOUNDS'
     * suspend-at-any-depth requirement is about.
     * THEY DECIDE NOTHING AND BOUND NOTHING (§NO BOUNDS): no source is refused for its length, no compile is
     * capped and no arm is skipped on either reading.
     * RETIREMENT: they go when the parse is a pull whose GRANULARITY solver/rest_unit.h OWNS. This clause
     * read `when a compile can REST` apposed to that, as though the two were one condition; a compile can
     * rest NOW and rest_unit.h declares no JS-production kind, so a reader checking the old clause would
     * have retired the only rows reporting on the new seam. When the ask moves to rest_unit_items, a stint
     * that met the slice is an ordinary preempted span and these have nothing left to report. */
    long classic_compiles;           /* classic program compiles, ONE PER PROGRAM, at flow_step's start site */
    long classic_compile_overruns;   /* compile STINTS that met the slice — NOT a subset of the row above */
    long classic_compile_again;      /* …of which the bytes had ALREADY been parsed by some flow: the REPEAT */
    /* AND WHAT THOSE RE-PARSES COVERED, WHICH IS THE ONLY FORM IN WHICH THE REPEAT IS A COST. A count of
       repeats says nothing about whether the diff §A-CAPABILITY-MATERIALIZED-PER-FLOW would justify is worth
       making: twenty repeats of a 200-byte inline script and twenty of a 1.4 MB chunk are the same number and
       two different answers. int64_t for `step_us`' reason, and the arithmetic is worse — the second of those
       overflows a four-byte total after about fifteen hundred repeats and reads NEGATIVE, which reads as a
       cheap engine. Asserted against its own count at engine_step_unit_runs, which is what catches that. */
    int64_t classic_compile_again_bytes;
    long classic_compile_own_decode; /* …whose body ONE flow decoded for its own delivery: the floor's bound */
    /* AND THE COMPLEMENT OF `classic_compiles`, WITHOUT WHICH THAT ROW FALLING IS UNATTRIBUTABLE — the classic
     * programs started from a parse another timeline had already finished. A shared parse raises none of the
     * four rows above: it parses no bytes, so it is not a compile, not a repeat, not a per-flow decode and not
     * a stint. That is exactly what makes `classic_compiles` alone ambiguous once parses are shared — a
     * document whose timelines share them reads as a document with fewer programs, which is the reading that
     * cannot be told from an engine that STOPPED STARTING THEM. Read the two together:
     *     classic_compiles + classic_parse_shared == the classic programs whose closure was obtained at all
     * and that sum is what is comparable with a pre-sharing revision's `classic_compiles`, while neither half
     * is. The mechanism itself, its key and why it is not a cache with an eviction policy are at
     * solver/dyn_body.h; the repeat this row is the other side of is `classic_compile_again` above.
     * IT IS A SUBSET OF NOTHING HERE and is not asserted against anything: it is raised in the arm where the
     * parse does not happen, so it partitions the programs STARTED and not the parses performed, and adding it
     * to either partition above would be adding answers about different events. What IS asserted at
     * engine_step_unit_runs is the implication — a share requires that some parse finished — and the gauge
     * `dyn_body_parses_held` against this row's own total.
     * `classic_compile_again` STAYS NONZERO BY DESIGN and a reader must not take it as the sharing failing: a
     * program whose parse FAILED holds nothing (an exception is one flow's completion, not a fact about the
     * bytes), and a flow already mid-parse of a row that another flow finished in the meantime resumes its own
     * parse to the end rather than abandoning it. Both really do parse bytes twice.
     * RETIREMENT: this row goes when `classic_compiles` is no longer read as a count of programs started —
     * that is, when the census publishes programs STARTED directly and the compile row is read only as a cost.
     * `prog_starts` is that number for every kind at once, so it is not it. */
    long classic_parse_shared;       /* classic programs started from a parse another flow had finished */
    /* AND HOW MANY OF THE STINTS CONTINUED A PARSE RATHER THAN BEGINNING ONE — the row without which
     * `compile-handed-the-thread-back` cannot say whether a parse handed back is a parse CARRIED FORWARD. The
     * stint population is already exact (the two arms above), and this partitions it on the one axis that
     * separates the two readings a small `classic_compiles` admits: subtract it from that arm and what is left
     * is the parses begun and NOT ENDED, because an ended parse of k stints parks k-1 times and resumes k-1
     * times while one still in flight parks k times and resumes k-1. Near zero is a seam carrying every parse
     * forward, so a program count under a document's row count is a BUDGET; large is parses being handed back
     * and not picked up. The engine's own derivation, the reachability argument for its zero and the reason it
     * is a count rather than a high-water mark are all at solver/engine.c's g_classic_compile_resumed.
     * IT IS NOT A SUBSET OF `classic_compiles`, so it is not asserted against it: the two are raised at
     * DIFFERENT EVENTS — one per stint that continued, one per parse that ended — which is the same
     * relationship `classic_compile_overruns` has to it and fails the same way if it is read as a rate. What it
     * IS contained in is the yielded arm, asserted at engine_step_unit_runs where both are in one hand. */
    long classic_compile_resumed;    /* compile STINTS that CONTINUED a parse — NOT a subset of the programs */
    /* WHY THE TURNS THAT DID NOT END A UNIT OF WORK DID NOT — the three-state answer behind `_unitsDone`
     * reading low, and the rows a reader needs before that number means anything at all.
     *
     * `_unitsDone` is a GATED count: the dispatch loop credits a unit only when a conjunction of three clauses
     * holds, so a low reading is consistent with a thread that did nothing AND with a thread that spent every
     * turn advancing programs it never got to finish. Those take opposite work — the first is a question about
     * where the turns went, the second is the frame gate showing up in a throughput row — and until these rows
     * existed the document could not tell them apart, because the refusal side of that gate was counted
     * nowhere. `mid_program` dominating is the second reading, and it is the expected shape of a forking
     * frontier: an arm is born holding the frame taken AT its branch, so it is inside a program by
     * construction.
     *
     * THEY ARE IN THIS STRUCT BECAUSE `steps` IS, AND `steps` IS THE DENOMINATOR. The four arms —
     * these three and the credited one — sum to `steps` exactly, asserted in engine.c at the line the
     * credited arm is written on, where all four are in one hand. That matters more than tidiness: the
     * document carries `_unitsDone` and `steps` in two DIFFERENT objects, so a reader composing the split
     * across them is composing it across two censuses that share no identity, and the assert is the only
     * thing that makes the composition legitimate. The credited arm is deliberately NOT repeated here — a
     * second spelling of one number in one document is the drift the record-field gate exists to catch.
     *
     * THEY DECIDE NOTHING AND BOUND NOTHING (§NO BOUNDS). No weight term reads them, no fork carries them and
     * nothing declines work on them; a per-turn refusal count is exactly the shape a watchdog on a flow that
     * "never finishes anything" would be built from, which is why that is said here as well as at the
     * counters. A frontier whose members are all mid-program is the design running, not a population to shed. */
    long unit_mid_program;      /* …the member held a live frame: inside a program, the trial still running */
    long unit_parked;           /* …the runtime held a parked continuation: suspended on an await or a reply */
    long unit_checkpoint_owed;  /* …the flow still owed its microtask checkpoint: its own reactions unrun */
    /* HOW MANY DESCENTS REACHED THE CLOCK BOUNDARY — THE DISCRIMINATOR ITS THREE OUTCOME ARMS STRUCTURALLY
     * CANNOT BE. `queue-rendering-opportunity`, `fire-due-timer` and `start-or-run-an-idle-period` are arms of
     * `arms` above, raised only when their hook TAKES the step, so each is a census of an OUTCOME over a gated
     * operation and a 0 there is two opposite things: the rung was reached and the clock legitimately had
     * nothing due, or no descent ever got far enough to ask it. Those take opposite work — the first is a fact
     * about the page's own timers and frames, the second a fact about the arms ABOVE this boundary — and
     * §AN-INVARIANT-OVER-A-GATED-OPERATION says the ask is recorded upstream of every arm that may legitimately
     * decline. solver/engine.c raises these AT the arm, ahead of the gate, and nothing here is relocated: the
     * three outcome arms stay exactly where they were, because moving an observation changes what its number
     * means and leaves the old meaning unread.
     *
     * THEY ARE SUFFIX SUMS, WHICH IS WHY THEY ANSWER WHAT `arms` CANNOT AT ANY VALUE. The three rungs are
     * consecutive arms of ONE `else if` chain: `clock_render_asks` counts every descent that reached that chain
     * at all, `clock_timer_asks` those the rendering rung did not take, `clock_idle_asks` those the timer rung
     * did not take either. `arms` is a PARTITION of outcomes, so `arms[k] == 0` says THIS ARM NEVER TOOK A
     * DISPATCH and never says the arm was not reached — an upper clock arm at 0 with a LOWER one nonzero is
     * that rung correctly declining every descent it was handed, not a localisation. The cumulative quantity is
     * the suffix sum, and these are it at the three points a clock rung can be asked.
     *
     * THE FOUR IDENTITIES ARE CHECKABLE FROM THE ROWS BESIDE THEM, WHICH IS WHY NO SUM IS PUBLISHED. A second
     * spelling of one number in one document is the drift the record-field gate exists to catch, and every
     * operand of all four already lands on the same census line:
     *     clock_render_asks == the ELEVEN arms of `arms` at or below this boundary (solver/engine.c names them)
     *     clock_timer_asks  == clock_render_asks - arms[STEP_UNIT_RENDERING]
     *     clock_idle_asks   == clock_timer_asks  - arms[STEP_UNIT_TIMER]
     *     clock_render_asks <= unframed_steps
     * All four are asserted at engine_step_unit_runs, where every operand is in one hand. The last is what
     * licenses reading these against `unframed_steps` in the document at all: both are raised per PASS through
     * the same `if (!f->frame)` block — that block's own entry comment predicted this pair — so a descent that
     * reached the chain is a descent that entered the block, and an ask above it is the chain having been
     * reached from somewhere else.
     *
     * LIFETIME COUNTS, PER INSTANCE, RELEASED BY NOTHING — the same scope as `arms` and `unframed_steps`, which
     * is what keeps the identities true across a session restart. They may be differenced and accumulated, they
     * cannot decrease, and a sample below its predecessor is this engine and not the run. The scheduler's
     * `g_orphan_asks` takes the OPPOSITE scope one rung up (engine_session_close clears it) and is the wrong
     * precedent to copy here for exactly that reason.
     * A REPORT AND NEVER A BOUND (§NO BOUNDS): nothing branches on one, no arm is narrowed by one, and no rung
     * is skipped because one is large.
     * RETIREMENT: these three go with `unframed_steps` and on its own condition — when solver/step_unit.h
     * declares each arm's side of `if (!f->frame)` AND of this boundary, the suffix sums are a sum over `arms`
     * that cannot disagree with flow_step, and neither this triple nor solver/engine.c's hand-written
     * eleven-unit list has anything left to carry. */
    long clock_render_asks;  /* descents that reached the rendering rung: the clock boundary's arrival count */
    long clock_timer_asks;   /* …of which the rendering rung declined: the timer rung's arrival count */
    long clock_idle_asks;    /* …of which the timer rung declined too: the idle rung's arrival count */
} EngineStepUnitRuns;
void engine_step_unit_runs(EngineStepUnitRuns *out);

/* ---- WHICH ARM TOOK THE STEP OF A MEMBER HOLDING A RUNNABLE TASK ----------------------------------------
 *
 * THE HISTOGRAM ABOVE CANNOT BE ASKED THIS. `step_unit_runs` is over EVERY step, so an arm cannot be
 * attributed to the members that had a task standing on their own queue — two documents with the same arm
 * histogram and opposite job backlogs are one reading there. These four are raised at the two arms of
 * flow_step that stand above the task arm and CAN be reached with a task runnable, plus the task arm's own two
 * reasons, so a reader holding them can say which arm the backlog is behind instead of inferring it.
 *
 * READ THEM AGAINST `jobsReadyTask` AND `run-a-task` AND NOT ALONE. `jobsReadyTask` is a GAUGE of what waits
 * and these are LIFETIME counts of what was taken instead; `run-a-task` on the @COLD line is the arm's own
 * step count and the last two of these partition it. The four sizes:
 *   `taskHeldDelivLifetime` SIZES A DIFFERENT THING EITHER SIDE OF THE STAMP, AND THE RETIRED WORDING IS
 *     KEPT BECAUSE A READER WHO FINDS THE ROW LARGE RE-DERIVES IT FROM THE ARM'S POSITION IN THE LADDER. It
 *     read: a large one says the networking delivery arm — which is NOT in the arrival order, by its own
 *     header's NOT COVERED clause — is what stands in front of the queue, and the diff is the stamp that
 *     folds it into that order. THE STAMP IS BUILT: a `pending` entry carries PEND_WORK_SEQ and the arm asks
 *     flow_task_precedes with it, so a delivery no longer precedes a QUEUED CALLBACK that arrived first and
 *     the row is now the size of the arm going in front of a YOUNGER task, which is that order WORKING. It
 *     still precedes every ROW, which is the half left open and is the residual stated at the arm.
 *     SO TWO CENSUSES ARE COMPARABLE ON THIS ROW ONLY IF THEIR ARTIFACTS AGREE ABOUT THE STAMP, and no row
 *     emitted anywhere says which of the two quantities a reader is holding — a census written before the
 *     next install reads the old one and the first after it reads the new one, in one column, silently. The
 *     discriminator is therefore outside the census and is the artifact's own stamped revision:
 *     git grep -c PEND_WORK_SEQ <that revision> -- engine/host, asked at BOTH passes before differencing.
 *     SOLVER/ENGINE.C STATES THE MECHANISM AT `g_task_held_deliv` AND IS THE COPY TO READ: it is the RAISE
 *     site, this is the declaration, and a declaration states an intention where a raise states what the
 *     counter got. This copy is the one that went stale when the stamp landed.
 *     RETIREMENT: this record goes when the census emits whether the arrival stamp was COMPILED IN beside
 *     the row, so the two quantities are told apart FROM the document and no out-of-band revision question
 *     is owed — MEASURED ABSENT with the command, so this condition is not born met:
 *     `grep -c taskArrivalStamped engine/host/solver/result.c` answers 0 against `grep -c
 *     taskHeldDelivLifetime engine/host/solver/result.c` answering 2 as the armed control.
 *   a large `taskHeldSeqLifetime` says the arrival comparison is answering NO: the document's remaining rows
 *     are OLDER than what its own code queued, so the sequence goes first. The diff is at that comparison.
 *   `taskArmOlderLifetime` above zero REFUTES both for the steps it counts — the comparison does hand the
 *     queue the thread ahead of a startable row.
 *   `taskArmNoRowLifetime` is the arm reached with no row at all, which on a real page is most of it, and it
 *     is the row that says the sequence is not what excludes those members.
 * ITS IDENTITY is `taskArmOlderLifetime + taskArmNoRowLifetime == run-a-task`, asserted in
 * engine_ladder_task_census where both halves and the histogram are in one hand.
 * LIFETIME, all four, released by nothing. A REPORT AND NEVER A BOUND (§NO BOUNDS) — solver/engine.c states
 * the rest at the counters, including the two populations no row here reaches. */
typedef struct {
    long task_held_deliv;   /* the reply-delivery arm took a step with a task runnable on the member's queue */
    long task_held_seq;     /* the program-sequence arm took it: `seq_compiles && !job_precedes`, task runnable */
    long task_arm_older;    /* the task arm ran because the queued task was strictly older than the cursor's row */
    long task_arm_no_row;   /* …and because there was no row to compare against at all (`seq_compiles` 0) */
} EngineLadderTaskCensus;
void engine_ladder_task_census(EngineLadderTaskCensus *out);

/* ---- THE FRONTIER'S OWN NUMBERS, AS ONE READING ----------------------------------------------------------
 *
 * Every row here is a static of engine.c that had EXACTLY ONE consumer — the `@COLD`/`@PROGRESS` printfs in
 * `run_scheduler` — and `run_scheduler` is reached only through `engine_run`, which the smoke fixture calls and
 * nothing else does. The extension's ABI drives `engine_sched_step` directly and never enters that loop, so
 * the pager's own accounting, the host-payment pair and the frontier's retirement count were computed on every
 * census of every production run and printed on none of them. That is §Testing's "measure what the shipped
 * path writes" with the writer on the wrong side of it: not a wrong number, a number about a host nobody runs.
 *
 * IT IS A FILLED STRUCT AND NOT TWELVE GETTERS FOR THE REASON `WfqCensus` IS ONE: the rows are a READING OF AN
 * INSTANT and must be taken together, so a caller that assembles them from separate calls is a caller that can
 * assemble them from two instants. solver/result.c renders it; this component decides what it holds. The
 * accessors above stay accessors because each of them is a TOTAL that any caller may ask for on its own.
 *
 * WHAT IS DELIBERATELY NOT HERE: the count of orphan drives (`engine_orphan_census`, which the document
 * already carries under its own name — a second spelling of one number in one document is the drift the
 * record-field gate exists to catch) and the RUNNING flow's cursor (a sample of one flow, which `deepest` and
 * `completed` answer as facts about the DOCUMENT). */
typedef struct {
    /* ─── THE TWO RETIREMENT TOTALS, EACH BESIDE THE TWO POPULATIONS IT IS THE SUM OF ────────────────────
     *
     * A FRONTIER HOLDS TWO POPULATIONS AND ONE COUNTER CANNOT REPORT ON EITHER. `finished` and `sold` are
     * over every member, and a member is one of two things: an EXPLORATION flow (a boot fork, a branch arm, a
     * loop arm, an orphan drive) or an @S CANDIDATE SESSION — solve.c's re-fire of one derived breakout,
     * which is a flow on the ONE frontier exactly as CLAUDE.md §THERE-IS-NO-GRIND requires and is NOT a
     * second executor. The two retire for OPPOSITE reasons and take OPPOSITE work: an exploration flow that
     * ran to its end is coverage this document actually gained, while a candidate MEMBER that ran to its end
     * is search this document spent on a derived payload that did not fire.
     * Summed, "the engine retired 47 flows" and "the search spent itself 47 times over" are one number, and
     * the reader cannot tell which it is holding.
     *
     * THE LABEL IS `Flow.cand_src` AND IT IS A BINARY PARTITION BY CONSTRUCTION. One field, set at birth
     * (solve.c's seed, engine_sibling_assemble's copy, cold.c's 'c' record), never cleared, freed only at
     * flow_release — so every member is on exactly one side of it at every instant of its life, and the two
     * arms below cannot overlap or leave a member out. That is why this is TWO rows and not three: being a
     * DRIVEN ORPHAN (`Flow.orphan`) is a separate field, and a drive seeded from a candidate parent inherits
     * the substitution AND gets the mark, so a candidate/orphan/plain split would not be a partition at all
     * and its "sum" would over-count the members that are both.
     *
     * BOTH ARMS COUNT MEMBERS AND NEITHER COUNTS SESSIONS, AND THE ROWS BELOW USED TO SAY OTHERWISE — kept
     * in their own words there because a reader who re-derives ONE ROW PER CANDIDATE from the word CANDIDATE
     * will write it again. `finished_cands` read "the @S candidate sessions: derived payloads that ran and did
     * not fire" and `sold_cands` "the candidate sessions", and each is raised ONCE PER MEMBER: flow_finish is
     * the one line a member ever completes on, engine_reclaim_tail the one line a member is ever sold on, and
     * both read `Flow.cand_src` off the member in front of them. A CANDIDATE SESSION IS A TREE OF MEMBERS.
     * engine_sibling_assemble copies `cand_src`, `cand_payload`, `cand_sink`, `cand_fired` and `cand_resumed`
     * to every sibling of a parent that has them, with its own assert making a sixth field an obligation
     * there; the copy is gated on the parent HAVING a substitution and on nothing else, so no arm of a
     * candidate is refused a fork or leaves one without the label — and N arms of ONE seed therefore each
     * reach flow_finish and each raise this count once. solver/solve.c records the same category error
     * being removed from an assert one component over, where `Cand.ends` counted flow finishes and
     * `Cand.tried` counted seeds; this is that unit wearing the other name in the EMITTED census, where a
     * reader holding one document has nothing to read it against.
     *
     * THE PER-SESSION COUNT IS ALREADY EMITTED AND IS `_candidates` — solve.c's `g_cands_seeded`, raised once
     * per candidate RUN seeded and zeroed by `solve_init`, which solver/result.c's own record establishes runs
     * EXACTLY ONCE IN AN AGENT'S LIFE on all three hosts, read off the CALL and never off a host. These rows
     * are lifetime totals of the same instance (neither is on engine_sched_begin's reset path), so the two
     * span the same thing and may be read against each other. WHAT MUST NOT BE WRITTEN IS AN
     * INEQUALITY BETWEEN THEM: arms legitimately outnumber seeds, so `finished_cands <= _candidates` is the
     * wrong-unit implication solve.c has just finished deleting and a dev build would die on it at the first
     * candidate's second arm. Read the other way it is an OBSERVATION and not an assert — a session cannot
     * finish more times than it was seeded, so `finished_cands` standing ABOVE `_candidates` in one document
     * is the excess being arms and can be nothing else.
     *
     * THE EMITTED KEY IS NOT RENAMED, AND THAT IS A CROSS-BOUNDARY DECISION RATHER THAN A PREFERENCE.
     * `finishedCands` is composed by solver/result.c's result_cold_json into `qjs.wasm` and read by
     * engine/build.mjs, which is INTERPRETED FROM THE TREE and therefore live on write; `coldFields()` derives
     * its required set from that composer's own format string, so a rename leaves the reader demanding a key
     * the shipped artifact does not emit and every run against an artifact older than the commit throws.
     * CLAUDE.md §A-CROSS-BOUNDARY-DIFF: the halves land together or not at all, and the C half needs a build.
     * It would also re-point every archived-log query at once. So the unit is stated at the declaration, at
     * the raise, and in the consumer's own English, and the key is renamed by whoever lands the row below with
     * a build behind it.
     *
     * WHAT IS NOT COVERED: no row anywhere says how many candidate SESSIONS have ended, which is the question
     * `_candidates` is the denominator of and the one a reader asking "how many payloads were discarded" has.
     * It is derivable from neither arm — a session ends when its LAST member ends, and nothing counts a seed's
     * live members. THE NEXT DIFF builds that where the members are known, which is solve.c's per-`Cand`
     * accounting and not this census: a live-member count per seed, decremented at the seam that already
     * reaches `Cand.ends`, and the seed credited when it reaches zero. HOW ITS ABSENCE SHOWS: a reader holding
     * a census can state how much search was spent in members and cannot state how many payloads were
     * discarded, so every sentence a consumer composes about payloads discarded is a sentence about members.
     * RETIREMENT: this record goes when that row is emitted beside `_candidates`, because the unit is then
     * readable off the document rather than argued here.
     *
     * THE TOTAL STAYS, AND THE PARTITION IS ASSERTED AGAINST IT — the same discipline solver/cold.h's
     * `step_units` keeps against `flows`. Each arm is incremented beside its total at the one site that
     * total is written at, so the identity is what a retirement path added later without a label breaks;
     * engine_frontier_census is where all three are read together and is where it fires. */
    long finished;          /* flows that ran to their end — `finished_flows + finished_cands`, asserted */
    long finished_flows;    /* …the EXPLORATION flows among them: coverage this document gained */
    long finished_cands;    /* …and the @S candidate MEMBERS among them: search spent on payloads that did
                               not fire. MEMBERS AND NOT SESSIONS — see above; `_candidates` is the sessions */
    long sold;              /* flows this instance PAGED OUT — `sold_flows + sold_cands`, asserted; see
                               g_flows_sold */
    long sold_flows;        /* …the exploration flows among them */
    long sold_cands;        /* …and the candidate MEMBERS — not sessions, see above — which is the sharper
                               half of the pair: a parked candidate comes back WITHOUT its ladder
                               (solver/flow.h — `cand_surv` and `cand_rung` are readings of a re-execution and
                               deliberately do not cross the tier), so paging one costs the search the
                               distance it had measured. */
    long forks;             /* decide.c's fork total: how many times the decision seam split a flow */
    /* THESE TWO ARE OVER THE SAME MIXED POPULATION AND ARE DELIBERATELY NOT SPLIT, which is a different
       answer from the one above and rests on a different fact. They are MAXIMA, not sums, and a candidate
       session "runs from the baseline" and "re-runs the document from the baseline" (solve.c) — it compiles
       and completes this document's own programs, consuming the detecting flow's recorded arms. So a program
       index reached only by a candidate is still a program THIS DOCUMENT reached, and the row is true of the
       document whichever population set it. Splitting them would answer "which population got there first",
       which is a question about the schedule and not about the document's coverage. */
    int  deepest;           /* highest program this document has STARTED */
    int  completed;         /* highest program it has run to its END */
    /* AND THE ROW THE CURSOR HISTOGRAM IS ONE PAST, WHICH IS NEITHER OF THOSE TWO. `deepest` is the deepest
     * program STARTED and this is the deepest ROW any flow has LEFT, started or not. The gap between them is
     * the one thing the three rows together can say and no two of them can: `deepest -1 / deepestLeft 0`
     * is a document that reached its first <script> and ran nothing at it, which on a page whose external
     * scripts 404 is the ordinary state and not an error. HTML §4.12.1.1 "Processing model"'s "execute the
     * script element" step 4 is that arm — "If el's result is null, then fire an event named error at el,
     * and return" — a row the cursor passes and the compile never sees, so the two numbers COME APART by
     * design and a reader who took `deepest` for how far the document got was reading past every skip.
     * IT IS WHAT THE `programCursors` IDENTITY IS ASSERTED AGAINST, in solver/result.c, and it is the only
     * one of the three that can be: the cursor moves for a row LEFT and `deepest` moves for a program
     * STARTED, so asserting the histogram against `deepest` charged every correct skip as a defect. See
     * solver/cold.h, which declares the identity, and engine.c's g_deepest_left for why merging the two
     * costs a diagnosis in whichever direction it is merged. */
    int  deepest_left;      /* highest row of its sequence any flow has LEFT — started or skipped */
    /* AND THE DENOMINATOR THOSE TWO ARE READ AGAINST, WHICH IS THE ONE NUMBER NEITHER CARRIES AND NOTHING
     * ELSE ON THIS LINE SUPPLIES. `deepest 7` says some flow started the eighth program; whether that is the
     * WHOLE of a document or a third of it is not derivable from any other row. The cursor histogram's extent
     * is taken from the LIVE MEMBERS rather than from the document, `progStarts` counts STARTS across
     * timelines rather than rows, and a fork COPIES a sequence rather than extending it. So "the document's
     * own scripts all ran" and "most of them were never reached" read identically — and they take opposite
     * work, the first sending a reader to the chunk-discovery path and the second to the order.
     * MEASURED AS A DEFECT RATHER THAN A HAZARD: a landed analysis of a real 4.5 MB bundle read `progStarts`
     * as the document's script count and reported `24 - 8 = 16` scripts that never start. The subtraction was
     * of two different things, and the sentence it produced was quoted into briefs.
     * IT IS THE SEED LENGTH AND NOT THE SEQUENCE LENGTH, which is the one thing it must not be read as. A
     * flow's sequence is these rows FOLLOWED BY every program the run queued into it — a lazy chunk, an
     * injected <script>, an @S candidate — so `deepest` may legitimately exceed `rootPrograms - 1` and may
     * legitimately fall short of it. No inequality holds in either direction, which is why there is
     * deliberately no assert between them: one would fire on a healthy run that reached a queued chunk.
     * ZERO IS A STATEMENT AND NOT A HOLE. A census taken outside a live session reads 0 because the seed
     * table is given back when a session closes (engine_session_close), and a document with no executable
     * <script> element reads 0 for the reason the seed's own type assert gives. */
    int  root_programs;     /* rows the ROOT DOCUMENT'S OWN <script> elements seeded into every flow of it */
    /* …AND THE PARTITION OF IT THAT SAYS WHETHER THIS RUN EVER LEARNED AN ENDPOINT. A seeded row either
     * already holds its source text or its bytes are still owed by the reply door, and the second kind is
     * what the bundle itself costs in reply-door openings. `replyAsked` has never had a denominator, so
     * `replyAsked == rootProgramsAwaitedAtSeed` — the run asked for exactly its own bundle and nothing else, so no
     * page `fetch()`, no XHR and no dynamic `import()` was ever reached — was a reading taken by counting a
     * document's `<script src>` elements by hand. Both arms are written at the one line the total is. */
    int  root_programs_held_at_seed;    /* …whose source text this instance HAD WHEN THE ROWS WERE SEEDED:
                                   inline, or external and already fetched, both of which the seed makes a
                                   DYN_PAGE_SCRIPT. Past tense on purpose — see solver/engine.c, where both
                                   arms are written at one line and never again */
    int  root_programs_awaited_at_seed; /* …and whose bytes the reply door owed AT THAT MOMENT — the seed makes each of these a
                                   DYN_SCRIPT_SRC that parks its flow, so this is the bundle's own share of
                                   `replyAsked` and everything above it is something the RUN reached */
    /* ─── AND THE COUNTS THOSE TWO MAXIMA CANNOT CARRY, WITH THE ASK THE CANDIDATE ARM IS MEASURED AGAINST ──
     *
     * `deepest` and `completed` are MAXIMA and answer how FAR, so neither can answer how MANY, or WHOSE.
     * A run reading `deepest: 12` may have started twelve programs or twelve thousand, and nothing above says
     * whether ONE of them was an @S candidate — which is the question §@S's only-firing rule turns into
     * the whole standard of proof for a security finding, since a constructed PoC that is never STARTED cannot
     * fire and reports exactly as one that ran and did not.
     *
     * THE PAIR THAT ANSWERS IT IS `progQueuedCand` AND `progStartsCand`, AND NEITHER IS READABLE ALONE. The
     * queue side is the solver's ASK, raised where the row is created and therefore upstream of the pick, the
     * compile and the destroyed-document walk — every one of which may decline a candidate for a good reason
     * (a breakout that does not fit its sink's context correctly never parses). The start side is the same
     * question asked of the scheduler. `0/0` says no breakout ever reached an executable position, so there
     * was nothing to run; `0/N` says N were constructed and queued and the frontier never handed a member the
     * thread at one of their rows. Those take opposite work — the first is solve_html.c's derivation and the
     * second is the WFQ — and until both rows existed they were one silence.
     *
     * `progStarts` IS THE THIRD DISCRIMINATOR AND IT IS THE ONE FOR THE RUN RATHER THAN THE SEARCH. A run that
     * started NO program of any kind has said nothing about candidates, and it reads identically to one that
     * started thousands with no candidate among them.
     *
     * `progStartsCand` MAY EXCEED `progQueuedCand`. A fork copies the whole queue, so one ask can stand
     * unstarted in N timelines and be started once by each — the same program on N paths, which is what a fork
     * is. There is deliberately no assertion between the two sides; the one that holds is the partition, over
     * a single event, and it is asserted at engine_frontier_census like the two above it. */
    long prog_starts;       /* programs STARTED — `prog_starts_cand + prog_starts_other`, asserted */
    long prog_starts_cand;  /* …the @S candidate programs among them: a constructed PoC that got its chance */
    long prog_starts_other; /* …and every other kind: the document's own coverage */
    long prog_queued_cand;  /* candidate programs the search ASKED to have run — the denominator of the above */
    long claims_met;        /* an inherited orphan drive whose body a take handed over */
    long claims_unmet;      /* …and one that FINISHED never having been handed one — the round trip's verdict */
    long host_asked;         /* rendezvous ids this instance MINTED — every one the host is shown and must pay */
    long host_answered;      /* …and the ONE delivery that SETTLED each: `answered <= asked` is asserted */
    long host_answers_extra; /* answers landing on an ALREADY-settled request — one per extra peer TIMELINE,
                              * each of which forks an arm. Not a payment: it unblocks nothing, and adding it
                              * into `host_answered` is what made one ask read as four. */
    long host_answers_late;  /* answers refused because the session had already closed */
    /* …AND ASKS THIS INSTANCE WITHDREW (engine_host_terminate), WHICH IS THE ONE POPULATION THE PAYMENT RATE
     * CANNOT SEE. `asked` counts mints and `answered` counts settlements, so a terminated id is an ask that
     * will never be paid and a widening gap is the rate's own signature for "the host is not paying" — the
     * diagnosis that pair exists to make in a glance, read backwards. A terminate is the engine RETRACTING the
     * question, so it belongs beside the two rather than inside either: `hostAsked - hostAnswered -
     * hostTerminated` is what is genuinely outstanding. */
    long host_terminated;    /* rendezvous ids WITHDRAWN — Fetch §2 Infrastructure's terminate-a-fetch-controller */
    long paged_reqs;         /* synchronous requests a sale took with it */
    /* …AND WHETHER THE ALLOCATOR'S REFUSAL EDGE WAS ASKED AT ALL, WHICH `sold` CANNOT STATE. A zero `sold` is
     * three runs at once — the frontier fitted and nothing refused, a refusal arrived where the safepoint is
     * not armed, or the pager was asked at the floor and held nothing but the running flow — and they take
     * opposite work. This is `host_asked`'s service to `host_answered` performed for the pager, and the four
     * rows are an exact partition (`unarmed + floor + sold == asks`), asserted at engine_frontier_census. */
    long paged_asks;         /* times the allocator's refusal edge reached this engine (engine_reclaim_tail) */
    long paged_unarmed;      /* …declined because the reclaim safepoint was not armed (outside the flow step) */
    long paged_floor;        /* …answered at the frontier's floor: no member but the flow that is running */
    /* ─── AND WHETHER A REPLY EVER BECAME A PROGRAM, PER DOOR, WITH THE DENOMINATOR EACH ONE IS A SHARE OF ──
     *
     * WHAT THE RUN COULD NOT SAY BEFORE. Two components turn a fetched reply into a program — solver/engine.c's
     * FLOW_PENDING_RESOLVE delivery and core/xhr/xml_http_request.c's `xhr_take_reply` — and both end in
     * `engine_queue_fetched_script`, which queues a DYN_PAGE_SCRIPT. The kind of the row is therefore the same
     * kind the document's own seeded `<script>` rows carry, so `progStartsOther` sums a chunk that arrived
     * over the network with the page's own bundle and NO ROW ANYWHERE SAID A PROGRAM HAD BEEN QUEUED FROM A
     * REPLY AT ALL. CLAUDE.md §Learning-from-replies makes "a fetch whose body is JAVASCRIPT is ALWAYS fetched
     * + EXECUTED" the headline moat surface, and the two doors that build it had no witness of their own.
     *
     * THE ASK IS RECORDED AT THE CALL AND NOT AT THE OUTCOME (CLAUDE.md §AN-INVARIANT-OVER-A-GATED-OPERATION).
     * Both doors LEGITIMATELY decline: a reply whose computed type is not JavaScript is not a program, and a
     * preload, a modulepreload and an image decode park a kind of their own PRECISELY so a JavaScript-typed
     * reply is not compiled (see engine_pending_resource_url above). A census of what LANDED cannot tell a
     * door that was never reached from one that correctly refused every reply it was shown, so each `…_asks`
     * row is raised where the door HOLDS A REPLY RECORD, upstream of the type gate and of the address guard,
     * and each `…_queued` row beside the queue call. `0/0` is a door the run never reached — for the fetch
     * door that is a page that issued no `fetch()`, for the XHR door a page that sent no XMLHttpRequest;
     * `0/N` is a door reached N times that queued nothing, which is either N correct refusals or the arm
     * failing and is a question about the replies rather than about whether the door exists.
     *
     * THE TWO DENOMINATORS COUNT ONE POPULATION, WHICH IS THE PART THAT HAD TO BE MADE TRUE RATHER THAN
     * ASSUMED (CLAUDE.md §AND-THE-DENOMINATOR-CAN-BE-THE-RIGHT-KIND). `xhr_take_reply` returns before its
     * program block for a network error — a reply with no body — so the fetch door's raise is guarded on the
     * reply being a RECORD for exactly that reason and not for a defensive one. Both rows are therefore
     * "reply records this door examined for a program", and neither counts a network error.
     *
     * `net_prog_queued` IS RAISED AT THE ONE ENTRY AND IS NOT THE SUM OF THE TWO ARMS. It is written inside
     * `engine_queue_fetched_script`, so every caller moves it, and the two door arms are written by the two
     * doors. The relation is `fetch_queued + xhr_queued <= net_prog_queued` and it is asserted; it is an
     * INEQUALITY rather than a partition because a third caller exists and is deliberate — `test_forced.c`'s
     * `loadScript` host edge stands in for a `<script src>`-shaped door and says at its own site that it is
     * the door which CANNOT exercise the delivery arm. So in the shipped program the residue is ZERO and a
     * reader can check that from the rows; in that fixture it is the fixture's own edge. WHAT THE ASSERT
     * CATCHES is the hazard CLAUDE.md §A-superseded-system-is-DELETED names: a door raising a queued arm
     * WITHOUT going through the one compile entry, which is a second compile door wearing an observation.
     *
     * A REPORT AND NEVER A BOUND (§NO BOUNDS): nothing reads them and no arm branches on one. */
    long net_prog_queued;       /* programs queued at engine_queue_fetched_script — EVERY caller */
    long net_prog_fetch_asks;   /* reply records the `fetch()` reply door examined for a program */
    long net_prog_fetch_queued; /* …and how many of them it queued: `queued <= asks` is asserted */
    long net_prog_xhr_asks;     /* reply records the XMLHttpRequest reply door examined for a program */
    long net_prog_xhr_queued;   /* …and how many of them it queued: `queued <= asks` is asserted */
} EngineFrontierCensus;
void engine_frontier_census(EngineFrontierCensus *out);

/* HOW MANY PROGRAM ROWS OF THE LIVE FRONTIER ARE STANDING ON AN ADDRESS WHOSE BYTES HAVE NOT ARRIVED — the
 * GAUGE that `rootProgramsAwaitedAtSeed` is the CONSTANT half of, and the reason it is a free function rather
 * than a field of the record above. That record is documented at its emitter (solver/result.c) as LIFETIME
 * COUNTS, and the grouping there is MECHANICAL — a row inherits its kind from the accessor that filled it —
 * so a gauge inside it would make that contract wrong about a row for the first time. `flow_host_owed_count`
 * is the existing member of this shape and this stands beside it on the census line for its reason.
 * WHAT THE PAIR SEPARATES, which is the whole of why it exists: `…AwaitedAtSeed` says what the document OWED
 * the reply door when its rows were laid down and cannot say whether those bytes ever came, so `17` beside a
 * `rowsAwaitingBytes` of 0 is a bundle that arrived WHOLE — a run that never reached its later programs is
 * then the ORDER failing — and `17` beside `17` is a bundle whose bytes never arrived, which is the fetch
 * path. Those take opposite work and no row on that line separated them.
 * IT IS SUMMED PER MEMBER AND THE FAN-OUT IS THE ANSWER. A fork copies its parent's rows, so one document row
 * awaited by N members counts N times: each of them stops at that position until its own delivery pays it. No
 * inequality against `…AwaitedAtSeed` holds in either direction — forking drives it above, a sale drives it
 * below — which is why there is no assert between the two. */
long engine_rows_awaiting_bytes(void);

/* HAS ANY PROGRAM STARTED IN THIS INSTANCE AT ALL — one bit, monotone, read by the @H surface to say which of
 * its records were minted before the page's own code had run a line.
 * WHY THE @H SURFACE NEEDS IT AND CANNOT COMPOSE IT. An endpoint record carries a PROVENANCE, and that field
 * answers what a request is EVIDENCE OF — whether a real load makes it, whether a forced arm is under it —
 * which is the firing policy's question and is the right question for the firing policy. It is not the
 * REPORTING question, and the two came apart on every real page measured: `engine_prov_of_running_path`
 * states in its own declaration above that it can never answer `observed`, so every subresource a PARSER
 * inserted and a browser algorithm recorded is graded `derived` — and `derived`'s own definition, the code
 * COMPUTED this address from real inputs, is the product's headline claim. A document whose whole surface is
 * its own markup therefore reports a surface of `derived` rows, and a reader counting them counts addresses
 * forced execution never composed. §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS is the shape exactly: one field,
 * two questions, decided by the stricter one, with the cost landing silently on the other. The cure that rule
 * prescribes is TWO predicates over ONE fact rather than a second field free to disagree, and the fact this
 * one is asked of is `g_prog_starts` — written at the single line a program starts, beside the two arms that
 * partition it.
 * IT IS A COUNT CROSSING ZERO AND NOT A HIGH-WATER MARK, DELIBERATELY. `g_deepest` would answer the same
 * question today and its own declaration is a paragraph about how it has been misread — it SATURATES, reads
 * flat, and was dispatched as a ceiling into three briefs. A reader who follows this call reaches a monotone
 * count whose zero means one thing.
 * A REPORT AND NEVER A BOUND (§NO BOUNDS): nothing branches on it, no request is refused because of it, and
 * the surface it feeds is a census row. */
int engine_any_program_started(void);

/* THE FOUR NOTES THE TWO REPLY DOORS WRITE — see EngineFrontierCensus's `net_prog_*` block for what they are
 * FOR; this states why they are four entries and not one with a door argument.
 *
 * ONE ENTRY PER DOOR PER SIDE, WHICH IS THE SHAPE core/xhr AND core/fetch ALREADY REACH THIS HOST THROUGH
 * (solver/endpoint.h's `endpoint_xhr_edge_began`/`…_placed`/`…_offered`). A single entry taking a door and a
 * did-it-queue flag would be CLAUDE.md §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS written as an argument list:
 * one call answering which door and whether the gate passed, decided at one site, with a caller free to state
 * the first and forget the second. Four named statements cannot be half-made.
 *
 * AND THE DOOR IS NOT A PARAMETER OF `engine_queue_fetched_script`, WHICH IS THE SHAPE A READER REACHES FOR
 * FIRST. That entry's own declaration says its task source and its script type are stated AT THE DEFINITION
 * rather than taken as parameters because "this entry is one spec step, and a caller that reached it has a
 * response in hand" — which transport carried those bytes is not a fact of that step, and a census-only
 * argument on it would be the first parameter it holds that the row it builds does not carry. The cost of
 * that choice is stated rather than hidden: the partition over the doors is an INEQUALITY against the entry's
 * own total rather than an equality, and the residue is a published row.
 *
 * THE ASK IS RAISED WHERE THE DOOR HOLDS A REPLY RECORD AND BEFORE ANY TYPE IS READ, which is the whole of
 * what makes the pair readable — a raise after the type gate would count only the replies that passed it and
 * the denominator would be the numerator. A caller that raises `…_queued` without having raised `…_asks` on
 * the same reply is caught at engine_frontier_census, where both are in one hand.
 *
 * THEY ARE VOID AND READ NOTHING BACK (§NO BOUNDS): a door's behaviour does not depend on having been counted. */
void engine_note_net_prog_fetch_ask(void);
void engine_note_net_prog_fetch_queued(void);
void engine_note_net_prog_xhr_ask(void);
void engine_note_net_prog_xhr_queued(void);

/* THE ALLOCATOR UNDER THE JS HEAP, which is the one number quickjs's own accounting structurally cannot give.
 * `JS_ComputeMemoryUsage` walks the RUNTIME; Lexbor's document arenas, the per-flow COW deltas and every other
 * `malloc` in this host are invisible to it, so a run whose RSS is sixteen times its JS heap has nothing in
 * that census to say what the other fifteen sixteenths are. `live` is what the C allocator currently has handed
 * out (quickjs's bytes INCLUDED, since js_malloc routes to malloc), and `arena` is the address space it has
 * ever needed. IN WASM THE TWO DIFFER PERMANENTLY AND THAT DIFFERENCE IS THE DIAGNOSIS: linear memory only
 * grows, so a page handed back stays mapped and `arena` is a HIGH-WATER MARK that RSS follows. A run whose
 * `live` is flat while `arena` climbs is FRAGMENTING and not leaking, and the two have different fixes. */
size_t engine_c_alloc_live(void);
size_t engine_c_alloc_arena(void);

/* WHAT THIS INSTANCE'S TIMELINES DID WITH THE ROUTED RECORDS HANDED TO IT — how many they DELIVERED (each one
   became §9.3.3 step 8's one global task at the receiving Window) and how many they CONSUMED as not theirs (a
   message belonging to the other side of a sender branch this timeline has taken a side at).
   BOTH OR NEITHER, because either alone is uninterpretable and the pair is what makes a delivery count mean
   anything at all. A routed record is attached to EVERY live flow of the receiving document (engine_route says
   why: a document's state IS its flows, and a delivery seeded from the baseline arrives at a document where
   the page's own listener was never registered), so the number of times a page's `message` handler runs is the
   number of TIMELINES that admitted the record — never the number of records the zone routed. A host that
   compares its own routed count against the handler's invocations is asserting that the receiver has exactly
   one timeline, which is true only of a receiver whose other timelines the scheduler never reached: it passes
   while they are starved and fails the moment they run, which is the schedule-dependent answer §Testing's
   differential exists to catch. These two numbers are what such a host compares against instead — `delivered`
   is exactly how many TASKS the engine queued, and `refused` is what says the rest of the frontier saw the
   record and correctly declined it rather than never having been offered it.
   `delivered` IS NOT A HANDLER-INVOCATION COUNT AND THIS LINE USED TO SAY IT WAS. A queued task has FOUR ends
   and only one of them runs a listener — see engine_routed_task_census below, which is the half that was
   missing and which a host had no choice but to guess at.
   AND NEITHER OF THEM IS ABOUT A RECORD, WHICH IS THE THIRD NUMBER AND THE REASON THE PAIR BECAME A TRIPLE.
   Both counts above are per (record, TIMELINE) attachment; "was this record admitted by any timeline at all"
   is per RECORD, so no arithmetic over the pair recovers it — one record admitted by N timelines pays for N-1
   records admitted by none, and a shortfall of `delivered` below a host's own routed count therefore names a
   loss when it fires and establishes nothing when it does not. `zero_delivery` is that question answered where
   the fact lives: how many routed records NO timeline of this instance has admitted.
   IT IS A GAUGE AND THE OTHER TWO ARE LIFETIME COUNTS, so it may FALL and they may not — a record still in
   flight is indistinguishable here from a record that is lost, and only a receiver DRAINED to a stall makes
   this number a loss rather than a backlog. A host that reads it says that it drained; a host that differences
   two samples of it is doing arithmetic on nothing. The raise site states how it is keyed and what the one
   named residual is.
   ALL THREE OR NONE, for engine_routed_task_census's reason exactly: a delivery count with no refusal count
   cannot tell "the frontier declined this" from "the frontier never saw it", and neither of them with no
   zero-delivery count beside it can tell either of those from "no timeline of this document ever received it",
   which is the one a page cannot distinguish from a message that was never sent. */
void engine_routed_census(long *delivered, long *refused, long *zero_delivery);

/* THE ORPHAN SURFACE'S CENSUS — how many drives of a function the page shipped and never called this session
 * SEEDED, and how many times a flow got as far as asking for one.
 *
 * `driven` COUNTS SEEDS AND NOT RUNS, WHICH IS WHERE ITS FIRST READER WENT WRONG. The count is raised the
 * instant a take succeeds, immediately before engine_sibling_assemble puts the drive on the frontier — so it
 * says a flow was CREATED for that body, never that the flow was picked, ran, or reached the call. Whether it
 * ran is answered by the drive's own FINDING (the endpoint it records), not by this number, and a reader who
 * takes `driven > 0` for "the uncalled code executed" is reading a seed as a result.
 *
 * IT IS TWO NUMBERS FOR THE REASON engine_routed_census IS, AND THE PAIR SEPARATES TWO STATES OF THREE — the
 * third needs the finding beside it, and saying so here is the whole of what stops the pair being over-read.
 * On a FRESH session (no residue, so the routing arm that consumes a take without seeding cannot fire):
 *     asked == 0                      no flow ever ran out of its own work, so the question was never
 *                                     reached — a scheduling result, and the one worth acting on. WHAT THAT
 *                                     CONDITION IS, NAMED so the row is checkable: flow_step asks the seed
 *                                     BELOW the three clock-driven sources, so "ran out of its own work" is
 *                                     "has no program, job, delivery, checkpoint or lifecycle stage due, AND
 *                                     no rendering opportunity, no due timer and no idle work" — and NOT "has
 *                                     no frame, timer or reply left", which is the exit that declares a
 *                                     timeline OVER.
 *                                     THIS SAID "THE LAST MOMENT BEFORE THE CLOCK MAY MOVE", AND THAT IS THE
 *                                     HALF THE OWNER'S ORDERING RETIRED. The reason it gave is intact and is
 *                                     why the clock now runs first: a rendering opportunity is generated for
 *                                     ever on a document that has one, so a row asked BELOW it is a fact
 *                                     about the DOCUMENT'S shape rather than about the frontier — which is
 *                                     exactly as true of this row now as it was of the exit then, and is the
 *                                     PRICE of serving the page's own arranged work first rather than an
 *                                     oversight. A zero here is therefore no longer "no flow ran out of its
 *                                     own work": it is that, OR every flow that did had a frame or a timer
 *                                     due, and solver/engine.c's clock-arrival counters are what separate
 *                                     them. A run in which it reads 0 while `live`
 *                                     climbs is now a statement about the five conditions above the rung —
 *                                     frame, sequence, job, block, lifecycle — PLUS the three clock rungs,
 *                                     and, AHEAD OF ALL EIGHT, about
 *                                     whether the members were DISPATCHED at all: every one of those is asked
 *                                     inside flow_step, so a member the pick never reaches asks nothing and
 *                                     appears in none of them. Read solver/cold.h's `stepUnits` `none` row
 *                                     (solver/step_unit.h's NONE, paired with @WFQ's `unrun`) FIRST, then
 *                                     `framed`, `outOfPrograms`
 *                                     and `blocked` for which arm holds the rest. Reasoning over the eight
 *                                     without asking the zeroth is how this row gets read as a ladder defect
 *                                     when it is a pick-order one.
 *     asked > 0, driven == 0          the walk ran and the heap held no uncalled function — a fact about the
 *                                     PAGE. It is NOT evidence about pick order, and reading it as such is
 *                                     reading "there was nothing to drive" as "something was starved".
 *     driven > 0, finding ABSENT      the drive was seeded and did not get far enough to record what it
 *                                     would have — THIS is the pick-order reading, and it needs the finding.
 * On a RESUMED session a take can ROUTE to a flow already waiting for that body without raising `driven`, so
 * the middle row is ambiguous there and the pair must be read on a fresh one.
 *
 * AND THERE IS A FOURTH STATE THIS PAIR CANNOT REACH, WHICH IS SAID HERE BECAUSE THE OBVIOUS WAYS TO REACH IT
 * ARE ALL WRONG. The third row above — seeded, finding absent — is one word for two different defects, and
 * they take opposite fixes: a drive NEVER GIVEN THE THREAD is a pick-order problem (a weight), and one PICKED
 * AND CUT SHORT before it reached its call is a dwell or preemption-granularity problem. Separating them wants
 * "was this seeded drive ever switched in", and every field that looks like it answers that is INHERITED BY
 * FORKS and therefore describes the drive's whole FAMILY rather than the seeded root: `orphan` is copied at
 * the fork (engine_sibling_assemble), `fn` is passed to the child, and `visits` is a WFQ term that §scheduler
 * REQUIRES a fork to carry, since a term a fork does not carry is a way for a flow to change its own rank by
 * branching. So a "was it picked" bit hung on `orphan` flows counts descendants, and a frontier walk over them
 * counts a family that grew. What the fourth state needs is a marker the seed sets and a fork does NOT copy,
 * raised once at the scheduler's switch-in, with `picked <= driven` asserted at this accessor — and it needs
 * the park's half too, or a resumed drive re-counts. That is a real diff on the hottest struct and the pick
 * path, and its failure mode is a WRONG NUMBER rather than a crash, which is the one outcome this census
 * exists to prevent. Until it is built, `driven > 0` with the finding absent says DISPLACEMENT and does not
 * say which kind.
 *
 * `driven` ALREADY EXISTED AND WAS UNREADABLE. It reached the heap/progress line and nothing else, and
 * §Testing says the renderer deliberately does not tee its stdout, so the number that says whether the
 * headline surface of this tool did anything at all could not be read off a run. Both cross in the result
 * document now, beside the @S arrival census they are the orphan-side twin of. */
void engine_orphan_census(long *driven, long *asked);
/* …AND WHICH EXIT EACH OF THOSE ASKS TOOK, WHICH IS A PARTITION THE ENGINE ALREADY ASSERTS AND HAS NEVER
 * PUBLISHED. `asked` above is raised at engine_orphan_seed's entry past the forking gate and exactly one of
 * these three is raised at each of its exits; engine_step_unit_runs asserts the equality. So the residue a
 * reader needs has existed, correct and checked, in three statics nothing emits — the write-with-no-reader
 * defect on the partition that decides which of two opposite repairs the orphan surface owes.
 * WHAT IT SEPARATES, AND THE TWO READINGS TAKE OPPOSITE WORK, which is the whole reason it is three rows and
 * not one. `took` is a walk that handed a body over. `empty` is a walk that ran and found nothing — a fact
 * about the HEAP, which engine_orphan_seed's residual states is NOT the same finding as "the bundle ships no
 * uncalled code", because the walk can only see a body with a LIVE FUNCTION OBJECT OF ITS OWN. And `memo` is
 * an ask the generation cache answered WITHOUT WALKING AT ALL — so `memo` high says the cache absorbs and the
 * walks that do happen are few, while `memo` low says the orphan generation moves as fast as flows run out of
 * work and essentially every ask is a full enumeration of `rt->gc_obj_list`. The first makes the cost PER WALK
 * and the repair is inside the walk; the second makes it PER ASK and the repair is the cache or the rung's
 * placement. No count of asks, drives or step arms can tell those apart.
 * MEASURED, WHICH IS WHY THIS IS A ROW: over three drives of one release artifact on one real app,
 * `seed-one-orphan-flow` overran the cooperative slice in 36 of 55, 100 of 121 and 122 of 140 of its own runs
 * — 51%, 72% and 76% of ALL overrunning turns in the run — while `deliver-one-reply` overran 2.9%, 3.9% and
 * 3.0% of its own and `resume-program` 0%, 5% and 2.4%. One arm carries three quarters of the overruns, its
 * walk is an enumeration of the whole GC object list with no step boundary in it, and nothing published says
 * how often that walk is actually performed.
 * ONE STRUCT AND ONE CALL, for engine_rival_miss's reason exactly: a partition read through three accessors is
 * three moments, and §Testing's rule is that a conservation identity holds WITHIN ONE SAMPLE and nowhere else.
 * PER SESSION, not per instance — solver/engine.c releases all four with the agent, which is why the equality
 * is an EQUALITY and not a floor, and which a reader comparing them against a per-instance row must know.
 * THEY DECIDE NOTHING AND BOUND NOTHING (§NO BOUNDS): no arm of any verdict branches on them, and "how often
 * did the orphan walk find nothing" is precisely what a stop-looking heuristic would be built from.
 * HOW THEIR ABSENCE SHOWS, as an observation and not an instance: a reader holding a large `orphansAsked`, a
 * small `orphansDriven` and an orphan arm carrying most of the slice overruns states whether the cost is the
 * walk or the asking — with no row in the artifact that could contradict them either way.
 * RETIREMENT: these go when the walk no longer enumerates the heap — when a body's orphan state is reachable
 * without a pass over `rt->gc_obj_list` — because `memo` then prices a cache over a cheap question and the
 * partition has nothing left to decide between. */
typedef struct {
    long memo;    /* the generation cache answered and NO walk was performed */
    long empty;   /* the walk ran and the heap held no takeable body */
    long took;    /* the walk handed a body over */
} EngineOrphanExits;
EngineOrphanExits engine_orphan_exits(void);
/* …AND HOW MANY OF THOSE DRIVES CAME FROM THE WALK'S PREFERRED PASS — see the definition for why the pair is a
   row rather than an inference, and quickjs.h's JS_OrphanPreferredTakes for what the preference is. */
long engine_orphan_preferred(void);
/* …AND OVER HOW MANY DISTINCT SCRIPTS THE DRIVES WERE SPREAD — see the definition for why this and the
   preference count are two independent facts about one walk, and quickjs.h's JS_OrphanScriptsDrawn for why a
   drive count alone cannot tell a spread run from a monopolised one. */
long engine_orphan_scripts(void);

/* ---- THE FOUR ENDS OF §9.3.3 STEP 8'S TASK, AND WHY ONE NUMBER COULD NOT SAY WHICH ---------------------
 *
 * `engine_routed_census`'s `delivered` counts tasks QUEUED. Nothing counted what became of them, so a host
 * looking at a page that ran its `message` listener fewer times than the engine delivered had exactly one
 * number for THREE different facts, each taking a different action: the task ran and the page saw the message;
 * the task ran and HTML §9.3.3 "Posting messages" step 8.1 declined it (the target's origin is not the one the
 * sender asked for); the task ran, or was taken off the queue before it could, and there was no Document left
 * to fire at (HTML §7.5.10 "Destroying documents" step 5 — reachable both ways, because engine.c's flow_deliver
 * enqueues a ROUTED delivery in the RECEIVING document's realm, which is exactly the realm step 5's removal
 * walk keys on); or the task never ran at all, which is a work item the ONE frontier dropped and is the only
 * one of the four that is a defect.
 * That is §@S's rule about a search that cannot be directed at a gap it reports with the same number as two
 * other gaps, one layer down and about deliveries instead of candidates — and it is what a driver measured
 * instead by counting the receiving page's own fetches, which cannot work: engine_pending_fetches dedups over
 * the (method, URL) pair, and N timelines of one document run the SAME listener and therefore issue byte-
 * identical requests, so the host's view of the fetch register collapses them by construction.
 * SUM ≥ `delivered`, NEVER `==`, and the inequality is not slack: a fork gives the arm its own Array naming
 * the parent's job RECORDS (flow.c's flow_job_fork), so a timeline that branches between the enqueue and the
 * run delivers the message once in each arm — two timelines, two deliveries, one queued task. A sum BELOW
 * `delivered` is the defect, and it is a task that was queued and never ran. */
enum {
    ROUTED_TASK_FIRED = 0,        /* §9.3.3 step 8.7: the event was fired at the target Window */
    ROUTED_TASK_TARGET_ORIGIN,    /* §9.3.3 step 8.1: the target is not same origin with the requested origin */
    ROUTED_TASK_TARGET_GONE,      /* §7.5.10 step 5: the target's Document was destroyed */
    ROUTED_TASK_THREW,            /* the task itself went abrupt before it could fire anything */
    ROUTED_TASK_END_N
};
/* REPORTED AT THE LINE THAT IS THAT END, and TARGET_GONE has two such lines because §7.5.10 step 5 is reachable
 * at two moments and is ONE fact either way. core/frame/window_message.c reports it when the task RUNS and
 * finds the navigable destroyed; solver/flow.c's flow_job_drop_realm reports it when step 5's own removal walk
 * takes the still-queued task off a destroyed document's queue ("without running those tasks"), which is the
 * path that used to leave no trace at all — and a delivery that vanished there is indistinguishable from one
 * the scheduler lost, which is the whole distinction this census exists to make.
 * ONCE PER TASK on the running side: the task records which end it reached, so a machine that is re-entered
 * cannot count its delivery twice. */
void engine_routed_task_end(int end);
/* ALL FOUR OR NONE, for engine_routed_census's reason exactly — a fired count with no declined count beside it
   cannot say whether the rest of the deliveries were refused by the spec or lost by the scheduler. */
void engine_routed_task_census(long *ends);

/* THE ORPHAN ROUND TRIP'S TWO NUMBERS — how many waits for a parked drive's function a TAKE satisfied this
   session, and how many waiting drives FINISHED never having been handed one. The third, how many were rebuilt,
   belongs to the cold tier and is asked of it (ColdResumed's `orphans`).
   THE SECOND IS THE VERDICT. A recipe for a driven orphan carries a cross-session NAME for the function, and a
   name that round-trips as text while naming nothing produces a frontier of drives that call nothing — which
   emits no findings, crashes nowhere, and is indistinguishable from a document with no uncalled code in it.
   Zero unmet on a document whose bytes did not change is the whole of the claim this feature makes.
   `met` MAY EXCEED THE RECORDS and that is not a fault: a waiting drive forks arms while it replays the
   document, and every arm of it is the same drive of the same body. */
void engine_orphan_claims(long *met, long *unmet);

/* WHO COUNTS THE DOM'S WRAPPERS. The scheduler's diagnostic line reports the identity map's size, and that map
   is the DOM's — so the DOM registers the counter rather than the solver naming node.h and dragging lexbor in
   behind it. */
void engine_set_wrap_stats(void (*fn)(long *n, long *cap));

/* THE SOLVER'S AGENT-LIFETIME STATE, RELEASED IN ONE CALL — core/platform.h's release column, for this half.
 *
 * The browser half's teardown is a LIST every host goes through so that a host cannot express an omission. The
 * solver half had no such call and its teardown was six lines written by hand into three hosts, which had
 * drifted exactly the way that list drifted before it had one: `solve_free` and `endpoint_free` were in main.c
 * and test_forced.c and in NEITHER of the WPT runner's, and `attr_shadow_free` was in test_forced.c alone, so the two hosts that lack it leak §@S's (element, slot) -> opaque map — every
 * entry a dup'd JSValue — whenever a flow stores a source in a DOM string slot.
 *
 * AND THIS CLASS CANNOT BE FOUND BY A DETECTOR, which is why the answer is a column and not a better walk. The
 * three emission tables are plain `malloc` holding no JSValue and no atom: the runtime's gc_obj_list walk
 * cannot see them (not GC objects), the atom walk cannot see them (not atoms), and even JS_DUMP_LEAKS's
 * `malloc_count` cannot (it counts `js_malloc_rt`, not `malloc`). Nothing quickjs has will ever report one.
 * The taint shadow is the mirror case: its entries ARE GC objects, so the gc_obj_list DCHECK would name them —
 * but only on a run where a flow actually stored a source in an attribute, which is the product entry under
 * real solver input, and no gate runs that entry. Both halves of the divergence were therefore invisible for
 * the same structural reason and not by luck.
 *
 * ORDER IS REVERSE DEPENDENCY, like the column it mirrors. The frontier goes first because everything under it
 * is reached through it (flow_registry_free already cascades the world registry, the decision chain, the path
 * constraint's pins, the cold tier and the pending register — it is this column in miniature and says so at
 * each line); the taint shadow next, because a shadow exists only because some flow wrote one; the emission
 * tables last, since they are read out of the result document long before any teardown runs.
 *
 * AND THE FRONTIER IS NOT ON THIS CALL, BECAUSE IT DOES NOT BELONG ON THIS SIDE OF THE BROWSER'S OWN COLUMN —
 * see solver_frontier_free below. */
void solver_agent_free(JSContext *ctx);

/* THE FRONTIER, RELEASED WHILE THE BROWSER IS STILL STANDING — the FIRST thing a host's teardown does, before
 * core/platform.h's release column and therefore before this half's own.
 *
 * A SUSPENDED FLOW IS A LIVE ACTIVATION OF THE BROWSER, and that one sentence is the whole of the ordering. A
 * flow's snapshot is its COW delta plus its suspended heap-frame chain, and that chain holds the STEP MACHINES
 * of every continuation-holding builtin and every browser algorithm it is stopped inside — a §2.9 dispatch, a
 * custom-element reaction, an IntersectionObserver delivery, HTML §8.1.4.6 Runtime script errors' report. Tearing one down runs each machine's
 * `fini`, which is that COMPONENT's code reading that component's agent state. So releasing the browser half
 * first is releasing a component while activations of it are still live, and the flows are then torn down
 * against a platform that has already been given back.
 *
 * IT WAS MEASURED AS ONE ABORT AND IT IS A CLASS. §8.1.4.6 step 6.1 sets the global's in error reporting mode
 * (HTML §8.1.3.3 Realms, settings objects, and global objects gives the flag to the GLOBAL, so the engine keeps it on the global under a
 * private Symbol the AGENT owns); a flow parked inside the `error` event's own dispatch owes that flag back,
 * and its `fini` is what gives it. With the platform released first, the Symbol is gone by then and the give-
 * back asks for a key that no longer exists — an abort whose message named the OTHER state that leaves that
 * component undeclared, "before report_exception_init ran", because `!ready` had two causes and one sentence.
 * Every other component whose step machine's `fini` touches agent state is the same defect with no assert
 * sharp enough to have said so.
 *
 * WHAT STAYS ON THE OTHER SIDE, AND WHY THE TWO CALLS ARE NOT ONE. The browser half CLAIMS slots in this half —
 * §8.1.7's timer step, §8.1.7.3's in-parallel half, §13.2.7's document-load step, the wrapper census, the
 * source registry's per-source encode sets — and a claimant releases at its own release, which is that column.
 * So this half's own state must go AFTER the platform and the frontier must go BEFORE it: the browser's column
 * sits between them, and neither end of this half can be moved to join the other. Each end asserts the other
 * ran (solver_agent_free reads a latch this sets; core/platform.c asks the runtime's own step-machine census),
 * so a host that collapses them back into one call aborts at the teardown naming which line to move. */
void solver_frontier_free(JSContext *ctx);

#endif
