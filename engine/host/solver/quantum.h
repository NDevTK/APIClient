/* quantum.h — THE COOPERATIVE QUANTUM'S EDGE: the asynchronous source that asks a running flow for the thread
 * after it has consumed a slice, and the ONE place that says what THIS host can actually measure.
 *
 * §scheduler names two orthogonal yields at the one per-opcode suspend point. The VALUE yield has a source: a
 * rank change raises the engine's yield request (flow.c's frontier_rank_changed -> JS_RequestFlowYield). The
 * COOPERATIVE-QUANTUM yield had none — it was a CLAUSE inside preempt_hook, and preempt_hook runs only when
 * something has already raised a request. The three things that raised one were all shapes of the PAGE'S OWN
 * BYTECODE (a loop back-edge, a call, a concolic fork), so a stretch of straight-line call-free code asked the
 * scheduler nothing for as long as that stretch ran, and the slice it was supposed to be bounded by could not
 * expire. A budget whose expiry is only noticed when the debtor volunteers is not a budget.
 *
 * So the edge is ASYNCHRONOUS BY CONSTRUCTION: something outside the flow's own instruction stream raises the
 * request, and the flow answers at its very next opcode. That is the whole of this component.
 *
 * WHAT IT IS MEASURED IN IS NOT A DETAIL. §Testing: "MEASURE THE THING THE INVARIANT IS ABOUT — CPU actually
 * consumed, or work actually performed, never elapsed time". The quantum is about THREAD-SHARING: how much of
 * the thread one flow may hold before the host gets a turn to pump its port, interleave another document's
 * engine, stream findings and snapshot. A thread that has been DESCHEDULED is holding nothing — and the host
 * that would use the returned thread is descheduled with it, since it is the same thread — so wall time that
 * passes while the flow is not running is time nobody was denied. CPU actually consumed is the quantity.
 *
 * AND THE TWO HOSTS DIFFER IN WHAT THEY CAN MEASURE, WHICH IS WHY THE ANSWER IS A FUNCTION AND NOT A CONSTANT:
 *
 *   NATIVE (test_forced.c's smoke, and anything else that drives engine_sched_step on Linux)
 *     timer_create(CLOCK_THREAD_CPUTIME_ID, SIGEV_THREAD_ID) armed one-shot for one quantum of THIS THREAD's
 *     CPU. The kernel delivers the signal to the flow's own thread; the handler raises the yield request; the
 *     interpreter answers it at the next opcode. The verdict is thread CPU, exactly.
 *
 *   EMSCRIPTEN (the extension's one WASM instance per document)
 *     There is NO CPU CLOCK and NO ASYNCHRONOUS EDGE, and both halves are facts about the transport rather than
 *     gaps in this file:
 *       - emscripten's WASI clock_time_get answers CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID and
 *         CLOCK_THREAD_CPUTIME_ID from one call to emscripten_get_now() (performance.now()), i.e. wall time;
 *         only CLOCK_REALTIME differs. quantum_begin CHECKS that rather than assuming it (see quantum.c), so
 *         the day a real CPU clock appears the assert names the switch to make instead of the wall clock
 *         quietly staying.
 *       - a single-threaded WASM instance cannot be interrupted mid-call by anything. The host's JS is
 *         run-to-completion, so it cannot reach the instance while the step call is on the stack; a periodic
 *         export called BETWEEN steps is by definition not mid-call and answers a different question. The only
 *         agent that could raise the request from outside the flow's instruction stream is a second thread
 *         storing the request byte in the engine's linear memory, which must then be SHARED: -pthread /
 *         -sSHARED_MEMORY, plus the address of the main thread's own thread-local copy of that byte.
 *         WHAT BLOCKS THAT IS NOT THE FLAG, AND THIS PARAGRAPH USED TO PRICE IT AGAINST THE WRONG DOCUMENT.
 *         It said "so a SharedArrayBuffer, so cross-origin isolation on the offscreen document". The engine has
 *         not run in the offscreen document since every instance became a RENDERER: renderer-host.js forks one
 *         `<iframe sandbox="allow-scripts" src="renderer.html">` per agent cluster, and renderer.html is in
 *         manifest.sandbox.pages, whose CSP `sandbox` directive gives that document an OPAQUE origin — the
 *         security boundary itself, not an incidental. MEASURED IN THAT REALM ON REAL CHROME: a shared
 *         WebAssembly.Memory CONSTRUCTS and grows there, and `new Worker` succeeds — what fails is handing the
 *         memory across, with `DataCloneError: SharedArrayBuffer transfer requires self.crossOriginIsolated`.
 *         So the gate is the AGENT CLUSTER's cross-origin isolation, and an opaque origin is same-origin with
 *         nothing, so it is never isolated: `crossOriginIsolated === false` there under the manifest's COOP at
 *         `same-origin-allow-popups` AND at `same-origin`, with the frame's sandbox attribute as shipped,
 *         widened with allow-same-origin, and removed entirely. The identical watchdog runs END TO END in the
 *         offscreen document at BOTH COOP values (worker created, memory transferred, Atomics.wait returned,
 *         its store read back on the main thread), which is exactly the point: the flip provisions the
 *         transport where the engine is not, and it is not free — measured under `same-origin` a live verify's
 *         `window.open` still navigated and the sink's proof relay still fired, but the openee's
 *         `window.opener` was null (HTML 7.1.3 "Cross-origin opener policies": under "same-origin" an
 *         auxiliary browsing context "will appear closed to the opener"), which is the handle a postMessage
 *         delivery arm would need. THE REQUIREMENT IS THEREFORE `self.crossOriginIsolated` IN THE ENGINE'S OWN
 *         REALM, which this project cannot buy without giving up the boundary that realm exists to be.
 *         renderer.html asserts on that exact gate, so the day it opens the crash names the watchdog to build;
 *         quantum.c #errors if this branch is ever linked WITH shared memory.
 *         Until then the extension's raise sources are the interpreter's own (back-edge, call, fork) — the
 *         yield poll is at every dispatch, so the SUSPEND POINT is universal and only the RAISE is not — and
 *         this host's slice is bounded by the wall clock read at whichever of those the flow next reaches.
 *     quantum_measure() answers with that, in one string, so no message anywhere restates it and goes stale.
 *
 * AND THE ANSWER IS SAID OUT LOUD, ONCE PER INSTANCE, AT THE FIRST SLICE — the `@QUANTUM {…}` line quantum.c
 * writes. Both readers of quantum_measure() were inside SEAM-ASSERTION MESSAGES, so
 * this fact reached a person only on the runs that aborted and never on the runs a person compares; and what
 * it changes is not the slice but the ORDER, because engine.c bills the WFQ's aging charge in this same
 * currency and that charge is a comparison BETWEEN flows, so a descheduling the OS chose moves one flow's rank
 * and re-picks. Two runs of one artifact over one document then take different frontier orders with nothing
 * about the tree differing. That is the host and is not to be silenced — §scheduler's razor forbids both cures
 * (drop the quantum and it is a drive-to-completion; bound the slice in steps and it is a cap) — so the whole
 * of the fix is that a run STATES which denomination produced its numbers. The line is UNCONDITIONAL rather
 * than dev-only because a consumer's contract is checked against it: engine/build.mjs THROWS when a run
 * printed the frontier census and not this line, and a writer compiled out from under its reader is
 * §Architecture's field contract broken from the producing side.
 *
 * AND THE LINE IS ONE OF TWO EMISSIONS OF ONE COMPOSER, NEVER THE ONLY ONE — quantum_json() below is the fact,
 * and the printf is a host whose output happens to be a stream of lines. A LINE IS NOT A SURFACE THE SHIPPED
 * PATH HAS: the extension's renderer drains this engine's stdout into every reply and its trusted zone reads a
 * DOCUMENT, so a denomination that existed only as a line was written on the one host and unread on the other,
 * which is §Testing's "measure what the shipped path writes" with the producer in the right and the surface in
 * the wrong. It rides solver/result.c's result_json as `_quantum` for exactly the reason `_cold`, `_heap`,
 * `_swap` and `_wfq` do, and it is NOT a fifth census: those are readings of an INSTANT whose value a schedule
 * chooses, and this is a constant property of the HOST and the build that no schedule and no instant can move.
 * That difference is why extension/bridge.js asserts it as three NAMED, TYPED fields rather than folding it
 * into the "every row is a finite number" loop the four censuses share, and why engine/solvergate.mjs COMPARES
 * it across schedules instead of dropping it.
 *
 * The slice is a FLOOR ON SHARING, never a cap: nothing is dropped, starved, skipped, reordered or forgotten
 * across it. The flow parks as a COW snapshot and the SAME flow resumes on the byte-identical frontier unless
 * the WFQ says otherwise. */
