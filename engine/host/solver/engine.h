/* The DISPATCH LOOP — drains the WFQ frontier. Each flow is the page's scripts run as ONE preemptible program
 * (JS_FlowNew), replaying the flow's decision vector; the first flow is the empty vector. A concolic branch
 * inside a run forks a sibling flow (decide.c); the loop keeps running the highest-value flow until the
 * frontier is empty. There is NO separate boot executor — the scripts ARE the first flow.
 *
 * A @S candidate re-fire is a FLOW seeded onto this same frontier (solve_seed_candidates), not a separate
 * executor: one scheduler runs exploration and verification alike. */
#ifndef ENGINE_HOST_SOLVER_ENGINE_H
#define ENGINE_HOST_SOLVER_ENGINE_H

#include <stddef.h>   /* size_t — every program crosses this header as (text, LENGTH); see engine_queue_fetched_script */
#include <stdint.h>   /* int64_t — EngineStepUnitRuns' `step_us` is a MICROSECOND accumulator and its width is
                         load-bearing rather than incidental; the paragraph at that field says why, and this
                         include is what stops the width from depending on whichever header happened to be
                         pulled in ahead of this one. */

#include <lexbor/dom/dom.h>

#include "core/fetch/fetch.h"
#include "core/loader/script_type.h"   /* which of §8.1.4.4's two algorithms runs entry i — see `types` below */
#include "core/timing/task_source.h"   /* HTML §8.1.7.1's `source` field — which task source, if any, queued a row */
#include "solver/step_unit.h"          /* the arms of flow_step — EngineStepUnitRuns is one count per arm */
#include "quickjs.h"

/* Run the page's scripts as one code flow: each script `bodies[i]` is its OWN program (JS_FlowNew — faithful
   per-<script> scope, NEVER concatenated), run in document order, sharing globals + the flow's COW delta. */

/* WHERE IN THE FLOW'S OWN PROGRAM SEQUENCE A QUEUED PROGRAM LANDS. It is a SPEC fact about the operation that
 * caused the program, not a scheduling preference, which is why the CALLER states it and the queue never
 * guesses — the same sentence engine_queue_into already makes about which flow and which document.
 *   DYN_POS_APPEND IS THE TAIL, AND EVERY TASK TAKES IT — but not everything that takes it is a task, which
 * is the half this said the other way round. A `javascript:` navigation (HTML §7.4.2.2 "Beginning navigation"
 * queues a global task on the navigation and traversal task source to reach §7.4.2.3.2) and a lazy chunk's
 * reply ARE tasks and a task queue is FIFO, so the tail is where they go; a document's own sequence being
 * filled one entry at a time is not a task at all and takes the tail because §4.12.1 fixes its order. Which of
 * the two a row is, is the row's TaskSource and is stated at each entry below.
 *   §8.7 Timers's STRING HANDLER USED TO BE IN THIS LIST AND IS NOT A ROW ANY MORE. §8.7 creates and runs it
 * inside step 9's task, so core/timing/timer.c runs the program on the firing flow's own trampoline and queues
 * nothing here; the entry that took it is gone with it, and the paragraph standing where that entry did says
 * why.
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
   Because a load sits behind a branch, the ONE BFS discovers different lazy scripts on different arms — lazy
   loading is not a separate system, just more code the flow runs and forks through. The body is copied; the
   queue is per-run and drained by the flow that owns it.
   `doc` NAMES WHICH DOCUMENT'S PROGRAM IT IS, which is WHERE IT IS COMPILED (solver/flow.h's `dyn_doc`). An
   instance is an ORIGIN-KEYED AGENT CLUSTER, so this is a child navigable's document as often as the
   session's, and a program compiled in the wrong realm is closed over the wrong Window — it defines the
   child's globals on its creator and reads the creator's back as the child's. There is no default: the caller
   knows which document's code it is holding, and a scheduler that guessed would guess the same way for every
   document this agent has.
   IT IS A TASK, so it is DYN_POS_APPEND and cannot be asked for anything else. The one script source that is
   NOT a task has its own entry further below.
   AND IT IS A CLASSIC SCRIPT, which is HTML §8.1.4.4 "Calling scripts"'s answer for a program with no
   `<script>` element behind it rather than a default this entry picks: a lazy chunk's reply is the body an
   already-running program asked for. A row that DOES have an element behind it states which of §8.1.4.4's two
   algorithms runs it.

   IT IS ITS OWN ENTRY BECAUSE HTML §8.1.4.1 "Scripts"'s BASE URL IS ITS OWN ANSWER, and an entry that could
   not express the difference answered it once for two callers. That field's own definition is the whole rule:
   "Null or a base URL used for resolving module specifiers. When non-null, this will either be the URL from
   which the script was obtained, for external scripts, or the document base URL of the containing document,
   for inline scripts." So NULL in the row's address column is not an absent value — it is the positive
   statement "the document's", read at the compile — and a program whose bytes came from a RESPONSE has an
   address that no consumer can re-derive from the document it ran in. Merged, the fetched case took the inline
   answer: a chunk served from /chunk/x.js compiled under the DOCUMENT's address, so the throw site §8.1.4.6
   "Runtime script errors" reports named the page rather than the chunk, a nested `import('./y.js')` resolved
   against the page, and — because the compile reads a missing address as `JS_EVAL_FLAG_INLINE_SCRIPT` — every
   record the chunk published was filed as state the server rendered against THIS visitor's credentials, which
   is the opposite of what a subresource served identically to everybody is. */
/* HTML §8.7 "Timers"'s STRING HANDLER HAS NO ENTRY HERE, AND THAT IS THE ANSWER RATHER THAN A GAP. §8.7 puts
   the create and the run at the EXPIRY, inside step 9's task (substeps 9.8.7-9.8.8), so the program is
   compiled with JS_EVAL_FLAG_TRAMP_CLOSURE and run by that task's own step machine on the firing flow's
   trampoline chain — core/timing/timer.c. Queued as a row here instead, at the SET, it put the TIMER TASK
   SOURCE in BOTH of a flow's queues at once (its Function arm's task reaches `jobs` through
   JS_EnqueueCallTask), which HTML §8.1.7.1 "Definitions" forbids: "For each event loop, every task source must
   be associated with a specific task queue." It also lost the handler's LENGTH — the entry's shape was a bare
   `const char *`, so `setTimeout("\0…")` was read to the first NUL — which the conversion in timer.c now
   carries end to end. */
