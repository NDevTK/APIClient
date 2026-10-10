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

/* The witness mark (`PENDING_PINNED_*`) for a request built by running the page's code, read off the running
 * flow; core/xhr/xml_http_request.c's §3.5.6 "The send() method" request is the caller it was written for. It
 * reads the flow itself, as engine_prov_of_running_path does, so a caller cannot pass a different one.
 *   With no running flow the answer is `unpinned`, although `pinned` is the safe word elsewhere: `path_pinned`
 * is nested inside `path_forced`, engine_prov_of_running_path answers `derived` on the same state, and
 * extension/lib/safe-fetch.js's `_firingRefusal` CHECKs against `pinned` beside `derived`, fatally in release.
 * A flow-less call is a broken invariant at every caller (DCHECKed here), and `unpinned` alone fires nothing.
 * The zone's third word, `unstated`, is what a reader composes for a record with no mark; this never spells
 * it. */
int engine_pinned_of_running_path(void);

/* The witness mark's wire spelling, shared by every writer of the field so the two words are one vocabulary
 * for the firing decision that reads them. An unknown value is fatal (CHECK), as for engine_provenance_token. */
const char *engine_pinned_token(int pinned);

/* What the bytes are for: Fetch §2.2.5 "Requests"' destination, stated verbatim off the request record the park
 * carried (core/fetch/fetch.h), never derived here. It is a different fact from the initiator: an injected
 * `<script src>`, a dynamic `import()` and a `fetch()` all have initiator `script`, and only the first two
 * are code loads. The vocabulary is §2.2.5's whole destination enumeration, and the empty string is a value
 * (data, as for `fetch()` and XMLHttpRequest). The consumer reads §2.2.5's script-like predicate
 * ("audioworklet", "paintworklet", "script", "serviceworker", "sharedworker", "worker"), which is the CORB
 * class. The join asserts every value is a destination type, so a new one crashes at the producer rather than
 * reaching the zone as data.
 *   Only `script` is declared, because it is the one the solver's own script parks emit (§8.1.4.2 "Fetching
 * scripts" creates classic and module requests with it); every other destination is the literal written by
 * the component whose algorithm names it (`image` at HTML §4.8.4.3.5, `document` at a navigation). */
#define PENDING_DESTINATION_SCRIPT "script"
/* The destination enumeration and the script-like predicate are statics of solver/engine.c, since every use is
   there: the join refuses to write a value §2.2.5 does not define, the split refuses to believe one, and the
   join's fold states a deduped set's strictest destination, because one reply satisfies every park in it. The
   fold decides what the line says, never what is fetched; the CORB decision is the trusted zone's, which asks
   the same predicate in `extension/lib/safe-fetch.js` over the bytes it read. */

/* What the host still owes the frontier's network parks: one
 * `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` line per outstanding
 * request, newline-terminated, "" for none, deduped by (method, url). engine_pending_split is the authority on
 * the shape. The buffer is this function's and is valid until the next call.
 *   The method is part of the request's identity, so a GET and a POST to one address never collect each
 * other's bodies. The destination (Fetch §2.2.5) is on every line so the chokepoint can apply CORB to every
 * park. The credentials mode is §2.2.5's three-valued token, spelled by core/fetch/fetch.h's
 * `fetch_credentials_token`: only the algorithm that created the request knows it, and `same-origin` is a
 * condition the trusted zone resolves (extension/lib/safe-fetch.js), never the engine.
 *   No field can hold a TAB: URL Standard §4.4 URL parsing strips tabs, a method matches RFC 9110 §5.6.2 Tokens
 * (Fetch §2.2.1 Methods), and every destination is lowercase ASCII. The join asserts all three. */
const char *engine_pending_fetches(void);
/* Split one engine_pending_fetches line where it was joined, so hosts do not each re-derive the grammar. `line`
   is the host's own mutable copy of one line without its newline; each TAB becomes a NUL and the seven fields
   are returned pointing into it. Every out-parameter is required (asserted): a host that skipped the
   destination would fetch a script as data, one that skipped the provenance would carry a forced reply as an
   observation, and one that skipped the credentials mode would decide by silence whose session pays. A
   missing TAB is fatal (CHECK). */
