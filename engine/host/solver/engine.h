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
#include "core/loader/script_type.h"   /* which of §8.1.3.3's two algorithms runs entry i — see `types` below */
#include "quickjs.h"

/* Run the page's scripts as one code flow: each script `bodies[i]` is its OWN program (JS_FlowNew — faithful
   per-<script> scope, NEVER concatenated), run in document order, sharing globals + the flow's COW delta. */

/* Queue a script body to run in the CURRENT flow after the current script, sharing its globals + COW delta.
   Three callers, and they are one thing — code the page caused to run: a DYNAMICALLY-LOADED body (a lazy
   chunk / injected <script> / import()), a `setTimeout` STRING handler, and the classic scripts of a DOCUMENT
   this flow just created (§7.4 step 14's load, whose realm builder hands them over). Because a load sits
   behind a branch, the ONE BFS discovers different lazy scripts on different arms — lazy loading is not a
   separate system, just more code the flow runs and forks through. The body is copied; the queue is per-run
   and drained by the flow that owns it.
   `doc` NAMES WHICH DOCUMENT'S PROGRAM IT IS, which is WHERE IT IS COMPILED (solver/flow.h's `dyn_doc`). An
   instance is an ORIGIN-KEYED AGENT CLUSTER, so this is a child navigable's document as often as the
   session's, and a program compiled in the wrong realm is closed over the wrong Window — it defines the
   child's globals on its creator and reads the creator's back as the child's. There is no default: the caller
   knows which document's code it is holding, and a scheduler that guessed would guess the same way for every
   document this agent has. */
void engine_queue_script(uint32_t doc, const char *body);
/* An @S CANDIDATE, queued as the program it would be if it fired. Same queue, one difference: it is ALLOWED not
   to compile, because most breakouts do not fit most sink contexts and a candidate that does not parse simply
   never fires. A page script that does not compile still asserts. */
void engine_queue_candidate(const char *body);
/* A `javascript:` URL's SCRIPT SOURCE, queued as the program HTML §7.4.2.3.2's evaluate-a-javascript:-URL runs.
   Same queue, and it is the same thing — code the page caused to run — but it differs from a page <script> at
   both ends of that program's life, which is why it is its own entry point rather than a third caller of the
   one above.
     It is ALLOWED NOT TO COMPILE. "Create a classic script" with a syntax error produces a script whose
   evaluation is an abrupt completion, which the caller turns into a null newDocument and a navigation that does
   not happen — a page's `javascript:{{{` does nothing at all, where a <script> that will not parse is this
   engine's own assert.
     Its COMPLETION VALUE DECIDES A NAVIGATION. A <script>'s is unobservable; this one's is step 9's whole
   condition — "if evaluationStatus.[[Value]] is a String", the Document is REPLACED by an HTML parse of that
   string. The scheduler is the only place that value exists, so that is where the engine says it cannot yet act
   on one.
     `doc` is the TARGET NAVIGABLE'S ACTIVE DOCUMENT, which step 5 names as the settings object the classic
   script is created with — the realm it is compiled in, and not always the session's. */
void engine_queue_javascript_url(uint32_t doc, const char *body);
/* Park the running flow on an injected <script src>: the host fetches it, and the reply becomes this flow's next
   program rather than a promise's value. */
void engine_pending_script_url(JSContext *ctx, const char *url);
/* Park the running flow on the document's OWN external <script src> at position `script_i`: classic scripts run
   in document order, so the flow waits there, and the reply fills the shared slot every flow reads. */
void engine_pending_docscript(JSContext *ctx, const char *url, int script_i);
/* Park the running flow on a PUBLISHED API DESCRIPTION — the engine's own active discovery (solver/discovery.h).
   It takes a URL and NOTHING ELSE: the verb is the constant GET inside, so "a state-mutating request is NEVER
   fired to learn" is the shape of this entry point rather than a check at its callers. */
void engine_pending_discovery_url(JSContext *ctx, const char *url);

/* THE DOCUMENT'S LOAD LIFECYCLE, owned by the browser layer and asked by the scheduler. Called once per stage
   per flow when that flow has run everything the document gave it: stage 0 fires DOMContentLoaded, stage 1
   fires load. Returns how many listener tasks it scheduled. Registered by the host that owns a Document; a
   scheduler with no document (the solver fixture) simply never has one. */
