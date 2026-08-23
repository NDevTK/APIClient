/* The DISPATCH LOOP — drains the WFQ frontier. Each flow is the page's scripts run as ONE preemptible program
 * (JS_FlowNew), replaying the flow's decision vector; the first flow is the empty vector. A concolic branch
 * inside a run forks a sibling flow (decide.c); the loop keeps running the highest-value flow until the
 * frontier is empty. There is NO separate boot executor — the scripts ARE the first flow.
 *
 * A @S candidate re-fire is a FLOW seeded onto this same frontier (solve_seed_candidates), not a separate
 * executor: one scheduler runs exploration and verification alike. */
#ifndef ENGINE_HOST_SOLVER_ENGINE_H
#define ENGINE_HOST_SOLVER_ENGINE_H

#include <lexbor/dom/dom.h>

#include "core/fetch/fetch.h"
#include "core/loader/script_type.h"   /* which of §8.1.4.4's two algorithms runs entry i — see `types` below */
#include "quickjs.h"

/* Run the page's scripts as one code flow: each script `bodies[i]` is its OWN program (JS_FlowNew — faithful
   per-<script> scope, NEVER concatenated), run in document order, sharing globals + the flow's COW delta. */

/* WHERE IN THE FLOW'S OWN PROGRAM SEQUENCE A QUEUED PROGRAM LANDS. It is a SPEC fact about the operation that
 * caused the program, not a scheduling preference, which is why the CALLER states it and the queue never
 * guesses — the same sentence engine_queue_into already makes about which flow and which document.
 *   DYN_POS_APPEND is what a TASK is. An `error`/`load` event fired at an element, a `javascript:` navigation
 * (HTML §7.4.2.2 "Beginning navigation" queues a global task on the navigation and traversal task source to
 * reach §7.4.2.3.2), a §8.6 string timer, a lazy chunk's reply, a document's own sequence being filled one
 * entry at a time: each of those is queued and runs when the sequence reaches it, and a task queue is FIFO,
 * so the TAIL is its position.
 *   DYN_POS_IMMEDIATE is where a program the RUNNING program caused runs. HTML §4.12.1.1 "Processing model"
 * ends "prepare the script element" with "Otherwise, immediately execute the script element el, even if other
 * scripts are already executing"; ECMAScript §19.2.1.1 PerformEval pushes evalContext, evaluates the body,
 * pops it, and returns that completion INTO the call expression. Both say the caused program runs before the
 * next thing the sequence holds — never after everything it holds.
 *   THE DEFAULT USED TO BE APPEND AND WAS NEVER STATED, and that is how a proof this solver had ALREADY
 * CONSTRUCTED became conditional on the flow draining the whole rest of the document. §@S records a finding at
 * the marker precisely so nothing waits for a flow to reach completion (solver/solve.c says so at js_x9); a
 * fired PoC placed at the tail of an unbounded sequence reintroduces exactly that wait one layer down, where
 * no assert was looking. A kill, a park or an eviction then loses the proof FIRST, and reports it as a search
 * that has not solved — the one verdict §@S forbids being arrived at by omission. */
typedef enum { DYN_POS_APPEND, DYN_POS_IMMEDIATE } DynPos;

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
   document this agent has.
   EVERY ONE OF THOSE THREE IS A TASK, so this entry is DYN_POS_APPEND and cannot be asked for anything else:
   a lazy chunk's reply, a §8.6 string handler and a document's own sequence each run when the sequence reaches
   them. The one script source that is NOT a task has its own entry below.
   AND EVERY ONE OF THEM IS A CLASSIC SCRIPT, which is HTML §8.1.4.4 "Calling scripts"'s answer for a program
   with no `<script>` element behind it rather than a default this entry picks: §8.6's string handler is
   evaluated as a classic script and a lazy chunk's reply is the body an already-running program asked for. A
   row that DOES have an element behind it states which of §8.1.4.4's two algorithms runs it — the entry below
   this one. */
