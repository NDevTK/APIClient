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
 *         run-to-completion, so it cannot reach the instance while ccall("qjs_step") is on the stack; a
 *         periodic export called BETWEEN steps is by definition not mid-call and answers a different question.
 *         The only agent that could raise the request from outside the flow's instruction stream is a second
 *         thread writing the request byte in SHARED linear memory, which needs -pthread/-sSHARED_MEMORY (so a
 *         SharedArrayBuffer, so cross-origin isolation on the offscreen document) AND the address of the main
 *         thread's own thread-local copy of that byte. That is the transport requirement, named; until it is
 *         provisioned the extension's raise sources are the interpreter's own (back-edge, call, fork), and this
 *         host's slice is bounded by the wall clock read at whichever of those the flow next reaches.
 *     quantum_measure() answers with that, in one string, so no message anywhere restates it and goes stale.
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
   step that returns on it are the same event seen from two levels. */
int quantum_expired(void);
/* This thread's consumed CPU in microseconds — the currency the WFQ's aging term is denominated in, and the
   same one the slice is. Where the host has no CPU clock this is its wall clock and quantum_measure() says so. */
int64_t quantum_thread_us(void);
/* WHAT THE VERDICT ABOVE IS ACTUALLY MADE OF, as a string a diagnostic prints instead of claiming. */
const char *quantum_measure(void);
/* …and the same fact as a PREDICATE, for the one thing a string cannot be used for: gating an ASSERTION on
   whether quantum_thread_us() is real CPU. §Testing forbids a verdict a loaded machine can falsify, so a check
   that aborts on consumed time may only run where consumed time is what is being read. Asked, never assumed. */
int quantum_measure_is_cpu(void);

#endif
