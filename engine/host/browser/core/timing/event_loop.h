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
 * mechanism §8.12 Animation frames's map of animation frame callbacks and §9.4.4 Message ports's port
 * message queue use, for the same reason CLAUDE.md gives: platform data a flow queues is a JS value, never
 * malloc'd C. It is built at agent init, which is pre-boot, so it is BASELINE and every flow's writes to it
 * are captured rather than shared.
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

#include <stdint.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT, before any page script runs — the record must be in the pre-boot baseline. */
void event_loop_init(JSContext *ctx);
/* Undone ONCE PER AGENT, from core/platform.c's release column — which takes the RUNTIME, because the record
   this gives back is the agent's and not any realm's. */
void event_loop_free(JSRuntime *rt);

/* THE CLOCK HAS TWO MOVERS AND FOR A LONG TIME IT HAD ONE, WHICH IS WHY EVERY DURATION THIS ENGINE MEASURED
 * WITHIN ONE TASK WAS EXACTLY ZERO.
 *
 * A TASK SOURCE BECOMING DUE moves it absolutely (event_loop_advance_to, below): the loop jumps to the moment
 * the next thing it selected is due at. Those are the only two callers this file ever had — a timer expiry and
 * a rendering frame — and BOTH of them run BETWEEN tasks. So within one task nothing moved the clock at all,
 * and for the agent's first realm, whose HIGH RESOLUTION TIME Level 3 §4 Time Origin is stamped from this
 * clock at creation, that number was 0 and stayed 0.
 *
 * HTML §8.1.7.3 Processing model SAYS OTHERWISE IN ITS OWN STEPS. Its continual list reads the unsafe shared
 * current time TWICE around one task — step 2.2 "Set taskStartTime to the unsafe shared current time", then,
 * after step 2.6 "Perform oldestTask's steps" and step 2.8's microtask checkpoint have both run, step 3 "Let
 * taskEndTime be the unsafe shared current time" — and step 4 hands both to "Report long tasks". A pair of
 * reads bracketing one task, differenced, is the standard stating that TIME PASSES WHILE THE TASK'S STEPS RUN.
 * On a clock only a task SELECTION moves, taskEndTime is taskStartTime, every long task is 0ms long, every
 * `performance.now()` delta inside a handler is 0, and a page that waits for time to pass inside one task —
 * a busy-wait spinner, a frame-budget guard, a benchmark loop — has no exit condition it can ever satisfy.
 * Nothing is capped and no bound is violated when that happens: the loop is preemptible bytecode and the flow
 * parks and resumes for ever. It simply never emits again, which is worse than a crash, because it looks like
 * exploration.
 *
 * SO THE SECOND MOVER IS THE WORK THE RUNNING FLOW PERFORMED, AND IT IS NOT A WALL CLOCK. That is a design
 * constraint of this whole project rather than a simplification, and it is the same one core/timing/hr_time.h
 * gives for declining the jitter HIGH RESOLUTION TIME Level 3 §3 Tools for Specification Authors permits (its
 * `coarsen time` step 3, "In an implementation-defined manner, coarsen and POTENTIALLY JITTER timestamp such
 * that its resolution will not exceed time resolution"): §Testing's solver differential is the only oracle
 * the SOLVER's semantics have, and its entire content is that ONE build must agree with ITSELF about one
 * document across several schedules. A real clock is not a function of the flow's path — it is a function of
 * the machine, the load average and which sibling held the thread — so every timestamp under it becomes a
 * disagreement the gate reports as a scheduling bug, and a flow parked to the cold tier on Monday resumes
 * into a clock that moved without it. §Time-travel-resume's razor calls exactly that a CAP: a resume that is
 * not byte-identical. Work-derived, the clock is a pure function of the flow's own path, so a park is invisible to it.
 *
 * HIGH RESOLUTION TIME Level 3 §2.1 Clocks IS WHAT MAKES THAT CONFORMING RATHER THAN A LIBERTY TAKEN. It
 * requires of the monotonic clock exactly two things: its unsafe current time "never decreases", and it "only
 * exists within a single execution of the user agent, so it can't be used to compare events that might happen
 * in different executions". Both hold here — advance_to's monotonicity invariant is the first, and a virtual
 * clock is the second by construction. What §2.1 says about matching real-world time is deliberately NOT a
 * requirement: "All clocks on the web platform ATTEMPT to count 1 millisecond of clock time per 1 millisecond
 * of real-world time, but they differ in how they handle cases where they can't be exactly correct." An engine
 * with no wall clock to attempt it against is such a case, and §2.2 Moments and Durations says the rest — a
 * moment is "a point in time" that "can't be directly stored as numbers", and a duration is a distance between
 * two of them on THE SAME clock. Nothing a page can read compares this clock to another one.
 *
 * THE UNIT OF WORK IS OPCODES RETIRED BY THE RUNNING FLOW, and the two nearby quantities that are NOT it are
 * worth naming, because each was reached for and each breaks the razor in a different place.
 *   — solver/engine.h's `engine_work_done` is AGENT-GLOBAL: forks taken, flows created, jobs run, context
 *     switches. A flow's clock would move because a SIBLING forked, so the same flow on two schedules reads
 *     two clocks and a resume observes moments its own path never produced.
 *   — the count of times the interpreter's yield POLL was reached is schedule-dependent for a subtler reason:
 *     a poll happens when the request byte is raised, the quantum's CPU-time edge raises it asynchronously,
 *     and "last raise wins" means an asynchronous raise landing on an already-raised byte adds no poll while
 *     one landing on a clear byte adds one. The number of polls is therefore a fact about the machine.
 * Opcodes retired is neither: it is what the flow DID, in the order its own bytecode says, and it is identical
 * on every schedule and across a park.
 *
 * THE CLOCK IS THEREFORE STORED AS TWO FIELDS AND NOT ONE — A BASE AND A RETIRED-WORK COUNT — WHICH IS AN
 * ANSWER TO FLOATING-POINT ORDER AND NOT AN OPTIMISATION. Folding each batch of retired work into the moment
 * as it arrives would make the moment depend on HOW THE BATCHES WERE SPLIT, because floating-point addition is
 * not associative; and how they are split is precisely the schedule-dependent poll count above. Held as an
 * exact integer count and divided once at the read, the moment is a pure function of the TOTAL, so a run that
 * polled twice and a run that polled two hundred times over the same opcodes answer the same number. That is
 * the razor discharged by construction rather than by an invariant somebody has to remember.
 *
 * BOTH FIELDS RIDE THE PER-FLOW COW DELTA, like every other field of this record and for the reason the note
 * at the top of this file gives. Two siblings that each retire work each write their own count into their own
 * delta over the shared baseline, so neither moves the other; a sibling that is DISCARDED has its delta
 * released and the baseline it was layered over is untouched, so its time never happened. There is nothing to
 * reconcile and nothing to merge, which is the whole reason the clock is a heap record rather than a static.
 *
 * THE RATE IS A CALIBRATION AND NOT A GRANULARITY, so §NO BOUNDS' rule about naming a policy input does not
 * reach it: a granularity decides how often a flow OFFERS to rest and a bound decides that work will not
 * happen, while this decides only how fast MODELLED time runs against modelled work — no flow runs less far
 * under any value of it. It is fixed here rather than made tunable for the same reason the coarsening declines
 * the jitter HIGH RESOLUTION TIME Level 3 §3 Tools for Specification Authors' `coarsen time` permits: a clock
 * that is a function of a SETTING is a clock two configurations of one build disagree about, and the
 * differential's whole content is that they must not. */