/* The event loop's timer step (timer.h). Registered by the timer component; asked when a flow is idle. */
void engine_set_timer_hook(int (*fn)(JSContext *ctx));
/* HTML §8.1.7.3's IN-PARALLEL half — the rendering task source (rendering.h). Registered by the rendering
   component for the reason the timer step is registered rather than named: the scheduler may not depend on the
   browser half. Asked at the same moment and immediately BEFORE the timer step, because the two are due at
   moments on the ONE virtual clock and this one defers to a timer that expires first. */
void engine_set_rendering_hook(int (*fn)(JSContext *ctx));
void engine_set_document_done_hook(int (*fn)(JSContext *ctx));

/* solver_decide calls this at a forking branch to stash the sibling's hot decision + pins; the interpreter's
   fork hook (engine_fork_finalize) assembles the sibling from the frame clone + these. */
void engine_prepare_fork(void *dec_blob, void *pin_blob);

/* Run the scripts to frontier exhaustion: seed the first flow, bracket each run with the decision state +
   per-flow COW delta, and drain the frontier by WFQ order.
   `recipes` is the PARKED RESIDUE this host stored for the document, or NULL/"" for one with none — the same
   parameter engine_sched_begin takes and for the same reason, because the choice between a residue and a boot
   flow is the SCHEDULER's and not the store's. This driver used to pass NULL unconditionally, on the reasoning
   that the cold tier "belongs to the host that has an IndexedDB"; that made the entire resume path unreachable
   from every host in this tree that can be run without a browser, so the language cold_resume parses had no
   producer any process could reach. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types, int n,
                const char *recipes);

/* …AND WHETHER THIS HOST WANTS THE RESIDUE, asked at each step boundary of the one-call driver. It is the
   Level-1 eviction seam: the HOST decides there is pressure (it is the only zone that can see the other
   documents' engines and the summed working set) and the ENGINE decides when — the next boundary with no flow
   switched in, which is the only moment every flow's state is in its own blob. A non-zero answer requests the
   park (engine_request_park); a host that never evicts installs nothing, which is every existing caller.
   IT IS A QUESTION, NOT A BUDGET. The park writes EVERY member of the frontier and the host stores it, so what
   this decides is when the residue leaves memory and never how much of it survives. */
void engine_set_park_hook(int (*want_park)(void));

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
#define ENGINE_QUANTUM_MS  12  /* a thread-sharing floor, not a cap: nothing is dropped across it. It is a budget
                                  of CPU actually consumed — solver/quantum.h owns the edge that expires it and
                                  says what each host can measure. */
/* THE SEAM ASSERTION'S MARGIN, counted in WORK the step performed (forks + flows created + jobs run) rather
   than in milliseconds — see the verdict in engine_sched_step for why a WALL clock cannot decide this on a
   loaded machine. A step that performs this much work without once consulting the preempt hook has no
   suspend/resume seam on that path, whatever else is running on the box. Deliberately enormous: an ordinary step
   forks a handful of times between two suspend points, so nothing short of a genuinely non-returning stretch
   approaches it. It is a DIAGNOSTIC, never a bound — it truncates no work, drops no flow, and is compiled out
   of release. */
#define ENGINE_SEAMLESS_WORK 1000
/* THE OTHER MARGIN, in CPU ACTUALLY CONSUMED, for the seamless stretch the work count is blind to by
   construction: a bare `for(;;);` inside C forks nothing, queues nothing and emits nothing, so it reaches
   ENGINE_SEAMLESS_WORK never and hangs the engine silently. Consumed CPU is the one quantity a loaded machine
   cannot inflate, which is what makes this decidable where a wall clock was not — so it is asked only where
   quantum_measure_is_cpu() says the reading IS CPU. 400x the quantum: anything under it is merely a slow step,
   anything over it has offered the scheduler nothing across four hundred slices' worth of thread it actually
   burned. A DIAGNOSTIC on the same terms as the one above — no truncation, no drop, absent in release. */
#define ENGINE_SEAMLESS_CPU_US ((int64_t)ENGINE_QUANTUM_MS * 1000 * 400)
/* WHAT THE HOST IS OWED. The scheduler asks this ONE seam before it decides the frontier is exhausted; a
   non-zero answer means STALLED rather than DONE. It is a question, not policy: the scheduler holds no idea of
   what a reply is, and the host holds no idea of what a flow is. */