/* …AND A PROGRAM WHOSE BYTES CAME FROM A RESPONSE — a lazy chunk, a body an already-running program asked for.
   `url` IS §8.1.4.1's base URL and is REQUIRED, which is the half the merged entry could not state. §8.1.4.2
   "Fetching scripts" creates the script with the response's URL, so a fetched program always has one: an entry
   reached with none is a caller that has the bytes and threw away where they came from, which is unrecoverable
   here (the document's address is a different script's answer, not a weaker form of this one).
   `body_n` IS THE PROGRAM'S LENGTH for ECMAScript §11.1's reason above — a decoded response body may hold a
   U+0000 and a browser runs the whole of it.
   ITS SOURCE IS §8.1.7.4 "Generic task sources"' NETWORKING TASK SOURCE — "This task source is used for
   features that trigger in response to network activity", and this row exists because a response arrived.
   Stated at the definition rather than taken as a parameter, for the reason the type below is: this entry is
   one spec step, and a caller that reached it has a response in hand. */
void engine_queue_fetched_script(uint32_t doc, const char *body, size_t body_n, const char *url);
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
   only for an inline CLASSIC script — the entry below.
   AND ITS SOURCE IS NONE, WHICH IS A STATEMENT AND NOT A GAP. Nothing queues a task to run an element's
   inline program: the parse that reached the element runs it, and §8.1.7.1 "Definitions" describes a parse as
   work a task DOES rather than as a task apiece. The tail is where it goes because §4.12.1 fixed its order
   against the scripts written around it, not because a task queue is FIFO. */
/* `el` IS THAT ELEMENT, and the row carries it for the same reason it carries the type: "execute the script
   element" is a switch on EL, and its "classic" arm sets that document's §3.1.7 `currentScript` to it for the
   whole of the run. The run is a WORK ITEM here — it starts in one scheduler step and completes in another —
   so nothing at the completion could re-derive which element it was, and a C save/restore bracket around the
   compile would set the slot for whichever flow was running when the NEXT program started. See solver/flow.h's
   `dyn_el`. */
/* `body_n` IS THE PROGRAM'S LENGTH and is not `strlen(body)`: an element page code INSERTED carries whatever
   was assigned to its `.textContent`, which never went through HTML §13.2.5.4 "Script data state" (the state
   that turns a U+0000 into a U+FFFD), so its text may hold a NUL that a parsed document's inline script
   provably cannot. The DOM already answers the length — DOM §4.11 "Interface Text"'s child text content fills
   one (core/dom/text_content.h) — and dropping it was how an injected chunk ran as a prefix of itself. */
void engine_queue_element_script(uint32_t doc, const char *body, size_t body_n, ScriptType stype,
                                 lxb_dom_element_t *el);
/* …AND THE ONE THAT IS NOT. HTML §4.12.1.1 "Processing model": an inline classic script whose element a page
   INSERTED reaches the end of "prepare the script element" — "Otherwise, immediately execute the script
   element el, even if other scripts are already executing" — and "execute the script element" then runs the
   classic script right there. So it takes the slot AFTER the program that inserted it rather than the tail,
   and nothing the sequence already holds may run in between. Separate from the two entries above rather than a
   flag on it, because the two are different spec steps and the four callers of that one must not be able to
   pick this by accident.
   THE TYPE IS CLASSIC AND IS NOT A PARAMETER: §4.12.1.1 reaches this step only for what falls past "If el's
   type is `classic` and el has a src attribute, or el's type is `module`", so every module — inline or not —
   has already gone to one of the three lists by then. An inline module has a graph to LOAD before its result
   exists, which is why the standard does not run it in place. */
/* `el` IS THE ELEMENT THE PAGE INSERTED, and `body_n` ITS PROGRAM'S LENGTH — both for the reasons
   engine_queue_element_script states, and the length for that entry's exactly: this element's text is a page's
   own string, not a tokenizer's output. */
/* ITS SOURCE IS NONE AND THIS IS THE ENTRY THE WORD WAS COINED FOR: "immediately execute the script element"
   runs inside the algorithm that prepared it, so there is no task and nothing to give a source to. A row
   here that named one would be a task asked to interpose ahead of tasks already queued, which is why the
   queueing point asserts the pair rather than either half. */
void engine_queue_script_immediate(uint32_t doc, const char *body, size_t body_n, lxb_dom_element_t *el);
/* THE SAME POSITION IN THE SAME SEQUENCE, FOR A SCRIPT WHOSE SOURCE IS AN ADDRESS. §4.12.1 fixes an external
   script's position against the scripts written around it — a `pending parsing-blocking script` blocks the
   tokenizer (§13.2.6.4.8), and the `list of scripts that will execute when the document has finished parsing`
   runs IN ORDER (§13.2.7) — so the entry occupies that position with only its URL, the flow WAITS there, and the
   host's reply becomes the program in the slot. Without it a document's external scripts could only park on
   their replies and run in ARRIVAL order, which is why an inline script after a `<script src>` used to abort.
   `url` is already resolved: §8.1.3.2 "Environment settings objects"' API base URL belongs to the document
   whose element it is (§4.4 stood here and is "Grouping content"), so only the
   caller can resolve it. The ASAP SET does not come here — it has no position, so it parks with
   engine_pending_script_url and runs when its reply drains.
   `stype` IS THE ELEMENT'S, and it survives the reply: §8.1.4.2 "Fetching scripts" decodes a module's bytes as
   UTF-8 whatever the response says and a classic script's through the response's charset label, and §8.1.4.4
   then runs the source with the matching one of its two algorithms. The row keeps its ADDRESS across that
   replacement too — §8.1.4.2 creates the script with the RESPONSE'S URL, which is the base a nested
   `import('./chunk.js')` resolves against and, for a module, the module map KEY. */
/* `el` IS THE ELEMENT WHOSE `src` THIS IS — see engine_queue_element_script. It survives the reply exactly as
   the type and the address do: the row is the element's program whether its bytes have arrived or not. */
/* ITS SOURCE IS NONE, AND THE REASON IS THAT THIS ROW IS A POSITION RATHER THAN A TASK. §4.12.1 fixes where an
   external script runs among the scripts written around it, and this entry is that place being held; the
   NETWORKING task the response eventually queues is a different work item, and the register it lands on is
   what serves it (engine_pending_docscript, and flow_deliver_one_reply's arm above the sequence). Calling the
   held slot a networking task would put one source on two carriers by naming, which is the thing this value
   exists to make visible. */