void engine_queue_script(uint32_t doc, const char *body);
/* …AND THE ROW A `<script>` ELEMENT PUT THERE, at the same position and carrying one more fact. HTML §4.12.1.1
   "Processing model"'s "execute the script element" ends in a switch on the ELEMENT's type — "classic" runs the
   classic script, "module" runs the module script — so the row carries `stype` and flow_step routes it to
   §8.1.4.4's run-a-classic-script or run-a-module-script. Three seams reach it, and they are the three ways a
   Document of this agent that is NOT the session's gets its inline programs: a child navigable's
   (core/frame/navigable.c), a joined one's (engine_join_document) and an element page code INSERTED
   (core/html/html_script.c). Each of those aborted outright on `<script type=module>` before this existed.
   THE TAIL IS ITS POSITION, and that is §4.12.1.1's own answer for every destination a module or an external
   script reaches: the when-parsed list and the in-order-as-soon-as-possible list hold their elements in order,
   and the as-soon-as-possible SET has no position at all (§13.2.7 waits for it only before the load event). The
   one destination that is not the tail is `immediately execute the script element`, which §4.12.1.1 reaches
   only for an inline CLASSIC script — the entry below. */
/* `el` IS THAT ELEMENT, and the row carries it for the same reason it carries the type: "execute the script
   element" is a switch on EL, and its "classic" arm sets that document's §3.1.7 `currentScript` to it for the
   whole of the run. The run is a WORK ITEM here — it starts in one scheduler step and completes in another —
   so nothing at the completion could re-derive which element it was, and a C save/restore bracket around the
   compile would set the slot for whichever flow was running when the NEXT program started. See solver/flow.h's
   `dyn_el`. */
void engine_queue_element_script(uint32_t doc, const char *body, ScriptType stype, lxb_dom_element_t *el);
/* …AND THE ONE THAT IS NOT. HTML §4.12.1.1 "Processing model": an inline classic script whose element a page
   INSERTED reaches the end of "prepare the script element" — "Otherwise, immediately execute the script
   element el, even if other scripts are already executing" — and "execute the script element" then runs the
   classic script right there. So it takes the slot AFTER the program that inserted it rather than the tail,
   and nothing the sequence already holds may run in between. Separate from engine_queue_script rather than a
   flag on it, because the two are different spec steps and the four callers of that one must not be able to
   pick this by accident.
   THE TYPE IS CLASSIC AND IS NOT A PARAMETER: §4.12.1.1 reaches this step only for what falls past "If el's
   type is `classic` and el has a src attribute, or el's type is `module`", so every module — inline or not —
   has already gone to one of the three lists by then. An inline module has a graph to LOAD before its result
   exists, which is why the standard does not run it in place. */
/* `el` IS THE ELEMENT THE PAGE INSERTED — see engine_queue_element_script. */
void engine_queue_script_immediate(uint32_t doc, const char *body, lxb_dom_element_t *el);
/* THE SAME POSITION IN THE SAME SEQUENCE, FOR A SCRIPT WHOSE SOURCE IS AN ADDRESS. §4.12.1 fixes an external
   script's position against the scripts written around it — a `pending parsing-blocking script` blocks the
   tokenizer (§13.2.6.4.8), and the `list of scripts that will execute when the document has finished parsing`
   runs IN ORDER (§13.2.7) — so the entry occupies that position with only its URL, the flow WAITS there, and the
   host's reply becomes the program in the slot. Without it a document's external scripts could only park on
   their replies and run in ARRIVAL order, which is why an inline script after a `<script src>` used to abort.
   `url` is already resolved: §4.4's API base URL belongs to the document whose element it is, so only the
   caller can resolve it. The ASAP SET does not come here — it has no position, so it parks with
   engine_pending_script_url and runs when its reply drains.
   `stype` IS THE ELEMENT'S, and it survives the reply: §8.1.4.2 "Fetching scripts" decodes a module's bytes as
   UTF-8 whatever the response says and a classic script's through the response's charset label, and §8.1.4.4
   then runs the source with the matching one of its two algorithms. The row keeps its ADDRESS across that
   replacement too — §8.1.4.2 creates the script with the RESPONSE'S URL, which is the base a nested
   `import('./chunk.js')` resolves against and, for a module, the module map KEY. */