#define EVENT_LOOP_WORK_PER_MS 100000.0

/* The VIRTUAL clock, in ms since the agent started — the one clock every task source is ordered by, and the
   one an Event's `timeStamp` and a file's modification time are stamped from. A moment: a number, or unknown
   external input. It is the BASE plus the work retired since the base was set, which is why it is computed
   rather than read; with no work retired it is the base value itself and not a sum equal to it, because
   §8.1.7.1's last render opportunity time is asserted IDENTICAL to it and a derivation of an unknown moment is
   not identical to that moment. OWNED. */
JSValue event_loop_now(JSContext *ctx);

/* THE SECOND MOVER — `units` of work the RUNNING FLOW has just retired, in opcodes. See the note above for why
   the unit is that and not one of the two quantities beside it, and why the count is accumulated exactly
   rather than folded into the moment.
   IT IS NOT event_loop_advance_to AND MUST NOT BECOME IT. advance_to's second invariant is that the loop may
   not step OVER a task source that is already due, which is a statement about SELECTING a task; time passing
   while a task's steps run steps over a due timer all the time, and that is what a browser does — HTML
   §8.1.7.3's continual list runs one task's steps to completion (step 2.6) before it ever selects another, so
   a timer whose expiry passes mid-task is due at the next selection and not before it. Applying advance_to's
   invariant here would assert against the spec. */
void event_loop_work_advance(JSContext *ctx, uint64_t units);
/* RESIDUAL — NARROWER THAN HIGH RESOLUTION TIME Level 3 §2.1 Clocks, AND NAMED SO THE NEXT DIFF IS THE THING
 * AND NOT THE SEARCH FOR IT.
 *   WHAT IS NOT COVERED: nothing counts retired opcodes yet, so this operation has no caller and the clock
 *     still moves only between tasks. The count is not something that can be read from where the clock lives:
 *     the interpreter's per-opcode attention check counts NOTHING today — `DISPATCH` loads the thread-local
 *     yield-request byte and branches, and `do_yield_poll`'s own comment says "Nothing counts and nothing is
 *     bounded here" — and no per-flow retired-work quantity is exported by quickjs.h.
 *   WHAT THE NEXT DIFF BUILDS: a retired-opcode counter incremented at `DISPATCH` beside the `sf->cur_pc`
 *     store it already makes, thread-local for the same reason the yield request is, taken to zero when the
 *     scheduler switches a flow IN (so the count handed over is always the running flow's own), and handed to
 *     this operation from `do_yield_poll` BEFORE it asks the preempt policy — so that a flow which parks there
 *     has already banked the work it did to reach that point. Batching is free of consequence by the
 *     accumulate-exactly rule above, which is what lets the hand-over sit at the poll rather than at every
 *     opcode.
 *   HOW ITS ABSENCE SHOWS: every `Event.timeStamp` read within one task is the same number and every delta
 *     between two of them is exactly 0; `performance.now()` differences inside one handler are 0; the agent's
 *     first realm's time origin is 0 until a timer or a frame fires; and
 *     `dom/events/Event-timestamp-safe-resolution.html`'s `do { … } while (delta == 0)` has no exit at any
 *     resolution. When the counter lands, all four move together. */

/* MOVE THE CLOCK to the moment a task source becomes due. `due` is the earliest moment ANOTHER source is
   already due at, or JS_UNDEFINED when none is — the caller has it, because it is what decided this move.
   Both invariants are asserted here rather than trusted to the caller: time may not run backwards, and the
   loop may not step OVER a source that becomes due first. Both are read from what the caller's own ask
   decided (see the header note); neither is re-evaluated, because over an unknown that is a fork.
   IT IS AN ABSOLUTE MOVE, so it also RETIRES the work counted since the last one: `when` is a moment on this
   clock that the caller has already established is at or after the moment the loop stands at, and the work
   that got the loop there is subsumed by it rather than added to it. */
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