void engine_queue_docscript_url(uint32_t doc, const char *url, ScriptType stype, lxb_dom_element_t *el);
/* An @S CANDIDATE, queued as the program it would be if it fired. Same queue, one difference: it is ALLOWED not
   to compile, because most breakouts do not fit most sink contexts and a candidate that does not parse simply
   never fires. A page script that does not compile still asserts.
   `pos` IS THE SINK'S OWN SEMANTICS AND NOT THE SOLVER'S PREFERENCE, which is the whole reason it is a
   parameter here. An eval sink IS ECMAScript §19.2.1.1 PerformEval, so its code runs inside the call
   expression — IMMEDIATE. A markup sink's auto-firing `onerror`/`onload` and a URL sink's `javascript:`
   navigation are TASKS, so they take the tail like every other task — APPEND. §@S's "the firing vector is
   chosen per sink from its real semantics" is the same sentence about the same table. */
/* `body_n` IS THE CANDIDATE'S LENGTH, and here the pair is load-bearing for the SOLVER rather than for
   fidelity: a candidate is constructed out of attacker-shaped bytes (a `%00` percent-decoded from a hash, a
   U+0000 a JSON reply carried), so reading it to its first NUL fires a program the search did not choose and the
   "no hit" that follows is a verdict about a payload nobody built. */
/* AND ITS SOURCE IS THE SOLVER'S OWN, WHICH IS NOT ONE OF §8.1.7.1's AND MUST NOT BE MADE TO LOOK LIKE ONE.
   No algorithm of the standard queued this program: the solver did, to see whether a constructed input reaches
   a sink. Giving it a task source would order it against the page's real tasks by a fact nobody observed —
   §@H's own line between a value the code determined and a value invented to satisfy a gate, one layer up. The
   position is what the sink genuinely decides and it is already `pos`. */
void engine_queue_candidate(const char *body, size_t body_n, DynPos pos);
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
/*   `body_n` is what STEP 3 produced. "Let scriptSource be the UTF-8 decoding of the percent-decoding of
   encodedScriptSource" — URL §1.3 "Percent-encoded bytes"'s percent-decode reaches all 256 byte values, so
   `javascript:a=%00` is a source text with a U+0000 in it and ECMAScript §11.1 "Source Text" permits one. */
/* ITS SOURCE IS §8.1.7.4's NAVIGATION AND TRAVERSAL TASK SOURCE, WHICH §7.4.2.2 "Beginning navigation" STATES
   OUTRIGHT. Its step 21 is "Queue a global task on the navigation and traversal task source given navigable's
   active window to navigate to a javascript: URL"; this row is that task's program, so the task source is the
   row's and the tail is where a task goes.
   AND THAT SOURCE IS ON TWO OF A FLOW'S CARRIERS, WHICH IS THE ONE THING THE DECLARATION IS HERE TO SHOW. The
   document-load job of the same section reaches `jobs` through JS_EnqueueCallTask (core/frame/navigable.c),
   this row reaches `dyn`, and §8.1.7.1 "Definitions" requires one queue per source precisely so that "the user
   agent would never process events from any one task source out of order". This is the shape §8.7 Timers's
   string handler had before its create and its run moved to the expiry, arriving a second time through a
   different section — so the enumeration is what had to exist first, and it now does: a source reaches a
   carrier iff a producer on that carrier names it, and a grep for this enumerator is the whole answer for
   `dyn`.
   WHAT THE NEXT DIFF BUILDS is the same declaration on the OTHER carrier: a TaskSource carried by
   JS_EnqueueCallTask to the host's job-enqueue hook and recorded on the job, exactly as that hook's `is_task`
   already travels (quickjs.h declares both), so the split is an ASSERT at the two queueing points rather than
   this paragraph, and so the repair — whichever carrier the source ends up on alone — has something that
   fails while it is half done.
   HOW ITS ABSENCE SHOWS, and it needs no assert to be seen: a flow that has queued a document load and a
   `javascript:` navigation in one turn runs them in an order fixed by which arm of flow_step stands above the
   other, so writing the two statements the other way round does not put the two effects the other way round.
   That is the pair of orderings no arrangement of two arms can both serve, and it is observable from page
   code with nothing but an `<iframe>` and a `location` write. */
void engine_queue_javascript_url(uint32_t doc, const char *body, size_t body_n);
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
/* HTML §4.6.8.20 Link type "preload"'s browsing-context-connected time for the elements a PARSE produced
   (html_link.h). Registered by the link component for the reason the two above are registered rather than
   named. Asked AHEAD of everything else a flow could do, which is a spec position and not a preference: a
   browser connects those elements during tree construction, so their requests precede every script of that
   document. It exists at all because §4.2.4.3 "Fetching and processing a resource from a link element" ends in
   a fetch with no task and no microtask in front of it, and a fetch parks on a FLOW — so the walk that finds
   the elements (which for a session's own document runs at the pre-boot baseline) can only inventory them. */
void engine_set_link_connected_hook(int (*fn)(JSContext *ctx));
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

/* WHERE A SIBLING COMES BACK — the one thing a prepared fork needs that the decision seam cannot derive, so
 * this is where it is decided and where a fork that has no answer CRASHES.
 *
 * decide.c calls this with the sibling's decision + pin blobs already built. Two things can consume them, and
 * exactly one of them will:
 *   - AN ACTIVATION THAT WILL BE CLONED. The interpreter asking at an OP_if, or the step driver asking for a
 *     machine that yielded JS_STEP_FORK, both hold a resume point and clone it a moment later. They ask
 *     through the flow-control hooks, which is how this knows: engine.c installs its own wrappers and they
 *     declare it. The blobs are stashed and engine_fork_finalize assembles from them plus the clone. Returns 1
 *     — the caller still owes the snapshot.
 *   - THE FLOW'S OWN SCHEDULER STEP, when `restartable` says the asking code is re-reached by re-running it
 *     (solver_decide_restartable). There is no activation, and none is needed: the sibling is assembled here
 *     with NO frame, and re-entering its step re-runs the same engine code and replays the arm recorded for
 *     it. Returns 0 — nothing is owed and the FORKED bit is never raised.
 * A C body that is neither — already inside its own activation with nothing that will clone it — has no
 * resume point at all, and this DFAILs naming the predicate it asked. What that names is the declaration to
 * build (JS_CFUNC_STEP_DEF), never a way to ask less. `asked` is the constraint key, or NULL.
 *
 * IT USED TO STASH UNCONDITIONALLY, and the cost of that was a diagnosis three layers from the cause: a C body
 * took the FORKED bit, nothing consumed it, and the NEXT fork anywhere in the agent aborted on a stash that
 * was still full — 20 documents of one WPT area, with nothing in the message about where the blobs came from. */
int engine_prepare_fork(JSContext *ctx, void *dec_blob, void *pin_blob, const char *asked, int restartable);

