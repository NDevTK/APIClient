/* THE EVENT LOOP'S OWN STATE — HTML §8.1.7, and it TIME-TRAVELS.
 *
 * WHY THIS IS A COMPONENT AND NOT TWO STATICS. An event loop has state of its own that no task source owns: in
 * this engine the VIRTUAL CLOCK (there is no wall clock to wait on, so the clock is the moment the loop has
 * advanced to), §8.1.7.1's LAST RENDER OPPORTUNITY TIME, and the INSERTION ORDER a task source breaks its own
 * ties by. Each of those lived as a `static double` — the clock in core/timing/timer.c, the render opportunity
 * in core/rendering/rendering.c — and each said in its comment that it belonged to the event loop rather than
 * to the file it was in. A fact stated in two files is answered in two files.
 *
 * IT IS PER-FLOW, and that is the whole reason it moved into the heap. A code flow is a complete timeline of
 * this event loop: it runs its own tasks, in its own order, and parks and resumes byte-identically. Virtual
 * time is DERIVED from that order — it is a function of which tasks this flow has run — so a clock shared with
 * the flow next door is a clock another timeline moved. Concretely, with one static: flow A fires a
 * `setTimeout(f, 10000)` and the agent's clock jumps to 10000; flow B, whose own tasks have reached moment 0,
 * then sets `setTimeout(g, 0)` and gets an expiry of 10000, and a flow parked before either resumes into a
 * clock it never advanced. §Time-travel-resume's razor calls that a CAP: a resume that is not byte-identical.
 *
 * SO THE RECORD IS A HEAP OBJECT, whose property writes the per-flow COW delta already captures — the same
 * mechanism §8.12 Animation frames's map of animation frame callbacks and §9.4.2's port message queue use, for the same reason
 * CLAUDE.md gives: platform data a flow queues is a JS value, never malloc'd C. It is built at agent init,
 * which is pre-boot, so it is BASELINE and every flow's writes to it are captured rather than shared.
 *
 * IT IS PER-AGENT AND NOT PER-REALM, which is the other half of the answer. §8.1.7 gives one event loop to a
 * similar-origin window agent, and a document and its same-origin iframe are ordered by that ONE loop: a
 * per-realm clock would order the parent's timers against the child's by nothing at all. §8.7 Timers's map of active
 * timers is the opposite — HTML puts one on every global — so the two live in different places on purpose.
 *
 * A MOMENT IS A VALUE, NOT A `double`, AND THAT IS THE WHOLE OF THE CONCOLIC CLOCK.
 *
 * §8.7 Timers's `run steps after a timeout` step 3 sets the map of active timers to "startTime plus
 * milliseconds", and `milliseconds` reaches this engine from a page that may have written
 * `setTimeout(f, someUnknown)`. The sum is then unknown, the entry is ordered against every other by a FORK
 * (both orders are real programs), and firing it moves this clock to a moment nothing computed. A `double`
 * cannot hold that, so every attempt to fire such a timer aborted here — which meant the sibling arm that
 * KEEPS the unknown, the one arm the whole §8.7 step-4 fork exists to produce, could never run a single
 * callback. Every `setTimeout` over attacker- or config-derived input died at that line.
 *
 * So `eventLoopNow` and §8.1.7.1's last render opportunity time are VALUES: a number, or unknown external
 * input carrying its provenance, its domain and — when the code computed one — its concrete example. Nothing
 * is picked, clamped or read off an example: §Solver-half's rule is that a moment no arm decided is not a
 * number to guess but a question, and the question is asked below.
 *
 * WHICH MAKES ORDERING THIS COMPONENT'S JOB, AND IT IS THE ONE SPELLER OF IT. Two moments are compared in
 * three places — the timer source orders one expiry against another (§8.7 `run steps after a timeout`
 * step 4.2), the in-parallel rendering loop orders the next frame against the clock (§8.1.7.3 step 2.1's
 * choice of task queue), and this file's own invariants order a move against the moment already reached. If
 * each spelled its own relation, one flow could answer the SAME question two ways: decide `A < B` true under
 * §8.7's name and false under §8.1.7's, and stand in a world neither arm is in — the path constraint is keyed
 * by the identity of the value a branch tests, and two names are two keys. They are one relation on one clock,
 * so there is one spelling of it, here.
 *
 * AND THE MONOTONICITY ASSERT IS READ FROM THAT DECISION RATHER THAN RE-EVALUATED. `when >= now` is a
 * comparison, and over an unknown a comparison is not a fact to test but an arm to take — asking it inside an
 * assertion would FORK, minting a sibling flow whose only content is a violated invariant. The proof already
 * exists: the caller reached the move by ASKING the ordering (event_loop_before), so the arm it took is in
 * this flow's own constraint, and the assert READS it (event_loop_before_decided, which never forks). It fires
 * on a DECIDED CONTRADICTION and stays silent on uncertainty, which is the one direction §Solver-half allows
 * anywhere else in this engine: a contradicted branch is pruned, an undecided one keeps both arms. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_EVENT_LOOP_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_EVENT_LOOP_H

#include "quickjs.h"

/* Declared ONCE PER AGENT, before any page script runs — the record must be in the pre-boot baseline. */
void event_loop_init(JSContext *ctx);
/* Undone ONCE PER AGENT, from core/platform.c's release column — which takes the RUNTIME, because the record
   this gives back is the agent's and not any realm's. */