/* `el` IS THE ELEMENT WHOSE `src` THIS IS — see engine_queue_element_script. It survives the reply exactly as
   the type and the address do: the row is the element's program whether its bytes have arrived or not. */
void engine_queue_docscript_url(uint32_t doc, const char *url, ScriptType stype, lxb_dom_element_t *el);
/* An @S CANDIDATE, queued as the program it would be if it fired. Same queue, one difference: it is ALLOWED not
   to compile, because most breakouts do not fit most sink contexts and a candidate that does not parse simply
   never fires. A page script that does not compile still asserts.
   `pos` IS THE SINK'S OWN SEMANTICS AND NOT THE SOLVER'S PREFERENCE, which is the whole reason it is a
   parameter here. An eval sink IS ECMAScript §19.2.1.1 PerformEval, so its code runs inside the call
   expression — IMMEDIATE. A markup sink's auto-firing `onerror`/`onload` and a URL sink's `javascript:`
   navigation are TASKS, so they take the tail like every other task — APPEND. §@S's "the firing vector is
   chosen per sink from its real semantics" is the same sentence about the same table. */
void engine_queue_candidate(const char *body, DynPos pos);
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
/* Park the running flow on a <script src> WITH NO POSITION TO HOLD: the host fetches it, and the reply becomes
   this flow's next program rather than a promise's value. Two kinds of element are that — one a page INJECTED,
   and a member of §4.12.1's `set of scripts that will execute as soon as possible`, which is a SET (§13.2.7
   waits for it only before the load event, so arrival order is a correct order). An element whose position
   §4.12.1 does fix takes a slot instead: engine_queue_docscript_url.
   `stype` travels with the park for the reason it travels with the row above: the reply is a PROGRAM, and
   §4.12.1.1's "execute the script element" switches on the element's type to decide which of §8.1.4.4's two
   algorithms runs it. `<script type=module src>` injected by page code is how a modern bundle loads a chunk. */
/* `el` travels with the park for the same reason `stype` does, and the park is where it would OTHERWISE BE
   LOST: the flow leaves the insertion steps with the node in hand and comes back to a URL and a reply, so the
   element rides the register (solver/pending.h's `scriptEl`) and the drain puts it on the row. */
void engine_pending_script_url(JSContext *ctx, const char *url, ScriptType stype, lxb_dom_element_t *el);
/* Park the running flow on a document's OWN external <script src> at sequence position `script_i`: the scripts
   §4.12.1 orders run in that order, so the flow waits there and the reply fills the slot. THE SLOT IS ALWAYS
   THIS FLOW'S OWN DYN_SCRIPT_SRC ROW — the session document's scripts are seeded as rows of the same table as
   every other document's, so there is no shared half left to be in. One host fetch still answers every flow
   parked on that address: engine_provide fills every register naming it and un-marks each of those flows. */
void engine_pending_docscript(JSContext *ctx, const char *url, int script_i);
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
/* THE END OF A MICROTASK CHECKPOINT — HTML §8.1.7.3's "perform a microtask checkpoint", the step that runs
   once the microtask queue has drained and before the checkpoint flag is cleared. It is a SCHEDULER fact and
   nothing else can answer it: the checkpoint is over exactly when the flow that just ran a unit of work holds
   no microtask, which is a property of the frontier and not of any call site.
   Registered by the browser component that owns the steps HTML invokes there, for the same reason the timer
   step is registered rather than named — the scheduler may not depend on the browser half. Today's one caller
   is Indexed Database §2.7.1's "cleanup Indexed Database transactions", which is what deactivates a
   transaction a script created and left active; its own note ("the steps are run at most once for each
   transaction") is why asking at every step costs nothing. */