/* DOES THIS SESSION FORK AT ALL — the explore/verify bit, asked rather than copied.
 *
 * The bit reaches the INTERPRETER and the STEP DRIVER through the flow-control hook table, and each of those
 * two already has its own answer for a session that installs none: the interpreter's `branch` is absent, so
 * the arm is -1 and the ordinary ToBool decides; the step driver's `outcome` is absent, so the machine takes
 * outcome 0, which every step machine numbers as its ordinary completion. A caller that asks the decision seam
 * BY SYMBOL — a browser component with no OP_if and no machine — consults neither table, so decide.c asks this
 * and answers with the arm THAT SITE declared (solver/decide.h's `nonforking`).
 *
 * IT IS ASKED AND NOT PASSED IN, because a second copy of the bit is a second thing that can disagree with the
 * hook table, and the disagreement would be invisible: a session whose hooks say verify and whose copy says
 * explore mints frontier members from inside a verification, which is the exact defect engine_prepare_fork's
 * own assert names. There is one writer, at the one point a session declares its policy. */
int engine_session_forks(void);

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
   frontier is empty and the session's hooks are uninstalled.
   THERE ARE THREE AND EVERY HOST CARRIES THREE. The shipped ABI (main.c's qjs_step) used to FOLD the stall
   into the yield, and this file said elsewhere that it did; that fold is deleted, because it is the one a
   host cannot undo. A YIELD and a STALL ask for opposite things and only the engine knows which it meant. */
#define ENGINE_STEP_DONE   0
#define ENGINE_STEP_YIELD  2   /* the value the extension bridge's qjs_step already speaks */
/* STALLED: every flow has run as far as it can, but the frontier is not exhausted — one or more are parked on
   something only the HOST can supply (a reply the sandbox cannot fetch). The session stays LIVE and every
   parked flow keeps its snapshot; the host supplies what is owed and steps again. Without this the scheduler
   closes the session on an empty run-queue and those flows are never resumed, which is how a page whose config
   gates its later endpoints loses everything after the first request.
   IT IS A BILL, WHICH IS WHY IT MAY NOT BE ANSWERED BY STEPPING AGAIN. A yield asks to be OUTRANKED and costs
   nothing to ignore — step back in and the same top flow runs on. A stall asks to be PAID, and a host that
   answers it with another step converts nothing into work: measured on the two-instance drive, 10.8 million
   steps against a peer owed one reply, with zero context switches, zero jobs and no emission, draining on the
   very next step once the reply was supplied. So a driver's loop needs this as a TERMINATOR (a peer that
   stalls on a payment the driver will not make has said all it is going to say) and never a step count — the
   engine states the condition, so nothing has to count rounds to guess at it. */
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

/* THE SESSION ENDS WHEN THE HOST STOPS STEPPING, AND EVERY HOST SAYS SO THE SAME WAY. `begin`/`step`/`end`,
 * with `end` called unconditionally at the point the host leaves its loop — never `if (r != ENGINE_STEP_DONE)`,
 * which is a condition each host has to copy correctly and which one of them will not.
 *
 * WHAT IT IS FOR. Only ONE way out of a stepping loop closes the session by itself: DONE, where the frontier
 * drained. Every other exit is the host's own decision — a stall nobody will pay, a measurement that is over —
 * and it leaves the session LIVE: the scheduler's hooks installed over a frontier the host is about to tear
 * down, and, the part that is not merely untidy, ONE FLOW STILL SWITCHED IN. That flow's heap delta is applied
 * to the shared baseline, its created DOM nodes are in the document, and its decision state is in decide.c's
 * globals rather than in its own blob — so the teardown releases one copy of each while the scheduler still
 * holds the other, and flow_release asserts exactly that ("the RUNNING flow was released"). Measured: a wpt
 * runner that ended its loop when testharness reported the file complete aborted 26 files on that assert.
 *
 * IT IS NOT A DROP, WHICH IS THE WHOLE REASON THIS IS A MECHANISM AND NOT A `free`. Ending performs the
 * ORDINARY SUSPEND on the running flow — the same switch-out a park takes — so the frontier a closed session
 * leaves behind is a set of SNAPSHOTS, every member in the state §Time-travel says a parked flow is in.
 * Nothing is dropped, starved, skipped, reordered or forgotten; what ends is the SESSION, and whether those
 * snapshots are then resumed is a question about the host's frontier and not about this call.
 *
 * CALLING IT AFTER DONE IS CORRECT AND IS NOTHING. A session that already closed has no running flow to
 * suspend and no hooks left installed, which is a positive answer rather than a guard against a caller that
 * got it wrong — and it is what lets the rule above be "call it when you stop stepping", with no condition. */
void engine_sched_end(void);

/* A SECOND DOCUMENT OF THIS AGENT RUNS ITS OWN SCRIPTS, ON THE FRONTIER THAT IS ALREADY RUNNING.
 *
 * An instance is an ORIGIN-KEYED AGENT CLUSTER, so several documents are one instance's — and a document the
 * HOST hands over (`qjs_join`) is not one any flow of this agent created. That is the whole difference from
 * §7.3.1.3 "Child navigables"' child navigable (§7.4 stood here and is "Navigation and session history",
 * which navigates one rather than creating it) and it decides everything about this entry: a flow-created
 * Document is built INSIDE
 * the creating flow, so its scripts are that flow's next programs (core/frame/navigable.c seeds them there);
 * a joined Document is built at the BASELINE like the root's, before any flow of it exists, so its scripts are
 * the programs of a flow that has to be MINTED for them. There is no flow to queue into, which is why this is
 * an entry of its own and not a second caller of engine_queue_fetched_script.
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
   the engine out of the pool's hot state" — and it is not, even now that main.c carries the code rather than
   folding it into a yield. The code says PAY ME; it does not say what this engine is worth against another
   one, and the pool's order is a comparison of weights and nothing else. Reading a rank out of a step code
   would be two answers to one question, which is the defect this whole paragraph is about. With flow_best
   answering here, a document whose every flow was waiting on the host reported the weight of a flow that
   cannot run, burned no CPU so its weight never aged, and was therefore the one engine a weight-ordered
   eviction would
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

/* WITHDRAW THE RENDEZVOUS — Fetch §2 Infrastructure's "To terminate a fetch controller controller, set
 * controller's state to 'terminated'", which is the THIRD thing that can happen to an outstanding id and the
 * only one this seam did not have. It could be waited on (engine_host_answered) or taken (engine_host_take);
 * it could not be given back, and the two callers that must give one back are named in the spec: XHR §3.5.1
 * "The open() method" step 10 ("Terminate this's fetch controller. A fetch can be ongoing at this point.") and
 * XHR §3.2 "Garbage collection" ("If an XMLHttpRequest object is garbage collected while its connection is
 * still open, the user agent must terminate the XMLHttpRequest object's fetch controller").
 *
 * WHAT IT IS FOR IS A FLOW THAT WOULD OTHERWISE BE BLOCKED BY NOBODY. An unanswered synchronous request is what
 * makes a flow blocked (pending_blocked, and engine_host_request asserts it at the ask), and the mark comes off
 * only at a HOST EVENT — so when the machine standing at the call site is DESTROYED, its entry stays on the
 * register with no reader and the flow is never picked again. That is not a slow flow, it is a flow removed
 * from the frontier with nothing anywhere to say so, and §scheduler's razor calls a resume that forgets a flow
 * a CAP. This is the host event that ends it: the entry leaves and flow_clear_host_owed runs, so the flow is
 * askable again on the very next pick, with its snapshot and its remaining work untouched.
 *
 * IT IS `terminate` AND NOT `abort`, WHICH FETCH MAKES TWO OPERATIONS AND NOT TWO SPELLINGS. §2 Infrastructure
 * defines both over the same struct: "To abort a fetch controller controller with an optional error" sets the
 * state to "aborted" and records an "AbortError" DOMException as the serialized abort reason, while terminate
 * sets the state to "terminated" and carries NO error at all. A fetch params is "canceled" under either. The
 * difference is whether an ERROR IS DELIVERED SOMEWHERE, and at a terminate there is by construction nowhere
 * to deliver one — §3.2's XMLHttpRequest object has been collected and §3.5.1's has been re-`open`ed, so the
 * continuation that would have caught it no longer exists. Delivering an "AbortError" here anyway would be a
 * throw raised at a call site nobody is standing at, and a value freed by the unwind is the drop this entry
 * exists to prevent wearing the costume of preventing it.
 *
 * NAMED RESIDUAL — this is CORRECT for terminate and NARROWER than §2 Infrastructure's pair.
 *   NOT COVERED: "abort a fetch controller", whose error IS delivered — XHR §3.5.7 "The abort() method" step 1,
 *     and Fetch §5.6 "Fetch methods"' "To abort a fetch() call … Reject promise with error".
 *   WHAT THE NEXT DIFF BUILDS: `engine_host_abort(ctx, req, reason)` beside this, answering the rendezvous with
 *     ENGINE_COMPLETION_THROW so engine_host_take_completion re-raises the reason at the parked call site —
 *     which needs the id to be reachable from the object the page aborts (XHR's controller lives in the
 *     lifecycle machine's step state, and §3.5.7 is a different machine), and needs `fetch()` to carry a
 *     `signal` at all.
 *   HOW ITS ABSENCE WOULD SHOW: a page that calls `xhr.abort()` mid-flight fires §3.5.7's own abort/loadend
 *     and then fires them A SECOND TIME when the reply the zone was never told to stop finally lands and the
 *     parked machine reaches §3.5.6's "handle errors" with the aborted flag set.
 *
 * THE ZONE IS ASKED, NEVER TOLD BY THE REGISTER. §3.5.7 says abort() "Cancels any network activity", and the
 * network is the trusted zone's alone (SECURITY.md: all of it through the one chokepoint) — so stopping the
 * transfer is a REQUEST that leaves on the one-way notice line as `hostreq.terminate<TAB><id>`, exactly as
 * `remoteop.retracted` hands back a cross-agent question. The engine decides nothing about it and does not
 * wait for it: withdrawal of the ENGINE'S half is complete when the entry is gone.
 *
 * A LATE ANSWER TO A WITHDRAWN ID IS ALREADY HANDLED and is not this function's problem to prevent: a value the
 * trusted zone computed names no register once the entry has left, which engine_host_answer's ENGINE_ANSWER_HOST
 * arm returns 0 for — "nobody is waiting" — so a transfer already in flight when the notice was written lands
 * harmlessly. That is why the notice is an optimisation of the NETWORK and never of the register.
 *
 * IT ANSWERS NOTHING, AND THAT IS A STATEMENT RATHER THAN AN OMISSION. The one fact it could hand back —
 * whether a register still named the id — is not one any caller can act on: the flow that asked may have
 * finished or been freed with its document (HTML §7.5.10 "Destroying documents"), and from a call site that is
 * itself a teardown that is indistinguishable from a machine holding an id its own take had already spent. A
 * value computed for nobody to read is the mirror of the field nobody writes, so the boolean arrives on the day
 * a caller can assert on it — §3.5.1's step 10 is that caller, because a re-`open`ed object's flow is alive by
 * construction — and not before. The withdrawal is COUNTED instead (EngineFrontierCensus's host_terminated),
 * which is the reading a session genuinely has a use for. */
void     engine_host_terminate(JSContext *ctx, uint32_t req);

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
   the asking flow is gone, which is not an error: nobody is waiting.
   `world` NAMES THE TIMELINE THAT COMPUTED THE ANSWER, in world_serialize's grammar, and it is required of a
   PEER answer and forbidden of a HOST one. That asymmetry is the whole of `source` made checkable: a peer's
   document state is its flows, so one question has N true answers, and two of them are told apart from ONE
   answer relayed twice only by the flow that produced each. Without it a duplicate relay is indistinguishable
   from another timeline — which is not hypothetical: the harness zone kept one answer per token in a one-slot
   map, and a page reading `w.closed` twice in one expression was answered out of two contradictory timelines
   of one document with nothing able to say so. NULL for ENGINE_ANSWER_HOST, which has no flow to name. */
int      engine_host_answer(JSContext *ctx, uint32_t req, const char *world, JSValueConst value, int completion,
                            int source);
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

/* A TAB-DELIMITED RECORD BUILT FROM ITS FIELDS AND SIZED EXACTLY — the way a notice with more than one field
   is assembled, because the alternative is a hand-written format string beside a hand-computed slack constant,
   and those two drift INDEPENDENTLY. The caller frees.
   MEASURED, AND THE REASON THIS EXISTS: a field added to `navigable.create` was added to the record's strlen
   sum and NOT to the constant that pays for the TAB in front of it, so the sum was one byte short and snprintf
   TRUNCATED — silently, because truncation is what snprintf is for. The dropped byte was the record's last,
   and the record's last field is the raw CSP header, which was EMPTY in the fixture that caught it: the final
   TAB went, seventeen fields arrived as sixteen, and every reader that counts them refused the record. WITH A
   NON-EMPTY POLICY THE SAME ONE BYTE COMES OFF THE END OF THE POLICY TEXT — a Content-Security-Policy crossing
   to a peer instance one character shorter than the one the server sent, which nothing counts and no reader
   can see, and which is a security decision made on bytes no document ever stated.
   THE COUNT IS THE CALLER'S `sizeof` AND NEVER A TERMINATOR — CLAUDE.md §UB-DOES-NOT-ONLY-CRASH: a
   NULL-terminated variadic field list is a contract the optimiser is entitled to assume every caller keeps,
   and the one it miscompiles into a two-byte self-jump for the caller that forgets. `sizeof a / sizeof a[0]`
   at the call site is derived from the array the fields are written into, so a field added to that array is
   counted, paid for and delimited by the same edit that adds it, and there is nothing left to keep in sync.
   THE LAST FIELD IS THE REMAINDER and is the only one that may contain HTAB — a raw CSP header may hold one
   (RFC 9110 §5.5 "Field Values" admits HTAB inside a field value), which is why the policy is last on every
   record that carries one. Every field BEFORE it is checked for one HERE rather than at each call site: a
   middle field carrying a tab is silently a record with an EXTRA field, which shifts every field after it, and
   a shift is the one corruption a reader's field COUNT still passes. */
char       *engine_notice_build(const char *op, const char *const *fields, size_t nfields);

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

/* HAND BACK EVERY CROSS-AGENT OPERATION THIS INSTANCE WAS ASKED — `remoteop.retracted<TAB><token>`, one notice
 * per distinct token, and no member owes an answer when it returns. Taken at the park, before the residue is
 * written, which is why the park's own asserts can stay at full strength instead of being taught to tolerate a
 * question the residue cannot carry.
 *
 * BOTH HALVES OF THE DEBT, and the STARTED one is not a different kind. A question is queued on the arrival
 * slot until a flow performs it and then rides its program's row as a token; this returns it from wherever it
 * is. The half-run program is not a loss to weigh: its partial work is the parked flow's own COW delta and
 * leaves with the flow, exactly as every other suspended program's does, and what the peer is owed is a CALL
 * rather than a value — HTML §7.2.1.3.5 "CrossOriginGet ( O, P, Receiver )" ends "Return ? Call(getter,
 * Receiver)", so a call abandoned before it completes has been made zero times and the re-ask makes it once.
 *
 * ONE NOTICE PER QUESTION, NOT PER HOLDER. An operation is attached to EVERY live timeline (engine_perform),
 * so a notice per flow would be one hand-back repeated thousands of times — and worse than noisy: a notice
 * sent while another timeline still holds the operation tells the zone to forget a token that timeline is
 * about to answer under. The notice therefore belongs to the LAST holder leaving, which is why the single-flow
 * form of this exists at all (the pager sells one member while the rest keep answering).
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

/* WHAT THE HAND-BACK DID, counted AT THE RETRACTION so it cannot disagree with the notices that left.
 * `flows` is how many members held a question at all; `started` is how many PROGRAM ROWS were returned — the
 * half a park used to refuse, and the one number that distinguishes a run which exercised it from a run which
 * only met the queued half; `handed_back` is how many notices left, which is the number of distinct QUESTIONS
 * and not of holders. `handed_back` far below `flows` is the last-holder rule working; `handed_back` equal to
 * `flows` on a forked frontier is that rule not being applied. */
void        engine_retract_census(long *flows, long *started, long *handed_back);

/* HOW MANY OF THIS FRONTIER'S MEMBERS ARE MID-ANSWER — program rows still carrying a peer's rendezvous token.
 * It is the state a park used to refuse, asked of the frontier while it is live; the same number after the fact
 * is engine_retract_census's `started`. A live 0 is a positive statement: every question this instance was
 * asked is either still queued on the arrival slots or already answered.
 * AND WHICH HOSTS CAN ACT ON A NON-ZERO ANSWER IS PART OF THE QUESTION, which the sentence here used to leave
 * out: it said a host "CHOOSING a moment to evict at can choose one that contains it rather than hope one
 * does", and that is false for a host whose slices can only end at the stall or at exhaustion. flow_perform
 * appends the operation's program to a RUNNABLE row, so a member mid-answer is in neither of those exits and
 * the started state never survives a slice — a single-document host asking this between two slices reads 0
 * however it picks its moment, and no ask timing changes that. The exits that CAN end a slice over a started
 * operation are the CPU quantum and the level-1 yield floor, so this number is actionable for a host that
 * orders several engines and hopeful for one that does not. */
long        engine_operations_started(void);

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

/* THE THIRD INBOUND STATEMENT, AND THE ONE THE BROWSER MAKES RATHER THAN A PEER: the Document named by `doc` is
   no longer the active document of its navigable, because the REAL BROWSER navigated that navigable.
   IT IS HTML §7.4.6.1 "Updating the traversable"'s DEACTIVATE A DOCUMENT FOR A CROSS-DOCUMENT NAVIGATION, and
   NOT §7.3.1.6 "Navigable destruction" — a navigation destroys no navigable, it replaces the Document active
   in one. The two meet one step down at §7.5.9/§7.5.10, which is why one machine serves both
   (core/frame/document_lifecycle.h).
   IT IS ATTACHED TO EVERY LIVE TIMELINE, exactly as a routed delivery and a cross-agent operation are, and for
   a reason those two only hint at: a destruction is state a PAGE observes, and every piece of it — the
   `pagehide`/`unload` listeners, the map of active timers, the navigable's browsing context — belongs to the
   timeline that produced it. Nothing runs inside this call, and no flow is dropped, starved or paged: a flow
   suspended inside the replaced document keeps its snapshot and its place on the frontier.
   THE INCOMING DOCUMENT IS NOT TAKEN, because §7.5.9 "Unloading documents" does not spend it on the queue: step
   6 of unload-a-document-and-its-descendants queues the task on the OUTGOING document's own relevant global
   object, and the optional `newDocument`'s whole use is the document unload timing info this user agent does
   not carry (step 3 is the standard's answer for an absent one). Taking it made this entry unperformable for
   the navigation that needs it most — a cross-origin incoming Document is a PEER instance's, so its realm is
   not here to queue anything in, while the outgoing one is local by construction. */
void engine_unload_document(uint32_t doc);

/* WHO ASKED FOR THE REQUEST, AS A FACT ABOUT THE PARK AND NEVER AS A POLICY. HTML §4.12.1 "The script element"
 * gives every `script` element a PARSER-INSERTED flag and a parser document: a parser-inserted script is named
 * by the BYTES THE ZONE ITSELF FETCHED; every other park is made by RUNNING CODE.
 * IT IS NOT THE PROVENANCE AND IT NEVER WAS — it is ONE OF THE TWO FACTS the provenance is composed from, and
 * this comment used to call it "the whole of the difference the trusted zone has to be able to see" only
 * because the other fact did not exist: it said so in its own next sentence ("a distinction this register
 * cannot yet draw, because nothing on a flow records whether its path took a forced arm"). A flow records it
 * now (solver/flow.h's `path_forced`), the park composes the two at the moment it is made (pending.h's
 * PROV_*), and PENDING_PROVENANCE_* below is what a request IS. This field says only who asked.
 * THE TWO TOKENS ARE THE SAME LENGTH, and that is now a coincidence rather than a load-bearing property: the
 * join used to upgrade a duplicate's initiator by overwriting it in place and asserted the widths to make that
 * sound. It shifts the field instead (engine_pending_fetches' join_set_tokens), because the provenance beside
 * it has a VOCABULARY rather than a pair and choosing three tokens of equal length would be picking the words
 * to fit a memcpy. */
#define PENDING_INITIATOR_PARSER "parser"   /* HTML §4.12.1's parser-inserted script of the loaded document */
#define PENDING_INITIATOR_SCRIPT "script"   /* a park made by running code: fetch(), import(), an injected src */

/* WHAT THE REQUEST IS EVIDENCE OF — CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's three names, stated by the
 * engine and acted on by nobody here. The values and the composition are pending.h's (PROV_*), which is where
 * the park stamps them; these are their spelling on the wire.
 * THE ENGINE STATES AND THE ZONE DECIDES, which is the split CLAUDE.md names in as many words: "the engine
 * holds no network policy by construction, so `safeFetch` decides, from the provenance the request declares
 * beside its method and credential state". A branch here that refused to LIST a park would be that policy
 * inside the engine, and the flow it silenced would wait forever with nothing saying so.
 * A DEDUPED SET STATES THE MOST OBSERVED OF ITS MEMBERS, for the initiator's reason exactly and with the
 * argument spelled out at the join: the set is ONE request, so if any member's path stood on no contradicted
 * arm then a real client makes it and a reply to it is evidence about the app. */
#define PENDING_PROVENANCE_OBSERVED "observed" /* a real load of this document makes exactly this request */
#define PENDING_PROVENANCE_DERIVED  "derived"  /* the page's own code computed it from real inputs */
#define PENDING_PROVENANCE_FORCED   "forced"   /* a value in it exists only because a gate was forced */

/* THE SAME THREE WORDS FOR AN ACT THAT IS NOT A PARK — one composition, in one place, for every request this
 * engine builds by RUNNING THE PAGE'S CODE rather than by parking on a reply. A NAVIGATION is the caller this
 * was written for (core/frame/navigable.c's §7.4.5 "Populating a session history entry" load and its
 * `navigable.create`) and a ROUTE DECLARATION is the other (solver/route_seed.c); both used to be — or would
 * have been — a hand-written ternary over `flow_path_forced`, which is the second copy of a rule this file
 * already owns.
 *
 * IT ANSWERS `derived` OR `forced` AND NEVER `observed`, AND THAT IS A FACT ABOUT THE ACT RATHER THAN A
 * NARROWING OF THE VOCABULARY. `observed` is "a REAL LOAD of this document makes exactly this request", and
 * its first conjunct is HTML §4.12.1 "The script element"'s PARSER-INSERTED flag — a fact the PARK register
 * holds because a parser-inserted script's address came out of bytes the trusted zone itself fetched
 * (solver/pending.h's FLOW_PENDING_DOCSCRIPT). Nothing this door serves has that conjunct available: it is
 * asked where code RAN.
 *   RESIDUAL — CORRECT AND NARROWER, NAMED RATHER THAN CRASHED ON, because the code is right for what it does
 * and there is no case here to abort on. NOT COVERED: a child navigable whose `<iframe src>` came out of the
 * PARSER of bytes the trusted zone itself fetched. §4.12.1's argument reaches it exactly as it reaches a
 * parser-inserted `<script src>` — a real load of this document makes precisely that request — and it is
 * answered `derived` here, which under-claims. WHAT THE NEXT DIFF BUILDS: the parser-inserted conjunct as a
 * PARAMETER of this function, stated at the one site that knows it and threaded rather than inferred —
 * core/html/html_iframe.c's `iframe_document_parsed` is the parser's walk and is one of exactly TWO callers of
 * `iframe_create_navigable` (the other is core/dom/element.c's insertion steps, which is the script route), so
 * the bit travels `iframe_create_navigable` → `navigable_create` → `navigable_load_enqueue` beside the address
 * it belongs to. HOW ITS ABSENCE WOULD SHOW: the day any host treats the two words differently for a
 * NAVIGATION — a per-origin setting that fires derived navigations rather than only observed ones, or a report
 * that separates what a real load reaches from what only forced execution does — every parser-inserted child
 * navigable lands on the wrong side of it. Both hosts navigate `observed` and `derived` identically today,
 * which is also why building the distinction before it has a reader would be a computed writer with none.
 *
 * NO RUNNING FLOW IS A POSITIVE ANSWER AND NOT A DEFAULT. A path that does not exist has stood on no
 * contradicted arm, so the answer is `derived` — which is also the direction a provenance is allowed to be
 * wrong in: under-claiming costs one request a person may authorise per origin, while over-claiming carries a
 * reply to a request no client makes into the observed pool, which CLAUDE.md §@H forbids outright. A caller
 * for which a flow-less act would itself be a broken invariant asserts that at its own site (route_seed.c
 * does; §7.4.4's URL and history update steps are reached only by running the page's code).
 *
 * THE ENGINE STATES AND THE ZONE DECIDES. This names what a request IS and refuses nothing — the firing
 * decision, the credential decision and the per-origin widening are all `extension/lib/safe-fetch.js`'s and
 * its callers', by construction. */
const char *engine_provenance_of_running_path(void);

/* …AND THE SAME ANSWER AS A NUMBER, for the one consumer that STORES a grade instead of writing it onto a
 * wire: solver/endpoint.c keys an @H record by it and compares two of them, which a string cannot do without
 * that file learning the vocabulary. It is the composition and the function above is `engine_provenance_token`
 * of it — one rule, two spellings, in that order, so a caller cannot reach a fourth answer.
 * EVERYTHING THE PARAGRAPH ABOVE SAYS APPLIES UNCHANGED, the flow-less answer included. */
int engine_prov_of_running_path(void);

/* THE PROVENANCE'S WIRE SPELLING, AND IT IS EXPORTED BECAUSE TWO SURFACES PRINT IT. The pending line states
 * what a request IS and the @H record states what a LEARNED ENDPOINT is, and a trusted zone reads both about
 * the same app — so the three words have to be one vocabulary rather than two files' agreement. A second
 * mapping in solver/endpoint.c would be free to drift, and the direction it would drift in is the one that
 * costs: a record spelled `derived` by one file and `observed` by the other is read as the stronger of the
 * two by whichever consumer sees it. Fatal, never a DCHECK, for the reason the pending line's own spelling is
 * (a release build falling through would print whatever the compiler left in the register). */
const char *engine_provenance_token(int prov);

/* WHAT THE BYTES ARE FOR, WHICH IS A DIFFERENT QUESTION FROM WHO ASKED — Fetch §2.2.5 "Requests"' DESTINATION,
 * stated verbatim off the request record the park carried (core/fetch/fetch.h) and never derived here.
 * THE TWO FIELDS ARE NOT TWO SPELLINGS OF ONE FACT, and reading them as one is what left a live hole. The
 * INITIATOR is HTML §4.12.1.1 "Processing model"'s parser-inserted flag and says whether a REAL LOAD of this
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
 * `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>URL` line per outstanding request,
 * newline-terminated, "" for none, DEDUPED BY THE PAIR.
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
   newline); each TAB is overwritten with a NUL and the five fields are handed back pointing into it.
   THE DESTINATION, THE INITIATOR AND THE PROVENANCE ARE OUT-PARAMETERS AND NONE IS OPTIONAL, deliberately: a
   host that did not want one could pass NULL and would then be a host reading a request whose LOAD CLASS — or
   whose PROVENANCE — it never asked about, which is the defaulted-field defect wearing a convenience. Two of
   them are sharp in different ways: a host that skips the load class fetches a script as data, which is the
   state that field was added to end; a host that skips the provenance fires a request no client makes and
   carries its reply as an observation, which is what CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE forbids in as
   many words. It costs a caller three locals and three membership asserts. */
void engine_pending_split(char *line, const char **method, const char **destination,
                          const char **initiator, const char **provenance, const char **url);
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
} EngineStepUnitRuns;
void engine_step_unit_runs(EngineStepUnitRuns *out);

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
     * ran to its end is coverage this document actually gained, while a candidate session that ran to its end
     * is one derived payload that did NOT fire and was discarded, which is the search spending itself.
     * Summed, "the engine retired 47 flows" and "the search discarded 47 candidates having proved nothing"
     * are one number, and the reader cannot tell which it is holding.
     *
     * THE LABEL IS `Flow.cand_src` AND IT IS A BINARY PARTITION BY CONSTRUCTION. One field, set at birth
     * (solve.c's seed, engine_sibling_assemble's copy, cold.c's 'c' record), never cleared, freed only at
     * flow_release — so every member is on exactly one side of it at every instant of its life, and the two
     * arms below cannot overlap or leave a member out. That is why this is TWO rows and not three: being a
     * DRIVEN ORPHAN (`Flow.orphan`) is a separate field, and a drive seeded from a candidate parent inherits
     * the substitution AND gets the mark, so a candidate/orphan/plain split would not be a partition at all
     * and its "sum" would over-count the members that are both.
     *
     * THE TOTAL STAYS, AND THE PARTITION IS ASSERTED AGAINST IT — the same discipline solver/cold.h's
     * `step_units` keeps against `flows`. Each arm is incremented beside its total at the one site that
     * total is written at, so the identity is what a retirement path added later without a label breaks;
     * engine_frontier_census is where all three are read together and is where it fires. */
    long finished;          /* flows that ran to their end — `finished_flows + finished_cands`, asserted */
    long finished_flows;    /* …the EXPLORATION flows among them: coverage this document gained */
    long finished_cands;    /* …and the @S candidate sessions: derived payloads that ran and did not fire */
    long sold;              /* flows this instance PAGED OUT — `sold_flows + sold_cands`, asserted; see
                               g_flows_sold */
    long sold_flows;        /* …the exploration flows among them */
    long sold_cands;        /* …and the candidate sessions, which is the sharper half of the pair: a parked
                               candidate comes back WITHOUT its ladder (solver/flow.h — `cand_surv` and
                               `cand_rung` are readings of a re-execution and deliberately do not cross the
                               tier), so paging one costs the search the distance it had measured. */
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
} EngineFrontierCensus;
void engine_frontier_census(EngineFrontierCensus *out);

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
   missing and which a host had no choice but to guess at. */