void engine_set_stall_hook(int (*owed)(void));

/* THIS INSTANCE'S DOCUMENT IS ONE ANOTHER INSTANCE HOLDS A REFERENCE INTO, so its timelines may not RUN OUT.
   A document's state IS its flows (engine_perform says so where it attaches an operation to every one of
   them), so a peer that still holds a WindowProxy for this document can ask it something at any moment — and a
   frontier that drained has nothing left to answer in. Seeding a fresh flow at that point is not the same
   document: the page's own scripts wrote into the delta of the flow that ran them, so a flow starting from the
   baseline answers about a document where none of them ever ran, silently.
   So the last timeline does not finish; it reports itself HOST-OWED (flow.h) — waiting, not finished — and the
   session STALLS instead of closing. Nothing spins on that: a host-owed flow is out of the pick, so the
   scheduler returns to the host, which is blocked on the channel the next operation arrives over. It is the
   ordinary park: the flow keeps its snapshot, its delta and its place in the WFQ, and the operation that
   arrives is the work that resumes it.
   Set by the HOST, because whether a peer holds a reference is a fact about the instance's provisioning and not
   one the engine can see: a child instance exists precisely because some other agent created its navigable. */
void engine_set_referenced(int referenced);

/* HOW THE HOST PAYS WHAT IT OWES, for the driver that runs the frontier to completion in one call
   (engine_run). The stepping entry answers a stall by returning to its caller, which is what the extension's
   qjs_step does; a one-call driver has no such caller, so a stall with nobody to answer it ended the run — and
   a flow that had issued a request stopped at that request, its continuation never reaching the reply. The
   provider fills what engine_pending_urls and engine_host_requests name, and answers how many entries it
   filled; 0 at a stall ends the run, which is the honest answer to "nobody can supply this".
   IT IS CALLED ONLY ON ENGINE_STEP_STALLED, AND THAT IS A KNOWN DEFECT rather than the design — the extension's
   bridge pulls both registers after EVERY return (main.c folds STALLED into YIELD deliberately), so in the
   product a blocked flow is answered at the next quantum, while here it is answered only once every OTHER flow
   in the document has also blocked. That makes one flow's reply a function of its siblings' states, which is a
   coupling the scheduler forbids everywhere else. Turning it into a per-slice payment is the fix and it is not
   free: the attempt aborted the smoke fixture in the fetch delivery on a record with no §2.2.6 URL list, which
   the drain's own assert now names at the entry rather than three frames later. run_scheduler carries the
   sequence to follow. */
void engine_set_provider(int (*provide)(JSContext *ctx));

/* `recipes` is the PARKED RESIDUE the host stored for this bundle (';'-joined records — see solver/cold.h), or
   NULL/"" for a document with none. It seeds the frontier INSTEAD of the boot flow, never beside it: a resumed
   flow re-runs the same document under its own recorded arms, so adding a fresh boot flow would explore the
   un-forked path twice and re-fork every branch the residue already stands on.
   `types[i]` is entry i's HTML §4.12.1 script type, and it is what the compile ASKS rather than assumes: a
   classic script is wrapped in a preemptible program frame (JS_FlowNew) and completes with a value, while a
   MODULE is linked and evaluated (JS_FlowEvalModule) and completes with a PROMISE. The scheduler cannot
   recover the kind from the body — `await` at the top level is a SyntaxError in one and legal in the other,
   which is exactly the difference that has to arrive from the element. The array is BORROWED for the life of
   the session, like `bodies` and `srcs`. */
void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types, int n,
                        int forking, const char *recipes);
int  engine_sched_step(void);

/* THE RAM→DISK FLOOR. The HOST sees the pressure (it is the only zone that knows the other documents' engines
   and the summed working set) and asks this engine to give up its residue; the ENGINE decides when, which is
   the next step boundary with no flow switched in. The step that takes it writes the park document
   (cold_park_json, published in the result) and answers ENGINE_STEP_DONE — the session is over, and the flows'
   memory goes with the instance the host then tears down. STARVE means deprioritize-and-page: nothing is
   dropped, truncated or forgotten, and the residue comes back through the same admission step it left by. */