void engine_set_checkpoint_hook(void (*fn)(JSContext *ctx));

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
/* `els[i]` is entry i's `script` ELEMENT, which HTML §4.12.1.1's "execute the script element" needs and which
   only the scan that built the table can supply — see engine_sched_begin, which this hands it straight to. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                lxb_dom_element_t **els, int n, const char *recipes);

/* …AND WHETHER THIS ENGINE SHOULD LEAVE MEMORY NOW, asked at each step boundary of the one-call driver. It is
   the Level-1 eviction seam: the HOST decides (it is the only zone that can see the other documents' engines and
   the summed working set) and the ENGINE decides when — the next boundary with no flow switched in, which is the
   only moment every flow's state is in its own blob. A non-zero answer requests the park (engine_request_park);
   a host that never evicts installs nothing.
   IT IS A QUESTION, NOT A BUDGET. The park writes EVERY member of the frontier and the host stores it, so what
   this decides is when the residue leaves memory and never how much of it survives.
   PRESSURE IS NOT THE ONLY REASON, AND THE SEAM NEVER ASKED FOR ONE. The other is a host that has what it came
   for: the fixture is finished when the document it is DEMONSTRATING has answered every statement it makes, and
   its frontier is unbounded, so "the frontier drained" is a completion condition no document owes it. Both
   answers mean the same thing here — this engine leaves memory, with its residue written down — which is also
   why there is no "stop driving" seam beside this one: a driver that merely stopped would hand the teardown
   flows suspended mid-frame with replies outstanding, and flow_release asserts that is a DROPPED work item. A
   residue is parked or it is nothing. */
void engine_set_park_hook(int (*want_park)(void));

/* THE WORK THIS ENGINE HAS PERFORMED — forks taken, flows created, jobs run, context switches. Exported because
   a host that REPORTS on its own run needs the cadence to be the same quantity the engine's own progress stream
   uses; a host carrying its own would be a second definition of "has anything happened", and the one that
   already existed went silent exactly when there was most to say (see the definition). */
long engine_work_done(void);

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
/* WHAT THE HOST IS OWED — non-zero if ANY member of the frontier is still waiting on something only the host
   can supply, which is the scheduler's own last question before it may call a frontier exhausted (STALLED
   rather than DONE). It is the union of the two lists below and is answered from the same registers they walk,
   which is the correction: this was a host CALLBACK, every host restated it, and main.c restated it wrong by
   naming only the reply register — so a frontier suspended entirely inside cross-instance reads reported
   exhausted and its continuations died with the session. A host may still ask it (the fixture's park hook does,
   because a park wants the same moment), but nothing has to TELL the engine any more. */
int engine_host_owes(void);

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
   provider fills what engine_pending_fetches and engine_host_requests name, and answers how many entries it
   filled; 0 at a stall ends the run, which is the honest answer to "nobody can supply this".
   IT IS CALLED AT EVERY SLICE, which is the fix this paragraph used to describe as a known defect with the
   sequence to follow written out beneath it. That instruction OUTLIVED the absence it named: run_scheduler
   pays the provider unconditionally after every engine_sched_step and asserts at the seam that the payment
   left nothing outstanding, so both registers are settled at the next quantum exactly as the extension's
   bridge settles them — a blocked flow's reply is no longer a function of every sibling in the document also
   having blocked. What the STALL still decides is only whether the driver STOPS: a stall with `filled == 0` is
   a frontier waiting on something outside this host's tables, and nothing this provider holds will move it. */
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
   the session, like `bodies` and `srcs`.
   `bodies[i]` and `srcs[i]` are the TWO INDEPENDENT ITEMS core/loader/document_scripts.h states — source text,
   and the address §8.1.4.1 "Scripts" makes the script's base URL — so a host that has already fetched an
   external entry passes BOTH and the row runs as a program AT its own address. Only NEITHER is refused. */