void event_loop_free(JSRuntime *rt);

/* The VIRTUAL clock, in ms since the agent started — the one clock every task source is ordered by, and the
   one an Event's `timeStamp` and a file's modification time are stamped from. A moment: a number, or unknown
   external input. OWNED. */
JSValue event_loop_now(JSContext *ctx);

/* MOVE THE CLOCK to the moment a task source becomes due. `due` is the earliest moment ANOTHER source is
   already due at, or JS_UNDEFINED when none is — the caller has it, because it is what decided this move.
   Both invariants are asserted here rather than trusted to the caller: time may not run backwards, and the
   loop may not step OVER a source that becomes due first. Both are read from what the caller's own ask
   decided (see the header note); neither is re-evaluated, because over an unknown that is a fork. */
void event_loop_advance_to(JSContext *ctx, JSValueConst when, JSValueConst due);

/* §8.1.7.1's LAST RENDER OPPORTUNITY TIME — the moment the rendering task source last became due. OWNED. */
JSValue event_loop_last_render(JSContext *ctx);
/* The setter asserts the moment IS the one the clock now stands at, which is stronger than the `when <= now`
   that stood here and — unlike it — is decidable over an unknown. It is also what §8.1.7.3 Processing model
   says in its own words: the in-parallel loop's step 2 is "Set eventLoop's last render opportunity time to
   the unsafe shared current time", and the unsafe shared current time in this engine IS this clock. So
   equality is the truth and `<=` was admitting a state no caller can produce; an inequality over two unknowns
   can only be answered by forking, and an invariant that forks is an invariant that manufactures the world in
   which it holds. */
void event_loop_set_last_render(JSContext *ctx, JSValueConst when);

/* IS MOMENT `a` BEFORE MOMENT `b` ON THIS CLOCK? — the one comparison of two moments this engine has, and a
 * FORK wherever either is unknown: both orders are real programs (the page's other timer runs second in one
 * of them), and picking one deletes the other.
 *
 * ASKED AS A RESTARTABLE BRANCH, which every caller of it can promise and most sites cannot. §8.1.7.3
 * "Processing model" step 2.1 chooses the next task queue "in an implementation-defined manner", so this walk
 * runs BETWEEN a flow's tasks: the flow is switched in and nothing of it is on any stack, so the sibling is
 * assembled with no frame and re-entering its scheduler step re-runs the same walk and replays the arm
 * recorded for it. A caller that cannot make that promise must not call this.
 *
 * REFLEXIVITY IS DECIDED AND NOT ASKED: a moment is not before ITSELF, whatever it is, so an ask over two
 * copies of one moment answers 0 without minting a predicate. Without that, `advance_to(now)` — which the
 * rendering loop performs whenever the clock has already moved past the frame it was about to take — would
 * ask a question with one feasible answer and park a sibling exploring a world that cannot happen. */
int event_loop_before(JSContext *ctx, JSValueConst a, JSValueConst b);

/* THE SAME QUESTION, READ RATHER THAN ASKED — 1 decided `a < b`, 0 decided otherwise, -1 not decided by this
   flow. It NEVER forks and never records, which is what makes it usable inside an invariant. -1 is the honest
   answer for a pair this flow never ordered, and an assert built on it fires only on a contradiction. */
int event_loop_before_decided(JSContext *ctx, JSValueConst a, JSValueConst b);

/* DO TWO MOMENTS COINCIDE? — the second half of the timer source's key, and a fork over an unknown for the
   same reason `before` is. §8.7 `run steps after a timeout` step 4.2 orders by `milliseconds` and the event
   loop's INSERTION ORDER is what breaks an exact tie, so the two questions are distinct and are asked
   separately: folding them into one `<=` would put "equal, and set first" into whichever arm the fold picked,
   and that is the case the tie-break exists for. */