void engine_request_park(void);
/* WAS THIS WHOLE FRONTIER WRITTEN OUT rather than drained? The INSTANCE teardown's asserts read it (main.c):
   the replies the host still owed and the synchronous requests still outstanding are re-issued by the replay
   each recipe drives, so neither is the dropped work item those asserts exist to catch.
   IT IS AN ENGINE-WIDE ANSWER AND IT IS NOT WHAT A SINGLE FLOW'S RELEASE MAY ASK. A partial self-park writes a
   low-value TAIL and releases it while everything above it keeps running and stays unwritten, so this would be
   true for the tail and false for the rest at the same instant; a flow carries its own `paged` (solver/flow.h)
   and flow_release asks that one. */
int  engine_frontier_paged(void);

/* WAS A REPLY OWED TO A FLOW THIS ENGINE PAGED OUT? Answers once per such reply and consumes it, which is why
   it is a `take` and never a predicate: the partial self-park sells the lowest-weight member at the RAM floor,
   and a BLOCKED flow is the cheapest thing there is to sell (its recipe re-issues the request next session and
   gets today's answer). The reply the host is already fetching then lands with nobody parked on it. That is a
   sale, not the mispairing the provide edge asserts against, and this is what tells the two apart. */
int  engine_take_paged_owed(void);

/* The same park, with the URL only the TRUSTED HOST can fetch. The value arrives later through engine_provide;
   until it does the flow cannot finish, which is what keeps reply-gated code reachable. ONE register — the
   flow's own — because the reaction the resolve enqueues belongs to that flow and to its COW delta. */
void engine_pending_fetch_url(JSContext *ctx, JSValueConst resolve, JSValueConst value,
                              const FetchRequest *req);
/* The same park for a DYNAMIC `import()`, whose delivery differs: a module load is owed SOURCE TEXT, so the
   drain settles `resolve` with the reply's BODY rather than with the reply record `fetch()` makes a Response
   out of. Sharing the fetch park handed the module compiler that record. */
void engine_pending_module_url(JSContext *ctx, JSValueConst resolve, const char *url);
/* THE FRONTIER'S BEST WEIGHT — what the host ranks this document's engine by against every other live one.
   Level-1 and level-2 are ONE policy (§scheduler): the host orders engines by their best flow exactly as the
   engine orders flows, so this is flow_weight of flow_best and nothing else. -inf when the frontier is EMPTY,
   so an engine with no work never outranks one that has some.
   IT IS EVERY MEMBER, INCLUDING THE ONES WAITING ON THE HOST, and the line that used to say "-inf when nothing
   is runnable" claimed otherwise. It could not be made true here: the host reads this BETWEEN slices, and a
   flow's host-owed mark is cleared at the top of every slice precisely because the host is what answers it — so
   at the moment this is asked, every member is askable again by construction. The Level-1 question it sounds
   like ("is this engine merely waiting?") is answered where it is known, by the step code: a slice whose every
   member is waiting returns ENGINE_STEP_STALLED, which is what moves the engine out of the pool's hot state. */
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
/* Has it been answered? BORROWED, so a machine re-entered before it is ready to consume may read it again.
   It answers about the ANSWER'S ARRIVAL and not about its completion type: a throw has arrived exactly as a
   value has, and a machine that yielded until "answered" must be re-entered for either. */
int      engine_host_answered(uint32_t req, JSValueConst *out);

/* AN ANSWER IS A COMPLETION (ECMA-262 6.2.4), NOT A VALUE — and that is the whole reason these three
 * signatures carry a type beside the value.
 *
 * A peer resolves a cross-instance operation by RUNNING A PROGRAM, and a program either returns or THROWS. A
 * channel with a field for the value and none for its type delivers the peer's throw as `undefined`: the
 * asking flow's `try { remote.x = 1 } catch (e) {}` never runs its handler, and the flow proceeds on a write
 * that did not happen. So the type is a parameter of the delivery rather than a second entry point beside it —
 * a host answering a request must decide which completion it is answering with, and cannot answer without
 * saying. The THROWN VALUE is a value like any other and crosses by the same rules: an Error is an object, so
 * it crosses as a NAME (remote_object.h) and the catch clause holds a reference to the peer's Error. */