#ifndef ENGINE_HOST_SOLVER_QUANTUM_H
#define ENGINE_HOST_SOLVER_QUANTUM_H

#include <stdint.h>

/* Open a slice: arm the edge for one ENGINE_QUANTUM_MS of this thread's CPU and clear the expiry. Exactly one
   slice is open at a time — the scheduler brackets each engine_sched_step with these two. */
void quantum_begin(void);
/* Close it: disarm, so a fire cannot land on the host's own time between two steps. */
void quantum_end(void);
/* Has this slice's budget been consumed? Read by preempt_hook (park the running flow) and by the scheduler
   loop (return the thread to the host). ONE budget, one edge, two consumers — a flow that parks on it and a
   step that returns on it are the same event seen from two levels.
   ONLY INSIDE AN OPEN SLICE, and that is ASSERTED rather than assumed, because a host that drives flows without
   the scheduler's bracket already exists: wpt_runner.c runs JS_FlowNew/JS_FlowResume under its own preempt
   policy with no frontier to be fair between, and it declines this edge deliberately. Outside a slice the two
   hosts answer OPPOSITE lies — the clock host measures against a start time never set (always expired, so every
   flow parks at its first opcode) and the timer host has nothing armed (never expired, so the flow keeps the
   thread) — and neither would say why. */