/* `els[i]` is entry i's `script` ELEMENT (core/loader/document_scripts.h), BORROWED for the life of the
   session like the three columns beside it. It is what HTML §4.12.1.1 "Processing model"'s "execute the script
   element" is a switch ON, and what its "classic" arm sets §3.1.7's `currentScript` to for the whole of the run
   — the one fact about a `<script>` that nothing downstream of the scan can recover, because by the time the
   scheduler holds a body the element is behind it. NULL for a sequence no element produced (a host driving a
   synthesized program list); the row's `currentScript` is then null, which is §3.1.7's own answer for a
   document that is not executing a script element. */
void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                        lxb_dom_element_t **els, int n, int forking, const char *recipes);
int  engine_sched_step(void);

/* A SECOND DOCUMENT OF THIS AGENT RUNS ITS OWN SCRIPTS, ON THE FRONTIER THAT IS ALREADY RUNNING.
 *
 * An instance is an ORIGIN-KEYED AGENT CLUSTER, so several documents are one instance's — and a document the
 * HOST hands over (`qjs_join`) is not one any flow of this agent created. That is the whole difference from
 * §7.4's child navigable and it decides everything about this entry: a flow-created Document is built INSIDE
 * the creating flow, so its scripts are that flow's next programs (core/frame/navigable.c seeds them there);
 * a joined Document is built at the BASELINE like the root's, before any flow of it exists, so its scripts are
 * the programs of a flow that has to be MINTED for them. There is no flow to queue into, which is why this is
 * an entry of its own and not a second caller of engine_queue_script.
 *
 * IT IS A MEMBER OF THE ONE FRONTIER AND NOT A SECOND SESSION. §scheduler: a new document APPENDS its flows to
 * the one continuous frontier; it does not start a scheduler, a run or an attention. So this adds ONE flow —
 * the joined document's boot flow, an empty decision vector over the agent's baseline, ranked, preemptible,
 * forkable and parkable exactly like the root's — and returns. Nothing runs here.
 *
 * `cctx` IS THAT DOCUMENT'S REALM, which is WHERE ITS PROGRAMS ARE COMPILED: a program is closed over the
 * compiling realm's global, so a joined document's script compiled in the session's realm would define the
 * joined document's globals on the root's Window and read the root's back as its own. It is asserted to be the
 * realm `doc` answers with, because the queue carries the NAME and the compile resolves it.
 *
 * `bodies`/`srcs`/`types`/`n` are that document's §4.12.1 inventory (core/loader/document_scripts.h), in
 * document order, and they are COPIED rather than borrowed — unlike engine_sched_begin's, which is the
 * session's own sequence and lives as long as the session. A joined document's inventory is a fact the host
 * read once out of a tree it then hands to this agent, so it has no owner to outlive this call. */
/* `els[i]` is entry i's `script` ELEMENT — BORROWED, like every other reader of this column, because it names
   a node of the tree the host handed over and the rows die with the flow long before that tree does. */
void engine_join_document(JSContext *cctx, uint32_t doc, char **bodies, char **srcs,
                          const ScriptType *types, lxb_dom_element_t **els, int n);

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
   engine orders flows, so this is flow_weight of the flow the SCHEDULER WOULD PICK — flow_next_to_run with no
   incumbent — and nothing else. -inf when the frontier holds nothing the thread can be handed to, which is an
   EMPTY frontier and a fully host-owed one alike: neither can convert a slice into work, so neither may
   outrank an engine that can.
   IT IS THE RUNNABLE MEMBERS, AND THE TWO REASONS THIS FILE GAVE FOR ASKING flow_best INSTEAD ARE BOTH FALSE
   ABOUT THIS TREE. It said a flow's host-owed mark "is cleared at the top of every slice", so every member is
   askable again by construction at the instant the host reads this — that line is DELETED (engine.c's pick
   loop says so where it stood, and says what it cost: a fully blocked frontier re-admitted the ~59 flows one
   slice had time for and swapped COW deltas 1.76 million times without finishing one). Marks now live until
   the HOST does something that could have answered them, so a blocked member is still blocked here. And it
   said the Level-1 question is answered by the step code returning ENGINE_STEP_STALLED, "which is what moves
   the engine out of the pool's hot state" — nothing host-side moves it: main.c FOLDS STALLED into YIELD at the
   ABI (the bridge speaks two values), so a stall reaches the host as "call me again". With flow_best answering
   here, a document whose every flow was waiting on the host reported the weight of a flow that cannot run,
   burned no CPU so its weight never aged, and was therefore the one engine a weight-ordered eviction would
   NEVER choose — it sat in the hot pool at whatever rank its last emission had bought it, forever.
   -Infinity IS THE ANSWER AND NOT A SENTINEL BESIDE ONE: it is the value this engine already publishes for an
   empty frontier and the value extension/mojo.js declares the Level-1 input carries, so a stalled engine sorts
   last through the ordering that already exists rather than through a second question the host has to ask. */
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