void engine_routed_census(long *delivered, long *refused);

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
 *                                     CONDITION IS, NAMED so the row is checkable: flow_step asks the seed at
 *                                     the last moment BEFORE the clock may move, so "ran out of its own work"
 *                                     is "has no program, job, delivery, checkpoint or lifecycle stage due at
 *                                     the current moment" and NOT "has no frame, timer or reply left" — the
 *                                     second is the exit that declares a timeline OVER, it sits below a
 *                                     rendering opportunity that is generated for ever on a document that has
 *                                     one, and asking there made this row a fact about the DOCUMENT'S shape
 *                                     rather than about the frontier. A run in which it reads 0 while `live`
 *                                     climbs is now a statement about the five conditions above the rung —
 *                                     frame, sequence, job, block, lifecycle — and, AHEAD OF ALL FIVE, about
 *                                     whether the members were DISPATCHED at all: every one of those is asked
 *                                     inside flow_step, so a member the pick never reaches asks nothing and
 *                                     appears in none of them. Read solver/cold.h's `stepUnits` `none` row
 *                                     (solver/step_unit.h's NONE, paired with @WFQ's `unrun`) FIRST, then
 *                                     `framed`, `outOfPrograms`
 *                                     and `blocked` for which arm holds the rest. Reasoning over the five
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