int event_loop_coincident(JSContext *ctx, JSValueConst a, JSValueConst b);

/* A MOMENT PLUS A DURATION — ECMAScript §13.15.3's `+`, run by the ENGINE over operands either of which may be
   unknown. §8.7 step 3's "startTime plus milliseconds" and §8.1.7.3's next rendering opportunity are the same
   arithmetic on the same clock, so there is one of it. Where an operand is unknown the result carries that
   operand's provenance AND — when it has one — the real arithmetic run on its example; composing a number
   instead would be the invention §Solver forbids, and carrying a derived label instead of running the operator
   would be the recorded transform-expression §Re-execution forbids. OWNED. */
JSValue event_loop_moment_plus(JSContext *ctx, JSValueConst moment, JSValueConst delta);

/* THE INSERTION ORDER OF A TASK, allocated by the loop rather than by a source, because it is what orders two
   sources' tasks that become due at the SAME moment — and because a per-source counter cannot: §8.7 Timers's map of
   active timers is per-global, so two same-origin documents each hand out handle 1 and a tie between them
   would be decided by nothing. `_peek` answers the next number without allocating it, which is what makes "an
   entry this flow can see was queued on this flow's own timeline" an assertable statement. */
double event_loop_task_seq(JSContext *ctx);
double event_loop_task_seq_peek(JSContext *ctx);

/* HTML §8.7 Timers's TIMER NESTING LEVEL OF THE CURRENTLY RUNNING TASK — the timer initialization steps'
 * step 3, "If the surrounding agent's event loop's currently running task is a task that was created by this
 * algorithm, then let nestingLevel be the task's timer nesting level. Otherwise, let nestingLevel be 0."
 *
 * IT IS THE EVENT LOOP'S AND NOT §8.7 Timers's MAP'S, and step 3's own sentence is why: the question names the
 * SURROUNDING AGENT's event loop, never the global whose `setTimeout` was called. An iframe's timer callback
 * calling `parent.setTimeout(f, 0)` IS nested — one invocation of this algorithm from inside another's task,
 * which step 3's note says is what the level counts ("it represents nested invocations of this algorithm, not
 * of a particular method") — and a per-realm home would answer it out of the parent's record, which is a
 * different question with a different answer. §8.1.7 Event loops states the carrier in the same words: "Each
 * event loop has a currently running task, which is either a task or null." That is one fact per agent, and
 * this record is where this engine keeps the agent's event-loop facts; §8.7 Timers's map of active timers is
 * per-global for the opposite reason and lives in core/timing/timer.c.
 *
 * 0 IS A POSITIVE STATEMENT, NOT A DEFAULT FILLING A HOLE. Step 10 ("Increment nestingLevel by one") runs
 * before step 11 hangs it on the task, so EVERY task the timer initialization steps create carries a level of
 * at least ONE — which makes 0 exactly step 3's "Otherwise" and never a value a reader has to guess the
 * provenance of. A task queued by any other algorithm publishes 0 for the same reason it reads 0: it was not
 * created by this algorithm. §8.7's own `run steps after a timeout`, reached by this engine's internal
 * callers, is one of those — it is a different algorithm and its task is not this one's.
 *
 * §8.1.7.3 Processing model DECIDES THE BRACKET, AND IT IS NARROWER THAN "WHILE THE CALLBACK'S CODE RUNS":
 * step 2.5 is "Set the event loop's currently running task to oldestTask", 2.6 is "Perform oldestTask's
 * steps", 2.7 is "Set the event loop's currently running task back to null" and only THEN does 2.8 "Perform a
 * microtask checkpoint". So a promise reaction the handler queued runs with NO currently running task and its
 * own `setTimeout` is at level 0 — a `setTimeout(f,0)` chain routed through `await` is therefore NOT clamped,
 * which is a real difference a page can see and not a corner this engine may round off. It falls out
 * structurally here rather than being arranged for: a microtask is a separate job of the flow's queue, so it
 * is never inside the task's own bracket.
 *
 * IT TIME-TRAVELS like every other field of this record, which is what makes it answerable at all: two flows
 * are two timelines of one event loop, so "which task is running" has one answer PER FLOW and a C static
 * would hand flow B the level of a task only flow A ever ran. */
int event_loop_timer_nesting(JSContext *ctx);
/* §8.1.7.3 Processing model steps 2.5 and 2.7, performed by §8.7 Timers's own task and by nothing else — the
   task is the only thing that knows the level it was given, and the level is meaningless outside its steps. */
void event_loop_set_timer_nesting(JSContext *ctx, int level);

#endif