/* THE DEATH OF A WORLD, ANNOUNCED — `world.gone<TAB><world name>`, one notice per name, and the ONE writer of
   that record. The names come from world_flow_gone (a flow left the frontier) or world_session_gone (the whole
   frontier parked); both are lists, because a world's death frees every ancestor whose last live descendant it
   was. It is a notice and not a routed delivery for the reason the create is one: nothing waits on it, so a
   flow that parks or is outranked never has to un-send it.
   BROADCAST BY THE TRUSTED ZONE. The sending engine does not track which peers a flow reached — that would be
   state kept only to avoid a no-op, and releasing a world with no segment IS a no-op (world.h) — so the record
   carries no target document and the zone hands it to every instance but this one. */
void        engine_notify_worlds_gone(JSContext *ctx, const char *const *names, int n);

/* …AND THE FOREIGN WORLDS A PARK IS CARRYING ACROSS THE TIER — `world.parked<TAB><world vector>`, one notice
 * per segment, announced at the PARK beside the death above and for the opposite reason. The death says "a
 * world of MINE has ended, drop your segment"; this says "a segment of YOURS lives in the residue I am about
 * to store, and it will be rebuilt by whichever instance resumes this document".
 *
 * WHY THE ZONE HAS TO BE TOLD, AND WHY IT CANNOT DERIVE IT. A `world.gone` is BROADCAST to the instances that
 * are LIVE — the sender does not track which peers a flow reached, because releasing a world with no segment
 * is a no-op (world.h). A parked instance is not live, so every death announced while this document is cold
 * misses it, and the instance that resumes rebuilds a segment for a world that no longer exists and holds it
 * for the rest of ITS process. That is the leak the death record exists to close, re-opened by the park — and
 * the only zone that can close it is the one that knows an instance is parked and which document it was.
 * This record is what makes the set of worlds it has to hold deaths for EXACT and finite: the ones the residue
 * actually carries, rather than every death for every cold document forever. */
void        engine_notify_worlds_parked(JSContext *ctx, const char *const *vectors, int n);

/* HAND BACK EVERY CROSS-AGENT OPERATION THIS INSTANCE WAS ASKED AND HAS NOT STARTED — `remoteop.retracted<TAB>
 * <token>`, one notice per distinct token, and the queues are EMPTY when it returns. Taken at the park, before
 * the residue is written, which is why the park's own assert can stay at full strength instead of being taught
 * to tolerate a queued entry.
 *
 * WHY IT IS RETURNED AND NOT PARKED, which is the whole question and it turns on ONE fact: A TOKEN'S LIFETIME
 * IS THE ZONE SESSION'S, and that is strictly SHORTER than the residue's. The token is minted by the trusted
 * zone (it is not the asking flow's request id, which is unique only inside the asking instance), it names an
 * entry in that zone's in-memory routing table, and it carries no generation — so a token written into a
 * recipe and answered in a later browser session names nothing, and the zone's own check ("a peer answered
 * under a rendezvous token this zone never minted") is what would fire. That is the identical defect the 'g'
 * record closes for a WorldId and remote_object.c REFUSES for an export id, one namespace over, and unlike
 * those two it cannot be fixed by adding a coordinate: the thing a token names is a suspended flow in another
 * instance's register, and that does not survive a session either.
 *
 * SO NOTHING HAS TO CROSS, IN EITHER DIRECTION OF THE PARK. If this instance comes back inside the same zone
 * session, the zone still holds the token, the record and the asker, and the asking flow is still suspended —
 * so it simply asks again. If the browser restarted, the ASKER is gone too, and its own recipe RE-ISSUES the
 * request and is answered with today's value — which is not a new claim, it is the one g_host_answers_late is
 * already built on ("the flow that asked is written down as a recipe, and the replay RE-ISSUES the request").
 * The asking side of this seam has always worked that way; this is the same rule applied to the answering side,
 * where the only difference is that the thing that parks is not the thing that asked, so the RE-ASK has to come
 * from the zone rather than from a replay.
 *
 * AND THE ZONE'S ACTION IS TO FORGET, NOT TO STORE. It suppresses re-asking a request it has already carried
 * (otherwise an operation would be performed once per step, each one a program with the page's side effects);
 * this record is what lifts that suppression. `engine_host_requests` deliberately does not dedupe, so the
 * asking flow's request is still being reported every step, and the next sighting asks again — routed, by the
 * zone, to whichever instance holds that document by then. */