enum { ENGINE_COMPLETION_NORMAL, ENGINE_COMPLETION_THROW };

/* Take the answer; the request leaves the register. The value is OWNED by the caller, and `*pcompletion` says
   what it IS — a result, or a thrown value to re-raise. Required, because a taker that does not read the type
   is a taker that delivers a throw as a value. */
JSValue  engine_host_take(JSContext *ctx, uint32_t req, int *pcompletion);
/* TAKE THE ANSWER AS THE COMPLETION IT IS, which is what every cross-instance step machine wants: a normal
   completion's value is placed in `*presult` and the machine is DONE; a THROW is RE-RAISED in the asking flow
   at the call site that parked on it, exactly as it would have been raised had the operation been local, and
   the machine is ABRUPT. Returns JS_STEP_DONE or JS_STEP_ABRUPT (quickjs-step.h) — one place that knows a
   peer's throw comes back as a throw, rather than that knowledge copied into each machine. */
int      engine_host_take_completion(JSContext *ctx, uint32_t req, JSValue *presult);
/* WHO COMPUTED THE ANSWER, and it is a parameter because the two have different MULTIPLICITIES — the one thing
 * about a delivery that only the caller can state.
 * A HOST answer is a value the trusted zone computed ITSELF (§7.4 step 14's load, XHR §3.5.6's fetch): there is
 * exactly one of it, so a second one is that zone answering twice and is its bug.
 * A PEER answer is a completion another instance's flow produced by RUNNING A PROGRAM, and a peer's document
 * state IS its flows — so one question has N answers for N of its timelines and every one of them is true. The
 * asking flow forks one arm per distinct answer (engine.c), which is a fork over a VALUE rather than over a
 * predicate and is exactly as much a fork as a branch's.
 * Sniffing the request's op text to tell them apart would be the recognizer shape this codebase bans; the two
 * deliveries are different operations and say so. */
enum { ENGINE_ANSWER_HOST, ENGINE_ANSWER_PEER };

/* The host delivers. Routed by id to ONE call site — never broadcast the way a fetched body is. Returns 0 when
   the asking flow is gone, which is not an error: nobody is waiting. */
int      engine_host_answer(JSContext *ctx, uint32_t req, JSValueConst value, int completion, int source);
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
/* THE INBOUND HALF: a record another instance emitted as a notice, routed HERE by the trusted zone because this
   instance holds the document it names, with the SENDER'S ORIGIN stamped by that zone (the untrusted engine may
   not compute one for a foreign message — SECURITY.md).
   IT MAKES THE RECORD A WORK ITEM OF EVERY LIVE TIMELINE OF THE RECEIVING DOCUMENT and returns; each of those
   flows makes its own delivery when the scheduler next runs it, in its OWN world. That is not the same as
   seeding one flow under the SENDER's world, and the difference is the whole of why this shape exists: the
   page's `message` listener was registered by a script, so it lives in the delta of the flow that ran it, and a
   delivery made anywhere else arrives at a document where nothing is listening. What the sender's world
   contributes is its SEGMENT in this instance — see engine.c, where the conjunction of the two is stated and
   the part of it that cannot yet be built crashes. There is no inbound queue because the frontier is one. */
void engine_route(JSContext *ctx, const char *record, const char *sender_origin);

/* THE INBOUND HALF THAT OWES AN ANSWER — a cross-agent OPERATION (core/frame/remote_op.h) another instance's
   flow is parked on, routed here by the trusted zone because this instance holds the document it names.
   IT IS ATTACHED TO EVERY LIVE TIMELINE, exactly as a routed delivery is, and for a reason the one-way case
   only hints at: a peer's document state IS its flows, so `otherW.length` has N answers for N timelines and a
   channel with one answer slot would silently pick one. Each flow performs the operation as its own next
   PROGRAM — a peer answers by running one, never by reading a property from C — and emits that program's
   COMPLETION as a notice naming `token`, which the zone routes back to the instance and request that asked.
   `token` is the ZONE's rendezvous, opaque here: the asking flow's request id is unique only inside the
   instance that minted it, and two peers may ask this one the same number. Nothing runs inside this call. */
void engine_perform(JSContext *ctx, const char *token, const char *record);

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