void engine_pending_split(char *line, const char **method, const char **destination,
                          const char **initiator, const char **provenance, const char **pinned,
                          const char **credentials, const char **url);
/* Deliver a body for one request, keyed on the `(method, url)` the flow parked on. Returns how many entries it
   filled; 0 means the host's pairing is off or the record was sold (engine_take_paged_owed), which the caller
   tells apart because it owns the credit. */
int engine_provide(JSContext *ctx, const char *method, const char *url, JSValueConst value);

/* Refuse one request, keyed on the same `(method, url)` pair; returns how many records it newly refused (0 is a
 * pairing error or a sale, which this tells apart itself). It is not a network error: a refusal a real browser
 * also makes (a blocked scheme, Fetch §4.3 "Scheme fetch"; a §4.10 "CORS check" failure; a CORB-blocked body)
 * arrives through engine_provide as Fetch §5.6 "Fetch methods"' network error. This is for a refusal only this
 * tool makes, where a network error would be an observation that does not exist and would spend the page's
 * failure path before the person could widen the origin. The zone grades its own refusal
 * (extension/lib/safe-fetch.js).
 *   The refusal makes the flow fork: one arm keeps waiting with no invented reply, the other takes the network
 * error with its path marked forced. flow_decline_fork builds the pair at a scheduler step, because an arm
 * minted between steps would clone whichever flow ran last; this only records the fact. `reason` is the zone's
 * own words and is copied. */
int engine_decline(JSContext *ctx, const char *method, const char *url, const char *reason);

/* The same refusal for a synchronous request, keyed on the rendezvous id from engine_host_requests: such a
 * request has no (method, url), and pending_push keeps FLOW_PENDING_HOSTREQ out of the pair index, so
 * engine_decline cannot reach it (a §7.4 navigation is the first case). It is a separate entry, not a third
 * completion: a decline means the operation did not happen, and a refusal arriving at engine_host_take as a
 * value is what flow_decline_fork asserts against. Exactly one flow's register can name the id
 * (engine_sibling_assemble unshares an unanswered synchronous request and mints a fresh rendezvous), so the
 * walk stops at the first match. It records the fact and does not settle the rendezvous; flow_decline_fork
 * builds the pair. Returns 1 if a record was marked, 0 if no flow is parked on that id. */
int engine_host_decline(JSContext *ctx, uint32_t req, const char *reason);

/* Install as JSTimeTravelHooks.gen_fork: a concolic branch inside a synchronously-driven generator body forked
   the flow, and clone_deep_flow built a per-flow gen_data clone. Stash the swap; engine_fork_finalize drains it
   onto the new sibling's COW delta (so the shared generator object resolves per-flow). */
void engine_gen_fork(JSContext *ctx, JSValueConst genobj, void *base_gd, void *cur_gd);

/* How many times the dispatch loop context-switched between flows, reported because an interleaving scheduler
   and a FIFO one agree on easy pages, so the interleave has to be observable on its own. Then the jobs queued
   and the jobs run. */
int  engine_switch_count(void);
long engine_jobs_queued(void);
long engine_jobs_run(void);
/* How many completed units of work this instance's flows have been credited: HTML §8.1.4.4 "Calling scripts"
   step 3 of clean up after running script, "if the JavaScript execution context stack is now empty". It is the
   precondition for running a queued job, which is what makes `jobsRun` readable. */
long engine_units_done(void);

/* Lifetime step traffic per arm of flow_step, filled in one call so every row is one reading. It is not
 * solver/cold.h's `step_units`, a gauge of members standing in each arm: a zero there says nobody is in that
 * arm now, a zero here says the ladder never reached it, and the two take opposite work. A report, never a
 * bound: nothing in the engine reads any row. The array extents are solver/step_unit.h's own list, so a caller
 * cannot size them from a second copy.
 * Microsecond rows are int64_t because `long` is 32 bits on wasm32, where a microsecond sum saturates in 35.8
 * minutes and signed overflow would read as a negative cost; solver/engine.c asserts the width at `g_step_us`.
 * They are in the slice's own measure (`quantum_thread_us()`, CPU or wall as @QUANTUM says), and a reading is
 * a ratio within one run, since totals across runs are not comparable. */