void        engine_retract_operations(JSContext *ctx);

/* …AND THE INBOUND HALF OF IT: a peer says one of ITS worlds is gone, so the segment this instance holds for
   that world can go. The third record on the one-way line, beside a routed delivery and a performed operation,
   and the only one that seeds nothing — a death is not work, it is the end of some. */
void engine_world_gone(JSContext *ctx, const char *world);
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

/* WHAT THE HOST STILL OWES THE FRONTIER'S NETWORK PARKS — one `METHOD<TAB>URL` line per outstanding request,
 * newline-terminated, "" for none, DEDUPED BY THE PAIR.
 *
 * THE METHOD IS PART OF THE REQUEST'S IDENTITY, and this seam used to answer an ADDRESS ALONE. The register
 * has carried the method since the day it carried the whole request (PEND_METHOD), and it was dropped at
 * exactly these two edges: the join listed URLs and engine_provide filled every entry naming the URL. So a page
 * that issues a GET and a POST to one address had them collect each other's bodies — not a missing feature, a
 * WRONG ANSWER, and every @H example value, every branch that reads that body and every @S verdict on that path
 * was derived from a response the page never received. It is the same defect the XHR path was corrected for
 * (SECURITY.md §Network: "a wrong answer, which is worse than an absent one"), one seam over.
 *
 * WHY A TAB, AND WHY THAT IS NOT AN INVENTED DELIMITER. Neither field can contain one. A serialized URL cannot:
 * URL Standard §4.4 URL parsing removes all ASCII tab or newline from its input before anything else, so no
 * URL record can hold one and no serialization can produce one. A method cannot: Fetch §2.2.1 Methods says a
 * method "is a byte sequence that matches the method token production", and RFC 9110 §5.6.2 Tokens excludes
 * HTAB from tchar. The join ASSERTS both rather than trusting them, and it is the same shape
 * engine_host_requests already answers in (`id<TAB>op`) — one seam, one grammar.
 *
 * The buffer is this function's and is valid until the next call. */
const char *engine_pending_fetches(void);
/* ONE LINE, SPLIT WHERE IT WAS JOINED — because three hosts each deriving the pair is three places to get it
   wrong, which is the hand-copy 59d0e42d abolished. `line` is the host's own mutable copy of one line (no
   newline); the TAB is overwritten with a NUL and the two halves are handed back pointing into it. */
void engine_pending_split(char *line, const char **method, const char **url);
/* DELIVER A BODY FOR ONE REQUEST — keyed on `(method, url)`, which is what the flow parked on. Returns how many
   entries it filled; 0 with nothing matched is the host's pairing being off (or a sale — engine_take_paged_owed),
   and it is the CALLER that tells those apart because the caller owns the credit. */
int engine_provide(JSContext *ctx, const char *method, const char *url, JSValueConst value);

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
 * tables last, since they are read out of the result document long before any teardown runs. */
void solver_agent_free(JSContext *ctx);

#endif