/* ---- THE FOUR ENDS OF §9.3.3 STEP 8'S TASK, AND WHY ONE NUMBER COULD NOT SAY WHICH ---------------------
 *
 * `engine_routed_census`'s `delivered` counts tasks QUEUED. Nothing counted what became of them, so a host
 * looking at a page that ran its `message` listener fewer times than the engine delivered had exactly one
 * number for THREE different facts, each taking a different action: the task ran and the page saw the message;
 * the task ran and HTML §9.3.3 "Posting messages" step 8.1 declined it (the target's origin is not the one the
 * sender asked for); the task ran, or was taken off the queue before it could, and there was no Document left
 * to fire at (HTML §7.5.10 "Destroying documents" step 7 — reachable both ways, because engine.c's flow_deliver
 * enqueues a ROUTED delivery in the RECEIVING document's realm, which is exactly the realm step 7's removal
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
    ROUTED_TASK_TARGET_GONE,      /* §7.5.10 step 7: the target's Document was destroyed */
    ROUTED_TASK_THREW,            /* the task itself went abrupt before it could fire anything */
    ROUTED_TASK_END_N
};
/* REPORTED AT THE LINE THAT IS THAT END, and TARGET_GONE has two such lines because §7.5.10 step 7 is reachable
 * at two moments and is ONE fact either way. core/frame/window_message.c reports it when the task RUNS and
 * finds the navigable destroyed; solver/flow.c's flow_job_drop_realm reports it when step 7's own removal walk
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