typedef struct {
    long steps;              /* scheduler steps: every entry into flow_step, counted at its own entry */
    long arms[STEP_UNIT_N];  /* …and how many ran each arm, in solver/step_unit.h's order, counted at the
                                scheduler's convergence point; engine.c asserts `sum(arms) == steps` there */
    /* Passes through flow_step's work ladder, the `if (!f->frame)` block every non-frame arm sits in, so a
       step on a framed member is in none of its arms. It is not derivable from published rows:
       `unit_mid_program` reads the frame after the step, step_unit.h labels no arm's side of the block, and
       solver/flow.h's `unframed_picks_lifetime` counts switch-ins, not descents. It separates the readings of
       `orphansAsked == 0`: zero here puts the cause upstream of the ladder, a large value puts it in an arm
       above the orphan rung. It counts passes, not entries, so it is not comparable with `steps` (the loop body
       iterates); it contains engine_orphan_census's `asked`, asserted at engine_step_unit_runs. It is per
       instance while `asked` is per session, so across a restart only the containment holds. */
    long unframed_steps;
    /* Thread measure each dispatch-loop turn that stepped a flow cost, charged at the line that computes the
       delta for flow_age_running. Charges telescope from the previous turn's end (or the slice's entry), so a
       turn includes its pick (solve_seed_candidates, flow_next_to_run), both delta swaps and the step, and
       excludes the host's time between slices. `step_us / steps` against the slice separates a slice-bound loop
       from one given little thread time; take the slice from `@QUANTUM`, the artifact's own report. */
    int64_t step_us;
    /* The split of step_us: `slice_us` is the step's own bracket and `sched_us` everything else in the turn,
     * which, because charges telescope, includes the previous turn's tail (checkpoint, finish). A step that
     * overruns the slice points at the quantum's transport; a large pick and swap points at the frontier's
     * shape. Two rows rather than a subtraction, so `slice_us + sched_us == step_us` (asserted) breaks when a
     * phase is added. */
    int64_t slice_us;
    int64_t sched_us;
    /* Thread measure since this instance's dispatch loop first ran, on the same clock as `step_us`, so
     * `step_us / instance_us` is the share of the engine's thread spent in turns; `step_us / steps` alone
     * cannot tell cheap turns from a loop barely entered. Taken in the same reading as `steps` and `step_us`;
     * `step_us <= instance_us` is asserted. */
    int64_t instance_us;
    /* The partition of instance_us: `loop_us` is thread measure inside engine_sched_step's bracket over every
     * slice, `between_slices_us` the measure between one slice's return and the next entry plus the span since
     * the last return (the host's). `loop_us + between_slices_us == instance_us` and `step_us <= loop_us` are
     * asserted, and engine_step_unit_runs asserts `!quantum_slice_open()` so the open tail is the host's. A
     * small `loop_us` means the engine was barely given the thread (look at the driver); a large `loop_us` with
     * a small `step_us` is time in the loop outside turns, first of all slices that dispatched nobody, which
     * `slices` against `steps` reads. `slices` is counted in engine_sched_step beside quantum_begin(). */
    int64_t loop_us;
    int64_t between_slices_us;
    long    slices;
    /* Turns whose step alone met or exceeded the slice budget, so `slice_overruns / steps` is a fraction of a
     * published denominator; the lifetime mean `step_us / steps` hides that a few long turns usually carry the
     * time. It compares the step's two clock readings against ENGINE_QUANTUM_MS, the margin quantum_expired()
     * uses, though on Linux quantum_expired() reads a timer-set flag rather than the clock. Neither seam verdict
     * in engine_sched_step can judge these turns: both require `g_preempt_asked == pa0`, and an overrunning turn
     * consulted the hook to end. A count, not a maximum, so it cannot plateau like a ceiling. */
    int64_t slice_overruns;
    /* slice_overruns per arm, in solver/step_unit.h's order; `sum(over_arms) == slice_overruns` is asserted
     * with the turn in hand. Read it as a rate against `arms` as well as a count, and against the arm's real
     * population: `start-a-classic-program` is only a start that returned with its frame live, one of four
     * start rows. It says which arm cannot rest, not where time went. It is not the count of turns that ended
     * on the quantum: a slice holds many turns and at most one ends on that clause, so those are bounded by
     * `slices`, and how a turn in a given arm ended (quantum, outranked, blocked) is not readable per arm. */
    long over_arms[STEP_UNIT_N];
    /* Suspend points offered over the overrunning turns (the `g_preempt_asked` delta across each turn's step
     * bracket), and how many of those turns offered none. A seamless overrun is a C activation with no step
     * boundary, fixed by a step-machine conversion in that component; one that offered points is the page's
     * own back-edge-free stretch, which is not capped. `slice_overrun_asks == 0` iff `slice_overrun_seamless ==
     * slice_overruns`, and `slice_overrun_asks <= engine_preempt_asks()`, both asserted. A seamless overrun
     * also performed no rival rescan, since flow_rival_of runs only after a consultation. Every build. */
    uint64_t slice_overrun_asks;      /* suspend points offered, summed over the turns that met the slice */
    long     slice_overrun_seamless;  /* …and how many of those turns offered none */
    /* slice_overrun_seamless per arm: a partition of it and a subset of `over_arms` per arm, both asserted in
     * the overrun branch with the turn in hand. Lifetime, every build. */
    long     over_seamless_arms[STEP_UNIT_N];
    /* slice_overrun_asks per arm (`sum == slice_overrun_asks`, asserted in the overrun branch), because arms
     * differ in density by orders of magnitude. Its denominator is `over_arms - over_seamless_arms`, since a
     * seamless turn adds nothing. A `long` because solver/result.c composes step-unit rows through one
     * `cold_hist_json` that takes `long`; the add asserts its headroom. Lifetime, every build. */
    long     over_ask_arms[STEP_UNIT_N];
    /* The longest stretch with no suspend point offered, in the slice's own measure, over non-seamless
     * overrunning turns, overall and per arm; seamless turns are excluded because their stretch is the whole
     * turn. quantum_expired() is monotone within a slice and a true answer ends the turn, so every consultation
     * of an overrunning turn falls within ENGINE_QUANTUM_MS of the slice's start and the gap lies in [span -
     * ENGINE_QUANTUM_MS, span]: to within one budget it is the arm's worst turn span. A gap far below that bound
     * means a wanted preempt was dropped (the `requested > fired` seam message). A maximum, read as a floor
     * against the budget and never as a ceiling; `max(over_gap_arms) == slice_overrun_gap_us` is asserted in the
     * overrun branch. It is not engine.c's dev-only wall `g_max_gap`, which covers seamless turns. The per-arm
     * row is `long` for `over_ask_arms`' reason, with its headroom asserted. Every build.
     *   Named residual: testing/live-run.js does not carry `stepUnitOverrunGapArms` or `sliceOverrunGapUs`, so
     * only a build's own smoke reports them; the next diff adds both to that driver's cold row lists beside the
     * ask rows; its absence shows as a real-site report quoting a per-turn ask density with no gap row to
     * contradict it. */
    int64_t  slice_overrun_gap_us;    /* the worst no-suspend-point stretch of any non-seamless overrunning turn */
    long     over_gap_arms[STEP_UNIT_N];
    /* The classic compile phase of a start step. A compile rests: JS_FlowCompileStep polls the preempt hook in
     * the parse's own dispatch and parks through `f->compile` (`compile-handed-the-thread-back`).
     * `classic_compiles` is one per program, raised when a parse finishes; `classic_compile_overruns` is one
     * per stint that met the slice, so neither is a subset of the other. Stints are `classic_compiles +
     * arms[STEP_UNIT_COMPILE_YIELDED]`, and overruns are asserted under that and under `slice_overruns`. Module
     * compiles are in none of these rows (their stint is `module-compile-handed-the-thread-back`).
     * `classic_compile_again` counts parses of bytes some flow of this process had already parsed, identity
     * being the body and not a hash; it is a floor, and `classic_compile_own_decode` (bodies one flow decoded
     * for its own delivery) bounds it, so the true figure is in [again, again + own_decode]. Both are subsets of
     * `classic_compiles` and not of each other (asserted). Do not read `classic_compiles` against
     * `root_programs`: forks, candidates, replays and appended chunks all compile. */
    long classic_compiles;           /* classic program compiles, one per program, at flow_step's start site */
    long classic_compile_overruns;   /* compile stints that met the slice; not a subset of the row above */
    long classic_compile_again;      /* …of which the bytes had already been parsed by some flow: the repeat */
    /* The bytes those repeats covered, so a repeat can be weighed as a cost; int64_t for `step_us`'s reason, and
       asserted not below its own count. */
    int64_t classic_compile_again_bytes;
    long classic_compile_own_decode; /* …whose body one flow decoded for its own delivery: the floor's bound */
    /* Classic programs started from a parse another timeline had already finished (solver/dyn_body.h); they
     * raise none of the rows above. `classic_compiles + classic_parse_shared` is the classic programs whose
     * closure was obtained, which is the figure comparable with a pre-sharing revision's `classic_compiles`.
     * Asserted: a share requires a finished parse, and `dyn_body_parses_held() <= classic_compiles`.
     * `classic_compile_again` stays nonzero by design: a failed parse holds nothing, and a flow mid-parse when
     * another finishes completes its own. */
    long classic_parse_shared;       /* classic programs started from a parse another flow had finished */
    /* Compile stints that continued a parse rather than beginning one; contained in
     * arms[STEP_UNIT_COMPILE_YIELDED] (asserted), and that arm minus this is the parses begun and not ended. Near
     * zero means the seam carries every parse forward; large means parses handed back and not picked up. Raised
     * at solver/engine.c's g_classic_compile_resumed. */
    long classic_compile_resumed;    /* compile stints that continued a parse; not a subset of the programs */
    /* Why turns that did not end a unit of work did not: the refusal arms of the gate behind `_unitsDone`. With
     * the credited arm they sum to `steps` exactly, asserted in engine.c where the credited arm is written. A
     * dominant `unit_mid_program` is a forking frontier's expected shape, since an arm is born holding the frame
     * taken at its branch. */
    long unit_mid_program;      /* …the member held a live frame: inside a program, the trial still running */
    long unit_parked;           /* …the runtime held a parked continuation: suspended on an await or a reply */
    long unit_checkpoint_owed;  /* …the flow still owed its microtask checkpoint: its own reactions unrun */
    /* Arrival counts at the clock boundary, raised ahead of each rung's gate, so a zero outcome arm in `arms`
     * can be told apart from a rung never reached. They are suffix sums over one `else if` chain, asserted at
     * engine_step_unit_runs:
     *     clock_render_asks == the eleven arms of `arms` at or below the boundary (listed in solver/engine.c)
     *     clock_timer_asks  == clock_render_asks - arms[STEP_UNIT_RENDERING]
     *     clock_idle_asks   == clock_timer_asks  - arms[STEP_UNIT_TIMER]
     *     clock_render_asks <= unframed_steps
     * Lifetime and per instance, like `arms`, so the identities survive a session restart. */
    long clock_render_asks;  /* descents that reached the rendering rung: the clock boundary's arrival count */
    long clock_timer_asks;   /* …of which the rendering rung declined: the timer rung's arrival count */
    long clock_idle_asks;    /* …of which the timer rung declined too: the idle rung's arrival count */
} EngineStepUnitRuns;
void engine_step_unit_runs(EngineStepUnitRuns *out);

/* Which arm took the step of a member that had a runnable task on its own queue: the two arms above the task
 * arm that can be reached with one, and the task arm's two reasons. Read them against the `jobsReadyTask` gauge
 * and the `run-a-task` arm; `task_arm_older + task_arm_no_row == run-a-task` is asserted in
 * engine_ladder_task_census. Lifetime counts, released by nothing; solver/engine.c states the mechanism at the
 * counters. `task_held_deliv` changed meaning when replies gained PEND_WORK_SEQ (it now counts deliveries
 * ahead of younger tasks), so compare it only across artifacts that both carry that stamp. */
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