int quantum_expired(void);
/* IS A SLICE OPEN RIGHT NOW — the scheduler's bracket as a FACT this component owns, asked rather than each
   host keeping its own copy of it. It exists for exactly two ASSERTIONS, and they are the two halves of one
   invariant that had only ever been checked from one side:
     - the engine may only be EXECUTING inside a slice. solver/engine.c's preempt_hook asserts it at the ONE
       point the scheduling policy is consulted, which is where a flow is by definition running.
     - the shipped ABI may never RETURN to the host holding one. main.c's qjs_step asserts it, and that is what
       makes every OTHER entry in that ABI (provide, route, perform, answer, result) host-time by construction
       rather than by inspection.
   NEVER CONTROL FLOW. A caller that BRANCHES on this is choosing between a scheduled path and an unscheduled
   one, which is the fallback §C-stack bans — the whole point of the pair above is that there is one path and
   the other case crashes. */
int quantum_slice_open(void);
/* This thread's consumed CPU in microseconds — the currency the WFQ's aging term is denominated in, and the
   same one the slice is. Where the host has no CPU clock this is its wall clock and quantum_measure() says so. */
int64_t quantum_thread_us(void);
/* WHAT THE VERDICT ABOVE IS ACTUALLY MADE OF, as a string a diagnostic prints instead of claiming. */
const char *quantum_measure(void);
/* …and the same fact as a PREDICATE, for the one thing a string cannot be used for: gating an ASSERTION on
   whether quantum_thread_us() is real CPU. §Testing forbids a verdict a loaded machine can falsify, so a check
   that aborts on consumed time may only run where consumed time is what is being read. Asked, never assumed.

   WHAT A TRUE ANSWER MEANS, AND — THE HALF EVERY READER OF IT GOT WRONG — WHAT IT DOES NOT. Two renderers
   branch on this to tell a person whether one run's numbers may be compared with another's (engine/build.mjs's
   quantumText, extension/popup.js's `denom`), and BOTH used to render the true arm as "real thread CPU, so this
   run's census series is invariant to what else this box was doing". That is FALSE, and false in the direction
   that costs: it licenses reading a SINGLE run as a measurement, which is §Testing's artifact-of-HOW reported
   as a fact about WHAT ran — the defect this whole component exists to end, arriving in the sentence the
   component supplies to end it.
     TRUE BUYS AN ATTRIBUTION. solver/engine.c bills `flow_age_running(quantum_thread_us() - t0)`, so where this
     answers 1 every microsecond charged is one the flow HELD THE THREAD for, and no flow is demoted for time
     the OS spent elsewhere. That is the whole of the difference from a wall-clocked host, and it is a
     statement about WHOSE BILL a charge lands on.
     IT DOES NOT BUY A REPRODUCIBLE ORDER, and cannot, because the SLICE is denominated in this same quantity
     while the WORK a flow completes inside one microsecond of it is not a constant of the program: a contended
     machine costs one opcode sequence more thread CPU (a stall cycle is on-CPU time), so the timer fires at a
     different opcode and the suspend point moves. It reaches the order a second way with no timer in it at
     all — solver/flow.h's `flow_silence_notch` is a FLOOR over whole quanta of this quantity and is a term of
     `flow_weight`, so the step at which a flow crosses a notch, and the pick that follows it, move with what a
     microsecond bought.
   SO THE TWO ARMS DIFFER IN THE SOURCE OF THE VARIANCE, NEVER IN WHETHER THERE IS ONE, and any instruction to
   compare two runs of one revision before reading a difference between two revisions is UNCONDITIONAL. §NO
   BOUNDS forbids the cures that would make it conditional — drop the quantum and it is a drive-to-completion,
   denominate the slice in steps or in work and it is a cap — so what a reader is owed is the truth about the
   instrument and never a number that has stopped moving. */
int quantum_measure_is_cpu(void);
/* THE THREE OF THEM AS ONE DOCUMENT — `{"measure":…,"isCpu":…,"sliceMs":…}`, malloc'd, caller frees; NULL only
   on allocation failure, which is the shape solver/result.c's other composers answer in and which result_json
   already folds into its one abort arm.

   ONE COMPOSER, TWO EMISSION SITES, AND THE BYTES ARE THE SAME BYTES — solver/result.h states that idiom for
   the three subsystem censuses and the argument is identical here, one level smaller. A host whose output IS a
   stream of lines has to print this when it is taken or it is not in the output at all (quantum.c's announce);
   the shipped path reads a DOCUMENT and gets it as result_json's `_quantum`. What the idiom forbids is the
   second HAND-SERIALIZATION — a `printf("cpu=%d slice=%dms measure=%s")` in this file beside a snprintf of the
   same three facts in result.c is two lists kept by hand, which is precisely how `svc_min` came to be computed
   on every census and printed by nobody. There is one format string and both readers parse it.

   IT LIVES HERE AND NOT IN result.c, WHICH IS THE ONLY PLACE IT COULD LIVE. result.c already includes this
   header; a composer in result.c would make the EDGE depend on the findings document to say what it measures,
   and this file's first paragraph is that it holds nothing but the edge — no policy, no ranking, no knowledge
   of a flow, and certainly no knowledge of what a finding is.

   ASKABLE AT ANY TIME, INSIDE A SLICE OR OUT, which quantum_expired() deliberately is not. Every field is a
   compile-time constant of this translation unit's branch — the two literals below and engine.h's
   ENGINE_QUANTUM_MS — so there is no slice state to read and nothing to assert about the caller. That is also
   what makes `_quantum` present on EVERY result document including one composed by a host that never opened a
   slice, where the `@QUANTUM` line is legitimately absent: the line reports what a stage DID, the field
   reports what this host IS. */
char *quantum_json(void);

#endif
